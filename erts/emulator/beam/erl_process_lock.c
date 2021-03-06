/*
 * %CopyrightBegin%
 *
 * Copyright Ericsson AB 2007-2010. All Rights Reserved.
 *
 * The contents of this file are subject to the Erlang Public License,
 * Version 1.1, (the "License"); you may not use this file except in
 * compliance with the License. You should have received a copy of the
 * Erlang Public License along with this software. If not, it can be
 * retrieved online at http://www.erlang.org/.
 *
 * Software distributed under the License is distributed on an "AS IS"
 * basis, WITHOUT WARRANTY OF ANY KIND, either express or implied. See
 * the License for the specific language governing rights and limitations
 * under the License.
 *
 * %CopyrightEnd%
 */


/*
 * Description:	Impementation of Erlang process locks.
 *
 * Author: 	Rickard Green
 */

/*
 * A short explanation of the process lock implementation:
 *     Each process has a lock bitfield and a number of lock wait
 *   queues.
 *     The bit field contains of a number of lock flags (L1, L2, ...)
 *   and a number of wait flags (W1, W2, ...). Each lock flag has a
 *   corresponding wait flag. The bit field isn't guarranteed to be
 *   larger than 32-bits which sets a maximum of 16 different locks
 *   per process. Currently, only 4 locks per process are used. The
 *   bit field is operated on by use of atomic operations (custom
 *   made bitwise atomic operations). When a lock is locked the
 *   corresponding lock bit is set. When a thread is waiting on a
 *   lock the wait flag for the lock is set.
 *     The process table is protected by pix (process index) locks
 *   which is spinlocks that protects a number of process indices in
 *   the process table. The pix locks also protects the lock queues
 *   and modifications of wait flags.
 *     When acquiring a process lock we first try to set the lock
 *   flag. If we are able to set the lock flag and the wait flag
 *   isn't set we are done. If the lock flag was already set we
 *   have to acquire the pix lock, set the wait flag, and put
 *   ourselves in the wait queue.
 *   Process locks will always be acquired in fifo order.
 *     When releasing a process lock we first unset all lock flags
 *   whose corresponding wait flag is clear (which will succeed).
 *   If wait flags were set for the locks being released, we acquire
 *   the pix lock, and transfer the lock to the first thread
 *   in the wait queue.
 *     Note that wait flags may be read without the pix lock, but
 *   it is important that wait flags only are modified when the pix
 *   lock is held.
 *     This implementation assumes that erts_smp_atomic_or_retold()
 *   provides necessary memorybarriers for a lock operation, and that
 *   erts_smp_atomic_and_retold() provides necessary memorybarriers
 *   for an unlock operation.
 */

#ifdef HAVE_CONFIG_H
#  include "config.h"
#endif

#include "erl_process.h"

const Process erts_proc_lock_busy;

#ifdef ERTS_SMP

#define ERTS_PROC_LOCK_SPIN_COUNT_MAX  2000
#define ERTS_PROC_LOCK_SPIN_COUNT_SCHED_INC 32
#define ERTS_PROC_LOCK_SPIN_COUNT_BASE 1000
#define ERTS_PROC_LOCK_AUX_SPIN_COUNT 50

#define ERTS_PROC_LOCK_SPIN_UNTIL_YIELD 25

#ifdef ERTS_PROC_LOCK_DEBUG
#define ERTS_PROC_LOCK_HARD_DEBUG
#endif

#ifdef ERTS_PROC_LOCK_HARD_DEBUG
static void check_queue(erts_proc_lock_t *lck);
#endif

#if SIZEOF_INT < 4
#error "The size of the 'uflgs' field of the erts_tse_t type is too small"
#endif

struct erts_proc_lock_queues_t_ {
    erts_proc_lock_queues_t *next;
    erts_tse_t *queue[ERTS_PROC_LOCK_MAX_BIT+1];
};

static erts_proc_lock_queues_t zeroqs = {0};

static erts_smp_spinlock_t qs_lock;
static erts_proc_lock_queues_t *queue_free_list;

#ifdef ERTS_ENABLE_LOCK_CHECK
static struct {
    Sint16 proc_lock_main;
    Sint16 proc_lock_link;
    Sint16 proc_lock_msgq;
    Sint16 proc_lock_status;
} lc_id;
#endif

erts_pix_lock_t erts_pix_locks[ERTS_NO_OF_PIX_LOCKS];

static int proc_lock_spin_count;
static int aux_thr_proc_lock_spin_count;

static void cleanup_tse(void);

void
erts_init_proc_lock(void)
{
    int i;
    int cpus;
    erts_smp_spinlock_init(&qs_lock, "proc_lck_qs_alloc");
    for (i = 0; i < ERTS_NO_OF_PIX_LOCKS; i++) {
#ifdef ERTS_ENABLE_LOCK_COUNT
	erts_smp_spinlock_init_x(&erts_pix_locks[i].u.spnlck,
				      "pix_lock", make_small(i));
#else
	erts_smp_spinlock_init(&erts_pix_locks[i].u.spnlck, "pix_lock");
#endif
    }
    queue_free_list = NULL;
    erts_thr_install_exit_handler(cleanup_tse);
#ifdef ERTS_ENABLE_LOCK_CHECK
    lc_id.proc_lock_main	= erts_lc_get_lock_order_id("proc_main");
    lc_id.proc_lock_link	= erts_lc_get_lock_order_id("proc_link");
    lc_id.proc_lock_msgq	= erts_lc_get_lock_order_id("proc_msgq");
    lc_id.proc_lock_status	= erts_lc_get_lock_order_id("proc_status");
#endif
    cpus = erts_get_cpu_configured(erts_cpuinfo);
    if (cpus > 1) {
	proc_lock_spin_count = ERTS_PROC_LOCK_SPIN_COUNT_BASE;
	proc_lock_spin_count += (ERTS_PROC_LOCK_SPIN_COUNT_SCHED_INC
				 * ((int) erts_no_schedulers));
	aux_thr_proc_lock_spin_count = ERTS_PROC_LOCK_AUX_SPIN_COUNT;
    }
    else if (cpus == 1) {
	proc_lock_spin_count = 0;
	aux_thr_proc_lock_spin_count = 0;
    }
    else { /* No of cpus unknown. Assume multi proc, but be conservative. */
	proc_lock_spin_count = ERTS_PROC_LOCK_SPIN_COUNT_BASE/2;
	aux_thr_proc_lock_spin_count = ERTS_PROC_LOCK_AUX_SPIN_COUNT/2;
    }
    if (proc_lock_spin_count > ERTS_PROC_LOCK_SPIN_COUNT_MAX)
	proc_lock_spin_count = ERTS_PROC_LOCK_SPIN_COUNT_MAX;
}

#ifdef ERTS_ENABLE_LOCK_CHECK
static void
check_unused_tse(erts_tse_t *wtr)
{
    int i;
    erts_proc_lock_queues_t *queues = wtr->udata;
    ERTS_LC_ASSERT(wtr->uflgs == 0);
    for (i = 0; i <= ERTS_PROC_LOCK_MAX_BIT; i++)
	ERTS_LC_ASSERT(!queues->queue[i]);
}
#define CHECK_UNUSED_TSE(W) check_unused_tse((W))
#else
#define CHECK_UNUSED_TSE(W)
#endif

static ERTS_INLINE erts_tse_t *
tse_fetch(erts_pix_lock_t *pix_lock)
{
    erts_tse_t *tse = erts_tse_fetch();
    if (!tse->udata) {
	erts_proc_lock_queues_t *qs;
#if ERTS_PROC_LOCK_SPINLOCK_IMPL && !ERTS_PROC_LOCK_ATOMIC_IMPL
	if (pix_lock)
	    erts_pix_unlock(pix_lock);
#endif
	erts_smp_spin_lock(&qs_lock);
	qs = queue_free_list;
	if (qs) {
	    queue_free_list = queue_free_list->next;
	    erts_smp_spin_unlock(&qs_lock);
	}
	else {
	    erts_smp_spin_unlock(&qs_lock);
	    qs = erts_alloc(ERTS_ALC_T_PROC_LCK_QS,
			    sizeof(erts_proc_lock_queues_t));
	    sys_memcpy((void *) qs,
		       (void *) &zeroqs,
		       sizeof(erts_proc_lock_queues_t));
	}
	tse->udata = qs;
#if ERTS_PROC_LOCK_SPINLOCK_IMPL && !ERTS_PROC_LOCK_ATOMIC_IMPL
	if (pix_lock)
	    erts_pix_lock(pix_lock);
#endif
    }
    tse->uflgs = 0;
    return tse;
}

static ERTS_INLINE void
tse_return(erts_tse_t *tse, int force_free_q)
{
    CHECK_UNUSED_TSE(tse);
    if (force_free_q || erts_tse_is_tmp(tse)) {
	erts_proc_lock_queues_t *qs = tse->udata;
	ASSERT(qs);
	erts_smp_spin_lock(&qs_lock);
	qs->next = queue_free_list;
	queue_free_list = qs;
	erts_smp_spin_unlock(&qs_lock);
	tse->udata = NULL;
    }
    erts_tse_return(tse);
}

void
erts_proc_lock_prepare_proc_lock_waiter(void)
{
    tse_return(tse_fetch(NULL), 0);
}


static void
cleanup_tse(void)
{
    erts_tse_t *tse = erts_tse_fetch();
    if (tse) {
	if (tse->udata)
	    tse_return(tse, 1);
	else
	    erts_tse_return(tse);
    }
}


/*
 * Waiters are queued in a circular double linked list;
 * where qs->queue[lock_ix] is the first waiter in queue, and
 * qs->queue[lock_ix]->prev is the last waiter in queue.
 */

static ERTS_INLINE void
enqueue_waiter(erts_proc_lock_queues_t *qs,
	       int ix,
	       erts_tse_t *wtr)
{
    if (!qs->queue[ix]) {
	qs->queue[ix] = wtr;
	wtr->next = wtr;
	wtr->prev = wtr;
    }
    else {
	ERTS_LC_ASSERT(qs->queue[ix]->next && qs->queue[ix]->prev);
	wtr->next = qs->queue[ix];
	wtr->prev = qs->queue[ix]->prev;
	wtr->prev->next = wtr;
	qs->queue[ix]->prev = wtr;
    }
}

static erts_tse_t *
dequeue_waiter(erts_proc_lock_queues_t *qs, int ix)
{
    erts_tse_t *wtr = qs->queue[ix];
    ERTS_LC_ASSERT(qs->queue[ix]);
    if (wtr->next == wtr) {
	ERTS_LC_ASSERT(qs->queue[ix]->prev == wtr);
	qs->queue[ix] = NULL;
    }
    else {
	ERTS_LC_ASSERT(wtr->next != wtr);
	ERTS_LC_ASSERT(wtr->prev != wtr);
	wtr->next->prev = wtr->prev;
	wtr->prev->next = wtr->next;
	qs->queue[ix] = wtr->next;
    }
    return wtr;
}

/*
 * Tries to aquire as many locks as possible in lock order,
 * and sets the wait flag on the first lock not possible to
 * aquire.
 *
 * Note: We need the pix lock during this operation. Wait
 *       flags are only allowed to be manipulated under pix
 *       lock.
 */
static ERTS_INLINE void
try_aquire(erts_proc_lock_t *lck, erts_tse_t *wtr)
{
    ErtsProcLocks got_locks = (ErtsProcLocks) 0;
    ErtsProcLocks locks = wtr->uflgs;
    int lock_no;

    ERTS_LC_ASSERT(lck->queues);
    ERTS_LC_ASSERT(got_locks != locks);

    for (lock_no = 0; lock_no <= ERTS_PROC_LOCK_MAX_BIT; lock_no++) {
	ErtsProcLocks lock = ((ErtsProcLocks) 1) << lock_no;
	if (locks & lock) {
	    ErtsProcLocks wflg, old_lflgs;
	    if (lck->queues->queue[lock_no]) {
		/* Others already waiting */
	    enqueue:
		ERTS_LC_ASSERT(ERTS_PROC_LOCK_FLGS_READ_(lck)
			       & (lock << ERTS_PROC_LOCK_WAITER_SHIFT));
		enqueue_waiter(lck->queues, lock_no, wtr);
		break;
	    }
	    wflg = lock << ERTS_PROC_LOCK_WAITER_SHIFT;
	    old_lflgs = ERTS_PROC_LOCK_FLGS_BOR_(lck, wflg | lock);
	    if (old_lflgs & lock) {
		/* Didn't get the lock */
		goto enqueue;
	    }
	    else {
		/* Got the lock */
		got_locks |= lock;
		ERTS_LC_ASSERT(!(old_lflgs & wflg));
		/* No one else can be waiting for the lock; remove wait flag */
		(void) ERTS_PROC_LOCK_FLGS_BAND_(lck, ~wflg);
		if (got_locks == locks)
		    break;
	    }
	}
    }

    wtr->uflgs &= ~got_locks;
}

/*
 * Transfer 'trnsfr_lcks' held by this executing thread to other
 * threads waiting for the locks. When a lock has been transferred
 * we also have to try to aquire as many lock as possible for the
 * other thread.
 */
static int
transfer_locks(Process *p,
	       ErtsProcLocks trnsfr_lcks,
	       erts_pix_lock_t *pix_lock,
	       int unlock)
{
    int transferred = 0;
    erts_tse_t *wake = NULL;
    erts_tse_t *wtr;
    ErtsProcLocks unset_waiter = 0;
    ErtsProcLocks tlocks = trnsfr_lcks;
    int lock_no;

    ERTS_LC_ASSERT(erts_lc_pix_lock_is_locked(pix_lock));

#ifdef ERTS_PROC_LOCK_HARD_DEBUG
    check_queue(&p->lock);
#endif

    for (lock_no = 0; tlocks && lock_no <= ERTS_PROC_LOCK_MAX_BIT; lock_no++) {
	ErtsProcLocks lock = ((ErtsProcLocks) 1) << lock_no;
	if (tlocks & lock) {
	    erts_proc_lock_queues_t *qs = p->lock.queues;
	    /* Transfer lock */
#ifdef ERTS_ENABLE_LOCK_CHECK
	    tlocks &= ~lock;
#endif
	    ERTS_LC_ASSERT(ERTS_PROC_LOCK_FLGS_READ_(&p->lock)
			   & (lock << ERTS_PROC_LOCK_WAITER_SHIFT));
	    transferred++;
	    wtr = dequeue_waiter(qs, lock_no);
	    ERTS_LC_ASSERT(wtr);
	    if (!qs->queue[lock_no])
		unset_waiter |= lock;
	    ERTS_LC_ASSERT(wtr->uflgs & lock);
	    wtr->uflgs &= ~lock;
	    if (wtr->uflgs)
		try_aquire(&p->lock, wtr);
	    if (!wtr->uflgs) {
		/*
		 * The other thread got all locks it needs;
		 * need to wake it up.
		 */
		wtr->next = wake;
		wake = wtr;
	    }
	}

    }

    if (unset_waiter) {
	unset_waiter <<= ERTS_PROC_LOCK_WAITER_SHIFT;
	(void) ERTS_PROC_LOCK_FLGS_BAND_(&p->lock, ~unset_waiter);
    }

#ifdef ERTS_PROC_LOCK_HARD_DEBUG
    check_queue(&p->lock);
#endif

    ERTS_LC_ASSERT(tlocks == 0); /* We should have transferred all of them */

    if (!wake) {
	if (unlock)
	    erts_pix_unlock(pix_lock);
    }
    else {
	erts_pix_unlock(pix_lock);
    
	do {
	    erts_tse_t *tmp = wake;
	    wake = wake->next;
	    erts_atomic_set(&tmp->uaflgs, 0);
	    erts_tse_set(tmp);
	} while (wake);

	if (!unlock)
	    erts_pix_lock(pix_lock);
    }
    return transferred;
}

/*
 * Determine which locks in 'need_locks' are not currently locked in
 * 'in_use', but do not return any locks "above" some lock we need,
 * so we do not attempt to grab locks out of order.
 *
 * For example, if we want to lock 10111, and 00100 was already locked, this
 * would return 00011, indicating we should not try for 10000 yet because
 * that would be a lock-ordering violation.
 */
static ERTS_INLINE ErtsProcLocks
in_order_locks(ErtsProcLocks in_use, ErtsProcLocks need_locks)
{
    /* All locks we want that are already locked by someone else. */
    ErtsProcLocks busy = in_use & need_locks;

    /* Just the lowest numbered lock we want that's in use; 0 if none. */
    ErtsProcLocks lowest_busy = busy & -busy;

    /* All locks below the lowest one we want that's in use already. */
    return need_locks & (lowest_busy - 1);
}

/*
 * Try to grab locks one at a time in lock order and wait on the lowest
 * lock we fail to grab, if any.
 *
 * If successful, this returns 0 and all locks in 'need_locks' are held.
 *
 * On entry, the pix lock is held iff !ERTS_PROC_LOCK_ATOMIC_IMPL.
 * On exit it is not held.
 */
static void
wait_for_locks(Process *p,
               erts_pix_lock_t *pixlck,
	       ErtsProcLocks locks,
               ErtsProcLocks need_locks,
               ErtsProcLocks olflgs)
{
    erts_pix_lock_t *pix_lock = pixlck ? pixlck : ERTS_PID2PIXLOCK(p->id);
    erts_tse_t *wtr;
    erts_proc_lock_queues_t *qs;

    /* Acquire a waiter object on which this thread can wait. */
    wtr = tse_fetch(pix_lock);
    
    /* Record which locks this waiter needs. */
    wtr->uflgs = need_locks;

    ASSERT((wtr->uflgs & ~ERTS_PROC_LOCKS_ALL) == 0);

#if ERTS_PROC_LOCK_ATOMIC_IMPL
    erts_pix_lock(pix_lock);
#endif

    ERTS_LC_ASSERT(erts_lc_pix_lock_is_locked(pix_lock));

    qs = wtr->udata;
    ASSERT(qs);
    /* Provide the process with waiter queues, if it doesn't have one. */
    if (!p->lock.queues) {
	qs->next = NULL;
	p->lock.queues = qs;
    }
    else {
	qs->next = p->lock.queues->next;
	p->lock.queues->next = qs;
    }

#ifdef ERTS_PROC_LOCK_HARD_DEBUG
    check_queue(&p->lock);
#endif

    /* Try to aquire locks one at a time in lock order and set wait flag */
    try_aquire(&p->lock, wtr);

    ASSERT((wtr->uflgs & ~ERTS_PROC_LOCKS_ALL) == 0);

#ifdef ERTS_PROC_LOCK_HARD_DEBUG
    check_queue(&p->lock);
#endif

    if (wtr->uflgs) {
	/* We didn't get them all; need to wait... */

	ASSERT((wtr->uflgs & ~ERTS_PROC_LOCKS_ALL) == 0);

	erts_atomic_set(&wtr->uaflgs, 1);
	erts_pix_unlock(pix_lock);

	while (1) {
	    int res;
	    erts_tse_reset(wtr);

	    if (erts_atomic_read(&wtr->uaflgs) == 0)
		break;

	    /*
	     * Wait for needed locks. When we are woken all needed locks have
	     * have been acquired by other threads and transfered to us.
	     * However, we need to be prepared for spurious wakeups.
	     */
	    do {
		res = erts_tse_wait(wtr); /* might return EINTR */
	    } while (res != 0);
	}

	erts_pix_lock(pix_lock);

	ASSERT(wtr->uflgs == 0);
    }

    /* Recover some queues to store in the waiter. */
    ERTS_LC_ASSERT(p->lock.queues);
    if (p->lock.queues->next) {
	qs = p->lock.queues->next;
	p->lock.queues->next = qs->next;
    }
    else {
	qs = p->lock.queues;
	p->lock.queues = NULL;
    }
    wtr->udata = qs;

    erts_pix_unlock(pix_lock);

    ERTS_LC_ASSERT(locks == (ERTS_PROC_LOCK_FLGS_READ_(&p->lock) & locks));

    tse_return(wtr, 0);
}

/*
 * erts_proc_lock_failed() is called when erts_smp_proc_lock()
 * wasn't able to lock all locks. We may need to transfer locks
 * to waiters and wait for our turn on locks.
 *
 * Iff !ERTS_PROC_LOCK_ATOMIC_IMPL, the pix lock is locked on entry.
 *
 * This always returns with the pix lock unlocked.
 */
void
erts_proc_lock_failed(Process *p,
		      erts_pix_lock_t *pixlck,
		      ErtsProcLocks locks,
		      ErtsProcLocks old_lflgs)
{
    int until_yield = ERTS_PROC_LOCK_SPIN_UNTIL_YIELD;
    int thr_spin_count;
    int spin_count;
    ErtsProcLocks need_locks = locks;
    ErtsProcLocks olflgs = old_lflgs;

    if (erts_thr_get_main_status())
	thr_spin_count = proc_lock_spin_count;
    else
	thr_spin_count = aux_thr_proc_lock_spin_count;

    spin_count = thr_spin_count;

    while (need_locks != 0) {
        ErtsProcLocks can_grab;

	can_grab = in_order_locks(olflgs, need_locks);

        if (can_grab == 0) {
            /* Someone already has the lowest-numbered lock we want. */

            if (spin_count-- <= 0) {
                /* Too many retries, give up and sleep for the lock. */
                wait_for_locks(p, pixlck, locks, need_locks, olflgs);
                return;
            }

	    ERTS_SPIN_BODY;

	    if (--until_yield == 0) {
		until_yield = ERTS_PROC_LOCK_SPIN_UNTIL_YIELD;
		erts_thr_yield();
	    }

            olflgs = ERTS_PROC_LOCK_FLGS_READ_(&p->lock);
        }
        else {
            /* Try to grab all of the grabbable locks at once with cmpxchg. */
            ErtsProcLocks grabbed = olflgs | can_grab;
            ErtsProcLocks nflgs =
                ERTS_PROC_LOCK_FLGS_CMPXCHG_ACQB_(&p->lock, grabbed, olflgs);

            if (nflgs == olflgs) {
                /* Success! We grabbed the 'can_grab' locks. */
                olflgs = grabbed;
                need_locks &= ~can_grab;

                /* Since we made progress, reset the spin count. */
                spin_count = thr_spin_count;
            }
            else {
                /* Compare-and-exchange failed, try again. */
                olflgs = nflgs;
            }
        }
    }

   /* Now we have all of the locks we wanted. */

#if !ERTS_PROC_LOCK_ATOMIC_IMPL
    erts_pix_unlock(pixlck);
#endif
}

/*
 * erts_proc_unlock_failed() is called when erts_smp_proc_unlock()
 * wasn't able to unlock all locks. We may need to transfer locks
 * to waiters.
 */
void
erts_proc_unlock_failed(Process *p,
			erts_pix_lock_t *pixlck,
			ErtsProcLocks wait_locks)
{
    erts_pix_lock_t *pix_lock = pixlck ? pixlck : ERTS_PID2PIXLOCK(p->id);

#if ERTS_PROC_LOCK_ATOMIC_IMPL
    erts_pix_lock(pix_lock);
#endif

    transfer_locks(p, wait_locks, pix_lock, 1); /* unlocks pix_lock */
}

/*
 * proc_safelock() locks process locks on two processes. In order
 * to avoid a deadlock, proc_safelock() unlocks those locks that
 * needs to be unlocked,  and then acquires locks in lock order
 * (including the previously unlocked ones).
 */

static void
proc_safelock(Process *a_proc,
	      erts_pix_lock_t *a_pix_lck,
	      ErtsProcLocks a_have_locks,
	      ErtsProcLocks a_need_locks,
	      Process *b_proc,
	      erts_pix_lock_t *b_pix_lck,
	      ErtsProcLocks b_have_locks,
	      ErtsProcLocks b_need_locks)
{
    Process *p1, *p2;
    Eterm pid1, pid2;
    erts_pix_lock_t *pix_lck1, *pix_lck2;
    ErtsProcLocks need_locks1, have_locks1, need_locks2, have_locks2;
    ErtsProcLocks unlock_mask;
    int lock_no, refc1 = 0, refc2 = 0;

    ERTS_LC_ASSERT(b_proc);


    /* Determine inter process lock order...
     * Locks with the same lock order should be locked on p1 before p2.
     */
    if (a_proc) {
	if (a_proc->id < b_proc->id) {
	    p1 = a_proc;
	    pid1 = a_proc->id;
	    pix_lck1 = a_pix_lck;
	    need_locks1 = a_need_locks;
	    have_locks1 = a_have_locks;
	    p2 = b_proc;
	    pid2 = b_proc->id;
	    pix_lck2 = b_pix_lck;
	    need_locks2 = b_need_locks;
	    have_locks2 = b_have_locks;
	}
	else if (a_proc->id > b_proc->id) {
	    p1 = b_proc;
	    pid1 = b_proc->id;
	    pix_lck1 = b_pix_lck;
	    need_locks1 = b_need_locks;
	    have_locks1 = b_have_locks;
	    p2 = a_proc;
	    pid2 = a_proc->id;
	    pix_lck2 = a_pix_lck;
	    need_locks2 = a_need_locks;
	    have_locks2 = a_have_locks;
	}
	else {
	    ERTS_LC_ASSERT(a_proc == b_proc);
	    ERTS_LC_ASSERT(a_proc->id == b_proc->id);
	    p1 = a_proc;
	    pid1 = a_proc->id;
	    pix_lck1 = a_pix_lck;
	    need_locks1 = a_need_locks | b_need_locks;
	    have_locks1 = a_have_locks | b_have_locks;
	    p2 = NULL;
	    pid2 = 0;
	    pix_lck2 = NULL;
	    need_locks2 = 0;
	    have_locks2 = 0;
	}
    }
    else {
	p1 = b_proc;
	pid1 = b_proc->id;
	pix_lck1 = b_pix_lck;
	need_locks1 = b_need_locks;
	have_locks1 = b_have_locks;
	p2 = NULL;
	pid2 = 0;
	pix_lck2 = NULL;
	need_locks2 = 0;
	have_locks2 = 0;
#ifdef ERTS_ENABLE_LOCK_CHECK
	a_need_locks = 0;
	a_have_locks = 0;
#endif
    }

#ifdef ERTS_ENABLE_LOCK_CHECK
    if (p1)
	erts_proc_lc_chk_proc_locks(p1, have_locks1);
    if (p2)
	erts_proc_lc_chk_proc_locks(p2, have_locks2);

    if ((need_locks1 & have_locks1) != have_locks1)
	erts_lc_fail("Thread tries to release process lock(s) "
		     "on %T via erts_proc_safelock().", pid1);
    if ((need_locks2 & have_locks2) != have_locks2)
	erts_lc_fail("Thread tries to release process lock(s) "
		     "on %T via erts_proc_safelock().",
		     pid2);
#endif


    need_locks1 &= ~have_locks1;
    need_locks2 &= ~have_locks2;

    /* Figure out the range of locks that needs to be unlocked... */
    unlock_mask = ERTS_PROC_LOCKS_ALL;
    for (lock_no = 0;
	 lock_no <= ERTS_PROC_LOCK_MAX_BIT;
	 lock_no++) {
	ErtsProcLocks lock = (1 << lock_no);
	if (lock & need_locks1)
	    break;
	unlock_mask &= ~lock;
	if (lock & need_locks2)
	    break;
    }

    /* ... and unlock locks in that range... */
    if (have_locks1 || have_locks2) {
	ErtsProcLocks unlock_locks;
	unlock_locks = unlock_mask & have_locks1;
	if (unlock_locks) {
	    have_locks1 &= ~unlock_locks;
	    need_locks1 |= unlock_locks;
	    if (!have_locks1) {
		refc1 = 1;
		erts_smp_proc_inc_refc(p1);
	    }
	    erts_smp_proc_unlock__(p1, pix_lck1, unlock_locks);
	}
	unlock_locks = unlock_mask & have_locks2;
	if (unlock_locks) {
	    have_locks2 &= ~unlock_locks;
	    need_locks2 |= unlock_locks;
	    if (!have_locks2) {
		refc2 = 1;
		erts_smp_proc_inc_refc(p2);
	    }
	    erts_smp_proc_unlock__(p2, pix_lck2, unlock_locks);
	}
    }

    /*
     * lock_no equals the number of the first lock to lock on
     * either p1 *or* p2.
     */


#ifdef ERTS_ENABLE_LOCK_CHECK
    if (p1)
	erts_proc_lc_chk_proc_locks(p1, have_locks1);
    if (p2)
	erts_proc_lc_chk_proc_locks(p2, have_locks2);
#endif

    /* Lock locks in lock order... */
    while (lock_no <= ERTS_PROC_LOCK_MAX_BIT) {
	ErtsProcLocks locks;
	ErtsProcLocks lock = (1 << lock_no);
	ErtsProcLocks lock_mask = 0;
	if (need_locks1 & lock) {
	    do {
		lock = (1 << lock_no++);
		lock_mask |= lock;
	    } while (lock_no <= ERTS_PROC_LOCK_MAX_BIT
		     && !(need_locks2 & lock));
	    if (need_locks2 & lock)
		lock_no--;
	    locks = need_locks1 & lock_mask;
	    erts_smp_proc_lock__(p1, pix_lck1, locks);
	    have_locks1 |= locks;
	    need_locks1 &= ~locks;
	}
	else if (need_locks2 & lock) {
	    while (lock_no <= ERTS_PROC_LOCK_MAX_BIT
		   && !(need_locks1 & lock)) {
		lock_mask |= lock;
		lock = (1 << ++lock_no);
	    }
	    locks = need_locks2 & lock_mask;
	    erts_smp_proc_lock__(p2, pix_lck2, locks);
	    have_locks2 |= locks;
	    need_locks2 &= ~locks;
	}
	else
	    lock_no++;
    }

#ifdef ERTS_ENABLE_LOCK_CHECK
    if (p1)
	erts_proc_lc_chk_proc_locks(p1, have_locks1);
    if (p2)
	erts_proc_lc_chk_proc_locks(p2, have_locks2);

    if (p1 && p2) {
	if (p1 == a_proc) {
	    ERTS_LC_ASSERT(a_need_locks == have_locks1);
	    ERTS_LC_ASSERT(b_need_locks == have_locks2);
	}
	else {
	    ERTS_LC_ASSERT(a_need_locks == have_locks2);
	    ERTS_LC_ASSERT(b_need_locks == have_locks1);
	}
    }
    else {
	ERTS_LC_ASSERT(p1);
	if (a_proc) {
	    ERTS_LC_ASSERT(have_locks1 == (a_need_locks | b_need_locks));
	}
	else {
	    ERTS_LC_ASSERT(have_locks1 == b_need_locks);
	}
    }
#endif

    if (refc1)
	erts_smp_proc_dec_refc(p1);
    if (refc2)
	erts_smp_proc_dec_refc(p2);
}

void
erts_proc_safelock(Process *a_proc,
		   ErtsProcLocks a_have_locks,
		   ErtsProcLocks a_need_locks,
		   Process *b_proc,
		   ErtsProcLocks b_have_locks,
		   ErtsProcLocks b_need_locks)
{
    proc_safelock(a_proc,
		  a_proc ? ERTS_PID2PIXLOCK(a_proc->id) : NULL,
		  a_have_locks,
		  a_need_locks,
		  b_proc,
		  b_proc ? ERTS_PID2PIXLOCK(b_proc->id) : NULL,
		  b_have_locks,
		  b_need_locks);
}

/*
 * erts_pid2proc_safelock() is called from erts_pid2proc_opt() when
 * it wasn't possible to trylock all locks needed. 
 *   c_p		- current process
 *   c_p_have_locks	- locks held on c_p
 *   pid                - process id of process we are looking up
 *   proc               - process struct of process we are looking
 *			  up (both in and out argument)
 *   need_locks         - all locks we need (including have_locks)
 *   pix_lock		- pix lock for process we are looking up
 *   flags		- option flags
 */
void
erts_pid2proc_safelock(Process *c_p,
		       ErtsProcLocks c_p_have_locks,
		       Process **proc,
		       ErtsProcLocks need_locks,
		       erts_pix_lock_t *pix_lock,
		       int flags)
{
    Process *p = *proc;
    ERTS_LC_ASSERT(p->lock.refc > 0);
    ERTS_LC_ASSERT(process_tab[internal_pid_index(p->id)] == p);
    p->lock.refc++;
    erts_pix_unlock(pix_lock);

    proc_safelock(c_p,
		  c_p ? ERTS_PID2PIXLOCK(c_p->id) : NULL,
		  c_p_have_locks,
		  c_p_have_locks,
		  p,
		  pix_lock,
		  0,
		  need_locks);

    erts_pix_lock(pix_lock);

    if (!p->is_exiting
	|| ((flags & ERTS_P2P_FLG_ALLOW_OTHER_X)
	    && process_tab[internal_pid_index(p->id)] == p)) {
	ERTS_LC_ASSERT(p->lock.refc > 1);
	p->lock.refc--;
    }
    else {
	/* No proc. Note, we need to keep refc until after process unlock */
	erts_pix_unlock(pix_lock);
	erts_smp_proc_unlock__(p, pix_lock, need_locks);
	*proc = NULL;
	erts_pix_lock(pix_lock);
	ERTS_LC_ASSERT(p->lock.refc > 0);
	if (--p->lock.refc == 0) {
	    erts_pix_unlock(pix_lock);
	    erts_free_proc(p);
	    erts_pix_lock(pix_lock);
	}
    }
}

void
erts_proc_lock_init(Process *p)
{
    /* We always start with all locks locked */
#if ERTS_PROC_LOCK_ATOMIC_IMPL
    erts_smp_atomic_init(&p->lock.flags, (long) ERTS_PROC_LOCKS_ALL);
#else
    p->lock.flags = ERTS_PROC_LOCKS_ALL;
#endif
    p->lock.queues = NULL;
    p->lock.refc = 1;
#ifdef ERTS_ENABLE_LOCK_COUNT
    erts_lcnt_proc_lock_init(p);
    erts_lcnt_proc_lock(&(p->lock), ERTS_PROC_LOCKS_ALL);
    erts_lcnt_proc_lock_post_x(&(p->lock), ERTS_PROC_LOCKS_ALL, __FILE__, __LINE__);
#endif
    
#ifdef ERTS_ENABLE_LOCK_CHECK
    erts_proc_lc_trylock(p, ERTS_PROC_LOCKS_ALL, 1);
#endif
#ifdef ERTS_PROC_LOCK_DEBUG
    {
	int i;
	for (i = 0; i <= ERTS_PROC_LOCK_MAX_BIT; i++)
	    erts_smp_atomic_init(&p->lock.locked[i], (long) 1);
    }
#endif
}

/* --- Process lock counting ----------------------------------------------- */

#ifdef ERTS_ENABLE_LOCK_COUNT
void erts_lcnt_proc_lock_init(Process *p) {
    
    if (p->id != ERTS_INVALID_PID) {
	erts_lcnt_init_lock_x(&(p->lock.lcnt_main),   "proc_main",   ERTS_LCNT_LT_PROCLOCK, p->id);
	erts_lcnt_init_lock_x(&(p->lock.lcnt_msgq),   "proc_msgq",   ERTS_LCNT_LT_PROCLOCK, p->id);
	erts_lcnt_init_lock_x(&(p->lock.lcnt_link),   "proc_link",   ERTS_LCNT_LT_PROCLOCK, p->id);
	erts_lcnt_init_lock_x(&(p->lock.lcnt_status), "proc_status", ERTS_LCNT_LT_PROCLOCK, p->id);
    } else {
	erts_lcnt_init_lock(&(p->lock.lcnt_main),   "proc_main",   ERTS_LCNT_LT_PROCLOCK);
	erts_lcnt_init_lock(&(p->lock.lcnt_msgq),   "proc_msgq",   ERTS_LCNT_LT_PROCLOCK);
	erts_lcnt_init_lock(&(p->lock.lcnt_link),   "proc_link",   ERTS_LCNT_LT_PROCLOCK);
	erts_lcnt_init_lock(&(p->lock.lcnt_status), "proc_status", ERTS_LCNT_LT_PROCLOCK);
    }
}
	

void erts_lcnt_proc_lock_destroy(Process *p) {
    erts_lcnt_destroy_lock(&(p->lock.lcnt_main));
    erts_lcnt_destroy_lock(&(p->lock.lcnt_msgq));
    erts_lcnt_destroy_lock(&(p->lock.lcnt_link));
    erts_lcnt_destroy_lock(&(p->lock.lcnt_status));
}

void erts_lcnt_proc_lock(erts_proc_lock_t *lock, ErtsProcLocks locks) {
    if (erts_lcnt_rt_options & ERTS_LCNT_OPT_PROCLOCK) { 
    if (locks & ERTS_PROC_LOCK_MAIN) {
	erts_lcnt_lock(&(lock->lcnt_main));
    }
    if (locks & ERTS_PROC_LOCK_MSGQ) {
        erts_lcnt_lock(&(lock->lcnt_msgq));
    }
    if (locks & ERTS_PROC_LOCK_LINK) {
	erts_lcnt_lock(&(lock->lcnt_link));
    }
    if (locks & ERTS_PROC_LOCK_STATUS) {
	erts_lcnt_lock(&(lock->lcnt_status));
    }
    }
}

void erts_lcnt_proc_lock_post_x(erts_proc_lock_t *lock, ErtsProcLocks locks, char *file, unsigned int line) {
    if (erts_lcnt_rt_options & ERTS_LCNT_OPT_PROCLOCK) { 
    if (locks & ERTS_PROC_LOCK_MAIN) {
	erts_lcnt_lock_post_x(&(lock->lcnt_main), file, line);
    }
    if (locks & ERTS_PROC_LOCK_MSGQ) {
        erts_lcnt_lock_post_x(&(lock->lcnt_msgq), file, line);
    }
    if (locks & ERTS_PROC_LOCK_LINK) {
	erts_lcnt_lock_post_x(&(lock->lcnt_link), file, line);
    }
    if (locks & ERTS_PROC_LOCK_STATUS) {
	erts_lcnt_lock_post_x(&(lock->lcnt_status), file, line);
    }
    }
}

void erts_lcnt_proc_lock_unaquire(erts_proc_lock_t *lock, ErtsProcLocks locks) {
    if (erts_lcnt_rt_options & ERTS_LCNT_OPT_PROCLOCK) { 
    if (locks & ERTS_PROC_LOCK_MAIN) {
	erts_lcnt_lock_unaquire(&(lock->lcnt_main));
    }
    if (locks & ERTS_PROC_LOCK_MSGQ) {
        erts_lcnt_lock_unaquire(&(lock->lcnt_msgq));
    }
    if (locks & ERTS_PROC_LOCK_LINK) {
	erts_lcnt_lock_unaquire(&(lock->lcnt_link));
    }
    if (locks & ERTS_PROC_LOCK_STATUS) {
	erts_lcnt_lock_unaquire(&(lock->lcnt_status));
    }
    }
}

void erts_lcnt_proc_unlock(erts_proc_lock_t *lock, ErtsProcLocks locks) {
    if (erts_lcnt_rt_options & ERTS_LCNT_OPT_PROCLOCK) { 
    if (locks & ERTS_PROC_LOCK_MAIN) {
	erts_lcnt_unlock(&(lock->lcnt_main));
    }
    if (locks & ERTS_PROC_LOCK_MSGQ) {
        erts_lcnt_unlock(&(lock->lcnt_msgq));
    }
    if (locks & ERTS_PROC_LOCK_LINK) {
	erts_lcnt_unlock(&(lock->lcnt_link));
    }
    if (locks & ERTS_PROC_LOCK_STATUS) {
	erts_lcnt_unlock(&(lock->lcnt_status));
    }
    }
}
void erts_lcnt_proc_trylock(erts_proc_lock_t *lock, ErtsProcLocks locks, int res) {
    if (erts_lcnt_rt_options & ERTS_LCNT_OPT_PROCLOCK) { 
    if (locks & ERTS_PROC_LOCK_MAIN) {
	erts_lcnt_trylock(&(lock->lcnt_main), res);
    }
    if (locks & ERTS_PROC_LOCK_MSGQ) {
        erts_lcnt_trylock(&(lock->lcnt_msgq), res);
    }
    if (locks & ERTS_PROC_LOCK_LINK) {
	erts_lcnt_trylock(&(lock->lcnt_link), res);
    }
    if (locks & ERTS_PROC_LOCK_STATUS) {
	erts_lcnt_trylock(&(lock->lcnt_status), res);
    }
    }
}

#endif /* ifdef ERTS_ENABLE_LOCK_COUNT */


/* --- Process lock checking ----------------------------------------------- */

#ifdef ERTS_ENABLE_LOCK_CHECK

void
erts_proc_lc_lock(Process *p, ErtsProcLocks locks)
{
    erts_lc_lock_t lck = ERTS_LC_LOCK_INIT(-1,
					   p->id,
					   ERTS_LC_FLG_LT_PROCLOCK);
    if (locks & ERTS_PROC_LOCK_MAIN) {
	lck.id = lc_id.proc_lock_main;
	erts_lc_lock(&lck);
    }
    if (locks & ERTS_PROC_LOCK_LINK) {
	lck.id = lc_id.proc_lock_link;
	erts_lc_lock(&lck);
    }
    if (locks & ERTS_PROC_LOCK_MSGQ) {
	lck.id = lc_id.proc_lock_msgq;
	erts_lc_lock(&lck);
    }
    if (locks & ERTS_PROC_LOCK_STATUS) {
	lck.id = lc_id.proc_lock_status;
	erts_lc_lock(&lck);
    }
}

void
erts_proc_lc_trylock(Process *p, ErtsProcLocks locks, int locked)
{
    erts_lc_lock_t lck = ERTS_LC_LOCK_INIT(-1,
					   p->id,
					   ERTS_LC_FLG_LT_PROCLOCK);
    if (locks & ERTS_PROC_LOCK_MAIN) {
	lck.id = lc_id.proc_lock_main;
	erts_lc_trylock(locked, &lck);
    }
    if (locks & ERTS_PROC_LOCK_LINK) {
	lck.id = lc_id.proc_lock_link;
	erts_lc_trylock(locked, &lck);
    }
    if (locks & ERTS_PROC_LOCK_MSGQ) {
	lck.id = lc_id.proc_lock_msgq;
	erts_lc_trylock(locked, &lck);
    }
    if (locks & ERTS_PROC_LOCK_STATUS) {
	lck.id = lc_id.proc_lock_status;
	erts_lc_trylock(locked, &lck);
    }
}

void
erts_proc_lc_unlock(Process *p, ErtsProcLocks locks)
{
    erts_lc_lock_t lck = ERTS_LC_LOCK_INIT(-1,
					   p->id,
					   ERTS_LC_FLG_LT_PROCLOCK);
    if (locks & ERTS_PROC_LOCK_STATUS) {
	lck.id = lc_id.proc_lock_status;
	erts_lc_unlock(&lck);
    }
    if (locks & ERTS_PROC_LOCK_MSGQ) {
	lck.id = lc_id.proc_lock_msgq;
	erts_lc_unlock(&lck);
    }
    if (locks & ERTS_PROC_LOCK_LINK) {
	lck.id = lc_id.proc_lock_link;
	erts_lc_unlock(&lck);
    }
    if (locks & ERTS_PROC_LOCK_MAIN) {
	lck.id = lc_id.proc_lock_main;
	erts_lc_unlock(&lck);
    }
}

void
erts_proc_lc_might_unlock(Process *p, ErtsProcLocks locks)
{
    erts_lc_lock_t lck = ERTS_LC_LOCK_INIT(-1,
					   p->id,
					   ERTS_LC_FLG_LT_PROCLOCK);
    if (locks & ERTS_PROC_LOCK_STATUS) {
	lck.id = lc_id.proc_lock_status;
	erts_lc_might_unlock(&lck);
    }
    if (locks & ERTS_PROC_LOCK_MSGQ) {
	lck.id = lc_id.proc_lock_msgq;
	erts_lc_might_unlock(&lck);
    }
    if (locks & ERTS_PROC_LOCK_LINK) {
	lck.id = lc_id.proc_lock_link;
	erts_lc_might_unlock(&lck);
    }
    if (locks & ERTS_PROC_LOCK_MAIN) {
	lck.id = lc_id.proc_lock_main;
	erts_lc_might_unlock(&lck);
    }
}

void
erts_proc_lc_require_lock(Process *p, ErtsProcLocks locks)
{
    erts_lc_lock_t lck = ERTS_LC_LOCK_INIT(-1,
					   p->id,
					   ERTS_LC_FLG_LT_PROCLOCK);
    if (locks & ERTS_PROC_LOCK_MAIN) {
	lck.id = lc_id.proc_lock_main;
	erts_lc_require_lock(&lck);
    }
    if (locks & ERTS_PROC_LOCK_LINK) {
	lck.id = lc_id.proc_lock_link;
	erts_lc_require_lock(&lck);
    }
    if (locks & ERTS_PROC_LOCK_MSGQ) {
	lck.id = lc_id.proc_lock_msgq;
	erts_lc_require_lock(&lck);
    }
    if (locks & ERTS_PROC_LOCK_STATUS) {
	lck.id = lc_id.proc_lock_status;
	erts_lc_require_lock(&lck);
    }
}

void
erts_proc_lc_unrequire_lock(Process *p, ErtsProcLocks locks)
{
    erts_lc_lock_t lck = ERTS_LC_LOCK_INIT(-1,
					   p->id,
					   ERTS_LC_FLG_LT_PROCLOCK);
    if (locks & ERTS_PROC_LOCK_STATUS) {
	lck.id = lc_id.proc_lock_status;
	erts_lc_unrequire_lock(&lck);
    }
    if (locks & ERTS_PROC_LOCK_MSGQ) {
	lck.id = lc_id.proc_lock_msgq;
	erts_lc_unrequire_lock(&lck);
    }
    if (locks & ERTS_PROC_LOCK_LINK) {
	lck.id = lc_id.proc_lock_link;
	erts_lc_unrequire_lock(&lck);
    }
    if (locks & ERTS_PROC_LOCK_MAIN) {
	lck.id = lc_id.proc_lock_main;
	erts_lc_unrequire_lock(&lck);
    }
}


int
erts_proc_lc_trylock_force_busy(Process *p, ErtsProcLocks locks)
{
    if (locks & ERTS_PROC_LOCKS_ALL) {
	erts_lc_lock_t lck = ERTS_LC_LOCK_INIT(-1,
					       p->id,
					       ERTS_LC_FLG_LT_PROCLOCK);

	if (locks & ERTS_PROC_LOCK_MAIN)
	    lck.id = lc_id.proc_lock_main;
	else if (locks & ERTS_PROC_LOCK_LINK)
	    lck.id = lc_id.proc_lock_link;
	else if (locks & ERTS_PROC_LOCK_MSGQ)
	    lck.id = lc_id.proc_lock_msgq;
	else if (locks & ERTS_PROC_LOCK_STATUS)
	    lck.id = lc_id.proc_lock_status;
	else
	    erts_lc_fail("Unknown proc lock found");

	return erts_lc_trylock_force_busy(&lck);
    }
    return 0;
}

void erts_proc_lc_chk_only_proc_main(Process *p)
{
    erts_lc_lock_t proc_main = ERTS_LC_LOCK_INIT(lc_id.proc_lock_main,
						 p->id,
						 ERTS_LC_FLG_LT_PROCLOCK);
    erts_lc_check_exact(&proc_main, 1);
}

#define ERTS_PROC_LC_EMPTY_LOCK_INIT \
  ERTS_LC_LOCK_INIT(-1, THE_NON_VALUE, ERTS_LC_FLG_LT_PROCLOCK)

void
erts_proc_lc_chk_have_proc_locks(Process *p, ErtsProcLocks locks)
{
    int have_locks_len = 0;
    erts_lc_lock_t have_locks[4] = {ERTS_PROC_LC_EMPTY_LOCK_INIT,
				    ERTS_PROC_LC_EMPTY_LOCK_INIT,
				    ERTS_PROC_LC_EMPTY_LOCK_INIT,
				    ERTS_PROC_LC_EMPTY_LOCK_INIT};
    if (locks & ERTS_PROC_LOCK_MAIN) {
	have_locks[have_locks_len].id = lc_id.proc_lock_main;
	have_locks[have_locks_len++].extra = p->id;
    }
    if (locks & ERTS_PROC_LOCK_LINK) {
	have_locks[have_locks_len].id = lc_id.proc_lock_link;
	have_locks[have_locks_len++].extra = p->id;
    }
    if (locks & ERTS_PROC_LOCK_MSGQ) {
	have_locks[have_locks_len].id = lc_id.proc_lock_msgq;
	have_locks[have_locks_len++].extra = p->id;
    }
    if (locks & ERTS_PROC_LOCK_STATUS) {
	have_locks[have_locks_len].id = lc_id.proc_lock_status;
	have_locks[have_locks_len++].extra = p->id;
    }

    erts_lc_check(have_locks, have_locks_len, NULL, 0);
}

void
erts_proc_lc_chk_proc_locks(Process *p, ErtsProcLocks locks)
{
    int have_locks_len = 0;
    int have_not_locks_len = 0;
    erts_lc_lock_t have_locks[4] = {ERTS_PROC_LC_EMPTY_LOCK_INIT,
				    ERTS_PROC_LC_EMPTY_LOCK_INIT,
				    ERTS_PROC_LC_EMPTY_LOCK_INIT,
				    ERTS_PROC_LC_EMPTY_LOCK_INIT};
    erts_lc_lock_t have_not_locks[4] = {ERTS_PROC_LC_EMPTY_LOCK_INIT,
					ERTS_PROC_LC_EMPTY_LOCK_INIT,
					ERTS_PROC_LC_EMPTY_LOCK_INIT,
					ERTS_PROC_LC_EMPTY_LOCK_INIT};

    if (locks & ERTS_PROC_LOCK_MAIN) {
	have_locks[have_locks_len].id = lc_id.proc_lock_main;
	have_locks[have_locks_len++].extra = p->id;
    }
    else {
	have_not_locks[have_not_locks_len].id = lc_id.proc_lock_main;
	have_not_locks[have_not_locks_len++].extra = p->id;
    }
    if (locks & ERTS_PROC_LOCK_LINK) {
	have_locks[have_locks_len].id = lc_id.proc_lock_link;
	have_locks[have_locks_len++].extra = p->id;
    }
    else {
	have_not_locks[have_not_locks_len].id = lc_id.proc_lock_link;
	have_not_locks[have_not_locks_len++].extra = p->id;
    }
    if (locks & ERTS_PROC_LOCK_MSGQ) {
	have_locks[have_locks_len].id = lc_id.proc_lock_msgq;
	have_locks[have_locks_len++].extra = p->id;
    }
    else {
	have_not_locks[have_not_locks_len].id = lc_id.proc_lock_msgq;
	have_not_locks[have_not_locks_len++].extra = p->id;
    }
    if (locks & ERTS_PROC_LOCK_STATUS) {
	have_locks[have_locks_len].id = lc_id.proc_lock_status;
	have_locks[have_locks_len++].extra = p->id;
    }
    else {
	have_not_locks[have_not_locks_len].id = lc_id.proc_lock_status;
	have_not_locks[have_not_locks_len++].extra = p->id;
    }

    erts_lc_check(have_locks, have_locks_len,
		  have_not_locks, have_not_locks_len);
}

ErtsProcLocks
erts_proc_lc_my_proc_locks(Process *p)
{
    int resv[4];
    erts_lc_lock_t locks[4] = {ERTS_LC_LOCK_INIT(lc_id.proc_lock_main,
						 p->id,
						 ERTS_LC_FLG_LT_PROCLOCK),
			       ERTS_LC_LOCK_INIT(lc_id.proc_lock_link,
						 p->id,
						 ERTS_LC_FLG_LT_PROCLOCK),
			       ERTS_LC_LOCK_INIT(lc_id.proc_lock_msgq,
						 p->id,
						 ERTS_LC_FLG_LT_PROCLOCK),
			       ERTS_LC_LOCK_INIT(lc_id.proc_lock_status,
						 p->id,
						 ERTS_LC_FLG_LT_PROCLOCK)};

    ErtsProcLocks res = 0;

    erts_lc_have_locks(resv, locks, 4);
    if (resv[0])
	res |= ERTS_PROC_LOCK_MAIN;
    if (resv[1])
	res |= ERTS_PROC_LOCK_LINK;
    if (resv[2])
	res |= ERTS_PROC_LOCK_MSGQ;
    if (resv[3])
	res |= ERTS_PROC_LOCK_STATUS;

    return res;
}

void
erts_proc_lc_chk_no_proc_locks(char *file, int line)
{
    int resv[4];
    int ids[4] = {lc_id.proc_lock_main,
		  lc_id.proc_lock_link,
		  lc_id.proc_lock_msgq,
		  lc_id.proc_lock_status};
    erts_lc_have_lock_ids(resv, ids, 4);
    if (resv[0] || resv[1] || resv[2] || resv[3]) {
	erts_lc_fail("%s:%d: Thread has process locks locked when expected "
		     "not to have any process locks locked",
		     file, line);
    }
}

#endif /* #ifdef ERTS_ENABLE_LOCK_CHECK */

#ifdef ERTS_PROC_LOCK_HARD_DEBUG
void
check_queue(erts_proc_lock_t *lck)
{
    int lock_no;
    ErtsProcLocks lflgs = ERTS_PROC_LOCK_FLGS_READ_(lck);

    for (lock_no = 0; lock_no <= ERTS_PROC_LOCK_MAX_BIT; lock_no++) {
	ErtsProcLocks wtr;
	wtr = (((ErtsProcLocks) 1) << lock_no) << ERTS_PROC_LOCK_WAITER_SHIFT;
	if (lflgs & wtr) {
	    int n;
	    erts_tse_t *wtr;
	    ERTS_LC_ASSERT(lck->queues && lck->queues->queue[lock_no]);
	    wtr = lck->queues->queue[lock_no];
	    n = 0;
	    do {
		wtr = wtr->next;
		n++;
	    } while (wtr != lck->queues->queue[lock_no]);
	    do {
		wtr = wtr->prev;
		n--;
	    } while (wtr != lck->queues->queue[lock_no]);
	    ERTS_LC_ASSERT(n == 0);
	}
	else {
	    ERTS_LC_ASSERT(!lck->queues || !lck->queues->queue[lock_no]);
	}
    }
}
#endif

#endif /* ERTS_SMP (the whole file) */

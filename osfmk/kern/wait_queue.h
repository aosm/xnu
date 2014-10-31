/*
 * Copyright (c) 2000-2005 Apple Computer, Inc. All rights reserved.
 *
 * @APPLE_OSREFERENCE_LICENSE_HEADER_START@
 * 
 * This file contains Original Code and/or Modifications of Original Code
 * as defined in and that are subject to the Apple Public Source License
 * Version 2.0 (the 'License'). You may not use this file except in
 * compliance with the License. The rights granted to you under the License
 * may not be used to create, or enable the creation or redistribution of,
 * unlawful or unlicensed copies of an Apple operating system, or to
 * circumvent, violate, or enable the circumvention or violation of, any
 * terms of an Apple operating system software license agreement.
 * 
 * Please obtain a copy of the License at
 * http://www.opensource.apple.com/apsl/ and read it before using this file.
 * 
 * The Original Code and all software distributed under the License are
 * distributed on an 'AS IS' basis, WITHOUT WARRANTY OF ANY KIND, EITHER
 * EXPRESS OR IMPLIED, AND APPLE HEREBY DISCLAIMS ALL SUCH WARRANTIES,
 * INCLUDING WITHOUT LIMITATION, ANY WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE, QUIET ENJOYMENT OR NON-INFRINGEMENT.
 * Please see the License for the specific language governing rights and
 * limitations under the License.
 * 
 * @APPLE_OSREFERENCE_LICENSE_HEADER_END@
 */

#ifdef	KERNEL_PRIVATE

#ifndef _KERN_WAIT_QUEUE_H_
#define _KERN_WAIT_QUEUE_H_

#include <mach/mach_types.h>
#include <mach/sync_policy.h>
#include <mach/kern_return.h>		/* for kern_return_t */

#include <kern/kern_types.h>		/* for wait_queue_t */
#include <kern/queue.h>
#include <kern/assert.h>

#include <sys/cdefs.h>

#ifdef	MACH_KERNEL_PRIVATE

#include <kern/simple_lock.h>
#include <mach/branch_predicates.h>

#include <machine/cpu_number.h>
#include <machine/machine_routines.h> /* machine_timeout_suspended() */

/*
 * The event mask is of 60 bits on 64 bit architeture and 28 bits on
 * 32 bit architecture and so we calculate its size using sizeof(long).
 * If the bitfield for wq_type and wq_fifo is changed, then value of 
 * EVENT_MASK_BITS will also change. 
 */
#define EVENT_MASK_BITS  ((sizeof(long) * 8) - 4)

/*
 * Zero out the 4 msb of the event.
 */
#define CAST_TO_EVENT_MASK(event)  (((CAST_DOWN(unsigned long, event)) << 4) >> 4)
/*
 *	wait_queue_t
 *	This is the definition of the common event wait queue
 *	that the scheduler APIs understand.  It is used
 *	internally by the gerneralized event waiting mechanism
 *	(assert_wait), and also for items that maintain their
 *	own wait queues (such as ports and semaphores).
 *
 *	It is not published to other kernel components.  They
 *	can create wait queues by calling wait_queue_alloc.
 *
 *	NOTE:  Hardware locks are used to protect event wait
 *	queues since interrupt code is free to post events to
 *	them.
 */
typedef struct wait_queue {
    unsigned long int                    /* flags */
    /* boolean_t */	wq_type:2,		/* only public field */
					wq_fifo:1,		/* fifo wakeup policy? */
					wq_prepost:1,	/* waitq supports prepost? set only */
					wq_eventmask:EVENT_MASK_BITS; 
    hw_lock_data_t	wq_interlock;	/* interlock */
    queue_head_t	wq_queue;		/* queue of elements */
} WaitQueue;

/*
 *	wait_queue_set_t
 *	This is the common definition for a set wait queue.
 *	These can be linked as members/elements of multiple regular
 *	wait queues.  They have an additional set of linkages to
 *	identify the linkage structures that point to them.
 */
typedef struct wait_queue_set {
	WaitQueue		wqs_wait_queue; /* our wait queue */
	queue_head_t	wqs_setlinks;	/* links from set perspective */
	queue_head_t	wqs_preposts;	/* preposted links */
} WaitQueueSet;

#define wqs_type		wqs_wait_queue.wq_type
#define wqs_fifo		wqs_wait_queue.wq_fifo
#define wqs_prepost	wqs_wait_queue.wq_prepost
#define wqs_queue		wqs_wait_queue.wq_queue

/*
 *	wait_queue_element_t
 *	This structure describes the elements on an event wait
 *	queue.  It is the common first fields in a thread shuttle
 *	and wait_queue_link_t.  In that way, a wait queue can
 *	consist of both thread shuttle elements and links off of
 *	to other (set) wait queues.
 *
 *	WARNING: These fields correspond to fields in the thread
 *	shuttle (run queue links and run queue pointer). Any change in
 *	the layout here will have to be matched with a change there.
 */
typedef struct wait_queue_element {
	queue_chain_t	wqe_links;	/* link of elements on this queue */
	void *			wqe_type;	/* Identifies link vs. thread */
	wait_queue_t	wqe_queue;	/* queue this element is on */
} WaitQueueElement;

typedef WaitQueueElement *wait_queue_element_t;

/*
 *	wait_queue_link_t
 *	Specialized wait queue element type for linking set
 *	event waits queues onto a wait queue.  In this way, an event
 *	can be constructed so that any thread waiting on any number
 *	of associated wait queues can handle the event, while letting
 *	the thread only be linked on the single wait queue it blocked on.
 *
 *	One use: ports in multiple portsets.  Each thread is queued up
 *	on the portset that it specifically blocked on during a receive
 *	operation.  Each port's event queue links in all the portset
 *	event queues of which it is a member.  An IPC event post associated
 *	with that port may wake up any thread from any of those portsets,
 *	or one that was waiting locally on the port itself.
 */
typedef struct _wait_queue_link {
	WaitQueueElement		wql_element;	/* element on master */
	queue_chain_t			wql_setlinks;	/* element on set */
	queue_chain_t			wql_preposts;	/* element on set prepost list */
    wait_queue_set_t		wql_setqueue;	/* set queue */
} WaitQueueLink;

#define wql_links wql_element.wqe_links
#define wql_type  wql_element.wqe_type
#define wql_queue wql_element.wqe_queue

#define _WAIT_QUEUE_inited		0x2
#define _WAIT_QUEUE_SET_inited		0x3

#define wait_queue_is_queue(wq)	\
	((wq)->wq_type == _WAIT_QUEUE_inited)

#define wait_queue_is_set(wqs)	\
	((wqs)->wqs_type == _WAIT_QUEUE_SET_inited)

#define wait_queue_is_valid(wq)	\
	(((wq)->wq_type & ~1) == _WAIT_QUEUE_inited)

#define wait_queue_empty(wq)	(queue_empty(&(wq)->wq_queue))

#define wait_queue_held(wq)		(hw_lock_held(&(wq)->wq_interlock))
#define wait_queue_lock_try(wq) (hw_lock_try(&(wq)->wq_interlock))

/* For x86, the hardware timeout is in TSC units. */
#if defined(i386) || defined(x86_64)
#define	hwLockTimeOut LockTimeOutTSC
#else
#define	hwLockTimeOut LockTimeOut
#endif
/*
 * Double the standard lock timeout, because wait queues tend
 * to iterate over a number of threads - locking each.  If there is
 * a problem with a thread lock, it normally times out at the wait
 * queue level first, hiding the real problem.
 */

static inline void wait_queue_lock(wait_queue_t wq) {
	if (__improbable(hw_lock_to(&(wq)->wq_interlock, hwLockTimeOut * 2) == 0)) {
		boolean_t wql_acquired = FALSE;

		while (machine_timeout_suspended()) {
#if	defined(__i386__) || defined(__x86_64__)
/*
 * i386/x86_64 return with preemption disabled on a timeout for
 * diagnostic purposes.
 */
			mp_enable_preemption();
#endif
			if ((wql_acquired = hw_lock_to(&(wq)->wq_interlock, hwLockTimeOut * 2)))
				break;
		}
		if (wql_acquired == FALSE)
			panic("wait queue deadlock - wq=%p, cpu=%d\n", wq, cpu_number());
	}
	assert(wait_queue_held(wq));
}

static inline void wait_queue_unlock(wait_queue_t wq) {
	assert(wait_queue_held(wq));
	hw_lock_unlock(&(wq)->wq_interlock);
}

#define wqs_lock(wqs)		wait_queue_lock(&(wqs)->wqs_wait_queue)
#define wqs_unlock(wqs)		wait_queue_unlock(&(wqs)->wqs_wait_queue)
#define wqs_lock_try(wqs)	wait_queue__try_lock(&(wqs)->wqs_wait_queue)
#define wqs_is_preposted(wqs)	((wqs)->wqs_prepost && !queue_empty(&(wqs)->wqs_preposts))

#define wql_is_preposted(wql)	((wql)->wql_preposts.next != NULL)
#define wql_clear_prepost(wql)  ((wql)->wql_preposts.next = (wql)->wql_preposts.prev = NULL)

#define wait_queue_assert_possible(thread) \
			((thread)->wait_queue == WAIT_QUEUE_NULL)

/* bootstrap interface - can allocate/link wait_queues and sets after calling this */
__private_extern__ void wait_queue_bootstrap(void);

/******** Decomposed interfaces (to build higher level constructs) ***********/

/* assert intent to wait on a locked wait queue */
__private_extern__ wait_result_t wait_queue_assert_wait64_locked(
			wait_queue_t wait_queue,
			event64_t wait_event,
			wait_interrupt_t interruptible,
			wait_timeout_urgency_t urgency,
			uint64_t deadline,
			uint64_t leeway,
			thread_t thread);

/* pull a thread from its wait queue */
__private_extern__ void wait_queue_pull_thread_locked(
			wait_queue_t wait_queue,
			thread_t thread,
			boolean_t unlock);

/* wakeup all threads waiting for a particular event on locked queue */
__private_extern__ kern_return_t wait_queue_wakeup64_all_locked(
			wait_queue_t wait_queue,
			event64_t wake_event,
			wait_result_t result,
			boolean_t unlock);

/* wakeup one thread waiting for a particular event on locked queue */
__private_extern__ kern_return_t wait_queue_wakeup64_one_locked(
			wait_queue_t wait_queue,
			event64_t wake_event,
			wait_result_t result,
			boolean_t unlock);

/* return identity of a thread awakened for a particular <wait_queue,event> */
__private_extern__ thread_t wait_queue_wakeup64_identity_locked(
			wait_queue_t wait_queue,
			event64_t wake_event,
			wait_result_t result,
			boolean_t unlock);

/* wakeup thread iff its still waiting for a particular event on locked queue */
__private_extern__ kern_return_t wait_queue_wakeup64_thread_locked(
			wait_queue_t wait_queue,
			event64_t wake_event,
			thread_t thread,
			wait_result_t result,
			boolean_t unlock);

extern uint32_t num_wait_queues;
extern struct wait_queue *wait_queues;
/* The Jenkins "one at a time" hash.
 * TBD: There may be some value to unrolling here,
 * depending on the architecture.
 */
static inline uint32_t wq_hash(char *key)
{
	uint32_t hash = 0;
	size_t i, length = sizeof(char *);

	for (i = 0; i < length; i++) {
		hash += key[i];
		hash += (hash << 10);
		hash ^= (hash >> 6);
	}
 
	hash += (hash << 3);
	hash ^= (hash >> 11);
	hash += (hash << 15);

	hash &= (num_wait_queues - 1);
	return hash;
}

#define	wait_hash(event) wq_hash((char *)&event) 

#endif	/* MACH_KERNEL_PRIVATE */

__BEGIN_DECLS

/******** Semi-Public interfaces (not a part of a higher construct) ************/

extern unsigned int wait_queue_set_size(void);
extern unsigned int wait_queue_link_size(void);

extern kern_return_t wait_queue_init(
			wait_queue_t wait_queue,
			int policy);

extern wait_queue_set_t wait_queue_set_alloc(
			int policy);

extern kern_return_t wait_queue_set_init(
			wait_queue_set_t set_queue,
			int policy);

extern kern_return_t wait_queue_set_free(
			wait_queue_set_t set_queue);

extern wait_queue_link_t wait_queue_link_alloc(
			int policy);

extern kern_return_t wait_queue_link_free(
			wait_queue_link_t link_element);

extern kern_return_t wait_queue_link(
			wait_queue_t wait_queue,
			wait_queue_set_t set_queue);

extern kern_return_t wait_queue_link_noalloc(
			wait_queue_t wait_queue,
			wait_queue_set_t set_queue,
			wait_queue_link_t link);

extern boolean_t wait_queue_member(
			wait_queue_t wait_queue,
			wait_queue_set_t set_queue);

extern kern_return_t wait_queue_unlink(
			wait_queue_t wait_queue,
			wait_queue_set_t set_queue);

extern kern_return_t wait_queue_unlink_all(
			wait_queue_t wait_queue);

extern kern_return_t wait_queue_set_unlink_all(
			wait_queue_set_t set_queue);

#ifdef XNU_KERNEL_PRIVATE
extern kern_return_t wait_queue_set_unlink_one(
			wait_queue_set_t set_queue,
			wait_queue_link_t link);

extern kern_return_t wait_queue_unlink_nofree(
			wait_queue_t wait_queue,
			wait_queue_set_t set_queue,
			wait_queue_link_t *wqlp);

extern kern_return_t wait_queue_unlink_all_nofree(
			wait_queue_t wait_queue,
			queue_t links);

extern kern_return_t wait_queue_set_unlink_all_nofree(
			wait_queue_set_t set_queue,
			queue_t links);

extern wait_queue_link_t wait_queue_link_allocate(void);

#endif /* XNU_KERNEL_PRIVATE */

/* legacy API */
kern_return_t wait_queue_sub_init(
			wait_queue_set_t set_queue,
			int policy);

kern_return_t wait_queue_sub_clearrefs(
			wait_queue_set_t wq_set);

extern kern_return_t wait_subqueue_unlink_all(
			wait_queue_set_t set_queue);

extern wait_queue_t wait_queue_alloc(
			int policy);

extern kern_return_t wait_queue_free(
			wait_queue_t wait_queue);

/* assert intent to wait on <wait_queue,event64> pair */
extern wait_result_t wait_queue_assert_wait64(
			wait_queue_t wait_queue,
			event64_t wait_event,
			wait_interrupt_t interruptible,
			uint64_t deadline);

extern wait_result_t wait_queue_assert_wait64_with_leeway(
			wait_queue_t wait_queue,
			event64_t wait_event,
			wait_interrupt_t interruptible,
			wait_timeout_urgency_t urgency,
			uint64_t deadline,
			uint64_t leeway);

/* wakeup the most appropriate thread waiting on <wait_queue,event64> pair */
extern kern_return_t wait_queue_wakeup64_one(
			wait_queue_t wait_queue,
			event64_t wake_event,
			wait_result_t result);

/* wakeup all the threads waiting on <wait_queue,event64> pair */
extern kern_return_t wait_queue_wakeup64_all(
			wait_queue_t wait_queue,
			event64_t wake_event,
			wait_result_t result);

/* wakeup a specified thread waiting iff waiting on <wait_queue,event64> pair */
extern kern_return_t wait_queue_wakeup64_thread(
			wait_queue_t wait_queue,
			event64_t wake_event,
			thread_t thread,
			wait_result_t result);

/*
 * Compatibility Wait Queue APIs based on pointer events instead of 64bit
 * integer events.
 */

/* assert intent to wait on <wait_queue,event> pair */
extern wait_result_t wait_queue_assert_wait(
			wait_queue_t wait_queue,
			event_t wait_event,
			wait_interrupt_t interruptible,
			uint64_t deadline);

/* assert intent to wait on <wait_queue,event> pair */
extern wait_result_t wait_queue_assert_wait_with_leeway(
			wait_queue_t wait_queue,
			event_t wait_event,
			wait_interrupt_t interruptible,
			wait_timeout_urgency_t urgency,
			uint64_t deadline,
			uint64_t leeway);

/* wakeup the most appropriate thread waiting on <wait_queue,event> pair */
extern kern_return_t wait_queue_wakeup_one(
			wait_queue_t wait_queue,
			event_t wake_event,
			wait_result_t result,
	                int priority);

/* wakeup all the threads waiting on <wait_queue,event> pair */
extern kern_return_t wait_queue_wakeup_all(
			wait_queue_t wait_queue,
			event_t wake_event,
			wait_result_t result);

/* wakeup a specified thread waiting iff waiting on <wait_queue,event> pair */
extern kern_return_t wait_queue_wakeup_thread(
			wait_queue_t wait_queue,
			event_t wake_event,
			thread_t thread,
			wait_result_t result);

__END_DECLS

#endif	/* _KERN_WAIT_QUEUE_H_ */

#endif	/* KERNEL_PRIVATE */

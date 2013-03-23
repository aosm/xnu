/*
 * Copyright (c) 2000-2009 Apple Inc. All rights reserved.
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
/*
 * @OSF_COPYRIGHT@
 */
/* 
 * Mach Operating System
 * Copyright (c) 1991,1990,1989,1988,1987 Carnegie Mellon University
 * All Rights Reserved.
 * 
 * Permission to use, copy, modify and distribute this software and its
 * documentation is hereby granted, provided that both the copyright
 * notice and this permission notice appear in all copies of the
 * software, derivative works or modified versions, and any portions
 * thereof, and that both notices appear in supporting documentation.
 * 
 * CARNEGIE MELLON ALLOWS FREE USE OF THIS SOFTWARE IN ITS "AS IS"
 * CONDITION.  CARNEGIE MELLON DISCLAIMS ANY LIABILITY OF ANY KIND FOR
 * ANY DAMAGES WHATSOEVER RESULTING FROM THE USE OF THIS SOFTWARE.
 * 
 * Carnegie Mellon requests users of this software to return to
 * 
 *  Software Distribution Coordinator  or  Software.Distribution@CS.CMU.EDU
 *  School of Computer Science
 *  Carnegie Mellon University
 *  Pittsburgh PA 15213-3890
 * 
 * any improvements or extensions that they make and grant Carnegie Mellon
 * the rights to redistribute these changes.
 */
/*
 */
/*
 *	File:	vm/vm_pageout.c
 *	Author:	Avadis Tevanian, Jr., Michael Wayne Young
 *	Date:	1985
 *
 *	The proverbial page-out daemon.
 */

#include <stdint.h>

#include <debug.h>
#include <mach_pagemap.h>
#include <mach_cluster_stats.h>
#include <mach_kdb.h>
#include <advisory_pageout.h>

#include <mach/mach_types.h>
#include <mach/memory_object.h>
#include <mach/memory_object_default.h>
#include <mach/memory_object_control_server.h>
#include <mach/mach_host_server.h>
#include <mach/upl.h>
#include <mach/vm_map.h>
#include <mach/vm_param.h>
#include <mach/vm_statistics.h>
#include <mach/sdt.h>

#include <kern/kern_types.h>
#include <kern/counters.h>
#include <kern/host_statistics.h>
#include <kern/machine.h>
#include <kern/misc_protos.h>
#include <kern/sched.h>
#include <kern/thread.h>
#include <kern/xpr.h>
#include <kern/kalloc.h>

#include <machine/vm_tuning.h>
#include <machine/commpage.h>

#if CONFIG_EMBEDDED
#include <sys/kern_memorystatus.h>
#endif

#include <vm/pmap.h>
#include <vm/vm_fault.h>
#include <vm/vm_map.h>
#include <vm/vm_object.h>
#include <vm/vm_page.h>
#include <vm/vm_pageout.h>
#include <vm/vm_protos.h> /* must be last */
#include <vm/memory_object.h>
#include <vm/vm_purgeable_internal.h>

/*
 * ENCRYPTED SWAP:
 */
#include <../bsd/crypto/aes/aes.h>
extern u_int32_t random(void);	/* from <libkern/libkern.h> */

#if UPL_DEBUG
#include <libkern/OSDebug.h>
#endif

#ifndef VM_PAGEOUT_BURST_ACTIVE_THROTTLE   /* maximum iterations of the active queue to move pages to inactive */
#define VM_PAGEOUT_BURST_ACTIVE_THROTTLE  100
#endif

#ifndef VM_PAGEOUT_BURST_INACTIVE_THROTTLE  /* maximum iterations of the inactive queue w/o stealing/cleaning a page */
#ifdef	CONFIG_EMBEDDED
#define VM_PAGEOUT_BURST_INACTIVE_THROTTLE 1024
#else
#define VM_PAGEOUT_BURST_INACTIVE_THROTTLE 4096
#endif
#endif

#ifndef VM_PAGEOUT_DEADLOCK_RELIEF
#define VM_PAGEOUT_DEADLOCK_RELIEF 100	/* number of pages to move to break deadlock */
#endif

#ifndef VM_PAGEOUT_INACTIVE_RELIEF
#define VM_PAGEOUT_INACTIVE_RELIEF 50	/* minimum number of pages to move to the inactive q */
#endif

#ifndef	VM_PAGE_LAUNDRY_MAX
#define	VM_PAGE_LAUNDRY_MAX	16UL	/* maximum pageouts on a given pageout queue */
#endif	/* VM_PAGEOUT_LAUNDRY_MAX */

#ifndef	VM_PAGEOUT_BURST_WAIT
#define	VM_PAGEOUT_BURST_WAIT	30	/* milliseconds per page */
#endif	/* VM_PAGEOUT_BURST_WAIT */

#ifndef	VM_PAGEOUT_EMPTY_WAIT
#define VM_PAGEOUT_EMPTY_WAIT	200	/* milliseconds */
#endif	/* VM_PAGEOUT_EMPTY_WAIT */

#ifndef	VM_PAGEOUT_DEADLOCK_WAIT
#define VM_PAGEOUT_DEADLOCK_WAIT	300	/* milliseconds */
#endif	/* VM_PAGEOUT_DEADLOCK_WAIT */

#ifndef	VM_PAGEOUT_IDLE_WAIT
#define VM_PAGEOUT_IDLE_WAIT	10	/* milliseconds */
#endif	/* VM_PAGEOUT_IDLE_WAIT */

#ifndef VM_PAGE_SPECULATIVE_TARGET
#define VM_PAGE_SPECULATIVE_TARGET(total) ((total) * 1 / 20)
#endif /* VM_PAGE_SPECULATIVE_TARGET */

#ifndef VM_PAGE_INACTIVE_HEALTHY_LIMIT
#define VM_PAGE_INACTIVE_HEALTHY_LIMIT(total) ((total) * 1 / 200)
#endif /* VM_PAGE_INACTIVE_HEALTHY_LIMIT */


/*
 *	To obtain a reasonable LRU approximation, the inactive queue
 *	needs to be large enough to give pages on it a chance to be
 *	referenced a second time.  This macro defines the fraction
 *	of active+inactive pages that should be inactive.
 *	The pageout daemon uses it to update vm_page_inactive_target.
 *
 *	If vm_page_free_count falls below vm_page_free_target and
 *	vm_page_inactive_count is below vm_page_inactive_target,
 *	then the pageout daemon starts running.
 */

#ifndef	VM_PAGE_INACTIVE_TARGET
#define	VM_PAGE_INACTIVE_TARGET(avail)	((avail) * 1 / 3)
#endif	/* VM_PAGE_INACTIVE_TARGET */

/*
 *	Once the pageout daemon starts running, it keeps going
 *	until vm_page_free_count meets or exceeds vm_page_free_target.
 */

#ifndef	VM_PAGE_FREE_TARGET
#ifdef	CONFIG_EMBEDDED
#define	VM_PAGE_FREE_TARGET(free)	(15 + (free) / 100)
#else
#define	VM_PAGE_FREE_TARGET(free)	(15 + (free) / 80)
#endif
#endif	/* VM_PAGE_FREE_TARGET */

/*
 *	The pageout daemon always starts running once vm_page_free_count
 *	falls below vm_page_free_min.
 */

#ifndef	VM_PAGE_FREE_MIN
#ifdef	CONFIG_EMBEDDED
#define	VM_PAGE_FREE_MIN(free)		(10 + (free) / 200)
#else
#define	VM_PAGE_FREE_MIN(free)		(10 + (free) / 100)
#endif
#endif	/* VM_PAGE_FREE_MIN */

#define VM_PAGE_FREE_MIN_LIMIT		1500
#define VM_PAGE_FREE_TARGET_LIMIT	2000


/*
 *	When vm_page_free_count falls below vm_page_free_reserved,
 *	only vm-privileged threads can allocate pages.  vm-privilege
 *	allows the pageout daemon and default pager (and any other
 *	associated threads needed for default pageout) to continue
 *	operation by dipping into the reserved pool of pages.
 */

#ifndef	VM_PAGE_FREE_RESERVED
#define	VM_PAGE_FREE_RESERVED(n)	\
	((unsigned) (6 * VM_PAGE_LAUNDRY_MAX) + (n))
#endif	/* VM_PAGE_FREE_RESERVED */

/*
 *	When we dequeue pages from the inactive list, they are
 *	reactivated (ie, put back on the active queue) if referenced.
 *	However, it is possible to starve the free list if other
 *	processors are referencing pages faster than we can turn off
 *	the referenced bit.  So we limit the number of reactivations
 *	we will make per call of vm_pageout_scan().
 */
#define VM_PAGE_REACTIVATE_LIMIT_MAX 20000
#ifndef	VM_PAGE_REACTIVATE_LIMIT
#ifdef	CONFIG_EMBEDDED
#define	VM_PAGE_REACTIVATE_LIMIT(avail)	(VM_PAGE_INACTIVE_TARGET(avail) / 2)
#else
#define	VM_PAGE_REACTIVATE_LIMIT(avail)	(MAX((avail) * 1 / 20,VM_PAGE_REACTIVATE_LIMIT_MAX))
#endif
#endif	/* VM_PAGE_REACTIVATE_LIMIT */
#define VM_PAGEOUT_INACTIVE_FORCE_RECLAIM	100


/*
 * must hold the page queues lock to
 * manipulate this structure
 */
struct vm_pageout_queue {
        queue_head_t	pgo_pending;	/* laundry pages to be processed by pager's iothread */
        unsigned int	pgo_laundry;	/* current count of laundry pages on queue or in flight */
        unsigned int	pgo_maxlaundry;

        unsigned int	pgo_idle:1,	/* iothread is blocked waiting for work to do */
	                pgo_busy:1,     /* iothread is currently processing request from pgo_pending */
			pgo_throttled:1,/* vm_pageout_scan thread needs a wakeup when pgo_laundry drops */
			:0;
};

#define VM_PAGE_Q_THROTTLED(q)		\
        ((q)->pgo_laundry >= (q)->pgo_maxlaundry)


/*
 * Exported variable used to broadcast the activation of the pageout scan
 * Working Set uses this to throttle its use of pmap removes.  In this
 * way, code which runs within memory in an uncontested context does
 * not keep encountering soft faults.
 */

unsigned int	vm_pageout_scan_event_counter = 0;

/*
 * Forward declarations for internal routines.
 */

static void vm_pageout_garbage_collect(int);
static void vm_pageout_iothread_continue(struct vm_pageout_queue *);
static void vm_pageout_iothread_external(void);
static void vm_pageout_iothread_internal(void);

extern void vm_pageout_continue(void);
extern void vm_pageout_scan(void);

static thread_t	vm_pageout_external_iothread = THREAD_NULL;
static thread_t	vm_pageout_internal_iothread = THREAD_NULL;

unsigned int vm_pageout_reserved_internal = 0;
unsigned int vm_pageout_reserved_really = 0;

unsigned int vm_pageout_idle_wait = 0;		/* milliseconds */
unsigned int vm_pageout_empty_wait = 0;		/* milliseconds */
unsigned int vm_pageout_burst_wait = 0;		/* milliseconds */
unsigned int vm_pageout_deadlock_wait = 0;	/* milliseconds */
unsigned int vm_pageout_deadlock_relief = 0;
unsigned int vm_pageout_inactive_relief = 0;
unsigned int vm_pageout_burst_active_throttle = 0;
unsigned int vm_pageout_burst_inactive_throttle = 0;

/*
 *	Protection against zero fill flushing live working sets derived
 *	from existing backing store and files
 */
unsigned int vm_accellerate_zf_pageout_trigger = 400;
unsigned int zf_queue_min_count = 100;
unsigned int vm_zf_queue_count = 0;

#if defined(__ppc__) /* On ppc, vm statistics are still 32-bit */
unsigned int vm_zf_count = 0;
#else
uint64_t vm_zf_count __attribute__((aligned(8))) = 0;
#endif

/*
 *	These variables record the pageout daemon's actions:
 *	how many pages it looks at and what happens to those pages.
 *	No locking needed because only one thread modifies the variables.
 */

unsigned int vm_pageout_active = 0;		/* debugging */
unsigned int vm_pageout_inactive = 0;		/* debugging */
unsigned int vm_pageout_inactive_throttled = 0;	/* debugging */
unsigned int vm_pageout_inactive_forced = 0;	/* debugging */
unsigned int vm_pageout_inactive_nolock = 0;	/* debugging */
unsigned int vm_pageout_inactive_avoid = 0;	/* debugging */
unsigned int vm_pageout_inactive_busy = 0;	/* debugging */
unsigned int vm_pageout_inactive_absent = 0;	/* debugging */
unsigned int vm_pageout_inactive_used = 0;	/* debugging */
unsigned int vm_pageout_inactive_clean = 0;	/* debugging */
unsigned int vm_pageout_inactive_dirty = 0;	/* debugging */
unsigned int vm_pageout_inactive_deactivated = 0;	/* debugging */
unsigned int vm_pageout_inactive_zf = 0;	/* debugging */
unsigned int vm_pageout_dirty_no_pager = 0;	/* debugging */
unsigned int vm_pageout_purged_objects = 0;	/* debugging */
unsigned int vm_stat_discard = 0;		/* debugging */
unsigned int vm_stat_discard_sent = 0;		/* debugging */
unsigned int vm_stat_discard_failure = 0;	/* debugging */
unsigned int vm_stat_discard_throttle = 0;	/* debugging */
unsigned int vm_pageout_reactivation_limit_exceeded = 0;	/* debugging */
unsigned int vm_pageout_catch_ups = 0;				/* debugging */
unsigned int vm_pageout_inactive_force_reclaim = 0;	/* debugging */

unsigned int vm_pageout_scan_active_throttled = 0;
unsigned int vm_pageout_scan_inactive_throttled = 0;
unsigned int vm_pageout_scan_throttle = 0;			/* debugging */
unsigned int vm_pageout_scan_throttle_aborted = 0;		/* debugging */
unsigned int vm_pageout_scan_burst_throttle = 0;		/* debugging */
unsigned int vm_pageout_scan_empty_throttle = 0;		/* debugging */
unsigned int vm_pageout_scan_deadlock_detected = 0;		/* debugging */
unsigned int vm_pageout_scan_active_throttle_success = 0;	/* debugging */
unsigned int vm_pageout_scan_inactive_throttle_success = 0;	/* debugging */

unsigned int vm_page_speculative_count_drifts = 0;
unsigned int vm_page_speculative_count_drift_max = 0;

/*
 * Backing store throttle when BS is exhausted
 */
unsigned int	vm_backing_store_low = 0;

unsigned int vm_pageout_out_of_line  = 0;
unsigned int vm_pageout_in_place  = 0;

unsigned int vm_page_steal_pageout_page = 0;

/*
 * ENCRYPTED SWAP:
 * counters and statistics...
 */
unsigned long vm_page_decrypt_counter = 0;
unsigned long vm_page_decrypt_for_upl_counter = 0;
unsigned long vm_page_encrypt_counter = 0;
unsigned long vm_page_encrypt_abort_counter = 0;
unsigned long vm_page_encrypt_already_encrypted_counter = 0;
boolean_t vm_pages_encrypted = FALSE; /* are there encrypted pages ? */

struct	vm_pageout_queue vm_pageout_queue_internal;
struct	vm_pageout_queue vm_pageout_queue_external;

unsigned int vm_page_speculative_target = 0;

vm_object_t 	vm_pageout_scan_wants_object = VM_OBJECT_NULL;

static boolean_t (* volatile consider_buffer_cache_collect)(void) = NULL;

#if DEVELOPMENT || DEBUG
unsigned long vm_cs_validated_resets = 0;
#endif

/*
 *	Routine:	vm_backing_store_disable
 *	Purpose:
 *		Suspend non-privileged threads wishing to extend
 *		backing store when we are low on backing store
 *		(Synchronized by caller)
 */
void
vm_backing_store_disable(
	boolean_t	disable)
{
	if(disable) {
		vm_backing_store_low = 1;
	} else {
		if(vm_backing_store_low) {
			vm_backing_store_low = 0;
			thread_wakeup((event_t) &vm_backing_store_low);
		}
	}
}


#if MACH_CLUSTER_STATS
unsigned long vm_pageout_cluster_dirtied = 0;
unsigned long vm_pageout_cluster_cleaned = 0;
unsigned long vm_pageout_cluster_collisions = 0;
unsigned long vm_pageout_cluster_clusters = 0;
unsigned long vm_pageout_cluster_conversions = 0;
unsigned long vm_pageout_target_collisions = 0;
unsigned long vm_pageout_target_page_dirtied = 0;
unsigned long vm_pageout_target_page_freed = 0;
#define CLUSTER_STAT(clause)	clause
#else	/* MACH_CLUSTER_STATS */
#define CLUSTER_STAT(clause)
#endif	/* MACH_CLUSTER_STATS */

/* 
 *	Routine:	vm_pageout_object_terminate
 *	Purpose:
 *		Destroy the pageout_object, and perform all of the
 *		required cleanup actions.
 * 
 *	In/Out conditions:
 *		The object must be locked, and will be returned locked.
 */
void
vm_pageout_object_terminate(
	vm_object_t	object)
{
	vm_object_t	shadow_object;

	/*
	 * Deal with the deallocation (last reference) of a pageout object
	 * (used for cleaning-in-place) by dropping the paging references/
	 * freeing pages in the original object.
	 */

	assert(object->pageout);
	shadow_object = object->shadow;
	vm_object_lock(shadow_object);

	while (!queue_empty(&object->memq)) {
		vm_page_t 		p, m;
		vm_object_offset_t	offset;

		p = (vm_page_t) queue_first(&object->memq);

		assert(p->private);
		assert(p->pageout);
		p->pageout = FALSE;
		assert(!p->cleaning);

		offset = p->offset;
		VM_PAGE_FREE(p);
		p = VM_PAGE_NULL;

		m = vm_page_lookup(shadow_object,
			offset + object->shadow_offset);

		if(m == VM_PAGE_NULL)
			continue;
		assert(m->cleaning);
		/* used as a trigger on upl_commit etc to recognize the */
		/* pageout daemon's subseqent desire to pageout a cleaning */
		/* page.  When the bit is on the upl commit code will   */
		/* respect the pageout bit in the target page over the  */
		/* caller's page list indication */
		m->dump_cleaning = FALSE;

		assert((m->dirty) || (m->precious) ||
				(m->busy && m->cleaning));

		/*
		 * Handle the trusted pager throttle.
		 * Also decrement the burst throttle (if external).
		 */
		vm_page_lock_queues();
		if (m->laundry) {
			vm_pageout_throttle_up(m);
		}

		/*
		 * Handle the "target" page(s). These pages are to be freed if
		 * successfully cleaned. Target pages are always busy, and are
		 * wired exactly once. The initial target pages are not mapped,
		 * (so cannot be referenced or modified) but converted target
		 * pages may have been modified between the selection as an
		 * adjacent page and conversion to a target.
		 */
		if (m->pageout) {
			assert(m->busy);
			assert(m->wire_count == 1);
			m->cleaning = FALSE;
			m->encrypted_cleaning = FALSE;
			m->pageout = FALSE;
#if MACH_CLUSTER_STATS
			if (m->wanted) vm_pageout_target_collisions++;
#endif
			/*
			 * Revoke all access to the page. Since the object is
			 * locked, and the page is busy, this prevents the page
			 * from being dirtied after the pmap_disconnect() call
			 * returns.
			 *
			 * Since the page is left "dirty" but "not modifed", we
			 * can detect whether the page was redirtied during
			 * pageout by checking the modify state.
			 */
			if (pmap_disconnect(m->phys_page) & VM_MEM_MODIFIED)
			      m->dirty = TRUE;
			else
			      m->dirty = FALSE;

			if (m->dirty) {
				CLUSTER_STAT(vm_pageout_target_page_dirtied++;)
				vm_page_unwire(m);/* reactivates */
				VM_STAT_INCR(reactivations);
				PAGE_WAKEUP_DONE(m);
			} else {
				CLUSTER_STAT(vm_pageout_target_page_freed++;)
				vm_page_free(m);/* clears busy, etc. */
			}
			vm_page_unlock_queues();
			continue;
		}
		/*
		 * Handle the "adjacent" pages. These pages were cleaned in
		 * place, and should be left alone.
		 * If prep_pin_count is nonzero, then someone is using the
		 * page, so make it active.
		 */
		if (!m->active && !m->inactive && !m->throttled && !m->private) {
			if (m->reference)
				vm_page_activate(m);
			else
				vm_page_deactivate(m);
		}
		if((m->busy) && (m->cleaning)) {

			/* the request_page_list case, (COPY_OUT_FROM FALSE) */
			m->busy = FALSE;

			/* We do not re-set m->dirty ! */
			/* The page was busy so no extraneous activity     */
			/* could have occurred. COPY_INTO is a read into the */
			/* new pages. CLEAN_IN_PLACE does actually write   */
			/* out the pages but handling outside of this code */
			/* will take care of resetting dirty. We clear the */
			/* modify however for the Programmed I/O case.     */ 
			pmap_clear_modify(m->phys_page);

			m->absent = FALSE;
			m->overwriting = FALSE;
		} else if (m->overwriting) {
			/* alternate request page list, write to page_list */
			/* case.  Occurs when the original page was wired  */
			/* at the time of the list request */
			assert(VM_PAGE_WIRED(m));
			vm_page_unwire(m);/* reactivates */
			m->overwriting = FALSE;
		} else {
		/*
		 * Set the dirty state according to whether or not the page was
		 * modified during the pageout. Note that we purposefully do
		 * NOT call pmap_clear_modify since the page is still mapped.
		 * If the page were to be dirtied between the 2 calls, this
		 * this fact would be lost. This code is only necessary to
		 * maintain statistics, since the pmap module is always
		 * consulted if m->dirty is false.
		 */
#if MACH_CLUSTER_STATS
			m->dirty = pmap_is_modified(m->phys_page);

			if (m->dirty)	vm_pageout_cluster_dirtied++;
			else		vm_pageout_cluster_cleaned++;
			if (m->wanted)	vm_pageout_cluster_collisions++;
#else
			m->dirty = 0;
#endif
		}
		m->cleaning = FALSE;
		m->encrypted_cleaning = FALSE;

		/*
		 * Wakeup any thread waiting for the page to be un-cleaning.
		 */
		PAGE_WAKEUP(m);
		vm_page_unlock_queues();
	}
	/*
	 * Account for the paging reference taken in vm_paging_object_allocate.
	 */
	vm_object_activity_end(shadow_object);
	vm_object_unlock(shadow_object);

	assert(object->ref_count == 0);
	assert(object->paging_in_progress == 0);
	assert(object->activity_in_progress == 0);
	assert(object->resident_page_count == 0);
	return;
}

/*
 * Routine:	vm_pageclean_setup
 *
 * Purpose:	setup a page to be cleaned (made non-dirty), but not
 *		necessarily flushed from the VM page cache.
 *		This is accomplished by cleaning in place.
 *
 *		The page must not be busy, and new_object
 *		must be locked.
 *
 */
void
vm_pageclean_setup(
	vm_page_t		m,
	vm_page_t		new_m,
	vm_object_t		new_object,
	vm_object_offset_t	new_offset)
{
	assert(!m->busy);
#if 0
	assert(!m->cleaning);
#endif

	XPR(XPR_VM_PAGEOUT,
    "vm_pageclean_setup, obj 0x%X off 0x%X page 0x%X new 0x%X new_off 0x%X\n",
		m->object, m->offset, m, 
		new_m, new_offset);

	pmap_clear_modify(m->phys_page);

	/*
	 * Mark original page as cleaning in place.
	 */
	m->cleaning = TRUE;
	m->dirty = TRUE;
	m->precious = FALSE;

	/*
	 * Convert the fictitious page to a private shadow of
	 * the real page.
	 */
	assert(new_m->fictitious);
	assert(new_m->phys_page == vm_page_fictitious_addr);
	new_m->fictitious = FALSE;
	new_m->private = TRUE;
	new_m->pageout = TRUE;
	new_m->phys_page = m->phys_page;

	vm_page_lockspin_queues();
	vm_page_wire(new_m);
	vm_page_unlock_queues();

	vm_page_insert(new_m, new_object, new_offset);
	assert(!new_m->wanted);
	new_m->busy = FALSE;
}

/*
 *	Routine:	vm_pageout_initialize_page
 *	Purpose:
 *		Causes the specified page to be initialized in
 *		the appropriate memory object. This routine is used to push
 *		pages into a copy-object when they are modified in the
 *		permanent object.
 *
 *		The page is moved to a temporary object and paged out.
 *
 *	In/out conditions:
 *		The page in question must not be on any pageout queues.
 *		The object to which it belongs must be locked.
 *		The page must be busy, but not hold a paging reference.
 *
 *	Implementation:
 *		Move this page to a completely new object.
 */
void	
vm_pageout_initialize_page(
	vm_page_t	m)
{
	vm_object_t		object;
	vm_object_offset_t	paging_offset;
	vm_page_t		holding_page;
	memory_object_t		pager;

	XPR(XPR_VM_PAGEOUT,
		"vm_pageout_initialize_page, page 0x%X\n",
		m, 0, 0, 0, 0);
	assert(m->busy);

	/*
	 *	Verify that we really want to clean this page
	 */
	assert(!m->absent);
	assert(!m->error);
	assert(m->dirty);

	/*
	 *	Create a paging reference to let us play with the object.
	 */
	object = m->object;
	paging_offset = m->offset + object->paging_offset;

	if (m->absent || m->error || m->restart || (!m->dirty && !m->precious)) {
		VM_PAGE_FREE(m);
		panic("reservation without pageout?"); /* alan */
		vm_object_unlock(object);

		return;
	}

	/*
	 * If there's no pager, then we can't clean the page.  This should 
	 * never happen since this should be a copy object and therefore not
	 * an external object, so the pager should always be there.
	 */

	pager = object->pager;

	if (pager == MEMORY_OBJECT_NULL) {
		VM_PAGE_FREE(m);
		panic("missing pager for copy object");
		return;
	}

	/* set the page for future call to vm_fault_list_request */
	vm_object_paging_begin(object);
	holding_page = NULL;

	pmap_clear_modify(m->phys_page);
	m->dirty = TRUE;
	m->busy = TRUE;
	m->list_req_pending = TRUE;
	m->cleaning = TRUE;
	m->pageout = TRUE;

	vm_page_lockspin_queues();
	vm_page_wire(m);
	vm_page_unlock_queues();

	vm_object_unlock(object);

	/*
	 *	Write the data to its pager.
	 *	Note that the data is passed by naming the new object,
	 *	not a virtual address; the pager interface has been
	 *	manipulated to use the "internal memory" data type.
	 *	[The object reference from its allocation is donated
	 *	to the eventual recipient.]
	 */
	memory_object_data_initialize(pager, paging_offset, PAGE_SIZE);

	vm_object_lock(object);
	vm_object_paging_end(object);
}

#if	MACH_CLUSTER_STATS
#define MAXCLUSTERPAGES	16
struct {
	unsigned long pages_in_cluster;
	unsigned long pages_at_higher_offsets;
	unsigned long pages_at_lower_offsets;
} cluster_stats[MAXCLUSTERPAGES];
#endif	/* MACH_CLUSTER_STATS */


/*
 * vm_pageout_cluster:
 *
 * Given a page, queue it to the appropriate I/O thread,
 * which will page it out and attempt to clean adjacent pages
 * in the same operation.
 *
 * The page must be busy, and the object and queues locked. We will take a
 * paging reference to prevent deallocation or collapse when we
 * release the object lock back at the call site.  The I/O thread
 * is responsible for consuming this reference
 *
 * The page must not be on any pageout queue.
 */

void
vm_pageout_cluster(vm_page_t m)
{
	vm_object_t	object = m->object;
        struct		vm_pageout_queue *q;


	XPR(XPR_VM_PAGEOUT,
		"vm_pageout_cluster, object 0x%X offset 0x%X page 0x%X\n",
		object, m->offset, m, 0, 0);

	VM_PAGE_CHECK(m);

	/*
	 * Only a certain kind of page is appreciated here.
	 */
	assert(m->busy && (m->dirty || m->precious) && (!VM_PAGE_WIRED(m)));
	assert(!m->cleaning && !m->pageout && !m->inactive && !m->active);
	assert(!m->throttled);

	/*
	 * protect the object from collapse - 
	 * locking in the object's paging_offset.
	 */
	vm_object_paging_begin(object);

	/*
	 * set the page for future call to vm_fault_list_request
	 * page should already be marked busy
	 */
	vm_page_wire(m);
	m->list_req_pending = TRUE;
	m->cleaning = TRUE;
	m->pageout = TRUE;
        m->laundry = TRUE;

	if (object->internal == TRUE)
	        q = &vm_pageout_queue_internal;
	else
	        q = &vm_pageout_queue_external;
	q->pgo_laundry++;

	m->pageout_queue = TRUE;
	queue_enter(&q->pgo_pending, m, vm_page_t, pageq);
	
	if (q->pgo_idle == TRUE) {
	        q->pgo_idle = FALSE;
	        thread_wakeup((event_t) &q->pgo_pending);
	}

	VM_PAGE_CHECK(m);
}


unsigned long vm_pageout_throttle_up_count = 0;

/*
 * A page is back from laundry or we are stealing it back from 
 * the laundering state.  See if there are some pages waiting to
 * go to laundry and if we can let some of them go now.
 *
 * Object and page queues must be locked.
 */
void
vm_pageout_throttle_up(
	vm_page_t	m)
{
        struct vm_pageout_queue *q;

	assert(m->laundry);
	assert(m->object != VM_OBJECT_NULL);
	assert(m->object != kernel_object);

	vm_pageout_throttle_up_count++;

	if (m->object->internal == TRUE)
	        q = &vm_pageout_queue_internal;
	else
	        q = &vm_pageout_queue_external;

	if (m->pageout_queue == TRUE) {
		m->pageout_queue = FALSE;

		queue_remove(&q->pgo_pending, m, vm_page_t, pageq);
		m->pageq.next = NULL;
		m->pageq.prev = NULL;

		vm_object_paging_end(m->object);
	}
	m->laundry = FALSE;
	q->pgo_laundry--;

	if (q->pgo_throttled == TRUE) {
	        q->pgo_throttled = FALSE;
	        thread_wakeup((event_t) &q->pgo_laundry);
	}
}


/*
 *	vm_pageout_scan does the dirty work for the pageout daemon.
 *	It returns with vm_page_queue_free_lock held and
 *	vm_page_free_wanted == 0.
 */

#define VM_PAGEOUT_DELAYED_UNLOCK_LIMIT  (3 * MAX_UPL_TRANSFER)

#define	FCS_IDLE		0
#define FCS_DELAYED		1
#define FCS_DEADLOCK_DETECTED	2

struct flow_control {
        int		state;
        mach_timespec_t	ts;
};


/*
 * VM memory pressure monitoring.
 *
 * vm_pageout_scan() keeps track of the number of pages it considers and
 * reclaims, in the currently active vm_pageout_stat[vm_pageout_stat_now].
 *
 * compute_memory_pressure() is called every second from compute_averages()
 * and moves "vm_pageout_stat_now" forward, to start accumulating the number
 * of recalimed pages in a new vm_pageout_stat[] bucket.
 *
 * mach_vm_pressure_monitor() collects past statistics about memory pressure.
 * The caller provides the number of seconds ("nsecs") worth of statistics
 * it wants, up to 30 seconds.
 * It computes the number of pages reclaimed in the past "nsecs" seconds and
 * also returns the number of pages the system still needs to reclaim at this
 * moment in time.
 */
#define VM_PAGEOUT_STAT_SIZE	31
struct vm_pageout_stat {
	unsigned int considered;
	unsigned int reclaimed;
} vm_pageout_stats[VM_PAGEOUT_STAT_SIZE] = {{0,0}, };
unsigned int vm_pageout_stat_now = 0;
unsigned int vm_memory_pressure = 0;

#define VM_PAGEOUT_STAT_BEFORE(i) \
	(((i) == 0) ? VM_PAGEOUT_STAT_SIZE - 1 : (i) - 1)
#define VM_PAGEOUT_STAT_AFTER(i) \
	(((i) == VM_PAGEOUT_STAT_SIZE - 1) ? 0 : (i) + 1)

/*
 * Called from compute_averages().
 */
void
compute_memory_pressure(
	__unused void *arg)
{
	unsigned int vm_pageout_next;

	vm_memory_pressure =
		vm_pageout_stats[VM_PAGEOUT_STAT_BEFORE(vm_pageout_stat_now)].reclaimed;

	commpage_set_memory_pressure( vm_memory_pressure );

	/* move "now" forward */
	vm_pageout_next = VM_PAGEOUT_STAT_AFTER(vm_pageout_stat_now);
	vm_pageout_stats[vm_pageout_next].considered = 0;
	vm_pageout_stats[vm_pageout_next].reclaimed = 0;
	vm_pageout_stat_now = vm_pageout_next;
}

unsigned int
mach_vm_ctl_page_free_wanted(void)
{
	unsigned int page_free_target, page_free_count, page_free_wanted;

	page_free_target = vm_page_free_target;
	page_free_count = vm_page_free_count;
	if (page_free_target > page_free_count) {
		page_free_wanted = page_free_target - page_free_count;
	} else {
		page_free_wanted = 0;
	}

	return page_free_wanted;
}

kern_return_t
mach_vm_pressure_monitor(
	boolean_t	wait_for_pressure,
	unsigned int	nsecs_monitored,
	unsigned int	*pages_reclaimed_p,
	unsigned int	*pages_wanted_p)
{
	wait_result_t	wr;
	unsigned int	vm_pageout_then, vm_pageout_now;
	unsigned int	pages_reclaimed;

	/*
	 * We don't take the vm_page_queue_lock here because we don't want
	 * vm_pressure_monitor() to get in the way of the vm_pageout_scan()
	 * thread when it's trying to reclaim memory.  We don't need fully
	 * accurate monitoring anyway...
	 */

	if (wait_for_pressure) {
		/* wait until there's memory pressure */
		while (vm_page_free_count >= vm_page_free_target) {
			wr = assert_wait((event_t) &vm_page_free_wanted,
					 THREAD_INTERRUPTIBLE);
			if (wr == THREAD_WAITING) {
				wr = thread_block(THREAD_CONTINUE_NULL);
			}
			if (wr == THREAD_INTERRUPTED) {
				return KERN_ABORTED;
			}
			if (wr == THREAD_AWAKENED) {
				/*
				 * The memory pressure might have already
				 * been relieved but let's not block again
				 * and let's report that there was memory
				 * pressure at some point.
				 */
				break;
			}
		}
	}

	/* provide the number of pages the system wants to reclaim */
	if (pages_wanted_p != NULL) {
		*pages_wanted_p = mach_vm_ctl_page_free_wanted();
	}

	if (pages_reclaimed_p == NULL) {
		return KERN_SUCCESS;
	}

	/* provide number of pages reclaimed in the last "nsecs_monitored" */
	do {
		vm_pageout_now = vm_pageout_stat_now;
		pages_reclaimed = 0;
		for (vm_pageout_then =
			     VM_PAGEOUT_STAT_BEFORE(vm_pageout_now);
		     vm_pageout_then != vm_pageout_now &&
			     nsecs_monitored-- != 0;
		     vm_pageout_then =
			     VM_PAGEOUT_STAT_BEFORE(vm_pageout_then)) {
			pages_reclaimed += vm_pageout_stats[vm_pageout_then].reclaimed;
		}
	} while (vm_pageout_now != vm_pageout_stat_now);
	*pages_reclaimed_p = pages_reclaimed;

	return KERN_SUCCESS;
}

/* Page States: Used below to maintain the page state
   before it's removed from it's Q. This saved state
   helps us do the right accounting in certain cases
*/

#define PAGE_STATE_SPECULATIVE	1
#define PAGE_STATE_THROTTLED	2
#define PAGE_STATE_ZEROFILL	3
#define PAGE_STATE_INACTIVE	4

#define VM_PAGEOUT_SCAN_HANDLE_REUSABLE_PAGE(m)				\
	MACRO_BEGIN							\
	/*								\
	 * If a "reusable" page somehow made it back into		\
	 * the active queue, it's been re-used and is not		\
	 * quite re-usable.						\
	 * If the VM object was "all_reusable", consider it		\
	 * as "all re-used" instead of converting it to			\
	 * "partially re-used", which could be expensive.		\
	 */								\
	if ((m)->reusable ||						\
	    (m)->object->all_reusable) {				\
		vm_object_reuse_pages((m)->object,			\
				      (m)->offset,			\
				      (m)->offset + PAGE_SIZE_64,	\
				      FALSE);				\
	}								\
	MACRO_END

void
vm_pageout_scan(void)
{
	unsigned int loop_count = 0;
	unsigned int inactive_burst_count = 0;
	unsigned int active_burst_count = 0;
	unsigned int reactivated_this_call;
	unsigned int reactivate_limit;
	vm_page_t   local_freeq = NULL;
	int         local_freed = 0;
	int         delayed_unlock;
	int	    refmod_state = 0;
        int	vm_pageout_deadlock_target = 0;
	struct	vm_pageout_queue *iq;
	struct	vm_pageout_queue *eq;
        struct	vm_speculative_age_q *sq;
	struct  flow_control	flow_control = { 0, { 0, 0 } };
        boolean_t inactive_throttled = FALSE;
	boolean_t try_failed;
	mach_timespec_t		ts;
	unsigned int msecs = 0;
	vm_object_t	object;
	vm_object_t	last_object_tried;
#if defined(__ppc__) /* On ppc, vm statistics are still 32-bit */
	unsigned int	zf_ratio;
	unsigned int	zf_run_count;
#else
	uint64_t	zf_ratio;
	uint64_t	zf_run_count;
#endif
	uint32_t	catch_up_count = 0;
	uint32_t	inactive_reclaim_run;
	boolean_t	forced_reclaim;
	int		page_prev_state = 0;

	flow_control.state = FCS_IDLE;
	iq = &vm_pageout_queue_internal;
	eq = &vm_pageout_queue_external;
	sq = &vm_page_queue_speculative[VM_PAGE_SPECULATIVE_AGED_Q];


        XPR(XPR_VM_PAGEOUT, "vm_pageout_scan\n", 0, 0, 0, 0, 0);

        
	vm_page_lock_queues();
	delayed_unlock = 1;	/* must be nonzero if Qs are locked, 0 if unlocked */

	/*
	 *	Calculate the max number of referenced pages on the inactive
	 *	queue that we will reactivate.
	 */
	reactivated_this_call = 0;
	reactivate_limit = VM_PAGE_REACTIVATE_LIMIT(vm_page_active_count +
						    vm_page_inactive_count);
	inactive_reclaim_run = 0;


/*???*/	/*
	 *	We want to gradually dribble pages from the active queue
	 *	to the inactive queue.  If we let the inactive queue get
	 *	very small, and then suddenly dump many pages into it,
	 *	those pages won't get a sufficient chance to be referenced
	 *	before we start taking them from the inactive queue.
	 *
	 *	We must limit the rate at which we send pages to the pagers.
	 *	data_write messages consume memory, for message buffers and
	 *	for map-copy objects.  If we get too far ahead of the pagers,
	 *	we can potentially run out of memory.
	 *
	 *	We can use the laundry count to limit directly the number
	 *	of pages outstanding to the default pager.  A similar
	 *	strategy for external pagers doesn't work, because
	 *	external pagers don't have to deallocate the pages sent them,
	 *	and because we might have to send pages to external pagers
	 *	even if they aren't processing writes.  So we also
	 *	use a burst count to limit writes to external pagers.
	 *
	 *	When memory is very tight, we can't rely on external pagers to
	 *	clean pages.  They probably aren't running, because they
	 *	aren't vm-privileged.  If we kept sending dirty pages to them,
	 *	we could exhaust the free list.
	 */


Restart:
	assert(delayed_unlock!=0);
	
	/*
	 *	A page is "zero-filled" if it was not paged in from somewhere,
	 *	and it belongs to an object at least VM_ZF_OBJECT_SIZE_THRESHOLD big.
	 *	Recalculate the zero-filled page ratio.  We use this to apportion
	 *	victimized pages between the normal and zero-filled inactive
	 *	queues according to their relative abundance in memory.  Thus if a task
	 *	is flooding memory with zf pages, we begin to hunt them down.
	 *	It would be better to throttle greedy tasks at a higher level,
	 *	but at the moment mach vm cannot do this.
	 */
	{
#if defined(__ppc__) /* On ppc, vm statistics are still 32-bit */
 		uint32_t  total  = vm_page_active_count + vm_page_inactive_count;
 		uint32_t  normal = total - vm_zf_count;
#else
		uint64_t  total  = vm_page_active_count + vm_page_inactive_count;
		uint64_t  normal = total - vm_zf_count;
#endif

		/* zf_ratio is the number of zf pages we victimize per normal page */
		
		if (vm_zf_count < vm_accellerate_zf_pageout_trigger)
			zf_ratio = 0;
		else if ((vm_zf_count <= normal) || (normal == 0))
			zf_ratio = 1;
		else 
			zf_ratio = vm_zf_count / normal;
			
		zf_run_count = 0;
	}
        
	/*
	 *	Recalculate vm_page_inactivate_target.
	 */
	vm_page_inactive_target = VM_PAGE_INACTIVE_TARGET(vm_page_active_count +
							  vm_page_inactive_count +
							  vm_page_speculative_count);
	/*
	 * don't want to wake the pageout_scan thread up everytime we fall below
	 * the targets... set a low water mark at 0.25% below the target
	 */
	vm_page_inactive_min = vm_page_inactive_target - (vm_page_inactive_target / 400);

	vm_page_speculative_target = VM_PAGE_SPECULATIVE_TARGET(vm_page_active_count +
								vm_page_inactive_count);
	object = NULL;
	last_object_tried = NULL;
	try_failed = FALSE;
	
	if ((vm_page_inactive_count + vm_page_speculative_count) < VM_PAGE_INACTIVE_HEALTHY_LIMIT(vm_page_active_count))
	        catch_up_count = vm_page_inactive_count + vm_page_speculative_count;
	else
	        catch_up_count = 0;
		    
	for (;;) {
		vm_page_t m;

		DTRACE_VM2(rev, int, 1, (uint64_t *), NULL);

		if (delayed_unlock == 0) {
		        vm_page_lock_queues();
			delayed_unlock = 1;
		}

		/*
		 *	Don't sweep through active queue more than the throttle
		 *	which should be kept relatively low
		 */
		active_burst_count = MIN(vm_pageout_burst_active_throttle,
					 vm_page_active_count);

		/*
		 *	Move pages from active to inactive.
		 */
		if ((vm_page_inactive_count + vm_page_speculative_count) >= vm_page_inactive_target)
		        goto done_moving_active_pages;

		while (!queue_empty(&vm_page_queue_active) && active_burst_count) {

		        if (active_burst_count)
			       active_burst_count--;

			vm_pageout_active++;

			m = (vm_page_t) queue_first(&vm_page_queue_active);

			assert(m->active && !m->inactive);
			assert(!m->laundry);
			assert(m->object != kernel_object);
			assert(m->phys_page != vm_page_guard_addr);

			DTRACE_VM2(scan, int, 1, (uint64_t *), NULL);

			/*
			 * Try to lock object; since we've already got the
			 * page queues lock, we can only 'try' for this one.
			 * if the 'try' fails, we need to do a mutex_pause
			 * to allow the owner of the object lock a chance to
			 * run... otherwise, we're likely to trip over this
			 * object in the same state as we work our way through
			 * the queue... clumps of pages associated with the same
			 * object are fairly typical on the inactive and active queues
			 */
			if (m->object != object) {
			        if (object != NULL) {
				        vm_object_unlock(object);
					object = NULL;
					vm_pageout_scan_wants_object = VM_OBJECT_NULL;
				}
			        if (!vm_object_lock_try_scan(m->object)) {
				        /*
					 * move page to end of active queue and continue
					 */
				        queue_remove(&vm_page_queue_active, m,
						     vm_page_t, pageq);
					queue_enter(&vm_page_queue_active, m,
						    vm_page_t, pageq);

					try_failed = TRUE;
					
					m = (vm_page_t) queue_first(&vm_page_queue_active);
					/*
					 * this is the next object we're going to be interested in
					 * try to make sure it's available after the mutex_yield
					 * returns control
					 */
					vm_pageout_scan_wants_object = m->object;

					goto done_with_activepage;
				}
				object = m->object;

				try_failed = FALSE;
			}

			/*
			 * if the page is BUSY, then we pull it
			 * off the active queue and leave it alone.
			 * when BUSY is cleared, it will get stuck
			 * back on the appropriate queue
			 */
			if (m->busy) {
				queue_remove(&vm_page_queue_active, m,
					     vm_page_t, pageq);
				m->pageq.next = NULL;
				m->pageq.prev = NULL;

				if (!m->fictitious)
					vm_page_active_count--;
				m->active = FALSE;

				goto done_with_activepage;
			}

			/* deal with a rogue "reusable" page */
			VM_PAGEOUT_SCAN_HANDLE_REUSABLE_PAGE(m);

			/*
			 *	Deactivate the page while holding the object
			 *	locked, so we know the page is still not busy.
			 *	This should prevent races between pmap_enter
			 *	and pmap_clear_reference.  The page might be
			 *	absent or fictitious, but vm_page_deactivate
			 *	can handle that.
			 */
			vm_page_deactivate(m);

done_with_activepage:
			if (delayed_unlock++ > VM_PAGEOUT_DELAYED_UNLOCK_LIMIT || try_failed == TRUE) {

			        if (object != NULL) {
					vm_pageout_scan_wants_object = VM_OBJECT_NULL;
				        vm_object_unlock(object);
					object = NULL;
				}
			        if (local_freeq) {
					vm_page_unlock_queues();
				        vm_page_free_list(local_freeq, TRUE);
					
					local_freeq = NULL;
					local_freed = 0;
					vm_page_lock_queues();
				} else
					lck_mtx_yield(&vm_page_queue_lock);

				delayed_unlock = 1;

				/*
				 * continue the while loop processing
				 * the active queue... need to hold
				 * the page queues lock
				 */
			}
		}



		/**********************************************************************
		 * above this point we're playing with the active queue
		 * below this point we're playing with the throttling mechanisms
		 * and the inactive queue
		 **********************************************************************/

done_moving_active_pages:

		/*
		 *	We are done if we have met our target *and*
		 *	nobody is still waiting for a page.
		 */
		if (vm_page_free_count + local_freed >= vm_page_free_target) {
			if (object != NULL) {
			        vm_object_unlock(object);
				object = NULL;
			}
			vm_pageout_scan_wants_object = VM_OBJECT_NULL;

			if (local_freeq) {
				vm_page_unlock_queues();
			        vm_page_free_list(local_freeq, TRUE);
					
				local_freeq = NULL;
				local_freed = 0;
				vm_page_lock_queues();
			}
			/*
			 * inactive target still not met... keep going
			 * until we get the queues balanced
			 */

			/*
			 *	Recalculate vm_page_inactivate_target.
			 */
			vm_page_inactive_target = VM_PAGE_INACTIVE_TARGET(vm_page_active_count +
									  vm_page_inactive_count +
									  vm_page_speculative_count);

#ifndef	CONFIG_EMBEDDED
			/*
			 * XXX: if no active pages can be reclaimed, pageout scan can be stuck trying 
			 *      to balance the queues
			 */
			if (((vm_page_inactive_count + vm_page_speculative_count) < vm_page_inactive_target) &&
			    !queue_empty(&vm_page_queue_active))
			        continue;
#endif

		        lck_mtx_lock(&vm_page_queue_free_lock);

			if ((vm_page_free_count >= vm_page_free_target) &&
			    (vm_page_free_wanted == 0) && (vm_page_free_wanted_privileged == 0)) {

			        vm_page_unlock_queues();

				thread_wakeup((event_t) &vm_pageout_garbage_collect);

				assert(vm_pageout_scan_wants_object == VM_OBJECT_NULL);

				return;
			}
			lck_mtx_unlock(&vm_page_queue_free_lock);
		}
		
		/*
		 * Before anything, we check if we have any ripe volatile 
		 * objects around. If so, try to purge the first object.
		 * If the purge fails, fall through to reclaim a page instead.
		 * If the purge succeeds, go back to the top and reevalute
		 * the new memory situation.
		 */
		assert (available_for_purge>=0);
		if (available_for_purge)
		{
		        if (object != NULL) {
			        vm_object_unlock(object);
				object = NULL;
			}
			if(TRUE == vm_purgeable_object_purge_one()) {
				continue;
			}
		}
        
		if (queue_empty(&sq->age_q) && vm_page_speculative_count) {
		        /*
			 * try to pull pages from the aging bins
			 * see vm_page.h for an explanation of how
			 * this mechanism works
			 */
		        struct vm_speculative_age_q	*aq;
			mach_timespec_t	ts_fully_aged;
			boolean_t	can_steal = FALSE;
			int num_scanned_queues;
		       
			aq = &vm_page_queue_speculative[speculative_steal_index];

			num_scanned_queues = 0;
			while (queue_empty(&aq->age_q) &&
			       num_scanned_queues++ != VM_PAGE_MAX_SPECULATIVE_AGE_Q) {

			        speculative_steal_index++;

				if (speculative_steal_index > VM_PAGE_MAX_SPECULATIVE_AGE_Q)
				        speculative_steal_index = VM_PAGE_MIN_SPECULATIVE_AGE_Q;
				
				aq = &vm_page_queue_speculative[speculative_steal_index];
			}

			if (num_scanned_queues ==
			    VM_PAGE_MAX_SPECULATIVE_AGE_Q + 1) {
				/*
				 * XXX We've scanned all the speculative
				 * queues but still haven't found one
				 * that is not empty, even though
				 * vm_page_speculative_count is not 0.
				 */
				/* report the anomaly... */
				printf("vm_pageout_scan: "
				       "all speculative queues empty "
				       "but count=%d.  Re-adjusting.\n",
				       vm_page_speculative_count);
				if (vm_page_speculative_count >
				    vm_page_speculative_count_drift_max)
					vm_page_speculative_count_drift_max = vm_page_speculative_count;
				vm_page_speculative_count_drifts++;
#if 6553678
				Debugger("vm_pageout_scan: no speculative pages");
#endif
				/* readjust... */
				vm_page_speculative_count = 0;
				/* ... and continue */
				continue;
			}

			if (vm_page_speculative_count > vm_page_speculative_target)
			        can_steal = TRUE;
			else {
			        ts_fully_aged.tv_sec = (VM_PAGE_MAX_SPECULATIVE_AGE_Q * VM_PAGE_SPECULATIVE_Q_AGE_MS) / 1000;
				ts_fully_aged.tv_nsec = ((VM_PAGE_MAX_SPECULATIVE_AGE_Q * VM_PAGE_SPECULATIVE_Q_AGE_MS) % 1000)
				                      * 1000 * NSEC_PER_USEC;

				ADD_MACH_TIMESPEC(&ts_fully_aged, &aq->age_ts);

				clock_sec_t sec;
				clock_nsec_t nsec;
			        clock_get_system_nanotime(&sec, &nsec);
				ts.tv_sec = (unsigned int) sec;
				ts.tv_nsec = nsec;

				if (CMP_MACH_TIMESPEC(&ts, &ts_fully_aged) >= 0)
				        can_steal = TRUE;
			}
			if (can_steal == TRUE)
			        vm_page_speculate_ageit(aq);
		}

		/*
		 * Sometimes we have to pause:
		 *	1) No inactive pages - nothing to do.
		 *	2) Flow control - default pageout queue is full
		 *	3) Loop control - no acceptable pages found on the inactive queue
		 *         within the last vm_pageout_burst_inactive_throttle iterations
		 */
		if (queue_empty(&vm_page_queue_inactive) && queue_empty(&vm_page_queue_zf) && queue_empty(&sq->age_q) &&
		    (VM_PAGE_Q_THROTTLED(iq) || queue_empty(&vm_page_queue_throttled))) {
		        vm_pageout_scan_empty_throttle++;
			msecs = vm_pageout_empty_wait;
			goto vm_pageout_scan_delay;

		} else if (inactive_burst_count >= 
			   MIN(vm_pageout_burst_inactive_throttle,
			       (vm_page_inactive_count +
				vm_page_speculative_count))) {
		        vm_pageout_scan_burst_throttle++;
			msecs = vm_pageout_burst_wait;
			goto vm_pageout_scan_delay;

		} else if (VM_PAGE_Q_THROTTLED(iq) && IP_VALID(memory_manager_default)) {
			clock_sec_t sec;
			clock_nsec_t nsec;

		        switch (flow_control.state) {

			case FCS_IDLE:
reset_deadlock_timer:
			        ts.tv_sec = vm_pageout_deadlock_wait / 1000;
				ts.tv_nsec = (vm_pageout_deadlock_wait % 1000) * 1000 * NSEC_PER_USEC;
			        clock_get_system_nanotime(&sec, &nsec);
				flow_control.ts.tv_sec = (unsigned int) sec;
				flow_control.ts.tv_nsec = nsec;
				ADD_MACH_TIMESPEC(&flow_control.ts, &ts);
				
				flow_control.state = FCS_DELAYED;
				msecs = vm_pageout_deadlock_wait;

				break;
					
			case FCS_DELAYED:
			        clock_get_system_nanotime(&sec, &nsec);
				ts.tv_sec = (unsigned int) sec;
				ts.tv_nsec = nsec;

				if (CMP_MACH_TIMESPEC(&ts, &flow_control.ts) >= 0) {
				        /*
					 * the pageout thread for the default pager is potentially
					 * deadlocked since the 
					 * default pager queue has been throttled for more than the
					 * allowable time... we need to move some clean pages or dirty
					 * pages belonging to the external pagers if they aren't throttled
					 * vm_page_free_wanted represents the number of threads currently
					 * blocked waiting for pages... we'll move one page for each of
					 * these plus a fixed amount to break the logjam... once we're done
					 * moving this number of pages, we'll re-enter the FSC_DELAYED state
					 * with a new timeout target since we have no way of knowing 
					 * whether we've broken the deadlock except through observation
					 * of the queue associated with the default pager... we need to
					 * stop moving pages and allow the system to run to see what
					 * state it settles into.
					 */
				        vm_pageout_deadlock_target = vm_pageout_deadlock_relief + vm_page_free_wanted + vm_page_free_wanted_privileged;
					vm_pageout_scan_deadlock_detected++;
					flow_control.state = FCS_DEADLOCK_DETECTED;

					thread_wakeup((event_t) &vm_pageout_garbage_collect);
					goto consider_inactive;
				}
				/*
				 * just resniff instead of trying
				 * to compute a new delay time... we're going to be
				 * awakened immediately upon a laundry completion,
				 * so we won't wait any longer than necessary
				 */
				msecs = vm_pageout_idle_wait;
				break;

			case FCS_DEADLOCK_DETECTED:
			        if (vm_pageout_deadlock_target)
				        goto consider_inactive;
				goto reset_deadlock_timer;

			}
			vm_pageout_scan_throttle++;
			iq->pgo_throttled = TRUE;
vm_pageout_scan_delay:
			if (object != NULL) {
			        vm_object_unlock(object);
				object = NULL;
			}
			vm_pageout_scan_wants_object = VM_OBJECT_NULL;

			if (local_freeq) {
				vm_page_unlock_queues();
			        vm_page_free_list(local_freeq, TRUE);
					
				local_freeq = NULL;
				local_freed = 0;
				vm_page_lock_queues();

				if (flow_control.state == FCS_DELAYED &&
				    !VM_PAGE_Q_THROTTLED(iq)) {
					flow_control.state = FCS_IDLE;
					vm_pageout_scan_throttle_aborted++;
					goto consider_inactive;
				}
			}
#if CONFIG_EMBEDDED
			{
			int percent_avail;

			/*
			 * Decide if we need to send a memory status notification.
			 */
			percent_avail = 
				(vm_page_active_count + vm_page_inactive_count + 
				 vm_page_speculative_count + vm_page_free_count +
				 (IP_VALID(memory_manager_default)?0:vm_page_purgeable_count) ) * 100 /
				atop_64(max_mem);
			if (percent_avail >= (kern_memorystatus_level + 5) || 
			    percent_avail <= (kern_memorystatus_level - 5)) {
				kern_memorystatus_level = percent_avail;
				thread_wakeup((event_t)&kern_memorystatus_wakeup);
			}
			}
#endif
			assert_wait_timeout((event_t) &iq->pgo_laundry, THREAD_INTERRUPTIBLE, msecs, 1000*NSEC_PER_USEC);
			counter(c_vm_pageout_scan_block++);

			vm_page_unlock_queues();

			assert(vm_pageout_scan_wants_object == VM_OBJECT_NULL);

			thread_block(THREAD_CONTINUE_NULL);

			vm_page_lock_queues();
			delayed_unlock = 1;

			iq->pgo_throttled = FALSE;

			if (loop_count >= vm_page_inactive_count)
				loop_count = 0;
			inactive_burst_count = 0;

			goto Restart;
			/*NOTREACHED*/
		}


		flow_control.state = FCS_IDLE;
consider_inactive:
		loop_count++;
		inactive_burst_count++;
		vm_pageout_inactive++;

		/* Choose a victim. */
		
		while (1) {	
			m = NULL;
			
			if (IP_VALID(memory_manager_default)) {
				assert(vm_page_throttled_count == 0);
				assert(queue_empty(&vm_page_queue_throttled));
			}

			/*
			 * The most eligible pages are ones we paged in speculatively,
			 * but which have not yet been touched.
			 */
			if ( !queue_empty(&sq->age_q) ) {
			        m = (vm_page_t) queue_first(&sq->age_q);
				break;
			}
			/*
			 * Time for a zero-filled inactive page?
			 */
			if ( ((zf_run_count < zf_ratio) && vm_zf_queue_count >= zf_queue_min_count) ||
			     queue_empty(&vm_page_queue_inactive)) {
				if ( !queue_empty(&vm_page_queue_zf) ) {
					m = (vm_page_t) queue_first(&vm_page_queue_zf);
					zf_run_count++;
					break;
				}
			}
			/*
			 * It's either a normal inactive page or nothing.
			 */
                        if ( !queue_empty(&vm_page_queue_inactive) ) {
                                m = (vm_page_t) queue_first(&vm_page_queue_inactive);
                                zf_run_count = 0;
				break;
                        }

                        panic("vm_pageout: no victim");
		}

		assert(!m->active && (m->inactive || m->speculative || m->throttled));
		assert(!m->laundry);
		assert(m->object != kernel_object);
		assert(m->phys_page != vm_page_guard_addr);

		if (!m->speculative) {
			vm_pageout_stats[vm_pageout_stat_now].considered++;
		}

		DTRACE_VM2(scan, int, 1, (uint64_t *), NULL);

		/*
		 * check to see if we currently are working
		 * with the same object... if so, we've
		 * already got the lock
		 */
		if (m->object != object) {
		        /*
			 * the object associated with candidate page is 
			 * different from the one we were just working
			 * with... dump the lock if we still own it
			 */
		        if (object != NULL) {
			        vm_object_unlock(object);
				object = NULL;
				vm_pageout_scan_wants_object = VM_OBJECT_NULL;
			}
			/*
			 * Try to lock object; since we've alread got the
			 * page queues lock, we can only 'try' for this one.
			 * if the 'try' fails, we need to do a mutex_pause
			 * to allow the owner of the object lock a chance to
			 * run... otherwise, we're likely to trip over this
			 * object in the same state as we work our way through
			 * the queue... clumps of pages associated with the same
			 * object are fairly typical on the inactive and active queues
			 */
			if (!vm_object_lock_try_scan(m->object)) {
				vm_pageout_inactive_nolock++;

			requeue_page:
			        /*
				 *	Move page to end and continue.
				 * 	Don't re-issue ticket
				 */
			        if (m->zero_fill) {
					if (m->speculative) {
						panic("vm_pageout_scan(): page %p speculative and zero-fill !?\n", m);
					}
					assert(!m->speculative);
				        queue_remove(&vm_page_queue_zf, m,
						     vm_page_t, pageq);
					queue_enter(&vm_page_queue_zf, m,
						    vm_page_t, pageq);
				} else if (m->speculative) {
				        remque(&m->pageq);
					m->speculative = FALSE;
					vm_page_speculative_count--;
					
					/*
					 * move to the head of the inactive queue
					 * to get it out of the way... the speculative
					 * queue is generally too small to depend
					 * on there being enough pages from other
					 * objects to make cycling it back on the
					 * same queue a winning proposition
					 */
					queue_enter_first(&vm_page_queue_inactive, m,
							  vm_page_t, pageq);
					m->inactive = TRUE;
					vm_page_inactive_count++;
					token_new_pagecount++;
				}  else if (m->throttled) {
					queue_remove(&vm_page_queue_throttled, m,
						     vm_page_t, pageq);
					m->throttled = FALSE;
					vm_page_throttled_count--;
					
					/*
					 * not throttled any more, so can stick
					 * it on the inactive queue.
					 */
					queue_enter(&vm_page_queue_inactive, m,
						    vm_page_t, pageq);
					m->inactive = TRUE;
					vm_page_inactive_count++;
					token_new_pagecount++;
				} else {
				        queue_remove(&vm_page_queue_inactive, m,
						     vm_page_t, pageq);
#if MACH_ASSERT
					vm_page_inactive_count--;	/* balance for purgeable queue asserts */
#endif
					vm_purgeable_q_advance_all();

					queue_enter(&vm_page_queue_inactive, m,
						    vm_page_t, pageq);
#if MACH_ASSERT
					vm_page_inactive_count++;	/* balance for purgeable queue asserts */
#endif
					token_new_pagecount++;
				}
				pmap_clear_reference(m->phys_page);
				m->reference = FALSE;

				if ( !queue_empty(&sq->age_q) )
				        m = (vm_page_t) queue_first(&sq->age_q);
				else if ( ((zf_run_count < zf_ratio) && vm_zf_queue_count >= zf_queue_min_count) ||
					  queue_empty(&vm_page_queue_inactive)) {
				        if ( !queue_empty(&vm_page_queue_zf) )
					        m = (vm_page_t) queue_first(&vm_page_queue_zf);
				} else if ( !queue_empty(&vm_page_queue_inactive) ) {
				        m = (vm_page_t) queue_first(&vm_page_queue_inactive);
				}
				/*
				 * this is the next object we're going to be interested in
				 * try to make sure its available after the mutex_yield
				 * returns control
				 */
				vm_pageout_scan_wants_object = m->object;

				/*
				 * force us to dump any collected free pages
				 * and to pause before moving on
				 */
				try_failed = TRUE;

				goto done_with_inactivepage;
			}
			object = m->object;
			vm_pageout_scan_wants_object = VM_OBJECT_NULL;

			try_failed = FALSE;
		}

		/*
		 *	Paging out pages of external objects which
		 *	are currently being created must be avoided.
		 *	The pager may claim for memory, thus leading to a
		 *	possible dead lock between it and the pageout thread,
		 *	if such pages are finally chosen. The remaining assumption
		 *	is that there will finally be enough available pages in the
		 *	inactive pool to page out in order to satisfy all memory
		 *	claimed by the thread which concurrently creates the pager.
		 */
		if (!object->pager_initialized && object->pager_created) {
			/*
			 *	Move page to end and continue, hoping that
			 *	there will be enough other inactive pages to
			 *	page out so that the thread which currently
			 *	initializes the pager will succeed.
			 *	Don't re-grant the ticket, the page should
			 *	pulled from the queue and paged out whenever
			 *	one of its logically adjacent fellows is
			 *	targeted.
			 */
			vm_pageout_inactive_avoid++;
			goto requeue_page;
		}
		/*
		 *	Remove the page from its list.
		 */
		if (m->speculative) {
			remque(&m->pageq);
			page_prev_state = PAGE_STATE_SPECULATIVE;
			m->speculative = FALSE;
			vm_page_speculative_count--;
		} else if (m->throttled) {
			queue_remove(&vm_page_queue_throttled, m, vm_page_t, pageq);
			page_prev_state = PAGE_STATE_THROTTLED;
			m->throttled = FALSE;
			vm_page_throttled_count--;
		} else {
			if (m->zero_fill) {
				queue_remove(&vm_page_queue_zf, m, vm_page_t, pageq);
				page_prev_state = PAGE_STATE_ZEROFILL;
				vm_zf_queue_count--;
			} else {
				page_prev_state = PAGE_STATE_INACTIVE;
			        queue_remove(&vm_page_queue_inactive, m, vm_page_t, pageq);
			}
			m->inactive = FALSE;
			if (!m->fictitious)
				vm_page_inactive_count--;
			vm_purgeable_q_advance_all();
		}

		m->pageq.next = NULL;
		m->pageq.prev = NULL;

		if ( !m->fictitious && catch_up_count)
		        catch_up_count--;

		/*
		 * ENCRYPTED SWAP:
		 * if this page has already been picked up as part of a
		 * page-out cluster, it will be busy because it is being
		 * encrypted (see vm_object_upl_request()).  But we still
		 * want to demote it from "clean-in-place" (aka "adjacent")
		 * to "clean-and-free" (aka "target"), so let's ignore its
		 * "busy" bit here and proceed to check for "cleaning" a
		 * little bit below...
		 */
		if ( !m->encrypted_cleaning && (m->busy || !object->alive)) {
			/*
			 *	Somebody is already playing with this page.
			 *	Leave it off the pageout queues.
			 *
			 */
			vm_pageout_inactive_busy++;

			goto done_with_inactivepage;
		}

		/*
		 *	If it's absent or in error, we can reclaim the page.
		 */

		if (m->absent || m->error) {
			vm_pageout_inactive_absent++;
reclaim_page:
			if (vm_pageout_deadlock_target) {
				vm_pageout_scan_inactive_throttle_success++;
			        vm_pageout_deadlock_target--;
			}

			DTRACE_VM2(dfree, int, 1, (uint64_t *), NULL);

			if (object->internal) {
				DTRACE_VM2(anonfree, int, 1, (uint64_t *), NULL);
			} else {
				DTRACE_VM2(fsfree, int, 1, (uint64_t *), NULL);
			}
			vm_page_free_prepare_queues(m);

			/*
			 * remove page from object here since we're already
			 * behind the object lock... defer the rest of the work
			 * we'd normally do in vm_page_free_prepare_object
			 * until 'vm_page_free_list' is called
			 */
			if (m->tabled)
				vm_page_remove(m, TRUE);

			assert(m->pageq.next == NULL &&
			       m->pageq.prev == NULL);
			m->pageq.next = (queue_entry_t)local_freeq;
			local_freeq = m;
			local_freed++;

			inactive_burst_count = 0;

			if(page_prev_state != PAGE_STATE_SPECULATIVE) {
				vm_pageout_stats[vm_pageout_stat_now].reclaimed++;
				page_prev_state = 0;
			}

			goto done_with_inactivepage;
		}

		assert(!m->private);
		assert(!m->fictitious);

		/*
		 *	If already cleaning this page in place, convert from
		 *	"adjacent" to "target". We can leave the page mapped,
		 *	and vm_pageout_object_terminate will determine whether
		 *	to free or reactivate.
		 */

		if (m->cleaning) {
			m->busy = TRUE;
			m->pageout = TRUE;
			m->dump_cleaning = TRUE;
			vm_page_wire(m);

			CLUSTER_STAT(vm_pageout_cluster_conversions++);

			inactive_burst_count = 0;

			goto done_with_inactivepage;
		}

		/*
		 * If the object is empty, the page must be reclaimed even
		 * if dirty or used.
		 * If the page belongs to a volatile object, we stick it back
		 * on.
		 */
		if (object->copy == VM_OBJECT_NULL) {
			if (object->purgable == VM_PURGABLE_EMPTY) {
				m->busy = TRUE;
				if (m->pmapped == TRUE) {
					/* unmap the page */
					refmod_state = pmap_disconnect(m->phys_page);
					if (refmod_state & VM_MEM_MODIFIED) {
						m->dirty = TRUE;
					}
				}
				if (m->dirty || m->precious) {
					/* we saved the cost of cleaning this page ! */
					vm_page_purged_count++;
				}
				goto reclaim_page;
			}
			if (object->purgable == VM_PURGABLE_VOLATILE) {
				/* if it's wired, we can't put it on our queue */
				assert(!VM_PAGE_WIRED(m));
				/* just stick it back on! */
				goto reactivate_page;
			}
		}

		/*
		 *	If it's being used, reactivate.
		 *	(Fictitious pages are either busy or absent.)
		 *	First, update the reference and dirty bits
		 *	to make sure the page is unreferenced.
		 */
		refmod_state = -1;

		if (m->reference == FALSE && m->pmapped == TRUE) {
		        refmod_state = pmap_get_refmod(m->phys_page);
		  
		        if (refmod_state & VM_MEM_REFERENCED)
			        m->reference = TRUE;
		        if (refmod_state & VM_MEM_MODIFIED)
			        m->dirty = TRUE;
		}

		if (m->reference || m->dirty) {
			/* deal with a rogue "reusable" page */
			VM_PAGEOUT_SCAN_HANDLE_REUSABLE_PAGE(m);
		}

		if (m->reference && !m->no_cache) {
			/*
			 * The page we pulled off the inactive list has
			 * been referenced.  It is possible for other
			 * processors to be touching pages faster than we
			 * can clear the referenced bit and traverse the
			 * inactive queue, so we limit the number of
			 * reactivations.
			 */
			if (++reactivated_this_call >= reactivate_limit) {
				vm_pageout_reactivation_limit_exceeded++;
			} else if (catch_up_count) {
				vm_pageout_catch_ups++;
			} else if (++inactive_reclaim_run >= VM_PAGEOUT_INACTIVE_FORCE_RECLAIM) {
				vm_pageout_inactive_force_reclaim++;
			} else {
				uint32_t isinuse;
reactivate_page:
				if ( !object->internal && object->pager != MEMORY_OBJECT_NULL &&
				     vnode_pager_get_isinuse(object->pager, &isinuse) == KERN_SUCCESS && !isinuse) {
					/*
					 * no explict mappings of this object exist
					 * and it's not open via the filesystem
					 */
					vm_page_deactivate(m);
					vm_pageout_inactive_deactivated++;
				} else {
					/*
					 * The page was/is being used, so put back on active list.
					 */
					vm_page_activate(m);
					VM_STAT_INCR(reactivations);
				}
				vm_pageout_inactive_used++;
				inactive_burst_count = 0;

                                goto done_with_inactivepage;
			}
			/* 
			 * Make sure we call pmap_get_refmod() if it
			 * wasn't already called just above, to update
			 * the dirty bit.
			 */
			if ((refmod_state == -1) && !m->dirty && m->pmapped) {
				refmod_state = pmap_get_refmod(m->phys_page);
				if (refmod_state & VM_MEM_MODIFIED)
					m->dirty = TRUE;
			}
			forced_reclaim = TRUE;
		} else {
			forced_reclaim = FALSE;
		}

                XPR(XPR_VM_PAGEOUT,
                "vm_pageout_scan, replace object 0x%X offset 0x%X page 0x%X\n",
                object, m->offset, m, 0,0);

		/*
		 * we've got a candidate page to steal...
		 *
		 * m->dirty is up to date courtesy of the
		 * preceding check for m->reference... if 
		 * we get here, then m->reference had to be
		 * FALSE (or possibly "reactivate_limit" was
                 * exceeded), but in either case we called
                 * pmap_get_refmod() and updated both
                 * m->reference and m->dirty
		 *
		 * if it's dirty or precious we need to
		 * see if the target queue is throtttled
		 * it if is, we need to skip over it by moving it back
		 * to the end of the inactive queue
		 */

		inactive_throttled = FALSE;

		if (m->dirty || m->precious) {
		        if (object->internal) {
				if (VM_PAGE_Q_THROTTLED(iq))
				        inactive_throttled = TRUE;
			} else if (VM_PAGE_Q_THROTTLED(eq)) {
				inactive_throttled = TRUE;
			}
		}
		if (inactive_throttled == TRUE) {
throttle_inactive:
			if (!IP_VALID(memory_manager_default) &&
				object->internal && 
				(object->purgable == VM_PURGABLE_DENY ||
				 object->purgable == VM_PURGABLE_NONVOLATILE ||
				 object->purgable == VM_PURGABLE_VOLATILE )) {
			        queue_enter(&vm_page_queue_throttled, m,
					    vm_page_t, pageq);
				m->throttled = TRUE;
				vm_page_throttled_count++;
			} else {
			        if (m->zero_fill) {
					queue_enter(&vm_page_queue_zf, m,
						    vm_page_t, pageq);
					vm_zf_queue_count++;
				} else 
					queue_enter(&vm_page_queue_inactive, m,
						    vm_page_t, pageq);
				m->inactive = TRUE;
				if (!m->fictitious) {
				        vm_page_inactive_count++;
					token_new_pagecount++;
				}
			}
			vm_pageout_scan_inactive_throttled++;
			goto done_with_inactivepage;
		}

		/*
		 * we've got a page that we can steal...
		 * eliminate all mappings and make sure
		 * we have the up-to-date modified state
		 * first take the page BUSY, so that no new
		 * mappings can be made
		 */
		m->busy = TRUE;
		
		/*
		 * if we need to do a pmap_disconnect then we
		 * need to re-evaluate m->dirty since the pmap_disconnect
		 * provides the true state atomically... the 
		 * page was still mapped up to the pmap_disconnect
		 * and may have been dirtied at the last microsecond
		 *
		 * we also check for the page being referenced 'late'
		 * if it was, we first need to do a WAKEUP_DONE on it
		 * since we already set m->busy = TRUE, before 
		 * going off to reactivate it
		 *
		 * Note that if 'pmapped' is FALSE then the page is not
		 * and has not been in any map, so there is no point calling
		 * pmap_disconnect().  m->dirty and/or m->reference could
		 * have been set in anticipation of likely usage of the page.
		 */
		if (m->pmapped == TRUE) {
		        refmod_state = pmap_disconnect(m->phys_page);

		        if (refmod_state & VM_MEM_MODIFIED)
			        m->dirty = TRUE;
		        if (refmod_state & VM_MEM_REFERENCED) {
				
				/* If m->reference is already set, this page must have
				 * already failed the reactivate_limit test, so don't
				 * bump the counts twice.
				 */
				if ( ! m->reference ) {
					m->reference = TRUE;
					if (forced_reclaim ||
					    ++reactivated_this_call >= reactivate_limit)
						vm_pageout_reactivation_limit_exceeded++;
					else {
						PAGE_WAKEUP_DONE(m);
						goto reactivate_page;
					}
				}
			}
		}
		/*
		 * reset our count of pages that have been reclaimed 
		 * since the last page was 'stolen'
		 */
		inactive_reclaim_run = 0;

		/*
		 *	If it's clean and not precious, we can free the page.
		 */
		if (!m->dirty && !m->precious) {
			if (m->zero_fill)
				vm_pageout_inactive_zf++;
			vm_pageout_inactive_clean++;

			goto reclaim_page;
		}

		/*
		 * The page may have been dirtied since the last check
		 * for a throttled target queue (which may have been skipped
		 * if the page was clean then).  With the dirty page
		 * disconnected here, we can make one final check.
		 */
		{
			boolean_t disconnect_throttled = FALSE;
			if (object->internal) {
				if (VM_PAGE_Q_THROTTLED(iq))
					disconnect_throttled = TRUE;
			} else if (VM_PAGE_Q_THROTTLED(eq)) {
				disconnect_throttled = TRUE;
			}

			if (disconnect_throttled == TRUE) {
				PAGE_WAKEUP_DONE(m);
				goto throttle_inactive;
			}
		}

		vm_pageout_stats[vm_pageout_stat_now].reclaimed++;

		vm_pageout_cluster(m);

		if (m->zero_fill)
			vm_pageout_inactive_zf++;
		vm_pageout_inactive_dirty++;

		inactive_burst_count = 0;

done_with_inactivepage:
		if (delayed_unlock++ > VM_PAGEOUT_DELAYED_UNLOCK_LIMIT || try_failed == TRUE) {

		        if (object != NULL) {
				vm_pageout_scan_wants_object = VM_OBJECT_NULL;
			        vm_object_unlock(object);
				object = NULL;
			}
		        if (local_freeq) {
				vm_page_unlock_queues();
			        vm_page_free_list(local_freeq, TRUE);
				
				local_freeq = NULL;
				local_freed = 0;
				vm_page_lock_queues();
			} else
				lck_mtx_yield(&vm_page_queue_lock);

			delayed_unlock = 1;
		}
		/*
		 * back to top of pageout scan loop
		 */
	}
}


int vm_page_free_count_init;

void
vm_page_free_reserve(
	int pages)
{
	int		free_after_reserve;

	vm_page_free_reserved += pages;

	free_after_reserve = vm_page_free_count_init - vm_page_free_reserved;

	vm_page_free_min = vm_page_free_reserved +
		VM_PAGE_FREE_MIN(free_after_reserve);

	if (vm_page_free_min > VM_PAGE_FREE_MIN_LIMIT)
	        vm_page_free_min = VM_PAGE_FREE_MIN_LIMIT;

	vm_page_free_target = vm_page_free_reserved +
		VM_PAGE_FREE_TARGET(free_after_reserve);

	if (vm_page_free_target > VM_PAGE_FREE_TARGET_LIMIT)
	        vm_page_free_target = VM_PAGE_FREE_TARGET_LIMIT;

	if (vm_page_free_target < vm_page_free_min + 5)
		vm_page_free_target = vm_page_free_min + 5;

	vm_page_throttle_limit = vm_page_free_target - (vm_page_free_target / 3);
	vm_page_creation_throttle = vm_page_free_target / 2;
}

/*
 *	vm_pageout is the high level pageout daemon.
 */

void
vm_pageout_continue(void)
{
	DTRACE_VM2(pgrrun, int, 1, (uint64_t *), NULL);
	vm_pageout_scan_event_counter++;
	vm_pageout_scan();
	/* we hold vm_page_queue_free_lock now */
	assert(vm_page_free_wanted == 0);
	assert(vm_page_free_wanted_privileged == 0);
	assert_wait((event_t) &vm_page_free_wanted, THREAD_UNINT);
	lck_mtx_unlock(&vm_page_queue_free_lock);

	counter(c_vm_pageout_block++);
	thread_block((thread_continue_t)vm_pageout_continue);
	/*NOTREACHED*/
}


#ifdef FAKE_DEADLOCK

#define FAKE_COUNT	5000

int internal_count = 0;
int fake_deadlock = 0;

#endif

static void
vm_pageout_iothread_continue(struct vm_pageout_queue *q)
{
	vm_page_t	m = NULL;
	vm_object_t	object;
	boolean_t	need_wakeup;
	memory_object_t	pager;
	thread_t	self = current_thread();

	if ((vm_pageout_internal_iothread != THREAD_NULL)
	    && (self == vm_pageout_external_iothread )
	    && (self->options & TH_OPT_VMPRIV))
		self->options &= ~TH_OPT_VMPRIV;

	vm_page_lockspin_queues();

        while ( !queue_empty(&q->pgo_pending) ) {

		   q->pgo_busy = TRUE;
		   queue_remove_first(&q->pgo_pending, m, vm_page_t, pageq);
		   VM_PAGE_CHECK(m);
		   m->pageout_queue = FALSE;
		   m->pageq.next = NULL;
		   m->pageq.prev = NULL;
		   vm_page_unlock_queues();

#ifdef FAKE_DEADLOCK
		   if (q == &vm_pageout_queue_internal) {
		           vm_offset_t addr;
			   int	pg_count;

			   internal_count++;

			   if ((internal_count == FAKE_COUNT)) {

				   pg_count = vm_page_free_count + vm_page_free_reserved;

			           if (kmem_alloc(kernel_map, &addr, PAGE_SIZE * pg_count) == KERN_SUCCESS) {
				           kmem_free(kernel_map, addr, PAGE_SIZE * pg_count);
				   }
				   internal_count = 0;
				   fake_deadlock++;
			   }
		   }
#endif
		   object = m->object;

		   vm_object_lock(object);

		   if (!object->pager_initialized) {

			   /*
			    *	If there is no memory object for the page, create
			    *	one and hand it to the default pager.
			    */

			   if (!object->pager_initialized)
			           vm_object_collapse(object,
						      (vm_object_offset_t) 0,
						      TRUE);
			   if (!object->pager_initialized)
			           vm_object_pager_create(object);
			   if (!object->pager_initialized) {
			           /*
				    *	Still no pager for the object.
				    *	Reactivate the page.
				    *
				    *	Should only happen if there is no
				    *	default pager.
				    */
			           vm_page_lockspin_queues();

				   vm_pageout_queue_steal(m, TRUE);
				   vm_pageout_dirty_no_pager++;
				   vm_page_activate(m);

				   vm_page_unlock_queues();

				   /*
				    *	And we are done with it.
				    */
				   PAGE_WAKEUP_DONE(m);

			           vm_object_paging_end(object);
				   vm_object_unlock(object);

				   vm_page_lockspin_queues();
				   continue;
			   }
		   }
		   pager = object->pager;
	           if (pager == MEMORY_OBJECT_NULL) {
		           /*
			    * This pager has been destroyed by either
			    * memory_object_destroy or vm_object_destroy, and
			    * so there is nowhere for the page to go.
			    * Just free the page... VM_PAGE_FREE takes
			    * care of cleaning up all the state...
			    * including doing the vm_pageout_throttle_up
			    */

		           VM_PAGE_FREE(m);

			   vm_object_paging_end(object);
			   vm_object_unlock(object);

			   vm_page_lockspin_queues();
			   continue;
		   }
		   VM_PAGE_CHECK(m);
		   vm_object_unlock(object);
		   /*
		    * we expect the paging_in_progress reference to have
		    * already been taken on the object before it was added
		    * to the appropriate pageout I/O queue... this will
		    * keep the object from being terminated and/or the 
		    * paging_offset from changing until the I/O has 
		    * completed... therefore no need to lock the object to
		    * pull the paging_offset from it.
		    *
		    * Send the data to the pager.
		    * any pageout clustering happens there
		    */
		   memory_object_data_return(pager,
					     m->offset + object->paging_offset,
					     PAGE_SIZE,
					     NULL,
					     NULL,
					     FALSE,
					     FALSE,
					     0);

		   vm_object_lock(object);
		   vm_object_paging_end(object);
		   vm_object_unlock(object);

		   vm_page_lockspin_queues();
	}
	assert_wait((event_t) q, THREAD_UNINT);


	if (q->pgo_throttled == TRUE && !VM_PAGE_Q_THROTTLED(q)) {
	        q->pgo_throttled = FALSE;
		need_wakeup = TRUE;
	} else
		need_wakeup = FALSE;

	q->pgo_busy = FALSE;
	q->pgo_idle = TRUE;
	vm_page_unlock_queues();

	if (need_wakeup == TRUE)
	        thread_wakeup((event_t) &q->pgo_laundry);

	thread_block_parameter((thread_continue_t)vm_pageout_iothread_continue, (void *) &q->pgo_pending);
	/*NOTREACHED*/
}


static void
vm_pageout_iothread_external(void)
{
	thread_t	self = current_thread();

	self->options |= TH_OPT_VMPRIV;

	vm_pageout_iothread_continue(&vm_pageout_queue_external);
	/*NOTREACHED*/
}


static void
vm_pageout_iothread_internal(void)
{
	thread_t	self = current_thread();

	self->options |= TH_OPT_VMPRIV;

	vm_pageout_iothread_continue(&vm_pageout_queue_internal);
	/*NOTREACHED*/
}

kern_return_t
vm_set_buffer_cleanup_callout(boolean_t (*func)(void)) 
{
	if (OSCompareAndSwapPtr(NULL, func, (void * volatile *) &consider_buffer_cache_collect)) {
		return KERN_SUCCESS;
	} else {
		return KERN_FAILURE; /* Already set */
	}
}

static void
vm_pageout_garbage_collect(int collect)
{
	if (collect) {
		boolean_t buf_large_zfree = FALSE;
		stack_collect();

		/*
		 * consider_zone_gc should be last, because the other operations
		 * might return memory to zones.
		 */
		consider_machine_collect();
		if (consider_buffer_cache_collect != NULL) {
			buf_large_zfree = (*consider_buffer_cache_collect)();
		}
		consider_zone_gc(buf_large_zfree);

		consider_machine_adjust();
	}

	assert_wait((event_t) &vm_pageout_garbage_collect, THREAD_UNINT);

	thread_block_parameter((thread_continue_t) vm_pageout_garbage_collect, (void *)1);
	/*NOTREACHED*/
}



void
vm_pageout(void)
{
	thread_t	self = current_thread();
	thread_t	thread;
	kern_return_t	result;
	spl_t		s;

	/*
	 * Set thread privileges.
	 */
	s = splsched();
	thread_lock(self);
	self->priority = BASEPRI_PREEMPT - 1;
	set_sched_pri(self, self->priority);
	thread_unlock(self);

	if (!self->reserved_stack)
		self->reserved_stack = self->kernel_stack;

	splx(s);

	/*
	 *	Initialize some paging parameters.
	 */

	if (vm_pageout_idle_wait == 0)
		vm_pageout_idle_wait = VM_PAGEOUT_IDLE_WAIT;

	if (vm_pageout_burst_wait == 0)
		vm_pageout_burst_wait = VM_PAGEOUT_BURST_WAIT;

	if (vm_pageout_empty_wait == 0)
		vm_pageout_empty_wait = VM_PAGEOUT_EMPTY_WAIT;

	if (vm_pageout_deadlock_wait == 0)
		vm_pageout_deadlock_wait = VM_PAGEOUT_DEADLOCK_WAIT;

	if (vm_pageout_deadlock_relief == 0)
		vm_pageout_deadlock_relief = VM_PAGEOUT_DEADLOCK_RELIEF;

	if (vm_pageout_inactive_relief == 0)
		vm_pageout_inactive_relief = VM_PAGEOUT_INACTIVE_RELIEF;

	if (vm_pageout_burst_active_throttle == 0)
	        vm_pageout_burst_active_throttle = VM_PAGEOUT_BURST_ACTIVE_THROTTLE;

	if (vm_pageout_burst_inactive_throttle == 0)
	        vm_pageout_burst_inactive_throttle = VM_PAGEOUT_BURST_INACTIVE_THROTTLE;

	/*
	 * Set kernel task to low backing store privileged 
	 * status
	 */
	task_lock(kernel_task);
	kernel_task->priv_flags |= VM_BACKING_STORE_PRIV;
	task_unlock(kernel_task);

	vm_page_free_count_init = vm_page_free_count;

	/*
	 * even if we've already called vm_page_free_reserve
	 * call it again here to insure that the targets are
	 * accurately calculated (it uses vm_page_free_count_init)
	 * calling it with an arg of 0 will not change the reserve
	 * but will re-calculate free_min and free_target
	 */
	if (vm_page_free_reserved < VM_PAGE_FREE_RESERVED(processor_count)) {
		vm_page_free_reserve((VM_PAGE_FREE_RESERVED(processor_count)) - vm_page_free_reserved);
	} else
		vm_page_free_reserve(0);


	queue_init(&vm_pageout_queue_external.pgo_pending);
	vm_pageout_queue_external.pgo_maxlaundry = VM_PAGE_LAUNDRY_MAX;
	vm_pageout_queue_external.pgo_laundry = 0;
	vm_pageout_queue_external.pgo_idle = FALSE;
	vm_pageout_queue_external.pgo_busy = FALSE;
	vm_pageout_queue_external.pgo_throttled = FALSE;

	queue_init(&vm_pageout_queue_internal.pgo_pending);
	vm_pageout_queue_internal.pgo_maxlaundry = 0;
	vm_pageout_queue_internal.pgo_laundry = 0;
	vm_pageout_queue_internal.pgo_idle = FALSE;
	vm_pageout_queue_internal.pgo_busy = FALSE;
	vm_pageout_queue_internal.pgo_throttled = FALSE;


	/* internal pageout thread started when default pager registered first time */
	/* external pageout and garbage collection threads started here */

	result = kernel_thread_start_priority((thread_continue_t)vm_pageout_iothread_external, NULL, 
					      BASEPRI_PREEMPT - 1, 
					      &vm_pageout_external_iothread);
	if (result != KERN_SUCCESS)
		panic("vm_pageout_iothread_external: create failed");

	thread_deallocate(vm_pageout_external_iothread);

	result = kernel_thread_start_priority((thread_continue_t)vm_pageout_garbage_collect, NULL,
					      MINPRI_KERNEL, 
					      &thread);
	if (result != KERN_SUCCESS)
		panic("vm_pageout_garbage_collect: create failed");

	thread_deallocate(thread);

	vm_object_reaper_init();


	vm_pageout_continue();

	/*
	 * Unreached code!
	 *
	 * The vm_pageout_continue() call above never returns, so the code below is never
	 * executed.  We take advantage of this to declare several DTrace VM related probe
	 * points that our kernel doesn't have an analog for.  These are probe points that
	 * exist in Solaris and are in the DTrace documentation, so people may have written
	 * scripts that use them.  Declaring the probe points here means their scripts will
	 * compile and execute which we want for portability of the scripts, but since this
	 * section of code is never reached, the probe points will simply never fire.  Yes,
	 * this is basically a hack.  The problem is the DTrace probe points were chosen with
	 * Solaris specific VM events in mind, not portability to different VM implementations.
	 */

	DTRACE_VM2(execfree, int, 1, (uint64_t *), NULL);
	DTRACE_VM2(execpgin, int, 1, (uint64_t *), NULL);
	DTRACE_VM2(execpgout, int, 1, (uint64_t *), NULL);
	DTRACE_VM2(pgswapin, int, 1, (uint64_t *), NULL);
	DTRACE_VM2(pgswapout, int, 1, (uint64_t *), NULL);
	DTRACE_VM2(swapin, int, 1, (uint64_t *), NULL);
	DTRACE_VM2(swapout, int, 1, (uint64_t *), NULL);
	/*NOTREACHED*/
}

kern_return_t
vm_pageout_internal_start(void)
{
	kern_return_t result;

	vm_pageout_queue_internal.pgo_maxlaundry = VM_PAGE_LAUNDRY_MAX;
	result = kernel_thread_start_priority((thread_continue_t)vm_pageout_iothread_internal, NULL, BASEPRI_PREEMPT - 1, &vm_pageout_internal_iothread);
	if (result == KERN_SUCCESS)
		thread_deallocate(vm_pageout_internal_iothread);
	return result;
}


/*
 * when marshalling pages into a UPL and subsequently committing
 * or aborting them, it is necessary to hold 
 * the vm_page_queue_lock (a hot global lock) for certain operations
 * on the page... however, the majority of the work can be done
 * while merely holding the object lock... in fact there are certain
 * collections of pages that don't require any work brokered by the
 * vm_page_queue_lock... to mitigate the time spent behind the global
 * lock, go to a 2 pass algorithm... collect pages up to DELAYED_WORK_LIMIT
 * while doing all of the work that doesn't require the vm_page_queue_lock...
 * then call dw_do_work to acquire the vm_page_queue_lock and do the
 * necessary work for each page... we will grab the busy bit on the page
 * if it's not already held so that dw_do_work can drop the object lock
 * if it can't immediately take the vm_page_queue_lock in order to compete
 * for the locks in the same order that vm_pageout_scan takes them.
 * the operation names are modeled after the names of the routines that
 * need to be called in order to make the changes very obvious in the
 * original loop
 */

#define DELAYED_WORK_LIMIT	32

#define DW_vm_page_unwire		0x01
#define DW_vm_page_wire			0x02
#define DW_vm_page_free			0x04
#define DW_vm_page_activate		0x08
#define DW_vm_page_deactivate_internal	0x10
#define DW_vm_page_speculate	 	0x20
#define DW_vm_page_lru		 	0x40
#define DW_vm_pageout_throttle_up	0x80
#define DW_PAGE_WAKEUP			0x100
#define DW_clear_busy			0x200
#define DW_clear_reference		0x400
#define DW_set_reference		0x800

struct dw {
	vm_page_t	dw_m;
	int		dw_mask;
};


static void dw_do_work(vm_object_t object, struct dw *dwp, int dw_count);



static upl_t
upl_create(int type, int flags, upl_size_t size)
{
	upl_t	upl;
	int	page_field_size = 0;
	int	upl_flags = 0;
	int	upl_size  = sizeof(struct upl);

	size = round_page_32(size);

	if (type & UPL_CREATE_LITE) {
		page_field_size = (atop(size) + 7) >> 3;
		page_field_size = (page_field_size + 3) & 0xFFFFFFFC;

		upl_flags |= UPL_LITE;
	}
	if (type & UPL_CREATE_INTERNAL) {
		upl_size += (int) sizeof(struct upl_page_info) * atop(size);

		upl_flags |= UPL_INTERNAL;
	}
	upl = (upl_t)kalloc(upl_size + page_field_size);

	if (page_field_size)
	        bzero((char *)upl + upl_size, page_field_size);

	upl->flags = upl_flags | flags;
	upl->src_object = NULL;
	upl->kaddr = (vm_offset_t)0;
	upl->size = 0;
	upl->map_object = NULL;
	upl->ref_count = 1;
	upl->highest_page = 0;
	upl_lock_init(upl);
	upl->vector_upl = NULL;
#if UPL_DEBUG
	upl->ubc_alias1 = 0;
	upl->ubc_alias2 = 0;

	upl->upl_creator = current_thread();
	upl->upl_state = 0;
	upl->upl_commit_index = 0;
	bzero(&upl->upl_commit_records[0], sizeof(upl->upl_commit_records));

	(void) OSBacktrace(&upl->upl_create_retaddr[0], UPL_DEBUG_STACK_FRAMES);
#endif /* UPL_DEBUG */

	return(upl);
}

static void
upl_destroy(upl_t upl)
{
	int	page_field_size;  /* bit field in word size buf */
        int	size;

#if UPL_DEBUG
	{
		vm_object_t	object;

		if (upl->flags & UPL_SHADOWED) {
			object = upl->map_object->shadow;
		} else {
			object = upl->map_object;
		}
		vm_object_lock(object);
		queue_remove(&object->uplq, upl, upl_t, uplq);
		vm_object_unlock(object);
	}
#endif /* UPL_DEBUG */
	/*
	 * drop a reference on the map_object whether or
	 * not a pageout object is inserted
	 */
	if (upl->flags & UPL_SHADOWED)
		vm_object_deallocate(upl->map_object);

        if (upl->flags & UPL_DEVICE_MEMORY)
	        size = PAGE_SIZE;
	else
	        size = upl->size;
	page_field_size = 0;

	if (upl->flags & UPL_LITE) {
		page_field_size = ((size/PAGE_SIZE) + 7) >> 3;
		page_field_size = (page_field_size + 3) & 0xFFFFFFFC;
	}
	upl_lock_destroy(upl);
	upl->vector_upl = (vector_upl_t) 0xfeedbeef;
	if (upl->flags & UPL_INTERNAL) {
		kfree(upl,
		      sizeof(struct upl) + 
		      (sizeof(struct upl_page_info) * (size/PAGE_SIZE))
		      + page_field_size);
	} else {
		kfree(upl, sizeof(struct upl) + page_field_size);
	}
}

void uc_upl_dealloc(upl_t upl);
__private_extern__ void
uc_upl_dealloc(upl_t upl)
{
	if (--upl->ref_count == 0)
		upl_destroy(upl);
}

void
upl_deallocate(upl_t upl)
{
	if (--upl->ref_count == 0) {
		if(vector_upl_is_valid(upl))
			vector_upl_deallocate(upl);
		upl_destroy(upl);
	}
}

#if DEVELOPMENT || DEBUG
/*/*
 * Statistics about UPL enforcement of copy-on-write obligations.
 */
unsigned long upl_cow = 0;
unsigned long upl_cow_again = 0;
unsigned long upl_cow_pages = 0;
unsigned long upl_cow_again_pages = 0;

unsigned long iopl_cow = 0;
unsigned long iopl_cow_pages = 0;
#endif

/*  
 *	Routine:	vm_object_upl_request 
 *	Purpose:	
 *		Cause the population of a portion of a vm_object.
 *		Depending on the nature of the request, the pages
 *		returned may be contain valid data or be uninitialized.
 *		A page list structure, listing the physical pages
 *		will be returned upon request.
 *		This function is called by the file system or any other
 *		supplier of backing store to a pager.
 *		IMPORTANT NOTE: The caller must still respect the relationship
 *		between the vm_object and its backing memory object.  The
 *		caller MUST NOT substitute changes in the backing file
 *		without first doing a memory_object_lock_request on the 
 *		target range unless it is know that the pages are not
 *		shared with another entity at the pager level.
 *		Copy_in_to:
 *			if a page list structure is present
 *			return the mapped physical pages, where a
 *			page is not present, return a non-initialized
 *			one.  If the no_sync bit is turned on, don't
 *			call the pager unlock to synchronize with other
 *			possible copies of the page. Leave pages busy
 *			in the original object, if a page list structure
 *			was specified.  When a commit of the page list
 *			pages is done, the dirty bit will be set for each one.
 *		Copy_out_from:
 *			If a page list structure is present, return
 *			all mapped pages.  Where a page does not exist
 *			map a zero filled one. Leave pages busy in
 *			the original object.  If a page list structure
 *			is not specified, this call is a no-op. 
 *
 *		Note:  access of default pager objects has a rather interesting
 *		twist.  The caller of this routine, presumably the file system
 *		page cache handling code, will never actually make a request
 *		against a default pager backed object.  Only the default
 *		pager will make requests on backing store related vm_objects
 *		In this way the default pager can maintain the relationship
 *		between backing store files (abstract memory objects) and 
 *		the vm_objects (cache objects), they support.
 *
 */

__private_extern__ kern_return_t
vm_object_upl_request(
	vm_object_t		object,
	vm_object_offset_t	offset,
	upl_size_t		size,
	upl_t			*upl_ptr,
	upl_page_info_array_t	user_page_list,
	unsigned int		*page_list_count,
	int			cntrl_flags)
{
	vm_page_t		dst_page = VM_PAGE_NULL;
	vm_object_offset_t	dst_offset;
	upl_size_t		xfer_size;
	boolean_t		dirty;
	boolean_t		hw_dirty;
	upl_t			upl = NULL;
	unsigned int		entry;
#if MACH_CLUSTER_STATS
	boolean_t		encountered_lrp = FALSE;
#endif
	vm_page_t		alias_page = NULL;
        int			refmod_state = 0;
	wpl_array_t 		lite_list = NULL;
	vm_object_t		last_copy_object;
	struct	dw		dw_array[DELAYED_WORK_LIMIT];
	struct	dw		*dwp;
	int			dw_count;

	if (cntrl_flags & ~UPL_VALID_FLAGS) {
		/*
		 * For forward compatibility's sake,
		 * reject any unknown flag.
		 */
		return KERN_INVALID_VALUE;
	}
	if ( (!object->internal) && (object->paging_offset != 0) )
		panic("vm_object_upl_request: external object with non-zero paging offset\n");
	if (object->phys_contiguous)
	        panic("vm_object_upl_request: contiguous object specified\n");


	if ((size / PAGE_SIZE) > MAX_UPL_SIZE)
		size = MAX_UPL_SIZE * PAGE_SIZE;

	if ( (cntrl_flags & UPL_SET_INTERNAL) && page_list_count != NULL)
	        *page_list_count = MAX_UPL_SIZE;

	if (cntrl_flags & UPL_SET_INTERNAL) {
	        if (cntrl_flags & UPL_SET_LITE) {

			upl = upl_create(UPL_CREATE_INTERNAL | UPL_CREATE_LITE, 0, size);

			user_page_list = (upl_page_info_t *) (((uintptr_t)upl) + sizeof(struct upl));
			lite_list = (wpl_array_t)
					(((uintptr_t)user_page_list) + 
					((size/PAGE_SIZE) * sizeof(upl_page_info_t)));
			if (size == 0) {
				user_page_list = NULL;
				lite_list = NULL;
			}
		} else {
		        upl = upl_create(UPL_CREATE_INTERNAL, 0, size);

			user_page_list = (upl_page_info_t *) (((uintptr_t)upl) + sizeof(struct upl));
			if (size == 0) {
				user_page_list = NULL;
			}
		}
	} else {
	        if (cntrl_flags & UPL_SET_LITE) {

			upl = upl_create(UPL_CREATE_EXTERNAL | UPL_CREATE_LITE, 0, size);

			lite_list = (wpl_array_t) (((uintptr_t)upl) + sizeof(struct upl));
			if (size == 0) {
				lite_list = NULL;
			}
		} else {
		        upl = upl_create(UPL_CREATE_EXTERNAL, 0, size);
		}
	}
	*upl_ptr = upl;
	
	if (user_page_list)
	        user_page_list[0].device = FALSE;

	if (cntrl_flags & UPL_SET_LITE) {
	        upl->map_object = object;
	} else {
	        upl->map_object = vm_object_allocate(size);
		/*
		 * No neeed to lock the new object: nobody else knows
		 * about it yet, so it's all ours so far.
		 */
		upl->map_object->shadow = object;
		upl->map_object->pageout = TRUE;
		upl->map_object->can_persist = FALSE;
		upl->map_object->copy_strategy = MEMORY_OBJECT_COPY_NONE;
		upl->map_object->shadow_offset = offset;
		upl->map_object->wimg_bits = object->wimg_bits;

		VM_PAGE_GRAB_FICTITIOUS(alias_page);

		upl->flags |= UPL_SHADOWED;
	}
	/*
	 * ENCRYPTED SWAP:
	 * Just mark the UPL as "encrypted" here.
	 * We'll actually encrypt the pages later,
	 * in upl_encrypt(), when the caller has
	 * selected which pages need to go to swap.
	 */
	if (cntrl_flags & UPL_ENCRYPT)
		upl->flags |= UPL_ENCRYPTED;

	if (cntrl_flags & UPL_FOR_PAGEOUT)
		upl->flags |= UPL_PAGEOUT;

	vm_object_lock(object);
	vm_object_activity_begin(object);

	/*
	 * we can lock in the paging_offset once paging_in_progress is set
	 */
	upl->size = size;
	upl->offset = offset + object->paging_offset;

#if UPL_DEBUG
	queue_enter(&object->uplq, upl, upl_t, uplq);
#endif /* UPL_DEBUG */

	if ((cntrl_flags & UPL_WILL_MODIFY) && object->copy != VM_OBJECT_NULL) {
		/*
		 * Honor copy-on-write obligations
		 *
		 * The caller is gathering these pages and
		 * might modify their contents.  We need to
		 * make sure that the copy object has its own
		 * private copies of these pages before we let
		 * the caller modify them.
		 */
		vm_object_update(object,
				 offset,
				 size,
				 NULL,
				 NULL,
				 FALSE,	/* should_return */
				 MEMORY_OBJECT_COPY_SYNC,
				 VM_PROT_NO_CHANGE);
#if DEVELOPMENT || DEBUG
		upl_cow++;
		upl_cow_pages += size >> PAGE_SHIFT;
#endif
	}
	/*
	 * remember which copy object we synchronized with
	 */
	last_copy_object = object->copy;
	entry = 0;

	xfer_size = size;
	dst_offset = offset;

	dwp = &dw_array[0];
	dw_count = 0;

	while (xfer_size) {

		dwp->dw_mask = 0;

		if ((alias_page == NULL) && !(cntrl_flags & UPL_SET_LITE)) {
			vm_object_unlock(object);
			VM_PAGE_GRAB_FICTITIOUS(alias_page);
			vm_object_lock(object);
		}
		if (cntrl_flags & UPL_COPYOUT_FROM) {
		        upl->flags |= UPL_PAGE_SYNC_DONE;

			if ( ((dst_page = vm_page_lookup(object, dst_offset)) == VM_PAGE_NULL) ||
				dst_page->fictitious ||
				dst_page->absent ||
				dst_page->error ||
			       (VM_PAGE_WIRED(dst_page) && !dst_page->pageout && !dst_page->list_req_pending)) {

				if (user_page_list)
					user_page_list[entry].phys_addr = 0;

				goto try_next_page;
			}
			/*
			 * grab this up front...
			 * a high percentange of the time we're going to
			 * need the hardware modification state a bit later
			 * anyway... so we can eliminate an extra call into
			 * the pmap layer by grabbing it here and recording it
			 */
			if (dst_page->pmapped)
			        refmod_state = pmap_get_refmod(dst_page->phys_page);
			else
			        refmod_state = 0;

			if ( (refmod_state & VM_MEM_REFERENCED) && dst_page->inactive ) {
			        /*
				 * page is on inactive list and referenced...
				 * reactivate it now... this gets it out of the
				 * way of vm_pageout_scan which would have to
				 * reactivate it upon tripping over it
				 */
				dwp->dw_mask |= DW_vm_page_activate;
			}
			if (cntrl_flags & UPL_RET_ONLY_DIRTY) {
			        /*
				 * we're only asking for DIRTY pages to be returned
				 */
			        if (dst_page->list_req_pending || !(cntrl_flags & UPL_FOR_PAGEOUT)) {
				        /*
					 * if we were the page stolen by vm_pageout_scan to be
					 * cleaned (as opposed to a buddy being clustered in 
					 * or this request is not being driven by a PAGEOUT cluster
					 * then we only need to check for the page being dirty or
					 * precious to decide whether to return it
					 */
				        if (dst_page->dirty || dst_page->precious || (refmod_state & VM_MEM_MODIFIED))
					        goto check_busy;
					goto dont_return;
				}
				/*
				 * this is a request for a PAGEOUT cluster and this page
				 * is merely along for the ride as a 'buddy'... not only
				 * does it have to be dirty to be returned, but it also
				 * can't have been referenced recently... note that we've
				 * already filtered above based on whether this page is
				 * currently on the inactive queue or it meets the page
				 * ticket (generation count) check
				 */
				if ( !(refmod_state & VM_MEM_REFERENCED) && 
				     ((refmod_state & VM_MEM_MODIFIED) || dst_page->dirty || dst_page->precious) ) {
				        goto check_busy;
				}
dont_return:
				/*
				 * if we reach here, we're not to return
				 * the page... go on to the next one
				 */
				if (user_page_list)
				        user_page_list[entry].phys_addr = 0;

				goto try_next_page;
			}
check_busy:			
			if (dst_page->busy && (!(dst_page->list_req_pending && dst_page->pageout))) {
			        if (cntrl_flags & UPL_NOBLOCK) {
				        if (user_page_list)
					        user_page_list[entry].phys_addr = 0;

					goto try_next_page;
				}
				/*
				 * someone else is playing with the
				 * page.  We will have to wait.
				 */
				PAGE_SLEEP(object, dst_page, THREAD_UNINT);

				continue;
			}
			/*
			 * Someone else already cleaning the page?
			 */
			if ((dst_page->cleaning || dst_page->absent || VM_PAGE_WIRED(dst_page)) && !dst_page->list_req_pending) {
			        if (user_page_list)
				        user_page_list[entry].phys_addr = 0;

				goto try_next_page;
			}
			/*
			 * ENCRYPTED SWAP:
			 * The caller is gathering this page and might
			 * access its contents later on.  Decrypt the
			 * page before adding it to the UPL, so that
			 * the caller never sees encrypted data.
			 */
			if (! (cntrl_flags & UPL_ENCRYPT) && dst_page->encrypted) {
			        int  was_busy;

				/*
				 * save the current state of busy
				 * mark page as busy while decrypt
				 * is in progress since it will drop
				 * the object lock...
				 */
				was_busy = dst_page->busy;
				dst_page->busy = TRUE;

				vm_page_decrypt(dst_page, 0);
				vm_page_decrypt_for_upl_counter++;
				/*
				 * restore to original busy state
				 */
				dst_page->busy = was_busy;
			}
			if (dst_page->pageout_queue == TRUE) {

				vm_page_lockspin_queues();

				if (dst_page->pageout_queue == TRUE) {
					/*
					 * we've buddied up a page for a clustered pageout
					 * that has already been moved to the pageout
					 * queue by pageout_scan... we need to remove
					 * it from the queue and drop the laundry count
					 * on that queue
					 */
					vm_pageout_throttle_up(dst_page);
				}
				vm_page_unlock_queues();
			}
#if MACH_CLUSTER_STATS
			/*
			 * pageout statistics gathering.  count
			 * all the pages we will page out that
			 * were not counted in the initial
			 * vm_pageout_scan work
			 */
			if (dst_page->list_req_pending)
			        encountered_lrp = TRUE;
			if ((dst_page->dirty ||	(dst_page->object->internal && dst_page->precious)) && !dst_page->list_req_pending) {
			        if (encountered_lrp)
				        CLUSTER_STAT(pages_at_higher_offsets++;)
				else
				        CLUSTER_STAT(pages_at_lower_offsets++;)
			}
#endif
			/*
			 * Turn off busy indication on pending
			 * pageout.  Note: we can only get here
			 * in the request pending case.
			 */
			dst_page->list_req_pending = FALSE;
			dst_page->busy = FALSE;

			hw_dirty = refmod_state & VM_MEM_MODIFIED;
			dirty = hw_dirty ? TRUE : dst_page->dirty;

			if (dst_page->phys_page > upl->highest_page)
			        upl->highest_page = dst_page->phys_page;

			if (cntrl_flags & UPL_SET_LITE) {
				unsigned int	pg_num;

				pg_num = (unsigned int) ((dst_offset-offset)/PAGE_SIZE);
				assert(pg_num == (dst_offset-offset)/PAGE_SIZE);
				lite_list[pg_num>>5] |= 1 << (pg_num & 31);

				if (hw_dirty)
				        pmap_clear_modify(dst_page->phys_page);

				/*
				 * Mark original page as cleaning 
				 * in place.
				 */
				dst_page->cleaning = TRUE;
				dst_page->precious = FALSE;
			} else {
			        /*
				 * use pageclean setup, it is more
				 * convenient even for the pageout
				 * cases here
				 */
			        vm_object_lock(upl->map_object);
				vm_pageclean_setup(dst_page, alias_page, upl->map_object, size - xfer_size);
				vm_object_unlock(upl->map_object);

				alias_page->absent = FALSE;
				alias_page = NULL;
			}
#if     MACH_PAGEMAP
			/*
			 * Record that this page has been 
			 * written out
			 */
			vm_external_state_set(object->existence_map, dst_page->offset);
#endif  /*MACH_PAGEMAP*/
			dst_page->dirty = dirty;

			if (!dirty)
				dst_page->precious = TRUE;

			if (dst_page->pageout)
			        dst_page->busy = TRUE;

			if ( (cntrl_flags & UPL_ENCRYPT) ) {
			        /*
				 * ENCRYPTED SWAP:
				 * We want to deny access to the target page
				 * because its contents are about to be
				 * encrypted and the user would be very
				 * confused to see encrypted data instead
				 * of their data.
				 * We also set "encrypted_cleaning" to allow
				 * vm_pageout_scan() to demote that page
				 * from "adjacent/clean-in-place" to
				 * "target/clean-and-free" if it bumps into
				 * this page during its scanning while we're
				 * still processing this cluster.
				 */
			        dst_page->busy = TRUE;
				dst_page->encrypted_cleaning = TRUE;
			}
			if ( !(cntrl_flags & UPL_CLEAN_IN_PLACE) ) {
			        /*
				 * deny access to the target page
				 * while it is being worked on
				 */
			        if ((!dst_page->pageout) && ( !VM_PAGE_WIRED(dst_page))) {
				        dst_page->busy = TRUE;
					dst_page->pageout = TRUE;

					dwp->dw_mask |= DW_vm_page_wire;
				}
			}
		} else {
			if ((cntrl_flags & UPL_WILL_MODIFY) && object->copy != last_copy_object) {
				/*
				 * Honor copy-on-write obligations
				 *
				 * The copy object has changed since we
				 * last synchronized for copy-on-write.
				 * Another copy object might have been
				 * inserted while we released the object's
				 * lock.  Since someone could have seen the
				 * original contents of the remaining pages
				 * through that new object, we have to
				 * synchronize with it again for the remaining
				 * pages only.  The previous pages are "busy"
				 * so they can not be seen through the new
				 * mapping.  The new mapping will see our
				 * upcoming changes for those previous pages,
				 * but that's OK since they couldn't see what
				 * was there before.  It's just a race anyway
				 * and there's no guarantee of consistency or
				 * atomicity.  We just don't want new mappings
				 * to see both the *before* and *after* pages.
				 */
				if (object->copy != VM_OBJECT_NULL) {
					vm_object_update(
						object,
						dst_offset,/* current offset */
						xfer_size, /* remaining size */
						NULL,
						NULL,
						FALSE,	   /* should_return */
						MEMORY_OBJECT_COPY_SYNC,
						VM_PROT_NO_CHANGE);

#if DEVELOPMENT || DEBUG
					upl_cow_again++;
					upl_cow_again_pages += xfer_size >> PAGE_SHIFT;
#endif
				}
				/*
				 * remember the copy object we synced with
				 */
				last_copy_object = object->copy;
			}
			dst_page = vm_page_lookup(object, dst_offset);
			
			if (dst_page != VM_PAGE_NULL) {

				if ((cntrl_flags & UPL_RET_ONLY_ABSENT)) {

					if ( !(dst_page->absent && dst_page->list_req_pending) ) {
						/*
						 * skip over pages already present in the cache
						 */
						if (user_page_list)
							user_page_list[entry].phys_addr = 0;

						goto try_next_page;
					}
				}
			        if ( !(dst_page->list_req_pending) ) {

					if (dst_page->cleaning) {
					        /*
						 * someone else is writing to the page... wait...
						 */
					        PAGE_SLEEP(object, dst_page, THREAD_UNINT);

						continue;
					}
				} else {
				        if (dst_page->fictitious &&
					    dst_page->phys_page == vm_page_fictitious_addr) {
					        assert( !dst_page->speculative);
					        /*
						 * dump the fictitious page
						 */
					        dst_page->list_req_pending = FALSE;

						VM_PAGE_FREE(dst_page);

						dst_page = NULL;

					} else if (dst_page->absent) {
					        /*
						 * the default_pager case
						 */
					        dst_page->list_req_pending = FALSE;
						dst_page->busy = FALSE;

					} else if (dst_page->pageout) {
						/*
						 * page was earmarked by vm_pageout_scan
						 * to be cleaned and stolen... we're going
						 * to take it back since we are not attempting
						 * to read that page and we don't want to stall
						 * waiting for it to be cleaned for 2 reasons...
						 * 1 - no use paging it out and back in
						 * 2 - if we stall, we may casue a deadlock in 
						 *     the FS trying to acquire the its locks
						 *     on the VNOP_PAGEOUT path presuming that
						 *     those locks are already held on the read
						 *     path before trying to create this UPL
						 *
						 * so undo all of the state that vm_pageout_scan
						 * hung on this page
						 */
						dst_page->busy = FALSE;

					       	vm_pageout_queue_steal(dst_page, FALSE);
					}
				}
			}
			if (dst_page == VM_PAGE_NULL) {
				if (object->private) {
					/* 
					 * This is a nasty wrinkle for users 
					 * of upl who encounter device or 
					 * private memory however, it is 
					 * unavoidable, only a fault can
					 * resolve the actual backing
					 * physical page by asking the
					 * backing device.
					 */
					if (user_page_list)
						user_page_list[entry].phys_addr = 0;

					goto try_next_page;
				}
				/*
				 * need to allocate a page
				 */
		 		dst_page = vm_page_grab();

				if (dst_page == VM_PAGE_NULL) {
				        if ( (cntrl_flags & (UPL_RET_ONLY_ABSENT | UPL_NOBLOCK)) == (UPL_RET_ONLY_ABSENT | UPL_NOBLOCK)) {
					       /*
						* we don't want to stall waiting for pages to come onto the free list
						* while we're already holding absent pages in this UPL
						* the caller will deal with the empty slots
						*/
					        if (user_page_list)
						        user_page_list[entry].phys_addr = 0;

						goto try_next_page;
					}
				        /*
					 * no pages available... wait
					 * then try again for the same
					 * offset...
					 */
					vm_object_unlock(object);
					VM_PAGE_WAIT();
					vm_object_lock(object);

					continue;
				}
				vm_page_insert(dst_page, object, dst_offset);

				dst_page->absent = TRUE;
				dst_page->busy = FALSE;

				if (cntrl_flags & UPL_RET_ONLY_ABSENT) {
				        /*
					 * if UPL_RET_ONLY_ABSENT was specified,
					 * than we're definitely setting up a
					 * upl for a clustered read/pagein 
					 * operation... mark the pages as clustered
					 * so upl_commit_range can put them on the
					 * speculative list
					 */
				        dst_page->clustered = TRUE;
				}
			}
			if (dst_page->fictitious) {
				panic("need corner case for fictitious page");
			}
			if (dst_page->busy) {
				/*
				 * someone else is playing with the
				 * page.  We will have to wait.
				 */
				PAGE_SLEEP(object, dst_page, THREAD_UNINT);

				continue;
			}
			/*
			 * ENCRYPTED SWAP:
			 */
			if (cntrl_flags & UPL_ENCRYPT) {
				/*
				 * The page is going to be encrypted when we
				 * get it from the pager, so mark it so.
				 */
				dst_page->encrypted = TRUE;
			} else {
				/*
				 * Otherwise, the page will not contain
				 * encrypted data.
				 */
				dst_page->encrypted = FALSE;
			}
			dst_page->overwriting = TRUE;

			if (dst_page->pmapped) {
			        if ( !(cntrl_flags & UPL_FILE_IO))
				        /*
					 * eliminate all mappings from the
					 * original object and its prodigy
					 */
				        refmod_state = pmap_disconnect(dst_page->phys_page);
				else
				        refmod_state = pmap_get_refmod(dst_page->phys_page);
			} else
			        refmod_state = 0;

			hw_dirty = refmod_state & VM_MEM_MODIFIED;
			dirty = hw_dirty ? TRUE : dst_page->dirty;

			if (cntrl_flags & UPL_SET_LITE) {
				unsigned int	pg_num;

				pg_num = (unsigned int) ((dst_offset-offset)/PAGE_SIZE);
				assert(pg_num == (dst_offset-offset)/PAGE_SIZE);
				lite_list[pg_num>>5] |= 1 << (pg_num & 31);

				if (hw_dirty)
				        pmap_clear_modify(dst_page->phys_page);

				/*
				 * Mark original page as cleaning 
				 * in place.
				 */
				dst_page->cleaning = TRUE;
				dst_page->precious = FALSE;
			} else {
				/*
				 * use pageclean setup, it is more
				 * convenient even for the pageout
				 * cases here
				 */
			        vm_object_lock(upl->map_object);
				vm_pageclean_setup(dst_page, alias_page, upl->map_object, size - xfer_size);
			        vm_object_unlock(upl->map_object);

				alias_page->absent = FALSE;
				alias_page = NULL;
			}

			if (cntrl_flags & UPL_CLEAN_IN_PLACE) {
				/*
				 * clean in place for read implies
				 * that a write will be done on all
				 * the pages that are dirty before
				 * a upl commit is done.  The caller
				 * is obligated to preserve the
				 * contents of all pages marked dirty
				 */
				upl->flags |= UPL_CLEAR_DIRTY;
			}
			dst_page->dirty = dirty;

			if (!dirty)
				dst_page->precious = TRUE;

			if ( !VM_PAGE_WIRED(dst_page)) {
			        /*
				 * deny access to the target page while
				 * it is being worked on
				 */
				dst_page->busy = TRUE;
			} else
				dwp->dw_mask |= DW_vm_page_wire;

			/*
			 * We might be about to satisfy a fault which has been
			 * requested. So no need for the "restart" bit.
			 */
			dst_page->restart = FALSE;
			if (!dst_page->absent && !(cntrl_flags & UPL_WILL_MODIFY)) {
			        /*
				 * expect the page to be used
				 */
				dwp->dw_mask |= DW_set_reference;
			}
			dst_page->precious = (cntrl_flags & UPL_PRECIOUS) ? TRUE : FALSE;
		}
		if (dst_page->phys_page > upl->highest_page)
		        upl->highest_page = dst_page->phys_page;
		if (user_page_list) {
			user_page_list[entry].phys_addr = dst_page->phys_page;
			user_page_list[entry].pageout	= dst_page->pageout;
			user_page_list[entry].absent	= dst_page->absent;
			user_page_list[entry].dirty	= dst_page->dirty;
			user_page_list[entry].precious	= dst_page->precious;
			user_page_list[entry].device	= FALSE;
			if (dst_page->clustered == TRUE)
			        user_page_list[entry].speculative = dst_page->speculative;
			else
			        user_page_list[entry].speculative = FALSE;
			user_page_list[entry].cs_validated = dst_page->cs_validated;
			user_page_list[entry].cs_tainted = dst_page->cs_tainted;
		}
	        /*
		 * if UPL_RET_ONLY_ABSENT is set, then
		 * we are working with a fresh page and we've
		 * just set the clustered flag on it to
		 * indicate that it was drug in as part of a
		 * speculative cluster... so leave it alone
		 */
		if ( !(cntrl_flags & UPL_RET_ONLY_ABSENT)) {
		        /*
			 * someone is explicitly grabbing this page...
			 * update clustered and speculative state
			 * 
			 */
		        VM_PAGE_CONSUME_CLUSTERED(dst_page);
		}
try_next_page:
		if (dwp->dw_mask) {
			if (dwp->dw_mask & DW_vm_page_activate)
				VM_STAT_INCR(reactivations);

			if (dst_page->busy == FALSE) {
				/*
				 * dw_do_work may need to drop the object lock
				 * if it does, we need the pages it's looking at to
				 * be held stable via the busy bit.
				 */
				dst_page->busy = TRUE;
				dwp->dw_mask |= (DW_clear_busy | DW_PAGE_WAKEUP);
			}
			dwp->dw_m = dst_page;
			dwp++;
			dw_count++;

			if (dw_count >= DELAYED_WORK_LIMIT) {
				dw_do_work(object, &dw_array[0], dw_count);

				dwp = &dw_array[0];
				dw_count = 0;
			}
		}
		entry++;
		dst_offset += PAGE_SIZE_64;
		xfer_size -= PAGE_SIZE;
	}
	if (dw_count)
		dw_do_work(object, &dw_array[0], dw_count);

	if (alias_page != NULL) {
		VM_PAGE_FREE(alias_page);
	}

	if (page_list_count != NULL) {
	        if (upl->flags & UPL_INTERNAL)
			*page_list_count = 0;
		else if (*page_list_count > entry)
			*page_list_count = entry;
	}
#if UPL_DEBUG
	upl->upl_state = 1;
#endif
	vm_object_unlock(object);

	return KERN_SUCCESS;
}

/* JMM - Backward compatability for now */
kern_return_t
vm_fault_list_request(			/* forward */
	memory_object_control_t		control,
	vm_object_offset_t	offset,
	upl_size_t		size,
	upl_t			*upl_ptr,
	upl_page_info_t		**user_page_list_ptr,
	unsigned int		page_list_count,
	int			cntrl_flags);
kern_return_t
vm_fault_list_request(
	memory_object_control_t		control,
	vm_object_offset_t	offset,
	upl_size_t		size,
	upl_t			*upl_ptr,
	upl_page_info_t		**user_page_list_ptr,
	unsigned int		page_list_count,
	int			cntrl_flags)
{
	unsigned int		local_list_count;
	upl_page_info_t		*user_page_list;
	kern_return_t		kr;

	if((cntrl_flags & UPL_VECTOR)==UPL_VECTOR)
		 return KERN_INVALID_ARGUMENT;

	if (user_page_list_ptr != NULL) {
		local_list_count = page_list_count;
		user_page_list = *user_page_list_ptr;
	} else {
		local_list_count = 0;
		user_page_list = NULL;
	}
	kr =  memory_object_upl_request(control,
				offset,
				size,
				upl_ptr,
				user_page_list,
				&local_list_count,
				cntrl_flags);

	if(kr != KERN_SUCCESS)
		return kr;

	if ((user_page_list_ptr != NULL) && (cntrl_flags & UPL_INTERNAL)) {
		*user_page_list_ptr = UPL_GET_INTERNAL_PAGE_LIST(*upl_ptr);
	}

	return KERN_SUCCESS;
}

		

/*  
 *	Routine:	vm_object_super_upl_request
 *	Purpose:	
 *		Cause the population of a portion of a vm_object
 *		in much the same way as memory_object_upl_request.
 *		Depending on the nature of the request, the pages
 *		returned may be contain valid data or be uninitialized.
 *		However, the region may be expanded up to the super
 *		cluster size provided.
 */

__private_extern__ kern_return_t
vm_object_super_upl_request(
	vm_object_t object,
	vm_object_offset_t	offset,
	upl_size_t		size,
	upl_size_t		super_cluster,
	upl_t			*upl,
	upl_page_info_t		*user_page_list,
	unsigned int		*page_list_count,
	int			cntrl_flags)
{
	if (object->paging_offset > offset  || ((cntrl_flags & UPL_VECTOR)==UPL_VECTOR))
		return KERN_FAILURE;

	assert(object->paging_in_progress);
	offset = offset - object->paging_offset;

	if (super_cluster > size) {

		vm_object_offset_t	base_offset;
		upl_size_t		super_size;
		vm_object_size_t	super_size_64;

		base_offset = (offset & ~((vm_object_offset_t) super_cluster - 1));
		super_size = (offset + size) > (base_offset + super_cluster) ? super_cluster<<1 : super_cluster;
		super_size_64 = ((base_offset + super_size) > object->size) ? (object->size - base_offset) : super_size;
		super_size = (upl_size_t) super_size_64;
		assert(super_size == super_size_64);

		if (offset > (base_offset + super_size)) {
		        panic("vm_object_super_upl_request: Missed target pageout"
			      " %#llx,%#llx, %#x, %#x, %#x, %#llx\n",
			      offset, base_offset, super_size, super_cluster,
			      size, object->paging_offset);
		}
		/*
		 * apparently there is a case where the vm requests a
		 * page to be written out who's offset is beyond the
		 * object size
		 */
		if ((offset + size) > (base_offset + super_size)) {
		        super_size_64 = (offset + size) - base_offset;
			super_size = (upl_size_t) super_size_64;
			assert(super_size == super_size_64);
		}

		offset = base_offset;
		size = super_size;
	}
	return vm_object_upl_request(object, offset, size, upl, user_page_list, page_list_count, cntrl_flags);
}


kern_return_t
vm_map_create_upl(
	vm_map_t		map,
	vm_map_address_t	offset,
	upl_size_t		*upl_size,
	upl_t			*upl,
	upl_page_info_array_t	page_list,
	unsigned int		*count,
	int			*flags)
{
	vm_map_entry_t	entry;
	int		caller_flags;
	int		force_data_sync;
	int		sync_cow_data;
	vm_object_t	local_object;
	vm_map_offset_t	local_offset;
	vm_map_offset_t	local_start;
	kern_return_t	ret;

	caller_flags = *flags;

	if (caller_flags & ~UPL_VALID_FLAGS) {
		/*
		 * For forward compatibility's sake,
		 * reject any unknown flag.
		 */
		return KERN_INVALID_VALUE;
	}
	force_data_sync = (caller_flags & UPL_FORCE_DATA_SYNC);
	sync_cow_data = !(caller_flags & UPL_COPYOUT_FROM);

	if (upl == NULL)
		return KERN_INVALID_ARGUMENT;

REDISCOVER_ENTRY:
	vm_map_lock_read(map);

	if (vm_map_lookup_entry(map, offset, &entry)) {

		if ((entry->vme_end - offset) < *upl_size) {
			*upl_size = (upl_size_t) (entry->vme_end - offset);
			assert(*upl_size == entry->vme_end - offset);
		}

		if (caller_flags & UPL_QUERY_OBJECT_TYPE) {
		        *flags = 0;

			if ( !entry->is_sub_map && entry->object.vm_object != VM_OBJECT_NULL) {
			        if (entry->object.vm_object->private)
				        *flags = UPL_DEV_MEMORY;

				if (entry->object.vm_object->phys_contiguous)
					*flags |= UPL_PHYS_CONTIG;
			}
			vm_map_unlock_read(map);

			return KERN_SUCCESS;
		}
	        if (entry->object.vm_object == VM_OBJECT_NULL || !entry->object.vm_object->phys_contiguous) {
        		if ((*upl_size/PAGE_SIZE) > MAX_UPL_SIZE)
               			*upl_size = MAX_UPL_SIZE * PAGE_SIZE;
		}
		/*
		 *      Create an object if necessary.
		 */
		if (entry->object.vm_object == VM_OBJECT_NULL) {

			if (vm_map_lock_read_to_write(map))
				goto REDISCOVER_ENTRY;

			entry->object.vm_object = vm_object_allocate((vm_size_t)(entry->vme_end - entry->vme_start));
			entry->offset = 0;

			vm_map_lock_write_to_read(map);
		}
		if (!(caller_flags & UPL_COPYOUT_FROM)) {
			if (!(entry->protection & VM_PROT_WRITE)) {
				vm_map_unlock_read(map);
				return KERN_PROTECTION_FAILURE;
			}
			if (entry->needs_copy)  {
				/*
				 * Honor copy-on-write for COPY_SYMMETRIC
				 * strategy.
				 */
				vm_map_t		local_map;
				vm_object_t		object;
				vm_object_offset_t	new_offset;
				vm_prot_t		prot;
				boolean_t		wired;
				vm_map_version_t	version;
				vm_map_t		real_map;

				local_map = map;

				if (vm_map_lookup_locked(&local_map,
							 offset, VM_PROT_WRITE,
							 OBJECT_LOCK_EXCLUSIVE,
							 &version, &object,
							 &new_offset, &prot, &wired,
							 NULL,
							 &real_map) != KERN_SUCCESS) {
				        vm_map_unlock_read(local_map);
					return KERN_FAILURE;
				}
				if (real_map != map)
					vm_map_unlock(real_map);
				vm_map_unlock_read(local_map);

				vm_object_unlock(object);

				goto REDISCOVER_ENTRY;
			}
		}
		if (entry->is_sub_map) {
			vm_map_t	submap;

			submap = entry->object.sub_map;
			local_start = entry->vme_start;
			local_offset = entry->offset;

			vm_map_reference(submap);
			vm_map_unlock_read(map);

			ret = vm_map_create_upl(submap, 
						local_offset + (offset - local_start), 
						upl_size, upl, page_list, count, flags);
			vm_map_deallocate(submap);

			return ret;
		}
		if (sync_cow_data) {
			if (entry->object.vm_object->shadow || entry->object.vm_object->copy) {
				local_object = entry->object.vm_object;
				local_start = entry->vme_start;
				local_offset = entry->offset;

				vm_object_reference(local_object);
				vm_map_unlock_read(map);

				if (local_object->shadow && local_object->copy) {
				        vm_object_lock_request(
							       local_object->shadow,
							       (vm_object_offset_t)
							       ((offset - local_start) +
								local_offset) +
							       local_object->shadow_offset,
							       *upl_size, FALSE, 
							       MEMORY_OBJECT_DATA_SYNC,
							       VM_PROT_NO_CHANGE);
				}
				sync_cow_data = FALSE;
				vm_object_deallocate(local_object);

				goto REDISCOVER_ENTRY;
			}
		}
		if (force_data_sync) {
			local_object = entry->object.vm_object;
			local_start = entry->vme_start;
			local_offset = entry->offset;

			vm_object_reference(local_object);
		        vm_map_unlock_read(map);

			vm_object_lock_request(
					       local_object,
					       (vm_object_offset_t)
					       ((offset - local_start) + local_offset),
					       (vm_object_size_t)*upl_size, FALSE, 
					       MEMORY_OBJECT_DATA_SYNC,
					       VM_PROT_NO_CHANGE);

			force_data_sync = FALSE;
			vm_object_deallocate(local_object);

			goto REDISCOVER_ENTRY;
		}
		if (entry->object.vm_object->private)
		        *flags = UPL_DEV_MEMORY;
		else
		        *flags = 0;

		if (entry->object.vm_object->phys_contiguous)
		        *flags |= UPL_PHYS_CONTIG;

		local_object = entry->object.vm_object;
		local_offset = entry->offset;
		local_start = entry->vme_start;

		vm_object_reference(local_object);
		vm_map_unlock_read(map);

		ret = vm_object_iopl_request(local_object, 
					      (vm_object_offset_t) ((offset - local_start) + local_offset),
					      *upl_size,
					      upl,
					      page_list,
					      count,
					      caller_flags);
		vm_object_deallocate(local_object);

		return(ret);
	} 
	vm_map_unlock_read(map);

	return(KERN_FAILURE);
}

/*
 * Internal routine to enter a UPL into a VM map.
 * 
 * JMM - This should just be doable through the standard
 * vm_map_enter() API.
 */
kern_return_t
vm_map_enter_upl(
	vm_map_t		map, 
	upl_t			upl, 
	vm_map_offset_t		*dst_addr)
{
	vm_map_size_t	 	size;
	vm_object_offset_t 	offset;
	vm_map_offset_t		addr;
	vm_page_t		m;
	kern_return_t		kr;
	int			isVectorUPL = 0, curr_upl=0;
	upl_t			vector_upl = NULL;
	vm_offset_t		vector_upl_dst_addr = 0;
	vm_map_t		vector_upl_submap = NULL;
	upl_offset_t 		subupl_offset = 0;
	upl_size_t		subupl_size = 0;

	if (upl == UPL_NULL)
		return KERN_INVALID_ARGUMENT;

	if((isVectorUPL = vector_upl_is_valid(upl))) {
		int mapped=0,valid_upls=0;
		vector_upl = upl;

		upl_lock(vector_upl);
		for(curr_upl=0; curr_upl < MAX_VECTOR_UPL_ELEMENTS; curr_upl++) {
			upl =  vector_upl_subupl_byindex(vector_upl, curr_upl );
			if(upl == NULL)
				continue;
			valid_upls++;
			if (UPL_PAGE_LIST_MAPPED & upl->flags)
				mapped++;
		}

		if(mapped) { 
			if(mapped != valid_upls)
				panic("Only %d of the %d sub-upls within the Vector UPL are alread mapped\n", mapped, valid_upls);
			else {
				upl_unlock(vector_upl);
				return KERN_FAILURE;
			}
		}

		kr = kmem_suballoc(map, &vector_upl_dst_addr, vector_upl->size, FALSE, VM_FLAGS_ANYWHERE, &vector_upl_submap);
		if( kr != KERN_SUCCESS )
			panic("Vector UPL submap allocation failed\n");
		map = vector_upl_submap;
		vector_upl_set_submap(vector_upl, vector_upl_submap, vector_upl_dst_addr);
		curr_upl=0;
	}
	else
		upl_lock(upl);

process_upl_to_enter:
	if(isVectorUPL){
		if(curr_upl == MAX_VECTOR_UPL_ELEMENTS) {
			*dst_addr = vector_upl_dst_addr;
			upl_unlock(vector_upl);
			return KERN_SUCCESS;
		}
		upl =  vector_upl_subupl_byindex(vector_upl, curr_upl++ );
		if(upl == NULL)
			goto process_upl_to_enter;
		vector_upl_get_iostate(vector_upl, upl, &subupl_offset, &subupl_size);
		*dst_addr = (vm_map_offset_t)(vector_upl_dst_addr + (vm_map_offset_t)subupl_offset);
	}

	/*
	 * check to see if already mapped
	 */
	if (UPL_PAGE_LIST_MAPPED & upl->flags) {
		upl_unlock(upl);
		return KERN_FAILURE;
	}

	if ((!(upl->flags & UPL_SHADOWED)) && !((upl->flags & (UPL_DEVICE_MEMORY | UPL_IO_WIRE)) ||
					       (upl->map_object->phys_contiguous))) {
		vm_object_t 		object;
		vm_page_t		alias_page;
		vm_object_offset_t	new_offset;
		unsigned int		pg_num;
		wpl_array_t 		lite_list;

		if (upl->flags & UPL_INTERNAL) {
			lite_list = (wpl_array_t) 
				((((uintptr_t)upl) + sizeof(struct upl))
				 + ((upl->size/PAGE_SIZE) * sizeof(upl_page_info_t)));
		} else {
		        lite_list = (wpl_array_t)(((uintptr_t)upl) + sizeof(struct upl));
		}
		object = upl->map_object;
		upl->map_object = vm_object_allocate(upl->size);

		vm_object_lock(upl->map_object);

		upl->map_object->shadow = object;
		upl->map_object->pageout = TRUE;
		upl->map_object->can_persist = FALSE;
		upl->map_object->copy_strategy = MEMORY_OBJECT_COPY_NONE;
		upl->map_object->shadow_offset = upl->offset - object->paging_offset;
		upl->map_object->wimg_bits = object->wimg_bits;
		offset = upl->map_object->shadow_offset;
		new_offset = 0;
		size = upl->size;

		upl->flags |= UPL_SHADOWED;

		while (size) {
			pg_num = (unsigned int) (new_offset / PAGE_SIZE);
			assert(pg_num == new_offset / PAGE_SIZE);

			if (lite_list[pg_num>>5] & (1 << (pg_num & 31))) {

				VM_PAGE_GRAB_FICTITIOUS(alias_page);

				vm_object_lock(object);

				m = vm_page_lookup(object, offset);
				if (m == VM_PAGE_NULL) {
				        panic("vm_upl_map: page missing\n");
				}

				/*
				 * Convert the fictitious page to a private 
				 * shadow of the real page.
				 */
				assert(alias_page->fictitious);
				alias_page->fictitious = FALSE;
				alias_page->private = TRUE;
				alias_page->pageout = TRUE;
				/*
				 * since m is a page in the upl it must
				 * already be wired or BUSY, so it's
				 * safe to assign the underlying physical
				 * page to the alias
				 */
				alias_page->phys_page = m->phys_page;

			        vm_object_unlock(object);

				vm_page_lockspin_queues();
				vm_page_wire(alias_page);
				vm_page_unlock_queues();
				
				/*
				 * ENCRYPTED SWAP:
				 * The virtual page ("m") has to be wired in some way
				 * here or its physical page ("m->phys_page") could
				 * be recycled at any time.
				 * Assuming this is enforced by the caller, we can't
				 * get an encrypted page here.  Since the encryption
				 * key depends on the VM page's "pager" object and
				 * the "paging_offset", we couldn't handle 2 pageable
				 * VM pages (with different pagers and paging_offsets)
				 * sharing the same physical page:  we could end up
				 * encrypting with one key (via one VM page) and
				 * decrypting with another key (via the alias VM page).
				 */
				ASSERT_PAGE_DECRYPTED(m);

				vm_page_insert(alias_page, upl->map_object, new_offset);

				assert(!alias_page->wanted);
				alias_page->busy = FALSE;
				alias_page->absent = FALSE;
			}
			size -= PAGE_SIZE;
			offset += PAGE_SIZE_64;
			new_offset += PAGE_SIZE_64;
		}
		vm_object_unlock(upl->map_object);
	}
	if ((upl->flags & (UPL_DEVICE_MEMORY | UPL_IO_WIRE)) || upl->map_object->phys_contiguous)
	        offset = upl->offset - upl->map_object->paging_offset;
	else
	        offset = 0;
	size = upl->size;
	
	vm_object_reference(upl->map_object);

	if(!isVectorUPL) {
		*dst_addr = 0;
		/*
	 	* NEED A UPL_MAP ALIAS
	 	*/
		kr = vm_map_enter(map, dst_addr, (vm_map_size_t)size, (vm_map_offset_t) 0,
				  VM_FLAGS_ANYWHERE, upl->map_object, offset, FALSE,
				  VM_PROT_DEFAULT, VM_PROT_ALL, VM_INHERIT_DEFAULT);
	}
	else {
		kr = vm_map_enter(map, dst_addr, (vm_map_size_t)size, (vm_map_offset_t) 0,
				  VM_FLAGS_FIXED, upl->map_object, offset, FALSE,
				  VM_PROT_DEFAULT, VM_PROT_ALL, VM_INHERIT_DEFAULT);
		if(kr)
			panic("vm_map_enter failed for a Vector UPL\n");
	}

	if (kr != KERN_SUCCESS) {
		upl_unlock(upl);
		return(kr);
	}
	vm_object_lock(upl->map_object);

	for (addr = *dst_addr; size > 0; size -= PAGE_SIZE, addr += PAGE_SIZE) {
		m = vm_page_lookup(upl->map_object, offset);

		if (m) {
		        unsigned int	cache_attr;
			cache_attr = ((unsigned int)m->object->wimg_bits) & VM_WIMG_MASK;

			m->pmapped = TRUE;

			/* CODE SIGNING ENFORCEMENT: page has been wpmapped, 
			 * but only in kernel space. If this was on a user map,
			 * we'd have to set the wpmapped bit. */
			/* m->wpmapped = TRUE; */
			assert(map==kernel_map);
	
			PMAP_ENTER(map->pmap, addr, m, VM_PROT_ALL, cache_attr, TRUE);
		}
		offset += PAGE_SIZE_64;
	}
	vm_object_unlock(upl->map_object);

	/*
	 * hold a reference for the mapping
	 */
	upl->ref_count++;
	upl->flags |= UPL_PAGE_LIST_MAPPED;
	upl->kaddr = (vm_offset_t) *dst_addr;
	assert(upl->kaddr == *dst_addr);
	
	if(!isVectorUPL)
		upl_unlock(upl);
	else
		goto process_upl_to_enter;

	return KERN_SUCCESS;
}
	
/*
 * Internal routine to remove a UPL mapping from a VM map.
 *
 * XXX - This should just be doable through a standard
 * vm_map_remove() operation.  Otherwise, implicit clean-up
 * of the target map won't be able to correctly remove
 * these (and release the reference on the UPL).  Having
 * to do this means we can't map these into user-space
 * maps yet.
 */
kern_return_t
vm_map_remove_upl(
	vm_map_t	map, 
	upl_t		upl)
{
	vm_address_t	addr;
	upl_size_t	size;
	int		isVectorUPL = 0, curr_upl = 0;
	upl_t		vector_upl = NULL;

	if (upl == UPL_NULL)
		return KERN_INVALID_ARGUMENT;

	if((isVectorUPL = vector_upl_is_valid(upl))) {
		int 	unmapped=0, valid_upls=0;
		vector_upl = upl;
		upl_lock(vector_upl);
		for(curr_upl=0; curr_upl < MAX_VECTOR_UPL_ELEMENTS; curr_upl++) {
			upl =  vector_upl_subupl_byindex(vector_upl, curr_upl );
			if(upl == NULL)
				continue;
			valid_upls++;
			if (!(UPL_PAGE_LIST_MAPPED & upl->flags))
				unmapped++;
		}

		if(unmapped) {
			if(unmapped != valid_upls)
				panic("%d of the %d sub-upls within the Vector UPL is/are not mapped\n", unmapped, valid_upls);
			else {
				upl_unlock(vector_upl);
				return KERN_FAILURE;
			}
		}
		curr_upl=0;
	}
	else
		upl_lock(upl);

process_upl_to_remove:
	if(isVectorUPL) {
		if(curr_upl == MAX_VECTOR_UPL_ELEMENTS) {
			vm_map_t v_upl_submap;
			vm_offset_t v_upl_submap_dst_addr;
			vector_upl_get_submap(vector_upl, &v_upl_submap, &v_upl_submap_dst_addr);

			vm_map_remove(map, v_upl_submap_dst_addr, v_upl_submap_dst_addr + vector_upl->size, VM_MAP_NO_FLAGS);
			vm_map_deallocate(v_upl_submap);
			upl_unlock(vector_upl);
			return KERN_SUCCESS;
		}

		upl =  vector_upl_subupl_byindex(vector_upl, curr_upl++ );
		if(upl == NULL)
			goto process_upl_to_remove;	
	}

	if (upl->flags & UPL_PAGE_LIST_MAPPED) {
		addr = upl->kaddr;
		size = upl->size;

		assert(upl->ref_count > 1);
		upl->ref_count--;		/* removing mapping ref */

		upl->flags &= ~UPL_PAGE_LIST_MAPPED;
		upl->kaddr = (vm_offset_t) 0;
		
		if(!isVectorUPL) {
			upl_unlock(upl);
		
			vm_map_remove(map,
				vm_map_trunc_page(addr),
				vm_map_round_page(addr + size),
				VM_MAP_NO_FLAGS);
		
			return KERN_SUCCESS;
		}
		else {
			/*
			* If it's a Vectored UPL, we'll be removing the entire
			* submap anyways, so no need to remove individual UPL
			* element mappings from within the submap
			*/	
			goto process_upl_to_remove;
		}
	}
	upl_unlock(upl);

	return KERN_FAILURE;
}

static void
dw_do_work(
	vm_object_t 	object,
	struct dw 	*dwp,
	int		dw_count)
{
	int		j;
	boolean_t	held_as_spin = TRUE;

	/*
	 * pageout_scan takes the vm_page_lock_queues first
	 * then tries for the object lock... to avoid what
	 * is effectively a lock inversion, we'll go to the
	 * trouble of taking them in that same order... otherwise
	 * if this object contains the majority of the pages resident
	 * in the UBC (or a small set of large objects actively being
	 * worked on contain the majority of the pages), we could
	 * cause the pageout_scan thread to 'starve' in its attempt
	 * to find pages to move to the free queue, since it has to
	 * successfully acquire the object lock of any candidate page
	 * before it can steal/clean it.
	 */
	if (!vm_page_trylockspin_queues()) {
		vm_object_unlock(object);

		vm_page_lockspin_queues();

		for (j = 0; ; j++) {
			if (!vm_object_lock_avoid(object) &&
			    _vm_object_lock_try(object))
				break;
			vm_page_unlock_queues();
			mutex_pause(j);
			vm_page_lockspin_queues();
		}
	}
	for (j = 0; j < dw_count; j++, dwp++) {

		if (dwp->dw_mask & DW_vm_pageout_throttle_up)
			vm_pageout_throttle_up(dwp->dw_m);

		if (dwp->dw_mask & DW_vm_page_wire)
			vm_page_wire(dwp->dw_m);
		else if (dwp->dw_mask & DW_vm_page_unwire)
			vm_page_unwire(dwp->dw_m);

		if (dwp->dw_mask & DW_vm_page_free) {
			if (held_as_spin == TRUE) {
				vm_page_lockconvert_queues();
				held_as_spin = FALSE;
			}
			vm_page_free(dwp->dw_m);
		} else {
			if (dwp->dw_mask & DW_vm_page_deactivate_internal)
				vm_page_deactivate_internal(dwp->dw_m, FALSE);
			else if (dwp->dw_mask & DW_vm_page_activate)
				vm_page_activate(dwp->dw_m);
			else if (dwp->dw_mask & DW_vm_page_speculate)
				vm_page_speculate(dwp->dw_m, TRUE);
			else if (dwp->dw_mask & DW_vm_page_lru)
				vm_page_lru(dwp->dw_m);
			
			if (dwp->dw_mask & DW_set_reference)
				dwp->dw_m->reference = TRUE;
			else if (dwp->dw_mask & DW_clear_reference)
				dwp->dw_m->reference = FALSE;

			if (dwp->dw_mask & DW_clear_busy)
				dwp->dw_m->busy = FALSE;

			if (dwp->dw_mask & DW_PAGE_WAKEUP)
				PAGE_WAKEUP(dwp->dw_m);
		}
	}
	vm_page_unlock_queues();
}



kern_return_t
upl_commit_range(
	upl_t			upl, 
	upl_offset_t		offset, 
	upl_size_t		size,
	int			flags,
	upl_page_info_t		*page_list,
	mach_msg_type_number_t	count,
	boolean_t		*empty) 
{
	upl_size_t		xfer_size, subupl_size = size;
	vm_object_t		shadow_object;
	vm_object_t		object;
	vm_object_offset_t	target_offset;
	upl_offset_t		subupl_offset = offset;
	int			entry;
	wpl_array_t 		lite_list;
	int			occupied;
	int			clear_refmod = 0;
	int			pgpgout_count = 0;
	struct	dw		dw_array[DELAYED_WORK_LIMIT];
	struct	dw		*dwp;
	int			dw_count, isVectorUPL = 0;
	upl_t			vector_upl = NULL;

	*empty = FALSE;

	if (upl == UPL_NULL)
		return KERN_INVALID_ARGUMENT;

	if (count == 0)
		page_list = NULL;

	if((isVectorUPL = vector_upl_is_valid(upl))) {
		vector_upl = upl;
		upl_lock(vector_upl);
	}
	else
		upl_lock(upl);

process_upl_to_commit:

	if(isVectorUPL) {
		size = subupl_size;
		offset = subupl_offset;
		if(size == 0) {
			upl_unlock(vector_upl);
			return KERN_SUCCESS;
		}
		upl =  vector_upl_subupl_byoffset(vector_upl, &offset, &size);
		if(upl == NULL) {
			upl_unlock(vector_upl);
			return KERN_FAILURE;
		}
		page_list = UPL_GET_INTERNAL_PAGE_LIST_SIMPLE(upl);
		subupl_size -= size;
		subupl_offset += size;
	}

#if UPL_DEBUG
	if (upl->upl_commit_index < UPL_DEBUG_COMMIT_RECORDS) {
		(void) OSBacktrace(&upl->upl_commit_records[upl->upl_commit_index].c_retaddr[0], UPL_DEBUG_STACK_FRAMES);
		
		upl->upl_commit_records[upl->upl_commit_index].c_beg = offset;
		upl->upl_commit_records[upl->upl_commit_index].c_end = (offset + size);

		upl->upl_commit_index++;
	}
#endif
	if (upl->flags & UPL_DEVICE_MEMORY)
		xfer_size = 0;
	else if ((offset + size) <= upl->size)
	        xfer_size = size;
	else {
		if(!isVectorUPL)
			upl_unlock(upl);
		else {
			upl_unlock(vector_upl);
		}
		return KERN_FAILURE;
	}
	if (upl->flags & UPL_CLEAR_DIRTY)
	        flags |= UPL_COMMIT_CLEAR_DIRTY;

	if (upl->flags & UPL_INTERNAL)
		lite_list = (wpl_array_t) ((((uintptr_t)upl) + sizeof(struct upl))
					   + ((upl->size/PAGE_SIZE) * sizeof(upl_page_info_t)));
	else
		lite_list = (wpl_array_t) (((uintptr_t)upl) + sizeof(struct upl));

	object = upl->map_object;

	if (upl->flags & UPL_SHADOWED) {
	        vm_object_lock(object);
		shadow_object = object->shadow;
	} else {
		shadow_object = object;
	}
	entry = offset/PAGE_SIZE;
	target_offset = (vm_object_offset_t)offset;

	if (upl->flags & UPL_KERNEL_OBJECT)
		vm_object_lock_shared(shadow_object);
	else
		vm_object_lock(shadow_object);

	if (upl->flags & UPL_ACCESS_BLOCKED) {
		assert(shadow_object->blocked_access);
		shadow_object->blocked_access = FALSE;
		vm_object_wakeup(object, VM_OBJECT_EVENT_UNBLOCKED);
	}

	if (shadow_object->code_signed) {
		/*
		 * CODE SIGNING:
		 * If the object is code-signed, do not let this UPL tell
		 * us if the pages are valid or not.  Let the pages be
		 * validated by VM the normal way (when they get mapped or
		 * copied).
		 */
		flags &= ~UPL_COMMIT_CS_VALIDATED;
	}
	if (! page_list) {
		/*
		 * No page list to get the code-signing info from !?
		 */
		flags &= ~UPL_COMMIT_CS_VALIDATED;
	}

	dwp = &dw_array[0];
	dw_count = 0;

	while (xfer_size) {
		vm_page_t	t, m;

		dwp->dw_mask = 0;
		clear_refmod = 0;

		m = VM_PAGE_NULL;

		if (upl->flags & UPL_LITE) {
			unsigned int	pg_num;

			pg_num = (unsigned int) (target_offset/PAGE_SIZE);
			assert(pg_num == target_offset/PAGE_SIZE);

			if (lite_list[pg_num>>5] & (1 << (pg_num & 31))) {
			        lite_list[pg_num>>5] &= ~(1 << (pg_num & 31));

				if (!(upl->flags & UPL_KERNEL_OBJECT))
					m = vm_page_lookup(shadow_object, target_offset + (upl->offset - shadow_object->paging_offset));
			}
		}
		if (upl->flags & UPL_SHADOWED) {
			if ((t = vm_page_lookup(object, target_offset))	!= VM_PAGE_NULL) {

				t->pageout = FALSE;

				VM_PAGE_FREE(t);

				if (m == VM_PAGE_NULL)
					m = vm_page_lookup(shadow_object, target_offset + object->shadow_offset);
			}
		}
		if ((upl->flags & UPL_KERNEL_OBJECT) || m == VM_PAGE_NULL)
			goto commit_next_page;

		if (flags & UPL_COMMIT_CS_VALIDATED) {
			/*
			 * CODE SIGNING:
			 * Set the code signing bits according to
			 * what the UPL says they should be.
			 */
			m->cs_validated = page_list[entry].cs_validated;
			m->cs_tainted = page_list[entry].cs_tainted;
		}
		if (upl->flags & UPL_IO_WIRE) {

			dwp->dw_mask |= DW_vm_page_unwire;

			if (page_list)
				page_list[entry].phys_addr = 0;

			if (flags & UPL_COMMIT_SET_DIRTY)
				m->dirty = TRUE;
			else if (flags & UPL_COMMIT_CLEAR_DIRTY) {
				m->dirty = FALSE;

				if (! (flags & UPL_COMMIT_CS_VALIDATED) &&
				    m->cs_validated && !m->cs_tainted) {
					/*
					 * CODE SIGNING:
					 * This page is no longer dirty
					 * but could have been modified,
					 * so it will need to be
					 * re-validated.
					 */
					m->cs_validated = FALSE;
#if DEVELOPMENT || DEBUG
					vm_cs_validated_resets++;
#endif
					pmap_disconnect(m->phys_page);
				}
				clear_refmod |= VM_MEM_MODIFIED;
			}
			if (flags & UPL_COMMIT_INACTIVATE) {
				dwp->dw_mask |= DW_vm_page_deactivate_internal;
				clear_refmod |= VM_MEM_REFERENCED;
			}
			if (upl->flags & UPL_ACCESS_BLOCKED) {
				/*
				 * We blocked access to the pages in this UPL.
				 * Clear the "busy" bit and wake up any waiter
				 * for this page.
				 */
				dwp->dw_mask |= (DW_clear_busy | DW_PAGE_WAKEUP);
			}
			goto commit_next_page;
		}
		/*
		 * make sure to clear the hardware
		 * modify or reference bits before
		 * releasing the BUSY bit on this page
		 * otherwise we risk losing a legitimate
		 * change of state
		 */
		if (flags & UPL_COMMIT_CLEAR_DIRTY) {
			m->dirty = FALSE;

			if (! (flags & UPL_COMMIT_CS_VALIDATED) &&
			    m->cs_validated && !m->cs_tainted) {
				/*
				 * CODE SIGNING:
				 * This page is no longer dirty
				 * but could have been modified,
				 * so it will need to be
				 * re-validated.
				 */
				m->cs_validated = FALSE;
#if DEVELOPMENT || DEBUG
				vm_cs_validated_resets++;
#endif
				pmap_disconnect(m->phys_page);
			}
			clear_refmod |= VM_MEM_MODIFIED;
		}
		if (page_list) {
			upl_page_info_t *p;

			p = &(page_list[entry]);

			if (p->phys_addr && p->pageout && !m->pageout) {
				m->busy = TRUE;
				m->pageout = TRUE;

				dwp->dw_mask |= DW_vm_page_wire;

			} else if (p->phys_addr &&
				   !p->pageout && m->pageout &&
				   !m->dump_cleaning) {
				m->pageout = FALSE;
				m->absent = FALSE;
				m->overwriting = FALSE;

				dwp->dw_mask |= (DW_vm_page_unwire | DW_clear_busy | DW_PAGE_WAKEUP);
			}
			page_list[entry].phys_addr = 0;
		}
		m->dump_cleaning = FALSE;

		if (m->laundry)
			dwp->dw_mask |= DW_vm_pageout_throttle_up;

		if (m->pageout) {
			m->cleaning = FALSE;
			m->encrypted_cleaning = FALSE;
			m->pageout = FALSE;
#if MACH_CLUSTER_STATS
			if (m->wanted) vm_pageout_target_collisions++;
#endif
			m->dirty = FALSE;

			if (! (flags & UPL_COMMIT_CS_VALIDATED) &&
			    m->cs_validated && !m->cs_tainted) {
				/*
				 * CODE SIGNING:
				 * This page is no longer dirty
				 * but could have been modified,
				 * so it will need to be
				 * re-validated.
				 */
				m->cs_validated = FALSE;
#if DEVELOPMENT || DEBUG
				vm_cs_validated_resets++;
#endif
				pmap_disconnect(m->phys_page);
			}

			if ((flags & UPL_COMMIT_SET_DIRTY) ||
			    (m->pmapped && (pmap_disconnect(m->phys_page) & VM_MEM_MODIFIED)))
				m->dirty = TRUE;

			if (m->dirty) {
				/*
				 * page was re-dirtied after we started
				 * the pageout... reactivate it since 
				 * we don't know whether the on-disk
				 * copy matches what is now in memory
				 */
				dwp->dw_mask |= (DW_vm_page_unwire | DW_clear_busy | DW_PAGE_WAKEUP);

				if (upl->flags & UPL_PAGEOUT) {
					CLUSTER_STAT(vm_pageout_target_page_dirtied++;)
					VM_STAT_INCR(reactivations);
					DTRACE_VM2(pgrec, int, 1, (uint64_t *), NULL);
				}
			} else {
				/*
				 * page has been successfully cleaned
				 * go ahead and free it for other use
				 */

				if (m->object->internal) {
					DTRACE_VM2(anonpgout, int, 1, (uint64_t *), NULL);
				} else {
					DTRACE_VM2(fspgout, int, 1, (uint64_t *), NULL);
				}
				dwp->dw_mask |= DW_vm_page_free;
 
				if (upl->flags & UPL_PAGEOUT) {
					CLUSTER_STAT(vm_pageout_target_page_freed++;)

					if (page_list[entry].dirty) {
						VM_STAT_INCR(pageouts);
						DTRACE_VM2(pgout, int, 1, (uint64_t *), NULL);
						pgpgout_count++;
					}
				}
			}
			goto commit_next_page;
		}
#if MACH_CLUSTER_STATS
		if (m->wpmapped)
			m->dirty = pmap_is_modified(m->phys_page);

		if (m->dirty)   vm_pageout_cluster_dirtied++;
		else            vm_pageout_cluster_cleaned++;
		if (m->wanted)  vm_pageout_cluster_collisions++;
#endif
		m->dirty = FALSE;

		if (! (flags & UPL_COMMIT_CS_VALIDATED) &&
		    m->cs_validated && !m->cs_tainted) {
			/*
			 * CODE SIGNING:
			 * This page is no longer dirty
			 * but could have been modified,
			 * so it will need to be
			 * re-validated.
			 */
			m->cs_validated = FALSE;
#if DEVELOPMENT || DEBUG
			vm_cs_validated_resets++;
#endif
			pmap_disconnect(m->phys_page);
		}

		if ((m->busy) && (m->cleaning)) {
			/*
			 * the request_page_list case
			 */
			m->absent = FALSE;
			m->overwriting = FALSE;

			dwp->dw_mask |= DW_clear_busy;

		} else if (m->overwriting) {
			/*
			 * alternate request page list, write to 
			 * page_list case.  Occurs when the original
			 * page was wired at the time of the list
			 * request
			 */
			assert(VM_PAGE_WIRED(m));
			m->overwriting = FALSE;

			dwp->dw_mask |= DW_vm_page_unwire; /* reactivates */
		}
		m->cleaning = FALSE;
		m->encrypted_cleaning = FALSE;

		/*
		 * It is a part of the semantic of COPYOUT_FROM
		 * UPLs that a commit implies cache sync
		 * between the vm page and the backing store
		 * this can be used to strip the precious bit
		 * as well as clean
		 */
		if ((upl->flags & UPL_PAGE_SYNC_DONE) || (flags & UPL_COMMIT_CLEAR_PRECIOUS))
			m->precious = FALSE;

		if (flags & UPL_COMMIT_SET_DIRTY)
			m->dirty = TRUE;

		if ((flags & UPL_COMMIT_INACTIVATE) && !m->clustered && !m->speculative) {
			dwp->dw_mask |= DW_vm_page_deactivate_internal;
			clear_refmod |= VM_MEM_REFERENCED;

		} else if (!m->active && !m->inactive && !m->speculative) {

			if (m->clustered || (flags & UPL_COMMIT_SPECULATE))
				dwp->dw_mask |= DW_vm_page_speculate;
			else if (m->reference)
				dwp->dw_mask |= DW_vm_page_activate;
			else {
				dwp->dw_mask |= DW_vm_page_deactivate_internal;
				clear_refmod |= VM_MEM_REFERENCED;
			}
		}
		if (upl->flags & UPL_ACCESS_BLOCKED) {
			/*
			 * We blocked access to the pages in this URL.
			 * Clear the "busy" bit on this page before we
			 * wake up any waiter.
			 */
			dwp->dw_mask |= DW_clear_busy;
		}
		/*
		 * Wakeup any thread waiting for the page to be un-cleaning.
		 */
		dwp->dw_mask |= DW_PAGE_WAKEUP;

commit_next_page:
		if (clear_refmod)
			pmap_clear_refmod(m->phys_page, clear_refmod);

		target_offset += PAGE_SIZE_64;
		xfer_size -= PAGE_SIZE;
		entry++;

		if (dwp->dw_mask) {
			if (dwp->dw_mask & ~(DW_clear_busy | DW_PAGE_WAKEUP)) {
				if (m->busy == FALSE) {
					/*
					 * dw_do_work may need to drop the object lock
					 * if it does, we need the pages it's looking at to
					 * be held stable via the busy bit.
					 */
					m->busy = TRUE;
					dwp->dw_mask |= (DW_clear_busy | DW_PAGE_WAKEUP);
				}
				dwp->dw_m = m;
				dwp++;
				dw_count++;

				if (dw_count >= DELAYED_WORK_LIMIT) {
					dw_do_work(shadow_object, &dw_array[0], dw_count);
			
					dwp = &dw_array[0];
					dw_count = 0;
				}
			} else {
				if (dwp->dw_mask & DW_clear_busy)
					m->busy = FALSE;

				if (dwp->dw_mask & DW_PAGE_WAKEUP)
					PAGE_WAKEUP(m);
			}
		}
	}
	if (dw_count)
		dw_do_work(shadow_object, &dw_array[0], dw_count);

	occupied = 1;

	if (upl->flags & UPL_DEVICE_MEMORY)  {
		occupied = 0;
	} else if (upl->flags & UPL_LITE) {
		int	pg_num;
		int	i;

		pg_num = upl->size/PAGE_SIZE;
		pg_num = (pg_num + 31) >> 5;
		occupied = 0;

		for (i = 0; i < pg_num; i++) {
			if (lite_list[i] != 0) {
				occupied = 1;
				break;
			}
		}
	} else {
		if (queue_empty(&upl->map_object->memq))
			occupied = 0;
	}
	if (occupied == 0) {
		/*
		 * If this UPL element belongs to a Vector UPL and is
		 * empty, then this is the right function to deallocate
		 * it. So go ahead set the *empty variable. The flag
		 * UPL_COMMIT_NOTIFY_EMPTY, from the caller's point of view
		 * should be considered relevant for the Vector UPL and not
		 * the internal UPLs.
		 */
		if ((upl->flags & UPL_COMMIT_NOTIFY_EMPTY) || isVectorUPL)
			*empty = TRUE;

		if (object == shadow_object && !(upl->flags & UPL_KERNEL_OBJECT)) {
		        /*
			 * this is not a paging object
			 * so we need to drop the paging reference
			 * that was taken when we created the UPL
			 * against this object
			 */
			vm_object_activity_end(shadow_object);
		} else {
		         /*
			  * we dontated the paging reference to
			  * the map object... vm_pageout_object_terminate
			  * will drop this reference
			  */
		}
	}
	vm_object_unlock(shadow_object);
	if (object != shadow_object)
	        vm_object_unlock(object);
	
	if(!isVectorUPL)
		upl_unlock(upl);
	else {
		/* 
		 * If we completed our operations on an UPL that is
		 * part of a Vectored UPL and if empty is TRUE, then
		 * we should go ahead and deallocate this UPL element. 
		 * Then we check if this was the last of the UPL elements
		 * within that Vectored UPL. If so, set empty to TRUE
		 * so that in ubc_upl_commit_range or ubc_upl_commit, we
		 * can go ahead and deallocate the Vector UPL too.
		 */
		if(*empty==TRUE) {
			*empty = vector_upl_set_subupl(vector_upl, upl, 0);
			upl_deallocate(upl);
		}
		goto process_upl_to_commit;
	}

	if (pgpgout_count) {
		DTRACE_VM2(pgpgout, int, pgpgout_count, (uint64_t *), NULL);
	}

	return KERN_SUCCESS;
}

kern_return_t
upl_abort_range(
	upl_t			upl, 
	upl_offset_t		offset, 
	upl_size_t		size,
	int			error,
	boolean_t		*empty) 
{
	upl_size_t		xfer_size, subupl_size = size;
	vm_object_t		shadow_object;
	vm_object_t		object;
	vm_object_offset_t	target_offset;
	upl_offset_t		subupl_offset = offset;
	int			entry;
	wpl_array_t 	 	lite_list;
	int			occupied;
	struct	dw		dw_array[DELAYED_WORK_LIMIT];
	struct	dw		*dwp;
	int			dw_count, isVectorUPL = 0;
	upl_t			vector_upl = NULL;

	*empty = FALSE;

	if (upl == UPL_NULL)
		return KERN_INVALID_ARGUMENT;

	if ( (upl->flags & UPL_IO_WIRE) && !(error & UPL_ABORT_DUMP_PAGES) )
		return upl_commit_range(upl, offset, size, 0, NULL, 0, empty);

	if((isVectorUPL = vector_upl_is_valid(upl))) {
		vector_upl = upl;
		upl_lock(vector_upl);
	}
	else
		upl_lock(upl);

process_upl_to_abort:
	if(isVectorUPL) {
		size = subupl_size;
		offset = subupl_offset;
		if(size == 0) {
			upl_unlock(vector_upl);
			return KERN_SUCCESS;
		}
		upl =  vector_upl_subupl_byoffset(vector_upl, &offset, &size);
		if(upl == NULL) {
			upl_unlock(vector_upl);
			return KERN_FAILURE;
		}
		subupl_size -= size;
		subupl_offset += size;
	}

	*empty = FALSE;

#if UPL_DEBUG
	if (upl->upl_commit_index < UPL_DEBUG_COMMIT_RECORDS) {
		(void) OSBacktrace(&upl->upl_commit_records[upl->upl_commit_index].c_retaddr[0], UPL_DEBUG_STACK_FRAMES);
		
		upl->upl_commit_records[upl->upl_commit_index].c_beg = offset;
		upl->upl_commit_records[upl->upl_commit_index].c_end = (offset + size);
		upl->upl_commit_records[upl->upl_commit_index].c_aborted = 1;

		upl->upl_commit_index++;
	}
#endif
	if (upl->flags & UPL_DEVICE_MEMORY)
		xfer_size = 0;
	else if ((offset + size) <= upl->size)
	        xfer_size = size;
	else {
		if(!isVectorUPL)
			upl_unlock(upl);
		else {
			upl_unlock(vector_upl);
		}

		return KERN_FAILURE;
	}
	if (upl->flags & UPL_INTERNAL) {
		lite_list = (wpl_array_t) 
			((((uintptr_t)upl) + sizeof(struct upl))
			+ ((upl->size/PAGE_SIZE) * sizeof(upl_page_info_t)));
	} else {
		lite_list = (wpl_array_t) 
			(((uintptr_t)upl) + sizeof(struct upl));
	}
	object = upl->map_object;

	if (upl->flags & UPL_SHADOWED) {
	        vm_object_lock(object);
		shadow_object = object->shadow;
	} else
		shadow_object = object;

	entry = offset/PAGE_SIZE;
	target_offset = (vm_object_offset_t)offset;

	if (upl->flags & UPL_KERNEL_OBJECT)
		vm_object_lock_shared(shadow_object);
	else
		vm_object_lock(shadow_object);

	if (upl->flags & UPL_ACCESS_BLOCKED) {
		assert(shadow_object->blocked_access);
		shadow_object->blocked_access = FALSE;
		vm_object_wakeup(object, VM_OBJECT_EVENT_UNBLOCKED);
	}

	dwp = &dw_array[0];
	dw_count = 0;

	if ((error & UPL_ABORT_DUMP_PAGES) && (upl->flags & UPL_KERNEL_OBJECT))
		panic("upl_abort_range: kernel_object being DUMPED");

	while (xfer_size) {
		vm_page_t	t, m;

		dwp->dw_mask = 0;

		m = VM_PAGE_NULL;

		if (upl->flags & UPL_LITE) {
			unsigned int	pg_num;

			pg_num = (unsigned int) (target_offset/PAGE_SIZE);
			assert(pg_num == target_offset/PAGE_SIZE);
			

			if (lite_list[pg_num>>5] & (1 << (pg_num & 31))) {
				lite_list[pg_num>>5] &= ~(1 << (pg_num & 31));

				if ( !(upl->flags & UPL_KERNEL_OBJECT))
					m = vm_page_lookup(shadow_object, target_offset +
							   (upl->offset - shadow_object->paging_offset));
			}
		}
		if (upl->flags & UPL_SHADOWED) {
		        if ((t = vm_page_lookup(object, target_offset))	!= VM_PAGE_NULL) {
			        t->pageout = FALSE;

				VM_PAGE_FREE(t);

				if (m == VM_PAGE_NULL)
					m = vm_page_lookup(shadow_object, target_offset + object->shadow_offset);
			}
		}
		if ((upl->flags & UPL_KERNEL_OBJECT))
			goto abort_next_page;

		if (m != VM_PAGE_NULL) {

			if (m->absent) {
			        boolean_t must_free = TRUE;

				m->clustered = FALSE;
				/*
				 * COPYOUT = FALSE case
				 * check for error conditions which must
				 * be passed back to the pages customer
				 */
				if (error & UPL_ABORT_RESTART) {
					m->restart = TRUE;
					m->absent = FALSE;
					m->unusual = TRUE;
					must_free = FALSE;
				} else if (error & UPL_ABORT_UNAVAILABLE) {
					m->restart = FALSE;
					m->unusual = TRUE;
					must_free = FALSE;
				} else if (error & UPL_ABORT_ERROR) {
					m->restart = FALSE;
					m->absent = FALSE;
					m->error = TRUE;
					m->unusual = TRUE;
					must_free = FALSE;
				}

				/*
				 * ENCRYPTED SWAP:
				 * If the page was already encrypted,
				 * we don't really need to decrypt it
				 * now.  It will get decrypted later,
				 * on demand, as soon as someone needs
				 * to access its contents.
				 */

				m->cleaning = FALSE;
				m->encrypted_cleaning = FALSE;
				m->overwriting = FALSE;

				dwp->dw_mask |= (DW_clear_busy | DW_PAGE_WAKEUP);

				if (must_free == TRUE)
					dwp->dw_mask |= DW_vm_page_free;
				else
					dwp->dw_mask |= DW_vm_page_activate;
			} else {
			        /*                          
				 * Handle the trusted pager throttle.
				 */                     
			        if (m->laundry)
					dwp->dw_mask |= DW_vm_pageout_throttle_up;

				if (m->pageout) {
				        assert(m->busy);
					assert(m->wire_count == 1);
					m->pageout = FALSE;

					dwp->dw_mask |= DW_vm_page_unwire;
				}
				m->dump_cleaning = FALSE;
				m->cleaning = FALSE;
				m->encrypted_cleaning = FALSE;
				m->overwriting = FALSE;
#if	MACH_PAGEMAP
				vm_external_state_clr(m->object->existence_map, m->offset);
#endif	/* MACH_PAGEMAP */
				if (error & UPL_ABORT_DUMP_PAGES) {
					pmap_disconnect(m->phys_page);

					dwp->dw_mask |= DW_vm_page_free;
				} else {
				        if (error & UPL_ABORT_REFERENCE) {
						/*
						 * we've been told to explictly
						 * reference this page... for 
						 * file I/O, this is done by
						 * implementing an LRU on the inactive q
						 */
						dwp->dw_mask |= DW_vm_page_lru;
					}
					dwp->dw_mask |= (DW_clear_busy | DW_PAGE_WAKEUP);
				}
			}
		}
abort_next_page:
		target_offset += PAGE_SIZE_64;
		xfer_size -= PAGE_SIZE;
		entry++;

		if (dwp->dw_mask) {
			if (dwp->dw_mask & ~(DW_clear_busy | DW_PAGE_WAKEUP)) {
				if (m->busy == FALSE) {
					/*
					 * dw_do_work may need to drop the object lock
					 * if it does, we need the pages it's looking at to
					 * be held stable via the busy bit.
					 */
					m->busy = TRUE;
					dwp->dw_mask |= (DW_clear_busy | DW_PAGE_WAKEUP);
				}
				dwp->dw_m = m;
				dwp++;
				dw_count++;

				if (dw_count >= DELAYED_WORK_LIMIT) {
					dw_do_work(shadow_object, &dw_array[0], dw_count);
				
					dwp = &dw_array[0];
					dw_count = 0;
				}
			} else {
				if (dwp->dw_mask & DW_clear_busy)
					m->busy = FALSE;

				if (dwp->dw_mask & DW_PAGE_WAKEUP)
					PAGE_WAKEUP(m);
			}
		}
	}
	if (dw_count)
		dw_do_work(shadow_object, &dw_array[0], dw_count);

	occupied = 1;

	if (upl->flags & UPL_DEVICE_MEMORY)  {
		occupied = 0;
	} else if (upl->flags & UPL_LITE) {
		int	pg_num;
		int	i;

		pg_num = upl->size/PAGE_SIZE;
		pg_num = (pg_num + 31) >> 5;
		occupied = 0;

		for (i = 0; i < pg_num; i++) {
			if (lite_list[i] != 0) {
				occupied = 1;
				break;
			}
		}
	} else {
		if (queue_empty(&upl->map_object->memq))
			occupied = 0;
	}
	if (occupied == 0) {
		/*
		 * If this UPL element belongs to a Vector UPL and is
		 * empty, then this is the right function to deallocate
		 * it. So go ahead set the *empty variable. The flag
		 * UPL_COMMIT_NOTIFY_EMPTY, from the caller's point of view
		 * should be considered relevant for the Vector UPL and
		 * not the internal UPLs.
		 */
		if ((upl->flags & UPL_COMMIT_NOTIFY_EMPTY) || isVectorUPL)
			*empty = TRUE;

		if (object == shadow_object && !(upl->flags & UPL_KERNEL_OBJECT)) {
		        /*
			 * this is not a paging object
			 * so we need to drop the paging reference
			 * that was taken when we created the UPL
			 * against this object
			 */
			vm_object_activity_end(shadow_object);
		} else {
		         /*
			  * we dontated the paging reference to
			  * the map object... vm_pageout_object_terminate
			  * will drop this reference
			  */
		}
	}
	vm_object_unlock(shadow_object);
	if (object != shadow_object)
	        vm_object_unlock(object);
	
	if(!isVectorUPL)
		upl_unlock(upl);
	else {
		/* 
		* If we completed our operations on an UPL that is
	 	* part of a Vectored UPL and if empty is TRUE, then
	 	* we should go ahead and deallocate this UPL element. 
	 	* Then we check if this was the last of the UPL elements
	 	* within that Vectored UPL. If so, set empty to TRUE
	 	* so that in ubc_upl_abort_range or ubc_upl_abort, we
	 	* can go ahead and deallocate the Vector UPL too.
	 	*/
		if(*empty == TRUE) {
			*empty = vector_upl_set_subupl(vector_upl, upl,0);
			upl_deallocate(upl);
		}
		goto process_upl_to_abort;
	}

	return KERN_SUCCESS;
}


kern_return_t
upl_abort(
	upl_t	upl,
	int	error)
{
	boolean_t	empty;

	return upl_abort_range(upl, 0, upl->size, error, &empty);
}


/* an option on commit should be wire */
kern_return_t
upl_commit(
	upl_t			upl,
	upl_page_info_t		*page_list,
	mach_msg_type_number_t	count)
{
	boolean_t	empty;

	return upl_commit_range(upl, 0, upl->size, 0, page_list, count, &empty);
}


unsigned int vm_object_iopl_request_sleep_for_cleaning = 0;

kern_return_t
vm_object_iopl_request(
	vm_object_t		object,
	vm_object_offset_t	offset,
	upl_size_t		size,
	upl_t			*upl_ptr,
	upl_page_info_array_t	user_page_list,
	unsigned int		*page_list_count,
	int			cntrl_flags)
{
	vm_page_t		dst_page;
	vm_object_offset_t	dst_offset;
	upl_size_t		xfer_size;
	upl_t			upl = NULL;
	unsigned int		entry;
	wpl_array_t 		lite_list = NULL;
	int			no_zero_fill = FALSE;
	u_int32_t		psize;
	kern_return_t		ret;
	vm_prot_t		prot;
	struct vm_object_fault_info fault_info;
	struct	dw		dw_array[DELAYED_WORK_LIMIT];
	struct	dw		*dwp;
	int			dw_count;
	int			dw_index;

	if (cntrl_flags & ~UPL_VALID_FLAGS) {
		/*
		 * For forward compatibility's sake,
		 * reject any unknown flag.
		 */
		return KERN_INVALID_VALUE;
	}
	if (vm_lopage_poolsize == 0)
	        cntrl_flags &= ~UPL_NEED_32BIT_ADDR;

	if (cntrl_flags & UPL_NEED_32BIT_ADDR) {
	        if ( (cntrl_flags & (UPL_SET_IO_WIRE | UPL_SET_LITE)) != (UPL_SET_IO_WIRE | UPL_SET_LITE))
		        return KERN_INVALID_VALUE;

		if (object->phys_contiguous) {
		        if ((offset + object->shadow_offset) >= (vm_object_offset_t)max_valid_dma_address)
			        return KERN_INVALID_ADDRESS;
	      
			if (((offset + object->shadow_offset) + size) >= (vm_object_offset_t)max_valid_dma_address)
			        return KERN_INVALID_ADDRESS;
		}
	}

	if (cntrl_flags & UPL_ENCRYPT) {
		/*
		 * ENCRYPTED SWAP:
		 * The paging path doesn't use this interface,
		 * so we don't support the UPL_ENCRYPT flag
		 * here.  We won't encrypt the pages.
		 */
		assert(! (cntrl_flags & UPL_ENCRYPT));
	}
	if (cntrl_flags & UPL_NOZEROFILL)
	        no_zero_fill = TRUE;

	if (cntrl_flags & UPL_COPYOUT_FROM)
		prot = VM_PROT_READ;
	else
		prot = VM_PROT_READ | VM_PROT_WRITE;

	if (((size/PAGE_SIZE) > MAX_UPL_SIZE) && !object->phys_contiguous)
		size = MAX_UPL_SIZE * PAGE_SIZE;

	if (cntrl_flags & UPL_SET_INTERNAL) {
		if (page_list_count != NULL)
			*page_list_count = MAX_UPL_SIZE;
	}
	if (((cntrl_flags & UPL_SET_INTERNAL) && !(object->phys_contiguous)) &&
	    ((page_list_count != NULL) && (*page_list_count != 0) && *page_list_count < (size/page_size)))
	        return KERN_INVALID_ARGUMENT;

	if ((!object->internal) && (object->paging_offset != 0))
		panic("vm_object_iopl_request: external object with non-zero paging offset\n");


	if (object->phys_contiguous)
	        psize = PAGE_SIZE;
	else
	        psize = size;

	if (cntrl_flags & UPL_SET_INTERNAL) {
	        upl = upl_create(UPL_CREATE_INTERNAL | UPL_CREATE_LITE, UPL_IO_WIRE, psize);

		user_page_list = (upl_page_info_t *) (((uintptr_t)upl) + sizeof(struct upl));
		lite_list = (wpl_array_t) (((uintptr_t)user_page_list) +
					   ((psize / PAGE_SIZE) * sizeof(upl_page_info_t)));
		if (size == 0) {
			user_page_list = NULL;
			lite_list = NULL;
		}
	} else {
	        upl = upl_create(UPL_CREATE_LITE, UPL_IO_WIRE, psize);

		lite_list = (wpl_array_t) (((uintptr_t)upl) + sizeof(struct upl));
		if (size == 0) {
			lite_list = NULL;
		}
	}
	if (user_page_list)
	        user_page_list[0].device = FALSE;
	*upl_ptr = upl;

	upl->map_object = object;
	upl->size = size;

	if (object == kernel_object &&
	    !(cntrl_flags & (UPL_NEED_32BIT_ADDR | UPL_BLOCK_ACCESS))) {
		upl->flags |= UPL_KERNEL_OBJECT;
#if UPL_DEBUG
		vm_object_lock(object);
#else
		vm_object_lock_shared(object);
#endif
	} else {
		vm_object_lock(object);
		vm_object_activity_begin(object);
	}
	/*
	 * paging in progress also protects the paging_offset
	 */
	upl->offset = offset + object->paging_offset;

	if (cntrl_flags & UPL_BLOCK_ACCESS) {
		/*
		 * The user requested that access to the pages in this URL
		 * be blocked until the UPL is commited or aborted.
		 */
		upl->flags |= UPL_ACCESS_BLOCKED;
	}

	if (object->phys_contiguous) {
#if UPL_DEBUG
		queue_enter(&object->uplq, upl, upl_t, uplq);
#endif /* UPL_DEBUG */

		if (upl->flags & UPL_ACCESS_BLOCKED) {
			assert(!object->blocked_access);
			object->blocked_access = TRUE;
		}

		vm_object_unlock(object);

		/*
		 * don't need any shadow mappings for this one
		 * since it is already I/O memory
		 */
		upl->flags |= UPL_DEVICE_MEMORY;

		upl->highest_page = (ppnum_t) ((offset + object->shadow_offset + size - 1)>>PAGE_SHIFT);

		if (user_page_list) {
		        user_page_list[0].phys_addr = (ppnum_t) ((offset + object->shadow_offset)>>PAGE_SHIFT);
			user_page_list[0].device = TRUE;
		}
		if (page_list_count != NULL) {
		        if (upl->flags & UPL_INTERNAL)
			        *page_list_count = 0;
			else
			        *page_list_count = 1;
		}
		return KERN_SUCCESS;
	}
	if (object != kernel_object) {
		/*
		 * Protect user space from future COW operations
		 */
		object->true_share = TRUE;

		if (object->copy_strategy == MEMORY_OBJECT_COPY_SYMMETRIC)
			object->copy_strategy = MEMORY_OBJECT_COPY_DELAY;
	}

#if UPL_DEBUG
	queue_enter(&object->uplq, upl, upl_t, uplq);
#endif /* UPL_DEBUG */

	if (!(cntrl_flags & UPL_COPYOUT_FROM) &&
	    object->copy != VM_OBJECT_NULL) {
		/*
		 * Honor copy-on-write obligations
		 *
		 * The caller is gathering these pages and
		 * might modify their contents.  We need to
		 * make sure that the copy object has its own
		 * private copies of these pages before we let
		 * the caller modify them.
		 *
		 * NOTE: someone else could map the original object
		 * after we've done this copy-on-write here, and they
		 * could then see an inconsistent picture of the memory
		 * while it's being modified via the UPL.  To prevent this,
		 * we would have to block access to these pages until the
		 * UPL is released.  We could use the UPL_BLOCK_ACCESS
		 * code path for that...
		 */
		vm_object_update(object,
				 offset,
				 size,
				 NULL,
				 NULL,
				 FALSE,	/* should_return */
				 MEMORY_OBJECT_COPY_SYNC,
				 VM_PROT_NO_CHANGE);
#if DEVELOPMENT || DEBUG
		iopl_cow++;
		iopl_cow_pages += size >> PAGE_SHIFT;
#endif
	}


	entry = 0;

	xfer_size = size;
	dst_offset = offset;

	fault_info.behavior = VM_BEHAVIOR_SEQUENTIAL;
	fault_info.user_tag  = 0;
	fault_info.lo_offset = offset;
	fault_info.hi_offset = offset + xfer_size;
	fault_info.no_cache  = FALSE;
	fault_info.stealth = FALSE;

	dwp = &dw_array[0];
	dw_count = 0;

	while (xfer_size) {
	        vm_fault_return_t	result;
		unsigned int		pg_num;

		dwp->dw_mask = 0;

		dst_page = vm_page_lookup(object, dst_offset);

		/*
		 * ENCRYPTED SWAP:
		 * If the page is encrypted, we need to decrypt it,
		 * so force a soft page fault.
		 */
		if (dst_page == VM_PAGE_NULL ||
		    dst_page->busy ||
		    dst_page->encrypted ||
		    dst_page->error || 
		    dst_page->restart ||
		    dst_page->absent ||
		    dst_page->fictitious) {

		   if (object == kernel_object)
			   panic("vm_object_iopl_request: missing/bad page in kernel object\n");

		   do {
			vm_page_t	top_page;
			kern_return_t	error_code;
			int		interruptible;

			if (cntrl_flags & UPL_SET_INTERRUPTIBLE)
				interruptible = THREAD_ABORTSAFE;
			else
				interruptible = THREAD_UNINT;

			fault_info.interruptible = interruptible;
			fault_info.cluster_size = xfer_size;

			vm_object_paging_begin(object);

			result = vm_fault_page(object, dst_offset,
					       prot | VM_PROT_WRITE, FALSE, 
					       &prot, &dst_page, &top_page,
					       (int *)0,
					       &error_code, no_zero_fill,
					       FALSE, &fault_info);

			switch (result) {

			case VM_FAULT_SUCCESS:

				PAGE_WAKEUP_DONE(dst_page);
				/*
				 *	Release paging references and
				 *	top-level placeholder page, if any.
				 */
				if (top_page != VM_PAGE_NULL) {
					vm_object_t local_object;

					local_object = top_page->object;

					if (top_page->object != dst_page->object) {
						vm_object_lock(local_object);
						VM_PAGE_FREE(top_page);
						vm_object_paging_end(local_object);
						vm_object_unlock(local_object);
					} else {
						VM_PAGE_FREE(top_page);
						vm_object_paging_end(local_object);
					}
				}
				vm_object_paging_end(object);
				break;
			
			case VM_FAULT_RETRY:
				vm_object_lock(object);
				break;

			case VM_FAULT_FICTITIOUS_SHORTAGE:
				vm_page_more_fictitious();

				vm_object_lock(object);
				break;

			case VM_FAULT_MEMORY_SHORTAGE:
				if (vm_page_wait(interruptible)) {
					vm_object_lock(object);
					break;
				}
				/* fall thru */

			case VM_FAULT_INTERRUPTED:
				error_code = MACH_SEND_INTERRUPTED;
			case VM_FAULT_MEMORY_ERROR:
			memory_error:
				ret = (error_code ? error_code:	KERN_MEMORY_ERROR);

				vm_object_lock(object);
				goto return_err;

			case VM_FAULT_SUCCESS_NO_VM_PAGE:
				/* success but no page: fail */
				vm_object_paging_end(object);
				vm_object_unlock(object);
				goto memory_error;

			default:
				panic("vm_object_iopl_request: unexpected error"
				      " 0x%x from vm_fault_page()\n", result);
			}
		   } while (result != VM_FAULT_SUCCESS);

		}

		if (upl->flags & UPL_KERNEL_OBJECT)
			goto record_phys_addr;

		if (dst_page->cleaning) {
			/*
			 * Someone else is cleaning this page in place.as
			 * In theory, we should be able to  proceed and use this
			 * page but they'll probably end up clearing the "busy"
			 * bit on it in upl_commit_range() but they didn't set
			 * it, so they would clear our "busy" bit and open
			 * us to race conditions.
			 * We'd better wait for the cleaning to complete and
			 * then try again.
			 */
			vm_object_iopl_request_sleep_for_cleaning++;
			PAGE_SLEEP(object, dst_page, THREAD_UNINT);
			continue;
		}
		if ( (cntrl_flags & UPL_NEED_32BIT_ADDR) &&
		     dst_page->phys_page >= (max_valid_dma_address >> PAGE_SHIFT) ) {
		        vm_page_t	low_page;
			int 		refmod;

			/*
			 * support devices that can't DMA above 32 bits
			 * by substituting pages from a pool of low address
			 * memory for any pages we find above the 4G mark
			 * can't substitute if the page is already wired because
			 * we don't know whether that physical address has been
			 * handed out to some other 64 bit capable DMA device to use
			 */
			if (VM_PAGE_WIRED(dst_page)) {
			        ret = KERN_PROTECTION_FAILURE;
				goto return_err;
			}
			low_page = vm_page_grablo();

			if (low_page == VM_PAGE_NULL) {
			        ret = KERN_RESOURCE_SHORTAGE;
				goto return_err;
			}
			/*
			 * from here until the vm_page_replace completes
			 * we musn't drop the object lock... we don't
			 * want anyone refaulting this page in and using
			 * it after we disconnect it... we want the fault
			 * to find the new page being substituted.
			 */
			if (dst_page->pmapped)
			        refmod = pmap_disconnect(dst_page->phys_page);
			else
			        refmod = 0;
			vm_page_copy(dst_page, low_page);
		  
			low_page->reference = dst_page->reference;
			low_page->dirty     = dst_page->dirty;

			if (refmod & VM_MEM_REFERENCED)
			        low_page->reference = TRUE;
			if (refmod & VM_MEM_MODIFIED)
			        low_page->dirty = TRUE;

			vm_page_replace(low_page, object, dst_offset);

			dst_page = low_page;
			/*
			 * vm_page_grablo returned the page marked
			 * BUSY... we don't need a PAGE_WAKEUP_DONE
			 * here, because we've never dropped the object lock
			 */
			dst_page->busy = FALSE;
		}
		dwp->dw_mask |= DW_vm_page_wire;

		if (cntrl_flags & UPL_BLOCK_ACCESS) {
			/*
			 * Mark the page "busy" to block any future page fault
			 * on this page.  We'll also remove the mapping
			 * of all these pages before leaving this routine.
			 */
			assert(!dst_page->fictitious);
			dst_page->busy = TRUE;
		}
		/*
		 * expect the page to be used
		 * page queues lock must be held to set 'reference'
		 */
		dwp->dw_mask |= DW_set_reference;

   		if (!(cntrl_flags & UPL_COPYOUT_FROM))
			dst_page->dirty = TRUE;
record_phys_addr:
		pg_num = (unsigned int) ((dst_offset-offset)/PAGE_SIZE);
		assert(pg_num == (dst_offset-offset)/PAGE_SIZE);
		lite_list[pg_num>>5] |= 1 << (pg_num & 31);

		if (dst_page->phys_page > upl->highest_page)
		        upl->highest_page = dst_page->phys_page;

		if (user_page_list) {
			user_page_list[entry].phys_addr	= dst_page->phys_page;
			user_page_list[entry].pageout	= dst_page->pageout;
			user_page_list[entry].absent	= dst_page->absent;
			user_page_list[entry].dirty 	= dst_page->dirty;
			user_page_list[entry].precious	= dst_page->precious;
			user_page_list[entry].device 	= FALSE;
			if (dst_page->clustered == TRUE)
			        user_page_list[entry].speculative = dst_page->speculative;
			else
			        user_page_list[entry].speculative = FALSE;
			user_page_list[entry].cs_validated = dst_page->cs_validated;
			user_page_list[entry].cs_tainted = dst_page->cs_tainted;
		}
		if (object != kernel_object) {
			/*
			 * someone is explicitly grabbing this page...
			 * update clustered and speculative state
			 * 
			 */
			VM_PAGE_CONSUME_CLUSTERED(dst_page);
		}
		entry++;
		dst_offset += PAGE_SIZE_64;
		xfer_size -= PAGE_SIZE;

		if (dwp->dw_mask) {
			if (dst_page->busy == FALSE) {
				/*
				 * dw_do_work may need to drop the object lock
				 * if it does, we need the pages it's looking at to
				 * be held stable via the busy bit.
				 */
				dst_page->busy = TRUE;
				dwp->dw_mask |= (DW_clear_busy | DW_PAGE_WAKEUP);
			}
			dwp->dw_m = dst_page;
			dwp++;
			dw_count++;

			if (dw_count >= DELAYED_WORK_LIMIT) {
				dw_do_work(object, &dw_array[0], dw_count);
				
				dwp = &dw_array[0];
				dw_count = 0;
			}
		}
	}
	if (dw_count)
		dw_do_work(object, &dw_array[0], dw_count);

	if (page_list_count != NULL) {
	        if (upl->flags & UPL_INTERNAL)
			*page_list_count = 0;
		else if (*page_list_count > entry)
			*page_list_count = entry;
	}
	vm_object_unlock(object);

	if (cntrl_flags & UPL_BLOCK_ACCESS) {
		/*
		 * We've marked all the pages "busy" so that future
		 * page faults will block.
		 * Now remove the mapping for these pages, so that they
		 * can't be accessed without causing a page fault.
		 */
		vm_object_pmap_protect(object, offset, (vm_object_size_t)size,
				       PMAP_NULL, 0, VM_PROT_NONE);
		assert(!object->blocked_access);
		object->blocked_access = TRUE;
	}
	return KERN_SUCCESS;

return_err:
	dw_index = 0;

	for (; offset < dst_offset; offset += PAGE_SIZE) {
	        dst_page = vm_page_lookup(object, offset);

		if (dst_page == VM_PAGE_NULL)
		        panic("vm_object_iopl_request: Wired pages missing. \n");

		if (dw_count) {
			if (dw_array[dw_index].dw_m == dst_page) {
				dw_index++;
				dw_count--;
				continue;
			}
		}
		vm_page_lockspin_queues();
		vm_page_unwire(dst_page);
		vm_page_unlock_queues();

		VM_STAT_INCR(reactivations);
	}
#if UPL_DEBUG
	upl->upl_state = 2;
#endif
	if (! (upl->flags & UPL_KERNEL_OBJECT)) {
		vm_object_activity_end(object);
	}
	vm_object_unlock(object);
	upl_destroy(upl);

	return ret;
}

kern_return_t
upl_transpose(
	upl_t		upl1,
	upl_t		upl2)
{
	kern_return_t		retval;
	boolean_t		upls_locked;
	vm_object_t		object1, object2;

	if (upl1 == UPL_NULL || upl2 == UPL_NULL || upl1 == upl2  || ((upl1->flags & UPL_VECTOR)==UPL_VECTOR)  || ((upl2->flags & UPL_VECTOR)==UPL_VECTOR)) {
		return KERN_INVALID_ARGUMENT;
	}
	
	upls_locked = FALSE;

	/*
	 * Since we need to lock both UPLs at the same time,
	 * avoid deadlocks by always taking locks in the same order.
	 */
	if (upl1 < upl2) {
		upl_lock(upl1);
		upl_lock(upl2);
	} else {
		upl_lock(upl2);
		upl_lock(upl1);
	}
	upls_locked = TRUE;	/* the UPLs will need to be unlocked */

	object1 = upl1->map_object;
	object2 = upl2->map_object;

	if (upl1->offset != 0 || upl2->offset != 0 ||
	    upl1->size != upl2->size) {
		/*
		 * We deal only with full objects, not subsets.
		 * That's because we exchange the entire backing store info
		 * for the objects: pager, resident pages, etc...  We can't do
		 * only part of it.
		 */
		retval = KERN_INVALID_VALUE;
		goto done;
	}

	/*
	 * Tranpose the VM objects' backing store.
	 */
	retval = vm_object_transpose(object1, object2,
				     (vm_object_size_t) upl1->size);

	if (retval == KERN_SUCCESS) {
		/*
		 * Make each UPL point to the correct VM object, i.e. the
		 * object holding the pages that the UPL refers to...
		 */
#if UPL_DEBUG
		queue_remove(&object1->uplq, upl1, upl_t, uplq);
		queue_remove(&object2->uplq, upl2, upl_t, uplq);
#endif
		upl1->map_object = object2;
		upl2->map_object = object1;
#if UPL_DEBUG
		queue_enter(&object1->uplq, upl2, upl_t, uplq);
		queue_enter(&object2->uplq, upl1, upl_t, uplq);
#endif
	}

done:
	/*
	 * Cleanup.
	 */
	if (upls_locked) {
		upl_unlock(upl1);
		upl_unlock(upl2);
		upls_locked = FALSE;
	}

	return retval;
}

/*
 * ENCRYPTED SWAP:
 *
 * Rationale:  the user might have some encrypted data on disk (via
 * FileVault or any other mechanism).  That data is then decrypted in
 * memory, which is safe as long as the machine is secure.  But that
 * decrypted data in memory could be paged out to disk by the default
 * pager.  The data would then be stored on disk in clear (not encrypted)
 * and it could be accessed by anyone who gets physical access to the
 * disk (if the laptop or the disk gets stolen for example).  This weakens
 * the security offered by FileVault.
 *
 * Solution:  the default pager will optionally request that all the
 * pages it gathers for pageout be encrypted, via the UPL interfaces,
 * before it sends this UPL to disk via the vnode_pageout() path.
 * 
 * Notes:
 * 
 * To avoid disrupting the VM LRU algorithms, we want to keep the
 * clean-in-place mechanisms, which allow us to send some extra pages to 
 * swap (clustering) without actually removing them from the user's
 * address space.  We don't want the user to unknowingly access encrypted
 * data, so we have to actually remove the encrypted pages from the page
 * table.  When the user accesses the data, the hardware will fail to
 * locate the virtual page in its page table and will trigger a page
 * fault.  We can then decrypt the page and enter it in the page table
 * again.  Whenever we allow the user to access the contents of a page,
 * we have to make sure it's not encrypted.
 *
 * 
 */
/*
 * ENCRYPTED SWAP:
 * Reserve of virtual addresses in the kernel address space.
 * We need to map the physical pages in the kernel, so that we
 * can call the encryption/decryption routines with a kernel
 * virtual address.  We keep this pool of pre-allocated kernel
 * virtual addresses so that we don't have to scan the kernel's
 * virtaul address space each time we need to encrypt or decrypt
 * a physical page.
 * It would be nice to be able to encrypt and decrypt in physical
 * mode but that might not always be more efficient...
 */
decl_simple_lock_data(,vm_paging_lock)
#define VM_PAGING_NUM_PAGES	64
vm_map_offset_t vm_paging_base_address = 0;
boolean_t	vm_paging_page_inuse[VM_PAGING_NUM_PAGES] = { FALSE, };
int		vm_paging_max_index = 0;
int		vm_paging_page_waiter = 0;
int		vm_paging_page_waiter_total = 0;
unsigned long	vm_paging_no_kernel_page = 0;
unsigned long	vm_paging_objects_mapped = 0;
unsigned long	vm_paging_pages_mapped = 0;
unsigned long	vm_paging_objects_mapped_slow = 0;
unsigned long	vm_paging_pages_mapped_slow = 0;

void
vm_paging_map_init(void)
{
	kern_return_t	kr;
	vm_map_offset_t	page_map_offset;
	vm_map_entry_t	map_entry;

	assert(vm_paging_base_address == 0);

	/*
	 * Initialize our pool of pre-allocated kernel
	 * virtual addresses.
	 */
	page_map_offset = 0;
	kr = vm_map_find_space(kernel_map,
			       &page_map_offset,
			       VM_PAGING_NUM_PAGES * PAGE_SIZE,
			       0,
			       0,
			       &map_entry);
	if (kr != KERN_SUCCESS) {
		panic("vm_paging_map_init: kernel_map full\n");
	}
	map_entry->object.vm_object = kernel_object;
	map_entry->offset = page_map_offset;
	vm_object_reference(kernel_object);
	vm_map_unlock(kernel_map);

	assert(vm_paging_base_address == 0);
	vm_paging_base_address = page_map_offset;
}

/*
 * ENCRYPTED SWAP:
 * vm_paging_map_object:
 *	Maps part of a VM object's pages in the kernel
 * 	virtual address space, using the pre-allocated
 *	kernel virtual addresses, if possible.
 * Context:
 * 	The VM object is locked.  This lock will get
 * 	dropped and re-acquired though, so the caller
 * 	must make sure the VM object is kept alive
 *	(by holding a VM map that has a reference
 * 	on it, for example, or taking an extra reference).
 * 	The page should also be kept busy to prevent
 *	it from being reclaimed.
 */
kern_return_t
vm_paging_map_object(
	vm_map_offset_t		*address,
	vm_page_t		page,
	vm_object_t		object,
	vm_object_offset_t	offset,
	vm_map_size_t		*size,
	vm_prot_t		protection,
	boolean_t		can_unlock_object)
{
	kern_return_t		kr;
	vm_map_offset_t		page_map_offset;
	vm_map_size_t		map_size;
	vm_object_offset_t	object_offset;
	int			i;

	
	if (page != VM_PAGE_NULL && *size == PAGE_SIZE) {
		assert(page->busy);
		/*
		 * Use one of the pre-allocated kernel virtual addresses
		 * and just enter the VM page in the kernel address space
		 * at that virtual address.
		 */
		simple_lock(&vm_paging_lock);

		/*
		 * Try and find an available kernel virtual address
		 * from our pre-allocated pool.
		 */
		page_map_offset = 0;
		for (;;) {
			for (i = 0; i < VM_PAGING_NUM_PAGES; i++) {
				if (vm_paging_page_inuse[i] == FALSE) {
					page_map_offset =
						vm_paging_base_address +
						(i * PAGE_SIZE);
					break;
				}
			}
			if (page_map_offset != 0) {
				/* found a space to map our page ! */
				break;
			}

			if (can_unlock_object) {
				/*
				 * If we can afford to unlock the VM object,
				 * let's take the slow path now...
				 */
				break;
			}
			/*
			 * We can't afford to unlock the VM object, so
			 * let's wait for a space to become available...
			 */
			vm_paging_page_waiter_total++;
			vm_paging_page_waiter++;
			thread_sleep_fast_usimple_lock(&vm_paging_page_waiter,
						       &vm_paging_lock,
						       THREAD_UNINT);
			vm_paging_page_waiter--;
			/* ... and try again */
		}

		if (page_map_offset != 0) {
			/*
			 * We found a kernel virtual address;
			 * map the physical page to that virtual address.
			 */
			if (i > vm_paging_max_index) {
				vm_paging_max_index = i;
			}
			vm_paging_page_inuse[i] = TRUE;
			simple_unlock(&vm_paging_lock);

			if (page->pmapped == FALSE) {
				pmap_sync_page_data_phys(page->phys_page);
			}
			page->pmapped = TRUE;

			/*
			 * Keep the VM object locked over the PMAP_ENTER
			 * and the actual use of the page by the kernel,
			 * or this pmap mapping might get undone by a 
			 * vm_object_pmap_protect() call...
			 */
			PMAP_ENTER(kernel_pmap,
				   page_map_offset,
				   page,
				   protection,
				   ((int) page->object->wimg_bits &
				    VM_WIMG_MASK),
				   TRUE);
			vm_paging_objects_mapped++;
			vm_paging_pages_mapped++; 
			*address = page_map_offset;

			/* all done and mapped, ready to use ! */
			return KERN_SUCCESS;
		}

		/*
		 * We ran out of pre-allocated kernel virtual
		 * addresses.  Just map the page in the kernel
		 * the slow and regular way.
		 */
		vm_paging_no_kernel_page++;
		simple_unlock(&vm_paging_lock);
	}

	if (! can_unlock_object) {
		return KERN_NOT_SUPPORTED;
	}

	object_offset = vm_object_trunc_page(offset);
	map_size = vm_map_round_page(*size);

	/*
	 * Try and map the required range of the object
	 * in the kernel_map
	 */

	vm_object_reference_locked(object);	/* for the map entry */
	vm_object_unlock(object);

	kr = vm_map_enter(kernel_map,
			  address,
			  map_size,
			  0,
			  VM_FLAGS_ANYWHERE,
			  object,
			  object_offset,
			  FALSE,
			  protection,
			  VM_PROT_ALL,
			  VM_INHERIT_NONE);
	if (kr != KERN_SUCCESS) {
		*address = 0;
		*size = 0;
		vm_object_deallocate(object);	/* for the map entry */
		vm_object_lock(object);
		return kr;
	}

	*size = map_size;

	/*
	 * Enter the mapped pages in the page table now.
	 */
	vm_object_lock(object);
	/*
	 * VM object must be kept locked from before PMAP_ENTER()
	 * until after the kernel is done accessing the page(s).
	 * Otherwise, the pmap mappings in the kernel could be
	 * undone by a call to vm_object_pmap_protect().
	 */

	for (page_map_offset = 0;
	     map_size != 0;
	     map_size -= PAGE_SIZE_64, page_map_offset += PAGE_SIZE_64) {
		unsigned int	cache_attr;

		page = vm_page_lookup(object, offset + page_map_offset);
		if (page == VM_PAGE_NULL) {
			printf("vm_paging_map_object: no page !?");
			vm_object_unlock(object);
			kr = vm_map_remove(kernel_map, *address, *size,
					   VM_MAP_NO_FLAGS);
			assert(kr == KERN_SUCCESS);
			*address = 0;
			*size = 0;
			vm_object_lock(object);
			return KERN_MEMORY_ERROR;
		}
		if (page->pmapped == FALSE) {
			pmap_sync_page_data_phys(page->phys_page);
		}
		page->pmapped = TRUE;
		cache_attr = ((unsigned int) object->wimg_bits) & VM_WIMG_MASK;

		//assert(pmap_verify_free(page->phys_page));
		PMAP_ENTER(kernel_pmap,
			   *address + page_map_offset,
			   page,
			   protection,
			   cache_attr,
			   TRUE);
	}
			   
	vm_paging_objects_mapped_slow++;
	vm_paging_pages_mapped_slow += (unsigned long) (map_size / PAGE_SIZE_64);

	return KERN_SUCCESS;
}

/*
 * ENCRYPTED SWAP:
 * vm_paging_unmap_object:
 *	Unmaps part of a VM object's pages from the kernel
 * 	virtual address space.
 * Context:
 * 	The VM object is locked.  This lock will get
 * 	dropped and re-acquired though.
 */
void
vm_paging_unmap_object(
	vm_object_t	object,
	vm_map_offset_t	start,
	vm_map_offset_t	end)
{
	kern_return_t	kr;
	int		i;

	if ((vm_paging_base_address == 0) ||
	    (start < vm_paging_base_address) ||
	    (end > (vm_paging_base_address
		     + (VM_PAGING_NUM_PAGES * PAGE_SIZE)))) {
		/*
		 * We didn't use our pre-allocated pool of
		 * kernel virtual address.  Deallocate the
		 * virtual memory.
		 */
		if (object != VM_OBJECT_NULL) {
			vm_object_unlock(object);
		}
		kr = vm_map_remove(kernel_map, start, end, VM_MAP_NO_FLAGS);
		if (object != VM_OBJECT_NULL) {
			vm_object_lock(object);
		}
		assert(kr == KERN_SUCCESS);
	} else {
		/*
		 * We used a kernel virtual address from our
		 * pre-allocated pool.  Put it back in the pool
		 * for next time.
		 */
		assert(end - start == PAGE_SIZE);
		i = (int) ((start - vm_paging_base_address) >> PAGE_SHIFT);
		assert(i >= 0 && i < VM_PAGING_NUM_PAGES);

		/* undo the pmap mapping */
		pmap_remove(kernel_pmap, start, end);

		simple_lock(&vm_paging_lock);
		vm_paging_page_inuse[i] = FALSE;
		if (vm_paging_page_waiter) {
			thread_wakeup(&vm_paging_page_waiter);
		}
		simple_unlock(&vm_paging_lock);
	}
}

#if CRYPTO
/*
 * Encryption data.
 * "iv" is the "initial vector".  Ideally, we want to
 * have a different one for each page we encrypt, so that
 * crackers can't find encryption patterns too easily.
 */
#define SWAP_CRYPT_AES_KEY_SIZE	128	/* XXX 192 and 256 don't work ! */
boolean_t		swap_crypt_ctx_initialized = FALSE;
aes_32t 		swap_crypt_key[8]; /* big enough for a 256 key */
aes_ctx			swap_crypt_ctx;
const unsigned char	swap_crypt_null_iv[AES_BLOCK_SIZE] = {0xa, };

#if DEBUG
boolean_t		swap_crypt_ctx_tested = FALSE;
unsigned char swap_crypt_test_page_ref[4096] __attribute__((aligned(4096)));
unsigned char swap_crypt_test_page_encrypt[4096] __attribute__((aligned(4096)));
unsigned char swap_crypt_test_page_decrypt[4096] __attribute__((aligned(4096)));
#endif /* DEBUG */

/*
 * Initialize the encryption context: key and key size.
 */
void swap_crypt_ctx_initialize(void); /* forward */
void
swap_crypt_ctx_initialize(void)
{
	unsigned int	i;

	/*
	 * No need for locking to protect swap_crypt_ctx_initialized
	 * because the first use of encryption will come from the
	 * pageout thread (we won't pagein before there's been a pageout)
	 * and there's only one pageout thread.
	 */
	if (swap_crypt_ctx_initialized == FALSE) {
		for (i = 0;
		     i < (sizeof (swap_crypt_key) /
			  sizeof (swap_crypt_key[0]));
		     i++) {
			swap_crypt_key[i] = random();
		}
		aes_encrypt_key((const unsigned char *) swap_crypt_key,
				SWAP_CRYPT_AES_KEY_SIZE,
				&swap_crypt_ctx.encrypt);
		aes_decrypt_key((const unsigned char *) swap_crypt_key,
				SWAP_CRYPT_AES_KEY_SIZE,
				&swap_crypt_ctx.decrypt);
		swap_crypt_ctx_initialized = TRUE;
	}

#if DEBUG
	/*
	 * Validate the encryption algorithms.
	 */
	if (swap_crypt_ctx_tested == FALSE) {
		/* initialize */
		for (i = 0; i < 4096; i++) {
			swap_crypt_test_page_ref[i] = (char) i;
		}
		/* encrypt */
		aes_encrypt_cbc(swap_crypt_test_page_ref,
				swap_crypt_null_iv,
				PAGE_SIZE / AES_BLOCK_SIZE,
				swap_crypt_test_page_encrypt,
				&swap_crypt_ctx.encrypt);
		/* decrypt */
		aes_decrypt_cbc(swap_crypt_test_page_encrypt,
				swap_crypt_null_iv,
				PAGE_SIZE / AES_BLOCK_SIZE,
				swap_crypt_test_page_decrypt,
				&swap_crypt_ctx.decrypt);
		/* compare result with original */
		for (i = 0; i < 4096; i ++) {
			if (swap_crypt_test_page_decrypt[i] !=
			    swap_crypt_test_page_ref[i]) {
				panic("encryption test failed");
			}
		}

		/* encrypt again */
		aes_encrypt_cbc(swap_crypt_test_page_decrypt,
				swap_crypt_null_iv,
				PAGE_SIZE / AES_BLOCK_SIZE,
				swap_crypt_test_page_decrypt,
				&swap_crypt_ctx.encrypt);
		/* decrypt in place */
		aes_decrypt_cbc(swap_crypt_test_page_decrypt,
				swap_crypt_null_iv,
				PAGE_SIZE / AES_BLOCK_SIZE,
				swap_crypt_test_page_decrypt,
				&swap_crypt_ctx.decrypt);
		for (i = 0; i < 4096; i ++) {
			if (swap_crypt_test_page_decrypt[i] !=
			    swap_crypt_test_page_ref[i]) {
				panic("in place encryption test failed");
			}
		}

		swap_crypt_ctx_tested = TRUE;
	}
#endif /* DEBUG */
}

/*
 * ENCRYPTED SWAP:
 * vm_page_encrypt:
 * 	Encrypt the given page, for secure paging.
 * 	The page might already be mapped at kernel virtual
 * 	address "kernel_mapping_offset".  Otherwise, we need
 * 	to map it.
 * 
 * Context:
 * 	The page's object is locked, but this lock will be released
 * 	and re-acquired.
 * 	The page is busy and not accessible by users (not entered in any pmap).
 */
void
vm_page_encrypt(
	vm_page_t	page,
	vm_map_offset_t	kernel_mapping_offset)
{
	kern_return_t		kr;
	vm_map_size_t		kernel_mapping_size;
	vm_offset_t		kernel_vaddr;
	union {
		unsigned char	aes_iv[AES_BLOCK_SIZE];
		struct {
			memory_object_t		pager_object;
			vm_object_offset_t	paging_offset;
		} vm;
	} encrypt_iv;

	if (! vm_pages_encrypted) {
		vm_pages_encrypted = TRUE;
	}

	assert(page->busy);
	assert(page->dirty || page->precious);
	
	if (page->encrypted) {
		/*
		 * Already encrypted: no need to do it again.
		 */
		vm_page_encrypt_already_encrypted_counter++;
		return;
	}
	ASSERT_PAGE_DECRYPTED(page);

	/*
	 * Take a paging-in-progress reference to keep the object
	 * alive even if we have to unlock it (in vm_paging_map_object()
	 * for example)...
	 */
	vm_object_paging_begin(page->object);

	if (kernel_mapping_offset == 0) {
		/*
		 * The page hasn't already been mapped in kernel space
		 * by the caller.  Map it now, so that we can access
		 * its contents and encrypt them.
		 */
		kernel_mapping_size = PAGE_SIZE;
		kr = vm_paging_map_object(&kernel_mapping_offset,
					  page,
					  page->object,
					  page->offset,
					  &kernel_mapping_size,
					  VM_PROT_READ | VM_PROT_WRITE,
					  FALSE);
		if (kr != KERN_SUCCESS) {
			panic("vm_page_encrypt: "
			      "could not map page in kernel: 0x%x\n",
			      kr);
		}
	} else {
		kernel_mapping_size = 0;
	}
	kernel_vaddr = CAST_DOWN(vm_offset_t, kernel_mapping_offset);

	if (swap_crypt_ctx_initialized == FALSE) {
		swap_crypt_ctx_initialize();
	}
	assert(swap_crypt_ctx_initialized);

	/*
	 * Prepare an "initial vector" for the encryption.
	 * We use the "pager" and the "paging_offset" for that
	 * page to obfuscate the encrypted data a bit more and
	 * prevent crackers from finding patterns that they could
	 * use to break the key.
	 */
	bzero(&encrypt_iv.aes_iv[0], sizeof (encrypt_iv.aes_iv));
	encrypt_iv.vm.pager_object = page->object->pager;
	encrypt_iv.vm.paging_offset =
		page->object->paging_offset + page->offset;

	/* encrypt the "initial vector" */
	aes_encrypt_cbc((const unsigned char *) &encrypt_iv.aes_iv[0],
			swap_crypt_null_iv,
			1,
			&encrypt_iv.aes_iv[0],
			&swap_crypt_ctx.encrypt);
		  
	/*
	 * Encrypt the page.
	 */
	aes_encrypt_cbc((const unsigned char *) kernel_vaddr,
			&encrypt_iv.aes_iv[0],
			PAGE_SIZE / AES_BLOCK_SIZE,
			(unsigned char *) kernel_vaddr,
			&swap_crypt_ctx.encrypt);

	vm_page_encrypt_counter++;

	/*
	 * Unmap the page from the kernel's address space,
	 * if we had to map it ourselves.  Otherwise, let
	 * the caller undo the mapping if needed.
	 */
	if (kernel_mapping_size != 0) {
		vm_paging_unmap_object(page->object,
				       kernel_mapping_offset,
				       kernel_mapping_offset + kernel_mapping_size);
	}

	/*
	 * Clear the "reference" and "modified" bits.
	 * This should clean up any impact the encryption had
	 * on them.
	 * The page was kept busy and disconnected from all pmaps,
	 * so it can't have been referenced or modified from user
	 * space.
	 * The software bits will be reset later after the I/O
	 * has completed (in upl_commit_range()).
	 */
	pmap_clear_refmod(page->phys_page, VM_MEM_REFERENCED | VM_MEM_MODIFIED);

	page->encrypted = TRUE;

	vm_object_paging_end(page->object);
}

/*
 * ENCRYPTED SWAP:
 * vm_page_decrypt:
 * 	Decrypt the given page.
 * 	The page might already be mapped at kernel virtual
 * 	address "kernel_mapping_offset".  Otherwise, we need
 * 	to map it.
 *
 * Context:
 *	The page's VM object is locked but will be unlocked and relocked.
 * 	The page is busy and not accessible by users (not entered in any pmap).
 */
void
vm_page_decrypt(
	vm_page_t	page,
	vm_map_offset_t	kernel_mapping_offset)
{
	kern_return_t		kr;
	vm_map_size_t		kernel_mapping_size;
	vm_offset_t		kernel_vaddr;
	union {
		unsigned char	aes_iv[AES_BLOCK_SIZE];
		struct {
			memory_object_t		pager_object;
			vm_object_offset_t	paging_offset;
		} vm;
	} decrypt_iv;

	assert(page->busy);
	assert(page->encrypted);

	/*
	 * Take a paging-in-progress reference to keep the object
	 * alive even if we have to unlock it (in vm_paging_map_object()
	 * for example)...
	 */
	vm_object_paging_begin(page->object);

	if (kernel_mapping_offset == 0) {
		/*
		 * The page hasn't already been mapped in kernel space
		 * by the caller.  Map it now, so that we can access
		 * its contents and decrypt them.
		 */
		kernel_mapping_size = PAGE_SIZE;
		kr = vm_paging_map_object(&kernel_mapping_offset,
					  page,
					  page->object,
					  page->offset,
					  &kernel_mapping_size,
					  VM_PROT_READ | VM_PROT_WRITE,
					  FALSE);
		if (kr != KERN_SUCCESS) {
			panic("vm_page_decrypt: "
			      "could not map page in kernel: 0x%x\n",
			      kr);
		}
	} else {
		kernel_mapping_size = 0;
	}
	kernel_vaddr = CAST_DOWN(vm_offset_t, kernel_mapping_offset);

	assert(swap_crypt_ctx_initialized);

	/*
	 * Prepare an "initial vector" for the decryption.
	 * It has to be the same as the "initial vector" we
	 * used to encrypt that page.
	 */
	bzero(&decrypt_iv.aes_iv[0], sizeof (decrypt_iv.aes_iv));
	decrypt_iv.vm.pager_object = page->object->pager;
	decrypt_iv.vm.paging_offset =
		page->object->paging_offset + page->offset;

	/* encrypt the "initial vector" */
	aes_encrypt_cbc((const unsigned char *) &decrypt_iv.aes_iv[0],
			swap_crypt_null_iv,
			1,
			&decrypt_iv.aes_iv[0],
			&swap_crypt_ctx.encrypt);

	/*
	 * Decrypt the page.
	 */
	aes_decrypt_cbc((const unsigned char *) kernel_vaddr,
			&decrypt_iv.aes_iv[0],
			PAGE_SIZE / AES_BLOCK_SIZE,
			(unsigned char *) kernel_vaddr,
			&swap_crypt_ctx.decrypt);
	vm_page_decrypt_counter++;

	/*
	 * Unmap the page from the kernel's address space,
	 * if we had to map it ourselves.  Otherwise, let
	 * the caller undo the mapping if needed.
	 */
	if (kernel_mapping_size != 0) {
		vm_paging_unmap_object(page->object,
				       kernel_vaddr,
				       kernel_vaddr + PAGE_SIZE);
	}

	/*
	 * After decryption, the page is actually clean.
	 * It was encrypted as part of paging, which "cleans"
	 * the "dirty" pages.
	 * Noone could access it after it was encrypted
	 * and the decryption doesn't count.
	 */
	page->dirty = FALSE;
	assert (page->cs_validated == FALSE);
	pmap_clear_refmod(page->phys_page, VM_MEM_MODIFIED | VM_MEM_REFERENCED);
	page->encrypted = FALSE;

	/*
	 * We've just modified the page's contents via the data cache and part
	 * of the new contents might still be in the cache and not yet in RAM.
	 * Since the page is now available and might get gathered in a UPL to
	 * be part of a DMA transfer from a driver that expects the memory to
	 * be coherent at this point, we have to flush the data cache.
	 */
	pmap_sync_page_attributes_phys(page->phys_page);
	/*
	 * Since the page is not mapped yet, some code might assume that it
	 * doesn't need to invalidate the instruction cache when writing to
	 * that page.  That code relies on "pmapped" being FALSE, so that the
	 * caches get synchronized when the page is first mapped.
	 */
	assert(pmap_verify_free(page->phys_page));
	page->pmapped = FALSE;
	page->wpmapped = FALSE;

	vm_object_paging_end(page->object);
}

#if DEVELOPMENT || DEBUG
unsigned long upl_encrypt_upls = 0;
unsigned long upl_encrypt_pages = 0;
#endif

/*
 * ENCRYPTED SWAP:
 *
 * upl_encrypt:
 * 	Encrypts all the pages in the UPL, within the specified range.
 *
 */
void
upl_encrypt(
	upl_t			upl,
	upl_offset_t		crypt_offset,
	upl_size_t		crypt_size)
{
	upl_size_t		upl_size, subupl_size=crypt_size;
	upl_offset_t		offset_in_upl, subupl_offset=crypt_offset;
	vm_object_t		upl_object;
	vm_object_offset_t	upl_offset;
	vm_page_t		page;
	vm_object_t		shadow_object;
	vm_object_offset_t	shadow_offset;
	vm_object_offset_t	paging_offset;
	vm_object_offset_t	base_offset;
	int	 		isVectorUPL = 0;
	upl_t			vector_upl = NULL;

	if((isVectorUPL = vector_upl_is_valid(upl)))
		vector_upl = upl;

process_upl_to_encrypt:
	if(isVectorUPL) {
		crypt_size = subupl_size;
		crypt_offset = subupl_offset;
		upl =  vector_upl_subupl_byoffset(vector_upl, &crypt_offset, &crypt_size);
		if(upl == NULL)
			panic("upl_encrypt: Accessing a sub-upl that doesn't exist\n");
		subupl_size -= crypt_size;
		subupl_offset += crypt_size;
	}

#if DEVELOPMENT || DEBUG
	upl_encrypt_upls++;
	upl_encrypt_pages += crypt_size / PAGE_SIZE;
#endif
	upl_object = upl->map_object;
	upl_offset = upl->offset;
	upl_size = upl->size;

	vm_object_lock(upl_object);

	/*
	 * Find the VM object that contains the actual pages.
	 */
	if (upl_object->pageout) {
		shadow_object = upl_object->shadow;
		/*
		 * The offset in the shadow object is actually also
		 * accounted for in upl->offset.  It possibly shouldn't be
		 * this way, but for now don't account for it twice.
		 */
		shadow_offset = 0;
		assert(upl_object->paging_offset == 0);	/* XXX ? */
		vm_object_lock(shadow_object);
	} else {
		shadow_object = upl_object;
		shadow_offset = 0;
	}

	paging_offset = shadow_object->paging_offset;
	vm_object_paging_begin(shadow_object);

	if (shadow_object != upl_object)
	        vm_object_unlock(upl_object);


	base_offset = shadow_offset;
	base_offset += upl_offset;
	base_offset += crypt_offset;
	base_offset -= paging_offset;

	assert(crypt_offset + crypt_size <= upl_size);

	for (offset_in_upl = 0;
	     offset_in_upl < crypt_size;
	     offset_in_upl += PAGE_SIZE) {
		page = vm_page_lookup(shadow_object,
				      base_offset + offset_in_upl);
		if (page == VM_PAGE_NULL) {
			panic("upl_encrypt: "
			      "no page for (obj=%p,off=%lld+%d)!\n",
			      shadow_object,
			      base_offset,
			      offset_in_upl);
		}
		/*
		 * Disconnect the page from all pmaps, so that nobody can
		 * access it while it's encrypted.  After that point, all
		 * accesses to this page will cause a page fault and block
		 * while the page is busy being encrypted.  After the
		 * encryption completes, any access will cause a
		 * page fault and the page gets decrypted at that time.
		 */
		pmap_disconnect(page->phys_page);
		vm_page_encrypt(page, 0);

		if (vm_object_lock_avoid(shadow_object)) {
			/*
			 * Give vm_pageout_scan() a chance to convert more
			 * pages from "clean-in-place" to "clean-and-free",
			 * if it's interested in the same pages we selected
			 * in this cluster.
			 */
			vm_object_unlock(shadow_object);
			mutex_pause(2);
			vm_object_lock(shadow_object);
		}
	}

	vm_object_paging_end(shadow_object);
	vm_object_unlock(shadow_object);
	
	if(isVectorUPL && subupl_size)
		goto process_upl_to_encrypt;
}

#else /* CRYPTO */
void
upl_encrypt(
	__unused upl_t			upl,
	__unused upl_offset_t	crypt_offset,
	__unused upl_size_t	crypt_size)
{
}

void
vm_page_encrypt(
	__unused vm_page_t		page,
	__unused vm_map_offset_t	kernel_mapping_offset)
{
} 

void
vm_page_decrypt(
	__unused vm_page_t		page,
	__unused vm_map_offset_t	kernel_mapping_offset)
{
}

#endif /* CRYPTO */

void
vm_pageout_queue_steal(vm_page_t page, boolean_t queues_locked)
{
	page->list_req_pending = FALSE;
	page->cleaning = FALSE;
	page->pageout = FALSE;

	if (!queues_locked) {
		vm_page_lockspin_queues();
	}

	/*
	 * need to drop the laundry count...
	 * we may also need to remove it
	 * from the I/O paging queue...
	 * vm_pageout_throttle_up handles both cases
	 *
	 * the laundry and pageout_queue flags are cleared...
	 */
	vm_pageout_throttle_up(page);

	/*
	 * toss the wire count we picked up
	 * when we intially set this page up
	 * to be cleaned...
	 */
	vm_page_unwire(page);

	vm_page_steal_pageout_page++;

	if (!queues_locked) {
		vm_page_unlock_queues();
	}
}

upl_t
vector_upl_create(vm_offset_t upl_offset)
{
	int	vector_upl_size  = sizeof(struct _vector_upl);
	int i=0;
	upl_t	upl;
	vector_upl_t vector_upl = (vector_upl_t)kalloc(vector_upl_size);

	upl = upl_create(0,UPL_VECTOR,0);
	upl->vector_upl = vector_upl;
	upl->offset = upl_offset;
	vector_upl->size = 0;
	vector_upl->offset = upl_offset;
	vector_upl->invalid_upls=0;
	vector_upl->num_upls=0;
	vector_upl->pagelist = NULL;
	
	for(i=0; i < MAX_VECTOR_UPL_ELEMENTS ; i++) {
		vector_upl->upl_iostates[i].size = 0;
		vector_upl->upl_iostates[i].offset = 0;
		
	}
	return upl;
}

void
vector_upl_deallocate(upl_t upl)
{
	if(upl) {
		vector_upl_t vector_upl = upl->vector_upl;
		if(vector_upl) {
			if(vector_upl->invalid_upls != vector_upl->num_upls)
				panic("Deallocating non-empty Vectored UPL\n");
			kfree(vector_upl->pagelist,(sizeof(struct upl_page_info)*(vector_upl->size/PAGE_SIZE)));
			vector_upl->invalid_upls=0;
			vector_upl->num_upls = 0;
			vector_upl->pagelist = NULL;
			vector_upl->size = 0;
			vector_upl->offset = 0;
			kfree(vector_upl, sizeof(struct _vector_upl));
			vector_upl = (vector_upl_t)0xdeadbeef;
		}
		else
			panic("vector_upl_deallocate was passed a non-vectored upl\n");
	}
	else
		panic("vector_upl_deallocate was passed a NULL upl\n");
}

boolean_t
vector_upl_is_valid(upl_t upl)
{
	if(upl &&  ((upl->flags & UPL_VECTOR)==UPL_VECTOR)) {
		vector_upl_t vector_upl = upl->vector_upl;
		if(vector_upl == NULL || vector_upl == (vector_upl_t)0xdeadbeef || vector_upl == (vector_upl_t)0xfeedbeef)
			return FALSE;
		else
			return TRUE;
	}
	return FALSE;
}

boolean_t
vector_upl_set_subupl(upl_t upl,upl_t subupl, uint32_t io_size)
{
	if(vector_upl_is_valid(upl)) {		
		vector_upl_t vector_upl = upl->vector_upl;
		
		if(vector_upl) {
			if(subupl) {
				if(io_size) {
					if(io_size < PAGE_SIZE)
						io_size = PAGE_SIZE;
					subupl->vector_upl = (void*)vector_upl;
					vector_upl->upl_elems[vector_upl->num_upls++] = subupl;
					vector_upl->size += io_size;
					upl->size += io_size;
				}
				else {
					uint32_t i=0,invalid_upls=0;
					for(i = 0; i < vector_upl->num_upls; i++) {
						if(vector_upl->upl_elems[i] == subupl)
							break;
					}
					if(i == vector_upl->num_upls)
						panic("Trying to remove sub-upl when none exists");
					
					vector_upl->upl_elems[i] = NULL;
					invalid_upls = hw_atomic_add(&(vector_upl)->invalid_upls, 1); 
					if(invalid_upls == vector_upl->num_upls)
						return TRUE;
					else 
						return FALSE;
				}
			}
			else
				panic("vector_upl_set_subupl was passed a NULL upl element\n");
		}
		else
			panic("vector_upl_set_subupl was passed a non-vectored upl\n");
	}
	else
		panic("vector_upl_set_subupl was passed a NULL upl\n");

	return FALSE;
}	

void
vector_upl_set_pagelist(upl_t upl)
{
	if(vector_upl_is_valid(upl)) {		
		uint32_t i=0;
		vector_upl_t vector_upl = upl->vector_upl;

		if(vector_upl) {
			vm_offset_t pagelist_size=0, cur_upl_pagelist_size=0;

			vector_upl->pagelist = (upl_page_info_array_t)kalloc(sizeof(struct upl_page_info)*(vector_upl->size/PAGE_SIZE));
			
			for(i=0; i < vector_upl->num_upls; i++) {
				cur_upl_pagelist_size = sizeof(struct upl_page_info) * vector_upl->upl_elems[i]->size/PAGE_SIZE;
				bcopy(UPL_GET_INTERNAL_PAGE_LIST_SIMPLE(vector_upl->upl_elems[i]), (char*)vector_upl->pagelist + pagelist_size, cur_upl_pagelist_size);
				pagelist_size += cur_upl_pagelist_size;
				if(vector_upl->upl_elems[i]->highest_page > upl->highest_page)
					upl->highest_page = vector_upl->upl_elems[i]->highest_page;
			}
			assert( pagelist_size == (sizeof(struct upl_page_info)*(vector_upl->size/PAGE_SIZE)) );
		}
		else
			panic("vector_upl_set_pagelist was passed a non-vectored upl\n");
	}
	else
		panic("vector_upl_set_pagelist was passed a NULL upl\n");

}

upl_t
vector_upl_subupl_byindex(upl_t upl, uint32_t index)
{
	if(vector_upl_is_valid(upl)) {		
		vector_upl_t vector_upl = upl->vector_upl;
		if(vector_upl) {
			if(index < vector_upl->num_upls)
				return vector_upl->upl_elems[index];
		}
		else
			panic("vector_upl_subupl_byindex was passed a non-vectored upl\n");
	}
	return NULL;
}

upl_t
vector_upl_subupl_byoffset(upl_t upl, upl_offset_t *upl_offset, upl_size_t *upl_size)
{
	if(vector_upl_is_valid(upl)) {		
		uint32_t i=0;
		vector_upl_t vector_upl = upl->vector_upl;

		if(vector_upl) {
			upl_t subupl = NULL;
			vector_upl_iostates_t subupl_state;

			for(i=0; i < vector_upl->num_upls; i++) {
				subupl = vector_upl->upl_elems[i];
				subupl_state = vector_upl->upl_iostates[i];
				if( *upl_offset <= (subupl_state.offset + subupl_state.size - 1)) {
					/* We could have been passed an offset/size pair that belongs
					 * to an UPL element that has already been committed/aborted.
					 * If so, return NULL.
					 */
					if(subupl == NULL)
						return NULL;
					if((subupl_state.offset + subupl_state.size) < (*upl_offset + *upl_size)) {
						*upl_size = (subupl_state.offset + subupl_state.size) - *upl_offset;
						if(*upl_size > subupl_state.size)
							*upl_size = subupl_state.size;
					}
					if(*upl_offset >= subupl_state.offset)
						*upl_offset -= subupl_state.offset;
					else if(i)
						panic("Vector UPL offset miscalculation\n");
					return subupl;
				}	
			}
		}
		else
			panic("vector_upl_subupl_byoffset was passed a non-vectored UPL\n");
	}
	return NULL;
}

void
vector_upl_get_submap(upl_t upl, vm_map_t *v_upl_submap, vm_offset_t *submap_dst_addr)
{
	*v_upl_submap = NULL;

	if(vector_upl_is_valid(upl)) {		
		vector_upl_t vector_upl = upl->vector_upl;
		if(vector_upl) {
			*v_upl_submap = vector_upl->submap;
			*submap_dst_addr = vector_upl->submap_dst_addr;
		}
		else
			panic("vector_upl_get_submap was passed a non-vectored UPL\n");
	}
	else
		panic("vector_upl_get_submap was passed a null UPL\n");
}

void
vector_upl_set_submap(upl_t upl, vm_map_t submap, vm_offset_t submap_dst_addr)
{
	if(vector_upl_is_valid(upl)) {		
		vector_upl_t vector_upl = upl->vector_upl;
		if(vector_upl) {
			vector_upl->submap = submap;
			vector_upl->submap_dst_addr = submap_dst_addr;
		}
		else
			panic("vector_upl_get_submap was passed a non-vectored UPL\n");
	}
	else
		panic("vector_upl_get_submap was passed a NULL UPL\n");
}

void
vector_upl_set_iostate(upl_t upl, upl_t subupl, upl_offset_t offset, upl_size_t size)
{
	if(vector_upl_is_valid(upl)) {		
		uint32_t i = 0;
		vector_upl_t vector_upl = upl->vector_upl;

		if(vector_upl) {
			for(i = 0; i < vector_upl->num_upls; i++) {
				if(vector_upl->upl_elems[i] == subupl)
					break;
			}
			
			if(i == vector_upl->num_upls)
				panic("setting sub-upl iostate when none exists");

			vector_upl->upl_iostates[i].offset = offset;
			if(size < PAGE_SIZE)
				size = PAGE_SIZE;
			vector_upl->upl_iostates[i].size = size;
		}
		else
			panic("vector_upl_set_iostate was passed a non-vectored UPL\n");
	}
	else
		panic("vector_upl_set_iostate was passed a NULL UPL\n");
}

void
vector_upl_get_iostate(upl_t upl, upl_t subupl, upl_offset_t *offset, upl_size_t *size)
{
	if(vector_upl_is_valid(upl)) {		
		uint32_t i = 0;
		vector_upl_t vector_upl = upl->vector_upl;

		if(vector_upl) {
			for(i = 0; i < vector_upl->num_upls; i++) {
				if(vector_upl->upl_elems[i] == subupl)
					break;
			}
			
			if(i == vector_upl->num_upls)
				panic("getting sub-upl iostate when none exists");

			*offset = vector_upl->upl_iostates[i].offset;
			*size = vector_upl->upl_iostates[i].size;
		}
		else
			panic("vector_upl_get_iostate was passed a non-vectored UPL\n");
	}
	else
		panic("vector_upl_get_iostate was passed a NULL UPL\n");
}

void
vector_upl_get_iostate_byindex(upl_t upl, uint32_t index, upl_offset_t *offset, upl_size_t *size)
{
	if(vector_upl_is_valid(upl)) {		
		vector_upl_t vector_upl = upl->vector_upl;
		if(vector_upl) {
			if(index < vector_upl->num_upls) {
				*offset = vector_upl->upl_iostates[index].offset;
				*size = vector_upl->upl_iostates[index].size;
			}
			else
				*offset = *size = 0;
		}
		else
			panic("vector_upl_get_iostate_byindex was passed a non-vectored UPL\n");
	}
	else
		panic("vector_upl_get_iostate_byindex was passed a NULL UPL\n");
}

upl_page_info_t *
upl_get_internal_vectorupl_pagelist(upl_t upl)
{
	return ((vector_upl_t)(upl->vector_upl))->pagelist;
}

void *
upl_get_internal_vectorupl(upl_t upl)
{
	return upl->vector_upl;
}

vm_size_t
upl_get_internal_pagelist_offset(void)
{
	return sizeof(struct upl);
}

void
upl_clear_dirty(
	upl_t		upl,
	boolean_t 	value)
{
	if (value) {
		upl->flags |= UPL_CLEAR_DIRTY;
	} else {
		upl->flags &= ~UPL_CLEAR_DIRTY;
	}
}


#ifdef MACH_BSD

boolean_t  upl_device_page(upl_page_info_t *upl)
{
	return(UPL_DEVICE_PAGE(upl));
}
boolean_t  upl_page_present(upl_page_info_t *upl, int index)
{
	return(UPL_PAGE_PRESENT(upl, index));
}
boolean_t  upl_speculative_page(upl_page_info_t *upl, int index)
{
	return(UPL_SPECULATIVE_PAGE(upl, index));
}
boolean_t  upl_dirty_page(upl_page_info_t *upl, int index)
{
	return(UPL_DIRTY_PAGE(upl, index));
}
boolean_t  upl_valid_page(upl_page_info_t *upl, int index)
{
	return(UPL_VALID_PAGE(upl, index));
}
ppnum_t  upl_phys_page(upl_page_info_t *upl, int index)
{
	return(UPL_PHYS_PAGE(upl, index));
}


void
vm_countdirtypages(void)
{
	vm_page_t m;
	int dpages;
	int pgopages;
	int precpages;


	dpages=0;
	pgopages=0;
	precpages=0;

	vm_page_lock_queues();
	m = (vm_page_t) queue_first(&vm_page_queue_inactive);
	do {
		if (m ==(vm_page_t )0) break;

		if(m->dirty) dpages++;
		if(m->pageout) pgopages++;
		if(m->precious) precpages++;

		assert(m->object != kernel_object);
		m = (vm_page_t) queue_next(&m->pageq);
		if (m ==(vm_page_t )0) break;

	} while (!queue_end(&vm_page_queue_inactive,(queue_entry_t) m));
	vm_page_unlock_queues();

	vm_page_lock_queues();
	m = (vm_page_t) queue_first(&vm_page_queue_throttled);
	do {
		if (m ==(vm_page_t )0) break;

		dpages++;
		assert(m->dirty);
		assert(!m->pageout);
		assert(m->object != kernel_object);
		m = (vm_page_t) queue_next(&m->pageq);
		if (m ==(vm_page_t )0) break;

	} while (!queue_end(&vm_page_queue_throttled,(queue_entry_t) m));
	vm_page_unlock_queues();

	vm_page_lock_queues();
	m = (vm_page_t) queue_first(&vm_page_queue_zf);
	do {
		if (m ==(vm_page_t )0) break;

		if(m->dirty) dpages++;
		if(m->pageout) pgopages++;
		if(m->precious) precpages++;

		assert(m->object != kernel_object);
		m = (vm_page_t) queue_next(&m->pageq);
		if (m ==(vm_page_t )0) break;

	} while (!queue_end(&vm_page_queue_zf,(queue_entry_t) m));
	vm_page_unlock_queues();

	printf("IN Q: %d : %d : %d\n", dpages, pgopages, precpages);

	dpages=0;
	pgopages=0;
	precpages=0;

	vm_page_lock_queues();
	m = (vm_page_t) queue_first(&vm_page_queue_active);

	do {
		if(m == (vm_page_t )0) break;
		if(m->dirty) dpages++;
		if(m->pageout) pgopages++;
		if(m->precious) precpages++;

		assert(m->object != kernel_object);
		m = (vm_page_t) queue_next(&m->pageq);
		if(m == (vm_page_t )0) break;

	} while (!queue_end(&vm_page_queue_active,(queue_entry_t) m));
	vm_page_unlock_queues();

	printf("AC Q: %d : %d : %d\n", dpages, pgopages, precpages);

}
#endif /* MACH_BSD */

ppnum_t upl_get_highest_page(
			     upl_t			upl)
{
        return upl->highest_page;
}

upl_size_t upl_get_size(
			     upl_t			upl)
{
        return upl->size;
}

#if UPL_DEBUG
kern_return_t  upl_ubc_alias_set(upl_t upl, uintptr_t alias1, uintptr_t alias2)
{
	upl->ubc_alias1 = alias1;
	upl->ubc_alias2 = alias2;
	return KERN_SUCCESS;
}
int  upl_ubc_alias_get(upl_t upl, uintptr_t * al, uintptr_t * al2)
{
	if(al)
		*al = upl->ubc_alias1;
	if(al2)
		*al2 = upl->ubc_alias2;
	return KERN_SUCCESS;
}
#endif /* UPL_DEBUG */



#if	MACH_KDB
#include <ddb/db_output.h>
#include <ddb/db_print.h>
#include <vm/vm_print.h>

#define	printf	kdbprintf
void		db_pageout(void);

void
db_vm(void)
{

	iprintf("VM Statistics:\n");
	db_indent += 2;
	iprintf("pages:\n");
	db_indent += 2;
	iprintf("activ %5d  inact %5d  free  %5d",
		vm_page_active_count, vm_page_inactive_count,
		vm_page_free_count);
	printf("   wire  %5d  gobbl %5d\n",
	       vm_page_wire_count, vm_page_gobble_count);
	db_indent -= 2;
	iprintf("target:\n");
	db_indent += 2;
	iprintf("min   %5d  inact %5d  free  %5d",
		vm_page_free_min, vm_page_inactive_target,
		vm_page_free_target);
	printf("   resrv %5d\n", vm_page_free_reserved);
	db_indent -= 2;
	iprintf("pause:\n");
	db_pageout();
	db_indent -= 2;
}

#if	MACH_COUNTERS
extern int c_laundry_pages_freed;
#endif	/* MACH_COUNTERS */

void
db_pageout(void)
{
	iprintf("Pageout Statistics:\n");
	db_indent += 2;
	iprintf("active %5d  inactv %5d\n",
		vm_pageout_active, vm_pageout_inactive);
	iprintf("nolock %5d  avoid  %5d  busy   %5d  absent %5d\n",
		vm_pageout_inactive_nolock, vm_pageout_inactive_avoid,
		vm_pageout_inactive_busy, vm_pageout_inactive_absent);
	iprintf("used   %5d  clean  %5d  dirty  %5d\n",
		vm_pageout_inactive_used, vm_pageout_inactive_clean,
		vm_pageout_inactive_dirty);
#if	MACH_COUNTERS
	iprintf("laundry_pages_freed %d\n", c_laundry_pages_freed);
#endif	/* MACH_COUNTERS */
#if	MACH_CLUSTER_STATS
	iprintf("Cluster Statistics:\n");
	db_indent += 2;
	iprintf("dirtied   %5d   cleaned  %5d   collisions  %5d\n",
		vm_pageout_cluster_dirtied, vm_pageout_cluster_cleaned,
		vm_pageout_cluster_collisions);
	iprintf("clusters  %5d   conversions  %5d\n",
		vm_pageout_cluster_clusters, vm_pageout_cluster_conversions);
	db_indent -= 2;
	iprintf("Target Statistics:\n");
	db_indent += 2;
	iprintf("collisions   %5d   page_dirtied  %5d   page_freed  %5d\n",
		vm_pageout_target_collisions, vm_pageout_target_page_dirtied,
		vm_pageout_target_page_freed);
	db_indent -= 2;
#endif	/* MACH_CLUSTER_STATS */
	db_indent -= 2;
}

#endif	/* MACH_KDB */

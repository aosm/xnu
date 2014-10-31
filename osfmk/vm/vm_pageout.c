/*
 * Copyright (c) 2000-2014 Apple Inc. All rights reserved.
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

#include <vm/pmap.h>
#include <vm/vm_compressor_pager.h>
#include <vm/vm_fault.h>
#include <vm/vm_map.h>
#include <vm/vm_object.h>
#include <vm/vm_page.h>
#include <vm/vm_pageout.h>
#include <vm/vm_protos.h> /* must be last */
#include <vm/memory_object.h>
#include <vm/vm_purgeable_internal.h>
#include <vm/vm_shared_region.h>
#include <vm/vm_compressor.h>

#if CONFIG_PHANTOM_CACHE
#include <vm/vm_phantom_cache.h>
#endif
/*
 * ENCRYPTED SWAP:
 */
#include <libkern/crypto/aes.h>
extern u_int32_t random(void);	/* from <libkern/libkern.h> */

extern int cs_debug;

#if UPL_DEBUG
#include <libkern/OSDebug.h>
#endif

extern void m_drain(void);

#if VM_PRESSURE_EVENTS
extern unsigned int memorystatus_available_pages;
extern unsigned int memorystatus_available_pages_pressure;
extern unsigned int memorystatus_available_pages_critical;
extern unsigned int memorystatus_frozen_count;
extern unsigned int memorystatus_suspended_count;

extern vm_pressure_level_t memorystatus_vm_pressure_level;
int memorystatus_purge_on_warning = 2;
int memorystatus_purge_on_urgent = 5;
int memorystatus_purge_on_critical = 8;

void vm_pressure_response(void);
boolean_t vm_pressure_thread_running = FALSE;
extern void consider_vm_pressure_events(void);

#define MEMORYSTATUS_SUSPENDED_THRESHOLD  4
#endif /* VM_PRESSURE_EVENTS */

boolean_t	vm_pressure_changed = FALSE;

#ifndef VM_PAGEOUT_BURST_ACTIVE_THROTTLE   /* maximum iterations of the active queue to move pages to inactive */
#define VM_PAGEOUT_BURST_ACTIVE_THROTTLE  100
#endif

#ifndef VM_PAGEOUT_BURST_INACTIVE_THROTTLE  /* maximum iterations of the inactive queue w/o stealing/cleaning a page */
#define VM_PAGEOUT_BURST_INACTIVE_THROTTLE 4096
#endif

#ifndef VM_PAGEOUT_DEADLOCK_RELIEF
#define VM_PAGEOUT_DEADLOCK_RELIEF 100	/* number of pages to move to break deadlock */
#endif

#ifndef VM_PAGEOUT_INACTIVE_RELIEF
#define VM_PAGEOUT_INACTIVE_RELIEF 50	/* minimum number of pages to move to the inactive q */
#endif

#ifndef	VM_PAGE_LAUNDRY_MAX
#define	VM_PAGE_LAUNDRY_MAX	128UL	/* maximum pageouts on a given pageout queue */
#endif	/* VM_PAGEOUT_LAUNDRY_MAX */

#ifndef	VM_PAGEOUT_BURST_WAIT
#define	VM_PAGEOUT_BURST_WAIT	10	/* milliseconds */
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

#ifndef	VM_PAGEOUT_SWAP_WAIT
#define VM_PAGEOUT_SWAP_WAIT	50	/* milliseconds */
#endif	/* VM_PAGEOUT_SWAP_WAIT */

#ifndef VM_PAGEOUT_PRESSURE_PAGES_CONSIDERED
#define VM_PAGEOUT_PRESSURE_PAGES_CONSIDERED		1000	/* maximum pages considered before we issue a pressure event */
#endif /* VM_PAGEOUT_PRESSURE_PAGES_CONSIDERED */

#ifndef VM_PAGEOUT_PRESSURE_EVENT_MONITOR_SECS
#define VM_PAGEOUT_PRESSURE_EVENT_MONITOR_SECS		5	/* seconds */
#endif /* VM_PAGEOUT_PRESSURE_EVENT_MONITOR_SECS */

unsigned int	vm_page_speculative_q_age_ms = VM_PAGE_SPECULATIVE_Q_AGE_MS;
unsigned int	vm_page_speculative_percentage = 5;

#ifndef VM_PAGE_SPECULATIVE_TARGET
#define VM_PAGE_SPECULATIVE_TARGET(total) ((total) * 1 / (100 / vm_page_speculative_percentage))
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
#define	VM_PAGE_INACTIVE_TARGET(avail)	((avail) * 1 / 2)
#endif	/* VM_PAGE_INACTIVE_TARGET */

/*
 *	Once the pageout daemon starts running, it keeps going
 *	until vm_page_free_count meets or exceeds vm_page_free_target.
 */

#ifndef	VM_PAGE_FREE_TARGET
#define	VM_PAGE_FREE_TARGET(free)	(15 + (free) / 80)
#endif	/* VM_PAGE_FREE_TARGET */


/*
 *	The pageout daemon always starts running once vm_page_free_count
 *	falls below vm_page_free_min.
 */

#ifndef	VM_PAGE_FREE_MIN
#define	VM_PAGE_FREE_MIN(free)		(10 + (free) / 100)
#endif	/* VM_PAGE_FREE_MIN */

#define VM_PAGE_FREE_RESERVED_LIMIT	1700
#define VM_PAGE_FREE_MIN_LIMIT		3500
#define VM_PAGE_FREE_TARGET_LIMIT	4000

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
#define	VM_PAGE_REACTIVATE_LIMIT(avail)	(MAX((avail) * 1 / 20,VM_PAGE_REACTIVATE_LIMIT_MAX))
#endif	/* VM_PAGE_REACTIVATE_LIMIT */
#define VM_PAGEOUT_INACTIVE_FORCE_RECLAIM	100


extern boolean_t hibernate_cleaning_in_progress;

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
struct cq {
	struct vm_pageout_queue *q;
	void			*current_chead;
	char			*scratch_buf;
};


#if VM_PRESSURE_EVENTS
void vm_pressure_thread(void);

boolean_t VM_PRESSURE_NORMAL_TO_WARNING(void);
boolean_t VM_PRESSURE_WARNING_TO_CRITICAL(void);

boolean_t VM_PRESSURE_WARNING_TO_NORMAL(void);
boolean_t VM_PRESSURE_CRITICAL_TO_WARNING(void);
#endif
static void vm_pageout_garbage_collect(int);
static void vm_pageout_iothread_continue(struct vm_pageout_queue *);
static void vm_pageout_iothread_external(void);
static void vm_pageout_iothread_internal(struct cq *cq);
static void vm_pageout_adjust_io_throttles(struct vm_pageout_queue *, struct vm_pageout_queue *, boolean_t);

extern void vm_pageout_continue(void);
extern void vm_pageout_scan(void);

static thread_t	vm_pageout_external_iothread = THREAD_NULL;
static thread_t	vm_pageout_internal_iothread = THREAD_NULL;

unsigned int vm_pageout_reserved_internal = 0;
unsigned int vm_pageout_reserved_really = 0;

unsigned int vm_pageout_swap_wait = 0;
unsigned int vm_pageout_idle_wait = 0;		/* milliseconds */
unsigned int vm_pageout_empty_wait = 0;		/* milliseconds */
unsigned int vm_pageout_burst_wait = 0;		/* milliseconds */
unsigned int vm_pageout_deadlock_wait = 0;	/* milliseconds */
unsigned int vm_pageout_deadlock_relief = 0;
unsigned int vm_pageout_inactive_relief = 0;
unsigned int vm_pageout_burst_active_throttle = 0;
unsigned int vm_pageout_burst_inactive_throttle = 0;

int	vm_upl_wait_for_pages = 0;


/*
 *	These variables record the pageout daemon's actions:
 *	how many pages it looks at and what happens to those pages.
 *	No locking needed because only one thread modifies the variables.
 */

unsigned int vm_pageout_active = 0;		/* debugging */
unsigned int vm_pageout_active_busy = 0;	/* debugging */
unsigned int vm_pageout_inactive = 0;		/* debugging */
unsigned int vm_pageout_inactive_throttled = 0;	/* debugging */
unsigned int vm_pageout_inactive_forced = 0;	/* debugging */
unsigned int vm_pageout_inactive_nolock = 0;	/* debugging */
unsigned int vm_pageout_inactive_avoid = 0;	/* debugging */
unsigned int vm_pageout_inactive_busy = 0;	/* debugging */
unsigned int vm_pageout_inactive_error = 0;	/* debugging */
unsigned int vm_pageout_inactive_absent = 0;	/* debugging */
unsigned int vm_pageout_inactive_notalive = 0;	/* debugging */
unsigned int vm_pageout_inactive_used = 0;	/* debugging */
unsigned int vm_pageout_cache_evicted = 0;	/* debugging */
unsigned int vm_pageout_inactive_clean = 0;	/* debugging */
unsigned int vm_pageout_speculative_clean = 0;	/* debugging */

unsigned int vm_pageout_freed_from_cleaned = 0;
unsigned int vm_pageout_freed_from_speculative = 0;
unsigned int vm_pageout_freed_from_inactive_clean = 0;

unsigned int vm_pageout_enqueued_cleaned_from_inactive_clean = 0;
unsigned int vm_pageout_enqueued_cleaned_from_inactive_dirty = 0;

unsigned int vm_pageout_cleaned_reclaimed = 0;		/* debugging; how many cleaned pages are reclaimed by the pageout scan */
unsigned int vm_pageout_cleaned_reactivated = 0;	/* debugging; how many cleaned pages are found to be referenced on pageout (and are therefore reactivated) */
unsigned int vm_pageout_cleaned_reference_reactivated = 0;
unsigned int vm_pageout_cleaned_volatile_reactivated = 0;
unsigned int vm_pageout_cleaned_fault_reactivated = 0;
unsigned int vm_pageout_cleaned_commit_reactivated = 0;	/* debugging; how many cleaned pages are found to be referenced on commit (and are therefore reactivated) */
unsigned int vm_pageout_cleaned_busy = 0;
unsigned int vm_pageout_cleaned_nolock = 0;

unsigned int vm_pageout_inactive_dirty_internal = 0;	/* debugging */
unsigned int vm_pageout_inactive_dirty_external = 0;	/* debugging */
unsigned int vm_pageout_inactive_deactivated = 0;	/* debugging */
unsigned int vm_pageout_inactive_anonymous = 0;	/* debugging */
unsigned int vm_pageout_dirty_no_pager = 0;	/* debugging */
unsigned int vm_pageout_purged_objects = 0;	/* debugging */
unsigned int vm_stat_discard = 0;		/* debugging */
unsigned int vm_stat_discard_sent = 0;		/* debugging */
unsigned int vm_stat_discard_failure = 0;	/* debugging */
unsigned int vm_stat_discard_throttle = 0;	/* debugging */
unsigned int vm_pageout_reactivation_limit_exceeded = 0;	/* debugging */
unsigned int vm_pageout_catch_ups = 0;				/* debugging */
unsigned int vm_pageout_inactive_force_reclaim = 0;	/* debugging */

unsigned int vm_pageout_scan_reclaimed_throttled = 0;
unsigned int vm_pageout_scan_active_throttled = 0;
unsigned int vm_pageout_scan_inactive_throttled_internal = 0;
unsigned int vm_pageout_scan_inactive_throttled_external = 0;
unsigned int vm_pageout_scan_throttle = 0;			/* debugging */
unsigned int vm_pageout_scan_burst_throttle = 0;		/* debugging */
unsigned int vm_pageout_scan_empty_throttle = 0;		/* debugging */
unsigned int vm_pageout_scan_swap_throttle = 0;		/* debugging */
unsigned int vm_pageout_scan_deadlock_detected = 0;		/* debugging */
unsigned int vm_pageout_scan_active_throttle_success = 0;	/* debugging */
unsigned int vm_pageout_scan_inactive_throttle_success = 0;	/* debugging */
unsigned int vm_pageout_inactive_external_forced_jetsam_count = 0;	/* debugging */
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

boolean_t (* volatile consider_buffer_cache_collect)(int) = NULL;

#if DEVELOPMENT || DEBUG
unsigned long vm_cs_validated_resets = 0;
#endif

int	vm_debug_events	= 0;

#if CONFIG_MEMORYSTATUS
#if !CONFIG_JETSAM
extern boolean_t memorystatus_idle_exit_from_VM(void);
#endif
extern boolean_t memorystatus_kill_on_VM_page_shortage(boolean_t async);
extern void memorystatus_on_pageout_scan_end(void);
#endif

boolean_t	vm_page_compressions_failing = FALSE;

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
		assert(!p->laundry);

		offset = p->offset;
		VM_PAGE_FREE(p);
		p = VM_PAGE_NULL;

		m = vm_page_lookup(shadow_object,
			offset + object->vo_shadow_offset);

		if(m == VM_PAGE_NULL)
			continue;

		assert((m->dirty) || (m->precious) ||
				(m->busy && m->cleaning));

		/*
		 * Handle the trusted pager throttle.
		 * Also decrement the burst throttle (if external).
		 */
		vm_page_lock_queues();
		if (m->pageout_queue)
			vm_pageout_throttle_up(m);

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
			if (pmap_disconnect(m->phys_page) & VM_MEM_MODIFIED) {
				SET_PAGE_DIRTY(m, FALSE);
			} else {
				m->dirty = FALSE;
			}

			if (m->dirty) {
				CLUSTER_STAT(vm_pageout_target_page_dirtied++;)
				vm_page_unwire(m, TRUE);	/* reactivates */
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
		if (m->overwriting) {
			/*
			 * the (COPY_OUT_FROM == FALSE) request_page_list case
			 */
			if (m->busy) {
				/*
				 * We do not re-set m->dirty !
				 * The page was busy so no extraneous activity
				 * could have occurred. COPY_INTO is a read into the
				 * new pages. CLEAN_IN_PLACE does actually write
				 * out the pages but handling outside of this code
				 * will take care of resetting dirty. We clear the
				 * modify however for the Programmed I/O case.
				 */
				pmap_clear_modify(m->phys_page);

				m->busy = FALSE;
				m->absent = FALSE;
			} else {
				/*
				 * alternate (COPY_OUT_FROM == FALSE) request_page_list case
				 * Occurs when the original page was wired
				 * at the time of the list request
				 */
				 assert(VM_PAGE_WIRED(m));
				 vm_page_unwire(m, TRUE);	/* reactivates */
			}
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
			m->dirty = FALSE;
#endif
		}
		if (m->encrypted_cleaning == TRUE) {
			m->encrypted_cleaning = FALSE;
			m->busy = FALSE;
		}
		m->cleaning = FALSE;

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
	SET_PAGE_DIRTY(m, FALSE);
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

	/*
	 * set the page for future call to vm_fault_list_request
	 */
	pmap_clear_modify(m->phys_page);
	SET_PAGE_DIRTY(m, FALSE);
	m->pageout = TRUE;

	/*
	 * keep the object from collapsing or terminating
	 */
	vm_object_paging_begin(object);
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
 * The object and queues must be locked. We will take a
 * paging reference to prevent deallocation or collapse when we
 * release the object lock back at the call site.  The I/O thread
 * is responsible for consuming this reference
 *
 * The page must not be on any pageout queue.
 */

void
vm_pageout_cluster(vm_page_t m, boolean_t pageout)
{
	vm_object_t	object = m->object;
        struct		vm_pageout_queue *q;


	XPR(XPR_VM_PAGEOUT,
		"vm_pageout_cluster, object 0x%X offset 0x%X page 0x%X\n",
		object, m->offset, m, 0, 0);

	VM_PAGE_CHECK(m);
#if DEBUG
	lck_mtx_assert(&vm_page_queue_lock, LCK_MTX_ASSERT_OWNED);
#endif
	vm_object_lock_assert_exclusive(object);

	/*
	 * Only a certain kind of page is appreciated here.
	 */
	assert((m->dirty || m->precious) && (!VM_PAGE_WIRED(m)));
	assert(!m->cleaning && !m->pageout && !m->laundry);
#ifndef CONFIG_FREEZE
	assert(!m->inactive && !m->active);
	assert(!m->throttled);
#endif

	/*
	 * protect the object from collapse or termination
	 */
	vm_object_activity_begin(object);

	m->pageout = pageout;

	if (object->internal == TRUE) {
		if (COMPRESSED_PAGER_IS_ACTIVE || DEFAULT_FREEZER_COMPRESSED_PAGER_IS_ACTIVE)
			m->busy = TRUE;

	        q = &vm_pageout_queue_internal;
	} else
	        q = &vm_pageout_queue_external;

	/* 
	 * pgo_laundry count is tied to the laundry bit
	 */
	m->laundry = TRUE;
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
       vm_page_t       m)
{
       struct vm_pageout_queue *q;

       assert(m->object != VM_OBJECT_NULL);
       assert(m->object != kernel_object);

#if DEBUG
       lck_mtx_assert(&vm_page_queue_lock, LCK_MTX_ASSERT_OWNED);
       vm_object_lock_assert_exclusive(m->object);
#endif

       vm_pageout_throttle_up_count++;

       if (m->object->internal == TRUE)
               q = &vm_pageout_queue_internal;
       else
               q = &vm_pageout_queue_external;

       if (m->pageout_queue == TRUE) {

	       queue_remove(&q->pgo_pending, m, vm_page_t, pageq);
	       m->pageout_queue = FALSE;

	       m->pageq.next = NULL;
	       m->pageq.prev = NULL;

	       vm_object_activity_end(m->object);
       }
       if (m->laundry == TRUE) {

	       m->laundry = FALSE;
	       q->pgo_laundry--;

	       if (q->pgo_throttled == TRUE) {
		       q->pgo_throttled = FALSE;
                       thread_wakeup((event_t) &q->pgo_laundry);
               }
	       if (q->pgo_draining == TRUE && q->pgo_laundry == 0) {
		       q->pgo_draining = FALSE;
		       thread_wakeup((event_t) (&q->pgo_laundry+1));
	       }
	}
}


static void
vm_pageout_throttle_up_batch(
	struct vm_pageout_queue *q,
	int		batch_cnt)
{
#if DEBUG
       lck_mtx_assert(&vm_page_queue_lock, LCK_MTX_ASSERT_OWNED);
#endif

       vm_pageout_throttle_up_count += batch_cnt;

       q->pgo_laundry -= batch_cnt;

       if (q->pgo_throttled == TRUE) {
	       q->pgo_throttled = FALSE;
	       thread_wakeup((event_t) &q->pgo_laundry);
       }
       if (q->pgo_draining == TRUE && q->pgo_laundry == 0) {
	       q->pgo_draining = FALSE;
	       thread_wakeup((event_t) (&q->pgo_laundry+1));
       }
}



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

#if VM_PAGE_BUCKETS_CHECK
int vm_page_buckets_check_interval = 10; /* in seconds */
#endif /* VM_PAGE_BUCKETS_CHECK */

/*
 * Called from compute_averages().
 */
void
compute_memory_pressure(
	__unused void *arg)
{
	unsigned int vm_pageout_next;

#if VM_PAGE_BUCKETS_CHECK
	/* check the consistency of VM page buckets at regular interval */
	static int counter = 0;
	if ((++counter % vm_page_buckets_check_interval) == 0) {
		vm_page_buckets_check();
	}
#endif /* VM_PAGE_BUCKETS_CHECK */

	vm_memory_pressure =
		vm_pageout_stats[VM_PAGEOUT_STAT_BEFORE(vm_pageout_stat_now)].reclaimed;

	commpage_set_memory_pressure( vm_memory_pressure );

	/* move "now" forward */
	vm_pageout_next = VM_PAGEOUT_STAT_AFTER(vm_pageout_stat_now);
	vm_pageout_stats[vm_pageout_next].considered = 0;
	vm_pageout_stats[vm_pageout_next].reclaimed = 0;
	vm_pageout_stat_now = vm_pageout_next;
}


/*
 * IMPORTANT
 * mach_vm_ctl_page_free_wanted() is called indirectly, via
 * mach_vm_pressure_monitor(), when taking a stackshot. Therefore, 
 * it must be safe in the restricted stackshot context. Locks and/or 
 * blocking are not allowable.
 */
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


/*
 * IMPORTANT:
 * mach_vm_pressure_monitor() is called when taking a stackshot, with 
 * wait_for_pressure FALSE, so that code path must remain safe in the
 * restricted stackshot context. No blocking or locks are allowable.
 * on that code path.
 */

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



/*
 * function in BSD to apply I/O throttle to the pageout thread
 */
extern void vm_pageout_io_throttle(void);

/*
 * Page States: Used below to maintain the page state
 * before it's removed from it's Q. This saved state
 * helps us do the right accounting in certain cases
 */
#define PAGE_STATE_SPECULATIVE		1
#define PAGE_STATE_ANONYMOUS		2
#define PAGE_STATE_INACTIVE		3
#define PAGE_STATE_INACTIVE_FIRST	4
#define PAGE_STATE_CLEAN      5


#define VM_PAGEOUT_SCAN_HANDLE_REUSABLE_PAGE(m)                         \
        MACRO_BEGIN                                                     \
        /*                                                              \
         * If a "reusable" page somehow made it back into               \
         * the active queue, it's been re-used and is not               \
         * quite re-usable.                                             \
         * If the VM object was "all_reusable", consider it             \
         * as "all re-used" instead of converting it to                 \
         * "partially re-used", which could be expensive.               \
         */                                                             \
        if ((m)->reusable ||                                            \
            (m)->object->all_reusable) {                                \
                vm_object_reuse_pages((m)->object,                      \
                                      (m)->offset,                      \
                                      (m)->offset + PAGE_SIZE_64,       \
                                      FALSE);                           \
        }                                                               \
        MACRO_END


#define VM_PAGEOUT_DELAYED_UNLOCK_LIMIT  	64
#define VM_PAGEOUT_DELAYED_UNLOCK_LIMIT_MAX	1024

#define	FCS_IDLE		0
#define FCS_DELAYED		1
#define FCS_DEADLOCK_DETECTED	2

struct flow_control {
        int		state;
        mach_timespec_t	ts;
};

uint32_t vm_pageout_considered_page = 0;
uint32_t vm_page_filecache_min = 0;

#define	VM_PAGE_FILECACHE_MIN	50000
#define ANONS_GRABBED_LIMIT	2

/*
 *	vm_pageout_scan does the dirty work for the pageout daemon.
 *	It returns with both vm_page_queue_free_lock and vm_page_queue_lock
 *	held and vm_page_free_wanted == 0.
 */
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
	int	    delayed_unlock_limit = 0;
	int	    refmod_state = 0;
        int	vm_pageout_deadlock_target = 0;
	struct	vm_pageout_queue *iq;
	struct	vm_pageout_queue *eq;
        struct	vm_speculative_age_q *sq;
	struct  flow_control	flow_control = { 0, { 0, 0 } };
        boolean_t inactive_throttled = FALSE;
	boolean_t try_failed;
	mach_timespec_t	ts;
	unsigned	int msecs = 0;
	vm_object_t	object;
	vm_object_t	last_object_tried;
	uint32_t	catch_up_count = 0;
	uint32_t	inactive_reclaim_run;
	boolean_t	forced_reclaim;
	boolean_t	exceeded_burst_throttle;
	boolean_t	grab_anonymous = FALSE;
	boolean_t	force_anonymous = FALSE;
	int		anons_grabbed = 0;
	int		page_prev_state = 0;
	int		cache_evict_throttle = 0;
	uint32_t	vm_pageout_inactive_external_forced_reactivate_limit = 0;
	int		force_purge = 0;

#if VM_PRESSURE_EVENTS
	vm_pressure_level_t pressure_level;
#endif /* VM_PRESSURE_EVENTS */

	VM_DEBUG_EVENT(vm_pageout_scan, VM_PAGEOUT_SCAN, DBG_FUNC_START,
		       vm_pageout_speculative_clean, vm_pageout_inactive_clean,
		       vm_pageout_inactive_dirty_internal, vm_pageout_inactive_dirty_external);

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

	vm_pageout_inactive_external_forced_reactivate_limit = vm_page_active_count + vm_page_inactive_count;

	/*
	 *	We want to gradually dribble pages from the active queue
	 *	to the inactive queue.  If we let the inactive queue get
	 *	very small, and then suddenly dump many pages into it,
	 *	those pages won't get a sufficient chance to be referenced
	 *	before we start taking them from the inactive queue.
	 *
	 *	We must limit the rate at which we send pages to the pagers
	 *	so that we don't tie up too many pages in the I/O queues.
	 *	We implement a throttling mechanism using the laundry count
	 * 	to limit the number of pages outstanding to the default
	 *	and external pagers.  We can bypass the throttles and look
	 *	for clean pages if the pageout queues don't drain in a timely
	 *	fashion since this may indicate that the pageout paths are
	 *	stalled waiting for memory, which only we can provide.
	 */


Restart:
	assert(delayed_unlock!=0);
	
	/*
	 *	Recalculate vm_page_inactivate_target.
	 */
	vm_page_inactive_target = VM_PAGE_INACTIVE_TARGET(vm_page_active_count +
							  vm_page_inactive_count +
							  vm_page_speculative_count);

	vm_page_anonymous_min = vm_page_inactive_target / 20;


	/*
	 * don't want to wake the pageout_scan thread up everytime we fall below
	 * the targets... set a low water mark at 0.25% below the target
	 */
	vm_page_inactive_min = vm_page_inactive_target - (vm_page_inactive_target / 400);

	if (vm_page_speculative_percentage > 50)
		vm_page_speculative_percentage = 50;
	else if (vm_page_speculative_percentage <= 0)
		vm_page_speculative_percentage = 1;

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
		if (vm_upl_wait_for_pages < 0)
			vm_upl_wait_for_pages = 0;

		delayed_unlock_limit = VM_PAGEOUT_DELAYED_UNLOCK_LIMIT + vm_upl_wait_for_pages;

		if (delayed_unlock_limit > VM_PAGEOUT_DELAYED_UNLOCK_LIMIT_MAX)
			delayed_unlock_limit = VM_PAGEOUT_DELAYED_UNLOCK_LIMIT_MAX;

		/*
		 * Move pages from active to inactive if we're below the target
		 */
		/* if we are trying to make clean, we need to make sure we actually have inactive - mj */
		if ((vm_page_inactive_count + vm_page_speculative_count) >= vm_page_inactive_target)
			goto done_moving_active_pages;

		if (object != NULL) {
			vm_object_unlock(object);
			object = NULL;
			vm_pageout_scan_wants_object = VM_OBJECT_NULL;
		}
		/*
		 * Don't sweep through active queue more than the throttle
		 * which should be kept relatively low
		 */
		active_burst_count = MIN(vm_pageout_burst_active_throttle, vm_page_active_count);

		VM_DEBUG_EVENT(vm_pageout_balance, VM_PAGEOUT_BALANCE, DBG_FUNC_START,
			       vm_pageout_inactive, vm_pageout_inactive_used, vm_page_free_count, local_freed);

		VM_DEBUG_EVENT(vm_pageout_balance, VM_PAGEOUT_BALANCE, DBG_FUNC_NONE,
			       vm_pageout_speculative_clean, vm_pageout_inactive_clean,
			       vm_pageout_inactive_dirty_internal, vm_pageout_inactive_dirty_external);
		memoryshot(VM_PAGEOUT_BALANCE, DBG_FUNC_START);


		while (!queue_empty(&vm_page_queue_active) && active_burst_count--) {

			vm_pageout_active++;

			m = (vm_page_t) queue_first(&vm_page_queue_active);

			assert(m->active && !m->inactive);
			assert(!m->laundry);
			assert(m->object != kernel_object);
			assert(m->phys_page != vm_page_guard_addr);

			DTRACE_VM2(scan, int, 1, (uint64_t *), NULL);

			/*
			 * by not passing in a pmap_flush_context we will forgo any TLB flushing, local or otherwise...
			 *
			 * a TLB flush isn't really needed here since at worst we'll miss the reference bit being
			 * updated in the PTE if a remote processor still has this mapping cached in its TLB when the
			 * new reference happens. If no futher references happen on the page after that remote TLB flushes
			 * we'll see a clean, non-referenced page when it eventually gets pulled out of the inactive queue
			 * by pageout_scan, which is just fine since the last reference would have happened quite far
			 * in the past (TLB caches don't hang around for very long), and of course could just as easily
			 * have happened before we moved the page
			 */
			pmap_clear_refmod_options(m->phys_page, VM_MEM_REFERENCED, PMAP_OPTIONS_NOFLUSH, (void *)NULL);

			/*
			 * The page might be absent or busy,
			 * but vm_page_deactivate can handle that.
			 * FALSE indicates that we don't want a H/W clear reference
			 */
			vm_page_deactivate_internal(m, FALSE);

			if (delayed_unlock++ > delayed_unlock_limit) {

				if (local_freeq) {
					vm_page_unlock_queues();
					
					VM_DEBUG_EVENT(vm_pageout_freelist, VM_PAGEOUT_FREELIST, DBG_FUNC_START,
						       vm_page_free_count, local_freed, delayed_unlock_limit, 1);

					vm_page_free_list(local_freeq, TRUE);
						
					VM_DEBUG_EVENT(vm_pageout_freelist, VM_PAGEOUT_FREELIST, DBG_FUNC_END,
						       vm_page_free_count, 0, 0, 1);

					local_freeq = NULL;
					local_freed = 0;
					vm_page_lock_queues();
				} else {
					lck_mtx_yield(&vm_page_queue_lock);
				}
				
				delayed_unlock = 1;

				/*
				 * continue the while loop processing
				 * the active queue... need to hold
				 * the page queues lock
				 */
			}
		}

		VM_DEBUG_EVENT(vm_pageout_balance, VM_PAGEOUT_BALANCE, DBG_FUNC_END,
			       vm_page_active_count, vm_page_inactive_count, vm_page_speculative_count, vm_page_inactive_target);
		memoryshot(VM_PAGEOUT_BALANCE, DBG_FUNC_END);

		/**********************************************************************
		 * above this point we're playing with the active queue
		 * below this point we're playing with the throttling mechanisms
		 * and the inactive queue
		 **********************************************************************/

done_moving_active_pages:

		if (vm_page_free_count + local_freed >= vm_page_free_target) {
			if (object != NULL) {
			        vm_object_unlock(object);
				object = NULL;
			}
			vm_pageout_scan_wants_object = VM_OBJECT_NULL;

			if (local_freeq) {
				vm_page_unlock_queues();
					
				VM_DEBUG_EVENT(vm_pageout_freelist, VM_PAGEOUT_FREELIST, DBG_FUNC_START,
					       vm_page_free_count, local_freed, delayed_unlock_limit, 2);

				vm_page_free_list(local_freeq, TRUE);
					
				VM_DEBUG_EVENT(vm_pageout_freelist, VM_PAGEOUT_FREELIST, DBG_FUNC_END,
					       vm_page_free_count, local_freed, 0, 2);

				local_freeq = NULL;
				local_freed = 0;
				vm_page_lock_queues();
			}
			/*
			 * make sure the pageout I/O threads are running
			 * throttled in case there are still requests 
			 * in the laundry... since we have met our targets
			 * we don't need the laundry to be cleaned in a timely
			 * fashion... so let's avoid interfering with foreground
			 * activity
			 */
			vm_pageout_adjust_io_throttles(iq, eq, TRUE);

			/*
			 * recalculate vm_page_inactivate_target
			 */
			vm_page_inactive_target = VM_PAGE_INACTIVE_TARGET(vm_page_active_count +
									  vm_page_inactive_count +
									  vm_page_speculative_count);
			if (((vm_page_inactive_count + vm_page_speculative_count) < vm_page_inactive_target) &&
			    !queue_empty(&vm_page_queue_active)) {
				/*
				 * inactive target still not met... keep going
				 * until we get the queues balanced...
				 */
			        continue;
			}
		        lck_mtx_lock(&vm_page_queue_free_lock);

			if ((vm_page_free_count >= vm_page_free_target) &&
			    (vm_page_free_wanted == 0) && (vm_page_free_wanted_privileged == 0)) {
				/*
				 * done - we have met our target *and*
				 * there is no one waiting for a page.
				 */
return_from_scan:
				assert(vm_pageout_scan_wants_object == VM_OBJECT_NULL);

				VM_DEBUG_EVENT(vm_pageout_scan, VM_PAGEOUT_SCAN, DBG_FUNC_NONE,
					       vm_pageout_inactive, vm_pageout_inactive_used, 0, 0);
				VM_DEBUG_EVENT(vm_pageout_scan, VM_PAGEOUT_SCAN, DBG_FUNC_END,
					       vm_pageout_speculative_clean, vm_pageout_inactive_clean,
					       vm_pageout_inactive_dirty_internal, vm_pageout_inactive_dirty_external);

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
		force_purge = 0; /* no force-purging */

#if VM_PRESSURE_EVENTS
		pressure_level = memorystatus_vm_pressure_level;

		if (pressure_level > kVMPressureNormal) {

			if (pressure_level >= kVMPressureCritical) {
				force_purge = memorystatus_purge_on_critical;
			} else if (pressure_level >= kVMPressureUrgent) {
				force_purge = memorystatus_purge_on_urgent;
			} else if (pressure_level >= kVMPressureWarning) {
				force_purge = memorystatus_purge_on_warning;
			}
		}
#endif /* VM_PRESSURE_EVENTS */

		if (available_for_purge || force_purge) {

		        if (object != NULL) {
			        vm_object_unlock(object);
				object = NULL;
			}

			memoryshot(VM_PAGEOUT_PURGEONE, DBG_FUNC_START);

			VM_DEBUG_EVENT(vm_pageout_purgeone, VM_PAGEOUT_PURGEONE, DBG_FUNC_START, vm_page_free_count, 0, 0, 0);
			if (vm_purgeable_object_purge_one(force_purge, C_DONT_BLOCK)) {

				VM_DEBUG_EVENT(vm_pageout_purgeone, VM_PAGEOUT_PURGEONE, DBG_FUNC_END, vm_page_free_count, 0, 0, 0);
				memoryshot(VM_PAGEOUT_PURGEONE, DBG_FUNC_END);
				continue;
			}
			VM_DEBUG_EVENT(vm_pageout_purgeone, VM_PAGEOUT_PURGEONE, DBG_FUNC_END, 0, 0, 0, -1);
			memoryshot(VM_PAGEOUT_PURGEONE, DBG_FUNC_END);
		}

		if (queue_empty(&sq->age_q) && vm_page_speculative_count) {
		        /*
			 * try to pull pages from the aging bins...
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

			if (num_scanned_queues == VM_PAGE_MAX_SPECULATIVE_AGE_Q + 1) {
				/*
				 * XXX We've scanned all the speculative
				 * queues but still haven't found one
				 * that is not empty, even though
				 * vm_page_speculative_count is not 0.
				 *
				 * report the anomaly...
				 */
				printf("vm_pageout_scan: "
				       "all speculative queues empty "
				       "but count=%d.  Re-adjusting.\n",
				       vm_page_speculative_count);
				if (vm_page_speculative_count > vm_page_speculative_count_drift_max)
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
			        ts_fully_aged.tv_sec = (VM_PAGE_MAX_SPECULATIVE_AGE_Q * vm_page_speculative_q_age_ms) / 1000;
				ts_fully_aged.tv_nsec = ((VM_PAGE_MAX_SPECULATIVE_AGE_Q * vm_page_speculative_q_age_ms) % 1000)
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
		if (queue_empty(&sq->age_q) && cache_evict_throttle == 0) {
			int 	pages_evicted;

		        if (object != NULL) {
			        vm_object_unlock(object);
				object = NULL;
			}
			pages_evicted = vm_object_cache_evict(100, 10);

			if (pages_evicted) {

				vm_pageout_cache_evicted += pages_evicted;

				VM_DEBUG_EVENT(vm_pageout_cache_evict, VM_PAGEOUT_CACHE_EVICT, DBG_FUNC_NONE,
					       vm_page_free_count, pages_evicted, vm_pageout_cache_evicted, 0);
				memoryshot(VM_PAGEOUT_CACHE_EVICT, DBG_FUNC_NONE);

				/*
				 * we just freed up to 100 pages,
				 * so go back to the top of the main loop
				 * and re-evaulate the memory situation
				 */
				continue;
			} else
				cache_evict_throttle = 100;
		}
		if  (cache_evict_throttle)
			cache_evict_throttle--;


		exceeded_burst_throttle = FALSE;
		/*
		 * Sometimes we have to pause:
		 *	1) No inactive pages - nothing to do.
		 *	2) Loop control - no acceptable pages found on the inactive queue
		 *         within the last vm_pageout_burst_inactive_throttle iterations
		 *	3) Flow control - default pageout queue is full
		 */
		if (queue_empty(&vm_page_queue_inactive) && queue_empty(&vm_page_queue_anonymous) && queue_empty(&sq->age_q)) {
		        vm_pageout_scan_empty_throttle++;
			msecs = vm_pageout_empty_wait;
			goto vm_pageout_scan_delay;

		} else if (inactive_burst_count >= 
			   MIN(vm_pageout_burst_inactive_throttle,
			       (vm_page_inactive_count +
				vm_page_speculative_count))) {
		        vm_pageout_scan_burst_throttle++;
			msecs = vm_pageout_burst_wait;

			exceeded_burst_throttle = TRUE;
			goto vm_pageout_scan_delay;

		} else if (vm_page_free_count > (vm_page_free_reserved / 4) &&
			   VM_PAGEOUT_SCAN_NEEDS_TO_THROTTLE()) {
		        vm_pageout_scan_swap_throttle++;
			msecs = vm_pageout_swap_wait;
			goto vm_pageout_scan_delay;

		} else if (VM_PAGE_Q_THROTTLED(iq) && 
				  VM_DYNAMIC_PAGING_ENABLED(memory_manager_default)) {
			clock_sec_t sec;
			clock_nsec_t nsec;

		        switch (flow_control.state) {

			case FCS_IDLE:
				if ((vm_page_free_count + local_freed) < vm_page_free_target) {

					if (vm_page_pageable_external_count > vm_page_filecache_min && !queue_empty(&vm_page_queue_inactive)) {
						anons_grabbed = ANONS_GRABBED_LIMIT;
						goto consider_inactive;
					}
					if (((vm_page_inactive_count + vm_page_speculative_count) < vm_page_inactive_target) && vm_page_active_count)
						continue;
				}
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
vm_pageout_scan_delay:
			if (object != NULL) {
			        vm_object_unlock(object);
				object = NULL;
			}
			vm_pageout_scan_wants_object = VM_OBJECT_NULL;

			vm_page_unlock_queues();

			if (local_freeq) {

				VM_DEBUG_EVENT(vm_pageout_freelist, VM_PAGEOUT_FREELIST, DBG_FUNC_START,
					       vm_page_free_count, local_freed, delayed_unlock_limit, 3);

				vm_page_free_list(local_freeq, TRUE);
					
				VM_DEBUG_EVENT(vm_pageout_freelist, VM_PAGEOUT_FREELIST, DBG_FUNC_END,
					       vm_page_free_count, local_freed, 0, 3);

				local_freeq = NULL;
				local_freed = 0;
			}
			if (COMPRESSED_PAGER_IS_ACTIVE || DEFAULT_FREEZER_COMPRESSED_PAGER_IS_ACTIVE)
				vm_consider_waking_compactor_swapper();

			vm_page_lock_queues();

			if (flow_control.state == FCS_DELAYED &&
			    !VM_PAGE_Q_THROTTLED(iq)) {
				flow_control.state = FCS_IDLE;
				goto consider_inactive;
			}
			
			if (vm_page_free_count >= vm_page_free_target) {
				/*
				 * we're here because
				 *  1) someone else freed up some pages while we had
				 *     the queues unlocked above
				 * and we've hit one of the 3 conditions that 
				 * cause us to pause the pageout scan thread
				 *
				 * since we already have enough free pages,
				 * let's avoid stalling and return normally
				 *
				 * before we return, make sure the pageout I/O threads
				 * are running throttled in case there are still requests 
				 * in the laundry... since we have enough free pages
				 * we don't need the laundry to be cleaned in a timely
				 * fashion... so let's avoid interfering with foreground
				 * activity
				 *
				 * we don't want to hold vm_page_queue_free_lock when
				 * calling vm_pageout_adjust_io_throttles (since it
				 * may cause other locks to be taken), we do the intitial
				 * check outside of the lock.  Once we take the lock,
				 * we recheck the condition since it may have changed.
				 * if it has, no problem, we will make the threads
				 * non-throttled before actually blocking
				 */
				vm_pageout_adjust_io_throttles(iq, eq, TRUE);
			}
			lck_mtx_lock(&vm_page_queue_free_lock);

			if (vm_page_free_count >= vm_page_free_target &&
			    (vm_page_free_wanted == 0) && (vm_page_free_wanted_privileged == 0)) {
				goto return_from_scan;
			}
			lck_mtx_unlock(&vm_page_queue_free_lock);
			
			if ((vm_page_free_count + vm_page_cleaned_count) < vm_page_free_target) {
				/*
				 * we're most likely about to block due to one of
				 * the 3 conditions that cause vm_pageout_scan to
				 * not be able to make forward progress w/r
				 * to providing new pages to the free queue,
				 * so unthrottle the I/O threads in case we
				 * have laundry to be cleaned... it needs
				 * to be completed ASAP.
				 *
				 * even if we don't block, we want the io threads
				 * running unthrottled since the sum of free +
				 * clean pages is still under our free target
				 */
				vm_pageout_adjust_io_throttles(iq, eq, FALSE);
			}
			if (vm_page_cleaned_count > 0 && exceeded_burst_throttle == FALSE) {
				/*
				 * if we get here we're below our free target and
				 * we're stalling due to a full laundry queue or
				 * we don't have any inactive pages other then
				 * those in the clean queue...
				 * however, we have pages on the clean queue that
				 * can be moved to the free queue, so let's not
				 * stall the pageout scan
				 */
				flow_control.state = FCS_IDLE;
				goto consider_inactive;
			}
			VM_CHECK_MEMORYSTATUS;

			if (flow_control.state != FCS_IDLE)
				vm_pageout_scan_throttle++;
			iq->pgo_throttled = TRUE;

			if (COMPRESSED_PAGER_IS_ACTIVE || DEFAULT_FREEZER_COMPRESSED_PAGER_IS_ACTIVE)
				vm_consider_waking_compactor_swapper();

			assert_wait_timeout((event_t) &iq->pgo_laundry, THREAD_INTERRUPTIBLE, msecs, 1000*NSEC_PER_USEC);
			counter(c_vm_pageout_scan_block++);

			vm_page_unlock_queues();

			assert(vm_pageout_scan_wants_object == VM_OBJECT_NULL);

			VM_DEBUG_EVENT(vm_pageout_thread_block, VM_PAGEOUT_THREAD_BLOCK, DBG_FUNC_START, 
				       iq->pgo_laundry, iq->pgo_maxlaundry, msecs, 0);
			memoryshot(VM_PAGEOUT_THREAD_BLOCK, DBG_FUNC_START);

			thread_block(THREAD_CONTINUE_NULL);

			VM_DEBUG_EVENT(vm_pageout_thread_block, VM_PAGEOUT_THREAD_BLOCK, DBG_FUNC_END,
				       iq->pgo_laundry, iq->pgo_maxlaundry, msecs, 0);
			memoryshot(VM_PAGEOUT_THREAD_BLOCK, DBG_FUNC_END);

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
		vm_pageout_inactive_external_forced_reactivate_limit = MIN((vm_page_active_count + vm_page_inactive_count), 
									    vm_pageout_inactive_external_forced_reactivate_limit);
		loop_count++;
		inactive_burst_count++;
		vm_pageout_inactive++;


		/*
		 * Choose a victim.
		 */
		while (1) {
			m = NULL;
			
			if (VM_DYNAMIC_PAGING_ENABLED(memory_manager_default)) {
				assert(vm_page_throttled_count == 0);
				assert(queue_empty(&vm_page_queue_throttled));
			}
			/*
			 * The most eligible pages are ones we paged in speculatively,
			 * but which have not yet been touched.
			 */
			if (!queue_empty(&sq->age_q) && force_anonymous == FALSE) {
				m = (vm_page_t) queue_first(&sq->age_q);

				page_prev_state = PAGE_STATE_SPECULATIVE;

				break;
			}
			/*
			 * Try a clean-queue inactive page.
			 */
			if (!queue_empty(&vm_page_queue_cleaned)) {
				m = (vm_page_t) queue_first(&vm_page_queue_cleaned);
                    
				page_prev_state = PAGE_STATE_CLEAN;
                    
				break;
			}

			grab_anonymous = (vm_page_anonymous_count > vm_page_anonymous_min);

			if (vm_page_pageable_external_count < vm_page_filecache_min || force_anonymous == TRUE) {
				grab_anonymous = TRUE;
				anons_grabbed = 0;
			}

			if (grab_anonymous == FALSE || anons_grabbed >= ANONS_GRABBED_LIMIT || queue_empty(&vm_page_queue_anonymous)) {

				if ( !queue_empty(&vm_page_queue_inactive) ) {
					m = (vm_page_t) queue_first(&vm_page_queue_inactive);
				
					page_prev_state = PAGE_STATE_INACTIVE;
					anons_grabbed = 0;

					break;
				}
			}
			if ( !queue_empty(&vm_page_queue_anonymous) ) {
				m = (vm_page_t) queue_first(&vm_page_queue_anonymous);

				page_prev_state = PAGE_STATE_ANONYMOUS;
				anons_grabbed++;

				break;
			}

			/*
			 * if we've gotten here, we have no victim page.
			 * if making clean, free the local freed list and return.
			 * if making free, check to see if we've finished balancing the queues
			 * yet, if we haven't just continue, else panic
			 */
			vm_page_unlock_queues();
				
			if (object != NULL) {
				vm_object_unlock(object);
				object = NULL;
			}
			vm_pageout_scan_wants_object = VM_OBJECT_NULL;
				
			if (local_freeq) {
				VM_DEBUG_EVENT(vm_pageout_freelist, VM_PAGEOUT_FREELIST, DBG_FUNC_START,
					       vm_page_free_count, local_freed, delayed_unlock_limit, 5);
					
				vm_page_free_list(local_freeq, TRUE);
					
				VM_DEBUG_EVENT(vm_pageout_freelist, VM_PAGEOUT_FREELIST, DBG_FUNC_END,
					       vm_page_free_count, local_freed, 0, 5);
					
				local_freeq = NULL;
				local_freed = 0;
			}
			vm_page_lock_queues();
			delayed_unlock = 1;

			force_anonymous = FALSE;

			if ((vm_page_inactive_count + vm_page_speculative_count) < vm_page_inactive_target)
				goto Restart;

			if (!queue_empty(&sq->age_q))
				goto Restart;

			panic("vm_pageout: no victim");
			
			/* NOTREACHED */
		}
		force_anonymous = FALSE;
		
		/*
		 * we just found this page on one of our queues...
		 * it can't also be on the pageout queue, so safe
		 * to call VM_PAGE_QUEUES_REMOVE
		 */
		assert(!m->pageout_queue);

		VM_PAGE_QUEUES_REMOVE(m);

		assert(!m->laundry);
		assert(!m->private);
		assert(!m->fictitious);
		assert(m->object != kernel_object);
		assert(m->phys_page != vm_page_guard_addr);


		if (page_prev_state != PAGE_STATE_SPECULATIVE)
			vm_pageout_stats[vm_pageout_stat_now].considered++;

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
				vm_page_t m_want = NULL;

				vm_pageout_inactive_nolock++;

				if (page_prev_state == PAGE_STATE_CLEAN)
					vm_pageout_cleaned_nolock++;

				if (page_prev_state == PAGE_STATE_SPECULATIVE)
					page_prev_state = PAGE_STATE_INACTIVE_FIRST;

				pmap_clear_reference(m->phys_page);
				m->reference = FALSE;

				/*
				 * m->object must be stable since we hold the page queues lock...
				 * we can update the scan_collisions field sans the object lock
				 * since it is a separate field and this is the only spot that does
				 * a read-modify-write operation and it is never executed concurrently...
				 * we can asynchronously set this field to 0 when creating a UPL, so it
				 * is possible for the value to be a bit non-determistic, but that's ok
				 * since it's only used as a hint
				 */
				m->object->scan_collisions++;

				if ( !queue_empty(&sq->age_q) )
					m_want = (vm_page_t) queue_first(&sq->age_q);
				else if ( !queue_empty(&vm_page_queue_cleaned))
					m_want = (vm_page_t) queue_first(&vm_page_queue_cleaned);
				else if (anons_grabbed >= ANONS_GRABBED_LIMIT || queue_empty(&vm_page_queue_anonymous))
					m_want = (vm_page_t) queue_first(&vm_page_queue_inactive);
				else if ( !queue_empty(&vm_page_queue_anonymous))
					m_want = (vm_page_t) queue_first(&vm_page_queue_anonymous);

				/*
				 * this is the next object we're going to be interested in
				 * try to make sure its available after the mutex_yield
				 * returns control
				 */
				if (m_want)
					vm_pageout_scan_wants_object = m_want->object;

				/*
				 * force us to dump any collected free pages
				 * and to pause before moving on
				 */
				try_failed = TRUE;

				goto requeue_page;
			}
			object = m->object;
			vm_pageout_scan_wants_object = VM_OBJECT_NULL;

			try_failed = FALSE;
		}
		if (catch_up_count)
		        catch_up_count--;

		if (m->busy) {
			if (m->encrypted_cleaning) {
				/*
				 * ENCRYPTED SWAP:
				 * if this page has already been picked up as
				 * part of a page-out cluster, it will be busy 
				 * because it is being encrypted (see
				 * vm_object_upl_request()).  But we still
				 * want to demote it from "clean-in-place"
				 * (aka "adjacent") to "clean-and-free" (aka
				 * "target"), so let's ignore its "busy" bit
				 * here and proceed to check for "cleaning" a
				 * little bit below...
				 *
				 * CAUTION CAUTION:
				 * A "busy" page should still be left alone for
				 * most purposes, so we have to be very careful
				 * not to process that page too much.
				 */
				assert(m->cleaning);
				goto consider_inactive_page;
			}

			/*
			 *	Somebody is already playing with this page.
			 *	Put it back on the appropriate queue
			 *
			 */
			vm_pageout_inactive_busy++;

			if (page_prev_state == PAGE_STATE_CLEAN)
				vm_pageout_cleaned_busy++;
			
requeue_page:
			switch (page_prev_state) {

			case PAGE_STATE_SPECULATIVE:
				vm_page_speculate(m, FALSE);
				break;

			case PAGE_STATE_ANONYMOUS:
			case PAGE_STATE_CLEAN:
			case PAGE_STATE_INACTIVE:
				VM_PAGE_ENQUEUE_INACTIVE(m, FALSE);
				break;

			case PAGE_STATE_INACTIVE_FIRST:
				VM_PAGE_ENQUEUE_INACTIVE(m, TRUE);
				break;
			}
			goto done_with_inactivepage;
		}


		/*
		 *	If it's absent, in error or the object is no longer alive,
		 *	we can reclaim the page... in the no longer alive case,
		 *	there are 2 states the page can be in that preclude us
		 *	from reclaiming it - busy or cleaning - that we've already
		 *	dealt with
		 */
		if (m->absent || m->error || !object->alive) {

			if (m->absent)
				vm_pageout_inactive_absent++;
			else if (!object->alive)
				vm_pageout_inactive_notalive++;
			else
				vm_pageout_inactive_error++;
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
			assert(!m->cleaning);
			assert(!m->laundry);

			m->busy = TRUE;

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
			
			if (page_prev_state == PAGE_STATE_SPECULATIVE)
				vm_pageout_freed_from_speculative++;
			else if (page_prev_state == PAGE_STATE_CLEAN)
				vm_pageout_freed_from_cleaned++;
			else
				vm_pageout_freed_from_inactive_clean++;

			if (page_prev_state != PAGE_STATE_SPECULATIVE)
				vm_pageout_stats[vm_pageout_stat_now].reclaimed++;

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
				if (m->pmapped == TRUE) {
					/* unmap the page */
					refmod_state = pmap_disconnect(m->phys_page);
					if (refmod_state & VM_MEM_MODIFIED) {
						SET_PAGE_DIRTY(m, FALSE);
					}
				}
				if (m->dirty || m->precious) {
					/* we saved the cost of cleaning this page ! */
					vm_page_purged_count++;
				}
				goto reclaim_page;
			}

			if (COMPRESSED_PAGER_IS_ACTIVE) {
				/*
				 * With the VM compressor, the cost of
				 * reclaiming a page is much lower (no I/O),
				 * so if we find a "volatile" page, it's better
				 * to let it get compressed rather than letting
				 * it occupy a full page until it gets purged.
				 * So no need to check for "volatile" here.
				 */
			} else if (object->purgable == VM_PURGABLE_VOLATILE) {
				/*
				 * Avoid cleaning a "volatile" page which might
				 * be purged soon.
				 */

				/* if it's wired, we can't put it on our queue */
				assert(!VM_PAGE_WIRED(m));

				/* just stick it back on! */
				reactivated_this_call++;

				if (page_prev_state == PAGE_STATE_CLEAN)
					vm_pageout_cleaned_volatile_reactivated++;

				goto reactivate_page;
			}
		}

consider_inactive_page:
		if (m->busy) {
			/*
			 * CAUTION CAUTION:
			 * A "busy" page should always be left alone, except...
			 */
			if (m->cleaning && m->encrypted_cleaning) {
				/*
				 * ENCRYPTED_SWAP:
				 * We could get here with a "busy" page 
				 * if it's being encrypted during a
				 * "clean-in-place" operation.  We'll deal
				 * with it right away by testing if it has been
				 * referenced and either reactivating it or
				 * promoting it from "clean-in-place" to
				 * "clean-and-free".
				 */
			} else {
				panic("\"busy\" page considered for pageout\n");
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
		        if (refmod_state & VM_MEM_MODIFIED) {
				SET_PAGE_DIRTY(m, FALSE);
			}
		}
		
		/*
		 *   if (m->cleaning && !m->pageout)
		 *	If already cleaning this page in place and it hasn't
		 *	been recently referenced, just pull off the queue.
		 *	We can leave the page mapped, and upl_commit_range
		 *	will put it on the clean queue.
		 *
		 *	note: if m->encrypted_cleaning == TRUE, then
		 *		m->cleaning == TRUE
		 *	and we'll handle it here
		 *
		 *   if (m->pageout && !m->cleaning)
		 *	an msync INVALIDATE is in progress...
		 *	this page has been marked for destruction
		 * 	after it has been cleaned,
		 * 	but not yet gathered into a UPL
		 *	where 'cleaning' will be set...
		 *	just leave it off the paging queues
		 *
		 *   if (m->pageout && m->clenaing)
		 *	an msync INVALIDATE is in progress
		 *	and the UPL has already gathered this page...
		 *	just leave it off the paging queues
		 */
		
		/*
		 * page with m->pageout and still on the queues means that an
		 * MS_INVALIDATE is in progress on this page... leave it alone
		 */
		if (m->pageout) {
			goto done_with_inactivepage;
		}
		
		/* if cleaning, reactivate if referenced.  otherwise, just pull off queue */
		if (m->cleaning) {
			if (m->reference == TRUE) {
				reactivated_this_call++;
				goto reactivate_page;
			} else {
				goto done_with_inactivepage;
			}
		}

                if (m->reference || m->dirty) {
                        /* deal with a rogue "reusable" page */
                        VM_PAGEOUT_SCAN_HANDLE_REUSABLE_PAGE(m);
                }

		if (!m->no_cache &&
		    (m->reference ||
		     (m->xpmapped && !object->internal && (vm_page_xpmapped_external_count < (vm_page_external_count / 4))))) {
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

				if (page_prev_state == PAGE_STATE_CLEAN)
					vm_pageout_cleaned_reference_reactivated++;
				
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
					inactive_burst_count = 0;
				}
				
				if (page_prev_state == PAGE_STATE_CLEAN)
					vm_pageout_cleaned_reactivated++;

				vm_pageout_inactive_used++;

                                goto done_with_inactivepage;
			}
			/* 
			 * Make sure we call pmap_get_refmod() if it
			 * wasn't already called just above, to update
			 * the dirty bit.
			 */
			if ((refmod_state == -1) && !m->dirty && m->pmapped) {
				refmod_state = pmap_get_refmod(m->phys_page);
				if (refmod_state & VM_MEM_MODIFIED) {
					SET_PAGE_DIRTY(m, FALSE);
				}
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
throttle_inactive:
		if (!VM_DYNAMIC_PAGING_ENABLED(memory_manager_default) &&
		    object->internal && m->dirty &&
		    (object->purgable == VM_PURGABLE_DENY ||
		     object->purgable == VM_PURGABLE_NONVOLATILE ||
		     object->purgable == VM_PURGABLE_VOLATILE)) {
			queue_enter(&vm_page_queue_throttled, m,
				    vm_page_t, pageq);
			m->throttled = TRUE;
			vm_page_throttled_count++;

			vm_pageout_scan_reclaimed_throttled++;

			inactive_burst_count = 0;
			goto done_with_inactivepage;
		}
		if (inactive_throttled == TRUE) {

			if (object->internal == FALSE) {
                                /*
				 * we need to break up the following potential deadlock case...
				 *  a) The external pageout thread is stuck on the truncate lock for a file that is being extended i.e. written.
				 *  b) The thread doing the writing is waiting for pages while holding the truncate lock
				 *  c) Most of the pages in the inactive queue belong to this file.
				 *
				 * we are potentially in this deadlock because...
				 *  a) the external pageout queue is throttled
				 *  b) we're done with the active queue and moved on to the inactive queue
				 *  c) we've got a dirty external page
				 *
				 * since we don't know the reason for the external pageout queue being throttled we
				 * must suspect that we are deadlocked, so move the current page onto the active queue
				 * in an effort to cause a page from the active queue to 'age' to the inactive queue
				 *
				 * if we don't have jetsam configured (i.e. we have a dynamic pager), set
				 * 'force_anonymous' to TRUE to cause us to grab a page from the cleaned/anonymous
				 * pool the next time we select a victim page... if we can make enough new free pages,
				 * the deadlock will break, the external pageout queue will empty and it will no longer
				 * be throttled
				 *
				 * if we have jestam configured, keep a count of the pages reactivated this way so
				 * that we can try to find clean pages in the active/inactive queues before
				 * deciding to jetsam a process
				 */
				vm_pageout_scan_inactive_throttled_external++;			

				queue_enter(&vm_page_queue_active, m, vm_page_t, pageq);
				m->active = TRUE;
				vm_page_active_count++;
				vm_page_pageable_external_count++;

				vm_pageout_adjust_io_throttles(iq, eq, FALSE);

#if CONFIG_MEMORYSTATUS && CONFIG_JETSAM
				vm_pageout_inactive_external_forced_reactivate_limit--;

				if (vm_pageout_inactive_external_forced_reactivate_limit <= 0) {
					vm_pageout_inactive_external_forced_reactivate_limit = vm_page_active_count + vm_page_inactive_count;
					/*
					 * Possible deadlock scenario so request jetsam action
					 */
					assert(object);
					vm_object_unlock(object);
					object = VM_OBJECT_NULL;
					vm_page_unlock_queues();
					
					VM_DEBUG_EVENT(vm_pageout_jetsam, VM_PAGEOUT_JETSAM, DBG_FUNC_START,
    					       vm_page_active_count, vm_page_inactive_count, vm_page_free_count, vm_page_free_count);

                                        /* Kill first suitable process */
					if (memorystatus_kill_on_VM_page_shortage(FALSE) == FALSE) {
						panic("vm_pageout_scan: Jetsam request failed\n");	
					}
					
					VM_DEBUG_EVENT(vm_pageout_jetsam, VM_PAGEOUT_JETSAM, DBG_FUNC_END, 0, 0, 0, 0);

					vm_pageout_inactive_external_forced_jetsam_count++;
					vm_page_lock_queues();	
					delayed_unlock = 1;
				}
#else /* CONFIG_MEMORYSTATUS && CONFIG_JETSAM */
				force_anonymous = TRUE;
#endif
				inactive_burst_count = 0;
				goto done_with_inactivepage;
			} else {
				if (page_prev_state == PAGE_STATE_SPECULATIVE)
					page_prev_state = PAGE_STATE_INACTIVE;

				vm_pageout_scan_inactive_throttled_internal++;

				goto requeue_page;
			}
		}

		/*
		 * we've got a page that we can steal...
		 * eliminate all mappings and make sure
		 * we have the up-to-date modified state
		 *
		 * if we need to do a pmap_disconnect then we
		 * need to re-evaluate m->dirty since the pmap_disconnect
		 * provides the true state atomically... the 
		 * page was still mapped up to the pmap_disconnect
		 * and may have been dirtied at the last microsecond
		 *
		 * Note that if 'pmapped' is FALSE then the page is not
		 * and has not been in any map, so there is no point calling
		 * pmap_disconnect().  m->dirty could have been set in anticipation
		 * of likely usage of the page.
		 */
		if (m->pmapped == TRUE) {

			if (DEFAULT_PAGER_IS_ACTIVE || DEFAULT_FREEZER_IS_ACTIVE || DEFAULT_FREEZER_COMPRESSED_PAGER_IS_ACTIVE || object->internal == FALSE) {
				/*
				 * Don't count this page as going into the compressor if any of these are true:
				 * 1) We have the dynamic pager i.e. no compressed pager
				 * 2) Freezer enabled device with a freezer file to hold the app data i.e. no compressed pager
				 * 3) Freezer enabled device with compressed pager backend (exclusive use) i.e. most of the VM system
				      (including vm_pageout_scan) has no knowledge of the compressor
				 * 4) This page belongs to a file and hence will not be sent into the compressor
				 */

		        	refmod_state = pmap_disconnect_options(m->phys_page, 0, NULL);
			} else {
				refmod_state = pmap_disconnect_options(m->phys_page, PMAP_OPTIONS_COMPRESSOR, NULL);
			}

			if (refmod_state & VM_MEM_MODIFIED) {
				SET_PAGE_DIRTY(m, FALSE);
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

			if (page_prev_state == PAGE_STATE_SPECULATIVE)
				vm_pageout_speculative_clean++;
			else {
				if (page_prev_state == PAGE_STATE_ANONYMOUS)
					vm_pageout_inactive_anonymous++;
				else if (page_prev_state == PAGE_STATE_CLEAN)
					vm_pageout_cleaned_reclaimed++;

				vm_pageout_inactive_clean++;
			}

			/*
			 * OK, at this point we have found a page we are going to free.
			 */
#if CONFIG_PHANTOM_CACHE
			if (!object->internal)
				vm_phantom_cache_add_ghost(m);
#endif
			goto reclaim_page;
		}

		/*
		 * The page may have been dirtied since the last check
		 * for a throttled target queue (which may have been skipped
		 * if the page was clean then).  With the dirty page
		 * disconnected here, we can make one final check.
		 */
		if (object->internal) {
			if (VM_PAGE_Q_THROTTLED(iq))
				inactive_throttled = TRUE;
		} else if (VM_PAGE_Q_THROTTLED(eq)) {
			inactive_throttled = TRUE;
		}

		if (inactive_throttled == TRUE)
			goto throttle_inactive;
	
#if VM_PRESSURE_EVENTS
#if CONFIG_JETSAM

		/*
		 * If Jetsam is enabled, then the sending
		 * of memory pressure notifications is handled
		 * from the same thread that takes care of high-water
		 * and other jetsams i.e. the memorystatus_thread.
		 */

#else /* CONFIG_JETSAM */
		
		vm_pressure_response();

#endif /* CONFIG_JETSAM */
#endif /* VM_PRESSURE_EVENTS */
		
		/*
		 * do NOT set the pageout bit!
		 * sure, we might need free pages, but this page is going to take time to become free 
		 * anyway, so we may as well put it on the clean queue first and take it from there later
		 * if necessary.  that way, we'll ensure we don't free up too much. -mj
		 */
		vm_pageout_cluster(m, FALSE);

		if (page_prev_state == PAGE_STATE_ANONYMOUS)
			vm_pageout_inactive_anonymous++;
		if (object->internal)
			vm_pageout_inactive_dirty_internal++;
		else
			vm_pageout_inactive_dirty_external++;


done_with_inactivepage:

		if (delayed_unlock++ > delayed_unlock_limit || try_failed == TRUE) {
			boolean_t	need_delay = TRUE;

		        if (object != NULL) {
				vm_pageout_scan_wants_object = VM_OBJECT_NULL;
			        vm_object_unlock(object);
				object = NULL;
			}
			vm_page_unlock_queues();

		        if (local_freeq) {

				VM_DEBUG_EVENT(vm_pageout_freelist, VM_PAGEOUT_FREELIST, DBG_FUNC_START,
					       vm_page_free_count, local_freed, delayed_unlock_limit, 4);
					
				vm_page_free_list(local_freeq, TRUE);
				
				VM_DEBUG_EVENT(vm_pageout_freelist, VM_PAGEOUT_FREELIST, DBG_FUNC_END,
					       vm_page_free_count, local_freed, 0, 4);

				local_freeq = NULL;
				local_freed = 0;
				need_delay = FALSE;
			}
			if (COMPRESSED_PAGER_IS_ACTIVE || DEFAULT_FREEZER_COMPRESSED_PAGER_IS_ACTIVE) {
				vm_consider_waking_compactor_swapper();
				need_delay = FALSE;
			}
			vm_page_lock_queues();

			if (need_delay == TRUE)
				lck_mtx_yield(&vm_page_queue_lock);

			delayed_unlock = 1;
		}
		vm_pageout_considered_page++;

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

	if (COMPRESSED_PAGER_IS_ACTIVE || DEFAULT_FREEZER_COMPRESSED_PAGER_IS_ACTIVE) {

		if ((vm_page_free_reserved + pages + COMPRESSOR_FREE_RESERVED_LIMIT) >= (VM_PAGE_FREE_RESERVED_LIMIT + COMPRESSOR_FREE_RESERVED_LIMIT))
			vm_page_free_reserved = VM_PAGE_FREE_RESERVED_LIMIT + COMPRESSOR_FREE_RESERVED_LIMIT;
		else
			vm_page_free_reserved += (pages + COMPRESSOR_FREE_RESERVED_LIMIT);

	} else {
		if ((vm_page_free_reserved + pages) >= VM_PAGE_FREE_RESERVED_LIMIT)
			vm_page_free_reserved = VM_PAGE_FREE_RESERVED_LIMIT;
		else
			vm_page_free_reserved += pages;
	}
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
	vm_page_creation_throttle = vm_page_free_target * 3;
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
	/*
	 * we hold both the vm_page_queue_free_lock
	 * and the vm_page_queues_lock at this point
	 */
	assert(vm_page_free_wanted == 0);
	assert(vm_page_free_wanted_privileged == 0);
	assert_wait((event_t) &vm_page_free_wanted, THREAD_UNINT);

	lck_mtx_unlock(&vm_page_queue_free_lock);
	vm_page_unlock_queues();

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
	vm_object_offset_t offset;
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
		   if (m->object->object_slid) {
			   panic("slid page %p not allowed on this path\n", m);
		   }
		   VM_PAGE_CHECK(m);
		   m->pageout_queue = FALSE;
		   m->pageq.next = NULL;
		   m->pageq.prev = NULL;

		   /*
		    * grab a snapshot of the object and offset this
		    * page is tabled in so that we can relookup this
		    * page after we've taken the object lock - these
		    * fields are stable while we hold the page queues lock
		    * but as soon as we drop it, there is nothing to keep
		    * this page in this object... we hold an activity_in_progress
		    * on this object which will keep it from terminating
		    */
		   object = m->object;
		   offset = m->offset;

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
		   vm_object_lock(object);

		   m = vm_page_lookup(object, offset);

		   if (m == NULL ||
		       m->busy || m->cleaning || m->pageout_queue || !m->laundry) {
			   /*
			    * it's either the same page that someone else has
			    * started cleaning (or it's finished cleaning or
			    * been put back on the pageout queue), or
			    * the page has been freed or we have found a
			    * new page at this offset... in all of these cases
			    * we merely need to release the activity_in_progress
			    * we took when we put the page on the pageout queue
			    */
			   vm_object_activity_end(object);
			   vm_object_unlock(object);

			   vm_page_lockspin_queues();
			   continue;
		   }
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
				   m->pageout = FALSE;

			           vm_page_lockspin_queues();

				   vm_pageout_throttle_up(m);
				   vm_page_activate(m);
				   vm_pageout_dirty_no_pager++;

				   vm_page_unlock_queues();

				   /*
				    *	And we are done with it.
				    */
			           vm_object_activity_end(object);
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
			    */
			   if (m->pageout) {
				   /*
				    * Just free the page... VM_PAGE_FREE takes
				    * care of cleaning up all the state...
				    * including doing the vm_pageout_throttle_up
				    */
				   VM_PAGE_FREE(m);
			   } else {
			           vm_page_lockspin_queues();

				   vm_pageout_throttle_up(m);
				   vm_page_activate(m);
				   
				   vm_page_unlock_queues();

				   /*
				    *	And we are done with it.
				    */
			   }
			   vm_object_activity_end(object);
			   vm_object_unlock(object);

			   vm_page_lockspin_queues();
			   continue;
		   }
#if 0
		   /*
		    * we don't hold the page queue lock
		    * so this check isn't safe to make
		    */
		   VM_PAGE_CHECK(m);
#endif
		   /*
		    * give back the activity_in_progress reference we
		    * took when we queued up this page and replace it
		    * it with a paging_in_progress reference that will
                    * also hold the paging offset from changing and
                    * prevent the object from terminating
		    */
		   vm_object_activity_end(object);
		   vm_object_paging_begin(object);
		   vm_object_unlock(object);

                   /*
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

		   vm_pageout_io_throttle();

		   vm_page_lockspin_queues();
	}
	q->pgo_busy = FALSE;
	q->pgo_idle = TRUE;

	assert_wait((event_t) &q->pgo_pending, THREAD_UNINT);
	vm_page_unlock_queues();

	thread_block_parameter((thread_continue_t)vm_pageout_iothread_continue, (void *) q);
	/*NOTREACHED*/
}


static void
vm_pageout_iothread_external_continue(struct vm_pageout_queue *q)
{
	vm_page_t	m = NULL;
	vm_object_t	object;
	vm_object_offset_t offset;
	memory_object_t	pager;


	if (vm_pageout_internal_iothread != THREAD_NULL)
		current_thread()->options &= ~TH_OPT_VMPRIV;

	vm_page_lockspin_queues();

        while ( !queue_empty(&q->pgo_pending) ) {

		   q->pgo_busy = TRUE;
		   queue_remove_first(&q->pgo_pending, m, vm_page_t, pageq);
		   if (m->object->object_slid) {
			   panic("slid page %p not allowed on this path\n", m);
		   }
		   VM_PAGE_CHECK(m);
		   m->pageout_queue = FALSE;
		   m->pageq.next = NULL;
		   m->pageq.prev = NULL;

		   /*
		    * grab a snapshot of the object and offset this
		    * page is tabled in so that we can relookup this
		    * page after we've taken the object lock - these
		    * fields are stable while we hold the page queues lock
		    * but as soon as we drop it, there is nothing to keep
		    * this page in this object... we hold an activity_in_progress
		    * on this object which will keep it from terminating
		    */
		   object = m->object;
		   offset = m->offset;

		   vm_page_unlock_queues();

		   vm_object_lock(object);

		   m = vm_page_lookup(object, offset);

		   if (m == NULL ||
		       m->busy || m->cleaning || m->pageout_queue || !m->laundry) {
			   /*
			    * it's either the same page that someone else has
			    * started cleaning (or it's finished cleaning or
			    * been put back on the pageout queue), or
			    * the page has been freed or we have found a
			    * new page at this offset... in all of these cases
			    * we merely need to release the activity_in_progress
			    * we took when we put the page on the pageout queue
			    */
			   vm_object_activity_end(object);
			   vm_object_unlock(object);

			   vm_page_lockspin_queues();
			   continue;
		   }
		   pager = object->pager;

	           if (pager == MEMORY_OBJECT_NULL) {
		           /*
			    * This pager has been destroyed by either
			    * memory_object_destroy or vm_object_destroy, and
			    * so there is nowhere for the page to go.
			    */
			   if (m->pageout) {
				   /*
				    * Just free the page... VM_PAGE_FREE takes
				    * care of cleaning up all the state...
				    * including doing the vm_pageout_throttle_up
				    */
				   VM_PAGE_FREE(m);
			   } else {
			           vm_page_lockspin_queues();

				   vm_pageout_throttle_up(m);
				   vm_page_activate(m);
				   
				   vm_page_unlock_queues();

				   /*
				    *	And we are done with it.
				    */
			   }
			   vm_object_activity_end(object);
			   vm_object_unlock(object);

			   vm_page_lockspin_queues();
			   continue;
		   }
#if 0
		   /*
		    * we don't hold the page queue lock
		    * so this check isn't safe to make
		    */
		   VM_PAGE_CHECK(m);
#endif
		   /*
		    * give back the activity_in_progress reference we
		    * took when we queued up this page and replace it
		    * it with a paging_in_progress reference that will
                    * also hold the paging offset from changing and
                    * prevent the object from terminating
		    */
		   vm_object_activity_end(object);
		   vm_object_paging_begin(object);
		   vm_object_unlock(object);

                   /*
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

		   vm_pageout_io_throttle();

		   vm_page_lockspin_queues();
	}
	q->pgo_busy = FALSE;
	q->pgo_idle = TRUE;

	assert_wait((event_t) &q->pgo_pending, THREAD_UNINT);
	vm_page_unlock_queues();

	thread_block_parameter((thread_continue_t)vm_pageout_iothread_external_continue, (void *) q);
	/*NOTREACHED*/
}


uint32_t	vm_compressor_failed;

static void
vm_pageout_iothread_internal_continue(struct cq *cq)
{
	struct vm_pageout_queue *q;
	vm_page_t	m = NULL;
	vm_object_t	object;
	memory_object_t	pager;
	boolean_t	pgo_draining;
	vm_page_t   local_q;
	int	    local_cnt;
	vm_page_t   local_freeq = NULL;
	int         local_freed = 0;
	int	    local_batch_size;
	kern_return_t	retval;
	int		compressed_count_delta;


	KERNEL_DEBUG(0xe040000c | DBG_FUNC_END, 0, 0, 0, 0, 0);

	q = cq->q;
	local_batch_size = q->pgo_maxlaundry / (vm_compressor_thread_count * 4);

	while (TRUE) {

		local_cnt = 0;
		local_q = NULL;

		KERNEL_DEBUG(0xe0400014 | DBG_FUNC_START, 0, 0, 0, 0, 0);
	
		vm_page_lock_queues();

		KERNEL_DEBUG(0xe0400014 | DBG_FUNC_END, 0, 0, 0, 0, 0);

		KERNEL_DEBUG(0xe0400018 | DBG_FUNC_START, 0, 0, 0, 0, 0);

		while ( !queue_empty(&q->pgo_pending) && local_cnt <  local_batch_size) {

			queue_remove_first(&q->pgo_pending, m, vm_page_t, pageq);

			VM_PAGE_CHECK(m);

			m->pageout_queue = FALSE;
			m->pageq.prev = NULL;

			m->pageq.next = (queue_entry_t)local_q;
			local_q = m;
			local_cnt++;
		}
		if (local_q == NULL)
			break;

		q->pgo_busy = TRUE;

		if ((pgo_draining = q->pgo_draining) == FALSE) 
			vm_pageout_throttle_up_batch(q, local_cnt);

		vm_page_unlock_queues();

		KERNEL_DEBUG(0xe0400018 | DBG_FUNC_END, 0, 0, 0, 0, 0);

		while (local_q) {
		
			m = local_q;
			local_q = (vm_page_t)m->pageq.next;
			m->pageq.next = NULL;

			if (m->object->object_slid) {
				panic("slid page %p not allowed on this path\n", m);
			}

			object = m->object;
			pager = object->pager;

			if (!object->pager_initialized || pager == MEMORY_OBJECT_NULL)  {
				
				KERNEL_DEBUG(0xe0400010 | DBG_FUNC_START, object, pager, 0, 0, 0);

				vm_object_lock(object);

				/*
				 * If there is no memory object for the page, create
				 * one and hand it to the compression pager.
				 */

				if (!object->pager_initialized)
					vm_object_collapse(object, (vm_object_offset_t) 0, TRUE);
				if (!object->pager_initialized)
					vm_object_compressor_pager_create(object);

				if (!object->pager_initialized) {
					/*
					 * Still no pager for the object.
					 * Reactivate the page.
					 *
					 * Should only happen if there is no
					 * compression pager
					 */
					m->pageout = FALSE;
					m->laundry = FALSE;
					PAGE_WAKEUP_DONE(m);

					vm_page_lockspin_queues();
					vm_page_activate(m);
					vm_pageout_dirty_no_pager++;
					vm_page_unlock_queues();
					
					/*
					 *	And we are done with it.
					 */
					vm_object_activity_end(object);
					vm_object_unlock(object);

					continue;
				}
				pager = object->pager;

				if (pager == MEMORY_OBJECT_NULL) {
					/*
					 * This pager has been destroyed by either
					 * memory_object_destroy or vm_object_destroy, and
					 * so there is nowhere for the page to go.
					 */
					if (m->pageout) {
						/*
						 * Just free the page... VM_PAGE_FREE takes
						 * care of cleaning up all the state...
						 * including doing the vm_pageout_throttle_up
						 */
						VM_PAGE_FREE(m);
					} else {
						m->laundry = FALSE;
						PAGE_WAKEUP_DONE(m);

						vm_page_lockspin_queues();
						vm_page_activate(m);
						vm_page_unlock_queues();

						/*
						 *	And we are done with it.
						 */
					}
					vm_object_activity_end(object);
					vm_object_unlock(object);

					continue;
				}
				vm_object_unlock(object);

				KERNEL_DEBUG(0xe0400010 | DBG_FUNC_END, object, pager, 0, 0, 0);
			}
			while (vm_page_free_count < (vm_page_free_reserved - COMPRESSOR_FREE_RESERVED_LIMIT)) {
				kern_return_t	wait_result;
				int		need_wakeup = 0;

				if (local_freeq) {
					vm_page_free_list(local_freeq, TRUE);

					local_freeq = NULL;
					local_freed = 0;

					continue;
				}
				lck_mtx_lock_spin(&vm_page_queue_free_lock);

				if (vm_page_free_count < (vm_page_free_reserved - COMPRESSOR_FREE_RESERVED_LIMIT)) {
				
					if (vm_page_free_wanted_privileged++ == 0)
						need_wakeup = 1;
					wait_result = assert_wait((event_t)&vm_page_free_wanted_privileged, THREAD_UNINT);

					lck_mtx_unlock(&vm_page_queue_free_lock);

					if (need_wakeup)
						thread_wakeup((event_t)&vm_page_free_wanted);

					if (wait_result == THREAD_WAITING)
						thread_block(THREAD_CONTINUE_NULL);
				} else
					lck_mtx_unlock(&vm_page_queue_free_lock);
			}

			assert(object->activity_in_progress > 0);

			retval = vm_compressor_pager_put(
				pager,
				m->offset + object->paging_offset,
				m->phys_page,
				&cq->current_chead,
				cq->scratch_buf,
				&compressed_count_delta);

			vm_object_lock(object);
			assert(object->activity_in_progress > 0);

			assert(m->object == object);

			vm_compressor_pager_count(pager,
						  compressed_count_delta,
						  FALSE, /* shared_lock */
						  object);

			m->laundry = FALSE;
			m->pageout = FALSE;

			if (retval == KERN_SUCCESS) {
				/*
				 * If the object is purgeable, its owner's
				 * purgeable ledgers will be updated in
				 * vm_page_remove() but the page still
				 * contributes to the owner's memory footprint,
				 * so account for it as such.
				 */
				if (object->purgable != VM_PURGABLE_DENY &&
				    object->vo_purgeable_owner != NULL) {
					/* one more compressed purgeable page */
					vm_purgeable_compressed_update(object,
								       +1);
				}

				vm_page_compressions_failing = FALSE;
				
				VM_STAT_INCR(compressions);
			
				if (m->tabled)
					vm_page_remove(m, TRUE);
				vm_object_activity_end(object);
				vm_object_unlock(object);

				m->pageq.next = (queue_entry_t)local_freeq;
				local_freeq = m;
				local_freed++;

			} else {
				PAGE_WAKEUP_DONE(m);

				vm_page_lockspin_queues();

				vm_page_activate(m);
				vm_compressor_failed++;

				vm_page_compressions_failing = TRUE;

				vm_page_unlock_queues();

				vm_object_activity_end(object);
				vm_object_unlock(object);
			}
		}
		if (local_freeq) {
			vm_page_free_list(local_freeq, TRUE);
				
			local_freeq = NULL;
			local_freed = 0;
		}
		if (pgo_draining == TRUE) {
			vm_page_lockspin_queues();
			vm_pageout_throttle_up_batch(q, local_cnt);
			vm_page_unlock_queues();
		}
	}
	KERNEL_DEBUG(0xe040000c | DBG_FUNC_START, 0, 0, 0, 0, 0);

	/*
	 * queue lock is held and our q is empty
	 */
	q->pgo_busy = FALSE;
	q->pgo_idle = TRUE;

	assert_wait((event_t) &q->pgo_pending, THREAD_UNINT);
	vm_page_unlock_queues();

	KERNEL_DEBUG(0xe0400018 | DBG_FUNC_END, 0, 0, 0, 0, 0);

	thread_block_parameter((thread_continue_t)vm_pageout_iothread_internal_continue, (void *) cq);
	/*NOTREACHED*/
}



static void
vm_pageout_adjust_io_throttles(struct vm_pageout_queue *iq, struct vm_pageout_queue *eq, boolean_t req_lowpriority)
{
	uint32_t 	policy;
	boolean_t	set_iq = FALSE;
	boolean_t	set_eq = FALSE;
	
	if (hibernate_cleaning_in_progress == TRUE)
		req_lowpriority = FALSE;

	if ((DEFAULT_PAGER_IS_ACTIVE || DEFAULT_FREEZER_IS_ACTIVE) && iq->pgo_inited == TRUE && iq->pgo_lowpriority != req_lowpriority)
		set_iq = TRUE;

	if (eq->pgo_inited == TRUE && eq->pgo_lowpriority != req_lowpriority)
		set_eq = TRUE;
	
	if (set_iq == TRUE || set_eq == TRUE) {

		vm_page_unlock_queues();

		if (req_lowpriority == TRUE) {
			policy = THROTTLE_LEVEL_PAGEOUT_THROTTLED;
			DTRACE_VM(laundrythrottle);
		} else {
			policy = THROTTLE_LEVEL_PAGEOUT_UNTHROTTLED;
			DTRACE_VM(laundryunthrottle);
		}
		if (set_iq == TRUE) {
			proc_set_task_policy_thread(kernel_task, iq->pgo_tid, TASK_POLICY_EXTERNAL, TASK_POLICY_IO, policy);

			iq->pgo_lowpriority = req_lowpriority;
		}
		if (set_eq == TRUE) {
			proc_set_task_policy_thread(kernel_task, eq->pgo_tid, TASK_POLICY_EXTERNAL, TASK_POLICY_IO, policy);

			eq->pgo_lowpriority = req_lowpriority;
		}
		vm_page_lock_queues();
	}
}


static void
vm_pageout_iothread_external(void)
{
	thread_t	self = current_thread();

	self->options |= TH_OPT_VMPRIV;

	DTRACE_VM2(laundrythrottle, int, 1, (uint64_t *), NULL);	

	proc_set_task_policy_thread(kernel_task, self->thread_id, TASK_POLICY_EXTERNAL,
	                            TASK_POLICY_IO, THROTTLE_LEVEL_PAGEOUT_THROTTLED);

	vm_page_lock_queues();

	vm_pageout_queue_external.pgo_tid = self->thread_id;
	vm_pageout_queue_external.pgo_lowpriority = TRUE;
	vm_pageout_queue_external.pgo_inited = TRUE;

	vm_page_unlock_queues();

	if (COMPRESSED_PAGER_IS_ACTIVE || DEFAULT_FREEZER_COMPRESSED_PAGER_IS_ACTIVE)
		vm_pageout_iothread_external_continue(&vm_pageout_queue_external);
	else
		vm_pageout_iothread_continue(&vm_pageout_queue_external);

	/*NOTREACHED*/
}


static void
vm_pageout_iothread_internal(struct cq *cq)
{
	thread_t	self = current_thread();

	self->options |= TH_OPT_VMPRIV;

	if (DEFAULT_PAGER_IS_ACTIVE || DEFAULT_FREEZER_IS_ACTIVE) {
		DTRACE_VM2(laundrythrottle, int, 1, (uint64_t *), NULL);	

		proc_set_task_policy_thread(kernel_task, self->thread_id, TASK_POLICY_EXTERNAL,
		                            TASK_POLICY_IO, THROTTLE_LEVEL_PAGEOUT_THROTTLED);
	}
	vm_page_lock_queues();

	vm_pageout_queue_internal.pgo_tid = self->thread_id;
	vm_pageout_queue_internal.pgo_lowpriority = TRUE;
	vm_pageout_queue_internal.pgo_inited = TRUE;

	vm_page_unlock_queues();

	if (COMPRESSED_PAGER_IS_ACTIVE || DEFAULT_FREEZER_COMPRESSED_PAGER_IS_ACTIVE) {
		cq->q = &vm_pageout_queue_internal;
		cq->current_chead = NULL;
		cq->scratch_buf = kalloc(COMPRESSOR_SCRATCH_BUF_SIZE);

		vm_pageout_iothread_internal_continue(cq);
	} else
		vm_pageout_iothread_continue(&vm_pageout_queue_internal);

	/*NOTREACHED*/
}

kern_return_t
vm_set_buffer_cleanup_callout(boolean_t (*func)(int)) 
{
	if (OSCompareAndSwapPtr(NULL, func, (void * volatile *) &consider_buffer_cache_collect)) {
		return KERN_SUCCESS;
	} else {
		return KERN_FAILURE; /* Already set */
	}
}

extern boolean_t	memorystatus_manual_testing_on;
extern unsigned int 	memorystatus_level;


#if VM_PRESSURE_EVENTS

boolean_t vm_pressure_events_enabled = FALSE;

void
vm_pressure_response(void)
{

	vm_pressure_level_t	old_level = kVMPressureNormal;
	int			new_level = -1;

	uint64_t		available_memory = 0;

	if (vm_pressure_events_enabled == FALSE)
		return;


	available_memory = (((uint64_t) AVAILABLE_NON_COMPRESSED_MEMORY) * 100);


	memorystatus_level = (unsigned int) (available_memory / atop_64(max_mem));

	if (memorystatus_manual_testing_on) {
		return;
	}
	
	old_level = memorystatus_vm_pressure_level;

	switch (memorystatus_vm_pressure_level) {

		case kVMPressureNormal:
		{
			if (VM_PRESSURE_WARNING_TO_CRITICAL()) {
				new_level = kVMPressureCritical;
			}  else if (VM_PRESSURE_NORMAL_TO_WARNING()) {
				new_level = kVMPressureWarning;
			}
			break;
		}

		case kVMPressureWarning:
		case kVMPressureUrgent:
		{
			if (VM_PRESSURE_WARNING_TO_NORMAL()) {
				new_level = kVMPressureNormal;
			}  else if (VM_PRESSURE_WARNING_TO_CRITICAL()) {
				new_level = kVMPressureCritical;
			}
			break;
		}

		case kVMPressureCritical:
		{
			if (VM_PRESSURE_WARNING_TO_NORMAL()) {
				new_level = kVMPressureNormal;
			}  else if (VM_PRESSURE_CRITICAL_TO_WARNING()) {
				new_level = kVMPressureWarning;
			}
			break;
		}

		default:
			return;
	}
		
	if (new_level != -1) {
		memorystatus_vm_pressure_level = (vm_pressure_level_t) new_level;

		if ((memorystatus_vm_pressure_level != kVMPressureNormal) || (old_level != new_level)) {
			if (vm_pressure_thread_running == FALSE) {
				thread_wakeup(&vm_pressure_thread);
			}

			if (old_level != new_level) {
				thread_wakeup(&vm_pressure_changed);
			}
		}
	}

}
#endif /* VM_PRESSURE_EVENTS */

kern_return_t
mach_vm_pressure_level_monitor(__unused boolean_t wait_for_pressure, __unused unsigned int *pressure_level) {

#if   !VM_PRESSURE_EVENTS
	
	return KERN_FAILURE;

#else /* VM_PRESSURE_EVENTS */

	kern_return_t	kr = KERN_SUCCESS;

	if (pressure_level != NULL) {

		vm_pressure_level_t	old_level = memorystatus_vm_pressure_level;

		if (wait_for_pressure == TRUE) {
			wait_result_t		wr = 0;

			while (old_level == *pressure_level) {
				wr = assert_wait((event_t) &vm_pressure_changed,
						 THREAD_INTERRUPTIBLE);
				if (wr == THREAD_WAITING) {
					wr = thread_block(THREAD_CONTINUE_NULL);
				}
				if (wr == THREAD_INTERRUPTED) {
					return KERN_ABORTED;
				}
				if (wr == THREAD_AWAKENED) {
					
					old_level = memorystatus_vm_pressure_level;

					if (old_level != *pressure_level) {
						break;
					}
				}
			}
		}

		*pressure_level = old_level;
		kr = KERN_SUCCESS;
	} else {
		kr = KERN_INVALID_ARGUMENT;
	}

	return kr;
#endif /* VM_PRESSURE_EVENTS */
}

#if VM_PRESSURE_EVENTS
void
vm_pressure_thread(void) {
	static boolean_t thread_initialized = FALSE;

	if (thread_initialized == TRUE) {
		vm_pressure_thread_running = TRUE;
		consider_vm_pressure_events();
		vm_pressure_thread_running = FALSE;
	}

	thread_initialized = TRUE;
	assert_wait((event_t) &vm_pressure_thread, THREAD_UNINT);
	thread_block((thread_continue_t)vm_pressure_thread);
}
#endif /* VM_PRESSURE_EVENTS */


uint32_t vm_pageout_considered_page_last = 0;

/*
 * called once per-second via "compute_averages"
 */
void
compute_pageout_gc_throttle()
{
	if (vm_pageout_considered_page != vm_pageout_considered_page_last) {

		vm_pageout_considered_page_last = vm_pageout_considered_page;

		thread_wakeup((event_t) &vm_pageout_garbage_collect);
	}
}


static void
vm_pageout_garbage_collect(int collect)
{

	if (collect) {
		boolean_t buf_large_zfree = FALSE;
		boolean_t first_try = TRUE;

		stack_collect();

		consider_machine_collect();
		m_drain();

		do {
			if (consider_buffer_cache_collect != NULL) {
				buf_large_zfree = (*consider_buffer_cache_collect)(0);
			}
			if (first_try == TRUE || buf_large_zfree == TRUE) {
				/*
				 * consider_zone_gc should be last, because the other operations
				 * might return memory to zones.
				 */
				consider_zone_gc(buf_large_zfree);
			}
			first_try = FALSE;

		} while (buf_large_zfree == TRUE && vm_page_free_count < vm_page_free_target);

		consider_machine_adjust();
	}
	assert_wait((event_t) &vm_pageout_garbage_collect, THREAD_UNINT);

	thread_block_parameter((thread_continue_t) vm_pageout_garbage_collect, (void *)1);
	/*NOTREACHED*/
}


void	vm_pageout_reinit_tuneables(void);

void
vm_pageout_reinit_tuneables(void)
{
	vm_page_filecache_min = (uint32_t) (max_mem / PAGE_SIZE) / 15;

	if (vm_page_filecache_min < VM_PAGE_FILECACHE_MIN)
		vm_page_filecache_min = VM_PAGE_FILECACHE_MIN;

	vm_compressor_minorcompact_threshold_divisor = 18;
	vm_compressor_majorcompact_threshold_divisor = 22;
	vm_compressor_unthrottle_threshold_divisor = 32;
}


#if VM_PAGE_BUCKETS_CHECK
#if VM_PAGE_FAKE_BUCKETS
extern vm_map_offset_t vm_page_fake_buckets_start, vm_page_fake_buckets_end;
#endif /* VM_PAGE_FAKE_BUCKETS */
#endif /* VM_PAGE_BUCKETS_CHECK */

#define FBDP_TEST_COLLAPSE_COMPRESSOR 0
#if FBDP_TEST_COLLAPSE_COMPRESSOR
extern boolean_t vm_object_collapse_compressor_allowed;
#include <IOKit/IOLib.h>
#endif /* FBDP_TEST_COLLAPSE_COMPRESSOR */

#define FBDP_TEST_WIRE_AND_EXTRACT 0
#if FBDP_TEST_WIRE_AND_EXTRACT
extern ledger_template_t	task_ledger_template;
#include <mach/mach_vm.h>
extern ppnum_t vm_map_get_phys_page(vm_map_t map,
				    vm_offset_t offset);
#endif /* FBDP_TEST_WIRE_AND_EXTRACT */

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

	if (vm_pageout_swap_wait == 0)
		vm_pageout_swap_wait = VM_PAGEOUT_SWAP_WAIT;

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

#if !CONFIG_JETSAM
	vm_page_filecache_min = (uint32_t) (max_mem / PAGE_SIZE) / 20;
	if (vm_page_filecache_min < VM_PAGE_FILECACHE_MIN)
		vm_page_filecache_min = VM_PAGE_FILECACHE_MIN;
#endif

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
	vm_pageout_queue_external.pgo_draining = FALSE;
	vm_pageout_queue_external.pgo_lowpriority = FALSE;
	vm_pageout_queue_external.pgo_tid = -1;
	vm_pageout_queue_external.pgo_inited = FALSE;


	queue_init(&vm_pageout_queue_internal.pgo_pending);
	vm_pageout_queue_internal.pgo_maxlaundry = 0;
	vm_pageout_queue_internal.pgo_laundry = 0;
	vm_pageout_queue_internal.pgo_idle = FALSE;
	vm_pageout_queue_internal.pgo_busy = FALSE;
	vm_pageout_queue_internal.pgo_throttled = FALSE;
	vm_pageout_queue_internal.pgo_draining = FALSE;
	vm_pageout_queue_internal.pgo_lowpriority = FALSE;
	vm_pageout_queue_internal.pgo_tid = -1;
	vm_pageout_queue_internal.pgo_inited = FALSE;

	/* internal pageout thread started when default pager registered first time */
	/* external pageout and garbage collection threads started here */

	result = kernel_thread_start_priority((thread_continue_t)vm_pageout_iothread_external, NULL, 
					      BASEPRI_PREEMPT - 1, 
					      &vm_pageout_external_iothread);
	if (result != KERN_SUCCESS)
		panic("vm_pageout_iothread_external: create failed");

	thread_deallocate(vm_pageout_external_iothread);

	result = kernel_thread_start_priority((thread_continue_t)vm_pageout_garbage_collect, NULL,
					      BASEPRI_DEFAULT, 
					      &thread);
	if (result != KERN_SUCCESS)
		panic("vm_pageout_garbage_collect: create failed");

	thread_deallocate(thread);

#if VM_PRESSURE_EVENTS
	result = kernel_thread_start_priority((thread_continue_t)vm_pressure_thread, NULL,
						BASEPRI_DEFAULT,
						&thread);

	if (result != KERN_SUCCESS)
		panic("vm_pressure_thread: create failed");

	thread_deallocate(thread);
#endif

	vm_object_reaper_init();
	
	if (COMPRESSED_PAGER_IS_ACTIVE || DEFAULT_FREEZER_COMPRESSED_PAGER_IS_ACTIVE)
		vm_compressor_pager_init();

#if VM_PRESSURE_EVENTS
	vm_pressure_events_enabled = TRUE;
#endif /* VM_PRESSURE_EVENTS */

#if CONFIG_PHANTOM_CACHE
	vm_phantom_cache_init();
#endif
#if VM_PAGE_BUCKETS_CHECK
#if VM_PAGE_FAKE_BUCKETS
	printf("**** DEBUG: protecting fake buckets [0x%llx:0x%llx]\n",
	       (uint64_t) vm_page_fake_buckets_start,
	       (uint64_t) vm_page_fake_buckets_end);
	pmap_protect(kernel_pmap,
		     vm_page_fake_buckets_start,
		     vm_page_fake_buckets_end,
		     VM_PROT_READ);
//	*(char *) vm_page_fake_buckets_start = 'x';	/* panic! */
#endif /* VM_PAGE_FAKE_BUCKETS */
#endif /* VM_PAGE_BUCKETS_CHECK */

#if VM_OBJECT_TRACKING
	vm_object_tracking_init();
#endif /* VM_OBJECT_TRACKING */


#if FBDP_TEST_COLLAPSE_COMPRESSOR
	vm_object_size_t	backing_size, top_size;
	vm_object_t		backing_object, top_object;
	vm_map_offset_t		backing_offset, top_offset;
	unsigned char		*backing_address, *top_address;
	kern_return_t		kr;

	printf("FBDP_TEST_COLLAPSE_COMPRESSOR:\n");

	/* create backing object */
	backing_size = 15 * PAGE_SIZE;
	backing_object = vm_object_allocate(backing_size);
	assert(backing_object != VM_OBJECT_NULL);
	printf("FBDP_TEST_COLLAPSE_COMPRESSOR: created backing object %p\n",
		backing_object);
	/* map backing object */
	backing_offset = 0;
	kr = vm_map_enter(kernel_map, &backing_offset, backing_size, 0,
			  VM_FLAGS_ANYWHERE, backing_object, 0, FALSE,
			  VM_PROT_DEFAULT, VM_PROT_DEFAULT, VM_INHERIT_DEFAULT);
	assert(kr == KERN_SUCCESS);
	backing_address = (unsigned char *) backing_offset;
	printf("FBDP_TEST_COLLAPSE_COMPRESSOR: "
	       "mapped backing object %p at 0x%llx\n",
	       backing_object, (uint64_t) backing_offset);
	/* populate with pages to be compressed in backing object */
	backing_address[0x1*PAGE_SIZE] = 0xB1;
	backing_address[0x4*PAGE_SIZE] = 0xB4;
	backing_address[0x7*PAGE_SIZE] = 0xB7;
	backing_address[0xa*PAGE_SIZE] = 0xBA;
	backing_address[0xd*PAGE_SIZE] = 0xBD;
	printf("FBDP_TEST_COLLAPSE_COMPRESSOR: "
	       "populated pages to be compressed in "
	       "backing_object %p\n", backing_object);
	/* compress backing object */
	vm_object_pageout(backing_object);
	printf("FBDP_TEST_COLLAPSE_COMPRESSOR: compressing backing_object %p\n",
	       backing_object);
	/* wait for all the pages to be gone */
	while (*(volatile int *)&backing_object->resident_page_count != 0)
		IODelay(10);
	printf("FBDP_TEST_COLLAPSE_COMPRESSOR: backing_object %p compressed\n",
	       backing_object);
	/* populate with pages to be resident in backing object */
	backing_address[0x0*PAGE_SIZE] = 0xB0;
	backing_address[0x3*PAGE_SIZE] = 0xB3;
	backing_address[0x6*PAGE_SIZE] = 0xB6;
	backing_address[0x9*PAGE_SIZE] = 0xB9;
	backing_address[0xc*PAGE_SIZE] = 0xBC;
	printf("FBDP_TEST_COLLAPSE_COMPRESSOR: "
	       "populated pages to be resident in "
	       "backing_object %p\n", backing_object);
	/* leave the other pages absent */
	/* mess with the paging_offset of the backing_object */
	assert(backing_object->paging_offset == 0);
	backing_object->paging_offset = 0x3000;

	/* create top object */
	top_size = 9 * PAGE_SIZE;
	top_object = vm_object_allocate(top_size);
	assert(top_object != VM_OBJECT_NULL);
	printf("FBDP_TEST_COLLAPSE_COMPRESSOR: created top object %p\n",
		top_object);
	/* map top object */
	top_offset = 0;
	kr = vm_map_enter(kernel_map, &top_offset, top_size, 0,
			  VM_FLAGS_ANYWHERE, top_object, 0, FALSE,
			  VM_PROT_DEFAULT, VM_PROT_DEFAULT, VM_INHERIT_DEFAULT);
	assert(kr == KERN_SUCCESS);
	top_address = (unsigned char *) top_offset;
	printf("FBDP_TEST_COLLAPSE_COMPRESSOR: "
	       "mapped top object %p at 0x%llx\n",
	       top_object, (uint64_t) top_offset);
	/* populate with pages to be compressed in top object */
	top_address[0x3*PAGE_SIZE] = 0xA3;
	top_address[0x4*PAGE_SIZE] = 0xA4;
	top_address[0x5*PAGE_SIZE] = 0xA5;
	printf("FBDP_TEST_COLLAPSE_COMPRESSOR: "
	       "populated pages to be compressed in "
	       "top_object %p\n", top_object);
	/* compress top object */
	vm_object_pageout(top_object);
	printf("FBDP_TEST_COLLAPSE_COMPRESSOR: compressing top_object %p\n",
	       top_object);
	/* wait for all the pages to be gone */
	while (top_object->resident_page_count != 0);
	printf("FBDP_TEST_COLLAPSE_COMPRESSOR: top_object %p compressed\n",
	       top_object);
	/* populate with pages to be resident in top object */
	top_address[0x0*PAGE_SIZE] = 0xA0;
	top_address[0x1*PAGE_SIZE] = 0xA1;
	top_address[0x2*PAGE_SIZE] = 0xA2;
	printf("FBDP_TEST_COLLAPSE_COMPRESSOR: "
	       "populated pages to be resident in "
	       "top_object %p\n", top_object);
	/* leave the other pages absent */
	
	/* link the 2 objects */
	vm_object_reference(backing_object);
	top_object->shadow = backing_object;
	top_object->vo_shadow_offset = 0x3000;
	printf("FBDP_TEST_COLLAPSE_COMPRESSOR: linked %p and %p\n",
	       top_object, backing_object);

	/* unmap backing object */
	vm_map_remove(kernel_map,
		      backing_offset,
		      backing_offset + backing_size,
		      0);
	printf("FBDP_TEST_COLLAPSE_COMPRESSOR: "
	       "unmapped backing_object %p [0x%llx:0x%llx]\n",
	       backing_object,
	       (uint64_t) backing_offset,
	       (uint64_t) (backing_offset + backing_size));

	/* collapse */
	printf("FBDP_TEST_COLLAPSE_COMPRESSOR: collapsing %p\n", top_object);
	vm_object_lock(top_object);
	vm_object_collapse(top_object, 0, FALSE);
	vm_object_unlock(top_object);
	printf("FBDP_TEST_COLLAPSE_COMPRESSOR: collapsed %p\n", top_object);

	/* did it work? */
	if (top_object->shadow != VM_OBJECT_NULL) {
		printf("FBDP_TEST_COLLAPSE_COMPRESSOR: not collapsed\n");
		printf("FBDP_TEST_COLLAPSE_COMPRESSOR: FAIL\n");
		if (vm_object_collapse_compressor_allowed) {
			panic("FBDP_TEST_COLLAPSE_COMPRESSOR: FAIL\n");
		}
	} else {
		/* check the contents of the mapping */
		unsigned char expect[9] =
			{ 0xA0, 0xA1, 0xA2,	/* resident in top */
			  0xA3, 0xA4, 0xA5,	/* compressed in top */
			  0xB9,	/* resident in backing + shadow_offset */
			  0xBD,	/* compressed in backing + shadow_offset + paging_offset */
			  0x00 };		/* absent in both */
		unsigned char actual[9];
		unsigned int i, errors;

		errors = 0;
		for (i = 0; i < sizeof (actual); i++) {
			actual[i] = (unsigned char) top_address[i*PAGE_SIZE];
			if (actual[i] != expect[i]) {
				errors++;
			}
		}
		printf("FBDP_TEST_COLLAPSE_COMPRESSOR: "
		       "actual [%x %x %x %x %x %x %x %x %x] "
		       "expect [%x %x %x %x %x %x %x %x %x] "
		       "%d errors\n",
		       actual[0], actual[1], actual[2], actual[3],
		       actual[4], actual[5], actual[6], actual[7],
		       actual[8],
		       expect[0], expect[1], expect[2], expect[3],
		       expect[4], expect[5], expect[6], expect[7],
		       expect[8],
		       errors);
		if (errors) {
			panic("FBDP_TEST_COLLAPSE_COMPRESSOR: FAIL\n"); 
		} else {
			printf("FBDP_TEST_COLLAPSE_COMPRESSOR: PASS\n");
		}
	}
#endif /* FBDP_TEST_COLLAPSE_COMPRESSOR */

#if FBDP_TEST_WIRE_AND_EXTRACT
	ledger_t		ledger;
	vm_map_t		user_map, wire_map;
	mach_vm_address_t	user_addr, wire_addr;
	mach_vm_size_t		user_size, wire_size;
	mach_vm_offset_t	cur_offset;
	vm_prot_t		cur_prot, max_prot;
	ppnum_t			user_ppnum, wire_ppnum;
	kern_return_t		kr;

	ledger = ledger_instantiate(task_ledger_template,
				    LEDGER_CREATE_ACTIVE_ENTRIES);
	user_map = vm_map_create(pmap_create(ledger, 0, TRUE),
				 0x100000000ULL,
				 0x200000000ULL,
				 TRUE);
	wire_map = vm_map_create(NULL,
				 0x100000000ULL,
				 0x200000000ULL,
				 TRUE);
	user_addr = 0;
	user_size = 0x10000;
	kr = mach_vm_allocate(user_map,
			      &user_addr,
			      user_size,
			      VM_FLAGS_ANYWHERE);
	assert(kr == KERN_SUCCESS);
	wire_addr = 0;
	wire_size = user_size;
	kr = mach_vm_remap(wire_map,
			   &wire_addr,
			   wire_size,
			   0,
			   VM_FLAGS_ANYWHERE,
			   user_map,
			   user_addr,
			   FALSE,
			   &cur_prot,
			   &max_prot,
			   VM_INHERIT_NONE);
	assert(kr == KERN_SUCCESS);
	for (cur_offset = 0;
	     cur_offset < wire_size;
	     cur_offset += PAGE_SIZE) {
		kr = vm_map_wire_and_extract(wire_map,
					     wire_addr + cur_offset,
					     VM_PROT_DEFAULT,
					     TRUE,
					     &wire_ppnum);
		assert(kr == KERN_SUCCESS);
		user_ppnum = vm_map_get_phys_page(user_map,
						  user_addr + cur_offset);
		printf("FBDP_TEST_WIRE_AND_EXTRACT: kr=0x%x "
		       "user[%p:0x%llx:0x%x] wire[%p:0x%llx:0x%x]\n",
		       kr,
		       user_map, user_addr + cur_offset, user_ppnum,
		       wire_map, wire_addr + cur_offset, wire_ppnum);
		if (kr != KERN_SUCCESS ||
		    wire_ppnum == 0 ||
		    wire_ppnum != user_ppnum) {
			panic("FBDP_TEST_WIRE_AND_EXTRACT: FAIL\n");
		}
	}
	cur_offset -= PAGE_SIZE;
	kr = vm_map_wire_and_extract(wire_map,
				     wire_addr + cur_offset,
				     VM_PROT_DEFAULT,
				     TRUE,
				     &wire_ppnum);
	assert(kr == KERN_SUCCESS);
	printf("FBDP_TEST_WIRE_AND_EXTRACT: re-wire kr=0x%x "
	       "user[%p:0x%llx:0x%x] wire[%p:0x%llx:0x%x]\n",
	       kr,
	       user_map, user_addr + cur_offset, user_ppnum,
	       wire_map, wire_addr + cur_offset, wire_ppnum);
	if (kr != KERN_SUCCESS ||
	    wire_ppnum == 0 ||
	    wire_ppnum != user_ppnum) {
		panic("FBDP_TEST_WIRE_AND_EXTRACT: FAIL\n");
	}
	
	printf("FBDP_TEST_WIRE_AND_EXTRACT: PASS\n");
#endif /* FBDP_TEST_WIRE_AND_EXTRACT */


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



#define MAX_COMRPESSOR_THREAD_COUNT	8

struct cq ciq[MAX_COMRPESSOR_THREAD_COUNT];

int vm_compressor_thread_count = 2;

kern_return_t
vm_pageout_internal_start(void)
{
	kern_return_t	result;
	int		i;
	host_basic_info_data_t hinfo;

	if (COMPRESSED_PAGER_IS_ACTIVE || DEFAULT_FREEZER_COMPRESSED_PAGER_IS_ACTIVE) {
		mach_msg_type_number_t count = HOST_BASIC_INFO_COUNT;
#define BSD_HOST 1
		host_info((host_t)BSD_HOST, HOST_BASIC_INFO, (host_info_t)&hinfo, &count);

		assert(hinfo.max_cpus > 0);

		if (vm_compressor_thread_count >= hinfo.max_cpus)
			vm_compressor_thread_count = hinfo.max_cpus - 1;
		if (vm_compressor_thread_count <= 0)
			vm_compressor_thread_count = 1;
		else if (vm_compressor_thread_count > MAX_COMRPESSOR_THREAD_COUNT)
			vm_compressor_thread_count = MAX_COMRPESSOR_THREAD_COUNT;

		vm_pageout_queue_internal.pgo_maxlaundry = (vm_compressor_thread_count * 4) * VM_PAGE_LAUNDRY_MAX;
	} else {
		vm_compressor_thread_count = 1;
		vm_pageout_queue_internal.pgo_maxlaundry = VM_PAGE_LAUNDRY_MAX;
	}

	for (i = 0; i < vm_compressor_thread_count; i++) {

		result = kernel_thread_start_priority((thread_continue_t)vm_pageout_iothread_internal, (void *)&ciq[i], BASEPRI_PREEMPT - 1, &vm_pageout_internal_iothread);
		if (result == KERN_SUCCESS)
			thread_deallocate(vm_pageout_internal_iothread);
		else
			break;
	}
	return result;
}

#if CONFIG_IOSCHED
/*
 * To support I/O Expedite for compressed files we mark the upls with special flags.
 * The way decmpfs works is that we create a big upl which marks all the pages needed to
 * represent the compressed file as busy. We tag this upl with the flag UPL_DECMP_REQ. Decmpfs
 * then issues smaller I/Os for compressed I/Os, deflates them and puts the data into the pages
 * being held in the big original UPL. We mark each of these smaller UPLs with the flag
 * UPL_DECMP_REAL_IO. Any outstanding real I/O UPL is tracked by the big req upl using the
 * decmp_io_upl field (in the upl structure). This link is protected in the forward direction
 * by the req upl lock (the reverse link doesnt need synch. since we never inspect this link
 * unless the real I/O upl is being destroyed).
 */


static void
upl_set_decmp_info(upl_t upl, upl_t src_upl)
{
        assert((src_upl->flags & UPL_DECMP_REQ) != 0);

        upl_lock(src_upl);
        if (src_upl->decmp_io_upl) {
                /*
                 * If there is already an alive real I/O UPL, ignore this new UPL.
                 * This case should rarely happen and even if it does, it just means
                 * that we might issue a spurious expedite which the driver is expected
                 * to handle.
                 */ 
                upl_unlock(src_upl);
                return;
        }
        src_upl->decmp_io_upl = (void *)upl;
        src_upl->ref_count++;
	upl_unlock(src_upl);

        upl->flags |= UPL_DECMP_REAL_IO;
        upl->decmp_io_upl = (void *)src_upl;

}
#endif /* CONFIG_IOSCHED */  

#if UPL_DEBUG
int	upl_debug_enabled = 1;
#else
int	upl_debug_enabled = 0;
#endif

static upl_t
upl_create(int type, int flags, upl_size_t size)
{
	upl_t	upl;
	vm_size_t	page_field_size = 0;
	int	upl_flags = 0;
	vm_size_t	upl_size  = sizeof(struct upl);

	size = round_page_32(size);

	if (type & UPL_CREATE_LITE) {
		page_field_size = (atop(size) + 7) >> 3;
		page_field_size = (page_field_size + 3) & 0xFFFFFFFC;

		upl_flags |= UPL_LITE;
	}
	if (type & UPL_CREATE_INTERNAL) {
		upl_size += sizeof(struct upl_page_info) * atop(size);

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
	upl->ext_ref_count = 0;
	upl->highest_page = 0;
	upl_lock_init(upl);
	upl->vector_upl = NULL;
#if CONFIG_IOSCHED
	if (type & UPL_CREATE_IO_TRACKING) {
		upl->upl_priority = proc_get_effective_thread_policy(current_thread(), TASK_POLICY_IO);
	}
	
	upl->upl_reprio_info = 0;
	upl->decmp_io_upl = 0;
	if ((type & UPL_CREATE_INTERNAL) && (type & UPL_CREATE_EXPEDITE_SUP)) {
		/* Only support expedite on internal UPLs */
		thread_t        curthread = current_thread();
		upl->upl_reprio_info = (uint64_t *)kalloc(sizeof(uint64_t) * atop(size));
		bzero(upl->upl_reprio_info, (sizeof(uint64_t) * atop(size)));
		upl->flags |= UPL_EXPEDITE_SUPPORTED;
		if (curthread->decmp_upl != NULL) 
			upl_set_decmp_info(upl, curthread->decmp_upl);
	}
#endif
#if CONFIG_IOSCHED || UPL_DEBUG
	if ((type & UPL_CREATE_IO_TRACKING) || upl_debug_enabled) {
		upl->upl_creator = current_thread();
		upl->uplq.next = 0;
		upl->uplq.prev = 0;
		upl->flags |= UPL_TRACKED_BY_OBJECT;
	}
#endif

#if UPL_DEBUG
	upl->ubc_alias1 = 0;
	upl->ubc_alias2 = 0;

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

	if (upl->ext_ref_count) {
		panic("upl(%p) ext_ref_count", upl);
	}

#if CONFIG_IOSCHED
        if ((upl->flags & UPL_DECMP_REAL_IO) && upl->decmp_io_upl) {
                upl_t src_upl;
                src_upl = upl->decmp_io_upl;
                assert((src_upl->flags & UPL_DECMP_REQ) != 0);
                upl_lock(src_upl);
                src_upl->decmp_io_upl = NULL;
                upl_unlock(src_upl);
                upl_deallocate(src_upl);
        }
#endif /* CONFIG_IOSCHED */

#if CONFIG_IOSCHED || UPL_DEBUG
	if ((upl->flags & UPL_TRACKED_BY_OBJECT) && !(upl->flags & UPL_VECTOR)) {
		vm_object_t	object;

		if (upl->flags & UPL_SHADOWED) {
			object = upl->map_object->shadow;
		} else {
			object = upl->map_object;
		}

		vm_object_lock(object);
		queue_remove(&object->uplq, upl, upl_t, uplq);
		vm_object_activity_end(object);
		vm_object_collapse(object, 0, TRUE);
		vm_object_unlock(object);
	}
#endif
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

#if CONFIG_IOSCHED
	if (upl->flags & UPL_EXPEDITE_SUPPORTED)
		kfree(upl->upl_reprio_info, sizeof(uint64_t) * (size/PAGE_SIZE));
#endif

	if (upl->flags & UPL_INTERNAL) {
		kfree(upl,
		      sizeof(struct upl) + 
		      (sizeof(struct upl_page_info) * (size/PAGE_SIZE))
		      + page_field_size);
	} else {
		kfree(upl, sizeof(struct upl) + page_field_size);
	}
}

void
upl_deallocate(upl_t upl)
{
	upl_lock(upl);
	if (--upl->ref_count == 0) {
		if(vector_upl_is_valid(upl))
			vector_upl_deallocate(upl);
		upl_unlock(upl);	
		upl_destroy(upl);
	}
	else
		upl_unlock(upl);
}

#if CONFIG_IOSCHED
void
upl_mark_decmp(upl_t upl)
{
	if (upl->flags & UPL_TRACKED_BY_OBJECT) {
		upl->flags |= UPL_DECMP_REQ;
		upl->upl_creator->decmp_upl = (void *)upl;
	}	
}

void
upl_unmark_decmp(upl_t upl)
{
	if(upl && (upl->flags & UPL_DECMP_REQ)) {
		upl->upl_creator->decmp_upl = NULL;
	}
} 

#endif /* CONFIG_IOSCHED */

#define VM_PAGE_Q_BACKING_UP(q)		\
        ((q)->pgo_laundry >= (((q)->pgo_maxlaundry * 8) / 10))

boolean_t must_throttle_writes(void);

boolean_t
must_throttle_writes()
{
	if (VM_PAGE_Q_BACKING_UP(&vm_pageout_queue_external) &&
	    vm_page_pageable_external_count > (AVAILABLE_NON_COMPRESSED_MEMORY * 6) / 10)
		return (TRUE);

	return (FALSE);
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
	unsigned int		size_in_pages;
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
	struct	vm_page_delayed_work	dw_array[DEFAULT_DELAYED_WORK_LIMIT];
	struct	vm_page_delayed_work	*dwp;
	int			dw_count;
	int			dw_limit;
	int 			io_tracking_flag = 0;

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


	if (size > MAX_UPL_SIZE_BYTES)
		size = MAX_UPL_SIZE_BYTES;

	if ( (cntrl_flags & UPL_SET_INTERNAL) && page_list_count != NULL)
	        *page_list_count = MAX_UPL_SIZE_BYTES >> PAGE_SHIFT;

#if CONFIG_IOSCHED || UPL_DEBUG
	if (object->io_tracking || upl_debug_enabled)
		io_tracking_flag |= UPL_CREATE_IO_TRACKING;
#endif
#if CONFIG_IOSCHED
	if (object->io_tracking)
		io_tracking_flag |= UPL_CREATE_EXPEDITE_SUP;
#endif

	if (cntrl_flags & UPL_SET_INTERNAL) {
	        if (cntrl_flags & UPL_SET_LITE) {

			upl = upl_create(UPL_CREATE_INTERNAL | UPL_CREATE_LITE | io_tracking_flag, 0, size);

			user_page_list = (upl_page_info_t *) (((uintptr_t)upl) + sizeof(struct upl));
			lite_list = (wpl_array_t)
					(((uintptr_t)user_page_list) + 
					((size/PAGE_SIZE) * sizeof(upl_page_info_t)));
			if (size == 0) {
				user_page_list = NULL;
				lite_list = NULL;
			}
		} else {
		        upl = upl_create(UPL_CREATE_INTERNAL | io_tracking_flag, 0, size);

			user_page_list = (upl_page_info_t *) (((uintptr_t)upl) + sizeof(struct upl));
			if (size == 0) {
				user_page_list = NULL;
			}
		}
	} else {
	        if (cntrl_flags & UPL_SET_LITE) {

			upl = upl_create(UPL_CREATE_EXTERNAL | UPL_CREATE_LITE | io_tracking_flag, 0, size);

			lite_list = (wpl_array_t) (((uintptr_t)upl) + sizeof(struct upl));
			if (size == 0) {
				lite_list = NULL;
			}
		} else {
		        upl = upl_create(UPL_CREATE_EXTERNAL | io_tracking_flag, 0, size);
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
		upl->map_object->vo_shadow_offset = offset;
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

#if CONFIG_IOSCHED || UPL_DEBUG
	if (object->io_tracking || upl_debug_enabled) {
		vm_object_activity_begin(object);
		queue_enter(&object->uplq, upl, upl_t, uplq);
	}
#endif
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
	size_in_pages = size / PAGE_SIZE;

	dwp = &dw_array[0];
	dw_count = 0;
	dw_limit = DELAYED_WORK_LIMIT(DEFAULT_DELAYED_WORK_LIMIT);

	if (vm_page_free_count > (vm_page_free_target + size_in_pages) ||
	    object->resident_page_count < ((MAX_UPL_SIZE_BYTES * 2) >> PAGE_SHIFT))
		object->scan_collisions = 0;

	if ((cntrl_flags & UPL_WILL_MODIFY) && must_throttle_writes() == TRUE) {
		boolean_t	isSSD = FALSE;

		vnode_pager_get_isSSD(object->pager, &isSSD);
		vm_object_unlock(object);
		
		OSAddAtomic(size_in_pages, &vm_upl_wait_for_pages);

		if (isSSD == TRUE)
			delay(1000 * size_in_pages);
		else
			delay(5000 * size_in_pages);
		OSAddAtomic(-size_in_pages, &vm_upl_wait_for_pages);

		vm_object_lock(object);
	}

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
			        dst_page->cleaning ||
			        (VM_PAGE_WIRED(dst_page))) {
				
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
			        if (dst_page->laundry || !(cntrl_flags & UPL_FOR_PAGEOUT)) {
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
				 * can't have been referenced recently...
				 */
				if ( (hibernate_cleaning_in_progress == TRUE ||
				      (!((refmod_state & VM_MEM_REFERENCED) || dst_page->reference) || dst_page->throttled)) && 
				      ((refmod_state & VM_MEM_MODIFIED) || dst_page->dirty || dst_page->precious) ) {
				        goto check_busy;
				}
dont_return:
				/*
				 * if we reach here, we're not to return
				 * the page... go on to the next one
				 */
				if (dst_page->laundry == TRUE) {
					/*
					 * if we get here, the page is not 'cleaning' (filtered out above).
					 * since it has been referenced, remove it from the laundry
					 * so we don't pay the cost of an I/O to clean a page
					 * we're just going to take back
					 */
					vm_page_lockspin_queues();

					vm_pageout_steal_laundry(dst_page, TRUE);
					vm_page_activate(dst_page);
					
					vm_page_unlock_queues();
				}
				if (user_page_list)
				        user_page_list[entry].phys_addr = 0;

				goto try_next_page;
			}
check_busy:			
			if (dst_page->busy) {
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
			if (dst_page->pageout)
			        encountered_lrp = TRUE;
			if ((dst_page->dirty ||	(dst_page->object->internal && dst_page->precious))) {
			        if (encountered_lrp)
				        CLUSTER_STAT(pages_at_higher_offsets++;)
				else
				        CLUSTER_STAT(pages_at_lower_offsets++;)
			}
#endif
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
			if (dirty) {
				SET_PAGE_DIRTY(dst_page, FALSE);
			} else {
				dst_page->dirty = FALSE;
			}

			if (!dirty)
				dst_page->precious = TRUE;

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
				if ( !VM_PAGE_WIRED(dst_page))
					dst_page->pageout = TRUE;
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
					/*
					 * skip over pages already present in the cache
					 */
					if (user_page_list)
						user_page_list[entry].phys_addr = 0;

					goto try_next_page;
				}
				if (dst_page->fictitious) {
					panic("need corner case for fictitious page");
				}

				if (dst_page->busy || dst_page->cleaning) {
					/*
					 * someone else is playing with the
					 * page.  We will have to wait.
					 */
					PAGE_SLEEP(object, dst_page, THREAD_UNINT);

					continue;
				}
				if (dst_page->laundry) {
					dst_page->pageout = FALSE;

					vm_pageout_steal_laundry(dst_page, FALSE);
				}
			} else {
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
				if (object->scan_collisions) {
					/*
					 * the pageout_scan thread is trying to steal
					 * pages from this object, but has run into our
					 * lock... grab 2 pages from the head of the object...
					 * the first is freed on behalf of pageout_scan, the
					 * 2nd is for our own use... we use vm_object_page_grab
					 * in both cases to avoid taking pages from the free
					 * list since we are under memory pressure and our
					 * lock on this object is getting in the way of
					 * relieving it
					 */
					dst_page = vm_object_page_grab(object);

					if (dst_page != VM_PAGE_NULL)
						vm_page_release(dst_page);

					dst_page = vm_object_page_grab(object);
				}
				if (dst_page == VM_PAGE_NULL) {
					/*
					 * need to allocate a page
					 */
					dst_page = vm_page_grab();
				}
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
					
					OSAddAtomic(size_in_pages, &vm_upl_wait_for_pages);

					VM_DEBUG_EVENT(vm_upl_page_wait, VM_UPL_PAGE_WAIT, DBG_FUNC_START, vm_upl_wait_for_pages, 0, 0, 0);

					VM_PAGE_WAIT();
					OSAddAtomic(-size_in_pages, &vm_upl_wait_for_pages);

					VM_DEBUG_EVENT(vm_upl_page_wait, VM_UPL_PAGE_WAIT, DBG_FUNC_END, vm_upl_wait_for_pages, 0, 0, 0);

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

					if ( !(cntrl_flags & UPL_FILE_IO))
						VM_STAT_INCR(pageins);
				}
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

			if (cntrl_flags & UPL_REQUEST_SET_DIRTY) {
				upl->flags &= ~UPL_CLEAR_DIRTY;
				upl->flags |= UPL_SET_DIRTY;
				dirty = TRUE;
				upl->flags |= UPL_SET_DIRTY;
			} else if (cntrl_flags & UPL_CLEAN_IN_PLACE) {
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
			if (cntrl_flags & UPL_PRECIOUS) {
				if (dst_page->object->internal) {
					SET_PAGE_DIRTY(dst_page, FALSE);
					dst_page->precious = FALSE;
				} else {
					dst_page->precious = TRUE;
				}
			} else {
				dst_page->precious = FALSE;
			}
		}
		if (dst_page->busy)
			upl->flags |= UPL_HAS_BUSY;

		if (dst_page->phys_page > upl->highest_page)
		        upl->highest_page = dst_page->phys_page;
		if (user_page_list) {
			user_page_list[entry].phys_addr = dst_page->phys_page;
			user_page_list[entry].pageout	= dst_page->pageout;
			user_page_list[entry].absent	= dst_page->absent;
			user_page_list[entry].dirty	= dst_page->dirty;
			user_page_list[entry].precious	= dst_page->precious;
			user_page_list[entry].device	= FALSE;
			user_page_list[entry].needed    = FALSE;
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
			if (dst_page->clustered)
				VM_PAGE_CONSUME_CLUSTERED(dst_page);
		}
try_next_page:
		if (dwp->dw_mask) {
			if (dwp->dw_mask & DW_vm_page_activate)
				VM_STAT_INCR(reactivations);

			VM_PAGE_ADD_DELAYED_WORK(dwp, dst_page, dw_count);

			if (dw_count >= dw_limit) {
				vm_page_do_delayed_work(object, &dw_array[0], dw_count);

				dwp = &dw_array[0];
				dw_count = 0;
			}
		}
		entry++;
		dst_offset += PAGE_SIZE_64;
		xfer_size -= PAGE_SIZE;
	}
	if (dw_count)
		vm_page_do_delayed_work(object, &dw_array[0], dw_count);

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
		super_size_64 = ((base_offset + super_size) > object->vo_size) ? (object->vo_size - base_offset) : super_size;
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

	        if (entry->object.vm_object == VM_OBJECT_NULL || !entry->object.vm_object->phys_contiguous) {
        		if (*upl_size > MAX_UPL_SIZE_BYTES)
               			*upl_size = MAX_UPL_SIZE_BYTES;
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
		}

		local_object = entry->object.vm_object;
		if (vm_map_entry_should_cow_for_true_share(entry) &&
		    local_object->vo_size > *upl_size &&
		    *upl_size != 0) {
			vm_prot_t	prot;

			/*
			 * Set up the targeted range for copy-on-write to avoid
			 * applying true_share/copy_delay to the entire object.
			 */

			if (vm_map_lock_read_to_write(map)) {
				goto REDISCOVER_ENTRY;
			}

			vm_map_clip_start(map,
					  entry,
					  vm_map_trunc_page(offset,
							    VM_MAP_PAGE_MASK(map)));
			vm_map_clip_end(map,
					entry,
					vm_map_round_page(offset + *upl_size,
							  VM_MAP_PAGE_MASK(map)));
			if ((entry->vme_end - offset) < *upl_size) {
				*upl_size = (upl_size_t) (entry->vme_end - offset);
				assert(*upl_size == entry->vme_end - offset);
			}

			prot = entry->protection & ~VM_PROT_WRITE;
			if (override_nx(map, entry->alias) && prot)
				prot |= VM_PROT_EXECUTE;
			vm_object_pmap_protect(local_object,
					       entry->offset,
					       entry->vme_end - entry->vme_start,
					       ((entry->is_shared || map->mapped_in_other_pmaps)
						? PMAP_NULL
						: map->pmap),
					       entry->vme_start,
					       prot);
			entry->needs_copy = TRUE;

			vm_map_lock_write_to_read(map);
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
			vm_prot_t		fault_type;

			local_map = map;

			if (caller_flags & UPL_COPYOUT_FROM) {
				fault_type = VM_PROT_READ | VM_PROT_COPY;
				vm_counters.create_upl_extra_cow++;
				vm_counters.create_upl_extra_cow_pages += (entry->vme_end - entry->vme_start) / PAGE_SIZE;
			} else {
				fault_type = VM_PROT_WRITE;
			}
			if (vm_map_lookup_locked(&local_map,
						 offset, fault_type,
						 OBJECT_LOCK_EXCLUSIVE,
						 &version, &object,
						 &new_offset, &prot, &wired,
						 NULL,
						 &real_map) != KERN_SUCCESS) {
				if (fault_type == VM_PROT_WRITE) {
					vm_counters.create_upl_lookup_failure_write++;
				} else {
					vm_counters.create_upl_lookup_failure_copy++;
				}
				vm_map_unlock_read(local_map);
				return KERN_FAILURE;
			}
			if (real_map != map)
				vm_map_unlock(real_map);
			vm_map_unlock_read(local_map);

			vm_object_unlock(object);

			goto REDISCOVER_ENTRY;
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
							       local_object->vo_shadow_offset,
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
	} else {
		/*
		 * check to see if already mapped
		 */
		if (UPL_PAGE_LIST_MAPPED & upl->flags) {
			upl_unlock(upl);
			return KERN_FAILURE;
		}
	}
	if ((!(upl->flags & UPL_SHADOWED)) &&
	    ((upl->flags & UPL_HAS_BUSY) ||
	     !((upl->flags & (UPL_DEVICE_MEMORY | UPL_IO_WIRE)) || (upl->map_object->phys_contiguous)))) {

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
		upl->map_object->vo_shadow_offset = upl->offset - object->paging_offset;
		upl->map_object->wimg_bits = object->wimg_bits;
		offset = upl->map_object->vo_shadow_offset;
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
	if (upl->flags & UPL_SHADOWED)
	        offset = 0;
	else
	        offset = upl->offset - upl->map_object->paging_offset;

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

		if (kr != KERN_SUCCESS) {
			upl_unlock(upl);
			return(kr);
		}
	}
	else {
		kr = vm_map_enter(map, dst_addr, (vm_map_size_t)size, (vm_map_offset_t) 0,
				  VM_FLAGS_FIXED, upl->map_object, offset, FALSE,
				  VM_PROT_DEFAULT, VM_PROT_ALL, VM_INHERIT_DEFAULT);
		if(kr)
			panic("vm_map_enter failed for a Vector UPL\n");
	}
	vm_object_lock(upl->map_object);

	for (addr = *dst_addr; size > 0; size -= PAGE_SIZE, addr += PAGE_SIZE) {
		m = vm_page_lookup(upl->map_object, offset);

		if (m) {
			m->pmapped = TRUE;

			/* CODE SIGNING ENFORCEMENT: page has been wpmapped, 
			 * but only in kernel space. If this was on a user map,
			 * we'd have to set the wpmapped bit. */
			/* m->wpmapped = TRUE; */
			assert(map->pmap == kernel_pmap);
	
			PMAP_ENTER(map->pmap, addr, m, VM_PROT_DEFAULT, VM_PROT_NONE, 0, TRUE);
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
	
	if(isVectorUPL)
		goto process_upl_to_enter;

	upl_unlock(upl);

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
		
			vm_map_remove(
				map,
				vm_map_trunc_page(addr,
						  VM_MAP_PAGE_MASK(map)),
				vm_map_round_page(addr + size,
						  VM_MAP_PAGE_MASK(map)),
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
	struct	vm_page_delayed_work	dw_array[DEFAULT_DELAYED_WORK_LIMIT];
	struct	vm_page_delayed_work	*dwp;
	int			dw_count;
	int			dw_limit;
	int			isVectorUPL = 0;
	upl_t			vector_upl = NULL;
	boolean_t		should_be_throttled = FALSE;

	vm_page_t		nxt_page = VM_PAGE_NULL;
	int			fast_path_possible = 0;
	int			fast_path_full_commit = 0;
	int			throttle_page = 0;
	int			unwired_count = 0;
	int			local_queue_count = 0;
	queue_head_t		local_queue;

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
	if (upl->flags & UPL_SET_DIRTY)
		flags |= UPL_COMMIT_SET_DIRTY;
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
	if (!VM_DYNAMIC_PAGING_ENABLED(memory_manager_default) && shadow_object->internal)
		should_be_throttled = TRUE;

	dwp = &dw_array[0];
	dw_count = 0;
	dw_limit = DELAYED_WORK_LIMIT(DEFAULT_DELAYED_WORK_LIMIT);

	if ((upl->flags & UPL_IO_WIRE) &&
	    !(flags & UPL_COMMIT_FREE_ABSENT) &&
	    !isVectorUPL &&
	    shadow_object->purgable != VM_PURGABLE_VOLATILE &&
	    shadow_object->purgable != VM_PURGABLE_EMPTY) {

		if (!queue_empty(&shadow_object->memq)) {
			queue_init(&local_queue);
			if (size == shadow_object->vo_size) {
				nxt_page = (vm_page_t)queue_first(&shadow_object->memq);
				fast_path_full_commit = 1;
			}
			fast_path_possible = 1;

			if (!VM_DYNAMIC_PAGING_ENABLED(memory_manager_default) && shadow_object->internal &&
			    (shadow_object->purgable == VM_PURGABLE_DENY ||
			     shadow_object->purgable == VM_PURGABLE_NONVOLATILE ||
			     shadow_object->purgable == VM_PURGABLE_VOLATILE)) {
				throttle_page = 1;
			}
		}
	}

	while (xfer_size) {
		vm_page_t	t, m;

		dwp->dw_mask = 0;
		clear_refmod = 0;

		m = VM_PAGE_NULL;

		if (upl->flags & UPL_LITE) {
			unsigned int	pg_num;

			if (nxt_page != VM_PAGE_NULL) {
				m = nxt_page;
				nxt_page = (vm_page_t)queue_next(&nxt_page->listq);
				target_offset = m->offset;
			}
			pg_num = (unsigned int) (target_offset/PAGE_SIZE);
			assert(pg_num == target_offset/PAGE_SIZE);

			if (lite_list[pg_num>>5] & (1 << (pg_num & 31))) {
			        lite_list[pg_num>>5] &= ~(1 << (pg_num & 31));

				if (!(upl->flags & UPL_KERNEL_OBJECT) && m == VM_PAGE_NULL)
					m = vm_page_lookup(shadow_object, target_offset + (upl->offset - shadow_object->paging_offset));
			} else
				m = NULL;
		}
		if (upl->flags & UPL_SHADOWED) {
			if ((t = vm_page_lookup(object, target_offset))	!= VM_PAGE_NULL) {

				t->pageout = FALSE;

				VM_PAGE_FREE(t);

				if (!(upl->flags & UPL_KERNEL_OBJECT) && m == VM_PAGE_NULL)
					m = vm_page_lookup(shadow_object, target_offset + object->vo_shadow_offset);
			}
		}
		if (m == VM_PAGE_NULL)
			goto commit_next_page;

		if (m->compressor) {
			assert(m->busy);

			dwp->dw_mask |= (DW_clear_busy | DW_PAGE_WAKEUP);
			goto commit_next_page;
		}

		if (flags & UPL_COMMIT_CS_VALIDATED) {
			/*
			 * CODE SIGNING:
			 * Set the code signing bits according to
			 * what the UPL says they should be.
			 */
			m->cs_validated = page_list[entry].cs_validated;
			m->cs_tainted = page_list[entry].cs_tainted;
		}
		if (flags & UPL_COMMIT_WRITTEN_BY_KERNEL)
			m->written_by_kernel = TRUE;

		if (upl->flags & UPL_IO_WIRE) {

			if (page_list)
				page_list[entry].phys_addr = 0;

			if (flags & UPL_COMMIT_SET_DIRTY) {
				SET_PAGE_DIRTY(m, FALSE);
			} else if (flags & UPL_COMMIT_CLEAR_DIRTY) {
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
					if (m->slid) {
						panic("upl_commit_range(%p): page %p was slid\n",
						      upl, m);
					}
					assert(!m->slid);
					m->cs_validated = FALSE;
#if DEVELOPMENT || DEBUG
					vm_cs_validated_resets++;
#endif
					pmap_disconnect(m->phys_page);
				}
				clear_refmod |= VM_MEM_MODIFIED;
			}
			if (upl->flags & UPL_ACCESS_BLOCKED) {
				/*
				 * We blocked access to the pages in this UPL.
				 * Clear the "busy" bit and wake up any waiter
				 * for this page.
				 */
				dwp->dw_mask |= (DW_clear_busy | DW_PAGE_WAKEUP);
			}
			if (fast_path_possible) {
				assert(m->object->purgable != VM_PURGABLE_EMPTY);
				assert(m->object->purgable != VM_PURGABLE_VOLATILE);
				if (m->absent) {
					assert(m->wire_count == 0);
					assert(m->busy);

					m->absent = FALSE;
					dwp->dw_mask |= (DW_clear_busy | DW_PAGE_WAKEUP);
				} else {
					if (m->wire_count == 0)
						panic("wire_count == 0, m = %p, obj = %p\n", m, shadow_object);

					/*
					 * XXX FBDP need to update some other
					 * counters here (purgeable_wired_count)
					 * (ledgers), ...
					 */
					assert(m->wire_count);
					m->wire_count--;

					if (m->wire_count == 0)
						unwired_count++;
				}
				if (m->wire_count == 0) {
					queue_enter(&local_queue, m, vm_page_t, pageq);
					local_queue_count++;

					if (throttle_page) {
						m->throttled = TRUE;
					} else {
						if (flags & UPL_COMMIT_INACTIVATE)
							m->inactive = TRUE;
						else
							m->active = TRUE;
					}
				}
			} else {
				if (flags & UPL_COMMIT_INACTIVATE) {
					dwp->dw_mask |= DW_vm_page_deactivate_internal;
					clear_refmod |= VM_MEM_REFERENCED;
				}
				if (m->absent) {
					if (flags & UPL_COMMIT_FREE_ABSENT)
						dwp->dw_mask |= DW_vm_page_free;
					else {
						m->absent = FALSE;
						dwp->dw_mask |= (DW_clear_busy | DW_PAGE_WAKEUP);

						if ( !(dwp->dw_mask & DW_vm_page_deactivate_internal))
							dwp->dw_mask |= DW_vm_page_activate;
					}
				} else
					dwp->dw_mask |= DW_vm_page_unwire;
			}
			goto commit_next_page;
		}
		assert(!m->compressor);

		if (page_list)
			page_list[entry].phys_addr = 0;

		/*
		 * make sure to clear the hardware
		 * modify or reference bits before
		 * releasing the BUSY bit on this page
		 * otherwise we risk losing a legitimate
		 * change of state
		 */
		if (flags & UPL_COMMIT_CLEAR_DIRTY) {
			m->dirty = FALSE;

			clear_refmod |= VM_MEM_MODIFIED;
		}
		if (m->laundry)
			dwp->dw_mask |= DW_vm_pageout_throttle_up;

		if (VM_PAGE_WIRED(m))
			m->pageout = FALSE;
		
		if (! (flags & UPL_COMMIT_CS_VALIDATED) &&
		    m->cs_validated && !m->cs_tainted) {
			/*
			 * CODE SIGNING:
			 * This page is no longer dirty
			 * but could have been modified,
			 * so it will need to be
			 * re-validated.
			 */
			if (m->slid) {
				panic("upl_commit_range(%p): page %p was slid\n",
				      upl, m);
			}
			assert(!m->slid);
			m->cs_validated = FALSE;
#if DEVELOPMENT || DEBUG
			vm_cs_validated_resets++;
#endif
			pmap_disconnect(m->phys_page);
		}
		if (m->overwriting) {
			/*
			 * the (COPY_OUT_FROM == FALSE) request_page_list case
			 */
			if (m->busy) {
#if CONFIG_PHANTOM_CACHE
				if (m->absent && !m->object->internal)
					dwp->dw_mask |= DW_vm_phantom_cache_update;
#endif
				m->absent = FALSE;

				dwp->dw_mask |= DW_clear_busy;
			} else {
				/*
				 * alternate (COPY_OUT_FROM == FALSE) page_list case
				 * Occurs when the original page was wired
				 * at the time of the list request
				 */
				assert(VM_PAGE_WIRED(m));

				dwp->dw_mask |= DW_vm_page_unwire; /* reactivates */
			}
			m->overwriting = FALSE;
		}
		if (m->encrypted_cleaning == TRUE) {
			m->encrypted_cleaning = FALSE;

			dwp->dw_mask |= DW_clear_busy | DW_PAGE_WAKEUP;
		}
		m->cleaning = FALSE;

		if (m->pageout) {
			/* 
			 * With the clean queue enabled, UPL_PAGEOUT should
			 * no longer set the pageout bit. It's pages now go 
			 * to the clean queue.
			 */
			assert(!(flags & UPL_PAGEOUT));

			m->pageout = FALSE;
#if MACH_CLUSTER_STATS
			if (m->wanted) vm_pageout_target_collisions++;
#endif
			if ((flags & UPL_COMMIT_SET_DIRTY) ||
			    (m->pmapped && (pmap_disconnect(m->phys_page) & VM_MEM_MODIFIED))) {
				/*
				 * page was re-dirtied after we started
				 * the pageout... reactivate it since 
				 * we don't know whether the on-disk
				 * copy matches what is now in memory
				 */
				SET_PAGE_DIRTY(m, FALSE);
				
				dwp->dw_mask |= DW_vm_page_activate | DW_PAGE_WAKEUP;

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
				m->dirty = FALSE;
				m->busy = TRUE;

				dwp->dw_mask |= DW_vm_page_free;
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
		/*
		 * It is a part of the semantic of COPYOUT_FROM
		 * UPLs that a commit implies cache sync
		 * between the vm page and the backing store
		 * this can be used to strip the precious bit
		 * as well as clean
		 */
		if ((upl->flags & UPL_PAGE_SYNC_DONE) || (flags & UPL_COMMIT_CLEAR_PRECIOUS))
			m->precious = FALSE;

		if (flags & UPL_COMMIT_SET_DIRTY) {
			SET_PAGE_DIRTY(m, FALSE);
		} else {
			m->dirty = FALSE;
		}

		/* with the clean queue on, move *all* cleaned pages to the clean queue */
		if (hibernate_cleaning_in_progress == FALSE && !m->dirty && (upl->flags & UPL_PAGEOUT)) {
			pgpgout_count++;

			VM_STAT_INCR(pageouts);
			DTRACE_VM2(pgout, int, 1, (uint64_t *), NULL);

			dwp->dw_mask |= DW_enqueue_cleaned;
			vm_pageout_enqueued_cleaned_from_inactive_dirty++;
		} else if (should_be_throttled == TRUE && !m->active && !m->inactive && !m->speculative && !m->throttled) {
			/*
			 * page coming back in from being 'frozen'...
			 * it was dirty before it was frozen, so keep it so
			 * the vm_page_activate will notice that it really belongs
			 * on the throttle queue and put it there
			 */
			SET_PAGE_DIRTY(m, FALSE);
			dwp->dw_mask |= DW_vm_page_activate;

		} else {
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
				VM_PAGE_ADD_DELAYED_WORK(dwp, m, dw_count);

				if (dw_count >= dw_limit) {
					vm_page_do_delayed_work(shadow_object, &dw_array[0], dw_count);
			
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
		vm_page_do_delayed_work(shadow_object, &dw_array[0], dw_count);

	if (fast_path_possible) {

		assert(shadow_object->purgable != VM_PURGABLE_VOLATILE);
		assert(shadow_object->purgable != VM_PURGABLE_EMPTY);

		if (local_queue_count || unwired_count) {

			if (local_queue_count) {
				vm_page_t	first_local, last_local;
				vm_page_t	first_target;
				queue_head_t	*target_queue;

				if (throttle_page)
					target_queue = &vm_page_queue_throttled;
				else {
					if (flags & UPL_COMMIT_INACTIVATE) {
						if (shadow_object->internal)
							target_queue = &vm_page_queue_anonymous;
						else
							target_queue = &vm_page_queue_inactive;
					} else
						target_queue = &vm_page_queue_active;
				}
				/*
				 * Transfer the entire local queue to a regular LRU page queues.
				 */
				first_local = (vm_page_t) queue_first(&local_queue);
				last_local = (vm_page_t) queue_last(&local_queue);

				vm_page_lockspin_queues();

				first_target = (vm_page_t) queue_first(target_queue);

				if (queue_empty(target_queue))
					queue_last(target_queue) = (queue_entry_t) last_local;
				else
					queue_prev(&first_target->pageq) = (queue_entry_t) last_local;

				queue_first(target_queue) = (queue_entry_t) first_local;
				queue_prev(&first_local->pageq) = (queue_entry_t) target_queue;
				queue_next(&last_local->pageq) = (queue_entry_t) first_target;

				/*
				 * Adjust the global page counts.
				 */
				if (throttle_page) {
					vm_page_throttled_count += local_queue_count;
				} else {
					if (flags & UPL_COMMIT_INACTIVATE) {
						if (shadow_object->internal)
							vm_page_anonymous_count += local_queue_count;
						vm_page_inactive_count += local_queue_count;

						token_new_pagecount += local_queue_count;
					} else
						vm_page_active_count += local_queue_count;

					if (shadow_object->internal)
						vm_page_pageable_internal_count += local_queue_count;
					else
						vm_page_pageable_external_count += local_queue_count;
				}
			} else {
				vm_page_lockspin_queues();
			}
			if (unwired_count) {	
				vm_page_wire_count -= unwired_count;
				VM_CHECK_MEMORYSTATUS;
			}
			vm_page_unlock_queues();

			shadow_object->wired_page_count -= unwired_count;
		}
	}
	occupied = 1;

	if (upl->flags & UPL_DEVICE_MEMORY)  {
		occupied = 0;
	} else if (upl->flags & UPL_LITE) {
		int	pg_num;
		int	i;

		occupied = 0;

		if (!fast_path_full_commit) {
			pg_num = upl->size/PAGE_SIZE;
			pg_num = (pg_num + 31) >> 5;

			for (i = 0; i < pg_num; i++) {
				if (lite_list[i] != 0) {
					occupied = 1;
					break;
				}
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
			vm_object_collapse(shadow_object, 0, TRUE);
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
	upl_page_info_t		*user_page_list = NULL;
	upl_size_t		xfer_size, subupl_size = size;
	vm_object_t		shadow_object;
	vm_object_t		object;
	vm_object_offset_t	target_offset;
	upl_offset_t		subupl_offset = offset;
	int			entry;
	wpl_array_t 	 	lite_list;
	int			occupied;
	struct	vm_page_delayed_work	dw_array[DEFAULT_DELAYED_WORK_LIMIT];
	struct	vm_page_delayed_work	*dwp;
	int			dw_count;
	int			dw_limit;
	int			isVectorUPL = 0;
	upl_t			vector_upl = NULL;

	*empty = FALSE;

	if (upl == UPL_NULL)
		return KERN_INVALID_ARGUMENT;

	if ( (upl->flags & UPL_IO_WIRE) && !(error & UPL_ABORT_DUMP_PAGES) )
		return upl_commit_range(upl, offset, size, UPL_COMMIT_FREE_ABSENT, NULL, 0, empty);

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

		user_page_list = (upl_page_info_t *) (((uintptr_t)upl) + sizeof(struct upl));
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
	dw_limit = DELAYED_WORK_LIMIT(DEFAULT_DELAYED_WORK_LIMIT);

	if ((error & UPL_ABORT_DUMP_PAGES) && (upl->flags & UPL_KERNEL_OBJECT))
		panic("upl_abort_range: kernel_object being DUMPED");

	while (xfer_size) {
		vm_page_t	t, m;
		unsigned int	pg_num;
		boolean_t	needed;

		pg_num = (unsigned int) (target_offset/PAGE_SIZE);
		assert(pg_num == target_offset/PAGE_SIZE);

		needed = FALSE;

		if (user_page_list)
			needed = user_page_list[pg_num].needed;

		dwp->dw_mask = 0;
		m = VM_PAGE_NULL;

		if (upl->flags & UPL_LITE) {

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
					m = vm_page_lookup(shadow_object, target_offset + object->vo_shadow_offset);
			}
		}
		if ((upl->flags & UPL_KERNEL_OBJECT))
			goto abort_next_page;

		if (m != VM_PAGE_NULL) {

			assert(!m->compressor);

			if (m->absent) {
			        boolean_t must_free = TRUE;

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
				if (m->clustered && needed == FALSE) {
					/*
					 * This page was a part of a speculative
					 * read-ahead initiated by the kernel
					 * itself.  No one is expecting this
					 * page and no one will clean up its
					 * error state if it ever becomes valid
					 * in the future.
					 * We have to free it here.
					 */
					must_free = TRUE;
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

				if (m->overwriting && !m->busy) {
					/*
					 * this shouldn't happen since
					 * this is an 'absent' page, but
					 * it doesn't hurt to check for
					 * the 'alternate' method of 
					 * stabilizing the page...
					 * we will mark 'busy' to be cleared
					 * in the following code which will
					 * take care of the primary stabilzation
					 * method (i.e. setting 'busy' to TRUE)
					 */
					dwp->dw_mask |= DW_vm_page_unwire;
				}
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

				if (upl->flags & UPL_ACCESS_BLOCKED) {
					/*
					 * We blocked access to the pages in this UPL.
					 * Clear the "busy" bit and wake up any waiter
					 * for this page.
					 */
					dwp->dw_mask |= DW_clear_busy;
				}
				if (m->overwriting) {
					if (m->busy)
						dwp->dw_mask |= DW_clear_busy;
					else {
						/*
						 * deal with the 'alternate' method
						 * of stabilizing the page...
						 * we will either free the page
						 * or mark 'busy' to be cleared
						 * in the following code which will
						 * take care of the primary stabilzation
						 * method (i.e. setting 'busy' to TRUE)
						 */
						dwp->dw_mask |= DW_vm_page_unwire;
					}
					m->overwriting = FALSE;
				}
				if (m->encrypted_cleaning == TRUE) {
					m->encrypted_cleaning = FALSE;

					dwp->dw_mask |= DW_clear_busy;
				}
				m->pageout = FALSE;
				m->cleaning = FALSE;
#if	MACH_PAGEMAP
				vm_external_state_clr(m->object->existence_map, m->offset);
#endif	/* MACH_PAGEMAP */
				if (error & UPL_ABORT_DUMP_PAGES) {
					pmap_disconnect(m->phys_page);

					dwp->dw_mask |= DW_vm_page_free;
				} else {
					if (!(dwp->dw_mask & DW_vm_page_unwire)) {
						if (error & UPL_ABORT_REFERENCE) {
							/*
							 * we've been told to explictly
							 * reference this page... for 
							 * file I/O, this is done by
							 * implementing an LRU on the inactive q
							 */
							dwp->dw_mask |= DW_vm_page_lru;

						} else if (!m->active && !m->inactive && !m->speculative)
							dwp->dw_mask |= DW_vm_page_deactivate_internal;
					}
					dwp->dw_mask |= DW_PAGE_WAKEUP;
				}
			}
		}
abort_next_page:
		target_offset += PAGE_SIZE_64;
		xfer_size -= PAGE_SIZE;
		entry++;

		if (dwp->dw_mask) {
			if (dwp->dw_mask & ~(DW_clear_busy | DW_PAGE_WAKEUP)) {
				VM_PAGE_ADD_DELAYED_WORK(dwp, m, dw_count);

				if (dw_count >= dw_limit) {
					vm_page_do_delayed_work(shadow_object, &dw_array[0], dw_count);
				
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
		vm_page_do_delayed_work(shadow_object, &dw_array[0], dw_count);

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
			vm_object_collapse(shadow_object, 0, TRUE);
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


void
iopl_valid_data(
	upl_t	upl)
{
	vm_object_t	object;
	vm_offset_t	offset;
	vm_page_t	m, nxt_page = VM_PAGE_NULL;
	upl_size_t	size;
	int		wired_count = 0;

	if (upl == NULL)
		panic("iopl_valid_data: NULL upl");
	if (vector_upl_is_valid(upl))
		panic("iopl_valid_data: vector upl");
	if ((upl->flags & (UPL_DEVICE_MEMORY|UPL_SHADOWED|UPL_ACCESS_BLOCKED|UPL_IO_WIRE|UPL_INTERNAL)) != UPL_IO_WIRE)
		panic("iopl_valid_data: unsupported upl, flags = %x", upl->flags);

	object = upl->map_object;

	if (object == kernel_object || object == compressor_object)
		panic("iopl_valid_data: object == kernel or compressor");

	if (object->purgable == VM_PURGABLE_VOLATILE)
		panic("iopl_valid_data: object == VM_PURGABLE_VOLATILE");

	size = upl->size;

	vm_object_lock(object);

	if (object->vo_size == size && object->resident_page_count == (size / PAGE_SIZE))
		nxt_page = (vm_page_t)queue_first(&object->memq);
	else
		offset = 0 + upl->offset - object->paging_offset;

	while (size) {

		if (nxt_page != VM_PAGE_NULL) {
			m = nxt_page;
			nxt_page = (vm_page_t)queue_next(&nxt_page->listq);
		} else {
			m = vm_page_lookup(object, offset);
			offset += PAGE_SIZE;

			if (m == VM_PAGE_NULL)
				panic("iopl_valid_data: missing expected page at offset %lx", (long)offset);
		}
		if (m->busy) {
			if (!m->absent)
				panic("iopl_valid_data: busy page w/o absent");

			if (m->pageq.next || m->pageq.prev)
				panic("iopl_valid_data: busy+absent page on page queue");

			m->absent = FALSE;
			m->dirty = TRUE;
			m->wire_count++;
			wired_count++;
			
			PAGE_WAKEUP_DONE(m);
		}
		size -= PAGE_SIZE;
	}
	if (wired_count) {
		object->wired_page_count += wired_count;

		vm_page_lockspin_queues();
		vm_page_wire_count += wired_count;
		vm_page_unlock_queues();
	}
	vm_object_unlock(object);
}




void
vm_object_set_pmap_cache_attr(
		vm_object_t		object,
		upl_page_info_array_t	user_page_list,
		unsigned int		num_pages,
		boolean_t		batch_pmap_op)
{
	unsigned int    cache_attr = 0;

	cache_attr = object->wimg_bits & VM_WIMG_MASK;
	assert(user_page_list);
	if (cache_attr != VM_WIMG_USE_DEFAULT) {
		PMAP_BATCH_SET_CACHE_ATTR(object, user_page_list, cache_attr, num_pages, batch_pmap_op);
	}
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
	unsigned int		size_in_pages;
	u_int32_t		psize;
	kern_return_t		ret;
	vm_prot_t		prot;
	struct vm_object_fault_info fault_info;
	struct	vm_page_delayed_work	dw_array[DEFAULT_DELAYED_WORK_LIMIT];
	struct	vm_page_delayed_work	*dwp;
	int			dw_count;
	int			dw_limit;
	int			dw_index;
	boolean_t		caller_lookup;
	int			io_tracking_flag = 0;
	int			interruptible;

	boolean_t		set_cache_attr_needed = FALSE;
	boolean_t		free_wired_pages = FALSE;
	int			fast_path_possible = 0;
	

	if (cntrl_flags & ~UPL_VALID_FLAGS) {
		/*
		 * For forward compatibility's sake,
		 * reject any unknown flag.
		 */
		return KERN_INVALID_VALUE;
	}
	if (vm_lopage_needed == FALSE)
	        cntrl_flags &= ~UPL_NEED_32BIT_ADDR;

	if (cntrl_flags & UPL_NEED_32BIT_ADDR) {
	        if ( (cntrl_flags & (UPL_SET_IO_WIRE | UPL_SET_LITE)) != (UPL_SET_IO_WIRE | UPL_SET_LITE))
		        return KERN_INVALID_VALUE;

		if (object->phys_contiguous) {
		        if ((offset + object->vo_shadow_offset) >= (vm_object_offset_t)max_valid_dma_address)
			        return KERN_INVALID_ADDRESS;
	      
			if (((offset + object->vo_shadow_offset) + size) >= (vm_object_offset_t)max_valid_dma_address)
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
	if (cntrl_flags & (UPL_NOZEROFILL | UPL_NOZEROFILLIO))
	        no_zero_fill = TRUE;

	if (cntrl_flags & UPL_COPYOUT_FROM)
		prot = VM_PROT_READ;
	else
		prot = VM_PROT_READ | VM_PROT_WRITE;

	if ((!object->internal) && (object->paging_offset != 0))
		panic("vm_object_iopl_request: external object with non-zero paging offset\n");

#if CONFIG_IOSCHED || UPL_DEBUG
	if ((object->io_tracking && object != kernel_object) || upl_debug_enabled)
		io_tracking_flag |= UPL_CREATE_IO_TRACKING;
#endif

#if CONFIG_IOSCHED
	if (object->io_tracking) {
		/* Check if we're dealing with the kernel object. We do not support expedite on kernel object UPLs */
		if (object != kernel_object)
			io_tracking_flag |= UPL_CREATE_EXPEDITE_SUP;
	}
#endif

	if (object->phys_contiguous)
	        psize = PAGE_SIZE;
	else
	        psize = size;

	if (cntrl_flags & UPL_SET_INTERNAL) {
	        upl = upl_create(UPL_CREATE_INTERNAL | UPL_CREATE_LITE | io_tracking_flag, UPL_IO_WIRE, psize);

		user_page_list = (upl_page_info_t *) (((uintptr_t)upl) + sizeof(struct upl));
		lite_list = (wpl_array_t) (((uintptr_t)user_page_list) +
					   ((psize / PAGE_SIZE) * sizeof(upl_page_info_t)));
		if (size == 0) {
			user_page_list = NULL;
			lite_list = NULL;
		}
	} else {
	        upl = upl_create(UPL_CREATE_LITE | io_tracking_flag, UPL_IO_WIRE, psize);

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

	size_in_pages = size / PAGE_SIZE;

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
		 * The user requested that access to the pages in this UPL
		 * be blocked until the UPL is commited or aborted.
		 */
		upl->flags |= UPL_ACCESS_BLOCKED;
	}

	if (!(cntrl_flags & (UPL_NEED_32BIT_ADDR | UPL_BLOCK_ACCESS)) &&
	    object->purgable != VM_PURGABLE_VOLATILE &&
	    object->purgable != VM_PURGABLE_EMPTY &&
	    object->copy == NULL &&
	    size == object->vo_size &&
	    offset == 0 &&
	    object->resident_page_count == 0 &&
	    object->shadow == NULL &&
	    object->pager == NULL)
	{
		fast_path_possible = 1;
		set_cache_attr_needed = TRUE;
	}

#if CONFIG_IOSCHED || UPL_DEBUG
	if (upl->flags & UPL_TRACKED_BY_OBJECT) {
		vm_object_activity_begin(object);
		queue_enter(&object->uplq, upl, upl_t, uplq);
	}
#endif

	if (object->phys_contiguous) {

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

		upl->highest_page = (ppnum_t) ((offset + object->vo_shadow_offset + size - 1)>>PAGE_SHIFT);

		if (user_page_list) {
		        user_page_list[0].phys_addr = (ppnum_t) ((offset + object->vo_shadow_offset)>>PAGE_SHIFT);
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
	if (object != kernel_object && object != compressor_object) {
		/*
		 * Protect user space from future COW operations
		 */
#if VM_OBJECT_TRACKING_OP_TRUESHARE
		if (!object->true_share &&
		    vm_object_tracking_inited) {
			void *bt[VM_OBJECT_TRACKING_BTDEPTH];
			int num = 0;

			num = OSBacktrace(bt,
					  VM_OBJECT_TRACKING_BTDEPTH);
			btlog_add_entry(vm_object_tracking_btlog,
					object,
					VM_OBJECT_TRACKING_OP_TRUESHARE,
					bt,
					num);
		}
#endif /* VM_OBJECT_TRACKING_OP_TRUESHARE */

		object->true_share = TRUE;

		if (object->copy_strategy == MEMORY_OBJECT_COPY_SYMMETRIC)
			object->copy_strategy = MEMORY_OBJECT_COPY_DELAY;
	}

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
	if (cntrl_flags & UPL_SET_INTERRUPTIBLE)
		interruptible = THREAD_ABORTSAFE;
	else
		interruptible = THREAD_UNINT;

	entry = 0;

	xfer_size = size;
	dst_offset = offset;
	dw_count = 0;

	if (fast_path_possible) {
		int	wired_count = 0;

		while (xfer_size) {
			
			while ( (dst_page = vm_page_grab()) == VM_PAGE_NULL) {
				OSAddAtomic(size_in_pages, &vm_upl_wait_for_pages);

				VM_DEBUG_EVENT(vm_iopl_page_wait, VM_IOPL_PAGE_WAIT, DBG_FUNC_START, vm_upl_wait_for_pages, 0, 0, 0);

				if (vm_page_wait(interruptible) == FALSE) {
					/*
					 * interrupted case
					 */
					OSAddAtomic(-size_in_pages, &vm_upl_wait_for_pages);

					VM_DEBUG_EVENT(vm_iopl_page_wait, VM_IOPL_PAGE_WAIT, DBG_FUNC_END, vm_upl_wait_for_pages, 0, 0, -1);

					if (wired_count) {
						vm_page_lockspin_queues();
						vm_page_wire_count += wired_count;
						vm_page_unlock_queues();

						free_wired_pages = TRUE;
					}
					ret = MACH_SEND_INTERRUPTED;

					goto return_err;
				}
				OSAddAtomic(-size_in_pages, &vm_upl_wait_for_pages);

				VM_DEBUG_EVENT(vm_iopl_page_wait, VM_IOPL_PAGE_WAIT, DBG_FUNC_END, vm_upl_wait_for_pages, 0, 0, 0);
			}
			if (no_zero_fill == FALSE)
				vm_page_zero_fill(dst_page);
			else
				dst_page->absent = TRUE;

			dst_page->reference = TRUE;

			if (!(cntrl_flags & UPL_COPYOUT_FROM)) {
				SET_PAGE_DIRTY(dst_page, FALSE);	
			}
			if (dst_page->absent == FALSE) {
				assert(object->purgable != VM_PURGABLE_VOLATILE);
				assert(object->purgable != VM_PURGABLE_EMPTY);
				dst_page->wire_count++;
				wired_count++;

				PAGE_WAKEUP_DONE(dst_page);
			}
			vm_page_insert_internal(dst_page, object, dst_offset, FALSE, TRUE, TRUE);
			
			lite_list[entry>>5] |= 1 << (entry & 31);
		
			if (dst_page->phys_page > upl->highest_page)
				upl->highest_page = dst_page->phys_page;

			if (user_page_list) {
				user_page_list[entry].phys_addr	= dst_page->phys_page;
				user_page_list[entry].absent	= dst_page->absent;
				user_page_list[entry].dirty 	= dst_page->dirty;
				user_page_list[entry].precious	= FALSE;
				user_page_list[entry].pageout	= FALSE;
				user_page_list[entry].device 	= FALSE;
				user_page_list[entry].needed    = FALSE;
			        user_page_list[entry].speculative = FALSE;
				user_page_list[entry].cs_validated = FALSE;
				user_page_list[entry].cs_tainted = FALSE;
			}
			entry++;
			dst_offset += PAGE_SIZE_64;
			xfer_size -= PAGE_SIZE;
			size_in_pages--;
		}
		if (wired_count) {
			vm_page_lockspin_queues();
			vm_page_wire_count += wired_count;
			vm_page_unlock_queues();
		}
		goto finish;
	}

	fault_info.behavior = VM_BEHAVIOR_SEQUENTIAL;
	fault_info.user_tag  = 0;
	fault_info.lo_offset = offset;
	fault_info.hi_offset = offset + xfer_size;
	fault_info.no_cache  = FALSE;
	fault_info.stealth = FALSE;
	fault_info.io_sync = FALSE;
	fault_info.cs_bypass = FALSE;
	fault_info.mark_zf_absent = TRUE;
	fault_info.interruptible = interruptible;
	fault_info.batch_pmap_op = TRUE;

	dwp = &dw_array[0];
	dw_limit = DELAYED_WORK_LIMIT(DEFAULT_DELAYED_WORK_LIMIT);

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
		   if (object == compressor_object)
			   panic("vm_object_iopl_request: missing/bad page in compressor object\n");

		   if (cntrl_flags & UPL_REQUEST_NO_FAULT) {
			   ret = KERN_MEMORY_ERROR;
			   goto return_err;
		   }
		   set_cache_attr_needed = TRUE;

                   /*
		    * We just looked up the page and the result remains valid
		    * until the object lock is release, so send it to
		    * vm_fault_page() (as "dst_page"), to avoid having to
		    * look it up again there.
		    */
		   caller_lookup = TRUE;

		   do {
			vm_page_t	top_page;
			kern_return_t	error_code;

			fault_info.cluster_size = xfer_size;

			vm_object_paging_begin(object);

			result = vm_fault_page(object, dst_offset,
					       prot | VM_PROT_WRITE, FALSE,
					       caller_lookup,
					       &prot, &dst_page, &top_page,
					       (int *)0,
					       &error_code, no_zero_fill,
					       FALSE, &fault_info);

                        /* our lookup is no longer valid at this point */
			caller_lookup = FALSE;

			switch (result) {

			case VM_FAULT_SUCCESS:

				if ( !dst_page->absent) {
					PAGE_WAKEUP_DONE(dst_page);
				} else {
					/*
					 * we only get back an absent page if we
					 * requested that it not be zero-filled
					 * because we are about to fill it via I/O
					 * 
					 * absent pages should be left BUSY
					 * to prevent them from being faulted
					 * into an address space before we've
					 * had a chance to complete the I/O on
					 * them since they may contain info that
					 * shouldn't be seen by the faulting task
					 */
				}
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

			case VM_FAULT_MEMORY_SHORTAGE:
				OSAddAtomic(size_in_pages, &vm_upl_wait_for_pages);

				VM_DEBUG_EVENT(vm_iopl_page_wait, VM_IOPL_PAGE_WAIT, DBG_FUNC_START, vm_upl_wait_for_pages, 0, 0, 0);

				if (vm_page_wait(interruptible)) {
					OSAddAtomic(-size_in_pages, &vm_upl_wait_for_pages);

					VM_DEBUG_EVENT(vm_iopl_page_wait, VM_IOPL_PAGE_WAIT, DBG_FUNC_END, vm_upl_wait_for_pages, 0, 0, 0);
					vm_object_lock(object);

					break;
				}
				OSAddAtomic(-size_in_pages, &vm_upl_wait_for_pages);

				VM_DEBUG_EVENT(vm_iopl_page_wait, VM_IOPL_PAGE_WAIT, DBG_FUNC_END, vm_upl_wait_for_pages, 0, 0, -1);

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

		if (dst_page->compressor) {
			dst_page->busy = TRUE;
			goto record_phys_addr;
		}

		if (dst_page->cleaning) {
			/*
			 * Someone else is cleaning this page in place.
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
		if (dst_page->laundry) {
			dst_page->pageout = FALSE;
			
			vm_pageout_steal_laundry(dst_page, FALSE);
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

			if (!dst_page->absent)
				vm_page_copy(dst_page, low_page);
		  
			low_page->reference = dst_page->reference;
			low_page->dirty     = dst_page->dirty;
			low_page->absent    = dst_page->absent;

			if (refmod & VM_MEM_REFERENCED)
			        low_page->reference = TRUE;
			if (refmod & VM_MEM_MODIFIED) {
			        SET_PAGE_DIRTY(low_page, FALSE);
			}

			vm_page_replace(low_page, object, dst_offset);

			dst_page = low_page;
			/*
			 * vm_page_grablo returned the page marked
			 * BUSY... we don't need a PAGE_WAKEUP_DONE
			 * here, because we've never dropped the object lock
			 */
			if ( !dst_page->absent)
				dst_page->busy = FALSE;
		}
		if ( !dst_page->busy)
			dwp->dw_mask |= DW_vm_page_wire;

		if (cntrl_flags & UPL_BLOCK_ACCESS) {
			/*
			 * Mark the page "busy" to block any future page fault
			 * on this page in addition to wiring it.
			 * We'll also remove the mapping
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

   		if (!(cntrl_flags & UPL_COPYOUT_FROM)) {
			SET_PAGE_DIRTY(dst_page, TRUE);	
		}
		if ((cntrl_flags & UPL_REQUEST_FORCE_COHERENCY) && dst_page->written_by_kernel == TRUE) {
			pmap_sync_page_attributes_phys(dst_page->phys_page);
			dst_page->written_by_kernel = FALSE;
		}

record_phys_addr:
		if (dst_page->busy)
			upl->flags |= UPL_HAS_BUSY;

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
			user_page_list[entry].needed    = FALSE;
			if (dst_page->clustered == TRUE)
			        user_page_list[entry].speculative = dst_page->speculative;
			else
			        user_page_list[entry].speculative = FALSE;
			user_page_list[entry].cs_validated = dst_page->cs_validated;
			user_page_list[entry].cs_tainted = dst_page->cs_tainted;
		}
		if (object != kernel_object && object != compressor_object) {
			/*
			 * someone is explicitly grabbing this page...
			 * update clustered and speculative state
			 * 
			 */
			if (dst_page->clustered)
				VM_PAGE_CONSUME_CLUSTERED(dst_page);
		}
		entry++;
		dst_offset += PAGE_SIZE_64;
		xfer_size -= PAGE_SIZE;
		size_in_pages--;

		if (dwp->dw_mask) {
			VM_PAGE_ADD_DELAYED_WORK(dwp, dst_page, dw_count);

			if (dw_count >= dw_limit) {
				vm_page_do_delayed_work(object, &dw_array[0], dw_count);
				
				dwp = &dw_array[0];
				dw_count = 0;
			}
		}
	}
	if (dw_count)
		vm_page_do_delayed_work(object, &dw_array[0], dw_count);

finish:
	if (user_page_list && set_cache_attr_needed == TRUE)
		vm_object_set_pmap_cache_attr(object, user_page_list, entry, TRUE);

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
		boolean_t need_unwire;

	        dst_page = vm_page_lookup(object, offset);

		if (dst_page == VM_PAGE_NULL)
		        panic("vm_object_iopl_request: Wired page missing. \n");

		/*
		 * if we've already processed this page in an earlier 
		 * dw_do_work, we need to undo the wiring... we will
		 * leave the dirty and reference bits on if they
		 * were set, since we don't have a good way of knowing
		 * what the previous state was and we won't get here
		 * under any normal circumstances...  we will always
		 * clear BUSY and wakeup any waiters via vm_page_free
		 * or PAGE_WAKEUP_DONE
		 */
		need_unwire = TRUE;

		if (dw_count) {
			if (dw_array[dw_index].dw_m == dst_page) {
				/*
				 * still in the deferred work list
				 * which means we haven't yet called
				 * vm_page_wire on this page
				 */
				need_unwire = FALSE;

				dw_index++;
				dw_count--;
			}
		}
		vm_page_lock_queues();

		if (dst_page->absent || free_wired_pages == TRUE) {
			vm_page_free(dst_page);

			need_unwire = FALSE;
		} else {
			if (need_unwire == TRUE)
				vm_page_unwire(dst_page, TRUE);

			PAGE_WAKEUP_DONE(dst_page);
		}
		vm_page_unlock_queues();

		if (need_unwire == TRUE)
			VM_STAT_INCR(reactivations);
	}
#if UPL_DEBUG
	upl->upl_state = 2;
#endif
	if (! (upl->flags & UPL_KERNEL_OBJECT)) {
		vm_object_activity_end(object);
		vm_object_collapse(object, 0, TRUE);
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
#if CONFIG_IOSCHED || UPL_DEBUG
		if ((upl1->flags & UPL_TRACKED_BY_OBJECT) || (upl2->flags & UPL_TRACKED_BY_OBJECT)) {
			vm_object_lock(object1);
			vm_object_lock(object2);
		}
		if (upl1->flags & UPL_TRACKED_BY_OBJECT)
			queue_remove(&object1->uplq, upl1, upl_t, uplq);
		if (upl2->flags & UPL_TRACKED_BY_OBJECT)
			queue_remove(&object2->uplq, upl2, upl_t, uplq);
#endif
		upl1->map_object = object2;
		upl2->map_object = object1;

#if CONFIG_IOSCHED || UPL_DEBUG
		if (upl1->flags & UPL_TRACKED_BY_OBJECT)
			queue_enter(&object2->uplq, upl1, upl_t, uplq);
		if (upl2->flags & UPL_TRACKED_BY_OBJECT)
			queue_enter(&object1->uplq, upl2, upl_t, uplq);
		if ((upl1->flags & UPL_TRACKED_BY_OBJECT) || (upl2->flags & UPL_TRACKED_BY_OBJECT)) {
			vm_object_unlock(object2);
			vm_object_unlock(object1);
		}
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

void
upl_range_needed(
	upl_t		upl,
	int		index,
	int		count)
{
	upl_page_info_t	*user_page_list;
	int		size_in_pages;

	if ( !(upl->flags & UPL_INTERNAL) || count <= 0)
		return;

	size_in_pages = upl->size / PAGE_SIZE;

	user_page_list = (upl_page_info_t *) (((uintptr_t)upl) + sizeof(struct upl));

	while (count-- && index < size_in_pages)
		user_page_list[index++].needed = TRUE;
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
	map_entry->protection = VM_PROT_NONE;
	map_entry->max_protection = VM_PROT_NONE;
	map_entry->permanent = TRUE;
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
	vm_page_t		page,
	vm_object_t		object,
	vm_object_offset_t	offset,
	vm_prot_t		protection,
	boolean_t		can_unlock_object,
	vm_map_size_t		*size,		/* IN/OUT */
	vm_map_offset_t		*address,	/* OUT */
	boolean_t		*need_unmap)	/* OUT */
{
	kern_return_t		kr;
	vm_map_offset_t		page_map_offset;
	vm_map_size_t		map_size;
	vm_object_offset_t	object_offset;
	int			i;

	if (page != VM_PAGE_NULL && *size == PAGE_SIZE) {
		/* use permanent 1-to-1 kernel mapping of physical memory ? */
#if __x86_64__
		*address = (vm_map_offset_t)
			PHYSMAP_PTOV((pmap_paddr_t)page->phys_page <<
				     PAGE_SHIFT);
		*need_unmap = FALSE;
		return KERN_SUCCESS;
#else
#warn "vm_paging_map_object: no 1-to-1 kernel mapping of physical memory..."
#endif

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
			kr = assert_wait((event_t)&vm_paging_page_waiter, THREAD_UNINT);
			if (kr == THREAD_WAITING) {
				simple_unlock(&vm_paging_lock);
				kr = thread_block(THREAD_CONTINUE_NULL);
				simple_lock(&vm_paging_lock);
			}
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
				   VM_PROT_NONE,
				   0,
				   TRUE);
			vm_paging_objects_mapped++;
			vm_paging_pages_mapped++; 
			*address = page_map_offset;
			*need_unmap = TRUE;

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
		*address = 0;
		*size = 0;
		*need_unmap = FALSE;
		return KERN_NOT_SUPPORTED;
	}

	object_offset = vm_object_trunc_page(offset);
	map_size = vm_map_round_page(*size,
				     VM_MAP_PAGE_MASK(kernel_map));

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
		*need_unmap = FALSE;
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

		page = vm_page_lookup(object, offset + page_map_offset);
		if (page == VM_PAGE_NULL) {
			printf("vm_paging_map_object: no page !?");
			vm_object_unlock(object);
			kr = vm_map_remove(kernel_map, *address, *size,
					   VM_MAP_NO_FLAGS);
			assert(kr == KERN_SUCCESS);
			*address = 0;
			*size = 0;
			*need_unmap = FALSE;
			vm_object_lock(object);
			return KERN_MEMORY_ERROR;
		}
		page->pmapped = TRUE;

		//assert(pmap_verify_free(page->phys_page));
		PMAP_ENTER(kernel_pmap,
			   *address + page_map_offset,
			   page,
			   protection,
			   VM_PROT_NONE,
			   0,
			   TRUE);
	}
			   
	vm_paging_objects_mapped_slow++;
	vm_paging_pages_mapped_slow += (unsigned long) (map_size / PAGE_SIZE_64);

	*need_unmap = TRUE;

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

#if ENCRYPTED_SWAP
/*
 * Encryption data.
 * "iv" is the "initial vector".  Ideally, we want to
 * have a different one for each page we encrypt, so that
 * crackers can't find encryption patterns too easily.
 */
#define SWAP_CRYPT_AES_KEY_SIZE	128	/* XXX 192 and 256 don't work ! */
boolean_t		swap_crypt_ctx_initialized = FALSE;
uint32_t 		swap_crypt_key[8]; /* big enough for a 256 key */
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
	boolean_t		kernel_mapping_needs_unmap;
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
	
	if (page->encrypted) {
		/*
		 * Already encrypted: no need to do it again.
		 */
		vm_page_encrypt_already_encrypted_counter++;
		return;
	}
	assert(page->dirty || page->precious);

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
		kernel_mapping_needs_unmap = FALSE;
		kr = vm_paging_map_object(page,
					  page->object,
					  page->offset,
					  VM_PROT_READ | VM_PROT_WRITE,
					  FALSE,
					  &kernel_mapping_size,
					  &kernel_mapping_offset,
					  &kernel_mapping_needs_unmap);
		if (kr != KERN_SUCCESS) {
			panic("vm_page_encrypt: "
			      "could not map page in kernel: 0x%x\n",
			      kr);
		}
	} else {
		kernel_mapping_size = 0;
		kernel_mapping_needs_unmap = FALSE;
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
	if (kernel_mapping_needs_unmap) {
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
	boolean_t		kernel_mapping_needs_unmap;
	union {
		unsigned char	aes_iv[AES_BLOCK_SIZE];
		struct {
			memory_object_t		pager_object;
			vm_object_offset_t	paging_offset;
		} vm;
	} decrypt_iv;
	boolean_t		was_dirty;

	assert(page->busy);
	assert(page->encrypted);

	was_dirty = page->dirty;

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
		kernel_mapping_needs_unmap = FALSE;
		kr = vm_paging_map_object(page,
					  page->object,
					  page->offset,
					  VM_PROT_READ | VM_PROT_WRITE,
					  FALSE,
					  &kernel_mapping_size,
					  &kernel_mapping_offset,
					  &kernel_mapping_needs_unmap);
		if (kr != KERN_SUCCESS) {
			panic("vm_page_decrypt: "
			      "could not map page in kernel: 0x%x\n",
			      kr);
		}
	} else {
		kernel_mapping_size = 0;
		kernel_mapping_needs_unmap = FALSE;
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
	if (kernel_mapping_needs_unmap) {
		vm_paging_unmap_object(page->object,
				       kernel_vaddr,
				       kernel_vaddr + PAGE_SIZE);
	}

	if (was_dirty) {
		/*
		 * The pager did not specify that the page would be
		 * clean when it got paged in, so let's not clean it here
		 * either.
		 */
	} else {
		/*
		 * After decryption, the page is actually still clean.
		 * It was encrypted as part of paging, which "cleans"
		 * the "dirty" pages.
		 * Noone could access it after it was encrypted
		 * and the decryption doesn't count.
		 */
		page->dirty = FALSE;
		assert (page->cs_validated == FALSE);
		pmap_clear_refmod(page->phys_page, VM_MEM_MODIFIED | VM_MEM_REFERENCED);
	}
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
			      "no page for (obj=%p,off=0x%llx+0x%x)!\n",
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

#else /* ENCRYPTED_SWAP */
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

#endif /* ENCRYPTED_SWAP */

/*
 * page->object must be locked
 */
void
vm_pageout_steal_laundry(vm_page_t page, boolean_t queues_locked)
{
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
			vector_upl = (vector_upl_t)0xfeedfeed;
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
		if(vector_upl == NULL || vector_upl == (vector_upl_t)0xfeedfeed || vector_upl == (vector_upl_t)0xfeedbeef)
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

void
upl_set_referenced(
	upl_t		upl,
	boolean_t 	value)
{
	upl_lock(upl);
	if (value) {
		upl->ext_ref_count++;
	} else {
		if (!upl->ext_ref_count) {
			panic("upl_set_referenced not %p\n", upl);
		}
		upl->ext_ref_count--;
	}
	upl_unlock(upl);
}

#if CONFIG_IOSCHED
void
upl_set_blkno(
	upl_t		upl,
	vm_offset_t	upl_offset,
	int		io_size,
	int64_t		blkno)
{
		int i,j;
		if ((upl->flags & UPL_EXPEDITE_SUPPORTED) == 0)
			return;
			
		assert(upl->upl_reprio_info != 0);	
		for(i = (int)(upl_offset / PAGE_SIZE), j = 0; j < io_size; i++, j += PAGE_SIZE) {
			UPL_SET_REPRIO_INFO(upl, i, blkno, io_size);
		}
}
#endif

boolean_t
vm_page_is_slideable(vm_page_t m)
{
	boolean_t result = FALSE;
	vm_shared_region_slide_info_t si;

	vm_object_lock_assert_held(m->object);

	/* make sure our page belongs to the one object allowed to do this */
	if (!m->object->object_slid) {
		goto done;
	}

	si = m->object->vo_slide_info;
	if (si == NULL) {
		goto done;
	}

	if(!m->slid && (si->start <= m->offset && si->end > m->offset)) {
		result = TRUE;
	}

done:
	return result;
}

int vm_page_slide_counter = 0;
int vm_page_slide_errors = 0;
kern_return_t
vm_page_slide(
	vm_page_t	page,
	vm_map_offset_t	kernel_mapping_offset)
{
	kern_return_t		kr;
	vm_map_size_t		kernel_mapping_size;
	boolean_t		kernel_mapping_needs_unmap;
	vm_offset_t		kernel_vaddr;
	uint32_t		pageIndex = 0;

	assert(!page->slid);
	assert(page->object->object_slid);
	vm_object_lock_assert_exclusive(page->object);

	if (page->error)
		return KERN_FAILURE;
	
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
		kernel_mapping_needs_unmap = FALSE;
		kr = vm_paging_map_object(page,
					  page->object,
					  page->offset,
					  VM_PROT_READ | VM_PROT_WRITE,
					  FALSE,
					  &kernel_mapping_size,
					  &kernel_mapping_offset,
					  &kernel_mapping_needs_unmap);
		if (kr != KERN_SUCCESS) {
			panic("vm_page_slide: "
			      "could not map page in kernel: 0x%x\n",
			      kr);
		}
	} else {
		kernel_mapping_size = 0;
		kernel_mapping_needs_unmap = FALSE;
	}
	kernel_vaddr = CAST_DOWN(vm_offset_t, kernel_mapping_offset);

	/*
	 * Slide the pointers on the page.
	 */

	/*assert that slide_file_info.start/end are page-aligned?*/

	assert(!page->slid);
	assert(page->object->object_slid);

	/* on some platforms this is an extern int, on others it's a cpp macro */
	__unreachable_ok_push
        /* TODO: Consider this */
	if (!TEST_PAGE_SIZE_4K) {
		for (int i = 0; i < 4; i++) {
			pageIndex = (uint32_t)((page->offset - page->object->vo_slide_info->start)/0x1000);
			kr = vm_shared_region_slide_page(page->object->vo_slide_info, kernel_vaddr + (0x1000*i), pageIndex + i);
		}
	} else {
		pageIndex = (uint32_t)((page->offset - page->object->vo_slide_info->start)/PAGE_SIZE);
		kr = vm_shared_region_slide_page(page->object->vo_slide_info, kernel_vaddr, pageIndex);
	}
	__unreachable_ok_pop

	vm_page_slide_counter++;

	/*
	 * Unmap the page from the kernel's address space,
	 */
	if (kernel_mapping_needs_unmap) {
		vm_paging_unmap_object(page->object,
				       kernel_vaddr,
				       kernel_vaddr + PAGE_SIZE);
	}
	
	page->dirty = FALSE;
	pmap_clear_refmod(page->phys_page, VM_MEM_MODIFIED | VM_MEM_REFERENCED);
	
	if (kr != KERN_SUCCESS || cs_debug > 1) {
		printf("vm_page_slide(%p): "
		       "obj %p off 0x%llx mobj %p moff 0x%llx\n",
		       page,
		       page->object, page->offset,
		       page->object->pager,
		       page->offset + page->object->paging_offset);
	}

	if (kr == KERN_SUCCESS) {
		page->slid = TRUE;
	} else {
		page->error = TRUE;
		vm_page_slide_errors++;
	}

	vm_object_paging_end(page->object);

	return kr;
}

void inline memoryshot(unsigned int event, unsigned int control)
{
	if (vm_debug_events) {
		KERNEL_DEBUG_CONSTANT1((MACHDBG_CODE(DBG_MACH_VM_PRESSURE, event)) | control,
					vm_page_active_count, vm_page_inactive_count,
					vm_page_free_count, vm_page_speculative_count,
					vm_page_throttled_count);
	} else {
		(void) event;
		(void) control;
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
	m = (vm_page_t) queue_first(&vm_page_queue_anonymous);
	do {
		if (m ==(vm_page_t )0) break;

		if(m->dirty) dpages++;
		if(m->pageout) pgopages++;
		if(m->precious) precpages++;

		assert(m->object != kernel_object);
		m = (vm_page_t) queue_next(&m->pageq);
		if (m ==(vm_page_t )0) break;

	} while (!queue_end(&vm_page_queue_anonymous,(queue_entry_t) m));
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

#if VM_PRESSURE_EVENTS
/*
 * Upward trajectory.
 */
extern boolean_t vm_compressor_low_on_space(void);

boolean_t
VM_PRESSURE_NORMAL_TO_WARNING(void)	{

	if (DEFAULT_PAGER_IS_ACTIVE || DEFAULT_FREEZER_IS_ACTIVE || DEFAULT_FREEZER_COMPRESSED_PAGER_IS_SWAPLESS) {
		
		/* Available pages below our threshold */
		if (memorystatus_available_pages < memorystatus_available_pages_pressure) {
			/* No frozen processes to kill */
			if (memorystatus_frozen_count == 0) {
				/* Not enough suspended processes available. */
				if (memorystatus_suspended_count < MEMORYSTATUS_SUSPENDED_THRESHOLD) {
					return TRUE;
				}
			}
		}
		return FALSE;

	} else {
		return ((AVAILABLE_NON_COMPRESSED_MEMORY < VM_PAGE_COMPRESSOR_COMPACT_THRESHOLD) ? 1 : 0);
	}
}

boolean_t
VM_PRESSURE_WARNING_TO_CRITICAL(void) {

	if (DEFAULT_PAGER_IS_ACTIVE || DEFAULT_FREEZER_IS_ACTIVE || DEFAULT_FREEZER_COMPRESSED_PAGER_IS_SWAPLESS) {
		/* Available pages below our threshold */
		if (memorystatus_available_pages < memorystatus_available_pages_critical) {
			return TRUE;
		}
		return FALSE;
	} else {
		return (vm_compressor_low_on_space() || (AVAILABLE_NON_COMPRESSED_MEMORY < ((12 * VM_PAGE_COMPRESSOR_SWAP_UNTHROTTLE_THRESHOLD) / 10)) ? 1 : 0);
	}
}

/*
 * Downward trajectory.
 */
boolean_t
VM_PRESSURE_WARNING_TO_NORMAL(void) {

	if (DEFAULT_PAGER_IS_ACTIVE || DEFAULT_FREEZER_IS_ACTIVE || DEFAULT_FREEZER_COMPRESSED_PAGER_IS_SWAPLESS) {
		/* Available pages above our threshold */
		unsigned int target_threshold = memorystatus_available_pages_pressure + ((15 * memorystatus_available_pages_pressure) / 100);
		if (memorystatus_available_pages > target_threshold) {
			return TRUE;
		}
		return FALSE;
	} else {
		return ((AVAILABLE_NON_COMPRESSED_MEMORY > ((12 * VM_PAGE_COMPRESSOR_COMPACT_THRESHOLD) / 10)) ? 1 : 0);
	}
}

boolean_t
VM_PRESSURE_CRITICAL_TO_WARNING(void) {

	if (DEFAULT_PAGER_IS_ACTIVE || DEFAULT_FREEZER_IS_ACTIVE || DEFAULT_FREEZER_COMPRESSED_PAGER_IS_SWAPLESS) {
		/* Available pages above our threshold */
		unsigned int target_threshold = memorystatus_available_pages_critical + ((15 * memorystatus_available_pages_critical) / 100);
		if (memorystatus_available_pages > target_threshold) {
			return TRUE;
		}
		return FALSE;
	} else {
		return ((AVAILABLE_NON_COMPRESSED_MEMORY > ((14 * VM_PAGE_COMPRESSOR_SWAP_UNTHROTTLE_THRESHOLD) / 10)) ? 1 : 0);
	}
}
#endif /* VM_PRESSURE_EVENTS */


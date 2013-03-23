/*
 * Copyright (c) 2000-2007 Apple Inc. All rights reserved.
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
 *	File:	vm_object.h
 *	Author:	Avadis Tevanian, Jr., Michael Wayne Young
 *	Date:	1985
 *
 *	Virtual memory object module definitions.
 */

#ifndef	_VM_VM_OBJECT_H_
#define _VM_VM_OBJECT_H_

#include <mach_pagemap.h>
#include <task_swapper.h>

#include <mach/kern_return.h>
#include <mach/boolean.h>
#include <mach/memory_object_types.h>
#include <mach/port.h>
#include <mach/vm_prot.h>
#include <mach/vm_param.h>
#include <mach/machine/vm_types.h>
#include <kern/queue.h>
#include <kern/lock.h>
#include <kern/locks.h>
#include <kern/assert.h>
#include <kern/misc_protos.h>
#include <kern/macro_help.h>
#include <ipc/ipc_types.h>
#include <vm/pmap.h>

#if	MACH_PAGEMAP
#include <vm/vm_external.h>
#endif	/* MACH_PAGEMAP */

struct vm_page;

/*
 *	Types defined:
 *
 *	vm_object_t		Virtual memory object.
 *	vm_object_fault_info_t	Used to determine cluster size.
 */

struct vm_object_fault_info {
	int		interruptible;
        uint32_t	user_tag;
        vm_size_t	cluster_size;
        vm_behavior_t	behavior;
        vm_map_offset_t	lo_offset;
	vm_map_offset_t	hi_offset;
	boolean_t	no_cache;
};



struct vm_object {
	queue_head_t		memq;		/* Resident memory */
        lck_rw_t		Lock;		/* Synchronization */

	vm_object_size_t	size;		/* Object size (only valid
						 * if internal)
						 */
	struct vm_page		*memq_hint;
	int			ref_count;	/* Number of references */
#if	TASK_SWAPPER
	int			res_count;	/* Residency references (swap)*/
#endif	/* TASK_SWAPPER */
	unsigned int		resident_page_count;
						/* number of resident pages */

	struct vm_object	*copy;		/* Object that should receive
						 * a copy of my changed pages,
						 * for copy_delay, or just the
						 * temporary object that
						 * shadows this object, for
						 * copy_call.
						 */
	struct vm_object	*shadow;	/* My shadow */
	vm_object_offset_t	shadow_offset;	/* Offset into shadow */

	memory_object_t		pager;		/* Where to get data */
	vm_object_offset_t	paging_offset;	/* Offset into memory object */
	memory_object_control_t	pager_control;	/* Where data comes back */

	memory_object_copy_strategy_t
				copy_strategy;	/* How to handle data copy */

	int			paging_in_progress;
						/* The memory object ports are
						 * being used (e.g., for pagein
						 * or pageout) -- don't change
						 * any of these fields (i.e.,
						 * don't collapse, destroy or
						 * terminate)
						 */
	unsigned int
	/* boolean_t array */	all_wanted:11,	/* Bit array of "want to be
						 * awakened" notations.  See
						 * VM_OBJECT_EVENT_* items
						 * below */
	/* boolean_t */	pager_created:1,	/* Has pager been created? */
	/* boolean_t */	pager_initialized:1,	/* Are fields ready to use? */
	/* boolean_t */	pager_ready:1,		/* Will pager take requests? */

	/* boolean_t */		pager_trusted:1,/* The pager for this object
						 * is trusted. This is true for
						 * all internal objects (backed
						 * by the default pager)
						 */
	/* boolean_t */		can_persist:1,	/* The kernel may keep the data
						 * for this object (and rights
						 * to the memory object) after
						 * all address map references 
						 * are deallocated?
						 */
	/* boolean_t */		internal:1,	/* Created by the kernel (and
						 * therefore, managed by the
						 * default memory manger)
						 */
	/* boolean_t */		temporary:1,	/* Permanent objects may be
						 * changed externally by the 
						 * memory manager, and changes
						 * made in memory must be
						 * reflected back to the memory
						 * manager.  Temporary objects
						 * lack both of these
						 * characteristics.
						 */
	/* boolean_t */		private:1,	/* magic device_pager object,
						 * holds private pages only */
	/* boolean_t */		pageout:1,	/* pageout object. contains
						 * private pages that refer to
						 * a real memory object. */
	/* boolean_t */		alive:1,	/* Not yet terminated */

	/* boolean_t */		purgable:2,	/* Purgable state.  See
						 * VM_PURGABLE_* 
						 */
	/* boolean_t */		shadowed:1,	/* Shadow may exist */
	/* boolean_t */		silent_overwrite:1,
						/* Allow full page overwrite
						 * without data_request if
						 * page is absent */
	/* boolean_t */		advisory_pageout:1,
						/* Instead of sending page
						 * via OOL, just notify
						 * pager that the kernel
						 * wants to discard it, page
						 * remains in object */
	/* boolean_t */		true_share:1,
						/* This object is mapped
						 * in more than one place
						 * and hence cannot be 
						 * coalesced */
	/* boolean_t */		terminating:1,
						/* Allows vm_object_lookup
						 * and vm_object_deallocate
						 * to special case their
						 * behavior when they are
						 * called as a result of
						 * page cleaning during
						 * object termination
						 */
	/* boolean_t */		named:1,	/* An enforces an internal
						 * naming convention, by
						 * calling the right routines
						 * for allocation and 
						 * destruction, UBC references
						 * against the vm_object are
						 * checked.
						 */
	/* boolean_t */		shadow_severed:1,
						/* When a permanent object 
						 * backing a COW goes away
					  	 * unexpectedly.  This bit
						 * allows vm_fault to return
						 * an error rather than a
						 * zero filled page.
						 */
	/* boolean_t */		phys_contiguous:1,
						/* Memory is wired and
						 * guaranteed physically 
						 * contiguous.  However
						 * it is not device memory
						 * and obeys normal virtual
						 * memory rules w.r.t pmap
						 * access bits.
						 */
	/* boolean_t */		nophyscache:1;
						/* When mapped at the 
						 * pmap level, don't allow
						 * primary caching. (for
						 * I/O)
						 */
						


	queue_chain_t		cached_list;	/* Attachment point for the
						 * list of objects cached as a
						 * result of their can_persist
						 * value
						 */

	queue_head_t		msr_q;		/* memory object synchronise
						   request queue */

  /*
   * the following fields are not protected by any locks
   * they are updated via atomic compare and swap
   */
	vm_object_offset_t	last_alloc;	/* last allocation offset */
	int			sequential;	/* sequential access size */

        uint32_t		pages_created;
        uint32_t		pages_used;
#if	MACH_PAGEMAP
	vm_external_map_t	existence_map;	/* bitmap of pages written to
						 * backing storage */
#endif	/* MACH_PAGEMAP */
	vm_offset_t		cow_hint;	/* last page present in     */
						/* shadow but not in object */
#if	MACH_ASSERT
	struct vm_object	*paging_object;	/* object which pages to be
						 * swapped out are temporary
						 * put in current object
						 */
#endif
	/* hold object lock when altering */
	unsigned	int
		wimg_bits:8,	        /* cache WIMG bits         */		
		code_signed:1,		/* pages are signed and should be
					   validated; the signatures are stored
					   with the pager */
		mapping_in_progress:1,	/* pager being mapped/unmapped */
		not_in_use:22;		/* for expansion */

#ifdef	UPL_DEBUG
	queue_head_t		uplq;		/* List of outstanding upls */
#endif /* UPL_DEBUG */

#ifdef	VM_PIP_DEBUG
/*
 * Keep track of the stack traces for the first holders
 * of a "paging_in_progress" reference for this VM object.
 */
#define VM_PIP_DEBUG_STACK_FRAMES	25	/* depth of each stack trace */
#define VM_PIP_DEBUG_MAX_REFS		10	/* track that many references */
	struct __pip_backtrace {
		void *pip_retaddr[VM_PIP_DEBUG_STACK_FRAMES];
	} pip_holders[VM_PIP_DEBUG_MAX_REFS];
#endif	/* VM_PIP_DEBUG  */

        queue_chain_t       objq;      /* object queue - currently used for purgable queues */
};

#define VM_PAGE_REMOVE(page)						\
	MACRO_BEGIN							\
	vm_page_t __page = (page);					\
	vm_object_t __object = __page->object;				\
	if (__page == __object->memq_hint) {				\
		vm_page_t	__new_hint;				\
		queue_entry_t	__qe;					\
		__qe = queue_next(&__page->listq);			\
		if (queue_end(&__object->memq, __qe)) {			\
			__qe = queue_prev(&__page->listq);		\
			if (queue_end(&__object->memq, __qe)) {		\
				__qe = NULL;				\
			}						\
		}							\
		__new_hint = (vm_page_t) __qe;				\
		__object->memq_hint = __new_hint;			\
	}								\
	queue_remove(&__object->memq, __page, vm_page_t, listq);	\
	MACRO_END

#define VM_PAGE_INSERT(page, object)				\
	MACRO_BEGIN						\
	vm_page_t __page = (page);				\
	vm_object_t __object = (object);			\
	queue_enter(&__object->memq, __page, vm_page_t, listq); \
	__object->memq_hint = __page;				\
	MACRO_END

__private_extern__
vm_object_t	kernel_object;		/* the single kernel object */

__private_extern__
unsigned int	vm_object_absent_max;	/* maximum number of absent pages
					   at a time for each object */

# define	VM_MSYNC_INITIALIZED			0
# define	VM_MSYNC_SYNCHRONIZING			1
# define	VM_MSYNC_DONE				2

struct msync_req {
	queue_chain_t		msr_q;		/* object request queue */
	queue_chain_t		req_q;		/* vm_msync request queue */
	unsigned int		flag;
	vm_object_offset_t	offset;
	vm_object_size_t	length;
	vm_object_t		object;		/* back pointer */
	decl_mutex_data(,	msync_req_lock)	/* Lock for this structure */
};

typedef struct msync_req	*msync_req_t;
#define MSYNC_REQ_NULL		((msync_req_t) 0)

/*
 * Macros to allocate and free msync_reqs
 */
#define msync_req_alloc(msr)						\
	MACRO_BEGIN							\
        (msr) = (msync_req_t)kalloc(sizeof(struct msync_req));		\
        mutex_init(&(msr)->msync_req_lock, 0);		\
	msr->flag = VM_MSYNC_INITIALIZED;				\
        MACRO_END

#define msync_req_free(msr)						\
	(kfree((msr), sizeof(struct msync_req)))

#define msr_lock(msr)   mutex_lock(&(msr)->msync_req_lock)
#define msr_unlock(msr) mutex_unlock(&(msr)->msync_req_lock)

/*
 *	Declare procedures that operate on VM objects.
 */

__private_extern__ void		vm_object_bootstrap(void) __attribute__((section("__TEXT, initcode")));

__private_extern__ void		vm_object_init(void);

__private_extern__ void		vm_object_init_lck_grp(void);

__private_extern__ void		vm_object_reaper_init(void);

__private_extern__ vm_object_t	vm_object_allocate(
					vm_object_size_t	size);

__private_extern__ void    _vm_object_allocate(vm_object_size_t size,
			    vm_object_t object);

#if	TASK_SWAPPER

__private_extern__ void	vm_object_res_reference(
				vm_object_t 		object);
__private_extern__ void	vm_object_res_deallocate(
				vm_object_t		object);
#define	VM_OBJ_RES_INCR(object)	(object)->res_count++
#define	VM_OBJ_RES_DECR(object)	(object)->res_count--

#else	/* TASK_SWAPPER */

#define	VM_OBJ_RES_INCR(object)
#define	VM_OBJ_RES_DECR(object)
#define vm_object_res_reference(object)
#define vm_object_res_deallocate(object)

#endif	/* TASK_SWAPPER */

#define vm_object_reference_locked(object)		\
	MACRO_BEGIN					\
	vm_object_t RLObject = (object);		\
	vm_object_lock_assert_exclusive(object);	\
	assert((RLObject)->ref_count > 0);		\
	(RLObject)->ref_count++;			\
	assert((RLObject)->ref_count > 1);		\
	vm_object_res_reference(RLObject);		\
	MACRO_END


#define vm_object_reference_shared(object)				\
	MACRO_BEGIN							\
	vm_object_t RLObject = (object);				\
	vm_object_lock_assert_shared(object);				\
	assert((RLObject)->ref_count > 0);				\
	OSAddAtomic(1, (SInt32 *)&(RLObject)->ref_count);		\
	assert((RLObject)->ref_count > 1);				\
	/* XXX we would need an atomic version of the following ... */	\
	vm_object_res_reference(RLObject);				\
	MACRO_END


__private_extern__ void		vm_object_reference(
					vm_object_t	object);

#if	!MACH_ASSERT

#define	vm_object_reference(object)			\
MACRO_BEGIN						\
	vm_object_t RObject = (object);			\
	if (RObject) {					\
		vm_object_lock(RObject);		\
		vm_object_reference_locked(RObject);	\
		vm_object_unlock(RObject);		\
	}						\
MACRO_END

#endif	/* MACH_ASSERT */

__private_extern__ void		vm_object_deallocate(
					vm_object_t	object);

__private_extern__ kern_return_t vm_object_release_name(
					vm_object_t	object,
					int		flags);
							
__private_extern__ void		vm_object_pmap_protect(
					vm_object_t		object,
					vm_object_offset_t	offset,
					vm_object_size_t	size,
					pmap_t			pmap,
					vm_map_offset_t		pmap_start,
					vm_prot_t		prot);

__private_extern__ void		vm_object_page_remove(
					vm_object_t		object,
					vm_object_offset_t	start,
					vm_object_offset_t	end);

__private_extern__ void		vm_object_deactivate_pages(
					vm_object_t		object,
					vm_object_offset_t	offset,
					vm_object_size_t	size,
					boolean_t               kill_page);

__private_extern__ unsigned int	vm_object_purge(
					vm_object_t		object);

__private_extern__ kern_return_t vm_object_purgable_control(
	vm_object_t	object,
	vm_purgable_t	control,
	int		*state);

__private_extern__ boolean_t	vm_object_coalesce(
					vm_object_t		prev_object,
					vm_object_t		next_object,
					vm_object_offset_t	prev_offset,
					vm_object_offset_t	next_offset,
					vm_object_size_t	prev_size,
					vm_object_size_t	next_size);

__private_extern__ boolean_t	vm_object_shadow(
					vm_object_t		*object,
					vm_object_offset_t	*offset,
					vm_object_size_t	length);

__private_extern__ void		vm_object_collapse(
					vm_object_t		object,
					vm_object_offset_t	offset,
					boolean_t		can_bypass);

__private_extern__ boolean_t	vm_object_copy_quickly(
				vm_object_t		*_object,
				vm_object_offset_t	src_offset,
				vm_object_size_t	size,
				boolean_t		*_src_needs_copy,
				boolean_t		*_dst_needs_copy);

__private_extern__ kern_return_t	vm_object_copy_strategically(
				vm_object_t		src_object,
				vm_object_offset_t	src_offset,
				vm_object_size_t	size,
				vm_object_t		*dst_object,
				vm_object_offset_t	*dst_offset,
				boolean_t		*dst_needs_copy);

__private_extern__ kern_return_t	vm_object_copy_slowly(
				vm_object_t		src_object,
				vm_object_offset_t	src_offset,
				vm_object_size_t	size,
				int			interruptible,
				vm_object_t		*_result_object);

__private_extern__ vm_object_t	vm_object_copy_delayed(
				vm_object_t		src_object,
				vm_object_offset_t	src_offset,
				vm_object_size_t	size,
				boolean_t		src_object_shared);



__private_extern__ kern_return_t	vm_object_destroy(
					vm_object_t	object,
					kern_return_t	reason);

__private_extern__ void		vm_object_pager_create(
					vm_object_t	object);

__private_extern__ void		vm_object_page_map(
				vm_object_t	object,
				vm_object_offset_t	offset,
				vm_object_size_t	size,
				vm_object_offset_t	(*map_fn)
					(void *, vm_object_offset_t),
					void 		*map_fn_data);

__private_extern__ kern_return_t vm_object_upl_request(
				vm_object_t		object, 
				vm_object_offset_t	offset,
				upl_size_t		size,
				upl_t			*upl,
				upl_page_info_t		*page_info,
				unsigned int		*count,
				int			flags);

__private_extern__ kern_return_t vm_object_transpose(
				vm_object_t		object1,
				vm_object_t		object2,
				vm_object_size_t	transpose_size);

__private_extern__ boolean_t vm_object_sync(
				vm_object_t		object,
				vm_object_offset_t	offset,
				vm_object_size_t	size,
				boolean_t		should_flush,
				boolean_t		should_return,
				boolean_t		should_iosync);

__private_extern__ kern_return_t vm_object_update(
				vm_object_t		object,
				vm_object_offset_t	offset,
				vm_object_size_t	size,
				vm_object_offset_t	*error_offset,
				int			*io_errno,
				memory_object_return_t	should_return,
				int			flags,
				vm_prot_t		prot);

__private_extern__ kern_return_t vm_object_lock_request(
				vm_object_t		object,
				vm_object_offset_t	offset,
				vm_object_size_t	size,
				memory_object_return_t	should_return,
				int			flags,
				vm_prot_t		prot);



__private_extern__ vm_object_t	vm_object_enter(
					memory_object_t		pager,
					vm_object_size_t	size,
					boolean_t		internal,
					boolean_t		init,
					boolean_t		check_named);


__private_extern__ void	vm_object_cluster_size(
					vm_object_t		object,
					vm_object_offset_t	*start,
					vm_size_t		*length,
					vm_object_fault_info_t  fault_info);

__private_extern__ kern_return_t vm_object_populate_with_private(
	vm_object_t		object,
	vm_object_offset_t	offset,
	ppnum_t			phys_page,
	vm_size_t		size);

extern kern_return_t adjust_vm_object_cache(
	vm_size_t oval,
	vm_size_t nval);

extern kern_return_t vm_object_page_op(
	vm_object_t		object,
	vm_object_offset_t	offset,
	int			ops,
	ppnum_t			*phys_entry,
	int			*flags);

extern kern_return_t vm_object_range_op(
	vm_object_t		object,
	vm_object_offset_t	offset_beg,
	vm_object_offset_t	offset_end,
	int                     ops,
	int                     *range);

/*
 *	Event waiting handling
 */

#define	VM_OBJECT_EVENT_INITIALIZED		0
#define	VM_OBJECT_EVENT_PAGER_READY		1
#define	VM_OBJECT_EVENT_PAGING_IN_PROGRESS	2
#define	VM_OBJECT_EVENT_MAPPING_IN_PROGRESS	3
#define	VM_OBJECT_EVENT_LOCK_IN_PROGRESS	4
#define	VM_OBJECT_EVENT_UNCACHING		5
#define	VM_OBJECT_EVENT_COPY_CALL		6
#define	VM_OBJECT_EVENT_CACHING			7

#define	vm_object_assert_wait(object, event, interruptible)		\
	(((object)->all_wanted |= 1 << (event)),			\
	 assert_wait((event_t)((vm_offset_t)(object)+(event)),(interruptible)))

#define	vm_object_wait(object, event, interruptible)			\
	(vm_object_assert_wait((object),(event),(interruptible)),	\
	vm_object_unlock(object),					\
	thread_block(THREAD_CONTINUE_NULL))				\

#define thread_sleep_vm_object(object, event, interruptible)		\
        lck_rw_sleep(&(object)->Lock, LCK_SLEEP_DEFAULT, (event_t)(event), (interruptible))

#define vm_object_sleep(object, event, interruptible)			\
	(((object)->all_wanted |= 1 << (event)),			\
	 thread_sleep_vm_object((object), 				\
		((vm_offset_t)(object)+(event)), (interruptible)))

#define	vm_object_wakeup(object, event)					\
	MACRO_BEGIN							\
	if ((object)->all_wanted & (1 << (event)))			\
		thread_wakeup((event_t)((vm_offset_t)(object) + (event))); \
	(object)->all_wanted &= ~(1 << (event));			\
	MACRO_END

#define	vm_object_set_wanted(object, event)				\
	MACRO_BEGIN							\
	((object)->all_wanted |= (1 << (event)));			\
	MACRO_END

#define	vm_object_wanted(object, event)					\
	((object)->all_wanted & (1 << (event)))

/*
 *	Routines implemented as macros
 */
#ifdef VM_PIP_DEBUG
#include <libkern/OSDebug.h>
#define VM_PIP_DEBUG_BEGIN(object)					\
	MACRO_BEGIN							\
	if ((object)->paging_in_progress < VM_PIP_DEBUG_MAX_REFS) {	\
		int pip = (object)->paging_in_progress;			\
		(void) OSBacktrace(&(object)->pip_holders[pip].pip_retaddr[0], \
				   VM_PIP_DEBUG_STACK_FRAMES);		\
	}								\
	MACRO_END
#else	/* VM_PIP_DEBUG */
#define VM_PIP_DEBUG_BEGIN(object)
#endif	/* VM_PIP_DEBUG */

#define		vm_object_paging_begin(object) 				\
	MACRO_BEGIN							\
	vm_object_lock_assert_exclusive((object));			\
	assert((object)->paging_in_progress >= 0);			\
	VM_PIP_DEBUG_BEGIN((object));					\
	(object)->paging_in_progress++;					\
	MACRO_END

#define		vm_object_paging_end(object) 				\
	MACRO_BEGIN							\
	vm_object_lock_assert_exclusive((object));			\
	assert((object)->paging_in_progress > 0);			\
	if (--(object)->paging_in_progress == 0) {			\
		vm_object_wakeup(object,				\
			VM_OBJECT_EVENT_PAGING_IN_PROGRESS);		\
	}								\
	MACRO_END

#define		vm_object_paging_wait(object, interruptible)		\
	MACRO_BEGIN							\
	vm_object_lock_assert_exclusive((object));			\
	while ((object)->paging_in_progress != 0) {			\
		wait_result_t  _wr;					\
									\
		_wr = vm_object_sleep((object),				\
				VM_OBJECT_EVENT_PAGING_IN_PROGRESS,	\
				(interruptible));			\
									\
		/*XXX if ((interruptible) && (_wr != THREAD_AWAKENED))*/\
			/*XXX break; */					\
	}								\
	MACRO_END


#define vm_object_mapping_begin(object) 				\
	MACRO_BEGIN							\
	vm_object_lock_assert_exclusive((object));			\
	assert(! (object)->mapping_in_progress);			\
	(object)->mapping_in_progress = TRUE;				\
	MACRO_END

#define vm_object_mapping_end(object)					\
	MACRO_BEGIN							\
	vm_object_lock_assert_exclusive((object));			\
	assert((object)->mapping_in_progress);				\
	(object)->mapping_in_progress = FALSE;				\
	vm_object_wakeup((object),					\
			 VM_OBJECT_EVENT_MAPPING_IN_PROGRESS);		\
	MACRO_END

#define vm_object_mapping_wait(object, interruptible)			\
	MACRO_BEGIN							\
	vm_object_lock_assert_exclusive((object));			\
	while ((object)->mapping_in_progress) {				\
		wait_result_t	_wr;					\
									\
		_wr = vm_object_sleep((object),				\
				      VM_OBJECT_EVENT_MAPPING_IN_PROGRESS, \
				      (interruptible));			\
		/*XXX if ((interruptible) && (_wr != THREAD_AWAKENED))*/\
			/*XXX break; */					\
	}								\
	assert(!(object)->mapping_in_progress);				\
	MACRO_END



#define OBJECT_LOCK_SHARED	0
#define OBJECT_LOCK_EXCLUSIVE	1

extern lck_grp_t	vm_object_lck_grp;
extern lck_grp_attr_t	vm_object_lck_grp_attr;
extern lck_attr_t	vm_object_lck_attr;
extern lck_attr_t	kernel_object_lck_attr;

extern vm_object_t	vm_pageout_scan_wants_object;

extern void		vm_object_lock(vm_object_t);
extern boolean_t	vm_object_lock_try(vm_object_t);
extern void		vm_object_lock_shared(vm_object_t);
extern boolean_t	vm_object_lock_try_shared(vm_object_t);

/*
 *	Object locking macros
 */

#define vm_object_lock_init(object)					\
	lck_rw_init(&(object)->Lock, &vm_object_lck_grp,		\
		    (((object) == kernel_object ||			\
		      (object) == vm_submap_object) ?			\
		     &kernel_object_lck_attr :				\
		     &vm_object_lck_attr))
#define vm_object_lock_destroy(object)	lck_rw_destroy(&(object)->Lock, &vm_object_lck_grp)

#define vm_object_unlock(object)	lck_rw_done(&(object)->Lock)
#define vm_object_lock_upgrade(object)	lck_rw_lock_shared_to_exclusive(&(object)->Lock)
#define vm_object_lock_try_scan(object)	lck_rw_try_lock_exclusive(&(object)->Lock)

/*
 * CAUTION: the following vm_object_lock_assert_held*() macros merely
 * check if anyone is holding the lock, but the holder may not necessarily
 * be the caller...
 */
#if DEBUG
#define vm_object_lock_assert_held(object) \
	lck_rw_assert(&(object)->Lock, LCK_RW_ASSERT_HELD)
#define vm_object_lock_assert_shared(object)	\
	lck_rw_assert(&(object)->Lock, LCK_RW_ASSERT_SHARED)
#define vm_object_lock_assert_exclusive(object) \
	lck_rw_assert(&(object)->Lock, LCK_RW_ASSERT_EXCLUSIVE)
#else /* DEBUG */
#define vm_object_lock_assert_held(object)
#define vm_object_lock_assert_shared(object)
#define vm_object_lock_assert_exclusive(object)
#endif /* DEBUG */

#define vm_object_round_page(x) (((vm_object_offset_t)(x) + PAGE_MASK) & ~((signed)PAGE_MASK))
#define vm_object_trunc_page(x) ((vm_object_offset_t)(x) & ~((signed)PAGE_MASK))

#endif	/* _VM_VM_OBJECT_H_ */

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
 *	File:	vm/vm_object.c
 *	Author:	Avadis Tevanian, Jr., Michael Wayne Young
 *
 *	Virtual memory object module.
 */

#include <debug.h>
#include <mach_pagemap.h>
#include <task_swapper.h>

#include <mach/mach_types.h>
#include <mach/memory_object.h>
#include <mach/memory_object_default.h>
#include <mach/memory_object_control_server.h>
#include <mach/vm_param.h>

#include <ipc/ipc_types.h>
#include <ipc/ipc_port.h>

#include <kern/kern_types.h>
#include <kern/assert.h>
#include <kern/lock.h>
#include <kern/queue.h>
#include <kern/xpr.h>
#include <kern/zalloc.h>
#include <kern/host.h>
#include <kern/host_statistics.h>
#include <kern/processor.h>
#include <kern/misc_protos.h>

#include <vm/memory_object.h>
#include <vm/vm_fault.h>
#include <vm/vm_map.h>
#include <vm/vm_object.h>
#include <vm/vm_page.h>
#include <vm/vm_pageout.h>
#include <vm/vm_protos.h>
#include <vm/vm_purgeable_internal.h>

#if CONFIG_EMBEDDED
#include <sys/kern_memorystatus.h>
#endif

/*
 *	Virtual memory objects maintain the actual data
 *	associated with allocated virtual memory.  A given
 *	page of memory exists within exactly one object.
 *
 *	An object is only deallocated when all "references"
 *	are given up.
 *
 *	Associated with each object is a list of all resident
 *	memory pages belonging to that object; this list is
 *	maintained by the "vm_page" module, but locked by the object's
 *	lock.
 *
 *	Each object also records the memory object reference
 *	that is used by the kernel to request and write
 *	back data (the memory object, field "pager"), etc...
 *
 *	Virtual memory objects are allocated to provide
 *	zero-filled memory (vm_allocate) or map a user-defined
 *	memory object into a virtual address space (vm_map).
 *
 *	Virtual memory objects that refer to a user-defined
 *	memory object are called "permanent", because all changes
 *	made in virtual memory are reflected back to the
 *	memory manager, which may then store it permanently.
 *	Other virtual memory objects are called "temporary",
 *	meaning that changes need be written back only when
 *	necessary to reclaim pages, and that storage associated
 *	with the object can be discarded once it is no longer
 *	mapped.
 *
 *	A permanent memory object may be mapped into more
 *	than one virtual address space.  Moreover, two threads
 *	may attempt to make the first mapping of a memory
 *	object concurrently.  Only one thread is allowed to
 *	complete this mapping; all others wait for the
 *	"pager_initialized" field is asserted, indicating
 *	that the first thread has initialized all of the
 *	necessary fields in the virtual memory object structure.
 *
 *	The kernel relies on a *default memory manager* to
 *	provide backing storage for the zero-filled virtual
 *	memory objects.  The pager memory objects associated
 *	with these temporary virtual memory objects are only
 *	requested from the default memory manager when it
 *	becomes necessary.  Virtual memory objects
 *	that depend on the default memory manager are called
 *	"internal".  The "pager_created" field is provided to
 *	indicate whether these ports have ever been allocated.
 *	
 *	The kernel may also create virtual memory objects to
 *	hold changed pages after a copy-on-write operation.
 *	In this case, the virtual memory object (and its
 *	backing storage -- its memory object) only contain
 *	those pages that have been changed.  The "shadow"
 *	field refers to the virtual memory object that contains
 *	the remainder of the contents.  The "shadow_offset"
 *	field indicates where in the "shadow" these contents begin.
 *	The "copy" field refers to a virtual memory object
 *	to which changed pages must be copied before changing
 *	this object, in order to implement another form
 *	of copy-on-write optimization.
 *
 *	The virtual memory object structure also records
 *	the attributes associated with its memory object.
 *	The "pager_ready", "can_persist" and "copy_strategy"
 *	fields represent those attributes.  The "cached_list"
 *	field is used in the implementation of the persistence
 *	attribute.
 *
 * ZZZ Continue this comment.
 */

/* Forward declarations for internal functions. */
static kern_return_t	vm_object_terminate(
				vm_object_t	object);

extern void		vm_object_remove(
				vm_object_t	object);

static kern_return_t	vm_object_copy_call(
				vm_object_t		src_object,
				vm_object_offset_t	src_offset,
				vm_object_size_t	size,
				vm_object_t		*_result_object);

static void		vm_object_do_collapse(
				vm_object_t	object,
				vm_object_t	backing_object);

static void		vm_object_do_bypass(
				vm_object_t	object,
				vm_object_t	backing_object);

static void		vm_object_release_pager(
	                        memory_object_t	pager,
				boolean_t	hashed);

static zone_t		vm_object_zone;		/* vm backing store zone */

/*
 *	All wired-down kernel memory belongs to a single virtual
 *	memory object (kernel_object) to avoid wasting data structures.
 */
static struct vm_object			kernel_object_store;
vm_object_t						kernel_object;


/*
 *	The submap object is used as a placeholder for vm_map_submap
 *	operations.  The object is declared in vm_map.c because it
 *	is exported by the vm_map module.  The storage is declared
 *	here because it must be initialized here.
 */
static struct vm_object			vm_submap_object_store;

/*
 *	Virtual memory objects are initialized from
 *	a template (see vm_object_allocate).
 *
 *	When adding a new field to the virtual memory
 *	object structure, be sure to add initialization
 *	(see _vm_object_allocate()).
 */
static struct vm_object			vm_object_template;

unsigned int vm_page_purged_wired = 0;
unsigned int vm_page_purged_busy = 0;
unsigned int vm_page_purged_others = 0;

#if VM_OBJECT_CACHE
/*
 *	Virtual memory objects that are not referenced by
 *	any address maps, but that are allowed to persist
 *	(an attribute specified by the associated memory manager),
 *	are kept in a queue (vm_object_cached_list).
 *
 *	When an object from this queue is referenced again,
 *	for example to make another address space mapping,
 *	it must be removed from the queue.  That is, the
 *	queue contains *only* objects with zero references.
 *
 *	The kernel may choose to terminate objects from this
 *	queue in order to reclaim storage.  The current policy
 *	is to permit a fixed maximum number of unreferenced
 *	objects (vm_object_cached_max).
 *
 *	A spin lock (accessed by routines
 *	vm_object_cache_{lock,lock_try,unlock}) governs the
 *	object cache.  It must be held when objects are
 *	added to or removed from the cache (in vm_object_terminate).
 *	The routines that acquire a reference to a virtual
 *	memory object based on one of the memory object ports
 *	must also lock the cache.
 *
 *	Ideally, the object cache should be more isolated
 *	from the reference mechanism, so that the lock need
 *	not be held to make simple references.
 */
static vm_object_t	vm_object_cache_trim(
				boolean_t called_from_vm_object_deallocate);

static queue_head_t	vm_object_cached_list;
static int		vm_object_cached_count=0;
static int		vm_object_cached_high;	/* highest # cached objects */
static int		vm_object_cached_max = 512;	/* may be patched*/

static lck_mtx_t	vm_object_cached_lock_data;
static lck_mtx_ext_t	vm_object_cached_lock_data_ext;

#define vm_object_cache_lock()		\
		lck_mtx_lock(&vm_object_cached_lock_data)
#define vm_object_cache_lock_try()		\
		lck_mtx_try_lock(&vm_object_cached_lock_data)
#define vm_object_cache_lock_spin()		\
		lck_mtx_lock_spin(&vm_object_cached_lock_data)
#define vm_object_cache_unlock()	\
		lck_mtx_unlock(&vm_object_cached_lock_data)

#endif	/* VM_OBJECT_CACHE */


static void		vm_object_deactivate_all_pages(
				vm_object_t	object);


#define	VM_OBJECT_HASH_COUNT		1024
#define	VM_OBJECT_HASH_LOCK_COUNT	512

static lck_mtx_t	vm_object_hashed_lock_data[VM_OBJECT_HASH_LOCK_COUNT];
static lck_mtx_ext_t	vm_object_hashed_lock_data_ext[VM_OBJECT_HASH_LOCK_COUNT];

static queue_head_t	vm_object_hashtable[VM_OBJECT_HASH_COUNT];
static struct zone	*vm_object_hash_zone;

struct vm_object_hash_entry {
	queue_chain_t		hash_link;	/* hash chain link */
	memory_object_t	pager;		/* pager we represent */
	vm_object_t		object;		/* corresponding object */
	boolean_t		waiting;	/* someone waiting for
						 * termination */
};

typedef struct vm_object_hash_entry	*vm_object_hash_entry_t;
#define VM_OBJECT_HASH_ENTRY_NULL	((vm_object_hash_entry_t) 0)

#define VM_OBJECT_HASH_SHIFT	5
#define vm_object_hash(pager) \
	((int)((((uintptr_t)pager) >> VM_OBJECT_HASH_SHIFT) % VM_OBJECT_HASH_COUNT))

#define vm_object_lock_hash(pager) \
	((int)((((uintptr_t)pager) >> VM_OBJECT_HASH_SHIFT) % VM_OBJECT_HASH_LOCK_COUNT))

void vm_object_hash_entry_free(
	vm_object_hash_entry_t	entry);

static void vm_object_reap(vm_object_t object);
static void vm_object_reap_async(vm_object_t object);
static void vm_object_reaper_thread(void);

static lck_mtx_t	vm_object_reaper_lock_data;
static lck_mtx_ext_t	vm_object_reaper_lock_data_ext;

static queue_head_t vm_object_reaper_queue; /* protected by vm_object_reaper_lock() */
unsigned int vm_object_reap_count = 0;
unsigned int vm_object_reap_count_async = 0;

#define vm_object_reaper_lock()		\
		lck_mtx_lock(&vm_object_reaper_lock_data)
#define vm_object_reaper_lock_spin()		\
		lck_mtx_lock_spin(&vm_object_reaper_lock_data)
#define vm_object_reaper_unlock()	\
		lck_mtx_unlock(&vm_object_reaper_lock_data)



static lck_mtx_t *
vm_object_hash_lock_spin(
	memory_object_t	pager)
{
	int	index;

	index = vm_object_lock_hash(pager);

	lck_mtx_lock_spin(&vm_object_hashed_lock_data[index]);

	return (&vm_object_hashed_lock_data[index]);
}

static void
vm_object_hash_unlock(lck_mtx_t *lck)
{
	lck_mtx_unlock(lck);
}


/*
 *	vm_object_hash_lookup looks up a pager in the hashtable
 *	and returns the corresponding entry, with optional removal.
 */
static vm_object_hash_entry_t
vm_object_hash_lookup(
	memory_object_t	pager,
	boolean_t	remove_entry)
{
	queue_t			bucket;
	vm_object_hash_entry_t	entry;

	bucket = &vm_object_hashtable[vm_object_hash(pager)];

	entry = (vm_object_hash_entry_t)queue_first(bucket);
	while (!queue_end(bucket, (queue_entry_t)entry)) {
		if (entry->pager == pager) {
			if (remove_entry) {
				queue_remove(bucket, entry,
					     vm_object_hash_entry_t, hash_link);
			}
			return(entry);
		}
		entry = (vm_object_hash_entry_t)queue_next(&entry->hash_link);
	}
	return(VM_OBJECT_HASH_ENTRY_NULL);
}

/*
 *	vm_object_hash_enter enters the specified
 *	pager / cache object association in the hashtable.
 */

static void
vm_object_hash_insert(
	vm_object_hash_entry_t	entry,
	vm_object_t		object)
{
	queue_t		bucket;

	bucket = &vm_object_hashtable[vm_object_hash(entry->pager)];

	queue_enter(bucket, entry, vm_object_hash_entry_t, hash_link);

	entry->object = object;
	object->hashed = TRUE;
}

static vm_object_hash_entry_t
vm_object_hash_entry_alloc(
	memory_object_t	pager)
{
	vm_object_hash_entry_t	entry;

	entry = (vm_object_hash_entry_t)zalloc(vm_object_hash_zone);
	entry->pager = pager;
	entry->object = VM_OBJECT_NULL;
	entry->waiting = FALSE;

	return(entry);
}

void
vm_object_hash_entry_free(
	vm_object_hash_entry_t	entry)
{
	zfree(vm_object_hash_zone, entry);
}

/*
 *	vm_object_allocate:
 *
 *	Returns a new object with the given size.
 */

__private_extern__ void
_vm_object_allocate(
	vm_object_size_t	size,
	vm_object_t		object)
{
	XPR(XPR_VM_OBJECT,
		"vm_object_allocate, object 0x%X size 0x%X\n",
		object, size, 0,0,0);

	*object = vm_object_template;
	queue_init(&object->memq);
	queue_init(&object->msr_q);
#if UPL_DEBUG
	queue_init(&object->uplq);
#endif /* UPL_DEBUG */
	vm_object_lock_init(object);
	object->size = size;
}

__private_extern__ vm_object_t
vm_object_allocate(
	vm_object_size_t	size)
{
	register vm_object_t object;

	object = (vm_object_t) zalloc(vm_object_zone);
	
//	dbgLog(object, size, 0, 2);			/* (TEST/DEBUG) */

	if (object != VM_OBJECT_NULL)
		_vm_object_allocate(size, object);

	return object;
}


lck_grp_t		vm_object_lck_grp;
lck_grp_attr_t	vm_object_lck_grp_attr;
lck_attr_t		vm_object_lck_attr;
lck_attr_t		kernel_object_lck_attr;

/*
 *	vm_object_bootstrap:
 *
 *	Initialize the VM objects module.
 */
__private_extern__ void
vm_object_bootstrap(void)
{
	register int	i;

	vm_object_zone = zinit((vm_size_t) sizeof(struct vm_object),
				round_page(512*1024),
				round_page(12*1024),
				"vm objects");
	zone_change(vm_object_zone, Z_NOENCRYPT, TRUE);

	vm_object_init_lck_grp();

#if VM_OBJECT_CACHE
	queue_init(&vm_object_cached_list);

	lck_mtx_init_ext(&vm_object_cached_lock_data,
		&vm_object_cached_lock_data_ext,
		&vm_object_lck_grp,
		&vm_object_lck_attr);
#endif
	queue_init(&vm_object_reaper_queue);

	for (i = 0; i < VM_OBJECT_HASH_LOCK_COUNT; i++) {
		lck_mtx_init_ext(&vm_object_hashed_lock_data[i],
				 &vm_object_hashed_lock_data_ext[i],
				 &vm_object_lck_grp,
				 &vm_object_lck_attr);
	}
	lck_mtx_init_ext(&vm_object_reaper_lock_data,
		&vm_object_reaper_lock_data_ext,
		&vm_object_lck_grp,
		&vm_object_lck_attr);

	vm_object_hash_zone =
			zinit((vm_size_t) sizeof (struct vm_object_hash_entry),
			      round_page(512*1024),
			      round_page(12*1024),
			      "vm object hash entries");
	zone_change(vm_object_hash_zone, Z_NOENCRYPT, TRUE);

	for (i = 0; i < VM_OBJECT_HASH_COUNT; i++)
		queue_init(&vm_object_hashtable[i]);


	/*
	 *	Fill in a template object, for quick initialization
	 */

	/* memq; Lock; init after allocation */
	vm_object_template.memq.prev = NULL;
	vm_object_template.memq.next = NULL;
#if 0
	/*
	 * We can't call vm_object_lock_init() here because that will
	 * allocate some memory and VM is not fully initialized yet.
	 * The lock will be initialized for each allocated object in
	 * _vm_object_allocate(), so we don't need to initialize it in
	 * the vm_object_template.
	 */
	vm_object_lock_init(&vm_object_template);
#endif
	vm_object_template.size = 0;
	vm_object_template.memq_hint = VM_PAGE_NULL;
	vm_object_template.ref_count = 1;
#if	TASK_SWAPPER
	vm_object_template.res_count = 1;
#endif	/* TASK_SWAPPER */
	vm_object_template.resident_page_count = 0;
	vm_object_template.wired_page_count = 0;
	vm_object_template.reusable_page_count = 0;
	vm_object_template.copy = VM_OBJECT_NULL;
	vm_object_template.shadow = VM_OBJECT_NULL;
	vm_object_template.shadow_offset = (vm_object_offset_t) 0;
	vm_object_template.pager = MEMORY_OBJECT_NULL;
	vm_object_template.paging_offset = 0;
	vm_object_template.pager_control = MEMORY_OBJECT_CONTROL_NULL;
	vm_object_template.copy_strategy = MEMORY_OBJECT_COPY_SYMMETRIC;
	vm_object_template.paging_in_progress = 0;
	vm_object_template.activity_in_progress = 0;

	/* Begin bitfields */
	vm_object_template.all_wanted = 0; /* all bits FALSE */
	vm_object_template.pager_created = FALSE;
	vm_object_template.pager_initialized = FALSE;
	vm_object_template.pager_ready = FALSE;
	vm_object_template.pager_trusted = FALSE;
	vm_object_template.can_persist = FALSE;
	vm_object_template.internal = TRUE;
	vm_object_template.temporary = TRUE;
	vm_object_template.private = FALSE;
	vm_object_template.pageout = FALSE;
	vm_object_template.alive = TRUE;
	vm_object_template.purgable = VM_PURGABLE_DENY;
	vm_object_template.shadowed = FALSE;
	vm_object_template.silent_overwrite = FALSE;
	vm_object_template.advisory_pageout = FALSE;
	vm_object_template.true_share = FALSE;
	vm_object_template.terminating = FALSE;
	vm_object_template.named = FALSE;
	vm_object_template.shadow_severed = FALSE;
	vm_object_template.phys_contiguous = FALSE;
	vm_object_template.nophyscache = FALSE;
	/* End bitfields */

	vm_object_template.cached_list.prev = NULL;
	vm_object_template.cached_list.next = NULL;
	vm_object_template.msr_q.prev = NULL;
	vm_object_template.msr_q.next = NULL;
	
	vm_object_template.last_alloc = (vm_object_offset_t) 0;
	vm_object_template.sequential = (vm_object_offset_t) 0;
	vm_object_template.pages_created = 0;
	vm_object_template.pages_used = 0;

#if	MACH_PAGEMAP
	vm_object_template.existence_map = VM_EXTERNAL_NULL;
#endif	/* MACH_PAGEMAP */
	vm_object_template.cow_hint = ~(vm_offset_t)0;
#if	MACH_ASSERT
	vm_object_template.paging_object = VM_OBJECT_NULL;
#endif	/* MACH_ASSERT */

	/* cache bitfields */
	vm_object_template.wimg_bits = VM_WIMG_DEFAULT;
	vm_object_template.code_signed = FALSE;
	vm_object_template.hashed = FALSE;
	vm_object_template.transposed = FALSE;
	vm_object_template.mapping_in_progress = FALSE;
	vm_object_template.volatile_empty = FALSE;
	vm_object_template.volatile_fault = FALSE;
	vm_object_template.all_reusable = FALSE;
	vm_object_template.blocked_access = FALSE;
	vm_object_template.__object2_unused_bits = 0;
#if UPL_DEBUG
	vm_object_template.uplq.prev = NULL;
	vm_object_template.uplq.next = NULL;
#endif /* UPL_DEBUG */
#ifdef VM_PIP_DEBUG
	bzero(&vm_object_template.pip_holders,
	      sizeof (vm_object_template.pip_holders));
#endif /* VM_PIP_DEBUG */

	vm_object_template.objq.next=NULL;
	vm_object_template.objq.prev=NULL;

	
	/*
	 *	Initialize the "kernel object"
	 */

	kernel_object = &kernel_object_store;

/*
 *	Note that in the following size specifications, we need to add 1 because 
 *	VM_MAX_KERNEL_ADDRESS (vm_last_addr) is a maximum address, not a size.
 */

#ifdef ppc
	_vm_object_allocate(vm_last_addr + 1,
			    kernel_object);
#else
	_vm_object_allocate(VM_MAX_KERNEL_ADDRESS + 1,
			    kernel_object);
#endif
	kernel_object->copy_strategy = MEMORY_OBJECT_COPY_NONE;

	/*
	 *	Initialize the "submap object".  Make it as large as the
	 *	kernel object so that no limit is imposed on submap sizes.
	 */

	vm_submap_object = &vm_submap_object_store;
#ifdef ppc
	_vm_object_allocate(vm_last_addr + 1,
			    vm_submap_object);
#else
	_vm_object_allocate(VM_MAX_KERNEL_ADDRESS + 1,
			    vm_submap_object);
#endif
	vm_submap_object->copy_strategy = MEMORY_OBJECT_COPY_NONE;

	/*
	 * Create an "extra" reference to this object so that we never
	 * try to deallocate it; zfree doesn't like to be called with
	 * non-zone memory.
	 */
	vm_object_reference(vm_submap_object);

#if	MACH_PAGEMAP
	vm_external_module_initialize();
#endif	/* MACH_PAGEMAP */
}

void
vm_object_reaper_init(void)
{
	kern_return_t	kr;
	thread_t	thread;

	kr = kernel_thread_start_priority(
		(thread_continue_t) vm_object_reaper_thread,
		NULL,
		BASEPRI_PREEMPT - 1,
		&thread);
	if (kr != KERN_SUCCESS) {
		panic("failed to launch vm_object_reaper_thread kr=0x%x", kr);
	}
	thread_deallocate(thread);
}

__private_extern__ void
vm_object_init(void)
{
	/*
	 *	Finish initializing the kernel object.
	 */
}


__private_extern__ void
vm_object_init_lck_grp(void)
{
	/*
	 * initialze the vm_object lock world
	 */
	lck_grp_attr_setdefault(&vm_object_lck_grp_attr);
	lck_grp_init(&vm_object_lck_grp, "vm_object", &vm_object_lck_grp_attr);
	lck_attr_setdefault(&vm_object_lck_attr);
	lck_attr_setdefault(&kernel_object_lck_attr);
	lck_attr_cleardebug(&kernel_object_lck_attr);
}

#if VM_OBJECT_CACHE
#define	MIGHT_NOT_CACHE_SHADOWS		1
#if	MIGHT_NOT_CACHE_SHADOWS
static int cache_shadows = TRUE;
#endif	/* MIGHT_NOT_CACHE_SHADOWS */
#endif

/*
 *	vm_object_deallocate:
 *
 *	Release a reference to the specified object,
 *	gained either through a vm_object_allocate
 *	or a vm_object_reference call.  When all references
 *	are gone, storage associated with this object
 *	may be relinquished.
 *
 *	No object may be locked.
 */
unsigned long vm_object_deallocate_shared_successes = 0;
unsigned long vm_object_deallocate_shared_failures = 0;
unsigned long vm_object_deallocate_shared_swap_failures = 0;
__private_extern__ void
vm_object_deallocate(
	register vm_object_t	object)
{
#if VM_OBJECT_CACHE
	boolean_t	retry_cache_trim = FALSE;
	uint32_t	try_failed_count = 0;
#endif
	vm_object_t	shadow = VM_OBJECT_NULL;
	
//	if(object)dbgLog(object, object->ref_count, object->can_persist, 3);	/* (TEST/DEBUG) */
//	else dbgLog(object, 0, 0, 3);	/* (TEST/DEBUG) */

	if (object == VM_OBJECT_NULL)
	        return;

	if (object == kernel_object) {
		vm_object_lock_shared(object);

		OSAddAtomic(-1, &object->ref_count);

		if (object->ref_count == 0) {
			panic("vm_object_deallocate: losing kernel_object\n");
		}
		vm_object_unlock(object);
		return;
	}

	if (object->ref_count > 2 ||
	    (!object->named && object->ref_count > 1)) {
		UInt32		original_ref_count;
		volatile UInt32	*ref_count_p;
		Boolean		atomic_swap;

		/*
		 * The object currently looks like it is not being
		 * kept alive solely by the reference we're about to release.
		 * Let's try and release our reference without taking
		 * all the locks we would need if we had to terminate the
		 * object (cache lock + exclusive object lock).
		 * Lock the object "shared" to make sure we don't race with
		 * anyone holding it "exclusive".
		 */
	        vm_object_lock_shared(object);
		ref_count_p = (volatile UInt32 *) &object->ref_count;
		original_ref_count = object->ref_count;
		/*
		 * Test again as "ref_count" could have changed.
		 * "named" shouldn't change.
		 */
		if (original_ref_count > 2 ||
		    (!object->named && original_ref_count > 1)) {
			atomic_swap = OSCompareAndSwap(
				original_ref_count,
				original_ref_count - 1,
				(UInt32 *) &object->ref_count);
			if (atomic_swap == FALSE) {
				vm_object_deallocate_shared_swap_failures++;
			}

		} else {
			atomic_swap = FALSE;
		}
		vm_object_unlock(object);

		if (atomic_swap) {
			/*
			 * ref_count was updated atomically !
			 */
			vm_object_deallocate_shared_successes++;
			return;
		}

		/*
		 * Someone else updated the ref_count at the same
		 * time and we lost the race.  Fall back to the usual
		 * slow but safe path...
		 */
		vm_object_deallocate_shared_failures++;
	}

	while (object != VM_OBJECT_NULL) {

		vm_object_lock(object);

		assert(object->ref_count > 0);

		/*
		 *	If the object has a named reference, and only
		 *	that reference would remain, inform the pager
		 *	about the last "mapping" reference going away.
		 */
		if ((object->ref_count == 2)  && (object->named)) {
			memory_object_t	pager = object->pager;

			/* Notify the Pager that there are no */
			/* more mappers for this object */

			if (pager != MEMORY_OBJECT_NULL) {
				vm_object_mapping_wait(object, THREAD_UNINT);
				vm_object_mapping_begin(object);
				vm_object_unlock(object);

				memory_object_last_unmap(pager);

				vm_object_lock(object);
				vm_object_mapping_end(object);
			}
			/*
			 * recheck the ref_count since we dropped the object lock
			 * to call 'memory_object_last_unmap'... it's possible
			 * additional references got taken and we only want
			 * to deactivate the pages if this 'named' object will only
			 * referenced by the backing pager once we drop our reference
			 * below
			 */
			if (!object->terminating && object->ref_count == 2)
				vm_object_deactivate_all_pages(object);

			assert(object->ref_count > 0);
		}

		/*
		 *	Lose the reference. If other references
		 *	remain, then we are done, unless we need
		 *	to retry a cache trim.
		 *	If it is the last reference, then keep it
		 *	until any pending initialization is completed.
		 */

		/* if the object is terminating, it cannot go into */
		/* the cache and we obviously should not call      */
		/* terminate again.  */

		if ((object->ref_count > 1) || object->terminating) {
			vm_object_lock_assert_exclusive(object);
			object->ref_count--;
			vm_object_res_deallocate(object);

			if (object->ref_count == 1 &&
			    object->shadow != VM_OBJECT_NULL) {
				/*
				 * There's only one reference left on this
				 * VM object.  We can't tell if it's a valid
				 * one (from a mapping for example) or if this
				 * object is just part of a possibly stale and
				 * useless shadow chain.
				 * We would like to try and collapse it into
				 * its parent, but we don't have any pointers
				 * back to this parent object.
				 * But we can try and collapse this object with
				 * its own shadows, in case these are useless
				 * too...
				 * We can't bypass this object though, since we
				 * don't know if this last reference on it is
				 * meaningful or not.
				 */
				vm_object_collapse(object, 0, FALSE);
			}
			vm_object_unlock(object); 
#if VM_OBJECT_CACHE
			if (retry_cache_trim &&
			    ((object = vm_object_cache_trim(TRUE)) !=
			     VM_OBJECT_NULL)) {
				continue;
			}
#endif
			return;
		}

		/*
		 *	We have to wait for initialization
		 *	before destroying or caching the object.
		 */
		
		if (object->pager_created && ! object->pager_initialized) {
			assert(! object->can_persist);
			vm_object_assert_wait(object,
					      VM_OBJECT_EVENT_INITIALIZED,
					      THREAD_UNINT);
			vm_object_unlock(object);

			thread_block(THREAD_CONTINUE_NULL);
			continue;
		}

#if VM_OBJECT_CACHE
		/*
		 *	If this object can persist, then enter it in
		 *	the cache. Otherwise, terminate it.
		 *
		 * 	NOTE:  Only permanent objects are cached, and
		 *	permanent objects cannot have shadows.  This
		 *	affects the residence counting logic in a minor
		 *	way (can do it in-line, mostly).
		 */

		if ((object->can_persist) && (object->alive)) {
			/*
			 *	Now it is safe to decrement reference count,
			 *	and to return if reference count is > 0.
			 */

			vm_object_lock_assert_exclusive(object);
			if (--object->ref_count > 0) {
				vm_object_res_deallocate(object);
				vm_object_unlock(object);

				if (retry_cache_trim &&
				    ((object = vm_object_cache_trim(TRUE)) !=
				     VM_OBJECT_NULL)) {
					continue;
				}
				return;
			}

#if	MIGHT_NOT_CACHE_SHADOWS
			/*
			 *	Remove shadow now if we don't
			 *	want to cache shadows.
			 */
			if (! cache_shadows) {
				shadow = object->shadow;
				object->shadow = VM_OBJECT_NULL;
			}
#endif	/* MIGHT_NOT_CACHE_SHADOWS */

			/*
			 *	Enter the object onto the queue of
			 *	cached objects, and deactivate
			 *	all of its pages.
			 */
			assert(object->shadow == VM_OBJECT_NULL);
			VM_OBJ_RES_DECR(object);
			XPR(XPR_VM_OBJECT,
		      "vm_o_deallocate: adding %x to cache, queue = (%x, %x)\n",
				object,
				vm_object_cached_list.next,
				vm_object_cached_list.prev,0,0);


			vm_object_unlock(object);

			try_failed_count = 0;
			for (;;) {
				vm_object_cache_lock();

				/*
				 * if we try to take a regular lock here
				 * we risk deadlocking against someone
				 * holding a lock on this object while
				 * trying to vm_object_deallocate a different
				 * object
				 */
				if (vm_object_lock_try(object))
					break;
				vm_object_cache_unlock();
				try_failed_count++;

				mutex_pause(try_failed_count);  /* wait a bit */
			}
			vm_object_cached_count++;
			if (vm_object_cached_count > vm_object_cached_high)
				vm_object_cached_high = vm_object_cached_count;
			queue_enter(&vm_object_cached_list, object,
				vm_object_t, cached_list);
			vm_object_cache_unlock();

			vm_object_deactivate_all_pages(object);
			vm_object_unlock(object);

#if	MIGHT_NOT_CACHE_SHADOWS
			/*
			 *	If we have a shadow that we need
			 *	to deallocate, do so now, remembering
			 *	to trim the cache later.
			 */
			if (! cache_shadows && shadow != VM_OBJECT_NULL) {
				object = shadow;
				retry_cache_trim = TRUE;
				continue;
			}
#endif	/* MIGHT_NOT_CACHE_SHADOWS */

			/*
			 *	Trim the cache. If the cache trim
			 *	returns with a shadow for us to deallocate,
			 *	then remember to retry the cache trim
			 *	when we are done deallocating the shadow.
			 *	Otherwise, we are done.
			 */

			object = vm_object_cache_trim(TRUE);
			if (object == VM_OBJECT_NULL) {
				return;
			}
			retry_cache_trim = TRUE;
		} else
#endif	/* VM_OBJECT_CACHE */
		{
			/*
			 *	This object is not cachable; terminate it.
			 */
			XPR(XPR_VM_OBJECT,
	 "vm_o_deallocate: !cacheable 0x%X res %d paging_ops %d thread 0x%p ref %d\n",
			    object, object->resident_page_count,
			    object->paging_in_progress,
			    (void *)current_thread(),object->ref_count);

			VM_OBJ_RES_DECR(object);	/* XXX ? */
			/*
			 *	Terminate this object. If it had a shadow,
			 *	then deallocate it; otherwise, if we need
			 *	to retry a cache trim, do so now; otherwise,
			 *	we are done. "pageout" objects have a shadow,
			 *	but maintain a "paging reference" rather than
			 *	a normal reference.
			 */
			shadow = object->pageout?VM_OBJECT_NULL:object->shadow;

			if (vm_object_terminate(object) != KERN_SUCCESS) {
				return;
			}
			if (shadow != VM_OBJECT_NULL) {
				object = shadow;
				continue;
			}
#if VM_OBJECT_CACHE
			if (retry_cache_trim &&
			    ((object = vm_object_cache_trim(TRUE)) !=
			     VM_OBJECT_NULL)) {
				continue;
			}
#endif
			return;
		}
	}
#if VM_OBJECT_CACHE
	assert(! retry_cache_trim);
#endif
}


#if VM_OBJECT_CACHE
/*
 *	Check to see whether we really need to trim
 *	down the cache. If so, remove an object from
 *	the cache, terminate it, and repeat.
 *
 *	Called with, and returns with, cache lock unlocked.
 */
vm_object_t
vm_object_cache_trim(
	boolean_t called_from_vm_object_deallocate)
{
	register vm_object_t object = VM_OBJECT_NULL;
	vm_object_t shadow;

	for (;;) {

		/*
		 *	If we no longer need to trim the cache,
		 *	then we are done.
		 */
		if (vm_object_cached_count <= vm_object_cached_max)
			return VM_OBJECT_NULL;

		vm_object_cache_lock();
		if (vm_object_cached_count <= vm_object_cached_max) {
			vm_object_cache_unlock();
			return VM_OBJECT_NULL;
		}

		/*
		 *	We must trim down the cache, so remove
		 *	the first object in the cache.
		 */
		XPR(XPR_VM_OBJECT,
		"vm_object_cache_trim: removing from front of cache (%x, %x)\n",
			vm_object_cached_list.next,
			vm_object_cached_list.prev, 0, 0, 0);

		object = (vm_object_t) queue_first(&vm_object_cached_list);
		if(object == (vm_object_t) &vm_object_cached_list) {
			/* something's wrong with the calling parameter or */
			/* the value of vm_object_cached_count, just fix   */
			/* and return */
			if(vm_object_cached_max < 0)
				vm_object_cached_max = 0;
			vm_object_cached_count = 0;
			vm_object_cache_unlock();
			return VM_OBJECT_NULL;
		}
		vm_object_lock(object);
		queue_remove(&vm_object_cached_list, object, vm_object_t,
			     cached_list);
		vm_object_cached_count--;

		vm_object_cache_unlock();
		/*
		 *	Since this object is in the cache, we know
		 *	that it is initialized and has no references.
		 *	Take a reference to avoid recursive deallocations.
		 */

		assert(object->pager_initialized);
		assert(object->ref_count == 0);
		vm_object_lock_assert_exclusive(object);
		object->ref_count++;

		/*
		 *	Terminate the object.
		 *	If the object had a shadow, we let vm_object_deallocate
		 *	deallocate it. "pageout" objects have a shadow, but
		 *	maintain a "paging reference" rather than a normal
		 *	reference.
		 *	(We are careful here to limit recursion.)
		 */
		shadow = object->pageout?VM_OBJECT_NULL:object->shadow;

		if(vm_object_terminate(object) != KERN_SUCCESS)
			continue;

		if (shadow != VM_OBJECT_NULL) {
			if (called_from_vm_object_deallocate) {
				return shadow;
			} else {
				vm_object_deallocate(shadow);
			}
		}
	}
}
#endif


/*
 *	Routine:	vm_object_terminate
 *	Purpose:
 *		Free all resources associated with a vm_object.
 *	In/out conditions:
 *		Upon entry, the object must be locked,
 *		and the object must have exactly one reference.
 *
 *		The shadow object reference is left alone.
 *
 *		The object must be unlocked if its found that pages
 *		must be flushed to a backing object.  If someone
 *		manages to map the object while it is being flushed
 *		the object is returned unlocked and unchanged.  Otherwise,
 *		upon exit, the cache will be unlocked, and the
 *		object will cease to exist.
 */
static kern_return_t
vm_object_terminate(
	vm_object_t	object)
{
	vm_object_t	shadow_object;

	XPR(XPR_VM_OBJECT, "vm_object_terminate, object 0x%X ref %d\n",
		object, object->ref_count, 0, 0, 0);

	if (!object->pageout && (!object->temporary || object->can_persist) &&
	    (object->pager != NULL || object->shadow_severed)) {
		/*
		 * Clear pager_trusted bit so that the pages get yanked
		 * out of the object instead of cleaned in place.  This
		 * prevents a deadlock in XMM and makes more sense anyway.
		 */
		object->pager_trusted = FALSE;

		vm_object_reap_pages(object, REAP_TERMINATE);
	}
	/*
	 *	Make sure the object isn't already being terminated
	 */
	if (object->terminating) {
		vm_object_lock_assert_exclusive(object);
		object->ref_count--;
		assert(object->ref_count > 0);
		vm_object_unlock(object);
		return KERN_FAILURE;
	}

	/*
	 * Did somebody get a reference to the object while we were
	 * cleaning it?
	 */
	if (object->ref_count != 1) {
		vm_object_lock_assert_exclusive(object);
		object->ref_count--;
		assert(object->ref_count > 0);
		vm_object_res_deallocate(object);
		vm_object_unlock(object);
		return KERN_FAILURE;
	}

	/*
	 *	Make sure no one can look us up now.
	 */

	object->terminating = TRUE;
	object->alive = FALSE;

	if (object->hashed) {
		lck_mtx_t	*lck;

		lck = vm_object_hash_lock_spin(object->pager);
		vm_object_remove(object);
		vm_object_hash_unlock(lck);
	}
	/*
	 *	Detach the object from its shadow if we are the shadow's
	 *	copy. The reference we hold on the shadow must be dropped
	 *	by our caller.
	 */
	if (((shadow_object = object->shadow) != VM_OBJECT_NULL) &&
	    !(object->pageout)) {
		vm_object_lock(shadow_object);
		if (shadow_object->copy == object)
			shadow_object->copy = VM_OBJECT_NULL;
		vm_object_unlock(shadow_object);
	}

	if (object->paging_in_progress != 0 ||
	    object->activity_in_progress != 0) {
		/*
		 * There are still some paging_in_progress references
		 * on this object, meaning that there are some paging
		 * or other I/O operations in progress for this VM object.
		 * Such operations take some paging_in_progress references
		 * up front to ensure that the object doesn't go away, but
		 * they may also need to acquire a reference on the VM object,
		 * to map it in kernel space, for example.  That means that
		 * they may end up releasing the last reference on the VM
		 * object, triggering its termination, while still holding
		 * paging_in_progress references.  Waiting for these
		 * pending paging_in_progress references to go away here would
		 * deadlock.
		 *
		 * To avoid deadlocking, we'll let the vm_object_reaper_thread
		 * complete the VM object termination if it still holds
		 * paging_in_progress references at this point.
		 *
		 * No new paging_in_progress should appear now that the
		 * VM object is "terminating" and not "alive".
		 */
		vm_object_reap_async(object);
		vm_object_unlock(object);
		/*
		 * Return KERN_FAILURE to let the caller know that we
		 * haven't completed the termination and it can't drop this
		 * object's reference on its shadow object yet.
		 * The reaper thread will take care of that once it has
		 * completed this object's termination.
		 */
		return KERN_FAILURE;
	}
	/*
	 * complete the VM object termination
	 */
	vm_object_reap(object);
	object = VM_OBJECT_NULL;

	/*
	 * the object lock was released by vm_object_reap()
	 *
	 * KERN_SUCCESS means that this object has been terminated
	 * and no longer needs its shadow object but still holds a
	 * reference on it.
	 * The caller is responsible for dropping that reference.
	 * We can't call vm_object_deallocate() here because that
	 * would create a recursion.
	 */
	return KERN_SUCCESS;
}


/*
 * vm_object_reap():
 *
 * Complete the termination of a VM object after it's been marked
 * as "terminating" and "!alive" by vm_object_terminate().
 *
 * The VM object must be locked by caller.
 * The lock will be released on return and the VM object is no longer valid.
 */
void
vm_object_reap(
	vm_object_t object)
{
	memory_object_t		pager;

	vm_object_lock_assert_exclusive(object);
	assert(object->paging_in_progress == 0);
	assert(object->activity_in_progress == 0);

	vm_object_reap_count++;

	pager = object->pager;
	object->pager = MEMORY_OBJECT_NULL;

	if (pager != MEMORY_OBJECT_NULL)
		memory_object_control_disable(object->pager_control);

	object->ref_count--;
#if	TASK_SWAPPER
	assert(object->res_count == 0);
#endif	/* TASK_SWAPPER */

	assert (object->ref_count == 0);

	/*
	 * remove from purgeable queue if it's on
	 */
	if (object->objq.next || object->objq.prev) {
	        purgeable_q_t queue = vm_purgeable_object_remove(object);
		assert(queue);

		/* Must take page lock for this - using it to protect token queue */
		vm_page_lock_queues();
		vm_purgeable_token_delete_first(queue);
        
		assert(queue->debug_count_objects>=0);
		vm_page_unlock_queues();
	}
    
	/*
	 *	Clean or free the pages, as appropriate.
	 *	It is possible for us to find busy/absent pages,
	 *	if some faults on this object were aborted.
	 */
	if (object->pageout) {
		assert(object->shadow != VM_OBJECT_NULL);

		vm_pageout_object_terminate(object);

	} else if (((object->temporary && !object->can_persist) || (pager == MEMORY_OBJECT_NULL))) {

		vm_object_reap_pages(object, REAP_REAP);
	}
	assert(queue_empty(&object->memq));
	assert(object->paging_in_progress == 0);
	assert(object->activity_in_progress == 0);
	assert(object->ref_count == 0);

	/*
	 * If the pager has not already been released by
	 * vm_object_destroy, we need to terminate it and
	 * release our reference to it here.
	 */
	if (pager != MEMORY_OBJECT_NULL) {
		vm_object_unlock(object);
		vm_object_release_pager(pager, object->hashed);
		vm_object_lock(object);
	}

	/* kick off anyone waiting on terminating */
	object->terminating = FALSE;
	vm_object_paging_begin(object);
	vm_object_paging_end(object);
	vm_object_unlock(object);

#if	MACH_PAGEMAP
	vm_external_destroy(object->existence_map, object->size);
#endif	/* MACH_PAGEMAP */

	object->shadow = VM_OBJECT_NULL;

	vm_object_lock_destroy(object);
	/*
	 *	Free the space for the object.
	 */
	zfree(vm_object_zone, object);
	object = VM_OBJECT_NULL;
}



#define V_O_R_MAX_BATCH 128


#define VM_OBJ_REAP_FREELIST(_local_free_q, do_disconnect)		\
	MACRO_BEGIN							\
	if (_local_free_q) {						\
		if (do_disconnect) {					\
			vm_page_t m;					\
			for (m = _local_free_q;				\
			     m != VM_PAGE_NULL;				\
			     m = (vm_page_t) m->pageq.next) {		\
				if (m->pmapped) {			\
					pmap_disconnect(m->phys_page);	\
				}					\
			}						\
		}							\
		vm_page_free_list(_local_free_q, TRUE);			\
		_local_free_q = VM_PAGE_NULL;				\
	}								\
	MACRO_END


void
vm_object_reap_pages(
	vm_object_t 	object,
	int		reap_type)
{
	vm_page_t	p;
	vm_page_t	next;
	vm_page_t	local_free_q = VM_PAGE_NULL;
	int		loop_count;
	boolean_t	disconnect_on_release;

	if (reap_type == REAP_DATA_FLUSH) {
		/*
		 * We need to disconnect pages from all pmaps before
		 * releasing them to the free list
		 */
		disconnect_on_release = TRUE;
	} else {
		/*
		 * Either the caller has already disconnected the pages
		 * from all pmaps, or we disconnect them here as we add
		 * them to out local list of pages to be released.
		 * No need to re-disconnect them when we release the pages
		 * to the free list.
		 */
		disconnect_on_release = FALSE;
	}
		
restart_after_sleep:
	if (queue_empty(&object->memq))
		return;
	loop_count = V_O_R_MAX_BATCH + 1;

	vm_page_lockspin_queues();

	next = (vm_page_t)queue_first(&object->memq);

	while (!queue_end(&object->memq, (queue_entry_t)next)) {

		p = next;
		next = (vm_page_t)queue_next(&next->listq);

		if (--loop_count == 0) {
					
			vm_page_unlock_queues();

			if (local_free_q) {
				/*
				 * Free the pages we reclaimed so far
				 * and take a little break to avoid
				 * hogging the page queue lock too long
				 */
				VM_OBJ_REAP_FREELIST(local_free_q,
						     disconnect_on_release);
			} else
				mutex_pause(0);

			loop_count = V_O_R_MAX_BATCH + 1;

			vm_page_lockspin_queues();
		}
		if (reap_type == REAP_DATA_FLUSH || reap_type == REAP_TERMINATE) {

			if (reap_type == REAP_DATA_FLUSH &&
			    ((p->pageout == TRUE || p->cleaning == TRUE) && p->list_req_pending == TRUE)) {
				p->list_req_pending = FALSE;
				p->cleaning = FALSE;
				/*
				 * need to drop the laundry count...
				 * we may also need to remove it
				 * from the I/O paging queue...
				 * vm_pageout_throttle_up handles both cases
				 *
				 * the laundry and pageout_queue flags are cleared...
				 */
#if CONFIG_EMBEDDED
				if (p->laundry) 
					vm_pageout_throttle_up(p);
#else
				vm_pageout_throttle_up(p);
#endif
				if (p->pageout == TRUE) {
					/*
					 * toss the wire count we picked up
					 * when we initially set this page up
					 * to be cleaned and stolen...
					 */
					vm_page_unwire(p, TRUE);
					p->pageout = FALSE;
				}
				PAGE_WAKEUP(p);

			} else if (p->busy || p->cleaning) {

				vm_page_unlock_queues();
				/*
				 * free the pages reclaimed so far
				 */
				VM_OBJ_REAP_FREELIST(local_free_q,
						     disconnect_on_release);

				PAGE_SLEEP(object, p, THREAD_UNINT);

				goto restart_after_sleep;
			}
		}
		switch (reap_type) {

		case REAP_DATA_FLUSH:
			if (VM_PAGE_WIRED(p)) {
				/*
				 * this is an odd case... perhaps we should
				 * zero-fill this page since we're conceptually
				 * tossing its data at this point, but leaving
				 * it on the object to honor the 'wire' contract
				 */
				continue;
			}
			break;
			
		case REAP_PURGEABLE:
			if (VM_PAGE_WIRED(p)) {
				/* can't purge a wired page */
				vm_page_purged_wired++;
				continue;
			}

			if (p->busy) {
				/*
				 * We can't reclaim a busy page but we can
				 * make it pageable (it's not wired) to make
				 * sure that it gets considered by
				 * vm_pageout_scan() later.
				 */
				vm_page_deactivate(p);
				vm_page_purged_busy++;
				continue;
			}

			if (p->cleaning || p->laundry || p->list_req_pending) {
				/*
				 * page is being acted upon,
				 * so don't mess with it
				 */
				vm_page_purged_others++;
				continue;
			}
			assert(p->object != kernel_object);

			/*
			 * we can discard this page...
			 */
			if (p->pmapped == TRUE) {
				int refmod_state;
				/*
				 * unmap the page
				 */
				refmod_state = pmap_disconnect(p->phys_page);
				if (refmod_state & VM_MEM_MODIFIED) {
					p->dirty = TRUE;
				}
			}
			if (p->dirty || p->precious) {
				/*
				 * we saved the cost of cleaning this page !
				 */
				vm_page_purged_count++;
			}

			break;

		case REAP_TERMINATE:
			if (p->absent || p->private) {
				/*
				 *	For private pages, VM_PAGE_FREE just
				 *	leaves the page structure around for
				 *	its owner to clean up.  For absent
				 *	pages, the structure is returned to
				 *	the appropriate pool.
				 */
				break;
			}
			if (p->fictitious) {
				assert (p->phys_page == vm_page_guard_addr);
				break;
			}
			if (!p->dirty && p->wpmapped)
				p->dirty = pmap_is_modified(p->phys_page);

			if ((p->dirty || p->precious) && !p->error && object->alive) {

				p->busy = TRUE;

				VM_PAGE_QUEUES_REMOVE(p);

				vm_page_unlock_queues();
				/*
				 * free the pages reclaimed so far
				 */
				VM_OBJ_REAP_FREELIST(local_free_q,
						     disconnect_on_release);

				/*
				 * flush page... page will be freed
				 * upon completion of I/O
				 */
				vm_pageout_cluster(p);
				vm_object_paging_wait(object, THREAD_UNINT);

				goto restart_after_sleep;
			}
			break;

		case REAP_REAP:
			break;
		}
		vm_page_free_prepare_queues(p);
		assert(p->pageq.next == NULL && p->pageq.prev == NULL);
		/*
		 * Add this page to our list of reclaimed pages,
		 * to be freed later.
		 */
		p->pageq.next = (queue_entry_t) local_free_q;
		local_free_q = p;
	}
	vm_page_unlock_queues();

	/*
	 * Free the remaining reclaimed pages
	 */
	VM_OBJ_REAP_FREELIST(local_free_q,
			     disconnect_on_release);
}


void
vm_object_reap_async(
	vm_object_t	object)
{
	vm_object_lock_assert_exclusive(object);

	vm_object_reaper_lock_spin();

	vm_object_reap_count_async++;

	/* enqueue the VM object... */
	queue_enter(&vm_object_reaper_queue, object,
		    vm_object_t, cached_list);

	vm_object_reaper_unlock();

	/* ... and wake up the reaper thread */
	thread_wakeup((event_t) &vm_object_reaper_queue);
}


void
vm_object_reaper_thread(void)
{
	vm_object_t	object, shadow_object;

	vm_object_reaper_lock_spin();

	while (!queue_empty(&vm_object_reaper_queue)) {
		queue_remove_first(&vm_object_reaper_queue,
				   object,
				   vm_object_t,
				   cached_list);

		vm_object_reaper_unlock();
		vm_object_lock(object);

		assert(object->terminating);
		assert(!object->alive);
		
		/*
		 * The pageout daemon might be playing with our pages.
		 * Now that the object is dead, it won't touch any more
		 * pages, but some pages might already be on their way out.
		 * Hence, we wait until the active paging activities have
		 * ceased before we break the association with the pager
		 * itself.
		 */
		while (object->paging_in_progress != 0 ||
			object->activity_in_progress != 0) {
			vm_object_wait(object,
				       VM_OBJECT_EVENT_PAGING_IN_PROGRESS,
				       THREAD_UNINT);
			vm_object_lock(object);
		}

		shadow_object =
			object->pageout ? VM_OBJECT_NULL : object->shadow;

		vm_object_reap(object);
		/* cache is unlocked and object is no longer valid */
		object = VM_OBJECT_NULL;

		if (shadow_object != VM_OBJECT_NULL) {
			/*
			 * Drop the reference "object" was holding on
			 * its shadow object.
			 */
			vm_object_deallocate(shadow_object);
			shadow_object = VM_OBJECT_NULL;
		}
		vm_object_reaper_lock_spin();
	}

	/* wait for more work... */
	assert_wait((event_t) &vm_object_reaper_queue, THREAD_UNINT);

	vm_object_reaper_unlock();

	thread_block((thread_continue_t) vm_object_reaper_thread);
	/*NOTREACHED*/
}

/*
 *	Routine:	vm_object_pager_wakeup
 *	Purpose:	Wake up anyone waiting for termination of a pager.
 */

static void
vm_object_pager_wakeup(
	memory_object_t	pager)
{
	vm_object_hash_entry_t	entry;
	boolean_t		waiting = FALSE;
	lck_mtx_t		*lck;

	/*
	 *	If anyone was waiting for the memory_object_terminate
	 *	to be queued, wake them up now.
	 */
	lck = vm_object_hash_lock_spin(pager);
	entry = vm_object_hash_lookup(pager, TRUE);
	if (entry != VM_OBJECT_HASH_ENTRY_NULL)
		waiting = entry->waiting;
	vm_object_hash_unlock(lck);

	if (entry != VM_OBJECT_HASH_ENTRY_NULL) {
		if (waiting)
			thread_wakeup((event_t) pager);
		vm_object_hash_entry_free(entry);
	}
}

/*
 *	Routine:	vm_object_release_pager
 *	Purpose:	Terminate the pager and, upon completion,
 *			release our last reference to it.
 *			just like memory_object_terminate, except
 *			that we wake up anyone blocked in vm_object_enter
 *			waiting for termination message to be queued
 *			before calling memory_object_init.
 */
static void
vm_object_release_pager(
	memory_object_t	pager,
	boolean_t	hashed)
{

	/*
	 *	Terminate the pager.
	 */

	(void) memory_object_terminate(pager);

	if (hashed == TRUE) {
		/*
		 *	Wakeup anyone waiting for this terminate
		 *      and remove the entry from the hash
		 */
		vm_object_pager_wakeup(pager);
	}
	/*
	 *	Release reference to pager.
	 */
	memory_object_deallocate(pager);
}

/*
 *	Routine:	vm_object_destroy
 *	Purpose:
 *		Shut down a VM object, despite the
 *		presence of address map (or other) references
 *		to the vm_object.
 */
kern_return_t
vm_object_destroy(
	vm_object_t		object,
	__unused kern_return_t		reason)
{
	memory_object_t		old_pager;

	if (object == VM_OBJECT_NULL)
		return(KERN_SUCCESS);

	/*
	 *	Remove the pager association immediately.
	 *
	 *	This will prevent the memory manager from further
	 *	meddling.  [If it wanted to flush data or make
	 *	other changes, it should have done so before performing
	 *	the destroy call.]
	 */

	vm_object_lock(object);
	object->can_persist = FALSE;
	object->named = FALSE;
	object->alive = FALSE;

	if (object->hashed) {
		lck_mtx_t	*lck;
		/*
		 *	Rip out the pager from the vm_object now...
		 */
		lck = vm_object_hash_lock_spin(object->pager);
		vm_object_remove(object);
		vm_object_hash_unlock(lck);
	}
	old_pager = object->pager;
	object->pager = MEMORY_OBJECT_NULL;
	if (old_pager != MEMORY_OBJECT_NULL)
		memory_object_control_disable(object->pager_control);

	/*
	 * Wait for the existing paging activity (that got
	 * through before we nulled out the pager) to subside.
	 */

	vm_object_paging_wait(object, THREAD_UNINT);
	vm_object_unlock(object);

	/*
	 *	Terminate the object now.
	 */
	if (old_pager != MEMORY_OBJECT_NULL) {
		vm_object_release_pager(old_pager, object->hashed);

		/* 
		 * JMM - Release the caller's reference.  This assumes the
		 * caller had a reference to release, which is a big (but
		 * currently valid) assumption if this is driven from the
		 * vnode pager (it is holding a named reference when making
		 * this call)..
		 */
		vm_object_deallocate(object);

	}
	return(KERN_SUCCESS);
}


#define VM_OBJ_DEACT_ALL_STATS DEBUG
#if VM_OBJ_DEACT_ALL_STATS
uint32_t vm_object_deactivate_all_pages_batches = 0;
uint32_t vm_object_deactivate_all_pages_pages = 0;
#endif /* VM_OBJ_DEACT_ALL_STATS */
/*
 *	vm_object_deactivate_all_pages
 *
 *	Deactivate all pages in the specified object.  (Keep its pages
 *	in memory even though it is no longer referenced.)
 *
 *	The object must be locked.
 */
static void
vm_object_deactivate_all_pages(
	register vm_object_t	object)
{
	register vm_page_t	p;
	int			loop_count;
#if VM_OBJ_DEACT_ALL_STATS
	int			pages_count;
#endif /* VM_OBJ_DEACT_ALL_STATS */
#define V_O_D_A_P_MAX_BATCH	256

	loop_count = V_O_D_A_P_MAX_BATCH;
#if VM_OBJ_DEACT_ALL_STATS
	pages_count = 0;
#endif /* VM_OBJ_DEACT_ALL_STATS */
	vm_page_lock_queues();
	queue_iterate(&object->memq, p, vm_page_t, listq) {
		if (--loop_count == 0) {
#if VM_OBJ_DEACT_ALL_STATS
			hw_atomic_add(&vm_object_deactivate_all_pages_batches,
				      1);
			hw_atomic_add(&vm_object_deactivate_all_pages_pages,
				      pages_count);
			pages_count = 0;
#endif /* VM_OBJ_DEACT_ALL_STATS */
			lck_mtx_yield(&vm_page_queue_lock);
			loop_count = V_O_D_A_P_MAX_BATCH;
		}
		if (!p->busy && !p->throttled) {
#if VM_OBJ_DEACT_ALL_STATS
			pages_count++;
#endif /* VM_OBJ_DEACT_ALL_STATS */
			vm_page_deactivate(p);
		}
	}
#if VM_OBJ_DEACT_ALL_STATS
	if (pages_count) {
		hw_atomic_add(&vm_object_deactivate_all_pages_batches, 1);
		hw_atomic_add(&vm_object_deactivate_all_pages_pages,
			      pages_count);
		pages_count = 0;
	}
#endif /* VM_OBJ_DEACT_ALL_STATS */
	vm_page_unlock_queues();
}



/*
 * when deallocating pages it is necessary to hold 
 * the vm_page_queue_lock (a hot global lock) for certain operations
 * on the page... however, the majority of the work can be done
 * while merely holding the object lock... to mitigate the time spent behind the
 * global lock, go to a 2 pass algorithm... collect pages up to DELAYED_WORK_LIMIT
 * while doing all of the work that doesn't require the vm_page_queue_lock...
 * them call dw_do_work to acquire the vm_page_queue_lock and do the
 * necessary work for each page... we will grab the busy bit on the page
 * so that dw_do_work can drop the object lock if it can't immediately take the
 * vm_page_queue_lock in order to compete for the locks in the same order that
 * vm_pageout_scan takes them.
 */

#define DELAYED_WORK_LIMIT	32

#define DW_clear_reference	0x01
#define DW_move_page		0x02
#define DW_clear_busy		0x04
#define DW_PAGE_WAKEUP		0x08


struct dw {
	vm_page_t	dw_m;
	int		dw_mask;
};

static void dw_do_work(vm_object_t object, struct dw *dwp, int dw_count);


static void
dw_do_work(
	vm_object_t 	object,
	struct dw 	*dwp,
	int		dw_count)
{
	vm_page_t	m;
	int		j;

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

		m = dwp->dw_m;

		if (dwp->dw_mask & DW_clear_reference)
			m->reference = FALSE;

		if (dwp->dw_mask & DW_move_page) {
			VM_PAGE_QUEUES_REMOVE(m);

			assert(!m->laundry);
			assert(m->object != kernel_object);
			assert(m->pageq.next == NULL &&
			       m->pageq.prev == NULL);
					
			if (m->zero_fill) {
				queue_enter_first(&vm_page_queue_zf, m, vm_page_t, pageq);
				vm_zf_queue_count++;
			} else {
				queue_enter_first(&vm_page_queue_inactive, m, vm_page_t, pageq);
			}
			m->inactive = TRUE;

			if (!m->fictitious) {
				vm_page_inactive_count++;
				token_new_pagecount++;
			} else {
				assert(m->phys_page == vm_page_fictitious_addr);
			}
		}
		if (dwp->dw_mask & DW_clear_busy)
			dwp->dw_m->busy = FALSE;

		if (dwp->dw_mask & DW_PAGE_WAKEUP)
			PAGE_WAKEUP(dwp->dw_m);
	}
	vm_page_unlock_queues();

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
}



/*
 * The "chunk" macros are used by routines below when looking for pages to deactivate.  These
 * exist because of the need to handle shadow chains.  When deactivating pages, we only
 * want to deactive the ones at the top most level in the object chain.  In order to do
 * this efficiently, the specified address range is divided up into "chunks" and we use
 * a bit map to keep track of which pages have already been processed as we descend down
 * the shadow chain.  These chunk macros hide the details of the bit map implementation
 * as much as we can.
 *
 * For convenience, we use a 64-bit data type as the bit map, and therefore a chunk is
 * set to 64 pages.  The bit map is indexed from the low-order end, so that the lowest
 * order bit represents page 0 in the current range and highest order bit represents
 * page 63.
 *
 * For further convenience, we also use negative logic for the page state in the bit map.
 * The bit is set to 1 to indicate it has not yet been seen, and to 0 to indicate it has
 * been processed.  This way we can simply test the 64-bit long word to see if it's zero
 * to easily tell if the whole range has been processed.  Therefore, the bit map starts
 * out with all the bits set.  The macros below hide all these details from the caller.
 */

#define PAGES_IN_A_CHUNK	64	/* The number of pages in the chunk must */
					/* be the same as the number of bits in  */
					/* the chunk_state_t type. We use 64     */
					/* just for convenience.		 */

#define CHUNK_SIZE	(PAGES_IN_A_CHUNK * PAGE_SIZE_64)	/* Size of a chunk in bytes */

typedef uint64_t	chunk_state_t;

/*
 * The bit map uses negative logic, so we start out with all 64 bits set to indicate
 * that no pages have been processed yet.  Also, if len is less than the full CHUNK_SIZE,
 * then we mark pages beyond the len as having been "processed" so that we don't waste time
 * looking at pages in that range.  This can save us from unnecessarily chasing down the 
 * shadow chain.
 */

#define CHUNK_INIT(c, len) 						\
	MACRO_BEGIN							\
	uint64_t p;							\
									\
	(c) = 0xffffffffffffffffLL; 					\
									\
	for (p = (len) / PAGE_SIZE_64; p < PAGES_IN_A_CHUNK; p++)	\
		MARK_PAGE_HANDLED(c, p);				\
	MACRO_END

/*
 * Return true if all pages in the chunk have not yet been processed.
 */

#define CHUNK_NOT_COMPLETE(c)	((c) != 0)

/*
 * Return true if the page at offset 'p' in the bit map has already been handled
 * while processing a higher level object in the shadow chain.
 */

#define PAGE_ALREADY_HANDLED(c, p)	(((c) & (1LL << (p))) == 0)

/*
 * Mark the page at offset 'p' in the bit map as having been processed.
 */

#define MARK_PAGE_HANDLED(c, p) \
MACRO_BEGIN \
	(c) = (c) & ~(1LL << (p)); \
MACRO_END


/*
 * Return true if the page at the given offset has been paged out.  Object is
 * locked upon entry and returned locked.
 */

static boolean_t
page_is_paged_out(
	vm_object_t		object,
	vm_object_offset_t	offset)
{
	kern_return_t	kr;
	memory_object_t	pager;

	/*
	 * Check the existence map for the page if we have one, otherwise
	 * ask the pager about this page.
	 */

#if MACH_PAGEMAP
	if (object->existence_map) {
		if (vm_external_state_get(object->existence_map, offset)
		    == VM_EXTERNAL_STATE_EXISTS) {
			/*
			 * We found the page
			 */

			return TRUE;
		}
	} else
#endif
		if (object->internal &&
		   object->alive &&
		   !object->terminating &&
		   object->pager_ready) {

		/*
		 * We're already holding a "paging in progress" reference
		 * so the object can't disappear when we release the lock.
		 */

		assert(object->paging_in_progress);
		pager = object->pager;
		vm_object_unlock(object);

		kr = memory_object_data_request(
			pager,
			offset + object->paging_offset,
			0,	/* just poke the pager */
			VM_PROT_READ,
			NULL);

		vm_object_lock(object);

		if (kr == KERN_SUCCESS) {

			/*
			 * We found the page
			 */

			return TRUE;
		}
	}

	return FALSE;
}


/*
 * Deactivate the pages in the specified object and range.  If kill_page is set, also discard any
 * page modified state from the pmap.  Update the chunk_state as we go along.  The caller must specify
 * a size that is less than or equal to the CHUNK_SIZE.
 */

static void
deactivate_pages_in_object(
	vm_object_t		object,
	vm_object_offset_t	offset,
	vm_object_size_t	size,
	boolean_t               kill_page,
	boolean_t		reusable_page,
#if !MACH_ASSERT
	__unused
#endif
	boolean_t		all_reusable,
	chunk_state_t		*chunk_state)
{
	vm_page_t	m;
	int		p;
	struct	dw	dw_array[DELAYED_WORK_LIMIT];
	struct	dw	*dwp;
	int		dw_count;
	unsigned int	reusable = 0;


	/*
	 * Examine each page in the chunk.  The variable 'p' is the page number relative to the start of the
	 * chunk.  Since this routine is called once for each level in the shadow chain, the chunk_state may
	 * have pages marked as having been processed already.  We stop the loop early if we find we've handled
	 * all the pages in the chunk.
	 */

	dwp = &dw_array[0];
	dw_count = 0;

	for(p = 0; size && CHUNK_NOT_COMPLETE(*chunk_state); p++, size -= PAGE_SIZE_64, offset += PAGE_SIZE_64) {

		/*
		 * If this offset has already been found and handled in a higher level object, then don't
		 * do anything with it in the current shadow object.
		 */

		if (PAGE_ALREADY_HANDLED(*chunk_state, p))
			continue;
	
		/*
		 * See if the page at this offset is around.  First check to see if the page is resident,
		 * then if not, check the existence map or with the pager.
		 */

	        if ((m = vm_page_lookup(object, offset)) != VM_PAGE_NULL) {

			/*
			 * We found a page we were looking for.  Mark it as "handled" now in the chunk_state
			 * so that we won't bother looking for a page at this offset again if there are more
			 * shadow objects.  Then deactivate the page.
			 */

			MARK_PAGE_HANDLED(*chunk_state, p);
	
			if (( !VM_PAGE_WIRED(m)) && (!m->private) && (!m->gobbled) && (!m->busy)) {
				int	clear_refmod;
	
				assert(!m->laundry);
	
				clear_refmod = VM_MEM_REFERENCED;
				dwp->dw_mask = DW_clear_reference;

				if ((kill_page) && (object->internal)) {
			        	m->precious = FALSE;
				        m->dirty = FALSE;

					clear_refmod |= VM_MEM_MODIFIED;
					if (m->throttled) {
						/*
						 * This page is now clean and
						 * reclaimable.  Move it out
						 * of the throttled queue, so
						 * that vm_pageout_scan() can
						 * find it.
						 */
						dwp->dw_mask |= DW_move_page;
					}
#if	MACH_PAGEMAP
					vm_external_state_clr(object->existence_map, offset);
#endif	/* MACH_PAGEMAP */

					if (reusable_page && !m->reusable) {
						assert(!all_reusable);
						assert(!object->all_reusable);
						m->reusable = TRUE;
						object->reusable_page_count++;
						assert(object->resident_page_count >= object->reusable_page_count);
						reusable++;
#if CONFIG_EMBEDDED
					} else {
						if (m->reusable) {
							m->reusable = FALSE;
							object->reusable_page_count--;
						}
#endif
					}
				}
				pmap_clear_refmod(m->phys_page, clear_refmod);

				if (!m->throttled && !(reusable_page || all_reusable))
					dwp->dw_mask |= DW_move_page;
				/*
				 * dw_do_work may need to drop the object lock
				 * if it does, we need the pages its looking at to
				 * be held stable via the busy bit.
				 */
				m->busy = TRUE;
				dwp->dw_mask |= (DW_clear_busy | DW_PAGE_WAKEUP);

				dwp->dw_m = m;
				dwp++;
				dw_count++;

				if (dw_count >= DELAYED_WORK_LIMIT) {
					if (reusable) {
						OSAddAtomic(reusable,
							    &vm_page_stats_reusable.reusable_count);
						vm_page_stats_reusable.reusable += reusable;
						reusable = 0;
					}
					dw_do_work(object, &dw_array[0], dw_count);

					dwp = &dw_array[0];
					dw_count = 0;
				}
			}

		} else {

			/*
			 * The page at this offset isn't memory resident, check to see if it's
			 * been paged out.  If so, mark it as handled so we don't bother looking
			 * for it in the shadow chain.
			 */

			if (page_is_paged_out(object, offset)) {
				MARK_PAGE_HANDLED(*chunk_state, p);

				/*
				 * If we're killing a non-resident page, then clear the page in the existence 
				 * map so we don't bother paging it back in if it's touched again in the future.
				 */

				if ((kill_page) && (object->internal)) {
#if	MACH_PAGEMAP
					vm_external_state_clr(object->existence_map, offset);
#endif	/* MACH_PAGEMAP */
				}
			}
		}
	}

	if (reusable) {
		OSAddAtomic(reusable, &vm_page_stats_reusable.reusable_count);
		vm_page_stats_reusable.reusable += reusable;	
		reusable = 0;
	}
		
	if (dw_count)
		dw_do_work(object, &dw_array[0], dw_count);
}


/*
 * Deactive a "chunk" of the given range of the object starting at offset.  A "chunk"
 * will always be less than or equal to the given size.  The total range is divided up
 * into chunks for efficiency and performance related to the locks and handling the shadow
 * chain.  This routine returns how much of the given "size" it actually processed.  It's
 * up to the caler to loop and keep calling this routine until the entire range they want
 * to process has been done.
 */

static vm_object_size_t
deactivate_a_chunk(
	vm_object_t		orig_object,
	vm_object_offset_t	offset,
	vm_object_size_t	size,
	boolean_t               kill_page,
	boolean_t		reusable_page,
	boolean_t		all_reusable)
{
	vm_object_t		object;
	vm_object_t		tmp_object;
	vm_object_size_t	length;
	chunk_state_t		chunk_state;


	/*
	 * Get set to do a chunk.  We'll do up to CHUNK_SIZE, but no more than the
	 * remaining size the caller asked for.
	 */

	length = MIN(size, CHUNK_SIZE);

	/*
	 * The chunk_state keeps track of which pages we've already processed if there's
	 * a shadow chain on this object.  At this point, we haven't done anything with this
	 * range of pages yet, so initialize the state to indicate no pages processed yet.
	 */

	CHUNK_INIT(chunk_state, length);
	object = orig_object;

	/*
	 * Start at the top level object and iterate around the loop once for each object
	 * in the shadow chain.  We stop processing early if we've already found all the pages
	 * in the range.  Otherwise we stop when we run out of shadow objects.
	 */

	while (object && CHUNK_NOT_COMPLETE(chunk_state)) {
		vm_object_paging_begin(object);

		deactivate_pages_in_object(object, offset, length, kill_page, reusable_page, all_reusable, &chunk_state);

		vm_object_paging_end(object);

		/*
		 * We've finished with this object, see if there's a shadow object.  If
		 * there is, update the offset and lock the new object.  We also turn off
		 * kill_page at this point since we only kill pages in the top most object.
		 */

		tmp_object = object->shadow;

		if (tmp_object) {
			kill_page = FALSE;
			reusable_page = FALSE;
			all_reusable = FALSE;
		        offset += object->shadow_offset;
		        vm_object_lock(tmp_object);
		}

		if (object != orig_object)
		        vm_object_unlock(object);

		object = tmp_object;
	}

	if (object && object != orig_object)
	        vm_object_unlock(object);

	return length;
}



/*
 * Move any resident pages in the specified range to the inactive queue.  If kill_page is set,
 * we also clear the modified status of the page and "forget" any changes that have been made
 * to the page.
 */

__private_extern__ void
vm_object_deactivate_pages(
	vm_object_t		object,
	vm_object_offset_t	offset,
	vm_object_size_t	size,
	boolean_t               kill_page,
	boolean_t		reusable_page)
{
	vm_object_size_t	length;
	boolean_t		all_reusable;

	/*
	 * We break the range up into chunks and do one chunk at a time.  This is for
	 * efficiency and performance while handling the shadow chains and the locks.	
	 * The deactivate_a_chunk() function returns how much of the range it processed.
	 * We keep calling this routine until the given size is exhausted.
	 */


	all_reusable = FALSE;
	if (reusable_page &&
	    object->size != 0 &&
	    object->size == size &&
	    object->reusable_page_count == 0) {
		all_reusable = TRUE;
		reusable_page = FALSE;
	}

#if CONFIG_EMBEDDED
	if ((reusable_page || all_reusable) && object->all_reusable) {
		/* This means MADV_FREE_REUSABLE has been called twice, which 
		 * is probably illegal. */
		return;
	}
#endif

	while (size) {
		length = deactivate_a_chunk(object, offset, size, kill_page, reusable_page, all_reusable);

		size -= length;
		offset += length;
	}

	if (all_reusable) {
		if (!object->all_reusable) {
			unsigned int reusable;

			object->all_reusable = TRUE;
			assert(object->reusable_page_count == 0);
			/* update global stats */
			reusable = object->resident_page_count;
			OSAddAtomic(reusable,
				    &vm_page_stats_reusable.reusable_count);
			vm_page_stats_reusable.reusable += reusable;
			vm_page_stats_reusable.all_reusable_calls++;
		}
	} else if (reusable_page) {
		vm_page_stats_reusable.partial_reusable_calls++;
	}
}

void
vm_object_reuse_pages(
	vm_object_t		object,
	vm_object_offset_t	start_offset,
	vm_object_offset_t	end_offset,
	boolean_t		allow_partial_reuse)
{
	vm_object_offset_t	cur_offset;
	vm_page_t		m;
	unsigned int		reused, reusable;

#define VM_OBJECT_REUSE_PAGE(object, m, reused)				\
	MACRO_BEGIN							\
		if ((m) != VM_PAGE_NULL &&				\
		    (m)->reusable) {					\
			assert((object)->reusable_page_count <=		\
			       (object)->resident_page_count);		\
			assert((object)->reusable_page_count > 0);	\
			(object)->reusable_page_count--;		\
			(m)->reusable = FALSE;				\
			(reused)++;					\
		}							\
	MACRO_END

	reused = 0;
	reusable = 0;

	vm_object_lock_assert_exclusive(object);

	if (object->all_reusable) {
		assert(object->reusable_page_count == 0);
		object->all_reusable = FALSE;
		if (end_offset - start_offset == object->size ||
		    !allow_partial_reuse) {
			vm_page_stats_reusable.all_reuse_calls++;
			reused = object->resident_page_count;
		} else {
			vm_page_stats_reusable.partial_reuse_calls++;
			queue_iterate(&object->memq, m, vm_page_t, listq) {
				if (m->offset < start_offset ||
				    m->offset >= end_offset) {
					m->reusable = TRUE;
					object->reusable_page_count++;
					assert(object->resident_page_count >= object->reusable_page_count);
					continue;
				} else {
					assert(!m->reusable);
					reused++;
				}
			}
		}
	} else if (object->resident_page_count >
		   ((end_offset - start_offset) >> PAGE_SHIFT)) {
		vm_page_stats_reusable.partial_reuse_calls++;
		for (cur_offset = start_offset;
		     cur_offset < end_offset;
		     cur_offset += PAGE_SIZE_64) {
			if (object->reusable_page_count == 0) {
				break;
			}
			m = vm_page_lookup(object, cur_offset);
			VM_OBJECT_REUSE_PAGE(object, m, reused);
		}
	} else {
		vm_page_stats_reusable.partial_reuse_calls++;
		queue_iterate(&object->memq, m, vm_page_t, listq) {
			if (object->reusable_page_count == 0) {
				break;
			}
			if (m->offset < start_offset ||
			    m->offset >= end_offset) {
				continue;
			}
			VM_OBJECT_REUSE_PAGE(object, m, reused);
		}
	}

	/* update global stats */
	OSAddAtomic(reusable-reused, &vm_page_stats_reusable.reusable_count);
	vm_page_stats_reusable.reused += reused;
	vm_page_stats_reusable.reusable += reusable;
}

/*
 *	Routine:	vm_object_pmap_protect
 *
 *	Purpose:
 *		Reduces the permission for all physical
 *		pages in the specified object range.
 *
 *		If removing write permission only, it is
 *		sufficient to protect only the pages in
 *		the top-level object; only those pages may
 *		have write permission.
 *
 *		If removing all access, we must follow the
 *		shadow chain from the top-level object to
 *		remove access to all pages in shadowed objects.
 *
 *		The object must *not* be locked.  The object must
 *		be temporary/internal.  
 *
 *              If pmap is not NULL, this routine assumes that
 *              the only mappings for the pages are in that
 *              pmap.
 */

__private_extern__ void
vm_object_pmap_protect(
	register vm_object_t		object,
	register vm_object_offset_t	offset,
	vm_object_size_t		size,
	pmap_t				pmap,
	vm_map_offset_t			pmap_start,
	vm_prot_t			prot)
{
	if (object == VM_OBJECT_NULL)
	    return;
	size = vm_object_round_page(size);
	offset = vm_object_trunc_page(offset);

	vm_object_lock(object);

	if (object->phys_contiguous) {
		if (pmap != NULL) {
			vm_object_unlock(object);
			pmap_protect(pmap, pmap_start, pmap_start + size, prot);
		} else {
			vm_object_offset_t phys_start, phys_end, phys_addr;

			phys_start = object->shadow_offset + offset;
			phys_end = phys_start + size;
			assert(phys_start <= phys_end);
			assert(phys_end <= object->shadow_offset + object->size);
			vm_object_unlock(object);

			for (phys_addr = phys_start;
			     phys_addr < phys_end;
			     phys_addr += PAGE_SIZE_64) {
				pmap_page_protect((ppnum_t) (phys_addr >> PAGE_SHIFT), prot);
			}
		}
		return;
	}

	assert(object->internal);

	while (TRUE) {
	   if (ptoa_64(object->resident_page_count) > size/2 && pmap != PMAP_NULL) {
		vm_object_unlock(object);
		pmap_protect(pmap, pmap_start, pmap_start + size, prot);
		return;
	    }

	    /* if we are doing large ranges with respect to resident */
	    /* page count then we should interate over pages otherwise */
	    /* inverse page look-up will be faster */
	    if (ptoa_64(object->resident_page_count / 4) <  size) {
		vm_page_t		p;
		vm_object_offset_t	end;

		end = offset + size;

		if (pmap != PMAP_NULL) {
		  queue_iterate(&object->memq, p, vm_page_t, listq) {
		    if (!p->fictitious &&
			(offset <= p->offset) && (p->offset < end)) {
			vm_map_offset_t start;

			start = pmap_start + p->offset - offset;
			pmap_protect(pmap, start, start + PAGE_SIZE_64, prot);
		    }
		  }
		} else {
		  queue_iterate(&object->memq, p, vm_page_t, listq) {
		    if (!p->fictitious &&
			(offset <= p->offset) && (p->offset < end)) {

		        pmap_page_protect(p->phys_page, prot);
		    }
		  }
		}
	   } else {
		vm_page_t		p;
		vm_object_offset_t	end;
		vm_object_offset_t	target_off;

		end = offset + size;

		if (pmap != PMAP_NULL) {
			for(target_off = offset; 
			    target_off < end;
			    target_off += PAGE_SIZE) {
				p = vm_page_lookup(object, target_off);
				if (p != VM_PAGE_NULL) {
					vm_object_offset_t start;
					start = pmap_start + 
						(p->offset - offset);
					pmap_protect(pmap, start, 
						     start + PAGE_SIZE, prot);
				}
		    	}
		} else {
			for(target_off = offset; 
				target_off < end; target_off += PAGE_SIZE) {
				p = vm_page_lookup(object, target_off);
				if (p != VM_PAGE_NULL) {
				        pmap_page_protect(p->phys_page, prot);
				}
		    	}
		}
	  }

	    if (prot == VM_PROT_NONE) {
		/*
		 * Must follow shadow chain to remove access
		 * to pages in shadowed objects.
		 */
		register vm_object_t	next_object;

		next_object = object->shadow;
		if (next_object != VM_OBJECT_NULL) {
		    offset += object->shadow_offset;
		    vm_object_lock(next_object);
		    vm_object_unlock(object);
		    object = next_object;
		}
		else {
		    /*
		     * End of chain - we are done.
		     */
		    break;
		}
	    }
	    else {
		/*
		 * Pages in shadowed objects may never have
		 * write permission - we may stop here.
		 */
		break;
	    }
	}

	vm_object_unlock(object);
}

/*
 *	Routine:	vm_object_copy_slowly
 *
 *	Description:
 *		Copy the specified range of the source
 *		virtual memory object without using
 *		protection-based optimizations (such
 *		as copy-on-write).  The pages in the
 *		region are actually copied.
 *
 *	In/out conditions:
 *		The caller must hold a reference and a lock
 *		for the source virtual memory object.  The source
 *		object will be returned *unlocked*.
 *
 *	Results:
 *		If the copy is completed successfully, KERN_SUCCESS is
 *		returned.  If the caller asserted the interruptible
 *		argument, and an interruption occurred while waiting
 *		for a user-generated event, MACH_SEND_INTERRUPTED is
 *		returned.  Other values may be returned to indicate
 *		hard errors during the copy operation.
 *
 *		A new virtual memory object is returned in a
 *		parameter (_result_object).  The contents of this
 *		new object, starting at a zero offset, are a copy
 *		of the source memory region.  In the event of
 *		an error, this parameter will contain the value
 *		VM_OBJECT_NULL.
 */
__private_extern__ kern_return_t
vm_object_copy_slowly(
	register vm_object_t	src_object,
	vm_object_offset_t	src_offset,
	vm_object_size_t	size,
	boolean_t		interruptible,
	vm_object_t		*_result_object)	/* OUT */
{
	vm_object_t		new_object;
	vm_object_offset_t	new_offset;

	struct vm_object_fault_info fault_info;

	XPR(XPR_VM_OBJECT, "v_o_c_slowly obj 0x%x off 0x%x size 0x%x\n",
	    src_object, src_offset, size, 0, 0);

	if (size == 0) {
		vm_object_unlock(src_object);
		*_result_object = VM_OBJECT_NULL;
		return(KERN_INVALID_ARGUMENT);
	}

	/*
	 *	Prevent destruction of the source object while we copy.
	 */

	vm_object_reference_locked(src_object);
	vm_object_unlock(src_object);

	/*
	 *	Create a new object to hold the copied pages.
	 *	A few notes:
	 *		We fill the new object starting at offset 0,
	 *		 regardless of the input offset.
	 *		We don't bother to lock the new object within
	 *		 this routine, since we have the only reference.
	 */

	new_object = vm_object_allocate(size);
	new_offset = 0;

	assert(size == trunc_page_64(size));	/* Will the loop terminate? */

	fault_info.interruptible = interruptible;
	fault_info.behavior  = VM_BEHAVIOR_SEQUENTIAL;
	fault_info.user_tag  = 0;
	fault_info.lo_offset = src_offset;
	fault_info.hi_offset = src_offset + size;
	fault_info.no_cache  = FALSE;
	fault_info.stealth = TRUE;
	fault_info.mark_zf_absent = FALSE;

	for ( ;
	    size != 0 ;
	    src_offset += PAGE_SIZE_64, 
			new_offset += PAGE_SIZE_64, size -= PAGE_SIZE_64
	    ) {
		vm_page_t	new_page;
		vm_fault_return_t result;

		vm_object_lock(new_object);

		while ((new_page = vm_page_alloc(new_object, new_offset))
				== VM_PAGE_NULL) {

			vm_object_unlock(new_object);

			if (!vm_page_wait(interruptible)) {
				vm_object_deallocate(new_object);
				vm_object_deallocate(src_object);
				*_result_object = VM_OBJECT_NULL;
				return(MACH_SEND_INTERRUPTED);
			}
			vm_object_lock(new_object);
		}
		vm_object_unlock(new_object);

		do {
			vm_prot_t	prot = VM_PROT_READ;
			vm_page_t	_result_page;
			vm_page_t	top_page;
			register
			vm_page_t	result_page;
			kern_return_t	error_code;

			vm_object_lock(src_object);
			vm_object_paging_begin(src_object);

			if (size > (vm_size_t) -1) {
				/* 32-bit overflow */
				fault_info.cluster_size = (vm_size_t) (0 - PAGE_SIZE);
			} else {
				fault_info.cluster_size = (vm_size_t) size;
				assert(fault_info.cluster_size == size);
			}

			XPR(XPR_VM_FAULT,"vm_object_copy_slowly -> vm_fault_page",0,0,0,0,0);
			result = vm_fault_page(src_object, src_offset,
				VM_PROT_READ, FALSE,
				&prot, &_result_page, &top_page,
			        (int *)0,
				&error_code, FALSE, FALSE, &fault_info);

			switch(result) {
			case VM_FAULT_SUCCESS:
				result_page = _result_page;

				/*
				 *	We don't need to hold the object
				 *	lock -- the busy page will be enough.
				 *	[We don't care about picking up any
				 *	new modifications.]
				 *
				 *	Copy the page to the new object.
				 *
				 *	POLICY DECISION:
				 *		If result_page is clean,
				 *		we could steal it instead
				 *		of copying.
				 */

				vm_object_unlock(result_page->object);
				vm_page_copy(result_page, new_page);

				/*
				 *	Let go of both pages (make them
				 *	not busy, perform wakeup, activate).
				 */
				vm_object_lock(new_object);
				new_page->dirty = TRUE;
				PAGE_WAKEUP_DONE(new_page);
				vm_object_unlock(new_object);

				vm_object_lock(result_page->object);
				PAGE_WAKEUP_DONE(result_page);

				vm_page_lockspin_queues();
				if (!result_page->active &&
				    !result_page->inactive &&
				    !result_page->throttled)
					vm_page_activate(result_page);
				vm_page_activate(new_page);
				vm_page_unlock_queues();

				/*
				 *	Release paging references and
				 *	top-level placeholder page, if any.
				 */

				vm_fault_cleanup(result_page->object,
						 top_page);

				break;
				
			case VM_FAULT_RETRY:
				break;

			case VM_FAULT_FICTITIOUS_SHORTAGE:
				vm_page_more_fictitious();
				break;

			case VM_FAULT_MEMORY_SHORTAGE:
				if (vm_page_wait(interruptible))
					break;
				/* fall thru */

			case VM_FAULT_INTERRUPTED:
				vm_object_lock(new_object);
				VM_PAGE_FREE(new_page);
				vm_object_unlock(new_object);
					
				vm_object_deallocate(new_object);
				vm_object_deallocate(src_object);
				*_result_object = VM_OBJECT_NULL;
				return(MACH_SEND_INTERRUPTED);

			case VM_FAULT_SUCCESS_NO_VM_PAGE:
				/* success but no VM page: fail */
				vm_object_paging_end(src_object);
				vm_object_unlock(src_object);
				/*FALLTHROUGH*/
			case VM_FAULT_MEMORY_ERROR:
				/*
				 * A policy choice:
				 *	(a) ignore pages that we can't
				 *	    copy
				 *	(b) return the null object if
				 *	    any page fails [chosen]
				 */

				vm_object_lock(new_object);
				VM_PAGE_FREE(new_page);
				vm_object_unlock(new_object);

				vm_object_deallocate(new_object);
				vm_object_deallocate(src_object);
				*_result_object = VM_OBJECT_NULL;
				return(error_code ? error_code:
				       KERN_MEMORY_ERROR);

			default:
				panic("vm_object_copy_slowly: unexpected error"
				      " 0x%x from vm_fault_page()\n", result);
			}
		} while (result != VM_FAULT_SUCCESS);
	}

	/*
	 *	Lose the extra reference, and return our object.
	 */
	vm_object_deallocate(src_object);
	*_result_object = new_object;
	return(KERN_SUCCESS);
}

/*
 *	Routine:	vm_object_copy_quickly
 *
 *	Purpose:
 *		Copy the specified range of the source virtual
 *		memory object, if it can be done without waiting
 *		for user-generated events.
 *
 *	Results:
 *		If the copy is successful, the copy is returned in
 *		the arguments; otherwise, the arguments are not
 *		affected.
 *
 *	In/out conditions:
 *		The object should be unlocked on entry and exit.
 */

/*ARGSUSED*/
__private_extern__ boolean_t
vm_object_copy_quickly(
	vm_object_t		*_object,		/* INOUT */
	__unused vm_object_offset_t	offset,	/* IN */
	__unused vm_object_size_t	size,	/* IN */
	boolean_t		*_src_needs_copy,	/* OUT */
	boolean_t		*_dst_needs_copy)	/* OUT */
{
	vm_object_t	object = *_object;
	memory_object_copy_strategy_t copy_strategy;

	XPR(XPR_VM_OBJECT, "v_o_c_quickly obj 0x%x off 0x%x size 0x%x\n",
	    *_object, offset, size, 0, 0);
	if (object == VM_OBJECT_NULL) {
		*_src_needs_copy = FALSE;
		*_dst_needs_copy = FALSE;
		return(TRUE);
	}

	vm_object_lock(object);

	copy_strategy = object->copy_strategy;

	switch (copy_strategy) {
	case MEMORY_OBJECT_COPY_SYMMETRIC:

		/*
		 *	Symmetric copy strategy.
		 *	Make another reference to the object.
		 *	Leave object/offset unchanged.
		 */

		vm_object_reference_locked(object);
		object->shadowed = TRUE;
		vm_object_unlock(object);

		/*
		 *	Both source and destination must make
		 *	shadows, and the source must be made
		 *	read-only if not already.
		 */

		*_src_needs_copy = TRUE;
		*_dst_needs_copy = TRUE;

		break;

	case MEMORY_OBJECT_COPY_DELAY:
		vm_object_unlock(object);
		return(FALSE);

	default:
		vm_object_unlock(object);
		return(FALSE);
	}
	return(TRUE);
}

static int copy_call_count = 0;
static int copy_call_sleep_count = 0;
static int copy_call_restart_count = 0;

/*
 *	Routine:	vm_object_copy_call [internal]
 *
 *	Description:
 *		Copy the source object (src_object), using the
 *		user-managed copy algorithm.
 *
 *	In/out conditions:
 *		The source object must be locked on entry.  It
 *		will be *unlocked* on exit.
 *
 *	Results:
 *		If the copy is successful, KERN_SUCCESS is returned.
 *		A new object that represents the copied virtual
 *		memory is returned in a parameter (*_result_object).
 *		If the return value indicates an error, this parameter
 *		is not valid.
 */
static kern_return_t
vm_object_copy_call(
	vm_object_t		src_object,
	vm_object_offset_t	src_offset,
	vm_object_size_t	size,
	vm_object_t		*_result_object)	/* OUT */
{
	kern_return_t	kr;
	vm_object_t	copy;
	boolean_t	check_ready = FALSE;
	uint32_t	try_failed_count = 0;

	/*
	 *	If a copy is already in progress, wait and retry.
	 *
	 *	XXX
	 *	Consider making this call interruptable, as Mike
	 *	intended it to be.
	 *
	 *	XXXO
	 *	Need a counter or version or something to allow
	 *	us to use the copy that the currently requesting
	 *	thread is obtaining -- is it worth adding to the
	 *	vm object structure? Depends how common this case it.
	 */
	copy_call_count++;
	while (vm_object_wanted(src_object, VM_OBJECT_EVENT_COPY_CALL)) {
		vm_object_sleep(src_object, VM_OBJECT_EVENT_COPY_CALL,
			       THREAD_UNINT);
		copy_call_restart_count++;
	}

	/*
	 *	Indicate (for the benefit of memory_object_create_copy)
	 *	that we want a copy for src_object. (Note that we cannot
	 *	do a real assert_wait before calling memory_object_copy,
	 *	so we simply set the flag.)
	 */

	vm_object_set_wanted(src_object, VM_OBJECT_EVENT_COPY_CALL);
	vm_object_unlock(src_object);

	/*
	 *	Ask the memory manager to give us a memory object
	 *	which represents a copy of the src object.
	 *	The memory manager may give us a memory object
	 *	which we already have, or it may give us a
	 *	new memory object. This memory object will arrive
	 *	via memory_object_create_copy.
	 */

	kr = KERN_FAILURE;	/* XXX need to change memory_object.defs */
	if (kr != KERN_SUCCESS) {
		return kr;
	}

	/*
	 *	Wait for the copy to arrive.
	 */
	vm_object_lock(src_object);
	while (vm_object_wanted(src_object, VM_OBJECT_EVENT_COPY_CALL)) {
		vm_object_sleep(src_object, VM_OBJECT_EVENT_COPY_CALL,
			       THREAD_UNINT);
		copy_call_sleep_count++;
	}
Retry:
	assert(src_object->copy != VM_OBJECT_NULL);
	copy = src_object->copy;
	if (!vm_object_lock_try(copy)) {
		vm_object_unlock(src_object);

		try_failed_count++;
		mutex_pause(try_failed_count);	/* wait a bit */

		vm_object_lock(src_object);
		goto Retry;
	}
	if (copy->size < src_offset+size)
		copy->size = src_offset+size;

	if (!copy->pager_ready)
		check_ready = TRUE;

	/*
	 *	Return the copy.
	 */
	*_result_object = copy;
	vm_object_unlock(copy);
	vm_object_unlock(src_object);

	/* Wait for the copy to be ready. */
	if (check_ready == TRUE) {
		vm_object_lock(copy);
		while (!copy->pager_ready) {
			vm_object_sleep(copy, VM_OBJECT_EVENT_PAGER_READY, THREAD_UNINT);
		}
		vm_object_unlock(copy);
	}

	return KERN_SUCCESS;
}

static int copy_delayed_lock_collisions = 0;
static int copy_delayed_max_collisions = 0;
static int copy_delayed_lock_contention = 0;
static int copy_delayed_protect_iterate = 0;

/*
 *	Routine:	vm_object_copy_delayed [internal]
 *
 *	Description:
 *		Copy the specified virtual memory object, using
 *		the asymmetric copy-on-write algorithm.
 *
 *	In/out conditions:
 *		The src_object must be locked on entry.  It will be unlocked
 *		on exit - so the caller must also hold a reference to it.
 *
 *		This routine will not block waiting for user-generated
 *		events.  It is not interruptible.
 */
__private_extern__ vm_object_t
vm_object_copy_delayed(
	vm_object_t		src_object,
	vm_object_offset_t	src_offset,
	vm_object_size_t	size,
	boolean_t		src_object_shared)
{
	vm_object_t		new_copy = VM_OBJECT_NULL;
	vm_object_t		old_copy;
	vm_page_t		p;
	vm_object_size_t	copy_size = src_offset + size;


	int collisions = 0;
	/*
	 *	The user-level memory manager wants to see all of the changes
	 *	to this object, but it has promised not to make any changes on
 	 *	its own.
	 *
	 *	Perform an asymmetric copy-on-write, as follows:
	 *		Create a new object, called a "copy object" to hold
	 *		 pages modified by the new mapping  (i.e., the copy,
	 *		 not the original mapping).
	 *		Record the original object as the backing object for
	 *		 the copy object.  If the original mapping does not
	 *		 change a page, it may be used read-only by the copy.
	 *		Record the copy object in the original object.
	 *		 When the original mapping causes a page to be modified,
	 *		 it must be copied to a new page that is "pushed" to
	 *		 the copy object.
	 *		Mark the new mapping (the copy object) copy-on-write.
	 *		 This makes the copy object itself read-only, allowing
	 *		 it to be reused if the original mapping makes no
	 *		 changes, and simplifying the synchronization required
	 *		 in the "push" operation described above.
	 *
	 *	The copy-on-write is said to be assymetric because the original
	 *	object is *not* marked copy-on-write. A copied page is pushed
	 *	to the copy object, regardless which party attempted to modify
	 *	the page.
	 *
	 *	Repeated asymmetric copy operations may be done. If the
	 *	original object has not been changed since the last copy, its
	 *	copy object can be reused. Otherwise, a new copy object can be
	 *	inserted between the original object and its previous copy
	 *	object.  Since any copy object is read-only, this cannot affect
	 *	affect the contents of the previous copy object.
	 *
	 *	Note that a copy object is higher in the object tree than the
	 *	original object; therefore, use of the copy object recorded in
	 *	the original object must be done carefully, to avoid deadlock.
	 */

 Retry:
 
	/*
	 * Wait for paging in progress.
	 */
	if (!src_object->true_share &&
	    (src_object->paging_in_progress != 0 ||
	     src_object->activity_in_progress != 0)) {
	        if (src_object_shared == TRUE) {
		        vm_object_unlock(src_object);
			vm_object_lock(src_object);
			src_object_shared = FALSE;
			goto Retry;
		}
		vm_object_paging_wait(src_object, THREAD_UNINT);
	}
	/*
	 *	See whether we can reuse the result of a previous
	 *	copy operation.
	 */

	old_copy = src_object->copy;
	if (old_copy != VM_OBJECT_NULL) {
	        int lock_granted;

		/*
		 *	Try to get the locks (out of order)
		 */
		if (src_object_shared == TRUE)
		        lock_granted = vm_object_lock_try_shared(old_copy);
		else
		        lock_granted = vm_object_lock_try(old_copy);

		if (!lock_granted) {
			vm_object_unlock(src_object);

			if (collisions++ == 0)
				copy_delayed_lock_contention++;
			mutex_pause(collisions);

			/* Heisenberg Rules */
			copy_delayed_lock_collisions++;

			if (collisions > copy_delayed_max_collisions)
				copy_delayed_max_collisions = collisions;

			if (src_object_shared == TRUE)
			        vm_object_lock_shared(src_object);
			else
			        vm_object_lock(src_object);

			goto Retry;
		}

		/*
		 *	Determine whether the old copy object has
		 *	been modified.
		 */

		if (old_copy->resident_page_count == 0 &&
		    !old_copy->pager_created) {
			/*
			 *	It has not been modified.
			 *
			 *	Return another reference to
			 *	the existing copy-object if
			 *	we can safely grow it (if
			 *	needed).
			 */

			if (old_copy->size < copy_size) {
			        if (src_object_shared == TRUE) {
				        vm_object_unlock(old_copy);
					vm_object_unlock(src_object);
				
					vm_object_lock(src_object);
					src_object_shared = FALSE;
					goto Retry;
				}
				/*
				 * We can't perform a delayed copy if any of the
				 * pages in the extended range are wired (because
				 * we can't safely take write permission away from
				 * wired pages).  If the pages aren't wired, then
				 * go ahead and protect them.
				 */
				copy_delayed_protect_iterate++;

				queue_iterate(&src_object->memq, p, vm_page_t, listq) {
					if (!p->fictitious && 
					    p->offset >= old_copy->size && 
					    p->offset < copy_size) {
						if (VM_PAGE_WIRED(p)) {
							vm_object_unlock(old_copy);
							vm_object_unlock(src_object);

							if (new_copy != VM_OBJECT_NULL) {
								vm_object_unlock(new_copy);
								vm_object_deallocate(new_copy);
							}

							return VM_OBJECT_NULL;
						} else {
							pmap_page_protect(p->phys_page, 
									  (VM_PROT_ALL & ~VM_PROT_WRITE));
						}
					}
				}
				old_copy->size = copy_size;
			}
			if (src_object_shared == TRUE)
			        vm_object_reference_shared(old_copy);
			else
			        vm_object_reference_locked(old_copy);
			vm_object_unlock(old_copy);
			vm_object_unlock(src_object);

			if (new_copy != VM_OBJECT_NULL) {
				vm_object_unlock(new_copy);
				vm_object_deallocate(new_copy);
			}
			return(old_copy);
		}
		
		

		/*
		 * Adjust the size argument so that the newly-created 
		 * copy object will be large enough to back either the
		 * old copy object or the new mapping.
		 */
		if (old_copy->size > copy_size)
			copy_size = old_copy->size;

		if (new_copy == VM_OBJECT_NULL) {
			vm_object_unlock(old_copy);
			vm_object_unlock(src_object);
			new_copy = vm_object_allocate(copy_size);
			vm_object_lock(src_object);
			vm_object_lock(new_copy);

			src_object_shared = FALSE;
			goto Retry;
		}
		new_copy->size = copy_size;	

		/*
		 *	The copy-object is always made large enough to
		 *	completely shadow the original object, since
		 *	it may have several users who want to shadow
		 *	the original object at different points.
		 */

		assert((old_copy->shadow == src_object) &&
		    (old_copy->shadow_offset == (vm_object_offset_t) 0));

	} else if (new_copy == VM_OBJECT_NULL) {
		vm_object_unlock(src_object);
		new_copy = vm_object_allocate(copy_size);
		vm_object_lock(src_object);
		vm_object_lock(new_copy);

		src_object_shared = FALSE;
		goto Retry;
	}

	/*
	 * We now have the src object locked, and the new copy object
	 * allocated and locked (and potentially the old copy locked).
	 * Before we go any further, make sure we can still perform
	 * a delayed copy, as the situation may have changed.
	 *
	 * Specifically, we can't perform a delayed copy if any of the
	 * pages in the range are wired (because we can't safely take
	 * write permission away from wired pages).  If the pages aren't
	 * wired, then go ahead and protect them.
	 */
	copy_delayed_protect_iterate++;

	queue_iterate(&src_object->memq, p, vm_page_t, listq) {
		if (!p->fictitious && p->offset < copy_size) {
			if (VM_PAGE_WIRED(p)) {
				if (old_copy)
					vm_object_unlock(old_copy);
				vm_object_unlock(src_object);
				vm_object_unlock(new_copy);
				vm_object_deallocate(new_copy);
				return VM_OBJECT_NULL;
			} else {
				pmap_page_protect(p->phys_page, 
						  (VM_PROT_ALL & ~VM_PROT_WRITE));
			}
		}
	}
	if (old_copy != VM_OBJECT_NULL) {
		/*
		 *	Make the old copy-object shadow the new one.
		 *	It will receive no more pages from the original
		 *	object.
		 */

		/* remove ref. from old_copy */
		vm_object_lock_assert_exclusive(src_object);
		src_object->ref_count--;
		assert(src_object->ref_count > 0);
		vm_object_lock_assert_exclusive(old_copy);
		old_copy->shadow = new_copy;
		vm_object_lock_assert_exclusive(new_copy);
		assert(new_copy->ref_count > 0);
		new_copy->ref_count++;		/* for old_copy->shadow ref. */

#if TASK_SWAPPER
		if (old_copy->res_count) {
			VM_OBJ_RES_INCR(new_copy);
			VM_OBJ_RES_DECR(src_object);
		}
#endif

		vm_object_unlock(old_copy);	/* done with old_copy */
	}

	/*
	 *	Point the new copy at the existing object.
	 */
	vm_object_lock_assert_exclusive(new_copy);
	new_copy->shadow = src_object;
	new_copy->shadow_offset = 0;
	new_copy->shadowed = TRUE;	/* caller must set needs_copy */

	vm_object_lock_assert_exclusive(src_object);
	vm_object_reference_locked(src_object);
	src_object->copy = new_copy;
	vm_object_unlock(src_object);
	vm_object_unlock(new_copy);

	XPR(XPR_VM_OBJECT,
		"vm_object_copy_delayed: used copy object %X for source %X\n",
		new_copy, src_object, 0, 0, 0);

	return new_copy;
}

/*
 *	Routine:	vm_object_copy_strategically
 *
 *	Purpose:
 *		Perform a copy according to the source object's
 *		declared strategy.  This operation may block,
 *		and may be interrupted.
 */
__private_extern__ kern_return_t
vm_object_copy_strategically(
	register vm_object_t	src_object,
	vm_object_offset_t	src_offset,
	vm_object_size_t	size,
	vm_object_t		*dst_object,	/* OUT */
	vm_object_offset_t	*dst_offset,	/* OUT */
	boolean_t		*dst_needs_copy) /* OUT */
{
	boolean_t	result;
	boolean_t	interruptible = THREAD_ABORTSAFE; /* XXX */
	boolean_t	object_lock_shared = FALSE;
	memory_object_copy_strategy_t copy_strategy;

	assert(src_object != VM_OBJECT_NULL);

	copy_strategy = src_object->copy_strategy;

	if (copy_strategy == MEMORY_OBJECT_COPY_DELAY) {
	        vm_object_lock_shared(src_object);
		object_lock_shared = TRUE;
	} else
	        vm_object_lock(src_object);

	/*
	 *	The copy strategy is only valid if the memory manager
	 *	is "ready". Internal objects are always ready.
	 */

	while (!src_object->internal && !src_object->pager_ready) {
		wait_result_t wait_result;

		if (object_lock_shared == TRUE) {
		        vm_object_unlock(src_object);
			vm_object_lock(src_object);
			object_lock_shared = FALSE;
			continue;
		}
		wait_result = vm_object_sleep(	src_object,
						VM_OBJECT_EVENT_PAGER_READY,
						interruptible);
		if (wait_result != THREAD_AWAKENED) {
			vm_object_unlock(src_object);
			*dst_object = VM_OBJECT_NULL;
			*dst_offset = 0;
			*dst_needs_copy = FALSE;
			return(MACH_SEND_INTERRUPTED);
		}
	}

	/*
	 *	Use the appropriate copy strategy.
	 */

	switch (copy_strategy) {
	    case MEMORY_OBJECT_COPY_DELAY:
		*dst_object = vm_object_copy_delayed(src_object,
						     src_offset, size, object_lock_shared);
		if (*dst_object != VM_OBJECT_NULL) {
			*dst_offset = src_offset;
			*dst_needs_copy = TRUE;
			result = KERN_SUCCESS;
			break;
		}
		vm_object_lock(src_object);
		/* fall thru when delayed copy not allowed */

	    case MEMORY_OBJECT_COPY_NONE:
		result = vm_object_copy_slowly(src_object, src_offset, size,
					       interruptible, dst_object);
		if (result == KERN_SUCCESS) {
			*dst_offset = 0;
			*dst_needs_copy = FALSE;
		}
		break;

	    case MEMORY_OBJECT_COPY_CALL:
		result = vm_object_copy_call(src_object, src_offset, size,
				dst_object);
		if (result == KERN_SUCCESS) {
			*dst_offset = src_offset;
			*dst_needs_copy = TRUE;
		}
		break;

	    case MEMORY_OBJECT_COPY_SYMMETRIC:
		XPR(XPR_VM_OBJECT, "v_o_c_strategically obj 0x%x off 0x%x size 0x%x\n", src_object, src_offset, size, 0, 0);
		vm_object_unlock(src_object);
		result = KERN_MEMORY_RESTART_COPY;
		break;

	    default:
		panic("copy_strategically: bad strategy");
		result = KERN_INVALID_ARGUMENT;
	}
	return(result);
}

/*
 *	vm_object_shadow:
 *
 *	Create a new object which is backed by the
 *	specified existing object range.  The source
 *	object reference is deallocated.
 *
 *	The new object and offset into that object
 *	are returned in the source parameters.
 */
boolean_t vm_object_shadow_check = FALSE;

__private_extern__ boolean_t
vm_object_shadow(
	vm_object_t		*object,	/* IN/OUT */
	vm_object_offset_t	*offset,	/* IN/OUT */
	vm_object_size_t	length)
{
	register vm_object_t	source;
	register vm_object_t	result;

	source = *object;
#if 0
	/*
	 * XXX FBDP
	 * This assertion is valid but it gets triggered by Rosetta for example
	 * due to a combination of vm_remap() that changes a VM object's
	 * copy_strategy from SYMMETRIC to DELAY and vm_protect(VM_PROT_COPY)
	 * that then sets "needs_copy" on its map entry.  This creates a
	 * mapping situation that VM should never see and doesn't know how to
	 * handle.
	 * It's not clear if this can create any real problem but we should
	 * look into fixing this, probably by having vm_protect(VM_PROT_COPY)
	 * do more than just set "needs_copy" to handle the copy-on-write...
	 * In the meantime, let's disable the assertion.
	 */
	assert(source->copy_strategy == MEMORY_OBJECT_COPY_SYMMETRIC);
#endif

	/*
	 *	Determine if we really need a shadow.
	 */

	if (vm_object_shadow_check && source->ref_count == 1 &&
	    (source->shadow == VM_OBJECT_NULL ||
	     source->shadow->copy == VM_OBJECT_NULL))
	{
		source->shadowed = FALSE;
		return FALSE;
	}

	/*
	 *	Allocate a new object with the given length
	 */

	if ((result = vm_object_allocate(length)) == VM_OBJECT_NULL)
		panic("vm_object_shadow: no object for shadowing");

	/*
	 *	The new object shadows the source object, adding
	 *	a reference to it.  Our caller changes his reference
	 *	to point to the new object, removing a reference to
	 *	the source object.  Net result: no change of reference
	 *	count.
	 */
	result->shadow = source;
	
	/*
	 *	Store the offset into the source object,
	 *	and fix up the offset into the new object.
	 */

	result->shadow_offset = *offset;

	/*
	 *	Return the new things
	 */

	*offset = 0;
	*object = result;
	return TRUE;
}

/*
 *	The relationship between vm_object structures and
 *	the memory_object requires careful synchronization.
 *
 *	All associations are created by memory_object_create_named
 *  for external pagers and vm_object_pager_create for internal
 *  objects as follows:
 *
 *		pager:	the memory_object itself, supplied by
 *			the user requesting a mapping (or the kernel,
 *			when initializing internal objects); the
 *			kernel simulates holding send rights by keeping
 *			a port reference;
 *
 *		pager_request:
 *			the memory object control port,
 *			created by the kernel; the kernel holds
 *			receive (and ownership) rights to this
 *			port, but no other references.
 *
 *	When initialization is complete, the "initialized" field
 *	is asserted.  Other mappings using a particular memory object,
 *	and any references to the vm_object gained through the
 *	port association must wait for this initialization to occur.
 *
 *	In order to allow the memory manager to set attributes before
 *	requests (notably virtual copy operations, but also data or
 *	unlock requests) are made, a "ready" attribute is made available.
 *	Only the memory manager may affect the value of this attribute.
 *	Its value does not affect critical kernel functions, such as
 *	internal object initialization or destruction.  [Furthermore,
 *	memory objects created by the kernel are assumed to be ready
 *	immediately; the default memory manager need not explicitly
 *	set the "ready" attribute.]
 *
 *	[Both the "initialized" and "ready" attribute wait conditions
 *	use the "pager" field as the wait event.]
 *
 *	The port associations can be broken down by any of the
 *	following routines:
 *		vm_object_terminate:
 *			No references to the vm_object remain, and
 *			the object cannot (or will not) be cached.
 *			This is the normal case, and is done even
 *			though one of the other cases has already been
 *			done.
 *		memory_object_destroy:
 *			The memory manager has requested that the
 *			kernel relinquish references to the memory
 *			object. [The memory manager may not want to
 *			destroy the memory object, but may wish to
 *			refuse or tear down existing memory mappings.]
 *
 *	Each routine that breaks an association must break all of
 *	them at once.  At some later time, that routine must clear
 *	the pager field and release the memory object references.
 *	[Furthermore, each routine must cope with the simultaneous
 *	or previous operations of the others.]
 *
 *	In addition to the lock on the object, the vm_object_hash_lock
 *	governs the associations.  References gained through the
 *	association require use of the hash lock.
 *
 *	Because the pager field may be cleared spontaneously, it
 *	cannot be used to determine whether a memory object has
 *	ever been associated with a particular vm_object.  [This
 *	knowledge is important to the shadow object mechanism.]
 *	For this reason, an additional "created" attribute is
 *	provided.
 *
 *	During various paging operations, the pager reference found in the
 *	vm_object must be valid.  To prevent this from being released,
 *	(other than being removed, i.e., made null), routines may use
 *	the vm_object_paging_begin/end routines [actually, macros].
 *	The implementation uses the "paging_in_progress" and "wanted" fields.
 *	[Operations that alter the validity of the pager values include the
 *	termination routines and vm_object_collapse.]
 */


/*
 *	Routine:	vm_object_enter
 *	Purpose:
 *		Find a VM object corresponding to the given
 *		pager; if no such object exists, create one,
 *		and initialize the pager.
 */
vm_object_t
vm_object_enter(
	memory_object_t		pager,
	vm_object_size_t	size,
	boolean_t		internal,
	boolean_t		init,
	boolean_t		named)
{
	register vm_object_t	object;
	vm_object_t		new_object;
	boolean_t		must_init;
	vm_object_hash_entry_t	entry, new_entry;
	uint32_t        try_failed_count = 0;
	lck_mtx_t	*lck;

	if (pager == MEMORY_OBJECT_NULL)
		return(vm_object_allocate(size));

	new_object = VM_OBJECT_NULL;
	new_entry = VM_OBJECT_HASH_ENTRY_NULL;
	must_init = init;

	/*
	 *	Look for an object associated with this port.
	 */
Retry:
	lck = vm_object_hash_lock_spin(pager);
	do {
		entry = vm_object_hash_lookup(pager, FALSE);

		if (entry == VM_OBJECT_HASH_ENTRY_NULL) {
			if (new_object == VM_OBJECT_NULL) {
				/*
				 *	We must unlock to create a new object;
				 *	if we do so, we must try the lookup again.
				 */
				vm_object_hash_unlock(lck);
				assert(new_entry == VM_OBJECT_HASH_ENTRY_NULL);
				new_entry = vm_object_hash_entry_alloc(pager);
				new_object = vm_object_allocate(size);
				lck = vm_object_hash_lock_spin(pager);
			} else {
				/*
				 *	Lookup failed twice, and we have something
				 *	to insert; set the object.
				 */
				vm_object_hash_insert(new_entry, new_object);
				entry = new_entry;
				new_entry = VM_OBJECT_HASH_ENTRY_NULL;
				new_object = VM_OBJECT_NULL;
				must_init = TRUE;
			}
		} else if (entry->object == VM_OBJECT_NULL) {
			/*
		 	 *	If a previous object is being terminated,
			 *	we must wait for the termination message
			 *	to be queued (and lookup the entry again).
			 */
			entry->waiting = TRUE;
			entry = VM_OBJECT_HASH_ENTRY_NULL;
			assert_wait((event_t) pager, THREAD_UNINT);
			vm_object_hash_unlock(lck);

			thread_block(THREAD_CONTINUE_NULL);
			lck = vm_object_hash_lock_spin(pager);
		}
	} while (entry == VM_OBJECT_HASH_ENTRY_NULL);

	object = entry->object;
	assert(object != VM_OBJECT_NULL);

	if (!must_init) {
	        if ( !vm_object_lock_try(object)) {

		        vm_object_hash_unlock(lck);

		        try_failed_count++;
			mutex_pause(try_failed_count);  /* wait a bit */
			goto Retry;
		}
		assert(!internal || object->internal);
#if VM_OBJECT_CACHE
		if (object->ref_count == 0) {
			if ( !vm_object_cache_lock_try()) {

				vm_object_hash_unlock(lck);
				vm_object_unlock(object);

				try_failed_count++;
				mutex_pause(try_failed_count);  /* wait a bit */
				goto Retry;
			}
			XPR(XPR_VM_OBJECT_CACHE,
			    "vm_object_enter: removing %x from cache, head (%x, %x)\n",
				object,
				vm_object_cached_list.next,
				vm_object_cached_list.prev, 0,0);
			queue_remove(&vm_object_cached_list, object,
				     vm_object_t, cached_list);
			vm_object_cached_count--;

			vm_object_cache_unlock();
		}
#endif
		if (named) {
			assert(!object->named);
			object->named = TRUE;
		}
		vm_object_lock_assert_exclusive(object);
		object->ref_count++;
		vm_object_res_reference(object);

		vm_object_hash_unlock(lck);
		vm_object_unlock(object);

		VM_STAT_INCR(hits);
	} else
		vm_object_hash_unlock(lck);

	assert(object->ref_count > 0);

	VM_STAT_INCR(lookups);

	XPR(XPR_VM_OBJECT,
		"vm_o_enter: pager 0x%x obj 0x%x must_init %d\n",
		pager, object, must_init, 0, 0);

	/*
	 *	If we raced to create a vm_object but lost, let's
	 *	throw away ours.
	 */

	if (new_object != VM_OBJECT_NULL)
		vm_object_deallocate(new_object);

	if (new_entry != VM_OBJECT_HASH_ENTRY_NULL)
		vm_object_hash_entry_free(new_entry);

	if (must_init) {
		memory_object_control_t control;

		/*
		 *	Allocate request port.
		 */

		control = memory_object_control_allocate(object);
		assert (control != MEMORY_OBJECT_CONTROL_NULL);

		vm_object_lock(object);
		assert(object != kernel_object);

		/*
		 *	Copy the reference we were given.
		 */

		memory_object_reference(pager);
		object->pager_created = TRUE;
		object->pager = pager;
		object->internal = internal;
		object->pager_trusted = internal;
		if (!internal) {
			/* copy strategy invalid until set by memory manager */
			object->copy_strategy = MEMORY_OBJECT_COPY_INVALID;
		}
		object->pager_control = control;
		object->pager_ready = FALSE;

		vm_object_unlock(object);

		/*
		 *	Let the pager know we're using it.
		 */

		(void) memory_object_init(pager,
			object->pager_control,
			PAGE_SIZE);

		vm_object_lock(object);
		if (named)
			object->named = TRUE;
		if (internal) {
			object->pager_ready = TRUE;
			vm_object_wakeup(object, VM_OBJECT_EVENT_PAGER_READY);
		}

		object->pager_initialized = TRUE;
		vm_object_wakeup(object, VM_OBJECT_EVENT_INITIALIZED);
	} else {
		vm_object_lock(object);
	}

	/*
	 *	[At this point, the object must be locked]
	 */

	/*
	 *	Wait for the work above to be done by the first
	 *	thread to map this object.
	 */

	while (!object->pager_initialized) {
		vm_object_sleep(object,
				VM_OBJECT_EVENT_INITIALIZED,
				THREAD_UNINT);
	}
	vm_object_unlock(object);

	XPR(XPR_VM_OBJECT,
	    "vm_object_enter: vm_object %x, memory_object %x, internal %d\n",
	    object, object->pager, internal, 0,0);
	return(object);
}

/*
 *	Routine:	vm_object_pager_create
 *	Purpose:
 *		Create a memory object for an internal object.
 *	In/out conditions:
 *		The object is locked on entry and exit;
 *		it may be unlocked within this call.
 *	Limitations:
 *		Only one thread may be performing a
 *		vm_object_pager_create on an object at
 *		a time.  Presumably, only the pageout
 *		daemon will be using this routine.
 */

void
vm_object_pager_create(
	register vm_object_t	object)
{
	memory_object_t		pager;
	vm_object_hash_entry_t	entry;
	lck_mtx_t		*lck;
#if	MACH_PAGEMAP
	vm_object_size_t	size;
	vm_external_map_t	map;
#endif	/* MACH_PAGEMAP */

	XPR(XPR_VM_OBJECT, "vm_object_pager_create, object 0x%X\n",
		object, 0,0,0,0);

	assert(object != kernel_object);

	if (memory_manager_default_check() != KERN_SUCCESS)
		return;

	/*
	 *	Prevent collapse or termination by holding a paging reference
	 */

	vm_object_paging_begin(object);
	if (object->pager_created) {
		/*
		 *	Someone else got to it first...
		 *	wait for them to finish initializing the ports
		 */
		while (!object->pager_initialized) {
			vm_object_sleep(object,
				        VM_OBJECT_EVENT_INITIALIZED,
				        THREAD_UNINT);
		}
		vm_object_paging_end(object);
		return;
	}

	/*
	 *	Indicate that a memory object has been assigned
	 *	before dropping the lock, to prevent a race.
	 */

	object->pager_created = TRUE;
	object->paging_offset = 0;
		
#if	MACH_PAGEMAP
	size = object->size;
#endif	/* MACH_PAGEMAP */
	vm_object_unlock(object);

#if	MACH_PAGEMAP
	map = vm_external_create(size);
	vm_object_lock(object);
	assert(object->size == size);
	object->existence_map = map;
	vm_object_unlock(object);
#endif	/* MACH_PAGEMAP */

	if ((uint32_t) object->size != object->size) {
		panic("vm_object_pager_create(): object size 0x%llx >= 4GB\n",
		      (uint64_t) object->size);
	}

	/*
	 *	Create the [internal] pager, and associate it with this object.
	 *
	 *	We make the association here so that vm_object_enter()
	 * 	can look up the object to complete initializing it.  No
	 *	user will ever map this object.
	 */
	{
		memory_object_default_t		dmm;

		/* acquire a reference for the default memory manager */
		dmm = memory_manager_default_reference();

		assert(object->temporary);

		/* create our new memory object */
		assert((vm_size_t) object->size == object->size);
		(void) memory_object_create(dmm, (vm_size_t) object->size,
					    &pager);

		memory_object_default_deallocate(dmm);
       }

	entry = vm_object_hash_entry_alloc(pager);

	lck = vm_object_hash_lock_spin(pager);
	vm_object_hash_insert(entry, object);
	vm_object_hash_unlock(lck);

	/*
	 *	A reference was returned by
	 *	memory_object_create(), and it is
	 *	copied by vm_object_enter().
	 */

	if (vm_object_enter(pager, object->size, TRUE, TRUE, FALSE) != object)
		panic("vm_object_pager_create: mismatch");

	/*
	 *	Drop the reference we were passed.
	 */
	memory_object_deallocate(pager);

	vm_object_lock(object);

	/*
	 *	Release the paging reference
	 */
	vm_object_paging_end(object);
}

/*
 *	Routine:	vm_object_remove
 *	Purpose:
 *		Eliminate the pager/object association
 *		for this pager.
 *	Conditions:
 *		The object cache must be locked.
 */
__private_extern__ void
vm_object_remove(
	vm_object_t	object)
{
	memory_object_t pager;

	if ((pager = object->pager) != MEMORY_OBJECT_NULL) {
		vm_object_hash_entry_t	entry;

		entry = vm_object_hash_lookup(pager, FALSE);
		if (entry != VM_OBJECT_HASH_ENTRY_NULL)
			entry->object = VM_OBJECT_NULL;
	}

}

/*
 *	Global variables for vm_object_collapse():
 *
 *		Counts for normal collapses and bypasses.
 *		Debugging variables, to watch or disable collapse.
 */
static long	object_collapses = 0;
static long	object_bypasses  = 0;

static boolean_t	vm_object_collapse_allowed = TRUE;
static boolean_t	vm_object_bypass_allowed = TRUE;

#if MACH_PAGEMAP
static int	vm_external_discarded;
static int	vm_external_collapsed;
#endif

unsigned long vm_object_collapse_encrypted = 0;

/*
 *	Routine:	vm_object_do_collapse
 *	Purpose:
 *		Collapse an object with the object backing it.
 *		Pages in the backing object are moved into the
 *		parent, and the backing object is deallocated.
 *	Conditions:
 *		Both objects and the cache are locked; the page
 *		queues are unlocked.
 *
 */
static void
vm_object_do_collapse(
	vm_object_t object,
	vm_object_t backing_object)
{
	vm_page_t p, pp;
	vm_object_offset_t new_offset, backing_offset;
	vm_object_size_t size;

	vm_object_lock_assert_exclusive(object);
	vm_object_lock_assert_exclusive(backing_object);

	backing_offset = object->shadow_offset;
	size = object->size;

	/*
	 *	Move all in-memory pages from backing_object
	 *	to the parent.  Pages that have been paged out
	 *	will be overwritten by any of the parent's
	 *	pages that shadow them.
	 */
	
	while (!queue_empty(&backing_object->memq)) {
		
		p = (vm_page_t) queue_first(&backing_object->memq);
		
		new_offset = (p->offset - backing_offset);
		
		assert(!p->busy || p->absent);

		/*
		 *	If the parent has a page here, or if
		 *	this page falls outside the parent,
		 *	dispose of it.
		 *
		 *	Otherwise, move it as planned.
		 */
		
		if (p->offset < backing_offset || new_offset >= size) {
			VM_PAGE_FREE(p);
		} else {
			/*
			 * ENCRYPTED SWAP:
			 * The encryption key includes the "pager" and the
			 * "paging_offset".  These will not change during the 
			 * object collapse, so we can just move an encrypted
			 * page from one object to the other in this case.
			 * We can't decrypt the page here, since we can't drop
			 * the object lock.
			 */
			if (p->encrypted) {
				vm_object_collapse_encrypted++;
			}
			pp = vm_page_lookup(object, new_offset);
			if (pp == VM_PAGE_NULL) {

				/*
				 *	Parent now has no page.
				 *	Move the backing object's page up.
				 */

				vm_page_rename(p, object, new_offset, TRUE);
#if	MACH_PAGEMAP
			} else if (pp->absent) {

				/*
				 *	Parent has an absent page...
				 *	it's not being paged in, so
				 *	it must really be missing from
				 *	the parent.
				 *
				 *	Throw out the absent page...
				 *	any faults looking for that
				 *	page will restart with the new
				 *	one.
				 */

				VM_PAGE_FREE(pp);
				vm_page_rename(p, object, new_offset, TRUE);
#endif	/* MACH_PAGEMAP */
			} else {
				assert(! pp->absent);

				/*
				 *	Parent object has a real page.
				 *	Throw away the backing object's
				 *	page.
				 */
				VM_PAGE_FREE(p);
			}
		}
	}
	
#if	!MACH_PAGEMAP
	assert((!object->pager_created && (object->pager == MEMORY_OBJECT_NULL))
		|| (!backing_object->pager_created
		&&  (backing_object->pager == MEMORY_OBJECT_NULL)));
#else 
        assert(!object->pager_created && object->pager == MEMORY_OBJECT_NULL);
#endif	/* !MACH_PAGEMAP */

	if (backing_object->pager != MEMORY_OBJECT_NULL) {
		vm_object_hash_entry_t	entry;

		/*
		 *	Move the pager from backing_object to object.
		 *
		 *	XXX We're only using part of the paging space
		 *	for keeps now... we ought to discard the
		 *	unused portion.
		 */

		assert(!object->paging_in_progress);
		assert(!object->activity_in_progress);
		object->pager = backing_object->pager;

		if (backing_object->hashed) {
			lck_mtx_t	*lck;

			lck = vm_object_hash_lock_spin(backing_object->pager);
			entry = vm_object_hash_lookup(object->pager, FALSE);
			assert(entry != VM_OBJECT_HASH_ENTRY_NULL);
			entry->object = object;
			vm_object_hash_unlock(lck);

			object->hashed = TRUE;
		}
		object->pager_created = backing_object->pager_created;
		object->pager_control = backing_object->pager_control;
		object->pager_ready = backing_object->pager_ready;
		object->pager_initialized = backing_object->pager_initialized;
		object->paging_offset =
		    backing_object->paging_offset + backing_offset;
		if (object->pager_control != MEMORY_OBJECT_CONTROL_NULL) {
			memory_object_control_collapse(object->pager_control,
						       object);
		}
	}

#if	MACH_PAGEMAP
	/*
	 *	If the shadow offset is 0, the use the existence map from
	 *	the backing object if there is one. If the shadow offset is
	 *	not zero, toss it.
	 *
	 *	XXX - If the shadow offset is not 0 then a bit copy is needed
	 *	if the map is to be salvaged.  For now, we just just toss the
	 *	old map, giving the collapsed object no map. This means that
	 *	the pager is invoked for zero fill pages.  If analysis shows
	 *	that this happens frequently and is a performance hit, then
	 *	this code should be fixed to salvage the map.
	 */
	assert(object->existence_map == VM_EXTERNAL_NULL);
	if (backing_offset || (size != backing_object->size)) {
		vm_external_discarded++;
		vm_external_destroy(backing_object->existence_map,
			backing_object->size);
	}
	else {
		vm_external_collapsed++;
		object->existence_map = backing_object->existence_map;
	}
	backing_object->existence_map = VM_EXTERNAL_NULL;
#endif	/* MACH_PAGEMAP */

	/*
	 *	Object now shadows whatever backing_object did.
	 *	Note that the reference to backing_object->shadow
	 *	moves from within backing_object to within object.
	 */
	
	assert(!object->phys_contiguous);
	assert(!backing_object->phys_contiguous);
	object->shadow = backing_object->shadow;
	if (object->shadow) {
		object->shadow_offset += backing_object->shadow_offset;
	} else {
		/* no shadow, therefore no shadow offset... */
		object->shadow_offset = 0;
	}
	assert((object->shadow == VM_OBJECT_NULL) ||
	       (object->shadow->copy != backing_object));

	/*
	 *	Discard backing_object.
	 *
	 *	Since the backing object has no pages, no
	 *	pager left, and no object references within it,
	 *	all that is necessary is to dispose of it.
	 */
	
	assert((backing_object->ref_count == 1) &&
	       (backing_object->resident_page_count == 0) &&
	       (backing_object->paging_in_progress == 0) &&
	       (backing_object->activity_in_progress == 0));

	backing_object->alive = FALSE;
	vm_object_unlock(backing_object);

	XPR(XPR_VM_OBJECT, "vm_object_collapse, collapsed 0x%X\n",
		backing_object, 0,0,0,0);

	vm_object_lock_destroy(backing_object);

	zfree(vm_object_zone, backing_object);
	
	object_collapses++;
}

static void
vm_object_do_bypass(
	vm_object_t object,
	vm_object_t backing_object)
{
	/*
	 *	Make the parent shadow the next object
	 *	in the chain.
	 */
	
	vm_object_lock_assert_exclusive(object);
	vm_object_lock_assert_exclusive(backing_object);

#if	TASK_SWAPPER
	/*
	 *	Do object reference in-line to 
	 *	conditionally increment shadow's
	 *	residence count.  If object is not
	 *	resident, leave residence count
	 *	on shadow alone.
	 */
	if (backing_object->shadow != VM_OBJECT_NULL) {
		vm_object_lock(backing_object->shadow);
		vm_object_lock_assert_exclusive(backing_object->shadow);
		backing_object->shadow->ref_count++;
		if (object->res_count != 0)
			vm_object_res_reference(backing_object->shadow);
		vm_object_unlock(backing_object->shadow);
	}
#else	/* TASK_SWAPPER */
	vm_object_reference(backing_object->shadow);
#endif	/* TASK_SWAPPER */

	assert(!object->phys_contiguous);
	assert(!backing_object->phys_contiguous);
	object->shadow = backing_object->shadow;
	if (object->shadow) {
		object->shadow_offset += backing_object->shadow_offset;
	} else {
		/* no shadow, therefore no shadow offset... */
		object->shadow_offset = 0;
	}
	
	/*
	 *	Backing object might have had a copy pointer
	 *	to us.  If it did, clear it. 
	 */
	if (backing_object->copy == object) {
		backing_object->copy = VM_OBJECT_NULL;
	}
	
	/*
	 *	Drop the reference count on backing_object.
#if	TASK_SWAPPER
	 *	Since its ref_count was at least 2, it
	 *	will not vanish; so we don't need to call
	 *	vm_object_deallocate.
	 *	[with a caveat for "named" objects]
	 * 
	 *	The res_count on the backing object is
	 *	conditionally decremented.  It's possible
	 *	(via vm_pageout_scan) to get here with
	 *	a "swapped" object, which has a 0 res_count,
	 *	in which case, the backing object res_count
	 *	is already down by one.
#else
	 *	Don't call vm_object_deallocate unless
	 *	ref_count drops to zero.
	 *
	 *	The ref_count can drop to zero here if the
	 *	backing object could be bypassed but not
	 *	collapsed, such as when the backing object
	 *	is temporary and cachable.
#endif
	 */
	if (backing_object->ref_count > 2 ||
	    (!backing_object->named && backing_object->ref_count > 1)) {
		vm_object_lock_assert_exclusive(backing_object);
		backing_object->ref_count--;
#if	TASK_SWAPPER
		if (object->res_count != 0)
			vm_object_res_deallocate(backing_object);
		assert(backing_object->ref_count > 0);
#endif	/* TASK_SWAPPER */
		vm_object_unlock(backing_object);
	} else {

		/*
		 *	Drop locks so that we can deallocate
		 *	the backing object.
		 */

#if	TASK_SWAPPER
		if (object->res_count == 0) {
			/* XXX get a reference for the deallocate below */
			vm_object_res_reference(backing_object);
		}
#endif	/* TASK_SWAPPER */
		vm_object_unlock(object);
		vm_object_unlock(backing_object);
		vm_object_deallocate(backing_object);

		/*
		 *	Relock object. We don't have to reverify
		 *	its state since vm_object_collapse will
		 *	do that for us as it starts at the
		 *	top of its loop.
		 */

		vm_object_lock(object);
	}
	
	object_bypasses++;
}

		
/*
 *	vm_object_collapse:
 *
 *	Perform an object collapse or an object bypass if appropriate.
 *	The real work of collapsing and bypassing is performed in
 *	the routines vm_object_do_collapse and vm_object_do_bypass.
 *
 *	Requires that the object be locked and the page queues be unlocked.
 *
 */
static unsigned long vm_object_collapse_calls = 0;
static unsigned long vm_object_collapse_objects = 0;
static unsigned long vm_object_collapse_do_collapse = 0;
static unsigned long vm_object_collapse_do_bypass = 0;
static unsigned long vm_object_collapse_delays = 0;
__private_extern__ void
vm_object_collapse(
	register vm_object_t			object,
	register vm_object_offset_t		hint_offset,
	boolean_t				can_bypass)
{
	register vm_object_t			backing_object;
	register unsigned int			rcount;
	register unsigned int			size;
	vm_object_t				original_object;
	int					object_lock_type;
	int					backing_object_lock_type;

	vm_object_collapse_calls++;

	if (! vm_object_collapse_allowed &&
	    ! (can_bypass && vm_object_bypass_allowed)) {
		return;
	}

	XPR(XPR_VM_OBJECT, "vm_object_collapse, obj 0x%X\n", 
		object, 0,0,0,0);

	if (object == VM_OBJECT_NULL)
		return;

	original_object = object;

	/*
	 * The top object was locked "exclusive" by the caller.
	 * In the first pass, to determine if we can collapse the shadow chain,
	 * take a "shared" lock on the shadow objects.  If we can collapse,
	 * we'll have to go down the chain again with exclusive locks.
	 */
	object_lock_type = OBJECT_LOCK_EXCLUSIVE;
	backing_object_lock_type = OBJECT_LOCK_SHARED;

retry:
	object = original_object;
	vm_object_lock_assert_exclusive(object);

	while (TRUE) {
		vm_object_collapse_objects++;
		/*
		 *	Verify that the conditions are right for either
		 *	collapse or bypass:
		 */

		/*
		 *	There is a backing object, and
		 */
	
		backing_object = object->shadow;
		if (backing_object == VM_OBJECT_NULL) {
			if (object != original_object) {
				vm_object_unlock(object);
			}
			return;
		}
		if (backing_object_lock_type == OBJECT_LOCK_SHARED) {
			vm_object_lock_shared(backing_object);
		} else {
			vm_object_lock(backing_object);
		}

		/*
		 *	No pages in the object are currently
		 *	being paged out, and
		 */
		if (object->paging_in_progress != 0 ||
		    object->activity_in_progress != 0) {
			/* try and collapse the rest of the shadow chain */
			if (object != original_object) {
				vm_object_unlock(object);
			}
			object = backing_object;
			object_lock_type = backing_object_lock_type;
			continue;
		}

		/*
		 *	...
		 *		The backing object is not read_only,
		 *		and no pages in the backing object are
		 *		currently being paged out.
		 *		The backing object is internal.
		 *
		 */
	
		if (!backing_object->internal ||
		    backing_object->paging_in_progress != 0 ||
		    backing_object->activity_in_progress != 0) {
			/* try and collapse the rest of the shadow chain */
			if (object != original_object) {
				vm_object_unlock(object);
			}
			object = backing_object;
			object_lock_type = backing_object_lock_type;
			continue;
		}
	
		/*
		 *	The backing object can't be a copy-object:
		 *	the shadow_offset for the copy-object must stay
		 *	as 0.  Furthermore (for the 'we have all the
		 *	pages' case), if we bypass backing_object and
		 *	just shadow the next object in the chain, old
		 *	pages from that object would then have to be copied
		 *	BOTH into the (former) backing_object and into the
		 *	parent object.
		 */
		if (backing_object->shadow != VM_OBJECT_NULL &&
		    backing_object->shadow->copy == backing_object) {
			/* try and collapse the rest of the shadow chain */
			if (object != original_object) {
				vm_object_unlock(object);
			}
			object = backing_object;
			object_lock_type = backing_object_lock_type;
			continue;
		}

		/*
		 *	We can now try to either collapse the backing
		 *	object (if the parent is the only reference to
		 *	it) or (perhaps) remove the parent's reference
		 *	to it.
		 *
		 *	If there is exactly one reference to the backing
		 *	object, we may be able to collapse it into the
		 *	parent.
		 *
		 *	If MACH_PAGEMAP is defined:
		 *	The parent must not have a pager created for it,
		 *	since collapsing a backing_object dumps new pages
		 *	into the parent that its pager doesn't know about
		 *	(and the collapse code can't merge the existence
		 *	maps).
		 *	Otherwise:
		 *	As long as one of the objects is still not known
		 *	to the pager, we can collapse them.
		 */
		if (backing_object->ref_count == 1 &&
		    (!object->pager_created 
#if	!MACH_PAGEMAP
		     || !backing_object->pager_created
#endif	/*!MACH_PAGEMAP */
		    ) && vm_object_collapse_allowed) {

			/*
			 * We need the exclusive lock on the VM objects.
			 */
			if (backing_object_lock_type != OBJECT_LOCK_EXCLUSIVE) {
				/*
				 * We have an object and its shadow locked 
				 * "shared".  We can't just upgrade the locks
				 * to "exclusive", as some other thread might
				 * also have these objects locked "shared" and
				 * attempt to upgrade one or the other to 
				 * "exclusive".  The upgrades would block
				 * forever waiting for the other "shared" locks
				 * to get released.
				 * So we have to release the locks and go
				 * down the shadow chain again (since it could
				 * have changed) with "exclusive" locking.
				 */
				vm_object_unlock(backing_object);
				if (object != original_object)
					vm_object_unlock(object);
				object_lock_type = OBJECT_LOCK_EXCLUSIVE;
				backing_object_lock_type = OBJECT_LOCK_EXCLUSIVE;
				goto retry;
			}

			XPR(XPR_VM_OBJECT, 
		   "vm_object_collapse: %x to %x, pager %x, pager_control %x\n",
				backing_object, object,
				backing_object->pager, 
				backing_object->pager_control, 0);

			/*
			 *	Collapse the object with its backing
			 *	object, and try again with the object's
			 *	new backing object.
			 */

			vm_object_do_collapse(object, backing_object);
			vm_object_collapse_do_collapse++;
			continue;
		}

		/*
		 *	Collapsing the backing object was not possible
		 *	or permitted, so let's try bypassing it.
		 */

		if (! (can_bypass && vm_object_bypass_allowed)) {
			/* try and collapse the rest of the shadow chain */
			if (object != original_object) {
				vm_object_unlock(object);
			}
			object = backing_object;
			object_lock_type = backing_object_lock_type;
			continue;
		}


		/*
		 *	If the object doesn't have all its pages present,
		 *	we have to make sure no pages in the backing object
		 *	"show through" before bypassing it.
		 */
		size = atop(object->size);
		rcount = object->resident_page_count;
		if (rcount != size) {
			vm_object_offset_t	offset;
			vm_object_offset_t	backing_offset;
			unsigned int     	backing_rcount;
			unsigned int		lookups = 0;

			/*
			 *	If the backing object has a pager but no pagemap,
			 *	then we cannot bypass it, because we don't know
			 *	what pages it has.
			 */
			if (backing_object->pager_created
#if	MACH_PAGEMAP
			    && (backing_object->existence_map == VM_EXTERNAL_NULL)
#endif	/* MACH_PAGEMAP */
				) {
				/* try and collapse the rest of the shadow chain */
				if (object != original_object) {
					vm_object_unlock(object);
				}
				object = backing_object;
				object_lock_type = backing_object_lock_type;
				continue;
			}

			/*
			 *	If the object has a pager but no pagemap,
			 *	then we cannot bypass it, because we don't know
			 *	what pages it has.
			 */
			if (object->pager_created
#if	MACH_PAGEMAP
			    && (object->existence_map == VM_EXTERNAL_NULL)
#endif	/* MACH_PAGEMAP */
				) {
				/* try and collapse the rest of the shadow chain */
				if (object != original_object) {
					vm_object_unlock(object);
				}
				object = backing_object;
				object_lock_type = backing_object_lock_type;
				continue;
			}

			/*
			 *	If all of the pages in the backing object are
			 *	shadowed by the parent object, the parent
			 *	object no longer has to shadow the backing
			 *	object; it can shadow the next one in the
			 *	chain.
			 *
			 *	If the backing object has existence info,
			 *	we must check examine its existence info
			 *	as well.
			 *
			 */

			backing_offset = object->shadow_offset;
			backing_rcount = backing_object->resident_page_count;

#if	MACH_PAGEMAP
#define EXISTS_IN_OBJECT(obj, off, rc) \
	(vm_external_state_get((obj)->existence_map, \
	 (vm_offset_t)(off)) == VM_EXTERNAL_STATE_EXISTS || \
	 ((rc) && ++lookups && vm_page_lookup((obj), (off)) != VM_PAGE_NULL && (rc)--))
#else
#define EXISTS_IN_OBJECT(obj, off, rc) \
	(((rc) && ++lookups && vm_page_lookup((obj), (off)) != VM_PAGE_NULL && (rc)--))
#endif	/* MACH_PAGEMAP */

			/*
			 * Check the hint location first
			 * (since it is often the quickest way out of here).
			 */
			if (object->cow_hint != ~(vm_offset_t)0)
				hint_offset = (vm_object_offset_t)object->cow_hint;
			else
				hint_offset = (hint_offset > 8 * PAGE_SIZE_64) ?
				              (hint_offset - 8 * PAGE_SIZE_64) : 0;

			if (EXISTS_IN_OBJECT(backing_object, hint_offset +
			                     backing_offset, backing_rcount) &&
			    !EXISTS_IN_OBJECT(object, hint_offset, rcount)) {
				/* dependency right at the hint */
				object->cow_hint = (vm_offset_t) hint_offset; /* atomic */
				/* try and collapse the rest of the shadow chain */
				if (object != original_object) {
					vm_object_unlock(object);
				}
				object = backing_object;
				object_lock_type = backing_object_lock_type;
				continue;
			}

			/*
			 * If the object's window onto the backing_object
			 * is large compared to the number of resident
			 * pages in the backing object, it makes sense to
			 * walk the backing_object's resident pages first.
			 *
			 * NOTE: Pages may be in both the existence map and 
			 * resident.  So, we can't permanently decrement
			 * the rcount here because the second loop may
			 * find the same pages in the backing object'
			 * existence map that we found here and we would
			 * double-decrement the rcount.  We also may or
			 * may not have found the 
			 */
			if (backing_rcount && 
#if	MACH_PAGEMAP
			    size > ((backing_object->existence_map) ?
			     backing_rcount : (backing_rcount >> 1))
#else
			    size > (backing_rcount >> 1)
#endif	/* MACH_PAGEMAP */
				) {
				unsigned int rc = rcount;
				vm_page_t p;

				backing_rcount = backing_object->resident_page_count;
				p = (vm_page_t)queue_first(&backing_object->memq);
				do {
					/* Until we get more than one lookup lock */
					if (lookups > 256) {
						vm_object_collapse_delays++;
						lookups = 0;
						mutex_pause(0);
					}

					offset = (p->offset - backing_offset);
					if (offset < object->size &&
					    offset != hint_offset &&
					    !EXISTS_IN_OBJECT(object, offset, rc)) {
						/* found a dependency */
						object->cow_hint = (vm_offset_t) offset; /* atomic */
						
						break;
					}
					p = (vm_page_t) queue_next(&p->listq);

				} while (--backing_rcount);
				if (backing_rcount != 0 ) {
					/* try and collapse the rest of the shadow chain */
					if (object != original_object) {
						vm_object_unlock(object);
					}
					object = backing_object;
					object_lock_type = backing_object_lock_type;
					continue;
				}
			}

			/*
			 * Walk through the offsets looking for pages in the
			 * backing object that show through to the object.
			 */
			if (backing_rcount
#if MACH_PAGEMAP
			    || backing_object->existence_map
#endif	/* MACH_PAGEMAP */
				) {
				offset = hint_offset;
				
				while((offset =
				      (offset + PAGE_SIZE_64 < object->size) ?
				      (offset + PAGE_SIZE_64) : 0) != hint_offset) {

					/* Until we get more than one lookup lock */
					if (lookups > 256) {
						vm_object_collapse_delays++;
						lookups = 0;
						mutex_pause(0);
					}

					if (EXISTS_IN_OBJECT(backing_object, offset +
				            backing_offset, backing_rcount) &&
					    !EXISTS_IN_OBJECT(object, offset, rcount)) {
						/* found a dependency */
						object->cow_hint = (vm_offset_t) offset; /* atomic */
						break;
					}
				}
				if (offset != hint_offset) {
					/* try and collapse the rest of the shadow chain */
					if (object != original_object) {
						vm_object_unlock(object);
					}
					object = backing_object;
					object_lock_type = backing_object_lock_type;
					continue;
				}
			}
		}

		/*
		 * We need "exclusive" locks on the 2 VM objects.
		 */
		if (backing_object_lock_type != OBJECT_LOCK_EXCLUSIVE) {
			vm_object_unlock(backing_object);
			if (object != original_object)
				vm_object_unlock(object);
			object_lock_type = OBJECT_LOCK_EXCLUSIVE;
			backing_object_lock_type = OBJECT_LOCK_EXCLUSIVE;
			goto retry;
		}

		/* reset the offset hint for any objects deeper in the chain */
		object->cow_hint = (vm_offset_t)0;

		/*
		 *	All interesting pages in the backing object
		 *	already live in the parent or its pager.
		 *	Thus we can bypass the backing object.
		 */

		vm_object_do_bypass(object, backing_object);
		vm_object_collapse_do_bypass++;

		/*
		 *	Try again with this object's new backing object.
		 */

		continue;
	}

	if (object != original_object) {
		vm_object_unlock(object);
	}
}

/*
 *	Routine:	vm_object_page_remove: [internal]
 *	Purpose:
 *		Removes all physical pages in the specified
 *		object range from the object's list of pages.
 *
 *	In/out conditions:
 *		The object must be locked.
 *		The object must not have paging_in_progress, usually
 *		guaranteed by not having a pager.
 */
unsigned int vm_object_page_remove_lookup = 0;
unsigned int vm_object_page_remove_iterate = 0;

__private_extern__ void
vm_object_page_remove(
	register vm_object_t		object,
	register vm_object_offset_t	start,
	register vm_object_offset_t	end)
{
	register vm_page_t	p, next;

	/*
	 *	One and two page removals are most popular.
	 *	The factor of 16 here is somewhat arbitrary.
	 *	It balances vm_object_lookup vs iteration.
	 */

	if (atop_64(end - start) < (unsigned)object->resident_page_count/16) {
		vm_object_page_remove_lookup++;

		for (; start < end; start += PAGE_SIZE_64) {
			p = vm_page_lookup(object, start);
			if (p != VM_PAGE_NULL) {
				assert(!p->cleaning && !p->pageout);
				if (!p->fictitious && p->pmapped)
				        pmap_disconnect(p->phys_page);
				VM_PAGE_FREE(p);
			}
		}
	} else {
		vm_object_page_remove_iterate++;

		p = (vm_page_t) queue_first(&object->memq);
		while (!queue_end(&object->memq, (queue_entry_t) p)) {
			next = (vm_page_t) queue_next(&p->listq);
			if ((start <= p->offset) && (p->offset < end)) {
				assert(!p->cleaning && !p->pageout);
				if (!p->fictitious && p->pmapped)
				        pmap_disconnect(p->phys_page);
				VM_PAGE_FREE(p);
			}
			p = next;
		}
	}
}


/*
 *	Routine:	vm_object_coalesce
 *	Function:	Coalesces two objects backing up adjoining
 *			regions of memory into a single object.
 *
 *	returns TRUE if objects were combined.
 *
 *	NOTE:	Only works at the moment if the second object is NULL -
 *		if it's not, which object do we lock first?
 *
 *	Parameters:
 *		prev_object	First object to coalesce
 *		prev_offset	Offset into prev_object
 *		next_object	Second object into coalesce
 *		next_offset	Offset into next_object
 *
 *		prev_size	Size of reference to prev_object
 *		next_size	Size of reference to next_object
 *
 *	Conditions:
 *	The object(s) must *not* be locked. The map must be locked
 *	to preserve the reference to the object(s).
 */
static int vm_object_coalesce_count = 0;

__private_extern__ boolean_t
vm_object_coalesce(
	register vm_object_t		prev_object,
	vm_object_t			next_object,
	vm_object_offset_t		prev_offset,
	__unused vm_object_offset_t next_offset,
	vm_object_size_t		prev_size,
	vm_object_size_t		next_size)
{
	vm_object_size_t	newsize;

#ifdef	lint
	next_offset++;
#endif	/* lint */

	if (next_object != VM_OBJECT_NULL) {
		return(FALSE);
	}

	if (prev_object == VM_OBJECT_NULL) {
		return(TRUE);
	}

	XPR(XPR_VM_OBJECT,
       "vm_object_coalesce: 0x%X prev_off 0x%X prev_size 0x%X next_size 0x%X\n",
		prev_object, prev_offset, prev_size, next_size, 0);

	vm_object_lock(prev_object);

	/*
	 *	Try to collapse the object first
	 */
	vm_object_collapse(prev_object, prev_offset, TRUE);

	/*
	 *	Can't coalesce if pages not mapped to
	 *	prev_entry may be in use any way:
	 *	. more than one reference
	 *	. paged out
	 *	. shadows another object
	 *	. has a copy elsewhere
	 *	. is purgeable
	 *	. paging references (pages might be in page-list)
	 */

	if ((prev_object->ref_count > 1) ||
	    prev_object->pager_created ||
	    (prev_object->shadow != VM_OBJECT_NULL) ||
	    (prev_object->copy != VM_OBJECT_NULL) ||
	    (prev_object->true_share != FALSE) ||
	    (prev_object->purgable != VM_PURGABLE_DENY) ||
	    (prev_object->paging_in_progress != 0) ||
	    (prev_object->activity_in_progress != 0)) {
		vm_object_unlock(prev_object);
		return(FALSE);
	}

	vm_object_coalesce_count++;

	/*
	 *	Remove any pages that may still be in the object from
	 *	a previous deallocation.
	 */
	vm_object_page_remove(prev_object,
		prev_offset + prev_size,
		prev_offset + prev_size + next_size);

	/*
	 *	Extend the object if necessary.
	 */
	newsize = prev_offset + prev_size + next_size;
	if (newsize > prev_object->size) {
#if	MACH_PAGEMAP
		/*
		 *	We cannot extend an object that has existence info,
		 *	since the existence info might then fail to cover
		 *	the entire object.
		 *
		 *	This assertion must be true because the object
		 *	has no pager, and we only create existence info
		 *	for objects with pagers.
		 */
		assert(prev_object->existence_map == VM_EXTERNAL_NULL);
#endif	/* MACH_PAGEMAP */
		prev_object->size = newsize;
	}

	vm_object_unlock(prev_object);
	return(TRUE);
}

/*
 *	Attach a set of physical pages to an object, so that they can
 *	be mapped by mapping the object.  Typically used to map IO memory.
 *
 *	The mapping function and its private data are used to obtain the
 *	physical addresses for each page to be mapped.
 */
void
vm_object_page_map(
	vm_object_t		object,
	vm_object_offset_t	offset,
	vm_object_size_t	size,
	vm_object_offset_t	(*map_fn)(void *map_fn_data, 
		vm_object_offset_t offset),
		void 		*map_fn_data)	/* private to map_fn */
{
	int64_t	num_pages;
	int	i;
	vm_page_t	m;
	vm_page_t	old_page;
	vm_object_offset_t	addr;

	num_pages = atop_64(size);

	for (i = 0; i < num_pages; i++, offset += PAGE_SIZE_64) {

	    addr = (*map_fn)(map_fn_data, offset);

	    while ((m = vm_page_grab_fictitious()) == VM_PAGE_NULL)
		vm_page_more_fictitious();

	    vm_object_lock(object);
	    if ((old_page = vm_page_lookup(object, offset))
			!= VM_PAGE_NULL)
	    {
		    VM_PAGE_FREE(old_page);
	    }

	    assert((ppnum_t) addr == addr);
	    vm_page_init(m, (ppnum_t) addr, FALSE);
	    /*
	     * private normally requires lock_queues but since we
	     * are initializing the page, its not necessary here
	     */
	    m->private = TRUE;		/* don`t free page */
	    m->wire_count = 1;
	    vm_page_insert(m, object, offset);

	    PAGE_WAKEUP_DONE(m);
	    vm_object_unlock(object);
	}
}

#include <mach_kdb.h>

#if	MACH_KDB
#include <ddb/db_output.h>
#include <vm/vm_print.h>

#define printf	kdbprintf

extern boolean_t	vm_object_cached(
				vm_object_t object);

extern void		print_bitstring(
				char byte);

boolean_t	vm_object_print_pages = FALSE;

void
print_bitstring(
	char byte)
{
	printf("%c%c%c%c%c%c%c%c",
	       ((byte & (1 << 0)) ? '1' : '0'),
	       ((byte & (1 << 1)) ? '1' : '0'),
	       ((byte & (1 << 2)) ? '1' : '0'),
	       ((byte & (1 << 3)) ? '1' : '0'),
	       ((byte & (1 << 4)) ? '1' : '0'),
	       ((byte & (1 << 5)) ? '1' : '0'),
	       ((byte & (1 << 6)) ? '1' : '0'),
	       ((byte & (1 << 7)) ? '1' : '0'));
}

boolean_t
vm_object_cached(
	__unused register vm_object_t object)
{
#if VM_OBJECT_CACHE
	register vm_object_t o;

	queue_iterate(&vm_object_cached_list, o, vm_object_t, cached_list) {
		if (object == o) {
			return TRUE;
		}
	}
#endif
	return FALSE;
}

#if	MACH_PAGEMAP
/*
 *	vm_external_print:	[ debug ]
 */
void
vm_external_print(
	vm_external_map_t 	emap,
	vm_object_size_t 	size)
{
	if (emap == VM_EXTERNAL_NULL) {
		printf("0  ");
	} else {
		vm_object_size_t existence_size = stob(size);
		printf("{ size=%lld, map=[", (uint64_t) existence_size);
		if (existence_size > 0) {
			print_bitstring(emap[0]);
		}
		if (existence_size > 1) {
			print_bitstring(emap[1]);
		}
		if (existence_size > 2) {
			printf("...");
			print_bitstring(emap[existence_size-1]);
		}
		printf("] }\n");
	}
	return;
}
#endif	/* MACH_PAGEMAP */

int
vm_follow_object(
	vm_object_t object)
{
	int count = 0;
	int orig_db_indent = db_indent;

	while (TRUE) {
		if (object == VM_OBJECT_NULL) {
			db_indent = orig_db_indent;
			return count;
		}

		count += 1;

		iprintf("object 0x%x", object);
		printf(", shadow=0x%x", object->shadow);
		printf(", copy=0x%x", object->copy);
		printf(", pager=0x%x", object->pager);
		printf(", ref=%d\n", object->ref_count);

		db_indent += 2;
		object = object->shadow;
	}

}

/*
 *	vm_object_print:	[ debug ]
 */
void
vm_object_print(db_expr_t db_addr, __unused boolean_t have_addr,
		__unused db_expr_t arg_count, __unused char *modif)
{
	vm_object_t	object;
	register vm_page_t p;
	const char *s;

	register int count;

	object = (vm_object_t) (long) db_addr;
	if (object == VM_OBJECT_NULL)
		return;

	iprintf("object 0x%x\n", object);

	db_indent += 2;

	iprintf("size=0x%x", object->size);
	printf(", memq_hint=%p", object->memq_hint);
	printf(", ref_count=%d\n", object->ref_count);
	iprintf("");
#if	TASK_SWAPPER
	printf("res_count=%d, ", object->res_count);
#endif	/* TASK_SWAPPER */
	printf("resident_page_count=%d\n", object->resident_page_count);

	iprintf("shadow=0x%x", object->shadow);
	if (object->shadow) {
		register int i = 0;
		vm_object_t shadow = object;
		while((shadow = shadow->shadow))
			i++;
		printf(" (depth %d)", i);
	}
	printf(", copy=0x%x", object->copy);
	printf(", shadow_offset=0x%x", object->shadow_offset);
	printf(", last_alloc=0x%x\n", object->last_alloc);

	iprintf("pager=0x%x", object->pager);
	printf(", paging_offset=0x%x", object->paging_offset);
	printf(", pager_control=0x%x\n", object->pager_control);

	iprintf("copy_strategy=%d[", object->copy_strategy);
	switch (object->copy_strategy) {
		case MEMORY_OBJECT_COPY_NONE:
		printf("copy_none");
		break;

		case MEMORY_OBJECT_COPY_CALL:
		printf("copy_call");
		break;

		case MEMORY_OBJECT_COPY_DELAY:
		printf("copy_delay");
		break;

		case MEMORY_OBJECT_COPY_SYMMETRIC:
		printf("copy_symmetric");
		break;

		case MEMORY_OBJECT_COPY_INVALID:
		printf("copy_invalid");
		break;

		default:
		printf("?");
	}
	printf("]");

	iprintf("all_wanted=0x%x<", object->all_wanted);
	s = "";
	if (vm_object_wanted(object, VM_OBJECT_EVENT_INITIALIZED)) {
		printf("%sinit", s);
		s = ",";
	}
	if (vm_object_wanted(object, VM_OBJECT_EVENT_PAGER_READY)) {
		printf("%sready", s);
		s = ",";
	}
	if (vm_object_wanted(object, VM_OBJECT_EVENT_PAGING_IN_PROGRESS)) {
		printf("%spaging", s);
		s = ",";
	}
	if (vm_object_wanted(object, VM_OBJECT_EVENT_LOCK_IN_PROGRESS)) {
		printf("%slock", s);
		s = ",";
	}
	if (vm_object_wanted(object, VM_OBJECT_EVENT_UNCACHING)) {
		printf("%suncaching", s);
		s = ",";
	}
	if (vm_object_wanted(object, VM_OBJECT_EVENT_COPY_CALL)) {
		printf("%scopy_call", s);
		s = ",";
	}
	if (vm_object_wanted(object, VM_OBJECT_EVENT_CACHING)) {
		printf("%scaching", s);
		s = ",";
	}
	printf(">");
	printf(", paging_in_progress=%d\n", object->paging_in_progress);
	printf(", activity_in_progress=%d\n", object->activity_in_progress);

	iprintf("%screated, %sinit, %sready, %spersist, %strusted, %spageout, %s, %s\n",
		(object->pager_created ? "" : "!"),
		(object->pager_initialized ? "" : "!"),
		(object->pager_ready ? "" : "!"),
		(object->can_persist ? "" : "!"),
		(object->pager_trusted ? "" : "!"),
		(object->pageout ? "" : "!"),
		(object->internal ? "internal" : "external"),
		(object->temporary ? "temporary" : "permanent"));
	iprintf("%salive, %spurgeable, %spurgeable_volatile, %spurgeable_empty, %sshadowed, %scached, %sprivate\n",
		(object->alive ? "" : "!"),
		((object->purgable != VM_PURGABLE_DENY) ? "" : "!"),
		((object->purgable == VM_PURGABLE_VOLATILE) ? "" : "!"),
		((object->purgable == VM_PURGABLE_EMPTY) ? "" : "!"),
		(object->shadowed ? "" : "!"),
		(vm_object_cached(object) ? "" : "!"),
		(object->private ? "" : "!"));
	iprintf("%sadvisory_pageout, %ssilent_overwrite\n",
		(object->advisory_pageout ? "" : "!"),
		(object->silent_overwrite ? "" : "!"));

#if	MACH_PAGEMAP
	iprintf("existence_map=");
	vm_external_print(object->existence_map, object->size);
#endif	/* MACH_PAGEMAP */
#if	MACH_ASSERT
	iprintf("paging_object=0x%x\n", object->paging_object);
#endif	/* MACH_ASSERT */

	if (vm_object_print_pages) {
		count = 0;
		p = (vm_page_t) queue_first(&object->memq);
		while (!queue_end(&object->memq, (queue_entry_t) p)) {
			if (count == 0) {
				iprintf("memory:=");
			} else if (count == 2) {
				printf("\n");
				iprintf(" ...");
				count = 0;
			} else {
				printf(",");
			}
			count++;

			printf("(off=0x%llX,page=%p)", p->offset, p);
			p = (vm_page_t) queue_next(&p->listq);
		}
		if (count != 0) {
			printf("\n");
		}
	}
	db_indent -= 2;
}


/*
 *	vm_object_find		[ debug ]
 *
 *	Find all tasks which reference the given vm_object.
 */

boolean_t vm_object_find(vm_object_t object);
boolean_t vm_object_print_verbose = FALSE;

boolean_t
vm_object_find(
	vm_object_t     object)
{
        task_t task;
	vm_map_t map;
	vm_map_entry_t entry;
	boolean_t found = FALSE;

	queue_iterate(&tasks, task, task_t, tasks) {
		map = task->map;
		for (entry = vm_map_first_entry(map);
			 entry && entry != vm_map_to_entry(map);
			 entry = entry->vme_next) {

			vm_object_t obj;

			/* 
			 * For the time being skip submaps,
			 * only the kernel can have submaps,
			 * and unless we are interested in 
			 * kernel objects, we can simply skip 
			 * submaps. See sb/dejan/nmk18b7/src/mach_kernel/vm
			 * for a full solution.
			 */
			if (entry->is_sub_map)
				continue;
			if (entry) 
				obj = entry->object.vm_object;
			else 
				continue;

			while (obj != VM_OBJECT_NULL) {
				if (obj == object) {
					if (!found) {
						printf("TASK\t\tMAP\t\tENTRY\n");
						found = TRUE;
					}
					printf("0x%x\t0x%x\t0x%x\n", 
						   task, map, entry);
				}
				obj = obj->shadow;
			}
		}
	}

	return(found);
}

#endif	/* MACH_KDB */

kern_return_t
vm_object_populate_with_private(
		vm_object_t		object,
		vm_object_offset_t	offset,
		ppnum_t			phys_page,
		vm_size_t		size)
{
	ppnum_t			base_page;
	vm_object_offset_t	base_offset;


	if(!object->private)
		return KERN_FAILURE;

	base_page = phys_page;

	vm_object_lock(object);
	if(!object->phys_contiguous) {
		vm_page_t	m;
		if((base_offset = trunc_page_64(offset)) != offset) {
			vm_object_unlock(object);
			return KERN_FAILURE;
		}
		base_offset += object->paging_offset;
		while(size) {
			m = vm_page_lookup(object, base_offset);
			if(m != VM_PAGE_NULL) {
				if(m->fictitious) {
					if (m->phys_page != vm_page_guard_addr) {

						vm_page_lockspin_queues();
						m->private = TRUE;
						vm_page_unlock_queues();

						m->fictitious = FALSE;
						m->phys_page = base_page;
						if(!m->busy) {
							m->busy = TRUE;
						}
						if(!m->absent) {
							m->absent = TRUE;
						}
						m->list_req_pending = TRUE;
					}
				} else if (m->phys_page != base_page) {
				        if (m->pmapped) {
					        /*
						 * pmap call to clear old mapping
						 */
					        pmap_disconnect(m->phys_page);
					}
					m->phys_page = base_page;
				}

				/*
				 * ENCRYPTED SWAP:
				 * We're not pointing to the same
				 * physical page any longer and the
				 * contents of the new one are not
				 * supposed to be encrypted.
				 * XXX What happens to the original
				 * physical page. Is it lost ?
				 */
				m->encrypted = FALSE;

			} else {
				while ((m = vm_page_grab_fictitious()) == VM_PAGE_NULL)
                			vm_page_more_fictitious();	

				/*
				 * private normally requires lock_queues but since we
				 * are initializing the page, its not necessary here
				 */
				m->private = TRUE;
				m->fictitious = FALSE;
				m->phys_page = base_page;
				m->list_req_pending = TRUE;
				m->absent = TRUE;
				m->unusual = TRUE;

	    			vm_page_insert(m, object, base_offset);
			}
			base_page++;									/* Go to the next physical page */
			base_offset += PAGE_SIZE;
			size -= PAGE_SIZE;
		}
	} else {
		/* NOTE: we should check the original settings here */
		/* if we have a size > zero a pmap call should be made */
		/* to disable the range */	

		/* pmap_? */
		
		/* shadows on contiguous memory are not allowed */
		/* we therefore can use the offset field */
		object->shadow_offset = (vm_object_offset_t)phys_page << PAGE_SHIFT;
		object->size = size;
	}
	vm_object_unlock(object);
	return KERN_SUCCESS;
}

/*
 *	memory_object_free_from_cache:
 *
 *	Walk the vm_object cache list, removing and freeing vm_objects 
 *	which are backed by the pager identified by the caller, (pager_ops).  
 *	Remove up to "count" objects, if there are that may available
 *	in the cache.
 *
 *	Walk the list at most once, return the number of vm_objects
 *	actually freed.
 */

__private_extern__ kern_return_t
memory_object_free_from_cache(
	__unused host_t		host,
	__unused memory_object_pager_ops_t pager_ops,
	int		*count)
{
#if VM_OBJECT_CACHE
	int	object_released = 0;

	register vm_object_t object = VM_OBJECT_NULL;
	vm_object_t shadow;

/*
	if(host == HOST_NULL)
		return(KERN_INVALID_ARGUMENT);
*/

 try_again:
	vm_object_cache_lock();

	queue_iterate(&vm_object_cached_list, object, 
					vm_object_t, cached_list) {
		if (object->pager &&
		    (pager_ops == object->pager->mo_pager_ops)) {
			vm_object_lock(object);
			queue_remove(&vm_object_cached_list, object, 
					vm_object_t, cached_list);
			vm_object_cached_count--;

			vm_object_cache_unlock();
			/*
		 	*	Since this object is in the cache, we know
		 	*	that it is initialized and has only a pager's
			*	(implicit) reference. Take a reference to avoid
			*	recursive deallocations.
		 	*/

			assert(object->pager_initialized);
			assert(object->ref_count == 0);
			vm_object_lock_assert_exclusive(object);
			object->ref_count++;

			/*
		 	*	Terminate the object.
		 	*	If the object had a shadow, we let 
			*	vm_object_deallocate deallocate it. 
			*	"pageout" objects have a shadow, but
		 	*	maintain a "paging reference" rather 
			*	than a normal reference.
		 	*	(We are careful here to limit recursion.)
		 	*/
			shadow = object->pageout?VM_OBJECT_NULL:object->shadow;

			if ((vm_object_terminate(object) == KERN_SUCCESS)
					&& (shadow != VM_OBJECT_NULL)) {
				vm_object_deallocate(shadow);
			}
		
			if(object_released++ == *count)
				return KERN_SUCCESS;
			goto try_again;
		}
	}
	vm_object_cache_unlock();
	*count  = object_released;
#else
	*count = 0;
#endif
	return KERN_SUCCESS;
}



kern_return_t
memory_object_create_named(
	memory_object_t	pager,
	memory_object_offset_t	size,
	memory_object_control_t		*control)
{
	vm_object_t 		object;
	vm_object_hash_entry_t	entry;
	lck_mtx_t		*lck;

	*control = MEMORY_OBJECT_CONTROL_NULL;
	if (pager == MEMORY_OBJECT_NULL)
		return KERN_INVALID_ARGUMENT;

	lck = vm_object_hash_lock_spin(pager);
	entry = vm_object_hash_lookup(pager, FALSE);

	if ((entry != VM_OBJECT_HASH_ENTRY_NULL) &&
			(entry->object != VM_OBJECT_NULL)) {
		if (entry->object->named == TRUE)
			panic("memory_object_create_named: caller already holds the right");	}
	vm_object_hash_unlock(lck);

	if ((object = vm_object_enter(pager, size, FALSE, FALSE, TRUE)) == VM_OBJECT_NULL) {
		return(KERN_INVALID_OBJECT);
	}
	
	/* wait for object (if any) to be ready */
	if (object != VM_OBJECT_NULL) {
		vm_object_lock(object);
		object->named = TRUE;
		while (!object->pager_ready) {
			vm_object_sleep(object,
					VM_OBJECT_EVENT_PAGER_READY,
					THREAD_UNINT);
		}
		*control = object->pager_control;
		vm_object_unlock(object);
	}
	return (KERN_SUCCESS);
}


/*
 *	Routine:	memory_object_recover_named [user interface]
 *	Purpose:
 *		Attempt to recover a named reference for a VM object.
 *		VM will verify that the object has not already started
 *		down the termination path, and if it has, will optionally
 *		wait for that to finish.
 *	Returns:
 *		KERN_SUCCESS - we recovered a named reference on the object
 *		KERN_FAILURE - we could not recover a reference (object dead)
 *		KERN_INVALID_ARGUMENT - bad memory object control
 */
kern_return_t
memory_object_recover_named(
	memory_object_control_t	control,
	boolean_t		wait_on_terminating)
{
	vm_object_t		object;

	object = memory_object_control_to_vm_object(control);
	if (object == VM_OBJECT_NULL) {
		return (KERN_INVALID_ARGUMENT);
	}
restart:
	vm_object_lock(object);

	if (object->terminating && wait_on_terminating) {
		vm_object_wait(object, 
			VM_OBJECT_EVENT_PAGING_IN_PROGRESS, 
			THREAD_UNINT);
		goto restart;
	}

	if (!object->alive) {
		vm_object_unlock(object);
		return KERN_FAILURE;
	}

	if (object->named == TRUE) {
		vm_object_unlock(object);
		return KERN_SUCCESS;
	}
#if VM_OBJECT_CACHE
	if ((object->ref_count == 0) && (!object->terminating)) {
		if (!vm_object_cache_lock_try()) {
			vm_object_unlock(object);
			goto restart;
		}
		queue_remove(&vm_object_cached_list, object,
				     vm_object_t, cached_list);
		vm_object_cached_count--;
		XPR(XPR_VM_OBJECT_CACHE,
		    "memory_object_recover_named: removing %X, head (%X, %X)\n",
		    object, 
		    vm_object_cached_list.next,
		    vm_object_cached_list.prev, 0,0);
		
		vm_object_cache_unlock();
	}
#endif
	object->named = TRUE;
	vm_object_lock_assert_exclusive(object);
	object->ref_count++;
	vm_object_res_reference(object);
	while (!object->pager_ready) {
		vm_object_sleep(object,
				VM_OBJECT_EVENT_PAGER_READY,
				THREAD_UNINT);
	}
	vm_object_unlock(object);
	return (KERN_SUCCESS);
}


/*
 *	vm_object_release_name:  
 *
 *	Enforces name semantic on memory_object reference count decrement
 *	This routine should not be called unless the caller holds a name
 *	reference gained through the memory_object_create_named.
 *
 *	If the TERMINATE_IDLE flag is set, the call will return if the
 *	reference count is not 1. i.e. idle with the only remaining reference
 *	being the name.
 *	If the decision is made to proceed the name field flag is set to
 *	false and the reference count is decremented.  If the RESPECT_CACHE
 *	flag is set and the reference count has gone to zero, the 
 *	memory_object is checked to see if it is cacheable otherwise when
 *	the reference count is zero, it is simply terminated.
 */

__private_extern__ kern_return_t
vm_object_release_name(
	vm_object_t	object,
	int		flags)
{
	vm_object_t	shadow;
	boolean_t	original_object = TRUE;

	while (object != VM_OBJECT_NULL) {

		vm_object_lock(object);

		assert(object->alive);
		if (original_object)
			assert(object->named);
		assert(object->ref_count > 0);

		/*
		 *	We have to wait for initialization before
		 *	destroying or caching the object.
		 */

		if (object->pager_created && !object->pager_initialized) {
			assert(!object->can_persist);
			vm_object_assert_wait(object,
					VM_OBJECT_EVENT_INITIALIZED,
					THREAD_UNINT);
			vm_object_unlock(object);
			thread_block(THREAD_CONTINUE_NULL);
			continue;
		}

		if (((object->ref_count > 1)
			&& (flags & MEMORY_OBJECT_TERMINATE_IDLE))
			|| (object->terminating)) {
			vm_object_unlock(object);
			return KERN_FAILURE;
		} else {
			if (flags & MEMORY_OBJECT_RELEASE_NO_OP) {
				vm_object_unlock(object);
				return KERN_SUCCESS;
			}
		}
		
		if ((flags & MEMORY_OBJECT_RESPECT_CACHE) &&
					(object->ref_count == 1)) {
			if (original_object)
				object->named = FALSE;
			vm_object_unlock(object);
			/* let vm_object_deallocate push this thing into */
			/* the cache, if that it is where it is bound */
			vm_object_deallocate(object);
			return KERN_SUCCESS;
		}
		VM_OBJ_RES_DECR(object);
		shadow = object->pageout?VM_OBJECT_NULL:object->shadow;

		if (object->ref_count == 1) {
			if (vm_object_terminate(object) != KERN_SUCCESS) {
				if (original_object) {
					return KERN_FAILURE;
				} else {
					return KERN_SUCCESS;
				}
			}
			if (shadow != VM_OBJECT_NULL) {
				original_object = FALSE;
				object = shadow;
				continue;
			}
			return KERN_SUCCESS;
		} else {
			vm_object_lock_assert_exclusive(object);
			object->ref_count--;
			assert(object->ref_count > 0);
			if(original_object)
				object->named = FALSE;
			vm_object_unlock(object);
			return KERN_SUCCESS;
		}
	}
	/*NOTREACHED*/
	assert(0);
	return KERN_FAILURE;
}


__private_extern__ kern_return_t
vm_object_lock_request(
	vm_object_t			object,
	vm_object_offset_t		offset,
	vm_object_size_t		size,
	memory_object_return_t		should_return,
	int				flags,
	vm_prot_t			prot)
{
	__unused boolean_t	should_flush;

	should_flush = flags & MEMORY_OBJECT_DATA_FLUSH;

        XPR(XPR_MEMORY_OBJECT,
	    "vm_o_lock_request, obj 0x%X off 0x%X size 0x%X flags %X prot %X\n",
	    object, offset, size, 
 	    (((should_return&1)<<1)|should_flush), prot);

	/*
	 *	Check for bogus arguments.
	 */
	if (object == VM_OBJECT_NULL)
		return (KERN_INVALID_ARGUMENT);

	if ((prot & ~VM_PROT_ALL) != 0 && prot != VM_PROT_NO_CHANGE)
		return (KERN_INVALID_ARGUMENT);

	size = round_page_64(size);

	/*
	 *	Lock the object, and acquire a paging reference to
	 *	prevent the memory_object reference from being released.
	 */
	vm_object_lock(object);
	vm_object_paging_begin(object);

	(void)vm_object_update(object,
		offset, size, NULL, NULL, should_return, flags, prot);

	vm_object_paging_end(object);
	vm_object_unlock(object);

	return (KERN_SUCCESS);
}

/*
 * Empty a purgeable object by grabbing the physical pages assigned to it and
 * putting them on the free queue without writing them to backing store, etc.
 * When the pages are next touched they will be demand zero-fill pages.  We
 * skip pages which are busy, being paged in/out, wired, etc.  We do _not_
 * skip referenced/dirty pages, pages on the active queue, etc.  We're more
 * than happy to grab these since this is a purgeable object.  We mark the
 * object as "empty" after reaping its pages.
 *
 * On entry the object must be locked and it must be
 * purgeable with no delayed copies pending.
 */
void
vm_object_purge(vm_object_t object)
{
        vm_object_lock_assert_exclusive(object);

	if (object->purgable == VM_PURGABLE_DENY)
		return;

	assert(object->copy == VM_OBJECT_NULL);
	assert(object->copy_strategy == MEMORY_OBJECT_COPY_NONE);

	if(object->purgable == VM_PURGABLE_VOLATILE) {
		unsigned int delta;
		assert(object->resident_page_count >=
		       object->wired_page_count);
		delta = (object->resident_page_count -
			 object->wired_page_count);
		if (delta != 0) {
			assert(vm_page_purgeable_count >=
			       delta);
			OSAddAtomic(-delta,
				    (SInt32 *)&vm_page_purgeable_count);
		}
		if (object->wired_page_count != 0) {
			assert(vm_page_purgeable_wired_count >=
			       object->wired_page_count);
			OSAddAtomic(-object->wired_page_count,
				    (SInt32 *)&vm_page_purgeable_wired_count);
		}
	}
	object->purgable = VM_PURGABLE_EMPTY;
	
	vm_object_reap_pages(object, REAP_PURGEABLE);
}
				

/*
 * vm_object_purgeable_control() allows the caller to control and investigate the
 * state of a purgeable object.  A purgeable object is created via a call to
 * vm_allocate() with VM_FLAGS_PURGABLE specified.  A purgeable object will
 * never be coalesced with any other object -- even other purgeable objects --
 * and will thus always remain a distinct object.  A purgeable object has
 * special semantics when its reference count is exactly 1.  If its reference
 * count is greater than 1, then a purgeable object will behave like a normal
 * object and attempts to use this interface will result in an error return
 * of KERN_INVALID_ARGUMENT.
 *
 * A purgeable object may be put into a "volatile" state which will make the
 * object's pages elligable for being reclaimed without paging to backing
 * store if the system runs low on memory.  If the pages in a volatile
 * purgeable object are reclaimed, the purgeable object is said to have been
 * "emptied."  When a purgeable object is emptied the system will reclaim as
 * many pages from the object as it can in a convenient manner (pages already
 * en route to backing store or busy for other reasons are left as is).  When
 * a purgeable object is made volatile, its pages will generally be reclaimed
 * before other pages in the application's working set.  This semantic is
 * generally used by applications which can recreate the data in the object
 * faster than it can be paged in.  One such example might be media assets
 * which can be reread from a much faster RAID volume.
 *
 * A purgeable object may be designated as "non-volatile" which means it will
 * behave like all other objects in the system with pages being written to and
 * read from backing store as needed to satisfy system memory needs.  If the
 * object was emptied before the object was made non-volatile, that fact will
 * be returned as the old state of the purgeable object (see
 * VM_PURGABLE_SET_STATE below).  In this case, any pages of the object which
 * were reclaimed as part of emptying the object will be refaulted in as
 * zero-fill on demand.  It is up to the application to note that an object
 * was emptied and recreate the objects contents if necessary.  When a
 * purgeable object is made non-volatile, its pages will generally not be paged
 * out to backing store in the immediate future.  A purgeable object may also
 * be manually emptied.
 *
 * Finally, the current state (non-volatile, volatile, volatile & empty) of a
 * volatile purgeable object may be queried at any time.  This information may
 * be used as a control input to let the application know when the system is
 * experiencing memory pressure and is reclaiming memory.
 *
 * The specified address may be any address within the purgeable object.  If
 * the specified address does not represent any object in the target task's
 * virtual address space, then KERN_INVALID_ADDRESS will be returned.  If the
 * object containing the specified address is not a purgeable object, then
 * KERN_INVALID_ARGUMENT will be returned.  Otherwise, KERN_SUCCESS will be
 * returned.
 *
 * The control parameter may be any one of VM_PURGABLE_SET_STATE or
 * VM_PURGABLE_GET_STATE.  For VM_PURGABLE_SET_STATE, the in/out parameter
 * state is used to set the new state of the purgeable object and return its
 * old state.  For VM_PURGABLE_GET_STATE, the current state of the purgeable
 * object is returned in the parameter state.
 *
 * The in/out parameter state may be one of VM_PURGABLE_NONVOLATILE,
 * VM_PURGABLE_VOLATILE or VM_PURGABLE_EMPTY.  These, respectively, represent
 * the non-volatile, volatile and volatile/empty states described above.
 * Setting the state of a purgeable object to VM_PURGABLE_EMPTY will
 * immediately reclaim as many pages in the object as can be conveniently
 * collected (some may have already been written to backing store or be
 * otherwise busy).
 *
 * The process of making a purgeable object non-volatile and determining its
 * previous state is atomic.  Thus, if a purgeable object is made
 * VM_PURGABLE_NONVOLATILE and the old state is returned as
 * VM_PURGABLE_VOLATILE, then the purgeable object's previous contents are
 * completely intact and will remain so until the object is made volatile
 * again.  If the old state is returned as VM_PURGABLE_EMPTY then the object
 * was reclaimed while it was in a volatile state and its previous contents
 * have been lost.
 */
/*
 * The object must be locked.
 */
kern_return_t
vm_object_purgable_control(
	vm_object_t	object,
	vm_purgable_t	control,
	int		*state)
{
	int		old_state;
	int		new_state;

	if (object == VM_OBJECT_NULL) {
		/*
		 * Object must already be present or it can't be purgeable.
		 */
		return KERN_INVALID_ARGUMENT;
	}

	/*
	 * Get current state of the purgeable object.
	 */
	old_state = object->purgable;
	if (old_state == VM_PURGABLE_DENY)
		return KERN_INVALID_ARGUMENT;
    
	/* purgeable cant have delayed copies - now or in the future */
	assert(object->copy == VM_OBJECT_NULL); 
	assert(object->copy_strategy == MEMORY_OBJECT_COPY_NONE);

	/*
	 * Execute the desired operation.
	 */
	if (control == VM_PURGABLE_GET_STATE) {
		*state = old_state;
		return KERN_SUCCESS;
	}

	if ((*state) & VM_PURGABLE_DEBUG_EMPTY) {
		object->volatile_empty = TRUE;
	}
	if ((*state) & VM_PURGABLE_DEBUG_FAULT) {
		object->volatile_fault = TRUE;
	}

	new_state = *state & VM_PURGABLE_STATE_MASK;
	if (new_state == VM_PURGABLE_VOLATILE &&
	    object->volatile_empty) {
		new_state = VM_PURGABLE_EMPTY;
	}

	switch (new_state) {
	case VM_PURGABLE_DENY:
	case VM_PURGABLE_NONVOLATILE:
		object->purgable = new_state;

		if (old_state == VM_PURGABLE_VOLATILE) {
			unsigned int delta;

			assert(object->resident_page_count >=
			       object->wired_page_count);
			delta = (object->resident_page_count -
				 object->wired_page_count);

			assert(vm_page_purgeable_count >= delta);

			if (delta != 0) {
				OSAddAtomic(-delta,
					    (SInt32 *)&vm_page_purgeable_count);
			}
			if (object->wired_page_count != 0) {
				assert(vm_page_purgeable_wired_count >=
				       object->wired_page_count);
				OSAddAtomic(-object->wired_page_count,
					    (SInt32 *)&vm_page_purgeable_wired_count);
			}

			vm_page_lock_queues();

			assert(object->objq.next != NULL && object->objq.prev != NULL); /* object should be on a queue */
			purgeable_q_t queue = vm_purgeable_object_remove(object);
			assert(queue);

			vm_purgeable_token_delete_first(queue);
			assert(queue->debug_count_objects>=0);

			vm_page_unlock_queues();
		}
		break;

	case VM_PURGABLE_VOLATILE:
		if (object->volatile_fault) {
			vm_page_t	p;
			int		refmod;

			queue_iterate(&object->memq, p, vm_page_t, listq) {
				if (p->busy ||
				    VM_PAGE_WIRED(p) ||
				    p->fictitious) {
					continue;
				}
				refmod = pmap_disconnect(p->phys_page);
				if ((refmod & VM_MEM_MODIFIED) &&
				    !p->dirty) {
					p->dirty = TRUE;
				}
			}
		}
					       
		if (old_state == VM_PURGABLE_EMPTY &&
		    object->resident_page_count == 0)
			break;

		purgeable_q_t queue;
        
		/* find the correct queue */
		if ((*state&VM_PURGABLE_ORDERING_MASK) == VM_PURGABLE_ORDERING_OBSOLETE)
		        queue = &purgeable_queues[PURGEABLE_Q_TYPE_OBSOLETE];
		else {
		        if ((*state&VM_PURGABLE_BEHAVIOR_MASK) == VM_PURGABLE_BEHAVIOR_FIFO)
			        queue = &purgeable_queues[PURGEABLE_Q_TYPE_FIFO];
			else
			        queue = &purgeable_queues[PURGEABLE_Q_TYPE_LIFO];
		}
        
		if (old_state == VM_PURGABLE_NONVOLATILE ||
		    old_state == VM_PURGABLE_EMPTY) {
			unsigned int delta;

		        /* try to add token... this can fail */
		        vm_page_lock_queues();

			kern_return_t result = vm_purgeable_token_add(queue);
			if (result != KERN_SUCCESS) {
			        vm_page_unlock_queues();
				return result;
			}
			vm_page_unlock_queues();

			assert(object->resident_page_count >=
			       object->wired_page_count);
			delta = (object->resident_page_count -
				 object->wired_page_count);

			if (delta != 0) {
				OSAddAtomic(delta,
					    &vm_page_purgeable_count);
			}
			if (object->wired_page_count != 0) {
				OSAddAtomic(object->wired_page_count,
					    &vm_page_purgeable_wired_count);
			}

			object->purgable = new_state;

			/* object should not be on a queue */
			assert(object->objq.next == NULL && object->objq.prev == NULL);
		}
		else if (old_state == VM_PURGABLE_VOLATILE) {
		        /*
			 * if reassigning priorities / purgeable groups, we don't change the
			 * token queue. So moving priorities will not make pages stay around longer.
			 * Reasoning is that the algorithm gives most priority to the most important
			 * object. If a new token is added, the most important object' priority is boosted.
			 * This biases the system already for purgeable queues that move a lot.
			 * It doesn't seem more biasing is neccessary in this case, where no new object is added.
			 */
		        assert(object->objq.next != NULL && object->objq.prev != NULL); /* object should be on a queue */
            
			purgeable_q_t old_queue=vm_purgeable_object_remove(object);
			assert(old_queue);
            
			if (old_queue != queue) {
				kern_return_t result;

			        /* Changing queue. Have to move token. */
			        vm_page_lock_queues();
				vm_purgeable_token_delete_first(old_queue);
				result = vm_purgeable_token_add(queue);
				vm_page_unlock_queues();

				assert(result==KERN_SUCCESS);   /* this should never fail since we just freed a token */
			}
		};
		vm_purgeable_object_add(object, queue, (*state&VM_VOLATILE_GROUP_MASK)>>VM_VOLATILE_GROUP_SHIFT );

		assert(queue->debug_count_objects>=0);
        
		break;


	case VM_PURGABLE_EMPTY:
		if (object->volatile_fault) {
			vm_page_t	p;
			int		refmod;

			queue_iterate(&object->memq, p, vm_page_t, listq) {
				if (p->busy ||
				    VM_PAGE_WIRED(p) ||
				    p->fictitious) {
					continue;
				}
				refmod = pmap_disconnect(p->phys_page);
				if ((refmod & VM_MEM_MODIFIED) &&
				    !p->dirty) {
					p->dirty = TRUE;
				}
			}
		}

		if (old_state != new_state) {
			assert(old_state == VM_PURGABLE_NONVOLATILE ||
			       old_state == VM_PURGABLE_VOLATILE);
			if (old_state == VM_PURGABLE_VOLATILE) {
				purgeable_q_t old_queue;

				/* object should be on a queue */
				assert(object->objq.next != NULL &&
				       object->objq.prev != NULL);
				old_queue = vm_purgeable_object_remove(object);
				assert(old_queue);
				vm_page_lock_queues();
				vm_purgeable_token_delete_first(old_queue);
				vm_page_unlock_queues();
			}
			(void) vm_object_purge(object);
		}
		break;

	}
	*state = old_state;

	return KERN_SUCCESS;
}

#if	TASK_SWAPPER
/*
 * vm_object_res_deallocate
 *
 * (recursively) decrement residence counts on vm objects and their shadows.
 * Called from vm_object_deallocate and when swapping out an object.
 *
 * The object is locked, and remains locked throughout the function,
 * even as we iterate down the shadow chain.  Locks on intermediate objects
 * will be dropped, but not the original object.
 *
 * NOTE: this function used to use recursion, rather than iteration.
 */

__private_extern__ void
vm_object_res_deallocate(
	vm_object_t	object)
{
	vm_object_t orig_object = object;
	/*
	 * Object is locked so it can be called directly
	 * from vm_object_deallocate.  Original object is never
	 * unlocked.
	 */
	assert(object->res_count > 0);
	while  (--object->res_count == 0) {
		assert(object->ref_count >= object->res_count);
		vm_object_deactivate_all_pages(object);
		/* iterate on shadow, if present */
		if (object->shadow != VM_OBJECT_NULL) {
			vm_object_t tmp_object = object->shadow;
			vm_object_lock(tmp_object);
			if (object != orig_object)
				vm_object_unlock(object);
			object = tmp_object;
			assert(object->res_count > 0);
		} else
			break;
	}
	if (object != orig_object)
		vm_object_unlock(object);
}

/*
 * vm_object_res_reference
 *
 * Internal function to increment residence count on a vm object
 * and its shadows.  It is called only from vm_object_reference, and
 * when swapping in a vm object, via vm_map_swap.
 *
 * The object is locked, and remains locked throughout the function,
 * even as we iterate down the shadow chain.  Locks on intermediate objects
 * will be dropped, but not the original object.
 *
 * NOTE: this function used to use recursion, rather than iteration.
 */

__private_extern__ void
vm_object_res_reference(
	vm_object_t	object)
{
	vm_object_t orig_object = object;
	/* 
	 * Object is locked, so this can be called directly
	 * from vm_object_reference.  This lock is never released.
	 */
	while  ((++object->res_count == 1)  && 
		(object->shadow != VM_OBJECT_NULL)) {
		vm_object_t tmp_object = object->shadow;

		assert(object->ref_count >= object->res_count);
		vm_object_lock(tmp_object);
		if (object != orig_object)
			vm_object_unlock(object);
		object = tmp_object;
	}
	if (object != orig_object)
		vm_object_unlock(object);
	assert(orig_object->ref_count >= orig_object->res_count);
}
#endif	/* TASK_SWAPPER */

/*
 *	vm_object_reference:
 *
 *	Gets another reference to the given object.
 */
#ifdef vm_object_reference
#undef vm_object_reference
#endif
__private_extern__ void
vm_object_reference(
	register vm_object_t	object)
{
	if (object == VM_OBJECT_NULL)
		return;

	vm_object_lock(object);
	assert(object->ref_count > 0);
	vm_object_reference_locked(object);
	vm_object_unlock(object);
}

#ifdef MACH_BSD
/*
 * Scale the vm_object_cache
 * This is required to make sure that the vm_object_cache is big
 * enough to effectively cache the mapped file.
 * This is really important with UBC as all the regular file vnodes
 * have memory object associated with them. Havving this cache too
 * small results in rapid reclaim of vnodes and hurts performance a LOT!
 *
 * This is also needed as number of vnodes can be dynamically scaled.
 */
kern_return_t
adjust_vm_object_cache(
	__unused vm_size_t oval,
	__unused vm_size_t nval)
{
#if VM_OBJECT_CACHE
	vm_object_cached_max = nval;
	vm_object_cache_trim(FALSE);
#endif
	return (KERN_SUCCESS);
}
#endif /* MACH_BSD */


/*
 * vm_object_transpose
 *
 * This routine takes two VM objects of the same size and exchanges
 * their backing store.
 * The objects should be "quiesced" via a UPL operation with UPL_SET_IO_WIRE
 * and UPL_BLOCK_ACCESS if they are referenced anywhere.
 *
 * The VM objects must not be locked by caller.
 */
unsigned int vm_object_transpose_count = 0;
kern_return_t
vm_object_transpose(
	vm_object_t		object1,
	vm_object_t		object2,
	vm_object_size_t	transpose_size)
{
	vm_object_t		tmp_object;
	kern_return_t		retval;
	boolean_t		object1_locked, object2_locked;
	vm_page_t		page;
	vm_object_offset_t	page_offset;
	lck_mtx_t		*hash_lck;
	vm_object_hash_entry_t	hash_entry;

	tmp_object = VM_OBJECT_NULL;
	object1_locked = FALSE; object2_locked = FALSE;

	if (object1 == object2 ||
	    object1 == VM_OBJECT_NULL ||
	    object2 == VM_OBJECT_NULL) {
		/*
		 * If the 2 VM objects are the same, there's
		 * no point in exchanging their backing store.
		 */
		retval = KERN_INVALID_VALUE;
		goto done;
	}

	/*
	 * Since we need to lock both objects at the same time,
	 * make sure we always lock them in the same order to
	 * avoid deadlocks.
	 */
	if (object1 >  object2) {
		tmp_object = object1;
		object1 = object2;
		object2 = tmp_object;
	}

	/*
	 * Allocate a temporary VM object to hold object1's contents
	 * while we copy object2 to object1.
	 */
	tmp_object = vm_object_allocate(transpose_size);
	vm_object_lock(tmp_object);
	tmp_object->can_persist = FALSE;


	/*
	 * Grab control of the 1st VM object.
	 */
	vm_object_lock(object1);
	object1_locked = TRUE;
	if (!object1->alive || object1->terminating ||
	    object1->copy || object1->shadow || object1->shadowed ||
	    object1->purgable != VM_PURGABLE_DENY) {
		/*
		 * We don't deal with copy or shadow objects (yet).
		 */
		retval = KERN_INVALID_VALUE;
		goto done;
	}
	/*
	 * We're about to mess with the object's backing store and 
	 * taking a "paging_in_progress" reference wouldn't be enough
	 * to prevent any paging activity on this object, so the caller should
	 * have "quiesced" the objects beforehand, via a UPL operation with
	 * UPL_SET_IO_WIRE (to make sure all the pages are there and wired)
	 * and UPL_BLOCK_ACCESS (to mark the pages "busy").
	 * 
	 * Wait for any paging operation to complete (but only paging, not 
	 * other kind of activities not linked to the pager).  After we're
	 * statisfied that there's no more paging in progress, we keep the
	 * object locked, to guarantee that no one tries to access its pager.
	 */
	vm_object_paging_only_wait(object1, THREAD_UNINT);

	/*
	 * Same as above for the 2nd object...
	 */
	vm_object_lock(object2);
	object2_locked = TRUE;
	if (! object2->alive || object2->terminating ||
	    object2->copy || object2->shadow || object2->shadowed ||
	    object2->purgable != VM_PURGABLE_DENY) {
		retval = KERN_INVALID_VALUE;
		goto done;
	}
	vm_object_paging_only_wait(object2, THREAD_UNINT);


	if (object1->size != object2->size ||
	    object1->size != transpose_size) {
		/*
		 * If the 2 objects don't have the same size, we can't
		 * exchange their backing stores or one would overflow.
		 * If their size doesn't match the caller's
		 * "transpose_size", we can't do it either because the
		 * transpose operation will affect the entire span of 
		 * the objects.
		 */
		retval = KERN_INVALID_VALUE;
		goto done;
	}


	/*
	 * Transpose the lists of resident pages.
	 * This also updates the resident_page_count and the memq_hint.
	 */
	if (object1->phys_contiguous || queue_empty(&object1->memq)) {
		/*
		 * No pages in object1, just transfer pages
		 * from object2 to object1.  No need to go through
		 * an intermediate object.
		 */
		while (!queue_empty(&object2->memq)) {
			page = (vm_page_t) queue_first(&object2->memq);
			vm_page_rename(page, object1, page->offset, FALSE);
		}
		assert(queue_empty(&object2->memq));
	} else if (object2->phys_contiguous || queue_empty(&object2->memq)) {
		/*
		 * No pages in object2, just transfer pages
		 * from object1 to object2.  No need to go through
		 * an intermediate object.
		 */
		while (!queue_empty(&object1->memq)) {
			page = (vm_page_t) queue_first(&object1->memq);
			vm_page_rename(page, object2, page->offset, FALSE);
		}
		assert(queue_empty(&object1->memq));
	} else {
		/* transfer object1's pages to tmp_object */
		while (!queue_empty(&object1->memq)) {
			page = (vm_page_t) queue_first(&object1->memq);
			page_offset = page->offset;
			vm_page_remove(page, TRUE);
			page->offset = page_offset;
			queue_enter(&tmp_object->memq, page, vm_page_t, listq);
		}
		assert(queue_empty(&object1->memq));
		/* transfer object2's pages to object1 */
		while (!queue_empty(&object2->memq)) {
			page = (vm_page_t) queue_first(&object2->memq);
			vm_page_rename(page, object1, page->offset, FALSE);
		}
		assert(queue_empty(&object2->memq));
		/* transfer tmp_object's pages to object1 */
		while (!queue_empty(&tmp_object->memq)) {
			page = (vm_page_t) queue_first(&tmp_object->memq);
			queue_remove(&tmp_object->memq, page,
				     vm_page_t, listq);
			vm_page_insert(page, object2, page->offset);
		}
		assert(queue_empty(&tmp_object->memq));
	}

#define __TRANSPOSE_FIELD(field)				\
MACRO_BEGIN							\
	tmp_object->field = object1->field;			\
	object1->field = object2->field;			\
	object2->field = tmp_object->field;			\
MACRO_END

	/* "Lock" refers to the object not its contents */
	/* "size" should be identical */
	assert(object1->size == object2->size);
	/* "memq_hint" was updated above when transposing pages */
	/* "ref_count" refers to the object not its contents */
#if TASK_SWAPPER
	/* "res_count" refers to the object not its contents */
#endif
	/* "resident_page_count" was updated above when transposing pages */
	/* "wired_page_count" was updated above when transposing pages */
	/* "reusable_page_count" was updated above when transposing pages */
	/* there should be no "copy" */
	assert(!object1->copy);
	assert(!object2->copy);
	/* there should be no "shadow" */
	assert(!object1->shadow);
	assert(!object2->shadow);
	__TRANSPOSE_FIELD(shadow_offset); /* used by phys_contiguous objects */
	__TRANSPOSE_FIELD(pager);
	__TRANSPOSE_FIELD(paging_offset);
	__TRANSPOSE_FIELD(pager_control);
	/* update the memory_objects' pointers back to the VM objects */
	if (object1->pager_control != MEMORY_OBJECT_CONTROL_NULL) {
		memory_object_control_collapse(object1->pager_control,
					       object1);
	}
	if (object2->pager_control != MEMORY_OBJECT_CONTROL_NULL) {
		memory_object_control_collapse(object2->pager_control,
					       object2);
	}
	__TRANSPOSE_FIELD(copy_strategy);
	/* "paging_in_progress" refers to the object not its contents */
	assert(!object1->paging_in_progress);
	assert(!object2->paging_in_progress);
	assert(object1->activity_in_progress);
	assert(object2->activity_in_progress);
	/* "all_wanted" refers to the object not its contents */
	__TRANSPOSE_FIELD(pager_created);
	__TRANSPOSE_FIELD(pager_initialized);
	__TRANSPOSE_FIELD(pager_ready);
	__TRANSPOSE_FIELD(pager_trusted);
	__TRANSPOSE_FIELD(can_persist);
	__TRANSPOSE_FIELD(internal);
	__TRANSPOSE_FIELD(temporary);
	__TRANSPOSE_FIELD(private);
	__TRANSPOSE_FIELD(pageout);
	/* "alive" should be set */
	assert(object1->alive);
	assert(object2->alive);
	/* "purgeable" should be non-purgeable */
	assert(object1->purgable == VM_PURGABLE_DENY);
	assert(object2->purgable == VM_PURGABLE_DENY);
	/* "shadowed" refers to the the object not its contents */
	__TRANSPOSE_FIELD(silent_overwrite);
	__TRANSPOSE_FIELD(advisory_pageout);
	__TRANSPOSE_FIELD(true_share);
	/* "terminating" should not be set */
	assert(!object1->terminating);
	assert(!object2->terminating);
	__TRANSPOSE_FIELD(named);
	/* "shadow_severed" refers to the object not its contents */
	__TRANSPOSE_FIELD(phys_contiguous);
	__TRANSPOSE_FIELD(nophyscache);
	/* "cached_list.next" points to transposed object */
	object1->cached_list.next = (queue_entry_t) object2;
	object2->cached_list.next = (queue_entry_t) object1;
	/* "cached_list.prev" should be NULL */
	assert(object1->cached_list.prev == NULL);
	assert(object2->cached_list.prev == NULL);
	/* "msr_q" is linked to the object not its contents */
	assert(queue_empty(&object1->msr_q));
	assert(queue_empty(&object2->msr_q));
	__TRANSPOSE_FIELD(last_alloc);
	__TRANSPOSE_FIELD(sequential);
	__TRANSPOSE_FIELD(pages_created);
	__TRANSPOSE_FIELD(pages_used);
#if MACH_PAGEMAP
	__TRANSPOSE_FIELD(existence_map);
#endif
	__TRANSPOSE_FIELD(cow_hint);
#if MACH_ASSERT
	__TRANSPOSE_FIELD(paging_object);
#endif
	__TRANSPOSE_FIELD(wimg_bits);
	__TRANSPOSE_FIELD(code_signed);
	if (object1->hashed) {
		hash_lck = vm_object_hash_lock_spin(object2->pager);
		hash_entry = vm_object_hash_lookup(object2->pager, FALSE);
		assert(hash_entry != VM_OBJECT_HASH_ENTRY_NULL);
		hash_entry->object = object2;
		vm_object_hash_unlock(hash_lck);
	}
	if (object2->hashed) {
		hash_lck = vm_object_hash_lock_spin(object1->pager);
		hash_entry = vm_object_hash_lookup(object1->pager, FALSE);
		assert(hash_entry != VM_OBJECT_HASH_ENTRY_NULL);
		hash_entry->object = object1;
		vm_object_hash_unlock(hash_lck);
	}
	__TRANSPOSE_FIELD(hashed);
	object1->transposed = TRUE;
	object2->transposed = TRUE;
	__TRANSPOSE_FIELD(mapping_in_progress);
	__TRANSPOSE_FIELD(volatile_empty);
	__TRANSPOSE_FIELD(volatile_fault);
	__TRANSPOSE_FIELD(all_reusable);
	assert(object1->blocked_access);
	assert(object2->blocked_access);
	assert(object1->__object2_unused_bits == 0);
	assert(object2->__object2_unused_bits == 0);
#if UPL_DEBUG
	/* "uplq" refers to the object not its contents (see upl_transpose()) */
#endif
	assert(object1->objq.next == NULL);
	assert(object1->objq.prev == NULL);
	assert(object2->objq.next == NULL);
	assert(object2->objq.prev == NULL);

#undef __TRANSPOSE_FIELD

	retval = KERN_SUCCESS;

done:
	/*
	 * Cleanup.
	 */
	if (tmp_object != VM_OBJECT_NULL) {
		vm_object_unlock(tmp_object);
		/*
		 * Re-initialize the temporary object to avoid
		 * deallocating a real pager.
		 */
		_vm_object_allocate(transpose_size, tmp_object);
		vm_object_deallocate(tmp_object);
		tmp_object = VM_OBJECT_NULL;
	}

	if (object1_locked) {
		vm_object_unlock(object1);
		object1_locked = FALSE;
	}
	if (object2_locked) {
		vm_object_unlock(object2);
		object2_locked = FALSE;
	}

	vm_object_transpose_count++;

	return retval;
}


/*
 *      vm_object_cluster_size
 *
 *      Determine how big a cluster we should issue an I/O for...
 *
 *	Inputs:   *start == offset of page needed
 *		  *length == maximum cluster pager can handle
 *	Outputs:  *start == beginning offset of cluster
 *		  *length == length of cluster to try
 *
 *	The original *start will be encompassed by the cluster
 *
 */
extern int speculative_reads_disabled;
#if CONFIG_EMBEDDED
unsigned int preheat_pages_max = MAX_UPL_TRANSFER;
unsigned int preheat_pages_min = 8;
unsigned int preheat_pages_mult = 4;
#else
unsigned int preheat_pages_max = MAX_UPL_TRANSFER;
unsigned int preheat_pages_min = 8;
unsigned int preheat_pages_mult = 4;
#endif

uint32_t pre_heat_scaling[MAX_UPL_TRANSFER + 1];
uint32_t pre_heat_cluster[MAX_UPL_TRANSFER + 1];


__private_extern__ void
vm_object_cluster_size(vm_object_t object, vm_object_offset_t *start,
		       vm_size_t *length, vm_object_fault_info_t fault_info, uint32_t *io_streaming)
{
	vm_size_t		pre_heat_size;
	vm_size_t		tail_size;
	vm_size_t		head_size;
	vm_size_t		max_length;
	vm_size_t		cluster_size;
	vm_object_offset_t	object_size;
	vm_object_offset_t	orig_start;
	vm_object_offset_t	target_start;
	vm_object_offset_t	offset;
	vm_behavior_t		behavior;
	boolean_t		look_behind = TRUE;
	boolean_t		look_ahead  = TRUE;
	uint32_t		throttle_limit;
	int			sequential_run;
	int			sequential_behavior = VM_BEHAVIOR_SEQUENTIAL;
	unsigned int		max_ph_size;
	unsigned int		min_ph_size;
	unsigned int		ph_mult;

	assert( !(*length & PAGE_MASK));
	assert( !(*start & PAGE_MASK_64));

	if ( (ph_mult = preheat_pages_mult) < 1 ) 
		ph_mult = 1;
	if ( (min_ph_size = preheat_pages_min) < 1 ) 
		min_ph_size = 1;
	if ( (max_ph_size = preheat_pages_max) > MAX_UPL_TRANSFER ) 
		max_ph_size = MAX_UPL_TRANSFER;
	
	if ( (max_length = *length) > (max_ph_size * PAGE_SIZE) ) 
	        max_length = (max_ph_size * PAGE_SIZE);

	/*
	 * we'll always return a cluster size of at least
	 * 1 page, since the original fault must always
	 * be processed
	 */
	*length = PAGE_SIZE;
	*io_streaming = 0;

	if (speculative_reads_disabled || fault_info == NULL || max_length == 0) {
	        /*
		 * no cluster... just fault the page in
		 */
	        return;
	}
	orig_start = *start;
	target_start = orig_start;
	cluster_size = round_page(fault_info->cluster_size);
	behavior = fault_info->behavior;

	vm_object_lock(object);

	if (object->internal)
	        object_size = object->size;
	else if (object->pager != MEMORY_OBJECT_NULL)
	        vnode_pager_get_object_size(object->pager, &object_size);
	else
		goto out;	/* pager is gone for this object, nothing more to do */

	object_size = round_page_64(object_size);

	if (orig_start >= object_size) {
	        /*
		 * fault occurred beyond the EOF...
		 * we need to punt w/o changing the
		 * starting offset
		 */
	        goto out;
	}
	if (object->pages_used > object->pages_created) {
	        /*
		 * must have wrapped our 32 bit counters
		 * so reset
		 */
 	        object->pages_used = object->pages_created = 0;
	}
	if ((sequential_run = object->sequential)) {
		  if (sequential_run < 0) {
		          sequential_behavior = VM_BEHAVIOR_RSEQNTL;
			  sequential_run = 0 - sequential_run;
		  } else {
		          sequential_behavior = VM_BEHAVIOR_SEQUENTIAL;
		  }

	}
	switch(behavior) {

	default:
	        behavior = VM_BEHAVIOR_DEFAULT;

	case VM_BEHAVIOR_DEFAULT:
	        if (object->internal && fault_info->user_tag == VM_MEMORY_STACK)
		        goto out;

		if (sequential_run >= (3 * PAGE_SIZE)) {
		        pre_heat_size = sequential_run + PAGE_SIZE;

			if (sequential_behavior == VM_BEHAVIOR_SEQUENTIAL)
			        look_behind = FALSE;
			else
			        look_ahead = FALSE;

			*io_streaming = 1;
		} else {

			if (object->pages_created < 32 * ph_mult) {
			        /*
				 * prime the pump
				 */
			        pre_heat_size = PAGE_SIZE * 8 * ph_mult;
			        break;
			}
			/*
			 * Linear growth in PH size: The maximum size is max_length...
			 * this cacluation will result in a size that is neither a 
			 * power of 2 nor a multiple of PAGE_SIZE... so round
			 * it up to the nearest PAGE_SIZE boundary
			 */
			pre_heat_size = (ph_mult * (max_length * object->pages_used) / object->pages_created);
			
			if (pre_heat_size < PAGE_SIZE * min_ph_size)
				pre_heat_size = PAGE_SIZE * min_ph_size;
			else
				pre_heat_size = round_page(pre_heat_size);
		}
		break;

	case VM_BEHAVIOR_RANDOM:
	        if ((pre_heat_size = cluster_size) <= PAGE_SIZE)
		        goto out;
	        break;

	case VM_BEHAVIOR_SEQUENTIAL:
	        if ((pre_heat_size = cluster_size) == 0)
		        pre_heat_size = sequential_run + PAGE_SIZE;
		look_behind = FALSE;
		*io_streaming = 1;

	        break;

	case VM_BEHAVIOR_RSEQNTL:
	        if ((pre_heat_size = cluster_size) == 0)
		        pre_heat_size = sequential_run + PAGE_SIZE;
		look_ahead = FALSE;
		*io_streaming = 1;

	        break;

	}
	throttle_limit = (uint32_t) max_length;
	assert(throttle_limit == max_length);

	if (vnode_pager_check_hard_throttle(object->pager, &throttle_limit, *io_streaming) == KERN_SUCCESS) {
		if (max_length > throttle_limit)
			max_length = throttle_limit;
	}
	if (pre_heat_size > max_length)
	        pre_heat_size = max_length;

	if (behavior == VM_BEHAVIOR_DEFAULT) {
		if (vm_page_free_count < vm_page_throttle_limit)
			pre_heat_size = trunc_page(pre_heat_size / 8);
		else if (vm_page_free_count < vm_page_free_target)
			pre_heat_size = trunc_page(pre_heat_size / 2);

		if (pre_heat_size <= PAGE_SIZE)
			goto out;
	}
	if (look_ahead == TRUE) {
	        if (look_behind == TRUE) { 
			/*
			 * if we get here its due to a random access... 
			 * so we want to center the original fault address
			 * within the cluster we will issue... make sure
			 * to calculate 'head_size' as a multiple of PAGE_SIZE...
			 * 'pre_heat_size' is a multiple of PAGE_SIZE but not
			 * necessarily an even number of pages so we need to truncate
			 * the result to a PAGE_SIZE boundary
			 */
			head_size = trunc_page(pre_heat_size / 2);

			if (target_start > head_size)
				target_start -= head_size;
			else
				target_start = 0;

			/*
			 * 'target_start' at this point represents the beginning offset
			 * of the cluster we are considering... 'orig_start' will be in
			 * the center of this cluster if we didn't have to clip the start
			 * due to running into the start of the file
			 */
		}
	        if ((target_start + pre_heat_size) > object_size)
		        pre_heat_size = (vm_size_t)(round_page_64(object_size - target_start));
		/*
		 * at this point caclulate the number of pages beyond the original fault
		 * address that we want to consider... this is guaranteed not to extend beyond
		 * the current EOF...
		 */
		assert((vm_size_t)(orig_start - target_start) == (orig_start - target_start));
	        tail_size = pre_heat_size - (vm_size_t)(orig_start - target_start) - PAGE_SIZE;
	} else {
	        if (pre_heat_size > target_start)
	                pre_heat_size = (vm_size_t) target_start; /* XXX: 32-bit vs 64-bit ? Joe ? */
		tail_size = 0;
	}
	assert( !(target_start & PAGE_MASK_64));
	assert( !(pre_heat_size & PAGE_MASK));

	pre_heat_scaling[pre_heat_size / PAGE_SIZE]++;

	if (pre_heat_size <= PAGE_SIZE)
	        goto out;

	if (look_behind == TRUE) {
	        /*
		 * take a look at the pages before the original
		 * faulting offset... recalculate this in case
		 * we had to clip 'pre_heat_size' above to keep 
		 * from running past the EOF.
		 */
	        head_size = pre_heat_size - tail_size - PAGE_SIZE;

	        for (offset = orig_start - PAGE_SIZE_64; head_size; offset -= PAGE_SIZE_64, head_size -= PAGE_SIZE) {
		        /*
			 * don't poke below the lowest offset 
			 */
		        if (offset < fault_info->lo_offset)
			        break;
		        /*
			 * for external objects and internal objects w/o an existence map
			 * vm_externl_state_get will return VM_EXTERNAL_STATE_UNKNOWN
			 */
#if MACH_PAGEMAP
		        if (vm_external_state_get(object->existence_map, offset) == VM_EXTERNAL_STATE_ABSENT) {
			        /*
				 * we know for a fact that the pager can't provide the page
				 * so don't include it or any pages beyond it in this cluster
				 */
			        break;
			}
#endif
			if (vm_page_lookup(object, offset) != VM_PAGE_NULL) {
			        /*
				 * don't bridge resident pages
				 */
			        break;
			}
			*start = offset;
			*length += PAGE_SIZE;
		}
	}
	if (look_ahead == TRUE) {
	        for (offset = orig_start + PAGE_SIZE_64; tail_size; offset += PAGE_SIZE_64, tail_size -= PAGE_SIZE) {
		        /*
			 * don't poke above the highest offset 
			 */
		        if (offset >= fault_info->hi_offset)
			        break;
			assert(offset < object_size);

		        /*
			 * for external objects and internal objects w/o an existence map
			 * vm_externl_state_get will return VM_EXTERNAL_STATE_UNKNOWN
			 */
#if MACH_PAGEMAP
		        if (vm_external_state_get(object->existence_map, offset) == VM_EXTERNAL_STATE_ABSENT) {
			        /*
				 * we know for a fact that the pager can't provide the page
				 * so don't include it or any pages beyond it in this cluster
				 */
			        break;
			}
#endif
			if (vm_page_lookup(object, offset) != VM_PAGE_NULL) {
			        /*
				 * don't bridge resident pages
				 */
			        break;
			}
			*length += PAGE_SIZE;
		}
	}
out:
	if (*length > max_length)
		*length = max_length;

	pre_heat_cluster[*length / PAGE_SIZE]++;

	vm_object_unlock(object);
}


/*
 * Allow manipulation of individual page state.  This is actually part of
 * the UPL regimen but takes place on the VM object rather than on a UPL
 */

kern_return_t
vm_object_page_op(
	vm_object_t		object,
	vm_object_offset_t	offset,
	int			ops,
	ppnum_t			*phys_entry,
	int			*flags)
{
	vm_page_t		dst_page;

	vm_object_lock(object);

	if(ops & UPL_POP_PHYSICAL) {
		if(object->phys_contiguous) {
			if (phys_entry) {
				*phys_entry = (ppnum_t)
					(object->shadow_offset >> PAGE_SHIFT);
			}
			vm_object_unlock(object);
			return KERN_SUCCESS;
		} else {
			vm_object_unlock(object);
			return KERN_INVALID_OBJECT;
		}
	}
	if(object->phys_contiguous) {
		vm_object_unlock(object);
		return KERN_INVALID_OBJECT;
	}

	while(TRUE) {
		if((dst_page = vm_page_lookup(object,offset)) == VM_PAGE_NULL) {
			vm_object_unlock(object);
			return KERN_FAILURE;
		}

		/* Sync up on getting the busy bit */
		if((dst_page->busy || dst_page->cleaning) && 
			   (((ops & UPL_POP_SET) && 
			   (ops & UPL_POP_BUSY)) || (ops & UPL_POP_DUMP))) {
			/* someone else is playing with the page, we will */
			/* have to wait */
			PAGE_SLEEP(object, dst_page, THREAD_UNINT);
			continue;
		}

		if (ops & UPL_POP_DUMP) {
			if (dst_page->pmapped == TRUE)
			        pmap_disconnect(dst_page->phys_page);

			VM_PAGE_FREE(dst_page);
			break;
		}

		if (flags) {
		        *flags = 0;

			/* Get the condition of flags before requested ops */
			/* are undertaken */

			if(dst_page->dirty) *flags |= UPL_POP_DIRTY;
			if(dst_page->pageout) *flags |= UPL_POP_PAGEOUT;
			if(dst_page->precious) *flags |= UPL_POP_PRECIOUS;
			if(dst_page->absent) *flags |= UPL_POP_ABSENT;
			if(dst_page->busy) *flags |= UPL_POP_BUSY;
		}

		/* The caller should have made a call either contingent with */
		/* or prior to this call to set UPL_POP_BUSY */
		if(ops & UPL_POP_SET) {
			/* The protection granted with this assert will */
			/* not be complete.  If the caller violates the */
			/* convention and attempts to change page state */
			/* without first setting busy we may not see it */
			/* because the page may already be busy.  However */
			/* if such violations occur we will assert sooner */
			/* or later. */
			assert(dst_page->busy || (ops & UPL_POP_BUSY));
			if (ops & UPL_POP_DIRTY) dst_page->dirty = TRUE;
			if (ops & UPL_POP_PAGEOUT) dst_page->pageout = TRUE;
			if (ops & UPL_POP_PRECIOUS) dst_page->precious = TRUE;
			if (ops & UPL_POP_ABSENT) dst_page->absent = TRUE;
			if (ops & UPL_POP_BUSY) dst_page->busy = TRUE;
		}

		if(ops & UPL_POP_CLR) {
			assert(dst_page->busy);
			if (ops & UPL_POP_DIRTY) dst_page->dirty = FALSE;
			if (ops & UPL_POP_PAGEOUT) dst_page->pageout = FALSE;
			if (ops & UPL_POP_PRECIOUS) dst_page->precious = FALSE;
			if (ops & UPL_POP_ABSENT) dst_page->absent = FALSE;
			if (ops & UPL_POP_BUSY) {
			        dst_page->busy = FALSE;
				PAGE_WAKEUP(dst_page);
			}
		}

		if (dst_page->encrypted) {
			/*
			 * ENCRYPTED SWAP:
			 * We need to decrypt this encrypted page before the
			 * caller can access its contents.
			 * But if the caller really wants to access the page's
			 * contents, they have to keep the page "busy".
			 * Otherwise, the page could get recycled or re-encrypted
			 * at any time.
			 */
			if ((ops & UPL_POP_SET) && (ops & UPL_POP_BUSY) &&
			    dst_page->busy) {
				/*
				 * The page is stable enough to be accessed by
				 * the caller, so make sure its contents are
				 * not encrypted.
				 */
				vm_page_decrypt(dst_page, 0);
			} else {
				/*
				 * The page is not busy, so don't bother
				 * decrypting it, since anything could
				 * happen to it between now and when the
				 * caller wants to access it.
				 * We should not give the caller access
				 * to this page.
				 */
				assert(!phys_entry);
			}
		}

		if (phys_entry) {
			/*
			 * The physical page number will remain valid
			 * only if the page is kept busy.
			 * ENCRYPTED SWAP: make sure we don't let the
			 * caller access an encrypted page.
			 */
			assert(dst_page->busy);
			assert(!dst_page->encrypted);
			*phys_entry = dst_page->phys_page;
		}

		break;
	}

	vm_object_unlock(object);
	return KERN_SUCCESS;
				
}

/*
 * vm_object_range_op offers performance enhancement over 
 * vm_object_page_op for page_op functions which do not require page 
 * level state to be returned from the call.  Page_op was created to provide 
 * a low-cost alternative to page manipulation via UPLs when only a single 
 * page was involved.  The range_op call establishes the ability in the _op 
 * family of functions to work on multiple pages where the lack of page level
 * state handling allows the caller to avoid the overhead of the upl structures.
 */

kern_return_t
vm_object_range_op(
	vm_object_t		object,
	vm_object_offset_t	offset_beg,
	vm_object_offset_t	offset_end,
	int                     ops,
	uint32_t		*range)
{
        vm_object_offset_t	offset;
	vm_page_t		dst_page;

	if (offset_end - offset_beg > (uint32_t) -1) {
		/* range is too big and would overflow "*range" */
		return KERN_INVALID_ARGUMENT;
	} 
	if (object->resident_page_count == 0) {
	        if (range) {
		        if (ops & UPL_ROP_PRESENT) {
			        *range = 0;
			} else {
			        *range = (uint32_t) (offset_end - offset_beg);
				assert(*range == (offset_end - offset_beg));
			}
		}
		return KERN_SUCCESS;
	}
	vm_object_lock(object);

	if (object->phys_contiguous) {
		vm_object_unlock(object);
	        return KERN_INVALID_OBJECT;
	}
	
	offset = offset_beg & ~PAGE_MASK_64;

	while (offset < offset_end) {
		dst_page = vm_page_lookup(object, offset);
		if (dst_page != VM_PAGE_NULL) {
			if (ops & UPL_ROP_DUMP) {
				if (dst_page->busy || dst_page->cleaning) {
				        /*
					 * someone else is playing with the 
					 * page, we will have to wait
					 */
				        PAGE_SLEEP(object, dst_page, THREAD_UNINT);
					/*
					 * need to relook the page up since it's
					 * state may have changed while we slept
					 * it might even belong to a different object
					 * at this point
					 */
					continue;
				}
				if (dst_page->pmapped == TRUE)
				        pmap_disconnect(dst_page->phys_page);

				VM_PAGE_FREE(dst_page);

			} else if ((ops & UPL_ROP_ABSENT) && !dst_page->absent)
			        break;
		} else if (ops & UPL_ROP_PRESENT)
		        break;

		offset += PAGE_SIZE;
	}
	vm_object_unlock(object);

	if (range) {
	        if (offset > offset_end)
		        offset = offset_end;
		if(offset > offset_beg) {
			*range = (uint32_t) (offset - offset_beg);
			assert(*range == (offset - offset_beg));
		} else {
			*range = 0;
		}
	}
	return KERN_SUCCESS;
}


uint32_t scan_object_collision = 0;

void
vm_object_lock(vm_object_t object)
{
        if (object == vm_pageout_scan_wants_object) {
	        scan_object_collision++;
	        mutex_pause(2);
	}
        lck_rw_lock_exclusive(&object->Lock);
}

boolean_t
vm_object_lock_avoid(vm_object_t object)
{
        if (object == vm_pageout_scan_wants_object) {
	        scan_object_collision++;
		return TRUE;
	}
	return FALSE;
}

boolean_t
_vm_object_lock_try(vm_object_t object)
{
	return (lck_rw_try_lock_exclusive(&object->Lock));
}

boolean_t
vm_object_lock_try(vm_object_t object)
{
    // called from hibernate path so check before blocking
	if (vm_object_lock_avoid(object) && ml_get_interrupts_enabled()) {
		mutex_pause(2);
	}
	return _vm_object_lock_try(object);
}
void
vm_object_lock_shared(vm_object_t object)
{
        if (vm_object_lock_avoid(object)) {
	        mutex_pause(2);
	}
	lck_rw_lock_shared(&object->Lock);
}

boolean_t
vm_object_lock_try_shared(vm_object_t object)
{
        if (vm_object_lock_avoid(object)) {
	        mutex_pause(2);
	}
	return (lck_rw_try_lock_shared(&object->Lock));
}

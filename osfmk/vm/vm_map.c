/*
 * Copyright (c) 2000-2005 Apple Computer, Inc. All rights reserved.
 *
 * @APPLE_LICENSE_HEADER_START@
 * 
 * The contents of this file constitute Original Code as defined in and
 * are subject to the Apple Public Source License Version 1.1 (the
 * "License").  You may not use this file except in compliance with the
 * License.  Please obtain a copy of the License at
 * http://www.apple.com/publicsource and read it before using this file.
 * 
 * This Original Code and all software distributed under the License are
 * distributed on an "AS IS" basis, WITHOUT WARRANTY OF ANY KIND, EITHER
 * EXPRESS OR IMPLIED, AND APPLE HEREBY DISCLAIMS ALL SUCH WARRANTIES,
 * INCLUDING WITHOUT LIMITATION, ANY WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE OR NON-INFRINGEMENT.  Please see the
 * License for the specific language governing rights and limitations
 * under the License.
 * 
 * @APPLE_LICENSE_HEADER_END@
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
 *	File:	vm/vm_map.c
 *	Author:	Avadis Tevanian, Jr., Michael Wayne Young
 *	Date:	1985
 *
 *	Virtual memory mapping module.
 */

#include <task_swapper.h>
#include <mach_assert.h>
#include <libkern/OSAtomic.h>

#include <mach/kern_return.h>
#include <mach/port.h>
#include <mach/vm_attributes.h>
#include <mach/vm_param.h>
#include <mach/vm_behavior.h>
#include <mach/vm_statistics.h>
#include <mach/memory_object.h>
#include <machine/cpu_capabilities.h>

#include <kern/assert.h>
#include <kern/counters.h>
#include <kern/kalloc.h>
#include <kern/zalloc.h>

#include <vm/cpm.h>
#include <vm/vm_init.h>
#include <vm/vm_fault.h>
#include <vm/vm_map.h>
#include <vm/vm_object.h>
#include <vm/vm_page.h>
#include <vm/vm_kern.h>
#include <ipc/ipc_port.h>
#include <kern/sched_prim.h>
#include <kern/misc_protos.h>
#include <ddb/tr.h>
#include <machine/db_machdep.h>
#include <kern/xpr.h>

#include <mach/vm_map_server.h>
#include <mach/mach_host_server.h>
#include <vm/vm_shared_memory_server.h>
#include <vm/vm_protos.h>       // for vm_map_commpage64 and vm_map_remove_compage64

#ifdef ppc
#include <ppc/mappings.h>
#endif /* ppc */

#include <vm/vm_protos.h>

/* Internal prototypes
 */
 
static void vm_map_simplify_range(
	vm_map_t	map,
	vm_map_offset_t	start,
	vm_map_offset_t	end);	/* forward */

static boolean_t	vm_map_range_check(
				vm_map_t	map,
				vm_map_offset_t	start,
				vm_map_offset_t	end,
				vm_map_entry_t	*entry);

static vm_map_entry_t	_vm_map_entry_create(
				struct vm_map_header	*map_header);

static void		_vm_map_entry_dispose(
				struct vm_map_header	*map_header,
				vm_map_entry_t		entry);

static void		vm_map_pmap_enter(
				vm_map_t		map,
				vm_map_offset_t 	addr,
				vm_map_offset_t		end_addr,
				vm_object_t 		object,
				vm_object_offset_t	offset,
				vm_prot_t		protection);

static void		_vm_map_clip_end(
				struct vm_map_header	*map_header,
				vm_map_entry_t		entry,
				vm_map_offset_t		end);

static void		_vm_map_clip_start(
				struct vm_map_header	*map_header,
				vm_map_entry_t		entry,
				vm_map_offset_t		start);

static void		vm_map_entry_delete(
				vm_map_t	map,
				vm_map_entry_t	entry);

static kern_return_t	vm_map_delete(
				vm_map_t	map,
				vm_map_offset_t	start,
				vm_map_offset_t	end,
				int		flags,
				vm_map_t	zap_map);

static kern_return_t	vm_map_copy_overwrite_unaligned(
				vm_map_t	dst_map,
				vm_map_entry_t	entry,
				vm_map_copy_t	copy,
				vm_map_address_t start);

static kern_return_t	vm_map_copy_overwrite_aligned(
				vm_map_t	dst_map,
				vm_map_entry_t	tmp_entry,
				vm_map_copy_t	copy,
				vm_map_offset_t start,
				pmap_t		pmap);

static kern_return_t	vm_map_copyin_kernel_buffer(
				vm_map_t	src_map,
				vm_map_address_t src_addr,
				vm_map_size_t	len,
				boolean_t	src_destroy,
				vm_map_copy_t	*copy_result);  /* OUT */

static kern_return_t	vm_map_copyout_kernel_buffer(
				vm_map_t	map,
				vm_map_address_t *addr,	/* IN/OUT */
				vm_map_copy_t	copy,
				boolean_t	overwrite);

static void		vm_map_fork_share(
				vm_map_t	old_map,
				vm_map_entry_t	old_entry,
				vm_map_t	new_map);

static boolean_t	vm_map_fork_copy(
				vm_map_t	old_map,
				vm_map_entry_t	*old_entry_p,
				vm_map_t	new_map);

static void		vm_map_region_top_walk(
        			vm_map_entry_t		   entry,
				vm_region_top_info_t       top);

static void		vm_map_region_walk(
				vm_map_t		   map,
				vm_map_offset_t		   va,
        			vm_map_entry_t		   entry,
				vm_object_offset_t	   offset,
				vm_object_size_t	   range,
				vm_region_extended_info_t  extended);

static kern_return_t	vm_map_wire_nested(
				vm_map_t		   map,
				vm_map_offset_t		   start,
				vm_map_offset_t		   end,
				vm_prot_t		   access_type,
				boolean_t		   user_wire,
				pmap_t			   map_pmap, 
				vm_map_offset_t		   pmap_addr);

static kern_return_t	vm_map_unwire_nested(
				vm_map_t		   map,
				vm_map_offset_t		   start,
				vm_map_offset_t		   end,
				boolean_t		   user_wire,
				pmap_t			   map_pmap,
				vm_map_offset_t		   pmap_addr);

static kern_return_t	vm_map_overwrite_submap_recurse(
				vm_map_t		   dst_map,
				vm_map_offset_t		   dst_addr,
				vm_map_size_t		   dst_size);

static kern_return_t	vm_map_copy_overwrite_nested(
				vm_map_t		   dst_map,
				vm_map_offset_t		   dst_addr,
				vm_map_copy_t		   copy,
				boolean_t		   interruptible,
				pmap_t			   pmap);

static kern_return_t	vm_map_remap_extract(
				vm_map_t		map,
				vm_map_offset_t		addr,
				vm_map_size_t		size,
				boolean_t		copy,
				struct vm_map_header 	*map_header,
				vm_prot_t		*cur_protection,
				vm_prot_t		*max_protection,
				vm_inherit_t		inheritance,
				boolean_t		pageable);

static kern_return_t	vm_map_remap_range_allocate(
				vm_map_t		map,
				vm_map_address_t	*address,
				vm_map_size_t		size,
				vm_map_offset_t		mask,
				boolean_t		anywhere,
				vm_map_entry_t		*map_entry);

static void		vm_map_region_look_for_page(
				vm_map_t		   map,
				vm_map_offset_t            va,
				vm_object_t		   object,
				vm_object_offset_t	   offset,
				int                        max_refcnt,
				int                        depth,
				vm_region_extended_info_t  extended);

static int		vm_map_region_count_obj_refs(
				vm_map_entry_t    	   entry,
				vm_object_t       	   object);

/*
 * Macros to copy a vm_map_entry. We must be careful to correctly
 * manage the wired page count. vm_map_entry_copy() creates a new
 * map entry to the same memory - the wired count in the new entry
 * must be set to zero. vm_map_entry_copy_full() creates a new
 * entry that is identical to the old entry.  This preserves the
 * wire count; it's used for map splitting and zone changing in
 * vm_map_copyout.
 */
#define vm_map_entry_copy(NEW,OLD) \
MACRO_BEGIN                                     \
                *(NEW) = *(OLD);                \
                (NEW)->is_shared = FALSE;	\
                (NEW)->needs_wakeup = FALSE;    \
                (NEW)->in_transition = FALSE;   \
                (NEW)->wired_count = 0;         \
                (NEW)->user_wired_count = 0;    \
MACRO_END

#define vm_map_entry_copy_full(NEW,OLD)        (*(NEW) = *(OLD))

/*
 *	Virtual memory maps provide for the mapping, protection,
 *	and sharing of virtual memory objects.  In addition,
 *	this module provides for an efficient virtual copy of
 *	memory from one map to another.
 *
 *	Synchronization is required prior to most operations.
 *
 *	Maps consist of an ordered doubly-linked list of simple
 *	entries; a single hint is used to speed up lookups.
 *
 *	Sharing maps have been deleted from this version of Mach.
 *	All shared objects are now mapped directly into the respective
 *	maps.  This requires a change in the copy on write strategy;
 *	the asymmetric (delayed) strategy is used for shared temporary
 *	objects instead of the symmetric (shadow) strategy.  All maps
 *	are now "top level" maps (either task map, kernel map or submap
 *	of the kernel map).  
 *
 *	Since portions of maps are specified by start/end addreses,
 *	which may not align with existing map entries, all
 *	routines merely "clip" entries to these start/end values.
 *	[That is, an entry is split into two, bordering at a
 *	start or end value.]  Note that these clippings may not
 *	always be necessary (as the two resulting entries are then
 *	not changed); however, the clipping is done for convenience.
 *	No attempt is currently made to "glue back together" two
 *	abutting entries.
 *
 *	The symmetric (shadow) copy strategy implements virtual copy
 *	by copying VM object references from one map to
 *	another, and then marking both regions as copy-on-write.
 *	It is important to note that only one writeable reference
 *	to a VM object region exists in any map when this strategy
 *	is used -- this means that shadow object creation can be
 *	delayed until a write operation occurs.  The symmetric (delayed)
 *	strategy allows multiple maps to have writeable references to
 *	the same region of a vm object, and hence cannot delay creating
 *	its copy objects.  See vm_object_copy_quickly() in vm_object.c.
 *	Copying of permanent objects is completely different; see
 *	vm_object_copy_strategically() in vm_object.c.
 */

static zone_t	vm_map_zone;		/* zone for vm_map structures */
static zone_t	vm_map_entry_zone;	/* zone for vm_map_entry structures */
static zone_t	vm_map_kentry_zone;	/* zone for kernel entry structures */
static zone_t	vm_map_copy_zone;	/* zone for vm_map_copy structures */


/*
 *	Placeholder object for submap operations.  This object is dropped
 *	into the range by a call to vm_map_find, and removed when
 *	vm_map_submap creates the submap.
 */

vm_object_t	vm_submap_object;

/*
 *	vm_map_init:
 *
 *	Initialize the vm_map module.  Must be called before
 *	any other vm_map routines.
 *
 *	Map and entry structures are allocated from zones -- we must
 *	initialize those zones.
 *
 *	There are three zones of interest:
 *
 *	vm_map_zone:		used to allocate maps.
 *	vm_map_entry_zone:	used to allocate map entries.
 *	vm_map_kentry_zone:	used to allocate map entries for the kernel.
 *
 *	The kernel allocates map entries from a special zone that is initially
 *	"crammed" with memory.  It would be difficult (perhaps impossible) for
 *	the kernel to allocate more memory to a entry zone when it became
 *	empty since the very act of allocating memory implies the creation
 *	of a new entry.
 */

static void		*map_data;
static vm_map_size_t	map_data_size;
static void		*kentry_data;
static vm_map_size_t	kentry_data_size;
static int		kentry_count = 2048;		/* to init kentry_data_size */

#define         NO_COALESCE_LIMIT  (1024 * 128)

/*
 *	Threshold for aggressive (eager) page map entering for vm copyout
 *	operations.  Any copyout larger will NOT be aggressively entered.
 */
static vm_map_size_t vm_map_aggressive_enter_max;		/* set by bootstrap */

/* Skip acquiring locks if we're in the midst of a kernel core dump */
extern unsigned int not_in_kdp;

void
vm_map_init(
	void)
{
	vm_map_zone = zinit((vm_map_size_t) sizeof(struct vm_map), 40*1024,
					PAGE_SIZE, "maps");

	vm_map_entry_zone = zinit((vm_map_size_t) sizeof(struct vm_map_entry),
					1024*1024, PAGE_SIZE*5,
					"non-kernel map entries");

	vm_map_kentry_zone = zinit((vm_map_size_t) sizeof(struct vm_map_entry),
					kentry_data_size, kentry_data_size,
					"kernel map entries");

	vm_map_copy_zone = zinit((vm_map_size_t) sizeof(struct vm_map_copy),
					16*1024, PAGE_SIZE, "map copies");

	/*
	 *	Cram the map and kentry zones with initial data.
	 *	Set kentry_zone non-collectible to aid zone_gc().
	 */
	zone_change(vm_map_zone, Z_COLLECT, FALSE);
	zone_change(vm_map_kentry_zone, Z_COLLECT, FALSE);
	zone_change(vm_map_kentry_zone, Z_EXPAND, FALSE);
	zcram(vm_map_zone, map_data, map_data_size);
	zcram(vm_map_kentry_zone, kentry_data, kentry_data_size);
}

void
vm_map_steal_memory(
	void)
{
	map_data_size = vm_map_round_page(10 * sizeof(struct vm_map));
	map_data = pmap_steal_memory(map_data_size);

#if 0
	/*
	 * Limiting worst case: vm_map_kentry_zone needs to map each "available"
	 * physical page (i.e. that beyond the kernel image and page tables)
	 * individually; we guess at most one entry per eight pages in the
	 * real world. This works out to roughly .1 of 1% of physical memory,
	 * or roughly 1900 entries (64K) for a 64M machine with 4K pages.
	 */
#endif
	kentry_count = pmap_free_pages() / 8;


	kentry_data_size =
		vm_map_round_page(kentry_count * sizeof(struct vm_map_entry));
	kentry_data = pmap_steal_memory(kentry_data_size);
}

/*
 *	vm_map_create:
 *
 *	Creates and returns a new empty VM map with
 *	the given physical map structure, and having
 *	the given lower and upper address bounds.
 */
vm_map_t
vm_map_create(
	pmap_t			pmap,
	vm_map_offset_t	min,
	vm_map_offset_t	max,
	boolean_t		pageable)
{
	register vm_map_t	result;

	result = (vm_map_t) zalloc(vm_map_zone);
	if (result == VM_MAP_NULL)
		panic("vm_map_create");

	vm_map_first_entry(result) = vm_map_to_entry(result);
	vm_map_last_entry(result)  = vm_map_to_entry(result);
	result->hdr.nentries = 0;
	result->hdr.entries_pageable = pageable;

	result->size = 0;
	result->ref_count = 1;
#if	TASK_SWAPPER
	result->res_count = 1;
	result->sw_state = MAP_SW_IN;
#endif	/* TASK_SWAPPER */
	result->pmap = pmap;
	result->min_offset = min;
	result->max_offset = max;
	result->wiring_required = FALSE;
	result->no_zero_fill = FALSE;
	result->mapped = FALSE;
	result->wait_for_space = FALSE;
	result->first_free = vm_map_to_entry(result);
	result->hint = vm_map_to_entry(result);
	vm_map_lock_init(result);
	mutex_init(&result->s_lock, 0);

	return(result);
}

/*
 *	vm_map_entry_create:	[ internal use only ]
 *
 *	Allocates a VM map entry for insertion in the
 *	given map (or map copy).  No fields are filled.
 */
#define	vm_map_entry_create(map) \
	    _vm_map_entry_create(&(map)->hdr)

#define	vm_map_copy_entry_create(copy) \
	    _vm_map_entry_create(&(copy)->cpy_hdr)

static vm_map_entry_t
_vm_map_entry_create(
	register struct vm_map_header	*map_header)
{
	register zone_t	zone;
	register vm_map_entry_t	entry;

	if (map_header->entries_pageable)
	    zone = vm_map_entry_zone;
	else
	    zone = vm_map_kentry_zone;

	entry = (vm_map_entry_t) zalloc(zone);
	if (entry == VM_MAP_ENTRY_NULL)
		panic("vm_map_entry_create");

	return(entry);
}

/*
 *	vm_map_entry_dispose:	[ internal use only ]
 *
 *	Inverse of vm_map_entry_create.
 */
#define	vm_map_entry_dispose(map, entry)			\
MACRO_BEGIN							\
	if((entry) == (map)->first_free)			\
		(map)->first_free = vm_map_to_entry(map);	\
	if((entry) == (map)->hint)				\
		(map)->hint = vm_map_to_entry(map);		\
	_vm_map_entry_dispose(&(map)->hdr, (entry));		\
MACRO_END

#define	vm_map_copy_entry_dispose(map, entry) \
	_vm_map_entry_dispose(&(copy)->cpy_hdr, (entry))

static void
_vm_map_entry_dispose(
	register struct vm_map_header	*map_header,
	register vm_map_entry_t		entry)
{
	register zone_t		zone;

	if (map_header->entries_pageable)
	    zone = vm_map_entry_zone;
	else
	    zone = vm_map_kentry_zone;

	zfree(zone, entry);
}

#if MACH_ASSERT
static boolean_t first_free_is_valid(vm_map_t map);	/* forward */
static boolean_t first_free_check = FALSE;
static boolean_t
first_free_is_valid(
	vm_map_t	map)
{
	vm_map_entry_t	entry, next;

	if (!first_free_check)
		return TRUE;
		
	entry = vm_map_to_entry(map);
	next = entry->vme_next;
	while (vm_map_trunc_page(next->vme_start) == vm_map_trunc_page(entry->vme_end) ||
	       (vm_map_trunc_page(next->vme_start) == vm_map_trunc_page(entry->vme_start) &&
		next != vm_map_to_entry(map))) {
		entry = next;
		next = entry->vme_next;
		if (entry == vm_map_to_entry(map))
			break;
	}
	if (map->first_free != entry) {
		printf("Bad first_free for map 0x%x: 0x%x should be 0x%x\n",
		       map, map->first_free, entry);
		return FALSE;
	}
	return TRUE;
}
#endif /* MACH_ASSERT */

/*
 *	UPDATE_FIRST_FREE:
 *
 *	Updates the map->first_free pointer to the
 *	entry immediately before the first hole in the map.
 * 	The map should be locked.
 */
#define UPDATE_FIRST_FREE(map, new_first_free) 				\
MACRO_BEGIN 								\
	vm_map_t	UFF_map; 					\
	vm_map_entry_t	UFF_first_free; 				\
	vm_map_entry_t	UFF_next_entry; 				\
	UFF_map = (map); 						\
	UFF_first_free = (new_first_free);				\
	UFF_next_entry = UFF_first_free->vme_next; 			\
	while (vm_map_trunc_page(UFF_next_entry->vme_start) == 		\
	       vm_map_trunc_page(UFF_first_free->vme_end) || 			\
	       (vm_map_trunc_page(UFF_next_entry->vme_start) == 		\
		vm_map_trunc_page(UFF_first_free->vme_start) &&		\
		UFF_next_entry != vm_map_to_entry(UFF_map))) { 		\
		UFF_first_free = UFF_next_entry; 			\
		UFF_next_entry = UFF_first_free->vme_next; 		\
		if (UFF_first_free == vm_map_to_entry(UFF_map)) 	\
			break; 						\
	} 								\
	UFF_map->first_free = UFF_first_free; 				\
	assert(first_free_is_valid(UFF_map));				\
MACRO_END

/*
 *	vm_map_entry_{un,}link:
 *
 *	Insert/remove entries from maps (or map copies).
 */
#define vm_map_entry_link(map, after_where, entry)			\
MACRO_BEGIN 								\
	vm_map_t VMEL_map; 						\
	vm_map_entry_t VMEL_entry; 					\
	VMEL_map = (map);						\
	VMEL_entry = (entry); 						\
	_vm_map_entry_link(&VMEL_map->hdr, after_where, VMEL_entry); 	\
	UPDATE_FIRST_FREE(VMEL_map, VMEL_map->first_free); 		\
MACRO_END


#define vm_map_copy_entry_link(copy, after_where, entry)		\
	_vm_map_entry_link(&(copy)->cpy_hdr, after_where, (entry))

#define _vm_map_entry_link(hdr, after_where, entry)			\
	MACRO_BEGIN							\
	(hdr)->nentries++;						\
	(entry)->vme_prev = (after_where);				\
	(entry)->vme_next = (after_where)->vme_next;			\
	(entry)->vme_prev->vme_next = (entry)->vme_next->vme_prev = (entry); \
	MACRO_END

#define vm_map_entry_unlink(map, entry)					\
MACRO_BEGIN 								\
	vm_map_t VMEU_map; 						\
	vm_map_entry_t VMEU_entry; 					\
	vm_map_entry_t VMEU_first_free;					\
	VMEU_map = (map); 						\
	VMEU_entry = (entry); 						\
	if (VMEU_entry->vme_start <= VMEU_map->first_free->vme_start)	\
		VMEU_first_free = VMEU_entry->vme_prev;			\
	else								\
		VMEU_first_free = VMEU_map->first_free;			\
	_vm_map_entry_unlink(&VMEU_map->hdr, VMEU_entry); 		\
	UPDATE_FIRST_FREE(VMEU_map, VMEU_first_free);			\
MACRO_END

#define vm_map_copy_entry_unlink(copy, entry)				\
	_vm_map_entry_unlink(&(copy)->cpy_hdr, (entry))

#define _vm_map_entry_unlink(hdr, entry)				\
	MACRO_BEGIN							\
	(hdr)->nentries--;						\
	(entry)->vme_next->vme_prev = (entry)->vme_prev; 		\
	(entry)->vme_prev->vme_next = (entry)->vme_next; 		\
	MACRO_END

#if	MACH_ASSERT && TASK_SWAPPER
/*
 *	vm_map_res_reference:
 *
 *	Adds another valid residence count to the given map.
 *
 *	Map is locked so this function can be called from
 *	vm_map_swapin.
 *
 */
void vm_map_res_reference(register vm_map_t map)
{
	/* assert map is locked */
	assert(map->res_count >= 0);
	assert(map->ref_count >= map->res_count);
	if (map->res_count == 0) {
		mutex_unlock(&map->s_lock);
		vm_map_lock(map);
		vm_map_swapin(map);
		mutex_lock(&map->s_lock);
		++map->res_count;
		vm_map_unlock(map);
	} else
		++map->res_count;
}

/*
 *	vm_map_reference_swap:
 *
 *	Adds valid reference and residence counts to the given map.
 *
 *	The map may not be in memory (i.e. zero residence count).
 *
 */
void vm_map_reference_swap(register vm_map_t map)
{
	assert(map != VM_MAP_NULL);
	mutex_lock(&map->s_lock);
	assert(map->res_count >= 0);
	assert(map->ref_count >= map->res_count);
	map->ref_count++;
	vm_map_res_reference(map);
	mutex_unlock(&map->s_lock);
}

/*
 *	vm_map_res_deallocate:
 *
 *	Decrement residence count on a map; possibly causing swapout.
 *
 *	The map must be in memory (i.e. non-zero residence count).
 *
 *	The map is locked, so this function is callable from vm_map_deallocate.
 *
 */
void vm_map_res_deallocate(register vm_map_t map)
{
	assert(map->res_count > 0);
	if (--map->res_count == 0) {
		mutex_unlock(&map->s_lock);
		vm_map_lock(map);
		vm_map_swapout(map);
		vm_map_unlock(map);
		mutex_lock(&map->s_lock);
	}
	assert(map->ref_count >= map->res_count);
}
#endif	/* MACH_ASSERT && TASK_SWAPPER */

/*
 *	vm_map_destroy:
 *
 *	Actually destroy a map.
 */
void
vm_map_destroy(
	register vm_map_t	map)
{	
	vm_map_lock(map);
	(void) vm_map_delete(map, map->min_offset,
			     map->max_offset, VM_MAP_NO_FLAGS,
			     VM_MAP_NULL);
	vm_map_unlock(map);
	
#ifdef __PPC__
	if (map->hdr.nentries!=0)
		vm_map_remove_commpage64(map);
#endif /* __PPC__ */
	
	assert(map->hdr.nentries==0);

	if(map->pmap)
		pmap_destroy(map->pmap);

	zfree(vm_map_zone, map);
}

#if	TASK_SWAPPER
/*
 * vm_map_swapin/vm_map_swapout
 *
 * Swap a map in and out, either referencing or releasing its resources.  
 * These functions are internal use only; however, they must be exported
 * because they may be called from macros, which are exported.
 *
 * In the case of swapout, there could be races on the residence count, 
 * so if the residence count is up, we return, assuming that a 
 * vm_map_deallocate() call in the near future will bring us back.
 *
 * Locking:
 *	-- We use the map write lock for synchronization among races.
 *	-- The map write lock, and not the simple s_lock, protects the
 *	   swap state of the map.
 *	-- If a map entry is a share map, then we hold both locks, in
 *	   hierarchical order.
 *
 * Synchronization Notes:
 *	1) If a vm_map_swapin() call happens while swapout in progress, it
 *	will block on the map lock and proceed when swapout is through.
 *	2) A vm_map_reference() call at this time is illegal, and will
 *	cause a panic.  vm_map_reference() is only allowed on resident
 *	maps, since it refuses to block.
 *	3) A vm_map_swapin() call during a swapin will block, and 
 *	proceeed when the first swapin is done, turning into a nop.
 *	This is the reason the res_count is not incremented until
 *	after the swapin is complete.
 *	4) There is a timing hole after the checks of the res_count, before
 *	the map lock is taken, during which a swapin may get the lock
 *	before a swapout about to happen.  If this happens, the swapin
 *	will detect the state and increment the reference count, causing
 *	the swapout to be a nop, thereby delaying it until a later 
 *	vm_map_deallocate.  If the swapout gets the lock first, then 
 *	the swapin will simply block until the swapout is done, and 
 *	then proceed.
 *
 * Because vm_map_swapin() is potentially an expensive operation, it
 * should be used with caution.
 *
 * Invariants:
 *	1) A map with a residence count of zero is either swapped, or
 *	   being swapped.
 *	2) A map with a non-zero residence count is either resident,
 *	   or being swapped in.
 */

int vm_map_swap_enable = 1;

void vm_map_swapin (vm_map_t map)
{
	register vm_map_entry_t entry;
	
	if (!vm_map_swap_enable)	/* debug */
		return;

	/*
	 * Map is locked
	 * First deal with various races.
	 */
	if (map->sw_state == MAP_SW_IN)
		/* 
		 * we raced with swapout and won.  Returning will incr.
		 * the res_count, turning the swapout into a nop.
		 */
		return;

	/*
	 * The residence count must be zero.  If we raced with another
	 * swapin, the state would have been IN; if we raced with a
	 * swapout (after another competing swapin), we must have lost
	 * the race to get here (see above comment), in which case
	 * res_count is still 0.
	 */
	assert(map->res_count == 0);

	/*
	 * There are no intermediate states of a map going out or
	 * coming in, since the map is locked during the transition.
	 */
	assert(map->sw_state == MAP_SW_OUT);

	/*
	 * We now operate upon each map entry.  If the entry is a sub- 
	 * or share-map, we call vm_map_res_reference upon it.
	 * If the entry is an object, we call vm_object_res_reference
	 * (this may iterate through the shadow chain).
	 * Note that we hold the map locked the entire time,
	 * even if we get back here via a recursive call in
	 * vm_map_res_reference.
	 */
	entry = vm_map_first_entry(map);

	while (entry != vm_map_to_entry(map)) {
		if (entry->object.vm_object != VM_OBJECT_NULL) {
			if (entry->is_sub_map) {
				vm_map_t lmap = entry->object.sub_map;
				mutex_lock(&lmap->s_lock);
				vm_map_res_reference(lmap);
				mutex_unlock(&lmap->s_lock);
			} else {
				vm_object_t object = entry->object.vm_object;
				vm_object_lock(object);
				/*
				 * This call may iterate through the
				 * shadow chain.
				 */
				vm_object_res_reference(object);
				vm_object_unlock(object);
			}
		}
		entry = entry->vme_next;
	}
	assert(map->sw_state == MAP_SW_OUT);
	map->sw_state = MAP_SW_IN;
}

void vm_map_swapout(vm_map_t map)
{
	register vm_map_entry_t entry;
	
	/*
	 * Map is locked
	 * First deal with various races.
	 * If we raced with a swapin and lost, the residence count
	 * will have been incremented to 1, and we simply return.
	 */
	mutex_lock(&map->s_lock);
	if (map->res_count != 0) {
		mutex_unlock(&map->s_lock);
		return;
	}
	mutex_unlock(&map->s_lock);

	/*
	 * There are no intermediate states of a map going out or
	 * coming in, since the map is locked during the transition.
	 */
	assert(map->sw_state == MAP_SW_IN);

	if (!vm_map_swap_enable)
		return;

	/*
	 * We now operate upon each map entry.  If the entry is a sub- 
	 * or share-map, we call vm_map_res_deallocate upon it.
	 * If the entry is an object, we call vm_object_res_deallocate
	 * (this may iterate through the shadow chain).
	 * Note that we hold the map locked the entire time,
	 * even if we get back here via a recursive call in
	 * vm_map_res_deallocate.
	 */
	entry = vm_map_first_entry(map);

	while (entry != vm_map_to_entry(map)) {
		if (entry->object.vm_object != VM_OBJECT_NULL) {
			if (entry->is_sub_map) {
				vm_map_t lmap = entry->object.sub_map;
				mutex_lock(&lmap->s_lock);
				vm_map_res_deallocate(lmap);
				mutex_unlock(&lmap->s_lock);
			} else {
				vm_object_t object = entry->object.vm_object;
				vm_object_lock(object);
				/*
				 * This call may take a long time, 
				 * since it could actively push 
				 * out pages (if we implement it 
				 * that way).
				 */
				vm_object_res_deallocate(object);
				vm_object_unlock(object);
			}
		}
		entry = entry->vme_next;
	}
	assert(map->sw_state == MAP_SW_IN);
	map->sw_state = MAP_SW_OUT;
}

#endif	/* TASK_SWAPPER */


/*
 *	SAVE_HINT:
 *
 *	Saves the specified entry as the hint for
 *	future lookups.  Performs necessary interlocks.
 */
#define	SAVE_HINT(map,value) \
MACRO_BEGIN \
		mutex_lock(&(map)->s_lock); \
		(map)->hint = (value); \
		mutex_unlock(&(map)->s_lock); \
MACRO_END

/*
 *	vm_map_lookup_entry:	[ internal use only ]
 *
 *	Finds the map entry containing (or
 *	immediately preceding) the specified address
 *	in the given map; the entry is returned
 *	in the "entry" parameter.  The boolean
 *	result indicates whether the address is
 *	actually contained in the map.
 */
boolean_t
vm_map_lookup_entry(
	register vm_map_t		map,
	register vm_map_offset_t	address,
	vm_map_entry_t		*entry)		/* OUT */
{
	register vm_map_entry_t		cur;
	register vm_map_entry_t		last;

	/*
	 *	Start looking either from the head of the
	 *	list, or from the hint.
	 */
	if (not_in_kdp)
	  mutex_lock(&map->s_lock);
	cur = map->hint;
	if (not_in_kdp)
	  mutex_unlock(&map->s_lock);

	if (cur == vm_map_to_entry(map))
		cur = cur->vme_next;

	if (address >= cur->vme_start) {
	    	/*
		 *	Go from hint to end of list.
		 *
		 *	But first, make a quick check to see if
		 *	we are already looking at the entry we
		 *	want (which is usually the case).
		 *	Note also that we don't need to save the hint
		 *	here... it is the same hint (unless we are
		 *	at the header, in which case the hint didn't
		 *	buy us anything anyway).
		 */
		last = vm_map_to_entry(map);
		if ((cur != last) && (cur->vme_end > address)) {
			*entry = cur;
			return(TRUE);
		}
	}
	else {
	    	/*
		 *	Go from start to hint, *inclusively*
		 */
		last = cur->vme_next;
		cur = vm_map_first_entry(map);
	}

	/*
	 *	Search linearly
	 */

	while (cur != last) {
		if (cur->vme_end > address) {
			if (address >= cur->vme_start) {
			    	/*
				 *	Save this lookup for future
				 *	hints, and return
				 */

				*entry = cur;
				if (not_in_kdp)
				  SAVE_HINT(map, cur);
				return(TRUE);
			}
			break;
		}
		cur = cur->vme_next;
	}
	*entry = cur->vme_prev;
	if (not_in_kdp)
	  SAVE_HINT(map, *entry);
	return(FALSE);
}

/*
 *	Routine:	vm_map_find_space
 *	Purpose:
 *		Allocate a range in the specified virtual address map,
 *		returning the entry allocated for that range.
 *		Used by kmem_alloc, etc.
 *
 *		The map must be NOT be locked. It will be returned locked
 *		on KERN_SUCCESS, unlocked on failure.
 *
 *		If an entry is allocated, the object/offset fields
 *		are initialized to zero.
 */
kern_return_t
vm_map_find_space(
	register vm_map_t	map,
	vm_map_offset_t		*address,	/* OUT */
	vm_map_size_t		size,
	vm_map_offset_t		mask,
	vm_map_entry_t		*o_entry)	/* OUT */
{
	register vm_map_entry_t	entry, new_entry;
	register vm_map_offset_t	start;
	register vm_map_offset_t	end;

	if (size == 0) {
		*address = 0;
		return KERN_INVALID_ARGUMENT;
	}

	new_entry = vm_map_entry_create(map);

	/*
	 *	Look for the first possible address; if there's already
	 *	something at this address, we have to start after it.
	 */

	vm_map_lock(map);

	assert(first_free_is_valid(map));
	if ((entry = map->first_free) == vm_map_to_entry(map))
		start = map->min_offset;
	else
		start = entry->vme_end;

	/*
	 *	In any case, the "entry" always precedes
	 *	the proposed new region throughout the loop:
	 */

	while (TRUE) {
		register vm_map_entry_t	next;

		/*
		 *	Find the end of the proposed new region.
		 *	Be sure we didn't go beyond the end, or
		 *	wrap around the address.
		 */

		end = ((start + mask) & ~mask);
		if (end < start) {
			vm_map_entry_dispose(map, new_entry);
			vm_map_unlock(map);
			return(KERN_NO_SPACE);
		}
		start = end;
		end += size;

		if ((end > map->max_offset) || (end < start)) {
			vm_map_entry_dispose(map, new_entry);
			vm_map_unlock(map);
			return(KERN_NO_SPACE);
		}

		/*
		 *	If there are no more entries, we must win.
		 */

		next = entry->vme_next;
		if (next == vm_map_to_entry(map))
			break;

		/*
		 *	If there is another entry, it must be
		 *	after the end of the potential new region.
		 */

		if (next->vme_start >= end)
			break;

		/*
		 *	Didn't fit -- move to the next entry.
		 */

		entry = next;
		start = entry->vme_end;
	}

	/*
	 *	At this point,
	 *		"start" and "end" should define the endpoints of the
	 *			available new range, and
	 *		"entry" should refer to the region before the new
	 *			range, and
	 *
	 *		the map should be locked.
	 */

	*address = start;

	new_entry->vme_start = start;
	new_entry->vme_end = end;
	assert(page_aligned(new_entry->vme_start));
	assert(page_aligned(new_entry->vme_end));

	new_entry->is_shared = FALSE;
	new_entry->is_sub_map = FALSE;
	new_entry->use_pmap = FALSE;
	new_entry->object.vm_object = VM_OBJECT_NULL;
	new_entry->offset = (vm_object_offset_t) 0;

	new_entry->needs_copy = FALSE;

	new_entry->inheritance = VM_INHERIT_DEFAULT;
	new_entry->protection = VM_PROT_DEFAULT;
	new_entry->max_protection = VM_PROT_ALL;
	new_entry->behavior = VM_BEHAVIOR_DEFAULT;
	new_entry->wired_count = 0;
	new_entry->user_wired_count = 0;

	new_entry->in_transition = FALSE;
	new_entry->needs_wakeup = FALSE;

	/*
	 *	Insert the new entry into the list
	 */

	vm_map_entry_link(map, entry, new_entry);

	map->size += size;

	/*
	 *	Update the lookup hint
	 */
	SAVE_HINT(map, new_entry);

	*o_entry = new_entry;
	return(KERN_SUCCESS);
}

int vm_map_pmap_enter_print = FALSE;
int vm_map_pmap_enter_enable = FALSE;

/*
 *	Routine:	vm_map_pmap_enter [internal only]
 *
 *	Description:
 *		Force pages from the specified object to be entered into
 *		the pmap at the specified address if they are present.
 *		As soon as a page not found in the object the scan ends.
 *
 *	Returns:
 *		Nothing.  
 *
 *	In/out conditions:
 *		The source map should not be locked on entry.
 */
static void
vm_map_pmap_enter(
	vm_map_t		map,
	register vm_map_offset_t 	addr,
	register vm_map_offset_t	end_addr,
	register vm_object_t 	object,
	vm_object_offset_t	offset,
	vm_prot_t		protection)
{
	unsigned int		cache_attr;

	if(map->pmap == 0)
		return;

	while (addr < end_addr) {
		register vm_page_t	m;

		vm_object_lock(object);
		vm_object_paging_begin(object);

		m = vm_page_lookup(object, offset);
		/*
		 * ENCRYPTED SWAP:
		 * The user should never see encrypted data, so do not
		 * enter an encrypted page in the page table.
		 */
		if (m == VM_PAGE_NULL || m->busy || m->encrypted ||
		    (m->unusual && ( m->error || m->restart || m->absent ||
				    protection & m->page_lock))) {

			vm_object_paging_end(object);
			vm_object_unlock(object);
			return;
		}

		assert(!m->fictitious);	/* XXX is this possible ??? */

		if (vm_map_pmap_enter_print) {
			printf("vm_map_pmap_enter:");
			printf("map: %x, addr: %llx, object: %x, offset: %llx\n",
				map, (unsigned long long)addr, object, (unsigned long long)offset);
		}
		m->busy = TRUE;

		if (m->no_isync == TRUE) {
		        pmap_sync_page_data_phys(m->phys_page);
			m->no_isync = FALSE;
		}

		cache_attr = ((unsigned int)object->wimg_bits) & VM_WIMG_MASK;
		vm_object_unlock(object);

		PMAP_ENTER(map->pmap, addr, m, 
				protection, cache_attr, FALSE);

		vm_object_lock(object);

		PAGE_WAKEUP_DONE(m);
		vm_page_lock_queues();
		if (!m->active && !m->inactive)
		    vm_page_activate(m);
		vm_page_unlock_queues();
		vm_object_paging_end(object);
		vm_object_unlock(object);

		offset += PAGE_SIZE_64;
		addr += PAGE_SIZE;
	}
}

boolean_t vm_map_pmap_is_empty(
	vm_map_t	map,
	vm_map_offset_t	start,
	vm_map_offset_t end);
boolean_t vm_map_pmap_is_empty(
	vm_map_t	map,
	vm_map_offset_t	start,
	vm_map_offset_t	end)
{
	vm_map_offset_t	offset;
	ppnum_t		phys_page;

	if (map->pmap == NULL) {
		return TRUE;
	}
	for (offset = start;
	     offset < end;
	     offset += PAGE_SIZE) {
		phys_page = pmap_find_phys(map->pmap, offset);
		if (phys_page) {
			kprintf("vm_map_pmap_is_empty(%p,0x%llx,0x%llx): "
				"page %d at 0x%llx\n",
				map, start, end, phys_page, offset);
			return FALSE;
		}
	}
	return TRUE;
}

/*
 *	Routine:	vm_map_enter
 *
 *	Description:
 *		Allocate a range in the specified virtual address map.
 *		The resulting range will refer to memory defined by
 *		the given memory object and offset into that object.
 *
 *		Arguments are as defined in the vm_map call.
 */
int _map_enter_debug = 0;
static unsigned int vm_map_enter_restore_successes = 0;
static unsigned int vm_map_enter_restore_failures = 0;
kern_return_t
vm_map_enter(
	vm_map_t		map,
	vm_map_offset_t	*address,	/* IN/OUT */
	vm_map_size_t		size,
	vm_map_offset_t	mask,
	int			flags,
	vm_object_t		object,
	vm_object_offset_t	offset,
	boolean_t		needs_copy,
	vm_prot_t		cur_protection,
	vm_prot_t		max_protection,
	vm_inherit_t		inheritance)
{
	vm_map_entry_t		entry, new_entry;
	vm_map_offset_t		start, tmp_start;
	vm_map_offset_t		end, tmp_end;
	kern_return_t		result = KERN_SUCCESS;
	vm_map_t		zap_old_map = VM_MAP_NULL;
	vm_map_t		zap_new_map = VM_MAP_NULL;
	boolean_t		map_locked = FALSE;
	boolean_t		pmap_empty = TRUE;
	boolean_t		new_mapping_established = FALSE;
	boolean_t		anywhere = ((flags & VM_FLAGS_ANYWHERE) != 0);
	boolean_t		purgable = ((flags & VM_FLAGS_PURGABLE) != 0);
	boolean_t		overwrite = ((flags & VM_FLAGS_OVERWRITE) != 0);
	char			alias;

	if (size == 0) {
		*address = 0;
		return KERN_INVALID_ARGUMENT;
	}

	VM_GET_FLAGS_ALIAS(flags, alias);

#define	RETURN(value)	{ result = value; goto BailOut; }

	assert(page_aligned(*address));
	assert(page_aligned(size));

	/*
	 * Only zero-fill objects are allowed to be purgable.
	 * LP64todo - limit purgable objects to 32-bits for now
	 */
	if (purgable &&
	    (offset != 0 ||
	     (object != VM_OBJECT_NULL &&
	      (object->size != size ||
	       object->purgable == VM_OBJECT_NONPURGABLE))
	     || size > VM_MAX_ADDRESS)) /* LP64todo: remove when dp capable */
		return KERN_INVALID_ARGUMENT;

	if (!anywhere && overwrite) {
		/*
		 * Create a temporary VM map to hold the old mappings in the
		 * affected area while we create the new one.
		 * This avoids releasing the VM map lock in
		 * vm_map_entry_delete() and allows atomicity
		 * when we want to replace some mappings with a new one.
		 * It also allows us to restore the old VM mappings if the
		 * new mapping fails.
		 */
		zap_old_map = vm_map_create(PMAP_NULL,
					    *address,
					    *address + size,
					    TRUE);
	}

 StartAgain: ;

	start = *address;

	if (anywhere) {
		vm_map_lock(map);
		map_locked = TRUE;

		/*
		 *	Calculate the first possible address.
		 */

		if (start < map->min_offset)
			start = map->min_offset;
		if (start > map->max_offset)
			RETURN(KERN_NO_SPACE);

		/*
		 *	Look for the first possible address;
		 *	if there's already something at this
		 *	address, we have to start after it.
		 */

		assert(first_free_is_valid(map));
		if (start == map->min_offset) {
			if ((entry = map->first_free) != vm_map_to_entry(map))
				start = entry->vme_end;
		} else {
			vm_map_entry_t	tmp_entry;
			if (vm_map_lookup_entry(map, start, &tmp_entry))
				start = tmp_entry->vme_end;
			entry = tmp_entry;
		}

		/*
		 *	In any case, the "entry" always precedes
		 *	the proposed new region throughout the
		 *	loop:
		 */

		while (TRUE) {
			register vm_map_entry_t	next;

		    	/*
			 *	Find the end of the proposed new region.
			 *	Be sure we didn't go beyond the end, or
			 *	wrap around the address.
			 */

			end = ((start + mask) & ~mask);
			if (end < start)
				RETURN(KERN_NO_SPACE);
			start = end;
			end += size;

			if ((end > map->max_offset) || (end < start)) {
				if (map->wait_for_space) {
					if (size <= (map->max_offset -
						     map->min_offset)) {
						assert_wait((event_t)map,
							    THREAD_ABORTSAFE);
						vm_map_unlock(map);
						map_locked = FALSE;
						thread_block(THREAD_CONTINUE_NULL);
						goto StartAgain;
					}
				}
				RETURN(KERN_NO_SPACE);
			}

			/*
			 *	If there are no more entries, we must win.
			 */

			next = entry->vme_next;
			if (next == vm_map_to_entry(map))
				break;

			/*
			 *	If there is another entry, it must be
			 *	after the end of the potential new region.
			 */

			if (next->vme_start >= end)
				break;

			/*
			 *	Didn't fit -- move to the next entry.
			 */

			entry = next;
			start = entry->vme_end;
		}
		*address = start;
	} else {
		vm_map_entry_t		temp_entry;

		/*
		 *	Verify that:
		 *		the address doesn't itself violate
		 *		the mask requirement.
		 */

		vm_map_lock(map);
		map_locked = TRUE;
		if ((start & mask) != 0)
			RETURN(KERN_NO_SPACE);

		/*
		 *	...	the address is within bounds
		 */

		end = start + size;

		if ((start < map->min_offset) ||
		    (end > map->max_offset) ||
		    (start >= end)) {
			RETURN(KERN_INVALID_ADDRESS);
		}

		if (overwrite && zap_old_map != VM_MAP_NULL) {
			/*
			 * Fixed mapping and "overwrite" flag: attempt to
			 * remove all existing mappings in the specified
			 * address range, saving them in our "zap_old_map".
			 */
			(void) vm_map_delete(map, start, end,
					     VM_MAP_REMOVE_SAVE_ENTRIES,
					     zap_old_map);
		}

		/*
		 *	...	the starting address isn't allocated
		 */

		if (vm_map_lookup_entry(map, start, &temp_entry))
			RETURN(KERN_NO_SPACE);

		entry = temp_entry;

		/*
		 *	...	the next region doesn't overlap the
		 *		end point.
		 */

		if ((entry->vme_next != vm_map_to_entry(map)) &&
		    (entry->vme_next->vme_start < end))
			RETURN(KERN_NO_SPACE);
	}

	/*
	 *	At this point,
	 *		"start" and "end" should define the endpoints of the
	 *			available new range, and
	 *		"entry" should refer to the region before the new
	 *			range, and
	 *
	 *		the map should be locked.
	 */

	/*
	 *	See whether we can avoid creating a new entry (and object) by
	 *	extending one of our neighbors.  [So far, we only attempt to
	 *	extend from below.]  Note that we can never extend/join
	 *	purgable objects because they need to remain distinct
	 *	entities in order to implement their "volatile object"
	 *	semantics.
	 */

	if (purgable) {
		if (object == VM_OBJECT_NULL) {
			object = vm_object_allocate(size);
			object->copy_strategy = MEMORY_OBJECT_COPY_NONE;
			object->purgable = VM_OBJECT_PURGABLE_NONVOLATILE;
			offset = (vm_object_offset_t)0;
		}
	} else if ((object == VM_OBJECT_NULL) &&
	    (entry != vm_map_to_entry(map)) &&
	    (entry->vme_end == start) &&
	    (!entry->is_shared) &&
	    (!entry->is_sub_map) &&
	    (entry->alias == alias) &&
	    (entry->inheritance == inheritance) &&
	    (entry->protection == cur_protection) &&
	    (entry->max_protection == max_protection) &&
	    (entry->behavior == VM_BEHAVIOR_DEFAULT) &&
	    (entry->in_transition == 0) &&
	    ((alias == VM_MEMORY_REALLOC) || ((entry->vme_end - entry->vme_start) + size < NO_COALESCE_LIMIT)) &&
	    (entry->wired_count == 0)) { /* implies user_wired_count == 0 */
		if (vm_object_coalesce(entry->object.vm_object,
				VM_OBJECT_NULL,
				entry->offset,
				(vm_object_offset_t) 0,
				(vm_map_size_t)(entry->vme_end - entry->vme_start),
				(vm_map_size_t)(end - entry->vme_end))) {

			/*
			 *	Coalesced the two objects - can extend
			 *	the previous map entry to include the
			 *	new range.
			 */
			map->size += (end - entry->vme_end);
			entry->vme_end = end;
			UPDATE_FIRST_FREE(map, map->first_free);
			RETURN(KERN_SUCCESS);
		}
	}

	/*
	 *	Create a new entry
	 *	LP64todo - for now, we can only allocate 4GB internal objects
	 *	because the default pager can't page bigger ones.  Remove this
	 *	when it can.
	 */
	tmp_start = start;
	if (object == VM_OBJECT_NULL && size > (vm_map_size_t)VM_MAX_ADDRESS) 
		tmp_end = tmp_start + (vm_map_size_t)VM_MAX_ADDRESS;
	else
		tmp_end = end;
	do {
		new_entry = vm_map_entry_insert(map, entry, tmp_start, tmp_end,
					object,	offset, needs_copy, FALSE, FALSE,
					cur_protection, max_protection,
					VM_BEHAVIOR_DEFAULT, inheritance, 0);
		new_entry->alias = alias;
		entry = new_entry;
	} while (object == VM_OBJECT_NULL &&
		 tmp_end != end && 
		 (tmp_start = tmp_end) &&
		 (tmp_end = (end - tmp_end > (vm_map_size_t)VM_MAX_ADDRESS) ? 
		  tmp_end + (vm_map_size_t)VM_MAX_ADDRESS : end));

	vm_map_unlock(map);
	map_locked = FALSE;

	new_mapping_established = TRUE;

	/*	Wire down the new entry if the user
	 *	requested all new map entries be wired.
	 */
	if (map->wiring_required) {
		pmap_empty = FALSE; /* pmap won't be empty */
		result = vm_map_wire(map, start, end,
				     new_entry->protection, TRUE);
		RETURN(result);
	}

	if ((object != VM_OBJECT_NULL) &&
	    (vm_map_pmap_enter_enable) &&
	    (!anywhere)	 &&
	    (!needs_copy) && 
	    (size < (128*1024))) {
		pmap_empty = FALSE; /* pmap won't be empty */
		vm_map_pmap_enter(map, start, end, 
				  object, offset, cur_protection);
	}

 BailOut: ;
	if (result == KERN_SUCCESS &&
	    pmap_empty &&
	    !(flags & VM_FLAGS_NO_PMAP_CHECK)) {
		assert(vm_map_pmap_is_empty(map, *address, *address+size));
	}

	if (result != KERN_SUCCESS) {
		if (new_mapping_established) {
			/*
			 * We have to get rid of the new mappings since we
			 * won't make them available to the user.
			 * Try and do that atomically, to minimize the risk
			 * that someone else create new mappings that range.
			 */
			zap_new_map = vm_map_create(PMAP_NULL,
						    *address,
						    *address + size,
						    TRUE);
			if (!map_locked) {
				vm_map_lock(map);
				map_locked = TRUE;
			}
			(void) vm_map_delete(map, *address, *address+size,
					     VM_MAP_REMOVE_SAVE_ENTRIES,
					     zap_new_map);
		}
		if (zap_old_map != VM_MAP_NULL &&
		    zap_old_map->hdr.nentries != 0) {
			vm_map_entry_t	entry1, entry2;

			/*
			 * The new mapping failed.  Attempt to restore
			 * the old mappings, saved in the "zap_old_map".
			 */
			if (!map_locked) {
				vm_map_lock(map);
				map_locked = TRUE;
			}

			/* first check if the coast is still clear */
			start = vm_map_first_entry(zap_old_map)->vme_start;
			end = vm_map_last_entry(zap_old_map)->vme_end;
			if (vm_map_lookup_entry(map, start, &entry1) ||
			    vm_map_lookup_entry(map, end, &entry2) ||
			    entry1 != entry2) {
				/*
				 * Part of that range has already been
				 * re-mapped:  we can't restore the old
				 * mappings...
				 */
				vm_map_enter_restore_failures++;
			} else {
				/*
				 * Transfer the saved map entries from
				 * "zap_old_map" to the original "map",
				 * inserting them all after "entry1".
				 */
				for (entry2 = vm_map_first_entry(zap_old_map);
				     entry2 != vm_map_to_entry(zap_old_map);
				     entry2 = vm_map_first_entry(zap_old_map)) {
					vm_map_entry_unlink(zap_old_map,
							    entry2);
					vm_map_entry_link(map, entry1, entry2);
					entry1 = entry2;
				}
				if (map->wiring_required) {
					/*
					 * XXX TODO: we should rewire the
					 * old pages here...
					 */
				}
				vm_map_enter_restore_successes++;
			}
		}
	}

	if (map_locked) {
		vm_map_unlock(map);
	}

	/*
	 * Get rid of the "zap_maps" and all the map entries that
	 * they may still contain.
	 */
	if (zap_old_map != VM_MAP_NULL) {
		vm_map_destroy(zap_old_map);
		zap_old_map = VM_MAP_NULL;
	}
	if (zap_new_map != VM_MAP_NULL) {
		vm_map_destroy(zap_new_map);
		zap_new_map = VM_MAP_NULL;
	}

	return result;

#undef	RETURN
}


#if	VM_CPM

#ifdef MACH_ASSERT
extern vm_offset_t	avail_start, avail_end;
#endif

/*
 *	Allocate memory in the specified map, with the caveat that
 *	the memory is physically contiguous.  This call may fail
 *	if the system can't find sufficient contiguous memory.
 *	This call may cause or lead to heart-stopping amounts of
 *	paging activity.
 *
 *	Memory obtained from this call should be freed in the
 *	normal way, viz., via vm_deallocate.
 */
kern_return_t
vm_map_enter_cpm(
	vm_map_t		map,
	vm_map_offset_t	*addr,
	vm_map_size_t		size,
	int			flags)
{
	vm_object_t		cpm_obj;
	pmap_t			pmap;
	vm_page_t		m, pages;
	kern_return_t		kr;
	vm_map_offset_t		va, start, end, offset;
#if	MACH_ASSERT
	vm_map_offset_t		prev_addr;
#endif	/* MACH_ASSERT */

	boolean_t		anywhere = ((VM_FLAGS_ANYWHERE & flags) != 0);

	if (!vm_allocate_cpm_enabled)
		return KERN_FAILURE;

	if (size == 0) {
		*addr = 0;
		return KERN_SUCCESS;
	}

	if (anywhere)
		*addr = vm_map_min(map);
	else
		*addr = vm_map_trunc_page(*addr);
	size = vm_map_round_page(size);

	/*
	 * LP64todo - cpm_allocate should probably allow
	 * allocations of >4GB, but not with the current
	 * algorithm, so just cast down the size for now.
	 */
	if (size > VM_MAX_ADDRESS)
		return KERN_RESOURCE_SHORTAGE;
	if ((kr = cpm_allocate(CAST_DOWN(vm_size_t, size),
			       &pages, TRUE)) != KERN_SUCCESS)
		return kr;

	cpm_obj = vm_object_allocate((vm_object_size_t)size);
	assert(cpm_obj != VM_OBJECT_NULL);
	assert(cpm_obj->internal);
	assert(cpm_obj->size == (vm_object_size_t)size);
	assert(cpm_obj->can_persist == FALSE);
	assert(cpm_obj->pager_created == FALSE);
	assert(cpm_obj->pageout == FALSE);
	assert(cpm_obj->shadow == VM_OBJECT_NULL);

	/*
	 *	Insert pages into object.
	 */

	vm_object_lock(cpm_obj);
	for (offset = 0; offset < size; offset += PAGE_SIZE) {
		m = pages;
		pages = NEXT_PAGE(m);

		assert(!m->gobbled);
		assert(!m->wanted);
		assert(!m->pageout);
		assert(!m->tabled);
		/*
		 * ENCRYPTED SWAP:
		 * "m" is not supposed to be pageable, so it
		 * should not be encrypted.  It wouldn't be safe
		 * to enter it in a new VM object while encrypted.
		 */
		ASSERT_PAGE_DECRYPTED(m);
		assert(m->busy);
		assert(m->phys_page>=avail_start && m->phys_page<=avail_end);

		m->busy = FALSE;
		vm_page_insert(m, cpm_obj, offset);
	}
	assert(cpm_obj->resident_page_count == size / PAGE_SIZE);
	vm_object_unlock(cpm_obj);

	/*
	 *	Hang onto a reference on the object in case a
	 *	multi-threaded application for some reason decides
	 *	to deallocate the portion of the address space into
	 *	which we will insert this object.
	 *
	 *	Unfortunately, we must insert the object now before
	 *	we can talk to the pmap module about which addresses
	 *	must be wired down.  Hence, the race with a multi-
	 *	threaded app.
	 */
	vm_object_reference(cpm_obj);

	/*
	 *	Insert object into map.
	 */

	kr = vm_map_enter(
			  map,
			  addr,
			  size,
			  (vm_map_offset_t)0,
			  flags,
			  cpm_obj,
			  (vm_object_offset_t)0,
			  FALSE,
			  VM_PROT_ALL,
			  VM_PROT_ALL,
			  VM_INHERIT_DEFAULT);

	if (kr != KERN_SUCCESS) {
		/*
		 *	A CPM object doesn't have can_persist set,
		 *	so all we have to do is deallocate it to
		 *	free up these pages.
		 */
		assert(cpm_obj->pager_created == FALSE);
		assert(cpm_obj->can_persist == FALSE);
		assert(cpm_obj->pageout == FALSE);
		assert(cpm_obj->shadow == VM_OBJECT_NULL);
		vm_object_deallocate(cpm_obj); /* kill acquired ref */
		vm_object_deallocate(cpm_obj); /* kill creation ref */
	}

	/*
	 *	Inform the physical mapping system that the
	 *	range of addresses may not fault, so that
	 *	page tables and such can be locked down as well.
	 */
	start = *addr;
	end = start + size;
	pmap = vm_map_pmap(map);
	pmap_pageable(pmap, start, end, FALSE);

	/*
	 *	Enter each page into the pmap, to avoid faults.
	 *	Note that this loop could be coded more efficiently,
	 *	if the need arose, rather than looking up each page
	 *	again.
	 */
	for (offset = 0, va = start; offset < size;
	     va += PAGE_SIZE, offset += PAGE_SIZE) {
		vm_object_lock(cpm_obj);
		m = vm_page_lookup(cpm_obj, (vm_object_offset_t)offset);
		vm_object_unlock(cpm_obj);
		assert(m != VM_PAGE_NULL);
		PMAP_ENTER(pmap, va, m, VM_PROT_ALL, 	
			((unsigned int)(m->object->wimg_bits)) & VM_WIMG_MASK,
			TRUE);
	}

#if	MACH_ASSERT
	/*
	 *	Verify ordering in address space.
	 */
	for (offset = 0; offset < size; offset += PAGE_SIZE) {
		vm_object_lock(cpm_obj);
		m = vm_page_lookup(cpm_obj, (vm_object_offset_t)offset);
		vm_object_unlock(cpm_obj);
		if (m == VM_PAGE_NULL)
			panic("vm_allocate_cpm:  obj 0x%x off 0x%x no page",
			      cpm_obj, offset);
		assert(m->tabled);
		assert(!m->busy);
		assert(!m->wanted);
		assert(!m->fictitious);
		assert(!m->private);
		assert(!m->absent);
		assert(!m->error);
		assert(!m->cleaning);
		assert(!m->precious);
		assert(!m->clustered);
		if (offset != 0) {
			if (m->phys_page != prev_addr + 1) {
				printf("start 0x%x end 0x%x va 0x%x\n",
				       start, end, va);
				printf("obj 0x%x off 0x%x\n", cpm_obj, offset);
				printf("m 0x%x prev_address 0x%x\n", m,
				       prev_addr);
				panic("vm_allocate_cpm:  pages not contig!");
			}
		}
		prev_addr = m->phys_page;
	}
#endif	/* MACH_ASSERT */

	vm_object_deallocate(cpm_obj); /* kill extra ref */

	return kr;
}


#else	/* VM_CPM */

/*
 *	Interface is defined in all cases, but unless the kernel
 *	is built explicitly for this option, the interface does
 *	nothing.
 */

kern_return_t
vm_map_enter_cpm(
	__unused vm_map_t	map,
	__unused vm_map_offset_t	*addr,
	__unused vm_map_size_t	size,
	__unused int		flags)
{
	return KERN_FAILURE;
}
#endif /* VM_CPM */

/*
 *	vm_map_clip_start:	[ internal use only ]
 *
 *	Asserts that the given entry begins at or after
 *	the specified address; if necessary,
 *	it splits the entry into two.
 */
#ifndef i386
#define vm_map_clip_start(map, entry, startaddr) 			\
MACRO_BEGIN 								\
	vm_map_t VMCS_map;						\
	vm_map_entry_t VMCS_entry;					\
	vm_map_offset_t VMCS_startaddr;					\
	VMCS_map = (map);						\
	VMCS_entry = (entry);						\
	VMCS_startaddr = (startaddr);					\
	if (VMCS_startaddr > VMCS_entry->vme_start) { 			\
		if(entry->use_pmap) {					\
	   		vm_map_offset_t	pmap_base_addr;			\
									\
	   		pmap_base_addr = 0xF0000000 & entry->vme_start;	\
   	   		pmap_unnest(map->pmap, (addr64_t)pmap_base_addr); \
			entry->use_pmap = FALSE;			\
		} else if(entry->object.vm_object			\
			&& !entry->is_sub_map				\
			&& entry->object.vm_object->phys_contiguous) {	\
			pmap_remove(map->pmap,				\
				(addr64_t)(entry->vme_start),		\
				(addr64_t)(entry->vme_end));		\
		}							\
		_vm_map_clip_start(&VMCS_map->hdr,VMCS_entry,VMCS_startaddr);\
	}								\
	UPDATE_FIRST_FREE(VMCS_map, VMCS_map->first_free);		\
MACRO_END
#else
#define vm_map_clip_start(map, entry, startaddr) 			\
MACRO_BEGIN 								\
	vm_map_t VMCS_map;						\
	vm_map_entry_t VMCS_entry;					\
	vm_map_offset_t VMCS_startaddr;					\
	VMCS_map = (map);						\
	VMCS_entry = (entry);						\
	VMCS_startaddr = (startaddr);					\
	if (VMCS_startaddr > VMCS_entry->vme_start) { 			\
		_vm_map_clip_start(&VMCS_map->hdr,VMCS_entry,VMCS_startaddr);\
	}								\
	UPDATE_FIRST_FREE(VMCS_map, VMCS_map->first_free);		\
MACRO_END
#endif

#define vm_map_copy_clip_start(copy, entry, startaddr) \
	MACRO_BEGIN \
	if ((startaddr) > (entry)->vme_start) \
		_vm_map_clip_start(&(copy)->cpy_hdr,(entry),(startaddr)); \
	MACRO_END

/*
 *	This routine is called only when it is known that
 *	the entry must be split.
 */
static void
_vm_map_clip_start(
	register struct vm_map_header	*map_header,
	register vm_map_entry_t		entry,
	register vm_map_offset_t		start)
{
	register vm_map_entry_t	new_entry;

	/*
	 *	Split off the front portion --
	 *	note that we must insert the new
	 *	entry BEFORE this one, so that
	 *	this entry has the specified starting
	 *	address.
	 */

	new_entry = _vm_map_entry_create(map_header);
	vm_map_entry_copy_full(new_entry, entry);

	new_entry->vme_end = start;
	entry->offset += (start - entry->vme_start);
	entry->vme_start = start;

	_vm_map_entry_link(map_header, entry->vme_prev, new_entry);

	if (entry->is_sub_map)
	 	vm_map_reference(new_entry->object.sub_map);
	else
		vm_object_reference(new_entry->object.vm_object);
}


/*
 *	vm_map_clip_end:	[ internal use only ]
 *
 *	Asserts that the given entry ends at or before
 *	the specified address; if necessary,
 *	it splits the entry into two.
 */
#ifndef i386
#define vm_map_clip_end(map, entry, endaddr) 				\
MACRO_BEGIN 								\
	vm_map_t VMCE_map;						\
	vm_map_entry_t VMCE_entry;					\
	vm_map_offset_t VMCE_endaddr;					\
	VMCE_map = (map);						\
	VMCE_entry = (entry);						\
	VMCE_endaddr = (endaddr);					\
	if (VMCE_endaddr < VMCE_entry->vme_end) { 			\
		if(entry->use_pmap) {					\
	   		vm_map_offset_t	pmap_base_addr;			\
									\
	   		pmap_base_addr = 0xF0000000 & entry->vme_start;	\
 	   		pmap_unnest(map->pmap, (addr64_t)pmap_base_addr); \
			entry->use_pmap = FALSE;			\
		} else if(entry->object.vm_object			\
			&& !entry->is_sub_map				\
			&& entry->object.vm_object->phys_contiguous) {	\
			pmap_remove(map->pmap,				\
				(addr64_t)(entry->vme_start),		\
				(addr64_t)(entry->vme_end));		\
		}							\
		_vm_map_clip_end(&VMCE_map->hdr,VMCE_entry,VMCE_endaddr); \
	}								\
	UPDATE_FIRST_FREE(VMCE_map, VMCE_map->first_free);		\
MACRO_END
#else
#define vm_map_clip_end(map, entry, endaddr) 				\
MACRO_BEGIN 								\
	vm_map_t VMCE_map;						\
	vm_map_entry_t VMCE_entry;					\
	vm_map_offset_t VMCE_endaddr;					\
	VMCE_map = (map);						\
	VMCE_entry = (entry);						\
	VMCE_endaddr = (endaddr);					\
	if (VMCE_endaddr < VMCE_entry->vme_end) { 			\
		_vm_map_clip_end(&VMCE_map->hdr,VMCE_entry,VMCE_endaddr); \
	}								\
	UPDATE_FIRST_FREE(VMCE_map, VMCE_map->first_free);		\
MACRO_END
#endif

#define vm_map_copy_clip_end(copy, entry, endaddr) \
	MACRO_BEGIN \
	if ((endaddr) < (entry)->vme_end) \
		_vm_map_clip_end(&(copy)->cpy_hdr,(entry),(endaddr)); \
	MACRO_END

/*
 *	This routine is called only when it is known that
 *	the entry must be split.
 */
static void
_vm_map_clip_end(
	register struct vm_map_header	*map_header,
	register vm_map_entry_t		entry,
	register vm_map_offset_t		end)
{
	register vm_map_entry_t	new_entry;

	/*
	 *	Create a new entry and insert it
	 *	AFTER the specified entry
	 */

	new_entry = _vm_map_entry_create(map_header);
	vm_map_entry_copy_full(new_entry, entry);

	new_entry->vme_start = entry->vme_end = end;
	new_entry->offset += (end - entry->vme_start);

	_vm_map_entry_link(map_header, entry, new_entry);

	if (entry->is_sub_map)
	 	vm_map_reference(new_entry->object.sub_map);
	else
		vm_object_reference(new_entry->object.vm_object);
}


/*
 *	VM_MAP_RANGE_CHECK:	[ internal use only ]
 *
 *	Asserts that the starting and ending region
 *	addresses fall within the valid range of the map.
 */
#define	VM_MAP_RANGE_CHECK(map, start, end)		\
		{					\
		if (start < vm_map_min(map))		\
			start = vm_map_min(map);	\
		if (end > vm_map_max(map))		\
			end = vm_map_max(map);		\
		if (start > end)			\
			start = end;			\
		}

/*
 *	vm_map_range_check:	[ internal use only ]
 *	
 *	Check that the region defined by the specified start and
 *	end addresses are wholly contained within a single map
 *	entry or set of adjacent map entries of the spacified map,
 *	i.e. the specified region contains no unmapped space.
 *	If any or all of the region is unmapped, FALSE is returned.
 *	Otherwise, TRUE is returned and if the output argument 'entry'
 *	is not NULL it points to the map entry containing the start
 *	of the region.
 *
 *	The map is locked for reading on entry and is left locked.
 */
static boolean_t
vm_map_range_check(
	register vm_map_t	map,
	register vm_map_offset_t	start,
	register vm_map_offset_t	end,
	vm_map_entry_t		*entry)
{
	vm_map_entry_t		cur;
	register vm_map_offset_t	prev;

	/*
	 * 	Basic sanity checks first
	 */
	if (start < vm_map_min(map) || end > vm_map_max(map) || start > end)
		return (FALSE);

	/*
	 * 	Check first if the region starts within a valid
	 *	mapping for the map.
	 */
	if (!vm_map_lookup_entry(map, start, &cur))
		return (FALSE);

	/*
	 *	Optimize for the case that the region is contained 
	 *	in a single map entry.
	 */
	if (entry != (vm_map_entry_t *) NULL)
		*entry = cur;
	if (end <= cur->vme_end)
		return (TRUE);

	/*
	 * 	If the region is not wholly contained within a
	 * 	single entry, walk the entries looking for holes.
	 */
	prev = cur->vme_end;
	cur = cur->vme_next;
	while ((cur != vm_map_to_entry(map)) && (prev == cur->vme_start)) {
		if (end <= cur->vme_end)
			return (TRUE);
		prev = cur->vme_end;
		cur = cur->vme_next;
	}
	return (FALSE);
}

/*
 *	vm_map_submap:		[ kernel use only ]
 *
 *	Mark the given range as handled by a subordinate map.
 *
 *	This range must have been created with vm_map_find using
 *	the vm_submap_object, and no other operations may have been
 *	performed on this range prior to calling vm_map_submap.
 *
 *	Only a limited number of operations can be performed
 *	within this rage after calling vm_map_submap:
 *		vm_fault
 *	[Don't try vm_map_copyin!]
 *
 *	To remove a submapping, one must first remove the
 *	range from the superior map, and then destroy the
 *	submap (if desired).  [Better yet, don't try it.]
 */
kern_return_t
vm_map_submap(
	vm_map_t		map,
	vm_map_offset_t	start,
	vm_map_offset_t	end,
	vm_map_t		submap,
	vm_map_offset_t	offset,
#ifdef i386
	__unused
#endif
	boolean_t		use_pmap)
{
	vm_map_entry_t		entry;
	register kern_return_t	result = KERN_INVALID_ARGUMENT;
	register vm_object_t	object;

	vm_map_lock(map);

	submap->mapped = TRUE;

	VM_MAP_RANGE_CHECK(map, start, end);

	if (vm_map_lookup_entry(map, start, &entry)) {
		vm_map_clip_start(map, entry, start);
	}
	else
		entry = entry->vme_next;

	if(entry == vm_map_to_entry(map)) {
		vm_map_unlock(map);
		return KERN_INVALID_ARGUMENT;
	}

	vm_map_clip_end(map, entry, end);

	if ((entry->vme_start == start) && (entry->vme_end == end) &&
	    (!entry->is_sub_map) &&
	    ((object = entry->object.vm_object) == vm_submap_object) &&
	    (object->resident_page_count == 0) &&
	    (object->copy == VM_OBJECT_NULL) &&
	    (object->shadow == VM_OBJECT_NULL) &&
	    (!object->pager_created)) {
			entry->offset = (vm_object_offset_t)offset;
			entry->object.vm_object = VM_OBJECT_NULL;
			vm_object_deallocate(object);
			entry->is_sub_map = TRUE;
			entry->object.sub_map = submap;
			vm_map_reference(submap);
#ifndef i386
			if ((use_pmap) && (offset == 0)) {
				/* nest if platform code will allow */
				if(submap->pmap == NULL) {
					submap->pmap = pmap_create((vm_map_size_t) 0);
					if(submap->pmap == PMAP_NULL) {
						vm_map_unlock(map);
						return(KERN_NO_SPACE);
					}
				}
				result = pmap_nest(map->pmap, (entry->object.sub_map)->pmap, 
						   (addr64_t)start,
						   (addr64_t)start,
						   (uint64_t)(end - start));
				if(result)
					panic("vm_map_submap: pmap_nest failed, rc = %08X\n", result);
				entry->use_pmap = TRUE;
			}
#endif
#ifdef i386
			pmap_remove(map->pmap, (addr64_t)start, (addr64_t)end);
#endif
			result = KERN_SUCCESS;
	}
	vm_map_unlock(map);

	return(result);
}

/*
 *	vm_map_protect:
 *
 *	Sets the protection of the specified address
 *	region in the target map.  If "set_max" is
 *	specified, the maximum protection is to be set;
 *	otherwise, only the current protection is affected.
 */
kern_return_t
vm_map_protect(
	register vm_map_t	map,
	register vm_map_offset_t	start,
	register vm_map_offset_t	end,
	register vm_prot_t	new_prot,
	register boolean_t	set_max)
{
	register vm_map_entry_t		current;
	register vm_map_offset_t		prev;
	vm_map_entry_t			entry;
	vm_prot_t			new_max;
	boolean_t			clip;

	XPR(XPR_VM_MAP,
		"vm_map_protect, 0x%X start 0x%X end 0x%X, new 0x%X %d",
		(integer_t)map, start, end, new_prot, set_max);

	vm_map_lock(map);

	/* LP64todo - remove this check when vm_map_commpage64()
	 * no longer has to stuff in a map_entry for the commpage
	 * above the map's max_offset.
	 */
	if (start >= map->max_offset) {
		vm_map_unlock(map);
		return(KERN_INVALID_ADDRESS);
	}

	/*
	 * 	Lookup the entry.  If it doesn't start in a valid
	 *	entry, return an error.  Remember if we need to
	 *	clip the entry.  We don't do it here because we don't
	 *	want to make any changes until we've scanned the 
	 *	entire range below for address and protection
	 *	violations.
	 */
	if (!(clip = vm_map_lookup_entry(map, start, &entry))) {
		vm_map_unlock(map);
		return(KERN_INVALID_ADDRESS);
	}

	/*
	 *	Make a first pass to check for protection and address
	 *	violations.
	 */

	current = entry;
	prev = current->vme_start;
	while ((current != vm_map_to_entry(map)) &&
	       (current->vme_start < end)) {

		/*
		 * If there is a hole, return an error.
		 */
		if (current->vme_start != prev) {
			vm_map_unlock(map);
			return(KERN_INVALID_ADDRESS);
		}

		new_max = current->max_protection;
		if(new_prot & VM_PROT_COPY) {
			new_max |= VM_PROT_WRITE;
			if ((new_prot & (new_max | VM_PROT_COPY)) != new_prot) {
				vm_map_unlock(map);
				return(KERN_PROTECTION_FAILURE);
			}
		} else {
			if ((new_prot & new_max) != new_prot) {
				vm_map_unlock(map);
				return(KERN_PROTECTION_FAILURE);
			}
		}

		prev = current->vme_end;
		current = current->vme_next;
	}
	if (end > prev) {
		vm_map_unlock(map);
		return(KERN_INVALID_ADDRESS);
	}

	/*
	 *	Go back and fix up protections.
	 *	Clip to start here if the range starts within
	 *	the entry.
	 */

	current = entry;
	if (clip) {
		vm_map_clip_start(map, entry, start);
	}
	while ((current != vm_map_to_entry(map)) &&
	       (current->vme_start < end)) {

		vm_prot_t	old_prot;

		vm_map_clip_end(map, current, end);

		old_prot = current->protection;

		if(new_prot & VM_PROT_COPY) {
			/* caller is asking specifically to copy the      */
			/* mapped data, this implies that max protection  */
			/* will include write.  Caller must be prepared   */
			/* for loss of shared memory communication in the */
			/* target area after taking this step */
			current->needs_copy = TRUE;
			current->max_protection |= VM_PROT_WRITE;
		}

		if (set_max)
			current->protection =
				(current->max_protection = 
					new_prot & ~VM_PROT_COPY) &
					old_prot;
		else
			current->protection = new_prot & ~VM_PROT_COPY;

		/*
		 *	Update physical map if necessary.
		 *	If the request is to turn off write protection, 
		 *	we won't do it for real (in pmap). This is because 
		 *	it would cause copy-on-write to fail.  We've already 
		 *	set, the new protection in the map, so if a 
		 *	write-protect fault occurred, it will be fixed up 
		 *	properly, COW or not.
		 */
	 	/* the 256M hack for existing hardware limitations */
		if (current->protection != old_prot) {
	 	   if(current->is_sub_map && current->use_pmap) {
			vm_map_offset_t	pmap_base_addr;
			vm_map_offset_t	pmap_end_addr;
#ifdef i386
			__unused
#endif
			vm_map_entry_t	local_entry;

			pmap_base_addr = 0xF0000000 & current->vme_start;
			pmap_end_addr = (pmap_base_addr + 0x10000000) - 1;
#ifndef i386
			if(!vm_map_lookup_entry(map, 
					pmap_base_addr, &local_entry))
			   panic("vm_map_protect: nested pmap area is missing");
			   while ((local_entry != vm_map_to_entry(map)) &&
	       	  	          (local_entry->vme_start < pmap_end_addr)) {
				local_entry->use_pmap = FALSE;
				local_entry = local_entry->vme_next;
			   }
			   pmap_unnest(map->pmap, (addr64_t)pmap_base_addr);
#endif
		   }
		   if (!(current->protection & VM_PROT_WRITE)) {
			/* Look one level in we support nested pmaps */
			/* from mapped submaps which are direct entries */
			/* in our map */
			if(current->is_sub_map && current->use_pmap) {
				pmap_protect(current->object.sub_map->pmap, 
					current->vme_start,
					current->vme_end,
					current->protection);
			} else {
				pmap_protect(map->pmap, current->vme_start,
					current->vme_end,
					current->protection);
			}
		   }
		}
		current = current->vme_next;
	}

	current = entry;
	while ((current != vm_map_to_entry(map)) &&
	       (current->vme_start <= end)) {
		vm_map_simplify_entry(map, current);
		current = current->vme_next;
	}

	vm_map_unlock(map);
	return(KERN_SUCCESS);
}

/*
 *	vm_map_inherit:
 *
 *	Sets the inheritance of the specified address
 *	range in the target map.  Inheritance
 *	affects how the map will be shared with
 *	child maps at the time of vm_map_fork.
 */
kern_return_t
vm_map_inherit(
	register vm_map_t	map,
	register vm_map_offset_t	start,
	register vm_map_offset_t	end,
	register vm_inherit_t	new_inheritance)
{
	register vm_map_entry_t	entry;
	vm_map_entry_t	temp_entry;

	vm_map_lock(map);

	VM_MAP_RANGE_CHECK(map, start, end);

	if (vm_map_lookup_entry(map, start, &temp_entry)) {
		entry = temp_entry;
		vm_map_clip_start(map, entry, start);
	}
	else {
		temp_entry = temp_entry->vme_next;
		entry = temp_entry;
	}

	/* first check entire range for submaps which can't support the */
	/* given inheritance. */
	while ((entry != vm_map_to_entry(map)) && (entry->vme_start < end)) {
		if(entry->is_sub_map) {
			if(new_inheritance == VM_INHERIT_COPY) {
				vm_map_unlock(map);
				return(KERN_INVALID_ARGUMENT);
			}
		}

		entry = entry->vme_next;
	}

	entry = temp_entry;

	while ((entry != vm_map_to_entry(map)) && (entry->vme_start < end)) {
		vm_map_clip_end(map, entry, end);

		entry->inheritance = new_inheritance;

		entry = entry->vme_next;
	}

	vm_map_unlock(map);
	return(KERN_SUCCESS);
}

/*
 *	vm_map_wire:
 *
 *	Sets the pageability of the specified address range in the
 *	target map as wired.  Regions specified as not pageable require
 *	locked-down physical memory and physical page maps.  The
 *	access_type variable indicates types of accesses that must not
 *	generate page faults.  This is checked against protection of
 *	memory being locked-down.
 *
 *	The map must not be locked, but a reference must remain to the
 *	map throughout the call.
 */
static kern_return_t
vm_map_wire_nested(
	register vm_map_t	map,
	register vm_map_offset_t	start,
	register vm_map_offset_t	end,
	register vm_prot_t	access_type,
	boolean_t		user_wire,
	pmap_t			map_pmap, 
	vm_map_offset_t		pmap_addr)
{
	register vm_map_entry_t	entry;
	struct vm_map_entry	*first_entry, tmp_entry;
	vm_map_t		real_map;
	register vm_map_offset_t	s,e;
	kern_return_t		rc;
	boolean_t		need_wakeup;
	boolean_t		main_map = FALSE;
	wait_interrupt_t	interruptible_state;
	thread_t		cur_thread;
	unsigned int		last_timestamp;
	vm_map_size_t		size;

	vm_map_lock(map);
	if(map_pmap == NULL)
		main_map = TRUE;
	last_timestamp = map->timestamp;

	VM_MAP_RANGE_CHECK(map, start, end);
	assert(page_aligned(start));
	assert(page_aligned(end));
	if (start == end) {
		/* We wired what the caller asked for, zero pages */
		vm_map_unlock(map);
		return KERN_SUCCESS;
	}

	if (vm_map_lookup_entry(map, start, &first_entry)) {
		entry = first_entry;
		/* vm_map_clip_start will be done later. */
	} else {
		/* Start address is not in map */
		vm_map_unlock(map);
		return(KERN_INVALID_ADDRESS);
	}

	s=start;
	need_wakeup = FALSE;
	cur_thread = current_thread();
	while ((entry != vm_map_to_entry(map)) && (entry->vme_start < end)) {
		/*
		 * If another thread is wiring/unwiring this entry then
		 * block after informing other thread to wake us up.
		 */
		if (entry->in_transition) {
			wait_result_t wait_result;

			/*
			 * We have not clipped the entry.  Make sure that
			 * the start address is in range so that the lookup
			 * below will succeed.
			 */
			s = entry->vme_start < start? start: entry->vme_start;

			entry->needs_wakeup = TRUE;

			/*
			 * wake up anybody waiting on entries that we have
			 * already wired.
			 */
			if (need_wakeup) {
				vm_map_entry_wakeup(map);
				need_wakeup = FALSE;
			}
			/*
			 * User wiring is interruptible
			 */
			wait_result = vm_map_entry_wait(map, 
					  (user_wire) ? THREAD_ABORTSAFE :
					                THREAD_UNINT);
			if (user_wire && wait_result ==	THREAD_INTERRUPTED) {
				/*
				 * undo the wirings we have done so far
				 * We do not clear the needs_wakeup flag,
				 * because we cannot tell if we were the
				 * only one waiting.
				 */
				vm_map_unlock(map);
				vm_map_unwire(map, start, s, user_wire);
				return(KERN_FAILURE);
			}

			/*
			 * Cannot avoid a lookup here. reset timestamp.
			 */
			last_timestamp = map->timestamp;

			/*
			 * The entry could have been clipped, look it up again.
			 * Worse that can happen is, it may not exist anymore.
			 */
			if (!vm_map_lookup_entry(map, s, &first_entry)) {
				if (!user_wire)
					panic("vm_map_wire: re-lookup failed");

				/*
				 * User: undo everything upto the previous
				 * entry.  let vm_map_unwire worry about
				 * checking the validity of the range.
				 */
				vm_map_unlock(map);
				vm_map_unwire(map, start, s, user_wire);
				return(KERN_FAILURE);
			}
			entry = first_entry;
			continue;
		}
		
		if(entry->is_sub_map) {
			vm_map_offset_t	sub_start;
			vm_map_offset_t	sub_end;
			vm_map_offset_t	local_start;
			vm_map_offset_t	local_end;
			pmap_t		pmap;
			
			vm_map_clip_start(map, entry, start);
			vm_map_clip_end(map, entry, end);

			sub_start = entry->offset;
			sub_end = entry->vme_end - entry->vme_start;
			sub_end += entry->offset;
			
			local_end = entry->vme_end;
			if(map_pmap == NULL) {
				if(entry->use_pmap) {
					pmap = entry->object.sub_map->pmap;
					/* ppc implementation requires that */
					/* submaps pmap address ranges line */
					/* up with parent map */
#ifdef notdef
					pmap_addr = sub_start;
#endif
					pmap_addr = start;
				} else {
					pmap = map->pmap;
					pmap_addr = start;
				}
				if (entry->wired_count) {
					if (entry->wired_count 
							>= MAX_WIRE_COUNT)
					panic("vm_map_wire: too many wirings");

					if (user_wire &&
			    			entry->user_wired_count 
							>= MAX_WIRE_COUNT) {
				   	   vm_map_unlock(map);
				   	   vm_map_unwire(map, start,
						entry->vme_start, user_wire);
				   	   return(KERN_FAILURE);
					}
					if(user_wire)
						entry->user_wired_count++;
					if((!user_wire) ||
					     (entry->user_wired_count == 0))
						entry->wired_count++;
					entry = entry->vme_next;
					continue;

				} else {
					vm_object_t		object;
					vm_map_offset_t		offset_hi;
					vm_map_offset_t		offset_lo;
					vm_object_offset_t	offset;
					vm_prot_t		prot;
					boolean_t		wired;
					vm_behavior_t		behavior;
					vm_map_entry_t		local_entry;
					vm_map_version_t	 version;
					vm_map_t		lookup_map;

					/* call vm_map_lookup_locked to */
					/* cause any needs copy to be   */
					/* evaluated */
					local_start = entry->vme_start;
					lookup_map = map;
					vm_map_lock_write_to_read(map);
					if(vm_map_lookup_locked(
						&lookup_map, local_start, 
						access_type,
						&version, &object,
						&offset, &prot, &wired,
						&behavior, &offset_lo,
						&offset_hi, &real_map)) {
						
						vm_map_unlock_read(lookup_map);
				   	   	vm_map_unwire(map, start,
						   entry->vme_start, user_wire);
				   	   	return(KERN_FAILURE);
					}
					if(real_map != lookup_map)
						vm_map_unlock(real_map);
					vm_map_unlock_read(lookup_map);
					vm_map_lock(map);
					vm_object_unlock(object);

					if (!vm_map_lookup_entry(map, 
						local_start, &local_entry)) {
						vm_map_unlock(map);
				   	   	vm_map_unwire(map, start,
						   entry->vme_start, user_wire);
				   	   	return(KERN_FAILURE);
					}
					/* did we have a change of type? */
					if (!local_entry->is_sub_map) {
						last_timestamp = map->timestamp;
						continue;
					}
					entry = local_entry;
					if (user_wire)
						entry->user_wired_count++;
					if((!user_wire) || 
						(entry->user_wired_count == 1))
					entry->wired_count++;

					entry->in_transition = TRUE;

					vm_map_unlock(map);
					rc = vm_map_wire_nested(
						entry->object.sub_map, 
						sub_start, sub_end,
						access_type, 
						user_wire, pmap, pmap_addr);
					vm_map_lock(map);
				}
			} else {
				local_start = entry->vme_start;
				if (user_wire)
					entry->user_wired_count++;
				if((!user_wire) || 
					(entry->user_wired_count == 1))
					entry->wired_count++;
				vm_map_unlock(map);
				rc = vm_map_wire_nested(entry->object.sub_map, 
						sub_start, sub_end,
						access_type, 
						user_wire, map_pmap, pmap_addr);
				vm_map_lock(map);
			}
			s = entry->vme_start;
			e = entry->vme_end;

			/*
			 * Find the entry again.  It could have been clipped
			 * after we unlocked the map.
			 */
			if (!vm_map_lookup_entry(map, local_start,
						 &first_entry))
				panic("vm_map_wire: re-lookup failed");
			entry = first_entry;

			last_timestamp = map->timestamp;
			while ((entry != vm_map_to_entry(map)) &&
		       		     (entry->vme_start < e)) {
				assert(entry->in_transition);
				entry->in_transition = FALSE;
				if (entry->needs_wakeup) {
					entry->needs_wakeup = FALSE;
					need_wakeup = TRUE;
				}
				if (rc != KERN_SUCCESS) {/* from vm_*_wire */
					if (user_wire)
						entry->user_wired_count--;
					if ((!user_wire) || 
						(entry->user_wired_count == 0))
						entry->wired_count--;
				}
				entry = entry->vme_next;
			}
			if (rc != KERN_SUCCESS) {	/* from vm_*_wire */
				vm_map_unlock(map);
				if (need_wakeup)
					vm_map_entry_wakeup(map);
				/*
				 * undo everything upto the previous entry.
				 */
				(void)vm_map_unwire(map, start, s, user_wire);
				return rc;
			}
			continue;
		}

		/*
		 * If this entry is already wired then increment
		 * the appropriate wire reference count.
		 */
		if (entry->wired_count) {
			/* sanity check: wired_count is a short */
			if (entry->wired_count >= MAX_WIRE_COUNT)
				panic("vm_map_wire: too many wirings");

			if (user_wire &&
			    entry->user_wired_count >= MAX_WIRE_COUNT) {
				vm_map_unlock(map);
				vm_map_unwire(map, start,
						entry->vme_start, user_wire);
				return(KERN_FAILURE);
			}
			/*
			 * entry is already wired down, get our reference
			 * after clipping to our range.
			 */
			vm_map_clip_start(map, entry, start);
			vm_map_clip_end(map, entry, end);
			if (user_wire)
				entry->user_wired_count++;
			if ((!user_wire) || (entry->user_wired_count == 1))
				entry->wired_count++;

			entry = entry->vme_next;
			continue;
		}

		/*
		 * Unwired entry or wire request transmitted via submap
		 */


		/*
		 * Perform actions of vm_map_lookup that need the write
		 * lock on the map: create a shadow object for a
		 * copy-on-write region, or an object for a zero-fill
		 * region.
		 */
		size = entry->vme_end - entry->vme_start;
		/*
		 * If wiring a copy-on-write page, we need to copy it now
		 * even if we're only (currently) requesting read access.
		 * This is aggressive, but once it's wired we can't move it.
		 */
		if (entry->needs_copy) {
			vm_object_shadow(&entry->object.vm_object,
					 &entry->offset, size);
			entry->needs_copy = FALSE;
		} else if (entry->object.vm_object == VM_OBJECT_NULL) {
			entry->object.vm_object = vm_object_allocate(size);
			entry->offset = (vm_object_offset_t)0;
		}

		vm_map_clip_start(map, entry, start);
		vm_map_clip_end(map, entry, end);

		s = entry->vme_start;
		e = entry->vme_end;

		/*
		 * Check for holes and protection mismatch.
		 * Holes: Next entry should be contiguous unless this
		 *	  is the end of the region.
		 * Protection: Access requested must be allowed, unless
		 *	wiring is by protection class
		 */
		if ((((entry->vme_end < end) &&
		     ((entry->vme_next == vm_map_to_entry(map)) ||
		      (entry->vme_next->vme_start > entry->vme_end))) ||
		     ((entry->protection & access_type) != access_type))) {
			/*
			 * Found a hole or protection problem.
			 * Unwire the region we wired so far.
			 */
			if (start != entry->vme_start) {
				vm_map_unlock(map);
				vm_map_unwire(map, start, s, user_wire);
			} else {
				vm_map_unlock(map);
			}
			return((entry->protection&access_type) != access_type?
				KERN_PROTECTION_FAILURE: KERN_INVALID_ADDRESS);
		}

		assert(entry->wired_count == 0 && entry->user_wired_count == 0);

		if (user_wire)
			entry->user_wired_count++;
		if ((!user_wire) || (entry->user_wired_count == 1))
			entry->wired_count++;

		entry->in_transition = TRUE;

		/*
		 * This entry might get split once we unlock the map.
		 * In vm_fault_wire(), we need the current range as
		 * defined by this entry.  In order for this to work
		 * along with a simultaneous clip operation, we make a
		 * temporary copy of this entry and use that for the
		 * wiring.  Note that the underlying objects do not
		 * change during a clip.
		 */
		tmp_entry = *entry;

		/*
		 * The in_transition state guarentees that the entry
		 * (or entries for this range, if split occured) will be
		 * there when the map lock is acquired for the second time.
		 */
		vm_map_unlock(map);

		if (!user_wire && cur_thread != THREAD_NULL)
			interruptible_state = thread_interrupt_level(THREAD_UNINT);
		else
			interruptible_state = THREAD_UNINT;

		if(map_pmap)
			rc = vm_fault_wire(map, 
					&tmp_entry, map_pmap, pmap_addr);
		else
			rc = vm_fault_wire(map, 
					&tmp_entry, map->pmap, 
					tmp_entry.vme_start);

		if (!user_wire && cur_thread != THREAD_NULL)
			thread_interrupt_level(interruptible_state);

		vm_map_lock(map);

		if (last_timestamp+1 != map->timestamp) {
			/*
			 * Find the entry again.  It could have been clipped
			 * after we unlocked the map.
			 */
			if (!vm_map_lookup_entry(map, tmp_entry.vme_start,
								&first_entry))
				panic("vm_map_wire: re-lookup failed");

			entry = first_entry;
		}

		last_timestamp = map->timestamp;

		while ((entry != vm_map_to_entry(map)) &&
		       (entry->vme_start < tmp_entry.vme_end)) {
			assert(entry->in_transition);
			entry->in_transition = FALSE;
			if (entry->needs_wakeup) {
				entry->needs_wakeup = FALSE;
				need_wakeup = TRUE;
			}
			if (rc != KERN_SUCCESS) {	/* from vm_*_wire */
				if (user_wire)
					entry->user_wired_count--;
				if ((!user_wire) || 
						(entry->user_wired_count == 0))
					entry->wired_count--;
			}
			entry = entry->vme_next;
		}

		if (rc != KERN_SUCCESS) {		/* from vm_*_wire */
			vm_map_unlock(map);
			if (need_wakeup)
				vm_map_entry_wakeup(map);
			/*
			 * undo everything upto the previous entry.
			 */
			(void)vm_map_unwire(map, start, s, user_wire);
			return rc;
		}
	} /* end while loop through map entries */
	vm_map_unlock(map);

	/*
	 * wake up anybody waiting on entries we wired.
	 */
	if (need_wakeup)
		vm_map_entry_wakeup(map);

	return(KERN_SUCCESS);

}

kern_return_t
vm_map_wire(
	register vm_map_t	map,
	register vm_map_offset_t	start,
	register vm_map_offset_t	end,
	register vm_prot_t	access_type,
	boolean_t		user_wire)
{

	kern_return_t	kret;

#ifdef ppc
        /*
	 * the calls to mapping_prealloc and mapping_relpre
	 * (along with the VM_MAP_RANGE_CHECK to insure a
	 * resonable range was passed in) are
	 * currently necessary because
	 * we haven't enabled kernel pre-emption
	 * and/or the pmap_enter cannot purge and re-use
	 * existing mappings
	 */
	VM_MAP_RANGE_CHECK(map, start, end);
        mapping_prealloc(end - start);
#endif
	kret = vm_map_wire_nested(map, start, end, access_type, 
						user_wire, (pmap_t)NULL, 0);
#ifdef ppc
	mapping_relpre();
#endif
	return kret;
}

/*
 *	vm_map_unwire:
 *
 *	Sets the pageability of the specified address range in the target
 *	as pageable.  Regions specified must have been wired previously.
 *
 *	The map must not be locked, but a reference must remain to the map
 *	throughout the call.
 *
 *	Kernel will panic on failures.  User unwire ignores holes and
 *	unwired and intransition entries to avoid losing memory by leaving
 *	it unwired.
 */
static kern_return_t
vm_map_unwire_nested(
	register vm_map_t	map,
	register vm_map_offset_t	start,
	register vm_map_offset_t	end,
	boolean_t		user_wire,
	pmap_t			map_pmap,
	vm_map_offset_t		pmap_addr)
{
	register vm_map_entry_t	entry;
	struct vm_map_entry	*first_entry, tmp_entry;
	boolean_t		need_wakeup;
	boolean_t		main_map = FALSE;
	unsigned int		last_timestamp;

	vm_map_lock(map);
	if(map_pmap == NULL)
		main_map = TRUE;
	last_timestamp = map->timestamp;

	VM_MAP_RANGE_CHECK(map, start, end);
	assert(page_aligned(start));
	assert(page_aligned(end));

	if (vm_map_lookup_entry(map, start, &first_entry)) {
		entry = first_entry;
		/*	vm_map_clip_start will be done later. */
	}
	else {
		/*	Start address is not in map. */
		vm_map_unlock(map);
		return(KERN_INVALID_ADDRESS);
	}

	need_wakeup = FALSE;
	while ((entry != vm_map_to_entry(map)) && (entry->vme_start < end)) {
		if (entry->in_transition) {
			/*
			 * 1)
			 * Another thread is wiring down this entry. Note
			 * that if it is not for the other thread we would
			 * be unwiring an unwired entry.  This is not
			 * permitted.  If we wait, we will be unwiring memory
			 * we did not wire.
			 *
			 * 2)
			 * Another thread is unwiring this entry.  We did not
			 * have a reference to it, because if we did, this
			 * entry will not be getting unwired now.
			 */
			if (!user_wire)
				panic("vm_map_unwire: in_transition entry");

			entry = entry->vme_next;
			continue;
		}

		if(entry->is_sub_map) {
			vm_map_offset_t	sub_start;
			vm_map_offset_t	sub_end;
			vm_map_offset_t	local_end;
			pmap_t		pmap;
			

			vm_map_clip_start(map, entry, start);
			vm_map_clip_end(map, entry, end);

			sub_start = entry->offset;
			sub_end = entry->vme_end - entry->vme_start;
			sub_end += entry->offset;
			local_end = entry->vme_end;
			if(map_pmap == NULL) {
			   if(entry->use_pmap) {
					pmap = entry->object.sub_map->pmap;
					pmap_addr = sub_start;
			   } else {
					pmap = map->pmap;
					pmap_addr = start;
			   }
			   if (entry->wired_count == 0 ||
		   	       (user_wire && entry->user_wired_count == 0)) {
				if (!user_wire)
				   panic("vm_map_unwire: entry is unwired");
			      entry = entry->vme_next;
			      continue;
			   }

			   /*
		 	    * Check for holes
		 	    * Holes: Next entry should be contiguous unless
		 	    * this is the end of the region.
		 	    */
			   if (((entry->vme_end < end) && 
		    		((entry->vme_next == vm_map_to_entry(map)) ||
		     		(entry->vme_next->vme_start 
						> entry->vme_end)))) {
				if (!user_wire)
				  panic("vm_map_unwire: non-contiguous region");
/*
				entry = entry->vme_next;
				continue;
*/
			   }

			   if (!user_wire || (--entry->user_wired_count == 0))
				entry->wired_count--;

			   if (entry->wired_count != 0) {
				entry = entry->vme_next;
				continue;
			   }

			   entry->in_transition = TRUE;
			   tmp_entry = *entry;/* see comment in vm_map_wire() */

			   /*
		 	    * We can unlock the map now. The in_transition state
		 	    * guarantees existance of the entry.
		 	    */
			   vm_map_unlock(map);
			   vm_map_unwire_nested(entry->object.sub_map, 
				sub_start, sub_end, user_wire, pmap, pmap_addr);
			   vm_map_lock(map);

			   if (last_timestamp+1 != map->timestamp) {
				/*
				 * Find the entry again.  It could have been 
				 * clipped or deleted after we unlocked the map.
			 	 */
				if (!vm_map_lookup_entry(map, 
						tmp_entry.vme_start,
							&first_entry)) {
					if (!user_wire)
				          panic("vm_map_unwire: re-lookup failed");
					entry = first_entry->vme_next;
				} else
					entry = first_entry;
			   }
			   last_timestamp = map->timestamp;

			   /*
		 	    * clear transition bit for all constituent entries
		 	    * that were in the original entry (saved in 
			    * tmp_entry).  Also check for waiters.
		 	    */
			   while ((entry != vm_map_to_entry(map)) &&
		       		(entry->vme_start < tmp_entry.vme_end)) {
				assert(entry->in_transition);
				entry->in_transition = FALSE;
				if (entry->needs_wakeup) {
					entry->needs_wakeup = FALSE;
					need_wakeup = TRUE;
				}
				entry = entry->vme_next;
			   }
			   continue;
			} else {
			   vm_map_unlock(map);
			   vm_map_unwire_nested(entry->object.sub_map,
			   	sub_start, sub_end, user_wire, map_pmap,
				pmap_addr);
			   vm_map_lock(map);

			   if (last_timestamp+1 != map->timestamp) {
				/*
				 * Find the entry again.  It could have been 
				 * clipped or deleted after we unlocked the map.
			 	 */
				if (!vm_map_lookup_entry(map, 
						tmp_entry.vme_start,
							&first_entry)) {
					if (!user_wire)
				          panic("vm_map_unwire: re-lookup failed");
					entry = first_entry->vme_next;
				} else
					entry = first_entry;
			   }
			   last_timestamp = map->timestamp;
			}
		}


		if ((entry->wired_count == 0) ||
		   (user_wire && entry->user_wired_count == 0)) {
			if (!user_wire)
				panic("vm_map_unwire: entry is unwired");

			entry = entry->vme_next;
			continue;
		}
		
		assert(entry->wired_count > 0 &&
			(!user_wire || entry->user_wired_count > 0));

		vm_map_clip_start(map, entry, start);
		vm_map_clip_end(map, entry, end);

		/*
		 * Check for holes
		 * Holes: Next entry should be contiguous unless
		 *	  this is the end of the region.
		 */
		if (((entry->vme_end < end) && 
		    ((entry->vme_next == vm_map_to_entry(map)) ||
		     (entry->vme_next->vme_start > entry->vme_end)))) {

			if (!user_wire)
				panic("vm_map_unwire: non-contiguous region");
			entry = entry->vme_next;
			continue;
		}

		if (!user_wire || (--entry->user_wired_count == 0))
			entry->wired_count--;

		if (entry->wired_count != 0) {
			entry = entry->vme_next;
			continue;
		}

		entry->in_transition = TRUE;
		tmp_entry = *entry;	/* see comment in vm_map_wire() */

		/*
		 * We can unlock the map now. The in_transition state
		 * guarantees existance of the entry.
		 */
		vm_map_unlock(map);
		if(map_pmap) {
			vm_fault_unwire(map, 
				&tmp_entry, FALSE, map_pmap, pmap_addr);
		} else {
			vm_fault_unwire(map, 
				&tmp_entry, FALSE, map->pmap, 
				tmp_entry.vme_start);
		}
		vm_map_lock(map);

		if (last_timestamp+1 != map->timestamp) {
			/*
			 * Find the entry again.  It could have been clipped
			 * or deleted after we unlocked the map.
			 */
			if (!vm_map_lookup_entry(map, tmp_entry.vme_start,
								&first_entry)) {
				if (!user_wire)
				       panic("vm_map_unwire: re-lookup failed");
				entry = first_entry->vme_next;
			} else
				entry = first_entry;
		}
		last_timestamp = map->timestamp;

		/*
		 * clear transition bit for all constituent entries that
		 * were in the original entry (saved in tmp_entry).  Also
		 * check for waiters.
		 */
		while ((entry != vm_map_to_entry(map)) &&
		       (entry->vme_start < tmp_entry.vme_end)) {
			assert(entry->in_transition);
			entry->in_transition = FALSE;
			if (entry->needs_wakeup) {
				entry->needs_wakeup = FALSE;
				need_wakeup = TRUE;
			}
			entry = entry->vme_next;
		}
	}

	/*
	 * We might have fragmented the address space when we wired this
	 * range of addresses.  Attempt to re-coalesce these VM map entries
	 * with their neighbors now that they're no longer wired.
	 * Under some circumstances, address space fragmentation can
	 * prevent VM object shadow chain collapsing, which can cause
	 * swap space leaks.
	 */
	vm_map_simplify_range(map, start, end);

	vm_map_unlock(map);
	/*
	 * wake up anybody waiting on entries that we have unwired.
	 */
	if (need_wakeup)
		vm_map_entry_wakeup(map);
	return(KERN_SUCCESS);

}

kern_return_t
vm_map_unwire(
	register vm_map_t	map,
	register vm_map_offset_t	start,
	register vm_map_offset_t	end,
	boolean_t		user_wire)
{
	return vm_map_unwire_nested(map, start, end, 
					user_wire, (pmap_t)NULL, 0);
}


/*
 *	vm_map_entry_delete:	[ internal use only ]
 *
 *	Deallocate the given entry from the target map.
 */		
static void
vm_map_entry_delete(
	register vm_map_t	map,
	register vm_map_entry_t	entry)
{
	register vm_map_offset_t	s, e;
	register vm_object_t	object;
	register vm_map_t	submap;

	s = entry->vme_start;
	e = entry->vme_end;
	assert(page_aligned(s));
	assert(page_aligned(e));
	assert(entry->wired_count == 0);
	assert(entry->user_wired_count == 0);

	if (entry->is_sub_map) {
		object = NULL;
		submap = entry->object.sub_map;
	} else {
		submap = NULL;
		object = entry->object.vm_object;
	}

	vm_map_entry_unlink(map, entry);
	map->size -= e - s;

	vm_map_entry_dispose(map, entry);

	vm_map_unlock(map);
	/*
	 *	Deallocate the object only after removing all
	 *	pmap entries pointing to its pages.
	 */
	if (submap)
		vm_map_deallocate(submap);
	else
	 	vm_object_deallocate(object);

}

void
vm_map_submap_pmap_clean(
	vm_map_t	map,
	vm_map_offset_t	start,
	vm_map_offset_t	end,
	vm_map_t	sub_map,
	vm_map_offset_t	offset)
{
	vm_map_offset_t	submap_start;
	vm_map_offset_t	submap_end;
	vm_map_size_t	remove_size;
	vm_map_entry_t	entry;

	submap_end = offset + (end - start);
	submap_start = offset;
	if(vm_map_lookup_entry(sub_map, offset, &entry)) {
			
		remove_size = (entry->vme_end - entry->vme_start);
		if(offset > entry->vme_start)
			remove_size -= offset - entry->vme_start;
			

		if(submap_end < entry->vme_end) {
			remove_size -=
				entry->vme_end - submap_end;
		}
		if(entry->is_sub_map) {
			vm_map_submap_pmap_clean(
				sub_map,
				start,
				start + remove_size,
				entry->object.sub_map,
				entry->offset);
		} else {

			if((map->mapped) && (map->ref_count)
				&& (entry->object.vm_object != NULL)) {
				vm_object_pmap_protect(
					entry->object.vm_object,
					entry->offset,
					remove_size,
					PMAP_NULL,
					entry->vme_start,
					VM_PROT_NONE);
			} else {
				pmap_remove(map->pmap, 
					(addr64_t)start, 
					(addr64_t)(start + remove_size));
			}
		}
	}

	entry = entry->vme_next;
	
	while((entry != vm_map_to_entry(sub_map)) 
			&& (entry->vme_start < submap_end)) {
		remove_size = (entry->vme_end - entry->vme_start); 
		if(submap_end < entry->vme_end) {
			remove_size -= entry->vme_end - submap_end;
		}
		if(entry->is_sub_map) {
			vm_map_submap_pmap_clean(
				sub_map,
				(start + entry->vme_start) - offset,
				((start + entry->vme_start) - offset) + remove_size,
				entry->object.sub_map,
				entry->offset);
		} else {
			if((map->mapped) && (map->ref_count)
				&& (entry->object.vm_object != NULL)) {
				vm_object_pmap_protect(
					entry->object.vm_object,
					entry->offset,
					remove_size,
					PMAP_NULL,
					entry->vme_start,
					VM_PROT_NONE);
			} else {
				pmap_remove(map->pmap, 
				   (addr64_t)((start + entry->vme_start) 
								- offset),
				   (addr64_t)(((start + entry->vme_start) 
					- offset) + remove_size));
			}
		}
		entry = entry->vme_next;
	} 
	return;
}

/*
 *	vm_map_delete:	[ internal use only ]
 *
 *	Deallocates the given address range from the target map.
 *	Removes all user wirings. Unwires one kernel wiring if
 *	VM_MAP_REMOVE_KUNWIRE is set.  Waits for kernel wirings to go
 *	away if VM_MAP_REMOVE_WAIT_FOR_KWIRE is set.  Sleeps
 *	interruptibly if VM_MAP_REMOVE_INTERRUPTIBLE is set.
 *
 *	This routine is called with map locked and leaves map locked.
 */
static kern_return_t
vm_map_delete(
	vm_map_t		map,
	vm_map_offset_t		start,
	vm_map_offset_t		end,
	int			flags,
	vm_map_t		zap_map)
{
	vm_map_entry_t		entry, next;
	struct	 vm_map_entry	*first_entry, tmp_entry;
	register vm_map_offset_t	s, e;
	register vm_object_t	object;
	boolean_t		need_wakeup;
	unsigned int		last_timestamp = ~0; /* unlikely value */
	int			interruptible;

	interruptible = (flags & VM_MAP_REMOVE_INTERRUPTIBLE) ? 
	  		THREAD_ABORTSAFE : THREAD_UNINT;

	/*
	 * All our DMA I/O operations in IOKit are currently done by
	 * wiring through the map entries of the task requesting the I/O.
	 * Because of this, we must always wait for kernel wirings
	 * to go away on the entries before deleting them.
	 *
	 * Any caller who wants to actually remove a kernel wiring
	 * should explicitly set the VM_MAP_REMOVE_KUNWIRE flag to
	 * properly remove one wiring instead of blasting through
	 * them all.
	 */
	flags |= VM_MAP_REMOVE_WAIT_FOR_KWIRE;

	/*
	 *	Find the start of the region, and clip it
	 */
	if (vm_map_lookup_entry(map, start, &first_entry)) {
		entry = first_entry;
		vm_map_clip_start(map, entry, start);

		/*
		 *	Fix the lookup hint now, rather than each
		 *	time through the loop.
		 */
		SAVE_HINT(map, entry->vme_prev);
	} else {
		entry = first_entry->vme_next;
	}

	need_wakeup = FALSE;
	/*
	 *	Step through all entries in this region
	 */
	while ((entry != vm_map_to_entry(map)) && (entry->vme_start < end)) {

		vm_map_clip_end(map, entry, end);
		if (entry->in_transition) {
			wait_result_t wait_result;

			/*
			 * Another thread is wiring/unwiring this entry.
			 * Let the other thread know we are waiting.
			 */
			s = entry->vme_start;
			entry->needs_wakeup = TRUE;

			/*
			 * wake up anybody waiting on entries that we have
			 * already unwired/deleted.
			 */
			if (need_wakeup) {
				vm_map_entry_wakeup(map);
				need_wakeup = FALSE;
			}

			wait_result = vm_map_entry_wait(map, interruptible);

			if (interruptible &&
			    wait_result == THREAD_INTERRUPTED) {
				/*
				 * We do not clear the needs_wakeup flag,
				 * since we cannot tell if we were the only one.
				 */
				vm_map_unlock(map);
				return KERN_ABORTED;
			}

			/*
			 * The entry could have been clipped or it
			 * may not exist anymore.  Look it up again.
			 */
			if (!vm_map_lookup_entry(map, s, &first_entry)) {
				assert((map != kernel_map) && 
				       (!entry->is_sub_map));
				/*
				 * User: use the next entry
				 */
				entry = first_entry->vme_next;
			} else {
				entry = first_entry;
				SAVE_HINT(map, entry->vme_prev);
			}
			last_timestamp = map->timestamp;
			continue;
		} /* end in_transition */

		if (entry->wired_count) {
			/*
			 * 	Remove a kernel wiring if requested or if
			 *	there are user wirings.
			 */
			if ((flags & VM_MAP_REMOVE_KUNWIRE) || 
			   (entry->user_wired_count > 0))
				entry->wired_count--;

			/* remove all user wire references */
			entry->user_wired_count = 0;

			if (entry->wired_count != 0) {
				assert((map != kernel_map) && 
				       (!entry->is_sub_map));
				/*
				 * Cannot continue.  Typical case is when
				 * a user thread has physical io pending on
				 * on this page.  Either wait for the
				 * kernel wiring to go away or return an
				 * error.
				 */
				if (flags & VM_MAP_REMOVE_WAIT_FOR_KWIRE) {
					wait_result_t wait_result;

					s = entry->vme_start;
					entry->needs_wakeup = TRUE;
					wait_result = vm_map_entry_wait(map,
								interruptible);

					if (interruptible &&
			   		    wait_result == THREAD_INTERRUPTED) {
						/*
				 	 	 * We do not clear the 
						 * needs_wakeup flag, since we 
						 * cannot tell if we were the 
						 * only one.
				 	 	 */
						vm_map_unlock(map);
						return KERN_ABORTED;
					}

					/*
			 		 * The entry could have been clipped or
					 * it may not exist anymore.  Look it
					 * up again.
			 		 */
					if (!vm_map_lookup_entry(map, s, 
								&first_entry)) {
						assert((map != kernel_map) && 
				       		(!entry->is_sub_map));
						/*
				 		 * User: use the next entry
				 		 */
						entry = first_entry->vme_next;
					} else {
						entry = first_entry;
						SAVE_HINT(map, entry->vme_prev);
					}
					last_timestamp = map->timestamp;
					continue;
				}
				else {
					return KERN_FAILURE;
				}
			}

			entry->in_transition = TRUE;
			/*
			 * copy current entry.  see comment in vm_map_wire()
			 */
			tmp_entry = *entry;
			s = entry->vme_start;
			e = entry->vme_end;

			/*
			 * We can unlock the map now. The in_transition
			 * state guarentees existance of the entry.
			 */
			vm_map_unlock(map);
			vm_fault_unwire(map, &tmp_entry,
				tmp_entry.object.vm_object == kernel_object,
				map->pmap, tmp_entry.vme_start);
			vm_map_lock(map);

			if (last_timestamp+1 != map->timestamp) {
				/*
				 * Find the entry again.  It could have
				 * been clipped after we unlocked the map.
				 */
				if (!vm_map_lookup_entry(map, s, &first_entry)){
					assert((map != kernel_map) && 
				       	       (!entry->is_sub_map));
					first_entry = first_entry->vme_next;
				} else {
					SAVE_HINT(map, entry->vme_prev);
				}
			} else {
				SAVE_HINT(map, entry->vme_prev);
				first_entry = entry;
			}

			last_timestamp = map->timestamp;

			entry = first_entry;
			while ((entry != vm_map_to_entry(map)) &&
			       (entry->vme_start < tmp_entry.vme_end)) {
				assert(entry->in_transition);
				entry->in_transition = FALSE;
				if (entry->needs_wakeup) {
					entry->needs_wakeup = FALSE;
					need_wakeup = TRUE;
				}
				entry = entry->vme_next;
			}
			/*
			 * We have unwired the entry(s).  Go back and
			 * delete them.
			 */
			entry = first_entry;
			continue;
		}

		/* entry is unwired */
		assert(entry->wired_count == 0);
		assert(entry->user_wired_count == 0);

		if ((!entry->is_sub_map &&
		    entry->object.vm_object != kernel_object) ||
		    entry->is_sub_map) {
			if(entry->is_sub_map) {
			   if(entry->use_pmap) {
#ifndef i386
				pmap_unnest(map->pmap, (addr64_t)entry->vme_start);
#endif
				if((map->mapped) && (map->ref_count)) {
					/* clean up parent map/maps */
					vm_map_submap_pmap_clean(
						map, entry->vme_start,
						entry->vme_end,
						entry->object.sub_map,
						entry->offset);
				}
			   } else {
				vm_map_submap_pmap_clean(
					map, entry->vme_start, entry->vme_end,
					entry->object.sub_map,
					entry->offset);
			   }
			} else {
			   object = entry->object.vm_object;
			   if((map->mapped) && (map->ref_count)) {
			      vm_object_pmap_protect(
					object, entry->offset,
					entry->vme_end - entry->vme_start,
					PMAP_NULL,
					entry->vme_start,
					VM_PROT_NONE);
			   } else {
				   pmap_remove(map->pmap,
					       entry->vme_start,
					       entry->vme_end);
			   }
			}
		}

		/*
		 * All pmap mappings for this map entry must have been
		 * cleared by now.
		 */
		assert(vm_map_pmap_is_empty(map,
					    entry->vme_start,
					    entry->vme_end));

		next = entry->vme_next;
		s = next->vme_start;
		last_timestamp = map->timestamp;

		if ((flags & VM_MAP_REMOVE_SAVE_ENTRIES) &&
		    zap_map != VM_MAP_NULL) {
			/*
			 * The caller wants to save the affected VM map entries
			 * into the "zap_map".  The caller will take care of
			 * these entries.
			 */
			/* unlink the entry from "map" ... */
			vm_map_entry_unlink(map, entry);
			/* ... and add it to the end of the "zap_map" */
			vm_map_entry_link(zap_map,
					  vm_map_last_entry(zap_map),
					  entry);
		} else {
			vm_map_entry_delete(map, entry);
			/* vm_map_entry_delete unlocks the map */
			vm_map_lock(map);
		}

		entry = next;

		if(entry == vm_map_to_entry(map)) {
			break;
		}
		if (last_timestamp+1 != map->timestamp) {
			/*
			 * we are responsible for deleting everything
			 * from the give space, if someone has interfered
			 * we pick up where we left off, back fills should
			 * be all right for anyone except map_delete and
			 * we have to assume that the task has been fully
			 * disabled before we get here
			 */
        		if (!vm_map_lookup_entry(map, s, &entry)){
	               		entry = entry->vme_next;
        		} else {
				 SAVE_HINT(map, entry->vme_prev);
       		 	}
			/* 
			 * others can not only allocate behind us, we can 
			 * also see coalesce while we don't have the map lock 
			 */
			if(entry == vm_map_to_entry(map)) {
				break;
			}
			vm_map_clip_start(map, entry, s);
		}
		last_timestamp = map->timestamp;
	}

	if (map->wait_for_space)
		thread_wakeup((event_t) map);
	/*
	 * wake up anybody waiting on entries that we have already deleted.
	 */
	if (need_wakeup)
		vm_map_entry_wakeup(map);

	return KERN_SUCCESS;
}

/*
 *	vm_map_remove:
 *
 *	Remove the given address range from the target map.
 *	This is the exported form of vm_map_delete.
 */
kern_return_t
vm_map_remove(
	register vm_map_t	map,
	register vm_map_offset_t	start,
	register vm_map_offset_t	end,
	register boolean_t	flags)
{
	register kern_return_t	result;

	vm_map_lock(map);
	VM_MAP_RANGE_CHECK(map, start, end);
	result = vm_map_delete(map, start, end, flags, VM_MAP_NULL);
	vm_map_unlock(map);

	return(result);
}


/*
 *	Routine:	vm_map_copy_discard
 *
 *	Description:
 *		Dispose of a map copy object (returned by
 *		vm_map_copyin).
 */
void
vm_map_copy_discard(
	vm_map_copy_t	copy)
{
	TR_DECL("vm_map_copy_discard");

/*	tr3("enter: copy 0x%x type %d", copy, copy->type);*/

	if (copy == VM_MAP_COPY_NULL)
		return;

	switch (copy->type) {
	case VM_MAP_COPY_ENTRY_LIST:
		while (vm_map_copy_first_entry(copy) !=
					vm_map_copy_to_entry(copy)) {
			vm_map_entry_t	entry = vm_map_copy_first_entry(copy);

			vm_map_copy_entry_unlink(copy, entry);
			vm_object_deallocate(entry->object.vm_object);
			vm_map_copy_entry_dispose(copy, entry);
		}
		break;
        case VM_MAP_COPY_OBJECT:
		vm_object_deallocate(copy->cpy_object);
		break;
	case VM_MAP_COPY_KERNEL_BUFFER:

		/*
		 * The vm_map_copy_t and possibly the data buffer were
		 * allocated by a single call to kalloc(), i.e. the
		 * vm_map_copy_t was not allocated out of the zone.
		 */
		kfree(copy, copy->cpy_kalloc_size);
		return;
	}
	zfree(vm_map_copy_zone, copy);
}

/*
 *	Routine:	vm_map_copy_copy
 *
 *	Description:
 *			Move the information in a map copy object to
 *			a new map copy object, leaving the old one
 *			empty.
 *
 *			This is used by kernel routines that need
 *			to look at out-of-line data (in copyin form)
 *			before deciding whether to return SUCCESS.
 *			If the routine returns FAILURE, the original
 *			copy object will be deallocated; therefore,
 *			these routines must make a copy of the copy
 *			object and leave the original empty so that
 *			deallocation will not fail.
 */
vm_map_copy_t
vm_map_copy_copy(
	vm_map_copy_t	copy)
{
	vm_map_copy_t	new_copy;

	if (copy == VM_MAP_COPY_NULL)
		return VM_MAP_COPY_NULL;

	/*
	 * Allocate a new copy object, and copy the information
	 * from the old one into it.
	 */

	new_copy = (vm_map_copy_t) zalloc(vm_map_copy_zone);
	*new_copy = *copy;

	if (copy->type == VM_MAP_COPY_ENTRY_LIST) {
		/*
		 * The links in the entry chain must be
		 * changed to point to the new copy object.
		 */
		vm_map_copy_first_entry(copy)->vme_prev
			= vm_map_copy_to_entry(new_copy);
		vm_map_copy_last_entry(copy)->vme_next
			= vm_map_copy_to_entry(new_copy);
	}

	/*
	 * Change the old copy object into one that contains
	 * nothing to be deallocated.
	 */
	copy->type = VM_MAP_COPY_OBJECT;
	copy->cpy_object = VM_OBJECT_NULL;

	/*
	 * Return the new object.
	 */
	return new_copy;
}

static kern_return_t
vm_map_overwrite_submap_recurse(
	vm_map_t	dst_map,
	vm_map_offset_t	dst_addr,
	vm_map_size_t	dst_size)
{
	vm_map_offset_t	dst_end;
	vm_map_entry_t	tmp_entry;
	vm_map_entry_t	entry;
	kern_return_t	result;
	boolean_t	encountered_sub_map = FALSE;



	/*
	 *	Verify that the destination is all writeable
	 *	initially.  We have to trunc the destination
	 *	address and round the copy size or we'll end up
	 *	splitting entries in strange ways.
	 */

	dst_end = vm_map_round_page(dst_addr + dst_size);
	vm_map_lock(dst_map);

start_pass_1:
	if (!vm_map_lookup_entry(dst_map, dst_addr, &tmp_entry)) {
		vm_map_unlock(dst_map);
		return(KERN_INVALID_ADDRESS);
	}

	vm_map_clip_start(dst_map, tmp_entry, vm_map_trunc_page(dst_addr));

	for (entry = tmp_entry;;) {
		vm_map_entry_t	next;

		next = entry->vme_next;
		while(entry->is_sub_map) {
			vm_map_offset_t	sub_start;
			vm_map_offset_t	sub_end;
			vm_map_offset_t	local_end;

			if (entry->in_transition) {
                        /*
                         * Say that we are waiting, and wait for entry.
                         */
                        	entry->needs_wakeup = TRUE;
                        	vm_map_entry_wait(dst_map, THREAD_UNINT);

				goto start_pass_1;
			}

			encountered_sub_map = TRUE;
			sub_start = entry->offset;

			if(entry->vme_end < dst_end)
				sub_end = entry->vme_end;
			else 
				sub_end = dst_end;
			sub_end -= entry->vme_start;
			sub_end += entry->offset;
			local_end = entry->vme_end;
			vm_map_unlock(dst_map);
			
			result = vm_map_overwrite_submap_recurse(
					entry->object.sub_map,
					sub_start,
					sub_end - sub_start);

			if(result != KERN_SUCCESS)
				return result;
			if (dst_end <= entry->vme_end)
				return KERN_SUCCESS;
			vm_map_lock(dst_map);
			if(!vm_map_lookup_entry(dst_map, local_end, 
						&tmp_entry)) {
				vm_map_unlock(dst_map);
				return(KERN_INVALID_ADDRESS);
			}
			entry = tmp_entry;
			next = entry->vme_next;
		}

		if ( ! (entry->protection & VM_PROT_WRITE)) {
			vm_map_unlock(dst_map);
			return(KERN_PROTECTION_FAILURE);
		}

		/*
		 *	If the entry is in transition, we must wait
		 *	for it to exit that state.  Anything could happen
		 *	when we unlock the map, so start over.
		 */
                if (entry->in_transition) {

                        /*
                         * Say that we are waiting, and wait for entry.
                         */
                        entry->needs_wakeup = TRUE;
                        vm_map_entry_wait(dst_map, THREAD_UNINT);

			goto start_pass_1;
		}

/*
 *		our range is contained completely within this map entry
 */
		if (dst_end <= entry->vme_end) {
			vm_map_unlock(dst_map);
			return KERN_SUCCESS;
		}
/*
 *		check that range specified is contiguous region
 */
		if ((next == vm_map_to_entry(dst_map)) ||
		    (next->vme_start != entry->vme_end)) {
			vm_map_unlock(dst_map);
			return(KERN_INVALID_ADDRESS);
		}

		/*
		 *	Check for permanent objects in the destination.
		 */
		if ((entry->object.vm_object != VM_OBJECT_NULL) &&
			   ((!entry->object.vm_object->internal) ||
			   (entry->object.vm_object->true_share))) {
			if(encountered_sub_map) {
				vm_map_unlock(dst_map);
				return(KERN_FAILURE);
			}
		}


		entry = next;
	}/* for */
	vm_map_unlock(dst_map);
	return(KERN_SUCCESS);
}

/*
 *	Routine:	vm_map_copy_overwrite
 *
 *	Description:
 *		Copy the memory described by the map copy
 *		object (copy; returned by vm_map_copyin) onto
 *		the specified destination region (dst_map, dst_addr).
 *		The destination must be writeable.
 *
 *		Unlike vm_map_copyout, this routine actually
 *		writes over previously-mapped memory.  If the
 *		previous mapping was to a permanent (user-supplied)
 *		memory object, it is preserved.
 *
 *		The attributes (protection and inheritance) of the
 *		destination region are preserved.
 *
 *		If successful, consumes the copy object.
 *		Otherwise, the caller is responsible for it.
 *
 *	Implementation notes:
 *		To overwrite aligned temporary virtual memory, it is
 *		sufficient to remove the previous mapping and insert
 *		the new copy.  This replacement is done either on
 *		the whole region (if no permanent virtual memory
 *		objects are embedded in the destination region) or
 *		in individual map entries.
 *
 *		To overwrite permanent virtual memory , it is necessary
 *		to copy each page, as the external memory management
 *		interface currently does not provide any optimizations.
 *
 *		Unaligned memory also has to be copied.  It is possible
 *		to use 'vm_trickery' to copy the aligned data.  This is
 *		not done but not hard to implement.
 *
 *		Once a page of permanent memory has been overwritten,
 *		it is impossible to interrupt this function; otherwise,
 *		the call would be neither atomic nor location-independent.
 *		The kernel-state portion of a user thread must be
 *		interruptible.
 *
 *		It may be expensive to forward all requests that might
 *		overwrite permanent memory (vm_write, vm_copy) to
 *		uninterruptible kernel threads.  This routine may be
 *		called by interruptible threads; however, success is
 *		not guaranteed -- if the request cannot be performed
 *		atomically and interruptibly, an error indication is
 *		returned.
 */

static kern_return_t
vm_map_copy_overwrite_nested(
	vm_map_t		dst_map,
	vm_map_address_t	dst_addr,
	vm_map_copy_t		copy,
	boolean_t		interruptible,
	pmap_t			pmap)
{
	vm_map_offset_t		dst_end;
	vm_map_entry_t		tmp_entry;
	vm_map_entry_t		entry;
	kern_return_t		kr;
	boolean_t		aligned = TRUE;
	boolean_t		contains_permanent_objects = FALSE;
	boolean_t		encountered_sub_map = FALSE;
	vm_map_offset_t		base_addr;
	vm_map_size_t		copy_size;
	vm_map_size_t		total_size;


	/*
	 *	Check for null copy object.
	 */

	if (copy == VM_MAP_COPY_NULL)
		return(KERN_SUCCESS);

	/*
	 *	Check for special kernel buffer allocated
	 *	by new_ipc_kmsg_copyin.
	 */

	if (copy->type == VM_MAP_COPY_KERNEL_BUFFER) {
		return(vm_map_copyout_kernel_buffer(
						dst_map, &dst_addr, 
					    	copy, TRUE));
	}

	/*
	 *      Only works for entry lists at the moment.  Will
	 *	support page lists later.
	 */

	assert(copy->type == VM_MAP_COPY_ENTRY_LIST);

	if (copy->size == 0) {
		vm_map_copy_discard(copy);
		return(KERN_SUCCESS);
	}

	/*
	 *	Verify that the destination is all writeable
	 *	initially.  We have to trunc the destination
	 *	address and round the copy size or we'll end up
	 *	splitting entries in strange ways.
	 */

	if (!page_aligned(copy->size) ||
		!page_aligned (copy->offset) ||
		!page_aligned (dst_addr))
	{
		aligned = FALSE;
		dst_end = vm_map_round_page(dst_addr + copy->size);
	} else {
		dst_end = dst_addr + copy->size;
	}

	vm_map_lock(dst_map);

	/* LP64todo - remove this check when vm_map_commpage64()
	 * no longer has to stuff in a map_entry for the commpage
	 * above the map's max_offset.
	 */
	if (dst_addr >= dst_map->max_offset) {
		vm_map_unlock(dst_map);
		return(KERN_INVALID_ADDRESS);
	}
	 
start_pass_1:
	if (!vm_map_lookup_entry(dst_map, dst_addr, &tmp_entry)) {
		vm_map_unlock(dst_map);
		return(KERN_INVALID_ADDRESS);
	}
	vm_map_clip_start(dst_map, tmp_entry, vm_map_trunc_page(dst_addr));
	for (entry = tmp_entry;;) {
		vm_map_entry_t	next = entry->vme_next;

		while(entry->is_sub_map) {
			vm_map_offset_t	sub_start;
			vm_map_offset_t	sub_end;
			vm_map_offset_t	local_end;

                	if (entry->in_transition) {

                        /*
                         * Say that we are waiting, and wait for entry.
                         */
                        	entry->needs_wakeup = TRUE;
                        	vm_map_entry_wait(dst_map, THREAD_UNINT);

				goto start_pass_1;
			}

			local_end = entry->vme_end;
		        if (!(entry->needs_copy)) {
				/* if needs_copy we are a COW submap */
				/* in such a case we just replace so */
				/* there is no need for the follow-  */
				/* ing check.                        */
				encountered_sub_map = TRUE;
				sub_start = entry->offset;

				if(entry->vme_end < dst_end)
					sub_end = entry->vme_end;
				else 
					sub_end = dst_end;
				sub_end -= entry->vme_start;
				sub_end += entry->offset;
				vm_map_unlock(dst_map);
			
				kr = vm_map_overwrite_submap_recurse(
					entry->object.sub_map,
					sub_start,
					sub_end - sub_start);
				if(kr != KERN_SUCCESS)
					return kr;
				vm_map_lock(dst_map);
			}

			if (dst_end <= entry->vme_end)
				goto start_overwrite;
			if(!vm_map_lookup_entry(dst_map, local_end, 
						&entry)) {
				vm_map_unlock(dst_map);
				return(KERN_INVALID_ADDRESS);
			}
			next = entry->vme_next;
		}

		if ( ! (entry->protection & VM_PROT_WRITE)) {
			vm_map_unlock(dst_map);
			return(KERN_PROTECTION_FAILURE);
		}

		/*
		 *	If the entry is in transition, we must wait
		 *	for it to exit that state.  Anything could happen
		 *	when we unlock the map, so start over.
		 */
                if (entry->in_transition) {

                        /*
                         * Say that we are waiting, and wait for entry.
                         */
                        entry->needs_wakeup = TRUE;
                        vm_map_entry_wait(dst_map, THREAD_UNINT);

			goto start_pass_1;
		}

/*
 *		our range is contained completely within this map entry
 */
		if (dst_end <= entry->vme_end)
			break;
/*
 *		check that range specified is contiguous region
 */
		if ((next == vm_map_to_entry(dst_map)) ||
		    (next->vme_start != entry->vme_end)) {
			vm_map_unlock(dst_map);
			return(KERN_INVALID_ADDRESS);
		}


		/*
		 *	Check for permanent objects in the destination.
		 */
		if ((entry->object.vm_object != VM_OBJECT_NULL) &&
			   ((!entry->object.vm_object->internal) ||
			   (entry->object.vm_object->true_share))) {
			contains_permanent_objects = TRUE;
		}

		entry = next;
	}/* for */

start_overwrite:
	/*
	 *	If there are permanent objects in the destination, then
	 *	the copy cannot be interrupted.
	 */

	if (interruptible && contains_permanent_objects) {
		vm_map_unlock(dst_map);
		return(KERN_FAILURE);	/* XXX */
	}

	/*
 	 *
	 *	Make a second pass, overwriting the data
	 *	At the beginning of each loop iteration,
	 *	the next entry to be overwritten is "tmp_entry"
	 *	(initially, the value returned from the lookup above),
	 *	and the starting address expected in that entry
	 *	is "start".
	 */

	total_size = copy->size;
	if(encountered_sub_map) {
		copy_size = 0;
		/* re-calculate tmp_entry since we've had the map */
		/* unlocked */
		if (!vm_map_lookup_entry( dst_map, dst_addr, &tmp_entry)) {
			vm_map_unlock(dst_map);
			return(KERN_INVALID_ADDRESS);
		}
	} else {
		copy_size = copy->size;
	}
	
	base_addr = dst_addr;
	while(TRUE) {
		/* deconstruct the copy object and do in parts */
		/* only in sub_map, interruptable case */
		vm_map_entry_t	copy_entry;
		vm_map_entry_t	previous_prev = VM_MAP_ENTRY_NULL;
		vm_map_entry_t	next_copy = VM_MAP_ENTRY_NULL;
		int		nentries;
		int		remaining_entries = 0;
		int		new_offset = 0;
	
		for (entry = tmp_entry; copy_size == 0;) {
			vm_map_entry_t	next;

			next = entry->vme_next;

			/* tmp_entry and base address are moved along */
			/* each time we encounter a sub-map.  Otherwise */
			/* entry can outpase tmp_entry, and the copy_size */
			/* may reflect the distance between them */
			/* if the current entry is found to be in transition */
			/* we will start over at the beginning or the last */
			/* encounter of a submap as dictated by base_addr */
			/* we will zero copy_size accordingly. */
			if (entry->in_transition) {
                       		/*
                       		 * Say that we are waiting, and wait for entry.
                       		 */
                       		entry->needs_wakeup = TRUE;
                       		vm_map_entry_wait(dst_map, THREAD_UNINT);

				if(!vm_map_lookup_entry(dst_map, base_addr, 
								&tmp_entry)) {
					vm_map_unlock(dst_map);
					return(KERN_INVALID_ADDRESS);
				}
				copy_size = 0;
				entry = tmp_entry;
				continue;
			}
			if(entry->is_sub_map) {
				vm_map_offset_t	sub_start;
				vm_map_offset_t	sub_end;
				vm_map_offset_t	local_end;

		        	if (entry->needs_copy) {
					/* if this is a COW submap */
					/* just back the range with a */
					/* anonymous entry */
					if(entry->vme_end < dst_end)
						sub_end = entry->vme_end;
					else 
						sub_end = dst_end;
					if(entry->vme_start < base_addr)
						sub_start = base_addr;
					else 
						sub_start = entry->vme_start;
					vm_map_clip_end(
						dst_map, entry, sub_end);
					vm_map_clip_start(
						dst_map, entry, sub_start);
					entry->is_sub_map = FALSE;
					vm_map_deallocate(
						entry->object.sub_map);
					entry->object.sub_map = NULL;
					entry->is_shared = FALSE;
					entry->needs_copy = FALSE;
					entry->offset = 0;
					entry->protection = VM_PROT_ALL;
					entry->max_protection = VM_PROT_ALL;
					entry->wired_count = 0;
					entry->user_wired_count = 0;
					if(entry->inheritance 
							== VM_INHERIT_SHARE) 
					   entry->inheritance = VM_INHERIT_COPY;
					continue;
				}
				/* first take care of any non-sub_map */
				/* entries to send */
				if(base_addr < entry->vme_start) {
					/* stuff to send */
					copy_size = 
						entry->vme_start - base_addr;
					break;
				}
				sub_start = entry->offset;

				if(entry->vme_end < dst_end)
					sub_end = entry->vme_end;
				else 
					sub_end = dst_end;
				sub_end -= entry->vme_start;
				sub_end += entry->offset;
				local_end = entry->vme_end;
				vm_map_unlock(dst_map);
				copy_size = sub_end - sub_start;

				/* adjust the copy object */
				if (total_size > copy_size) {
					vm_map_size_t	local_size = 0;
					vm_map_size_t	entry_size;

				   nentries = 1;
				   new_offset = copy->offset;
				   copy_entry = vm_map_copy_first_entry(copy);
				   while(copy_entry != 
					         vm_map_copy_to_entry(copy)){
				       entry_size = copy_entry->vme_end - 
						      copy_entry->vme_start;
				       if((local_size < copy_size) &&
				       		((local_size + entry_size) 
					         >= copy_size)) {
				          vm_map_copy_clip_end(copy, 
					         copy_entry, 
					         copy_entry->vme_start +
					         (copy_size - local_size));
				          entry_size = copy_entry->vme_end - 
				  		         copy_entry->vme_start;
					  local_size += entry_size;
					  new_offset += entry_size;
				       }
				       if(local_size >= copy_size) {
				          next_copy = copy_entry->vme_next;
					  copy_entry->vme_next = 
					            vm_map_copy_to_entry(copy);
				          previous_prev = 
						   copy->cpy_hdr.links.prev;
					  copy->cpy_hdr.links.prev = copy_entry;
				          copy->size = copy_size;
					  remaining_entries = 
						        copy->cpy_hdr.nentries;
					  remaining_entries -= nentries;
					  copy->cpy_hdr.nentries = nentries;
				          break;
				       } else {
				          local_size += entry_size;
					  new_offset += entry_size;
					  nentries++;
				       }
				       copy_entry = copy_entry->vme_next;
				   }
				}
			
				if((entry->use_pmap) && (pmap == NULL)) {
					kr = vm_map_copy_overwrite_nested(
						entry->object.sub_map,
						sub_start,
						copy,
						interruptible, 
						entry->object.sub_map->pmap);
				} else if (pmap != NULL) {
					kr = vm_map_copy_overwrite_nested(
						entry->object.sub_map,
						sub_start,
						copy,
						interruptible, pmap);
				} else {
					kr = vm_map_copy_overwrite_nested(
						entry->object.sub_map,
						sub_start,
						copy,
						interruptible,
						dst_map->pmap);
				}
				if(kr != KERN_SUCCESS) {
					if(next_copy != NULL) {
					   copy->cpy_hdr.nentries += 
							   remaining_entries;
				           copy->cpy_hdr.links.prev->vme_next = 
							   next_copy;
				           copy->cpy_hdr.links.prev 
							   = previous_prev;
					   copy->size = total_size;
					}
					return kr;
				}
				if (dst_end <= local_end) {
					return(KERN_SUCCESS);
				}
				/* otherwise copy no longer exists, it was */
				/* destroyed after successful copy_overwrite */
			        copy = (vm_map_copy_t) 
						zalloc(vm_map_copy_zone);
				vm_map_copy_first_entry(copy) =
				   vm_map_copy_last_entry(copy) =
			           vm_map_copy_to_entry(copy);
				copy->type = VM_MAP_COPY_ENTRY_LIST;
				copy->offset = new_offset;

				total_size -= copy_size;
				copy_size = 0;
				/* put back remainder of copy in container */
				if(next_copy != NULL) {
				   copy->cpy_hdr.nentries = remaining_entries;
				   copy->cpy_hdr.links.next = next_copy;
			           copy->cpy_hdr.links.prev = previous_prev;
				   copy->size = total_size;
				   next_copy->vme_prev = 
					         vm_map_copy_to_entry(copy);
				   next_copy = NULL;
				}
				base_addr = local_end;
				vm_map_lock(dst_map);
				if(!vm_map_lookup_entry(dst_map, 
						local_end, &tmp_entry)) {
					vm_map_unlock(dst_map);
					return(KERN_INVALID_ADDRESS);
				}
				entry = tmp_entry;
				continue;
			} 
			if (dst_end <= entry->vme_end) {
				copy_size = dst_end - base_addr;
				break;
			}

			if ((next == vm_map_to_entry(dst_map)) ||
				    (next->vme_start != entry->vme_end)) {
				vm_map_unlock(dst_map);
				return(KERN_INVALID_ADDRESS);
			}

			entry = next;
		}/* for */

		next_copy = NULL;
		nentries = 1;

		/* adjust the copy object */
		if (total_size > copy_size) {
			vm_map_size_t	local_size = 0;
			vm_map_size_t	entry_size;

			new_offset = copy->offset;
			copy_entry = vm_map_copy_first_entry(copy);
			while(copy_entry != vm_map_copy_to_entry(copy)) {
				entry_size = copy_entry->vme_end - 
						copy_entry->vme_start;
				if((local_size < copy_size) &&
						((local_size + entry_size) 
						>= copy_size)) {
					vm_map_copy_clip_end(copy, copy_entry, 
						copy_entry->vme_start +
						(copy_size - local_size));
					entry_size = copy_entry->vme_end - 
				  		   copy_entry->vme_start;
					local_size += entry_size;
					new_offset += entry_size;
				}
				if(local_size >= copy_size) {
					next_copy = copy_entry->vme_next;
					copy_entry->vme_next = 
						vm_map_copy_to_entry(copy);
					previous_prev = 
						copy->cpy_hdr.links.prev;
					copy->cpy_hdr.links.prev = copy_entry;
					copy->size = copy_size;
					remaining_entries = 
						copy->cpy_hdr.nentries;
					remaining_entries -= nentries;
					copy->cpy_hdr.nentries = nentries;
					break;
				} else {
					local_size += entry_size;
					new_offset += entry_size;
					nentries++;
				}
				copy_entry = copy_entry->vme_next;
			}
		}

		if (aligned) {
			pmap_t	local_pmap;

			if(pmap)
				local_pmap = pmap;
			else
				local_pmap = dst_map->pmap;

			if ((kr =  vm_map_copy_overwrite_aligned( 
				dst_map, tmp_entry, copy,
				base_addr, local_pmap)) != KERN_SUCCESS) {
				if(next_copy != NULL) {
					copy->cpy_hdr.nentries += 
							   remaining_entries;
				        copy->cpy_hdr.links.prev->vme_next = 
							   next_copy;
			       		copy->cpy_hdr.links.prev = 
							previous_prev;
					copy->size += copy_size;
				}
				return kr;
			}
			vm_map_unlock(dst_map);
		} else {
		/*
		 * Performance gain:
		 *
		 * if the copy and dst address are misaligned but the same
		 * offset within the page we can copy_not_aligned the
		 * misaligned parts and copy aligned the rest.  If they are
		 * aligned but len is unaligned we simply need to copy
		 * the end bit unaligned.  We'll need to split the misaligned
		 * bits of the region in this case !
		 */
		/* ALWAYS UNLOCKS THE dst_map MAP */
			if ((kr =  vm_map_copy_overwrite_unaligned( dst_map,
				tmp_entry, copy, base_addr)) != KERN_SUCCESS) {
				if(next_copy != NULL) {
					copy->cpy_hdr.nentries +=
							     remaining_entries;
			       		copy->cpy_hdr.links.prev->vme_next = 
							     next_copy;
			       		copy->cpy_hdr.links.prev = 
						previous_prev;
					copy->size += copy_size;
				}
				return kr;
			}
		}
		total_size -= copy_size;
		if(total_size == 0)
			break;
		base_addr += copy_size;
		copy_size = 0;
		copy->offset = new_offset;
		if(next_copy != NULL) {
			copy->cpy_hdr.nentries = remaining_entries;
			copy->cpy_hdr.links.next = next_copy;
			copy->cpy_hdr.links.prev = previous_prev;
			next_copy->vme_prev = vm_map_copy_to_entry(copy);
			copy->size = total_size;
		}
		vm_map_lock(dst_map);
		while(TRUE) {
			if (!vm_map_lookup_entry(dst_map, 
						base_addr, &tmp_entry)) {
				vm_map_unlock(dst_map);
				return(KERN_INVALID_ADDRESS);
			}
                	if (tmp_entry->in_transition) {
                       		entry->needs_wakeup = TRUE;
                       		vm_map_entry_wait(dst_map, THREAD_UNINT);
			} else {
				break;
			}
		}
		vm_map_clip_start(dst_map, tmp_entry, vm_map_trunc_page(base_addr));

		entry = tmp_entry;
	} /* while */

	/*
	 *	Throw away the vm_map_copy object
	 */
	vm_map_copy_discard(copy);

	return(KERN_SUCCESS);
}/* vm_map_copy_overwrite */

kern_return_t
vm_map_copy_overwrite(
	vm_map_t	dst_map,
	vm_map_offset_t	dst_addr,
	vm_map_copy_t	copy,
	boolean_t	interruptible)
{
	return vm_map_copy_overwrite_nested(
			dst_map, dst_addr, copy, interruptible, (pmap_t) NULL);
}


/*
 *	Routine: vm_map_copy_overwrite_unaligned	[internal use only]
 *
 *	Decription:
 *	Physically copy unaligned data
 *
 *	Implementation:
 *	Unaligned parts of pages have to be physically copied.  We use
 *	a modified form of vm_fault_copy (which understands none-aligned
 *	page offsets and sizes) to do the copy.  We attempt to copy as
 *	much memory in one go as possibly, however vm_fault_copy copies
 *	within 1 memory object so we have to find the smaller of "amount left"
 *	"source object data size" and "target object data size".  With
 *	unaligned data we don't need to split regions, therefore the source
 *	(copy) object should be one map entry, the target range may be split
 *	over multiple map entries however.  In any event we are pessimistic
 *	about these assumptions.
 *
 *	Assumptions:
 *	dst_map is locked on entry and is return locked on success,
 *	unlocked on error.
 */

static kern_return_t
vm_map_copy_overwrite_unaligned(
	vm_map_t	dst_map,
	vm_map_entry_t	entry,
	vm_map_copy_t	copy,
	vm_map_offset_t	start)
{
	vm_map_entry_t		copy_entry = vm_map_copy_first_entry(copy);
	vm_map_version_t	version;
	vm_object_t		dst_object;
	vm_object_offset_t	dst_offset;
	vm_object_offset_t	src_offset;
	vm_object_offset_t	entry_offset;
	vm_map_offset_t		entry_end;
	vm_map_size_t		src_size,
				dst_size,
				copy_size,
				amount_left;
	kern_return_t		kr = KERN_SUCCESS;

	vm_map_lock_write_to_read(dst_map);

	src_offset = copy->offset - vm_object_trunc_page(copy->offset);
	amount_left = copy->size;
/*
 *	unaligned so we never clipped this entry, we need the offset into
 *	the vm_object not just the data.
 */	
	while (amount_left > 0) {

		if (entry == vm_map_to_entry(dst_map)) {
			vm_map_unlock_read(dst_map);
			return KERN_INVALID_ADDRESS;
		}

		/* "start" must be within the current map entry */
		assert ((start>=entry->vme_start) && (start<entry->vme_end));

		dst_offset = start - entry->vme_start;

		dst_size = entry->vme_end - start;

		src_size = copy_entry->vme_end -
			(copy_entry->vme_start + src_offset);

		if (dst_size < src_size) {
/*
 *			we can only copy dst_size bytes before
 *			we have to get the next destination entry
 */
			copy_size = dst_size;
		} else {
/*
 *			we can only copy src_size bytes before
 *			we have to get the next source copy entry
 */
			copy_size = src_size;
		}

		if (copy_size > amount_left) {
			copy_size = amount_left;
		}
/*
 *		Entry needs copy, create a shadow shadow object for
 *		Copy on write region.
 */
		if (entry->needs_copy &&
			 ((entry->protection & VM_PROT_WRITE) != 0))
		{
			if (vm_map_lock_read_to_write(dst_map)) {
				vm_map_lock_read(dst_map);
				goto RetryLookup;
			}
			vm_object_shadow(&entry->object.vm_object,
					&entry->offset,
					(vm_map_size_t)(entry->vme_end
						- entry->vme_start));
			entry->needs_copy = FALSE;
			vm_map_lock_write_to_read(dst_map);
		}
		dst_object = entry->object.vm_object;
/*
 *		unlike with the virtual (aligned) copy we're going
 *		to fault on it therefore we need a target object.
 */
                if (dst_object == VM_OBJECT_NULL) {
			if (vm_map_lock_read_to_write(dst_map)) {
				vm_map_lock_read(dst_map);
				goto RetryLookup;
			}
			dst_object = vm_object_allocate((vm_map_size_t)
					entry->vme_end - entry->vme_start);
			entry->object.vm_object = dst_object;
			entry->offset = 0;
			vm_map_lock_write_to_read(dst_map);
		}
/*
 *		Take an object reference and unlock map. The "entry" may
 *		disappear or change when the map is unlocked.
 */
		vm_object_reference(dst_object);
		version.main_timestamp = dst_map->timestamp;
		entry_offset = entry->offset;
		entry_end = entry->vme_end;
		vm_map_unlock_read(dst_map);
/*
 *		Copy as much as possible in one pass
 */
		kr = vm_fault_copy(
			copy_entry->object.vm_object,
			copy_entry->offset + src_offset,
			&copy_size,
			dst_object,
			entry_offset + dst_offset,
			dst_map,
			&version,
			THREAD_UNINT );

		start += copy_size;
		src_offset += copy_size;
		amount_left -= copy_size;
/*
 *		Release the object reference
 */
		vm_object_deallocate(dst_object);
/*
 *		If a hard error occurred, return it now
 */
		if (kr != KERN_SUCCESS)
			return kr;

		if ((copy_entry->vme_start + src_offset) == copy_entry->vme_end
			|| amount_left == 0)
		{
/*
 *			all done with this copy entry, dispose.
 */
			vm_map_copy_entry_unlink(copy, copy_entry);
			vm_object_deallocate(copy_entry->object.vm_object);
			vm_map_copy_entry_dispose(copy, copy_entry);

			if ((copy_entry = vm_map_copy_first_entry(copy))
				== vm_map_copy_to_entry(copy) && amount_left) {
/*
 *				not finished copying but run out of source
 */
				return KERN_INVALID_ADDRESS;
			}
			src_offset = 0;
		}

		if (amount_left == 0)
			return KERN_SUCCESS;

		vm_map_lock_read(dst_map);
		if (version.main_timestamp == dst_map->timestamp) {
			if (start == entry_end) {
/*
 *				destination region is split.  Use the version
 *				information to avoid a lookup in the normal
 *				case.
 */
				entry = entry->vme_next;
/*
 *				should be contiguous. Fail if we encounter
 *				a hole in the destination.
 */
				if (start != entry->vme_start) {
					vm_map_unlock_read(dst_map);
					return KERN_INVALID_ADDRESS ;
				}
			}
		} else {
/*
 *			Map version check failed.
 *			we must lookup the entry because somebody
 *			might have changed the map behind our backs.
 */
RetryLookup:
			if (!vm_map_lookup_entry(dst_map, start, &entry))
			{
				vm_map_unlock_read(dst_map);
				return KERN_INVALID_ADDRESS ;
			}
		}
	}/* while */

	return KERN_SUCCESS;
}/* vm_map_copy_overwrite_unaligned */

/*
 *	Routine: vm_map_copy_overwrite_aligned	[internal use only]
 *
 *	Description:
 *	Does all the vm_trickery possible for whole pages.
 *
 *	Implementation:
 *
 *	If there are no permanent objects in the destination,
 *	and the source and destination map entry zones match,
 *	and the destination map entry is not shared,
 *	then the map entries can be deleted and replaced
 *	with those from the copy.  The following code is the
 *	basic idea of what to do, but there are lots of annoying
 *	little details about getting protection and inheritance
 *	right.  Should add protection, inheritance, and sharing checks
 *	to the above pass and make sure that no wiring is involved.
 */

static kern_return_t
vm_map_copy_overwrite_aligned(
	vm_map_t	dst_map,
	vm_map_entry_t	tmp_entry,
	vm_map_copy_t	copy,
	vm_map_offset_t	start,
#if !BAD_OPTIMIZATION
	__unused
#endif /* !BAD_OPTIMIZATION */
	pmap_t		pmap)
{
	vm_object_t	object;
	vm_map_entry_t	copy_entry;
	vm_map_size_t	copy_size;
	vm_map_size_t	size;
	vm_map_entry_t	entry;
		
	while ((copy_entry = vm_map_copy_first_entry(copy))
		!= vm_map_copy_to_entry(copy))
	{
		copy_size = (copy_entry->vme_end - copy_entry->vme_start);
		
		entry = tmp_entry;
		if (entry == vm_map_to_entry(dst_map)) {
			vm_map_unlock(dst_map);
			return KERN_INVALID_ADDRESS;
		}
		size = (entry->vme_end - entry->vme_start);
		/*
		 *	Make sure that no holes popped up in the
		 *	address map, and that the protection is
		 *	still valid, in case the map was unlocked
		 *	earlier.
		 */

		if ((entry->vme_start != start) || ((entry->is_sub_map)
				&& !entry->needs_copy)) {
			vm_map_unlock(dst_map);
			return(KERN_INVALID_ADDRESS);
		}
		assert(entry != vm_map_to_entry(dst_map));

		/*
		 *	Check protection again
		 */

		if ( ! (entry->protection & VM_PROT_WRITE)) {
			vm_map_unlock(dst_map);
			return(KERN_PROTECTION_FAILURE);
		}

		/*
		 *	Adjust to source size first
		 */

		if (copy_size < size) {
			vm_map_clip_end(dst_map, entry, entry->vme_start + copy_size);
			size = copy_size;
		}

		/*
		 *	Adjust to destination size
		 */

		if (size < copy_size) {
			vm_map_copy_clip_end(copy, copy_entry,
				copy_entry->vme_start + size);
			copy_size = size;
		}

		assert((entry->vme_end - entry->vme_start) == size);
		assert((tmp_entry->vme_end - tmp_entry->vme_start) == size);
		assert((copy_entry->vme_end - copy_entry->vme_start) == size);

		/*
		 *	If the destination contains temporary unshared memory,
		 *	we can perform the copy by throwing it away and
		 *	installing the source data.
		 */

		object = entry->object.vm_object;
		if ((!entry->is_shared && 
		    ((object == VM_OBJECT_NULL) || 
		    (object->internal && !object->true_share))) ||
		    entry->needs_copy) {
			vm_object_t	old_object = entry->object.vm_object;
			vm_object_offset_t	old_offset = entry->offset;
			vm_object_offset_t	offset;

			/*
			 * Ensure that the source and destination aren't
			 * identical
			 */
			if (old_object == copy_entry->object.vm_object &&
			    old_offset == copy_entry->offset) {
				vm_map_copy_entry_unlink(copy, copy_entry);
				vm_map_copy_entry_dispose(copy, copy_entry);

				if (old_object != VM_OBJECT_NULL)
					vm_object_deallocate(old_object);

				start = tmp_entry->vme_end;
				tmp_entry = tmp_entry->vme_next;
				continue;
			}

			if (old_object != VM_OBJECT_NULL) {
				if(entry->is_sub_map) {
					if(entry->use_pmap) {
#ifndef i386
						pmap_unnest(dst_map->pmap, 
							    entry->vme_start);
#endif
						if(dst_map->mapped) {
							/* clean up parent */
							/* map/maps */
						   vm_map_submap_pmap_clean(
							dst_map, entry->vme_start,
							entry->vme_end,
							entry->object.sub_map,
							entry->offset);
						}
					} else {
						vm_map_submap_pmap_clean(
							dst_map, entry->vme_start, 
							entry->vme_end,
							entry->object.sub_map,
							entry->offset);
					}
				   	vm_map_deallocate(
						entry->object.sub_map);
			   	} else {
					if(dst_map->mapped) {
						vm_object_pmap_protect(
							entry->object.vm_object,
							entry->offset,
							entry->vme_end 
								- entry->vme_start,
							PMAP_NULL,
							entry->vme_start,
							VM_PROT_NONE);
					} else {
					   pmap_remove(dst_map->pmap, 
					     (addr64_t)(entry->vme_start), 
					     (addr64_t)(entry->vme_end));
					}
					vm_object_deallocate(old_object);
			   	}
			}

			entry->is_sub_map = FALSE;
			entry->object = copy_entry->object;
			object = entry->object.vm_object;
			entry->needs_copy = copy_entry->needs_copy;
			entry->wired_count = 0;
			entry->user_wired_count = 0;
			offset = entry->offset = copy_entry->offset;

			vm_map_copy_entry_unlink(copy, copy_entry);
			vm_map_copy_entry_dispose(copy, copy_entry);
#if BAD_OPTIMIZATION
			/*
			 * if we turn this optimization back on
			 * we need to revisit our use of pmap mappings
			 * large copies will cause us to run out and panic
			 * this optimization only saved on average 2 us per page if ALL
			 * the pages in the source were currently mapped
			 * and ALL the pages in the dest were touched, if there were fewer
			 * than 2/3 of the pages touched, this optimization actually cost more cycles
			 */

			/*
			 * Try to aggressively enter physical mappings
			 * (but avoid uninstantiated objects)
			 */
			if (object != VM_OBJECT_NULL) {
			    vm_map_offset_t	va = entry->vme_start;

			    while (va < entry->vme_end) {
				register vm_page_t	m;
				vm_prot_t		prot;

				/*
				 * Look for the page in the top object
				 */
				prot = entry->protection;
				vm_object_lock(object);
				vm_object_paging_begin(object);

				/*
				 * ENCRYPTED SWAP:
				 * If the page is encrypted, skip it:
				 * we can't let the user see the encrypted
				 * contents.  The page will get decrypted
				 * on demand when the user generates a
				 * soft-fault when trying to access it.
				 */
				if ((m = vm_page_lookup(object,offset)) !=
				    VM_PAGE_NULL && !m->busy &&
				    !m->fictitious && !m->encrypted &&
				    (!m->unusual || (!m->error &&
					!m->restart && !m->absent &&
					 (prot & m->page_lock) == 0))) {
					
					m->busy = TRUE;
					vm_object_unlock(object);
					
					/* 
					 * Honor COW obligations
					 */
					if (entry->needs_copy)
						prot &= ~VM_PROT_WRITE;
					/* It is our policy to require */
					/* explicit sync from anyone   */
					/* writing code and then       */
					/* a pc to execute it.         */
					/* No isync here */

					PMAP_ENTER(pmap, va, m, prot, 
						((unsigned int)
							(m->object->wimg_bits)) 
							& VM_WIMG_MASK,
						FALSE);
		
					vm_object_lock(object);
					vm_page_lock_queues();
					if (!m->active && !m->inactive)
						vm_page_activate(m);
					vm_page_unlock_queues();
					 PAGE_WAKEUP_DONE(m);
				}
				vm_object_paging_end(object);
				vm_object_unlock(object);

				offset += PAGE_SIZE_64;
				va += PAGE_SIZE;
			    } /* end while (va < entry->vme_end) */
			} /* end if (object) */
#endif
			/*
			 *	Set up for the next iteration.  The map
			 *	has not been unlocked, so the next
			 *	address should be at the end of this
			 *	entry, and the next map entry should be
			 *	the one following it.
			 */

			start = tmp_entry->vme_end;
			tmp_entry = tmp_entry->vme_next;
		} else {
			vm_map_version_t	version;
			vm_object_t		dst_object = entry->object.vm_object;
			vm_object_offset_t	dst_offset = entry->offset;
			kern_return_t		r;

			/*
			 *	Take an object reference, and record
			 *	the map version information so that the
			 *	map can be safely unlocked.
			 */

			vm_object_reference(dst_object);

			/* account for unlock bumping up timestamp */
			version.main_timestamp = dst_map->timestamp + 1;

			vm_map_unlock(dst_map);

			/*
			 *	Copy as much as possible in one pass
			 */

			copy_size = size;
			r = vm_fault_copy(
					copy_entry->object.vm_object,
					copy_entry->offset,
					&copy_size,
					dst_object,
					dst_offset,
					dst_map,
					&version,
					THREAD_UNINT );

			/*
			 *	Release the object reference
			 */

			vm_object_deallocate(dst_object);

			/*
			 *	If a hard error occurred, return it now
			 */

			if (r != KERN_SUCCESS)
				return(r);

			if (copy_size != 0) {
				/*
				 *	Dispose of the copied region
				 */

				vm_map_copy_clip_end(copy, copy_entry,
					copy_entry->vme_start + copy_size);
				vm_map_copy_entry_unlink(copy, copy_entry);
				vm_object_deallocate(copy_entry->object.vm_object);
				vm_map_copy_entry_dispose(copy, copy_entry);
			}

			/*
			 *	Pick up in the destination map where we left off.
			 *
			 *	Use the version information to avoid a lookup
			 *	in the normal case.
			 */

			start += copy_size;
			vm_map_lock(dst_map);
			if (version.main_timestamp == dst_map->timestamp) {
				/* We can safely use saved tmp_entry value */

				vm_map_clip_end(dst_map, tmp_entry, start);
				tmp_entry = tmp_entry->vme_next;
			} else {
				/* Must do lookup of tmp_entry */

				if (!vm_map_lookup_entry(dst_map, start, &tmp_entry)) {
					vm_map_unlock(dst_map);
					return(KERN_INVALID_ADDRESS);
				}
				vm_map_clip_start(dst_map, tmp_entry, start);
			}
		}
	}/* while */

	return(KERN_SUCCESS);
}/* vm_map_copy_overwrite_aligned */

/*
 *	Routine: vm_map_copyin_kernel_buffer [internal use only]
 *
 *	Description:
 *		Copy in data to a kernel buffer from space in the
 *		source map. The original space may be optionally
 *		deallocated.
 *
 *		If successful, returns a new copy object.
 */
static kern_return_t
vm_map_copyin_kernel_buffer(
	vm_map_t	src_map,
	vm_map_offset_t	src_addr,
	vm_map_size_t	len,
	boolean_t	src_destroy,
	vm_map_copy_t	*copy_result)
{
	kern_return_t kr;
	vm_map_copy_t copy;
	vm_map_size_t kalloc_size = sizeof(struct vm_map_copy) + len;

	copy = (vm_map_copy_t) kalloc(kalloc_size);
	if (copy == VM_MAP_COPY_NULL) {
		return KERN_RESOURCE_SHORTAGE;
	}
	copy->type = VM_MAP_COPY_KERNEL_BUFFER;
	copy->size = len;
	copy->offset = 0;
	copy->cpy_kdata = (void *) (copy + 1);
	copy->cpy_kalloc_size = kalloc_size;

	kr = copyinmap(src_map, src_addr, copy->cpy_kdata, len);
	if (kr != KERN_SUCCESS) {
		kfree(copy, kalloc_size);
		return kr;
	}
	if (src_destroy) {
		(void) vm_map_remove(src_map, vm_map_trunc_page(src_addr), 
				vm_map_round_page(src_addr + len),
				VM_MAP_REMOVE_INTERRUPTIBLE |
				VM_MAP_REMOVE_WAIT_FOR_KWIRE |
				(src_map == kernel_map) ?
					VM_MAP_REMOVE_KUNWIRE : 0);
	}
	*copy_result = copy;
	return KERN_SUCCESS;
}

/*
 *	Routine: vm_map_copyout_kernel_buffer	[internal use only]
 *
 *	Description:
 *		Copy out data from a kernel buffer into space in the
 *		destination map. The space may be otpionally dynamically
 *		allocated.
 *
 *		If successful, consumes the copy object.
 *		Otherwise, the caller is responsible for it.
 */
static int vm_map_copyout_kernel_buffer_failures = 0;
static kern_return_t
vm_map_copyout_kernel_buffer(
	vm_map_t		map,
	vm_map_address_t	*addr,	/* IN/OUT */
	vm_map_copy_t		copy,
	boolean_t		overwrite)
{
	kern_return_t kr = KERN_SUCCESS;
	thread_t thread = current_thread();

	if (!overwrite) {

		/*
		 * Allocate space in the target map for the data
		 */
		*addr = 0;
		kr = vm_map_enter(map, 
				  addr, 
				  vm_map_round_page(copy->size),
				  (vm_map_offset_t) 0, 
				  VM_FLAGS_ANYWHERE,
				  VM_OBJECT_NULL, 
				  (vm_object_offset_t) 0, 
				  FALSE,
				  VM_PROT_DEFAULT, 
				  VM_PROT_ALL,
				  VM_INHERIT_DEFAULT);
		if (kr != KERN_SUCCESS)
			return kr;
	}

	/*
	 * Copyout the data from the kernel buffer to the target map.
	 */	
	if (thread->map == map) {
	
		/*
		 * If the target map is the current map, just do
		 * the copy.
		 */
		if (copyout(copy->cpy_kdata, *addr, copy->size)) {
			kr = KERN_INVALID_ADDRESS;
		}
	}
	else {
		vm_map_t oldmap;

		/*
		 * If the target map is another map, assume the
		 * target's address space identity for the duration
		 * of the copy.
		 */
		vm_map_reference(map);
		oldmap = vm_map_switch(map);

		if (copyout(copy->cpy_kdata, *addr, copy->size)) {
			vm_map_copyout_kernel_buffer_failures++;
			kr = KERN_INVALID_ADDRESS;
		}
	
		(void) vm_map_switch(oldmap);
		vm_map_deallocate(map);
	}

	if (kr != KERN_SUCCESS) {
		/* the copy failed, clean up */
		if (!overwrite) {
			/*
			 * Deallocate the space we allocated in the target map.
			 */
			(void) vm_map_remove(map,
					     vm_map_trunc_page(*addr),
					     vm_map_round_page(*addr +
							       vm_map_round_page(copy->size)),
					     VM_MAP_NO_FLAGS);
			*addr = 0;
		}
	} else {
		/* copy was successful, dicard the copy structure */
		kfree(copy, copy->cpy_kalloc_size);
	}

	return kr;
}
		
/*
 *	Macro:		vm_map_copy_insert
 *	
 *	Description:
 *		Link a copy chain ("copy") into a map at the
 *		specified location (after "where").
 *	Side effects:
 *		The copy chain is destroyed.
 *	Warning:
 *		The arguments are evaluated multiple times.
 */
#define	vm_map_copy_insert(map, where, copy)				\
MACRO_BEGIN								\
	vm_map_t VMCI_map;						\
	vm_map_entry_t VMCI_where;					\
	vm_map_copy_t VMCI_copy;					\
	VMCI_map = (map);						\
	VMCI_where = (where);						\
	VMCI_copy = (copy);						\
	((VMCI_where->vme_next)->vme_prev = vm_map_copy_last_entry(VMCI_copy))\
		->vme_next = (VMCI_where->vme_next);			\
	((VMCI_where)->vme_next = vm_map_copy_first_entry(VMCI_copy))	\
		->vme_prev = VMCI_where;				\
	VMCI_map->hdr.nentries += VMCI_copy->cpy_hdr.nentries;		\
	UPDATE_FIRST_FREE(VMCI_map, VMCI_map->first_free);		\
	zfree(vm_map_copy_zone, VMCI_copy);				\
MACRO_END

/*
 *	Routine:	vm_map_copyout
 *
 *	Description:
 *		Copy out a copy chain ("copy") into newly-allocated
 *		space in the destination map.
 *
 *		If successful, consumes the copy object.
 *		Otherwise, the caller is responsible for it.
 */
kern_return_t
vm_map_copyout(
	vm_map_t		dst_map,
	vm_map_address_t	*dst_addr,	/* OUT */
	vm_map_copy_t		copy)
{
	vm_map_size_t		size;
	vm_map_size_t		adjustment;
	vm_map_offset_t		start;
	vm_object_offset_t	vm_copy_start;
	vm_map_entry_t		last;
	register
	vm_map_entry_t		entry;

	/*
	 *	Check for null copy object.
	 */

	if (copy == VM_MAP_COPY_NULL) {
		*dst_addr = 0;
		return(KERN_SUCCESS);
	}

	/*
	 *	Check for special copy object, created
	 *	by vm_map_copyin_object.
	 */

	if (copy->type == VM_MAP_COPY_OBJECT) {
		vm_object_t 		object = copy->cpy_object;
		kern_return_t 		kr;
		vm_object_offset_t	offset;

		offset = vm_object_trunc_page(copy->offset);
		size = vm_map_round_page(copy->size + 
				(vm_map_size_t)(copy->offset - offset));
		*dst_addr = 0;
		kr = vm_map_enter(dst_map, dst_addr, size,
				  (vm_map_offset_t) 0, VM_FLAGS_ANYWHERE,
				  object, offset, FALSE,
				  VM_PROT_DEFAULT, VM_PROT_ALL,
				  VM_INHERIT_DEFAULT);
		if (kr != KERN_SUCCESS)
			return(kr);
		/* Account for non-pagealigned copy object */
		*dst_addr += (vm_map_offset_t)(copy->offset - offset);
		zfree(vm_map_copy_zone, copy);
		return(KERN_SUCCESS);
	}

	/*
	 *	Check for special kernel buffer allocated
	 *	by new_ipc_kmsg_copyin.
	 */

	if (copy->type == VM_MAP_COPY_KERNEL_BUFFER) {
		return(vm_map_copyout_kernel_buffer(dst_map, dst_addr, 
						    copy, FALSE));
	}

	/*
	 *	Find space for the data
	 */

	vm_copy_start = vm_object_trunc_page(copy->offset);
	size =	vm_map_round_page((vm_map_size_t)copy->offset + copy->size) 
							- vm_copy_start;

 StartAgain: ;

	vm_map_lock(dst_map);
	assert(first_free_is_valid(dst_map));
	start = ((last = dst_map->first_free) == vm_map_to_entry(dst_map)) ?
		vm_map_min(dst_map) : last->vme_end;

	while (TRUE) {
		vm_map_entry_t	next = last->vme_next;
		vm_map_offset_t	end = start + size;

		if ((end > dst_map->max_offset) || (end < start)) {
			if (dst_map->wait_for_space) {
				if (size <= (dst_map->max_offset - dst_map->min_offset)) {
					assert_wait((event_t) dst_map,
						    THREAD_INTERRUPTIBLE);
					vm_map_unlock(dst_map);
					thread_block(THREAD_CONTINUE_NULL);
					goto StartAgain;
				}
			}
			vm_map_unlock(dst_map);
			return(KERN_NO_SPACE);
		}

		if ((next == vm_map_to_entry(dst_map)) ||
		    (next->vme_start >= end))
			break;

		last = next;
		start = last->vme_end;
	}

	/*
	 *	Since we're going to just drop the map
	 *	entries from the copy into the destination
	 *	map, they must come from the same pool.
	 */

	if (copy->cpy_hdr.entries_pageable != dst_map->hdr.entries_pageable) {
	    /*
	     * Mismatches occur when dealing with the default
	     * pager.
	     */
	    zone_t		old_zone;
	    vm_map_entry_t	next, new;

	    /*
	     * Find the zone that the copies were allocated from
	     */
	    old_zone = (copy->cpy_hdr.entries_pageable)
			? vm_map_entry_zone
			: vm_map_kentry_zone;
	    entry = vm_map_copy_first_entry(copy);

	    /*
	     * Reinitialize the copy so that vm_map_copy_entry_link
	     * will work.
	     */
	    copy->cpy_hdr.nentries = 0;
	    copy->cpy_hdr.entries_pageable = dst_map->hdr.entries_pageable;
	    vm_map_copy_first_entry(copy) =
	     vm_map_copy_last_entry(copy) =
		vm_map_copy_to_entry(copy);

	    /*
	     * Copy each entry.
	     */
	    while (entry != vm_map_copy_to_entry(copy)) {
		new = vm_map_copy_entry_create(copy);
		vm_map_entry_copy_full(new, entry);
		new->use_pmap = FALSE;	/* clr address space specifics */
		vm_map_copy_entry_link(copy,
				vm_map_copy_last_entry(copy),
				new);
		next = entry->vme_next;
		zfree(old_zone, entry);
		entry = next;
	    }
	}

	/*
	 *	Adjust the addresses in the copy chain, and
	 *	reset the region attributes.
	 */

	adjustment = start - vm_copy_start;
	for (entry = vm_map_copy_first_entry(copy);
	     entry != vm_map_copy_to_entry(copy);
	     entry = entry->vme_next) {
		entry->vme_start += adjustment;
		entry->vme_end += adjustment;

		entry->inheritance = VM_INHERIT_DEFAULT;
		entry->protection = VM_PROT_DEFAULT;
		entry->max_protection = VM_PROT_ALL;
		entry->behavior = VM_BEHAVIOR_DEFAULT;

		/*
		 * If the entry is now wired,
		 * map the pages into the destination map.
		 */
		if (entry->wired_count != 0) {
		    register vm_map_offset_t va;
		    vm_object_offset_t	 offset;
		    register vm_object_t object;

		    object = entry->object.vm_object;
		    offset = entry->offset;
		    va = entry->vme_start;

		    pmap_pageable(dst_map->pmap,
				  entry->vme_start,
				  entry->vme_end,
				  TRUE);

		    while (va < entry->vme_end) {
			register vm_page_t	m;

			/*
			 * Look up the page in the object.
			 * Assert that the page will be found in the
			 * top object:
			 * either
			 *	the object was newly created by
			 *	vm_object_copy_slowly, and has
			 *	copies of all of the pages from
			 *	the source object
			 * or
			 *	the object was moved from the old
			 *	map entry; because the old map
			 *	entry was wired, all of the pages
			 *	were in the top-level object.
			 *	(XXX not true if we wire pages for
			 *	 reading)
			 */
			vm_object_lock(object);
			vm_object_paging_begin(object);

			m = vm_page_lookup(object, offset);
			if (m == VM_PAGE_NULL || m->wire_count == 0 ||
			    m->absent)
			    panic("vm_map_copyout: wiring 0x%x", m);

			/*
			 * ENCRYPTED SWAP:
			 * The page is assumed to be wired here, so it
			 * shouldn't be encrypted.  Otherwise, we
			 * couldn't enter it in the page table, since
			 * we don't want the user to see the encrypted
			 * data.
			 */
			ASSERT_PAGE_DECRYPTED(m);

			m->busy = TRUE;
			vm_object_unlock(object);

			PMAP_ENTER(dst_map->pmap, va, m, entry->protection, 
						((unsigned int)
							(m->object->wimg_bits)) 
							& VM_WIMG_MASK,
						TRUE);

			vm_object_lock(object);
			PAGE_WAKEUP_DONE(m);
			/* the page is wired, so we don't have to activate */
			vm_object_paging_end(object);
			vm_object_unlock(object);

			offset += PAGE_SIZE_64;
			va += PAGE_SIZE;
		    }
		}
		else if (size <= vm_map_aggressive_enter_max) {

			register vm_map_offset_t	va;
			vm_object_offset_t	offset;
			register vm_object_t	object;
			vm_prot_t		prot;

			object = entry->object.vm_object;
			if (object != VM_OBJECT_NULL) {

				offset = entry->offset;
				va = entry->vme_start;
				while (va < entry->vme_end) {
					register vm_page_t	m;
				    
					/*
					 * Look up the page in the object.
					 * Assert that the page will be found
					 * in the top object if at all...
					 */
					vm_object_lock(object);
					vm_object_paging_begin(object);

					/*
					 * ENCRYPTED SWAP:
					 * If the page is encrypted, skip it:
					 * we can't let the user see the
					 * encrypted contents.  The page will
					 * get decrypted on demand when the
					 * user generates a soft-fault when
					 * trying to access it.
					 */
					if (((m = vm_page_lookup(object,
								 offset))
					     != VM_PAGE_NULL) &&
					    !m->busy && !m->fictitious &&
					    !m->encrypted &&
					    !m->absent && !m->error) {
						m->busy = TRUE;
						vm_object_unlock(object);

						/* honor cow obligations */
						prot = entry->protection;
						if (entry->needs_copy)
							prot &= ~VM_PROT_WRITE;

						PMAP_ENTER(dst_map->pmap, va, 
							m, prot, 
							((unsigned int)
						        (m->object->wimg_bits)) 
							   & VM_WIMG_MASK,
							FALSE);

						vm_object_lock(object);
						vm_page_lock_queues();
						if (!m->active && !m->inactive)
							vm_page_activate(m);
						vm_page_unlock_queues();
						PAGE_WAKEUP_DONE(m);
					}
					vm_object_paging_end(object);
					vm_object_unlock(object);

					offset += PAGE_SIZE_64;
					va += PAGE_SIZE;
				}
			}
		}
	}

	/*
	 *	Correct the page alignment for the result
	 */

	*dst_addr = start + (copy->offset - vm_copy_start);

	/*
	 *	Update the hints and the map size
	 */

	SAVE_HINT(dst_map, vm_map_copy_last_entry(copy));

	dst_map->size += size;

	/*
	 *	Link in the copy
	 */

	vm_map_copy_insert(dst_map, last, copy);

	vm_map_unlock(dst_map);

	/*
	 * XXX	If wiring_required, call vm_map_pageable
	 */

	return(KERN_SUCCESS);
}

/*
 *	Routine:	vm_map_copyin
 *
 *	Description:
 *		Copy the specified region (src_addr, len) from the
 *		source address space (src_map), possibly removing
 *		the region from the source address space (src_destroy).
 *
 *	Returns:
 *		A vm_map_copy_t object (copy_result), suitable for
 *		insertion into another address space (using vm_map_copyout),
 *		copying over another address space region (using
 *		vm_map_copy_overwrite).  If the copy is unused, it
 *		should be destroyed (using vm_map_copy_discard).
 *
 *	In/out conditions:
 *		The source map should not be locked on entry.
 */

typedef struct submap_map {
	vm_map_t	parent_map;
	vm_map_offset_t	base_start;
	vm_map_offset_t	base_end;
	struct submap_map *next;
} submap_map_t;

kern_return_t
vm_map_copyin_common(
	vm_map_t	src_map,
	vm_map_address_t src_addr,
	vm_map_size_t	len,
	boolean_t	src_destroy,
	__unused boolean_t	src_volatile,
	vm_map_copy_t	*copy_result,	/* OUT */
	boolean_t	use_maxprot)
{
	vm_map_entry_t	tmp_entry;	/* Result of last map lookup --
					 * in multi-level lookup, this
					 * entry contains the actual
					 * vm_object/offset.
					 */
	register
	vm_map_entry_t	new_entry = VM_MAP_ENTRY_NULL;	/* Map entry for copy */

	vm_map_offset_t	src_start;	/* Start of current entry --
					 * where copy is taking place now
					 */
	vm_map_offset_t	src_end;	/* End of entire region to be
					 * copied */
	vm_map_t	base_map = src_map;
	boolean_t	map_share=FALSE;
	submap_map_t	*parent_maps = NULL;

	register
	vm_map_copy_t	copy;		/* Resulting copy */
	vm_map_address_t	copy_addr;

	/*
	 *	Check for copies of zero bytes.
	 */

	if (len == 0) {
		*copy_result = VM_MAP_COPY_NULL;
		return(KERN_SUCCESS);
	}

	/*
	 *	Check that the end address doesn't overflow
	 */
	src_end = src_addr + len;
	if (src_end < src_addr)
		return KERN_INVALID_ADDRESS;

	/*
	 * If the copy is sufficiently small, use a kernel buffer instead
	 * of making a virtual copy.  The theory being that the cost of
	 * setting up VM (and taking C-O-W faults) dominates the copy costs
	 * for small regions.
	 */
	if ((len < msg_ool_size_small) && !use_maxprot)
	  return vm_map_copyin_kernel_buffer(src_map, src_addr, len,
					     src_destroy, copy_result);

	/*
	 *	Compute (page aligned) start and end of region
	 */
	src_start = vm_map_trunc_page(src_addr);
	src_end = vm_map_round_page(src_end);

	XPR(XPR_VM_MAP, "vm_map_copyin_common map 0x%x addr 0x%x len 0x%x dest %d\n", (natural_t)src_map, src_addr, len, src_destroy, 0);

	/*
	 *	Allocate a header element for the list.
	 *
	 *	Use the start and end in the header to 
	 *	remember the endpoints prior to rounding.
	 */

	copy = (vm_map_copy_t) zalloc(vm_map_copy_zone);
	vm_map_copy_first_entry(copy) =
	 vm_map_copy_last_entry(copy) = vm_map_copy_to_entry(copy);
	copy->type = VM_MAP_COPY_ENTRY_LIST;
	copy->cpy_hdr.nentries = 0;
	copy->cpy_hdr.entries_pageable = TRUE;

	copy->offset = src_addr;
	copy->size = len;
	
	new_entry = vm_map_copy_entry_create(copy);

#define	RETURN(x)						\
	MACRO_BEGIN						\
	vm_map_unlock(src_map);					\
	if(src_map != base_map)					\
		vm_map_deallocate(src_map);			\
	if (new_entry != VM_MAP_ENTRY_NULL)			\
		vm_map_copy_entry_dispose(copy,new_entry);	\
	vm_map_copy_discard(copy);				\
	{							\
		submap_map_t	*_ptr;				\
								\
		for(_ptr = parent_maps; _ptr != NULL; _ptr = parent_maps) { \
			parent_maps=parent_maps->next;		\
			if (_ptr->parent_map != base_map)	\
				vm_map_deallocate(_ptr->parent_map);	\
			kfree(_ptr, sizeof(submap_map_t));	\
		}						\
	}							\
	MACRO_RETURN(x);					\
	MACRO_END

	/*
	 *	Find the beginning of the region.
	 */

 	vm_map_lock(src_map);

	if (!vm_map_lookup_entry(src_map, src_start, &tmp_entry))
		RETURN(KERN_INVALID_ADDRESS);
	if(!tmp_entry->is_sub_map) {
		vm_map_clip_start(src_map, tmp_entry, src_start);
	}
	/* set for later submap fix-up */
	copy_addr = src_start;

	/*
	 *	Go through entries until we get to the end.
	 */

	while (TRUE) {
		register
		vm_map_entry_t	src_entry = tmp_entry;	/* Top-level entry */
		vm_map_size_t	src_size;		/* Size of source
							 * map entry (in both
							 * maps)
							 */

		register
		vm_object_t		src_object;	/* Object to copy */
		vm_object_offset_t	src_offset;

		boolean_t	src_needs_copy;		/* Should source map
							 * be made read-only
							 * for copy-on-write?
							 */

		boolean_t	new_entry_needs_copy;	/* Will new entry be COW? */

		boolean_t	was_wired;		/* Was source wired? */
		vm_map_version_t version;		/* Version before locks
							 * dropped to make copy
							 */
		kern_return_t	result;			/* Return value from
							 * copy_strategically.
							 */
		while(tmp_entry->is_sub_map) {
			vm_map_size_t submap_len;
			submap_map_t *ptr;

			ptr = (submap_map_t *)kalloc(sizeof(submap_map_t));
			ptr->next = parent_maps;
			parent_maps = ptr;
			ptr->parent_map = src_map;
			ptr->base_start = src_start;
			ptr->base_end = src_end;
			submap_len = tmp_entry->vme_end - src_start;
			if(submap_len > (src_end-src_start))
				submap_len = src_end-src_start;
			ptr->base_start += submap_len;
	
			src_start -= tmp_entry->vme_start;
			src_start += tmp_entry->offset;
			src_end = src_start + submap_len;
			src_map = tmp_entry->object.sub_map;
			vm_map_lock(src_map);
			/* keep an outstanding reference for all maps in */
			/* the parents tree except the base map */
			vm_map_reference(src_map);
			vm_map_unlock(ptr->parent_map);
			if (!vm_map_lookup_entry(
					src_map, src_start, &tmp_entry))
				RETURN(KERN_INVALID_ADDRESS);
			map_share = TRUE;
			if(!tmp_entry->is_sub_map)
			   vm_map_clip_start(src_map, tmp_entry, src_start);
			src_entry = tmp_entry;
		}
		if ((tmp_entry->object.vm_object != VM_OBJECT_NULL) && 
		    (tmp_entry->object.vm_object->phys_contiguous)) {
			/* This is not, supported for now.In future */
			/* we will need to detect the phys_contig   */
			/* condition and then upgrade copy_slowly   */
			/* to do physical copy from the device mem  */
			/* based object. We can piggy-back off of   */
			/* the was wired boolean to set-up the      */
			/* proper handling */
			RETURN(KERN_PROTECTION_FAILURE);
		}
		/*
		 *	Create a new address map entry to hold the result. 
		 *	Fill in the fields from the appropriate source entries.
		 *	We must unlock the source map to do this if we need
		 *	to allocate a map entry.
		 */
		if (new_entry == VM_MAP_ENTRY_NULL) {
		    version.main_timestamp = src_map->timestamp;
		    vm_map_unlock(src_map);

		    new_entry = vm_map_copy_entry_create(copy);

		    vm_map_lock(src_map);
		    if ((version.main_timestamp + 1) != src_map->timestamp) {
			if (!vm_map_lookup_entry(src_map, src_start,
					&tmp_entry)) {
				RETURN(KERN_INVALID_ADDRESS);
			}
			vm_map_clip_start(src_map, tmp_entry, src_start);
			continue; /* restart w/ new tmp_entry */
		    }
		}

		/*
		 *	Verify that the region can be read.
		 */
		if (((src_entry->protection & VM_PROT_READ) == VM_PROT_NONE &&
			!use_maxprot) ||
		    (src_entry->max_protection & VM_PROT_READ) == 0)
			RETURN(KERN_PROTECTION_FAILURE);

		/*
		 *	Clip against the endpoints of the entire region.
		 */

		vm_map_clip_end(src_map, src_entry, src_end);

		src_size = src_entry->vme_end - src_start;
		src_object = src_entry->object.vm_object;
		src_offset = src_entry->offset;
		was_wired = (src_entry->wired_count != 0);

		vm_map_entry_copy(new_entry, src_entry);
		new_entry->use_pmap = FALSE; /* clr address space specifics */

		/*
		 *	Attempt non-blocking copy-on-write optimizations.
		 */

		if (src_destroy && 
		    (src_object == VM_OBJECT_NULL || 
		    (src_object->internal && !src_object->true_share
		    && !map_share))) {
		    /*
		     * If we are destroying the source, and the object
		     * is internal, we can move the object reference
		     * from the source to the copy.  The copy is
		     * copy-on-write only if the source is.
		     * We make another reference to the object, because
		     * destroying the source entry will deallocate it.
		     */
		    vm_object_reference(src_object);

		    /*
		     * Copy is always unwired.  vm_map_copy_entry
		     * set its wired count to zero.
		     */

		    goto CopySuccessful;
		}


RestartCopy:
		XPR(XPR_VM_MAP, "vm_map_copyin_common src_obj 0x%x ent 0x%x obj 0x%x was_wired %d\n",
		    src_object, new_entry, new_entry->object.vm_object,
		    was_wired, 0);
		if ((src_object == VM_OBJECT_NULL ||
			(!was_wired && !map_share && !tmp_entry->is_shared)) &&
			vm_object_copy_quickly(
				&new_entry->object.vm_object,
				src_offset,
				src_size,
				&src_needs_copy,
				&new_entry_needs_copy)) {

			new_entry->needs_copy = new_entry_needs_copy;

			/*
			 *	Handle copy-on-write obligations
			 */

			if (src_needs_copy && !tmp_entry->needs_copy) {
				vm_object_pmap_protect(
					src_object,
					src_offset,
					src_size,
			      		(src_entry->is_shared ? 
						PMAP_NULL
						: src_map->pmap),
					src_entry->vme_start,
					src_entry->protection &
						~VM_PROT_WRITE);
				tmp_entry->needs_copy = TRUE;
			}

			/*
			 *	The map has never been unlocked, so it's safe
			 *	to move to the next entry rather than doing
			 *	another lookup.
			 */

			goto CopySuccessful;
		}

		/*
		 *	Take an object reference, so that we may
		 *	release the map lock(s).
		 */

		assert(src_object != VM_OBJECT_NULL);
		vm_object_reference(src_object);

		/*
		 *	Record the timestamp for later verification.
		 *	Unlock the map.
		 */

		version.main_timestamp = src_map->timestamp;
		vm_map_unlock(src_map);	/* Increments timestamp once! */

		/*
		 *	Perform the copy
		 */

		if (was_wired) {
		CopySlowly:
			vm_object_lock(src_object);
			result = vm_object_copy_slowly(
					src_object,
					src_offset,
					src_size,
					THREAD_UNINT,
					&new_entry->object.vm_object);
			new_entry->offset = 0;
			new_entry->needs_copy = FALSE;

		}
		else if (src_object->copy_strategy == MEMORY_OBJECT_COPY_SYMMETRIC &&
			(tmp_entry->is_shared  || map_share)) {
		  	vm_object_t new_object;

			vm_object_lock(src_object);
			new_object = vm_object_copy_delayed(
					src_object,
					src_offset,	
					src_size);
			if (new_object == VM_OBJECT_NULL)
			  	goto CopySlowly;

			new_entry->object.vm_object = new_object;
			new_entry->needs_copy = TRUE;
			result = KERN_SUCCESS;

		} else {
			result = vm_object_copy_strategically(src_object,
				src_offset,
				src_size,
				&new_entry->object.vm_object,
				&new_entry->offset,
				&new_entry_needs_copy);

			new_entry->needs_copy = new_entry_needs_copy;
		}

		if (result != KERN_SUCCESS &&
		    result != KERN_MEMORY_RESTART_COPY) {
			vm_map_lock(src_map);
			RETURN(result);
		}

		/*
		 *	Throw away the extra reference
		 */

		vm_object_deallocate(src_object);

		/*
		 *	Verify that the map has not substantially
		 *	changed while the copy was being made.
		 */

		vm_map_lock(src_map);

		if ((version.main_timestamp + 1) == src_map->timestamp)
			goto VerificationSuccessful;

		/*
		 *	Simple version comparison failed.
		 *
		 *	Retry the lookup and verify that the
		 *	same object/offset are still present.
		 *
		 *	[Note: a memory manager that colludes with
		 *	the calling task can detect that we have
		 *	cheated.  While the map was unlocked, the
		 *	mapping could have been changed and restored.]
		 */

		if (!vm_map_lookup_entry(src_map, src_start, &tmp_entry)) {
			RETURN(KERN_INVALID_ADDRESS);
		}

		src_entry = tmp_entry;
		vm_map_clip_start(src_map, src_entry, src_start);

		if ((((src_entry->protection & VM_PROT_READ) == VM_PROT_NONE) &&
		     !use_maxprot) ||
		    ((src_entry->max_protection & VM_PROT_READ) == 0))
			goto VerificationFailed;

		if (src_entry->vme_end < new_entry->vme_end)
			src_size = (new_entry->vme_end = src_entry->vme_end) - src_start;

		if ((src_entry->object.vm_object != src_object) ||
		    (src_entry->offset != src_offset) ) {

			/*
			 *	Verification failed.
			 *
			 *	Start over with this top-level entry.
			 */

		 VerificationFailed: ;

			vm_object_deallocate(new_entry->object.vm_object);
			tmp_entry = src_entry;
			continue;
		}

		/*
		 *	Verification succeeded.
		 */

	 VerificationSuccessful: ;

		if (result == KERN_MEMORY_RESTART_COPY)
			goto RestartCopy;

		/*
		 *	Copy succeeded.
		 */

	 CopySuccessful: ;

		/*
		 *	Link in the new copy entry.
		 */

		vm_map_copy_entry_link(copy, vm_map_copy_last_entry(copy),
				       new_entry);
		
		/*
		 *	Determine whether the entire region
		 *	has been copied.
		 */
		src_start = new_entry->vme_end;
		new_entry = VM_MAP_ENTRY_NULL;
		while ((src_start >= src_end) && (src_end != 0)) {
			if (src_map != base_map) {
				submap_map_t	*ptr;

				ptr = parent_maps;
				assert(ptr != NULL);
				parent_maps = parent_maps->next;
				vm_map_unlock(src_map);
				vm_map_deallocate(src_map);
				vm_map_lock(ptr->parent_map);
				src_map = ptr->parent_map;
				src_start = ptr->base_start;
				src_end = ptr->base_end;
				if ((src_end > src_start) &&
					      !vm_map_lookup_entry(
					      src_map, src_start, &tmp_entry))
					RETURN(KERN_INVALID_ADDRESS);
				kfree(ptr, sizeof(submap_map_t));
				if(parent_maps == NULL)
					map_share = FALSE;
				src_entry = tmp_entry->vme_prev;
			} else
				break;
		}
		if ((src_start >= src_end) && (src_end != 0))
			break;

		/*
		 *	Verify that there are no gaps in the region
		 */

		tmp_entry = src_entry->vme_next;
		if ((tmp_entry->vme_start != src_start) || 
				(tmp_entry == vm_map_to_entry(src_map)))
			RETURN(KERN_INVALID_ADDRESS);
	}

	/*
	 * If the source should be destroyed, do it now, since the
	 * copy was successful. 
	 */
	if (src_destroy) {
		(void) vm_map_delete(src_map,
				     vm_map_trunc_page(src_addr),
				     src_end,
				     (src_map == kernel_map) ?
					VM_MAP_REMOVE_KUNWIRE :
				        VM_MAP_NO_FLAGS,
				     VM_MAP_NULL);
	}

	vm_map_unlock(src_map);

	/* Fix-up start and end points in copy.  This is necessary */
	/* when the various entries in the copy object were picked */
	/* up from different sub-maps */

	tmp_entry = vm_map_copy_first_entry(copy);
	while (tmp_entry != vm_map_copy_to_entry(copy)) {
		tmp_entry->vme_end = copy_addr + 
			(tmp_entry->vme_end - tmp_entry->vme_start);
		tmp_entry->vme_start = copy_addr;
		copy_addr += tmp_entry->vme_end - tmp_entry->vme_start;
		tmp_entry = (struct vm_map_entry *)tmp_entry->vme_next;
	}

	*copy_result = copy;
	return(KERN_SUCCESS);

#undef	RETURN
}

/*
 *	vm_map_copyin_object:
 *
 *	Create a copy object from an object.
 *	Our caller donates an object reference.
 */

kern_return_t
vm_map_copyin_object(
	vm_object_t		object,
	vm_object_offset_t	offset,	/* offset of region in object */
	vm_object_size_t	size,	/* size of region in object */
	vm_map_copy_t	*copy_result)	/* OUT */
{
	vm_map_copy_t	copy;		/* Resulting copy */

	/*
	 *	We drop the object into a special copy object
	 *	that contains the object directly.
	 */

	copy = (vm_map_copy_t) zalloc(vm_map_copy_zone);
	copy->type = VM_MAP_COPY_OBJECT;
	copy->cpy_object = object;
	copy->offset = offset;
	copy->size = size;

	*copy_result = copy;
	return(KERN_SUCCESS);
}

static void
vm_map_fork_share(
	vm_map_t	old_map,
	vm_map_entry_t	old_entry,
	vm_map_t	new_map)
{
	vm_object_t 	object;
	vm_map_entry_t 	new_entry;

	/*
	 *	New sharing code.  New map entry
	 *	references original object.  Internal
	 *	objects use asynchronous copy algorithm for
	 *	future copies.  First make sure we have
	 *	the right object.  If we need a shadow,
	 *	or someone else already has one, then
	 *	make a new shadow and share it.
	 */
	
	object = old_entry->object.vm_object;
	if (old_entry->is_sub_map) {
		assert(old_entry->wired_count == 0);
#ifndef i386
		if(old_entry->use_pmap) {
			kern_return_t	result;

			result = pmap_nest(new_map->pmap, 
				(old_entry->object.sub_map)->pmap, 
				(addr64_t)old_entry->vme_start,
				(addr64_t)old_entry->vme_start,
				(uint64_t)(old_entry->vme_end - old_entry->vme_start));
			if(result)
				panic("vm_map_fork_share: pmap_nest failed!");
		}
#endif
	} else if (object == VM_OBJECT_NULL) {
		object = vm_object_allocate((vm_map_size_t)(old_entry->vme_end -
							old_entry->vme_start));
		old_entry->offset = 0;
		old_entry->object.vm_object = object;
		assert(!old_entry->needs_copy);
	} else if (object->copy_strategy !=
		 MEMORY_OBJECT_COPY_SYMMETRIC) {
		
		/*
		 *	We are already using an asymmetric
		 *	copy, and therefore we already have
		 *	the right object.
		 */
		
		assert(! old_entry->needs_copy);
	}
	else if (old_entry->needs_copy ||	/* case 1 */
		 object->shadowed ||		/* case 2 */
		 (!object->true_share && 	/* case 3 */
		 !old_entry->is_shared &&
		 (object->size >
		  (vm_map_size_t)(old_entry->vme_end -
			      old_entry->vme_start)))) {
		
		/*
		 *	We need to create a shadow.
		 *	There are three cases here.
		 *	In the first case, we need to
		 *	complete a deferred symmetrical
		 *	copy that we participated in.
		 *	In the second and third cases,
		 *	we need to create the shadow so
		 *	that changes that we make to the
		 *	object do not interfere with
		 *	any symmetrical copies which
		 *	have occured (case 2) or which
		 *	might occur (case 3).
		 *
		 *	The first case is when we had
		 *	deferred shadow object creation
		 *	via the entry->needs_copy mechanism.
		 *	This mechanism only works when
		 *	only one entry points to the source
		 *	object, and we are about to create
		 *	a second entry pointing to the
		 *	same object. The problem is that
		 *	there is no way of mapping from
		 *	an object to the entries pointing
		 *	to it. (Deferred shadow creation
		 *	works with one entry because occurs
		 *	at fault time, and we walk from the
		 *	entry to the object when handling
		 *	the fault.)
		 *
		 *	The second case is when the object
		 *	to be shared has already been copied
		 *	with a symmetric copy, but we point
		 *	directly to the object without
		 *	needs_copy set in our entry. (This
		 *	can happen because different ranges
		 *	of an object can be pointed to by
		 *	different entries. In particular,
		 *	a single entry pointing to an object
		 *	can be split by a call to vm_inherit,
		 *	which, combined with task_create, can
		 *	result in the different entries
		 *	having different needs_copy values.)
		 *	The shadowed flag in the object allows
		 *	us to detect this case. The problem
		 *	with this case is that if this object
		 *	has or will have shadows, then we
		 *	must not perform an asymmetric copy
		 *	of this object, since such a copy
		 *	allows the object to be changed, which
		 *	will break the previous symmetrical
		 *	copies (which rely upon the object
		 *	not changing). In a sense, the shadowed
		 *	flag says "don't change this object".
		 *	We fix this by creating a shadow
		 *	object for this object, and sharing
		 *	that. This works because we are free
		 *	to change the shadow object (and thus
		 *	to use an asymmetric copy strategy);
		 *	this is also semantically correct,
		 *	since this object is temporary, and
		 *	therefore a copy of the object is
		 *	as good as the object itself. (This
		 *	is not true for permanent objects,
		 *	since the pager needs to see changes,
		 *	which won't happen if the changes
		 *	are made to a copy.)
		 *
		 *	The third case is when the object
		 *	to be shared has parts sticking
		 *	outside of the entry we're working
		 *	with, and thus may in the future
		 *	be subject to a symmetrical copy.
		 *	(This is a preemptive version of
		 *	case 2.)
		 */
		
		assert(!(object->shadowed && old_entry->is_shared));
		vm_object_shadow(&old_entry->object.vm_object,
				 &old_entry->offset,
				 (vm_map_size_t) (old_entry->vme_end -
					      old_entry->vme_start));
		
		/*
		 *	If we're making a shadow for other than
		 *	copy on write reasons, then we have
		 *	to remove write permission.
		 */

		if (!old_entry->needs_copy &&
		    (old_entry->protection & VM_PROT_WRITE)) {
			if(old_map->mapped) {
				vm_object_pmap_protect(
					old_entry->object.vm_object,
					old_entry->offset,
					(old_entry->vme_end -
							old_entry->vme_start),
					PMAP_NULL,
					old_entry->vme_start,
					old_entry->protection & ~VM_PROT_WRITE);
			} else {
				pmap_protect(old_map->pmap,
				     old_entry->vme_start,
				     old_entry->vme_end,
				     old_entry->protection & ~VM_PROT_WRITE);
			}
		}
		
		old_entry->needs_copy = FALSE;
		object = old_entry->object.vm_object;
	}
	
	/*
	 *	If object was using a symmetric copy strategy,
	 *	change its copy strategy to the default
	 *	asymmetric copy strategy, which is copy_delay
	 *	in the non-norma case and copy_call in the
	 *	norma case. Bump the reference count for the
	 *	new entry.
	 */
	
	if(old_entry->is_sub_map) {
		vm_map_lock(old_entry->object.sub_map);
		vm_map_reference(old_entry->object.sub_map);
		vm_map_unlock(old_entry->object.sub_map);
	} else {
		vm_object_lock(object);
		object->ref_count++;
		vm_object_res_reference(object);
		if (object->copy_strategy == MEMORY_OBJECT_COPY_SYMMETRIC) {
			object->copy_strategy = MEMORY_OBJECT_COPY_DELAY;
		}
		vm_object_unlock(object);
	}
	
	/*
	 *	Clone the entry, using object ref from above.
	 *	Mark both entries as shared.
	 */
	
	new_entry = vm_map_entry_create(new_map);
	vm_map_entry_copy(new_entry, old_entry);
	old_entry->is_shared = TRUE;
	new_entry->is_shared = TRUE;
	
	/*
	 *	Insert the entry into the new map -- we
	 *	know we're inserting at the end of the new
	 *	map.
	 */
	
	vm_map_entry_link(new_map, vm_map_last_entry(new_map), new_entry);
	
	/*
	 *	Update the physical map
	 */
	
	if (old_entry->is_sub_map) {
		/* Bill Angell pmap support goes here */
	} else {
		pmap_copy(new_map->pmap, old_map->pmap, new_entry->vme_start,
		  old_entry->vme_end - old_entry->vme_start,
		  old_entry->vme_start);
	}
}

static boolean_t
vm_map_fork_copy(
	vm_map_t	old_map,
	vm_map_entry_t	*old_entry_p,
	vm_map_t	new_map)
{
	vm_map_entry_t old_entry = *old_entry_p;
	vm_map_size_t entry_size = old_entry->vme_end - old_entry->vme_start;
	vm_map_offset_t start = old_entry->vme_start;
	vm_map_copy_t copy;
	vm_map_entry_t last = vm_map_last_entry(new_map);

	vm_map_unlock(old_map);
	/*
	 *	Use maxprot version of copyin because we
	 *	care about whether this memory can ever
	 *	be accessed, not just whether it's accessible
	 *	right now.
	 */
	if (vm_map_copyin_maxprot(old_map, start, entry_size, FALSE, &copy)
	    != KERN_SUCCESS) {
		/*
		 *	The map might have changed while it
		 *	was unlocked, check it again.  Skip
		 *	any blank space or permanently
		 *	unreadable region.
		 */
		vm_map_lock(old_map);
		if (!vm_map_lookup_entry(old_map, start, &last) ||
		    (last->max_protection & VM_PROT_READ) == VM_PROT_NONE) {
			last = last->vme_next;
		}
		*old_entry_p = last;

		/*
		 * XXX	For some error returns, want to
		 * XXX	skip to the next element.  Note
		 *	that INVALID_ADDRESS and
		 *	PROTECTION_FAILURE are handled above.
		 */
		
		return FALSE;
	}
	
	/*
	 *	Insert the copy into the new map
	 */
	
	vm_map_copy_insert(new_map, last, copy);
	
	/*
	 *	Pick up the traversal at the end of
	 *	the copied region.
	 */
	
	vm_map_lock(old_map);
	start += entry_size;
	if (! vm_map_lookup_entry(old_map, start, &last)) {
		last = last->vme_next;
	} else {
		vm_map_clip_start(old_map, last, start);
	}
	*old_entry_p = last;

	return TRUE;
}

/*
 *	vm_map_fork:
 *
 *	Create and return a new map based on the old
 *	map, according to the inheritance values on the
 *	regions in that map.
 *
 *	The source map must not be locked.
 */
vm_map_t
vm_map_fork(
	vm_map_t	old_map)
{
	pmap_t		new_pmap = pmap_create((vm_map_size_t) 0);
	vm_map_t	new_map;
	vm_map_entry_t	old_entry;
	vm_map_size_t	new_size = 0, entry_size;
	vm_map_entry_t	new_entry;
	boolean_t	src_needs_copy;
	boolean_t	new_entry_needs_copy;

	vm_map_reference_swap(old_map);
	vm_map_lock(old_map);

	new_map = vm_map_create(new_pmap,
			old_map->min_offset,
			old_map->max_offset,
			old_map->hdr.entries_pageable);

	for (
	    old_entry = vm_map_first_entry(old_map);
	    old_entry != vm_map_to_entry(old_map);
	    ) {

		entry_size = old_entry->vme_end - old_entry->vme_start;

		switch (old_entry->inheritance) {
		case VM_INHERIT_NONE:
			break;

		case VM_INHERIT_SHARE:
			vm_map_fork_share(old_map, old_entry, new_map);
			new_size += entry_size;
			break;

		case VM_INHERIT_COPY:

			/*
			 *	Inline the copy_quickly case;
			 *	upon failure, fall back on call
			 *	to vm_map_fork_copy.
			 */

			if(old_entry->is_sub_map)
				break;
			if ((old_entry->wired_count != 0) ||
				((old_entry->object.vm_object != NULL) &&
				(old_entry->object.vm_object->true_share))) {
				goto slow_vm_map_fork_copy;
			}

			new_entry = vm_map_entry_create(new_map);
			vm_map_entry_copy(new_entry, old_entry);
			/* clear address space specifics */
			new_entry->use_pmap = FALSE;

			if (! vm_object_copy_quickly(
						&new_entry->object.vm_object,
						old_entry->offset,
						(old_entry->vme_end -
							old_entry->vme_start),
						&src_needs_copy,
						&new_entry_needs_copy)) {
				vm_map_entry_dispose(new_map, new_entry);
				goto slow_vm_map_fork_copy;
			}

			/*
			 *	Handle copy-on-write obligations
			 */
			
			if (src_needs_copy && !old_entry->needs_copy) {
				vm_object_pmap_protect(
					old_entry->object.vm_object,
					old_entry->offset,
					(old_entry->vme_end -
							old_entry->vme_start),
					((old_entry->is_shared 
						|| old_map->mapped)
							? PMAP_NULL :
							old_map->pmap),
					old_entry->vme_start,
					old_entry->protection & ~VM_PROT_WRITE);

				old_entry->needs_copy = TRUE;
			}
			new_entry->needs_copy = new_entry_needs_copy;
			
			/*
			 *	Insert the entry at the end
			 *	of the map.
			 */
			
			vm_map_entry_link(new_map, vm_map_last_entry(new_map),
					  new_entry);
			new_size += entry_size;
			break;

		slow_vm_map_fork_copy:
			if (vm_map_fork_copy(old_map, &old_entry, new_map)) {
				new_size += entry_size;
			}
			continue;
		}
		old_entry = old_entry->vme_next;
	}

	new_map->size = new_size;
	vm_map_unlock(old_map);
	vm_map_deallocate(old_map);

	return(new_map);
}


/*
 *	vm_map_lookup_locked:
 *
 *	Finds the VM object, offset, and
 *	protection for a given virtual address in the
 *	specified map, assuming a page fault of the
 *	type specified.
 *
 *	Returns the (object, offset, protection) for
 *	this address, whether it is wired down, and whether
 *	this map has the only reference to the data in question.
 *	In order to later verify this lookup, a "version"
 *	is returned.
 *
 *	The map MUST be locked by the caller and WILL be
 *	locked on exit.  In order to guarantee the
 *	existence of the returned object, it is returned
 *	locked.
 *
 *	If a lookup is requested with "write protection"
 *	specified, the map may be changed to perform virtual
 *	copying operations, although the data referenced will
 *	remain the same.
 */
kern_return_t
vm_map_lookup_locked(
	vm_map_t		*var_map,	/* IN/OUT */
	vm_map_offset_t	vaddr,
	vm_prot_t		fault_type,
	vm_map_version_t	*out_version,	/* OUT */
	vm_object_t		*object,	/* OUT */
	vm_object_offset_t	*offset,	/* OUT */
	vm_prot_t		*out_prot,	/* OUT */
	boolean_t		*wired,		/* OUT */
	int			*behavior,	/* OUT */
	vm_map_offset_t	*lo_offset,	/* OUT */
	vm_map_offset_t	*hi_offset,	/* OUT */
	vm_map_t		*real_map)
{
	vm_map_entry_t			entry;
	register vm_map_t		map = *var_map;
	vm_map_t			old_map = *var_map;
	vm_map_t			cow_sub_map_parent = VM_MAP_NULL;
	vm_map_offset_t			cow_parent_vaddr = 0;
	vm_map_offset_t			old_start = 0;
	vm_map_offset_t			old_end = 0;
	register vm_prot_t		prot;

	*real_map = map;
	RetryLookup: ;

	/*
	 *	If the map has an interesting hint, try it before calling
	 *	full blown lookup routine.
	 */

	mutex_lock(&map->s_lock);
	entry = map->hint;
	mutex_unlock(&map->s_lock);

	if ((entry == vm_map_to_entry(map)) ||
	    (vaddr < entry->vme_start) || (vaddr >= entry->vme_end)) {
		vm_map_entry_t	tmp_entry;

		/*
		 *	Entry was either not a valid hint, or the vaddr
		 *	was not contained in the entry, so do a full lookup.
		 */
		if (!vm_map_lookup_entry(map, vaddr, &tmp_entry)) {
			if((cow_sub_map_parent) && (cow_sub_map_parent != map))
				vm_map_unlock(cow_sub_map_parent);
			if((*real_map != map) 
					&& (*real_map != cow_sub_map_parent))
				vm_map_unlock(*real_map);
			return KERN_INVALID_ADDRESS;
		}

		entry = tmp_entry;
	}
	if(map == old_map) {
		old_start = entry->vme_start;
		old_end = entry->vme_end;
	}

	/*
	 *	Handle submaps.  Drop lock on upper map, submap is
	 *	returned locked.
	 */

submap_recurse:
	if (entry->is_sub_map) {
		vm_map_offset_t		local_vaddr;
		vm_map_offset_t		end_delta;
		vm_map_offset_t		start_delta; 
		vm_map_entry_t		submap_entry;
		boolean_t		mapped_needs_copy=FALSE;

		local_vaddr = vaddr;

		if ((!entry->needs_copy) && (entry->use_pmap)) {
			/* if real_map equals map we unlock below */
			if ((*real_map != map) && 
					(*real_map != cow_sub_map_parent))
				vm_map_unlock(*real_map);
			*real_map = entry->object.sub_map;
		}

		if(entry->needs_copy) {
			if (!mapped_needs_copy) {
				if (vm_map_lock_read_to_write(map)) {
					vm_map_lock_read(map);
					if(*real_map == entry->object.sub_map)
						*real_map = map;
					goto RetryLookup;
				}
				vm_map_lock_read(entry->object.sub_map);
				cow_sub_map_parent = map;
				/* reset base to map before cow object */
				/* this is the map which will accept   */
				/* the new cow object */
				old_start = entry->vme_start;
				old_end = entry->vme_end;
				cow_parent_vaddr = vaddr;
				mapped_needs_copy = TRUE;
			} else {
				vm_map_lock_read(entry->object.sub_map);
				if((cow_sub_map_parent != map) &&
							(*real_map != map))
					vm_map_unlock(map);
			}
		} else {
			vm_map_lock_read(entry->object.sub_map);
			/* leave map locked if it is a target */
			/* cow sub_map above otherwise, just  */
			/* follow the maps down to the object */
			/* here we unlock knowing we are not  */
			/* revisiting the map.  */
			if((*real_map != map) && (map != cow_sub_map_parent))
				vm_map_unlock_read(map);
		}

		*var_map = map = entry->object.sub_map;

		/* calculate the offset in the submap for vaddr */
		local_vaddr = (local_vaddr - entry->vme_start) + entry->offset;

RetrySubMap:
		if(!vm_map_lookup_entry(map, local_vaddr, &submap_entry)) {
			if((cow_sub_map_parent) && (cow_sub_map_parent != map)){
				vm_map_unlock(cow_sub_map_parent);
			}
			if((*real_map != map) 
					&& (*real_map != cow_sub_map_parent)) {
				vm_map_unlock(*real_map);
			}
			*real_map = map;
			return KERN_INVALID_ADDRESS;
		}
		/* find the attenuated shadow of the underlying object */
		/* on our target map */

		/* in english the submap object may extend beyond the     */
		/* region mapped by the entry or, may only fill a portion */
		/* of it.  For our purposes, we only care if the object   */
		/* doesn't fill.  In this case the area which will        */
		/* ultimately be clipped in the top map will only need    */
		/* to be as big as the portion of the underlying entry    */
		/* which is mapped */
		start_delta = submap_entry->vme_start > entry->offset ?
	  	            submap_entry->vme_start - entry->offset : 0;

		end_delta = 
		   (entry->offset + start_delta + (old_end - old_start)) <=
			submap_entry->vme_end ?
				0 : (entry->offset + 
					(old_end - old_start))
					- submap_entry->vme_end; 

		old_start += start_delta;
		old_end -= end_delta;

		if(submap_entry->is_sub_map) {
			entry = submap_entry;
			vaddr = local_vaddr;
			goto submap_recurse;
		}

		if(((fault_type & VM_PROT_WRITE) && cow_sub_map_parent)) {

			vm_object_t	copy_object;
			vm_map_offset_t	local_start;
			vm_map_offset_t	local_end;
			boolean_t		copied_slowly = FALSE;

			if (vm_map_lock_read_to_write(map)) {
				vm_map_lock_read(map);
				old_start -= start_delta;
				old_end += end_delta;
				goto RetrySubMap;
			}


			if (submap_entry->object.vm_object == VM_OBJECT_NULL) {
				submap_entry->object.vm_object = 
					vm_object_allocate(
						(vm_map_size_t)
						(submap_entry->vme_end 
						- submap_entry->vme_start));
				submap_entry->offset = 0;
			}
			local_start =  local_vaddr - 
					(cow_parent_vaddr - old_start);
			local_end = local_vaddr + 
					(old_end - cow_parent_vaddr);
			vm_map_clip_start(map, submap_entry, local_start);
			vm_map_clip_end(map, submap_entry, local_end);

			/* This is the COW case, lets connect */
			/* an entry in our space to the underlying */
			/* object in the submap, bypassing the  */
			/* submap. */


			if(submap_entry->wired_count != 0) {
					vm_object_lock(
					     submap_entry->object.vm_object);
					vm_object_copy_slowly(
						submap_entry->object.vm_object,
						submap_entry->offset,
						submap_entry->vme_end -
							submap_entry->vme_start,
						FALSE,
						&copy_object);
					copied_slowly = TRUE;
			} else {
				
				/* set up shadow object */
				copy_object = submap_entry->object.vm_object;
				vm_object_reference(copy_object);
				submap_entry->object.vm_object->shadowed = TRUE;
				submap_entry->needs_copy = TRUE;
				vm_object_pmap_protect(
					submap_entry->object.vm_object,
					submap_entry->offset,
					submap_entry->vme_end - 
						submap_entry->vme_start,
					(submap_entry->is_shared 
						|| map->mapped) ?
						PMAP_NULL : map->pmap,
					submap_entry->vme_start,
					submap_entry->protection &
						~VM_PROT_WRITE);
			}
			

			/* This works diffently than the   */
			/* normal submap case. We go back  */
			/* to the parent of the cow map and*/
			/* clip out the target portion of  */
			/* the sub_map, substituting the   */
			/* new copy object,                */

			vm_map_unlock(map);
			local_start = old_start;
			local_end = old_end;
			map = cow_sub_map_parent;
			*var_map = cow_sub_map_parent;
			vaddr = cow_parent_vaddr;
			cow_sub_map_parent = NULL;

			if(!vm_map_lookup_entry(map, 
					vaddr, &entry)) {
				        vm_object_deallocate(
							copy_object);
					vm_map_lock_write_to_read(map);
					return KERN_INVALID_ADDRESS;
			}
					
			/* clip out the portion of space */
			/* mapped by the sub map which   */
			/* corresponds to the underlying */
			/* object */
			vm_map_clip_start(map, entry, local_start);
			vm_map_clip_end(map, entry, local_end);


			/* substitute copy object for */
			/* shared map entry           */
			vm_map_deallocate(entry->object.sub_map);
			entry->is_sub_map = FALSE;
			entry->object.vm_object = copy_object;

			entry->protection |= VM_PROT_WRITE;
			entry->max_protection |= VM_PROT_WRITE;
			if(copied_slowly) {
				entry->offset = 0;
				entry->needs_copy = FALSE;
				entry->is_shared = FALSE;
			} else {
				entry->offset = submap_entry->offset;
				entry->needs_copy = TRUE;
				if(entry->inheritance == VM_INHERIT_SHARE) 
					entry->inheritance = VM_INHERIT_COPY;
				if (map != old_map)
					entry->is_shared = TRUE;
			}
			if(entry->inheritance == VM_INHERIT_SHARE) 
				entry->inheritance = VM_INHERIT_COPY;

			vm_map_lock_write_to_read(map);
		} else {
			if((cow_sub_map_parent)
					&& (cow_sub_map_parent != *real_map)
					&& (cow_sub_map_parent != map)) {
				vm_map_unlock(cow_sub_map_parent);
			}
			entry = submap_entry;
			vaddr = local_vaddr;
		}
	}
		
	/*
	 *	Check whether this task is allowed to have
	 *	this page.
	 */

	prot = entry->protection;
	if ((fault_type & (prot)) != fault_type) {
	  if (*real_map != map) {
		vm_map_unlock(*real_map);
	  }
	  *real_map = map;
	  return KERN_PROTECTION_FAILURE;
	}

	/*
	 *	If this page is not pageable, we have to get
	 *	it for all possible accesses.
	 */

	*wired = (entry->wired_count != 0);
	if (*wired)
		prot = fault_type = entry->protection;

	/*
	 *	If the entry was copy-on-write, we either ...
	 */

	if (entry->needs_copy) {
	    	/*
		 *	If we want to write the page, we may as well
		 *	handle that now since we've got the map locked.
		 *
		 *	If we don't need to write the page, we just
		 *	demote the permissions allowed.
		 */

		if ((fault_type & VM_PROT_WRITE) || *wired) {
			/*
			 *	Make a new object, and place it in the
			 *	object chain.  Note that no new references
			 *	have appeared -- one just moved from the
			 *	map to the new object.
			 */

			if (vm_map_lock_read_to_write(map)) {
				vm_map_lock_read(map);
				goto RetryLookup;
			}
			vm_object_shadow(&entry->object.vm_object,
					 &entry->offset,
					 (vm_map_size_t) (entry->vme_end -
						      entry->vme_start));

			entry->object.vm_object->shadowed = TRUE;
			entry->needs_copy = FALSE;
			vm_map_lock_write_to_read(map);
		}
		else {
			/*
			 *	We're attempting to read a copy-on-write
			 *	page -- don't allow writes.
			 */

			prot &= (~VM_PROT_WRITE);
		}
	}

	/*
	 *	Create an object if necessary.
	 */
	if (entry->object.vm_object == VM_OBJECT_NULL) {

		if (vm_map_lock_read_to_write(map)) {
			vm_map_lock_read(map);
			goto RetryLookup;
		}

		entry->object.vm_object = vm_object_allocate(
			(vm_map_size_t)(entry->vme_end - entry->vme_start));
		entry->offset = 0;
		vm_map_lock_write_to_read(map);
	}

	/*
	 *	Return the object/offset from this entry.  If the entry
	 *	was copy-on-write or empty, it has been fixed up.  Also
	 *	return the protection.
	 */

        *offset = (vaddr - entry->vme_start) + entry->offset;
        *object = entry->object.vm_object;
	*out_prot = prot;
	*behavior = entry->behavior;
	*lo_offset = entry->offset;
	*hi_offset = (entry->vme_end - entry->vme_start) + entry->offset;

	/*
	 *	Lock the object to prevent it from disappearing
	 */

	vm_object_lock(*object);

	/*
	 *	Save the version number
	 */

	out_version->main_timestamp = map->timestamp;

	return KERN_SUCCESS;
}


/*
 *	vm_map_verify:
 *
 *	Verifies that the map in question has not changed
 *	since the given version.  If successful, the map
 *	will not change until vm_map_verify_done() is called.
 */
boolean_t
vm_map_verify(
	register vm_map_t		map,
	register vm_map_version_t	*version)	/* REF */
{
	boolean_t	result;

	vm_map_lock_read(map);
	result = (map->timestamp == version->main_timestamp);

	if (!result)
		vm_map_unlock_read(map);

	return(result);
}

/*
 *	vm_map_verify_done:
 *
 *	Releases locks acquired by a vm_map_verify.
 *
 *	This is now a macro in vm/vm_map.h.  It does a
 *	vm_map_unlock_read on the map.
 */


/*
 *	TEMPORARYTEMPORARYTEMPORARYTEMPORARYTEMPORARYTEMPORARY
 *	Goes away after regular vm_region_recurse function migrates to
 *	64 bits
 *	vm_region_recurse: A form of vm_region which follows the
 *	submaps in a target map
 *
 */

kern_return_t
vm_map_region_recurse_64(
	vm_map_t		 map,
	vm_map_offset_t	*address,		/* IN/OUT */
	vm_map_size_t		*size,			/* OUT */
	natural_t	 	*nesting_depth,	/* IN/OUT */
	vm_region_submap_info_64_t	submap_info,	/* IN/OUT */
	mach_msg_type_number_t	*count)	/* IN/OUT */
{
	vm_region_extended_info_data_t	extended;
	vm_map_entry_t			tmp_entry;
	vm_map_offset_t			user_address;
	unsigned int			user_max_depth;

	/*
	 * "curr_entry" is the VM map entry preceding or including the
	 * address we're looking for.
	 * "curr_map" is the map or sub-map containing "curr_entry".
	 * "curr_offset" is the cumulated offset of "curr_map" in the
	 * target task's address space.
	 * "curr_depth" is the depth of "curr_map" in the chain of
	 * sub-maps.
	 * "curr_max_offset" is the maximum offset we should take into
	 * account in the current map.  It may be smaller than the current
	 * map's "max_offset" because we might not have mapped it all in
	 * the upper level map.
	 */
	vm_map_entry_t			curr_entry;
	vm_map_offset_t			curr_offset;
	vm_map_t			curr_map;
	unsigned int			curr_depth;
	vm_map_offset_t			curr_max_offset;

	/*
	 * "next_" is the same as "curr_" but for the VM region immediately
	 * after the address we're looking for.  We need to keep track of this
	 * too because we want to return info about that region if the
	 * address we're looking for is not mapped.
	 */
	vm_map_entry_t			next_entry;
	vm_map_offset_t			next_offset;
	vm_map_t			next_map;
	unsigned int			next_depth;
	vm_map_offset_t			next_max_offset;

	if (map == VM_MAP_NULL) {
		/* no address space to work on */
		return KERN_INVALID_ARGUMENT;
	}

	if (*count < VM_REGION_SUBMAP_INFO_COUNT_64) {
		/* "info" structure is not big enough and would overflow */
		return KERN_INVALID_ARGUMENT;
	}

	*count = VM_REGION_SUBMAP_INFO_COUNT_64;

	user_address = *address;
	user_max_depth = *nesting_depth;
	
	curr_entry = NULL;
	curr_map = map;
	curr_offset = 0;
	curr_depth = 0;
	curr_max_offset = curr_map->max_offset;

	next_entry = NULL;
	next_map = NULL;
	next_offset = 0;
	next_depth = 0;
	next_max_offset = curr_max_offset;

	if (not_in_kdp) {
		vm_map_lock_read(curr_map);
	}

	for (;;) {
		if (vm_map_lookup_entry(curr_map,
					user_address - curr_offset,
					&tmp_entry)) {
			/* tmp_entry contains the address we're looking for */
			curr_entry = tmp_entry;
		} else {
			/*
			 * The address is not mapped.  "tmp_entry" is the
			 * map entry preceding the address.  We want the next
			 * one, if it exists.
			 */
			curr_entry = tmp_entry->vme_next;
			if (curr_entry == vm_map_to_entry(curr_map) ||
			    curr_entry->vme_start >= curr_max_offset) {
				/* no next entry at this level: stop looking */
				if (not_in_kdp) {
					vm_map_unlock_read(curr_map);
				}
				curr_entry = NULL;
				curr_map = NULL;
				curr_offset = 0;
				curr_depth = 0;
				curr_max_offset = 0;
				break;
			}
		}

		/*
		 * Is the next entry at this level closer to the address (or
		 * deeper in the submap chain) than the one we had
		 * so far ?
		 */
		tmp_entry = curr_entry->vme_next;
		if (tmp_entry == vm_map_to_entry(curr_map)) {
			/* no next entry at this level */
		} else if (tmp_entry->vme_start >= curr_max_offset) {
			/*
			 * tmp_entry is beyond the scope of what we mapped of
			 * this submap in the upper level: ignore it.
			 */
		} else if ((next_entry == NULL) ||
			   (tmp_entry->vme_start + curr_offset <=
			    next_entry->vme_start + next_offset)) {
			/*
			 * We didn't have a "next_entry" or this one is
			 * closer to the address we're looking for:
			 * use this "tmp_entry" as the new "next_entry".
			 */
			if (next_entry != NULL) {
				/* unlock the last "next_map" */
				if (next_map != curr_map && not_in_kdp) {
					vm_map_unlock_read(next_map);
				}
			}
			next_entry = tmp_entry;
			next_map = curr_map;
			next_offset = curr_offset;
			next_depth = curr_depth;
			next_max_offset = curr_max_offset;
		}

		if (!curr_entry->is_sub_map ||
		    curr_depth >= user_max_depth) {
			/*
			 * We hit a leaf map or we reached the maximum depth
			 * we could, so stop looking.  Keep the current map
			 * locked.
			 */
			break;
		}

		/*
		 * Get down to the next submap level.
		 */

		/*
		 * Lock the next level and unlock the current level,
		 * unless we need to keep it locked to access the "next_entry"
		 * later.
		 */
		if (not_in_kdp) {
			vm_map_lock_read(curr_entry->object.sub_map);
		}
		if (curr_map == next_map) {
			/* keep "next_map" locked in case we need it */
		} else {
			/* release this map */
			vm_map_unlock_read(curr_map);
		}

		/*
		 * Adjust the offset.  "curr_entry" maps the submap
		 * at relative address "curr_entry->vme_start" in the
		 * curr_map but skips the first "curr_entry->offset"
		 * bytes of the submap.
		 * "curr_offset" always represents the offset of a virtual
		 * address in the curr_map relative to the absolute address
		 * space (i.e. the top-level VM map).
		 */
		curr_offset +=
			(curr_entry->vme_start - curr_entry->offset);
		/* switch to the submap */
		curr_map = curr_entry->object.sub_map;
		curr_depth++;
		/*
		 * "curr_max_offset" allows us to keep track of the
		 * portion of the submap that is actually mapped at this level:
		 * the rest of that submap is irrelevant to us, since it's not
		 * mapped here.
		 * The relevant portion of the map starts at
		 * "curr_entry->offset" up to the size of "curr_entry".
		 */
		curr_max_offset =
			curr_entry->vme_end - curr_entry->vme_start +
			curr_entry->offset;
		curr_entry = NULL;
	}

	if (curr_entry == NULL) {
		/* no VM region contains the address... */
		if (next_entry == NULL) {
			/* ... and no VM region follows it either */
			return KERN_INVALID_ADDRESS;
		}
		/* ... gather info about the next VM region */
		curr_entry = next_entry;
		curr_map = next_map;	/* still locked ... */
		curr_offset = next_offset;
		curr_depth = next_depth;
		curr_max_offset = next_max_offset;
	} else {
		/* we won't need "next_entry" after all */
		if (next_entry != NULL) {
			/* release "next_map" */
			if (next_map != curr_map && not_in_kdp) {
				vm_map_unlock_read(next_map);
			}
		}
	}
	next_entry = NULL;
	next_map = NULL;
	next_offset = 0;
	next_depth = 0;
	next_max_offset = 0;

	*nesting_depth = curr_depth;
	*size = curr_entry->vme_end - curr_entry->vme_start;
	*address = curr_entry->vme_start + curr_offset;

	submap_info->user_tag = curr_entry->alias;
	submap_info->offset = curr_entry->offset; 
	submap_info->protection = curr_entry->protection;
	submap_info->inheritance = curr_entry->inheritance;
	submap_info->max_protection = curr_entry->max_protection;
	submap_info->behavior = curr_entry->behavior;
	submap_info->user_wired_count = curr_entry->user_wired_count;
	submap_info->is_submap = curr_entry->is_sub_map;
	submap_info->object_id = (uint32_t) curr_entry->object.vm_object;

	extended.pages_resident = 0;
	extended.pages_swapped_out = 0;
	extended.pages_shared_now_private = 0;
	extended.pages_dirtied = 0;
	extended.external_pager = 0;
	extended.shadow_depth = 0;

	if (not_in_kdp) {
		if (!curr_entry->is_sub_map) {
			vm_map_region_walk(curr_map,
					   curr_entry->vme_start,
					   curr_entry,
					   curr_entry->offset,
					   (curr_entry->vme_end -
					    curr_entry->vme_start),
					   &extended);
			submap_info->share_mode = extended.share_mode;
			if (extended.external_pager &&
			    extended.ref_count == 2 &&
			    extended.share_mode == SM_SHARED) {
				submap_info->share_mode = SM_PRIVATE;
			}
			submap_info->ref_count = extended.ref_count;
		} else {
			if (curr_entry->use_pmap) {
				submap_info->share_mode = SM_TRUESHARED;
			} else {
				submap_info->share_mode = SM_PRIVATE;
			}
			submap_info->ref_count =
				curr_entry->object.sub_map->ref_count;
		}
	}

	submap_info->pages_resident = extended.pages_resident;
	submap_info->pages_swapped_out = extended.pages_swapped_out;
	submap_info->pages_shared_now_private =
		extended.pages_shared_now_private;
	submap_info->pages_dirtied = extended.pages_dirtied;
	submap_info->external_pager = extended.external_pager;
	submap_info->shadow_depth = extended.shadow_depth;

	if (not_in_kdp) {
		vm_map_unlock_read(curr_map);
	}

	return KERN_SUCCESS;
}

/*
 *	vm_region:
 *
 *	User call to obtain information about a region in
 *	a task's address map. Currently, only one flavor is
 *	supported.
 *
 *	XXX The reserved and behavior fields cannot be filled
 *	    in until the vm merge from the IK is completed, and
 *	    vm_reserve is implemented.
 */

kern_return_t
vm_map_region(
	vm_map_t		 map,
	vm_map_offset_t	*address,		/* IN/OUT */
	vm_map_size_t		*size,			/* OUT */
	vm_region_flavor_t	 flavor,		/* IN */
	vm_region_info_t	 info,			/* OUT */
	mach_msg_type_number_t	*count,	/* IN/OUT */
	mach_port_t		*object_name)		/* OUT */
{
	vm_map_entry_t		tmp_entry;
	vm_map_entry_t		entry;
	vm_map_offset_t		start;

	if (map == VM_MAP_NULL) 
		return(KERN_INVALID_ARGUMENT);

	switch (flavor) {

	case VM_REGION_BASIC_INFO:
	/* legacy for old 32-bit objects info */
	{
	    vm_region_basic_info_t	basic;

	    if (*count < VM_REGION_BASIC_INFO_COUNT)
		return(KERN_INVALID_ARGUMENT);

	    basic = (vm_region_basic_info_t) info;
	    *count = VM_REGION_BASIC_INFO_COUNT;

	    vm_map_lock_read(map);

	    start = *address;
	    if (!vm_map_lookup_entry(map, start, &tmp_entry)) {
		if ((entry = tmp_entry->vme_next) == vm_map_to_entry(map)) {
			vm_map_unlock_read(map);
		   	return(KERN_INVALID_ADDRESS);
		}
	    } else {
		entry = tmp_entry;
	    }

	    start = entry->vme_start;

	    basic->offset = (uint32_t)entry->offset;
	    basic->protection = entry->protection;
	    basic->inheritance = entry->inheritance;
	    basic->max_protection = entry->max_protection;
	    basic->behavior = entry->behavior;
	    basic->user_wired_count = entry->user_wired_count;
	    basic->reserved = entry->is_sub_map;
	    *address = start;
	    *size = (entry->vme_end - start);

	    if (object_name) *object_name = IP_NULL;
	    if (entry->is_sub_map) {
	        basic->shared = FALSE;
	    } else {
	        basic->shared = entry->is_shared;
	    }

	    vm_map_unlock_read(map);
	    return(KERN_SUCCESS);
	}

	case VM_REGION_BASIC_INFO_64:
	{
	    vm_region_basic_info_64_t	basic;

	    if (*count < VM_REGION_BASIC_INFO_COUNT_64)
		return(KERN_INVALID_ARGUMENT);

	    basic = (vm_region_basic_info_64_t) info;
	    *count = VM_REGION_BASIC_INFO_COUNT_64;

	    vm_map_lock_read(map);

	    start = *address;
	    if (!vm_map_lookup_entry(map, start, &tmp_entry)) {
		if ((entry = tmp_entry->vme_next) == vm_map_to_entry(map)) {
			vm_map_unlock_read(map);
		   	return(KERN_INVALID_ADDRESS);
		}
	    } else {
		entry = tmp_entry;
	    }

	    start = entry->vme_start;

	    basic->offset = entry->offset;
	    basic->protection = entry->protection;
	    basic->inheritance = entry->inheritance;
	    basic->max_protection = entry->max_protection;
	    basic->behavior = entry->behavior;
	    basic->user_wired_count = entry->user_wired_count;
	    basic->reserved = entry->is_sub_map;
	    *address = start;
	    *size = (entry->vme_end - start);

	    if (object_name) *object_name = IP_NULL;
	    if (entry->is_sub_map) {
	        basic->shared = FALSE;
	    } else {
	        basic->shared = entry->is_shared;
	    }

	    vm_map_unlock_read(map);
	    return(KERN_SUCCESS);
	}
	case VM_REGION_EXTENDED_INFO:
	{
	    vm_region_extended_info_t	extended;

	    if (*count < VM_REGION_EXTENDED_INFO_COUNT)
		return(KERN_INVALID_ARGUMENT);

	    extended = (vm_region_extended_info_t) info;
	    *count = VM_REGION_EXTENDED_INFO_COUNT;

	    vm_map_lock_read(map);

	    start = *address;
	    if (!vm_map_lookup_entry(map, start, &tmp_entry)) {
		if ((entry = tmp_entry->vme_next) == vm_map_to_entry(map)) {
			vm_map_unlock_read(map);
		   	return(KERN_INVALID_ADDRESS);
		}
	    } else {
		entry = tmp_entry;
	    }
	    start = entry->vme_start;

	    extended->protection = entry->protection;
	    extended->user_tag = entry->alias;
	    extended->pages_resident = 0;
	    extended->pages_swapped_out = 0;
	    extended->pages_shared_now_private = 0;
	    extended->pages_dirtied = 0;
	    extended->external_pager = 0;
	    extended->shadow_depth = 0;

	    vm_map_region_walk(map, start, entry, entry->offset, entry->vme_end - start, extended);

	    if (extended->external_pager && extended->ref_count == 2 && extended->share_mode == SM_SHARED)
	            extended->share_mode = SM_PRIVATE;

	    if (object_name)
	        *object_name = IP_NULL;
	    *address = start;
	    *size = (entry->vme_end - start);

	    vm_map_unlock_read(map);
	    return(KERN_SUCCESS);
	}
	case VM_REGION_TOP_INFO:
	{   
	    vm_region_top_info_t	top;

	    if (*count < VM_REGION_TOP_INFO_COUNT)
		return(KERN_INVALID_ARGUMENT);

	    top = (vm_region_top_info_t) info;
	    *count = VM_REGION_TOP_INFO_COUNT;

	    vm_map_lock_read(map);

	    start = *address;
	    if (!vm_map_lookup_entry(map, start, &tmp_entry)) {
		if ((entry = tmp_entry->vme_next) == vm_map_to_entry(map)) {
			vm_map_unlock_read(map);
		   	return(KERN_INVALID_ADDRESS);
		}
	    } else {
		entry = tmp_entry;

	    }
	    start = entry->vme_start;

	    top->private_pages_resident = 0;
	    top->shared_pages_resident = 0;

	    vm_map_region_top_walk(entry, top);

	    if (object_name)
	        *object_name = IP_NULL;
	    *address = start;
	    *size = (entry->vme_end - start);

	    vm_map_unlock_read(map);
	    return(KERN_SUCCESS);
	}
	default:
	    return(KERN_INVALID_ARGUMENT);
	}
}

static void
vm_map_region_top_walk(
        vm_map_entry_t		   entry,
	vm_region_top_info_t       top)
{
        register struct vm_object *obj, *tmp_obj;
	register int    ref_count;

	if (entry->object.vm_object == 0 || entry->is_sub_map) {
	    top->share_mode = SM_EMPTY;
	    top->ref_count = 0;
	    top->obj_id = 0;
	    return;
	}
	{
	    obj = entry->object.vm_object;

	    vm_object_lock(obj);

	    if ((ref_count = obj->ref_count) > 1 && obj->paging_in_progress)
	        ref_count--;

	    if (obj->shadow) {
		if (ref_count == 1)
		    top->private_pages_resident = obj->resident_page_count;
		else
		    top->shared_pages_resident = obj->resident_page_count;
		top->ref_count  = ref_count;
	        top->share_mode = SM_COW;
	    
	        while ((tmp_obj = obj->shadow)) {
		    vm_object_lock(tmp_obj);
		    vm_object_unlock(obj);
		    obj = tmp_obj;

		    if ((ref_count = obj->ref_count) > 1 && obj->paging_in_progress)
		        ref_count--;

		    top->shared_pages_resident += obj->resident_page_count;
		    top->ref_count += ref_count - 1;
		}
	    } else {
	        if (entry->needs_copy) {
		    top->share_mode = SM_COW;
		    top->shared_pages_resident = obj->resident_page_count;
		} else {
		    if (ref_count == 1 ||
		       (ref_count == 2 && !(obj->pager_trusted) && !(obj->internal))) {
		        top->share_mode = SM_PRIVATE;
			top->private_pages_resident = obj->resident_page_count;
		    } else {
		        top->share_mode = SM_SHARED;
			top->shared_pages_resident = obj->resident_page_count;
		    }
		}
		top->ref_count = ref_count;
	    }
	    top->obj_id = (int)obj;

	    vm_object_unlock(obj);
	}
}

static void
vm_map_region_walk(
	vm_map_t		   	map,
	vm_map_offset_t		va,
	vm_map_entry_t		entry,
	vm_object_offset_t		offset,
	vm_object_size_t		range,
	vm_region_extended_info_t	extended)
{
        register struct vm_object *obj, *tmp_obj;
	register vm_map_offset_t       last_offset;
	register int               i;
	register int               ref_count;
	struct vm_object	*shadow_object;
	int			shadow_depth;

	if ((entry->object.vm_object == 0) ||
	        (entry->is_sub_map) ||
		(entry->object.vm_object->phys_contiguous)) {
	    extended->share_mode = SM_EMPTY;
	    extended->ref_count = 0;
	    return;
	}
	{
	    obj = entry->object.vm_object;

	    vm_object_lock(obj);

	    if ((ref_count = obj->ref_count) > 1 && obj->paging_in_progress)
	        ref_count--;

	    for (last_offset = offset + range; offset < last_offset; offset += PAGE_SIZE_64, va += PAGE_SIZE)
		    vm_map_region_look_for_page(map, va, obj, offset, ref_count, 0, extended);

	    shadow_object = obj->shadow;
	    shadow_depth = 0;
	    if (shadow_object != VM_OBJECT_NULL) {
		    vm_object_lock(shadow_object);
		    for (;
			 shadow_object != VM_OBJECT_NULL;
			 shadow_depth++) {
			    vm_object_t	next_shadow;

			    next_shadow = shadow_object->shadow;
			    if (next_shadow) {
				    vm_object_lock(next_shadow);
			    }
			    vm_object_unlock(shadow_object);
			    shadow_object = next_shadow;
		    }
	    }
	    extended->shadow_depth = shadow_depth;

	    if (extended->shadow_depth || entry->needs_copy)
	        extended->share_mode = SM_COW;
	    else {
	        if (ref_count == 1)
		    extended->share_mode = SM_PRIVATE;
		else {
	            if (obj->true_share)
		        extended->share_mode = SM_TRUESHARED;
		    else
		        extended->share_mode = SM_SHARED;
		}
	    }
	    extended->ref_count = ref_count - extended->shadow_depth;
	    
	    for (i = 0; i < extended->shadow_depth; i++) {
	        if ((tmp_obj = obj->shadow) == 0)
		    break;
		vm_object_lock(tmp_obj);
		vm_object_unlock(obj);

		if ((ref_count = tmp_obj->ref_count) > 1 && tmp_obj->paging_in_progress)
		    ref_count--;

		extended->ref_count += ref_count;
		obj = tmp_obj;
	    }
	    vm_object_unlock(obj);

	    if (extended->share_mode == SM_SHARED) {
	        register vm_map_entry_t	     cur;
	        register vm_map_entry_t	     last;
		int      my_refs;

	        obj = entry->object.vm_object;
		last = vm_map_to_entry(map);
		my_refs = 0;

		if ((ref_count = obj->ref_count) > 1 && obj->paging_in_progress)
		        ref_count--;
		for (cur = vm_map_first_entry(map); cur != last; cur = cur->vme_next)
		    my_refs += vm_map_region_count_obj_refs(cur, obj);

		if (my_refs == ref_count)
		    extended->share_mode = SM_PRIVATE_ALIASED;
		else if (my_refs > 1)
		    extended->share_mode = SM_SHARED_ALIASED;
	    }
	}
}


/* object is locked on entry and locked on return */


static void
vm_map_region_look_for_page(
	__unused vm_map_t		map,
	__unused vm_map_offset_t    	va,
	vm_object_t		   	object,
	vm_object_offset_t   	offset,
	int				max_refcnt,
	int				depth,
	vm_region_extended_info_t	extended)
{
        register vm_page_t	   p;
        register vm_object_t	   shadow;
	register int               ref_count;
	vm_object_t		   caller_object;
        
	shadow = object->shadow;
	caller_object = object;

	
	while (TRUE) {

		if ( !(object->pager_trusted) && !(object->internal))
			    extended->external_pager = 1;

		if ((p = vm_page_lookup(object, offset)) != VM_PAGE_NULL) {
	        	if (shadow && (max_refcnt == 1))
		    		extended->pages_shared_now_private++;

			if (!p->fictitious && 
			    (p->dirty || pmap_is_modified(p->phys_page)))
		    		extended->pages_dirtied++;

	        	extended->pages_resident++;

			if(object != caller_object)
			     vm_object_unlock(object);

			return;
		}
		if (object->existence_map) {
	    		if (vm_external_state_get(object->existence_map, offset) == VM_EXTERNAL_STATE_EXISTS) {

	        		extended->pages_swapped_out++;

				if(object != caller_object)
				     vm_object_unlock(object);

				return;
	    		}
		}
		if (shadow) {
		    vm_object_lock(shadow);

			if ((ref_count = shadow->ref_count) > 1 && shadow->paging_in_progress)
			        ref_count--;

	    		if (++depth > extended->shadow_depth)
	        		extended->shadow_depth = depth;

	    		if (ref_count > max_refcnt)
	        		max_refcnt = ref_count;
			
			if(object != caller_object)
			     vm_object_unlock(object);

			offset = offset + object->shadow_offset;
			object = shadow;
			shadow = object->shadow;
			continue;
		}
		if(object != caller_object)
		     vm_object_unlock(object);
		break;
	}
}

static int
vm_map_region_count_obj_refs(
        vm_map_entry_t    entry,
	vm_object_t       object)
{
        register int ref_count;
	register vm_object_t chk_obj;
	register vm_object_t tmp_obj;

	if (entry->object.vm_object == 0)
	    return(0);

        if (entry->is_sub_map)
	    return(0);
	else {
	    ref_count = 0;

	    chk_obj = entry->object.vm_object;
	    vm_object_lock(chk_obj);

	    while (chk_obj) {
	        if (chk_obj == object)
		    ref_count++;
		tmp_obj = chk_obj->shadow;
		if (tmp_obj)
		   vm_object_lock(tmp_obj);
		vm_object_unlock(chk_obj);

		chk_obj = tmp_obj;
	    }
	}
	return(ref_count);
}


/*
 *	Routine:	vm_map_simplify
 *
 *	Description:
 *		Attempt to simplify the map representation in
 *		the vicinity of the given starting address.
 *	Note:
 *		This routine is intended primarily to keep the
 *		kernel maps more compact -- they generally don't
 *		benefit from the "expand a map entry" technology
 *		at allocation time because the adjacent entry
 *		is often wired down.
 */
void
vm_map_simplify_entry(
	vm_map_t	map,
	vm_map_entry_t	this_entry)
{
	vm_map_entry_t	prev_entry;

	counter(c_vm_map_simplify_entry_called++);

	prev_entry = this_entry->vme_prev;

	if ((this_entry != vm_map_to_entry(map)) &&
	     (prev_entry != vm_map_to_entry(map)) &&

	    (prev_entry->vme_end == this_entry->vme_start) &&

	    (prev_entry->is_sub_map == FALSE) &&
	    (this_entry->is_sub_map == FALSE) &&

	    (prev_entry->object.vm_object == this_entry->object.vm_object) &&
	    ((prev_entry->offset + (prev_entry->vme_end -
				    prev_entry->vme_start))
	     == this_entry->offset) &&

	    (prev_entry->inheritance == this_entry->inheritance) &&
	    (prev_entry->protection == this_entry->protection) &&
	    (prev_entry->max_protection == this_entry->max_protection) &&
	    (prev_entry->behavior == this_entry->behavior) &&
	    (prev_entry->alias == this_entry->alias) &&
	    (prev_entry->wired_count == this_entry->wired_count) &&
	    (prev_entry->user_wired_count == this_entry->user_wired_count) &&

	    (prev_entry->needs_copy == this_entry->needs_copy) &&

	    (prev_entry->use_pmap == FALSE) &&
	    (this_entry->use_pmap == FALSE) &&
	    (prev_entry->in_transition == FALSE) &&
	    (this_entry->in_transition == FALSE) &&
	    (prev_entry->needs_wakeup == FALSE) &&
	    (this_entry->needs_wakeup == FALSE) &&
	    (prev_entry->is_shared == FALSE) &&
	    (this_entry->is_shared == FALSE)
		    ) {
		_vm_map_entry_unlink(&map->hdr, prev_entry);
		this_entry->vme_start = prev_entry->vme_start;
		this_entry->offset = prev_entry->offset;
	 	vm_object_deallocate(prev_entry->object.vm_object);
		vm_map_entry_dispose(map, prev_entry);
		SAVE_HINT(map, this_entry);
		counter(c_vm_map_simplified++);
	}
}

void
vm_map_simplify(
	vm_map_t	map,
	vm_map_offset_t	start)
{
	vm_map_entry_t	this_entry;

	vm_map_lock(map);
	if (vm_map_lookup_entry(map, start, &this_entry)) {
		vm_map_simplify_entry(map, this_entry);
		vm_map_simplify_entry(map, this_entry->vme_next);
	}
	counter(c_vm_map_simplify_called++);
	vm_map_unlock(map);
}

static void
vm_map_simplify_range(
	vm_map_t	map,
	vm_map_offset_t	start,
	vm_map_offset_t	end)
{
	vm_map_entry_t	entry;

	/*
	 * The map should be locked (for "write") by the caller.
	 */

	if (start >= end) {
		/* invalid address range */
		return;
	}

	if (!vm_map_lookup_entry(map, start, &entry)) {
		/* "start" is not mapped and "entry" ends before "start" */
		if (entry == vm_map_to_entry(map)) {
			/* start with first entry in the map */
			entry = vm_map_first_entry(map);
		} else {
			/* start with next entry */
			entry = entry->vme_next;
		}
	}
		
	while (entry != vm_map_to_entry(map) &&
	       entry->vme_start <= end) {
		/* try and coalesce "entry" with its previous entry */
		vm_map_simplify_entry(map, entry);
		entry = entry->vme_next;
	}
}


/*
 *	Routine:	vm_map_machine_attribute
 *	Purpose:
 *		Provide machine-specific attributes to mappings,
 *		such as cachability etc. for machines that provide
 *		them.  NUMA architectures and machines with big/strange
 *		caches will use this.
 *	Note:
 *		Responsibilities for locking and checking are handled here,
 *		everything else in the pmap module. If any non-volatile
 *		information must be kept, the pmap module should handle
 *		it itself. [This assumes that attributes do not
 *		need to be inherited, which seems ok to me]
 */
kern_return_t
vm_map_machine_attribute(
	vm_map_t			map,
	vm_map_offset_t		start,
	vm_map_offset_t		end,
	vm_machine_attribute_t	attribute,
	vm_machine_attribute_val_t* value)		/* IN/OUT */
{
	kern_return_t	ret;
	vm_map_size_t sync_size;
	vm_map_entry_t entry;
	
	if (start < vm_map_min(map) || end > vm_map_max(map))
		return KERN_INVALID_ADDRESS;

	/* Figure how much memory we need to flush (in page increments) */
	sync_size = end - start;

	vm_map_lock(map);
	
	if (attribute != MATTR_CACHE) {	
		/* If we don't have to find physical addresses, we */
		/* don't have to do an explicit traversal here.    */
		ret = pmap_attribute(map->pmap, start, end-start,
				     attribute, value);
		vm_map_unlock(map);
		return ret;
	}

	ret = KERN_SUCCESS;										/* Assume it all worked */

	while(sync_size) {
		if (vm_map_lookup_entry(map, start, &entry)) {
			vm_map_size_t	sub_size;
			if((entry->vme_end - start) > sync_size) {
				sub_size = sync_size;
				sync_size = 0;
			} else {
				sub_size = entry->vme_end - start;
					sync_size -= sub_size;
			}
			if(entry->is_sub_map) {
				vm_map_offset_t sub_start;
				vm_map_offset_t sub_end;

				sub_start = (start - entry->vme_start) 
					    + entry->offset;
				sub_end = sub_start + sub_size;
				vm_map_machine_attribute(
					entry->object.sub_map, 
					sub_start,
					sub_end,
					attribute, value);
			} else {
				if(entry->object.vm_object) {
					vm_page_t		m;
					vm_object_t		object;
					vm_object_t		base_object;
					vm_object_t		last_object;
					vm_object_offset_t	offset;
					vm_object_offset_t	base_offset;
					vm_map_size_t		range;
					range = sub_size;
					offset = (start - entry->vme_start)
							+ entry->offset;
					base_offset = offset;
					object = entry->object.vm_object;
					base_object = object;
					last_object = NULL;

					vm_object_lock(object);

					while (range) {
						m = vm_page_lookup(
							object, offset);

						if (m && !m->fictitious) {
						        ret = 
							    pmap_attribute_cache_sync(
										      m->phys_page, 	
										      PAGE_SIZE, 
										      attribute, value);
							
						} else if (object->shadow) {
						        offset = offset + object->shadow_offset;
							last_object = object;
							object = object->shadow;
							vm_object_lock(last_object->shadow);
							vm_object_unlock(last_object);
							continue;
						}
						range -= PAGE_SIZE;

						if (base_object != object) {
						        vm_object_unlock(object);
							vm_object_lock(base_object);
							object = base_object;
						}
						/* Bump to the next page */
						base_offset += PAGE_SIZE;
						offset = base_offset;
					}
					vm_object_unlock(object);
				}
			}
			start += sub_size;
		} else {
			vm_map_unlock(map);
			return KERN_FAILURE;
		}
		
	}

	vm_map_unlock(map);

	return ret;
}

/*
 *	vm_map_behavior_set:
 *
 *	Sets the paging reference behavior of the specified address
 *	range in the target map.  Paging reference behavior affects
 *	how pagein operations resulting from faults on the map will be 
 *	clustered.
 */
kern_return_t 
vm_map_behavior_set(
	vm_map_t	map,
	vm_map_offset_t	start,
	vm_map_offset_t	end,
	vm_behavior_t	new_behavior)
{
	register vm_map_entry_t	entry;
	vm_map_entry_t	temp_entry;

	XPR(XPR_VM_MAP,
		"vm_map_behavior_set, 0x%X start 0x%X end 0x%X behavior %d",
		(integer_t)map, start, end, new_behavior, 0);

	switch (new_behavior) {
	case VM_BEHAVIOR_DEFAULT:
	case VM_BEHAVIOR_RANDOM:
	case VM_BEHAVIOR_SEQUENTIAL:
	case VM_BEHAVIOR_RSEQNTL:
		break;
	case VM_BEHAVIOR_WILLNEED:
	case VM_BEHAVIOR_DONTNEED:
		new_behavior = VM_BEHAVIOR_DEFAULT;
		break;
	default:
		return(KERN_INVALID_ARGUMENT);
	}

	vm_map_lock(map);

	/*
	 *	The entire address range must be valid for the map.
	 * 	Note that vm_map_range_check() does a 
	 *	vm_map_lookup_entry() internally and returns the
	 *	entry containing the start of the address range if
	 *	the entire range is valid.
	 */
	if (vm_map_range_check(map, start, end, &temp_entry)) {
		entry = temp_entry;
		vm_map_clip_start(map, entry, start);
	}
	else {
		vm_map_unlock(map);
		return(KERN_INVALID_ADDRESS);
	}

	while ((entry != vm_map_to_entry(map)) && (entry->vme_start < end)) {
		vm_map_clip_end(map, entry, end);

		entry->behavior = new_behavior;

		entry = entry->vme_next;
	}

	vm_map_unlock(map);
	return(KERN_SUCCESS);
}


#include <mach_kdb.h>
#if	MACH_KDB
#include <ddb/db_output.h>
#include <vm/vm_print.h>

#define	printf	db_printf

/*
 * Forward declarations for internal functions.
 */
extern void vm_map_links_print(
		struct vm_map_links	*links);

extern void vm_map_header_print(
		struct vm_map_header	*header);

extern void vm_map_entry_print(
		vm_map_entry_t		entry);

extern void vm_follow_entry(
		vm_map_entry_t		entry);

extern void vm_follow_map(
		vm_map_t		map);

/*
 *	vm_map_links_print:	[ debug ]
 */
void
vm_map_links_print(
	struct vm_map_links	*links)
{
	iprintf("prev = %08X  next = %08X  start = %016llX  end = %016llX\n",
		links->prev,
		links->next,
		(unsigned long long)links->start,
		(unsigned long long)links->end);
}

/*
 *	vm_map_header_print:	[ debug ]
 */
void
vm_map_header_print(
	struct vm_map_header	*header)
{
	vm_map_links_print(&header->links);
	iprintf("nentries = %08X, %sentries_pageable\n",
		header->nentries,
		(header->entries_pageable ? "" : "!"));
}

/*
 *	vm_follow_entry:	[ debug ]
 */
void
vm_follow_entry(
	vm_map_entry_t entry)
{
	int shadows;

	iprintf("map entry %08X\n", entry);

	db_indent += 2;

	shadows = vm_follow_object(entry->object.vm_object);
	iprintf("Total objects : %d\n",shadows);

	db_indent -= 2;
}

/*
 *	vm_map_entry_print:	[ debug ]
 */
void
vm_map_entry_print(
	register vm_map_entry_t	entry)
{
	static const char *inheritance_name[4] =
		{ "share", "copy", "none", "?"};
	static const char *behavior_name[4] =
		{ "dflt", "rand", "seqtl", "rseqntl" };
	
	iprintf("map entry %08X - prev = %08X  next = %08X\n", entry, entry->vme_prev, entry->vme_next);

	db_indent += 2;

	vm_map_links_print(&entry->links);

	iprintf("start = %016llX  end = %016llX - prot=%x/%x/%s\n",
		(unsigned long long)entry->vme_start,
		(unsigned long long)entry->vme_end,
		entry->protection,
		entry->max_protection,
		inheritance_name[(entry->inheritance & 0x3)]);

	iprintf("behavior = %s, wired_count = %d, user_wired_count = %d\n",
		behavior_name[(entry->behavior & 0x3)],
		entry->wired_count,
		entry->user_wired_count);
	iprintf("%sin_transition, %sneeds_wakeup\n",
		(entry->in_transition ? "" : "!"),
		(entry->needs_wakeup ? "" : "!"));

	if (entry->is_sub_map) {
		iprintf("submap = %08X - offset = %016llX\n",
		       entry->object.sub_map,
		       (unsigned long long)entry->offset);
	} else {
		iprintf("object = %08X  offset = %016llX - ",
			entry->object.vm_object,
			(unsigned long long)entry->offset);
		printf("%sis_shared, %sneeds_copy\n",
		       (entry->is_shared ? "" : "!"),
		       (entry->needs_copy ? "" : "!"));
	}

	db_indent -= 2;
}

/*
 *	vm_follow_map:	[ debug ]
 */
void
vm_follow_map(
	vm_map_t map)
{
	register vm_map_entry_t	entry;

	iprintf("task map %08X\n", map);

	db_indent += 2;

	for (entry = vm_map_first_entry(map);
	     entry && entry != vm_map_to_entry(map);
	     entry = entry->vme_next) {
	    vm_follow_entry(entry);
	}

	db_indent -= 2;
}

/*
 *	vm_map_print:	[ debug ]
 */
void
vm_map_print(
	db_addr_t inmap)
{
	register vm_map_entry_t	entry;
	vm_map_t map;
#if TASK_SWAPPER
	char *swstate;
#endif /* TASK_SWAPPER */

	map = (vm_map_t)(long)
		inmap;	/* Make sure we have the right type */

	iprintf("task map %08X\n", map);

	db_indent += 2;

	vm_map_header_print(&map->hdr);

	iprintf("pmap = %08X  size = %08X  ref = %d  hint = %08X  first_free = %08X\n",
		map->pmap,
		map->size,
		map->ref_count,
		map->hint,
		map->first_free);

	iprintf("%swait_for_space, %swiring_required, timestamp = %d\n",
		(map->wait_for_space ? "" : "!"),
		(map->wiring_required ? "" : "!"),
		map->timestamp);

#if	TASK_SWAPPER
	switch (map->sw_state) {
	    case MAP_SW_IN:
		swstate = "SW_IN";
		break;
	    case MAP_SW_OUT:
		swstate = "SW_OUT";
		break;
	    default:
		swstate = "????";
		break;
	}
	iprintf("res = %d, sw_state = %s\n", map->res_count, swstate);
#endif	/* TASK_SWAPPER */

	for (entry = vm_map_first_entry(map);
	     entry && entry != vm_map_to_entry(map);
	     entry = entry->vme_next) {
		vm_map_entry_print(entry);
	}

	db_indent -= 2;
}

/*
 *	Routine:	vm_map_copy_print
 *	Purpose:
 *		Pretty-print a copy object for ddb.
 */

void
vm_map_copy_print(
	db_addr_t	incopy)
{
	vm_map_copy_t copy;
	vm_map_entry_t entry;

	copy = (vm_map_copy_t)(long)
		incopy;	/* Make sure we have the right type */

	printf("copy object 0x%x\n", copy);

	db_indent += 2;

	iprintf("type=%d", copy->type);
	switch (copy->type) {
		case VM_MAP_COPY_ENTRY_LIST:
		printf("[entry_list]");
		break;
		
		case VM_MAP_COPY_OBJECT:
		printf("[object]");
		break;
		
		case VM_MAP_COPY_KERNEL_BUFFER:
		printf("[kernel_buffer]");
		break;

		default:
		printf("[bad type]");
		break;
	}
	printf(", offset=0x%llx", (unsigned long long)copy->offset);
	printf(", size=0x%x\n", copy->size);

	switch (copy->type) {
		case VM_MAP_COPY_ENTRY_LIST:
		vm_map_header_print(&copy->cpy_hdr);
		for (entry = vm_map_copy_first_entry(copy);
		     entry && entry != vm_map_copy_to_entry(copy);
		     entry = entry->vme_next) {
			vm_map_entry_print(entry);
		}
		break;

		case VM_MAP_COPY_OBJECT:
		iprintf("object=0x%x\n", copy->cpy_object);
		break;

		case VM_MAP_COPY_KERNEL_BUFFER:
		iprintf("kernel buffer=0x%x", copy->cpy_kdata);
		printf(", kalloc_size=0x%x\n", copy->cpy_kalloc_size);
		break;

	}

	db_indent -=2;
}

/*
 *	db_vm_map_total_size(map)	[ debug ]
 *
 *	return the total virtual size (in bytes) of the map
 */
vm_map_size_t
db_vm_map_total_size(
	db_addr_t	inmap)
{
	vm_map_entry_t	entry;
	vm_map_size_t	total;
	vm_map_t map;

	map = (vm_map_t)(long)
		inmap;	/* Make sure we have the right type */

	total = 0;
	for (entry = vm_map_first_entry(map);
	     entry != vm_map_to_entry(map);
	     entry = entry->vme_next) {
		total += entry->vme_end - entry->vme_start;
	}

	return total;
}

#endif	/* MACH_KDB */

/*
 *	Routine:	vm_map_entry_insert
 *
 *	Descritpion:	This routine inserts a new vm_entry in a locked map.
 */
vm_map_entry_t
vm_map_entry_insert(
	vm_map_t		map,
	vm_map_entry_t		insp_entry,
	vm_map_offset_t		start,
	vm_map_offset_t		end,
	vm_object_t		object,
	vm_object_offset_t	offset,
	boolean_t		needs_copy,
	boolean_t		is_shared,
	boolean_t		in_transition,
	vm_prot_t		cur_protection,
	vm_prot_t		max_protection,
	vm_behavior_t		behavior,
	vm_inherit_t		inheritance,
	unsigned		wired_count)
{
	vm_map_entry_t	new_entry;

	assert(insp_entry != (vm_map_entry_t)0);

	new_entry = vm_map_entry_create(map);

	new_entry->vme_start = start;
	new_entry->vme_end = end;
	assert(page_aligned(new_entry->vme_start));
	assert(page_aligned(new_entry->vme_end));

	new_entry->object.vm_object = object;
	new_entry->offset = offset;
	new_entry->is_shared = is_shared;
	new_entry->is_sub_map = FALSE;
	new_entry->needs_copy = needs_copy;
	new_entry->in_transition = in_transition;
	new_entry->needs_wakeup = FALSE;
	new_entry->inheritance = inheritance;
	new_entry->protection = cur_protection;
	new_entry->max_protection = max_protection;
	new_entry->behavior = behavior;
	new_entry->wired_count = wired_count;
	new_entry->user_wired_count = 0;
	new_entry->use_pmap = FALSE;

	/*
	 *	Insert the new entry into the list.
	 */

	vm_map_entry_link(map, insp_entry, new_entry);
	map->size += end - start;

	/*
	 *	Update the free space hint and the lookup hint.
	 */

	SAVE_HINT(map, new_entry);
	return new_entry;
}

/*
 *	Routine:	vm_map_remap_extract
 *
 *	Descritpion:	This routine returns a vm_entry list from a map.
 */
static kern_return_t
vm_map_remap_extract(
	vm_map_t		map,
	vm_map_offset_t		addr,
	vm_map_size_t		size,
	boolean_t		copy,
	struct vm_map_header	*map_header,
	vm_prot_t		*cur_protection,
	vm_prot_t		*max_protection,
	/* What, no behavior? */
	vm_inherit_t		inheritance,
	boolean_t		pageable)
{
	kern_return_t		result;
	vm_map_size_t		mapped_size;
	vm_map_size_t		tmp_size;
	vm_map_entry_t		src_entry;     /* result of last map lookup */
	vm_map_entry_t		new_entry;
	vm_object_offset_t	offset;
	vm_map_offset_t		map_address;
	vm_map_offset_t		src_start;     /* start of entry to map */
	vm_map_offset_t		src_end;       /* end of region to be mapped */
	vm_object_t		object;    
	vm_map_version_t	version;
	boolean_t		src_needs_copy;
	boolean_t		new_entry_needs_copy;

	assert(map != VM_MAP_NULL);
	assert(size != 0 && size == vm_map_round_page(size));
	assert(inheritance == VM_INHERIT_NONE ||
	       inheritance == VM_INHERIT_COPY ||
	       inheritance == VM_INHERIT_SHARE);

	/*
	 *	Compute start and end of region.
	 */
	src_start = vm_map_trunc_page(addr);
	src_end = vm_map_round_page(src_start + size);

	/*
	 *	Initialize map_header.
	 */
	map_header->links.next = (struct vm_map_entry *)&map_header->links;
	map_header->links.prev = (struct vm_map_entry *)&map_header->links;
	map_header->nentries = 0;
	map_header->entries_pageable = pageable;

	*cur_protection = VM_PROT_ALL;
	*max_protection = VM_PROT_ALL;

	map_address = 0;
	mapped_size = 0;
	result = KERN_SUCCESS;

	/*  
	 *	The specified source virtual space might correspond to
	 *	multiple map entries, need to loop on them.
	 */
	vm_map_lock(map);
	while (mapped_size != size) {
		vm_map_size_t	entry_size;

		/*
		 *	Find the beginning of the region.
		 */ 
		if (! vm_map_lookup_entry(map, src_start, &src_entry)) {
			result = KERN_INVALID_ADDRESS;
			break;
		}

		if (src_start < src_entry->vme_start ||
		    (mapped_size && src_start != src_entry->vme_start)) {
			result = KERN_INVALID_ADDRESS;
			break;
		}

		if(src_entry->is_sub_map) {
			result = KERN_INVALID_ADDRESS;
			break;
		}

		tmp_size = size - mapped_size;
		if (src_end > src_entry->vme_end)
			tmp_size -= (src_end - src_entry->vme_end);

		entry_size = (vm_map_size_t)(src_entry->vme_end -
					 src_entry->vme_start);

		if(src_entry->is_sub_map) {
			vm_map_reference(src_entry->object.sub_map);
			object = VM_OBJECT_NULL;
		} else {
			object = src_entry->object.vm_object;

			if (object == VM_OBJECT_NULL) {
				object = vm_object_allocate(entry_size);
				src_entry->offset = 0;
				src_entry->object.vm_object = object;
			} else if (object->copy_strategy !=
				   MEMORY_OBJECT_COPY_SYMMETRIC) {
				/*
				 *	We are already using an asymmetric
				 *	copy, and therefore we already have
				 *	the right object.
				 */
				assert(!src_entry->needs_copy);
			} else if (src_entry->needs_copy || object->shadowed ||
				   (object->internal && !object->true_share &&
				   !src_entry->is_shared &&
				    object->size > entry_size)) {

				vm_object_shadow(&src_entry->object.vm_object,
						 &src_entry->offset,
						 entry_size);

				if (!src_entry->needs_copy &&
				    (src_entry->protection & VM_PROT_WRITE)) {
					if(map->mapped) {
					   vm_object_pmap_protect(
						src_entry->object.vm_object,
						src_entry->offset,
						entry_size,
						PMAP_NULL,
						src_entry->vme_start,
						src_entry->protection &
						       ~VM_PROT_WRITE);
					} else {
					   pmap_protect(vm_map_pmap(map),
					  	 src_entry->vme_start,
					     	src_entry->vme_end,
					     	src_entry->protection &
						     ~VM_PROT_WRITE);
					}
				}

				object = src_entry->object.vm_object;
				src_entry->needs_copy = FALSE;
			}


			vm_object_lock(object);
			object->ref_count++;	/* object ref. for new entry */
			VM_OBJ_RES_INCR(object);
			if (object->copy_strategy == 
					MEMORY_OBJECT_COPY_SYMMETRIC) {
				object->copy_strategy = 
					MEMORY_OBJECT_COPY_DELAY;
			}
			vm_object_unlock(object);
		}

		offset = src_entry->offset + (src_start - src_entry->vme_start);

		new_entry = _vm_map_entry_create(map_header);
		vm_map_entry_copy(new_entry, src_entry);
		new_entry->use_pmap = FALSE; /* clr address space specifics */

		new_entry->vme_start = map_address;
		new_entry->vme_end = map_address + tmp_size;
		new_entry->inheritance = inheritance;
		new_entry->offset = offset;

		/*
		 * The new region has to be copied now if required.
		 */
	RestartCopy:
		if (!copy) {
			src_entry->is_shared = TRUE;
			new_entry->is_shared = TRUE;
			if (!(new_entry->is_sub_map)) 
				new_entry->needs_copy = FALSE;

		} else if (src_entry->is_sub_map) {
			/* make this a COW sub_map if not already */
			new_entry->needs_copy = TRUE;
			object = VM_OBJECT_NULL;
		} else if (src_entry->wired_count == 0 &&
			 vm_object_copy_quickly(&new_entry->object.vm_object,
						new_entry->offset,
						(new_entry->vme_end -
						    new_entry->vme_start),
						&src_needs_copy,
						&new_entry_needs_copy)) {

			new_entry->needs_copy = new_entry_needs_copy;
			new_entry->is_shared = FALSE;

			/*
			 * Handle copy_on_write semantics.
			 */
			if (src_needs_copy && !src_entry->needs_copy) {
				vm_object_pmap_protect(object,
						       offset,
						       entry_size,
						       ((src_entry->is_shared 
							  || map->mapped) ?
							PMAP_NULL : map->pmap),
						       src_entry->vme_start,
						       src_entry->protection &
						       ~VM_PROT_WRITE);

				src_entry->needs_copy = TRUE;
			}
			/*
			 * Throw away the old object reference of the new entry.
			 */
			vm_object_deallocate(object);

		} else {
			new_entry->is_shared = FALSE;

			/*
			 * The map can be safely unlocked since we
			 * already hold a reference on the object.
			 *
			 * Record the timestamp of the map for later
			 * verification, and unlock the map.
			 */
			version.main_timestamp = map->timestamp;
			vm_map_unlock(map); 	/* Increments timestamp once! */

			/*
			 * Perform the copy.
			 */
			if (src_entry->wired_count > 0) {
				vm_object_lock(object);
				result = vm_object_copy_slowly(
						object,
						offset,
						entry_size,
						THREAD_UNINT,
						&new_entry->object.vm_object);

				new_entry->offset = 0;
				new_entry->needs_copy = FALSE;
			} else {
				result = vm_object_copy_strategically(
						object,
						offset,
						entry_size,
						&new_entry->object.vm_object,
						&new_entry->offset,
						&new_entry_needs_copy);

				new_entry->needs_copy = new_entry_needs_copy;
			}

			/*
			 * Throw away the old object reference of the new entry.
			 */
			vm_object_deallocate(object);

			if (result != KERN_SUCCESS &&
			    result != KERN_MEMORY_RESTART_COPY) {
				_vm_map_entry_dispose(map_header, new_entry);
				break;
			}

			/*
			 * Verify that the map has not substantially
			 * changed while the copy was being made.
			 */

			vm_map_lock(map);
			if (version.main_timestamp + 1 != map->timestamp) {
				/*
				 * Simple version comparison failed.
				 *
				 * Retry the lookup and verify that the
				 * same object/offset are still present.
				 */
				vm_object_deallocate(new_entry->
						     object.vm_object);
				_vm_map_entry_dispose(map_header, new_entry);
				if (result == KERN_MEMORY_RESTART_COPY)
					result = KERN_SUCCESS;
				continue;
			}

			if (result == KERN_MEMORY_RESTART_COPY) {
				vm_object_reference(object);
				goto RestartCopy;
			}
		}

		_vm_map_entry_link(map_header,
				   map_header->links.prev, new_entry);

		*cur_protection &= src_entry->protection;
		*max_protection &= src_entry->max_protection;

		map_address += tmp_size;
		mapped_size += tmp_size;
		src_start += tmp_size;

	} /* end while */

	vm_map_unlock(map);
	if (result != KERN_SUCCESS) {
		/*
		 * Free all allocated elements.
		 */
		for (src_entry = map_header->links.next;
		     src_entry != (struct vm_map_entry *)&map_header->links;
		     src_entry = new_entry) {
			new_entry = src_entry->vme_next;
			_vm_map_entry_unlink(map_header, src_entry);
			vm_object_deallocate(src_entry->object.vm_object);
			_vm_map_entry_dispose(map_header, src_entry);
		}
	}
	return result;
}

/*
 *	Routine:	vm_remap
 *
 *			Map portion of a task's address space.
 *			Mapped region must not overlap more than
 *			one vm memory object. Protections and
 *			inheritance attributes remain the same
 *			as in the original task and are	out parameters.
 *			Source and Target task can be identical
 *			Other attributes are identical as for vm_map()
 */
kern_return_t
vm_map_remap(
	vm_map_t		target_map,
	vm_map_address_t	*address,
	vm_map_size_t		size,
	vm_map_offset_t		mask,
	boolean_t		anywhere,
	vm_map_t		src_map,
	vm_map_offset_t		memory_address,
	boolean_t		copy,
	vm_prot_t		*cur_protection,
	vm_prot_t		*max_protection,
	vm_inherit_t		inheritance)
{
	kern_return_t		result;
	vm_map_entry_t		entry;
	vm_map_entry_t		insp_entry;
	vm_map_entry_t		new_entry;
	struct vm_map_header	map_header;

	if (target_map == VM_MAP_NULL)
		return KERN_INVALID_ARGUMENT;

	switch (inheritance) {
	    case VM_INHERIT_NONE:
	    case VM_INHERIT_COPY:
	    case VM_INHERIT_SHARE:
		if (size != 0 && src_map != VM_MAP_NULL)
			break;
		/*FALL THRU*/
	    default:
		return KERN_INVALID_ARGUMENT;
	}

	size = vm_map_round_page(size);

	result = vm_map_remap_extract(src_map, memory_address,
				size, copy, &map_header,
				cur_protection,
				max_protection,
				inheritance,
				target_map->hdr.
				entries_pageable);

	if (result != KERN_SUCCESS) {
		return result;
	}

	/*
	 * Allocate/check a range of free virtual address
	 * space for the target
	 */
	*address = vm_map_trunc_page(*address);
	vm_map_lock(target_map);
	result = vm_map_remap_range_allocate(target_map, address, size,
					 mask, anywhere, &insp_entry);

	for (entry = map_header.links.next;
	     entry != (struct vm_map_entry *)&map_header.links;
	     entry = new_entry) {
		new_entry = entry->vme_next;
		_vm_map_entry_unlink(&map_header, entry);
		if (result == KERN_SUCCESS) {
			entry->vme_start += *address;
			entry->vme_end += *address;
			vm_map_entry_link(target_map, insp_entry, entry);
			insp_entry = entry;
		} else {
			if (!entry->is_sub_map) {
				vm_object_deallocate(entry->object.vm_object);
			} else {
				vm_map_deallocate(entry->object.sub_map);
			   }
			_vm_map_entry_dispose(&map_header, entry);
		}
	}

	if (result == KERN_SUCCESS) {
		target_map->size += size;
		SAVE_HINT(target_map, insp_entry);
	}
	vm_map_unlock(target_map);

	if (result == KERN_SUCCESS && target_map->wiring_required)
		result = vm_map_wire(target_map, *address,
				     *address + size, *cur_protection, TRUE);
	return result;
}

/*
 *	Routine:	vm_map_remap_range_allocate
 *
 *	Description:
 *		Allocate a range in the specified virtual address map.
 *		returns the address and the map entry just before the allocated
 *		range
 *
 *	Map must be locked.
 */

static kern_return_t
vm_map_remap_range_allocate(
	vm_map_t		map,
	vm_map_address_t	*address,	/* IN/OUT */
	vm_map_size_t		size,
	vm_map_offset_t		mask,
	boolean_t		anywhere,
	vm_map_entry_t		*map_entry)	/* OUT */
{
	register vm_map_entry_t	entry;
	register vm_map_offset_t	start;
	register vm_map_offset_t	end;

 StartAgain: ;

    start = *address;

    if (anywhere)
    {
	/*
	 *	Calculate the first possible address.
	 */

	if (start < map->min_offset)
	    start = map->min_offset;
	if (start > map->max_offset)
	    return(KERN_NO_SPACE);
		
	/*
	 *	Look for the first possible address;
	 *	if there's already something at this
	 *	address, we have to start after it.
	 */

	assert(first_free_is_valid(map));
	if (start == map->min_offset) {
	    if ((entry = map->first_free) != vm_map_to_entry(map))
		start = entry->vme_end;
	} else {
	    vm_map_entry_t	tmp_entry;
	    if (vm_map_lookup_entry(map, start, &tmp_entry))
		start = tmp_entry->vme_end;
	    entry = tmp_entry;
	}
		
	/*
	 *	In any case, the "entry" always precedes
	 *	the proposed new region throughout the
	 *	loop:
	 */

	while (TRUE) {
	    register vm_map_entry_t	next;

	    /*
	     *	Find the end of the proposed new region.
	     *	Be sure we didn't go beyond the end, or
	     *	wrap around the address.
	     */

	    end = ((start + mask) & ~mask);
	    if (end < start)
		    return(KERN_NO_SPACE);
	    start = end;
	    end += size;

	    if ((end > map->max_offset) || (end < start)) {
		if (map->wait_for_space) {
		    if (size <= (map->max_offset -
				 map->min_offset)) {
			assert_wait((event_t) map, THREAD_INTERRUPTIBLE);
			vm_map_unlock(map);
			thread_block(THREAD_CONTINUE_NULL);
			vm_map_lock(map);
			goto StartAgain;
		    }
		}
		
		return(KERN_NO_SPACE);
	    }

	    /*
	     *	If there are no more entries, we must win.
	     */

	    next = entry->vme_next;
	    if (next == vm_map_to_entry(map))
		break;

	    /*
	     *	If there is another entry, it must be
	     *	after the end of the potential new region.
	     */

	    if (next->vme_start >= end)
		break;

	    /*
	     *	Didn't fit -- move to the next entry.
	     */

	    entry = next;
	    start = entry->vme_end;
	}
	*address = start;
    } else {
	vm_map_entry_t		temp_entry;
	
	/*
	 *	Verify that:
	 *		the address doesn't itself violate
	 *		the mask requirement.
	 */

	if ((start & mask) != 0)
	    return(KERN_NO_SPACE);


	/*
	 *	...	the address is within bounds
	 */

	end = start + size;

	if ((start < map->min_offset) ||
	    (end > map->max_offset) ||
	    (start >= end)) {
	    return(KERN_INVALID_ADDRESS);
	}

	/*
	 *	...	the starting address isn't allocated
	 */

	if (vm_map_lookup_entry(map, start, &temp_entry))
	    return(KERN_NO_SPACE);

	entry = temp_entry;

	/*
	 *	...	the next region doesn't overlap the
	 *		end point.
	 */

	if ((entry->vme_next != vm_map_to_entry(map)) &&
	    (entry->vme_next->vme_start < end))
	    return(KERN_NO_SPACE);
    }
    *map_entry = entry;
    return(KERN_SUCCESS);
}

/*
 *	vm_map_switch:
 *
 *	Set the address map for the current thread to the specified map
 */

vm_map_t
vm_map_switch(
	vm_map_t	map)
{
	int		mycpu;
	thread_t	thread = current_thread();
	vm_map_t	oldmap = thread->map;

	mp_disable_preemption();
	mycpu = cpu_number();

	/*
	 *	Deactivate the current map and activate the requested map
	 */
	PMAP_SWITCH_USER(thread, map, mycpu);

	mp_enable_preemption();
	return(oldmap);
}


/*
 *	Routine:	vm_map_write_user
 *
 *	Description:
 *		Copy out data from a kernel space into space in the
 *		destination map. The space must already exist in the
 *		destination map.
 *		NOTE:  This routine should only be called by threads
 *		which can block on a page fault. i.e. kernel mode user
 *		threads.
 *
 */
kern_return_t
vm_map_write_user(
	vm_map_t		map,
	void			*src_p,
	vm_map_address_t	dst_addr,
	vm_size_t		size)
{
	kern_return_t	kr = KERN_SUCCESS;

	if(current_map() == map) {
		if (copyout(src_p, dst_addr, size)) {
			kr = KERN_INVALID_ADDRESS;
		}
	} else {
		vm_map_t	oldmap;

		/* take on the identity of the target map while doing */
		/* the transfer */

		vm_map_reference(map);
		oldmap = vm_map_switch(map);
		if (copyout(src_p, dst_addr, size)) {
			kr = KERN_INVALID_ADDRESS;
		}
		vm_map_switch(oldmap);
		vm_map_deallocate(map);
	}
	return kr;
}

/*
 *	Routine:	vm_map_read_user
 *
 *	Description:
 *		Copy in data from a user space source map into the
 *		kernel map. The space must already exist in the
 *		kernel map.
 *		NOTE:  This routine should only be called by threads
 *		which can block on a page fault. i.e. kernel mode user
 *		threads.
 *
 */
kern_return_t
vm_map_read_user(
	vm_map_t		map,
	vm_map_address_t	src_addr,
	void			*dst_p,
	vm_size_t		size)
{
	kern_return_t	kr = KERN_SUCCESS;

	if(current_map() == map) {
		if (copyin(src_addr, dst_p, size)) {
			kr = KERN_INVALID_ADDRESS;
		}
	} else {
		vm_map_t	oldmap;

		/* take on the identity of the target map while doing */
		/* the transfer */

		vm_map_reference(map);
		oldmap = vm_map_switch(map);
		if (copyin(src_addr, dst_p, size)) {
			kr = KERN_INVALID_ADDRESS;
		}
		vm_map_switch(oldmap);
		vm_map_deallocate(map);
	}
	return kr;
}


/*
 *	vm_map_check_protection:
 *
 *	Assert that the target map allows the specified
 *	privilege on the entire address region given.
 *	The entire region must be allocated.
 */
boolean_t vm_map_check_protection(map, start, end, protection)
	register vm_map_t	map;
	register vm_map_offset_t	start;
	register vm_map_offset_t	end;
	register vm_prot_t	protection;
{
	register vm_map_entry_t	entry;
	vm_map_entry_t		tmp_entry;

	vm_map_lock(map);

    if (start < vm_map_min(map) || end > vm_map_max(map) || start > end)
	{
			vm_map_unlock(map);
			return (FALSE);
	}

	if (!vm_map_lookup_entry(map, start, &tmp_entry)) {
		vm_map_unlock(map);
		return(FALSE);
	}

	entry = tmp_entry;

	while (start < end) {
		if (entry == vm_map_to_entry(map)) {
			vm_map_unlock(map);
			return(FALSE);
		}

		/*
		 *	No holes allowed!
		 */

		if (start < entry->vme_start) {
			vm_map_unlock(map);
			return(FALSE);
		}

		/*
		 * Check protection associated with entry.
		 */

		if ((entry->protection & protection) != protection) {
			vm_map_unlock(map);
			return(FALSE);
		}

		/* go to next entry */

		start = entry->vme_end;
		entry = entry->vme_next;
	}
	vm_map_unlock(map);
	return(TRUE);
}

kern_return_t
vm_map_purgable_control(
	vm_map_t		map,
	vm_map_offset_t		address,
	vm_purgable_t		control,
	int			*state)
{
	vm_map_entry_t		entry;
	vm_object_t		object;
	kern_return_t		kr;

	/*
	 * Vet all the input parameters and current type and state of the
	 * underlaying object.  Return with an error if anything is amiss.
	 */
	if (map == VM_MAP_NULL)
		return(KERN_INVALID_ARGUMENT);

	if (control != VM_PURGABLE_SET_STATE &&
	    control != VM_PURGABLE_GET_STATE)
		return(KERN_INVALID_ARGUMENT);

	if (control == VM_PURGABLE_SET_STATE &&
	    (*state < VM_PURGABLE_STATE_MIN ||
	     *state > VM_PURGABLE_STATE_MAX))
		return(KERN_INVALID_ARGUMENT);

	vm_map_lock(map);

	if (!vm_map_lookup_entry(map, address, &entry) || entry->is_sub_map) {

		/*
		 * Must pass a valid non-submap address.
		 */
		vm_map_unlock(map);
		return(KERN_INVALID_ADDRESS);
	}

	if ((entry->protection & VM_PROT_WRITE) == 0) {
		/*
		 * Can't apply purgable controls to something you can't write.
		 */
		vm_map_unlock(map);
		return(KERN_PROTECTION_FAILURE);
	}

	object = entry->object.vm_object;
	if (object == VM_OBJECT_NULL) {
		/*
		 * Object must already be present or it can't be purgable.
		 */
		vm_map_unlock(map);
		return KERN_INVALID_ARGUMENT;
	}
		     
	vm_object_lock(object);

	if (entry->offset != 0 || 
	    entry->vme_end - entry->vme_start != object->size) {
		/*
		 * Can only apply purgable controls to the whole (existing)
		 * object at once.
		 */
		vm_map_unlock(map);
		vm_object_unlock(object);
		return KERN_INVALID_ARGUMENT;
	}
		
	vm_map_unlock(map);

	kr = vm_object_purgable_control(object, control, state);

	vm_object_unlock(object);

	return kr;
}

kern_return_t
vm_map_page_info(
	vm_map_t		target_map,
	vm_map_offset_t	offset,
	int			*disposition,
	int			*ref_count)
{
	vm_map_entry_t	map_entry;
	vm_object_t	object;
	vm_page_t	m;

restart_page_query:
	*disposition = 0;
	*ref_count = 0;
	vm_map_lock(target_map);
	if(!vm_map_lookup_entry(target_map, offset, &map_entry)) {
		vm_map_unlock(target_map);
		return KERN_FAILURE;
	}
	offset -= map_entry->vme_start;  /* adjust to offset within entry */
	offset += map_entry->offset;	 /* adjust to target object offset */
	if(map_entry->object.vm_object != VM_OBJECT_NULL) {
		if(!map_entry->is_sub_map) {
			object = map_entry->object.vm_object;
		} else {
			vm_map_unlock(target_map);
			target_map = map_entry->object.sub_map;
			goto restart_page_query;
		}
	} else {
		vm_map_unlock(target_map);
		return KERN_FAILURE;
	}
	vm_object_lock(object);
	vm_map_unlock(target_map);
	while(TRUE) {
		m = vm_page_lookup(object, offset);
		if (m != VM_PAGE_NULL) {
			*disposition |= VM_PAGE_QUERY_PAGE_PRESENT;
			break;
		} else {
			if(object->shadow) {
				offset += object->shadow_offset;
				vm_object_unlock(object);
				object = object->shadow;
				vm_object_lock(object);
				continue;
			}
			vm_object_unlock(object);
			return KERN_FAILURE;
		}
	}

	/* The ref_count is not strictly accurate, it measures the number   */
	/* of entities holding a ref on the object, they may not be mapping */
	/* the object or may not be mapping the section holding the         */
	/* target page but its still a ball park number and though an over- */
	/* count, it picks up the copy-on-write cases                       */

	/* We could also get a picture of page sharing from pmap_attributes */
	/* but this would under count as only faulted-in mappings would     */
	/* show up.							    */

	*ref_count = object->ref_count;

	if (m->fictitious) {
		*disposition |= VM_PAGE_QUERY_PAGE_FICTITIOUS;
		vm_object_unlock(object);
		return KERN_SUCCESS;
	}

	if (m->dirty)
		*disposition |= VM_PAGE_QUERY_PAGE_DIRTY;
	else if(pmap_is_modified(m->phys_page))
		*disposition |= VM_PAGE_QUERY_PAGE_DIRTY;

	if (m->reference)
		*disposition |= VM_PAGE_QUERY_PAGE_REF;
	else if(pmap_is_referenced(m->phys_page))
		*disposition |= VM_PAGE_QUERY_PAGE_REF;

	vm_object_unlock(object);
	return KERN_SUCCESS;
	
}


/* For a given range, check all map entries.  If the entry coresponds to */
/* the old vm_region/map provided on the call, replace it with the 	 */
/* corresponding range in the new vm_region/map */
kern_return_t vm_map_region_replace(
	vm_map_t	target_map,
	ipc_port_t	old_region, 
	ipc_port_t	new_region,
	vm_map_offset_t	start,
	vm_map_offset_t	end)
{
	vm_named_entry_t	old_object;
	vm_named_entry_t	new_object;
	vm_map_t		old_submap;
	vm_map_t		new_submap;
	vm_map_offset_t		addr;
	vm_map_entry_t		entry;
	int			nested_pmap = 0;


	vm_map_lock(target_map);
	old_object = (vm_named_entry_t)old_region->ip_kobject;
	new_object = (vm_named_entry_t)new_region->ip_kobject;
	if((!old_object->is_sub_map) || (!new_object->is_sub_map)) {
		vm_map_unlock(target_map);
		return KERN_INVALID_ARGUMENT;
	}
	old_submap = (vm_map_t)old_object->backing.map;
	new_submap = (vm_map_t)new_object->backing.map;
	vm_map_lock(old_submap);
	if((old_submap->min_offset != new_submap->min_offset) || 
			(old_submap->max_offset != new_submap->max_offset)) {
		vm_map_unlock(old_submap);
		vm_map_unlock(target_map);
		return KERN_INVALID_ARGUMENT;
	}
	if(!vm_map_lookup_entry(target_map, start, &entry)) {
		/* if the src is not contained, the entry preceeds */
		/* our range */
		addr = entry->vme_start;
		if(entry == vm_map_to_entry(target_map)) {
			vm_map_unlock(old_submap);
			vm_map_unlock(target_map);
			return KERN_SUCCESS;
		}
	}
	if ((entry->use_pmap) && 
			(new_submap->pmap == NULL)) {
		new_submap->pmap = pmap_create((vm_map_size_t) 0);
		if(new_submap->pmap == PMAP_NULL) {
			vm_map_unlock(old_submap);
			vm_map_unlock(target_map);
			return(KERN_NO_SPACE);
		}
	}
	addr = entry->vme_start;
	vm_map_reference(old_submap);
	while((entry != vm_map_to_entry(target_map)) && 
					(entry->vme_start < end)) {
		if((entry->is_sub_map) && 
			(entry->object.sub_map == old_submap)) {
			if(entry->use_pmap) {
				if((start & 0x0fffffff) || 
					((end - start) != 0x10000000)) {
					vm_map_unlock(old_submap);
					vm_map_deallocate(old_submap);
					vm_map_unlock(target_map);
					return  KERN_INVALID_ARGUMENT;
				}
				nested_pmap = 1;
			}
			entry->object.sub_map = new_submap;
			vm_map_reference(new_submap);
			vm_map_deallocate(old_submap);
		}
		entry = entry->vme_next;
		addr = entry->vme_start;
	}
	if(nested_pmap) {
#ifndef i386
		pmap_unnest(target_map->pmap, (addr64_t)start);
		if(target_map->mapped) {
			vm_map_submap_pmap_clean(target_map,
				start, end, old_submap, 0);
		}
		pmap_nest(target_map->pmap, new_submap->pmap, 
				(addr64_t)start, (addr64_t)start, 
				(uint64_t)(end - start));
#endif /* i386 */
	} else {
		vm_map_submap_pmap_clean(target_map,
				start, end, old_submap, 0);
	}
	vm_map_unlock(old_submap);
	vm_map_deallocate(old_submap);
	vm_map_unlock(target_map);
	return KERN_SUCCESS;
}

/*
 *	vm_map_msync
 *
 *	Synchronises the memory range specified with its backing store
 *	image by either flushing or cleaning the contents to the appropriate
 *	memory manager engaging in a memory object synchronize dialog with
 *	the manager.  The client doesn't return until the manager issues
 *	m_o_s_completed message.  MIG Magically converts user task parameter
 *	to the task's address map.
 *
 *	interpretation of sync_flags
 *	VM_SYNC_INVALIDATE	- discard pages, only return precious
 *				  pages to manager.
 *
 *	VM_SYNC_INVALIDATE & (VM_SYNC_SYNCHRONOUS | VM_SYNC_ASYNCHRONOUS)
 *				- discard pages, write dirty or precious
 *				  pages back to memory manager.
 *
 *	VM_SYNC_SYNCHRONOUS | VM_SYNC_ASYNCHRONOUS
 *				- write dirty or precious pages back to
 *				  the memory manager.
 *
 *	VM_SYNC_CONTIGUOUS	- does everything normally, but if there
 *				  is a hole in the region, and we would
 *				  have returned KERN_SUCCESS, return
 *				  KERN_INVALID_ADDRESS instead.
 *
 *	NOTE
 *	The memory object attributes have not yet been implemented, this
 *	function will have to deal with the invalidate attribute
 *
 *	RETURNS
 *	KERN_INVALID_TASK		Bad task parameter
 *	KERN_INVALID_ARGUMENT		both sync and async were specified.
 *	KERN_SUCCESS			The usual.
 *	KERN_INVALID_ADDRESS		There was a hole in the region.
 */

kern_return_t
vm_map_msync(
	vm_map_t		map,
	vm_map_address_t	address,
	vm_map_size_t		size,
	vm_sync_t		sync_flags)
{
	msync_req_t		msr;
	msync_req_t		new_msr;
	queue_chain_t		req_q;	/* queue of requests for this msync */
	vm_map_entry_t		entry;
	vm_map_size_t		amount_left;
	vm_object_offset_t	offset;
	boolean_t		do_sync_req;
	boolean_t		modifiable;
	boolean_t		had_hole = FALSE;
	
	if ((sync_flags & VM_SYNC_ASYNCHRONOUS) &&
	    (sync_flags & VM_SYNC_SYNCHRONOUS))
		return(KERN_INVALID_ARGUMENT);

	/*
	 * align address and size on page boundaries
	 */
	size = vm_map_round_page(address + size) - vm_map_trunc_page(address);
	address = vm_map_trunc_page(address);

        if (map == VM_MAP_NULL)
                return(KERN_INVALID_TASK);

	if (size == 0)
		return(KERN_SUCCESS);

	queue_init(&req_q);
	amount_left = size;

	while (amount_left > 0) {
		vm_object_size_t	flush_size;
		vm_object_t		object;

		vm_map_lock(map);
		if (!vm_map_lookup_entry(map,
			vm_map_trunc_page(address), &entry)) {

			vm_size_t	skip;

			/*
			 * hole in the address map.
			 */
			had_hole = TRUE;

			/*
			 * Check for empty map.
			 */
			if (entry == vm_map_to_entry(map) &&
			    entry->vme_next == entry) {
				vm_map_unlock(map);
				break;
			}
			/*
			 * Check that we don't wrap and that
			 * we have at least one real map entry.
			 */
			if ((map->hdr.nentries == 0) ||
			    (entry->vme_next->vme_start < address)) {
				vm_map_unlock(map);
				break;
			}
			/*
			 * Move up to the next entry if needed
			 */
			skip = (entry->vme_next->vme_start - address);
			if (skip >= amount_left)
				amount_left = 0;
			else
				amount_left -= skip;
			address = entry->vme_next->vme_start;
			vm_map_unlock(map);
			continue;
		}

		offset = address - entry->vme_start;

		/*
		 * do we have more to flush than is contained in this
		 * entry ?
		 */
		if (amount_left + entry->vme_start + offset > entry->vme_end) {
			flush_size = entry->vme_end -
						 (entry->vme_start + offset);
		} else {
			flush_size = amount_left;
		}
		amount_left -= flush_size;
		address += flush_size;

		if (entry->is_sub_map == TRUE) {
			vm_map_t	local_map;
			vm_map_offset_t	local_offset;

			local_map = entry->object.sub_map;
			local_offset = entry->offset;
			vm_map_unlock(map);
			if (vm_map_msync(
				local_map,
				local_offset,
				flush_size,
				sync_flags) == KERN_INVALID_ADDRESS) {
				had_hole = TRUE;
			}
			continue;
		}
		object = entry->object.vm_object;

		/*
		 * We can't sync this object if the object has not been
		 * created yet
		 */
		if (object == VM_OBJECT_NULL) {
			vm_map_unlock(map);
			continue;
		}
		offset += entry->offset;
		modifiable = (entry->protection & VM_PROT_WRITE)
				!= VM_PROT_NONE;

                vm_object_lock(object);

		if (sync_flags & (VM_SYNC_KILLPAGES | VM_SYNC_DEACTIVATE)) {
		        boolean_t kill_pages = 0;

			if (sync_flags & VM_SYNC_KILLPAGES) {
			        if (object->ref_count == 1 && !entry->needs_copy && !object->shadow)
				        kill_pages = 1;
				else
				        kill_pages = -1;
			}
			if (kill_pages != -1)
			        vm_object_deactivate_pages(object, offset, 
							       (vm_object_size_t)flush_size, kill_pages);
			vm_object_unlock(object);
			vm_map_unlock(map);
			continue;
		}
		/*
		 * We can't sync this object if there isn't a pager.
		 * Don't bother to sync internal objects, since there can't
		 * be any "permanent" storage for these objects anyway.
		 */
		if ((object->pager == MEMORY_OBJECT_NULL) ||
		    (object->internal) || (object->private)) {
			vm_object_unlock(object);
			vm_map_unlock(map);
			continue;
		}
		/*
		 * keep reference on the object until syncing is done
		 */
		assert(object->ref_count > 0);
		object->ref_count++;
		vm_object_res_reference(object);
		vm_object_unlock(object);

		vm_map_unlock(map);

		do_sync_req = vm_object_sync(object,
					offset,
					flush_size,
					sync_flags & VM_SYNC_INVALIDATE,
					(modifiable &&
					(sync_flags & VM_SYNC_SYNCHRONOUS ||
					 sync_flags & VM_SYNC_ASYNCHRONOUS)),
					 sync_flags & VM_SYNC_SYNCHRONOUS);
		/*
		 * only send a m_o_s if we returned pages or if the entry
		 * is writable (ie dirty pages may have already been sent back)
		 */
		if (!do_sync_req && !modifiable) {
			vm_object_deallocate(object);
			continue;
		}
		msync_req_alloc(new_msr);

                vm_object_lock(object);
		offset += object->paging_offset;

		new_msr->offset = offset;
		new_msr->length = flush_size;
		new_msr->object = object;
		new_msr->flag = VM_MSYNC_SYNCHRONIZING;
re_iterate:
		queue_iterate(&object->msr_q, msr, msync_req_t, msr_q) {
			/*
			 * need to check for overlapping entry, if found, wait
			 * on overlapping msr to be done, then reiterate
			 */
			msr_lock(msr);
			if (msr->flag == VM_MSYNC_SYNCHRONIZING &&
			    ((offset >= msr->offset && 
			      offset < (msr->offset + msr->length)) ||
			     (msr->offset >= offset &&
			      msr->offset < (offset + flush_size))))
			{
				assert_wait((event_t) msr,THREAD_INTERRUPTIBLE);
				msr_unlock(msr);
				vm_object_unlock(object);
				thread_block(THREAD_CONTINUE_NULL);
				vm_object_lock(object);
				goto re_iterate;
			}
			msr_unlock(msr);
		}/* queue_iterate */

		queue_enter(&object->msr_q, new_msr, msync_req_t, msr_q);
		vm_object_unlock(object);

		queue_enter(&req_q, new_msr, msync_req_t, req_q);

		(void) memory_object_synchronize(
				object->pager,
				offset,
				flush_size,
				sync_flags & ~VM_SYNC_CONTIGUOUS);
	}/* while */

	/*
	 * wait for memory_object_sychronize_completed messages from pager(s)
	 */

	while (!queue_empty(&req_q)) {
		msr = (msync_req_t)queue_first(&req_q);
		msr_lock(msr);
		while(msr->flag != VM_MSYNC_DONE) {
			assert_wait((event_t) msr, THREAD_INTERRUPTIBLE);
			msr_unlock(msr);
			thread_block(THREAD_CONTINUE_NULL);
			msr_lock(msr);
		}/* while */
		queue_remove(&req_q, msr, msync_req_t, req_q);
		msr_unlock(msr);
		vm_object_deallocate(msr->object);
		msync_req_free(msr);
	}/* queue_iterate */

	/* for proper msync() behaviour */
	if (had_hole == TRUE && (sync_flags & VM_SYNC_CONTIGUOUS))
		return(KERN_INVALID_ADDRESS);

	return(KERN_SUCCESS);
}/* vm_msync */

/* Takes existing source and destination sub-maps and clones the contents of */
/* the source map */
kern_return_t
vm_region_clone(
	ipc_port_t	src_region, 
	ipc_port_t	dst_region)
{
	vm_named_entry_t	src_object;
	vm_named_entry_t	dst_object;
	vm_map_t		src_map;
	vm_map_t		dst_map;
	vm_map_offset_t		addr;
	vm_map_offset_t		max_off;
	vm_map_entry_t		entry;
	vm_map_entry_t		new_entry;
	vm_map_entry_t		insert_point;

	src_object = (vm_named_entry_t)src_region->ip_kobject;
	dst_object = (vm_named_entry_t)dst_region->ip_kobject;
	if((!src_object->is_sub_map) || (!dst_object->is_sub_map)) {
		return KERN_INVALID_ARGUMENT;
	}
	src_map = (vm_map_t)src_object->backing.map;
	dst_map = (vm_map_t)dst_object->backing.map;
	/* destination map is assumed to be unavailable to any other */
	/* activity.  i.e. it is new */
	vm_map_lock(src_map);
	if((src_map->min_offset != dst_map->min_offset) 
			|| (src_map->max_offset != dst_map->max_offset)) {
		vm_map_unlock(src_map);
		return KERN_INVALID_ARGUMENT;
	}
	addr = src_map->min_offset;
	vm_map_lookup_entry(dst_map, addr, &entry);
	if(entry == vm_map_to_entry(dst_map)) {
		entry = entry->vme_next;
	}
	if(entry == vm_map_to_entry(dst_map)) {
		max_off = src_map->max_offset;
	} else {
		max_off =  entry->vme_start;
	}
	vm_map_lookup_entry(src_map, addr, &entry);
	if(entry == vm_map_to_entry(src_map)) {
		entry = entry->vme_next;
	}
	vm_map_lookup_entry(dst_map, addr, &insert_point);
	while((entry != vm_map_to_entry(src_map)) && 
					(entry->vme_end <= max_off)) {
		addr = entry->vme_start;
		new_entry = vm_map_entry_create(dst_map);
		vm_map_entry_copy(new_entry, entry);
		vm_map_entry_link(dst_map, insert_point, new_entry);
		insert_point = new_entry;
		if (entry->object.vm_object != VM_OBJECT_NULL) {
			if (new_entry->is_sub_map) {
				vm_map_reference(new_entry->object.sub_map);
			} else {
				vm_object_reference(
					new_entry->object.vm_object);
			}
		}
		dst_map->size += new_entry->vme_end - new_entry->vme_start;
		entry = entry->vme_next;
	}
	vm_map_unlock(src_map);
	return KERN_SUCCESS;
}

/*
 *	Routine:	convert_port_entry_to_map
 *	Purpose:
 *		Convert from a port specifying an entry or a task
 *		to a map. Doesn't consume the port ref; produces a map ref,
 *		which may be null.  Unlike convert_port_to_map, the
 *		port may be task or a named entry backed.
 *	Conditions:
 *		Nothing locked.
 */


vm_map_t
convert_port_entry_to_map(
	ipc_port_t	port)
{
	vm_map_t map;
	vm_named_entry_t	named_entry;

	if(IP_VALID(port) && (ip_kotype(port) == IKOT_NAMED_ENTRY)) {
		while(TRUE) {
			ip_lock(port);
			if(ip_active(port) && (ip_kotype(port) 
						== IKOT_NAMED_ENTRY)) {
				named_entry =
					 (vm_named_entry_t)port->ip_kobject;
				if (!(mutex_try(&(named_entry)->Lock))) {
                       			ip_unlock(port);
                       			mutex_pause();
                       			continue;
                		}
				named_entry->ref_count++;
				mutex_unlock(&(named_entry)->Lock);
				ip_unlock(port);
				if ((named_entry->is_sub_map) &&
					(named_entry->protection 
					& VM_PROT_WRITE)) {
					map = named_entry->backing.map;
				} else {
					mach_destroy_memory_entry(port);
					return VM_MAP_NULL;
				}
				vm_map_reference_swap(map);
				mach_destroy_memory_entry(port);
				break;
			}
			else 
				return VM_MAP_NULL;
		}
	}
	else
		map = convert_port_to_map(port);

	return map;
}

/*
 *	Routine:	convert_port_entry_to_object
 *	Purpose:
 *		Convert from a port specifying a named entry to an
 *		object. Doesn't consume the port ref; produces a map ref,
 *		which may be null. 
 *	Conditions:
 *		Nothing locked.
 */


vm_object_t
convert_port_entry_to_object(
	ipc_port_t	port)
{
	vm_object_t object;
	vm_named_entry_t	named_entry;

	if(IP_VALID(port) && (ip_kotype(port) == IKOT_NAMED_ENTRY)) {
		while(TRUE) {
			ip_lock(port);
			if(ip_active(port) && (ip_kotype(port) 
						== IKOT_NAMED_ENTRY)) {
				named_entry =
					 (vm_named_entry_t)port->ip_kobject;
				if (!(mutex_try(&(named_entry)->Lock))) {
                       			ip_unlock(port);
                       			mutex_pause();
                       			continue;
                		}
				named_entry->ref_count++;
				mutex_unlock(&(named_entry)->Lock);
				ip_unlock(port);
				if ((!named_entry->is_sub_map) &&
					(!named_entry->is_pager) &&
					(named_entry->protection 
					& VM_PROT_WRITE)) {
					object = named_entry->backing.object;
				} else {
					mach_destroy_memory_entry(port);
					return (vm_object_t)NULL;
				}
				vm_object_reference(named_entry->backing.object);
				mach_destroy_memory_entry(port);
				break;
			}
			else 
				return (vm_object_t)NULL;
		}
	} else {
		return (vm_object_t)NULL;
	}

	return object;
}

/*
 * Export routines to other components for the things we access locally through
 * macros.
 */
#undef current_map
vm_map_t
current_map(void)
{
	return (current_map_fast());
}

/*
 *	vm_map_reference:
 *
 *	Most code internal to the osfmk will go through a
 *	macro defining this.  This is always here for the
 *	use of other kernel components.
 */
#undef vm_map_reference
void
vm_map_reference(
	register vm_map_t	map)
{
	if (map == VM_MAP_NULL)
		return;

	mutex_lock(&map->s_lock);
#if	TASK_SWAPPER
	assert(map->res_count > 0);
	assert(map->ref_count >= map->res_count);
	map->res_count++;
#endif
	map->ref_count++;
	mutex_unlock(&map->s_lock);
}

/*
 *	vm_map_deallocate:
 *
 *	Removes a reference from the specified map,
 *	destroying it if no references remain.
 *	The map should not be locked.
 */
void
vm_map_deallocate(
	register vm_map_t	map)
{
	unsigned int		ref;

	if (map == VM_MAP_NULL)
		return;

	mutex_lock(&map->s_lock);
	ref = --map->ref_count;
	if (ref > 0) {
		vm_map_res_deallocate(map);
		mutex_unlock(&map->s_lock);
		return;
	}
	assert(map->ref_count == 0);
	mutex_unlock(&map->s_lock);

#if	TASK_SWAPPER
	/*
	 * The map residence count isn't decremented here because
	 * the vm_map_delete below will traverse the entire map, 
	 * deleting entries, and the residence counts on objects
	 * and sharing maps will go away then.
	 */
#endif

	vm_map_destroy(map);
}

#ifdef __PPC__

/* LP64todo - this whole mechanism is temporary.  It should be redone when
 * the pmap layer can handle 64-bit address spaces.  Until then, we trump
 * up a map entry for the 64-bit commpage above the map's max_offset.
 */
extern vm_map_t		com_region_map64;	/* the submap for 64-bit commpage */
SInt32			commpage64s_in_use = 0;
 
void
vm_map_commpage64(
        vm_map_t        map )
{
        vm_map_entry_t  entry;
	vm_object_t	object;
        
        vm_map_lock(map);
        
        /* The commpage is necessarily the last entry in the map.
	 * See if one is already there (not sure if this can happen???)
	 */
	entry = vm_map_last_entry(map);
	if (entry != vm_map_to_entry(map)) {
		if (entry->vme_end >= (vm_map_offset_t)_COMM_PAGE_BASE_ADDRESS) {
			vm_map_unlock(map);
			return;
		}
	}
	
	entry = vm_map_first_entry(com_region_map64);	/* the 64-bit commpage */
	object = entry->object.vm_object;
	vm_object_reference(object);
        
	/* We bypass vm_map_enter() because we are adding the entry past the
	 * map's max_offset.
	 */
        entry = vm_map_entry_insert(
                        map,
                        vm_map_last_entry(map),		/* insert after last entry */
                        _COMM_PAGE_BASE_ADDRESS,
                        _COMM_PAGE_BASE_ADDRESS+_COMM_PAGE_AREA_USED,
                        object,
                        0,				/* offset */
                        FALSE,				/* needs_copy */
			FALSE,				/* is_shared */
                        FALSE,				/* in_transition */
			VM_PROT_READ,
			VM_PROT_READ,
			VM_BEHAVIOR_DEFAULT,
			VM_INHERIT_NONE,
			1 );				/* wired_count */
	
	vm_map_unlock(map);
	
	OSIncrementAtomic(&commpage64s_in_use);
}


/* LP64todo - remove this!  */

void
vm_map_remove_commpage64(
        vm_map_t        map )
{
        vm_map_entry_t  entry;
	int		deleted = 0;
	
	while( 1 ) {
		vm_map_lock(map);
	
		entry = vm_map_last_entry(map);
		if ((entry == vm_map_to_entry(map)) || 
			(entry->vme_start < (vm_map_offset_t)_COMM_PAGE_BASE_ADDRESS))
				break;
				
		/* clearing the wired count isn't strictly correct */
		entry->wired_count = 0;
		vm_map_entry_delete(map,entry);
		deleted++;
	}
	
	vm_map_unlock(map);
	
	if (deleted != 0)
		OSDecrementAtomic(&commpage64s_in_use);
}

#endif /* __PPC__ */

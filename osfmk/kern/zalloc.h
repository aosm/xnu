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
 *	File:	zalloc.h
 *	Author:	Avadis Tevanian, Jr.
 *	Date:	 1985
 *
 */

#ifdef	KERNEL_PRIVATE

#ifndef	_KERN_ZALLOC_H_
#define _KERN_ZALLOC_H_

#include <mach/machine/vm_types.h>
#include <kern/kern_types.h>
#include <sys/cdefs.h>

#ifdef	MACH_KERNEL_PRIVATE

#include <zone_debug.h>
#include <mach_kdb.h>
#include <kern/lock.h>
#include <kern/locks.h>
#include <kern/queue.h>
#include <kern/call_entry.h>

/*
 *	A zone is a collection of fixed size blocks for which there
 *	is fast allocation/deallocation access.  Kernel routines can
 *	use zones to manage data structures dynamically, creating a zone
 *	for each type of data structure to be managed.
 *
 */

struct zone {
	int		count;		/* Number of elements used now */
	vm_offset_t	free_elements;
	decl_lck_mtx_data(,lock)	/* zone lock */
	lck_mtx_ext_t   lock_ext;	/* placeholder for indirect mutex */
	lck_attr_t      lock_attr;	/* zone lock attribute */
	lck_grp_t       lock_grp;	/* zone lock group */
	lck_grp_attr_t  lock_grp_attr;	/* zone lock group attribute */
	vm_size_t	cur_size;	/* current memory utilization */
	vm_size_t	max_size;	/* how large can this zone grow */
	vm_size_t	elem_size;	/* size of an element */
	vm_size_t	alloc_size;	/* size used for more memory */
	uint64_t	sum_count;	/* count of allocs (life of zone) */
	unsigned int
	/* boolean_t */ exhaustible :1,	/* (F) merely return if empty? */
	/* boolean_t */	collectable :1,	/* (F) garbage collect empty pages */
	/* boolean_t */	expandable :1,	/* (T) expand zone (with message)? */
	/* boolean_t */ allows_foreign :1,/* (F) allow non-zalloc space */
	/* boolean_t */	doing_alloc :1,	/* is zone expanding now? */
	/* boolean_t */	waiting :1,	/* is thread waiting for expansion? */
	/* boolean_t */	async_pending :1,	/* asynchronous allocation pending? */
#if CONFIG_ZLEAKS
	/* boolean_t */ zleak_on :1,	/* Are we collecting allocation information? */
#endif	/* ZONE_DEBUG */
	/* boolean_t */	caller_acct: 1, /* do we account allocation/free to the caller? */  
	/* boolean_t */	doing_gc :1,	/* garbage collect in progress? */
	/* boolean_t */ noencrypt :1;
	int		index;		/* index into zone_info arrays for this zone */
	struct zone *	next_zone;	/* Link for all-zones list */
	call_entry_data_t	call_async_alloc;	/* callout for asynchronous alloc */
	const char	*zone_name;	/* a name for the zone */
#if	ZONE_DEBUG
	queue_head_t	active_zones;	/* active elements */
#endif	/* ZONE_DEBUG */

#if CONFIG_ZLEAKS
	uint32_t num_allocs;	/* alloc stats for zleak benchmarks */
	uint32_t num_frees;		/* free stats for zleak benchmarks */
	uint32_t zleak_capture; /* per-zone counter for capturing every N allocations */
#endif /* CONFIG_ZLEAKS */
};

/*
 *	structure for tracking zone usage
 *	Used either one per task/thread for all zones or <per-task,per-zone>.
 */
typedef struct zinfo_usage_store_t {
	/* These fields may be updated atomically, and so must be 8 byte aligned */
	uint64_t	alloc __attribute__((aligned(8)));		/* allocation counter */
	uint64_t	free __attribute__((aligned(8)));		/* free counter */
} zinfo_usage_store_t;
typedef zinfo_usage_store_t *zinfo_usage_t;

extern void		zone_gc(void);
extern void		consider_zone_gc(boolean_t);

/* Steal memory for zone module */
extern void		zone_steal_memory(void);

/* Bootstrap zone module (create zone zone) */
extern void		zone_bootstrap(void) __attribute__((section("__TEXT, initcode")));

/* Init zone module */
extern void		zone_init(
					vm_size_t	map_size) __attribute__((section("__TEXT, initcode")));

/* Handle per-task zone info */
extern void		zinfo_task_init(task_t task);
extern void		zinfo_task_free(task_t task);


/* Stack use statistics */
extern void		stack_fake_zone_init(int zone_index);
extern void		stack_fake_zone_info(
					int			*count, 
					vm_size_t	*cur_size, 
					vm_size_t	*max_size,
					vm_size_t	*elem_size,
					vm_size_t	*alloc_size, 
					uint64_t	*sum_size,
					int			*collectable, 
					int			*exhaustable,
					int		*caller_acct);

#if		ZONE_DEBUG

#if		MACH_KDB

extern void *	next_element(
				zone_t		z,
				void 		*elt);

extern void *	first_element(
				zone_t		z);

#endif	/* MACH_KDB */

extern void		zone_debug_enable(
				zone_t		z);

extern void		zone_debug_disable(
				zone_t		z);

#endif	/* ZONE_DEBUG */

#endif	/* MACH_KERNEL_PRIVATE */

__BEGIN_DECLS

#ifdef	XNU_KERNEL_PRIVATE

/* Allocate from zone */
extern void *	zalloc(
					zone_t		zone);

/* Free zone element */
extern void		zfree(
					zone_t		zone,
					void 		*elem);

/* Create zone */
extern zone_t	zinit(
					vm_size_t	size,		/* the size of an element */
					vm_size_t	maxmem,		/* maximum memory to use */
					vm_size_t	alloc,		/* allocation size */
					const char	*name);		/* a name for the zone */


/* Non-blocking version of zalloc */
extern void *	zalloc_noblock(
					zone_t		zone);

/* direct (non-wrappered) interface */
extern void *	zalloc_canblock(
					zone_t		zone,
					boolean_t	canblock);

/* Get from zone free list */
extern void *	zget(
					zone_t		zone);

/* Fill zone with memory */
extern void		zcram(
					zone_t		zone,
					void		*newmem,
					vm_size_t	size);

/* Initially fill zone with specified number of elements */
extern int		zfill(
					zone_t		zone,
					int			nelem);

/* Change zone parameters */
extern void		zone_change(
					zone_t			zone,
					unsigned int	item,
					boolean_t		value);

/* Item definitions */
#define Z_EXHAUST	1	/* Make zone exhaustible	*/
#define Z_COLLECT	2	/* Make zone collectable	*/
#define Z_EXPAND	3	/* Make zone expandable		*/
#define	Z_FOREIGN	4	/* Allow collectable zone to contain foreign elements */
#define Z_CALLERACCT	5	/* Account alloc/free against the caller */
#define Z_NOENCRYPT	6	/* Don't encrypt zone during hibernation */

/* Preallocate space for zone from zone map */
extern void		zprealloc(
					zone_t		zone,
					vm_size_t	size);

extern integer_t	zone_free_count(
						zone_t		zone);

/*
 * MAX_ZTRACE_DEPTH configures how deep of a stack trace is taken on each zalloc in the zone of interest.  15
 * levels is usually enough to get past all the layers of code in kalloc and IOKit and see who the actual
 * caller is up above these lower levels.
 *
 * This is used both for the zone leak detector and the zone corruption log.
 */

#define MAX_ZTRACE_DEPTH	15

/* 
 *  Structure for keeping track of a backtrace, used for leak detection.
 *  This is in the .h file because it is used during panic, see kern/debug.c
 *  A non-zero size indicates that the trace is in use.
 */
struct ztrace {
	vm_size_t		zt_size;			/* How much memory are all the allocations referring to this trace taking up? */
	uint32_t		zt_depth;			/* depth of stack (0 to MAX_ZTRACE_DEPTH) */
	void*			zt_stack[MAX_ZTRACE_DEPTH];	/* series of return addresses from OSBacktrace */
	uint32_t		zt_collisions;			/* How many times did a different stack land here while it was occupied? */
	uint32_t		zt_hit_count;			/* for determining effectiveness of hash function */
};

#if CONFIG_ZLEAKS

/* support for the kern.zleak.* sysctls */

extern kern_return_t zleak_activate(void);
extern vm_size_t zleak_max_zonemap_size;
extern vm_size_t zleak_global_tracking_threshold;
extern vm_size_t zleak_per_zone_tracking_threshold;

extern int get_zleak_state(void);

#endif	/* CONFIG_ZLEAKS */

/* These functions used for leak detection both in zalloc.c and mbuf.c */
extern uint32_t fastbacktrace(uintptr_t* bt, uint32_t max_frames);
extern uintptr_t hash_mix(uintptr_t x);
extern uint32_t hashbacktrace(uintptr_t* bt, uint32_t depth, uint32_t max_size);
extern uint32_t hashaddr(uintptr_t pt, uint32_t max_size);

#endif	/* XNU_KERNEL_PRIVATE */

__END_DECLS

#endif	/* _KERN_ZALLOC_H_ */

#endif	/* KERNEL_PRIVATE */

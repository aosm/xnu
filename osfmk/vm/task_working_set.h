/*
 * Copyright (c) 2000-2004 Apple Computer, Inc. All rights reserved.
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
 */

/*
 *	File:	vm/task_working_set.h
 *	Author:	Chris Youngworth
 *	Date:	2001
 *
 *	Working set detection and maintainence module
 *
 */

#ifndef	_VM_TASK_WORKING_SET_H_
#define _VM_TASK_WORKING_SET_H_

#include <mach/mach_types.h>

#ifdef	KERNEL_PRIVATE

#ifdef	MACH_KERNEL_PRIVATE

#include <kern/queue.h>
#include <vm/vm_object.h>

/* task working set */

#define tws_lock(tws)		mutex_lock(&(tws)->lock)
#define tws_lock_try(tws)	mutex_try(&(tws)->lock)
#define tws_unlock(tws)		mutex_unlock(&(tws)->lock)


#define	TWS_ARRAY_SIZE	8
#define TWS_HASH_LINE_COUNT 32
/* start out size to allow monitoring of working set without excessive use */
/* of wired memory resource. */
#define TWS_SMALL_HASH_LINE_COUNT 4 

/*
 * do not think of changing this hash unless you understand the implications
 * for the hash element page_cache field 
 */
#define do_tws_hash(object,offset, rows, lines) \
		((((((natural_t)(object)) +  \
			(((natural_t)(object)) >> 6) +  \
			(((natural_t)(object)) >> 12) +  \
			(((natural_t)(object)) >> 18) +  \
			(((natural_t)(object)) >> 24)) << 5) +  \
			((natural_t)(((vm_object_offset_t)(offset)) >> 17))) & \
			((rows * lines) -1))


#define alt_tws_hash(addr, rows, lines) \
		((((natural_t)(addr)) >> 17) & \
		((rows * lines) -1))


/* Long term startup data structures for initial cache filling */

#define	TWS_STARTUP_MAX_HASH_RETRY	3

/* 87 is the wrap skew, its based on  RETRY times the RETRY offset of 29 */
/*
#define do_startup_hash(addr, hash_size) \
		((((((natural_t)(addr)) >> 17) & \
		((2 * (hash_size)) -1)) + \
		(87 * (((addr) & TWS_ADDR_OFF_MASK)/(2 * (hash_size))))) & \
		((2 * (hash_size)) -1))
*/
#define do_startup_hash(addr, hash_size) \
		(((((natural_t)(addr)) >> 17) * 3) & \
		(hash_size -1))



struct tws_startup_ele {
	unsigned int		page_cache;
	vm_offset_t		page_addr;
};

typedef struct tws_startup_ele *tws_startup_ele_t;


struct tws_startup_ptr {
	tws_startup_ele_t	element;
	struct tws_startup_ptr	*next;
};

typedef struct tws_startup_ptr	*tws_startup_ptr_t;

struct tws_startup {
	unsigned int	tws_hash_size;	/* total size of struct in bytes */
	unsigned int	ele_count;
	unsigned int	array_size;	/* lines * rows * expansion_count */
	unsigned int	hash_count;
	
	tws_startup_ptr_t	*table; /* hash table */
	struct tws_startup_ptr	*ele;   /* hash elements */
	struct	tws_startup_ele	*array;
};

typedef struct tws_startup	*tws_startup_t;


/* Dynamic cache data structures for working set */

struct tws_hash_ele {
	vm_object_t		object;
	vm_object_offset_t	offset;
	unsigned int		page_cache;
	vm_offset_t		page_addr;
	int			line;
	vm_map_t		map;
};
typedef struct tws_hash_ele *tws_hash_ele_t;

#define TWS_HASH_OFF_MASK ((vm_object_offset_t)0xFFFFFFFFFFFE0000ULL)
#define TWS_ADDR_OFF_MASK ((vm_offset_t)0xFFFE0000)
#define TWS_INDEX_MASK ((vm_object_offset_t)0x000000000001F000ULL)

struct tws_hash_ptr {
	tws_hash_ele_t		element;
	struct tws_hash_ptr	*next;
};
typedef struct tws_hash_ptr *tws_hash_ptr_t;

struct tws_hash_line {
	unsigned int		ele_count;
	struct tws_hash_ele	list[TWS_ARRAY_SIZE];
};
typedef struct tws_hash_line *tws_hash_line_t;

#define TWS_HASH_STYLE_DEFAULT	0x0
#define TWS_HASH_STYLE_BASIC	0x1
#define TWS_HASH_STYLE_SIGNAL	0x2


#define TWS_ADDR_HASH 1
#define TWS_HASH_EXPANSION_MAX	10
#define TWS_MAX_REHASH 3


struct tws_hash {
	decl_mutex_data(,lock)          /* tws_hash's lock */
	int		style;

	unsigned int	current_line;
	unsigned int	pageout_count;
	unsigned int	line_count;

	unsigned int	number_of_lines;
	unsigned int	number_of_elements;
	unsigned int	expansion_count;
	unsigned int	time_of_creation;

	unsigned int	lookup_count;
	unsigned int	insert_count;

	tws_startup_t	startup_cache;
	char		*startup_name;
	int		startup_name_length;
	unsigned int	uid;
	int		mod;
	int		fid;

	unsigned int    obj_free_count[TWS_HASH_EXPANSION_MAX];
	unsigned int    addr_free_count[TWS_HASH_EXPANSION_MAX];
	tws_hash_ptr_t	free_hash_ele[TWS_HASH_EXPANSION_MAX];
	tws_hash_ptr_t	*table[TWS_HASH_EXPANSION_MAX];
	tws_hash_ptr_t	table_ele[TWS_HASH_EXPANSION_MAX];
	tws_hash_ptr_t	alt_ele[TWS_HASH_EXPANSION_MAX];
	struct tws_hash_line	*cache[TWS_HASH_EXPANSION_MAX];
}; 

typedef struct tws_hash *tws_hash_t;


extern kern_return_t tws_lookup(
			tws_hash_t		tws,	
			vm_object_offset_t	offset, 
			vm_object_t		object,
			tws_hash_line_t		 *line);

extern kern_return_t tws_insert(
			tws_hash_t		tws, 
			vm_object_offset_t	offset,
			vm_object_t		object,
			vm_offset_t		page_addr,
			vm_map_t		map);

extern void tws_build_cluster(
			tws_hash_t		tws,
			vm_object_t		object,
			vm_object_offset_t	*start,
			vm_object_offset_t	*end,
			vm_size_t		max_length);

extern void tws_line_signal(
			tws_hash_t		tws,
			vm_map_t		map,
			tws_hash_line_t 	hash_line,
			vm_offset_t		target_page);

extern void tws_hash_destroy(
			tws_hash_t		tws);

extern void tws_hash_ws_flush(
			tws_hash_t		tws);

extern kern_return_t tws_expand_working_set(
			tws_hash_t		old_tws,
			unsigned int		line_count,
			boolean_t		dump_data);

extern kern_return_t task_working_set_create(                
			task_t  		task,
			unsigned int 		lines,
			unsigned int 		rows,
			unsigned int 		style);

#endif	/* MACH_KERNEL_PRIVATE */

extern kern_return_t tws_handle_startup_file(
			task_t			task,
			unsigned int		uid,
			char			*app_name,
			void			*app_vp,
			boolean_t		*new_info);

extern kern_return_t tws_send_startup_info(
			task_t			task);

#endif	/* KERNEL_PRIVATE */

#endif  /* _VM_TASK_WORKING_SET_H_ */

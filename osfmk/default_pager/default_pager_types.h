/*
 * Copyright (c) 2000 Apple Computer, Inc. All rights reserved.
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


#ifndef	_MACH_DEFAULT_PAGER_TYPES_H_
#define _MACH_DEFAULT_PAGER_TYPES_H_


#include <mach/machine/vm_types.h>

#ifdef MACH_KERNEL_PRIVATE

/*
 *	Remember to update the mig type definitions
 *	in default_pager_types.defs when adding/removing fields.
 */

typedef struct default_pager_info {
	vm_size_t dpi_total_space;	/* size of backing store */
	vm_size_t dpi_free_space;	/* how much of it is unused */
	vm_size_t dpi_page_size;	/* the pager's vm page size */
} default_pager_info_t;

typedef integer_t *backing_store_info_t;
typedef int	backing_store_flavor_t;
typedef int	*vnode_ptr_t;

#define BACKING_STORE_BASIC_INFO	1
#define BACKING_STORE_BASIC_INFO_COUNT \
		(sizeof(struct backing_store_basic_info)/sizeof(integer_t))
struct backing_store_basic_info {
	natural_t	pageout_calls;		/* # pageout calls */
	natural_t	pagein_calls;		/* # pagein calls */
	natural_t	pages_in;		/* # pages paged in (total) */
	natural_t	pages_out;		/* # pages paged out (total) */
	natural_t	pages_unavail;		/* # zero-fill pages */
	natural_t	pages_init;		/* # page init requests */
	natural_t	pages_init_writes;	/* # page init writes */

	natural_t	bs_pages_total;		/* # pages (total) */
	natural_t	bs_pages_free;		/* # unallocated pages */
	natural_t	bs_pages_in;		/* # page read requests */
	natural_t	bs_pages_in_fail;	/* # page read errors */
	natural_t	bs_pages_out;		/* # page write requests */
	natural_t	bs_pages_out_fail;	/* # page write errors */

	integer_t	bs_priority;
	integer_t	bs_clsize;
};
typedef struct backing_store_basic_info	*backing_store_basic_info_t;


typedef struct default_pager_object {
	vm_offset_t dpo_object;		/* object managed by the pager */
	vm_size_t dpo_size;		/* backing store used for the object */
} default_pager_object_t;

typedef default_pager_object_t *default_pager_object_array_t;


typedef struct default_pager_page {
	vm_offset_t dpp_offset;		/* offset of the page in its object */
} default_pager_page_t;

typedef default_pager_page_t *default_pager_page_array_t;

#endif /* MACH_KERNEL_PRIVATE */

#define DEFAULT_PAGER_BACKING_STORE_MAXPRI	4

#define HI_WAT_ALERT	1
#define LO_WAT_ALERT	2

#endif	/* _MACH_DEFAULT_PAGER_TYPES_H_ */

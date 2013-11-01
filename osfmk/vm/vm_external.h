/*
 * Copyright (c) 2000 Apple Computer, Inc. All rights reserved.
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
 * Copyright (c) 1991,1990,1989 Carnegie Mellon University
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

#ifndef	VM_VM_EXTERNAL_H_
#define VM_VM_EXTERNAL_H_

#include <mach/boolean.h>
#include <mach/vm_types.h>
#include <mach/machine/vm_types.h>

/*
 *	External page management hint technology
 *
 *	The data structure exported by this module maintains
 *	a (potentially incomplete) map of the pages written
 *	to external storage for a range of virtual memory.
 */

typedef char	*vm_external_map_t;
#define	VM_EXTERNAL_NULL	((char *) 0)

/*
 *	The states that may be recorded for a page of external storage.
 */

typedef int	vm_external_state_t;
#define	VM_EXTERNAL_STATE_EXISTS		1
#define	VM_EXTERNAL_STATE_UNKNOWN		2
#define	VM_EXTERNAL_STATE_ABSENT		3

/*
 * Useful macros
 */
#define stob(s)	((atop_64((s)) + 07) >> 3)

/*
 *	Routines exported by this module.
 */
					/* Initialize the module */
extern void			vm_external_module_initialize(void);


extern vm_external_map_t	vm_external_create(
					/* Create a vm_external_map_t */
					vm_object_size_t	size);

extern void			vm_external_destroy(
					/* Destroy one */
					vm_external_map_t	map,
					vm_object_size_t	size);

extern vm_object_size_t		vm_external_map_size(
					/* Return size of map in bytes */
					vm_object_size_t	size);

extern void			vm_external_copy(
					/* Copy one into another */
					vm_external_map_t	old_map,
					vm_object_size_t	old_size,
					vm_external_map_t	new_map);

extern void			vm_external_state_set(	
					/* Set state of a page to
					 * VM_EXTERNAL_STATE_EXISTS */
					vm_external_map_t	map,
					vm_object_offset_t	offset);

extern void			vm_external_state_clr(	
					/* clear page state
					 */
					vm_external_map_t	map,
					vm_object_offset_t	offset);

#define	vm_external_state_get(map, offset)			  	\
			(((map) != VM_EXTERNAL_NULL) ? 			\
			  _vm_external_state_get((map), (offset)) :	\
			  VM_EXTERNAL_STATE_UNKNOWN)
					/* Retrieve the state for a
					 * given page, if known.  */

extern vm_external_state_t	_vm_external_state_get(
					/* HIDDEN routine */
					vm_external_map_t	map,
					vm_object_offset_t	offset);

boolean_t			vm_external_within(
					/* Check if new object size
					 * fits in current map */
					vm_object_size_t	new_size, 
					vm_object_size_t	old_size);

#endif	/* VM_VM_EXTERNAL_H_ */

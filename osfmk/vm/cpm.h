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
 * 
 */

#ifndef	_VM_CPM_H_
#define	_VM_CPM_H_

/*
 *	File:	vm/cpm.h
 *	Author:	Alan Langerman
 *	Date:	April 1995 and January 1996
 *
 *	Contiguous physical memory allocator.
 */

#include <mach_kdb.h>
#include <mach_counters.h>

/*
 *	Return a linked list of physically contiguous
 *	wired pages.  Caller is responsible for disposal
 *	via cpm_release.
 *
 *	These pages are all in "gobbled" state when .
 */
extern kern_return_t
	cpm_allocate(vm_size_t size, vm_page_t *list, boolean_t wire);

/*
 *	CPM-specific event counters.
 */
#define	VM_CPM_COUNTERS		(MACH_KDB && MACH_COUNTERS && VM_CPM)
#if	VM_CPM_COUNTERS
#define	cpm_counter(foo)	foo
#else	/* VM_CPM_COUNTERS */
#define	cpm_counter(foo)
#endif	/* VM_CPM_COUNTERS */

#endif	/* _VM_CPM_H_ */

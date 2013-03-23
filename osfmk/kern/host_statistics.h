/*
 * Copyright (c) 2000 Apple Computer, Inc. All rights reserved.
 *
 * @APPLE_LICENSE_HEADER_START@
 * 
 * Copyright (c) 1999-2003 Apple Computer, Inc.  All Rights Reserved.
 * 
 * This file contains Original Code and/or Modifications of Original Code
 * as defined in and that are subject to the Apple Public Source License
 * Version 2.0 (the 'License'). You may not use this file except in
 * compliance with the License. Please obtain a copy of the License at
 * http://www.opensource.apple.com/apsl/ and read it before using this
 * file.
 * 
 * The Original Code and all software distributed under the License are
 * distributed on an 'AS IS' basis, WITHOUT WARRANTY OF ANY KIND, EITHER
 * EXPRESS OR IMPLIED, AND APPLE HEREBY DISCLAIMS ALL SUCH WARRANTIES,
 * INCLUDING WITHOUT LIMITATION, ANY WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE, QUIET ENJOYMENT OR NON-INFRINGEMENT.
 * Please see the License for the specific language governing rights and
 * limitations under the License.
 * 
 * @APPLE_LICENSE_HEADER_END@
 */
/*
 * @OSF_COPYRIGHT@
 */
/*
 *	kern/host_statistics.h
 *
 *	Definitions for host VM/event statistics data structures.
 *
 */

#ifndef	_KERN_HOST_STATISTICS_H_
#define _KERN_HOST_STATISTICS_H_

#include <mach/vm_statistics.h>
#include <kern/cpu_number.h>
#include <kern/sched_prim.h>

extern vm_statistics_data_t	vm_stat[];

#define	VM_STAT(event)							\
MACRO_BEGIN 								\
	mp_disable_preemption();					\
	vm_stat[cpu_number()].event;					\
	mp_enable_preemption();						\
MACRO_END

#endif	/* _KERN_HOST_STATISTICS_H_ */

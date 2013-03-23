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

#ifndef _PPC_MEM_H_
#define _PPC_MEM_H_

#include <mach_kdb.h>
#include <mach_kgdb.h>

#include <ppc/proc_reg.h>
#include <ppc/pmap.h>
#include <ppc/pmap_internals.h>
#include <mach/vm_types.h>

extern vm_offset_t hash_table_base;
extern unsigned int hash_table_size;

void hash_table_init(vm_offset_t base, vm_offset_t size);

#define MAX_BAT		4

typedef struct ppcBAT {
	unsigned int	upper;	/* Upper half of BAT */
	unsigned int	lower;	/* Lower half of BAT */
} ppcBAT;

struct shadowBAT {
	ppcBAT	IBATs[MAX_BAT];	/* Instruction BATs */
	ppcBAT	DBATs[MAX_BAT];	/* Data BAT */
};

extern struct shadowBAT shadow_BAT;     

#endif /* _PPC_MEM_H_ */

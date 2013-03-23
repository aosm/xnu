/*
 * Copyright (c) 2003 Apple Computer, Inc. All rights reserved.
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

#define	ASSEMBLER
#include <sys/appleapiopts.h>
#include <ppc/asm.h>					// EXT, LEXT
#include <machine/cpu_capabilities.h>
#include <machine/commpage.h>

        .text
        .align	2


// *********************************************
// * M A C H _ A B S O L U T E _ T I M E _ 3 2 *
// *********************************************

mach_absolute_time_32:
1:        
        mftbu	r3
        mftb	r4
        mftbu	r5
        cmplw	r3,r5
        beqlr+
        b		1b
        
	COMMPAGE_DESCRIPTOR(mach_absolute_time_32,_COMM_PAGE_ABSOLUTE_TIME,0,k64Bit,kCommPage32)
        
        
// *********************************************
// * M A C H _ A B S O L U T E _ T I M E _ 6 4 *
// *********************************************
//
// This is the version that is called in 32-bit mode, so we return the TBR in r3 and r4.

mach_absolute_time_64:
        mftb	r4
        srdi	r3,r4,32
        blr

	COMMPAGE_DESCRIPTOR(mach_absolute_time_64,_COMM_PAGE_ABSOLUTE_TIME,k64Bit,0,kCommPage32)
        
        
// *************************************************
// * M A C H _ A B S O L U T E _ T I M E _ L P 6 4 *
// *************************************************
//
// This is the version that is called in 64-bit mode, so we return the TBR in r3.

mach_absolute_time_lp64:
        mftb	r3
        blr

	COMMPAGE_DESCRIPTOR(mach_absolute_time_lp64,_COMM_PAGE_ABSOLUTE_TIME,k64Bit,0,kCommPage64)

        

/*
 * Copyright (c) 2003 Apple Computer, Inc. All rights reserved.
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

#define __NO_UNDERSCORES__
#include <i386/asm.h>
#include <assym.s>

Entry(mcount)
        pushl   %ebp            	// setup mcount's frame
        movl    %esp,%ebp
        pushl	%eax			// save %eax
        pushf				// save interrupt state
        cli				// disable interrupts

	//
	// Check that this cpu is ready.
	// This delays the start of mcounting until a cpu is really prepared.
	//
        movl	%gs:CPU_RUNNING,%eax
        testl	%eax,%eax
	jz	1f

	//
	// Test for recursion as indicated by a per-cpu flag.
	// Skip if nested, otherwise set the flag and call the C mount().
	//
        movl	%gs:CPU_MCOUNT_OFF,%eax
        testl	%eax,%eax		// test for recursion
        jnz	1f

        incl	%gs:CPU_MCOUNT_OFF	// set recursion flag

        movl    (%ebp),%eax     	// frame pointer of mcount's caller
        movl    4(%eax),%eax    	// mcount's caller's return address
        pushl   4(%ebp)         	// push selfpc parameter for mcount()
        pushl   %eax            	// push frompc parameter for mcount()
        call	_mcount			// call the C mcount
	addl	$8,%esp			// pop args

        decl	%gs:CPU_MCOUNT_OFF	// turn off recursion flag
1:
        popf				// restore interrupt state
        popl	%eax
        movl    %ebp,%esp       	// tear down mcount's frame
        popl    %ebp
        ret

/*
 * Copyright (c) 1999-2007 Apple Inc. All rights reserved.
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
/* Copyright 1998 Apple Computer, Inc. */

#include "SYS.h"

#if defined(__ppc__) || defined(__ppc64__)

/* This syscall is special cased: the timeval is returned in r3/r4.
 * Note also that the "seconds" field of the timeval is a long, so
 * it's size is mode dependent.
 */
MI_ENTRY_POINT(___gettimeofday)
    mr      r12,r3              // save ptr to timeval
    SYSCALL_NONAME(gettimeofday,0)
	stg     r3,0(r12)           // "stw" in 32-bit mode, "std" in 64-bit mode
	stw     r4,GPR_BYTES(r12)
	li      r3,0
	blr

#elif defined(__i386__)

/*
 *	This syscall is special cased: the timeval is returned in eax/edx.
 */
LABEL(___gettimeofday)
    UNIX_SYSCALL_INT_NONAME(gettimeofday,0)
	mov		4(%esp),%ecx
	mov		%eax,(%ecx)
	mov		%edx,4(%ecx)
	xor		%eax,%eax
	ret

#elif defined(__x86_64__)

/*
 *	This syscall is special cased: the timeval is returned in rax:rdx.
 */
LABEL(___gettimeofday)
    UNIX_SYSCALL_NONAME(gettimeofday,0)
    movq	%rax, (%rdi)
    movl	%edx, 8(%rdi)
    xorl	%eax, %eax
    ret

#elif defined(__arm__)
/*
 * This syscall is special cased: the timeval is returned in r0/r1.
 */
MI_ENTRY_POINT(___gettimeofday)
    mov     r3, r0                  // save ptr to timeval
    SYSCALL_NONAME(gettimeofday,2)
    stmia   r3, { r0, r1 }
    mov     r0, #0
    bx      lr

#else
#error Unsupported architecture
#endif

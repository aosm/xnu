/*
 * Copyright (c) 2006 Apple Computer, Inc. All Rights Reserved.
 * 
 * @APPLE_LICENSE_OSREFERENCE_HEADER_START@
 * 
 * This file contains Original Code and/or Modifications of Original Code 
 * as defined in and that are subject to the Apple Public Source License 
 * Version 2.0 (the 'License'). You may not use this file except in 
 * compliance with the License.  The rights granted to you under the 
 * License may not be used to create, or enable the creation or 
 * redistribution of, unlawful or unlicensed copies of an Apple operating 
 * system, or to circumvent, violate, or enable the circumvention or 
 * violation of, any terms of an Apple operating system software license 
 * agreement.
 *
 * Please obtain a copy of the License at 
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
 * @APPLE_LICENSE_OSREFERENCE_HEADER_END@
 */
/* Copyright (c) 1991,1993 NeXT Computer, Inc.  All rights reserved.
 * 
 *	File:	machdep/ppc/libc/memmove.c
 *	History:
 *
 *	Fixed sleep integration problem. sleep was not properly
 *	handling thread states of THREAD_INTERRUPTED and 
 *	THREAD_MUST_TERMINATE, so callers of sleep were getting
 *	confused and many times looping.  This fixes the (in)famous
 *	unkillable gdb problem, the PB (and other processes) don't
 *	terminate, and more. Removed debugging kprintf left in 
 *	bcopy code
 *
 */

#include <string.h>

#if 0
void *memcpy(void *dst, const void *src, unsigned int ulen)
{
	bcopy(src, dst, ulen);
	return dst;
}
#endif /* 0 */

void *
memmove(void *dst, const void *src, size_t ulen)
{
	bcopy(src, dst, ulen);
	return dst;
}



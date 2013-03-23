/*
 * Copyright (c) 2003-2006 Apple Computer, Inc. All rights reserved.
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

#include <sys/appleapiopts.h>
#include <machine/cpu_capabilities.h>
#include <machine/commpage.h>

#define _PTHREAD_TSD_OFFSET32 0x48
#define _PTHREAD_TSD_OFFSET64 0x60


/* These routines do not need to be on the copmmpage on Intel.  They are for now
 * to avoid revlock, but the code should move to Libc, and we should eventually remove
 * these.
 */
        .text
        .align  2, 0x90

Lpthread_getspecific:
	movl	4(%esp), %eax
	movl	%gs:_PTHREAD_TSD_OFFSET32(,%eax,4), %eax
	ret

	COMMPAGE_DESCRIPTOR(pthread_getspecific,_COMM_PAGE_PTHREAD_GETSPECIFIC,0,0)

Lpthread_self:
	movl	%gs:_PTHREAD_TSD_OFFSET32, %eax
	ret

	COMMPAGE_DESCRIPTOR(pthread_self,_COMM_PAGE_PTHREAD_SELF,0,0)

/* the 64-bit versions: */
	
	.code64
Lpthread_getspecific_64:
	movq	%gs:_PTHREAD_TSD_OFFSET64(,%rdi,8), %rax
	ret

	COMMPAGE_DESCRIPTOR(pthread_getspecific_64,_COMM_PAGE_PTHREAD_GETSPECIFIC,0,0)

Lpthread_self_64:
	movq	%gs:_PTHREAD_TSD_OFFSET64, %rax
	ret

	COMMPAGE_DESCRIPTOR(pthread_self_64,_COMM_PAGE_PTHREAD_SELF,0,0)

/*
 * Copyright (c) 2000-2002 Apple Computer, Inc. All rights reserved.
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
 * Mach Operating System
 * Copyright (c) 1989 Carnegie-Mellon University
 * Copyright (c) 1988 Carnegie-Mellon University
 * Copyright (c) 1987 Carnegie-Mellon University
 * All rights reserved.  The CMU software License Agreement specifies
 * the terms and conditions for use and redistribution.
 */

/*
 *	Codes for Unix software exceptions under EXC_SOFTWARE.
 */

#ifndef	_SYS_UX_EXCEPTION_H_
#define _SYS_UX_EXCEPTION_H_

#include <sys/appleapiopts.h>

#ifdef __APPLE_API_UNSTABLE

#define EXC_UNIX_BAD_SYSCALL	0x10000		/* SIGSYS */

#define EXC_UNIX_BAD_PIPE	0x10001		/* SIGPIPE */

#define EXC_UNIX_ABORT		0x10002		/* SIGABRT */

#endif /* __APPLE_API_UNSTABLE */

#ifdef	KERNEL
#ifdef __APPLE_API_PRIVATE
/*
 *	Kernel data structures for Unix exception handler.
 */

#include <mach/port.h>

extern mach_port_name_t			ux_exception_port;

#endif /* __APPLE_API_PRIVATE */
#endif /* KERNEL */

#endif	/* _SYS_UX_EXCEPTION_H_ */

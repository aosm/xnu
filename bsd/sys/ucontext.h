/*
 * Copyright (c) 2002 Apple Computer, Inc. All rights reserved.
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

#ifndef _SYS_UCONTEXT_H_
#define _SYS_UCONTEXT_H_

#include <machine/ucontext.h>

struct ucontext {
	int		uc_onstack;
	sigset_t	uc_sigmask;	/* signal mask used by this context */
	stack_t 	uc_stack;	/* stack used by this context */
	struct ucontext	*uc_link;	/* pointer to resuming context */
	size_t		uc_mcsize;	/* size of the machine context passed in */
	mcontext_t	uc_mcontext;	/* machine specific context */
};


typedef struct ucontext ucontext_t;

struct ucontext64 {
	int		uc_onstack;
	sigset_t	uc_sigmask;	/* signal mask used by this context */
	stack_t 	uc_stack;	/* stack used by this context */
	struct ucontext	*uc_link;	/* pointer to resuming context */
	size_t		uc_mcsize;	/* size of the machine context passed in */
	mcontext64_t	uc_mcontext64;	/* machine specific context */
};

typedef struct ucontext64 ucontext64_t;

#endif /* _SYS_UCONTEXT_H_ */

/*
 * Copyright (c) 1993-1995, 1999-2000 Apple Computer, Inc.
 * All rights reserved.
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
 * Declarations for timer interrupt callouts.
 *
 * HISTORY
 *
 * 20 December 2000 (debo)
 *	Created.
 */

#ifndef _KERN_TIMER_CALL_H_
#define _KERN_TIMER_CALL_H_

#include <mach/mach_types.h>

#ifdef MACH_KERNEL_PRIVATE

typedef struct call_entry	*timer_call_t;
typedef void				*timer_call_param_t;
typedef void				(*timer_call_func_t)(
									timer_call_param_t		param0,
									timer_call_param_t		param1);

boolean_t
timer_call_enter(
	timer_call_t			call,
	uint64_t				deadline);

boolean_t
timer_call_enter1(
	timer_call_t			call,
	timer_call_param_t		param1,
	uint64_t				deadline);

boolean_t
timer_call_cancel(
	timer_call_t			call);

boolean_t
timer_call_is_delayed(
	timer_call_t			call,
	uint64_t				*deadline);

#include <kern/call_entry.h>

typedef struct call_entry	timer_call_data_t;

void
timer_call_initialize(void);

void
timer_call_setup(
	timer_call_t			call,
	timer_call_func_t		func,
	timer_call_param_t		param0);

#endif /* MACH_KERNEL_PRIVATE */

#endif /* _KERN_TIMER_CALL_H_ */

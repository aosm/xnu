/*
 * Copyright (c) 2003-2007 Apple Inc. All rights reserved.
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

#include <stdint.h>
#include <mach/boolean.h>
#include <mach/mach_types.h>

#include <kern/kern_types.h>
#include <kern/processor.h>
#include <kern/timer_call.h>
#include <kern/thread_call.h>
#include <kern/kalloc.h>
#include <kern/thread.h>

#include <libkern/OSAtomic.h>

#include <machine/machine_routines.h>
#include <machine/cpu_data.h>

#include <chud/chud_xnu.h>
#include <chud/chud_xnu_private.h>

#if 0
#pragma mark **** timer ****
#endif

__private_extern__ chud_timer_t
chudxnu_timer_alloc(chudxnu_timer_callback_func_t func, thread_call_param_t param0)
{
    return (chud_timer_t)thread_call_allocate((thread_call_func_t)func, param0);
}

__private_extern__ kern_return_t
chudxnu_timer_callback_enter(
	chud_timer_t timer,
	thread_call_param_t param1,
	uint32_t time,
	uint32_t units)
{
    uint64_t t_delay;
    clock_interval_to_deadline(time, units, &t_delay);
    thread_call_enter1_delayed((thread_call_t)timer, param1, t_delay);
    return KERN_SUCCESS;
}

__private_extern__ kern_return_t
chudxnu_timer_callback_cancel(chud_timer_t timer)
{
    thread_call_cancel((thread_call_t)timer);
    return KERN_SUCCESS;
}

__private_extern__ kern_return_t
chudxnu_timer_free(chud_timer_t timer)
{
    thread_call_cancel((thread_call_t)timer);
    thread_call_free((thread_call_t)timer);
    return KERN_SUCCESS;
}



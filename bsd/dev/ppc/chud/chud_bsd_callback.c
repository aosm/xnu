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

#include <stdint.h>
#include <mach/boolean.h>
#include <mach/mach_types.h>

#include <sys/syscall.h>
#include <sys/types.h> /* u_int */
#include <sys/proc.h> /* struct proc */
#include <sys/systm.h> /* struct sysent */
#include <sys/sysproto.h>

#pragma mark **** kern debug ****
typedef void (*chudxnu_kdebug_callback_func_t)(uint32_t debugid, uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4);
static chudxnu_kdebug_callback_func_t kdebug_callback_fn = NULL;

extern void kdbg_control_chudxnu(int val, void *fn);
extern unsigned int kdebug_enable;

static void chudxnu_private_kdebug_callback(unsigned int debugid, unsigned int arg0, unsigned int arg1, unsigned int arg2, unsigned int arg3, unsigned int arg4)
{
    if(kdebug_callback_fn) {
        (kdebug_callback_fn)(debugid, arg0, arg1, arg2, arg3, arg4);
    }
}

__private_extern__
kern_return_t chudxnu_kdebug_callback_enter(chudxnu_kdebug_callback_func_t func)
{
    kdebug_callback_fn = func;

    kdbg_control_chud(TRUE, (void *)chudxnu_private_kdebug_callback);
    kdebug_enable |= 0x10;

    return KERN_SUCCESS;
}

__private_extern__
kern_return_t chudxnu_kdebug_callback_cancel(void)
{
    kdebug_callback_fn = NULL;
    kdbg_control_chud(FALSE, NULL);
    kdebug_enable &= ~(0x10);

    return KERN_SUCCESS;
}

/*
 * Copyright (c) 1998-2000 Apple Computer, Inc. All rights reserved.
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
#warning include <dev/evsio.h> is going away use <IOKit/hidsystem/IOHIDTypes.h> instead

#include <sys/appleapiopts.h>

#ifdef __APPLE_API_OBSOLETE
#include <IOKit/hidsystem/IOHIDTypes.h>

/*
 * Identify this driver as one that uses the new driverkit and messaging API
 */
#ifndef _NeXT_MACH_EVENT_DRIVER_
#define _NeXT_MACH_EVENT_DRIVER_	(1)
#endif /* _NeXT_MACH_EVENT_DRIVER_ */

#endif /* __APPLE_API_OBSOLETE */

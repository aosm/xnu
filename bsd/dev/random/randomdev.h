/*
 * Copyright (c) 1999, 2000-2002 Apple Computer, Inc. All rights reserved.
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

#ifndef __DEV_RANDOMDEV_H__
#define __DEV_RANDOMDEV_H__

#include <sys/appleapiopts.h>

#ifdef __APPLE_API_PRIVATE

#include <sys/random.h>

int random_open(dev_t dev, int flags, int devtype, struct proc *pp);
int random_close(dev_t dev, int flags, int mode, struct proc *pp);
int random_read(dev_t dev, struct uio *uio, int ioflag);
int random_write(dev_t dev, struct uio *uio, int ioflag);

u_long RandomULong();

#endif /* __APPLE_API_PRIVATE */
#endif /* __DEV_RANDOMDEV_H__ */


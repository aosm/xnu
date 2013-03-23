/*
 * Copyright (c) 2000 Apple Computer, Inc. All rights reserved.
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
/*	$NetBSD: if_tun.h,v 1.5 1994/06/29 06:36:27 cgd Exp $	*/

/*
 * Copyright (c) 1988, Julian Onions <jpo@cs.nott.ac.uk>
 * Nottingham University 1987.
 *
 * This source may be freely distributed, however I would be interested
 * in any changes that are made.
 *
 * This driver takes packets off the IP i/f and hands them up to a
 * user process to have its wicked way with. This driver has it's
 * roots in a similar driver written by Phil Cockcroft (formerly) at
 * UCL. This driver is based much more on read/write/select mode of
 * operation though.
 *
 */

#ifndef _NET_IF_TUN_H_
#define _NET_IF_TUN_H_
#include <sys/appleapiopts.h>
#ifdef __APPLE_API_PRIVATE

/* Refer to if_tunvar.h for the softc stuff */

/* Maximum transmit packet size (default) */
#define	TUNMTU		1500

/* Maximum receive packet size (hard limit) */
#define	TUNMRU		16384

struct tuninfo {
	int	baudrate;		/* linespeed */
	short	mtu;			/* maximum transmission unit */
	u_char	type;			/* ethernet, tokenring, etc. */
	u_char	dummy;			/* place holder */
};

/* ioctl's for get/set debug */
#define	TUNSDEBUG	_IOW('t', 90, int)
#define	TUNGDEBUG	_IOR('t', 89, int)
#define	TUNSIFINFO	_IOW('t', 91, struct tuninfo)
#define	TUNGIFINFO	_IOR('t', 92, struct tuninfo)

#endif /* __APPLE_API_PRIVATE */
#endif /* !_NET_IF_TUN_H_ */

/*
 * Copyright (c) 2000 Apple Computer, Inc. All rights reserved.
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
/* Copyright (c) 1997, 1998 Apple Computer, Inc. All Rights Reserved */
/*
 *	@(#)ndrv.h	1.1 (MacOSX) 6/10/43
 * Justin Walker - 970604
 */

#ifndef _NET_NDRV_VAR_H
#define _NET_NDRV_VAR_H

/*
 * The cb is plugged into the socket (so_pcb), and the ifnet structure
 *  of BIND is plugged in here.
 * For now, it looks like a raw_cb up front...
 */
struct ndrv_cb
{
    struct ndrv_cb *nd_next;	/* Doubly-linked list */
	struct ndrv_cb *nd_prev;
	struct socket *nd_socket;	/* Back to the socket */
	unsigned int nd_signature;	/* Just double-checking */
	struct sockaddr_ndrv *nd_faddr;
	struct sockaddr_ndrv *nd_laddr;
	struct sockproto nd_proto;	/* proto family, protocol */
	int nd_descrcnt;		/* # elements in nd_dlist - Obsolete */
	TAILQ_HEAD(dlist, dlil_demux_desc) nd_dlist; /* Descr. list */
	struct ifnet *nd_if;
    u_long	nd_send_tag;
    u_long	nd_tag;
    u_long	nd_family;
    short	nd_unit;
};

#define	sotondrvcb(so)		((struct ndrv_cb *)(so)->so_pcb)
#define NDRV_SIGNATURE	0x4e445256 /* "NDRV" */

/* Nominal allocated space for NDRV sockets */
#define NDRVSNDQ	 8192
#define NDRVRCVQ	 8192

#if KERNEL
extern struct ndrv_cb ndrvl;		/* Head of controlblock list */
#endif
#endif	/* _NET_NDRV_VAR_H */

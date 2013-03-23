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
/*
 * @OSF_COPYRIGHT@
 */
/* 
 * Mach Operating System
 * Copyright (c) 1991,1990,1989 Carnegie Mellon University
 * All Rights Reserved.
 * 
 * Permission to use, copy, modify and distribute this software and its
 * documentation is hereby granted, provided that both the copyright
 * notice and this permission notice appear in all copies of the
 * software, derivative works or modified versions, and any portions
 * thereof, and that both notices appear in supporting documentation.
 * 
 * CARNEGIE MELLON ALLOWS FREE USE OF THIS SOFTWARE IN ITS "AS IS"
 * CONDITION.  CARNEGIE MELLON DISCLAIMS ANY LIABILITY OF ANY KIND FOR
 * ANY DAMAGES WHATSOEVER RESULTING FROM THE USE OF THIS SOFTWARE.
 * 
 * Carnegie Mellon requests users of this software to return to
 * 
 *  Software Distribution Coordinator  or  Software.Distribution@CS.CMU.EDU
 *  School of Computer Science
 *  Carnegie Mellon University
 *  Pittsburgh PA 15213-3890
 * 
 * any improvements or extensions that they make and grant Carnegie Mellon
 * the rights to redistribute these changes.
 */
/*
 */
/*
 *	File:	ipc/ipc_pset.h
 *	Author:	Rich Draves
 *	Date:	1989
 *
 *	Definitions for port sets.
 */

#ifndef	_IPC_IPC_PSET_H_
#define _IPC_IPC_PSET_H_

#include <mach/port.h>
#include <mach/kern_return.h>
#include <kern/ipc_kobject.h>
#include <ipc/ipc_object.h>
#include <ipc/ipc_mqueue.h>
#include <kern/thread_pool.h>

#include <mach_kdb.h>

typedef struct ipc_pset {

	/*
	 * Initial sub-structure in common with all ipc_objects.
	 */
	struct ipc_object	ips_object;

	struct ipc_mqueue	ips_messages;
	struct ipc_pset		*ips_pset_self;   /* self (used from ipsm) */
} *ipc_pset_t;

typedef struct ipc_pset_mqueue {
	struct ipc_mqueue	ipsm_messages;
	struct ipc_pset		*ipsm_pset;
} *ipc_pset_mqueue_t;

#define	ips_references		ips_object.io_references
#define ips_local_name		ips_object.io_receiver_name
#define ips_thread_pool		ips_object.io_thread_pool

#define	IPS_NULL		((ipc_pset_t) IO_NULL)

#define	ips_active(pset)	io_active(&(pset)->ips_object)
#define	ips_lock(pset)		io_lock(&(pset)->ips_object)
#define	ips_lock_try(pset)	io_lock_try(&(pset)->ips_object)
#define	ips_unlock(pset)	io_unlock(&(pset)->ips_object)
#define	ips_check_unlock(pset)	io_check_unlock(&(pset)->ips_object)
#define	ips_reference(pset)	io_reference(&(pset)->ips_object)
#define	ips_release(pset)	io_release(&(pset)->ips_object)

/* Allocate a port set */
extern kern_return_t ipc_pset_alloc(
	ipc_space_t		space,
	mach_port_name_t	*namep,
	ipc_pset_t		*psetp);

/* Allocate a port set, with a specific name */
extern kern_return_t ipc_pset_alloc_name(
	ipc_space_t		space,
	mach_port_name_t	name,
	ipc_pset_t		*psetp);

/* Add a port to a port set */
extern kern_return_t ipc_pset_add(
	ipc_pset_t	pset,
	ipc_port_t	port);

/* Remove a port from a port set */
extern kern_return_t ipc_pset_remove(
	ipc_pset_t	pset,
	ipc_port_t	port);

/* Remove a port from all its current port sets */
extern kern_return_t ipc_pset_remove_all(
	ipc_port_t	port);

/* Destroy a port_set */
extern void ipc_pset_destroy(
	ipc_pset_t	pset);

#define	ipc_pset_reference(pset)	\
		ipc_object_reference(&(pset)->ips_object)

#define	ipc_pset_release(pset)		\
		ipc_object_release(&(pset)->ips_object)


#if	MACH_KDB
int ipc_list_count(struct ipc_kmsg *base);
#endif	/* MACH_KDB */

#endif	/* _IPC_IPC_PSET_H_ */

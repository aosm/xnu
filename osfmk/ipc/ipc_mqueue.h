/*
 * Copyright (c) 2000-2004 Apple Computer, Inc. All rights reserved.
 *
 * @APPLE_LICENSE_HEADER_START@
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
 *	File:	ipc/ipc_mqueue.h
 *	Author:	Rich Draves
 *	Date:	1989
 *
 *	Definitions for message queues.
 */

#ifndef	_IPC_IPC_MQUEUE_H_
#define _IPC_IPC_MQUEUE_H_

#include <mach_assert.h>

#include <mach/message.h>

#include <kern/assert.h>
#include <kern/macro_help.h>
#include <kern/kern_types.h>
#include <kern/wait_queue.h>

#include <ipc/ipc_kmsg.h>
#include <ipc/ipc_object.h>
#include <ipc/ipc_types.h>

typedef struct ipc_mqueue {
	union {
		struct {
			struct  wait_queue	wait_queue;	
			struct ipc_kmsg_queue	messages;
			mach_port_msgcount_t	msgcount;
			mach_port_msgcount_t	qlimit;
		 	mach_port_seqno_t 	seqno;
			boolean_t		fullwaiters;
		} port;
		struct wait_queue_set		set_queue;
	} data;
} *ipc_mqueue_t;

#define	IMQ_NULL		((ipc_mqueue_t) 0)

#define imq_wait_queue		data.port.wait_queue
#define imq_messages		data.port.messages
#define imq_msgcount		data.port.msgcount
#define imq_qlimit		data.port.qlimit
#define imq_seqno		data.port.seqno
#define imq_fullwaiters		data.port.fullwaiters

#define imq_set_queue		data.set_queue
#define imq_setlinks		data.set_queue.wqs_setlinks
#define imq_is_set(mq)		wait_queue_is_set(&(mq)->imq_set_queue)

#define	imq_lock(mq)		wait_queue_lock(&(mq)->imq_wait_queue)
#define	imq_lock_try(mq)	wait_queue_lock_try(&(mq)->imq_wait_queue)
#define	imq_unlock(mq)		wait_queue_unlock(&(mq)->imq_wait_queue)
#define imq_held(mq)		wait_queue_held(&(mq)->imq_wait_queue)

#define imq_full(mq)		((mq)->imq_msgcount >= (mq)->imq_qlimit)

extern int ipc_mqueue_full;
extern int ipc_mqueue_rcv;

#define IPC_MQUEUE_FULL		(event64_t)&ipc_mqueue_full
#define IPC_MQUEUE_RECEIVE	(event64_t)&ipc_mqueue_rcv

/*
 * Exported interfaces
 */

/* Initialize a newly-allocated message queue */
extern void ipc_mqueue_init(
	ipc_mqueue_t		mqueue,
	boolean_t		is_set);

/* destroy an mqueue */
extern void ipc_mqueue_destroy(
	ipc_mqueue_t		mqueue);

/* Wake up receivers waiting in a message queue */
extern void ipc_mqueue_changed(
	ipc_mqueue_t		mqueue);

/* Add the specific mqueue as a member of the set */
extern kern_return_t ipc_mqueue_add(
	ipc_mqueue_t		mqueue,
	ipc_mqueue_t	 	set_mqueue);

/* Check to see if mqueue is member of set_mqueue */
extern boolean_t ipc_mqueue_member(
	ipc_mqueue_t		mqueue,
	ipc_mqueue_t		set_mqueue);

/* Remove an mqueue from a specific set */
extern kern_return_t ipc_mqueue_remove(
	ipc_mqueue_t	 	mqueue,
	ipc_mqueue_t		set_mqueue);

/* Remove an mqueue from all sets */
extern void ipc_mqueue_remove_from_all(
	ipc_mqueue_t		mqueue);

/* Remove all the members of the specifiied set */
extern void ipc_mqueue_remove_all(
	ipc_mqueue_t		mqueue);

/* Send a message to a port */
extern mach_msg_return_t ipc_mqueue_send(
	ipc_mqueue_t		mqueue,
	ipc_kmsg_t		kmsg,
	mach_msg_option_t	option,
	mach_msg_timeout_t	timeout_val);

/* Deliver message to message queue or waiting receiver */
extern void ipc_mqueue_post(
	ipc_mqueue_t		mqueue,
	ipc_kmsg_t		kmsg);

/* Receive a message from a message queue */
extern void ipc_mqueue_receive(
	ipc_mqueue_t		mqueue,
	mach_msg_option_t	option,
	mach_msg_size_t		max_size,
	mach_msg_timeout_t	timeout_val,
	int                     interruptible);

/* Continuation routine for message receive */
extern void ipc_mqueue_receive_continue(
	void			*param,
	wait_result_t		wresult);

/* Select a message from a queue and try to post it to ourself */
extern void ipc_mqueue_select(
	ipc_mqueue_t		mqueue,
	mach_msg_option_t	option,
	mach_msg_size_t		max_size);

/* Clear a message count reservation */
extern void ipc_mqueue_release_msgcount(
	ipc_mqueue_t		mqueue);

/* Change a queue limit */
extern void ipc_mqueue_set_qlimit(
	ipc_mqueue_t		mqueue,
	mach_port_msgcount_t	qlimit);

/* Change a queue's sequence number */
extern void ipc_mqueue_set_seqno(
	ipc_mqueue_t		mqueue, 
	mach_port_seqno_t 	seqno);

/* Convert a name in a space to a message queue */
extern mach_msg_return_t ipc_mqueue_copyin(
	ipc_space_t		space,
	mach_port_name_t	name,
	ipc_mqueue_t		*mqueuep,
	ipc_object_t		*objectp);

#endif	/* _IPC_IPC_MQUEUE_H_ */

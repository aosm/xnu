/*
 * Copyright (c) 2000-2007 Apple Inc. All rights reserved.
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
#include <kern/spl.h>
#include <kern/wait_queue.h>

#include <ipc/ipc_kmsg.h>
#include <ipc/ipc_object.h>
#include <ipc/ipc_types.h>

#include <sys/event.h>

typedef struct ipc_mqueue {
	union {
		struct {
			struct  wait_queue	wait_queue;	
			struct ipc_kmsg_queue	messages;
			mach_port_msgcount_t	msgcount;
			mach_port_msgcount_t	qlimit;
			mach_port_seqno_t 	seqno;
			mach_port_name_t	receiver_name;
			boolean_t		fullwaiters;
			natural_t		pset_count;
		} port;
		struct {
			struct wait_queue_set	set_queue;
			mach_port_name_t	local_name;
		} pset;
	} data;
} *ipc_mqueue_t;

#define	IMQ_NULL		((ipc_mqueue_t) 0)

#define imq_wait_queue		data.port.wait_queue
#define imq_messages		data.port.messages
#define imq_msgcount		data.port.msgcount
#define imq_qlimit		data.port.qlimit
#define imq_seqno		data.port.seqno
#define imq_receiver_name	data.port.receiver_name
#define imq_fullwaiters		data.port.fullwaiters
#define imq_pset_count		data.port.pset_count

#define imq_set_queue		data.pset.set_queue
#define imq_setlinks		data.pset.set_queue.wqs_setlinks
#define imq_preposts		data.pset.set_queue.wqs_preposts
#define imq_local_name		data.pset.local_name
#define imq_is_set(mq)		wait_queue_is_set(&(mq)->imq_set_queue)

#define	imq_lock(mq)		wait_queue_lock(&(mq)->imq_wait_queue)
#define	imq_lock_try(mq)	wait_queue_lock_try(&(mq)->imq_wait_queue)
#define	imq_unlock(mq)		wait_queue_unlock(&(mq)->imq_wait_queue)
#define imq_held(mq)		wait_queue_held(&(mq)->imq_wait_queue)

#define imq_full(mq)		((mq)->imq_msgcount >= (mq)->imq_qlimit)
#define imq_full_kernel(mq)	((mq)->imq_msgcount >= MACH_PORT_QLIMIT_KERNEL)

extern int ipc_mqueue_full;
// extern int ipc_mqueue_rcv;

#define IPC_MQUEUE_FULL		CAST_EVENT64_T(&ipc_mqueue_full)
#define IPC_MQUEUE_RECEIVE	NO_EVENT64

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
	ipc_mqueue_t	 	set_mqueue,
	wait_queue_link_t	wql);

/* Check to see if mqueue is member of set_mqueue */
extern boolean_t ipc_mqueue_member(
	ipc_mqueue_t		mqueue,
	ipc_mqueue_t		set_mqueue);

/* Remove an mqueue from a specific set */
extern kern_return_t ipc_mqueue_remove(
	ipc_mqueue_t	 	mqueue,
	ipc_mqueue_t		set_mqueue,
	wait_queue_link_t 	*wqlp);

/* Remove an mqueue from all sets */
extern void ipc_mqueue_remove_from_all(
	ipc_mqueue_t		mqueue,
	queue_t 		links);

/* Remove all the members of the specifiied set */
extern void ipc_mqueue_remove_all(
	ipc_mqueue_t		mqueue,
	queue_t			links);

/* Send a message to a port */
extern mach_msg_return_t ipc_mqueue_send(
	ipc_mqueue_t		mqueue,
	ipc_kmsg_t		kmsg,
	mach_msg_option_t	option,
	mach_msg_timeout_t	timeout_val,
	spl_t			s);

/* check for queue send queue full of a port */
extern mach_msg_return_t ipc_mqueue_preflight_send(
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

/* Receive a message from a message queue using a specified thread */
extern wait_result_t ipc_mqueue_receive_on_thread(
        ipc_mqueue_t            mqueue,
	mach_msg_option_t       option,
	mach_msg_size_t         max_size,
	mach_msg_timeout_t      rcv_timeout,
	int                     interruptible,
	thread_t                thread);

/* Continuation routine for message receive */
extern void ipc_mqueue_receive_continue(
	void			*param,
	wait_result_t		wresult);

/* Select a message from a queue and try to post it to ourself */
extern void ipc_mqueue_select_on_thread(
	ipc_mqueue_t		mqueue,
	mach_msg_option_t	option,
	mach_msg_size_t		max_size,
	thread_t                thread);

/* Peek into a messaqe queue to see if there are messages */
extern unsigned ipc_mqueue_peek(
	ipc_mqueue_t		mqueue,
	mach_port_seqno_t	*msg_seqnop,
	mach_msg_size_t		*msg_sizep,
	mach_msg_id_t		*msg_idp,
	mach_msg_max_trailer_t	*msg_trailerp);

/* Peek into a messaqe queue set to see if there are queues with messages */
extern unsigned ipc_mqueue_set_peek(
	ipc_mqueue_t		mqueue);

/* Gather the names of member port for a given set */
extern void ipc_mqueue_set_gather_member_names(
	ipc_mqueue_t		mqueue,
	ipc_entry_num_t		maxnames,
	mach_port_name_t	*names,
	ipc_entry_num_t		*actualp);

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

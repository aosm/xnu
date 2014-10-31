/*
 * Copyright (c) 2012-2013 Apple Inc. All rights reserved.
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

#ifndef _ATM_ATM_INTERNAL_H_
#define _ATM_ATM_INTERNAL_H_

#include <stdint.h>
#include <mach/mach_types.h>
#include <atm/atm_types.h>

#ifdef	MACH_KERNEL_PRIVATE

#include <kern/thread.h>
#include <kern/locks.h>
#include <kern/queue.h>
#include <ipc/ipc_voucher.h>

/* Flags for atm task descriptor */
#define ATM_TASK_DEAD             0x1

/* Default value for Voucher Attribute Manager for ATM */
#define VAM_DEFAULT_VALUE NULL

typedef mach_voucher_attr_value_handle_t atm_voucher_id_t;

struct atm_task_descriptor {
	decl_lck_mtx_data(,lock)             /* lock to protect reference count */
	mach_port_t     trace_buffer;		 /* named memory entry registered by user */
	uint64_t        trace_buffer_size;   /* size of the trace_buffer registered */
	uint64_t        mailbox_array_size;	 /* Mailbox array size in bytes. */
	void *          mailbox_kernel_addr; /* Kernel address where the mailbox is mapped. */
	uint32_t        reference_count:31,
	                flags:1;
#if DEVELOPMENT || DEBUG
	task_t          task;           /* task pointer for debugging purposes */
	queue_chain_t   descriptor_elt; /* global chain of all descriptors */
#endif
};

typedef struct atm_task_descriptor *atm_task_descriptor_t;
#define ATM_TASK_DESCRIPTOR_NULL NULL

struct atm_value {
	aid_t 		 aid;			/* activity id */
	queue_head_t 	 listeners;		/* List of listeners who register for this activity */
	decl_lck_mtx_data( ,listener_lock)	/* Lock to protect listener list */
	queue_chain_t 	 vid_hash_elt;		/* Next hash element in the global hash table */
#if DEVELOPMENT || DEBUG
	queue_chain_t	 value_elt;	 	/* global chain of all values */
#endif
	uint32_t	 sync;			/* Made ref count given to voucher sub system. */
	uint32_t	 listener_count;	/* Number of Listerners listening on the value. */
	int32_t		 reference_count;	/* use count on the atm value, 1 taken by the global hash table */	
};

typedef struct atm_value *atm_value_t;
#define ATM_VALUE_NULL NULL

/* Flags for atm link objects */
#define ATM_LINK_REMOVE             0x1

struct atm_link_object {
	atm_task_descriptor_t  descriptor;
	void *                 mailbox;		     /* Offset in the mailbox registered by the user for an activity. */
	uint32_t                reference_count;     /* Refernece count for link object */
	uint8_t                flags;		     /* Flags used mark for deletion from the listener list */
	queue_chain_t	       listeners_element;    /* Head is atm_value->listeners. */
};

typedef struct atm_link_object *atm_link_object_t;

#define atm_link_object_reference_internal(elem)	\
	(hw_atomic_add(&(elem)->reference_count, 1))

#define atm_link_object_release_internal(elem)	\
	(hw_atomic_sub(&(elem)->reference_count, 1))

struct atm_value_hash {
	queue_head_t	hash_list;
	decl_lck_mtx_data(, hash_list_lock)	/* lock to protect bucket list. */
};

typedef struct atm_value_hash *atm_value_hash_t;

void atm_init(void);
void atm_task_descriptor_destroy(atm_task_descriptor_t task_descriptor);
kern_return_t atm_register_trace_memory(task_t	task, uint64_t trace_buffer_address, uint64_t buffer_size, uint64_t mailbox_array_size);
kern_return_t atm_send_proc_inspect_notification(task_t task, int32_t traced_pid, uint64_t traced_uniqueid);

#endif /* MACH_KERNEL_PRIVATE */

#endif /* _ATM_ATM_INTERNAL_H_ */

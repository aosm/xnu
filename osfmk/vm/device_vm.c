/*
 * Copyright (c) 2000-2001 Apple Computer, Inc. All rights reserved.
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

#include <sys/errno.h>
#include <kern/host.h>
#include <mach/mach_types.h>
#include <vm/vm_map.h>
#include <vm/vm_kern.h>
#include <vm/vm_pageout.h>
#include <mach/kern_return.h>
#include <mach/memory_object_types.h>
#include <mach/port.h>
#include <mach/policy.h>
#include <ipc/ipc_port.h>
#include <ipc/ipc_space.h>
#include <kern/thread.h>
#include <device/device_port.h>
#include <vm/vm_pageout.h>

/* Device VM COMPONENT INTERFACES */


/* 
 * Device PAGER 
 */


/* until component support available */



/* until component support available */
int	device_pager_workaround;

typedef int device_port_t;

typedef struct device_pager {
	int 		*pager;		/* pager workaround pointer	*/
	unsigned int	pager_ikot;	/* fake ip_kotype() 		*/
	unsigned int	ref_count;	/* reference count		*/
	memory_object_control_t	control_handle;	/* mem object's cntrl handle */
	device_port_t   device_handle;  /* device_handle */
	vm_size_t	size;
	int		flags;
} *device_pager_t;



void 
device_pager_bootstrap(
	void);


memory_object_t
device_pager_setup(
	memory_object_t,
	int,
	vm_size_t,
	int);

device_pager_t
device_pager_lookup(
	memory_object_t);

kern_return_t
device_pager_init(
	memory_object_t, 
	memory_object_control_t, 
	vm_size_t);


kern_return_t
device_pager_data_request( 
	memory_object_t, 
	memory_object_offset_t,
	vm_size_t, 
	vm_prot_t);

kern_return_t
device_pager_data_return(
	memory_object_t,
	memory_object_offset_t,
	vm_size_t,
	boolean_t,
	boolean_t);

void
device_pager_reference(
	memory_object_t);

void
device_pager_deallocate(
	memory_object_t);

kern_return_t
device_pager_data_initialize(
	memory_object_t,
	memory_object_offset_t,
	vm_size_t);

kern_return_t
device_pager_data_unlock(
	memory_object_t,
	memory_object_offset_t,
	vm_size_t,
	vm_prot_t);

kern_return_t
device_pager_terminate(
	memory_object_t);

kern_return_t
device_pager_synchronize(
	memory_object_t,
	memory_object_offset_t,
	vm_offset_t,
	vm_sync_t);

kern_return_t
device_pager_unmap(
	memory_object_t);

device_pager_t
device_object_create(void);

zone_t	device_pager_zone;


#define	DEVICE_PAGER_NULL	((device_pager_t) 0)


#define	MAX_DNODE		10000





/*
 *
 */
void
device_pager_bootstrap(void)
{
	register vm_size_t      size;

	size = (vm_size_t) sizeof(struct device_pager);
	device_pager_zone = zinit(size, (vm_size_t) MAX_DNODE*size,
				PAGE_SIZE, "device node pager structures");

	return;
}

/*
 *
 */
memory_object_t
device_pager_setup(
	memory_object_t	device,
	int		device_handle,
	vm_size_t	size,
	int		flags)
{
	device_pager_t	device_object;

	device_object = device_object_create();
	if (device_object == DEVICE_PAGER_NULL)
		panic("device_pager_setup: device_object_create() failed");

	device_object->device_handle = device_handle;
	device_object->size = size;
	device_object->flags = flags;

	return((memory_object_t)device_object);
}

/*
 *
 */
kern_return_t
device_pager_populate_object(
	memory_object_t		device,
	memory_object_offset_t	offset,
	vm_offset_t		phys_addr,
	vm_size_t		size)
{
	device_pager_t	device_object;
	vm_object_t	vm_object;
	kern_return_t	kr;
	upl_t		upl;
	ipc_port_t	previous;

	device_object = device_pager_lookup(device);
	if(device_object == DEVICE_PAGER_NULL)
		return KERN_FAILURE;

	vm_object = (vm_object_t)memory_object_control_to_vm_object(
					device_object->control_handle);
	if(vm_object == NULL) 
		return KERN_FAILURE;

	kr =  vm_object_populate_with_private(
				vm_object, offset, phys_addr, size);
	if(kr != KERN_SUCCESS)
		return kr;

	if(!vm_object->phys_contiguous) {
		int null_size = 0;
        	kr = vm_object_upl_request(vm_object,
             		(vm_object_offset_t)offset, size, &upl,  NULL,
			&null_size, (UPL_NO_SYNC | UPL_CLEAN_IN_PLACE)); 

		if(kr != KERN_SUCCESS)
			panic("device_pager_populate_object: list_req failed");

		upl_commit(upl, NULL);
		upl_deallocate(upl);
	}


	return kr;
}

/*
 *
 */
device_pager_t
device_pager_lookup(
	memory_object_t	name)
{
	device_pager_t	device_object;

	device_object = (device_pager_t)name;
	assert(device_object->pager == &device_pager_workaround);
	return (device_object);
}

/*
 *
 */
kern_return_t
device_pager_init(memory_object_t mem_obj, 
		memory_object_control_t control, 
		vm_size_t pg_size)
{
	device_pager_t   device_object;
	kern_return_t   kr;
	memory_object_attr_info_data_t  attributes;

	vm_object_t	vm_object;


	if (control == MEMORY_OBJECT_CONTROL_NULL)
		return KERN_INVALID_ARGUMENT;

	device_object = device_pager_lookup(mem_obj);

	memory_object_control_reference(control);
	device_object->control_handle = control;


/* The following settings should be done through an expanded change */
/* attributes call */

	vm_object = (vm_object_t)memory_object_control_to_vm_object(control);
	vm_object_lock(vm_object);
	vm_object->private = TRUE;
	if(device_object->flags & DEVICE_PAGER_CONTIGUOUS)
		vm_object->phys_contiguous = TRUE;
	if(device_object->flags & DEVICE_PAGER_NOPHYSCACHE)
		vm_object->nophyscache = TRUE;

	vm_object->wimg_bits = device_object->flags & VM_WIMG_MASK;
	vm_object_unlock(vm_object);


	attributes.copy_strategy = MEMORY_OBJECT_COPY_DELAY;
	/* attributes.cluster_size = (1 << (CLUSTER_SHIFT + PAGE_SHIFT));*/
	attributes.cluster_size = (1 << (PAGE_SHIFT));
	attributes.may_cache_object = FALSE;
	attributes.temporary = TRUE;

	kr = memory_object_change_attributes(
					control,
					MEMORY_OBJECT_ATTRIBUTE_INFO,
					(memory_object_info_t) &attributes,
					MEMORY_OBJECT_ATTR_INFO_COUNT);
	if (kr != KERN_SUCCESS)
		panic("device_pager_init: memory_object_change_attributes() failed");

	return(KERN_SUCCESS);
}

/*
 *
 */
kern_return_t
device_pager_data_return(
        memory_object_t		mem_obj,
        memory_object_offset_t	offset,
        vm_size_t		data_cnt,
        boolean_t		dirty,
        boolean_t		kernel_copy)  
{
	device_pager_t	device_object;

	device_object = device_pager_lookup(mem_obj);
	if (device_object == DEVICE_PAGER_NULL)
		panic("device_pager_data_return: lookup failed");

	return device_data_action(device_object->device_handle, device_object,
			VM_PROT_READ | VM_PROT_WRITE, offset, data_cnt);
}

/*
 *
 */
kern_return_t	
device_pager_data_request(
	memory_object_t		mem_obj,
	memory_object_offset_t	offset,
	vm_size_t		length,
	vm_prot_t		protection_required)
{
	device_pager_t	device_object;

	device_object = device_pager_lookup(mem_obj);

	if (device_object == DEVICE_PAGER_NULL)
		panic("device_pager_data_request: lookup failed");

	device_data_action(device_object->device_handle, device_object,
					VM_PROT_READ, offset, length);
	return KERN_SUCCESS;
}

/*
 *
 */
void
device_pager_reference(
	memory_object_t		mem_obj)
{	
	device_pager_t		device_object;
	unsigned int		new_ref_count;

	device_object = device_pager_lookup(mem_obj);
	new_ref_count = hw_atomic_add(&device_object->ref_count, 1);
	assert(new_ref_count > 1);
}

/*
 *
 */
void
device_pager_deallocate(
	memory_object_t		mem_obj)
{
	device_pager_t	device_object;

	device_object = device_pager_lookup(mem_obj);

	if (hw_atomic_sub(&device_object->ref_count, 1) == 0) {
		if (device_object->device_handle != (device_port_t) NULL) {
			device_close(device_object->device_handle);
		}
		zfree(device_pager_zone, (vm_offset_t) device_object);
	}
	return;
}

kern_return_t
device_pager_data_initialize(
        memory_object_t		mem_obj,
        memory_object_offset_t	offset,
        vm_size_t		data_cnt)
{
	return KERN_FAILURE;
}

kern_return_t
device_pager_data_unlock(
	memory_object_t		mem_obj,
	memory_object_offset_t	offset,
	vm_size_t		size,
	vm_prot_t		desired_access)
{
	return KERN_FAILURE;
}

device_pager_terminate(
	memory_object_t	mem_obj)
{
	return KERN_SUCCESS;
}



/*
 *
 */
kern_return_t
device_pager_synchronize(
	memory_object_t		mem_obj,
	memory_object_offset_t	offset,
	vm_offset_t		length,
	vm_sync_t		sync_flags)
{
	device_pager_t	device_object;

	device_object = device_pager_lookup(mem_obj);

	memory_object_synchronize_completed(
			device_object->control_handle, offset, length);

	return KERN_SUCCESS;
}

/*
 *
 */
kern_return_t
device_pager_unmap(
	memory_object_t	mem_obj)
{
	return KERN_SUCCESS;
}



/*
 *
 */
device_pager_t
device_object_create()
{
	register device_pager_t  device_object;

	device_object = (struct device_pager *) zalloc(device_pager_zone);
	if (device_object == DEVICE_PAGER_NULL)
		return(DEVICE_PAGER_NULL);
	device_object->pager = &device_pager_workaround;
	device_object->pager_ikot = IKOT_MEMORY_OBJECT;
	device_object->ref_count = 1;
	device_object->control_handle = MEMORY_OBJECT_CONTROL_NULL;


	return(device_object);
}


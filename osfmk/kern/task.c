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
 * @OSF_FREE_COPYRIGHT@
 */
/* 
 * Mach Operating System
 * Copyright (c) 1991,1990,1989,1988 Carnegie Mellon University
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
 *	File:	kern/task.c
 *	Author:	Avadis Tevanian, Jr., Michael Wayne Young, David Golub,
 *		David Black
 *
 *	Task management primitives implementation.
 */
/*
 * Copyright (c) 1993 The University of Utah and
 * the Computer Systems Laboratory (CSL).  All rights reserved.
 *
 * Permission to use, copy, modify and distribute this software and its
 * documentation is hereby granted, provided that both the copyright
 * notice and this permission notice appear in all copies of the
 * software, derivative works or modified versions, and any portions
 * thereof, and that both notices appear in supporting documentation.
 *
 * THE UNIVERSITY OF UTAH AND CSL ALLOW FREE USE OF THIS SOFTWARE IN ITS "AS
 * IS" CONDITION.  THE UNIVERSITY OF UTAH AND CSL DISCLAIM ANY LIABILITY OF
 * ANY KIND FOR ANY DAMAGES WHATSOEVER RESULTING FROM THE USE OF THIS SOFTWARE.
 *
 * CSL requests users of this software to return to csl-dist@cs.utah.edu any
 * improvements that they make and grant CSL redistribution rights.
 *
 */

#include <mach_kdb.h>
#include <mach_host.h>
#include <mach_prof.h>
#include <fast_tas.h>
#include <task_swapper.h>
#include <platforms.h>

#include <mach/boolean.h>
#include <mach/machine/vm_types.h>
#include <mach/vm_param.h>
#include <mach/semaphore.h>
#include <mach/task_info.h>
#include <mach/task_special_ports.h>
#include <mach/mach_types.h>
#include <mach/machine/rpc.h>
#include <ipc/ipc_space.h>
#include <ipc/ipc_entry.h>
#include <kern/mach_param.h>
#include <kern/misc_protos.h>
#include <kern/task.h>
#include <kern/thread.h>
#include <kern/zalloc.h>
#include <kern/kalloc.h>
#include <kern/processor.h>
#include <kern/sched_prim.h>	/* for thread_wakeup */
#include <kern/sf.h>
#include <kern/mk_sp.h>	/*** ??? fix so this can be removed ***/
#include <kern/ipc_tt.h>
#include <kern/ledger.h>
#include <kern/host.h>
#include <vm/vm_kern.h>		/* for kernel_map, ipc_kernel_map */
#include <kern/profile.h>
#include <kern/assert.h>
#include <kern/sync_lock.h>
#if	MACH_KDB
#include <ddb/db_sym.h>
#endif	/* MACH_KDB */

#if	TASK_SWAPPER
#include <kern/task_swap.h>
#endif	/* TASK_SWAPPER */

/*
 * Exported interfaces
 */

#include <mach/task_server.h>
#include <mach/mach_host_server.h>
#include <mach/host_security_server.h>

task_t	kernel_task;
zone_t	task_zone;

/* Forwards */

void		task_hold_locked(
			task_t		task);
void		task_wait_locked(
			task_t		task);
void		task_release_locked(
			task_t		task);
void		task_collect_scan(void);
void		task_free(
			task_t		task );
void		task_synchronizer_destroy_all(
			task_t		task);
void		task_subsystem_destroy_all(
			task_t		task);

kern_return_t	task_set_ledger(
			task_t		task,
			ledger_t	wired,
			ledger_t	paged);

void
task_init(void)
{
	task_zone = zinit(
			sizeof(struct task),
			TASK_MAX * sizeof(struct task),
			TASK_CHUNK * sizeof(struct task),
			"tasks");

	eml_init();

	/*
	 * Create the kernel task as the first task.
	 * Task_create_local must assign to kernel_task as a side effect,
	 * for other initialization. (:-()
	 */
	if (task_create_local(
			TASK_NULL, FALSE, FALSE, &kernel_task) != KERN_SUCCESS)
		panic("task_init\n");
	vm_map_deallocate(kernel_task->map);
	kernel_task->map = kernel_map;

#if	MACH_ASSERT
	if (watchacts & WA_TASK)
	    printf("task_init: kernel_task = %x map=%x\n",
				kernel_task, kernel_map);
#endif	/* MACH_ASSERT */
}

#if	MACH_HOST
void
task_freeze(
	task_t task)
{
	task_lock(task);
	/*
	 *	If may_assign is false, task is already being assigned,
	 *	wait for that to finish.
	 */
	while (task->may_assign == FALSE) {
		task->assign_active = TRUE;
		thread_sleep_mutex((event_t) &task->assign_active,
					&task->lock, THREAD_INTERRUPTIBLE);
		task_lock(task);
	}
	task->may_assign = FALSE;
	task_unlock(task);

	return;
}

void
task_unfreeze(
	task_t task)
{
	task_lock(task);
	assert(task->may_assign == FALSE);
	task->may_assign = TRUE;
	if (task->assign_active == TRUE) {
		task->assign_active = FALSE;
		thread_wakeup((event_t)&task->assign_active);
	}
	task_unlock(task);

	return;
}
#endif	/* MACH_HOST */

/*
 * Create a task running in the kernel address space.  It may
 * have its own map of size mem_size and may have ipc privileges.
 */
kern_return_t
kernel_task_create(
	task_t			parent_task,
	vm_offset_t		map_base,
	vm_size_t		map_size,
	task_t			*child_task)
{
	kern_return_t		result;
	task_t			new_task;
	vm_map_t		old_map;

	/*
	 * Create the task.
	 */
	result = task_create_local(parent_task, FALSE, TRUE, &new_task);
	if (result != KERN_SUCCESS)
		return (result);

	/*
	 * Task_create_local creates the task with a user-space map.
	 * We attempt to replace the map and free it afterwards; else
	 * task_deallocate will free it (can NOT set map to null before
	 * task_deallocate, this impersonates a norma placeholder task).
	 * _Mark the memory as pageable_ -- this is what we
	 * want for images (like servers) loaded into the kernel.
	 */
	if (map_size == 0) {
		vm_map_deallocate(new_task->map);
		new_task->map = kernel_map;
		*child_task = new_task;
	} else {
		old_map = new_task->map;
		if ((result = kmem_suballoc(kernel_map, &map_base,
					    map_size, TRUE, FALSE,
					    &new_task->map)) != KERN_SUCCESS) {
			/*
			 * New task created with ref count of 2 -- decrement by
			 * one to force task deletion.
			 */
			printf("kmem_suballoc(%x,%x,%x,1,0,&new) Fails\n",
			       kernel_map, map_base, map_size);
			--new_task->ref_count;
			task_deallocate(new_task);
			return (result);
		}
		vm_map_deallocate(old_map);
		*child_task = new_task;
	}
	return (KERN_SUCCESS);
}

kern_return_t
task_create(
	task_t			parent_task,
        ledger_port_array_t	ledger_ports,
        mach_msg_type_number_t	num_ledger_ports,
	boolean_t		inherit_memory,
	task_t			*child_task)		/* OUT */
{
	if (parent_task == TASK_NULL)
		return(KERN_INVALID_ARGUMENT);

	return task_create_local(
	    		parent_task, inherit_memory, FALSE, child_task);
}

kern_return_t
host_security_create_task_token(
        host_security_t		host_security,
	task_t			parent_task,
        security_token_t	sec_token,
	host_priv_t		host_priv,
        ledger_port_array_t	ledger_ports,
        mach_msg_type_number_t	num_ledger_ports,
	boolean_t		inherit_memory,
	task_t			*child_task)		/* OUT */
{
        kern_return_t		result;
        
	if (parent_task == TASK_NULL)
		return(KERN_INVALID_ARGUMENT);

	if (host_security == HOST_NULL)
		return(KERN_INVALID_SECURITY);

	result = task_create_local(
			parent_task, inherit_memory, FALSE, child_task);

        if (result != KERN_SUCCESS)
                return(result);

	result = host_security_set_task_token(host_security,
					      *child_task,
					      sec_token,
					      host_priv);

	if (result != KERN_SUCCESS)
		return(result);

	return(result);
}

kern_return_t
task_create_local(
	task_t		parent_task,
	boolean_t	inherit_memory,
	boolean_t	kernel_loaded,
	task_t		*child_task)		/* OUT */
{
	task_t		new_task;
	processor_set_t	pset;

	new_task = (task_t) zalloc(task_zone);

	if (new_task == TASK_NULL)
		return(KERN_RESOURCE_SHORTAGE);

	/* one ref for just being alive; one for our caller */
	new_task->ref_count = 2;

	if (inherit_memory)
		new_task->map = vm_map_fork(parent_task->map);
	else
		new_task->map = vm_map_create(pmap_create(0),
					round_page(VM_MIN_ADDRESS),
					trunc_page(VM_MAX_ADDRESS), TRUE);

	mutex_init(&new_task->lock, ETAP_THREAD_TASK_NEW);
	queue_init(&new_task->subsystem_list);
	queue_init(&new_task->thr_acts);
	new_task->suspend_count = 0;
	new_task->thr_act_count = 0;
	new_task->res_act_count = 0;
	new_task->active_act_count = 0;
	new_task->user_stop_count = 0;
	new_task->importance = 0;
	new_task->active = TRUE;
	new_task->kernel_loaded = kernel_loaded;
	new_task->user_data = 0;
	new_task->faults = 0;
	new_task->cow_faults = 0;
	new_task->pageins = 0;
	new_task->messages_sent = 0;
	new_task->messages_received = 0;
	new_task->syscalls_mach = 0;
	new_task->syscalls_unix=0;
	new_task->csw=0;

#ifdef MACH_BSD
	new_task->bsd_info = 0;
#endif /* MACH_BSD */

#if	TASK_SWAPPER
	new_task->swap_state = TASK_SW_IN;
	new_task->swap_flags = 0;
	new_task->swap_ast_waiting = 0;
	new_task->swap_stamp = sched_tick;
	new_task->swap_rss = 0;
	new_task->swap_nswap = 0;
#endif	/* TASK_SWAPPER */

	queue_init(&new_task->semaphore_list);
	queue_init(&new_task->lock_set_list);
	new_task->semaphores_owned = 0;
	new_task->lock_sets_owned = 0;

#if	MACH_HOST
	new_task->may_assign = TRUE;
	new_task->assign_active = FALSE;
#endif	/* MACH_HOST */
	eml_task_reference(new_task, parent_task);

	ipc_task_init(new_task, parent_task);

	new_task->total_user_time.seconds = 0;
	new_task->total_user_time.microseconds = 0;
	new_task->total_system_time.seconds = 0;
	new_task->total_system_time.microseconds = 0;

	task_prof_init(new_task);

	if (parent_task != TASK_NULL) {
#if	MACH_HOST
		/*
		 * Freeze the parent, so that parent_task->processor_set
		 * cannot change.
		 */
		task_freeze(parent_task);
#endif	/* MACH_HOST */
		pset = parent_task->processor_set;
		if (!pset->active)
			pset = &default_pset;

		new_task->policy = parent_task->policy;

		new_task->priority = parent_task->priority;
		new_task->max_priority = parent_task->max_priority;

		new_task->sec_token = parent_task->sec_token;

		shared_region_mapping_ref(parent_task->system_shared_region);
		new_task->system_shared_region = parent_task->system_shared_region;

		new_task->wired_ledger_port = ledger_copy(
			convert_port_to_ledger(parent_task->wired_ledger_port));
		new_task->paged_ledger_port = ledger_copy(
			convert_port_to_ledger(parent_task->paged_ledger_port));
	}
	else {
		pset = &default_pset;

		if (kernel_task == TASK_NULL) {
			new_task->policy = POLICY_RR;

			new_task->priority = MINPRI_KERNBAND;
			new_task->max_priority = MAXPRI_KERNBAND;
		}
		else {
			new_task->policy = POLICY_TIMESHARE;

			new_task->priority = BASEPRI_DEFAULT;
			new_task->max_priority = MAXPRI_HIGHBAND;
		}

		new_task->sec_token = KERNEL_SECURITY_TOKEN;
		new_task->wired_ledger_port = ledger_copy(root_wired_ledger);
		new_task->paged_ledger_port = ledger_copy(root_paged_ledger);
	}

	pset_lock(pset);
	pset_add_task(pset, new_task);
	pset_unlock(pset);
#if	MACH_HOST
	if (parent_task != TASK_NULL)
		task_unfreeze(parent_task);
#endif	/* MACH_HOST */

#if	FAST_TAS
 	if (inherit_memory) {
 		new_task->fast_tas_base = parent_task->fast_tas_base;
 		new_task->fast_tas_end  = parent_task->fast_tas_end;
 	} else {
 		new_task->fast_tas_base = (vm_offset_t)0;
 		new_task->fast_tas_end  = (vm_offset_t)0;
 	}
#endif	/* FAST_TAS */

	ipc_task_enable(new_task);

#if	TASK_SWAPPER
	task_swapout_eligible(new_task);
#endif	/* TASK_SWAPPER */

#if	MACH_ASSERT
	if (watchacts & WA_TASK)
	    printf("*** task_create_local(par=%x inh=%x) == 0x%x\n",
			parent_task, inherit_memory, new_task);
#endif	/* MACH_ASSERT */

	*child_task = new_task;
	return(KERN_SUCCESS);
}

/*
 *	task_free:
 *
 *	Called by task_deallocate when the task's reference count drops to zero.
 *	Task is locked.
 */
void
task_free(
	task_t		task)
{
	processor_set_t	pset;

#if	MACH_ASSERT
	assert(task != 0);
	if (watchacts & (WA_EXIT|WA_TASK))
	    printf("task_free(%x(%d)) map ref %d\n", task, task->ref_count,
			task->map->ref_count);
#endif	/* MACH_ASSERT */

#if	TASK_SWAPPER
	/* task_terminate guarantees that this task is off the list */
	assert((task->swap_state & TASK_SW_ELIGIBLE) == 0);
#endif	/* TASK_SWAPPER */

	eml_task_deallocate(task);

	/*
	 * Temporarily restore the reference we dropped above, then
	 * freeze the task so that the task->processor_set field
	 * cannot change. In the !MACH_HOST case, the logic can be
	 * simplified, since the default_pset is the only pset.
	 */
	++task->ref_count;
	task_unlock(task);
#if	MACH_HOST
	task_freeze(task);
#endif	/* MACH_HOST */
	
	pset = task->processor_set;
	pset_lock(pset);
	task_lock(task);
	if (--task->ref_count > 0) {
		/*
		 * A new reference appeared (probably from the pset).
		 * Back out. Must unfreeze inline since we'already
		 * dropped our reference.
		 */
#if	MACH_HOST
		assert(task->may_assign == FALSE);
		task->may_assign = TRUE;
		if (task->assign_active == TRUE) {
			task->assign_active = FALSE;
			thread_wakeup((event_t)&task->assign_active);
		}
#endif	/* MACH_HOST */
		task_unlock(task);
		pset_unlock(pset);
		return;
	}
	pset_remove_task(pset,task);
	task_unlock(task);
	pset_unlock(pset);
	pset_deallocate(pset);

	ipc_task_terminate(task);
	shared_region_mapping_dealloc(task->system_shared_region);

	if (task->kernel_loaded)
	    vm_map_remove(kernel_map, task->map->min_offset,
			  task->map->max_offset, VM_MAP_NO_FLAGS);
	vm_map_deallocate(task->map);
	is_release(task->itk_space);
	task_prof_deallocate(task);
	zfree(task_zone, (vm_offset_t) task);
}

void
task_deallocate(
	task_t		task)
{
	if (task != TASK_NULL) {
		int	c;

		task_lock(task);
		c = --task->ref_count;
		if (c == 0)
		    task_free(task);	/* unlocks task */
		else
		    task_unlock(task);
	}
}

void
task_reference(
	task_t		task)
{
	if (task != TASK_NULL) {
		task_lock(task);
		task->ref_count++;
		task_unlock(task);
	}
}

boolean_t
task_reference_try(
	task_t		task)
{
	if (task != TASK_NULL) {
		if (task_lock_try(task)) {
			task->ref_count++;
			task_unlock(task);
			return TRUE;
		}
	}
	return FALSE;
}

/*
 *	task_terminate:
 *
 *	Terminate the specified task.  See comments on thread_terminate
 *	(kern/thread.c) about problems with terminating the "current task."
 */

kern_return_t
task_terminate(
	task_t		task)
{
	if (task == TASK_NULL)
		return(KERN_INVALID_ARGUMENT);
	if (task->bsd_info)
		return(KERN_FAILURE);
	return (task_terminate_internal(task));
}

kern_return_t
task_terminate_internal(
	task_t		task)
{
	thread_act_t	thr_act, cur_thr_act;
	task_t		cur_task;

	assert(task != kernel_task);

	cur_thr_act = current_act();
	cur_task = cur_thr_act->task;

#if	TASK_SWAPPER
	/*
	 *	If task is not resident (swapped out, or being swapped
	 *	out), we want to bring it back in (this can block).
	 *	NOTE: The only way that this can happen in the current
	 *	system is if the task is swapped while it has a thread
	 *	in exit(), and the thread does not hit a clean point
	 *	to swap itself before getting here.  
	 *	Terminating other tasks is another way to this code, but
	 *	it is not yet fully supported.
	 *	The task_swapin is unconditional.  It used to be done
	 *	only if the task is not resident.  Swapping in a
	 *	resident task will prevent it from being swapped out 
	 *	while it terminates.
	 */
	task_swapin(task, TRUE);	/* TRUE means make it unswappable */
#endif	/* TASK_SWAPPER */

	/*
	 *	Get the task locked and make sure that we are not racing
	 *	with someone else trying to terminate us.
	 */
	if (task == cur_task) {
		task_lock(task);
	} else if (task < cur_task) {
		task_lock(task);
		task_lock(cur_task);
	} else {
		task_lock(cur_task);
		task_lock(task);
	}

	if (!task->active || !cur_thr_act->active) {
		/*
		 *	Task or current act is already being terminated.
		 *	Just return an error. If we are dying, this will
		 *	just get us to our AST special handler and that
		 *	will get us to finalize the termination of ourselves.
		 */
		task_unlock(task);
		if (cur_task != task)
			task_unlock(cur_task);
		return(KERN_FAILURE);
	}
	if (cur_task != task)
		task_unlock(cur_task);

	/*
	 *	Indicate that we want all the threads to stop executing
	 *	at user space by holding the task (we would have held
	 *	each thread independently in thread_terminate_internal -
	 *	but this way we may be more likely to already find it
	 *	held there).  Mark the task inactive, and prevent
	 *	further task operations via the task port.
	 */
	task_hold_locked(task);
	task->active = FALSE;
	ipc_task_disable(task);

	/*
	 *	Terminate each activation in the task.
	 *
	 *	Each terminated activation will run it's special handler
	 *	when its current kernel context is unwound.  That will
	 *	clean up most of the thread resources.  Then it will be
	 *	handed over to the reaper, who will finally remove the
	 *	thread from the task list and free the structures.
	 *
	 *	We can't terminate the current activation yet, because
	 *	it has to wait for the others in an interruptible state.
	 *	We may also block interruptibly during the rest of the
	 *	cleanup.  Wait until the very last to terminate ourself.
	 *
	 *	But if we have virtual machine state, we need to clean
	 *	that up now, because it may be holding wirings the task's
	 *	map that would get stuck in the vm_map_remove() below.
         */
	queue_iterate(&task->thr_acts, thr_act, thread_act_t, thr_acts) {
		if (thr_act != cur_thr_act)
			thread_terminate_internal(thr_act);
		else
			act_virtual_machine_destroy(thr_act);
	}
	task_unlock(task);

	/*
	 *	Destroy all synchronizers owned by the task.
	 */
	task_synchronizer_destroy_all(task);

	/*
	 *	Deallocate all subsystems owned by the task.
	 */
	task_subsystem_destroy_all(task);

	/*
	 *	Destroy the IPC space, leaving just a reference for it.
	 */
	if (!task->kernel_loaded)
		ipc_space_destroy(task->itk_space);

	/*
	 * If the current thread is a member of the task
	 * being terminated, then the last reference to
	 * the task will not be dropped until the thread
	 * is finally reaped.  To avoid incurring the
	 * expense of removing the address space regions
	 * at reap time, we do it explictly here.
	 */
	(void) vm_map_remove(task->map,
			     task->map->min_offset,
			     task->map->max_offset, VM_MAP_NO_FLAGS);

	/*
	 *	Finally, mark ourself for termination and then
	 *	deallocate the task's reference to itself.
	 */
	if (task == cur_task)
		thread_terminate(cur_thr_act);
	task_deallocate(task);

	return(KERN_SUCCESS);
}

/*
 * task_halt -	Shut the current task down (except for the current thread) in
 *		preparation for dramatic changes to the task (probably exec).
 *		We hold the task, terminate all other threads in the task and
 *		wait for them to terminate, clean up the portspace, and when
 *		all done, let the current thread go.
 */
kern_return_t
task_halt(
	task_t		task)
{
	thread_act_t	thr_act, cur_thr_act;
	task_t		cur_task;

	assert(task != kernel_task);

	cur_thr_act = current_act();
	cur_task = cur_thr_act->task;

	if (task != cur_task) {
		return(KERN_INVALID_ARGUMENT);
	}

#if	TASK_SWAPPER
	/*
	 *	If task is not resident (swapped out, or being swapped
	 *	out), we want to bring it back in and make it unswappable.
	 *	This can block, so do it early.
	 */
	task_swapin(task, TRUE);	/* TRUE means make it unswappable */
#endif	/* TASK_SWAPPER */

	task_lock(task);

	if (!task->active || !cur_thr_act->active) {
		/*
		 *	Task or current thread is already being terminated.
		 *	Hurry up and return out of the current kernel context
		 *	so that we run our AST special handler to terminate
		 *	ourselves.
		 */
		task_unlock(task);
		return(KERN_FAILURE);
	}

	if (task->thr_act_count > 1) {
		/*
		 * Mark all the threads to keep them from starting any more
		 * user-level execution.  The thread_terminate_internal code
		 * would do this on a thread by thread basis anyway, but this
		 * gives us a better chance of not having to wait there.
		 */
		task_hold_locked(task);

		/*
		 *	Terminate all the other activations in the task.
		 *
		 *	Each terminated activation will run it's special handler
		 *	when its current kernel context is unwound.  That will
		 *	clean up most of the thread resources.  Then it will be
		 *	handed over to the reaper, who will finally remove the
		 *	thread from the task list and free the structures.
		 *
		 *	If the current thread has any virtual machine state
		 *	associated with it, clean that up now before we try
		 *	to clean up the task VM and port spaces.
		 */
		queue_iterate(&task->thr_acts, thr_act, thread_act_t,thr_acts) {
			if (thr_act != cur_thr_act)
				thread_terminate_internal(thr_act);
			else
				act_virtual_machine_destroy(thr_act);
		}
		task_release_locked(task);
	}
	task_unlock(task);

	/*
	 *	Destroy all synchronizers owned by the task.
	 */
	task_synchronizer_destroy_all(task);

	/*
	 *	Deallocate all subsystems owned by the task.
	 */
	task_subsystem_destroy_all(task);

	/*
	 *	Destroy the IPC space, leaving just a reference for it.
	 */
#if 0
	if (!task->kernel_loaded)
		ipc_space_clean(task->itk_space);
#endif

	/*
	 * Clean out the address space, as we are going to be
	 * getting a new one.
	 */
	(void) vm_map_remove(task->map,
			     task->map->min_offset,
			     task->map->max_offset, VM_MAP_NO_FLAGS);

	return KERN_SUCCESS;
}

/*
 *	task_hold_locked:
 *
 *	Suspend execution of the specified task.
 *	This is a recursive-style suspension of the task, a count of
 *	suspends is maintained.
 *
 * 	CONDITIONS: the task is locked and active.
 */
void
task_hold_locked(
	register task_t	task)
{
	register thread_act_t	thr_act;

	assert(task->active);

	task->suspend_count++;

	/*
	 *	Iterate through all the thread_act's and hold them.
	 */
	queue_iterate(&task->thr_acts, thr_act, thread_act_t, thr_acts) {
		act_lock_thread(thr_act);
		thread_hold(thr_act);
		act_unlock_thread(thr_act);
	}
}

/*
 *	task_hold:
 *
 *	Same as the internal routine above, except that is must lock
 *	and verify that the task is active.  This differs from task_suspend
 *	in that it places a kernel hold on the task rather than just a 
 *	user-level hold.  This keeps users from over resuming and setting
 *	it running out from under the kernel.
 *
 * 	CONDITIONS: the caller holds a reference on the task
 */
kern_return_t
task_hold(task_t task)
{
        kern_return_t kret;
        
	if (task == TASK_NULL)
		return (KERN_INVALID_ARGUMENT);
	task_lock(task);
	if (!task->active) {
		task_unlock(task);
		return (KERN_FAILURE);
	}
        task_hold_locked(task);
        task_unlock(task);

        return(KERN_SUCCESS);
}

/*
 * Routine:	task_wait_locked
 *	Wait for all threads in task to stop.
 *
 * Conditions:
 *	Called with task locked, active, and held.
 */
void
task_wait_locked(
	register task_t		task)
{
	register thread_act_t	thr_act, cur_thr_act;

	assert(task->active);
	assert(task->suspend_count > 0);

	cur_thr_act = current_act();
	/*
	 *	Iterate through all the thread's and wait for them to
	 *	stop.  Do not wait for the current thread if it is within
	 *	the task.
	 */
	queue_iterate(&task->thr_acts, thr_act, thread_act_t, thr_acts) {
		if (thr_act != cur_thr_act) {
			thread_shuttle_t thr_shuttle;

			thr_shuttle = act_lock_thread(thr_act);
			thread_wait(thr_shuttle);
			act_unlock_thread(thr_act);
		}
	}
}

/*
 *	task_release_locked:
 *
 *	Release a kernel hold on a task.
 *
 * 	CONDITIONS: the task is locked and active
 */
void
task_release_locked(
	register task_t	task)
{
	register thread_act_t	thr_act;

	assert(task->active);

	task->suspend_count--;
	assert(task->suspend_count >= 0);

	/*
	 *	Iterate through all the thread_act's and hold them.
	 *	Do not hold the current thread_act if it is within the
	 *	task.
	 */
	queue_iterate(&task->thr_acts, thr_act, thread_act_t, thr_acts) {
		act_lock_thread(thr_act);
		thread_release(thr_act);
		act_unlock_thread(thr_act);
	}
}

/*
 *	task_release:
 *
 *	Same as the internal routine above, except that it must lock
 *	and verify that the task is active.
 *
 * 	CONDITIONS: The caller holds a reference to the task
 */
kern_return_t
task_release(task_t task)
{
        kern_return_t kret;
        
	if (task == TASK_NULL)
		return (KERN_INVALID_ARGUMENT);
	task_lock(task);
	if (!task->active) {
		task_unlock(task);
		return (KERN_FAILURE);
	}
        task_release_locked(task);
        task_unlock(task);

        return(KERN_SUCCESS);
}

kern_return_t
task_threads(
	task_t			task,
	thread_act_array_t	*thr_act_list,
	mach_msg_type_number_t	*count)
{
	unsigned int		actual;	/* this many thr_acts */
	thread_act_t		thr_act;
	thread_act_t		*thr_acts;
	thread_t		thread;
	int			i, j;

	vm_size_t size, size_needed;
	vm_offset_t addr;

	if (task == TASK_NULL)
		return KERN_INVALID_ARGUMENT;

	size = 0; addr = 0;

	for (;;) {
		task_lock(task);
		if (!task->active) {
			task_unlock(task);
			if (size != 0)
				kfree(addr, size);
			return KERN_FAILURE;
		}

		actual = task->thr_act_count;

		/* do we have the memory we need? */
		size_needed = actual * sizeof(mach_port_t);
		if (size_needed <= size)
			break;

		/* unlock the task and allocate more memory */
		task_unlock(task);

		if (size != 0)
			kfree(addr, size);

		assert(size_needed > 0);
		size = size_needed;

		addr = kalloc(size);
		if (addr == 0)
			return KERN_RESOURCE_SHORTAGE;
	}

	/* OK, have memory and the task is locked & active */
	thr_acts = (thread_act_t *) addr;

	for (i = j = 0, thr_act = (thread_act_t) queue_first(&task->thr_acts);
	     i < actual;
	     i++, thr_act = (thread_act_t) queue_next(&thr_act->thr_acts)) {
		act_lock(thr_act);
		if (thr_act->ref_count > 0) {
			act_locked_act_reference(thr_act);
			thr_acts[j++] = thr_act;
		}
		act_unlock(thr_act);
	}
	assert(queue_end(&task->thr_acts, (queue_entry_t) thr_act));

	actual = j;
	size_needed = actual * sizeof(mach_port_t);

	/* can unlock task now that we've got the thr_act refs */
	task_unlock(task);

	if (actual == 0) {
		/* no thr_acts, so return null pointer and deallocate memory */

		*thr_act_list = 0;
		*count = 0;

		if (size != 0)
			kfree(addr, size);
	} else {
		/* if we allocated too much, must copy */

		if (size_needed < size) {
			vm_offset_t newaddr;

			newaddr = kalloc(size_needed);
			if (newaddr == 0) {
				for (i = 0; i < actual; i++)
					act_deallocate(thr_acts[i]);
				kfree(addr, size);
				return KERN_RESOURCE_SHORTAGE;
			}

			bcopy((char *) addr, (char *) newaddr, size_needed);
			kfree(addr, size);
			thr_acts = (thread_act_t *) newaddr;
		}

		*thr_act_list = thr_acts;
		*count = actual;

		/* do the conversion that Mig should handle */

		for (i = 0; i < actual; i++)
			((ipc_port_t *) thr_acts)[i] =
				convert_act_to_port(thr_acts[i]);
	}

	return KERN_SUCCESS;
}

/*
 * Routine:	task_suspend
 *	Implement a user-level suspension on a task.
 *
 * Conditions:
 * 	The caller holds a reference to the task
 */
kern_return_t
task_suspend(
	register task_t		task)
{
	if (task == TASK_NULL)
		return (KERN_INVALID_ARGUMENT);

	task_lock(task);
	if (!task->active) {
		task_unlock(task);
		return (KERN_FAILURE);
	}
	if ((task->user_stop_count)++ > 0) {
		/*
		 *	If the stop count was positive, the task is
		 *	already stopped and we can exit.
		 */
		task_unlock(task);
		return (KERN_SUCCESS);
	}

	/*
	 * Put a kernel-level hold on the threads in the task (all
	 * user-level task suspensions added together represent a
	 * single kernel-level hold).  We then wait for the threads
	 * to stop executing user code.
	 */
	task_hold_locked(task);
	task_wait_locked(task);
	task_unlock(task);
	return (KERN_SUCCESS);
}

/*
 * Routine:	task_resume
 *		Release a kernel hold on a task.
 *		
 * Conditions:
 *		The caller holds a reference to the task
 */
kern_return_t 
task_resume(register task_t task)
{
	register boolean_t	release;

	if (task == TASK_NULL)
		return(KERN_INVALID_ARGUMENT);

	release = FALSE;
	task_lock(task);
	if (!task->active) {
		task_unlock(task);
		return(KERN_FAILURE);
	}
	if (task->user_stop_count > 0) {
		if (--(task->user_stop_count) == 0)
	    		release = TRUE;
	}
	else {
		task_unlock(task);
		return(KERN_FAILURE);
	}

	/*
	 *	Release the task if necessary.
	 */
	if (release)
		task_release_locked(task);

	task_unlock(task);
	return(KERN_SUCCESS);
}

kern_return_t
host_security_set_task_token(
        host_security_t  host_security,
        task_t		 task,
        security_token_t sec_token,
	host_priv_t	 host_priv)
{
	kern_return_t	 kr;

	if (task == TASK_NULL)
		return(KERN_INVALID_ARGUMENT);

	if (host_security == HOST_NULL)
		return(KERN_INVALID_SECURITY);

        task_lock(task);
        task->sec_token = sec_token;
        task_unlock(task);

	if (host_priv != HOST_PRIV_NULL) {
		kr = task_set_special_port(task,
				TASK_HOST_PORT,
				ipc_port_make_send(realhost.host_priv_self));
	} else {
		kr = task_set_special_port(task,
				TASK_HOST_PORT,
				ipc_port_make_send(realhost.host_self));
	}
        return(kr);
}

/*
 * Utility routine to set a ledger
 */
kern_return_t
task_set_ledger(
        task_t		task,
        ledger_t	wired,
        ledger_t	paged)
{
	if (task == TASK_NULL)
		return(KERN_INVALID_ARGUMENT);

        task_lock(task);
        if (wired) {
                ipc_port_release_send(task->wired_ledger_port);
                task->wired_ledger_port = ledger_copy(wired);
        }                
        if (paged) {
                ipc_port_release_send(task->paged_ledger_port);
                task->paged_ledger_port = ledger_copy(paged);
        }                
        task_unlock(task);

        return(KERN_SUCCESS);
}

/*
 * This routine was added, pretty much exclusively, for registering the
 * RPC glue vector for in-kernel short circuited tasks.  Rather than
 * removing it completely, I have only disabled that feature (which was
 * the only feature at the time).  It just appears that we are going to
 * want to add some user data to tasks in the future (i.e. bsd info,
 * task names, etc...), so I left it in the formal task interface.
 */
kern_return_t
task_set_info(
	task_t		task,
	task_flavor_t	flavor,
	task_info_t	task_info_in,		/* pointer to IN array */
	mach_msg_type_number_t	task_info_count)
{
	vm_map_t		map;

	if (task == TASK_NULL)
		return(KERN_INVALID_ARGUMENT);

	switch (flavor) {
	    default:
		return (KERN_INVALID_ARGUMENT);
	}
	return (KERN_SUCCESS);
}

kern_return_t
task_info(
	task_t			task,
	task_flavor_t		flavor,
	task_info_t		task_info_out,
	mach_msg_type_number_t	*task_info_count)
{
	thread_t	thread;
	vm_map_t	map;

	if (task == TASK_NULL)
		return(KERN_INVALID_ARGUMENT);

	switch (flavor) {

	    case TASK_BASIC_INFO:
	    {
		register task_basic_info_t	basic_info;

		if (*task_info_count < TASK_BASIC_INFO_COUNT) {
		    return(KERN_INVALID_ARGUMENT);
		}

		basic_info = (task_basic_info_t) task_info_out;

		map = (task == kernel_task) ? kernel_map : task->map;

		basic_info->virtual_size  = map->size;
		basic_info->resident_size = pmap_resident_count(map->pmap)
						   * PAGE_SIZE;

		task_lock(task);
		basic_info->policy = task->policy;
		basic_info->suspend_count = task->user_stop_count;
		basic_info->user_time.seconds
				= task->total_user_time.seconds;
		basic_info->user_time.microseconds
				= task->total_user_time.microseconds;
		basic_info->system_time.seconds
				= task->total_system_time.seconds;
		basic_info->system_time.microseconds 
				= task->total_system_time.microseconds;
		task_unlock(task);

		*task_info_count = TASK_BASIC_INFO_COUNT;
		break;
	    }

	    case TASK_THREAD_TIMES_INFO:
	    {
		register task_thread_times_info_t times_info;
		register thread_t	thread;
		register thread_act_t	thr_act;

		if (*task_info_count < TASK_THREAD_TIMES_INFO_COUNT) {
		    return (KERN_INVALID_ARGUMENT);
		}

		times_info = (task_thread_times_info_t) task_info_out;
		times_info->user_time.seconds = 0;
		times_info->user_time.microseconds = 0;
		times_info->system_time.seconds = 0;
		times_info->system_time.microseconds = 0;

		task_lock(task);
		queue_iterate(&task->thr_acts, thr_act,
			      thread_act_t, thr_acts)
		{
		    time_value_t user_time, system_time;
		    spl_t	 s;

		    thread = act_lock_thread(thr_act);

		    /* Skip empty threads and threads that have migrated
		     * into this task:
		     */
		    if (!thread || thr_act->pool_port) {
			act_unlock_thread(thr_act);
			continue;
		    }
		    assert(thread);  /* Must have thread, if no thread_pool*/
		    s = splsched();
		    thread_lock(thread);

		    thread_read_times(thread, &user_time, &system_time);

		    thread_unlock(thread);
		    splx(s);
		    act_unlock_thread(thr_act);

		    time_value_add(&times_info->user_time, &user_time);
		    time_value_add(&times_info->system_time, &system_time);
		}
		task_unlock(task);

		*task_info_count = TASK_THREAD_TIMES_INFO_COUNT;
		break;
	    }

	    case TASK_SCHED_FIFO_INFO:
	    {
		register policy_fifo_base_t	fifo_base;

		if (*task_info_count < POLICY_FIFO_BASE_COUNT)
			return(KERN_INVALID_ARGUMENT);

		fifo_base = (policy_fifo_base_t) task_info_out;

		task_lock(task);
		if (task->policy != POLICY_FIFO) {
			task_unlock(task);
			return(KERN_INVALID_POLICY);
		}

		fifo_base->base_priority = task->priority;
		task_unlock(task);

		*task_info_count = POLICY_FIFO_BASE_COUNT;
		break;
	    }

	    case TASK_SCHED_RR_INFO:
	    {
		register policy_rr_base_t	rr_base;

		if (*task_info_count < POLICY_RR_BASE_COUNT)
			return(KERN_INVALID_ARGUMENT);

		rr_base = (policy_rr_base_t) task_info_out;

		task_lock(task);
		if (task->policy != POLICY_RR) {
			task_unlock(task);
			return(KERN_INVALID_POLICY);
		}

		rr_base->base_priority = task->priority;
		task_unlock(task);

		rr_base->quantum = (min_quantum	* tick) / 1000;

		*task_info_count = POLICY_RR_BASE_COUNT;
		break;
	    }

	    case TASK_SCHED_TIMESHARE_INFO:
	    {
		register policy_timeshare_base_t	ts_base;

		if (*task_info_count < POLICY_TIMESHARE_BASE_COUNT)
			return(KERN_INVALID_ARGUMENT);

		ts_base = (policy_timeshare_base_t) task_info_out;

		task_lock(task);
		if (task->policy != POLICY_TIMESHARE) {
			task_unlock(task);
			return(KERN_INVALID_POLICY);
		}

		ts_base->base_priority = task->priority;
		task_unlock(task);

		*task_info_count = POLICY_TIMESHARE_BASE_COUNT;
		break;
	    }

            case TASK_SECURITY_TOKEN:
	    {
                register security_token_t	*sec_token_p;

		if (*task_info_count < TASK_SECURITY_TOKEN_COUNT) {
		    return(KERN_INVALID_ARGUMENT);
		}

		sec_token_p = (security_token_t *) task_info_out;

		task_lock(task);
		*sec_token_p = task->sec_token;
		task_unlock(task);

		*task_info_count = TASK_SECURITY_TOKEN_COUNT;
                break;
            }
            
	    case TASK_SCHED_INFO:
			return(KERN_INVALID_ARGUMENT);

	    case TASK_EVENTS_INFO:
	    {
		register task_events_info_t	events_info;

		if (*task_info_count < TASK_EVENTS_INFO_COUNT) {
		    return(KERN_INVALID_ARGUMENT);
		}

		events_info = (task_events_info_t) task_info_out;

		task_lock(task);
		events_info->faults = task->faults;
		events_info->pageins = task->pageins;
		events_info->cow_faults = task->cow_faults;
		events_info->messages_sent = task->messages_sent;
		events_info->messages_received = task->messages_received;
		events_info->syscalls_mach = task->syscalls_mach;
		events_info->syscalls_unix = task->syscalls_unix;
		events_info->csw = task->csw;
		task_unlock(task);

		*task_info_count = TASK_EVENTS_INFO_COUNT;
		break;
	    }

	    default:
		return (KERN_INVALID_ARGUMENT);
	}

	return(KERN_SUCCESS);
}

/*
 *	task_assign:
 *
 *	Change the assigned processor set for the task
 */
kern_return_t
task_assign(
	task_t		task,
	processor_set_t	new_pset,
	boolean_t	assign_threads)
{
#ifdef	lint
	task++; new_pset++; assign_threads++;
#endif	/* lint */
	return(KERN_FAILURE);
}

/*
 *	task_assign_default:
 *
 *	Version of task_assign to assign to default processor set.
 */
kern_return_t
task_assign_default(
	task_t		task,
	boolean_t	assign_threads)
{
    return (task_assign(task, &default_pset, assign_threads));
}

/*
 *	task_get_assignment
 *
 *	Return name of processor set that task is assigned to.
 */
kern_return_t
task_get_assignment(
	task_t		task,
	processor_set_t	*pset)
{
	if (!task->active)
		return(KERN_FAILURE);

	*pset = task->processor_set;
	pset_reference(*pset);
	return(KERN_SUCCESS);
}


/*
 * 	task_policy
 *
 *	Set scheduling policy and parameters, both base and limit, for
 *	the given task. Policy must be a policy which is enabled for the
 *	processor set. Change contained threads if requested. 
 */
kern_return_t
task_policy(
	task_t					task,
	policy_t				policy_id,
	policy_base_t			base,
	mach_msg_type_number_t	count,
	boolean_t				set_limit,
	boolean_t				change)
{
	return(KERN_FAILURE);
}

/*
 *	task_set_policy
 *
 *	Set scheduling policy and parameters, both base and limit, for 
 *	the given task. Policy can be any policy implemented by the
 *	processor set, whether enabled or not. Change contained threads
 *	if requested.
 */
kern_return_t
task_set_policy(
	task_t					task,
	processor_set_t			pset,
	policy_t				policy_id,
	policy_base_t			base,
	mach_msg_type_number_t	base_count,
	policy_limit_t			limit,
	mach_msg_type_number_t	limit_count,
	boolean_t				change)
{
	return(KERN_FAILURE);
}

/*
 *	task_collect_scan:
 *
 *	Attempt to free resources owned by tasks.
 */

void
task_collect_scan(void)
{
	register task_t		task, prev_task;
	processor_set_t		pset = &default_pset;

	prev_task = TASK_NULL;

	pset_lock(pset);
	pset->ref_count++;
	task = (task_t) queue_first(&pset->tasks);
	while (!queue_end(&pset->tasks, (queue_entry_t) task)) {
		task_reference(task);
		pset_unlock(pset);

		pmap_collect(task->map->pmap);

		if (prev_task != TASK_NULL)
			task_deallocate(prev_task);
		prev_task = task;

		pset_lock(pset);
		task = (task_t) queue_next(&task->pset_tasks);
	}
	pset_unlock(pset);

	pset_deallocate(pset);

	if (prev_task != TASK_NULL)
		task_deallocate(prev_task);
}

boolean_t task_collect_allowed = FALSE;
unsigned task_collect_last_tick = 0;
unsigned task_collect_max_rate = 0;		/* in ticks */

/*
 *	consider_task_collect:
 *
 *	Called by the pageout daemon when the system needs more free pages.
 */

void
consider_task_collect(void)
{
	/*
	 *	By default, don't attempt task collection more frequently
	 *	than once per second.
	 */

	if (task_collect_max_rate == 0)
		task_collect_max_rate = (2 << SCHED_TICK_SHIFT);

	if (task_collect_allowed &&
	    (sched_tick > (task_collect_last_tick + task_collect_max_rate))) {
		task_collect_last_tick = sched_tick;
		task_collect_scan();
	}
}

kern_return_t
task_set_ras_pc(
 	task_t		task,
 	vm_offset_t	pc,
 	vm_offset_t	endpc)
{
#if	FAST_TAS
	extern int fast_tas_debug;
 
	if (fast_tas_debug) {
		printf("task 0x%x: setting fast_tas to [0x%x, 0x%x]\n",
		       task, pc, endpc);
	}
	task_lock(task);
	task->fast_tas_base = pc;
	task->fast_tas_end =  endpc;
	task_unlock(task);
	return KERN_SUCCESS;
 
#else	/* FAST_TAS */
#ifdef	lint
	task++;
	pc++;
	endpc++;
#endif	/* lint */
 
	return KERN_FAILURE;
 
#endif	/* FAST_TAS */
}

void
task_synchronizer_destroy_all(task_t task)
{
	semaphore_t	semaphore;
	lock_set_t	lock_set;

	/*
	 *  Destroy owned semaphores
	 */

	while (!queue_empty(&task->semaphore_list)) {
		semaphore = (semaphore_t) queue_first(&task->semaphore_list);
		(void) semaphore_destroy(task, semaphore);
	}

	/*
	 *  Destroy owned lock sets
	 */

	while (!queue_empty(&task->lock_set_list)) {
		lock_set = (lock_set_t) queue_first(&task->lock_set_list);
		(void) lock_set_destroy(task, lock_set);
	}
}

void
task_subsystem_destroy_all(task_t task)
{
	subsystem_t	subsystem;

	/*
	 *  Destroy owned subsystems
	 */

	while (!queue_empty(&task->subsystem_list)) {
		subsystem = (subsystem_t) queue_first(&task->subsystem_list);
		subsystem_deallocate(subsystem);
	}
}

/*
 *	task_set_port_space:
 *
 *	Set port name space of task to specified size.
 */

kern_return_t
task_set_port_space(
 	task_t		task,
 	int		table_entries)
{
	kern_return_t kr;
	
	is_write_lock(task->itk_space);
	kr = ipc_entry_grow_table(task->itk_space, table_entries);
	if (kr == KERN_SUCCESS)
		is_write_unlock(task->itk_space);
	return kr;
}

/*
 * We need to export some functions to other components that
 * are currently implemented in macros within the osfmk
 * component.  Just export them as functions of the same name.
 */
boolean_t is_kerneltask(task_t t)
{
	if (t == kernel_task)
		return(TRUE);
	else
		return((t->kernel_loaded));
}

#undef current_task
task_t current_task()
{
	return (current_task_fast());
}

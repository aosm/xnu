/*
 * Copyright (c) 2000-2008 Apple Inc. All rights reserved.
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
 */

/*
 *	processor.c: processor and processor_set manipulation routines.
 */

#include <mach/boolean.h>
#include <mach/policy.h>
#include <mach/processor.h>
#include <mach/processor_info.h>
#include <mach/vm_param.h>
#include <kern/cpu_number.h>
#include <kern/host.h>
#include <kern/machine.h>
#include <kern/misc_protos.h>
#include <kern/processor.h>
#include <kern/sched.h>
#include <kern/task.h>
#include <kern/thread.h>
#include <kern/ipc_host.h>
#include <kern/ipc_tt.h>
#include <ipc/ipc_port.h>
#include <kern/kalloc.h>

/*
 * Exported interface
 */
#include <mach/mach_host_server.h>
#include <mach/processor_set_server.h>

struct processor_set	pset0;
struct pset_node		pset_node0;
decl_simple_lock_data(static,pset_node_lock)

queue_head_t			tasks;
int						tasks_count;
queue_head_t			threads;
int						threads_count;
decl_mutex_data(,tasks_threads_lock)

processor_t				processor_list;
unsigned int			processor_count;
static processor_t		processor_list_tail;
decl_simple_lock_data(,processor_list_lock)

uint32_t				processor_avail_count;

processor_t	master_processor;
int 		master_cpu = 0;

/* Forwards */
kern_return_t	processor_set_things(
		processor_set_t		pset,
		mach_port_t		**thing_list,
		mach_msg_type_number_t	*count,
		int			type);

void
processor_bootstrap(void)
{
	pset_init(&pset0, &pset_node0);
	pset_node0.psets = &pset0;

	simple_lock_init(&pset_node_lock, 0);

	mutex_init(&tasks_threads_lock, 0);
	queue_init(&tasks);
	queue_init(&threads);

	simple_lock_init(&processor_list_lock, 0);

	master_processor = cpu_to_processor(master_cpu);

	processor_init(master_processor, master_cpu, &pset0);
}

/*
 *	Initialize the given processor for the cpu
 *	indicated by slot_num, and assign to the
 *	specified processor set.
 */
void
processor_init(
	processor_t		p,
	int				slot_num,
	processor_set_t	pset)
{
	run_queue_init(&p->runq);

	p->state = PROCESSOR_OFF_LINE;
	p->active_thread = p->next_thread = p->idle_thread = THREAD_NULL;
	p->processor_set = pset;
	p->current_pri = MINPRI;
	timer_call_setup(&p->quantum_timer, thread_quantum_expire, p);
	p->deadline = UINT64_MAX;
	p->timeslice = 0;
	p->processor_self = IP_NULL;
	simple_lock_init(&p->lock, 0);
	processor_data_init(p);
	PROCESSOR_DATA(p, slot_num) = slot_num;
	p->processor_list = NULL;

	simple_lock(&processor_list_lock);
	if (processor_list == NULL)
		processor_list = p;
	else
		processor_list_tail->processor_list = p;
	processor_list_tail = p;
	processor_count++;
	simple_unlock(&processor_list_lock);
}

processor_set_t
processor_pset(
	processor_t	processor)
{
	return (processor->processor_set);
}

pset_node_t
pset_node_root(void)
{
	return &pset_node0;
}

processor_set_t
pset_create(
	pset_node_t			node)
{
	processor_set_t		*prev, pset = kalloc(sizeof (*pset));

	if (pset != PROCESSOR_SET_NULL) {
		pset_init(pset, node);

		simple_lock(&pset_node_lock);

		prev = &node->psets;
		while (*prev != PROCESSOR_SET_NULL)
			prev = &(*prev)->pset_list;

		*prev = pset;

		simple_unlock(&pset_node_lock);
	}

	return (pset);
}

/*
 *	Initialize the given processor_set structure.
 */
void
pset_init(
	processor_set_t		pset,
	pset_node_t			node)
{
	queue_init(&pset->active_queue);
	queue_init(&pset->idle_queue);
	pset->idle_count = 0;
	pset->processor_count = 0;
	pset->low_pri = PROCESSOR_NULL;
	pset_lock_init(pset);
	pset->pset_self = IP_NULL;
	pset->pset_name_self = IP_NULL;
	pset->pset_list = PROCESSOR_SET_NULL;
	pset->node = node;
}

kern_return_t
processor_info_count(
	processor_flavor_t		flavor,
	mach_msg_type_number_t	*count)
{
	switch (flavor) {

	case PROCESSOR_BASIC_INFO:
		*count = PROCESSOR_BASIC_INFO_COUNT;
		break;

	case PROCESSOR_CPU_LOAD_INFO:
		*count = PROCESSOR_CPU_LOAD_INFO_COUNT;
		break;

	default:
		return (cpu_info_count(flavor, count));
	}

	return (KERN_SUCCESS);
}


kern_return_t
processor_info(
	register processor_t	processor,
	processor_flavor_t		flavor,
	host_t					*host,
	processor_info_t		info,
	mach_msg_type_number_t	*count)
{
	register int	slot_num, state;
	kern_return_t	result;

	if (processor == PROCESSOR_NULL)
		return (KERN_INVALID_ARGUMENT);

	slot_num = PROCESSOR_DATA(processor, slot_num);

	switch (flavor) {

	case PROCESSOR_BASIC_INFO:
	{
		register processor_basic_info_t		basic_info;

		if (*count < PROCESSOR_BASIC_INFO_COUNT)
			return (KERN_FAILURE);

		basic_info = (processor_basic_info_t) info;
		basic_info->cpu_type = slot_type(slot_num);
		basic_info->cpu_subtype = slot_subtype(slot_num);
		state = processor->state;
		if (state == PROCESSOR_OFF_LINE)
			basic_info->running = FALSE;
		else
			basic_info->running = TRUE;
		basic_info->slot_num = slot_num;
		if (processor == master_processor) 
			basic_info->is_master = TRUE;
		else
			basic_info->is_master = FALSE;

		*count = PROCESSOR_BASIC_INFO_COUNT;
		*host = &realhost;

	    return (KERN_SUCCESS);
	}

	case PROCESSOR_CPU_LOAD_INFO:
	{
		register processor_cpu_load_info_t	cpu_load_info;

	    if (*count < PROCESSOR_CPU_LOAD_INFO_COUNT)
			return (KERN_FAILURE);

	    cpu_load_info = (processor_cpu_load_info_t) info;
		cpu_load_info->cpu_ticks[CPU_STATE_USER] =
							timer_grab(&PROCESSOR_DATA(processor, user_state)) / hz_tick_interval;
		cpu_load_info->cpu_ticks[CPU_STATE_SYSTEM] =
							timer_grab(&PROCESSOR_DATA(processor, system_state)) / hz_tick_interval;
		cpu_load_info->cpu_ticks[CPU_STATE_IDLE] =
							timer_grab(&PROCESSOR_DATA(processor, idle_state)) / hz_tick_interval;
		cpu_load_info->cpu_ticks[CPU_STATE_NICE] = 0;

	    *count = PROCESSOR_CPU_LOAD_INFO_COUNT;
	    *host = &realhost;

	    return (KERN_SUCCESS);
	}

	default:
	    result = cpu_info(flavor, slot_num, info, count);
	    if (result == KERN_SUCCESS)
			*host = &realhost;		   

	    return (result);
	}
}

kern_return_t
processor_start(
	processor_t			processor)
{
	processor_set_t		pset;
	thread_t			thread;   
	kern_return_t		result;
	spl_t				s;

	if (processor == PROCESSOR_NULL || processor->processor_set == PROCESSOR_SET_NULL)
		return (KERN_INVALID_ARGUMENT);

	if (processor == master_processor) {
		processor_t		prev;

		prev = thread_bind(processor);
		thread_block(THREAD_CONTINUE_NULL);

		result = cpu_start(PROCESSOR_DATA(processor, slot_num));

		thread_bind(prev);

		return (result);
	}

	s = splsched();
	pset = processor->processor_set;
	pset_lock(pset);
	if (processor->state != PROCESSOR_OFF_LINE) {
		pset_unlock(pset);
		splx(s);

		return (KERN_FAILURE);
	}

	processor->state = PROCESSOR_START;
	pset_unlock(pset);
	splx(s);

	/*
	 *	Create the idle processor thread.
	 */
	if (processor->idle_thread == THREAD_NULL) {
		result = idle_thread_create(processor);
		if (result != KERN_SUCCESS) {
			s = splsched();
			pset_lock(pset);
			processor->state = PROCESSOR_OFF_LINE;
			pset_unlock(pset);
			splx(s);

			return (result);
		}
	}

	/*
	 *	If there is no active thread, the processor
	 *	has never been started.  Create a dedicated
	 *	start up thread.
	 */
	if (	processor->active_thread == THREAD_NULL		&&
			processor->next_thread == THREAD_NULL		) {
		result = kernel_thread_create((thread_continue_t)processor_start_thread, NULL, MAXPRI_KERNEL, &thread);
		if (result != KERN_SUCCESS) {
			s = splsched();
			pset_lock(pset);
			processor->state = PROCESSOR_OFF_LINE;
			pset_unlock(pset);
			splx(s);

			return (result);
		}

		s = splsched();
		thread_lock(thread);
		thread->bound_processor = processor;
		processor->next_thread = thread;
		thread->state = TH_RUN;
		thread_unlock(thread);
		splx(s);

		thread_deallocate(thread);
	}

	if (processor->processor_self == IP_NULL)
		ipc_processor_init(processor);

	result = cpu_start(PROCESSOR_DATA(processor, slot_num));
	if (result != KERN_SUCCESS) {
		s = splsched();
		pset_lock(pset);
		processor->state = PROCESSOR_OFF_LINE;
		timer_call_shutdown(processor);
		pset_unlock(pset);
		splx(s);

		return (result);
	}

	ipc_processor_enable(processor);

	return (KERN_SUCCESS);
}

kern_return_t
processor_exit(
	processor_t	processor)
{
	if (processor == PROCESSOR_NULL)
		return(KERN_INVALID_ARGUMENT);

	return(processor_shutdown(processor));
}

kern_return_t
processor_control(
	processor_t		processor,
	processor_info_t	info,
	mach_msg_type_number_t	count)
{
	if (processor == PROCESSOR_NULL)
		return(KERN_INVALID_ARGUMENT);

	return(cpu_control(PROCESSOR_DATA(processor, slot_num), info, count));
}
	    
kern_return_t
processor_set_create(
	__unused host_t		host,
	__unused processor_set_t	*new_set,
	__unused processor_set_t	*new_name)
{
	return(KERN_FAILURE);
}

kern_return_t
processor_set_destroy(
	__unused processor_set_t	pset)
{
	return(KERN_FAILURE);
}

kern_return_t
processor_get_assignment(
	processor_t	processor,
	processor_set_t	*pset)
{
	int state;

	state = processor->state;
	if (state == PROCESSOR_SHUTDOWN || state == PROCESSOR_OFF_LINE)
		return(KERN_FAILURE);

	*pset = &pset0;

	return(KERN_SUCCESS);
}

kern_return_t
processor_set_info(
	processor_set_t		pset,
	int			flavor,
	host_t			*host,
	processor_set_info_t	info,
	mach_msg_type_number_t	*count)
{
	if (pset == PROCESSOR_SET_NULL)
		return(KERN_INVALID_ARGUMENT);

	if (flavor == PROCESSOR_SET_BASIC_INFO) {
		register processor_set_basic_info_t	basic_info;

		if (*count < PROCESSOR_SET_BASIC_INFO_COUNT)
			return(KERN_FAILURE);

		basic_info = (processor_set_basic_info_t) info;
		basic_info->processor_count = processor_avail_count;
		basic_info->default_policy = POLICY_TIMESHARE;

		*count = PROCESSOR_SET_BASIC_INFO_COUNT;
		*host = &realhost;
		return(KERN_SUCCESS);
	}
	else if (flavor == PROCESSOR_SET_TIMESHARE_DEFAULT) {
		register policy_timeshare_base_t	ts_base;

		if (*count < POLICY_TIMESHARE_BASE_COUNT)
			return(KERN_FAILURE);

		ts_base = (policy_timeshare_base_t) info;
		ts_base->base_priority = BASEPRI_DEFAULT;

		*count = POLICY_TIMESHARE_BASE_COUNT;
		*host = &realhost;
		return(KERN_SUCCESS);
	}
	else if (flavor == PROCESSOR_SET_FIFO_DEFAULT) {
		register policy_fifo_base_t		fifo_base;

		if (*count < POLICY_FIFO_BASE_COUNT)
			return(KERN_FAILURE);

		fifo_base = (policy_fifo_base_t) info;
		fifo_base->base_priority = BASEPRI_DEFAULT;

		*count = POLICY_FIFO_BASE_COUNT;
		*host = &realhost;
		return(KERN_SUCCESS);
	}
	else if (flavor == PROCESSOR_SET_RR_DEFAULT) {
		register policy_rr_base_t		rr_base;

		if (*count < POLICY_RR_BASE_COUNT)
			return(KERN_FAILURE);

		rr_base = (policy_rr_base_t) info;
		rr_base->base_priority = BASEPRI_DEFAULT;
		rr_base->quantum = 1;

		*count = POLICY_RR_BASE_COUNT;
		*host = &realhost;
		return(KERN_SUCCESS);
	}
	else if (flavor == PROCESSOR_SET_TIMESHARE_LIMITS) {
		register policy_timeshare_limit_t	ts_limit;

		if (*count < POLICY_TIMESHARE_LIMIT_COUNT)
			return(KERN_FAILURE);

		ts_limit = (policy_timeshare_limit_t) info;
		ts_limit->max_priority = MAXPRI_KERNEL;

		*count = POLICY_TIMESHARE_LIMIT_COUNT;
		*host = &realhost;
		return(KERN_SUCCESS);
	}
	else if (flavor == PROCESSOR_SET_FIFO_LIMITS) {
		register policy_fifo_limit_t		fifo_limit;

		if (*count < POLICY_FIFO_LIMIT_COUNT)
			return(KERN_FAILURE);

		fifo_limit = (policy_fifo_limit_t) info;
		fifo_limit->max_priority = MAXPRI_KERNEL;

		*count = POLICY_FIFO_LIMIT_COUNT;
		*host = &realhost;
		return(KERN_SUCCESS);
	}
	else if (flavor == PROCESSOR_SET_RR_LIMITS) {
		register policy_rr_limit_t		rr_limit;

		if (*count < POLICY_RR_LIMIT_COUNT)
			return(KERN_FAILURE);

		rr_limit = (policy_rr_limit_t) info;
		rr_limit->max_priority = MAXPRI_KERNEL;

		*count = POLICY_RR_LIMIT_COUNT;
		*host = &realhost;
		return(KERN_SUCCESS);
	}
	else if (flavor == PROCESSOR_SET_ENABLED_POLICIES) {
		register int				*enabled;

		if (*count < (sizeof(*enabled)/sizeof(int)))
			return(KERN_FAILURE);

		enabled = (int *) info;
		*enabled = POLICY_TIMESHARE | POLICY_RR | POLICY_FIFO;

		*count = sizeof(*enabled)/sizeof(int);
		*host = &realhost;
		return(KERN_SUCCESS);
	}


	*host = HOST_NULL;
	return(KERN_INVALID_ARGUMENT);
}

/*
 *	processor_set_statistics
 *
 *	Returns scheduling statistics for a processor set. 
 */
kern_return_t 
processor_set_statistics(
	processor_set_t         pset,
	int                     flavor,
	processor_set_info_t    info,
	mach_msg_type_number_t	*count)
{
	if (pset == PROCESSOR_SET_NULL || pset != &pset0)
		return (KERN_INVALID_PROCESSOR_SET);

	if (flavor == PROCESSOR_SET_LOAD_INFO) {
		register processor_set_load_info_t     load_info;

		if (*count < PROCESSOR_SET_LOAD_INFO_COUNT)
			return(KERN_FAILURE);

		load_info = (processor_set_load_info_t) info;

		load_info->mach_factor = sched_mach_factor;
		load_info->load_average = sched_load_average;

		load_info->task_count = tasks_count;
		load_info->thread_count = threads_count;

		*count = PROCESSOR_SET_LOAD_INFO_COUNT;
		return(KERN_SUCCESS);
	}

	return(KERN_INVALID_ARGUMENT);
}

/*
 *	processor_set_max_priority:
 *
 *	Specify max priority permitted on processor set.  This affects
 *	newly created and assigned threads.  Optionally change existing
 * 	ones.
 */
kern_return_t
processor_set_max_priority(
	__unused processor_set_t	pset,
	__unused int			max_priority,
	__unused boolean_t		change_threads)
{
	return (KERN_INVALID_ARGUMENT);
}

/*
 *	processor_set_policy_enable:
 *
 *	Allow indicated policy on processor set.
 */

kern_return_t
processor_set_policy_enable(
	__unused processor_set_t	pset,
	__unused int			policy)
{
	return (KERN_INVALID_ARGUMENT);
}

/*
 *	processor_set_policy_disable:
 *
 *	Forbid indicated policy on processor set.  Time sharing cannot
 *	be forbidden.
 */
kern_return_t
processor_set_policy_disable(
	__unused processor_set_t	pset,
	__unused int			policy,
	__unused boolean_t		change_threads)
{
	return (KERN_INVALID_ARGUMENT);
}

#define THING_TASK	0
#define THING_THREAD	1

/*
 *	processor_set_things:
 *
 *	Common internals for processor_set_{threads,tasks}
 */
kern_return_t
processor_set_things(
	processor_set_t			pset,
	mach_port_t				**thing_list,
	mach_msg_type_number_t	*count,
	int						type)
{
	unsigned int actual;	/* this many things */
	unsigned int maxthings;
	unsigned int i;

	vm_size_t size, size_needed;
	void  *addr;

	if (pset == PROCESSOR_SET_NULL || pset != &pset0)
		return (KERN_INVALID_ARGUMENT);

	size = 0;
	addr = NULL;

	for (;;) {
		mutex_lock(&tasks_threads_lock);

		if (type == THING_TASK)
			maxthings = tasks_count;
		else
			maxthings = threads_count;

		/* do we have the memory we need? */

		size_needed = maxthings * sizeof (mach_port_t);
		if (size_needed <= size)
			break;

		/* unlock and allocate more memory */
		mutex_unlock(&tasks_threads_lock);

		if (size != 0)
			kfree(addr, size);

		assert(size_needed > 0);
		size = size_needed;

		addr = kalloc(size);
		if (addr == 0)
			return (KERN_RESOURCE_SHORTAGE);
	}

	/* OK, have memory and the list locked */

	actual = 0;
	switch (type) {

	case THING_TASK: {
		task_t		task, *task_list = (task_t *)addr;

		for (task = (task_t)queue_first(&tasks);
						!queue_end(&tasks, (queue_entry_t)task);
								task = (task_t)queue_next(&task->tasks)) {
#if defined(SECURE_KERNEL)
			if (task != kernel_task) {
#endif
				task_reference_internal(task);
				task_list[actual++] = task;
#if defined(SECURE_KERNEL)
			}
#endif
		}

		break;
	}

	case THING_THREAD: {
		thread_t	thread, *thread_list = (thread_t *)addr;

		for (thread = (thread_t)queue_first(&threads);
						!queue_end(&threads, (queue_entry_t)thread);
								thread = (thread_t)queue_next(&thread->threads)) {
			thread_reference_internal(thread);
			thread_list[actual++] = thread;
		}

		break;
	}

	}
		
	mutex_unlock(&tasks_threads_lock);

	if (actual < maxthings)
		size_needed = actual * sizeof (mach_port_t);

	if (actual == 0) {
		/* no things, so return null pointer and deallocate memory */
		*thing_list = NULL;
		*count = 0;

		if (size != 0)
			kfree(addr, size);
	}
	else {
		/* if we allocated too much, must copy */

		if (size_needed < size) {
			void *newaddr;

			newaddr = kalloc(size_needed);
			if (newaddr == 0) {
				switch (type) {

				case THING_TASK: {
					task_t		*task_list = (task_t *)addr;

					for (i = 0; i < actual; i++)
						task_deallocate(task_list[i]);
					break;
				}

				case THING_THREAD: {
					thread_t	*thread_list = (thread_t *)addr;

					for (i = 0; i < actual; i++)
						thread_deallocate(thread_list[i]);
					break;
				}

				}

				kfree(addr, size);
				return (KERN_RESOURCE_SHORTAGE);
			}

			bcopy((void *) addr, (void *) newaddr, size_needed);
			kfree(addr, size);
			addr = newaddr;
		}

		*thing_list = (mach_port_t *)addr;
		*count = actual;

		/* do the conversion that Mig should handle */

		switch (type) {

		case THING_TASK: {
			task_t		*task_list = (task_t *)addr;

			for (i = 0; i < actual; i++)
				(*thing_list)[i] = convert_task_to_port(task_list[i]);
			break;
		}

		case THING_THREAD: {
			thread_t	*thread_list = (thread_t *)addr;

			for (i = 0; i < actual; i++)
			  	(*thing_list)[i] = convert_thread_to_port(thread_list[i]);
			break;
		}

		}
	}

	return (KERN_SUCCESS);
}


/*
 *	processor_set_tasks:
 *
 *	List all tasks in the processor set.
 */
kern_return_t
processor_set_tasks(
	processor_set_t		pset,
	task_array_t		*task_list,
	mach_msg_type_number_t	*count)
{
    return(processor_set_things(pset, (mach_port_t **)task_list, count, THING_TASK));
}

/*
 *	processor_set_threads:
 *
 *	List all threads in the processor set.
 */
#if defined(SECURE_KERNEL)
kern_return_t
processor_set_threads(
	__unused processor_set_t		pset,
	__unused thread_array_t		*thread_list,
	__unused mach_msg_type_number_t	*count)
{
    return KERN_FAILURE;
}
#elif defined(CONFIG_EMBEDDED)
kern_return_t
processor_set_threads(
	__unused processor_set_t		pset,
	__unused thread_array_t		*thread_list,
	__unused mach_msg_type_number_t	*count)
{
    return KERN_NOT_SUPPORTED;
}
#else
kern_return_t
processor_set_threads(
	processor_set_t		pset,
	thread_array_t		*thread_list,
	mach_msg_type_number_t	*count)
{
    return(processor_set_things(pset, (mach_port_t **)thread_list, count, THING_THREAD));
}
#endif

/*
 *	processor_set_policy_control
 *
 *	Controls the scheduling attributes governing the processor set.
 *	Allows control of enabled policies, and per-policy base and limit
 *	priorities.
 */
kern_return_t
processor_set_policy_control(
	__unused processor_set_t		pset,
	__unused int				flavor,
	__unused processor_set_info_t	policy_info,
	__unused mach_msg_type_number_t	count,
	__unused boolean_t			change)
{
	return (KERN_INVALID_ARGUMENT);
}

#undef pset_deallocate
void pset_deallocate(processor_set_t pset);
void
pset_deallocate(
__unused processor_set_t	pset)
{
	return;
}

#undef pset_reference
void pset_reference(processor_set_t pset);
void
pset_reference(
__unused processor_set_t	pset)
{
	return;
}

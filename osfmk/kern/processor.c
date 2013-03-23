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

#include <cpus.h>
#include <mach_host.h>

#include <mach/boolean.h>
#include <mach/policy.h>
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

#if	MACH_HOST
#include <kern/zalloc.h>
zone_t	pset_zone;
#endif	/* MACH_HOST */

#include <kern/sf.h>
#include <kern/mk_sp.h>	/*** ??? fix so this can be removed ***/

/*
 * Exported interface
 */
#include <mach/mach_host_server.h>

/*
 *	Exported variables.
 */
struct processor_set default_pset;
struct processor processor_array[NCPUS];

processor_t	master_processor;
processor_t	processor_ptr[NCPUS];

/* Forwards */
void	pset_init(
		processor_set_t	pset);

void	processor_init(
		register processor_t	pr,
		int			slot_num);

void	quantum_set(
		processor_set_t		pset);

kern_return_t	processor_set_base(
		processor_set_t 	pset,
		policy_t             	policy,
	        policy_base_t           base,
		boolean_t       	change);

kern_return_t	processor_set_limit(
		processor_set_t 	pset,
		policy_t		policy,
	        policy_limit_t    	limit,
		boolean_t       	change);

kern_return_t	processor_set_things(
		processor_set_t		pset,
		mach_port_t		**thing_list,
		mach_msg_type_number_t	*count,
		int			type);


/*
 *	Bootstrap the processor/pset system so the scheduler can run.
 */
void
pset_sys_bootstrap(void)
{
	register int	i;

	pset_init(&default_pset);
	for (i = 0; i < NCPUS; i++) {
		/*
		 *	Initialize processor data structures.
		 *	Note that cpu_to_processor(i) is processor_ptr[i].
		 */
		processor_ptr[i] = &processor_array[i];
		processor_init(processor_ptr[i], i);
	}
	master_processor = cpu_to_processor(master_cpu);
	default_pset.active = TRUE;
}

/*
 *	Initialize the given processor_set structure.
 */

void pset_init(
	register processor_set_t	pset)
{
	int	i;

	/* setup run-queues */
	simple_lock_init(&pset->runq.lock, ETAP_THREAD_PSET_RUNQ);
	pset->runq.count = 0;
	for (i = 0; i < NRQBM; i++) {
	    pset->runq.bitmap[i] = 0;
	}
	setbit(MAXPRI - IDLEPRI, pset->runq.bitmap); 
	pset->runq.highq = IDLEPRI;
	for (i = 0; i < NRQS; i++) {
	    queue_init(&(pset->runq.queues[i]));
	}

	queue_init(&pset->idle_queue);
	pset->idle_count = 0;
	simple_lock_init(&pset->idle_lock, ETAP_THREAD_PSET_IDLE);
	pset->mach_factor = pset->load_average = 0;
	pset->sched_load = 0;
	queue_init(&pset->processors);
	pset->processor_count = 0;
	simple_lock_init(&pset->processors_lock, ETAP_THREAD_PSET);
	queue_init(&pset->tasks);
	pset->task_count = 0;
	queue_init(&pset->threads);
	pset->thread_count = 0;
	pset->ref_count = 1;
	pset->active = FALSE;
	mutex_init(&pset->lock, ETAP_THREAD_PSET);
	pset->pset_self = IP_NULL;
	pset->pset_name_self = IP_NULL;
	pset->max_priority = MAXPRI_STANDARD;
	pset->policies = POLICY_TIMESHARE | POLICY_FIFO | POLICY_RR;
	pset->set_quantum = min_quantum;

	pset->quantum_adj_index = 0;
	simple_lock_init(&pset->quantum_adj_lock, ETAP_THREAD_PSET_QUANT);

	for (i = 0; i <= NCPUS; i++) {
	    pset->machine_quantum[i] = min_quantum;
	}

	pset->policy_default = POLICY_TIMESHARE;
	pset->policy_limit.ts.max_priority = MAXPRI_STANDARD;
	pset->policy_limit.rr.max_priority = MAXPRI_STANDARD;
	pset->policy_limit.fifo.max_priority = MAXPRI_STANDARD;
	pset->policy_base.ts.base_priority = BASEPRI_DEFAULT;
	pset->policy_base.rr.base_priority = BASEPRI_DEFAULT;
	pset->policy_base.rr.quantum = min_quantum;
	pset->policy_base.fifo.base_priority = BASEPRI_DEFAULT;
}

/*
 *	Initialize the given processor structure for the processor in
 *	the slot specified by slot_num.
 */
void
processor_init(
	register processor_t	pr,
	int			slot_num)
{
	int	i;

	/* setup run-queues */
	simple_lock_init(&pr->runq.lock, ETAP_THREAD_PROC_RUNQ);
	pr->runq.count = 0;
	for (i = 0; i < NRQBM; i++) {
	    pr->runq.bitmap[i] = 0;
	}
	setbit(MAXPRI - IDLEPRI, pr->runq.bitmap); 
	pr->runq.highq = IDLEPRI;
	for (i = 0; i < NRQS; i++) {
	    queue_init(&(pr->runq.queues[i]));
	}

	queue_init(&pr->processor_queue);
	pr->state = PROCESSOR_OFF_LINE;
	pr->next_thread = THREAD_NULL;
	pr->idle_thread = THREAD_NULL;
	pr->quantum = 0;
	pr->first_quantum = FALSE;
	pr->last_quantum = 0;
	pr->processor_set = PROCESSOR_SET_NULL;
	pr->processor_set_next = PROCESSOR_SET_NULL;
	queue_init(&pr->processors);
	simple_lock_init(&pr->lock, ETAP_THREAD_PROC);
	pr->processor_self = IP_NULL;
	pr->slot_num = slot_num;
}

/*
 *	pset_remove_processor() removes a processor from a processor_set.
 *	It can only be called on the current processor.  Caller must
 *	hold lock on current processor and processor set.
 */
void
pset_remove_processor(
	processor_set_t	pset,
	processor_t	processor)
{
	if (pset != processor->processor_set)
		panic("pset_remove_processor: wrong pset");

	queue_remove(&pset->processors, processor, processor_t, processors);
	processor->processor_set = PROCESSOR_SET_NULL;
	pset->processor_count--;
	quantum_set(pset);
}

/*
 *	pset_add_processor() adds a  processor to a processor_set.
 *	It can only be called on the current processor.  Caller must
 *	hold lock on curent processor and on pset.  No reference counting on
 *	processors.  Processor reference to pset is implicit.
 */
void
pset_add_processor(
	processor_set_t	pset,
	processor_t	processor)
{
	queue_enter(&pset->processors, processor, processor_t, processors);
	processor->processor_set = pset;
	pset->processor_count++;
	quantum_set(pset);
}

/*
 *	pset_remove_task() removes a task from a processor_set.
 *	Caller must hold locks on pset and task.  Pset reference count
 *	is not decremented; caller must explicitly pset_deallocate.
 */
void
pset_remove_task(
	processor_set_t	pset,
	task_t		task)
{
	if (pset != task->processor_set)
		return;

	queue_remove(&pset->tasks, task, task_t, pset_tasks);
	task->processor_set = PROCESSOR_SET_NULL;
	pset->task_count--;
}

/*
 *	pset_add_task() adds a  task to a processor_set.
 *	Caller must hold locks on pset and task.  Pset references to
 *	tasks are implicit.
 */
void
pset_add_task(
	processor_set_t	pset,
	task_t		task)
{
	queue_enter(&pset->tasks, task, task_t, pset_tasks);
	task->processor_set = pset;
	pset->task_count++;
	pset->ref_count++;
}

/*
 *	pset_remove_thread() removes a thread from a processor_set.
 *	Caller must hold locks on pset and thread.  Pset reference count
 *	is not decremented; caller must explicitly pset_deallocate.
 */
void
pset_remove_thread(
	processor_set_t	pset,
	thread_t	thread)
{
	queue_remove(&pset->threads, thread, thread_t, pset_threads);
	thread->processor_set = PROCESSOR_SET_NULL;
	pset->thread_count--;
}

/*
 *	pset_add_thread() adds a  thread to a processor_set.
 *	Caller must hold locks on pset and thread.  Pset references to
 *	threads are implicit.
 */
void
pset_add_thread(
	processor_set_t	pset,
	thread_t	thread)
{
	queue_enter(&pset->threads, thread, thread_t, pset_threads);
	thread->processor_set = pset;
	pset->thread_count++;
	pset->ref_count++;
}

/*
 *	thread_change_psets() changes the pset of a thread.  Caller must
 *	hold locks on both psets and thread.  The old pset must be
 *	explicitly pset_deallocat()'ed by caller.
 */
void
thread_change_psets(
	thread_t	thread,
	processor_set_t old_pset,
	processor_set_t new_pset)
{
	queue_remove(&old_pset->threads, thread, thread_t, pset_threads);
	old_pset->thread_count--;
	queue_enter(&new_pset->threads, thread, thread_t, pset_threads);
	thread->processor_set = new_pset;
	new_pset->thread_count++;
	new_pset->ref_count++;
}	

/*
 *	pset_deallocate:
 *
 *	Remove one reference to the processor set.  Destroy processor_set
 *	if this was the last reference.
 */
void
pset_deallocate(
	processor_set_t	pset)
{
	if (pset == PROCESSOR_SET_NULL)
		return;

	pset_lock(pset);
	if (--pset->ref_count > 0) {
		pset_unlock(pset);
		return;
	}

	panic("pset_deallocate: default_pset destroyed");
}

/*
 *	pset_reference:
 *
 *	Add one reference to the processor set.
 */
void
pset_reference(
	processor_set_t	pset)
{
	pset_lock(pset);
	pset->ref_count++;
	pset_unlock(pset);
}


kern_return_t
processor_info_count(
	processor_flavor_t	flavor,
	mach_msg_type_number_t	*count)
{
	kern_return_t		kr;

	switch (flavor) {
	case PROCESSOR_BASIC_INFO:
		*count = PROCESSOR_BASIC_INFO_COUNT;
		return KERN_SUCCESS;
	case PROCESSOR_CPU_LOAD_INFO:
		*count = PROCESSOR_CPU_LOAD_INFO_COUNT;
		return KERN_SUCCESS;
	default:
		kr = cpu_info_count(flavor, count);
		return kr;
	}
}


kern_return_t
processor_info(
	register processor_t	processor,
	processor_flavor_t	flavor,
	host_t			*host,
	processor_info_t	info,
	mach_msg_type_number_t	*count)
{
	register int	i, slot_num, state;
	register processor_basic_info_t		basic_info;
	register processor_cpu_load_info_t	cpu_load_info;
	kern_return_t   kr;

	if (processor == PROCESSOR_NULL)
		return(KERN_INVALID_ARGUMENT);

	slot_num = processor->slot_num;

	switch (flavor) {

	case PROCESSOR_BASIC_INFO:
	  {
	    if (*count < PROCESSOR_BASIC_INFO_COUNT)
	      return(KERN_FAILURE);

	    basic_info = (processor_basic_info_t) info;
	    basic_info->cpu_type = machine_slot[slot_num].cpu_type;
	    basic_info->cpu_subtype = machine_slot[slot_num].cpu_subtype;
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
	    return(KERN_SUCCESS);
	  }
	case PROCESSOR_CPU_LOAD_INFO:
	  {
	    if (*count < PROCESSOR_CPU_LOAD_INFO_COUNT)
	      return(KERN_FAILURE);

	    cpu_load_info = (processor_cpu_load_info_t) info;
	    for (i=0;i<CPU_STATE_MAX;i++)
	      cpu_load_info->cpu_ticks[i] = machine_slot[slot_num].cpu_ticks[i];

	    *count = PROCESSOR_CPU_LOAD_INFO_COUNT;
	    *host = &realhost;
	    return(KERN_SUCCESS);
	  }
	default:
	  {
	    kr=cpu_info(flavor, slot_num, info, count);
	    if (kr == KERN_SUCCESS)
		*host = &realhost;		   
	    return(kr);
	  }
	}
}

kern_return_t
processor_start(
	processor_t	processor)
{
	int	state;
	spl_t	s;
	kern_return_t	kr;

	if (processor == PROCESSOR_NULL)
		return(KERN_INVALID_ARGUMENT);

	if (processor == master_processor)
		return(cpu_start(processor->slot_num));

	s = splsched();
	processor_lock(processor);

	state = processor->state;
	if (state != PROCESSOR_OFF_LINE) {
		processor_unlock(processor);
		splx(s);
		return(KERN_FAILURE);
	}
	processor->state = PROCESSOR_START;
	processor_unlock(processor);
	splx(s);

	if (processor->next_thread == THREAD_NULL) {
		thread_t		thread;   
		extern void		start_cpu_thread(void);
	
		thread = kernel_thread_with_priority(kernel_task, MAXPRI_KERNBAND,
										start_cpu_thread, FALSE);   

		s = splsched();
		thread_lock(thread);
		thread_bind_locked(thread, processor);
		thread_go_locked(thread, THREAD_AWAKENED);
		(void)rem_runq(thread);
		processor->next_thread = thread;
		thread_unlock(thread);
		splx(s);
	}

	kr = cpu_start(processor->slot_num);

	if (kr != KERN_SUCCESS) {
		s = splsched();
		processor_lock(processor);
		processor->state = PROCESSOR_OFF_LINE;
		processor_unlock(processor);
		splx(s);
	}

	return(kr);
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

	return(cpu_control(processor->slot_num, info, count));
}

/*
 *	Precalculate the appropriate system quanta based on load.  The
 *	index into machine_quantum is the number of threads on the
 *	processor set queue.  It is limited to the number of processors in
 *	the set.
 */

void
quantum_set(
	processor_set_t		pset)
{
#if NCPUS > 1
	register int    i, ncpus;

	ncpus = pset->processor_count;

	for (i=1; i <= ncpus; i++)
		pset->machine_quantum[i] = ((min_quantum * ncpus) + (i / 2)) / i ;

	pset->machine_quantum[0] = pset->machine_quantum[1];

	i = (pset->runq.count > ncpus) ? ncpus : pset->runq.count;
	pset->set_quantum = pset->machine_quantum[i];
#else   /* NCPUS > 1 */
	default_pset.set_quantum = min_quantum;
#endif  /* NCPUS > 1 */
}
	    
kern_return_t
processor_set_create(
	host_t	host,
	processor_set_t	*new_set,
	processor_set_t	*new_name)
{
#ifdef	lint
	host++; new_set++; new_name++;
#endif	/* lint */
	return(KERN_FAILURE);
}

kern_return_t
processor_set_destroy(
	processor_set_t	pset)
{
#ifdef	lint
	pset++;
#endif	/* lint */
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

	*pset = processor->processor_set;
	pset_reference(*pset);
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

		pset_lock(pset);
		simple_lock(&pset->processors_lock);
		basic_info->processor_count = pset->processor_count;
		simple_unlock(&pset->processors_lock);
		basic_info->default_policy = pset->policy_default;
		pset_unlock(pset);

		*count = PROCESSOR_SET_BASIC_INFO_COUNT;
		*host = &realhost;
		return(KERN_SUCCESS);
	}
	else if (flavor == PROCESSOR_SET_TIMESHARE_DEFAULT) {
		register policy_timeshare_base_t	ts_base;

		if (*count < POLICY_TIMESHARE_BASE_COUNT)
			return(KERN_FAILURE);

		ts_base = (policy_timeshare_base_t) info;

		pset_lock(pset);
		*ts_base = pset->policy_base.ts;
		pset_unlock(pset);

		*count = POLICY_TIMESHARE_BASE_COUNT;
		*host = &realhost;
		return(KERN_SUCCESS);
	}
	else if (flavor == PROCESSOR_SET_FIFO_DEFAULT) {
		register policy_fifo_base_t		fifo_base;

		if (*count < POLICY_FIFO_BASE_COUNT)
			return(KERN_FAILURE);

		fifo_base = (policy_fifo_base_t) info;

		pset_lock(pset);
		*fifo_base = pset->policy_base.fifo;
		pset_unlock(pset);

		*count = POLICY_FIFO_BASE_COUNT;
		*host = &realhost;
		return(KERN_SUCCESS);
	}
	else if (flavor == PROCESSOR_SET_RR_DEFAULT) {
		register policy_rr_base_t		rr_base;

		if (*count < POLICY_RR_BASE_COUNT)
			return(KERN_FAILURE);

		rr_base = (policy_rr_base_t) info;

		pset_lock(pset);
		*rr_base = pset->policy_base.rr;
		pset_unlock(pset);

		*count = POLICY_RR_BASE_COUNT;
		*host = &realhost;
		return(KERN_SUCCESS);
	}
	else if (flavor == PROCESSOR_SET_TIMESHARE_LIMITS) {
		register policy_timeshare_limit_t	ts_limit;

		if (*count < POLICY_TIMESHARE_LIMIT_COUNT)
			return(KERN_FAILURE);

		ts_limit = (policy_timeshare_limit_t) info;

		pset_lock(pset);
		*ts_limit = pset->policy_limit.ts;
		pset_unlock(pset);

		*count = POLICY_TIMESHARE_LIMIT_COUNT;
		*host = &realhost;
		return(KERN_SUCCESS);
	}
	else if (flavor == PROCESSOR_SET_FIFO_LIMITS) {
		register policy_fifo_limit_t		fifo_limit;

		if (*count < POLICY_FIFO_LIMIT_COUNT)
			return(KERN_FAILURE);

		fifo_limit = (policy_fifo_limit_t) info;

		pset_lock(pset);
		*fifo_limit = pset->policy_limit.fifo;
		pset_unlock(pset);

		*count = POLICY_FIFO_LIMIT_COUNT;
		*host = &realhost;
		return(KERN_SUCCESS);
	}
	else if (flavor == PROCESSOR_SET_RR_LIMITS) {
		register policy_rr_limit_t		rr_limit;

		if (*count < POLICY_RR_LIMIT_COUNT)
			return(KERN_FAILURE);

		rr_limit = (policy_rr_limit_t) info;

		pset_lock(pset);
		*rr_limit = pset->policy_limit.rr;
		pset_unlock(pset);

		*count = POLICY_RR_LIMIT_COUNT;
		*host = &realhost;
		return(KERN_SUCCESS);
	}
	else if (flavor == PROCESSOR_SET_ENABLED_POLICIES) {
		register int				*enabled;

		if (*count < (sizeof(*enabled)/sizeof(int)))
			return(KERN_FAILURE);

		enabled = (int *) info;

		pset_lock(pset);
		*enabled = pset->policies;
		pset_unlock(pset);

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
        if (pset == PROCESSOR_SET_NULL)
                return (KERN_INVALID_PROCESSOR_SET);

        if (flavor == PROCESSOR_SET_LOAD_INFO) {
                register processor_set_load_info_t     load_info;

                if (*count < PROCESSOR_SET_LOAD_INFO_COUNT)
                        return(KERN_FAILURE);

                load_info = (processor_set_load_info_t) info;

                pset_lock(pset);
                load_info->task_count = pset->task_count;
                load_info->thread_count = pset->thread_count;
				simple_lock(&pset->processors_lock);
                load_info->mach_factor = pset->mach_factor;
                load_info->load_average = pset->load_average;
				simple_unlock(&pset->processors_lock);
                pset_unlock(pset);

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
	processor_set_t	pset,
	int		max_priority,
	boolean_t	change_threads)
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
	processor_set_t	pset,
	int		policy)
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
	processor_set_t	pset,
	int		policy,
	boolean_t	change_threads)
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
	processor_set_t		pset,
	mach_port_t		**thing_list,
	mach_msg_type_number_t	*count,
	int			type)
{
	unsigned int actual;	/* this many things */
	int i;

	vm_size_t size, size_needed;
	vm_offset_t addr;

	if (pset == PROCESSOR_SET_NULL)
		return KERN_INVALID_ARGUMENT;

	size = 0; addr = 0;

	for (;;) {
		pset_lock(pset);
		if (!pset->active) {
			pset_unlock(pset);
			return KERN_FAILURE;
		}

		if (type == THING_TASK)
			actual = pset->task_count;
		else
			actual = pset->thread_count;

		/* do we have the memory we need? */

		size_needed = actual * sizeof(mach_port_t);
		if (size_needed <= size)
			break;

		/* unlock the pset and allocate more memory */
		pset_unlock(pset);

		if (size != 0)
			kfree(addr, size);

		assert(size_needed > 0);
		size = size_needed;

		addr = kalloc(size);
		if (addr == 0)
			return KERN_RESOURCE_SHORTAGE;
	}

	/* OK, have memory and the processor_set is locked & active */

	switch (type) {
	    case THING_TASK: {
		task_t *tasks = (task_t *) addr;
		task_t task;

		for (i = 0, task = (task_t) queue_first(&pset->tasks);
		     i < actual;
		     i++, task = (task_t) queue_next(&task->pset_tasks)) {
			/* take ref for convert_task_to_port */
			task_reference(task);
			tasks[i] = task;
		}
		assert(queue_end(&pset->tasks, (queue_entry_t) task));
		break;
	    }

	    case THING_THREAD: {
		thread_act_t *thr_acts = (thread_act_t *) addr;
		thread_t thread;
		thread_act_t thr_act;
	    	queue_head_t *list;

		list = &pset->threads;
	    	thread = (thread_t) queue_first(list);
		i = 0;
	    	while (i < actual && !queue_end(list, (queue_entry_t)thread)) {
		  	thr_act = thread_lock_act(thread);
			if (thr_act && thr_act->ref_count > 0) {
				/* take ref for convert_act_to_port */
				act_locked_act_reference(thr_act);
				thr_acts[i] = thr_act;
				i++;
			}
			thread_unlock_act(thread);
			thread = (thread_t) queue_next(&thread->pset_threads);
		}
		if (i < actual) {
		  	actual = i;
			size_needed = actual * sizeof(mach_port_t);
		}
		break;
	    }
	}

	/* can unlock processor set now that we have the task/thread refs */
	pset_unlock(pset);

	if (actual == 0) {
		/* no things, so return null pointer and deallocate memory */
		*thing_list = 0;
		*count = 0;

		if (size != 0)
			kfree(addr, size);
	} else {
		/* if we allocated too much, must copy */

		if (size_needed < size) {
			vm_offset_t newaddr;

			newaddr = kalloc(size_needed);
			if (newaddr == 0) {
				switch (type) {
				    case THING_TASK: {
					task_t *tasks = (task_t *) addr;

					for (i = 0; i < actual; i++)
						task_deallocate(tasks[i]);
					break;
				    }

				    case THING_THREAD: {
					thread_t *threads = (thread_t *) addr;

					for (i = 0; i < actual; i++)
						thread_deallocate(threads[i]);
					break;
				    }
				}
				kfree(addr, size);
				return KERN_RESOURCE_SHORTAGE;
			}

			bcopy((char *) addr, (char *) newaddr, size_needed);
			kfree(addr, size);
			addr = newaddr;
		}

		*thing_list = (mach_port_t *) addr;
		*count = actual;

		/* do the conversion that Mig should handle */

		switch (type) {
		    case THING_TASK: {
			task_t *tasks = (task_t *) addr;

			for (i = 0; i < actual; i++)
				(*thing_list)[i] = convert_task_to_port(tasks[i]);
			break;
		    }

		    case THING_THREAD: {
			thread_act_t *thr_acts = (thread_act_t *) addr;

			for (i = 0; i < actual; i++)
			  	(*thing_list)[i] = convert_act_to_port(thr_acts[i]);
			break;
		    }
		}
	}

	return(KERN_SUCCESS);
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
kern_return_t
processor_set_threads(
	processor_set_t		pset,
	thread_array_t		*thread_list,
	mach_msg_type_number_t	*count)
{
    return(processor_set_things(pset, (mach_port_t **)thread_list, count, THING_THREAD));
}

/*
 *      processor_set_base:
 *
 *      Specify per-policy base priority for a processor set.  Set processor
 *	set default policy to the given policy. This affects newly created
 *      and assigned threads.  Optionally change existing ones.
 */
kern_return_t
processor_set_base(
	processor_set_t 	pset,
	policy_t             	policy,
        policy_base_t           base,
	boolean_t       	change)
{
	return (KERN_INVALID_ARGUMENT);
}

/*
 *      processor_set_limit:
 *
 *      Specify per-policy limits for a processor set.  This affects
 *      newly created and assigned threads.  Optionally change existing
 *      ones.
 */
kern_return_t
processor_set_limit(
	processor_set_t 	pset,
	policy_t		policy,
        policy_limit_t    	limit,
	boolean_t       	change)
{
	return (KERN_POLICY_LIMIT);
}

/*
 *	processor_set_policy_control
 *
 *	Controls the scheduling attributes governing the processor set.
 *	Allows control of enabled policies, and per-policy base and limit
 *	priorities.
 */
kern_return_t
processor_set_policy_control(
	processor_set_t		pset,
	int			flavor,
	processor_set_info_t	policy_info,
	mach_msg_type_number_t	count,
	boolean_t		change)
{
	return (KERN_INVALID_ARGUMENT);
}

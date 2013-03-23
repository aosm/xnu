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
#include <ppc/machine_routines.h>
#include <ppc/machine_cpu.h>
#include <ppc/exception.h>
#include <ppc/misc_protos.h>
#include <ppc/Firmware.h>
#include <vm/vm_page.h>
#include <ppc/pmap.h>
#include <ppc/proc_reg.h>
#include <kern/processor.h>

unsigned int max_cpus_initialized = 0;
extern int forcenap;

#define	MAX_CPUS_SET	0x1
#define	MAX_CPUS_WAIT	0x2

boolean_t get_interrupts_enabled(void);

/* Map memory map IO space */
vm_offset_t 
ml_io_map(
	vm_offset_t phys_addr, 
	vm_size_t size)
{
	return(io_map(phys_addr,size));
}

/* static memory allocation */
vm_offset_t 
ml_static_malloc(
	vm_size_t size)
{
	extern vm_offset_t static_memory_end;
	extern boolean_t pmap_initialized;
	vm_offset_t vaddr;

	if (pmap_initialized)
		return((vm_offset_t)NULL);
	else {
		vaddr = static_memory_end;
		static_memory_end = round_page_32(vaddr+size);
		return(vaddr);
	}
}

vm_offset_t
ml_static_ptovirt(
	vm_offset_t paddr)
{
	extern vm_offset_t static_memory_end;
	vm_offset_t vaddr;

	/* Static memory is map V=R */
	vaddr = paddr;
	if ( (vaddr < static_memory_end) && (pmap_extract(kernel_pmap, vaddr)==paddr) )
		return(vaddr);
	else
		return((vm_offset_t)NULL);
}

void
ml_static_mfree(
	vm_offset_t vaddr,
	vm_size_t size)
{
	vm_offset_t paddr_cur, vaddr_cur;

	for (vaddr_cur = round_page_32(vaddr);
	     vaddr_cur < trunc_page_32(vaddr+size);
	     vaddr_cur += PAGE_SIZE) {
		paddr_cur = pmap_extract(kernel_pmap, vaddr_cur);
		if (paddr_cur != (vm_offset_t)NULL) {
			vm_page_wire_count--;
			pmap_remove(kernel_pmap, (addr64_t)vaddr_cur, (addr64_t)(vaddr_cur+PAGE_SIZE));
			vm_page_create(paddr_cur>>12,(paddr_cur+PAGE_SIZE)>>12);
		}
	}
}

/* virtual to physical on wired pages */
vm_offset_t ml_vtophys(
	vm_offset_t vaddr)
{
	return(pmap_extract(kernel_pmap, vaddr));
}

/* Initialize Interrupt Handler */
void ml_install_interrupt_handler(
	void *nub,
	int source,
	void *target,
	IOInterruptHandler handler,
	void *refCon)
{
	int	current_cpu;
	boolean_t current_state;

	current_cpu = cpu_number();
	current_state = ml_get_interrupts_enabled();

	per_proc_info[current_cpu].interrupt_nub     = nub;
	per_proc_info[current_cpu].interrupt_source  = source;
	per_proc_info[current_cpu].interrupt_target  = target;
	per_proc_info[current_cpu].interrupt_handler = handler;
	per_proc_info[current_cpu].interrupt_refCon  = refCon;

	per_proc_info[current_cpu].interrupts_enabled = TRUE;  
	(void) ml_set_interrupts_enabled(current_state);

	initialize_screen(0, kPEAcquireScreen);
}

/* Initialize Interrupts */
void ml_init_interrupt(void)
{
	int	current_cpu;
	boolean_t current_state;

	current_state = ml_get_interrupts_enabled();

	current_cpu = cpu_number();
	per_proc_info[current_cpu].interrupts_enabled = TRUE;  
	(void) ml_set_interrupts_enabled(current_state);
}

/* Get Interrupts Enabled */
boolean_t ml_get_interrupts_enabled(void)
{
	return((mfmsr() & MASK(MSR_EE)) != 0);
}

/* Check if running at interrupt context */
boolean_t ml_at_interrupt_context(void)
{
	boolean_t	ret;
	boolean_t	current_state;

	current_state = ml_set_interrupts_enabled(FALSE);
 	ret = (per_proc_info[cpu_number()].istackptr == 0);	
	ml_set_interrupts_enabled(current_state);
	return(ret);
}

/* Generate a fake interrupt */
void ml_cause_interrupt(void)
{
	CreateFakeIO();
}

void ml_thread_policy(
	thread_t thread,
	unsigned policy_id,
	unsigned policy_info)
{
	if ((policy_id == MACHINE_GROUP) &&
		((per_proc_info[0].pf.Available) & pfSMPcap))
			thread_bind(thread, master_processor);

	if (policy_info & MACHINE_NETWORK_WORKLOOP) {
		spl_t		s = splsched();

		thread_lock(thread);

		thread->sched_mode |= TH_MODE_FORCEDPREEMPT;
		set_priority(thread, thread->priority + 1);

		thread_unlock(thread);
		splx(s);
	}
}

void machine_idle(void)
{
        if (per_proc_info[cpu_number()].interrupts_enabled == TRUE) {
	        int cur_decr;

	        machine_idle_ppc();

		/*
		 * protect against a lost decrementer trap
		 * if the current decrementer value is negative
		 * by more than 10 ticks, re-arm it since it's 
		 * unlikely to fire at this point... a hardware
		 * interrupt got us out of machine_idle and may
		 * also be contributing to this state
		 */
		cur_decr = isync_mfdec();

		if (cur_decr < -10) {
		        mtdec(1);
		}
	}
}

void
machine_signal_idle(
	processor_t processor)
{
	(void)cpu_signal(processor->slot_num, SIGPwake, 0, 0);
}

kern_return_t
ml_processor_register(
	ml_processor_info_t *processor_info,
	processor_t	    *processor,
	ipi_handler_t       *ipi_handler)
{
	kern_return_t ret;
	int target_cpu, cpu;
	int donap;

	if (processor_info->boot_cpu == FALSE) {
		 if (cpu_register(&target_cpu) != KERN_SUCCESS)
			return KERN_FAILURE;
	} else {
		/* boot_cpu is always 0 */
		target_cpu = 0;
	}

	per_proc_info[target_cpu].cpu_id = processor_info->cpu_id;
	per_proc_info[target_cpu].start_paddr = processor_info->start_paddr;

	donap = processor_info->supports_nap;		/* Assume we use requested nap */
	if(forcenap) donap = forcenap - 1;			/* If there was an override, use that */
	
	if(per_proc_info[target_cpu].pf.Available & pfCanNap)
	  if(donap) 
		per_proc_info[target_cpu].pf.Available |= pfWillNap;

	if(processor_info->time_base_enable !=  (void(*)(cpu_id_t, boolean_t ))NULL)
		per_proc_info[target_cpu].time_base_enable = processor_info->time_base_enable;
	else
		per_proc_info[target_cpu].time_base_enable = (void(*)(cpu_id_t, boolean_t ))NULL;
	
	if(target_cpu == cpu_number()) 
		__asm__ volatile("mtsprg 2,%0" : : "r" (per_proc_info[target_cpu].pf.Available));	/* Set live value */

	*processor = cpu_to_processor(target_cpu);
	*ipi_handler = cpu_signal_handler;

	return KERN_SUCCESS;
}

boolean_t
ml_enable_nap(int target_cpu, boolean_t nap_enabled)
{
    boolean_t prev_value = (per_proc_info[target_cpu].pf.Available & pfCanNap) && (per_proc_info[target_cpu].pf.Available & pfWillNap);
    
 	if(forcenap) nap_enabled = forcenap - 1;		/* If we are to force nap on or off, do it */
 
 	if(per_proc_info[target_cpu].pf.Available & pfCanNap) {				/* Can the processor nap? */
		if (nap_enabled) per_proc_info[target_cpu].pf.Available |= pfWillNap;	/* Is nap supported on this machine? */
		else per_proc_info[target_cpu].pf.Available &= ~pfWillNap;		/* Clear if not */
	}

	if(target_cpu == cpu_number()) 
		__asm__ volatile("mtsprg 2,%0" : : "r" (per_proc_info[target_cpu].pf.Available));	/* Set live value */
 
    return (prev_value);
}

void
ml_init_max_cpus(unsigned long max_cpus)
{
	boolean_t current_state;

	current_state = ml_set_interrupts_enabled(FALSE);
	if (max_cpus_initialized != MAX_CPUS_SET) {
		if (max_cpus > 0 && max_cpus < NCPUS)
			machine_info.max_cpus = max_cpus;
		if (max_cpus_initialized == MAX_CPUS_WAIT)
			wakeup((event_t)&max_cpus_initialized);
		max_cpus_initialized = MAX_CPUS_SET;
	}
	(void) ml_set_interrupts_enabled(current_state);
}

int
ml_get_max_cpus(void)
{
	boolean_t current_state;

	current_state = ml_set_interrupts_enabled(FALSE);
	if (max_cpus_initialized != MAX_CPUS_SET) {
		max_cpus_initialized = MAX_CPUS_WAIT;
		assert_wait((event_t)&max_cpus_initialized, THREAD_UNINT);
		(void)thread_block(THREAD_CONTINUE_NULL);
	}
	(void) ml_set_interrupts_enabled(current_state);
	return(machine_info.max_cpus);
}

int
ml_get_current_cpus(void)
{
	return machine_info.avail_cpus;
}

void
ml_cpu_get_info(ml_cpu_info_t *cpu_info)
{
  if (cpu_info == 0) return;
  
  cpu_info->vector_unit = (per_proc_info[0].pf.Available & pfAltivec) != 0;
  cpu_info->cache_line_size = per_proc_info[0].pf.lineSize;
  cpu_info->l1_icache_size = per_proc_info[0].pf.l1iSize;
  cpu_info->l1_dcache_size = per_proc_info[0].pf.l1dSize;
  
  if (per_proc_info[0].pf.Available & pfL2) {
    cpu_info->l2_settings = per_proc_info[0].pf.l2cr;
    cpu_info->l2_cache_size = per_proc_info[0].pf.l2Size;
  } else {
    cpu_info->l2_settings = 0;
    cpu_info->l2_cache_size = 0xFFFFFFFF;
  }
  if (per_proc_info[0].pf.Available & pfL3) {
    cpu_info->l3_settings = per_proc_info[0].pf.l3cr;
    cpu_info->l3_cache_size = per_proc_info[0].pf.l3Size;
  } else {
    cpu_info->l3_settings = 0;
    cpu_info->l3_cache_size = 0xFFFFFFFF;
  }
}

#define l2em 0x80000000
#define l3em 0x80000000

extern int real_ncpus;

int
ml_enable_cache_level(int cache_level, int enable)
{
  int old_mode;
  unsigned long available, ccr;
  
  if (real_ncpus != 1) return -1;
  
  available = per_proc_info[0].pf.Available;
  
  if ((cache_level == 2) && (available & pfL2)) {
    ccr = per_proc_info[0].pf.l2cr;
    old_mode = (ccr & l2em) ? TRUE : FALSE;
    if (old_mode != enable) {
      if (enable) ccr = per_proc_info[0].pf.l2crOriginal;
      else ccr = 0;
      per_proc_info[0].pf.l2cr = ccr;
      cacheInit();
    }
    
    return old_mode;
  }
  
  if ((cache_level == 3) && (available & pfL3)) {
    ccr = per_proc_info[0].pf.l3cr;
    old_mode = (ccr & l3em) ? TRUE : FALSE;
    if (old_mode != enable) {
      if (enable) ccr = per_proc_info[0].pf.l3crOriginal;
      else ccr = 0;
      per_proc_info[0].pf.l3cr = ccr;
      cacheInit();
    }
    
    return old_mode;
  }
  
  return -1;
}

void
init_ast_check(processor_t processor)
{}
                
void
cause_ast_check(
	processor_t		processor)
{
	if (	processor != current_processor()								&&
	    	per_proc_info[processor->slot_num].interrupts_enabled == TRUE	)
		cpu_signal(processor->slot_num, SIGPast, NULL, NULL);
}
              
thread_t        
switch_to_shutdown_context(
	thread_t	thread,
	void		(*doshutdown)(processor_t),
	processor_t	processor)
{
	CreateShutdownCTX();   
	return((thread_t)(per_proc_info[cpu_number()].old_thread));
}

int
set_be_bit()
{
	
	int mycpu;
	boolean_t current_state;

	current_state = ml_set_interrupts_enabled(FALSE);	/* Can't allow interruptions when mucking with per_proc flags */
	mycpu = cpu_number();
	per_proc_info[mycpu].cpu_flags |= traceBE;
	(void) ml_set_interrupts_enabled(current_state);
	return(1);
}

int
clr_be_bit()
{
	int mycpu;
	boolean_t current_state;

	current_state = ml_set_interrupts_enabled(FALSE);	/* Can't allow interruptions when mucking with per_proc flags */
	mycpu = cpu_number();
	per_proc_info[mycpu].cpu_flags &= ~traceBE;
	(void) ml_set_interrupts_enabled(current_state);
	return(1);
}

int
be_tracing()
{
  int mycpu = cpu_number();
  return(per_proc_info[mycpu].cpu_flags & traceBE);
}


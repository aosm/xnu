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
/*
 * @OSF_COPYRIGHT@
 */

#ifndef	_PPC_MACHINE_ROUTINES_H_
#define	_PPC_MACHINE_ROUTINES_H_

#include <mach/mach_types.h>
#include <mach/boolean.h>
#include <kern/kern_types.h>
#include <pexpert/pexpert.h>

#include <sys/appleapiopts.h>

/* Get Interrupts Enabled */
boolean_t ml_get_interrupts_enabled(void);

/* Set Interrupts Enabled */
boolean_t ml_set_interrupts_enabled(boolean_t enable);

/* Check if running at interrupt context */
boolean_t ml_at_interrupt_context(void);

/* Generate a fake interrupt */
void ml_cause_interrupt(void);

/* Type for the IPI Hander */
typedef void (*ipi_handler_t)(void);

/* Type for the Time Base Enable function */
typedef void (*time_base_enable_t)(cpu_id_t cpu_id, boolean_t enable);

/* enables (or disables) the processor nap mode the function returns the previous value*/
boolean_t ml_enable_nap(
        int target_cpu,
        boolean_t nap_enabled);

/* Put the processor to sleep */
void ml_ppc_sleep(void);
void ml_get_timebase(unsigned long long *timstamp);
void ml_sense__nmi(void);

int ml_enable_cache_level(int cache_level, int enable);

void ml_static_mfree(
	vm_offset_t,
	vm_size_t);
        
/* Init Interrupts */
void ml_install_interrupt_handler(
	void *nub,
	int source,
	void *target,
	IOInterruptHandler handler,
	void *refCon);
               
#ifdef __APPLE_API_UNSTABLE

vm_offset_t
ml_static_ptovirt(
	vm_offset_t);

/* virtual to physical on wired pages */
vm_offset_t ml_vtophys(
	vm_offset_t vaddr);

/* PCI config cycle probing */
boolean_t ml_probe_read(
	vm_offset_t paddr,
	unsigned int *val);

/* Read physical address byte */
unsigned int ml_phys_read_byte(
	vm_offset_t paddr);

/* Read physical address */
unsigned int ml_phys_read(
	vm_offset_t paddr);

/* Write physical address byte */
void ml_phys_write_byte(
	vm_offset_t paddr, unsigned int data);

/* Write physical address */
void ml_phys_write(
	vm_offset_t paddr, unsigned int data);

/* Struct for ml_processor_register */
struct ml_processor_info_t {
	cpu_id_t			cpu_id;
	boolean_t			boot_cpu;
	vm_offset_t			start_paddr;
	boolean_t			supports_nap;
	unsigned long		l2cr_value;
	time_base_enable_t	time_base_enable;
};

typedef struct ml_processor_info_t ml_processor_info_t;

/* Register a processor */
kern_return_t ml_processor_register(
	ml_processor_info_t *processor_info,
	processor_t *processor,
	ipi_handler_t *ipi_handler);

#endif /* __APPLE_API_UNSTABLE */

#ifdef  __APPLE_API_PRIVATE
#if	defined(PEXPERT_KERNEL_PRIVATE) || defined(MACH_KERNEL_PRIVATE)
/* IO memory map services */

/* Map memory map IO space */
vm_offset_t ml_io_map(
	vm_offset_t phys_addr, 
	vm_size_t size);

/* boot memory allocation */
vm_offset_t ml_static_malloc(
	vm_size_t size);

#endif /* PEXPERT_KERNEL_PRIVATE || MACH_KERNEL_PRIVATE */

#ifdef	MACH_KERNEL_PRIVATE
void	ml_init_interrupt(void);

boolean_t fake_get_interrupts_enabled(void);

boolean_t fake_set_interrupts_enabled(
	boolean_t enable);

/* check pending timers */
void machine_clock_assist(void);

void machine_idle(void);

void machine_signal_idle(
	processor_t processor);

void cacheInit(void);

void cacheDisable(void);

void ml_thrm_init(void);
unsigned int ml_read_temp(void);

void ml_thrm_set(	
	unsigned int low, 
	unsigned int high);

unsigned int ml_throttle(
	unsigned int);

#endif /* MACH_KERNEL_PRIVATE */

void ml_thread_policy(
	thread_t thread,
	unsigned policy_id,
	unsigned policy_info);

#define MACHINE_GROUP				0x00000001
#define MACHINE_NETWORK_GROUP		0x10000000 
#define MACHINE_NETWORK_WORKLOOP	0x00000001
#define MACHINE_NETWORK_NETISR		0x00000002

/* Initialize the maximum number of CPUs */
void ml_init_max_cpus(
	unsigned long max_cpus);

/* Return the maximum number of CPUs set by ml_init_max_cpus() */
int ml_get_max_cpus(
	void);

/* Return the current number of CPUs */
int ml_get_current_cpus(
	void);

/* Struct for ml_cpu_get_info */
struct ml_cpu_info {
	unsigned long		vector_unit;
	unsigned long		cache_line_size;
	unsigned long		l1_icache_size;
	unsigned long		l1_dcache_size;
	unsigned long		l2_settings;
	unsigned long		l2_cache_size;
	unsigned long		l3_settings;
	unsigned long		l3_cache_size;
};

typedef struct ml_cpu_info ml_cpu_info_t;

/* Get processor info */
void ml_cpu_get_info(ml_cpu_info_t *cpu_info);

void ml_set_processor_speed(unsigned long speed);
void ml_set_processor_voltage(unsigned long voltage);

#endif /* __APPLE_API_PRIVATE */

#endif /* _PPC_MACHINE_ROUTINES_H_ */

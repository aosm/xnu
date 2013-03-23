/*
 * Copyright (c) 2000-2009 Apple Inc. All rights reserved.
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

#include <i386/pmap.h>
#include <i386/proc_reg.h>
#include <i386/mp_desc.h>
#include <i386/misc_protos.h>
#include <i386/mp.h>
#include <i386/cpu_data.h>
#include <i386/mtrr.h>
#if CONFIG_VMX
#include <i386/vmx/vmx_cpu.h>
#endif
#include <i386/acpi.h>
#include <i386/fpu.h>
#include <i386/lapic.h>
#include <i386/mp.h>
#include <i386/mp_desc.h>
#include <i386/serial_io.h>
#if CONFIG_MCA
#include <i386/machine_check.h>
#endif
#include <i386/pmCPU.h>

#include <kern/cpu_data.h>
#include <console/serial_protos.h>

#if HIBERNATION
#include <IOKit/IOHibernatePrivate.h>
#endif
#include <IOKit/IOPlatformExpert.h>

#if CONFIG_SLEEP
extern void	acpi_sleep_cpu(acpi_sleep_callback, void * refcon);
extern void acpi_wake_prot(void);
#endif

extern void 	fpinit(void);

vm_offset_t
acpi_install_wake_handler(void)
{
#if CONFIG_SLEEP
	install_real_mode_bootstrap(acpi_wake_prot);
	return REAL_MODE_BOOTSTRAP_OFFSET;
#else
	return 0;
#endif
}

#if HIBERNATION
struct acpi_hibernate_callback_data {
	acpi_sleep_callback func;
	void *refcon;
};
typedef struct acpi_hibernate_callback_data acpi_hibernate_callback_data_t;

#if CONFIG_SLEEP
static void
acpi_hibernate(void *refcon)
{
	uint32_t mode;

	acpi_hibernate_callback_data_t *data =
		(acpi_hibernate_callback_data_t *)refcon;

	if (current_cpu_datap()->cpu_hibernate) 
	{
#if defined(__i386__)
		cpu_IA32e_enable(current_cpu_datap());
#endif

		mode = hibernate_write_image();

		if( mode == kIOHibernatePostWriteHalt )
		{
			// off
			HIBLOG("power off\n");
			if (PE_halt_restart) (*PE_halt_restart)(kPEHaltCPU);
		}
		else if( mode == kIOHibernatePostWriteRestart )
		{
			// restart
			HIBLOG("restart\n");
			if (PE_halt_restart) (*PE_halt_restart)(kPERestartCPU);
		}
		else
		{
			// sleep
			HIBLOG("sleep\n");
	
			// should we come back via regular wake, set the state in memory.
			cpu_datap(0)->cpu_hibernate = 0;			
		}

#if defined(__i386__)
		/*
		 * If we're in 64-bit mode, drop back into legacy mode during sleep.
		 */
		cpu_IA32e_disable(current_cpu_datap());
#endif
	}

	(data->func)(data->refcon);

	/* should never get here! */
}
#endif /* CONFIG_SLEEP */
#endif /* HIBERNATION */

static uint64_t		acpi_sleep_abstime;
extern void			slave_pstart(void);

void
acpi_sleep_kernel(acpi_sleep_callback func, void *refcon)
{
#if HIBERNATION
	acpi_hibernate_callback_data_t data;
#endif
	boolean_t did_hibernate;
	unsigned int	cpu;
	kern_return_t	rc;
	unsigned int	my_cpu;

	kprintf("acpi_sleep_kernel hib=%d\n",
			current_cpu_datap()->cpu_hibernate);

    	/* Get all CPUs to be in the "off" state */
    	my_cpu = cpu_number();
	for (cpu = 0; cpu < real_ncpus; cpu += 1) {
	    	if (cpu == my_cpu)
			continue;
		rc = pmCPUExitHaltToOff(cpu);
		if (rc != KERN_SUCCESS)
			panic("Error %d trying to transition CPU %d to OFF",
			      rc, cpu);
	}

	/* shutdown local APIC before passing control to BIOS */
	lapic_shutdown();

#if HIBERNATION
	data.func = func;
	data.refcon = refcon;
#endif

	/* Save power management timer state */
	pmTimerSave();

#if CONFIG_VMX
	/* 
	 * Turn off VT, otherwise switching to legacy mode will fail
	 */
	vmx_suspend();
#endif

#if defined(__i386__)
	/*
	 * If we're in 64-bit mode, drop back into legacy mode during sleep.
	 */
	cpu_IA32e_disable(current_cpu_datap());
#endif

	acpi_sleep_abstime = mach_absolute_time();

#if CONFIG_SLEEP
	/*
	 * Save master CPU state and sleep platform.
	 * Will not return until platform is woken up,
	 * or if sleep failed.
	 */
#ifdef __x86_64__
	uint64_t old_cr3 = x86_64_pre_sleep();
#endif
#if HIBERNATION
	acpi_sleep_cpu(acpi_hibernate, &data);
#else
	acpi_sleep_cpu(func, refcon);
#endif
#ifdef __x86_64__
	x86_64_post_sleep(old_cr3);
#endif

#endif /* CONFIG_SLEEP */

	/* Reset UART if kprintf is enabled.
	 * However kprintf should not be used before rtc_sleep_wakeup()
	 * for compatibility with firewire kprintf.
	 */

	if (FALSE == disable_serial_output)
		serial_init();

#if HIBERNATION
	if (current_cpu_datap()->cpu_hibernate) {
#if defined(__i386__)
		int i;
		for (i = 0; i < PMAP_NWINDOWS; i++)
			*current_cpu_datap()->cpu_pmap->mapwindow[i].prv_CMAP = 0;
#endif
		did_hibernate = TRUE;

	} else
#endif 
	{
		did_hibernate = FALSE;
	}

	/* Re-enable mode (including 64-bit if applicable) */
	cpu_mode_init(current_cpu_datap());

#if CONFIG_MCA
	/* Re-enable machine check handling */
	mca_cpu_init();
#endif

	/* restore MTRR settings */
	mtrr_update_cpu();

#if CONFIG_VMX
	/* 
	 * Restore VT mode
	 */
	vmx_resume();
#endif

	/* set up PAT following boot processor power up */
	pat_init();

	/*
	 * Go through all of the CPUs and mark them as requiring
	 * a full restart.
	 */
	pmMarkAllCPUsOff();

	/* let the realtime clock reset */
	rtc_sleep_wakeup(acpi_sleep_abstime);

	if (did_hibernate){
		hibernate_machine_init();
		current_cpu_datap()->cpu_hibernate = 0;
	}
	/* re-enable and re-init local apic */
	if (lapic_probe())
		lapic_configure();

	/* Restore power management register state */
	pmCPUMarkRunning(current_cpu_datap());

	/* Restore power management timer state */
	pmTimerRestore();

	/* Restart tick interrupts from the LAPIC timer */
	rtc_lapic_start_ticking();

	fpinit();
	clear_fpu();

#if HIBERNATION
#ifdef __i386__
	/* The image is written out using the copy engine, which disables
	 * preemption. Since the copy engine writes out the page which contains
	 * the preemption variable when it is disabled, we need to explicitly
	 * enable it here */
	if (did_hibernate)
		enable_preemption();
#endif

	kprintf("ret from acpi_sleep_cpu hib=%d\n", did_hibernate);
#endif

#if CONFIG_SLEEP
	/* Becase we don't save the bootstrap page, and we share it
	 * between sleep and mp slave init, we need to recreate it 
	 * after coming back from sleep or hibernate */
	install_real_mode_bootstrap(slave_pstart);
#endif
}

extern char real_mode_bootstrap_end[];
extern char real_mode_bootstrap_base[];

void
install_real_mode_bootstrap(void *prot_entry)
{
	/*
	 * Copy the boot entry code to the real-mode vector area REAL_MODE_BOOTSTRAP_OFFSET.
	 * This is in page 1 which has been reserved for this purpose by
	 * machine_startup() from the boot processor.
	 * The slave boot code is responsible for switching to protected
	 * mode and then jumping to the common startup, _start().
	 */
	bcopy_phys(kvtophys((vm_offset_t) real_mode_bootstrap_base),
		   (addr64_t) REAL_MODE_BOOTSTRAP_OFFSET,
		   real_mode_bootstrap_end-real_mode_bootstrap_base);

	/*
	 * Set the location at the base of the stack to point to the
	 * common startup entry.
	 */
	ml_phys_write_word(
		PROT_MODE_START+REAL_MODE_BOOTSTRAP_OFFSET,
		(unsigned int)kvtophys((vm_offset_t)prot_entry));
	
	/* Flush caches */
	__asm__("wbinvd");
}


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

#include <debug.h>
#include <mach_kdb.h>
#include <mach_kdp.h>

#include <kern/misc_protos.h>
#include <kern/thread.h>
#include <kern/processor.h>
#include <machine/machine_routines.h>
#include <ppc/boot.h>
#include <ppc/proc_reg.h>
#include <ppc/misc_protos.h>
#include <ppc/pmap.h>
#include <ppc/new_screen.h>
#include <ppc/exception.h>
#include <ppc/Firmware.h>
#include <ppc/savearea.h>
#include <ppc/low_trace.h>
#include <ppc/Diagnostics.h>
#include <ppc/mem.h>

#include <pexpert/pexpert.h>

extern const char version[];
extern const char version_variant[];

extern unsigned int intstack_top_ss;	/* declared in start.s */
extern unsigned int debstackptr;	/* declared in start.s */
extern unsigned int debstack_top_ss;	/* declared in start.s */

extern void thandler(void);     /* trap handler */
extern void ihandler(void);     /* interrupt handler */
extern void shandler(void);     /* syscall handler */
extern void chandler(void);     /* system choke */
extern void fpu_switch(void);   /* fp handler */
extern void vec_switch(void);   /* vector handler */
extern void atomic_switch_trap(void);   /* fast path atomic thread switch */

void (*exception_handlers[])(void) = {
	thandler,	/* 0x000   INVALID EXCEPTION (T_IN_VAIN) */
	thandler,	/* 0x100   System reset (T_RESET) */
	thandler,	/* 0x200   Machine check (T_MACHINE_CHECK) */
	thandler,	/* 0x300   Data access (T_DATA_ACCESS) */
	thandler,	/* 0x400   Instruction access (T_INSTRUCTION_ACCESS) */
	ihandler,	/* 0x500   External interrupt (T_INTERRUPT) */
	thandler,	/* 0x600   Alignment (T_ALIGNMENT) */
	thandler,	/* 0x700   fp exc, ill/priv instr, trap  (T_PROGRAM) */
	fpu_switch,	/* 0x800   Floating point disabled (T_FP_UNAVAILABLE) */
	ihandler,	/* 0x900   Decrementer (T_DECREMENTER) */
	thandler,	/* 0xA00   I/O controller interface (T_IO_ERROR) */
	thandler,	/* 0xB00   INVALID EXCEPTION (T_RESERVED) */
	shandler,	/* 0xC00   System call exception (T_SYSTEM_CALL) */
	thandler,	/* 0xD00   Trace (T_TRACE) */
	thandler,	/* 0xE00   FP assist (T_FP_ASSIST) */
	thandler,	/* 0xF00   Performance monitor (T_PERF_MON) */
	vec_switch,	/* 0xF20   VMX (T_VMX) */
	thandler,	/* 0x1000  INVALID EXCEPTION (T_INVALID_EXCP0) */
	thandler,	/* 0x1100  INVALID EXCEPTION (T_INVALID_EXCP1) */
	thandler,	/* 0x1200  INVALID EXCEPTION (T_INVALID_EXCP2) */
	thandler,	/* 0x1300  instruction breakpoint (T_INSTRUCTION_BKPT) */
	ihandler,	/* 0x1400  system management (T_SYSTEM_MANAGEMENT) */
	thandler,	/* 0x1600  Altivec Assist (T_ALTIVEC_ASSIST) */
	ihandler,	/* 0x1700  Thermal interruption (T_THERMAL) */
	thandler,	/* 0x1800  INVALID EXCEPTION (T_INVALID_EXCP5) */
	thandler,	/* 0x1900  INVALID EXCEPTION (T_INVALID_EXCP6) */
	thandler,	/* 0x1A00  INVALID EXCEPTION (T_INVALID_EXCP7) */
	thandler,	/* 0x1B00  INVALID EXCEPTION (T_INVALID_EXCP8) */
	thandler,	/* 0x1C00  INVALID EXCEPTION (T_INVALID_EXCP9) */
	thandler,	/* 0x1D00  INVALID EXCEPTION (T_INVALID_EXCP10) */
	thandler,	/* 0x1E00  INVALID EXCEPTION (T_INVALID_EXCP11) */
	thandler,	/* 0x1F00  INVALID EXCEPTION (T_INVALID_EXCP12) */
	thandler,	/* 0x1F00  INVALID EXCEPTION (T_INVALID_EXCP13) */
	thandler,	/* 0x2000  Run Mode/Trace (T_RUNMODE_TRACE) */

	ihandler,	/* Software  Signal processor (T_SIGP) */
	thandler,	/* Software  Preemption (T_PREEMPT) */
	ihandler,	/* Software  INVALID EXCEPTION (T_CSWITCH) */ 
	ihandler,	/* Software  Shutdown Context (T_SHUTDOWN) */
	chandler	/* Software  System choke (crash) (T_CHOKE) */
};

int pc_trace_buf[1024] = {0};
int pc_trace_cnt = 1024;

void ppc_init(boot_args *args)
{
	int i;
	unsigned long *src,*dst;
	char *str;
	unsigned long	addr, videoAddr;
	unsigned int	maxmem;
	unsigned int	cputrace;
	bat_t		    bat;
	extern vm_offset_t static_memory_end;
	
	/*
	 * Setup per_proc info for first cpu.
	 */

	per_proc_info[0].cpu_number = 0;
	per_proc_info[0].cpu_flags = 0;
	per_proc_info[0].istackptr = 0;	/* we're on the interrupt stack */
	per_proc_info[0].intstack_top_ss = intstack_top_ss;
	per_proc_info[0].debstackptr = debstackptr;
	per_proc_info[0].debstack_top_ss = debstack_top_ss;
	per_proc_info[0].interrupts_enabled = 0;
	per_proc_info[0].active_kloaded = (unsigned int)
		&active_kloaded[0];
	set_machine_current_thread(&pageout_thread);
	set_machine_current_act(&pageout_act);
	pageout_thread.top_act = &pageout_act;
	pageout_act.thread = &pageout_thread;
	per_proc_info[0].pp_preemption_count = 1;
	per_proc_info[0].pp_simple_lock_count = 0;
	per_proc_info[0].pp_interrupt_level = 0;
	per_proc_info[0].active_stacks = (unsigned int)
		&active_stacks[0];
	per_proc_info[0].need_ast = (unsigned int)
		&need_ast[0];
	per_proc_info[0].FPU_owner = 0;
	per_proc_info[0].VMX_owner = 0;

	machine_slot[0].is_cpu = TRUE;

	cpu_init();

	/*
	 * Setup some processor related structures to satisfy funnels.
	 * Must be done before using unparallelized device drivers.
	 */
	processor_ptr[0] = &processor_array[0];
	master_cpu = 0;
	master_processor = cpu_to_processor(master_cpu);

	/* Set up segment registers as VM through space 0 */
	for (i=0; i<=15; i++) {
	  isync();
	  mtsrin((KERNEL_SEG_REG0_VALUE | (i << 20)), i * 0x10000000);
	  sync();
	}

	static_memory_end = round_page(args->topOfKernelData);;
        /* Get platform expert set up */
	PE_init_platform(FALSE, args);


	/* This is how the BATs get configured */
	/* IBAT[0] maps Segment 0 1:1 */
	/* DBAT[0] maps Segment 0 1:1 */
	/* DBAT[2] maps the I/O Segment 1:1 */
	/* DBAT[3] maps the Video Segment 1:1 */


	/* Initialize shadow IBATs */
	shadow_BAT.IBATs[0].upper=BAT_INVALID;
	shadow_BAT.IBATs[0].lower=BAT_INVALID;
	shadow_BAT.IBATs[1].upper=BAT_INVALID;
	shadow_BAT.IBATs[1].lower=BAT_INVALID;
	shadow_BAT.IBATs[2].upper=BAT_INVALID;
	shadow_BAT.IBATs[2].lower=BAT_INVALID;
	shadow_BAT.IBATs[3].upper=BAT_INVALID;
	shadow_BAT.IBATs[3].lower=BAT_INVALID;

	/* Initialize shadow DBATs */
	shadow_BAT.DBATs[0].upper=BAT_INVALID;
	shadow_BAT.DBATs[0].lower=BAT_INVALID;
	shadow_BAT.DBATs[1].upper=BAT_INVALID;
	shadow_BAT.DBATs[1].lower=BAT_INVALID;
	shadow_BAT.DBATs[2].upper=BAT_INVALID;
	shadow_BAT.DBATs[2].lower=BAT_INVALID;
	shadow_BAT.DBATs[3].upper=BAT_INVALID;
	shadow_BAT.DBATs[3].lower=BAT_INVALID;


	/* If v_baseAddr is non zero, use DBAT3 to map the video segment */
	videoAddr = args->Video.v_baseAddr & 0xF0000000;
	if (videoAddr) {
		/* start off specifying 1-1 mapping of video seg */
		bat.upper.word	     = videoAddr;
		bat.lower.word	     = videoAddr;
		
		bat.upper.bits.bl    = 0x7ff;	/* size = 256M */
		bat.upper.bits.vs    = 1;
		bat.upper.bits.vp    = 0;
		
		bat.lower.bits.wimg  = PTE_WIMG_IO;
		bat.lower.bits.pp    = 2;	/* read/write access */
				
		shadow_BAT.DBATs[3].upper = bat.upper.word;
		shadow_BAT.DBATs[3].lower = bat.lower.word;
		
		sync();isync();
		
		mtdbatu(3, BAT_INVALID); /* invalidate old mapping */
		mtdbatl(3, bat.lower.word);
		mtdbatu(3, bat.upper.word);
		sync();isync();
	}
	
	/* Use DBAT2 to map the io segment */
	addr = get_io_base_addr() & 0xF0000000;
	if (addr != videoAddr) {
		/* start off specifying 1-1 mapping of io seg */
		bat.upper.word	     = addr;
		bat.lower.word	     = addr;
		
		bat.upper.bits.bl    = 0x7ff;	/* size = 256M */
		bat.upper.bits.vs    = 1;
		bat.upper.bits.vp    = 0;
		
		bat.lower.bits.wimg  = PTE_WIMG_IO;
		bat.lower.bits.pp    = 2;	/* read/write access */
				
		shadow_BAT.DBATs[2].upper = bat.upper.word;
		shadow_BAT.DBATs[2].lower = bat.lower.word;
		
		sync();isync();
		mtdbatu(2, BAT_INVALID); /* invalidate old mapping */
		mtdbatl(2, bat.lower.word);
		mtdbatu(2, bat.upper.word);
		sync();isync();
	}

	if (!PE_parse_boot_arg("diag", &dgWork.dgFlags)) dgWork.dgFlags=0;	/* Set diagnostic flags */
	if(dgWork.dgFlags & enaExpTrace) trcWork.traceMask = 0xFFFFFFFF;	/* If tracing requested, enable it */

	if(PE_parse_boot_arg("ctrc", &cputrace)) {							/* See if tracing is limited to a specific cpu */
		trcWork.traceMask = (trcWork.traceMask & 0xFFFFFFF0) | (cputrace & 0xF);	/* Limit to 4 */
	}

#if 0
	GratefulDebInit((bootBumbleC *)&(args->Video));	/* Initialize the GratefulDeb debugger */
#endif

	printf_init();						/* Init this in case we need debugger */
	panic_init();						/* Init this in case we need debugger */

	/* setup debugging output if one has been chosen */
	PE_init_kprintf(FALSE);
	kprintf("kprintf initialized\n");

	/* create the console for verbose or pretty mode */
	PE_create_console();

	/* setup console output */
	PE_init_printf(FALSE);

	kprintf("version_variant = %s\n", version_variant);
	kprintf("version         = %s\n", version);

#if DEBUG
	printf("\n\n\nThis program was compiled using gcc %d.%d for powerpc\n",
	       __GNUC__,__GNUC_MINOR__);

	/* Processor version information */
	{       
		unsigned int pvr;
		__asm__ ("mfpvr %0" : "=r" (pvr));
		printf("processor version register : 0x%08x\n",pvr);
	}
	for (i = 0; i < kMaxDRAMBanks; i++) {
		if (args->PhysicalDRAM[i].size) 
			printf("DRAM at 0x%08x size 0x%08x\n",
			       args->PhysicalDRAM[i].base,
			       args->PhysicalDRAM[i].size);
	}
#endif /* DEBUG */

	/*   
	 * VM initialization, after this we're using page tables...
	 */
	if (!PE_parse_boot_arg("maxmem", &maxmem))
		maxmem=0;
	else
		maxmem = maxmem * (1024 * 1024);

	ppc_vm_init(maxmem, args);

	PE_init_platform(TRUE, args);
	
	machine_startup(args);
}

ppc_init_cpu(
	struct per_proc_info *proc_info)
{
	int i;

	if(!(proc_info->next_savearea)) 		/* Do we have a savearea set up already? */
		proc_info->next_savearea = (savearea *)save_get_init();	/* Get a savearea  */
	
	cpu_init();

	proc_info->pp_preemption_count = 1;
	proc_info->pp_simple_lock_count = 0;
	proc_info->pp_interrupt_level = 0;

	proc_info->Lastpmap = 0;				/* Clear last used space */

	/* Set up segment registers as VM through space 0 */
	for (i=0; i<=15; i++) {
	  isync();
	  mtsrin((KERNEL_SEG_REG0_VALUE | (i << 20)), i * 0x10000000);
	  sync();
	}

	ppc_vm_cpu_init(proc_info);

	ml_thrm_init();							/* Start thermal monitoring on this processor */

	slave_main();
}

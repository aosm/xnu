/*
 * Copyright (c) 2005-2006 Apple Computer, Inc. All rights reserved.
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

#include <string.h>
#include <mach/vm_param.h>
#include <mach/vm_prot.h>
#include <mach/machine.h>
#include <mach/time_value.h>
#include <kern/spl.h>
#include <kern/assert.h>
#include <kern/debug.h>
#include <kern/misc_protos.h>
#include <kern/startup.h>
#include <kern/clock.h>
#include <kern/cpu_data.h>
#include <kern/processor.h>
#include <vm/vm_page.h>
#include <vm/pmap.h>
#include <vm/vm_kern.h>
#include <i386/pmap.h>
#include <i386/misc_protos.h>
#include <i386/cpuid.h>
#include <i386/mp.h>
#include <i386/machine_cpu.h>
#include <i386/machine_routines.h>
#include <i386/io_map_entries.h>
#include <architecture/i386/pio.h>
#include <i386/cpuid.h>
#include <i386/apic.h>
#include <i386/tsc.h>
#include <i386/hpet.h>
#include <i386/pmCPU.h>
#include <i386/cpu_topology.h>
#include <i386/cpu_threads.h>
#include <pexpert/device_tree.h>
#if	MACH_KDB
#include <i386/db_machdep.h>
#endif
#if	MACH_KDB
#include <ddb/db_aout.h>
#include <ddb/db_access.h>
#include <ddb/db_sym.h>
#include <ddb/db_variables.h>
#include <ddb/db_command.h>
#include <ddb/db_output.h>
#include <ddb/db_expr.h>
#endif /* MACH_KDB */
#include <ddb/tr.h>

/* Decimal powers: */
#define kilo (1000ULL)
#define Mega (kilo * kilo)
#define Giga (kilo * Mega)
#define Tera (kilo * Giga)
#define Peta (kilo * Tera)

uint32_t hpetArea = 0;			
uint32_t hpetAreap = 0;			
uint64_t hpetFemto = 0;
uint64_t hpetFreq = 0;
uint64_t hpetCvt = 0;			/* (TAKE OUT LATER)  */
uint64_t hpetCvtt2n = 0;
uint64_t hpetCvtn2t = 0;
uint64_t tsc2hpet = 0;
uint64_t hpet2tsc = 0;
uint64_t bus2hpet = 0;
uint64_t hpet2bus = 0;

uint32_t rcbaArea = 0;			
uint32_t rcbaAreap = 0;			

static int (*hpet_req)(uint32_t apicid, void *arg, hpetRequest_t *hpet) = NULL;
static void *hpet_arg = NULL;

#if DEBUG
#define DBG(x...)	kprintf("DBG: " x)
#else
#define DBG(x...)
#endif

int
hpet_register_callback(int (*hpet_reqst)(uint32_t apicid,
					 void *arg,
					 hpetRequest_t *hpet),
		       void *arg)
{
    hpet_req = hpet_reqst;
    hpet_arg = arg;
    return(0);
}

/*
 * This routine is called to obtain an HPET and have it assigned
 * to a CPU.  It returns 0 if successful and non-zero if one could
 * not be assigned.
 */
int
hpet_request(uint32_t cpu)
{
    hpetRequest_t	hpetReq;
    int			rc;
    x86_lcpu_t		*lcpu;
    x86_core_t		*core;
    x86_pkg_t		*pkg;
    boolean_t		enabled;

    if (hpet_req == NULL) {
	return(-1);
    }

    rc = (*hpet_req)(ml_get_apicid(cpu), hpet_arg, &hpetReq);
    if (rc != 0) {
	return(rc);
    }

    enabled = ml_set_interrupts_enabled(FALSE);
    lcpu = cpu_to_lcpu(cpu);
    core = lcpu->core;
    pkg  = core->package;

    /*
     * Compute the address of the HPET.
     */
    core->Hpet = (hpetTimer_t *)((uint8_t *)hpetArea + hpetReq.hpetOffset);
    core->HpetVec = hpetReq.hpetVector;

    /*
     * Enable interrupts
     */
    core->Hpet->Config |= Tn_INT_ENB_CNF;

    /*
     * Save the configuration
     */
    core->HpetCfg = core->Hpet->Config;
    core->HpetCmp = 0;

    /*
     * If the CPU is the "primary" for the package, then
     * add the HPET to the package too.
     */
    if (lcpu->primary) {
	pkg->Hpet = core->Hpet;
	pkg->HpetCfg = core->HpetCfg;
	pkg->HpetCmp = core->HpetCmp;
	pkg->flags |= X86PKG_FL_HAS_HPET;
    }

    ml_set_interrupts_enabled(enabled);

    return(0);
}

/*
 * Map the RCBA area.
 */
static void
map_rcbaArea(void)
{
	/*
	 * Get RCBA area physical address and map it
	 */
	outl(cfgAdr, lpcCfg | (0xF0 & 0xFC));
	rcbaAreap = inl(cfgDat | (0xF0 & 0x03));
	rcbaArea = io_map_spec(rcbaAreap & -4096, PAGE_SIZE * 4, VM_WIMG_IO);
	kprintf("RCBA: vaddr = %08X, paddr = %08X\n", rcbaArea, rcbaAreap);
}

/*
 * Initialize the HPET
 */
void
hpet_init(void)
{
	unsigned int	*xmod;

	map_rcbaArea();

	/*
	 * Is the HPET memory already enabled?
	 * If not, set address and enable.
	 */
	xmod = (uint32_t *)(rcbaArea + 0x3404);	/* Point to the HPTC */
	uint32_t hptc = *xmod;			/* Get HPET config */
	DBG("    current RCBA.HPTC:  %08X\n", *xmod);
	if(!(hptc & hptcAE)) {
		DBG("HPET memory is not enabled, "
		    "enabling and assigning to 0xFED00000 (hope that's ok)\n");
		*xmod = (hptc & ~3) | hptcAE;
	}

	/*
	 * Get physical address of HPET and map it.
	 */
	hpetAreap = hpetAddr | ((hptc & 3) << 12);
	hpetArea = io_map_spec(hpetAreap & -4096, PAGE_SIZE * 4, VM_WIMG_IO);
	kprintf("HPET: vaddr = %08X, paddr = %08X\n", hpetArea, hpetAreap);

	/*
	 * Extract the HPET tick rate.
	 * The period of the HPET is reported in femtoseconds (10**-15s)
	 * and convert to frequency in hertz.
	 */
	hpetFemto = (uint32_t)(((hpetReg_t *)hpetArea)->GCAP_ID >> 32);
	hpetFreq = (1 * Peta) / hpetFemto;

	/*
	 * The conversion factor is the number of nanoseconds per HPET tick
	 * with about 32 bits of fraction.  The value is converted to a
	 * base-2 fixed point number.  To convert from HPET to nanoseconds,
	 * multiply the value by the conversion factor using 96-bit arithmetic,
	 * then shift right 32 bits.  If the value is known to be small,
	 * 64-bit arithmetic will work.
	 */

	/*
	 * Begin conversion of base 10 femtoseconds to base 2, calculate:
	 *  - HPET ticks to nanoseconds conversion in base 2 fraction (* 2**32)
	 *  - nanoseconds to HPET ticks conversion
	 */
	hpetCvtt2n = (uint64_t)hpetFemto << 32;
	hpetCvtt2n = hpetCvtt2n / 1000000ULL;
	hpetCvtn2t = 0xFFFFFFFFFFFFFFFFULL / hpetCvtt2n;
	kprintf("HPET: Frequency = %6d.%04dMHz, "
		"cvtt2n = %08X.%08X, cvtn2t = %08X.%08X\n",
		(uint32_t)(hpetFreq / Mega), (uint32_t)(hpetFreq % Mega), 
		(uint32_t)(hpetCvtt2n >> 32), (uint32_t)hpetCvtt2n,
		(uint32_t)(hpetCvtn2t >> 32), (uint32_t)hpetCvtn2t);


	/* (TAKE OUT LATER)
	 * Begin conversion of base 10 femtoseconds to base 2
	 * HPET ticks to nanoseconds in base 2 fraction (times 1048576)
	 */
	hpetCvt = (uint64_t)hpetFemto << 20;
	hpetCvt = hpetCvt / 1000000ULL;

	/* Calculate conversion from TSC to HPET */
	tsc2hpet = tmrCvt(tscFCvtt2n, hpetCvtn2t);
	DBG(" CVT: TSC to HPET = %08X.%08X\n",
	    (uint32_t)(tsc2hpet >> 32), (uint32_t)tsc2hpet);

	/* Calculate conversion from HPET to TSC */
	hpet2tsc = tmrCvt(hpetCvtt2n, tscFCvtn2t);
	DBG(" CVT: HPET to TSC = %08X.%08X\n",
	    (uint32_t)(hpet2tsc >> 32), (uint32_t)hpet2tsc);

	/* Calculate conversion from BUS to HPET */
	bus2hpet = tmrCvt(busFCvtt2n, hpetCvtn2t);
	DBG(" CVT: BUS to HPET = %08X.%08X\n",
	    (uint32_t)(bus2hpet >> 32), (uint32_t)bus2hpet);

	/* Calculate conversion from HPET to BUS */
	hpet2bus = tmrCvt(hpetCvtt2n, busFCvtn2t);
	DBG(" CVT: HPET to BUS = %08X.%08X\n",
	    (uint32_t)(hpet2bus >> 32), (uint32_t)hpet2bus);

	/* Make sure the counter is off in the HPET configuration flags */
	uint64_t hpetcon = ((hpetReg_t *)hpetArea)->GEN_CONF;
	hpetcon = hpetcon & ~1;
	((hpetReg_t *)hpetArea)->GEN_CONF = hpetcon;

	/*
	 * Convert current TSC to HPET value,
	 * set it, and start it ticking.
	 */
	uint64_t currtsc = rdtsc64();
	uint64_t tscInHPET = tmrCvt(currtsc, tsc2hpet);
	((hpetReg_t *)hpetArea)->MAIN_CNT = tscInHPET;
	hpetcon = hpetcon | 1;
	((hpetReg_t *)hpetArea)->GEN_CONF = hpetcon;
	kprintf("HPET started: TSC = %08X.%08X, HPET = %08X.%08X\n", 
		(uint32_t)(currtsc >> 32), (uint32_t)currtsc,
		(uint32_t)(tscInHPET >> 32), (uint32_t)tscInHPET);

#if MACH_KDB
	db_display_hpet((hpetReg_t *)hpetArea);	/* (BRINGUP) */
#endif
}

/*
 * This routine is used to get various information about the HPET
 * without having to export gobs of globals.  It fills in a data
 * structure with the info.
 */
void
hpet_get_info(hpetInfo_t *info)
{
    info->hpetCvtt2n = hpetCvtt2n;
    info->hpetCvtn2t = hpetCvtn2t;
    info->tsc2hpet   = tsc2hpet;
    info->hpet2tsc   = hpet2tsc;
    info->bus2hpet   = bus2hpet;
    info->hpet2bus   = hpet2bus;
    info->rcbaArea   = rcbaArea;
    info->rcbaAreap  = rcbaAreap;
}


/*
 * This routine is called by the HPET driver
 * when it assigns an HPET timer to a processor.
 *
 * XXX with the new callback into the HPET driver,
 * this routine will be deprecated.
 */
void
ml_hpet_cfg(uint32_t cpu, uint32_t hpetVect)
{
	uint64_t	*hpetVaddr;
	hpetTimer_t	*hpet;
	x86_lcpu_t	*lcpu;
	x86_core_t	*core;
	x86_pkg_t	*pkg;
	boolean_t	enabled;
	
	if(cpu > 1) {
		panic("ml_hpet_cfg: invalid cpu = %d\n", cpu);
	}

	lcpu = cpu_to_lcpu(cpu);
	core = lcpu->core;
	pkg  = core->package;

	/*
	 * Only deal with the primary CPU for the package.
	 */
	if (!lcpu->primary)
	    return;

	enabled = ml_set_interrupts_enabled(FALSE);

	/* Calculate address of the HPET for this processor */
	hpetVaddr = (uint64_t *)(((uint32_t)&(((hpetReg_t *)hpetArea)->TIM1_CONF)) + (cpu << 5));
	hpet = (hpetTimer_t *)hpetVaddr;

	DBG("ml_hpet_cfg: HPET for cpu %d at %p, vector = %d\n",
	     cpu, hpetVaddr, hpetVect);

	/* Save the address and vector of the HPET for this processor */
	core->Hpet = hpet;
	core->HpetVec = hpetVect;

	/*
	 * Enable interrupts
	 */
	core->Hpet->Config |= Tn_INT_ENB_CNF;

	/* Save the configuration */
	core->HpetCfg = core->Hpet->Config;
	core->HpetCmp = 0;

	/*
	 * We're only doing this for the primary CPU, so go
	 * ahead and add the HPET to the package too.
	 */
	pkg->Hpet = core->Hpet;
	pkg->HpetVec = core->HpetVec;
	pkg->HpetCfg = core->HpetCfg;
	pkg->HpetCmp = core->HpetCmp;
	pkg->flags |= X86PKG_FL_HAS_HPET;

	ml_set_interrupts_enabled(enabled);
}

/*
 * This is the HPET interrupt handler.
 *
 * It just hands off to the power management code so that the
 * appropriate things get done there.
 */
int
HPETInterrupt(void)
{

	/* All we do here is to bump the count */
	x86_package()->HpetInt++;

	/*
	 * Let power management do it's thing.
	 */
	pmHPETInterrupt();

	/* Return and show that the 'rupt has been handled... */
	return 1;
}


static hpetReg_t saved_hpet;

void
hpet_save(void)
{
	hpetReg_t	*from = (hpetReg_t *) hpetArea;
	hpetReg_t	*to = &saved_hpet;

	to->GEN_CONF  = from->GEN_CONF;
	to->TIM0_CONF = from->TIM0_CONF;
	to->TIM0_COMP = from->TIM0_COMP;
	to->TIM1_CONF = from->TIM1_CONF;
	to->TIM1_COMP = from->TIM1_COMP;
	to->TIM2_CONF = from->TIM2_CONF;
	to->TIM2_COMP = from->TIM2_COMP;
	to->MAIN_CNT  = from->MAIN_CNT;
}

void
hpet_restore(void)
{
	hpetReg_t	*from = &saved_hpet;
	hpetReg_t	*to = (hpetReg_t *) hpetArea;

	/*
	 * Is the HPET memory already enabled?
	 * If not, set address and enable.
	 */
	uint32_t *hptcp = (uint32_t *)(rcbaArea + 0x3404);
	uint32_t hptc = *hptcp;
	if(!(hptc & hptcAE)) {
		DBG("HPET memory is not enabled, "
		    "enabling and assigning to 0xFED00000 (hope that's ok)\n");
		*hptcp = (hptc & ~3) | hptcAE;
	}

	to->GEN_CONF  = from->GEN_CONF & ~1;

	to->TIM0_CONF = from->TIM0_CONF;
	to->TIM0_COMP = from->TIM0_COMP;
	to->TIM1_CONF = from->TIM1_CONF;
	to->TIM1_COMP = from->TIM1_COMP;
	to->TIM2_CONF = from->TIM2_CONF;
	to->TIM2_COMP = from->TIM2_COMP;
	to->GINTR_STA = -1ULL;
	to->MAIN_CNT  = from->MAIN_CNT;

	to->GEN_CONF = from->GEN_CONF;
}

/*
 *      Read the HPET timer
 *
 */
uint64_t
rdHPET(void)
{
	hpetReg_t		*hpetp = (hpetReg_t *) hpetArea;
	volatile uint32_t	*regp = (uint32_t *) &hpetp->MAIN_CNT;
	uint32_t		high;
	uint32_t		low;

	do {
		high = *(regp + 1);
		low = *regp;
	} while (high != *(regp + 1));

	return (((uint64_t) high) << 32) | low;
}

#if MACH_KDB

#define HI32(x)	((uint32_t)(((x) >> 32) & 0xFFFFFFFF))
#define LO32(x)	((uint32_t)((x) & 0xFFFFFFFF))

/*
 *	Displays HPET memory mapped area
 *	hp
 */
void 
db_hpet(__unused db_expr_t addr, __unused int have_addr, __unused db_expr_t count, __unused char *modif)
{

	db_display_hpet((hpetReg_t *) hpetArea);	/* Dump out the HPET
							 * stuff */
	return;
}

void
db_display_hpet(hpetReg_t *hpt)
{
	uint64_t        cmain;

	cmain = hpt->MAIN_CNT;	/* Get the main timer */

	/* General capabilities */
	db_printf("  GCAP_ID = %08X.%08X\n",
		  HI32(hpt->GCAP_ID), LO32(hpt->GCAP_ID));
	/* General configuration */
	db_printf(" GEN_CONF = %08X.%08X\n",
		  HI32(hpt->GEN_CONF), LO32(hpt->GEN_CONF));
	/* General Interrupt status */
	db_printf("GINTR_STA = %08X.%08X\n",
		  HI32(hpt->GINTR_STA), LO32(hpt->GINTR_STA));
	/* Main counter */
	db_printf(" MAIN_CNT = %08X.%08X\n",
		  HI32(cmain), LO32(cmain));
	/* Timer 0 config and cap */
	db_printf("TIM0_CONF = %08X.%08X\n",
		  HI32(hpt->TIM0_CONF), LO32(hpt->TIM0_CONF));
	/* Timer 0 comparator */
	db_printf("TIM0_COMP = %08X.%08X\n",
		  HI32(hpt->TIM0_COMP), LO32(hpt->TIM0_COMP));
	/* Timer 1 config and cap */
	db_printf("TIM0_CONF = %08X.%08X\n",
		  HI32(hpt->TIM1_CONF), LO32(hpt->TIM1_CONF));
	/* Timer 1 comparator */
	db_printf("TIM1_COMP = %08X.%08X\n",
		  HI32(hpt->TIM1_COMP), LO32(hpt->TIM1_COMP));
	/* Timer 2 config and cap */
	db_printf("TIM2_CONF = %08X.%08X\n",
		  HI32(hpt->TIM2_CONF), LO32(hpt->TIM2_CONF));
	/* Timer 2 comparator */
	db_printf("TIM2_COMP = %08X.%08X\n",
		  HI32(hpt->TIM2_COMP), LO32(hpt->TIM2_COMP));

	db_printf("\nHPET Frequency = %d.%05dMHz\n",
	  (uint32_t) (hpetFreq / 1000000), (uint32_t) (hpetFreq % 1000000));
}
#endif

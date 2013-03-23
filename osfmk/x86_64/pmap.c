/*
 * Copyright (c) 2000-2010 Apple Inc. All rights reserved.
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
 *	File:	pmap.c
 *	Author:	Avadis Tevanian, Jr., Michael Wayne Young
 *	(These guys wrote the Vax version)
 *
 *	Physical Map management code for Intel i386, i486, and i860.
 *
 *	Manages physical address maps.
 *
 *	In addition to hardware address maps, this
 *	module is called upon to provide software-use-only
 *	maps which may or may not be stored in the same
 *	form as hardware maps.  These pseudo-maps are
 *	used to store intermediate results from copy
 *	operations to and from address spaces.
 *
 *	Since the information managed by this module is
 *	also stored by the logical address mapping module,
 *	this module may throw away valid virtual-to-physical
 *	mappings at almost any time.  However, invalidations
 *	of virtual-to-physical mappings must be done as
 *	requested.
 *
 *	In order to cope with hardware architectures which
 *	make virtual-to-physical map invalidates expensive,
 *	this module may delay invalidate or reduced protection
 *	operations until such time as they are actually
 *	necessary.  This module is given full information as
 *	to which processors are currently using which maps,
 *	and to when physical maps must be made correct.
 */

#include <string.h>
#include <mach_kdb.h>
#include <mach_ldebug.h>

#include <libkern/OSAtomic.h>

#include <mach/machine/vm_types.h>

#include <mach/boolean.h>
#include <kern/thread.h>
#include <kern/zalloc.h>
#include <kern/queue.h>
#include <kern/mach_param.h>

#include <kern/lock.h>
#include <kern/kalloc.h>
#include <kern/spl.h>

#include <vm/pmap.h>
#include <vm/vm_map.h>
#include <vm/vm_kern.h>
#include <mach/vm_param.h>
#include <mach/vm_prot.h>
#include <vm/vm_object.h>
#include <vm/vm_page.h>

#include <mach/machine/vm_param.h>
#include <machine/thread.h>

#include <kern/misc_protos.h>			/* prototyping */
#include <i386/misc_protos.h>
#include <i386/i386_lowmem.h>
#include <x86_64/lowglobals.h>

#include <i386/cpuid.h>
#include <i386/cpu_data.h>
#include <i386/cpu_number.h>
#include <i386/machine_cpu.h>
#include <i386/seg.h>
#include <i386/serial_io.h>
#include <i386/cpu_capabilities.h>
#include <i386/machine_routines.h>
#include <i386/proc_reg.h>
#include <i386/tsc.h>
#include <i386/pmap_internal.h>
#include <i386/pmap_pcid.h>

#if	MACH_KDB
#include <ddb/db_command.h>
#include <ddb/db_output.h>
#include <ddb/db_sym.h>
#include <ddb/db_print.h>
#endif	/* MACH_KDB */

#include <vm/vm_protos.h>

#include <i386/mp.h>
#include <i386/mp_desc.h>


#ifdef IWANTTODEBUG
#undef	DEBUG
#define DEBUG 1
#define POSTCODE_DELAY 1
#include <i386/postcode.h>
#endif /* IWANTTODEBUG */

#ifdef	PMAP_DEBUG
#define DBG(x...)	kprintf("DBG: " x)
#else
#define DBG(x...)
#endif
/* Compile time assert to ensure adjacency/alignment of per-CPU data fields used
 * in the trampolines for kernel/user boundary TLB coherency.
 */
char pmap_cpu_data_assert[(((offsetof(cpu_data_t, cpu_tlb_invalid) - offsetof(cpu_data_t, cpu_active_cr3)) == 8) && (offsetof(cpu_data_t, cpu_active_cr3) % 64 == 0)) ? 1 : -1];
boolean_t pmap_trace = FALSE;

boolean_t	no_shared_cr3 = DEBUG;		/* TRUE for DEBUG by default */

int nx_enabled = 1;			/* enable no-execute protection */
int allow_data_exec  = VM_ABI_32;	/* 32-bit apps may execute data by default, 64-bit apps may not */
int allow_stack_exec = 0;		/* No apps may execute from the stack by default */

const boolean_t cpu_64bit  = TRUE; /* Mais oui! */

uint64_t max_preemption_latency_tsc = 0;

pv_hashed_entry_t     *pv_hash_table;  /* hash lists */

uint32_t npvhash = 0;

pv_hashed_entry_t	pv_hashed_free_list = PV_HASHED_ENTRY_NULL;
pv_hashed_entry_t	pv_hashed_kern_free_list = PV_HASHED_ENTRY_NULL;
decl_simple_lock_data(,pv_hashed_free_list_lock)
decl_simple_lock_data(,pv_hashed_kern_free_list_lock)
decl_simple_lock_data(,pv_hash_table_lock)

zone_t		pv_hashed_list_zone;	/* zone of pv_hashed_entry structures */

/*
 *	First and last physical addresses that we maintain any information
 *	for.  Initialized to zero so that pmap operations done before
 *	pmap_init won't touch any non-existent structures.
 */
boolean_t	pmap_initialized = FALSE;/* Has pmap_init completed? */

static struct vm_object kptobj_object_store;
static struct vm_object kpml4obj_object_store;
static struct vm_object kpdptobj_object_store;

/*
 *	Array of physical page attribites for managed pages.
 *	One byte per physical page.
 */
char		*pmap_phys_attributes;
unsigned int	last_managed_page = 0;

/*
 *	Amount of virtual memory mapped by one
 *	page-directory entry.
 */

uint64_t pde_mapped_size = PDE_MAPPED_SIZE;

unsigned pmap_memory_region_count;
unsigned pmap_memory_region_current;

pmap_memory_region_t pmap_memory_regions[PMAP_MEMORY_REGIONS_SIZE];

/*
 *	Other useful macros.
 */
#define current_pmap()		(vm_map_pmap(current_thread()->map))

struct pmap	kernel_pmap_store;
pmap_t		kernel_pmap;

pd_entry_t	high_shared_pde;
pd_entry_t	commpage64_pde;

struct zone	*pmap_zone;		/* zone of pmap structures */

struct zone	*pmap_anchor_zone;
int		pmap_debug = 0;		/* flag for debugging prints */

unsigned int	inuse_ptepages_count = 0;
long long	alloc_ptepages_count __attribute__((aligned(8))) = 0; /* aligned for atomic access */
unsigned int	bootstrap_wired_pages = 0;
int		pt_fake_zone_index = -1;

extern 	long	NMIPI_acks;

boolean_t	kernel_text_ps_4K = TRUE;
boolean_t	wpkernel = TRUE;

extern char	end;

static int	nkpt;

pt_entry_t     *DMAP1, *DMAP2;
caddr_t         DADDR1;
caddr_t         DADDR2;

/*
 * unlinks the pv_hashed_entry_t pvh from the singly linked hash chain.
 * properly deals with the anchor.
 * must be called with the hash locked, does not unlock it
 */


/*
 *	Map memory at initialization.  The physical addresses being
 *	mapped are not managed and are never unmapped.
 *
 *	For now, VM is already on, we only need to map the
 *	specified memory.
 */
vm_offset_t
pmap_map(
	vm_offset_t	virt,
	vm_map_offset_t	start_addr,
	vm_map_offset_t	end_addr,
	vm_prot_t	prot,
	unsigned int	flags)
{
	int		ps;

	ps = PAGE_SIZE;
	while (start_addr < end_addr) {
		pmap_enter(kernel_pmap, (vm_map_offset_t)virt,
			   (ppnum_t) i386_btop(start_addr), prot, flags, FALSE);
		virt += ps;
		start_addr += ps;
	}
	return(virt);
}

extern	char			*first_avail;
extern	vm_offset_t		virtual_avail, virtual_end;
extern	pmap_paddr_t		avail_start, avail_end;
extern  vm_offset_t		sHIB;
extern  vm_offset_t		eHIB;
extern  vm_offset_t		stext;
extern  vm_offset_t		etext;
extern  vm_offset_t		sdata;

extern void			*KPTphys;

boolean_t pmap_smep_enabled = FALSE;

void
pmap_cpu_init(void)
{
	/*
	 * Here early in the life of a processor (from cpu_mode_init()).
	 * Ensure global page feature is disabled at this point.
	 */

	set_cr4(get_cr4() &~ CR4_PGE);

	/*
	 * Initialize the per-cpu, TLB-related fields.
	 */
	current_cpu_datap()->cpu_kernel_cr3 = kernel_pmap->pm_cr3;
	current_cpu_datap()->cpu_active_cr3 = kernel_pmap->pm_cr3;
	current_cpu_datap()->cpu_tlb_invalid = FALSE;
	current_cpu_datap()->cpu_task_map = TASK_MAP_64BIT;
	pmap_pcid_configure();
	if (cpuid_leaf7_features() & CPUID_LEAF7_FEATURE_SMEP) {
		boolean_t nsmep;
		if (!PE_parse_boot_argn("-pmap_smep_disable", &nsmep, sizeof(nsmep))) {
			set_cr4(get_cr4() | CR4_SMEP);
			pmap_smep_enabled = TRUE;
		}
	}
}



/*
 *	Bootstrap the system enough to run with virtual memory.
 *	Map the kernel's code and data, and allocate the system page table.
 *	Called with mapping OFF.  Page_size must already be set.
 */

void
pmap_bootstrap(
	__unused vm_offset_t	load_start,
	__unused boolean_t	IA32e)
{
#if NCOPY_WINDOWS > 0
	vm_offset_t	va;
	int i;
#endif
	assert(IA32e);

	vm_last_addr = VM_MAX_KERNEL_ADDRESS;	/* Set the highest address
						 * known to VM */
	/*
	 *	The kernel's pmap is statically allocated so we don't
	 *	have to use pmap_create, which is unlikely to work
	 *	correctly at this part of the boot sequence.
	 */

	kernel_pmap = &kernel_pmap_store;
	kernel_pmap->ref_count = 1;
	kernel_pmap->nx_enabled = FALSE;
	kernel_pmap->pm_task_map = TASK_MAP_64BIT;
	kernel_pmap->pm_obj = (vm_object_t) NULL;
	kernel_pmap->dirbase = (pd_entry_t *)((uintptr_t)IdlePTD);
	kernel_pmap->pm_pdpt = (pd_entry_t *) ((uintptr_t)IdlePDPT);
	kernel_pmap->pm_pml4 = IdlePML4;
	kernel_pmap->pm_cr3 = (uintptr_t)ID_MAP_VTOP(IdlePML4);
	pmap_pcid_initialize_kernel(kernel_pmap);

	

	current_cpu_datap()->cpu_kernel_cr3 = (addr64_t) kernel_pmap->pm_cr3;

	nkpt = NKPT;
	OSAddAtomic(NKPT,  &inuse_ptepages_count);
	OSAddAtomic64(NKPT,  &alloc_ptepages_count);
	bootstrap_wired_pages = NKPT;

	virtual_avail = (vm_offset_t)(VM_MIN_KERNEL_ADDRESS) + (vm_offset_t)first_avail;
	virtual_end = (vm_offset_t)(VM_MAX_KERNEL_ADDRESS);

#if NCOPY_WINDOWS > 0
	/*
	 * Reserve some special page table entries/VA space for temporary
	 * mapping of pages.
	 */
#define	SYSMAP(c, p, v, n)	\
	v = (c)va; va += ((n)*INTEL_PGBYTES);

	va = virtual_avail;

        for (i=0; i<PMAP_NWINDOWS; i++) {
#if 1
	    kprintf("trying to do SYSMAP idx %d %p\n", i,
	 	current_cpu_datap());
	    kprintf("cpu_pmap %p\n", current_cpu_datap()->cpu_pmap);
	    kprintf("mapwindow %p\n", current_cpu_datap()->cpu_pmap->mapwindow);
	    kprintf("two stuff %p %p\n",
		   (void *)(current_cpu_datap()->cpu_pmap->mapwindow[i].prv_CMAP),
                   (void *)(current_cpu_datap()->cpu_pmap->mapwindow[i].prv_CADDR));
#endif
            SYSMAP(caddr_t,
		   (current_cpu_datap()->cpu_pmap->mapwindow[i].prv_CMAP),
                   (current_cpu_datap()->cpu_pmap->mapwindow[i].prv_CADDR),
		   1);
	    current_cpu_datap()->cpu_pmap->mapwindow[i].prv_CMAP =
	        &(current_cpu_datap()->cpu_pmap->mapwindow[i].prv_CMAP_store);
            *current_cpu_datap()->cpu_pmap->mapwindow[i].prv_CMAP = 0;
        }

	/* DMAP user for debugger */
	SYSMAP(caddr_t, DMAP1, DADDR1, 1);
	SYSMAP(caddr_t, DMAP2, DADDR2, 1);  /* XXX temporary - can remove */

	virtual_avail = va;
#endif

	if (PE_parse_boot_argn("npvhash", &npvhash, sizeof (npvhash))) {
		if (0 != ((npvhash + 1) & npvhash)) {
			kprintf("invalid hash %d, must be ((2^N)-1), "
				"using default %d\n", npvhash, NPVHASH);
			npvhash = NPVHASH;
		}
	} else {
		npvhash = NPVHASH;
	}

	simple_lock_init(&kernel_pmap->lock, 0);
	simple_lock_init(&pv_hashed_free_list_lock, 0);
	simple_lock_init(&pv_hashed_kern_free_list_lock, 0);
	simple_lock_init(&pv_hash_table_lock,0);

	pmap_cpu_init();

	if (pmap_pcid_ncpus)
		printf("PMAP: PCID enabled\n");

	if (pmap_smep_enabled)
		printf("PMAP: Supervisor Mode Execute Protection enabled\n");

	boot_args *args = (boot_args *)PE_state.bootArgs;
	if (args->efiMode == kBootArgsEfiMode32) {
		printf("EFI32: kernel virtual space limited to 4GB\n");
		virtual_end = VM_MAX_KERNEL_ADDRESS_EFI32;
	}
	kprintf("Kernel virtual space from 0x%lx to 0x%lx.\n",
			(long)KERNEL_BASE, (long)virtual_end);
	kprintf("Available physical space from 0x%llx to 0x%llx\n",
			avail_start, avail_end);

	/*
	 * The -no_shared_cr3 boot-arg is a debugging feature (set by default
	 * in the DEBUG kernel) to force the kernel to switch to its own map
	 * (and cr3) when control is in kernelspace. The kernel's map does not
	 * include (i.e. share) userspace so wild references will cause
	 * a panic. Only copyin and copyout are exempt from this. 
	 */
	(void) PE_parse_boot_argn("-no_shared_cr3",
				  &no_shared_cr3, sizeof (no_shared_cr3));
	if (no_shared_cr3)
		kprintf("Kernel not sharing user map\n");
		
#ifdef	PMAP_TRACES
	if (PE_parse_boot_argn("-pmap_trace", &pmap_trace, sizeof (pmap_trace))) {
		kprintf("Kernel traces for pmap operations enabled\n");
	}	
#endif	/* PMAP_TRACES */
}

void
pmap_virtual_space(
	vm_offset_t *startp,
	vm_offset_t *endp)
{
	*startp = virtual_avail;
	*endp = virtual_end;
}

/*
 *	Initialize the pmap module.
 *	Called by vm_init, to initialize any structures that the pmap
 *	system needs to map virtual memory.
 */
void
pmap_init(void)
{
	long			npages;
	vm_offset_t		addr;
	vm_size_t		s, vsize;
	vm_map_offset_t		vaddr;
	ppnum_t ppn;


	kernel_pmap->pm_obj_pml4 = &kpml4obj_object_store;
	_vm_object_allocate((vm_object_size_t)NPML4PGS, &kpml4obj_object_store);

	kernel_pmap->pm_obj_pdpt = &kpdptobj_object_store;
	_vm_object_allocate((vm_object_size_t)NPDPTPGS, &kpdptobj_object_store);

	kernel_pmap->pm_obj = &kptobj_object_store;
	_vm_object_allocate((vm_object_size_t)NPDEPGS, &kptobj_object_store);

	/*
	 *	Allocate memory for the pv_head_table and its lock bits,
	 *	the modify bit array, and the pte_page table.
	 */

	/*
	 * zero bias all these arrays now instead of off avail_start
	 * so we cover all memory
	 */

	npages = i386_btop(avail_end);
	s = (vm_size_t) (sizeof(struct pv_rooted_entry) * npages
			 + (sizeof (struct pv_hashed_entry_t *) * (npvhash+1))
			 + pv_lock_table_size(npages)
			 + pv_hash_lock_table_size((npvhash+1))
				+ npages);

	s = round_page(s);
	if (kernel_memory_allocate(kernel_map, &addr, s, 0,
				   KMA_KOBJECT | KMA_PERMANENT)
	    != KERN_SUCCESS)
		panic("pmap_init");

	memset((char *)addr, 0, s);

	vaddr = addr;
	vsize = s;

#if PV_DEBUG
	if (0 == npvhash) panic("npvhash not initialized");
#endif

	/*
	 *	Allocate the structures first to preserve word-alignment.
	 */
	pv_head_table = (pv_rooted_entry_t) addr;
	addr = (vm_offset_t) (pv_head_table + npages);

	pv_hash_table = (pv_hashed_entry_t *)addr;
	addr = (vm_offset_t) (pv_hash_table + (npvhash + 1));

	pv_lock_table = (char *) addr;
	addr = (vm_offset_t) (pv_lock_table + pv_lock_table_size(npages));

	pv_hash_lock_table = (char *) addr;
	addr = (vm_offset_t) (pv_hash_lock_table + pv_hash_lock_table_size((npvhash+1)));

	pmap_phys_attributes = (char *) addr;

	ppnum_t  last_pn = i386_btop(avail_end);
        unsigned int i;
	pmap_memory_region_t *pmptr = pmap_memory_regions;
	for (i = 0; i < pmap_memory_region_count; i++, pmptr++) {
		if (pmptr->type != kEfiConventionalMemory)
			continue;
		unsigned int pn;
		for (pn = pmptr->base; pn <= pmptr->end; pn++) {
			if (pn < last_pn) {
				pmap_phys_attributes[pn] |= PHYS_MANAGED;

				if (pn > last_managed_page)
					last_managed_page = pn;

				if (pn >= lowest_hi && pn <= highest_hi)
					pmap_phys_attributes[pn] |= PHYS_NOENCRYPT;
			}
		}
	}
	while (vsize) {
		ppn = pmap_find_phys(kernel_pmap, vaddr);

		pmap_phys_attributes[ppn] |= PHYS_NOENCRYPT;

		vaddr += PAGE_SIZE;
		vsize -= PAGE_SIZE;
	}
	/*
	 *	Create the zone of physical maps,
	 *	and of the physical-to-virtual entries.
	 */
	s = (vm_size_t) sizeof(struct pmap);
	pmap_zone = zinit(s, 400*s, 4096, "pmap"); /* XXX */
        zone_change(pmap_zone, Z_NOENCRYPT, TRUE);

	pmap_anchor_zone = zinit(PAGE_SIZE, task_max, PAGE_SIZE, "pagetable anchors");
	zone_change(pmap_anchor_zone, Z_NOENCRYPT, TRUE);

#if	ZONE_DEBUG
	/* The anchor is required to be page aligned. Zone debugging adds
	 * padding which may violate that requirement. Disable it
	 * to avoid assumptions.
	 */
	zone_debug_disable(pmap_anchor_zone);
#endif	

	s = (vm_size_t) sizeof(struct pv_hashed_entry);
	pv_hashed_list_zone = zinit(s, 10000*s /* Expandable zone */,
	    4096 * 3 /* LCM x86_64*/, "pv_list");
	zone_change(pv_hashed_list_zone, Z_NOENCRYPT, TRUE);

	/* create pv entries for kernel pages mapped by low level
	   startup code.  these have to exist so we can pmap_remove()
	   e.g. kext pages from the middle of our addr space */

	vaddr = (vm_map_offset_t) VM_MIN_KERNEL_ADDRESS;
	for (ppn = VM_MIN_KERNEL_PAGE; ppn < i386_btop(avail_start); ppn++) {
		pv_rooted_entry_t pv_e;

		pv_e = pai_to_pvh(ppn);
		pv_e->va = vaddr;
		vaddr += PAGE_SIZE;
		pv_e->pmap = kernel_pmap;
		queue_init(&pv_e->qlink);
	}
	pmap_initialized = TRUE;

	max_preemption_latency_tsc = tmrCvt((uint64_t)MAX_PREEMPTION_LATENCY_NS, tscFCvtn2t);

	/*
	 * Ensure the kernel's PML4 entry exists for the basement
	 * before this is shared with any user.
	 */
	pmap_expand_pml4(kernel_pmap, KERNEL_BASEMENT);
}

/*
 * Called once VM is fully initialized so that we can release unused
 * sections of low memory to the general pool.
 * Also complete the set-up of identity-mapped sections of the kernel:
 *  1) write-protect kernel text
 *  2) map kernel text using large pages if possible
 *  3) read and write-protect page zero (for K32)
 *  4) map the global page at the appropriate virtual address.
 *
 * Use of large pages
 * ------------------
 * To effectively map and write-protect all kernel text pages, the text
 * must be 2M-aligned at the base, and the data section above must also be
 * 2M-aligned. That is, there's padding below and above. This is achieved
 * through linker directives. Large pages are used only if this alignment
 * exists (and not overriden by the -kernel_text_page_4K boot-arg). The
 * memory layout is:
 * 
 *                       :                :
 *                       |     __DATA     |
 *               sdata:  ==================  2Meg
 *                       |                |
 *                       |  zero-padding  |
 *                       |                |
 *               etext:  ------------------ 
 *                       |                |
 *                       :                :
 *                       |                |
 *                       |     __TEXT     |
 *                       |                |
 *                       :                :
 *                       |                |
 *               stext:  ==================  2Meg
 *                       |                |
 *                       |  zero-padding  |
 *                       |                |
 *               eHIB:   ------------------ 
 *                       |     __HIB      |
 *                       :                :
 *
 * Prior to changing the mapping from 4K to 2M, the zero-padding pages
 * [eHIB,stext] and [etext,sdata] are ml_static_mfree()'d. Then all the
 * 4K pages covering [stext,etext] are coalesced as 2M large pages.
 * The now unused level-1 PTE pages are also freed.
 */
extern uint32_t pmap_reserved_ranges;
void
pmap_lowmem_finalize(void)
{
	spl_t           spl;
	int		i;

	/* Check the kernel is linked at the expected base address */
	if (i386_btop(kvtophys((vm_offset_t) &IdlePML4)) !=
	    I386_KERNEL_IMAGE_BASE_PAGE)
		panic("pmap_lowmem_finalize() unexpected kernel base address");

	/*
	 * Update wired memory statistics for early boot pages
	 */
	PMAP_ZINFO_PALLOC(bootstrap_wired_pages * PAGE_SIZE);

	/*
	 * Free all pages in pmap regions below the base:
	 * rdar://6332712
	 *	We can't free all the pages to VM that EFI reports available.
	 *	Pages in the range 0xc0000-0xff000 aren't safe over sleep/wake.
	 *	There's also a size miscalculation here: pend is one page less
	 *	than it should be but this is not fixed to be backwards
	 *	compatible.
	 *	Due to this current EFI limitation, we take only the first
	 *	entry in the memory region table. However, the loop is retained
	 * 	(with the intended termination criteria commented out) in the
	 *	hope that some day we can free all low-memory ranges.
	 */
	for (i = 0;
//	     pmap_memory_regions[i].end <= I386_KERNEL_IMAGE_BASE_PAGE;
	     i < 1  && (pmap_reserved_ranges == 0);
	     i++) {
		vm_offset_t	pbase = (vm_offset_t)i386_ptob(pmap_memory_regions[i].base);
		vm_offset_t	pend  = (vm_offset_t)i386_ptob(pmap_memory_regions[i].end);
//		vm_offset_t	pend  = i386_ptob(pmap_memory_regions[i].end+1);

		DBG("ml_static_mfree(%p,%p) for pmap region %d\n",
		    (void *) ml_static_ptovirt(pbase),
		    (void *) (pend - pbase), i);
		ml_static_mfree(ml_static_ptovirt(pbase), pend - pbase);
	}

	/*
	 * If text and data are both 2MB-aligned,
	 * we can map text with large-pages,
	 * unless the -kernel_text_ps_4K boot-arg overrides.
	 */
	if ((stext & I386_LPGMASK) == 0 && (sdata & I386_LPGMASK) == 0) {
		kprintf("Kernel text is 2MB aligned");
		kernel_text_ps_4K = FALSE;
		if (PE_parse_boot_argn("-kernel_text_ps_4K",
				       &kernel_text_ps_4K,
				       sizeof (kernel_text_ps_4K)))
			kprintf(" but will be mapped with 4K pages\n");
		else
			kprintf(" and will be mapped with 2M pages\n");
	}

	(void) PE_parse_boot_argn("wpkernel", &wpkernel, sizeof (wpkernel));
	if (wpkernel)
		kprintf("Kernel text %p-%p to be write-protected\n",
			(void *) stext, (void *) etext);

	spl = splhigh();

	/*
	 * Scan over text if mappings are to be changed:
	 * - Remap kernel text readonly unless the "wpkernel" boot-arg is 0 
 	 * - Change to large-pages if possible and not overriden.
	 */
	if (kernel_text_ps_4K && wpkernel) {
		vm_offset_t     myva;
		for (myva = stext; myva < etext; myva += PAGE_SIZE) {
			pt_entry_t     *ptep;

			ptep = pmap_pte(kernel_pmap, (vm_map_offset_t)myva);
			if (ptep)
				pmap_store_pte(ptep, *ptep & ~INTEL_PTE_RW);
		}
	}

	if (!kernel_text_ps_4K) {
		vm_offset_t     myva;

		/*
		 * Release zero-filled page padding used for 2M-alignment.
		 */
		DBG("ml_static_mfree(%p,%p) for padding below text\n",
			(void *) eHIB, (void *) (stext - eHIB));
		ml_static_mfree(eHIB, stext - eHIB);
		DBG("ml_static_mfree(%p,%p) for padding above text\n",
			(void *) etext, (void *) (sdata - etext));
		ml_static_mfree(etext, sdata - etext);

		/*
		 * Coalesce text pages into large pages.
		 */
		for (myva = stext; myva < sdata; myva += I386_LPGBYTES) {
			pt_entry_t	*ptep;
			vm_offset_t	pte_phys;
			pt_entry_t	*pdep;
			pt_entry_t	pde;

			pdep = pmap_pde(kernel_pmap, (vm_map_offset_t)myva);
			ptep = pmap_pte(kernel_pmap, (vm_map_offset_t)myva);
			DBG("myva: %p pdep: %p ptep: %p\n",
				(void *) myva, (void *) pdep, (void *) ptep);
			if ((*ptep & INTEL_PTE_VALID) == 0)
				continue;
			pte_phys = (vm_offset_t)(*ptep & PG_FRAME);
			pde = *pdep & PTMASK;	/* page attributes from pde */
			pde |= INTEL_PTE_PS;	/* make it a 2M entry */
			pde |= pte_phys;	/* take page frame from pte */

			if (wpkernel)
				pde &= ~INTEL_PTE_RW;
			DBG("pmap_store_pte(%p,0x%llx)\n",
				(void *)pdep, pde);
			pmap_store_pte(pdep, pde);

			/*
			 * Free the now-unused level-1 pte.
			 * Note: ptep is a virtual address to the pte in the
			 *   recursive map. We can't use this address to free
			 *   the page. Instead we need to compute its address
			 *   in the Idle PTEs in "low memory".
			 */
			vm_offset_t vm_ptep = (vm_offset_t) KPTphys
						+ (pte_phys >> PTPGSHIFT);
			DBG("ml_static_mfree(%p,0x%x) for pte\n",
				(void *) vm_ptep, PAGE_SIZE);
			ml_static_mfree(vm_ptep, PAGE_SIZE);
		}

		/* Change variable read by sysctl machdep.pmap */
		pmap_kernel_text_ps = I386_LPGBYTES;
	}

	/* map lowmem global page into fixed addr */
	pt_entry_t *pte = NULL;
	if (0 == (pte = pmap_pte(kernel_pmap,
				 VM_MIN_KERNEL_LOADED_ADDRESS + 0x2000)))
		panic("lowmem pte");
	/* make sure it is defined on page boundary */
	assert(0 == ((vm_offset_t) &lowGlo & PAGE_MASK));
	pmap_store_pte(pte, kvtophys((vm_offset_t)&lowGlo)
				| INTEL_PTE_REF
				| INTEL_PTE_MOD
				| INTEL_PTE_WIRED
				| INTEL_PTE_VALID
				| INTEL_PTE_RW);
	splx(spl);
	if (pmap_pcid_ncpus)
		tlb_flush_global();
	else
		flush_tlb_raw();
}

/*
 * this function is only used for debugging fron the vm layer
 */
boolean_t
pmap_verify_free(
		 ppnum_t pn)
{
	pv_rooted_entry_t	pv_h;
	int		pai;
	boolean_t	result;

	assert(pn != vm_page_fictitious_addr);

	if (!pmap_initialized)
		return(TRUE);

	if (pn == vm_page_guard_addr)
		return TRUE;

	pai = ppn_to_pai(pn);
	if (!IS_MANAGED_PAGE(pai))
		return(FALSE);
	pv_h = pai_to_pvh(pn);
	result = (pv_h->pmap == PMAP_NULL);
	return(result);
}

boolean_t
pmap_is_empty(
       pmap_t          pmap,
       vm_map_offset_t va_start,
       vm_map_offset_t va_end)
{
	vm_map_offset_t offset;
	ppnum_t         phys_page;

	if (pmap == PMAP_NULL) {
		return TRUE;
	}

	/*
	 * Check the resident page count
	 * - if it's zero, the pmap is completely empty.
	 * This short-circuit test prevents a virtual address scan which is
	 * painfully slow for 64-bit spaces.
	 * This assumes the count is correct
	 * .. the debug kernel ought to be checking perhaps by page table walk.
	 */
	if (pmap->stats.resident_count == 0)
		return TRUE;

	for (offset = va_start;
	     offset < va_end;
	     offset += PAGE_SIZE_64) {
		phys_page = pmap_find_phys(pmap, offset);
		if (phys_page) {
			kprintf("pmap_is_empty(%p,0x%llx,0x%llx): "
				"page %d at 0x%llx\n",
				pmap, va_start, va_end, phys_page, offset);
			return FALSE;
		}
	}

	return TRUE;
}


/*
 *	Create and return a physical map.
 *
 *	If the size specified for the map
 *	is zero, the map is an actual physical
 *	map, and may be referenced by the
 *	hardware.
 *
 *	If the size specified is non-zero,
 *	the map will be used in software only, and
 *	is bounded by that size.
 */
pmap_t
pmap_create(
	    vm_map_size_t	sz,
	    boolean_t		is_64bit)
{
	pmap_t		p;
	vm_size_t	size;
	pml4_entry_t    *pml4;
	pml4_entry_t    *kpml4;

	PMAP_TRACE(PMAP_CODE(PMAP__CREATE) | DBG_FUNC_START,
		   (uint32_t) (sz>>32), (uint32_t) sz, is_64bit, 0, 0);

	size = (vm_size_t) sz;

	/*
	 *	A software use-only map doesn't even need a map.
	 */

	if (size != 0) {
		return(PMAP_NULL);
	}

	p = (pmap_t) zalloc(pmap_zone);
	if (PMAP_NULL == p)
		panic("pmap_create zalloc");
	/* Zero all fields */
	bzero(p, sizeof(*p));
	/* init counts now since we'll be bumping some */
	simple_lock_init(&p->lock, 0);
	p->stats.resident_count = 0;
	p->stats.resident_max = 0;
	p->stats.wired_count = 0;
	p->ref_count = 1;
	p->nx_enabled = 1;
	p->pm_shared = FALSE;

	p->pm_task_map = is_64bit ? TASK_MAP_64BIT : TASK_MAP_32BIT;;
	if (pmap_pcid_ncpus)
		pmap_pcid_initialize(p);
	p->pm_pml4 = zalloc(pmap_anchor_zone);

	pmap_assert((((uintptr_t)p->pm_pml4) & PAGE_MASK) == 0);

	memset((char *)p->pm_pml4, 0, PAGE_SIZE);

	p->pm_cr3 = (pmap_paddr_t)kvtophys((vm_offset_t)p->pm_pml4);

	/* allocate the vm_objs to hold the pdpt, pde and pte pages */

	p->pm_obj_pml4 = vm_object_allocate((vm_object_size_t)(NPML4PGS));
	if (NULL == p->pm_obj_pml4)
		panic("pmap_create pdpt obj");

	p->pm_obj_pdpt = vm_object_allocate((vm_object_size_t)(NPDPTPGS));
	if (NULL == p->pm_obj_pdpt)
		panic("pmap_create pdpt obj");

	p->pm_obj = vm_object_allocate((vm_object_size_t)(NPDEPGS));
	if (NULL == p->pm_obj)
		panic("pmap_create pte obj");

	/* All pmaps share the kernel's pml4 */
	pml4 = pmap64_pml4(p, 0ULL);
	kpml4 = kernel_pmap->pm_pml4;
	pml4[KERNEL_PML4_INDEX]    = kpml4[KERNEL_PML4_INDEX];
	pml4[KERNEL_KEXTS_INDEX]   = kpml4[KERNEL_KEXTS_INDEX];
	pml4[KERNEL_PHYSMAP_INDEX] = kpml4[KERNEL_PHYSMAP_INDEX];

	PMAP_TRACE(PMAP_CODE(PMAP__CREATE) | DBG_FUNC_START,
		   p, is_64bit, 0, 0, 0);

	return(p);
}

/*
 *	Retire the given physical map from service.
 *	Should only be called if the map contains
 *	no valid mappings.
 */

void
pmap_destroy(pmap_t	p)
{
	int		c;

	if (p == PMAP_NULL)
		return;

	PMAP_TRACE(PMAP_CODE(PMAP__DESTROY) | DBG_FUNC_START,
		   p, 0, 0, 0, 0);

	PMAP_LOCK(p);

	c = --p->ref_count;

	pmap_assert((current_thread() && (current_thread()->map)) ? (current_thread()->map->pmap != p) : TRUE);

	if (c == 0) {
		/* 
		 * If some cpu is not using the physical pmap pointer that it
		 * is supposed to be (see set_dirbase), we might be using the
		 * pmap that is being destroyed! Make sure we are
		 * physically on the right pmap:
		 */
		PMAP_UPDATE_TLBS(p, 0x0ULL, 0xFFFFFFFFFFFFF000ULL);
		if (pmap_pcid_ncpus)
			pmap_destroy_pcid_sync(p);
	}

	PMAP_UNLOCK(p);

	if (c != 0) {
		PMAP_TRACE(PMAP_CODE(PMAP__DESTROY) | DBG_FUNC_END,
			   p, 1, 0, 0, 0);
		pmap_assert(p == kernel_pmap);
	        return;	/* still in use */
	}

	/*
	 *	Free the memory maps, then the
	 *	pmap structure.
	 */
	int inuse_ptepages = 0;

	zfree(pmap_anchor_zone, p->pm_pml4);

	inuse_ptepages += p->pm_obj_pml4->resident_page_count;
	vm_object_deallocate(p->pm_obj_pml4);

	inuse_ptepages += p->pm_obj_pdpt->resident_page_count;
	vm_object_deallocate(p->pm_obj_pdpt);

	inuse_ptepages += p->pm_obj->resident_page_count;
	vm_object_deallocate(p->pm_obj);

	OSAddAtomic(-inuse_ptepages,  &inuse_ptepages_count);
	PMAP_ZINFO_PFREE(inuse_ptepages * PAGE_SIZE);

	zfree(pmap_zone, p);

	PMAP_TRACE(PMAP_CODE(PMAP__DESTROY) | DBG_FUNC_END,
		   0, 0, 0, 0, 0);
}

/*
 *	Add a reference to the specified pmap.
 */

void
pmap_reference(pmap_t	p)
{
	if (p != PMAP_NULL) {
	        PMAP_LOCK(p);
		p->ref_count++;
		PMAP_UNLOCK(p);;
	}
}

/*
 *	Remove phys addr if mapped in specified map
 *
 */
void
pmap_remove_some_phys(
	__unused pmap_t		map,
	__unused ppnum_t         pn)
{

/* Implement to support working set code */

}


/*
 *	Set the physical protection on the
 *	specified range of this map as requested.
 *	Will not increase permissions.
 */
void
pmap_protect(
	pmap_t		map,
	vm_map_offset_t	sva,
	vm_map_offset_t	eva,
	vm_prot_t	prot)
{
	pt_entry_t	*pde;
	pt_entry_t	*spte, *epte;
	vm_map_offset_t lva;
	vm_map_offset_t orig_sva;
	boolean_t       set_NX;
	int             num_found = 0;

	pmap_intr_assert();

	if (map == PMAP_NULL)
		return;

	if (prot == VM_PROT_NONE) {
		pmap_remove(map, sva, eva);
		return;
	}
	PMAP_TRACE(PMAP_CODE(PMAP__PROTECT) | DBG_FUNC_START,
		   map,
		   (uint32_t) (sva >> 32), (uint32_t) sva,
		   (uint32_t) (eva >> 32), (uint32_t) eva);

	if ((prot & VM_PROT_EXECUTE) || !nx_enabled || !map->nx_enabled)
		set_NX = FALSE;
	else
		set_NX = TRUE;

	PMAP_LOCK(map);

	orig_sva = sva;
	while (sva < eva) {
		lva = (sva + pde_mapped_size) & ~(pde_mapped_size - 1);
		if (lva > eva)
			lva = eva;
		pde = pmap_pde(map, sva);
		if (pde && (*pde & INTEL_PTE_VALID)) {
			if (*pde & INTEL_PTE_PS) {
				/* superpage */
				spte = pde;
				epte = spte+1; /* excluded */
			} else {
				spte = pmap_pte(map, (sva & ~(pde_mapped_size - 1)));
				spte = &spte[ptenum(sva)];
				epte = &spte[intel_btop(lva - sva)];
			}

			for (; spte < epte; spte++) {
				if (!(*spte & INTEL_PTE_VALID))
					continue;

				if (prot & VM_PROT_WRITE)
					pmap_update_pte(spte, *spte,
						*spte | INTEL_PTE_WRITE);
				else
					pmap_update_pte(spte, *spte,
						*spte & ~INTEL_PTE_WRITE);

				if (set_NX)
					pmap_update_pte(spte, *spte,
						*spte | INTEL_PTE_NX);
				else
					pmap_update_pte(spte, *spte,
						*spte & ~INTEL_PTE_NX);

				num_found++;
			}
		}
		sva = lva;
	}
	if (num_found)
		PMAP_UPDATE_TLBS(map, orig_sva, eva);

	PMAP_UNLOCK(map);

	PMAP_TRACE(PMAP_CODE(PMAP__PROTECT) | DBG_FUNC_END,
		   0, 0, 0, 0, 0);

}

/* Map a (possibly) autogenned block */
void
pmap_map_block(
	pmap_t		pmap, 
	addr64_t	va,
	ppnum_t 	pa,
	uint32_t	size,
	vm_prot_t	prot,
	int		attr,
	__unused unsigned int	flags)
{
	uint32_t        page;
	int		cur_page_size;

	if (attr & VM_MEM_SUPERPAGE)
		cur_page_size =  SUPERPAGE_SIZE;
	else 
		cur_page_size =  PAGE_SIZE;

	for (page = 0; page < size; page+=cur_page_size/PAGE_SIZE) {
		pmap_enter(pmap, va, pa, prot, attr, TRUE);
		va += cur_page_size;
		pa+=cur_page_size/PAGE_SIZE;
	}
}


void
pmap_expand_pml4(
	pmap_t		map,
	vm_map_offset_t	vaddr)
{
	vm_page_t	m;
	pmap_paddr_t	pa;
	uint64_t	i;
	ppnum_t		pn;
	pml4_entry_t	*pml4p;

	DBG("pmap_expand_pml4(%p,%p)\n", map, (void *)vaddr);

	/*
	 *	Allocate a VM page for the pml4 page
	 */
	while ((m = vm_page_grab()) == VM_PAGE_NULL)
		VM_PAGE_WAIT();

	/*
	 *	put the page into the pmap's obj list so it
	 *	can be found later.
	 */
	pn = m->phys_page;
	pa = i386_ptob(pn);
	i = pml4idx(map, vaddr);

	/*
	 *	Zero the page.
	 */
	pmap_zero_page(pn);

	vm_page_lockspin_queues();
	vm_page_wire(m);
	vm_page_unlock_queues();

	OSAddAtomic(1,  &inuse_ptepages_count);
	OSAddAtomic64(1,  &alloc_ptepages_count);
	PMAP_ZINFO_PALLOC(PAGE_SIZE);

	/* Take the oject lock (mutex) before the PMAP_LOCK (spinlock) */
	vm_object_lock(map->pm_obj_pml4);

	PMAP_LOCK(map);
	/*
	 *	See if someone else expanded us first
	 */
	if (pmap64_pdpt(map, vaddr) != PDPT_ENTRY_NULL) {
	        PMAP_UNLOCK(map);
		vm_object_unlock(map->pm_obj_pml4);

		VM_PAGE_FREE(m);

		OSAddAtomic(-1,  &inuse_ptepages_count);
		PMAP_ZINFO_PFREE(PAGE_SIZE);
		return;
	}

#if 0 /* DEBUG */
       if (0 != vm_page_lookup(map->pm_obj_pml4, (vm_object_offset_t)i)) {
	       panic("pmap_expand_pml4: obj not empty, pmap %p pm_obj %p vaddr 0x%llx i 0x%llx\n",
		     map, map->pm_obj_pml4, vaddr, i);
       }
#endif
	vm_page_insert(m, map->pm_obj_pml4, (vm_object_offset_t)i);
	vm_object_unlock(map->pm_obj_pml4);

	/*
	 *	Set the page directory entry for this page table.
	 */
	pml4p = pmap64_pml4(map, vaddr); /* refetch under lock */

	pmap_store_pte(pml4p, pa_to_pte(pa)
				| INTEL_PTE_VALID
				| INTEL_PTE_USER
				| INTEL_PTE_WRITE);

	PMAP_UNLOCK(map);

	return;
}

void
pmap_expand_pdpt(
		 pmap_t map,
		 vm_map_offset_t vaddr)
{
	vm_page_t	m;
	pmap_paddr_t	pa;
	uint64_t	i;
	ppnum_t		pn;
	pdpt_entry_t	*pdptp;

	DBG("pmap_expand_pdpt(%p,%p)\n", map, (void *)vaddr);

	while ((pdptp = pmap64_pdpt(map, vaddr)) == PDPT_ENTRY_NULL) {
		pmap_expand_pml4(map, vaddr);
	}

	/*
	 *	Allocate a VM page for the pdpt page
	 */
	while ((m = vm_page_grab()) == VM_PAGE_NULL)
		VM_PAGE_WAIT();

	/*
	 *	put the page into the pmap's obj list so it
	 *	can be found later.
	 */
	pn = m->phys_page;
	pa = i386_ptob(pn);
	i = pdptidx(map, vaddr);

	/*
	 *	Zero the page.
	 */
	pmap_zero_page(pn);

	vm_page_lockspin_queues();
	vm_page_wire(m);
	vm_page_unlock_queues();

	OSAddAtomic(1,  &inuse_ptepages_count);
	OSAddAtomic64(1,  &alloc_ptepages_count);
	PMAP_ZINFO_PALLOC(PAGE_SIZE);

	/* Take the oject lock (mutex) before the PMAP_LOCK (spinlock) */
	vm_object_lock(map->pm_obj_pdpt);

	PMAP_LOCK(map);
	/*
	 *	See if someone else expanded us first
	 */
	if (pmap64_pde(map, vaddr) != PD_ENTRY_NULL) {
		PMAP_UNLOCK(map);
		vm_object_unlock(map->pm_obj_pdpt);

		VM_PAGE_FREE(m);

		OSAddAtomic(-1,  &inuse_ptepages_count);
		PMAP_ZINFO_PFREE(PAGE_SIZE);
		return;
	}

#if 0 /* DEBUG */
       if (0 != vm_page_lookup(map->pm_obj_pdpt, (vm_object_offset_t)i)) {
	       panic("pmap_expand_pdpt: obj not empty, pmap %p pm_obj %p vaddr 0x%llx i 0x%llx\n",
		     map, map->pm_obj_pdpt, vaddr, i);
       }
#endif
	vm_page_insert(m, map->pm_obj_pdpt, (vm_object_offset_t)i);
	vm_object_unlock(map->pm_obj_pdpt);

	/*
	 *	Set the page directory entry for this page table.
	 */
	pdptp = pmap64_pdpt(map, vaddr); /* refetch under lock */

	pmap_store_pte(pdptp, pa_to_pte(pa)
				| INTEL_PTE_VALID
				| INTEL_PTE_USER
				| INTEL_PTE_WRITE);

	PMAP_UNLOCK(map);

	return;

}



/*
 *	Routine:	pmap_expand
 *
 *	Expands a pmap to be able to map the specified virtual address.
 *
 *	Allocates new virtual memory for the P0 or P1 portion of the
 *	pmap, then re-maps the physical pages that were in the old
 *	pmap to be in the new pmap.
 *
 *	Must be called with the pmap system and the pmap unlocked,
 *	since these must be unlocked to use vm_allocate or vm_deallocate.
 *	Thus it must be called in a loop that checks whether the map
 *	has been expanded enough.
 *	(We won't loop forever, since page tables aren't shrunk.)
 */
void
pmap_expand(
	pmap_t		map,
	vm_map_offset_t	vaddr)
{
	pt_entry_t		*pdp;
	register vm_page_t	m;
	register pmap_paddr_t	pa;
	uint64_t		i;
	ppnum_t                 pn;


	/*
 	 * For the kernel, the virtual address must be in or above the basement
	 * which is for kexts and is in the 512GB immediately below the kernel..
	 * XXX - should use VM_MIN_KERNEL_AND_KEXT_ADDRESS not KERNEL_BASEMENT
	 */
	if (map == kernel_pmap && 
	    !(vaddr >= KERNEL_BASEMENT && vaddr <= VM_MAX_KERNEL_ADDRESS))
		panic("pmap_expand: bad vaddr 0x%llx for kernel pmap", vaddr);


	while ((pdp = pmap64_pde(map, vaddr)) == PD_ENTRY_NULL) {
		/* need room for another pde entry */
		pmap_expand_pdpt(map, vaddr);
	}

	/*
	 *	Allocate a VM page for the pde entries.
	 */
	while ((m = vm_page_grab()) == VM_PAGE_NULL)
		VM_PAGE_WAIT();

	/*
	 *	put the page into the pmap's obj list so it
	 *	can be found later.
	 */
	pn = m->phys_page;
	pa = i386_ptob(pn);
	i = pdeidx(map, vaddr);

	/*
	 *	Zero the page.
	 */
	pmap_zero_page(pn);

	vm_page_lockspin_queues();
	vm_page_wire(m);
	vm_page_unlock_queues();

	OSAddAtomic(1,  &inuse_ptepages_count);
	OSAddAtomic64(1,  &alloc_ptepages_count);
	PMAP_ZINFO_PALLOC(PAGE_SIZE);

	/* Take the oject lock (mutex) before the PMAP_LOCK (spinlock) */
	vm_object_lock(map->pm_obj);

	PMAP_LOCK(map);

	/*
	 *	See if someone else expanded us first
	 */
	if (pmap_pte(map, vaddr) != PT_ENTRY_NULL) {
		PMAP_UNLOCK(map);
		vm_object_unlock(map->pm_obj);

		VM_PAGE_FREE(m);

		OSAddAtomic(-1,  &inuse_ptepages_count);
		PMAP_ZINFO_PFREE(PAGE_SIZE);
		return;
	}

#if 0 /* DEBUG */
       if (0 != vm_page_lookup(map->pm_obj, (vm_object_offset_t)i)) {
	       panic("pmap_expand: obj not empty, pmap 0x%x pm_obj 0x%x vaddr 0x%llx i 0x%llx\n",
		     map, map->pm_obj, vaddr, i);
       }
#endif
	vm_page_insert(m, map->pm_obj, (vm_object_offset_t)i);
	vm_object_unlock(map->pm_obj);

	/*
	 *	Set the page directory entry for this page table.
	 */
	pdp = pmap_pde(map, vaddr);
	pmap_store_pte(pdp, pa_to_pte(pa)
				| INTEL_PTE_VALID
				| INTEL_PTE_USER
				| INTEL_PTE_WRITE);

	PMAP_UNLOCK(map);

	return;
}

/* On K64 machines with more than 32GB of memory, pmap_steal_memory
 * will allocate past the 1GB of pre-expanded virtual kernel area. This
 * function allocates all the page tables using memory from the same pool
 * that pmap_steal_memory uses, rather than calling vm_page_grab (which
 * isn't available yet). */
void
pmap_pre_expand(pmap_t pmap, vm_map_offset_t vaddr)
{
	ppnum_t pn;
	pt_entry_t		*pte;

	PMAP_LOCK(pmap);

	if(pmap64_pdpt(pmap, vaddr) == PDPT_ENTRY_NULL) {
		if (!pmap_next_page_hi(&pn))
			panic("pmap_pre_expand");

		pmap_zero_page(pn);

		pte = pmap64_pml4(pmap, vaddr);

		pmap_store_pte(pte, pa_to_pte(i386_ptob(pn))
				| INTEL_PTE_VALID
				| INTEL_PTE_USER
				| INTEL_PTE_WRITE);
	}

	if(pmap64_pde(pmap, vaddr) == PD_ENTRY_NULL) {
		if (!pmap_next_page_hi(&pn))
			panic("pmap_pre_expand");

		pmap_zero_page(pn);

		pte = pmap64_pdpt(pmap, vaddr);

		pmap_store_pte(pte, pa_to_pte(i386_ptob(pn))
				| INTEL_PTE_VALID
				| INTEL_PTE_USER
				| INTEL_PTE_WRITE);
	}

	if(pmap_pte(pmap, vaddr) == PT_ENTRY_NULL) {
		if (!pmap_next_page_hi(&pn))
			panic("pmap_pre_expand");

		pmap_zero_page(pn);

		pte = pmap64_pde(pmap, vaddr);

		pmap_store_pte(pte, pa_to_pte(i386_ptob(pn))
				| INTEL_PTE_VALID
				| INTEL_PTE_USER
				| INTEL_PTE_WRITE);
	}

	PMAP_UNLOCK(pmap);
}

/*
 * pmap_sync_page_data_phys(ppnum_t pa)
 * 
 * Invalidates all of the instruction cache on a physical page and
 * pushes any dirty data from the data cache for the same physical page
 * Not required in i386.
 */
void
pmap_sync_page_data_phys(__unused ppnum_t pa)
{
	return;
}

/*
 * pmap_sync_page_attributes_phys(ppnum_t pa)
 * 
 * Write back and invalidate all cachelines on a physical page.
 */
void
pmap_sync_page_attributes_phys(ppnum_t pa)
{
	cache_flush_page_phys(pa);
}



#ifdef CURRENTLY_UNUSED_AND_UNTESTED

int	collect_ref;
int	collect_unref;

/*
 *	Routine:	pmap_collect
 *	Function:
 *		Garbage collects the physical map system for
 *		pages which are no longer used.
 *		Success need not be guaranteed -- that is, there
 *		may well be pages which are not referenced, but
 *		others may be collected.
 *	Usage:
 *		Called by the pageout daemon when pages are scarce.
 */
void
pmap_collect(
	pmap_t 		p)
{
	register pt_entry_t	*pdp, *ptp;
	pt_entry_t		*eptp;
	int			wired;

	if (p == PMAP_NULL)
		return;

	if (p == kernel_pmap)
		return;

	/*
	 *	Garbage collect map.
	 */
	PMAP_LOCK(p);

	for (pdp = (pt_entry_t *)p->dirbase;
	     pdp < (pt_entry_t *)&p->dirbase[(UMAXPTDI+1)];
	     pdp++)
	{
	   if (*pdp & INTEL_PTE_VALID) {
	      if(*pdp & INTEL_PTE_REF) {
		pmap_store_pte(pdp, *pdp & ~INTEL_PTE_REF);
		collect_ref++;
	      } else {
		collect_unref++;
		ptp = pmap_pte(p, pdetova(pdp - (pt_entry_t *)p->dirbase));
		eptp = ptp + NPTEPG;

		/*
		 * If the pte page has any wired mappings, we cannot
		 * free it.
		 */
		wired = 0;
		{
		    register pt_entry_t *ptep;
		    for (ptep = ptp; ptep < eptp; ptep++) {
			if (iswired(*ptep)) {
			    wired = 1;
			    break;
			}
		    }
		}
		if (!wired) {
		    /*
		     * Remove the virtual addresses mapped by this pte page.
		     */
		    pmap_remove_range(p,
				pdetova(pdp - (pt_entry_t *)p->dirbase),
				ptp,
				eptp);

		    /*
		     * Invalidate the page directory pointer.
		     */
		    pmap_store_pte(pdp, 0x0);
		 
		    PMAP_UNLOCK(p);

		    /*
		     * And free the pte page itself.
		     */
		    {
			register vm_page_t m;

			vm_object_lock(p->pm_obj);

			m = vm_page_lookup(p->pm_obj,(vm_object_offset_t)(pdp - (pt_entry_t *)&p->dirbase[0]));
			if (m == VM_PAGE_NULL)
			    panic("pmap_collect: pte page not in object");

			vm_object_unlock(p->pm_obj);

			VM_PAGE_FREE(m);

			OSAddAtomic(-1,  &inuse_ptepages_count);
			PMAP_ZINFO_PFREE(PAGE_SIZE);
		    }

		    PMAP_LOCK(p);
		}
	      }
	   }
	}

	PMAP_UPDATE_TLBS(p, 0x0, 0xFFFFFFFFFFFFF000ULL);
	PMAP_UNLOCK(p);
	return;

}
#endif


void
pmap_copy_page(ppnum_t src, ppnum_t dst)
{
	bcopy_phys((addr64_t)i386_ptob(src),
		   (addr64_t)i386_ptob(dst),
		   PAGE_SIZE);
}


/*
 *	Routine:	pmap_pageable
 *	Function:
 *		Make the specified pages (by pmap, offset)
 *		pageable (or not) as requested.
 *
 *		A page which is not pageable may not take
 *		a fault; therefore, its page table entry
 *		must remain valid for the duration.
 *
 *		This routine is merely advisory; pmap_enter
 *		will specify that these pages are to be wired
 *		down (or not) as appropriate.
 */
void
pmap_pageable(
	__unused pmap_t			pmap,
	__unused vm_map_offset_t	start_addr,
	__unused vm_map_offset_t	end_addr,
	__unused boolean_t		pageable)
{
#ifdef	lint
	pmap++; start_addr++; end_addr++; pageable++;
#endif	/* lint */
}


void 
invalidate_icache(__unused vm_offset_t	addr,
		  __unused unsigned	cnt,
		  __unused int		phys)
{
	return;
}

void 
flush_dcache(__unused vm_offset_t	addr,
	     __unused unsigned		count,
	     __unused int		phys)
{
	return;
}

#if CONFIG_DTRACE
/*
 * Constrain DTrace copyin/copyout actions
 */
extern kern_return_t dtrace_copyio_preflight(addr64_t);
extern kern_return_t dtrace_copyio_postflight(addr64_t);

kern_return_t dtrace_copyio_preflight(__unused addr64_t va)
{
	thread_t thread = current_thread();
	uint64_t ccr3;

	if (current_map() == kernel_map)
		return KERN_FAILURE;
	else if (((ccr3 = get_cr3_base()) != thread->map->pmap->pm_cr3) && (no_shared_cr3 == FALSE))
		return KERN_FAILURE;
	else if (no_shared_cr3 && (ccr3 != kernel_pmap->pm_cr3))
		return KERN_FAILURE;
	else if (thread->machine.specFlags & CopyIOActive)
		return KERN_FAILURE;
	else
		return KERN_SUCCESS;
}
 
kern_return_t dtrace_copyio_postflight(__unused addr64_t va)
{
	return KERN_SUCCESS;
}
#endif /* CONFIG_DTRACE */

#include <mach_vm_debug.h>
#if	MACH_VM_DEBUG
#include <vm/vm_debug.h>

int
pmap_list_resident_pages(
	__unused pmap_t		pmap,
	__unused vm_offset_t	*listp,
	__unused int		space)
{
	return 0;
}
#endif	/* MACH_VM_DEBUG */



/* temporary workaround */
boolean_t
coredumpok(__unused vm_map_t map, __unused vm_offset_t va)
{
#if 0
	pt_entry_t     *ptep;

	ptep = pmap_pte(map->pmap, va);
	if (0 == ptep)
		return FALSE;
	return ((*ptep & (INTEL_PTE_NCACHE | INTEL_PTE_WIRED)) != (INTEL_PTE_NCACHE | INTEL_PTE_WIRED));
#else
	return TRUE;
#endif
}


boolean_t
phys_page_exists(ppnum_t pn)
{
	assert(pn != vm_page_fictitious_addr);

	if (!pmap_initialized)
		return TRUE;

	if (pn == vm_page_guard_addr)
		return FALSE;

	if (!IS_MANAGED_PAGE(ppn_to_pai(pn)))
		return FALSE;

	return TRUE;
}



void
pmap_switch(pmap_t tpmap)
{
        spl_t	s;

	s = splhigh();		/* Make sure interruptions are disabled */
	set_dirbase(tpmap, current_thread());
	splx(s);
}


/*
 * disable no-execute capability on
 * the specified pmap
 */
void
pmap_disable_NX(pmap_t pmap)
{
        pmap->nx_enabled = 0;
}

void 
pt_fake_zone_init(int zone_index)
{
	pt_fake_zone_index = zone_index;
}

void
pt_fake_zone_info(
	int		*count,
	vm_size_t	*cur_size,
	vm_size_t	*max_size,
	vm_size_t	*elem_size,
	vm_size_t	*alloc_size,
	uint64_t	*sum_size,
	int		*collectable,
	int		*exhaustable,
	int		*caller_acct)
{
        *count      = inuse_ptepages_count;
	*cur_size   = PAGE_SIZE * inuse_ptepages_count;
	*max_size   = PAGE_SIZE * (inuse_ptepages_count +
				   vm_page_inactive_count +
				   vm_page_active_count +
				   vm_page_free_count);
	*elem_size  = PAGE_SIZE;
	*alloc_size = PAGE_SIZE;
	*sum_size = alloc_ptepages_count * PAGE_SIZE;

	*collectable = 1;
	*exhaustable = 0;
	*caller_acct = 1;
}

static inline void
pmap_cpuset_NMIPI(cpu_set cpu_mask) {
	unsigned int cpu, cpu_bit;
	uint64_t deadline;

	for (cpu = 0, cpu_bit = 1; cpu < real_ncpus; cpu++, cpu_bit <<= 1) {
		if (cpu_mask & cpu_bit)
			cpu_NMI_interrupt(cpu);
	}
	deadline = mach_absolute_time() + (LockTimeOut);
	while (mach_absolute_time() < deadline)
		cpu_pause();
}

/*
 * Called with pmap locked, we:
 *  - scan through per-cpu data to see which other cpus need to flush
 *  - send an IPI to each non-idle cpu to be flushed
 *  - wait for all to signal back that they are inactive or we see that
 *    they are at a safe point (idle).
 *  - flush the local tlb if active for this pmap
 *  - return ... the caller will unlock the pmap
 */

void
pmap_flush_tlbs(pmap_t	pmap, vm_map_offset_t startv, vm_map_offset_t endv)
{
	unsigned int	cpu;
	unsigned int	cpu_bit;
	cpu_set		cpus_to_signal;
	unsigned int	my_cpu = cpu_number();
	pmap_paddr_t	pmap_cr3 = pmap->pm_cr3;
	boolean_t	flush_self = FALSE;
	uint64_t	deadline;
	boolean_t	pmap_is_shared = (pmap->pm_shared || (pmap == kernel_pmap));

	assert((processor_avail_count < 2) ||
	       (ml_get_interrupts_enabled() && get_preemption_level() != 0));

	/*
	 * Scan other cpus for matching active or task CR3.
	 * For idle cpus (with no active map) we mark them invalid but
	 * don't signal -- they'll check as they go busy.
	 */
	cpus_to_signal = 0;

	if (pmap_pcid_ncpus) {
		pmap_pcid_invalidate_all_cpus(pmap);
		__asm__ volatile("mfence":::"memory");
	}

	for (cpu = 0, cpu_bit = 1; cpu < real_ncpus; cpu++, cpu_bit <<= 1) {
		if (!cpu_datap(cpu)->cpu_running)
			continue;
		uint64_t	cpu_active_cr3 = CPU_GET_ACTIVE_CR3(cpu);
		uint64_t	cpu_task_cr3 = CPU_GET_TASK_CR3(cpu);

		if ((pmap_cr3 == cpu_task_cr3) ||
		    (pmap_cr3 == cpu_active_cr3) ||
		    (pmap_is_shared)) {
			if (cpu == my_cpu) {
				flush_self = TRUE;
				continue;
			}
			if (pmap_pcid_ncpus && pmap_is_shared)
				cpu_datap(cpu)->cpu_tlb_invalid_global = TRUE;
			else
				cpu_datap(cpu)->cpu_tlb_invalid_local = TRUE;
			__asm__ volatile("mfence":::"memory");

			/*
			 * We don't need to signal processors which will flush
			 * lazily at the idle state or kernel boundary.
			 * For example, if we're invalidating the kernel pmap,
			 * processors currently in userspace don't need to flush
			 * their TLBs until the next time they enter the kernel.
			 * Alterations to the address space of a task active
			 * on a remote processor result in a signal, to
			 * account for copy operations. (There may be room
			 * for optimization in such cases).
			 * The order of the loads below with respect
			 * to the store to the "cpu_tlb_invalid" field above
			 * is important--hence the barrier.
			 */
			if (CPU_CR3_IS_ACTIVE(cpu) &&
			    (pmap_cr3 == CPU_GET_ACTIVE_CR3(cpu) ||
			    pmap->pm_shared ||
			    (pmap_cr3 == CPU_GET_TASK_CR3(cpu)))) {
				cpus_to_signal |= cpu_bit;
				i386_signal_cpu(cpu, MP_TLB_FLUSH, ASYNC);
			}
		}
	}

	PMAP_TRACE_CONSTANT(PMAP_CODE(PMAP__FLUSH_TLBS) | DBG_FUNC_START,
		   pmap, cpus_to_signal, flush_self, startv, endv);

	/*
	 * Flush local tlb if required.
	 * Do this now to overlap with other processors responding.
	 */
	if (flush_self) {
		if (pmap_pcid_ncpus) {
			pmap_pcid_validate_cpu(pmap, my_cpu);
			if (pmap_is_shared)
				tlb_flush_global();
			else
				flush_tlb_raw();
		}
		else
			flush_tlb_raw();
	}

	if (cpus_to_signal) {
		cpu_set	cpus_to_respond = cpus_to_signal;

		deadline = mach_absolute_time() + LockTimeOut;
		/*
		 * Wait for those other cpus to acknowledge
		 */
		while (cpus_to_respond != 0) {
			long orig_acks = 0;

			for (cpu = 0, cpu_bit = 1; cpu < real_ncpus; cpu++, cpu_bit <<= 1) {
				/* Consider checking local/global invalidity
				 * as appropriate in the PCID case.
				 */
				if ((cpus_to_respond & cpu_bit) != 0) {
					if (!cpu_datap(cpu)->cpu_running ||
					    cpu_datap(cpu)->cpu_tlb_invalid == FALSE ||
					    !CPU_CR3_IS_ACTIVE(cpu)) {
						cpus_to_respond &= ~cpu_bit;
					}
					cpu_pause();
				}
				if (cpus_to_respond == 0)
					break;
			}
			if (cpus_to_respond && (mach_absolute_time() > deadline)) {
				if (machine_timeout_suspended())
					continue;
				pmap_tlb_flush_timeout = TRUE;
				orig_acks = NMIPI_acks;
				pmap_cpuset_NMIPI(cpus_to_respond);

				panic("TLB invalidation IPI timeout: "
				    "CPU(s) failed to respond to interrupts, unresponsive CPU bitmap: 0x%lx, NMIPI acks: orig: 0x%lx, now: 0x%lx",
				    cpus_to_respond, orig_acks, NMIPI_acks);
			}
		}
	}

	PMAP_TRACE_CONSTANT(PMAP_CODE(PMAP__FLUSH_TLBS) | DBG_FUNC_END,
	    pmap, cpus_to_signal, startv, endv, 0);
}

void
process_pmap_updates(void)
{
	int ccpu = cpu_number();
	pmap_assert(ml_get_interrupts_enabled() == 0 || get_preemption_level() != 0);
	if (pmap_pcid_ncpus) {
		pmap_pcid_validate_current();
		if (cpu_datap(ccpu)->cpu_tlb_invalid_global) {
			cpu_datap(ccpu)->cpu_tlb_invalid = FALSE;
			tlb_flush_global();
		}
		else {
			cpu_datap(ccpu)->cpu_tlb_invalid_local = FALSE;
			flush_tlb_raw();
		}
	}
	else {
		current_cpu_datap()->cpu_tlb_invalid = FALSE;
		flush_tlb_raw();
	}

	__asm__ volatile("mfence");
}

void
pmap_update_interrupt(void)
{
        PMAP_TRACE(PMAP_CODE(PMAP__UPDATE_INTERRUPT) | DBG_FUNC_START,
		   0, 0, 0, 0, 0);

	process_pmap_updates();

        PMAP_TRACE(PMAP_CODE(PMAP__UPDATE_INTERRUPT) | DBG_FUNC_END,
		   0, 0, 0, 0, 0);
}

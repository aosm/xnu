/*
 * Copyright (c) 2000-2012 Apple Inc. All rights reserved.
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
 *	File:	pmap.h
 *
 *	Authors:  Avadis Tevanian, Jr., Michael Wayne Young
 *	Date:	1985
 *
 *	Machine-dependent structures for the physical map module.
 */
#ifdef KERNEL_PRIVATE
#ifndef	_PMAP_MACHINE_
#define _PMAP_MACHINE_	1

#ifndef	ASSEMBLER


#include <mach/kern_return.h>
#include <mach/machine/vm_types.h>
#include <mach/vm_prot.h>
#include <mach/vm_statistics.h>
#include <mach/machine/vm_param.h>
#include <kern/kern_types.h>
#include <kern/thread.h>
#include <kern/simple_lock.h>
#include <mach/branch_predicates.h>

#include <i386/mp.h>
#include <i386/proc_reg.h>

#include <i386/pal_routines.h>

/*
 *	Define the generic in terms of the specific
 */

#define	INTEL_PGBYTES		I386_PGBYTES
#define INTEL_PGSHIFT		I386_PGSHIFT
#define	intel_btop(x)		i386_btop(x)
#define	intel_ptob(x)		i386_ptob(x)
#define	intel_round_page(x)	i386_round_page(x)
#define	intel_trunc_page(x)	i386_trunc_page(x)
#define trunc_intel_to_vm(x)	trunc_i386_to_vm(x)
#define round_intel_to_vm(x)	round_i386_to_vm(x)
#define vm_to_intel(x)		vm_to_i386(x)

/*
 *	i386/i486/i860 Page Table Entry
 */

#endif	/* ASSEMBLER */

#define NPGPTD          4ULL
#define PDESHIFT        21ULL
#define PTEMASK         0x1ffULL
#define PTEINDX         3ULL

#define PTESHIFT        12ULL


#ifdef __x86_64__
#define LOW_4GB_MASK	((vm_offset_t)0x00000000FFFFFFFFUL)
#endif

#define PDESIZE		sizeof(pd_entry_t) /* for assembly files */
#define PTESIZE		sizeof(pt_entry_t) /* for assembly files */

#define INTEL_OFFMASK	(I386_PGBYTES - 1)
#define INTEL_LOFFMASK	(I386_LPGBYTES - 1)
#define PG_FRAME        0x000FFFFFFFFFF000ULL
#define NPTEPG          (PAGE_SIZE/(sizeof (pt_entry_t)))
#define NPTDPG          (PAGE_SIZE/(sizeof (pd_entry_t)))

#define NBPTD           (NPGPTD << PAGE_SHIFT)
#define NPDEPTD         (NBPTD / (sizeof (pd_entry_t)))
#define NPDEPG          (PAGE_SIZE/(sizeof (pd_entry_t)))
#define NBPDE           (1ULL << PDESHIFT)
#define PDEMASK         (NBPDE - 1)

#define PTE_PER_PAGE	512 /* number of PTE's per page on any level */

 /* cleanly define parameters for all the page table levels */
typedef uint64_t        pml4_entry_t;
#define NPML4PG         (PAGE_SIZE/(sizeof (pml4_entry_t)))
#define PML4SHIFT       39
#define PML4PGSHIFT     9
#define NBPML4          (1ULL << PML4SHIFT)
#define PML4MASK        (NBPML4-1)
#define PML4_ENTRY_NULL ((pml4_entry_t *) 0)

typedef uint64_t        pdpt_entry_t;
#define NPDPTPG         (PAGE_SIZE/(sizeof (pdpt_entry_t)))
#define PDPTSHIFT       30
#define PDPTPGSHIFT     9
#define NBPDPT          (1ULL << PDPTSHIFT)
#define PDPTMASK        (NBPDPT-1)
#define PDPT_ENTRY_NULL ((pdpt_entry_t *) 0)

typedef uint64_t        pd_entry_t;
#define NPDPG           (PAGE_SIZE/(sizeof (pd_entry_t)))
#define PDSHIFT         21
#define PDPGSHIFT       9
#define NBPD            (1ULL << PDSHIFT)
#define PDMASK          (NBPD-1)
#define PD_ENTRY_NULL   ((pd_entry_t *) 0)

typedef uint64_t        pt_entry_t;
#define NPTPG           (PAGE_SIZE/(sizeof (pt_entry_t)))
#define PTSHIFT         12
#define PTPGSHIFT       9
#define NBPT            (1ULL << PTSHIFT)
#define PTMASK          (NBPT-1)
#define PT_ENTRY_NULL	((pt_entry_t *) 0)

typedef uint64_t  pmap_paddr_t;

#if	DEBUG
#define PMAP_ASSERT 1
#endif
#if PMAP_ASSERT
#define	pmap_assert(ex) ((ex) ? (void)0 : Assert(__FILE__, __LINE__, # ex))

#define pmap_assert2(ex, fmt, args...)					\
	do {								\
		if (!(ex)) {						\
			kprintf("Assertion %s failed (%s:%d, caller %p) " fmt , #ex, __FILE__, __LINE__, __builtin_return_address(0),  ##args); \
			panic("Assertion %s failed (%s:%d, caller %p) " fmt , #ex, __FILE__, __LINE__, __builtin_return_address(0),  ##args); 		\
		}							\
	} while(0)
#else
#define pmap_assert(ex)
#define pmap_assert2(ex, fmt, args...)
#endif

/* superpages */
#ifdef __x86_64__
#define SUPERPAGE_NBASEPAGES 512
#else
#define SUPERPAGE_NBASEPAGES 1	/* we don't support superpages on i386 */
#endif

/*
 * Atomic 64-bit store of a page table entry.
 */
static inline void
pmap_store_pte(pt_entry_t *entryp, pt_entry_t value)
{
	/*
	 * In the 32-bit kernel a compare-and-exchange loop was
	 * required to provide atomicity. For K64, life is easier:
	 */
	*entryp = value;
}

/* in 64 bit spaces, the number of each type of page in the page tables */
#define NPML4PGS        (1ULL * (PAGE_SIZE/(sizeof (pml4_entry_t))))
#define NPDPTPGS        (NPML4PGS * (PAGE_SIZE/(sizeof (pdpt_entry_t))))
#define NPDEPGS         (NPDPTPGS * (PAGE_SIZE/(sizeof (pd_entry_t))))
#define NPTEPGS         (NPDEPGS * (PAGE_SIZE/(sizeof (pt_entry_t))))

#define KERNEL_PML4_INDEX		511
#define KERNEL_KEXTS_INDEX	510	/* Home of KEXTs - the basement */
#define KERNEL_PHYSMAP_PML4_INDEX	509	/* virtual to physical map */ 
#define KERNEL_BASE		(0ULL - NBPML4)
#define KERNEL_BASEMENT		(KERNEL_BASE - NBPML4)

#define	VM_WIMG_COPYBACK	VM_MEM_COHERENT
#define	VM_WIMG_COPYBACKLW	VM_WIMG_COPYBACK
#define	VM_WIMG_DEFAULT		VM_MEM_COHERENT
/* ?? intel ?? */
#define VM_WIMG_IO		(VM_MEM_COHERENT | 	\
				VM_MEM_NOT_CACHEABLE | VM_MEM_GUARDED)
#define VM_WIMG_WTHRU		(VM_MEM_WRITE_THROUGH | VM_MEM_COHERENT | VM_MEM_GUARDED)
/* write combining mode, aka store gather */
#define VM_WIMG_WCOMB		(VM_MEM_NOT_CACHEABLE | VM_MEM_COHERENT) 
#define	VM_WIMG_INNERWBACK	VM_MEM_COHERENT
/*
 * Pte related macros
 */
#define KVADDR(pmi, pdpi, pdi, pti)		  \
	 ((vm_offset_t)			  \
		((uint64_t) -1    << 47)        | \
		((uint64_t)(pmi)  << PML4SHIFT) | \
		((uint64_t)(pdpi) << PDPTSHIFT) | \
		((uint64_t)(pdi)  << PDESHIFT)  | \
		((uint64_t)(pti)  << PTESHIFT))

/*
 * Size of Kernel address space.  This is the number of page table pages
 * (4MB each) to use for the kernel.  256 pages == 1 Gigabyte.
 * This **MUST** be a multiple of 4 (eg: 252, 256, 260, etc).
 */
#ifndef KVA_PAGES
#define KVA_PAGES	1024
#endif

#ifndef NKPT
#define	NKPT		500	/* actual number of kernel page tables */
#endif
#ifndef NKPDE
#define NKPDE	(KVA_PAGES - 1)	/* addressable number of page tables/pde's */
#endif



/*
 *	Convert address offset to page descriptor index
 */
#define pdptnum(pmap, a) (((vm_offset_t)(a) >> PDPTSHIFT) & PDPTMASK)
#define pdenum(pmap, a)	(((vm_offset_t)(a) >> PDESHIFT) & PDEMASK)
#define PMAP_INVALID_PDPTNUM (~0ULL)

#define pdeidx(pmap, a)    (((a) >> PDSHIFT)   & ((1ULL<<(48 - PDSHIFT)) -1))
#define pdptidx(pmap, a)   (((a) >> PDPTSHIFT) & ((1ULL<<(48 - PDPTSHIFT)) -1))
#define pml4idx(pmap, a)   (((a) >> PML4SHIFT) & ((1ULL<<(48 - PML4SHIFT)) -1))


/*
 *	Convert page descriptor index to user virtual address
 */
#define pdetova(a)	((vm_offset_t)(a) << PDESHIFT)

/*
 *	Convert address offset to page table index
 */
#define ptenum(a)	(((vm_offset_t)(a) >> PTESHIFT) & PTEMASK)

/*
 *	Hardware pte bit definitions (to be used directly on the ptes
 *	without using the bit fields).
 */

#define INTEL_PTE_VALID		0x00000001ULL
#define INTEL_PTE_WRITE		0x00000002ULL
#define INTEL_PTE_RW		0x00000002ULL
#define INTEL_PTE_USER		0x00000004ULL
#define INTEL_PTE_WTHRU		0x00000008ULL
#define INTEL_PTE_NCACHE 	0x00000010ULL
#define INTEL_PTE_REF		0x00000020ULL
#define INTEL_PTE_MOD		0x00000040ULL
#define INTEL_PTE_PS		0x00000080ULL
#define INTEL_PTE_PTA		0x00000080ULL
#define INTEL_PTE_GLOBAL	0x00000100ULL
#define INTEL_PTE_WIRED		0x00000200ULL
#define INTEL_PDPTE_NESTED	0x00000400ULL
#define INTEL_PTE_PFN		PG_FRAME

#define INTEL_PTE_NX		(1ULL << 63)

#define INTEL_PTE_INVALID       0
/* This is conservative, but suffices */
#define INTEL_PTE_RSVD		((1ULL << 10) | (1ULL << 11) | (0x1FFULL << 54))

#define INTEL_PTE_COMPRESSED	INTEL_PTE_REF /* marker, for invalid PTE only */

#define	pa_to_pte(a)		((a) & INTEL_PTE_PFN) /* XXX */
#define	pte_to_pa(p)		((p) & INTEL_PTE_PFN) /* XXX */
#define	pte_increment_pa(p)	((p) += INTEL_OFFMASK+1)

#define pte_kernel_rw(p)          ((pt_entry_t)(pa_to_pte(p) | INTEL_PTE_VALID|INTEL_PTE_RW))
#define pte_kernel_ro(p)          ((pt_entry_t)(pa_to_pte(p) | INTEL_PTE_VALID))
#define pte_user_rw(p)            ((pt_entry)t)(pa_to_pte(p) | INTEL_PTE_VALID|INTEL_PTE_USER|INTEL_PTE_RW))
#define pte_user_ro(p)            ((pt_entry_t)(pa_to_pte(p) | INTEL_PTE_VALID|INTEL_PTE_USER))

#define PMAP_DEFAULT_CACHE	0
#define PMAP_INHIBIT_CACHE	1
#define PMAP_GUARDED_CACHE	2
#define PMAP_ACTIVATE_CACHE	4
#define PMAP_NO_GUARD_CACHE	8


#ifndef	ASSEMBLER

#include <sys/queue.h>

/*
 * Address of current and alternate address space page table maps
 * and directories.
 */

extern pt_entry_t	*PTmap;
extern pdpt_entry_t	*IdlePDPT;
extern pml4_entry_t	*IdlePML4;
extern boolean_t	no_shared_cr3;
extern addr64_t		kernel64_cr3;
extern pd_entry_t	*IdlePTD;	/* physical addr of "Idle" state PTD */

extern uint64_t		pmap_pv_hashlist_walks;
extern uint64_t		pmap_pv_hashlist_cnts;
extern uint32_t		pmap_pv_hashlist_max;
extern uint32_t		pmap_kernel_text_ps;



#ifdef __x86_64__
#define ID_MAP_VTOP(x)	((void *)(((uint64_t)(x)) & LOW_4GB_MASK))

extern	uint64_t physmap_base, physmap_max;

#define NPHYSMAP (MAX(K64_MAXMEM/GB + 4, 4))

static inline boolean_t physmap_enclosed(addr64_t a) {
	return (a < (NPHYSMAP * GB));
}

static	inline void * PHYSMAP_PTOV_check(void *paddr) {
	uint64_t pvaddr = (uint64_t)paddr + physmap_base;

	if (__improbable(pvaddr >= physmap_max))
		panic("PHYSMAP_PTOV bounds exceeded, 0x%qx, 0x%qx, 0x%qx",
		      pvaddr, physmap_base, physmap_max);

	return (void *)pvaddr;
}

#define PHYSMAP_PTOV(x)	(PHYSMAP_PTOV_check((void*) (x)))

/*
 * For KASLR, we alias the master processor's IDT and GDT at fixed
 * virtual addresses to defeat SIDT/SGDT address leakage.
 * And non-boot processor's GDT aliases likewise (skipping LOWGLOBAL_ALIAS)
 * The low global vector page is mapped at a fixed alias also.
 */
#define MASTER_IDT_ALIAS	(VM_MIN_KERNEL_ADDRESS + 0x0000)
#define MASTER_GDT_ALIAS	(VM_MIN_KERNEL_ADDRESS + 0x1000)
#define LOWGLOBAL_ALIAS		(VM_MIN_KERNEL_ADDRESS + 0x2000)
#define CPU_GDT_ALIAS(_cpu)	(LOWGLOBAL_ALIAS + (0x1000*(_cpu)))

#endif /*__x86_64__ */

#include <vm/vm_page.h>

/*
 *	For each vm_page_t, there is a list of all currently
 *	valid virtual mappings of that page.  An entry is
 *	a pv_entry_t; the list is the pv_table.
 */

struct pmap {
	decl_simple_lock_data(,lock)	/* lock on map */
	pmap_paddr_t    pm_cr3;         /* physical addr */
	boolean_t       pm_shared;
        pd_entry_t      *dirbase;        /* page directory pointer */
        vm_object_t     pm_obj;         /* object to hold pde's */
        task_map_t      pm_task_map;
        pdpt_entry_t    *pm_pdpt;       /* KVA of 3rd level page */
	pml4_entry_t    *pm_pml4;       /* VKA of top level */
	vm_object_t     pm_obj_pdpt;    /* holds pdpt pages */
	vm_object_t     pm_obj_pml4;    /* holds pml4 pages */
#define	PMAP_PCID_MAX_CPUS	(48)	/* Must be a multiple of 8 */
	pcid_t		pmap_pcid_cpus[PMAP_PCID_MAX_CPUS];
	volatile uint8_t pmap_pcid_coherency_vector[PMAP_PCID_MAX_CPUS];
	struct pmap_statistics	stats;	/* map statistics */
	int		ref_count;	/* reference count */
        int		nx_enabled;
	ledger_t	ledger;		/* ledger tracking phys mappings */
};


#if NCOPY_WINDOWS > 0
#define PMAP_PDPT_FIRST_WINDOW 0
#define PMAP_PDPT_NWINDOWS 4
#define PMAP_PDE_FIRST_WINDOW (PMAP_PDPT_NWINDOWS)
#define PMAP_PDE_NWINDOWS 4
#define PMAP_PTE_FIRST_WINDOW (PMAP_PDE_FIRST_WINDOW + PMAP_PDE_NWINDOWS)
#define PMAP_PTE_NWINDOWS 4

#define PMAP_NWINDOWS_FIRSTFREE (PMAP_PTE_FIRST_WINDOW + PMAP_PTE_NWINDOWS)
#define PMAP_WINDOW_SIZE 8
#define PMAP_NWINDOWS (PMAP_NWINDOWS_FIRSTFREE + PMAP_WINDOW_SIZE)

typedef struct {
	pt_entry_t	*prv_CMAP;
	caddr_t		prv_CADDR;
} mapwindow_t;

typedef struct cpu_pmap {
        int                     pdpt_window_index;
        int                     pde_window_index;
        int                     pte_window_index;
	mapwindow_t		mapwindow[PMAP_NWINDOWS];
} cpu_pmap_t;


extern mapwindow_t *pmap_get_mapwindow(pt_entry_t pentry);
extern void         pmap_put_mapwindow(mapwindow_t *map);
#endif

typedef struct pmap_memory_regions {
	ppnum_t base;		/* first page of this region */
	ppnum_t alloc_up;	/* pages below this one have been "stolen" */
	ppnum_t alloc_down;	/* pages above this one have been "stolen" */
	ppnum_t end;		/* last page of this region */
	uint32_t type;
	uint64_t attribute;
} pmap_memory_region_t;

extern unsigned pmap_memory_region_count;
extern unsigned pmap_memory_region_current;

#define PMAP_MEMORY_REGIONS_SIZE 128

extern pmap_memory_region_t pmap_memory_regions[];
#include <i386/pmap_pcid.h>

static inline void
set_dirbase(pmap_t tpmap, __unused thread_t thread, int my_cpu) {
	int ccpu = my_cpu;
	cpu_datap(ccpu)->cpu_task_cr3 = tpmap->pm_cr3;
	cpu_datap(ccpu)->cpu_task_map = tpmap->pm_task_map;
	/*
	 * Switch cr3 if necessary
	 * - unless running with no_shared_cr3 debugging mode
	 *   and we're not on the kernel's cr3 (after pre-empted copyio)
	 */
	if (__probable(!no_shared_cr3)) {
		if (get_cr3_base() != tpmap->pm_cr3) {
			if (pmap_pcid_ncpus) {
				pmap_pcid_activate(tpmap, ccpu);
			}
			else
				set_cr3_raw(tpmap->pm_cr3);
		}
	} else {
		if (get_cr3_base() != cpu_datap(ccpu)->cpu_kernel_cr3)
			set_cr3_raw(cpu_datap(ccpu)->cpu_kernel_cr3);
	}
}

/*
 *	External declarations for PMAP_ACTIVATE.
 */

extern void		process_pmap_updates(void);
extern void		pmap_update_interrupt(void);

/*
 *	Machine dependent routines that are used only for i386/i486/i860.
 */

extern addr64_t		(kvtophys)(
				vm_offset_t	addr);

extern kern_return_t	pmap_expand(
				pmap_t		pmap,
				vm_map_offset_t	addr,
				unsigned int options);
#if	!defined(__x86_64__)
extern pt_entry_t	*pmap_pte(
				struct pmap	*pmap,
				vm_map_offset_t	addr);

extern pd_entry_t	*pmap_pde(
				struct pmap	*pmap,
				vm_map_offset_t	addr);

extern pd_entry_t	*pmap64_pde(
				struct pmap	*pmap,
				vm_map_offset_t	addr);

extern pdpt_entry_t	*pmap64_pdpt(
				struct pmap	*pmap,
				vm_map_offset_t	addr);
#endif
extern vm_offset_t	pmap_map(
				vm_offset_t	virt,
				vm_map_offset_t	start,
				vm_map_offset_t	end,
				vm_prot_t	prot,
				unsigned int	flags);

extern vm_offset_t	pmap_map_bd(
				vm_offset_t	virt,
				vm_map_offset_t	start,
				vm_map_offset_t	end,
				vm_prot_t	prot,
				unsigned int	flags);

extern void		pmap_bootstrap(
				vm_offset_t	load_start,
				boolean_t	IA32e);

extern boolean_t	pmap_valid_page(
				ppnum_t	pn);

extern int		pmap_list_resident_pages(
				struct pmap	*pmap,
				vm_offset_t	*listp,
				int		space);
extern void		x86_filter_TLB_coherency_interrupts(boolean_t);
/*
 * Get cache attributes (as pagetable bits) for the specified phys page
 */
extern	unsigned	pmap_get_cache_attributes(ppnum_t);
#if NCOPY_WINDOWS > 0
extern struct cpu_pmap	*pmap_cpu_alloc(
				boolean_t	is_boot_cpu);
extern void		pmap_cpu_free(
				struct cpu_pmap	*cp);
#endif

extern void		pmap_map_block(
				pmap_t pmap, 
				addr64_t va,
				ppnum_t pa,
				uint32_t size,
				vm_prot_t prot,
				int attr,
				unsigned int flags);
				
extern void invalidate_icache(vm_offset_t addr, unsigned cnt, int phys);
extern void flush_dcache(vm_offset_t addr, unsigned count, int phys);
extern ppnum_t          pmap_find_phys(pmap_t map, addr64_t va);

extern void pmap_cpu_init(void);
extern void pmap_disable_NX(pmap_t pmap);

extern void pt_fake_zone_init(int);
extern void pt_fake_zone_info(int *, vm_size_t *, vm_size_t *, vm_size_t *, vm_size_t *, 
			      uint64_t *, int *, int *, int *);
extern void pmap_pagetable_corruption_msg_log(int (*)(const char * fmt, ...)__printflike(1,2));

/*
 *	Macros for speed.
 */


#include <kern/spl.h>

				  
#define PMAP_ACTIVATE_MAP(map, thread, my_cpu)	{				\
	register pmap_t		tpmap;					\
                                                                        \
        tpmap = vm_map_pmap(map);					\
        set_dirbase(tpmap, thread, my_cpu);					\
}

#if   defined(__x86_64__)
#define PMAP_DEACTIVATE_MAP(map, thread, ccpu)				\
	pmap_assert(pmap_pcid_ncpus ? (pcid_for_pmap_cpu_tuple(map->pmap, ccpu) == (get_cr3_raw() & 0xFFF)) : TRUE);
#else
#define PMAP_DEACTIVATE_MAP(map, thread)
#endif

#define	PMAP_SWITCH_CONTEXT(old_th, new_th, my_cpu) {			\
                                                                        \
	pmap_assert(ml_get_interrupts_enabled() == FALSE);		\
	if (old_th->map != new_th->map) {				\
		PMAP_DEACTIVATE_MAP(old_th->map, old_th, my_cpu);		\
		PMAP_ACTIVATE_MAP(new_th->map, new_th, my_cpu);		\
	}								\
}

#if NCOPY_WINDOWS > 0
#define	PMAP_SWITCH_USER(th, new_map, my_cpu) {				\
	spl_t		spl;						\
									\
	spl = splhigh();						\
	PMAP_DEACTIVATE_MAP(th->map, th);				\
	th->map = new_map;						\
	PMAP_ACTIVATE_MAP(th->map, th);					\
	splx(spl);							\
	inval_copy_windows(th);						\
}
#else
#define	PMAP_SWITCH_USER(th, new_map, my_cpu) {				\
	spl_t		spl;						\
									\
	spl = splhigh();						\
	PMAP_DEACTIVATE_MAP(th->map, th, my_cpu);				\
	th->map = new_map;						\
	PMAP_ACTIVATE_MAP(th->map, th, my_cpu);				\
	splx(spl);							\
}
#endif

/*
 * Marking the current cpu's cr3 inactive is achieved by setting its lsb.
 * Marking the current cpu's cr3 active once more involves clearng this bit.
 * Note that valid page tables are page-aligned and so the bottom 12 bits
 * are normally zero, modulo PCID.
 * We can only mark the current cpu active/inactive but we can test any cpu.
 */
#define CPU_CR3_MARK_INACTIVE()						\
	current_cpu_datap()->cpu_active_cr3 |= 1

#define CPU_CR3_MARK_ACTIVE()	 					\
	current_cpu_datap()->cpu_active_cr3 &= ~1

#define CPU_CR3_IS_ACTIVE(cpu)						\
	((cpu_datap(cpu)->cpu_active_cr3 & 1) == 0)

#define CPU_GET_ACTIVE_CR3(cpu)						\
	(cpu_datap(cpu)->cpu_active_cr3 & ~1)

#define CPU_GET_TASK_CR3(cpu)						\
	(cpu_datap(cpu)->cpu_task_cr3)

/*
 *	Mark this cpu idle, and remove it from the active set,
 *	since it is not actively using any pmap.  Signal_cpus
 *	will notice that it is idle, and avoid signaling it,
 *	but will queue the update request for when the cpu
 *	becomes active.
 */
#define MARK_CPU_IDLE(my_cpu)	{					\
	assert(ml_get_interrupts_enabled() == FALSE);			\
	CPU_CR3_MARK_INACTIVE();					\
	mfence();									\
}

#define MARK_CPU_ACTIVE(my_cpu) {					\
	assert(ml_get_interrupts_enabled() == FALSE);			\
	/*								\
	 *	If a kernel_pmap update was requested while this cpu	\
	 *	was idle, process it as if we got the interrupt.	\
	 *	Before doing so, remove this cpu from the idle set.	\
	 *	Since we do not grab any pmap locks while we flush	\
	 *	our TLB, another cpu may start an update operation	\
	 *	before we finish.  Removing this cpu from the idle	\
	 *	set assures that we will receive another update		\
	 *	interrupt if this happens.				\
	 */								\
	CPU_CR3_MARK_ACTIVE();						\
	mfence();									\
									\
	if (current_cpu_datap()->cpu_tlb_invalid)			\
	    process_pmap_updates();					\
}

#define PMAP_CONTEXT(pmap, thread)

#define pmap_kernel_va(VA)	\
	((((vm_offset_t) (VA)) >= vm_min_kernel_address) &&	\
	 (((vm_offset_t) (VA)) <= vm_max_kernel_address))


#define pmap_compressed(pmap)		((pmap)->stats.compressed)
#define pmap_resident_count(pmap)	((pmap)->stats.resident_count)
#define pmap_resident_max(pmap)		((pmap)->stats.resident_max)
#define	pmap_copy(dst_pmap,src_pmap,dst_addr,len,src_addr)
#define	pmap_attribute(pmap,addr,size,attr,value) \
					(KERN_INVALID_ADDRESS)
#define	pmap_attribute_cache_sync(addr,size,attr,value) \
					(KERN_INVALID_ADDRESS)

#define MACHINE_PMAP_IS_EMPTY	1
extern boolean_t pmap_is_empty(pmap_t		pmap,
			       vm_map_offset_t	start,
			       vm_map_offset_t	end);

#define MACHINE_BOOTSTRAPPTD	1	/* Static bootstrap page-tables */

kern_return_t
pmap_permissions_verify(pmap_t, vm_map_t, vm_offset_t, vm_offset_t);

#endif	/* ASSEMBLER */


#endif	/* _PMAP_MACHINE_ */


#endif  /* KERNEL_PRIVATE */

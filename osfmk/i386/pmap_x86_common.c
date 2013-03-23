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
#include <vm/pmap.h>
#include <vm/vm_map.h>
#include <i386/pmap_internal.h>


void		pmap_remove_range(
			pmap_t		pmap,
			vm_map_offset_t	va,
			pt_entry_t	*spte,
			pt_entry_t	*epte);

pv_rooted_entry_t	pv_head_table;		/* array of entries, one per
						 * page */
thread_call_t 		mapping_adjust_call;
static thread_call_data_t mapping_adjust_call_data;
uint32_t		mappingrecurse = 0;

pmap_pagetable_corruption_record_t pmap_pagetable_corruption_records[PMAP_PAGETABLE_CORRUPTION_MAX_LOG];
uint32_t pmap_pagetable_corruption_incidents;
uint64_t pmap_pagetable_corruption_last_abstime = (~(0ULL) >> 1);
uint64_t pmap_pagetable_corruption_interval_abstime;
thread_call_t 	pmap_pagetable_corruption_log_call;
static thread_call_data_t 	pmap_pagetable_corruption_log_call_data;
boolean_t pmap_pagetable_corruption_timeout = FALSE;

/*
 * The Intel platform can nest at the PDE level, so NBPDE (i.e. 2MB) at a time,
 * on a NBPDE boundary.
 */

/* These symbols may be referenced directly by VM */
uint64_t pmap_nesting_size_min = NBPDE;
uint64_t pmap_nesting_size_max = 0 - (uint64_t)NBPDE;

/*
 *	kern_return_t pmap_nest(grand, subord, va_start, size)
 *
 *	grand  = the pmap that we will nest subord into
 *	subord = the pmap that goes into the grand
 *	va_start  = start of range in pmap to be inserted
 *	nstart  = start of range in pmap nested pmap
 *	size   = Size of nest area (up to 16TB)
 *
 *	Inserts a pmap into another.  This is used to implement shared segments.
 *
 *	Note that we depend upon higher level VM locks to insure that things don't change while
 *	we are doing this.  For example, VM should not be doing any pmap enters while it is nesting
 *	or do 2 nests at once.
 */

/*
 * This routine can nest subtrees either at the PDPT level (1GiB) or at the
 * PDE level (2MiB). We currently disallow disparate offsets for the "subord"
 * container and the "grand" parent. A minor optimization to consider for the
 * future: make the "subord" truly a container rather than a full-fledged
 * pagetable hierarchy which can be unnecessarily sparse (DRK).
 */

kern_return_t pmap_nest(pmap_t grand, pmap_t subord, addr64_t va_start, addr64_t nstart, uint64_t size) {
	vm_map_offset_t	vaddr, nvaddr;
	pd_entry_t	*pde,*npde;
	unsigned int	i;
	uint64_t	num_pde;

	if ((size & (pmap_nesting_size_min-1)) ||
	    (va_start & (pmap_nesting_size_min-1)) ||
	    (nstart & (pmap_nesting_size_min-1)) ||
	    ((size >> 28) > 65536))	/* Max size we can nest is 16TB */
		return KERN_INVALID_VALUE;

	if(size == 0) {
		panic("pmap_nest: size is invalid - %016llX\n", size);
	}

	if (va_start != nstart)
		panic("pmap_nest: va_start(0x%llx) != nstart(0x%llx)\n", va_start, nstart);

	PMAP_TRACE(PMAP_CODE(PMAP__NEST) | DBG_FUNC_START,
	    (int) grand, (int) subord,
	    (int) (va_start>>32), (int) va_start, 0);

	nvaddr = (vm_map_offset_t)nstart;
	num_pde = size >> PDESHIFT;

	PMAP_LOCK(subord);

	subord->pm_shared = TRUE;

	for (i = 0; i < num_pde;) {
		if (((nvaddr & PDPTMASK) == 0) && (num_pde - i) >= NPDEPG && cpu_64bit) {

			npde = pmap64_pdpt(subord, nvaddr);

			while (0 == npde || ((*npde & INTEL_PTE_VALID) == 0)) {
				PMAP_UNLOCK(subord);
				pmap_expand_pdpt(subord, nvaddr);
				PMAP_LOCK(subord);
				npde = pmap64_pdpt(subord, nvaddr);
			}
			*npde |= INTEL_PDPTE_NESTED;
			nvaddr += NBPDPT;
			i += (uint32_t)NPDEPG;
		}
		else {
			npde = pmap_pde(subord, nvaddr);

			while (0 == npde || ((*npde & INTEL_PTE_VALID) == 0)) {
				PMAP_UNLOCK(subord);
				pmap_expand(subord, nvaddr);
				PMAP_LOCK(subord);
				npde = pmap_pde(subord, nvaddr);
			}
			nvaddr += NBPDE;
			i++;
		}
	}

	PMAP_UNLOCK(subord);

	vaddr = (vm_map_offset_t)va_start;

	PMAP_LOCK(grand);

	for (i = 0;i < num_pde;) {
		pd_entry_t tpde;

		if (((vaddr & PDPTMASK) == 0) && ((num_pde - i) >= NPDEPG) && cpu_64bit) {
			npde = pmap64_pdpt(subord, vaddr);
			if (npde == 0)
				panic("pmap_nest: no PDPT, subord %p nstart 0x%llx", subord, vaddr);
			tpde = *npde;
			pde = pmap64_pdpt(grand, vaddr);
			if (0 == pde) {
				PMAP_UNLOCK(grand);
				pmap_expand_pml4(grand, vaddr);
				PMAP_LOCK(grand);
				pde = pmap64_pdpt(grand, vaddr);
			}
			if (pde == 0)
				panic("pmap_nest: no PDPT, grand  %p vaddr 0x%llx", grand, vaddr);
			pmap_store_pte(pde, tpde);
			vaddr += NBPDPT;
			i += (uint32_t) NPDEPG;
		}
		else {
			npde = pmap_pde(subord, nstart);
			if (npde == 0)
				panic("pmap_nest: no npde, subord %p nstart 0x%llx", subord, nstart);
			tpde = *npde;
			nstart += NBPDE;
			pde = pmap_pde(grand, vaddr);
			if ((0 == pde) && cpu_64bit) {
				PMAP_UNLOCK(grand);
				pmap_expand_pdpt(grand, vaddr);
				PMAP_LOCK(grand);
				pde = pmap_pde(grand, vaddr);
			}

			if (pde == 0)
				panic("pmap_nest: no pde, grand  %p vaddr 0x%llx", grand, vaddr);
			vaddr += NBPDE;
			pmap_store_pte(pde, tpde);
			i++;
		}
	}

	PMAP_UNLOCK(grand);

	PMAP_TRACE(PMAP_CODE(PMAP__NEST) | DBG_FUNC_END, 0, 0, 0, 0, 0);

	return KERN_SUCCESS;
}

/*
 *	kern_return_t pmap_unnest(grand, vaddr)
 *
 *	grand  = the pmap that we will un-nest subord from
 *	vaddr  = start of range in pmap to be unnested
 *
 *	Removes a pmap from another.  This is used to implement shared segments.
 */

kern_return_t pmap_unnest(pmap_t grand, addr64_t vaddr, uint64_t size) {
			
	pd_entry_t *pde;
	unsigned int i;
	uint64_t num_pde;
	addr64_t va_start, va_end;
	uint64_t npdpt = PMAP_INVALID_PDPTNUM;

	PMAP_TRACE(PMAP_CODE(PMAP__UNNEST) | DBG_FUNC_START,
	    (int) grand, 
	    (int) (vaddr>>32), (int) vaddr, 0, 0);

	if ((size & (pmap_nesting_size_min-1)) ||
	    (vaddr & (pmap_nesting_size_min-1))) {
		panic("pmap_unnest(%p,0x%llx,0x%llx): unaligned...\n",
		    grand, vaddr, size);
	}

	/* align everything to PDE boundaries */
	va_start = vaddr & ~(NBPDE-1);
	va_end = (vaddr + size + NBPDE - 1) & ~(NBPDE-1);
	size = va_end - va_start;

	PMAP_LOCK(grand);

	num_pde = size >> PDESHIFT;
	vaddr = va_start;

	for (i = 0; i < num_pde; ) {
		if ((pdptnum(grand, vaddr) != npdpt) && cpu_64bit) {
			npdpt = pdptnum(grand, vaddr);
			pde = pmap64_pdpt(grand, vaddr);
			if (pde && (*pde & INTEL_PDPTE_NESTED)) {
				pmap_store_pte(pde, (pd_entry_t)0);
				i += (uint32_t) NPDEPG;
				vaddr += NBPDPT;
				continue;
			}
		}
		pde = pmap_pde(grand, (vm_map_offset_t)vaddr);
		if (pde == 0)
			panic("pmap_unnest: no pde, grand %p vaddr 0x%llx\n", grand, vaddr);
		pmap_store_pte(pde, (pd_entry_t)0);
		i++;
		vaddr += NBPDE;
	}

	PMAP_UPDATE_TLBS(grand, va_start, va_end);

	PMAP_UNLOCK(grand);
		
	PMAP_TRACE(PMAP_CODE(PMAP__UNNEST) | DBG_FUNC_END, 0, 0, 0, 0, 0);

	return KERN_SUCCESS;
}

/* Invoked by the Mach VM to determine the platform specific unnest region */

boolean_t pmap_adjust_unnest_parameters(pmap_t p, vm_map_offset_t *s, vm_map_offset_t *e) {
	pd_entry_t *pdpte;
	boolean_t rval = FALSE;

	if (!cpu_64bit)
		return rval;

	PMAP_LOCK(p);

	pdpte = pmap64_pdpt(p, *s);
	if (pdpte && (*pdpte & INTEL_PDPTE_NESTED)) {
		*s &= ~(NBPDPT -1);
		rval = TRUE;
	}

	pdpte = pmap64_pdpt(p, *e);
	if (pdpte && (*pdpte & INTEL_PDPTE_NESTED)) {
		*e = ((*e + NBPDPT) & ~(NBPDPT -1));
		rval = TRUE;
	}

	PMAP_UNLOCK(p);

	return rval;
}

/*
 * pmap_find_phys returns the (4K) physical page number containing a
 * given virtual address in a given pmap.
 * Note that pmap_pte may return a pde if this virtual address is
 * mapped by a large page and this is taken into account in order
 * to return the correct page number in this case.
 */
ppnum_t
pmap_find_phys(pmap_t pmap, addr64_t va)
{
	pt_entry_t	*ptp;
	pd_entry_t	*pdep;
	ppnum_t		ppn = 0;
	pd_entry_t	pde;
	pt_entry_t	pte;

	mp_disable_preemption();

	/* This refcount test is a band-aid--several infrastructural changes
	 * are necessary to eliminate invocation of this routine from arbitrary
	 * contexts.
	 */
	
	if (!pmap->ref_count)
		goto pfp_exit;

	pdep = pmap_pde(pmap, va);

	if ((pdep != PD_ENTRY_NULL) && ((pde = *pdep) & INTEL_PTE_VALID)) {
		if (pde & INTEL_PTE_PS) {
			ppn = (ppnum_t) i386_btop(pte_to_pa(pde));
			ppn += (ppnum_t) ptenum(va);
		}
		else {
			ptp = pmap_pte(pmap, va);
			if ((PT_ENTRY_NULL != ptp) && (((pte = *ptp) & INTEL_PTE_VALID) != 0)) {
				ppn = (ppnum_t) i386_btop(pte_to_pa(pte));
			}
		}
	}
pfp_exit:	
	mp_enable_preemption();

        return ppn;
}

/*
 *	Insert the given physical page (p) at
 *	the specified virtual address (v) in the
 *	target physical map with the protection requested.
 *
 *	If specified, the page will be wired down, meaning
 *	that the related pte cannot be reclaimed.
 *
 *	NB:  This is the only routine which MAY NOT lazy-evaluate
 *	or lose information.  That is, this routine must actually
 *	insert this page into the given map NOW.
 */
void
pmap_enter(
	register pmap_t		pmap,
 	vm_map_offset_t		vaddr,
	ppnum_t                 pn,
	vm_prot_t		prot,
	unsigned int 		flags,
	boolean_t		wired)
{
	pt_entry_t		*pte;
	pv_rooted_entry_t	pv_h;
	int			pai;
	pv_hashed_entry_t	pvh_e;
	pv_hashed_entry_t	pvh_new;
	pt_entry_t		template;
	pmap_paddr_t		old_pa;
	pmap_paddr_t		pa = (pmap_paddr_t) i386_ptob(pn);
	boolean_t		need_tlbflush = FALSE;
	boolean_t		set_NX;
	char			oattr;
	boolean_t		old_pa_locked;
	/* 2MiB mappings are confined to x86_64 by VM */
	boolean_t		superpage = flags & VM_MEM_SUPERPAGE;
	vm_object_t		delpage_pm_obj = NULL;
	int			delpage_pde_index = 0;
	pt_entry_t		old_pte;

	pmap_intr_assert();
	assert(pn != vm_page_fictitious_addr);

	if (pmap == PMAP_NULL)
		return;
	if (pn == vm_page_guard_addr)
		return;

	PMAP_TRACE(PMAP_CODE(PMAP__ENTER) | DBG_FUNC_START,
		   pmap,
		   (uint32_t) (vaddr >> 32), (uint32_t) vaddr,
		   pn, prot);

	if ((prot & VM_PROT_EXECUTE) || !nx_enabled || !pmap->nx_enabled)
		set_NX = FALSE;
	else
		set_NX = TRUE;

	/*
	 *	Must allocate a new pvlist entry while we're unlocked;
	 *	zalloc may cause pageout (which will lock the pmap system).
	 *	If we determine we need a pvlist entry, we will unlock
	 *	and allocate one.  Then we will retry, throughing away
	 *	the allocated entry later (if we no longer need it).
	 */

	pvh_new = PV_HASHED_ENTRY_NULL;
Retry:
	pvh_e = PV_HASHED_ENTRY_NULL;

	PMAP_LOCK(pmap);

	/*
	 *	Expand pmap to include this pte.  Assume that
	 *	pmap is always expanded to include enough hardware
	 *	pages to map one VM page.
	 */
	 if(superpage) {
	 	while ((pte = pmap64_pde(pmap, vaddr)) == PD_ENTRY_NULL) {
			/* need room for another pde entry */
			PMAP_UNLOCK(pmap);
			pmap_expand_pdpt(pmap, vaddr);
			PMAP_LOCK(pmap);
		}
	} else {
		while ((pte = pmap_pte(pmap, vaddr)) == PT_ENTRY_NULL) {
			/*
			 * Must unlock to expand the pmap
			 * going to grow pde level page(s)
			 */
			PMAP_UNLOCK(pmap);
			pmap_expand(pmap, vaddr);
			PMAP_LOCK(pmap);
		}
	}

	if (superpage && *pte && !(*pte & INTEL_PTE_PS)) {
		/*
		 * There is still an empty page table mapped that
		 * was used for a previous base page mapping.
		 * Remember the PDE and the PDE index, so that we
		 * can free the page at the end of this function.
		 */
		delpage_pde_index = (int)pdeidx(pmap, vaddr);
		delpage_pm_obj = pmap->pm_obj;
		*pte = 0;
	}


	old_pa = pte_to_pa(*pte);
	pai = pa_index(old_pa);
	old_pa_locked = FALSE;

	/*
	 * if we have a previous managed page, lock the pv entry now. after
	 * we lock it, check to see if someone beat us to the lock and if so
	 * drop the lock
	 */
	if ((0 != old_pa) && IS_MANAGED_PAGE(pai)) {
		LOCK_PVH(pai);
		old_pa_locked = TRUE;
		old_pa = pte_to_pa(*pte);
		if (0 == old_pa) {
			UNLOCK_PVH(pai);	/* another path beat us to it */
			old_pa_locked = FALSE;
		}
	}

	/*
	 *	Special case if the incoming physical page is already mapped
	 *	at this address.
	 */
	if (old_pa == pa) {

		/*
	         *	May be changing its wired attribute or protection
	         */

		template = pa_to_pte(pa) | INTEL_PTE_VALID;

		if (VM_MEM_NOT_CACHEABLE ==
		    (flags & (VM_MEM_NOT_CACHEABLE | VM_WIMG_USE_DEFAULT))) {
			if (!(flags & VM_MEM_GUARDED))
				template |= INTEL_PTE_PTA;
			template |= INTEL_PTE_NCACHE;
		}
		if (pmap != kernel_pmap)
			template |= INTEL_PTE_USER;
		if (prot & VM_PROT_WRITE)
			template |= INTEL_PTE_WRITE;

		if (set_NX)
			template |= INTEL_PTE_NX;

		if (wired) {
			template |= INTEL_PTE_WIRED;
			if (!iswired(*pte))
				OSAddAtomic(+1,
					&pmap->stats.wired_count);
		} else {
			if (iswired(*pte)) {
				assert(pmap->stats.wired_count >= 1);
				OSAddAtomic(-1,
					&pmap->stats.wired_count);
			}
		}
		if (superpage)		/* this path can not be used */
			template |= INTEL_PTE_PS;	/* to change the page size! */

		/* store modified PTE and preserve RC bits */
		pmap_update_pte(pte, *pte,
			template | (*pte & (INTEL_PTE_REF | INTEL_PTE_MOD)));
		if (old_pa_locked) {
			UNLOCK_PVH(pai);
			old_pa_locked = FALSE;
		}
		need_tlbflush = TRUE;
		goto Done;
	}

	/*
	 *	Outline of code from here:
	 *	   1) If va was mapped, update TLBs, remove the mapping
	 *	      and remove old pvlist entry.
	 *	   2) Add pvlist entry for new mapping
	 *	   3) Enter new mapping.
	 *
	 *	If the old physical page is not managed step 1) is skipped
	 *	(except for updating the TLBs), and the mapping is
	 *	overwritten at step 3).  If the new physical page is not
	 *	managed, step 2) is skipped.
	 */

	if (old_pa != (pmap_paddr_t) 0) {

		/*
	         *	Don't do anything to pages outside valid memory here.
	         *	Instead convince the code that enters a new mapping
	         *	to overwrite the old one.
	         */

		/* invalidate the PTE */
		pmap_update_pte(pte, *pte, (*pte & ~INTEL_PTE_VALID));
		/* propagate invalidate everywhere */
		PMAP_UPDATE_TLBS(pmap, vaddr, vaddr + PAGE_SIZE);
		/* remember reference and change */
		old_pte	= *pte;
		oattr = (char) (old_pte & (PHYS_MODIFIED | PHYS_REFERENCED));
		/* completely invalidate the PTE */
		pmap_store_pte(pte, 0);

		if (IS_MANAGED_PAGE(pai)) {
#if TESTING
			if (pmap->stats.resident_count < 1)
				panic("pmap_enter: resident_count");
#endif
			assert(pmap->stats.resident_count >= 1);
			OSAddAtomic(-1,
				&pmap->stats.resident_count);

			if (iswired(*pte)) {
#if TESTING
				if (pmap->stats.wired_count < 1)
					panic("pmap_enter: wired_count");
#endif
				assert(pmap->stats.wired_count >= 1);
				OSAddAtomic(-1,
					&pmap->stats.wired_count);
			}
			pmap_phys_attributes[pai] |= oattr;

			/*
			 *	Remove the mapping from the pvlist for
			 *	this physical page.
			 *      We'll end up with either a rooted pv or a
			 *      hashed pv
			 */
			pvh_e = pmap_pv_remove(pmap, vaddr, (ppnum_t *) &pai, &old_pte);

		} else {

			/*
			 *	old_pa is not managed.
			 *	Do removal part of accounting.
			 */

			if (iswired(*pte)) {
				assert(pmap->stats.wired_count >= 1);
				OSAddAtomic(-1,
					&pmap->stats.wired_count);
			}
		}
	}

	/*
	 * if we had a previously managed paged locked, unlock it now
	 */
	if (old_pa_locked) {
		UNLOCK_PVH(pai);
		old_pa_locked = FALSE;
	}

	pai = pa_index(pa);	/* now working with new incoming phys page */
	if (IS_MANAGED_PAGE(pai)) {

		/*
	         *	Step 2) Enter the mapping in the PV list for this
	         *	physical page.
	         */
		pv_h = pai_to_pvh(pai);

		LOCK_PVH(pai);

		if (pv_h->pmap == PMAP_NULL) {
			/*
			 *	No mappings yet, use rooted pv
			 */
			pv_h->va = vaddr;
			pv_h->pmap = pmap;
			queue_init(&pv_h->qlink);
		} else {
			/*
			 *	Add new pv_hashed_entry after header.
			 */
			if ((PV_HASHED_ENTRY_NULL == pvh_e) && pvh_new) {
				pvh_e = pvh_new;
				pvh_new = PV_HASHED_ENTRY_NULL;
			} else if (PV_HASHED_ENTRY_NULL == pvh_e) {
				PV_HASHED_ALLOC(pvh_e);
				if (PV_HASHED_ENTRY_NULL == pvh_e) {
					/*
					 * the pv list is empty. if we are on
					 * the kernel pmap we'll use one of
					 * the special private kernel pv_e's,
					 * else, we need to unlock
					 * everything, zalloc a pv_e, and
					 * restart bringing in the pv_e with
					 * us.
					 */
					if (kernel_pmap == pmap) {
						PV_HASHED_KERN_ALLOC(pvh_e);
					} else {
						UNLOCK_PVH(pai);
						PMAP_UNLOCK(pmap);
						pvh_new = (pv_hashed_entry_t) zalloc(pv_hashed_list_zone);
						goto Retry;
					}
				}
			}
			
			if (PV_HASHED_ENTRY_NULL == pvh_e)
				panic("Mapping alias chain exhaustion, possibly induced by numerous kernel virtual double mappings");

			pvh_e->va = vaddr;
			pvh_e->pmap = pmap;
			pvh_e->ppn = pn;
			pv_hash_add(pvh_e, pv_h);

			/*
			 *	Remember that we used the pvlist entry.
			 */
			pvh_e = PV_HASHED_ENTRY_NULL;
		}

		/*
	         * only count the mapping
	         * for 'managed memory'
	         */
		OSAddAtomic(+1,  & pmap->stats.resident_count);
		if (pmap->stats.resident_count > pmap->stats.resident_max) {
			pmap->stats.resident_max = pmap->stats.resident_count;
		}
	}
	/*
	 * Step 3) Enter the mapping.
	 *
	 *	Build a template to speed up entering -
	 *	only the pfn changes.
	 */
	template = pa_to_pte(pa) | INTEL_PTE_VALID;

	if (flags & VM_MEM_NOT_CACHEABLE) {
		if (!(flags & VM_MEM_GUARDED))
			template |= INTEL_PTE_PTA;
		template |= INTEL_PTE_NCACHE;
	}
	if (pmap != kernel_pmap)
		template |= INTEL_PTE_USER;
	if (prot & VM_PROT_WRITE)
		template |= INTEL_PTE_WRITE;
	if (set_NX)
		template |= INTEL_PTE_NX;
	if (wired) {
		template |= INTEL_PTE_WIRED;
		OSAddAtomic(+1,  & pmap->stats.wired_count);
	}
	if (superpage)
		template |= INTEL_PTE_PS;
	pmap_store_pte(pte, template);

	/*
	 * if this was a managed page we delayed unlocking the pv until here
	 * to prevent pmap_page_protect et al from finding it until the pte
	 * has been stored
	 */
	if (IS_MANAGED_PAGE(pai)) {
		UNLOCK_PVH(pai);
	}
Done:
	if (need_tlbflush == TRUE)
		PMAP_UPDATE_TLBS(pmap, vaddr, vaddr + PAGE_SIZE);

	if (pvh_e != PV_HASHED_ENTRY_NULL) {
		PV_HASHED_FREE_LIST(pvh_e, pvh_e, 1);
	}
	if (pvh_new != PV_HASHED_ENTRY_NULL) {
		PV_HASHED_KERN_FREE_LIST(pvh_new, pvh_new, 1);
	}
	PMAP_UNLOCK(pmap);

	if (delpage_pm_obj) {
		vm_page_t m;

		vm_object_lock(delpage_pm_obj);
		m = vm_page_lookup(delpage_pm_obj, delpage_pde_index);
		if (m == VM_PAGE_NULL)
		    panic("pmap_enter: pte page not in object");
		VM_PAGE_FREE(m);
		OSAddAtomic(-1,  &inuse_ptepages_count);
		vm_object_unlock(delpage_pm_obj);
	}

	PMAP_TRACE(PMAP_CODE(PMAP__ENTER) | DBG_FUNC_END, 0, 0, 0, 0, 0);
}

/*
 *	Remove a range of hardware page-table entries.
 *	The entries given are the first (inclusive)
 *	and last (exclusive) entries for the VM pages.
 *	The virtual address is the va for the first pte.
 *
 *	The pmap must be locked.
 *	If the pmap is not the kernel pmap, the range must lie
 *	entirely within one pte-page.  This is NOT checked.
 *	Assumes that the pte-page exists.
 */

void
pmap_remove_range(
	pmap_t			pmap,
	vm_map_offset_t		start_vaddr,
	pt_entry_t		*spte,
	pt_entry_t		*epte)
{
	pt_entry_t		*cpte;
	pv_hashed_entry_t       pvh_et = PV_HASHED_ENTRY_NULL;
	pv_hashed_entry_t       pvh_eh = PV_HASHED_ENTRY_NULL;
	pv_hashed_entry_t       pvh_e;
	int			pvh_cnt = 0;
	int			num_removed, num_unwired, num_found, num_invalid;
	int			pai;
	pmap_paddr_t		pa;
	vm_map_offset_t		vaddr;

	num_removed = 0;
	num_unwired = 0;
	num_found   = 0;
	num_invalid = 0;
#if	defined(__i386__)
	if (pmap != kernel_pmap &&
	    pmap->pm_task_map == TASK_MAP_32BIT &&
	    start_vaddr >= HIGH_MEM_BASE) {
		/*
		 * The range is in the "high_shared_pde" which is shared
		 * between the kernel and all 32-bit tasks.  It holds
		 * the 32-bit commpage but also the trampolines, GDT, etc...
		 * so we can't let user tasks remove anything from it.
		 */
		return;
	}
#endif
	/* invalidate the PTEs first to "freeze" them */
	for (cpte = spte, vaddr = start_vaddr;
	     cpte < epte;
	     cpte++, vaddr += PAGE_SIZE_64) {
		pt_entry_t p = *cpte;

		pa = pte_to_pa(p);
		if (pa == 0)
			continue;
		num_found++;

		if (iswired(p))
			num_unwired++;
		
		pai = pa_index(pa);

		if (!IS_MANAGED_PAGE(pai)) {
			/*
			 *	Outside range of managed physical memory.
			 *	Just remove the mappings.
			 */
			pmap_store_pte(cpte, 0);
			continue;
		}

		if ((p & INTEL_PTE_VALID) == 0)
			num_invalid++;

		/* invalidate the PTE */ 
		pmap_update_pte(cpte, *cpte, (*cpte & ~INTEL_PTE_VALID));
	}

	if (num_found == 0) {
		/* nothing was changed: we're done */
	        goto update_counts;
	}

	/* propagate the invalidates to other CPUs */

	PMAP_UPDATE_TLBS(pmap, start_vaddr, vaddr);

	for (cpte = spte, vaddr = start_vaddr;
	     cpte < epte;
	     cpte++, vaddr += PAGE_SIZE_64) {

		pa = pte_to_pa(*cpte);
		if (pa == 0)
			continue;

		pai = pa_index(pa);

		LOCK_PVH(pai);

		pa = pte_to_pa(*cpte);
		if (pa == 0) {
			UNLOCK_PVH(pai);
			continue;
		}
		num_removed++;

		/*
	       	 * Get the modify and reference bits, then
	       	 * nuke the entry in the page table
	       	 */
		/* remember reference and change */
		pmap_phys_attributes[pai] |=
			(char) (*cpte & (PHYS_MODIFIED | PHYS_REFERENCED));

		/*
	      	 * Remove the mapping from the pvlist for this physical page.
	         */
		pvh_e = pmap_pv_remove(pmap, vaddr, (ppnum_t *) &pai, cpte);

		/* completely invalidate the PTE */
		pmap_store_pte(cpte, 0);

		UNLOCK_PVH(pai);

		if (pvh_e != PV_HASHED_ENTRY_NULL) {
			pvh_e->qlink.next = (queue_entry_t) pvh_eh;
			pvh_eh = pvh_e;

			if (pvh_et == PV_HASHED_ENTRY_NULL) {
				pvh_et = pvh_e;
			}
			pvh_cnt++;
		}
	} /* for loop */

	if (pvh_eh != PV_HASHED_ENTRY_NULL) {
		PV_HASHED_FREE_LIST(pvh_eh, pvh_et, pvh_cnt);
	}
update_counts:
	/*
	 *	Update the counts
	 */
#if TESTING
	if (pmap->stats.resident_count < num_removed)
	        panic("pmap_remove_range: resident_count");
#endif
	assert(pmap->stats.resident_count >= num_removed);
	OSAddAtomic(-num_removed,  &pmap->stats.resident_count);

#if TESTING
	if (pmap->stats.wired_count < num_unwired)
	        panic("pmap_remove_range: wired_count");
#endif
	assert(pmap->stats.wired_count >= num_unwired);
	OSAddAtomic(-num_unwired,  &pmap->stats.wired_count);

	return;
}


/*
 *	Remove the given range of addresses
 *	from the specified map.
 *
 *	It is assumed that the start and end are properly
 *	rounded to the hardware page size.
 */
void
pmap_remove(
	pmap_t		map,
	addr64_t	s64,
	addr64_t	e64)
{
	pt_entry_t     *pde;
	pt_entry_t     *spte, *epte;
	addr64_t        l64;
	uint64_t        deadline;

	pmap_intr_assert();

	if (map == PMAP_NULL || s64 == e64)
		return;

	PMAP_TRACE(PMAP_CODE(PMAP__REMOVE) | DBG_FUNC_START,
		   map,
		   (uint32_t) (s64 >> 32), s64,
		   (uint32_t) (e64 >> 32), e64);


	PMAP_LOCK(map);

#if 0
	/*
	 * Check that address range in the kernel does not overlap the stacks.
	 * We initialize local static min/max variables once to avoid making
	 * 2 function calls for every remove. Note also that these functions
	 * both return 0 before kernel stacks have been initialized, and hence
	 * the panic is not triggered in this case.
	 */
	if (map == kernel_pmap) {
		static vm_offset_t kernel_stack_min = 0;
		static vm_offset_t kernel_stack_max = 0;

		if (kernel_stack_min == 0) {
			kernel_stack_min = min_valid_stack_address();
			kernel_stack_max = max_valid_stack_address();
		}
		if ((kernel_stack_min <= s64 && s64 < kernel_stack_max) ||
		    (kernel_stack_min < e64 && e64 <= kernel_stack_max))
			panic("pmap_remove() attempted in kernel stack");
	}
#else

	/*
	 * The values of kernel_stack_min and kernel_stack_max are no longer
	 * relevant now that we allocate kernel stacks in the kernel map,
	 * so the old code above no longer applies.  If we wanted to check that
	 * we weren't removing a mapping of a page in a kernel stack we'd 
	 * mark the PTE with an unused bit and check that here.
	 */

#endif

	deadline = rdtsc64() + max_preemption_latency_tsc;

	while (s64 < e64) {
		l64 = (s64 + pde_mapped_size) & ~(pde_mapped_size - 1);
		if (l64 > e64)
			l64 = e64;
		pde = pmap_pde(map, s64);

		if (pde && (*pde & INTEL_PTE_VALID)) {
			if (*pde & INTEL_PTE_PS) {
				/*
				 * If we're removing a superpage, pmap_remove_range()
				 * must work on level 2 instead of level 1; and we're
				 * only passing a single level 2 entry instead of a
				 * level 1 range.
				 */
				spte = pde;
				epte = spte+1; /* excluded */
			} else {
				spte = pmap_pte(map, (s64 & ~(pde_mapped_size - 1)));
				spte = &spte[ptenum(s64)];
				epte = &spte[intel_btop(l64 - s64)];
			}
			pmap_remove_range(map, s64, spte, epte);
		}
		s64 = l64;

		if (s64 < e64 && rdtsc64() >= deadline) {
			PMAP_UNLOCK(map)
			PMAP_LOCK(map)
			deadline = rdtsc64() + max_preemption_latency_tsc;
		}
	}

	PMAP_UNLOCK(map);

	PMAP_TRACE(PMAP_CODE(PMAP__REMOVE) | DBG_FUNC_END,
		   map, 0, 0, 0, 0);

}

/*
 *	Routine:	pmap_page_protect
 *
 *	Function:
 *		Lower the permission for all mappings to a given
 *		page.
 */
void
pmap_page_protect(
        ppnum_t         pn,
	vm_prot_t	prot)
{
	pv_hashed_entry_t	pvh_eh = PV_HASHED_ENTRY_NULL;
	pv_hashed_entry_t	pvh_et = PV_HASHED_ENTRY_NULL;
	pv_hashed_entry_t	nexth;
	int			pvh_cnt = 0;
	pv_rooted_entry_t	pv_h;
	pv_rooted_entry_t	pv_e;
	pv_hashed_entry_t	pvh_e;
	pt_entry_t		*pte;
	int			pai;
	pmap_t			pmap;
	boolean_t		remove;

	pmap_intr_assert();
	assert(pn != vm_page_fictitious_addr);
	if (pn == vm_page_guard_addr)
		return;

	pai = ppn_to_pai(pn);

	if (!IS_MANAGED_PAGE(pai)) {
		/*
	         *	Not a managed page.
	         */
		return;
	}
	PMAP_TRACE(PMAP_CODE(PMAP__PAGE_PROTECT) | DBG_FUNC_START,
		   pn, prot, 0, 0, 0);

	/*
	 * Determine the new protection.
	 */
	switch (prot) {
	case VM_PROT_READ:
	case VM_PROT_READ | VM_PROT_EXECUTE:
		remove = FALSE;
		break;
	case VM_PROT_ALL:
		return;		/* nothing to do */
	default:
		remove = TRUE;
		break;
	}

	pv_h = pai_to_pvh(pai);

	LOCK_PVH(pai);


	/*
	 * Walk down PV list, if any, changing or removing all mappings.
	 */
	if (pv_h->pmap == PMAP_NULL)
		goto done;

	pv_e = pv_h;
	pvh_e = (pv_hashed_entry_t) pv_e;	/* cheat */

	do {
		vm_map_offset_t vaddr;

		pmap = pv_e->pmap;
		vaddr = pv_e->va;
		pte = pmap_pte(pmap, vaddr);

#if	DEBUG
		if (pa_index(pte_to_pa(*pte)) != pn)
			panic("pmap_page_protect: PTE mismatch, pn: 0x%x, pmap: %p, vaddr: 0x%llx, pte: 0x%llx", pn, pmap, vaddr, *pte);
#endif
		if (0 == pte) {
			panic("pmap_page_protect() "
				"pmap=%p pn=0x%x vaddr=0x%llx\n",
				pmap, pn, vaddr);
		}
		nexth = (pv_hashed_entry_t) queue_next(&pvh_e->qlink);

		/*
		 * Remove the mapping if new protection is NONE
		 * or if write-protecting a kernel mapping.
		 */
		if (remove || pmap == kernel_pmap) {
			/*
		         * Remove the mapping, collecting dirty bits.
		         */
			pmap_update_pte(pte, *pte, *pte & ~INTEL_PTE_VALID);
			PMAP_UPDATE_TLBS(pmap, vaddr, vaddr+PAGE_SIZE);
			pmap_phys_attributes[pai] |=
				*pte & (PHYS_MODIFIED|PHYS_REFERENCED);
			pmap_store_pte(pte, 0);

#if TESTING
			if (pmap->stats.resident_count < 1)
				panic("pmap_page_protect: resident_count");
#endif
			assert(pmap->stats.resident_count >= 1);
			OSAddAtomic(-1,  &pmap->stats.resident_count);

			/*
		         * Deal with the pv_rooted_entry.
		         */

			if (pv_e == pv_h) {
				/*
				 * Fix up head later.
				 */
				pv_h->pmap = PMAP_NULL;

				pmap_phys_attributes[pai] &= ~PHYS_NOENCRYPT;
			} else {
				/*
				 * Delete this entry.
				 */
				pv_hash_remove(pvh_e);
				pvh_e->qlink.next = (queue_entry_t) pvh_eh;
				pvh_eh = pvh_e;

				if (pvh_et == PV_HASHED_ENTRY_NULL)
					pvh_et = pvh_e;
				pvh_cnt++;
			}
		} else {
			/*
		         * Write-protect.
		         */
			pmap_update_pte(pte, *pte, *pte & ~INTEL_PTE_WRITE);
			PMAP_UPDATE_TLBS(pmap, vaddr, vaddr+PAGE_SIZE);
		}
		pvh_e = nexth;
	} while ((pv_e = (pv_rooted_entry_t) nexth) != pv_h);


	/*
	 * If pv_head mapping was removed, fix it up.
	 */
	if (pv_h->pmap == PMAP_NULL) {
		pvh_e = (pv_hashed_entry_t) queue_next(&pv_h->qlink);

		if (pvh_e != (pv_hashed_entry_t) pv_h) {
			pv_hash_remove(pvh_e);
			pv_h->pmap = pvh_e->pmap;
			pv_h->va = pvh_e->va;
			pvh_e->qlink.next = (queue_entry_t) pvh_eh;
			pvh_eh = pvh_e;

			if (pvh_et == PV_HASHED_ENTRY_NULL)
				pvh_et = pvh_e;
			pvh_cnt++;
		}
	}
	if (pvh_eh != PV_HASHED_ENTRY_NULL) {
		PV_HASHED_FREE_LIST(pvh_eh, pvh_et, pvh_cnt);
	}
done:
	UNLOCK_PVH(pai);

	PMAP_TRACE(PMAP_CODE(PMAP__PAGE_PROTECT) | DBG_FUNC_END,
		   0, 0, 0, 0, 0);
}

__private_extern__ void
pmap_pagetable_corruption_msg_log(int (*log_func)(const char * fmt, ...)__printflike(1,2)) {
	if (pmap_pagetable_corruption_incidents > 0) {
		int i, e = MIN(pmap_pagetable_corruption_incidents, PMAP_PAGETABLE_CORRUPTION_MAX_LOG);
		(*log_func)("%u pagetable corruption incident(s) detected, timeout: %u\n", pmap_pagetable_corruption_incidents, pmap_pagetable_corruption_timeout);
		for (i = 0; i < e; i++) {
			(*log_func)("Incident 0x%x, reason: 0x%x, action: 0x%x, time: 0x%llx\n", pmap_pagetable_corruption_records[i].incident,  pmap_pagetable_corruption_records[i].reason, pmap_pagetable_corruption_records[i].action, pmap_pagetable_corruption_records[i].abstime);
		}
	}
}

void
mapping_free_prime(void)
{
	int			i;
	pv_hashed_entry_t	pvh_e;
	pv_hashed_entry_t	pvh_eh;
	pv_hashed_entry_t	pvh_et;
	int			pv_cnt;

	pv_cnt = 0;
	pvh_eh = pvh_et = PV_HASHED_ENTRY_NULL;
	for (i = 0; i < (5 * PV_HASHED_ALLOC_CHUNK); i++) {
		pvh_e = (pv_hashed_entry_t) zalloc(pv_hashed_list_zone);

		pvh_e->qlink.next = (queue_entry_t)pvh_eh;
		pvh_eh = pvh_e;

		if (pvh_et == PV_HASHED_ENTRY_NULL)
		        pvh_et = pvh_e;
		pv_cnt++;
	}
	PV_HASHED_FREE_LIST(pvh_eh, pvh_et, pv_cnt);

	pv_cnt = 0;
	pvh_eh = pvh_et = PV_HASHED_ENTRY_NULL;
	for (i = 0; i < PV_HASHED_KERN_ALLOC_CHUNK; i++) {
		pvh_e = (pv_hashed_entry_t) zalloc(pv_hashed_list_zone);

		pvh_e->qlink.next = (queue_entry_t)pvh_eh;
		pvh_eh = pvh_e;

		if (pvh_et == PV_HASHED_ENTRY_NULL)
		        pvh_et = pvh_e;
		pv_cnt++;
	}
	PV_HASHED_KERN_FREE_LIST(pvh_eh, pvh_et, pv_cnt);

}

static inline void
pmap_pagetable_corruption_log_setup(void) {
	if (pmap_pagetable_corruption_log_call == NULL) {
		nanotime_to_absolutetime(PMAP_PAGETABLE_CORRUPTION_INTERVAL, 0, &pmap_pagetable_corruption_interval_abstime);
		thread_call_setup(&pmap_pagetable_corruption_log_call_data,
		    (thread_call_func_t) pmap_pagetable_corruption_msg_log,
		    (thread_call_param_t) &printf);
		pmap_pagetable_corruption_log_call = &pmap_pagetable_corruption_log_call_data;
	}
}

void
mapping_adjust(void)
{
	pv_hashed_entry_t	pvh_e;
	pv_hashed_entry_t	pvh_eh;
	pv_hashed_entry_t	pvh_et;
	int			pv_cnt;
	int             	i;

	if (mapping_adjust_call == NULL) {
		thread_call_setup(&mapping_adjust_call_data,
				  (thread_call_func_t) mapping_adjust,
				  (thread_call_param_t) NULL);
		mapping_adjust_call = &mapping_adjust_call_data;
	}

	pmap_pagetable_corruption_log_setup();

	pv_cnt = 0;
	pvh_eh = pvh_et = PV_HASHED_ENTRY_NULL;
	if (pv_hashed_kern_free_count < PV_HASHED_KERN_LOW_WATER_MARK) {
		for (i = 0; i < PV_HASHED_KERN_ALLOC_CHUNK; i++) {
			pvh_e = (pv_hashed_entry_t) zalloc(pv_hashed_list_zone);

			pvh_e->qlink.next = (queue_entry_t)pvh_eh;
			pvh_eh = pvh_e;

			if (pvh_et == PV_HASHED_ENTRY_NULL)
			        pvh_et = pvh_e;
			pv_cnt++;
		}
		PV_HASHED_KERN_FREE_LIST(pvh_eh, pvh_et, pv_cnt);
	}

	pv_cnt = 0;
	pvh_eh = pvh_et = PV_HASHED_ENTRY_NULL;
	if (pv_hashed_free_count < PV_HASHED_LOW_WATER_MARK) {
		for (i = 0; i < PV_HASHED_ALLOC_CHUNK; i++) {
			pvh_e = (pv_hashed_entry_t) zalloc(pv_hashed_list_zone);

			pvh_e->qlink.next = (queue_entry_t)pvh_eh;
			pvh_eh = pvh_e;

			if (pvh_et == PV_HASHED_ENTRY_NULL)
			        pvh_et = pvh_e;
			pv_cnt++;
		}
		PV_HASHED_FREE_LIST(pvh_eh, pvh_et, pv_cnt);
	}
	mappingrecurse = 0;
}


boolean_t
pmap_is_noencrypt(ppnum_t pn)
{
	int		pai;

	pai = ppn_to_pai(pn);

	if (!IS_MANAGED_PAGE(pai))
		return (TRUE);

	if (pmap_phys_attributes[pai] & PHYS_NOENCRYPT)
		return (TRUE);

	return (FALSE);
}


void
pmap_set_noencrypt(ppnum_t pn)
{
	int		pai;

	pai = ppn_to_pai(pn);

	if (IS_MANAGED_PAGE(pai)) {
		LOCK_PVH(pai);

		pmap_phys_attributes[pai] |= PHYS_NOENCRYPT;

		UNLOCK_PVH(pai);
	}
}


void
pmap_clear_noencrypt(ppnum_t pn)
{
	int		pai;

	pai = ppn_to_pai(pn);

	if (IS_MANAGED_PAGE(pai)) {
		LOCK_PVH(pai);

		pmap_phys_attributes[pai] &= ~PHYS_NOENCRYPT;

		UNLOCK_PVH(pai);
	}
}


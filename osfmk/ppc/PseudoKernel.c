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
 	File:		PseudoKernel.c

 	Contains:	BlueBox PseudoKernel calls
	Written by:	Mark Gorlinsky
				Bill Angell

 	Copyright:	1997 by Apple Computer, Inc., all rights reserved

*/

#include <mach/mach_types.h>
#include <mach/kern_return.h>
#include <kern/host.h>
#include <kern/task.h>
#include <kern/thread.h>
#include <ppc/PseudoKernel.h>
#include <ppc/exception.h>
#include <ppc/misc_protos.h>
#include <ppc/proc_reg.h>
#include <vm/vm_kern.h>

void bbSetRupt(ReturnHandler *rh, thread_act_t ct);

/*
** Function:	NotifyInterruption
**
** Inputs:
**		ppcInterrupHandler	- interrupt handler to execute
**		interruptStatePtr	- current interrupt state
**
** Outputs:
**
** Notes:
**
*/
kern_return_t syscall_notify_interrupt ( void ) {
  
    UInt32			interruptState; 
    task_t			task;
	thread_act_t 	act, fact;
	thread_t		thread;
	bbRupt			*bbr;
	BTTD_t			*bttd;
	int				i;

	task = current_task();							/* Figure out who our task is */

	task_lock(task);						/* Lock our task */
	
	fact = (thread_act_t)task->threads.next;		/* Get the first activation on task */
	act = 0;										/* Pretend we didn't find it yet */
	
	for(i = 0; i < task->thread_count; i++) {		/* Scan the whole list */
		if(fact->mact.bbDescAddr) {					/* Is this a Blue thread? */
			bttd = (BTTD_t *)(fact->mact.bbDescAddr & -PAGE_SIZE);
			if(bttd->InterruptVector) {				/* Is this the Blue interrupt thread? */
				act = fact;							/* Yeah... */
				break;								/* Found it, Bail the loop... */
			}
		}
		fact = (thread_act_t)fact->task_threads.next;	/* Go to the next one */
	}

	if(!act) {								/* Couldn't find a bluebox */
		task_unlock(task);					/* Release task lock */
		return KERN_FAILURE;				/* No tickie, no shirtee... */
	}
	
	act_lock_thread(act);							/* Make sure this stays 'round */
	task_unlock(task);								/* Safe to release now */

	/* if the calling thread is the BlueBox thread that handles interrupts
	 * we know that we are in the PsuedoKernel and we can short circuit 
	 * setting up the asynchronous task by setting a pending interrupt.
	 */
	
	if ( (unsigned int)act == (unsigned int)current_act() ) {		
		bttd->InterruptControlWord = bttd->InterruptControlWord | 
			((bttd->postIntMask >> kCR2ToBackupShift) & kBackupCR2Mask);
				
		act_unlock_thread(act);						/* Unlock the activation */
		return KERN_SUCCESS;
	}

	if(act->mact.emPendRupts >= 16) {				/* Have we hit the arbitrary maximum? */
		act_unlock_thread(act);						/* Unlock the activation */
		return KERN_RESOURCE_SHORTAGE;				/* Too many pending right now */
	}
	
	if(!(bbr = (bbRupt *)kalloc(sizeof(bbRupt)))) {	/* Get a return handler control block */
		act_unlock_thread(act);						/* Unlock the activation */
		return KERN_RESOURCE_SHORTAGE;				/* No storage... */
	}
	
	(void)hw_atomic_add(&act->mact.emPendRupts, 1);	/* Count this 'rupt */
	bbr->rh.handler = bbSetRupt;					/* Set interruption routine */

	bbr->rh.next = act->handlers;					/* Put our interrupt at the start of the list */
	act->handlers = &bbr->rh;

	act_set_apc(act);								/* Set an APC AST */

	act_unlock_thread(act);							/* Unlock the activation */
	return KERN_SUCCESS;							/* We're done... */
}

/* 
 *	This guy is fired off asynchronously to actually do the 'rupt.
 *	We will find the user state savearea and modify it.  If we can't,
 *	we just leave after releasing our work area
 */

void bbSetRupt(ReturnHandler *rh, thread_act_t act) {

	savearea 	*sv;
	BTTD_t		*bttd;
	bbRupt		*bbr;
	UInt32		interruptState;
	
	bbr = (bbRupt *)rh;								/* Make our area convenient */

	if(!(act->mact.bbDescAddr)) {					/* Is BlueBox still enabled? */
		kfree((vm_offset_t)bbr, sizeof(bbRupt));	/* No, release the control block */
		return;
	}

	(void)hw_atomic_sub(&act->mact.emPendRupts, 1);	/* Uncount this 'rupt */

	if(!(sv = find_user_regs(act))) {				/* Find the user state registers */
		kfree((vm_offset_t)bbr, sizeof(bbRupt));	/* Couldn't find 'em, release the control block */
		return;
	}

	bttd = (BTTD_t *)(act->mact.bbDescAddr & -PAGE_SIZE);
		
    interruptState = (bttd->InterruptControlWord & kInterruptStateMask) >> kInterruptStateShift; 

    switch (interruptState) {
		
		case kInSystemContext:
			sv->save_cr |= bttd->postIntMask;		/* post int in CR2 */
			break;
			
		case kInAlternateContext:
			bttd->InterruptControlWord = (bttd->InterruptControlWord & ~kInterruptStateMask) | 
				(kInPseudoKernel << kInterruptStateShift);
				
			bttd->exceptionInfo.srr0 = (unsigned int)sv->save_srr0;		/* Save the current PC */
			sv->save_srr0 = (uint64_t)act->mact.bbInterrupt;	/* Set the new PC */
			bttd->exceptionInfo.sprg1 = (unsigned int)sv->save_r1;		/* Save the original R1 */
			sv->save_r1 = (uint64_t)bttd->exceptionInfo.sprg0;	/* Set the new R1 */
			bttd->exceptionInfo.srr1 = (unsigned int)sv->save_srr1;		/* Save the original MSR */
			sv->save_srr1 &= ~(MASK(MSR_BE)|MASK(MSR_SE));	/* Clear SE|BE bits in MSR */
			act->mact.specFlags &= ~bbNoMachSC;				/* reactivate Mach SCs */ 
			disable_preemption();							/* Don't move us around */
			per_proc_info[cpu_number()].spcFlags = act->mact.specFlags;	/* Copy the flags */
			enable_preemption();							/* Ok to move us around */
			/* drop through to post int in backup CR2 in ICW */

		case kInExceptionHandler:
		case kInPseudoKernel:
		case kOutsideBlue:
			bttd->InterruptControlWord = bttd->InterruptControlWord | 
				((bttd->postIntMask >> kCR2ToBackupShift) & kBackupCR2Mask);
			break;
				
		default:
			break;
	}

	kfree((vm_offset_t)bbr, sizeof(bbRupt));	/* Release the control block */
	return;

}

/*
 * This function is used to enable the firmware assist code for bluebox traps, system calls
 * and interrupts.
 *
 * The assist code can be called from two types of threads.  The blue thread, which handles 
 * traps, system calls and interrupts and preemptive threads that only issue system calls.
 *
 */ 

kern_return_t enable_bluebox(
      host_t host,
	  void *taskID,								/* opaque task ID */
	  void *TWI_TableStart,						/* Start of TWI table */
	  char *Desc_TableStart						/* Start of descriptor table */
	 ) {
	
	thread_t 		th;
	vm_offset_t		kerndescaddr, origdescoffset;
	kern_return_t 	ret;
	ppnum_t			physdescpage;
	BTTD_t			*bttd;
	
	th = current_thread();									/* Get our thread */					

	if ( host == HOST_NULL ) return KERN_INVALID_HOST;
	if ( ! is_suser() ) return KERN_FAILURE;						/* We will only do this for the superuser */
	if ( th->top_act->mact.bbDescAddr ) return KERN_FAILURE;		/* Bail if already authorized... */
	if ( ! (unsigned int) Desc_TableStart ) return KERN_FAILURE;	/* There has to be a descriptor page */ 
	if ( ! TWI_TableStart ) return KERN_FAILURE;					/* There has to be a TWI table */ 

	/* Get the page offset of the descriptor */
	origdescoffset = (vm_offset_t)Desc_TableStart & (PAGE_SIZE - 1);

	/* Align the descriptor to a page */
	Desc_TableStart = (char *)((vm_offset_t)Desc_TableStart & -PAGE_SIZE);

	ret = vm_map_wire(th->top_act->map, 					/* Kernel wire the descriptor in the user's map */
		(vm_offset_t)Desc_TableStart,
		(vm_offset_t)Desc_TableStart + PAGE_SIZE,
		VM_PROT_READ | VM_PROT_WRITE,
		FALSE);															
		
	if(ret != KERN_SUCCESS) {								/* Couldn't wire it, spit on 'em... */
		return KERN_FAILURE;	
	}
		
	physdescpage = 											/* Get the physical page number of the page */
		pmap_find_phys(th->top_act->map->pmap, (addr64_t)Desc_TableStart);

	ret =  kmem_alloc_pageable(kernel_map, &kerndescaddr, PAGE_SIZE);	/* Find a virtual address to use */
	if(ret != KERN_SUCCESS) {								/* Could we get an address? */
		(void) vm_map_unwire(th->top_act->map,				/* No, unwire the descriptor */
			(vm_offset_t)Desc_TableStart,
			(vm_offset_t)Desc_TableStart + PAGE_SIZE,
			TRUE);
		return KERN_FAILURE;								/* Split... */
	}
	
	(void) pmap_enter(kernel_pmap, 							/* Map this into the kernel */
		kerndescaddr, physdescpage, VM_PROT_READ|VM_PROT_WRITE, 
		VM_WIMG_USE_DEFAULT, TRUE);
	
	bttd = (BTTD_t *)kerndescaddr;							/* Get the address in a convienient spot */ 
	
	th->top_act->mact.bbDescAddr = (unsigned int)kerndescaddr+origdescoffset;	/* Set kernel address of the table */
	th->top_act->mact.bbUserDA = (unsigned int)Desc_TableStart;	/* Set user address of the table */
	th->top_act->mact.bbTableStart = (unsigned int)TWI_TableStart;	/* Set address of the trap table */
	th->top_act->mact.bbTaskID = (unsigned int)taskID;		/* Assign opaque task ID */
	th->top_act->mact.bbTaskEnv = 0;						/* Clean task environment data */
	th->top_act->mact.emPendRupts = 0;						/* Clean pending 'rupt count */
	th->top_act->mact.bbTrap = bttd->TrapVector;			/* Remember trap vector */
	th->top_act->mact.bbSysCall = bttd->SysCallVector;		/* Remember syscall vector */
	th->top_act->mact.bbInterrupt = bttd->InterruptVector;	/* Remember interrupt vector */
	th->top_act->mact.bbPending = bttd->PendingIntVector;	/* Remember pending vector */
	th->top_act->mact.specFlags &= ~(bbNoMachSC | bbPreemptive);	/* Make sure mach SCs are enabled and we are not marked preemptive */
	th->top_act->mact.specFlags |= bbThread;				/* Set that we are Classic thread */
		
	if(!(bttd->InterruptVector)) {							/* See if this is a preemptive (MP) BlueBox thread */
		th->top_act->mact.specFlags |= bbPreemptive;		/* Yes, remember it */
	}
		
	disable_preemption();									/* Don't move us around */
	per_proc_info[cpu_number()].spcFlags = th->top_act->mact.specFlags;	/* Copy the flags */
	enable_preemption();									/* Ok to move us around */
		
	{
		/* mark the proc to indicate that this is a TBE proc */
		extern void tbeproc(void *proc);

		tbeproc(th->top_act->task->bsd_info);
	}

	return KERN_SUCCESS;
}

kern_return_t disable_bluebox( host_t host ) {				/* User call to terminate bluebox */
	
	thread_act_t 	act;
	
	act = current_act();									/* Get our thread */					

	if (host == HOST_NULL) return KERN_INVALID_HOST;
	
	if(!is_suser()) return KERN_FAILURE;					/* We will only do this for the superuser */
	if(!act->mact.bbDescAddr) return KERN_FAILURE;			/* Bail if not authorized... */

	disable_bluebox_internal(act);							/* Clean it all up */
	return KERN_SUCCESS;									/* Leave */
}

void disable_bluebox_internal(thread_act_t act) {			/* Terminate bluebox */
		
	(void) vm_map_unwire(act->map,							/* Unwire the descriptor in user's address space */
		(vm_offset_t)act->mact.bbUserDA,
		(vm_offset_t)act->mact.bbUserDA + PAGE_SIZE,
		FALSE);
		
	kmem_free(kernel_map, (vm_offset_t)act->mact.bbDescAddr & -PAGE_SIZE, PAGE_SIZE);	/* Release the page */
	
	act->mact.bbDescAddr = 0;								/* Clear kernel pointer to it */
	act->mact.bbUserDA = 0;									/* Clear user pointer to it */
	act->mact.bbTableStart = 0;								/* Clear user pointer to TWI table */
	act->mact.bbTaskID = 0;									/* Clear opaque task ID */
	act->mact.bbTaskEnv = 0;								/* Clean task environment data */
	act->mact.emPendRupts = 0;								/* Clean pending 'rupt count */
	act->mact.specFlags &= ~(bbNoMachSC | bbPreemptive | bbThread);	/* Clean up Blue Box enables */
	disable_preemption();								/* Don't move us around */
	per_proc_info[cpu_number()].spcFlags = act->mact.specFlags;	/* Copy the flags */
	enable_preemption();								/* Ok to move us around */
	return;
}

/*
 * Use the new PPCcall method to enable blue box threads
 *
 *	save->r3 = taskID
 *	save->r4 = TWI_TableStart
 *	save->r5 = Desc_TableStart
 *
 */
int bb_enable_bluebox( struct savearea *save )
{
	kern_return_t rc;

	rc = enable_bluebox( (host_t)0xFFFFFFFF, (void *)save->save_r3, (void *)save->save_r4, (char *)save->save_r5 );
	save->save_r3 = rc;
	return 1;										/* Return with normal AST checking */
}

/*
 * Use the new PPCcall method to disable blue box threads
 *
 */
int bb_disable_bluebox( struct savearea *save )
{
	kern_return_t rc;

	rc = disable_bluebox( (host_t)0xFFFFFFFF );
	save->save_r3 = rc;
	return 1;										/* Return with normal AST checking */
}

/*
 * Search through the list of threads to find the matching taskIDs, then
 * set the task environment pointer.  A task in this case is a preemptive thread
 * in MacOS 9.
 *
 *	save->r3 = taskID
 *	save->r4 = taskEnv
 */

int bb_settaskenv( struct savearea *save )
{
	int				i;
    task_t			task;
	thread_act_t	act, fact;


	task = current_task();							/* Figure out who our task is */

	task_lock(task);								/* Lock our task */
	fact = (thread_act_t)task->threads.next;		/* Get the first activation on task */
	act = 0;										/* Pretend we didn't find it yet */
	
	for(i = 0; i < task->thread_count; i++) {		/* Scan the whole list */
		if(fact->mact.bbDescAddr) {					/* Is this a Blue thread? */
			if ( fact->mact.bbTaskID == save->save_r3 ) {	/* Is this the task we are looking for? */
				act = fact;							/* Yeah... */
				break;								/* Found it, Bail the loop... */
			}
		}
		fact = (thread_act_t)fact->task_threads.next;	/* Go to the next one */
	}

	if ( !act || !act->active) {
		task_unlock(task);							/* Release task lock */
		goto failure;
	}

	act_lock_thread(act);							/* Make sure this stays 'round */
	task_unlock(task);								/* Safe to release now */

	act->mact.bbTaskEnv = save->save_r4;
	if(act == current_act()) {						/* Are we setting our own? */
		disable_preemption();						/* Don't move us around */
		per_proc_info[cpu_number()].ppbbTaskEnv = act->mact.bbTaskEnv;	/* Remember the environment */
		enable_preemption();						/* Ok to move us around */
	}

	act_unlock_thread(act);							/* Unlock the activation */
	save->save_r3 = 0;
	return 1;

failure:
	save->save_r3 = -1;								/* we failed to find the taskID */
	return 1;
}

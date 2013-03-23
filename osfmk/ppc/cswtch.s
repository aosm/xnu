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

#include <ppc/asm.h>
#include <ppc/proc_reg.h>
#include <cpus.h>
#include <assym.s>
#include <debug.h>
#include <mach/ppc/vm_param.h>
#include <ppc/exception.h>
#include <ppc/savearea.h>

#define FPVECDBG 0
#define GDDBG 0

	.text
	
/*
 * void     load_context(thread_t        thread)
 *
 * Load the context for the first kernel thread, and go.
 *
 * NOTE - if DEBUG is set, the former routine is a piece
 * of C capable of printing out debug info before calling the latter,
 * otherwise both entry points are identical.
 */

			.align	5
			.globl	EXT(load_context)

LEXT(load_context)

			.globl	EXT(Load_context)

LEXT(Load_context)

/*
 * Since this is the first thread, we came in on the interrupt
 * stack. The first thread never returns, so there is no need to
 * worry about saving its frame, hence we can reset the istackptr
 * back to the saved_state structure at it's top
 */
			

/*
 * get new thread pointer and set it into the active_threads pointer
 *
 */
	
			mfsprg	r6,0
			lwz		r0,PP_INTSTACK_TOP_SS(r6)
			stw		r0,PP_ISTACKPTR(r6)
			stw		r3,PP_ACTIVE_THREAD(r6)

/* Find the new stack and store it in active_stacks */
	
			lwz		r12,PP_ACTIVE_STACKS(r6)
			lwz		r1,THREAD_KERNEL_STACK(r3)
			lwz		r9,THREAD_TOP_ACT(r3)			/* Point to the active activation */
			mtsprg	1,r9
			stw		r1,0(r12)
			li		r0,0							/* Clear a register */
			lwz		r3,ACT_MACT_PCB(r9)				/* Get the savearea used */
			mfmsr	r5								/* Since we are passing control, get our MSR values */
			lwz		r11,SAVprev+4(r3)				/* Get the previous savearea */
			lwz		r1,saver1+4(r3)					/* Load new stack pointer */
			stw		r0,saver3+4(r3)					/* Make sure we pass in a 0 for the continuation */
			stw		r0,FM_BACKPTR(r1)				/* zero backptr */
			stw		r5,savesrr1+4(r3)				/* Pass our MSR to the new guy */
			stw		r11,ACT_MACT_PCB(r9)			/* Unstack our savearea */
			b		EXT(exception_exit)				/* Go end it all... */
	
/* struct thread_shuttle *Switch_context(struct thread_shuttle   *old,
 * 				      	 void                    (*cont)(void),
 *				         struct thread_shuttle   *new)
 *
 * Switch from one thread to another. If a continuation is supplied, then
 * we do not need to save callee save registers.
 *
 */

/* void Call_continuation( void (*continuation)(void),  vm_offset_t stack_ptr)
 */

			.align	5
			.globl	EXT(Call_continuation)

LEXT(Call_continuation)

			mtlr	r3
			mr		r1, r4							/* Load new stack pointer */
			blr										/* Jump to the continuation */

/*
 * Get the old kernel stack, and store into the thread structure.
 * See if a continuation is supplied, and skip state save if so.
 *
 * Note that interrupts must be disabled before we get here (i.e., splsched)
 */

/* 			Context switches are double jumps.  We pass the following to the
 *			context switch firmware call:
 *
 *			R3  = switchee's savearea, virtual if continuation, low order physical for full switch
 *			R4  = old thread
 *			R5  = new SRR0
 *			R6  = new SRR1
 *			R7  = high order physical address of savearea for full switch
 *
 *			savesrr0 is set to go to switch_in
 *			savesrr1 is set to uninterruptible with translation on
 */


			.align	5
			.globl	EXT(Switch_context)

LEXT(Switch_context)

			lwz		r11,THREAD_KERNEL_STACK(r5)		; Get the new stack pointer
			mfsprg	r12,0							; Get the per_proc block
			lwz		r10,PP_ACTIVE_STACKS(r12)		; Get the pointer to the current stack
#if DEBUG
			lwz		r0,PP_ISTACKPTR(r12)			; (DEBUG/TRACE) make sure we are not
			mr.		r0,r0							; (DEBUG/TRACE) on the interrupt
			bne++	notonintstack					; (DEBUG/TRACE) stack
			BREAKPOINT_TRAP
notonintstack:
#endif	
		
#if 0
			lwz		r8,lgPPStart(0)					; (TEST/DEBUG) Get the start of per_procs
			sub		r7,r12,r8						; (TEST/DEBUG) Find offset to our per_proc
			xori	r7,r7,0x1000					; (TEST/DEBUG) Switch to other proc
			add		r8,r8,r7						; (TEST/DEBUG) Switch to it
			lwz		r8,PP_ACTIVE_THREAD(r8)			; (TEST/DEBUG) Get the other active thread
			cmplw	r8,r5							; (TEST/DEBUG) Trying to switch to an active thread?
			bne++	snively							; (TEST/DEBUG) Nope...
			BREAKPOINT_TRAP							; (TEST/DEBUG) Get to debugger...

snively:											; (TEST/DEBUG)
#endif

			stw		r5,PP_ACTIVE_THREAD(r12)		; Make the new thread current
 			lwz		r5,THREAD_TOP_ACT(r5)			; Get the new activation
			stw		r4,THREAD_CONTINUATION(r3)		; Set continuation into the thread
			lwz		r7,0(r10)						; Get the current stack
			cmpwi	cr1,r4,0						; Remeber if there is a continuation - used waaaay down below 
			stw		r11,0(r10)						; Save the new kernel stack address

			lwz		r8,ACT_MACT_PCB(r5)				; Get the PCB for the new guy
			lwz		r9,cioSpace(r5)					; Get copyin/out address space
			stw		r7,THREAD_KERNEL_STACK(r3)		; Remember the current stack in the thread (do not need???)
			mtsprg	1,r5							; Set the current activation pointer
			lwz		r7,CTHREAD_SELF(r5)				; Pick up the user assist word
			lwz		r11,ACT_MACT_BTE(r5)			; Get BlueBox Task Environment
			lwz		r6,cioRelo(r5)					; Get copyin/out relocation top
			lwz		r2,cioRelo+4(r5)				; Get copyin/out relocation bottom
			
			stw		r7,UAW(r12)						; Save the assist word for the "ultra fast path"

			lwz		r7,ACT_MACT_SPF(r5)				; Get the special flags
			
			lwz		r0,ACT_KLOADED(r5)
			sth		r9,ppCIOmp+mpSpace(r12)			; Save the space
			stw		r6,ppCIOmp+mpNestReloc(r12)		; Save top part of physical address
			stw		r2,ppCIOmp+mpNestReloc+4(r12)	; Save bottom part of physical address
			lwz		r10,PP_ACTIVE_KLOADED(r12)		; Get kernel loaded flag address
			subfic	r0,r0,0							; Get bit 0 to 0 if not kloaded, 1 otherwise
			lwz		r2,traceMask(0)					; Get the enabled traces
			stw		r11,ppbbTaskEnv(r12)			; Save the bb task env
			srawi	r0,r0,31						; Get 0 if not kloaded, ffffffff otherwise
			stw		r7,spcFlags(r12)				; Set per_proc copy of the special flags
			and		r0,r5,r0						; Get 0 if not kloaded, activation otherwise
		
			mr.		r2,r2							; Any tracing going on?
			stw		r0,0(r10)						; Set the kloaded stuff
			lis		r0,hi16(CutTrace)				; Trace FW call
			lwz		r11,SAVprev+4(r8)				; Get the previous of the switchee savearea 
			ori		r0,r0,lo16(CutTrace)			; Trace FW call
			mr		r10,r3							; Save across trace
			beq++	cswNoTrc						; No trace today, dude...
			lwz		r2,THREAD_TOP_ACT(r3)			; Trace old activation
			mr		r3,r11							; Trace prev savearea
			sc										; Cut trace entry of context switch
			mr		r3,r10							; Restore
			
cswNoTrc:	lwz		r2,curctx(r5)					; Grab our current context pointer
			lwz		r10,FPUowner(r12)				; Grab the owner of the FPU			
			lwz		r9,VMXowner(r12)				; Grab the owner of the vector
			lhz		r0,PP_CPU_NUMBER(r12)			; Get our CPU number
			mfmsr	r6								; Get the MSR because the switched to thread should inherit it 
			stw		r11,ACT_MACT_PCB(r5)			; Dequeue the savearea we are switching to
			li		r0,1							; Get set to hold off quickfret

			rlwinm	r6,r6,0,MSR_FP_BIT+1,MSR_FP_BIT-1	; Turn off the FP
			cmplw	r10,r2							; Do we have the live float context?
			lwz		r10,FPUlevel(r2)				; Get the live level
			mr		r4,r3							; Save our old thread to pass back 
			cmplw	cr5,r9,r2						; Do we have the live vector context?		
			rlwinm	r6,r6,0,MSR_VEC_BIT+1,MSR_VEC_BIT-1	; Turn off the vector
			stw		r0,holdQFret(r12)				; Make sure we hold off releasing quickfret
			bne++	cswnofloat						; Float is not ours...
			
			cmplw	r10,r11							; Is the level the same?
			lwz		r5,FPUcpu(r2)					; Get the owning cpu
			bne++	cswnofloat						; Level not the same, this is not live...
			
			cmplw	r5,r0							; Still owned by this cpu?
			lwz		r10,FPUsave(r2)					; Get the level
			bne++	cswnofloat						; CPU claimed by someone else...
			
			mr.		r10,r10							; Is there a savearea here?
			ori		r6,r6,lo16(MASK(MSR_FP))		; Enable floating point
			
			beq--	cswnofloat						; No savearea to check...
			
			lwz		r3,SAVlevel(r10)				; Get the level
			lwz		r5,SAVprev+4(r10)				; Get the previous of this savearea
			cmplw	r3,r11							; Is it for the current level?
			
			bne++	cswnofloat						; Nope...
			
			stw		r5,FPUsave(r2)					; Pop off this savearea

			rlwinm	r3,r10,0,0,19					; Move back to start of page

			lwz		r5,quickfret(r12)				; Get the first in quickfret list (top)					
			lwz		r9,quickfret+4(r12)				; Get the first in quickfret list (bottom)					
			lwz		r7,SACvrswap(r3)				; Get the virtual to real conversion (top)
			lwz		r3,SACvrswap+4(r3)				; Get the virtual to real conversion (bottom)
			stw		r5,SAVprev(r10)					; Link the old in (top)					
			stw		r9,SAVprev+4(r10)				; Link the old in (bottom)					
			xor		r3,r10,r3						; Convert to physical
			stw		r7,quickfret(r12)				; Set the first in quickfret list (top)					
			stw		r3,quickfret+4(r12)				; Set the first in quickfret list (bottom)					

#if FPVECDBG
			lis		r0,hi16(CutTrace)				; (TEST/DEBUG)
			mr		r7,r2							; (TEST/DEBUG)
			li		r2,0x4401						; (TEST/DEBUG)
			oris	r0,r0,lo16(CutTrace)			; (TEST/DEBUG)
			sc										; (TEST/DEBUG)
			lhz		r0,PP_CPU_NUMBER(r12)			; (TEST/DEBUG)
			mr		r2,r7							; (TEST/DEBUG) 
#endif	

cswnofloat:	bne++	cr5,cswnovect					; Vector is not ours...

			lwz		r10,VMXlevel(r2)				; Get the live level
			
			cmplw	r10,r11							; Is the level the same?
			lwz		r5,VMXcpu(r2)					; Get the owning cpu
			bne++	cswnovect						; Level not the same, this is not live...
			
			cmplw	r5,r0							; Still owned by this cpu?
			lwz		r10,VMXsave(r2)					; Get the level
			bne++	cswnovect						; CPU claimed by someone else...
			
			mr.		r10,r10							; Is there a savearea here?
			oris	r6,r6,hi16(MASK(MSR_VEC))		; Enable vector
			
			beq--	cswnovect						; No savearea to check...
			
			lwz		r3,SAVlevel(r10)				; Get the level
			lwz		r5,SAVprev+4(r10)				; Get the previous of this savearea
			cmplw	r3,r11							; Is it for the current level?
			
			bne++	cswnovect						; Nope...
			
			stw		r5,VMXsave(r2)					; Pop off this savearea
			rlwinm	r3,r10,0,0,19					; Move back to start of page

			lwz		r5,quickfret(r12)				; Get the first in quickfret list (top)					
			lwz		r9,quickfret+4(r12)				; Get the first in quickfret list (bottom)					
			lwz		r2,SACvrswap(r3)				; Get the virtual to real conversion (top)
			lwz		r3,SACvrswap+4(r3)				; Get the virtual to real conversion (bottom)
			stw		r5,SAVprev(r10)					; Link the old in (top)					
			stw		r9,SAVprev+4(r10)				; Link the old in (bottom)					
			xor		r3,r10,r3						; Convert to physical
			stw		r2,quickfret(r12)				; Set the first in quickfret list (top)					
			stw		r3,quickfret+4(r12)				; Set the first in quickfret list (bottom)					

#if FPVECDBG
			lis		r0,hi16(CutTrace)				; (TEST/DEBUG)
			li		r2,0x4501						; (TEST/DEBUG)
			oris	r0,r0,lo16(CutTrace)			; (TEST/DEBUG)
			sc										; (TEST/DEBUG)
#endif	

cswnovect:	li		r0,0							; Get set to release quickfret holdoff	
			rlwinm	r11,r8,0,0,19					; Switch to savearea base
			lis		r9,hi16(EXT(switch_in))			; Get top of switch in routine 
			lwz		r5,savesrr0+4(r8)				; Set up the new SRR0
			lwz		r7,SACvrswap(r11)				; Get the high order V to R translation
			lwz		r11,SACvrswap+4(r11)			; Get the low order V to R translation
			ori		r9,r9,lo16(EXT(switch_in))		; Bottom half of switch in 
			stw		r0,holdQFret(r12)				; Make sure we release quickfret holdoff
			stw		r9,savesrr0+4(r8)				; Make us jump to the switch in routine 

			lwz		r9,SAVflags(r8)					/* Get the flags */
			lis		r0,hi16(SwitchContextCall)		/* Top part of switch context */
			li		r10,MSR_SUPERVISOR_INT_OFF		/* Get the switcher's MSR */
			ori		r0,r0,lo16(SwitchContextCall)	/* Bottom part of switch context */
			stw		r10,savesrr1+4(r8)				/* Set up for switch in */
			rlwinm	r9,r9,0,15,13					/* Reset the syscall flag */
			xor		r3,r11,r8						/* Get the physical address of the new context save area */
			stw		r9,SAVflags(r8)					/* Set the flags */

			bne		cr1,swtchtocont					; Switch to the continuation
			sc										/* Switch to the new context */
	
/*			We come back here in the new thread context	
 * 			R4 was set to hold the old thread pointer, but switch_in will put it into
 *			R3 where it belongs.
 */
			blr										/* Jump into the new thread */

;
;			This is where we go when a continuation is set.  We are actually
;			killing off the old context of the new guy so we need to pop off
;			any float or vector states for the ditched level.
;
;			Note that we do the same kind of thing a chkfac in hw_exceptions.s
;

		
swtchtocont:

			stw		r5,savesrr0+4(r8)				; Set the pc
			stw		r6,savesrr1+4(r8)				; Set the next MSR to use
			stw		r4,saver3+4(r8)					; Make sure we pass back the old thread
			mr		r3,r8							; Pass in the virtual address of savearea
			
			b		EXT(exception_exit)				; Blocking on continuation, toss old context...



/*
 *			All switched to threads come here first to clean up the old thread.
 *			We need to do the following contortions because we need to keep
 *			the LR clean. And because we need to manipulate the savearea chain
 *			with translation on.  If we could, this should be done in lowmem_vectors
 *			before translation is turned on.  But we can't, dang it!
 *
 *			R3  = switcher's savearea (32-bit virtual)
 *			saver4  = old thread in switcher's save
 *			saver5  = new SRR0 in switcher's save
 *			saver6  = new SRR1 in switcher's save


 */
 

			.align	5
			.globl	EXT(switch_in)

LEXT(switch_in)

			lwz		r4,saver4+4(r3)					; Get the old thread 
			lwz		r5,saver5+4(r3)					; Get the srr0 value 
			
	 		mfsprg	r0,2							; Get feature flags 
			lwz		r9,THREAD_TOP_ACT(r4)			; Get the switched from ACT
			lwz		r6,saver6+4(r3)					; Get the srr1 value 
			rlwinm.	r0,r0,0,pf64Bitb,pf64Bitb		; Check for 64-bit
			lwz		r10,ACT_MACT_PCB(r9)			; Get the top PCB on the old thread 

			stw		r3,ACT_MACT_PCB(r9)				; Put the new one on top
			stw		r10,SAVprev+4(r3)				; Chain on the old one

			mr		r3,r4							; Pass back the old thread 

			mtsrr0	r5								; Set return point
			mtsrr1	r6								; Set return MSR
			
			bne++	siSixtyFour						; Go do 64-bit...

			rfi										; Jam...
			
siSixtyFour:
			rfid									; Jam...
			
/*
 * void fpu_save(facility_context ctx)
 *
 *			Note that there are some oddities here when we save a context we are using.
 *			It is really not too cool to do this, but what the hey...  Anyway, 
 *			we turn fpus and vecs off before we leave., The oddity is that if you use fpus after this, the
 *			savearea containing the context just saved will go away.  So, bottom line is
 *			that don't use fpus until after you are done with the saved context.
 */
			.align	5
			.globl	EXT(fpu_save)

LEXT(fpu_save)
			
			lis		r2,hi16(MASK(MSR_VEC))			; Get the vector enable
			li		r12,lo16(MASK(MSR_EE))			; Get the EE bit
			ori		r2,r2,lo16(MASK(MSR_FP))		; Get FP

			mfmsr	r0								; Get the MSR
			andc	r0,r0,r2						; Clear FP, VEC
			andc	r2,r0,r12						; Clear EE
			ori		r2,r2,MASK(MSR_FP)				; Enable the floating point feature for now also
			mtmsr	r2								; Set the MSR
			isync

			mfsprg	r6,0							; Get the per_processor block 
			lwz		r12,FPUowner(r6)				; Get the context ID for owner

#if FPVECDBG
			mr		r7,r0							; (TEST/DEBUG)
			li		r4,0							; (TEST/DEBUG)
			mr		r10,r3							; (TEST/DEBUG)
			lis		r0,hi16(CutTrace)				; (TEST/DEBUG)
			mr.		r3,r12							; (TEST/DEBUG)
			li		r2,0x6F00						; (TEST/DEBUG)
			li		r5,0							; (TEST/DEBUG)
			beq--	noowneryet						; (TEST/DEBUG)
			lwz		r4,FPUlevel(r12)				; (TEST/DEBUG)
			lwz		r5,FPUsave(r12)					; (TEST/DEBUG)

noowneryet:	oris	r0,r0,lo16(CutTrace)			; (TEST/DEBUG)
			sc										; (TEST/DEBUG)
			mr		r0,r7							; (TEST/DEBUG)
			mr		r3,r10							; (TEST/DEBUG)
#endif	
			mflr	r2								; Save the return address

fsretry:	mr.		r12,r12							; Anyone own the FPU?
			lhz		r11,PP_CPU_NUMBER(r6)			; Get our CPU number
			beq--	fsret							; Nobody owns the FPU, no save required...
			
			cmplw	cr1,r3,r12						; Is the specified context live?
			
			isync									; Force owner check first
			
			lwz		r9,FPUcpu(r12)					; Get the cpu that context was last on		
			bne--	cr1,fsret						; No, it is not...
			
			cmplw	cr1,r9,r11						; Was the context for this processor? 
			beq--	cr1,fsgoodcpu					; Facility last used on this processor...

			b		fsret							; Someone else claimed it...
			
			.align	5
			
fsgoodcpu:	lwz		r3,FPUsave(r12)					; Get the current FPU savearea for the thread
			lwz		r9,FPUlevel(r12)				; Get our current level indicator
			
			cmplwi	cr1,r3,0						; Have we ever saved this facility context?
			beq-	cr1,fsneedone					; Never saved it, so go do it...
			
			lwz		r8,SAVlevel(r3)					; Get the level this savearea is for
			cmplw	cr1,r9,r8						; Correct level?
			beq--	cr1,fsret						; The current level is already saved, bail out...

fsneedone:	bl		EXT(save_get)					; Get a savearea for the context

			mfsprg	r6,0							; Get back per_processor block
			li		r4,SAVfloat						; Get floating point tag			
			lwz		r12,FPUowner(r6)				; Get back our thread
			stb		r4,SAVflags+2(r3)				; Mark this savearea as a float
			mr.		r12,r12							; See if we were disowned while away. Very, very small chance of it...
			beq--	fsbackout						; If disowned, just toss savearea...
			lwz		r4,facAct(r12)					; Get the activation associated with live context
			lwz		r8,FPUsave(r12)					; Get the current top floating point savearea
			stw		r4,SAVact(r3)					; Indicate the right activation for this context
			lwz		r9,FPUlevel(r12)				; Get our current level indicator again		
			stw		r3,FPUsave(r12)					; Set this as the most current floating point context
			stw		r8,SAVprev+4(r3)				; And then chain this in front

			stw		r9,SAVlevel(r3)					; Show level in savearea

            bl		fp_store						; save all 32 FPRs in the save area at r3
			mtlr	r2								; Restore return
            
fsret:		mtmsr	r0								; Put interrupts on if they were and floating point off
			isync

			blr

fsbackout:	mr		r4,r0							; restore the original MSR
			b		EXT(save_ret_wMSR)				; Toss savearea and return from there...

/*
 * fpu_switch()
 *
 * Entered to handle the floating-point unavailable exception and
 * switch fpu context
 *
 * This code is run in virtual address mode on with interrupts off.
 *
 * Upon exit, the code returns to the users context with the floating
 * point facility turned on.
 *
 * ENTRY:	VM switched ON
 *		Interrupts  OFF
 *              State is saved in savearea pointed to by R4.
 *				All other registers are free.
 * 
 */

			.align	5
			.globl	EXT(fpu_switch)

LEXT(fpu_switch)

#if DEBUG
			lis		r3,hi16(EXT(fpu_trap_count))	; Get address of FP trap counter
			ori		r3,r3,lo16(EXT(fpu_trap_count))	; Get address of FP trap counter
			lwz		r1,0(r3)
			addi	r1,r1,1
			stw		r1,0(r3)
#endif /* DEBUG */

			mfsprg	r26,0							; Get the per_processor block
			mfmsr	r19								; Get the current MSR 
			
			mr		r25,r4							; Save the entry savearea
			lwz		r22,FPUowner(r26)				; Get the thread that owns the FPU
			lwz		r10,PP_ACTIVE_THREAD(r26)		; Get the pointer to the active thread
			ori		r19,r19,lo16(MASK(MSR_FP))		; Enable the floating point feature
			lwz		r17,THREAD_TOP_ACT(r10)			; Now get the activation that is running
			
			mtmsr	r19								; Enable floating point instructions
			isync

			lwz		r27,ACT_MACT_PCB(r17)			; Get the current level
			lwz		r29,curctx(r17)					; Grab the current context anchor of the current thread

;			R22 has the "old" context anchor
;			R29 has the "new" context anchor

#if FPVECDBG
			lis		r0,hi16(CutTrace)				; (TEST/DEBUG)
			li		r2,0x7F01						; (TEST/DEBUG)
			mr		r3,r22							; (TEST/DEBUG)
			mr		r5,r29							; (TEST/DEBUG)
			oris	r0,r0,lo16(CutTrace)			; (TEST/DEBUG)
			sc										; (TEST/DEBUG)
#endif	
						
			lhz		r16,PP_CPU_NUMBER(r26)			; Get the current CPU number

fswretry:	mr.		r22,r22							; See if there is any live FP status			

			beq-	fsnosave						; No live context, so nothing to save...

			isync									; Make sure we see this in the right order

			lwz		r30,FPUsave(r22)				; Get the top savearea
			cmplw	cr2,r22,r29						; Are both old and new the same context?
			lwz		r18,FPUcpu(r22)					; Get the last CPU we ran on
			cmplwi	cr1,r30,0						; Anything saved yet?
			cmplw	r18,r16							; Make sure we are on the right processor
			lwz		r31,FPUlevel(r22)				; Get the context level

			bne-	fsnosave						; No, not on the same processor...
						
;
;			Check to see if the live context has already been saved.
;			Also check to see if all we are here just to re-enable the MSR
;			and handle specially if so.
;

			cmplw	r31,r27							; See if the current and active levels are the same
			crand	cr0_eq,cr2_eq,cr0_eq			; Remember if both the levels and contexts are the same
			li		r3,0							; Clear this
			
			beq-	fsthesame						; New and old are the same, just go enable...

			beq-	cr1,fsmstsave					; Not saved yet, go do it...
			
			lwz		r11,SAVlevel(r30)				; Get the level of top saved context
			
			cmplw	r31,r11							; Are live and saved the same?

#if FPVECDBG
			lis		r0,hi16(CutTrace)				; (TEST/DEBUG)
			li		r2,0x7F02						; (TEST/DEBUG)
			mr		r3,r30							; (TEST/DEBUG)
			mr		r5,r31							; (TEST/DEBUG)
			oris	r0,r0,lo16(CutTrace)			; (TEST/DEBUG)
			sc										; (TEST/DEBUG)
			li		r3,0							; (TEST/DEBUG)
#endif	

			beq+	fsnosave						; Same level, so already saved...
			
			
fsmstsave:	stw		r3,FPUowner(r26)				; Kill the context now
			eieio									; Make sure everyone sees it
			bl		EXT(save_get)					; Go get a savearea

			mr.		r31,r31							; Are we saving the user state?
			la		r15,FPUsync(r22)				; Point to the sync word
			beq++	fswusave						; Yeah, no need for lock...
;
;			Here we make sure that the live context is not tossed while we are
;			trying to push it.  This can happen only for kernel context and
;			then only by a race with act_machine_sv_free.
;
;			We only need to hold this for a very short time, so no sniffing needed.
;			If we find any change to the level, we just abandon.
;
fswsync:	lwarx	r19,0,r15						; Get the sync word
			li		r0,1							; Get the lock
			cmplwi	cr1,r19,0						; Is it unlocked?
			stwcx.	r0,0,r15						; Store lock and test reservation
			cror	cr0_eq,cr1_eq,cr0_eq			; Combine lost reservation and previously locked
			bne--	fswsync							; Try again if lost reservation or locked...

			isync									; Toss speculation
			
			lwz		r0,FPUlevel(r22)				; Pick up the level again
			li		r7,0							; Get unlock value
			cmplw	r0,r31							; Same level?
			beq++	fswusave						; Yeah, we expect it to be...
			
			stw		r7,FPUsync(r22)					; Unlock lock. No need to sync here
			
			bl		EXT(save_ret)					; Toss save area because we are abandoning save				
			b		fsnosave						; Skip the save...

			.align	5

fswusave:	lwz		r12,facAct(r22)					; Get the activation associated with the context
			stw		r3,FPUsave(r22)					; Set this as the latest context savearea for the thread
			mr.		r31,r31							; Check again if we were user level
			stw		r30,SAVprev+4(r3)				; Point us to the old context
			stw		r31,SAVlevel(r3)				; Tag our level
			li		r7,SAVfloat						; Get the floating point ID
			stw		r12,SAVact(r3)					; Make sure we point to the right guy
			stb		r7,SAVflags+2(r3)				; Set that we have a floating point save area
			
			li		r7,0							; Get the unlock value

			beq--	fswnulock						; Skip unlock if user (we did not lock it)...
			eieio									; Make sure that these updates make it out
			stw		r7,FPUsync(r22)					; Unlock it.
			
fswnulock:		

#if FPVECDBG
			lis		r0,hi16(CutTrace)				; (TEST/DEBUG)
			li		r2,0x7F03						; (TEST/DEBUG)
			oris	r0,r0,lo16(CutTrace)			; (TEST/DEBUG)
			sc										; (TEST/DEBUG)
#endif	

            bl		fp_store						; store all 32 FPRs

;
;			The context is all saved now and the facility is free.
;
;			If we do not we need to fill the registers with junk, because this level has 
;			never used them before and some thieving bastard could hack the old values
;			of some thread!  Just imagine what would happen if they could!  Why, nothing
;			would be safe! My God! It is terrifying!
;


fsnosave:	lwz		r15,ACT_MACT_PCB(r17)			; Get the current level of the "new" one
			lwz		r19,FPUcpu(r29)					; Get the last CPU we ran on
			lwz		r14,FPUsave(r29)				; Point to the top of the "new" context stack

			stw		r16,FPUcpu(r29)					; Claim context for us
			eieio

#if FPVECDBG
			lwz		r13,FPUlevel(r29)				; (TEST/DEBUG)
			lis		r0,hi16(CutTrace)				; (TEST/DEBUG)
			li		r2,0x7F04						; (TEST/DEBUG)
			mr		r1,r15							; (TEST/DEBUG)
			mr		r3,r14							; (TEST/DEBUG)
			mr		r5,r13							; (TEST/DEBUG)
			oris	r0,r0,lo16(CutTrace)			; (TEST/DEBUG)
			sc										; (TEST/DEBUG)
#endif	
			
			lis		r18,hi16(EXT(per_proc_info))	; Set base per_proc
			mulli	r19,r19,ppSize					; Find offset to the owner per_proc			
			ori		r18,r18,lo16(EXT(per_proc_info))	; Set base per_proc
			li		r16,FPUowner					; Displacement to float owner
			add		r19,r18,r19						; Point to the owner per_proc	
			
fsinvothr:	lwarx	r18,r16,r19						; Get the owner
			sub		r0,r18,r29						; Subtract one from the other
			sub		r11,r29,r18						; Subtract the other from the one
			or		r11,r11,r0						; Combine them
			srawi	r11,r11,31						; Get a 0 if equal or -1 of not
			and		r18,r18,r11						; Make 0 if same, unchanged if not
			stwcx.	r18,r16,r19						; Try to invalidate it
			bne--	fsinvothr						; Try again if there was a collision...
		
			cmplwi	cr1,r14,0						; Do we possibly have some context to load?
			la		r11,savefp0(r14)				; Point to first line to bring in
			stw		r15,FPUlevel(r29)				; Set the "new" active level
			eieio
			stw		r29,FPUowner(r26)				; Mark us as having the live context
			
			beq++	cr1,MakeSureThatNoTerroristsCanHurtUsByGod	; No "new" context to load...
			
			dcbt	0,r11							; Touch line in

			lwz		r3,SAVprev+4(r14)				; Get the previous context
			lwz		r0,SAVlevel(r14)				; Get the level of first facility savearea
			cmplw	r0,r15							; Top level correct to load?
			bne--	MakeSureThatNoTerroristsCanHurtUsByGod	; No, go initialize...

			stw		r3,FPUsave(r29)					; Pop the context (we will toss the savearea later)
			
#if FPVECDBG
			lis		r0,hi16(CutTrace)				; (TEST/DEBUG)
			li		r2,0x7F05						; (TEST/DEBUG)
			oris	r0,r0,lo16(CutTrace)			; (TEST/DEBUG)
			sc										; (TEST/DEBUG)
#endif	

// Note this code is used both by 32- and 128-byte processors.  This means six extra DCBTs
// are executed on a 128-byte machine, but that is better than a mispredicted branch.

			la		r11,savefp4(r14)				; Point to next line
			dcbt	0,r11							; Touch line in
			lfd     f0, savefp0(r14)
			lfd     f1,savefp1(r14)
			lfd     f2,savefp2(r14)
			la		r11,savefp8(r14)				; Point to next line
			lfd     f3,savefp3(r14)
			dcbt	0,r11							; Touch line in
			lfd     f4,savefp4(r14)
			lfd     f5,savefp5(r14)
			lfd     f6,savefp6(r14)
			la		r11,savefp12(r14)				; Point to next line
			lfd     f7,savefp7(r14)
			dcbt	0,r11							; Touch line in
			lfd     f8,savefp8(r14)
			lfd     f9,savefp9(r14)
			lfd     f10,savefp10(r14)
			la		r11,savefp16(r14)				; Point to next line
			lfd     f11,savefp11(r14)
			dcbt	0,r11							; Touch line in
			lfd     f12,savefp12(r14)
			lfd     f13,savefp13(r14)
			lfd     f14,savefp14(r14)
			la		r11,savefp20(r14)				; Point to next line
			lfd     f15,savefp15(r14)
			dcbt	0,r11							; Touch line in
			lfd     f16,savefp16(r14)
			lfd     f17,savefp17(r14)
			lfd     f18,savefp18(r14)
			la		r11,savefp24(r14)				; Point to next line
			lfd     f19,savefp19(r14)
			dcbt	0,r11							; Touch line in
			lfd     f20,savefp20(r14)
			lfd     f21,savefp21(r14)
			la		r11,savefp28(r14)				; Point to next line
			lfd     f22,savefp22(r14)
			lfd     f23,savefp23(r14)
			dcbt	0,r11							; Touch line in
			lfd     f24,savefp24(r14)
			lfd     f25,savefp25(r14)
			lfd     f26,savefp26(r14)
			lfd     f27,savefp27(r14)
			lfd     f28,savefp28(r14)
			lfd     f29,savefp29(r14)
			lfd     f30,savefp30(r14)
			lfd     f31,savefp31(r14)
			
			mr		r3,r14							; Get the old savearea (we popped it before)
			bl		EXT(save_ret)					; Toss it
			
fsenable:	lwz		r8,savesrr1+4(r25)				; Get the msr of the interrupted guy
			ori		r8,r8,MASK(MSR_FP)				; Enable the floating point feature
			lwz		r10,ACT_MACT_SPF(r17)			; Get the act special flags
			lwz		r11,spcFlags(r26)				; Get per_proc spec flags cause not in sync with act
			oris	r10,r10,hi16(floatUsed|floatCng)	; Set that we used floating point
			oris	r11,r11,hi16(floatUsed|floatCng)	; Set that we used floating point
			rlwinm.	r0,r8,0,MSR_PR_BIT,MSR_PR_BIT	; See if we are doing this for user state
			stw		r8,savesrr1+4(r25)				; Set the msr of the interrupted guy
			mr		r3,r25							; Pass the virtual addres of savearea
			beq-	fsnuser							; We are not user state...
			stw		r10,ACT_MACT_SPF(r17)			; Set the activation copy
			stw		r11,spcFlags(r26)				; Set per_proc copy

fsnuser:
#if FPVECDBG
			lis		r0,hi16(CutTrace)				; (TEST/DEBUG)
			li		r2,0x7F07						; (TEST/DEBUG)
			oris	r0,r0,lo16(CutTrace)			; (TEST/DEBUG)
			sc										; (TEST/DEBUG)
#endif	
			
			b		EXT(exception_exit)				; Exit to the fray...

/*
 *			Initialize the registers to some bogus value
 */

MakeSureThatNoTerroristsCanHurtUsByGod:

#if FPVECDBG
			lis		r0,hi16(CutTrace)				; (TEST/DEBUG)
			li		r2,0x7F06						; (TEST/DEBUG)
			oris	r0,r0,lo16(CutTrace)			; (TEST/DEBUG)
			sc										; (TEST/DEBUG)
#endif	
			lis		r5,hi16(EXT(FloatInit))			; Get top secret floating point init value address
			ori		r5,r5,lo16(EXT(FloatInit))		; Slam bottom
			lfd		f0,0(r5)						; Initialize FP0 
			fmr		f1,f0							; Do them all						
			fmr		f2,f0								
			fmr		f3,f0								
			fmr		f4,f0								
			fmr		f5,f0						
			fmr		f6,f0						
			fmr		f7,f0						
			fmr		f8,f0						
			fmr		f9,f0						
			fmr		f10,f0						
			fmr		f11,f0						
			fmr		f12,f0						
			fmr		f13,f0						
			fmr		f14,f0						
			fmr		f15,f0						
			fmr		f16,f0						
			fmr		f17,f0
			fmr		f18,f0						
			fmr		f19,f0						
			fmr		f20,f0						
			fmr		f21,f0						
			fmr		f22,f0						
			fmr		f23,f0						
			fmr		f24,f0						
			fmr		f25,f0						
			fmr		f26,f0						
			fmr		f27,f0						
			fmr		f28,f0						
			fmr		f29,f0						
			fmr		f30,f0						
			fmr		f31,f0						
			b		fsenable						; Finish setting it all up...				


;
;			We get here when we are switching to the same context at the same level and the context
;			is still live.  Essentially, all we are doing is turning on the faility.  It may have
;			gotten turned off due to doing a context save for the current level or a context switch
;			back to the live guy.
;

			.align	5
			
fsthesame:

#if FPVECDBG
			lis		r0,hi16(CutTrace)				; (TEST/DEBUG)
			li		r2,0x7F0A						; (TEST/DEBUG)
			oris	r0,r0,lo16(CutTrace)			; (TEST/DEBUG)
			sc										; (TEST/DEBUG)
#endif	
			beq-	cr1,fsenable					; Not saved yet, nothing to pop, go enable and exit...
			
			lwz		r11,SAVlevel(r30)				; Get the level of top saved context
			lwz		r14,SAVprev+4(r30)				; Get the previous savearea
			
			cmplw	r11,r31							; Are live and saved the same?

			bne+	fsenable						; Level not the same, nothing to pop, go enable and exit...
			
			mr		r3,r30							; Get the old savearea (we popped it before)
			stw		r14,FPUsave(r22)				; Pop the savearea from the stack
			bl		EXT(save_ret)					; Toss it
			b		fsenable						; Go enable and exit...


;
;			This function invalidates any live floating point context for the passed in facility_context.
;			This is intended to be called just before act_machine_sv_free tosses saveareas.
;

			.align	5
			.globl	EXT(toss_live_fpu)

LEXT(toss_live_fpu)
			
			lis		r0,hi16(MASK(MSR_VEC))			; Get VEC
			mfmsr	r9								; Get the MSR
			ori		r0,r0,lo16(MASK(MSR_FP))		; Add in FP
			rlwinm.	r8,r9,0,MSR_FP_BIT,MSR_FP_BIT	; Are floats on right now?
			andc	r9,r9,r0						; Force off VEC and FP
			ori		r0,r0,lo16(MASK(MSR_EE))		; Turn off EE
			andc	r0,r9,r0						; Turn off EE now
			mtmsr	r0								; No interruptions
			isync
			beq+	tlfnotours						; Floats off, can not be live here...

			mfsprg	r8,0							; Get the per proc

;
;			Note that at this point, since floats are on, we are the owner
;			of live state on this processor
;

			lwz		r6,FPUowner(r8)					; Get the thread that owns the floats
			li		r0,0							; Clear this just in case we need it
			cmplw	r6,r3							; Are we tossing our own context?
			bne--	tlfnotours						; Nope...
			
			lfd		f1,Zero(0)						; Make a 0			
			mtfsf	0xFF,f1							; Clear it

tlfnotours:	lwz		r11,FPUcpu(r3)					; Get the cpu on which we last loaded context
			lis		r12,hi16(EXT(per_proc_info))	; Set base per_proc
			mulli	r11,r11,ppSize					; Find offset to the owner per_proc			
			ori		r12,r12,lo16(EXT(per_proc_info))	; Set base per_proc
			li		r10,FPUowner					; Displacement to float owner
			add		r11,r12,r11						; Point to the owner per_proc	
			
tlfinvothr:	lwarx	r12,r10,r11						; Get the owner

			sub		r0,r12,r3						; Subtract one from the other
			sub		r8,r3,r12						; Subtract the other from the one
			or		r8,r8,r0						; Combine them
			srawi	r8,r8,31						; Get a 0 if equal or -1 of not
			and		r12,r12,r8						; Make 0 if same, unchanged if not
			stwcx.	r12,r10,r11						; Try to invalidate it
			bne--	tlfinvothr						; Try again if there was a collision...

			mtmsr	r9								; Restore interruptions
			isync									; Could be turning off floats here
			blr										; Leave...


/*
 *			Altivec stuff is here. The techniques used are pretty identical to
 *			the floating point. Except that we will honor the VRSAVE register
 *			settings when loading and restoring registers.
 *
 *			There are two indications of saved VRs: the VRSAVE register and the vrvalid
 *			mask. VRSAVE is set by the vector user and represents the VRs that they
 *			say that they are using. The vrvalid mask indicates which vector registers
 *			are saved in the savearea. Whenever context is saved, it is saved according
 *			to the VRSAVE register.  It is loaded based on VRSAVE anded with
 *			vrvalid (all other registers are splatted with 0s). This is done because we
 *			don't want to load any registers we don't have a copy of, we want to set them
 *			to zero instead.
 *
 *			Note that there are some oddities here when we save a context we are using.
 *			It is really not too cool to do this, but what the hey...  Anyway, 
 *			we turn vectors and fpu off before we leave.
 *			The oddity is that if you use vectors after this, the
 *			savearea containing the context just saved will go away.  So, bottom line is
 *			that don't use vectors until after you are done with the saved context.
 *
 */

			.align	5
			.globl	EXT(vec_save)

LEXT(vec_save)


			lis		r2,hi16(MASK(MSR_VEC))			; Get VEC
			mfmsr	r0								; Get the MSR
			ori		r2,r2,lo16(MASK(MSR_FP))		; Add in FP
			andc	r0,r0,r2						; Force off VEC and FP
			ori		r2,r2,lo16(MASK(MSR_EE))		; Clear EE
			andc	r2,r0,r2						; Clear EE for now
			oris	r2,r2,hi16(MASK(MSR_VEC))		; Enable the vector facility for now also
			mtmsr	r2								; Set the MSR
			isync
		
			mfsprg	r6,0							; Get the per_processor block 
			lwz		r12,VMXowner(r6)				; Get the context ID for owner

#if FPVECDBG
			mr		r7,r0							; (TEST/DEBUG)
			li		r4,0							; (TEST/DEBUG)
			mr		r10,r3							; (TEST/DEBUG)
			lis		r0,hi16(CutTrace)				; (TEST/DEBUG)
			mr.		r3,r12							; (TEST/DEBUG)
			li		r2,0x5F00						; (TEST/DEBUG)
			li		r5,0							; (TEST/DEBUG)
			beq-	noowneryeu						; (TEST/DEBUG)
			lwz		r4,VMXlevel(r12)				; (TEST/DEBUG)
			lwz		r5,VMXsave(r12)					; (TEST/DEBUG)

noowneryeu:	oris	r0,r0,lo16(CutTrace)			; (TEST/DEBUG)
			sc										; (TEST/DEBUG)
			mr		r0,r7							; (TEST/DEBUG)
			mr		r3,r10							; (TEST/DEBUG)
#endif	
			mflr	r2								; Save the return address

vsretry:	mr.		r12,r12							; Anyone own the vector?
			lhz		r11,PP_CPU_NUMBER(r6)			; Get our CPU number
			beq-	vsret							; Nobody owns the vector, no save required...
			
			cmplw	cr1,r3,r12						; Is the specified context live?
			
			isync									; Force owner check first

			lwz		r9,VMXcpu(r12)					; Get the cpu that context was last on		
			bne-	cr1,vsret						; Specified context is not live
			
			cmplw	cr1,r9,r11						; Was the context for this processor? 
			beq+	cr1,vsgoodcpu					; Facility last used on this processor...

			b		vsret							; Someone else claimed this...
			
			.align	5
			
vsgoodcpu:	lwz		r3,VMXsave(r12)					; Get the current vector savearea for the thread
			lwz		r10,liveVRS(r6)					; Get the right VRSave register
			lwz		r9,VMXlevel(r12)				; Get our current level indicator
			
			
			cmplwi	cr1,r3,0						; Have we ever saved this facility context?
			beq-	cr1,vsneedone					; Never saved it, so we need an area...
			
			lwz		r8,SAVlevel(r3)					; Get the level this savearea is for
			mr.		r10,r10							; Is VRsave set to 0?
			cmplw	cr1,r9,r8						; Correct level?
			bne-	cr1,vsneedone					; Different level, so we need to save...
			
			bne+	vsret							; VRsave is non-zero so we need to keep what is saved...
						
			lwz		r4,SAVprev+4(r3)				; Pick up the previous area
			lwz		r5,SAVlevel(r4)					; Get the level associated with save
			stw		r4,VMXsave(r12)					; Dequeue this savearea
			li		r4,0							; Clear
			stw		r5,VMXlevel(r12)				; Save the level
	
			stw		r4,VMXowner(r12)				; Show no live context here
			eieio

vsbackout:	mr		r4,r0							; restore the saved MSR			
			b		EXT(save_ret_wMSR)				; Toss the savearea and return from there...

			.align	5

vsneedone:	mr.		r10,r10							; Is VRsave set to 0?
			beq-	vsret							; Yeah, they do not care about any of them...

			bl		EXT(save_get)					; Get a savearea for the context
			
			mfsprg	r6,0							; Get back per_processor block
			li		r4,SAVvector					; Get vector tag			
			lwz		r12,VMXowner(r6)				; Get back our context ID
			stb		r4,SAVflags+2(r3)				; Mark this savearea as a vector
			mr.		r12,r12							; See if we were disowned while away. Very, very small chance of it...
			beq-	vsbackout						; If disowned, just toss savearea...
			lwz		r4,facAct(r12)					; Get the activation associated with live context
			lwz		r8,VMXsave(r12)					; Get the current top vector savearea
			stw		r4,SAVact(r3)					; Indicate the right activation for this context
			lwz		r9,VMXlevel(r12)				; Get our current level indicator again		
			stw		r3,VMXsave(r12)					; Set this as the most current floating point context
			stw		r8,SAVprev+4(r3)				; And then chain this in front

			stw		r9,SAVlevel(r3)					; Set level in savearea
            mfcr	r12								; save CRs across call to vr_store
			lwz		r10,liveVRS(r6)					; Get the right VRSave register
            
            bl		vr_store						; store live VRs into savearea as required (uses r4-r11)

			mtcrf	255,r12							; Restore the non-volatile CRs
            mtlr	r2								; restore return address
		
vsret:		mtmsr	r0								; Put interrupts on if they were and vector off
			isync

			blr

/*
 * vec_switch()
 *
 * Entered to handle the vector unavailable exception and
 * switch vector context
 *
 * This code is run with virtual address mode on and interrupts off.
 *
 * Upon exit, the code returns to the users context with the vector
 * facility turned on.
 *
 * ENTRY:	VM switched ON
 *		Interrupts  OFF
 *              State is saved in savearea pointed to by R4.
 *				All other registers are free.
 * 
 */

			.align	5
			.globl	EXT(vec_switch)

LEXT(vec_switch)

#if DEBUG
			lis		r3,hi16(EXT(vec_trap_count))	; Get address of vector trap counter
			ori		r3,r3,lo16(EXT(vec_trap_count))	; Get address of vector trap counter
			lwz		r1,0(r3)
			addi	r1,r1,1
			stw		r1,0(r3)
#endif /* DEBUG */

			mfsprg	r26,0							; Get the per_processor block
			mfmsr	r19								; Get the current MSR 
			
			mr		r25,r4							; Save the entry savearea
			lwz		r22,VMXowner(r26)				; Get the thread that owns the vector
			lwz		r10,PP_ACTIVE_THREAD(r26)		; Get the pointer to the active thread
			oris	r19,r19,hi16(MASK(MSR_VEC))		; Enable the vector feature
			lwz		r17,THREAD_TOP_ACT(r10)			; Now get the activation that is running
				
			mtmsr	r19								; Enable vector instructions
			isync
			
			lwz		r27,ACT_MACT_PCB(r17)			; Get the current level
			lwz		r29,curctx(r17)					; Grab the current context anchor of the current thread

;			R22 has the "old" context anchor
;			R29 has the "new" context anchor

#if FPVECDBG
			lis		r0,HIGH_ADDR(CutTrace)			; (TEST/DEBUG)
			li		r2,0x5F01						; (TEST/DEBUG)
			mr		r3,r22							; (TEST/DEBUG)
			mr		r5,r29							; (TEST/DEBUG)
			oris	r0,r0,LOW_ADDR(CutTrace)		; (TEST/DEBUG)
			sc										; (TEST/DEBUG)
#endif	

			lhz		r16,PP_CPU_NUMBER(r26)			; Get the current CPU number
			
vsvretry:	mr.		r22,r22							; See if there is any live vector status
			
			beq-	vsnosave						; No live context, so nothing to save...

			isync									; Make sure we see this in the right order

			lwz		r30,VMXsave(r22)				; Get the top savearea
			cmplw	cr2,r22,r29						; Are both old and new the same context?
			lwz		r18,VMXcpu(r22)					; Get the last CPU we ran on
			cmplwi	cr1,r30,0						; Anything saved yet?
			cmplw	r18,r16							; Make sure we are on the right processor
			lwz		r31,VMXlevel(r22)				; Get the context level
			
			lwz		r10,liveVRS(r26)				; Get the right VRSave register

			bne-	vsnosave						; No, not on the same processor...
		
;
;			Check to see if the live context has already been saved.
;			Also check to see if all we are here just to re-enable the MSR
;			and handle specially if so.
;

			cmplw	r31,r27							; See if the current and active levels are the same
			crand	cr0_eq,cr2_eq,cr0_eq			; Remember if both the levels and contexts are the same
			li		r8,0							; Clear this
			
			beq-	vsthesame						; New and old are the same, just go enable...

			cmplwi	cr2,r10,0						; Check VRSave to see if we really need to save anything...
			beq-	cr1,vsmstsave					; Not saved yet, go do it...
			
			lwz		r11,SAVlevel(r30)				; Get the level of top saved context
			
			cmplw	r31,r11							; Are live and saved the same?

#if FPVECDBG
			lis		r0,hi16(CutTrace)				; (TEST/DEBUG)
			li		r2,0x5F02						; (TEST/DEBUG)
			mr		r3,r30							; (TEST/DEBUG)
			mr		r5,r31							; (TEST/DEBUG)
			oris	r0,r0,lo16(CutTrace)			; (TEST/DEBUG)
			sc										; (TEST/DEBUG)
#endif	

			bne-	vsmstsave						; Live context has not been saved yet...

			bne-	cr2,vsnosave					; Live context saved and VRSave not 0, no save and keep context...
			
			lwz		r4,SAVprev+4(r30)				; Pick up the previous area
			li		r5,0							; Assume this is the only one (which should be the ususal case)
			mr.		r4,r4							; Was this the only one?
			stw		r4,VMXsave(r22)					; Dequeue this savearea
			beq+	vsonlyone						; This was the only one...
			lwz		r5,SAVlevel(r4)					; Get the level associated with previous save

vsonlyone:	stw		r5,VMXlevel(r22)				; Save the level
			stw		r8,VMXowner(r26)				; Clear owner
			eieio
			mr		r3,r30							; Copy the savearea we are tossing
			bl		EXT(save_ret)					; Toss the savearea
			b		vsnosave						; Go load up the context...

			.align	5

	
vsmstsave:	stw		r8,VMXowner(r26)				; Clear owner
			eieio
			beq-	cr2,vsnosave					; The VRSave was 0, so there is nothing to save...

			bl		EXT(save_get)					; Go get a savearea

			mr.		r31,r31							; Are we saving the user state?
			la		r15,VMXsync(r22)				; Point to the sync word
			beq++	vswusave						; Yeah, no need for lock...
;
;			Here we make sure that the live context is not tossed while we are
;			trying to push it.  This can happen only for kernel context and
;			then only by a race with act_machine_sv_free.
;
;			We only need to hold this for a very short time, so no sniffing needed.
;			If we find any change to the level, we just abandon.
;
vswsync:	lwarx	r19,0,r15						; Get the sync word
			li		r0,1							; Get the lock
			cmplwi	cr1,r19,0						; Is it unlocked?
			stwcx.	r0,0,r15						; Store lock and test reservation
			cror	cr0_eq,cr1_eq,cr0_eq			; Combine lost reservation and previously locked
			bne--	vswsync							; Try again if lost reservation or locked...

			isync									; Toss speculation
			
			lwz		r0,VMXlevel(r22)				; Pick up the level again
			li		r7,0							; Get unlock value
			cmplw	r0,r31							; Same level?
			beq++	fswusave						; Yeah, we expect it to be...
			
			stw		r7,VMXsync(r22)					; Unlock lock. No need to sync here
			
			bl		EXT(save_ret)					; Toss save area because we are abandoning save				
			b		fsnosave						; Skip the save...

			.align	5

vswusave:	lwz		r12,facAct(r22)					; Get the activation associated with the context
			stw		r3,VMXsave(r22)					; Set this as the latest context savearea for the thread
			mr.		r31,r31							; Check again if we were user level
			stw		r30,SAVprev+4(r3)				; Point us to the old context
			stw		r31,SAVlevel(r3)				; Tag our level
			li		r7,SAVvector					; Get the vector ID
			stw		r12,SAVact(r3)					; Make sure we point to the right guy
			stb		r7,SAVflags+2(r3)				; Set that we have a vector save area
		
			li		r7,0							; Get the unlock value

			beq--	vswnulock						; Skip unlock if user (we did not lock it)...
			eieio									; Make sure that these updates make it out
			stw		r7,VMXsync(r22)					; Unlock it.
			
vswnulock:		

#if FPVECDBG
			lis		r0,hi16(CutTrace)				; (TEST/DEBUG)
			li		r2,0x5F03						; (TEST/DEBUG)
			oris	r0,r0,lo16(CutTrace)			; (TEST/DEBUG)
			sc										; (TEST/DEBUG)
#endif	

			lwz		r10,liveVRS(r26)				; Get the right VRSave register
            bl		vr_store						; store VRs into savearea according to vrsave (uses r4-r11)
			

;
;			The context is all saved now and the facility is free.
;
;			If we do not we need to fill the registers with junk, because this level has 
;			never used them before and some thieving bastard could hack the old values
;			of some thread!  Just imagine what would happen if they could!  Why, nothing
;			would be safe! My God! It is terrifying!
;
;			Also, along the way, thanks to Ian Ollmann, we generate the 0x7FFFDEAD (QNaNbarbarian)
;			constant that we may need to fill unused vector registers.
;




vsnosave:	vspltisb v31,-10						; Get 0xF6F6F6F6	
			lwz		r15,ACT_MACT_PCB(r17)			; Get the current level of the "new" one
			vspltisb v30,5							; Get 0x05050505	
			lwz		r19,VMXcpu(r29)					; Get the last CPU we ran on
			vspltish v29,4							; Get 0x00040004
			lwz		r14,VMXsave(r29)				; Point to the top of the "new" context stack
			vrlb	v31,v31,v30						; Get 0xDEDEDEDE

			stw		r16,VMXcpu(r29)					; Claim context for us
			eieio

#if FPVECDBG
			lwz		r13,VMXlevel(r29)				; (TEST/DEBUG)
			lis		r0,hi16(CutTrace)				; (TEST/DEBUG)
			li		r2,0x5F04						; (TEST/DEBUG)
			mr		r1,r15							; (TEST/DEBUG)
			mr		r3,r14							; (TEST/DEBUG)
			mr		r5,r13							; (TEST/DEBUG)
			oris	r0,r0,lo16(CutTrace)			; (TEST/DEBUG)
			sc										; (TEST/DEBUG)
#endif	
			
			lis		r18,hi16(EXT(per_proc_info))	; Set base per_proc
			vspltisb v28,-2							; Get 0xFEFEFEFE		   
			mulli	r19,r19,ppSize					; Find offset to the owner per_proc			
			vsubuhm	v31,v31,v29						; Get 0xDEDADEDA
			ori		r18,r18,lo16(EXT(per_proc_info))	; Set base per_proc
			vpkpx	v30,v28,v3						; Get 0x7FFF7FFF
			li		r16,VMXowner					; Displacement to vector owner
			add		r19,r18,r19						; Point to the owner per_proc	
			vrlb	v31,v31,v29						; Get 0xDEADDEAD	
			
vsinvothr:	lwarx	r18,r16,r19						; Get the owner

			sub		r0,r18,r29						; Subtract one from the other
			sub		r11,r29,r18						; Subtract the other from the one
			or		r11,r11,r0						; Combine them
			srawi	r11,r11,31						; Get a 0 if equal or -1 of not
			and		r18,r18,r11						; Make 0 if same, unchanged if not
			stwcx.	r18,r16,r19						; Try to invalidate it
			bne--	vsinvothr						; Try again if there was a collision...		
	
			cmplwi	cr1,r14,0						; Do we possibly have some context to load?
			vmrghh	v31,v30,v31						; Get 0x7FFFDEAD.  V31 keeps this value until the bitter end
			stw		r15,VMXlevel(r29)				; Set the "new" active level
			eieio
			stw		r29,VMXowner(r26)				; Mark us as having the live context

			beq--	cr1,ProtectTheAmericanWay		; Nothing to restore, first time use...
		
			lwz		r3,SAVprev+4(r14)				; Get the previous context
			lwz		r0,SAVlevel(r14)				; Get the level of first facility savearea
			cmplw	r0,r15							; Top level correct to load?
			bne--	ProtectTheAmericanWay			; No, go initialize...
			
			stw		r3,VMXsave(r29)					; Pop the context (we will toss the savearea later)

#if FPVECDBG
			lis		r0,hi16(CutTrace)				; (TEST/DEBUG)
			li		r2,0x5F05						; (TEST/DEBUG)
			oris	r0,r0,lo16(CutTrace)			; (TEST/DEBUG)
			sc										; (TEST/DEBUG)
#endif	

			lwz		r10,savevrvalid(r14)			; Get the valid VRs in the savearea
			lwz		r22,savevrsave(r25)				; Get the most current VRSAVE
			and		r10,r10,r22						; Figure out just what registers need to be loaded
            mr		r3,r14							; r3 <- ptr to savearea with VRs
            bl		vr_load							; load VRs from save area based on vrsave in r10
            			
			bl		EXT(save_ret)					; Toss the save area after loading VRs
			
vrenable:	lwz		r8,savesrr1+4(r25)				; Get the msr of the interrupted guy
			oris	r8,r8,hi16(MASK(MSR_VEC))		; Enable the vector facility
			lwz		r10,ACT_MACT_SPF(r17)			; Get the act special flags
			lwz		r11,spcFlags(r26)				; Get per_proc spec flags cause not in sync with act
			oris	r10,r10,hi16(vectorUsed|vectorCng)	; Set that we used vectors
			oris	r11,r11,hi16(vectorUsed|vectorCng)	; Set that we used vectors
			rlwinm.	r0,r8,0,MSR_PR_BIT,MSR_PR_BIT	; See if we are doing this for user state
			stw		r8,savesrr1+4(r25)				; Set the msr of the interrupted guy
			mr		r3,r25							; Pass virtual address of the savearea
			beq-	vrnuser							; We are not user state...
			stw		r10,ACT_MACT_SPF(r17)			; Set the activation copy
			stw		r11,spcFlags(r26)				; Set per_proc copy

vrnuser:
#if FPVECDBG
			lis		r0,hi16(CutTrace)				; (TEST/DEBUG)
			li		r2,0x5F07						; (TEST/DEBUG)
			oris	r0,r0,lo16(CutTrace)			; (TEST/DEBUG)
			sc										; (TEST/DEBUG)
#endif	
			b		EXT(exception_exit)				; Exit to the fray...

/*
 *			Initialize the registers to some bogus value
 */

ProtectTheAmericanWay:
			
#if FPVECDBG
			lis		r0,hi16(CutTrace)				; (TEST/DEBUG)
			li		r2,0x5F06						; (TEST/DEBUG)
			oris	r0,r0,lo16(CutTrace)			; (TEST/DEBUG)
			sc										; (TEST/DEBUG)
#endif	
			
			vor		v0,v31,v31						; Copy into the next register
			vor		v1,v31,v31						; Copy into the next register
			vor		v2,v31,v31						; Copy into the next register
			vor		v3,v31,v31						; Copy into the next register
			vor		v4,v31,v31						; Copy into the next register
			vor		v5,v31,v31						; Copy into the next register
			vor		v6,v31,v31						; Copy into the next register
			vor		v7,v31,v31						; Copy into the next register
			vor		v8,v31,v31						; Copy into the next register
			vor		v9,v31,v31						; Copy into the next register
			vor		v10,v31,v31						; Copy into the next register
			vor		v11,v31,v31						; Copy into the next register
			vor		v12,v31,v31						; Copy into the next register
			vor		v13,v31,v31						; Copy into the next register
			vor		v14,v31,v31						; Copy into the next register
			vor		v15,v31,v31						; Copy into the next register
			vor		v16,v31,v31						; Copy into the next register
			vor		v17,v31,v31						; Copy into the next register
			vor		v18,v31,v31						; Copy into the next register
			vor		v19,v31,v31						; Copy into the next register
			vor		v20,v31,v31						; Copy into the next register
			vor		v21,v31,v31						; Copy into the next register
			vor		v22,v31,v31						; Copy into the next register
			vor		v23,v31,v31						; Copy into the next register
			vor		v24,v31,v31						; Copy into the next register
			vor		v25,v31,v31						; Copy into the next register
			vor		v26,v31,v31						; Copy into the next register
			vor		v27,v31,v31						; Copy into the next register
			vor		v28,v31,v31						; Copy into the next register
			vor		v29,v31,v31						; Copy into the next register
			vor		v30,v31,v31						; Copy into the next register
			b		vrenable						; Finish setting it all up...				



;
;			We get here when we are switching to the same context at the same level and the context
;			is still live.  Essentially, all we are doing is turning on the faility.  It may have
;			gotten turned off due to doing a context save for the current level or a context switch
;			back to the live guy.
;

			.align	5
			
vsthesame:

#if FPVECDBG
			lis		r0,hi16(CutTrace)				; (TEST/DEBUG)
			li		r2,0x5F0A						; (TEST/DEBUG)
			oris	r0,r0,lo16(CutTrace)			; (TEST/DEBUG)
			sc										; (TEST/DEBUG)
#endif	
			beq-	cr1,vrenable					; Not saved yet, nothing to pop, go enable and exit...
			
			lwz		r11,SAVlevel(r30)				; Get the level of top saved context
			lwz		r14,SAVprev+4(r30)				; Get the previous savearea
			
			cmplw	r11,r31							; Are live and saved the same?

			bne+	vrenable						; Level not the same, nothing to pop, go enable and exit...
			
			mr		r3,r30							; Get the old savearea (we popped it before)
			stw		r11,VMXsave(r22)				; Pop the vector stack
			bl		EXT(save_ret)					; Toss it
			b		vrenable						; Go enable and exit...


;
;			This function invalidates any live vector context for the passed in facility_context.
;			This is intended to be called just before act_machine_sv_free tosses saveareas.
;

			.align	5
			.globl	EXT(toss_live_vec)

LEXT(toss_live_vec)
			
			lis		r0,hi16(MASK(MSR_VEC))			; Get VEC
			mfmsr	r9								; Get the MSR
			ori		r0,r0,lo16(MASK(MSR_FP))		; Add in FP
			rlwinm.	r8,r9,0,MSR_VEC_BIT,MSR_VEC_BIT	; Are vectors on right now?
			andc	r9,r9,r0						; Force off VEC and FP
			ori		r0,r0,lo16(MASK(MSR_EE))		; Turn off EE
			andc	r0,r9,r0						; Turn off EE now
			mtmsr	r0								; No interruptions
			isync
			beq+	tlvnotours						; Vector off, can not be live here...

			mfsprg	r8,0							; Get the per proc

;
;			Note that at this point, since vecs are on, we are the owner
;			of live state on this processor
;

			lwz		r6,VMXowner(r8)					; Get the thread that owns the vector
			li		r0,0							; Clear this just in case we need it
			cmplw	r6,r3							; Are we tossing our own context?
			bne-	tlvnotours						; Nope...
			
			vspltish v1,1							; Turn on the non-Java bit and saturate
			vspltisw v0,1							; Turn on the saturate bit
			vxor	v1,v1,v0						; Turn off saturate	
			mtspr	vrsave,r0						; Clear VRSAVE 
			mtvscr	v1								; Set the non-java, no saturate status

tlvnotours:	lwz		r11,VMXcpu(r3)					; Get the cpu on which we last loaded context
			lis		r12,hi16(EXT(per_proc_info))	; Set base per_proc
			mulli	r11,r11,ppSize					; Find offset to the owner per_proc			
			ori		r12,r12,lo16(EXT(per_proc_info))	; Set base per_proc
			li		r10,VMXowner					; Displacement to vector owner
			add		r11,r12,r11						; Point to the owner per_proc	
			li		r0,0							; Set a 0 to invalidate context
			
tlvinvothr:	lwarx	r12,r10,r11						; Get the owner

			sub		r0,r12,r3						; Subtract one from the other
			sub		r8,r3,r12						; Subtract the other from the one
			or		r8,r8,r0						; Combine them
			srawi	r8,r8,31						; Get a 0 if equal or -1 of not
			and		r12,r12,r8						; Make 0 if same, unchanged if not
			stwcx.	r12,r10,r11						; Try to invalidate it
			bne--	tlvinvothr						; Try again if there was a collision...		

			mtmsr	r9								; Restore interruptions
			isync									; Could be turning off vectors here
			blr										; Leave....

#if 0
;
;			This function invalidates any live vector context for the passed in facility_context
;			if the level is current.  It also tosses the corresponding savearea if there is one.
;			This function is primarily used whenever we detect a VRSave that is all zeros.
;

			.align	5
			.globl	EXT(vec_trash)

LEXT(vec_trash)
			
			lwz		r12,facAct(r3)					; Get the activation
			lwz		r11,VMXlevel(r3)				; Get the context level
			lwz		r10,ACT_MACT_PCB(r12)			; Grab the current level for the thread
			lwz		r9,VMXsave(r3)					; Get the savearea, if any
			cmplw	r10,r11							; Are we at the right level?
			cmplwi	cr1,r9,0						; Remember if there is a savearea
			bnelr+									; No, we do nothing...			
			
			lwz		r11,VMXcpu(r3)					; Get the cpu on which we last loaded context
			lis		r12,hi16(EXT(per_proc_info))	; Set base per_proc
			mulli	r11,r11,ppSize					; Find offset to the owner per_proc			
			ori		r12,r12,lo16(EXT(per_proc_info))	; Set base per_proc
			li		r10,VMXowner					; Displacement to vector owner
			add		r11,r12,r11						; Point to the owner per_proc	
			
vtinvothr:	lwarx	r12,r10,r11						; Get the owner

			sub		r0,r12,r3						; Subtract one from the other
			sub		r8,r3,r12						; Subtract the other from the one
			or		r8,r8,r0						; Combine them
			srawi	r8,r8,31						; Get a 0 if equal or -1 of not
			and		r12,r12,r8						; Make 0 if same, unchanged if not
			stwcx.	r12,r10,r11						; Try to invalidate it
			bne--	vtinvothr						; Try again if there was a collision...		


			beqlr++	cr1								; Leave if there is no savearea
			lwz		r8,SAVlevel(r9)					; Get the level of the savearea
			cmplw	r8,r11							; Savearea for the current level?
			bnelr++									; No, nothing to release...
			
			lwz		r8,SAVprev+4(r9)				; Pick up the previous area
			mr.		r8,r8							; Is there a previous?
			beq--	vtnoprev						; Nope...
			lwz		r7,SAVlevel(r8)					; Get the level associated with save

vtnoprev:	stw		r8,VMXsave(r3)					; Dequeue this savearea
			stw		r7,VMXlevel(r3)					; Pop the level
			
			mr		r3,r9							; Get the savearea to release
			b		EXT(save_ret)					; Go and toss the save area (note, we will return from there)...
#endif	
			
;
;			Just some test code to force vector and/or floating point in the kernel
;			

			.align	5
			.globl	EXT(fctx_test)

LEXT(fctx_test)
			
			mfsprg	r3,0							; Get the per_proc block
			lwz		r3,PP_ACTIVE_THREAD(r3)			; Get the thread pointer
			mr.		r3,r3							; Are we actually up and running?
			beqlr-									; No...
			
			fmr		f0,f0							; Use floating point
			mftb	r4								; Get time base for a random number
			li		r5,1							; Get a potential vrsave to use
			andi.	r4,r4,0x3F						; Get a number from 0 - 63
			slw		r5,r5,r4						; Choose a register to save (should be 0 half the time)
			mtspr	vrsave,r5						; Set VRSave
			vor		v0,v0,v0						; Use vectors
			blr


// *******************
// * f p _ s t o r e *
// *******************
//
// Store FPRs into a save area.   Called by fpu_save and fpu_switch.
//
// When called:
//		floating pt is enabled
//		r3 = ptr to save area
//
// We destroy:
//		r11.

fp_store:
            mfsprg	r11,2					; get feature flags
            mtcrf	0x02,r11				; put cache line size bits in cr6
            la		r11,savefp0(r3)			; point to 1st line
            dcbz128	0,r11					; establish 1st line no matter what linesize is
            bt--	pf32Byteb,fp_st32		; skip if a 32-byte machine
            
// Store the FPRs on a 128-byte machine.
			
			stfd    f0,savefp0(r3)
			stfd    f1,savefp1(r3)
			la		r11,savefp16(r3)		; Point to the 2nd cache line
			stfd    f2,savefp2(r3)
			stfd    f3,savefp3(r3)
			dcbz128	0,r11					; establish 2nd line
			stfd    f4,savefp4(r3)
			stfd    f5,savefp5(r3)
			stfd    f6,savefp6(r3)
			stfd    f7,savefp7(r3)
			stfd    f8,savefp8(r3)
			stfd    f9,savefp9(r3)
			stfd    f10,savefp10(r3)
			stfd    f11,savefp11(r3)
			stfd    f12,savefp12(r3)
			stfd    f13,savefp13(r3)
			stfd    f14,savefp14(r3)
			stfd    f15,savefp15(r3)
			stfd    f16,savefp16(r3)
			stfd    f17,savefp17(r3)
			stfd    f18,savefp18(r3)
			stfd    f19,savefp19(r3)
			stfd    f20,savefp20(r3)
			stfd    f21,savefp21(r3)
			stfd    f22,savefp22(r3)
			stfd    f23,savefp23(r3)
			stfd    f24,savefp24(r3)
			stfd    f25,savefp25(r3)
			stfd    f26,savefp26(r3)
			stfd    f27,savefp27(r3)
			stfd    f28,savefp28(r3)
			stfd    f29,savefp29(r3)
			stfd    f30,savefp30(r3)
			stfd    f31,savefp31(r3)
            blr
            
// Store FPRs on a 32-byte machine.

fp_st32:
			la		r11,savefp4(r3)				; Point to the 2nd line
			stfd    f0,savefp0(r3)
			dcbz	0,r11						; Allocate cache
			stfd    f1,savefp1(r3)
			stfd    f2,savefp2(r3)
			la		r11,savefp8(r3)				; Point to the 3rd line
			stfd    f3,savefp3(r3)
			dcbz	0,r11						; Allocate cache
			stfd    f4,savefp4(r3)
			stfd    f5,savefp5(r3)
			stfd    f6,savefp6(r3)
			la		r11,savefp12(r3)			; Point to the 4th line
			stfd    f7,savefp7(r3)
			dcbz	0,r11						; Allocate cache
			stfd    f8,savefp8(r3)
			stfd    f9,savefp9(r3)
			stfd    f10,savefp10(r3)
			la		r11,savefp16(r3)			; Point to the 5th line
			stfd    f11,savefp11(r3)
			dcbz	0,r11						; Allocate cache
			stfd    f12,savefp12(r3)
			stfd    f13,savefp13(r3)
			stfd    f14,savefp14(r3)
			la		r11,savefp20(r3)			; Point to the 6th line 
			stfd    f15,savefp15(r3)
			dcbz	0,r11						; Allocate cache
			stfd    f16,savefp16(r3)
			stfd    f17,savefp17(r3)
			stfd    f18,savefp18(r3)
			la		r11,savefp24(r3)			; Point to the 7th line
			stfd    f19,savefp19(r3)
			dcbz	0,r11						; Allocate cache
			stfd    f20,savefp20(r3)

			stfd    f21,savefp21(r3)
			stfd    f22,savefp22(r3)
			la		r11,savefp28(r3)			; Point to the 8th line
			stfd    f23,savefp23(r3)
			dcbz	0,r11						; allocate it
			stfd    f24,savefp24(r3)
			stfd    f25,savefp25(r3)
			stfd    f26,savefp26(r3)
			stfd    f27,savefp27(r3)

			stfd    f28,savefp28(r3)
			stfd    f29,savefp29(r3)
			stfd    f30,savefp30(r3)
			stfd    f31,savefp31(r3)
            blr


// *******************
// * v r _ s t o r e *
// *******************
//
// Store VRs into savearea, according to bits set in passed vrsave bitfield.  This routine is used
// both by vec_save and vec_switch.  In order to minimize conditional branches and touching in
// unnecessary cache blocks, we either save all or none of the VRs in a block.  We have separate paths
// for each cache block size.
//
// When called:
//		interrupts are off, vectors are enabled
//		r3 = ptr to save area
//		r10 = vrsave (not 0)
//
// We destroy:
//		r4 - r11, all CRs.

vr_store:
            mfsprg	r9,2					; get feature flags
			stw		r10,savevrvalid(r3)		; Save the validity information in savearea
			slwi	r8,r10,1				; Shift over 1
            mtcrf	0x02,r9					; put cache line size bits in cr6 where we can test
			or		r8,r10,r8				; r8 <- even bits show which pairs are in use
            bt--	pf32Byteb,vr_st32		; skip if 32-byte cacheline processor

            
; Save vectors on a 128-byte linesize processor.  We save all or none of the 8 registers in each of
; the four cache lines.  This minimizes mispredicted branches yet handles cache lines optimally.

            slwi	r7,r8,2					; shift groups-of-2 over by 2
            li		r4,16					; load offsets for X-form stores
            or		r8,r7,r8				; show if any in group of 4 are in use
            li		r5,32
            slwi	r7,r8,4					; shift groups-of-4 over by 4
            li		r6,48
            or		r11,r7,r8				; show if any in group of 8 are in use
            li		r7,64
            mtcrf	0x80,r11				; set CRs one at a time (faster)
            li		r8,80
            mtcrf	0x20,r11
            li		r9,96
            mtcrf	0x08,r11
            li		r10,112
            mtcrf	0x02,r11
            
            bf		0,vr_st64b				; skip if none of vr0-vr7 are in use
            la		r11,savevr0(r3)			; get address of this group of registers in save area
            dcbz128	0,r11					; zero the line
            stvxl	v0,0,r11				; save 8 VRs in the line
            stvxl	v1,r4,r11
            stvxl	v2,r5,r11
            stvxl	v3,r6,r11
            stvxl	v4,r7,r11
            stvxl	v5,r8,r11
            stvxl	v6,r9,r11
            stvxl	v7,r10,r11
            
vr_st64b:
            bf		8,vr_st64c				; skip if none of vr8-vr15 are in use
            la		r11,savevr8(r3)			; get address of this group of registers in save area
            dcbz128	0,r11					; zero the line
            stvxl	v8,0,r11				; save 8 VRs in the line
            stvxl	v9,r4,r11
            stvxl	v10,r5,r11
            stvxl	v11,r6,r11
            stvxl	v12,r7,r11
            stvxl	v13,r8,r11
            stvxl	v14,r9,r11
            stvxl	v15,r10,r11

vr_st64c:
            bf		16,vr_st64d				; skip if none of vr16-vr23 are in use
            la		r11,savevr16(r3)		; get address of this group of registers in save area
            dcbz128	0,r11					; zero the line
            stvxl	v16,0,r11				; save 8 VRs in the line
            stvxl	v17,r4,r11
            stvxl	v18,r5,r11
            stvxl	v19,r6,r11
            stvxl	v20,r7,r11
            stvxl	v21,r8,r11
            stvxl	v22,r9,r11
            stvxl	v23,r10,r11

vr_st64d:
            bflr	24						; done if none of vr24-vr31 are in use
            la		r11,savevr24(r3)		; get address of this group of registers in save area
            dcbz128	0,r11					; zero the line
            stvxl	v24,0,r11				; save 8 VRs in the line
            stvxl	v25,r4,r11
            stvxl	v26,r5,r11
            stvxl	v27,r6,r11
            stvxl	v28,r7,r11
            stvxl	v29,r8,r11
            stvxl	v30,r9,r11
            stvxl	v31,r10,r11
            blr            
            
; Save vectors on a 32-byte linesize processor.  We save in 16 groups of 2: we either save both
; or neither in each group.  This cuts down on conditional branches.
;			 r8 = bitmask with bit n set (for even n) if either of that pair of VRs is in use
;		     r3 = savearea

vr_st32:
            mtcrf	0xFF,r8					; set CR bits so we can branch on them
            li		r4,16					; load offset for X-form stores

            bf		0,vr_st32b				; skip if neither VR in this pair is in use
            la		r11,savevr0(r3)			; get address of this group of registers in save area
            dcba	0,r11					; establish the line wo reading it
            stvxl	v0,0,r11				; save the two VRs in the line
            stvxl	v1,r4,r11

vr_st32b:
            bf		2,vr_st32c				; skip if neither VR in this pair is in use
            la		r11,savevr2(r3)			; get address of this group of registers in save area
            dcba	0,r11					; establish the line wo reading it
            stvxl	v2,0,r11				; save the two VRs in the line
            stvxl	v3,r4,r11

vr_st32c:
            bf		4,vr_st32d				; skip if neither VR in this pair is in use
            la		r11,savevr4(r3)			; get address of this group of registers in save area
            dcba	0,r11					; establish the line wo reading it
            stvxl	v4,0,r11				; save the two VRs in the line
            stvxl	v5,r4,r11

vr_st32d:
            bf		6,vr_st32e				; skip if neither VR in this pair is in use
            la		r11,savevr6(r3)			; get address of this group of registers in save area
            dcba	0,r11					; establish the line wo reading it
            stvxl	v6,0,r11				; save the two VRs in the line
            stvxl	v7,r4,r11

vr_st32e:
            bf		8,vr_st32f				; skip if neither VR in this pair is in use
            la		r11,savevr8(r3)			; get address of this group of registers in save area
            dcba	0,r11					; establish the line wo reading it
            stvxl	v8,0,r11				; save the two VRs in the line
            stvxl	v9,r4,r11

vr_st32f:
            bf		10,vr_st32g				; skip if neither VR in this pair is in use
            la		r11,savevr10(r3)		; get address of this group of registers in save area
            dcba	0,r11					; establish the line wo reading it
            stvxl	v10,0,r11				; save the two VRs in the line
            stvxl	v11,r4,r11

vr_st32g:
            bf		12,vr_st32h				; skip if neither VR in this pair is in use
            la		r11,savevr12(r3)		; get address of this group of registers in save area
            dcba	0,r11					; establish the line wo reading it
            stvxl	v12,0,r11				; save the two VRs in the line
            stvxl	v13,r4,r11

vr_st32h:
            bf		14,vr_st32i				; skip if neither VR in this pair is in use
            la		r11,savevr14(r3)		; get address of this group of registers in save area
            dcba	0,r11					; establish the line wo reading it
            stvxl	v14,0,r11				; save the two VRs in the line
            stvxl	v15,r4,r11

vr_st32i:
            bf		16,vr_st32j				; skip if neither VR in this pair is in use
            la		r11,savevr16(r3)		; get address of this group of registers in save area
            dcba	0,r11					; establish the line wo reading it
            stvxl	v16,0,r11				; save the two VRs in the line
            stvxl	v17,r4,r11

vr_st32j:
            bf		18,vr_st32k				; skip if neither VR in this pair is in use
            la		r11,savevr18(r3)		; get address of this group of registers in save area
            dcba	0,r11					; establish the line wo reading it
            stvxl	v18,0,r11				; save the two VRs in the line
            stvxl	v19,r4,r11

vr_st32k:
            bf		20,vr_st32l				; skip if neither VR in this pair is in use
            la		r11,savevr20(r3)		; get address of this group of registers in save area
            dcba	0,r11					; establish the line wo reading it
            stvxl	v20,0,r11				; save the two VRs in the line
            stvxl	v21,r4,r11

vr_st32l:
            bf		22,vr_st32m				; skip if neither VR in this pair is in use
            la		r11,savevr22(r3)		; get address of this group of registers in save area
            dcba	0,r11					; establish the line wo reading it
            stvxl	v22,0,r11				; save the two VRs in the line
            stvxl	v23,r4,r11

vr_st32m:
            bf		24,vr_st32n				; skip if neither VR in this pair is in use
            la		r11,savevr24(r3)		; get address of this group of registers in save area
            dcba	0,r11					; establish the line wo reading it
            stvxl	v24,0,r11				; save the two VRs in the line
            stvxl	v25,r4,r11

vr_st32n:
            bf		26,vr_st32o				; skip if neither VR in this pair is in use
            la		r11,savevr26(r3)		; get address of this group of registers in save area
            dcba	0,r11					; establish the line wo reading it
            stvxl	v26,0,r11				; save the two VRs in the line
            stvxl	v27,r4,r11

vr_st32o:
            bf		28,vr_st32p				; skip if neither VR in this pair is in use
            la		r11,savevr28(r3)		; get address of this group of registers in save area
            dcba	0,r11					; establish the line wo reading it
            stvxl	v28,0,r11				; save the two VRs in the line
            stvxl	v29,r4,r11

vr_st32p:
            bflr	30						; done if neither VR in this pair is in use
            la		r11,savevr30(r3)		; get address of this group of registers in save area
            dcba	0,r11					; establish the line wo reading it
            stvxl	v30,0,r11				; save the two VRs in the line
            stvxl	v31,r4,r11
            blr


// *****************
// * v r _ l o a d *
// *****************
//
// Load live VRs from a savearea, according to bits set in a passed vector.  This is the reverse
// of "vr_store".  Like it, we avoid touching unnecessary cache blocks and minimize conditional
// branches by loading all VRs from a cache line, if we have to load any.  If we don't load the VRs
// in a cache line, we bug them.  Note that this behavior is slightly different from earlier kernels,
// which would bug all VRs that aren't live.
//
// When called:
//		interrupts are off, vectors are enabled
//		r3 = ptr to save area
//		r10 = vector of live regs to load (ie, savevrsave & savevrvalid, may be 0)
//		v31 = bugbug constant (0x7FFFDEAD7FFFDEAD7FFFDEAD7FFFDEAD)
//
// We destroy:
//		r4 - r11, all CRs.

vr_load:
            mfsprg	r9,2					; get feature flags
            li		r6,1					; assuming 32-byte, get (#VRs)-1 in a cacheline
            mtcrf	0x02,r9					; set cache line size bits in cr6
            lis		r7,0xC000				; assuming 32-byte, set bits 0-1
            bt--	pf32Byteb,vr_ld0		; skip if 32-bit processor
            li		r6,7					; 128-byte machines have 8 VRs in a cacheline
            lis		r7,0xFF00				; so set bits 0-7
            
// Loop touching in cache blocks we will load from.
//		r3 = savearea ptr
//		r5 = we light bits for the VRs we will be loading
//		r6 = 1 if 32-byte, 7 if 128-byte
//		r7 = 0xC0000000 if 32-byte, 0xFF000000 if 128-byte
//		r10 = live VR bits
//		v31 = bugbug constant

vr_ld0:
            li		r5,0					; initialize set of VRs to load
            la		r11,savevr0(r3)			; get address of register file
            b		vr_ld2					; enter loop in middle
            
            .align	5
vr_ld1:										; loop over each cache line we will load
            dcbt	r4,r11					; start prefetch of the line
            andc	r10,r10,r9				; turn off the bits in this line
            or		r5,r5,r9				; we will load all these
vr_ld2:										; initial entry pt
            cntlzw	r4,r10					; get offset to next live VR
            andc	r4,r4,r6				; cacheline align it
            srw.	r9,r7,r4				; position bits for VRs in that cache line
            slwi	r4,r4,4					; get byte offset within register file to that line
            bne		vr_ld1					; loop if more bits in r10
            
            bf--	pf128Byteb,vr_ld32		; skip if not 128-byte lines

// Handle a processor with 128-byte cache lines.  Four groups of 8 VRs.
//		r3 = savearea ptr
//		r5 = 1st bit in each cacheline is 1 iff any reg in that line must be loaded
//		r11 = addr(savevr0)
//		v31 = bugbug constant

            mtcrf	0x80,r5					; set up bits for conditional branches
            li		r4,16					; load offsets for X-form stores
            li		r6,48
            mtcrf	0x20,r5					; load CRs ona at a time, which is faster
            li		r7,64
            li		r8,80
            mtcrf	0x08,r5
            li		r9,96
            li		r10,112
            mtcrf	0x02,r5
            li		r5,32
            
            bt		0,vr_ld128a				; skip if this line must be loaded
            vor		v0,v31,v31				; no VR must be loaded, so bug them all
            vor		v1,v31,v31
            vor		v2,v31,v31
            vor		v3,v31,v31
            vor		v4,v31,v31
            vor		v5,v31,v31
            vor		v6,v31,v31
            vor		v7,v31,v31
            b		vr_ld128b
vr_ld128a:									; must load from this line
            lvxl	v0,0,r11
            lvxl	v1,r4,r11
            lvxl	v2,r5,r11
            lvxl	v3,r6,r11
            lvxl	v4,r7,r11
            lvxl	v5,r8,r11
            lvxl	v6,r9,r11
            lvxl	v7,r10,r11
            
vr_ld128b:   								; here to handle next cache line         
            la		r11,savevr8(r3)			; load offset to it
            bt		8,vr_ld128c				; skip if this line must be loaded
            vor		v8,v31,v31				; no VR must be loaded, so bug them all
            vor		v9,v31,v31
            vor		v10,v31,v31
            vor		v11,v31,v31
            vor		v12,v31,v31
            vor		v13,v31,v31
            vor		v14,v31,v31
            vor		v15,v31,v31
            b		vr_ld128d
vr_ld128c:									; must load from this line
            lvxl	v8,0,r11
            lvxl	v9,r4,r11
            lvxl	v10,r5,r11
            lvxl	v11,r6,r11
            lvxl	v12,r7,r11
            lvxl	v13,r8,r11
            lvxl	v14,r9,r11
            lvxl	v15,r10,r11
            
vr_ld128d:   								; here to handle next cache line         
            la		r11,savevr16(r3)		; load offset to it
            bt		16,vr_ld128e			; skip if this line must be loaded
            vor		v16,v31,v31				; no VR must be loaded, so bug them all
            vor		v17,v31,v31
            vor		v18,v31,v31
            vor		v19,v31,v31
            vor		v20,v31,v31
            vor		v21,v31,v31
            vor		v22,v31,v31
            vor		v23,v31,v31
            b		vr_ld128f
vr_ld128e:									; must load from this line
            lvxl	v16,0,r11
            lvxl	v17,r4,r11
            lvxl	v18,r5,r11
            lvxl	v19,r6,r11
            lvxl	v20,r7,r11
            lvxl	v21,r8,r11
            lvxl	v22,r9,r11
            lvxl	v23,r10,r11
            
vr_ld128f:   								; here to handle next cache line         
            la		r11,savevr24(r3)		; load offset to it
            bt		24,vr_ld128g			; skip if this line must be loaded
            vor		v24,v31,v31				; no VR must be loaded, so bug them all
            vor		v25,v31,v31
            vor		v26,v31,v31
            vor		v27,v31,v31
            vor		v28,v31,v31
            vor		v29,v31,v31
            vor		v30,v31,v31
            blr
vr_ld128g:									; must load from this line
            lvxl	v24,0,r11
            lvxl	v25,r4,r11
            lvxl	v26,r5,r11
            lvxl	v27,r6,r11
            lvxl	v28,r7,r11
            lvxl	v29,r8,r11
            lvxl	v30,r9,r11
            lvxl	v31,r10,r11
            blr
            
// Handle a processor with 32-byte cache lines.  Sixteen groups of two VRs.
//		r5 = 1st bit in each cacheline is 1 iff any reg in that line must be loaded
//		r11 = addr(savevr0)

vr_ld32:
            mtcrf	0xFF,r5					; set up bits for conditional branches
            li		r4,16					; load offset for X-form stores
            
            bt		0,vr_ld32load0			; skip if we must load this line
            vor		v0,v31,v31				; neither VR is live, so bug them both
            vor		v1,v31,v31
            b		vr_ld32test2
vr_ld32load0:								; must load VRs in this line
            lvxl	v0,0,r11
            lvxl	v1,r4,r11
            
vr_ld32test2:								; here to handle next cache line
            la		r11,savevr2(r3)			; get offset to next cache line
            bt		2,vr_ld32load2			; skip if we must load this line
            vor		v2,v31,v31				; neither VR is live, so bug them both
            vor		v3,v31,v31
            b		vr_ld32test4
vr_ld32load2:								; must load VRs in this line
            lvxl	v2,0,r11
            lvxl	v3,r4,r11
            
vr_ld32test4:								; here to handle next cache line
            la		r11,savevr4(r3)			; get offset to next cache line
            bt		4,vr_ld32load4			; skip if we must load this line
            vor		v4,v31,v31				; neither VR is live, so bug them both
            vor		v5,v31,v31
            b		vr_ld32test6
vr_ld32load4:								; must load VRs in this line
            lvxl	v4,0,r11
            lvxl	v5,r4,r11
            
vr_ld32test6:								; here to handle next cache line
            la		r11,savevr6(r3)			; get offset to next cache line
            bt		6,vr_ld32load6			; skip if we must load this line
            vor		v6,v31,v31				; neither VR is live, so bug them both
            vor		v7,v31,v31
            b		vr_ld32test8
vr_ld32load6:								; must load VRs in this line
            lvxl	v6,0,r11
            lvxl	v7,r4,r11
            
vr_ld32test8:								; here to handle next cache line
            la		r11,savevr8(r3)			; get offset to next cache line
            bt		8,vr_ld32load8			; skip if we must load this line
            vor		v8,v31,v31				; neither VR is live, so bug them both
            vor		v9,v31,v31
            b		vr_ld32test10
vr_ld32load8:								; must load VRs in this line
            lvxl	v8,0,r11
            lvxl	v9,r4,r11
            
vr_ld32test10:								; here to handle next cache line
            la		r11,savevr10(r3)		; get offset to next cache line
            bt		10,vr_ld32load10		; skip if we must load this line
            vor		v10,v31,v31				; neither VR is live, so bug them both
            vor		v11,v31,v31
            b		vr_ld32test12
vr_ld32load10:								; must load VRs in this line
            lvxl	v10,0,r11
            lvxl	v11,r4,r11
            
vr_ld32test12:								; here to handle next cache line
            la		r11,savevr12(r3)		; get offset to next cache line
            bt		12,vr_ld32load12		; skip if we must load this line
            vor		v12,v31,v31				; neither VR is live, so bug them both
            vor		v13,v31,v31
            b		vr_ld32test14
vr_ld32load12:								; must load VRs in this line
            lvxl	v12,0,r11
            lvxl	v13,r4,r11
            
vr_ld32test14:								; here to handle next cache line
            la		r11,savevr14(r3)		; get offset to next cache line
            bt		14,vr_ld32load14		; skip if we must load this line
            vor		v14,v31,v31				; neither VR is live, so bug them both
            vor		v15,v31,v31
            b		vr_ld32test16
vr_ld32load14:								; must load VRs in this line
            lvxl	v14,0,r11
            lvxl	v15,r4,r11
            
vr_ld32test16:								; here to handle next cache line
            la		r11,savevr16(r3)		; get offset to next cache line
            bt		16,vr_ld32load16		; skip if we must load this line
            vor		v16,v31,v31				; neither VR is live, so bug them both
            vor		v17,v31,v31
            b		vr_ld32test18
vr_ld32load16:								; must load VRs in this line
            lvxl	v16,0,r11
            lvxl	v17,r4,r11
            
vr_ld32test18:								; here to handle next cache line
            la		r11,savevr18(r3)		; get offset to next cache line
            bt		18,vr_ld32load18		; skip if we must load this line
            vor		v18,v31,v31				; neither VR is live, so bug them both
            vor		v19,v31,v31
            b		vr_ld32test20
vr_ld32load18:								; must load VRs in this line
            lvxl	v18,0,r11
            lvxl	v19,r4,r11
            
vr_ld32test20:								; here to handle next cache line
            la		r11,savevr20(r3)		; get offset to next cache line
            bt		20,vr_ld32load20		; skip if we must load this line
            vor		v20,v31,v31				; neither VR is live, so bug them both
            vor		v21,v31,v31
            b		vr_ld32test22
vr_ld32load20:								; must load VRs in this line
            lvxl	v20,0,r11
            lvxl	v21,r4,r11
            
vr_ld32test22:								; here to handle next cache line
            la		r11,savevr22(r3)		; get offset to next cache line
            bt		22,vr_ld32load22		; skip if we must load this line
            vor		v22,v31,v31				; neither VR is live, so bug them both
            vor		v23,v31,v31
            b		vr_ld32test24
vr_ld32load22:								; must load VRs in this line
            lvxl	v22,0,r11
            lvxl	v23,r4,r11
            
vr_ld32test24:								; here to handle next cache line
            la		r11,savevr24(r3)		; get offset to next cache line
            bt		24,vr_ld32load24		; skip if we must load this line
            vor		v24,v31,v31				; neither VR is live, so bug them both
            vor		v25,v31,v31
            b		vr_ld32test26
vr_ld32load24:								; must load VRs in this line
            lvxl	v24,0,r11
            lvxl	v25,r4,r11
            
vr_ld32test26:								; here to handle next cache line
            la		r11,savevr26(r3)		; get offset to next cache line
            bt		26,vr_ld32load26		; skip if we must load this line
            vor		v26,v31,v31				; neither VR is live, so bug them both
            vor		v27,v31,v31
            b		vr_ld32test28
vr_ld32load26:								; must load VRs in this line
            lvxl	v26,0,r11
            lvxl	v27,r4,r11
            
vr_ld32test28:								; here to handle next cache line
            la		r11,savevr28(r3)		; get offset to next cache line
            bt		28,vr_ld32load28		; skip if we must load this line
            vor		v28,v31,v31				; neither VR is live, so bug them both
            vor		v29,v31,v31
            b		vr_ld32test30
vr_ld32load28:								; must load VRs in this line
            lvxl	v28,0,r11
            lvxl	v29,r4,r11
            
vr_ld32test30:								; here to handle next cache line
            la		r11,savevr30(r3)		; get offset to next cache line
            bt		30,vr_ld32load30		; skip if we must load this line
            vor		v30,v31,v31				; neither VR is live, so bug them both
            blr
vr_ld32load30:								; must load VRs in this line
            lvxl	v30,0,r11
            lvxl	v31,r4,r11
            blr

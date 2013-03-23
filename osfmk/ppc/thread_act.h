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
 * 
 */

#ifndef	_PPC_THREAD_ACT_H_
#define	_PPC_THREAD_ACT_H_

#include <mach_kgdb.h>
#include <mach/boolean.h>
#include <mach/ppc/vm_types.h>
#include <mach/thread_status.h>
#include <kern/lock.h>
#include <kern/clock.h>
#include <ppc/savearea.h>

/*
 * Kernel state structure
 *
 * This holds the kernel state that is saved and restored across context
 * switches. 
 */

/*
 * PPC process control block
 *
 * The PCB holds normal context.  It does not contain vector or floating point 
 * registers.
 *
 */

typedef struct savearea pcb;
typedef struct savearea *pcb_t;

struct facility_context {

	savearea_fpu	*FPUsave;		/* The floating point savearea */
	savearea		*FPUlevel;		/* The floating point context level */
	unsigned int	FPUcpu;			/* The last processor to enable floating point */
	savearea_vec	*VMXsave;		/* The VMX savearea */
	savearea		*VMXlevel;		/* The VMX context level */
	unsigned int	VMXcpu;			/* The last processor to enable vector */
	struct thread_activation *facAct;	/* Activation associated with context */
};

typedef struct facility_context facility_context;

/*
 * Maps state flavor to number of words in the state:
 */
extern unsigned int state_count[];

#define USER_REGS(ThrAct)	((ThrAct)->mact.pcb)

#define	user_pc(ThrAct)		((ThrAct)->mact.pcb->save_srr0)

#define act_machine_state_ptr(ThrAct)	(thread_state_t)USER_REGS(ThrAct)

typedef struct MachineThrAct {
	/*
	 * pointer to process control block control blocks.  Potentially
	 * one for each active facility context.  They may point to the
	 * same saveareas.
	 */
	savearea		*pcb;			/* The "normal" savearea */
	facility_context *curctx;		/* Current facility context */
	facility_context *deferctx;		/* Deferred facility context */
	facility_context facctx;		/* "Normal" facility context */
	struct vmmCntrlEntry *vmmCEntry;	/* Pointer current emulation context or 0 */
	struct vmmCntrlTable *vmmControl;	/* Pointer to virtual machine monitor control table */
	uint64_t		qactTimer;		/* Time thread needs to interrupt. This is a single-shot timer. Zero is unset */
	unsigned int	ksp;			/* points to TOP OF STACK or zero */
	unsigned int	bbDescAddr;		/* Points to Blue Box Trap descriptor area in kernel (page aligned) */
	unsigned int	bbUserDA;		/* Points to Blue Box Trap descriptor area in user (page aligned) */
	unsigned int	bbTableStart;	/* Points to Blue Box Trap dispatch area in user */
	unsigned int	emPendRupts;	/* Number of pending emulated interruptions */
	unsigned int	bbTaskID;		/* Opaque task ID for Blue Box threads */
	unsigned int	bbTaskEnv;		/* Opaque task data reference for Blue Box threads */
	unsigned int	specFlags;		/* Special flags */

/* special flags bits */

#define ignoreZeroFaultbit		0
#define floatUsedbit			1
#define vectorUsedbit			2
#define runningVMbit			4
#define floatCngbit				5
#define vectorCngbit			6
#define timerPopbit				7
#define userProtKeybit			8
#define trapUnalignbit			9
#define notifyUnalignbit		10
#define FamVMenabit			11
#define FamVMmodebit			12
/*	NOTE: Do not move or assign bit 31 without changing exception vector ultra fast path code */
#define bbThreadbit				28
#define bbNoMachSCbit	 		29
#define bbPreemptivebit			30
#define spfReserved1			31	/* See note above */

#define ignoreZeroFault		(1<<(31-ignoreZeroFaultbit))
#define floatUsed			(1<<(31-floatUsedbit))
#define vectorUsed			(1<<(31-vectorUsedbit))
#define runningVM			(1<<(31-runningVMbit))
#define floatCng			(1<<(31-floatCngbit))
#define vectorCng			(1<<(31-vectorCngbit))
#define timerPop			(1<<(31-timerPopbit))
#define userProtKey			(1<<(31-userProtKeybit))
#define trapUnalign			(1<<(31-trapUnalignbit))
#define notifyUnalign		(1<<(31-notifyUnalignbit))
#define	FamVMena			(1<<(31-FamVMenabit))
#define	FamVMmode			(1<<(31-FamVMmodebit))
#define bbThread			(1<<(31-bbThreadbit))
#define bbNoMachSC			(1<<(31-bbNoMachSCbit))
#define bbPreemptive		(1<<(31-bbPreemptivebit))

#define fvChkb 0
#define fvChk 0x80000000

#ifdef	MACH_BSD
	unsigned long	cthread_self;	/* for use of cthread package */
#endif

} MachineThrAct, *MachineThrAct_t;

extern struct savearea *find_user_regs(thread_act_t act);
extern struct savearea *get_user_regs(thread_act_t);
extern struct savearea_fpu *find_user_fpu(thread_act_t act);
extern struct savearea_vec *find_user_vec(thread_act_t act);
extern int thread_enable_fpe(thread_act_t act, int onoff);

extern void *act_thread_csave(void);
extern void act_thread_catt(void *ctx);
extern void act_thread_cfree(void *ctx);

#define	current_act_fast()	current_act()

#endif	/* _PPC_THREAD_ACT_H_ */

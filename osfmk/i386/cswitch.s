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
 * @OSF_COPYRIGHT@
 */
/* 
 * Mach Operating System
 * Copyright (c) 1991,1990 Carnegie Mellon University
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

#include <platforms.h>

#include <i386/asm.h>
#include <i386/proc_reg.h>
#include <assym.s>

#ifdef	SYMMETRY
#include <sqt/asm_macros.h>
#endif

#if	AT386
#include <i386/mp.h>
#endif	/* AT386 */

#define	CX(addr, reg)	addr(,reg,4)

/*
 * Context switch routines for i386.
 */

Entry(Load_context)
	movl	S_ARG0,%ecx			/* get thread */
	movl	TH_KERNEL_STACK(%ecx),%ecx	/* get kernel stack */
	lea	KERNEL_STACK_SIZE-IKS_SIZE-IEL_SIZE(%ecx),%edx
						/* point to stack top */
	movl	%ecx,%gs:CPU_ACTIVE_STACK	/* store stack address */
	movl	%edx,%gs:CPU_KERNEL_STACK	/* store stack top */

	movl	%edx,%esp
	movl	%edx,%ebp

	xorl	%eax,%eax			/* return zero (no old thread) */
	pushl	%eax
	call	EXT(thread_continue)

/*
 *	This really only has to save registers
 *	when there is no explicit continuation.
 */

Entry(Switch_context)
	movl	%gs:CPU_ACTIVE_STACK,%ecx /* get old kernel stack */

	movl	%ebx,KSS_EBX(%ecx)		/* save registers */
	movl	%ebp,KSS_EBP(%ecx)
	movl	%edi,KSS_EDI(%ecx)
	movl	%esi,KSS_ESI(%ecx)
	popl	KSS_EIP(%ecx)			/* save return PC */
	movl	%esp,KSS_ESP(%ecx)		/* save SP */

	movl	0(%esp),%eax			/* return old thread */
	movl	8(%esp),%ebx			/* get new thread */
	movl    %ebx,%gs:CPU_ACTIVE_THREAD                /* new thread is active */
	movl	TH_KERNEL_STACK(%ebx),%ecx	/* get its kernel stack */
	lea	KERNEL_STACK_SIZE-IKS_SIZE-IEL_SIZE(%ecx),%ebx
						/* point to stack top */

	movl	%ecx,%gs:CPU_ACTIVE_STACK	/* set current stack */
	movl	%ebx,%gs:CPU_KERNEL_STACK	/* set stack top */


	movl	$0,%gs:CPU_ACTIVE_KLOADED

	movl	KSS_ESP(%ecx),%esp		/* switch stacks */
	movl	KSS_ESI(%ecx),%esi		/* restore registers */
	movl	KSS_EDI(%ecx),%edi
	movl	KSS_EBP(%ecx),%ebp
	movl	KSS_EBX(%ecx),%ebx
	jmp	*KSS_EIP(%ecx)			/* return old thread */

Entry(Thread_continue)
	pushl	%eax				/* push the thread argument */
	xorl	%ebp,%ebp			/* zero frame pointer */
	call	*%ebx				/* call real continuation */

/*
 * void machine_processor_shutdown(thread_t thread,
 *				   void (*routine)(processor_t),
 *				   processor_t processor)
 *
 * saves the kernel context of the thread,
 * switches to the interrupt stack,
 * continues the thread (with thread_continue),
 * then runs routine on the interrupt stack.
 *
 * Assumes that the thread is a kernel thread (thus
 * has no FPU state)
 */
Entry(machine_processor_shutdown)
	movl	%gs:CPU_ACTIVE_STACK,%ecx /* get old kernel stack */
	movl	%ebx,KSS_EBX(%ecx)		/* save registers */
	movl	%ebp,KSS_EBP(%ecx)
	movl	%edi,KSS_EDI(%ecx)
	movl	%esi,KSS_ESI(%ecx)
	popl	KSS_EIP(%ecx)			/* save return PC */
	movl	%esp,KSS_ESP(%ecx)		/* save SP */

	movl	0(%esp),%eax			/* get old thread */
	movl	%ecx,TH_KERNEL_STACK(%eax)	/* save old stack */
	movl	4(%esp),%ebx			/* get routine to run next */
	movl	8(%esp),%esi			/* get its argument */

	movl	%gs:CPU_INT_STACK_TOP,%esp 	/* switch to interrupt stack */

	pushl	%esi				/* push argument */
	call	*%ebx				/* call routine to run */
	hlt					/* (should never return) */


        .text

	.globl	EXT(locore_end)
LEXT(locore_end)


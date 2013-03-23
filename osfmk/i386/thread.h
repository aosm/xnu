/*
 * Copyright (c) 2000-2006 Apple Computer, Inc. All rights reserved.
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
 * Copyright (c) 1991,1990,1989 Carnegie Mellon University
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
 *	File:	machine/thread.h
 *
 *	This file contains the structure definitions for the thread
 *	state as applied to I386 processors.
 */

#ifndef	_I386_THREAD_H_
#define _I386_THREAD_H_

#include <mach/boolean.h>
#include <mach/i386/vm_types.h>
#include <mach/i386/fp_reg.h>
#include <mach/thread_status.h>

#include <kern/lock.h>

#include <i386/iopb.h>
#include <i386/seg.h>
#include <i386/tss.h>
#include <i386/eflags.h>

#include <i386/cpu_data.h>


/*
 *	i386_saved_state:
 *
 *	Has been exported to servers.  See: mach/i386/thread_status.h
 *
 *	This structure corresponds to the state of user registers
 *	as saved upon kernel entry.  It lives in the pcb.
 *	It is also pushed onto the stack for exceptions in the kernel.
 *	For performance, it is also used directly in syscall exceptions
 *	if the server has requested i386_THREAD_STATE flavor for the exception
 *	port.
 */

/*
 *	Save area for user floating-point state.
 *	Allocated only when necessary.
 */

typedef	enum {
		FXSAVE32 = 1,
		FXSAVE64 = 2,
		XSAVE32  = 3,
		XSAVE64  = 4,
		FP_UNUSED = 5
	} fp_save_layout_t;



/*
 *	x86_kernel_state:
 *
 *	This structure corresponds to the state of kernel registers
 *	as saved in a context-switch.  It lives at the base of the stack.
 */

#ifdef __i386__
struct x86_kernel_state {
	int			k_ebx;	/* kernel context */
	int			k_esp;
	int			k_ebp;
	int			k_edi;
	int			k_esi;
	int			k_eip;
	/*
	 * Kernel stacks are 16-byte aligned with a 4-byte i386_exception_link at
	 * the top, followed by an x86_kernel_state.  After both structs have
	 * been pushed, we want to be 16-byte aligned.  A dummy int gets us there.
	 */
	int			dummy;
};
#else
struct x86_kernel_state {
	unsigned long k_rbx;	/* kernel context */
	unsigned long k_rsp;
	unsigned long k_rbp;
	unsigned long k_r12;
	unsigned long k_r13;
	unsigned long k_r14;
	unsigned long k_r15;
	unsigned long k_rip;
	unsigned long dummy;
};
#endif

typedef struct pcb {
	void			*sf;
	x86_saved_state_t	*iss;
	void			*ifps;
#ifdef	MACH_BSD
	uint64_t	cthread_self;		/* for use of cthread package */
        struct real_descriptor cthread_desc;
	unsigned long  uldt_selector;          /* user ldt selector to set */
	struct real_descriptor uldt_desc;      /* the actual user setable ldt data */
#endif
	decl_simple_lock_data(,lock);
	uint64_t	iss_pte0;
	uint64_t	iss_pte1;
	void		*ids;
	uint32_t	arg_store_valid;
} *pcb_t;

/*
 * Maps state flavor to number of words in the state:
 */
__private_extern__ unsigned int _MachineStateCount[];

#define USER_STATE(ThrAct)	((ThrAct)->machine.pcb->iss)
#define USER_REGS32(ThrAct)	(saved_state32(USER_STATE(ThrAct)))
#define USER_REGS64(ThrAct)	(saved_state64(USER_STATE(ThrAct)))

#define	user_pc(ThrAct)		(is_saved_state32(USER_STATE(ThrAct)) ?	\
					USER_REGS32(ThrAct)->eip :	\
					USER_REGS64(ThrAct)->isf.rip )


struct machine_thread {
	/*
	 * pointer to process control block
	 *	(actual storage may as well be here, too)
	 */
	struct pcb xxx_pcb;
	pcb_t pcb;

	uint32_t	specFlags;
#define		OnProc		0x1
#define		CopyIOActive 	0x2 /* Checked to ensure DTrace actions do not re-enter copyio(). */
  
#if NCOPY_WINDOWS > 0

        struct {
	        user_addr_t	user_base;
	} copy_window[NCOPY_WINDOWS];
        int		nxt_window;
        int		copyio_state;
#define		WINDOWS_DIRTY	0
#define		WINDOWS_CLEAN	1
#define		WINDOWS_CLOSED	2
#define		WINDOWS_OPENED	3
        uint64_t	physwindow_pte;
        int		physwindow_busy;
#endif
};


extern void *get_user_regs(thread_t);

extern void *act_thread_csave(void);
extern void act_thread_catt(void *ctx);
extern void act_thread_cfree(void *ctx);

/*
 *	i386_exception_link:
 *
 *	This structure lives at the high end of the kernel stack.
 *	It points to the current thread`s user registers.
 */
struct i386_exception_link {
	x86_saved_state_t	*saved_state;
};


/*
 *	On the kernel stack is:
 *	stack:	...
 *		struct i386_exception_link (pointer to user state)
 *		struct x86_kernel_state
 *	stack+kernel_stack_size
 */

#define STACK_IKS(stack)	\
	((struct x86_kernel_state *)((stack) + kernel_stack_size) - 1)
#define STACK_IEL(stack)	\
	((struct i386_exception_link *)STACK_IKS(stack) - 1)

/*
 * Return the current stack depth
 * including x86_kernel_state and i386_exception_link
 */
static inline vm_offset_t
current_stack_depth(void)
{
	vm_offset_t	stack_ptr;

	assert(get_preemption_level() > 0 || !ml_get_interrupts_enabled());

#if defined(__x86_64__)
       __asm__ volatile("mov %%rsp, %0" : "=m" (stack_ptr));
#else
       __asm__ volatile("mov %%esp, %0" : "=m" (stack_ptr));
#endif
	return (current_cpu_datap()->cpu_kernel_stack
		+ sizeof(struct x86_kernel_state)
		+ sizeof(struct i386_exception_link *)
		- stack_ptr); 
}

/*
 * Return address of the function that called current function, given
 *	address of the first parameter of current function.
 */
#define	GET_RETURN_PC(addr)	(__builtin_return_address(0))

/*
 * Defining this indicates that MD code will supply an exception()
 * routine, conformant with kern/exception.c (dependency alert!)
 * but which does wonderfully fast, machine-dependent magic.
 */
#define MACHINE_FAST_EXCEPTION 1

#endif	/* _I386_THREAD_H_ */

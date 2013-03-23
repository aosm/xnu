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
/*
 * @OSF_COPYRIGHT@
 */
/* 
 * Mach Operating System
 * Copyright (c) 1991,1990,1989,1988,1987 Carnegie Mellon University
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
 *
 *	This file contains routines to check whether an ast is needed.
 *
 *	ast_check() - check whether ast is needed for interrupt or context
 *	switch.  Usually called by clock interrupt handler.
 *
 */

#include <cputypes.h>
#include <platforms.h>

#include <kern/ast.h>
#include <kern/counters.h>
#include <kern/cpu_number.h>
#include <kern/misc_protos.h>
#include <kern/queue.h>
#include <kern/sched_prim.h>
#include <kern/thread.h>
#include <kern/processor.h>
#include <kern/spl.h>
#include <kern/wait_queue.h>
#include <mach/policy.h>
#include <machine/trap.h> // for CHUD AST hook
#include <machine/pal_routines.h>


volatile perfASTCallback perfASTHook;


void
ast_init(void)
{
}

/*
 * Called at splsched.
 */
void
ast_taken(
	ast_t		reasons,
	boolean_t	enable
)
{
	boolean_t		preempt_trap = (reasons == AST_PREEMPTION);
	ast_t			*myast = ast_pending();
	thread_t		thread = current_thread();
	perfASTCallback	perf_hook = perfASTHook;

	/*
	 * CHUD hook - all threads including idle processor threads
	 */
	if (perf_hook) {
		if (*myast & AST_CHUD_ALL) {
			(*perf_hook)(reasons, myast);
			
			if (*myast == AST_NONE)
				return;
		}
	}
	else
		*myast &= ~AST_CHUD_ALL;

	reasons &= *myast;
	*myast &= ~reasons;

	/*
	 * Handle ASTs for all threads
	 * except idle processor threads.
	 */
	if (!(thread->state & TH_IDLE)) {
		/*
		 * Check for urgent preemption.
		 */
		if (	(reasons & AST_URGENT)				&&
				wait_queue_assert_possible(thread)		) {
			if (reasons & AST_PREEMPT) {
				counter(c_ast_taken_block++);
				thread_block_reason(THREAD_CONTINUE_NULL, NULL,
										AST_PREEMPT | AST_URGENT);
			}

			reasons &= ~AST_PREEMPTION;
		}

		/*
		 * The kernel preempt traps
		 * skip all other ASTs.
		 */
		if (!preempt_trap) {
			ml_set_interrupts_enabled(enable);

#ifdef	MACH_BSD
			/*
			 * Handle BSD hook.
			 */
			if (reasons & AST_BSD) {
				thread_ast_clear(thread, AST_BSD);
				bsd_ast(thread);
			}
#endif

			/* 
			 * Thread APC hook.
			 */
			if (reasons & AST_APC)
				act_execute_returnhandlers();

			ml_set_interrupts_enabled(FALSE);

			/* 
			 * Check for preemption.
			 */
			if (reasons & AST_PREEMPT)
				reasons = csw_check(current_processor());

			if (	(reasons & AST_PREEMPT)				&&
					wait_queue_assert_possible(thread)		) {		
				counter(c_ast_taken_block++);
				thread_block_reason((thread_continue_t)thread_exception_return, NULL, AST_PREEMPT);
			}
		}
	}

	ml_set_interrupts_enabled(enable);
}

/*
 * Called at splsched.
 */
void
ast_check(
	processor_t		processor)
{
	thread_t			thread = processor->active_thread;

	processor->current_pri = thread->sched_pri;
	processor->current_thmode = thread->sched_mode;
	if (	processor->state == PROCESSOR_RUNNING		||
			processor->state == PROCESSOR_SHUTDOWN		) {
		ast_t			preempt;

		/*
		 *	Propagate thread ast to processor.
		 */
		pal_ast_check(thread);

		ast_propagate(thread->ast);

		/*
		 *	Context switch check.
		 */
		if ((preempt = csw_check(processor)) != AST_NONE)
			ast_on(preempt);
	}
}

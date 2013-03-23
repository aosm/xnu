/*
 * Copyright (c) 2000-2004 Apple Computer, Inc. All rights reserved.
 *
 * @APPLE_LICENSE_OSREFERENCE_HEADER_START@
 * 
 * This file contains Original Code and/or Modifications of Original Code 
 * as defined in and that are subject to the Apple Public Source License 
 * Version 2.0 (the 'License'). You may not use this file except in 
 * compliance with the License.  The rights granted to you under the 
 * License may not be used to create, or enable the creation or 
 * redistribution of, unlawful or unlicensed copies of an Apple operating 
 * system, or to circumvent, violate, or enable the circumvention or 
 * violation of, any terms of an Apple operating system software license 
 * agreement.
 *
 * Please obtain a copy of the License at 
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
 * @APPLE_LICENSE_OSREFERENCE_HEADER_END@
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
 *	File:	clock_prim.c
 *	Author:	Avadis Tevanian, Jr.
 *	Date:	1986
 *
 *	Clock primitives.
 */
#include <mach_prof.h>
#include <gprof.h>

#include <mach/boolean.h>
#include <mach/machine.h>
#include <mach/time_value.h>
#include <mach/vm_param.h>
#include <mach/vm_prot.h>
#include <kern/clock.h>
#include <kern/counters.h>
#include <kern/cpu_number.h>
#include <kern/host.h>
#include <kern/lock.h>
#include <kern/mach_param.h>
#include <kern/misc_protos.h>
#include <kern/processor.h>
#include <kern/profile.h>
#include <kern/sched.h>
#include <kern/sched_prim.h>
#include <kern/spl.h>
#include <kern/thread.h>
#include <vm/vm_kern.h>					/* kernel_map */

#include <mach/clock_server.h>
#include <mach/clock_priv_server.h>
#include <mach/mach_host_server.h>

#include <profiling/profile-mk.h>

boolean_t	profile_kernel_services = TRUE;	/* Indicates wether or not we
											 * account kernel services 

											 * samples for user task */
#ifdef MACH_BSD
extern void	bsd_hardclock(
			boolean_t	usermode,
			natural_t	pc,
			int		numticks);
#endif /* MACH_BSD */

/*
 * Hertz rate clock interrupt servicing. Primarily used to
 * update CPU statistics, recompute thread priority, and to
 * do profiling
 */
void
hertz_tick(
#if	STAT_TIME
	natural_t		ticks,
#endif	/* STAT_TIME */
	boolean_t		usermode,
	natural_t		pc)
{
	processor_t		processor = current_processor();
	thread_t		thread = current_thread();
	int				state;
#if		MACH_PROF
#ifdef	__MACHO__
#define	ETEXT		etext
	extern long		etext;
#else
#define	ETEXT		&etext
	extern char		etext;
#endif
	boolean_t		inkernel;
#endif	/* MACH_PROF */
#if GPROF
	struct profile_vars	*pv;
	prof_uptrint_t		s;
#endif

#ifdef	lint
	pc++;
#endif	/* lint */

	/*
	 *	The system startup sequence initializes the clock
	 *	before kicking off threads.   So it's possible,
	 *	especially when debugging, to wind up here with
	 *	no thread to bill against.  So ignore the tick.
	 */
	if (thread == THREAD_NULL)
		return;

#if		MACH_PROF
	inkernel = !usermode && (pc < (unsigned int)ETEXT);
#endif	/* MACH_PROF */

	/*
	 * Hertz processing performed by all processors
	 * includes statistics gathering, state tracking,
	 * and quantum updating.
	 */
	counter(c_clock_ticks++);

#if     GPROF
	pv = PROFILE_VARS(cpu_number());
#endif

	if (usermode) {
		TIMER_BUMP(&thread->user_timer, ticks);
		if (thread->priority < BASEPRI_DEFAULT)
			state = CPU_STATE_NICE;
		else
			state = CPU_STATE_USER;
#if GPROF
			if (pv->active)
			    PROF_CNT_INC(pv->stats.user_ticks);
#endif
	}
	else {
		TIMER_BUMP(&thread->system_timer, ticks);

		state = processor->state;
		if (	state == PROCESSOR_IDLE			||
				state == PROCESSOR_DISPATCHING)
			state = CPU_STATE_IDLE;
		else
		if (thread->options & TH_OPT_DELAYIDLE)
			state = CPU_STATE_IDLE;
		else
			state = CPU_STATE_SYSTEM;
#if GPROF
		if (pv->active) {
			if (state == CPU_STATE_SYSTEM)
				PROF_CNT_INC(pv->stats.kernel_ticks);
			else
				PROF_CNT_INC(pv->stats.idle_ticks);

			if ((prof_uptrint_t)pc < _profile_vars.profil_info.lowpc)
				PROF_CNT_INC(pv->stats.too_low);
			else {
				s = (prof_uptrint_t)pc - _profile_vars.profil_info.lowpc;
				if (s < pv->profil_info.text_len) {
					LHISTCOUNTER *ptr = (LHISTCOUNTER *) pv->profil_buf;
					LPROF_CNT_INC(ptr[s / HISTFRACTION]);
				}
				else
					PROF_CNT_INC(pv->stats.too_high);
			}
		}
#endif
	}

	PROCESSOR_DATA(processor, cpu_ticks[state]++);

#ifdef MACH_BSD
	/*XXX*/
	if (processor == master_processor) {
		bsd_hardclock(usermode, pc, 1);
	}
	/*XXX*/
#endif /* MACH_BSD */

#if	MACH_PROF
	if (thread->act_profiled) {
		if (inkernel && thread->map != kernel_map) {
			/* 
			 * Non-kernel thread running in kernel
			 * Register user pc (mach_msg, vm_allocate ...)
			 */
		  	if (profile_kernel_services)
		  		profile(user_pc(thread), thread->profil_buffer);
		}
		else
			/*
			 * User thread and user mode or
			 * user (server) thread in kernel-loaded server or
			 * kernel thread and kernel mode
			 * register interrupted pc
			 */
			profile(pc, thread->profil_buffer);
	}
	if (kernel_task->task_profiled) {
		if (inkernel && thread->map != kernel_map)
		  	/*
			 * User thread not profiled in kernel mode,
			 * kernel task profiled, register kernel pc
			 * for kernel task
			 */
			profile(pc, kernel_task->profil_buffer);
	}
#endif	/* MACH_PROF */
}

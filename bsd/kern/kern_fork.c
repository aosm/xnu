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
/* Copyright (c) 1995, 1997 Apple Computer, Inc. All Rights Reserved */
/*
 * Copyright (c) 1982, 1986, 1989, 1991, 1993
 *	The Regents of the University of California.  All rights reserved.
 * (c) UNIX System Laboratories, Inc.
 * All or some portions of this file are derived from material licensed
 * to the University of California by American Telephone and Telegraph
 * Co. or Unix System Laboratories, Inc. and are reproduced herein with
 * the permission of UNIX System Laboratories, Inc.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. All advertising materials mentioning features or use of this software
 *    must display the following acknowledgement:
 *	This product includes software developed by the University of
 *	California, Berkeley and its contributors.
 * 4. Neither the name of the University nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE REGENTS AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 *	@(#)kern_fork.c	8.8 (Berkeley) 2/14/95
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/filedesc.h>
#include <sys/kernel.h>
#include <sys/malloc.h>
#include <sys/proc.h>
#include <sys/user.h>
#include <sys/resourcevar.h>
#include <sys/vnode.h>
#include <sys/file.h>
#include <sys/acct.h>
#include <sys/ktrace.h>

#include <mach/mach_types.h>
#include <kern/mach_param.h>

#include <machine/spl.h>

thread_t cloneproc(struct proc *, int); 
thread_t procdup();

#define	DOFORK	0x1	/* fork() system call */
#define	DOVFORK	0x2	/* vfork() system call */
static int fork1(struct proc *, long, register_t *);

/*
 * fork system call.
 */
int
fork(p, uap, retval)
	struct proc *p;
	void *uap;
	register_t *retval;
{
	return (fork1(p, (long)DOFORK, retval));
}

/*
 * vfork system call
 */
int
vfork(p, uap, retval)
	struct proc *p;
	void *uap;
	register_t *retval;
{
	return (fork1(p, (long)DOVFORK, retval));
}

static int
fork1(p1, flags, retval)
	struct proc *p1;
	long flags;
	register_t *retval;
{
	register struct proc *p2;
	register uid_t uid;
	thread_t newth, self = current_thread();
	int s, count;
        task_t t;

	/*
	 * Although process entries are dynamically created, we still keep
	 * a global limit on the maximum number we will create.  Don't allow
	 * a nonprivileged user to use the last process; don't let root
	 * exceed the limit. The variable nprocs is the current number of
	 * processes, maxproc is the limit.
	 */
	uid = p1->p_cred->p_ruid;
	if ((nprocs >= maxproc - 1 && uid != 0) || nprocs >= maxproc) {
		tablefull("proc");
		retval[1] = 0;
		return (EAGAIN);
	}

	/*
	 * Increment the count of procs running with this uid. Don't allow
	 * a nonprivileged user to exceed their current limit.
	 */
	count = chgproccnt(uid, 1);
	if (uid != 0 && count > p1->p_rlimit[RLIMIT_NPROC].rlim_cur) {
		(void)chgproccnt(uid, -1);
		return (EAGAIN);
	}

	/* The newly created process comes with signal lock held */
	newth = cloneproc(p1, 1);
	thread_dup(current_act(), newth);
	/* p2 = newth->task->proc; */
	p2 = (struct proc *)(get_bsdtask_info(get_threadtask(newth)));

	thread_set_child(newth, p2->p_pid);

	s = splhigh();
	p2->p_stats->p_start = time;
	splx(s);
	p2->p_acflag = AFORK;

	/*
	 * Preserve synchronization semantics of vfork.  If waiting for
	 * child to exec or exit, set P_PPWAIT on child, and sleep on our
	 * proc (in case of exit).
	 */
	if (flags == DOVFORK)
		p2->p_flag |= P_PPWAIT;
	/* drop the signal lock on the child */
	signal_unlock(p2);

	(void) thread_resume(newth);

        /* drop the extra references we got during the creation */
        if (t = get_threadtask(newth)) {
                task_deallocate(t);
        }
        act_deallocate(newth);

	while (p2->p_flag & P_PPWAIT)
		tsleep(p1, PWAIT, "ppwait", 0);

	retval[0] = p2->p_pid;
	retval[1] = 0;			/* mark parent */

	return (0);
}

/*
 * cloneproc()
 *
 * Create a new process from a specified process.
 * On return newly created child process has signal
 * lock held to block delivery of signal to it if called with
 * lock set. fork() code needs to explicity remove this lock 
 * before signals can be delivered
 */
thread_t
cloneproc(p1, lock)
	register struct proc *p1;
	register int lock;
{
	register struct proc *p2, *newproc;
	static int nextpid = 0, pidchecked = 0;
	thread_t th;

	/* Allocate new proc. */
	MALLOC_ZONE(newproc, struct proc *,
			sizeof *newproc, M_PROC, M_WAITOK);
	MALLOC_ZONE(newproc->p_cred, struct pcred *,
			sizeof *newproc->p_cred, M_SUBPROC, M_WAITOK);
	MALLOC_ZONE(newproc->p_stats, struct pstats *,
			sizeof *newproc->p_stats, M_SUBPROC, M_WAITOK);
	MALLOC_ZONE(newproc->p_sigacts, struct sigacts *,
			sizeof *newproc->p_sigacts, M_SUBPROC, M_WAITOK);

	/*
	 * Find an unused process ID.  We remember a range of unused IDs
	 * ready to use (from nextpid+1 through pidchecked-1).
	 */
	nextpid++;
retry:
	/*
	 * If the process ID prototype has wrapped around,
	 * restart somewhat above 0, as the low-numbered procs
	 * tend to include daemons that don't exit.
	 */
	if (nextpid >= PID_MAX) {
		nextpid = 100;
		pidchecked = 0;
	}
	if (nextpid >= pidchecked) {
		int doingzomb = 0;

		pidchecked = PID_MAX;
		/*
		 * Scan the active and zombie procs to check whether this pid
		 * is in use.  Remember the lowest pid that's greater
		 * than nextpid, so we can avoid checking for a while.
		 */
		p2 = allproc.lh_first;
again:
		for (; p2 != 0; p2 = p2->p_list.le_next) {
			while (p2->p_pid == nextpid ||
			    p2->p_pgrp->pg_id == nextpid) {
				nextpid++;
				if (nextpid >= pidchecked)
					goto retry;
			}
			if (p2->p_pid > nextpid && pidchecked > p2->p_pid)
				pidchecked = p2->p_pid;
			if (p2->p_pgrp && p2->p_pgrp->pg_id > nextpid && 
			    pidchecked > p2->p_pgrp->pg_id)
				pidchecked = p2->p_pgrp->pg_id;
		}
		if (!doingzomb) {
			doingzomb = 1;
			p2 = zombproc.lh_first;
			goto again;
		}
	}

	nprocs++;
	p2 = newproc;
	p2->p_stat = SIDL;
	p2->p_pid = nextpid;

	/*
	 * Make a proc table entry for the new process.
	 * Start by zeroing the section of proc that is zero-initialized,
	 * then copy the section that is copied directly from the parent.
	 */
	bzero(&p2->p_startzero,
	    (unsigned) ((caddr_t)&p2->p_endzero - (caddr_t)&p2->p_startzero));
	bcopy(&p1->p_startcopy, &p2->p_startcopy,
	    (unsigned) ((caddr_t)&p2->p_endcopy - (caddr_t)&p2->p_startcopy));
	p2->vm_shm = (void *)NULL; /* Make sure it is zero */

	/*
	 * Duplicate sub-structures as needed.
	 * Increase reference counts on shared objects.
	 * The p_stats and p_sigacts substructs are set in vm_fork.
	 */
	p2->p_flag = P_INMEM;
	if (p1->p_flag & P_PROFIL)
		startprofclock(p2);
	bcopy(p1->p_cred, p2->p_cred, sizeof(*p2->p_cred));
	p2->p_cred->p_refcnt = 1;
	crhold(p1->p_ucred);
	lockinit(&p2->p_cred->pc_lock, PLOCK, "proc cred", 0, 0);

	/* bump references to the text vnode (for procfs) */
	p2->p_textvp = p1->p_textvp;
	if (p2->p_textvp)
		VREF(p2->p_textvp);

	p2->p_fd = fdcopy(p1);
	if (p1->vm_shm) {
		shmfork(p1,p2);
	}
	/*
	 * If p_limit is still copy-on-write, bump refcnt,
	 * otherwise get a copy that won't be modified.
	 * (If PL_SHAREMOD is clear, the structure is shared
	 * copy-on-write.)
	 */
	if (p1->p_limit->p_lflags & PL_SHAREMOD)
		p2->p_limit = limcopy(p1->p_limit);
	else {
		p2->p_limit = p1->p_limit;
		p2->p_limit->p_refcnt++;
	}

	bzero(&p2->p_stats->pstat_startzero,
	    (unsigned) ((caddr_t)&p2->p_stats->pstat_endzero -
	    (caddr_t)&p2->p_stats->pstat_startzero));
	bcopy(&p1->p_stats->pstat_startcopy, &p2->p_stats->pstat_startcopy,
	    ((caddr_t)&p2->p_stats->pstat_endcopy -
	     (caddr_t)&p2->p_stats->pstat_startcopy));

	if (p1->p_sigacts != NULL)
		(void)memcpy(p2->p_sigacts,
				p1->p_sigacts, sizeof *p2->p_sigacts);
	else
		(void)memset(p2->p_sigacts, 0, sizeof *p2->p_sigacts);

	if (p1->p_session->s_ttyvp != NULL && p1->p_flag & P_CONTROLT)
		p2->p_flag |= P_CONTROLT;

	p2->p_xstat = 0;
	p2->p_ru = NULL;

	p2->p_debugger = 0;	/* don't inherit */
	lockinit(&p2->signal_lock, PVM, "signal", 0, 0);
	/* block all signals to reach the process */
	if (lock)
		signal_lock(p2);
	p2->sigwait = FALSE;
	p2->sigwait_thread = NULL;
	p2->exit_thread = NULL;
	p2->user_stack = p1->user_stack;
	p2->p_sigpending = 0;

#if KTRACE
	/*
	 * Copy traceflag and tracefile if enabled.
	 * If not inherited, these were zeroed above.
	 */
	if (p1->p_traceflag&KTRFAC_INHERIT) {
		p2->p_traceflag = p1->p_traceflag;
		if ((p2->p_tracep = p1->p_tracep) != NULL)
			VREF(p2->p_tracep);
	}
#endif

	th = procdup(p2, p1);	/* child, parent */
	LIST_INSERT_AFTER(p1, p2, p_pglist);
	p2->p_pptr = p1;
	LIST_INSERT_HEAD(&p1->p_children, p2, p_sibling);
	LIST_INIT(&p2->p_children);
	LIST_INSERT_HEAD(&allproc, p2, p_list);
	LIST_INSERT_HEAD(PIDHASH(p2->p_pid), p2, p_hash);
	TAILQ_INIT(&p2->p_evlist);
	/*
	 * Make child runnable, set start time.
	 */
	p2->p_stat = SRUN;

	return(th);
}

#include <kern/zalloc.h>

struct zone	*uthread_zone;
int uthread_zone_inited = 0;

void
uthread_zone_init()
{
	if (!uthread_zone_inited) {
		uthread_zone = zinit(sizeof(struct uthread),
							THREAD_MAX * sizeof(struct uthread),
							THREAD_CHUNK * sizeof(struct uthread),
							"uthreads");
		uthread_zone_inited = 1;
	}
}

void *
uthread_alloc(void)
{
	void *ut;

	if (!uthread_zone_inited)
		uthread_zone_init();

	ut = (void *)zalloc(uthread_zone);
	bzero(ut, sizeof(struct uthread));
	return (ut);
}

void
uthread_free(void *uthread)
{
	struct _select *sel;
	struct uthread *uth = (struct uthread *)uthread;

	sel = &uth->uu_state.ss_select;
	/* cleanup the select bit space */
	if (sel->nbytes) {
		FREE(sel->ibits, M_TEMP);
		FREE(sel->obits, M_TEMP);
	}

	/* and free the uthread itself */
	zfree(uthread_zone, (vm_offset_t)uthread);
}

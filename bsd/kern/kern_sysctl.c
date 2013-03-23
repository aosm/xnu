/*
 * Copyright (c) 2000-2011 Apple Inc. All rights reserved.
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
/* Copyright (c) 1995 NeXT Computer, Inc. All Rights Reserved */
/*-
 * Copyright (c) 1982, 1986, 1989, 1993
 *	The Regents of the University of California.  All rights reserved.
 *
 * This code is derived from software contributed to Berkeley by
 * Mike Karels at Berkeley Software Design, Inc.
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
 *	@(#)kern_sysctl.c	8.4 (Berkeley) 4/14/94
 */
/*
 * NOTICE: This file was modified by SPARTA, Inc. in 2005 to introduce
 * support for mandatory and extensible security protections.  This notice
 * is included in support of clause 2.2 (b) of the Apple Public License,
 * Version 2.0.
 */

/*
* DEPRECATED sysctl system call code
 *
 * Everything in this file is deprecated. Sysctls should be handled
 * by the code in kern_newsysctl.c.
 * The remaining "case" sections are supposed to be converted into
 * SYSCTL_*-style definitions, and as soon as all of them are gone,
 * this source file is supposed to die.
 *
 * DO NOT ADD ANY MORE "case" SECTIONS TO THIS FILE, instead define
 * your sysctl with SYSCTL_INT, SYSCTL_PROC etc. in your source file.
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/malloc.h>
#include <sys/proc_internal.h>
#include <sys/kauth.h>
#include <sys/file_internal.h>
#include <sys/vnode_internal.h>
#include <sys/unistd.h>
#include <sys/buf.h>
#include <sys/ioctl.h>
#include <sys/namei.h>
#include <sys/tty.h>
#include <sys/disklabel.h>
#include <sys/vm.h>
#include <sys/sysctl.h>
#include <sys/user.h>
#include <sys/aio_kern.h>
#include <sys/reboot.h>

#include <security/audit/audit.h>
#include <kern/kalloc.h>

#include <mach/machine.h>
#include <mach/mach_host.h>
#include <mach/mach_types.h>
#include <mach/vm_param.h>
#include <kern/mach_param.h>
#include <kern/task.h>
#include <kern/thread.h>
#include <kern/lock.h>
#include <kern/processor.h>
#include <kern/debug.h>
#include <vm/vm_kern.h>
#include <vm/vm_map.h>
#include <mach/host_info.h>

#include <sys/mount_internal.h>
#include <sys/kdebug.h>
#include <sys/sysproto.h>

#include <IOKit/IOPlatformExpert.h>
#include <pexpert/pexpert.h>

#include <machine/machine_routines.h>
#include <machine/exec.h>

#include <vm/vm_protos.h>
#include <sys/imgsrc.h>

#if defined(__i386__) || defined(__x86_64__)
#include <i386/cpuid.h>
#endif

#if CONFIG_FREEZE
#include <sys/kern_memorystatus.h>
#endif

/*
 * deliberately setting max requests to really high number
 * so that runaway settings do not cause MALLOC overflows
 */
#define AIO_MAX_REQUESTS (128 * CONFIG_AIO_MAX)

extern sysctlfn net_sysctl;
extern sysctlfn cpu_sysctl;
extern int aio_max_requests;  				
extern int aio_max_requests_per_process;	
extern int aio_worker_threads;				
extern int lowpri_IO_window_msecs;
extern int lowpri_IO_delay_msecs;
extern int nx_enabled;
extern int speculative_reads_disabled;
extern int ignore_is_ssd;
extern unsigned int speculative_prefetch_max;
extern unsigned int speculative_prefetch_max_iosize;
extern unsigned int preheat_pages_max;
extern unsigned int preheat_pages_min;
extern long numvnodes;

extern unsigned int vm_max_delayed_work_limit;
extern unsigned int vm_max_batch;

extern unsigned int vm_page_free_min;
extern unsigned int vm_page_free_target;
extern unsigned int vm_page_free_reserved;
extern unsigned int vm_page_speculative_percentage;
extern unsigned int vm_page_speculative_q_age_ms;

/*
 * Conditionally allow dtrace to see these functions for debugging purposes.
 */
#ifdef STATIC
#undef STATIC
#endif
#if 0
#define STATIC
#else
#define STATIC static
#endif

extern boolean_t    mach_timer_coalescing_enabled;

STATIC void
fill_user32_eproc(proc_t, struct user32_eproc *__restrict);
STATIC void
fill_user32_externproc(proc_t, struct user32_extern_proc *__restrict);
STATIC void
fill_user64_eproc(proc_t, struct user64_eproc *__restrict);
STATIC void
fill_user64_proc(proc_t, struct user64_kinfo_proc *__restrict);
STATIC void
fill_user64_externproc(proc_t, struct user64_extern_proc *__restrict);
STATIC void
fill_user32_proc(proc_t, struct user32_kinfo_proc *__restrict);

extern int 
kdbg_control(int *name, u_int namelen, user_addr_t where, size_t * sizep);
#if NFSCLIENT
extern int 
netboot_root(void);
#endif
int
pcsamples_ops(int *name, u_int namelen, user_addr_t where, size_t *sizep, 
              proc_t p);
__private_extern__ kern_return_t
reset_vmobjectcache(unsigned int val1, unsigned int val2);
int
sysctl_procargs(int *name, u_int namelen, user_addr_t where, 
				size_t *sizep, proc_t cur_proc);
STATIC int
sysctl_procargsx(int *name, u_int namelen, user_addr_t where, size_t *sizep, 
                 proc_t cur_proc, int argc_yes);
int
sysctl_struct(user_addr_t oldp, size_t *oldlenp, user_addr_t newp, 
              size_t newlen, void *sp, int len);

STATIC int sysdoproc_filt_KERN_PROC_PID(proc_t p, void * arg);
STATIC int sysdoproc_filt_KERN_PROC_PGRP(proc_t p, void * arg);
STATIC int sysdoproc_filt_KERN_PROC_TTY(proc_t p, void * arg);
STATIC int  sysdoproc_filt_KERN_PROC_UID(proc_t p, void * arg);
STATIC int  sysdoproc_filt_KERN_PROC_RUID(proc_t p, void * arg);
#if CONFIG_LCTX
STATIC int  sysdoproc_filt_KERN_PROC_LCID(proc_t p, void * arg);
#endif
int sysdoproc_callback(proc_t p, void *arg);


/* forward declarations for non-static STATIC */
STATIC void fill_loadavg64(struct loadavg *la, struct user64_loadavg *la64);
STATIC void fill_loadavg32(struct loadavg *la, struct user32_loadavg *la32);
STATIC int sysctl_handle_kern_threadname(struct sysctl_oid *oidp, void *arg1, int arg2, struct sysctl_req *req);
STATIC int sysctl_sched_stats(struct sysctl_oid *oidp, void *arg1, int arg2, struct sysctl_req *req);
STATIC int sysctl_sched_stats_enable(struct sysctl_oid *oidp, void *arg1, int arg2, struct sysctl_req *req);
STATIC int sysctl_kdebug_ops SYSCTL_HANDLER_ARGS;
STATIC int sysctl_dotranslate SYSCTL_HANDLER_ARGS;
STATIC int sysctl_doaffinity SYSCTL_HANDLER_ARGS;
#if COUNT_SYSCALLS
STATIC int sysctl_docountsyscalls SYSCTL_HANDLER_ARGS;
#endif	/* COUNT_SYSCALLS */
#if !CONFIG_EMBEDDED
STATIC int sysctl_doprocargs SYSCTL_HANDLER_ARGS;
#endif	/* !CONFIG_EMBEDDED */
STATIC int sysctl_doprocargs2 SYSCTL_HANDLER_ARGS;
STATIC int sysctl_prochandle SYSCTL_HANDLER_ARGS;
#if DEBUG
STATIC int sysctl_dodebug SYSCTL_HANDLER_ARGS;
#endif
STATIC int sysctl_aiomax(struct sysctl_oid *oidp, void *arg1, int arg2, struct sysctl_req *req);
STATIC int sysctl_aioprocmax(struct sysctl_oid *oidp, void *arg1, int arg2, struct sysctl_req *req);
STATIC int sysctl_aiothreads(struct sysctl_oid *oidp, void *arg1, int arg2, struct sysctl_req *req);
STATIC int sysctl_maxproc(struct sysctl_oid *oidp, void *arg1, int arg2, struct sysctl_req *req);
STATIC int sysctl_osversion(struct sysctl_oid *oidp, void *arg1, int arg2, struct sysctl_req *req);
STATIC int sysctl_sysctl_bootargs(struct sysctl_oid *oidp, void *arg1, int arg2, struct sysctl_req *req);
STATIC int sysctl_maxvnodes(struct sysctl_oid *oidp, void *arg1, int arg2, struct sysctl_req *req);
STATIC int sysctl_securelvl(struct sysctl_oid *oidp, void *arg1, int arg2, struct sysctl_req *req);
STATIC int sysctl_domainname(struct sysctl_oid *oidp, void *arg1, int arg2, struct sysctl_req *req);
STATIC int sysctl_hostname(struct sysctl_oid *oidp, void *arg1, int arg2, struct sysctl_req *req);
STATIC int sysctl_procname(struct sysctl_oid *oidp, void *arg1, int arg2, struct sysctl_req *req);
STATIC int sysctl_boottime(struct sysctl_oid *oidp, void *arg1, int arg2, struct sysctl_req *req);
STATIC int sysctl_symfile(struct sysctl_oid *oidp, void *arg1, int arg2, struct sysctl_req *req);
#if NFSCLIENT
STATIC int sysctl_netboot(struct sysctl_oid *oidp, void *arg1, int arg2, struct sysctl_req *req);
#endif
#ifdef CONFIG_IMGSRC_ACCESS
STATIC int sysctl_imgsrcdev(struct sysctl_oid *oidp, void *arg1, int arg2, struct sysctl_req *req);
#endif
STATIC int sysctl_usrstack(struct sysctl_oid *oidp, void *arg1, int arg2, struct sysctl_req *req);
STATIC int sysctl_usrstack64(struct sysctl_oid *oidp, void *arg1, int arg2, struct sysctl_req *req);
STATIC int sysctl_coredump(struct sysctl_oid *oidp, void *arg1, int arg2, struct sysctl_req *req);
STATIC int sysctl_suid_coredump(struct sysctl_oid *oidp, void *arg1, int arg2, struct sysctl_req *req);
STATIC int sysctl_delayterm(struct sysctl_oid *oidp, void *arg1, int arg2, struct sysctl_req *req);
STATIC int sysctl_rage_vnode(struct sysctl_oid *oidp, void *arg1, int arg2, struct sysctl_req *req);
STATIC int sysctl_kern_check_openevt(struct sysctl_oid *oidp, void *arg1, int arg2, struct sysctl_req *req);
STATIC int sysctl_nx(struct sysctl_oid *oidp, void *arg1, int arg2, struct sysctl_req *req);
STATIC int sysctl_loadavg(struct sysctl_oid *oidp, void *arg1, int arg2, struct sysctl_req *req);
STATIC int sysctl_vm_toggle_address_reuse(struct sysctl_oid *oidp, void *arg1, int arg2, struct sysctl_req *req);
STATIC int sysctl_swapusage(struct sysctl_oid *oidp, void *arg1, int arg2, struct sysctl_req *req);
#if defined(__i386__) || defined(__x86_64__)
STATIC int sysctl_sysctl_exec_affinity(struct sysctl_oid *oidp, void *arg1, int arg2, struct sysctl_req *req);
#endif
STATIC int fetch_process_cputype( proc_t cur_proc, int *name, u_int namelen, cpu_type_t *cputype);
STATIC int sysctl_sysctl_native(struct sysctl_oid *oidp, void *arg1, int arg2, struct sysctl_req *req);
STATIC int sysctl_sysctl_cputype(struct sysctl_oid *oidp, void *arg1, int arg2, struct sysctl_req *req);
STATIC int sysctl_safeboot(struct sysctl_oid *oidp, void *arg1, int arg2, struct sysctl_req *req);
STATIC int sysctl_singleuser(struct sysctl_oid *oidp, void *arg1, int arg2, struct sysctl_req *req);
STATIC int sysctl_slide(struct sysctl_oid *oidp, void *arg1, int arg2, struct sysctl_req *req);


extern void IORegistrySetOSBuildVersion(char * build_version); 

STATIC void
fill_loadavg64(struct loadavg *la, struct user64_loadavg *la64)
{
	la64->ldavg[0]	= la->ldavg[0];
	la64->ldavg[1]	= la->ldavg[1];
	la64->ldavg[2]	= la->ldavg[2];
	la64->fscale	= (user64_long_t)la->fscale;
}

STATIC void
fill_loadavg32(struct loadavg *la, struct user32_loadavg *la32)
{
	la32->ldavg[0]	= la->ldavg[0];
	la32->ldavg[1]	= la->ldavg[1];
	la32->ldavg[2]	= la->ldavg[2];
	la32->fscale	= (user32_long_t)la->fscale;
}

/*
 * sysctl_mem_hold
 *
 * Description:	Wire down the callers address map on behalf of sysctl's
 *		that perform their own copy operations while holding
 *		locks e.g. in the paging path, which could lead to a
 *		deadlock, or while holding a spinlock.
 *
 * Parameters:	addr			User buffer address
 *		len			User buffer length
 *
 * Returns:	0			Success
 *	vslock:ENOMEM			Insufficient physical pages to wire
 *	vslock:EACCES			Bad protection mode
 *	vslock:EINVAL			Invalid parameters
 *
 * Notes:	This code is invoked for the first OID element where the
 *		CTLFLAG_LOCKED is not specified for a given OID node
 *		element durng OID traversal, and is held for all
 *		subsequent node traversals, and only released after the
 *		leaf node handler invocation is complete.
 *
 * Legacy:	For legacy scyctl's provided by third party code which
 *		expect funnel protection for calls into their code, this
 *		routine will also take the funnel, which will also only
 *		be released after the leaf node handler is complete.
 *
 *		This is to support legacy 32 bit BSD KEXTs and legacy 32
 *		bit single threaded filesystem KEXTs and similar code
 *		which relies on funnel protection, e.g. for things like
 *		FSID based sysctl's.
 *
 *		NEW CODE SHOULD NOT RELY ON THIS BEHAVIOUR!  IT WILL BE
 *		REMOVED IN A FUTURE RELASE OF Mac OS X!
 *
 * Bugs:	This routine does nothing with the new_addr and new_len
 *		at present, but it should, since read from the user space
 *		process adddress space which could potentially trigger
 *		paging may also be occurring deep down.  This is due to
 *		a current limitation of the vslock() routine, which will
 *		always request a wired mapping be read/write, due to not
 *		taking an access mode parameter.  Note that this could
 *		also cause problems for output on architectures where
 *		write access does not require read acccess if the current
 *		mapping lacks read access.
 *
 * XXX:		To be moved to kern_newsysctl.c to avoid __private_extern__
 */
int sysctl_mem_lock(user_addr_t old_addr, user_size_t old_len, user_addr_t new_addr, user_size_t new_len);
int
sysctl_mem_lock(__unused user_addr_t old_addr, __unused user_size_t old_len, __unused user_addr_t new_addr, __unused user_size_t new_len)
{
	return 0;
}

/*
 * Locking and stats
 */

/* sysctl() syscall */
int
__sysctl(proc_t p, struct __sysctl_args *uap, __unused int32_t *retval)
{
	boolean_t funnel_state = FALSE;		/* not held if unknown */
	int error;
	size_t savelen = 0, oldlen = 0, newlen;
	int name[CTL_MAXNAME];
	int error1;
	boolean_t vslock_taken = FALSE;
	boolean_t funnel_taken = FALSE;
#if CONFIG_MACF
	kauth_cred_t my_cred;
#endif

	/*
	 * all top-level sysctl names are non-terminal
	 */
	if (uap->namelen > CTL_MAXNAME || uap->namelen < 2)
		return (EINVAL);
	error = copyin(uap->name, &name[0], uap->namelen * sizeof(int));
	if (error)
		return (error);
		
	AUDIT_ARG(ctlname, name, uap->namelen);

	if (proc_is64bit(p)) {
		/* uap->newlen is a size_t value which grows to 64 bits 
		 * when coming from a 64-bit process.  since it's doubtful we'll 
		 * have a sysctl newp buffer greater than 4GB we shrink it to size_t
		 */
		newlen = CAST_DOWN(size_t, uap->newlen);
	}
	else {
		newlen = uap->newlen;
	}

/*
 * XXX TODO:	push down rights check for CTL_HW OIDs; most duplicate
 * XXX		it anyway, which is a performance sink, and requires use
 * XXX		of SUID root programs (see <rdar://3915692>).
 *
 * Note:	Opt out of non-leaf node enforcement by removing this
 *		check for the top level OID value, and then adding
 *		CTLFLAG_ANYBODY to the leaf nodes in question.  Enforce as
 *		suser for writed in leaf nodes by omitting this flag.
 *		Enforce with a higher granularity by making the leaf node
 *		of type SYSCTL_PROC() in order to provide a procedural
 *		enforcement call site.
 *
 * NOTE:	This function is called prior to any subfunctions being
 *		called with a fallback to userland_sysctl(); as such, this
 *		permissions check here will veto the fallback operation.
 */
	/* CTL_UNSPEC is used to get oid to AUTO_OID */
	if (uap->new != USER_ADDR_NULL
	    && ((name[0] == CTL_HW)
	    || (name[0] == CTL_VM))
	    && (error = suser(kauth_cred_get(), &p->p_acflag)))
		return (error);

// XXX need to relocate into each terminal instead of leaving this here...
// XXX macf preemptory check.
#if CONFIG_MACF
	my_cred = kauth_cred_proc_ref(p);
	error = mac_system_check_sysctl(
	    my_cred, 
	    (int *) name,
	    uap->namelen,
  	    uap->old,
	    uap->oldlenp,
	    0,		/* XXX 1 for CTL_KERN checks */
	    uap->new,
	    newlen
   	);
	kauth_cred_unref(&my_cred);
	if (error)
		return (error);
#endif

	if (uap->oldlenp != USER_ADDR_NULL) {
		uint64_t	oldlen64 = fuulong(uap->oldlenp);

		oldlen = CAST_DOWN(size_t, oldlen64);
		/*
		 * If more than 4G, clamp to 4G - useracc() below will catch
		 * with an EFAULT, if it's actually necessary.
		 */
		if (oldlen64 > 0x00000000ffffffffULL)
			oldlen = 0xffffffffUL;
	}

	if ((name[0] == CTL_VFS || name[0] == CTL_VM)) {
		/*
		 * Always take the funnel for CTL_VFS and CTL_VM
		 *
		 * XXX We should also take it for any OID without the
		 * XXX CTLFLAG_LOCKED set on it; fix this later!
		 */
		funnel_state = thread_funnel_set(kernel_flock, TRUE);
		funnel_taken = TRUE;

		/*
		 * XXX Take the vslock() only when we are copying out; this
		 * XXX erroneously assumes that the copy in will not cause
		 * XXX a fault if caled from the paging path due to the
		 * XXX having been recently touched in order to establish
		 * XXX the input data.  This is a bad assumption.
		 *
		 * Note:	This is overkill, but third parties might
		 *		already call sysctl internally in KEXTs that
		 *		implement mass storage drivers.  If you are
		 *		writing a new KEXT, don't do that.
		 */
		if(uap->old != USER_ADDR_NULL) {
			if (!useracc(uap->old, (user_size_t)oldlen, B_WRITE)) {
				thread_funnel_set(kernel_flock, funnel_state);
				return (EFAULT);
			}

			if (oldlen) {
				if ((error = vslock(uap->old, (user_size_t)oldlen))) {
					thread_funnel_set(kernel_flock, funnel_state);
					return(error);
				}
				savelen = oldlen;
				vslock_taken = TRUE;
			}
		}
	}

	/*
	 * XXX convert vfs_sysctl subelements to newsysctl; this is hard
	 * XXX because of VFS_NUMMNTOPS being top level.
	 */
	error = ENOTSUP;
	if (name[0] == CTL_VFS) {
	        error = vfs_sysctl(name + 1, uap->namelen - 1, uap->old,
                       &oldlen, uap->new, newlen, p);
	}

	if (vslock_taken == TRUE) {
	        error1 = vsunlock(uap->old, (user_size_t)savelen, B_WRITE);
		if (!error)
		        error = error1;
        }

	if ( (name[0] != CTL_VFS) && (error == ENOTSUP) ) {
		size_t	tmp = oldlen;
		error = userland_sysctl(p, name, uap->namelen, uap->old, &tmp, 
		                        uap->new, newlen, &oldlen);
	}

	/*
	 * If we took the funnel, which we only do for CTL_VFS and CTL_VM on
	 * 32 bit architectures, then drop it.
	 *
	 * XXX the grabbing and dropping need to move into the leaf nodes,
	 * XXX for sysctl's that are not marked CTLFLAG_LOCKED, but this is
	 * XXX true for the vslock, as well.  We have a start at a routine
	 * to wrapper this (above), but it's not turned on.  The current code
	 * removed the funnel and the vslock() from all but these two top
	 * level OIDs.  Note that VFS only needs to take the funnel if the FS
	 * against which it's operating is not thread safe (but since an FS
	 * can be in the paging path, it still needs to take the vslock()).
	 */
	if (funnel_taken)
		thread_funnel_set(kernel_flock, funnel_state);

	if ((error) && (error != ENOMEM))
		return (error);

	if (uap->oldlenp != USER_ADDR_NULL)
	        error = suulong(uap->oldlenp, oldlen);

	return (error);
}

/*
 * Attributes stored in the kernel.
 */
__private_extern__ char corefilename[MAXPATHLEN+1];
__private_extern__ int do_coredump;
__private_extern__ int sugid_coredump;

#if COUNT_SYSCALLS
__private_extern__ int do_count_syscalls;
#endif

#ifdef INSECURE
int securelevel = -1;
#else
int securelevel;
#endif

STATIC int
sysctl_doaffinity SYSCTL_HANDLER_ARGS
{
	__unused int cmd = oidp->oid_arg2;	/* subcommand*/
	int *name = arg1;		/* oid element argument vector */
	int namelen = arg2;		/* number of oid element arguments */
	user_addr_t oldp = req->oldptr;	/* user buffer copy out address */
	size_t *oldlenp = &req->oldlen;	/* user buffer copy out size */
	user_addr_t newp = req->newptr;	/* user buffer copy in address */
//	size_t newlen = req->newlen;	/* user buffer copy in size */

	int error = ENOTSUP;		/* Default to failure */

	proc_t cur_proc = current_proc();

	if (namelen < 1)
		return (ENOTSUP);

	if (name[0] == 0 && 1 == namelen) {
		error = sysctl_rdint(oldp, oldlenp, newp,
			                (cur_proc->p_flag & P_AFFINITY) ? 1 : 0);
	} else if (name[0] == 1 && 2 == namelen) {
		if (name[1] == 0) {
			OSBitAndAtomic(~((uint32_t)P_AFFINITY), &cur_proc->p_flag);
		} else {
			OSBitOrAtomic(P_AFFINITY, &cur_proc->p_flag);
		}
		error =  0;
	}

	/* adjust index so we return the right required/consumed amount */
	if (!error)
		req->oldidx += req->oldlen;

	return (error);
}
SYSCTL_PROC(_kern, KERN_AFFINITY, affinity, CTLTYPE_NODE|CTLFLAG_RD | CTLFLAG_LOCKED,
	0,			/* Pointer argument (arg1) */
	0,			/* Integer argument (arg2) */
	sysctl_doaffinity,	/* Handler function */
	NULL,			/* Data pointer */
	"");

STATIC int
sysctl_dotranslate SYSCTL_HANDLER_ARGS
{
	__unused int cmd = oidp->oid_arg2;	/* subcommand*/
	int *name = arg1;		/* oid element argument vector */
	int namelen = arg2;		/* number of oid element arguments */
	user_addr_t oldp = req->oldptr;	/* user buffer copy out address */
	size_t *oldlenp = &req->oldlen;	/* user buffer copy out size */
	user_addr_t newp = req->newptr;	/* user buffer copy in address */
//	size_t newlen = req->newlen;	/* user buffer copy in size */
	int error;

	proc_t cur_proc = current_proc();
	proc_t p;
	int istranslated = 0;
	kauth_cred_t my_cred;
	uid_t uid;

	if (namelen != 1)
		return (ENOTSUP);

	p = proc_find(name[0]);
	if (p == NULL)
		return (EINVAL);

	my_cred = kauth_cred_proc_ref(p);
	uid = kauth_cred_getuid(my_cred);
	kauth_cred_unref(&my_cred);
	if ((uid != kauth_cred_getuid(kauth_cred_get())) 
		&& suser(kauth_cred_get(), &cur_proc->p_acflag)) {
		proc_rele(p);
		return (EPERM);
	}

	istranslated = (p->p_flag & P_TRANSLATED);
	proc_rele(p);
	error =  sysctl_rdint(oldp, oldlenp, newp,
		                (istranslated != 0) ? 1 : 0);

	/* adjust index so we return the right required/consumed amount */
	if (!error)
		req->oldidx += req->oldlen;

	return (error);
}
/*
 * XXX make CTLFLAG_RW so sysctl_rdint() will EPERM on attempts to write;
 * XXX this may not be necessary.
 */
SYSCTL_PROC(_kern, KERN_TRANSLATE, translate, CTLTYPE_NODE|CTLFLAG_RW | CTLFLAG_LOCKED,
	0,			/* Pointer argument (arg1) */
	0,			/* Integer argument (arg2) */
	sysctl_dotranslate,	/* Handler function */
	NULL,			/* Data pointer */
	"");

STATIC int
sysctl_handle_kern_threadname(	__unused struct sysctl_oid *oidp, __unused void *arg1,
	      __unused int arg2, struct sysctl_req *req)
{
	int error;
	struct uthread *ut = get_bsdthread_info(current_thread());
	user_addr_t oldp=0, newp=0;
	size_t *oldlenp=NULL;
	size_t newlen=0;

	oldp = req->oldptr;
	oldlenp = &(req->oldlen);
	newp = req->newptr;
	newlen = req->newlen;

	/* We want the current length, and maybe the string itself */
	if(oldlenp) {
		/* if we have no thread name yet tell'em we want MAXTHREADNAMESIZE - 1 */
		size_t currlen = MAXTHREADNAMESIZE - 1;
		
		if(ut->pth_name)
			/* use length of current thread name */
			currlen = strlen(ut->pth_name);
		if(oldp) {
			if(*oldlenp < currlen)
				return ENOMEM;
			/* NOTE - we do not copy the NULL terminator */
			if(ut->pth_name) {
				error = copyout(ut->pth_name,oldp,currlen);
				if(error)
					return error;
			}
		}	
		/* return length of thread name minus NULL terminator (just like strlen)  */
		req->oldidx = currlen;
	}

	/* We want to set the name to something */
	if(newp) 
	{
		if(newlen > (MAXTHREADNAMESIZE - 1))
			return ENAMETOOLONG;
		if(!ut->pth_name)
		{
			ut->pth_name = (char*)kalloc( MAXTHREADNAMESIZE );
			if(!ut->pth_name)
				return ENOMEM;
		}
		bzero(ut->pth_name, MAXTHREADNAMESIZE);
		error = copyin(newp, ut->pth_name, newlen);
		if(error)
			return error;
	}
		
	return 0;
}

SYSCTL_PROC(_kern, KERN_THREADNAME, threadname, CTLFLAG_ANYBODY | CTLTYPE_STRING | CTLFLAG_RW | CTLFLAG_LOCKED, 0, 0, sysctl_handle_kern_threadname,"A","");

#define BSD_HOST 1
STATIC int
sysctl_sched_stats(__unused struct sysctl_oid *oidp, __unused void *arg1, __unused int arg2, struct sysctl_req *req)
{
	host_basic_info_data_t hinfo;
	kern_return_t kret;
	uint32_t size;
	int changed;
	mach_msg_type_number_t count = HOST_BASIC_INFO_COUNT;
	struct _processor_statistics_np *buf;
	int error;

	kret = host_info((host_t)BSD_HOST, HOST_BASIC_INFO, (host_info_t)&hinfo, &count);
	if (kret != KERN_SUCCESS) {
		return EINVAL;
	}

	size = sizeof(struct _processor_statistics_np) * (hinfo.logical_cpu_max + 2); /* One for RT Queue, One for Fair Share Queue */
	
	if (req->oldlen < size) {
		return EINVAL;
	}

	MALLOC(buf, struct _processor_statistics_np*, size, M_TEMP, M_ZERO | M_WAITOK);
	
	kret = get_sched_statistics(buf, &size);
	if (kret != KERN_SUCCESS) {
		error = EINVAL;
		goto out;
	}

	error = sysctl_io_opaque(req, buf, size, &changed);
	if (error) {
		goto out;
	}

	if (changed) {
		panic("Sched info changed?!");
	}
out:
	FREE(buf, M_TEMP);
	return error;
}

SYSCTL_PROC(_kern, OID_AUTO, sched_stats, CTLFLAG_LOCKED, 0, 0, sysctl_sched_stats, "-", "");

STATIC int
sysctl_sched_stats_enable(__unused struct sysctl_oid *oidp, __unused void *arg1, __unused int arg2, __unused struct sysctl_req *req)
{
	boolean_t active;
	int res;

	if (req->newlen != sizeof(active)) {
		return EINVAL;
	}

	res = copyin(req->newptr, &active, sizeof(active));
	if (res != 0) {
		return res;
	}

	return set_sched_stats_active(active);
}

SYSCTL_PROC(_kern, OID_AUTO, sched_stats_enable, CTLFLAG_LOCKED | CTLFLAG_WR, 0, 0, sysctl_sched_stats_enable, "-", "");

extern int get_kernel_symfile(proc_t, char **);

#if COUNT_SYSCALLS
#define KERN_COUNT_SYSCALLS (KERN_OSTYPE + 1000)

extern int 	nsysent;
extern int syscalls_log[];
extern const char *syscallnames[];

STATIC int
sysctl_docountsyscalls SYSCTL_HANDLER_ARGS
{
	__unused int cmd = oidp->oid_arg2;	/* subcommand*/
	__unused int *name = arg1;	/* oid element argument vector */
	__unused int namelen = arg2;	/* number of oid element arguments */
	user_addr_t oldp = req->oldptr;	/* user buffer copy out address */
	size_t *oldlenp = &req->oldlen;	/* user buffer copy out size */
	user_addr_t newp = req->newptr;	/* user buffer copy in address */
	size_t newlen = req->newlen;	/* user buffer copy in size */
	int error;

	int tmp;

	/* valid values passed in:
	 * = 0 means don't keep called counts for each bsd syscall
	 * > 0 means keep called counts for each bsd syscall
	 * = 2 means dump current counts to the system log
	 * = 3 means reset all counts
	 * for example, to dump current counts:  
	 *		sysctl -w kern.count_calls=2
	 */
	error = sysctl_int(oldp, oldlenp, newp, newlen, &tmp);
	if ( error != 0 ) {
		return (error);
	}
		
	if ( tmp == 1 ) {
		do_count_syscalls = 1;
	}
	else if ( tmp == 0 || tmp == 2 || tmp == 3 ) {
		int			i;
		for ( i = 0; i < nsysent; i++ ) {
			if ( syscalls_log[i] != 0 ) {
				if ( tmp == 2 ) {
					printf("%d calls - name %s \n", syscalls_log[i], syscallnames[i]);
				}
				else {
					syscalls_log[i] = 0;
				}
			}
		}
		if ( tmp != 0 ) {
			do_count_syscalls = 1;
		}
	}

	/* adjust index so we return the right required/consumed amount */
	if (!error)
		req->oldidx += req->oldlen;

	return (error);
}
SYSCTL_PROC(_kern, KERN_COUNT_SYSCALLS, count_syscalls, CTLTYPE_NODE|CTLFLAG_RD | CTLFLAG_LOCKED,
	0,			/* Pointer argument (arg1) */
	0,			/* Integer argument (arg2) */
	sysctl_docountsyscalls,	/* Handler function */
	NULL,			/* Data pointer */
	"");
#endif	/* COUNT_SYSCALLS */

#if DEBUG
/*
 * Debugging related system variables.
 */
#if DIAGNOSTIC
extern
#endif /* DIAGNOSTIC */
struct ctldebug debug0, debug1;
struct ctldebug debug2, debug3, debug4;
struct ctldebug debug5, debug6, debug7, debug8, debug9;
struct ctldebug debug10, debug11, debug12, debug13, debug14;
struct ctldebug debug15, debug16, debug17, debug18, debug19;
STATIC struct ctldebug *debugvars[CTL_DEBUG_MAXID] = {
	&debug0, &debug1, &debug2, &debug3, &debug4,
	&debug5, &debug6, &debug7, &debug8, &debug9,
	&debug10, &debug11, &debug12, &debug13, &debug14,
	&debug15, &debug16, &debug17, &debug18, &debug19,
};
STATIC int
sysctl_dodebug SYSCTL_HANDLER_ARGS
{
	int cmd = oidp->oid_arg2;	/* subcommand*/
	int *name = arg1;		/* oid element argument vector */
	int namelen = arg2;		/* number of oid element arguments */
	user_addr_t oldp = req->oldptr;	/* user buffer copy out address */
	size_t *oldlenp = &req->oldlen;	/* user buffer copy out size */
	user_addr_t newp = req->newptr;	/* user buffer copy in address */
	size_t newlen = req->newlen;	/* user buffer copy in size */
	int error;

	struct ctldebug *cdp;

	/* all sysctl names at this level are name and field */
	if (namelen != 1)
		return (ENOTSUP);		/* overloaded */
	if (cmd < 0 || cmd >= CTL_DEBUG_MAXID)
		return (ENOTSUP);
	cdp = debugvars[cmd];
	if (cdp->debugname == 0)
		return (ENOTSUP);
	switch (name[0]) {
	case CTL_DEBUG_NAME:
		error = sysctl_rdstring(oldp, oldlenp, newp, cdp->debugname);
		break;
	case CTL_DEBUG_VALUE:
		error = sysctl_int(oldp, oldlenp, newp, newlen, cdp->debugvar);
		break;
	default:
		error = ENOTSUP;
		break;
	}

	/* adjust index so we return the right required/consumed amount */
	if (!error)
		req->oldidx += req->oldlen;

	return (error);
}
/*
 * XXX We mark this RW instead of RD to let sysctl_rdstring() return the
 * XXX historical error.
 */
SYSCTL_PROC(_debug, CTL_DEBUG_NAME, name, CTLTYPE_NODE|CTLFLAG_RW | CTLFLAG_LOCKED,
	0,			/* Pointer argument (arg1) */
	CTL_DEBUG_NAME,		/* Integer argument (arg2) */
	sysctl_dodebug,		/* Handler function */
	NULL,			/* Data pointer */
	"Debugging");
SYSCTL_PROC(_debug, CTL_DEBUG_VALUE, value, CTLTYPE_NODE|CTLFLAG_RW | CTLFLAG_LOCKED,
	0,			/* Pointer argument (arg1) */
	CTL_DEBUG_VALUE,	/* Integer argument (arg2) */
	sysctl_dodebug,		/* Handler function */
	NULL,			/* Data pointer */
	"Debugging");
#endif /* DEBUG */

/*
 * The following sysctl_* functions should not be used
 * any more, as they can only cope with callers in
 * user mode: Use new-style
 *  sysctl_io_number()
 *  sysctl_io_string()
 *  sysctl_io_opaque()
 * instead.
 */

/*
 * Validate parameters and get old / set new parameters
 * for an integer-valued sysctl function.
 */
int
sysctl_int(user_addr_t oldp, size_t *oldlenp, 
           user_addr_t newp, size_t newlen, int *valp)
{
	int error = 0;

	if (oldp != USER_ADDR_NULL && oldlenp == NULL)
		return (EFAULT);
	if (oldp && *oldlenp < sizeof(int))
		return (ENOMEM);
	if (newp && newlen != sizeof(int))
		return (EINVAL);
	*oldlenp = sizeof(int);
	if (oldp)
		error = copyout(valp, oldp, sizeof(int));
	if (error == 0 && newp) {
		error = copyin(newp, valp, sizeof(int));
		AUDIT_ARG(value32, *valp);
	}
	return (error);
}

/*
 * As above, but read-only.
 */
int
sysctl_rdint(user_addr_t oldp, size_t *oldlenp, user_addr_t newp, int val)
{
	int error = 0;

	if (oldp != USER_ADDR_NULL && oldlenp == NULL)
		return (EFAULT);
	if (oldp && *oldlenp < sizeof(int))
		return (ENOMEM);
	if (newp)
		return (EPERM);
	*oldlenp = sizeof(int);
	if (oldp)
		error = copyout((caddr_t)&val, oldp, sizeof(int));
	return (error);
}

/*
 * Validate parameters and get old / set new parameters
 * for an quad(64bit)-valued sysctl function.
 */
int
sysctl_quad(user_addr_t oldp, size_t *oldlenp, 
            user_addr_t newp, size_t newlen, quad_t *valp)
{
	int error = 0;

	if (oldp != USER_ADDR_NULL && oldlenp == NULL)
		return (EFAULT);
	if (oldp && *oldlenp < sizeof(quad_t))
		return (ENOMEM);
	if (newp && newlen != sizeof(quad_t))
		return (EINVAL);
	*oldlenp = sizeof(quad_t);
	if (oldp)
		error = copyout(valp, oldp, sizeof(quad_t));
	if (error == 0 && newp)
		error = copyin(newp, valp, sizeof(quad_t));
	return (error);
}

/*
 * As above, but read-only.
 */
int
sysctl_rdquad(user_addr_t oldp, size_t *oldlenp, user_addr_t newp, quad_t val)
{
	int error = 0;

	if (oldp != USER_ADDR_NULL && oldlenp == NULL)
		return (EFAULT);
	if (oldp && *oldlenp < sizeof(quad_t))
		return (ENOMEM);
	if (newp)
		return (EPERM);
	*oldlenp = sizeof(quad_t);
	if (oldp)
		error = copyout((caddr_t)&val, oldp, sizeof(quad_t));
	return (error);
}

/*
 * Validate parameters and get old / set new parameters
 * for a string-valued sysctl function.  Unlike sysctl_string, if you
 * give it a too small (but larger than 0 bytes) buffer, instead of
 * returning ENOMEM, it truncates the returned string to the buffer
 * size.  This preserves the semantics of some library routines
 * implemented via sysctl, which truncate their returned data, rather
 * than simply returning an error. The returned string is always NUL
 * terminated.
 */
int
sysctl_trstring(user_addr_t oldp, size_t *oldlenp, 
              user_addr_t newp, size_t newlen, char *str, int maxlen)
{
	int len, copylen, error = 0;

	if (oldp != USER_ADDR_NULL && oldlenp == NULL)
		return (EFAULT);
	copylen = len = strlen(str) + 1;
	if (oldp && (len < 0 || *oldlenp < 1))
		return (ENOMEM);
	if (oldp && (*oldlenp < (size_t)len))
		copylen = *oldlenp + 1;
	if (newp && (maxlen < 0 || newlen >= (size_t)maxlen))
		return (EINVAL);
	*oldlenp = copylen - 1; /* deal with NULL strings correctly */
	if (oldp) {
		error = copyout(str, oldp, copylen);
		if (!error) {
			unsigned char c = 0;
			/* NUL terminate */
			oldp += *oldlenp;
			error = copyout((void *)&c, oldp, sizeof(char));
		}
	}
	if (error == 0 && newp) {
		error = copyin(newp, str, newlen);
		str[newlen] = 0;
		AUDIT_ARG(text, (char *)str);
	}
	return (error);
}

/*
 * Validate parameters and get old / set new parameters
 * for a string-valued sysctl function.
 */
int
sysctl_string(user_addr_t oldp, size_t *oldlenp, 
              user_addr_t newp, size_t newlen, char *str, int maxlen)
{
	int len, error = 0;

	if (oldp != USER_ADDR_NULL && oldlenp == NULL)
		return (EFAULT);
	len = strlen(str) + 1;
	if (oldp && (len < 0 || *oldlenp < (size_t)len))
		return (ENOMEM);
	if (newp && (maxlen < 0 || newlen >= (size_t)maxlen))
		return (EINVAL);
	*oldlenp = len -1; /* deal with NULL strings correctly */
	if (oldp) {
		error = copyout(str, oldp, len);
	}
	if (error == 0 && newp) {
		error = copyin(newp, str, newlen);
		str[newlen] = 0;
		AUDIT_ARG(text, (char *)str);
	}
	return (error);
}

/*
 * As above, but read-only.
 */
int
sysctl_rdstring(user_addr_t oldp, size_t *oldlenp, 
                user_addr_t newp, char *str)
{
	int len, error = 0;

	if (oldp != USER_ADDR_NULL && oldlenp == NULL)
		return (EFAULT);
	len = strlen(str) + 1;
	if (oldp && *oldlenp < (size_t)len)
		return (ENOMEM);
	if (newp)
		return (EPERM);
	*oldlenp = len;
	if (oldp)
		error = copyout(str, oldp, len);
	return (error);
}

/*
 * Validate parameters and get old / set new parameters
 * for a structure oriented sysctl function.
 */
int
sysctl_struct(user_addr_t oldp, size_t *oldlenp, 
              user_addr_t newp, size_t newlen, void *sp, int len)
{
	int error = 0;

	if (oldp != USER_ADDR_NULL && oldlenp == NULL)
		return (EFAULT);
	if (oldp && (len < 0 || *oldlenp < (size_t)len))
		return (ENOMEM);
	if (newp && (len < 0 || newlen > (size_t)len))
		return (EINVAL);
	if (oldp) {
		*oldlenp = len;
		error = copyout(sp, oldp, len);
	}
	if (error == 0 && newp)
		error = copyin(newp, sp, len);
	return (error);
}

/*
 * Validate parameters and get old parameters
 * for a structure oriented sysctl function.
 */
int
sysctl_rdstruct(user_addr_t oldp, size_t *oldlenp, 
                user_addr_t newp, void *sp, int len)
{
	int error = 0;

	if (oldp != USER_ADDR_NULL && oldlenp == NULL)
		return (EFAULT);
	if (oldp && (len < 0 || *oldlenp < (size_t)len))
		return (ENOMEM);
	if (newp)
		return (EPERM);
	*oldlenp = len;
	if (oldp)
		error = copyout(sp, oldp, len);
	return (error);
}

STATIC int
sysdoproc_filt_KERN_PROC_PID(proc_t p, void * arg)
{
	if (p->p_pid != (pid_t)*(int*)arg)
		return(0);
	else
		return(1);
}

STATIC int
sysdoproc_filt_KERN_PROC_PGRP(proc_t p, void * arg)
{
	if (p->p_pgrpid != (pid_t)*(int*)arg)
		return(0);
	else
	  return(1);
}

STATIC int
sysdoproc_filt_KERN_PROC_TTY(proc_t p, void * arg)
{
	boolean_t funnel_state;
	int retval;
	struct tty *tp;

	
	funnel_state = thread_funnel_set(kernel_flock, TRUE);
	/* This is very racy but list lock is held.. Hmmm. */
	if ((p->p_flag & P_CONTROLT) == 0 ||
		(p->p_pgrp == NULL) || (p->p_pgrp->pg_session == NULL) ||
			(tp = SESSION_TP(p->p_pgrp->pg_session)) == TTY_NULL ||
			tp->t_dev != (dev_t)*(int*)arg)
				retval = 0;
	else
		retval = 1;

	thread_funnel_set(kernel_flock, funnel_state);

	return(retval);
}

STATIC int
sysdoproc_filt_KERN_PROC_UID(proc_t p, void * arg)
{
	kauth_cred_t my_cred;
	uid_t uid;

	if (p->p_ucred == NULL)
		return(0);
	my_cred = kauth_cred_proc_ref(p);
	uid = kauth_cred_getuid(my_cred);
	kauth_cred_unref(&my_cred);

	if (uid != (uid_t)*(int*)arg)
		return(0);
	else
		return(1);
}


STATIC int
sysdoproc_filt_KERN_PROC_RUID(proc_t p, void * arg)
{
	kauth_cred_t my_cred;
	uid_t ruid;

	if (p->p_ucred == NULL)
		return(0);
	my_cred = kauth_cred_proc_ref(p);
	ruid = kauth_cred_getruid(my_cred);
	kauth_cred_unref(&my_cred);

	if (ruid != (uid_t)*(int*)arg)
		return(0);
	else
		return(1);
}

#if CONFIG_LCTX
STATIC int
sysdoproc_filt_KERN_PROC_LCID(proc_t p, void * arg)
{
	if ((p->p_lctx == NULL) ||
		(p->p_lctx->lc_id != (pid_t)*(int*)arg))
		return(0);
	else
		return(1);
}
#endif

/*
 * try over estimating by 5 procs
 */
#define KERN_PROCSLOP	(5 * sizeof (struct kinfo_proc))
struct sysdoproc_args {
	int	buflen;
	void	*kprocp;
	boolean_t is_64_bit;
	user_addr_t	dp;
	size_t needed;
	int sizeof_kproc;
	int *errorp;
	int uidcheck;
	int ruidcheck;
	int ttycheck;
	int uidval;
};

int
sysdoproc_callback(proc_t p, void *arg)
{
	struct sysdoproc_args *args = arg;

	if (args->buflen >= args->sizeof_kproc) {
		if ((args->ruidcheck != 0) && (sysdoproc_filt_KERN_PROC_RUID(p, &args->uidval) == 0))
			return (PROC_RETURNED);
		if ((args->uidcheck != 0) && (sysdoproc_filt_KERN_PROC_UID(p, &args->uidval) == 0))
			return (PROC_RETURNED);
		if ((args->ttycheck != 0) && (sysdoproc_filt_KERN_PROC_TTY(p, &args->uidval) == 0))
			return (PROC_RETURNED);

		bzero(args->kprocp, args->sizeof_kproc);
		if (args->is_64_bit)
			fill_user64_proc(p, args->kprocp);
		else
			fill_user32_proc(p, args->kprocp);
		int error = copyout(args->kprocp, args->dp, args->sizeof_kproc);
		if (error) {
			*args->errorp = error;
			return (PROC_RETURNED_DONE);
		}
		args->dp += args->sizeof_kproc;
		args->buflen -= args->sizeof_kproc;
	}
	args->needed += args->sizeof_kproc;
	return (PROC_RETURNED);
}

SYSCTL_NODE(_kern, KERN_PROC, proc, CTLFLAG_RD | CTLFLAG_LOCKED, 0, "");
STATIC int
sysctl_prochandle SYSCTL_HANDLER_ARGS
{
	int cmd = oidp->oid_arg2;	/* subcommand for multiple nodes */
	int *name = arg1;		/* oid element argument vector */
	int namelen = arg2;		/* number of oid element arguments */
	user_addr_t where = req->oldptr;/* user buffer copy out address */

	user_addr_t dp = where;
	size_t needed = 0;
	int buflen = where != USER_ADDR_NULL ? req->oldlen : 0;
	int error = 0;
	boolean_t is_64_bit = proc_is64bit(current_proc());
	struct user32_kinfo_proc  user32_kproc;
	struct user64_kinfo_proc  user_kproc;
	int sizeof_kproc;
	void *kprocp;
	int (*filterfn)(proc_t, void *) = 0;
	struct sysdoproc_args args;
	int uidcheck = 0;
	int ruidcheck = 0;
	int ttycheck = 0;

	if (namelen != 1 && !(namelen == 0 && cmd == KERN_PROC_ALL))
		return (EINVAL);

	if (is_64_bit) {
		sizeof_kproc = sizeof(user_kproc);
		kprocp = &user_kproc;
	} else {
		sizeof_kproc = sizeof(user32_kproc);
		kprocp = &user32_kproc;
	}

	switch (cmd) {

		case KERN_PROC_PID:
			filterfn = sysdoproc_filt_KERN_PROC_PID;
			break;

		case KERN_PROC_PGRP:
			filterfn = sysdoproc_filt_KERN_PROC_PGRP;
			break;
	
		case KERN_PROC_TTY:
			ttycheck = 1;
			break;

		case KERN_PROC_UID:
			uidcheck = 1;
			break;

		case KERN_PROC_RUID:
			ruidcheck = 1;
			break;

#if CONFIG_LCTX
		case KERN_PROC_LCID:
			filterfn = sysdoproc_filt_KERN_PROC_LCID;
			break;
#endif
		case KERN_PROC_ALL:
			break;

		default:
			/* must be kern.proc.<unknown> */
			return (ENOTSUP);
	}

	error = 0;
	args.buflen = buflen;
	args.kprocp = kprocp;
	args.is_64_bit = is_64_bit;
	args.dp = dp;
	args.needed = needed;
	args.errorp = &error;
	args.uidcheck = uidcheck;
	args.ruidcheck = ruidcheck;
	args.ttycheck = ttycheck;
	args.sizeof_kproc = sizeof_kproc;
	if (namelen)
		args.uidval = name[0];

	proc_iterate((PROC_ALLPROCLIST | PROC_ZOMBPROCLIST),
	    sysdoproc_callback, &args, filterfn, name);

	if (error)
		return (error);

	dp = args.dp;
	needed = args.needed;
	
	if (where != USER_ADDR_NULL) {
		req->oldlen = dp - where;
		if (needed > req->oldlen)
			return (ENOMEM);
	} else {
		needed += KERN_PROCSLOP;
		req->oldlen = needed;
	}
	/* adjust index so we return the right required/consumed amount */
	req->oldidx += req->oldlen;
	return (0);
}

/*
 * We specify the subcommand code for multiple nodes as the 'req->arg2' value
 * in the sysctl declaration itself, which comes into the handler function
 * as 'oidp->oid_arg2'.
 *
 * For these particular sysctls, since they have well known OIDs, we could
 * have just obtained it from the '((int *)arg1)[0]' parameter, but that would
 * not demonstrate how to handle multiple sysctls that used OID_AUTO instead
 * of a well known value with a common handler function.  This is desirable,
 * because we want well known values to "go away" at some future date.
 *
 * It should be noted that the value of '((int *)arg1)[1]' is used for many
 * an integer parameter to the subcommand for many of these sysctls; we'd
 * rather have used '((int *)arg1)[0]' for that, or even better, an element
 * in a structure passed in as the the 'newp' argument to sysctlbyname(3),
 * and then use leaf-node permissions enforcement, but that would have
 * necessitated modifying user space code to correspond to the interface
 * change, and we are striving for binary backward compatibility here; even
 * though these are SPI, and not intended for use by user space applications
 * which are not themselves system tools or libraries, some applications
 * have erroneously used them.
 */
SYSCTL_PROC(_kern_proc, KERN_PROC_ALL, all, CTLTYPE_NODE|CTLFLAG_RD | CTLFLAG_LOCKED,
	0,			/* Pointer argument (arg1) */
	KERN_PROC_ALL,		/* Integer argument (arg2) */
	sysctl_prochandle,	/* Handler function */
	NULL,			/* Data is size variant on ILP32/LP64 */
	"");
SYSCTL_PROC(_kern_proc, KERN_PROC_PID, pid, CTLTYPE_NODE|CTLFLAG_RD | CTLFLAG_LOCKED,
	0,			/* Pointer argument (arg1) */
	KERN_PROC_PID,		/* Integer argument (arg2) */
	sysctl_prochandle,	/* Handler function */
	NULL,			/* Data is size variant on ILP32/LP64 */
	"");
SYSCTL_PROC(_kern_proc, KERN_PROC_TTY, tty, CTLTYPE_NODE|CTLFLAG_RD | CTLFLAG_LOCKED,
	0,			/* Pointer argument (arg1) */
	KERN_PROC_TTY,		/* Integer argument (arg2) */
	sysctl_prochandle,	/* Handler function */
	NULL,			/* Data is size variant on ILP32/LP64 */
	"");
SYSCTL_PROC(_kern_proc, KERN_PROC_PGRP, pgrp, CTLTYPE_NODE|CTLFLAG_RD | CTLFLAG_LOCKED,
	0,			/* Pointer argument (arg1) */
	KERN_PROC_PGRP,		/* Integer argument (arg2) */
	sysctl_prochandle,	/* Handler function */
	NULL,			/* Data is size variant on ILP32/LP64 */
	"");
SYSCTL_PROC(_kern_proc, KERN_PROC_UID, uid, CTLTYPE_NODE|CTLFLAG_RD | CTLFLAG_LOCKED,
	0,			/* Pointer argument (arg1) */
	KERN_PROC_UID,		/* Integer argument (arg2) */
	sysctl_prochandle,	/* Handler function */
	NULL,			/* Data is size variant on ILP32/LP64 */
	"");
SYSCTL_PROC(_kern_proc, KERN_PROC_RUID, ruid, CTLTYPE_NODE|CTLFLAG_RD | CTLFLAG_LOCKED,
	0,			/* Pointer argument (arg1) */
	KERN_PROC_RUID,		/* Integer argument (arg2) */
	sysctl_prochandle,	/* Handler function */
	NULL,			/* Data is size variant on ILP32/LP64 */
	"");
SYSCTL_PROC(_kern_proc, KERN_PROC_LCID, lcid, CTLTYPE_NODE|CTLFLAG_RD | CTLFLAG_LOCKED,
	0,			/* Pointer argument (arg1) */
	KERN_PROC_LCID,		/* Integer argument (arg2) */
	sysctl_prochandle,	/* Handler function */
	NULL,			/* Data is size variant on ILP32/LP64 */
	"");


/*
 * Fill in non-zero fields of an eproc structure for the specified process.
 */
STATIC void
fill_user32_eproc(proc_t p, struct user32_eproc *__restrict ep)
{
	struct tty *tp;
	struct pgrp *pg;
	struct session *sessp;
	kauth_cred_t my_cred;

	pg = proc_pgrp(p);
	sessp = proc_session(p);

	if (pg != PGRP_NULL) {
		ep->e_pgid = p->p_pgrpid;
		ep->e_jobc = pg->pg_jobc;
		if (sessp != SESSION_NULL && sessp->s_ttyvp)
			ep->e_flag = EPROC_CTTY;
	}
#if CONFIG_LCTX
	if (p->p_lctx)
		ep->e_lcid = p->p_lctx->lc_id;
#endif
	ep->e_ppid = p->p_ppid;
	if (p->p_ucred) {
		my_cred = kauth_cred_proc_ref(p);

		/* A fake historical pcred */
		ep->e_pcred.p_ruid = kauth_cred_getruid(my_cred);
		ep->e_pcred.p_svuid = kauth_cred_getsvuid(my_cred);
		ep->e_pcred.p_rgid = kauth_cred_getrgid(my_cred);
		ep->e_pcred.p_svgid = kauth_cred_getsvgid(my_cred);

		/* A fake historical *kauth_cred_t */
		ep->e_ucred.cr_ref = my_cred->cr_ref;
		ep->e_ucred.cr_uid = kauth_cred_getuid(my_cred);
		ep->e_ucred.cr_ngroups = posix_cred_get(my_cred)->cr_ngroups;
		bcopy(posix_cred_get(my_cred)->cr_groups,
			ep->e_ucred.cr_groups, NGROUPS * sizeof (gid_t));

		kauth_cred_unref(&my_cred);
	}

	if ((p->p_flag & P_CONTROLT) && (sessp != SESSION_NULL) &&
	     (tp = SESSION_TP(sessp))) {
		ep->e_tdev = tp->t_dev;
		ep->e_tpgid = sessp->s_ttypgrpid;
	} else
		ep->e_tdev = NODEV;

	if (sessp != SESSION_NULL) {
		if (SESS_LEADER(p, sessp))
			ep->e_flag |= EPROC_SLEADER;
		session_rele(sessp);
	}
	if (pg != PGRP_NULL)
		pg_rele(pg);
}

/*
 * Fill in non-zero fields of an LP64 eproc structure for the specified process.
 */
STATIC void
fill_user64_eproc(proc_t p, struct user64_eproc *__restrict ep)
{
	struct tty *tp;
	struct pgrp *pg;
	struct session *sessp;
	kauth_cred_t my_cred;
	
	pg = proc_pgrp(p);
	sessp = proc_session(p);

	if (pg != PGRP_NULL) {
		ep->e_pgid = p->p_pgrpid;
		ep->e_jobc = pg->pg_jobc;
		if (sessp != SESSION_NULL && sessp->s_ttyvp)
			ep->e_flag = EPROC_CTTY;
	}
#if CONFIG_LCTX
	if (p->p_lctx)
		ep->e_lcid = p->p_lctx->lc_id;
#endif
	ep->e_ppid = p->p_ppid;
	if (p->p_ucred) {
		my_cred = kauth_cred_proc_ref(p);

		/* A fake historical pcred */
		ep->e_pcred.p_ruid = kauth_cred_getruid(my_cred);
		ep->e_pcred.p_svuid = kauth_cred_getsvuid(my_cred);
		ep->e_pcred.p_rgid = kauth_cred_getrgid(my_cred);
		ep->e_pcred.p_svgid = kauth_cred_getsvgid(my_cred);

		/* A fake historical *kauth_cred_t */
		ep->e_ucred.cr_ref = my_cred->cr_ref;
		ep->e_ucred.cr_uid = kauth_cred_getuid(my_cred);
		ep->e_ucred.cr_ngroups = posix_cred_get(my_cred)->cr_ngroups;
		bcopy(posix_cred_get(my_cred)->cr_groups,
			ep->e_ucred.cr_groups, NGROUPS * sizeof (gid_t));

		kauth_cred_unref(&my_cred);
	}

	if ((p->p_flag & P_CONTROLT) && (sessp != SESSION_NULL) &&
	     (tp = SESSION_TP(sessp))) {
		ep->e_tdev = tp->t_dev;
		ep->e_tpgid = sessp->s_ttypgrpid;
	} else
		ep->e_tdev = NODEV;

	if (sessp != SESSION_NULL) {
		if (SESS_LEADER(p, sessp))
			ep->e_flag |= EPROC_SLEADER;
		session_rele(sessp);
	}
	if (pg != PGRP_NULL)
		pg_rele(pg);
}

/*
 * Fill in an eproc structure for the specified process.
 * bzeroed by our caller, so only set non-zero fields.
 */
STATIC void
fill_user32_externproc(proc_t p, struct user32_extern_proc *__restrict exp)
{
	exp->p_starttime.tv_sec = p->p_start.tv_sec;
	exp->p_starttime.tv_usec = p->p_start.tv_usec;
	exp->p_flag = p->p_flag;
	if (p->p_lflag & P_LTRACED)
		exp->p_flag |= P_TRACED;
	if (p->p_lflag & P_LPPWAIT)
		exp->p_flag |= P_PPWAIT;
	if (p->p_lflag & P_LEXIT)
		exp->p_flag |= P_WEXIT;
	exp->p_stat = p->p_stat;
	exp->p_pid = p->p_pid;
	exp->p_oppid = p->p_oppid;
	/* Mach related  */
	exp->user_stack = p->user_stack;
	exp->p_debugger = p->p_debugger;
	exp->sigwait = p->sigwait;
	/* scheduling */
#ifdef _PROC_HAS_SCHEDINFO_
	exp->p_estcpu = p->p_estcpu;
	exp->p_pctcpu = p->p_pctcpu;
	exp->p_slptime = p->p_slptime;
#endif
	exp->p_realtimer.it_interval.tv_sec =
		(user32_time_t)p->p_realtimer.it_interval.tv_sec;
	exp->p_realtimer.it_interval.tv_usec =
		(__int32_t)p->p_realtimer.it_interval.tv_usec;

	exp->p_realtimer.it_value.tv_sec =
		(user32_time_t)p->p_realtimer.it_value.tv_sec;
	exp->p_realtimer.it_value.tv_usec =
		(__int32_t)p->p_realtimer.it_value.tv_usec;

	exp->p_rtime.tv_sec = (user32_time_t)p->p_rtime.tv_sec;
	exp->p_rtime.tv_usec = (__int32_t)p->p_rtime.tv_usec;

	exp->p_sigignore = p->p_sigignore;
	exp->p_sigcatch = p->p_sigcatch;
	exp->p_priority = p->p_priority;
	exp->p_nice = p->p_nice;
	bcopy(&p->p_comm, &exp->p_comm, MAXCOMLEN);
	exp->p_xstat = p->p_xstat;
	exp->p_acflag = p->p_acflag;
}

/*
 * Fill in an LP64 version of extern_proc structure for the specified process.
 */
STATIC void
fill_user64_externproc(proc_t p, struct user64_extern_proc *__restrict exp)
{
	exp->p_starttime.tv_sec = p->p_start.tv_sec;
	exp->p_starttime.tv_usec = p->p_start.tv_usec;
	exp->p_flag = p->p_flag;
	if (p->p_lflag & P_LTRACED)
		exp->p_flag |= P_TRACED;
	if (p->p_lflag & P_LPPWAIT)
		exp->p_flag |= P_PPWAIT;
	if (p->p_lflag & P_LEXIT)
		exp->p_flag |= P_WEXIT;
	exp->p_stat = p->p_stat;
	exp->p_pid = p->p_pid;
	exp->p_oppid = p->p_oppid;
	/* Mach related  */
	exp->user_stack = p->user_stack;
	exp->p_debugger = p->p_debugger;
	exp->sigwait = p->sigwait;
	/* scheduling */
#ifdef _PROC_HAS_SCHEDINFO_
	exp->p_estcpu = p->p_estcpu;
	exp->p_pctcpu = p->p_pctcpu;
	exp->p_slptime = p->p_slptime;
#endif
	exp->p_realtimer.it_interval.tv_sec = p->p_realtimer.it_interval.tv_sec;
	exp->p_realtimer.it_interval.tv_usec = p->p_realtimer.it_interval.tv_usec;

	exp->p_realtimer.it_value.tv_sec = p->p_realtimer.it_value.tv_sec;
	exp->p_realtimer.it_value.tv_usec = p->p_realtimer.it_value.tv_usec;

	exp->p_rtime.tv_sec = p->p_rtime.tv_sec;
	exp->p_rtime.tv_usec = p->p_rtime.tv_usec;

	exp->p_sigignore = p->p_sigignore;
	exp->p_sigcatch = p->p_sigcatch;
	exp->p_priority = p->p_priority;
	exp->p_nice = p->p_nice;
	bcopy(&p->p_comm, &exp->p_comm, MAXCOMLEN);
	exp->p_xstat = p->p_xstat;
	exp->p_acflag = p->p_acflag;
}

STATIC void
fill_user32_proc(proc_t p, struct user32_kinfo_proc *__restrict kp)
{
	/* on a 64 bit kernel, 32 bit users get some truncated information */
	fill_user32_externproc(p, &kp->kp_proc);
	fill_user32_eproc(p, &kp->kp_eproc);
}

STATIC void
fill_user64_proc(proc_t p, struct user64_kinfo_proc *__restrict kp)
{
	fill_user64_externproc(p, &kp->kp_proc);
	fill_user64_eproc(p, &kp->kp_eproc);
}

STATIC int
sysctl_kdebug_ops SYSCTL_HANDLER_ARGS
{
	__unused int cmd = oidp->oid_arg2;	/* subcommand*/
	int *name = arg1;		/* oid element argument vector */
	int namelen = arg2;		/* number of oid element arguments */
	user_addr_t oldp = req->oldptr;	/* user buffer copy out address */
	size_t *oldlenp = &req->oldlen;	/* user buffer copy out size */
//	user_addr_t newp = req->newptr;	/* user buffer copy in address */
//	size_t newlen = req->newlen;	/* user buffer copy in size */

	proc_t p = current_proc();
	int ret=0;

	if (namelen == 0)
		return(ENOTSUP);
	
	ret = suser(kauth_cred_get(), &p->p_acflag);
	if (ret)
		return(ret);
	
	switch(name[0]) {
	case KERN_KDEFLAGS:
	case KERN_KDDFLAGS:
	case KERN_KDENABLE:
	case KERN_KDGETBUF:
	case KERN_KDSETUP:
	case KERN_KDREMOVE:
	case KERN_KDSETREG:
	case KERN_KDGETREG:
	case KERN_KDREADTR:
        case KERN_KDWRITETR:
        case KERN_KDWRITEMAP:
	case KERN_KDPIDTR:
	case KERN_KDTHRMAP:
	case KERN_KDPIDEX:
	case KERN_KDSETRTCDEC:
	case KERN_KDSETBUF:
	case KERN_KDGETENTROPY:
	case KERN_KDENABLE_BG_TRACE:
	case KERN_KDDISABLE_BG_TRACE:
	case KERN_KDSET_TYPEFILTER:

	        ret = kdbg_control(name, namelen, oldp, oldlenp);
	        break;
	default:
		ret= ENOTSUP;
		break;
	}

	/* adjust index so we return the right required/consumed amount */
	if (!ret)
		req->oldidx += req->oldlen;

	return (ret);
}
SYSCTL_PROC(_kern, KERN_KDEBUG, kdebug, CTLTYPE_NODE|CTLFLAG_RD | CTLFLAG_LOCKED,
	0,			/* Pointer argument (arg1) */
	0,			/* Integer argument (arg2) */
	sysctl_kdebug_ops,	/* Handler function */
	NULL,			/* Data pointer */
	"");


#if !CONFIG_EMBEDDED
/*
 * Return the top *sizep bytes of the user stack, or the entire area of the
 * user stack down through the saved exec_path, whichever is smaller.
 */
STATIC int
sysctl_doprocargs SYSCTL_HANDLER_ARGS
{
	__unused int cmd = oidp->oid_arg2;	/* subcommand*/
	int *name = arg1;		/* oid element argument vector */
	int namelen = arg2;		/* number of oid element arguments */
	user_addr_t oldp = req->oldptr;	/* user buffer copy out address */
	size_t *oldlenp = &req->oldlen;	/* user buffer copy out size */
//	user_addr_t newp = req->newptr;	/* user buffer copy in address */
//	size_t newlen = req->newlen;	/* user buffer copy in size */
	int error;

	error =  sysctl_procargsx( name, namelen, oldp, oldlenp, current_proc(), 0);

	/* adjust index so we return the right required/consumed amount */
	if (!error)
		req->oldidx += req->oldlen;

	return (error);
}
SYSCTL_PROC(_kern, KERN_PROCARGS, procargs, CTLTYPE_NODE|CTLFLAG_RD | CTLFLAG_LOCKED,
	0,			/* Pointer argument (arg1) */
	0,			/* Integer argument (arg2) */
	sysctl_doprocargs,	/* Handler function */
	NULL,			/* Data pointer */
	"");
#endif	/* !CONFIG_EMBEDDED */

STATIC int
sysctl_doprocargs2 SYSCTL_HANDLER_ARGS
{
	__unused int cmd = oidp->oid_arg2;	/* subcommand*/
	int *name = arg1;		/* oid element argument vector */
	int namelen = arg2;		/* number of oid element arguments */
	user_addr_t oldp = req->oldptr;	/* user buffer copy out address */
	size_t *oldlenp = &req->oldlen;	/* user buffer copy out size */
//	user_addr_t newp = req->newptr;	/* user buffer copy in address */
//	size_t newlen = req->newlen;	/* user buffer copy in size */
	int error;

	error = sysctl_procargsx( name, namelen, oldp, oldlenp, current_proc(), 1);

	/* adjust index so we return the right required/consumed amount */
	if (!error)
		req->oldidx += req->oldlen;

	return (error);
}
SYSCTL_PROC(_kern, KERN_PROCARGS2, procargs2, CTLTYPE_NODE|CTLFLAG_RD | CTLFLAG_LOCKED,
	0,			/* Pointer argument (arg1) */
	0,			/* Integer argument (arg2) */
	sysctl_doprocargs2,	/* Handler function */
	NULL,			/* Data pointer */
	"");

STATIC int
sysctl_procargsx(int *name, u_int namelen, user_addr_t where, 
                 size_t *sizep, proc_t cur_proc, int argc_yes)
{
	proc_t p;
	int buflen = where != USER_ADDR_NULL ? *sizep : 0;
	int error = 0;
	struct _vm_map *proc_map;
	struct task * task;
	vm_map_copy_t	tmp;
	user_addr_t	arg_addr;
	size_t		arg_size;
	caddr_t data;
	size_t argslen=0;
	int size;
	vm_offset_t	copy_start, copy_end;
	kern_return_t ret;
	int pid;
	kauth_cred_t my_cred;
	uid_t uid;

	if ( namelen < 1 )
		return(EINVAL);
	
	if (argc_yes)
		buflen -= sizeof(int);		/* reserve first word to return argc */

	/* we only care about buflen when where (oldp from sysctl) is not NULL. */
	/* when where (oldp from sysctl) is NULL and sizep (oldlenp from sysctl */
	/* is not NULL then the caller wants us to return the length needed to */
	/* hold the data we would return */ 
	if (where != USER_ADDR_NULL && (buflen <= 0 || buflen > ARG_MAX)) {
		return(EINVAL);
	}
	arg_size = buflen;

	/*
	 *	Lookup process by pid
	 */
	pid = name[0];
	p = proc_find(pid);
	if (p == NULL) {
		return(EINVAL);
	}

	/*
	 *	Copy the top N bytes of the stack.
	 *	On all machines we have so far, the stack grows
	 *	downwards.
	 *
	 *	If the user expects no more than N bytes of
	 *	argument list, use that as a guess for the
	 *	size.
	 */

	if (!p->user_stack) {
		proc_rele(p);
		return(EINVAL);
	}

	if (where == USER_ADDR_NULL) {
		/* caller only wants to know length of proc args data */
		if (sizep == NULL) {
			proc_rele(p);
			return(EFAULT);
		}
			
		 size = p->p_argslen;
		proc_rele(p);
		 if (argc_yes) {
		 	size += sizeof(int);
		 }
		 else {
			/*
			 * old PROCARGS will return the executable's path and plus some
			 * extra space for work alignment and data tags
			 */
		 	size += PATH_MAX + (6 * sizeof(int));
		 }
		size += (size & (sizeof(int) - 1)) ? (sizeof(int) - (size & (sizeof(int) - 1))) : 0;
		*sizep = size;
		return (0);
	}
	
	my_cred = kauth_cred_proc_ref(p);
	uid = kauth_cred_getuid(my_cred);
	kauth_cred_unref(&my_cred);

	if ((uid != kauth_cred_getuid(kauth_cred_get())) 
		&& suser(kauth_cred_get(), &cur_proc->p_acflag)) {
		proc_rele(p);
		return (EINVAL);
	}

	if ((u_int)arg_size > p->p_argslen)
	        arg_size = round_page(p->p_argslen);

	arg_addr = p->user_stack - arg_size;


	/*
	 *	Before we can block (any VM code), make another
	 *	reference to the map to keep it alive.  We do
	 *	that by getting a reference on the task itself.
	 */
	task = p->task;
	if (task == NULL) {
		proc_rele(p);
		return(EINVAL);
	}
	
	argslen = p->p_argslen;
	/*
	 * Once we have a task reference we can convert that into a
	 * map reference, which we will use in the calls below.  The
	 * task/process may change its map after we take this reference
	 * (see execve), but the worst that will happen then is a return
	 * of stale info (which is always a possibility).
	 */
	task_reference(task);
	proc_rele(p);
	proc_map = get_task_map_reference(task);
	task_deallocate(task);
	
	if (proc_map == NULL)
		return(EINVAL);


	ret = kmem_alloc(kernel_map, &copy_start, round_page(arg_size));
	if (ret != KERN_SUCCESS) {
		vm_map_deallocate(proc_map);
		return(ENOMEM);
	}

	copy_end = round_page(copy_start + arg_size);

	if( vm_map_copyin(proc_map, (vm_map_address_t)arg_addr, 
			  (vm_map_size_t)arg_size, FALSE, &tmp) != KERN_SUCCESS) {
			vm_map_deallocate(proc_map);
			kmem_free(kernel_map, copy_start,
					round_page(arg_size));
			return (EIO);
	}

	/*
	 *	Now that we've done the copyin from the process'
	 *	map, we can release the reference to it.
	 */
	vm_map_deallocate(proc_map);

	if( vm_map_copy_overwrite(kernel_map, 
				  (vm_map_address_t)copy_start, 
				  tmp, FALSE) != KERN_SUCCESS) {
			kmem_free(kernel_map, copy_start,
					round_page(arg_size));
			return (EIO);
	}

	if (arg_size > argslen) {
		data = (caddr_t) (copy_end - argslen);
		size = argslen;
	} else {
		data = (caddr_t) (copy_end - arg_size);
		size = arg_size;
	}

	if (argc_yes) {
		/* Put processes argc as the first word in the copyout buffer */
		suword(where, p->p_argc);
		error = copyout(data, (where + sizeof(int)), size);
		size += sizeof(int);
	} else {
		error = copyout(data, where, size);

		/*
		 * Make the old PROCARGS work to return the executable's path
		 * But, only if there is enough space in the provided buffer
		 *
		 * on entry: data [possibily] points to the beginning of the path 
		 * 
		 * Note: we keep all pointers&sizes aligned to word boundries
		 */
		if ( (! error) && (buflen > 0 && (u_int)buflen > argslen) )
		{
			int binPath_sz, alignedBinPath_sz = 0;
			int extraSpaceNeeded, addThis;
			user_addr_t placeHere;
			char * str = (char *) data;
			int max_len = size;

			/* Some apps are really bad about messing up their stacks
			   So, we have to be extra careful about getting the length
			   of the executing binary.  If we encounter an error, we bail.
			*/

			/* Limit ourselves to PATH_MAX paths */
			if ( max_len > PATH_MAX ) max_len = PATH_MAX;

			binPath_sz = 0;

			while ( (binPath_sz < max_len-1) && (*str++ != 0) )
				binPath_sz++;

			/* If we have a NUL terminator, copy it, too */
			if (binPath_sz < max_len-1) binPath_sz += 1;

			/* Pre-Flight the space requiremnts */

			/* Account for the padding that fills out binPath to the next word */
			alignedBinPath_sz += (binPath_sz & (sizeof(int)-1)) ? (sizeof(int)-(binPath_sz & (sizeof(int)-1))) : 0;

			placeHere = where + size;

			/* Account for the bytes needed to keep placeHere word aligned */ 
			addThis = (placeHere & (sizeof(int)-1)) ? (sizeof(int)-(placeHere & (sizeof(int)-1))) : 0;

			/* Add up all the space that is needed */
			extraSpaceNeeded = alignedBinPath_sz + addThis + binPath_sz + (4 * sizeof(int));

			/* is there is room to tack on argv[0]? */
			if ( (buflen & ~(sizeof(int)-1)) >= ( argslen + extraSpaceNeeded ))
			{
				placeHere += addThis;
				suword(placeHere, 0);
				placeHere += sizeof(int);
				suword(placeHere, 0xBFFF0000);
				placeHere += sizeof(int);
				suword(placeHere, 0);
				placeHere += sizeof(int);
				error = copyout(data, placeHere, binPath_sz);
				if ( ! error )
				{
					placeHere += binPath_sz;
					suword(placeHere, 0);
					size += extraSpaceNeeded;
				}
			}
		}
	}

	if (copy_start != (vm_offset_t) 0) {
		kmem_free(kernel_map, copy_start, copy_end - copy_start);
	}
	if (error) {
		return(error);
	}

	if (where != USER_ADDR_NULL)
		*sizep = size;
	return (0);
}


/*
 * Max number of concurrent aio requests
 */
STATIC int
sysctl_aiomax
(__unused struct sysctl_oid *oidp, __unused void *arg1, __unused int arg2, struct sysctl_req *req)
{
	int new_value, changed;
	int error = sysctl_io_number(req, aio_max_requests, sizeof(int), &new_value, &changed);
	if (changed) {
		 /* make sure the system-wide limit is greater than the per process limit */
		if (new_value >= aio_max_requests_per_process && new_value <= AIO_MAX_REQUESTS)
			aio_max_requests = new_value;
		else
			error = EINVAL;
	}
	return(error);
}


/*
 * Max number of concurrent aio requests per process
 */
STATIC int
sysctl_aioprocmax
(__unused struct sysctl_oid *oidp, __unused void *arg1, __unused int arg2, struct sysctl_req *req)
{
	int new_value, changed;
	int error = sysctl_io_number(req, aio_max_requests_per_process, sizeof(int), &new_value, &changed);
	if (changed) {
		/* make sure per process limit is less than the system-wide limit */
		if (new_value <= aio_max_requests && new_value >= AIO_LISTIO_MAX)
			aio_max_requests_per_process = new_value;
		else
			error = EINVAL;
	}
	return(error);
}


/*
 * Max number of async IO worker threads
 */
STATIC int
sysctl_aiothreads
(__unused struct sysctl_oid *oidp, __unused void *arg1, __unused int arg2, struct sysctl_req *req)
{
	int new_value, changed;
	int error = sysctl_io_number(req, aio_worker_threads, sizeof(int), &new_value, &changed);
	if (changed) {
		/* we only allow an increase in the number of worker threads */
	        if (new_value > aio_worker_threads ) {
		        _aio_create_worker_threads((new_value - aio_worker_threads));
			aio_worker_threads = new_value;
		}
		else
		        error = EINVAL;
	}
	return(error);
}


/*
 * System-wide limit on the max number of processes
 */
STATIC int
sysctl_maxproc
(__unused struct sysctl_oid *oidp, __unused void *arg1, __unused int arg2, struct sysctl_req *req)
{
	int new_value, changed;
	int error = sysctl_io_number(req, maxproc, sizeof(int), &new_value, &changed);
	if (changed) {
		AUDIT_ARG(value32, new_value);
		/* make sure the system-wide limit is less than the configured hard
		   limit set at kernel compilation */
		if (new_value <= hard_maxproc && new_value > 0)
			maxproc = new_value;
		else
			error = EINVAL;
	}
	return(error);
}

SYSCTL_STRING(_kern, KERN_OSTYPE, ostype, 
		CTLFLAG_RD | CTLFLAG_KERN | CTLFLAG_LOCKED, 
		ostype, 0, "");
SYSCTL_STRING(_kern, KERN_OSRELEASE, osrelease, 
		CTLFLAG_RD | CTLFLAG_KERN | CTLFLAG_LOCKED, 
		osrelease, 0, "");
SYSCTL_INT(_kern, KERN_OSREV, osrevision, 
		CTLFLAG_RD | CTLFLAG_KERN | CTLFLAG_LOCKED, 
		(int *)NULL, BSD, "");
SYSCTL_STRING(_kern, KERN_VERSION, version, 
		CTLFLAG_RD | CTLFLAG_KERN | CTLFLAG_LOCKED, 
		version, 0, "");
SYSCTL_STRING(_kern, OID_AUTO, uuid, 
		CTLFLAG_RD | CTLFLAG_KERN | CTLFLAG_LOCKED, 
		&kernel_uuid[0], 0, "");

#if DEBUG
int debug_kprint_syscall = 0;
char debug_kprint_syscall_process[MAXCOMLEN+1];

/* Thread safe: bits and string value are not used to reclaim state */
SYSCTL_INT (_debug, OID_AUTO, kprint_syscall,
	    CTLFLAG_RW | CTLFLAG_LOCKED, &debug_kprint_syscall, 0, "kprintf syscall tracing");
SYSCTL_STRING(_debug, OID_AUTO, kprint_syscall_process, 
			  CTLFLAG_RW | CTLFLAG_LOCKED, debug_kprint_syscall_process, sizeof(debug_kprint_syscall_process),
			  "name of process for kprintf syscall tracing");

int debug_kprint_current_process(const char **namep)
{
	struct proc *p = current_proc();

	if (p == NULL) {
		return 0;
	}

	if (debug_kprint_syscall_process[0]) {
		/* user asked to scope tracing to a particular process name */
		if(0 == strncmp(debug_kprint_syscall_process,
						p->p_comm, sizeof(debug_kprint_syscall_process))) {
			/* no value in telling the user that we traced what they asked */
			if(namep) *namep = NULL;

			return 1;
		} else {
			return 0;
		}
	}

	/* trace all processes. Tell user what we traced */
	if (namep) {
		*namep = p->p_comm;
	}

	return 1;
}
#endif

/* PR-5293665: need to use a callback function for kern.osversion to set
 * osversion in IORegistry */

STATIC int
sysctl_osversion(__unused struct sysctl_oid *oidp, void *arg1, int arg2, struct sysctl_req *req)
{
    int rval = 0;

    rval = sysctl_handle_string(oidp, arg1, arg2, req);

    if (req->newptr) {
        IORegistrySetOSBuildVersion((char *)arg1); 
    }

    return rval;
}

SYSCTL_PROC(_kern, KERN_OSVERSION, osversion,
        CTLFLAG_RW | CTLFLAG_KERN | CTLTYPE_STRING | CTLFLAG_LOCKED,
        osversion, 256 /* OSVERSIZE*/, 
        sysctl_osversion, "A", "");

STATIC int
sysctl_sysctl_bootargs
(__unused struct sysctl_oid *oidp, __unused void *arg1, __unused int arg2, struct sysctl_req *req)
{
	int error;
	char buf[256];

	strlcpy(buf, PE_boot_args(), 256);
	error = sysctl_io_string(req, buf, 256, 0, NULL);
	return(error);
}

SYSCTL_PROC(_kern, OID_AUTO, bootargs,
	CTLFLAG_LOCKED | CTLFLAG_RD | CTLFLAG_KERN | CTLTYPE_STRING,
	NULL, 0,
	sysctl_sysctl_bootargs, "A", "bootargs");

SYSCTL_INT(_kern, KERN_MAXFILES, maxfiles, 
		CTLFLAG_RW | CTLFLAG_KERN | CTLFLAG_LOCKED,
		&maxfiles, 0, "");
SYSCTL_INT(_kern, KERN_ARGMAX, argmax, 
		CTLFLAG_RD | CTLFLAG_KERN | CTLFLAG_LOCKED, 
		(int *)NULL, ARG_MAX, "");
SYSCTL_INT(_kern, KERN_POSIX1, posix1version, 
		CTLFLAG_RD | CTLFLAG_KERN | CTLFLAG_LOCKED, 
		(int *)NULL, _POSIX_VERSION, "");
SYSCTL_INT(_kern, KERN_NGROUPS, ngroups, 
		CTLFLAG_RD | CTLFLAG_KERN | CTLFLAG_LOCKED, 
		(int *)NULL, NGROUPS_MAX, "");
SYSCTL_INT(_kern, KERN_JOB_CONTROL, job_control, 
		CTLFLAG_RD | CTLFLAG_KERN | CTLFLAG_LOCKED, 
		(int *)NULL, 1, "");
#if 1	/* _POSIX_SAVED_IDS from <unistd.h> */
SYSCTL_INT(_kern, KERN_SAVED_IDS, saved_ids, 
		CTLFLAG_RD | CTLFLAG_KERN | CTLFLAG_LOCKED, 
		(int *)NULL, 1, "");
#else
SYSCTL_INT(_kern, KERN_SAVED_IDS, saved_ids, 
		CTLFLAG_RD | CTLFLAG_KERN | CTLFLAG_LOCKED, 
		NULL, 0, "");
#endif
SYSCTL_INT(_kern, OID_AUTO, num_files, 
		CTLFLAG_RD | CTLFLAG_LOCKED, 
		&nfiles, 0, "");
SYSCTL_COMPAT_INT(_kern, OID_AUTO, num_vnodes, 
		CTLFLAG_RD | CTLFLAG_LOCKED, 
		&numvnodes, 0, "");
SYSCTL_INT(_kern, OID_AUTO, num_tasks, 
		CTLFLAG_RD | CTLFLAG_LOCKED, 
		&task_max, 0, "");
SYSCTL_INT(_kern, OID_AUTO, num_threads, 
		CTLFLAG_RD | CTLFLAG_LOCKED, 
		&thread_max, 0, "");
SYSCTL_INT(_kern, OID_AUTO, num_taskthreads, 
		CTLFLAG_RD | CTLFLAG_LOCKED, 
		&task_threadmax, 0, "");

STATIC int
sysctl_maxvnodes (__unused struct sysctl_oid *oidp, __unused void *arg1, __unused int arg2, struct sysctl_req *req)
{
	int oldval = desiredvnodes;
	int error = sysctl_io_number(req, desiredvnodes, sizeof(int), &desiredvnodes, NULL);

	if (oldval != desiredvnodes) {
		reset_vmobjectcache(oldval, desiredvnodes);
		resize_namecache(desiredvnodes);
	}

	return(error);
}

SYSCTL_INT(_kern, OID_AUTO, namecache_disabled, 
		CTLFLAG_RW | CTLFLAG_LOCKED, 
		&nc_disabled, 0, ""); 

SYSCTL_PROC(_kern, KERN_MAXVNODES, maxvnodes,
		CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED,
		0, 0, sysctl_maxvnodes, "I", "");

SYSCTL_PROC(_kern, KERN_MAXPROC, maxproc,
		CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED,
		0, 0, sysctl_maxproc, "I", "");

SYSCTL_PROC(_kern, KERN_AIOMAX, aiomax,
		CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED,
		0, 0, sysctl_aiomax, "I", "");

SYSCTL_PROC(_kern, KERN_AIOPROCMAX, aioprocmax,
		CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED,
		0, 0, sysctl_aioprocmax, "I", "");

SYSCTL_PROC(_kern, KERN_AIOTHREADS, aiothreads,
		CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED,
		0, 0, sysctl_aiothreads, "I", "");

STATIC int
sysctl_securelvl
(__unused struct sysctl_oid *oidp, __unused void *arg1, __unused int arg2, struct sysctl_req *req)
{
	int new_value, changed;
	int error = sysctl_io_number(req, securelevel, sizeof(int), &new_value, &changed);
	if (changed) {
		if (!(new_value < securelevel && req->p->p_pid != 1)) {
			proc_list_lock();
			securelevel = new_value;
			proc_list_unlock();
		} else {
			error = EPERM;
		}
	}
	return(error);
}

SYSCTL_PROC(_kern, KERN_SECURELVL, securelevel,
		CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED,
		0, 0, sysctl_securelvl, "I", "");


STATIC int
sysctl_domainname
(__unused struct sysctl_oid *oidp, __unused void *arg1, __unused int arg2, struct sysctl_req *req)
{
	int error, changed;
	error = sysctl_io_string(req, domainname, sizeof(domainname), 0, &changed);
	if (changed) {
		domainnamelen = strlen(domainname);
	}
	return(error);
}

SYSCTL_PROC(_kern, KERN_DOMAINNAME, nisdomainname,
		CTLTYPE_STRING | CTLFLAG_RW | CTLFLAG_LOCKED,
		0, 0, sysctl_domainname, "A", "");

SYSCTL_COMPAT_INT(_kern, KERN_HOSTID, hostid, 
		CTLFLAG_RW | CTLFLAG_KERN | CTLFLAG_LOCKED,
		&hostid, 0, "");

STATIC int
sysctl_hostname
(__unused struct sysctl_oid *oidp, __unused void *arg1, __unused int arg2, struct sysctl_req *req)
{
	int error, changed;
	error = sysctl_io_string(req, hostname, sizeof(hostname), 1, &changed);
	if (changed) {
		hostnamelen = req->newlen;
	}
	return(error);
}


SYSCTL_PROC(_kern, KERN_HOSTNAME, hostname,
		CTLTYPE_STRING | CTLFLAG_RW | CTLFLAG_LOCKED,
		0, 0, sysctl_hostname, "A", "");

STATIC int
sysctl_procname
(__unused struct sysctl_oid *oidp, __unused void *arg1, __unused int arg2, struct sysctl_req *req)
{
	/* Original code allowed writing, I'm copying this, although this all makes
	   no sense to me. Besides, this sysctl is never used. */
	return sysctl_io_string(req, &req->p->p_name[0], (2*MAXCOMLEN+1), 1, NULL);
}

SYSCTL_PROC(_kern, KERN_PROCNAME, procname,
		CTLTYPE_STRING | CTLFLAG_RW | CTLFLAG_ANYBODY | CTLFLAG_LOCKED,
		0, 0, sysctl_procname, "A", "");

SYSCTL_INT(_kern, KERN_SPECULATIVE_READS, speculative_reads_disabled, 
		CTLFLAG_RW | CTLFLAG_KERN | CTLFLAG_LOCKED,
		&speculative_reads_disabled, 0, "");

SYSCTL_INT(_kern, OID_AUTO, ignore_is_ssd, 
		CTLFLAG_RW | CTLFLAG_KERN | CTLFLAG_LOCKED,
		&ignore_is_ssd, 0, "");

SYSCTL_UINT(_kern, OID_AUTO, preheat_pages_max, 
		CTLFLAG_RW | CTLFLAG_KERN | CTLFLAG_LOCKED,
		&preheat_pages_max, 0, "");

SYSCTL_UINT(_kern, OID_AUTO, preheat_pages_min, 
		CTLFLAG_RW | CTLFLAG_KERN | CTLFLAG_LOCKED,
		&preheat_pages_min, 0, "");

SYSCTL_UINT(_kern, OID_AUTO, speculative_prefetch_max, 
		CTLFLAG_RW | CTLFLAG_KERN | CTLFLAG_LOCKED,
		&speculative_prefetch_max, 0, "");

SYSCTL_UINT(_kern, OID_AUTO, speculative_prefetch_max_iosize, 
		CTLFLAG_RW | CTLFLAG_KERN | CTLFLAG_LOCKED,
		&speculative_prefetch_max_iosize, 0, "");

SYSCTL_UINT(_kern, OID_AUTO, vm_page_free_target,
		CTLFLAG_RW | CTLFLAG_KERN | CTLFLAG_LOCKED,
		&vm_page_free_target, 0, "");

SYSCTL_UINT(_kern, OID_AUTO, vm_page_free_min,
		CTLFLAG_RW | CTLFLAG_KERN | CTLFLAG_LOCKED,
		&vm_page_free_min, 0, "");

SYSCTL_UINT(_kern, OID_AUTO, vm_page_free_reserved,
		CTLFLAG_RW | CTLFLAG_KERN | CTLFLAG_LOCKED,
		&vm_page_free_reserved, 0, "");

SYSCTL_UINT(_kern, OID_AUTO, vm_page_speculative_percentage,
		CTLFLAG_RW | CTLFLAG_KERN | CTLFLAG_LOCKED,
		&vm_page_speculative_percentage, 0, "");

SYSCTL_UINT(_kern, OID_AUTO, vm_page_speculative_q_age_ms,
		CTLFLAG_RW | CTLFLAG_KERN | CTLFLAG_LOCKED,
		&vm_page_speculative_q_age_ms, 0, "");

SYSCTL_UINT(_kern, OID_AUTO, vm_max_delayed_work_limit,
		CTLFLAG_RW | CTLFLAG_KERN | CTLFLAG_LOCKED,
		&vm_max_delayed_work_limit, 0, "");

SYSCTL_UINT(_kern, OID_AUTO, vm_max_batch,
		CTLFLAG_RW | CTLFLAG_KERN | CTLFLAG_LOCKED,
		&vm_max_batch, 0, "");


STATIC int
sysctl_boottime
(__unused struct sysctl_oid *oidp, __unused void *arg1, __unused int arg2, struct sysctl_req *req)
{
	time_t tv_sec = boottime_sec();
	struct proc *p = req->p;

	if (proc_is64bit(p)) {
		struct user64_timeval t;
		t.tv_sec = tv_sec;
		t.tv_usec = 0;
		return sysctl_io_opaque(req, &t, sizeof(t), NULL);
	} else {
		struct user32_timeval t;
		t.tv_sec = tv_sec;
		t.tv_usec = 0;
		return sysctl_io_opaque(req, &t, sizeof(t), NULL);
	}
}

SYSCTL_PROC(_kern, KERN_BOOTTIME, boottime,
		CTLTYPE_STRUCT | CTLFLAG_RD | CTLFLAG_LOCKED,
		0, 0, sysctl_boottime, "S,timeval", "");

STATIC int
sysctl_symfile
(__unused struct sysctl_oid *oidp, __unused void *arg1, __unused int arg2, struct sysctl_req *req)
{
	char *str;
	int error = get_kernel_symfile(req->p, &str);
	if (error)
		return (error);
	return sysctl_io_string(req, str, 0, 0, NULL);
}


SYSCTL_PROC(_kern, KERN_SYMFILE, symfile,
		CTLTYPE_STRING | CTLFLAG_RD | CTLFLAG_LOCKED,
		0, 0, sysctl_symfile, "A", "");

#if NFSCLIENT
STATIC int
sysctl_netboot
(__unused struct sysctl_oid *oidp, __unused void *arg1, __unused int arg2, struct sysctl_req *req)
{
	return sysctl_io_number(req, netboot_root(), sizeof(int), NULL, NULL);
}

SYSCTL_PROC(_kern, KERN_NETBOOT, netboot,
		CTLTYPE_INT | CTLFLAG_RD | CTLFLAG_LOCKED,
		0, 0, sysctl_netboot, "I", "");
#endif

#ifdef CONFIG_IMGSRC_ACCESS
/*
 * Legacy--act as if only one layer of nesting is possible.
 */
STATIC int
sysctl_imgsrcdev 
(__unused struct sysctl_oid *oidp, __unused void *arg1, __unused int arg2, struct sysctl_req *req)
{
	vfs_context_t ctx = vfs_context_current();
	vnode_t devvp;
	int result;

	if (!vfs_context_issuser(ctx)) {
		return EPERM;
	}    

	if (imgsrc_rootvnodes[0] == NULL) {
		return ENOENT;
	}    

	result = vnode_getwithref(imgsrc_rootvnodes[0]);
	if (result != 0) {
		return result;
	}
	
	devvp = vnode_mount(imgsrc_rootvnodes[0])->mnt_devvp;
	result = vnode_getwithref(devvp);
	if (result != 0) {
		goto out;
	}

	result = sysctl_io_number(req, vnode_specrdev(devvp), sizeof(dev_t), NULL, NULL);

	vnode_put(devvp);
out:
	vnode_put(imgsrc_rootvnodes[0]);
	return result;
}

SYSCTL_PROC(_kern, OID_AUTO, imgsrcdev,
		CTLTYPE_INT | CTLFLAG_RD | CTLFLAG_LOCKED,
		0, 0, sysctl_imgsrcdev, "I", ""); 

STATIC int
sysctl_imgsrcinfo
(__unused struct sysctl_oid *oidp, __unused void *arg1, __unused int arg2, struct sysctl_req *req)
{
	int error;
	struct imgsrc_info info[MAX_IMAGEBOOT_NESTING];	/* 2 for now, no problem */
	uint32_t i;
	vnode_t rvp, devvp;

	if (imgsrc_rootvnodes[0] == NULLVP) {
		return ENXIO;
	}

	for (i = 0; i < MAX_IMAGEBOOT_NESTING; i++) {
		/*
		 * Go get the root vnode.
		 */
		rvp = imgsrc_rootvnodes[i];
		if (rvp == NULLVP) {
			break;
		}

		error = vnode_get(rvp);
		if (error != 0) {
			return error;
		}

		/* 
		 * For now, no getting at a non-local volume.
		 */
		devvp = vnode_mount(rvp)->mnt_devvp;
		if (devvp == NULL) {
			vnode_put(rvp);
			return EINVAL;	
		}

		error = vnode_getwithref(devvp);
		if (error != 0) {
			vnode_put(rvp);
			return error;
		}

		/*
		 * Fill in info.
		 */
		info[i].ii_dev = vnode_specrdev(devvp);
		info[i].ii_flags = 0;
		info[i].ii_height = i;
		bzero(info[i].ii_reserved, sizeof(info[i].ii_reserved));

		vnode_put(devvp);
		vnode_put(rvp);
	}

	return sysctl_io_opaque(req, info, i * sizeof(info[0]), NULL);
}

SYSCTL_PROC(_kern, OID_AUTO, imgsrcinfo,
		CTLTYPE_OPAQUE | CTLFLAG_RD | CTLFLAG_LOCKED,
		0, 0, sysctl_imgsrcinfo, "I", ""); 

#endif /* CONFIG_IMGSRC_ACCESS */

SYSCTL_INT(_kern, OID_AUTO, timer_coalescing_enabled, 
		CTLFLAG_RW | CTLFLAG_LOCKED,
		&mach_timer_coalescing_enabled, 0, "");

STATIC int
sysctl_usrstack
(__unused struct sysctl_oid *oidp, __unused void *arg1, __unused int arg2, struct sysctl_req *req)
{
	return sysctl_io_number(req, (int)req->p->user_stack, sizeof(int), NULL, NULL);
}

SYSCTL_PROC(_kern, KERN_USRSTACK32, usrstack,
		CTLTYPE_INT | CTLFLAG_RD | CTLFLAG_LOCKED,
		0, 0, sysctl_usrstack, "I", "");

STATIC int
sysctl_usrstack64
(__unused struct sysctl_oid *oidp, __unused void *arg1, __unused int arg2, struct sysctl_req *req)
{
	return sysctl_io_number(req, req->p->user_stack, sizeof(req->p->user_stack), NULL, NULL);
}

SYSCTL_PROC(_kern, KERN_USRSTACK64, usrstack64,
		CTLTYPE_QUAD | CTLFLAG_RD | CTLFLAG_LOCKED,
		0, 0, sysctl_usrstack64, "Q", "");

SYSCTL_STRING(_kern, KERN_COREFILE, corefile, 
		CTLFLAG_RW | CTLFLAG_KERN | CTLFLAG_LOCKED,
		corefilename, sizeof(corefilename), "");

STATIC int
sysctl_coredump
(__unused struct sysctl_oid *oidp, __unused void *arg1, __unused int arg2, struct sysctl_req *req)
{
#ifdef SECURE_KERNEL
	return (ENOTSUP);
#endif
	int new_value, changed;
	int error = sysctl_io_number(req, do_coredump, sizeof(int), &new_value, &changed);
	if (changed) {
		if ((new_value == 0) || (new_value == 1))
			do_coredump = new_value;
		else
			error = EINVAL;
	}
	return(error);
}

SYSCTL_PROC(_kern, KERN_COREDUMP, coredump,
		CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED,
		0, 0, sysctl_coredump, "I", "");

STATIC int
sysctl_suid_coredump
(__unused struct sysctl_oid *oidp, __unused void *arg1, __unused int arg2, struct sysctl_req *req)
{
#ifdef SECURE_KERNEL
	return (ENOTSUP);
#endif
	int new_value, changed;
	int error = sysctl_io_number(req, sugid_coredump, sizeof(int), &new_value, &changed);
	if (changed) {
		if ((new_value == 0) || (new_value == 1))
			sugid_coredump = new_value;
		else
			error = EINVAL;
	}
	return(error);
}

SYSCTL_PROC(_kern, KERN_SUGID_COREDUMP, sugid_coredump,
		CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED,
		0, 0, sysctl_suid_coredump, "I", "");

STATIC int
sysctl_delayterm
(__unused struct sysctl_oid *oidp, __unused void *arg1, __unused int arg2, struct sysctl_req *req)
{
	struct proc *p = req->p;
	int new_value, changed;
	int error = sysctl_io_number(req, (req->p->p_lflag & P_LDELAYTERM)? 1: 0, sizeof(int), &new_value, &changed);
	if (changed) {
		proc_lock(p);
		if (new_value)
			req->p->p_lflag |=  P_LDELAYTERM;
		else
			req->p->p_lflag &=  ~P_LDELAYTERM;
		proc_unlock(p);
	}
	return(error);
}

SYSCTL_PROC(_kern, KERN_PROCDELAYTERM, delayterm,
		CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED,
		0, 0, sysctl_delayterm, "I", "");


STATIC int
sysctl_rage_vnode
(__unused struct sysctl_oid *oidp, __unused void *arg1, __unused int arg2, struct sysctl_req *req)
{
	struct proc *p = req->p;
        struct  uthread *ut;
	int new_value, old_value, changed;
	int error;

	ut = get_bsdthread_info(current_thread());

	if (ut->uu_flag & UT_RAGE_VNODES)
	        old_value = KERN_RAGE_THREAD;
	else if (p->p_lflag & P_LRAGE_VNODES)
	        old_value = KERN_RAGE_PROC;
	else
	        old_value = 0;

	error = sysctl_io_number(req, old_value, sizeof(int), &new_value, &changed);

	if (error == 0) {
	        switch (new_value) {
		case KERN_RAGE_PROC:
		        proc_lock(p);
			p->p_lflag |= P_LRAGE_VNODES;
			proc_unlock(p);
			break;
		case KERN_UNRAGE_PROC:
		        proc_lock(p);
			p->p_lflag &= ~P_LRAGE_VNODES;
			proc_unlock(p);
			break;

		case KERN_RAGE_THREAD:
			ut->uu_flag |= UT_RAGE_VNODES;
			break;
		case KERN_UNRAGE_THREAD:
		        ut = get_bsdthread_info(current_thread());
			ut->uu_flag &= ~UT_RAGE_VNODES;
			break;
		}
	}
	return(error);
}

SYSCTL_PROC(_kern, KERN_RAGEVNODE, rage_vnode,
		CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_ANYBODY | CTLFLAG_LOCKED,
		0, 0, sysctl_rage_vnode, "I", "");

/* XXX move this interface into libproc and remove this sysctl */
STATIC int
sysctl_setthread_cpupercent
(__unused struct sysctl_oid *oidp, __unused void *arg1, __unused int arg2, struct sysctl_req *req)
{
	int new_value, old_value;
	int error = 0;
	kern_return_t kret = KERN_SUCCESS;
	uint8_t percent = 0;
	int ms_refill = 0;

	old_value = 0;

	if ((error = sysctl_io_number(req, old_value, sizeof(old_value), &new_value, NULL)) != 0)
		return (error);

	percent = new_value & 0xff;			/* low 8 bytes for perent */
	ms_refill = (new_value >> 8) & 0xffffff;	/* upper 24bytes represent ms refill value */
	if (percent > 100)
		return (EINVAL);

	/*
	 * If the caller is specifying a percentage of 0, this will unset the CPU limit, if present.
	 */
	if ((kret = thread_set_cpulimit(THREAD_CPULIMIT_BLOCK, percent, ms_refill * NSEC_PER_MSEC)) != 0)
		return (EIO);
	
	return (0);
}

SYSCTL_PROC(_kern, OID_AUTO, setthread_cpupercent,
		CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_ANYBODY,
		0, 0, sysctl_setthread_cpupercent, "I", "set thread cpu percentage limit");


STATIC int
sysctl_kern_check_openevt
(__unused struct sysctl_oid *oidp, __unused void *arg1, __unused int arg2, struct sysctl_req *req)
{
	struct proc *p = req->p;
	int new_value, old_value, changed;
	int error;

	if (p->p_flag & P_CHECKOPENEVT) {
		old_value = KERN_OPENEVT_PROC;
	} else {
	        old_value = 0;
	}

	error = sysctl_io_number(req, old_value, sizeof(int), &new_value, &changed);

	if (error == 0) {
	        switch (new_value) {
		case KERN_OPENEVT_PROC:
			OSBitOrAtomic(P_CHECKOPENEVT, &p->p_flag);
			break;

		case KERN_UNOPENEVT_PROC:
			OSBitAndAtomic(~((uint32_t)P_CHECKOPENEVT), &p->p_flag);
			break;

		default:
			error = EINVAL;
		}
	}
	return(error);
}

SYSCTL_PROC(_kern, KERN_CHECKOPENEVT, check_openevt, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_ANYBODY | CTLFLAG_LOCKED,
            0, 0, sysctl_kern_check_openevt, "I", "set the per-process check-open-evt flag");



STATIC int
sysctl_nx
(__unused struct sysctl_oid *oidp, __unused void *arg1, __unused int arg2, struct sysctl_req *req)
{
#ifdef SECURE_KERNEL
	return ENOTSUP;
#endif
	int new_value, changed;
	int error;

	error = sysctl_io_number(req, nx_enabled, sizeof(nx_enabled), &new_value, &changed);
	if (error)
		return error;

	if (changed) {
#if defined(__i386__) || defined(__x86_64__)
		/*
		 * Only allow setting if NX is supported on the chip
		 */
		if (!(cpuid_extfeatures() & CPUID_EXTFEATURE_XD))
			return ENOTSUP;
#endif
		nx_enabled = new_value;
	}
	return(error);
}



SYSCTL_PROC(_kern, KERN_NX_PROTECTION, nx, 
		CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_KERN | CTLFLAG_LOCKED,
		0, 0, sysctl_nx, "I", "");

STATIC int
sysctl_loadavg
(__unused struct sysctl_oid *oidp, __unused void *arg1, __unused int arg2, struct sysctl_req *req)
{
		if (proc_is64bit(req->p)) {
			struct user64_loadavg loadinfo64;
			fill_loadavg64(&averunnable, &loadinfo64);
			return sysctl_io_opaque(req, &loadinfo64, sizeof(loadinfo64), NULL);
		} else {
			struct user32_loadavg loadinfo32;
			fill_loadavg32(&averunnable, &loadinfo32);
			return sysctl_io_opaque(req, &loadinfo32, sizeof(loadinfo32), NULL);
		}
}

SYSCTL_PROC(_vm, VM_LOADAVG, loadavg,
		CTLTYPE_STRUCT | CTLFLAG_RD | CTLFLAG_LOCKED,
		0, 0, sysctl_loadavg, "S,loadavg", "");

/*
 * Note:	Thread safe; vm_map_lock protects in  vm_toggle_entry_reuse()
 */
STATIC int
sysctl_vm_toggle_address_reuse(__unused struct sysctl_oid *oidp, __unused void *arg1,
	      __unused int arg2, struct sysctl_req *req)
{
	int old_value=0, new_value=0, error=0;
	
	if(vm_toggle_entry_reuse( VM_TOGGLE_GETVALUE, &old_value ))
		return(error);
	error = sysctl_io_number(req, old_value, sizeof(int), &new_value, NULL);
	if (!error) {
		return (vm_toggle_entry_reuse(new_value, NULL));
	}
	return(error);
}

SYSCTL_PROC(_debug, OID_AUTO, toggle_address_reuse, CTLFLAG_ANYBODY | CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED, 0, 0, sysctl_vm_toggle_address_reuse,"I","");

STATIC int
sysctl_swapusage
(__unused struct sysctl_oid *oidp, __unused void *arg1, __unused int arg2, struct sysctl_req *req)
{
		int			error;
		uint64_t		swap_total;
		uint64_t		swap_avail;
		vm_size_t		swap_pagesize;
		boolean_t		swap_encrypted;
		struct xsw_usage	xsu;

		error = macx_swapinfo(&swap_total,
				      &swap_avail,
				      &swap_pagesize,
				      &swap_encrypted);
		if (error)
			return error;

		xsu.xsu_total = swap_total;
		xsu.xsu_avail = swap_avail;
		xsu.xsu_used = swap_total - swap_avail;
		xsu.xsu_pagesize = swap_pagesize;
		xsu.xsu_encrypted = swap_encrypted;
		return sysctl_io_opaque(req, &xsu, sizeof(xsu), NULL);
}



SYSCTL_PROC(_vm, VM_SWAPUSAGE, swapusage,
		CTLTYPE_STRUCT | CTLFLAG_RD | CTLFLAG_LOCKED,
		0, 0, sysctl_swapusage, "S,xsw_usage", "");

#if CONFIG_FREEZE
extern void vm_page_reactivate_all_throttled(void);

static int
sysctl_freeze_enabled SYSCTL_HANDLER_ARGS
{
#pragma unused(arg1, arg2)
	int error, val = memorystatus_freeze_enabled ? 1 : 0;
	boolean_t disabled;

	error = sysctl_handle_int(oidp, &val, 0, req);
	if (error || !req->newptr)
 		return (error);
	
	/* 
	 * If freeze is being disabled, we need to move dirty pages out from the throttle to the active queue. 
	 */
	disabled = (!val && memorystatus_freeze_enabled);
	
	memorystatus_freeze_enabled = val ? TRUE : FALSE;
	
	if (disabled) {
		vm_page_reactivate_all_throttled();
	}
	
	return (0);
}

SYSCTL_PROC(_vm, OID_AUTO, freeze_enabled, CTLTYPE_INT|CTLFLAG_RW, &memorystatus_freeze_enabled, 0, sysctl_freeze_enabled, "I", "");
#endif /* CONFIG_FREEZE */

/* this kernel does NOT implement shared_region_make_private_np() */
SYSCTL_INT(_kern, KERN_SHREG_PRIVATIZABLE, shreg_private, 
		CTLFLAG_RD | CTLFLAG_LOCKED, 
		(int *)NULL, 0, "");

#if defined(__i386__) || defined(__x86_64__)
STATIC int
sysctl_sysctl_exec_affinity(__unused struct sysctl_oid *oidp,
			   __unused void *arg1, __unused int arg2,
			   struct sysctl_req *req)
{
	proc_t cur_proc = req->p;
	int error;
	
	if (req->oldptr != USER_ADDR_NULL) {
		cpu_type_t oldcputype = (cur_proc->p_flag & P_AFFINITY) ? CPU_TYPE_POWERPC : CPU_TYPE_I386;
		if ((error = SYSCTL_OUT(req, &oldcputype, sizeof(oldcputype))))
			return error;
	}

	if (req->newptr != USER_ADDR_NULL) {
		cpu_type_t newcputype;
		if ((error = SYSCTL_IN(req, &newcputype, sizeof(newcputype))))
			return error;
		if (newcputype == CPU_TYPE_I386)
			OSBitAndAtomic(~((uint32_t)P_AFFINITY), &cur_proc->p_flag);
		else if (newcputype == CPU_TYPE_POWERPC)
			OSBitOrAtomic(P_AFFINITY, &cur_proc->p_flag);
		else
			return (EINVAL);
	}
	
	return 0;
}
SYSCTL_PROC(_sysctl, OID_AUTO, proc_exec_affinity, CTLTYPE_INT|CTLFLAG_RW|CTLFLAG_ANYBODY | CTLFLAG_LOCKED, 0, 0, sysctl_sysctl_exec_affinity ,"I","proc_exec_affinity");
#endif

STATIC int
fetch_process_cputype(
	proc_t cur_proc,
	int *name,
	u_int namelen,
	cpu_type_t *cputype)
{
	proc_t p = PROC_NULL;
	int refheld = 0;
	cpu_type_t ret = 0;
	int error = 0;
	
	if (namelen == 0)
		p = cur_proc;
	else if (namelen == 1) {
		p = proc_find(name[0]);
		if (p == NULL)
			return (EINVAL);
		refheld = 1;
	} else {
		error = EINVAL;
		goto out;
	}

#if defined(__i386__) || defined(__x86_64__)
	if (p->p_flag & P_TRANSLATED) {
		ret = CPU_TYPE_POWERPC;
	}
	else
#endif
	{
		ret = cpu_type();
		if (IS_64BIT_PROCESS(p))
			ret |= CPU_ARCH_ABI64;
	}
	*cputype = ret;
	
	if (refheld != 0)
		proc_rele(p);
out:
	return (error);
}

STATIC int
sysctl_sysctl_native(__unused struct sysctl_oid *oidp, void *arg1, int arg2,
		    struct sysctl_req *req)
{
	int error;
	cpu_type_t proc_cputype = 0;
	if ((error = fetch_process_cputype(req->p, (int *)arg1, arg2, &proc_cputype)) != 0)
		return error;
	int res = 1;
	if ((proc_cputype & ~CPU_ARCH_MASK) != (cpu_type() & ~CPU_ARCH_MASK))
		res = 0;
	return SYSCTL_OUT(req, &res, sizeof(res));
}	
SYSCTL_PROC(_sysctl, OID_AUTO, proc_native, CTLTYPE_NODE|CTLFLAG_RD | CTLFLAG_LOCKED, 0, 0, sysctl_sysctl_native ,"I","proc_native");

STATIC int
sysctl_sysctl_cputype(__unused struct sysctl_oid *oidp, void *arg1, int arg2,
		     struct sysctl_req *req)
{
	int error;
	cpu_type_t proc_cputype = 0;
	if ((error = fetch_process_cputype(req->p, (int *)arg1, arg2, &proc_cputype)) != 0)
		return error;
	return SYSCTL_OUT(req, &proc_cputype, sizeof(proc_cputype));
}
SYSCTL_PROC(_sysctl, OID_AUTO, proc_cputype, CTLTYPE_NODE|CTLFLAG_RD | CTLFLAG_LOCKED, 0, 0, sysctl_sysctl_cputype ,"I","proc_cputype");

STATIC int
sysctl_safeboot
(__unused struct sysctl_oid *oidp, __unused void *arg1, __unused int arg2, struct sysctl_req *req)
{
	return sysctl_io_number(req, boothowto & RB_SAFEBOOT ? 1 : 0, sizeof(int), NULL, NULL);
}

SYSCTL_PROC(_kern, KERN_SAFEBOOT, safeboot,
		CTLTYPE_INT | CTLFLAG_RD | CTLFLAG_LOCKED,
		0, 0, sysctl_safeboot, "I", "");

STATIC int
sysctl_singleuser
(__unused struct sysctl_oid *oidp, __unused void *arg1, __unused int arg2, struct sysctl_req *req)
{
	return sysctl_io_number(req, boothowto & RB_SINGLE ? 1 : 0, sizeof(int), NULL, NULL);
}

SYSCTL_PROC(_kern, OID_AUTO, singleuser,
		CTLTYPE_INT | CTLFLAG_RD | CTLFLAG_LOCKED,
		0, 0, sysctl_singleuser, "I", "");

/*
 * Controls for debugging affinity sets - see osfmk/kern/affinity.c
 */
extern boolean_t	affinity_sets_enabled;
extern int		affinity_sets_mapping;

SYSCTL_INT (_kern, OID_AUTO, affinity_sets_enabled,
	    CTLFLAG_RW | CTLFLAG_LOCKED, (int *) &affinity_sets_enabled, 0, "hinting enabled");
SYSCTL_INT (_kern, OID_AUTO, affinity_sets_mapping,
	    CTLFLAG_RW | CTLFLAG_LOCKED, &affinity_sets_mapping, 0, "mapping policy");

/*
 * Boolean indicating if KASLR is active.
 */
STATIC int
sysctl_slide
(__unused struct sysctl_oid *oidp, __unused void *arg1, __unused int arg2, struct sysctl_req *req)
{
	uint32_t	slide;

	slide = vm_kernel_slide ? 1 : 0;

	return sysctl_io_number( req, slide, sizeof(int), NULL, NULL);
}

SYSCTL_PROC(_kern, OID_AUTO, slide,
		CTLTYPE_INT | CTLFLAG_RD | CTLFLAG_LOCKED,
		0, 0, sysctl_slide, "I", "");

/*
 * Limit on total memory users can wire.
 *
 * vm_global_user_wire_limit - system wide limit on wired memory from all processes combined. 
 *
 * vm_user_wire_limit - per address space limit on wired memory.  This puts a cap on the process's rlimit value.
 *
 * These values are initialized to reasonable defaults at boot time based on the available physical memory in
 * kmem_init().
 *
 * All values are in bytes.
 */

vm_map_size_t	vm_global_no_user_wire_amount;
vm_map_size_t	vm_global_user_wire_limit;
vm_map_size_t	vm_user_wire_limit;

/*
 * There needs to be a more automatic/elegant way to do this
 */
SYSCTL_QUAD(_vm, OID_AUTO, global_no_user_wire_amount, CTLFLAG_RW | CTLFLAG_LOCKED, &vm_global_no_user_wire_amount, "");
SYSCTL_QUAD(_vm, OID_AUTO, global_user_wire_limit, CTLFLAG_RW | CTLFLAG_LOCKED, &vm_global_user_wire_limit, "");
SYSCTL_QUAD(_vm, OID_AUTO, user_wire_limit, CTLFLAG_RW | CTLFLAG_LOCKED, &vm_user_wire_limit, "");

extern int vm_map_copy_overwrite_aligned_src_not_internal;
extern int vm_map_copy_overwrite_aligned_src_not_symmetric;
extern int vm_map_copy_overwrite_aligned_src_large;
SYSCTL_INT(_vm, OID_AUTO, vm_copy_src_not_internal, CTLFLAG_RD | CTLFLAG_LOCKED, &vm_map_copy_overwrite_aligned_src_not_internal, 0, "");
SYSCTL_INT(_vm, OID_AUTO, vm_copy_src_not_symmetric, CTLFLAG_RD | CTLFLAG_LOCKED, &vm_map_copy_overwrite_aligned_src_not_symmetric, 0, "");
SYSCTL_INT(_vm, OID_AUTO, vm_copy_src_large, CTLFLAG_RD | CTLFLAG_LOCKED, &vm_map_copy_overwrite_aligned_src_large, 0, "");


/*
 * enable back trace events for thread blocks
 */

extern	uint32_t        kdebug_thread_block;

SYSCTL_INT (_kern, OID_AUTO, kdebug_thread_block,
	    CTLFLAG_RW | CTLFLAG_LOCKED, &kdebug_thread_block, 0, "kdebug thread_block");

/*
 * Kernel stack size and depth
 */
SYSCTL_INT (_kern, OID_AUTO, stack_size,
	    CTLFLAG_RD | CTLFLAG_LOCKED, (int *) &kernel_stack_size, 0, "Kernel stack size");
SYSCTL_INT (_kern, OID_AUTO, stack_depth_max,
	    CTLFLAG_RD | CTLFLAG_LOCKED, (int *) &kernel_stack_depth_max, 0, "Max kernel stack depth at interrupt or context switch");

/*
 * enable back trace for port allocations
 */
extern int ipc_portbt;

SYSCTL_INT(_kern, OID_AUTO, ipc_portbt, 
		CTLFLAG_RW | CTLFLAG_KERN | CTLFLAG_LOCKED,
		&ipc_portbt, 0, "");

/*
 * Scheduler sysctls
 */

/*
 * See osfmk/kern/sched_prim.c for the corresponding definition
 * in osfmk/. If either version changes, update the other.
 */
#define SCHED_STRING_MAX_LENGTH (48)

extern char sched_string[SCHED_STRING_MAX_LENGTH];
SYSCTL_STRING(_kern, OID_AUTO, sched,
			  CTLFLAG_RD | CTLFLAG_KERN | CTLFLAG_LOCKED,
			  sched_string, sizeof(sched_string),
			  "Timeshare scheduler implementation");

/*
 * Only support runtime modification on embedded platforms
 * with development config enabled
 */
#if CONFIG_EMBEDDED
#if !SECURE_KERNEL
extern int precise_user_kernel_time;
SYSCTL_INT(_kern, OID_AUTO, precise_user_kernel_time, 
		CTLFLAG_RW | CTLFLAG_LOCKED,
		&precise_user_kernel_time, 0, "Precise accounting of kernel vs. user time");
#endif
#endif

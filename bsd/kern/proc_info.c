/*
 * Copyright (c) 2005-2013 Apple Inc. All rights reserved.
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
 * sysctl system call.
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
#include <sys/kern_memorystatus.h>

#include <security/audit/audit.h>

#include <mach/machine.h>
#include <mach/mach_types.h>
#include <mach/vm_param.h>
#include <kern/task.h>
#include <kern/lock.h>
#include <kern/kalloc.h>
#include <kern/assert.h>
#include <vm/vm_kern.h>
#include <vm/vm_map.h>
#include <mach/host_info.h>
#include <mach/task_info.h>
#include <mach/thread_info.h>
#include <mach/vm_region.h>

#include <sys/mount_internal.h>
#include <sys/proc_info.h>
#include <sys/bsdtask_info.h>
#include <sys/kdebug.h>
#include <sys/sysproto.h>
#include <sys/msgbuf.h>
#include <sys/priv.h>

#include <sys/guarded.h>

#include <machine/machine_routines.h>

#include <kern/ipc_misc.h>

#include <vm/vm_protos.h>

struct pshmnode;
struct psemnode;
struct pipe;
struct kqueue;
struct atalk;

uint64_t get_dispatchqueue_offset_from_proc(void *);
uint64_t get_dispatchqueue_serialno_offset_from_proc(void *);
int proc_info_internal(int callnum, int pid, int flavor, uint64_t arg, user_addr_t buffer, uint32_t buffersize, int32_t * retval);

/* protos for proc_info calls */
int proc_listpids(uint32_t type, uint32_t tyoneinfo, user_addr_t buffer, uint32_t buffersize, int32_t * retval);
int proc_pidinfo(int pid, int flavor, uint64_t arg, user_addr_t buffer, uint32_t buffersize, int32_t * retval);
int proc_pidfdinfo(int pid, int flavor,int fd, user_addr_t buffer, uint32_t buffersize, int32_t * retval);
int proc_kernmsgbuf(user_addr_t buffer, uint32_t buffersize, int32_t * retval);
int proc_setcontrol(int pid, int flavor, uint64_t arg, user_addr_t buffer, uint32_t buffersize, int32_t * retval);
int proc_pidfileportinfo(int pid, int flavor, mach_port_name_t name, user_addr_t buffer, uint32_t buffersize, int32_t *retval);
int proc_dirtycontrol(int pid, int flavor, uint64_t arg, int32_t * retval);
int proc_terminate(int pid, int32_t * retval);
int proc_pid_rusage(int pid, int flavor, user_addr_t buffer, int32_t * retval);

/* protos for procpidinfo calls */
int proc_pidfdlist(proc_t p, user_addr_t buffer, uint32_t buffersize, int32_t *retval);
int proc_pidbsdinfo(proc_t p, struct proc_bsdinfo *pbsd, int zombie);
int proc_pidshortbsdinfo(proc_t p, struct proc_bsdshortinfo *pbsd_shortp, int zombie);
int proc_pidtaskinfo(proc_t p, struct proc_taskinfo *ptinfo);
int proc_pidallinfo(proc_t p, int flavor, uint64_t arg, user_addr_t buffer, uint32_t buffersize, int32_t *retval);
int proc_pidthreadinfo(proc_t p, uint64_t arg,  int thuniqueid, struct proc_threadinfo *pthinfo);
int proc_pidthreadpathinfo(proc_t p, uint64_t arg,  struct proc_threadwithpathinfo *pinfo);
int proc_pidlistthreads(proc_t p,  user_addr_t buffer, uint32_t buffersize, int32_t *retval);
int proc_pidregioninfo(proc_t p, uint64_t arg, user_addr_t buffer, uint32_t buffersize, int32_t *retval);
int proc_pidregionpathinfo(proc_t p,  uint64_t arg, user_addr_t buffer, uint32_t buffersize, int32_t *retval);
int proc_pidvnodepathinfo(proc_t p,  uint64_t arg, user_addr_t buffer, uint32_t buffersize, int32_t *retval);
int proc_pidpathinfo(proc_t p, uint64_t arg, user_addr_t buffer, uint32_t buffersize, int32_t *retval);
int proc_pidworkqueueinfo(proc_t p, struct proc_workqueueinfo *pwqinfo);
int proc_pidfileportlist(proc_t p, user_addr_t buffer, uint32_t buffersize, int32_t *retval);
void proc_piduniqidentifierinfo(proc_t p, struct proc_uniqidentifierinfo *p_uniqidinfo);


/* protos for proc_pidfdinfo calls */
int pid_vnodeinfo(vnode_t vp, uint32_t vid, struct fileproc * fp, int closeonexec, user_addr_t  buffer, uint32_t buffersize, int32_t * retval);
int pid_vnodeinfopath(vnode_t vp, uint32_t vid, struct fileproc * fp, int closeonexec, user_addr_t  buffer, uint32_t buffersize, int32_t * retval);
int pid_socketinfo(socket_t  so, struct fileproc *fp, int closeonexec, user_addr_t  buffer, uint32_t buffersize, int32_t * retval);
int pid_pseminfo(struct psemnode * psem, struct fileproc * fp,  int closeonexec, user_addr_t  buffer, uint32_t buffersize, int32_t * retval);
int pid_pshminfo(struct pshmnode * pshm, struct fileproc * fp,  int closeonexec, user_addr_t  buffer, uint32_t buffersize, int32_t * retval);
int pid_pipeinfo(struct pipe * p, struct fileproc * fp,  int closeonexec, user_addr_t  buffer, uint32_t buffersize, int32_t * retval);
int pid_kqueueinfo(struct kqueue * kq, struct fileproc * fp,  int closeonexec, user_addr_t  buffer, uint32_t buffersize, int32_t * retval);
int pid_atalkinfo(struct atalk  * at, struct fileproc * fp,  int closeonexec, user_addr_t  buffer, uint32_t buffersize, int32_t * retval);


/* protos for misc */

int fill_vnodeinfo(vnode_t vp, struct vnode_info *vinfo);
void  fill_fileinfo(struct fileproc * fp, int closeonexec, struct proc_fileinfo * finfo);
int proc_security_policy(proc_t targetp, int callnum, int flavor, boolean_t check_same_user);
static void munge_vinfo_stat(struct stat64 *sbp, struct vinfo_stat *vsbp);

extern int cansignal(struct proc *, kauth_cred_t, struct proc *, int, int);
extern int proc_get_rusage(proc_t proc, int flavor, user_addr_t buffer, int is_zombie);

#define CHECK_SAME_USER         TRUE
#define NO_CHECK_SAME_USER      FALSE

uint64_t get_dispatchqueue_offset_from_proc(void *p)
{
	if(p != NULL) {
		proc_t pself = (proc_t)p;
		return (pself->p_dispatchqueue_offset);
	} else {
		return (uint64_t)0;
	}
}

uint64_t get_dispatchqueue_serialno_offset_from_proc(void *p)
{
	if(p != NULL) {
		proc_t pself = (proc_t)p;
		return (pself->p_dispatchqueue_serialno_offset);
	} else {
		return (uint64_t)0;
	}
}

/***************************** proc_info ********************/

int
proc_info(__unused struct proc *p, struct proc_info_args * uap, int32_t *retval)
{
	return(proc_info_internal(uap->callnum, uap->pid, uap->flavor, uap->arg, uap->buffer, uap->buffersize, retval));
}


int 
proc_info_internal(int callnum, int pid, int flavor, uint64_t arg, user_addr_t buffer, uint32_t  buffersize, int32_t * retval)
{

	switch(callnum) {
		case PROC_INFO_CALL_LISTPIDS:
			/* pid contains type and flavor contains typeinfo */
			return(proc_listpids(pid, flavor, buffer, buffersize, retval));
		case PROC_INFO_CALL_PIDINFO:
			return(proc_pidinfo(pid, flavor, arg, buffer, buffersize, retval));
		case PROC_INFO_CALL_PIDFDINFO:
			return(proc_pidfdinfo(pid, flavor, (int)arg, buffer, buffersize, retval));
		case PROC_INFO_CALL_KERNMSGBUF:
			return(proc_kernmsgbuf(buffer, buffersize, retval));
		case PROC_INFO_CALL_SETCONTROL:
			return(proc_setcontrol(pid, flavor, arg, buffer, buffersize, retval));
		case PROC_INFO_CALL_PIDFILEPORTINFO:
			return(proc_pidfileportinfo(pid, flavor, (mach_port_name_t)arg, buffer, buffersize, retval));
		case PROC_INFO_CALL_TERMINATE:
			return(proc_terminate(pid, retval));
		case PROC_INFO_CALL_DIRTYCONTROL:
			return(proc_dirtycontrol(pid, flavor, arg, retval));
		case PROC_INFO_CALL_PIDRUSAGE:
			return (proc_pid_rusage(pid, flavor, buffer, retval));
		default:
				return(EINVAL);
	}

	return(EINVAL);
}

/******************* proc_listpids routine ****************/
int
proc_listpids(uint32_t type, uint32_t typeinfo, user_addr_t buffer, uint32_t  buffersize, int32_t * retval)
{
	int numprocs, wantpids;
	char * kbuf;
	int * ptr;
	int n, skip;
	struct proc * p;
	struct tty * tp;
	int error = 0;
	struct proclist *current_list;

	/* Do we have permission to look into this? */
	if ((error = proc_security_policy(PROC_NULL, PROC_INFO_CALL_LISTPIDS, type, NO_CHECK_SAME_USER)))
		return (error);

	/* if the buffer is null, return num of procs */
	if (buffer == (user_addr_t)0) {
		*retval = ((nprocs+20) * sizeof(int));
		return(0);
	}

	if (buffersize < sizeof(int)) {
		return(ENOMEM);
	}
	wantpids = buffersize/sizeof(int);
	numprocs = nprocs+20;
	if (numprocs > wantpids)
		numprocs = wantpids;

	kbuf = (char *)kalloc((vm_size_t)(numprocs * sizeof(int)));
	if (kbuf == NULL)
		return(ENOMEM);
	bzero(kbuf, sizeof(int));

	proc_list_lock();

	
	n = 0;
	ptr = (int *)kbuf;
	current_list = &allproc;
proc_loop:
	LIST_FOREACH(p, current_list, p_list) {
		skip = 0;
		switch (type) {
			case PROC_PGRP_ONLY:
				if (p->p_pgrpid != (pid_t)typeinfo)
					skip = 1;
			  	break;
			case PROC_PPID_ONLY:
				if ((p->p_ppid != (pid_t)typeinfo) && (((p->p_lflag & P_LTRACED) == 0) || (p->p_oppid != (pid_t)typeinfo)))
					skip = 1;
			  	break;

			case PROC_ALL_PIDS:
				skip = 0;
			  	break;
			case PROC_TTY_ONLY:
				/* racy but list lock is held */
				if ((p->p_flag & P_CONTROLT) == 0 ||
					(p->p_pgrp == NULL) || (p->p_pgrp->pg_session == NULL) ||
			    	(tp = SESSION_TP(p->p_pgrp->pg_session)) == TTY_NULL ||
			    	tp->t_dev != (dev_t)typeinfo)
					skip = 1;
			  	break;
			case PROC_UID_ONLY:
				if (p->p_ucred == NULL)
					skip = 1;
				else {
					kauth_cred_t my_cred;
					uid_t uid;
			
					my_cred = kauth_cred_proc_ref(p);
					uid = kauth_cred_getuid(my_cred);
					kauth_cred_unref(&my_cred);
					if (uid != (uid_t)typeinfo)
						skip = 1;
				}
			  	break;
			case PROC_RUID_ONLY:
				if (p->p_ucred == NULL)
					skip = 1;
				else {
					kauth_cred_t my_cred;
					uid_t uid;
			
					my_cred = kauth_cred_proc_ref(p);
					uid = kauth_cred_getruid(my_cred);
					kauth_cred_unref(&my_cred);
					if (uid != (uid_t)typeinfo)
						skip = 1;
				}
			  	break;
			default:
			  skip = 1;
			  break;
		};

		if(skip == 0) {
			*ptr++ = p->p_pid;
			n++;
		}
		if (n >= numprocs)
			break;
	}
	
	if ((n < numprocs) && (current_list == &allproc)) {
		current_list = &zombproc;
		goto proc_loop;
	}

	proc_list_unlock();

	ptr = (int *)kbuf;
	error = copyout((caddr_t)ptr, buffer, n * sizeof(int));
	if (error == 0)
		*retval = (n * sizeof(int));
	kfree((void *)kbuf, (vm_size_t)(numprocs * sizeof(int)));

	return(error);
}


/********************************** proc_pidinfo routines ********************************/

int 
proc_pidfdlist(proc_t p, user_addr_t buffer, uint32_t  buffersize, int32_t *retval)
{
		int numfds, needfds;
		char * kbuf;
		struct proc_fdinfo * pfd;
		struct fileproc * fp;
		int n;
		int count = 0;
		int error = 0;
		
	 	numfds = p->p_fd->fd_nfiles;	

		if (buffer == (user_addr_t) 0) {
			numfds += 20;
			*retval = (numfds * sizeof(struct proc_fdinfo));
			return(0);
		}

		/* buffersize is big enough atleast for one struct */
		needfds = buffersize/sizeof(struct proc_fdinfo);

		if (numfds > needfds)
			numfds = needfds;

		kbuf = (char *)kalloc((vm_size_t)(numfds * sizeof(struct proc_fdinfo)));
		if (kbuf == NULL)
			return(ENOMEM);
		bzero(kbuf, numfds * sizeof(struct proc_fdinfo));

		proc_fdlock(p);

		pfd = (struct proc_fdinfo *)kbuf;

		for (n = 0; ((n < numfds) && (n < p->p_fd->fd_nfiles)); n++) {
			if (((fp = p->p_fd->fd_ofiles[n]) != 0) 
			     && ((p->p_fd->fd_ofileflags[n] & UF_RESERVED) == 0)) {
				file_type_t fdtype = FILEGLOB_DTYPE(fp->f_fglob);
				pfd->proc_fd = n;
				pfd->proc_fdtype = (fdtype != DTYPE_ATALK) ?
					fdtype : PROX_FDTYPE_ATALK;
				count++;
				pfd++;
			}
		}
		proc_fdunlock(p);

		error = copyout(kbuf, buffer, count * sizeof(struct proc_fdinfo));
		kfree((void *)kbuf, (vm_size_t)(numfds * sizeof(struct proc_fdinfo)));
		if (error == 0)
			*retval = (count * sizeof(struct proc_fdinfo));
		return(error);		
}

/*
 * Helper functions for proc_pidfileportlist.
 */
static int
proc_fileport_count(__unused mach_port_name_t name,
    __unused struct fileglob *fg, void *arg)
{
	uint32_t *counter = arg;

	*counter += 1;
	return (0);
}

struct fileport_fdtype_args {
	struct proc_fileportinfo *ffa_pfi;
	struct proc_fileportinfo *ffa_pfi_end;
};

static int
proc_fileport_fdtype(mach_port_name_t name, struct fileglob *fg, void *arg)
{
	struct fileport_fdtype_args *ffa = arg;

	if (ffa->ffa_pfi != ffa->ffa_pfi_end) {
		file_type_t fdtype = FILEGLOB_DTYPE(fg);

		ffa->ffa_pfi->proc_fdtype = (fdtype != DTYPE_ATALK) ?
			fdtype : PROX_FDTYPE_ATALK;
		ffa->ffa_pfi->proc_fileport = name;
		ffa->ffa_pfi++;
		return (0);		/* keep walking */
	} else
		return (-1);		/* stop the walk! */
}

int
proc_pidfileportlist(proc_t p,
	user_addr_t buffer, uint32_t buffersize, int32_t *retval)
{
	void *kbuf;
	vm_size_t kbufsize;
	struct proc_fileportinfo *pfi;
	uint32_t needfileports, numfileports;
	struct fileport_fdtype_args ffa;
	int error;

	needfileports = buffersize / sizeof (*pfi);
	if ((user_addr_t)0 == buffer || needfileports > (uint32_t)maxfiles) {
		/*
		 * Either (i) the user is asking for a fileport count,
		 * or (ii) the number of fileports they're asking for is
		 * larger than the maximum number of open files (!); count
		 * them to bound subsequent heap allocations.
		 */
		numfileports = 0;
		switch (fileport_walk(p->task,
		    proc_fileport_count, &numfileports)) {
		case KERN_SUCCESS:
			break;
		case KERN_RESOURCE_SHORTAGE:
			return (ENOMEM);
		case KERN_INVALID_TASK:
			return (ESRCH);
		default:
			return (EINVAL);
		}

		if (numfileports == 0) {
			*retval = 0;		/* none at all, bail */
			return (0);
		}
		if ((user_addr_t)0 == buffer) {
			numfileports += 20;	/* accelerate convergence */
			*retval = numfileports * sizeof (*pfi);
			return (0);
		}
		if (needfileports > numfileports)
			needfileports = numfileports;
	}

	assert(buffersize >= PROC_PIDLISTFILEPORTS_SIZE);

	kbufsize = (vm_size_t)needfileports * sizeof (*pfi);
	pfi = kbuf = kalloc(kbufsize);
	if (kbuf == NULL)
	   	return (ENOMEM);
	bzero(kbuf, kbufsize);

	ffa.ffa_pfi = pfi;
	ffa.ffa_pfi_end = pfi + needfileports;

	switch (fileport_walk(p->task, proc_fileport_fdtype, &ffa)) {
	case KERN_SUCCESS:
		error = 0;
		pfi = ffa.ffa_pfi;
		if ((numfileports = pfi - (typeof(pfi))kbuf) == 0)
			break;
		if (numfileports > needfileports)
			panic("more fileports returned than requested");
		error = copyout(kbuf, buffer, numfileports * sizeof (*pfi));
		break;
	case KERN_RESOURCE_SHORTAGE:
		error = ENOMEM;
		break;
	case KERN_INVALID_TASK:
		error = ESRCH;
		break;
	default:
		error = EINVAL;
		break;
	}
	kfree(kbuf, kbufsize);
	if (error == 0)
		*retval = numfileports * sizeof (*pfi);
	return (error);
}

int 
proc_pidbsdinfo(proc_t p, struct proc_bsdinfo * pbsd, int zombie)
{
	register struct tty *tp;
	struct  session *sessionp = NULL;
	struct pgrp * pg;
	kauth_cred_t my_cred;

	pg = proc_pgrp(p);
	sessionp = proc_session(p);

	my_cred = kauth_cred_proc_ref(p);
	bzero(pbsd, sizeof(struct proc_bsdinfo));
	pbsd->pbi_status = p->p_stat;
	pbsd->pbi_xstatus = p->p_xstat;
	pbsd->pbi_pid = p->p_pid;
	pbsd->pbi_ppid = p->p_ppid;
	pbsd->pbi_uid = kauth_cred_getuid(my_cred);
	pbsd->pbi_gid = kauth_cred_getgid(my_cred); 
	pbsd->pbi_ruid =  kauth_cred_getruid(my_cred);
	pbsd->pbi_rgid = kauth_cred_getrgid(my_cred);
	pbsd->pbi_svuid =  kauth_cred_getsvuid(my_cred);
	pbsd->pbi_svgid = kauth_cred_getsvgid(my_cred);
	kauth_cred_unref(&my_cred);
	
	pbsd->pbi_nice = p->p_nice;
	pbsd->pbi_start_tvsec = p->p_start.tv_sec;
	pbsd->pbi_start_tvusec = p->p_start.tv_usec;
	bcopy(&p->p_comm, &pbsd->pbi_comm[0], MAXCOMLEN);
	pbsd->pbi_comm[MAXCOMLEN - 1] = '\0';
	bcopy(&p->p_name, &pbsd->pbi_name[0], 2*MAXCOMLEN);
	pbsd->pbi_name[(2*MAXCOMLEN) - 1] = '\0';

	pbsd->pbi_flags = 0;	
	if ((p->p_flag & P_SYSTEM) == P_SYSTEM) 
		pbsd->pbi_flags |= PROC_FLAG_SYSTEM;
	if ((p->p_lflag & P_LTRACED) == P_LTRACED) 
		pbsd->pbi_flags |= PROC_FLAG_TRACED;
	if ((p->p_lflag & P_LEXIT) == P_LEXIT) 
		pbsd->pbi_flags |= PROC_FLAG_INEXIT;
	if ((p->p_lflag & P_LPPWAIT) == P_LPPWAIT) 
		pbsd->pbi_flags |= PROC_FLAG_PPWAIT;
	if ((p->p_flag & P_LP64) == P_LP64) 
		pbsd->pbi_flags |= PROC_FLAG_LP64;
	if ((p->p_flag & P_CONTROLT) == P_CONTROLT) 
		pbsd->pbi_flags |= PROC_FLAG_CONTROLT;
	if ((p->p_flag & P_THCWD) == P_THCWD) 
		pbsd->pbi_flags |= PROC_FLAG_THCWD;
	if ((p->p_flag & P_SUGID) == P_SUGID) 
		pbsd->pbi_flags |= PROC_FLAG_PSUGID;
	if ((p->p_flag & P_EXEC) == P_EXEC) 
		pbsd->pbi_flags |= PROC_FLAG_EXEC;

	if (sessionp != SESSION_NULL) {
		if (SESS_LEADER(p, sessionp))
			pbsd->pbi_flags |= PROC_FLAG_SLEADER;
		if (sessionp->s_ttyvp)
			pbsd->pbi_flags |= PROC_FLAG_CTTY;
	}

	if ((p->p_flag & P_DELAYIDLESLEEP) == P_DELAYIDLESLEEP) 
		pbsd->pbi_flags |= PROC_FLAG_DELAYIDLESLEEP;

	switch(PROC_CONTROL_STATE(p)) {
		case P_PCTHROTTLE:
			pbsd->pbi_flags |= PROC_FLAG_PC_THROTTLE;
			break;
		case P_PCSUSP:
			pbsd->pbi_flags |= PROC_FLAG_PC_SUSP;
			break;
		case P_PCKILL:
			pbsd->pbi_flags |= PROC_FLAG_PC_KILL;
			break;
	};

	switch(PROC_ACTION_STATE(p)) {
		case P_PCTHROTTLE:
			pbsd->pbi_flags |= PROC_FLAG_PA_THROTTLE;
			break;
		case P_PCSUSP:
			pbsd->pbi_flags |= PROC_FLAG_PA_SUSP;
			break;
	};
		
	/* if process is a zombie skip bg state */
	if ((zombie == 0) && (p->p_stat != SZOMB) && (p->task != TASK_NULL))
		proc_get_darwinbgstate(p->task, &pbsd->pbi_flags);

	if (zombie == 0)
		pbsd->pbi_nfiles = p->p_fd->fd_nfiles;
	
	pbsd->e_tdev = NODEV;
	if (pg != PGRP_NULL) {
		pbsd->pbi_pgid = p->p_pgrpid;
		pbsd->pbi_pjobc = pg->pg_jobc;
		if ((p->p_flag & P_CONTROLT) && (sessionp != SESSION_NULL) && (tp = SESSION_TP(sessionp))) {
			pbsd->e_tdev = tp->t_dev;
			pbsd->e_tpgid = sessionp->s_ttypgrpid;
		}
	} 
	if (sessionp != SESSION_NULL)
		session_rele(sessionp);
	if (pg != PGRP_NULL)
		pg_rele(pg);

	return(0);
}


int 
proc_pidshortbsdinfo(proc_t p, struct proc_bsdshortinfo * pbsd_shortp, int zombie)
{
	bzero(pbsd_shortp, sizeof(struct proc_bsdshortinfo));
	pbsd_shortp->pbsi_pid = p->p_pid;
	pbsd_shortp->pbsi_ppid = p->p_ppid;
	pbsd_shortp->pbsi_pgid = p->p_pgrpid;
	pbsd_shortp->pbsi_status = p->p_stat;
	bcopy(&p->p_comm, &pbsd_shortp->pbsi_comm[0], MAXCOMLEN);
	pbsd_shortp->pbsi_comm[MAXCOMLEN - 1] = '\0';

	pbsd_shortp->pbsi_flags = 0;	
	if ((p->p_flag & P_SYSTEM) == P_SYSTEM) 
		pbsd_shortp->pbsi_flags |= PROC_FLAG_SYSTEM;
	if ((p->p_lflag & P_LTRACED) == P_LTRACED) 
		pbsd_shortp->pbsi_flags |= PROC_FLAG_TRACED;
	if ((p->p_lflag & P_LEXIT) == P_LEXIT) 
		pbsd_shortp->pbsi_flags |= PROC_FLAG_INEXIT;
	if ((p->p_lflag & P_LPPWAIT) == P_LPPWAIT) 
		pbsd_shortp->pbsi_flags |= PROC_FLAG_PPWAIT;
	if ((p->p_flag & P_LP64) == P_LP64) 
		pbsd_shortp->pbsi_flags |= PROC_FLAG_LP64;
	if ((p->p_flag & P_CONTROLT) == P_CONTROLT) 
		pbsd_shortp->pbsi_flags |= PROC_FLAG_CONTROLT;
	if ((p->p_flag & P_THCWD) == P_THCWD) 
		pbsd_shortp->pbsi_flags |= PROC_FLAG_THCWD;
	if ((p->p_flag & P_SUGID) == P_SUGID) 
		pbsd_shortp->pbsi_flags |= PROC_FLAG_PSUGID;
	if ((p->p_flag & P_EXEC) == P_EXEC) 
		pbsd_shortp->pbsi_flags |= PROC_FLAG_EXEC;
	if ((p->p_flag & P_DELAYIDLESLEEP) == P_DELAYIDLESLEEP) 
		pbsd_shortp->pbsi_flags |= PROC_FLAG_DELAYIDLESLEEP;

	switch(PROC_CONTROL_STATE(p)) {
		case P_PCTHROTTLE:
			pbsd_shortp->pbsi_flags |= PROC_FLAG_PC_THROTTLE;
			break;
		case P_PCSUSP:
			pbsd_shortp->pbsi_flags |= PROC_FLAG_PC_SUSP;
			break;
		case P_PCKILL:
			pbsd_shortp->pbsi_flags |= PROC_FLAG_PC_KILL;
			break;
	};

	switch(PROC_ACTION_STATE(p)) {
		case P_PCTHROTTLE:
			pbsd_shortp->pbsi_flags |= PROC_FLAG_PA_THROTTLE;
			break;
		case P_PCSUSP:
			pbsd_shortp->pbsi_flags |= PROC_FLAG_PA_SUSP;
			break;
	};
		
	/* if process is a zombie skip bg state */
	if ((zombie == 0) && (p->p_stat != SZOMB) && (p->task != TASK_NULL))
		proc_get_darwinbgstate(p->task, &pbsd_shortp->pbsi_flags);

	pbsd_shortp->pbsi_uid = p->p_uid;
	pbsd_shortp->pbsi_gid = p->p_gid; 
	pbsd_shortp->pbsi_ruid =  p->p_ruid;
	pbsd_shortp->pbsi_rgid = p->p_rgid;
	pbsd_shortp->pbsi_svuid =  p->p_svuid;
	pbsd_shortp->pbsi_svgid = p->p_svgid;
	
	return(0);
}

int 
proc_pidtaskinfo(proc_t p, struct proc_taskinfo * ptinfo)
{
	task_t task;
	
	task = p->task;

	bzero(ptinfo, sizeof(struct proc_taskinfo));
	fill_taskprocinfo(task, (struct proc_taskinfo_internal *)ptinfo);

	return(0);
}



int 
proc_pidthreadinfo(proc_t p, uint64_t arg,  int thuniqueid, struct proc_threadinfo *pthinfo)
{
	int error = 0;
	uint64_t threadaddr = (uint64_t)arg;

	bzero(pthinfo, sizeof(struct proc_threadinfo));

	error = fill_taskthreadinfo(p->task, threadaddr, thuniqueid, (struct proc_threadinfo_internal *)pthinfo, NULL, NULL);
	if (error)
		return(ESRCH);
	else
		return(0);

}

void 
bsd_getthreadname(void *uth, char *buffer)
{
	struct uthread *ut = (struct uthread *)uth;
	if(ut->pth_name)
		bcopy(ut->pth_name,buffer,MAXTHREADNAMESIZE);
}

void
bsd_threadcdir(void * uth, void *vptr, int *vidp)
{
	struct uthread * ut = (struct uthread *)uth;
	vnode_t vp;
	vnode_t *vpp = (vnode_t *)vptr;

	vp = ut->uu_cdir;
	if (vp  != NULLVP) {
		if (vpp != NULL) {
			*vpp = vp;
			if (vidp != NULL)
				*vidp = vp->v_id;
		}
	}
}


int 
proc_pidthreadpathinfo(proc_t p, uint64_t arg,  struct proc_threadwithpathinfo *pinfo)
{
	vnode_t vp = NULLVP;
	int vid;
	int error = 0;
	uint64_t threadaddr = (uint64_t)arg;
	int count;

	bzero(pinfo, sizeof(struct proc_threadwithpathinfo));

	error = fill_taskthreadinfo(p->task, threadaddr, 0, (struct proc_threadinfo_internal *)&pinfo->pt, (void *)&vp, &vid);
	if (error)
		return(ESRCH);

	if ((vp != NULLVP) && ((vnode_getwithvid(vp, vid)) == 0)) {
		error = fill_vnodeinfo(vp, &pinfo->pvip.vip_vi) ;
		if (error == 0) {
			count = MAXPATHLEN;
			vn_getpath(vp, &pinfo->pvip.vip_path[0], &count);
			pinfo->pvip.vip_path[MAXPATHLEN-1] = 0;
		}
		vnode_put(vp);
	}	
	return(error);
}



int 
proc_pidlistthreads(proc_t p,  user_addr_t buffer, uint32_t  buffersize, int32_t *retval)
{
	int count = 0;	
	int ret = 0;
	int error = 0;
	void * kbuf;
	int numthreads;

	
	count = buffersize/(sizeof(uint64_t));
	numthreads = get_numthreads(p->task);

	numthreads += 10;

	if (numthreads > count)
		numthreads = count;

	kbuf = (void *)kalloc(numthreads * sizeof(uint64_t));
	if (kbuf == NULL)
		return(ENOMEM);
	bzero(kbuf, numthreads * sizeof(uint64_t));
	
	ret = fill_taskthreadlist(p->task, kbuf, numthreads);
	
	error = copyout(kbuf, buffer, ret);
	kfree(kbuf, numthreads * sizeof(uint64_t));
	if (error == 0)
		*retval = ret;
	return(error);
	
}


int 
proc_pidregioninfo(proc_t p, uint64_t arg, user_addr_t buffer, __unused uint32_t  buffersize, int32_t *retval)
{
	struct proc_regioninfo preginfo;
	int ret, error = 0;

	bzero(&preginfo, sizeof(struct proc_regioninfo));
	ret = fill_procregioninfo( p->task, arg, (struct proc_regioninfo_internal *)&preginfo, (uintptr_t *)0, (uint32_t *)0);
	if (ret == 0)
		return(EINVAL);
	error = copyout(&preginfo, buffer, sizeof(struct proc_regioninfo));
	if (error == 0)
		*retval = sizeof(struct proc_regioninfo);
	return(error);
}


int 
proc_pidregionpathinfo(proc_t p, uint64_t arg, user_addr_t buffer, __unused uint32_t  buffersize, int32_t *retval)
{
	struct proc_regionwithpathinfo preginfo;
	int ret, error = 0;
	uintptr_t vnodeaddr= 0;
	uint32_t vnodeid= 0;
	vnode_t vp;
	int count;

	bzero(&preginfo, sizeof(struct proc_regionwithpathinfo));

	ret = fill_procregioninfo( p->task, arg, (struct proc_regioninfo_internal *)&preginfo.prp_prinfo, (uintptr_t *)&vnodeaddr, (uint32_t *)&vnodeid);
	if (ret == 0)
		return(EINVAL);
	if (vnodeaddr) {
		vp = (vnode_t)vnodeaddr;
		if ((vnode_getwithvid(vp, vnodeid)) == 0) {
			/* FILL THE VNODEINFO */
			error = fill_vnodeinfo(vp, &preginfo.prp_vip.vip_vi);
			count = MAXPATHLEN;
			vn_getpath(vp, &preginfo.prp_vip.vip_path[0], &count);
			/* Always make sure it is null terminated */
			preginfo.prp_vip.vip_path[MAXPATHLEN-1] = 0;
			vnode_put(vp);
		}
	}
	error = copyout(&preginfo, buffer, sizeof(struct proc_regionwithpathinfo));
	if (error == 0)
		*retval = sizeof(struct proc_regionwithpathinfo);
	return(error);
}

/*
 * Path is relative to current process directory; may different from current
 * thread directory.
 */
int 
proc_pidvnodepathinfo(proc_t p, __unused uint64_t arg, user_addr_t buffer, __unused uint32_t  buffersize, int32_t *retval)
{
	struct proc_vnodepathinfo pvninfo;
	int error = 0;
	vnode_t vncdirvp = NULLVP;
	uint32_t vncdirid=0;
	vnode_t vnrdirvp = NULLVP;
	uint32_t vnrdirid=0;
	int count;

	bzero(&pvninfo, sizeof(struct proc_vnodepathinfo));

	proc_fdlock(p);
	if (p->p_fd->fd_cdir) {
		vncdirvp = p->p_fd->fd_cdir;
		vncdirid = p->p_fd->fd_cdir->v_id;
	}
	if (p->p_fd->fd_rdir) {
		vnrdirvp = p->p_fd->fd_rdir;
		vnrdirid = p->p_fd->fd_rdir->v_id;
	}
	proc_fdunlock(p);

	if (vncdirvp != NULLVP) {
		if ((error = vnode_getwithvid(vncdirvp, vncdirid)) == 0) {
			/* FILL THE VNODEINFO */
			error = fill_vnodeinfo(vncdirvp, &pvninfo.pvi_cdir.vip_vi);
			if ( error == 0) {
				count = MAXPATHLEN;
				vn_getpath(vncdirvp, &pvninfo.pvi_cdir.vip_path[0], &count);
				pvninfo.pvi_cdir.vip_path[MAXPATHLEN-1] = 0;
			}	
			vnode_put(vncdirvp);
		} else {
			goto out;
		}
	}

	if ((error == 0) && (vnrdirvp != NULLVP)) {
		if ((error = vnode_getwithvid(vnrdirvp, vnrdirid)) == 0) {
			/* FILL THE VNODEINFO */
			error = fill_vnodeinfo(vnrdirvp, &pvninfo.pvi_rdir.vip_vi);
			if ( error == 0) {
				count = MAXPATHLEN;
				vn_getpath(vnrdirvp, &pvninfo.pvi_rdir.vip_path[0], &count);
				pvninfo.pvi_rdir.vip_path[MAXPATHLEN-1] = 0;
			}	
			vnode_put(vnrdirvp);
		} else {
			goto out;
		}
	}
	if (error == 0) {
		error = copyout(&pvninfo, buffer, sizeof(struct proc_vnodepathinfo));
		if (error == 0)
			*retval = sizeof(struct proc_vnodepathinfo);
	}
out:
	return(error);
}

int 
proc_pidpathinfo(proc_t p, __unused uint64_t arg, user_addr_t buffer, uint32_t buffersize, __unused int32_t *retval)
{
	int vid, error;
	vnode_t tvp;
	vnode_t nvp = NULLVP;
	int len = buffersize; 
	char * buf;

	tvp = p->p_textvp;

	if (tvp == NULLVP)
		return(ESRCH);

	buf = (char *)kalloc(buffersize);
	if (buf == NULL) 
		return(ENOMEM);


	vid = vnode_vid(tvp);
	error = vnode_getwithvid(tvp, vid);
	if (error == 0) {
		error = vn_getpath_fsenter(tvp, buf, &len);
		vnode_put(tvp);
		if (error == 0) {
			error = vnode_lookup(buf, 0, &nvp, vfs_context_current()); 
			if ((error == 0) && ( nvp != NULLVP))
				vnode_put(nvp);
			if (error == 0) {
				error = copyout(buf, buffer, len);
			}
		}
	}
	kfree(buf, buffersize);
	return(error);
}


int 
proc_pidworkqueueinfo(proc_t p, struct proc_workqueueinfo *pwqinfo)
{
	int error = 0;

	bzero(pwqinfo, sizeof(struct proc_workqueueinfo));

	error = fill_procworkqueue(p, pwqinfo);
	if (error)
		return(ESRCH);
	else
		return(0);

}


void
proc_piduniqidentifierinfo(proc_t p, struct proc_uniqidentifierinfo *p_uniqidinfo)
{
	p_uniqidinfo->p_uniqueid = proc_uniqueid(p);
	proc_getexecutableuuid(p, (unsigned char *)&p_uniqidinfo->p_uuid, sizeof(p_uniqidinfo->p_uuid));
	p_uniqidinfo->p_puniqueid = proc_puniqueid(p);
	p_uniqidinfo->p_reserve2 = 0;
	p_uniqidinfo->p_reserve3 = 0;
	p_uniqidinfo->p_reserve4 = 0;
}

/********************************** proc_pidinfo ********************************/


int
proc_pidinfo(int pid, int flavor, uint64_t arg, user_addr_t buffer, uint32_t  buffersize, int32_t * retval)
{
	struct proc * p = PROC_NULL;
	int error = ENOTSUP;
	int gotref = 0;
	int findzomb = 0;
	int shortversion = 0;
	uint32_t size;
	int zombie = 0;
	int thuniqueid = 0;
	int uniqidversion = 0;
	boolean_t check_same_user;

	switch (flavor) {
		case PROC_PIDLISTFDS:
			size = PROC_PIDLISTFD_SIZE;
			if (buffer == (user_addr_t)0)
				size = 0;
			break;
		case PROC_PIDTBSDINFO:
			size = PROC_PIDTBSDINFO_SIZE;
			break;
		case PROC_PIDTASKINFO:
			size = PROC_PIDTASKINFO_SIZE;
			break;
		case PROC_PIDTASKALLINFO:
			size = PROC_PIDTASKALLINFO_SIZE;
			break;
		case PROC_PIDTHREADINFO:
			size = PROC_PIDTHREADINFO_SIZE;
			break;
		case PROC_PIDLISTTHREADS:
			size = PROC_PIDLISTTHREADS_SIZE;
			break;
		case PROC_PIDREGIONINFO:
			size = PROC_PIDREGIONINFO_SIZE;
			break;
		case PROC_PIDREGIONPATHINFO:
			size = PROC_PIDREGIONPATHINFO_SIZE;
			break;
		case PROC_PIDVNODEPATHINFO:
			size = PROC_PIDVNODEPATHINFO_SIZE;
			break;
		case PROC_PIDTHREADPATHINFO:
			size = PROC_PIDTHREADPATHINFO_SIZE;
			break;
		case PROC_PIDPATHINFO:
			size = MAXPATHLEN;
			break;
		case PROC_PIDWORKQUEUEINFO:
			/* kernel does not have workq info */
			if (pid == 0)
				return(EINVAL);
			else
				size = PROC_PIDWORKQUEUEINFO_SIZE;
			break;
		case PROC_PIDT_SHORTBSDINFO:
			size = PROC_PIDT_SHORTBSDINFO_SIZE;
			break;
		case PROC_PIDLISTFILEPORTS:
			size = PROC_PIDLISTFILEPORTS_SIZE;
			if (buffer == (user_addr_t)0)
				size = 0;
			break;
		case PROC_PIDTHREADID64INFO:
			size = PROC_PIDTHREADID64INFO_SIZE;
			break;
		case PROC_PIDUNIQIDENTIFIERINFO:
			size = PROC_PIDUNIQIDENTIFIERINFO_SIZE;
			break;
		case PROC_PIDT_BSDINFOWITHUNIQID:
			size = PROC_PIDT_BSDINFOWITHUNIQID_SIZE;
			break;
		default:
			return(EINVAL);
	}

	if (buffersize < size) 
		return(ENOMEM);

	if ((flavor == PROC_PIDPATHINFO) && (buffersize > PROC_PIDPATHINFO_MAXSIZE)) {
		return(EOVERFLOW);
	}

	/* Check if we need to look for zombies */
	if ((flavor == PROC_PIDTBSDINFO) || (flavor == PROC_PIDT_SHORTBSDINFO) || (flavor == PROC_PIDT_BSDINFOWITHUNIQID) 
			|| (flavor == PROC_PIDUNIQIDENTIFIERINFO)) {
		if (arg)
			findzomb = 1;
	}

	if ((p = proc_find(pid)) == PROC_NULL) {
		if (findzomb)
			p = proc_find_zombref(pid);
		if (p == PROC_NULL) {
			error = ESRCH;
			goto out;
		}
		zombie = 1;
	} else {
		gotref = 1;
	}

	/* Certain operations don't require privileges */
	switch (flavor) {
		case PROC_PIDT_SHORTBSDINFO:
		case PROC_PIDUNIQIDENTIFIERINFO:
		case PROC_PIDPATHINFO:
			check_same_user = NO_CHECK_SAME_USER;
			break;
		default:
			check_same_user = CHECK_SAME_USER;
			break;
	}

	/* Do we have permission to look into this? */
	if ((error = proc_security_policy(p, PROC_INFO_CALL_PIDINFO, flavor, check_same_user)))
		goto out;

	switch (flavor) {
		case PROC_PIDLISTFDS: {
			error = proc_pidfdlist(p, buffer, buffersize, retval);
		}
		break;

		case PROC_PIDUNIQIDENTIFIERINFO: {
			struct proc_uniqidentifierinfo p_uniqidinfo;

			proc_piduniqidentifierinfo(p, &p_uniqidinfo);
			error = copyout(&p_uniqidinfo, buffer, sizeof(struct proc_uniqidentifierinfo));
			if (error == 0)
				*retval = sizeof(struct proc_uniqidentifierinfo);
		}
		break;

		case PROC_PIDT_SHORTBSDINFO:
			shortversion = 1;
		case PROC_PIDT_BSDINFOWITHUNIQID: 
		case PROC_PIDTBSDINFO: {
			struct proc_bsdinfo pbsd;
			struct proc_bsdshortinfo pbsd_short;
			struct proc_bsdinfowithuniqid pbsd_uniqid;
		
			if (flavor == PROC_PIDT_BSDINFOWITHUNIQID)
				uniqidversion = 1;
			
			if (shortversion != 0) {
				error = proc_pidshortbsdinfo(p, &pbsd_short, zombie);
			} else {
				error = proc_pidbsdinfo(p, &pbsd, zombie);
				if (uniqidversion != 0) { 
					proc_piduniqidentifierinfo(p, &pbsd_uniqid.p_uniqidentifier);
					pbsd_uniqid.pbsd = pbsd;
				}
			}
			
			if (error == 0) {
				if (shortversion != 0) {
					error = copyout(&pbsd_short, buffer, sizeof(struct proc_bsdshortinfo));
					if (error == 0)
						*retval = sizeof(struct proc_bsdshortinfo);
				 } else if (uniqidversion != 0) {
					error = copyout(&pbsd_uniqid, buffer, sizeof(struct proc_bsdinfowithuniqid));
					if (error == 0)
						*retval = sizeof(struct proc_bsdinfowithuniqid);
				} else {
					error = copyout(&pbsd, buffer, sizeof(struct proc_bsdinfo));
					if (error == 0)
						*retval = sizeof(struct proc_bsdinfo);
				}
			}	
		}
		break;

		case PROC_PIDTASKINFO: {
			struct proc_taskinfo ptinfo;

			error =  proc_pidtaskinfo(p, &ptinfo);
			if (error == 0) {
				error = copyout(&ptinfo, buffer, sizeof(struct proc_taskinfo));
				if (error == 0)
					*retval = sizeof(struct proc_taskinfo);
			}	
		}
		break;

		case PROC_PIDTASKALLINFO: {
		struct proc_taskallinfo pall;

			error = proc_pidbsdinfo(p, &pall.pbsd, 0);
			error =  proc_pidtaskinfo(p, &pall.ptinfo);
			if (error == 0) {
				error = copyout(&pall, buffer, sizeof(struct proc_taskallinfo));
				if (error == 0)
					*retval = sizeof(struct proc_taskallinfo);
			}	
		}
		break;

		case PROC_PIDTHREADID64INFO:
			thuniqueid = 1;
		case PROC_PIDTHREADINFO:{
		struct proc_threadinfo pthinfo;

			error  = proc_pidthreadinfo(p,  arg, thuniqueid, &pthinfo);
			if (error == 0) {
				error = copyout(&pthinfo, buffer, sizeof(struct proc_threadinfo));
				if (error == 0)
					*retval = sizeof(struct proc_threadinfo);
			}	
		}
		break;

		case PROC_PIDLISTTHREADS:{
			error =  proc_pidlistthreads(p,  buffer, buffersize, retval);
		}
		break;

		case PROC_PIDREGIONINFO:{
			error =  proc_pidregioninfo(p,  arg, buffer, buffersize, retval);
		}
		break;


		case PROC_PIDREGIONPATHINFO:{
			error =  proc_pidregionpathinfo(p, arg, buffer, buffersize, retval);
		}
		break;

		case PROC_PIDVNODEPATHINFO:{
			error =  proc_pidvnodepathinfo(p, arg, buffer, buffersize, retval);
		}
		break;


		case PROC_PIDTHREADPATHINFO:{
		struct proc_threadwithpathinfo pinfo;

			error  = proc_pidthreadpathinfo(p,  arg, &pinfo);
			if (error == 0) {
				error = copyout((caddr_t)&pinfo, buffer, sizeof(struct proc_threadwithpathinfo));
				if (error == 0)
						*retval = sizeof(struct proc_threadwithpathinfo);
			}
		}
		break;

		case PROC_PIDPATHINFO: {
			error =  proc_pidpathinfo(p, arg, buffer, buffersize, retval);
		}
		break;


		case PROC_PIDWORKQUEUEINFO:{
		struct proc_workqueueinfo pwqinfo;

			error  = proc_pidworkqueueinfo(p, &pwqinfo);
			if (error == 0) {
				error = copyout(&pwqinfo, buffer, sizeof(struct proc_workqueueinfo));
				if (error == 0)
					*retval = sizeof(struct proc_workqueueinfo);
			}	
		}
		break;

		case PROC_PIDLISTFILEPORTS: {
			error = proc_pidfileportlist(p, buffer, buffersize,
			    retval);
		}
		break;

		default:
			error = ENOTSUP;
	}
	
out:
	if (gotref)
		proc_rele(p);
	else if (zombie)
		proc_drop_zombref(p);
	return(error);
}


int 
pid_vnodeinfo(vnode_t vp, uint32_t vid, struct fileproc * fp, int closeonexec, user_addr_t  buffer, __unused uint32_t buffersize, int32_t * retval) 
{
	struct vnode_fdinfo vfi;
	int error= 0;

	if ((error = vnode_getwithvid(vp, vid)) != 0) {
		return(error);
	}
	bzero(&vfi, sizeof(struct vnode_fdinfo));
	fill_fileinfo(fp, closeonexec, &vfi.pfi);
	error = fill_vnodeinfo(vp, &vfi.pvi);
	vnode_put(vp);
	if (error == 0) {
		error = copyout((caddr_t)&vfi, buffer, sizeof(struct vnode_fdinfo));
		if (error == 0)
			*retval = sizeof(struct vnode_fdinfo);
	}
	return(error);
}

int 
pid_vnodeinfopath(vnode_t vp, uint32_t vid, struct fileproc * fp, int closeonexec, user_addr_t  buffer, __unused uint32_t buffersize, int32_t * retval) 
{
	struct vnode_fdinfowithpath vfip;
	int count, error= 0;

	if ((error = vnode_getwithvid(vp, vid)) != 0) {
		return(error);
	}
	bzero(&vfip, sizeof(struct vnode_fdinfowithpath));
	fill_fileinfo(fp, closeonexec, &vfip.pfi);
	error = fill_vnodeinfo(vp, &vfip.pvip.vip_vi) ;
	if (error == 0) {
		count = MAXPATHLEN;
		vn_getpath(vp, &vfip.pvip.vip_path[0], &count);
		vfip.pvip.vip_path[MAXPATHLEN-1] = 0;
		vnode_put(vp);
		error = copyout((caddr_t)&vfip, buffer, sizeof(struct vnode_fdinfowithpath));
		if (error == 0)
			*retval = sizeof(struct vnode_fdinfowithpath);
	} else 
		vnode_put(vp);
	return(error);
}

void  
fill_fileinfo(struct fileproc * fp, int closeonexec, struct proc_fileinfo * fproc)
{
	fproc->fi_openflags = fp->f_fglob->fg_flag;
	fproc->fi_status = 0;
	fproc->fi_offset = fp->f_fglob->fg_offset;
	fproc->fi_type = FILEGLOB_DTYPE(fp->f_fglob);
	if (fp->f_fglob->fg_count > 1)
		fproc->fi_status |= PROC_FP_SHARED;
	if (closeonexec != 0)
		fproc->fi_status |= PROC_FP_CLEXEC;

	if (FILEPROC_TYPE(fp) == FTYPE_GUARDED) {
		fproc->fi_status |= PROC_FP_GUARDED;
		fproc->fi_guardflags = 0;
		if (fp_isguarded(fp, GUARD_CLOSE))
			fproc->fi_guardflags |= PROC_FI_GUARD_CLOSE;
		if (fp_isguarded(fp, GUARD_DUP))
			fproc->fi_guardflags |= PROC_FI_GUARD_DUP;
		if (fp_isguarded(fp, GUARD_SOCKET_IPC))
			fproc->fi_guardflags |= PROC_FI_GUARD_SOCKET_IPC;
		if (fp_isguarded(fp, GUARD_FILEPORT))
			fproc->fi_guardflags |= PROC_FI_GUARD_FILEPORT;
	}
}



int
fill_vnodeinfo(vnode_t vp, struct vnode_info *vinfo)
{
		vfs_context_t context;
		struct stat64 sb;
		int error = 0;

		context = vfs_context_create((vfs_context_t)0);
		error = vn_stat(vp, &sb, NULL, 1, context);
		(void)vfs_context_rele(context);

		munge_vinfo_stat(&sb, &vinfo->vi_stat);

		if (error != 0)
			goto out;

		if (vp->v_mount != dead_mountp) {
			vinfo->vi_fsid = vp->v_mount->mnt_vfsstat.f_fsid;
		} else {
			vinfo->vi_fsid.val[0] = 0;
			vinfo->vi_fsid.val[1] = 0;
		}
		vinfo->vi_type = vp->v_type;
out:
		return(error);
}

int
pid_socketinfo(socket_t so, struct fileproc *fp, int closeonexec, user_addr_t  buffer, __unused uint32_t buffersize, int32_t * retval)
{
#if SOCKETS
	struct socket_fdinfo s;
	int error = 0;

	bzero(&s, sizeof(struct socket_fdinfo));
	fill_fileinfo(fp, closeonexec, &s.pfi);
	if ((error = fill_socketinfo(so, &s.psi)) == 0) {
		if ((error = copyout(&s, buffer, sizeof(struct socket_fdinfo))) == 0)
				*retval = sizeof(struct socket_fdinfo);
	}
	return (error);
#else
#pragma unused(so, fp, closeonexec, buffer)
	*retval = 0;
	return (ENOTSUP);
#endif
}

int
pid_pseminfo(struct psemnode *psem, struct fileproc *fp,  int closeonexec, user_addr_t  buffer, __unused uint32_t buffersize, int32_t * retval)
{
	struct psem_fdinfo pseminfo;
	int error = 0;
 
	bzero(&pseminfo, sizeof(struct psem_fdinfo));
	fill_fileinfo(fp, closeonexec, &pseminfo.pfi);

	if ((error = fill_pseminfo(psem, &pseminfo.pseminfo)) == 0) {
		if ((error = copyout(&pseminfo, buffer, sizeof(struct psem_fdinfo))) == 0)
				*retval = sizeof(struct psem_fdinfo);
	}

	return(error);
}

int
pid_pshminfo(struct pshmnode *pshm, struct fileproc *fp,  int closeonexec, user_addr_t  buffer, __unused uint32_t buffersize, int32_t * retval)
{
	struct pshm_fdinfo pshminfo;
	int error = 0;
 
	bzero(&pshminfo, sizeof(struct pshm_fdinfo));
	fill_fileinfo(fp, closeonexec, &pshminfo.pfi);

	if ((error = fill_pshminfo(pshm, &pshminfo.pshminfo)) == 0) {
		if ((error = copyout(&pshminfo, buffer, sizeof(struct pshm_fdinfo))) == 0)
				*retval = sizeof(struct pshm_fdinfo);
	}

	return(error);
}

int
pid_pipeinfo(struct pipe *  p, struct fileproc *fp,  int closeonexec, user_addr_t  buffer, __unused uint32_t buffersize, int32_t * retval)
{
	struct pipe_fdinfo pipeinfo;
	int error = 0;

	bzero(&pipeinfo, sizeof(struct pipe_fdinfo));
	fill_fileinfo(fp, closeonexec, &pipeinfo.pfi);
	if ((error = fill_pipeinfo(p, &pipeinfo.pipeinfo)) == 0) {
		if ((error = copyout(&pipeinfo, buffer, sizeof(struct pipe_fdinfo))) == 0)
				*retval = sizeof(struct pipe_fdinfo);
	}

	return(error);
}

int
pid_kqueueinfo(struct kqueue * kq, struct fileproc *fp,  int closeonexec, user_addr_t  buffer, __unused uint32_t buffersize, int32_t * retval)
{
	struct kqueue_fdinfo kqinfo;
	int error = 0;
	
	bzero(&kqinfo, sizeof(struct kqueue_fdinfo));
 
	fill_fileinfo(fp, closeonexec, &kqinfo.pfi);

	if ((error = fill_kqueueinfo(kq, &kqinfo.kqueueinfo)) == 0) {
		if ((error = copyout(&kqinfo, buffer, sizeof(struct kqueue_fdinfo))) == 0)
				*retval = sizeof(struct kqueue_fdinfo);
	}

	return(error);
}

int
pid_atalkinfo(__unused struct atalk * at, __unused struct fileproc *fp,  __unused int closeonexec, __unused user_addr_t  buffer, __unused uint32_t buffersize, __unused int32_t * retval)
{
	return ENOTSUP;
}



/************************** proc_pidfdinfo routine ***************************/
int
proc_pidfdinfo(int pid, int flavor,  int fd, user_addr_t buffer, uint32_t buffersize, int32_t * retval)
{
	proc_t p;
	int error = ENOTSUP;
	struct fileproc * fp;
	uint32_t size;
	int closeonexec = 0;

	switch (flavor) {
		case PROC_PIDFDVNODEINFO:
			size = PROC_PIDFDVNODEINFO_SIZE;
			break;
		case PROC_PIDFDVNODEPATHINFO:
			size = PROC_PIDFDVNODEPATHINFO_SIZE;
			break;
		case PROC_PIDFDSOCKETINFO:
			size = PROC_PIDFDSOCKETINFO_SIZE;
			break;
		case PROC_PIDFDPSEMINFO:
			size = PROC_PIDFDPSEMINFO_SIZE;
			break;
		case PROC_PIDFDPSHMINFO:
			size = PROC_PIDFDPSHMINFO_SIZE;
			break;
		case PROC_PIDFDPIPEINFO:
			size = PROC_PIDFDPIPEINFO_SIZE;
			break;
		case PROC_PIDFDKQUEUEINFO:
			size = PROC_PIDFDKQUEUEINFO_SIZE;
			break;
		case PROC_PIDFDATALKINFO:
			size = PROC_PIDFDATALKINFO_SIZE;
			break;

		default:
			return(EINVAL);

	}

	if (buffersize < size)
		return(ENOMEM);

	if ((p = proc_find(pid)) == PROC_NULL) {
		error = ESRCH;
		goto out;
	}

	/* Do we have permission to look into this? */
	if ((error = proc_security_policy(p, PROC_INFO_CALL_PIDFDINFO, flavor, CHECK_SAME_USER)))
		goto out1;

	switch (flavor) {
		case PROC_PIDFDVNODEINFO: {
			vnode_t vp;
			uint32_t vid=0;

			if ((error = fp_getfvpandvid(p, fd, &fp,  &vp, &vid)) !=0) {
				goto out1;
			}
			/* no need to be under the fdlock */
			closeonexec = p->p_fd->fd_ofileflags[fd] & UF_EXCLOSE;
			error =  pid_vnodeinfo(vp, vid, fp, closeonexec, buffer, buffersize, retval);
		}
		break;

		case PROC_PIDFDVNODEPATHINFO: {
			vnode_t vp;
			uint32_t vid=0;

			if ((error = fp_getfvpandvid(p, fd, &fp,  &vp, &vid)) !=0) {
				goto out1;
			}

			/* no need to be under the fdlock */
			closeonexec = p->p_fd->fd_ofileflags[fd] & UF_EXCLOSE;
			error =  pid_vnodeinfopath(vp, vid, fp, closeonexec, buffer, buffersize, retval);
		}
		break;

		case PROC_PIDFDSOCKETINFO: {
			socket_t so; 

			if ((error = fp_getfsock(p, fd, &fp,  &so)) !=0) {
				goto out1;
			}
			/* no need to be under the fdlock */
			closeonexec = p->p_fd->fd_ofileflags[fd] & UF_EXCLOSE;
			error =  pid_socketinfo(so, fp, closeonexec, buffer, buffersize, retval);
		}
		break;

		case PROC_PIDFDPSEMINFO: {
			struct psemnode * psem;

			if ((error = fp_getfpsem(p, fd, &fp,  &psem)) !=0) {
				goto out1;
			}
			/* no need to be under the fdlock */
			closeonexec = p->p_fd->fd_ofileflags[fd] & UF_EXCLOSE;
			error =  pid_pseminfo(psem, fp, closeonexec, buffer, buffersize, retval);
		}
		break;

		case PROC_PIDFDPSHMINFO: {
			struct pshmnode * pshm;

			if ((error = fp_getfpshm(p, fd, &fp,  &pshm)) !=0) {
				goto out1;
			}
			/* no need to be under the fdlock */
			closeonexec = p->p_fd->fd_ofileflags[fd] & UF_EXCLOSE;
			error =  pid_pshminfo(pshm, fp, closeonexec, buffer, buffersize, retval);
		}
		break;

		case PROC_PIDFDPIPEINFO: {
			struct pipe * cpipe;

			if ((error = fp_getfpipe(p, fd, &fp,  &cpipe)) !=0) {
				goto out1;
			}
			/* no need to be under the fdlock */
			closeonexec = p->p_fd->fd_ofileflags[fd] & UF_EXCLOSE;
			error =  pid_pipeinfo(cpipe, fp, closeonexec, buffer, buffersize, retval);
		}
		break;

		case PROC_PIDFDKQUEUEINFO: {
			struct kqueue * kq;

			if ((error = fp_getfkq(p, fd, &fp,  &kq)) !=0) {
				goto out1;
			}
			/* no need to be under the fdlock */
			closeonexec = p->p_fd->fd_ofileflags[fd] & UF_EXCLOSE;
			error =  pid_kqueueinfo(kq, fp, closeonexec, buffer, buffersize, retval);
		}
		break;

		default: {
			error = EINVAL;
			goto out1;
		}
	}

	fp_drop(p, fd, fp , 0); 	
out1 :
	proc_rele(p);
out:
	return(error);
}

/*
 * Helper function for proc_pidfileportinfo
 */

struct fileport_info_args {
	int		fia_flavor;
	user_addr_t	fia_buffer;
	uint32_t	fia_buffersize;
	int32_t		*fia_retval;
};

static kern_return_t
proc_fileport_info(__unused mach_port_name_t name,
	struct fileglob *fg, void *arg)
{
	struct fileport_info_args *fia = arg;
	struct fileproc __fileproc, *fp = &__fileproc;
	int error;

	bzero(fp, sizeof (*fp));
	fp->f_fglob = fg;

	switch (fia->fia_flavor) {
	case PROC_PIDFILEPORTVNODEPATHINFO: {
		vnode_t vp;

		if (FILEGLOB_DTYPE(fg) != DTYPE_VNODE) {
			error = ENOTSUP;
			break;
		}
		vp = (struct vnode *)fg->fg_data;
		error = pid_vnodeinfopath(vp, vnode_vid(vp), fp, 0,
		    fia->fia_buffer, fia->fia_buffersize, fia->fia_retval);
	}	break;

	case PROC_PIDFILEPORTSOCKETINFO: {
		socket_t so;

		if (FILEGLOB_DTYPE(fg) != DTYPE_SOCKET) {
			error = EOPNOTSUPP;
			break;
		}
		so = (socket_t)fg->fg_data;
		error = pid_socketinfo(so, fp, 0,
		    fia->fia_buffer, fia->fia_buffersize, fia->fia_retval);
	}	break;

	case PROC_PIDFILEPORTPSHMINFO: {
		struct pshmnode *pshm;

		if (FILEGLOB_DTYPE(fg) != DTYPE_PSXSHM) {
			error = EBADF;		/* ick - mirror fp_getfpshm */
			break;
		}
		pshm = (struct pshmnode *)fg->fg_data;
		error = pid_pshminfo(pshm, fp, 0,
		    fia->fia_buffer, fia->fia_buffersize, fia->fia_retval);
	}	break;

	case PROC_PIDFILEPORTPIPEINFO: {
		struct pipe *cpipe;

		if (FILEGLOB_DTYPE(fg) != DTYPE_PIPE) {
			error = EBADF;		/* ick - mirror fp_getfpipe */
			break;
		}
		cpipe = (struct pipe *)fg->fg_data;
		error = pid_pipeinfo(cpipe, fp, 0,
		    fia->fia_buffer, fia->fia_buffersize, fia->fia_retval);
	}	break;

	default:
		error = EINVAL;
		break;
	}

	return (error);
}

/************************* proc_pidfileportinfo routine *********************/
int
proc_pidfileportinfo(int pid, int flavor, mach_port_name_t name,
	user_addr_t buffer, uint32_t buffersize, int32_t *retval)
{
	proc_t p;
	int error = ENOTSUP;
	uint32_t size;
	struct fileport_info_args fia;

	/* fileport types are restricted by filetype_issendable() */

	switch (flavor) {
	case PROC_PIDFILEPORTVNODEPATHINFO:
		size = PROC_PIDFILEPORTVNODEPATHINFO_SIZE;
		break;
	case PROC_PIDFILEPORTSOCKETINFO:
		size = PROC_PIDFILEPORTSOCKETINFO_SIZE;
		break;
	case PROC_PIDFILEPORTPSHMINFO:
		size = PROC_PIDFILEPORTPSHMINFO_SIZE;
		break;
	case PROC_PIDFILEPORTPIPEINFO:
		size = PROC_PIDFILEPORTPIPEINFO_SIZE;
		break;
	default:
		return (EINVAL);
	}
 
	if (buffersize < size)
		return (ENOMEM);
	if ((p = proc_find(pid)) == PROC_NULL) {
		error = ESRCH;
		goto out;
	}

	/* Do we have permission to look into this? */
	if ((error = proc_security_policy(p, PROC_INFO_CALL_PIDFILEPORTINFO, flavor, CHECK_SAME_USER)))
		goto out1;

	fia.fia_flavor = flavor;
	fia.fia_buffer = buffer;
	fia.fia_buffersize = buffersize;
	fia.fia_retval = retval;

	if (fileport_invoke(p->task, name,
	    proc_fileport_info, &fia, &error) != KERN_SUCCESS)
		error = EINVAL;
out1:
	proc_rele(p);
out:
	return (error);
}

int
proc_security_policy(proc_t targetp, __unused int callnum, __unused int flavor, boolean_t check_same_user)
{
#if CONFIG_MACF
	int error = 0;

	if ((error = mac_proc_check_proc_info(current_proc(), targetp, callnum, flavor)))
		return (error);
#endif

	/* The 'listpids' call doesn't have a target proc */
	if (targetp == PROC_NULL) {
		assert(callnum == PROC_INFO_CALL_LISTPIDS && check_same_user == NO_CHECK_SAME_USER);
		return (0);
	}

	/*
	 * Check for 'get information for processes owned by other users' privilege
	 * root has this privilege by default
	 */
	if (priv_check_cred(kauth_cred_get(), PRIV_GLOBAL_PROC_INFO, 0) == 0)
		check_same_user = FALSE;

	if (check_same_user) {
		kauth_cred_t target_cred;
		uid_t        target_uid;

		target_cred = kauth_cred_proc_ref(targetp);
		target_uid  = kauth_cred_getuid(target_cred);
		kauth_cred_unref(&target_cred);

		if (kauth_getuid() != target_uid)
			return(EPERM);
	}

	return(0);
}

int 
proc_kernmsgbuf(user_addr_t buffer, uint32_t buffersize, int32_t * retval)
{
	if (suser(kauth_cred_get(), (u_short *)0) == 0) {
		return(log_dmesg(buffer, buffersize, retval));
	} else
		return(EPERM);
}

/* ********* process control sets on self only */
int 
proc_setcontrol(int pid, int flavor, uint64_t arg, user_addr_t buffer, uint32_t buffersize, __unused int32_t * retval)
{
	struct proc * pself = PROC_NULL;
	int error = 0;
	uint32_t pcontrol = (uint32_t)arg;
	struct uthread *ut = NULL;


	pself = current_proc();
	if (pid != pself->p_pid)
		return(EINVAL);

	/* Do we have permission to look into this? */
	if ((error = proc_security_policy(pself, PROC_INFO_CALL_SETCONTROL, flavor, NO_CHECK_SAME_USER)))
		goto out;

	switch (flavor) {
		case PROC_SELFSET_PCONTROL: {
			if (pcontrol > P_PCMAX)
				return(EINVAL);
			proc_lock(pself);
			/* reset existing control setting while retaining action state */
			pself->p_pcaction &= PROC_ACTION_MASK;
			/* set new control state */
			pself->p_pcaction |= pcontrol;
			proc_unlock(pself);
		}
		break;

		case PROC_SELFSET_THREADNAME: {
			/* PROC_SELFSET_THREADNAME_SIZE = (MAXTHREADNAMESIZE -1) */
			if(buffersize > PROC_SELFSET_THREADNAME_SIZE)
				return ENAMETOOLONG;
			ut = current_uthread();

			if(!ut->pth_name)
			{
				ut->pth_name = (char*)kalloc(MAXTHREADNAMESIZE );
				if(!ut->pth_name)
					return ENOMEM;
			}
			bzero(ut->pth_name, MAXTHREADNAMESIZE);
			error = copyin(buffer, ut->pth_name, buffersize);
		}
		break;

		case PROC_SELFSET_VMRSRCOWNER: {
			/* need to to be superuser */
			if (suser(kauth_cred_get(), (u_short *)0) != 0) {
				error = EPERM;
				goto out;
			}

			proc_lock(pself);
			/* reset existing control setting while retaining action state */
			pself->p_lflag |= P_LVMRSRCOWNER;
			proc_unlock(pself);
		}
		break;

		case PROC_SELFSET_DELAYIDLESLEEP: {
			/* mark or clear the process property to delay idle sleep disk IO */
			if (pcontrol != 0)
				OSBitOrAtomic(P_DELAYIDLESLEEP, &pself->p_flag);
			else
				OSBitAndAtomic(~((uint32_t)P_DELAYIDLESLEEP), &pself->p_flag);
		}
		break;

		default:
			error = ENOTSUP;
	}
	
out:
	return(error);
}

#if CONFIG_MEMORYSTATUS

int
proc_dirtycontrol(int pid, int flavor, uint64_t arg, int32_t *retval) {
	struct proc *target_p;
	int error = 0;
	uint32_t pcontrol = (uint32_t)arg;
	kauth_cred_t my_cred, target_cred;
	boolean_t self = FALSE;
	boolean_t child = FALSE;
	boolean_t zombref = FALSE;
	pid_t selfpid;

	target_p = proc_find(pid);

	if (target_p == PROC_NULL) {
		if (flavor == PROC_DIRTYCONTROL_GET) {
			target_p = proc_find_zombref(pid);
			zombref = 1;
		}

		if (target_p == PROC_NULL)
			return(ESRCH);

	}

	my_cred = kauth_cred_get();
	target_cred = kauth_cred_proc_ref(target_p);

	/* Do we have permission to look into this? */
	if ((error = proc_security_policy(target_p, PROC_INFO_CALL_DIRTYCONTROL, flavor, NO_CHECK_SAME_USER)))
		goto out;

	selfpid = proc_selfpid();
	if (pid == selfpid) {
		self = TRUE;
	} else if (target_p->p_ppid == selfpid) {
		child = TRUE;
	}
	
	switch (flavor) {
		case PROC_DIRTYCONTROL_TRACK: {
			/* Only allow the process itself, its parent, or root */
			if ((self == FALSE) && (child == FALSE) && kauth_cred_issuser(kauth_cred_get()) != TRUE) {
				error = EPERM;
				goto out;
			}

			error = memorystatus_dirty_track(target_p, pcontrol);
		}
		break;

		case PROC_DIRTYCONTROL_SET: {			
			/* Check privileges; use cansignal() here since the process could be terminated */
			if (!cansignal(current_proc(), my_cred, target_p, SIGKILL, 0)) {
				error = EPERM;
				goto out;
			}
			
			error = memorystatus_dirty_set(target_p, self, pcontrol);	
		}
		break;
		
		case PROC_DIRTYCONTROL_GET: {
			/* No permissions check - dirty state is freely available */
			if (retval) {
				*retval = memorystatus_dirty_get(target_p);
			} else {
				error = EINVAL;
			}
		}
		break;
	}

out:
	if (zombref)
		proc_drop_zombref(target_p);
	else
		proc_rele(target_p);

	kauth_cred_unref(&target_cred);
	
	return(error);	
}
#else

int
proc_dirtycontrol(__unused int pid, __unused int flavor, __unused uint64_t arg, __unused int32_t *retval) {
        return ENOTSUP;
}

#endif /* CONFIG_MEMORYSTATUS */

/*
 * proc_terminate() provides support for sudden termination.
 * SIGKILL is issued to tracked, clean processes; otherwise,
 * SIGTERM is sent.
 */

int
proc_terminate(int pid, int32_t *retval)
{
	int error = 0;
	proc_t p;
	kauth_cred_t uc = kauth_cred_get();
	int sig;

#if 0
	/* XXX: Check if these are necessary */
	AUDIT_ARG(pid, pid);
	AUDIT_ARG(signum, sig);
#endif

	if (pid <= 0 || retval == NULL) {
		return (EINVAL);
	}

	if ((p = proc_find(pid)) == NULL) {
		return (ESRCH);
	}

#if 0
	/* XXX: Check if these are necessary */
	AUDIT_ARG(process, p);
#endif

	/* Check privileges; if SIGKILL can be issued, then SIGTERM is also OK */
	if (!cansignal(current_proc(), uc, p, SIGKILL, 0)) {
		error = EPERM;
		goto out;
	}

	/* Not allowed to sudden terminate yourself */
	if (p == current_proc()) {
		error = EPERM;
		goto out;
	}

#if CONFIG_MEMORYSTATUS
	/* Determine requisite signal to issue */
	sig = memorystatus_on_terminate(p);
#else
	sig = SIGTERM;
#endif

	proc_set_task_policy(p->task, THREAD_NULL, TASK_POLICY_ATTRIBUTE,
	                     TASK_POLICY_TERMINATED, TASK_POLICY_ENABLE);

	psignal(p, sig);
	*retval = sig;

out:
	proc_rele(p);
	
	return error;
}

/*
 * copy stat64 structure into vinfo_stat structure.
 */
static void
munge_vinfo_stat(struct stat64 *sbp, struct vinfo_stat *vsbp)
{
        bzero(vsbp, sizeof(struct vinfo_stat));

	vsbp->vst_dev = sbp->st_dev;
	vsbp->vst_mode = sbp->st_mode;
	vsbp->vst_nlink = sbp->st_nlink;
	vsbp->vst_ino = sbp->st_ino;
	vsbp->vst_uid = sbp->st_uid;
	vsbp->vst_gid = sbp->st_gid;
	vsbp->vst_atime = sbp->st_atimespec.tv_sec;
	vsbp->vst_atimensec = sbp->st_atimespec.tv_nsec;
	vsbp->vst_mtime = sbp->st_mtimespec.tv_sec;
	vsbp->vst_mtimensec = sbp->st_mtimespec.tv_nsec;
	vsbp->vst_ctime = sbp->st_ctimespec.tv_sec;
	vsbp->vst_ctimensec = sbp->st_ctimespec.tv_nsec;
	vsbp->vst_birthtime = sbp->st_birthtimespec.tv_sec;
	vsbp->vst_birthtimensec = sbp->st_birthtimespec.tv_nsec;
	vsbp->vst_size = sbp->st_size;
	vsbp->vst_blocks = sbp->st_blocks;
	vsbp->vst_blksize = sbp->st_blksize;
	vsbp->vst_flags = sbp->st_flags;
	vsbp->vst_gen = sbp->st_gen;
	vsbp->vst_rdev = sbp->st_rdev;
	vsbp->vst_qspare[0] = sbp->st_qspare[0];
	vsbp->vst_qspare[1] = sbp->st_qspare[1];
}

int
proc_pid_rusage(int pid, int flavor, user_addr_t buffer, __unused int32_t *retval)
{
	proc_t          p;
	int             error;
	int             zombie = 0;

	if ((p = proc_find(pid)) == PROC_NULL) {
		if ((p = proc_find_zombref(pid)) == PROC_NULL) {
			return (ESRCH);
		}
		zombie = 1;
	}

	/* Do we have permission to look into this? */
	if ((error = proc_security_policy(p, PROC_INFO_CALL_PIDRUSAGE, flavor, CHECK_SAME_USER)))
		goto out;

	error = proc_get_rusage(p, flavor, buffer, zombie);

out:
	if (zombie)
		proc_drop_zombref(p);
	else
		proc_rele(p);

	return (error);
}


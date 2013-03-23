/*
 * Copyright (c) 2000-2004 Apple Computer, Inc. All rights reserved.
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
/* Copyright (c) 1995 NeXT Computer, Inc. All Rights Reserved */
/*
 * Copyright (c) 1982, 1986, 1989, 1993
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
 *	@(#)vfs_lookup.c	8.10 (Berkeley) 5/27/95
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/syslimits.h>
#include <sys/time.h>
#include <sys/namei.h>
#include <sys/vm.h>
#include <sys/vnode.h>
#include <sys/mount.h>
#include <sys/errno.h>
#include <sys/malloc.h>
#include <sys/filedesc.h>
#include <sys/proc.h>
#include <sys/kdebug.h>
#include <sys/unistd.h>		/* For _PC_NAME_MAX */

#include <bsm/audit_kernel.h>

#if KTRACE
#include <sys/ktrace.h>
#endif

static void kdebug_lookup(struct vnode *dp, struct componentname *cnp);

/*
 * Convert a pathname into a pointer to a locked inode.
 *
 * The FOLLOW flag is set when symbolic links are to be followed
 * when they occur at the end of the name translation process.
 * Symbolic links are always followed for all other pathname
 * components other than the last.
 *
 * The segflg defines whether the name is to be copied from user
 * space or kernel space.
 *
 * Overall outline of namei:
 *
 *	copy in name
 *	get starting directory
 *	while (!done && !error) {
 *		call lookup to search path.
 *		if symbolic link, massage name in buffer and continue
 *	}
 */
int
namei(ndp)
	register struct nameidata *ndp;
{
	register struct filedesc *fdp;	/* pointer to file descriptor state */
	register char *cp;		/* pointer into pathname argument */
	register struct vnode *dp;	/* the directory we are searching */
	struct iovec aiov;		/* uio for reading symbolic links */
	struct uio auio;
	int error, linklen;
	struct componentname *cnp = &ndp->ni_cnd;
	struct proc *p = cnp->cn_proc;
	char *tmppn;

	ndp->ni_cnd.cn_cred = ndp->ni_cnd.cn_proc->p_ucred;
#if DIAGNOSTIC
	if (!cnp->cn_cred || !cnp->cn_proc)
		panic ("namei: bad cred/proc");
	if (cnp->cn_nameiop & (~OPMASK))
		panic ("namei: nameiop contaminated with flags");
	if (cnp->cn_flags & OPMASK)
		panic ("namei: flags contaminated with nameiops");
#endif
	fdp = p->p_fd;

	/*
	 * Get a buffer for the name to be translated, and copy the
	 * name into the buffer.
	 */
	if ((cnp->cn_flags & HASBUF) == 0) {
		MALLOC_ZONE(cnp->cn_pnbuf, caddr_t,
				MAXPATHLEN, M_NAMEI, M_WAITOK);
		cnp->cn_pnlen = MAXPATHLEN;
		cnp->cn_flags |= HASBUF;
	}
	if (ndp->ni_segflg == UIO_SYSSPACE)
		error = copystr(ndp->ni_dirp, cnp->cn_pnbuf,
			    MAXPATHLEN, (size_t *)&ndp->ni_pathlen);
	else
		error = copyinstr(ndp->ni_dirp, cnp->cn_pnbuf,
			    MAXPATHLEN, (size_t *)&ndp->ni_pathlen);

	/* If we are auditing the kernel pathname, save the user pathname */
	if (cnp->cn_flags & AUDITVNPATH1)
		AUDIT_ARG(upath, p, cnp->cn_pnbuf, ARG_UPATH1); 
	if (cnp->cn_flags & AUDITVNPATH2)
		AUDIT_ARG(upath, p, cnp->cn_pnbuf, ARG_UPATH2); 

	/*
	 * Do not allow empty pathnames
	 */
	if (!error && *cnp->cn_pnbuf == '\0')
		error = ENOENT;

	if (!error && ((dp = fdp->fd_cdir) == NULL))
		error = EPERM;		/* 3382843 */

	if (error) {
		tmppn = cnp->cn_pnbuf;
		cnp->cn_pnbuf = NULL;
		cnp->cn_flags &= ~HASBUF;
		FREE_ZONE(tmppn, cnp->cn_pnlen, M_NAMEI);
		ndp->ni_vp = NULL;
		return (error);
	}
	ndp->ni_loopcnt = 0;
#if KTRACE
	if (KTRPOINT(p, KTR_NAMEI))
		ktrnamei(p->p_tracep, cnp->cn_pnbuf);
#endif

	/*
	 * Get starting point for the translation.
	 */
	if ((ndp->ni_rootdir = fdp->fd_rdir) == NULL)
		ndp->ni_rootdir = rootvnode;
	if (ndp->ni_cnd.cn_flags & USEDVP) {
	    dp = ndp->ni_dvp;
	    ndp->ni_dvp = NULL;
	} else {
	    dp = fdp->fd_cdir;
	}

	VREF(dp);
	for (;;) {
		/*
		 * Check if root directory should replace current directory.
		 * Done at start of translation and after symbolic link.
		 */
		cnp->cn_nameptr = cnp->cn_pnbuf;
		if (*(cnp->cn_nameptr) == '/') {
			vrele(dp);
			while (*(cnp->cn_nameptr) == '/') {
				cnp->cn_nameptr++;
				ndp->ni_pathlen--;
			}
			dp = ndp->ni_rootdir;
			VREF(dp);
		}
		ndp->ni_startdir = dp;
		if (error = lookup(ndp)) {
			long len = cnp->cn_pnlen;
			tmppn = cnp->cn_pnbuf;
			cnp->cn_pnbuf = NULL;
			cnp->cn_flags &= ~HASBUF;
			FREE_ZONE(tmppn, len, M_NAMEI);
			return (error);
		}
		/*
		 * Check for symbolic link
		 */
		if ((cnp->cn_flags & ISSYMLINK) == 0) {
			if ((cnp->cn_flags & (SAVENAME | SAVESTART)) == 0) {
				tmppn = cnp->cn_pnbuf;
				cnp->cn_pnbuf = NULL;
				cnp->cn_flags &= ~HASBUF;
				FREE_ZONE(tmppn, cnp->cn_pnlen, M_NAMEI);
			} else {
				cnp->cn_flags |= HASBUF;
			}
			return (0);
		}
		if ((cnp->cn_flags & LOCKPARENT) && ndp->ni_pathlen == 1)
			VOP_UNLOCK(ndp->ni_dvp, 0, p);
		if (ndp->ni_loopcnt++ >= MAXSYMLINKS) {
			error = ELOOP;
			break;
		}
		if (ndp->ni_pathlen > 1) {
			MALLOC_ZONE(cp, char *, MAXPATHLEN, M_NAMEI, M_WAITOK);
		} else {
			cp = cnp->cn_pnbuf;
		}
		aiov.iov_base = cp;
		aiov.iov_len = MAXPATHLEN;
		auio.uio_iov = &aiov;
		auio.uio_iovcnt = 1;
		auio.uio_offset = 0;
		auio.uio_rw = UIO_READ;
		auio.uio_segflg = UIO_SYSSPACE;
		auio.uio_procp = (struct proc *)0;
		auio.uio_resid = MAXPATHLEN;
		if (error = VOP_READLINK(ndp->ni_vp, &auio, cnp->cn_cred)) {
			if (ndp->ni_pathlen > 1)
				FREE_ZONE(cp, MAXPATHLEN, M_NAMEI);
			break;
		}
		linklen = MAXPATHLEN - auio.uio_resid;
		if (linklen + ndp->ni_pathlen >= MAXPATHLEN) {
			if (ndp->ni_pathlen > 1)
				FREE_ZONE(cp, MAXPATHLEN, M_NAMEI);
			error = ENAMETOOLONG;
			break;
		}
		if (ndp->ni_pathlen > 1) {
			long len = cnp->cn_pnlen;
			tmppn = cnp->cn_pnbuf;
			bcopy(ndp->ni_next, cp + linklen, ndp->ni_pathlen);
			cnp->cn_pnbuf = cp;
			cnp->cn_pnlen = MAXPATHLEN;
			FREE_ZONE(tmppn, len, M_NAMEI);
		} else
			cnp->cn_pnbuf[linklen] = '\0';
		ndp->ni_pathlen += linklen;
		vput(ndp->ni_vp);
		dp = ndp->ni_dvp;
	}

	tmppn = cnp->cn_pnbuf;
	cnp->cn_pnbuf = NULL;
	cnp->cn_flags &= ~HASBUF;
	FREE_ZONE(tmppn, cnp->cn_pnlen, M_NAMEI);

	vrele(ndp->ni_dvp);
	vput(ndp->ni_vp);
	ndp->ni_vp = NULL;
	return (error);
}

/*
 * Search a pathname.
 * This is a very central and rather complicated routine.
 *
 * The pathname is pointed to by ni_ptr and is of length ni_pathlen.
 * The starting directory is taken from ni_startdir. The pathname is
 * descended until done, or a symbolic link is encountered. The variable
 * ni_more is clear if the path is completed; it is set to one if a
 * symbolic link needing interpretation is encountered.
 *
 * The flag argument is LOOKUP, CREATE, RENAME, or DELETE depending on
 * whether the name is to be looked up, created, renamed, or deleted.
 * When CREATE, RENAME, or DELETE is specified, information usable in
 * creating, renaming, or deleting a directory entry may be calculated.
 * If flag has LOCKPARENT or'ed into it, the parent directory is returned
 * locked. If flag has WANTPARENT or'ed into it, the parent directory is
 * returned unlocked. Otherwise the parent directory is not returned. If
 * the target of the pathname exists and LOCKLEAF is or'ed into the flag
 * the target is returned locked, otherwise it is returned unlocked.
 * When creating or renaming and LOCKPARENT is specified, the target may not
 * be ".".  When deleting and LOCKPARENT is specified, the target may be ".".
 * 
 * Overall outline of lookup:
 *
 * dirloop:
 *	identify next component of name at ndp->ni_ptr
 *	handle degenerate case where name is null string
 *	if .. and crossing mount points and on mounted filesys, find parent
 *	call VOP_LOOKUP routine for next component name
 *	    directory vnode returned in ni_dvp, unlocked unless LOCKPARENT set
 *	    component vnode returned in ni_vp (if it exists), locked.
 *	if result vnode is mounted on and crossing mount points,
 *	    find mounted on vnode
 *	if more components of name, do next level at dirloop
 *	return the answer in ni_vp, locked if LOCKLEAF set
 *	    if LOCKPARENT set, return locked parent in ni_dvp
 *	    if WANTPARENT set, return unlocked parent in ni_dvp
 */
int
lookup(ndp)
	register struct nameidata *ndp;
{
	register char *cp;		/* pointer into pathname argument */
	register struct vnode *dp = 0;	/* the directory we are searching */
	struct vnode *tdp;		/* saved dp */
	struct mount *mp;		/* mount table entry */
	int namemax = 0;			/* maximun number of bytes for filename returned by pathconf() */
	int docache;			/* == 0 do not cache last component */
	int wantparent;			/* 1 => wantparent or lockparent flag */
	int dp_unlocked = 0;	/* 1 => dp already VOP_UNLOCK()-ed */
	int rdonly;			/* lookup read-only flag bit */
	int trailing_slash = 0;
	int error = 0;
	struct componentname *cnp = &ndp->ni_cnd;
	struct proc *p = cnp->cn_proc;
	int i;

	/*
	 * Setup: break out flag bits into variables.
	 */
	wantparent = cnp->cn_flags & (LOCKPARENT | WANTPARENT);
	docache = (cnp->cn_flags & NOCACHE) ^ NOCACHE;
	if (cnp->cn_nameiop == DELETE ||
	    (wantparent && cnp->cn_nameiop != CREATE &&
             cnp->cn_nameiop != LOOKUP))
		docache = 0;
	rdonly = cnp->cn_flags & RDONLY;
	ndp->ni_dvp = NULL;
	cnp->cn_flags &= ~ISSYMLINK;
	dp = ndp->ni_startdir;
	ndp->ni_startdir = NULLVP;
	vn_lock(dp, LK_EXCLUSIVE | LK_RETRY, p);
	cnp->cn_consume = 0;

dirloop:
	/*
	 * Search a new directory.
	 *
	 * The cn_hash value is for use by vfs_cache.
	 * The last component of the filename is left accessible via
	 * cnp->cn_nameptr for callers that need the name. Callers needing
	 * the name set the SAVENAME flag. When done, they assume
	 * responsibility for freeing the pathname buffer.
	 */
	{
		register unsigned int hash;
		register unsigned int ch;
		register int i;
	
		hash = 0;
		cp = cnp->cn_nameptr;
		ch = *cp;
		if (ch == '\0') {
			cnp->cn_namelen = 0;
			goto emptyname;
		}

		for (i = 1; (ch != '/') && (ch != '\0'); i++) {
			hash += ch * i;
			ch = *(++cp);
		}
		cnp->cn_hash = hash;
	}
	cnp->cn_namelen = cp - cnp->cn_nameptr;
	if (cnp->cn_namelen > NCHNAMLEN) {
		if (VOP_PATHCONF(dp, _PC_NAME_MAX, &namemax))
			namemax = NAME_MAX;
		if (cnp->cn_namelen > namemax) {
			error = ENAMETOOLONG;
			goto bad;
		}
	}
#ifdef NAMEI_DIAGNOSTIC
	{ char c = *cp;
	*cp = '\0';
	printf("{%s}: ", cnp->cn_nameptr);
	*cp = c; }
#endif
	ndp->ni_pathlen -= cnp->cn_namelen;
	ndp->ni_next = cp;

	/*
	 * Replace multiple slashes by a single slash and trailing slashes
	 * by a null.  This must be done before VOP_LOOKUP() because some
	 * fs's don't know about trailing slashes.  Remember if there were
	 * trailing slashes to handle symlinks, existing non-directories
	 * and non-existing files that won't be directories specially later.
	 */
	trailing_slash = 0;
	while (*cp == '/' && (cp[1] == '/' || cp[1] == '\0')) {
		cp++;
		ndp->ni_pathlen--;
		if (*cp == '\0') {
			trailing_slash = 1;
			*ndp->ni_next = '\0';
		}
	}
	ndp->ni_next = cp;

	cnp->cn_flags |= MAKEENTRY;
	if (*cp == '\0' && docache == 0)
		cnp->cn_flags &= ~MAKEENTRY;

	if (*ndp->ni_next == 0)
		cnp->cn_flags |= ISLASTCN;
	else
		cnp->cn_flags &= ~ISLASTCN;

	/*
	 * Handle "..": two special cases.
	 * 1. If at root directory (e.g. after chroot)
	 *    or at absolute root directory
	 *    then ignore it so can't get out.
	 * 2. If this vnode is the root of a mounted
	 *    filesystem, then replace it with the
	 *    vnode which was mounted on so we take the
	 *    .. in the other file system.
	 */
	if (cnp->cn_namelen == 2 &&
	    cnp->cn_nameptr[1] == '.' && cnp->cn_nameptr[0] == '.') {
		cnp->cn_flags |= ISDOTDOT;

		for (;;) {
			if (dp == ndp->ni_rootdir || dp == rootvnode) {
				ndp->ni_dvp = dp;
				ndp->ni_vp = dp;
				VREF(dp);
				goto nextname;
			}
			if ((dp->v_flag & VROOT) == 0 ||
			    (cnp->cn_flags & NOCROSSMOUNT))
				break;
			if (dp->v_mount == NULL) {	/* forced umount */
				error = EBADF;
				goto bad;
			}

			tdp = dp;
			dp = dp->v_mount->mnt_vnodecovered;
			vput(tdp);
			VREF(dp);
			vn_lock(dp, LK_EXCLUSIVE | LK_RETRY, p);
		}
	} else {
		cnp->cn_flags &= ~ISDOTDOT;
	}

	/*
	 * We now have a segment name to search for, and a directory to search.
	 */
unionlookup:
	ndp->ni_dvp = dp;
	ndp->ni_vp = NULL;
	if (error = VOP_LOOKUP(dp, &ndp->ni_vp, cnp)) {
#if DIAGNOSTIC
		if (ndp->ni_vp != NULL)
			panic("leaf should be empty");
#endif
#ifdef NAMEI_DIAGNOSTIC
		printf("not found\n");
#endif
		if ((error == ENOENT) &&
		    (dp->v_flag & VROOT) && (dp->v_mount != NULL) &&
		    (dp->v_mount->mnt_flag & MNT_UNION)) {
			tdp = dp;
			dp = dp->v_mount->mnt_vnodecovered;
			vput(tdp);
			VREF(dp);
			vn_lock(dp, LK_EXCLUSIVE | LK_RETRY, p);
			goto unionlookup;
		}

		if (error != EJUSTRETURN)
			goto bad;
		/*
		 * If creating and at end of pathname, then can consider
		 * allowing file to be created.
		 */
		if (rdonly) {
			error = EROFS;
			goto bad;
		}
		if (*cp == '\0' && trailing_slash &&
			!(cnp->cn_flags & WILLBEDIR)) {
			error = ENOENT;
			goto bad;
		}
		/*
		 * We return with ni_vp NULL to indicate that the entry
		 * doesn't currently exist, leaving a pointer to the
		 * (possibly locked) directory inode in ndp->ni_dvp.
		 */
		if (cnp->cn_flags & SAVESTART) {
			ndp->ni_startdir = ndp->ni_dvp;
			VREF(ndp->ni_startdir);
		}
		if (kdebug_enable)
		        kdebug_lookup(ndp->ni_dvp, cnp);
		return (0);
	}
#ifdef NAMEI_DIAGNOSTIC
	printf("found\n");
#endif

	/*
	 * Take into account any additional components consumed by
	 * the underlying filesystem.
	 */
	if (cnp->cn_consume > 0) {
		cnp->cn_nameptr += cnp->cn_consume;
		ndp->ni_next += cnp->cn_consume;
		ndp->ni_pathlen -= cnp->cn_consume;
		cnp->cn_consume = 0;
	} else {
	    int isdot_or_dotdot;

	    isdot_or_dotdot = (cnp->cn_namelen == 1 && cnp->cn_nameptr[0] == '.') || (cnp->cn_flags & ISDOTDOT);
	    
	    if (VNAME(ndp->ni_vp) == NULL &&  isdot_or_dotdot == 0) {
		VNAME(ndp->ni_vp) = add_name(cnp->cn_nameptr, cnp->cn_namelen, cnp->cn_hash, 0);
	    }
	    if (VPARENT(ndp->ni_vp) == NULL && isdot_or_dotdot == 0) {
		if (vget(ndp->ni_dvp, 0, p) == 0) {
		    VPARENT(ndp->ni_vp) = ndp->ni_dvp;
		}
	    }
	}

	dp = ndp->ni_vp;
	/*
	 * Check to see if the vnode has been mounted on;
	 * if so find the root of the mounted file system.
	 */
	while (dp->v_type == VDIR && (mp = dp->v_mountedhere) &&
	       (cnp->cn_flags & NOCROSSMOUNT) == 0) {
		if (vfs_busy(mp, LK_NOWAIT, 0, p)) {
			error = ENOENT;
			goto bad2;
		}
		VOP_UNLOCK(dp, 0, p);
		error = VFS_ROOT(mp, &tdp);
		vfs_unbusy(mp, p);
		if (error) {
			dp_unlocked = 1;		/* Signal error path 'dp' has already been unlocked */
			goto bad2;
		};
		vrele(dp);
		ndp->ni_vp = dp = tdp;
	}

	/*
	 * Check for symbolic link
	 */
	if ((dp->v_type == VLNK) &&
	    ((cnp->cn_flags & FOLLOW) || trailing_slash ||
		*ndp->ni_next == '/')) {
		cnp->cn_flags |= ISSYMLINK;
		return (0);
	}

	/*
	 * Check for bogus trailing slashes.
	 */
	if (trailing_slash) {
		if (dp->v_type != VDIR) {
			error = ENOTDIR;
			goto bad2;
		}
		trailing_slash = 0;
	 }

nextname:
	/*
	 * Not a symbolic link.  If more pathname,
	 * continue at next component, else return.
	 */
	if (*ndp->ni_next == '/') {
		cnp->cn_nameptr = ndp->ni_next + 1;
		ndp->ni_pathlen--;
		while (*cnp->cn_nameptr == '/') {
			cnp->cn_nameptr++;
			ndp->ni_pathlen--;
		}
		vrele(ndp->ni_dvp);
		goto dirloop;
	}
				  
	/*
	 * Disallow directory write attempts on read-only file systems.
	 */
	if (rdonly &&
	    (cnp->cn_nameiop == DELETE || cnp->cn_nameiop == RENAME)) {
		error = EROFS;
		goto bad2;
	}
	if (cnp->cn_flags & SAVESTART) {
		ndp->ni_startdir = ndp->ni_dvp;
		VREF(ndp->ni_startdir);
	}
	if (!wantparent)
		vrele(ndp->ni_dvp);
	if (cnp->cn_flags & AUDITVNPATH1)
		AUDIT_ARG(vnpath, dp, ARG_VNODE1);
	else if (cnp->cn_flags & AUDITVNPATH2)
		AUDIT_ARG(vnpath, dp, ARG_VNODE2);
	if ((cnp->cn_flags & LOCKLEAF) == 0)
		VOP_UNLOCK(dp, 0, p);
	if (kdebug_enable)
	        kdebug_lookup(dp, cnp);
	return (0);

emptyname:
	/*
	 * A degenerate name (e.g. / or "") which is a way of
	 * talking about a directory, e.g. like "/." or ".".
	 */
	if (dp->v_type != VDIR) {
		error = ENOTDIR;
		goto bad;
	}
	if (cnp->cn_nameiop != LOOKUP) {
		error = EISDIR;
		goto bad;
	}
	if (wantparent) {
		ndp->ni_dvp = dp;
		VREF(dp);
	}
	cnp->cn_flags &= ~ISDOTDOT;
	cnp->cn_flags |= ISLASTCN;
	ndp->ni_next = cp;
	ndp->ni_vp = dp;
	if (cnp->cn_flags & AUDITVNPATH1)
		AUDIT_ARG(vnpath, dp, ARG_VNODE1);
	else if (cnp->cn_flags & AUDITVNPATH2)
		AUDIT_ARG(vnpath, dp, ARG_VNODE2);
	if (!(cnp->cn_flags & (LOCKPARENT | LOCKLEAF)))
		VOP_UNLOCK(dp, 0, p);
	if (cnp->cn_flags & SAVESTART)
		panic("lookup: SAVESTART");
	return (0);

bad2:
	if ((cnp->cn_flags & LOCKPARENT) && *ndp->ni_next == '\0')
		VOP_UNLOCK(ndp->ni_dvp, 0, p);
	vrele(ndp->ni_dvp);
bad:
	if (dp_unlocked) {
		vrele(dp);
	} else {
		vput(dp);
	};
	ndp->ni_vp = NULL;
	if (kdebug_enable)
	        kdebug_lookup(dp, cnp);
	return (error);
}

/*
 * relookup - lookup a path name component
 *    Used by lookup to re-aquire things.
 */
int
relookup(dvp, vpp, cnp)
	struct vnode *dvp, **vpp;
	struct componentname *cnp;
{
	struct proc *p = cnp->cn_proc;
	struct vnode *dp = 0;		/* the directory we are searching */
	int docache;			/* == 0 do not cache last component */
	int wantparent;			/* 1 => wantparent or lockparent flag */
	int rdonly;			/* lookup read-only flag bit */
	int error = 0;
#ifdef NAMEI_DIAGNOSTIC
	int i, newhash;			/* DEBUG: check name hash */
	char *cp;			/* DEBUG: check name ptr/len */
#endif

	/*
	 * Setup: break out flag bits into variables.
	 */
	wantparent = cnp->cn_flags & (LOCKPARENT|WANTPARENT);
	docache = (cnp->cn_flags & NOCACHE) ^ NOCACHE;
	if (cnp->cn_nameiop == DELETE ||
	    (wantparent && cnp->cn_nameiop != CREATE))
		docache = 0;
	rdonly = cnp->cn_flags & RDONLY;
	cnp->cn_flags &= ~ISSYMLINK;
	dp = dvp;
	vn_lock(dp, LK_EXCLUSIVE | LK_RETRY, p);

/* dirloop: */
	/*
	 * Search a new directory.
	 *
	 * The cn_hash value is for use by vfs_cache.
	 * The last component of the filename is left accessible via
	 * cnp->cn_nameptr for callers that need the name. Callers needing
	 * the name set the SAVENAME flag. When done, they assume
	 * responsibility for freeing the pathname buffer.
	 */
#ifdef NAMEI_DIAGNOSTIC
	for (i=1, newhash = 0, cp = cnp->cn_nameptr; *cp != 0 && *cp != '/'; cp++)
		newhash += (unsigned char)*cp * i;
	if (newhash != cnp->cn_hash)
		panic("relookup: bad hash");
	if (cnp->cn_namelen != cp - cnp->cn_nameptr)
		panic ("relookup: bad len");
	if (*cp != 0)
		panic("relookup: not last component");
	printf("{%s}: ", cnp->cn_nameptr);
#endif

	/*
	 * Check for degenerate name (e.g. / or "")
	 * which is a way of talking about a directory,
	 * e.g. like "/." or ".".
	 */
	if (cnp->cn_nameptr[0] == '\0') {
		if (cnp->cn_nameiop != LOOKUP || wantparent) {
			error = EISDIR;
			goto bad;
		}
		if (dp->v_type != VDIR) {
			error = ENOTDIR;
			goto bad;
		}
		if (!(cnp->cn_flags & LOCKLEAF))
			VOP_UNLOCK(dp, 0, p);
		*vpp = dp;
		if (cnp->cn_flags & SAVESTART)
			panic("lookup: SAVESTART");
		return (0);
	}

	if (cnp->cn_flags & ISDOTDOT)
		panic ("relookup: lookup on dot-dot");

	/*
	 * We now have a segment name to search for, and a directory to search.
	 */
	if (error = VOP_LOOKUP(dp, vpp, cnp)) {
#if DIAGNOSTIC
		if (*vpp != NULL)
			panic("leaf should be empty");
#endif
		if (error != EJUSTRETURN)
			goto bad;
		/*
		 * If creating and at end of pathname, then can consider
		 * allowing file to be created.
		 */
		if (rdonly) {
			error = EROFS;
			goto bad;
		}
		/* ASSERT(dvp == ndp->ni_startdir) */
		if (cnp->cn_flags & SAVESTART)
			VREF(dvp);
		/*
		 * We return with ni_vp NULL to indicate that the entry
		 * doesn't currently exist, leaving a pointer to the
		 * (possibly locked) directory inode in ndp->ni_dvp.
		 */
		return (0);
	}
	dp = *vpp;

#if DIAGNOSTIC
	/*
	 * Check for symbolic link
	 */
	if (dp->v_type == VLNK && (cnp->cn_flags & FOLLOW))
		panic ("relookup: symlink found.\n");
#endif

	/*
	 * Disallow directory write attempts on read-only file systems.
	 */
	if (rdonly &&
	    (cnp->cn_nameiop == DELETE || cnp->cn_nameiop == RENAME)) {
		error = EROFS;
		goto bad2;
	}
	/* ASSERT(dvp == ndp->ni_startdir) */
	if (cnp->cn_flags & SAVESTART)
		VREF(dvp);
	
	if (!wantparent)
		vrele(dvp);
	if ((cnp->cn_flags & LOCKLEAF) == 0)
		VOP_UNLOCK(dp, 0, p);
	return (0);

bad2:
	if ((cnp->cn_flags & LOCKPARENT) && (cnp->cn_flags & ISLASTCN))
		VOP_UNLOCK(dvp, 0, p);
	vrele(dvp);
bad:
	vput(dp);
	*vpp = NULL;
	return (error);
}


#define NUMPARMS 23

static void
kdebug_lookup(dp, cnp)
	struct vnode *dp;
	struct componentname *cnp;
{
	register int i, n;
	register int dbg_namelen;
	register int save_dbg_namelen;
	register char *dbg_nameptr;
	long dbg_parms[NUMPARMS];
	char dbg_buf[4];
	static char *dbg_filler = ">>>>";

	/* Collect the pathname for tracing */
	dbg_namelen = (cnp->cn_nameptr - cnp->cn_pnbuf) + cnp->cn_namelen;
	dbg_nameptr = cnp->cn_nameptr + cnp->cn_namelen;

	if (dbg_namelen > sizeof(dbg_parms))
	    dbg_namelen = sizeof(dbg_parms);
	dbg_nameptr -= dbg_namelen;
	save_dbg_namelen = dbg_namelen;

	i = 0;

	while (dbg_namelen > 0) {
	    if (dbg_namelen >= 4) {
	        dbg_parms[i++] = *(long *)dbg_nameptr;
		dbg_nameptr += sizeof(long);
		dbg_namelen -= sizeof(long);
	    } else {
	        for (n = 0; n < dbg_namelen; n++)
		    dbg_buf[n] = *dbg_nameptr++;
		while (n <= 3) {
		    if (*dbg_nameptr)
		        dbg_buf[n++] = '>';
		    else
		        dbg_buf[n++] = 0;
		}
		dbg_parms[i++] = *(long *)&dbg_buf[0];

		break;
	    }
	}
	while (i < NUMPARMS) {
	    if (*dbg_nameptr)
	        dbg_parms[i++] = *(long *)dbg_filler;
	    else
	        dbg_parms[i++] = 0;
	}

	/*
	  In the event that we collect multiple, consecutive pathname
	  entries, we must mark the start of the path's string.
	*/
	KERNEL_DEBUG_CONSTANT((FSDBG_CODE(DBG_FSRW,36)) | DBG_FUNC_START,
		(unsigned int)dp, dbg_parms[0], dbg_parms[1], dbg_parms[2], 0);

	for (dbg_namelen = save_dbg_namelen-12, i=3;
	     dbg_namelen > 0;
	     dbg_namelen -=(4 * sizeof(long)), i+= 4)
	  {
	    KERNEL_DEBUG_CONSTANT((FSDBG_CODE(DBG_FSRW,36)) | DBG_FUNC_NONE,
				  dbg_parms[i], dbg_parms[i+1], dbg_parms[i+2], dbg_parms[i+3], 0);
	  }
}

/*
 * Copyright (c) 2000-2004 Apple Computer, Inc. All rights reserved.
 *
 * @APPLE_LICENSE_HEADER_START@
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
 * Copyright 1997,1998 Julian Elischer.  All rights reserved.
 * julian@freebsd.org
 * 
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 *  1. Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer.
 *  2. Redistributions in binary form must reproduce the above copyright notice,
 *     this list of conditions and the following disclaimer in the documentation
 *     and/or other materials provided with the distribution.
 * 
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDER ``AS IS'' AND ANY EXPRESS
 * OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED.  IN NO EVENT SHALL THE HOLDER OR CONTRIBUTORS BE LIABLE FOR
 * ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 * CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 * 
 * devfs_vnops.c
 */

/*
 * HISTORY
 *  Clark Warner (warner_c@apple.com) Tue Feb 10 2000
 *  - Added err_copyfile to the vnode operations table
 *  Dieter Siegmund (dieter@apple.com) Thu Apr  8 14:08:19 PDT 1999
 *  - instead of duplicating specfs here, created a vnode-ops table
 *    that redirects most operations to specfs (as is done with ufs);
 *  - removed routines that made no sense
 *  - cleaned up reclaim: replaced devfs_vntodn() with a macro VTODN()
 *  - cleaned up symlink, link locking
 *  - added the devfs_lock to protect devfs data structures against
 *    driver's calling devfs_add_devswf()/etc.
 *  Dieter Siegmund (dieter@apple.com) Wed Jul 14 13:37:59 PDT 1999
 *  - free the devfs devnode in devfs_inactive(), not just in devfs_reclaim()
 *    to free up kernel memory as soon as it's available
 *  - got rid of devfsspec_{read, write}
 *  Dieter Siegmund (dieter@apple.com) Fri Sep 17 09:58:38 PDT 1999
 *  - update the mod/access times
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/namei.h>
#include <sys/kernel.h>
#include <sys/fcntl.h>
#include <sys/conf.h>
#include <sys/disklabel.h>
#include <sys/lock.h>
#include <sys/stat.h>
#include <sys/mount_internal.h>
#include <sys/proc.h>
#include <sys/kauth.h>
#include <sys/time.h>
#include <sys/vnode_internal.h>
#include <miscfs/specfs/specdev.h>
#include <sys/dirent.h>
#include <sys/vmmeter.h>
#include <sys/vm.h>
#include <sys/uio_internal.h>

#include "devfsdefs.h"

static int devfs_update(struct vnode *vp, struct timeval *access,
                        struct timeval *modify);


/*
 * Convert a component of a pathname into a pointer to a locked node.
 * This is a very central and rather complicated routine.
 * If the file system is not maintained in a strict tree hierarchy,
 * this can result in a deadlock situation (see comments in code below).
 *
 * The flag argument is LOOKUP, CREATE, RENAME, or DELETE depending on
 * whether the name is to be looked up, created, renamed, or deleted.
 * When CREATE, RENAME, or DELETE is specified, information usable in
 * creating, renaming, or deleting a directory entry may be calculated.
 * If flag has LOCKPARENT or'ed into it and the target of the pathname
 * exists, lookup returns both the target and its parent directory locked.
 * When creating or renaming and LOCKPARENT is specified, the target may
 * not be ".".  When deleting and LOCKPARENT is specified, the target may
 * be "."., but the caller must check to ensure it does an vrele and DNUNLOCK
 * instead of two DNUNLOCKs.
 *
 * Overall outline of devfs_lookup:
 *
 *	check accessibility of directory
 *	null terminate the component (lookup leaves the whole string alone)
 *	look for name in cache, if found, then if at end of path
 *	  and deleting or creating, drop it, else return name
 *	search for name in directory, to found or notfound
 * notfound:
 *	if creating, return locked directory,
 *	else return error
 * found:
 *	if at end of path and deleting, return information to allow delete
 *	if at end of path and rewriting (RENAME and LOCKPARENT), lock target
 *	  node and return info to allow rewrite
 *	if not at end, add name to cache; if at end and neither creating
 *	  nor deleting, add name to cache
 * On return to lookup, remove the null termination we put in at the start.
 *
 * NOTE: (LOOKUP | LOCKPARENT) currently returns the parent node unlocked.
 */
static int
devfs_lookup(struct vnop_lookup_args *ap)
        /*struct vnop_lookup_args {
                struct vnode * a_dvp; directory vnode ptr
                struct vnode ** a_vpp; where to put the result
                struct componentname * a_cnp; the name we want
		vfs_context_t a_context;
        };*/
{
	struct componentname *cnp = ap->a_cnp;
	vfs_context_t ctx = cnp->cn_context;
	struct proc *p = vfs_context_proc(ctx);
	struct vnode *dir_vnode = ap->a_dvp;
	struct vnode **result_vnode = ap->a_vpp;
	devnode_t *   dir_node;       /* the directory we are searching */
	devnode_t *   node = NULL;       /* the node we are searching for */
	devdirent_t * nodename;
	int flags = cnp->cn_flags;
	int op = cnp->cn_nameiop;       /* LOOKUP, CREATE, RENAME, or DELETE */
	int wantparent = flags & (LOCKPARENT|WANTPARENT);
	int error = 0;
	char	heldchar;	/* the char at the end of the name componet */

retry:

	*result_vnode = NULL; /* safe not sorry */ /*XXX*/

	//if (dir_vnode->v_usecount == 0)
	    //printf("devfs_lookup: dir had no refs ");
	dir_node = VTODN(dir_vnode);

	/*
	 * Make sure that our node is a directory as well.
	 */
	if (dir_node->dn_type != DEV_DIR) {
		return (ENOTDIR);
	}

	DEVFS_LOCK();
	/*
	 * temporarily terminate string component
	 */
	heldchar = cnp->cn_nameptr[cnp->cn_namelen];
	cnp->cn_nameptr[cnp->cn_namelen] = '\0';

	nodename = dev_findname(dir_node, cnp->cn_nameptr);
	/*
	 * restore saved character
	 */
	cnp->cn_nameptr[cnp->cn_namelen] = heldchar;

	if (nodename) {
	        /* entry exists */
	        node = nodename->de_dnp;

		/* Do potential vnode allocation here inside the lock 
		 * to make sure that our device node has a non-NULL dn_vn
		 * associated with it.  The device node might otherwise
		 * get deleted out from under us (see devfs_dn_free()).
		 */
		error = devfs_dntovn(node, result_vnode, p);
	}
	DEVFS_UNLOCK();

	if (error) {
	        if (error == EAGAIN)
		        goto retry;
		return error;
	}
	if (!nodename) {
		/*
		 * we haven't called devfs_dntovn if we get here
		 * we have not taken a reference on the node.. no
		 * vnode_put is necessary on these error returns
		 *
		 * If it doesn't exist and we're not the last component,
		 * or we're at the last component, but we're not creating
		 * or renaming, return ENOENT.
		 */
        	if (!(flags & ISLASTCN) || !(op == CREATE || op == RENAME)) {
			return ENOENT;
		}
		/*
		 * We return with the directory locked, so that
		 * the parameters we set up above will still be
		 * valid if we actually decide to add a new entry.
		 * We return ni_vp == NULL to indicate that the entry
		 * does not currently exist; we leave a pointer to
		 * the (locked) directory vnode in namei_data->ni_dvp.
		 *
		 * NB - if the directory is unlocked, then this
		 * information cannot be used.
		 */
		return (EJUSTRETURN);
	}
	/*
	 * from this point forward, we need to vnode_put the reference
	 * picked up in devfs_dntovn if we decide to return an error
	 */

	/*
	 * If deleting, and at end of pathname, return
	 * parameters which can be used to remove file.
	 * If the wantparent flag isn't set, we return only
	 * the directory (in namei_data->ni_dvp), otherwise we go
	 * on and lock the node, being careful with ".".
	 */
	if (op == DELETE && (flags & ISLASTCN)) {

		/*
		 * we are trying to delete '.'.  What does this mean? XXX
		 */
		if (dir_node == node) {
		        if (*result_vnode) {
			        vnode_put(*result_vnode);
			        *result_vnode = NULL;
			}				
			if ( ((error = vnode_get(dir_vnode)) == 0) ) {
			        *result_vnode = dir_vnode;
			}
			return (error);
		}
		return (0);
	}

	/*
	 * If rewriting (RENAME), return the vnode and the
	 * information required to rewrite the present directory
	 * Must get node of directory entry to verify it's a
	 * regular file, or empty directory.
	 */
	if (op == RENAME && wantparent && (flags & ISLASTCN)) {

		/*
		 * Careful about locking second node.
		 * This can only occur if the target is ".".
		 */
		if (dir_node == node) {
		        error = EISDIR;
			goto drop_ref;
		}
		return (0);
	}

	/*
	 * Step through the translation in the name.  We do not unlock the
	 * directory because we may need it again if a symbolic link
	 * is relative to the current directory.  Instead we save it
	 * unlocked as "saved_dir_node" XXX.  We must get the target
	 * node before unlocking
	 * the directory to insure that the node will not be removed
	 * before we get it.  We prevent deadlock by always fetching
	 * nodes from the root, moving down the directory tree. Thus
	 * when following backward pointers ".." we must unlock the
	 * parent directory before getting the requested directory.
	 * There is a potential race condition here if both the current
	 * and parent directories are removed before the lock for the
	 * node associated with ".." returns.  We hope that this occurs
	 * infrequently since we cannot avoid this race condition without
	 * implementing a sophisticated deadlock detection algorithm.
	 * Note also that this simple deadlock detection scheme will not
	 * work if the file system has any hard links other than ".."
	 * that point backwards in the directory structure.
	 */
	if ((flags & ISDOTDOT) == 0 && dir_node == node) {
	        if (*result_vnode) {
		        vnode_put(*result_vnode);
		        *result_vnode = NULL;
		}
		if ( (error = vnode_get(dir_vnode)) ) {
			return (error);
		}
		*result_vnode = dir_vnode;
	}
	return (0);

drop_ref:
	if (*result_vnode) {
	        vnode_put(*result_vnode);
		*result_vnode = NULL;
	}
	return (error);
}

static int
devfs_getattr(struct vnop_getattr_args *ap)
        /*struct vnop_getattr_args {
                struct vnode *a_vp;
                struct vnode_attr *a_vap;
                kauth_cred_t a_cred;
                struct proc *a_p;
        } */ 
{
	struct vnode *vp = ap->a_vp;
	struct vnode_attr *vap = ap->a_vap;
	devnode_t *	file_node;
	struct timeval now;

	file_node = VTODN(vp);

	DEVFS_LOCK();

	microtime(&now);
	dn_times(file_node, &now, &now, &now);

	VATTR_RETURN(vap, va_mode, file_node->dn_mode);

	switch (file_node->dn_type)
	{
	case 	DEV_DIR:
		VATTR_RETURN(vap, va_rdev,  (dev_t)file_node->dn_dvm);
		vap->va_mode |= (S_IFDIR);
		break;
	case	DEV_CDEV:
		VATTR_RETURN(vap, va_rdev, file_node->dn_typeinfo.dev);
		vap->va_mode |= (S_IFCHR);
		break;
	case	DEV_BDEV:
		VATTR_RETURN(vap, va_rdev, file_node->dn_typeinfo.dev);
		vap->va_mode |= (S_IFBLK);
		break;
	case	DEV_SLNK:
		VATTR_RETURN(vap, va_rdev, 0);
		vap->va_mode |= (S_IFLNK);
		break;
	default:
		VATTR_RETURN(vap, va_rdev, 0);	/* default value only */
	}
	VATTR_RETURN(vap, va_type, vp->v_type);
	VATTR_RETURN(vap, va_nlink, file_node->dn_links);
	VATTR_RETURN(vap, va_uid, file_node->dn_uid);
	VATTR_RETURN(vap, va_gid, file_node->dn_gid);
	VATTR_RETURN(vap, va_fsid, (uintptr_t)file_node->dn_dvm);
	VATTR_RETURN(vap, va_fileid, (uintptr_t)file_node);
	VATTR_RETURN(vap, va_data_size, file_node->dn_len);

	/* return an override block size (advisory) */
	if (vp->v_type == VBLK)
		VATTR_RETURN(vap, va_iosize, BLKDEV_IOSIZE);
	else if (vp->v_type == VCHR)
		VATTR_RETURN(vap, va_iosize, MAXPHYSIO);
	else
		VATTR_RETURN(vap, va_iosize, vp->v_mount->mnt_vfsstat.f_iosize);
	/* if the time is bogus, set it to the boot time */
	if (file_node->dn_ctime.tv_sec == 0) {
		file_node->dn_ctime.tv_sec = boottime_sec();
		file_node->dn_ctime.tv_nsec = 0;
	}
	if (file_node->dn_mtime.tv_sec == 0)
	    file_node->dn_mtime = file_node->dn_ctime;
	if (file_node->dn_atime.tv_sec == 0)
	    file_node->dn_atime = file_node->dn_ctime;
	VATTR_RETURN(vap, va_change_time, file_node->dn_ctime);
	VATTR_RETURN(vap, va_modify_time, file_node->dn_mtime);
	VATTR_RETURN(vap, va_access_time, file_node->dn_atime);
	VATTR_RETURN(vap, va_gen, 0);
	VATTR_RETURN(vap, va_flags, 0);
	VATTR_RETURN(vap, va_filerev, 0);
	VATTR_RETURN(vap, va_acl, NULL);

	DEVFS_UNLOCK();

	return 0;
}

static int
devfs_setattr(struct vnop_setattr_args *ap)
	/*struct vnop_setattr_args  {
	  struct vnode *a_vp;
	  struct vnode_attr *a_vap;
	  vfs_context_t a_context;
          } */ 
{
  	struct vnode *vp = ap->a_vp;
 	struct vnode_attr *vap = ap->a_vap;
  	kauth_cred_t cred = vfs_context_ucred(ap->a_context);
  	struct proc *p = vfs_context_proc(ap->a_context);
  	int error = 0;
  	devnode_t *	file_node;
  	struct timeval atimeval, mtimeval;
  
  	file_node = VTODN(vp);
  
 	DEVFS_LOCK();
  	/*
  	 * Go through the fields and update if set.
  	 */
 	if (VATTR_IS_ACTIVE(vap, va_access_time) || VATTR_IS_ACTIVE(vap, va_modify_time)) {
  
  
		if (VATTR_IS_ACTIVE(vap, va_access_time))
			file_node->dn_access = 1;
		if (VATTR_IS_ACTIVE(vap, va_modify_time)) {
			file_node->dn_change = 1;
			file_node->dn_update = 1;
		}
		atimeval.tv_sec = vap->va_access_time.tv_sec;
		atimeval.tv_usec = vap->va_access_time.tv_nsec / 1000;
		mtimeval.tv_sec = vap->va_modify_time.tv_sec;
		mtimeval.tv_usec = vap->va_modify_time.tv_nsec / 1000;
  
		if ( (error = devfs_update(vp, &atimeval, &mtimeval)) )
			goto exit;
 	}
 	VATTR_SET_SUPPORTED(vap, va_access_time);
 	VATTR_SET_SUPPORTED(vap, va_change_time);
  
  	/*
  	 * Change the permissions.
  	 */
 	if (VATTR_IS_ACTIVE(vap, va_mode)) {
  		file_node->dn_mode &= ~07777;
  		file_node->dn_mode |= vap->va_mode & 07777;
  	}
 	VATTR_SET_SUPPORTED(vap, va_mode);
  
  	/*
  	 * Change the owner.
  	 */
 	if (VATTR_IS_ACTIVE(vap, va_uid))
  		file_node->dn_uid = vap->va_uid;
 	VATTR_SET_SUPPORTED(vap, va_uid);
  
  	/*
  	 * Change the group.
  	 */
 	if (VATTR_IS_ACTIVE(vap, va_gid))
  		file_node->dn_gid = vap->va_gid;
 	VATTR_SET_SUPPORTED(vap, va_gid);
	exit:
	DEVFS_UNLOCK();

	return error;
}

static int
devfs_read(struct vnop_read_args *ap)
        /* struct vnop_read_args {
                struct vnode *a_vp;
                struct uio *a_uio;
                int  a_ioflag;
		vfs_context_t a_context;
        } */
{
    	devnode_t * dn_p = VTODN(ap->a_vp);

	switch (ap->a_vp->v_type) {
	  case VDIR: {
	      dn_p->dn_access = 1;

	      return VNOP_READDIR(ap->a_vp, ap->a_uio, 0, NULL, NULL, ap->a_context);
	  }
	  default: {
	      printf("devfs_read(): bad file type %d", ap->a_vp->v_type);
	      return(EINVAL);
	      break;
	  }
	}
	return (0); /* not reached */
}

static int
devfs_close(struct vnop_close_args *ap)
        /* struct vnop_close_args {
		struct vnode *a_vp;
		int  a_fflag;
		vfs_context_t a_context;
	} */
{
    	struct vnode *	    	vp = ap->a_vp;
	register devnode_t * 	dnp = VTODN(vp);
	struct timeval now;

	if (vnode_isinuse(vp, 1)) {
	    DEVFS_LOCK();
	    microtime(&now);
	    dn_times(dnp, &now, &now, &now);
	    DEVFS_UNLOCK();
	}
	return (0);
}

static int
devfsspec_close(struct vnop_close_args *ap)
        /* struct vnop_close_args {
		struct vnode *a_vp;
		int  a_fflag;
		vfs_context_t a_context;
	} */
{
    	struct vnode *	    	vp = ap->a_vp;
	register devnode_t * 	dnp = VTODN(vp);
	struct timeval now;

	if (vnode_isinuse(vp, 1)) {
	    DEVFS_LOCK();
	    microtime(&now);
	    dn_times(dnp, &now, &now, &now);
	    DEVFS_UNLOCK();
	}
	return (VOCALL (spec_vnodeop_p, VOFFSET(vnop_close), ap));
}

static int
devfsspec_read(struct vnop_read_args *ap)
        /* struct vnop_read_args {
                struct vnode *a_vp;
                struct uio *a_uio;
                int  a_ioflag;
                kauth_cred_t a_cred;
        } */
{
	register devnode_t * 	dnp = VTODN(ap->a_vp);

	dnp->dn_access = 1;

	return (VOCALL (spec_vnodeop_p, VOFFSET(vnop_read), ap));
}

static int
devfsspec_write(struct vnop_write_args *ap)
        /* struct vnop_write_args  {
                struct vnode *a_vp;
                struct uio *a_uio;
                int  a_ioflag;
		vfs_context_t a_context;
        } */
{
	register devnode_t * 	dnp = VTODN(ap->a_vp);

	dnp->dn_change = 1;
	dnp->dn_update = 1;

	return (VOCALL (spec_vnodeop_p, VOFFSET(vnop_write), ap));
}

/*
 *  Write data to a file or directory.
 */
static int
devfs_write(struct vnop_write_args *ap)
        /* struct vnop_write_args  {
                struct vnode *a_vp;
                struct uio *a_uio;
                int  a_ioflag;
                kauth_cred_t a_cred;
        } */
{
	switch (ap->a_vp->v_type) {
	case VDIR:
		return(EISDIR);
	default:
		printf("devfs_write(): bad file type %d", ap->a_vp->v_type);
		return (EINVAL);
	}
	return 0; /* not reached */
}

static int
devfs_remove(struct vnop_remove_args *ap)
        /* struct vnop_remove_args  {
                struct vnode *a_dvp;
                struct vnode *a_vp;
                struct componentname *a_cnp;
        } */ 
{
	struct vnode *vp = ap->a_vp;
	struct vnode *dvp = ap->a_dvp;
	struct componentname *cnp = ap->a_cnp;
	vfs_context_t ctx = cnp->cn_context;
	devnode_t *  tp;
	devnode_t *  tdp;
	devdirent_t * tnp;
	int doingdirectory = 0;
	int error = 0;
	uid_t ouruid = kauth_cred_getuid(vfs_context_ucred(ctx));

	/*
	 * assume that the name is null terminated as they
	 * are the end of the path. Get pointers to all our
	 * devfs structures.
	 */
	tp = VTODN(vp);
	tdp = VTODN(dvp);

	DEVFS_LOCK();

	tnp = dev_findname(tdp, cnp->cn_nameptr);

	if (tnp == NULL) {
	        error = ENOENT;
		goto abort;
	}

	/*
	 * Make sure that we don't try do something stupid
	 */
	if ((tp->dn_type) == DEV_DIR) {
		/*
		 * Avoid ".", "..", and aliases of "." for obvious reasons.
		 */
		if ( (cnp->cn_namelen == 1 && cnp->cn_nameptr[0] == '.') 
		    || (cnp->cn_flags&ISDOTDOT) ) {
			error = EINVAL;
			goto abort;
		}
		doingdirectory++;
	}

	/***********************************
	 * Start actually doing things.... *
	 ***********************************/
	tdp->dn_change = 1;
	tdp->dn_update = 1;

	/*
	 * Target must be empty if a directory and have no links
	 * to it. Also, ensure source and target are compatible
	 * (both directories, or both not directories).
	 */
	if (( doingdirectory) && (tp->dn_links > 2)) {
	    error = ENOTEMPTY;
	    goto abort;
	}
	dev_free_name(tnp);
abort:
	DEVFS_UNLOCK();

	return (error);
}

/*
 */
static int
devfs_link(struct vnop_link_args *ap)
        /*struct vnop_link_args  {
                struct vnode *a_tdvp;
                struct vnode *a_vp;
                struct componentname *a_cnp;
		vfs_context_t a_context;
        } */ 
{
	struct vnode *vp = ap->a_vp;
	struct vnode *tdvp = ap->a_tdvp;
	struct componentname *cnp = ap->a_cnp;
	devnode_t * fp;
	devnode_t * tdp;
	devdirent_t * tnp;
	int error = 0;
	struct timeval now;

	/*
	 * First catch an arbitrary restriction for this FS
	 */
	if (cnp->cn_namelen > DEVMAXNAMESIZE) {
		error = ENAMETOOLONG;
		goto out1;
	}

	/*
	 * Lock our directories and get our name pointers
	 * assume that the names are null terminated as they
	 * are the end of the path. Get pointers to all our
	 * devfs structures.
	 */
	tdp = VTODN(tdvp);
	fp = VTODN(vp);
	
	if (tdvp->v_mount != vp->v_mount) {
		return (EXDEV);
	}
	DEVFS_LOCK();


	/***********************************
	 * Start actually doing things.... *
	 ***********************************/
	fp->dn_change = 1;

	microtime(&now);
	error = devfs_update(vp, &now, &now);

	if (!error) {
	    error = dev_add_name(cnp->cn_nameptr, tdp, NULL, fp, &tnp);
	}
out1:
	DEVFS_UNLOCK();

	return (error);
}

/*
 * Rename system call. Seems overly complicated to me...
 * 	rename("foo", "bar");
 * is essentially
 *	unlink("bar");
 *	link("foo", "bar");
 *	unlink("foo");
 * but ``atomically''.
 *
 * When the target exists, both the directory
 * and target vnodes are locked.
 * the source and source-parent vnodes are referenced
 *
 *
 * Basic algorithm is:
 *
 * 1) Bump link count on source while we're linking it to the
 *    target.  This also ensure the inode won't be deleted out
 *    from underneath us while we work (it may be truncated by
 *    a concurrent `trunc' or `open' for creation).
 * 2) Link source to destination.  If destination already exists,
 *    delete it first.
 * 3) Unlink source reference to node if still around. If a
 *    directory was moved and the parent of the destination
 *    is different from the source, patch the ".." entry in the
 *    directory.
 */
static int
devfs_rename(struct vnop_rename_args *ap)
        /*struct vnop_rename_args  {
                struct vnode *a_fdvp; 
                struct vnode *a_fvp;  
                struct componentname *a_fcnp;
                struct vnode *a_tdvp;
                struct vnode *a_tvp;
                struct componentname *a_tcnp;
		vfs_context_t a_context;
        } */
{
	struct vnode *tvp = ap->a_tvp;
	struct vnode *tdvp = ap->a_tdvp;
	struct vnode *fvp = ap->a_fvp;
	struct vnode *fdvp = ap->a_fdvp;
	struct componentname *tcnp = ap->a_tcnp;
	struct componentname *fcnp = ap->a_fcnp;
	devnode_t *fp, *fdp, *tp, *tdp;
	devdirent_t *fnp,*tnp;
	int doingdirectory = 0;
	int error = 0;
	struct timeval now;

	DEVFS_LOCK();
	/*
	 * First catch an arbitrary restriction for this FS
	 */
	if (tcnp->cn_namelen > DEVMAXNAMESIZE) {
		error = ENAMETOOLONG;
		goto out;
	}

	/*
	 * assume that the names are null terminated as they
	 * are the end of the path. Get pointers to all our
	 * devfs structures.
	 */
	tdp = VTODN(tdvp);
	fdp = VTODN(fdvp);
	fp = VTODN(fvp);

	fnp = dev_findname(fdp, fcnp->cn_nameptr);

	if (fnp == NULL) {
	        error = ENOENT;
		goto out;
	}
	tp = NULL;
	tnp = NULL;

	if (tvp) {
		tnp = dev_findname(tdp, tcnp->cn_nameptr);

		if (tnp == NULL) {
		        error = ENOENT;
			goto out;
		}
		tp = VTODN(tvp);
	}
	
	/*
	 * Make sure that we don't try do something stupid
	 */
	if ((fp->dn_type) == DEV_DIR) {
		/*
		 * Avoid ".", "..", and aliases of "." for obvious reasons.
		 */
		if ((fcnp->cn_namelen == 1 && fcnp->cn_nameptr[0] == '.') 
		    || (fcnp->cn_flags&ISDOTDOT) 
		    || (tcnp->cn_namelen == 1 && tcnp->cn_nameptr[0] == '.') 
		    || (tcnp->cn_flags&ISDOTDOT) 
		    || (tdp == fp )) {
			error = EINVAL;
			goto out;
		}
		doingdirectory++;
	}

	/*
	 * If ".." must be changed (ie the directory gets a new
	 * parent) then the source directory must not be in the
	 * directory hierarchy above the target, as this would
	 * orphan everything below the source directory. Also
	 * the user must have write permission in the source so
	 * as to be able to change "..". 
	 */
	if (doingdirectory && (tdp != fdp)) {
		devnode_t * tmp, *ntmp;
		tmp = tdp;
		do {
			if(tmp == fp) {
				/* XXX unlock stuff here probably */
				error = EINVAL;
				goto out;
			}
			ntmp = tmp;
		} while ((tmp = tmp->dn_typeinfo.Dir.parent) != ntmp);
	}

	/***********************************
	 * Start actually doing things.... *
	 ***********************************/
	fp->dn_change = 1;
	microtime(&now);

	if ( (error = devfs_update(fvp, &now, &now)) ) {
	    goto out;
	}
	/*
	 * Check if just deleting a link name.
	 */
	if (fvp == tvp) {
		if (fvp->v_type == VDIR) {
			error = EINVAL;
			goto out;
		}
		/* Release destination completely. */
		dev_free_name(fnp);

		DEVFS_UNLOCK();
		return 0;
	}
	/*
	 * 1) Bump link count while we're moving stuff
	 *    around.  If we crash somewhere before
	 *    completing our work,  too bad :)
	 */
	fp->dn_links++;
	/*
	 * If the target exists zap it (unless it's a non-empty directory)
	 * We could do that as well but won't
 	 */
	if (tp) {
		int ouruid = kauth_cred_getuid(vfs_context_ucred(tcnp->cn_context));
		/*
		 * Target must be empty if a directory and have no links
		 * to it. Also, ensure source and target are compatible
		 * (both directories, or both not directories).
		 */
		if (( doingdirectory) && (tp->dn_links > 2)) {
		        error = ENOTEMPTY;
			goto bad;
		}
		dev_free_name(tnp);
		tp = NULL;
	}
	dev_add_name(tcnp->cn_nameptr,tdp,NULL,fp,&tnp);
	fnp->de_dnp = NULL;
	fp->dn_links--; /* one less link to it.. */

	dev_free_name(fnp);
bad:
	fp->dn_links--; /* we added one earlier*/
out:
	DEVFS_UNLOCK();
	return (error);
}

static int
devfs_symlink(struct vnop_symlink_args *ap)
        /*struct vnop_symlink_args {
                struct vnode *a_dvp;
                struct vnode **a_vpp;
                struct componentname *a_cnp;
                struct vnode_attr *a_vap;
                char *a_target;
		vfs_context_t a_context;
        } */
{
	struct componentname * cnp = ap->a_cnp;
	vfs_context_t ctx = cnp->cn_context;
	struct proc *p = vfs_context_proc(ctx);
	int error = 0;
	devnode_t * dir_p;
	devnode_type_t typeinfo;
	devdirent_t * nm_p;
	devnode_t * dev_p;
	struct vnode_attr *	vap = ap->a_vap;
	struct vnode * * vpp = ap->a_vpp;

	dir_p = VTODN(ap->a_dvp);
	typeinfo.Slnk.name = ap->a_target;
	typeinfo.Slnk.namelen = strlen(ap->a_target);

	DEVFS_LOCK();
	error = dev_add_entry(cnp->cn_nameptr, dir_p, DEV_SLNK, 
			      &typeinfo, NULL, NULL, &nm_p);
	if (error) {
	    goto failure;
	}
	dev_p = nm_p->de_dnp;
	dev_p->dn_uid = dir_p->dn_uid;
	dev_p->dn_gid = dir_p->dn_gid;
	dev_p->dn_mode = vap->va_mode;
	dn_copy_times(dev_p, dir_p);

	error = devfs_dntovn(dev_p, vpp, p);
failure:
	DEVFS_UNLOCK();

	return error;
}

/*
 * Mknod vnode call
 */
static int
devfs_mknod(struct vnop_mknod_args *ap)
        /* struct vnop_mknod_args {
		struct vnode *a_dvp;
		struct vnode **a_vpp;
		struct componentname *a_cnp;
		struct vnode_attr *a_vap;
		vfs_context_t a_context;
	} */
{
    	struct componentname * cnp = ap->a_cnp;
	vfs_context_t ctx = cnp->cn_context;
	struct proc *p = vfs_context_proc(ctx);
	devnode_t *	dev_p;
	devdirent_t *	devent;
	devnode_t *	dir_p;	/* devnode for parent directory */
    	struct vnode * 	dvp = ap->a_dvp;
	int 		error = 0;
	devnode_type_t	typeinfo;
	struct vnode_attr *	vap = ap->a_vap;
	struct vnode ** vpp = ap->a_vpp;

	*vpp = NULL;
	if (!(vap->va_type == VBLK) && !(vap->va_type == VCHR)) {
	        return (EINVAL); /* only support mknod of special files */
	}
	dir_p = VTODN(dvp);
	typeinfo.dev = vap->va_rdev;

	DEVFS_LOCK();
	error = dev_add_entry(cnp->cn_nameptr, dir_p, 
			      (vap->va_type == VBLK) ? DEV_BDEV : DEV_CDEV,
			      &typeinfo, NULL, NULL, &devent);
	if (error) {
	        goto failure;
	}
	dev_p = devent->de_dnp;
	error = devfs_dntovn(dev_p, vpp, p);
	if (error)
	        goto failure;
	dev_p->dn_uid = vap->va_uid;
	dev_p->dn_gid = vap->va_gid;
	dev_p->dn_mode = vap->va_mode;
	VATTR_SET_SUPPORTED(vap, va_uid);
	VATTR_SET_SUPPORTED(vap, va_gid);
	VATTR_SET_SUPPORTED(vap, va_mode);
failure:
	DEVFS_UNLOCK();

	return (error);
}

/*
 * Vnode op for readdir
 */
static int
devfs_readdir(struct vnop_readdir_args *ap)
        /*struct vnop_readdir_args {
                struct vnode *a_vp;
                struct uio *a_uio;
		int a_flags;
		int *a_eofflag;
		int *a_numdirent;
		vfs_context_t a_context;
        } */
{
	struct vnode *vp = ap->a_vp;
	struct uio *uio = ap->a_uio;
	struct dirent dirent;
	devnode_t * dir_node;
	devdirent_t *	name_node;
	char	*name;
	int error = 0;
	int reclen;
	int nodenumber;
	int	startpos,pos;

	if (ap->a_flags & (VNODE_READDIR_EXTENDED | VNODE_READDIR_REQSEEKOFF))
		return (EINVAL);

	/*  set up refs to dir */
	dir_node = VTODN(vp);
	if (dir_node->dn_type != DEV_DIR)
		return(ENOTDIR);
	pos = 0;
	startpos = uio->uio_offset;

	DEVFS_LOCK();

	name_node = dir_node->dn_typeinfo.Dir.dirlist;
	nodenumber = 0;

	dir_node->dn_access = 1;

	while ((name_node || (nodenumber < 2)) && (uio_resid(uio) > 0))
	{
		switch(nodenumber)
		{
		case	0:
			dirent.d_fileno = (int32_t)(void *)dir_node;
			name = ".";
			dirent.d_namlen = 1;
			dirent.d_type = DT_DIR;
			break;
		case	1:
			if(dir_node->dn_typeinfo.Dir.parent)
			    dirent.d_fileno
				= (int32_t)dir_node->dn_typeinfo.Dir.parent;
			else
				dirent.d_fileno = (u_int32_t)dir_node;
			name = "..";
			dirent.d_namlen = 2;
			dirent.d_type = DT_DIR;
			break;
		default:
			dirent.d_fileno = (int32_t)(void *)name_node->de_dnp;
			dirent.d_namlen = strlen(name_node->de_name);
			name = name_node->de_name;
			switch(name_node->de_dnp->dn_type) {
			case DEV_BDEV:
				dirent.d_type = DT_BLK;
				break;
			case DEV_CDEV:
				dirent.d_type = DT_CHR;
				break;
			case DEV_DIR:
				dirent.d_type = DT_DIR;
				break;
			case DEV_SLNK:
				dirent.d_type = DT_LNK;
				break;
			default:
				dirent.d_type = DT_UNKNOWN;
			}
		}
#define	GENERIC_DIRSIZ(dp) \
    ((sizeof (struct dirent) - (MAXNAMLEN+1)) + (((dp)->d_namlen+1 + 3) &~ 3))

		reclen = dirent.d_reclen = GENERIC_DIRSIZ(&dirent);

		if(pos >= startpos)	/* made it to the offset yet? */
		{
			if (uio_resid(uio) < reclen) /* will it fit? */
				break;
			strcpy( dirent.d_name,name);
			if ((error = uiomove ((caddr_t)&dirent,
					dirent.d_reclen, uio)) != 0)
				break;
		}
		pos += reclen;
		if((nodenumber >1) && name_node)
			name_node = name_node->de_next;
		nodenumber++;
	}
	DEVFS_UNLOCK();
	uio->uio_offset = pos;

	return (error);
}


/*
 */
static int
devfs_readlink(struct vnop_readlink_args *ap)
        /*struct vnop_readlink_args {
                struct vnode *a_vp;
                struct uio *a_uio;
		vfs_context_t a_context;
        } */
{
	struct vnode *vp = ap->a_vp;
	struct uio *uio = ap->a_uio;
	devnode_t * lnk_node;
	int error = 0;

	/*  set up refs to dir */
	lnk_node = VTODN(vp);

	if (lnk_node->dn_type != DEV_SLNK) {
	        error = EINVAL;
		goto out;
	}
	error = uiomove(lnk_node->dn_typeinfo.Slnk.name, 
			lnk_node->dn_typeinfo.Slnk.namelen, uio);
out:	
	return error;
}

static int
devfs_reclaim(struct vnop_reclaim_args *ap)
        /*struct vnop_reclaim_args {
		struct vnode *a_vp;
        } */
{
    struct vnode *	vp = ap->a_vp;
    devnode_t * 	dnp = VTODN(vp);
    
    DEVFS_LOCK();

    if (dnp) {
	/* 
	 * do the same as devfs_inactive in case it is not called
	 * before us (can that ever happen?)
	 */
	dnp->dn_vn = NULL;
	vp->v_data = NULL;

	if (dnp->dn_delete) {
	    devnode_free(dnp);
	}
    }
    DEVFS_UNLOCK();

    return(0);
}


/*
 * Get configurable pathname variables.
 */
static int
devs_vnop_pathconf(
	struct vnop_pathconf_args /* {
		struct vnode *a_vp;
		int a_name;
		int *a_retval;
		vfs_context_t a_context;
	} */ *ap)
{
	switch (ap->a_name) {
	case _PC_LINK_MAX:
		/* arbitrary limit matching HFS; devfs has no hard limit */
		*ap->a_retval = 32767;
		break;
	case _PC_NAME_MAX:
		*ap->a_retval = DEVMAXNAMESIZE - 1;	/* includes NUL */
		break;
	case _PC_PATH_MAX:
		*ap->a_retval = DEVMAXPATHSIZE - 1;	/* XXX nonconformant */
		break;
	case _PC_CHOWN_RESTRICTED:
		*ap->a_retval = 1;
		break;
	case _PC_NO_TRUNC:
		*ap->a_retval = 0;
		break;
	case _PC_CASE_SENSITIVE:
		*ap->a_retval = 1;
		break;
	case _PC_CASE_PRESERVING:
		*ap->a_retval = 1;
		break;
	default:
		return (EINVAL);
	}

	return (0);
}



/**************************************************************************\
* pseudo ops *
\**************************************************************************/

/*
 *
 *	struct vnop_inactive_args {
 *		struct vnode *a_vp;
 *		vfs_context_t a_context;
 *	} 
 */

static int
devfs_inactive(__unused struct vnop_inactive_args *ap)
{
    return (0);
}

/*
 * called with DEVFS_LOCK held
 */
static int
devfs_update(struct vnode *vp, struct timeval *access, struct timeval *modify)
{
	devnode_t * ip;
	struct timeval now;

	ip = VTODN(vp);
	if (vp->v_mount->mnt_flag & MNT_RDONLY) {
	        ip->dn_access = 0;
	        ip->dn_change = 0;
	        ip->dn_update = 0;

		return (0);
	}
	microtime(&now);
	dn_times(ip, access, modify, &now);

	return (0);
}

#define VOPFUNC int (*)(void *)

/* The following ops are used by directories and symlinks */
int (**devfs_vnodeop_p)(void *);
static struct vnodeopv_entry_desc devfs_vnodeop_entries[] = {
	{ &vnop_default_desc, (VOPFUNC)vn_default_error },
	{ &vnop_lookup_desc, (VOPFUNC)devfs_lookup },		/* lookup */
	{ &vnop_create_desc, (VOPFUNC)err_create },		/* create */
	{ &vnop_whiteout_desc, (VOPFUNC)err_whiteout },		/* whiteout */
	{ &vnop_mknod_desc, (VOPFUNC)devfs_mknod },		/* mknod */
	{ &vnop_open_desc, (VOPFUNC)nop_open },			/* open */
	{ &vnop_close_desc, (VOPFUNC)devfs_close },		/* close */
	{ &vnop_getattr_desc, (VOPFUNC)devfs_getattr },		/* getattr */
	{ &vnop_setattr_desc, (VOPFUNC)devfs_setattr },		/* setattr */
	{ &vnop_read_desc, (VOPFUNC)devfs_read },		/* read */
	{ &vnop_write_desc, (VOPFUNC)devfs_write },		/* write */
	{ &vnop_ioctl_desc, (VOPFUNC)err_ioctl },		/* ioctl */
	{ &vnop_select_desc, (VOPFUNC)err_select },		/* select */
	{ &vnop_revoke_desc, (VOPFUNC)err_revoke },		/* revoke */
	{ &vnop_mmap_desc, (VOPFUNC)err_mmap },			/* mmap */
	{ &vnop_fsync_desc, (VOPFUNC)nop_fsync },		/* fsync */
	{ &vnop_remove_desc, (VOPFUNC)devfs_remove },		/* remove */
	{ &vnop_link_desc, (VOPFUNC)devfs_link },		/* link */
	{ &vnop_rename_desc, (VOPFUNC)devfs_rename },		/* rename */
	{ &vnop_mkdir_desc, (VOPFUNC)err_mkdir },		/* mkdir */
	{ &vnop_rmdir_desc, (VOPFUNC)err_rmdir },		/* rmdir */
	{ &vnop_symlink_desc, (VOPFUNC)devfs_symlink },		/* symlink */
	{ &vnop_readdir_desc, (VOPFUNC)devfs_readdir },		/* readdir */
	{ &vnop_readlink_desc, (VOPFUNC)devfs_readlink },	/* readlink */
	{ &vnop_inactive_desc, (VOPFUNC)devfs_inactive },	/* inactive */
	{ &vnop_reclaim_desc, (VOPFUNC)devfs_reclaim },		/* reclaim */
	{ &vnop_strategy_desc, (VOPFUNC)err_strategy },		/* strategy */
	{ &vnop_pathconf_desc, (VOPFUNC)devs_vnop_pathconf },	/* pathconf */
	{ &vnop_advlock_desc, (VOPFUNC)err_advlock },		/* advlock */
	{ &vnop_bwrite_desc, (VOPFUNC)err_bwrite },
	{ &vnop_pagein_desc, (VOPFUNC)err_pagein },		/* Pagein */
	{ &vnop_pageout_desc, (VOPFUNC)err_pageout },		/* Pageout */
	{ &vnop_copyfile_desc, (VOPFUNC)err_copyfile },		/* Copyfile */
	{ &vnop_blktooff_desc, (VOPFUNC)err_blktooff },		/* blktooff */
	{ &vnop_offtoblk_desc, (VOPFUNC)err_offtoblk },		/* offtoblk */
	{ &vnop_blockmap_desc, (VOPFUNC)err_blockmap },		/* blockmap */
	{ (struct vnodeop_desc*)NULL, (int(*)())NULL }
};
struct vnodeopv_desc devfs_vnodeop_opv_desc =
	{ &devfs_vnodeop_p, devfs_vnodeop_entries };

/* The following ops are used by the device nodes */
int (**devfs_spec_vnodeop_p)(void *);
static struct vnodeopv_entry_desc devfs_spec_vnodeop_entries[] = {
	{ &vnop_default_desc, (VOPFUNC)vn_default_error },
	{ &vnop_lookup_desc, (VOPFUNC)spec_lookup },		/* lookup */
	{ &vnop_create_desc, (VOPFUNC)spec_create },		/* create */
	{ &vnop_mknod_desc, (VOPFUNC)spec_mknod },		/* mknod */
	{ &vnop_open_desc, (VOPFUNC)spec_open },			/* open */
	{ &vnop_close_desc, (VOPFUNC)devfsspec_close },		/* close */
	{ &vnop_getattr_desc, (VOPFUNC)devfs_getattr },		/* getattr */
	{ &vnop_setattr_desc, (VOPFUNC)devfs_setattr },		/* setattr */
	{ &vnop_read_desc, (VOPFUNC)devfsspec_read },		/* read */
	{ &vnop_write_desc, (VOPFUNC)devfsspec_write },		/* write */
	{ &vnop_ioctl_desc, (VOPFUNC)spec_ioctl },		/* ioctl */
	{ &vnop_select_desc, (VOPFUNC)spec_select },		/* select */
	{ &vnop_revoke_desc, (VOPFUNC)spec_revoke },		/* revoke */
	{ &vnop_mmap_desc, (VOPFUNC)spec_mmap },			/* mmap */
	{ &vnop_fsync_desc, (VOPFUNC)spec_fsync },		/* fsync */
	{ &vnop_remove_desc, (VOPFUNC)devfs_remove },		/* remove */
	{ &vnop_link_desc, (VOPFUNC)devfs_link },		/* link */
	{ &vnop_rename_desc, (VOPFUNC)spec_rename },		/* rename */
	{ &vnop_mkdir_desc, (VOPFUNC)spec_mkdir },		/* mkdir */
	{ &vnop_rmdir_desc, (VOPFUNC)spec_rmdir },		/* rmdir */
	{ &vnop_symlink_desc, (VOPFUNC)spec_symlink },		/* symlink */
	{ &vnop_readdir_desc, (VOPFUNC)spec_readdir },		/* readdir */
	{ &vnop_readlink_desc, (VOPFUNC)spec_readlink },		/* readlink */
	{ &vnop_inactive_desc, (VOPFUNC)devfs_inactive },	/* inactive */
	{ &vnop_reclaim_desc, (VOPFUNC)devfs_reclaim },		/* reclaim */
	{ &vnop_strategy_desc, (VOPFUNC)spec_strategy },		/* strategy */
	{ &vnop_pathconf_desc, (VOPFUNC)spec_pathconf },		/* pathconf */
	{ &vnop_advlock_desc, (VOPFUNC)spec_advlock },		/* advlock */
	{ &vnop_bwrite_desc, (VOPFUNC)vn_bwrite },
	{ &vnop_devblocksize_desc, (VOPFUNC)spec_devblocksize },	/* devblocksize */
	{ &vnop_pagein_desc, (VOPFUNC)err_pagein },		/* Pagein */
	{ &vnop_pageout_desc, (VOPFUNC)err_pageout },		/* Pageout */
	{ &vnop_copyfile_desc, (VOPFUNC)err_copyfile },		/* Copyfile */
	{ &vnop_blktooff_desc, (VOPFUNC)spec_blktooff },	/* blktooff */
	{ &vnop_blktooff_desc, (VOPFUNC)spec_offtoblk  },	/* blkofftoblk */
	{ &vnop_blockmap_desc, (VOPFUNC)spec_blockmap },	/* blockmap */
	{ (struct vnodeop_desc*)NULL, (int(*)())NULL }
};
struct vnodeopv_desc devfs_spec_vnodeop_opv_desc =
	{ &devfs_spec_vnodeop_p, devfs_spec_vnodeop_entries };


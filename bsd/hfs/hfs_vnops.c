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

#include <sys/systm.h>
#include <sys/param.h>
#include <sys/kernel.h>
#include <sys/file_internal.h>
#include <sys/dirent.h>
#include <sys/stat.h>
#include <sys/buf.h>
#include <sys/mount.h>
#include <sys/vnode_if.h>
#include <sys/vnode_internal.h>
#include <sys/malloc.h>
#include <sys/ubc.h>
#include <sys/ubc_internal.h>
#include <sys/paths.h>
#include <sys/quota.h>
#include <sys/time.h>
#include <sys/disk.h>
#include <sys/kauth.h>
#include <sys/uio_internal.h>

#include <miscfs/specfs/specdev.h>
#include <miscfs/fifofs/fifo.h>
#include <vfs/vfs_support.h>
#include <machine/spl.h>

#include <sys/kdebug.h>
#include <sys/sysctl.h>

#include "hfs.h"
#include "hfs_catalog.h"
#include "hfs_cnode.h"
#include "hfs_dbg.h"
#include "hfs_mount.h"
#include "hfs_quota.h"
#include "hfs_endian.h"

#include "hfscommon/headers/BTreesInternal.h"
#include "hfscommon/headers/FileMgrInternal.h"

#define KNDETACH_VNLOCKED 0x00000001

/* Global vfs data structures for hfs */

/* Always F_FULLFSYNC? 1=yes,0=no (default due to "various" reasons is 'no') */
int always_do_fullfsync = 0;
SYSCTL_DECL(_vfs_generic);
SYSCTL_INT (_vfs_generic, OID_AUTO, always_do_fullfsync, CTLFLAG_RW, &always_do_fullfsync, 0, "always F_FULLFSYNC when fsync is called");

static int hfs_makenode(struct vnode *dvp, struct vnode **vpp,
                        struct componentname *cnp, struct vnode_attr *vap,
                        vfs_context_t ctx);

static int hfs_metasync(struct hfsmount *hfsmp, daddr64_t node, __unused struct proc *p);
static int hfs_metasync_all(struct hfsmount *hfsmp);

static int hfs_removedir(struct vnode *, struct vnode *, struct componentname *,
                         int);

static int hfs_removefile(struct vnode *, struct vnode *, struct componentname *,
                          int, int, int, struct vnode *);

#if FIFO
static int hfsfifo_read(struct vnop_read_args *);
static int hfsfifo_write(struct vnop_write_args *);
static int hfsfifo_close(struct vnop_close_args *);

extern int (**fifo_vnodeop_p)(void *);
#endif /* FIFO */

static int hfs_vnop_close(struct vnop_close_args*);
static int hfs_vnop_create(struct vnop_create_args*);
static int hfs_vnop_exchange(struct vnop_exchange_args*);
static int hfs_vnop_fsync(struct vnop_fsync_args*);
static int hfs_vnop_mkdir(struct vnop_mkdir_args*);
static int hfs_vnop_mknod(struct vnop_mknod_args*);
static int hfs_vnop_getattr(struct vnop_getattr_args*);
static int hfs_vnop_open(struct vnop_open_args*);
static int hfs_vnop_readdir(struct vnop_readdir_args*);
static int hfs_vnop_remove(struct vnop_remove_args*);
static int hfs_vnop_rename(struct vnop_rename_args*);
static int hfs_vnop_rmdir(struct vnop_rmdir_args*);
static int hfs_vnop_symlink(struct vnop_symlink_args*);
static int hfs_vnop_setattr(struct vnop_setattr_args*);
static int hfs_vnop_readlink(struct vnop_readlink_args *);
static int hfs_vnop_pathconf(struct vnop_pathconf_args *);
static int hfs_vnop_whiteout(struct vnop_whiteout_args *);
static int hfsspec_read(struct vnop_read_args *);
static int hfsspec_write(struct vnop_write_args *);
static int hfsspec_close(struct vnop_close_args *);

/* Options for hfs_removedir and hfs_removefile */
#define HFSRM_SKIP_RESERVE  0x01




/*****************************************************************************
*
* Common Operations on vnodes
*
*****************************************************************************/

/*
 * Create a regular file.
 */
static int
hfs_vnop_create(struct vnop_create_args *ap)
{
	int error;

again:
	error = hfs_makenode(ap->a_dvp, ap->a_vpp, ap->a_cnp, ap->a_vap, ap->a_context);

	/*
	 * We speculatively skipped the original lookup of the leaf
	 * for CREATE.  Since it exists, go get it as long as they
	 * didn't want an exclusive create.
	 */
	if ((error == EEXIST) && !(ap->a_vap->va_vaflags & VA_EXCLUSIVE)) {
		struct vnop_lookup_args args;

		args.a_desc = &vnop_lookup_desc;
		args.a_dvp = ap->a_dvp;
		args.a_vpp = ap->a_vpp;
		args.a_cnp = ap->a_cnp;
		args.a_context = ap->a_context;
		args.a_cnp->cn_nameiop = LOOKUP;
		error = hfs_vnop_lookup(&args);
		/*
		 * We can also race with remove for this file.
		 */
		if (error == ENOENT) {
			goto again;
		}

		/* Make sure it was file. */
		if ((error == 0) && !vnode_isreg(*args.a_vpp)) {
			vnode_put(*args.a_vpp);
			error = EEXIST;
		}
		args.a_cnp->cn_nameiop = CREATE;
	}
	return (error);
}

/*
 * Make device special file.
 */
static int
hfs_vnop_mknod(struct vnop_mknod_args *ap)
{
	struct vnode_attr *vap = ap->a_vap;
	struct vnode *dvp = ap->a_dvp;
	struct vnode **vpp = ap->a_vpp;
	struct cnode *cp;
	int error;

	if (VTOVCB(dvp)->vcbSigWord != kHFSPlusSigWord) {
		return (ENOTSUP);
	}

	/* Create the vnode */
	error = hfs_makenode(dvp, vpp, ap->a_cnp, vap, ap->a_context);
	if (error)
		return (error);

	cp = VTOC(*vpp);
	cp->c_touch_acctime = TRUE;
	cp->c_touch_chgtime = TRUE;
	cp->c_touch_modtime = TRUE;

	if ((vap->va_rdev != VNOVAL) &&
	    (vap->va_type == VBLK || vap->va_type == VCHR))
		cp->c_rdev = vap->va_rdev;

	return (0);
}

#if HFS_COMPRESSION
/* 
 *	hfs_ref_data_vp(): returns the data fork vnode for a given cnode. 
 *	In the (hopefully rare) case where the data fork vnode is not 
 *	present, it will use hfs_vget() to create a new vnode for the
 *	data fork. 
 *	
 *	NOTE: If successful and a vnode is returned, the caller is responsible
 *	for releasing the returned vnode with vnode_rele().
 */
static int
hfs_ref_data_vp(struct cnode *cp, struct vnode **data_vp, int skiplock)
{
	int vref = 0;

	if (!data_vp || !cp) /* sanity check incoming parameters */
		return EINVAL;
	
	/* maybe we should take the hfs cnode lock here, and if so, use the skiplock parameter to tell us not to */

	if (!skiplock) hfs_lock(cp, HFS_SHARED_LOCK);
	struct vnode *c_vp = cp->c_vp;
	if (c_vp) {
		/* we already have a data vnode */
		*data_vp = c_vp;
		vref = vnode_ref(*data_vp);
		if (!skiplock) hfs_unlock(cp);
		if (vref == 0) {
			return 0;
		}
		return EINVAL;
	}
	/* no data fork vnode in the cnode, so ask hfs for one. */

	if (!cp->c_rsrc_vp) {
		/* if we don't have either a c_vp or c_rsrc_vp, we can't really do anything useful */
		*data_vp = NULL;
		if (!skiplock) hfs_unlock(cp);
		return EINVAL;
	}
	
	if (0 == hfs_vget(VTOHFS(cp->c_rsrc_vp), cp->c_cnid, data_vp, 1) &&
		0 != data_vp) {
		vref = vnode_ref(*data_vp);
		vnode_put(*data_vp);
		if (!skiplock) hfs_unlock(cp);
		if (vref == 0) {
			return 0;
		}
		return EINVAL;
	}
	/* there was an error getting the vnode */
	*data_vp = NULL;
	if (!skiplock) hfs_unlock(cp);
	return EINVAL;
}

/*
 *	hfs_lazy_init_decmpfs_cnode(): returns the decmpfs_cnode for a cnode,
 *	allocating it if necessary; returns NULL if there was an allocation error
 */
static decmpfs_cnode *
hfs_lazy_init_decmpfs_cnode(struct cnode *cp)
{
	if (!cp->c_decmp) {
		decmpfs_cnode *dp = NULL;
		MALLOC_ZONE(dp, decmpfs_cnode *, sizeof(decmpfs_cnode), M_DECMPFS_CNODE, M_WAITOK);
		if (!dp) {
			/* error allocating a decmpfs cnode */
			return NULL;
		}
		decmpfs_cnode_init(dp);
		if (!OSCompareAndSwapPtr(NULL, dp, (void * volatile *)&cp->c_decmp)) {
			/* another thread got here first, so free the decmpfs_cnode we allocated */
			decmpfs_cnode_destroy(dp);
			FREE_ZONE(dp, sizeof(*dp), M_DECMPFS_CNODE);
		}
	}
	
	return cp->c_decmp;
}

/*
 *	hfs_file_is_compressed(): returns 1 if the file is compressed, and 0 (zero) if not.
 *	if the file's compressed flag is set, makes sure that the decmpfs_cnode field
 *	is allocated by calling hfs_lazy_init_decmpfs_cnode(), then makes sure it is populated,
 *	or else fills it in via the decmpfs_file_is_compressed() function.
 */
int
hfs_file_is_compressed(struct cnode *cp, int skiplock)
{
	int ret = 0;
	
	/* fast check to see if file is compressed. If flag is clear, just answer no */
	if (!(cp->c_flags & UF_COMPRESSED)) {
		return 0;
	}

	decmpfs_cnode *dp = hfs_lazy_init_decmpfs_cnode(cp);
	if (!dp) {
		/* error allocating a decmpfs cnode, treat the file as uncompressed */
		return 0;
	}
	
	/* flag was set, see if the decmpfs_cnode state is valid (zero == invalid) */
	uint32_t decmpfs_state = decmpfs_cnode_get_vnode_state(dp);
	switch(decmpfs_state) {
		case FILE_IS_COMPRESSED:
		case FILE_IS_CONVERTING: /* treat decompressing files as if they are compressed */
			return 1;
		case FILE_IS_NOT_COMPRESSED:
			return 0;
		/* otherwise the state is not cached yet */
	}
	
	/* decmpfs hasn't seen this file yet, so call decmpfs_file_is_compressed() to init the decmpfs_cnode struct */
	struct vnode *data_vp = NULL;
	if (0 == hfs_ref_data_vp(cp, &data_vp, skiplock)) {
		if (data_vp) {
			ret = decmpfs_file_is_compressed(data_vp, VTOCMP(data_vp)); // fill in decmpfs_cnode
			vnode_rele(data_vp);
		}
	}
	return ret;
}

/*	hfs_uncompressed_size_of_compressed_file() - get the uncompressed size of the file.
 *	if the caller has passed a valid vnode (has a ref count > 0), then hfsmp and fid are not required.
 *	if the caller doesn't have a vnode, pass NULL in vp, and pass valid hfsmp and fid.
 *	files size is returned in size (required)
 */
int
hfs_uncompressed_size_of_compressed_file(struct hfsmount *hfsmp, struct vnode *vp, cnid_t fid, off_t *size, int skiplock)
{
	int ret = 0;
	int putaway = 0;									/* flag to remember if we used hfs_vget() */

	if (!size) {
		return EINVAL;									/* no place to put the file size */
	}

	if (NULL == vp) {
		if (!hfsmp || !fid) {							/* make sure we have the required parameters */
			return EINVAL;
		}
		if (0 != hfs_vget(hfsmp, fid, &vp, skiplock)) {		/* vnode is null, use hfs_vget() to get it */
			vp = NULL;
		} else {
			putaway = 1;								/* note that hfs_vget() was used to aquire the vnode */
		}
	}
	/* this double check for compression (hfs_file_is_compressed)
	 * ensures the cached size is present in case decmpfs hasn't 
	 * encountered this node yet.
	 */
	if ( ( NULL != vp ) && hfs_file_is_compressed(VTOC(vp), skiplock) ) {
		*size = decmpfs_cnode_get_vnode_cached_size(VTOCMP(vp));	/* file info will be cached now, so get size */
	} else {
		ret = EINVAL;
	}
	
	if (putaway) {		/* did we use hfs_vget() to get this vnode? */
		vnode_put(vp);	/* if so, release it and set it to null */
		vp = NULL;
	}
	return ret;
}

int
hfs_hides_rsrc(vfs_context_t ctx, struct cnode *cp, int skiplock)
{
	if (ctx == decmpfs_ctx)
		return 0;
	if (!hfs_file_is_compressed(cp, skiplock))
		return 0;
	return decmpfs_hides_rsrc(ctx, cp->c_decmp);
}

int
hfs_hides_xattr(vfs_context_t ctx, struct cnode *cp, const char *name, int skiplock)
{
	if (ctx == decmpfs_ctx)
		return 0;
	if (!hfs_file_is_compressed(cp, skiplock))
		return 0;
	return decmpfs_hides_xattr(ctx, cp->c_decmp, name);
}
#endif /* HFS_COMPRESSION */
		
/*
 * Open a file/directory.
 */
static int
hfs_vnop_open(struct vnop_open_args *ap)
{
	struct vnode *vp = ap->a_vp;
	struct filefork *fp;
	struct timeval tv;
	int error;
	static int past_bootup = 0;
	struct cnode *cp = VTOC(vp);
	struct hfsmount *hfsmp = VTOHFS(vp);
	
#if HFS_COMPRESSION
	if (ap->a_mode & FWRITE) {
		/* open for write */
		if ( hfs_file_is_compressed(cp, 1) ) { /* 1 == don't take the cnode lock */
			/* opening a compressed file for write, so convert it to decompressed */
			struct vnode *data_vp = NULL;
			error = hfs_ref_data_vp(cp, &data_vp, 1); /* 1 == don't take the cnode lock */
			if (0 == error) {
				if (data_vp) {
					error = decmpfs_decompress_file(data_vp, VTOCMP(data_vp), -1, 1, 0);
					vnode_rele(data_vp);
				} else {
					error = EINVAL;
				}
			}
			if (error != 0)
				return error;
		}
	} else {
		/* open for read */
		if (hfs_file_is_compressed(cp, 1) ) { /* 1 == don't take the cnode lock */
			if (VNODE_IS_RSRC(vp)) {
				/* opening the resource fork of a compressed file, so nothing to do */
			} else {
				/* opening a compressed file for read, make sure it validates */
				error = decmpfs_validate_compressed_file(vp, VTOCMP(vp));
				if (error != 0)
					return error;
			}
		}
	}
#endif

	/*
	 * Files marked append-only must be opened for appending.
	 */
	if ((cp->c_flags & APPEND) && !vnode_isdir(vp) &&
	    (ap->a_mode & (FWRITE | O_APPEND)) == FWRITE)
		return (EPERM);

	if (vnode_isreg(vp) && !UBCINFOEXISTS(vp))
		return (EBUSY);  /* file is in use by the kernel */

	/* Don't allow journal file to be opened externally. */
	if (cp->c_fileid == hfsmp->hfs_jnlfileid)
		return (EPERM);

	/* If we're going to write to the file, initialize quotas. */
#if QUOTA
	if ((ap->a_mode & FWRITE) && (hfsmp->hfs_flags & HFS_QUOTAS))
		(void)hfs_getinoquota(cp);
#endif /* QUOTA */

	/*
	 * On the first (non-busy) open of a fragmented
	 * file attempt to de-frag it (if its less than 20MB).
	 */
	if ((hfsmp->hfs_flags & HFS_READ_ONLY) ||
	    (hfsmp->jnl == NULL) ||
#if NAMEDSTREAMS
	    !vnode_isreg(vp) || vnode_isinuse(vp, 0) || vnode_isnamedstream(vp)) {
#else
	    !vnode_isreg(vp) || vnode_isinuse(vp, 0)) {
#endif
		return (0);
	}

	if ((error = hfs_lock(cp, HFS_EXCLUSIVE_LOCK)))
		return (error);
	fp = VTOF(vp);
	if (fp->ff_blocks &&
	    fp->ff_extents[7].blockCount != 0 &&
	    fp->ff_size <= (20 * 1024 * 1024)) {
		int no_mods = 0;
		struct timeval now;
		/* 
		 * Wait until system bootup is done (3 min).
		 * And don't relocate a file that's been modified
		 * within the past minute -- this can lead to
		 * system thrashing.
		 */

		if (!past_bootup) {
			microuptime(&tv);
			if (tv.tv_sec > (60*3)) {
				past_bootup = 1;
			}
		}
		
		microtime(&now);
		if ((now.tv_sec - cp->c_mtime) > 60) {	
			no_mods = 1;
		} 
		
		if (past_bootup && no_mods) {
			(void) hfs_relocate(vp, hfsmp->nextAllocation + 4096,
					vfs_context_ucred(ap->a_context),
					vfs_context_proc(ap->a_context));
		}
	}
	hfs_unlock(cp);

	return (0);
}


/*
 * Close a file/directory.
 */
static int
hfs_vnop_close(ap)
	struct vnop_close_args /* {
		struct vnode *a_vp;
		int a_fflag;
		vfs_context_t a_context;
	} */ *ap;
{
	register struct vnode *vp = ap->a_vp;
 	register struct cnode *cp;
	struct proc *p = vfs_context_proc(ap->a_context);
	struct hfsmount *hfsmp;
	int busy;
	int tooktrunclock = 0;
	int knownrefs = 0;

	if ( hfs_lock(VTOC(vp), HFS_EXCLUSIVE_LOCK) != 0)
		return (0);
	cp = VTOC(vp);
	hfsmp = VTOHFS(vp);

	/* 
	 * If the rsrc fork is a named stream, it can cause the data fork to
	 * stay around, preventing de-allocation of these blocks. 
	 * Do checks for truncation on close. Purge extra extents if they exist.
	 * Make sure the vp is not a directory, and that it has a resource fork,
	 * and that resource fork is also a named stream.
	 */

	if ((vp->v_type == VREG) && (cp->c_rsrc_vp)
			&& (vnode_isnamedstream(cp->c_rsrc_vp))) {
		uint32_t blks;

		blks = howmany(VTOF(vp)->ff_size, VTOVCB(vp)->blockSize);
		/*
		 * If there are extra blocks and there are only 2 refs on
		 * this vp (ourselves + rsrc fork holding ref on us), go ahead
		 * and try to truncate.
		 */
		if ((blks < VTOF(vp)->ff_blocks) && (!vnode_isinuse(vp, 2))) {
			// release cnode lock; must acquire truncate lock BEFORE cnode lock
			hfs_unlock(cp);

			hfs_lock_truncate(cp, TRUE);
			tooktrunclock = 1;

			if (hfs_lock(VTOC(vp), HFS_EXCLUSIVE_LOCK) != 0) { 
				hfs_unlock_truncate(cp, TRUE);
				// bail out if we can't re-acquire cnode lock
				return 0;
			}
			// now re-test to make sure it's still valid
			if (cp->c_rsrc_vp) {
				knownrefs = 1 + vnode_isnamedstream(cp->c_rsrc_vp);
				if (!vnode_isinuse(vp, knownrefs)){
					// now we can truncate the file, if necessary
					blks = howmany(VTOF(vp)->ff_size, VTOVCB(vp)->blockSize);
					if (blks < VTOF(vp)->ff_blocks){
						(void) hfs_truncate(vp, VTOF(vp)->ff_size, IO_NDELAY, 0, 0, ap->a_context);
					}
				}
			}
		}
	}


	// if we froze the fs and we're exiting, then "thaw" the fs 
	if (hfsmp->hfs_freezing_proc == p && proc_exiting(p)) {
	    hfsmp->hfs_freezing_proc = NULL;
	    hfs_global_exclusive_lock_release(hfsmp);
	    lck_rw_unlock_exclusive(&hfsmp->hfs_insync);
	}

	busy = vnode_isinuse(vp, 1);

	if (busy) {
		hfs_touchtimes(VTOHFS(vp), cp);	
	}
	if (vnode_isdir(vp)) {
		hfs_reldirhints(cp, busy);
	} else if (vnode_issystem(vp) && !busy) {
		vnode_recycle(vp);
	}

	if (tooktrunclock){
		hfs_unlock_truncate(cp, TRUE);
	}
	hfs_unlock(cp);

	if (ap->a_fflag & FWASWRITTEN) {
		hfs_sync_ejectable(hfsmp);
	}

	return (0);
}

/*
 * Get basic attributes.
 */
static int
hfs_vnop_getattr(struct vnop_getattr_args *ap)
{
#define VNODE_ATTR_TIMES  \
	(VNODE_ATTR_va_access_time|VNODE_ATTR_va_change_time|VNODE_ATTR_va_modify_time)
#define VNODE_ATTR_AUTH  \
	(VNODE_ATTR_va_mode | VNODE_ATTR_va_uid | VNODE_ATTR_va_gid | \
         VNODE_ATTR_va_flags | VNODE_ATTR_va_acl)

	struct vnode *vp = ap->a_vp;
	struct vnode_attr *vap = ap->a_vap;
	struct vnode *rvp = NULLVP;
	struct hfsmount *hfsmp;
	struct cnode *cp;
	uint64_t data_size;
	enum vtype v_type;
	int error = 0;
	cp = VTOC(vp);

#if HFS_COMPRESSION
	/* we need to inspect the decmpfs state of the file before we take the hfs cnode lock */
	int compressed = 0;
	int hide_size = 0;
	off_t uncompressed_size = -1;
	if (VATTR_IS_ACTIVE(vap, va_data_size) || VATTR_IS_ACTIVE(vap, va_total_alloc) || VATTR_IS_ACTIVE(vap, va_data_alloc) || VATTR_IS_ACTIVE(vap, va_total_size)) {
		/* we only care about whether the file is compressed if asked for the uncompressed size */
		if (VNODE_IS_RSRC(vp)) {
			/* if it's a resource fork, decmpfs may want us to hide the size */
			hide_size = hfs_hides_rsrc(ap->a_context, cp, 0);
		} else {
			/* if it's a data fork, we need to know if it was compressed so we can report the uncompressed size */
			compressed = hfs_file_is_compressed(cp, 0);
		}
		if (compressed && (VATTR_IS_ACTIVE(vap, va_data_size) || VATTR_IS_ACTIVE(vap, va_total_size))) {
			if (0 != hfs_uncompressed_size_of_compressed_file(NULL, vp, 0, &uncompressed_size, 0)) {
				/* failed to get the uncompressed size, we'll check for this later */
				uncompressed_size = -1;
			}
		}
	}
#endif

	/*
	 * Shortcut for vnode_authorize path.  Each of the attributes
	 * in this set is updated atomically so we don't need to take
	 * the cnode lock to access them.
	 */
	if ((vap->va_active & ~VNODE_ATTR_AUTH) == 0) {
		/* Make sure file still exists. */
		if (cp->c_flag & C_NOEXISTS)
			return (ENOENT);

		vap->va_uid = cp->c_uid;
		vap->va_gid = cp->c_gid;
		vap->va_mode = cp->c_mode;
		vap->va_flags = cp->c_flags;
		vap->va_supported |= VNODE_ATTR_AUTH & ~VNODE_ATTR_va_acl;

		if ((cp->c_attr.ca_recflags & kHFSHasSecurityMask) == 0) {
			vap->va_acl = (kauth_acl_t) KAUTH_FILESEC_NONE;
			VATTR_SET_SUPPORTED(vap, va_acl);
		}
	
		return (0);
	}

	hfsmp = VTOHFS(vp);
	v_type = vnode_vtype(vp);
	/*
	 * If time attributes are requested and we have cnode times
	 * that require updating, then acquire an exclusive lock on
	 * the cnode before updating the times.  Otherwise we can
	 * just acquire a shared lock.
	 */
	if ((vap->va_active & VNODE_ATTR_TIMES) &&
	    (cp->c_touch_acctime || cp->c_touch_chgtime || cp->c_touch_modtime)) {
		if ((error = hfs_lock(cp, HFS_EXCLUSIVE_LOCK)))
			return (error);
		hfs_touchtimes(hfsmp, cp);
	}
 	else {
		if ((error = hfs_lock(cp, HFS_SHARED_LOCK)))
			return (error);
	}

	if (v_type == VDIR) {
		data_size = (cp->c_entries + 2) * AVERAGE_HFSDIRENTRY_SIZE;

		if (VATTR_IS_ACTIVE(vap, va_nlink)) {
			int nlink;
	
			/*
			 * For directories, the va_nlink is esentially a count
			 * of the ".." references to a directory plus the "."
			 * reference and the directory itself. So for HFS+ this
			 * becomes the sub-directory count plus two.
			 *
			 * In the absence of a sub-directory count we use the
			 * directory's item count.  This will be too high in
			 * most cases since it also includes files.
			 */
			if ((hfsmp->hfs_flags & HFS_FOLDERCOUNT) && 
			    (cp->c_attr.ca_recflags & kHFSHasFolderCountMask))
				nlink = cp->c_attr.ca_dircount;  /* implied ".." entries */
			else
				nlink = cp->c_entries;

			/* Account for ourself and our "." entry */
			nlink += 2;  
			 /* Hide our private directories. */
			if (cp->c_cnid == kHFSRootFolderID) {
				if (hfsmp->hfs_private_desc[FILE_HARDLINKS].cd_cnid != 0) {
					--nlink;    
				}
				if (hfsmp->hfs_private_desc[DIR_HARDLINKS].cd_cnid != 0) {
					--nlink;
				}
			}
			VATTR_RETURN(vap, va_nlink, (u_int64_t)nlink);
		}		
		if (VATTR_IS_ACTIVE(vap, va_nchildren)) {
			int entries;
	
			entries = cp->c_entries;
			/* Hide our private files and directories. */
			if (cp->c_cnid == kHFSRootFolderID) {
				if (hfsmp->hfs_private_desc[FILE_HARDLINKS].cd_cnid != 0)
					--entries;
				if (hfsmp->hfs_private_desc[DIR_HARDLINKS].cd_cnid != 0)
					--entries;
				if (hfsmp->jnl || ((hfsmp->vcbAtrb & kHFSVolumeJournaledMask) && (hfsmp->hfs_flags & HFS_READ_ONLY)))
					entries -= 2;   /* hide the journal files */
			}
			VATTR_RETURN(vap, va_nchildren, entries);
		}
		/*
		 * The va_dirlinkcount is the count of real directory hard links.
		 * (i.e. its not the sum of the implied "." and ".." references)
		 */
		if (VATTR_IS_ACTIVE(vap, va_dirlinkcount)) {
			VATTR_RETURN(vap, va_dirlinkcount, (uint32_t)cp->c_linkcount);
		}
	} else /* !VDIR */ {
		data_size = VCTOF(vp, cp)->ff_size;

		VATTR_RETURN(vap, va_nlink, (u_int64_t)cp->c_linkcount);
		if (VATTR_IS_ACTIVE(vap, va_data_alloc)) {
			u_int64_t blocks;
	
#if HFS_COMPRESSION
			if (hide_size) {
				VATTR_RETURN(vap, va_data_alloc, 0);
			} else if (compressed) {
				/* for compressed files, we report all allocated blocks as belonging to the data fork */
				blocks = cp->c_blocks;
				VATTR_RETURN(vap, va_data_alloc, blocks * (u_int64_t)hfsmp->blockSize);
			}
			else
#endif
			{
				blocks = VCTOF(vp, cp)->ff_blocks;
				VATTR_RETURN(vap, va_data_alloc, blocks * (u_int64_t)hfsmp->blockSize);
			}
		}
	}

	/* conditional because 64-bit arithmetic can be expensive */
	if (VATTR_IS_ACTIVE(vap, va_total_size)) {
		if (v_type == VDIR) {
			VATTR_RETURN(vap, va_total_size, (cp->c_entries + 2) * AVERAGE_HFSDIRENTRY_SIZE);
		} else {
			u_int64_t total_size = ~0ULL;
			struct cnode *rcp;
#if HFS_COMPRESSION
			if (hide_size) {
				/* we're hiding the size of this file, so just return 0 */
				total_size = 0;
			} else if (compressed) {
				if (uncompressed_size == -1) {
					/*
					 * We failed to get the uncompressed size above,
					 * so we'll fall back to the standard path below
					 * since total_size is still -1
					 */
				} else {
					/* use the uncompressed size we fetched above */
					total_size = uncompressed_size;
				}
			}
#endif
			if (total_size == ~0ULL) {
				if (cp->c_datafork) {
					total_size = cp->c_datafork->ff_size;
				}
				
				if (cp->c_blocks - VTOF(vp)->ff_blocks) {
					/* We deal with rsrc fork vnode iocount at the end of the function */
					error = hfs_vgetrsrc(hfsmp, vp, &rvp, TRUE, TRUE);
					if (error) {
						/* 
						 * hfs_vgetrsrc may have returned a vnode in rvp even though
						 * we got an error, because we specified error_on_unlinked.
						 * We need to drop the iocount after we release the cnode lock, so
						 * it will be taken care of at the end of the function if it's needed.
						 */
						goto out;
					}
					
					rcp = VTOC(rvp);
					if (rcp && rcp->c_rsrcfork) {
						total_size += rcp->c_rsrcfork->ff_size;
					}
				}
			}
			
			VATTR_RETURN(vap, va_total_size, total_size);
		}
	}
	if (VATTR_IS_ACTIVE(vap, va_total_alloc)) {
		if (v_type == VDIR) {
			VATTR_RETURN(vap, va_total_alloc, 0);
		} else {
			VATTR_RETURN(vap, va_total_alloc, (u_int64_t)cp->c_blocks * (u_int64_t)hfsmp->blockSize);
		}
	}

	/*
	 * If the VFS wants extended security data, and we know that we
	 * don't have any (because it never told us it was setting any)
	 * then we can return the supported bit and no data.  If we do
	 * have extended security, we can just leave the bit alone and
	 * the VFS will use the fallback path to fetch it.
	 */
	if (VATTR_IS_ACTIVE(vap, va_acl)) {
		if ((cp->c_attr.ca_recflags & kHFSHasSecurityMask) == 0) {
			vap->va_acl = (kauth_acl_t) KAUTH_FILESEC_NONE;
			VATTR_SET_SUPPORTED(vap, va_acl);
		}
	}
	if (VATTR_IS_ACTIVE(vap, va_access_time)) {
		/* Access times are lazily updated, get current time if needed */
		if (cp->c_touch_acctime) {
			struct timeval tv;
	
			microtime(&tv);
			vap->va_access_time.tv_sec = tv.tv_sec;
		} else {
			vap->va_access_time.tv_sec = cp->c_atime;
		}
		vap->va_access_time.tv_nsec = 0;
		VATTR_SET_SUPPORTED(vap, va_access_time);
	}
	vap->va_create_time.tv_sec = cp->c_itime;
	vap->va_create_time.tv_nsec = 0;
	vap->va_modify_time.tv_sec = cp->c_mtime;
	vap->va_modify_time.tv_nsec = 0;
	vap->va_change_time.tv_sec = cp->c_ctime;
	vap->va_change_time.tv_nsec = 0;
	vap->va_backup_time.tv_sec = cp->c_btime;
	vap->va_backup_time.tv_nsec = 0;	

	/* XXX is this really a good 'optimal I/O size'? */
	vap->va_iosize = hfsmp->hfs_logBlockSize;
	vap->va_uid = cp->c_uid;
	vap->va_gid = cp->c_gid;
	vap->va_mode = cp->c_mode;
	vap->va_flags = cp->c_flags;

	/*
	 * Exporting file IDs from HFS Plus:
	 *
	 * For "normal" files the c_fileid is the same value as the
	 * c_cnid.  But for hard link files, they are different - the
	 * c_cnid belongs to the active directory entry (ie the link)
	 * and the c_fileid is for the actual inode (ie the data file).
	 *
	 * The stat call (getattr) uses va_fileid and the Carbon APIs,
	 * which are hardlink-ignorant, will ask for va_linkid.
	 */
	vap->va_fileid = (u_int64_t)cp->c_fileid;
	/* 
	 * We need to use the origin cache for both hardlinked files 
	 * and directories. Hardlinked directories have multiple cnids 
	 * and parents (one per link). Hardlinked files also have their 
	 * own parents and link IDs separate from the indirect inode number. 
	 * If we don't use the cache, we could end up vending the wrong ID 
	 * because the cnode will only reflect the link that was looked up most recently.
	 */
	if (cp->c_flag & C_HARDLINK) {
		vap->va_linkid = (u_int64_t)hfs_currentcnid(cp);
		vap->va_parentid = (u_int64_t)hfs_currentparent(cp);
	} else {
		vap->va_linkid = (u_int64_t)cp->c_cnid;
		vap->va_parentid = (u_int64_t)cp->c_parentcnid;
	}
	vap->va_fsid = hfsmp->hfs_raw_dev;
	vap->va_filerev = 0;
	vap->va_encoding = cp->c_encoding;
	vap->va_rdev = (v_type == VBLK || v_type == VCHR) ? cp->c_rdev : 0;
#if HFS_COMPRESSION
	if (VATTR_IS_ACTIVE(vap, va_data_size)) {
		if (hide_size)
			vap->va_data_size = 0;
		else if (compressed) {
			if (uncompressed_size == -1) {
				/* failed to get the uncompressed size above, so just return data_size */
				vap->va_data_size = data_size;
			} else {
				/* use the uncompressed size we fetched above */
				vap->va_data_size = uncompressed_size;
			}
		} else
			vap->va_data_size = data_size;
//		vap->va_supported |= VNODE_ATTR_va_data_size;
		VATTR_SET_SUPPORTED(vap, va_data_size);
	}
#else
	vap->va_data_size = data_size;
	vap->va_supported |= VNODE_ATTR_va_data_size;
#endif
    
	/* Mark them all at once instead of individual VATTR_SET_SUPPORTED calls. */
	vap->va_supported |= VNODE_ATTR_va_create_time | VNODE_ATTR_va_modify_time |
	                     VNODE_ATTR_va_change_time| VNODE_ATTR_va_backup_time |
	                     VNODE_ATTR_va_iosize | VNODE_ATTR_va_uid |
	                     VNODE_ATTR_va_gid | VNODE_ATTR_va_mode |
	                     VNODE_ATTR_va_flags |VNODE_ATTR_va_fileid |
	                     VNODE_ATTR_va_linkid | VNODE_ATTR_va_parentid |
	                     VNODE_ATTR_va_fsid | VNODE_ATTR_va_filerev |
	                     VNODE_ATTR_va_encoding | VNODE_ATTR_va_rdev;

	/* If this is the root, let VFS to find out the mount name, which 
	 * may be different from the real name.  Otherwise, we need to take care
	 * for hardlinked files, which need to be looked up, if necessary 
	 */
	if (VATTR_IS_ACTIVE(vap, va_name) && (cp->c_cnid != kHFSRootFolderID)) {
		struct cat_desc linkdesc;
		int lockflags;
		int uselinkdesc = 0;
		cnid_t nextlinkid = 0;
		cnid_t prevlinkid = 0;

		/* Get the name for ATTR_CMN_NAME.  We need to take special care for hardlinks      
		 * here because the info. for the link ID requested by getattrlist may be
		 * different than what's currently in the cnode.  This is because the cnode     
		 * will be filled in with the information for the most recent link ID that went
		 * through namei/lookup().  If there are competing lookups for hardlinks that point 
	 	 * to the same inode, one (or more) getattrlists could be vended incorrect name information.
		 * Also, we need to beware of open-unlinked files which could have a namelen of 0.     
		 */

		if ((cp->c_flag & C_HARDLINK) && 
				((cp->c_desc.cd_namelen == 0) || (vap->va_linkid != cp->c_cnid))) {
			/* If we have no name and our link ID is the raw inode number, then we may
			 * have an open-unlinked file.  Go to the next link in this case.
			 */
			if ((cp->c_desc.cd_namelen == 0) && (vap->va_linkid == cp->c_fileid)) {
				if ((error = hfs_lookuplink(hfsmp, vap->va_linkid, &prevlinkid, &nextlinkid))){
					goto out;
				}
			}	
			else {
				/* just use link obtained from vap above */
				nextlinkid = vap->va_linkid;
			}

			/* We need to probe the catalog for the descriptor corresponding to the link ID
			 * stored in nextlinkid.  Note that we don't know if we have the exclusive lock
			 * for the cnode here, so we can't just update the descriptor.  Instead,
			 * we should just store the descriptor's value locally and then use it to pass
			 * out the name value as needed below. 
			 */ 
			if (nextlinkid){
				lockflags = hfs_systemfile_lock(hfsmp, SFL_CATALOG, HFS_SHARED_LOCK);
				error = cat_findname(hfsmp, nextlinkid, &linkdesc);
				hfs_systemfile_unlock(hfsmp, lockflags);	
				if (error == 0) {
					uselinkdesc = 1;
				}
			}
		}

		/* By this point, we've either patched up the name above and the c_desc
		 * points to the correct data, or it already did, in which case we just proceed
		 * by copying the name into the vap.  Note that we will never set va_name to
		 * supported if nextlinkid is never initialized.  This could happen in the degenerate
		 * case above involving the raw inode number, where it has no nextlinkid.  In this case
		 * we will simply not mark the name bit as supported.
		 */
		if (uselinkdesc) {
			strlcpy(vap->va_name, (const char*) linkdesc.cd_nameptr, MAXPATHLEN);
			VATTR_SET_SUPPORTED(vap, va_name);
			cat_releasedesc(&linkdesc);
		}	
		else if (cp->c_desc.cd_namelen) {
			strlcpy(vap->va_name, (const char*) cp->c_desc.cd_nameptr, MAXPATHLEN);
			VATTR_SET_SUPPORTED(vap, va_name);
		}
	}

out:
	hfs_unlock(cp);
	/*
	 * We need to vnode_put the rsrc fork vnode only *after* we've released
	 * the cnode lock, since vnode_put can trigger an inactive call, which 
	 * will go back into HFS and try to acquire a cnode lock.
	 */
	if (rvp) {
		vnode_put (rvp);
	}

	return (error);
}

static int
hfs_vnop_setattr(ap)
	struct vnop_setattr_args /* {
		struct vnode *a_vp;
		struct vnode_attr *a_vap;
		vfs_context_t a_context;
	} */ *ap;
{
	struct vnode_attr *vap = ap->a_vap;
	struct vnode *vp = ap->a_vp;
	struct cnode *cp = NULL;
	struct hfsmount *hfsmp;
	kauth_cred_t cred = vfs_context_ucred(ap->a_context);
	struct proc *p = vfs_context_proc(ap->a_context);
	int error = 0;
	uid_t nuid;
	gid_t ngid;

#if HFS_COMPRESSION
	int decmpfs_reset_state = 0;
	/*
	 we call decmpfs_update_attributes even if the file is not compressed
	 because we want to update the incoming flags if the xattrs are invalid
	 */
	error = decmpfs_update_attributes(vp, vap);
	if (error)
		return error;
#endif

	hfsmp = VTOHFS(vp);

	/* Don't allow modification of the journal file. */
	if (hfsmp->hfs_jnlfileid == VTOC(vp)->c_fileid) {
		return (EPERM);
	}

	/*
	 * File size change request.
	 * We are guaranteed that this is not a directory, and that
	 * the filesystem object is writeable.
	 *
	 * NOTE: HFS COMPRESSION depends on the data_size being set *before* the bsd flags are updated
	 */
	VATTR_SET_SUPPORTED(vap, va_data_size);
	if (VATTR_IS_ACTIVE(vap, va_data_size) && !vnode_islnk(vp)) {
#if HFS_COMPRESSION
		/* keep the compressed state locked until we're done truncating the file */
		decmpfs_cnode *dp = VTOCMP(vp);
		if (!dp) {
			/*
			 * call hfs_lazy_init_decmpfs_cnode() to make sure that the decmpfs_cnode
			 * is filled in; we need a decmpfs_cnode to lock out decmpfs state changes
			 * on this file while it's truncating
			 */
			dp = hfs_lazy_init_decmpfs_cnode(VTOC(vp));
			if (!dp) {
				/* failed to allocate a decmpfs_cnode */
				return ENOMEM; /* what should this be? */
			}
		}
		
		decmpfs_lock_compressed_data(dp, 1);
		if (hfs_file_is_compressed(VTOC(vp), 1)) {
			error = decmpfs_decompress_file(vp, dp, -1/*vap->va_data_size*/, 0, 1);
			if (error != 0) {
				decmpfs_unlock_compressed_data(dp, 1);
				return error;
			}
		}
#endif

		/* Take truncate lock before taking cnode lock. */
		hfs_lock_truncate(VTOC(vp), TRUE);
		
		/* Perform the ubc_setsize before taking the cnode lock. */
		ubc_setsize(vp, vap->va_data_size);

		if ((error = hfs_lock(VTOC(vp), HFS_EXCLUSIVE_LOCK))) {
			hfs_unlock_truncate(VTOC(vp), TRUE);
#if HFS_COMPRESSION
			decmpfs_unlock_compressed_data(dp, 1);
#endif
			return (error);
		}
		cp = VTOC(vp);

		error = hfs_truncate(vp, vap->va_data_size, vap->va_vaflags & 0xffff, 1, 0, ap->a_context);

		hfs_unlock_truncate(cp, TRUE);
#if HFS_COMPRESSION
		decmpfs_unlock_compressed_data(dp, 1);
#endif
		if (error)
			goto out;
	}
	if (cp == NULL) {
		if ((error = hfs_lock(VTOC(vp), HFS_EXCLUSIVE_LOCK)))
			return (error);
		cp = VTOC(vp);
	}

	/*
	 * If it is just an access time update request by itself
	 * we know the request is from kernel level code, and we
	 * can delay it without being as worried about consistency.
	 * This change speeds up mmaps, in the rare case that they
	 * get caught behind a sync.
	 */

	if (vap->va_active == VNODE_ATTR_va_access_time) {
		cp->c_touch_acctime=TRUE;
		goto out;
	}



	/*
	 * Owner/group change request.
	 * We are guaranteed that the new owner/group is valid and legal.
	 */
	VATTR_SET_SUPPORTED(vap, va_uid);
	VATTR_SET_SUPPORTED(vap, va_gid);
	nuid = VATTR_IS_ACTIVE(vap, va_uid) ? vap->va_uid : (uid_t)VNOVAL;
	ngid = VATTR_IS_ACTIVE(vap, va_gid) ? vap->va_gid : (gid_t)VNOVAL;
	if (((nuid != (uid_t)VNOVAL) || (ngid != (gid_t)VNOVAL)) &&
	    ((error = hfs_chown(vp, nuid, ngid, cred, p)) != 0))
		goto out;

	/*
	 * Mode change request.
	 * We are guaranteed that the mode value is valid and that in
	 * conjunction with the owner and group, this change is legal.
   	*/
	VATTR_SET_SUPPORTED(vap, va_mode);
	if (VATTR_IS_ACTIVE(vap, va_mode) &&
	    ((error = hfs_chmod(vp, (int)vap->va_mode, cred, p)) != 0))
	    goto out;

	/*
	 * File flags change.
	 * We are guaranteed that only flags allowed to change given the
	 * current securelevel are being changed.
	 */
	VATTR_SET_SUPPORTED(vap, va_flags);
	if (VATTR_IS_ACTIVE(vap, va_flags)) {
		u_int16_t *fdFlags;

#if HFS_COMPRESSION
		if ((cp->c_flags ^ vap->va_flags) & UF_COMPRESSED) {
			/*
			 * the UF_COMPRESSED was toggled, so reset our cached compressed state
			 * but we don't want to actually do the update until we've released the cnode lock down below
			 * NOTE: turning the flag off doesn't actually decompress the file, so that we can
			 * turn off the flag and look at the "raw" file for debugging purposes
			 */
			decmpfs_reset_state = 1;
		}
#endif

		cp->c_flags = vap->va_flags;
		cp->c_touch_chgtime = TRUE;
		
		/*
		 * Mirror the UF_HIDDEN flag to the invisible bit of the Finder Info.
		 *
		 * The fdFlags for files and frFlags for folders are both 8 bytes
		 * into the userInfo (the first 16 bytes of the Finder Info).  They
		 * are both 16-bit fields.
		 */
		fdFlags = (u_int16_t *) &cp->c_finderinfo[8];
		if (vap->va_flags & UF_HIDDEN)
			*fdFlags |= OSSwapHostToBigConstInt16(kFinderInvisibleMask);
		else
			*fdFlags &= ~OSSwapHostToBigConstInt16(kFinderInvisibleMask);
	}

	/*
	 * Timestamp updates.
	 */
	VATTR_SET_SUPPORTED(vap, va_create_time);
	VATTR_SET_SUPPORTED(vap, va_access_time);
	VATTR_SET_SUPPORTED(vap, va_modify_time);
	VATTR_SET_SUPPORTED(vap, va_backup_time);
	VATTR_SET_SUPPORTED(vap, va_change_time);
	if (VATTR_IS_ACTIVE(vap, va_create_time) ||
	    VATTR_IS_ACTIVE(vap, va_access_time) ||
	    VATTR_IS_ACTIVE(vap, va_modify_time) ||
	    VATTR_IS_ACTIVE(vap, va_backup_time)) {
		if (VATTR_IS_ACTIVE(vap, va_create_time))
			cp->c_itime = vap->va_create_time.tv_sec;
		if (VATTR_IS_ACTIVE(vap, va_access_time)) {
			cp->c_atime = vap->va_access_time.tv_sec;
			cp->c_touch_acctime = FALSE;
		}
		if (VATTR_IS_ACTIVE(vap, va_modify_time)) {
			cp->c_mtime = vap->va_modify_time.tv_sec;
			cp->c_touch_modtime = FALSE;
			cp->c_touch_chgtime = TRUE;

			/*
			 * The utimes system call can reset the modification
			 * time but it doesn't know about HFS create times.
			 * So we need to ensure that the creation time is
			 * always at least as old as the modification time.
			 */
			if ((VTOVCB(vp)->vcbSigWord == kHFSPlusSigWord) &&
			    (cp->c_cnid != kHFSRootFolderID) &&
			    (cp->c_mtime < cp->c_itime)) {
				cp->c_itime = cp->c_mtime;
			}
		}
		if (VATTR_IS_ACTIVE(vap, va_backup_time))
			cp->c_btime = vap->va_backup_time.tv_sec;
		cp->c_flag |= C_MODIFIED;
	}
	
	/*
	 * Set name encoding.
	 */
	VATTR_SET_SUPPORTED(vap, va_encoding);
	if (VATTR_IS_ACTIVE(vap, va_encoding)) {
		cp->c_encoding = vap->va_encoding;
		hfs_setencodingbits(hfsmp, cp->c_encoding);
	}

	if ((error = hfs_update(vp, TRUE)) != 0)
		goto out;
out:
	if (cp) {
		/* Purge origin cache for cnode, since caller now has correct link ID for it 
		 * We purge it here since it was acquired for us during lookup, and we no longer need it.
		 */
		if ((cp->c_flag & C_HARDLINK) && (vp->v_type != VDIR)){
			hfs_relorigin(cp, 0);
		}

		hfs_unlock(cp);
#if HFS_COMPRESSION
		if (decmpfs_reset_state) {
			/*
			 * we've changed the UF_COMPRESSED flag, so reset the decmpfs state for this cnode
			 * but don't do it while holding the hfs cnode lock
			 */
			decmpfs_cnode *dp = VTOCMP(vp);
			if (!dp) {
				/*
				 * call hfs_lazy_init_decmpfs_cnode() to make sure that the decmpfs_cnode
				 * is filled in; we need a decmpfs_cnode to prevent decmpfs state changes
				 * on this file if it's locked
				 */
				dp = hfs_lazy_init_decmpfs_cnode(VTOC(vp));
				if (!dp) {
					/* failed to allocate a decmpfs_cnode */
					return ENOMEM; /* what should this be? */
				}
			}
			decmpfs_cnode_set_vnode_state(dp, FILE_TYPE_UNKNOWN, 0);
		}
#endif
	}
	return (error);
}


/*
 * Change the mode on a file.
 * cnode must be locked before calling.
 */
__private_extern__
int
hfs_chmod(struct vnode *vp, int mode, __unused kauth_cred_t cred, __unused struct proc *p)
{
	register struct cnode *cp = VTOC(vp);

	if (VTOVCB(vp)->vcbSigWord != kHFSPlusSigWord)
		return (0);

	// XXXdbg - don't allow modification of the journal or journal_info_block
	if (VTOHFS(vp)->jnl && cp && cp->c_datafork) {
		struct HFSPlusExtentDescriptor *extd;

		extd = &cp->c_datafork->ff_extents[0];
		if (extd->startBlock == VTOVCB(vp)->vcbJinfoBlock || extd->startBlock == VTOHFS(vp)->jnl_start) {
			return EPERM;
		}
	}

#if OVERRIDE_UNKNOWN_PERMISSIONS
	if (((unsigned int)vfs_flags(VTOVFS(vp))) & MNT_UNKNOWNPERMISSIONS) {
		return (0);
	};
#endif
	cp->c_mode &= ~ALLPERMS;
	cp->c_mode |= (mode & ALLPERMS);
	cp->c_touch_chgtime = TRUE;
	return (0);
}


__private_extern__
int
hfs_write_access(struct vnode *vp, kauth_cred_t cred, struct proc *p, Boolean considerFlags)
{
	struct cnode *cp = VTOC(vp);
	int retval = 0;
	int is_member;

	/*
	 * Disallow write attempts on read-only file systems;
	 * unless the file is a socket, fifo, or a block or
	 * character device resident on the file system.
	 */
	switch (vnode_vtype(vp)) {
	case VDIR:
 	case VLNK:
	case VREG:
		if (VTOHFS(vp)->hfs_flags & HFS_READ_ONLY)
			return (EROFS);
		break;
	default:
		break;
 	}
 
	/* If immutable bit set, nobody gets to write it. */
	if (considerFlags && (cp->c_flags & IMMUTABLE))
		return (EPERM);

	/* Otherwise, user id 0 always gets access. */
	if (!suser(cred, NULL))
		return (0);

	/* Otherwise, check the owner. */
	if ((retval = hfs_owner_rights(VTOHFS(vp), cp->c_uid, cred, p, false)) == 0)
		return ((cp->c_mode & S_IWUSR) == S_IWUSR ? 0 : EACCES);
 
	/* Otherwise, check the groups. */
	if (kauth_cred_ismember_gid(cred, cp->c_gid, &is_member) == 0 && is_member) {
		return ((cp->c_mode & S_IWGRP) == S_IWGRP ? 0 : EACCES);
 	}
 
	/* Otherwise, check everyone else. */
	return ((cp->c_mode & S_IWOTH) == S_IWOTH ? 0 : EACCES);
}


/*
 * Perform chown operation on cnode cp;
 * code must be locked prior to call.
 */
__private_extern__
int
#if !QUOTA
hfs_chown(struct vnode *vp, uid_t uid, gid_t gid, __unused kauth_cred_t cred,
	__unused struct proc *p)
#else 
hfs_chown(struct vnode *vp, uid_t uid, gid_t gid, kauth_cred_t cred,
	__unused struct proc *p)
#endif
{
	register struct cnode *cp = VTOC(vp);
	uid_t ouid;
	gid_t ogid;
#if QUOTA
	int error = 0;
	register int i;
	int64_t change;
#endif /* QUOTA */

	if (VTOVCB(vp)->vcbSigWord != kHFSPlusSigWord)
		return (ENOTSUP);

	if (((unsigned int)vfs_flags(VTOVFS(vp))) & MNT_UNKNOWNPERMISSIONS)
		return (0);
	
	if (uid == (uid_t)VNOVAL)
		uid = cp->c_uid;
	if (gid == (gid_t)VNOVAL)
		gid = cp->c_gid;

#if 0	/* we are guaranteed that this is already the case */
	/*
	 * If we don't own the file, are trying to change the owner
	 * of the file, or are not a member of the target group,
	 * the caller must be superuser or the call fails.
	 */
	if ((kauth_cred_getuid(cred) != cp->c_uid || uid != cp->c_uid ||
	    (gid != cp->c_gid &&
	     (kauth_cred_ismember_gid(cred, gid, &is_member) || !is_member))) &&
	    (error = suser(cred, 0)))
		return (error);
#endif

	ogid = cp->c_gid;
	ouid = cp->c_uid;
#if QUOTA
	if ((error = hfs_getinoquota(cp)))
		return (error);
	if (ouid == uid) {
		dqrele(cp->c_dquot[USRQUOTA]);
		cp->c_dquot[USRQUOTA] = NODQUOT;
	}
	if (ogid == gid) {
		dqrele(cp->c_dquot[GRPQUOTA]);
		cp->c_dquot[GRPQUOTA] = NODQUOT;
	}

	/*
	 * Eventually need to account for (fake) a block per directory
	 * if (vnode_isdir(vp))
	 *     change = VTOHFS(vp)->blockSize;
	 * else
	 */

	change = (int64_t)(cp->c_blocks) * (int64_t)VTOVCB(vp)->blockSize;
	(void) hfs_chkdq(cp, -change, cred, CHOWN);
	(void) hfs_chkiq(cp, -1, cred, CHOWN);
	for (i = 0; i < MAXQUOTAS; i++) {
		dqrele(cp->c_dquot[i]);
		cp->c_dquot[i] = NODQUOT;
	}
#endif /* QUOTA */
	cp->c_gid = gid;
	cp->c_uid = uid;
#if QUOTA
	if ((error = hfs_getinoquota(cp)) == 0) {
		if (ouid == uid) {
			dqrele(cp->c_dquot[USRQUOTA]);
			cp->c_dquot[USRQUOTA] = NODQUOT;
		}
		if (ogid == gid) {
			dqrele(cp->c_dquot[GRPQUOTA]);
			cp->c_dquot[GRPQUOTA] = NODQUOT;
		}
		if ((error = hfs_chkdq(cp, change, cred, CHOWN)) == 0) {
			if ((error = hfs_chkiq(cp, 1, cred, CHOWN)) == 0)
				goto good;
			else
				(void) hfs_chkdq(cp, -change, cred, CHOWN|FORCE);
		}
		for (i = 0; i < MAXQUOTAS; i++) {
			dqrele(cp->c_dquot[i]);
			cp->c_dquot[i] = NODQUOT;
		}
	}
	cp->c_gid = ogid;
	cp->c_uid = ouid;
	if (hfs_getinoquota(cp) == 0) {
		if (ouid == uid) {
			dqrele(cp->c_dquot[USRQUOTA]);
			cp->c_dquot[USRQUOTA] = NODQUOT;
		}
		if (ogid == gid) {
			dqrele(cp->c_dquot[GRPQUOTA]);
			cp->c_dquot[GRPQUOTA] = NODQUOT;
		}
		(void) hfs_chkdq(cp, change, cred, FORCE|CHOWN);
		(void) hfs_chkiq(cp, 1, cred, FORCE|CHOWN);
		(void) hfs_getinoquota(cp);
	}
	return (error);
good:
	if (hfs_getinoquota(cp))
		panic("hfs_chown: lost quota");
#endif /* QUOTA */


	/*
	  According to the SUSv3 Standard, chown() shall mark
	  for update the st_ctime field of the file.
	  (No exceptions mentioned)
	*/
		cp->c_touch_chgtime = TRUE;
	return (0);
}


/*
 * The hfs_exchange routine swaps the fork data in two files by
 * exchanging some of the information in the cnode.  It is used
 * to preserve the file ID when updating an existing file, in
 * case the file is being tracked through its file ID. Typically
 * its used after creating a new file during a safe-save.
 */
static int
hfs_vnop_exchange(ap)
	struct vnop_exchange_args /* {
		struct vnode *a_fvp;
		struct vnode *a_tvp;
		int a_options;
		vfs_context_t a_context;
	} */ *ap;
{
	struct vnode *from_vp = ap->a_fvp;
	struct vnode *to_vp = ap->a_tvp;
	struct cnode *from_cp;
	struct cnode *to_cp;
	struct hfsmount *hfsmp;
	struct cat_desc tempdesc;
	struct cat_attr tempattr;
	const unsigned char *from_nameptr;
	const unsigned char *to_nameptr;
	char from_iname[32];
	char to_iname[32];
	u_int32_t tempflag;
	cnid_t  from_parid;
	cnid_t  to_parid;
	int lockflags;
	int error = 0, started_tr = 0, got_cookie = 0;
	cat_cookie_t cookie;

	/* The files must be on the same volume. */
	if (vnode_mount(from_vp) != vnode_mount(to_vp))
		return (EXDEV);

	if (from_vp == to_vp)
		return (EINVAL);

#if HFS_COMPRESSION
	if ( hfs_file_is_compressed(VTOC(from_vp), 0) ) {
		if ( 0 != ( error = decmpfs_decompress_file(from_vp, VTOCMP(from_vp), -1, 0, 1) ) ) {
			return error;
		}
	}
	
	if ( hfs_file_is_compressed(VTOC(to_vp), 0) ) {
		if ( 0 != ( error = decmpfs_decompress_file(to_vp, VTOCMP(to_vp), -1, 0, 1) ) ) {
			return error;
		}
	}
#endif // HFS_COMPRESSION
	
	if ((error = hfs_lockpair(VTOC(from_vp), VTOC(to_vp), HFS_EXCLUSIVE_LOCK)))
		return (error);

	from_cp = VTOC(from_vp);
	to_cp = VTOC(to_vp);
	hfsmp = VTOHFS(from_vp);

	/* Only normal files can be exchanged. */
	if (!vnode_isreg(from_vp) || !vnode_isreg(to_vp) ||
	    VNODE_IS_RSRC(from_vp) || VNODE_IS_RSRC(to_vp)) {
		error = EINVAL;
		goto exit;
	}

	// XXXdbg - don't allow modification of the journal or journal_info_block
	if (hfsmp->jnl) {
		struct HFSPlusExtentDescriptor *extd;

		if (from_cp->c_datafork) {
			extd = &from_cp->c_datafork->ff_extents[0];
			if (extd->startBlock == VTOVCB(from_vp)->vcbJinfoBlock || extd->startBlock == hfsmp->jnl_start) {
				error = EPERM;
				goto exit;
			}
		}

		if (to_cp->c_datafork) {
			extd = &to_cp->c_datafork->ff_extents[0];
			if (extd->startBlock == VTOVCB(to_vp)->vcbJinfoBlock || extd->startBlock == hfsmp->jnl_start) {
				error = EPERM;
				goto exit;
			}
		}
	}

	if ((error = hfs_start_transaction(hfsmp)) != 0) {
	    goto exit;
	}
	started_tr = 1;
	
	/*
	 * Reserve some space in the Catalog file.
	 */
	if ((error = cat_preflight(hfsmp, CAT_EXCHANGE, &cookie, vfs_context_proc(ap->a_context)))) {
		goto exit;
	}
	got_cookie = 1;

	/* The backend code always tries to delete the virtual
	 * extent id for exchanging files so we need to lock
	 * the extents b-tree.
	 */
	lockflags = hfs_systemfile_lock(hfsmp, SFL_CATALOG | SFL_EXTENTS | SFL_ATTRIBUTE, HFS_EXCLUSIVE_LOCK);

	/* Account for the location of the catalog objects. */
	if (from_cp->c_flag & C_HARDLINK) {
		MAKE_INODE_NAME(from_iname, sizeof(from_iname),
				from_cp->c_attr.ca_linkref);
		from_nameptr = (unsigned char *)from_iname;
		from_parid = hfsmp->hfs_private_desc[FILE_HARDLINKS].cd_cnid;
		from_cp->c_hint = 0;
	} else {
		from_nameptr = from_cp->c_desc.cd_nameptr;
		from_parid = from_cp->c_parentcnid;
	}
	if (to_cp->c_flag & C_HARDLINK) {
		MAKE_INODE_NAME(to_iname, sizeof(to_iname),
				to_cp->c_attr.ca_linkref);
		to_nameptr = (unsigned char *)to_iname;
		to_parid = hfsmp->hfs_private_desc[FILE_HARDLINKS].cd_cnid;
		to_cp->c_hint = 0;
	} else {
		to_nameptr = to_cp->c_desc.cd_nameptr;
		to_parid = to_cp->c_parentcnid;
	}

	/* Do the exchange */
	error = ExchangeFileIDs(hfsmp, from_nameptr, to_nameptr, from_parid,
	                        to_parid, from_cp->c_hint, to_cp->c_hint);
	hfs_systemfile_unlock(hfsmp, lockflags);

	/*
	 * Note that we don't need to exchange any extended attributes
	 * since the attributes are keyed by file ID.
	 */

	if (error != E_NONE) {
		error = MacToVFSError(error);
		goto exit;
	}

	/* Purge the vnodes from the name cache */
 	if (from_vp)
		cache_purge(from_vp);
	if (to_vp)
		cache_purge(to_vp);

	/* Save a copy of from attributes before swapping. */
	bcopy(&from_cp->c_desc, &tempdesc, sizeof(struct cat_desc));
	bcopy(&from_cp->c_attr, &tempattr, sizeof(struct cat_attr));
	tempflag = from_cp->c_flag & (C_HARDLINK | C_HASXATTRS);

	/*
	 * Swap the descriptors and all non-fork related attributes.
	 * (except the modify date)
	 */
	bcopy(&to_cp->c_desc, &from_cp->c_desc, sizeof(struct cat_desc));

	from_cp->c_hint = 0;
	from_cp->c_fileid = from_cp->c_cnid;
	from_cp->c_itime = to_cp->c_itime;
	from_cp->c_btime = to_cp->c_btime;
	from_cp->c_atime = to_cp->c_atime;
	from_cp->c_ctime = to_cp->c_ctime;
	from_cp->c_gid = to_cp->c_gid;
	from_cp->c_uid = to_cp->c_uid;
	from_cp->c_flags = to_cp->c_flags;
	from_cp->c_mode = to_cp->c_mode;
	from_cp->c_linkcount = to_cp->c_linkcount;
	from_cp->c_flag = to_cp->c_flag & (C_HARDLINK | C_HASXATTRS);
	from_cp->c_attr.ca_recflags = to_cp->c_attr.ca_recflags;
	bcopy(to_cp->c_finderinfo, from_cp->c_finderinfo, 32);

	bcopy(&tempdesc, &to_cp->c_desc, sizeof(struct cat_desc));
	to_cp->c_hint = 0;
	to_cp->c_fileid = to_cp->c_cnid;
	to_cp->c_itime = tempattr.ca_itime;
	to_cp->c_btime = tempattr.ca_btime;
	to_cp->c_atime = tempattr.ca_atime;
	to_cp->c_ctime = tempattr.ca_ctime;
	to_cp->c_gid = tempattr.ca_gid;
	to_cp->c_uid = tempattr.ca_uid;
	to_cp->c_flags = tempattr.ca_flags;
	to_cp->c_mode = tempattr.ca_mode;
	to_cp->c_linkcount = tempattr.ca_linkcount;
	to_cp->c_flag = tempflag;
	to_cp->c_attr.ca_recflags = tempattr.ca_recflags;
	bcopy(tempattr.ca_finderinfo, to_cp->c_finderinfo, 32);

	/* Rehash the cnodes using their new file IDs */
	hfs_chash_rehash(hfsmp, from_cp, to_cp);

	/*
	 * When a file moves out of "Cleanup At Startup"
	 * we can drop its NODUMP status.
	 */
	if ((from_cp->c_flags & UF_NODUMP) &&
	    (from_cp->c_parentcnid != to_cp->c_parentcnid)) {
		from_cp->c_flags &= ~UF_NODUMP;
		from_cp->c_touch_chgtime = TRUE;
	}
	if ((to_cp->c_flags & UF_NODUMP) &&
	    (to_cp->c_parentcnid != from_cp->c_parentcnid)) {
		to_cp->c_flags &= ~UF_NODUMP;
		to_cp->c_touch_chgtime = TRUE;
	}

exit:
	if (got_cookie) {
	        cat_postflight(hfsmp, &cookie, vfs_context_proc(ap->a_context));
	}
	if (started_tr) {
	    hfs_end_transaction(hfsmp);
	}

	hfs_unlockpair(from_cp, to_cp);
	return (error);
}


/*
 *  cnode must be locked
 */
__private_extern__
int
hfs_fsync(struct vnode *vp, int waitfor, int fullsync, struct proc *p)
{
	struct cnode *cp = VTOC(vp);
	struct filefork *fp = NULL;
	int retval = 0;
	struct hfsmount *hfsmp = VTOHFS(vp);
	struct rl_entry *invalid_range;
	struct timeval tv;
	int waitdata;		/* attributes necessary for data retrieval */
	int wait;		/* all other attributes (e.g. atime, etc.) */
	int lockflag;
	int took_trunc_lock = 0;
	boolean_t trunc_lock_exclusive = FALSE;

	/*
	 * Applications which only care about data integrity rather than full
	 * file integrity may opt out of (delay) expensive metadata update
	 * operations as a performance optimization.
	 */
	wait = (waitfor == MNT_WAIT);
	waitdata = (waitfor == MNT_DWAIT) | wait;
	if (always_do_fullfsync)
		fullsync = 1;
	
	/* HFS directories don't have any data blocks. */
	if (vnode_isdir(vp))
		goto metasync;
	fp = VTOF(vp);

	/*
	 * For system files flush the B-tree header and
	 * for regular files write out any clusters
	 */
	if (vnode_issystem(vp)) {
	    if (VTOF(vp)->fcbBTCBPtr != NULL) {
			// XXXdbg
			if (hfsmp->jnl == NULL) {
				BTFlushPath(VTOF(vp));
			}
	    }
	} else if (UBCINFOEXISTS(vp)) {
		hfs_unlock(cp);
		hfs_lock_truncate(cp, trunc_lock_exclusive);
		took_trunc_lock = 1;

		if (fp->ff_unallocblocks != 0) {
			hfs_unlock_truncate(cp, trunc_lock_exclusive);

			trunc_lock_exclusive = TRUE;
			hfs_lock_truncate(cp, trunc_lock_exclusive);
		}
		/* Don't hold cnode lock when calling into cluster layer. */
		(void) cluster_push(vp, waitdata ? IO_SYNC : 0);

		hfs_lock(cp, HFS_FORCE_LOCK);
	}
	/*
	 * When MNT_WAIT is requested and the zero fill timeout
	 * has expired then we must explicitly zero out any areas
	 * that are currently marked invalid (holes).
	 *
	 * Files with NODUMP can bypass zero filling here.
	 */
	if (fp && (((cp->c_flag & C_ALWAYS_ZEROFILL) && !TAILQ_EMPTY(&fp->ff_invalidranges)) ||
	    ((wait || (cp->c_flag & C_ZFWANTSYNC)) &&
		((cp->c_flags & UF_NODUMP) == 0) &&
		UBCINFOEXISTS(vp) && (vnode_issystem(vp) ==0) &&
		cp->c_zftimeout != 0))) {

		microuptime(&tv);
		if ((cp->c_flag & C_ALWAYS_ZEROFILL) == 0 && !fullsync && tv.tv_sec < (long)cp->c_zftimeout) {
			/* Remember that a force sync was requested. */
			cp->c_flag |= C_ZFWANTSYNC;
			goto datasync;
		}
		if (!TAILQ_EMPTY(&fp->ff_invalidranges)) {
			if (!took_trunc_lock || trunc_lock_exclusive == FALSE) {
				hfs_unlock(cp);
				if (took_trunc_lock)
					hfs_unlock_truncate(cp, trunc_lock_exclusive);

				trunc_lock_exclusive = TRUE;
				hfs_lock_truncate(cp, trunc_lock_exclusive);
				hfs_lock(cp, HFS_FORCE_LOCK);
				took_trunc_lock = 1;
			}
			while ((invalid_range = TAILQ_FIRST(&fp->ff_invalidranges))) {
				off_t start = invalid_range->rl_start;
				off_t end = invalid_range->rl_end;
    		
				/* The range about to be written must be validated
				 * first, so that VNOP_BLOCKMAP() will return the
				 * appropriate mapping for the cluster code:
				 */
				rl_remove(start, end, &fp->ff_invalidranges);

				/* Don't hold cnode lock when calling into cluster layer. */
				hfs_unlock(cp);
				(void) cluster_write(vp, (struct uio *) 0,
						     fp->ff_size, end + 1, start, (off_t)0,
						     IO_HEADZEROFILL | IO_NOZERODIRTY | IO_NOCACHE);
				hfs_lock(cp, HFS_FORCE_LOCK);
				cp->c_flag |= C_MODIFIED;
			}
			hfs_unlock(cp);
			(void) cluster_push(vp, waitdata ? IO_SYNC : 0);
			hfs_lock(cp, HFS_FORCE_LOCK);
		}
		cp->c_flag &= ~C_ZFWANTSYNC;
		cp->c_zftimeout = 0;
	}
datasync:
	if (took_trunc_lock) {
		hfs_unlock_truncate(cp, trunc_lock_exclusive);
		took_trunc_lock = 0;
	}
	/*
	 * if we have a journal and if journal_active() returns != 0 then the
	 * we shouldn't do anything to a locked block (because it is part 
	 * of a transaction).  otherwise we'll just go through the normal 
	 * code path and flush the buffer.  note journal_active() can return
	 * -1 if the journal is invalid -- however we still need to skip any 
	 * locked blocks as they get cleaned up when we finish the transaction
	 * or close the journal.
	 */
	// if (hfsmp->jnl && journal_active(hfsmp->jnl) >= 0)
	if (hfsmp->jnl)
	        lockflag = BUF_SKIP_LOCKED;
	else
	        lockflag = 0;

	/*
	 * Flush all dirty buffers associated with a vnode.
	 */
	buf_flushdirtyblks(vp, waitdata, lockflag, "hfs_fsync");

metasync:
	if (vnode_isreg(vp) && vnode_issystem(vp)) {
		if (VTOF(vp)->fcbBTCBPtr != NULL) {
			microuptime(&tv);
			BTSetLastSync(VTOF(vp), tv.tv_sec);
		}
		cp->c_touch_acctime = FALSE;
		cp->c_touch_chgtime = FALSE;
		cp->c_touch_modtime = FALSE;
	} else if ( !(vp->v_flag & VSWAP) ) /* User file */ {
		retval = hfs_update(vp, wait);

		/*
		 * When MNT_WAIT is requested push out the catalog record for
		 * this file.  If they asked for a full fsync, we can skip this
		 * because the journal_flush or hfs_metasync_all will push out
		 * all of the metadata changes.
		 */
   		if ((retval == 0) && wait && !fullsync && cp->c_hint &&
   		    !ISSET(cp->c_flag, C_DELETED | C_NOEXISTS)) {
   			hfs_metasync(VTOHFS(vp), (daddr64_t)cp->c_hint, p);
		}

		/*
		 * If this was a full fsync, make sure all metadata
		 * changes get to stable storage.
		 */
		if (fullsync) {
		    if (hfsmp->jnl) {
			hfs_journal_flush(hfsmp);
		    } else {
			retval = hfs_metasync_all(hfsmp);
		    	/* XXX need to pass context! */
			VNOP_IOCTL(hfsmp->hfs_devvp, DKIOCSYNCHRONIZECACHE, NULL, FWRITE, NULL);
		    }
		}
	}

	return (retval);
}


/* Sync an hfs catalog b-tree node */
static int
hfs_metasync(struct hfsmount *hfsmp, daddr64_t node, __unused struct proc *p)
{
	vnode_t	vp;
	buf_t	bp;
	int lockflags;

	vp = HFSTOVCB(hfsmp)->catalogRefNum;

	// XXXdbg - don't need to do this on a journaled volume
	if (hfsmp->jnl) {
		return 0;
	}

	lockflags = hfs_systemfile_lock(hfsmp, SFL_CATALOG, HFS_EXCLUSIVE_LOCK);
	/*
	 * Look for a matching node that has been delayed
	 * but is not part of a set (B_LOCKED).
	 *
	 * BLK_ONLYVALID causes buf_getblk to return a
	 * buf_t for the daddr64_t specified only if it's
	 * currently resident in the cache... the size
	 * parameter to buf_getblk is ignored when this flag
	 * is set
	 */
	bp = buf_getblk(vp, node, 0, 0, 0, BLK_META | BLK_ONLYVALID);

	if (bp) {
	        if ((buf_flags(bp) & (B_LOCKED | B_DELWRI)) == B_DELWRI)
		        (void) VNOP_BWRITE(bp);
		else
		        buf_brelse(bp);
	}

	hfs_systemfile_unlock(hfsmp, lockflags);

	return (0);
}


/*
 * Sync all hfs B-trees.  Use this instead of journal_flush for a volume
 * without a journal.  Note that the volume bitmap does not get written;
 * we rely on fsck_hfs to fix that up (which it can do without any loss
 * of data).
 */
static int
hfs_metasync_all(struct hfsmount *hfsmp)
{
	int lockflags;

	/* Lock all of the B-trees so we get a mutually consistent state */
	lockflags = hfs_systemfile_lock(hfsmp,
		SFL_CATALOG|SFL_EXTENTS|SFL_ATTRIBUTE, HFS_EXCLUSIVE_LOCK);

	/* Sync each of the B-trees */
	if (hfsmp->hfs_catalog_vp)
		hfs_btsync(hfsmp->hfs_catalog_vp, 0);
	if (hfsmp->hfs_extents_vp)
		hfs_btsync(hfsmp->hfs_extents_vp, 0);
	if (hfsmp->hfs_attribute_vp)
		hfs_btsync(hfsmp->hfs_attribute_vp, 0);
	
	/* Wait for all of the writes to complete */
	if (hfsmp->hfs_catalog_vp)
		vnode_waitforwrites(hfsmp->hfs_catalog_vp, 0, 0, 0, "hfs_metasync_all");
	if (hfsmp->hfs_extents_vp)
		vnode_waitforwrites(hfsmp->hfs_extents_vp, 0, 0, 0, "hfs_metasync_all");
	if (hfsmp->hfs_attribute_vp)
		vnode_waitforwrites(hfsmp->hfs_attribute_vp, 0, 0, 0, "hfs_metasync_all");

	hfs_systemfile_unlock(hfsmp, lockflags);
	
	return 0;
}


/*ARGSUSED 1*/
static int
hfs_btsync_callback(struct buf *bp, __unused void *dummy)
{
	buf_clearflags(bp, B_LOCKED);
	(void) buf_bawrite(bp);

	return(BUF_CLAIMED);
}


__private_extern__
int
hfs_btsync(struct vnode *vp, int sync_transaction)
{
	struct cnode *cp = VTOC(vp);
	struct timeval tv;
	int    flags = 0;

	if (sync_transaction)
	        flags |= BUF_SKIP_NONLOCKED;
	/*
	 * Flush all dirty buffers associated with b-tree.
	 */
	buf_iterate(vp, hfs_btsync_callback, flags, 0);

	microuptime(&tv);
	if (vnode_issystem(vp) && (VTOF(vp)->fcbBTCBPtr != NULL))
		(void) BTSetLastSync(VTOF(vp), tv.tv_sec);
	cp->c_touch_acctime = FALSE;
	cp->c_touch_chgtime = FALSE;
	cp->c_touch_modtime = FALSE;

	return 0;
}

/*
 * Remove a directory.
 */
static int
hfs_vnop_rmdir(ap)
	struct vnop_rmdir_args /* {
		struct vnode *a_dvp;
		struct vnode *a_vp;
		struct componentname *a_cnp;
		vfs_context_t a_context;
	} */ *ap;
{
	struct vnode *dvp = ap->a_dvp;
	struct vnode *vp = ap->a_vp;
	struct cnode *dcp = VTOC(dvp);
	struct cnode *cp = VTOC(vp);
	int error;

	if (!S_ISDIR(cp->c_mode)) {
		return (ENOTDIR);
	}
	if (dvp == vp) {
		return (EINVAL);
	}
	if ((error = hfs_lockpair(dcp, cp, HFS_EXCLUSIVE_LOCK))) {
		return (error);
	}

	/* Check for a race with rmdir on the parent directory */
	if (dcp->c_flag & (C_DELETED | C_NOEXISTS)) {
		hfs_unlockpair (dcp, cp);
		return ENOENT;
	}
	error = hfs_removedir(dvp, vp, ap->a_cnp, 0);

	hfs_unlockpair(dcp, cp);

	return (error);
}

/*
 * Remove a directory
 *
 * Both dvp and vp cnodes are locked
 */
static int
hfs_removedir(struct vnode *dvp, struct vnode *vp, struct componentname *cnp,
              int skip_reserve)
{
	struct cnode *cp;
	struct cnode *dcp;
	struct hfsmount * hfsmp;
	struct cat_desc desc;
	int lockflags;
	int error = 0, started_tr = 0;

	cp = VTOC(vp);
	dcp = VTOC(dvp);
	hfsmp = VTOHFS(vp);

	if (dcp == cp) {
		return (EINVAL);	/* cannot remove "." */
	}
	if (cp->c_flag & (C_NOEXISTS | C_DELETED)) {
		return (0);
	}
	if (cp->c_entries != 0) {
		return (ENOTEMPTY);
	}

	/* Check if we're removing the last link to an empty directory. */
	if (cp->c_flag & C_HARDLINK) {
		/* We could also return EBUSY here */
		return hfs_unlink(hfsmp, dvp, vp, cnp, skip_reserve);
	}
	
	/*
	 * We want to make sure that if the directory has a lot of attributes, we process them
	 * in separate transactions to ensure we don't panic in the journal with a gigantic
	 * transaction. This means we'll let hfs_removefile deal with the directory, which generally
	 * follows the same codepath as open-unlinked files.  Note that the last argument to 
	 * hfs_removefile specifies that it is supposed to handle directories for this case.
	 */
	if ((hfsmp->hfs_attribute_vp != NULL) &&
	    (cp->c_attr.ca_recflags & kHFSHasAttributesMask) != 0) {

	    return hfs_removefile(dvp, vp, cnp, 0, 0, 1, NULL);
	}

	dcp->c_flag |= C_DIR_MODIFICATION;

#if QUOTA
	if (hfsmp->hfs_flags & HFS_QUOTAS)
		(void)hfs_getinoquota(cp);
#endif
	if ((error = hfs_start_transaction(hfsmp)) != 0) {
	    goto out;
	}
	started_tr = 1;

	/*
	 * Verify the directory is empty (and valid).
	 * (Rmdir ".." won't be valid since
	 *  ".." will contain a reference to
	 *  the current directory and thus be
	 *  non-empty.)
	 */
	if ((dcp->c_flags & APPEND) || (cp->c_flags & (IMMUTABLE | APPEND))) {
		error = EPERM;
		goto out;
	}

	/* Remove the entry from the namei cache: */
	cache_purge(vp);

	/* 
	 * Protect against a race with rename by using the component
	 * name passed in and parent id from dvp (instead of using 
	 * the cp->c_desc which may have changed).
	 */
	desc.cd_nameptr = (const u_int8_t *)cnp->cn_nameptr;
	desc.cd_namelen = cnp->cn_namelen;
	desc.cd_parentcnid = dcp->c_fileid;
	desc.cd_cnid = cp->c_cnid;
	desc.cd_flags = CD_ISDIR;
	desc.cd_encoding = cp->c_encoding;
	desc.cd_hint = 0;

	if (!hfs_valid_cnode(hfsmp, dvp, cnp, cp->c_fileid)) {
	    error = 0;
	    goto out;
	}

	/* Remove entry from catalog */
	lockflags = hfs_systemfile_lock(hfsmp, SFL_CATALOG | SFL_ATTRIBUTE | SFL_BITMAP, HFS_EXCLUSIVE_LOCK);

	if (!skip_reserve) {
		/*
		 * Reserve some space in the Catalog file.
		 */
		if ((error = cat_preflight(hfsmp, CAT_DELETE, NULL, 0))) {
			hfs_systemfile_unlock(hfsmp, lockflags);
			goto out;
		}
	}

	error = cat_delete(hfsmp, &desc, &cp->c_attr);
	if (error == 0) {
		/* The parent lost a child */
		if (dcp->c_entries > 0)
			dcp->c_entries--;
		DEC_FOLDERCOUNT(hfsmp, dcp->c_attr);
		dcp->c_dirchangecnt++;
		dcp->c_touch_chgtime = TRUE;
		dcp->c_touch_modtime = TRUE;
		hfs_touchtimes(hfsmp, cp);
		(void) cat_update(hfsmp, &dcp->c_desc, &dcp->c_attr, NULL, NULL);
		cp->c_flag &= ~(C_MODIFIED | C_FORCEUPDATE);
	}

	hfs_systemfile_unlock(hfsmp, lockflags);

	if (error)
		goto out;

#if QUOTA
	if (hfsmp->hfs_flags & HFS_QUOTAS)
		(void)hfs_chkiq(cp, -1, NOCRED, 0);
#endif /* QUOTA */

	hfs_volupdate(hfsmp, VOL_RMDIR, (dcp->c_cnid == kHFSRootFolderID));

	/*
	 * directory open or in use (e.g. opendir() or current working
	 * directory for some process); wait for inactive to actually
	 * remove catalog entry
	 */
	if (vnode_isinuse(vp, 0)) {
		cp->c_flag |= C_DELETED;
	} else {
		cp->c_flag |= C_NOEXISTS;
	}
out:
	dcp->c_flag &= ~C_DIR_MODIFICATION;
	wakeup((caddr_t)&dcp->c_flag);

	if (started_tr) { 
	    hfs_end_transaction(hfsmp);
	}

	return (error);
}


/*
 * Remove a file or link.
 */
static int
hfs_vnop_remove(ap)
	struct vnop_remove_args /* {
		struct vnode *a_dvp;
		struct vnode *a_vp;
		struct componentname *a_cnp;
		int a_flags;
		vfs_context_t a_context;
	} */ *ap;
{
	struct vnode *dvp = ap->a_dvp;
	struct vnode *vp = ap->a_vp;
	struct cnode *dcp = VTOC(dvp);
	struct cnode *cp = VTOC(vp);
	struct vnode *rvp = NULL;
	struct hfsmount *hfsmp = VTOHFS(vp);	
	int error=0, recycle_rsrc=0;
	int drop_rsrc_vnode = 0;
	int vref;

	if (dvp == vp) {
		return (EINVAL);
	}

	/* 
 	 * We need to grab the cnode lock on 'cp' before the lockpair() 
	 * to get an iocount on the rsrc fork BEFORE we enter hfs_removefile.
	 * To prevent other deadlocks, it's best to call hfs_vgetrsrc in a way that
	 * allows it to drop the cnode lock that it expects to be held coming in.  
	 * If we don't, we could commit a lock order violation, causing a deadlock.  
	 * In order to safely get the rsrc vnode with an iocount, we need to only hold the 
	 * lock on the file temporarily.  Unlike hfs_vnop_rename, we don't have to worry 
	 * about one rsrc fork getting recycled for another, but we do want to ensure
	 * that there are no deadlocks due to lock ordering issues.
	 * 
	 * Note: this function may be invoked for directory hardlinks, so just skip these
	 * steps if 'vp' is a directory.
	 */


	if ((vp->v_type == VLNK) || (vp->v_type == VREG)) {

		if ((error = hfs_lock (cp, HFS_EXCLUSIVE_LOCK))) {
			return (error);
		}
		error = hfs_vgetrsrc(hfsmp, vp, &rvp, TRUE, TRUE);
		hfs_unlock(cp);
		if (error) {
			/* We may have gotten a rsrc vp out even though we got an error back. */
			if (rvp) {
				vnode_put(rvp);
				rvp = NULL;
			}
			return error;
		}
		drop_rsrc_vnode = 1;
	}
	/* Now that we may have an iocount on rvp, do the lock pair */
	hfs_lock_truncate(cp, TRUE);

	if ((error = hfs_lockpair(dcp, cp, HFS_EXCLUSIVE_LOCK))) {
		hfs_unlock_truncate(cp, TRUE);
		/* drop the iocount on rvp if necessary */
		if (drop_rsrc_vnode) {
			vnode_put (rvp);
		}
		return (error);
	}

	/* 
	 * Check to see if we raced rmdir for the parent directory 
	 * hfs_removefile already checks for a race on vp/cp
	 */
	if (dcp->c_flag & (C_DELETED | C_NOEXISTS)) {
		error = ENOENT;
		goto rm_done;	
	}

	error = hfs_removefile(dvp, vp, ap->a_cnp, ap->a_flags, 0, 0, rvp);

	//
	// If the remove succeeded and it's an open-unlinked file that has
	// a resource fork vnode that's not in use, we will want to recycle
	// the rvp *after* we're done unlocking everything.  Otherwise the
	// resource vnode will keep a v_parent reference on this vnode which
	// prevents it from going through inactive/reclaim which means that
	// the disk space associated with this file won't get free'd until
	// something forces the resource vnode to get recycled (and that can
	// take a very long time).
	//
	if (error == 0 && (cp->c_flag & C_DELETED) && 
			(rvp) && !vnode_isinuse(rvp, 0)) {
	    recycle_rsrc = 1;
	}

	/*
	 * Drop the truncate lock before unlocking the cnode
	 * (which can potentially perform a vnode_put and
	 * recycle the vnode which in turn might require the
	 * truncate lock)
	 */
rm_done:
	hfs_unlock_truncate(cp, TRUE);
	hfs_unlockpair(dcp, cp);

	if (recycle_rsrc) {
		vref = vnode_ref(rvp);
		if (vref == 0) {
			/* vnode_ref could return an error, only release if we got a ref */
			vnode_rele(rvp);
		}
		vnode_recycle(rvp);
	} 
	
	if (drop_rsrc_vnode) {
		/* drop iocount on rsrc fork, was obtained at beginning of fxn */
		vnode_put(rvp);
	}

	return (error);
}


static int
hfs_removefile_callback(struct buf *bp, void *hfsmp) {

        if ( !(buf_flags(bp) & B_META))
	        panic("hfs: symlink bp @ %p is not marked meta-data!\n", bp);
	/*
	 * it's part of the current transaction, kill it.
	 */
	journal_kill_block(((struct hfsmount *)hfsmp)->jnl, bp);

	return (BUF_CLAIMED);
}

/*
 * hfs_removefile
 *
 * Similar to hfs_vnop_remove except there are additional options.
 * This function may be used to remove directories if they have
 * lots of EA's -- note the 'allow_dirs' argument.
 *
 * The 'rvp' argument is used to pass in a resource fork vnode with
 * an iocount to prevent it from getting recycled during usage.  If it
 * is NULL, then it is assumed the caller is a VNOP that cannot operate
 * on resource forks, like hfs_vnop_symlink or hfs_removedir. Otherwise in 
 * a VNOP that takes multiple vnodes, we could violate lock order and 
 * cause a deadlock.  
 *
 * Requires cnode and truncate locks to be held.
 */
static int
hfs_removefile(struct vnode *dvp, struct vnode *vp, struct componentname *cnp,
               int flags, int skip_reserve, int allow_dirs, struct vnode *rvp)
{
	struct cnode *cp;
	struct cnode *dcp;
	struct hfsmount *hfsmp;
	struct cat_desc desc;
	struct timeval tv;
	vfs_context_t ctx = cnp->cn_context;
	int dataforkbusy = 0;
	int rsrcforkbusy = 0;
	int truncated = 0;
	int lockflags;
	int error = 0;
	int started_tr = 0;
	int isbigfile = 0, defer_remove=0, isdir=0;

	cp = VTOC(vp);
	dcp = VTOC(dvp);
	hfsmp = VTOHFS(vp);

	/* Check if we lost a race post lookup. */
	if (cp->c_flag & (C_NOEXISTS | C_DELETED)) {
		return (0);
	}

	if (!hfs_valid_cnode(hfsmp, dvp, cnp, cp->c_fileid)) {
	    return 0;
	}

	/* Make sure a remove is permitted */
	if (VNODE_IS_RSRC(vp)) {
		return (EPERM);
	}
	/* Don't allow deleting the journal or journal_info_block. */
	if (hfsmp->jnl &&
	    (cp->c_fileid == hfsmp->hfs_jnlfileid || cp->c_fileid == hfsmp->hfs_jnlinfoblkid)) {
		return (EPERM);
	}
	/*
	 * Hard links require special handling.
	 */
	if (cp->c_flag & C_HARDLINK) {
		if ((flags & VNODE_REMOVE_NODELETEBUSY) && vnode_isinuse(vp, 0)) {
			return (EBUSY);
		} else {
			/* A directory hard link with a link count of one is 
			 * treated as a regular directory.  Therefore it should 
			 * only be removed using rmdir().
			 */
			if ((vnode_isdir(vp) == 1) && (cp->c_linkcount == 1) && 
			    (allow_dirs == 0)) {
			    	return (EPERM);
			}
			return hfs_unlink(hfsmp, dvp, vp, cnp, skip_reserve);
		}
	}
	/* Directories should call hfs_rmdir! (unless they have a lot of attributes) */
	if (vnode_isdir(vp)) {
		if (allow_dirs == 0)
			return (EPERM);  /* POSIX */
		isdir = 1;
	}
	/* Sanity check the parent ids. */
	if ((cp->c_parentcnid != hfsmp->hfs_private_desc[FILE_HARDLINKS].cd_cnid) &&
	    (cp->c_parentcnid != dcp->c_fileid)) {
		return (EINVAL);
	}

	dcp->c_flag |= C_DIR_MODIFICATION;

	// this guy is going away so mark him as such
	cp->c_flag |= C_DELETED;


	/* Remove our entry from the namei cache. */
	cache_purge(vp);

	/*
	 * We expect the caller, if operating on files,
	 * will have passed in a resource fork vnode with
	 * an iocount, even if there was no content.
	 * We only do the hfs_truncate on the rsrc fork
	 * if we know that it DID have content, however.
	 * This has the bonus of not requiring us to defer
	 * its removal, unless it is in use.
	 */

	/* Check if this file is being used. */
	if (isdir == 0) {
		dataforkbusy = vnode_isinuse(vp, 0);
		/* Only need to defer resource fork removal if in use and has content */
		if (rvp && (cp->c_blocks - VTOF(vp)->ff_blocks)) {
			rsrcforkbusy = vnode_isinuse(rvp, 0);
		}
	}
	
	/* Check if we have to break the deletion into multiple pieces. */
	if (isdir == 0) {
		isbigfile = ((cp->c_datafork->ff_size >= HFS_BIGFILE_SIZE) && overflow_extents(VTOF(vp)));
	}

	/* Check if the file has xattrs.  If it does we'll have to delete them in
	   individual transactions in case there are too many */
	if ((hfsmp->hfs_attribute_vp != NULL) &&
	    (cp->c_attr.ca_recflags & kHFSHasAttributesMask) != 0) {
	    defer_remove = 1;
	}

	/*
	 * Carbon semantics prohibit deleting busy files.
	 * (enforced when VNODE_REMOVE_NODELETEBUSY is requested)
	 */
	if (dataforkbusy || rsrcforkbusy) {
		if ((flags & VNODE_REMOVE_NODELETEBUSY) ||
		    (hfsmp->hfs_private_desc[FILE_HARDLINKS].cd_cnid == 0)) {
			error = EBUSY;
			goto out;
		}
	}

#if QUOTA
	if (hfsmp->hfs_flags & HFS_QUOTAS)
		(void)hfs_getinoquota(cp);
#endif /* QUOTA */

	/* Check if we need a ubc_setsize. */
	if (isdir == 0 && (!dataforkbusy || !rsrcforkbusy)) {
		/*
		 * A ubc_setsize can cause a pagein so defer it
		 * until after the cnode lock is dropped.  The
		 * cnode lock cannot be dropped/reacquired here
		 * since we might already hold the journal lock.
		 */
		if (!dataforkbusy && cp->c_datafork->ff_blocks && !isbigfile) {
			cp->c_flag |= C_NEED_DATA_SETSIZE;
		}
		if (!rsrcforkbusy && rvp) {
			cp->c_flag |= C_NEED_RSRC_SETSIZE;
		}
	}

	if ((error = hfs_start_transaction(hfsmp)) != 0) {
	    goto out;
	}
	started_tr = 1;

	// XXXdbg - if we're journaled, kill any dirty symlink buffers 
	if (hfsmp->jnl && vnode_islnk(vp))
	        buf_iterate(vp, hfs_removefile_callback, BUF_SKIP_NONLOCKED, (void *)hfsmp);

	/*
	 * Truncate any non-busy forks.  Busy forks will
	 * get truncated when their vnode goes inactive.
	 * Note that we will only enter this region if we
	 * can avoid creating an open-unlinked file.  If 
	 * either region is busy, we will have to create an open
	 * unlinked file.
	 * Since we're already inside a transaction,
	 * tell hfs_truncate to skip the ubc_setsize.
	 */
	if (isdir == 0 && (!dataforkbusy && !rsrcforkbusy)) {
		/* 
		 * Note that 5th argument to hfs_truncate indicates whether or not 
		 * hfs_update calls should be suppressed in call to do_hfs_truncate
		 */
		if (!dataforkbusy && !isbigfile && cp->c_datafork->ff_blocks != 0) {
			/* skip update in hfs_truncate */
			error = hfs_truncate(vp, (off_t)0, IO_NDELAY, 1, 1, ctx);
			if (error)
				goto out;
			truncated = 1;
		}
		if (!rsrcforkbusy && rvp) {
			/* skip update in hfs_truncate */
			error = hfs_truncate(rvp, (off_t)0, IO_NDELAY, 1, 1, ctx);
			if (error)
				goto out;
			truncated = 1;
		}
	}

	/* 
	 * Protect against a race with rename by using the component
	 * name passed in and parent id from dvp (instead of using 
	 * the cp->c_desc which may have changed).   Also, be aware that
	 * because we allow directories to be passed in, we need to special case
	 * this temporary descriptor in case we were handed a directory.
	 */
	if (isdir) {
		desc.cd_flags = CD_ISDIR;
	}
	else {
		desc.cd_flags = 0;
	}
	desc.cd_encoding = cp->c_desc.cd_encoding;
	desc.cd_nameptr = (const u_int8_t *)cnp->cn_nameptr;
	desc.cd_namelen = cnp->cn_namelen;
	desc.cd_parentcnid = dcp->c_fileid;
	desc.cd_hint = cp->c_desc.cd_hint;
	desc.cd_cnid = cp->c_cnid;
	microtime(&tv);

	/*
	 * There are two cases to consider:
	 *  1. File/Dir is busy/big/defer_remove ==> move/rename the file/dir
	 *  2. File is not in use ==> remove the file
	 * 
	 * We can get a directory in case 1 because it may have had lots of attributes,
	 * which need to get removed here.
	 */
	if (dataforkbusy || rsrcforkbusy || isbigfile || defer_remove) {
		char delname[32];
		struct cat_desc to_desc;
		struct cat_desc todir_desc;

		/*
		 * Orphan this file or directory (move to hidden directory).
		 * Again, we need to take care that we treat directories as directories,
		 * and files as files.  Because directories with attributes can be passed in
		 * check to make sure that we have a directory or a file before filling in the 
		 * temporary descriptor's flags.  We keep orphaned directories AND files in
		 * the FILE_HARDLINKS private directory since we're generalizing over all
		 * orphaned filesystem objects.
		 */
		bzero(&todir_desc, sizeof(todir_desc));
		todir_desc.cd_parentcnid = 2;

		MAKE_DELETED_NAME(delname, sizeof(delname), cp->c_fileid);
		bzero(&to_desc, sizeof(to_desc));
		to_desc.cd_nameptr = (const u_int8_t *)delname;
		to_desc.cd_namelen = strlen(delname);
		to_desc.cd_parentcnid = hfsmp->hfs_private_desc[FILE_HARDLINKS].cd_cnid;
		if (isdir) {
			to_desc.cd_flags = CD_ISDIR;
		}
		else {
			to_desc.cd_flags = 0;
		}
		to_desc.cd_cnid = cp->c_cnid;

		lockflags = hfs_systemfile_lock(hfsmp, SFL_CATALOG, HFS_EXCLUSIVE_LOCK);
		if (!skip_reserve) {
			if ((error = cat_preflight(hfsmp, CAT_RENAME, NULL, 0))) {
				hfs_systemfile_unlock(hfsmp, lockflags);
				goto out;
			}
		}

		error = cat_rename(hfsmp, &desc, &todir_desc,
				&to_desc, (struct cat_desc *)NULL);

		if (error == 0) {
			hfsmp->hfs_private_attr[FILE_HARDLINKS].ca_entries++;
			if (isdir == 1) {
				INC_FOLDERCOUNT(hfsmp, hfsmp->hfs_private_attr[FILE_HARDLINKS]);
			}
			(void) cat_update(hfsmp, &hfsmp->hfs_private_desc[FILE_HARDLINKS],
			                  &hfsmp->hfs_private_attr[FILE_HARDLINKS], NULL, NULL);

			/* Update the parent directory */
			if (dcp->c_entries > 0)
				dcp->c_entries--;
			if (isdir == 1) {
				DEC_FOLDERCOUNT(hfsmp, dcp->c_attr);
			}
			dcp->c_dirchangecnt++;
			dcp->c_ctime = tv.tv_sec;
			dcp->c_mtime = tv.tv_sec;
			(void) cat_update(hfsmp, &dcp->c_desc, &dcp->c_attr, NULL, NULL);

			/* Update the file or directory's state */
			cp->c_flag |= C_DELETED;
			cp->c_ctime = tv.tv_sec;
			--cp->c_linkcount;
			(void) cat_update(hfsmp, &to_desc, &cp->c_attr, NULL, NULL);
		}
		hfs_systemfile_unlock(hfsmp, lockflags);
		if (error)
			goto out;

	} else /* Not busy */ {

		if (cp->c_blocks > 0) {
			printf("hfs_remove: attempting to delete a non-empty file %s\n",
				cp->c_desc.cd_nameptr);
			error = EBUSY;
			goto out;
		}

		lockflags = hfs_systemfile_lock(hfsmp, SFL_CATALOG | SFL_ATTRIBUTE | SFL_BITMAP, HFS_EXCLUSIVE_LOCK);
		if (!skip_reserve) {
			if ((error = cat_preflight(hfsmp, CAT_DELETE, NULL, 0))) {
				hfs_systemfile_unlock(hfsmp, lockflags);
				goto out;
			}
		}

		error = cat_delete(hfsmp, &desc, &cp->c_attr);

		if (error && error != ENXIO && error != ENOENT && truncated) {
			if ((cp->c_datafork && cp->c_datafork->ff_size != 0) ||
					(cp->c_rsrcfork && cp->c_rsrcfork->ff_size != 0)) {
				off_t data_size = 0;
				off_t rsrc_size = 0;
				if (cp->c_datafork) {
					data_size = cp->c_datafork->ff_size;
				}
				if (cp->c_rsrcfork) {
					rsrc_size = cp->c_rsrcfork->ff_size;
				}
				printf("hfs: remove: couldn't delete a truncated file (%s)" 
						"(error %d, data sz %lld; rsrc sz %lld)",
					cp->c_desc.cd_nameptr, error, data_size, rsrc_size);
				hfs_mark_volume_inconsistent(hfsmp);
			} else {
				printf("hfs: remove: strangely enough, deleting truncated file %s (%d) got err %d\n",
						cp->c_desc.cd_nameptr, cp->c_attr.ca_fileid, error);
			}	
		}

		if (error == 0) {
			/* Update the parent directory */
			if (dcp->c_entries > 0)
				dcp->c_entries--;
			dcp->c_dirchangecnt++;
			dcp->c_ctime = tv.tv_sec;
			dcp->c_mtime = tv.tv_sec;
			(void) cat_update(hfsmp, &dcp->c_desc, &dcp->c_attr, NULL, NULL);
		}
		hfs_systemfile_unlock(hfsmp, lockflags);
		if (error)
			goto out;

#if QUOTA
		if (hfsmp->hfs_flags & HFS_QUOTAS)
			(void)hfs_chkiq(cp, -1, NOCRED, 0);
#endif /* QUOTA */

		cp->c_flag |= C_NOEXISTS;
		cp->c_flag &= ~C_DELETED;
		truncated = 0;  // because the catalog entry is gone

		cp->c_touch_chgtime = TRUE;   /* XXX needed ? */
		--cp->c_linkcount;

		/* 
		 * We must never get a directory if we're in this else block.  We could 
		 * accidentally drop the number of files in the volume header if we did.
		 */
		hfs_volupdate(hfsmp, VOL_RMFILE, (dcp->c_cnid == kHFSRootFolderID));
	}

	/*
	 * All done with this cnode's descriptor...
	 *
	 * Note: all future catalog calls for this cnode must be by
	 * fileid only.  This is OK for HFS (which doesn't have file
	 * thread records) since HFS doesn't support the removal of
	 * busy files.
	 */
	cat_releasedesc(&cp->c_desc);

out:
	if (error) {
	    cp->c_flag &= ~C_DELETED;
	}

	/* Commit the truncation to the catalog record */
	if (truncated) {
	    cp->c_flag |= C_FORCEUPDATE;
	    cp->c_touch_chgtime = TRUE;
	    cp->c_touch_modtime = TRUE;
	    (void) hfs_update(vp, 0);
	}

	if (started_tr) {
	    hfs_end_transaction(hfsmp);
	}

	dcp->c_flag &= ~C_DIR_MODIFICATION;
	wakeup((caddr_t)&dcp->c_flag);

	return (error);
}


__private_extern__ void
replace_desc(struct cnode *cp, struct cat_desc *cdp)
{
	// fixes 4348457 and 4463138
	if (&cp->c_desc == cdp) {
	    return;
	}

	/* First release allocated name buffer */
	if (cp->c_desc.cd_flags & CD_HASBUF && cp->c_desc.cd_nameptr != 0) {
		const u_int8_t *name = cp->c_desc.cd_nameptr;

		cp->c_desc.cd_nameptr = 0;
		cp->c_desc.cd_namelen = 0;
		cp->c_desc.cd_flags &= ~CD_HASBUF;
		vfs_removename((const char *)name);
	}
	bcopy(cdp, &cp->c_desc, sizeof(cp->c_desc));

	/* Cnode now owns the name buffer */
	cdp->cd_nameptr = 0;
	cdp->cd_namelen = 0;
	cdp->cd_flags &= ~CD_HASBUF;
}


/*
 * Rename a cnode.
 *
 * The VFS layer guarantees that:
 *   - source and destination will either both be directories, or
 *     both not be directories.
 *   - all the vnodes are from the same file system
 *
 * When the target is a directory, HFS must ensure that its empty.
 *
 * Note that this function requires up to 6 vnodes in order to work properly
 * if it is operating on files (and not on directories).  This is because only
 * files can have resource forks, and we now require iocounts to be held on the
 * vnodes corresponding to the resource forks (if applicable) as well as
 * the files or directories undergoing rename.  The problem with not holding 
 * iocounts on the resource fork vnodes is that it can lead to a deadlock 
 * situation: The rsrc fork of the source file may be recycled and reclaimed 
 * in order to provide a vnode for the destination file's rsrc fork.  Since
 * data and rsrc forks share the same cnode, we'd eventually try to lock the
 * source file's cnode in order to sync its rsrc fork to disk, but it's already 
 * been locked.  By taking the rsrc fork vnodes up front we ensure that they 
 * cannot be recycled, and that the situation mentioned above cannot happen.
 */
static int
hfs_vnop_rename(ap)
	struct vnop_rename_args  /* {
		struct vnode *a_fdvp;
		struct vnode *a_fvp;
		struct componentname *a_fcnp;
		struct vnode *a_tdvp;
		struct vnode *a_tvp;
		struct componentname *a_tcnp;
		vfs_context_t a_context;
	} */ *ap;
{
	struct vnode *tvp = ap->a_tvp;
	struct vnode *tdvp = ap->a_tdvp;
	struct vnode *fvp = ap->a_fvp;
	struct vnode *fdvp = ap->a_fdvp;
	struct vnode *fvp_rsrc = NULLVP;
	struct vnode *tvp_rsrc = NULLVP;
	struct componentname *tcnp = ap->a_tcnp;
	struct componentname *fcnp = ap->a_fcnp;
	struct proc *p = vfs_context_proc(ap->a_context);
	struct cnode *fcp;
	struct cnode *fdcp;
	struct cnode *tdcp;
	struct cnode *tcp;
	struct cnode *error_cnode;
	struct cat_desc from_desc;
	struct cat_desc to_desc;
	struct cat_desc out_desc;
	struct hfsmount *hfsmp;
	cat_cookie_t cookie;
	int tvp_deleted = 0;
	int started_tr = 0, got_cookie = 0;
	int took_trunc_lock = 0;
	int lockflags;
	int error;
	int recycle_rsrc = 0;


	/* 
	 * Before grabbing the four locks, we may need to get an iocount on the resource fork
	 * vnodes in question, just like hfs_vnop_remove.  If fvp and tvp are not
	 * directories, then go ahead and grab the resource fork vnodes now
	 * one at a time.  We don't actively need the fvp_rsrc to do the rename operation,
	 * but we need the iocount to prevent the vnode from getting recycled/reclaimed
	 * during the middle of the VNOP.
	 */


	if ((vnode_isreg(fvp)) || (vnode_islnk(fvp))) {

		if ((error = hfs_lock (VTOC(fvp), HFS_EXCLUSIVE_LOCK))) {
			return (error);
		}
		
		/*
		 * We care if we race against rename/delete with this cnode, so we'll
		 * error out if this file becomes open-unlinked during this call.
		 */
		error = hfs_vgetrsrc(VTOHFS(fvp), fvp, &fvp_rsrc, TRUE, TRUE);
		hfs_unlock (VTOC(fvp));
		if (error) {
			if (fvp_rsrc) {
				vnode_put (fvp_rsrc);
			}
			return error;
		}
	}
		
	if (tvp && (vnode_isreg(tvp) || vnode_islnk(tvp))) {
		/* 
		 * Lock failure is OK on tvp, since we may race with a remove on the dst.
		 * But this shouldn't stop rename from proceeding, so only try to
		 * grab the resource fork if the lock succeeded.
		 */
		if (hfs_lock (VTOC(tvp), HFS_EXCLUSIVE_LOCK) == 0) {
			tcp = VTOC(tvp);
			
			/* 
			 * We only care if we get an open-unlinked file on the dst so we 
			 * know to null out tvp/tcp to make the rename operation act 
			 * as if they never existed.  Because they're effectively out of the
			 * namespace already it's fine to do this.  If this is true, then
			 * make sure to unlock the cnode and drop the iocount only after the unlock.
			 */
			error = hfs_vgetrsrc(VTOHFS(tvp), tvp, &tvp_rsrc, TRUE, TRUE);
			hfs_unlock (tcp);
			if (error) {
				/*
				 * Since we specify TRUE for error-on-unlinked in hfs_vgetrsrc,
				 * we can get a rsrc fork vp even if it returns an error.
				 */
				tcp = NULL;
				tvp = NULL;
				if (tvp_rsrc) {
					vnode_put (tvp_rsrc);
					tvp_rsrc = NULLVP;
				}
				/* just bypass truncate lock and act as if we never got tcp/tvp */
				goto retry;
			}
		}
	}

	/* When tvp exists, take the truncate lock for hfs_removefile(). */
	if (tvp && (vnode_isreg(tvp) || vnode_islnk(tvp))) {
		hfs_lock_truncate(VTOC(tvp), TRUE);
		took_trunc_lock = 1;
	}

  retry:
	error = hfs_lockfour(VTOC(fdvp), VTOC(fvp), VTOC(tdvp), tvp ? VTOC(tvp) : NULL,
	                     HFS_EXCLUSIVE_LOCK, &error_cnode);
	if (error) {
		if (took_trunc_lock) {
			hfs_unlock_truncate(VTOC(tvp), TRUE);
			took_trunc_lock = 0;
		}
		/* 
		 * tvp might no longer exist.  If the cause of the lock failure 
		 * was tvp, then we can try again with tvp/tcp set to NULL.  
		 * This is ok because the vfs syscall will vnode_put the vnodes 
		 * after we return from hfs_vnop_rename.
		 */
		if ((error == ENOENT) && (tvp != NULL) && (error_cnode == VTOC(tvp))) {	
			tcp = NULL;
			tvp = NULL;
			goto retry;
		}
		/* otherwise, drop iocounts on the rsrc forks and bail out */
		if (fvp_rsrc) {
			vnode_put (fvp_rsrc);
		}
		if (tvp_rsrc) {
			vnode_put (tvp_rsrc);
		}
		return (error);
	}

	fdcp = VTOC(fdvp);
	fcp = VTOC(fvp);
	tdcp = VTOC(tdvp);
	tcp = tvp ? VTOC(tvp) : NULL;
	hfsmp = VTOHFS(tdvp);

	/* Ensure we didn't race src or dst parent directories with rmdir. */
	if (fdcp->c_flag & (C_NOEXISTS | C_DELETED)) {
		error = ENOENT;
		goto out;
	}

	if (tdcp->c_flag & (C_NOEXISTS | C_DELETED)) {
		error = ENOENT;
		goto out;	
	}


	/* Check for a race against unlink.  The hfs_valid_cnode checks validate
	 * the parent/child relationship with fdcp and tdcp, as well as the
	 * component name of the target cnodes.  
	 */
	if ((fcp->c_flag & (C_NOEXISTS | C_DELETED)) || !hfs_valid_cnode(hfsmp, fdvp, fcnp, fcp->c_fileid)) {
		error = ENOENT;
		goto out;
	}

	if (tcp && ((tcp->c_flag & (C_NOEXISTS | C_DELETED)) || !hfs_valid_cnode(hfsmp, tdvp, tcnp, tcp->c_fileid))) {
	    //
	    // hmm, the destination vnode isn't valid any more.
	    // in this case we can just drop him and pretend he
	    // never existed in the first place.
	    //
	    if (took_trunc_lock) {
		hfs_unlock_truncate(VTOC(tvp), TRUE);
		took_trunc_lock = 0;
	    }

	    hfs_unlockfour(fdcp, fcp, tdcp, tcp);

	    tcp = NULL;
	    tvp = NULL;
	    
	    // retry the locking with tvp null'ed out
	    goto retry;
	}

	fdcp->c_flag |= C_DIR_MODIFICATION;
	if (fdvp != tdvp) {
	    tdcp->c_flag |= C_DIR_MODIFICATION;
	}

	/*
	 * Disallow renaming of a directory hard link if the source and 
	 * destination parent directories are different, or a directory whose 
	 * descendant is a directory hard link and the one of the ancestors
	 * of the destination directory is a directory hard link.
	 */
	if (vnode_isdir(fvp) && (fdvp != tdvp)) {
		if (fcp->c_flag & C_HARDLINK) {
			error = EPERM;
			goto out;
		}
		if (fcp->c_attr.ca_recflags & kHFSHasChildLinkMask) {
		    lockflags = hfs_systemfile_lock(hfsmp, SFL_CATALOG, HFS_SHARED_LOCK);
		    if (cat_check_link_ancestry(hfsmp, tdcp->c_fileid, 0)) {
				error = EPERM;
				hfs_systemfile_unlock(hfsmp, lockflags);
				goto out;
			}
			hfs_systemfile_unlock(hfsmp, lockflags);
		}
	}

	/*
	 * The following edge case is caught here:
	 * (to cannot be a descendent of from)
	 *
	 *       o fdvp
	 *      /
	 *     /
	 *    o fvp
	 *     \
	 *      \
	 *       o tdvp
	 *      /
	 *     /
	 *    o tvp
	 */
	if (tdcp->c_parentcnid == fcp->c_fileid) {
		error = EINVAL;
		goto out;
	}

	/*
	 * The following two edge cases are caught here:
	 * (note tvp is not empty)
	 *
	 *       o tdvp               o tdvp
	 *      /                    /
	 *     /                    /
	 *    o tvp            tvp o fdvp
	 *     \                    \
	 *      \                    \
	 *       o fdvp               o fvp
	 *      /
	 *     /
	 *    o fvp
	 */
	if (tvp && vnode_isdir(tvp) && (tcp->c_entries != 0) && fvp != tvp) {
		error = ENOTEMPTY;
		goto out;
	}

	/*
	 * The following edge case is caught here:
	 * (the from child and parent are the same)
	 *
	 *          o tdvp
	 *         /
	 *        /
	 *  fdvp o fvp
	 */
	if (fdvp == fvp) {
		error = EINVAL;
		goto out;
	}

	/*
	 * Make sure "from" vnode and its parent are changeable.
	 */
	if ((fcp->c_flags & (IMMUTABLE | APPEND)) || (fdcp->c_flags & APPEND)) {
		error = EPERM;
		goto out;
	}

	/*
	 * If the destination parent directory is "sticky", then the
	 * user must own the parent directory, or the destination of
	 * the rename, otherwise the destination may not be changed
	 * (except by root). This implements append-only directories.
	 *
	 * Note that checks for immutable and write access are done
	 * by the call to hfs_removefile.
	 */
	if (tvp && (tdcp->c_mode & S_ISTXT) &&
	    (suser(vfs_context_ucred(tcnp->cn_context), NULL)) &&
	    (kauth_cred_getuid(vfs_context_ucred(tcnp->cn_context)) != tdcp->c_uid) &&
	    (hfs_owner_rights(hfsmp, tcp->c_uid, vfs_context_ucred(tcnp->cn_context), p, false)) ) {
		error = EPERM;
		goto out;
	}

#if QUOTA
	if (tvp)
		(void)hfs_getinoquota(tcp);
#endif
	/* Preflighting done, take fvp out of the name space. */
	cache_purge(fvp);

	bzero(&from_desc, sizeof(from_desc));
	from_desc.cd_nameptr = (const u_int8_t *)fcnp->cn_nameptr;
	from_desc.cd_namelen = fcnp->cn_namelen;
	from_desc.cd_parentcnid = fdcp->c_fileid;
	from_desc.cd_flags = fcp->c_desc.cd_flags & ~(CD_HASBUF | CD_DECOMPOSED);
	from_desc.cd_cnid = fcp->c_cnid;

	bzero(&to_desc, sizeof(to_desc));
	to_desc.cd_nameptr = (const u_int8_t *)tcnp->cn_nameptr;
	to_desc.cd_namelen = tcnp->cn_namelen;
	to_desc.cd_parentcnid = tdcp->c_fileid;
	to_desc.cd_flags = fcp->c_desc.cd_flags & ~(CD_HASBUF | CD_DECOMPOSED);
	to_desc.cd_cnid = fcp->c_cnid;

	if ((error = hfs_start_transaction(hfsmp)) != 0) {
	    goto out;
	}
	started_tr = 1;

	/* hfs_vnop_link() and hfs_vnop_rename() set kHFSHasChildLinkMask 
	 * inside a journal transaction and without holding a cnode lock.  
	 * As setting of this bit depends on being in journal transaction for 
	 * concurrency, check this bit again after we start journal transaction for rename
	 * to ensure that this directory does not have any descendant that
	 * is a directory hard link. 
	 */
	if (vnode_isdir(fvp) && (fdvp != tdvp)) {
		if (fcp->c_attr.ca_recflags & kHFSHasChildLinkMask) {
		    lockflags = hfs_systemfile_lock(hfsmp, SFL_CATALOG, HFS_SHARED_LOCK);
		    if (cat_check_link_ancestry(hfsmp, tdcp->c_fileid, 0)) {
				error = EPERM;
				hfs_systemfile_unlock(hfsmp, lockflags);
				goto out;
			}
			hfs_systemfile_unlock(hfsmp, lockflags);
		}
	}

	// if it's a hardlink then re-lookup the name so
	// that we get the correct cnid in from_desc (see
	// the comment in hfs_removefile for more details)
	//
	if (fcp->c_flag & C_HARDLINK) {
	    struct cat_desc tmpdesc;
	    cnid_t real_cnid;

	    tmpdesc.cd_nameptr = (const u_int8_t *)fcnp->cn_nameptr;
	    tmpdesc.cd_namelen = fcnp->cn_namelen;
	    tmpdesc.cd_parentcnid = fdcp->c_fileid;
	    tmpdesc.cd_hint = fdcp->c_childhint;
	    tmpdesc.cd_flags = fcp->c_desc.cd_flags & CD_ISDIR;
	    tmpdesc.cd_encoding = 0;
	    
	    lockflags = hfs_systemfile_lock(hfsmp, SFL_CATALOG, HFS_SHARED_LOCK);

	    if (cat_lookup(hfsmp, &tmpdesc, 0, NULL, NULL, NULL, &real_cnid) != 0) {
		hfs_systemfile_unlock(hfsmp, lockflags);
		goto out;
	    }

	    // use the real cnid instead of whatever happened to be there
	    from_desc.cd_cnid = real_cnid;
	    hfs_systemfile_unlock(hfsmp, lockflags);
	}

	/*
	 * Reserve some space in the Catalog file.
	 */
	if ((error = cat_preflight(hfsmp, CAT_RENAME + CAT_DELETE, &cookie, p))) {
		goto out;
	}
	got_cookie = 1;

	/*
	 * If the destination exists then it may need to be removed.
	 */
	if (tvp) {
		/*
		 * When fvp matches tvp they could be case variants
		 * or matching hard links.
		 */
		if (fvp == tvp) {
			if (!(fcp->c_flag & C_HARDLINK)) {
				goto skip_rm;  /* simple case variant */

			} else if ((fdvp != tdvp) ||
			           (hfsmp->hfs_flags & HFS_CASE_SENSITIVE)) {
				goto out;  /* matching hardlinks, nothing to do */

			} else if (hfs_namecmp((const u_int8_t *)fcnp->cn_nameptr, fcnp->cn_namelen,
			                       (const u_int8_t *)tcnp->cn_nameptr, tcnp->cn_namelen) == 0) {
				goto skip_rm;  /* case-variant hardlink in the same dir */
			} else {
				goto out;  /* matching hardlink, nothing to do */
			}
		}

		if (vnode_isdir(tvp))
			error = hfs_removedir(tdvp, tvp, tcnp, HFSRM_SKIP_RESERVE);
		else {
			error = hfs_removefile(tdvp, tvp, tcnp, 0, HFSRM_SKIP_RESERVE, 0, tvp_rsrc);

			/* 
			 * If the destination file had a rsrc fork vnode, it may have been cleaned up
			 * in hfs_removefile if it was not busy (had no usecounts).  This is possible
			 * because we grabbed the iocount on the rsrc fork safely at the beginning
			 * of the function before we did the lockfour.  However, we may still need
			 * to take action to prevent block leaks, so aggressively recycle the vnode
			 * if possible.  The vnode cannot be recycled because we hold an iocount on it.
			 */

			if ((error == 0) && (tcp->c_flag & C_DELETED) && tvp_rsrc && !vnode_isinuse(tvp_rsrc, 0)) {
				recycle_rsrc = 1;
			}	
		}

		if (error)
			goto out;
		tvp_deleted = 1;
	}
skip_rm:
	/*
	 * All done with tvp and fvp. 
	 * 
	 * We also jump to this point if there was no destination observed during lookup and namei.
	 * However, because only iocounts are held at the VFS layer, there is nothing preventing a 
	 * competing thread from racing us and creating a file or dir at the destination of this rename 
	 * operation.  If this occurs, it may cause us to get a spurious EEXIST out of the cat_rename 
	 * call below.  To preserve rename's atomicity, we need to signal VFS to re-drive the 
	 * namei/lookup and restart the rename operation.  EEXIST is an allowable errno to be bubbled 
	 * out of the rename syscall, but not for this reason, since it is a synonym errno for ENOTEMPTY.
	 * To signal VFS, we return ERECYCLE (which is also used for lookup restarts). This errno
	 * will be swallowed and it will restart the operation.
	 */
	
	lockflags = hfs_systemfile_lock(hfsmp, SFL_CATALOG, HFS_EXCLUSIVE_LOCK);
	error = cat_rename(hfsmp, &from_desc, &tdcp->c_desc, &to_desc, &out_desc);
	hfs_systemfile_unlock(hfsmp, lockflags);

	if (error) {
		if (error == EEXIST) {
			error = ERECYCLE;
		}
		goto out;
	}

	/* Invalidate negative cache entries in the destination directory */
	if (tdcp->c_flag & C_NEG_ENTRIES) {
		cache_purge_negatives(tdvp);
		tdcp->c_flag &= ~C_NEG_ENTRIES;
	}

	/* Update cnode's catalog descriptor */
	replace_desc(fcp, &out_desc);
	fcp->c_parentcnid = tdcp->c_fileid;
	fcp->c_hint = 0;

	hfs_volupdate(hfsmp, vnode_isdir(fvp) ? VOL_RMDIR : VOL_RMFILE,
	              (fdcp->c_cnid == kHFSRootFolderID));
	hfs_volupdate(hfsmp, vnode_isdir(fvp) ? VOL_MKDIR : VOL_MKFILE,
	              (tdcp->c_cnid == kHFSRootFolderID));

	/* Update both parent directories. */
	if (fdvp != tdvp) {
		if (vnode_isdir(fvp)) {
			/* If the source directory has directory hard link 
			 * descendants, set the kHFSHasChildLinkBit in the 
			 * destination parent hierarchy 
			 */
			if ((fcp->c_attr.ca_recflags & kHFSHasChildLinkMask) && 
			    !(tdcp->c_attr.ca_recflags & kHFSHasChildLinkMask)) {

				tdcp->c_attr.ca_recflags |= kHFSHasChildLinkMask;

				error = cat_set_childlinkbit(hfsmp, tdcp->c_parentcnid);
				if (error) {
					printf ("hfs_vnop_rename: error updating parent chain for %u\n", tdcp->c_cnid);
					error = 0;
				}
			}
			INC_FOLDERCOUNT(hfsmp, tdcp->c_attr);
			DEC_FOLDERCOUNT(hfsmp, fdcp->c_attr);
		}
		tdcp->c_entries++;
		tdcp->c_dirchangecnt++;
		if (fdcp->c_entries > 0)
			fdcp->c_entries--;
		fdcp->c_dirchangecnt++;
		fdcp->c_touch_chgtime = TRUE;
		fdcp->c_touch_modtime = TRUE;

		fdcp->c_flag |= C_FORCEUPDATE;  // XXXdbg - force it out!
		(void) hfs_update(fdvp, 0);
	}
	tdcp->c_childhint = out_desc.cd_hint;	/* Cache directory's location */
	tdcp->c_touch_chgtime = TRUE;
	tdcp->c_touch_modtime = TRUE;

	tdcp->c_flag |= C_FORCEUPDATE;  // XXXdbg - force it out!
	(void) hfs_update(tdvp, 0);
out:
	if (got_cookie) {
		cat_postflight(hfsmp, &cookie, p);
	}
	if (started_tr) {
	    hfs_end_transaction(hfsmp);
	}

	fdcp->c_flag &= ~C_DIR_MODIFICATION;
	wakeup((caddr_t)&fdcp->c_flag);
	if (fdvp != tdvp) {
	    tdcp->c_flag &= ~C_DIR_MODIFICATION;
	    wakeup((caddr_t)&tdcp->c_flag);
	}

	if (took_trunc_lock)
		hfs_unlock_truncate(VTOC(tvp), TRUE);	

	hfs_unlockfour(fdcp, fcp, tdcp, tcp);
	
	/* 
	 * Now that we've dropped all of the locks, we need to force an inactive and a recycle 
	 * on the old destination's rsrc fork to prevent a leak of its blocks.  Note that
	 * doing the ref/rele is to twiddle the VL_NEEDINACTIVE bit of the vnode's flags, so that
	 * on the last vnode_put for this vnode, we will force inactive to get triggered.
	 * We hold an iocount from the beginning of this function so we know it couldn't have been
	 * recycled already. 
	 */
	if (recycle_rsrc) {
		int vref; 
		vref = vnode_ref(tvp_rsrc);
		if (vref == 0) {
			vnode_rele(tvp_rsrc);
		}
		vnode_recycle(tvp_rsrc);
	}

	/* Now vnode_put the resource forks vnodes if necessary */
	if (tvp_rsrc) {
		vnode_put(tvp_rsrc);
	}
	if (fvp_rsrc) {
		vnode_put(fvp_rsrc);
	}

	/* After tvp is removed the only acceptable error is EIO */
	if (error && tvp_deleted)
		error = EIO;

	return (error);
}


/*
 * Make a directory.
 */
static int
hfs_vnop_mkdir(struct vnop_mkdir_args *ap)
{
	/***** HACK ALERT ********/
	ap->a_cnp->cn_flags |= MAKEENTRY;
	return hfs_makenode(ap->a_dvp, ap->a_vpp, ap->a_cnp, ap->a_vap, ap->a_context);
}


/*
 * Create a symbolic link.
 */
static int
hfs_vnop_symlink(struct vnop_symlink_args *ap)
{
	struct vnode **vpp = ap->a_vpp;
	struct vnode *dvp = ap->a_dvp;
	struct vnode *vp = NULL;
	struct cnode *cp = NULL;
	struct hfsmount *hfsmp;
	struct filefork *fp;
	struct buf *bp = NULL;
	char *datap;
	int started_tr = 0;
	u_int32_t len;
	int error;

	/* HFS standard disks don't support symbolic links */
	if (VTOVCB(dvp)->vcbSigWord != kHFSPlusSigWord)
		return (ENOTSUP);

	/* Check for empty target name */
	if (ap->a_target[0] == 0)
		return (EINVAL);

	hfsmp = VTOHFS(dvp);
	len = strlen(ap->a_target);

	/* Check for free space */
	if (((u_int64_t)hfs_freeblks(hfsmp, 0) * (u_int64_t)hfsmp->blockSize) < len) {
		return (ENOSPC);
	}

	/* Create the vnode */
	ap->a_vap->va_mode |= S_IFLNK;
	if ((error = hfs_makenode(dvp, vpp, ap->a_cnp, ap->a_vap, ap->a_context))) {
		goto out;
	}
	vp = *vpp;
	if ((error = hfs_lock(VTOC(vp), HFS_EXCLUSIVE_LOCK))) {
		goto out;
	}
	cp = VTOC(vp);
	fp = VTOF(vp);

	if (cp->c_flag & (C_NOEXISTS | C_DELETED)) {
	    goto out;
	}

#if QUOTA
	(void)hfs_getinoquota(cp);
#endif /* QUOTA */

	if ((error = hfs_start_transaction(hfsmp)) != 0) {
	    goto out;
	}
	started_tr = 1;

	/*
	 * Allocate space for the link.
	 *
	 * Since we're already inside a transaction,
	 * tell hfs_truncate to skip the ubc_setsize.
	 *
	 * Don't need truncate lock since a symlink is treated as a system file.
	 */
	error = hfs_truncate(vp, len, IO_NOZEROFILL, 1, 0, ap->a_context);

	/* On errors, remove the symlink file */
	if (error) {
		/*
		 * End the transaction so we don't re-take the cnode lock
		 * below while inside a transaction (lock order violation).
		 */
		hfs_end_transaction(hfsmp);

		/* hfs_removefile() requires holding the truncate lock */
		hfs_unlock(cp);
		hfs_lock_truncate(cp, TRUE);
		hfs_lock(cp, HFS_FORCE_LOCK);

		if (hfs_start_transaction(hfsmp) != 0) {
			started_tr = 0;
			hfs_unlock_truncate(cp, TRUE);
			goto out;
		}
		
		(void) hfs_removefile(dvp, vp, ap->a_cnp, 0, 0, 0, NULL);
		hfs_unlock_truncate(cp, TRUE);
		goto out;	
	}

	/* Write the link to disk */
	bp = buf_getblk(vp, (daddr64_t)0, roundup((int)fp->ff_size, hfsmp->hfs_physical_block_size),
			0, 0, BLK_META);
	if (hfsmp->jnl) {
		journal_modify_block_start(hfsmp->jnl, bp);
	}
	datap = (char *)buf_dataptr(bp);
	bzero(datap, buf_size(bp));
	bcopy(ap->a_target, datap, len);

	if (hfsmp->jnl) {
		journal_modify_block_end(hfsmp->jnl, bp, NULL, NULL);
	} else {
		buf_bawrite(bp);
	}
	/*
	 * We defered the ubc_setsize for hfs_truncate
	 * since we were inside a transaction.
	 *
	 * We don't need to drop the cnode lock here
	 * since this is a symlink.
	 */
	ubc_setsize(vp, len);
out:
	if (started_tr)
	    hfs_end_transaction(hfsmp);
	if ((cp != NULL) && (vp != NULL)) {
		hfs_unlock(cp);
	}
	if (error) {
		if (vp) {
			vnode_put(vp);
		}
		*vpp = NULL;
	}
	return (error);
}


/* structures to hold a "." or ".." directory entry */
struct hfs_stddotentry {
	u_int32_t	d_fileno;   /* unique file number */
	u_int16_t	d_reclen;   /* length of this structure */
	u_int8_t	d_type;     /* dirent file type */
	u_int8_t	d_namlen;   /* len of filename */
	char		d_name[4];  /* "." or ".." */
};

struct hfs_extdotentry {
	u_int64_t  d_fileno;   /* unique file number */
	u_int64_t  d_seekoff;  /* seek offset (optional, used by servers) */
	u_int16_t  d_reclen;   /* length of this structure */
	u_int16_t  d_namlen;   /* len of filename */
	u_int8_t   d_type;     /* dirent file type */
	u_char     d_name[3];  /* "." or ".." */
};

typedef union {
	struct hfs_stddotentry  std;
	struct hfs_extdotentry  ext;
} hfs_dotentry_t;

/*
 *  hfs_vnop_readdir reads directory entries into the buffer pointed
 *  to by uio, in a filesystem independent format.  Up to uio_resid
 *  bytes of data can be transferred.  The data in the buffer is a
 *  series of packed dirent structures where each one contains the
 *  following entries:
 *
 *	u_int32_t   d_fileno;              // file number of entry
 *	u_int16_t   d_reclen;              // length of this record
 *	u_int8_t    d_type;                // file type
 *	u_int8_t    d_namlen;              // length of string in d_name
 *	char        d_name[MAXNAMELEN+1];  // null terminated file name
 *
 *  The current position (uio_offset) refers to the next block of
 *  entries.  The offset can only be set to a value previously
 *  returned by hfs_vnop_readdir or zero.  This offset does not have
 *  to match the number of bytes returned (in uio_resid).
 *
 *  In fact, the offset used by HFS is essentially an index (26 bits)
 *  with a tag (6 bits).  The tag is for associating the next request
 *  with the current request.  This enables us to have multiple threads
 *  reading the directory while the directory is also being modified.
 *
 *  Each tag/index pair is tied to a unique directory hint.  The hint
 *  contains information (filename) needed to build the catalog b-tree
 *  key for finding the next set of entries.
 *
 * If the directory is marked as deleted-but-in-use (cp->c_flag & C_DELETED),
 * do NOT synthesize entries for "." and "..".
 */
static int
hfs_vnop_readdir(ap)
	struct vnop_readdir_args /* {
		vnode_t a_vp;
		uio_t a_uio;
		int a_flags;
		int *a_eofflag;
		int *a_numdirent;
		vfs_context_t a_context;
	} */ *ap;
{
	struct vnode *vp = ap->a_vp;
	uio_t uio = ap->a_uio;
	struct cnode *cp;
	struct hfsmount *hfsmp;
	directoryhint_t *dirhint = NULL;
	directoryhint_t localhint;
	off_t offset;
	off_t startoffset;
	int error = 0;
	int eofflag = 0;
	user_addr_t user_start = 0;
	user_size_t user_len = 0;
	int index;
	unsigned int tag;
	int items;
	int lockflags;
	int extended;
	int nfs_cookies;
	cnid_t cnid_hint = 0;

	items = 0;
	startoffset = offset = uio_offset(uio);
	extended = (ap->a_flags & VNODE_READDIR_EXTENDED);
	nfs_cookies = extended && (ap->a_flags & VNODE_READDIR_REQSEEKOFF);

	/* Sanity check the uio data. */
	if (uio_iovcnt(uio) > 1)
		return (EINVAL);
	/* Note that the dirhint calls require an exclusive lock. */
	if ((error = hfs_lock(VTOC(vp), HFS_EXCLUSIVE_LOCK)))
		return (error);
	cp = VTOC(vp);
	hfsmp = VTOHFS(vp);

	/* Pick up cnid hint (if any). */
	if (nfs_cookies) {
		cnid_hint = (cnid_t)(uio_offset(uio) >> 32);
		uio_setoffset(uio, uio_offset(uio) & 0x00000000ffffffffLL);
		if (cnid_hint == INT_MAX) { /* searching pass the last item */
			eofflag = 1;
			goto out;
		}
	}
	/*
	 * Synthesize entries for "." and "..", unless the directory has
	 * been deleted, but not closed yet (lazy delete in progress).
	 */
	if (offset == 0 && !(cp->c_flag & C_DELETED)) {
		hfs_dotentry_t  dotentry[2];
		size_t  uiosize;

		if (extended) {
			struct hfs_extdotentry *entry = &dotentry[0].ext;

			entry->d_fileno = cp->c_cnid;
			entry->d_reclen = sizeof(struct hfs_extdotentry);
			entry->d_type = DT_DIR;
			entry->d_namlen = 1;
			entry->d_name[0] = '.';
			entry->d_name[1] = '\0';
			entry->d_name[2] = '\0';
			entry->d_seekoff = 1;

			++entry;
			entry->d_fileno = cp->c_parentcnid;
			entry->d_reclen = sizeof(struct hfs_extdotentry);
			entry->d_type = DT_DIR;
			entry->d_namlen = 2;
			entry->d_name[0] = '.';
			entry->d_name[1] = '.';
			entry->d_name[2] = '\0';
			entry->d_seekoff = 2;
			uiosize = 2 * sizeof(struct hfs_extdotentry);
		} else {
			struct hfs_stddotentry *entry = &dotentry[0].std;

			entry->d_fileno = cp->c_cnid;
			entry->d_reclen = sizeof(struct hfs_stddotentry);
			entry->d_type = DT_DIR;
			entry->d_namlen = 1;
			*(int *)&entry->d_name[0] = 0;
			entry->d_name[0] = '.';

			++entry;
			entry->d_fileno = cp->c_parentcnid;
			entry->d_reclen = sizeof(struct hfs_stddotentry);
			entry->d_type = DT_DIR;
			entry->d_namlen = 2;
			*(int *)&entry->d_name[0] = 0;
			entry->d_name[0] = '.';
			entry->d_name[1] = '.';
			uiosize = 2 * sizeof(struct hfs_stddotentry);
		}
		if ((error = uiomove((caddr_t)&dotentry, uiosize, uio))) {
			goto out;
		}
		offset += 2;
	}

	/* If there are no real entries then we're done. */
	if (cp->c_entries == 0) {
		error = 0;
		eofflag = 1;
		uio_setoffset(uio, offset);
		goto seekoffcalc;
	}

	//
	// We have to lock the user's buffer here so that we won't
	// fault on it after we've acquired a shared lock on the
	// catalog file.  The issue is that you can get a 3-way
	// deadlock if someone else starts a transaction and then
	// tries to lock the catalog file but can't because we're
	// here and we can't service our page fault because VM is
	// blocked trying to start a transaction as a result of
	// trying to free up pages for our page fault.  It's messy
	// but it does happen on dual-processors that are paging
	// heavily (see radar 3082639 for more info).  By locking
	// the buffer up-front we prevent ourselves from faulting
	// while holding the shared catalog file lock.
	//
	// Fortunately this and hfs_search() are the only two places
	// currently (10/30/02) that can fault on user data with a
	// shared lock on the catalog file.
	//
	if (hfsmp->jnl && uio_isuserspace(uio)) {
		user_start = uio_curriovbase(uio);
		user_len = uio_curriovlen(uio);

		if ((error = vslock(user_start, user_len)) != 0) {
			user_start = 0;
			goto out;
		}
	}
	/* Convert offset into a catalog directory index. */
	index = (offset & HFS_INDEX_MASK) - 2;
	tag = offset & ~HFS_INDEX_MASK;

	/* Lock catalog during cat_findname and cat_getdirentries. */
	lockflags = hfs_systemfile_lock(hfsmp, SFL_CATALOG, HFS_SHARED_LOCK);

	/* When called from NFS, try and resolve a cnid hint. */
	if (nfs_cookies && cnid_hint != 0) {
		if (cat_findname(hfsmp, cnid_hint, &localhint.dh_desc) == 0) {
			if ( localhint.dh_desc.cd_parentcnid == cp->c_fileid) {
				localhint.dh_index = index - 1;
				localhint.dh_time = 0;
				bzero(&localhint.dh_link, sizeof(localhint.dh_link));
				dirhint = &localhint;  /* don't forget to release the descriptor */
			} else {
				cat_releasedesc(&localhint.dh_desc);
			}
		}
	}

	/* Get a directory hint (cnode must be locked exclusive) */
	if (dirhint == NULL) {
		dirhint = hfs_getdirhint(cp, ((index - 1) & HFS_INDEX_MASK) | tag, 0);

		/* Hide tag from catalog layer. */
		dirhint->dh_index &= HFS_INDEX_MASK;
		if (dirhint->dh_index == HFS_INDEX_MASK) {
			dirhint->dh_index = -1;
		}
	}
	
	if (index == 0) {
		dirhint->dh_threadhint = cp->c_dirthreadhint;
	}
	else {
		/*
		 * If we have a non-zero index, there is a possibility that during the last
		 * call to hfs_vnop_readdir we hit EOF for this directory.  If that is the case
		 * then we don't want to return any new entries for the caller.  Just return 0
		 * items, mark the eofflag, and bail out.  Because we won't have done any work, the 
		 * code at the end of the function will release the dirhint for us.  
		 *
		 * Don't forget to unlock the catalog lock on the way out, too.
		 */
		if (dirhint->dh_desc.cd_flags & CD_EOF) {
			error = 0;
			eofflag = 1;
			uio_setoffset(uio, startoffset);
			hfs_systemfile_unlock (hfsmp, lockflags);

			goto seekoffcalc;
		}
	}

	/* Pack the buffer with dirent entries. */
	error = cat_getdirentries(hfsmp, cp->c_entries, dirhint, uio, extended, &items, &eofflag);

	if (index == 0 && error == 0) {
		cp->c_dirthreadhint = dirhint->dh_threadhint;
	}

	hfs_systemfile_unlock(hfsmp, lockflags);

	if (error != 0) {
		goto out;
	}
	
	/* Get index to the next item */
	index += items;
	
	if (items >= (int)cp->c_entries) {
		eofflag = 1;
	}

	/* Convert catalog directory index back into an offset. */
	while (tag == 0)
		tag = (++cp->c_dirhinttag) << HFS_INDEX_BITS;	
	uio_setoffset(uio, (index + 2) | tag);
	dirhint->dh_index |= tag;

seekoffcalc:
	cp->c_touch_acctime = TRUE;

	if (ap->a_numdirent) {
		if (startoffset == 0)
			items += 2;
		*ap->a_numdirent = items;
	}

out:
	if (user_start) {
		vsunlock(user_start, user_len, TRUE);
	}
	/* If we didn't do anything then go ahead and dump the hint. */
	if ((dirhint != NULL) &&
	    (dirhint != &localhint) &&
	    (uio_offset(uio) == startoffset)) {
		hfs_reldirhint(cp, dirhint);
		eofflag = 1;
	}
	if (ap->a_eofflag) {
		*ap->a_eofflag = eofflag;
	}
	if (dirhint == &localhint) {
		cat_releasedesc(&localhint.dh_desc);
	}
	hfs_unlock(cp);
	return (error);
}


/*
 * Read contents of a symbolic link.
 */
static int
hfs_vnop_readlink(ap)
	struct vnop_readlink_args /* {
		struct vnode *a_vp;
		struct uio *a_uio;
		vfs_context_t a_context;
	} */ *ap;
{
	struct vnode *vp = ap->a_vp;
	struct cnode *cp;
	struct filefork *fp;
	int error;

	if (!vnode_islnk(vp))
		return (EINVAL);
 
	if ((error = hfs_lock(VTOC(vp), HFS_EXCLUSIVE_LOCK)))
		return (error);
	cp = VTOC(vp);
	fp = VTOF(vp);
   
	/* Zero length sym links are not allowed */
	if (fp->ff_size == 0 || fp->ff_size > MAXPATHLEN) {
		printf("hfs: zero length symlink on fileid %d\n", cp->c_fileid);
		error = EINVAL;
		goto exit;
	}
    
	/* Cache the path so we don't waste buffer cache resources */
	if (fp->ff_symlinkptr == NULL) {
		struct buf *bp = NULL;

		MALLOC(fp->ff_symlinkptr, char *, fp->ff_size, M_TEMP, M_WAITOK);
		if (fp->ff_symlinkptr == NULL) {
			error = ENOMEM;
			goto exit;
		}
		error = (int)buf_meta_bread(vp, (daddr64_t)0,
		                            roundup((int)fp->ff_size, VTOHFS(vp)->hfs_physical_block_size),
		                            vfs_context_ucred(ap->a_context), &bp);
		if (error) {
			if (bp)
				buf_brelse(bp);
			if (fp->ff_symlinkptr) {
				FREE(fp->ff_symlinkptr, M_TEMP);
				fp->ff_symlinkptr = NULL;
			}
			goto exit;
		}
		bcopy((char *)buf_dataptr(bp), fp->ff_symlinkptr, (size_t)fp->ff_size);

		if (VTOHFS(vp)->jnl && (buf_flags(bp) & B_LOCKED) == 0) {
		        buf_markinvalid(bp);		/* data no longer needed */
		}
		buf_brelse(bp);
	}
	error = uiomove((caddr_t)fp->ff_symlinkptr, (int)fp->ff_size, ap->a_uio);

	/*
	 * Keep track blocks read
	 */
	if ((VTOHFS(vp)->hfc_stage == HFC_RECORDING) && (error == 0)) {
		
		/*
		 * If this file hasn't been seen since the start of
		 * the current sampling period then start over.
		 */
		if (cp->c_atime < VTOHFS(vp)->hfc_timebase)
			VTOF(vp)->ff_bytesread = fp->ff_size;
		else
			VTOF(vp)->ff_bytesread += fp->ff_size;
		
	//	if (VTOF(vp)->ff_bytesread > fp->ff_size)
	//		cp->c_touch_acctime = TRUE;
	}

exit:
	hfs_unlock(cp);
	return (error);
}


/*
 * Get configurable pathname variables.
 */
static int
hfs_vnop_pathconf(ap)
	struct vnop_pathconf_args /* {
		struct vnode *a_vp;
		int a_name;
		int *a_retval;
		vfs_context_t a_context;
	} */ *ap;
{
	switch (ap->a_name) {
	case _PC_LINK_MAX:
		if (VTOHFS(ap->a_vp)->hfs_flags & HFS_STANDARD)
			*ap->a_retval = 1;
		else
			*ap->a_retval = HFS_LINK_MAX;
		break;
	case _PC_NAME_MAX:
		if (VTOHFS(ap->a_vp)->hfs_flags & HFS_STANDARD)
			*ap->a_retval = kHFSMaxFileNameChars;  /* 255 */
		else
			*ap->a_retval = kHFSPlusMaxFileNameChars;  /* 31 */
		break;
	case _PC_PATH_MAX:
		*ap->a_retval = PATH_MAX;  /* 1024 */
		break;
	case _PC_PIPE_BUF:
		*ap->a_retval = PIPE_BUF;
		break;
	case _PC_CHOWN_RESTRICTED:
		*ap->a_retval = 200112;		/* _POSIX_CHOWN_RESTRICTED */
		break;
	case _PC_NO_TRUNC:
		*ap->a_retval = 200112;		/* _POSIX_NO_TRUNC */
		break;
	case _PC_NAME_CHARS_MAX:
		*ap->a_retval = kHFSPlusMaxFileNameChars;
		break;
	case _PC_CASE_SENSITIVE:
		if (VTOHFS(ap->a_vp)->hfs_flags & HFS_CASE_SENSITIVE)
			*ap->a_retval = 1;
		else
			*ap->a_retval = 0;
		break;
	case _PC_CASE_PRESERVING:
		*ap->a_retval = 1;
		break;
	case _PC_FILESIZEBITS:
		if (VTOHFS(ap->a_vp)->hfs_flags & HFS_STANDARD)
			*ap->a_retval = 32;
		else
			*ap->a_retval = 64;	/* number of bits to store max file size */
		break;
	default:
		return (EINVAL);
	}

	return (0);
}


/*
 * Update a cnode's on-disk metadata.
 *
 * If waitfor is set, then wait for the disk write of
 * the node to complete.
 *
 * The cnode must be locked exclusive
 */
__private_extern__
int
hfs_update(struct vnode *vp, __unused int waitfor)
{
	struct cnode *cp = VTOC(vp);
	struct proc *p;
	struct cat_fork *dataforkp = NULL;
	struct cat_fork *rsrcforkp = NULL;
	struct cat_fork datafork;
	struct cat_fork rsrcfork;
	struct hfsmount *hfsmp;
	int lockflags;
	int error;

	p = current_proc();
	hfsmp = VTOHFS(vp);

	if (((vnode_issystem(vp) && (cp->c_cnid < kHFSFirstUserCatalogNodeID))) || 
	   	hfsmp->hfs_catalog_vp == NULL){
		return (0);
	}
	if ((hfsmp->hfs_flags & HFS_READ_ONLY) || (cp->c_mode == 0)) {
		cp->c_flag &= ~C_MODIFIED;
		cp->c_touch_acctime = 0;
		cp->c_touch_chgtime = 0;
		cp->c_touch_modtime = 0;
		return (0);
	}

	hfs_touchtimes(hfsmp, cp);

	/* Nothing to update. */
	if ((cp->c_flag & (C_MODIFIED | C_FORCEUPDATE)) == 0) {
		return (0);
	}
	
	if (cp->c_datafork)
		dataforkp = &cp->c_datafork->ff_data;
	if (cp->c_rsrcfork)
		rsrcforkp = &cp->c_rsrcfork->ff_data;

	/*
	 * For delayed allocations updates are
	 * postponed until an fsync or the file
	 * gets written to disk.
	 *
	 * Deleted files can defer meta data updates until inactive.
	 *
	 * If we're ever called with the C_FORCEUPDATE flag though
	 * we have to do the update.
	 */
	if (ISSET(cp->c_flag, C_FORCEUPDATE) == 0 &&
	    (ISSET(cp->c_flag, C_DELETED) || 
	    (dataforkp && cp->c_datafork->ff_unallocblocks) ||
	    (rsrcforkp && cp->c_rsrcfork->ff_unallocblocks))) {
	//	cp->c_flag &= ~(C_ACCESS | C_CHANGE | C_UPDATE);
		cp->c_flag |= C_MODIFIED;

		return (0);
	}

	if ((error = hfs_start_transaction(hfsmp)) != 0) {
	    return error;
	}

	/*
	 * For files with invalid ranges (holes) the on-disk
	 * field representing the size of the file (cf_size)
	 * must be no larger than the start of the first hole.
	 */
	if (dataforkp && !TAILQ_EMPTY(&cp->c_datafork->ff_invalidranges)) {
		bcopy(dataforkp, &datafork, sizeof(datafork));
		datafork.cf_size = TAILQ_FIRST(&cp->c_datafork->ff_invalidranges)->rl_start;
		dataforkp = &datafork;
	} else if (dataforkp && (cp->c_datafork->ff_unallocblocks != 0)) {
		// always make sure the block count and the size 
		// of the file match the number of blocks actually
		// allocated to the file on disk
		bcopy(dataforkp, &datafork, sizeof(datafork));
		// make sure that we don't assign a negative block count
		if (cp->c_datafork->ff_blocks < cp->c_datafork->ff_unallocblocks) {
		    panic("hfs: ff_blocks %d is less than unalloc blocks %d\n",
			  cp->c_datafork->ff_blocks, cp->c_datafork->ff_unallocblocks);
		}
		datafork.cf_blocks = (cp->c_datafork->ff_blocks - cp->c_datafork->ff_unallocblocks);
		datafork.cf_size   = datafork.cf_blocks * HFSTOVCB(hfsmp)->blockSize;
		dataforkp = &datafork;
	}

	/*
	 * For resource forks with delayed allocations, make sure
	 * the block count and file size match the number of blocks
	 * actually allocated to the file on disk.
	 */
	if (rsrcforkp && (cp->c_rsrcfork->ff_unallocblocks != 0)) {
		bcopy(rsrcforkp, &rsrcfork, sizeof(rsrcfork));
		rsrcfork.cf_blocks = (cp->c_rsrcfork->ff_blocks - cp->c_rsrcfork->ff_unallocblocks);
		rsrcfork.cf_size   = rsrcfork.cf_blocks * HFSTOVCB(hfsmp)->blockSize;
		rsrcforkp = &rsrcfork;
	}

	/*
	 * Lock the Catalog b-tree file.
	 */
	lockflags = hfs_systemfile_lock(hfsmp, SFL_CATALOG, HFS_EXCLUSIVE_LOCK);

	/* XXX - waitfor is not enforced */
	error = cat_update(hfsmp, &cp->c_desc, &cp->c_attr, dataforkp, rsrcforkp);

	hfs_systemfile_unlock(hfsmp, lockflags);

	/* After the updates are finished, clear the flags */
	cp->c_flag &= ~(C_MODIFIED | C_FORCEUPDATE);

	hfs_end_transaction(hfsmp);

	return (error);
}

/*
 * Allocate a new node
 * Note - Function does not create and return a vnode for whiteout creation.
 */
static int
hfs_makenode(struct vnode *dvp, struct vnode **vpp, struct componentname *cnp,
             struct vnode_attr *vap, vfs_context_t ctx)
{
	struct cnode *cp = NULL;
	struct cnode *dcp = NULL;
	struct vnode *tvp;
	struct hfsmount *hfsmp;
	struct cat_desc in_desc, out_desc;
	struct cat_attr attr;
	struct timeval tv;
	int lockflags;
	int error, started_tr = 0;
	enum vtype vnodetype;
	int mode;

	if ((error = hfs_lock(VTOC(dvp), HFS_EXCLUSIVE_LOCK)))
		return (error);

	/* set the cnode pointer only after successfully acquiring lock */
	dcp = VTOC(dvp);
	
	/* Don't allow creation of new entries in open-unlinked directories */
	if ((error = hfs_checkdeleted (dcp))) {
		hfs_unlock (dcp);
		return error;
	}

	dcp->c_flag |= C_DIR_MODIFICATION;
	
	hfsmp = VTOHFS(dvp);
	*vpp = NULL;
	tvp = NULL;
	out_desc.cd_flags = 0;
	out_desc.cd_nameptr = NULL;

	vnodetype = vap->va_type;
	if (vnodetype == VNON)
		vnodetype = VREG;
	mode = MAKEIMODE(vnodetype, vap->va_mode);

	/* Check if were out of usable disk space. */
	if ((hfs_freeblks(hfsmp, 1) == 0) && (vfs_context_suser(ctx) != 0)) {
		error = ENOSPC;
		goto exit;
	}

	microtime(&tv);

	/* Setup the default attributes */
	bzero(&attr, sizeof(attr));
	attr.ca_mode = mode;
	attr.ca_linkcount = 1;
	if (VATTR_IS_ACTIVE(vap, va_rdev)) {
		attr.ca_rdev = vap->va_rdev;
	}
	if (VATTR_IS_ACTIVE(vap, va_create_time)) {
		VATTR_SET_SUPPORTED(vap, va_create_time);
		attr.ca_itime = vap->va_create_time.tv_sec;
	} else {
		attr.ca_itime = tv.tv_sec;
	}
	if ((hfsmp->hfs_flags & HFS_STANDARD) && gTimeZone.tz_dsttime) {
		attr.ca_itime += 3600;	/* Same as what hfs_update does */
	}
	attr.ca_atime = attr.ca_ctime = attr.ca_mtime = attr.ca_itime;
	attr.ca_atimeondisk = attr.ca_atime;
	if (VATTR_IS_ACTIVE(vap, va_flags)) {
		VATTR_SET_SUPPORTED(vap, va_flags);
		attr.ca_flags = vap->va_flags;
	}
	
	/* 
	 * HFS+ only: all files get ThreadExists
	 * HFSX only: dirs get HasFolderCount
	 */
	if (!(hfsmp->hfs_flags & HFS_STANDARD)) {
		if (vnodetype == VDIR) {
			if (hfsmp->hfs_flags & HFS_FOLDERCOUNT)
				attr.ca_recflags = kHFSHasFolderCountMask;
		} else {
			attr.ca_recflags = kHFSThreadExistsMask;
		}
	}

	attr.ca_uid = vap->va_uid;
	attr.ca_gid = vap->va_gid;
	VATTR_SET_SUPPORTED(vap, va_mode);
	VATTR_SET_SUPPORTED(vap, va_uid);
	VATTR_SET_SUPPORTED(vap, va_gid);

#if QUOTA
	/* check to see if this node's creation would cause us to go over
	 * quota.  If so, abort this operation.
	 */
   	if (hfsmp->hfs_flags & HFS_QUOTAS) {
		if ((error = hfs_quotacheck(hfsmp, 1, attr.ca_uid, attr.ca_gid,
									vfs_context_ucred(ctx)))) {
			goto exit;
		}
	}	
#endif


	/* Tag symlinks with a type and creator. */
	if (vnodetype == VLNK) {
		struct FndrFileInfo *fip;

		fip = (struct FndrFileInfo *)&attr.ca_finderinfo;
		fip->fdType    = SWAP_BE32(kSymLinkFileType);
		fip->fdCreator = SWAP_BE32(kSymLinkCreator);
	}
	if (cnp->cn_flags & ISWHITEOUT)
		attr.ca_flags |= UF_OPAQUE;

	/* Setup the descriptor */
	in_desc.cd_nameptr = (const u_int8_t *)cnp->cn_nameptr;
	in_desc.cd_namelen = cnp->cn_namelen;
	in_desc.cd_parentcnid = dcp->c_fileid;
	in_desc.cd_flags = S_ISDIR(mode) ? CD_ISDIR : 0;
	in_desc.cd_hint = dcp->c_childhint;
	in_desc.cd_encoding = 0;

	if ((error = hfs_start_transaction(hfsmp)) != 0) {
	    goto exit;
	}
	started_tr = 1;

	// have to also lock the attribute file because cat_create() needs
	// to check that any fileID it wants to use does not have orphaned
	// attributes in it.
	lockflags = hfs_systemfile_lock(hfsmp, SFL_CATALOG | SFL_ATTRIBUTE, HFS_EXCLUSIVE_LOCK);

	/* Reserve some space in the Catalog file. */
	if ((error = cat_preflight(hfsmp, CAT_CREATE, NULL, 0))) {
		hfs_systemfile_unlock(hfsmp, lockflags);
		goto exit;
	}
	error = cat_create(hfsmp, &in_desc, &attr, &out_desc);
	if (error == 0) {
		/* Update the parent directory */
		dcp->c_childhint = out_desc.cd_hint;	/* Cache directory's location */
		dcp->c_entries++;
		if (vnodetype == VDIR) {
			INC_FOLDERCOUNT(hfsmp, dcp->c_attr);
		}
		dcp->c_dirchangecnt++;
		dcp->c_ctime = tv.tv_sec;
		dcp->c_mtime = tv.tv_sec;
		(void) cat_update(hfsmp, &dcp->c_desc, &dcp->c_attr, NULL, NULL);
	}
	hfs_systemfile_unlock(hfsmp, lockflags);
	if (error)
		goto exit;
	
	/* Invalidate negative cache entries in the directory */
	if (dcp->c_flag & C_NEG_ENTRIES) {
		cache_purge_negatives(dvp);
		dcp->c_flag &= ~C_NEG_ENTRIES;
	}

	hfs_volupdate(hfsmp, vnodetype == VDIR ? VOL_MKDIR : VOL_MKFILE,
		(dcp->c_cnid == kHFSRootFolderID));

	// XXXdbg
	// have to end the transaction here before we call hfs_getnewvnode()
	// because that can cause us to try and reclaim a vnode on a different
	// file system which could cause us to start a transaction which can
	// deadlock with someone on that other file system (since we could be
	// holding two transaction locks as well as various vnodes and we did
	// not obtain the locks on them in the proper order).
	//
	// NOTE: this means that if the quota check fails or we have to update
	//       the change time on a block-special device that those changes
	//       will happen as part of independent transactions.
	//
	if (started_tr) {
	    hfs_end_transaction(hfsmp);
	    started_tr = 0;
	}

	/* Do not create vnode for whiteouts */
	if (S_ISWHT(mode)) {
		goto exit;
	}

	/*
	 * Create a vnode for the object just created.
	 * 
	 * NOTE: Maintaining the cnode lock on the parent directory is important,
	 * as it prevents race conditions where other threads want to look up entries 
	 * in the directory and/or add things as we are in the process of creating
	 * the vnode below.  However, this has the potential for causing a 
	 * double lock panic when dealing with shadow files on a HFS boot partition. 
	 * The panic could occur if we are not cleaning up after ourselves properly 
	 * when done with a shadow file or in the error cases.  The error would occur if we 
	 * try to create a new vnode, and then end up reclaiming another shadow vnode to 
	 * create the new one.  However, if everything is working properly, this should
	 * be a non-issue as we would never enter that reclaim codepath.
	 *
	 * The cnode is locked on successful return.
	 */
	error = hfs_getnewvnode(hfsmp, dvp, cnp, &out_desc, GNV_CREATE, &attr, NULL, &tvp);
	if (error)
		goto exit;

	cp = VTOC(tvp);
	*vpp = tvp;
exit:
	cat_releasedesc(&out_desc);
	
	/*
	 * Make sure we release cnode lock on dcp.
	 */
	if (dcp) {
		dcp->c_flag &= ~C_DIR_MODIFICATION;
		wakeup((caddr_t)&dcp->c_flag);
		
		hfs_unlock(dcp);
	}
	if (error == 0 && cp != NULL) {
		hfs_unlock(cp);
	}
	if (started_tr) {
	    hfs_end_transaction(hfsmp);
	    started_tr = 0;
	}

	return (error);
}



/* hfs_vgetrsrc acquires a resource fork vnode corresponding to the cnode that is
 * found in 'vp'.  The rsrc fork vnode is returned with the cnode locked and iocount
 * on the rsrc vnode.
 * 
 * *rvpp is an output argument for returning the pointer to the resource fork vnode.
 * In most cases, the resource fork vnode will not be set if we return an error. 
 * However, if error_on_unlinked is set, we may have already acquired the resource fork vnode
 * before we discover the error (the file has gone open-unlinked).  In this case only,
 * we may return a vnode in the output argument despite an error.
 * 
 * If can_drop_lock is set, then it is safe for this function to temporarily drop
 * and then re-acquire the cnode lock.  We may need to do this, for example, in order to 
 * acquire an iocount or promote our lock.  
 * 
 * error_on_unlinked is an argument which indicates that we are to return an error if we 
 * discover that the cnode has gone into an open-unlinked state ( C_DELETED or C_NOEXISTS)
 * is set in the cnode flags.  This is only necessary if can_drop_lock is true, otherwise 
 * there's really no reason to double-check for errors on the cnode.
 */

__private_extern__
int
hfs_vgetrsrc(struct hfsmount *hfsmp, struct vnode *vp, 
		struct vnode **rvpp, int can_drop_lock, int error_on_unlinked)
{
	struct vnode *rvp;
	struct vnode *dvp = NULLVP;
	struct cnode *cp = VTOC(vp);
	int error;
	int vid;
	int delete_status = 0;


	/*
	 * Need to check the status of the cnode to validate it hasn't
	 * gone open-unlinked on us before we can actually do work with it.
	 */
	delete_status = hfs_checkdeleted (cp);
	if ((delete_status) && (error_on_unlinked)) {
		return delete_status;
	}

restart:
	/* Attempt to use exising vnode */
	if ((rvp = cp->c_rsrc_vp)) {
	        vid = vnode_vid(rvp);

		/*
		 * It is not safe to hold the cnode lock when calling vnode_getwithvid()
		 * for the alternate fork -- vnode_getwithvid() could deadlock waiting
		 * for a VL_WANTTERM while another thread has an iocount on the alternate
		 * fork vnode and is attempting to acquire the common cnode lock.
		 *
		 * But it's also not safe to drop the cnode lock when we're holding
		 * multiple cnode locks, like during a hfs_removefile() operation
		 * since we could lock out of order when re-acquiring the cnode lock.
		 *
		 * So we can only drop the lock here if its safe to drop it -- which is
		 * most of the time with the exception being hfs_removefile().
		 */
		if (can_drop_lock)
			hfs_unlock(cp);

		error = vnode_getwithvid(rvp, vid);

		if (can_drop_lock) {
			(void) hfs_lock(cp, HFS_FORCE_LOCK);

			/*
			 * When we relinquished our cnode lock, the cnode could have raced
			 * with a delete and gotten deleted.  If the caller did not want
			 * us to ignore open-unlinked files, then re-check the C_DELETED
			 * state and see if we need to return an ENOENT here because the item
			 * got deleted in the intervening time.
			 */
			if (error_on_unlinked) {
				if ((delete_status = hfs_checkdeleted(cp))) {
					/* 
					 * If error == 0, this means that we succeeded in acquiring an iocount on the 
					 * rsrc fork vnode.  However, if we're in this block of code, that 
					 * means that we noticed that the cnode has gone open-unlinked.  In 
					 * this case, the caller requested that we not do any other work and 
					 * return an errno.  The caller will be responsible for dropping the 
					 * iocount we just acquired because we can't do it until we've released 
					 * the cnode lock.  
					 */
					if (error == 0) {
						*rvpp = rvp;
					}
					return delete_status;
				}
			}

			/*
			 * When our lock was relinquished, the resource fork
			 * could have been recycled.  Check for this and try
			 * again.
			 */
			if (error == ENOENT)
				goto restart;
		}
		if (error) {
			const char * name = (const char *)VTOC(vp)->c_desc.cd_nameptr;

			if (name)
				printf("hfs_vgetrsrc: couldn't get resource"
				       " fork for %s, err %d\n", name, error);
			return (error);
		}
	} else {
		struct cat_fork rsrcfork;
		struct componentname cn;
		struct cat_desc *descptr = NULL;
		struct cat_desc to_desc;
		char delname[32];
		int lockflags;

		/*
		 * Make sure cnode lock is exclusive, if not upgrade it.
		 *
		 * We assume that we were called from a read-only VNOP (getattr)
		 * and that its safe to have the cnode lock dropped and reacquired.
		 */
		if (cp->c_lockowner != current_thread()) {
			if (!can_drop_lock) {				
				return (EINVAL);
			}
			/*
			 * If the upgrade fails we lose the lock and
			 * have to take the exclusive lock on our own.
			 */
			if (lck_rw_lock_shared_to_exclusive(&cp->c_rwlock) == FALSE)
				lck_rw_lock_exclusive(&cp->c_rwlock);
			cp->c_lockowner = current_thread();
		}

		/*
		 * hfs_vgetsrc may be invoked for a cnode that has already been marked
		 * C_DELETED.  This is because we need to continue to provide rsrc
		 * fork access to open-unlinked files.  In this case, build a fake descriptor
		 * like in hfs_removefile.  If we don't do this, buildkey will fail in
		 * cat_lookup because this cnode has no name in its descriptor. However,
		 * only do this if the caller did not specify that they wanted us to
		 * error out upon encountering open-unlinked files.
		 */

		if ((error_on_unlinked) && (can_drop_lock)) {
			if ((error = hfs_checkdeleted (cp))) {
				return error;
			}
		}

		if ((cp->c_flag & C_DELETED ) && (cp->c_desc.cd_namelen == 0)) {
			bzero (&to_desc, sizeof(to_desc));
			bzero (delname, 32);
			MAKE_DELETED_NAME(delname, sizeof(delname), cp->c_fileid);
			to_desc.cd_nameptr = (const u_int8_t*) delname;
			to_desc.cd_namelen = strlen(delname);
			to_desc.cd_parentcnid = hfsmp->hfs_private_desc[FILE_HARDLINKS].cd_cnid;
			to_desc.cd_flags = 0;
			to_desc.cd_cnid = cp->c_cnid;

			descptr = &to_desc;
		}
		else {
			descptr = &cp->c_desc;
		}


		lockflags = hfs_systemfile_lock(hfsmp, SFL_CATALOG, HFS_SHARED_LOCK);

		/* Get resource fork data */
		error = cat_lookup(hfsmp, descptr, 1, (struct cat_desc *)0,
				(struct cat_attr *)0, &rsrcfork, NULL);

		hfs_systemfile_unlock(hfsmp, lockflags);
		if (error) {
			return (error);
		}
		/*
		 * Supply hfs_getnewvnode with a component name. 
		 */
		cn.cn_pnbuf = NULL;
		if (descptr->cd_nameptr) {
			MALLOC_ZONE(cn.cn_pnbuf, caddr_t, MAXPATHLEN, M_NAMEI, M_WAITOK);
			cn.cn_nameiop = LOOKUP;
			cn.cn_flags = ISLASTCN | HASBUF;
			cn.cn_context = NULL;
			cn.cn_pnlen = MAXPATHLEN;
			cn.cn_nameptr = cn.cn_pnbuf;
			cn.cn_hash = 0;
			cn.cn_consume = 0;
			cn.cn_namelen = snprintf(cn.cn_nameptr, MAXPATHLEN,
						 "%s%s", descptr->cd_nameptr,
						 _PATH_RSRCFORKSPEC);
		}
		dvp = vnode_getparent(vp);
		error = hfs_getnewvnode(hfsmp, dvp, cn.cn_pnbuf ? &cn : NULL,
		                        descptr, GNV_WANTRSRC | GNV_SKIPLOCK, &cp->c_attr,
		                        &rsrcfork, &rvp);
		if (dvp)
			vnode_put(dvp);
		if (cn.cn_pnbuf)
			FREE_ZONE(cn.cn_pnbuf, cn.cn_pnlen, M_NAMEI);
		if (error)
			return (error);
	}

	*rvpp = rvp;
	return (0);
}

/*
 * Wrapper for special device reads
 */
static int
hfsspec_read(ap)
	struct vnop_read_args /* {
		struct vnode *a_vp;
		struct uio *a_uio;
		int  a_ioflag;
		vfs_context_t a_context;
	} */ *ap;
{
	/*
	 * Set access flag.
	 */
	VTOC(ap->a_vp)->c_touch_acctime = TRUE;
	return (VOCALL (spec_vnodeop_p, VOFFSET(vnop_read), ap));
}

/*
 * Wrapper for special device writes
 */
static int
hfsspec_write(ap)
	struct vnop_write_args /* {
		struct vnode *a_vp;
		struct uio *a_uio;
		int  a_ioflag;
		vfs_context_t a_context;
	} */ *ap;
{
	/*
	 * Set update and change flags.
	 */
	VTOC(ap->a_vp)->c_touch_chgtime = TRUE;
	VTOC(ap->a_vp)->c_touch_modtime = TRUE;
	return (VOCALL (spec_vnodeop_p, VOFFSET(vnop_write), ap));
}

/*
 * Wrapper for special device close
 *
 * Update the times on the cnode then do device close.
 */
static int
hfsspec_close(ap)
	struct vnop_close_args /* {
		struct vnode *a_vp;
		int  a_fflag;
		vfs_context_t a_context;
	} */ *ap;
{
	struct vnode *vp = ap->a_vp;
	struct cnode *cp;

	if (vnode_isinuse(ap->a_vp, 0)) {
		if (hfs_lock(VTOC(vp), HFS_EXCLUSIVE_LOCK) == 0) {
			cp = VTOC(vp);
			hfs_touchtimes(VTOHFS(vp), cp);
			hfs_unlock(cp);
		}
	}
	return (VOCALL (spec_vnodeop_p, VOFFSET(vnop_close), ap));
}

#if FIFO
/*
 * Wrapper for fifo reads
 */
static int
hfsfifo_read(ap)
	struct vnop_read_args /* {
		struct vnode *a_vp;
		struct uio *a_uio;
		int  a_ioflag;
		vfs_context_t a_context;
	} */ *ap;
{
	/*
	 * Set access flag.
	 */
	VTOC(ap->a_vp)->c_touch_acctime = TRUE;
	return (VOCALL (fifo_vnodeop_p, VOFFSET(vnop_read), ap));
}

/*
 * Wrapper for fifo writes
 */
static int
hfsfifo_write(ap)
	struct vnop_write_args /* {
		struct vnode *a_vp;
		struct uio *a_uio;
		int  a_ioflag;
		vfs_context_t a_context;
	} */ *ap;
{
	/*
	 * Set update and change flags.
	 */
	VTOC(ap->a_vp)->c_touch_chgtime = TRUE;
	VTOC(ap->a_vp)->c_touch_modtime = TRUE;
	return (VOCALL (fifo_vnodeop_p, VOFFSET(vnop_write), ap));
}

/*
 * Wrapper for fifo close
 *
 * Update the times on the cnode then do device close.
 */
static int
hfsfifo_close(ap)
	struct vnop_close_args /* {
		struct vnode *a_vp;
		int  a_fflag;
		vfs_context_t a_context;
	} */ *ap;
{
	struct vnode *vp = ap->a_vp;
	struct cnode *cp;

	if (vnode_isinuse(ap->a_vp, 1)) {
		if (hfs_lock(VTOC(vp), HFS_EXCLUSIVE_LOCK) == 0) {
			cp = VTOC(vp);
			hfs_touchtimes(VTOHFS(vp), cp);
			hfs_unlock(cp);
		}
	}
	return (VOCALL (fifo_vnodeop_p, VOFFSET(vnop_close), ap));
}


#endif /* FIFO */

/*
 * Synchronize a file's in-core state with that on disk.
 */
static int
hfs_vnop_fsync(ap)
	struct vnop_fsync_args /* {
		struct vnode *a_vp;
		int a_waitfor;
		vfs_context_t a_context;
	} */ *ap;
{
	struct vnode* vp = ap->a_vp;
	int error;

	/*
	 * We need to allow ENOENT lock errors since unlink
	 * systenm call can call VNOP_FSYNC during vclean.
	 */
	error = hfs_lock(VTOC(vp), HFS_EXCLUSIVE_LOCK);
	if (error)
		return (0);

	error = hfs_fsync(vp, ap->a_waitfor, 0, vfs_context_proc(ap->a_context));

	hfs_unlock(VTOC(vp));
	return (error);
}


static int
hfs_vnop_whiteout(ap) 
	struct vnop_whiteout_args /* {
		struct vnode *a_dvp;
		struct componentname *a_cnp;
		int a_flags;
		vfs_context_t a_context;
	} */ *ap;
{
	int error = 0;
	struct vnode *vp = NULL;
	struct vnode_attr va;
	struct vnop_lookup_args lookup_args;
	struct vnop_remove_args remove_args;
	struct hfsmount *hfsmp;

	hfsmp = VTOHFS(ap->a_dvp);
	if (hfsmp->hfs_flags & HFS_STANDARD) {
		error = ENOTSUP;
		goto exit;
	}

	switch (ap->a_flags) {
		case LOOKUP:
			error = 0;
			break;

		case CREATE: 
			VATTR_INIT(&va);
			VATTR_SET(&va, va_type, VREG);
			VATTR_SET(&va, va_mode, S_IFWHT);
			VATTR_SET(&va, va_uid, 0);
			VATTR_SET(&va, va_gid, 0);
			
			error = hfs_makenode(ap->a_dvp, &vp, ap->a_cnp, &va, ap->a_context);
			/* No need to release the vnode as no vnode is created for whiteouts */
			break;

		case DELETE:
			lookup_args.a_dvp = ap->a_dvp;
			lookup_args.a_vpp = &vp;
			lookup_args.a_cnp = ap->a_cnp;
			lookup_args.a_context = ap->a_context;

			error = hfs_vnop_lookup(&lookup_args);
			if (error) {
				break;
			}
			
			remove_args.a_dvp = ap->a_dvp;
			remove_args.a_vp = vp;
			remove_args.a_cnp = ap->a_cnp;
			remove_args.a_flags = 0;
			remove_args.a_context = ap->a_context;

			error = hfs_vnop_remove(&remove_args);
			vnode_put(vp);
			break;

		default:
			panic("hfs_vnop_whiteout: unknown operation (flag = %x)\n", ap->a_flags);
	};
	
exit:
	return (error);
}

int (**hfs_vnodeop_p)(void *);
int (**hfs_std_vnodeop_p) (void *);

#define VOPFUNC int (*)(void *)

static int hfs_readonly_op (__unused void* ap) { return (EROFS); }

/* 
 * In 10.6 and forward, HFS Standard is read-only and deprecated.  The vnop table below
 * is for use with HFS standard to block out operations that would modify the file system
 */

struct vnodeopv_entry_desc hfs_standard_vnodeop_entries[] = {
    { &vnop_default_desc, (VOPFUNC)vn_default_error },
    { &vnop_lookup_desc, (VOPFUNC)hfs_vnop_lookup },		/* lookup */
    { &vnop_create_desc, (VOPFUNC)hfs_readonly_op },		/* create (READONLY) */
    { &vnop_mknod_desc, (VOPFUNC)hfs_readonly_op },             /* mknod (READONLY) */
    { &vnop_open_desc, (VOPFUNC)hfs_vnop_open },			/* open */
    { &vnop_close_desc, (VOPFUNC)hfs_vnop_close },		/* close */
    { &vnop_getattr_desc, (VOPFUNC)hfs_vnop_getattr },		/* getattr */
    { &vnop_setattr_desc, (VOPFUNC)hfs_readonly_op },		/* setattr */
    { &vnop_read_desc, (VOPFUNC)hfs_vnop_read },			/* read */
    { &vnop_write_desc, (VOPFUNC)hfs_readonly_op },		/* write (READONLY) */
    { &vnop_ioctl_desc, (VOPFUNC)hfs_vnop_ioctl },		/* ioctl */
    { &vnop_select_desc, (VOPFUNC)hfs_vnop_select },		/* select */
    { &vnop_revoke_desc, (VOPFUNC)nop_revoke },			/* revoke */
    { &vnop_exchange_desc, (VOPFUNC)hfs_readonly_op },		/* exchange  (READONLY)*/
    { &vnop_mmap_desc, (VOPFUNC)err_mmap },			/* mmap */
    { &vnop_fsync_desc, (VOPFUNC)hfs_readonly_op},		/* fsync (READONLY) */
    { &vnop_remove_desc, (VOPFUNC)hfs_readonly_op },		/* remove (READONLY) */
    { &vnop_link_desc, (VOPFUNC)hfs_readonly_op },			/* link ( READONLLY) */
    { &vnop_rename_desc, (VOPFUNC)hfs_readonly_op },		/* rename (READONLY)*/
    { &vnop_mkdir_desc, (VOPFUNC)hfs_readonly_op },             /* mkdir (READONLY) */
    { &vnop_rmdir_desc, (VOPFUNC)hfs_readonly_op },		/* rmdir (READONLY) */
    { &vnop_symlink_desc, (VOPFUNC)hfs_readonly_op },         /* symlink (READONLY) */
    { &vnop_readdir_desc, (VOPFUNC)hfs_vnop_readdir },		/* readdir */
    { &vnop_readdirattr_desc, (VOPFUNC)hfs_vnop_readdirattr },	/* readdirattr */
    { &vnop_readlink_desc, (VOPFUNC)hfs_vnop_readlink },		/* readlink */
    { &vnop_inactive_desc, (VOPFUNC)hfs_vnop_inactive },		/* inactive */
    { &vnop_reclaim_desc, (VOPFUNC)hfs_vnop_reclaim },		/* reclaim */
    { &vnop_strategy_desc, (VOPFUNC)hfs_vnop_strategy },		/* strategy */
    { &vnop_pathconf_desc, (VOPFUNC)hfs_vnop_pathconf },		/* pathconf */
    { &vnop_advlock_desc, (VOPFUNC)err_advlock },		/* advlock */
    { &vnop_allocate_desc, (VOPFUNC)hfs_readonly_op },		/* allocate (READONLY) */
    { &vnop_searchfs_desc, (VOPFUNC)hfs_vnop_search },		/* search fs */
    { &vnop_bwrite_desc, (VOPFUNC)hfs_readonly_op },		/* bwrite (READONLY) */
    { &vnop_pagein_desc, (VOPFUNC)hfs_vnop_pagein },		/* pagein */
    { &vnop_pageout_desc,(VOPFUNC) hfs_readonly_op },		/* pageout (READONLY)  */
    { &vnop_copyfile_desc, (VOPFUNC)hfs_readonly_op },		/* copyfile (READONLY)*/
    { &vnop_blktooff_desc, (VOPFUNC)hfs_vnop_blktooff },		/* blktooff */
    { &vnop_offtoblk_desc, (VOPFUNC)hfs_vnop_offtoblk },		/* offtoblk */
    { &vnop_blockmap_desc, (VOPFUNC)hfs_vnop_blockmap },			/* blockmap */
    { &vnop_getxattr_desc, (VOPFUNC)hfs_vnop_getxattr},
    { &vnop_setxattr_desc, (VOPFUNC)hfs_readonly_op},         /* set xattr (READONLY) */
    { &vnop_removexattr_desc, (VOPFUNC)hfs_readonly_op},      /* remove xattr (READONLY) */
    { &vnop_listxattr_desc, (VOPFUNC)hfs_vnop_listxattr},
    { &vnop_whiteout_desc, (VOPFUNC)hfs_readonly_op},       /* whiteout (READONLY) */
#if NAMEDSTREAMS
    { &vnop_getnamedstream_desc, (VOPFUNC)hfs_vnop_getnamedstream },
    { &vnop_makenamedstream_desc, (VOPFUNC)hfs_readonly_op }, 
    { &vnop_removenamedstream_desc, (VOPFUNC)hfs_readonly_op },
#endif
    { NULL, (VOPFUNC)NULL }
};

struct vnodeopv_desc hfs_std_vnodeop_opv_desc =
{ &hfs_std_vnodeop_p, hfs_standard_vnodeop_entries };


/* VNOP table for HFS+ */
struct vnodeopv_entry_desc hfs_vnodeop_entries[] = {
    { &vnop_default_desc, (VOPFUNC)vn_default_error },
    { &vnop_lookup_desc, (VOPFUNC)hfs_vnop_lookup },		/* lookup */
    { &vnop_create_desc, (VOPFUNC)hfs_vnop_create },		/* create */
    { &vnop_mknod_desc, (VOPFUNC)hfs_vnop_mknod },             /* mknod */
    { &vnop_open_desc, (VOPFUNC)hfs_vnop_open },			/* open */
    { &vnop_close_desc, (VOPFUNC)hfs_vnop_close },		/* close */
    { &vnop_getattr_desc, (VOPFUNC)hfs_vnop_getattr },		/* getattr */
    { &vnop_setattr_desc, (VOPFUNC)hfs_vnop_setattr },		/* setattr */
    { &vnop_read_desc, (VOPFUNC)hfs_vnop_read },			/* read */
    { &vnop_write_desc, (VOPFUNC)hfs_vnop_write },		/* write */
    { &vnop_ioctl_desc, (VOPFUNC)hfs_vnop_ioctl },		/* ioctl */
    { &vnop_select_desc, (VOPFUNC)hfs_vnop_select },		/* select */
    { &vnop_revoke_desc, (VOPFUNC)nop_revoke },			/* revoke */
    { &vnop_exchange_desc, (VOPFUNC)hfs_vnop_exchange },		/* exchange */
    { &vnop_mmap_desc, (VOPFUNC)err_mmap },			/* mmap */
    { &vnop_fsync_desc, (VOPFUNC)hfs_vnop_fsync },		/* fsync */
    { &vnop_remove_desc, (VOPFUNC)hfs_vnop_remove },		/* remove */
    { &vnop_link_desc, (VOPFUNC)hfs_vnop_link },			/* link */
    { &vnop_rename_desc, (VOPFUNC)hfs_vnop_rename },		/* rename */
    { &vnop_mkdir_desc, (VOPFUNC)hfs_vnop_mkdir },             /* mkdir */
    { &vnop_rmdir_desc, (VOPFUNC)hfs_vnop_rmdir },		/* rmdir */
    { &vnop_symlink_desc, (VOPFUNC)hfs_vnop_symlink },         /* symlink */
    { &vnop_readdir_desc, (VOPFUNC)hfs_vnop_readdir },		/* readdir */
    { &vnop_readdirattr_desc, (VOPFUNC)hfs_vnop_readdirattr },	/* readdirattr */
    { &vnop_readlink_desc, (VOPFUNC)hfs_vnop_readlink },		/* readlink */
    { &vnop_inactive_desc, (VOPFUNC)hfs_vnop_inactive },		/* inactive */
    { &vnop_reclaim_desc, (VOPFUNC)hfs_vnop_reclaim },		/* reclaim */
    { &vnop_strategy_desc, (VOPFUNC)hfs_vnop_strategy },		/* strategy */
    { &vnop_pathconf_desc, (VOPFUNC)hfs_vnop_pathconf },		/* pathconf */
    { &vnop_advlock_desc, (VOPFUNC)err_advlock },		/* advlock */
    { &vnop_allocate_desc, (VOPFUNC)hfs_vnop_allocate },		/* allocate */
    { &vnop_searchfs_desc, (VOPFUNC)hfs_vnop_search },		/* search fs */
    { &vnop_bwrite_desc, (VOPFUNC)hfs_vnop_bwrite },		/* bwrite */
    { &vnop_pagein_desc, (VOPFUNC)hfs_vnop_pagein },		/* pagein */
    { &vnop_pageout_desc,(VOPFUNC) hfs_vnop_pageout },		/* pageout */
    { &vnop_copyfile_desc, (VOPFUNC)err_copyfile },		/* copyfile */
    { &vnop_blktooff_desc, (VOPFUNC)hfs_vnop_blktooff },		/* blktooff */
    { &vnop_offtoblk_desc, (VOPFUNC)hfs_vnop_offtoblk },		/* offtoblk */
    { &vnop_blockmap_desc, (VOPFUNC)hfs_vnop_blockmap },			/* blockmap */
    { &vnop_getxattr_desc, (VOPFUNC)hfs_vnop_getxattr},
    { &vnop_setxattr_desc, (VOPFUNC)hfs_vnop_setxattr},
    { &vnop_removexattr_desc, (VOPFUNC)hfs_vnop_removexattr},
    { &vnop_listxattr_desc, (VOPFUNC)hfs_vnop_listxattr},
    { &vnop_whiteout_desc, (VOPFUNC)hfs_vnop_whiteout},
#if NAMEDSTREAMS
    { &vnop_getnamedstream_desc, (VOPFUNC)hfs_vnop_getnamedstream },
    { &vnop_makenamedstream_desc, (VOPFUNC)hfs_vnop_makenamedstream },
    { &vnop_removenamedstream_desc, (VOPFUNC)hfs_vnop_removenamedstream },
#endif
    { NULL, (VOPFUNC)NULL }
};

struct vnodeopv_desc hfs_vnodeop_opv_desc =
{ &hfs_vnodeop_p, hfs_vnodeop_entries };


/* Spec Op vnop table for HFS+ */
int (**hfs_specop_p)(void *);
struct vnodeopv_entry_desc hfs_specop_entries[] = {
	{ &vnop_default_desc, (VOPFUNC)vn_default_error },
	{ &vnop_lookup_desc, (VOPFUNC)spec_lookup },		/* lookup */
	{ &vnop_create_desc, (VOPFUNC)spec_create },		/* create */
	{ &vnop_mknod_desc, (VOPFUNC)spec_mknod },              /* mknod */
	{ &vnop_open_desc, (VOPFUNC)spec_open },			/* open */
	{ &vnop_close_desc, (VOPFUNC)hfsspec_close },		/* close */
	{ &vnop_getattr_desc, (VOPFUNC)hfs_vnop_getattr },	/* getattr */
	{ &vnop_setattr_desc, (VOPFUNC)hfs_vnop_setattr },	/* setattr */
	{ &vnop_read_desc, (VOPFUNC)hfsspec_read },		/* read */
	{ &vnop_write_desc, (VOPFUNC)hfsspec_write },		/* write */
	{ &vnop_ioctl_desc, (VOPFUNC)spec_ioctl },		/* ioctl */
	{ &vnop_select_desc, (VOPFUNC)spec_select },		/* select */
	{ &vnop_revoke_desc, (VOPFUNC)spec_revoke },		/* revoke */
	{ &vnop_mmap_desc, (VOPFUNC)spec_mmap },			/* mmap */
	{ &vnop_fsync_desc, (VOPFUNC)hfs_vnop_fsync },		/* fsync */
	{ &vnop_remove_desc, (VOPFUNC)spec_remove },		/* remove */
	{ &vnop_link_desc, (VOPFUNC)spec_link },			/* link */
	{ &vnop_rename_desc, (VOPFUNC)spec_rename },		/* rename */
	{ &vnop_mkdir_desc, (VOPFUNC)spec_mkdir },              /* mkdir */
	{ &vnop_rmdir_desc, (VOPFUNC)spec_rmdir },		/* rmdir */
	{ &vnop_symlink_desc, (VOPFUNC)spec_symlink },          /* symlink */
	{ &vnop_readdir_desc, (VOPFUNC)spec_readdir },		/* readdir */
	{ &vnop_readlink_desc, (VOPFUNC)spec_readlink },		/* readlink */
	{ &vnop_inactive_desc, (VOPFUNC)hfs_vnop_inactive },	/* inactive */
	{ &vnop_reclaim_desc, (VOPFUNC)hfs_vnop_reclaim },	/* reclaim */
	{ &vnop_strategy_desc, (VOPFUNC)spec_strategy },		/* strategy */
	{ &vnop_pathconf_desc, (VOPFUNC)spec_pathconf },		/* pathconf */
	{ &vnop_advlock_desc, (VOPFUNC)err_advlock },		/* advlock */
	{ &vnop_bwrite_desc, (VOPFUNC)hfs_vnop_bwrite },
	{ &vnop_pagein_desc, (VOPFUNC)hfs_vnop_pagein },		/* Pagein */
	{ &vnop_pageout_desc, (VOPFUNC)hfs_vnop_pageout },	/* Pageout */
    { &vnop_copyfile_desc, (VOPFUNC)err_copyfile },		/* copyfile */
	{ &vnop_blktooff_desc, (VOPFUNC)hfs_vnop_blktooff },	/* blktooff */
	{ &vnop_offtoblk_desc, (VOPFUNC)hfs_vnop_offtoblk },	/* offtoblk */
	{ (struct vnodeop_desc*)NULL, (VOPFUNC)NULL }
};
struct vnodeopv_desc hfs_specop_opv_desc =
	{ &hfs_specop_p, hfs_specop_entries };

#if FIFO
/* HFS+ FIFO VNOP table  */
int (**hfs_fifoop_p)(void *);
struct vnodeopv_entry_desc hfs_fifoop_entries[] = {
	{ &vnop_default_desc, (VOPFUNC)vn_default_error },
	{ &vnop_lookup_desc, (VOPFUNC)fifo_lookup },		/* lookup */
	{ &vnop_create_desc, (VOPFUNC)fifo_create },		/* create */
	{ &vnop_mknod_desc, (VOPFUNC)fifo_mknod },              /* mknod */
	{ &vnop_open_desc, (VOPFUNC)fifo_open },			/* open */
	{ &vnop_close_desc, (VOPFUNC)hfsfifo_close },		/* close */
	{ &vnop_getattr_desc, (VOPFUNC)hfs_vnop_getattr },	/* getattr */
	{ &vnop_setattr_desc, (VOPFUNC)hfs_vnop_setattr },	/* setattr */
	{ &vnop_read_desc, (VOPFUNC)hfsfifo_read },		/* read */
	{ &vnop_write_desc, (VOPFUNC)hfsfifo_write },		/* write */
	{ &vnop_ioctl_desc, (VOPFUNC)fifo_ioctl },		/* ioctl */
	{ &vnop_select_desc, (VOPFUNC)fifo_select },		/* select */
	{ &vnop_revoke_desc, (VOPFUNC)fifo_revoke },		/* revoke */
	{ &vnop_mmap_desc, (VOPFUNC)fifo_mmap },			/* mmap */
	{ &vnop_fsync_desc, (VOPFUNC)hfs_vnop_fsync },		/* fsync */
	{ &vnop_remove_desc, (VOPFUNC)fifo_remove },		/* remove */
	{ &vnop_link_desc, (VOPFUNC)fifo_link },			/* link */
	{ &vnop_rename_desc, (VOPFUNC)fifo_rename },		/* rename */
	{ &vnop_mkdir_desc, (VOPFUNC)fifo_mkdir },              /* mkdir */
	{ &vnop_rmdir_desc, (VOPFUNC)fifo_rmdir },		/* rmdir */
	{ &vnop_symlink_desc, (VOPFUNC)fifo_symlink },          /* symlink */
	{ &vnop_readdir_desc, (VOPFUNC)fifo_readdir },		/* readdir */
	{ &vnop_readlink_desc, (VOPFUNC)fifo_readlink },		/* readlink */
	{ &vnop_inactive_desc, (VOPFUNC)hfs_vnop_inactive },	/* inactive */
	{ &vnop_reclaim_desc, (VOPFUNC)hfs_vnop_reclaim },	/* reclaim */
	{ &vnop_strategy_desc, (VOPFUNC)fifo_strategy },		/* strategy */
	{ &vnop_pathconf_desc, (VOPFUNC)fifo_pathconf },		/* pathconf */
	{ &vnop_advlock_desc, (VOPFUNC)err_advlock },		/* advlock */
	{ &vnop_bwrite_desc, (VOPFUNC)hfs_vnop_bwrite },
	{ &vnop_pagein_desc, (VOPFUNC)hfs_vnop_pagein },		/* Pagein */
	{ &vnop_pageout_desc, (VOPFUNC)hfs_vnop_pageout },	/* Pageout */
	{ &vnop_copyfile_desc, (VOPFUNC)err_copyfile }, 		/* copyfile */
	{ &vnop_blktooff_desc, (VOPFUNC)hfs_vnop_blktooff },	/* blktooff */
	{ &vnop_offtoblk_desc, (VOPFUNC)hfs_vnop_offtoblk },	/* offtoblk */
  	{ &vnop_blockmap_desc, (VOPFUNC)hfs_vnop_blockmap },		/* blockmap */
	{ (struct vnodeop_desc*)NULL, (VOPFUNC)NULL }
};
struct vnodeopv_desc hfs_fifoop_opv_desc =
	{ &hfs_fifoop_p, hfs_fifoop_entries };
#endif /* FIFO */




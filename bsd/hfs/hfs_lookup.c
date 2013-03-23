/*
 * Copyright (c) 1999-2008 Apple Inc. All rights reserved.
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
 * Copyright (c) 1989, 1993
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
 *	  notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *	  notice, this list of conditions and the following disclaimer in the
 *	  documentation and/or other materials provided with the distribution.
 * 3. All advertising materials mentioning features or use of this software
 *	  must display the following acknowledgement:
 *	This product includes software developed by the University of
 *	California, Berkeley and its contributors.
 * 4. Neither the name of the University nor the names of its contributors
 *	  may be used to endorse or promote products derived from this software
 *	  without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE REGENTS AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.	IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 *	@(#)hfs_lookup.c	1.0
 *	derived from @(#)ufs_lookup.c	8.15 (Berkeley) 6/16/95
 *
 *	(c) 1998-1999   Apple Computer, Inc.	 All Rights Reserved
 *	(c) 1990, 1992 	NeXT Computer, Inc.	All Rights Reserved
 *	
 *
 *	hfs_lookup.c -- code to handle directory traversal on HFS/HFS+ volume
 */

#include <sys/param.h>
#include <sys/file.h>
#include <sys/mount.h>
#include <sys/vnode.h>
#include <sys/malloc.h>
#include <sys/kdebug.h>
#include <sys/kauth.h>
#include <sys/namei.h>

#include "hfs.h"
#include "hfs_catalog.h"
#include "hfs_cnode.h"


/*	
 * FROM FREEBSD 3.1
 * Convert a component of a pathname into a pointer to a locked cnode.
 * This is a very central and rather complicated routine.
 * If the file system is not maintained in a strict tree hierarchy,
 * this can result in a deadlock situation (see comments in code below).
 *
 * The cnp->cn_nameiop argument is LOOKUP, CREATE, RENAME, or DELETE depending
 * on whether the name is to be looked up, created, renamed, or deleted.
 * When CREATE, RENAME, or DELETE is specified, information usable in
 * creating, renaming, or deleting a directory entry may be calculated.
 * Notice that these are the only operations that can affect the directory of the target.
 *
 * LOCKPARENT and WANTPARENT actually refer to the parent of the last item,
 * so if ISLASTCN is not set, they should be ignored. Also they are mutually exclusive, or
 * WANTPARENT really implies DONTLOCKPARENT. Either of them set means that the calling
 * routine wants to access the parent of the target, locked or unlocked.
 *
 * Keeping the parent locked as long as possible protects from other processes
 * looking up the same item, so it has to be locked until the cnode is totally finished
 *
 * hfs_cache_lookup() performs the following for us:
 *	check that it is a directory
 *	check accessibility of directory
 *	check for modification attempts on read-only mounts
 *	if name found in cache
 *		if at end of path and deleting or creating
 *		drop it
 *		 else
 *		return name.
 *	return hfs_lookup()
 *
 * Overall outline of hfs_lookup:
 *
 *	handle simple cases of . and ..
 *	search for name in directory, to found or notfound
 * notfound:
 *	if creating, return locked directory, leaving info on available slots
 *	else return error
 * found:
 *	if at end of path and deleting, return information to allow delete
 *	if at end of path and rewriting (RENAME and LOCKPARENT), lock target
 *	  cnode and return info to allow rewrite
 *	if not at end, add name to cache; if at end and neither creating
 *	  nor deleting, add name to cache
 */


/*	
 *	Lookup *cnp in directory *dvp, return it in *vpp.
 *	**vpp is held on exit.
 *	We create a cnode for the file, but we do NOT open the file here.

#% lookup	dvp L ? ?
#% lookup	vpp - L -

	IN struct vnode *dvp - Parent node of file;
	INOUT struct vnode **vpp - node of target file, its a new node if
		the target vnode did not exist;
	IN struct componentname *cnp - Name of file;

 *	When should we lock parent_hp in here ??
 */
static int
hfs_lookup(struct vnode *dvp, struct vnode **vpp, struct componentname *cnp, int *cnode_locked)
{
	struct cnode *dcp;	/* cnode for directory being searched */
	struct vnode *tvp;	/* target vnode */
	struct hfsmount *hfsmp;
	int flags;
	int nameiop;
	int retval = 0;
	int isDot;
	struct cat_desc desc;
	struct cat_desc cndesc;
	struct cat_attr attr;
	struct cat_fork fork;
	int lockflags;

  retry:
	dcp = NULL;
	hfsmp = VTOHFS(dvp);
	*vpp = NULL;
	*cnode_locked = 0;
	isDot = FALSE;
	tvp = NULL;
	nameiop = cnp->cn_nameiop;
	flags = cnp->cn_flags;
	bzero(&desc, sizeof(desc));

	/*
	 * First check to see if it is a . or .., else look it up.
	 */
	if (flags & ISDOTDOT) {		/* Wanting the parent */
		cnp->cn_flags &= ~MAKEENTRY;
		goto found;	/* .. is always defined */
	} else if ((cnp->cn_nameptr[0] == '.') && (cnp->cn_namelen == 1)) {
		isDot = TRUE;
		cnp->cn_flags &= ~MAKEENTRY;
		goto found;	/* We always know who we are */
	} else {
		if (hfs_lock(VTOC(dvp), HFS_EXCLUSIVE_LOCK) != 0) {
			retval = ENOENT;  /* The parent no longer exists ? */
			goto exit;
		}
		dcp = VTOC(dvp);

		if (dcp->c_flag & C_DIR_MODIFICATION) {
		    // XXXdbg - if we could msleep on a lck_rw_t then we would do that
		    //          but since we can't we have to unlock, delay for a bit
		    //          and then retry...
		    // msleep((caddr_t)&dcp->c_flag, &dcp->c_rwlock, PINOD, "hfs_vnop_lookup", 0);
		    hfs_unlock(dcp);
		    tsleep((caddr_t)dvp, PRIBIO, "hfs_lookup", 1);

		    goto retry;
		}

		/* No need to go to catalog if there are no children */
		if (dcp->c_entries == 0) {
			goto notfound;
		}

		bzero(&cndesc, sizeof(cndesc));
		cndesc.cd_nameptr = (const u_int8_t *)cnp->cn_nameptr;
		cndesc.cd_namelen = cnp->cn_namelen;
		cndesc.cd_parentcnid = dcp->c_fileid;
		cndesc.cd_hint = dcp->c_childhint;

		lockflags = hfs_systemfile_lock(hfsmp, SFL_CATALOG, HFS_SHARED_LOCK);

		retval = cat_lookup(hfsmp, &cndesc, 0, &desc, &attr, &fork, NULL);
		
		hfs_systemfile_unlock(hfsmp, lockflags);

		if (retval == 0) {
			dcp->c_childhint = desc.cd_hint;
			/*
			 * Note: We must drop the parent lock here before calling
			 * hfs_getnewvnode (which takes the child lock).
			 */
		    	hfs_unlock(dcp);
		    	dcp = NULL;
			goto found;
		}
notfound:
		/*
		 * ENAMETOOLONG supersedes other errors
		 *
		 * For a CREATE or RENAME operation on the last component
		 * the ENAMETOOLONG will be handled in the next VNOP.
		 */
		if ((retval != ENAMETOOLONG) && 
		    (cnp->cn_namelen > kHFSPlusMaxFileNameChars) &&
		    (((flags & ISLASTCN) == 0) || ((nameiop != CREATE) && (nameiop != RENAME)))) {
			retval = ENAMETOOLONG;
		} else if (retval == 0) {
			retval = ENOENT;
		}
		if (retval != ENOENT)
			goto exit;
		/*
		 * This is a non-existing entry
		 *
		 * If creating, and at end of pathname and current
		 * directory has not been removed, then can consider
		 * allowing file to be created.
		 */
		if ((nameiop == CREATE || nameiop == RENAME ||
		    (nameiop == DELETE &&
		    (cnp->cn_flags & DOWHITEOUT) &&
		    (cnp->cn_flags & ISWHITEOUT))) &&
		    (flags & ISLASTCN) &&
		    !(ISSET(dcp->c_flag, C_DELETED | C_NOEXISTS))) {
			retval = EJUSTRETURN;
			goto exit;
		}
		/*
		 * Insert name into the name cache (as non-existent).
		 */
		if ((hfsmp->hfs_flags & HFS_STANDARD) == 0 &&
		    (cnp->cn_flags & MAKEENTRY) &&
		    (nameiop != CREATE)) {
			cache_enter(dvp, NULL, cnp);
			dcp->c_flag |= C_NEG_ENTRIES;
		}
		goto exit;
	}

found:
	if (flags & ISLASTCN) {
		switch(nameiop) {
		case DELETE:
			cnp->cn_flags &= ~MAKEENTRY;
			break;

		case RENAME:
			cnp->cn_flags &= ~MAKEENTRY;
			if (isDot) {
				retval = EISDIR;
				goto exit;
			}
			break;
		}
	}

	if (isDot) {
		if ((retval = vnode_get(dvp)))
			goto exit;
		*vpp = dvp;
	} else if (flags & ISDOTDOT) {
		/*
		 * Directory hard links can have multiple parents so
		 * find the appropriate parent for the current thread.
		 */
		if ((retval = hfs_vget(hfsmp, hfs_currentparent(VTOC(dvp)), &tvp, 0))) {
			goto exit;
		}
		*cnode_locked = 1;
		*vpp = tvp;
	} else {
		int type = (attr.ca_mode & S_IFMT);
#if NAMEDRSRCFORK
		int rsrc_warn = 0;

		/*
		 * Check if caller wants the resource fork but utilized
		 * the legacy "file/rsrc" access path.
		 *
		 * This is deprecated behavior and support for it will not
		 * be allowed beyond case insensitive HFS+ and even that
		 * support will be removed in the next major OS release.
		 */
		if ((type == S_IFREG) &&
		    ((flags & ISLASTCN) == 0) &&
		    (cnp->cn_nameptr[cnp->cn_namelen] == '/') &&
		    (bcmp(&cnp->cn_nameptr[cnp->cn_namelen+1], "rsrc", 5) == 0) &&
		    ((hfsmp->hfs_flags & (HFS_STANDARD | HFS_CASE_SENSITIVE)) == 0)) {
		
			cnp->cn_consume = 5;
			cnp->cn_flags |= CN_WANTSRSRCFORK | ISLASTCN | NOCACHE;
			cnp->cn_flags &= ~MAKEENTRY;
			flags |= ISLASTCN;
			rsrc_warn = 1;
		}
#endif
		if (!(flags & ISLASTCN) && (type != S_IFDIR) && (type != S_IFLNK)) {
			retval = ENOTDIR;
			goto exit;
		}
		/* Don't cache directory hardlink names. */
		if (attr.ca_recflags & kHFSHasLinkChainMask) {
			cnp->cn_flags &= ~MAKEENTRY;
		}
		/* Names with composed chars are not cached. */
		if (cnp->cn_namelen != desc.cd_namelen)
			cnp->cn_flags &= ~MAKEENTRY;

		retval = hfs_getnewvnode(hfsmp, dvp, cnp, &desc, 0, &attr, &fork, &tvp);

		if (retval) {
			/*
			 * If this was a create operation lookup and another
			 * process removed the object before we had a chance
			 * to create the vnode, then just treat it as the not
			 * found case above and return EJUSTRETURN.
			 * We should do the same for the RENAME operation since we are
			 * going to write it in regardless.
			 */
			if ((retval == ENOENT) &&
			    ((cnp->cn_nameiop == CREATE) || (cnp->cn_nameiop == RENAME)) &&
			    (flags & ISLASTCN)) {
				retval = EJUSTRETURN;
			}
			goto exit;
		}

		/* 
		 * Save the origin info for file and directory hardlinks.  Directory hardlinks 
		 * need the origin for '..' lookups, and file hardlinks need it to ensure that 
		 * competing lookups do not cause us to vend different hardlinks than the ones requested.
		 * We want to restrict saving the cache entries to LOOKUP namei operations, since
		 * we're really doing this to protect getattr.
		 */
		if ((nameiop == LOOKUP) && (VTOC(tvp)->c_flag & C_HARDLINK)) {
			hfs_savelinkorigin(VTOC(tvp), VTOC(dvp)->c_fileid);
		}
		*cnode_locked = 1;
		*vpp = tvp;
#if NAMEDRSRCFORK
		if (rsrc_warn) {
			if ((VTOC(tvp)->c_flag & C_WARNED_RSRC) == 0) {
				VTOC(tvp)->c_flag |= C_WARNED_RSRC;
				printf("hfs: %.200s: file access by '/rsrc' was deprecated in 10.4\n",
				       cnp->cn_nameptr);
			}
		}
#endif
	}
exit:
	if (dcp) {
		hfs_unlock(dcp);
	}
	cat_releasedesc(&desc);
	return (retval);
}



/*
 * Name caching works as follows:
 *
 * Names found by directory scans are retained in a cache
 * for future reference.  It is managed LRU, so frequently
 * used names will hang around.	 Cache is indexed by hash value
 * obtained from (vp, name) where vp refers to the directory
 * containing name.
 *
 * If it is a "negative" entry, (i.e. for a name that is known NOT to
 * exist) the vnode pointer will be NULL.
 *
 * Upon reaching the last segment of a path, if the reference
 * is for DELETE, or NOCACHE is set (rewrite), and the
 * name is located in the cache, it will be dropped.
 *
 */

#define	S_IXALL	0000111

__private_extern__
int
hfs_vnop_lookup(struct vnop_lookup_args *ap)
{
	struct vnode *dvp = ap->a_dvp;
	struct vnode *vp;
	struct cnode *cp;
	struct cnode *dcp;
	int error;
	struct vnode **vpp = ap->a_vpp;
	struct componentname *cnp = ap->a_cnp;
	int flags = cnp->cn_flags;
	int cnode_locked;

	*vpp = NULL;
	dcp = VTOC(dvp);

	/*
	 * Lookup an entry in the cache
	 *
	 * If the lookup succeeds, the vnode is returned in *vpp,
	 * and a status of -1 is returned.
	 *
	 * If the lookup determines that the name does not exist
	 * (negative cacheing), a status of ENOENT is returned.
	 *
	 * If the lookup fails, a status of zero is returned.
	 */
	error = cache_lookup(dvp, vpp, cnp);
	if (error != -1) {
		if ((error == ENOENT) && (cnp->cn_nameiop != CREATE))		
			goto exit;	/* found a negative cache entry */
		goto lookup;		/* did not find it in the cache */
	}
	/*
	 * We have a name that matched
	 * cache_lookup returns the vp with an iocount reference already taken
	 */
	error = 0;
	vp = *vpp;

	/*
	 * If this is a hard-link vnode then we need to update
	 * the name (of the link), the parent ID, the cnid, the
	 * text encoding and the catalog hint.  This enables
	 * getattrlist calls to return the correct link info.
	 */
	cp = VTOC(vp);

	if ((flags & ISLASTCN) && (cp->c_flag & C_HARDLINK)) {
		hfs_lock(cp, HFS_FORCE_LOCK);
		if ((cp->c_parentcnid != dcp->c_cnid) ||
		    (bcmp(cnp->cn_nameptr, cp->c_desc.cd_nameptr, cp->c_desc.cd_namelen) != 0)) {
			struct cat_desc desc;
			int lockflags;

			/*
			 * Get an updated descriptor
			 */
			desc.cd_nameptr = (const u_int8_t *)cnp->cn_nameptr;
			desc.cd_namelen = cnp->cn_namelen;
			desc.cd_parentcnid = dcp->c_fileid;
			desc.cd_hint = dcp->c_childhint;
			desc.cd_encoding = 0;
			desc.cd_cnid = 0;
			desc.cd_flags = S_ISDIR(cp->c_mode) ? CD_ISDIR : 0;
	

			lockflags = hfs_systemfile_lock(VTOHFS(dvp), SFL_CATALOG, HFS_SHARED_LOCK);		
			if (cat_lookup(VTOHFS(vp), &desc, 0, &desc, NULL, NULL, NULL) == 0)
				replace_desc(cp, &desc);
			hfs_systemfile_unlock(VTOHFS(dvp), lockflags);

			/* 
			 * Save the origin info for file and directory hardlinks.  Directory hardlinks 
			 * need the origin for '..' lookups, and file hardlinks need it to ensure that 
			 * competing lookups do not cause us to vend different hardlinks than the ones requested.
			 * We want to restrict saving the cache entries to LOOKUP namei operations, since
			 * we're really doing this to protect getattr.
			 */
			if (cnp->cn_nameiop == LOOKUP) {
				hfs_savelinkorigin(cp, dcp->c_fileid);
			}
		}
		hfs_unlock(cp);
	}
#if NAMEDRSRCFORK
	/*
	 * Check if caller wants the resource fork but utilized
	 * the legacy "file/rsrc" access path.
	 *
	 * This is deprecated behavior and support for it will not
	 * be allowed beyond case insensitive HFS+ and even that
	 * support will be removed in the next major OS release.
	 */
	if ((dvp != vp) &&
	    ((flags & ISLASTCN) == 0) &&
	    vnode_isreg(vp) &&
	    (cnp->cn_nameptr[cnp->cn_namelen] == '/') &&
	    (bcmp(&cnp->cn_nameptr[cnp->cn_namelen+1], "rsrc", 5) == 0) &&
	    ((VTOHFS(vp)->hfs_flags & (HFS_STANDARD | HFS_CASE_SENSITIVE)) == 0)) {		
		cnp->cn_consume = 5;
		cnp->cn_flags |= CN_WANTSRSRCFORK | ISLASTCN | NOCACHE;
		cnp->cn_flags &= ~MAKEENTRY;

		hfs_lock(cp, HFS_FORCE_LOCK);
		if ((cp->c_flag & C_WARNED_RSRC) == 0) {
			cp->c_flag |= C_WARNED_RSRC;
			printf("hfs: %.200s: file access by '/rsrc' was deprecated in 10.4\n", cnp->cn_nameptr);
		}
		hfs_unlock(cp);
	}
#endif
	return (error);
	
lookup:
	/*
	 * The vnode was not in the name cache or it was stale.
	 *
	 * So we need to do a real lookup.
	 */
	cnode_locked = 0;

	error = hfs_lookup(dvp, vpp, cnp, &cnode_locked);
	
	if (cnode_locked)
		hfs_unlock(VTOC(*vpp));
exit:
	return (error);
}



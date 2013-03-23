/*
 * Copyright (c) 2002-2006 Apple Computer, Inc. All rights reserved.
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
 * Copyright (c) 1982, 1986, 1989, 1991, 1993, 1995
 *	The Regents of the University of California.  All rights reserved.
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
 *	@(#)hfs_chash.c
 *	derived from @(#)ufs_ihash.c	8.7 (Berkeley) 5/17/95
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/vnode.h>
#include <sys/kernel.h>
#include <sys/malloc.h>
#include <sys/proc.h>
#include <sys/queue.h>


#include "hfs.h"	/* XXX bringup */
#include "hfs_cnode.h"

extern lck_attr_t *  hfs_lock_attr;
extern lck_grp_t *  hfs_mutex_group;
extern lck_grp_t *  hfs_rwlock_group;

lck_grp_t * chash_lck_grp;
lck_grp_attr_t * chash_lck_grp_attr;
lck_attr_t * chash_lck_attr;

/*
 * Structures associated with cnode caching.
 */
LIST_HEAD(cnodehashhead, cnode) *cnodehashtbl;
u_long	cnodehash;		/* size of hash table - 1 */
#define CNODEHASH(device, inum) (&cnodehashtbl[((device) + (inum)) & cnodehash])

lck_mtx_t  hfs_chash_mutex;

/*
 * Initialize cnode hash table.
 */
__private_extern__
void
hfs_chashinit()
{
	chash_lck_grp_attr= lck_grp_attr_alloc_init();
	chash_lck_grp  = lck_grp_alloc_init("cnode_hash", chash_lck_grp_attr);
	chash_lck_attr = lck_attr_alloc_init();

	lck_mtx_init(&hfs_chash_mutex, chash_lck_grp, chash_lck_attr);
}

static void hfs_chash_lock(void) 
{
	lck_mtx_lock(&hfs_chash_mutex);
}

static void hfs_chash_unlock(void) 
{
	lck_mtx_unlock(&hfs_chash_mutex);
}

__private_extern__
void
hfs_chashinit_finish()
{
	hfs_chash_lock();
	if (!cnodehashtbl)
		cnodehashtbl = hashinit(desiredvnodes, M_HFSMNT, &cnodehash);
	hfs_chash_unlock();
}


/*
 * Use the device, inum pair to find the incore cnode.
 *
 * If it is in core, but locked, wait for it.
 */
__private_extern__
struct vnode *
hfs_chash_getvnode(dev_t dev, ino_t inum, int wantrsrc, int skiplock)
{
	struct cnode *cp;
	struct vnode *vp;
	int error;
	u_int32_t vid;

	/* 
	 * Go through the hash list
	 * If a cnode is in the process of being cleaned out or being
	 * allocated, wait for it to be finished and then try again.
	 */
loop:
	hfs_chash_lock();
	for (cp = CNODEHASH(dev, inum)->lh_first; cp; cp = cp->c_hash.le_next) {
		if ((cp->c_fileid != inum) || (cp->c_dev != dev))
			continue;
		/* Wait if cnode is being created or reclaimed. */
		if (ISSET(cp->c_hflag, H_ALLOC | H_TRANSIT | H_ATTACH)) {
		        SET(cp->c_hflag, H_WAITING);

			(void) msleep(cp, &hfs_chash_mutex, PDROP | PINOD,
			              "hfs_chash_getvnode", 0);
			goto loop;
		}
		/* Obtain the desired vnode. */
		vp = wantrsrc ? cp->c_rsrc_vp : cp->c_vp;
		if (vp == NULLVP)
			goto exit;

		vid = vnode_vid(vp);
		hfs_chash_unlock();

		if ((error = vnode_getwithvid(vp, vid))) {
		        /*
			 * If vnode is being reclaimed, or has
			 * already changed identity, no need to wait
			 */
		        return (NULL);
		}
		if (!skiplock && hfs_lock(cp, HFS_EXCLUSIVE_LOCK) != 0) {
			vnode_put(vp);
			return (NULL);
		}

		/*
		 * Skip cnodes that are not in the name space anymore
		 * we need to check with the cnode lock held because
		 * we may have blocked acquiring the vnode ref or the
		 * lock on the cnode which would allow the node to be
		 * unlinked
		 */
		if (cp->c_flag & (C_NOEXISTS | C_DELETED)) {
			if (!skiplock)
		        	hfs_unlock(cp);
			vnode_put(vp);

			return (NULL);
		}			
		return (vp);
	}
exit:
	hfs_chash_unlock();
	return (NULL);
}


/*
 * Use the device, fileid pair to snoop an incore cnode.
 */
__private_extern__
int
hfs_chash_snoop(dev_t dev, ino_t inum, int (*callout)(const struct cat_desc *,
                const struct cat_attr *, void *), void * arg)
{
	struct cnode *cp;
	int result = ENOENT;

	/* 
	 * Go through the hash list
	 * If a cnode is in the process of being cleaned out or being
	 * allocated, wait for it to be finished and then try again.
	 */
	hfs_chash_lock();
	for (cp = CNODEHASH(dev, inum)->lh_first; cp; cp = cp->c_hash.le_next) {
		if ((cp->c_fileid != inum) || (cp->c_dev != dev))
			continue;
		/* Skip cnodes being created or reclaimed. */
		if (!ISSET(cp->c_hflag, H_ALLOC | H_TRANSIT | H_ATTACH)) {
			result = callout(&cp->c_desc, &cp->c_attr, arg);
		}
		break;
	}
	hfs_chash_unlock();
	return (result);
}


/*
 * Use the device, fileid pair to find the incore cnode.
 * If no cnode if found one is created
 *
 * If it is in core, but locked, wait for it.
 *
 * If the cnode is C_DELETED, then return NULL since that 
 * inum is no longer valid for lookups (open-unlinked file).
 */
__private_extern__
struct cnode *
hfs_chash_getcnode(dev_t dev, ino_t inum, struct vnode **vpp, int wantrsrc, int skiplock)
{
	struct cnode	*cp;
	struct cnode	*ncp = NULL;
	vnode_t		vp;
	u_int32_t	vid;

	/* 
	 * Go through the hash list
	 * If a cnode is in the process of being cleaned out or being
	 * allocated, wait for it to be finished and then try again.
	 */
loop:
	hfs_chash_lock();

loop_with_lock:
	for (cp = CNODEHASH(dev, inum)->lh_first; cp; cp = cp->c_hash.le_next) {
		if ((cp->c_fileid != inum) || (cp->c_dev != dev))
			continue;
		/*
		 * Wait if cnode is being created, attached to or reclaimed.
		 */
		if (ISSET(cp->c_hflag, H_ALLOC | H_ATTACH | H_TRANSIT)) {
		        SET(cp->c_hflag, H_WAITING);

			(void) msleep(cp, &hfs_chash_mutex, PINOD,
			              "hfs_chash_getcnode", 0);
			goto loop_with_lock;
		}
		vp = wantrsrc ? cp->c_rsrc_vp : cp->c_vp;
		if (vp == NULL) {
			/*
			 * The desired vnode isn't there so tag the cnode.
			 */
			SET(cp->c_hflag, H_ATTACH);

			hfs_chash_unlock();
		} else {
			vid = vnode_vid(vp);

			hfs_chash_unlock();

			if (vnode_getwithvid(vp, vid))
		        	goto loop;
		}
		if (ncp) {
		        /*
			 * someone else won the race to create
			 * this cnode and add it to the hash
			 * just dump our allocation
			 */
		        FREE_ZONE(ncp, sizeof(struct cnode), M_HFSNODE);
			ncp = NULL;
		}

		if (!skiplock) {
			hfs_lock(cp, HFS_FORCE_LOCK);
		}

		/*
		 * Skip cnodes that are not in the name space anymore
		 * we need to check with the cnode lock held because
		 * we may have blocked acquiring the vnode ref or the
		 * lock on the cnode which would allow the node to be
		 * unlinked.
		 *
		 * Don't return a cnode in this case since the inum
		 * is no longer valid for lookups.
		 */
		if ((cp->c_flag & (C_NOEXISTS | C_DELETED)) && !wantrsrc) {
			if (!skiplock)
				hfs_unlock(cp);
			if (vp != NULLVP) {
				vnode_put(vp);
			} else {
				hfs_chash_lock();
		        	CLR(cp->c_hflag, H_ATTACH);
				if (ISSET(cp->c_hflag, H_WAITING)) {
					CLR(cp->c_hflag, H_WAITING);
					wakeup((caddr_t)cp);
				}
				hfs_chash_unlock();
			}
			vp = NULL;
			cp = NULL;
		}
		*vpp = vp;
		return (cp);
	}

	/* 
	 * Allocate a new cnode
	 */
	if (skiplock && !wantrsrc)
		panic("%s - should never get here when skiplock is set \n", __FUNCTION__);

	if (ncp == NULL) {
		hfs_chash_unlock();

	        MALLOC_ZONE(ncp, struct cnode *, sizeof(struct cnode), M_HFSNODE, M_WAITOK);
		/*
		 * since we dropped the chash lock, 
		 * we need to go back and re-verify
		 * that this node hasn't come into 
		 * existence...
		 */
		goto loop;
	}
	bzero(ncp, sizeof(struct cnode));
	SET(ncp->c_hflag, H_ALLOC);
	ncp->c_fileid = inum;
	ncp->c_dev = dev;
	TAILQ_INIT(&ncp->c_hintlist); /* make the list empty */
	TAILQ_INIT(&ncp->c_originlist);

	lck_rw_init(&ncp->c_rwlock, hfs_rwlock_group, hfs_lock_attr);
	if (!skiplock)
		(void) hfs_lock(ncp, HFS_EXCLUSIVE_LOCK);

	/* Insert the new cnode with it's H_ALLOC flag set */
	LIST_INSERT_HEAD(CNODEHASH(dev, inum), ncp, c_hash);
	hfs_chash_unlock();

	*vpp = NULL;
	return (ncp);
}


__private_extern__
void
hfs_chashwakeup(struct cnode *cp, int hflags)
{
	hfs_chash_lock();

	CLR(cp->c_hflag, hflags);

	if (ISSET(cp->c_hflag, H_WAITING)) {
	        CLR(cp->c_hflag, H_WAITING);
		wakeup((caddr_t)cp);
	}
	hfs_chash_unlock();
}


/*
 * Re-hash two cnodes in the hash table.
 */
__private_extern__
void
hfs_chash_rehash(struct cnode *cp1, struct cnode *cp2)
{
	hfs_chash_lock();

	LIST_REMOVE(cp1, c_hash);
	LIST_REMOVE(cp2, c_hash);
	LIST_INSERT_HEAD(CNODEHASH(cp1->c_dev, cp1->c_fileid), cp1, c_hash);
	LIST_INSERT_HEAD(CNODEHASH(cp2->c_dev, cp2->c_fileid), cp2, c_hash);

	hfs_chash_unlock();
}


/*
 * Remove a cnode from the hash table.
 */
__private_extern__
int
hfs_chashremove(struct cnode *cp)
{
	hfs_chash_lock();

	/* Check if a vnode is getting attached */
	if (ISSET(cp->c_hflag, H_ATTACH)) {
		hfs_chash_unlock();
		return (EBUSY);
	}
	if (cp->c_hash.le_next || cp->c_hash.le_prev) {
	    LIST_REMOVE(cp, c_hash);
	    cp->c_hash.le_next = NULL;
	    cp->c_hash.le_prev = NULL;
	}
	hfs_chash_unlock();
	return (0);
}

/*
 * Remove a cnode from the hash table and wakeup any waiters.
 */
__private_extern__
void
hfs_chash_abort(struct cnode *cp)
{
	hfs_chash_lock();

	LIST_REMOVE(cp, c_hash);
	cp->c_hash.le_next = NULL;
	cp->c_hash.le_prev = NULL;

	CLR(cp->c_hflag, H_ATTACH | H_ALLOC);
	if (ISSET(cp->c_hflag, H_WAITING)) {
	        CLR(cp->c_hflag, H_WAITING);
		wakeup((caddr_t)cp);
	}
	hfs_chash_unlock();
}


/*
 * mark a cnode as in transition
 */
__private_extern__
void
hfs_chash_mark_in_transit(struct cnode *cp)
{
	hfs_chash_lock();

        SET(cp->c_hflag, H_TRANSIT);

	hfs_chash_unlock();
}

/* Search a cnode in the hash.  This function does not return cnode which 
 * are getting created, destroyed or in transition.  Note that this function
 * does not acquire the cnode hash mutex, and expects the caller to acquire it.
 * On success, returns pointer to the cnode found.  On failure, returns NULL.
 */
static 
struct cnode *
hfs_chash_search_cnid(dev_t dev, cnid_t cnid) 
{
	struct cnode *cp;

	for (cp = CNODEHASH(dev, cnid)->lh_first; cp; cp = cp->c_hash.le_next) {
		if ((cp->c_fileid == cnid) && (cp->c_dev == dev)) {
			break;
		}
	}

	/* If cnode is being created or reclaimed, return error. */
	if (cp && ISSET(cp->c_hflag, H_ALLOC | H_TRANSIT | H_ATTACH)) {
		cp = NULL;
	}

	return cp;
}

/* Search a cnode corresponding to given device and ID in the hash.  If the 
 * found cnode has kHFSHasChildLinkBit cleared, set it.  If the cnode is not 
 * found, no new cnode is created and error is returned.
 * 
 * Return values - 
 *	-1 : The cnode was not found.
 * 	 0 : The cnode was found, and the kHFSHasChildLinkBit was already set.
 *	 1 : The cnode was found, the kHFSHasChildLinkBit was not set, and the 
 *	     function had to set that bit.
 */
__private_extern__ 
int
hfs_chash_set_childlinkbit(dev_t dev, cnid_t cnid)
{
	int retval = -1;
	struct cnode *cp;

	hfs_chash_lock();
	cp = hfs_chash_search_cnid(dev, cnid);
	if (cp) {
		if (cp->c_attr.ca_recflags & kHFSHasChildLinkMask) {
			retval = 0;
		} else {
			cp->c_attr.ca_recflags |= kHFSHasChildLinkMask;
			retval = 1;
		}
	}
	hfs_chash_unlock();

	return retval;
}

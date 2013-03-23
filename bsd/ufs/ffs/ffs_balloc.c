/*
 * Copyright (c) 2000-2002 Apple Computer, Inc. All rights reserved.
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
/*
 * Copyright (c) 1982, 1986, 1989, 1993
 *	The Regents of the University of California.  All rights reserved.
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
 *	@(#)ffs_balloc.c	8.8 (Berkeley) 6/16/95
 */

#include <rev_endian_fs.h>
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/buf_internal.h>
#include <sys/proc.h>
#include <sys/kauth.h>
#include <sys/file.h>
#include <sys/vnode_internal.h>
#include <sys/ubc.h>
#include <sys/quota.h>

#if REV_ENDIAN_FS
#include <sys/mount_internal.h>
#endif /* REV_ENDIAN_FS */

#include <sys/vm.h>

#include <ufs/ufs/quota.h>
#include <ufs/ufs/inode.h>
#include <ufs/ufs/ufs_extern.h>

#include <ufs/ffs/fs.h>
#include <ufs/ffs/ffs_extern.h>

#if REV_ENDIAN_FS
#include <ufs/ufs/ufs_byte_order.h>
#include <libkern/OSByteOrder.h>
#endif /* REV_ENDIAN_FS */

/*
 * Balloc defines the structure of file system storage
 * by allocating the physical blocks on a device given
 * the inode and the logical block number in a file.
 */
ffs_balloc(
	register struct inode *ip,
	register ufs_daddr_t lbn,
	int size,
	kauth_cred_t cred,
	struct buf **bpp,
	int flags,
	int * blk_alloc)
{
	register struct fs *fs;
	register ufs_daddr_t nb;
	struct buf *bp, *nbp;
	struct vnode *vp = ITOV(ip);
	struct indir indirs[NIADDR + 2];
	ufs_daddr_t newb, *bap, pref;
	int deallocated, osize, nsize, num, i, error;
	ufs_daddr_t *allocib, *blkp, *allocblk, allociblk[NIADDR + 1];
	int devBlockSize=0;
	int alloc_buffer = 1;
	struct mount *mp=vp->v_mount;
#if REV_ENDIAN_FS
	int rev_endian=(mp->mnt_flag & MNT_REVEND);
#endif /* REV_ENDIAN_FS */

	*bpp = NULL;
	if (lbn < 0)
		return (EFBIG);
	fs = ip->i_fs;
	if (flags & B_NOBUFF) 
		alloc_buffer = 0;

	if (blk_alloc)
		*blk_alloc = 0;

	/*
	 * If the next write will extend the file into a new block,
	 * and the file is currently composed of a fragment
	 * this fragment has to be extended to be a full block.
	 */
	nb = lblkno(fs, ip->i_size);
	if (nb < NDADDR && nb < lbn) {
		/* the filesize prior to this write  can fit in direct 
		 * blocks (ie.  fragmentaion is possibly done)
		 * we are now extending the file write beyond 
		 * the block which has end of file prior to this write 
		 */
		osize = blksize(fs, ip, nb); 
		/* osize gives disk allocated size in the last block. It is 
		 * either in fragments or a file system block size */
		if (osize < fs->fs_bsize && osize > 0) {
			/* few fragments are already allocated,since the
			 * current extends beyond this block 
			 * allocate the complete block as fragments are only
			 * in last block
			 */
			error = ffs_realloccg(ip, nb,
				ffs_blkpref(ip, nb, (int)nb, &ip->i_db[0]),
				osize, (int)fs->fs_bsize, cred, &bp);
			if (error)
				return (error);
			/* adjust the inode size we just grew */
			/* it is in nb+1 as nb starts from 0 */
			ip->i_size = (nb + 1) * fs->fs_bsize;
			ubc_setsize(vp, (off_t)ip->i_size);

			ip->i_db[nb] = dbtofsb(fs, (ufs_daddr_t)buf_blkno(bp));
			ip->i_flag |= IN_CHANGE | IN_UPDATE;

			if ((flags & B_SYNC) || (!alloc_buffer)) {
				if (!alloc_buffer) 
					buf_setflags(bp, B_NOCACHE);
				buf_bwrite(bp);
			} else
				buf_bdwrite(bp);
			/* note that bp is already released here */
		}
	}
	/*
	 * The first NDADDR blocks are direct blocks
	 */
	if (lbn < NDADDR) {
		nb = ip->i_db[lbn];
		if (nb != 0 && ip->i_size >= (lbn + 1) * fs->fs_bsize) {
			if (alloc_buffer) {
			error = (int)buf_bread(vp, (daddr64_t)((unsigned)lbn), fs->fs_bsize, NOCRED, &bp);
			if (error) {
				buf_brelse(bp);
				return (error);
			}
			*bpp = bp;
			}
			return (0);
		}
		if (nb != 0) {
			/*
			 * Consider need to reallocate a fragment.
			 */
			osize = fragroundup(fs, blkoff(fs, ip->i_size));
			nsize = fragroundup(fs, size);
			if (nsize <= osize) {
				if (alloc_buffer) {
				error = (int)buf_bread(vp, (daddr64_t)((unsigned)lbn), osize, NOCRED, &bp);
				if (error) {
					buf_brelse(bp);
					return (error);
				}
				ip->i_flag |= IN_CHANGE | IN_UPDATE;
				*bpp = bp;
				return (0);
				}
				else {
					ip->i_flag |= IN_CHANGE | IN_UPDATE;
					return (0);
				}
			} else {
				error = ffs_realloccg(ip, lbn,
				    ffs_blkpref(ip, lbn, (int)lbn,
					&ip->i_db[0]), osize, nsize, cred, &bp);
				if (error)
					return (error);
				ip->i_db[lbn] = dbtofsb(fs, (ufs_daddr_t)buf_blkno(bp));
				ip->i_flag |= IN_CHANGE | IN_UPDATE;

				/* adjust the inode size we just grew */
				ip->i_size = (lbn * fs->fs_bsize) + size;
				ubc_setsize(vp, (off_t)ip->i_size);

				if (!alloc_buffer) {
					buf_setflags(bp, B_NOCACHE);
					if (flags & B_SYNC)
						buf_bwrite(bp);
					else
						buf_bdwrite(bp);
				 } else
					*bpp = bp;
				return (0);

			}
		} else {
			if (ip->i_size < (lbn + 1) * fs->fs_bsize)
				nsize = fragroundup(fs, size);
			else
				nsize = fs->fs_bsize;
			error = ffs_alloc(ip, lbn,
			    ffs_blkpref(ip, lbn, (int)lbn, &ip->i_db[0]),
			    nsize, cred, &newb);
			if (error)
				return (error);
			if (alloc_buffer) {
			        bp = buf_getblk(vp, (daddr64_t)((unsigned)lbn), nsize, 0, 0, BLK_WRITE);
				buf_setblkno(bp, (daddr64_t)((unsigned)fsbtodb(fs, newb)));

				if (flags & B_CLRBUF)
				        buf_clear(bp);
			}
			ip->i_db[lbn] = newb;
			ip->i_flag |= IN_CHANGE | IN_UPDATE;
			if (blk_alloc) {
				*blk_alloc = nsize;
			}
			if (alloc_buffer)
				*bpp = bp;
			return (0);
		}
	}
	/*
	 * Determine the number of levels of indirection.
	 */
	pref = 0;
	if (error = ufs_getlbns(vp, lbn, indirs, &num))
		return(error);
#if DIAGNOSTIC
	if (num < 1)
		panic ("ffs_balloc: ufs_bmaparray returned indirect block");
#endif
	/*
	 * Fetch the first indirect block allocating if necessary.
	 */
	--num;
	nb = ip->i_ib[indirs[0].in_off];
	allocib = NULL;
	allocblk = allociblk;
	if (nb == 0) {
		pref = ffs_blkpref(ip, lbn, 0, (ufs_daddr_t *)0);
	        if (error = ffs_alloc(ip, lbn, pref, (int)fs->fs_bsize,
		    cred, &newb))
			return (error);
		nb = newb;
		*allocblk++ = nb;
		bp = buf_getblk(vp, (daddr64_t)((unsigned)(indirs[1].in_lbn)), fs->fs_bsize, 0, 0, BLK_META);
		buf_setblkno(bp, (daddr64_t)((unsigned)fsbtodb(fs, nb)));
		buf_clear(bp);
		/*
		 * Write synchronously conditional on mount flags.
		 */
		if ((vp)->v_mount->mnt_flag & MNT_ASYNC) {
			error = 0;
			buf_bdwrite(bp);
		} else if ((error = buf_bwrite(bp)) != 0) {
			goto fail;
		}
		allocib = &ip->i_ib[indirs[0].in_off];
		*allocib = nb;
		ip->i_flag |= IN_CHANGE | IN_UPDATE;
	}
	/*
	 * Fetch through the indirect blocks, allocating as necessary.
	 */
	for (i = 1;;) {
		error = (int)buf_meta_bread(vp, (daddr64_t)((unsigned)(indirs[i].in_lbn)), (int)fs->fs_bsize, NOCRED, &bp);
		if (error) {
			buf_brelse(bp);
			goto fail;
		}
		bap = (ufs_daddr_t *)buf_dataptr(bp);
#if	REV_ENDIAN_FS
	if (rev_endian)
		nb = OSSwapInt32(bap[indirs[i].in_off]);
	else {
#endif	/* REV_ENDIAN_FS */
		nb = bap[indirs[i].in_off];
#if REV_ENDIAN_FS
	}
#endif /* REV_ENDIAN_FS */
		if (i == num)
			break;
		i += 1;
		if (nb != 0) {
			buf_brelse(bp);
			continue;
		}
		if (pref == 0)
			pref = ffs_blkpref(ip, lbn, 0, (ufs_daddr_t *)0);
		if (error =
		    ffs_alloc(ip, lbn, pref, (int)fs->fs_bsize, cred, &newb)) {
			buf_brelse(bp);
			goto fail;
		}
		nb = newb;
		*allocblk++ = nb;
		nbp = buf_getblk(vp, (daddr64_t)((unsigned)(indirs[i].in_lbn)), fs->fs_bsize, 0, 0, BLK_META);
		buf_setblkno(nbp, (daddr64_t)((unsigned)fsbtodb(fs, nb)));
		buf_clear(nbp);
		/*
		 * Write synchronously conditional on mount flags.
		 */
		if ((vp)->v_mount->mnt_flag & MNT_ASYNC) {
			error = 0;
			buf_bdwrite(nbp);
		} else if (error = buf_bwrite(nbp)) {
			buf_brelse(bp);
			goto fail;
		}
#if	REV_ENDIAN_FS
	if (rev_endian)
		bap[indirs[i - 1].in_off] = OSSwapInt32(nb);
	else {
#endif	/* REV_ENDIAN_FS */
		bap[indirs[i - 1].in_off] = nb;
#if	REV_ENDIAN_FS
	}
#endif	/* REV_ENDIAN_FS */
		/*
		 * If required, write synchronously, otherwise use
		 * delayed write.
		 */
		if (flags & B_SYNC) {
			buf_bwrite(bp);
		} else {
			buf_bdwrite(bp);
		}
	}
	/*
	 * Get the data block, allocating if necessary.
	 */
	if (nb == 0) {
		pref = ffs_blkpref(ip, lbn, indirs[i].in_off, &bap[0]);
		if (error = ffs_alloc(ip,
		    lbn, pref, (int)fs->fs_bsize, cred, &newb)) {
			buf_brelse(bp);
			goto fail;
		}
		nb = newb;
		*allocblk++ = nb;
#if	REV_ENDIAN_FS
	if (rev_endian)
		bap[indirs[i].in_off] = OSSwapInt32(nb);
	else {
#endif	/* REV_ENDIAN_FS */
		bap[indirs[i].in_off] = nb;
#if	REV_ENDIAN_FS
	}
#endif	/* REV_ENDIAN_FS */
		/*
		 * If required, write synchronously, otherwise use
		 * delayed write.
		 */
		if ((flags & B_SYNC)) {
			buf_bwrite(bp);
		} else {
			buf_bdwrite(bp);
		}
		if(alloc_buffer ) {
		nbp = buf_getblk(vp, (daddr64_t)((unsigned)lbn), fs->fs_bsize, 0, 0, BLK_WRITE);
		buf_setblkno(nbp, (daddr64_t)((unsigned)fsbtodb(fs, nb)));

		if (flags & B_CLRBUF)
			buf_clear(nbp);
		}
		if (blk_alloc) {
			*blk_alloc = fs->fs_bsize;
		}
		if(alloc_buffer) 
			*bpp = nbp;

		return (0);
	}
	buf_brelse(bp);
	if (alloc_buffer) {
	        if (flags & B_CLRBUF) {
		        error = (int)buf_bread(vp, (daddr64_t)((unsigned)lbn), (int)fs->fs_bsize, NOCRED, &nbp);
			if (error) {
			        buf_brelse(nbp);
				goto fail;
			}
		} else {
		        nbp = buf_getblk(vp, (daddr64_t)((unsigned)lbn), fs->fs_bsize, 0, 0, BLK_WRITE);
			buf_setblkno(nbp, (daddr64_t)((unsigned)fsbtodb(fs, nb)));
		}
		*bpp = nbp;
	}
	return (0);
fail:
	/*
	 * If we have failed part way through block allocation, we
	 * have to deallocate any indirect blocks that we have allocated.
	 */
	for (deallocated = 0, blkp = allociblk; blkp < allocblk; blkp++) {
		ffs_blkfree(ip, *blkp, fs->fs_bsize);
		deallocated += fs->fs_bsize;
	}
	if (allocib != NULL)
		*allocib = 0;
	if (deallocated) {
	        devBlockSize = vfs_devblocksize(mp);
#if QUOTA
		/*
		 * Restore user's disk quota because allocation failed.
		 */
		(void) chkdq(ip, (int64_t)-deallocated, cred, FORCE);
#endif /* QUOTA */
		ip->i_blocks -= btodb(deallocated, devBlockSize);
		ip->i_flag |= IN_CHANGE | IN_UPDATE;
	}
	return (error);
}

/*
 * ffs_blkalloc allocates a disk block for ffs_pageout(), as a consequence
 * it does no buf_breads (that could lead to deadblock as the page may be already
 * marked busy as it is being paged out. Also important to note that we are not
 * growing the file in pageouts. So ip->i_size  cannot increase by this call
 * due to the way UBC works.  
 * This code is derived from ffs_balloc and many cases of that are  dealt
 * in ffs_balloc are not applicable here 
 * Do not call with B_CLRBUF flags as this should only be called only 
 * from pageouts
 */
ffs_blkalloc(
	struct inode *ip,
	ufs_daddr_t lbn,
	int size,
	kauth_cred_t cred,
	int flags)
{
	register struct fs *fs;
	register ufs_daddr_t nb;
	struct buf *bp, *nbp;
	struct vnode *vp = ITOV(ip);
	struct indir indirs[NIADDR + 2];
	ufs_daddr_t newb, *bap, pref;
	int deallocated, osize, nsize, num, i, error;
	ufs_daddr_t *allocib, *blkp, *allocblk, allociblk[NIADDR + 1];
	int devBlockSize=0;
	struct mount *mp=vp->v_mount;
#if REV_ENDIAN_FS
	int rev_endian=(mp->mnt_flag & MNT_REVEND);
#endif /* REV_ENDIAN_FS */

	fs = ip->i_fs;

	if(size > fs->fs_bsize)
		panic("ffs_blkalloc: too large for allocation");

	/*
	 * If the next write will extend the file into a new block,
	 * and the file is currently composed of a fragment
	 * this fragment has to be extended to be a full block.
	 */
	nb = lblkno(fs, ip->i_size);
	if (nb < NDADDR && nb < lbn) {
		panic("ffs_blkalloc():cannot extend file: i_size %d, lbn %d", ip->i_size, lbn);
	}
	/*
	 * The first NDADDR blocks are direct blocks
	 */
	if (lbn < NDADDR) {
		nb = ip->i_db[lbn];
		if (nb != 0 && ip->i_size >= (lbn + 1) * fs->fs_bsize) {
		/* TBD: trivial case; the block  is already allocated */
			return (0);
		}
		if (nb != 0) {
			/*
			 * Consider need to reallocate a fragment.
			 */
			osize = fragroundup(fs, blkoff(fs, ip->i_size));
			nsize = fragroundup(fs, size);
			if (nsize > osize) {
				panic("ffs_allocblk: trying to extend a fragment");
			}
			return(0);
		} else {
			if (ip->i_size < (lbn + 1) * fs->fs_bsize)
				nsize = fragroundup(fs, size);
			else
				nsize = fs->fs_bsize;
			error = ffs_alloc(ip, lbn,
			    ffs_blkpref(ip, lbn, (int)lbn, &ip->i_db[0]),
			    nsize, cred, &newb);
			if (error)
				return (error);
			ip->i_db[lbn] = newb;
			ip->i_flag |= IN_CHANGE | IN_UPDATE;
			return (0);
		}
	}
	/*
	 * Determine the number of levels of indirection.
	 */
	pref = 0;
	if (error = ufs_getlbns(vp, lbn, indirs, &num))
		return(error);

	if(num == 0) {
		panic("ffs_blkalloc: file with direct blocks only"); 
	}

	/*
	 * Fetch the first indirect block allocating if necessary.
	 */
	--num;
	nb = ip->i_ib[indirs[0].in_off];
	allocib = NULL;
	allocblk = allociblk;
	if (nb == 0) {
		pref = ffs_blkpref(ip, lbn, 0, (ufs_daddr_t *)0);
	        if (error = ffs_alloc(ip, lbn, pref, (int)fs->fs_bsize,
		    cred, &newb))
			return (error);
		nb = newb;
		*allocblk++ = nb;
		bp = buf_getblk(vp, (daddr64_t)((unsigned)(indirs[1].in_lbn)), fs->fs_bsize, 0, 0, BLK_META);
		buf_setblkno(bp, (daddr64_t)((unsigned)fsbtodb(fs, nb)));
		buf_clear(bp);
		/*
		 * Write synchronously conditional on mount flags.
		 */
		if ((vp)->v_mount->mnt_flag & MNT_ASYNC) {
			error = 0;
			buf_bdwrite(bp);
		} else if (error = buf_bwrite(bp)) {
			goto fail;
		}
		allocib = &ip->i_ib[indirs[0].in_off];
		*allocib = nb;
		ip->i_flag |= IN_CHANGE | IN_UPDATE;
	}
	/*
	 * Fetch through the indirect blocks, allocating as necessary.
	 */
	for (i = 1;;) {
		error = (int)buf_meta_bread(vp, (daddr64_t)((unsigned)(indirs[i].in_lbn)), (int)fs->fs_bsize, NOCRED, &bp);
		if (error) {
			buf_brelse(bp);
			goto fail;
		}
		bap = (ufs_daddr_t *)buf_dataptr(bp);
#if	REV_ENDIAN_FS
	if (rev_endian)
		nb = OSSwapInt32(bap[indirs[i].in_off]);
	else {
#endif	/* REV_ENDIAN_FS */
		nb = bap[indirs[i].in_off];
#if REV_ENDIAN_FS
	}
#endif /* REV_ENDIAN_FS */
		if (i == num)
			break;
		i += 1;
		if (nb != 0) {
			buf_brelse(bp);
			continue;
		}
		if (pref == 0)
			pref = ffs_blkpref(ip, lbn, 0, (ufs_daddr_t *)0);
		if (error =
		    ffs_alloc(ip, lbn, pref, (int)fs->fs_bsize, cred, &newb)) {
			buf_brelse(bp);
			goto fail;
		}
		nb = newb;
		*allocblk++ = nb;
		nbp = buf_getblk(vp, (daddr64_t)((unsigned)(indirs[i].in_lbn)), fs->fs_bsize, 0, 0, BLK_META);
		buf_setblkno(nbp, (daddr64_t)((unsigned)fsbtodb(fs, nb)));
		buf_clear(nbp);
		/*
		 * Write synchronously conditional on mount flags.
		 */
		if ((vp)->v_mount->mnt_flag & MNT_ASYNC) {
			error = 0;
			buf_bdwrite(nbp);
		} else if (error = buf_bwrite(nbp)) {
			buf_brelse(bp);
			goto fail;
		}
#if	REV_ENDIAN_FS
	if (rev_endian)
		bap[indirs[i - 1].in_off] = OSSwapInt32(nb);
	else {
#endif	/* REV_ENDIAN_FS */
		bap[indirs[i - 1].in_off] = nb;
#if	REV_ENDIAN_FS
	}
#endif	/* REV_ENDIAN_FS */
		/*
		 * If required, write synchronously, otherwise use
		 * delayed write.
		 */
		if (flags & B_SYNC) {
			buf_bwrite(bp);
		} else {
			buf_bdwrite(bp);
		}
	}
	/*
	 * Get the data block, allocating if necessary.
	 */
	if (nb == 0) {
		pref = ffs_blkpref(ip, lbn, indirs[i].in_off, &bap[0]);
		if (error = ffs_alloc(ip,
		    lbn, pref, (int)fs->fs_bsize, cred, &newb)) {
			buf_brelse(bp);
			goto fail;
		}
		nb = newb;
		*allocblk++ = nb;
#if	REV_ENDIAN_FS
	if (rev_endian)
		bap[indirs[i].in_off] = OSSwapInt32(nb);
	else {
#endif	/* REV_ENDIAN_FS */
		bap[indirs[i].in_off] = nb;
#if	REV_ENDIAN_FS
	}
#endif	/* REV_ENDIAN_FS */
		/*
		 * If required, write synchronously, otherwise use
		 * delayed write.
		 */
		if (flags & B_SYNC) {
			buf_bwrite(bp);
		} else {
			buf_bdwrite(bp);
		}
		return (0);
	}
	buf_brelse(bp);
	return (0);
fail:
	/*
	 * If we have failed part way through block allocation, we
	 * have to deallocate any indirect blocks that we have allocated.
	 */
	for (deallocated = 0, blkp = allociblk; blkp < allocblk; blkp++) {
		ffs_blkfree(ip, *blkp, fs->fs_bsize);
		deallocated += fs->fs_bsize;
	}
	if (allocib != NULL)
		*allocib = 0;
	if (deallocated) {
	        devBlockSize = vfs_devblocksize(mp);
#if QUOTA
		/*
		 * Restore user's disk quota because allocation failed.
		 */
		(void) chkdq(ip, (int64_t)-deallocated, cred, FORCE);
#endif /* QUOTA */
		ip->i_blocks -= btodb(deallocated, devBlockSize);
		ip->i_flag |= IN_CHANGE | IN_UPDATE;
	}
	return (error);
}

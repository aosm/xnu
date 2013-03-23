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
/* Copyright (c) 1995 NeXT Computer, Inc. All Rights Reserved */
/*-
 * Copyright (c) 1994 Christopher G. Demetriou
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
 * The NEXTSTEP Software License Agreement specifies the terms
 * and conditions for redistribution.
 *
 *	@(#)vfs_bio.c	8.6 (Berkeley) 1/11/94
 */


/*
 * Some references:
 *	Bach: The Design of the UNIX Operating System (Prentice Hall, 1986)
 *	Leffler, et al.: The Design and Implementation of the 4.3BSD
 *		UNIX Operating System (Addison Welley, 1989)
 */
#define ZALLOC_METADATA 1

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/proc.h>
#include <sys/buf.h>
#include <sys/vnode.h>
#include <sys/mount.h>
#include <sys/trace.h>
#include <sys/malloc.h>
#include <sys/resourcevar.h>
#include <miscfs/specfs/specdev.h>
#include <sys/ubc.h>
#include <vm/vm_pageout.h>
#if DIAGNOSTIC
#include <kern/assert.h>
#endif /* DIAGNOSTIC */
#include <kern/task.h>
#include <kern/zalloc.h>

#include <sys/kdebug.h>

extern void bufqinc(int q);
extern void bufqdec(int q);
extern void bufq_balance_thread_init();

extern void reassignbuf(struct buf *, struct vnode *);
static struct buf *getnewbuf(int slpflag, int slptimeo, int *queue);

extern int niobuf;	/* The number of IO buffer headers for cluster IO */
int blaundrycnt;

#if TRACE
struct	proc *traceproc;
int	tracewhich, tracebuf[TRCSIZ];
u_int	tracex;
char	traceflags[TR_NFLAGS];
#endif /* TRACE */

/*
 * Definitions for the buffer hash lists.
 */
#define	BUFHASH(dvp, lbn)	\
	(&bufhashtbl[((long)(dvp) / sizeof(*(dvp)) + (int)(lbn)) & bufhash])
LIST_HEAD(bufhashhdr, buf) *bufhashtbl, invalhash;
u_long	bufhash;

/* Definitions for the buffer stats. */
struct bufstats bufstats;

/*
 * Insq/Remq for the buffer hash lists.
 */
#if 0
#define	binshash(bp, dp)	LIST_INSERT_HEAD(dp, bp, b_hash)
#define	bremhash(bp)		LIST_REMOVE(bp, b_hash)
#endif /* 0 */


TAILQ_HEAD(ioqueue, buf) iobufqueue;
TAILQ_HEAD(bqueues, buf) bufqueues[BQUEUES];
int needbuffer;
int need_iobuffer;

/*
 * Insq/Remq for the buffer free lists.
 */
#define	binsheadfree(bp, dp, whichq)	do { \
				    TAILQ_INSERT_HEAD(dp, bp, b_freelist); \
					bufqinc((whichq));	\
					(bp)->b_whichq = whichq; \
				    (bp)->b_timestamp = time.tv_sec; \
				} while (0)

#define	binstailfree(bp, dp, whichq)	do { \
				    TAILQ_INSERT_TAIL(dp, bp, b_freelist); \
					bufqinc((whichq));	\
					(bp)->b_whichq = whichq; \
				    (bp)->b_timestamp = time.tv_sec; \
				} while (0)

#define BHASHENTCHECK(bp)	\
	if ((bp)->b_hash.le_prev != (struct buf **)0xdeadbeef)	\
		panic("%x: b_hash.le_prev is not deadbeef", (bp));

#define BLISTNONE(bp)	\
	(bp)->b_hash.le_next = (struct buf *)0;	\
	(bp)->b_hash.le_prev = (struct buf **)0xdeadbeef;

simple_lock_data_t bufhashlist_slock;		/* lock on buffer hash list */

/*
 * Time in seconds before a buffer on a list is 
 * considered as a stale buffer 
 */
#define LRU_IS_STALE 120 /* default value for the LRU */
#define AGE_IS_STALE 60  /* default value for the AGE */
#define META_IS_STALE 180 /* default value for the BQ_META */

int lru_is_stale = LRU_IS_STALE;
int age_is_stale = AGE_IS_STALE;
int meta_is_stale = META_IS_STALE;

#if 1
void
blistenterhead(struct bufhashhdr * head, struct buf * bp)
{
	if ((bp->b_hash.le_next = (head)->lh_first) != NULL)
		(head)->lh_first->b_hash.le_prev = &(bp)->b_hash.le_next;
	(head)->lh_first = bp;
	bp->b_hash.le_prev = &(head)->lh_first;
	if (bp->b_hash.le_prev == (struct buf **)0xdeadbeef) 
		panic("blistenterhead: le_prev is deadbeef");

}
#endif

#if 1
void 
binshash(struct buf *bp, struct bufhashhdr *dp)
{
int s;

struct buf *nbp;

	simple_lock(&bufhashlist_slock);
#if 0	
	if(incore(bp->b_vp, bp->b_lblkno)) {
		panic("adding to queue already existing element");
	}
#endif /* 0 */
	BHASHENTCHECK(bp);
	
	nbp = dp->lh_first;
	for(; nbp != NULL; nbp = nbp->b_hash.le_next) {
		if(nbp == bp) 
			panic("buf already in hashlist");
	}

#if 0
	LIST_INSERT_HEAD(dp, bp, b_hash);
#else
	blistenterhead(dp, bp);
#endif
	simple_unlock(&bufhashlist_slock);
}

void 
bremhash(struct buf *bp) 
{
	int s;

	simple_lock(&bufhashlist_slock);
	if (bp->b_hash.le_prev == (struct buf **)0xdeadbeef) 
		panic("bremhash le_prev is deadbeef");
	if (bp->b_hash.le_next == bp) 
		panic("bremhash: next points to self");

	if (bp->b_hash.le_next != NULL)
		bp->b_hash.le_next->b_hash.le_prev = bp->b_hash.le_prev;
	*bp->b_hash.le_prev = (bp)->b_hash.le_next;
	simple_unlock(&bufhashlist_slock);
}

#endif /* 1 */


/*
 * Remove a buffer from the free list it's on
 */
void
bremfree(bp)
	struct buf *bp;
{
	struct bqueues *dp = NULL;
	int whichq = -1;

	/*
	 * We only calculate the head of the freelist when removing
	 * the last element of the list as that is the only time that
	 * it is needed (e.g. to reset the tail pointer).
	 *
	 * NB: This makes an assumption about how tailq's are implemented.
	 */
	if (bp->b_freelist.tqe_next == NULL) {
		for (dp = bufqueues; dp < &bufqueues[BQUEUES]; dp++)
			if (dp->tqh_last == &bp->b_freelist.tqe_next)
				break;
		if (dp == &bufqueues[BQUEUES])
			panic("bremfree: lost tail");
	}
	TAILQ_REMOVE(dp, bp, b_freelist);
	whichq = bp->b_whichq;
	bufqdec(whichq);
	bp->b_whichq = -1;
	bp->b_timestamp = 0; 
}

static __inline__ void
bufhdrinit(struct buf *bp)
{
	bzero((char *)bp, sizeof *bp);
	bp->b_dev = NODEV;
	bp->b_rcred = NOCRED;
	bp->b_wcred = NOCRED;
	bp->b_vnbufs.le_next = NOLIST;
	bp->b_flags = B_INVAL;

	return;
}

/*
 * Initialize buffers and hash links for buffers.
 */
void
bufinit()
{
	register struct buf *bp;
	register struct bqueues *dp;
	register int i;
	int metabuf;
	long whichq;
	static void bufzoneinit();
	static void bcleanbuf_thread_init();

	/* Initialize the buffer queues ('freelists') and the hash table */
	for (dp = bufqueues; dp < &bufqueues[BQUEUES]; dp++)
		TAILQ_INIT(dp);
	bufhashtbl = hashinit(nbuf, M_CACHE, &bufhash);

	simple_lock_init(&bufhashlist_slock );

	metabuf = nbuf/8; /* reserved for meta buf */

	/* Initialize the buffer headers */
	for (i = 0; i < nbuf; i++) {
		bp = &buf[i];
		bufhdrinit(bp);

		/*
		 * metabuf buffer headers on the meta-data list and
		 * rest of the buffer headers on the empty list
		 */
		if (--metabuf) 
			whichq = BQ_META;
		else 
			whichq = BQ_EMPTY;

		BLISTNONE(bp);
		dp = &bufqueues[whichq];
		binsheadfree(bp, dp, whichq);
		binshash(bp, &invalhash);
	}

	for (; i < nbuf + niobuf; i++) {
		bp = &buf[i];
		bufhdrinit(bp);
		binsheadfree(bp, &iobufqueue, -1);
	}

	printf("using %d buffer headers and %d cluster IO buffer headers\n",
		nbuf, niobuf);

	/* Set up zones used by the buffer cache */
	bufzoneinit();

	/* start the bcleanbuf() thread */
	bcleanbuf_thread_init();

#if 0	/* notyet */
	/* create a thread to do dynamic buffer queue balancing */
	bufq_balance_thread_init();
#endif /* XXX */
}

/* __inline  */
struct buf *
bio_doread(vp, blkno, size, cred, async, queuetype)
	struct vnode *vp;
	daddr_t blkno;
	int size;
	struct ucred *cred;
	int async;
	int queuetype;
{
	register struct buf *bp;
	struct proc	*p = current_proc();

	bp = getblk(vp, blkno, size, 0, 0, queuetype);

	/*
	 * If buffer does not have data valid, start a read.
	 * Note that if buffer is B_INVAL, getblk() won't return it.
	 * Therefore, it's valid if it's I/O has completed or been delayed.
	 */
	if (!ISSET(bp->b_flags, (B_DONE | B_DELWRI))) {
		/* Start I/O for the buffer (keeping credentials). */
		SET(bp->b_flags, B_READ | async);
		if (cred != NOCRED && bp->b_rcred == NOCRED) {
			/*
			 * NFS has embedded ucred.
			 * Can not crhold() here as that causes zone corruption
			 */
			bp->b_rcred = crdup(cred);
		}
		VOP_STRATEGY(bp);

		trace(TR_BREADMISS, pack(vp, size), blkno);

		/* Pay for the read. */
		if (p && p->p_stats) 
			p->p_stats->p_ru.ru_inblock++;		/* XXX */
	} else if (async) {
		brelse(bp);
	}

	trace(TR_BREADHIT, pack(vp, size), blkno);

	return (bp);
}
/*
 * Read a disk block.
 * This algorithm described in Bach (p.54).
 */
int
bread(vp, blkno, size, cred, bpp)
	struct vnode *vp;
	daddr_t blkno;
	int size;
	struct ucred *cred;
	struct buf **bpp;
{
	register struct buf *bp;

	/* Get buffer for block. */
	bp = *bpp = bio_doread(vp, blkno, size, cred, 0, BLK_READ);

	/* Wait for the read to complete, and return result. */
	return (biowait(bp));
}

/*
 * Read a disk block. [bread() for meta-data]
 * This algorithm described in Bach (p.54).
 */
int
meta_bread(vp, blkno, size, cred, bpp)
	struct vnode *vp;
	daddr_t blkno;
	int size;
	struct ucred *cred;
	struct buf **bpp;
{
	register struct buf *bp;

	/* Get buffer for block. */
	bp = *bpp = bio_doread(vp, blkno, size, cred, 0, BLK_META);

	/* Wait for the read to complete, and return result. */
	return (biowait(bp));
}

/*
 * Read-ahead multiple disk blocks. The first is sync, the rest async.
 * Trivial modification to the breada algorithm presented in Bach (p.55).
 */
int
breadn(vp, blkno, size, rablks, rasizes, nrablks, cred, bpp)
	struct vnode *vp;
	daddr_t blkno; int size;
	daddr_t rablks[]; int rasizes[];
	int nrablks;
	struct ucred *cred;
	struct buf **bpp;
{
	register struct buf *bp;
	int i;

	bp = *bpp = bio_doread(vp, blkno, size, cred, 0, BLK_READ);

	/*
	 * For each of the read-ahead blocks, start a read, if necessary.
	 */
	for (i = 0; i < nrablks; i++) {
		/* If it's in the cache, just go on to next one. */
		if (incore(vp, rablks[i]))
			continue;

		/* Get a buffer for the read-ahead block */
		(void) bio_doread(vp, rablks[i], rasizes[i], cred, B_ASYNC, BLK_READ);
	}

	/* Otherwise, we had to start a read for it; wait until it's valid. */
	return (biowait(bp));
}

/*
 * Read with single-block read-ahead.  Defined in Bach (p.55), but
 * implemented as a call to breadn().
 * XXX for compatibility with old file systems.
 */
int
breada(vp, blkno, size, rablkno, rabsize, cred, bpp)
	struct vnode *vp;
	daddr_t blkno; int size;
	daddr_t rablkno; int rabsize;
	struct ucred *cred;
	struct buf **bpp;
{

	return (breadn(vp, blkno, size, &rablkno, &rabsize, 1, cred, bpp));	
}

/*
 * Block write.  Described in Bach (p.56)
 */
int
bwrite(bp)
	struct buf *bp;
{
	int rv, sync, wasdelayed;
	struct proc	*p = current_proc();
	upl_t  upl;
	upl_page_info_t *pl;
	void * object;
	kern_return_t kret;
	struct vnode *vp = bp->b_vp;

	/* Remember buffer type, to switch on it later. */
	sync = !ISSET(bp->b_flags, B_ASYNC);
	wasdelayed = ISSET(bp->b_flags, B_DELWRI);
	CLR(bp->b_flags, (B_READ | B_DONE | B_ERROR | B_DELWRI));

	if (!sync) {
		/*
		 * If not synchronous, pay for the I/O operation and make
		 * sure the buf is on the correct vnode queue.  We have
		 * to do this now, because if we don't, the vnode may not
		 * be properly notified that its I/O has completed.
		 */
		if (wasdelayed)
			reassignbuf(bp, vp);
		else
		if (p && p->p_stats) 
			p->p_stats->p_ru.ru_oublock++;		/* XXX */
	}

	trace(TR_BWRITE, pack(vp, bp->b_bcount), bp->b_lblkno);

	/* Initiate disk write.  Make sure the appropriate party is charged. */
	SET(bp->b_flags, B_WRITEINPROG);
	vp->v_numoutput++;
	
	VOP_STRATEGY(bp);

	if (sync) {
		/*
		 * If I/O was synchronous, wait for it to complete.
		 */
		rv = biowait(bp);

		/*
		 * Pay for the I/O operation, if it's not been paid for, and
		 * make sure it's on the correct vnode queue. (async operatings
		 * were payed for above.)
		 */
		if (wasdelayed)
			reassignbuf(bp, vp);
		else
		if (p && p->p_stats) 
			p->p_stats->p_ru.ru_oublock++;		/* XXX */

		/* Release the buffer. */
		brelse(bp);

		return (rv);
	} else {
		return (0);
	}
}

int
vn_bwrite(ap)
	struct vop_bwrite_args *ap;
{
	return (bwrite(ap->a_bp));
}

/*
 * Delayed write.
 *
 * The buffer is marked dirty, but is not queued for I/O.
 * This routine should be used when the buffer is expected
 * to be modified again soon, typically a small write that
 * partially fills a buffer.
 *
 * NB: magnetic tapes cannot be delayed; they must be
 * written in the order that the writes are requested.
 *
 * Described in Leffler, et al. (pp. 208-213).
 */
void
bdwrite(bp)
	struct buf *bp;
{
	struct proc *p = current_proc();
	kern_return_t kret;
	upl_t upl;
	upl_page_info_t *pl;

	/*
	 * If the block hasn't been seen before:
	 *	(1) Mark it as having been seen,
	 *	(2) Charge for the write.
	 *	(3) Make sure it's on its vnode's correct block list,
	 */
	if (!ISSET(bp->b_flags, B_DELWRI)) {
		SET(bp->b_flags, B_DELWRI);
		if (p && p->p_stats) 
			p->p_stats->p_ru.ru_oublock++;		/* XXX */

		reassignbuf(bp, bp->b_vp);
	}


	/* If this is a tape block, write it the block now. */
	if (ISSET(bp->b_flags, B_TAPE)) {
		/* bwrite(bp); */
        VOP_BWRITE(bp);
		return;
	}

	/* Otherwise, the "write" is done, so mark and release the buffer. */
	SET(bp->b_flags, B_DONE);
	brelse(bp);
}

/*
 * Asynchronous block write; just an asynchronous bwrite().
 */
void
bawrite(bp)
	struct buf *bp;
{

	SET(bp->b_flags, B_ASYNC);
	VOP_BWRITE(bp);
}

/*
 * Release a buffer on to the free lists.
 * Described in Bach (p. 46).
 */
void
brelse(bp)
	struct buf *bp;
{
	struct bqueues *bufq;
	int s;
	long whichq;

	KERNEL_DEBUG((FSDBG_CODE(DBG_FSRW, 388)) | DBG_FUNC_START,
		     bp->b_lblkno * PAGE_SIZE, (int)bp, (int)bp->b_data,
		     bp->b_flags, 0);

	trace(TR_BRELSE, pack(bp->b_vp, bp->b_bufsize), bp->b_lblkno);

	/* IO is done. Cleanup the UPL state */
	if (!ISSET(bp->b_flags, B_META)
		&& UBCINFOEXISTS(bp->b_vp) && bp->b_bufsize) {
		kern_return_t kret;
		upl_t	      upl;
		int           upl_flags;

		if ( !ISSET(bp->b_flags, B_PAGELIST)) {
		        if ( !ISSET(bp->b_flags, B_INVAL)) {
				kret = ubc_create_upl(bp->b_vp, 
								ubc_blktooff(bp->b_vp, bp->b_lblkno),
								bp->b_bufsize, 
							    &upl,
								NULL,
								UPL_PRECIOUS);
				if (kret != KERN_SUCCESS)
				        panic("brelse: Failed to get pagelists");
#ifdef  UBC_DEBUG
				upl_ubc_alias_set(upl, bp, 5);
#endif /* UBC_DEBUG */
			} else
				upl = (upl_t) 0;
		} else {
			upl = bp->b_pagelist;
			kret = ubc_upl_unmap(upl);

			if (kret != KERN_SUCCESS)
			        panic("kernel_upl_unmap failed");
			bp->b_data = 0;
		}
		if (upl) {
			if (bp->b_flags & (B_ERROR | B_INVAL)) {
			    if (bp->b_flags & (B_READ | B_INVAL))
				        upl_flags = UPL_ABORT_DUMP_PAGES;
				else
				        upl_flags = 0;
				ubc_upl_abort(upl, upl_flags);
			} else {
			    if (ISSET(bp->b_flags, B_NEEDCOMMIT))
				    upl_flags = UPL_COMMIT_CLEAR_DIRTY ;
			    else if (ISSET(bp->b_flags, B_DELWRI | B_WASDIRTY))
					upl_flags = UPL_COMMIT_SET_DIRTY ;
				else
				    upl_flags = UPL_COMMIT_CLEAR_DIRTY ;
				ubc_upl_commit_range(upl, 0, bp->b_bufsize, upl_flags |
					UPL_COMMIT_INACTIVATE | UPL_COMMIT_FREE_ON_EMPTY);
			}
			s = splbio();
			CLR(bp->b_flags, B_PAGELIST);
			bp->b_pagelist = 0;
			splx(s);
		}
	} else {
		if(ISSET(bp->b_flags, B_PAGELIST))
			panic("brelse: pagelist set for non VREG; vp=%x", bp->b_vp);
	}	

	/* Wake up any processes waiting for any buffer to become free. */
	if (needbuffer) {
		needbuffer = 0;
		wakeup(&needbuffer);
	}

	/* Wake up any proceeses waiting for _this_ buffer to become free. */
	if (ISSET(bp->b_flags, B_WANTED)) {
		CLR(bp->b_flags, B_WANTED);
		wakeup(bp);
	}

	/* Block disk interrupts. */
	s = splbio();

	/*
	 * Determine which queue the buffer should be on, then put it there.
	 */

	/* If it's locked, don't report an error; try again later. */
	if (ISSET(bp->b_flags, (B_LOCKED|B_ERROR)) == (B_LOCKED|B_ERROR))
		CLR(bp->b_flags, B_ERROR);

	/* If it's not cacheable, or an error, mark it invalid. */
	if (ISSET(bp->b_flags, (B_NOCACHE|B_ERROR)))
		SET(bp->b_flags, B_INVAL);

	if ((bp->b_bufsize <= 0) || ISSET(bp->b_flags, B_INVAL)) {
		/*
		 * If it's invalid or empty, dissociate it from its vnode
		 * and put on the head of the appropriate queue.
		 */
		if (bp->b_vp)
			brelvp(bp);
		CLR(bp->b_flags, B_DELWRI);
		if (bp->b_bufsize <= 0)
			whichq = BQ_EMPTY;	/* no data */
		else
			whichq = BQ_AGE;	/* invalid data */

		bufq = &bufqueues[whichq];
		binsheadfree(bp, bufq, whichq);
	} else {
		/*
		 * It has valid data.  Put it on the end of the appropriate
		 * queue, so that it'll stick around for as long as possible.
		 */
		if (ISSET(bp->b_flags, B_LOCKED))
			whichq = BQ_LOCKED;		/* locked in core */
		else if (ISSET(bp->b_flags, B_META))
			whichq = BQ_META;		/* meta-data */
		else if (ISSET(bp->b_flags, B_AGE))
			whichq = BQ_AGE;		/* stale but valid data */
		else
			whichq = BQ_LRU;		/* valid data */

		bufq = &bufqueues[whichq];
		binstailfree(bp, bufq, whichq);
	}

	/* Unlock the buffer. */
	CLR(bp->b_flags, (B_AGE | B_ASYNC | B_BUSY | B_NOCACHE));

	/* Allow disk interrupts. */
	splx(s);

	KERNEL_DEBUG((FSDBG_CODE(DBG_FSRW, 388)) | DBG_FUNC_END,
		     (int)bp, (int)bp->b_data, bp->b_flags, 0, 0);
}

/*
 * Determine if a block is in the cache.
 * Just look on what would be its hash chain.  If it's there, return
 * a pointer to it, unless it's marked invalid.  If it's marked invalid,
 * we normally don't return the buffer, unless the caller explicitly
 * wants us to.
 */
struct buf *
incore(vp, blkno)
	struct vnode *vp;
	daddr_t blkno;
{
	struct buf *bp;
	int bufseen = 0;

	bp = BUFHASH(vp, blkno)->lh_first;

	/* Search hash chain */
	for (; bp != NULL; bp = bp->b_hash.le_next, bufseen++) {
		if (bp->b_lblkno == blkno && bp->b_vp == vp &&
		    !ISSET(bp->b_flags, B_INVAL))
			return (bp);
	if(bufseen >= nbuf) 
		panic("walked more than nbuf in incore");
		
	}

	return (0);
}


/* XXX FIXME -- Update the comment to reflect the UBC changes (please) -- */
/*
 * Get a block of requested size that is associated with
 * a given vnode and block offset. If it is found in the
 * block cache, mark it as having been found, make it busy
 * and return it. Otherwise, return an empty block of the
 * correct size. It is up to the caller to insure that the
 * cached blocks be of the correct size.
 */
struct buf *
getblk(vp, blkno, size, slpflag, slptimeo, operation)
	register struct vnode *vp;
	daddr_t blkno;
	int size, slpflag, slptimeo, operation;
{
	struct buf *bp;
	int s, err;
	upl_t upl;
	upl_page_info_t *pl;
	kern_return_t kret;
	int error=0;
	int pagedirty = 0;

	KERNEL_DEBUG((FSDBG_CODE(DBG_FSRW, 386)) | DBG_FUNC_START,
		     blkno * PAGE_SIZE, size, operation, 0, 0);
start:

	s = splbio();
	if (bp = incore(vp, blkno)) {
		/* Found in the Buffer Cache */
		if (ISSET(bp->b_flags, B_BUSY)) {
			/* but is busy */
			switch (operation) {
			case BLK_READ:
			case BLK_WRITE:
			case BLK_META:
				SET(bp->b_flags, B_WANTED);
				bufstats.bufs_busyincore++;
				err = tsleep(bp, slpflag | (PRIBIO + 1), "getblk",
				    slptimeo);
				splx(s);
				/*
				 * Callers who call with PCATCH or timeout are
				 * willing to deal with the NULL pointer
				 */
				if (err && ((slpflag & PCATCH) || 
							 ((err == EWOULDBLOCK) && slptimeo)))
					return (NULL);
				goto start;
				/*NOTREACHED*/
				break;

			case BLK_PAGEIN:
				/* pagein operation must not use getblk */
				panic("getblk: pagein for incore busy buffer");
				splx(s);
				/*NOTREACHED*/
				break;

			case BLK_PAGEOUT:
				/* pageout operation must not use getblk */
				panic("getblk: pageout for incore busy buffer");
				splx(s);
				/*NOTREACHED*/
				break;

			default:
				panic("getblk: %d unknown operation 1", operation);
				/*NOTREACHED*/
				break;
			}		
		} else {
			/* not busy */
			SET(bp->b_flags, (B_BUSY | B_CACHE));
			bremfree(bp);
			bufstats.bufs_incore++;
			splx(s);

			allocbuf(bp, size);
			if (ISSET(bp->b_flags, B_PAGELIST))
					panic("pagelist buffer is not busy");

			switch (operation) {
			case BLK_READ:
			case BLK_WRITE:
			        if (UBCISVALID(bp->b_vp) && bp->b_bufsize) {
					kret = ubc_create_upl(vp,
									ubc_blktooff(vp, bp->b_lblkno), 
									bp->b_bufsize, 
									&upl, 
									&pl,
									UPL_PRECIOUS);
					if (kret != KERN_SUCCESS)
					        panic("Failed to get pagelists");

					SET(bp->b_flags, B_PAGELIST);
					bp->b_pagelist = upl;

					if (!upl_valid_page(pl, 0)) {
						if (vp->v_tag != VT_NFS)
					        	panic("getblk: incore buffer without valid page");
						CLR(bp->b_flags, B_CACHE);
					}

					if (upl_dirty_page(pl, 0))
					        SET(bp->b_flags, B_WASDIRTY);
					else
					        CLR(bp->b_flags, B_WASDIRTY);

					kret = ubc_upl_map(upl, (vm_address_t *)&(bp->b_data));
					if (kret != KERN_SUCCESS) {
					        panic("getblk: ubc_upl_map() failed with (%d)",
								  kret);
					}
					if (bp->b_data == 0) panic("ubc_upl_map mapped 0");
				}
				break;

			case BLK_META:
				/*
				 * VM is not involved in IO for the meta data
				 * buffer already has valid data 
				 */
			if(bp->b_data == 0)
					panic("bp->b_data null incore buf=%x", bp);
				break;

			case BLK_PAGEIN:
			case BLK_PAGEOUT:
				panic("getblk: paging operation 1");
				break;

			default:
				panic("getblk: %d unknown operation 2", operation);
				/*NOTREACHED*/
				break;
			}
		}
	} else { /* not incore() */
		int queue = BQ_EMPTY; /* Start with no preference */
		splx(s);
		
		if ((operation == BLK_META) || (UBCINVALID(vp)) ||
			!(UBCINFOEXISTS(vp))) {
			operation = BLK_META;
		}
		if ((bp = getnewbuf(slpflag, slptimeo, &queue)) == NULL)
			goto start;
		if (incore(vp, blkno)) {
			SET(bp->b_flags, B_INVAL);
			binshash(bp, &invalhash);
			brelse(bp);
			goto start;
		}

		/*
		 * if it is meta, the queue may be set to other 
		 * type so reset as well as mark it to be B_META
		 * so that when buffer is released it will goto META queue
		 * Also, if the vnode is not VREG, then it is META
		 */
		if (operation == BLK_META) {
			SET(bp->b_flags, B_META);
			queue = BQ_META;
		}
		/*
		 * Insert in the hash so that incore() can find it 
		 */
		binshash(bp, BUFHASH(vp, blkno)); 

		allocbuf(bp, size);

		switch (operation) {
		case BLK_META:
			/* buffer data is invalid */

#if !ZALLOC_METADATA
			if (bp->b_data)
				panic("bp->b_data is not nul; %x",bp);
			kret = kmem_alloc(kernel_map,
						&bp->b_data, bp->b_bufsize);
			if (kret != KERN_SUCCESS)
				panic("getblk: kmem_alloc() returned %d", kret);
#endif /* ZALLOC_METADATA */

			if(bp->b_data == 0)
				panic("bp->b_data is null %x",bp);

			bp->b_blkno = bp->b_lblkno = blkno;
			s = splbio();
			bgetvp(vp, bp);
			bufstats.bufs_miss++;
			splx(s);
			if (bp->b_data == 0)
				panic("b_data is 0: 2");

			/* wakeup the buffer */	
			CLR(bp->b_flags, B_WANTED);
			wakeup(bp);
			break;

		case BLK_READ:
		case BLK_WRITE:

			if (ISSET(bp->b_flags, B_PAGELIST))
				panic("B_PAGELIST in bp=%x",bp);

			kret = ubc_create_upl(vp,
							ubc_blktooff(vp, blkno),
							bp->b_bufsize, 
							&upl,
							&pl,
							UPL_PRECIOUS);
			if (kret != KERN_SUCCESS)
				panic("Failed to get pagelists");

#ifdef  UBC_DEBUG
			upl_ubc_alias_set(upl, bp, 4);
#endif /* UBC_DEBUG */
			bp->b_blkno = bp->b_lblkno = blkno;
			bp->b_pagelist = upl;

			SET(bp->b_flags, B_PAGELIST);

			if (upl_valid_page(pl, 0)) {
				SET(bp->b_flags, B_CACHE | B_DONE);
				bufstats.bufs_vmhits++;

				pagedirty = upl_dirty_page(pl, 0);

				if (pagedirty)
				        SET(bp->b_flags, B_WASDIRTY);

				if (vp->v_tag == VT_NFS) {
				        off_t  f_offset;
					int    valid_size;

					bp->b_validoff = 0;
					bp->b_dirtyoff = 0;

					f_offset = ubc_blktooff(vp, blkno);

					if (f_offset > vp->v_ubcinfo->ui_size) {
					        CLR(bp->b_flags, (B_CACHE|B_DONE|B_WASDIRTY));
						bp->b_validend = 0;
						bp->b_dirtyend = 0;
					} else {
					        valid_size = min(((unsigned int)(vp->v_ubcinfo->ui_size - f_offset)), PAGE_SIZE);
						bp->b_validend = valid_size;

						if (pagedirty)
						       bp->b_dirtyend = valid_size;
						else
						       bp->b_dirtyend = 0;

						KERNEL_DEBUG((FSDBG_CODE(DBG_FSRW, 386)) | DBG_FUNC_NONE,
							     bp->b_validend, bp->b_dirtyend, 
							     (int)vp->v_ubcinfo->ui_size, 0, 0);
					}
				} else {
					bp->b_validoff = 0;
					bp->b_dirtyoff = 0;

					if (pagedirty) {
						/* page is dirty */
						bp->b_validend = bp->b_bcount;
						bp->b_dirtyend = bp->b_bcount;
					} else {
						/* page is clean */
						bp->b_validend = bp->b_bcount;
						bp->b_dirtyend = 0;
					}
				}
				if (error = VOP_BMAP(vp, bp->b_lblkno, NULL, &bp->b_blkno, NULL)) {
					panic("VOP_BMAP failed in getblk");
					/*NOTREACHED*/
					/*
					 * XXX:  We probably should invalidate the VM Page
					 */
					bp->b_error = error;
					SET(bp->b_flags, (B_ERROR | B_INVAL));
					/* undo B_DONE that was set before upl_commit() */
					CLR(bp->b_flags, B_DONE);
					brelse(bp);
					return (0);
				}
			} else {
				bufstats.bufs_miss++;
			}
			kret = ubc_upl_map(upl, (vm_address_t *)&(bp->b_data));
			if (kret != KERN_SUCCESS) {
			        panic("getblk: ubc_upl_map() "
				      "failed with (%d)", kret);
			}
			if (bp->b_data == 0) panic("kernel_upl_map mapped 0");

			s = splbio();
			bgetvp(vp, bp);
			splx(s);

			break;

		case BLK_PAGEIN:
		case BLK_PAGEOUT:
			panic("getblk: paging operation 2");
			break;
		default:
			panic("getblk: %d unknown operation 3", operation);
			/*NOTREACHED*/
			break;
		}
	}

	if (bp->b_data == NULL)
		panic("getblk: bp->b_addr is null");

	if (bp->b_bufsize & 0xfff) {
#if ZALLOC_METADATA
		if (ISSET(bp->b_flags, B_META) && (bp->b_bufsize & 0x1ff))
#endif /* ZALLOC_METADATA */
			panic("getblk: bp->b_bufsize = %d", bp->b_bufsize);
	}

	KERNEL_DEBUG((FSDBG_CODE(DBG_FSRW, 386)) | DBG_FUNC_END,
		     (int)bp, (int)bp->b_data, bp->b_flags, 3, 0);

	return (bp);
}

/*
 * Get an empty, disassociated buffer of given size.
 */
struct buf *
geteblk(size)
	int size;
{
	struct buf *bp;
    int queue = BQ_EMPTY;
#if !ZALLOC_METADATA
	kern_return_t kret;
	vm_size_t desired_size = roundup(size, CLBYTES);

	if (desired_size > MAXBSIZE)
		panic("geteblk: buffer larger than MAXBSIZE requested");
#endif /* ZALLOC_METADATA */

	while ((bp = getnewbuf(0, 0, &queue)) == 0)
		;
#if ZALLOC_METADATA
	SET(bp->b_flags, (B_META|B_INVAL));
#else
	SET(bp->b_flags, B_INVAL);
#endif /* ZALLOC_METADATA */

#if DIAGNOSTIC
	assert(queue == BQ_EMPTY);
#endif /* DIAGNOSTIC */
	/* XXX need to implement logic to deal with other queues */

#if !ZALLOC_METADATA
	/* Empty buffer - allocate pages */
	kret = kmem_alloc_aligned(kernel_map, &bp->b_data, desired_size);
	if (kret != KERN_SUCCESS)
		panic("geteblk: kmem_alloc_aligned returned %d", kret);
#endif /* ZALLOC_METADATA */

	binshash(bp, &invalhash);
	allocbuf(bp, size);
	bufstats.bufs_eblk++;

	return (bp);
}

#if ZALLOC_METADATA
/*
 * Zones for the meta data buffers
 */

#define MINMETA 512
#define MAXMETA 4096

struct meta_zone_entry {
	zone_t mz_zone;
	vm_size_t mz_size;
	vm_size_t mz_max;
	char *mz_name;
};

struct meta_zone_entry meta_zones[] = {
	{NULL, (MINMETA * 1), 128 * (MINMETA * 1), "buf.512" },
	{NULL, (MINMETA * 2),  64 * (MINMETA * 2), "buf.1024" },
	{NULL, (MINMETA * 3),  16 * (MINMETA * 3), "buf.1536" },
	{NULL, (MINMETA * 4),  16 * (MINMETA * 4), "buf.2048" },
	{NULL, (MINMETA * 5),  16 * (MINMETA * 5), "buf.2560" },
	{NULL, (MINMETA * 6),  16 * (MINMETA * 6), "buf.3072" },
	{NULL, (MINMETA * 7),  16 * (MINMETA * 7), "buf.3584" },
	{NULL, (MINMETA * 8), 512 * (MINMETA * 8), "buf.4096" },
	{NULL, 0, 0, "" } /* End */
};
#endif /* ZALLOC_METADATA */

zone_t buf_hdr_zone;
int buf_hdr_count;

/*
 * Initialize the meta data zones
 */
static void
bufzoneinit(void)
{
#if ZALLOC_METADATA
	int i;

	for (i = 0; meta_zones[i].mz_size != 0; i++) {
		meta_zones[i].mz_zone = 
				zinit(meta_zones[i].mz_size,
					meta_zones[i].mz_max,
					PAGE_SIZE,
					meta_zones[i].mz_name);
	}
#endif /* ZALLOC_METADATA */
	buf_hdr_zone = zinit(sizeof(struct buf), 32, PAGE_SIZE, "buf headers");
}

#if ZALLOC_METADATA
static zone_t
getbufzone(size_t size)
{
	int i;

	if (size % 512)
		panic("getbufzone: incorect size = %d", size);

	i = (size / 512) - 1;
	return (meta_zones[i].mz_zone);
}
#endif /* ZALLOC_METADATA */

/*
 * With UBC, there is no need to expand / shrink the file data 
 * buffer. The VM uses the same pages, hence no waste.
 * All the file data buffers can have one size.
 * In fact expand / shrink would be an expensive operation.
 *
 * Only exception to this is meta-data buffers. Most of the
 * meta data operations are smaller than PAGE_SIZE. Having the
 * meta-data buffers grow and shrink as needed, optimizes use
 * of the kernel wired memory.
 */

int
allocbuf(bp, size)
	struct buf *bp;
	int size;
{
	vm_size_t desired_size;

	desired_size = roundup(size, CLBYTES);

	if(desired_size < PAGE_SIZE)
		desired_size = PAGE_SIZE;
	if (desired_size > MAXBSIZE)
		panic("allocbuf: buffer larger than MAXBSIZE requested");

#if ZALLOC_METADATA
	if (ISSET(bp->b_flags, B_META)) {
		kern_return_t kret;
		zone_t zprev, z;
		size_t nsize = roundup(size, MINMETA);

		if (bp->b_data) {
			vm_offset_t elem = (vm_offset_t)bp->b_data;

			if (ISSET(bp->b_flags, B_ZALLOC))
				if (bp->b_bufsize <= MAXMETA) {
					if (bp->b_bufsize < nsize) {
						/* reallocate to a bigger size */
						desired_size = nsize;

						zprev = getbufzone(bp->b_bufsize);
						z = getbufzone(nsize);
						bp->b_data = (caddr_t)zalloc(z);
						if(bp->b_data == 0)
							panic("allocbuf: zalloc() returned NULL");
						bcopy(elem, bp->b_data, bp->b_bufsize);
						zfree(zprev, elem);
					} else {
						desired_size = bp->b_bufsize;
					}
				} else
					panic("allocbuf: B_ZALLOC set incorrectly");
			else
				if (bp->b_bufsize < desired_size) {
					/* reallocate to a bigger size */
					kret = kmem_alloc(kernel_map, &bp->b_data, desired_size);
					if (kret != KERN_SUCCESS)
						panic("allocbuf: kmem_alloc() returned %d", kret);
					if(bp->b_data == 0)
						panic("allocbuf: null b_data");
					bcopy(elem, bp->b_data, bp->b_bufsize);
					kmem_free(kernel_map, elem, bp->b_bufsize); 
				} else {
					desired_size = bp->b_bufsize;
				}
		} else {
			/* new allocation */
			if (nsize <= MAXMETA) {
				desired_size = nsize;
				z = getbufzone(nsize);
				bp->b_data = (caddr_t)zalloc(z);
				if(bp->b_data == 0)
					panic("allocbuf: zalloc() returned NULL 2");
				SET(bp->b_flags, B_ZALLOC);
			} else {
				kret = kmem_alloc(kernel_map, &bp->b_data, desired_size);
				if (kret != KERN_SUCCESS)
					panic("allocbuf: kmem_alloc() 2 returned %d", kret);
				if(bp->b_data == 0)
					panic("allocbuf: null b_data 2");
			}
		}
	}

	if (ISSET(bp->b_flags, B_META) && (bp->b_data == 0))
		panic("allocbuf: bp->b_data is NULL");
#endif /* ZALLOC_METADATA */

		bp->b_bufsize = desired_size;
		bp->b_bcount = size;
}

/*
 *	Get a new buffer from one of the free lists.
 *
 *	Request for a queue is passes in. The queue from which the buffer was taken
 *	from is returned. Out of range queue requests get BQ_EMPTY. Request for
 *	BQUEUE means no preference. Use heuristics in that case.
 *	Heuristics is as follows:
 *	Try BQ_AGE, BQ_LRU, BQ_EMPTY, BQ_META in that order.
 *	If none available block till one is made available.
 *	If buffers available on both BQ_AGE and BQ_LRU, check the timestamps.
 *	Pick the most stale buffer.
 *	If found buffer was marked delayed write, start the async. write
 *	and restart the search.
 *	Initialize the fields and disassociate the buffer from the vnode.
 *	Remove the buffer from the hash. Return the buffer and the queue
 *	on which it was found.
 */

static struct buf *
getnewbuf(slpflag, slptimeo, queue)
	int slpflag, slptimeo;
	int *queue;
{
	register struct buf *bp;
	register struct buf *lru_bp;
	register struct buf *age_bp;
	register struct buf *meta_bp;
	register int age_time, lru_time, bp_time, meta_time;
	int s;
	struct ucred *cred;
	int req = *queue; /* save it for restarts */

start:
	s = splbio();
	
	/* invalid request gets empty queue */
	if ((*queue > BQUEUES) || (*queue < 0)
		|| (*queue == BQ_LAUNDRY) || (*queue == BQ_LOCKED))
		*queue = BQ_EMPTY;

	/* (*queue == BQUEUES) means no preference */
	if (*queue != BQUEUES) {
		/* Try for the requested queue first */
		bp = bufqueues[*queue].tqh_first;
		if (bp)
			goto found;
	}

	/* Unable to use requested queue */
	age_bp = bufqueues[BQ_AGE].tqh_first;
	lru_bp = bufqueues[BQ_LRU].tqh_first;
	meta_bp = bufqueues[BQ_META].tqh_first;

	if (!age_bp && !lru_bp && !meta_bp) { /* Unavailble on AGE or LRU */
		/* Try the empty list first */
		bp = bufqueues[BQ_EMPTY].tqh_first;
		if (bp) {
			*queue = BQ_EMPTY;
			goto found;
		}

		/* Create a new temparory buffer header */
		bp = (struct buf *)zalloc(buf_hdr_zone);
	
		if (bp) {
			bufhdrinit(bp);
			BLISTNONE(bp);
			binshash(bp, &invalhash);
			SET(bp->b_flags, B_HDRALLOC);
			*queue = BQ_EMPTY;
			binsheadfree(bp, &bufqueues[BQ_EMPTY], BQ_EMPTY);
			buf_hdr_count++;
			goto found;
		}

		/* Log this error condition */
		printf("getnewbuf: No useful buffers");

		/* wait for a free buffer of any kind */
		needbuffer = 1;
		bufstats.bufs_sleeps++;
		tsleep(&needbuffer, slpflag|(PRIBIO+1), "getnewbuf", slptimeo);
		splx(s);
		return (0);
	}

	/* Buffer available either on AGE or LRU or META */
	bp = NULL;
	*queue = -1;

	/* Buffer available either on AGE or LRU */
	if (!age_bp) {
		bp = lru_bp;
		*queue = BQ_LRU;
	} else if (!lru_bp) {
		bp = age_bp;
		*queue = BQ_AGE;
	} else { /* buffer available on both AGE and LRU */
		age_time = time.tv_sec - age_bp->b_timestamp;
		lru_time = time.tv_sec - lru_bp->b_timestamp;
		if ((age_time < 0) || (lru_time < 0)) { /* time set backwards */
			bp = age_bp;
			*queue = BQ_AGE;
			/*
			 * we should probably re-timestamp eveything in the
			 * queues at this point with the current time
			 */
		} else {
			if ((lru_time >= lru_is_stale) && (age_time < age_is_stale)) {
				bp = lru_bp;
				*queue = BQ_LRU;
			} else {
				bp = age_bp;
				*queue = BQ_AGE;
			}
		}
	}

	if (!bp) { /* Neither on AGE nor on LRU */
		bp = meta_bp;
		*queue = BQ_META;
	}  else if (meta_bp) {
		bp_time = time.tv_sec - bp->b_timestamp;
		meta_time = time.tv_sec - meta_bp->b_timestamp;

		if (!(bp_time < 0) && !(meta_time < 0)) {
			/* time not set backwards */
			int bp_is_stale;
			bp_is_stale = (*queue == BQ_LRU) ? 
					lru_is_stale : age_is_stale;

			if ((meta_time >= meta_is_stale) && 
					(bp_time < bp_is_stale)) {
				bp = meta_bp;
				*queue = BQ_META;
			}
		}
	}

	if (bp == NULL)
		panic("getnewbuf: null bp");

found:
	if (bp->b_hash.le_prev == (struct buf **)0xdeadbeef) 
		panic("getnewbuf: le_prev is deadbeef");

	if(ISSET(bp->b_flags, B_BUSY))
		panic("getnewbuf reusing BUSY buf");

	/* Clean it */
	if (bcleanbuf(bp)) {
		/* bawrite() issued, buffer not ready */
		splx(s);
		*queue = req;
		goto start;
	}
	splx(s);
	return (bp); 
}
#include <mach/mach_types.h>
#include <mach/memory_object_types.h>

/* 
 * Clean a buffer.
 * Returns 0 is buffer is ready to use,
 * Returns 1 if issued a bawrite() to indicate 
 * that the buffer is not ready.
 */
int
bcleanbuf(struct buf *bp)
{
	int s;
	struct ucred *cred;

	s = splbio();

	/* Remove from the queue */
	bremfree(bp);

	/* Buffer is no longer on free lists. */
	SET(bp->b_flags, B_BUSY);

	if (bp->b_hash.le_prev == (struct buf **)0xdeadbeef) 
		panic("bcleanbuf: le_prev is deadbeef");

	/*
	 * If buffer was a delayed write, start the IO by queuing
	 * it on the LAUNDRY queue, and return 1
	 */
	if (ISSET(bp->b_flags, B_DELWRI)) {
		splx(s);
		binstailfree(bp, &bufqueues[BQ_LAUNDRY], BQ_LAUNDRY);
		blaundrycnt++;
		wakeup(&blaundrycnt);
		return (1);
	}

	if (bp->b_vp)
		brelvp(bp);
	bremhash(bp);
	BLISTNONE(bp);

	splx(s);

	if (ISSET(bp->b_flags, B_META)) {
#if ZALLOC_METADATA
		vm_offset_t elem = (vm_offset_t)bp->b_data;
		if (elem == 0)
			panic("bcleanbuf: NULL bp->b_data B_META buffer");

		if (ISSET(bp->b_flags, B_ZALLOC)) {
			if (bp->b_bufsize <= MAXMETA) {
				zone_t z;

				z = getbufzone(bp->b_bufsize);
				bp->b_data = (caddr_t)0xdeadbeef;
				zfree(z, elem);
				CLR(bp->b_flags, B_ZALLOC);
			} else
				panic("bcleanbuf: B_ZALLOC set incorrectly");
		} else {
			bp->b_data = (caddr_t)0xdeadbeef;
			kmem_free(kernel_map, elem, bp->b_bufsize); 
		}
#else
	   if (bp->b_data == 0)
		   panic("bcleanbuf: bp->b_data == NULL for B_META buffer");

	   kmem_free(kernel_map, bp->b_data, bp->b_bufsize); 
#endif /* ZALLOC_METADATA */
	}

	trace(TR_BRELSE, pack(bp->b_vp, bp->b_bufsize), bp->b_lblkno);

	/* disassociate us from our vnode, if we had one... */
	s = splbio();

	/* clear out various other fields */
	bp->b_bufsize = 0;
	bp->b_data = 0;
	bp->b_flags = B_BUSY;
	bp->b_dev = NODEV;
	bp->b_blkno = bp->b_lblkno = 0;
	bp->b_iodone = 0;
	bp->b_error = 0;
	bp->b_resid = 0;
	bp->b_bcount = 0;
	bp->b_dirtyoff = bp->b_dirtyend = 0;
	bp->b_validoff = bp->b_validend = 0;

	/* nuke any credentials we were holding */
	cred = bp->b_rcred;
	if (cred != NOCRED) {
		bp->b_rcred = NOCRED; 
		crfree(cred);
	}
	cred = bp->b_wcred;
	if (cred != NOCRED) {
		bp->b_wcred = NOCRED;
		crfree(cred);
	}
	splx(s);
	return (0);
}


/*
 * Wait for operations on the buffer to complete.
 * When they do, extract and return the I/O's error value.
 */
int
biowait(bp)
	struct buf *bp;
{
	upl_t		upl;
	upl_page_info_t *pl;
	int s;
	kern_return_t kret;

	s = splbio();
	while (!ISSET(bp->b_flags, B_DONE))
		tsleep(bp, PRIBIO + 1, "biowait", 0);
	splx(s);
	
	/* check for interruption of I/O (e.g. via NFS), then errors. */
	if (ISSET(bp->b_flags, B_EINTR)) {
		CLR(bp->b_flags, B_EINTR);
		return (EINTR);
	} else if (ISSET(bp->b_flags, B_ERROR))
		return (bp->b_error ? bp->b_error : EIO);
	else
		return (0);
}

/*
 * Mark I/O complete on a buffer.
 *
 * If a callback has been requested, e.g. the pageout
 * daemon, do so. Otherwise, awaken waiting processes.
 *
 * [ Leffler, et al., says on p.247:
 *	"This routine wakes up the blocked process, frees the buffer
 *	for an asynchronous write, or, for a request by the pagedaemon
 *	process, invokes a procedure specified in the buffer structure" ]
 *
 * In real life, the pagedaemon (or other system processes) wants
 * to do async stuff to, and doesn't want the buffer brelse()'d.
 * (for swap pager, that puts swap buffers on the free lists (!!!),
 * for the vn device, that puts malloc'd buffers on the free lists!)
 */
void
biodone(bp)
	struct buf *bp;
{
	boolean_t 	funnel_state;
	int s;

	funnel_state = thread_funnel_set(kernel_flock, TRUE);

	KERNEL_DEBUG((FSDBG_CODE(DBG_FSRW, 387)) | DBG_FUNC_START,
		     (int)bp, (int)bp->b_data, bp->b_flags, 0, 0);

	if (ISSET(bp->b_flags, B_DONE))
		panic("biodone already");
	SET(bp->b_flags, B_DONE);		/* note that it's done */
	/*
	 * I/O was done, so don't believe
	 * the DIRTY state from VM anymore
	 */
	CLR(bp->b_flags, B_WASDIRTY);

	if (!ISSET(bp->b_flags, B_READ) && !ISSET(bp->b_flags, B_RAW))
		vwakeup(bp);	 /* wake up reader */

	if (ISSET(bp->b_flags, B_CALL)) {	/* if necessary, call out */
		CLR(bp->b_flags, B_CALL);	/* but note callout done */
		(*bp->b_iodone)(bp);
	} else if (ISSET(bp->b_flags, B_ASYNC))	/* if async, release it */
		brelse(bp);
	else {		                        /* or just wakeup the buffer */	
		CLR(bp->b_flags, B_WANTED);
		wakeup(bp);
	}

	KERNEL_DEBUG((FSDBG_CODE(DBG_FSRW, 387)) | DBG_FUNC_END,
		     (int)bp, (int)bp->b_data, bp->b_flags, 0, 0);

	thread_funnel_set(kernel_flock, funnel_state);
}

/*
 * Return a count of buffers on the "locked" queue.
 */
int
count_lock_queue()
{
	register struct buf *bp;
	register int n = 0;

	for (bp = bufqueues[BQ_LOCKED].tqh_first; bp;
	    bp = bp->b_freelist.tqe_next)
		n++;
	return (n);
}

/*
 * Return a count of 'busy' buffers. Used at the time of shutdown.
 */
int
count_busy_buffers()
{
	register struct buf *bp;
	register int nbusy = 0;

	for (bp = &buf[nbuf]; --bp >= buf; )
		if ((bp->b_flags & (B_BUSY|B_INVAL)) == B_BUSY)
			nbusy++;
	return (nbusy);
}

#if 1 /*DIAGNOSTIC */
/*
 * Print out statistics on the current allocation of the buffer pool.
 * Can be enabled to print out on every ``sync'' by setting "syncprt"
 * in vfs_syscalls.c using sysctl.
 */
void
vfs_bufstats()
{
	int s, i, j, count;
	register struct buf *bp;
	register struct bqueues *dp;
	int counts[MAXBSIZE/CLBYTES+1];
	static char *bname[BQUEUES] =
		{ "LOCKED", "LRU", "AGE", "EMPTY", "META", "LAUNDRY" };

	for (dp = bufqueues, i = 0; dp < &bufqueues[BQUEUES]; dp++, i++) {
		count = 0;
		for (j = 0; j <= MAXBSIZE/CLBYTES; j++)
			counts[j] = 0;
		s = splbio();
		for (bp = dp->tqh_first; bp; bp = bp->b_freelist.tqe_next) {
			counts[bp->b_bufsize/CLBYTES]++;
			count++;
		}
		splx(s);
		printf("%s: total-%d", bname[i], count);
		for (j = 0; j <= MAXBSIZE/CLBYTES; j++)
			if (counts[j] != 0)
				printf(", %d-%d", j * CLBYTES, counts[j]);
		printf("\n");
	}
}
#endif /* DIAGNOSTIC */

#define	NRESERVEDIOBUFS	16

struct buf *
alloc_io_buf(vp, priv)
	struct vnode *vp;
	int priv;
{
	register struct buf *bp;
	int s;

	s = splbio();

	while (niobuf - NRESERVEDIOBUFS < bufstats.bufs_iobufinuse && !priv) {
		need_iobuffer = 1;
		bufstats.bufs_iobufsleeps++;
		(void) tsleep(&need_iobuffer, (PRIBIO+1), "alloc_io_buf", 0);
	}

	while ((bp = iobufqueue.tqh_first) == NULL) {
		need_iobuffer = 1;
		bufstats.bufs_iobufsleeps++;
		(void) tsleep(&need_iobuffer, (PRIBIO+1), "alloc_io_buf1", 0);
	}

	TAILQ_REMOVE(&iobufqueue, bp, b_freelist);
	bp->b_timestamp = 0; 

	/* clear out various fields */
	bp->b_flags = B_BUSY;
	bp->b_blkno = bp->b_lblkno = 0;
	bp->b_iodone = 0;
	bp->b_error = 0;
	bp->b_resid = 0;
	bp->b_bcount = 0;
	bp->b_bufsize = 0;
	bp->b_vp = vp;

	if (vp->v_type == VBLK || vp->v_type == VCHR)
		bp->b_dev = vp->v_rdev;
	else
		bp->b_dev = NODEV;
	bufstats.bufs_iobufinuse++;
	if (bufstats.bufs_iobufinuse > bufstats.bufs_iobufmax)
		bufstats.bufs_iobufmax = bufstats.bufs_iobufinuse;
	splx(s);

	return (bp);
}

void
free_io_buf(bp)
	struct buf *bp;
{
	int s;

	s = splbio();
	/* put buffer back on the head of the iobufqueue */
	bp->b_vp = NULL;
	bp->b_flags = B_INVAL;

	binsheadfree(bp, &iobufqueue, -1);

	/* Wake up any processes waiting for any buffer to become free. */
	if (need_iobuffer) {
		need_iobuffer = 0;
		wakeup(&need_iobuffer);
	}
	bufstats.bufs_iobufinuse--;
	splx(s);
}


/* not hookedup yet */

/* XXX move this to a separate file */
/*
 * Dynamic Scaling of the Buffer Queues
 */

typedef long long blsize_t;

blsize_t MAXNBUF; /* initialize to (mem_size / PAGE_SIZE) */
/* Global tunable limits */
blsize_t nbufh;			/* number of buffer headers */
blsize_t nbuflow;		/* minimum number of buffer headers required */
blsize_t nbufhigh;		/* maximum number of buffer headers allowed */
blsize_t nbuftarget;	/* preferred number of buffer headers */

/*
 * assertions:
 *
 * 1.	0 < nbuflow <= nbufh <= nbufhigh
 * 2.	nbufhigh <= MAXNBUF
 * 3.	0 < nbuflow <= nbuftarget <= nbufhigh
 * 4.	nbufh can not be set by sysctl().
 */

/* Per queue tunable limits */

struct bufqlim {
	blsize_t	bl_nlow;	/* minimum number of buffer headers required */
	blsize_t	bl_num;		/* number of buffer headers on the queue */
	blsize_t	bl_nlhigh;	/* maximum number of buffer headers allowed */
	blsize_t	bl_target;	/* preferred number of buffer headers */
	long	bl_stale;	/* Seconds after which a buffer is considered stale */
} bufqlim[BQUEUES];

/*
 * assertions:
 *
 * 1.	0 <= bl_nlow <= bl_num <= bl_nlhigh
 * 2.	bl_nlhigh <= MAXNBUF
 * 3.  bufqlim[BQ_META].bl_nlow != 0
 * 4.  bufqlim[BQ_META].bl_nlow > (number of possible concurrent 
 *									file system IO operations)
 * 5.	bl_num can not be set by sysctl().
 * 6.	bl_nhigh <= nbufhigh
 */

/*
 * Rationale:
 * ----------
 * Defining it blsize_t as long permits 2^31 buffer headers per queue.
 * Which can describe (2^31 * PAGE_SIZE) memory per queue.
 * 
 * These limits are exported to by means of sysctl().
 * It was decided to define blsize_t as a 64 bit quantity.
 * This will make sure that we will not be required to change it
 * as long as we do not exceed 64 bit address space for the kernel.
 * 
 * low and high numbers parameters initialized at compile time
 * and boot arguments can be used to override them. sysctl() 
 * would not change the value. sysctl() can get all the values 
 * but can set only target. num is the current level.
 *
 * Advantages of having a "bufqscan" thread doing the balancing are, 
 * Keep enough bufs on BQ_EMPTY.
 *	getnewbuf() by default will always select a buffer from the BQ_EMPTY.
 *		getnewbuf() perfoms best if a buffer was found there.
 *		Also this minimizes the possibility of starting IO
 *		from getnewbuf(). That's a performance win, too.
 *
 *	Localize complex logic [balancing as well as time aging]
 *		to balancebufq().
 *
 *	Simplify getnewbuf() logic by elimination of time aging code.
 */

/* 
 * Algorithm:
 * -----------
 * The goal of the dynamic scaling of the buffer queues to to keep
 * the size of the LRU close to bl_target. Buffers on a queue would
 * be time aged.
 *
 * There would be a thread which will be responsible for "balancing"
 * the buffer cache queues.
 *
 * The scan order would be:	AGE, LRU, META, EMPTY.
 */

long bufqscanwait = 0;

extern void bufqscan_thread();
extern int balancebufq(int q);
extern int btrimempty(int n);
extern int initbufqscan(void);
extern int nextbufq(int q);
extern void buqlimprt(int all);

void
bufq_balance_thread_init()
{

	if (bufqscanwait++ == 0) {
		int i;

		/* Initalize globals */
		MAXNBUF = (mem_size / PAGE_SIZE);
		nbufh = nbuf;
		nbuflow = min(nbufh, 100);
		nbufhigh = min(MAXNBUF, max(nbufh, 2048));
		nbuftarget = (mem_size >> 5) / PAGE_SIZE;
		nbuftarget = max(nbuflow, nbuftarget);
		nbuftarget = min(nbufhigh, nbuftarget);

		/*
		 * Initialize the bufqlim 
		 */ 

		/* LOCKED queue */
		bufqlim[BQ_LOCKED].bl_nlow = 0;
		bufqlim[BQ_LOCKED].bl_nlhigh = 32;
		bufqlim[BQ_LOCKED].bl_target = 0;
		bufqlim[BQ_LOCKED].bl_stale = 30;

		/* LRU queue */
		bufqlim[BQ_LRU].bl_nlow = 0;
		bufqlim[BQ_LRU].bl_nlhigh = nbufhigh/4;
		bufqlim[BQ_LRU].bl_target = nbuftarget/4;
		bufqlim[BQ_LRU].bl_stale = LRU_IS_STALE;

		/* AGE queue */
		bufqlim[BQ_AGE].bl_nlow = 0;
		bufqlim[BQ_AGE].bl_nlhigh = nbufhigh/4;
		bufqlim[BQ_AGE].bl_target = nbuftarget/4;
		bufqlim[BQ_AGE].bl_stale = AGE_IS_STALE;

		/* EMPTY queue */
		bufqlim[BQ_EMPTY].bl_nlow = 0;
		bufqlim[BQ_EMPTY].bl_nlhigh = nbufhigh/4;
		bufqlim[BQ_EMPTY].bl_target = nbuftarget/4;
		bufqlim[BQ_EMPTY].bl_stale = 600000;

		/* META queue */
		bufqlim[BQ_META].bl_nlow = 0;
		bufqlim[BQ_META].bl_nlhigh = nbufhigh/4;
		bufqlim[BQ_META].bl_target = nbuftarget/4;
		bufqlim[BQ_META].bl_stale = META_IS_STALE;

		/* LAUNDRY queue */
		bufqlim[BQ_LOCKED].bl_nlow = 0;
		bufqlim[BQ_LOCKED].bl_nlhigh = 32;
		bufqlim[BQ_LOCKED].bl_target = 0;
		bufqlim[BQ_LOCKED].bl_stale = 30;

		buqlimprt(1);
	}

	/* create worker thread */
	kernel_thread(kernel_task, bufqscan_thread);
}

/* The workloop for the buffer balancing thread */
void
bufqscan_thread()
{
	boolean_t 	funnel_state;
	int moretodo = 0;

	funnel_state = thread_funnel_set(kernel_flock, TRUE);

	for(;;) {
		do {
			int q;	/* buffer queue to process */
		
			for (q = initbufqscan(); q; ) {
				moretodo |= balancebufq(q);
				q = nextbufq(q);
			}
		} while (moretodo);

#if 1 || DIAGNOSTIC
		vfs_bufstats();
		buqlimprt(0);
#endif
		(void)tsleep((void *)&bufqscanwait, PRIBIO, "bufqscanwait", 60 * hz);
		moretodo = 0;
	}

	(void) thread_funnel_set(kernel_flock, FALSE);
}

/* Seed for the buffer queue balancing */
int
initbufqscan()
{
	/* Start with AGE queue */
	return (BQ_AGE);
}

/* Pick next buffer queue to balance */
int
nextbufq(int q)
{
	int order[] = { BQ_AGE, BQ_LRU, BQ_META, BQ_EMPTY, 0 };
	
	q++;
	q %= sizeof(order);
	return (order[q]);
}

/* function to balance the buffer queues */
int
balancebufq(int q)
{
	int moretodo = 0;
	int s = splbio();
	int n;
	
	/* reject invalid q */
	if ((q < 0) || (q >= BQUEUES))
		goto out;

	/* LOCKED or LAUNDRY queue MUST not be balanced */
	if ((q == BQ_LOCKED) || (q == BQ_LAUNDRY))
		goto out;

	n = (bufqlim[q].bl_num - bufqlim[q].bl_target);

	/* If queue has less than target nothing more to do */
	if (n < 0)
		goto out;

	if ( n > 8 ) {
		/* Balance only a small amount (12.5%) at a time */
		n >>= 3;
	}

	/* EMPTY queue needs special handling */
	if (q == BQ_EMPTY) {
		moretodo |= btrimempty(n);
		goto out;
	}
	
	for (; n > 0; n--) {
		struct buf *bp = bufqueues[q].tqh_first;
		if (!bp)
			break;
		
		/* check if it's stale */
		if ((time.tv_sec - bp->b_timestamp) > bufqlim[q].bl_stale) {
			if (bcleanbuf(bp)) {
				/* bawrite() issued, bp not ready */
				moretodo = 1;
			} else {
				/* release the cleaned buffer to BQ_EMPTY */
				SET(bp->b_flags, B_INVAL);
				brelse(bp);
			}
		} else
			break;		
	}

out:
	splx(s);
	return (moretodo);		
}

int
btrimempty(int n)
{
	/*
	 * When struct buf are allocated dynamically, this would
	 * reclaim upto 'n' struct buf from the empty queue.
	 */
	 
	 return (0);
}

void
bufqinc(int q)
{
	if ((q < 0) || (q >= BQUEUES))
		return;

	bufqlim[q].bl_num++;
	return;
}

void
bufqdec(int q)
{
	if ((q < 0) || (q >= BQUEUES))
		return; 

	bufqlim[q].bl_num--;
	return;
}

void
buqlimprt(int all)
{
	int i;
    static char *bname[BQUEUES] =
		{ "LOCKED", "LRU", "AGE", "EMPTY", "META", "LAUNDRY" };

	if (all)
		for (i = 0; i < BQUEUES; i++) {
			printf("%s : ", bname[i]);
			printf("min = %d, ", (long)bufqlim[i].bl_nlow);
			printf("cur = %d, ", (long)bufqlim[i].bl_num);
			printf("max = %d, ", (long)bufqlim[i].bl_nlhigh);
			printf("target = %d, ", (long)bufqlim[i].bl_target);
			printf("stale after %d seconds\n", bufqlim[i].bl_stale);
		}
	else
		for (i = 0; i < BQUEUES; i++) {
			printf("%s : ", bname[i]);
			printf("cur = %d, ", (long)bufqlim[i].bl_num);
		}
}

/*
 * If the getnewbuf() calls bcleanbuf() on the same thread
 * there is a potential for stack overrun and deadlocks.
 * So we always handoff the work to worker thread for completion
 */

static void
bcleanbuf_thread_init()
{
	static void bcleanbuf_thread();

	/* create worker thread */
	kernel_thread(kernel_task, bcleanbuf_thread);
}

static void
bcleanbuf_thread()
{
	boolean_t 	funnel_state;
	struct buf *bp;

	funnel_state = thread_funnel_set(kernel_flock, TRUE);

doit:
	while (blaundrycnt == 0)
		(void)tsleep((void *)&blaundrycnt, PRIBIO, "blaundry", 60 * hz);
	bp = TAILQ_FIRST(&bufqueues[BQ_LAUNDRY]);
	/* Remove from the queue */
	bremfree(bp);
	blaundrycnt--;
	/* do the IO */
	bawrite(bp);
	/* start again */
	goto doit;

	(void) thread_funnel_set(kernel_flock, funnel_state);
}


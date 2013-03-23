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
/*
 * Copyright (c) 1993
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
 *	@(#)vfs_cluster.c	8.10 (Berkeley) 3/28/95
 */

#include <sys/param.h>
#include <sys/proc.h>
#include <sys/buf.h>
#include <sys/vnode.h>
#include <sys/mount.h>
#include <sys/trace.h>
#include <sys/malloc.h>
#include <sys/resourcevar.h>
#include <libkern/libkern.h>

#include <sys/ubc.h>
#include <vm/vm_pageout.h>
#include <mach/memory_object_types.h>

#include <sys/kdebug.h>


#define CL_READ      0x01
#define CL_ASYNC     0x02
#define CL_COMMIT    0x04
#define CL_NOMAP     0x08
#define CL_PAGEOUT   0x10
#define CL_AGE       0x20
#define CL_DUMP      0x40
#define CL_NOZERO    0x80
#define CL_PAGEIN    0x100

/*
 * throttle the number of async writes that
 * can be outstanding on a single vnode
 * before we issue a synchronous write 
 */
#define ASYNC_THROTTLE  3

static int
cluster_iodone(bp)
	struct buf *bp;
{
        int         b_flags;
        int         error;
	int         total_size;
	int         total_resid;
	int         upl_offset;
	upl_t       upl;
	struct buf *cbp;
	struct buf *cbp_head;
	struct buf *cbp_next;
	struct buf *real_bp;
	int         commit_size;
	int         pg_offset;


	cbp_head = (struct buf *)(bp->b_trans_head);

	KERNEL_DEBUG((FSDBG_CODE(DBG_FSRW, 20)) | DBG_FUNC_START,
		     cbp_head, bp->b_lblkno, bp->b_bcount, bp->b_flags, 0);

	for (cbp = cbp_head; cbp; cbp = cbp->b_trans_next) {
	        /*
		 * all I/O requests that are part of this transaction
		 * have to complete before we can process it
		 */
	        if ( !(cbp->b_flags & B_DONE)) {

		        KERNEL_DEBUG((FSDBG_CODE(DBG_FSRW, 20)) | DBG_FUNC_END,
				     cbp_head, cbp, cbp->b_bcount, cbp->b_flags, 0);

		        return 0;
		}
	}
	error       = 0;
	total_size  = 0;
	total_resid = 0;

	cbp        = cbp_head;
	upl_offset = cbp->b_uploffset;
	upl        = cbp->b_pagelist;
	b_flags    = cbp->b_flags;
	real_bp    = cbp->b_real_bp;

	while (cbp) {
		if (cbp->b_vectorcount > 1)
		        _FREE(cbp->b_vectorlist, M_SEGMENT);

		if ((cbp->b_flags & B_ERROR) && error == 0)
		        error = cbp->b_error;

		total_resid += cbp->b_resid;
		total_size  += cbp->b_bcount;

		cbp_next = cbp->b_trans_next;

		free_io_buf(cbp);

		cbp = cbp_next;
	}
	if ((b_flags & B_NEED_IODONE) && real_bp) {
		if (error) {
		        real_bp->b_flags |= B_ERROR;
			real_bp->b_error = error;
		}
		real_bp->b_resid = total_resid;

		biodone(real_bp);
	}
	if (error == 0 && total_resid)
	        error = EIO;

	if (b_flags & B_COMMIT_UPL) {
		pg_offset   = upl_offset & PAGE_MASK;
		commit_size = (((pg_offset + total_size) + (PAGE_SIZE - 1)) / PAGE_SIZE) * PAGE_SIZE;

		if (error || (b_flags & B_NOCACHE)) {
		        int upl_abort_code;

			if (b_flags & B_PAGEOUT)
			        upl_abort_code = UPL_ABORT_FREE_ON_EMPTY;
			else
			        upl_abort_code = UPL_ABORT_FREE_ON_EMPTY | UPL_ABORT_DUMP_PAGES;

			kernel_upl_abort_range(upl, upl_offset - pg_offset, commit_size, upl_abort_code);
			
		        KERNEL_DEBUG((FSDBG_CODE(DBG_FSRW, 20)) | DBG_FUNC_END,
				     upl, upl_offset - pg_offset, commit_size,
				     0x80000000|upl_abort_code, 0);

		} else {
		        int upl_commit_flags = UPL_COMMIT_FREE_ON_EMPTY;

			if ( !(b_flags & B_PAGEOUT))
			        upl_commit_flags |= UPL_COMMIT_CLEAR_DIRTY;
			if (b_flags & B_AGE)
			        upl_commit_flags |= UPL_COMMIT_INACTIVATE;

			kernel_upl_commit_range(upl, upl_offset - pg_offset, 
					commit_size, upl_commit_flags,
				 	UPL_GET_INTERNAL_PAGE_LIST(upl), 
					MAX_UPL_TRANSFER);

		        KERNEL_DEBUG((FSDBG_CODE(DBG_FSRW, 20)) | DBG_FUNC_END,
				     upl, upl_offset - pg_offset, commit_size,
				     upl_commit_flags, 0);
		}
	} else 
	        KERNEL_DEBUG((FSDBG_CODE(DBG_FSRW, 20)) | DBG_FUNC_END,
			     upl, upl_offset, 0, error, 0);

	return (error);
}


static void
cluster_zero(upl, upl_offset, size, flags, bp)
	upl_t         upl;
	vm_offset_t   upl_offset;
	int           size;
	int           flags;
	struct buf   *bp;
{
        vm_offset_t   io_addr = 0;
	kern_return_t kret;

	if ( !(flags & CL_NOMAP)) {
	        kret = kernel_upl_map(kernel_map, upl, &io_addr);
		
		if (kret != KERN_SUCCESS)
		        panic("cluster_zero: kernel_upl_map() failed with (%d)", kret);
		if (io_addr == 0) 
		        panic("cluster_zero: kernel_upl_map mapped 0");
	} else
	        io_addr = (vm_offset_t)bp->b_data;
	bzero((caddr_t)(io_addr + upl_offset), size);
	
	if ( !(flags & CL_NOMAP)) {
	        kret = kernel_upl_unmap(kernel_map, upl);

		if (kret != KERN_SUCCESS)
		        panic("cluster_zero: kernel_upl_unmap failed");
	}
}


static int
cluster_io(vp, upl, upl_offset, f_offset, size, flags, real_bp)
	struct vnode *vp;
	upl_t         upl;
	vm_offset_t   upl_offset;
	off_t         f_offset;
	int           size;
	int           flags;
	struct buf   *real_bp;
{
	struct buf   *cbp;
	struct iovec *iovp;
	int           io_flags;
	int           error = 0;
	int           retval = 0;
	struct buf   *cbp_head = 0;
	struct buf   *cbp_tail = 0;
	upl_page_info_t *pl;
	int pg_count;
	int pg_offset;

	if (flags & CL_READ)
	        io_flags = (B_VECTORLIST | B_READ);
	else
	        io_flags = (B_VECTORLIST | B_WRITEINPROG);

	pl = UPL_GET_INTERNAL_PAGE_LIST(upl);

	if (flags & CL_ASYNC)
	        io_flags |= (B_CALL | B_ASYNC);
	if (flags & CL_AGE)
	        io_flags |= B_AGE;
	if (flags & CL_DUMP)
	        io_flags |= B_NOCACHE;


	KERNEL_DEBUG((FSDBG_CODE(DBG_FSRW, 22)) | DBG_FUNC_START,
		     (int)f_offset, size, upl_offset, flags, 0);

	if ((flags & CL_READ) && ((upl_offset + size) & PAGE_MASK) && (!(flags & CL_NOZERO))) {
	        /*
		 * then we are going to end up
		 * with a page that we can't complete (the file size wasn't a multiple
		 * of PAGE_SIZE and we're trying to read to the end of the file
		 * so we'll go ahead and zero out the portion of the page we can't
		 * read in from the file
		 */
	        cluster_zero(upl, upl_offset + size, PAGE_SIZE - ((upl_offset + size) & PAGE_MASK), flags, real_bp);

		KERNEL_DEBUG((FSDBG_CODE(DBG_FSRW, 23)) | DBG_FUNC_NONE,
			     upl_offset + size, PAGE_SIZE - ((upl_offset + size) & PAGE_MASK),
			     flags, real_bp, 0);
	}
	while (size) {
		size_t io_size;
		int vsize;
		int i;
		int pl_index;
		int pg_resid;
		int num_contig;
		daddr_t lblkno;
		daddr_t blkno;

		if (size > MAXPHYSIO)
		        io_size = MAXPHYSIO;
		else
		        io_size = size;

		if (error = VOP_CMAP(vp, f_offset, io_size, &blkno, &io_size, NULL)) {
		        if (error == EOPNOTSUPP)
			        panic("VOP_CMAP Unimplemented");
			break;
		}

		KERNEL_DEBUG((FSDBG_CODE(DBG_FSRW, 24)) | DBG_FUNC_NONE,
			     (int)f_offset, (int)blkno, io_size, 0, 0);

		if ( (!(flags & CL_READ) && (long)blkno == -1) || io_size == 0) {
		        error = EINVAL;
			break;
		}
		lblkno = (daddr_t)(f_offset / PAGE_SIZE_64);
		/*
		 * we have now figured out how much I/O we can do - this is in 'io_size'
		 * pl_index represents the first page in the 'upl' that the I/O will occur for
		 * pg_offset is the starting point in the first page for the I/O
		 * pg_count is the number of full and partial pages that 'io_size' encompasses
		 */
		pl_index  = upl_offset / PAGE_SIZE; 
		pg_offset = upl_offset & PAGE_MASK;
		pg_count  = (io_size + pg_offset + (PAGE_SIZE - 1)) / PAGE_SIZE;

		if ((flags & CL_READ) && (long)blkno == -1) {
		        /*
			 * if we're reading and blkno == -1, then we've got a
			 * 'hole' in the file that we need to deal with by zeroing
			 * out the affected area in the upl
			 */
		        cluster_zero(upl, upl_offset, io_size, flags, real_bp);

			KERNEL_DEBUG((FSDBG_CODE(DBG_FSRW, 23)) | DBG_FUNC_NONE,
				     upl_offset, io_size, flags, real_bp, 0);

			pg_count = (io_size - pg_offset) / PAGE_SIZE;

			if (io_size == size && ((upl_offset + io_size) & PAGE_MASK))
			        pg_count++;

			if (pg_count) {
			        if (pg_offset)
				        pg_resid = PAGE_SIZE - pg_offset;
				else
				        pg_resid = 0;
				if (flags & CL_COMMIT)
				        kernel_upl_commit_range(upl, 
						upl_offset + pg_resid, 
						pg_count * PAGE_SIZE,
		 				UPL_COMMIT_CLEAR_DIRTY 
						   | UPL_COMMIT_FREE_ON_EMPTY, 
						pl, MAX_UPL_TRANSFER);
			}
			upl_offset += io_size;
			f_offset   += io_size;
			size       -= io_size;

			if (cbp_head && pg_count)
			        goto start_io;
			continue;
		} else if (real_bp && (real_bp->b_blkno == real_bp->b_lblkno)) {
		        real_bp->b_blkno = blkno;
		}
		if (pg_count > 1) {
		        /* 
			 * we need to allocate space for the vector list
			 */
		        iovp = (struct iovec *)_MALLOC(sizeof(struct iovec) * pg_count,
						       M_SEGMENT, M_NOWAIT);
			if (iovp == (struct iovec *) 0) {
			        /*
				 * if the allocation fails, then throttle down to a single page
				 */
			        io_size = PAGE_SIZE - pg_offset;
				pg_count = 1;
			}
		}
		cbp = alloc_io_buf(vp);


		if (pg_count == 1)
		        /*
			 * we use the io vector that's reserved in the buffer header
			 * this insures we can always issue an I/O even in a low memory
			 * condition that prevents the _MALLOC from succeeding... this
			 * is necessary to prevent deadlocks with the pager
			 */
			iovp = (struct iovec *)(&cbp->b_vects[0]);

		cbp->b_vectorlist  = (void *)iovp;
		cbp->b_vectorcount = pg_count;

		for (i = 0, vsize = io_size; i < pg_count; i++, iovp++) {
		        int     psize;

		        psize = PAGE_SIZE - pg_offset;

			if (psize > vsize)
			        psize = vsize;

			iovp->iov_len  = psize;
		        iovp->iov_base = (caddr_t)upl_phys_page(pl, pl_index + i);

			if (iovp->iov_base == (caddr_t) 0) {
				if (pg_count > 1)
				        _FREE(cbp->b_vectorlist, M_SEGMENT);
			        free_io_buf(cbp);

				error = EINVAL;
				break;
			}
			iovp->iov_base += pg_offset;
			pg_offset = 0;

			if (flags & CL_PAGEOUT) {
			        int         s;
				struct buf *bp;

			        s = splbio();
				if (bp = incore(vp, lblkno + i)) {
				        if (!ISSET(bp->b_flags, B_BUSY)) {
					        bremfree(bp);
						SET(bp->b_flags, (B_BUSY | B_INVAL));
						splx(s);
						brelse(bp);
					} else
					        panic("BUSY bp found in cluster_io");
				}
				splx(s);
			}
			vsize -= psize;
		}
		if (error)
		        break;

		if (flags & CL_ASYNC)
			cbp->b_iodone = (void *)cluster_iodone;
		cbp->b_flags |= io_flags;

		cbp->b_lblkno = lblkno;
		cbp->b_blkno  = blkno;
		cbp->b_bcount = io_size;
		cbp->b_pagelist  = upl;
		cbp->b_uploffset = upl_offset;
		cbp->b_trans_next = (struct buf *)0;

		if (flags & CL_READ)
			KERNEL_DEBUG((FSDBG_CODE(DBG_FSRW, 26)) | DBG_FUNC_NONE,
				     cbp->b_lblkno, cbp->b_blkno, upl_offset, io_size, 0);
		else
			KERNEL_DEBUG((FSDBG_CODE(DBG_FSRW, 27)) | DBG_FUNC_NONE,
				     cbp->b_lblkno, cbp->b_blkno, upl_offset, io_size, 0);

		if (cbp_head) {
		        cbp_tail->b_trans_next = cbp;
			cbp_tail = cbp;
		} else {
		        cbp_head = cbp;
			cbp_tail = cbp;
		}
		(struct buf *)(cbp->b_trans_head) = cbp_head;

		upl_offset += io_size;
		f_offset   += io_size;
		size       -= io_size;

		if ( !(upl_offset & PAGE_MASK) || size == 0) {
		        /*
			 * if we have no more I/O to issue or
			 * the current I/O we've prepared fully
			 * completes the last page in this request
			 * or it's been completed via a zero-fill
			 * due to a 'hole' in the file
			 * then go ahead and issue the I/O
			 */
start_io:		
		        if (flags & CL_COMMIT)
			        cbp_head->b_flags |= B_COMMIT_UPL;
			if (flags & CL_PAGEOUT)
			        cbp_head->b_flags |= B_PAGEOUT;

			if (real_bp) {
			        cbp_head->b_flags |= B_NEED_IODONE;
				cbp_head->b_real_bp = real_bp;
			}

		        for (cbp = cbp_head; cbp;) {
				struct buf * cbp_next;

			        if (io_flags & B_WRITEINPROG)
				        cbp->b_vp->v_numoutput++;

				cbp_next = cbp->b_trans_next;

				(void) VOP_STRATEGY(cbp);
				cbp = cbp_next;
			}
			if ( !(flags & CL_ASYNC)) {
			        for (cbp = cbp_head; cbp; cbp = cbp->b_trans_next)
				        biowait(cbp);

				if (error = cluster_iodone(cbp_head)) {
					retval = error;
					error  = 0;
				}
			}
			cbp_head = (struct buf *)0;
			cbp_tail = (struct buf *)0;
		}
	}
	if (error) {
	        for (cbp = cbp_head; cbp;) {
			struct buf * cbp_next;
 
		        if (cbp->b_vectorcount > 1)
			        _FREE(cbp->b_vectorlist, M_SEGMENT);
			cbp_next = cbp->b_trans_next;
			free_io_buf(cbp);
			cbp = cbp_next;

		}
		pg_offset = upl_offset & PAGE_MASK;
		pg_count  = (size + pg_offset + (PAGE_SIZE - 1)) / PAGE_SIZE;

		if (flags & CL_COMMIT) {
		        int upl_abort_code;

			if (flags & CL_PAGEOUT)
			        upl_abort_code = UPL_ABORT_FREE_ON_EMPTY;
			else if (flags & CL_PAGEIN)
			        upl_abort_code = UPL_ABORT_FREE_ON_EMPTY | UPL_ABORT_ERROR;
			else
			        upl_abort_code = UPL_ABORT_FREE_ON_EMPTY | UPL_ABORT_DUMP_PAGES;

		        kernel_upl_abort_range(upl, upl_offset - pg_offset, pg_count * PAGE_SIZE, upl_abort_code);

			KERNEL_DEBUG((FSDBG_CODE(DBG_FSRW, 28)) | DBG_FUNC_NONE,
				     upl, upl_offset - pg_offset, pg_count * PAGE_SIZE, error, 0);
		}
		if (real_bp) {
		        real_bp->b_flags |= B_ERROR;
			real_bp->b_error  = error;

			biodone(real_bp);
		}
		if (retval == 0)
		        retval = error;
	}
	KERNEL_DEBUG((FSDBG_CODE(DBG_FSRW, 22)) | DBG_FUNC_END,
		     (int)f_offset, size, upl_offset, retval, 0);

	return (retval);
}


static int
cluster_rd_prefetch(vp, object, f_offset, size, filesize, devblocksize)
	struct vnode *vp;
        void         *object;
	off_t         f_offset;
	u_int         size;
	off_t         filesize;
	int           devblocksize;
{
	upl_t         upl;
	upl_page_info_t *pl;
	int           pages_in_upl;
	int           start_pg;
	int           last_pg;
	int           last_valid;
	int           io_size;


	KERNEL_DEBUG((FSDBG_CODE(DBG_FSRW, 49)) | DBG_FUNC_START,
		     (int)f_offset, size, (int)filesize, 0, 0);

	if (f_offset >= filesize) {
	        KERNEL_DEBUG((FSDBG_CODE(DBG_FSRW, 49)) | DBG_FUNC_END,
			     (int)f_offset, 0, 0, 0, 0);
	        return(0);
	}
	if (memory_object_page_op(object, (vm_offset_t)f_offset, 0, 0, 0) == KERN_SUCCESS) {
	        KERNEL_DEBUG((FSDBG_CODE(DBG_FSRW, 49)) | DBG_FUNC_END,
			     (int)f_offset, 0, 0, 0, 0);
	        return(0);
	}
	if (size > MAXPHYSIO)
	        size = MAXPHYSIO;
	else
	        size = (size + (PAGE_SIZE - 1)) & ~(PAGE_SIZE - 1);

	if ((off_t)size > (filesize - f_offset))
	        size = ((filesize - f_offset) + (devblocksize - 1)) & ~(devblocksize - 1);
	
	pages_in_upl = (size + (PAGE_SIZE - 1)) / PAGE_SIZE;


	vm_fault_list_request(object, (vm_object_offset_t)f_offset, pages_in_upl * PAGE_SIZE, &upl, NULL, 0,
				      UPL_CLEAN_IN_PLACE | UPL_NO_SYNC | UPL_SET_INTERNAL);
	if (upl == (upl_t) 0)
	        return(0);

	pl = UPL_GET_INTERNAL_PAGE_LIST(upl);

	/*
	 * scan from the beginning of the upl looking for the first
	 * non-valid page.... this will become the first page in
	 * the request we're going to make to 'cluster_io'... if all
	 * of the pages are valid, we won't call through to 'cluster_io'
	 */
	for (start_pg = 0; start_pg < pages_in_upl; start_pg++) {
	        if (!upl_valid_page(pl, start_pg))
		        break;
	}

	/*
	 * scan from the starting invalid page looking for a valid
	 * page before the end of the upl is reached, if we 
	 * find one, then it will be the last page of the request to
	 * 'cluster_io'
	 */
	for (last_pg = start_pg; last_pg < pages_in_upl; last_pg++) {
	        if (upl_valid_page(pl, last_pg))
		        break;
	}

	/*
	 * if we find any more free valid pages at the tail of the upl
	 * than update maxra accordingly....
	 */
	for (last_valid = last_pg; last_valid < pages_in_upl; last_valid++) {
	        if (!upl_valid_page(pl, last_valid))
		        break;
	}
	if (start_pg < last_pg) {		
	        vm_offset_t   upl_offset;

	        /*
		 * we found a range of 'invalid' pages that must be filled
		 * 'size' has already been clipped to the LEOF
		 * make sure it's at least a multiple of the device block size
		 */
	        upl_offset = start_pg * PAGE_SIZE;
		io_size    = (last_pg - start_pg) * PAGE_SIZE;

		if ((upl_offset + io_size) > size) {
		        io_size = size - upl_offset;

			KERNEL_DEBUG(0xd001000, upl_offset, size, io_size, 0, 0);
		}
		cluster_io(vp, upl, upl_offset, f_offset + upl_offset, io_size,
			   CL_READ | CL_COMMIT | CL_ASYNC | CL_AGE, (struct buf *)0);
	}
	if (start_pg) {
	        /*
		 * start_pg of non-zero indicates we found some already valid pages
		 * at the beginning of the upl.... we need to release these without
		 * modifying there state
		 */
	        kernel_upl_abort_range(upl, 0, start_pg * PAGE_SIZE, UPL_ABORT_FREE_ON_EMPTY);

		KERNEL_DEBUG((FSDBG_CODE(DBG_FSRW, 50)) | DBG_FUNC_NONE,
			     upl, 0, start_pg * PAGE_SIZE, 0, 0);
	}
	if (last_pg < pages_in_upl) {
		/*
		 * the set of pages that we issued an I/O for did not extend all the
		 * way to the end of the upl... so just release them without modifying
		 * there state
		 */
	        kernel_upl_abort_range(upl, last_pg * PAGE_SIZE, (pages_in_upl - last_pg) * PAGE_SIZE,
				UPL_ABORT_FREE_ON_EMPTY);

		KERNEL_DEBUG((FSDBG_CODE(DBG_FSRW, 50)) | DBG_FUNC_NONE,
			     upl, last_pg * PAGE_SIZE, (pages_in_upl - last_pg) * PAGE_SIZE, 0, 0);
	}

	KERNEL_DEBUG((FSDBG_CODE(DBG_FSRW, 49)) | DBG_FUNC_END,
		     (int)f_offset + (last_valid * PAGE_SIZE), 0, 0, 0, 0);

	return(last_valid);
}



static void
cluster_rd_ahead(vp, object, b_lblkno, e_lblkno, filesize, devblocksize)
	struct vnode *vp;
        void         *object;
	daddr_t       b_lblkno;
	daddr_t       e_lblkno;
	off_t         filesize;
	int           devblocksize;
{
	daddr_t       r_lblkno;
	off_t         f_offset;
	int           size_of_prefetch;


	KERNEL_DEBUG((FSDBG_CODE(DBG_FSRW, 48)) | DBG_FUNC_START,
		     b_lblkno, e_lblkno, vp->v_lastr, 0, 0);

	if (b_lblkno == vp->v_lastr && b_lblkno == e_lblkno) {
		KERNEL_DEBUG((FSDBG_CODE(DBG_FSRW, 48)) | DBG_FUNC_END,
			     vp->v_ralen, vp->v_maxra, vp->v_lastr, 0, 0);
		return;
	}

	if (vp->v_lastr == -1 || (b_lblkno != vp->v_lastr && b_lblkno != (vp->v_lastr + 1) && b_lblkno != (vp->v_maxra + 1))) {
	        vp->v_ralen = 0;
		vp->v_maxra = 0;

		KERNEL_DEBUG((FSDBG_CODE(DBG_FSRW, 48)) | DBG_FUNC_END,
			     vp->v_ralen, vp->v_maxra, vp->v_lastr, 1, 0);

		return;
	}
	vp->v_ralen = vp->v_ralen ? min(MAXPHYSIO/PAGE_SIZE, vp->v_ralen << 1) : 1;

	if (((e_lblkno + 1) - b_lblkno) > vp->v_ralen)
	        vp->v_ralen = min(MAXPHYSIO/PAGE_SIZE, (e_lblkno + 1) - b_lblkno);

	if (e_lblkno < vp->v_maxra) {
	        if ((vp->v_maxra - e_lblkno) > ((MAXPHYSIO/PAGE_SIZE) / 4)) {

		        KERNEL_DEBUG((FSDBG_CODE(DBG_FSRW, 48)) | DBG_FUNC_END,
				     vp->v_ralen, vp->v_maxra, vp->v_lastr, 2, 0);
			return;
		}
	}
	r_lblkno = max(e_lblkno, vp->v_maxra) + 1;
	f_offset = (off_t)r_lblkno * PAGE_SIZE_64;

	size_of_prefetch = cluster_rd_prefetch(vp, object, f_offset, vp->v_ralen * PAGE_SIZE, filesize, devblocksize);

	if (size_of_prefetch)
	        vp->v_maxra = r_lblkno + (size_of_prefetch - 1);

	KERNEL_DEBUG((FSDBG_CODE(DBG_FSRW, 48)) | DBG_FUNC_END,
		     vp->v_ralen, vp->v_maxra, vp->v_lastr, 3, 0);
}


cluster_pageout(vp, upl, upl_offset, f_offset, size, filesize, devblocksize, flags)
	struct vnode *vp;
	upl_t         upl;
	vm_offset_t   upl_offset;
	off_t         f_offset;
	int           size;
	off_t         filesize;
	int           devblocksize;
	int           flags;
{
	int           io_size;
	int           pg_size;
        off_t         max_size;
	int local_flags = CL_PAGEOUT;

	if ((flags & UPL_IOSYNC) == 0) 
		local_flags |= CL_ASYNC;
	if ((flags & UPL_NOCOMMIT) == 0) 
		local_flags |= CL_COMMIT;

	if (upl == (upl_t) 0)
	        panic("cluster_pageout: can't handle NULL upl yet\n");


	KERNEL_DEBUG((FSDBG_CODE(DBG_FSRW, 52)) | DBG_FUNC_NONE,
		     (int)f_offset, size, (int)filesize, local_flags, 0);

	/*
	 * If they didn't specify any I/O, then we are done...
	 * we can't issue an abort because we don't know how
	 * big the upl really is
	 */
	if (size <= 0)
		return (EINVAL);

        if (vp->v_mount->mnt_flag & MNT_RDONLY) {
		if (local_flags & CL_COMMIT)
		        kernel_upl_abort_range(upl, upl_offset, size, UPL_ABORT_FREE_ON_EMPTY);
		return (EROFS);
	}
	/*
	 * can't page-in from a negative offset
	 * or if we're starting beyond the EOF
	 * or if the file offset isn't page aligned
	 * or the size requested isn't a multiple of PAGE_SIZE
	 */
	if (f_offset < 0 || f_offset >= filesize ||
	   (f_offset & PAGE_MASK_64) || (size & PAGE_MASK)) {
                if (local_flags & CL_COMMIT)
		        kernel_upl_abort_range(upl, upl_offset, size, UPL_ABORT_FREE_ON_EMPTY);
		return (EINVAL);
	}
	max_size = filesize - f_offset;

	if (size < max_size)
	        io_size = size;
	else
	        io_size = (max_size + (devblocksize - 1)) & ~(devblocksize - 1);

	pg_size = (io_size + (PAGE_SIZE - 1)) & ~PAGE_MASK;

	if (size > pg_size) {
	        if (local_flags & CL_COMMIT)
		        kernel_upl_abort_range(upl, upl_offset + pg_size, size - pg_size,
					UPL_ABORT_FREE_ON_EMPTY);
	}

	return (cluster_io(vp, upl, upl_offset, f_offset, io_size,
			   local_flags, (struct buf *)0));
}


cluster_pagein(vp, upl, upl_offset, f_offset, size, filesize, devblocksize, flags)
	struct vnode *vp;
	upl_t         upl;
	vm_offset_t   upl_offset;
	off_t         f_offset;
	int           size;
	off_t         filesize;
	int           devblocksize;
	int           flags;
{
	u_int         io_size;
	int           pg_size;
        off_t         max_size;
	int           retval;
	int           local_flags = 0;
	void         *object = 0;


	/*
	 * If they didn't ask for any data, then we are done...
	 * we can't issue an abort because we don't know how
	 * big the upl really is
	 */
	if (size <= 0)
	        return (EINVAL);

	if ((flags & UPL_NOCOMMIT) == 0) 
		local_flags = CL_COMMIT;

	KERNEL_DEBUG((FSDBG_CODE(DBG_FSRW, 56)) | DBG_FUNC_NONE,
		     (int)f_offset, size, (int)filesize, local_flags, 0);

	/*
	 * can't page-in from a negative offset
	 * or if we're starting beyond the EOF
	 * or if the file offset isn't page aligned
	 * or the size requested isn't a multiple of PAGE_SIZE
	 */
	if (f_offset < 0 || f_offset >= filesize ||
	   (f_offset & PAGE_MASK_64) || (size & PAGE_MASK)) {
	        if (local_flags & CL_COMMIT)
		        kernel_upl_abort_range(upl, upl_offset, size, UPL_ABORT_ERROR | UPL_ABORT_FREE_ON_EMPTY);
		return (EINVAL);
	}
	max_size = filesize - f_offset;

	if (size < max_size)
	        io_size = size;
	else
	        io_size = (max_size + (devblocksize - 1)) & ~(devblocksize - 1);

	pg_size = (io_size + (PAGE_SIZE - 1)) & ~PAGE_MASK;

	if (upl == (upl_t) 0) {
		object = ubc_getobject(vp, UBC_PAGINGOP|UBC_NOREACTIVATE);
		if (object == (void *)NULL)
			panic("cluster_pagein: ubc_getobject failed");

		vm_fault_list_request(object, (vm_offset_t)f_offset, pg_size, &upl, NULL, 0,
				      UPL_CLEAN_IN_PLACE | UPL_NO_SYNC | UPL_SET_INTERNAL);
		if (upl == (upl_t) 0)
		        return (EINVAL);

		upl_offset = (vm_offset_t)0;
		size = pg_size;
	}
	if (size > pg_size) {
	        if (local_flags & CL_COMMIT)
		        kernel_upl_abort_range(upl, upl_offset + pg_size, size - pg_size,
					UPL_ABORT_FREE_ON_EMPTY);
	}

	retval = cluster_io(vp, upl, upl_offset, f_offset, io_size,
			    local_flags | CL_READ | CL_PAGEIN, (struct buf *)0);

	if (retval == 0) {
	        int b_lblkno;
		int e_lblkno;

		b_lblkno = (int)(f_offset / PAGE_SIZE_64);
		e_lblkno = (int)
			((f_offset + ((off_t)io_size - 1)) / PAGE_SIZE_64);

		if (!(flags & UPL_NORDAHEAD) && !(vp->v_flag & VRAOFF)) {
		        if (object == (void *)0) {
			        object = ubc_getobject(vp, UBC_PAGINGOP|UBC_NOREACTIVATE);
					if (object == (void *)NULL)
				        panic("cluster_pagein: ubc_getobject failed");
			}
		        /*
			 * we haven't read the last page in of the file yet
			 * so let's try to read ahead if we're in 
			 * a sequential access pattern
			 */
		        cluster_rd_ahead(vp, object, b_lblkno, e_lblkno, filesize, devblocksize);
		}
	        vp->v_lastr = e_lblkno;
	}
	return (retval);
}


cluster_bp(bp)
	struct buf *bp;
{
        off_t  f_offset;
	int    flags;

	if (bp->b_pagelist == (upl_t) 0)
	        panic("cluster_bp: can't handle NULL upl yet\n");
	if (bp->b_flags & B_READ)
	        flags = CL_ASYNC | CL_NOMAP | CL_READ;
	else
	        flags = CL_ASYNC | CL_NOMAP;

	f_offset = ubc_blktooff(bp->b_vp, bp->b_lblkno);

        return (cluster_io(bp->b_vp, bp->b_pagelist, 0, f_offset, bp->b_bcount, flags, bp));
}


cluster_write(vp, uio, oldEOF, newEOF, headOff, tailOff, devblocksize, flags)
	struct vnode *vp;
	struct uio   *uio;
	off_t         oldEOF;
	off_t         newEOF;
	off_t         headOff;
	off_t         tailOff;
	int           devblocksize;
	int           flags;
{
	void          *object;
	int           prev_resid;
	int           clip_size;
	off_t         max_io_size;
	struct iovec  *iov;
	int           retval = 0;


	object = ubc_getobject(vp, UBC_NOREACTIVATE);
	if (object == (void *)NULL)
		panic("cluster_write: ubc_getobject failed");

	/*
	 * We set a threshhold of 4 pages to decide if the nocopy
	 * write loop is worth the trouble...
	 */

	if ((!uio) || (uio->uio_resid < 4 * PAGE_SIZE) ||
	    (flags & IO_TAILZEROFILL) || (flags & IO_HEADZEROFILL) ||
	    (uio->uio_segflg != UIO_USERSPACE) || (!(vp->v_flag & VNOCACHE_DATA)))
	  {
	    retval = cluster_write_x(object, vp, uio, oldEOF, newEOF, headOff, tailOff, devblocksize, flags);
	    return(retval);
	  }
	
	while (uio->uio_resid && uio->uio_offset < newEOF && retval == 0)
	  {
	    /* we know we have a resid, so this is safe */
	    iov = uio->uio_iov;
	    while (iov->iov_len == 0) {
	      uio->uio_iov++;
	      uio->uio_iovcnt--;
	      iov = uio->uio_iov;
	    }

	    if (uio->uio_offset & PAGE_MASK_64)
	      {
		/* Bring the file offset write up to a pagesize boundary */
		clip_size = (PAGE_SIZE - (uio->uio_offset & PAGE_MASK_64));
		if (uio->uio_resid < clip_size)
		  clip_size = uio->uio_resid;
		/* 
		 * Fake the resid going into the cluster_write_x call
		 * and restore it on the way out.
		 */
		prev_resid = uio->uio_resid;
		uio->uio_resid = clip_size;
		retval = cluster_write_x(object, vp, uio, oldEOF, newEOF, headOff, tailOff, devblocksize, flags);
		uio->uio_resid = prev_resid - (clip_size - uio->uio_resid);
	      }
	    else if ((int)iov->iov_base & PAGE_MASK_64)
	      {
		clip_size = iov->iov_len;
		prev_resid = uio->uio_resid;
		uio->uio_resid = clip_size;
		retval = cluster_write_x(object, vp, uio, oldEOF, newEOF, headOff, tailOff, devblocksize, flags);
		uio->uio_resid = prev_resid - (clip_size - uio->uio_resid);
	      }
	    else
	      {
		/* 
		 * If we come in here, we know the offset into
		 * the file is on a pagesize boundary
		 */

		max_io_size = newEOF - uio->uio_offset;
		clip_size = uio->uio_resid;
		if (iov->iov_len < clip_size)
		  clip_size = iov->iov_len;
		if (max_io_size < clip_size)
		  clip_size = max_io_size;

		if (clip_size < PAGE_SIZE)
		  {
		    /*
		     * Take care of tail end of write in this vector
		     */
		    prev_resid = uio->uio_resid;
		    uio->uio_resid = clip_size;
		    retval = cluster_write_x(object, vp, uio, oldEOF, newEOF, headOff, tailOff, devblocksize, flags);
		    uio->uio_resid = prev_resid - (clip_size - uio->uio_resid);
		  }
		else
		  {
		    /* round clip_size down to a multiple of pagesize */
		    clip_size = clip_size & ~(PAGE_MASK);
		    prev_resid = uio->uio_resid;
		    uio->uio_resid = clip_size;
		    retval = cluster_nocopy_write(object, vp, uio, newEOF, devblocksize, flags);
		    if ((retval == 0) && uio->uio_resid)
		      retval = cluster_write_x(object, vp, uio, oldEOF, newEOF, headOff, tailOff, devblocksize, flags);
		    uio->uio_resid = prev_resid - (clip_size - uio->uio_resid);
		  }
	      } /* end else */
	  } /* end while */
	return(retval);
}

static
cluster_nocopy_write(object, vp, uio, newEOF, devblocksize, flags)
        void         *object;
	struct vnode *vp;
	struct uio   *uio;
	off_t         newEOF;
	int           devblocksize;
	int           flags;
{
	upl_t            upl;
	upl_page_info_t  *pl;
	off_t 	         upl_f_offset;
	vm_offset_t      upl_offset;
	off_t            max_io_size;
	int              io_size;
	int              upl_size;
	int              upl_needed_size;
	int              pages_in_pl;
	int              upl_flags;
	kern_return_t    kret;
	struct iovec     *iov;
	int              i;
	int              force_data_sync;
	int              error  = 0;

	KERNEL_DEBUG((FSDBG_CODE(DBG_FSRW, 75)) | DBG_FUNC_START,
		     (int)uio->uio_offset, (int)uio->uio_resid, 
		     (int)newEOF, devblocksize, 0);

	/*
	 * When we enter this routine, we know
	 *  -- the offset into the file is on a pagesize boundary
	 *  -- the resid is a page multiple
	 *  -- the resid will not exceed iov_len
	 */

	iov = uio->uio_iov;
		
	while (uio->uio_resid && uio->uio_offset < newEOF && error == 0) {

	  io_size = uio->uio_resid;
	  if (io_size > MAXPHYSIO)
	    io_size = MAXPHYSIO;

	  upl_offset = (vm_offset_t)iov->iov_base & PAGE_MASK_64;
	  upl_needed_size = (upl_offset + io_size + (PAGE_SIZE -1)) & ~PAGE_MASK;

	  KERNEL_DEBUG((FSDBG_CODE(DBG_FSRW, 76)) | DBG_FUNC_START,
		       (int)upl_offset, upl_needed_size, iov->iov_base, io_size, 0);

	  for (force_data_sync = 0; force_data_sync < 3; force_data_sync++)
	    {
	      pages_in_pl = 0;
	      upl_size = upl_needed_size;
	      upl_flags = UPL_COPYOUT_FROM | UPL_NO_SYNC | UPL_CLEAN_IN_PLACE | UPL_SET_INTERNAL;

	      kret = vm_map_get_upl(current_map(),
				    (vm_offset_t)iov->iov_base & ~PAGE_MASK,
				    &upl_size, &upl, &pl, &pages_in_pl, &upl_flags, force_data_sync);

	      pages_in_pl = upl_size / PAGE_SIZE;

	      if (kret != KERN_SUCCESS)
		{
		  KERNEL_DEBUG((FSDBG_CODE(DBG_FSRW, 76)) | DBG_FUNC_END,
			       0, 0, 0, kret, 0);

		  KERNEL_DEBUG((FSDBG_CODE(DBG_FSRW, 75)) | DBG_FUNC_END,
			       (int)uio->uio_offset, (int)uio->uio_resid, kret, 1, 0);

		  /* cluster_nocopy_write: failed to get pagelist */
		  /* do not return kret here */
		  return(0);
		}
	      
	      for(i=0; i < pages_in_pl; i++)
		{
		  if (!upl_valid_page(pl, i))
		    break;		  
		}
	      
	      if (i == pages_in_pl)
		break;

	      kernel_upl_abort_range(upl, (upl_offset & ~PAGE_MASK), upl_size, 
				     UPL_ABORT_FREE_ON_EMPTY);
	    }

	  if (force_data_sync >= 3)
	    {
	      KERNEL_DEBUG((FSDBG_CODE(DBG_FSRW, 76)) | DBG_FUNC_END,
			   0, 0, 0, kret, 0);

	      KERNEL_DEBUG((FSDBG_CODE(DBG_FSRW, 75)) | DBG_FUNC_END,
			   (int)uio->uio_offset, (int)uio->uio_resid, kret, 2, 0);
	      return(0);
	    }

	  /*
	   * Consider the possibility that upl_size wasn't satisfied.
	   */
	  if (upl_size != upl_needed_size)
	    io_size = (upl_size - (int)upl_offset) & ~PAGE_MASK;

	  KERNEL_DEBUG((FSDBG_CODE(DBG_FSRW, 76)) | DBG_FUNC_END,
		       (int)upl_offset, upl_size, iov->iov_base, io_size, 0);		       

	  if (io_size == 0)
	    {
	      kernel_upl_abort_range(upl, (upl_offset & ~PAGE_MASK), upl_size, 
				   UPL_ABORT_FREE_ON_EMPTY);
	      KERNEL_DEBUG((FSDBG_CODE(DBG_FSRW, 75)) | DBG_FUNC_END,
		     (int)uio->uio_offset, uio->uio_resid, 0, 3, 0);

	      return(0);
	    }

	  /*
	   * Now look for pages already in the cache
	   * and throw them away.
	   */

	  upl_f_offset = uio->uio_offset;   /* this is page aligned in the file */
	  max_io_size = io_size;

	  while (max_io_size) {

	    /*
	     * Flag UPL_POP_DUMP says if the page is found
	     * in the page cache it must be thrown away.
	     */
	    memory_object_page_op(object, (vm_offset_t)upl_f_offset,
				  UPL_POP_SET | UPL_POP_BUSY | UPL_POP_DUMP,
				  0, 0);
	    max_io_size  -= PAGE_SIZE;
	    upl_f_offset += PAGE_SIZE;
	  }

	  /*
	   * issue a synchronous write to cluster_io
	   */

	  KERNEL_DEBUG((FSDBG_CODE(DBG_FSRW, 77)) | DBG_FUNC_START,
		       (int)upl_offset, (int)uio->uio_offset, io_size, 0, 0);

	  error = cluster_io(vp, upl, upl_offset, uio->uio_offset,
			     io_size, 0, (struct buf *)0);

	  if (error == 0) {
	    /*
	     * The cluster_io write completed successfully,
	     * update the uio structure and commit.
	     */

	    kernel_upl_commit_range(upl, (upl_offset & ~PAGE_MASK), upl_size, 
			    UPL_COMMIT_SET_DIRTY | UPL_COMMIT_FREE_ON_EMPTY, 
			    pl, MAX_UPL_TRANSFER);
	    
	    iov->iov_base += io_size;
	    iov->iov_len -= io_size;
	    uio->uio_resid -= io_size;
	    uio->uio_offset += io_size;
	  }
	  else {
	    kernel_upl_abort_range(upl, (upl_offset & ~PAGE_MASK), upl_size, 
				   UPL_ABORT_FREE_ON_EMPTY);
	  }

	  KERNEL_DEBUG((FSDBG_CODE(DBG_FSRW, 77)) | DBG_FUNC_END,
		       (int)upl_offset, (int)uio->uio_offset, (int)uio->uio_resid, error, 0);

	} /* end while */


	KERNEL_DEBUG((FSDBG_CODE(DBG_FSRW, 75)) | DBG_FUNC_END,
		     (int)uio->uio_offset, (int)uio->uio_resid, error, 4, 0);

	return (error);
}

static
cluster_write_x(object, vp, uio, oldEOF, newEOF, headOff, tailOff, devblocksize, flags)
	void         *object;
	struct vnode *vp;
	struct uio   *uio;
	off_t         oldEOF;
	off_t         newEOF;
	off_t         headOff;
	off_t         tailOff;
	int           devblocksize;
	int           flags;
{
	upl_page_info_t *pl;
	upl_t            upl;
	vm_offset_t      upl_offset;
	int              upl_size;
	off_t 	         upl_f_offset;
	int              pages_in_upl;
	int		 start_offset;
	int              xfer_resid;
	int              io_size;
	int              io_size_before_rounding;
	int              io_flags;
	vm_offset_t      io_address;
	int              io_offset;
	int              bytes_to_zero;
	int              bytes_to_move;
	kern_return_t    kret;
	int              retval = 0;
	int              uio_resid;
	long long        total_size;
	long long        zero_cnt;
	off_t            zero_off;
	long long        zero_cnt1;
	off_t            zero_off1;
	daddr_t          start_blkno;
	daddr_t          last_blkno;

	if (uio) {
	        KERNEL_DEBUG((FSDBG_CODE(DBG_FSRW, 40)) | DBG_FUNC_START,
			     (int)uio->uio_offset, uio->uio_resid, (int)oldEOF, (int)newEOF, 0);

	        uio_resid = uio->uio_resid;
	} else {
	        KERNEL_DEBUG((FSDBG_CODE(DBG_FSRW, 40)) | DBG_FUNC_START,
			     0, 0, (int)oldEOF, (int)newEOF, 0);

	        uio_resid = 0;
	}
	zero_cnt  = 0;
	zero_cnt1 = 0;

	if (flags & IO_HEADZEROFILL) {
	        /*
		 * some filesystems (HFS is one) don't support unallocated holes within a file...
		 * so we zero fill the intervening space between the old EOF and the offset
		 * where the next chunk of real data begins.... ftruncate will also use this
		 * routine to zero fill to the new EOF when growing a file... in this case, the
		 * uio structure will not be provided
		 */
	        if (uio) {
		        if (headOff < uio->uio_offset) {
			        zero_cnt = uio->uio_offset - headOff;
				zero_off = headOff;
			}
		} else if (headOff < newEOF) {	
		        zero_cnt = newEOF - headOff;
			zero_off = headOff;
		}
	}
	if (flags & IO_TAILZEROFILL) {
	        if (uio) {
		        zero_off1 = uio->uio_offset + uio->uio_resid;

			if (zero_off1 < tailOff)
			        zero_cnt1 = tailOff - zero_off1;
		}	
	}
	if (zero_cnt == 0 && uio == (struct uio *) 0)
	  {
	    KERNEL_DEBUG((FSDBG_CODE(DBG_FSRW, 40)) | DBG_FUNC_END,
			 retval, 0, 0, 0, 0);
	    return (0);
	  }

	while ((total_size = (uio_resid + zero_cnt + zero_cnt1)) && retval == 0) {
	        /*
		 * for this iteration of the loop, figure out where our starting point is
		 */
	        if (zero_cnt) {
		        start_offset = (int)(zero_off & PAGE_MASK_64);
			upl_f_offset = zero_off - start_offset;
		} else if (uio_resid) {
		        start_offset = (int)(uio->uio_offset & PAGE_MASK_64);
			upl_f_offset = uio->uio_offset - start_offset;
		} else {
		        start_offset = (int)(zero_off1 & PAGE_MASK_64);
			upl_f_offset = zero_off1 - start_offset;
		}
	        KERNEL_DEBUG((FSDBG_CODE(DBG_FSRW, 46)) | DBG_FUNC_NONE,
			     (int)zero_off, (int)zero_cnt, (int)zero_off1, (int)zero_cnt1, 0);

	        if (total_size > (long long)MAXPHYSIO)
			total_size = MAXPHYSIO;

		/*
		 * compute the size of the upl needed to encompass
		 * the requested write... limit each call to cluster_io
		 * to at most MAXPHYSIO, make sure to account for 
		 * a starting offset that's not page aligned
		 */
		upl_size = (start_offset + total_size + (PAGE_SIZE - 1)) & ~PAGE_MASK;

	        if (upl_size > MAXPHYSIO)
			upl_size = MAXPHYSIO;

		pages_in_upl = upl_size / PAGE_SIZE;
		io_size      = upl_size - start_offset;
		
		if ((long long)io_size > total_size)
		        io_size = total_size;

		start_blkno = (daddr_t)(upl_f_offset / PAGE_SIZE_64);
		last_blkno  = start_blkno + pages_in_upl;

		kret = vm_fault_list_request(object, 
				(vm_object_offset_t)upl_f_offset, upl_size, &upl, NULL, 0,  
				(UPL_NO_SYNC | UPL_CLEAN_IN_PLACE | UPL_SET_INTERNAL));

		if (kret != KERN_SUCCESS)
			panic("cluster_write: failed to get pagelist");

		pl = UPL_GET_INTERNAL_PAGE_LIST(upl);

	        KERNEL_DEBUG((FSDBG_CODE(DBG_FSRW, 41)) | DBG_FUNC_NONE,
			     upl, (int)upl_f_offset, upl_size, start_offset, 0);


		if (start_offset && !upl_valid_page(pl, 0)) {
		        int   read_size;

		        /*
			 * we're starting in the middle of the first page of the upl
			 * and the page isn't currently valid, so we're going to have
			 * to read it in first... this is a synchronous operation
			 */
			read_size = PAGE_SIZE;

			if ((upl_f_offset + read_size) > newEOF) {
			        read_size = newEOF - upl_f_offset;
				read_size = (read_size + (devblocksize - 1)) & ~(devblocksize - 1);
			}
		        retval = cluster_io(vp, upl, 0, upl_f_offset, read_size,
					    CL_READ, (struct buf *)0);
			if (retval) {
			        /*
				 * we had an error during the read which causes us to abort
				 * the current cluster_write request... before we do, we need
				 * to release the rest of the pages in the upl without modifying
				 * there state and mark the failed page in error
				 */
			        kernel_upl_abort_range(upl, 0, PAGE_SIZE, UPL_ABORT_DUMP_PAGES);
				kernel_upl_abort(upl, 0);

				KERNEL_DEBUG((FSDBG_CODE(DBG_FSRW, 45)) | DBG_FUNC_NONE,
					     upl, 0, 0, retval, 0);
				break;
			}
		}
		if ((start_offset == 0 || upl_size > PAGE_SIZE) && ((start_offset + io_size) & PAGE_MASK)) {
		        /* 
			 * the last offset we're writing to in this upl does not end on a page
			 * boundary... if it's not beyond the old EOF, then we'll also need to
			 * pre-read this page in if it isn't already valid
			 */
		        upl_offset = upl_size - PAGE_SIZE;

		        if ((upl_f_offset + start_offset + io_size) < oldEOF &&
			    !upl_valid_page(pl, upl_offset / PAGE_SIZE)) {
			        int   read_size;

				read_size = PAGE_SIZE;

				if ((upl_f_offset + upl_offset + read_size) > newEOF) {
				        read_size = newEOF - (upl_f_offset + upl_offset);
					read_size = (read_size + (devblocksize - 1)) & ~(devblocksize - 1);
				}
			        retval = cluster_io(vp, upl, upl_offset, upl_f_offset + upl_offset, read_size,
						    CL_READ, (struct buf *)0);
				if (retval) {
				        /*
					 * we had an error during the read which causes us to abort
					 * the current cluster_write request... before we do, we need
					 * to release the rest of the pages in the upl without modifying
					 * there state and mark the failed page in error
					 */
				        kernel_upl_abort_range(upl, upl_offset, PAGE_SIZE, UPL_ABORT_DUMP_PAGES);
					kernel_upl_abort(upl, 0);

					KERNEL_DEBUG((FSDBG_CODE(DBG_FSRW, 45)) | DBG_FUNC_NONE,
						     upl, 0, 0, retval, 0);
					break;
				}
			}
		}
		if ((kret = kernel_upl_map(kernel_map, upl, &io_address)) != KERN_SUCCESS)
		        panic("cluster_write: kernel_upl_map failed\n");
		xfer_resid = io_size;
		io_offset = start_offset;

		while (zero_cnt && xfer_resid) {

		        if (zero_cnt < (long long)xfer_resid)
			        bytes_to_zero = zero_cnt;
			else
			        bytes_to_zero = xfer_resid;

		        if ( !(flags & IO_NOZEROVALID)) {
				bzero((caddr_t)(io_address + io_offset), bytes_to_zero);

				KERNEL_DEBUG((FSDBG_CODE(DBG_FSRW, 43)) | DBG_FUNC_NONE,
					     (int)upl_f_offset + io_offset, bytes_to_zero,
					     (int)zero_cnt, xfer_resid, 0);
			} else {
			        bytes_to_zero = min(bytes_to_zero, PAGE_SIZE - (int)(zero_off & PAGE_MASK_64));

				if ( !upl_valid_page(pl, (int)(zero_off / PAGE_SIZE_64))) {
				        bzero((caddr_t)(io_address + io_offset), bytes_to_zero); 

					KERNEL_DEBUG((FSDBG_CODE(DBG_FSRW, 43)) | DBG_FUNC_NONE,
						     (int)upl_f_offset + io_offset, bytes_to_zero,
						     (int)zero_cnt, xfer_resid, 0);
				}
			}
			xfer_resid -= bytes_to_zero;
			zero_cnt   -= bytes_to_zero;
			zero_off   += bytes_to_zero;
			io_offset  += bytes_to_zero;
		}
		if (xfer_resid && uio_resid) {
			bytes_to_move = min(uio_resid, xfer_resid);

			KERNEL_DEBUG((FSDBG_CODE(DBG_FSRW, 42)) | DBG_FUNC_NONE,
				     (int)uio->uio_offset, bytes_to_move, uio_resid, xfer_resid, 0);

			retval = uiomove((caddr_t)(io_address + io_offset), bytes_to_move, uio);

			if (retval) {
			        if ((kret = kernel_upl_unmap(kernel_map, upl)) != KERN_SUCCESS)
				        panic("cluster_write: kernel_upl_unmap failed\n");
			        kernel_upl_abort(upl, UPL_ABORT_DUMP_PAGES);

				KERNEL_DEBUG((FSDBG_CODE(DBG_FSRW, 45)) | DBG_FUNC_NONE,
					     upl, 0, 0, retval, 0);
			} else {
			        uio_resid  -= bytes_to_move;
				xfer_resid -= bytes_to_move;
				io_offset  += bytes_to_move;
			}
		}
		while (xfer_resid && zero_cnt1 && retval == 0) {

		        if (zero_cnt1 < (long long)xfer_resid)
			        bytes_to_zero = zero_cnt1;
			else
			        bytes_to_zero = xfer_resid;

		        if ( !(flags & IO_NOZEROVALID)) {
			        bzero((caddr_t)(io_address + io_offset), bytes_to_zero);

				KERNEL_DEBUG((FSDBG_CODE(DBG_FSRW, 43)) | DBG_FUNC_NONE,
					     (int)upl_f_offset + io_offset,
					     bytes_to_zero, (int)zero_cnt1, xfer_resid, 0);
			} else {
			        bytes_to_zero = min(bytes_to_zero, PAGE_SIZE - (int)(zero_off1 & PAGE_MASK_64));
				if ( !upl_valid_page(pl, (int)(zero_off1 / PAGE_SIZE_64))) {
				        bzero((caddr_t)(io_address + io_offset), bytes_to_zero);

					KERNEL_DEBUG((FSDBG_CODE(DBG_FSRW, 43)) | DBG_FUNC_NONE,
						     (int)upl_f_offset + io_offset,
						     bytes_to_zero, (int)zero_cnt1, xfer_resid, 0);
				}
			}
			xfer_resid -= bytes_to_zero;
			zero_cnt1  -= bytes_to_zero;
			zero_off1  += bytes_to_zero;
			io_offset  += bytes_to_zero;
		}

		if (retval == 0) {
		        int must_push;
			int can_delay;

		        io_size += start_offset;

			if ((upl_f_offset + io_size) == newEOF && io_size < upl_size) {
			        /*
				 * if we're extending the file with this write
				 * we'll zero fill the rest of the page so that
				 * if the file gets extended again in such a way as to leave a
				 * hole starting at this EOF, we'll have zero's in the correct spot
				 */
			        bzero((caddr_t)(io_address + io_size), upl_size - io_size);

				KERNEL_DEBUG((FSDBG_CODE(DBG_FSRW, 43)) | DBG_FUNC_NONE,
					     (int)upl_f_offset + io_size,
					     upl_size - io_size, 0, 0, 0);
			}
		        if ((kret = kernel_upl_unmap(kernel_map, upl)) != KERN_SUCCESS)
			        panic("cluster_write: kernel_upl_unmap failed\n");

			io_size_before_rounding = io_size;

			if (io_size & (devblocksize - 1))
			        io_size = (io_size + (devblocksize - 1)) & ~(devblocksize - 1);

			must_push = 0;
			can_delay = 0;

			if (vp->v_clen) {
			        int newsize;

			        /*
				 * we have an existing cluster... see if this write will extend it nicely
				 */
			        if (start_blkno >= vp->v_cstart) {
				        if (last_blkno <= (vp->v_cstart + vp->v_clen)) {
					        /*
						 * we have a write that fits entirely
						 * within the existing cluster limits
						 */
					        if (last_blkno >= vp->v_lastw) {
						        /*
							 * if we're extending the dirty region within the cluster
							 * we need to update the cluster info... we check for blkno
							 * equality because we may be extending the file with a 
							 * partial write.... this in turn changes our idea of how
							 * much data to write out (v_ciosiz) for the last page
							 */
						        vp->v_lastw = last_blkno;
							newsize = io_size + ((start_blkno - vp->v_cstart) * PAGE_SIZE);

							if (newsize > vp->v_ciosiz)
							        vp->v_ciosiz = newsize;
						}
						can_delay = 1;
						goto finish_io;
					}
					if (start_blkno < (vp->v_cstart + vp->v_clen)) {
					        /*
						 * we have a write that starts in the middle of the current cluster
						 * but extends beyond the cluster's limit
						 * we'll clip the current cluster if we actually
						 * overlap with the new write and then push it out
						 * and start a new cluster with the current write
						 */
						 if (vp->v_lastw > start_blkno) {
						        vp->v_lastw = start_blkno;
							vp->v_ciosiz = (vp->v_lastw - vp->v_cstart) * PAGE_SIZE;
						 }
					}
					/*
					 * we also get here for the case where the current write starts
					 * beyond the limit of the existing cluster
					 */
					must_push = 1;
					goto check_delay;
				}
				/*
				 * the current write starts in front of the current cluster
				 */
				if (last_blkno > vp->v_cstart) {
				        /*
					 * the current write extends into the existing cluster
					 */
				        if ((vp->v_lastw - start_blkno) > vp->v_clen) {
					        /*
						 * if we were to combine this write with the current cluster
						 * we would exceed the cluster size limit....
						 * clip the current cluster by moving the start position
						 * to where the current write ends, and then push it
						 */
					        vp->v_ciosiz -= (last_blkno - vp->v_cstart) * PAGE_SIZE;
					        vp->v_cstart = last_blkno;

						/*
						 * round up the io_size to the nearest page size
						 * since we've coalesced with at least 1 pre-existing
						 * page in the current cluster... this write may have ended in the
						 * middle of the page which would cause io_size to give us an
						 * inaccurate view of how much I/O we actually need to do
						 */
						io_size = (io_size + (PAGE_SIZE - 1)) & ~PAGE_MASK;

						must_push = 1;
						goto check_delay;
					}
					/*
					 * we can coalesce the current write with the existing cluster
					 * adjust the cluster info to reflect this
					 */
					if (last_blkno > vp->v_lastw) {
					        /*
						 * the current write completey overlaps
						 * the existing cluster
						 */
					        vp->v_lastw = last_blkno;
						vp->v_ciosiz = io_size;
					} else {
					        vp->v_ciosiz += (vp->v_cstart - start_blkno) * PAGE_SIZE;

						if (io_size > vp->v_ciosiz)
						        vp->v_ciosiz = io_size;
					}
					vp->v_cstart = start_blkno;
					can_delay = 1;
					goto finish_io;
				}
				/*
				 * this I/O range is entirely in front of the current cluster
				 * so we need to push the current cluster out before beginning
				 * a new one
				 */
				must_push = 1;
			}
check_delay:
			if (must_push)
			        cluster_push(vp);

			if (io_size_before_rounding < MAXPHYSIO && !(flags & IO_SYNC)) {
			        vp->v_clen = MAXPHYSIO / PAGE_SIZE;
				vp->v_cstart = start_blkno;
				vp->v_lastw  = last_blkno;
				vp->v_ciosiz = io_size;
				
			        can_delay = 1;
			}
finish_io:
			if (can_delay) {
			        kernel_upl_commit_range(upl, 0, upl_size,
					 UPL_COMMIT_SET_DIRTY 
						| UPL_COMMIT_FREE_ON_EMPTY, 
					 pl, MAX_UPL_TRANSFER);
				continue;
			}
				        
			if ((flags & IO_SYNC) || (vp->v_numoutput > ASYNC_THROTTLE))
			        io_flags = CL_COMMIT | CL_AGE;
			else
			        io_flags = CL_COMMIT | CL_AGE | CL_ASYNC;

			if (vp->v_flag & VNOCACHE_DATA)
			        io_flags |= CL_DUMP;

			retval = cluster_io(vp, upl, 0, upl_f_offset, io_size,
					    io_flags, (struct buf *)0);
		}
	}
	KERNEL_DEBUG((FSDBG_CODE(DBG_FSRW, 40)) | DBG_FUNC_END,
		     retval, 0, 0, 0, 0);

	return (retval);
}

cluster_read(vp, uio, filesize, devblocksize, flags)
	struct vnode *vp;
	struct uio   *uio;
	off_t         filesize;
	int           devblocksize;
	int           flags;
{
	void          *object;
	int           prev_resid;
	int           clip_size;
	off_t         max_io_size;
	struct iovec  *iov;
	int           retval = 0;

	object = ubc_getobject(vp, UBC_NOREACTIVATE);
	if (object == (void *)NULL)
		panic("cluster_read: ubc_getobject failed");

	KERNEL_DEBUG((FSDBG_CODE(DBG_FSRW, 32)) | DBG_FUNC_START,
		     (int)uio->uio_offset, uio->uio_resid, (int)filesize, devblocksize, 0);

	/*
	 * We set a threshhold of 4 pages to decide if the nocopy
	 * read loop is worth the trouble...
	 */

	if ((!((vp->v_flag & VNOCACHE_DATA) && (uio->uio_segflg == UIO_USERSPACE)))
	    || (uio->uio_resid < 4 * PAGE_SIZE))
	  {
	    retval = cluster_read_x(object, vp, uio, filesize, devblocksize, flags);
	    KERNEL_DEBUG((FSDBG_CODE(DBG_FSRW, 32)) | DBG_FUNC_END,
			 (int)uio->uio_offset, uio->uio_resid, vp->v_lastr, retval, 0);
	    return(retval);

	  }

	while (uio->uio_resid && uio->uio_offset < filesize && retval == 0)
	  {
	    /* we know we have a resid, so this is safe */
	    iov = uio->uio_iov;
	    while (iov->iov_len == 0) {
	      uio->uio_iov++;
	      uio->uio_iovcnt--;
	      iov = uio->uio_iov;
	    }

	    if (uio->uio_offset & PAGE_MASK_64)
	      {
		/* Bring the file offset read up to a pagesize boundary */
		clip_size = (PAGE_SIZE - (int)(uio->uio_offset & PAGE_MASK_64));
		if (uio->uio_resid < clip_size)
		  clip_size = uio->uio_resid;
		/* 
		 * Fake the resid going into the cluster_read_x call
		 * and restore it on the way out.
		 */
		prev_resid = uio->uio_resid;
		uio->uio_resid = clip_size;
		retval = cluster_read_x(object, vp, uio, filesize, devblocksize, flags);
		uio->uio_resid = prev_resid - (clip_size - uio->uio_resid);
	      }
	    else if ((int)iov->iov_base & PAGE_MASK_64)
	      {
		clip_size = iov->iov_len;
		prev_resid = uio->uio_resid;
		uio->uio_resid = clip_size;
		retval = cluster_read_x(object, vp, uio, filesize, devblocksize, flags);
		uio->uio_resid = prev_resid - (clip_size - uio->uio_resid);
	      }
	    else
	      {
		/* 
		 * If we come in here, we know the offset into
		 * the file is on a pagesize boundary
		 */

		max_io_size = filesize - uio->uio_offset;
		clip_size = uio->uio_resid;
		if (iov->iov_len < clip_size)
		  clip_size = iov->iov_len;
		if (max_io_size < clip_size)
		  clip_size = (int)max_io_size;

		if (clip_size < PAGE_SIZE)
		  {
		    /*
		     * Take care of the tail end of the read in this vector.
		     */
		    prev_resid = uio->uio_resid;
		    uio->uio_resid = clip_size;
		    retval = cluster_read_x(object,vp, uio, filesize, devblocksize, flags);
		    uio->uio_resid = prev_resid - (clip_size - uio->uio_resid);
		  }
		else
		  {
		    /* round clip_size down to a multiple of pagesize */
		    clip_size = clip_size & ~(PAGE_MASK);
		    prev_resid = uio->uio_resid;
		    uio->uio_resid = clip_size;
		    retval = cluster_nocopy_read(object, vp, uio, filesize, devblocksize, flags);
		    if ((retval==0) && uio->uio_resid)
		      retval = cluster_read_x(object,vp, uio, filesize, devblocksize, flags);
		    uio->uio_resid = prev_resid - (clip_size - uio->uio_resid);
		  }
	      } /* end else */
	  } /* end while */

	KERNEL_DEBUG((FSDBG_CODE(DBG_FSRW, 32)) | DBG_FUNC_END,
		     (int)uio->uio_offset, uio->uio_resid, vp->v_lastr, retval, 0);

	return(retval);
}

static
cluster_read_x(object, vp, uio, filesize, devblocksize, flags)
	void         *object;
	struct vnode *vp;
	struct uio   *uio;
	off_t         filesize;
	int           devblocksize;
	int           flags;
{
	upl_page_info_t *pl;
	upl_t            upl;
	vm_offset_t      upl_offset;
	int              upl_size;
	off_t 	         upl_f_offset;
	int		 start_offset;
	int	         start_pg;
	int		 last_pg;
	int              uio_last;
	int              pages_in_upl;
	off_t            max_size;
	int              io_size;
	vm_offset_t      io_address;
	kern_return_t    kret;
	int              segflg;
	int              error  = 0;
	int              retval = 0;
	int              b_lblkno;
	int              e_lblkno;

	b_lblkno = (int)(uio->uio_offset / PAGE_SIZE_64);

	while (uio->uio_resid && uio->uio_offset < filesize && retval == 0) {
		/*
		 * compute the size of the upl needed to encompass
		 * the requested read... limit each call to cluster_io
		 * to at most MAXPHYSIO, make sure to account for 
		 * a starting offset that's not page aligned
		 */
		start_offset = (int)(uio->uio_offset & PAGE_MASK_64);
		upl_f_offset = uio->uio_offset - (off_t)start_offset;
		max_size     = filesize - uio->uio_offset;

		if (uio->uio_resid < max_size)
		        io_size = uio->uio_resid;
		else
		        io_size = max_size;
#ifdef ppc
		if (uio->uio_segflg == UIO_USERSPACE && !(vp->v_flag & VNOCACHE_DATA)) {
		        segflg = uio->uio_segflg;

			uio->uio_segflg = UIO_PHYS_USERSPACE;

			KERNEL_DEBUG((FSDBG_CODE(DBG_FSRW, 34)) | DBG_FUNC_START,
				     (int)uio->uio_offset, io_size, uio->uio_resid, 0, 0);

			while (io_size && retval == 0) {
			        int         xsize;
				vm_offset_t paddr;

				if (memory_object_page_op(object, (vm_offset_t)upl_f_offset, UPL_POP_SET | UPL_POP_BUSY,
							  &paddr, 0) != KERN_SUCCESS)
				        break;

				xsize = PAGE_SIZE - start_offset;
 			
				if (xsize > io_size)
				        xsize = io_size;

				retval = uiomove((caddr_t)(paddr + start_offset), xsize, uio);

				memory_object_page_op(object, (vm_offset_t)upl_f_offset, UPL_POP_CLR | UPL_POP_BUSY, 0, 0);

				io_size     -= xsize;
				start_offset = (int)
					(uio->uio_offset & PAGE_MASK_64);
				upl_f_offset = uio->uio_offset - start_offset;
			}
			KERNEL_DEBUG((FSDBG_CODE(DBG_FSRW, 34)) | DBG_FUNC_END,
				     (int)uio->uio_offset, io_size, uio->uio_resid, 0, 0);

			uio->uio_segflg = segflg;
			
			if (retval)
			        break;

			if (io_size == 0) {
			        /*
				 * we're already finished with this read request
				 * let's see if we should do a read-ahead
				 */
			        e_lblkno = (int)
					((uio->uio_offset - 1) / PAGE_SIZE_64);

			        if (!(vp->v_flag & VRAOFF))
				        /*
					 * let's try to read ahead if we're in 
					 * a sequential access pattern
					 */
				        cluster_rd_ahead(vp, object, b_lblkno, e_lblkno, filesize, devblocksize);
				vp->v_lastr = e_lblkno;

			        break;
			}
			max_size = filesize - uio->uio_offset;
		}
#endif
		upl_size = (start_offset + io_size + (PAGE_SIZE - 1)) & ~PAGE_MASK;
	        if (upl_size > MAXPHYSIO)
			upl_size = MAXPHYSIO;
		pages_in_upl = upl_size / PAGE_SIZE;

		KERNEL_DEBUG((FSDBG_CODE(DBG_FSRW, 33)) | DBG_FUNC_START,
			     upl, (int)upl_f_offset, upl_size, start_offset, 0);

		kret = vm_fault_list_request(object, 
				(vm_object_offset_t)upl_f_offset, upl_size, &upl, NULL, 0,  
				(UPL_NO_SYNC | UPL_CLEAN_IN_PLACE | UPL_SET_INTERNAL));

		if (kret != KERN_SUCCESS)
			panic("cluster_read: failed to get pagelist");

		pl = UPL_GET_INTERNAL_PAGE_LIST(upl);


		KERNEL_DEBUG((FSDBG_CODE(DBG_FSRW, 33)) | DBG_FUNC_END,
			     upl, (int)upl_f_offset, upl_size, start_offset, 0);

		/*
		 * scan from the beginning of the upl looking for the first
		 * non-valid page.... this will become the first page in
		 * the request we're going to make to 'cluster_io'... if all
		 * of the pages are valid, we won't call through to 'cluster_io'
		 */
		for (start_pg = 0; start_pg < pages_in_upl; start_pg++) {
			if (!upl_valid_page(pl, start_pg))
				break;
		}

		/*
		 * scan from the starting invalid page looking for a valid
		 * page before the end of the upl is reached, if we 
		 * find one, then it will be the last page of the request to
		 * 'cluster_io'
		 */
		for (last_pg = start_pg; last_pg < pages_in_upl; last_pg++) {
			if (upl_valid_page(pl, last_pg))
				break;
		}

		if (start_pg < last_pg) {		
		        /*
			 * we found a range of 'invalid' pages that must be filled
			 * if the last page in this range is the last page of the file
			 * we may have to clip the size of it to keep from reading past
			 * the end of the last physical block associated with the file
			 */
			upl_offset = start_pg * PAGE_SIZE;
			io_size    = (last_pg - start_pg) * PAGE_SIZE;

			if ((upl_f_offset + upl_offset + io_size) > filesize) {
			        io_size = filesize - (upl_f_offset + upl_offset);
				io_size = (io_size + (devblocksize - 1)) & ~(devblocksize - 1);
			}
			/*
			 * issue a synchronous read to cluster_io
			 */

			error = cluster_io(vp, upl, upl_offset, upl_f_offset + upl_offset,
					   io_size, CL_READ, (struct buf *)0);
		}
		if (error == 0) {
		        /*
			 * if the read completed successfully, or there was no I/O request
			 * issued, than map the upl into kernel address space and
			 * move the data into user land.... we'll first add on any 'valid'
			 * pages that were present in the upl when we acquired it.
			 */
			u_int  val_size;
			u_int  size_of_prefetch;

		        for (uio_last = last_pg; uio_last < pages_in_upl; uio_last++) {
			        if (!upl_valid_page(pl, uio_last))
				        break;
			}
			/*
			 * compute size to transfer this round,  if uio->uio_resid is
			 * still non-zero after this uiomove, we'll loop around and
			 * set up for another I/O.
			 */
			val_size = (uio_last * PAGE_SIZE) - start_offset;
		
			if (max_size < val_size)
			        val_size = max_size;

			if (uio->uio_resid < val_size)
			        val_size = uio->uio_resid;

			e_lblkno = (int)((uio->uio_offset + ((off_t)val_size - 1)) / PAGE_SIZE_64);

			if (size_of_prefetch = (uio->uio_resid - val_size)) {
			        /*
				 * if there's still I/O left to do for this request, then issue a
				 * pre-fetch I/O... the I/O wait time will overlap
				 * with the copying of the data
				 */
     				cluster_rd_prefetch(vp, object, uio->uio_offset + val_size, size_of_prefetch, filesize, devblocksize);
			} else {
			        if (!(vp->v_flag & VRAOFF) && !(vp->v_flag & VNOCACHE_DATA))
				        /*
					 * let's try to read ahead if we're in 
					 * a sequential access pattern
					 */
				        cluster_rd_ahead(vp, object, b_lblkno, e_lblkno, filesize, devblocksize);
				vp->v_lastr = e_lblkno;
			}
#ifdef ppc
			if (uio->uio_segflg == UIO_USERSPACE) {
				int       offset;

			        segflg = uio->uio_segflg;

				uio->uio_segflg = UIO_PHYS_USERSPACE;


				KERNEL_DEBUG((FSDBG_CODE(DBG_FSRW, 34)) | DBG_FUNC_START,
					     (int)uio->uio_offset, val_size, uio->uio_resid, 0, 0);

				offset = start_offset;

				while (val_size && retval == 0) {
	 				int   	  csize;
					int       i;
					caddr_t   paddr;

					i = offset / PAGE_SIZE;
					csize = min(PAGE_SIZE - start_offset, val_size);

				        paddr = (caddr_t)upl_phys_page(pl, i) + start_offset;

					retval = uiomove(paddr, csize, uio);

					val_size    -= csize;
					offset      += csize;
					start_offset = offset & PAGE_MASK;
				}
				KERNEL_DEBUG((FSDBG_CODE(DBG_FSRW, 34)) | DBG_FUNC_END,
					     (int)uio->uio_offset, val_size, uio->uio_resid, 0, 0);

				uio->uio_segflg = segflg;
			} else
#endif
			{
			        if ((kret = kernel_upl_map(kernel_map, upl, &io_address)) != KERN_SUCCESS)
				        panic("cluster_read: kernel_upl_map failed\n");

				retval = uiomove((caddr_t)(io_address + start_offset), val_size, uio);

			        if ((kret = kernel_upl_unmap(kernel_map, upl)) != KERN_SUCCESS)
				        panic("cluster_read: kernel_upl_unmap failed\n");
			}
		}
		if (start_pg < last_pg) {
		        /*
			 * compute the range of pages that we actually issued an I/O for
			 * and either commit them as valid if the I/O succeeded
			 * or abort them if the I/O failed
			 */
		        io_size = (last_pg - start_pg) * PAGE_SIZE;

			KERNEL_DEBUG((FSDBG_CODE(DBG_FSRW, 35)) | DBG_FUNC_START,
				     upl, start_pg * PAGE_SIZE, io_size, error, 0);

			if (error || (vp->v_flag & VNOCACHE_DATA))
			        kernel_upl_abort_range(upl, start_pg * PAGE_SIZE, io_size,
						UPL_ABORT_DUMP_PAGES | UPL_ABORT_FREE_ON_EMPTY);
			else
			        kernel_upl_commit_range(upl, 
					start_pg * PAGE_SIZE, io_size, 
					UPL_COMMIT_CLEAR_DIRTY 
						| UPL_COMMIT_FREE_ON_EMPTY 
						| UPL_COMMIT_INACTIVATE,
					pl, MAX_UPL_TRANSFER);

			KERNEL_DEBUG((FSDBG_CODE(DBG_FSRW, 35)) | DBG_FUNC_END,
				     upl, start_pg * PAGE_SIZE, io_size, error, 0);
		}
		if ((last_pg - start_pg) < pages_in_upl) {
		        int cur_pg;
			int commit_flags;

		        /*
			 * the set of pages that we issued an I/O for did not encompass
			 * the entire upl... so just release these without modifying
			 * there state
			 */
			if (error)
			        kernel_upl_abort(upl, 0);
			else {
			        KERNEL_DEBUG((FSDBG_CODE(DBG_FSRW, 35)) | DBG_FUNC_START,
					     upl, -1, pages_in_upl - (last_pg - start_pg), 0, 0);

			        if (start_pg) {
			              /*
				       * we found some already valid pages at the beginning of the upl
				       * commit these back to the inactive list with reference cleared
				       */
				        for (cur_pg = 0; cur_pg < start_pg; cur_pg++) {
					        commit_flags = UPL_COMMIT_FREE_ON_EMPTY | UPL_COMMIT_INACTIVATE;
						
						if (upl_dirty_page(pl, cur_pg))
						        commit_flags |= UPL_COMMIT_SET_DIRTY;
						
						if ( !(commit_flags & UPL_COMMIT_SET_DIRTY) && (vp->v_flag & VNOCACHE_DATA))
						        kernel_upl_abort_range(upl, cur_pg * PAGE_SIZE, PAGE_SIZE,
									UPL_ABORT_DUMP_PAGES | UPL_ABORT_FREE_ON_EMPTY);
						else
						        kernel_upl_commit_range(upl, cur_pg * PAGE_SIZE, 
								PAGE_SIZE, commit_flags, pl, MAX_UPL_TRANSFER);
					}
				}
				if (last_pg < uio_last) {
			              /*
				       * we found some already valid pages immediately after the pages we issued
				       * I/O for, commit these back to the inactive list with reference cleared
				       */
				        for (cur_pg = last_pg; cur_pg < uio_last; cur_pg++) {
					        commit_flags = UPL_COMMIT_FREE_ON_EMPTY | UPL_COMMIT_INACTIVATE;

						if (upl_dirty_page(pl, cur_pg))
						        commit_flags |= UPL_COMMIT_SET_DIRTY;
						
						if ( !(commit_flags & UPL_COMMIT_SET_DIRTY) && (vp->v_flag & VNOCACHE_DATA))
						        kernel_upl_abort_range(upl, cur_pg * PAGE_SIZE, PAGE_SIZE,
									UPL_ABORT_DUMP_PAGES | UPL_ABORT_FREE_ON_EMPTY);
						else
						        kernel_upl_commit_range(upl, cur_pg * PAGE_SIZE, 
								PAGE_SIZE, commit_flags, pl, MAX_UPL_TRANSFER);
					}
				}
				if (uio_last < pages_in_upl) {
				        /*
					 * there were some invalid pages beyond the valid pages that we didn't
					 * issue an I/O for, just release them unchanged
					 */
				        kernel_upl_abort(upl, 0);
				}

			        KERNEL_DEBUG((FSDBG_CODE(DBG_FSRW, 35)) | DBG_FUNC_END,
					     upl, -1, -1, 0, 0);
			}
		}
		if (retval == 0)
		        retval = error;
	}

	return (retval);
}

static
cluster_nocopy_read(object, vp, uio, filesize, devblocksize, flags)
        void         *object;
	struct vnode *vp;
	struct uio   *uio;
	off_t         filesize;
	int           devblocksize;
	int           flags;
{
	upl_t            upl;
	upl_page_info_t  *pl;
	off_t 	         upl_f_offset;
	vm_offset_t      upl_offset;
	off_t            start_upl_f_offset;
	off_t            max_io_size;
	int              io_size;
	int              upl_size;
	int              upl_needed_size;
	int              pages_in_pl;
	vm_offset_t      paddr;
	int              upl_flags;
	kern_return_t    kret;
	int              segflg;
	struct iovec     *iov;
	int              i;
	int              force_data_sync;
	int              error  = 0;
	int              retval = 0;

	KERNEL_DEBUG((FSDBG_CODE(DBG_FSRW, 70)) | DBG_FUNC_START,
		     (int)uio->uio_offset, uio->uio_resid, (int)filesize, devblocksize, 0);

	/*
	 * When we enter this routine, we know
	 *  -- the offset into the file is on a pagesize boundary
	 *  -- the resid is a page multiple
	 *  -- the resid will not exceed iov_len
	 */

	iov = uio->uio_iov;
	while (uio->uio_resid && uio->uio_offset < filesize && retval == 0) {

	  io_size = uio->uio_resid;

	  /*
	   * We don't come into this routine unless
	   * UIO_USERSPACE is set.
	   */
	  segflg = uio->uio_segflg;

	  uio->uio_segflg = UIO_PHYS_USERSPACE;

	  /*
	   * First look for pages already in the cache
	   * and move them to user space.
	   */
	  while (io_size && retval == 0) {

	    upl_f_offset = uio->uio_offset;

	    /*
	     * If this call fails, it means the page is not
	     * in the page cache.
	     */
	    if (memory_object_page_op(object, (vm_offset_t)upl_f_offset,
				      UPL_POP_SET | UPL_POP_BUSY,
				      &paddr, 0) != KERN_SUCCESS)
	      break;

	    retval = uiomove((caddr_t)(paddr), PAGE_SIZE, uio);
				
	    memory_object_page_op(object, (vm_offset_t)upl_f_offset, 
				  UPL_POP_CLR | UPL_POP_BUSY, 0, 0);
		  
	    io_size     -= PAGE_SIZE;
	    KERNEL_DEBUG((FSDBG_CODE(DBG_FSRW, 71)) | DBG_FUNC_NONE,
		       (int)uio->uio_offset, io_size, uio->uio_resid, 0, 0);
	  }

	  uio->uio_segflg = segflg;
			
	  if (retval)
	    {
	      KERNEL_DEBUG((FSDBG_CODE(DBG_FSRW, 70)) | DBG_FUNC_END,
			   (int)uio->uio_offset, uio->uio_resid, 2, retval, 0);	      
	      return(retval);
	    }

	  /* If we are already finished with this read, then return */
	  if (io_size == 0)
	    {

	      KERNEL_DEBUG((FSDBG_CODE(DBG_FSRW, 70)) | DBG_FUNC_END,
			   (int)uio->uio_offset, uio->uio_resid, 3, io_size, 0);
	      return(0);
	    }

	  max_io_size = io_size;
	  if (max_io_size > MAXPHYSIO)
	    max_io_size = MAXPHYSIO;

	  start_upl_f_offset = uio->uio_offset;   /* this is page aligned in the file */
	  upl_f_offset = start_upl_f_offset;
	  io_size = 0;

	  while(io_size < max_io_size)
	    {

	      if(memory_object_page_op(object, (vm_offset_t)upl_f_offset,
		       UPL_POP_SET | UPL_POP_BUSY, &paddr, 0) == KERN_SUCCESS)
	      {
		memory_object_page_op(object, (vm_offset_t)upl_f_offset,
				    UPL_POP_CLR | UPL_POP_BUSY, 0, 0);
		break;
	      }

	      /*
	       * Build up the io request parameters.
	       */

	      io_size += PAGE_SIZE;
	      upl_f_offset += PAGE_SIZE;
	    }

	  if (io_size == 0)
	    return(retval);

	  upl_offset = (vm_offset_t)iov->iov_base & PAGE_MASK_64;
	  upl_needed_size = (upl_offset + io_size + (PAGE_SIZE -1)) & ~PAGE_MASK;

	  KERNEL_DEBUG((FSDBG_CODE(DBG_FSRW, 72)) | DBG_FUNC_START,
		       (int)upl_offset, upl_needed_size, iov->iov_base, io_size, 0);

	  for (force_data_sync = 0; force_data_sync < 3; force_data_sync++)
	    {
	      pages_in_pl = 0;
	      upl_size = upl_needed_size;
	      upl_flags = UPL_NO_SYNC | UPL_CLEAN_IN_PLACE | UPL_SET_INTERNAL;

	      kret = vm_map_get_upl(current_map(),
				    (vm_offset_t)iov->iov_base & ~PAGE_MASK,
				    &upl_size, &upl, &pl, &pages_in_pl, &upl_flags, force_data_sync);

	      pages_in_pl = upl_size / PAGE_SIZE;

	      if (kret != KERN_SUCCESS)
		{
		  KERNEL_DEBUG((FSDBG_CODE(DBG_FSRW, 72)) | DBG_FUNC_END,
			       (int)upl_offset, upl_size, io_size, kret, 0);
		  
		  KERNEL_DEBUG((FSDBG_CODE(DBG_FSRW, 70)) | DBG_FUNC_END,
			       (int)uio->uio_offset, uio->uio_resid, 4, retval, 0);

		  /* cluster_nocopy_read: failed to get pagelist */
		  /* do not return kret here */
		  return(retval);
		}

	      for(i=0; i < pages_in_pl; i++)
		{
		  if (!upl_valid_page(pl, i))
		    break;		  
		}
	      if (i == pages_in_pl)
		break;

	      kernel_upl_abort_range(upl, (upl_offset & ~PAGE_MASK), upl_size, 
				     UPL_ABORT_FREE_ON_EMPTY);
	    }

	  if (force_data_sync >= 3)
	    {
		  KERNEL_DEBUG((FSDBG_CODE(DBG_FSRW, 72)) | DBG_FUNC_END,
			       (int)upl_offset, upl_size, io_size, kret, 0);
		  
		  KERNEL_DEBUG((FSDBG_CODE(DBG_FSRW, 70)) | DBG_FUNC_END,
			       (int)uio->uio_offset, uio->uio_resid, 5, retval, 0);
	      return(retval);
	    }
	  /*
	   * Consider the possibility that upl_size wasn't satisfied.
	   */
	  if (upl_size != upl_needed_size)
	    io_size = (upl_size - (int)upl_offset) & ~PAGE_MASK;

	  if (io_size == 0)
	    {
	      kernel_upl_abort_range(upl, (upl_offset & ~PAGE_MASK), upl_size, 
				   UPL_ABORT_FREE_ON_EMPTY);
	      return(retval);
	    }

	  KERNEL_DEBUG((FSDBG_CODE(DBG_FSRW, 72)) | DBG_FUNC_END,
		       (int)upl_offset, upl_size, io_size, kret, 0);

	  /*
	   * issue a synchronous read to cluster_io
	   */

	  KERNEL_DEBUG((FSDBG_CODE(DBG_FSRW, 73)) | DBG_FUNC_START,
		       upl, (int)upl_offset, (int)start_upl_f_offset, io_size, 0);

	  error = cluster_io(vp, upl, upl_offset, start_upl_f_offset,
			     io_size, CL_READ| CL_NOZERO, (struct buf *)0);

	  if (error == 0) {
	    /*
	     * The cluster_io read completed successfully,
	     * update the uio structure and commit.
	     */

	    kernel_upl_commit_range(upl, (upl_offset & ~PAGE_MASK), upl_size, 
				    UPL_COMMIT_SET_DIRTY 
						| UPL_COMMIT_FREE_ON_EMPTY, 
				    pl, MAX_UPL_TRANSFER);
	    
	    iov->iov_base += io_size;
	    iov->iov_len -= io_size;
	    uio->uio_resid -= io_size;
	    uio->uio_offset += io_size;
	  }
	  else {
	    kernel_upl_abort_range(upl, (upl_offset & ~PAGE_MASK), upl_size, 
				   UPL_ABORT_FREE_ON_EMPTY);
	  }

	  KERNEL_DEBUG((FSDBG_CODE(DBG_FSRW, 73)) | DBG_FUNC_END,
		       upl, (int)uio->uio_offset, (int)uio->uio_resid, error, 0);

	  if (retval == 0)
	    retval = error;

	} /* end while */


	KERNEL_DEBUG((FSDBG_CODE(DBG_FSRW, 70)) | DBG_FUNC_END,
		     (int)uio->uio_offset, (int)uio->uio_resid, 6, retval, 0);

	return (retval);
}



/*
 * generate advisory I/O's in the largest chunks possible
 * the completed pages will be released into the VM cache
 */
advisory_read(vp, filesize, f_offset, resid, devblocksize)
	struct vnode *vp;
	off_t         filesize;
	off_t         f_offset;
	int           resid;
	int           devblocksize;
{
	void            *object;
	upl_page_info_t *pl;
	upl_t            upl;
	vm_offset_t      upl_offset;
	int              upl_size;
	off_t 	         upl_f_offset;
	int		 start_offset;
	int	         start_pg;
	int		 last_pg;
	int              pages_in_upl;
	off_t            max_size;
	int              io_size;
	kern_return_t    kret;
	int              retval = 0;


	if (!UBCINFOEXISTS(vp))
		return(EINVAL);

	object = ubc_getobject(vp, UBC_NOREACTIVATE);
	if (object == (void *)NULL)
		panic("advisory_read: ubc_getobject failed");

	KERNEL_DEBUG((FSDBG_CODE(DBG_FSRW, 60)) | DBG_FUNC_START,
		     (int)f_offset, resid, (int)filesize, devblocksize, 0);

	while (resid && f_offset < filesize && retval == 0) {
		/*
		 * compute the size of the upl needed to encompass
		 * the requested read... limit each call to cluster_io
		 * to at most MAXPHYSIO, make sure to account for 
		 * a starting offset that's not page aligned
		 */
		start_offset = (int)(f_offset & PAGE_MASK_64);
		upl_f_offset = f_offset - (off_t)start_offset;
		max_size     = filesize - f_offset;

		if (resid < max_size)
		        io_size = resid;
		else
		        io_size = max_size;

		upl_size = (start_offset + io_size + (PAGE_SIZE - 1)) & ~PAGE_MASK;
	        if (upl_size > MAXPHYSIO)
			upl_size = MAXPHYSIO;
		pages_in_upl = upl_size / PAGE_SIZE;

		kret = vm_fault_list_request(object, 
				(vm_object_offset_t)upl_f_offset, upl_size, &upl, NULL, 0,  
				(UPL_NO_SYNC | UPL_CLEAN_IN_PLACE | UPL_SET_INTERNAL));

		if (kret != KERN_SUCCESS)
			panic("advisory_read: failed to get pagelist");

		pl = UPL_GET_INTERNAL_PAGE_LIST(upl);


		KERNEL_DEBUG((FSDBG_CODE(DBG_FSRW, 61)) | DBG_FUNC_NONE,
			     upl, (int)upl_f_offset, upl_size, start_offset, 0);

		/*
		 * scan from the beginning of the upl looking for the first
		 * non-valid page.... this will become the first page in
		 * the request we're going to make to 'cluster_io'... if all
		 * of the pages are valid, we won't call through to 'cluster_io'
		 */
		for (start_pg = 0; start_pg < pages_in_upl; start_pg++) {
			if (!upl_valid_page(pl, start_pg))
				break;
		}

		/*
		 * scan from the starting invalid page looking for a valid
		 * page before the end of the upl is reached, if we 
		 * find one, then it will be the last page of the request to
		 * 'cluster_io'
		 */
		for (last_pg = start_pg; last_pg < pages_in_upl; last_pg++) {
			if (upl_valid_page(pl, last_pg))
				break;
		}

		if (start_pg < last_pg) {		
		        /*
			 * we found a range of 'invalid' pages that must be filled
			 * if the last page in this range is the last page of the file
			 * we may have to clip the size of it to keep from reading past
			 * the end of the last physical block associated with the file
			 */
			upl_offset = start_pg * PAGE_SIZE;
			io_size    = (last_pg - start_pg) * PAGE_SIZE;

			if ((upl_f_offset + upl_offset + io_size) > filesize) {
			        io_size = filesize - (upl_f_offset + upl_offset);
				io_size = (io_size + (devblocksize - 1)) & ~(devblocksize - 1);
			}
			/*
			 * issue an asynchronous read to cluster_io
			 */
			retval = cluster_io(vp, upl, upl_offset, upl_f_offset + upl_offset, io_size,
					  CL_ASYNC | CL_READ | CL_COMMIT | CL_AGE, (struct buf *)0);
		}
		if (start_pg) {
			/*
			 * start_pg of non-zero indicates we found some already valid pages
			 * at the beginning of the upl.... we need to release these without
			 * modifying there state
			 */
		        kernel_upl_abort_range(upl, 0, start_pg * PAGE_SIZE, UPL_ABORT_FREE_ON_EMPTY);

			KERNEL_DEBUG((FSDBG_CODE(DBG_FSRW, 62)) | DBG_FUNC_NONE,
				     upl, 0, start_pg * PAGE_SIZE, 0, 0);
		}
		if (last_pg < pages_in_upl) {
			/*
			 * the set of pages that we issued an I/O for did not extend all the
			 * way to the end of the upl... so just release them without modifying
			 * there state
			 */
		        kernel_upl_abort_range(upl, last_pg * PAGE_SIZE, (pages_in_upl - last_pg) * PAGE_SIZE,
					UPL_ABORT_FREE_ON_EMPTY);

			KERNEL_DEBUG((FSDBG_CODE(DBG_FSRW, 63)) | DBG_FUNC_NONE,
				     upl, last_pg * PAGE_SIZE,
				     (pages_in_upl - last_pg) * PAGE_SIZE, 0, 0);
		}
		io_size = (last_pg * PAGE_SIZE) - start_offset;
		
		if (io_size > resid)
		        io_size = resid;
		f_offset += io_size;
		resid    -= io_size;
	}
	KERNEL_DEBUG((FSDBG_CODE(DBG_FSRW, 60)) | DBG_FUNC_END,
		     (int)f_offset, resid, retval, 0, 0);

	return(retval);
}


cluster_push(vp)
        struct vnode *vp;
{
	void            *object;
	upl_page_info_t *pl;
	upl_t            upl;
	vm_offset_t      upl_offset;
	int              upl_size;
	off_t 	         upl_f_offset;
        int              pages_in_upl;
	int              start_pg;
	int              last_pg;
	int              io_size;
	int              io_flags;
	int              size;
	kern_return_t    kret;


	if (!UBCINFOEXISTS(vp))
		return(0);

        if (vp->v_clen == 0 || (pages_in_upl = vp->v_lastw - vp->v_cstart) == 0)
	        return (0);
	upl_size = pages_in_upl * PAGE_SIZE;
	upl_f_offset = ((off_t)vp->v_cstart) * PAGE_SIZE_64;
	size = vp->v_ciosiz;
	vp->v_clen = 0;

	if (size > upl_size || (upl_size - size) > PAGE_SIZE)
	        panic("cluster_push: v_ciosiz doesn't match size of cluster\n");

	object = ubc_getobject(vp, UBC_NOREACTIVATE);
	if (object == (void *)NULL)
		panic("cluster_push: ubc_getobject failed");

	kret = vm_fault_list_request(object, 
				     (vm_object_offset_t)upl_f_offset, upl_size, &upl, NULL, 0,  
				     (UPL_NO_SYNC | UPL_CLEAN_IN_PLACE | UPL_SET_INTERNAL));
	if (kret != KERN_SUCCESS)
	        panic("cluster_push: failed to get pagelist");

	pl = UPL_GET_INTERNAL_PAGE_LIST(upl);

	last_pg = 0;

	while (size) {

		for (start_pg = last_pg; start_pg < pages_in_upl; start_pg++) {
			if (upl_valid_page(pl, start_pg) && upl_dirty_page(pl, start_pg))
				break;
		}
		if (start_pg > last_pg) {
		        io_size = (start_pg - last_pg) * PAGE_SIZE;

		        kernel_upl_abort_range(upl, last_pg * PAGE_SIZE, io_size, UPL_ABORT_FREE_ON_EMPTY);

			if (io_size < size)
			        size -= io_size;
			else
			        break;
		}
		for (last_pg = start_pg; last_pg < pages_in_upl; last_pg++) {
			if (!upl_valid_page(pl, last_pg) || !upl_dirty_page(pl, last_pg))
				break;
		}
		upl_offset = start_pg * PAGE_SIZE;

		io_size = min(size, (last_pg - start_pg) * PAGE_SIZE);

		if (vp->v_numoutput > ASYNC_THROTTLE)
		        io_flags = CL_COMMIT | CL_AGE;
		else
		        io_flags = CL_COMMIT | CL_AGE | CL_ASYNC;

		if (vp->v_flag & VNOCACHE_DATA)
		        io_flags |= CL_DUMP;

		cluster_io(vp, upl, upl_offset, upl_f_offset + upl_offset, io_size, io_flags, (struct buf *)0);

		size -= io_size;
	}
	return(1);
}

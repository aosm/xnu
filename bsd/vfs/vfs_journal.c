/*
 * Copyright (c) 1995-2004 Apple Computer, Inc. All rights reserved.
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
//
// This file implements a simple write-ahead journaling layer.  
// In theory any file system can make use of it by calling these 
// functions when the fs wants to modify meta-data blocks.  See
// vfs_journal.h for a more detailed description of the api and
// data structures.
//
// Dominic Giampaolo (dbg@apple.com)
//

#ifdef KERNEL

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/file_internal.h>
#include <sys/stat.h>
#include <sys/buf_internal.h>
#include <sys/proc_internal.h>
#include <sys/mount_internal.h>
#include <sys/namei.h>
#include <sys/vnode_internal.h>
#include <sys/ioctl.h>
#include <sys/tty.h>
#include <sys/ubc.h>
#include <sys/malloc.h>
#include <kern/thread.h>
#include <sys/disk.h>
#include <miscfs/specfs/specdev.h>

extern task_t kernel_task;

#else

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdarg.h>
#include <sys/types.h>
#include "compat.h"

#endif   /* KERNEL */

#include "vfs_journal.h"


// number of bytes to checksum in a block_list_header
// NOTE: this should be enough to clear out the header
//       fields as well as the first entry of binfo[]
#define BLHDR_CHECKSUM_SIZE 32



static int  end_transaction(transaction *tr, int force_it);
static void abort_transaction(journal *jnl, transaction *tr);
static void dump_journal(journal *jnl);

static __inline__ void  lock_journal(journal *jnl);
static __inline__ void  unlock_journal(journal *jnl);
static __inline__ void  lock_oldstart(journal *jnl);
static __inline__ void  unlock_oldstart(journal *jnl);




//
// 3105942 - Coalesce writes to the same block on journal replay
//

typedef struct bucket {
    off_t block_num;
    size_t jnl_offset;
    size_t block_size;
} bucket;

#define STARTING_BUCKETS 256

static int add_block(journal *jnl, struct bucket **buf_ptr, off_t block_num, size_t size, size_t offset, int *num_buckets_ptr, int *num_full_ptr);
static int grow_table(struct bucket **buf_ptr, int num_buckets, int new_size);
static int lookup_bucket(struct bucket **buf_ptr, off_t block_num, int num_full);
static int do_overlap(journal *jnl, struct bucket **buf_ptr, int blk_index, off_t block_num, size_t size, size_t offset, int *num_buckets_ptr, int *num_full_ptr);
static int insert_block(journal *jnl, struct bucket **buf_ptr, int blk_index, off_t num, size_t size, size_t offset, int *num_buckets_ptr, int *num_full_ptr, int overwriting);

#define CHECK_JOURNAL(jnl) \
    do { \
    if (jnl == NULL) {\
	panic("%s:%d: null journal ptr?\n", __FILE__, __LINE__);\
    }\
    if (jnl->jdev == NULL) { \
	panic("%s:%d: jdev is null!\n", __FILE__, __LINE__);\
    } \
    if (jnl->fsdev == NULL) { \
	panic("%s:%d: fsdev is null!\n", __FILE__, __LINE__);\
    } \
    if (jnl->jhdr->magic != JOURNAL_HEADER_MAGIC) {\
	panic("%s:%d: jhdr magic corrupted (0x%x != 0x%x)\n",\
	__FILE__, __LINE__, jnl->jhdr->magic, JOURNAL_HEADER_MAGIC);\
    }\
    if (   jnl->jhdr->start <= 0 \
	|| jnl->jhdr->start > jnl->jhdr->size\
	|| jnl->jhdr->start > 1024*1024*1024) {\
	panic("%s:%d: jhdr start looks bad (0x%llx max size 0x%llx)\n", \
	__FILE__, __LINE__, jnl->jhdr->start, jnl->jhdr->size);\
    }\
    if (   jnl->jhdr->end <= 0 \
	|| jnl->jhdr->end > jnl->jhdr->size\
	|| jnl->jhdr->end > 1024*1024*1024) {\
	panic("%s:%d: jhdr end looks bad (0x%llx max size 0x%llx)\n", \
	__FILE__, __LINE__, jnl->jhdr->end, jnl->jhdr->size);\
    }\
    if (jnl->jhdr->size > 1024*1024*1024) {\
	panic("%s:%d: jhdr size looks bad (0x%llx)\n",\
	__FILE__, __LINE__, jnl->jhdr->size);\
    } \
    } while(0)

#define CHECK_TRANSACTION(tr) \
    do {\
    if (tr == NULL) {\
	panic("%s:%d: null transaction ptr?\n", __FILE__, __LINE__);\
    }\
    if (tr->jnl == NULL) {\
	panic("%s:%d: null tr->jnl ptr?\n", __FILE__, __LINE__);\
    }\
    if (tr->blhdr != (block_list_header *)tr->tbuffer) {\
	panic("%s:%d: blhdr (0x%x) != tbuffer (0x%x)\n", __FILE__, __LINE__, tr->blhdr, tr->tbuffer);\
    }\
    if (tr->total_bytes < 0) {\
	panic("%s:%d: tr total_bytes looks bad: %d\n", __FILE__, __LINE__, tr->total_bytes);\
    }\
    if (tr->journal_start < 0 || tr->journal_start > 1024*1024*1024) {\
	panic("%s:%d: tr journal start looks bad: 0x%llx\n", __FILE__, __LINE__, tr->journal_start);\
    }\
    if (tr->journal_end < 0 || tr->journal_end > 1024*1024*1024) {\
	panic("%s:%d: tr journal end looks bad: 0x%llx\n", __FILE__, __LINE__, tr->journal_end);\
    }\
    if (tr->blhdr && (tr->blhdr->max_blocks <= 0 || tr->blhdr->max_blocks > (tr->jnl->jhdr->size/tr->jnl->jhdr->jhdr_size))) {\
	panic("%s:%d: tr blhdr max_blocks looks bad: %d\n", __FILE__, __LINE__, tr->blhdr->max_blocks);\
    }\
    } while(0)



//
// this isn't a great checksum routine but it will do for now.
// we use it to checksum the journal header and the block list
// headers that are at the start of each transaction.
//
static int
calc_checksum(char *ptr, int len)
{
    int i, cksum=0;

    // this is a lame checksum but for now it'll do
    for(i=0; i < len; i++, ptr++) {
		cksum = (cksum << 8) ^ (cksum + *(unsigned char *)ptr);
    }

    return (~cksum);
}

//
// Journal Locking
//
lck_grp_attr_t *  jnl_group_attr;
lck_attr_t *      jnl_lock_attr;
lck_grp_t *       jnl_mutex_group;

void
journal_init()
{
	jnl_lock_attr    = lck_attr_alloc_init();
	jnl_group_attr   = lck_grp_attr_alloc_init();
	jnl_mutex_group  = lck_grp_alloc_init("jnl-mutex", jnl_group_attr);

	/* Turn on lock debugging */
	//lck_attr_setdebug(jnl_lock_attr);
}

static __inline__ void
lock_journal(journal *jnl)
{
	lck_mtx_lock(&jnl->jlock);
}

static __inline__ void
unlock_journal(journal *jnl)
{
	lck_mtx_unlock(&jnl->jlock);
}

static __inline__ void
lock_oldstart(journal *jnl)
{
	lck_mtx_lock(&jnl->old_start_lock);
}

static __inline__ void
unlock_oldstart(journal *jnl)
{
	lck_mtx_unlock(&jnl->old_start_lock);
}



#define JNL_WRITE    0x0001
#define JNL_READ     0x0002
#define JNL_HEADER   0x8000

//
// This function sets up a fake buf and passes it directly to the
// journal device strategy routine (so that it won't get cached in
// the block cache.
//
// It also handles range checking the i/o so that we don't write
// outside the journal boundaries and it will wrap the i/o back
// to the beginning if necessary (skipping over the journal header)
// 
static size_t
do_journal_io(journal *jnl, off_t *offset, void *data, size_t len, int direction)
{
    int         err, io_sz=0, curlen=len;
    buf_t	bp;
    int 	max_iosize = 128 * 1024;
    struct vfsioattr ioattr;

    if (*offset < 0 || *offset > jnl->jhdr->size) {
		panic("jnl: do_jnl_io: bad offset 0x%llx (max 0x%llx)\n", *offset, jnl->jhdr->size);
    }
    vfs_ioattr(vnode_mount(jnl->jdev), &ioattr);

    if (direction & JNL_WRITE)
            max_iosize = ioattr.io_maxwritecnt;
    else if (direction & JNL_READ)
            max_iosize = ioattr.io_maxreadcnt;

  again:
    bp = alloc_io_buf(jnl->jdev, 1);

    if (*offset + (off_t)curlen > jnl->jhdr->size && *offset != 0 && jnl->jhdr->size != 0) {
		if (*offset == jnl->jhdr->size) {
			*offset = jnl->jhdr->jhdr_size;
		} else {
			curlen = (off_t)jnl->jhdr->size - *offset;
		}
    }

	if (curlen > max_iosize) {
		curlen = max_iosize;
	}

    if (curlen <= 0) {
		panic("jnl: do_jnl_io: curlen == %d, offset 0x%llx len %d\n", curlen, *offset, len);
    }

	if (*offset == 0 && (direction & JNL_HEADER) == 0) {
		panic("jnl: request for i/o to jnl-header without JNL_HEADER flag set! (len %d, data %p)\n", curlen, data);
	}

    if (direction & JNL_READ)
            buf_setflags(bp, B_READ);
    else {
            /*
	     * don't have to set any flags
	     */
            vnode_startwrite(jnl->jdev);
    }
    buf_setsize(bp, curlen);
    buf_setcount(bp, curlen);
    buf_setdataptr(bp, (uintptr_t)data);
    buf_setblkno(bp, (daddr64_t) ((jnl->jdev_offset + *offset) / (off_t)jnl->jhdr->jhdr_size));
    buf_setlblkno(bp, (daddr64_t) ((jnl->jdev_offset + *offset) / (off_t)jnl->jhdr->jhdr_size));

    err = VNOP_STRATEGY(bp);
    if (!err) {
		err = (int)buf_biowait(bp);
    }
    free_io_buf(bp);

    if (err) {
		printf("jnl: do_jnl_io: strategy err 0x%x\n", err);
		return 0;
    }

    *offset += curlen;
    io_sz   += curlen;
    if (io_sz != len) {
		// handle wrap-around
		data    = (char *)data + curlen;
		curlen  = len - io_sz;
		if (*offset >= jnl->jhdr->size) {
			*offset = jnl->jhdr->jhdr_size;
		}
		goto again;
    }

    return io_sz;
}

static size_t
read_journal_data(journal *jnl, off_t *offset, void *data, size_t len)
{
    return do_journal_io(jnl, offset, data, len, JNL_READ);
}

static size_t
write_journal_data(journal *jnl, off_t *offset, void *data, size_t len)
{
    return do_journal_io(jnl, offset, data, len, JNL_WRITE);
}


static int
read_journal_header(journal *jnl, void *data, size_t len)
{
	off_t hdr_offset = 0;
	
	return do_journal_io(jnl, &hdr_offset, data, len, JNL_READ|JNL_HEADER);
}

static int
write_journal_header(journal *jnl)
{
	static int num_err_prints = 0;
    int ret;
    off_t jhdr_offset = 0;
    struct vfs_context context;

    context.vc_proc = current_proc();
    context.vc_ucred = NOCRED;
    // 
    // XXXdbg note: this ioctl doesn't seem to do anything on firewire disks.
    //
    ret = VNOP_IOCTL(jnl->jdev, DKIOCSYNCHRONIZECACHE, NULL, FWRITE, &context);
    if (ret != 0) {
	//
	// Only print this error if it's a different error than the
	// previous one, or if it's the first time for this device
	// or if the total number of printfs is less than 25.  We
	// allow for up to 25 printfs to insure that some make it
	// into the on-disk syslog.  Otherwise if we only printed
	// one, it's possible it would never make it to the syslog
	// for the root volume and that makes debugging hard.
	//
	if (   ret != jnl->last_flush_err
	    || (jnl->flags & JOURNAL_FLUSHCACHE_ERR) == 0
	    || num_err_prints++ < 25) {
	    
	    printf("jnl: flushing fs disk buffer returned 0x%x\n", ret);
	    
	    jnl->flags |= JOURNAL_FLUSHCACHE_ERR;
	    jnl->last_flush_err = ret;
	}
    }

    
    jnl->jhdr->checksum = 0;
    jnl->jhdr->checksum = calc_checksum((char *)jnl->jhdr, sizeof(struct journal_header));
    if (do_journal_io(jnl, &jhdr_offset, jnl->header_buf, jnl->jhdr->jhdr_size, JNL_WRITE|JNL_HEADER) != jnl->jhdr->jhdr_size) {
	printf("jnl: write_journal_header: error writing the journal header!\n");
	jnl->flags |= JOURNAL_INVALID;
	return -1;
    }	

    // Have to flush after writing the journal header so that
    // a future transaction doesn't sneak out to disk before
    // the header does and thus overwrite data that the old
    // journal header refers to.  Saw this exact case happen
    // on an IDE bus analyzer with Larry Barras so while it
    // may seem obscure, it's not.
    //
    VNOP_IOCTL(jnl->jdev, DKIOCSYNCHRONIZECACHE, NULL, FWRITE, &context);

    return 0;
}



//
// this is a work function used to free up transactions that
// completed. they can't be free'd from buffer_flushed_callback
// because it is called from deep with the disk driver stack
// and thus can't do something that would potentially cause
// paging.  it gets called by each of the journal api entry
// points so stuff shouldn't hang around for too long.
//
static void
free_old_stuff(journal *jnl)
{
    transaction *tr, *next;

    lock_oldstart(jnl);
    tr = jnl->tr_freeme;
    jnl->tr_freeme = NULL;
    unlock_oldstart(jnl);

    for(; tr; tr=next) {
	next = tr->next;
	FREE_ZONE(tr, sizeof(transaction), M_JNL_TR);
    }

}



//
// This is our callback that lets us know when a buffer has been
// flushed to disk.  It's called from deep within the driver stack
// and thus is quite limited in what it can do.  Notably, it can
// not initiate any new i/o's or allocate/free memory.
//
static void
buffer_flushed_callback(struct buf *bp, void *arg)
{
    transaction  *tr;
    journal      *jnl;
    transaction  *ctr, *prev=NULL, *next;
    int           i, bufsize;


    //printf("jnl: buf flush: bp @ 0x%x l/blkno %qd/%qd vp 0x%x tr @ 0x%x\n",
    //	   bp, buf_lblkno(bp), buf_blkno(bp), buf_vnode(bp), arg);

    // snarf out the bits we want
    bufsize = buf_size(bp);
    tr      = (transaction *)arg;

    // then we've already seen it
    if (tr == NULL) {
		return;
    }

    CHECK_TRANSACTION(tr);

    jnl = tr->jnl;
    if (jnl->flags & JOURNAL_INVALID) {
		return;
    }

    CHECK_JOURNAL(jnl);

    // update the number of blocks that have been flushed.
    // this buf may represent more than one block so take
    // that into account.
    OSAddAtomic(bufsize, &tr->num_flushed);


    // if this transaction isn't done yet, just return as
    // there is nothing to do.
    if ((tr->num_flushed + tr->num_killed) < tr->total_bytes) {
		return;
    }

	// this will single thread checking the transaction
	lock_oldstart(jnl);

	if (tr->total_bytes == 0xfbadc0de) {
		// then someone beat us to it...
		unlock_oldstart(jnl);
		return;
	}

    // mark this so that we're the owner of dealing with the
	// cleanup for this transaction
    tr->total_bytes = 0xfbadc0de;

    //printf("jnl: tr 0x%x (0x%llx 0x%llx) in jnl 0x%x completed.\n",
    //   tr, tr->journal_start, tr->journal_end, jnl);

	// find this entry in the old_start[] index and mark it completed
	for(i=0; i < sizeof(jnl->old_start)/sizeof(jnl->old_start[0]); i++) {

		if ((jnl->old_start[i] & ~(0x8000000000000000LL)) == tr->journal_start) {
			jnl->old_start[i] &= ~(0x8000000000000000LL);
			break;
		}
	}
	if (i >= sizeof(jnl->old_start)/sizeof(jnl->old_start[0])) {
		panic("jnl: buffer_flushed: did not find tr w/start @ %lld (tr 0x%x, jnl 0x%x)\n",
			  tr->journal_start, tr, jnl);
	}
	unlock_oldstart(jnl);


    // if we are here then we need to update the journal header
    // to reflect that this transaction is complete
    if (tr->journal_start == jnl->active_start) {
		jnl->active_start = tr->journal_end;
		tr->journal_start = tr->journal_end = (off_t)0;
    }

    // go through the completed_trs list and try to coalesce
    // entries, restarting back at the beginning if we have to.
    for(ctr=jnl->completed_trs; ctr; prev=ctr, ctr=next) {
		if (ctr->journal_start == jnl->active_start) {
			jnl->active_start = ctr->journal_end;
			if (prev) {
				prev->next = ctr->next;
			}
			if (ctr == jnl->completed_trs) {
				jnl->completed_trs = ctr->next;
			}
	    
			lock_oldstart(jnl);
			next           = jnl->completed_trs;   // this starts us over again
			ctr->next      = jnl->tr_freeme;
			jnl->tr_freeme = ctr;
			ctr            = NULL;
			unlock_oldstart(jnl);
		} else if (tr->journal_end == ctr->journal_start) {
			ctr->journal_start = tr->journal_start;
			next               = jnl->completed_trs;  // this starts us over again
			ctr                = NULL;
			tr->journal_start  = tr->journal_end = (off_t)0;
		} else if (tr->journal_start == ctr->journal_end) {
			ctr->journal_end  = tr->journal_end;
			next              = ctr->next;
			tr->journal_start = tr->journal_end = (off_t)0;
		} else {
			next = ctr->next;
		}
    }
    
    // if this is true then we didn't merge with anyone
    // so link ourselves in at the head of the completed
    // transaction list.
    if (tr->journal_start != 0) {
		// put this entry into the correct sorted place
		// in the list instead of just at the head.
		//
	
		prev = NULL;
		for(ctr=jnl->completed_trs; ctr && tr->journal_start > ctr->journal_start; prev=ctr, ctr=ctr->next) {
			// just keep looping
		}

		if (ctr == NULL && prev == NULL) {
			jnl->completed_trs = tr;
			tr->next = NULL;
		} else if (ctr == jnl->completed_trs) {
			tr->next = jnl->completed_trs;
			jnl->completed_trs = tr;
		} else {
			tr->next = prev->next;
			prev->next = tr;
		}
    } else {
		// if we're here this tr got merged with someone else so
		// put it on the list to be free'd
		lock_oldstart(jnl);
		tr->next       = jnl->tr_freeme;
		jnl->tr_freeme = tr;
		unlock_oldstart(jnl);
    }
}


#include <libkern/OSByteOrder.h>

#define SWAP16(x) OSSwapInt16(x)
#define SWAP32(x) OSSwapInt32(x)
#define SWAP64(x) OSSwapInt64(x)


static void
swap_journal_header(journal *jnl)
{
    jnl->jhdr->magic      = SWAP32(jnl->jhdr->magic);
    jnl->jhdr->endian     = SWAP32(jnl->jhdr->endian);
    jnl->jhdr->start      = SWAP64(jnl->jhdr->start);
    jnl->jhdr->end        = SWAP64(jnl->jhdr->end);
    jnl->jhdr->size       = SWAP64(jnl->jhdr->size);
    jnl->jhdr->blhdr_size = SWAP32(jnl->jhdr->blhdr_size);
    jnl->jhdr->checksum   = SWAP32(jnl->jhdr->checksum);
    jnl->jhdr->jhdr_size  = SWAP32(jnl->jhdr->jhdr_size);
}

static void
swap_block_list_header(journal *jnl, block_list_header *blhdr)
{
    int i;
    
    blhdr->max_blocks = SWAP16(blhdr->max_blocks);
    blhdr->num_blocks = SWAP16(blhdr->num_blocks);
    blhdr->bytes_used = SWAP32(blhdr->bytes_used);
    blhdr->checksum   = SWAP32(blhdr->checksum);
    blhdr->pad        = SWAP32(blhdr->pad);

	if (blhdr->num_blocks * sizeof(blhdr->binfo[0]) > jnl->jhdr->blhdr_size) {
		printf("jnl: blhdr num blocks looks suspicious (%d).  not swapping.\n", blhdr->num_blocks);
		return;
	}

    for(i=0; i < blhdr->num_blocks; i++) {
		blhdr->binfo[i].bnum  = SWAP64(blhdr->binfo[i].bnum);
		blhdr->binfo[i].bsize = SWAP32(blhdr->binfo[i].bsize);
		blhdr->binfo[i].bp    = (void *)SWAP32((int)blhdr->binfo[i].bp);
    }
}


static int
update_fs_block(journal *jnl, void *block_ptr, off_t fs_block, size_t bsize)
{
    int		ret;
    struct buf *oblock_bp=NULL;
    
    // first read the block we want.
    ret = buf_meta_bread(jnl->fsdev, (daddr64_t)fs_block, bsize, NOCRED, &oblock_bp);
    if (ret != 0) {
		printf("jnl: update_fs_block: error reading fs block # %lld! (ret %d)\n", fs_block, ret);

		if (oblock_bp) {
			buf_brelse(oblock_bp);
			oblock_bp = NULL;
		}

		// let's try to be aggressive here and just re-write the block
		oblock_bp = buf_getblk(jnl->fsdev, (daddr64_t)fs_block, bsize, 0, 0, BLK_META);
		if (oblock_bp == NULL) {
			printf("jnl: update_fs_block: buf_getblk() for %lld failed! failing update.\n", fs_block);
			return -1;
		}
    }
	    
    // make sure it's the correct size.
    if (buf_size(oblock_bp) != bsize) {
		buf_brelse(oblock_bp);
		return -1;
    }

    // copy the journal data over top of it
    memcpy((void *)buf_dataptr(oblock_bp), block_ptr, bsize);

    if ((ret = VNOP_BWRITE(oblock_bp)) != 0) {
		printf("jnl: update_fs_block: failed to update block %lld (ret %d)\n", fs_block,ret);
		return ret;
    }

    // and now invalidate it so that if someone else wants to read
    // it in a different size they'll be able to do it.
    ret = buf_meta_bread(jnl->fsdev, (daddr64_t)fs_block, bsize, NOCRED, &oblock_bp);
    if (oblock_bp) {
                buf_markinvalid(oblock_bp);
		buf_brelse(oblock_bp);
    }
	    
    return 0;
}

static int
grow_table(struct bucket **buf_ptr, int num_buckets, int new_size)
{
    struct bucket *newBuf;
    int current_size = num_buckets, i;
    
    // return if newsize is less than the current size
    if (new_size < num_buckets) {
	return current_size;
    }
    
    if ((MALLOC(newBuf, struct bucket *, new_size*sizeof(struct bucket), M_TEMP, M_WAITOK)) == NULL) {
	printf("jnl: grow_table: no memory to expand coalesce buffer!\n");
	return -1;
    }
    
    //  printf("jnl: lookup_bucket: expanded co_buf to %d elems\n", new_size);
    
    // copy existing elements 
    bcopy(*buf_ptr, newBuf, num_buckets*sizeof(struct bucket));
    
    // initialize the new ones
    for(i=num_buckets; i < new_size; i++) {
	newBuf[i].block_num = (off_t)-1;
    }
    
    // free the old container
    FREE(*buf_ptr, M_TEMP);
    
    // reset the buf_ptr
    *buf_ptr = newBuf;
    
    return new_size;
}

static int
lookup_bucket(struct bucket **buf_ptr, off_t block_num, int num_full)
{
    int lo, hi, index, matches, i;
    
    if (num_full == 0) {
	return 0; // table is empty, so insert at index=0
    }
    
    lo = 0;
    hi = num_full - 1;
    index = -1;
    
    // perform binary search for block_num
    do {
	int mid = (hi - lo)/2 + lo;
	off_t this_num = (*buf_ptr)[mid].block_num;
	
	if (block_num == this_num) {
	    index = mid;
	    break;
	}
	
	if (block_num < this_num) {
	    hi = mid;
	    continue;
	}
	
	if (block_num > this_num) {
	    lo = mid + 1;
	    continue;
	}
    } while(lo < hi);
    
    // check if lo and hi converged on the match
    if (block_num == (*buf_ptr)[hi].block_num) {
	index = hi;
    }
    
    // if no existing entry found, find index for new one
    if (index == -1) {
	index = (block_num < (*buf_ptr)[hi].block_num) ? hi : hi + 1;
    } else {
	// make sure that we return the right-most index in the case of multiple matches
	matches = 0;
	i = index + 1;
	while(i < num_full && block_num == (*buf_ptr)[i].block_num) {
	    matches++;
	    i++;
	}

	index += matches;
    }
    
    return index;
}

static int
insert_block(journal *jnl, struct bucket **buf_ptr, int blk_index, off_t num, size_t size, size_t offset, int *num_buckets_ptr, int *num_full_ptr, int overwriting)
{
    if (!overwriting) {
	// grow the table if we're out of space
	if (*num_full_ptr >= *num_buckets_ptr) {
	    int new_size = *num_buckets_ptr * 2;
	    int grow_size = grow_table(buf_ptr, *num_buckets_ptr, new_size);
	    
	    if (grow_size < new_size) {
		printf("jnl: add_block: grow_table returned an error!\n");
		return -1;
	    }
	    
	    *num_buckets_ptr = grow_size; //update num_buckets to reflect the new size
	}
	
	// if we're not inserting at the end, we need to bcopy
	if (blk_index != *num_full_ptr) {
	    bcopy( (*buf_ptr)+(blk_index), (*buf_ptr)+(blk_index+1), (*num_full_ptr-blk_index)*sizeof(struct bucket) );
	}
	
	(*num_full_ptr)++; // increment only if we're not overwriting
    }

    // sanity check the values we're about to add
    if (offset >= jnl->jhdr->size) {
	offset = jnl->jhdr->jhdr_size + (offset - jnl->jhdr->size);
    }
    if (size <= 0) {
	panic("jnl: insert_block: bad size in insert_block (%d)\n", size);
    }	 

    (*buf_ptr)[blk_index].block_num = num;
    (*buf_ptr)[blk_index].block_size = size;
    (*buf_ptr)[blk_index].jnl_offset = offset;
    
    return blk_index;
}

static int
do_overlap(journal *jnl, struct bucket **buf_ptr, int blk_index, off_t block_num, size_t size, size_t offset, int *num_buckets_ptr, int *num_full_ptr)
{
    int num_to_remove, index, i, overwrite, err;
    size_t jhdr_size = jnl->jhdr->jhdr_size, new_offset;
    off_t overlap, block_start, block_end;

    block_start = block_num*jhdr_size;
    block_end = block_start + size;
    overwrite = (block_num == (*buf_ptr)[blk_index].block_num && size >= (*buf_ptr)[blk_index].block_size);

    // first, eliminate any overlap with the previous entry
    if (blk_index != 0 && !overwrite) {
	off_t prev_block_start = (*buf_ptr)[blk_index-1].block_num*jhdr_size;
	off_t prev_block_end = prev_block_start + (*buf_ptr)[blk_index-1].block_size;
	overlap = prev_block_end - block_start;
	if (overlap > 0) {
	    if (overlap % jhdr_size != 0) {
		panic("jnl: do_overlap: overlap with previous entry not a multiple of %d\n", jhdr_size);
	    }

	    // if the previous entry completely overlaps this one, we need to break it into two pieces.
	    if (prev_block_end > block_end) {
		off_t new_num = block_end / jhdr_size;
		size_t new_size = prev_block_end - block_end;

		new_offset = (*buf_ptr)[blk_index-1].jnl_offset + (block_end - prev_block_start);
		
		err = insert_block(jnl, buf_ptr, blk_index, new_num, new_size, new_offset, num_buckets_ptr, num_full_ptr, 0);
		if (err < 0) {
		    panic("jnl: do_overlap: error inserting during pre-overlap\n");
		}
	    }
	    
	    // Regardless, we need to truncate the previous entry to the beginning of the overlap
	    (*buf_ptr)[blk_index-1].block_size = block_start - prev_block_start;
	}
    }

    // then, bail out fast if there's no overlap with the entries that follow
    if (!overwrite && block_end <= (*buf_ptr)[blk_index].block_num*jhdr_size) {
	return 0; // no overlap, no overwrite
    } else if (overwrite && (blk_index + 1 >= *num_full_ptr || block_end <= (*buf_ptr)[blk_index+1].block_num*jhdr_size)) {
	return 1; // simple overwrite
    }
    
    // Otherwise, find all cases of total and partial overlap. We use the special
    // block_num of -2 to designate entries that are completely overlapped and must
    // be eliminated. The block_num, size, and jnl_offset of partially overlapped
    // entries must be adjusted to keep the array consistent.
    index = blk_index;
    num_to_remove = 0;
    while(index < *num_full_ptr && block_end > (*buf_ptr)[index].block_num*jhdr_size) {
	if (block_end >= ((*buf_ptr)[index].block_num*jhdr_size + (*buf_ptr)[index].block_size)) {
	    (*buf_ptr)[index].block_num = -2; // mark this for deletion
	    num_to_remove++;
	} else {
	    overlap = block_end - (*buf_ptr)[index].block_num*jhdr_size;
	    if (overlap > 0) {
		if (overlap % jhdr_size != 0) {
		    panic("jnl: do_overlap: overlap of %lld is not multiple of %d\n", overlap, jhdr_size);
		}
		
		// if we partially overlap this entry, adjust its block number, jnl offset, and size
		(*buf_ptr)[index].block_num += (overlap / jhdr_size); // make sure overlap is multiple of jhdr_size, or round up
		
		new_offset = (*buf_ptr)[index].jnl_offset + overlap; // check for wrap-around
		if (new_offset >= jnl->jhdr->size) {
		    new_offset = jhdr_size + (new_offset - jnl->jhdr->size);
		}
		(*buf_ptr)[index].jnl_offset = new_offset;
		
		(*buf_ptr)[index].block_size -= overlap; // sanity check for negative value
		if ((*buf_ptr)[index].block_size <= 0) {
		    panic("jnl: do_overlap: after overlap, new block size is invalid (%d)\n", (*buf_ptr)[index].block_size);
		    // return -1; // if above panic is removed, return -1 for error
		}
	    }
	    
	}

	index++;
    }

    // bcopy over any completely overlapped entries, starting at the right (where the above loop broke out)
    index--; // start with the last index used within the above loop
    while(index >= blk_index) {
	if ((*buf_ptr)[index].block_num == -2) {
	    if (index == *num_full_ptr-1) {
		(*buf_ptr)[index].block_num = -1; // it's the last item in the table... just mark as free
	    } else {
		bcopy( (*buf_ptr)+(index+1), (*buf_ptr)+(index), (*num_full_ptr - (index + 1)) * sizeof(struct bucket) );
	    }
	    (*num_full_ptr)--;
	}
	index--;
    }

    // eliminate any stale entries at the end of the table
    for(i=*num_full_ptr; i < (*num_full_ptr + num_to_remove); i++) {
	(*buf_ptr)[i].block_num = -1;
    }
    
    return 0; // if we got this far, we need to insert the entry into the table (rather than overwrite) 
}

// PR-3105942: Coalesce writes to the same block in journal replay
// We coalesce writes by maintaining a dynamic sorted array of physical disk blocks
// to be replayed and the corresponding location in the journal which contains
// the most recent data for those blocks. The array is "played" once the all the
// blocks in the journal have been coalesced. The code for the case of conflicting/
// overlapping writes to a single block is the most dense. Because coalescing can
// disrupt the existing time-ordering of blocks in the journal playback, care
// is taken to catch any overlaps and keep the array consistent. 
static int
add_block(journal *jnl, struct bucket **buf_ptr, off_t block_num, size_t size, size_t offset, int *num_buckets_ptr, int *num_full_ptr)
{
    int blk_index, overwriting;
    
    // on return from lookup_bucket(), blk_index is the index into the table where block_num should be
    // inserted (or the index of the elem to overwrite). 
    blk_index = lookup_bucket( buf_ptr, block_num, *num_full_ptr);
    
    // check if the index is within bounds (if we're adding this block to the end of
    // the table, blk_index will be equal to num_full)
    if (blk_index < 0 || blk_index > *num_full_ptr) {
        //printf("jnl: add_block: trouble adding block to co_buf\n");
	return -1;
    } // else printf("jnl: add_block: adding block 0x%llx at i=%d\n", block_num, blk_index);
    
    // Determine whether we're overwriting an existing entry by checking for overlap
    overwriting = do_overlap(jnl, buf_ptr, blk_index, block_num, size, offset, num_buckets_ptr, num_full_ptr);
    if (overwriting < 0) {
	return -1; // if we got an error, pass it along
    }
        
    // returns the index, or -1 on error
    blk_index = insert_block(jnl, buf_ptr, blk_index, block_num, size, offset, num_buckets_ptr, num_full_ptr, overwriting);
    
    return blk_index;
}

static int
replay_journal(journal *jnl)
{
    int i, ret, orig_checksum, checksum, max_bsize;
    block_list_header *blhdr;
    off_t offset;
    char *buff, *block_ptr=NULL;
    struct bucket *co_buf;
    int num_buckets = STARTING_BUCKETS, num_full;

    // wrap the start ptr if it points to the very end of the journal
    if (jnl->jhdr->start == jnl->jhdr->size) {
		jnl->jhdr->start = jnl->jhdr->jhdr_size;
    }
    if (jnl->jhdr->end == jnl->jhdr->size) {
		jnl->jhdr->end = jnl->jhdr->jhdr_size;
    }

    if (jnl->jhdr->start == jnl->jhdr->end) {
		return 0;
    }

    // allocate memory for the header_block.  we'll read each blhdr into this
    if (kmem_alloc(kernel_map, (vm_offset_t *)&buff, jnl->jhdr->blhdr_size)) {
		printf("jnl: replay_journal: no memory for block buffer! (%d bytes)\n",
			   jnl->jhdr->blhdr_size);
		return -1;
    }

    // allocate memory for the coalesce buffer
    if ((MALLOC(co_buf, struct bucket *, num_buckets*sizeof(struct bucket), M_TEMP, M_WAITOK)) == NULL) {
        printf("jnl: replay_journal: no memory for coalesce buffer!\n");
	return -1;
    }

    // initialize entries
    for(i=0; i < num_buckets; i++) {
        co_buf[i].block_num = -1;
    }
    num_full = 0; // empty at first


    printf("jnl: replay_journal: from: %lld to: %lld (joffset 0x%llx)\n",
		   jnl->jhdr->start, jnl->jhdr->end, jnl->jdev_offset);

    while(jnl->jhdr->start != jnl->jhdr->end) {
		offset = jnl->jhdr->start;
		ret = read_journal_data(jnl, &offset, buff, jnl->jhdr->blhdr_size);
		if (ret != jnl->jhdr->blhdr_size) {
			printf("jnl: replay_journal: Could not read block list header block @ 0x%llx!\n", offset);
			goto bad_replay;
		}

		blhdr = (block_list_header *)buff;
		
		orig_checksum = blhdr->checksum;
		blhdr->checksum = 0;
		if (jnl->flags & JOURNAL_NEED_SWAP) {
			// calculate the checksum based on the unswapped data
			// because it is done byte-at-a-time.
			orig_checksum = SWAP32(orig_checksum);
			checksum = calc_checksum((char *)blhdr, BLHDR_CHECKSUM_SIZE);
			swap_block_list_header(jnl, blhdr);
		} else {
			checksum = calc_checksum((char *)blhdr, BLHDR_CHECKSUM_SIZE);
		}
		if (checksum != orig_checksum) {
		    printf("jnl: replay_journal: bad block list header @ 0x%llx (checksum 0x%x != 0x%x)\n",
			   offset, orig_checksum, checksum);
		    goto bad_replay;
		}
		if (   blhdr->max_blocks <= 0 || blhdr->max_blocks > 2048
			   || blhdr->num_blocks <= 0 || blhdr->num_blocks > blhdr->max_blocks) {
		    printf("jnl: replay_journal: bad looking journal entry: max: %d num: %d\n",
			   blhdr->max_blocks, blhdr->num_blocks);
		    goto bad_replay;
		}
	
		for(i=1; i < blhdr->num_blocks; i++) {
			if (blhdr->binfo[i].bnum < 0 && blhdr->binfo[i].bnum != (off_t)-1) {
				printf("jnl: replay_journal: bogus block number 0x%llx\n", blhdr->binfo[i].bnum);
				goto bad_replay;
			}
		}

		//printf("jnl: replay_journal: adding %d blocks in journal entry @ 0x%llx to co_buf\n", 
		//       blhdr->num_blocks-1, jnl->jhdr->start);
		for(i=1; i < blhdr->num_blocks; i++) {
			int size, ret_val;
			off_t number;

			size = blhdr->binfo[i].bsize;
			number = blhdr->binfo[i].bnum;
			
			// don't add "killed" blocks
			if (number == (off_t)-1) {
			    //printf("jnl: replay_journal: skipping killed fs block (index %d)\n", i);
			} else {
			    // add this bucket to co_buf, coalescing where possible
			    // printf("jnl: replay_journal: adding block 0x%llx\n", number);
			    ret_val = add_block(jnl, &co_buf, number, size, (size_t) offset, &num_buckets, &num_full);
			    
			    if (ret_val == -1) {
				printf("jnl: replay_journal: trouble adding block to co_buf\n");
				goto bad_replay;
			    } // else printf("jnl: replay_journal: added block 0x%llx at i=%d\n", number);
			}
			
			// increment offset
			offset += size;
			
			// check if the last block added puts us off the end of the jnl.
			// if so, we need to wrap to the beginning and take any remainder
			// into account
			//
			if (offset >= jnl->jhdr->size) {
			    offset = jnl->jhdr->jhdr_size + (offset - jnl->jhdr->size);
			}
		}

		
		jnl->jhdr->start += blhdr->bytes_used;
		if (jnl->jhdr->start >= jnl->jhdr->size) {
			// wrap around and skip the journal header block
			jnl->jhdr->start = (jnl->jhdr->start % jnl->jhdr->size) + jnl->jhdr->jhdr_size;
		}
    }


    //printf("jnl: replay_journal: replaying %d blocks\n", num_full);
    
    /*
     * make sure it's at least one page in size, so
     * start max_bsize at PAGE_SIZE
     */
    for (i = 0, max_bsize = PAGE_SIZE; i < num_full; i++) {

            if (co_buf[i].block_num == (off_t)-1)
	            continue;

	    if (co_buf[i].block_size > max_bsize)
	            max_bsize = co_buf[i].block_size;
    }
    /*
     * round max_bsize up to the nearest PAGE_SIZE multiple
     */
    if (max_bsize & (PAGE_SIZE - 1)) {
            max_bsize = (max_bsize + PAGE_SIZE) & ~(PAGE_SIZE - 1);
    }

    if (kmem_alloc(kernel_map, (vm_offset_t *)&block_ptr, max_bsize)) {
	goto bad_replay;
    }
    
    // Replay the coalesced entries in the co-buf
    for(i=0; i < num_full; i++) {
	size_t size = co_buf[i].block_size;
	off_t jnl_offset = (off_t) co_buf[i].jnl_offset;
	off_t number = co_buf[i].block_num;
	
	
	// printf("replaying co_buf[%d]: block 0x%llx, size 0x%x, jnl_offset 0x%llx\n", i, co_buf[i].block_num,
	//      co_buf[i].block_size, co_buf[i].jnl_offset);
	
	if (number == (off_t)-1) {
	    // printf("jnl: replay_journal: skipping killed fs block\n");
	} else {
	    
	    // do journal read, and set the phys. block 
	    ret = read_journal_data(jnl, &jnl_offset, block_ptr, size);
	    if (ret != size) {
		printf("jnl: replay_journal: Could not read journal entry data @ offset 0x%llx!\n", offset);
		goto bad_replay;
	    }
	    	    
	    if (update_fs_block(jnl, block_ptr, number, size) != 0) {
		goto bad_replay;
	    }
	}
    }
    

    // done replaying; update jnl header
    if (write_journal_header(jnl) != 0) {
	goto bad_replay;
    }
    
    // free block_ptr
    kmem_free(kernel_map, (vm_offset_t)block_ptr, max_bsize);
    block_ptr = NULL;
    
    // free the coalesce buffer
    FREE(co_buf, M_TEMP);
    co_buf = NULL;
  
    kmem_free(kernel_map, (vm_offset_t)buff, jnl->jhdr->blhdr_size);
    return 0;

  bad_replay:
    if (block_ptr) {
		kmem_free(kernel_map, (vm_offset_t)block_ptr, max_bsize);
    }
    if (co_buf) {
      FREE(co_buf, M_TEMP);
    }
    kmem_free(kernel_map, (vm_offset_t)buff, jnl->jhdr->blhdr_size);

    return -1;
}


#define DEFAULT_TRANSACTION_BUFFER_SIZE  (128*1024)
//#define DEFAULT_TRANSACTION_BUFFER_SIZE  (256*1024)  // better performance but uses more mem
#define MAX_TRANSACTION_BUFFER_SIZE      (512*1024)

// XXXdbg - so I can change it in the debugger
int def_tbuffer_size = 0;


//
// This function sets the size of the tbuffer and the
// size of the blhdr.  It assumes that jnl->jhdr->size
// and jnl->jhdr->jhdr_size are already valid.
//
static void
size_up_tbuffer(journal *jnl, int tbuffer_size, int phys_blksz)
{
	//
	// one-time initialization based on how much memory 
	// there is in the machine.
	//
	if (def_tbuffer_size == 0) {
		if (mem_size < (256*1024*1024)) {
			def_tbuffer_size = DEFAULT_TRANSACTION_BUFFER_SIZE;
		} else if (mem_size < (512*1024*1024)) {
			def_tbuffer_size = DEFAULT_TRANSACTION_BUFFER_SIZE * 2;
		} else if (mem_size < (1024*1024*1024)) {
			def_tbuffer_size = DEFAULT_TRANSACTION_BUFFER_SIZE * 3;
		} else if (mem_size >= (1024*1024*1024)) {
			def_tbuffer_size = DEFAULT_TRANSACTION_BUFFER_SIZE * 4;
		}
	}

    // size up the transaction buffer... can't be larger than the number
    // of blocks that can fit in a block_list_header block.
    if (tbuffer_size == 0) {
		jnl->tbuffer_size = def_tbuffer_size;
    } else {
		// make sure that the specified tbuffer_size isn't too small
		if (tbuffer_size < jnl->jhdr->blhdr_size * 2) {
			tbuffer_size = jnl->jhdr->blhdr_size * 2;
		}
		// and make sure it's an even multiple of the block size
		if ((tbuffer_size % jnl->jhdr->jhdr_size) != 0) {
			tbuffer_size -= (tbuffer_size % jnl->jhdr->jhdr_size);
		}

		jnl->tbuffer_size = tbuffer_size;
    }

    if (jnl->tbuffer_size > (jnl->jhdr->size / 2)) {
		jnl->tbuffer_size = (jnl->jhdr->size / 2);
    }
    
    if (jnl->tbuffer_size > MAX_TRANSACTION_BUFFER_SIZE) {
		jnl->tbuffer_size = MAX_TRANSACTION_BUFFER_SIZE;
    }

    jnl->jhdr->blhdr_size = (jnl->tbuffer_size / jnl->jhdr->jhdr_size) * sizeof(block_info);
    if (jnl->jhdr->blhdr_size < phys_blksz) {
	jnl->jhdr->blhdr_size = phys_blksz;
    } else if ((jnl->jhdr->blhdr_size % phys_blksz) != 0) {
		// have to round up so we're an even multiple of the physical block size
		jnl->jhdr->blhdr_size = (jnl->jhdr->blhdr_size + (phys_blksz - 1)) & ~(phys_blksz - 1);
    }
}



journal *
journal_create(struct vnode *jvp,
			   off_t         offset,
			   off_t         journal_size,
			   struct vnode *fsvp,
			   size_t        min_fs_blksz,
			   int32_t       flags,
			   int32_t       tbuffer_size,
			   void        (*flush)(void *arg),
			   void         *arg)
{
    journal *jnl;
    int      phys_blksz;
    struct vfs_context context;

    context.vc_proc = current_proc();
    context.vc_ucred = FSCRED;

    /* Get the real physical block size. */
    if (VNOP_IOCTL(jvp, DKIOCGETBLOCKSIZE, (caddr_t)&phys_blksz, 0, &context)) {
		return NULL;
    }

    if (phys_blksz > min_fs_blksz) {
		printf("jnl: create: error: phys blksize %d bigger than min fs blksize %d\n",
			   phys_blksz, min_fs_blksz);
		return NULL;
    }

    if ((journal_size % phys_blksz) != 0) {
		printf("jnl: create: journal size 0x%llx is not an even multiple of block size 0x%x\n",
			   journal_size, phys_blksz);
		return NULL;
    }

    MALLOC_ZONE(jnl, struct journal *, sizeof(struct journal), M_JNL_JNL, M_WAITOK);
    memset(jnl, 0, sizeof(*jnl));

    jnl->jdev         = jvp;
    jnl->jdev_offset  = offset;
    jnl->fsdev        = fsvp;
    jnl->flush        = flush;
    jnl->flush_arg    = arg;
    jnl->flags        = (flags & JOURNAL_OPTION_FLAGS_MASK);
    lck_mtx_init(&jnl->old_start_lock, jnl_mutex_group, jnl_lock_attr);
	
    if (kmem_alloc(kernel_map, (vm_offset_t *)&jnl->header_buf, phys_blksz)) {
		printf("jnl: create: could not allocate space for header buffer (%d bytes)\n", phys_blksz);
		goto bad_kmem_alloc;
    }

    memset(jnl->header_buf, 0, phys_blksz);
    
    jnl->jhdr             = (journal_header *)jnl->header_buf;
    jnl->jhdr->magic      = JOURNAL_HEADER_MAGIC;
    jnl->jhdr->endian     = ENDIAN_MAGIC;
    jnl->jhdr->start      = phys_blksz;    // start at block #1, block #0 is for the jhdr itself
    jnl->jhdr->end        = phys_blksz;
    jnl->jhdr->size       = journal_size;
    jnl->jhdr->jhdr_size  = phys_blksz;
    size_up_tbuffer(jnl, tbuffer_size, phys_blksz);

	jnl->active_start     = jnl->jhdr->start;

    // XXXdbg  - for testing you can force the journal to wrap around
    // jnl->jhdr->start = jnl->jhdr->size - (phys_blksz*3);
    // jnl->jhdr->end   = jnl->jhdr->size - (phys_blksz*3);
    
	lck_mtx_init(&jnl->jlock, jnl_mutex_group, jnl_lock_attr);

    if (write_journal_header(jnl) != 0) {
		printf("jnl: journal_create: failed to write journal header.\n");
		goto bad_write;
    }

    return jnl;


  bad_write:
    kmem_free(kernel_map, (vm_offset_t)jnl->header_buf, phys_blksz);
  bad_kmem_alloc:
    jnl->jhdr = NULL;
	FREE_ZONE(jnl, sizeof(struct journal), M_JNL_JNL);
    return NULL;
}


journal *
journal_open(struct vnode *jvp,
			 off_t         offset,
			 off_t         journal_size,
			 struct vnode *fsvp,
			 size_t        min_fs_blksz,
			 int32_t       flags,
			 int32_t       tbuffer_size,
			 void        (*flush)(void *arg),
			 void         *arg)
{
    journal *jnl;
    int      orig_blksz=0, phys_blksz;
	int      orig_checksum, checksum;
    struct vfs_context context;

    context.vc_proc = current_proc();
    context.vc_ucred = FSCRED;

    /* Get the real physical block size. */
    if (VNOP_IOCTL(jvp, DKIOCGETBLOCKSIZE, (caddr_t)&phys_blksz, 0, &context)) {
		return NULL;
    }

    if (phys_blksz > min_fs_blksz) {
		printf("jnl: create: error: phys blksize %d bigger than min fs blksize %d\n",
			   phys_blksz, min_fs_blksz);
		return NULL;
    }

    if ((journal_size % phys_blksz) != 0) {
		printf("jnl: open: journal size 0x%llx is not an even multiple of block size 0x%x\n",
			   journal_size, phys_blksz);
		return NULL;
    }

    MALLOC_ZONE(jnl, struct journal *, sizeof(struct journal), M_JNL_JNL, M_WAITOK);
    memset(jnl, 0, sizeof(*jnl));

    jnl->jdev         = jvp;
    jnl->jdev_offset  = offset;
    jnl->fsdev        = fsvp;
    jnl->flush        = flush;
    jnl->flush_arg    = arg;
    jnl->flags        = (flags & JOURNAL_OPTION_FLAGS_MASK);
    lck_mtx_init(&jnl->old_start_lock, jnl_mutex_group, jnl_lock_attr);

    if (kmem_alloc(kernel_map, (vm_offset_t *)&jnl->header_buf, phys_blksz)) {
		printf("jnl: create: could not allocate space for header buffer (%d bytes)\n", phys_blksz);
		goto bad_kmem_alloc;
    }

    jnl->jhdr = (journal_header *)jnl->header_buf;
    memset(jnl->jhdr, 0, sizeof(journal_header)+4);

    // we have to set this up here so that do_journal_io() will work
    jnl->jhdr->jhdr_size = phys_blksz;

    if (read_journal_header(jnl, jnl->jhdr, phys_blksz) != phys_blksz) {
		printf("jnl: open: could not read %d bytes for the journal header.\n",
			   phys_blksz);
		goto bad_journal;
    }

	orig_checksum = jnl->jhdr->checksum;
	jnl->jhdr->checksum = 0;

	if (jnl->jhdr->magic == SWAP32(JOURNAL_HEADER_MAGIC)) {
		// do this before the swap since it's done byte-at-a-time
		orig_checksum = SWAP32(orig_checksum);
		checksum = calc_checksum((char *)jnl->jhdr, sizeof(struct journal_header));
		swap_journal_header(jnl);
		jnl->flags |= JOURNAL_NEED_SWAP;
	} else {
		checksum = calc_checksum((char *)jnl->jhdr, sizeof(struct journal_header));
	}

    if (jnl->jhdr->magic != JOURNAL_HEADER_MAGIC && jnl->jhdr->magic != OLD_JOURNAL_HEADER_MAGIC) {
		printf("jnl: open: journal magic is bad (0x%x != 0x%x)\n",
			   jnl->jhdr->magic, JOURNAL_HEADER_MAGIC);
		goto bad_journal;
    }

	// only check if we're the current journal header magic value
	if (jnl->jhdr->magic == JOURNAL_HEADER_MAGIC) {

		if (orig_checksum != checksum) {
			printf("jnl: open: journal checksum is bad (0x%x != 0x%x)\n",
				   orig_checksum, checksum);
				   
			//goto bad_journal;
		}
	}

	// XXXdbg - convert old style magic numbers to the new one
	if (jnl->jhdr->magic == OLD_JOURNAL_HEADER_MAGIC) {
		jnl->jhdr->magic = JOURNAL_HEADER_MAGIC;
	}

    if (phys_blksz != jnl->jhdr->jhdr_size && jnl->jhdr->jhdr_size != 0) {
		printf("jnl: open: phys_blksz %d does not match journal header size %d\n",
			   phys_blksz, jnl->jhdr->jhdr_size);

		orig_blksz = phys_blksz;
		phys_blksz = jnl->jhdr->jhdr_size;
		if (VNOP_IOCTL(jvp, DKIOCSETBLOCKSIZE, (caddr_t)&phys_blksz, FWRITE, &context)) {
			printf("jnl: could not set block size to %d bytes.\n", phys_blksz);
			goto bad_journal;
		}
//		goto bad_journal;
    }

    if (   jnl->jhdr->start <= 0
		   || jnl->jhdr->start > jnl->jhdr->size
		   || jnl->jhdr->start > 1024*1024*1024) {
		printf("jnl: open: jhdr start looks bad (0x%llx max size 0x%llx)\n",
			   jnl->jhdr->start, jnl->jhdr->size);
		goto bad_journal;
    }

    if (   jnl->jhdr->end <= 0
		   || jnl->jhdr->end > jnl->jhdr->size
		   || jnl->jhdr->end > 1024*1024*1024) {
		printf("jnl: open: jhdr end looks bad (0x%llx max size 0x%llx)\n",
			   jnl->jhdr->end, jnl->jhdr->size);
		goto bad_journal;
    }

    if (jnl->jhdr->size > 1024*1024*1024) {
		printf("jnl: open: jhdr size looks bad (0x%llx)\n", jnl->jhdr->size);
		goto bad_journal;
    }

// XXXdbg - can't do these checks because hfs writes all kinds of
//          non-uniform sized blocks even on devices that have a block size
//          that is larger than 512 bytes (i.e. optical media w/2k blocks).
//          therefore these checks will fail and so we just have to punt and
//          do more relaxed checking...
// XXXdbg    if ((jnl->jhdr->start % jnl->jhdr->jhdr_size) != 0) {
    if ((jnl->jhdr->start % 512) != 0) {
		printf("jnl: open: journal start (0x%llx) not a multiple of 512?\n",
			   jnl->jhdr->start);
		goto bad_journal;
    }

//XXXdbg    if ((jnl->jhdr->end % jnl->jhdr->jhdr_size) != 0) {
    if ((jnl->jhdr->end % 512) != 0) {
		printf("jnl: open: journal end (0x%llx) not a multiple of block size (0x%x)?\n",
			   jnl->jhdr->end, jnl->jhdr->jhdr_size);
		goto bad_journal;
    }

    // take care of replaying the journal if necessary
	if (flags & JOURNAL_RESET) {
		printf("jnl: journal start/end pointers reset! (jnl 0x%x; s 0x%llx e 0x%llx)\n",
			   jnl, jnl->jhdr->start, jnl->jhdr->end);
		jnl->jhdr->start = jnl->jhdr->end;
	} else if (replay_journal(jnl) != 0) {
		printf("jnl: journal_open: Error replaying the journal!\n");
		goto bad_journal;
    }

	if (orig_blksz != 0) {
		VNOP_IOCTL(jvp, DKIOCSETBLOCKSIZE, (caddr_t)&orig_blksz, FWRITE, &context);
		phys_blksz = orig_blksz;
		if (orig_blksz < jnl->jhdr->jhdr_size) {
			printf("jnl: open: jhdr_size is %d but orig phys blk size is %d.  switching.\n",
				   jnl->jhdr->jhdr_size, orig_blksz);
				   
			jnl->jhdr->jhdr_size = orig_blksz;
		}
	}

	// make sure this is in sync!
	jnl->active_start = jnl->jhdr->start;

    // set this now, after we've replayed the journal
    size_up_tbuffer(jnl, tbuffer_size, phys_blksz);

	lck_mtx_init(&jnl->jlock, jnl_mutex_group, jnl_lock_attr);

    return jnl;

  bad_journal:
	if (orig_blksz != 0) {
		phys_blksz = orig_blksz;
		VNOP_IOCTL(jvp, DKIOCSETBLOCKSIZE, (caddr_t)&orig_blksz, FWRITE, &context);
	}
    kmem_free(kernel_map, (vm_offset_t)jnl->header_buf, phys_blksz);
  bad_kmem_alloc:
	FREE_ZONE(jnl, sizeof(struct journal), M_JNL_JNL);
    return NULL;    
}

void
journal_close(journal *jnl)
{
    volatile off_t *start, *end;
    int             counter=0;

    CHECK_JOURNAL(jnl);

	// set this before doing anything that would block so that
	// we start tearing things down properly.
	//
	jnl->flags |= JOURNAL_CLOSE_PENDING;

    if (jnl->owner != current_thread()) {
		lock_journal(jnl);
    }

    //
    // only write stuff to disk if the journal is still valid
    //
    if ((jnl->flags & JOURNAL_INVALID) == 0) {

		if (jnl->active_tr) {
			journal_end_transaction(jnl);
		}
		
		// flush any buffered transactions
		if (jnl->cur_tr) {
			transaction *tr = jnl->cur_tr;

			jnl->cur_tr = NULL;
			end_transaction(tr, 1);   // force it to get flushed
		}
    
		//start = &jnl->jhdr->start;
		start = &jnl->active_start;
		end   = &jnl->jhdr->end;
    
		while (*start != *end && counter++ < 500) {
			printf("jnl: close: flushing the buffer cache (start 0x%llx end 0x%llx)\n", *start, *end);
			if (jnl->flush) {
				jnl->flush(jnl->flush_arg);
			}
			tsleep((caddr_t)jnl, PRIBIO, "jnl_close", 1);
		}

		if (*start != *end) {
			printf("jnl: close: buffer flushing didn't seem to flush out all the transactions! (0x%llx - 0x%llx)\n",
				   *start, *end);
		}

		// make sure this is in sync when we close the journal
		jnl->jhdr->start = jnl->active_start;

		// if this fails there's not much we can do at this point...
		write_journal_header(jnl);
    } else {
		// if we're here the journal isn't valid any more.
		// so make sure we don't leave any locked blocks lying around
		printf("jnl: close: journal 0x%x, is invalid.  aborting outstanding transactions\n", jnl);
		if (jnl->active_tr || jnl->cur_tr) {
			transaction *tr;
			if (jnl->active_tr) {
				tr = jnl->active_tr;
				jnl->active_tr = NULL;
			} else {
				tr = jnl->cur_tr;
				jnl->cur_tr = NULL;
			}

			abort_transaction(jnl, tr);
			if (jnl->active_tr || jnl->cur_tr) {
				panic("jnl: close: jnl @ 0x%x had both an active and cur tr\n", jnl);
			}
		}
    }

    free_old_stuff(jnl);

    kmem_free(kernel_map, (vm_offset_t)jnl->header_buf, jnl->jhdr->jhdr_size);
    jnl->jhdr = (void *)0xbeefbabe;

	FREE_ZONE(jnl, sizeof(struct journal), M_JNL_JNL);
}

static void
dump_journal(journal *jnl)
{
    transaction *ctr;

    printf("journal:");
    printf("  jdev_offset %.8llx\n", jnl->jdev_offset);
    printf("  magic: 0x%.8x\n", jnl->jhdr->magic);
    printf("  start: 0x%.8llx\n", jnl->jhdr->start);
    printf("  end:   0x%.8llx\n", jnl->jhdr->end);
    printf("  size:  0x%.8llx\n", jnl->jhdr->size);
    printf("  blhdr size: %d\n", jnl->jhdr->blhdr_size);
    printf("  jhdr size: %d\n", jnl->jhdr->jhdr_size);
    printf("  chksum: 0x%.8x\n", jnl->jhdr->checksum);
    
    printf("  completed transactions:\n");
    for(ctr=jnl->completed_trs; ctr; ctr=ctr->next) {
		printf("    0x%.8llx - 0x%.8llx\n", ctr->journal_start, ctr->journal_end);
    }
}



static off_t
free_space(journal *jnl)
{
    off_t free_space;
	
    if (jnl->jhdr->start < jnl->jhdr->end) {
		free_space = jnl->jhdr->size - (jnl->jhdr->end - jnl->jhdr->start) - jnl->jhdr->jhdr_size;
    } else if (jnl->jhdr->start > jnl->jhdr->end) {
		free_space = jnl->jhdr->start - jnl->jhdr->end;
    } else {
		// journal is completely empty
		free_space = jnl->jhdr->size - jnl->jhdr->jhdr_size;
    }

    return free_space;
}


//
// The journal must be locked on entry to this function.
// The "desired_size" is in bytes.
//
static int
check_free_space(journal *jnl, int desired_size)
{
    int    i, counter=0;

    //printf("jnl: check free space (desired 0x%x, avail 0x%Lx)\n",
//	   desired_size, free_space(jnl));
    
    while (1) {
		int old_start_empty;
		
		if (counter++ == 5000) {
			dump_journal(jnl);
			panic("jnl: check_free_space: buffer flushing isn't working "
				  "(jnl @ 0x%x s %lld e %lld f %lld [active start %lld]).\n", jnl,
				  jnl->jhdr->start, jnl->jhdr->end, free_space(jnl), jnl->active_start);
		}
		if (counter > 7500) {
			printf("jnl: check_free_space: giving up waiting for free space.\n");
			return ENOSPC;
		}

		// make sure there's space in the journal to hold this transaction
		if (free_space(jnl) > desired_size) {
			break;
		}

		//
		// here's where we lazily bump up jnl->jhdr->start.  we'll consume
		// entries until there is enough space for the next transaction.
		//
		old_start_empty = 1;
		lock_oldstart(jnl);
		for(i=0; i < sizeof(jnl->old_start)/sizeof(jnl->old_start[0]); i++) {
			int   counter;

			counter = 0;
			while (jnl->old_start[i] & 0x8000000000000000LL) {
				if (counter++ > 100) {
					panic("jnl: check_free_space: tr starting @ 0x%llx not flushing (jnl 0x%x).\n",
						  jnl->old_start[i], jnl);
				}
				
				unlock_oldstart(jnl);
				if (jnl->flush) {
					jnl->flush(jnl->flush_arg);
				}
				tsleep((caddr_t)jnl, PRIBIO, "check_free_space1", 1);
				lock_oldstart(jnl);
			}

			if (jnl->old_start[i] == 0) {
				continue;
			}

			old_start_empty   = 0;
			jnl->jhdr->start  = jnl->old_start[i];
			jnl->old_start[i] = 0;
			if (free_space(jnl) > desired_size) {
				unlock_oldstart(jnl);
				write_journal_header(jnl);
				lock_oldstart(jnl);
				break;
			}
		}
		unlock_oldstart(jnl);
		
		// if we bumped the start, loop and try again
		if (i < sizeof(jnl->old_start)/sizeof(jnl->old_start[0])) {
			continue;
		} else if (old_start_empty) {
			//
			// if there is nothing in old_start anymore then we can
			// bump the jhdr->start to be the same as active_start
			// since it is possible there was only one very large
			// transaction in the old_start array.  if we didn't do
			// this then jhdr->start would never get updated and we
			// would wind up looping until we hit the panic at the
			// start of the loop.
			//
			jnl->jhdr->start = jnl->active_start;
			write_journal_header(jnl);
			continue;
		}


		// if the file system gave us a flush function, call it to so that
		// it can flush some blocks which hopefully will cause some transactions
		// to complete and thus free up space in the journal.
		if (jnl->flush) {
			jnl->flush(jnl->flush_arg);
		}
	
		// wait for a while to avoid being cpu-bound (this will
		// put us to sleep for 10 milliseconds)
		tsleep((caddr_t)jnl, PRIBIO, "check_free_space2", 1);
    }

    return 0;
}

int
journal_start_transaction(journal *jnl)
{
    int ret;
    transaction *tr;

    CHECK_JOURNAL(jnl);
    
    if (jnl->flags & JOURNAL_INVALID) {
		return EINVAL;
    }

    if (jnl->owner == current_thread()) {
		if (jnl->active_tr == NULL) {
			panic("jnl: start_tr: active_tr is NULL (jnl @ 0x%x, owner 0x%x, current_thread 0x%x\n",
				  jnl, jnl->owner, current_thread());
		}
		jnl->nested_count++;
		return 0;
    }

    lock_journal(jnl);

    if (jnl->owner != NULL || jnl->nested_count != 0 || jnl->active_tr != NULL) {
		panic("jnl: start_tr: owner 0x%x, nested count 0x%x, active_tr 0x%x jnl @ 0x%x\n",
			  jnl->owner, jnl->nested_count, jnl->active_tr, jnl);
    }

    jnl->owner        = current_thread();
    jnl->nested_count = 1;

    free_old_stuff(jnl);

    // make sure there's room in the journal
    if (check_free_space(jnl, jnl->tbuffer_size) != 0) {
		printf("jnl: start transaction failed: no space\n");
		ret = ENOSPC;
		goto bad_start;
    }

    // if there's a buffered transaction, use it.
    if (jnl->cur_tr) {
		jnl->active_tr = jnl->cur_tr;
		jnl->cur_tr    = NULL;

		return 0;
    }

	MALLOC_ZONE(tr, transaction *, sizeof(transaction), M_JNL_TR, M_WAITOK);
    memset(tr, 0, sizeof(transaction));

    tr->tbuffer_size = jnl->tbuffer_size;

    if (kmem_alloc(kernel_map, (vm_offset_t *)&tr->tbuffer, tr->tbuffer_size)) {
		FREE_ZONE(tr, sizeof(transaction), M_JNL_TR);
		printf("jnl: start transaction failed: no tbuffer mem\n");
		ret = ENOMEM;
		goto bad_start;
    }

    // journal replay code checksum check depends on this.
    memset(tr->tbuffer, 0, BLHDR_CHECKSUM_SIZE);

    tr->blhdr = (block_list_header *)tr->tbuffer;
    tr->blhdr->max_blocks = (jnl->jhdr->blhdr_size / sizeof(block_info)) - 1;
    tr->blhdr->num_blocks = 1;      // accounts for this header block
    tr->blhdr->bytes_used = jnl->jhdr->blhdr_size;

    tr->num_blhdrs  = 1;
    tr->total_bytes = jnl->jhdr->blhdr_size;
    tr->jnl         = jnl;

    jnl->active_tr    = tr;

    // printf("jnl: start_tr: owner 0x%x new tr @ 0x%x\n", jnl->owner, tr);

    return 0;

  bad_start:
	jnl->owner        = NULL;
	jnl->nested_count = 0;
	unlock_journal(jnl);
	return ret;
}


int
journal_modify_block_start(journal *jnl, struct buf *bp)
{
    transaction *tr;
    
    CHECK_JOURNAL(jnl);

    if (jnl->flags & JOURNAL_INVALID) {
		return EINVAL;
    }

    // XXXdbg - for debugging I want this to be true.  later it may
    //          not be necessary.
    if ((buf_flags(bp) & B_META) == 0) {
		panic("jnl: modify_block_start: bp @ 0x%x is not a meta-data block! (jnl 0x%x)\n", bp, jnl);
    }

    tr = jnl->active_tr;
    CHECK_TRANSACTION(tr);

    if (jnl->owner != current_thread()) {
		panic("jnl: modify_block_start: called w/out a transaction! jnl 0x%x, owner 0x%x, curact 0x%x\n",
			  jnl, jnl->owner, current_thread());
    }

    free_old_stuff(jnl);

    //printf("jnl: mod block start (bp 0x%x vp 0x%x l/blkno %qd/%qd bsz %d; total bytes %d)\n",
    //   bp, buf_vnode(bp), buf_lblkno(bp), buf_blkno(bp), buf_size(bp), tr->total_bytes);

    // can't allow blocks that aren't an even multiple of the
    // underlying block size.
    if ((buf_size(bp) % jnl->jhdr->jhdr_size) != 0) {
		panic("jnl: mod block start: bufsize %d not a multiple of block size %d\n",
			  buf_size(bp), jnl->jhdr->jhdr_size);
		return -1;
    }

    // make sure that this transaction isn't bigger than the whole journal
    if (tr->total_bytes+buf_size(bp) >= (jnl->jhdr->size - jnl->jhdr->jhdr_size)) {
		panic("jnl: transaction too big (%d >= %lld bytes, bufsize %d, tr 0x%x bp 0x%x)\n",
			  tr->total_bytes, (tr->jnl->jhdr->size - jnl->jhdr->jhdr_size), buf_size(bp), tr, bp);
		return -1;
    }

    // if the block is dirty and not already locked we have to write
    // it out before we muck with it because it has data that belongs
    // (presumably) to another transaction.
    //
    if ((buf_flags(bp) & (B_DELWRI | B_LOCKED)) == B_DELWRI) {

		if (buf_flags(bp) & B_ASYNC) {
			panic("modify_block_start: bp @ 0x% has async flag set!\n", bp);
		}

		// this will cause it to not be buf_brelse()'d
                buf_setflags(bp, B_NORELSE);
		VNOP_BWRITE(bp);
    }
    buf_setflags(bp, B_LOCKED);
	
    return 0;
}

int
journal_modify_block_abort(journal *jnl, struct buf *bp)
{
    transaction *tr;
	block_list_header *blhdr;
	int i, j;
    
    CHECK_JOURNAL(jnl);

    tr = jnl->active_tr;
	
	//
	// if there's no active transaction then we just want to
	// call buf_brelse() and return since this is just a block
	// that happened to be modified as part of another tr.
	//
	if (tr == NULL) {
		buf_brelse(bp);
		return 0;
	}

    if (jnl->flags & JOURNAL_INVALID) {
		return EINVAL;
    }

    CHECK_TRANSACTION(tr);
    
    if (jnl->owner != current_thread()) {
		panic("jnl: modify_block_abort: called w/out a transaction! jnl 0x%x, owner 0x%x, curact 0x%x\n",
			  jnl, jnl->owner, current_thread());
    }

    free_old_stuff(jnl);

    // printf("jnl: modify_block_abort: tr 0x%x bp 0x%x\n", jnl->active_tr, bp);

    // first check if it's already part of this transaction
    for(blhdr=tr->blhdr; blhdr; blhdr=(block_list_header *)((long)blhdr->binfo[0].bnum)) {
		for(i=1; i < blhdr->num_blocks; i++) {
			if (bp == blhdr->binfo[i].bp) {
				if (buf_size(bp) != blhdr->binfo[i].bsize) {
					panic("jnl: bp @ 0x%x changed size on me! (%d vs. %d, jnl 0x%x)\n",
						  bp, buf_size(bp), blhdr->binfo[i].bsize, jnl);
				}
				break;
			}
		}

		if (i < blhdr->num_blocks) {
			break;
		}
    }

	//
	// if blhdr is null, then this block has only had modify_block_start
	// called on it as part of the current transaction.  that means that
	// it is ok to clear the LOCKED bit since it hasn't actually been
	// modified.  if blhdr is non-null then modify_block_end was called
	// on it and so we need to keep it locked in memory.
	//
	if (blhdr == NULL) { 
	          buf_clearflags(bp, B_LOCKED);
	}

    buf_brelse(bp);
    return 0;
}


int
journal_modify_block_end(journal *jnl, struct buf *bp)
{
    int                i, j, tbuffer_offset;
    char              *blkptr;
    block_list_header *blhdr, *prev=NULL;
    transaction       *tr;

    CHECK_JOURNAL(jnl);

    if (jnl->flags & JOURNAL_INVALID) {
		return EINVAL;
    }

    tr = jnl->active_tr;
    CHECK_TRANSACTION(tr);

    if (jnl->owner != current_thread()) {
		panic("jnl: modify_block_end: called w/out a transaction! jnl 0x%x, owner 0x%x, curact 0x%x\n",
			  jnl, jnl->owner, current_thread());
    }

    free_old_stuff(jnl);

    //printf("jnl: mod block end:  (bp 0x%x vp 0x%x l/blkno %qd/%qd bsz %d, total bytes %d)\n", 
    //   bp, buf_vnode(bp), buf_lblkno(bp), buf_blkno(bp), buf_size(bp), tr->total_bytes);

    if ((buf_flags(bp) & B_LOCKED) == 0) {
		panic("jnl: modify_block_end: bp 0x%x not locked! jnl @ 0x%x\n", bp, jnl);
    }
	 
    // first check if it's already part of this transaction
    for(blhdr=tr->blhdr; blhdr; prev=blhdr,blhdr=(block_list_header *)((long)blhdr->binfo[0].bnum)) {
		tbuffer_offset = jnl->jhdr->blhdr_size;

		for(i=1; i < blhdr->num_blocks; i++) {
			if (bp == blhdr->binfo[i].bp) {
				if (buf_size(bp) != blhdr->binfo[i].bsize) {
					panic("jnl: bp @ 0x%x changed size on me! (%d vs. %d, jnl 0x%x)\n",
						  bp, buf_size(bp), blhdr->binfo[i].bsize, jnl);
				}
				break;
			}
			tbuffer_offset += blhdr->binfo[i].bsize;
		}

		if (i < blhdr->num_blocks) {
			break;
		}
    }

    if (blhdr == NULL
		&& prev
		&& (prev->num_blocks+1) <= prev->max_blocks
		&& (prev->bytes_used+buf_size(bp)) <= tr->tbuffer_size) {
		blhdr = prev;
    } else if (blhdr == NULL) {
		block_list_header *nblhdr;

		if (prev == NULL) {
			panic("jnl: modify block end: no way man, prev == NULL?!?, jnl 0x%x, bp 0x%x\n", jnl, bp);
		}

		// we got to the end of the list, didn't find the block and there's
		// no room in the block_list_header pointed to by prev
	
		// we allocate another tbuffer and link it in at the end of the list
		// through prev->binfo[0].bnum.  that's a skanky way to do things but
		// avoids having yet another linked list of small data structures to manage.

		if (kmem_alloc(kernel_map, (vm_offset_t *)&nblhdr, tr->tbuffer_size)) {
			panic("jnl: end_tr: no space for new block tr @ 0x%x (total bytes: %d)!\n",
				  tr, tr->total_bytes);
		}

		// journal replay code checksum check depends on this.
		memset(nblhdr, 0, BLHDR_CHECKSUM_SIZE);

		// initialize the new guy
		nblhdr->max_blocks = (jnl->jhdr->blhdr_size / sizeof(block_info)) - 1;
		nblhdr->num_blocks = 1;      // accounts for this header block
		nblhdr->bytes_used = jnl->jhdr->blhdr_size;
	    
		tr->num_blhdrs++;
		tr->total_bytes += jnl->jhdr->blhdr_size;

		// then link him in at the end
		prev->binfo[0].bnum = (off_t)((long)nblhdr);

		// and finally switch to using the new guy
		blhdr          = nblhdr;
		tbuffer_offset = jnl->jhdr->blhdr_size;
		i              = 1;
    }


    if ((i+1) > blhdr->max_blocks) {
		panic("jnl: modify_block_end: i = %d, max_blocks %d\n", i, blhdr->max_blocks);
    }

    // copy the data into the in-memory transaction buffer
    blkptr = (char *)&((char *)blhdr)[tbuffer_offset];
    memcpy(blkptr, buf_dataptr(bp), buf_size(bp));

    // if this is true then this is a new block we haven't seen
    if (i >= blhdr->num_blocks) {
                int	bsize;
		vnode_t	vp;

		vp = buf_vnode(bp);
		vnode_ref(vp);
		bsize = buf_size(bp);

		blhdr->binfo[i].bnum  = (off_t)(buf_blkno(bp));
		blhdr->binfo[i].bsize = bsize;
		blhdr->binfo[i].bp    = bp;

		blhdr->bytes_used += bsize;
		tr->total_bytes   += bsize;

		blhdr->num_blocks++;
    }
    buf_bdwrite(bp);

    return 0;
}

int
journal_kill_block(journal *jnl, struct buf *bp)
{
    int                i;
    int		       bflags;
    block_list_header *blhdr;
    transaction       *tr;

    CHECK_JOURNAL(jnl);

    if (jnl->flags & JOURNAL_INVALID) {
		return EINVAL;
    }

    tr = jnl->active_tr;
    CHECK_TRANSACTION(tr);

    if (jnl->owner != current_thread()) {
		panic("jnl: modify_block_end: called w/out a transaction! jnl 0x%x, owner 0x%x, curact 0x%x\n",
			  jnl, jnl->owner, current_thread());
    }

    free_old_stuff(jnl);

    bflags = buf_flags(bp);

    if ( !(bflags & B_LOCKED))
            panic("jnl: modify_block_end: called with bp not B_LOCKED");

    /*
     * bp must be BL_BUSY and B_LOCKED
     */
    // first check if it's already part of this transaction
    for(blhdr=tr->blhdr; blhdr; blhdr=(block_list_header *)((long)blhdr->binfo[0].bnum)) {

		for(i=1; i < blhdr->num_blocks; i++) {
			if (bp == blhdr->binfo[i].bp) {
			        vnode_t vp;

				buf_clearflags(bp, B_LOCKED);

				// this undoes the vnode_ref() in journal_modify_block_end()
				vp = buf_vnode(bp);
				vnode_rele_ext(vp, 0, 1);

				// if the block has the DELWRI and FILTER bits sets, then
				// things are seriously weird.  if it was part of another
				// transaction then journal_modify_block_start() should
				// have force it to be written.
				//
				//if ((bflags & B_DELWRI) && (bflags & B_FILTER)) {
				//	panic("jnl: kill block: this defies all logic! bp 0x%x\n", bp);
				//} else {
					tr->num_killed += buf_size(bp);
				//}
				blhdr->binfo[i].bp   = NULL;
				blhdr->binfo[i].bnum = (off_t)-1;

				buf_brelse(bp);

				break;
			}
		}

		if (i < blhdr->num_blocks) {
			break;
		}
    }

    return 0;
}


static int
journal_binfo_cmp(void *a, void *b)
{
    block_info *bi_a = (struct block_info *)a;
    block_info *bi_b = (struct block_info *)b;
    daddr64_t res;

    if (bi_a->bp == NULL) {
		return 1;
    }
    if (bi_b->bp == NULL) {
		return -1;
    }

    // don't have to worry about negative block
    // numbers so this is ok to do.
    //
    res = (buf_blkno(bi_a->bp) - buf_blkno(bi_b->bp));

    return (int)res;
}


static int
end_transaction(transaction *tr, int force_it)
{
    int                 i, j, ret, amt;
    errno_t		errno;
    off_t               end;
    journal            *jnl = tr->jnl;
    struct buf         *bp;
    block_list_header  *blhdr=NULL, *next=NULL;

	if (jnl->cur_tr) {
		panic("jnl: jnl @ 0x%x already has cur_tr 0x%x, new tr: 0x%x\n",
			  jnl, jnl->cur_tr, tr);
	}

    // if there weren't any modified blocks in the transaction
    // just save off the transaction pointer and return.
    if (tr->total_bytes == jnl->jhdr->blhdr_size) {
		jnl->cur_tr = tr;
		return 0;
    }

    // if our transaction buffer isn't very full, just hang
    // on to it and don't actually flush anything.  this is
    // what is known as "group commit".  we will flush the
    // transaction buffer if it's full or if we have more than
    // one of them so we don't start hogging too much memory.
    //
    if (   force_it == 0
		   && (jnl->flags & JOURNAL_NO_GROUP_COMMIT) == 0 
		   && tr->num_blhdrs < 3
		   && (tr->total_bytes <= ((tr->tbuffer_size*tr->num_blhdrs) - tr->tbuffer_size/8))) {

		jnl->cur_tr = tr;
		return 0;
    }


    // if we're here we're going to flush the transaction buffer to disk.
    // make sure there is room in the journal first.
    check_free_space(jnl, tr->total_bytes);

    // range check the end index
    if (jnl->jhdr->end <= 0 || jnl->jhdr->end > jnl->jhdr->size) {
		panic("jnl: end_transaction: end is bogus 0x%llx (sz 0x%llx)\n",
			  jnl->jhdr->end, jnl->jhdr->size);
    }

    // this transaction starts where the current journal ends
    tr->journal_start = jnl->jhdr->end;
    end               = jnl->jhdr->end;

	//
	// if the first entry in old_start[] isn't free yet, loop calling the
	// file system flush routine until it is (or we panic).
	//
	i = 0;
	lock_oldstart(jnl);
	while ((jnl->old_start[0] & 0x8000000000000000LL) != 0) {
		if (jnl->flush) {
			unlock_oldstart(jnl);

			if (jnl->flush) {
				jnl->flush(jnl->flush_arg);
			}

			// yield the cpu so others can get in to clear the lock bit
			(void)tsleep((void *)jnl, PRIBIO, "jnl-old-start-sleep", 1);

			lock_oldstart(jnl);
		}
		if (i++ >= 500) {
			panic("jnl: transaction that started at 0x%llx is not completing! jnl 0x%x\n",
				  jnl->old_start[0] & (~0x8000000000000000LL), jnl);
		}
	}

	//
	// slide everyone else down and put our latest guy in the last
	// entry in the old_start array
	//
	memcpy(&jnl->old_start[0], &jnl->old_start[1], sizeof(jnl->old_start)-sizeof(jnl->old_start[0]));
	jnl->old_start[sizeof(jnl->old_start)/sizeof(jnl->old_start[0]) - 1] = tr->journal_start | 0x8000000000000000LL;

	unlock_oldstart(jnl);


    // for each block, make sure that the physical block # is set
    for(blhdr=tr->blhdr; blhdr; blhdr=next) {

		for(i=1; i < blhdr->num_blocks; i++) {
		        daddr64_t blkno;
		        daddr64_t lblkno;
			struct vnode *vp;

			bp = blhdr->binfo[i].bp;
			if (bp == NULL) {   // only true if a block was "killed" 
				if (blhdr->binfo[i].bnum != (off_t)-1) {
					panic("jnl: inconsistent binfo (NULL bp w/bnum %lld; jnl @ 0x%x, tr 0x%x)\n",
						  blhdr->binfo[i].bnum, jnl, tr);
				}
				continue;
			}
			vp = buf_vnode(bp);
			blkno = buf_blkno(bp);
			lblkno = buf_lblkno(bp);

			if (vp == NULL && lblkno == blkno) {
			    printf("jnl: end_tr: bad news! bp @ 0x%x w/null vp and l/blkno = %qd/%qd.  aborting the transaction (tr 0x%x jnl 0x%x).\n",
					  bp, lblkno, blkno, tr, jnl);
			    goto bad_journal;
			}
	    
			// if the lblkno is the same as blkno and this bp isn't
			// associated with the underlying file system device then
			// we need to call bmap() to get the actual physical block.
			//
			if ((lblkno == blkno) && (vp != jnl->fsdev)) {
			        off_t	f_offset;
				size_t 	contig_bytes;

				if (VNOP_BLKTOOFF(vp, lblkno, &f_offset)) {
					printf("jnl: end_tr: vnop_blktooff failed @ 0x%x, jnl 0x%x\n", bp, jnl);
					goto bad_journal;
				}
				if (VNOP_BLOCKMAP(vp, f_offset, buf_count(bp), &blkno, &contig_bytes, NULL, 0, NULL)) {
					printf("jnl: end_tr: can't blockmap the bp @ 0x%x, jnl 0x%x\n", bp, jnl);
					goto bad_journal;
				}
				if ((uint32_t)contig_bytes < buf_count(bp)) {
					printf("jnl: end_tr: blk not physically contiguous on disk@ 0x%x, jnl 0x%x\n", bp, jnl);
					goto bad_journal;
				}
				buf_setblkno(bp, blkno);
			}
			// update this so we write out the correct physical block number!
			blhdr->binfo[i].bnum = (off_t)(blkno);
		}

		next = (block_list_header *)((long)blhdr->binfo[0].bnum);
    }
    
    for(blhdr=tr->blhdr; blhdr; blhdr=(block_list_header *)((long)blhdr->binfo[0].bnum)) {

		amt = blhdr->bytes_used;

		blhdr->checksum = 0;
		blhdr->checksum = calc_checksum((char *)blhdr, BLHDR_CHECKSUM_SIZE);
	
		ret = write_journal_data(jnl, &end, blhdr, amt);
		if (ret != amt) {
			printf("jnl: end_transaction: only wrote %d of %d bytes to the journal!\n",
				   ret, amt);

			goto bad_journal;
		}
    }

    jnl->jhdr->end  = end;    // update where the journal now ends
    tr->journal_end = end;    // the transaction ends here too
    if (tr->journal_start == 0 || tr->journal_end == 0) {
		panic("jnl: end_transaction: bad tr journal start/end: 0x%llx 0x%llx\n",
			  tr->journal_start, tr->journal_end);
    }

    if (write_journal_header(jnl) != 0) {
		goto bad_journal;
    }

    //
    // setup for looping through all the blhdr's.  we null out the
    // tbuffer and blhdr fields so that they're not used any more.
    //
    blhdr       = tr->blhdr;
    tr->tbuffer = NULL;
    tr->blhdr   = NULL;

    // the buffer_flushed_callback will only be called for the 
    // real blocks that get flushed so we have to account for 
    // the block_list_headers here.
    //
    tr->num_flushed = tr->num_blhdrs * jnl->jhdr->blhdr_size;

    // for each block, set the iodone callback and unlock it
    for(; blhdr; blhdr=next) {

		// we can re-order the buf ptrs because everything is written out already
		qsort(&blhdr->binfo[1], blhdr->num_blocks-1, sizeof(block_info), journal_binfo_cmp);

		for(i=1; i < blhdr->num_blocks; i++) {
			if (blhdr->binfo[i].bp == NULL) {
				continue;
			}

			errno = buf_meta_bread(buf_vnode(blhdr->binfo[i].bp),
							 buf_lblkno(blhdr->binfo[i].bp),
							 buf_size(blhdr->binfo[i].bp),
							 NOCRED,
							 &bp);
			if (errno == 0 && bp != NULL) {
				struct vnode *save_vp;
				void *cur_filter;

				if (bp != blhdr->binfo[i].bp) {
					panic("jnl: end_tr: got back a different bp! (bp 0x%x should be 0x%x, jnl 0x%x\n",
						  bp, blhdr->binfo[i].bp, jnl);
				}

				if ((buf_flags(bp) & (B_LOCKED|B_DELWRI)) != (B_LOCKED|B_DELWRI)) {
					if (jnl->flags & JOURNAL_CLOSE_PENDING) {
					    buf_clearflags(bp, B_LOCKED);
					    buf_brelse(bp);
						continue;
					} else {
						panic("jnl: end_tr: !!!DANGER!!! bp 0x%x flags (0x%x) not LOCKED & DELWRI\n", bp, buf_flags(bp));
					}
				}
				save_vp = buf_vnode(bp);

				buf_setfilter(bp, buffer_flushed_callback, tr, &cur_filter, NULL);

				if (cur_filter) {
					panic("jnl: bp @ 0x%x (blkno %qd, vp 0x%x) has non-null iodone (0x%x) buffflushcb 0x%x\n",
						  bp, buf_blkno(bp), save_vp, cur_filter, buffer_flushed_callback);
				}
				buf_clearflags(bp, B_LOCKED);

				// kicking off the write here helps performance
				buf_bawrite(bp);
				// XXXdbg this is good for testing: buf_bdwrite(bp);
				//buf_bdwrite(bp);
				
				// this undoes the vnode_ref() in journal_modify_block_end()
				vnode_rele_ext(save_vp, 0, 1);
			} else {
				printf("jnl: end_transaction: could not find block %Ld vp 0x%x!\n",
					   blhdr->binfo[i].bnum, blhdr->binfo[i].bp);
				if (bp) {
					buf_clearflags(bp, B_LOCKED);
					buf_brelse(bp);
				}
			}
		}

		next = (block_list_header *)((long)blhdr->binfo[0].bnum);

		// we can free blhdr here since we won't need it any more
		blhdr->binfo[0].bnum = 0xdeadc0de;
		kmem_free(kernel_map, (vm_offset_t)blhdr, tr->tbuffer_size);
    }

    //printf("jnl: end_tr: tr @ 0x%x, jnl-blocks: 0x%llx - 0x%llx. exit!\n",
    //   tr, tr->journal_start, tr->journal_end);
    return 0;


  bad_journal:
    jnl->flags |= JOURNAL_INVALID;
    jnl->old_start[sizeof(jnl->old_start)/sizeof(jnl->old_start[0]) - 1] &= ~0x8000000000000000LL;
    abort_transaction(jnl, tr);
    return -1;
}

static void
abort_transaction(journal *jnl, transaction *tr)
{
    int                i;
    errno_t		errno;
    block_list_header *blhdr, *next;
    struct buf        *bp;
    struct vnode      *save_vp;

    // for each block list header, iterate over the blocks then
    // free up the memory associated with the block list.
    //
    // for each block, clear the lock bit and release it.
    //
    for(blhdr=tr->blhdr; blhdr; blhdr=next) {

		for(i=1; i < blhdr->num_blocks; i++) {
			if (blhdr->binfo[i].bp == NULL) {
				continue;
			}
			if ( (buf_vnode(blhdr->binfo[i].bp) == NULL) ||
			     !(buf_flags(blhdr->binfo[i].bp) & B_LOCKED) ) {
			        continue;
			}

			errno = buf_meta_bread(buf_vnode(blhdr->binfo[i].bp),
							 buf_lblkno(blhdr->binfo[i].bp),
							 buf_size(blhdr->binfo[i].bp),
							 NOCRED,
							 &bp);
			if (errno == 0) {
				if (bp != blhdr->binfo[i].bp) {
					panic("jnl: abort_tr: got back a different bp! (bp 0x%x should be 0x%x, jnl 0x%x\n",
						  bp, blhdr->binfo[i].bp, jnl);
				}

				// releasing a bp marked invalid
				// also clears the locked and delayed state
				buf_markinvalid(bp);
				save_vp = buf_vnode(bp);

				buf_brelse(bp);

				vnode_rele_ext(save_vp, 0, 1);
			} else {
				printf("jnl: abort_tr: could not find block %Ld vp 0x%x!\n",
					   blhdr->binfo[i].bnum, blhdr->binfo[i].bp);
				if (bp) {
					buf_brelse(bp);
				}
			}
		}

		next = (block_list_header *)((long)blhdr->binfo[0].bnum);

		// we can free blhdr here since we won't need it any more
		blhdr->binfo[0].bnum = 0xdeadc0de;
		kmem_free(kernel_map, (vm_offset_t)blhdr, tr->tbuffer_size);
    }

    tr->tbuffer     = NULL;
    tr->blhdr       = NULL;
    tr->total_bytes = 0xdbadc0de;
	FREE_ZONE(tr, sizeof(transaction), M_JNL_TR);
}


int
journal_end_transaction(journal *jnl)
{
    int ret;
	transaction *tr;
    
    CHECK_JOURNAL(jnl);

	if ((jnl->flags & JOURNAL_INVALID) && jnl->owner == NULL) {
		return 0;
	}

    if (jnl->owner != current_thread()) {
		panic("jnl: end_tr: I'm not the owner! jnl 0x%x, owner 0x%x, curact 0x%x\n",
			  jnl, jnl->owner, current_thread());
    }

    free_old_stuff(jnl);

    jnl->nested_count--;
    if (jnl->nested_count > 0) {
		return 0;
    } else if (jnl->nested_count < 0) {
		panic("jnl: jnl @ 0x%x has negative nested count (%d). bad boy.\n", jnl, jnl->nested_count);
    }
    
    if (jnl->flags & JOURNAL_INVALID) {
		if (jnl->active_tr) {
			if (jnl->cur_tr != NULL) {
				panic("jnl: journal @ 0x%x has active tr (0x%x) and cur tr (0x%x)\n",
					  jnl, jnl->active_tr, jnl->cur_tr);
			}
	    
			tr             = jnl->active_tr;
			jnl->active_tr = NULL;
			abort_transaction(jnl, tr);
		}

		jnl->owner = NULL;
		unlock_journal(jnl);

		return EINVAL;
    }

    tr = jnl->active_tr;
    CHECK_TRANSACTION(tr);

    // clear this out here so that when check_free_space() calls
    // the FS flush function, we don't panic in journal_flush()
    // if the FS were to call that.  note: check_free_space() is
    // called from end_transaction().
    // 
    jnl->active_tr = NULL;
    ret = end_transaction(tr, 0);

    jnl->owner = NULL;
    unlock_journal(jnl);

    return ret;
}


int
journal_flush(journal *jnl)
{
    int need_signal = 0;
    
    CHECK_JOURNAL(jnl);
    
    if (jnl->flags & JOURNAL_INVALID) {
		return -1;
    }

    if (jnl->owner != current_thread()) {
		int ret;

		lock_journal(jnl);
		need_signal = 1;
    }

    free_old_stuff(jnl);

    // if we're not active, flush any buffered transactions
    if (jnl->active_tr == NULL && jnl->cur_tr) {
		transaction *tr = jnl->cur_tr;

		jnl->cur_tr = NULL;
		end_transaction(tr, 1);   // force it to get flushed
    }

    if (need_signal) {
		unlock_journal(jnl);
    }

    return 0;
}

int
journal_active(journal *jnl)
{
    if (jnl->flags & JOURNAL_INVALID) {
		return -1;
    }
    
    return (jnl->active_tr == NULL) ? 0 : 1;
}

void *
journal_owner(journal *jnl)
{
    return jnl->owner;
}

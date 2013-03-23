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
/* Copyright 1998 Apple Computer, Inc.
 *
 * UFS byte swapping routines to make a big endian file system useful on a
 * little endian machine.
 *
 */

#ifdef	KERNEL_PRIVATE

#ifndef _UFS_BYTE_ORDER_H_
#define _UFS_BYTE_ORDER_H_

#include <sys/appleapiopts.h>

#ifdef __APPLE_API_PRIVATE
#include <rev_endian_fs.h>
#include <sys/vnode.h>
#include <sys/buf.h>
#include <ufs/ufs/quota.h>
#include <ufs/ufs/inode.h>
#include <ufs/ffs/fs.h>

void byte_swap_longlongs(unsigned long long *, int);
void byte_swap_ints(int *, int);
void byte_swap_shorts(short *, int);

/* void byte_swap_superblock(struct fs *); */
int byte_swap_sbin(struct fs *);
void byte_swap_sbout(struct fs *);
void byte_swap_csum(struct csum *);
void byte_swap_ocylgroup(struct cg *);
void byte_swap_cgin(struct cg *, struct fs *);
void byte_swap_cgout(struct cg *, struct fs *);

void byte_swap_inode_in(struct dinode *, struct inode *);
void byte_swap_inode_out(struct inode *, struct dinode *);

void byte_swap_dir_block_in(char *, int);
void byte_swap_dir_block_out(buf_t);
void byte_swap_direct(struct direct *);
void byte_swap_dirtemplate_in(struct dirtemplate *);
void byte_swap_minidir_in(struct direct *);

#endif /* __APPLE_API_PRIVATE */
#endif /* _UFS_BYTE_ORDER_H_ */
#endif	/* KERNEL_PRIVATE */

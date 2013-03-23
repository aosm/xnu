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
/*
 * This header file describes the structures of the file format for "fat"
 * architecture specific file (wrapper design).  At the begining of the file
 * there is one fat_header structure followed by a number of fat_arch
 * structures.  For each architecture in the file, specified by a pair of
 * cputype and cpusubtype, the fat_header describes the file offset, file
 * size and alignment in the file of the architecture specific member.
 * The padded bytes in the file to place each member on it's specific alignment
 * are defined to be read as zeros and can be left as "holes" if the file system
 * can support them as long as they read as zeros.
 *
 * All structures defined here are always written and read to/from disk
 * in big-endian order.
 */

/*
 * <mach/machine.h> is needed here for the cpu_type_t and cpu_subtype_t types
 * and contains the constants for the possible values of these types.
 */
#include <mach/machine.h>
#include <architecture/byte_order.h>

#define FAT_MAGIC	0xcafebabe
#define FAT_CIGAM	NXSwapLong(FAT_MAGIC)

struct fat_header {
	unsigned long	magic;		/* FAT_MAGIC */
	unsigned long	nfat_arch;	/* number of structs that follow */
};

struct fat_arch {
	cpu_type_t	cputype;	/* cpu specifier (int) */
	cpu_subtype_t	cpusubtype;	/* machine specifier (int) */
	unsigned long	offset;		/* file offset to this object file */
	unsigned long	size;		/* size of this object file */
	unsigned long	align;		/* alignment as a power of 2 */
};

#ifdef KERNEL

#include <mach/mach_types.h>

struct vnode;

/* XXX return type should be load_return_t, but mach_loader.h is not in scope */
int fatfile_getarch_affinity(struct vnode *vp, vm_offset_t data_ptr,
		struct fat_arch *archret, int affinity);
int fatfile_getarch(struct vnode *vp, vm_offset_t data_ptr,
		struct fat_arch *archret);
int fatfile_getarch_with_bits(struct vnode *vp, integer_t archbits,
		vm_offset_t data_ptr, struct fat_arch *archret);

#endif	/* KERNEL */

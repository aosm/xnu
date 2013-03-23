/*
 * Copyright (c) 2000-2004 Apple Computer, Inc. All rights reserved.
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
/*	$NetBSD: iso.h,v 1.9 1995/01/18 09:23:19 mycroft Exp $	*/

/*-
 * Copyright (c) 1994
 *	The Regents of the University of California.  All rights reserved.
 *
 * This code is derived from software contributed to Berkeley
 * by Pace Willisson (pace@blitz.com).  The Rock Ridge Extension
 * Support code is derived from software contributed to Berkeley
 * by Atsushi Murai (amurai@spec.co.jp).
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
 *	@(#)iso.h	8.4 (Berkeley) 12/5/94
 */
#ifndef _ISO_H_
#define _ISO_H_

#include <sys/appleapiopts.h>

#ifdef __APPLE_API_PRIVATE
#define ISODCL(from, to) (to - from + 1)

struct iso_volume_descriptor {
	char type            [ISODCL(1,1)]; /* 711 */
	char volume_desc_id  [ISODCL(2,6)];
	char version         [ISODCL(7,7)];
	char data            [ISODCL(8,2048)];
};

/* volume descriptor types */
#define ISO_VD_BOOT            0
#define ISO_VD_PRIMARY         1
#define ISO_VD_SUPPLEMENTARY   2
#define ISO_VD_PARTITION       3
#define ISO_VD_END           255

#define ISO_STANDARD_ID "CD001"
#define ISO_ECMA_ID     "CDW01"
#define ISO_XA_ID		"CD-XA001" 	/* XA style disk signature */
#define ISO9660SIGNATURE 0x4147		/* for getattrlist ATTR_VOL_SIGNATURE */

/* Universal Character Set implementation levels (for Joliet) */
#define ISO_UCS2_Level_1	"%/@"	/* No combining chars */
#define ISO_UCS2_Level_2	"%/C"	/* Combining chars allowed with restrictions */
#define ISO_UCS2_Level_3	"%/E"	/* Combining chars allowed, no restrictions */

#define UCS_SEPARATOR1	0x002e
#define UCS_SEPARATOR2	0x003b

#define ISO_DFLT_VOLUME_ID "ISO_9660_CD"

/* pathconf filename lengths */
#define ISO_NAMEMAX		(31+1)
#define ISO_JOLIET_NAMEMAX	(64*3)
#define ISO_RRIP_NAMEMAX	255

/* Finder flags, from Technical Note 40 */
#define		fLockedBit		0x8000
#define		fInvisibleBit	0x4000
#define		fHasBundleBit	0x2000
#define		fSystemBit		0x1000
#define		fNoCopyBit		0x0800
#define		fBusyBit		0x0400
#define		fChangedBit		0x0200
#define		fInitedBit		0x0100
#define		fCachedBit		0x0080
#define		fSharedBit		0x0040
#define		fAlwaysBit		0x0020		/* always switch-launch */
#define		fNeverBit		0x0010		/* never switch-launch */
#define		fOwnApplBit		0x0002
#define		fOnDesktopBit	0x0001

#define		EXTFNDRINFOSIZE		16

struct finder_info {
	unsigned long		fdType;
	unsigned long		fdCreator;
	unsigned short		fdFlags;
	struct {
	    short	v;		/* file's location */
	    short	h;
	} fdLocation;
	unsigned short		fdReserved;
};

struct iso_primary_descriptor {
	char type			[ISODCL (  1,   1)]; /* 711 */
	char volume_desc_id		[ISODCL (  2,   6)];
	char version			[ISODCL (  7,   7)]; /* 711 */
	char flags			[ISODCL (  8,   8)]; /* SVD only */
	char system_id			[ISODCL (  9,  40)]; /* achars */
	char volume_id			[ISODCL ( 41,  72)]; /* dchars */
	char unused2			[ISODCL ( 73,  80)];
	char volume_space_size		[ISODCL ( 81,  88)]; /* 733 */
	char escape_seq			[ISODCL ( 89, 120)]; /* SVD only */
	char volume_set_size		[ISODCL (121, 124)]; /* 723 */
	char volume_sequence_number	[ISODCL (125, 128)]; /* 723 */
	char logical_block_size		[ISODCL (129, 132)]; /* 723 */
	char path_table_size		[ISODCL (133, 140)]; /* 733 */
	char type_l_path_table		[ISODCL (141, 144)]; /* 731 */
	char opt_type_l_path_table	[ISODCL (145, 148)]; /* 731 */
	char type_m_path_table		[ISODCL (149, 152)]; /* 732 */
	char opt_type_m_path_table	[ISODCL (153, 156)]; /* 732 */
	char root_directory_record	[ISODCL (157, 190)]; /* 9.1 */
	char volume_set_id		[ISODCL (191, 318)]; /* dchars */
	char publisher_id		[ISODCL (319, 446)]; /* achars */
	char preparer_id		[ISODCL (447, 574)]; /* achars */
	char application_id		[ISODCL (575, 702)]; /* achars */
	char copyright_file_id		[ISODCL (703, 739)]; /* 7.5 dchars */
	char abstract_file_id		[ISODCL (740, 776)]; /* 7.5 dchars */
	char bibliographic_file_id	[ISODCL (777, 813)]; /* 7.5 dchars */
	char creation_date		[ISODCL (814, 830)]; /* 8.4.26.1 */
	char modification_date		[ISODCL (831, 847)]; /* 8.4.26.1 */
	char expiration_date		[ISODCL (848, 864)]; /* 8.4.26.1 */
	char effective_date		[ISODCL (865, 881)]; /* 8.4.26.1 */
	char file_structure_version	[ISODCL (882, 882)]; /* 711 */
	char unused4			[ISODCL (883, 883)];
	char application_data1		[ISODCL (884, 1024)];
	char CDXASignature			[ISODCL (1025, 1032)];
	char CDXAResv				[ISODCL (1033, 1050)];
	char application_data2		[ISODCL (1051, 1395)];
};
#define ISO_DEFAULT_BLOCK_SIZE		2048

/* from HighSierra.h in MacOS land */
typedef struct 
{
	char 	signature		[ISODCL (1, 2)]; 	/* x42 x41 - 'BA' signature */
	u_char 	systemUseID		[ISODCL (3, 3)]; 	/* 02 = no icon, 03 = icon, 04 = icon + bundle */
	u_char 	fileType		[ISODCL (4, 7)]; 	/* such as 'TEXT' or 'STAK' */
	u_char 	fileCreator		[ISODCL (8, 11)]; 	/* such as 'hscd' or 'WILD' */
	u_char 	finderFlags		[ISODCL (12, 13)]; 	/* optional for type 06 */
} AppleExtension;

typedef struct 
{
	char 	signature		[ISODCL (1, 2)]; 	/* x41 x41 - 'AA' signature */
	u_char 	OSULength		[ISODCL (3, 3)]; 	/* optional SystemUse length (size of this struct) */
	u_char 	systemUseID		[ISODCL (4, 4)]; 	/* 1 = ProDOS 2 = HFS */
	u_char 	fileType		[ISODCL (5, 8)]; 	/* such as 'TEXT' or 'STAK' */
	u_char 	fileCreator		[ISODCL (9, 12)]; 	/* such as 'hscd' or 'WILD' */
	u_char 	finderFlags		[ISODCL (13, 14)]; 	/* only certain bits of this are used */
} NewAppleExtension;

struct iso_directory_record {
	char length			[ISODCL (1, 1)]; /* 711 */
	char ext_attr_length		[ISODCL (2, 2)]; /* 711 */
	u_char extent			[ISODCL (3, 10)]; /* 733 */
	u_char size			[ISODCL (11, 18)]; /* 733 */
	char date			[ISODCL (19, 25)]; /* 7 by 711 */
	char flags			[ISODCL (26, 26)];
	char file_unit_size		[ISODCL (27, 27)]; /* 711 */
	char interleave			[ISODCL (28, 28)]; /* 711 */
	char volume_sequence_number	[ISODCL (29, 32)]; /* 723 */
	char name_len			[ISODCL (33, 33)]; /* 711 */
	char name			[1];			/* XXX */
};
/*
 *  cannot take sizeof(iso_directory_record), because of 
 *	possible alignment
 *	of the last entry (34 instead of 33)
 */
#define ISO_DIRECTORY_RECORD_SIZE	33

/*
 * iso_directory_record.flags for Directory Records (except CD-I discs)
 */
#define existenceBit	0x01	/* Invisible */
#define directoryBit	0x02
#define associatedBit	0x04
#define recordBit		0x08
#define protectionBit	0x10
#define multiextentBit	0x80

struct iso_extended_attributes {
	u_char owner			[ISODCL (1, 4)]; /* 723 */
	u_char group			[ISODCL (5, 8)]; /* 723 */
	u_char perm			[ISODCL (9, 10)]; /* 9.5.3 */
	char ctime			[ISODCL (11, 27)]; /* 8.4.26.1 */
	char mtime			[ISODCL (28, 44)]; /* 8.4.26.1 */
	char xtime			[ISODCL (45, 61)]; /* 8.4.26.1 */
	char ftime			[ISODCL (62, 78)]; /* 8.4.26.1 */
	char recfmt			[ISODCL (79, 79)]; /* 711 */
	char recattr			[ISODCL (80, 80)]; /* 711 */
	u_char reclen			[ISODCL (81, 84)]; /* 723 */
	char system_id			[ISODCL (85, 116)]; /* achars */
	char system_use			[ISODCL (117, 180)];
	char version			[ISODCL (181, 181)]; /* 711 */
	char len_esc			[ISODCL (182, 182)]; /* 711 */
	char reserved			[ISODCL (183, 246)];
	u_char len_au			[ISODCL (247, 250)]; /* 723 */
};

/* CD-ROM Format type */
enum ISO_FTYPE  { ISO_FTYPE_DEFAULT, ISO_FTYPE_9660, ISO_FTYPE_RRIP,
		  ISO_FTYPE_JOLIET, ISO_FTYPE_ECMA };

#ifndef	ISOFSMNT_ROOT
#define	ISOFSMNT_ROOT	0
#endif

struct iso_mnt {
	int im_flags;		/* mount flags */
	int im_flags2;		/* misc flags */	

	struct mount *im_mountp;
	dev_t im_dev;
	struct vnode *im_devvp;

	int logical_block_size;
	int im_bshift;
	int im_bmask;
	int im_sector_size;
	
	int volume_space_size;
	
	char root[ISODCL (157, 190)];
	int root_extent;
	int root_size;
	enum ISO_FTYPE  iso_ftype;
	
	int rr_skip;
	int rr_skip0;

	struct timespec creation_date;				/* needed for getattrlist */
	struct timespec modification_date;			/* needed for getattrlist */
	u_char volume_id[32];						/* name of volume */
	struct vnode *phys_devvp;					/* device for 2352-byte blocks */
	struct CDTOC *toc;							/* the TOC, or NULL for none */
	int video_dir_start;						/* start sector of the "MPEGAV" dir */
	int video_dir_end;							/* sector following end of "MPEGAV" dir */
};

/* bit settings for iso_mnt.im_flags2 */

/*
 *  CD is in XA format. Need this to find where apple extensions 
 *	are in the iso_directory_record
 */
#define	IMF2_IS_CDXA	0x00000001

/* CD is Video CD (version < 2.0) */
#define IMF2_IS_VCD		0x00000002

#define VFSTOISOFS(mp)	((struct iso_mnt *)(vfs_fsprivate(mp)))

#define blkoff(imp, loc)	((loc) & (imp)->im_bmask)
#define lblktosize(imp, blk)	((blk) << (imp)->im_bshift)
#define lblkno(imp, loc)	((loc) >> (imp)->im_bshift)
#define blksize(imp, ip, lbn)	((imp)->logical_block_size)

#define SECTOFF(imp, off)	\
	(off_t)(((off) / (imp)->im_sector_size) * (imp)->im_sector_size)


int cd9660_mount(struct mount *, vnode_t, user_addr_t, vfs_context_t);
int cd9660_start(struct mount *, int, vfs_context_t);
int cd9660_unmount(struct mount *, int, vfs_context_t);
int cd9660_root(struct mount *, struct vnode **, vfs_context_t);
int cd9660_statfs(struct mount *, struct vfsstatfs *, vfs_context_t);
int cd9660_vfs_getattr(struct mount *mp, struct vfs_attr *fsap, vfs_context_t context);
int cd9660_sync(struct mount *, int, vfs_context_t);
int cd9660_vget(struct mount *, ino64_t, struct vnode **, vfs_context_t);
int cd9660_fhtovp(struct mount *, int, unsigned char *, struct vnode **, vfs_context_t);
int cd9660_vptofh(struct vnode *, int *, unsigned char *, vfs_context_t);
int cd9660_init(struct vfsconf *);
int cd9660_mountroot(mount_t, vnode_t, vfs_context_t); 
int cd9660_sysctl(int *, u_int, user_addr_t, size_t *, user_addr_t, size_t, vfs_context_t);

int cd9660_vget_internal(mount_t, ino_t, vnode_t *, vnode_t, struct componentname *,
			 int, struct iso_directory_record *, proc_t);

extern int (**cd9660_vnodeop_p)(void *);
extern int (**cd9660_specop_p)(void *);
#if FIFO
extern int (**cd9660_fifoop_p)(void *);
#endif
extern int (**cd9660_cdxaop_p)(void *);

static __inline int
isonum_711(u_char *p)
{
	return *p;
}

static __inline int
isonum_712(char *p)
{
	return *p;
}

#ifndef UNALIGNED_ACCESS

static __inline int
isonum_723(u_char *p)
{
	return *p|(p[1] << 8);
}

static __inline int
isonum_733(u_char *p)
{
	return *p|(p[1] << 8)|(p[2] << 16)|(p[3] << 24);
}

#else /* UNALIGNED_ACCESS */

#if BYTE_ORDER == LITTLE_ENDIAN

static __inline int
isonum_723(u_char *p)
{
	return *(u_int16t *)p;
}

static __inline int
isonum_733(u_char *p)
{
	return *(u_int32t *)p;
}

#endif

#if BYTE_ORDER == BIG_ENDIAN

static __inline int
isonum_723(u_char *p)
{
	return *(u_int16t *)(p + 2);
}

static __inline int
isonum_733(u_char *p)
{
	return *(u_int32t *)(p + 4);
}

#endif

#endif /* UNALIGNED_ACCESS */

int isofncmp(u_char *fn, int fnlen, u_char *isofn, int isolen);
int ucsfncmp(u_int16_t *, int, u_int16_t *, int);
void isofntrans(u_char *infn, int infnlen, u_char *outfn, u_short *outfnlen,
		int original, int assoc);
void ucsfntrans(u_int16_t *, int, u_char *, u_short *, int, int);
int attrcalcsize(struct attrlist *attrlist);
struct iso_node;
void packcommonattr(struct attrlist *alist, struct iso_node *ip,
		    void **attrbufptrptr, void **varbufptrptr);
void packdirattr(struct attrlist *alist, struct iso_node *ip,
		 void **attrbufptrptr, void **varbufptrptr);
void packfileattr(struct attrlist *alist, struct iso_node *ip,
		  void **attrbufptrptr, void **varbufptrptr);
void packattrblk(struct attrlist *alist, struct vnode *vp,
				  void **attrbufptrptr, void **varbufptrptr);


/*
 * Associated files have a leading "._".
 */
#define	ASSOCCHAR1	'.'
#define	ASSOCCHAR2	'_'

/*
 * This header is prepended on media tracks, such as Video CD MPEG files.
 */
struct riff_header {
	char riff[4];			// "RIFF"
	u_int32_t fileSize;		// little endian file size, not including this field or sig
	char cdxa[4];			// "CDXA"
	char fmt[4];			// "fmt "
	u_int32_t fmtSize;		// always 16 (XXX this is an assumption)
	char fmtData[16];		// CDXA extension of ISO directory entry, padded to 16 bytes
	char data[4];			// "data"
	u_int32_t dataSize;		// number of sectors * 2352, little endian
};

#define CDXA_SECTOR_SIZE	2352


/*
 * AppleDouble constants
 */
#define APPLEDOUBLE_MAGIC    0x00051607
#define APPLEDOUBLE_VERSION  0x00020000

#define APPLEDOUBLE_DATAFORK     1
#define APPLEDOUBLE_RESFORK      2
#define APPLEDOUBLE_FINDERINFO   9

/*
 * Note that  68k alignment is needed to make sure that the first
 * AppleDoubleEntry (after the numEntries below) is *immediately*
 * after the numEntries, and not padded by 2 bytes.
 *
 * Consult RFC 1740 for details on AppleSingle/AppleDouble formats.
 */
#pragma options align=mac68k

struct apple_double_entry {
	u_int32_t  entryID;
	u_int32_t  offset;
	u_int32_t  length;
};
typedef struct apple_double_entry apple_double_entry_t;

struct apple_double_header {
	u_int32_t  magic;
	u_int32_t  version;
	u_int8_t  filler[16];
	u_int16_t  count;
	apple_double_entry_t  entries[2];	/* FinderInfo + ResourceFork */
	struct finder_info finfo;
};
typedef struct apple_double_header apple_double_header_t;

#define ADH_SIZE  4096
#define ADH_BLKS  2

#pragma options align=reset

#endif /* __APPLE_API_PRIVATE */
#endif /* ! _ISO_H_ */

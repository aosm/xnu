/*
 * Copyright (c) 2000-2002 Apple Computer, Inc. All rights reserved.
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
#ifndef _PPC_SAVEAREA_H_
#define _PPC_SAVEAREA_H_

#ifndef ASSEMBLER

#include <sys/appleapiopts.h>

#ifdef __APPLE_API_PRIVATE

#ifdef	MACH_KERNEL_PRIVATE
#include <mach/ppc/vm_types.h>
typedef struct savearea_comm {

/*
 *	The following fields are common to all saveareas and are used to manage individual
 *	contexts.
 *	
 *	Fields that start with "save" are part of the individual saveareas.  Those that
 *	start with "sac" pertain to the free pool stuff and are valid only on the first slot
 *	in the page.
 */


/*	Keep the save_prev, sac_next, and sac_prev in these positions, some assemble code depends upon it to
 *	match up with fields in saveanchor.
 */
	struct savearea	*save_prev;					/* The address of the previous (or next) savearea */
	unsigned int	*sac_next;					/* Points to next savearea page that has a free slot  - real */
	unsigned int	*sac_prev;					/* Points to previous savearea page that has a free slot  - real */
	unsigned int	save_flags;					/* Various flags */
	unsigned int	save_level;					/* Context ID */
	unsigned int	save_time[2];				/* Context save time - for debugging or performance */
	struct thread_activation	*save_act;		/* Associated activation  */

/*                                                 0x20 */

	unsigned int	sac_vrswap;					/* XOR mask to swap V to R or vice versa */
	unsigned int	sac_alloc;					/* Bitmap of allocated slots */
	unsigned int	sac_flags;					/* Various flags */
	unsigned int	save_misc0;					/* Various stuff */
	unsigned int	save_misc1;					/* Various stuff */
	unsigned int	save_misc2;					/* Various stuff */
	unsigned int	save_misc3;					/* Various stuff */
	unsigned int	save_misc4;					/* Various stuff */

	unsigned int	save_040[8];				/* Fill 32 bytes */

												/* offset 0x0060 */
} savearea_comm;
#endif

#ifdef BSD_KERNEL_PRIVATE
typedef struct savearea_comm {
	unsigned int	save_000[24];
} savearea_comm;
#endif

#if	defined(MACH_KERNEL_PRIVATE) || defined(BSD_KERNEL_PRIVATE)
/*
 *	This type of savearea contains all of the general context.
 */
 
typedef struct savearea {

	savearea_comm	save_hdr;					/* Stuff common to all saveareas */

	unsigned int	save_060[8];				/* Fill 32 bytes */
												/* offset 0x0080 */
	unsigned int 	save_r0;
	unsigned int 	save_r1;
	unsigned int 	save_r2;
	unsigned int 	save_r3;
	unsigned int 	save_r4;
	unsigned int 	save_r5;
	unsigned int 	save_r6;
	unsigned int 	save_r7;
	
												/* offset 0x0A0 */
	unsigned int 	save_r8;
	unsigned int 	save_r9;
	unsigned int 	save_r10;
	unsigned int 	save_r11;
	unsigned int 	save_r12;
	unsigned int 	save_r13;
	unsigned int 	save_r14;
	unsigned int 	save_r15;
	
												/* offset 0x0C0 */
	unsigned int 	save_r16;
	unsigned int 	save_r17;
	unsigned int 	save_r18;
	unsigned int 	save_r19;
	unsigned int 	save_r20;
	unsigned int 	save_r21;
	unsigned int 	save_r22;
	unsigned int 	save_r23;
	
												/* offset 0x0E0 */
	unsigned int 	save_r24;
	unsigned int 	save_r25;
	unsigned int 	save_r26;	
	unsigned int 	save_r27;
	unsigned int 	save_r28;
	unsigned int 	save_r29;
	unsigned int 	save_r30;
	unsigned int 	save_r31;
	
												/* offset 0x100 */
	unsigned int 	save_srr0;
	unsigned int 	save_srr1;
	unsigned int	save_cr;
	unsigned int 	save_xer;
	unsigned int 	save_lr;
	unsigned int 	save_ctr;
	unsigned int 	save_dar;
	unsigned int 	save_dsisr;
			

												/* offset 0x120 */
	unsigned int	save_vscr[4];	
	unsigned int	save_fpscrpad;
	unsigned int	save_fpscr;
	unsigned int	save_exception;
	unsigned int	save_vrsave;

												/* offset 0x140 */
	unsigned int 	save_sr0;
	unsigned int 	save_sr1;
	unsigned int 	save_sr2;
	unsigned int 	save_sr3;
	unsigned int 	save_sr4;
	unsigned int 	save_sr5;
	unsigned int 	save_sr6;
	unsigned int 	save_sr7;

												/* offset 0x160 */
	unsigned int 	save_sr8;
	unsigned int 	save_sr9;
	unsigned int 	save_sr10;
	unsigned int 	save_sr11;
	unsigned int 	save_sr12;
	unsigned int 	save_sr13;
	unsigned int 	save_sr14;
	unsigned int 	save_sr15;

												/* offset 0x180 */
	unsigned int	save_180[8];
	unsigned int	save_1A0[8];
	unsigned int	save_1C0[8];
	unsigned int	save_1E0[8];
	unsigned int	save_200[8];
	unsigned int	save_220[8];
	unsigned int	save_240[8];
	unsigned int	save_260[8];

												/* offset 0x280 */
} savearea;


/*
 *	This type of savearea contains all of the floating point context.
 */
 
typedef struct savearea_fpu {

	savearea_comm	save_hdr;					/* Stuff common to all saveareas */

	unsigned int	save_060[8];				/* Fill 32 bytes */
												/* offset 0x0080 */
	double			save_fp0;
	double			save_fp1;
	double			save_fp2;
	double			save_fp3;

	double			save_fp4;
	double			save_fp5;
	double			save_fp6;
	double			save_fp7;

	double			save_fp8;
	double			save_fp9;
	double			save_fp10;
	double			save_fp11;
	
	double			save_fp12;
	double			save_fp13;
	double			save_fp14;
	double			save_fp15;
	
	double			save_fp16;
	double			save_fp17;
	double			save_fp18;
	double			save_fp19;

	double			save_fp20;
	double			save_fp21;
	double			save_fp22;
	double			save_fp23;
	
	double			save_fp24;
	double			save_fp25;
	double			save_fp26;
	double			save_fp27;
	
	double			save_fp28;
	double			save_fp29;
	double			save_fp30;
	double			save_fp31;
												/* offset 0x180 */
	unsigned int	save_180[8];
	unsigned int	save_1A0[8];
	unsigned int	save_1C0[8];
	unsigned int	save_1E0[8];
	unsigned int	save_200[8];
	unsigned int	save_220[8];
	unsigned int	save_240[8];
	unsigned int	save_260[8];

												/* offset 0x280 */
} savearea_fpu;

	

/*
 *	This type of savearea contains all of the vector context.
 */
 
typedef struct savearea_vec {

	savearea_comm	save_hdr;					/* Stuff common to all saveareas */

	unsigned int	save_060[7];				/* Fill 32 bytes */
	unsigned int	save_vrvalid;				/* Valid registers in saved context */

												/* offset 0x0080 */
	unsigned int	save_vr0[4];
	unsigned int	save_vr1[4];
	unsigned int	save_vr2[4];
	unsigned int	save_vr3[4];
	unsigned int	save_vr4[4];
	unsigned int	save_vr5[4];
	unsigned int	save_vr6[4];
	unsigned int	save_vr7[4];
	unsigned int	save_vr8[4];
	unsigned int	save_vr9[4];
	unsigned int	save_vr10[4];
	unsigned int	save_vr11[4];
	unsigned int	save_vr12[4];
	unsigned int	save_vr13[4];
	unsigned int	save_vr14[4];
	unsigned int	save_vr15[4];
	unsigned int	save_vr16[4];
	unsigned int	save_vr17[4];
	unsigned int	save_vr18[4];
	unsigned int	save_vr19[4];
	unsigned int	save_vr20[4];
	unsigned int	save_vr21[4];
	unsigned int	save_vr22[4];
	unsigned int	save_vr23[4];
	unsigned int	save_vr24[4];
	unsigned int	save_vr25[4];
	unsigned int	save_vr26[4];
	unsigned int	save_vr27[4];
	unsigned int	save_vr28[4];
	unsigned int	save_vr29[4];
	unsigned int	save_vr30[4];
	unsigned int	save_vr31[4];

												/* offset 0x280 */
} savearea_vec;
#endif /* MACH_KERNEL_PRIVATE || BSD_KERNEL_PRIVATE */

#ifdef	MACH_KERNEL_PRIVATE

struct Saveanchor {

/*	
 *	Note that this force aligned in aligned_data.s and must be in V=R storage.
 *	Also, all addresses in chains are physical.  This structure can only be 
 *	updated with translation and interrupts disabled. This is because it is 
 *	locked during exception processing and if we were to take a PTE miss while the
 *	lock were held, well, that would be very bad now wouldn't it? 
 */

	unsigned int	savelock;				/* Lock word for savearea free list manipulation */
	unsigned int	*savepoolfwd;			/* Forward anchor for the free pool */
	unsigned int	*savepoolbwd;			/* Backward anchor for the free pool */
	volatile unsigned int	savefree;		/* Anchor for the global free list */
	volatile unsigned int	savefreecnt;	/* Number of saveareas on global free list */
	volatile int	saveadjust;				/* If 0 number of saveareas is ok, otherwise number to change (positive means grow, negative means shrink */
	volatile int	saveinuse;				/* Number of areas in use counting those on the local free list */
	volatile int	savetarget;				/* Number of savearea's needed */
	int				savemaxcount;			/* Maximum saveareas ever allocated */


};


#define sac_cnt		(4096 / sizeof(savearea))	/* Number of saveareas per page */
#define sac_empty	(0xFFFFFFFF << (32 - sac_cnt))	/* Mask with all entries empty */
#define sac_perm	0x40000000				/* Page permanently assigned */
#define sac_permb	1						/* Page permanently assigned - bit position */

#define LocalSaveTarget	(((8 + sac_cnt - 1) / sac_cnt) * sac_cnt)	/* Target for size of local savearea free list */
#define LocalSaveMin	(LocalSaveTarget / 2)	/* Min size of local savearea free list before we grow */
#define LocalSaveMax	(LocalSaveTarget * 2)	/* Max size of local savearea free list before we trim */

#define FreeListMin		(2 * LocalSaveTarget * NCPUS)	/* Always make sure there are enough to fill local list twice per processor */
#define SaveLowHysteresis	LocalSaveTarget	/* The number off from target before we adjust upwards */
#define SaveHighHysteresis	FreeListMin		/* The number off from target before we adjust downwards */
#define InitialSaveAreas 	(2 * FreeListMin)	/* The number of saveareas to make at boot time */
#define InitialSaveTarget	FreeListMin		/* The number of saveareas for an initial target. This should be the minimum ever needed. */
#define	InitialSaveBloks	(InitialSaveAreas + sac_cnt - 1) / sac_cnt	/* The number of savearea blocks to allocate at boot */


void 			save_release(struct savearea *);	/* Release a save area  */
struct savectl	*save_dequeue(void);			/* Find and dequeue one that is all empty */
unsigned int	save_queue(struct savearea *);	/* Add a new savearea block to the free list */
struct savearea	*save_get(void);				/* Obtains a savearea from the free list (returns virtual address) */
struct savearea	*save_get_phys(void);			/* Obtains a savearea from the free list (returns physical address) */
struct savearea	*save_alloc(void);				/* Obtains a savearea and allocates blocks if needed */
struct savearea	*save_cpv(struct savearea *);	/* Converts a physical savearea address to virtual */
void			save_ret(struct savearea *);	/* Returns a savearea to the free list */
void			save_ret_phys(struct savearea *);	/* Returns a savearea to the free list */
void			save_adjust(void);				/* Adjust size of the global free list */
struct savearea_comm	*save_trim_freet(void);			/* Remove free pages from savearea pool */

#endif /* MACH_KERNEL_PRIVATE */
#endif /* __APPLE_API_PRIVATE */

#endif /* ndef ASSEMBLER */

#define SAVattach	0x80000000				/* Savearea has valid context */
#define SAVrststk	0x00010000				/* Indicates that the current stack should be reset to empty */
#define SAVsyscall	0x00020000				/* Indicates that the savearea is associated with a syscall */
#define SAVredrive	0x00040000				/* Indicates that the low-level fault handler associated */
#define SAVtype		0x0000FF00				/* Shows type of savearea */
#define SAVtypeshft	8						/* Shift to position type */
#define SAVempty	0x86					/* Savearea is on free list */
#define SAVgeneral	0x01					/* Savearea contains general context */
#define SAVfloat	0x02					/* Savearea contains floating point context */
#define SAVvector	0x03					/* Savearea contains vector context */

#endif /* _PPC_SAVEAREA_H_ */

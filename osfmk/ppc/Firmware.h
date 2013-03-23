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
 * @OSF_FREE_COPYRIGHT@
 */
/*
 * @APPLE_FREE_COPYRIGHT@
 */

/*
 * Here be the firmware's public interfaces
 * Lovingly crafted by Bill Angell using traditional methods
*/

#ifndef _FIRMWARE_H_
#define _FIRMWARE_H_

#ifndef __ppc__
#error This file is only useful on PowerPC.
#endif

/*
 *	This routine is used to write debug output to either the modem or printer port.
 *	parm 1 is printer (0) or modem (1); parm 2 is ID (printed directly); parm 3 converted to hex
 */

void dbgDisp(unsigned int port, unsigned int id, unsigned int data);
void dbgLog(unsigned int d0, unsigned int d1, unsigned int d2, unsigned int d3);
void dbgLog2(unsigned int type, unsigned int p1, unsigned int p2);
void dbgDispLL(unsigned int port, unsigned int id, unsigned int data);
void fwSCCinit(unsigned int port);

extern void dbgTrace(unsigned int item1, unsigned int item2, unsigned int item3);
#if 0		/* (TEST/DEBUG) - eliminate inline */
extern __inline__ void dbgTrace(unsigned int item1, unsigned int item2, unsigned int item3) {
 
 		__asm__ volatile("mr   r3,%0" : : "r" (item1) : "r3");
 		__asm__ volatile("mr   r4,%0" : : "r" (item2) : "r4");
 		__asm__ volatile("mr   r5,%0" : : "r" (item3) : "r5");
        __asm__ volatile("lis  r0,hi16(CutTrace)" : : : "r0");
        __asm__ volatile("ori  r0,r0,lo16(CutTrace)" : : : "r0");
        __asm__ volatile("sc");
		return;
}
#endif

extern void DoPreempt(void);
extern __inline__ void DoPreempt(void) {
        __asm__ volatile("lis  r0,hi16(DoPreemptCall)" : : : "r0");
        __asm__ volatile("ori  r0,r0,lo16(DoPreemptCall)" : : : "r0");
        __asm__ volatile("sc");
		return;
}

extern void CreateFakeIO(void);
extern __inline__ void CreateFakeIO(void) {
		__asm__ volatile("lis  r0,hi16(CreateFakeIOCall)" : : : "r0");
		__asm__ volatile("ori  r0,r0,lo16(CreateFakeIOCall)" : : : "r0");
		__asm__ volatile("sc");
		return;
}

extern void CreateFakeDEC(void);
extern __inline__ void CreateFakeDEC(void) {
        __asm__ volatile("lis  r0,hi16(CreateFakeDECCall)" : : : "r0");
        __asm__ volatile("ori  r0,r0,lo16(CreateFakeDECCall)" : : : "r0");
		__asm__ volatile("sc");
		return;
}

extern void CreateShutdownCTX(void);
extern __inline__ void CreateShutdownCTX(void) {
        __asm__ volatile("lis  r0,hi16(CreateShutdownCTXCall)" : : : "r0");
        __asm__ volatile("ori  r0,r0,lo16(CreateShutdownCTXCall)" : : : "r0");
		__asm__ volatile("sc");
		return;
}

typedef struct Boot_Video bootBumbleC;

extern void StoreReal(unsigned int val, unsigned int addr);
extern void ReadReal(unsigned int raddr, unsigned int *vaddr);
extern void ClearReal(unsigned int addr, unsigned int lgn);
extern void LoadDBATs(unsigned int *bat);
extern void LoadIBATs(unsigned int *bat);
extern void stFloat(unsigned int *addr);
extern int stVectors(unsigned int *addr);
extern int stSpecrs(unsigned int *addr);
extern unsigned int LLTraceSet(unsigned int tflags);
extern void GratefulDebInit(bootBumbleC *boot_video_info);
extern void GratefulDebDisp(unsigned int coord, unsigned int data);
extern void checkNMI(void);

typedef struct GDWorkArea {			/* Grateful Deb work area one per processor */

/*	Note that a lot of info is duplicated for each processor */

	unsigned int GDsave[32];		/* Save area for registers */
	
	unsigned int GDfp0[2];
	unsigned int GDfp1[2];
	unsigned int GDfp2[2];
	unsigned int GDfp3[2];
	
	unsigned int GDtop;				/* Top pixel of CPU's window */
	unsigned int GDleft;			/* Left pixel of CPU's window */
	unsigned int GDtopleft;			/* Physical address of top left in frame buffer */
	unsigned int GDrowbytes;		/* Bytes per row */
	unsigned int GDrowchar;			/* Bytes per row of characters plus leading */
	unsigned int GDdepth;			/* Bits per pixel */
	unsigned int GDcollgn;			/* Column width in bytes */
	unsigned int GDready;			/* We are ready to go */
	unsigned int GDfiller[16];		/* Fill it up to a 256 byte boundary */
	
	unsigned int GDrowbuf1[128];	/* Buffer to an 8 character row */
	unsigned int GDrowbuf2[128];	/* Buffer to an 8 character row */

} GDWorkArea;
#define GDfontsize 16
#define GDdispcols 2

#endif /* _FIRMWARE_H_ */

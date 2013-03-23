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
 *
 *		Hardware specific performance counters
 */
#ifndef _HW_COUNTERS_H_
#define _HW_COUNTERS_H_

#ifndef __ppc__
#error This file is only useful on PowerPC.
#endif

#pragma pack(4)								/* Make sure the structure stays as we defined it */

typedef struct hw_counters {	

	unsigned int hw_InVains; 				/* In vain */
	unsigned int hw_Resets;					/* Reset */
	unsigned int hw_MachineChecks;			/* Machine check */
	unsigned int hw_DSIs; 					/* DSIs */
	unsigned int hw_ISIs; 					/* ISIs */
	unsigned int hw_Externals; 				/* Externals */
	unsigned int hw_Alignments; 			/* Alignment */
	unsigned int hw_Programs; 				/* Program */
	unsigned int hw_FloatPointUnavailable; 	/* Floating point */
	unsigned int hw_Decrementers; 			/* Decrementer */
	unsigned int hw_IOErrors; 				/* I/O error */
	unsigned int hw_rsvd0; 					/* Reserved */
	unsigned int hw_SystemCalls; 			/* System call */
	unsigned int hw_Traces; 				/* Trace */
	unsigned int hw_FloatingPointAssists; 	/* Floating point assist */
	unsigned int hw_PerformanceMonitors; 	/* Performance monitor */
	unsigned int hw_Altivecs; 				/* VMX */
	unsigned int hw_rsvd1; 					/* Reserved */
	unsigned int hw_rsvd2; 					/* Reserved */
	unsigned int hw_rsvd3; 					/* Reserved */
	unsigned int hw_InstBreakpoints; 		/* Instruction breakpoint */
	unsigned int hw_SystemManagements; 		/* System management */
	unsigned int hw_AltivecAssists; 		/* Altivec Assist */
	unsigned int hw_Thermal;				/* Thermals */
	unsigned int hw_rsvd5; 					/* Reserved */
	unsigned int hw_rsvd6; 					/* Reserved */
	unsigned int hw_rsvd7; 					/* Reserved */
	unsigned int hw_rsvd8;					/* Reserved */
	unsigned int hw_rsvd9; 					/* Reserved */
	unsigned int hw_rsvd10; 				/* Reserved */
	unsigned int hw_rsvd11; 				/* Reserved */
	unsigned int hw_rsvd12; 				/* Reserved */
	unsigned int hw_rsvd13; 				/* Reserved */
	unsigned int hw_Trace601;				/* Trace */
	unsigned int hw_SIGPs; 					/* SIGP */
	unsigned int hw_Preemptions; 			/* Preemption */
	unsigned int hw_ContextSwitchs;			/* Context switch */
	unsigned int hw_Shutdowns;				/* Shutdowns */
	unsigned int hw_Chokes;					/* System ABENDs */
	unsigned int hw_DataSegments;			/* Data Segment Interruptions */
	unsigned int hw_InstructionSegments;	/* Instruction Segment Interruptions */
	unsigned int hw_SoftPatches;			/* Soft Patch interruptions */
	unsigned int hw_Maintenances;			/* Maintenance interruptions */
	unsigned int hw_Instrumentations;		/* Instrumentation interruptions */
	unsigned int hw_rsvd14;					/* Reswerved */
	
	unsigned int hw_spare[19];				/* Pad to 256 bytes */

} hw_counters;
#pragma pack()

extern hw_counters hw_counts(NCPUS);

#endif /* _HW_COUNTERS_H_ */

/*
 * Copyright (c) 1999, 2000-2001 Apple Computer, Inc. All rights reserved.
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
	File:		smf.h

	Contains:	Secure malloc/free API.

	Written by:	Doug Mitchell

	Copyright: (c) 2000 by Apple Computer, Inc., all rights reserved.

	Change History (most recent first):

		02/10/00	dpm		Created, based on Counterpane's Yarrow code. 
 
*/

#ifndef _YARROW_SMF_H_
#define _YARROW_SMF_H_

#if defined(__cplusplus)
extern "C" {
#endif

/* smf.h */

	/*  
	Header file for secure malloc and free routines used by the Counterpane
	PRNG. Use this code to set up a memory-mapped file out of the system 
	paging file, allocate and free memory from it, and then return
	the memory to the system registry after having securely overwritten it.
	Details of the secure overwrite can be found in Gutmann 1996 (Usenix).
	Trying to explain it here will cause my head to begin to hurt.
	Ari Benbasat (pigsfly@unixg.ubc.ca)
	*/



#if		defined(macintosh) || defined(__APPLE__)
#include "macOnly.h"
#define MMPTR	void *

#ifndef SMFAPI 
#define SMFAPI 
#endif

#else	/* original Yarrow */

/* Declare HOOKSAPI as __declspec(dllexport) before
   including this file in the actual DLL */
#ifndef SMFAPI 
#define SMFAPI __declspec(dllimport)
#endif
#define MMPTR	BYTE

#endif /* macintosh */


#define MM_NULL	0

/* Function forward declarations */
SMFAPI void mmInit();
SMFAPI MMPTR mmMalloc(DWORD request);
SMFAPI void mmFree(MMPTR ptrnum);
SMFAPI LPVOID mmGetPtr(MMPTR ptrnum);
SMFAPI void mmReturnPtr(MMPTR ptrnum);
#if	0
SMFAPI void mmFreePtr(LPVOID ptr);
#endif

#if defined(__cplusplus)
}
#endif

#endif	/* _YARROW_SMF_H_*/

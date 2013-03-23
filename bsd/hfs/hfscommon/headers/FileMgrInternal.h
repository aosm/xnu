/*
 * Copyright (c) 2000-2001 Apple Computer, Inc. All rights reserved.
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
	File:		FilesInternal.h

	Contains:	IPI for File Manager (HFS Plus)

	Version:	HFS Plus 1.0

	Copyright:	� 1996-2001 by Apple Computer, Inc., all rights reserved.

*/
#ifndef __FILEMGRINTERNAL__
#define __FILEMGRINTERNAL__

#include <sys/param.h>
#include <sys/vnode.h>

#include "../../hfs.h"
#include "../../hfs_macos_defs.h"
#include "../../hfs_format.h"


#if PRAGMA_ONCE
#pragma once
#endif

#ifdef __cplusplus
extern "C" {
#endif

#if PRAGMA_IMPORT
#pragma import on
#endif

#if PRAGMA_STRUCT_ALIGN
	#pragma options align=mac68k
#elif PRAGMA_STRUCT_PACKPUSH
	#pragma pack(push, 2)
#elif PRAGMA_STRUCT_PACK
	#pragma pack(2)
#endif

/* CatalogNodeID is used to track catalog objects */
typedef UInt32		HFSCatalogNodeID;

/* internal error codes*/

#if TARGET_API_MACOS_X
  #define ERR_BASE	-32767
#else
  #define ERR_BASE	0
#endif

enum {
																/* FXM errors*/
	fxRangeErr					= ERR_BASE + 16,				/* file position beyond mapped range*/
	fxOvFlErr					= ERR_BASE + 17,				/* extents file overflow*/
																/* Unicode errors*/
	uniTooLongErr				= ERR_BASE + 24,				/* Unicode string too long to convert to Str31*/
	uniBufferTooSmallErr		= ERR_BASE + 25,				/* Unicode output buffer too small*/
	uniNotMappableErr			= ERR_BASE + 26,				/* Unicode string can't be mapped to given script*/
																/* BTree Manager errors*/
	btNotFound					= ERR_BASE + 32,				/* record not found*/
	btExists					= ERR_BASE + 33,				/* record already exists*/
	btNoSpaceAvail				= ERR_BASE + 34,				/* no available space*/
	btNoFit						= ERR_BASE + 35,				/* record doesn't fit in node */
	btBadNode					= ERR_BASE + 36,				/* bad node detected*/
	btBadHdr					= ERR_BASE + 37,				/* bad BTree header record detected*/
	dsBadRotate					= ERR_BASE + 64,				/* bad BTree rotate*/
																/* Catalog Manager errors*/
	cmNotFound					= ERR_BASE + 48,				/* CNode not found*/
	cmExists					= ERR_BASE + 49,				/* CNode already exists*/
	cmNotEmpty					= ERR_BASE + 50,				/* directory CNode not empty (valence = 0)*/
	cmRootCN					= ERR_BASE + 51,				/* invalid reference to root CNode*/
	cmBadNews					= ERR_BASE + 52,				/* detected bad catalog structure*/
	cmFThdDirErr				= ERR_BASE + 53,				/* thread belongs to a directory not a file*/
	cmFThdGone					= ERR_BASE + 54,				/* file thread doesn't exist*/
	cmParentNotFound			= ERR_BASE + 55,				/* CNode for parent ID does not exist*/
																/* TFS internal errors*/
	fsDSIntErr					= -127							/* Internal file system error*/
};


/* internal flags*/

enum {
	kEFContigBit				= 1,							/*	force contiguous allocation*/
	kEFContigMask				= 0x02,
	kEFAllBit					= 0,							/*	allocate all requested bytes or none*/
	kEFAllMask					= 0x01,							/*	TruncateFile option flags*/
	kTFTrunExtBit				= 0,							/*	truncate to the extent containing new PEOF*/
	kTFTrunExtMask				= 1
};

enum {
	kUndefinedStrLen			= 0,							/* Unknown string length */
	kNoHint						= 0,

																/*	FileIDs variables*/
	kNumExtentsToCache			= 4								/*	just guessing for ExchangeFiles*/
};


enum {
	kInvalidMRUCacheKey			= -1L,							/* flag to denote current MRU cache key is invalid*/
	kDefaultNumMRUCacheBlocks	= 16							/* default number of blocks in each cache*/
};


/* Universal Extent Key */

union ExtentKey {
	HFSExtentKey 					hfs;
	HFSPlusExtentKey 				hfsPlus;
};
typedef union ExtentKey					ExtentKey;
/* Universal extent descriptor */

union ExtentDescriptor {
	HFSExtentDescriptor 			hfs;
	HFSPlusExtentDescriptor 		hfsPlus;
};
typedef union ExtentDescriptor			ExtentDescriptor;
/* Universal extent record */

union ExtentRecord {
	HFSExtentRecord 				hfs;
	HFSPlusExtentRecord 			hfsPlus;
};
typedef union ExtentRecord				ExtentRecord;
/* Universal catalog key */

union CatalogKey {
	HFSCatalogKey 					hfs;
	HFSPlusCatalogKey 				hfsPlus;
};
typedef union CatalogKey				CatalogKey;
/* Universal catalog data record */

union CatalogRecord {
	SInt16 							recordType;
	HFSCatalogFolder 				hfsFolder;
	HFSCatalogFile 					hfsFile;
	HFSCatalogThread 				hfsThread;
	HFSPlusCatalogFolder 			hfsPlusFolder;
	HFSPlusCatalogFile 				hfsPlusFile;
	HFSPlusCatalogThread 			hfsPlusThread;
};
typedef union CatalogRecord				CatalogRecord;


enum {
	CMMaxCName					= kHFSMaxFileNameChars
};


enum {
	vcbMaxNam					= 27,							/* volumes currently have a 27 byte max name length*/
																/* VCB flags*/
	vcbManualEjectMask			= 0x0001,						/* bit 0	manual-eject bit: set if volume is in a manual-eject drive*/
	vcbFlushCriticalInfoMask	= 0x0002,						/* bit 1	critical info bit: set if critical MDB information needs to flush*/
																/*	IoParam->ioVAtrb*/
	kDefaultVolumeMask			= 0x0020,
	kFilesOpenMask				= 0x0040
};


/* Universal catalog name*/

union CatalogName {
	Str31 							pstr;
	HFSUniStr255 					ustr;
};
typedef union CatalogName CatalogName;


/*
 * MacOS accessor routines
 */
#define GetFileControlBlock(fref)			((FCB *)((fref)->v_data))
#define GetFileRefNumFromFCB(filePtr)		((filePtr)->h_vp)


/*	The following macro marks a VCB as dirty by setting the upper 8 bits of the flags*/
EXTERN_API_C( void )
MarkVCBDirty					(ExtendedVCB *vcb);

EXTERN_API_C( void )
MarkVCBClean					(ExtendedVCB *vcb);

EXTERN_API_C( Boolean )
IsVCBDirty						(ExtendedVCB *vcb);


#define VCB_LOCK_INIT(vcb)		simple_lock_init(&vcb->vcbSimpleLock)
#define VCB_LOCK(vcb)			simple_lock(&vcb->vcbSimpleLock)
#define VCB_UNLOCK(vcb)			simple_unlock(&vcb->vcbSimpleLock)

#define	MarkVCBDirty(vcb)		{ VCB_LOCK((vcb)); ((vcb)->vcbFlags |= 0xFF00); VCB_UNLOCK((vcb)); }
#define	MarkVCBClean(vcb)		{ VCB_LOCK((vcb)); ((vcb)->vcbFlags &= 0x00FF); VCB_UNLOCK((vcb)); }
#define	IsVCBDirty(vcb)			((Boolean) ((vcb->vcbFlags & 0xFF00) != 0))


/*	Test for error and return if error occurred*/
EXTERN_API_C( void )
ReturnIfError					(OSErr 					result);

#define	ReturnIfError(result)					if ( (result) != noErr ) return (result); else ;
/*	Test for passed condition and return if true*/
EXTERN_API_C( void )
ReturnErrorIf					(Boolean 				condition,
								 OSErr 					result);

#define	ReturnErrorIf(condition, error)			if ( (condition) )	return( (error) );
/*	Exit function on error*/
EXTERN_API_C( void )
ExitOnError						(OSErr 					result);

#define	ExitOnError( result )					if ( ( result ) != noErr )	goto ErrorExit; else ;



/* Catalog Manager Routines (IPI)*/

EXTERN_API_C( OSErr )
CreateCatalogNode				(ExtendedVCB *			volume,
								 HFSCatalogNodeID 		parentID,
								 ConstUTF8Param	 		name,
								 UInt32 				nodeType,
								 HFSCatalogNodeID *		catalogNodeID,
								 UInt32 *				catalogHint,
								 UInt32					teHint);

EXTERN_API_C( OSErr )
DeleteCatalogNode				(ExtendedVCB *			volume,
								 HFSCatalogNodeID 		parentID,
								 ConstUTF8Param 		name,
								 UInt32 				hint);

EXTERN_API_C( OSErr )
GetCatalogNode					(ExtendedVCB *			volume,
								 HFSCatalogNodeID 		parentID,
								 ConstUTF8Param 		name,
                                 UInt32 				length,
                                 UInt32 				hint,
								 CatalogNodeData *		nodeData,
								 UInt32 *				newHint);

EXTERN_API_C( OSErr )
GetCatalogOffspring				(ExtendedVCB *			volume,
								 HFSCatalogNodeID 		folderID,
								 UInt16 				index,
								 CatalogNodeData *		nodeData,
								 HFSCatalogNodeID *		nodeID,
								 SInt16 *			nodeType);

EXTERN_API_C( OSErr )
MoveRenameCatalogNode			(ExtendedVCB *			volume,
								 HFSCatalogNodeID		srcParentID,
								 ConstUTF8Param			srcName,
					  			 UInt32					srcHint,
					  			 HFSCatalogNodeID		dstParentID,
					  			 ConstUTF8Param			dstName,
					  			 UInt32 *				newHint,
					  			 UInt32					teHint);

EXTERN_API_C( OSErr )
UpdateCatalogNode				(ExtendedVCB *			volume,
								 HFSCatalogNodeID		parentID,
								 ConstUTF8Param			name,
								 UInt32					catalogHint, 
								 const CatalogNodeData * nodeData);

EXTERN_API_C( OSErr )
CreateFileIDRef					(ExtendedVCB *			volume,
								 HFSCatalogNodeID		parentID,
								 ConstUTF8Param			name,
								 UInt32					hint,
								 HFSCatalogNodeID *		threadID);

EXTERN_API_C( OSErr )
ExchangeFileIDs					(ExtendedVCB *			volume,
								 ConstUTF8Param			srcName,
								 ConstUTF8Param			destName,
								 HFSCatalogNodeID		srcID,
								 HFSCatalogNodeID		destID,
								 UInt32					srcHint,
								 UInt32					destHint );

EXTERN_API_C( OSErr )
LinkCatalogNode					(ExtendedVCB *			volume,
								 HFSCatalogNodeID 		parentID,
								 ConstUTF8Param 		name,
								 HFSCatalogNodeID 		linkParentID,
								 ConstUTF8Param 		linkName);

EXTERN_API_C( SInt32 )
CompareCatalogKeys				(HFSCatalogKey *		searchKey,
								 HFSCatalogKey *		trialKey);

EXTERN_API_C( SInt32 )
CompareExtendedCatalogKeys		(HFSPlusCatalogKey *	searchKey,
								 HFSPlusCatalogKey *	trialKey);

EXTERN_API_C( OSErr )
InitCatalogCache				(void);

EXTERN_API_C( void )
InvalidateCatalogCache			(ExtendedVCB *			volume);


/* GenericMRUCache Routines*/
EXTERN_API_C( OSErr )
InitMRUCache					(UInt32 				bufferSize,
								 UInt32 				numCacheBlocks,
								 Ptr *					cachePtr);

EXTERN_API_C( OSErr )
DisposeMRUCache					(Ptr 					cachePtr);

EXTERN_API_C( void )
TrashMRUCache					(Ptr 					cachePtr);

EXTERN_API_C( OSErr )
GetMRUCacheBlock				(UInt32 				key,
								 Ptr 					cachePtr,
								 Ptr *					buffer);

EXTERN_API_C( void )
InvalidateMRUCacheBlock			(Ptr 					cachePtr,
								 Ptr 					buffer);

EXTERN_API_C( void )
InsertMRUCacheBlock				(Ptr 					cachePtr,
								 UInt32 				key,
								 Ptr 					buffer);

/* BTree Manager Routines*/

typedef CALLBACK_API_C( SInt32 , KeyCompareProcPtr )(void *a, void *b);


EXTERN_API_C( OSErr )
SearchBTreeRecord				(FileReference 				refNum,
								 const void *			key,
								 UInt32 				hint,
								 void *					foundKey,
								 void *					data,
								 UInt16 *				dataSize,
								 UInt32 *				newHint);

EXTERN_API_C( OSErr )
InsertBTreeRecord				(FileReference 				refNum,
								 void *					key,
								 void *					data,
								 UInt16 				dataSize,
								 UInt32 *				newHint);

EXTERN_API_C( OSErr )
DeleteBTreeRecord				(FileReference 				refNum,
								 void *					key);

EXTERN_API_C( OSErr )
ReplaceBTreeRecord				(FileReference 				refNum,
								 const void *			key,
								 UInt32 				hint,
								 void *					newData,
								 UInt16 				dataSize,
								 UInt32 *				newHint);

/*	Prototypes for C->Asm glue*/
EXTERN_API_C( OSErr )
GetBlock_glue					(UInt16 				flags,
								 UInt32 				nodeNumber,
								 Ptr *					nodeBuffer,
                   						 FileReference 				refNum,
								 ExtendedVCB *			vcb);

EXTERN_API_C( OSErr )
RelBlock_glue					(Ptr 					nodeBuffer,
								 UInt16 				flags);

/*	Prototypes for exported routines in VolumeAllocation.c*/
EXTERN_API_C( OSErr )
BlockAllocate					(ExtendedVCB *			vcb,
								 UInt32 				startingBlock,
								 SInt64 				bytesRequested,
								 SInt64 				bytesMaximum,
								 Boolean 				forceContiguous,
								 UInt32 *				startBlock,
								 UInt32 *				actualBlocks);

EXTERN_API_C( OSErr )
BlockDeallocate					(ExtendedVCB *			vcb,
								 UInt32 				firstBlock,
								 UInt32 				numBlocks);

EXTERN_API_C( UInt32 )
FileBytesToBlocks				(SInt64 				numerator,
								 UInt32 				denominator);

/*	File Extent Mapping routines*/
EXTERN_API_C( OSErr )
FlushExtentFile					(ExtendedVCB *			vcb);

EXTERN_API_C( SInt32 )
CompareExtentKeys				(const HFSExtentKey *	searchKey,
								 const HFSExtentKey *	trialKey);

EXTERN_API_C( SInt32 )
CompareExtentKeysPlus			(const HFSPlusExtentKey *searchKey,
								 const HFSPlusExtentKey *trialKey);

EXTERN_API_C( OSErr )
DeleteFile						(ExtendedVCB *			vcb,
								 HFSCatalogNodeID 		parDirID,
								 ConstUTF8Param 		catalogName,
								 UInt32					catalogHint);

EXTERN_API_C( OSErr )
TruncateFileC					(ExtendedVCB *			vcb,
								 FCB *					fcb,
								 SInt64 				peof,
								 Boolean 				truncateToExtent);

EXTERN_API_C( OSErr )
ExtendFileC						(ExtendedVCB *			vcb,
								 FCB *					fcb,
								 SInt64 				bytesToAdd,
								 UInt32 				blockHint,
								 UInt32 				flags,
								 SInt64 *				actualBytesAdded);

EXTERN_API_C( OSErr )
MapFileBlockC					(ExtendedVCB *			vcb,
								 FCB *					fcb,
								 size_t 				numberOfBytes,
								 off_t 					offset,
								 daddr_t *				startBlock,
								 size_t *				availableBytes);

#if TARGET_API_MACOS_X
EXTERN_API_C( Boolean )
NodesAreContiguous				(ExtendedVCB *			vcb,
								 FCB *					fcb,
								 UInt32					nodeSize);
#endif

/*	Utility routines*/

EXTERN_API_C( void )
ClearMemory						(void *					start,
								 UInt32 				length);

EXTERN_API_C( OSErr )
VolumeWritable					(ExtendedVCB *	vcb);


/*	Get the current time in UTC (GMT)*/
EXTERN_API_C( UInt32 )
GetTimeUTC						(void);

/*	Get the current local time*/
EXTERN_API_C( UInt32 )
GetTimeLocal					(Boolean forHFS);

EXTERN_API_C( UInt32 )
LocalToUTC						(UInt32 				localTime);

EXTERN_API_C( UInt32 )
UTCToLocal						(UInt32 				utcTime);


/*	Volumes routines*/
EXTERN_API_C( OSErr )
FlushVolumeControlBlock			(ExtendedVCB *			vcb);

EXTERN_API_C( OSErr )
ValidVolumeHeader				(HFSPlusVolumeHeader *			volumeHeader);


#if PRAGMA_STRUCT_ALIGN
	#pragma options align=reset
#elif PRAGMA_STRUCT_PACKPUSH
	#pragma pack(pop)
#elif PRAGMA_STRUCT_PACK
	#pragma pack()
#endif

#ifdef PRAGMA_IMPORT_OFF
#pragma import off
#elif PRAGMA_IMPORT
#pragma import reset
#endif

#ifdef __cplusplus
}
#endif

#endif /* __FILEMGRINTERNAL__ */


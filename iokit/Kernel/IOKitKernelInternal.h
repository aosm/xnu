/*
 * Copyright (c) 1998-2000 Apple Computer, Inc. All rights reserved.
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


#ifndef _IOKIT_KERNELINTERNAL_H
#define _IOKIT_KERNELINTERNAL_H

#include <sys/cdefs.h>

__BEGIN_DECLS

#include <vm/vm_pageout.h>
#include <mach/memory_object_types.h>
#include <device/device_port.h>

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

#if (KDEBUG_LEVEL >= KDEBUG_LEVEL_STANDARD)

#define IOServiceTrace(csc, a, b, c, d) do {				\
    if(kIOTraceIOService & gIOKitDebug) {				\
	KERNEL_DEBUG_CONSTANT(IODBG_IOSERVICE(csc), a, b, c, d, 0);	\
    }									\
} while(0)

#else /* (KDEBUG_LEVEL >= KDEBUG_LEVEL_STANDARD) */

#define IOServiceTrace(csc, a, b, c, d) do {	\
  (void)a;					\
  (void)b;					\
  (void)c;					\
  (void)d;					\
} while (0)

#endif /* (KDEBUG_LEVEL >= KDEBUG_LEVEL_STANDARD) */

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

typedef kern_return_t (*IOIteratePageableMapsCallback)(vm_map_t map, void * ref);

void IOLibInit(void);
kern_return_t IOIteratePageableMaps(vm_size_t size,
                    IOIteratePageableMapsCallback callback, void * ref);
vm_map_t IOPageableMapForAddress(uintptr_t address);

kern_return_t 
IOMemoryDescriptorMapMemEntry(vm_map_t * map, ipc_port_t entry, IOOptionBits options, bool pageable,
				mach_vm_size_t offset, mach_vm_address_t * address, mach_vm_size_t length);
kern_return_t 
IOMemoryDescriptorMapCopy(vm_map_t * map, 
				IOOptionBits options,
				mach_vm_size_t offset, 
				mach_vm_address_t * address, mach_vm_size_t length);

mach_vm_address_t
IOKernelAllocateWithPhysicalRestrict(mach_vm_size_t size, mach_vm_address_t maxPhys, 
			                mach_vm_size_t alignment, bool contiguous);
void
IOKernelFreePhysical(mach_vm_address_t address, mach_vm_size_t size);


extern vm_size_t debug_iomallocpageable_size;

// osfmk/device/iokit_rpc.c
extern kern_return_t IOMapPages(vm_map_t map, mach_vm_address_t va, mach_vm_address_t pa,
                                 mach_vm_size_t length, unsigned int mapFlags);
extern kern_return_t IOUnmapPages(vm_map_t map, mach_vm_address_t va, mach_vm_size_t length);

extern kern_return_t IOProtectCacheMode(vm_map_t map, mach_vm_address_t va,
					mach_vm_size_t length, unsigned int mapFlags);

extern ppnum_t IOGetLastPageNumber(void);

extern ppnum_t gIOLastPage;

extern IOSimpleLock * gIOPageAllocLock;
extern queue_head_t   gIOPageAllocList;

/* Physical to physical copy (ints must be disabled) */
extern void bcopy_phys(addr64_t from, addr64_t to, vm_size_t size);

__END_DECLS

// Used for dedicated communications for IODMACommand
enum  {
    kIOMDWalkSegments             = 0x01000000,
    kIOMDFirstSegment	          = 1 | kIOMDWalkSegments,
    kIOMDGetCharacteristics       = 0x02000000,
    kIOMDGetCharacteristicsMapped = 1 | kIOMDGetCharacteristics,
    kIOMDDMAActive                = 0x03000000,
    kIOMDSetDMAActive             = 1 | kIOMDDMAActive,
    kIOMDSetDMAInactive           = kIOMDDMAActive,
    kIOMDAddDMAMapSpec            = 0x04000000,
    kIOMDDMAMap                   = 0x05000000,
    kIOMDDMACommandOperationMask  = 0xFF000000,
};
struct IOMDDMACharacteristics {
    UInt64 fLength;
    UInt32 fSGCount;
    UInt32 fPages;
    UInt32 fPageAlign;
    ppnum_t fHighestPage;
    IODirection fDirection;
    UInt8 fIsPrepared;
};
struct IOMDDMAWalkSegmentArgs {
    UInt64 fOffset;			// Input/Output offset
    UInt64 fIOVMAddr, fLength;		// Output variables
    UInt8 fMapped;			// Input Variable, Require mapped IOVMA
};
typedef UInt8 IOMDDMAWalkSegmentState[128];

struct IOMDDMAMapArgs {
    IOMapper *            fMapper;
    IODMAMapSpecification fMapSpec;
    uint64_t              fOffset;
    uint64_t              fLength;
    uint64_t              fAlloc;
    ppnum_t               fAllocCount;
    uint8_t               fMapContig;
};

struct IODMACommandInternal
{
    IOMDDMAWalkSegmentState fState;
    IOMDDMACharacteristics  fMDSummary;

    UInt64 fPreparedOffset;
    UInt64 fPreparedLength;

    UInt32 fSourceAlignMask;
	
    UInt8  fCursor;
    UInt8  fCheckAddressing;
    UInt8  fIterateOnly;
    UInt8  fMisaligned;
    UInt8  fMapContig;
    UInt8  fPrepared;
    UInt8  fDoubleBuffer;
    UInt8  fNewMD;
    UInt8  fLocalMapper;
	
    vm_page_t fCopyPageAlloc;
    vm_page_t fCopyNext;
    vm_page_t fNextRemapPage;

    ppnum_t  fCopyPageCount;

    addr64_t  fLocalMapperPageAlloc;
    ppnum_t  fLocalMapperPageCount;

    class IOBufferMemoryDescriptor * fCopyMD;

    IOService * fDevice;

    // IODMAEventSource use
    IOReturn fStatus;
    UInt64   fActualByteCount;
    AbsoluteTime    fTimeStamp;
};

struct IOMemoryDescriptorDevicePager {
    void *		         devicePager;
    unsigned int	     pagerContig:1;
    unsigned int	     unused:31;
    IOMemoryDescriptor * memory;
};

struct IOMemoryDescriptorReserved {
    IOMemoryDescriptorDevicePager dp;
    uint64_t                      preparationID;
    // for kernel IOMD subclasses... they have no expansion
    uint64_t                      kernReserved[4];
};

struct iopa_t
{
    IOLock       * lock;
    queue_head_t   list;
    vm_size_t      pagecount;
    vm_size_t      bytecount;
};

struct iopa_page_t
{
    queue_chain_t link;
    uint64_t      avail;
    uint32_t      signature;
};
typedef struct iopa_page_t iopa_page_t;

typedef uintptr_t (*iopa_proc_t)(iopa_t * a);

enum
{
    kIOPageAllocSignature  = 'iopa'
};

extern "C" void      iopa_init(iopa_t * a);
extern "C" uintptr_t iopa_alloc(iopa_t * a, iopa_proc_t alloc, vm_size_t bytes, uint32_t balign);
extern "C" uintptr_t iopa_free(iopa_t * a, uintptr_t addr, vm_size_t bytes);
extern "C" uint32_t  gIOPageAllocChunkBytes;

extern "C" iopa_t    gIOBMDPageAllocator;

extern "C" struct timeval gIOLastSleepTime;
extern "C" struct timeval gIOLastWakeTime;

extern clock_sec_t gIOConsoleLockTime;

extern OSSet * gIORemoveOnReadProperties;

extern "C" void IOKitResetTime( void );
extern "C" void IOKitInitializeTime( void );

extern "C" OSString * IOCopyLogNameForPID(int pid);

#if defined(__i386__) || defined(__x86_64__)
extern "C" void IOSetKeyStoreData(IOMemoryDescriptor * data);
#endif

void IOScreenLockTimeUpdate(clock_sec_t secs);


#endif /* ! _IOKIT_KERNELINTERNAL_H */

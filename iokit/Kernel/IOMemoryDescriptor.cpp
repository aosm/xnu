/*
 * Copyright (c) 1998-2000 Apple Computer, Inc. All rights reserved.
 *
 * @APPLE_LICENSE_HEADER_START@
 * 
 * Copyright (c) 1999-2003 Apple Computer, Inc.  All Rights Reserved.
 * 
 * This file contains Original Code and/or Modifications of Original Code
 * as defined in and that are subject to the Apple Public Source License
 * Version 2.0 (the 'License'). You may not use this file except in
 * compliance with the License. Please obtain a copy of the License at
 * http://www.opensource.apple.com/apsl/ and read it before using this
 * file.
 * 
 * The Original Code and all software distributed under the License are
 * distributed on an 'AS IS' basis, WITHOUT WARRANTY OF ANY KIND, EITHER
 * EXPRESS OR IMPLIED, AND APPLE HEREBY DISCLAIMS ALL SUCH WARRANTIES,
 * INCLUDING WITHOUT LIMITATION, ANY WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE, QUIET ENJOYMENT OR NON-INFRINGEMENT.
 * Please see the License for the specific language governing rights and
 * limitations under the License.
 * 
 * @APPLE_LICENSE_HEADER_END@
 */
/*
 * Copyright (c) 1998 Apple Computer, Inc.  All rights reserved. 
 *
 * HISTORY
 *
 */
// 45678901234567890123456789012345678901234567890123456789012345678901234567890
#include <sys/cdefs.h>

#include <IOKit/assert.h>
#include <IOKit/system.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IOMapper.h>

#include <IOKit/IOKitDebug.h>

#include <libkern/c++/OSContainers.h>
#include <libkern/c++/OSDictionary.h>
#include <libkern/c++/OSArray.h>
#include <libkern/c++/OSSymbol.h>
#include <libkern/c++/OSNumber.h>
#include <sys/cdefs.h>

__BEGIN_DECLS
#include <vm/pmap.h>
#include <mach/memory_object_types.h>
#include <device/device_port.h>

#ifndef i386
struct phys_entry      *pmap_find_physentry(ppnum_t pa);
#endif
void ipc_port_release_send(ipc_port_t port);

/* Copy between a physical page and a virtual address in the given vm_map */
kern_return_t copypv(addr64_t source, addr64_t sink, unsigned int size, int which);

memory_object_t
device_pager_setup(
	memory_object_t	pager,
	int		device_handle,
	vm_size_t	size,
	int		flags);
void
device_pager_deallocate(
        memory_object_t);
kern_return_t
device_pager_populate_object(
	memory_object_t		pager,
	vm_object_offset_t	offset,
	ppnum_t			phys_addr,
	vm_size_t		size);

/*
 *	Page fault handling based on vm_map (or entries therein)
 */
extern kern_return_t vm_fault(
		vm_map_t	map,
		vm_offset_t	vaddr,
		vm_prot_t	fault_type,
		boolean_t	change_wiring,
		int             interruptible,
		pmap_t		caller_pmap,
		vm_offset_t	caller_pmap_addr);

unsigned int  IOTranslateCacheBits(struct phys_entry *pp);

vm_map_t IOPageableMapForAddress( vm_address_t address );

typedef kern_return_t (*IOIteratePageableMapsCallback)(vm_map_t map, void * ref);

kern_return_t IOIteratePageableMaps(vm_size_t size,
                    IOIteratePageableMapsCallback callback, void * ref);
__END_DECLS

static IOMapper * gIOSystemMapper;

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

OSDefineMetaClassAndAbstractStructors( IOMemoryDescriptor, OSObject )

#define super IOMemoryDescriptor

OSDefineMetaClassAndStructors(IOGeneralMemoryDescriptor, IOMemoryDescriptor)

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

static IORecursiveLock * gIOMemoryLock;

#define LOCK	IORecursiveLockLock( gIOMemoryLock)
#define UNLOCK	IORecursiveLockUnlock( gIOMemoryLock)
#define SLEEP	IORecursiveLockSleep( gIOMemoryLock, (void *)this, THREAD_UNINT)
#define WAKEUP	\
    IORecursiveLockWakeup( gIOMemoryLock, (void *)this, /* one-thread */ false)

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

#define next_page(a) ( trunc_page_32(a) + PAGE_SIZE )


extern "C" {

kern_return_t device_data_action(
               int                     device_handle, 
               ipc_port_t              device_pager,
               vm_prot_t               protection, 
               vm_object_offset_t      offset, 
               vm_size_t               size)
{
    struct ExpansionData {
        void *				devicePager;
        unsigned int			pagerContig:1;
        unsigned int			unused:31;
	IOMemoryDescriptor *		memory;
    };
    kern_return_t	 kr;
    ExpansionData *      ref = (ExpansionData *) device_handle;
    IOMemoryDescriptor * memDesc;

    LOCK;
    memDesc = ref->memory;
    if( memDesc)
	kr = memDesc->handleFault( device_pager, 0, 0,
                offset, size, kIOMapDefaultCache /*?*/);
    else
	kr = KERN_ABORTED;
    UNLOCK;

    return( kr );
}

kern_return_t device_close(
               int     device_handle)
{
    struct ExpansionData {
        void *				devicePager;
        unsigned int			pagerContig:1;
        unsigned int			unused:31;
	IOMemoryDescriptor *		memory;
    };
    ExpansionData *   ref = (ExpansionData *) device_handle;

    IODelete( ref, ExpansionData, 1 );

    return( kIOReturnSuccess );
}

}

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

/*
 * withAddress:
 *
 * Create a new IOMemoryDescriptor.  The buffer is a virtual address
 * relative to the specified task.  If no task is supplied, the kernel
 * task is implied.
 */
IOMemoryDescriptor *
IOMemoryDescriptor::withAddress(void *      address,
                                IOByteCount   length,
                                IODirection direction)
{
    return IOMemoryDescriptor::
        withAddress((vm_address_t) address, length, direction, kernel_task);
}

IOMemoryDescriptor *
IOMemoryDescriptor::withAddress(vm_address_t address,
                                IOByteCount  length,
                                IODirection  direction,
                                task_t       task)
{
    IOGeneralMemoryDescriptor * that = new IOGeneralMemoryDescriptor;
    if (that)
    {
	if (that->initWithAddress(address, length, direction, task))
	    return that;

        that->release();
    }
    return 0;
}

IOMemoryDescriptor *
IOMemoryDescriptor::withPhysicalAddress(
				IOPhysicalAddress	address,
				IOByteCount		length,
				IODirection      	direction )
{
    IOGeneralMemoryDescriptor *self = new IOGeneralMemoryDescriptor;
    if (self
    && !self->initWithPhysicalAddress(address, length, direction)) {
        self->release();
        return 0;
    }

    return self;
}

IOMemoryDescriptor *
IOMemoryDescriptor::withRanges(	IOVirtualRange * ranges,
				UInt32           withCount,
				IODirection      direction,
				task_t           task,
				bool             asReference)
{
    IOGeneralMemoryDescriptor * that = new IOGeneralMemoryDescriptor;
    if (that)
    {
	if (that->initWithRanges(ranges, withCount, direction, task, asReference))
	    return that;

        that->release();
    }
    return 0;
}


/*
 * withRanges:
 *
 * Create a new IOMemoryDescriptor. The buffer is made up of several
 * virtual address ranges, from a given task.
 *
 * Passing the ranges as a reference will avoid an extra allocation.
 */
IOMemoryDescriptor *
IOMemoryDescriptor::withOptions(void *		buffers,
                                UInt32		count,
                                UInt32		offset,
                                task_t		task,
                                IOOptionBits	opts,
                                IOMapper *	mapper)
{
    IOGeneralMemoryDescriptor *self = new IOGeneralMemoryDescriptor;

    if (self
    && !self->initWithOptions(buffers, count, offset, task, opts, mapper))
    {
        self->release();
        return 0;
    }

    return self;
}

// Can't leave abstract but this should never be used directly,
bool IOMemoryDescriptor::initWithOptions(void *		buffers,
                                         UInt32		count,
                                         UInt32		offset,
                                         task_t		task,
                                         IOOptionBits	options,
                                         IOMapper *	mapper)
{
    // @@@ gvdl: Should I panic?
    panic("IOMD::initWithOptions called\n");
    return 0;
}

IOMemoryDescriptor *
IOMemoryDescriptor::withPhysicalRanges(	IOPhysicalRange * ranges,
                                        UInt32          withCount,
                                        IODirection     direction,
                                        bool            asReference)
{
    IOGeneralMemoryDescriptor * that = new IOGeneralMemoryDescriptor;
    if (that)
    {
	if (that->initWithPhysicalRanges(ranges, withCount, direction, asReference))
	    return that;

        that->release();
    }
    return 0;
}

IOMemoryDescriptor *
IOMemoryDescriptor::withSubRange(IOMemoryDescriptor *	of,
				IOByteCount		offset,
				IOByteCount		length,
				IODirection		direction)
{
    IOSubMemoryDescriptor *self = new IOSubMemoryDescriptor;

    if (self && !self->initSubRange(of, offset, length, direction)) {
        self->release();
	self = 0;
    }
    return self;
}

/*
 * initWithAddress:
 *
 * Initialize an IOMemoryDescriptor. The buffer is a virtual address
 * relative to the specified task.  If no task is supplied, the kernel
 * task is implied.
 *
 * An IOMemoryDescriptor can be re-used by calling initWithAddress or
 * initWithRanges again on an existing instance -- note this behavior
 * is not commonly supported in other I/O Kit classes, although it is
 * supported here.
 */
bool
IOGeneralMemoryDescriptor::initWithAddress(void *      address,
                                    IOByteCount   withLength,
                                    IODirection withDirection)
{
    _singleRange.v.address = (vm_address_t) address;
    _singleRange.v.length  = withLength;

    return initWithRanges(&_singleRange.v, 1, withDirection, kernel_task, true);
}

bool
IOGeneralMemoryDescriptor::initWithAddress(vm_address_t address,
                                    IOByteCount    withLength,
                                    IODirection  withDirection,
                                    task_t       withTask)
{
    _singleRange.v.address = address;
    _singleRange.v.length  = withLength;

    return initWithRanges(&_singleRange.v, 1, withDirection, withTask, true);
}

bool
IOGeneralMemoryDescriptor::initWithPhysicalAddress(
				 IOPhysicalAddress	address,
				 IOByteCount		withLength,
				 IODirection      	withDirection )
{
    _singleRange.p.address = address;
    _singleRange.p.length  = withLength;

    return initWithPhysicalRanges( &_singleRange.p, 1, withDirection, true);
}

bool
IOGeneralMemoryDescriptor::initWithPhysicalRanges(
                                IOPhysicalRange * ranges,
                                UInt32            count,
                                IODirection       direction,
                                bool              reference)
{
    IOOptionBits mdOpts = direction | kIOMemoryTypePhysical;

    if (reference)
        mdOpts |= kIOMemoryAsReference;

    return initWithOptions(ranges, count, 0, 0, mdOpts, /* mapper */ 0);
}

bool
IOGeneralMemoryDescriptor::initWithRanges(
                                   IOVirtualRange * ranges,
                                   UInt32           count,
                                   IODirection      direction,
                                   task_t           task,
                                   bool             reference)
{
    IOOptionBits mdOpts = direction;

    if (reference)
        mdOpts |= kIOMemoryAsReference;

    if (task) {
        mdOpts |= kIOMemoryTypeVirtual;
        if (task == kernel_task)
            mdOpts |= kIOMemoryAutoPrepare;
    }
    else
        mdOpts |= kIOMemoryTypePhysical;

    // @@@ gvdl: Need to remove this
    // Auto-prepare if this is a kernel memory descriptor as very few
    // clients bother to prepare() kernel memory.
    // But it has been enforced so what are you going to do?
    
    return initWithOptions(ranges, count, 0, task, mdOpts, /* mapper */ 0);
}

/*
 * initWithOptions:
 *
 *  IOMemoryDescriptor. The buffer is made up of several virtual address ranges,
 * from a given task or several physical ranges or finally an UPL from the ubc
 * system.
 *
 * Passing the ranges as a reference will avoid an extra allocation.
 *
 * An IOMemoryDescriptor can be re-used by calling initWithOptions again on an
 * existing instance -- note this behavior is not commonly supported in other
 * I/O Kit classes, although it is supported here.
 */

enum ioPLBlockFlags {
    kIOPLOnDevice  = 0x00000001,
    kIOPLExternUPL = 0x00000002,
};

struct ioPLBlock {
    upl_t fIOPL;
    vm_address_t fIOMDOffset;	// The offset of this iopl in descriptor
    vm_offset_t fPageInfo;	// Pointer to page list or index into it
    ppnum_t fMappedBase;	// Page number of first page in this iopl
    unsigned int fPageOffset;	// Offset within first page of iopl
    unsigned int fFlags;	// Flags
};

struct ioGMDData {
    IOMapper *fMapper;
    unsigned int fPageCnt;
    upl_page_info_t fPageList[0];	// @@@ gvdl need to get rid of this
                                        //  should be able to use upl directly
    ioPLBlock fBlocks[0];
};

#define getDataP(osd)	((ioGMDData *) (osd)->getBytesNoCopy())
#define getIOPLList(d)	((ioPLBlock *) &(d->fPageList[d->fPageCnt]))
#define getNumIOPL(d,len)	\
    ((len - ((char *) getIOPLList(d) - (char *) d)) / sizeof(ioPLBlock))
#define getPageList(d)	(&(d->fPageList[0]))
#define computeDataSize(p, u) \
    (sizeof(ioGMDData) + p * sizeof(upl_page_info_t) + u * sizeof(ioPLBlock))

bool
IOGeneralMemoryDescriptor::initWithOptions(void *	buffers,
                                           UInt32	count,
                                           UInt32	offset,
                                           task_t	task,
                                           IOOptionBits	options,
                                           IOMapper *	mapper)
{

    switch (options & kIOMemoryTypeMask) {
    case kIOMemoryTypeVirtual:
        assert(task);
        if (!task)
            return false;
        else
            break;

    case kIOMemoryTypePhysical:		// Neither Physical nor UPL should have a task
	mapper = kIOMapperNone;
    case kIOMemoryTypeUPL:
        assert(!task);
        break;
    default:
panic("IOGMD::iWO(): bad type");	// @@@ gvdl: for testing
        return false;	/* bad argument */
    }

    assert(buffers);
    assert(count);

    /*
     * We can check the _initialized  instance variable before having ever set
     * it to an initial value because I/O Kit guarantees that all our instance
     * variables are zeroed on an object's allocation.
     */

    if (_initialized) {
        /*
         * An existing memory descriptor is being retargeted to point to
         * somewhere else.  Clean up our present state.
         */

        while (_wireCount)
            complete();
        if (_kernPtrAligned)
            unmapFromKernel();
        if (_ranges.v && _rangesIsAllocated)
            IODelete(_ranges.v, IOVirtualRange, _rangesCount);
    }
    else {
        if (!super::init())
            return false;
        _initialized = true;
    }

    // Grab the appropriate mapper
    if (mapper == kIOMapperNone)
        mapper = 0;	// No Mapper
    else if (!mapper) {
        IOMapper::checkForSystemMapper();
        gIOSystemMapper = mapper = IOMapper::gSystem;
    }

    _flags		   = options;
    _task                  = task;

    // DEPRECATED variable initialisation
    _direction             = (IODirection) (_flags & kIOMemoryDirectionMask);
    _position              = 0;
    _kernPtrAligned        = 0;
    _cachedPhysicalAddress = 0;
    _cachedVirtualAddress  = 0;

    if ( (options & kIOMemoryTypeMask) == kIOMemoryTypeUPL) {

        ioGMDData *dataP;
        unsigned int dataSize = computeDataSize(/* pages */ 0, /* upls */ 1);

        if (!_memoryEntries) {
            _memoryEntries = OSData::withCapacity(dataSize);
            if (!_memoryEntries)
                return false;
        }
        else if (!_memoryEntries->initWithCapacity(dataSize))
            return false;

        _memoryEntries->appendBytes(0, sizeof(ioGMDData));
        dataP = getDataP(_memoryEntries);
        dataP->fMapper = mapper;
        dataP->fPageCnt = 0;

        _wireCount++;	// UPLs start out life wired

        _length    = count;
        _pages    += atop_32(offset + count + PAGE_MASK) - atop_32(offset);

        ioPLBlock iopl;
        upl_page_info_t *pageList = UPL_GET_INTERNAL_PAGE_LIST((upl_t) buffers);

        iopl.fIOPL = (upl_t) buffers;
        // Set the flag kIOPLOnDevice convieniently equal to 1
        iopl.fFlags  = pageList->device | kIOPLExternUPL;
        iopl.fIOMDOffset = 0;
        if (!pageList->device) {
            // @@@ gvdl: Ask JoeS are the pages contiguious with the list?
            // or there a chance that we may be inserting 0 phys_addrs?
            // Pre-compute the offset into the UPL's page list
            pageList = &pageList[atop_32(offset)];
            offset &= PAGE_MASK;
            if (mapper) {
                iopl.fMappedBase = mapper->iovmAlloc(_pages);
                mapper->iovmInsert(iopl.fMappedBase, 0, pageList, _pages);
            }
	    else
		iopl.fMappedBase = 0;
        }
	else
	    iopl.fMappedBase = 0;
        iopl.fPageInfo = (vm_address_t) pageList;
        iopl.fPageOffset = offset;

        _memoryEntries->appendBytes(&iopl, sizeof(iopl));
    }
    else {	/* kIOMemoryTypeVirtual | kIOMemoryTypePhysical */
        IOVirtualRange *ranges = (IOVirtualRange *) buffers;

        /*
         * Initialize the memory descriptor.
         */

        _length			= 0;
        _pages			= 0;
        for (unsigned ind = 0; ind < count; ind++) {
            IOVirtualRange cur = ranges[ind];
    
            _length += cur.length;
            _pages += atop_32(cur.address + cur.length + PAGE_MASK)
                   -  atop_32(cur.address);
        }

        _ranges.v		= 0;
        _rangesIsAllocated	= !(options & kIOMemoryAsReference);
        _rangesCount		= count;

        if (options & kIOMemoryAsReference)
            _ranges.v = ranges;
        else {
            _ranges.v = IONew(IOVirtualRange, count);
            if (!_ranges.v)
                return false;
            bcopy(/* from */ ranges, _ranges.v,
                  count * sizeof(IOVirtualRange));
        } 

        // Auto-prepare memory at creation time.
        // Implied completion when descriptor is free-ed
        if ( (options & kIOMemoryTypeMask) == kIOMemoryTypePhysical)
            _wireCount++;	// Physical MDs are start out wired
        else { /* kIOMemoryTypeVirtual */
            ioGMDData *dataP;
            unsigned int dataSize =
                computeDataSize(_pages, /* upls */ _rangesCount * 2);

            if (!_memoryEntries) {
                _memoryEntries = OSData::withCapacity(dataSize);
                if (!_memoryEntries)
                    return false;
            }
            else if (!_memoryEntries->initWithCapacity(dataSize))
                return false;
    
            _memoryEntries->appendBytes(0, sizeof(ioGMDData));
            dataP = getDataP(_memoryEntries);
            dataP->fMapper = mapper;
            dataP->fPageCnt = _pages;

            if ((_flags & kIOMemoryAutoPrepare)
             && prepare() != kIOReturnSuccess)
                return false;
        }
    }

    return true;
}

/*
 * free
 *
 * Free resources.
 */
void IOGeneralMemoryDescriptor::free()
{
    LOCK;
    if( reserved)
	reserved->memory = 0;
    UNLOCK;

    while (_wireCount)
        complete();
    if (_memoryEntries)
        _memoryEntries->release();

    if (_kernPtrAligned)
        unmapFromKernel();
    if (_ranges.v && _rangesIsAllocated)
        IODelete(_ranges.v, IOVirtualRange, _rangesCount);

    if (reserved && reserved->devicePager)
	device_pager_deallocate( (memory_object_t) reserved->devicePager );

    // memEntry holds a ref on the device pager which owns reserved
    // (ExpansionData) so no reserved access after this point
    if (_memEntry)
        ipc_port_release_send( (ipc_port_t) _memEntry );

    super::free();
}

/* DEPRECATED */ void IOGeneralMemoryDescriptor::unmapFromKernel()
/* DEPRECATED */ {
                    panic("IOGMD::unmapFromKernel deprecated");
/* DEPRECATED */ }
/* DEPRECATED */ 
/* DEPRECATED */ void IOGeneralMemoryDescriptor::mapIntoKernel(unsigned rangeIndex)
/* DEPRECATED */ {
                    panic("IOGMD::mapIntoKernel deprecated");
/* DEPRECATED */ }

/*
 * getDirection:
 *
 * Get the direction of the transfer.
 */
IODirection IOMemoryDescriptor::getDirection() const
{
    return _direction;
}

/*
 * getLength:
 *
 * Get the length of the transfer (over all ranges).
 */
IOByteCount IOMemoryDescriptor::getLength() const
{
    return _length;
}

void IOMemoryDescriptor::setTag( IOOptionBits tag )
{
    _tag = tag;    
}

IOOptionBits IOMemoryDescriptor::getTag( void )
{
    return( _tag);
}

// @@@ gvdl: who is using this API?  Seems like a wierd thing to implement.
IOPhysicalAddress IOMemoryDescriptor::getSourceSegment( IOByteCount   offset,
                                                        IOByteCount * length )
{
    IOPhysicalAddress physAddr = 0;

    if( prepare() == kIOReturnSuccess) {
        physAddr = getPhysicalSegment( offset, length );
        complete();
    }

    return( physAddr );
}

IOByteCount IOMemoryDescriptor::readBytes
                (IOByteCount offset, void *bytes, IOByteCount length)
{
    addr64_t dstAddr = (addr64_t) (UInt32) bytes;
    IOByteCount remaining;

    // Assert that this entire I/O is withing the available range
    assert(offset < _length);
    assert(offset + length <= _length);
    if (offset >= _length) {
IOLog("IOGMD(%p): rB = o%lx, l%lx\n", this, offset, length);	// @@@ gvdl
        return 0;
    }

    remaining = length = min(length, _length - offset);
    while (remaining) {	// (process another target segment?)
        addr64_t	srcAddr64;
        IOByteCount	srcLen;

        srcAddr64 = getPhysicalSegment64(offset, &srcLen);
        if (!srcAddr64)
            break;

        // Clip segment length to remaining
        if (srcLen > remaining)
            srcLen = remaining;

        copypv(srcAddr64, dstAddr, srcLen,
                            cppvPsrc | cppvFsnk | cppvKmap);

        dstAddr   += srcLen;
        offset    += srcLen;
        remaining -= srcLen;
    }

    assert(!remaining);

    return length - remaining;
}

IOByteCount IOMemoryDescriptor::writeBytes
                (IOByteCount offset, const void *bytes, IOByteCount length)
{
    addr64_t srcAddr = (addr64_t) (UInt32) bytes;
    IOByteCount remaining;

    // Assert that this entire I/O is withing the available range
    assert(offset < _length);
    assert(offset + length <= _length);

    assert( !(kIOMemoryPreparedReadOnly & _flags) );

    if ( (kIOMemoryPreparedReadOnly & _flags) || offset >= _length) {
IOLog("IOGMD(%p): wB = o%lx, l%lx\n", this, offset, length);	// @@@ gvdl
        return 0;
    }

    remaining = length = min(length, _length - offset);
    while (remaining) {	// (process another target segment?)
        addr64_t    dstAddr64;
        IOByteCount dstLen;

        dstAddr64 = getPhysicalSegment64(offset, &dstLen);
        if (!dstAddr64)
            break;

        // Clip segment length to remaining
        if (dstLen > remaining)
            dstLen = remaining;

        copypv(srcAddr, (addr64_t) dstAddr64, dstLen,
                            cppvPsnk | cppvFsnk | cppvNoModSnk | cppvKmap);

        srcAddr   += dstLen;
        offset    += dstLen;
        remaining -= dstLen;
    }

    assert(!remaining);

    return length - remaining;
}

// osfmk/device/iokit_rpc.c
extern "C" unsigned int IODefaultCacheBits(addr64_t pa);

/* DEPRECATED */ void IOGeneralMemoryDescriptor::setPosition(IOByteCount position)
/* DEPRECATED */ {
                    panic("IOGMD::setPosition deprecated");
/* DEPRECATED */ }

IOPhysicalAddress IOGeneralMemoryDescriptor::getPhysicalSegment
                        (IOByteCount offset, IOByteCount *lengthOfSegment)
{
    IOPhysicalAddress address = 0;
    IOPhysicalLength  length  = 0;

//  assert(offset <= _length);
    if (offset < _length) // (within bounds?)
    {
        if ( (_flags & kIOMemoryTypeMask) == kIOMemoryTypePhysical) {
            unsigned int ind;

            // Physical address based memory descriptor

            // Find offset within descriptor and make it relative
            // to the current _range.
            for (ind = 0 ; offset >= _ranges.p[ind].length; ind++ )
                offset -= _ranges.p[ind].length;
    
            IOPhysicalRange cur = _ranges.p[ind];
            address = cur.address + offset;
            length  = cur.length  - offset;

            // see how far we can coalesce ranges
            for (++ind; ind < _rangesCount; ind++) {
                cur =  _ranges.p[ind];
        
                if (address + length != cur.address)
                    break;
    
                length += cur.length;
            }

            // @@@ gvdl: should assert(address);
            // but can't as NVidia GeForce creates a bogus physical mem
            {
                assert(address || /*nvidia*/(!_ranges.p[0].address && 1 == _rangesCount));
            }
            assert(length);
        }
        else do {
            // We need wiring & we are wired.
            assert(_wireCount);

            if (!_wireCount)
	    {
		panic("IOGMD: not wired for getPhysicalSegment()");
                continue;
	    }

            assert(_memoryEntries);

            ioGMDData * dataP = getDataP(_memoryEntries);
            const ioPLBlock *ioplList = getIOPLList(dataP);
            UInt ind, numIOPLs = getNumIOPL(dataP, _memoryEntries->getLength());
            upl_page_info_t *pageList = getPageList(dataP);

            assert(numIOPLs > 0);

            // Scan through iopl info blocks looking for block containing offset
            for (ind = 1; ind < numIOPLs; ind++) {
                if (offset < ioplList[ind].fIOMDOffset)
                    break;
            }

            // Go back to actual range as search goes past it
            ioPLBlock ioplInfo = ioplList[ind - 1];

            if (ind < numIOPLs)
                length = ioplList[ind].fIOMDOffset;
            else
                length = _length;
            length -= offset;			// Remainder within iopl

            // Subtract offset till this iopl in total list
            offset -= ioplInfo.fIOMDOffset;

            // This is a mapped IOPL so we just need to compute an offset
            // relative to the mapped base.
            if (ioplInfo.fMappedBase) {
                offset += (ioplInfo.fPageOffset & PAGE_MASK);
                address = ptoa_32(ioplInfo.fMappedBase) + offset;
                continue;
            }

            // Currently the offset is rebased into the current iopl.
            // Now add the iopl 1st page offset.
            offset += ioplInfo.fPageOffset;

            // For external UPLs the fPageInfo field points directly to
            // the upl's upl_page_info_t array.
            if (ioplInfo.fFlags & kIOPLExternUPL)
                pageList = (upl_page_info_t *) ioplInfo.fPageInfo;
            else
                pageList = &pageList[ioplInfo.fPageInfo];

            // Check for direct device non-paged memory
            if ( ioplInfo.fFlags & kIOPLOnDevice ) {
                address = ptoa_32(pageList->phys_addr) + offset;
                continue;
            }

            // Now we need compute the index into the pageList
            ind = atop_32(offset);
            offset &= PAGE_MASK;

            IOPhysicalAddress pageAddr = pageList[ind].phys_addr;
            address = ptoa_32(pageAddr) + offset;

            // Check for the remaining data in this upl being longer than the
            // remainder on the current page.  This should be checked for
            // contiguous pages
            if (length > PAGE_SIZE - offset) {
                // See if the next page is contiguous.  Stop looking when we hit
                // the end of this upl, which is indicated by the
                // contigLength >= length.
                IOByteCount contigLength = PAGE_SIZE - offset;

                // Look for contiguous segment
                while (contigLength < length
                &&     ++pageAddr == pageList[++ind].phys_addr) {
                    contigLength += PAGE_SIZE;
                }
                if (length > contigLength)
                    length = contigLength;
            }
    
            assert(address);
            assert(length);

        } while (0);

        if (!address)
            length = 0;
    }

    if (lengthOfSegment)
        *lengthOfSegment = length;

    return address;
}

addr64_t IOMemoryDescriptor::getPhysicalSegment64
                        (IOByteCount offset, IOByteCount *lengthOfSegment)
{
    IOPhysicalAddress phys32;
    IOByteCount	      length;
    addr64_t 	      phys64;

    phys32 = getPhysicalSegment(offset, lengthOfSegment);
    if (!phys32)
	return 0;

    if (gIOSystemMapper)
    {
	IOByteCount origLen;

	phys64 = gIOSystemMapper->mapAddr(phys32);
	origLen = *lengthOfSegment;
	length = page_size - (phys64 & (page_size - 1));
	while ((length < origLen)
	    && ((phys64 + length) == gIOSystemMapper->mapAddr(phys32 + length)))
	    length += page_size;
	if (length > origLen)
	    length = origLen;

	*lengthOfSegment = length;
    }
    else
	phys64 = (addr64_t) phys32;

    return phys64;
}

IOPhysicalAddress IOGeneralMemoryDescriptor::getSourceSegment
                            (IOByteCount offset, IOByteCount *lengthOfSegment)
{
    IOPhysicalAddress address = 0;
    IOPhysicalLength  length  = 0;

    assert(offset <= _length);

    if ( (_flags & kIOMemoryTypeMask) == kIOMemoryTypeUPL)
	return super::getSourceSegment( offset, lengthOfSegment );

    if ( offset < _length ) // (within bounds?)
    {
        unsigned rangesIndex = 0;

        for ( ; offset >= _ranges.v[rangesIndex].length; rangesIndex++ )
        {
            offset -= _ranges.v[rangesIndex].length; // (make offset relative)
        }

        address = _ranges.v[rangesIndex].address + offset;
        length  = _ranges.v[rangesIndex].length  - offset;

        for ( ++rangesIndex; rangesIndex < _rangesCount; rangesIndex++ )
        {
            if ( address + length != _ranges.v[rangesIndex].address )  break;

            length += _ranges.v[rangesIndex].length; // (coalesce ranges)
        }

        assert(address);
        if ( address == 0 )  length = 0;
    }

    if ( lengthOfSegment )  *lengthOfSegment = length;

    return address;
}

/* DEPRECATED */ /* USE INSTEAD: map(), readBytes(), writeBytes() */
/* DEPRECATED */ void * IOGeneralMemoryDescriptor::getVirtualSegment(IOByteCount offset,
/* DEPRECATED */ 							IOByteCount * lengthOfSegment)
/* DEPRECATED */ {
                    if (_task == kernel_task)
                        return (void *) getSourceSegment(offset, lengthOfSegment);
                    else
                        panic("IOGMD::getVirtualSegment deprecated");

                    return 0;
/* DEPRECATED */ }
/* DEPRECATED */ /* USE INSTEAD: map(), readBytes(), writeBytes() */

IOReturn IOGeneralMemoryDescriptor::wireVirtual(IODirection forDirection)
{
    IOReturn error = kIOReturnNoMemory;
    ioGMDData *dataP;
    ppnum_t mapBase = 0;
    IOMapper *mapper;

    assert(!_wireCount);

    dataP = getDataP(_memoryEntries);
    mapper = dataP->fMapper;
    if (mapper && _pages)
        mapBase = mapper->iovmAlloc(_pages);

    // Note that appendBytes(NULL) zeros the data up to the
    // desired length.
    _memoryEntries->appendBytes(0, dataP->fPageCnt * sizeof(upl_page_info_t));
    dataP = 0;	// May no longer be valid so lets not get tempted.

    if (forDirection == kIODirectionNone)
        forDirection = _direction;

    int uplFlags;    // This Mem Desc's default flags for upl creation
    switch (forDirection)
    {
    case kIODirectionOut:
        // Pages do not need to be marked as dirty on commit
        uplFlags = UPL_COPYOUT_FROM;
        _flags |= kIOMemoryPreparedReadOnly;
        break;

    case kIODirectionIn:
    default:
        uplFlags = 0;	// i.e. ~UPL_COPYOUT_FROM
        break;
    }
    uplFlags |= UPL_SET_IO_WIRE | UPL_SET_LITE;

    //
    // Check user read/write access to the data buffer.
    //
    unsigned int pageIndex = 0;
    IOByteCount mdOffset = 0;
    vm_map_t curMap;
    if (_task == kernel_task && (kIOMemoryBufferPageable & _flags))
        curMap = 0;
    else
        { curMap = get_task_map(_task); }

    for (UInt range = 0; range < _rangesCount; range++) {
        ioPLBlock iopl;
        IOVirtualRange curRange = _ranges.v[range];
        vm_address_t startPage;
        IOByteCount numBytes;

        startPage = trunc_page_32(curRange.address);
        iopl.fPageOffset = (short) curRange.address & PAGE_MASK;
	if (mapper)
	    iopl.fMappedBase = mapBase + pageIndex;
	else
	    iopl.fMappedBase = 0;
        numBytes = iopl.fPageOffset + curRange.length;

        while (numBytes) {
            dataP = getDataP(_memoryEntries);
            vm_map_t theMap =
                (curMap)? curMap 
                        : IOPageableMapForAddress(startPage);
            upl_page_info_array_t pageInfo = getPageList(dataP);
            int ioplFlags = uplFlags;
            upl_page_list_ptr_t baseInfo = &pageInfo[pageIndex];

            vm_size_t ioplSize = round_page_32(numBytes);
            unsigned int numPageInfo = atop_32(ioplSize);
            error = vm_map_get_upl(theMap, 
                                   startPage,
                                   &ioplSize,
                                   &iopl.fIOPL,
                                   baseInfo,
                                   &numPageInfo,
                                   &ioplFlags,
                                   false);
            assert(ioplSize);
            if (error != KERN_SUCCESS)
                goto abortExit;

            error = kIOReturnNoMemory;

            if (baseInfo->device) {
                numPageInfo = 1;
                iopl.fFlags  = kIOPLOnDevice;
                // Don't translate device memory at all 
		if (mapper && mapBase) {
		    mapper->iovmFree(mapBase, _pages);
		    mapBase = 0;
		    iopl.fMappedBase = 0;
		}
            }
            else {
                iopl.fFlags = 0;
                if (mapper)
                    mapper->iovmInsert(mapBase, pageIndex,
                                       baseInfo, numPageInfo);
            }

            iopl.fIOMDOffset = mdOffset;
            iopl.fPageInfo = pageIndex;

	    if (_flags & kIOMemoryAutoPrepare)
	    {
		kernel_upl_commit(iopl.fIOPL, 0, 0);
		iopl.fIOPL = 0;
	    }

            if (!_memoryEntries->appendBytes(&iopl, sizeof(iopl))) {
                // Clean up partial created and unsaved iopl
		if (iopl.fIOPL)
		    kernel_upl_abort(iopl.fIOPL, 0);
                goto abortExit;
            }

            // Check for a multiple iopl's in one virtual range
            pageIndex += numPageInfo;
            mdOffset -= iopl.fPageOffset;
            if (ioplSize < numBytes) {
                numBytes -= ioplSize;
                startPage += ioplSize;
                mdOffset += ioplSize;
                iopl.fPageOffset = 0;
		if (mapper)
		    iopl.fMappedBase = mapBase + pageIndex;
            }
            else {
                mdOffset += numBytes;
                break;
            }
        }
    }

    return kIOReturnSuccess;

abortExit:
    {
        dataP = getDataP(_memoryEntries);
        UInt done = getNumIOPL(dataP, _memoryEntries->getLength());
        ioPLBlock *ioplList = getIOPLList(dataP);
    
        for (UInt range = 0; range < done; range++)
	{
	    if (ioplList[range].fIOPL)
		kernel_upl_abort(ioplList[range].fIOPL, 0);
	}

        if (mapper && mapBase)
            mapper->iovmFree(mapBase, _pages);
    }

    return error;
}

/*
 * prepare
 *
 * Prepare the memory for an I/O transfer.  This involves paging in
 * the memory, if necessary, and wiring it down for the duration of
 * the transfer.  The complete() method completes the processing of
 * the memory after the I/O transfer finishes.  This method needn't
 * called for non-pageable memory.
 */
IOReturn IOGeneralMemoryDescriptor::prepare(IODirection forDirection)
{
    IOReturn error = kIOReturnSuccess;

    if (!_wireCount && (_flags & kIOMemoryTypeMask) == kIOMemoryTypeVirtual) {
        error = wireVirtual(forDirection);
        if (error)
            return error;
    }

    _wireCount++;

    return kIOReturnSuccess;
}

/*
 * complete
 *
 * Complete processing of the memory after an I/O transfer finishes.
 * This method should not be called unless a prepare was previously
 * issued; the prepare() and complete() must occur in pairs, before
 * before and after an I/O transfer involving pageable memory.
 */
 
IOReturn IOGeneralMemoryDescriptor::complete(IODirection /* forDirection */)
{
    assert(_wireCount);

    if (!_wireCount)
        return kIOReturnSuccess;

    _wireCount--;
    if (!_wireCount) {
        if ((_flags & kIOMemoryTypeMask) == kIOMemoryTypePhysical) {
            /* kIOMemoryTypePhysical */
            // DO NOTHING
        }
        else {
            ioGMDData * dataP = getDataP(_memoryEntries);
            ioPLBlock *ioplList = getIOPLList(dataP);
	    UInt count = getNumIOPL(dataP, _memoryEntries->getLength());

            if (dataP->fMapper && _pages && ioplList[0].fMappedBase)
                dataP->fMapper->iovmFree(ioplList[0].fMappedBase, _pages);

            // Only complete iopls that we created which are for TypeVirtual
            if ( (_flags & kIOMemoryTypeMask) == kIOMemoryTypeVirtual) {
                for (UInt ind = 0; ind < count; ind++)
		    if (ioplList[ind].fIOPL)
			kernel_upl_commit(ioplList[ind].fIOPL, 0, 0);
            }

            (void) _memoryEntries->initWithBytes(dataP, sizeof(ioGMDData)); // == setLength()
        }
    }
    return kIOReturnSuccess;
}

IOReturn IOGeneralMemoryDescriptor::doMap(
	vm_map_t		addressMap,
	IOVirtualAddress *	atAddress,
	IOOptionBits		options,
	IOByteCount		sourceOffset,
	IOByteCount		length )
{
    kern_return_t kr;
    ipc_port_t sharedMem = (ipc_port_t) _memEntry;

    // mapping source == dest? (could be much better)
    if( _task && (addressMap == get_task_map(_task)) && (options & kIOMapAnywhere)
	&& (1 == _rangesCount) && (0 == sourceOffset)
	&& (length <= _ranges.v[0].length) ) {
	    *atAddress = _ranges.v[0].address;
	    return( kIOReturnSuccess );
    }

    if( 0 == sharedMem) {

        vm_size_t size = _pages << PAGE_SHIFT;

        if( _task) {
#ifndef i386
            vm_size_t actualSize = size;
            kr = mach_make_memory_entry( get_task_map(_task),
                        &actualSize, _ranges.v[0].address,
                        VM_PROT_READ | VM_PROT_WRITE, &sharedMem,
                        NULL );

            if( (KERN_SUCCESS == kr) && (actualSize != round_page_32(size))) {
#if IOASSERT
                IOLog("mach_make_memory_entry_64 (%08x) size (%08lx:%08x)\n",
                            _ranges.v[0].address, (UInt32)actualSize, size);
#endif
                kr = kIOReturnVMError;
                ipc_port_release_send( sharedMem );
            }

            if( KERN_SUCCESS != kr)
#endif /* i386 */
                sharedMem = MACH_PORT_NULL;

        } else do {

            memory_object_t 	pager;
	    unsigned int    	flags = 0;
    	    addr64_t		pa;
    	    IOPhysicalLength	segLen;

	    pa = getPhysicalSegment64( sourceOffset, &segLen );

            if( !reserved) {
                reserved = IONew( ExpansionData, 1 );
                if( !reserved)
                    continue;
            }
            reserved->pagerContig = (1 == _rangesCount);
	    reserved->memory = this;

	    /*What cache mode do we need*/
            switch(options & kIOMapCacheMask ) {

		case kIOMapDefaultCache:
		default:
		    flags = IODefaultCacheBits(pa);
		    break;
	
		case kIOMapInhibitCache:
		    flags = DEVICE_PAGER_CACHE_INHIB | 
				    DEVICE_PAGER_COHERENT | DEVICE_PAGER_GUARDED;
		    break;
	
		case kIOMapWriteThruCache:
		    flags = DEVICE_PAGER_WRITE_THROUGH |
				    DEVICE_PAGER_COHERENT | DEVICE_PAGER_GUARDED;
		    break;

		case kIOMapCopybackCache:
		    flags = DEVICE_PAGER_COHERENT;
		    break;

		case kIOMapWriteCombineCache:
		    flags = DEVICE_PAGER_CACHE_INHIB |
				    DEVICE_PAGER_COHERENT;
		    break;
            }

	    flags |= reserved->pagerContig ? DEVICE_PAGER_CONTIGUOUS : 0;

            pager = device_pager_setup( (memory_object_t) 0, (int) reserved, 
								size, flags);
            assert( pager );

            if( pager) {
                kr = mach_memory_object_memory_entry_64( (host_t) 1, false /*internal*/, 
                            size, VM_PROT_READ | VM_PROT_WRITE, pager, &sharedMem );

                assert( KERN_SUCCESS == kr );
                if( KERN_SUCCESS != kr) {
		    device_pager_deallocate( pager );
                    pager = MACH_PORT_NULL;
                    sharedMem = MACH_PORT_NULL;
                }
            }
	    if( pager && sharedMem)
		reserved->devicePager    = pager;
	    else {
		IODelete( reserved, ExpansionData, 1 );
		reserved = 0;
	    }

        } while( false );

        _memEntry = (void *) sharedMem;
    }

#ifndef i386
    if( 0 == sharedMem)
      kr = kIOReturnVMError;
    else
#endif
      kr = super::doMap( addressMap, atAddress,
                           options, sourceOffset, length );

    return( kr );
}

IOReturn IOGeneralMemoryDescriptor::doUnmap(
	vm_map_t		addressMap,
	IOVirtualAddress	logical,
	IOByteCount		length )
{
    // could be much better
    if( _task && (addressMap == get_task_map(_task)) && (1 == _rangesCount)
	 && (logical == _ranges.v[0].address)
	 && (length <= _ranges.v[0].length) )
	    return( kIOReturnSuccess );

    return( super::doUnmap( addressMap, logical, length ));
}

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

extern "C" {
// osfmk/device/iokit_rpc.c
extern kern_return_t IOMapPages( vm_map_t map, vm_offset_t va, vm_offset_t pa,
                                 vm_size_t length, unsigned int mapFlags);
extern kern_return_t IOUnmapPages(vm_map_t map, vm_offset_t va, vm_size_t length);
};

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

OSDefineMetaClassAndAbstractStructors( IOMemoryMap, OSObject )

/* inline function implementation */
IOPhysicalAddress IOMemoryMap::getPhysicalAddress()
    { return( getPhysicalSegment( 0, 0 )); }

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

class _IOMemoryMap : public IOMemoryMap
{
    OSDeclareDefaultStructors(_IOMemoryMap)

    IOMemoryDescriptor * memory;
    IOMemoryMap *	superMap;
    IOByteCount		offset;
    IOByteCount		length;
    IOVirtualAddress	logical;
    task_t		addressTask;
    vm_map_t		addressMap;
    IOOptionBits	options;

protected:
    virtual void taggedRelease(const void *tag = 0) const;
    virtual void free();

public:

    // IOMemoryMap methods
    virtual IOVirtualAddress 	getVirtualAddress();
    virtual IOByteCount 	getLength();
    virtual task_t		getAddressTask();
    virtual IOMemoryDescriptor * getMemoryDescriptor();
    virtual IOOptionBits 	getMapOptions();

    virtual IOReturn 		unmap();
    virtual void 		taskDied();

    virtual IOPhysicalAddress 	getPhysicalSegment(IOByteCount offset,
	       					   IOByteCount * length);

    // for IOMemoryDescriptor use
    _IOMemoryMap * copyCompatible(
		IOMemoryDescriptor *	owner,
                task_t			intoTask,
                IOVirtualAddress	toAddress,
                IOOptionBits		options,
                IOByteCount		offset,
                IOByteCount		length );

    bool initCompatible(
	IOMemoryDescriptor *	memory,
	IOMemoryMap *		superMap,
        IOByteCount		offset,
        IOByteCount		length );

    bool initWithDescriptor(
	IOMemoryDescriptor *	memory,
	task_t			intoTask,
	IOVirtualAddress	toAddress,
	IOOptionBits		options,
        IOByteCount		offset,
        IOByteCount		length );

    IOReturn redirect(
	task_t			intoTask, bool redirect );
};

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

#undef super
#define super IOMemoryMap

OSDefineMetaClassAndStructors(_IOMemoryMap, IOMemoryMap)

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

bool _IOMemoryMap::initCompatible(
	IOMemoryDescriptor *	_memory,
	IOMemoryMap *		_superMap,
        IOByteCount		_offset,
        IOByteCount		_length )
{

    if( !super::init())
	return( false);

    if( (_offset + _length) > _superMap->getLength())
	return( false);

    _memory->retain();
    memory	= _memory;
    _superMap->retain();
    superMap 	= _superMap;

    offset	= _offset;
    if( _length)
        length	= _length;
    else
        length	= _memory->getLength();

    options	= superMap->getMapOptions();
    logical	= superMap->getVirtualAddress() + offset;

    return( true );
}

bool _IOMemoryMap::initWithDescriptor(
        IOMemoryDescriptor *	_memory,
        task_t			intoTask,
        IOVirtualAddress	toAddress,
        IOOptionBits		_options,
        IOByteCount		_offset,
        IOByteCount		_length )
{
    bool	ok;

    if( (!_memory) || (!intoTask) || !super::init())
	return( false);

    if( (_offset + _length) > _memory->getLength())
	return( false);

    addressMap  = get_task_map(intoTask);
    if( !addressMap)
	return( false);
    vm_map_reference(addressMap);

    _memory->retain();
    memory	= _memory;

    offset	= _offset;
    if( _length)
        length	= _length;
    else
        length	= _memory->getLength();

    addressTask	= intoTask;
    logical	= toAddress;
    options 	= _options;

    if( options & kIOMapStatic)
	ok = true;
    else
	ok = (kIOReturnSuccess == memory->doMap( addressMap, &logical,
						 options, offset, length ));
    if( !ok) {
	logical = 0;
        memory->release();
        memory = 0;
        vm_map_deallocate(addressMap);
        addressMap = 0;
    }
    return( ok );
}

struct IOMemoryDescriptorMapAllocRef
{
    ipc_port_t		sharedMem;
    vm_size_t		size;
    vm_offset_t		mapped;
    IOByteCount		sourceOffset;
    IOOptionBits	options;
};

static kern_return_t IOMemoryDescriptorMapAlloc(vm_map_t map, void * _ref)
{
    IOMemoryDescriptorMapAllocRef * ref = (IOMemoryDescriptorMapAllocRef *)_ref;
    IOReturn			    err;

    do {
        if( ref->sharedMem) {
            vm_prot_t prot = VM_PROT_READ
                            | ((ref->options & kIOMapReadOnly) ? 0 : VM_PROT_WRITE);

            // set memory entry cache
            vm_prot_t memEntryCacheMode = prot | MAP_MEM_ONLY;
            switch (ref->options & kIOMapCacheMask)
            {
		case kIOMapInhibitCache:
                    SET_MAP_MEM(MAP_MEM_IO, memEntryCacheMode);
                    break;
	
		case kIOMapWriteThruCache:
                    SET_MAP_MEM(MAP_MEM_WTHRU, memEntryCacheMode);
                    break;

		case kIOMapWriteCombineCache:
                    SET_MAP_MEM(MAP_MEM_WCOMB, memEntryCacheMode);
                    break;

		case kIOMapCopybackCache:
                    SET_MAP_MEM(MAP_MEM_COPYBACK, memEntryCacheMode);
                    break;

		case kIOMapDefaultCache:
		default:
                    SET_MAP_MEM(MAP_MEM_NOOP, memEntryCacheMode);
                    break;
            }

            vm_size_t unused = 0;

            err = mach_make_memory_entry( NULL /*unused*/, &unused, 0 /*unused*/, 
                                            memEntryCacheMode, NULL, ref->sharedMem );
            if (KERN_SUCCESS != err)
                IOLog("MAP_MEM_ONLY failed %d\n", err);

            err = vm_map( map,
                            &ref->mapped,
                            ref->size, 0 /* mask */, 
                            (( ref->options & kIOMapAnywhere ) ? VM_FLAGS_ANYWHERE : VM_FLAGS_FIXED)
                            | VM_MAKE_TAG(VM_MEMORY_IOKIT), 
                            ref->sharedMem, ref->sourceOffset,
                            false, // copy
                            prot, // cur
                            prot, // max
                            VM_INHERIT_NONE);
    
            if( KERN_SUCCESS != err) {
                ref->mapped = 0;
                continue;
            }
    
        } else {
    
            err = vm_allocate( map, &ref->mapped, ref->size,
                            ((ref->options & kIOMapAnywhere) ? VM_FLAGS_ANYWHERE : VM_FLAGS_FIXED)
                            | VM_MAKE_TAG(VM_MEMORY_IOKIT) );
    
            if( KERN_SUCCESS != err) {
                ref->mapped = 0;
                continue;
            }
    
            // we have to make sure that these guys don't get copied if we fork.
            err = vm_inherit( map, ref->mapped, ref->size, VM_INHERIT_NONE);
            assert( KERN_SUCCESS == err );
        }

    } while( false );

    return( err );
}


IOReturn IOMemoryDescriptor::doMap(
	vm_map_t		addressMap,
	IOVirtualAddress *	atAddress,
	IOOptionBits		options,
	IOByteCount		sourceOffset,
	IOByteCount		length )
{
    IOReturn		err = kIOReturnSuccess;
    memory_object_t	pager;
    vm_address_t	logical;
    IOByteCount		pageOffset;
    IOPhysicalAddress	sourceAddr;
    IOMemoryDescriptorMapAllocRef	ref;

    ref.sharedMem	= (ipc_port_t) _memEntry;
    ref.sourceOffset	= sourceOffset;
    ref.options		= options;

    do {

        if( 0 == length)
            length = getLength();

        sourceAddr = getSourceSegment( sourceOffset, NULL );
        assert( sourceAddr );
        pageOffset = sourceAddr - trunc_page_32( sourceAddr );

        ref.size = round_page_32( length + pageOffset );

        logical = *atAddress;
        if( options & kIOMapAnywhere) 
            // vm_map looks for addresses above here, even when VM_FLAGS_ANYWHERE
            ref.mapped = 0;
        else {
            ref.mapped = trunc_page_32( logical );
            if( (logical - ref.mapped) != pageOffset) {
                err = kIOReturnVMError;
                continue;
            }
        }

        if( ref.sharedMem && (addressMap == kernel_map) && (kIOMemoryBufferPageable & _flags))
            err = IOIteratePageableMaps( ref.size, &IOMemoryDescriptorMapAlloc, &ref );
        else
            err = IOMemoryDescriptorMapAlloc( addressMap, &ref );

        if( err != KERN_SUCCESS)
            continue;

        if( reserved)
            pager = (memory_object_t) reserved->devicePager;
        else
            pager = MACH_PORT_NULL;

        if( !ref.sharedMem || pager )
            err = handleFault( pager, addressMap, ref.mapped, sourceOffset, length, options );

    } while( false );

    if( err != KERN_SUCCESS) {
        if( ref.mapped)
            doUnmap( addressMap, ref.mapped, ref.size );
        *atAddress = NULL;
    } else
        *atAddress = ref.mapped + pageOffset;

    return( err );
}

enum {
    kIOMemoryRedirected	= 0x00010000
};

IOReturn IOMemoryDescriptor::handleFault(
        void *			_pager,
	vm_map_t		addressMap,
	IOVirtualAddress	address,
	IOByteCount		sourceOffset,
	IOByteCount		length,
        IOOptionBits		options )
{
    IOReturn		err = kIOReturnSuccess;
    memory_object_t	pager = (memory_object_t) _pager;
    vm_size_t		size;
    vm_size_t		bytes;
    vm_size_t		page;
    IOByteCount		pageOffset;
    IOPhysicalLength	segLen;
    addr64_t		physAddr;

    if( !addressMap) {

        if( kIOMemoryRedirected & _flags) {
#ifdef DEBUG
            IOLog("sleep mem redirect %p, %lx\n", this, sourceOffset);
#endif
            do {
	    	SLEEP;
            } while( kIOMemoryRedirected & _flags );
        }

        return( kIOReturnSuccess );
    }

    physAddr = getPhysicalSegment64( sourceOffset, &segLen );
    assert( physAddr );
    pageOffset = physAddr - trunc_page_64( physAddr );

    size = length + pageOffset;
    physAddr -= pageOffset;

    segLen += pageOffset;
    bytes = size;
    do {
	// in the middle of the loop only map whole pages
	if( segLen >= bytes)
	    segLen = bytes;
	else if( segLen != trunc_page_32( segLen))
	    err = kIOReturnVMError;
        if( physAddr != trunc_page_64( physAddr))
	    err = kIOReturnBadArgument;

#ifdef DEBUG
	if( kIOLogMapping & gIOKitDebug)
	    IOLog("_IOMemoryMap::map(%p) %08lx->%08qx:%08lx\n",
                addressMap, address + pageOffset, physAddr + pageOffset,
		segLen - pageOffset);
#endif





#ifdef i386  
	/* i386 doesn't support faulting on device memory yet */
	if( addressMap && (kIOReturnSuccess == err))
            err = IOMapPages( addressMap, address, (IOPhysicalAddress) physAddr, segLen, options );
        assert( KERN_SUCCESS == err );
	if( err)
	    break;
#endif

        if( pager) {
            if( reserved && reserved->pagerContig) {
                IOPhysicalLength	allLen;
                addr64_t		allPhys;

                allPhys = getPhysicalSegment64( 0, &allLen );
                assert( allPhys );
                err = device_pager_populate_object( pager, 0, allPhys >> PAGE_SHIFT, round_page_32(allLen) );

            } else {

                for( page = 0;
                     (page < segLen) && (KERN_SUCCESS == err);
                     page += page_size) {
                        err = device_pager_populate_object(pager, sourceOffset + page,
                        	(ppnum_t)((physAddr + page) >> PAGE_SHIFT), page_size);
                }
            }
            assert( KERN_SUCCESS == err );
            if( err)
                break;
        }
#ifndef i386
	/*  *** ALERT *** */
	/*  *** Temporary Workaround *** */

	/* This call to vm_fault causes an early pmap level resolution	*/
	/* of the mappings created above.  Need for this is in absolute	*/
	/* violation of the basic tenet that the pmap layer is a cache.	*/
	/* Further, it implies a serious I/O architectural violation on	*/
	/* the part of some user of the mapping.  As of this writing, 	*/
	/* the call to vm_fault is needed because the NVIDIA driver 	*/
	/* makes a call to pmap_extract.  The NVIDIA driver needs to be	*/
	/* fixed as soon as possible.  The NVIDIA driver should not 	*/
	/* need to query for this info as it should know from the doMap	*/
	/* call where the physical memory is mapped.  When a query is 	*/
	/* necessary to find a physical mapping, it should be done 	*/
	/* through an iokit call which includes the mapped memory 	*/
	/* handle.  This is required for machine architecture independence.*/

	if(!(kIOMemoryRedirected & _flags)) {
		vm_fault(addressMap, address, 3, FALSE, FALSE, NULL, 0);
	}

	/*  *** Temporary Workaround *** */
	/*  *** ALERT *** */
#endif
	sourceOffset += segLen - pageOffset;
	address += segLen;
	bytes -= segLen;
	pageOffset = 0;

    } while( bytes
	&& (physAddr = getPhysicalSegment64( sourceOffset, &segLen )));

    if( bytes)
        err = kIOReturnBadArgument;

    return( err );
}

IOReturn IOMemoryDescriptor::doUnmap(
	vm_map_t		addressMap,
	IOVirtualAddress	logical,
	IOByteCount		length )
{
    IOReturn	err;

#ifdef DEBUG
    if( kIOLogMapping & gIOKitDebug)
	kprintf("IOMemoryDescriptor::doUnmap(%x) %08x:%08x\n",
                addressMap, logical, length );
#endif

    if( true /* && (addressMap == kernel_map) || (addressMap == get_task_map(current_task()))*/) {

        if( _memEntry && (addressMap == kernel_map) && (kIOMemoryBufferPageable & _flags))
            addressMap = IOPageableMapForAddress( logical );

        err = vm_deallocate( addressMap, logical, length );

    } else
        err = kIOReturnSuccess;

    return( err );
}

IOReturn IOMemoryDescriptor::redirect( task_t safeTask, bool redirect )
{
    IOReturn		err;
    _IOMemoryMap *	mapping = 0;
    OSIterator *	iter;

    LOCK;

    do {
	if( (iter = OSCollectionIterator::withCollection( _mappings))) {
            while( (mapping = (_IOMemoryMap *) iter->getNextObject()))
                mapping->redirect( safeTask, redirect );

            iter->release();
        }
    } while( false );

    if( redirect)
        _flags |= kIOMemoryRedirected;
    else {
        _flags &= ~kIOMemoryRedirected;
        WAKEUP;
    }

    UNLOCK;

    // temporary binary compatibility
    IOSubMemoryDescriptor * subMem;
    if( (subMem = OSDynamicCast( IOSubMemoryDescriptor, this)))
        err = subMem->redirect( safeTask, redirect );
    else
        err = kIOReturnSuccess;

    return( err );
}

IOReturn IOSubMemoryDescriptor::redirect( task_t safeTask, bool redirect )
{
    return( _parent->redirect( safeTask, redirect ));
}

IOReturn _IOMemoryMap::redirect( task_t safeTask, bool redirect )
{
    IOReturn err = kIOReturnSuccess;

    if( superMap) {
//        err = ((_IOMemoryMap *)superMap)->redirect( safeTask, redirect );
    } else {

        LOCK;
        if( logical && addressMap
        && (get_task_map( safeTask) != addressMap)
        && (0 == (options & kIOMapStatic))) {

            IOUnmapPages( addressMap, logical, length );
            if( !redirect) {
                err = vm_deallocate( addressMap, logical, length );
                err = memory->doMap( addressMap, &logical,
                                     (options & ~kIOMapAnywhere) /*| kIOMapReserve*/,
                                     offset, length );
            } else
                err = kIOReturnSuccess;
#ifdef DEBUG
            IOLog("IOMemoryMap::redirect(%d, %p) %x:%lx from %p\n", redirect, this, logical, length, addressMap);
#endif
        }
        UNLOCK;
    }

    return( err );
}

IOReturn _IOMemoryMap::unmap( void )
{
    IOReturn	err;

    LOCK;

    if( logical && addressMap && (0 == superMap)
	&& (0 == (options & kIOMapStatic))) {

        err = memory->doUnmap( addressMap, logical, length );
        vm_map_deallocate(addressMap);
        addressMap = 0;

    } else
	err = kIOReturnSuccess;

    logical = 0;

    UNLOCK;

    return( err );
}

void _IOMemoryMap::taskDied( void )
{
    LOCK;
    if( addressMap) {
        vm_map_deallocate(addressMap);
        addressMap = 0;
    }
    addressTask	= 0;
    logical	= 0;
    UNLOCK;
}

// Overload the release mechanism.  All mappings must be a member
// of a memory descriptors _mappings set.  This means that we
// always have 2 references on a mapping.  When either of these mappings
// are released we need to free ourselves.
void _IOMemoryMap::taggedRelease(const void *tag) const
{
    LOCK;
    super::taggedRelease(tag, 2);
    UNLOCK;
}

void _IOMemoryMap::free()
{
    unmap();

    if( memory) {
        LOCK;
	memory->removeMapping( this);
	UNLOCK;
	memory->release();
    }

    if( superMap)
	superMap->release();

    super::free();
}

IOByteCount _IOMemoryMap::getLength()
{
    return( length );
}

IOVirtualAddress _IOMemoryMap::getVirtualAddress()
{
    return( logical);
}

task_t _IOMemoryMap::getAddressTask()
{
    if( superMap)
	return( superMap->getAddressTask());
    else
        return( addressTask);
}

IOOptionBits _IOMemoryMap::getMapOptions()
{
    return( options);
}

IOMemoryDescriptor * _IOMemoryMap::getMemoryDescriptor()
{
    return( memory );
}

_IOMemoryMap * _IOMemoryMap::copyCompatible(
		IOMemoryDescriptor *	owner,
                task_t			task,
                IOVirtualAddress	toAddress,
                IOOptionBits		_options,
                IOByteCount		_offset,
                IOByteCount		_length )
{
    _IOMemoryMap * mapping;

    if( (!task) || (task != getAddressTask()))
	return( 0 );
    if( (options ^ _options) & kIOMapReadOnly)
	return( 0 );
    if( (kIOMapDefaultCache != (_options & kIOMapCacheMask)) 
     && ((options ^ _options) & kIOMapCacheMask))
	return( 0 );

    if( (0 == (_options & kIOMapAnywhere)) && (logical != toAddress))
	return( 0 );

    if( _offset < offset)
	return( 0 );

    _offset -= offset;

    if( (_offset + _length) > length)
	return( 0 );

    if( (length == _length) && (!_offset)) {
        retain();
	mapping = this;

    } else {
        mapping = new _IOMemoryMap;
        if( mapping
        && !mapping->initCompatible( owner, this, _offset, _length )) {
            mapping->release();
            mapping = 0;
        }
    }

    return( mapping );
}

IOPhysicalAddress _IOMemoryMap::getPhysicalSegment( IOByteCount _offset,
	       					    IOPhysicalLength * length)
{
    IOPhysicalAddress	address;

    LOCK;
    address = memory->getPhysicalSegment( offset + _offset, length );
    UNLOCK;

    return( address );
}

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

#undef super
#define super OSObject

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

void IOMemoryDescriptor::initialize( void )
{
    if( 0 == gIOMemoryLock)
	gIOMemoryLock = IORecursiveLockAlloc();
}

void IOMemoryDescriptor::free( void )
{
    if( _mappings)
	_mappings->release();

    super::free();
}

IOMemoryMap * IOMemoryDescriptor::setMapping(
	task_t			intoTask,
	IOVirtualAddress	mapAddress,
	IOOptionBits		options )
{
    _IOMemoryMap *		map;

    map = new _IOMemoryMap;

    LOCK;

    if( map
     && !map->initWithDescriptor( this, intoTask, mapAddress,
                    options | kIOMapStatic, 0, getLength() )) {
	map->release();
	map = 0;
    }

    addMapping( map);

    UNLOCK;

    return( map);
}

IOMemoryMap * IOMemoryDescriptor::map( 
	IOOptionBits		options )
{

    return( makeMapping( this, kernel_task, 0,
			options | kIOMapAnywhere,
			0, getLength() ));
}

IOMemoryMap * IOMemoryDescriptor::map(
	task_t			intoTask,
	IOVirtualAddress	toAddress,
	IOOptionBits		options,
	IOByteCount		offset,
	IOByteCount		length )
{
    if( 0 == length)
	length = getLength();

    return( makeMapping( this, intoTask, toAddress, options, offset, length ));
}

IOMemoryMap * IOMemoryDescriptor::makeMapping(
	IOMemoryDescriptor *	owner,
	task_t			intoTask,
	IOVirtualAddress	toAddress,
	IOOptionBits		options,
	IOByteCount		offset,
	IOByteCount		length )
{
    _IOMemoryMap *	mapping = 0;
    OSIterator *	iter;

    LOCK;

    do {
	// look for an existing mapping
	if( (iter = OSCollectionIterator::withCollection( _mappings))) {

            while( (mapping = (_IOMemoryMap *) iter->getNextObject())) {

		if( (mapping = mapping->copyCompatible( 
					owner, intoTask, toAddress,
					options | kIOMapReference,
					offset, length )))
		    break;
            }
            iter->release();
            if( mapping)
                continue;
        }


	if( mapping || (options & kIOMapReference))
	    continue;

	owner = this;

        mapping = new _IOMemoryMap;
	if( mapping
	&& !mapping->initWithDescriptor( owner, intoTask, toAddress, options,
			   offset, length )) {
#ifdef DEBUG
	    IOLog("Didn't make map %08lx : %08lx\n", offset, length );
#endif
	    mapping->release();
            mapping = 0;
	}

    } while( false );

    owner->addMapping( mapping);

    UNLOCK;

    return( mapping);
}

void IOMemoryDescriptor::addMapping(
	IOMemoryMap * mapping )
{
    if( mapping) {
        if( 0 == _mappings)
            _mappings = OSSet::withCapacity(1);
	if( _mappings )
	    _mappings->setObject( mapping );
    }
}

void IOMemoryDescriptor::removeMapping(
	IOMemoryMap * mapping )
{
    if( _mappings)
        _mappings->removeObject( mapping);
}

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

#undef super
#define super IOMemoryDescriptor

OSDefineMetaClassAndStructors(IOSubMemoryDescriptor, IOMemoryDescriptor)

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

bool IOSubMemoryDescriptor::initSubRange( IOMemoryDescriptor * parent,
					IOByteCount offset, IOByteCount length,
					IODirection direction )
{
    if( !parent)
	return( false);

    if( (offset + length) > parent->getLength())
	return( false);

    /*
     * We can check the _parent instance variable before having ever set it
     * to an initial value because I/O Kit guarantees that all our instance
     * variables are zeroed on an object's allocation.
     */

    if( !_parent) {
	if( !super::init())
	    return( false );
    } else {
	/*
	 * An existing memory descriptor is being retargeted to
	 * point to somewhere else.  Clean up our present state.
	 */

	_parent->release();
	_parent = 0;
    }

    parent->retain();
    _parent	= parent;
    _start	= offset;
    _length	= length;
    _direction  = direction;
    _tag	= parent->getTag();

    return( true );
}

void IOSubMemoryDescriptor::free( void )
{
    if( _parent)
	_parent->release();

    super::free();
}


IOPhysicalAddress IOSubMemoryDescriptor::getPhysicalSegment( IOByteCount offset,
						      	IOByteCount * length )
{
    IOPhysicalAddress	address;
    IOByteCount		actualLength;

    assert(offset <= _length);

    if( length)
        *length = 0;

    if( offset >= _length)
        return( 0 );

    address = _parent->getPhysicalSegment( offset + _start, &actualLength );

    if( address && length)
	*length = min( _length - offset, actualLength );

    return( address );
}

IOPhysicalAddress IOSubMemoryDescriptor::getSourceSegment( IOByteCount offset,
						      	   IOByteCount * length )
{
    IOPhysicalAddress	address;
    IOByteCount		actualLength;

    assert(offset <= _length);

    if( length)
        *length = 0;

    if( offset >= _length)
        return( 0 );

    address = _parent->getSourceSegment( offset + _start, &actualLength );

    if( address && length)
	*length = min( _length - offset, actualLength );

    return( address );
}

void * IOSubMemoryDescriptor::getVirtualSegment(IOByteCount offset,
					IOByteCount * lengthOfSegment)
{
    return( 0 );
}

IOByteCount IOSubMemoryDescriptor::readBytes(IOByteCount offset,
					void * bytes, IOByteCount length)
{
    IOByteCount	byteCount;

    assert(offset <= _length);

    if( offset >= _length)
        return( 0 );

    LOCK;
    byteCount = _parent->readBytes( _start + offset, bytes,
				min(length, _length - offset) );
    UNLOCK;

    return( byteCount );
}

IOByteCount IOSubMemoryDescriptor::writeBytes(IOByteCount offset,
				const void* bytes, IOByteCount length)
{
    IOByteCount	byteCount;

    assert(offset <= _length);

    if( offset >= _length)
        return( 0 );

    LOCK;
    byteCount = _parent->writeBytes( _start + offset, bytes,
				min(length, _length - offset) );
    UNLOCK;

    return( byteCount );
}

IOReturn IOSubMemoryDescriptor::prepare(
		IODirection forDirection)
{
    IOReturn	err;

    LOCK;
    err = _parent->prepare( forDirection);
    UNLOCK;

    return( err );
}

IOReturn IOSubMemoryDescriptor::complete(
		IODirection forDirection)
{
    IOReturn	err;

    LOCK;
    err = _parent->complete( forDirection);
    UNLOCK;

    return( err );
}

IOMemoryMap * IOSubMemoryDescriptor::makeMapping(
	IOMemoryDescriptor *	owner,
	task_t			intoTask,
	IOVirtualAddress	toAddress,
	IOOptionBits		options,
	IOByteCount		offset,
	IOByteCount		length )
{
    IOMemoryMap * mapping;

     mapping = (IOMemoryMap *) _parent->makeMapping(
					_parent, intoTask,
					toAddress - (_start + offset),
					options | kIOMapReference,
					_start + offset, length );

    if( !mapping)
        mapping = (IOMemoryMap *) _parent->makeMapping(
					_parent, intoTask,
					toAddress,
					options, _start + offset, length );

    if( !mapping)
	mapping = super::makeMapping( owner, intoTask, toAddress, options,
					offset, length );

    return( mapping );
}

/* ick */

bool
IOSubMemoryDescriptor::initWithAddress(void *      address,
                                    IOByteCount   length,
                                    IODirection direction)
{
    return( false );
}

bool
IOSubMemoryDescriptor::initWithAddress(vm_address_t address,
                                    IOByteCount    length,
                                    IODirection  direction,
                                    task_t       task)
{
    return( false );
}

bool
IOSubMemoryDescriptor::initWithPhysicalAddress(
				 IOPhysicalAddress	address,
				 IOByteCount		length,
				 IODirection      	direction )
{
    return( false );
}

bool
IOSubMemoryDescriptor::initWithRanges(
                                   	IOVirtualRange * ranges,
                                   	UInt32           withCount,
                                   	IODirection      direction,
                                   	task_t           task,
                                  	bool             asReference)
{
    return( false );
}

bool
IOSubMemoryDescriptor::initWithPhysicalRanges(	IOPhysicalRange * ranges,
                                        	UInt32           withCount,
                                        	IODirection      direction,
                                        	bool             asReference)
{
    return( false );
}

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

bool IOGeneralMemoryDescriptor::serialize(OSSerialize * s) const
{
    OSSymbol const *keys[2];
    OSObject *values[2];
    IOVirtualRange *vcopy;
    unsigned int index, nRanges;
    bool result;

    if (s == NULL) return false;
    if (s->previouslySerialized(this)) return true;

    // Pretend we are an array.
    if (!s->addXMLStartTag(this, "array")) return false;

    nRanges = _rangesCount;
    vcopy = (IOVirtualRange *) IOMalloc(sizeof(IOVirtualRange) * nRanges);
    if (vcopy == 0) return false;

    keys[0] = OSSymbol::withCString("address");
    keys[1] = OSSymbol::withCString("length");

    result = false;
    values[0] = values[1] = 0;

    // From this point on we can go to bail.

    // Copy the volatile data so we don't have to allocate memory
    // while the lock is held.
    LOCK;
    if (nRanges == _rangesCount) {
        for (index = 0; index < nRanges; index++) {
            vcopy[index] = _ranges.v[index];
        }
    } else {
	// The descriptor changed out from under us.  Give up.
        UNLOCK;
	result = false;
        goto bail;
    }
    UNLOCK;

    for (index = 0; index < nRanges; index++)
    {
	values[0] = OSNumber::withNumber(_ranges.v[index].address, sizeof(_ranges.v[index].address) * 8);
	if (values[0] == 0) {
	  result = false;
	  goto bail;
	}
	values[1] = OSNumber::withNumber(_ranges.v[index].length, sizeof(_ranges.v[index].length) * 8);
	if (values[1] == 0) {
	  result = false;
	  goto bail;
	}
        OSDictionary *dict = OSDictionary::withObjects((const OSObject **)values, (const OSSymbol **)keys, 2);
	if (dict == 0) {
	  result = false;
	  goto bail;
	}
	values[0]->release();
	values[1]->release();
	values[0] = values[1] = 0;

	result = dict->serialize(s);
	dict->release();
	if (!result) {
	  goto bail;
	}
    }
    result = s->addXMLEndTag("array");

 bail:
    if (values[0])
      values[0]->release();
    if (values[1])
      values[1]->release();
    if (keys[0])
      keys[0]->release();
    if (keys[1])
      keys[1]->release();
    if (vcopy)
        IOFree(vcopy, sizeof(IOVirtualRange) * nRanges);
    return result;
}

bool IOSubMemoryDescriptor::serialize(OSSerialize * s) const
{
    if (!s) {
	return (false);
    }
    if (s->previouslySerialized(this)) return true;

    // Pretend we are a dictionary.
    // We must duplicate the functionality of OSDictionary here
    // because otherwise object references will not work;
    // they are based on the value of the object passed to
    // previouslySerialized and addXMLStartTag.

    if (!s->addXMLStartTag(this, "dict")) return false;

    char const *keys[3] = {"offset", "length", "parent"};

    OSObject *values[3];
    values[0] = OSNumber::withNumber(_start, sizeof(_start) * 8);
    if (values[0] == 0)
	return false;
    values[1] = OSNumber::withNumber(_length, sizeof(_length) * 8);
    if (values[1] == 0) {
	values[0]->release();
	return false;
    }
    values[2] = _parent;

    bool result = true;
    for (int i=0; i<3; i++) {
        if (!s->addString("<key>") ||
	    !s->addString(keys[i]) ||
	    !s->addXMLEndTag("key") ||
	    !values[i]->serialize(s)) {
	  result = false;
	  break;
        }
    }
    values[0]->release();
    values[1]->release();
    if (!result) {
      return false;
    }

    return s->addXMLEndTag("dict");
}

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

OSMetaClassDefineReservedUsed(IOMemoryDescriptor, 0);
OSMetaClassDefineReservedUsed(IOMemoryDescriptor, 1);
OSMetaClassDefineReservedUsed(IOMemoryDescriptor, 2);
OSMetaClassDefineReservedUnused(IOMemoryDescriptor, 3);
OSMetaClassDefineReservedUnused(IOMemoryDescriptor, 4);
OSMetaClassDefineReservedUnused(IOMemoryDescriptor, 5);
OSMetaClassDefineReservedUnused(IOMemoryDescriptor, 6);
OSMetaClassDefineReservedUnused(IOMemoryDescriptor, 7);
OSMetaClassDefineReservedUnused(IOMemoryDescriptor, 8);
OSMetaClassDefineReservedUnused(IOMemoryDescriptor, 9);
OSMetaClassDefineReservedUnused(IOMemoryDescriptor, 10);
OSMetaClassDefineReservedUnused(IOMemoryDescriptor, 11);
OSMetaClassDefineReservedUnused(IOMemoryDescriptor, 12);
OSMetaClassDefineReservedUnused(IOMemoryDescriptor, 13);
OSMetaClassDefineReservedUnused(IOMemoryDescriptor, 14);
OSMetaClassDefineReservedUnused(IOMemoryDescriptor, 15);

/* ex-inline function implementation */
IOPhysicalAddress IOMemoryDescriptor::getPhysicalAddress()
        { return( getPhysicalSegment( 0, 0 )); }

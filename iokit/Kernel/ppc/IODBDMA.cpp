/*
 * Copyright (c) 1998-2000 Apple Computer, Inc. All rights reserved.
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
 * Copyright (c) 1997 Apple Computer, Inc.
 *
 *
 * HISTORY
 *
 * Simon Douglas  10 Nov 97
 * - first checked in, mostly from machdep/ppc/dbdma.c
 *
 */


#include <IOKit/ppc/IODBDMA.h>
#include <IOKit/IOLib.h>

void
IODBDMAStart( volatile IODBDMAChannelRegisters *registers, volatile IODBDMADescriptor *physicalDescPtr)
{

    if( ((int) physicalDescPtr) & 0xf)
	panic("IODBDMAStart: unaligned IODBDMADescriptor");

    eieio();
    IOSetDBDMAInterruptSelect(registers, 0xff000000);		// clear out interrupts

    IOSetDBDMAChannelControl( registers,
	IOClearDBDMAChannelControlBits( kdbdmaRun | kdbdmaPause | kdbdmaFlush | kdbdmaWake | kdbdmaDead | kdbdmaActive ));

    while( IOGetDBDMAChannelStatus( registers) & kdbdmaActive)
	eieio();

    IOSetDBDMACommandPtr( registers, (unsigned int) physicalDescPtr);

    IOSetDBDMAChannelControl( registers,
	IOSetDBDMAChannelControlBits( kdbdmaRun | kdbdmaWake ));

}

void
IODBDMAStop( volatile IODBDMAChannelRegisters *registers)
{

    IOSetDBDMAChannelControl( registers,
	  IOClearDBDMAChannelControlBits( kdbdmaRun )
	| IOSetDBDMAChannelControlBits(  kdbdmaFlush ));

    while( IOGetDBDMAChannelStatus( registers) & ( kdbdmaActive | kdbdmaFlush))
	eieio();

}

void
IODBDMAFlush( volatile IODBDMAChannelRegisters *registers)
{

    IOSetDBDMAChannelControl( registers,
	 IOSetDBDMAChannelControlBits(  kdbdmaFlush ));

    while( IOGetDBDMAChannelStatus( registers) & kdbdmaFlush)
	eieio();

}

void
IODBDMAReset( volatile IODBDMAChannelRegisters *registers)
{

    IOSetDBDMAChannelControl( registers,
	IOClearDBDMAChannelControlBits( kdbdmaRun | kdbdmaPause | kdbdmaFlush | kdbdmaWake | kdbdmaDead | kdbdmaActive ));

    while( IOGetDBDMAChannelStatus( registers) & kdbdmaActive)
	eieio();

}

void
IODBDMAContinue( volatile IODBDMAChannelRegisters *registers)
{

    IOSetDBDMAChannelControl( registers,
	  IOClearDBDMAChannelControlBits( kdbdmaPause | kdbdmaDead )
	| IOSetDBDMAChannelControlBits(  kdbdmaRun | kdbdmaWake ));

}

void
IODBDMAPause( volatile IODBDMAChannelRegisters *registers)
{

    IOSetDBDMAChannelControl( registers,
	 IOSetDBDMAChannelControlBits(  kdbdmaPause ));

    while( IOGetDBDMAChannelStatus( registers) & kdbdmaActive)
	eieio();

}

IOReturn
IOAllocatePhysicallyContiguousMemory(
		unsigned int /* size */, unsigned int /* options */,
		IOVirtualAddress * /* logical */,
		IOPhysicalAddress * /* physical */ )
{
#if 0
    IOReturn		err;
    vm_offset_t		mem;

    if( (size > 4096) || (options))
	return( kIOReturnUnsupported);

    mem = (vm_offset_t) IOMalloc( size);
    *logical = (IOVirtualAddress) mem;

    if( mem) {
	err = IOPhysicalFromVirtual( IOVmTaskSelf(), mem, (vm_offset_t *) physical);
	if( err)
	    IOFree( (char *)mem, size);

    } else {
	err = kIOReturnNoMemory;
	*physical = 0;
    }

    return( err);
#endif /* 0 */
	return (kIOReturnUnsupported);
}

IOReturn
IOFreePhysicallyContiguousMemory( IOVirtualAddress * logical, unsigned int size)
{
    IOFree( logical, size);
    return( kIOReturnSuccess);
}

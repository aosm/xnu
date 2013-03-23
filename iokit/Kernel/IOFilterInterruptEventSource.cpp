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

#include <IOKit/IOFilterInterruptEventSource.h>
#include <IOKit/IOService.h>
#include <IOKit/IOKitDebug.h>
#include <IOKit/IOTimeStamp.h>
#include <IOKit/IOWorkLoop.h>

#define super IOInterruptEventSource

OSDefineMetaClassAndStructors
    (IOFilterInterruptEventSource, IOInterruptEventSource)
OSMetaClassDefineReservedUnused(IOFilterInterruptEventSource, 0);
OSMetaClassDefineReservedUnused(IOFilterInterruptEventSource, 1);
OSMetaClassDefineReservedUnused(IOFilterInterruptEventSource, 2);
OSMetaClassDefineReservedUnused(IOFilterInterruptEventSource, 3);
OSMetaClassDefineReservedUnused(IOFilterInterruptEventSource, 4);
OSMetaClassDefineReservedUnused(IOFilterInterruptEventSource, 5);
OSMetaClassDefineReservedUnused(IOFilterInterruptEventSource, 6);
OSMetaClassDefineReservedUnused(IOFilterInterruptEventSource, 7);

/*
 * Implement the call throughs for the private protection conversion
 */
bool IOFilterInterruptEventSource::init(OSObject *inOwner,
                                        Action inAction,
                                        IOService *inProvider,
                                        int inIntIndex)
{
    return false;
}

IOInterruptEventSource *
IOFilterInterruptEventSource::interruptEventSource(OSObject *inOwner,
                                                   Action inAction,
                                                   IOService *inProvider,
                                                   int inIntIndex)
{
    return 0;
}

bool
IOFilterInterruptEventSource::init(OSObject *inOwner,
                                   Action inAction,
                                   Filter inFilterAction,
                                   IOService *inProvider,
                                   int inIntIndex)
{
    if ( !super::init(inOwner, inAction, inProvider, inIntIndex) )
        return false;

    if (!inFilterAction)
        return false;

    filterAction = inFilterAction;
    return true;
}

IOFilterInterruptEventSource *IOFilterInterruptEventSource
::filterInterruptEventSource(OSObject *inOwner,
                             Action inAction,
                             Filter inFilterAction,
                             IOService *inProvider,
                             int inIntIndex)
{
    IOFilterInterruptEventSource *me = new IOFilterInterruptEventSource;

    if (me
    && !me->init(inOwner, inAction, inFilterAction, inProvider, inIntIndex)) {
        me->release();
        return 0;
    }

    return me;
}

void IOFilterInterruptEventSource::signalInterrupt()
{
	bool trace = (gIOKitTrace & kIOTraceIntEventSource) ? true : false;

    producerCount++;

	if (trace)
	    IOTimeStampStartConstant(IODBG_INTES(IOINTES_SEMA), (uintptr_t) this, (uintptr_t) owner);
    
    signalWorkAvailable();
	
	if (trace)
	    IOTimeStampEndConstant(IODBG_INTES(IOINTES_SEMA), (uintptr_t) this, (uintptr_t) owner);
	
}


IOFilterInterruptEventSource::Filter
IOFilterInterruptEventSource::getFilterAction() const
{
    return filterAction;
}




void IOFilterInterruptEventSource::normalInterruptOccurred
    (void */*refcon*/, IOService */*prov*/, int /*source*/)
{
    bool filterRes;
	bool	trace = (gIOKitTrace & kIOTraceIntEventSource) ? true : false;

	if (trace)
		IOTimeStampStartConstant(IODBG_INTES(IOINTES_FILTER),
								 (uintptr_t) filterAction, (uintptr_t) owner, (uintptr_t) this, (uintptr_t) workLoop);

    // Call the filter.
    filterRes = (*filterAction)(owner, this);
	
	if (trace)
		IOTimeStampEndConstant(IODBG_INTES(IOINTES_FILTER),
							   (uintptr_t) filterAction, (uintptr_t) owner, (uintptr_t) this, (uintptr_t) workLoop);

    if (filterRes)
        signalInterrupt();
}

void IOFilterInterruptEventSource::disableInterruptOccurred
    (void */*refcon*/, IOService *prov, int source)
{
    bool filterRes;
	bool	trace = (gIOKitTrace & kIOTraceIntEventSource) ? true : false;

	if (trace)
		IOTimeStampStartConstant(IODBG_INTES(IOINTES_FILTER),
								 (uintptr_t) filterAction, (uintptr_t) owner, (uintptr_t) this, (uintptr_t) workLoop);

    // Call the filter.
    filterRes = (*filterAction)(owner, this);
	
	if (trace)
		IOTimeStampEndConstant(IODBG_INTES(IOINTES_FILTER),
							   (uintptr_t) filterAction, (uintptr_t) owner, (uintptr_t) this, (uintptr_t) workLoop);

    if (filterRes) {
        prov->disableInterrupt(source);	/* disable the interrupt */
        signalInterrupt();
    }
}

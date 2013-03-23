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
 * Copyright (c) 1999 Apple Computer, Inc.  All rights reserved.
 *
 */

#include <IOKit/assert.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include "RootDomainUserClient.h"
#include <IOKit/pwr_mgt/IOPMLibDefs.h>

#define super IOUserClient

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

OSDefineMetaClassAndStructors(RootDomainUserClient, IOUserClient)

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

RootDomainUserClient *RootDomainUserClient::withTask(task_t owningTask)
{
    RootDomainUserClient *me;

    me = new RootDomainUserClient;
    if(me) {
        if(!me->init()) {
            me->release();
            return NULL;
        }
        me->fTask = owningTask;
    }
    return me;
}

bool RootDomainUserClient::start( IOService * provider )
{
    assert(OSDynamicCast(IOPMrootDomain, provider));
    if(!super::start(provider))
        return false;
    fOwner = (IOPMrootDomain *)provider;

    // Got the owner, so initialize the call structures
    fMethods[kPMSetAggressiveness].object = provider;			// 0
    fMethods[kPMSetAggressiveness].func = (IOMethod)&IOPMrootDomain::setAggressiveness;
    fMethods[kPMSetAggressiveness].count0 = 2;
    fMethods[kPMSetAggressiveness].count1 = 0;
    fMethods[kPMSetAggressiveness].flags = kIOUCScalarIScalarO;

    fMethods[kPMGetAggressiveness].object = provider;			// 1
    fMethods[kPMGetAggressiveness].func = (IOMethod)&IOPMrootDomain::getAggressiveness;
    fMethods[kPMGetAggressiveness].count0 = 1;
    fMethods[kPMGetAggressiveness].count1 = 1;
    fMethods[kPMGetAggressiveness].flags = kIOUCScalarIScalarO;

    fMethods[kPMSleepSystem].object = provider;			// 2
    fMethods[kPMSleepSystem].func = (IOMethod)&IOPMrootDomain::sleepSystem;
    fMethods[kPMSleepSystem].count0 = 0;
    fMethods[kPMSleepSystem].count1 = 0;
    fMethods[kPMSleepSystem].flags = kIOUCScalarIScalarO;

    fMethods[kPMAllowPowerChange].object = provider;		// 3
    fMethods[kPMAllowPowerChange].func = (IOMethod)&IOPMrootDomain::allowPowerChange;
    fMethods[kPMAllowPowerChange].count0 = 1;
    fMethods[kPMAllowPowerChange].count1 = 0;
    fMethods[kPMAllowPowerChange].flags = kIOUCScalarIScalarO;

    fMethods[kPMCancelPowerChange].object = provider;		// 4
    fMethods[kPMCancelPowerChange].func = (IOMethod)&IOPMrootDomain::cancelPowerChange;
    fMethods[kPMCancelPowerChange].count0 = 1;
    fMethods[kPMCancelPowerChange].count1 = 0;
    fMethods[kPMCancelPowerChange].flags = kIOUCScalarIScalarO;

    return true;
}


IOReturn RootDomainUserClient::clientClose( void )
{
    detach( fOwner);

    return kIOReturnSuccess;
}

IOReturn RootDomainUserClient::clientDied( void )
{
    return( clientClose());
}

IOExternalMethod *
RootDomainUserClient::getExternalMethodForIndex( UInt32 index )
{
    if(index >= kNumPMMethods)
    	return NULL;
    else
        return &fMethods[index];
}

IOReturn
RootDomainUserClient::registerNotificationPort(
            mach_port_t port, UInt32 type )
{
    return kIOReturnUnsupported;
}


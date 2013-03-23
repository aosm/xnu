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

#include <IOKit/IOUserClient.h>
#include <IOKit/IOMessage.h>
#include <IOKit/system_management/IOWatchDogTimer.h>
#include <IOKit/pwr_mgt/RootDomain.h>


static IOReturn IOWatchDogTimerSleepHandler(void *target, void *refCon,
					    UInt32 messageType,
					    IOService *provider,
					    void *messageArgument,
					    vm_size_t argSize);


#define kWatchDogEnabledProperty     "IOWatchDogEnabled"


#define super IOService

OSDefineMetaClassAndAbstractStructors(IOWatchDogTimer, IOService);

OSMetaClassDefineReservedUnused(IOWatchDogTimer,  0);
OSMetaClassDefineReservedUnused(IOWatchDogTimer,  1);
OSMetaClassDefineReservedUnused(IOWatchDogTimer,  2);
OSMetaClassDefineReservedUnused(IOWatchDogTimer,  3);

bool IOWatchDogTimer::start(IOService *provider)
{
  if (!super::start(provider)) return false;
  
  notifier = registerSleepWakeInterest(IOWatchDogTimerSleepHandler, this);
  if (notifier == 0) return false;
  
  setProperty(kWatchDogEnabledProperty, kOSBooleanFalse);
  setWatchDogTimer(0);
  
  registerService();
  
  return true;
}

void IOWatchDogTimer::stop(IOService *provider)
{
  setWatchDogTimer(0);
  notifier->remove();
}

IOReturn IOWatchDogTimer::setProperties(OSObject *properties)
{
  OSNumber *theNumber;
  UInt32   theValue;
  IOReturn result;
  
  result = IOUserClient::clientHasPrivilege(current_task(),
					    kIOClientPrivilegeAdministrator);
  if (result != kIOReturnSuccess) return kIOReturnNotPrivileged;
  
  theNumber = OSDynamicCast(OSNumber, properties);
  if (theNumber == 0) return kIOReturnBadArgument;
  
  theValue = theNumber->unsigned32BitValue();
  if (theValue == 0) {
    setProperty(kWatchDogEnabledProperty, kOSBooleanFalse);
  } else {
    setProperty(kWatchDogEnabledProperty, kOSBooleanTrue);
  }
  
  setWatchDogTimer(theValue);
  
  return kIOReturnSuccess;
}

static IOReturn IOWatchDogTimerSleepHandler(void *target, void */*refCon*/,
					    UInt32 messageType,
					    IOService */*provider*/,
					    void *messageArgument,
					    vm_size_t /*argSize*/)
{
  IOWatchDogTimer *watchDogTimer = (IOWatchDogTimer *)target;
  sleepWakeNote *swNote = (sleepWakeNote *)messageArgument;
  
  if (messageType != kIOMessageSystemWillSleep) return kIOReturnUnsupported;
  
  watchDogTimer->setProperty(kWatchDogEnabledProperty, kOSBooleanFalse);
  watchDogTimer->setWatchDogTimer(0);
  
  swNote->returnValue = 0;
  acknowledgeSleepWakeNotification(swNote->powerRef);
  
  return kIOReturnSuccess;
}

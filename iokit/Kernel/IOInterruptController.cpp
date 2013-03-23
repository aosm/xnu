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
/*
 * Copyright (c) 1999 Apple Computer, Inc.  All rights reserved. 
 *
 *  DRI: Josh de Cesare
 *
 */


#if __ppc__
#include <ppc/proc_reg.h> 
#endif

#include <IOKit/IOLib.h>
#include <IOKit/IOService.h>
#include <IOKit/IOPlatformExpert.h>
#include <IOKit/IODeviceTreeSupport.h>
#include <IOKit/IOInterrupts.h>
#include <IOKit/IOInterruptController.h>


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

#define super IOService

OSDefineMetaClassAndAbstractStructors(IOInterruptController, IOService);

OSMetaClassDefineReservedUnused(IOInterruptController, 0);
OSMetaClassDefineReservedUnused(IOInterruptController, 1);
OSMetaClassDefineReservedUnused(IOInterruptController, 2);
OSMetaClassDefineReservedUnused(IOInterruptController, 3);
OSMetaClassDefineReservedUnused(IOInterruptController, 4);
OSMetaClassDefineReservedUnused(IOInterruptController, 5);

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

IOReturn IOInterruptController::registerInterrupt(IOService *nub, int source,
						  void *target,
						  IOInterruptHandler handler,
						  void *refCon)
{
  IOInterruptSource *interruptSources;
  long              vectorNumber;
  IOInterruptVector *vector;
  long              wasDisabledSoft;
  IOReturn          error;
  OSData            *vectorData;
  IOService         *originalNub;
  int               originalSource;
  IOOptionBits      options;
  bool              canBeShared, shouldBeShared, wasAlreadyRegisterd;
  
  interruptSources = nub->_interruptSources;
  vectorData = interruptSources[source].vectorData;
  vectorNumber = *(long *)vectorData->getBytesNoCopy();
  vector = &vectors[vectorNumber];
  
  // Get the lock for this vector.
  IOTakeLock(vector->interruptLock);
  
  // Check if the interrupt source can/should be shared.
  canBeShared = vectorCanBeShared(vectorNumber, vector);
  IODTGetInterruptOptions(nub, source, &options);
#if defined(__i386__) || defined(__x86_64__)
  int   interruptType;
  if (OSDynamicCast(IOPlatformDevice, getProvider()) &&
      (getInterruptType(nub, source, &interruptType) == kIOReturnSuccess) &&
      (kIOInterruptTypeLevel & interruptType))
  {
    options |= kIODTInterruptShared;
  }
#endif
  shouldBeShared = canBeShared && (options & kIODTInterruptShared);
  wasAlreadyRegisterd = vector->interruptRegistered;
  
  // If the vector is registered and can not be shared return error.
  if (wasAlreadyRegisterd && !canBeShared) {
    IOUnlock(vector->interruptLock);
    return kIOReturnNoResources;
  }
  
  // If this vector is already in use, and can be shared (implied),
  // or it is not registered and should be shared,
  // register as a shared interrupt.
  if (wasAlreadyRegisterd || shouldBeShared) {
    // If this vector is not already shared, break it out.
    if (vector->sharedController == 0) {
      // Make the IOShareInterruptController instance
      vector->sharedController = new IOSharedInterruptController;
      if (vector->sharedController == 0) {
        IOUnlock(vector->interruptLock);
        return kIOReturnNoMemory;
      }
      
      if (wasAlreadyRegisterd) {
	// Save the nub and source for the original consumer.
	originalNub = vector->nub;
	originalSource = vector->source;
	
	// Physically disable the interrupt, but mark it as being enabled in the hardware.
	// The interruptDisabledSoft now indicates the driver's request for enablement.
	disableVectorHard(vectorNumber, vector);
	vector->interruptDisabledHard = 0;
      }
      
      // Initialize the new shared interrupt controller.
      error = vector->sharedController->initInterruptController(this, vectorData);
      // If the IOSharedInterruptController could not be initalized,
      // if needed, put the original consumer's interrupt back to normal and
      // get rid of whats left of the shared controller.
      if (error != kIOReturnSuccess) {
	if (wasAlreadyRegisterd) enableInterrupt(originalNub, originalSource);
        vector->sharedController->release();
        vector->sharedController = 0;
        IOUnlock(vector->interruptLock);
        return error;
      }
      
      // If there was an original consumer try to register it on the shared controller.
      if (wasAlreadyRegisterd) {
	error = vector->sharedController->registerInterrupt(originalNub,
							    originalSource,
							    vector->target,
							    vector->handler,
							    vector->refCon);
	// If the original consumer could not be moved to the shared controller,
	// put the original consumor's interrupt back to normal and
	// get rid of whats left of the shared controller.
	if (error != kIOReturnSuccess) {
	  // Save the driver's interrupt enablement state.
	  wasDisabledSoft = vector->interruptDisabledSoft;
	  
	  // Make the interrupt really hard disabled.
	  vector->interruptDisabledSoft = 1;
	  vector->interruptDisabledHard = 1;
	  
	  // Enable the original consumer's interrupt if needed.
	  if (!wasDisabledSoft) originalNub->enableInterrupt(originalSource);
	  enableInterrupt(originalNub, originalSource);
	  
	  vector->sharedController->release();
	  vector->sharedController = 0;
	  IOUnlock(vector->interruptLock);
	  return error;
	}
      }
      
      // Fill in vector with the shared controller's info.
      vector->handler = (IOInterruptHandler)vector->sharedController->getInterruptHandlerAddress();
      vector->nub     = vector->sharedController;
      vector->source  = 0;
      vector->target  = vector->sharedController;
      vector->refCon  = 0;
      
      // If the interrupt was already registered,
      // save the driver's interrupt enablement state.
      if (wasAlreadyRegisterd) wasDisabledSoft = vector->interruptDisabledSoft;
      else wasDisabledSoft = true;
      
      // Do any specific initalization for this vector if it has not yet been used.
      if (!wasAlreadyRegisterd) initVector(vectorNumber, vector);
      
      // Make the interrupt really hard disabled.
      vector->interruptDisabledSoft = 1;
      vector->interruptDisabledHard = 1;
      vector->interruptRegistered   = 1;
      
      // Enable the original consumer's interrupt if needed.
      if (!wasDisabledSoft) originalNub->enableInterrupt(originalSource);
    }
    
    error = vector->sharedController->registerInterrupt(nub, source, target,
                                                        handler, refCon);
    IOUnlock(vector->interruptLock);
    return error;
  }
  
  // Fill in vector with the client's info.
  vector->handler = handler;
  vector->nub     = nub;
  vector->source  = source;
  vector->target  = target;
  vector->refCon  = refCon;
  
  // Do any specific initalization for this vector.
  initVector(vectorNumber, vector);
  
  // Get the vector ready.  It starts hard disabled.
  vector->interruptDisabledHard = 1;
  vector->interruptDisabledSoft = 1;
  vector->interruptRegistered   = 1;
  
  IOUnlock(vector->interruptLock);
  return kIOReturnSuccess;
}

IOReturn IOInterruptController::unregisterInterrupt(IOService *nub, int source)
{
  IOInterruptSource *interruptSources;
  long              vectorNumber;
  IOInterruptVector *vector;
  OSData            *vectorData;
  
  interruptSources = nub->_interruptSources;
  vectorData = interruptSources[source].vectorData;
  vectorNumber = *(long *)vectorData->getBytesNoCopy();
  vector = &vectors[vectorNumber];
  
  // Get the lock for this vector.
  IOTakeLock(vector->interruptLock);
  
  // Return success if it is not already registered
  if (!vector->interruptRegistered) {
    IOUnlock(vector->interruptLock);
    return kIOReturnSuccess;
  }
  
  // Soft disable the source.
  disableInterrupt(nub, source);
  
  // Turn the source off at hardware. 
  disableVectorHard(vectorNumber, vector);
  
  // Clear all the storage for the vector except for interruptLock.
  vector->interruptActive = 0;
  vector->interruptDisabledSoft = 0;
  vector->interruptDisabledHard = 0;
  vector->interruptRegistered = 0;
  vector->nub = 0;
  vector->source = 0;
  vector->handler = 0;
  vector->target = 0;
  vector->refCon = 0;
  
  IOUnlock(vector->interruptLock);
  return kIOReturnSuccess;
}

IOReturn IOInterruptController::getInterruptType(IOService *nub, int source,
						 int *interruptType)
{
  IOInterruptSource *interruptSources;
  long              vectorNumber;
  IOInterruptVector *vector;
  OSData            *vectorData;
  
  if (interruptType == 0) return kIOReturnBadArgument;
  
  interruptSources = nub->_interruptSources;
  vectorData = interruptSources[source].vectorData;
  vectorNumber = *(long *)vectorData->getBytesNoCopy();
  vector = &vectors[vectorNumber];
  
  *interruptType = getVectorType(vectorNumber, vector);
  
  return kIOReturnSuccess;
}

IOReturn IOInterruptController::enableInterrupt(IOService *nub, int source)
{
  IOInterruptSource *interruptSources;
  long              vectorNumber;
  IOInterruptVector *vector;
  OSData            *vectorData;
  
  interruptSources = nub->_interruptSources;
  vectorData = interruptSources[source].vectorData;
  vectorNumber = *(long *)vectorData->getBytesNoCopy();
  vector = &vectors[vectorNumber];
  
  if (vector->interruptDisabledSoft) {
    vector->interruptDisabledSoft = 0;
#if __ppc__
    sync();
    isync();
#endif
    
    if (!getPlatform()->atInterruptLevel()) {
      while (vector->interruptActive);
#if __ppc__
      isync();
#endif
    }
    if (vector->interruptDisabledHard) {
      vector->interruptDisabledHard = 0;
      
      enableVector(vectorNumber, vector);
    }
  }
  
  return kIOReturnSuccess;
}

IOReturn IOInterruptController::disableInterrupt(IOService *nub, int source)
{
  IOInterruptSource *interruptSources;
  long              vectorNumber;
  IOInterruptVector *vector;
  OSData            *vectorData;
  
  interruptSources = nub->_interruptSources;
  vectorData = interruptSources[source].vectorData;
  vectorNumber = *(long *)vectorData->getBytesNoCopy();
  vector = &vectors[vectorNumber];
  
  vector->interruptDisabledSoft = 1;
#if __ppc__
  sync();
  isync();
#endif
  
  if (!getPlatform()->atInterruptLevel()) {
    while (vector->interruptActive);
#if __ppc__
    isync();
#endif
  }
  
  return kIOReturnSuccess;
}

IOReturn IOInterruptController::causeInterrupt(IOService *nub, int source)
{
  IOInterruptSource *interruptSources;
  long              vectorNumber;
  IOInterruptVector *vector;
  OSData            *vectorData;

  interruptSources = nub->_interruptSources;
  vectorData = interruptSources[source].vectorData;
  vectorNumber = *(long *)vectorData->getBytesNoCopy();
  vector = &vectors[vectorNumber];
  
  causeVector(vectorNumber, vector);
  
  return kIOReturnSuccess;
}

IOInterruptAction IOInterruptController::getInterruptHandlerAddress(void)
{
  return 0;
}

IOReturn IOInterruptController::handleInterrupt(void *refCon, IOService *nub,
						int source)
{
  return kIOReturnInvalid;
}


// Methods to be overridden for simplifed interrupt controller subclasses.

bool IOInterruptController::vectorCanBeShared(long /*vectorNumber*/,
					      IOInterruptVector */*vector*/)
{
  return false;
}

void IOInterruptController::initVector(long /*vectorNumber*/,
				       IOInterruptVector */*vector*/)
{
}

int IOInterruptController::getVectorType(long /*vectorNumber*/,
					  IOInterruptVector */*vector*/)
{
  return kIOInterruptTypeEdge;
}

void IOInterruptController::disableVectorHard(long /*vectorNumber*/,
					      IOInterruptVector */*vector*/)
{
}

void IOInterruptController::enableVector(long /*vectorNumber*/,
					 IOInterruptVector */*vector*/)
{
}

void IOInterruptController::causeVector(long /*vectorNumber*/,
					IOInterruptVector */*vector*/)
{
}


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

#undef  super
#define super IOInterruptController

OSDefineMetaClassAndStructors(IOSharedInterruptController, IOInterruptController);

OSMetaClassDefineReservedUnused(IOSharedInterruptController, 0);
OSMetaClassDefineReservedUnused(IOSharedInterruptController, 1);
OSMetaClassDefineReservedUnused(IOSharedInterruptController, 2);
OSMetaClassDefineReservedUnused(IOSharedInterruptController, 3);

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

#define kIOSharedInterruptControllerDefaultVectors (128)

IOReturn IOSharedInterruptController::initInterruptController(IOInterruptController *parentController, OSData *parentSource)
{
  int      cnt, interruptType;
  IOReturn error;
  
  if (!super::init())
    return kIOReturnNoResources;
  
  // Set provider to this so enable/disable nub stuff works.
  provider = this;
  
  // Allocate the IOInterruptSource so this can act like a nub.
  _interruptSources = (IOInterruptSource *)IOMalloc(sizeof(IOInterruptSource));
  if (_interruptSources == 0) return kIOReturnNoMemory;
  _numInterruptSources = 1;
  
  // Set up the IOInterruptSource to point at this.
  parentController->retain();
  parentSource->retain();
  _interruptSources[0].interruptController = parentController;
  _interruptSources[0].vectorData = parentSource;
  
  sourceIsLevel = false;
  error = provider->getInterruptType(0, &interruptType);
  if (error == kIOReturnSuccess) {
    if (interruptType & kIOInterruptTypeLevel)
      sourceIsLevel = true;
  }
  
  // Allocate the memory for the vectors
  numVectors = kIOSharedInterruptControllerDefaultVectors; // For now a constant number.
  vectors = (IOInterruptVector *)IOMalloc(numVectors * sizeof(IOInterruptVector));
  if (vectors == NULL) {
    IOFree(_interruptSources, sizeof(IOInterruptSource));
    return kIOReturnNoMemory;
  }
  bzero(vectors, numVectors * sizeof(IOInterruptVector));
  
  // Allocate the lock for the controller.
  controllerLock = IOSimpleLockAlloc();
  if (controllerLock == 0) return kIOReturnNoResources;
  
  // Allocate locks for the vectors.
  for (cnt = 0; cnt < numVectors; cnt++) {
    vectors[cnt].interruptLock = IOLockAlloc();
    if (vectors[cnt].interruptLock == NULL) {
      for (cnt = 0; cnt < numVectors; cnt++) {
	if (vectors[cnt].interruptLock != NULL)
	  IOLockFree(vectors[cnt].interruptLock);
      }
      return kIOReturnNoResources;
    }
  }
  
  numVectors = 0; // reset the high water mark for used vectors
  vectorsRegistered = 0;
  vectorsEnabled = 0;
  controllerDisabled = 1;
  
  return kIOReturnSuccess;
}

IOReturn IOSharedInterruptController::registerInterrupt(IOService *nub,
							int source,
							void *target,
							IOInterruptHandler handler,
							void *refCon)
{
  IOInterruptSource *interruptSources;
  long              vectorNumber;
  IOInterruptVector *vector = 0;
  OSData            *vectorData;
  IOInterruptState  interruptState;
  
  interruptSources = nub->_interruptSources;
  
  // Find a free vector.
  vectorNumber = kIOSharedInterruptControllerDefaultVectors;
  while (vectorsRegistered != kIOSharedInterruptControllerDefaultVectors) {
    for (vectorNumber = 0; vectorNumber < kIOSharedInterruptControllerDefaultVectors; vectorNumber++) {
      vector = &vectors[vectorNumber];
      
      // Get the lock for this vector.
      IOTakeLock(vector->interruptLock);
      
      // Is it unregistered?
      if (!vector->interruptRegistered) break;
      
      // Move along to the next one.
      IOUnlock(vector->interruptLock);
    }
    
    if (vectorNumber != kIOSharedInterruptControllerDefaultVectors) break;
  }
  
  // Could not find a free one, so give up.
  if (vectorNumber == kIOSharedInterruptControllerDefaultVectors) {
    return kIOReturnNoResources;
  }
  
  // Create the vectorData for the IOInterruptSource.
  vectorData = OSData::withBytes(&vectorNumber, sizeof(vectorNumber));
  if (vectorData == 0) {
    return kIOReturnNoMemory;
  }
  
  // Fill in the IOInterruptSource with the controller's info.
  interruptSources[source].interruptController = this;
  interruptSources[source].vectorData = vectorData;
  
  // Fill in vector with the client's info.
  vector->handler = handler;
  vector->nub     = nub;
  vector->source  = source;
  vector->target  = target;
  vector->refCon  = refCon;
  
  // Get the vector ready.  It starts off soft disabled.
  vector->interruptDisabledSoft = 1;
  vector->interruptRegistered   = 1;
  
  interruptState = IOSimpleLockLockDisableInterrupt(controllerLock);
  // Move the high water mark if needed
  if (++vectorsRegistered > numVectors) numVectors = vectorsRegistered;
  IOSimpleLockUnlockEnableInterrupt(controllerLock, interruptState);
  
  IOUnlock(vector->interruptLock);
  return kIOReturnSuccess;
}

IOReturn IOSharedInterruptController::unregisterInterrupt(IOService *nub,
							  int source)
{
  IOInterruptSource *interruptSources;
  long              vectorNumber;
  IOInterruptVector *vector;
  OSData            *vectorData;
  IOInterruptState  interruptState;
  
  interruptSources = nub->_interruptSources;
  vectorData = interruptSources[source].vectorData;
  vectorNumber = *(long *)vectorData->getBytesNoCopy();
  vector = &vectors[vectorNumber];
  
  // Get the lock for this vector.
  IOTakeLock(vector->interruptLock);
  
  // Return success if it is not already registered
  if (!vector->interruptRegistered) {
    IOUnlock(vector->interruptLock);
    return kIOReturnSuccess;
  }
  
  // Soft disable the source and the controller too.
  disableInterrupt(nub, source);
  
  // Clear all the storage for the vector except for interruptLock.
  vector->interruptActive = 0;
  vector->interruptDisabledSoft = 0;
  vector->interruptDisabledHard = 0;
  vector->interruptRegistered = 0;
  vector->nub = 0;
  vector->source = 0;
  vector->handler = 0;
  vector->target = 0;
  vector->refCon = 0;
  
  interruptState = IOSimpleLockLockDisableInterrupt(controllerLock);
  vectorsRegistered--;
  IOSimpleLockUnlockEnableInterrupt(controllerLock, interruptState);
  
  IOUnlock(vector->interruptLock);
  
  // Re-enable the controller if all vectors are enabled.
  if (vectorsEnabled == vectorsRegistered) {
    controllerDisabled = 0;
    provider->enableInterrupt(0);
  }
  
  return kIOReturnSuccess;
}

IOReturn IOSharedInterruptController::getInterruptType(IOService */*nub*/,
						       int /*source*/,
						       int *interruptType)
{
  return provider->getInterruptType(0, interruptType);
}

IOReturn IOSharedInterruptController::enableInterrupt(IOService *nub,
						      int source)
{
  IOInterruptSource *interruptSources;
  long              vectorNumber;
  IOInterruptVector *vector;
  OSData            *vectorData;
  IOInterruptState  interruptState;
  
  interruptSources = nub->_interruptSources;
  vectorData = interruptSources[source].vectorData;
  vectorNumber = *(long *)vectorData->getBytesNoCopy();
  vector = &vectors[vectorNumber];
  
  interruptState = IOSimpleLockLockDisableInterrupt(controllerLock);
  if (!vector->interruptDisabledSoft) {
    IOSimpleLockUnlockEnableInterrupt(controllerLock, interruptState);
    return kIOReturnSuccess;
  }
  
  vector->interruptDisabledSoft = 0;
  vectorsEnabled++;
  IOSimpleLockUnlockEnableInterrupt(controllerLock, interruptState);
  
  if (controllerDisabled && (vectorsEnabled == vectorsRegistered)) {
    controllerDisabled = 0;
    provider->enableInterrupt(0);
  }
  
  return kIOReturnSuccess;
}

IOReturn IOSharedInterruptController::disableInterrupt(IOService *nub,
						       int source)
{
  IOInterruptSource *interruptSources;
  long              vectorNumber;
  IOInterruptVector *vector;
  OSData            *vectorData;
  IOInterruptState  interruptState;
  
  interruptSources = nub->_interruptSources;
  vectorData = interruptSources[source].vectorData;
  vectorNumber = *(long *)vectorData->getBytesNoCopy();
  vector = &vectors[vectorNumber];
  
  interruptState = IOSimpleLockLockDisableInterrupt(controllerLock); 
  if (!vector->interruptDisabledSoft) {
    vector->interruptDisabledSoft = 1;
#if __ppc__
    sync();
    isync();
#endif
    vectorsEnabled--;
  }
  IOSimpleLockUnlockEnableInterrupt(controllerLock, interruptState);
  
  if (!getPlatform()->atInterruptLevel()) {
    while (vector->interruptActive);
#if __ppc__
    isync();
#endif
  }
  
  return kIOReturnSuccess;
}

IOInterruptAction IOSharedInterruptController::getInterruptHandlerAddress(void)
{
    return OSMemberFunctionCast(IOInterruptAction,
			this, &IOSharedInterruptController::handleInterrupt);
}

IOReturn IOSharedInterruptController::handleInterrupt(void * /*refCon*/,
						      IOService * nub,
						      int /*source*/)
{
  long              vectorNumber;
  IOInterruptVector *vector;
  
  for (vectorNumber = 0; vectorNumber < numVectors; vectorNumber++) {
    vector = &vectors[vectorNumber];
    
    vector->interruptActive = 1;
#if __ppc__
    sync();
    isync();
#endif
    if (!vector->interruptDisabledSoft) {
#if __ppc__
      isync();
#endif
      
      // Call the handler if it exists.
      if (vector->interruptRegistered) {
	vector->handler(vector->target, vector->refCon,
			vector->nub, vector->source);
      }
    }
    
    vector->interruptActive = 0;
  }
  
  // if any of the vectors are dissabled, then dissable this controller.
  IOSimpleLockLock(controllerLock);
  if (vectorsEnabled != vectorsRegistered) {
    nub->disableInterrupt(0);
    controllerDisabled = 1;
  }
  IOSimpleLockUnlock(controllerLock);
  
  return kIOReturnSuccess;
}


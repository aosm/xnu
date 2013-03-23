/*
 * Copyright (c) 1999-2000 Apple Computer, Inc.  All rights reserved.
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
 * Copyright (c) 1999-2000 Apple Computer, Inc.  All rights reserved.
 *
 *  DRI: Josh de Cesare
 *
 */

extern "C" {
#include <machine/machine_routines.h>
#include <pexpert/pexpert.h>
}

#include <IOKit/IOLib.h>
#include <IOKit/IOPlatformExpert.h>
#include <IOKit/IOUserClient.h>
#include <IOKit/IOCPU.h>


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

kern_return_t PE_cpu_start(cpu_id_t target,
			   vm_offset_t start_paddr, vm_offset_t arg_paddr)
{
  IOCPU *targetCPU = OSDynamicCast(IOCPU, (OSObject *)target);
  
  if (targetCPU == 0) return KERN_FAILURE;
  return targetCPU->startCPU(start_paddr, arg_paddr);
}

void PE_cpu_halt(cpu_id_t target)
{
  IOCPU *targetCPU = OSDynamicCast(IOCPU, (OSObject *)target);
  
  if (targetCPU) targetCPU->haltCPU();
}

void PE_cpu_signal(cpu_id_t source, cpu_id_t target)
{
  IOCPU *sourceCPU = OSDynamicCast(IOCPU, (OSObject *)source);
  IOCPU *targetCPU = OSDynamicCast(IOCPU, (OSObject *)target);
  
  if (sourceCPU && targetCPU) sourceCPU->signalCPU(targetCPU);
}

void PE_cpu_machine_init(cpu_id_t target, boolean_t boot)
{
  IOCPU *targetCPU = OSDynamicCast(IOCPU, (OSObject *)target);
  
  if (targetCPU) targetCPU->initCPU(boot);
}

void PE_cpu_machine_quiesce(cpu_id_t target)
{
  IOCPU *targetCPU = OSDynamicCast(IOCPU, (OSObject *)target);
  
  if (targetCPU) targetCPU->quiesceCPU();
}

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

#define super IOService

OSDefineMetaClassAndAbstractStructors(IOCPU, IOService);
OSMetaClassDefineReservedUnused(IOCPU, 0);
OSMetaClassDefineReservedUnused(IOCPU, 1);
OSMetaClassDefineReservedUnused(IOCPU, 2);
OSMetaClassDefineReservedUnused(IOCPU, 3);
OSMetaClassDefineReservedUnused(IOCPU, 4);
OSMetaClassDefineReservedUnused(IOCPU, 5);
OSMetaClassDefineReservedUnused(IOCPU, 6);
OSMetaClassDefineReservedUnused(IOCPU, 7);

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

static OSArray *gIOCPUs;
static const OSSymbol *gIOCPUStateKey;
static OSString *gIOCPUStateNames[kIOCPUStateCount];

void IOCPUSleepKernel(void)
{
  long cnt, numCPUs;
  IOCPU *target;
  
  numCPUs = gIOCPUs->getCount();
  
  // Sleep the CPUs.
  cnt = numCPUs;
  while (cnt--) {
    target = OSDynamicCast(IOCPU, gIOCPUs->getObject(cnt));
    if (target->getCPUState() == kIOCPUStateRunning) {
      target->haltCPU();
    }
  }
  
  // Wake the other CPUs.
  for (cnt = 1; cnt < numCPUs; cnt++) {
    target = OSDynamicCast(IOCPU, gIOCPUs->getObject(cnt));
    if (target->getCPUState() == kIOCPUStateStopped) {
      processor_start(target->getMachProcessor());
    }
  }
}

void IOCPU::initCPUs(void)
{
  if (gIOCPUs == 0) {
    gIOCPUs = OSArray::withCapacity(1);
    
    gIOCPUStateKey = OSSymbol::withCStringNoCopy("IOCPUState");
    
    gIOCPUStateNames[kIOCPUStateUnregistered] =
      OSString::withCStringNoCopy("Unregistered");
    gIOCPUStateNames[kIOCPUStateUninitalized] =
      OSString::withCStringNoCopy("Uninitalized");
    gIOCPUStateNames[kIOCPUStateStopped] =
      OSString::withCStringNoCopy("Stopped");
    gIOCPUStateNames[kIOCPUStateRunning] =
      OSString::withCStringNoCopy("Running");
  }
}

bool IOCPU::start(IOService *provider)
{
  OSData *busFrequency, *cpuFrequency, *decFrequency;
  
  if (!super::start(provider)) return false;
  
  initCPUs();
  
  _cpuGroup = gIOCPUs;
  cpuNub = provider;
  
  gIOCPUs->setObject(this);
  
  // Correct the bus, cpu and dec frequencies in the device tree.
  busFrequency = OSData::withBytesNoCopy((void *)&gPEClockFrequencyInfo.bus_clock_rate_hz, 4);
  cpuFrequency = OSData::withBytesNoCopy((void *)&gPEClockFrequencyInfo.cpu_clock_rate_hz, 4);
  decFrequency = OSData::withBytesNoCopy((void *)&gPEClockFrequencyInfo.dec_clock_rate_hz, 4);
  provider->setProperty("bus-frequency", busFrequency);
  provider->setProperty("clock-frequency", cpuFrequency);
  provider->setProperty("timebase-frequency", decFrequency);
  busFrequency->release();
  cpuFrequency->release();
  decFrequency->release();
  
  setProperty("IOCPUID", (UInt32)this, 32);
  
  setCPUNumber(0);
  setCPUState(kIOCPUStateUnregistered);
  
  return true;
}

IOReturn IOCPU::setProperties(OSObject *properties)
{
  OSDictionary *dict = OSDynamicCast(OSDictionary, properties);
  OSString     *stateStr;
  
  if (dict == 0) return kIOReturnUnsupported;
  
  stateStr = OSDynamicCast(OSString, dict->getObject(gIOCPUStateKey));
  if (stateStr != 0) {
    if (!IOUserClient::clientHasPrivilege(current_task(), "root"))
      return kIOReturnNotPrivileged;
    
    if (_cpuNumber == 0) return kIOReturnUnsupported;
    
    if (stateStr->isEqualTo("running")) {
      if (_cpuState == kIOCPUStateStopped) {
	processor_start(machProcessor);
      } else if (_cpuState != kIOCPUStateRunning) {
	return kIOReturnUnsupported;
      }
    } else if (stateStr->isEqualTo("stopped")) {
      if (_cpuState == kIOCPUStateRunning) {
        haltCPU();
      } else if (_cpuState != kIOCPUStateStopped) {
        return kIOReturnUnsupported;
      }
    } else return kIOReturnUnsupported;
    
    return kIOReturnSuccess;
  }
  
  return kIOReturnUnsupported;
}

void IOCPU::signalCPU(IOCPU */*target*/)
{
}

void IOCPU::enableCPUTimeBase(bool /*enable*/)
{
}

UInt32 IOCPU::getCPUNumber(void)
{
  return _cpuNumber;
}

void IOCPU::setCPUNumber(UInt32 cpuNumber)
{
  _cpuNumber = cpuNumber;
  setProperty("IOCPUNumber", _cpuNumber, 32);
}

UInt32 IOCPU::getCPUState(void)
{
  return _cpuState;
}

void IOCPU::setCPUState(UInt32 cpuState)
{
  if ((cpuState >= 0) && (cpuState < kIOCPUStateCount)) {
    _cpuState = cpuState;
    setProperty(gIOCPUStateKey, gIOCPUStateNames[cpuState]);
  }
}

OSArray *IOCPU::getCPUGroup(void)
{
  return _cpuGroup;
}

UInt32 IOCPU::getCPUGroupSize(void)
{
  return _cpuGroup->getCount();
}

processor_t IOCPU::getMachProcessor(void)
{
  return machProcessor;
}


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

#undef super
#define super IOInterruptController

OSDefineMetaClassAndStructors(IOCPUInterruptController, IOInterruptController);

OSMetaClassDefineReservedUnused(IOCPUInterruptController, 0);
OSMetaClassDefineReservedUnused(IOCPUInterruptController, 1);
OSMetaClassDefineReservedUnused(IOCPUInterruptController, 2);
OSMetaClassDefineReservedUnused(IOCPUInterruptController, 3);
OSMetaClassDefineReservedUnused(IOCPUInterruptController, 4);
OSMetaClassDefineReservedUnused(IOCPUInterruptController, 5);



/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */


IOReturn IOCPUInterruptController::initCPUInterruptController(int sources)
{
  int cnt;
  
  if (!super::init()) return kIOReturnInvalid;
  
  numCPUs = sources;
  
  cpus = (IOCPU **)IOMalloc(numCPUs * sizeof(IOCPU *));
  if (cpus == 0) return kIOReturnNoMemory;
  bzero(cpus, numCPUs * sizeof(IOCPU *));
  
  vectors = (IOInterruptVector *)IOMalloc(numCPUs * sizeof(IOInterruptVector));
  if (vectors == 0) return kIOReturnNoMemory;
  bzero(vectors, numCPUs * sizeof(IOInterruptVector));
  
  // Allocate locks for the
  for (cnt = 0; cnt < numCPUs; cnt++) {
    vectors[cnt].interruptLock = IOLockAlloc();
    if (vectors[cnt].interruptLock == NULL) {
      for (cnt = 0; cnt < numCPUs; cnt++) {
	if (vectors[cnt].interruptLock != NULL)
	  IOLockFree(vectors[cnt].interruptLock);
      }
      return kIOReturnNoResources;
    }
  }
  
  return kIOReturnSuccess;
}

void IOCPUInterruptController::registerCPUInterruptController(void)
{
  registerService();
  
  getPlatform()->registerInterruptController(gPlatformInterruptControllerName,
					     this);
}

void IOCPUInterruptController::setCPUInterruptProperties(IOService *service)
{
  int          cnt;
  OSArray      *controller;
  OSArray      *specifier;
  OSData       *tmpData;
  long         tmpLong;
  
  // Create the interrupt specifer array.
  specifier = OSArray::withCapacity(numCPUs);
  for (cnt = 0; cnt < numCPUs; cnt++) {
    tmpLong = cnt;
    tmpData = OSData::withBytes(&tmpLong, sizeof(tmpLong));
    specifier->setObject(tmpData);
    tmpData->release();
  };
  
  // Create the interrupt controller array.
  controller = OSArray::withCapacity(numCPUs);
  for (cnt = 0; cnt < numCPUs; cnt++) {
    controller->setObject(gPlatformInterruptControllerName);
  }
  
  // Put the two arrays into the property table.
  service->setProperty(gIOInterruptControllersKey, controller);
  service->setProperty(gIOInterruptSpecifiersKey, specifier);
  controller->release();
  specifier->release();
}

void IOCPUInterruptController::enableCPUInterrupt(IOCPU *cpu)
{
  ml_install_interrupt_handler(cpu, cpu->getCPUNumber(), this,
                               (IOInterruptHandler)&IOCPUInterruptController::handleInterrupt, 0);
  
  enabledCPUs++;
  
  if (enabledCPUs == numCPUs) thread_wakeup(this);
}

IOReturn IOCPUInterruptController::registerInterrupt(IOService *nub,
						     int source,
						     void *target,
						     IOInterruptHandler handler,
						     void *refCon)
{
  IOInterruptVector *vector;
  
  if (source >= numCPUs) return kIOReturnNoResources;
  
  vector = &vectors[source];
  
  // Get the lock for this vector.
  IOTakeLock(vector->interruptLock);
  
  // Make sure the vector is not in use.
  if (vector->interruptRegistered) {
    IOUnlock(vector->interruptLock);
    return kIOReturnNoResources;
  }
  
  // Fill in vector with the client's info.
  vector->handler = handler;
  vector->nub     = nub;
  vector->source  = source;
  vector->target  = target;
  vector->refCon  = refCon;
  
  // Get the vector ready.  It starts hard disabled.
  vector->interruptDisabledHard = 1;
  vector->interruptDisabledSoft = 1;
  vector->interruptRegistered   = 1;
  
  IOUnlock(vector->interruptLock);
  
  if (enabledCPUs != numCPUs) {
    assert_wait(this, THREAD_UNINT);
    thread_block(0);
  }
  
  return kIOReturnSuccess;
}

IOReturn IOCPUInterruptController::getInterruptType(IOService */*nub*/,
						    int /*source*/,
						    int *interruptType)
{
  if (interruptType == 0) return kIOReturnBadArgument;
  
  *interruptType = kIOInterruptTypeLevel;
  
  return kIOReturnSuccess;
}

IOReturn IOCPUInterruptController::enableInterrupt(IOService */*nub*/,
						   int /*source*/)
{
//  ml_set_interrupts_enabled(true);
  return kIOReturnSuccess;
}

IOReturn IOCPUInterruptController::disableInterrupt(IOService */*nub*/,
						    int /*source*/)
{
//  ml_set_interrupts_enabled(false);
  return kIOReturnSuccess;
}

IOReturn IOCPUInterruptController::causeInterrupt(IOService */*nub*/,
						  int /*source*/)
{
  ml_cause_interrupt();
  return kIOReturnSuccess;
}

IOReturn IOCPUInterruptController::handleInterrupt(void */*refCon*/,
						   IOService */*nub*/,
						   int source)
{
  IOInterruptVector *vector;
  
  vector = &vectors[source];
  
  if (!vector->interruptRegistered) return kIOReturnInvalid;
  
  vector->handler(vector->target, vector->refCon,
		  vector->nub, vector->source);
  
  return kIOReturnSuccess;
}

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

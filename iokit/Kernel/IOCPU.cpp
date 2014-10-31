/*
 * Copyright (c) 1999-2000 Apple Computer, Inc.  All rights reserved.
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
 * Copyright (c) 1999-2000 Apple Computer, Inc.  All rights reserved.
 *
 *  DRI: Josh de Cesare
 *
 */

extern "C" {
#include <machine/machine_routines.h>
#include <pexpert/pexpert.h>
}

#include <machine/machine_routines.h>

#include <IOKit/IOLib.h>
#include <IOKit/IOPlatformExpert.h>
#include <IOKit/pwr_mgt/RootDomain.h>
#include <IOKit/pwr_mgt/IOPMPrivate.h>
#include <IOKit/IOUserClient.h>
#include <IOKit/IOKitKeysPrivate.h>
#include <IOKit/IOCPU.h>

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
#include <kern/queue.h>

typedef kern_return_t (*iocpu_platform_action_t)(void * refcon0, void * refcon1, uint32_t priority,
						 void * param1, void * param2, void * param3,
						 const char * name);

struct iocpu_platform_action_entry
{
    queue_chain_t                     link;
    iocpu_platform_action_t           action;
    int32_t	                      priority;
    const char *		      name;
    void *	                      refcon0;
    void *			      refcon1;
    struct iocpu_platform_action_entry * alloc_list;
};
typedef struct iocpu_platform_action_entry iocpu_platform_action_entry_t;

queue_head_t * 
iocpu_get_platform_quiesce_queue(void);

queue_head_t * 
iocpu_get_platform_active_queue(void);

void
iocpu_platform_cpu_action_init(queue_head_t * quiesce_queue, queue_head_t * init_queue);

void
iocpu_add_platform_action(queue_head_t * queue, iocpu_platform_action_entry_t * entry);

void
iocpu_remove_platform_action(iocpu_platform_action_entry_t * entry);

kern_return_t
iocpu_run_platform_actions(queue_head_t * queue, uint32_t first_priority, uint32_t last_priority,
					void * param1, void * param2, void * param3);

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

#define kBootCPUNumber  0

static iocpu_platform_action_entry_t * gIOAllActionsQueue;
static queue_head_t gIOSleepActionQueue;
static queue_head_t gIOWakeActionQueue;

static queue_head_t iocpu_quiesce_queue;
static queue_head_t iocpu_active_queue;

static queue_head_t gIOHaltRestartActionQueue;

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

void 
iocpu_platform_cpu_action_init(queue_head_t * quiesce_queue, __unused queue_head_t * init_queue)
{
#if 0
    enum { kNumQuiesceActions = 2 };
    static iocpu_platform_action_entry_t quiesce_actions[kNumQuiesceActions] = 
    {
	{ { NULL, NULL }, (iocpu_platform_action_t) &clean_mmu_dcache, 97000, 0, 0, NULL },    
	{ { NULL, NULL }, (iocpu_platform_action_t) &arm_sleep, 99000, 0, 0, NULL },    
    };
    unsigned int idx;

    for (idx = 0; idx < kNumQuiesceActions; idx++)
	iocpu_add_platform_action(quiesce_queue, &quiesce_actions[idx]);
#endif
}

queue_head_t * iocpu_get_platform_quiesce_queue(void)
{
    if (!iocpu_quiesce_queue.next)
    {
	queue_init(&iocpu_quiesce_queue);
	queue_init(&iocpu_active_queue);
	iocpu_platform_cpu_action_init(&iocpu_quiesce_queue, &iocpu_active_queue);
    }
    return (&iocpu_quiesce_queue);
}

queue_head_t * iocpu_get_platform_active_queue(void)
{
    if (!iocpu_active_queue.next)
    {
	queue_init(&iocpu_quiesce_queue);
	queue_init(&iocpu_active_queue);
	iocpu_platform_cpu_action_init(&iocpu_quiesce_queue, &iocpu_active_queue);
    }
    return (&iocpu_active_queue);
}

void iocpu_add_platform_action(queue_head_t * queue, iocpu_platform_action_entry_t * entry)
{
    iocpu_platform_action_entry_t * next;

    queue_iterate(queue, next, iocpu_platform_action_entry_t *, link)
    {
	if (next->priority > entry->priority)
	{
	    queue_insert_before(queue, entry, next, iocpu_platform_action_entry_t *, link);
	    return;
	}
    }
    queue_enter(queue, entry, iocpu_platform_action_entry_t *, link);	// at tail
}

void iocpu_remove_platform_action(iocpu_platform_action_entry_t * entry)
{
    remque(&entry->link);
}

kern_return_t
iocpu_run_platform_actions(queue_head_t * queue, uint32_t first_priority, uint32_t last_priority,
					void * param1, void * param2, void * param3)
{
    kern_return_t                ret = KERN_SUCCESS;
    kern_return_t                result = KERN_SUCCESS;
    iocpu_platform_action_entry_t * next;

    queue_iterate(queue, next, iocpu_platform_action_entry_t *, link)
    {
	uint32_t pri = (next->priority < 0) ? -next->priority : next->priority;
	if ((pri >= first_priority) && (pri <= last_priority))
	{
	    //kprintf("[%p]", next->action);
	    ret = (*next->action)(next->refcon0, next->refcon1, pri, param1, param2, param3, next->name);
	}
	if (KERN_SUCCESS == result)
	    result = ret;
    }
    return (result);
}

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

extern "C" kern_return_t 
IOCPURunPlatformQuiesceActions(void)
{
    return (iocpu_run_platform_actions(iocpu_get_platform_quiesce_queue(), 0, 0U-1,
				    NULL, NULL, NULL));
}

extern "C" kern_return_t 
IOCPURunPlatformActiveActions(void)
{
    return (iocpu_run_platform_actions(iocpu_get_platform_active_queue(), 0, 0U-1,
				    NULL, NULL, NULL));
}

static kern_return_t 
IOServicePlatformAction(void * refcon0, void * refcon1, uint32_t priority,
			  void * param1, void * param2, void * param3,
			  const char * service_name)
{
    IOReturn	     ret;
    IOService *      service  = (IOService *)      refcon0;
    const OSSymbol * function = (const OSSymbol *) refcon1;

    kprintf("%s -> %s\n", function->getCStringNoCopy(), service_name);

    ret = service->callPlatformFunction(function, false, 
					 (void *)(uintptr_t) priority, param1, param2, param3);

    return (ret);
}

static void
IOInstallServicePlatformAction(IOService * service, 
				const OSSymbol * key, queue_head_t * queue,
				bool reverse)
{
    OSNumber * num;
    iocpu_platform_action_entry_t * entry;
    uint32_t priority;

    num = OSDynamicCast(OSNumber, service->getProperty(key));
    if (!num)
	return;

    entry = IONew(iocpu_platform_action_entry_t, 1);
    entry->action = &IOServicePlatformAction;
    entry->name = service->getName();
    priority = num->unsigned32BitValue();
    if (reverse)
	entry->priority = -priority;
    else
	entry->priority = priority;
    entry->refcon0 = service;
    entry->refcon1 = (void *) key;

    iocpu_add_platform_action(queue, entry);
    entry->alloc_list = gIOAllActionsQueue;
    gIOAllActionsQueue = entry;
}

extern "C" kern_return_t 
IOCPURunPlatformHaltRestartActions(uint32_t message)
{
    kern_return_t	 ret;
    IORegistryIterator * iter;
    OSOrderedSet *       all;
    IOService *          service;

    if (!gIOHaltRestartActionQueue.next)
    {
	queue_init(&gIOHaltRestartActionQueue);
	iter = IORegistryIterator::iterateOver(gIOServicePlane,
						kIORegistryIterateRecursively);
	if (iter)
	{
	    all = 0;
	    do 
	    {
		if (all) all->release();
		all = iter->iterateAll();
	    }
	    while (!iter->isValid());
	    iter->release();
	    if (all)
	    {
		while((service = (IOService *) all->getFirstObject()))
		{
		    IOInstallServicePlatformAction(service, gIOPlatformHaltRestartActionKey, &gIOHaltRestartActionQueue, false);
		    all->removeObject(service);
		}
		all->release();
	    }	
	}
    }
    ret = iocpu_run_platform_actions(&gIOHaltRestartActionQueue, 0, 0U-1,
				     (void *)(uintptr_t) message, NULL, NULL);
    return (ret);
}

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

void PE_cpu_machine_init(cpu_id_t target, boolean_t bootb)
{
  IOCPU *targetCPU = OSDynamicCast(IOCPU, (OSObject *)target);
  
  if (targetCPU) targetCPU->initCPU(bootb);
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
    IOCPU *bootCPU = NULL;
    IOPMrootDomain  *rootDomain = IOService::getPMRootDomain();

    kprintf("IOCPUSleepKernel\n");

    IORegistryIterator * iter;
    OSOrderedSet *       all;
    IOService *          service;

    rootDomain->tracePoint( kIOPMTracePointSleepPlatformActions );

    queue_init(&gIOSleepActionQueue);
    queue_init(&gIOWakeActionQueue);
    queue_init(&gIOHaltRestartActionQueue);

    iter = IORegistryIterator::iterateOver( gIOServicePlane,
					    kIORegistryIterateRecursively );
    if( iter)
    {
	all = 0;
	do 
	{
	    if (all)
		all->release();
	    all = iter->iterateAll();
	}
	while (!iter->isValid());
	iter->release();

	if (all)
	{
	    while((service = (IOService *) all->getFirstObject()))
	    {
		IOInstallServicePlatformAction(service, gIOPlatformSleepActionKey,   &gIOSleepActionQueue,		 false);
		IOInstallServicePlatformAction(service, gIOPlatformWakeActionKey,    &gIOWakeActionQueue,		 true);
		IOInstallServicePlatformAction(service, gIOPlatformQuiesceActionKey, iocpu_get_platform_quiesce_queue(), false);
		IOInstallServicePlatformAction(service, gIOPlatformActiveActionKey,  iocpu_get_platform_active_queue(),  true);
		IOInstallServicePlatformAction(service, gIOPlatformHaltRestartActionKey, &gIOHaltRestartActionQueue,     false);
		all->removeObject(service);
	    }
	    all->release();
	}	
    }

    iocpu_run_platform_actions(&gIOSleepActionQueue, 0, 0U-1,
				NULL, NULL, NULL);

    rootDomain->tracePoint( kIOPMTracePointSleepCPUs );

    numCPUs = gIOCPUs->getCount();
    // Sleep the CPUs.
    cnt = numCPUs;
    while (cnt--) 
    {
        target = OSDynamicCast(IOCPU, gIOCPUs->getObject(cnt));
        
        // We make certain that the bootCPU is the last to sleep
        // We'll skip it for now, and halt it after finishing the
        // non-boot CPU's.
        if (target->getCPUNumber() == kBootCPUNumber) 
        {
            bootCPU = target;
        } else if (target->getCPUState() == kIOCPUStateRunning) 
        {
            target->haltCPU();
        }
    }

    rootDomain->tracePoint( kIOPMTracePointSleepPlatformDriver );

    // Now sleep the boot CPU.
    if (bootCPU)
        bootCPU->haltCPU();

    rootDomain->tracePoint( kIOPMTracePointWakePlatformActions );

    iocpu_run_platform_actions(&gIOWakeActionQueue, 0, 0U-1,
				    NULL, NULL, NULL);

    iocpu_platform_action_entry_t * entry;
    while ((entry = gIOAllActionsQueue))
    {
	gIOAllActionsQueue = entry->alloc_list;
	iocpu_remove_platform_action(entry);
	IODelete(entry, iocpu_platform_action_entry_t, 1);
    }

    if (!queue_empty(&gIOSleepActionQueue))       panic("gIOSleepActionQueue");
    if (!queue_empty(&gIOWakeActionQueue))  	  panic("gIOWakeActionQueue");
    if (!queue_empty(&gIOHaltRestartActionQueue)) panic("gIOHaltRestartActionQueue");
    gIOHaltRestartActionQueue.next = 0;
  
    rootDomain->tracePoint( kIOPMTracePointWakeCPUs );

    // Wake the other CPUs.
    for (cnt = 0; cnt < numCPUs; cnt++) 
    {
        target = OSDynamicCast(IOCPU, gIOCPUs->getObject(cnt));
        
        // Skip the already-woken boot CPU.
        if ((target->getCPUNumber() != kBootCPUNumber)
            && (target->getCPUState() == kIOCPUStateStopped))
        {
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
  OSData *busFrequency, *cpuFrequency, *timebaseFrequency;
  
  if (!super::start(provider)) return false;
  
  initCPUs();
  
  _cpuGroup = gIOCPUs;
  cpuNub = provider;
  
  gIOCPUs->setObject(this);
  
  // Correct the bus, cpu and timebase frequencies in the device tree.
  if (gPEClockFrequencyInfo.bus_frequency_hz < 0x100000000ULL) {
    busFrequency = OSData::withBytesNoCopy((void *)&gPEClockFrequencyInfo.bus_clock_rate_hz, 4);
  } else {
    busFrequency = OSData::withBytesNoCopy((void *)&gPEClockFrequencyInfo.bus_frequency_hz, 8);
  }
  provider->setProperty("bus-frequency", busFrequency);
  busFrequency->release();
    
  if (gPEClockFrequencyInfo.cpu_frequency_hz < 0x100000000ULL) {
    cpuFrequency = OSData::withBytesNoCopy((void *)&gPEClockFrequencyInfo.cpu_clock_rate_hz, 4);
  } else {
    cpuFrequency = OSData::withBytesNoCopy((void *)&gPEClockFrequencyInfo.cpu_frequency_hz, 8);
  }
  provider->setProperty("clock-frequency", cpuFrequency);
  cpuFrequency->release();
  
  timebaseFrequency = OSData::withBytesNoCopy((void *)&gPEClockFrequencyInfo.timebase_frequency_hz, 4);
  provider->setProperty("timebase-frequency", timebaseFrequency);
  timebaseFrequency->release();
  
  super::setProperty("IOCPUID", getRegistryEntryID(), sizeof(uint64_t)*8);
  
  setCPUNumber(0);
  setCPUState(kIOCPUStateUnregistered);
  
  return true;
}

OSObject *IOCPU::getProperty(const OSSymbol *aKey) const
{
  if (aKey == gIOCPUStateKey) return gIOCPUStateNames[_cpuState];
  
  return super::getProperty(aKey);
}

bool IOCPU::setProperty(const OSSymbol *aKey, OSObject *anObject)
{
  OSString *stateStr;
  
  if (aKey == gIOCPUStateKey) {
    stateStr = OSDynamicCast(OSString, anObject);
    if (stateStr == 0) return false;
    
    if (_cpuNumber == 0) return false;
    
    if (stateStr->isEqualTo("running")) {
      if (_cpuState == kIOCPUStateStopped) {
	processor_start(machProcessor);
      } else if (_cpuState != kIOCPUStateRunning) {
	return false;
      }
    } else if (stateStr->isEqualTo("stopped")) {
      if (_cpuState == kIOCPUStateRunning) {
        haltCPU();
      } else if (_cpuState != kIOCPUStateStopped) {
        return false;
      }
    } else return false;
    
    return true;
  }
  
  return super::setProperty(aKey, anObject);
}

bool IOCPU::serializeProperties(OSSerialize *serialize) const
{
	bool result;
	OSDictionary *dict = dictionaryWithProperties();
	dict->setObject(gIOCPUStateKey, gIOCPUStateNames[_cpuState]);
	result = dict->serialize(serialize);
	dict->release();  
	return result;
}

IOReturn IOCPU::setProperties(OSObject *properties)
{
  OSDictionary *dict = OSDynamicCast(OSDictionary, properties);
  OSString     *stateStr;
  IOReturn     result;
  
  if (dict == 0) return kIOReturnUnsupported;
  
  stateStr = OSDynamicCast(OSString, dict->getObject(gIOCPUStateKey));
  if (stateStr != 0) {
    result = IOUserClient::clientHasPrivilege(current_task(), kIOClientPrivilegeAdministrator);
    if (result != kIOReturnSuccess) return result;
    
    if (setProperty(gIOCPUStateKey, stateStr)) return kIOReturnSuccess;
    
    return kIOReturnUnsupported;
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
  super::setProperty("IOCPUNumber", _cpuNumber, 32);
}

UInt32 IOCPU::getCPUState(void)
{
  return _cpuState;
}

void IOCPU::setCPUState(UInt32 cpuState)
{
  if (cpuState < kIOCPUStateCount) {
    _cpuState = cpuState;
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
  
  ml_init_max_cpus(numCPUs);
  
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
  
  if ((service->getProperty(gIOInterruptControllersKey) != 0) &&
      (service->getProperty(gIOInterruptSpecifiersKey) != 0))
    return;
  
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
	IOInterruptHandler handler = OSMemberFunctionCast(
		IOInterruptHandler, this, &IOCPUInterruptController::handleInterrupt);

	ml_install_interrupt_handler(cpu, cpu->getCPUNumber(), this, handler, 0);

	// Ensure that the increment is seen by all processors
	OSIncrementAtomic(&enabledCPUs);

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
    thread_block(THREAD_CONTINUE_NULL);
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

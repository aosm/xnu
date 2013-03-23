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
 * Copyright (c) 1998,1999 Apple Computer, Inc.  All rights reserved. 
 *
 * HISTORY
 *
 */

#include <libkern/c++/OSUnserialize.h>
#include <IOKit/IORegistryEntry.h>
#include <IOKit/IODeviceTreeSupport.h>
#include <IOKit/IOCatalogue.h>
#include <IOKit/IOUserClient.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IOPlatformExpert.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOKitKeys.h>
#include <IOKit/IOKitDebug.h>

#include <IOKit/assert.h>

extern "C" {

extern void OSlibkernInit (void);
extern void IOLibInit(void);

#include <kern/clock.h>

/*XXX power management hacks XXX*/
#include <IOKit/IOReturn.h>
#include <IOKit/IOMessage.h>

extern void *registerSleepWakeInterest(
						void		*callback,
						void		*target,
						void		*refCon);
/*XXX power management hacks XXX*/

static void
calend_wakeup_resynch(
	thread_call_param_t		p0,
	thread_call_param_t		p1)
{
	void		IOKitResetTime(void);

	IOKitResetTime();
}

static thread_call_t	calend_sleep_wake_call;

static IOReturn
calend_sleep_wake_notif(
	void			*target,
	void			*refCon,
	unsigned int	messageType,
	void			*provider,
	void			*messageArg,
	vm_size_t		argSize)
{
	if (messageType != kIOMessageSystemHasPoweredOn)
		return (kIOReturnUnsupported);

	if (calend_sleep_wake_call != NULL)
		thread_call_enter(calend_sleep_wake_call);

	return (kIOReturnSuccess);
}

void IOKitResetTime( void )
{
    mach_timespec_t	t;

    t.tv_sec = 30;
    t.tv_nsec = 0;
    IOService::waitForService(
        IOService::resourceMatching("IORTC"), &t );
#ifndef i386
    IOService::waitForService(
        IOService::resourceMatching("IONVRAM"), &t );
#endif

	if (calend_sleep_wake_call == NULL) {
		calend_sleep_wake_call = thread_call_allocate(
											calend_wakeup_resynch, NULL);

		registerSleepWakeInterest((void *)calend_sleep_wake_notif, NULL, NULL);
	}

    clock_initialize_calendar();
}


void StartIOKit( void * p1, void * p2, void * p3, void * p4 )
{
    IOPlatformExpertDevice *	rootNub;
    int				debugFlags;
    IORegistryEntry *		root;
    OSObject *			obj;
    extern const char *         gIOKernelKmods;
    OSString *                  errorString = NULL; // must release
    OSDictionary *              fakeKmods;  // must release
    OSCollectionIterator *      kmodIter;   // must release
    OSString *                  kmodName;   // don't release

    IOLog( iokit_version );

    if( PE_parse_boot_arg( "io", &debugFlags ))
	gIOKitDebug = debugFlags;
    //
    // Have to start IOKit environment before we attempt to start
    // the C++ runtime environment.  At some stage we have to clean up
    // the initialisation path so that OS C++ can initialise independantly
    // of iokit basic service initialisation, or better we have IOLib stuff
    // initialise as basic OS services.
    //
    IOLibInit(); 
    OSlibkernInit();

    IOLog("_cppInit done\n");


   /*****
    * Declare the fake kmod_info structs for built-in components
    * that must be tracked as independent units for dependencies.
    */
    fakeKmods = OSDynamicCast(OSDictionary,
        OSUnserialize(gIOKernelKmods, &errorString));

    if (!fakeKmods) {
        if (errorString) {
            panic("Kernel kmod list syntax error: %s\n",
                    errorString->getCStringNoCopy());
            errorString->release();
        } else {
            panic("Error loading kernel kmod list.\n");
        }
    }

    kmodIter = OSCollectionIterator::withCollection(fakeKmods);
    if (!kmodIter) {
        panic("Can't declare in-kernel kmods.\n");
    }
    while ((kmodName = OSDynamicCast(OSString, kmodIter->getNextObject()))) {

        OSString * kmodVersion = OSDynamicCast(OSString,
            fakeKmods->getObject(kmodName));
        if (!kmodVersion) {
            panic("Can't declare in-kernel kmod; \"%s\" has "
                "an invalid version.\n",
                kmodName->getCStringNoCopy());
        }
        if (KERN_SUCCESS != kmod_create_fake(kmodName->getCStringNoCopy(),
                kmodVersion->getCStringNoCopy())) {
            panic("Failure declaring in-kernel kmod \"%s\".\n",
                kmodName->getCStringNoCopy());
        }
    }

    kmodIter->release();
    fakeKmods->release();



    root = IORegistryEntry::initialize();
    assert( root );
    IOService::initialize();
    IOCatalogue::initialize();
    IOUserClient::initialize();
    IOMemoryDescriptor::initialize();

    obj = OSString::withCString( iokit_version );
    assert( obj );
    if( obj ) {
        root->setProperty( kIOKitBuildVersionKey, obj );
	obj->release();
    }
    obj = IOKitDiagnostics::diagnostics();
    if( obj ) {
        root->setProperty( kIOKitDiagnosticsKey, obj );
	obj->release();
    }

    rootNub = new IOPlatformExpertDevice;

    if( rootNub && rootNub->initWithArgs( p1, p2, p3, p4)) {
        rootNub->attach( 0 );

       /* Enter into the catalogue the drivers
        * provided by BootX.
        */
        gIOCatalogue->recordStartupExtensions();

        rootNub->registerService();
    }
}

}; /* extern "C" */

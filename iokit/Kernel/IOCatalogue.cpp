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
 * Copyright (c) 1998 Apple Computer, Inc.  All rights reserved. 
 *
 * HISTORY
 *
 */

#include <IOKit/IODeviceTreeSupport.h>
#include <IOKit/IOService.h>
#include <libkern/c++/OSContainers.h>
#include <IOKit/IOCatalogue.h>
#include <libkern/c++/OSUnserialize.h>
extern "C" {
#include <machine/machine_routines.h>
#include <mach/kmod.h>
#include <mach-o/mach_header.h>
#include <kern/host.h>
};

#include <IOKit/IOLib.h>

#include <IOKit/assert.h>


extern "C" {
int IODTGetLoaderInfo( char *key, void **infoAddr, int *infoSize );
extern void IODTFreeLoaderInfo( char *key, void *infoAddr, int infoSize );
extern void OSRuntimeUnloadCPPForSegment(
    struct segment_command * segment);
};


/*****
 * At startup these function pointers are set to use the libsa in-kernel
 * linker for recording and loading kmods. Once the root filesystem
 * is available, the kmod_load_function pointer gets switched to point
 * at the kmod_load_extension() function built into the kernel, and the
 * others are set to zero. Those two functions must *always* be checked
 * before being invoked.
 */
extern "C" {
kern_return_t (*kmod_load_function)(char *extension_name) =
    &kmod_load_extension;
bool (*record_startup_extensions_function)(void) = 0;
bool (*add_from_mkext_function)(OSData * mkext) = 0;
void (*remove_startup_extension_function)(const char * name) = 0;
};


/*****
 * A few parts of IOCatalogue require knowledge of
 * whether the in-kernel linker is present. This
 * variable is set by libsa's bootstrap code.
 */
int kernelLinkerPresent = 0;

#define kModuleKey "CFBundleIdentifier"

#define super OSObject
OSDefineMetaClassAndStructors(IOCatalogue, OSObject)

#define CATALOGTEST 0

IOCatalogue    * gIOCatalogue;
const OSSymbol * gIOClassKey;
const OSSymbol * gIOProbeScoreKey;
const OSSymbol * gIOModuleIdentifierKey;
OSSet *          gIOCatalogModuleRequests;
OSSet *          gIOCatalogCacheMisses;
OSSet *		 gIOCatalogROMMkexts;
IOLock *	 gIOCatalogLock;
IOLock *	 gIOKLDLock;

/*********************************************************************
*********************************************************************/

OSArray * gIOPrelinkedModules = 0;

extern "C" kern_return_t
kmod_create_internal(
            kmod_info_t *info,
            kmod_t *id);

extern "C" kern_return_t
kmod_destroy_internal(kmod_t id);

extern "C" kern_return_t
kmod_start_or_stop(
    kmod_t id,
    int start,
    kmod_args_t *data,
    mach_msg_type_number_t *dataCount);

extern "C" kern_return_t kmod_retain(kmod_t id);
extern "C" kern_return_t kmod_release(kmod_t id);

static 
kern_return_t start_prelink_module(UInt32 moduleIndex)
{
    kern_return_t  kr = KERN_SUCCESS;
    UInt32 *       togo;
    SInt32	   count, where, end;
    UInt32 *       prelink;
    SInt32	   next, lastDep;
    OSData *       data;
    OSString *     str;
    OSDictionary * dict;

    OSArray *
    prelinkedModules = gIOPrelinkedModules;

    togo    = IONew(UInt32, prelinkedModules->getCount());
    togo[0] = moduleIndex;
    count   = 1;

    for (next = 0; next < count; next++)
    {
	dict = (OSDictionary *) prelinkedModules->getObject(togo[next]);

	data = OSDynamicCast(OSData, dict->getObject("OSBundlePrelink"));
	if (!data)
	{
	    // already started or no code
	    if (togo[next] == moduleIndex)
	    {
		kr = KERN_FAILURE;
		break;
	    }
	    continue;
	}
	prelink = (UInt32 *) data->getBytesNoCopy();
	lastDep = OSReadBigInt32(prelink, 12);
	for (SInt32 idx = OSReadBigInt32(prelink, 8); idx < lastDep; idx += sizeof(UInt32))
	{
	    UInt32 depIdx = OSReadBigInt32(prelink, idx) - 1;

	    for (where = next + 1;
		 (where < count) && (togo[where] > depIdx);
		 where++)	{}

	    if (where != count)
	    {
		if (togo[where] == depIdx)
		    continue;
		for (end = count; end != where; end--)
		    togo[end] = togo[end - 1];
	    }
	    count++;
	    togo[where] = depIdx;
	}
    }

    if (KERN_SUCCESS != kr)
	return kr;

    for (next = (count - 1); next >= 0; next--)
    {
	dict = (OSDictionary *) prelinkedModules->getObject(togo[next]);

	data = OSDynamicCast(OSData, dict->getObject("OSBundlePrelink"));
	if (!data)
	    continue;
	prelink = (UInt32 *) data->getBytesNoCopy();
    
	kmod_t id;
	kmod_info_t * kmod_info = (kmod_info_t *) OSReadBigInt32(prelink, 0);

	kr = kmod_create_internal(kmod_info, &id);
	if (KERN_SUCCESS != kr)
	    break;

	lastDep = OSReadBigInt32(prelink, 12);
	for (SInt32 idx = OSReadBigInt32(prelink, 8); idx < lastDep; idx += sizeof(UInt32))
	{
	    OSDictionary * depDict;
	    kmod_info_t *  depInfo;

	    depDict = (OSDictionary *) prelinkedModules->getObject(OSReadBigInt32(prelink, idx) - 1);
	    str = OSDynamicCast(OSString, depDict->getObject(kModuleKey));
	    depInfo = kmod_lookupbyname_locked(str->getCStringNoCopy());
	    if (depInfo)
	    {
		kr = kmod_retain(KMOD_PACK_IDS(id, depInfo->id));
		kfree((vm_offset_t) depInfo, sizeof(kmod_info_t));
	    } else
		IOLog("%s: NO DEP %s\n", kmod_info->name, str->getCStringNoCopy());
	}
	dict->removeObject("OSBundlePrelink");

	if (kmod_info->start)
	    kr = kmod_start_or_stop(kmod_info->id, 1, 0, 0);
    }

    IODelete(togo, UInt32, prelinkedModules->getCount());

    return kr;
}

/*********************************************************************
* This is a function that IOCatalogue calls in order to load a kmod.
*********************************************************************/

static 
kern_return_t kmod_load_from_cache_sym(const OSSymbol * kmod_name)
{
    OSArray *      prelinkedModules = gIOPrelinkedModules;
    kern_return_t  result = KERN_FAILURE;
    OSDictionary * dict;
    OSObject *     ident;
    UInt32	   idx;

    if (!gIOPrelinkedModules)
	return KERN_FAILURE;

    for (idx = 0; 
	 (dict = (OSDictionary *) prelinkedModules->getObject(idx));
	 idx++)
    {
	if ((ident = dict->getObject(kModuleKey))
	 && kmod_name->isEqualTo(ident))
	    break;
    }
    if (dict) 
    {
	if (kernelLinkerPresent && dict->getObject("OSBundleDefer"))
	{
	    kmod_load_extension((char *) kmod_name->getCStringNoCopy());
	    result = kIOReturnOffline;
	}
	else
	    result = start_prelink_module(idx);
    }

    return result;
}

extern "C" Boolean kmod_load_request(const char * moduleName, Boolean make_request)
{
    bool 		ret, cacheMiss = false;
    kern_return_t	kr;
    const OSSymbol *	sym = 0;
    kmod_info_t *	kmod_info;

    if (!moduleName)
        return false;

    /* To make sure this operation completes even if a bad extension needs
    * to be removed, take the kld lock for this whole block, spanning the
    * kmod_load_function() and remove_startup_extension_function() calls.
    */
    IOLockLock(gIOKLDLock);
    do
    {
	// Is the module already loaded?
	ret = (0 != (kmod_info = kmod_lookupbyname_locked((char *)moduleName)));
	if (ret) {
	    kfree((vm_offset_t) kmod_info, sizeof(kmod_info_t));
	    break;
	}
	sym = OSSymbol::withCString(moduleName);
	if (!sym) {
	    ret = false;
	    break;
	}

	kr = kmod_load_from_cache_sym(sym);
	ret = (kIOReturnSuccess == kr);
	cacheMiss = !ret;
	if (ret || !make_request || (kr == kIOReturnOffline))
	    break;

        // If the module hasn't been loaded, then load it.
        if (!kmod_load_function) {
            IOLog("IOCatalogue: %s cannot be loaded "
                "(kmod load function not set).\n",
                moduleName);
	    break;
	}

	kr = kmod_load_function((char *)moduleName);

	if (ret != kIOReturnSuccess) {
	    IOLog("IOCatalogue: %s cannot be loaded.\n", moduleName);

	    /* If the extension couldn't be loaded this time,
	    * make it unavailable so that no more requests are
	    * made in vain. This also enables other matching
	    * extensions to have a chance.
	    */
	    if (kernelLinkerPresent && remove_startup_extension_function) {
		(*remove_startup_extension_function)(moduleName);
	    }
	    ret = false;

	} else if (kernelLinkerPresent) {
	    // If kern linker is here, the driver is actually loaded,
	    // so return true.
	    ret = true;

	} else {
	    // kern linker isn't here, a request has been queued
	    // but the module isn't necessarily loaded yet, so stall.
	    ret = false;
	}
    }
    while (false);

    IOLockUnlock(gIOKLDLock);

    if (sym)
    {
	IOLockLock(gIOCatalogLock);
	gIOCatalogModuleRequests->setObject(sym);
	if (cacheMiss)
	    gIOCatalogCacheMisses->setObject(sym);
	IOLockUnlock(gIOCatalogLock);
    }

    return ret;
}

extern "C" kern_return_t kmod_unload_cache(void)
{
    OSArray *      prelinkedModules = gIOPrelinkedModules;
    kern_return_t  result = KERN_FAILURE;
    OSDictionary * dict;
    UInt32	   idx;
    UInt32 *       prelink;
    OSData *       data;

    if (!gIOPrelinkedModules)
	return KERN_SUCCESS;

    IOLockLock(gIOKLDLock);
    for (idx = 0; 
	 (dict = (OSDictionary *) prelinkedModules->getObject(idx));
	 idx++)
    {
	data = OSDynamicCast(OSData, dict->getObject("OSBundlePrelink"));
	if (!data)
	    continue;
	prelink = (UInt32 *) data->getBytesNoCopy();
    
	kmod_info_t * kmod_info = (kmod_info_t *) OSReadBigInt32(prelink, 0);
	vm_offset_t
	virt = ml_static_ptovirt(kmod_info->address);
	if( virt) {
	    ml_static_mfree(virt, kmod_info->size);
	}
    }

    gIOPrelinkedModules->release();
    gIOPrelinkedModules = 0;

    IOLockUnlock(gIOKLDLock);

    return result;
}

extern "C" kern_return_t kmod_load_from_cache(const char * kmod_name)
{
    kern_return_t kr;
    const OSSymbol * sym = OSSymbol::withCStringNoCopy(kmod_name);

    if (sym)
    {
	kr = kmod_load_from_cache_sym(sym);
	sym->release();
    }
    else
	kr = kIOReturnNoMemory;

    return kr;
}

/*********************************************************************
*********************************************************************/

static void UniqueProperties( OSDictionary * dict )
{
    OSString             * data;

    data = OSDynamicCast( OSString, dict->getObject( gIOClassKey ));
    if( data) {
        const OSSymbol *classSymbol = OSSymbol::withString(data);

        dict->setObject( gIOClassKey, (OSSymbol *) classSymbol);
        classSymbol->release();
    }

    data = OSDynamicCast( OSString, dict->getObject( gIOMatchCategoryKey ));
    if( data) {
        const OSSymbol *classSymbol = OSSymbol::withString(data);

        dict->setObject( gIOMatchCategoryKey, (OSSymbol *) classSymbol);
        classSymbol->release();
    }
}

void IOCatalogue::initialize( void )
{
    OSArray              * array;
    OSString             * errorString;
    bool		   rc;

    extern const char * gIOKernelConfigTables;

    array = OSDynamicCast(OSArray, OSUnserialize(gIOKernelConfigTables, &errorString));
    if (!array && errorString) {
	IOLog("KernelConfigTables syntax error: %s\n",
		errorString->getCStringNoCopy());
	errorString->release();
    }

    gIOClassKey              = OSSymbol::withCStringNoCopy( kIOClassKey );
    gIOProbeScoreKey 	     = OSSymbol::withCStringNoCopy( kIOProbeScoreKey );
    gIOModuleIdentifierKey   = OSSymbol::withCStringNoCopy( kModuleKey );
    gIOCatalogModuleRequests = OSSet::withCapacity(16);
    gIOCatalogCacheMisses    = OSSet::withCapacity(16);
    gIOCatalogROMMkexts      = OSSet::withCapacity(4);

    assert( array && gIOClassKey && gIOProbeScoreKey 
	    && gIOModuleIdentifierKey && gIOCatalogModuleRequests);

    gIOCatalogue = new IOCatalogue;
    assert(gIOCatalogue);
    rc = gIOCatalogue->init(array);
    assert(rc);
    array->release();
}

// Initialize the IOCatalog object.
bool IOCatalogue::init(OSArray * initArray)
{
    IORegistryEntry      * entry;
    OSDictionary         * dict;
    
    if ( !super::init() )
        return false;

    generation = 1;
    
    array = initArray;
    array->retain();
    kernelTables = OSCollectionIterator::withCollection( array );

    gIOCatalogLock = IOLockAlloc();
    gIOKLDLock     = IOLockAlloc();

    lock     = gIOCatalogLock;
    kld_lock = gIOKLDLock;

    kernelTables->reset();
    while( (dict = (OSDictionary *) kernelTables->getNextObject())) {
        UniqueProperties(dict);
        if( 0 == dict->getObject( gIOClassKey ))
            IOLog("Missing or bad \"%s\" key\n",
                    gIOClassKey->getCStringNoCopy());
    }

#if CATALOGTEST
    AbsoluteTime deadline;
    clock_interval_to_deadline( 1000, kMillisecondScale );
    thread_call_func_delayed( ping, this, deadline );
#endif

    entry = IORegistryEntry::getRegistryRoot();
    if ( entry )
        entry->setProperty(kIOCatalogueKey, this);

    return true;
}

// Release all resources used by IOCatalogue and deallocate.
// This will probably never be called.
void IOCatalogue::free( void )
{
    if ( array )
        array->release();

    if ( kernelTables )
        kernelTables->release();
    
    super::free();
}

#if CATALOGTEST

static int hackLimit;

enum { kDriversPerIter = 4 };

void IOCatalogue::ping( thread_call_param_t arg, thread_call_param_t)
{
    IOCatalogue 	 * self = (IOCatalogue *) arg;
    OSOrderedSet         * set;
    OSDictionary         * table;
    int	                   newLimit;

    set = OSOrderedSet::withCapacity( 1 );

    IOLockLock( &self->lock );

    for( newLimit = 0; newLimit < kDriversPerIter; newLimit++) {
	table = (OSDictionary *) self->array->getObject(
					hackLimit + newLimit );
	if( table) {
	    set->setLastObject( table );

	    OSSymbol * sym = (OSSymbol *) table->getObject( gIOClassKey );
	    kprintf("enabling %s\n", sym->getCStringNoCopy());

	} else {
	    newLimit--;
	    break;
	}
    }

    IOService::catalogNewDrivers( set );

    hackLimit += newLimit;
    self->generation++;

    IOLockUnlock( &self->lock );

    if( kDriversPerIter == newLimit) {
        AbsoluteTime deadline;
        clock_interval_to_deadline( 500, kMillisecondScale );
        thread_call_func_delayed( ping, this, deadline );
    }
}
#endif

OSOrderedSet * IOCatalogue::findDrivers( IOService * service,
					SInt32 * generationCount )
{
    OSDictionary         * nextTable;
    OSOrderedSet         * set;
    OSString             * imports;

    set = OSOrderedSet::withCapacity( 1, IOServiceOrdering,
                                      (void *)gIOProbeScoreKey );
    if( !set )
	return( 0 );

    IOLockLock( lock );
    kernelTables->reset();

#if CATALOGTEST
    int hackIndex = 0;
#endif
    while( (nextTable = (OSDictionary *) kernelTables->getNextObject())) {
#if CATALOGTEST
	if( hackIndex++ > hackLimit)
	    break;
#endif
        imports = OSDynamicCast( OSString,
			nextTable->getObject( gIOProviderClassKey ));
	if( imports && service->metaCast( imports ))
            set->setObject( nextTable );
    }

    *generationCount = getGenerationCount();

    IOLockUnlock( lock );

    return( set );
}

// Is personality already in the catalog?
OSOrderedSet * IOCatalogue::findDrivers( OSDictionary * matching,
                                         SInt32 * generationCount)
{
    OSDictionary         * dict;
    OSOrderedSet         * set;

    UniqueProperties(matching);

    set = OSOrderedSet::withCapacity( 1, IOServiceOrdering,
                                      (void *)gIOProbeScoreKey );

    IOLockLock( lock );
    kernelTables->reset();
    while ( (dict = (OSDictionary *) kernelTables->getNextObject()) ) {

       /* This comparison must be done with only the keys in the
        * "matching" dict to enable general searches.
        */
        if ( dict->isEqualTo(matching, matching) )
            set->setObject(dict);
    }
    *generationCount = getGenerationCount();
    IOLockUnlock( lock );

    return set;
}

// Add a new personality to the set if it has a unique IOResourceMatchKey value.
// XXX -- svail: This should be optimized.
// esb - There doesn't seem like any reason to do this - it causes problems
// esb - when there are more than one loadable driver matching on the same provider class
static void AddNewImports( OSOrderedSet * set, OSDictionary * dict )
{
    set->setObject(dict);
}

// Add driver config tables to catalog and start matching process.
bool IOCatalogue::addDrivers(OSArray * drivers,
                              bool doNubMatching )
{
    OSCollectionIterator * iter;
    OSDictionary         * dict;
    OSOrderedSet         * set;
    OSArray              * persons;
    OSString             * moduleName;
    bool                   ret;

    ret = true;
    persons = OSDynamicCast(OSArray, drivers);
    if ( !persons )
        return false;

    iter = OSCollectionIterator::withCollection( persons );
    if (!iter )
        return false;
    
    set = OSOrderedSet::withCapacity( 10, IOServiceOrdering,
                                      (void *)gIOProbeScoreKey );
    if ( !set ) {
        iter->release();
        return false;
    }

    IOLockLock( lock );
    while ( (dict = (OSDictionary *) iter->getNextObject()) )
    {
	if ((moduleName = OSDynamicCast(OSString, dict->getObject("OSBundleModuleDemand"))))
	{
	    IOLockUnlock( lock );
	    ret = kmod_load_request(moduleName->getCStringNoCopy(), false);
	    IOLockLock( lock );
	    ret = true;
	}
	else
	{
	    SInt count;
	    
	    UniqueProperties( dict );
    
	    // Add driver personality to catalogue.
	    count = array->getCount();
	    while ( count-- ) {
		OSDictionary * driver;
    
		// Be sure not to double up on personalities.
		driver = (OSDictionary *)array->getObject(count);
    
	    /* Unlike in other functions, this comparison must be exact!
		* The catalogue must be able to contain personalities that
		* are proper supersets of others.
		* Do not compare just the properties present in one driver
		* pesonality or the other.
		*/
		if (dict->isEqualTo(driver))
		    break;
	    }
	    if (count >= 0)
		// its a dup
		continue;
	    
	    ret = array->setObject( dict );
	    if (!ret)
		break;
    
	    AddNewImports( set, dict );
	}
    }
    // Start device matching.
    if (doNubMatching && (set->getCount() > 0)) {
        IOService::catalogNewDrivers( set );
        generation++;
    }
    IOLockUnlock( lock );

    set->release();
    iter->release();
    
    return ret;
}

// Remove drivers from the catalog which match the
// properties in the matching dictionary.
bool IOCatalogue::removeDrivers( OSDictionary * matching,
                                 bool doNubMatching)
{
    OSCollectionIterator * tables;
    OSDictionary         * dict;
    OSOrderedSet         * set;
    OSArray              * arrayCopy;

    if ( !matching )
        return false;

    set = OSOrderedSet::withCapacity(10,
                                     IOServiceOrdering,
                                     (void *)gIOProbeScoreKey);
    if ( !set )
        return false;

    arrayCopy = OSArray::withCapacity(100);
    if ( !arrayCopy ) {
        set->release();
        return false;
    }
    
    tables = OSCollectionIterator::withCollection(arrayCopy);
    arrayCopy->release();
    if ( !tables ) {
        set->release();
        return false;
    }

    UniqueProperties( matching );

    IOLockLock( lock );
    kernelTables->reset();
    arrayCopy->merge(array);
    array->flushCollection();
    tables->reset();
    while ( (dict = (OSDictionary *)tables->getNextObject()) ) {

       /* This comparison must be done with only the keys in the
        * "matching" dict to enable general searches.
        */
        if ( dict->isEqualTo(matching, matching) ) {
            AddNewImports( set, dict );
            continue;
        }

        array->setObject(dict);
    }
    // Start device matching.
    if ( doNubMatching && (set->getCount() > 0) ) {
        IOService::catalogNewDrivers(set);
        generation++;
    }
    IOLockUnlock( lock );
    
    set->release();
    tables->release();
    
    return true;
}

// Return the generation count.
SInt32 IOCatalogue::getGenerationCount( void ) const
{
    return( generation );
}

bool IOCatalogue::isModuleLoaded( OSString * moduleName ) const
{
    return isModuleLoaded(moduleName->getCStringNoCopy());
}

bool IOCatalogue::isModuleLoaded( const char * moduleName ) const
{
    return (kmod_load_request(moduleName, true));
}

// Check to see if module has been loaded already.
bool IOCatalogue::isModuleLoaded( OSDictionary * driver ) const
{
    OSString             * moduleName = NULL;

    if ( !driver )
        return false;

    moduleName = OSDynamicCast(OSString, driver->getObject(gIOModuleIdentifierKey));
    if ( moduleName )
        return isModuleLoaded(moduleName);

   /* If a personality doesn't hold the "CFBundleIdentifier" key
    * it is assumed to be an "in-kernel" driver.
    */
    return true;
}

// This function is called after a module has been loaded.
void IOCatalogue::moduleHasLoaded( OSString * moduleName )
{
    OSDictionary         * dict;

    dict = OSDictionary::withCapacity(2);
    dict->setObject(gIOModuleIdentifierKey, moduleName);
    startMatching(dict);
    dict->release();
}

void IOCatalogue::moduleHasLoaded( const char * moduleName )
{
    OSString             * name;

    name = OSString::withCString(moduleName);
    moduleHasLoaded(name);
    name->release();
}

IOReturn IOCatalogue::unloadModule( OSString * moduleName ) const
{
    kmod_info_t          * k_info = 0;
    kern_return_t          ret;
    const char           * name;

    ret = kIOReturnBadArgument;
    if ( moduleName ) {
        name = moduleName->getCStringNoCopy();
        k_info = kmod_lookupbyname_locked((char *)name);
        if ( k_info && (k_info->reference_count < 1) ) {
            if ( k_info->stop &&
                 !((ret = k_info->stop(k_info, 0)) == kIOReturnSuccess) ) {

                kfree((vm_offset_t) k_info, sizeof(kmod_info_t));
                return ret;
           }
            
           ret = kmod_destroy(host_priv_self(), k_info->id);
        }
    }
 
    if (k_info) {
        kfree((vm_offset_t) k_info, sizeof(kmod_info_t));
    }

    return ret;
}

static IOReturn _terminateDrivers( OSArray * array, OSDictionary * matching )
{
    OSCollectionIterator * tables;
    OSDictionary         * dict;
    OSIterator           * iter;
    OSArray              * arrayCopy;
    IOService            * service;
    IOReturn               ret;

    if ( !matching )
        return kIOReturnBadArgument;

    ret = kIOReturnSuccess;
    dict = 0;
    iter = IORegistryIterator::iterateOver(gIOServicePlane,
                                kIORegistryIterateRecursively);
    if ( !iter )
        return kIOReturnNoMemory;

    UniqueProperties( matching );

    // terminate instances.
    do {
        iter->reset();
        while( (service = (IOService *)iter->getNextObject()) ) {
            dict = service->getPropertyTable();
            if ( !dict )
                continue;

           /* Terminate only for personalities that match the matching dictionary.
            * This comparison must be done with only the keys in the
            * "matching" dict to enable general matching.
            */
            if ( !dict->isEqualTo(matching, matching) )
                 continue;

            if ( !service->terminate(kIOServiceRequired|kIOServiceSynchronous) ) {
                ret = kIOReturnUnsupported;
                break;
            }
        }
    } while( !service && !iter->isValid());
    iter->release();

    // remove configs from catalog.
    if ( ret != kIOReturnSuccess ) 
        return ret;

    arrayCopy = OSArray::withCapacity(100);
    if ( !arrayCopy )
        return kIOReturnNoMemory;

    tables = OSCollectionIterator::withCollection(arrayCopy);
    arrayCopy->release();
    if ( !tables )
        return kIOReturnNoMemory;

    arrayCopy->merge(array);
    array->flushCollection();
    tables->reset();
    while ( (dict = (OSDictionary *)tables->getNextObject()) ) {

       /* Remove from the catalogue's array any personalities
        * that match the matching dictionary.
        * This comparison must be done with only the keys in the
        * "matching" dict to enable general matching.
        */
        if ( dict->isEqualTo(matching, matching) )
            continue;

        array->setObject(dict);
    }

    tables->release();

    return ret;
}

IOReturn IOCatalogue::terminateDrivers( OSDictionary * matching )
{
    IOReturn ret;

    ret = kIOReturnSuccess;
    IOLockLock( lock );
    ret = _terminateDrivers(array, matching);
    kernelTables->reset();
    IOLockUnlock( lock );

    return ret;
}

IOReturn IOCatalogue::terminateDriversForModule(
                                      OSString * moduleName,
                                      bool unload )
{
    IOReturn ret;
    OSDictionary * dict;

    dict = OSDictionary::withCapacity(1);
    if ( !dict )
        return kIOReturnNoMemory;

    dict->setObject(gIOModuleIdentifierKey, moduleName);

    IOLockLock( lock );

    ret = _terminateDrivers(array, dict);
    kernelTables->reset();

    // Unload the module itself.
    if ( unload && ret == kIOReturnSuccess ) {
        // Do kmod stop first.
        ret = unloadModule(moduleName);
    }

    IOLockUnlock( lock );

    dict->release();

    return ret;
}

IOReturn IOCatalogue::terminateDriversForModule(
                                      const char * moduleName,
                                      bool unload )
{
    OSString * name;
    IOReturn ret;

    name = OSString::withCString(moduleName);
    if ( !name )
        return kIOReturnNoMemory;

    ret = terminateDriversForModule(name, unload);
    name->release();

    return ret;
}

bool IOCatalogue::startMatching( OSDictionary * matching )
{
    OSDictionary         * dict;
    OSOrderedSet         * set;
    
    if ( !matching )
        return false;

    set = OSOrderedSet::withCapacity(10, IOServiceOrdering,
                                     (void *)gIOProbeScoreKey);
    if ( !set )
        return false;

    IOLockLock( lock );
    kernelTables->reset();

    while ( (dict = (OSDictionary *)kernelTables->getNextObject()) ) {

       /* This comparison must be done with only the keys in the
        * "matching" dict to enable general matching.
        */
        if ( dict->isEqualTo(matching, matching) )
            AddNewImports(set, dict);
    }
    // Start device matching.
    if ( set->getCount() > 0 ) {
        IOService::catalogNewDrivers(set);
        generation++;
    }

    IOLockUnlock( lock );

    set->release();

    return true;
}

void IOCatalogue::reset(void)
{
    IOLog("Resetting IOCatalogue.\n");
}

bool IOCatalogue::serialize(OSSerialize * s) const
{
    bool ret;
    
    if ( !s )
        return false;

    IOLockLock( lock );

    ret = array->serialize(s);

    IOLockUnlock( lock );

    return ret;
}

bool IOCatalogue::serializeData(IOOptionBits kind, OSSerialize * s) const
{
    kern_return_t kr = kIOReturnSuccess;

    switch ( kind )
    {
        case kIOCatalogGetContents:
            if (!serialize(s))
                kr = kIOReturnNoMemory;
            break;

        case kIOCatalogGetModuleDemandList:
	    IOLockLock( lock );
            if (!gIOCatalogModuleRequests->serialize(s))
                kr = kIOReturnNoMemory;
	    IOLockUnlock( lock );
            break;

        case kIOCatalogGetCacheMissList:
	    IOLockLock( lock );
            if (!gIOCatalogCacheMisses->serialize(s))
                kr = kIOReturnNoMemory;
	    IOLockUnlock( lock );
            break;

        case kIOCatalogGetROMMkextList:
	    IOLockLock( lock );

	    if (!gIOCatalogROMMkexts || !gIOCatalogROMMkexts->getCount())
		kr = kIOReturnNoResources;
            else if (!gIOCatalogROMMkexts->serialize(s))
                kr = kIOReturnNoMemory;

	    if (gIOCatalogROMMkexts)
	    {
		gIOCatalogROMMkexts->release();
		gIOCatalogROMMkexts = 0;
	    }

	    IOLockUnlock( lock );
            break;

        default:
            kr = kIOReturnBadArgument;
            break;
    }

    return kr;
}


bool IOCatalogue::recordStartupExtensions(void) {
    bool result = false;

    IOLockLock(kld_lock);
    if (kernelLinkerPresent && record_startup_extensions_function) {
        result = (*record_startup_extensions_function)();
    } else {
        IOLog("Can't record startup extensions; "
            "kernel linker is not present.\n");
        result = false;
    }
    IOLockUnlock(kld_lock);

    return result;
}


/*********************************************************************
*********************************************************************/
bool IOCatalogue::addExtensionsFromArchive(OSData * mkext)
{
    OSData * copyData;
    bool result = false;
    bool prelinked;

   /* The mkext we've been handed (or the data it references) can go away,
    * so we need to make a local copy to keep around as long as it might
    * be needed.
    */
    copyData = OSData::withData(mkext);
    if (copyData)
    {
	struct section * infosect;
    
	infosect  = getsectbyname("__PRELINK", "__info");
	prelinked = (infosect && infosect->addr && infosect->size);

	IOLockLock(kld_lock);

	if (gIOCatalogROMMkexts)
	    gIOCatalogROMMkexts->setObject(copyData);

	if (prelinked) {
	    result = true;
	} else if (kernelLinkerPresent && add_from_mkext_function) {
	    result = (*add_from_mkext_function)(copyData);
	} else {
	    IOLog("Can't add startup extensions from archive; "
		"kernel linker is not present.\n");
	    result = false;
	}

	IOLockUnlock(kld_lock);

	copyData->release();
    }

    return result;
}


/*********************************************************************
* This function clears out all references to the in-kernel linker,
* frees the list of startup extensions in extensionDict, and
* deallocates the kernel's __KLD segment to reclaim that memory.
*********************************************************************/
kern_return_t IOCatalogue::removeKernelLinker(void) {
    kern_return_t result = KERN_SUCCESS;
    struct segment_command * segment;
    char * dt_segment_name;
    void * segment_paddress;
    int    segment_size;

   /* This must be the very first thing done by this function.
    */
    IOLockLock(kld_lock);


   /* If the kernel linker isn't here, that's automatically
    * a success.
    */
    if (!kernelLinkerPresent) {
        result = KERN_SUCCESS;
        goto finish;
    }

    IOLog("Jettisoning kernel linker.\n");

    kernelLinkerPresent = 0;

   /* Set the kmod_load_extension function as the means for loading
    * a kernel extension.
    */
    kmod_load_function = &kmod_load_extension;

    record_startup_extensions_function = 0;
    add_from_mkext_function = 0;
    remove_startup_extension_function = 0;


   /* Invoke destructors for the __KLD and __LINKEDIT segments.
    * Do this for all segments before actually freeing their
    * memory so that any cross-dependencies (not that there
    * should be any) are handled.
    */
    segment = getsegbyname("__KLD");
    if (!segment) {
        IOLog("error removing kernel linker: can't find %s segment\n",
            "__KLD");
        result = KERN_FAILURE;
        goto finish;
    }
    OSRuntimeUnloadCPPForSegment(segment);

    segment = getsegbyname("__LINKEDIT");
    if (!segment) {
        IOLog("error removing kernel linker: can't find %s segment\n",
            "__LINKEDIT");
        result = KERN_FAILURE;
        goto finish;
    }
    OSRuntimeUnloadCPPForSegment(segment);


   /* Free the memory that was set up by bootx.
    */
    dt_segment_name = "Kernel-__KLD";
    if (0 == IODTGetLoaderInfo(dt_segment_name, &segment_paddress, &segment_size)) {
        IODTFreeLoaderInfo(dt_segment_name, (void *)segment_paddress,
            (int)segment_size);
    }

    dt_segment_name = "Kernel-__LINKEDIT";
    if (0 == IODTGetLoaderInfo(dt_segment_name, &segment_paddress, &segment_size)) {
        IODTFreeLoaderInfo(dt_segment_name, (void *)segment_paddress,
            (int)segment_size);
    }

    struct section * sect;
    sect = getsectbyname("__PRELINK", "__symtab");
    if (sect && sect->addr)
    {
	vm_offset_t
	virt = ml_static_ptovirt(sect->addr);
	if( virt) {
	    ml_static_mfree(virt, sect->size);
	}
    }

finish:

   /* This must be the very last thing done before returning.
    */
    IOLockUnlock(kld_lock);

    return result;
}

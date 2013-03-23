/*
 * Copyright (c) 1998-2000 Apple Computer, Inc. All rights reserved.
 *
 * @APPLE_LICENSE_HEADER_START@
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

#include <IOKit/IODeviceMemory.h>

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

IODeviceMemory * IODeviceMemory::withRange( 
	IOPhysicalAddress	start,
	IOPhysicalLength	length )
{
    return( (IODeviceMemory *) IOMemoryDescriptor::withPhysicalAddress(
			start, length, kIODirectionNone ));
}


IODeviceMemory * IODeviceMemory::withSubRange( 
	IODeviceMemory *	of,
	IOPhysicalAddress	offset,
	IOPhysicalLength	length )
{
    return( (IODeviceMemory *) IOMemoryDescriptor::withSubRange(
		of, offset, length, kIODirectionNone ));
}


OSArray * IODeviceMemory::arrayFromList(
	InitElement		list[],
	IOItemCount		count )
{
    OSArray *		array;
    IODeviceMemory *	range;
    IOItemCount		i;

    array = OSArray::withCapacity( count );
    if( 0 == array )
	return( 0);

    for( i = 0; i < count; i++) {
	range = IODeviceMemory::withRange( list[i].start, list[i].length );
	if( range) {
	    range->setTag( list[i].tag );
	    array->setObject( range);
	    range->release();
	} else {
	    array->release();
	    array = 0;
	    break;
	}
    }

    return( array );
}


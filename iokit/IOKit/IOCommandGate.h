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
/*[
    1999-8-10	Godfrey van der Linden(gvdl)
	Created.
]*/
/*! @language embedded-c++ */

#ifndef _IOKIT_IOCOMMANDGATE_H
#define _IOKIT_IOCOMMANDGATE_H

#include <IOKit/IOEventSource.h>

/*!
    @class IOCommandGate : public IOEventSource
    @abstract Single-threaded work-loop client request mechanism.
    @discussion An IOCommandGate instance is an extremely light way mechanism
that executes an action on the driver's work-loop.  'On the work-loop' is
actually a lie but the work-loop single threaded semantic is maintained for this
event source.  Using the work-loop gate rather than execution by the workloop.
The command gate tests for a potential self dead lock by checking if the
runCommand request is made from the work-loop's thread, it doens't check for a
mutual dead lock though where a pair of work loop's dead lock each other.
<br><br>
	The IOCommandGate is a lighter weight version of the IOCommandQueue and
should be used in preference.  Generally use a command queue whenever you need a
client to submit a request to a work loop.  A typical command gate action would
check if the hardware is active, if so it will add the request to a pending
queue internal to the device or the device's family.  Otherwise if the hardware
is inactive then this request can be acted upon immediately.
<br><br>
	CAUTION: The runAction and runCommand functions can not be called from an interrupt context.

*/
class IOCommandGate : public IOEventSource
{
    OSDeclareDefaultStructors(IOCommandGate)

public:
/*!
    @typedef Action
    @discussion Type and arguments of callout C function that is used when
a runCommand is executed by a client.  Cast to this type when you want a C++
member function to be used.  Note the arg1 - arg3 parameters are straight pass
through from the runCommand to the action callout.
    @param owner
	Target of the function, can be used as a refcon.  The owner is set
during initialisation of the IOCommandGate instance.	 Note if a C++ function
was specified this parameter is implicitly the first paramter in the target
member function's parameter list.
    @param arg0 Argument to action from run operation.
    @param arg1 Argument to action from run operation.
    @param arg2 Argument to action from run operation.
    @param arg3 Argument to action from run operation.
*/
    typedef IOReturn (*Action)(OSObject *owner,
			       void *arg0, void *arg1,
			       void *arg2, void *arg3);

protected:
/*!
    @function checkForWork
    @abstract Not used, $link IOEventSource::checkForWork(). */
    virtual bool checkForWork();

/*! @struct ExpansionData
    @discussion This structure will be used to expand the capablilties of the IOWorkLoop in the future.
    */    
    struct ExpansionData { };

/*! @var reserved
    Reserved for future use.  (Internal use only)  */
    ExpansionData *reserved;

public:
/*! @function commandGate
    @abstract Factory method to create and initialise an IOCommandGate, See $link init.
    @result Returns a pointer to the new command gate if sucessful, 0 otherwise. */
    static IOCommandGate *commandGate(OSObject *owner, Action action = 0);

/*! @function init
    @abstract Class initialiser.
    @discussion Initialiser for IOCommandGate operates only on newly 'newed'
objects.  Shouldn't be used to re-init an existing instance.
    @param owner 	Owner of this, newly created, instance of the IOCommandGate.  This argument will be used as the first parameter in the action callout.
    @param action
	Pointer to a C function that is called whenever a client of the
IOCommandGate calls runCommand.	 NB Can be a C++ member function but caller
must cast the member function to $link IOCommandGate::Action and they will get a
compiler warning.  Defaults to zero, see $link IOEventSource::setAction.
    @result True if inherited classes initialise successfully. */
    virtual bool init(OSObject *owner, Action action = 0);

/*! @function runCommand
    @abstract Single thread a command with the target work-loop.
    @discussion Client function that causes the current action to be called in
a single threaded manner.  Beware the work-loop's gate is recursive and command
gates can cause direct or indirect re-entrancy.	 When the executing on a
client's thread runCommand will sleep until the work-loop's gate opens for
execution of client actions, the action is single threaded against all other
work-loop event sources.
    @param arg0 Parameter for action of command gate, defaults to 0.
    @param arg1 Parameter for action of command gate, defaults to 0.
    @param arg2 Parameter for action of command gate, defaults to 0.
    @param arg3 Parameter for action of command gate, defaults to 0.
    @result kIOReturnSuccess if successful. kIOReturnNotPermitted if this
event source is currently disabled, kIOReturnNoResources if no action available.
*/
    virtual IOReturn runCommand(void *arg0 = 0, void *arg1 = 0,
				void *arg2 = 0, void *arg3 = 0);

/*! @function runAction
    @abstract Single thread a call to an action with the target work-loop.
    @discussion Client function that causes the given action to be called in
a single threaded manner.  Beware the work-loop's gate is recursive and command
gates can cause direct or indirect re-entrancy.	 When the executing on a
client's thread runCommand will sleep until the work-loop's gate opens for
execution of client actions, the action is single threaded against all other
work-loop event sources.
    @param action Pointer to function to be executed in work-loop context.
    @param arg0 Parameter for action parameter, defaults to 0.
    @param arg1 Parameter for action parameter, defaults to 0.
    @param arg2 Parameter for action parameter, defaults to 0.
    @param arg3 Parameter for action parameter, defaults to 0.
    @result kIOReturnSuccess if successful. kIOReturnBadArgument if action is not defined, kIOReturnNotPermitted if this event source is currently disabled.
*/
    virtual IOReturn runAction(Action action,
			       void *arg0 = 0, void *arg1 = 0,
			       void *arg2 = 0, void *arg3 = 0);

/*! @function attemptCommand
    @abstract Single thread a command with the target work-loop.
    @discussion Client function that causes the current action to be called in
a single threaded manner.  Beware the work-loop's gate is recursive and command
gates can cause direct or indirect re-entrancy.	 When the executing on a
client's thread attemptCommand will fail if the work-loop's gate is open.
    @param arg0 Parameter for action of command gate, defaults to 0.
    @param arg1 Parameter for action of command gate, defaults to 0.
    @param arg2 Parameter for action of command gate, defaults to 0.
    @param arg3 Parameter for action of command gate, defaults to 0.
    @result kIOReturnSuccess if successful. kIOReturnNotPermitted if this event source is currently disabled, kIOReturnNoResources if no action available, kIOReturnCannotLock if lock attempt fails.
*/
    virtual IOReturn attemptCommand(void *arg0 = 0, void *arg1 = 0,
                                    void *arg2 = 0, void *arg3 = 0);

/*! @function attemptAction
    @abstract Single thread a call to an action with the target work-loop.
    @discussion Client function that causes the given action to be called in
a single threaded manner.  Beware the work-loop's gate is recursive and command
gates can cause direct or indirect re-entrancy.	 When the executing on a
client's thread attemptCommand will fail if the work-loop's gate is open.
    @param action Pointer to function to be executed in work-loop context.
    @param arg0 Parameter for action parameter, defaults to 0.
    @param arg1 Parameter for action parameter, defaults to 0.
    @param arg2 Parameter for action parameter, defaults to 0.
    @param arg3 Parameter for action parameter, defaults to 0.
    @result kIOReturnSuccess if successful. kIOReturnBadArgument if action is not defined, kIOReturnNotPermitted if this event source is currently disabled, kIOReturnCannotLock if lock attempt fails.

*/
    virtual IOReturn attemptAction(Action action,
                                   void *arg0 = 0, void *arg1 = 0,
                                   void *arg2 = 0, void *arg3 = 0);

/*! @function commandSleep  
    @abstract Put a thread that is currently holding the command gate to sleep.
    @discussion Put a thread to sleep waiting for an event but release the gate first.  If the event occurs then the commandGate is closed before the  returns.
    @param event Pointer to an address.
    @param interruptible THREAD_UNINT, THREAD_INTERRUPTIBLE or THREAD_ABORTSAFE,  defaults to THREAD_ABORTSAFE.
    @result THREAD_AWAKENED - normal wakeup, THREAD_TIMED_OUT - timeout expired, THREAD_INTERRUPTED - interrupted by clear_wait, THREAD_RESTART - restart operation entirely, kIOReturnNotPermitted if the calling thread does not hold the command gate. */
    virtual IOReturn commandSleep(void *event,
                                  UInt32 interruptible = THREAD_ABORTSAFE);

/*! @function commandWakeup
    @abstract Wakeup one or more threads that are asleep on an event.
    @param event Pointer to an address.
    @param onlyOneThread true to only wake up at most one thread, false otherwise. */
    virtual void commandWakeup(void *event, bool oneThread = false);

private:
    OSMetaClassDeclareReservedUnused(IOCommandGate, 0);
    OSMetaClassDeclareReservedUnused(IOCommandGate, 1);
    OSMetaClassDeclareReservedUnused(IOCommandGate, 2);
    OSMetaClassDeclareReservedUnused(IOCommandGate, 3);
    OSMetaClassDeclareReservedUnused(IOCommandGate, 4);
    OSMetaClassDeclareReservedUnused(IOCommandGate, 5);
    OSMetaClassDeclareReservedUnused(IOCommandGate, 6);
    OSMetaClassDeclareReservedUnused(IOCommandGate, 7);
};

#endif /* !_IOKIT_IOCOMMANDGATE_H */

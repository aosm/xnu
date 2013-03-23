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
Copyright (c) 1998 Apple Computer, Inc.	 All rights reserved.
HISTORY
    1998-7-13	Godfrey van der Linden(gvdl)
	Created.
    1998-10-30	Godfrey van der Linden(gvdl)
	Converted to C++
*/

#ifndef __IOKIT_IOWORKLOOP_H
#define __IOKIT_IOWORKLOOP_H

#include <libkern/c++/OSObject.h>
#include <IOKit/IOReturn.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOLocks.h>

#include <IOKit/system.h>

class IOEventSource;
class IOCommandGate;

/*! @class IOWorkLoop : public OSObject
    @discussion An IOWorkLoop is a thread of control that is intended to be used to provide single threaded access to hardware.	 This class has no knowledge of the nature and type of the events that it marshals and forwards.  When an device driver sucessfully starts, See $link IOService::start it is expected to create the event sources it will need to receive events from.	Then a work loop is initialised and the events are added to the work loop for monitoring.  In general this set up will be automated by the family superclass of the specific device.
<br><br>
	The thread main method walks the event source linked list and messages each one requesting a work check.  At this point each event source is expected to notify their registered owner that the event has occured.  After each event has been walked and they indicate that another loop isn't required by the 'more' flag being false the thread will go to sleep on a signaling semaphore.
<br><br>
	When an event source is registered with a work loop it is informed of the semaphore to use to wake up the loop.*/
class IOWorkLoop : public OSObject
{
    OSDeclareDefaultStructors(IOWorkLoop)

public:
/*!
    @typedef Action
    @discussion Type and arguments of callout C function that is used when
a runCommand is executed by a client.  Cast to this type when you want a C++
member function to be used.  Note the arg1 - arg3 parameters are straight pass
through from the runCommand to the action callout.
    @param target
	Target of the function, can be used as a refcon.  Note if a C++ function
was specified this parameter is implicitly the first paramter in the target
member function's parameter list.
    @param arg0 Argument to action from run operation.
    @param arg1 Argument to action from run operation.
    @param arg2 Argument to action from run operation.
    @param arg3 Argument to action from run operation.
*/
    typedef IOReturn (*Action)(OSObject *target,
			       void *arg0, void *arg1,
			       void *arg2, void *arg3);

private:
/*! @function launchThreadMain
    @abstract Static function that setup thread state and calls the continuation function, $link threadMainContinuation */
    static void launchThreadMain(void *self);

/*! @function threadMainContinuation
    @abstract Static function that calls the $link threadMain function. */
    static void threadMainContinuation();

protected:

/*! @typedef maintCommandEnum
    @discussion Enumeration of commands that $link _maintCommand can deal with.
    @enum 
    @constant mAddEvent Used to tag a Remove event source command.
    @constant mRemoveEvent Used to tag a Remove event source command. */    
    typedef enum { mAddEvent, mRemoveEvent } maintCommandEnum;

/*! @var gateLock
    Mutual exlusion lock that used by close and open Gate functions.  */
    IORecursiveLock *gateLock;

/*! @var eventChain Pointer to first Event Source in linked list.  */
    IOEventSource *eventChain;

/*! @var controlG Internal control gate to maintain event system.  */
    IOCommandGate *controlG;

/*! @var workSpinLock
    The spin lock that is used to guard the 'workToDo' variable.  */
    IOSimpleLock *workToDoLock;

/*! @var workThread Work loop thread.	 */
    IOThread workThread;

/*! @var workToDo
    Used to to indicate that an interrupt has fired and needs to be processed.
*/
    volatile bool workToDo;

/*! @var loopRestart
    If event chain has been changed and the system has to be rechecked from start this flag is set.  (Internal use only)  */
    bool loopRestart;

/*! @struct ExpansionData
    @discussion This structure will be used to expand the capablilties of the IOWorkLoop in the future.
    */    
    struct ExpansionData { };

/*! @var reserved
    Reserved for future use.  (Internal use only)  */
    ExpansionData *reserved;

/*! @function _maintRequest
    @abstract Synchrounous implementation of $link addEventSource & $link removeEventSource functions. */
    virtual IOReturn _maintRequest(void *command, void *data, void *, void *);

/*! @function free
    @discussion Mandatory free of the object independent of the current retain count.  If the work loop is running this method will not return until the thread has succefully terminated.  Each event source in the chain will be released and the working semaphore will be destroyed.
<br><br>
	If the client has some outstanding requests on an event they will never be informed of completion.	If an external thread is blocked on any of the event sources they will be awoken with a KERN_INTERUPTED status. */
    virtual void free();

/*! @function threadMain
    @discussion Work loop threads main function.  This function consists of 3 loops: the outermost loop is the semaphore clear and wait loop, the middle loop terminates when there is no more work and the inside loop walks the event list calling the $link checkForWork method in each event source.  If an event source has more work to do then it can set the more flag and the middle loop will repeat.  When no more work is outstanding the outermost will sleep until and event is signaled or the least wakeupTime whichever occurs first.  If the event source does not require the semaphore wait to time out it must set the provided wakeupTime parameter to zero. */
    virtual void threadMain();

public:

/*! @function workLoop
    @abstract Factory member function to constuct and intialise a work loop.
    @result workLoop instance if constructed successfully, 0 otherwise. */
    static IOWorkLoop *workLoop();

/*! @function init
    @description
    Initialises an instance of the workloop.  This method creates and initialses the signaling semaphore and forks the thread that will continue executing.
    @result true if initialised successfully, false otherwise. */
    virtual bool init();

/*! @function getThread
    @abstract Get'ter for $link workThread.
    @result Returns workThread */
    virtual IOThread getThread() const;

/*! @function onThread
    @abstract Is the current execution context on the work thread? 
    @result Returns true if IOThreadSelf() == workThread. */
    virtual bool onThread() const;

/*! @function inGate
    @abstract Is the current execution context holding the work-loop's gate? 
    @result Returns true if IOThreadSelf() is gate holder. */
    virtual bool inGate() const;
    
/*! @function addEventSource
    @discussion Add an event source to be monitored by the work loop.  This function does not return until the work loop has acknowledged the arrival of the new event source.	When a new event has been added the threadMain will always restart it's loop and check all outstanding events.	The event source is retained by the work loop
    @param newEvent Pointer to $link IOEventSource subclass to add.
    @result Always returns kIOReturnSuccess. */
    virtual IOReturn addEventSource(IOEventSource *newEvent);

/*! @function removeEventSource
    @discussion Remove an event source from the work loop.  This function does not return until the work loop has acknowledged the removal of the event source.	 When an event has been removed the threadMain will always restart it's loop and check all outstanding events.	The event source will be released before return.
    @param toRemove Pointer to $link IOEventSource subclass to remove.
    @result kIOReturnSuccess if successful, kIOReturnBadArgument if toRemove couldn't be found. */
    virtual IOReturn removeEventSource(IOEventSource *toRemove);

/*! @function enableAllEventSources
    @abstract Call enable() in all event sources
    @discussion For all event sources in $link eventChain call enable() function.  See $link IOEventSource::enable()  */
    virtual void enableAllEventSources() const;

/*! @function disableAllEventSources
    @abstract Call disable() in all event sources
    @discussion For all event sources in $link eventChain call disable() function.  See $link IOEventSource::disable() */
    virtual void disableAllEventSources() const;

/*! @function enableAllInterrupts
    @abstract Call enable() in all interrupt event sources
    @discussion For all event sources, ES, for which IODynamicCast(IOInterruptEventSource, ES) is valid, in $link eventChain call enable() function.  See $link IOEventSource::enable()	 */
    virtual void enableAllInterrupts() const;

/*! @function disableAllInterrupts
    @abstract Call disable() in all interrupt event sources
    @discussion For all event sources, ES, for which IODynamicCast(IOInterruptEventSource, ES) is valid,  in $link eventChain call disable() function.	See $link IOEventSource::disable() */
    virtual void disableAllInterrupts() const;


protected:
    // Internal APIs used by event sources to control the thread
    friend class IOEventSource;
    virtual void signalWorkAvailable();
    virtual void openGate();
    virtual void closeGate();
    virtual bool tryCloseGate();
    virtual int sleepGate(void *event, UInt32 interuptibleType);
    virtual void wakeupGate(void *event, bool oneThread);

public:
    /* methods available in Mac OS X 10.1 or later */

/*! @function runAction
    @abstract Single thread a call to an action with the work-loop.
    @discussion Client function that causes the given action to be called in
a single threaded manner.  Beware the work-loop's gate is recursive and runAction
 can cause direct or indirect re-entrancy.	 When the executing on a
client's thread runAction will sleep until the work-loop's gate opens for
execution of client actions, the action is single threaded against all other
work-loop event sources.
    @param action Pointer to function to be executed in work-loop context.
    @param arg0 Parameter for action parameter, defaults to 0.
    @param arg1 Parameter for action parameter, defaults to 0.
    @param arg2 Parameter for action parameter, defaults to 0.
    @param arg3 Parameter for action parameter, defaults to 0.
    @result return value of the Action callout.
*/
    virtual IOReturn runAction(Action action, OSObject *target,
			       void *arg0 = 0, void *arg1 = 0,
			       void *arg2 = 0, void *arg3 = 0);

protected:
    OSMetaClassDeclareReservedUsed(IOWorkLoop, 0);

    OSMetaClassDeclareReservedUnused(IOWorkLoop, 1);
    OSMetaClassDeclareReservedUnused(IOWorkLoop, 2);
    OSMetaClassDeclareReservedUnused(IOWorkLoop, 3);
    OSMetaClassDeclareReservedUnused(IOWorkLoop, 4);
    OSMetaClassDeclareReservedUnused(IOWorkLoop, 5);
    OSMetaClassDeclareReservedUnused(IOWorkLoop, 6);
    OSMetaClassDeclareReservedUnused(IOWorkLoop, 7);
};

#endif /* !__IOKIT_IOWORKLOOP_H */

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

#include <IOKit/IODataQueue.h>
#include <IOKit/IODataQueueShared.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOMemoryDescriptor.h>

#ifdef enqueue
#undef enqueue
#endif

#ifdef dequeue
#undef dequeue
#endif

#define super OSObject

OSDefineMetaClassAndStructors(IODataQueue, OSObject)

IODataQueue *IODataQueue::withCapacity(UInt32 size)
{
    IODataQueue *dataQueue = new IODataQueue;

    if (dataQueue) {
        if  (!dataQueue->initWithCapacity(size)) {
            dataQueue->release();
            dataQueue = 0;
        }
    }

    return dataQueue;
}

IODataQueue *IODataQueue::withEntries(UInt32 numEntries, UInt32 entrySize)
{
    IODataQueue *dataQueue = new IODataQueue;

    if (dataQueue) {
        if (!dataQueue->initWithEntries(numEntries, entrySize)) {
            dataQueue->release();
            dataQueue = 0;
        }
    }

    return dataQueue;
}

Boolean IODataQueue::initWithCapacity(UInt32 size)
{
    if (!super::init()) {
        return false;
    }

    dataQueue = (IODataQueueMemory *)IOMallocAligned(round_page(size + DATA_QUEUE_MEMORY_HEADER_SIZE), PAGE_SIZE);
    if (dataQueue == 0) {
        return false;
    }

    dataQueue->queueSize = size;
    dataQueue->head = 0;
    dataQueue->tail = 0;

    return true;
}

Boolean IODataQueue::initWithEntries(UInt32 numEntries, UInt32 entrySize)
{
    return (initWithCapacity((numEntries + 1) * (DATA_QUEUE_ENTRY_HEADER_SIZE + entrySize)));
}

void IODataQueue::free()
{
    if (dataQueue) {
        IOFreeAligned(dataQueue, round_page(dataQueue->queueSize + DATA_QUEUE_MEMORY_HEADER_SIZE));
    }

    super::free();

    return;
}

Boolean IODataQueue::enqueue(void * data, UInt32 dataSize)
{
    const UInt32       head      = dataQueue->head;  // volatile
    const UInt32       tail      = dataQueue->tail;
    const UInt32       entrySize = dataSize + DATA_QUEUE_ENTRY_HEADER_SIZE;
    IODataQueueEntry * entry;
    
    if ( tail >= head )
    {
        if ( (tail + entrySize) < dataQueue->queueSize )
        {
            entry = (IODataQueueEntry *)((UInt8 *)dataQueue->queue + tail);

            entry->size = dataSize;
            memcpy(&entry->data, data, dataSize);
            dataQueue->tail += entrySize;
        }
        else if ( head > entrySize )
        {
            // Wrap around to the beginning, but do not allow the tail to catch
            // up to the head.

            dataQueue->queue->size = dataSize;
            ((IODataQueueEntry *)((UInt8 *)dataQueue->queue + tail))->size = dataSize;
            memcpy(&dataQueue->queue->data, data, dataSize);
            dataQueue->tail = entrySize;
        }
        else
        {
            return false;	// queue is full
        }
    }
    else
    {
        // Do not allow the tail to catch up to the head when the queue is full.
        // That's why the comparison uses a '>' rather than '>='.

        if ( (head - tail) > entrySize )
        {
            entry = (IODataQueueEntry *)((UInt8 *)dataQueue->queue + tail);

            entry->size = dataSize;
            memcpy(&entry->data, data, dataSize);
            dataQueue->tail += entrySize;
        }
        else
        {
            return false;	// queue is full
        }
    }

    // Send notification (via mach message) that data is available.

    if ( ( head == tail )                /* queue was empty prior to enqueue() */
    ||   ( dataQueue->head == tail ) )   /* queue was emptied during enqueue() */
    {
        sendDataAvailableNotification();
    }

    return true;
}

void IODataQueue::setNotificationPort(mach_port_t port)
{
    static struct _notifyMsg init_msg = { {
        MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, 0),
        sizeof (struct _notifyMsg),
        MACH_PORT_NULL,
        MACH_PORT_NULL,
        0,
        0
    } };

    if (notifyMsg == 0) {
        notifyMsg = IOMalloc(sizeof(struct _notifyMsg));
    }

    *((struct _notifyMsg *)notifyMsg) = init_msg;

    ((struct _notifyMsg *)notifyMsg)->h.msgh_remote_port = port;
}

void IODataQueue::sendDataAvailableNotification()
{
    kern_return_t		kr;
    mach_msg_header_t *	msgh;

    msgh = (mach_msg_header_t *)notifyMsg;
    if (msgh) {
        kr = mach_msg_send_from_kernel(msgh, msgh->msgh_size);
        switch(kr) {
            case MACH_SEND_TIMED_OUT:	// Notification already sent
            case MACH_MSG_SUCCESS:
                break;
            default:
                IOLog("%s: dataAvailableNotification failed - msg_send returned: %d\n", /*getName()*/"IODataQueue", kr);
                break;
        }
    }
}

IOMemoryDescriptor *IODataQueue::getMemoryDescriptor()
{
    IOMemoryDescriptor *descriptor = 0;

    if (dataQueue != 0) {
        descriptor = IOMemoryDescriptor::withAddress(dataQueue, dataQueue->queueSize + DATA_QUEUE_MEMORY_HEADER_SIZE, kIODirectionOutIn);
    }

    return descriptor;
}


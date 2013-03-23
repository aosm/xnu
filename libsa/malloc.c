/*
 * Copyright (c) 2000 Apple Computer, Inc. All rights reserved.
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
#include <string.h>

#include <kern/queue.h>
#include <kern/kalloc.h>
#include <kern/lock.h>
#include <kern/assert.h> 
#include <vm/vm_kern.h>

#include "libsa/malloc.h"

extern void panic(const char *string, ...);

/*********************************************************************
* Structure for a client memory block. Contains linked-list pointers,
* a size field giving the TOTAL size of the block, including this
* header, and the address of the client's block. The client block
* field is guaranteed to lie on a 16-byte boundary.
*********************************************************************/
typedef struct malloc_block {

	struct malloc_block	*malFwd;
	struct malloc_block	*malBwd;
	unsigned int		malSize;
	unsigned int		malActl;
} malloc_block;

static malloc_block malAnchor = {&malAnchor, &malAnchor, 0, 0};

static int malInited = 0;
static mutex_t *malloc_lock;

__private_extern__
void * malloc(size_t size) {

    unsigned int nsize;
    unsigned int nmem, rmem;
    malloc_block *amem;
 
    assert(malInited);

 	nsize = size + sizeof(malloc_block) + 15;	/* Make sure we get enough to fit */

	nmem = (unsigned int)kalloc(nsize);			/* Get some */
	if(!nmem) {									/* Got any? */
		panic("malloc: no memory for a %08X sized request\n", nsize);
	}
	
	rmem = (nmem + 15) & -16;					/* Round to 16 byte boundary */
	amem = (malloc_block *)rmem;				/* Point to the block */
	amem->malActl = (unsigned int)nmem;			/* Set the actual address */
	amem->malSize = nsize;						/* Size */
	
	mutex_lock(malloc_lock);
	
	amem->malFwd = malAnchor.malFwd;			/* Move anchor to our forward */
	amem->malBwd = &malAnchor;					/* We point back to anchor */
	malAnchor.malFwd->malBwd = amem;			/* The old forward's back points to us */
	malAnchor.malFwd = amem;					/* Now we point the anchor to us */
	
	mutex_unlock(malloc_lock);				/* Unlock now */
	
	return (void *)(rmem + 16);					/* Return the block */

} /* malloc() */


/*********************************************************************
* free()
*
*********************************************************************/
__private_extern__
void free(void * address) {


    malloc_block *amem, *fore, *aft;
    
    if(!(unsigned int)address) return;			/* Leave if they try to free nothing */
    
    
    amem = (malloc_block *)((unsigned int)address - sizeof(malloc_block));	/* Point to the header */

 	mutex_lock(malloc_lock);

	fore = amem->malFwd;						/* Get the guy in front */
	aft  = amem->malBwd;						/* And the guy behind */
	fore->malBwd = aft;							/* The next guy's previous is now my previous */
	aft->malFwd = fore;							/* The previous guy's forward is now mine */	

	mutex_unlock(malloc_lock);				/* Unlock now */
   
 	kfree(amem->malActl, amem->malSize);		/* Toss it */

	return;	

} /* free() */

/*********************************************************************
* malloc_reset()
*
* Allocate the mutual exclusion lock that protect malloc's data.
*********************************************************************/
__private_extern__ void
malloc_init(void)
{
    malloc_lock = mutex_alloc(ETAP_IO_AHA);
    malInited = 1;
}


/*********************************************************************
* malloc_reset()
*
* Walks through the list of VM-allocated regions, destroying them
* all. Any subsequent access by clients to allocated data will cause
* a segmentation fault.
*********************************************************************/
__private_extern__
void malloc_reset(void) {
 
    malloc_block *amem, *bmem;

 	mutex_lock(malloc_lock);
	
	amem = malAnchor.malFwd;					/* Get the first one */
	
	while(amem != &malAnchor) {					/* Go until we hit the anchor */
	
		bmem = amem->malFwd;					/* Next one */
 		kfree(amem->malActl, amem->malSize);	/* Toss it */
 		amem = bmem;							/* Skip to it */
	
	} 

	malAnchor.malFwd = (struct malloc_block	*) 0x666;	/* Cause a fault if we try again */
	malAnchor.malBwd = (struct malloc_block	*) 0x666;	/* Cause a fault if we try again */
	
	mutex_unlock(malloc_lock);				/* Unlock now */

	mutex_free(malloc_lock);
    return;

} /* malloc_reset() */


/*********************************************************************
* realloc()
*
* This function simply allocates a new block and copies the existing
* data into it. Nothing too clever here, as cleanup and efficient
* memory usage are not important in this allocator package.
*********************************************************************/
__private_extern__
void * realloc(void * address, size_t new_client_size) {
    void * new_address;
    malloc_block *amem;

	amem = (malloc_block *)((unsigned int)address - sizeof(malloc_block));	/* Point to allocation block */
	
	new_address = malloc(new_client_size);		/* get a new one */
	if(!new_address) {							/* Did we get it? */
		panic("realloc: can not reallocate one of %08X size\n", new_client_size);
	}
	
    memcpy(new_address, address, amem->malSize - sizeof(malloc_block));	/* Copy the old in */
    
    free(address);								/* Toss the old one */
	
    return new_address;

} /* realloc() */



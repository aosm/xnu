/*
 * Copyright (c)1999-2004 Apple Computer, Inc. All rights reserved.
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

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/proc.h>
#include <sys/errno.h>
#include <sys/ioctl.h>
#include <sys/conf.h>
#include <sys/fcntl.h>
#include <string.h>
#include <miscfs/devfs/devfs.h>
#include <kern/lock.h>
#include <sys/time.h>
#include <sys/malloc.h>
#include <sys/uio_internal.h>

#include <dev/random/randomdev.h>
#include <dev/random/YarrowCoreLib/include/yarrow.h>
#include <crypto/sha1.h>

#define RANDOM_MAJOR  -1 /* let the kernel pick the device number */

d_ioctl_t       random_ioctl;

/*
 * A struct describing which functions will get invoked for certain
 * actions.
 */
static struct cdevsw random_cdevsw =
{
	random_open,		/* open */
	random_close,		/* close */
	random_read,		/* read */
	random_write,		/* write */
	random_ioctl,		/* ioctl */
	(stop_fcn_t *)nulldev, /* stop */
	(reset_fcn_t *)nulldev, /* reset */
	NULL,				/* tty's */
	eno_select,			/* select */
	eno_mmap,			/* mmap */
	eno_strat,			/* strategy */
	eno_getc,			/* getc */
	eno_putc,			/* putc */
	0					/* type */
};

/* Used to detect whether we've already been initialized */
static int gRandomInstalled = 0;
static PrngRef gPrngRef;
static int gRandomError = 1;
static mutex_t *gYarrowMutex = 0;

#define RESEED_TICKS 50 /* how long a reseed operation can take */


enum {kBSizeInBits = 160}; // MUST be a multiple of 32!!!
enum {kBSizeInBytes = kBSizeInBits / 8};
typedef u_int32_t BlockWord;
enum {kWordSizeInBits = 32};
enum {kBSize = 5};
typedef BlockWord Block[kBSize];

/* define prototypes to keep the compiler happy... */

void add_blocks(Block a, Block b, BlockWord carry);
void fips_initialize(void);
void random_block(Block b);

/*
 * Get 120 bits from yarrow
 */

/*
 * add block b to block a
 */
void
add_blocks(Block a, Block b, BlockWord carry)
{
	int i = kBSize;
	while (--i >= 0)
	{
		u_int64_t c = (u_int64_t)carry +
					  (u_int64_t)a[i] +
					  (u_int64_t)b[i];
		a[i] = c & ((1LL << kWordSizeInBits) - 1);
		carry = c >> kWordSizeInBits;
	}
}



struct sha1_ctxt g_sha1_ctx;
char zeros[(512 - kBSizeInBits) / 8];
Block g_xkey;
Block g_random_data;
int g_bytes_used;

/*
 * Setup for fips compliance
 */

/*
 * get a random block of data per fips 186-2
 */
void
random_block(Block b)
{
	// do one iteration
	Block xSeed;
	prngOutput (gPrngRef, (BYTE*) &xSeed, sizeof (xSeed));
	
	// add the seed to the previous value of g_xkey
	add_blocks (g_xkey, xSeed, 0);

	// compute "G"
	SHA1Update (&g_sha1_ctx, (const u_int8_t *) &g_xkey, sizeof (g_xkey));
	
	// add zeros to fill the internal SHA-1 buffer
	SHA1Update (&g_sha1_ctx, (const u_int8_t *)zeros, sizeof (zeros));
	
	// write the resulting block
	memmove(b, g_sha1_ctx.h.b8, sizeof (Block));
	
	// fix up the next value of g_xkey
	add_blocks (g_xkey, b, 1);
}

/*
 *Initialize ONLY the Yarrow generator.
 */
void
PreliminarySetup(void)
{
    prng_error_status perr;
    struct timeval tt;
    char buffer [16];

    /* create a Yarrow object */
    perr = prngInitialize(&gPrngRef);
    if (perr != 0) {
        printf ("Couldn't initialize Yarrow, /dev/random will not work.\n");
        return;
    }

	/* clear the error flag, reads and write should then work */
    gRandomError = 0;

    /* get a little non-deterministic data as an initial seed. */
    microtime(&tt);

    /*
	 * So how much of the system clock is entropic?
	 * It's hard to say, but assume that at least the
	 * least significant byte of a 64 bit structure
	 * is entropic.  It's probably more, how can you figure
	 * the exact time the user turned the computer on, for example.
    */
    perr = prngInput(gPrngRef, (BYTE*) &tt, sizeof (tt), SYSTEM_SOURCE, 8);
    if (perr != 0) {
        /* an error, complain */
        printf ("Couldn't seed Yarrow.\n");
        return;
    }
    
    /* turn the data around */
    perr = prngOutput(gPrngRef, (BYTE*)buffer, sizeof (buffer));
    
    /* and scramble it some more */
    perr = prngForceReseed(gPrngRef, RESEED_TICKS);
    
    /* make a mutex to control access */
    gYarrowMutex = mutex_alloc(0);
	
	fips_initialize ();
}

void
fips_initialize(void)
{
	/* Read the initial value of g_xkey from yarrow */
	prngOutput (gPrngRef, (BYTE*) &g_xkey, sizeof (g_xkey));
	
	/* initialize our SHA1 generator */
	SHA1Init (&g_sha1_ctx);
	
	/* other initializations */
	memset (zeros, 0, sizeof (zeros));
	g_bytes_used = 0;
	random_block(g_random_data);
}

/*
 * Called to initialize our device,
 * and to register ourselves with devfs
 */
void
random_init(void)
{
	int ret;

	if (gRandomInstalled)
		return;

	/* install us in the file system */
	gRandomInstalled = 1;

	/* setup yarrow and the mutex */
	PreliminarySetup();

	ret = cdevsw_add(RANDOM_MAJOR, &random_cdevsw);
	if (ret < 0) {
		printf("random_init: failed to allocate a major number!\n");
		gRandomInstalled = 0;
		return;
	}

	devfs_make_node(makedev (ret, 0), DEVFS_CHAR,
		UID_ROOT, GID_WHEEL, 0666, "random", 0);

	/*
	 * also make urandom 
	 * (which is exactly the same thing in our context)
	 */
	devfs_make_node(makedev (ret, 1), DEVFS_CHAR,
		UID_ROOT, GID_WHEEL, 0666, "urandom", 0);
}

int
random_ioctl(	__unused dev_t dev, u_long cmd, __unused caddr_t data, 
				__unused int flag, __unused struct proc *p  )
{
	switch (cmd) {
	case FIONBIO:
	case FIOASYNC:
		break;
	default:
		return ENODEV;
	}

	return (0);
}

/*
 * Open the device.  Make sure init happened, and make sure the caller is
 * authorized.
 */
 
int
random_open(__unused dev_t dev, int flags, __unused int devtype, __unused struct proc *p)
{
	if (gRandomError != 0) {
		/* forget it, yarrow didn't come up */
		return (ENOTSUP);
	}

	/*
	 * if we are being opened for write,
	 * make sure that we have privledges do so
	 */
	if (flags & FWRITE) {
		if (securelevel >= 2)
			return (EPERM);
#ifndef __APPLE__
		if ((securelevel >= 1) && proc_suser(p))
			return (EPERM);
#endif	/* !__APPLE__ */
	}

	return (0);
}


/*
 * close the device.
 */
 
int
random_close(__unused dev_t dev, __unused int flags, __unused int mode, __unused struct proc *p)
{
	return (0);
}


/*
 * Get entropic data from the Security Server, and use it to reseed the
 * prng.
 */
int
random_write (__unused dev_t dev, struct uio *uio, __unused int ioflag)
{
    int retCode = 0;
    char rdBuffer[256];

    if (gRandomError != 0) {
        return (ENOTSUP);
    }
    
    /* get control of the Yarrow instance, Yarrow is NOT thread safe */
    mutex_lock(gYarrowMutex);
    
    /* Security server is sending us entropy */

    while (uio_resid(uio) > 0 && retCode == 0) {
        /* get the user's data */
        // LP64todo - fix this!  uio_resid may be 64-bit value
        int bytesToInput = min(uio_resid(uio), sizeof (rdBuffer));
        retCode = uiomove(rdBuffer, bytesToInput, uio);
        if (retCode != 0)
            goto /*ugh*/ error_exit;
        
        /* put it in Yarrow */
        if (prngInput(gPrngRef, (BYTE*)rdBuffer,
			bytesToInput, SYSTEM_SOURCE,
        	bytesToInput * 8) != 0) {
            retCode = EIO;
            goto error_exit;
        }
    }
    
    /* force a reseed */
    if (prngForceReseed(gPrngRef, RESEED_TICKS) != 0) {
        retCode = EIO;
        goto error_exit;
    }
    
    /* retCode should be 0 at this point */
    
error_exit: /* do this to make sure the mutex unlocks. */
    mutex_unlock(gYarrowMutex);
    return (retCode);
}

/*
 * return data to the caller.  Results unpredictable.
 */ 
int random_read(__unused dev_t dev, struct uio *uio, __unused int ioflag)
{
    int retCode = 0;
	
    if (gRandomError != 0)
        return (ENOTSUP);

   /* lock down the mutex */
    mutex_lock(gYarrowMutex);

	int bytes_remaining = uio_resid(uio);
    while (bytes_remaining > 0 && retCode == 0) {
        /* get the user's data */
		int bytes_to_read = 0;
		
		int bytes_available = kBSizeInBytes - g_bytes_used;
        if (bytes_available == 0)
		{
			random_block(g_random_data);
			g_bytes_used = 0;
			bytes_available = kBSizeInBytes;
		}
		
		bytes_to_read = min (bytes_remaining, bytes_available);
		
        retCode = uiomove(((u_int8_t*)g_random_data)+ g_bytes_used, bytes_to_read, uio);
        g_bytes_used += bytes_to_read;

        if (retCode != 0)
            goto error_exit;
		
		bytes_remaining = uio_resid(uio);
    }
    
    retCode = 0;
    
error_exit:
    mutex_unlock(gYarrowMutex);
    return retCode;
}

/* export good random numbers to the rest of the kernel */
void
read_random(void* buffer, u_int numbytes)
{
    if (gYarrowMutex == 0) { /* are we initialized? */
        PreliminarySetup ();
    }
    
    mutex_lock(gYarrowMutex);

	int bytes_remaining = numbytes;
    while (bytes_remaining > 0) {
        int bytes_to_read = min(bytes_remaining, kBSizeInBytes - g_bytes_used);
        if (bytes_to_read == 0)
		{
			random_block(g_random_data);
			g_bytes_used = 0;
			bytes_to_read = min(bytes_remaining, kBSizeInBytes);
		}
		
		memmove (buffer, ((u_int8_t*)g_random_data)+ g_bytes_used, bytes_to_read);
		g_bytes_used += bytes_to_read;
		
		bytes_remaining -= bytes_to_read;
    }

    mutex_unlock(gYarrowMutex);
}

/*
 * Return an unsigned long pseudo-random number.
 */
u_long
RandomULong(void)
{
	u_long buf;
	read_random(&buf, sizeof (buf));
	return (buf);
}


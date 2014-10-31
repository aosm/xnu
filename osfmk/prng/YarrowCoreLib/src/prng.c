/*
 * Copyright (c) 1999, 2000-2013 Apple Inc. All rights reserved.
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
	File:		prng.c

	Contains:	Core routines for the Counterpane Yarrow PRNG.

	Written by:	Counterpane, Inc. 

	Copyright: (c) 2000 by Apple Computer, Inc., all rights reserved.

	Change History (most recent first):

		02/10/99	dpm		Created, based on Counterpane source.
 
*/
/*
	prng.c

	Core routines for the Counterpane PRNG
*/
#include "userdefines.h"
#include "assertverify.h"
#include "prng/YarrowCoreLib/include/yarrowUtils.h"

#if		defined(macintosh) || defined(__APPLE__)
/* FIXME - this file needs to be in a platform-independent place */

#include "macOnly.h"
#endif	/* macintosh */
#include "smf.h"
#include "sha1mod.h"
#include "entropysources.h"
#include "comp.h"
#include "prng/YarrowCoreLib/include/yarrow.h"
#include "prng.h"
#include "prngpriv.h"


#define _MAX(a,b) (((a)>(b))?(a):(b))
#define _MIN(a,b) (((a)<(b))?(a):(b))

#if		defined(macintosh) || defined(__APPLE__)
/*
 * No mutexes in this module for Macintosh/OSX. We handle the
 * required locking elsewhere. 
 */
#define MUTEX_ENABLE	0

#include <string.h>		/* memcpy, etc. */
#if		TARGET_API_MAC_OSX
	#include <sys/time.h>		/* for timespec */
#elif	TARGET_API_MAC_CARBON
	#include <Timer.h>				/* Microseconds */
	#include <Math64.h>
#elif	KERNEL_BUILD
	#include <sys/time.h>
#elif	MACH_KERNEL_PRIVATE
	#include <mach/mach_time.h>
	#include <mach/clock_types.h>
#else
	#error Unknown TARGET_API
#endif	/* TARGET_API */
#else
#define MUTEX_ENABLE	1
#endif	/* macintosh */

#if		MUTEX_ENABLE
static HANDLE Statmutex = NULL;
static DWORD mutexCreatorId = 0;
#endif

#if 0
#pragma mark -
#pragma mark * * * Static Utility functions * * * 
#endif

/* All error checking should be done in the function that calls these */

/*
 * out := SHA1(IV | out) 
 */
static void 
prng_do_SHA1(GEN_CTX *ctx) 
{
	YSHA1_CTX sha;

	YSHA1Init(&sha);
	YSHA1Update(&sha,ctx->IV,20);
	YSHA1Update(&sha,ctx->out,20);
	YSHA1Final(ctx->out,&sha);
	ctx->index = 0;
}

/*
 * IV  := newState
 * out := SHA1(IV)
 *
 * Called from init, prngForceReseed(), and prngOutput()
 * as anti-backtracking mechanism.
 */
static void 
prng_make_new_state(GEN_CTX *ctx,BYTE *newState) 
{
	YSHA1_CTX sha;

	memcpy(ctx->IV,newState,20);
	YSHA1Init(&sha);
	YSHA1Update(&sha,ctx->IV,20);
	YSHA1Final(ctx->out,&sha);
	ctx->numout = 0;
	ctx->index = 0;
}

#if		SLOW_POLL_ENABLE


/* Initialize the secret state with a slow poll */
/* Currently only called from prngInitialize */

#define SPLEN 65536  /* 64K */

static void 
prng_slow_init(PRNG *p)
/* This fails silently and must be fixed. */
{
	YSHA1_CTX* ctx = NULL;
	MMPTR mmctx = MM_NULL;
	BYTE* bigbuf = NULL;
	MMPTR mmbigbuf = MM_NULL;
	BYTE* buf = NULL;
	MMPTR mmbuf = MM_NULL;
	DWORD polllength;

	mmbigbuf = mmMalloc(SPLEN);
	if(mmbigbuf == MM_NULL) {goto cleanup_slow_init;}
	bigbuf = (BYTE*)mmGetPtr(mmbigbuf);

	mmbuf = mmMalloc(20);
	if(mmbuf == MM_NULL) {goto cleanup_slow_init;}
	buf = (BYTE*)mmGetPtr(mmbuf);

	mmctx = mmMalloc(sizeof(YSHA1_CTX));
	if(mmctx == MM_NULL) {goto cleanup_slow_init;}
	ctx = (YSHA1_CTX*)mmGetPtr(mmctx);


	/* Initialize the secret state. */
	/* Init entropy pool */
	YSHA1Init(&p->pool);
	/* Init output generator */
	polllength = prng_slow_poll(bigbuf,SPLEN);
	YSHA1Init(ctx);
	YSHA1Update(ctx,bigbuf,polllength);
	YSHA1Final(buf,ctx);
	prng_make_new_state(&p->outstate, buf);

cleanup_slow_init:
	mmFree(mmctx);
	mmFree(mmbigbuf);
	mmFree(mmbuf);

	return;
}

#endif	/* SLOW_POLL_ENABLE */

/* In-place modifed bubble sort */
static void 
bubbleSort( UINT *data, LONG len ) 
{
	LONG 	i,last,newlast;
	UINT	temp;

	last = len-1; 
	while(last!=-1) 
	{
		newlast = -1;
		for(i=0;i<last;i++) 
		{
			if(data[i+1] > data[i]) 
			{
				newlast = i;
				temp = data[i];
				data[i] = data[i+1];
				data[i+1] = temp;
			}
		}
		last = newlast;
	}		
}

#if 0
#pragma mark -
#pragma mark * * * Public functions * * * 
#endif

/* Set up the PRNG */
prng_error_status
prngInitialize(PrngRef *prng) 
{
	UINT i;
	comp_error_status resp;
	prng_error_status retval = PRNG_ERR_LOW_MEMORY;
	MMPTR	mmp;
	PRNG	*p;
	
	mmInit();
	
	#if	MUTEX_ENABLE
	/* Create the mutex */
	/* NOTE: on return the mutex should bve held, since our caller (prngInitialize)
	 * will release it. 
	 */
	if(mutexCreatorId!=0) {return PRNG_ERR_REINIT;}
	Statmutex = CreateMutex(NULL,TRUE,NULL);
	if(Statmutex == NULL) {mutexCreatorId = 0; return PRNG_ERR_MUTEX;}
	DuplicateHandle(GetCurrentProcess(),Statmutex,GetCurrentProcess(),&mutex,SYNCHRONIZE,FALSE,0);
	mutexCreatorId = GetCurrentProcessId();
	#endif	/* MUTEX_ENABLE */
	
	/* Assign memory */
	mmp = mmMalloc(sizeof(PRNG));
	if(mmp==MM_NULL)
	{
		goto cleanup_init;
	}
	else
	{
		p = (PRNG*)mmGetPtr(mmp);
		memset(p, 0, sizeof(PRNG));
	}

	/* Initialize Variables */
	for(i=0;i<TOTAL_SOURCES;i++) 
	{
		p->poolSize[i] = 0;
		p->poolEstBits[i] = 0;
	}

#ifdef WIN_NT
	/* Setup security on the registry so that remote users cannot predict the slow pool */
	prng_set_NT_security();
#endif

	/* Initialize the secret state. */
	/* FIXME - might want to make this an option here and have the caller
	 * do it after we return....? */
	YSHA1Init(&p->pool);
#if		SLOW_POLL_ENABLE
	prng_slow_init(p);	/* Does a slow poll and then calls prng_make_state(...) */
#else	
	/* NULL init */
	prng_do_SHA1(&p->outstate);
	prng_make_new_state(&p->outstate, p->outstate.out);
#endif	/* SLOW_POLL_ENABLE */

	/* Initialize compression routines */
	for(i=0;i<COMP_SOURCES;i++) 
	{
		resp = comp_init((p->comp_state)+i);
		if(resp!=COMP_SUCCESS) {retval = PRNG_ERR_COMPRESSION; goto cleanup_init;}
	}
	
	p->ready = PRNG_READY;
	*prng = (PrngRef)p;
	
	return PRNG_SUCCESS;

cleanup_init:
	/* Program failed on one of the mmmallocs */
	mmFree(mmp);
	mmp = MM_NULL;
	
	#if		MUTEX_ENABLE
	CloseHandle(Statmutex);
	Statmutex = NULL;
	mutexCreatorId = 0;
	#endif
	
	return retval; /* default PRNG_ERR_LOW_MEMORY */
}

/* Provide output */
prng_error_status
prngOutput(PRNG *p, BYTE *outbuf,UINT outbuflen) 
{
	UINT i;
	GEN_CTX	*ctx = &p->outstate;
	
	CHECKSTATE(p);
	GENCHECK(p);
	PCHECK(outbuf);
	chASSERT(BACKTRACKLIMIT > 0);

	for(i=0;i<outbuflen;i++,ctx->index++,ctx->numout++) 
	{
		/* Check backtracklimit */
		if(ctx->numout > BACKTRACKLIMIT) 
		{
			prng_do_SHA1(ctx);	
			prng_make_new_state(ctx, ctx->out);
		}
		/* Check position in IV */
		if(ctx->index>=20) 
		{
			prng_do_SHA1(ctx);
		}
		/* Output data */
		outbuf[i] = (ctx->out)[ctx->index];
	}

	return PRNG_SUCCESS;
}


/* Cause the PRNG to reseed now regardless of entropy pool */ 
/* Should this be public? */
prng_error_status
prngForceReseed(PRNG *p, LONGLONG ticks) 
{
	int i;
#ifdef WIN_NT
	FILETIME a,b,c,usertime;
#endif
	BYTE buf[64];
	BYTE dig[20];
#if	defined(macintosh) || defined(__APPLE__)
	#if		(defined(TARGET_API_MAC_OSX) || defined(KERNEL_BUILD))
		struct timeval 	tv;		
		int64_t			endTime, curTime;
	#elif		defined(MACH_KERNEL_PRIVATE)
		int64_t			endTime, curTime;
	#else	/* TARGET_API_MAC_CARBON */
		UnsignedWide 	uwide;		/* struct needed for Microseconds() */
		LONGLONG 		start;
		LONGLONG 		now;
	#endif
#endif

	CHECKSTATE(p);
	POOLCHECK(p);
	ZCHECK(ticks);
	
	/* Set up start and end times */
	#if		defined(macintosh) || defined(__APPLE__)
		#if		(defined(TARGET_API_MAC_OSX) || defined(KERNEL_BUILD))
			/* note we can't loop for more than a million microseconds */
            #ifdef KERNEL_BUILD
                microuptime (&tv);
            #else
                gettimeofday(&tv, NULL);
            #endif
			endTime = (int64_t)tv.tv_sec*1000000LL + (int64_t)tv.tv_usec + ticks;
		#elif		defined(MACH_KERNEL_PRIVATE)
			endTime = mach_absolute_time() + (ticks*NSEC_PER_USEC);
		#else	/* TARGET_API_MAC_OSX */
			Microseconds(&uwide);
			start = UnsignedWideToUInt64(uwide);
		#endif	/* TARGET_API_xxx */
	#endif	/* macintosh */
	do
	{
		/* Do a couple of iterations between time checks */
		prngOutput(p, buf,64);
		YSHA1Update(&p->pool,buf,64);
		prngOutput(p, buf,64);
		YSHA1Update(&p->pool,buf,64);
		prngOutput(p, buf,64);
		YSHA1Update(&p->pool,buf,64);
		prngOutput(p, buf,64);
		YSHA1Update(&p->pool,buf,64);
		prngOutput(p, buf,64);
		YSHA1Update(&p->pool,buf,64);

#if		defined(macintosh) || defined(__APPLE__)
	#if		defined(TARGET_API_MAC_OSX) || defined(KERNEL_BUILD)
        #ifdef TARGET_API_MAC_OSX
            gettimeofday(&tv, NULL);
        #else
            microuptime (&tv);
	    curTime = (int64_t)tv.tv_sec*1000000LL + (int64_t)tv.tv_usec;
        #endif
	} while(curTime < endTime);
	#elif		defined(MACH_KERNEL_PRIVATE)
	    curTime = mach_absolute_time();	
	} while(curTime < endTime);
	#else
		Microseconds(&uwide);
		now = UnsignedWideToUInt64(uwide);
	} while ( (now-start) < ticks) ;
	#endif
#else
	} while ( (now-start) < ticks) ;
#endif
	YSHA1Final(dig,&p->pool);
	YSHA1Update(&p->pool,dig,20); 
	YSHA1Final(dig,&p->pool);

	/* Reset secret state */
	YSHA1Init(&p->pool);
	prng_make_new_state(&p->outstate,dig);

	/* Clear counter variables */
	for(i=0;i<TOTAL_SOURCES;i++) 
	{
		p->poolSize[i] = 0;
		p->poolEstBits[i] = 0;
	}

	/* Cleanup memory */
	trashMemory(dig,20*sizeof(char));
	trashMemory(buf,64*sizeof(char));

	return PRNG_SUCCESS;
}


/* Input a state into the PRNG */
prng_error_status
prngProcessSeedBuffer(PRNG *p, BYTE *buf,LONGLONG ticks) 
{
	CHECKSTATE(p);
	GENCHECK(p);
	PCHECK(buf);

	/* Put the data into the entropy, add some data from the unknown state, reseed */
	YSHA1Update(&p->pool,buf,20);			/* Put it into the entropy pool */
	prng_do_SHA1(&p->outstate);				/* Output 20 more bytes and     */
	YSHA1Update(&p->pool,p->outstate.out,20);/* add it to the pool as well.  */
	prngForceReseed(p, ticks); 				/* Do a reseed */
	return prngOutput(p, buf,20); /* Return the first 20 bytes of output in buf */
}


/* Take some "random" data and make more "random-looking" data from it */
/* note: this routine has no context, no mutex wrapper */
prng_error_status
prngStretch(BYTE *inbuf,UINT inbuflen,BYTE *outbuf,UINT outbuflen) {
	long int left,prev;
	YSHA1_CTX ctx;
	BYTE dig[20];

	PCHECK(inbuf);
	PCHECK(outbuf);

	if(inbuflen >= outbuflen) 
	{
		memcpy(outbuf,inbuf,outbuflen);
		return PRNG_SUCCESS;
	}
	else  /* Extend using SHA1 hash of inbuf */
	{
		YSHA1Init(&ctx);
		YSHA1Update(&ctx,inbuf,inbuflen);
		YSHA1Final(dig,&ctx);
		for(prev=0,left=outbuflen;left>0;prev+=20,left-=20) 
		{
			YSHA1Update(&ctx,dig,20);
			YSHA1Final(dig,&ctx);
			memcpy(outbuf+prev,dig,(left>20)?20:left);
		}
		trashMemory(dig,20*sizeof(BYTE));
		
		return PRNG_SUCCESS;
	}

	return PRNG_ERR_PROGRAM_FLOW;
}


/* Add entropy to the PRNG from a source */
prng_error_status
prngInput(PRNG *p, BYTE *inbuf,UINT inbuflen,UINT poolnum, __unused UINT estbits)
{
	#ifndef	YARROW_KERNEL
	comp_error_status resp;
	#endif
	
	CHECKSTATE(p);
	POOLCHECK(p);
	PCHECK(inbuf);
	if(poolnum >= TOTAL_SOURCES) {return PRNG_ERR_OUT_OF_BOUNDS;}

	/* Add to entropy pool */
	YSHA1Update(&p->pool,inbuf,inbuflen);
	
	#ifndef	YARROW_KERNEL
	/* skip this step for the kernel */
	
	/* Update pool size, pool user estimate and pool compression context */
	p->poolSize[poolnum] += inbuflen;
	p->poolEstBits[poolnum] += estbits;
	if(poolnum<COMP_SOURCES)
	{
		resp = comp_add_data((p->comp_state)+poolnum,inbuf,inbuflen);
		if(resp!=COMP_SUCCESS) {return PRNG_ERR_COMPRESSION;}
	}
	#endif	/* YARROW_KERNEL */
	
	return PRNG_SUCCESS;
}



/* If we have enough entropy, allow a reseed of the system */
prng_error_status
prngAllowReseed(PRNG *p, LONGLONG ticks) 
{
	UINT temp[TOTAL_SOURCES];
	LONG i;
	UINT sum;
#ifndef KERNEL_BUILD
	float ratio;
#endif

#ifndef KERNEL_BUILD
	comp_error_status resp;
#endif

	CHECKSTATE(p);

	for(i=0;i<ENTROPY_SOURCES;i++)
	{
		/* Make sure that compression-based entropy estimates are current */
#ifndef KERNEL_BUILD // floating point in a kernel is BAD!
		resp = comp_get_ratio((p->comp_state)+i,&ratio);
		if(resp!=COMP_SUCCESS) {return PRNG_ERR_COMPRESSION;}
		/* Use 4 instead of 8 to half compression estimate */
		temp[i] = (int)(ratio*p->poolSize[i]*4);
#else
        temp[i] = p->poolSize[i] * 4;
#endif

	}
	/* Use minumum of user and compression estimate for compressed sources */
	for(i=ENTROPY_SOURCES;i<COMP_SOURCES;i++)
	{
#ifndef KERNEL_BUILD
		/* Make sure that compression-based entropy estimates are current */
		resp = comp_get_ratio((p->comp_state)+i,&ratio);
		if(resp!=COMP_SUCCESS) {return PRNG_ERR_COMPRESSION;}
		/* Use 4 instead of 8 to half compression estimate */
		temp[i] = _MIN((int)(ratio*p->poolSize[i]*4),(int)p->poolEstBits[i]);
#else
        temp[i] = _MIN (p->poolSize[i] * 4, p->poolEstBits[i]);
#endif

	}
	/* Use user estimate for remaining sources */
	for(i=COMP_SOURCES;i<TOTAL_SOURCES;i++) {temp[i] = p->poolEstBits[i];}

	if(K > 0) {
		/* pointless if we're not ignoring any sources */
		bubbleSort(temp,TOTAL_SOURCES);
	}
	for(i=K,sum=0;i<TOTAL_SOURCES;sum+=temp[i++]); /* Stupid C trick */
	if(sum>THRESHOLD) 
		return prngForceReseed(p, ticks);
	else 
		return PRNG_ERR_NOT_ENOUGH_ENTROPY;

	return PRNG_ERR_PROGRAM_FLOW;
}

#if		SLOW_POLL_ENABLE
/* Call a slow poll and insert the data into the entropy pool */
static prng_error_status
prngSlowPoll(PRNG *p, UINT pollsize)
{
	BYTE *buf;
	DWORD len;
	prng_error_status retval;

	CHECKSTATE(p);

	buf = (BYTE*)malloc(pollsize);
	if(buf==NULL) {return PRNG_ERR_LOW_MEMORY;}
	len = prng_slow_poll(buf,pollsize);	/* OS specific call */
	retval = prngInput(p, buf,len,SLOWPOLLSOURCE, len * 8);
	trashMemory(buf,pollsize);
	free(buf);

	return retval;
}
#endif	/* SLOW_POLL_ENABLE */


/* Delete the PRNG */
prng_error_status
prngDestroy(PRNG *p) 
{
	UINT i;

	#if	MUTEX_ENABLE
	if(GetCurrentProcessId()!=mutexCreatorId) {return PRNG_ERR_WRONG_CALLER;}
	#endif
	if(p==NULL) {return PRNG_SUCCESS;} /* Well, there is nothing to destroy... */

	p->ready = PRNG_NOT_READY;
	
	for(i=0;i<COMP_SOURCES;i++)
	{
		comp_end((p->comp_state)+i);
	}

	#if	MUTEX_ENABLE
	CloseHandle(Statmutex);
	Statmutex = NULL;
	mutexCreatorId = 0;
	#endif
	
	return PRNG_SUCCESS;
}



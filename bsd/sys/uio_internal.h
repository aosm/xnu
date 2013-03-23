/*
 * Copyright (c) 2000-2004 Apple Computer, Inc. All rights reserved.
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
/* Copyright (c) 1995 NeXT Computer, Inc. All Rights Reserved */
/*
 * Copyright (c) 1982, 1986, 1993, 1994
 *	The Regents of the University of California.  All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. All advertising materials mentioning features or use of this software
 *    must display the following acknowledgement:
 *	This product includes software developed by the University of
 *	California, Berkeley and its contributors.
 * 4. Neither the name of the University nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE REGENTS AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 *	@(#)uio.h	8.5 (Berkeley) 2/22/94
 */

#ifndef _SYS_UIO_INTERNAL_H_
#define	_SYS_UIO_INTERNAL_H_

#include <sys/appleapiopts.h>

#ifdef KERNEL_PRIVATE
#include <sys/uio.h>
#include <sys/malloc.h>

/*
 * user / kernel address space type flags.
 * WARNING - make sure to check when adding flags!  Be sure new flags
 * don't overlap the definitions in uio.h
 */
//	UIO_USERSPACE 				0	defined in uio.h
#define	UIO_USERISPACE			1
//	UIO_SYSSPACE				2	defined in uio.h
#define UIO_PHYS_USERSPACE		3
#define UIO_PHYS_SYSSPACE		4	
//	UIO_USERSPACE32				5	defined in uio.h
#define UIO_USERISPACE32		6
#define UIO_PHYS_USERSPACE32	7	
//	UIO_USERSPACE64				8	defined in uio.h
#define UIO_USERISPACE64		9	
#define UIO_PHYS_USERSPACE64	10
//	UIO_SYSSPACE32				11	defined in uio.h
#define UIO_PHYS_SYSSPACE32		12	
#define UIO_SYSSPACE64			13	
#define UIO_PHYS_SYSSPACE64		14	

__BEGIN_DECLS
struct user_iovec;

// uio_iovsaddr was __private_extern__ temporary chnage for 3777436
struct user_iovec * uio_iovsaddr( uio_t a_uio );
__private_extern__ void uio_calculateresid( uio_t a_uio );
__private_extern__ void uio_setcurriovlen( uio_t a_uio, user_size_t a_value );
// uio_spacetype was __private_extern__ temporary chnage for 3777436
int uio_spacetype( uio_t a_uio );
__private_extern__ uio_t 
	uio_createwithbuffer( int a_iovcount, off_t a_offset, int a_spacetype,
							int a_iodirection, void *a_buf_p, int a_buffer_size );

/* use kern_iovec for system space requests */
struct kern_iovec {
	u_int32_t	iov_base;	/* Base address. */
	u_int32_t	iov_len;	/* Length. */
};
	
/* use user_iovec for user space requests */
struct user_iovec {
	user_addr_t	iov_base;	/* Base address. */
	user_size_t	iov_len;	/* Length. */
};

#if 1 // LP64todo - remove this after kext adopt new KPI
#define uio_iov uio_iovs.iovp
#define iovec_32 kern_iovec
#define iovec_64 user_iovec
#define iov32p kiovp
#define iov64p uiovp
#endif

union iovecs {
	struct iovec		*iovp;
	struct kern_iovec	*kiovp;
	struct user_iovec 	*uiovp;
};

/* WARNING - use accessor calls for uio_iov and uio_resid since these */
/* fields vary depending on the originating address space. */
struct uio {
	union iovecs 	uio_iovs;		/* current iovec */
	int				uio_iovcnt;		/* active iovecs */
	off_t			uio_offset;
	int				uio_resid;		/* compatibility uio_resid (pre-LP64) */
	enum uio_seg 	uio_segflg;
	enum uio_rw 	uio_rw;
	proc_t		 	uio_procp;		/* obsolete - not used! */
	user_ssize_t	uio_resid_64;
	int				uio_size;		/* size for use with kfree */
	int				uio_max_iovs;	/* max number of iovecs this uio_t can hold */
	u_int32_t		uio_flags;		
};

/* values for uio_flags */
#define UIO_FLAGS_INITED 		0x00000001
#define UIO_FLAGS_WE_ALLOCED 	0x00000002

__END_DECLS

/*
 * UIO_SIZEOF - return the amount of space a uio_t requires to
 *	contain the given number of iovecs.  Use this macro to
 *  create a stack buffer that can be passed to uio_createwithbuffer.
 */
#define UIO_SIZEOF( a_iovcount ) \
	( sizeof(struct uio) + (sizeof(struct user_iovec) * (a_iovcount)) )
	
#define UIO_IS_64_BIT_SPACE( a_uio_t )  \
	( (a_uio_t)->uio_segflg == UIO_USERSPACE64 || (a_uio_t)->uio_segflg == UIO_USERISPACE64 || \
	  (a_uio_t)->uio_segflg == UIO_PHYS_USERSPACE64  || (a_uio_t)->uio_segflg == UIO_SYSSPACE64 || \
	  (a_uio_t)->uio_segflg == UIO_PHYS_SYSSPACE64 )

#define UIO_IS_32_BIT_SPACE( a_uio_t )  \
	( (a_uio_t)->uio_segflg == UIO_USERSPACE || (a_uio_t)->uio_segflg == UIO_USERISPACE || \
	  (a_uio_t)->uio_segflg == UIO_SYSSPACE  || (a_uio_t)->uio_segflg == UIO_PHYS_USERSPACE || \
	  (a_uio_t)->uio_segflg == UIO_USERISPACE32  || (a_uio_t)->uio_segflg == UIO_PHYS_USERSPACE32 || \
	  (a_uio_t)->uio_segflg == UIO_SYSSPACE32  || (a_uio_t)->uio_segflg == UIO_PHYS_SYSSPACE32 || \
	  (a_uio_t)->uio_segflg == UIO_PHYS_SYSSPACE || (a_uio_t)->uio_segflg == UIO_USERSPACE32 )

#define UIO_IS_USER_SPACE32( a_uio_t )  \
	( (a_uio_t)->uio_segflg == UIO_USERSPACE32 || (a_uio_t)->uio_segflg == UIO_PHYS_USERSPACE32 || \
	  (a_uio_t)->uio_segflg == UIO_USERISPACE32 )
#define UIO_IS_USER_SPACE64( a_uio_t )  \
	( (a_uio_t)->uio_segflg == UIO_USERSPACE64 || (a_uio_t)->uio_segflg == UIO_PHYS_USERSPACE64 || \
	  (a_uio_t)->uio_segflg == UIO_USERISPACE64 )
#define UIO_IS_USER_SPACE( a_uio_t )  \
	( UIO_IS_USER_SPACE32((a_uio_t)) || UIO_IS_USER_SPACE64((a_uio_t)) || \
	  (a_uio_t)->uio_segflg == UIO_USERSPACE || (a_uio_t)->uio_segflg == UIO_USERISPACE || \
	   (a_uio_t)->uio_segflg == UIO_PHYS_USERSPACE )

	
/*
 * W A R N I N G!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!
 * anything in this section will be removed.  please move to the uio KPI 
 */

#if 1 // UIO_KPI - WARNING OBSOLETE!!!!   LP64todo - remove these!!!!
// DO NOT USE THESE
#define IS_UIO_USER_SPACE32( segflg )  \
	( (segflg) == UIO_USERSPACE32 || (segflg) == UIO_PHYS_USERSPACE32 || \
	  (segflg) == UIO_USERISPACE32 )
#define IS_UIO_USER_SPACE64( segflg )  \
	( (segflg) == UIO_USERSPACE64 || (segflg) == UIO_PHYS_USERSPACE64 || \
	  (segflg) == UIO_USERISPACE64 )
#define IS_UIO_USER_SPACE( segflg )  \
	( IS_UIO_USER_SPACE32((segflg)) || IS_UIO_USER_SPACE64((segflg)) || \
	  (segflg) == UIO_USERSPACE || (segflg) == UIO_USERISPACE || \
	   (segflg) == UIO_PHYS_USERSPACE )

#define IS_UIO_SYS_SPACE32( segflg )  \
	( (segflg) == UIO_SYSSPACE32 || (segflg) == UIO_PHYS_SYSSPACE32 || \
	  (segflg) == UIO_SYSSPACE || (segflg) == UIO_PHYS_SYSSPACE )
#define IS_UIO_SYS_SPACE64( segflg )  \
	( (segflg) == UIO_SYSSPACE64 || (segflg) == UIO_PHYS_SYSSPACE64 )
#define IS_UIO_SYS_SPACE( segflg )  \
	( IS_UIO_SYS_SPACE32((segflg)) || IS_UIO_SYS_SPACE64((segflg)) )

#define IS_OBSOLETE_UIO_SEGFLG(segflg)  \
	( (segflg) == UIO_USERSPACE || (segflg) == UIO_USERISPACE || \
	  (segflg) == UIO_SYSSPACE  || (segflg) == UIO_PHYS_USERSPACE || \
	  (segflg) == UIO_PHYS_SYSSPACE )
#define IS_VALID_UIO_SEGFLG(segflg)  \
	( IS_UIO_USER_SPACE((segflg)) || IS_UIO_SYS_SPACE((segflg)) )

/* accessor routines for uio and embedded iovecs */
// WARNING all these are OBSOLETE!!!!
static inline int64_t uio_uio_resid( struct uio *a_uiop );
static inline void uio_uio_resid_add( struct uio *a_uiop, int64_t a_amount );
static inline void uio_uio_resid_set( struct uio *a_uiop, int64_t a_value );

static inline void uio_iov_base_add( struct uio *a_uiop, int64_t a_amount );
static inline void uio_iov_base_add_at( struct uio *a_uiop, int64_t a_amount, int a_index );
static inline void uio_iov_len_add( struct uio *a_uiop, int64_t a_amount );
static inline void uio_iov_len_add_at( struct uio *a_uiop, int64_t a_amount, int a_index );
static inline u_int64_t uio_iov_len( struct uio *a_uiop );
static inline u_int64_t uio_iov_len_at( struct uio *a_uiop, int a_index );
static inline u_int64_t uio_iov_base( struct uio *a_uiop );
static inline u_int64_t uio_iov_base_at( struct uio *a_uiop, int a_index );
static inline void uio_next_iov( struct uio *a_uiop );
static inline void uio_iov_len_set( struct uio *a_uiop, u_int64_t a_value );
static inline void uio_iov_len_set_at( struct uio *a_uiop, u_int64_t a_value, int a_index );


static inline int64_t uio_uio_resid( struct uio *a_uiop )
{
//#warning obsolete - use uio_resid call
	return( (int64_t)a_uiop->uio_resid );
}

static inline void uio_uio_resid_add( struct uio *a_uiop, int64_t a_amount )
{
//#warning obsolete - use uio_update or uio_addiov or uio_setresid if in kernel and you must
	a_uiop->uio_resid += ((int32_t) a_amount);
}

static inline void uio_uio_resid_set( struct uio *a_uiop, int64_t a_value )
{
//#warning obsolete - use uio_update or uio_addiov or uio_setresid if in kernel and you must
	a_uiop->uio_resid = a_value;
}

static inline u_int64_t uio_iov_base( struct uio *a_uiop )
{
//#warning obsolete - use uio_curriovbase call
	return(uio_iov_base_at(a_uiop, 0));
}

static inline u_int64_t uio_iov_base_at( struct uio *a_uiop, int a_index )
{
//#warning obsolete - use uio_curriovbase call
	if (IS_UIO_USER_SPACE32(a_uiop->uio_segflg) || IS_OBSOLETE_UIO_SEGFLG(a_uiop->uio_segflg)) {
		/* user space iovec was most likely a struct iovec so we must cast to uintptr_t first */
		return((u_int64_t)((uintptr_t)a_uiop->uio_iovs.iov32p[a_index].iov_base));
	}
	if (IS_UIO_SYS_SPACE32(a_uiop->uio_segflg)) {
		return((u_int64_t)a_uiop->uio_iovs.iov32p[a_index].iov_base);
	}
	if (IS_UIO_USER_SPACE64(a_uiop->uio_segflg) || IS_UIO_SYS_SPACE64(a_uiop->uio_segflg)) {
		return(a_uiop->uio_iovs.iov64p[a_index].iov_base);
	}
	return(0);
}

static inline u_int64_t uio_iov_len( struct uio *a_uiop )
{
//#warning obsolete - use uio_curriovlen call
	return(uio_iov_len_at(a_uiop, 0));
}

static inline u_int64_t uio_iov_len_at( struct uio *a_uiop, int a_index )
{
//#warning obsolete - use uio_curriovlen call
	if (IS_UIO_USER_SPACE32(a_uiop->uio_segflg) || 
		IS_UIO_SYS_SPACE32(a_uiop->uio_segflg)  ||
		IS_OBSOLETE_UIO_SEGFLG(a_uiop->uio_segflg)) {
		return((u_int64_t)a_uiop->uio_iovs.iov32p[a_index].iov_len);
	}
	if (IS_UIO_USER_SPACE64(a_uiop->uio_segflg) || IS_UIO_SYS_SPACE64(a_uiop->uio_segflg)) {
		return(a_uiop->uio_iovs.iov64p[a_index].iov_len);
	}
	return(0);
}

static inline void uio_iov_len_set_at( struct uio *a_uiop, u_int64_t a_value, int a_index )
{
//#warning obsolete - use uio_addiov call
	if (IS_UIO_USER_SPACE32(a_uiop->uio_segflg) || 
		IS_UIO_SYS_SPACE32(a_uiop->uio_segflg)	||
		IS_OBSOLETE_UIO_SEGFLG(a_uiop->uio_segflg)) {
		a_uiop->uio_iovs.iov32p[a_index].iov_len = a_value;
	}
	else if (IS_UIO_USER_SPACE64(a_uiop->uio_segflg) || IS_UIO_SYS_SPACE64(a_uiop->uio_segflg)) {
		a_uiop->uio_iovs.iov64p[a_index].iov_len = a_value;
	}
	return;
}

static inline void uio_iov_len_set( struct uio *a_uiop, u_int64_t a_value )
{
//#warning obsolete - use uio_addiov call
	return(uio_iov_len_set_at(a_uiop, a_value, 0));
}

static inline void uio_iov_len_add_at( struct uio *a_uiop, int64_t a_amount, int a_index )
{
//#warning obsolete - use uio_addiov call
	if (IS_UIO_USER_SPACE32(a_uiop->uio_segflg) || 
		IS_UIO_SYS_SPACE32(a_uiop->uio_segflg)	||
		IS_OBSOLETE_UIO_SEGFLG(a_uiop->uio_segflg)) {
		a_uiop->uio_iovs.iov32p[a_index].iov_len += ((int32_t) a_amount);
	}
	else if (IS_UIO_USER_SPACE64(a_uiop->uio_segflg) || IS_UIO_SYS_SPACE64(a_uiop->uio_segflg)) {
		a_uiop->uio_iovs.iov64p[a_index].iov_len += a_amount;
	}
	return;
}

static inline void uio_iov_len_add( struct uio *a_uiop, int64_t a_amount )
{
//#warning obsolete - use uio_addiov call
	return(uio_iov_len_add_at(a_uiop, a_amount, 0));
}

static inline void uio_iov_base_add_at( struct uio *a_uiop, int64_t a_amount, int a_index )
{
//#warning obsolete - use uio_addiov call
	if (IS_UIO_USER_SPACE32(a_uiop->uio_segflg) || 
		IS_UIO_SYS_SPACE32(a_uiop->uio_segflg)	||
		IS_OBSOLETE_UIO_SEGFLG(a_uiop->uio_segflg)) {
		a_uiop->uio_iovs.iov32p[a_index].iov_base += ((int32_t) a_amount);
	}
	else if (IS_UIO_USER_SPACE64(a_uiop->uio_segflg) || IS_UIO_SYS_SPACE64(a_uiop->uio_segflg)) {
		a_uiop->uio_iovs.iov64p[a_index].iov_base += a_amount;
	}
	return;
}

static inline void uio_iov_base_add( struct uio *a_uiop, int64_t a_amount )
{
//#warning obsolete - use uio_addiov call
	return(uio_iov_base_add_at(a_uiop, a_amount, 0));
}

static inline void uio_next_iov( struct uio *a_uiop )
{
//#warning obsolete - use uio_update call
	if (IS_UIO_USER_SPACE32(a_uiop->uio_segflg) || 
		IS_UIO_SYS_SPACE32(a_uiop->uio_segflg)	||
		IS_OBSOLETE_UIO_SEGFLG(a_uiop->uio_segflg)) {
		a_uiop->uio_iovs.iov32p++;
	}
	else if (IS_UIO_USER_SPACE64(a_uiop->uio_segflg) || IS_UIO_SYS_SPACE64(a_uiop->uio_segflg)) {
		a_uiop->uio_iovs.iov64p++;
	}
	return;
}
	
/*
 * WARNING - this routine relies on iovec_64 being larger than iovec_32 and will
 * not work if you are going to initialize an array of iovec_64 as an array of 
 * iovec_32 then pass that array in a uio (since uio_iov is always expected to
 * be an array of like sized iovecs - see how uio_next_iov gets to the next iovec)
 */
static inline void init_iovec( u_int64_t a_base, 
							   u_int64_t a_len, 
							   struct iovec_64 *a_iovp, 
							   int is_64bit_process )
{
//#warning obsolete - use uio_create call
	if (is_64bit_process) {
		a_iovp->iov_base = a_base;
		a_iovp->iov_len = a_len;
	}
	else {
		struct iovec_32 *a_iov32p = (struct iovec_32 *) a_iovp;
		a_iov32p->iov_base = a_base;
		a_iov32p->iov_len = a_len;
	}
	return;
}

#define INIT_UIO_BASE( uiop, iovcnt, offset, resid, rw, procp ) \
{ \
	(uiop)->uio_iovcnt = (iovcnt); \
	(uiop)->uio_offset = (offset); \
	(uiop)->uio_resid = (resid); \
	(uiop)->uio_rw = (rw); \
	(uiop)->uio_procp = (procp); \
}
#define INIT_UIO_USER32( uiop, iovp, iovcnt, offset, resid, rw, procp ) \
{ \
	(uiop)->uio_iovs.iov32p = (iovp);  \
	(uiop)->uio_segflg = UIO_USERSPACE; \
	INIT_UIO_BASE((uiop), (iovcnt), (offset), (resid), (rw), (procp)); \
}
#define INIT_UIO_USER64( uiop, iovp, iovcnt, offset, resid, rw, procp ) \
{ \
	(uiop)->uio_iovs.iov64p = (iovp);  \
	(uiop)->uio_segflg = UIO_USERSPACE64; \
	INIT_UIO_BASE((uiop), (iovcnt), (offset), (resid), (rw), (procp)); \
}
#define INIT_UIO_SYS32( uiop, iovp, iovcnt, offset, resid, rw, procp ) \
{ \
	(uiop)->uio_iovs.iov32p = (iovp);  \
	(uiop)->uio_segflg = UIO_SYSSPACE; \
	INIT_UIO_BASE((uiop), (iovcnt), (offset), (resid), (rw), (procp)); \
}
#define INIT_UIO_USERSPACE( uiop, iovp, iovcnt, offset, resid, rw, procp ) \
{ \
	if (IS_64BIT_PROCESS((procp))) { \
		(uiop)->uio_iovs.iov64p = (iovp);  \
		(uiop)->uio_segflg = UIO_USERSPACE64; \
	} \
	else {  \
		(uiop)->uio_iovs.iov32p = (struct iovec_32 *)(iovp);  \
		(uiop)->uio_segflg = UIO_USERSPACE; \
	}  \
	INIT_UIO_BASE((uiop), (iovcnt), (offset), (resid), (rw), (procp)); \
}
#define INIT_UIO_SYSSPACE( uiop, iovp, iovcnt, offset, resid, rw, procp ) \
{ \
	if (0) { /* we do not support 64-bit system space yet */ \
		(uiop)->uio_iovs.iov64p = (iovp);  \
		(uiop)->uio_segflg = UIO_SYSSPACE64; \
	} \
	else {  \
		(uiop)->uio_iovs.iov32p = (struct iovec_32 *)(iovp);  \
		(uiop)->uio_segflg = UIO_SYSSPACE; \
	}  \
	INIT_UIO_BASE((uiop), (iovcnt), (offset), (resid), (rw), (procp)); \
}
#endif // UIO_KPI - WARNING OBSOLETE!!!! 


#endif /* KERNEL */
#endif /* !_SYS_UIO_INTERNAL_H_ */

/*
 * Copyright (c) 2003 Apple Computer, Inc. All rights reserved.
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

#ifndef _I386_CPU_CAPABILITIES_H
#define _I386_CPU_CAPABILITIES_H

/* Sadly, some clients of this interface misspell __APPLE_API_PRIVATE.
 * To avoid breaking them, we accept the incorrect _APPLE_API_PRIVATE.
 */
#ifdef	_APPLE_API_PRIVATE
#ifndef __APPLE_API_PRIVATE
#define	__APPLE_API_PRIVATE
#endif	/* __APPLE_API_PRIVATE */
#endif	/* _APPLE_API_PRIVATE */
 
#ifndef __APPLE_API_PRIVATE
#error	cpu_capabilities.h is for Apple Internal use only
#else	/* __APPLE_API_PRIVATE */

/*
 * This is the authoritative way to determine from user mode what
 * implementation-specific processor features are available.
 * This API only supported for Apple internal use.
 * 
 */

/* Bit definitions for _cpu_capabilities: */

#define	kHasMMX				0x00000001
#define	kHasSSE				0x00000002
#define	kHasSSE2			0x00000004
#define	kHasPNI				0x00000008  // Prescott New Instructions
#define	kCache32			0x00000010	// cache line size is 32 bytes
#define	kCache64			0x00000020
#define	kCache128			0x00000040

#define	kUP				0x00008000	// set if (kNumCPUs == 1)
#define	kNumCPUs			0x00FF0000	// number of CPUs (see _NumCPUs() below)

#define	kNumCPUsShift		16			// see _NumCPUs() below

#ifndef	__ASSEMBLER__
 
extern uint32_t _get_cpu_capabilities( void );

inline static
int _NumCPUs( void )
{
	return (_get_cpu_capabilities() & kNumCPUs) >> kNumCPUsShift;
}

#endif /* __ASSEMBLER__ */


/*
 * The shared kernel/user "comm page(s)":
 *
 * The last eight pages of every address space are reserved for the kernel/user
 * "comm area". During system initialization, the kernel populates the comm page with
 * code customized for the particular processor and platform.
 *
 * Because Mach VM cannot map the last page of an address space, the max length of
 * the comm area is seven pages.
 */
 
#define _COMM_PAGE_BASE_ADDRESS			0xBFFF9000						// VM_MAX_ADDRESS - 7 * 4096 
#define _COMM_PAGE_SIGS_OFFSET			0x4000							// offset to routine signatures
#define	_COMM_PAGE_AREA_LENGTH			( 7*4096)						// reserved length of entire comm area
 
/* data in the comm page */
 
#define _COMM_PAGE_SIGNATURE			(_COMM_PAGE_BASE_ADDRESS+0x000)	// first few bytes are a signature
#define _COMM_PAGE_VERSION				(_COMM_PAGE_BASE_ADDRESS+0x01E)	// 16-bit version#
#define	_COMM_PAGE_THIS_VERSION			1								// this is version 1 of the commarea format
  
#define _COMM_PAGE_CPU_CAPABILITIES		(_COMM_PAGE_BASE_ADDRESS+0x020)	// uint32_t _cpu_capabilities
#define _COMM_PAGE_NCPUS				(_COMM_PAGE_BASE_ADDRESS+0x021)	// uint8_t number of configured CPUs
#define _COMM_PAGE_VECTOR_FLAVOR		(_COMM_PAGE_BASE_ADDRESS+0x024)	// uint8_t SSE/SSE2/PNI 
#define _COMM_PAGE_CACHE_LINESIZE		(_COMM_PAGE_BASE_ADDRESS+0x026)	// uint16_t cache line size
 
#define _COMM_PAGE_UNUSED1				(_COMM_PAGE_BASE_ADDRESS+0x030)	// 16 unused bytes
 
#define _COMM_PAGE_2_TO_52				(_COMM_PAGE_BASE_ADDRESS+0x040)	// double float constant 2**52
#define _COMM_PAGE_10_TO_6				(_COMM_PAGE_BASE_ADDRESS+0x048)	// double float constant 10**6
 
#define _COMM_PAGE_UNUSED2				(_COMM_PAGE_BASE_ADDRESS+0x050)	// 16 unused bytes
 
#define _COMM_PAGE_TIMEBASE				(_COMM_PAGE_BASE_ADDRESS+0x060)	// used by gettimeofday()
#define _COMM_PAGE_TIMESTAMP			(_COMM_PAGE_BASE_ADDRESS+0x068)	// used by gettimeofday()
#define _COMM_PAGE_SEC_PER_TICK			(_COMM_PAGE_BASE_ADDRESS+0x070)	// used by gettimeofday()
 
#define _COMM_PAGE_UNUSED3				(_COMM_PAGE_BASE_ADDRESS+0x080)	// 384 unused bytes
 
 /* jump table (bla to this address, which may be a branch to the actual code somewhere else) */
 /* When new jump table entries are added, corresponding symbols should be added below         */
 
#define _COMM_PAGE_ABSOLUTE_TIME		(_COMM_PAGE_BASE_ADDRESS+0x200)	// mach_absolute_time()
#define _COMM_PAGE_SPINLOCK_TRY			(_COMM_PAGE_BASE_ADDRESS+0x220)	// spinlock_try()
#define _COMM_PAGE_SPINLOCK_LOCK		(_COMM_PAGE_BASE_ADDRESS+0x260)	// spinlock_lock()
#define _COMM_PAGE_SPINLOCK_UNLOCK		(_COMM_PAGE_BASE_ADDRESS+0x2a0)	// spinlock_unlock()
#define _COMM_PAGE_PTHREAD_GETSPECIFIC	(_COMM_PAGE_BASE_ADDRESS+0x2c0)	// pthread_getspecific()
#define _COMM_PAGE_GETTIMEOFDAY			(_COMM_PAGE_BASE_ADDRESS+0x2e0)	// used by gettimeofday()
#define _COMM_PAGE_FLUSH_DCACHE			(_COMM_PAGE_BASE_ADDRESS+0x4e0)	// sys_dcache_flush()
#define _COMM_PAGE_FLUSH_ICACHE			(_COMM_PAGE_BASE_ADDRESS+0x520)	// sys_icache_invalidate()
#define _COMM_PAGE_PTHREAD_SELF			(_COMM_PAGE_BASE_ADDRESS+0x580)	// pthread_self()
#define _COMM_PAGE_UNUSED4				(_COMM_PAGE_BASE_ADDRESS+0x5a0)	// 32 unused bytes
#define _COMM_PAGE_RELINQUISH			(_COMM_PAGE_BASE_ADDRESS+0x5c0) // used by spinlocks
 
#define	_COMM_PAGE_UNUSED5				(_COMM_PAGE_BASE_ADDRESS+0x5e0) // 32 unused bytes
 
#define _COMM_PAGE_BZERO				(_COMM_PAGE_BASE_ADDRESS+0x600)	// bzero()
#define _COMM_PAGE_BCOPY				(_COMM_PAGE_BASE_ADDRESS+0x780)	// bcopy()
#define	_COMM_PAGE_MEMCPY				(_COMM_PAGE_BASE_ADDRESS+0x7a0)	// memcpy()
#define	_COMM_PAGE_MEMMOVE				(_COMM_PAGE_BASE_ADDRESS+0x7a0)	// memmove()

#define	_COMM_PAGE_UNUSED6				(_COMM_PAGE_BASE_ADDRESS+0xF80) // 128 unused bytes

#define	_COMM_PAGE_BIGCOPY				(_COMM_PAGE_BASE_ADDRESS+0x1000)// very-long-operand copies

#define _COMM_PAGE_END					(_COMM_PAGE_BASE_ADDRESS+0x1600)// end of common page

#ifdef __ASSEMBLER__
#ifdef __COMM_PAGE_SYMBOLS

#define CREATE_COMM_PAGE_SYMBOL(symbol_name, symbol_address)		\
				.org	(symbol_address - (_COMM_PAGE_BASE_ADDRESS & 0xFFFFE000)) ;\
symbol_name: nop

	.text		// Required to make a well behaved symbol file

	CREATE_COMM_PAGE_SYMBOL(___mach_absolute_time, _COMM_PAGE_ABSOLUTE_TIME)
	CREATE_COMM_PAGE_SYMBOL(___spin_lock_try, _COMM_PAGE_SPINLOCK_TRY)
	CREATE_COMM_PAGE_SYMBOL(___spin_lock, _COMM_PAGE_SPINLOCK_LOCK)
	CREATE_COMM_PAGE_SYMBOL(___spin_unlock, _COMM_PAGE_SPINLOCK_UNLOCK)
	CREATE_COMM_PAGE_SYMBOL(___pthread_getspecific, _COMM_PAGE_PTHREAD_GETSPECIFIC)
	CREATE_COMM_PAGE_SYMBOL(___gettimeofday, _COMM_PAGE_GETTIMEOFDAY)
	CREATE_COMM_PAGE_SYMBOL(___sys_dcache_flush, _COMM_PAGE_FLUSH_DCACHE)
	CREATE_COMM_PAGE_SYMBOL(___sys_icache_invalidate, _COMM_PAGE_FLUSH_ICACHE)
	CREATE_COMM_PAGE_SYMBOL(___pthread_self, _COMM_PAGE_PTHREAD_SELF)
	CREATE_COMM_PAGE_SYMBOL(___spin_lock_relinquish, _COMM_PAGE_RELINQUISH)
	CREATE_COMM_PAGE_SYMBOL(___bzero, _COMM_PAGE_BZERO)
	CREATE_COMM_PAGE_SYMBOL(___bcopy, _COMM_PAGE_BCOPY)
	CREATE_COMM_PAGE_SYMBOL(___memcpy, _COMM_PAGE_MEMCPY)
//	CREATE_COMM_PAGE_SYMBOL(___memmove, _COMM_PAGE_MEMMOVE)
	CREATE_COMM_PAGE_SYMBOL(___bigcopy, _COMM_PAGE_BIGCOPY)
	CREATE_COMM_PAGE_SYMBOL(___end_comm_page, _COMM_PAGE_END)

	.data		// Required to make a well behaved symbol file
	.long	0	// Required to make a well behaved symbol file

#endif /* __COMM_PAGE_SYMBOLS */
#endif /* __ASSEMBLER__ */

#endif /* __APPLE_API_PRIVATE */
#endif /* _I386_CPU_CAPABILITIES_H */

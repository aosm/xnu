/*
 * Copyright (c) 2000 Apple Computer, Inc. All rights reserved.
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
/*
 * @OSF_COPYRIGHT@
 */
/* 
 * Mach Operating System
 * Copyright (c) 1991,1990,1989,1988,1987 Carnegie Mellon University
 * All Rights Reserved.
 * 
 * Permission to use, copy, modify and distribute this software and its
 * documentation is hereby granted, provided that both the copyright
 * notice and this permission notice appear in all copies of the
 * software, derivative works or modified versions, and any portions
 * thereof, and that both notices appear in supporting documentation.
 * 
 * CARNEGIE MELLON ALLOWS FREE USE OF THIS SOFTWARE IN ITS "AS IS"
 * CONDITION.  CARNEGIE MELLON DISCLAIMS ANY LIABILITY OF ANY KIND FOR
 * ANY DAMAGES WHATSOEVER RESULTING FROM THE USE OF THIS SOFTWARE.
 * 
 * Carnegie Mellon requests users of this software to return to
 * 
 *  Software Distribution Coordinator  or  Software.Distribution@CS.CMU.EDU
 *  School of Computer Science
 *  Carnegie Mellon University
 *  Pittsburgh PA 15213-3890
 * 
 * any improvements or extensions that they make and grant Carnegie Mellon
 * the rights to redistribute these changes.
 */

#ifdef	KERNEL_PRIVATE

#ifndef	_PPC_SIMPLE_LOCK_TYPES_H_
#define	_PPC_SIMPLE_LOCK_TYPES_H_

#ifdef	KERNEL_PRIVATE
#include <mach/boolean.h>
#include <kern/kern_types.h>

#include <sys/appleapiopts.h>
#ifdef  MACH_KERNEL_PRIVATE
#include <ppc/hw_lock_types.h>
#include <ppc/locks.h>
#include <mach_ldebug.h>
#endif

#ifdef MACH_KERNEL_PRIVATE

#if MACH_LDEBUG
#define USLOCK_DEBUG 1
#else
#define USLOCK_DEBUG 0
#endif

#if     !USLOCK_DEBUG

typedef lck_spin_t usimple_lock_data_t, *usimple_lock_t;

#else

typedef struct uslock_debug {
	void			*lock_pc;	/* pc where lock operation began    */
	void			*lock_thread;	/* thread that acquired lock */
	unsigned long	duration[2];
	unsigned short	state;
	unsigned char	lock_cpu;
	void			*unlock_thread;	/* last thread to release lock */
	unsigned char	unlock_cpu;
	void			*unlock_pc;	/* pc where lock operation ended    */
} uslock_debug;

typedef struct {
	hw_lock_data_t	interlock;	/* must be first... see lock.c */
	unsigned short	lock_type;	/* must be second... see lock.c */
#define USLOCK_TAG	0x5353
	uslock_debug	debug;
} usimple_lock_data_t, *usimple_lock_t;

#endif	/* USLOCK_DEBUG */

#else

typedef	struct slock {
	unsigned int	lock_data[10];
} usimple_lock_data_t, *usimple_lock_t;

#endif	/* MACH_KERNEL_PRIVATE */

#define	USIMPLE_LOCK_NULL	((usimple_lock_t) 0)

#if !defined(decl_simple_lock_data)

typedef usimple_lock_data_t	*simple_lock_t;
typedef usimple_lock_data_t	simple_lock_data_t;

#define	decl_simple_lock_data(class,name) \
	class	simple_lock_data_t	name;

#endif	/* !defined(decl_simple_lock_data) */

#ifdef	MACH_KERNEL_PRIVATE
#if	!MACH_LDEBUG

#define MACHINE_SIMPLE_LOCK

extern void						ppc_usimple_lock_init(simple_lock_t,unsigned short);
extern void						ppc_usimple_lock(simple_lock_t);
extern void						ppc_usimple_unlock_rwmb(simple_lock_t);
extern void						ppc_usimple_unlock_rwcmb(simple_lock_t);
extern unsigned int				ppc_usimple_lock_try(simple_lock_t);

#define simple_lock_init(l,t)	ppc_usimple_lock_init(l,t)
#define simple_lock(l)			ppc_usimple_lock(l)
#define simple_unlock(l)		ppc_usimple_unlock_rwcmb(l)
#define simple_unlock_rwmb(l)	ppc_usimple_unlock_rwmb(l)
#define simple_lock_try(l)		ppc_usimple_lock_try(l)
#define simple_lock_addr(l)		(&(l))
#define thread_sleep_simple_lock(l, e, i) \
								thread_sleep_fast_usimple_lock((l), (e), (i))
#endif	/* !MACH_LDEBUG */

extern unsigned int		hw_lock_bit(
					unsigned int *,
					unsigned int,
					unsigned int);

extern unsigned int		hw_cpu_sync(
					unsigned int *,
					unsigned int);

extern unsigned int		hw_cpu_wcng(
					unsigned int *,
					unsigned int,
					unsigned int);

extern unsigned int		hw_lock_mbits(
					unsigned int *,
					unsigned int,
					unsigned int,
					unsigned int,
					unsigned int);

void				hw_unlock_bit(
					unsigned int *,
					unsigned int);

#endif	/* MACH_KERNEL_PRIVATE */
#endif	/* KERNEL_PRIVATE */

#endif /* !_PPC_SIMPLE_LOCK_TYPES_H_ */

#endif	/* KERNEL_PRIVATE */

/*
 * Copyright (c) 2000 Apple Computer, Inc. All rights reserved.
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
/*
 * @OSF_COPYRIGHT@
 */
/*
 * HISTORY
 * 
 * Revision 1.1.1.1  1998/09/22 21:05:31  wsanchez
 * Import of Mac OS X kernel (~semeria)
 *
 * Revision 1.1.1.1  1998/03/07 02:25:46  wsanchez
 * Import of OSF Mach kernel (~mburg)
 *
 * Revision 1.1.8.1  1996/12/09  16:50:19  stephen
 * 	nmklinux_1.0b3_shared into pmk1.1
 * 	[1996/12/09  10:51:34  stephen]
 *
 * Revision 1.1.6.1  1996/04/11  11:20:26  emcmanus
 * 	Copied from mainline.ppc.
 * 	[1996/04/10  16:57:16  emcmanus]
 * 
 * Revision 1.1.4.1  1995/11/23  17:37:13  stephen
 * 	first powerpc checkin to mainline.ppc
 * 	[1995/11/23  16:45:52  stephen]
 * 
 * Revision 1.1.2.1  1995/08/25  06:50:05  stephen
 * 	Initial checkin of files for PowerPC port
 * 	[1995/08/23  15:05:11  stephen]
 * 
 * Revision 1.2.8.2  1995/01/06  19:50:48  devrcs
 * 	mk6 CR668 - 1.3b26 merge
 * 	64bit cleanup
 * 	[1994/10/14  03:42:42  dwm]
 * 
 * Revision 1.2.8.1  1994/09/23  02:38:01  ezf
 * 	change marker to not FREE
 * 	[1994/09/22  21:40:30  ezf]
 * 
 * Revision 1.2.2.2  1993/06/09  02:41:01  gm
 * 	Added to OSF/1 R1.3 from NMK15.0.
 * 	[1993/06/02  21:16:38  jeffc]
 * 
 * Revision 1.2  1993/04/19  16:34:37  devrcs
 * 	ansi C conformance changes
 * 	[1993/02/02  18:56:34  david]
 * 
 * Revision 1.1  1992/09/30  02:30:57  robert
 * 	Initial revision
 * 
 * $EndLog$
 */
/* CMU_HIST */
/*
 * Revision 2.4  91/05/14  16:53:00  mrt
 * 	Correcting copyright
 * 
 * Revision 2.3  91/02/05  17:32:34  mrt
 * 	Changed to new Mach copyright
 * 	[91/02/01  17:10:49  mrt]
 * 
 * Revision 2.2  90/05/03  15:48:32  dbg
 * 	First checkin.
 * 
 * Revision 1.3  89/03/09  20:20:12  rpd
 * 	More cleanup.
 * 
 * Revision 1.2  89/02/26  13:01:20  gm0w
 * 	Changes for cleanup.
 * 
 * 31-Dec-88  Robert Baron (rvb) at Carnegie-Mellon University
 *	Derived from MACH2.0 vax release.
 *
 * 23-Apr-87  Michael Young (mwyoung) at Carnegie-Mellon University
 *	Changed things to "unsigned int" to appease the user community :-).
 *
 */
/* CMU_ENDHIST */
/* 
 * Mach Operating System
 * Copyright (c) 1991,1990,1989,1988 Carnegie Mellon University
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
/*
 */

/*
 *	File:	vm_types.h
 *	Author:	Avadis Tevanian, Jr.
 *	Date: 1985
 *
 *	Header file for VM data types.  I386 version.
 */

#ifndef	_MACH_PPC_VM_TYPES_H_
#define _MACH_PPC_VM_TYPES_H_

#ifndef	ASSEMBLER

/*
 * A natural_t is the type for the native
 * integer type, e.g. 32 or 64 or.. whatever
 * register size the machine has.  Unsigned, it is
 * used for entities that might be either
 * unsigned integers or pointers, and for
 * type-casting between the two.
 * For instance, the IPC system represents
 * a port in user space as an integer and
 * in kernel space as a pointer.
 */
typedef unsigned int	natural_t;

/*
 * An integer_t is the signed counterpart
 * of the natural_t type. Both types are
 * only supposed to be used to define
 * other types in a machine-independent
 * way.
 */
typedef int		integer_t;

#ifdef MACH_KERNEL_PRIVATE
/*
 * An int32 is an integer that is at least 32 bits wide
 */
typedef int		int32;
typedef unsigned int	uint32;
#endif

/*
 * A vm_offset_t is a type-neutral pointer,
 * e.g. an offset into a virtual memory space.
 */
typedef	natural_t	vm_offset_t;

/*
 * A vm_size_t is the proper type for e.g.
 * expressing the difference between two
 * vm_offset_t entities.
 */
typedef	natural_t		vm_size_t;
typedef	unsigned long long	vm_double_size_t;

/*
 * space_t is used in the pmap system
 */
typedef unsigned int	space_t;

#endif	/* ndef ASSEMBLER */

/*
 * If composing messages by hand (please dont)
 */

#define	MACH_MSG_TYPE_INTEGER_T	MACH_MSG_TYPE_INTEGER_32

#endif	/* _MACH_PPC_VM_TYPES_H_ */

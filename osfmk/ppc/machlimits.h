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
 * Revision 1.1.1.1  1998/09/22 21:05:41  wsanchez
 * Import of Mac OS X kernel (~semeria)
 *
 * Revision 1.1.1.1  1998/03/07 02:26:02  wsanchez
 * Import of OSF Mach kernel (~mburg)
 *
 * Revision 1.1.2.1  1996/12/09  16:55:05  stephen
 * 	nmklinux_1.0b3_shared into pmk1.1
 * 	New file based on hp_pa
 * 	[1996/12/09  11:09:22  stephen]
 *
 * $EndLog$
 */
/*
 * Copyright (c) 1988 The Regents of the University of California.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms are permitted
 * provided that the above copyright notice and this paragraph are
 * duplicated in all such forms and that any documentation,
 * advertising materials, and other materials related to such
 * distribution and use acknowledge that the software was developed
 * by the University of California, Berkeley.  The name of the
 * University may not be used to endorse or promote products derived
 * from this software without specific prior written permission.
 * THIS SOFTWARE IS PROVIDED ``AS IS'' AND WITHOUT ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, WITHOUT LIMITATION, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE.
 *
 *	@(#)machlimits.h	7.1 (Berkeley) 2/15/89
 */
#ifndef _MACH_MACHLIMITS_H_
#define _MACH_MACHLIMITS_H_

#define	CHAR_BIT	8		/* number of bits in a char */

#define	SCHAR_MAX	127		/* max value for a signed char */
#define	SCHAR_MIN	(-128)		/* min value for a signed char */

#define	UCHAR_MAX	255U		/* max value for an unsigned char */
#define	CHAR_MAX	127		/* max value for a char */
#define	CHAR_MIN	(-128)		/* min value for a char */

#define	USHRT_MAX	65535U		/* max value for an unsigned short */
#define	SHRT_MAX	32767		/* max value for a short */
#define	SHRT_MIN	(-32768)	/* min value for a short */

#define	UINT_MAX	0xFFFFFFFFU	/* max value for an unsigned int */
#define	INT_MAX		2147483647	/* max value for an int */
#define	INT_MIN		(-2147483647-1)	/* min value for an int */

#define	ULONG_MAX	UINT_MAX	/* max value for an unsigned long */
#define	LONG_MAX	INT_MAX		/* max value for a long */
#define	LONG_MIN	INT_MIN		/* min value for a long */

/* Must be at least two, for internationalization (NLS/KJI) */
#define MB_LEN_MAX	4		/* multibyte characters */

#endif /* _MACH_MACHLIMITS_H_ */

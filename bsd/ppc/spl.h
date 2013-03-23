/*
 * Copyright (c) 2000-2002 Apple Computer, Inc. All rights reserved.
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
#ifndef	_BSD_PPC_SPL_H_
#define	_BSD_PPC_SPL_H_

#ifdef KERNEL
#ifndef __ASSEMBLER__
/*
 *	Machine-dependent SPL definitions.
 *
 */
typedef unsigned	spl_t;

extern unsigned	sploff(void);
extern unsigned	splhigh(void);
extern unsigned	splsched(void);
extern unsigned	splclock(void);
extern unsigned	splpower(void);
extern unsigned	splvm(void);
extern unsigned	splbio(void);
extern unsigned	splimp(void);
extern unsigned	spltty(void);
extern unsigned	splnet(void);
extern unsigned	splsoftclock(void);

extern void	spllo(void);
extern void	splon(unsigned level);
extern void	splx(unsigned level);
extern void	spln(unsigned level);
#define splstatclock()	splhigh()

#endif /* __ASSEMBLER__ */

#endif

#endif	/* _BSD_PPC_SPL_H_ */

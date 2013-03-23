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
 * Mach Operating System
 * Copyright (c) 1987 Carnegie-Mellon University
 * All rights reserved.  The CMU software License Agreement specifies
 * the terms and conditions for use and redistribution.
 */
/*
 *	File:	vm_pager.h
 *	Author:	Avadis Tevanian, Jr., Michael Wayne Young
 *
 *	Copyright (C) 1986, Avadis Tevanian, Jr., Michael Wayne Young
 *	Copyright (C) 1985, Avadis Tevanian, Jr., Michael Wayne Young
 *
 *	Pager routine interface definition
 */

#ifndef	_VM_PAGER_
#define	_VM_PAGER_

#include <mach/boolean.h>

struct	pager_struct {
	boolean_t	is_device;
};
typedef	struct pager_struct	*vm_pager_t;
#define	vm_pager_null		((vm_pager_t) 0)

#define	PAGER_SUCCESS		0  /* page read or written */
#define	PAGER_ABSENT		1  /* pager does not have page */
#define	PAGER_ERROR		2  /* pager unable to read or write page */

#ifdef	KERNEL
typedef	int		pager_return_t;

extern vm_pager_t		vm_pager_allocate(void);
extern void				vm_pager_deallocate(void);
extern pager_return_t	vm_pager_get(void);
extern pager_return_t	vm_pager_put(void);
extern boolean_t		vm_pager_has_page(void);
#endif	/* KERNEL */

#endif	/* _VM_PAGER_ */

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
/*
 * @OSF_COPYRIGHT@
 */
#include <sys/appleapiopts.h>

#ifndef _MACH_TASK_LEDGER_H_
#define _MACH_TASK_LEDGER_H_

#ifdef  __APPLE_API_EVOLVING

/*
 * Definitions for task ledger line items
 */
#define ITEM_THREADS		0	/* number of threads	*/
#define ITEM_TASKS		1	/* number of tasks	*/

#define ITEM_VM	   		2	/* virtual space (bytes)*/

#define LEDGER_N_ITEMS		3	/* Total line items	*/

#define LEDGER_UNLIMITED	0	/* ignored item.maximum	*/

#endif  /* __APPLE_API_EVOLVING */

#endif  /* _MACH_TASK_LEDGER_H_ */

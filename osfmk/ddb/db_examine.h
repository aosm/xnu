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
 * Revision 1.1.1.1  1998/09/22 21:05:48  wsanchez
 * Import of Mac OS X kernel (~semeria)
 *
 * Revision 1.1.1.1  1998/03/07 02:26:09  wsanchez
 * Import of OSF Mach kernel (~mburg)
 *
 * Revision 1.1.7.2  1996/01/09  19:15:43  devrcs
 * 	Function prototypes for db_print_loc() & db_print_inst().
 * 	[1995/12/01  21:42:06  jfraser]
 *
 * 	Merged '64-bit safe' changes from DEC alpha port.
 * 	[1995/11/21  18:03:03  jfraser]
 *
 * Revision 1.1.7.1  1994/09/23  01:18:55  ezf
 * 	change marker to not FREE
 * 	[1994/09/22  21:09:49  ezf]
 * 
 * Revision 1.1.2.4  1993/09/17  21:34:33  robert
 * 	change marker to OSF_FREE_COPYRIGHT
 * 	[1993/09/17  21:27:11  robert]
 * 
 * Revision 1.1.2.3  1993/08/11  22:12:10  elliston
 * 	Add ANSI Prototypes.  CR #9523.
 * 	[1993/08/11  03:33:11  elliston]
 * 
 * Revision 1.1.2.2  1993/07/27  18:27:12  elliston
 * 	Add ANSI prototypes.  CR #9523.
 * 	[1993/07/27  18:11:28  elliston]
 * 
 * $EndLog$
 */

#ifndef	_DDB_DB_EXAMINE_H_
#define	_DDB_DB_EXAMINE_H_

#include <machine/db_machdep.h>
#include <kern/task.h>

/* Prototypes for functions exported by this module.
 */

void db_examine_cmd(
	db_expr_t	addr,
	int		have_addr,
	db_expr_t	count,
	char *		modif);

void db_examine_forward(
	db_expr_t	addr,
	int		have_addr,
	db_expr_t	count,
	char *		modif);

void db_examine_backward(
	db_expr_t	addr,
	int		have_addr,
	db_expr_t	count,
	char *		modif);

void db_examine(
	db_addr_t	addr,
	char *		fmt,	/* format string */
	int		count,	/* repeat count */
	task_t		task);

void db_print_cmd(void);

void db_print_loc(
	db_addr_t       loc,
	task_t          task);

void
db_print_inst(
	db_addr_t       loc,
	task_t          task);

void db_print_loc_and_inst(
	db_addr_t	loc,
	task_t		task);

void db_search_cmd(void);

void db_search(
	db_addr_t	addr,
	int		size,
	db_expr_t	value,
	db_expr_t	mask,
	unsigned int	count,
	task_t		task);

#endif	/* !_DDB_DB_EXAMINE_H_ */

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
#include <pexpert/pexpert.h>
#include <pexpert/protos.h>
#include <machine/machine_routines.h>
#include <sys/kdebug.h>

struct i386_interrupt_handler {
	IOInterruptHandler	handler;
	void			*nub;
	void			*target;
	void			*refCon;
};

typedef struct i386_interrupt_handler i386_interrupt_handler_t;

i386_interrupt_handler_t	PE_interrupt_handler;

void PE_platform_interrupt_initialize(void)
{
}

void
PE_incoming_interrupt(int interrupt, void *eip)
{
	boolean_t		save_int;
	i386_interrupt_handler_t	*vector;

	KERNEL_DEBUG_CONSTANT(MACHDBG_CODE(DBG_MACH_EXCP_INTR, 0) | DBG_FUNC_START,
	   0, (unsigned int)eip, 0, 0, 0);

	vector = &PE_interrupt_handler;
	save_int  = ml_set_interrupts_enabled(FALSE);
	vector->handler(vector->target, vector->refCon, vector->nub, interrupt);
	ml_set_interrupts_enabled(save_int);

	KERNEL_DEBUG_CONSTANT(MACHDBG_CODE(DBG_MACH_EXCP_INTR, 0) | DBG_FUNC_END,
	   0, 0, 0, 0, 0);

}

void PE_install_interrupt_handler(void *nub, int source,
				  void *target,
				  IOInterruptHandler handler,
				  void *refCon)
{
	i386_interrupt_handler_t	*vector;

	vector = &PE_interrupt_handler;

	/*vector->source = source; IGNORED */
	vector->handler = handler;
	vector->nub = nub;
	vector->target = target;
	vector->refCon = refCon;
}

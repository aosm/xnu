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
 * Copyright (c) 1992 NeXT Computer, Inc.
 *
 * Intel386 Family:	Selector based access to descriptor tables.
 *
 * HISTORY
 *
 * 2 April 1992 ? at NeXT
 *	Created.
 */
 
#include <architecture/i386/table.h>

#include <machdep/i386/gdt.h>
#include <machdep/i386/idt.h>

static inline
gdt_entry_t *
sel_to_gdt_entry(sel)
sel_t		sel;
{
    return (&gdt[sel.index]);
}

static inline
idt_entry_t *
sel_to_idt_entry(sel)
sel_t		sel;
{
    return (&idt[sel.index]);
}

static inline
ldt_entry_t *
sel_to_ldt_entry(tbl, sel)
ldt_t *		tbl;
sel_t		sel;
{
    return (&tbl[sel.index]);
}

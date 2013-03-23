/*
 * Copyright (c) 2007-2008 Apple Inc. All rights reserved.
 *
 * @APPLE_OSREFERENCE_LICENSE_HEADER_START@
 * 
 * This file contains Original Code and/or Modifications of Original Code
 * as defined in and that are subject to the Apple Public Source License
 * Version 2.0 (the 'License'). You may not use this file except in
 * compliance with the License. The rights granted to you under the License
 * may not be used to create, or enable the creation or redistribution of,
 * unlawful or unlicensed copies of an Apple operating system, or to
 * circumvent, violate, or enable the circumvention or violation of, any
 * terms of an Apple operating system software license agreement.
 * 
 * Please obtain a copy of the License at
 * http://www.opensource.apple.com/apsl/ and read it before using this file.
 * 
 * The Original Code and all software distributed under the License are
 * distributed on an 'AS IS' basis, WITHOUT WARRANTY OF ANY KIND, EITHER
 * EXPRESS OR IMPLIED, AND APPLE HEREBY DISCLAIMS ALL SUCH WARRANTIES,
 * INCLUDING WITHOUT LIMITATION, ANY WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE, QUIET ENJOYMENT OR NON-INFRINGEMENT.
 * Please see the License for the specific language governing rights and
 * limitations under the License.
 * 
 * @APPLE_OSREFERENCE_LICENSE_HEADER_END@
 */
#include <string.h>
#include <mach/boolean.h>
#include <mach/machine.h>
#include <sys/types.h>

#if KERNEL
    #include <libkern/libkern.h>
#else
    #include <libkern/OSByteOrder.h>
    #include <stdlib.h>
#endif

#define DEBUG_ASSERT_COMPONENT_NAME_STRING "kxld"
#include <AssertMacros.h>

#include "kxld_array.h"
#include "kxld_reloc.h"
#include "kxld_sect.h"
#include "kxld_seg.h"
#include "kxld_sym.h"
#include "kxld_symtab.h"
#include "kxld_util.h"

/* include target-specific relocation prototypes */
#include <mach-o/reloc.h>
#if KXLD_USER_OR_PPC
#include <mach-o/ppc/reloc.h>
#endif
#if KXLD_USER_OR_X86_64
#include <mach-o/x86_64/reloc.h>
#endif
#if KXLD_USER_OR_ARM
#include <mach-o/arm/reloc.h>
#endif

#define KXLD_TARGET_NONE        (u_int) 0x0
#define KXLD_TARGET_VALUE       (u_int) 0x1
#define KXLD_TARGET_SECTNUM     (u_int) 0x2
#define KXLD_TARGET_SYMBOLNUM   (u_int) 0x3
#define KXLD_TARGET_LOOKUP      (u_int) 0x4
#define KXLD_TARGET_GOT         (u_int) 0x5

#define ABSOLUTE_VALUE(x) (((x) < 0) ? -(x) : (x))

#define LO16(x) (0x0000FFFF & x)
#define LO16S(x) ((0x0000FFFF & x) << 16)
#define HI16(x) (0xFFFF0000 & x)
#define HI16S(x) ((0xFFFF0000 & x) >> 16)
#define BIT15(x) (0x00008000 & x)
#define BR14I(x) (0xFFFF0003 & x)
#define BR14D(x) (0x0000FFFC & x)
#define BR24I(x) (0xFC000003 & x)
#define BR24D(x) (0x03FFFFFC & x)
#define HADISP 0x00010000
#define BR14_LIMIT 0x00008000
#define BR24_LIMIT 0x02000000
#define IS_COND_BR_INSTR(x) ((x & 0xFC000000) == 0x40000000)
#define IS_NOT_ALWAYS_TAKEN(x) ((x & 0x03E00000) != 0x02800000)
#define FLIP_PREDICT_BIT(x) x ^= 0x00200000

#define SIGN_EXTEND_MASK(n) (1 << ((n) - 1))
#define SIGN_EXTEND(x,n) (((x) ^ SIGN_EXTEND_MASK(n)) - SIGN_EXTEND_MASK(n))
#define BR14_NBITS_DISPLACEMENT 16
#define BR24_NBITS_DISPLACEMENT 26

#define X86_64_RIP_RELATIVE_LIMIT 0x80000000UL

/*******************************************************************************
* Prototypes
*******************************************************************************/
#if KXLD_USER_OR_I386
static boolean_t generic_reloc_has_pair(u_int _type) 
    __attribute__((const));
static boolean_t generic_reloc_is_pair(u_int _type, u_int _prev_type)
    __attribute__((const));
static boolean_t generic_reloc_has_got(u_int _type)
    __attribute__((const));
static kern_return_t generic_process_reloc(u_char *instruction, u_int length, 
    u_int pcrel, kxld_addr_t base_pc, kxld_addr_t link_pc, kxld_addr_t link_disp,
    u_int type, kxld_addr_t target, kxld_addr_t pair_target, boolean_t swap);
#endif /* KXLD_USER_OR_I386 */

#if KXLD_USER_OR_PPC 
static boolean_t ppc_reloc_has_pair(u_int _type) 
    __attribute__((const));
static boolean_t ppc_reloc_is_pair(u_int _type, u_int _prev_type) 
    __attribute__((const));
static boolean_t ppc_reloc_has_got(u_int _type)
    __attribute__((const));
static kern_return_t ppc_process_reloc(u_char *instruction, u_int length, 
    u_int pcrel, kxld_addr_t base_pc, kxld_addr_t link_pc, kxld_addr_t link_disp,
    u_int type, kxld_addr_t target, kxld_addr_t pair_target, boolean_t swap);
#endif /* KXLD_USER_OR_PPC */

#if KXLD_USER_OR_X86_64 
static boolean_t x86_64_reloc_has_pair(u_int _type) 
    __attribute__((const));
static boolean_t x86_64_reloc_is_pair(u_int _type, u_int _prev_type) 
    __attribute__((const));
static boolean_t x86_64_reloc_has_got(u_int _type)
    __attribute__((const));
static kern_return_t x86_64_process_reloc(u_char *instruction, u_int length, 
    u_int pcrel, kxld_addr_t base_pc, kxld_addr_t link_pc, kxld_addr_t link_disp,
    u_int type, kxld_addr_t target, kxld_addr_t pair_target, boolean_t swap);
static kern_return_t calculate_displacement_x86_64(uint64_t target, 
    uint64_t adjustment, int32_t *instr32);
#endif /* KXLD_USER_OR_X86_64 */

#if KXLD_USER_OR_ARM
static boolean_t arm_reloc_has_pair(u_int _type) 
    __attribute__((const));
static boolean_t arm_reloc_is_pair(u_int _type, u_int _prev_type) 
    __attribute__((const));
static boolean_t arm_reloc_has_got(u_int _type)
    __attribute__((const));
static kern_return_t arm_process_reloc(u_char *instruction, u_int length, 
    u_int pcrel, kxld_addr_t base_pc, kxld_addr_t link_pc, kxld_addr_t link_disp,
    u_int type, kxld_addr_t target, kxld_addr_t pair_target, boolean_t swap);
#endif /* KXLD_USER_OR_ARM */

#if KXLD_USER_OR_ILP32
static kxld_addr_t get_pointer_at_addr_32(u_char *data, u_long offset,
    const KXLDRelocator *relocator __unused)
    __attribute__((pure, nonnull));
#endif /* KXLD_USER_OR_ILP32 */
#if KXLD_USER_OR_LP64
static kxld_addr_t get_pointer_at_addr_64(u_char *data, u_long offset,
    const KXLDRelocator *relocator __unused)
    __attribute__((pure, nonnull));
#endif /* KXLD_USER_OR_LP64 */

static u_int count_relocatable_relocs(const KXLDRelocator *relocator, 
    const struct relocation_info *relocs, u_int nrelocs)
    __attribute__((pure));

static kern_return_t calculate_targets(kxld_addr_t *_target, 
    kxld_addr_t *_pair_target, const KXLDReloc *reloc, 
    const KXLDArray *sectarray, const KXLDSymtab *symtab);
static kern_return_t get_target_by_address_lookup(kxld_addr_t *target, 
    kxld_addr_t addr, const KXLDArray *sectarray);

/*******************************************************************************
*******************************************************************************/
kern_return_t 
kxld_relocator_init(KXLDRelocator *relocator, cpu_type_t cputype, 
    cpu_subtype_t cpusubtype __unused, boolean_t swap)
{
    kern_return_t rval = KERN_FAILURE;

    check(relocator);
    
    switch(cputype) {
#if KXLD_USER_OR_I386
    case CPU_TYPE_I386:
        relocator->reloc_has_pair = generic_reloc_has_pair;
        relocator->reloc_is_pair = generic_reloc_is_pair;
        relocator->reloc_has_got = generic_reloc_has_got;
        relocator->process_reloc = generic_process_reloc;
        relocator->is_32_bit = TRUE;
        break;
#endif /* KXLD_USER_OR_I386 */
#if KXLD_USER_OR_PPC
    case CPU_TYPE_POWERPC:
        relocator->reloc_has_pair = ppc_reloc_has_pair;
        relocator->reloc_is_pair = ppc_reloc_is_pair;
        relocator->reloc_has_got = ppc_reloc_has_got;
        relocator->process_reloc = ppc_process_reloc;
        relocator->is_32_bit = TRUE;
        break;
#endif /* KXLD_USER_OR_PPC */
#if KXLD_USER_OR_X86_64
    case CPU_TYPE_X86_64:
        relocator->reloc_has_pair = x86_64_reloc_has_pair;
        relocator->reloc_is_pair = x86_64_reloc_is_pair;
        relocator->reloc_has_got = x86_64_reloc_has_got;
        relocator->process_reloc = x86_64_process_reloc;
        relocator->is_32_bit = FALSE;
        break;
#endif /* KXLD_USER_OR_X86_64 */
#if KXLD_USER_OR_ARM
    case CPU_TYPE_ARM:
        relocator->reloc_has_pair = arm_reloc_has_pair;
        relocator->reloc_is_pair = arm_reloc_is_pair;
        relocator->reloc_has_got = arm_reloc_has_got;
        relocator->process_reloc = arm_process_reloc;
        relocator->is_32_bit = TRUE;
        break;
#endif /* KXLD_USER_OR_ARM */
    default:
        rval = KERN_FAILURE;
        kxld_log(kKxldLogLinking, kKxldLogErr,
            kKxldLogArchNotSupported, cputype);
        goto finish;
    }

    relocator->is_32_bit = kxld_is_32_bit(cputype);
    relocator->swap = swap;

    rval = KERN_SUCCESS;

finish:
    return rval;
}

/*******************************************************************************
*******************************************************************************/
kern_return_t
kxld_reloc_create_macho(KXLDArray *relocarray, const KXLDRelocator *relocator, 
    const struct relocation_info *srcs, u_int nsrcs)
{
    kern_return_t rval = KERN_FAILURE;
    KXLDReloc *reloc = NULL;
    u_int nrelocs = 0;
    const struct relocation_info *src = NULL, *prev_src = NULL;
    const struct scattered_relocation_info *scatsrc = NULL, *prev_scatsrc = NULL;
    u_int i = 0;
    u_int reloc_index = 0;

    check(relocarray);
    check(srcs);

    /* If there are no relocation entries, just return */
    if (!nsrcs) {
        rval = KERN_SUCCESS;
        goto finish;
    }

    /* Count the number of non-pair relocs */
    nrelocs = count_relocatable_relocs(relocator, srcs, nsrcs);

    if (nrelocs) {

        /* Allocate the array of relocation entries */

        rval = kxld_array_init(relocarray, sizeof(KXLDReloc), nrelocs);
        require_noerr(rval, finish);

        /* Initialize the relocation entries */
        
        for (i = 0; i < nsrcs; ++i) {
            src = srcs + i;
            scatsrc = (const struct scattered_relocation_info *) src;

            /* A section-based relocation entry can be skipped for absolute 
             * symbols.
             */

            if (!(src->r_address & R_SCATTERED) && !(src->r_extern) && 
                (R_ABS == src->r_symbolnum))
            {
                continue;
            }
            
            /* Pull out the data from the relocation entries.  The target_type
             * depends on the r_extern bit:
             *  Scattered -> Section Lookup by Address
             *  Local (not extern) -> Section by Index
             *  Extern -> Symbolnum by Index
             */
            reloc = kxld_array_get_item(relocarray, reloc_index++);
            if (src->r_address & R_SCATTERED) {
                reloc->address = scatsrc->r_address;
                reloc->pcrel = scatsrc->r_pcrel;
                reloc->length = scatsrc->r_length;
                reloc->reloc_type = scatsrc->r_type;
                reloc->target = scatsrc->r_value;
                reloc->target_type = KXLD_TARGET_LOOKUP;
            } else {
                reloc->address = src->r_address;
                reloc->pcrel = src->r_pcrel;
                reloc->length = src->r_length;
                reloc->reloc_type = src->r_type;
                reloc->target = src->r_symbolnum;

                if (0 == src->r_extern) {
                    reloc->target_type = KXLD_TARGET_SECTNUM;
                    reloc->target -= 1;
                } else {
                    reloc->target_type = KXLD_TARGET_SYMBOLNUM;
                }
            }
            
            /* Find the pair entry if it exists */

            if (relocator->reloc_has_pair(reloc->reloc_type)) {
                ++i;
                require_action(i < nsrcs, finish, rval=KERN_FAILURE);

                prev_src = src;
                src = srcs + i;
                prev_scatsrc = (const struct scattered_relocation_info *) prev_src;
                scatsrc = (const struct scattered_relocation_info *) src;
                 
                if (src->r_address & R_SCATTERED) {
                    require_action(relocator->reloc_is_pair(
                        scatsrc->r_type, reloc->reloc_type), 
                        finish, rval=KERN_FAILURE);
                    reloc->pair_target = scatsrc->r_value;
                    reloc->pair_target_type = KXLD_TARGET_LOOKUP;
                } else {
                    require_action(relocator->reloc_is_pair(src->r_type, 
                        reloc->reloc_type), finish, rval=KERN_FAILURE);

                    if (src->r_extern) {
                        reloc->pair_target = src->r_symbolnum;
                        reloc->pair_target_type = KXLD_TARGET_SYMBOLNUM;
                    } else {
                        reloc->pair_target = src->r_address;
                        reloc->pair_target_type = KXLD_TARGET_VALUE;
                    }
                }
            } else {
                reloc->pair_target = 0;
                if (relocator->reloc_has_got(reloc->reloc_type)) {
                   reloc->pair_target_type = KXLD_TARGET_GOT;
                } else {
                   reloc->pair_target_type = KXLD_TARGET_NONE;
                }
            }
        }
    }

    rval = KERN_SUCCESS;

finish:
    return rval;
}


/*******************************************************************************
* Relocatable relocs :
*   1) Are not _PAIR_ relocs
*   2) Don't reference N_ABS symbols
*******************************************************************************/
static u_int
count_relocatable_relocs(const KXLDRelocator *relocator, 
    const struct relocation_info *relocs, u_int nrelocs)
{
    u_int num_nonpair_relocs = 0;
    u_int i = 0;
    u_int prev_type = 0;
    const struct relocation_info *reloc = NULL;
    const struct scattered_relocation_info *sreloc = NULL;

    check(relocator);
    check(relocs);

    /* Loop over all of the relocation entries */

    num_nonpair_relocs = 1;
    prev_type = relocs->r_type;
    for (i = 1; i < nrelocs; ++i) {
        reloc = relocs + i;

        if (reloc->r_address & R_SCATTERED) {
            /* A scattered relocation entry is relocatable as long as it's not a
             * pair.
             */
            sreloc = (const struct scattered_relocation_info *) reloc;

            num_nonpair_relocs += 
                (!relocator->reloc_is_pair(sreloc->r_type, prev_type));

            prev_type = sreloc->r_type;
        } else {
            /* A normal relocation entry is relocatable if it is not a pair and
             * if it is not a section-based relocation for an absolute symbol.
             */
            num_nonpair_relocs += 
                !(relocator->reloc_is_pair(reloc->r_type, prev_type)
                 || (0 == reloc->r_extern && R_ABS == reloc->r_symbolnum));

            prev_type = reloc->r_type;
        }

    }
    
    return num_nonpair_relocs;
}

/*******************************************************************************
*******************************************************************************/
void
kxld_relocator_clear(KXLDRelocator *relocator)
{
    bzero(relocator, sizeof(*relocator));
}

/*******************************************************************************
*******************************************************************************/
boolean_t 
kxld_relocator_has_pair(const KXLDRelocator *relocator, u_int r_type)
{
    check(relocator);

    return relocator->reloc_has_pair(r_type);
}

/*******************************************************************************
*******************************************************************************/
boolean_t 
kxld_relocator_is_pair(const KXLDRelocator *relocator, u_int r_type, 
    u_int prev_r_type)
{
    check(relocator);

    return relocator->reloc_is_pair(r_type, prev_r_type);
}

/*******************************************************************************
*******************************************************************************/
boolean_t 
kxld_relocator_has_got(const KXLDRelocator *relocator, u_int r_type)
{
    check(relocator);

    return relocator->reloc_has_got(r_type);
}

/*******************************************************************************
*******************************************************************************/
KXLDSym *
kxld_reloc_get_symbol(const KXLDRelocator *relocator, const KXLDReloc *reloc, 
    u_char *data, const KXLDSymtab *symtab)
{
    KXLDSym *sym = NULL;
    kxld_addr_t value = 0;

    check(reloc);
    check(symtab);

    switch (reloc->target_type) {
    case KXLD_TARGET_SYMBOLNUM:
        sym = kxld_symtab_get_symbol_by_index(symtab, reloc->target);
        break;
    case KXLD_TARGET_SECTNUM:
        if (data) {
            KXLD_3264_FUNC(relocator->is_32_bit, value,
                get_pointer_at_addr_32, get_pointer_at_addr_64,
                data, reloc->address, relocator);
            sym = kxld_symtab_get_cxx_symbol_by_value(symtab, value);           
        }
        break;
    default:
        sym = NULL;
        break;
    }

    return sym;
}

/*******************************************************************************
*******************************************************************************/
kern_return_t
kxld_reloc_get_reloc_index_by_offset(const KXLDArray *relocs, 
    kxld_size_t offset, u_int *idx)
{
    kern_return_t rval = KERN_FAILURE;
    KXLDReloc *reloc = NULL;
    u_int i = 0;

    for (i = 0; i < relocs->nitems; ++i) {
        reloc = kxld_array_get_item(relocs, i);
        if (reloc->address == offset) break;
    }
    
    if (i >= relocs->nitems) {
        rval = KERN_FAILURE;
        goto finish;
    }

    *idx = i;
    rval = KERN_SUCCESS;

finish:
    return rval;
}

/*******************************************************************************
*******************************************************************************/
KXLDReloc *
kxld_reloc_get_reloc_by_offset(const KXLDArray *relocs, kxld_addr_t offset)
{
    kern_return_t rval = KERN_FAILURE;
    KXLDReloc *reloc = NULL;
    u_int i = 0;

    rval = kxld_reloc_get_reloc_index_by_offset(relocs, offset, &i);
    if (rval) goto finish;

    reloc = kxld_array_get_item(relocs, i);
    
finish:
    return reloc;
}

#if KXLD_USER_OR_ILP32
/*******************************************************************************
*******************************************************************************/
static kxld_addr_t
get_pointer_at_addr_32(u_char *data, u_long offset,
    const KXLDRelocator *relocator __unused)
{
    uint32_t addr = 0;
    
    check(relocator);
    check(data);

    addr = *(uint32_t *) (data + offset);
#if !KERNEL
    if (relocator->swap) {
        addr = OSSwapInt32(addr);
    }
#endif

    return (kxld_addr_t) addr;
}
#endif /* KXLD_USER_OR_ILP32 */

#if KXLD_USER_OR_LP64
/*******************************************************************************
*******************************************************************************/
static kxld_addr_t
get_pointer_at_addr_64(u_char *data, u_long offset,
    const KXLDRelocator *relocator __unused)
{
    uint64_t addr = 0;
    
    check(relocator);
    check(data);

    addr = *(uint64_t *) (data + offset);
#if !KERNEL
    if (relocator->swap) {
        addr = OSSwapInt64(addr);
    }
#endif

    return (kxld_addr_t) addr;
}
#endif /* KXLD_USER_OR_LP64 */

/*******************************************************************************
*******************************************************************************/
kern_return_t 
kxld_relocator_process_sect_reloc(const KXLDRelocator *relocator,
    const KXLDReloc *reloc, const struct kxld_sect *sect,
    const KXLDArray *sectarray, const struct kxld_symtab *symtab)
{
    kern_return_t rval = KERN_FAILURE;
    u_char *instruction = NULL;
    kxld_addr_t target = 0;
    kxld_addr_t pair_target = 0;
    kxld_addr_t base_pc = 0;
    kxld_addr_t link_pc = 0;
    kxld_addr_t link_disp = 0;

    check(relocator);
    check(reloc);
    check(sect);
    check(sectarray);
    check(symtab);

    /* Find the instruction */

    instruction = sect->data + reloc->address;

    /* Calculate the target */

    rval = calculate_targets(&target, &pair_target, reloc, sectarray, symtab);
    require_noerr(rval, finish);

    base_pc = reloc->address;
    link_pc = base_pc + sect->link_addr;
    link_disp = sect->link_addr - sect->base_addr;

    /* Relocate */

    rval = relocator->process_reloc(instruction, reloc->length, reloc->pcrel,
        base_pc, link_pc, link_disp, reloc->reloc_type, target, pair_target, 
        relocator->swap);
    require_noerr(rval, finish);
    
    /* Return */

    rval = KERN_SUCCESS;

finish:
    return rval;
}

/*******************************************************************************
*******************************************************************************/
kern_return_t 
kxld_reloc_update_symindex(KXLDReloc *reloc, u_int symindex)
{
    kern_return_t rval = KERN_FAILURE;

    require_action(reloc->target_type == KXLD_TARGET_SYMBOLNUM, 
        finish, rval = KERN_FAILURE);

    reloc->target = symindex;

    rval = KERN_SUCCESS;

finish:
    return rval;
}

/*******************************************************************************
*******************************************************************************/
kern_return_t 
kxld_relocator_process_table_reloc(const KXLDRelocator *relocator,
    const KXLDReloc *reloc, const KXLDSeg *seg, u_char *file, 
    const struct kxld_array *sectarray, const struct kxld_symtab *symtab)
{
    kern_return_t rval = KERN_FAILURE;
    u_char *instruction = NULL;
    kxld_addr_t target = 0;
    kxld_addr_t pair_target = 0;
    kxld_addr_t base_pc = 0;
    kxld_addr_t link_pc = 0;
    kxld_addr_t link_disp = 0;

    check(relocator);
    check(reloc);
    check(file);
    check(sectarray);
    check(symtab);

    /* Find the instruction */

    instruction = file + seg->fileoff + reloc->address;

    /* Calculate the target */

    rval = calculate_targets(&target, &pair_target, reloc, sectarray, symtab);
    require_noerr(rval, finish);

    base_pc = reloc->address;
    link_pc = base_pc + seg->link_addr;
    link_disp = seg->link_addr - seg->base_addr;

    /* Relocate */

    rval = relocator->process_reloc(instruction, reloc->length, reloc->pcrel,
        base_pc, link_pc, link_disp, reloc->reloc_type, target, pair_target, 
        relocator->swap);
    require_noerr(rval, finish);
    
    /* Return */

    rval = KERN_SUCCESS;

finish:
    return rval;
}

/*******************************************************************************
*******************************************************************************/
static kern_return_t
calculate_targets(kxld_addr_t *_target, kxld_addr_t *_pair_target,
    const KXLDReloc *reloc, const KXLDArray *sectarray, const KXLDSymtab *symtab)
{
    kern_return_t rval = KERN_FAILURE;
    const KXLDSect *sect = NULL;
    const KXLDSym *sym = NULL;
    kxld_addr_t target = 0;
    kxld_addr_t pair_target = 0;

    check(_target);
    check(_pair_target);
    check(sectarray);
    check(symtab);
    *_target = 0;
    *_pair_target = 0;

    /* Find the target based on the lookup type */

    switch(reloc->target_type) {
    case KXLD_TARGET_LOOKUP:
        require_action(reloc->pair_target_type == KXLD_TARGET_NONE ||
            reloc->pair_target_type == KXLD_TARGET_LOOKUP ||
            reloc->pair_target_type == KXLD_TARGET_VALUE,
            finish, rval=KERN_FAILURE);

        rval = get_target_by_address_lookup(&target, reloc->target, sectarray);
        require_noerr(rval, finish);

        if (reloc->pair_target_type == KXLD_TARGET_LOOKUP) {
            rval = get_target_by_address_lookup(&pair_target,
                reloc->pair_target, sectarray);
            require_noerr(rval, finish);
        } else if (reloc->pair_target_type == KXLD_TARGET_VALUE) {
            pair_target = reloc->pair_target;
        }
        break;
    case KXLD_TARGET_SECTNUM:
        require_action(reloc->pair_target_type == KXLD_TARGET_NONE ||
            reloc->pair_target_type == KXLD_TARGET_VALUE, 
            finish, rval=KERN_FAILURE);

        /* Get the target's section by section number */
        sect = kxld_array_get_item(sectarray, reloc->target);
        require_action(sect, finish, rval=KERN_FAILURE);

        /* target is the change in the section's address */
        target = sect->link_addr - sect->base_addr;

        if (reloc->pair_target_type) {
            pair_target = reloc->pair_target;
        } else {
            /* x86_64 needs to know when we have a non-external relocation,
             * so we hack that information in here.
             */
            pair_target = TRUE;
        }
        break;
    case KXLD_TARGET_SYMBOLNUM:
        require_action(reloc->pair_target_type == KXLD_TARGET_NONE ||
            reloc->pair_target_type == KXLD_TARGET_GOT ||
            reloc->pair_target_type == KXLD_TARGET_SYMBOLNUM ||
            reloc->pair_target_type == KXLD_TARGET_VALUE, finish,
            rval=KERN_FAILURE);

        /* Get the target's symbol by symbol number */
        sym = kxld_symtab_get_symbol_by_index(symtab, reloc->target);
        require_action(sym, finish, rval=KERN_FAILURE);
        target = sym->link_addr;

        /* Some relocation types need the GOT entry address instead of the
         * symbol's actual address.  These types don't have pair relocation
         * entries, so we store the GOT entry address as the pair target.
         */
        if (reloc->pair_target_type == KXLD_TARGET_VALUE) {
            pair_target = reloc->pair_target;
        } else if (reloc->pair_target_type == KXLD_TARGET_SYMBOLNUM ) {
            sym = kxld_symtab_get_symbol_by_index(symtab, reloc->pair_target);
            require_action(sym, finish, rval=KERN_FAILURE);
            pair_target = sym->link_addr;
        } else if (reloc->pair_target_type == KXLD_TARGET_GOT) {
            pair_target = sym->got_addr;
        }
        break;
    default:
        rval = KERN_FAILURE;
        goto finish;
    }

    *_target = target;
    *_pair_target = pair_target;
    rval = KERN_SUCCESS;

finish:
    return rval;
}

/*******************************************************************************
*******************************************************************************/
static kern_return_t
get_target_by_address_lookup(kxld_addr_t *target, kxld_addr_t addr,
    const KXLDArray *sectarray)
{
    kern_return_t rval = KERN_FAILURE;
    const KXLDSect *sect = NULL;
    kxld_addr_t start = 0;
    kxld_addr_t end = 0;
    u_int i = 0;

    check(target);
    check(sectarray);
    *target = 0;

    for (i = 0; i < sectarray->nitems; ++i) {
        sect = kxld_array_get_item(sectarray, i);
        start = sect->base_addr;
        end = start + sect->size;

        if (start <= addr && addr < end) break;
    }
    require_action(i < sectarray->nitems, finish, 
        rval=KERN_FAILURE);

    *target = sect->link_addr - sect->base_addr;
    rval = KERN_SUCCESS;

finish:
    return rval;
}

#if KXLD_USER_OR_I386 
/*******************************************************************************
*******************************************************************************/
static boolean_t
generic_reloc_has_pair(u_int _type)
{
    enum reloc_type_generic type = _type;

    return (type == GENERIC_RELOC_SECTDIFF || 
        type == GENERIC_RELOC_LOCAL_SECTDIFF);
}

/*******************************************************************************
*******************************************************************************/
static boolean_t 
generic_reloc_is_pair(u_int _type, u_int _prev_type __unused)
{
    enum reloc_type_generic type = _type;

    return (type == GENERIC_RELOC_PAIR);
}

/*******************************************************************************
*******************************************************************************/
static boolean_t generic_reloc_has_got(u_int _type __unused)
{
    return FALSE;
}

/*******************************************************************************
*******************************************************************************/
static kern_return_t 
generic_process_reloc(u_char *instruction, u_int length, u_int pcrel,
    kxld_addr_t _base_pc, kxld_addr_t _link_pc, kxld_addr_t _link_disp __unused, 
    u_int _type, kxld_addr_t _target, kxld_addr_t _pair_target, 
    boolean_t swap __unused)
{
    kern_return_t rval = KERN_FAILURE;
    uint32_t base_pc = (uint32_t) _base_pc;
    uint32_t link_pc = (uint32_t) _link_pc;
    uint32_t *instr_addr = NULL;
    uint32_t instr_data = 0;
    uint32_t target = (uint32_t) _target;
    uint32_t pair_target = (uint32_t) _pair_target;
    enum reloc_type_generic type = _type;

    check(instruction);
    require_action(length == 2, finish, rval=KERN_FAILURE);

    if (pcrel) target = target + base_pc - link_pc;

    instr_addr = (uint32_t *)instruction;
    instr_data = *instr_addr;

#if !KERNEL
    if (swap) instr_data = OSSwapInt32(instr_data);
#endif

    switch (type) {
    case GENERIC_RELOC_VANILLA:
        instr_data += target;
        break;
    case GENERIC_RELOC_SECTDIFF:
    case GENERIC_RELOC_LOCAL_SECTDIFF:
        instr_data = instr_data + target - pair_target;
        break;
    case GENERIC_RELOC_PB_LA_PTR:
        rval = KERN_FAILURE;
        goto finish;
    case GENERIC_RELOC_PAIR:
    default:
        rval = KERN_FAILURE;
        goto finish;
    }

#if !KERNEL
    if (swap) instr_data = OSSwapInt32(instr_data);
#endif

    *instr_addr = instr_data;

    rval = KERN_SUCCESS;

finish:
    return rval;
}
#endif /* KXLD_USER_OR_I386 */

#if KXLD_USER_OR_PPC
/*******************************************************************************
*******************************************************************************/
static boolean_t
ppc_reloc_has_pair(u_int _type)
{
    enum reloc_type_ppc type = _type;

    switch(type) {
    case PPC_RELOC_HI16:
    case PPC_RELOC_LO16:
    case PPC_RELOC_HA16:
    case PPC_RELOC_LO14:
    case PPC_RELOC_JBSR:
    case PPC_RELOC_SECTDIFF:
        return TRUE;
    default:
        return FALSE;
    }
}

/*******************************************************************************
*******************************************************************************/
static boolean_t
ppc_reloc_is_pair(u_int _type, u_int _prev_type __unused)
{
    enum reloc_type_ppc type = _type;

    return (type == PPC_RELOC_PAIR);
}

/*******************************************************************************
*******************************************************************************/
static boolean_t ppc_reloc_has_got(u_int _type __unused)
{
    return FALSE;
}

/*******************************************************************************
*******************************************************************************/
static kern_return_t
ppc_process_reloc(u_char *instruction, u_int length, u_int pcrel,
    kxld_addr_t _base_pc, kxld_addr_t _link_pc, kxld_addr_t _link_disp __unused,
    u_int _type, kxld_addr_t _target, kxld_addr_t _pair_target __unused,
    boolean_t swap __unused)
{
    kern_return_t rval = KERN_FAILURE;
    uint32_t *instr_addr = NULL;
    uint32_t instr_data = 0;
    uint32_t base_pc = (uint32_t) _base_pc;
    uint32_t link_pc = (uint32_t) _link_pc;
    uint32_t target = (uint32_t) _target;
    uint32_t pair_target = (uint32_t) _pair_target;
    int32_t addend = 0;
    int32_t displacement = 0;
    uint32_t difference = 0;
    uint32_t br14_disp_sign = 0;
    enum reloc_type_ppc type = _type;

    check(instruction);
    require_action(length == 2 || length == 3, finish, 
        rval=KERN_FAILURE);

    if (pcrel) displacement = target + base_pc - link_pc;

    instr_addr = (uint32_t *)instruction;
    instr_data = *instr_addr;
    
#if !KERNEL
    if (swap) instr_data = OSSwapInt32(instr_data);
#endif

    switch (type) {
    case PPC_RELOC_VANILLA:
        require_action(!pcrel, finish, rval=KERN_FAILURE);

        instr_data += target;
        break;
    case PPC_RELOC_BR14:
        require_action(pcrel, finish, rval=KERN_FAILURE);

        addend = BR14D(instr_data);
        displacement += SIGN_EXTEND(addend, BR14_NBITS_DISPLACEMENT);
        difference = ABSOLUTE_VALUE(displacement);
        require_action(difference < BR14_LIMIT, finish, 
            rval=KERN_FAILURE;
            kxld_log(kKxldLogLinking, kKxldLogErr, kKxldLogRelocationOverflow));


        br14_disp_sign = BIT15(instr_data);
        instr_data = BR14I(instr_data) | BR14D(displacement);
        
        /* If this is a predicted conditional branch (signified by an
         * instruction length of 3) that is not branch-always, and the sign of
         * the displacement is different after relocation, then flip the y-bit
         * to preserve the branch prediction
         */
        if ((length == 3) && 
            IS_COND_BR_INSTR(instr_data) &&
            IS_NOT_ALWAYS_TAKEN(instr_data) && 
            (BIT15(instr_data) != br14_disp_sign))
        {     
            FLIP_PREDICT_BIT(instr_data);
        }
        break;
    case PPC_RELOC_BR24:
        require_action(pcrel, finish, rval=KERN_FAILURE);

        addend = BR24D(instr_data);
        displacement += SIGN_EXTEND(addend, BR24_NBITS_DISPLACEMENT);
        difference = ABSOLUTE_VALUE(displacement);
        require_action(difference < BR24_LIMIT, finish, 
            rval=KERN_FAILURE;
            kxld_log(kKxldLogLinking, kKxldLogErr, kKxldLogRelocationOverflow));

        instr_data = BR24I(instr_data) | BR24D(displacement);
        break;
    case PPC_RELOC_HI16:
        require_action(!pcrel, finish, rval=KERN_FAILURE);

        target += LO16S(instr_data) | LO16(pair_target);
        instr_data = HI16(instr_data) | HI16S(target);
        break;
    case PPC_RELOC_LO16:
        require_action(!pcrel, finish, rval=KERN_FAILURE);

        target += LO16S(pair_target) | LO16(instr_data);
        instr_data = HI16(instr_data) | LO16(target);
        break;
    case PPC_RELOC_HA16:
        require_action(!pcrel, finish, rval=KERN_FAILURE);

        instr_data -= BIT15(pair_target) ? 1 : 0;
        target += LO16S(instr_data) | LO16(pair_target);
        instr_data = HI16(instr_data) | HI16S(target);
        instr_data += BIT15(target) ? 1 : 0;
        break;
    case PPC_RELOC_JBSR:
        require_action(!pcrel, finish, rval=KERN_FAILURE);

        /* The generated code as written branches to an island that loads the
         * absolute address of the target.  If we can branch to the target 
         * directly with less than 24 bits of displacement, we modify the branch
         * instruction to do so which avoids the cost of the island.
         */

        displacement = target + pair_target - link_pc;
        difference = ABSOLUTE_VALUE(displacement);
        if (difference < BR24_LIMIT) {
            instr_data = BR24I(instr_data) | BR24D(displacement);
        }
        break;
    case PPC_RELOC_SECTDIFF:
        require_action(!pcrel, finish, rval=KERN_FAILURE);
        
        instr_data = instr_data + target - pair_target;
        break;
    case PPC_RELOC_LO14:
    case PPC_RELOC_PB_LA_PTR:
    case PPC_RELOC_HI16_SECTDIFF:
    case PPC_RELOC_LO16_SECTDIFF:
    case PPC_RELOC_HA16_SECTDIFF:
    case PPC_RELOC_LO14_SECTDIFF:
    case PPC_RELOC_LOCAL_SECTDIFF:
        rval = KERN_FAILURE;
        goto finish;
    case PPC_RELOC_PAIR:
    default:
        rval = KERN_FAILURE;
        goto finish;
    }

#if !KERNEL
    if (swap) instr_data = OSSwapInt32(instr_data);
#endif

    *instr_addr = instr_data;

    rval = KERN_SUCCESS;
finish:

    return rval;
}
#endif /* KXLD_USER_OR_PPC */

#if KXLD_USER_OR_X86_64
/*******************************************************************************
*******************************************************************************/
static boolean_t 
x86_64_reloc_has_pair(u_int _type)
{
    enum reloc_type_x86_64 type = _type;

    return (type == X86_64_RELOC_SUBTRACTOR);
}

/*******************************************************************************
*******************************************************************************/
static boolean_t 
x86_64_reloc_is_pair(u_int _type, u_int _prev_type)
{
    enum reloc_type_x86_64 type = _type;
    enum reloc_type_x86_64 prev_type = _prev_type;

    return (x86_64_reloc_has_pair(prev_type) && type == X86_64_RELOC_UNSIGNED);
}

/*******************************************************************************
*******************************************************************************/
static boolean_t 
x86_64_reloc_has_got(u_int _type)
{
    enum reloc_type_x86_64 type = _type;

    return (type == X86_64_RELOC_GOT_LOAD || type == X86_64_RELOC_GOT);
}

/*******************************************************************************
*******************************************************************************/
static kern_return_t 
x86_64_process_reloc(u_char *instruction, u_int length, u_int pcrel,
    kxld_addr_t _base_pc __unused, kxld_addr_t _link_pc, kxld_addr_t _link_disp,
    u_int _type, kxld_addr_t _target, kxld_addr_t _pair_target, 
    boolean_t swap __unused)
{
    kern_return_t rval = KERN_FAILURE;
    enum reloc_type_x86_64 type = _type;
    int32_t *instr32p = NULL;
    int32_t instr32 = 0;
    uint64_t *instr64p = NULL;
    uint64_t instr64 = 0;
    uint64_t target = _target;
    uint64_t pair_target = _pair_target;
    uint64_t link_pc = (uint64_t) _link_pc;
    uint64_t link_disp = (uint64_t) _link_disp;
    uint64_t adjustment = 0;

    check(instruction);
    require_action(length == 2 || length == 3, 
        finish, rval=KERN_FAILURE);

    if (length == 2) {
        instr32p = (int32_t *) instruction;
        instr32 = *instr32p;

#if !KERNEL
        if (swap) instr32 = OSSwapInt32(instr32);
#endif

        /* There are a number of different small adjustments for pc-relative
         * relocation entries.  The general case is to subtract the size of the
         * relocation (represented by the length parameter), and it applies to
         * the GOT types and external SIGNED types.  The non-external signed types
         * have a different adjustment corresponding to the specific type.
         */
        switch (type) {
        case X86_64_RELOC_SIGNED:
            if (pair_target) {
                adjustment = 0;    
                break;
            }
            /* Fall through */
        case X86_64_RELOC_SIGNED_1:
            if (pair_target) {
                adjustment = 1;
                break;
            }
            /* Fall through */
        case X86_64_RELOC_SIGNED_2:
            if (pair_target) {
                adjustment = 2;
                break;
            }
            /* Fall through */
        case X86_64_RELOC_SIGNED_4:
            if (pair_target) {
                adjustment = 4;
                break;
            }
            /* Fall through */
        case X86_64_RELOC_BRANCH:
        case X86_64_RELOC_GOT:
        case X86_64_RELOC_GOT_LOAD:
            adjustment = (1 << length);
            break;
        default:
            break;
        }

        /* Perform the actual relocation.  All of the 32-bit relocations are 
         * pc-relative except for SUBTRACTOR, so a good chunk of the logic is
         * stuck in calculate_displacement_x86_64.  The signed relocations are
         * a special case, because when they are non-external, the instruction
         * already contains the pre-relocation displacement, so we only need to
         * find the difference between how far the PC was relocated, and how
         * far the target is relocated.  Since the target variable already
         * contains the difference between the target's base and link
         * addresses, we add the difference between the PC's base and link
         * addresses to the adjustment variable.  This will yield the
         * appropriate displacement in calculate_displacement.
         */
        switch (type) {
        case X86_64_RELOC_BRANCH:
            require_action(pcrel, finish, rval=KERN_FAILURE);
            adjustment += link_pc;
            break;
        case X86_64_RELOC_SIGNED:
        case X86_64_RELOC_SIGNED_1:
        case X86_64_RELOC_SIGNED_2:
        case X86_64_RELOC_SIGNED_4:
            require_action(pcrel, finish, rval=KERN_FAILURE);
            adjustment += (pair_target) ? (link_disp) : (link_pc);
            break;
        case X86_64_RELOC_GOT:
        case X86_64_RELOC_GOT_LOAD:
            require_action(pcrel, finish, rval=KERN_FAILURE);
            adjustment += link_pc;
            target = pair_target;
            break;
        case X86_64_RELOC_SUBTRACTOR:
            require_action(!pcrel, finish, rval=KERN_FAILURE);
            instr32 = (int32_t) (target - pair_target);
            break;
        case X86_64_RELOC_UNSIGNED:
        default:
            rval = KERN_FAILURE;
            goto finish;
        }

        /* Call calculate_displacement for the pc-relative relocations */
        if (pcrel) {
            rval = calculate_displacement_x86_64(target, adjustment, &instr32); 
            require_noerr(rval, finish);
        }

#if !KERNEL
        if (swap) instr32 = OSSwapInt32(instr32);
#endif

        *instr32p = instr32;
    } else {
        instr64p = (uint64_t *) instruction;
        instr64 = *instr64p;

#if !KERNEL
        if (swap) instr64 = OSSwapInt64(instr64);
#endif

        switch (type) {
        case X86_64_RELOC_UNSIGNED:
            require_action(!pcrel, finish, rval=KERN_FAILURE);
            
            instr64 += target;
            break;
        case X86_64_RELOC_SUBTRACTOR:
            require_action(!pcrel, finish, rval=KERN_FAILURE);

            instr64 = target - pair_target;
            break;
        case X86_64_RELOC_SIGNED_1:
        case X86_64_RELOC_SIGNED_2:
        case X86_64_RELOC_SIGNED_4:
        case X86_64_RELOC_GOT_LOAD:
        case X86_64_RELOC_BRANCH:
        case X86_64_RELOC_SIGNED:
        case X86_64_RELOC_GOT:
        default:
            rval = KERN_FAILURE;
            goto finish;
        }

#if !KERNEL
        if (swap) instr64 = OSSwapInt64(instr64);
#endif

        *instr64p = instr64;
    }

    rval = KERN_SUCCESS;

finish:
    return rval;
}

/*******************************************************************************
*******************************************************************************/
static kern_return_t
calculate_displacement_x86_64(uint64_t target, uint64_t adjustment, 
    int32_t *instr32)
{
    kern_return_t rval = KERN_FAILURE;
    int64_t displacement;
    uint64_t difference;

    displacement = *instr32 + target - adjustment;
    difference = ABSOLUTE_VALUE(displacement);
    require_action(difference < X86_64_RIP_RELATIVE_LIMIT, finish, 
        rval=KERN_FAILURE;
        kxld_log(kKxldLogLinking, kKxldLogErr, kKxldLogRelocationOverflow));

    *instr32 = (int32_t) displacement;
    rval = KERN_SUCCESS;

finish:
    return rval;
}
#endif /* KXLD_USER_OR_X86_64 */

#if KXLD_USER_OR_ARM
/*******************************************************************************
*******************************************************************************/
static boolean_t 
arm_reloc_has_pair(u_int _type)
{
    enum reloc_type_arm type = _type;

    switch(type) {
    case ARM_RELOC_SECTDIFF:
        return TRUE;
    default:
        return FALSE;
    }
    return FALSE;
}

/*******************************************************************************
*******************************************************************************/
static boolean_t 
arm_reloc_is_pair(u_int _type, u_int _prev_type __unused)
{
    enum reloc_type_arm type = _type;

    return (type == ARM_RELOC_PAIR);
}

/*******************************************************************************
*******************************************************************************/
static boolean_t 
arm_reloc_has_got(u_int _type __unused)
{
    return FALSE;
}

/*******************************************************************************
*******************************************************************************/
static kern_return_t 
arm_process_reloc(u_char *instruction, u_int length, u_int pcrel,
    kxld_addr_t _base_pc __unused, kxld_addr_t _link_pc __unused, kxld_addr_t _link_disp __unused,
    u_int _type __unused, kxld_addr_t _target __unused, kxld_addr_t _pair_target __unused, 
    boolean_t swap __unused)
{
    kern_return_t rval = KERN_FAILURE;
    uint32_t *instr_addr = NULL;
    uint32_t instr_data = 0;
    uint32_t base_pc = (uint32_t) _base_pc;
    uint32_t link_pc = (uint32_t) _link_pc;
    uint32_t target = (uint32_t) _target;
    int32_t displacement = 0;
    enum reloc_type_arm type = _type;

    check(instruction);
    require_action(length == 2, finish, rval=KERN_FAILURE);

    if (pcrel) displacement = target + base_pc - link_pc;

    instr_addr = (uint32_t *)instruction;
    instr_data = *instr_addr;
    
#if !KERNEL
    if (swap) instr_data = OSSwapInt32(instr_data);
#endif

    switch (type) {
    case ARM_RELOC_VANILLA:
        require_action(!pcrel, finish, rval=KERN_FAILURE);
        instr_data += target;
        break;

    /*
     * If the displacement is 0 (the offset between the pc and the target has
     * not changed), then we don't need to do anything for BR24 and BR22
     * relocs.  As it turns out, because kexts build with -mlong-calls all
     * relocations currently end up being either vanilla (handled above) or 
     * BR22/BR24 with a displacement of 0.
     * We could handle other displacements here but to keep things simple, we
     * won't until it is needed (at which point the kernelcache will fail to
     * link)
     */
    case ARM_RELOC_BR24:
        require_action(pcrel, finish, rval=KERN_FAILURE);
        require_action(displacement == 0, finish, rval=KERN_FAILURE);
        break;
    case ARM_THUMB_RELOC_BR22:
        require_action(pcrel, finish, rval=KERN_FAILURE);
        require_action(displacement == 0, finish, rval=KERN_FAILURE);
        break;

    case ARM_RELOC_SECTDIFF:
    case ARM_RELOC_LOCAL_SECTDIFF:
    case ARM_RELOC_PB_LA_PTR:
        rval = KERN_FAILURE;
        goto finish;

    case ARM_RELOC_PAIR:
    default:
        rval = KERN_FAILURE;
        goto finish;
    }

#if !KERNEL
    if (swap) instr_data = OSSwapInt32(instr_data);
#endif

    *instr_addr = instr_data;

    rval = KERN_SUCCESS;

finish:
    return rval;
}

#endif /* KXLD_USER_OR_ARM */


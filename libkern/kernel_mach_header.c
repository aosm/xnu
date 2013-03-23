/*
 * Copyright (c) 2000-2008 Apple Inc. All rights reserved.
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
/*
 *	File: libkern/kernel_mach_header.c
 *
 *	Functions for accessing mach-o headers.
 *
 * NOTE:	This file supports only kernel mach headers at the present
 *		time; it's primary use is by kld, and all externally
 *		referenced routines at the present time operate against
 *		the kernel mach header _mh_execute_header, which is the
 *		header for the currently executing kernel. 
 *
 */

#include <vm/vm_map.h>
#include <vm/vm_kern.h>
#include <libkern/kernel_mach_header.h>
#include <string.h>		// from libsa

/*
 * return the last address (first avail)
 *
 * This routine operates against the currently executing kernel only
 */
vm_offset_t
getlastaddr(void)
{
	kernel_segment_command_t	*sgp;
	vm_offset_t		last_addr = 0;
	kernel_mach_header_t *header = &_mh_execute_header;
	unsigned long i;

	sgp = (kernel_segment_command_t *)
		((uintptr_t)header + sizeof(kernel_mach_header_t));
	for (i = 0; i < header->ncmds; i++){
		if (   sgp->cmd == LC_SEGMENT_KERNEL) {
			if (sgp->vmaddr + sgp->vmsize > last_addr)
				last_addr = sgp->vmaddr + sgp->vmsize;
		}
		sgp = (kernel_segment_command_t *)((uintptr_t)sgp + sgp->cmdsize);
	}
	return last_addr;
}

/*
 * Find the UUID load command in the Mach-O headers, and return
 * the address of the UUID blob and size in "*size". If the
 * Mach-O image is missing a UUID, NULL is returned.
 */
void *
getuuidfromheader(kernel_mach_header_t *mhp, unsigned long *size)
{
	struct uuid_command *uuidp;
	unsigned long i;

	uuidp = (struct uuid_command *)
		((uintptr_t)mhp + sizeof(kernel_mach_header_t));
	for(i = 0; i < mhp->ncmds; i++){
		if(uuidp->cmd == LC_UUID) {
			if (size)
				*size = sizeof(uuidp->uuid);

			return (void *)uuidp->uuid;
		}

		uuidp = (struct uuid_command *)((uintptr_t)uuidp + uuidp->cmdsize);
	}

	return NULL;
}

/*
 * This routine returns the a pointer to the data for the named section in the
 * named segment if it exist in the mach header passed to it.  Also it returns
 * the size of the section data indirectly through the pointer size.  Otherwise
 *  it returns zero for the pointer and the size.
 *
 * This routine can operate against any kernel mach header.
 */
void *
getsectdatafromheader(
    kernel_mach_header_t *mhp,
    const char *segname,
    const char *sectname,
    unsigned long *size)
{		
	const kernel_section_t *sp;
	void *result;

	sp = getsectbynamefromheader(mhp, segname, sectname);
	if(sp == (kernel_section_t *)0){
	    *size = 0;
	    return((char *)0);
	}
	*size = sp->size;
	result = (void *)sp->addr; 
	return result;
}

/*
 * This routine returns the a pointer to the data for the named segment
 * if it exist in the mach header passed to it.  Also it returns
 * the size of the segment data indirectly through the pointer size.
 * Otherwise it returns zero for the pointer and the size.
 */
void *
getsegdatafromheader(
    kernel_mach_header_t *mhp,
	const char *segname,
	unsigned long *size)
{
	const kernel_segment_command_t *sc;
	void *result;

	sc = getsegbynamefromheader(mhp, segname);
	if(sc == (kernel_segment_command_t *)0){
	    *size = 0;
	    return((char *)0);
	}
	*size = sc->vmsize;
	result = (void *)sc->vmaddr;
	return result;
}

/*
 * This routine returns the section structure for the named section in the
 * named segment for the mach_header pointer passed to it if it exist.
 * Otherwise it returns zero.
 *
 * This routine can operate against any kernel mach header.
 */
kernel_section_t *
getsectbynamefromheader(
    kernel_mach_header_t *mhp,
    const char *segname,
    const char *sectname)
{
	kernel_segment_command_t *sgp;
	kernel_section_t *sp;
	unsigned long i, j;

	sgp = (kernel_segment_command_t *)
	      ((uintptr_t)mhp + sizeof(kernel_mach_header_t));
	for(i = 0; i < mhp->ncmds; i++){
	    if(sgp->cmd == LC_SEGMENT_KERNEL)
		if(strncmp(sgp->segname, segname, sizeof(sgp->segname)) == 0 ||
		   mhp->filetype == MH_OBJECT){
		    sp = (kernel_section_t *)((uintptr_t)sgp +
			 sizeof(kernel_segment_command_t));
		    for(j = 0; j < sgp->nsects; j++){
			if(strncmp(sp->sectname, sectname,
			   sizeof(sp->sectname)) == 0 &&
			   strncmp(sp->segname, segname,
			   sizeof(sp->segname)) == 0)
			    return(sp);
			sp = (kernel_section_t *)((uintptr_t)sp +
			     sizeof(kernel_section_t));
		    }
		}
	    sgp = (kernel_segment_command_t *)((uintptr_t)sgp + sgp->cmdsize);
	}
	return((kernel_section_t *)NULL);
}

/*
 * This routine can operate against any kernel mach header.
 */
kernel_segment_command_t *
getsegbynamefromheader(
	kernel_mach_header_t	*header,
	const char		*seg_name)
{
	kernel_segment_command_t *sgp;
	unsigned long i;

	sgp = (kernel_segment_command_t *)
		((uintptr_t)header + sizeof(kernel_mach_header_t));
	for (i = 0; i < header->ncmds; i++){
		if (   sgp->cmd == LC_SEGMENT_KERNEL
		    && !strncmp(sgp->segname, seg_name, sizeof(sgp->segname)))
			return sgp;
		sgp = (kernel_segment_command_t *)((uintptr_t)sgp + sgp->cmdsize);
	}
	return (kernel_segment_command_t *)NULL;
}

/*
 * Return the first segment_command in the header.
 */
kernel_segment_command_t *
firstseg(void)
{
    return firstsegfromheader(&_mh_execute_header);
}

kernel_segment_command_t *
firstsegfromheader(kernel_mach_header_t *header)
{
    u_int i = 0;
    kernel_segment_command_t *sgp = (kernel_segment_command_t *)
        ((uintptr_t)header + sizeof(*header));

    for (i = 0; i < header->ncmds; i++){
        if (sgp->cmd == LC_SEGMENT_KERNEL)
            return sgp;
        sgp = (kernel_segment_command_t *)((uintptr_t)sgp + sgp->cmdsize);
    }
    return (kernel_segment_command_t *)NULL;
}

/*
 * This routine operates against any kernel mach segment_command structure
 * pointer and the provided kernel header, to obtain the sequentially next
 * segment_command structure in that header.
 */
kernel_segment_command_t *
nextsegfromheader(
        kernel_mach_header_t	*header,
        kernel_segment_command_t	*seg)
{
    u_int i = 0;
    kernel_segment_command_t *sgp = (kernel_segment_command_t *)
        ((uintptr_t)header + sizeof(*header));

    /* Find the index of the passed-in segment */
    for (i = 0; sgp != seg && i < header->ncmds; i++) {
        sgp = (kernel_segment_command_t *)((uintptr_t)sgp + sgp->cmdsize);
    }

    /* Increment to the next load command */
    i++;
    sgp = (kernel_segment_command_t *)((uintptr_t)sgp + sgp->cmdsize);

    /* Return the next segment command, if any */
    for (; i < header->ncmds; i++) {
        if (sgp->cmd == LC_SEGMENT_KERNEL) return sgp;

        sgp = (kernel_segment_command_t *)((uintptr_t)sgp + sgp->cmdsize);
    }

    return (kernel_segment_command_t *)NULL;
}


/*
 * Return the address of the named Mach-O segment from the currently
 * executing kernel kernel, or NULL.
 */
kernel_segment_command_t *
getsegbyname(const char *seg_name)
{
	return(getsegbynamefromheader(&_mh_execute_header, seg_name));
}

/*
 * This routine returns the a pointer the section structure of the named
 * section in the named segment if it exists in the currently executing
 * kernel, which it is presumed to be linked into.  Otherwise it returns NULL.
 */
kernel_section_t *
getsectbyname(
    const char *segname,
    const char *sectname)
{
	return(getsectbynamefromheader(
		(kernel_mach_header_t *)&_mh_execute_header, segname, sectname));
}

/*
 * This routine can operate against any kernel segment_command structure to
 * return the first kernel section immediately following that structure.  If
 * there are no sections associated with the segment_command structure, it
 * returns NULL.
 */
kernel_section_t *
firstsect(kernel_segment_command_t *sgp)
{
	if (!sgp || sgp->nsects == 0)
		return (kernel_section_t *)NULL;

	return (kernel_section_t *)(sgp+1);
}

/*
 * This routine can operate against any kernel segment_command structure and
 * kernel section to return the next consecutive  kernel section immediately
 * following the kernel section provided.  If there are no sections following
 * the provided section, it returns NULL.
 */
kernel_section_t *
nextsect(kernel_segment_command_t *sgp, kernel_section_t *sp)
{
	kernel_section_t *fsp = firstsect(sgp);

	if (((uintptr_t)(sp - fsp) + 1) >= sgp->nsects)
		return (kernel_section_t *)NULL;

	return sp+1;
}

#ifdef MACH_KDB
/*
 * This routine returns the section command for the symbol table in the
 * named segment for the mach_header pointer passed to it if it exist.
 * Otherwise it returns zero.
 */
static struct symtab_command *
getsectcmdsymtabfromheader(
	kernel_mach_header_t *mhp)
{
	kernel_segment_command_t *sgp;
	unsigned long i;

	sgp = (kernel_segment_command_t *)
		((uintptr_t)mhp + sizeof(kernel_mach_header_t));
	for(i = 0; i < mhp->ncmds; i++){
		if(sgp->cmd == LC_SYMTAB)
		return((struct symtab_command *)sgp);
		sgp = (kernel_segment_command_t *)((uintptr_t)sgp + sgp->cmdsize);
	}
	return((struct symtab_command *)NULL);
}

boolean_t getsymtab(kernel_mach_header_t *header,
			vm_offset_t *symtab,
			int *nsyms,
			vm_offset_t *strtab,
			vm_size_t *strtabsize)
{
	kernel_segment_command_t *seglink_cmd;
	struct symtab_command *symtab_cmd;

	seglink_cmd = NULL;
	
	if((header->magic != MH_MAGIC)
	 && (header->magic != MH_MAGIC_64)) {						/* Check if this is a valid header format */
		return (FALSE);									/* Bye y'all... */
	}
	
	seglink_cmd = getsegbynamefromheader(header,"__LINKEDIT");
	if (seglink_cmd == NULL) {
		return(FALSE);
	}

	symtab_cmd = NULL;
	symtab_cmd = getsectcmdsymtabfromheader(header);
	if (symtab_cmd == NULL)
		return(FALSE);

	*nsyms = symtab_cmd->nsyms;
	if(symtab_cmd->nsyms == 0) return (FALSE);	/* No symbols */

	*strtabsize = symtab_cmd->strsize;
	if(symtab_cmd->strsize == 0) return (FALSE);	/* Symbol length is 0 */
	
	*symtab = seglink_cmd->vmaddr + symtab_cmd->symoff -
		seglink_cmd->fileoff;

	*strtab = seglink_cmd->vmaddr + symtab_cmd->stroff -
			seglink_cmd->fileoff;

	return(TRUE);
}
#endif

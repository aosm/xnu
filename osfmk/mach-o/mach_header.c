/*
 * Copyright (c) 2000 Apple Computer, Inc. All rights reserved.
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
 *	File: kern/mach_header.c
 *
 *	Functions for accessing mach-o headers.
 *
 * HISTORY
 * 27-MAR-97  Umesh Vaishampayan (umeshv@NeXT.com)
 *	Added getsegdatafromheader();
 *
 * 29-Jan-92  Mike DeMoney (mike@next.com)
 *	Made into machine independent form from machdep/m68k/mach_header.c.
 *	Ifdef'ed out most of this since I couldn't find any references.
 */

#include <vm/vm_map.h>
#include <vm/vm_kern.h>
#include <mach-o/mach_header.h>

#ifdef __MACHO__

extern struct mach_header _mh_execute_header;

struct section *getsectbynamefromheader(
	struct mach_header	*header,
	char			*seg_name,
	char			*sect_name);
struct segment_command *getsegbynamefromheader(
	struct mach_header	*header,
	char			*seg_name);

/*
 * return the last address (first avail)
 */
#ifdef	MACH_BSD
__private_extern__
#endif
vm_offset_t getlastaddr(void)
{
	struct segment_command	*sgp;
	vm_offset_t		last_addr = 0;
	struct mach_header *header = &_mh_execute_header;
	int i;

	sgp = (struct segment_command *)
		((char *)header + sizeof(struct mach_header));
	for (i = 0; i < header->ncmds; i++){
		if (   sgp->cmd == LC_SEGMENT) {
			if (sgp->vmaddr + sgp->vmsize > last_addr)
				last_addr = sgp->vmaddr + sgp->vmsize;
		}
		sgp = (struct segment_command *)((char *)sgp + sgp->cmdsize);
	}
	return last_addr;
}

#ifdef	XXX_MACH_BSD
__private_extern__
#endif
struct mach_header **
getmachheaders(void)
{
	extern struct mach_header _mh_execute_header;
	struct mach_header **tl;
	
	if (kmem_alloc(kernel_map, (vm_offset_t *) &tl, 2*sizeof(struct mach_header *)) != KERN_SUCCESS)
		return	NULL;

	tl[0] = &_mh_execute_header;
	tl[1] = (struct mach_header *)0;
	return tl;
}

/*
 * This routine returns the a pointer to the data for the named section in the
 * named segment if it exist in the mach header passed to it.  Also it returns
 * the size of the section data indirectly through the pointer size.  Otherwise
 *  it returns zero for the pointer and the size.
 */
#ifdef	MACH_BSD
__private_extern__
#endif
void *
getsectdatafromheader(
    struct mach_header *mhp,
    char *segname,
    char *sectname,
    int *size)
{		
	const struct section *sp;
	void *result;

	sp = getsectbynamefromheader(mhp, segname, sectname);
	if(sp == (struct section *)0){
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
#ifdef	MACH_BSD
__private_extern__
#endif
void *
getsegdatafromheader(
    struct mach_header *mhp,
	char *segname,
	int *size)
{
	const struct segment_command *sc;
	void *result;

	sc = getsegbynamefromheader(mhp, segname);
	if(sc == (struct segment_command *)0){
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
 */
#ifdef	MACH_BSD
__private_extern__
#endif
struct section *
getsectbynamefromheader(
    struct mach_header *mhp,
    char *segname,
    char *sectname)
{
	struct segment_command *sgp;
	struct section *sp;
	long i, j;

	sgp = (struct segment_command *)
	      ((char *)mhp + sizeof(struct mach_header));
	for(i = 0; i < mhp->ncmds; i++){
	    if(sgp->cmd == LC_SEGMENT)
		if(strncmp(sgp->segname, segname, sizeof(sgp->segname)) == 0 ||
		   mhp->filetype == MH_OBJECT){
		    sp = (struct section *)((char *)sgp +
			 sizeof(struct segment_command));
		    for(j = 0; j < sgp->nsects; j++){
			if(strncmp(sp->sectname, sectname,
			   sizeof(sp->sectname)) == 0 &&
			   strncmp(sp->segname, segname,
			   sizeof(sp->segname)) == 0)
			    return(sp);
			sp = (struct section *)((char *)sp +
			     sizeof(struct section));
		    }
		}
	    sgp = (struct segment_command *)((char *)sgp + sgp->cmdsize);
	}
	return((struct section *)0);
}

#ifdef	MACH_BSD
__private_extern__
#endif
struct segment_command *getsegbynamefromheader(
	struct mach_header	*header,
	char			*seg_name)
{
	struct segment_command *sgp;
	int i;

	sgp = (struct segment_command *)
		((char *)header + sizeof(struct mach_header));
	for (i = 0; i < header->ncmds; i++){
		if (   sgp->cmd == LC_SEGMENT
		    && !strncmp(sgp->segname, seg_name, sizeof(sgp->segname)))
			return sgp;
		sgp = (struct segment_command *)((char *)sgp + sgp->cmdsize);
	}
	return (struct segment_command *)0;
}


/*
 * For now at least, all the rest of this seems unused.
 * NOTE: The constant in here for segment alignment is machine-dependent,
 * so if you include this, define a machine dependent constant for it's
 * value.
 */
static struct {
	struct segment_command	seg;
	struct section		sect;
} fvm_data = {
	{
		LC_SEGMENT, 		// cmd
		sizeof(fvm_data),	// cmdsize
		"__USER",		// segname
		0,			// vmaddr
		0,			// vmsize
		0,			// fileoff
		0,			// filesize
		VM_PROT_READ,		// maxprot
		VM_PROT_READ,		// initprot,
		1,			// nsects
		0			// flags
	},
	{
		"",			// sectname
		"__USER",		// segname
		0,			// addr
		0,			// size
		0,			// offset
		4,			// align
		0,			// reloff
		0,			// nreloc
		0			// flags
	}
};

#ifdef	MACH_BSD
static
#endif
struct segment_command *fvm_seg;

static struct fvmfile_command *fvmfilefromheader(struct mach_header *header);
static vm_offset_t getsizeofmacho(struct mach_header *header);

/*
 * Return the first segment_command in the header.
 */
#ifdef	MACH_BSD
__private_extern__
#endif
struct segment_command *firstseg(void)
{
	return firstsegfromheader(&_mh_execute_header);
}

#ifdef	MACH_BSD
__private_extern__
#endif
struct segment_command *firstsegfromheader(struct mach_header *header)
{
	struct segment_command *sgp;
	int i;

	sgp = (struct segment_command *)
		((char *)header + sizeof(struct mach_header));
	for (i = 0; i < header->ncmds; i++){
		if (sgp->cmd == LC_SEGMENT)
			return sgp;
		sgp = (struct segment_command *)((char *)sgp + sgp->cmdsize);
	}
	return (struct segment_command *)0;
}

#ifdef	MACH_BSD
__private_extern__
#endif
struct segment_command *nextseg(struct segment_command *sgp)
{
	struct segment_command *this;

	this = nextsegfromheader(&_mh_execute_header, sgp);

	/*
	 * For the kernel's header add on the faked segment for the
	 * USER boot code identified by a FVMFILE_COMMAND in the mach header.
	 */
	if (!this && sgp != fvm_seg)
		this = fvm_seg;

	return this;
}

#ifdef	MACH_BSD
__private_extern__
#endif
struct segment_command *nextsegfromheader(
	struct mach_header	*header,
	struct segment_command	*seg)
{
	struct segment_command *sgp;
	int i;

	sgp = (struct segment_command *)
		((char *)header + sizeof(struct mach_header));
	for (i = 0; i < header->ncmds; i++) {
		if (sgp == seg)
			break;
		sgp = (struct segment_command *)((char *)sgp + sgp->cmdsize);
	}

	if (i == header->ncmds)
		return (struct segment_command *)0;

	sgp = (struct segment_command *)((char *)sgp + sgp->cmdsize);
	for (; i < header->ncmds; i++) {
		if (sgp->cmd == LC_SEGMENT)
			return sgp;
		sgp = (struct segment_command *)((char *)sgp + sgp->cmdsize);
	}

	return (struct segment_command *)0;
}


/*
 * Return the address of the named Mach-O segment, or NULL.
 */
#ifdef	MACH_BSD
__private_extern__
#endif
struct segment_command *getsegbyname(char *seg_name)
{
	struct segment_command *this;

	this = getsegbynamefromheader(&_mh_execute_header, seg_name);

	/*
	 * For the kernel's header add on the faked segment for the
	 * USER boot code identified by a FVMFILE_COMMAND in the mach header.
	 */
	if (!this && strcmp(seg_name, fvm_seg->segname) == 0)
		this = fvm_seg;

	return this;
}

/*
 * This routine returns the a pointer the section structure of the named
 * section in the named segment if it exist in the mach executable it is
 * linked into.  Otherwise it returns zero.
 */
#ifdef	MACH_BSD
__private_extern__
#endif
struct section *
getsectbyname(
    char *segname,
    char *sectname)
{
	return(getsectbynamefromheader(
		(struct mach_header *)&_mh_execute_header, segname, sectname));
}

#ifdef	MACH_BSD
__private_extern__
#endif
struct section *firstsect(struct segment_command *sgp)
{
	struct section *sp;

	if (!sgp || sgp->nsects == 0)
		return (struct section *)0;

	return (struct section *)(sgp+1);
}

#ifdef	MACH_BSD
__private_extern__
#endif
struct section *nextsect(struct segment_command *sgp, struct section *sp)
{
	struct section *fsp = firstsect(sgp);

	if (sp - fsp >= sgp->nsects-1)
		return (struct section *)0;

	return sp+1;
}

static struct fvmfile_command *fvmfilefromheader(struct mach_header *header)
{
	struct fvmfile_command *fvp;
	int i;

	fvp = (struct fvmfile_command *)
		((char *)header + sizeof(struct mach_header));
	for (i = 0; i < header->ncmds; i++){
		if (fvp->cmd == LC_FVMFILE)
			return fvp;
		fvp = (struct fvmfile_command *)((char *)fvp + fvp->cmdsize);
	}
	return (struct fvmfile_command *)0;
}

/*
 * Create a fake USER seg if a fvmfile_command is present.
 */
#ifdef	MACH_BSD
__private_extern__
#endif
struct segment_command *getfakefvmseg(void)
{
	struct segment_command *sgp = getsegbyname("__USER");
	struct fvmfile_command *fvp = fvmfilefromheader(&_mh_execute_header);
	struct section *sp;

	if (sgp)
		return sgp;

	if (!fvp)
		return (struct segment_command *)0;

	fvm_seg = &fvm_data.seg;
	sgp = fvm_seg;
	sp = &fvm_data.sect;

	sgp->vmaddr = fvp->header_addr;
	sgp->vmsize = getsizeofmacho((struct mach_header *)(sgp->vmaddr));

	strcpy(sp->sectname, fvp->name.ptr);
	sp->addr = sgp->vmaddr;
	sp->size = sgp->vmsize;

#if	DEBUG
	printf("fake fvm seg __USER/\"%s\" at 0x%x, size 0x%x\n",
		sp->sectname, sp->addr, sp->size);
#endif	/*DEBUG*/
	return sgp;
}

/*
 * Figure out the size the size of the data associated with a
 * loaded mach_header.
 */
static vm_offset_t getsizeofmacho(struct mach_header *header)
{
	struct segment_command	*sgp;
	struct section		*sp;
	vm_offset_t		last_addr;

	last_addr = 0;
	for (  sgp = firstsegfromheader(header)
	    ; sgp
	    ; sgp = nextsegfromheader(header, sgp))
	{
		if (sgp->fileoff + sgp->filesize > last_addr)
			last_addr = sgp->fileoff + sgp->filesize;
	}

	return last_addr;
}

#ifdef MACH_KDB
/*
 * This routine returns the section command for the symbol table in the
 * named segment for the mach_header pointer passed to it if it exist.
 * Otherwise it returns zero.
 */
struct symtab_command *
getsectcmdsymtabfromheader(
	struct mach_header *mhp)
{
	struct segment_command *sgp;
	struct section *sp;
	long i;

	sgp = (struct segment_command *)
		((char *)mhp + sizeof(struct mach_header));
	for(i = 0; i < mhp->ncmds; i++){
		if(sgp->cmd == LC_SYMTAB)
		return((struct symtab_command *)sgp);
		sgp = (struct segment_command *)((char *)sgp + sgp->cmdsize);
	}
	return(NULL);
}

boolean_t getsymtab(struct mach_header *header,
			vm_offset_t *symtab,
			int *nsyms,
			vm_offset_t *strtab,
			vm_size_t *strtabsize)
{
	struct segment_command *seglink_cmd;
	struct symtab_command *symtab_cmd;

	seglink_cmd = NULL;
	
	if(header->magic != MH_MAGIC) {						/* Check if this is a valid header format */
		printf("Attempt to use invalid header (magic = %08X) to find symbol table\n", 
			header->magic);								/* Tell them what's wrong */
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

#else

void * getsegdatafromheader( struct mach_header *mhp, char *segname, int *size)
{
	return	0;
}

#endif

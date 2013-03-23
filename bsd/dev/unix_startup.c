/*
 * Copyright (c) 2000-2004 Apple Computer, Inc. All rights reserved.
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
 * Copyright (c) 1992,7 NeXT Computer, Inc.
 *
 * Unix data structure initialization.
 *
 */

#include <mach/mach_types.h>

#include <vm/vm_kern.h>
#include <mach/vm_prot.h>

#include <sys/param.h>
#include <sys/buf_internal.h>
#include <sys/clist.h>
#include <sys/mbuf.h>
#include <sys/systm.h>
#include <sys/tty.h>
#include <sys/vnode.h>
#include <sys/sysctl.h>
#include <dev/ppc/cons.h>
#include <pexpert/pexpert.h>

extern vm_map_t mb_map;

#if INET || INET6
extern u_long   tcp_sendspace;
extern u_long   tcp_recvspace;
#endif

void            bsd_bufferinit(void) __attribute__((section("__TEXT, initcode")));
extern void     md_prepare_for_shutdown(int, int, char *);

int		bsd_mbuf_cluster_reserve(void);

/*
 * Declare these as initialized data so we can patch them.
 */

#ifdef	NBUF
int             max_nbuf_headers = NBUF;
int             niobuf_headers = NBUF / 2;
int 		nbuf_hashelements = NBUF;
int 		nbuf_headers = NBUF;
#else
int             max_nbuf_headers = 0;
int             niobuf_headers = 0;
int 		nbuf_hashelements = 0;
int		nbuf_headers = 0;
#endif

SYSCTL_INT (_kern, OID_AUTO, nbuf, CTLFLAG_RD, &nbuf_headers, 0, "");
SYSCTL_INT (_kern, OID_AUTO, maxnbuf, CTLFLAG_RW, &max_nbuf_headers, 0, "");

__private_extern__ int customnbuf = 0;
int             srv = 0;	/* Flag indicates a server boot when set */
int             ncl = 0;
static unsigned int mbuf_poolsz;

vm_map_t        buffer_map;
vm_map_t        bufferhdr_map;


extern void     bsd_startupearly(void) __attribute__((section("__TEXT, initcode")));

void
bsd_startupearly(void)
{
	vm_offset_t     firstaddr;
	vm_size_t       size;
	kern_return_t   ret;

	/* clip the number of buf headers upto 16k */
	if (max_nbuf_headers == 0)
		max_nbuf_headers = atop(sane_size / 50);	/* Get 2% of ram, but no more than we can map */
	if ((customnbuf == 0) && (max_nbuf_headers > 16384))
		max_nbuf_headers = 16384;
	if (max_nbuf_headers < CONFIG_MIN_NBUF)
		max_nbuf_headers = CONFIG_MIN_NBUF;

	/* clip the number of hash elements  to 200000 */
	if ( (customnbuf == 0 ) && nbuf_hashelements == 0) {
		nbuf_hashelements = atop(sane_size / 50);
		if (nbuf_hashelements > 200000)
			nbuf_hashelements = 200000;
	} else
		nbuf_hashelements = max_nbuf_headers;

	if (niobuf_headers == 0)
		niobuf_headers = max_nbuf_headers;
	if (niobuf_headers > 4096)
		niobuf_headers = 4096;
	if (niobuf_headers < CONFIG_MIN_NIOBUF)
		niobuf_headers = CONFIG_MIN_NIOBUF;

	size = (max_nbuf_headers + niobuf_headers) * sizeof(struct buf);
	size = round_page(size);

	ret = kmem_suballoc(kernel_map,
			    &firstaddr,
			    size,
			    FALSE,
			    VM_FLAGS_ANYWHERE,
			    &bufferhdr_map);

	if (ret != KERN_SUCCESS)
		panic("Failed to create bufferhdr_map");

	ret = kernel_memory_allocate(bufferhdr_map,
				     &firstaddr,
				     size,
				     0,
				     KMA_HERE | KMA_KOBJECT);

	if (ret != KERN_SUCCESS)
		panic("Failed to allocate bufferhdr_map");

	buf_headers = (struct buf *) firstaddr;
	bzero(buf_headers, size);

#if SOCKETS
	{
#if CONFIG_USESOCKTHRESHOLD
		static const unsigned int	maxspace = 64 * 1024;
#else
		static const unsigned int	maxspace = 128 * 1024;
#endif
		int             scale;

		nmbclusters = bsd_mbuf_cluster_reserve() / MCLBYTES;

#if INET || INET6
		if ((scale = nmbclusters / NMBCLUSTERS) > 1) {
			tcp_sendspace *= scale;
			tcp_recvspace *= scale;

			if (tcp_sendspace > maxspace)
				tcp_sendspace = maxspace;
			if (tcp_recvspace > maxspace)
				tcp_recvspace = maxspace;
		}
#endif /* INET || INET6 */
	}
#endif /* SOCKETS */

	/*
	 * Size vnodes based on memory 
	 * Number vnodes  is (memsize/64k) + 1024 
	 * This is the calculation that is used by launchd in tiger
	 * we are clipping the max based on 16G 
	 * ie ((16*1024*1024*1024)/(64 *1024)) + 1024 = 263168;
	 * CONFIG_VNODES is set to 263168 for "medium" configurations (the default)
	 * but can be smaller or larger. 
	 */
	desiredvnodes  = (sane_size/65536) + 1024;
	if (desiredvnodes > CONFIG_VNODES)
		desiredvnodes = CONFIG_VNODES;
}

void
bsd_bufferinit(void)
{
	kern_return_t   ret;

	cons.t_dev = makedev(12, 0);

	bsd_startupearly();

#if SOCKETS
	ret = kmem_suballoc(kernel_map,
			    (vm_offset_t *) & mbutl,
			    (vm_size_t) (nmbclusters * MCLBYTES),
			    FALSE,
			    VM_FLAGS_ANYWHERE,
			    &mb_map);

	if (ret != KERN_SUCCESS)
		panic("Failed to allocate mb_map\n");
#endif /* SOCKETS */

	/*
	 * Set up buffers, so they can be used to read disk labels.
	 */
	bufinit();
}

/* 512 MB hard limit on size of the mbuf pool */
#define MAX_MBUF_POOL   (512 << MBSHIFT)
#define MAX_NCL         (MAX_MBUF_POOL >> MCLSHIFT)

/*
 * this has been broken out into a separate routine that
 * can be called from the x86 early vm initialization to
 * determine how much lo memory to reserve on systems with
 * DMA hardware that can't fully address all of the physical
 * memory that is present.
 */
int
bsd_mbuf_cluster_reserve(void)
{
	/* If called more than once, return the previously calculated size */
        if (mbuf_poolsz != 0)
                goto done;

	PE_parse_boot_argn("ncl", &ncl, sizeof (ncl));

        if (sane_size > (64 * 1024 * 1024) || ncl) {
	        if ((nmbclusters = ncl) == 0) {
		        if ((nmbclusters = ((sane_size / 16)/MCLBYTES)) > 32768)
			        nmbclusters = 32768;
		}
		/* Make sure it's not odd in case ncl is manually set */
		if (nmbclusters & 0x1)
			--nmbclusters;

                /* And obey the upper limit */
                if (nmbclusters > MAX_NCL)
                	nmbclusters = MAX_NCL;

	}
	mbuf_poolsz = nmbclusters << MCLSHIFT;
done:
	return (nmbclusters * MCLBYTES);
}

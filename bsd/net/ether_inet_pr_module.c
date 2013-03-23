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
 * Copyright (c) 1982, 1989, 1993
 *	The Regents of the University of California.  All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. All advertising materials mentioning features or use of this software
 *    must display the following acknowledgement:
 *	This product includes software developed by the University of
 *	California, Berkeley and its contributors.
 * 4. Neither the name of the University nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE REGENTS AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 */



#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/malloc.h>
#include <sys/mbuf.h>
#include <sys/socket.h>
#include <sys/sockio.h>
#include <sys/sysctl.h>

#include <net/if.h>
#include <net/netisr.h>
#include <net/route.h>
#include <net/if_llc.h>
#include <net/if_dl.h>
#include <net/if_types.h>

#include <netinet/in.h>
#include <netinet/in_var.h>
#include <netinet/if_ether.h>
#include <netinet/in_systm.h>
#include <netinet/ip.h>

#include <sys/socketvar.h>

#include <net/dlil.h>

#if LLC && CCITT
extern struct ifqueue pkintrq;
#endif


#if BRIDGE
#include <net/bridge.h>
#endif

/* #include "vlan.h" */
#if NVLAN > 0
#include <net/if_vlan_var.h>
#endif /* NVLAN > 0 */

static u_long lo_dlt = 0;
static ivedonethis = 0;
static u_char	etherbroadcastaddr[6] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };

#define IFP2AC(IFP) ((struct arpcom *)IFP)




/*
 * Process a received Ethernet packet;
 * the packet is in the mbuf chain m without
 * the ether header, which is provided separately.
 */
int
inet_ether_input(m, frame_header, ifp, dl_tag, sync_ok)
    struct mbuf  *m;
    char         *frame_header;
    struct ifnet *ifp;
    u_long	     dl_tag;
    int          sync_ok;

{
    register struct ether_header *eh = (struct ether_header *) frame_header;
    register struct ifqueue *inq=0;
    u_short ether_type;
    int s;
    u_int16_t ptype = -1;
    unsigned char buf[18];

#if ISO || LLC || NETAT
    register struct llc *l;
#endif

    if ((ifp->if_flags & IFF_UP) == 0) {
	 m_freem(m);
	 return EJUSTRETURN;
    }

    ifp->if_lastchange = time;

    if (eh->ether_dhost[0] & 1) {
	if (bcmp((caddr_t)etherbroadcastaddr, (caddr_t)eh->ether_dhost,
		 sizeof(etherbroadcastaddr)) == 0)
	    m->m_flags |= M_BCAST;
	else
	    m->m_flags |= M_MCAST;
    }
    if (m->m_flags & (M_BCAST|M_MCAST))
	ifp->if_imcasts++;

    ether_type = ntohs(eh->ether_type);

#if NVLAN > 0
	if (ether_type == vlan_proto) {
		if (vlan_input(eh, m) < 0)
			ifp->if_data.ifi_noproto++;
		return EJUSTRETURN;
	}
#endif /* NVLAN > 0 */

    switch (ether_type) {

    case ETHERTYPE_IP:
	if (ipflow_fastforward(m))
	    return EJUSTRETURN;
	ptype = mtod(m, struct ip *)->ip_p;
	if ((sync_ok == 0) || 
	    (ptype != IPPROTO_TCP && ptype != IPPROTO_UDP)) {
	    schednetisr(NETISR_IP); 
	}

	inq = &ipintrq;
	break;

    case ETHERTYPE_ARP:
	schednetisr(NETISR_ARP);
	inq = &arpintrq;
	break;

    default: {
	return ENOENT;
	}
    }

    if (inq == 0)
	return ENOENT;

	s = splimp();
	if (IF_QFULL(inq)) {
		IF_DROP(inq);
		m_freem(m);
		splx(s);
		return EJUSTRETURN;
	} else
		IF_ENQUEUE(inq, m);
	splx(s);

    if ((sync_ok) && 
	(ptype == IPPROTO_TCP || ptype == IPPROTO_UDP)) {
	extern void ipintr(void);

	s = splnet();
	ipintr();
	splx(s);
    }

    return 0;
}




int
inet_ether_pre_output(ifp, m0, dst_netaddr, route, type, edst, dl_tag )
    struct ifnet    *ifp;
    struct mbuf     **m0;
    struct sockaddr *dst_netaddr;
    caddr_t	    route;
    char	    *type;
    char            *edst;
    u_long	    dl_tag;
{
    struct rtentry  *rt0 = (struct rtentry *) route;
    int s;
    register struct mbuf *m = *m0;
    register struct rtentry *rt;
    register struct ether_header *eh;
    int off, len = m->m_pkthdr.len;
    int hlen;	/* link layer header lenght */
    struct arpcom *ac = IFP2AC(ifp);



    if ((ifp->if_flags & (IFF_UP|IFF_RUNNING)) != (IFF_UP|IFF_RUNNING)) 
	return ENETDOWN;

    rt = rt0;
    if (rt) {
	if ((rt->rt_flags & RTF_UP) == 0) {
	    rt0 = rt = rtalloc1(dst_netaddr, 1, 0UL);
	    if (rt0)
		rtunref(rt);
	    else
		return EHOSTUNREACH;
	}

	if (rt->rt_flags & RTF_GATEWAY) {
	    if (rt->rt_gwroute == 0)
		goto lookup;
	    if (((rt = rt->rt_gwroute)->rt_flags & RTF_UP) == 0) {
		rtfree(rt); rt = rt0;
	    lookup: rt->rt_gwroute = rtalloc1(rt->rt_gateway, 1,
					      0UL);
		if ((rt = rt->rt_gwroute) == 0)
		    return (EHOSTUNREACH);
	    }
	}

	
	if (rt->rt_flags & RTF_REJECT)
	    if (rt->rt_rmx.rmx_expire == 0 ||
		time_second < rt->rt_rmx.rmx_expire)
		return (rt == rt0 ? EHOSTDOWN : EHOSTUNREACH);
    }

    hlen = ETHER_HDR_LEN;

    /*
     * Tell ether_frameout it's ok to loop packet unless negated below.
     */
    m->m_flags |= M_LOOP;

    switch (dst_netaddr->sa_family) {

    case AF_INET:
	if (!arpresolve(ac, rt, m, dst_netaddr, edst, rt0))
	    return (EJUSTRETURN);	/* if not yet resolved */
	off = m->m_pkthdr.len - m->m_len;
	*(u_short *)type = htons(ETHERTYPE_IP);
	break;

    case AF_UNSPEC:
	m->m_flags &= ~M_LOOP;
	eh = (struct ether_header *)dst_netaddr->sa_data;
	(void)memcpy(edst, eh->ether_dhost, 6);
	*(u_short *)type = eh->ether_type;
	break;

    default:
	kprintf("%s%d: can't handle af%d\n", ifp->if_name, ifp->if_unit,
	       dst_netaddr->sa_family);

        return EAFNOSUPPORT;
    }

    return (0);
}


int
ether_inet_prmod_ioctl(dl_tag, ifp, command, data)
    u_long       dl_tag;
    struct ifnet *ifp;
    int          command;
    caddr_t      data;
{
    struct ifaddr *ifa = (struct ifaddr *) data;
    struct ifreq *ifr = (struct ifreq *) data;
    struct rslvmulti_req *rsreq = (struct rslvmulti_req *) data;
    int error = 0;
    boolean_t funnel_state;
    struct arpcom *ac = (struct arpcom *) ifp;
    struct sockaddr_dl *sdl;
    struct sockaddr_in *sin;
    u_char *e_addr;


#if 0
	/* No tneeded at soo_ioctlis already funnelled */
    funnel_state = thread_funnel_set(network_flock,TRUE);
#endif

    switch (command) {
    case SIOCRSLVMULTI: {
	switch(rsreq->sa->sa_family) {

	case AF_INET:
		sin = (struct sockaddr_in *)rsreq->sa;
		if (!IN_MULTICAST(ntohl(sin->sin_addr.s_addr)))
			return EADDRNOTAVAIL;
		MALLOC(sdl, struct sockaddr_dl *, sizeof *sdl, M_IFMADDR,
		       M_WAITOK);
		sdl->sdl_len = sizeof *sdl;
		sdl->sdl_family = AF_LINK;
		sdl->sdl_index = ifp->if_index;
		sdl->sdl_type = IFT_ETHER;
		sdl->sdl_nlen = 0;
		sdl->sdl_alen = ETHER_ADDR_LEN;
		sdl->sdl_slen = 0;
		e_addr = LLADDR(sdl);
		ETHER_MAP_IP_MULTICAST(&sin->sin_addr, e_addr);
		*rsreq->llsa = (struct sockaddr *)sdl;
		return EJUSTRETURN;

	default:
		/* 
		 * Well, the text isn't quite right, but it's the name
		 * that counts...
		 */
		return EAFNOSUPPORT;
	}

    }
    case SIOCSIFADDR:
	 if ((ifp->if_flags & IFF_RUNNING) == 0) {
	      ifp->if_flags |= IFF_UP;
	      dlil_ioctl(0, ifp, SIOCSIFFLAGS, (caddr_t) 0);
	 }

	 switch (ifa->ifa_addr->sa_family) {

	 case AF_INET:

	    if (ifp->if_init)
		ifp->if_init(ifp->if_softc);	/* before arpwhohas */

	    arp_ifinit(IFP2AC(ifp), ifa);

	    /*
	     * Register new IP and MAC addresses with the kernel debugger
	     * for the en0 interface.
	     */
	    if (ifp->if_unit == 0)
		kdp_set_ip_and_mac_addresses(&(IA_SIN(ifa)->sin_addr), &(IFP2AC(ifp)->ac_enaddr));

	    break;

	default:
	    break;
	}

	break;

    case SIOCGIFADDR:
    {
	struct sockaddr *sa;

	sa = (struct sockaddr *) & ifr->ifr_data;
	bcopy(IFP2AC(ifp)->ac_enaddr,
	      (caddr_t) sa->sa_data, ETHER_ADDR_LEN);
    }
    break;

    case SIOCSIFMTU:
	/*
	 * IOKit IONetworkFamily will set the right MTU according to the driver
	 */

	 return (0);

    default:
	 return EOPNOTSUPP;
    }

    //(void) thread_funnel_set(network_flock, FALSE);

    return (error);
}





int
ether_attach_inet(struct ifnet *ifp, u_long *dl_tag)
{
    struct dlil_proto_reg_str   reg;
    struct dlil_demux_desc      desc;
    struct dlil_demux_desc      desc2;
    u_short en_native=ETHERTYPE_IP;
    u_short arp_native=ETHERTYPE_ARP;
    int   stat;
    int i;


    stat = dlil_find_dltag(ifp->if_family, ifp->if_unit, PF_INET, dl_tag);
    if (stat == 0)
	 return (stat);

    TAILQ_INIT(&reg.demux_desc_head);
    desc.type = DLIL_DESC_RAW;
    desc.variants.bitmask.proto_id_length = 0;
    desc.variants.bitmask.proto_id = 0;
    desc.variants.bitmask.proto_id_mask = 0;
    desc.native_type = (char *) &en_native;
    TAILQ_INSERT_TAIL(&reg.demux_desc_head, &desc, next);
    reg.interface_family = ifp->if_family;
    reg.unit_number      = ifp->if_unit;
    reg.input            = inet_ether_input;
    reg.pre_output       = inet_ether_pre_output;
    reg.event            = 0;
    reg.offer            = 0;
    reg.ioctl            = ether_inet_prmod_ioctl;
    reg.default_proto    = 1;
    reg.protocol_family  = PF_INET;

    desc2 = desc;
    desc2.native_type = (char *) &arp_native;
    TAILQ_INSERT_TAIL(&reg.demux_desc_head, &desc2, next);

    stat = dlil_attach_protocol(&reg, dl_tag);
    if (stat) {
	printf("WARNING: ether_attach_inet can't attach ip to interface\n");
	return stat;
    }
    return (0);
}

int  ether_detach_inet(struct ifnet *ifp, u_long dl_tag)
{
    int         stat;

    stat = dlil_find_dltag(ifp->if_family, ifp->if_unit, PF_INET, &dl_tag);
    if (stat == 0) {
        stat = dlil_detach_protocol(dl_tag);
        if (stat) {
            printf("WARNING: ether_detach_inet can't detach ip from interface\n");
        }
    }
    return stat;
}


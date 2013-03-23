/*
 * Copyright (c) 2000-2010 Apple Inc. All rights reserved.
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
 * Copyright (c) 1982, 1986, 1993
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
 *	@(#)ip_var.h	8.2 (Berkeley) 1/9/95
 */
/*
 * NOTICE: This file was modified by SPARTA, Inc. in 2007 to introduce
 * support for mandatory and extensible security protections.  This notice
 * is included in support of clause 2.2 (b) of the Apple Public License,
 * Version 2.0.
 */

#ifndef _NETINET_IP_VAR_H_
#define	_NETINET_IP_VAR_H_
#include <sys/appleapiopts.h>

/*
 * Overlay for ip header used by other protocols (tcp, udp).
 */
struct ipovly {
	u_char	ih_x1[9];		/* (unused) */
	u_char	ih_pr;			/* protocol */
	u_short	ih_len;			/* protocol length */
	struct	in_addr ih_src;		/* source internet address */
	struct	in_addr ih_dst;		/* destination internet address */
};

#ifdef KERNEL_PRIVATE
#if CONFIG_MACF_NET
struct label;
#endif
/*
 * Ip reassembly queue structure.  Each fragment
 * being reassembled is attached to one of these structures.
 * They are timed out after ipq_ttl drops to 0, and may also
 * be reclaimed if memory becomes tight.
 */
struct ipq {
	struct	ipq *next,*prev;	/* to other reass headers */
	u_char	ipq_ttl;		/* time for reass q to live */
	u_char	ipq_p;			/* protocol of this fragment */
	u_short	ipq_id;			/* sequence id for reassembly */
	struct mbuf *ipq_frags;		/* to ip headers of fragments */
	struct	in_addr ipq_src,ipq_dst;
	u_int32_t	ipq_nfrags;
	TAILQ_ENTRY(ipq) ipq_list;
#if CONFIG_MACF_NET
	struct label *ipq_label;	/* MAC label */
#endif
#if IPDIVERT
#ifdef IPDIVERT_44
	u_int32_t ipq_div_info;		/* ipfw divert port & flags */
#else
	u_int16_t ipq_divert;		/* ipfw divert port (Maintain backward compat.) */
#endif
	u_int16_t ipq_div_cookie;	/* ipfw divert cookie */
#endif
};

/*
 * Structure stored in mbuf in inpcb.ip_options
 * and passed to ip_output when ip options are in use.
 * The actual length of the options (including ipopt_dst)
 * is in m_len.
 */
#endif /* KERNEL_PRIVATE */
#define MAX_IPOPTLEN	40
#ifdef XNU_KERNEL_PRIVATE

struct ipoption {
	struct	in_addr ipopt_dst;	/* first-hop dst if source routed */
	char	ipopt_list[MAX_IPOPTLEN];	/* options proper */
};

/*
 * Structure attached to inpcb.ip_moptions and
 * passed to ip_output when IP multicast options are in use.
 */
struct ip_moptions {
	decl_lck_mtx_data(, imo_lock);
	uint32_t imo_refcnt;		/* ref count */
	uint32_t imo_debug;		/* see ifa_debug flags */
	struct	ifnet *imo_multicast_ifp; /* ifp for outgoing multicasts */
	u_char	imo_multicast_ttl;	/* TTL for outgoing multicasts */
	u_char	imo_multicast_loop;	/* 1 => hear sends if a member */
	u_short	imo_num_memberships;	/* no. memberships this socket */
	u_short	imo_max_memberships;	/* max memberships this socket */
	struct	in_multi **imo_membership;	/* group memberships */
	struct	in_mfilter *imo_mfilters;	/* source filters */
	u_int32_t imo_multicast_vif;	/* vif num outgoing multicasts */
	struct	in_addr imo_multicast_addr; /* ifindex/addr on MULTICAST_IF */
	void (*imo_trace)		/* callback fn for tracing refs */
	    (struct ip_moptions *, int);
};

#define	IMO_LOCK_ASSERT_HELD(_imo)					\
	lck_mtx_assert(&(_imo)->imo_lock, LCK_MTX_ASSERT_OWNED)

#define	IMO_LOCK_ASSERT_NOTHELD(_imo)					\
	lck_mtx_assert(&(_imo)->imo_lock, LCK_MTX_ASSERT_NOTOWNED)

#define	IMO_LOCK(_imo)							\
	lck_mtx_lock(&(_imo)->imo_lock)

#define	IMO_LOCK_SPIN(_imo)						\
	lck_mtx_lock_spin(&(_imo)->imo_lock)

#define	IMO_CONVERT_LOCK(_imo) do {					\
	IMO_LOCK_ASSERT_HELD(_imo);					\
	lck_mtx_convert_spin(&(_imo)->imo_lock);			\
} while (0)

#define	IMO_UNLOCK(_imo)						\
	lck_mtx_unlock(&(_imo)->imo_lock)

#define	IMO_ADDREF(_imo)						\
	imo_addref(_imo, 0)

#define	IMO_ADDREF_LOCKED(_imo)						\
	imo_addref(_imo, 1)

#define	IMO_REMREF(_imo)						\
	imo_remref(_imo)

/* mbuf tag for ip_forwarding info */
struct ip_fwd_tag {
	struct sockaddr_in *next_hop;	/* next_hop */
};

#endif /* XNU_KERNEL_PRIVATE */

struct	ipstat {
	u_int32_t	ips_total;		/* total packets received */
	u_int32_t	ips_badsum;		/* checksum bad */
	u_int32_t	ips_tooshort;		/* packet too short */
	u_int32_t	ips_toosmall;		/* not enough data */
	u_int32_t	ips_badhlen;		/* ip header length < data size */
	u_int32_t	ips_badlen;		/* ip length < ip header length */
	u_int32_t	ips_fragments;		/* fragments received */
	u_int32_t	ips_fragdropped;	/* frags dropped (dups, out of space) */
	u_int32_t	ips_fragtimeout;	/* fragments timed out */
	u_int32_t	ips_forward;		/* packets forwarded */
	u_int32_t	ips_fastforward;	/* packets fast forwarded */
	u_int32_t	ips_cantforward;	/* packets rcvd for unreachable dest */
	u_int32_t	ips_redirectsent;	/* packets forwarded on same net */
	u_int32_t	ips_noproto;		/* unknown or unsupported protocol */
	u_int32_t	ips_delivered;		/* datagrams delivered to upper level*/
	u_int32_t	ips_localout;		/* total ip packets generated here */
	u_int32_t	ips_odropped;		/* lost packets due to nobufs, etc. */
	u_int32_t	ips_reassembled;	/* total packets reassembled ok */
	u_int32_t	ips_fragmented;		/* datagrams successfully fragmented */
	u_int32_t	ips_ofragments;		/* output fragments created */
	u_int32_t	ips_cantfrag;		/* don't fragment flag was set, etc. */
	u_int32_t	ips_badoptions;		/* error in option processing */
	u_int32_t	ips_noroute;		/* packets discarded due to no route */
	u_int32_t	ips_badvers;		/* ip version != 4 */
	u_int32_t	ips_rawout;		/* total raw ip packets generated */
	u_int32_t	ips_toolong;		/* ip length > max ip packet size */
	u_int32_t	ips_notmember;		/* multicasts for unregistered grps */
	u_int32_t	ips_nogif;		/* no match gif found */
	u_int32_t	ips_badaddr;		/* invalid address on header */
#ifdef PRIVATE
	u_int32_t	ips_pktdropcntrl;		/* pkt dropped, no mbufs for control data */
#endif /* PRIVATE */
};

struct ip_linklocal_stat {
	u_int32_t	iplls_in_total;
	u_int32_t	iplls_in_badttl;
	u_int32_t	iplls_out_total;
	u_int32_t	iplls_out_badttl;
};

#ifdef KERNEL_PRIVATE
/* flags passed to ip_output as last parameter */
#define	IP_FORWARDING		0x1		/* most of ip header exists */
#define	IP_RAWOUTPUT		0x2		/* raw ip header exists */
#define	IP_NOIPSEC		0x4		/* No IPSec processing */
#define	IP_ROUTETOIF		SO_DONTROUTE	/* bypass routing tables (0x0010) */
#define	IP_ALLOWBROADCAST	SO_BROADCAST	/* can send broadcast packets (0x0020) */
#define	IP_OUTARGS		0x100		/* has ancillary output info */

struct ip;
struct inpcb;
struct route;
struct sockopt;

/*
 * Extra information passed to ip_output when IP_OUTARGS is set.
 */
struct ip_out_args {
	unsigned int	ipoa_boundif;	/* bound outgoing interface */
	unsigned int	ipoa_nocell;	/* don't use IFT_CELLULAR */
};

extern struct	ipstat	ipstat;
#if !defined(RANDOM_IP_ID) || RANDOM_IP_ID == 0
extern u_short	ip_id;				/* ip packet ctr, for ids */
#endif
extern int	ip_defttl;			/* default IP ttl */
extern int	ipforwarding;			/* ip forwarding */
extern struct protosw *ip_protox[];
extern struct socket *ip_rsvpd;	/* reservation protocol daemon */
extern struct socket *ip_mrouter; /* multicast routing daemon */
extern int	(*legal_vif_num)(int);
extern u_int32_t	(*ip_mcast_src)(int);
extern int rsvp_on;
extern struct	pr_usrreqs rip_usrreqs;
extern int	ip_doscopedroute;

extern void ip_moptions_init(void);
extern struct ip_moptions *ip_allocmoptions(int);
extern int inp_getmoptions(struct inpcb *, struct sockopt *);
extern int inp_setmoptions(struct inpcb *, struct sockopt *);
extern void imo_addref(struct ip_moptions *, int);
extern void imo_remref(struct ip_moptions *);

int	 ip_ctloutput(struct socket *, struct sockopt *sopt);
void	 ip_drain(void);
void	 ip_init(void) __attribute__((section("__TEXT, initcode")));
extern int	 (*ip_mforward)(struct ip *, struct ifnet *, struct mbuf *,
			  struct ip_moptions *);
extern int ip_output(struct mbuf *, struct mbuf *, struct route *, int,
    struct ip_moptions *, struct ip_out_args *);
extern int ip_output_list(struct mbuf *, int, struct mbuf *, struct route *,
    int, struct ip_moptions *, struct ip_out_args *);
struct in_ifaddr *ip_rtaddr(struct in_addr);
int	 ip_savecontrol(struct inpcb *, struct mbuf **, struct ip *,
		struct mbuf *);
void	 ip_slowtimo(void);
struct mbuf *
	 ip_srcroute(void);
void	 ip_stripoptions(struct mbuf *, struct mbuf *);
#if RANDOM_IP_ID
u_int16_t	
	 ip_randomid(void);
#endif
int	rip_ctloutput(struct socket *, struct sockopt *);
void	rip_ctlinput(int, struct sockaddr *, void *);
void	rip_init(void) __attribute__((section("__TEXT, initcode")));
void	rip_input(struct mbuf *, int);
int	rip_output(struct mbuf *, struct socket *, u_int32_t, struct mbuf *);
int	rip_unlock(struct socket *, int, void *);
void	ipip_input(struct mbuf *, int);
void	rsvp_input(struct mbuf *, int);
int	ip_rsvp_init(struct socket *);
int	ip_rsvp_done(void);
int	ip_rsvp_vif_init(struct socket *, struct sockopt *);
int	ip_rsvp_vif_done(struct socket *, struct sockopt *);
void	ip_rsvp_force_done(struct socket *);

void	in_delayed_cksum(struct mbuf *m);

extern void tcp_in_cksum_stats(u_int32_t);
extern void tcp_out_cksum_stats(u_int32_t);

extern void udp_in_cksum_stats(u_int32_t);
extern void udp_out_cksum_stats(u_int32_t);

int rip_send(struct socket *, int , struct mbuf *, struct sockaddr *, 
			struct mbuf *, struct proc *);

extern int ip_fragment(struct mbuf *, struct ifnet *, unsigned long, int);

#endif /* KERNEL_PRIVATE */
#endif /* !_NETINET_IP_VAR_H_ */

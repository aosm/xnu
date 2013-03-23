/*
 * Copyright (c) 2004-2009 Apple Inc. All rights reserved.
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

#include <kern/debug.h>
#include <netinet/in_arp.h>
#include <sys/types.h>
#include <sys/param.h>
#include <sys/kernel_types.h>
#include <sys/syslog.h>
#include <sys/systm.h>
#include <sys/time.h>
#include <sys/kernel.h>
#include <sys/mbuf.h>
#include <sys/sysctl.h>
#include <string.h>
#include <net/if_arp.h>
#include <net/if_dl.h>
#include <net/dlil.h>
#include <net/if_types.h>
#include <net/route.h>
#include <netinet/if_ether.h>
#include <netinet/in_var.h>
#include <kern/zalloc.h>

#define	SA(p) ((struct sockaddr *)(p))
#define SIN(s) ((struct sockaddr_in *)s)
#define CONST_LLADDR(s) ((const u_char*)((s)->sdl_data + (s)->sdl_nlen))
#define	rt_expire rt_rmx.rmx_expire
#define	equal(a1, a2) (bcmp((caddr_t)(a1), (caddr_t)(a2), (a1)->sa_len) == 0)

static const size_t MAX_HW_LEN = 10;

SYSCTL_DECL(_net_link_ether);
SYSCTL_NODE(_net_link_ether, PF_INET, inet, CTLFLAG_RW|CTLFLAG_LOCKED, 0, "");

/* timer values */
static int arpt_prune = (5*60*1); /* walk list every 5 minutes */
static int arpt_keep = (20*60); /* once resolved, good for 20 more minutes */
static int arpt_down = 20;	/* once declared down, don't send for 20 sec */

/* Apple Hardware SUM16 checksuming */
int apple_hwcksum_tx = 1;
int apple_hwcksum_rx = 1;

SYSCTL_INT(_net_link_ether_inet, OID_AUTO, prune_intvl, CTLFLAG_RW,
	   &arpt_prune, 0, "");
SYSCTL_INT(_net_link_ether_inet, OID_AUTO, max_age, CTLFLAG_RW, 
	   &arpt_keep, 0, "");
SYSCTL_INT(_net_link_ether_inet, OID_AUTO, host_down_time, CTLFLAG_RW,
	   &arpt_down, 0, "");
SYSCTL_INT(_net_link_ether_inet, OID_AUTO, apple_hwcksum_tx, CTLFLAG_RW,
	   &apple_hwcksum_tx, 0, "");
SYSCTL_INT(_net_link_ether_inet, OID_AUTO, apple_hwcksum_rx, CTLFLAG_RW,
	   &apple_hwcksum_rx, 0, "");

struct llinfo_arp {
	/*
	 * The following are protected by rnh_lock
	 */
	LIST_ENTRY(llinfo_arp) la_le;
	struct	rtentry *la_rt;
	/*
	 * The following are protected by rt_lock
	 */
	struct	mbuf *la_hold;		/* last packet until resolved/timeout */
	int32_t	la_asked;		/* last time we QUERIED for this addr */
};

/*
 * Synchronization notes:
 *
 * The global list of ARP entries are stored in llinfo_arp; an entry
 * gets inserted into the list when the route is created and gets
 * removed from the list when it is deleted; this is done as part
 * of RTM_ADD/RTM_RESOLVE/RTM_DELETE in arp_rtrequest().
 *
 * Because rnh_lock and rt_lock for the entry are held during those
 * operations, the same locks (and thus lock ordering) must be used
 * elsewhere to access the relevant data structure fields:
 *
 * la_le.{le_next,le_prev}, la_rt
 *
 *	- Routing lock (rnh_lock)
 *
 * la_hold, la_asked
 *
 *	- Routing entry lock (rt_lock)
 *
 * Due to the dependency on rt_lock, llinfo_arp has the same lifetime
 * as the route entry itself.  When a route is deleted (RTM_DELETE),
 * it is simply removed from the global list but the memory is not
 * freed until the route itself is freed.
 */
static LIST_HEAD(, llinfo_arp) llinfo_arp;

static int	arp_inuse, arp_allocated;

static int	arp_maxtries = 5;
static int	useloopback = 1; /* use loopback interface for local traffic */
static int	arp_proxyall = 0;
static int	arp_sendllconflict = 0;

SYSCTL_INT(_net_link_ether_inet, OID_AUTO, maxtries, CTLFLAG_RW,
	   &arp_maxtries, 0, "");
SYSCTL_INT(_net_link_ether_inet, OID_AUTO, useloopback, CTLFLAG_RW,
	   &useloopback, 0, "");
SYSCTL_INT(_net_link_ether_inet, OID_AUTO, proxyall, CTLFLAG_RW,
	   &arp_proxyall, 0, "");
SYSCTL_INT(_net_link_ether_inet, OID_AUTO, sendllconflict, CTLFLAG_RW,
	   &arp_sendllconflict, 0, "");

static int log_arp_warnings = 0;

SYSCTL_INT(_net_link_ether_inet, OID_AUTO, log_arp_warnings, CTLFLAG_RW,
	&log_arp_warnings, 0,
	"log arp warning messages");

static int keep_announcements = 1;
SYSCTL_INT(_net_link_ether_inet, OID_AUTO, keep_announcements, CTLFLAG_RW,
	&keep_announcements, 0,
	"keep arp announcements");

static int send_conflicting_probes = 1;
SYSCTL_INT(_net_link_ether_inet, OID_AUTO, send_conflicting_probes, CTLFLAG_RW,
	&send_conflicting_probes, 0,
	"send conflicting link-local arp probes");

static errno_t arp_lookup_route(const struct in_addr *, int,
    int, route_t *, unsigned int);
static void arptimer(void *);
static struct llinfo_arp *arp_llinfo_alloc(void);
static void arp_llinfo_free(void *);

extern u_int32_t	ipv4_ll_arp_aware;

static int arpinit_done;

static struct zone *llinfo_arp_zone;
#define	LLINFO_ARP_ZONE_MAX	256		/* maximum elements in zone */
#define	LLINFO_ARP_ZONE_NAME	"llinfo_arp"	/* name for zone */

void
arp_init(void)
{
	if (arpinit_done) {
		log(LOG_NOTICE, "arp_init called more than once (ignored)\n");
		return;
	}

	LIST_INIT(&llinfo_arp);

	llinfo_arp_zone = zinit(sizeof (struct llinfo_arp),
	    LLINFO_ARP_ZONE_MAX * sizeof (struct llinfo_arp), 0,
	    LLINFO_ARP_ZONE_NAME);
	if (llinfo_arp_zone == NULL)
		panic("%s: failed allocating llinfo_arp_zone", __func__);

	zone_change(llinfo_arp_zone, Z_EXPAND, TRUE);

	arpinit_done = 1;

	/* start timer */
	timeout(arptimer, (caddr_t)0, hz);
}

static struct llinfo_arp *
arp_llinfo_alloc(void)
{
	return (zalloc(llinfo_arp_zone));
}

static void
arp_llinfo_free(void *arg)
{
	struct llinfo_arp *la = arg;

	if (la->la_le.le_next != NULL || la->la_le.le_prev != NULL) {
		panic("%s: trying to free %p when it is in use", __func__, la);
		/* NOTREACHED */
	}

	/* Just in case there's anything there, free it */
	if (la->la_hold != NULL) {
		m_freem(la->la_hold);
		la->la_hold = NULL;
	}

	zfree(llinfo_arp_zone, la);
}

/*
 * Free an arp entry.
 */
static void
arptfree(struct llinfo_arp *la)
{
	struct rtentry *rt = la->la_rt;
	struct sockaddr_dl *sdl;

	lck_mtx_assert(rnh_lock, LCK_MTX_ASSERT_OWNED);
	RT_LOCK_ASSERT_HELD(rt);

	if (rt->rt_refcnt > 0 && (sdl = SDL(rt->rt_gateway)) &&
	    sdl->sdl_family == AF_LINK) {
		sdl->sdl_alen = 0;
		la->la_asked = 0;
		rt->rt_flags &= ~RTF_REJECT;
		RT_UNLOCK(rt);
	} else {
		/*
		 * Safe to drop rt_lock and use rt_key, since holding
		 * rnh_lock here prevents another thread from calling
		 * rt_setgate() on this route.
		 */
		RT_UNLOCK(rt);
		rtrequest_locked(RTM_DELETE, rt_key(rt), NULL, rt_mask(rt),
		    0, NULL);
	}
}

/*
 * Timeout routine.  Age arp_tab entries periodically.
 */
/* ARGSUSED */
static void
arptimer(void *ignored_arg)
{
#pragma unused (ignored_arg)
	struct llinfo_arp *la, *ola;
	struct timeval timenow;

	lck_mtx_lock(rnh_lock);
	la = llinfo_arp.lh_first;
	getmicrotime(&timenow);
	while ((ola = la) != 0) {
		struct rtentry *rt = la->la_rt;
		la = la->la_le.le_next;
		RT_LOCK(rt);
		if (rt->rt_expire && rt->rt_expire <= timenow.tv_sec)
			arptfree(ola); /* timer has expired, clear */
		else
			RT_UNLOCK(rt);
	}
	lck_mtx_unlock(rnh_lock);
	timeout(arptimer, (caddr_t)0, arpt_prune * hz);
}

/*
 * Parallel to llc_rtrequest.
 */
static void
arp_rtrequest(
	int req,
	struct rtentry *rt,
	__unused struct sockaddr *sa)
{
	struct sockaddr *gate = rt->rt_gateway;
	struct llinfo_arp *la = rt->rt_llinfo;
	static struct sockaddr_dl null_sdl = {sizeof(null_sdl), AF_LINK, 0, 0, 0, 0, 0, {0}};
	struct timeval timenow;

	if (!arpinit_done) {
		panic("%s: ARP has not been initialized", __func__);
		/* NOTREACHED */
	}
	lck_mtx_assert(rnh_lock, LCK_MTX_ASSERT_OWNED);
	RT_LOCK_ASSERT_HELD(rt);

	if (rt->rt_flags & RTF_GATEWAY)
		return;
	getmicrotime(&timenow);
	switch (req) {

	case RTM_ADD:
		/*
		 * XXX: If this is a manually added route to interface
		 * such as older version of routed or gated might provide,
		 * restore cloning bit.
		 */
		if ((rt->rt_flags & RTF_HOST) == 0 &&
		    SIN(rt_mask(rt))->sin_addr.s_addr != 0xffffffff)
			rt->rt_flags |= RTF_CLONING;
		if (rt->rt_flags & RTF_CLONING) {
			/*
			 * Case 1: This route should come from a route to iface.
			 */
			if (rt_setgate(rt, rt_key(rt),
			    (struct sockaddr *)&null_sdl) == 0) {
				gate = rt->rt_gateway;
				SDL(gate)->sdl_type = rt->rt_ifp->if_type;
				SDL(gate)->sdl_index = rt->rt_ifp->if_index;
				/*
				 * In case we're called before 1.0 sec.
				 * has elapsed.
				 */
				rt->rt_expire = MAX(timenow.tv_sec, 1);
			}
			break;
		}
		/* Announce a new entry if requested. */
		if (rt->rt_flags & RTF_ANNOUNCE) {
			RT_UNLOCK(rt);
			dlil_send_arp(rt->rt_ifp, ARPOP_REQUEST,
			    SDL(gate), rt_key(rt), NULL, rt_key(rt));
			RT_LOCK(rt);
		}
		/*FALLTHROUGH*/
	case RTM_RESOLVE:
		if (gate->sa_family != AF_LINK ||
		    gate->sa_len < sizeof(null_sdl)) {
		        if (log_arp_warnings)
				log(LOG_DEBUG, "arp_rtrequest: bad gateway value\n");
			break;
		}
		SDL(gate)->sdl_type = rt->rt_ifp->if_type;
		SDL(gate)->sdl_index = rt->rt_ifp->if_index;
		if (la != 0)
			break; /* This happens on a route change */
		/*
		 * Case 2:  This route may come from cloning, or a manual route
		 * add with a LL address.
		 */
		rt->rt_llinfo = la = arp_llinfo_alloc();
		if (la == NULL) {
			if (log_arp_warnings)
				log(LOG_DEBUG, "%s: malloc failed\n", __func__);
			break;
		}
		rt->rt_llinfo_free = arp_llinfo_free;

		arp_inuse++, arp_allocated++;
		Bzero(la, sizeof(*la));
		la->la_rt = rt;
		rt->rt_flags |= RTF_LLINFO;
		LIST_INSERT_HEAD(&llinfo_arp, la, la_le);

		/*
		 * This keeps the multicast addresses from showing up
		 * in `arp -a' listings as unresolved.  It's not actually
		 * functional.  Then the same for broadcast.
		 */
		if (IN_MULTICAST(ntohl(SIN(rt_key(rt))->sin_addr.s_addr))) {
			RT_UNLOCK(rt);
			dlil_resolve_multi(rt->rt_ifp, rt_key(rt), gate,
			    sizeof(struct sockaddr_dl));
			RT_LOCK(rt);
			rt->rt_expire = 0;
		}
		else if (in_broadcast(SIN(rt_key(rt))->sin_addr, rt->rt_ifp)) {
			struct sockaddr_dl	*gate_ll = SDL(gate);
			size_t	broadcast_len;
			ifnet_llbroadcast_copy_bytes(rt->rt_ifp,
			    LLADDR(gate_ll), sizeof(gate_ll->sdl_data),
			    &broadcast_len);
			gate_ll->sdl_alen = broadcast_len;
			gate_ll->sdl_family = AF_LINK;
			gate_ll->sdl_len = sizeof(struct sockaddr_dl);
			/* In case we're called before 1.0 sec. has elapsed */
			rt->rt_expire = MAX(timenow.tv_sec, 1);
		}

		if (SIN(rt_key(rt))->sin_addr.s_addr ==
		    (IA_SIN(rt->rt_ifa))->sin_addr.s_addr) {
		    /*
		     * This test used to be
		     *	if (loif.if_flags & IFF_UP)
		     * It allowed local traffic to be forced
		     * through the hardware by configuring the loopback down.
		     * However, it causes problems during network configuration
		     * for boards that can't receive packets they send.
		     * It is now necessary to clear "useloopback" and remove
		     * the route to force traffic out to the hardware.
		     */
			rt->rt_expire = 0;
			ifnet_lladdr_copy_bytes(rt->rt_ifp, LLADDR(SDL(gate)), SDL(gate)->sdl_alen = 6);
			if (useloopback)
				rt->rt_ifp = lo_ifp;

		}
		break;

	case RTM_DELETE:
		if (la == 0)
			break;
		arp_inuse--;
		/*
		 * Unchain it but defer the actual freeing until the route
		 * itself is to be freed.  rt->rt_llinfo still points to
		 * llinfo_arp, and likewise, la->la_rt still points to this
		 * route entry, except that RTF_LLINFO is now cleared.
		 */
		LIST_REMOVE(la, la_le);
		la->la_le.le_next = NULL;
		la->la_le.le_prev = NULL;
		rt->rt_flags &= ~RTF_LLINFO;
		if (la->la_hold != NULL)
			m_freem(la->la_hold);
		la->la_hold = NULL;
	}
}

/*
 * convert hardware address to hex string for logging errors.
 */
static const char *
sdl_addr_to_hex(const struct sockaddr_dl *sdl, char * orig_buf, int buflen)
{
	char *		buf = orig_buf;
	int 		i;
	const u_char *	lladdr = (u_char *)(size_t)sdl->sdl_data;
	int			maxbytes = buflen / 3;
	
	if (maxbytes > sdl->sdl_alen) {
		maxbytes = sdl->sdl_alen;
	}	
	*buf = '\0';
	for (i = 0; i < maxbytes; i++) {
		snprintf(buf, 3, "%02x", lladdr[i]);
		buf += 2;
		*buf = (i == maxbytes - 1) ? '\0' : ':';
		buf++;
	}
	return (orig_buf);
}

/*
 * arp_lookup_route will lookup the route for a given address.
 *
 * The address must be for a host on a local network on this interface.
 * If the returned route is non-NULL, the route is locked and the caller
 * is responsible for unlocking it and releasing its reference.
 */
static errno_t
arp_lookup_route(const struct in_addr *addr, int create, int proxy,
    route_t *route, unsigned int ifscope)
{
	struct sockaddr_inarp sin = {sizeof(sin), AF_INET, 0, {0}, {0}, 0, 0};
	const char *why = NULL;
	errno_t	error = 0;
	route_t rt;

	*route = NULL;

	sin.sin_addr.s_addr = addr->s_addr;
	sin.sin_other = proxy ? SIN_PROXY : 0;

	rt = rtalloc1_scoped((struct sockaddr*)&sin, create, 0, ifscope);
	if (rt == NULL)
		return (ENETUNREACH);

	RT_LOCK(rt);

	if (rt->rt_flags & RTF_GATEWAY) {
		why = "host is not on local network";
		error = ENETUNREACH;
	} else if (!(rt->rt_flags & RTF_LLINFO)) {
		why = "could not allocate llinfo";
		error = ENOMEM;
	} else if (rt->rt_gateway->sa_family != AF_LINK) {
		why = "gateway route is not ours";
		error = EPROTONOSUPPORT;
	}

	if (error != 0) {
		if (create && log_arp_warnings) {
			char tmp[MAX_IPv4_STR_LEN];
			log(LOG_DEBUG, "arplookup link#%d %s failed: %s\n",
			    ifscope, inet_ntop(AF_INET, addr, tmp,
			    sizeof (tmp)), why);
		}

		/*
		 * If there are no references to this route, and it is
		 * a cloned route, and not static, and ARP had created
		 * the route, then purge it from the routing table as
		 * it is probably bogus.
		 */
		if (rt->rt_refcnt == 1 &&
		    (rt->rt_flags & (RTF_WASCLONED | RTF_STATIC)) ==
		    RTF_WASCLONED) {
			/*
			 * Prevent another thread from modiying rt_key,
			 * rt_gateway via rt_setgate() after rt_lock is
			 * dropped by marking the route as defunct.
			 */
			rt->rt_flags |= RTF_CONDEMNED;
			RT_UNLOCK(rt);
			rtrequest(RTM_DELETE, rt_key(rt), rt->rt_gateway,
			    rt_mask(rt), rt->rt_flags, 0);
			rtfree(rt);
		} else {
			RT_REMREF_LOCKED(rt);
			RT_UNLOCK(rt);
		}
		return (error);
	}

	/*
	 * Caller releases reference and does RT_UNLOCK(rt).
	 */
	*route = rt;
	return (0);
}

/*
 * arp_route_to_gateway_route will find the gateway route for a given route.
 *
 * If the route is down, look the route up again.
 * If the route goes through a gateway, get the route to the gateway.
 * If the gateway route is down, look it up again.
 * If the route is set to reject, verify it hasn't expired.
 *
 * If the returned route is non-NULL, the caller is responsible for
 * releasing the reference and unlocking the route.
 */
#define senderr(e) { error = (e); goto bad; }
__private_extern__ errno_t
arp_route_to_gateway_route(const struct sockaddr *net_dest, route_t hint0,
     route_t *out_route)
{
	struct timeval timenow;
	route_t rt = hint0, hint = hint0;
	errno_t error = 0;

	*out_route = NULL;

	/*
	 * Next hop determination.  Because we may involve the gateway route
	 * in addition to the original route, locking is rather complicated.
	 * The general concept is that regardless of whether the route points
	 * to the original route or to the gateway route, this routine takes
	 * an extra reference on such a route.  This extra reference will be
	 * released at the end.
	 *
	 * Care must be taken to ensure that the "hint0" route never gets freed
	 * via rtfree(), since the caller may have stored it inside a struct
	 * route with a reference held for that placeholder.
	 */
	if (rt != NULL) {
		unsigned int ifindex;

		RT_LOCK_SPIN(rt);
		ifindex = rt->rt_ifp->if_index;
		RT_ADDREF_LOCKED(rt);
		if (!(rt->rt_flags & RTF_UP)) {
			RT_REMREF_LOCKED(rt);
			RT_UNLOCK(rt);
			/* route is down, find a new one */
			hint = rt = rtalloc1_scoped((struct sockaddr *)
			    (size_t)net_dest, 1, 0, ifindex);
			if (hint != NULL) {
				RT_LOCK_SPIN(rt);
				ifindex = rt->rt_ifp->if_index;
			} else {
				senderr(EHOSTUNREACH);
			}
		}

		/*
		 * We have a reference to "rt" by now; it will either
		 * be released or freed at the end of this routine.
		 */
		RT_LOCK_ASSERT_HELD(rt);
		if (rt->rt_flags & RTF_GATEWAY) {
			struct rtentry *gwrt = rt->rt_gwroute;
			struct sockaddr_in gw;

			/* If there's no gateway rt, look it up */
			if (gwrt == NULL) {
				gw = *((struct sockaddr_in *)rt->rt_gateway);
				RT_UNLOCK(rt);
				goto lookup;
			}
			/* Become a regular mutex */
			RT_CONVERT_LOCK(rt);

			/*
			 * Take gwrt's lock while holding route's lock;
			 * this is okay since gwrt never points back
			 * to "rt", so no lock ordering issues.
			 */
			RT_LOCK_SPIN(gwrt);
			if (!(gwrt->rt_flags & RTF_UP)) {
				struct rtentry *ogwrt;

				rt->rt_gwroute = NULL;
				RT_UNLOCK(gwrt);
				gw = *((struct sockaddr_in *)rt->rt_gateway);
				RT_UNLOCK(rt);
				rtfree(gwrt);
lookup:
				gwrt = rtalloc1_scoped(
				    (struct sockaddr *)&gw, 1, 0, ifindex);

				RT_LOCK(rt);
				/*
				 * Bail out if the route is down, no route
				 * to gateway, circular route, or if the
				 * gateway portion of "rt" has changed.
				 */
				if (!(rt->rt_flags & RTF_UP) ||
				    gwrt == NULL || gwrt == rt ||
				    !equal(SA(&gw), rt->rt_gateway)) {
					if (gwrt == rt) {
						RT_REMREF_LOCKED(gwrt);
						gwrt = NULL;
					}
					RT_UNLOCK(rt);
					if (gwrt != NULL)
						rtfree(gwrt);
					senderr(EHOSTUNREACH);
				}

				/* Remove any existing gwrt */
				ogwrt = rt->rt_gwroute;
				if ((rt->rt_gwroute = gwrt) != NULL)
					RT_ADDREF(gwrt);

				/* Clean up "rt" now while we can */
				if (rt == hint0) {
					RT_REMREF_LOCKED(rt);
					RT_UNLOCK(rt);
				} else {
					RT_UNLOCK(rt);
					rtfree(rt);
				}
				rt = gwrt;
				/* Now free the replaced gwrt */
				if (ogwrt != NULL)
					rtfree(ogwrt);
				/* If still no route to gateway, bail out */
				if (rt == NULL)
					senderr(EHOSTUNREACH);
			} else {
				RT_ADDREF_LOCKED(gwrt);
				RT_UNLOCK(gwrt);
				/* Clean up "rt" now while we can */
				if (rt == hint0) {
					RT_REMREF_LOCKED(rt);
					RT_UNLOCK(rt);
				} else {
					RT_UNLOCK(rt);
					rtfree(rt);
				}
				rt = gwrt;
			}

			/* rt == gwrt; if it is now down, give up */
			RT_LOCK_SPIN(rt);
			if (!(rt->rt_flags & RTF_UP)) {
				RT_UNLOCK(rt);
				senderr(EHOSTUNREACH);
			}
		}

		if (rt->rt_flags & RTF_REJECT) {
			getmicrotime(&timenow);
			if (rt->rt_rmx.rmx_expire == 0 ||
			    timenow.tv_sec < rt->rt_rmx.rmx_expire) {
				RT_UNLOCK(rt);
				senderr(rt == hint ? EHOSTDOWN : EHOSTUNREACH);
			}
		}

		/* Become a regular mutex */
		RT_CONVERT_LOCK(rt);

		/* Caller is responsible for cleaning up "rt" */
		*out_route = rt;
	}
	return (0);

bad:
	/* Clean up route (either it is "rt" or "gwrt") */
	if (rt != NULL) {
		RT_LOCK_SPIN(rt);
		if (rt == hint0) {
			RT_REMREF_LOCKED(rt);
			RT_UNLOCK(rt);
		} else {
			RT_UNLOCK(rt);
			rtfree(rt);
		}
	}
	return (error);
}
#undef senderr

/*
 * This is the ARP pre-output routine; care must be taken to ensure that
 * the "hint" route never gets freed via rtfree(), since the caller may
 * have stored it inside a struct route with a reference held for that
 * placeholder.
 */
errno_t
arp_lookup_ip(ifnet_t ifp, const struct sockaddr_in *net_dest,
    struct sockaddr_dl *ll_dest, size_t	ll_dest_len, route_t hint,
    mbuf_t packet)
{
	route_t	route = NULL;	/* output route */
	errno_t	result = 0;
	struct sockaddr_dl	*gateway;
	struct llinfo_arp	*llinfo;
	struct timeval timenow;

	if (net_dest->sin_family != AF_INET)
		return (EAFNOSUPPORT);

	if ((ifp->if_flags & (IFF_UP|IFF_RUNNING)) != (IFF_UP|IFF_RUNNING))
		return (ENETDOWN);

	/*
	 * If we were given a route, verify the route and grab the gateway
	 */
	if (hint != NULL) {
		/*
		 * Callee holds a reference on the route and returns
		 * with the route entry locked, upon success.
		 */
		result = arp_route_to_gateway_route((const struct sockaddr*)
		    net_dest, hint, &route);
		if (result != 0)
			return (result);
		if (route != NULL)
			RT_LOCK_ASSERT_HELD(route);
	}

	if (packet->m_flags & M_BCAST) {
		size_t	broadcast_len;
		bzero(ll_dest, ll_dest_len);
		result = ifnet_llbroadcast_copy_bytes(ifp, LLADDR(ll_dest),
		    ll_dest_len - offsetof(struct sockaddr_dl, sdl_data),
		    &broadcast_len);
		if (result == 0) {
			ll_dest->sdl_alen = broadcast_len;
			ll_dest->sdl_family = AF_LINK;
			ll_dest->sdl_len = sizeof(struct sockaddr_dl);
		}
		goto release;
	}
	if (packet->m_flags & M_MCAST) {
		if (route != NULL)
			RT_UNLOCK(route);
		result = dlil_resolve_multi(ifp,
		    (const struct sockaddr*)net_dest,
		    (struct sockaddr*)ll_dest, ll_dest_len);
		if (route != NULL)
			RT_LOCK(route);
		goto release;
	}

	/*
	 * If we didn't find a route, or the route doesn't have
	 * link layer information, trigger the creation of the
	 * route and link layer information.
	 */
	if (route == NULL || route->rt_llinfo == NULL) {
		/* Clean up now while we can */
		if (route != NULL) {
			if (route == hint) {
				RT_REMREF_LOCKED(route);
				RT_UNLOCK(route);
			} else {
				RT_UNLOCK(route);
				rtfree(route);
			}
		}
		/*
		 * Callee holds a reference on the route and returns
		 * with the route entry locked, upon success.
		 */
		result = arp_lookup_route(&net_dest->sin_addr, 1, 0, &route,
		    ifp->if_index);
		if (result == 0)
			RT_LOCK_ASSERT_HELD(route);
	}

	if (result || route == NULL || route->rt_llinfo == NULL) {
		char	tmp[MAX_IPv4_STR_LEN];

		/* In case result is 0 but no route, return an error */
		if (result == 0)
			result = EHOSTUNREACH;

		if (log_arp_warnings &&
		    route != NULL && route->rt_llinfo == NULL)
			log(LOG_DEBUG, "arpresolve: can't allocate llinfo "
			    "for %s\n", inet_ntop(AF_INET, &net_dest->sin_addr,
			    tmp, sizeof(tmp)));
		goto release;
	}

	/*
	 * Now that we have the right route, is it filled in?
	 */
	gateway = SDL(route->rt_gateway);
	getmicrotime(&timenow);
	if ((route->rt_rmx.rmx_expire == 0 ||
	    route->rt_rmx.rmx_expire > timenow.tv_sec) && gateway != NULL &&
	    gateway->sdl_family == AF_LINK && gateway->sdl_alen != 0) {
		bcopy(gateway, ll_dest, MIN(gateway->sdl_len, ll_dest_len));
		result = 0;
		goto release;
	}

	if (ifp->if_flags & IFF_NOARP) {
		result = ENOTSUP;
		goto release;
	}

	/*
	 * Route wasn't complete/valid. We need to arp.
	 */
	llinfo = route->rt_llinfo;
	if (packet != NULL) {
		if (llinfo->la_hold != NULL)
			m_freem(llinfo->la_hold);
		llinfo->la_hold = packet;
	}

	if (route->rt_rmx.rmx_expire) {
		route->rt_flags &= ~RTF_REJECT;
		if (llinfo->la_asked == 0 ||
		    route->rt_rmx.rmx_expire != timenow.tv_sec) {
			route->rt_rmx.rmx_expire = timenow.tv_sec;
			if (llinfo->la_asked++ < arp_maxtries) {
				struct ifaddr *rt_ifa = route->rt_ifa;
				ifaref(rt_ifa);
				RT_UNLOCK(route);
				dlil_send_arp(ifp, ARPOP_REQUEST, NULL,
				    rt_ifa->ifa_addr, NULL,
				    (const struct sockaddr*)net_dest);
				ifafree(rt_ifa);
				RT_LOCK(route);
				result = EJUSTRETURN;
				goto release;
			} else {
				route->rt_flags |= RTF_REJECT;
				route->rt_rmx.rmx_expire += arpt_down;
				llinfo->la_asked = 0;
				llinfo->la_hold = NULL;
				result = EHOSTUNREACH;
				goto release;
			}
		}
	}

	/* The packet is now held inside la_hold (can "packet" be NULL?) */
	result = EJUSTRETURN;

release:
	if (route != NULL) {
		if (route == hint) {
			RT_REMREF_LOCKED(route);
			RT_UNLOCK(route);
		} else {
			RT_UNLOCK(route);
			rtfree(route);
		}
	}
	return (result);
}

errno_t
arp_ip_handle_input(
	ifnet_t		ifp,
	u_short		arpop,
	const struct sockaddr_dl *sender_hw,
	const struct sockaddr_in *sender_ip,
	const struct sockaddr_in *target_ip)
{
	char	ipv4str[MAX_IPv4_STR_LEN];
	struct sockaddr_dl proxied;
	struct sockaddr_dl *gateway, *target_hw = NULL;
	struct ifaddr *ifa;
	struct in_ifaddr *ia;
	struct in_ifaddr *best_ia = NULL;
	route_t	route = NULL;
	char buf[3 * MAX_HW_LEN]; // enough for MAX_HW_LEN byte hw address
	struct llinfo_arp *llinfo;
	errno_t	error;
	int created_announcement = 0;
	int bridged = 0, is_bridge = 0;
	
	/* Do not respond to requests for 0.0.0.0 */
	if (target_ip->sin_addr.s_addr == 0 && arpop == ARPOP_REQUEST)
		goto done;
	
 	if (ifp->if_bridge)
		bridged = 1;
	if (ifp->if_type == IFT_BRIDGE)
		is_bridge = 1;

	/*
	 * Determine if this ARP is for us
	 * For a bridge, we want to check the address irrespective 
	 * of the receive interface.
	 */
	lck_rw_lock_shared(in_ifaddr_rwlock);
	TAILQ_FOREACH(ia, INADDR_HASH(target_ip->sin_addr.s_addr), ia_hash) {
		if (((bridged && ia->ia_ifp->if_bridge != NULL) ||
			(ia->ia_ifp == ifp)) &&
		    ia->ia_addr.sin_addr.s_addr == target_ip->sin_addr.s_addr) {
				best_ia = ia;
				ifaref(&best_ia->ia_ifa);
				lck_rw_done(in_ifaddr_rwlock);
				goto match;
		}
	}

	TAILQ_FOREACH(ia, INADDR_HASH(sender_ip->sin_addr.s_addr), ia_hash) {
		if (((bridged && ia->ia_ifp->if_bridge != NULL) ||
			(ia->ia_ifp == ifp)) &&
		    ia->ia_addr.sin_addr.s_addr == sender_ip->sin_addr.s_addr) {
				best_ia = ia;
				ifaref(&best_ia->ia_ifa);
				lck_rw_done(in_ifaddr_rwlock);
				goto match;
		}
	}

#define BDG_MEMBER_MATCHES_ARP(addr, ifp, ia)								\
	(ia->ia_ifp->if_bridge == ifp->if_softc &&								\
	!bcmp(ifnet_lladdr(ia->ia_ifp), ifnet_lladdr(ifp), ifp->if_addrlen) &&	\
	addr == ia->ia_addr.sin_addr.s_addr)
	/*
	 * Check the case when bridge shares its MAC address with
	 * some of its children, so packets are claimed by bridge
	 * itself (bridge_input() does it first), but they are really
	 * meant to be destined to the bridge member.
	 */
	if (is_bridge) {
		TAILQ_FOREACH(ia, INADDR_HASH(target_ip->sin_addr.s_addr), ia_hash) {
			if (BDG_MEMBER_MATCHES_ARP(target_ip->sin_addr.s_addr, ifp, ia)) {
				ifp = ia->ia_ifp;
				best_ia = ia;
				ifaref(&best_ia->ia_ifa);
				lck_rw_done(in_ifaddr_rwlock);
				goto match;
			}
		}
	}
	lck_rw_done(in_ifaddr_rwlock);

	/*
	 * No match, use the first inet address on the receive interface
	 * as a dummy address for the rest of the function; we may be
	 * proxying for another address.
	 */
	ifnet_lock_shared(ifp);
	TAILQ_FOREACH(ifa, &ifp->if_addrhead, ifa_link) {
		if (ifa->ifa_addr->sa_family != AF_INET)
			continue;
		best_ia = (struct in_ifaddr *)ifa;
		ifaref(&best_ia->ia_ifa);
		ifnet_lock_done(ifp);
		goto match;
	}
	ifnet_lock_done(ifp);

	/*
	 * If we're not a bridge member, or if we are but there's no
	 * IPv4 address to use for the interface, drop the packet.
	 */
	if (!bridged || best_ia == NULL)
		goto done;

match:
	/* If the packet is from this interface, ignore the packet */
	if (!bcmp(CONST_LLADDR(sender_hw), ifnet_lladdr(ifp), sender_hw->sdl_len)) {
		goto done;
	}

	/* Check for a conflict */
	if (!bridged && sender_ip->sin_addr.s_addr == best_ia->ia_addr.sin_addr.s_addr) {
		struct kev_msg        ev_msg;
		struct kev_in_collision	*in_collision;
		u_char	storage[sizeof(struct kev_in_collision) + MAX_HW_LEN];
		in_collision = (struct kev_in_collision*)storage;
		log(LOG_ERR, "%s%d duplicate IP address %s sent from address %s\n",
			ifp->if_name, ifp->if_unit,
			inet_ntop(AF_INET, &sender_ip->sin_addr, ipv4str, sizeof(ipv4str)),
			sdl_addr_to_hex(sender_hw, buf, sizeof(buf)));

		/* Send a kernel event so anyone can learn of the conflict */
		in_collision->link_data.if_family = ifp->if_family;
		in_collision->link_data.if_unit = ifp->if_unit;
		strncpy(&in_collision->link_data.if_name[0], ifp->if_name, IFNAMSIZ);
		in_collision->ia_ipaddr = sender_ip->sin_addr;
		in_collision->hw_len = sender_hw->sdl_alen < MAX_HW_LEN ? sender_hw->sdl_alen : MAX_HW_LEN;
		bcopy(CONST_LLADDR(sender_hw), (caddr_t)in_collision->hw_addr, in_collision->hw_len);
		ev_msg.vendor_code = KEV_VENDOR_APPLE;
		ev_msg.kev_class = KEV_NETWORK_CLASS;
		ev_msg.kev_subclass = KEV_INET_SUBCLASS;
		ev_msg.event_code = KEV_INET_ARPCOLLISION;
		ev_msg.dv[0].data_ptr = in_collision;
		ev_msg.dv[0].data_length = sizeof(struct kev_in_collision) + in_collision->hw_len;
		ev_msg.dv[1].data_length = 0;
		kev_post_msg(&ev_msg);

		goto respond;
	}

	/*
	 * Look up the routing entry. If it doesn't exist and we are the
	 * target, and the sender isn't 0.0.0.0, go ahead and create one.
	 * Callee holds a reference on the route and returns with the route
	 * entry locked, upon success.
	 */
	error = arp_lookup_route(&sender_ip->sin_addr,
	    (target_ip->sin_addr.s_addr == best_ia->ia_addr.sin_addr.s_addr &&
	    sender_ip->sin_addr.s_addr != 0), 0, &route, ifp->if_index);

	if (error == 0)
		RT_LOCK_ASSERT_HELD(route);

	if (error || route == 0 || route->rt_gateway == 0) {
		if (arpop != ARPOP_REQUEST) {
			goto respond;
		}
		if (arp_sendllconflict
		    && send_conflicting_probes != 0
		    && (ifp->if_eflags & IFEF_ARPLL) != 0 
		    && IN_LINKLOCAL(ntohl(target_ip->sin_addr.s_addr))
		    && sender_ip->sin_addr.s_addr == 0) {
			/*
			 * Verify this ARP probe doesn't conflict with an IPv4LL we know of
			 * on another interface.
			 */
			if (route != NULL) {
				RT_REMREF_LOCKED(route);
				RT_UNLOCK(route);
				route = NULL;
			}
			/*
			 * Callee holds a reference on the route and returns
			 * with the route entry locked, upon success.
			 */
			error = arp_lookup_route(&target_ip->sin_addr, 0, 0,
			    &route, ifp->if_index);

			if (error == 0)
				RT_LOCK_ASSERT_HELD(route);

			if (error == 0 && route && route->rt_gateway) {
				gateway = SDL(route->rt_gateway);
				if (route->rt_ifp != ifp && gateway->sdl_alen != 0 
				    && (gateway->sdl_alen != sender_hw->sdl_alen 
					|| bcmp(CONST_LLADDR(gateway), CONST_LLADDR(sender_hw),
						gateway->sdl_alen) != 0)) {
					/*
					 * A node is probing for an IPv4LL we know exists on a
					 * different interface. We respond with a conflicting probe
					 * to force the new device to pick a different IPv4LL
					 * address.
					 */
					if (log_arp_warnings) {
					    log(LOG_INFO,
						"arp: %s on %s%d sent probe for %s, already on %s%d\n",
						sdl_addr_to_hex(sender_hw, buf, sizeof(buf)),
						ifp->if_name, ifp->if_unit,
						inet_ntop(AF_INET, &target_ip->sin_addr, ipv4str,
								  sizeof(ipv4str)),
						route->rt_ifp->if_name, route->rt_ifp->if_unit);
					    log(LOG_INFO,
						"arp: sending conflicting probe to %s on %s%d\n",
						sdl_addr_to_hex(sender_hw, buf, sizeof(buf)),
						ifp->if_name, ifp->if_unit);
					}
					/* We're done with the route */
					RT_REMREF_LOCKED(route);
					RT_UNLOCK(route);
					route = NULL;
					/*
					 * Send a conservative unicast "ARP probe".
					 * This should force the other device to pick a new number.
					 * This will not force the device to pick a new number if the device
					 * has already assigned that number.
					 * This will not imply to the device that we own that address.
					 */
					ifnet_lock_shared(ifp);
					ifa = TAILQ_FIRST(&ifp->if_addrhead);
					if (ifa != NULL)
						ifaref(ifa);
					ifnet_lock_done(ifp);
					dlil_send_arp_internal(ifp, ARPOP_REQUEST,
						ifa != NULL ? SDL(ifa->ifa_addr) : NULL,
						(const struct sockaddr*)sender_ip, sender_hw,
						(const struct sockaddr*)target_ip);
					if (ifa != NULL) {
						ifafree(ifa);
						ifa = NULL;
					}
			 	}
			}
			goto respond;
		} else if (keep_announcements != 0
			   && target_ip->sin_addr.s_addr == sender_ip->sin_addr.s_addr) {
			/* don't create entry if link-local address and link-local is disabled */
			if (!IN_LINKLOCAL(ntohl(sender_ip->sin_addr.s_addr)) 
			    || (ifp->if_eflags & IFEF_ARPLL) != 0) {
				if (route != NULL) {
					RT_REMREF_LOCKED(route);
					RT_UNLOCK(route);
					route = NULL;
				}
				/*
				 * Callee holds a reference on the route and
				 * returns with the route entry locked, upon
				 * success.
				 */
				error = arp_lookup_route(&sender_ip->sin_addr,
				    1, 0, &route, ifp->if_index);

				if (error == 0)
					RT_LOCK_ASSERT_HELD(route);

				if (error == 0 && route != NULL && route->rt_gateway != NULL) {
					created_announcement = 1;
				}
			}
			if (created_announcement == 0) {
				goto respond;
			}
		} else {
			goto respond;
		}
	}

	RT_LOCK_ASSERT_HELD(route);
	gateway = SDL(route->rt_gateway);
	if (!bridged && route->rt_ifp != ifp) {
		if (!IN_LINKLOCAL(ntohl(sender_ip->sin_addr.s_addr)) || (ifp->if_eflags & IFEF_ARPLL) == 0) {
			if (log_arp_warnings)
				log(LOG_ERR, "arp: %s is on %s%d but got reply from %s on %s%d\n",
					inet_ntop(AF_INET, &sender_ip->sin_addr, ipv4str,
							  sizeof(ipv4str)),
					route->rt_ifp->if_name,
					route->rt_ifp->if_unit,
					sdl_addr_to_hex(sender_hw, buf, sizeof(buf)),
					ifp->if_name, ifp->if_unit);
			goto respond;
		}
		else {
			/* Don't change a permanent address */
			if (route->rt_rmx.rmx_expire == 0) {
				goto respond;
			}

			/*
			 * We're about to check and/or change the route's ifp
			 * and ifa, so do the lock dance: drop rt_lock, hold
			 * rnh_lock and re-hold rt_lock to avoid violating the
			 * lock ordering.  We have an extra reference on the
			 * route, so it won't go away while we do this.
			 */
			RT_UNLOCK(route);
			lck_mtx_lock(rnh_lock);
			RT_LOCK(route);
			/*
			 * Don't change the cloned route away from the
			 * parent's interface if the address did resolve
			 * or if the route is defunct.  rt_ifp on both
			 * the parent and the clone can now be freely
			 * accessed now that we have acquired rnh_lock.
			 */
			gateway = SDL(route->rt_gateway);
			if ((gateway->sdl_alen != 0 && route->rt_parent &&
			    route->rt_parent->rt_ifp == route->rt_ifp) ||
			    (route->rt_flags & RTF_CONDEMNED)) {
				RT_REMREF_LOCKED(route);
				RT_UNLOCK(route);
				route = NULL;
				lck_mtx_unlock(rnh_lock);
				goto respond;
			}
			/* Change the interface when the existing route is on */
			route->rt_ifp = ifp;
			rtsetifa(route, &best_ia->ia_ifa);
			gateway->sdl_index = ifp->if_index;
			RT_UNLOCK(route);
			lck_mtx_unlock(rnh_lock);
			RT_LOCK(route);
			/* Don't bother if the route is down */
			if (!(route->rt_flags & RTF_UP))
				goto respond;
			/* Refresh gateway pointer */
			gateway = SDL(route->rt_gateway);
		}
		RT_LOCK_ASSERT_HELD(route);
	}

	if (gateway->sdl_alen && bcmp(LLADDR(gateway), CONST_LLADDR(sender_hw), gateway->sdl_alen)) {
		if (route->rt_rmx.rmx_expire && log_arp_warnings) {
			char buf2[3 * MAX_HW_LEN];
			log(LOG_INFO, "arp: %s moved from %s to %s on %s%d\n",
			    inet_ntop(AF_INET, &sender_ip->sin_addr, ipv4str,
			    sizeof(ipv4str)),
			    sdl_addr_to_hex(gateway, buf, sizeof(buf)),
			    sdl_addr_to_hex(sender_hw, buf2, sizeof(buf2)),
			    ifp->if_name, ifp->if_unit);
		}
		else if (route->rt_rmx.rmx_expire == 0) {
			if (log_arp_warnings) {
				log(LOG_ERR, "arp: %s attempts to modify "
				    "permanent entry for %s on %s%d\n",
				    sdl_addr_to_hex(sender_hw, buf,
				    sizeof(buf)),
				    inet_ntop(AF_INET, &sender_ip->sin_addr,
				    ipv4str, sizeof(ipv4str)),
				    ifp->if_name, ifp->if_unit);
			}
			goto respond;
		}
	}

	/* Copy the sender hardware address in to the route's gateway address */
	gateway->sdl_alen = sender_hw->sdl_alen;
	bcopy(CONST_LLADDR(sender_hw), LLADDR(gateway), gateway->sdl_alen);

	/* Update the expire time for the route and clear the reject flag */
	if (route->rt_rmx.rmx_expire) {
		struct timeval timenow;

		getmicrotime(&timenow);
		route->rt_rmx.rmx_expire = timenow.tv_sec + arpt_keep;
	}
	route->rt_flags &= ~RTF_REJECT;

	/* update the llinfo, send a queued packet if there is one */
	llinfo = route->rt_llinfo;
	llinfo->la_asked = 0;
	if (llinfo->la_hold) {
		struct mbuf *m0;
		m0 = llinfo->la_hold;
		llinfo->la_hold = 0;

		RT_UNLOCK(route);
		dlil_output(ifp, PF_INET, m0, (caddr_t)route, rt_key(route), 0);
		RT_REMREF(route);
		route = NULL;
	}

respond:
	if (route != NULL) {
		RT_REMREF_LOCKED(route);
		RT_UNLOCK(route);
		route = NULL;
	}

	if (arpop != ARPOP_REQUEST)
		goto done;

	/* If we are not the target, check if we should proxy */
	if (target_ip->sin_addr.s_addr != best_ia->ia_addr.sin_addr.s_addr) {
		/*
		 * Find a proxy route; callee holds a reference on the
		 * route and returns with the route entry locked, upon
		 * success.
		 */
		error = arp_lookup_route(&target_ip->sin_addr, 0, SIN_PROXY,
		    &route, ifp->if_index);

		if (error == 0) {
			RT_LOCK_ASSERT_HELD(route);
			/*
			 * Return proxied ARP replies only on the interface
			 * or bridge cluster where this network resides.
			 * Otherwise we may conflict with the host we are
			 * proxying for.
			 */
			if (route->rt_ifp != ifp &&
				(route->rt_ifp->if_bridge != ifp->if_bridge ||
				 ifp->if_bridge == NULL)) {
					RT_REMREF_LOCKED(route);
					RT_UNLOCK(route);
					goto done;
				}
			proxied = *SDL(route->rt_gateway);
			target_hw = &proxied;
		} else {
			/*
			 * We don't have a route entry indicating we should
			 * use proxy.  If we aren't supposed to proxy all,
			 * we are done.
			 */
			if (!arp_proxyall)
				goto done;

			/*
			 * See if we have a route to the target ip before
			 * we proxy it.
			 */
			route = rtalloc1_scoped((struct sockaddr *)
			    (size_t)target_ip, 0, 0, ifp->if_index);
			if (!route)
				goto done;

			/*
			 * Don't proxy for hosts already on the same interface.
			 */
			RT_LOCK(route);
			if (route->rt_ifp == ifp) {
				RT_UNLOCK(route);
				rtfree(route);
				goto done;
			}
		}
		RT_REMREF_LOCKED(route);
		RT_UNLOCK(route);
	}

	dlil_send_arp(ifp, ARPOP_REPLY,
	    target_hw, (const struct sockaddr*)target_ip,
	    sender_hw, (const struct sockaddr*)sender_ip);

done:
	if (best_ia != NULL)
		ifafree(&best_ia->ia_ifa);
	return 0;
}

void
arp_ifinit(
	struct ifnet *ifp,
	struct ifaddr *ifa)
{
	ifa->ifa_rtrequest = arp_rtrequest;
	ifa->ifa_flags |= RTF_CLONING;
	dlil_send_arp(ifp, ARPOP_REQUEST, NULL, ifa->ifa_addr, NULL, ifa->ifa_addr);
}

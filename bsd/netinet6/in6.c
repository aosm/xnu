/*
 * Copyright (c) 2003-2009 Apple Inc. All rights reserved.
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

/*	$FreeBSD: src/sys/netinet6/in6.c,v 1.7.2.7 2001/08/06 20:26:22 ume Exp $	*/
/*	$KAME: in6.c,v 1.187 2001/05/24 07:43:59 itojun Exp $	*/

/*
 * Copyright (C) 1995, 1996, 1997, and 1998 WIDE Project.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the project nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE PROJECT AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE PROJECT OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

/*
 * Copyright (c) 1982, 1986, 1991, 1993
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
 *	@(#)in.c	8.2 (Berkeley) 11/15/93
 */


#include <sys/param.h>
#include <sys/ioctl.h>
#include <sys/errno.h>
#include <sys/malloc.h>
#include <sys/socket.h>
#include <sys/socketvar.h>
#include <sys/sockio.h>
#include <sys/systm.h>
#include <sys/time.h>
#include <sys/kernel.h>
#include <sys/syslog.h>
#include <sys/kern_event.h>

#include <kern/locks.h>
#include <kern/zalloc.h>
#include <libkern/OSAtomic.h>
#include <machine/machine_routines.h>

#include <net/if.h>
#include <net/if_types.h>
#include <net/if_var.h>
#include <net/route.h>
#include <net/if_dl.h>
#include <net/kpi_protocol.h>

#include <netinet/in.h>
#include <netinet/in_var.h>
#include <netinet/if_ether.h>
#ifndef SCOPEDROUTING
#include <netinet/in_systm.h>
#include <netinet/ip.h>
#include <netinet/in_pcb.h>
#endif

#include <netinet6/nd6.h>
#include <netinet/ip6.h>
#include <netinet6/ip6_var.h>
#include <netinet6/mld6_var.h>
#include <netinet6/ip6_mroute.h>
#include <netinet6/in6_ifattach.h>
#include <netinet6/scope6_var.h>
#ifndef SCOPEDROUTING
#include <netinet6/in6_pcb.h>
#endif

#include <net/net_osdep.h>

#if PF
#include <net/pfvar.h>
#endif /* PF */

#ifndef __APPLE__
MALLOC_DEFINE(M_IPMADDR, "in6_multi", "internet multicast address");
#endif
 /*
 * Definitions of some costant IP6 addresses.
 */
const struct in6_addr in6addr_any = IN6ADDR_ANY_INIT;
const struct in6_addr in6addr_loopback = IN6ADDR_LOOPBACK_INIT;
const struct in6_addr in6addr_nodelocal_allnodes =
	IN6ADDR_NODELOCAL_ALLNODES_INIT;
const struct in6_addr in6addr_linklocal_allnodes =
	IN6ADDR_LINKLOCAL_ALLNODES_INIT;
const struct in6_addr in6addr_linklocal_allrouters =
	IN6ADDR_LINKLOCAL_ALLROUTERS_INIT;

const struct in6_addr in6mask0 = IN6MASK0;
const struct in6_addr in6mask32 = IN6MASK32;
const struct in6_addr in6mask64 = IN6MASK64;
const struct in6_addr in6mask96 = IN6MASK96;
const struct in6_addr in6mask128 = IN6MASK128;

const struct sockaddr_in6 sa6_any = {sizeof(sa6_any), AF_INET6,
				     0, 0, IN6ADDR_ANY_INIT, 0};

static int in6_lifaddr_ioctl(struct socket *, u_long, caddr_t,
	struct ifnet *, struct proc *);
static int in6_ifinit(struct ifnet *, struct in6_ifaddr *,
			   struct sockaddr_in6 *, int);
static void in6_unlink_ifa(struct in6_ifaddr *, struct ifnet *, int);
static struct in6_ifaddr *in6_ifaddr_alloc(int);
static void in6_ifaddr_free(struct ifaddr *);
static void in6_ifaddr_trace(struct ifaddr *, int);
static struct in6_aliasreq *in6_aliasreq_to_native(void *, int,
    struct in6_aliasreq *);

struct in6_multihead in6_multihead;	/* XXX BSS initialization */
extern lck_mtx_t *nd6_mutex;
extern lck_mtx_t *ip6_mutex;
extern int in6_init2done;

struct in6_ifaddr_dbg {
	struct in6_ifaddr	in6ifa;			/* in6_ifaddr */
	struct in6_ifaddr	in6ifa_old;		/* saved in6_ifaddr */
	u_int16_t		in6ifa_refhold_cnt;	/* # of ifaref */
	u_int16_t		in6ifa_refrele_cnt;	/* # of ifafree */
	/*
	 * Alloc and free callers.
	 */
	ctrace_t		in6ifa_alloc;
	ctrace_t		in6ifa_free;
	/*
	 * Circular lists of ifaref and ifafree callers.
	 */
	ctrace_t		in6ifa_refhold[CTRACE_HIST_SIZE];
	ctrace_t		in6ifa_refrele[CTRACE_HIST_SIZE];
};

static unsigned int in6ifa_debug;		/* debug flags */
static unsigned int in6ifa_size;		/* size of zone element */
static struct zone *in6ifa_zone;		/* zone for in6_ifaddr */

#define	IN6IFA_ZONE_MAX		64		/* maximum elements in zone */
#define	IN6IFA_ZONE_NAME	"in6_ifaddr"	/* zone name */

/*
 * Subroutine for in6_ifaddloop() and in6_ifremloop().
 * This routine does actual work.
 */
static void
in6_ifloop_request(int cmd, struct ifaddr *ifa)
{
	struct sockaddr_in6 all1_sa;
	struct rtentry *nrt = NULL;
	int e;

	bzero(&all1_sa, sizeof(all1_sa));
	all1_sa.sin6_family = AF_INET6;
	all1_sa.sin6_len = sizeof(struct sockaddr_in6);
	all1_sa.sin6_addr = in6mask128;

	/*
	 * We specify the address itself as the gateway, and set the
	 * RTF_LLINFO flag, so that the corresponding host route would have
	 * the flag, and thus applications that assume traditional behavior
	 * would be happy.  Note that we assume the caller of the function
	 * (probably implicitly) set nd6_rtrequest() to ifa->ifa_rtrequest,
	 * which changes the outgoing interface to the loopback interface.
	 */
	lck_mtx_lock(rnh_lock);
	e = rtrequest_locked(cmd, ifa->ifa_addr, ifa->ifa_addr,
		      (struct sockaddr *)&all1_sa,
		      RTF_UP|RTF_HOST|RTF_LLINFO, &nrt);
	if (e != 0) {
		log(LOG_ERR, "in6_ifloop_request: "
		    "%s operation failed for %s (errno=%d)\n",
		    cmd == RTM_ADD ? "ADD" : "DELETE",
		    ip6_sprintf(&((struct in6_ifaddr *)ifa)->ia_addr.sin6_addr),
		    e);
	}

	if (nrt != NULL)
		RT_LOCK(nrt);
	/*
	 * Make sure rt_ifa be equal to IFA, the second argument of the
	 * function.
	 * We need this because when we refer to rt_ifa->ia6_flags in
	 * ip6_input, we assume that the rt_ifa points to the address instead
	 * of the loopback address.
	 */
	if (cmd == RTM_ADD && nrt && ifa != nrt->rt_ifa) {
		rtsetifa(nrt, ifa);
	}

	/*
	 * Report the addition/removal of the address to the routing socket.
	 * XXX: since we called rtinit for a p2p interface with a destination,
	 *      we end up reporting twice in such a case.  Should we rather
	 *      omit the second report?
	 */
	if (nrt != NULL) {
		rt_newaddrmsg(cmd, ifa, e, nrt);
		if (cmd == RTM_DELETE) {
			RT_UNLOCK(nrt);
			rtfree_locked(nrt);
		} else {
			/* the cmd must be RTM_ADD here */
			RT_REMREF_LOCKED(nrt);
			RT_UNLOCK(nrt);
		}
	}
	lck_mtx_unlock(rnh_lock);
}

/*
 * Add ownaddr as loopback rtentry.  We previously add the route only if
 * necessary (ex. on a p2p link).  However, since we now manage addresses
 * separately from prefixes, we should always add the route.  We can't
 * rely on the cloning mechanism from the corresponding interface route
 * any more.
 */
static void
in6_ifaddloop(struct ifaddr *ifa)
{
	struct rtentry *rt;

	/* If there is no loopback entry, allocate one. */
	rt = rtalloc1(ifa->ifa_addr, 0, 0);
	if (rt != NULL)
		RT_LOCK(rt);
	if (rt == NULL || (rt->rt_flags & RTF_HOST) == 0 ||
	    (rt->rt_ifp->if_flags & IFF_LOOPBACK) == 0) {
		if (rt != NULL) {
			RT_REMREF_LOCKED(rt);
			RT_UNLOCK(rt);
		}
		in6_ifloop_request(RTM_ADD, ifa);
	} else if (rt != NULL) {
		RT_REMREF_LOCKED(rt);
		RT_UNLOCK(rt);
	}
}

/*
 * Remove loopback rtentry of ownaddr generated by in6_ifaddloop(),
 * if it exists.
 */
static void
in6_ifremloop(struct ifaddr *ifa, int locked)
{
	struct in6_ifaddr *ia;
	struct rtentry *rt;
	int ia_count = 0;

	/*
	 * Some of BSD variants do not remove cloned routes
	 * from an interface direct route, when removing the direct route
	 * (see comments in net/net_osdep.h).  Even for variants that do remove
	 * cloned routes, they could fail to remove the cloned routes when
	 * we handle multple addresses that share a common prefix.
	 * So, we should remove the route corresponding to the deleted address
	 * regardless of the result of in6_is_ifloop_auto().
	 */

	/*
	 * Delete the entry only if exact one ifa exists.  More than one ifa
	 * can exist if we assign a same single address to multiple
	 * (probably p2p) interfaces.
	 * XXX: we should avoid such a configuration in IPv6...
	 */
	if (!locked)
		lck_mtx_lock(nd6_mutex);
	for (ia = in6_ifaddrs; ia; ia = ia->ia_next) {
		if (IN6_ARE_ADDR_EQUAL(IFA_IN6(ifa), &ia->ia_addr.sin6_addr)) {
			ia_count++;
			if (ia_count > 1)
				break;
		}
	}
	if (!locked)
		lck_mtx_unlock(nd6_mutex);

	if (ia_count == 1) {
		/*
		 * Before deleting, check if a corresponding loopbacked host
		 * route surely exists.  With this check, we can avoid to
		 * delete an interface direct route whose destination is same
		 * as the address being removed.  This can happen when remofing
		 * a subnet-router anycast address on an interface attahced
		 * to a shared medium.
		 */
		rt = rtalloc1(ifa->ifa_addr, 0, 0);
		if (rt != NULL) {
			RT_LOCK(rt);
			if ((rt->rt_flags & RTF_HOST) != 0 &&
			    (rt->rt_ifp->if_flags & IFF_LOOPBACK) != 0) {
				RT_REMREF_LOCKED(rt);
				RT_UNLOCK(rt);
				in6_ifloop_request(RTM_DELETE, ifa);
			} else {
				RT_UNLOCK(rt);
			}
		}
	}
}

#if 0
/* Not used */
int
in6_ifindex2scopeid(idx)
	int idx;
{
	struct ifnet *ifp;
	struct ifaddr *ifa;
	struct sockaddr_in6 *sin6;

	ifnet_head_lock_shared();
	if (idx <= 0 || if_index < idx) {
		ifnet_head_done();
		return -1;
	}
	
	ifp = ifindex2ifnet[idx];
	ifnet_head_done();

	ifnet_lock_shared(ifp);
	TAILQ_FOREACH(ifa, &ifp->if_addrlist, ifa_list)
	{
		if (ifa->ifa_addr->sa_family != AF_INET6)
			continue;
		sin6 = (struct sockaddr_in6 *)ifa->ifa_addr;
		if (IN6_IS_ADDR_SITELOCAL(&sin6->sin6_addr)) {
			int scopeid = sin6->sin6_scope_id & 0xffff;
			ifnet_lock_done(ifp);
			return scopeid;
		}
	}
	ifnet_lock_done(ifp);

	return -1;
}
#endif


int
in6_mask2len(mask, lim0)
	struct in6_addr *mask;
	u_char *lim0;
{
	int x = 0, y;
	u_char *lim = lim0, *p;

	if (lim0 == NULL ||
	    lim0 - (u_char *)mask > sizeof(*mask)) /* ignore the scope_id part */
		lim = (u_char *)mask + sizeof(*mask);
	for (p = (u_char *)mask; p < lim; x++, p++) {
		if (*p != 0xff)
			break;
	}
	y = 0;
	if (p < lim) {
		for (y = 0; y < 8; y++) {
			if ((*p & (0x80 >> y)) == 0)
				break;
		}
	}

	/*
	 * when the limit pointer is given, do a stricter check on the
	 * remaining bits.
	 */
	if (p < lim) {
		if (y != 0 && (*p & (0x00ff >> y)) != 0)
			return(-1);
		for (p = p + 1; p < lim; p++)
			if (*p != 0)
				return(-1);
	}
	
	return x * 8 + y;
}

void
in6_len2mask(mask, len)
	struct in6_addr *mask;
	int len;
{
	int i;

	bzero(mask, sizeof(*mask));
	for (i = 0; i < len / 8; i++)
		mask->s6_addr8[i] = 0xff;
	if (len % 8)
		mask->s6_addr8[i] = (0xff00 >> (len % 8)) & 0xff;
}

void
in6_aliasreq_64_to_32(struct in6_aliasreq_64 *src, struct in6_aliasreq_32 *dst)
{
	bzero(dst, sizeof (*dst));
	bcopy(src->ifra_name, dst->ifra_name, sizeof (dst->ifra_name));
	dst->ifra_addr = src->ifra_addr;
	dst->ifra_dstaddr = src->ifra_dstaddr;
	dst->ifra_prefixmask = src->ifra_prefixmask;
	dst->ifra_flags = src->ifra_flags;
	dst->ifra_lifetime.ia6t_expire = src->ifra_lifetime.ia6t_expire;
	dst->ifra_lifetime.ia6t_preferred = src->ifra_lifetime.ia6t_preferred;
	dst->ifra_lifetime.ia6t_vltime = src->ifra_lifetime.ia6t_vltime;
	dst->ifra_lifetime.ia6t_pltime = src->ifra_lifetime.ia6t_pltime;
}

void
in6_aliasreq_32_to_64(struct in6_aliasreq_32 *src, struct in6_aliasreq_64 *dst)
{
	bzero(dst, sizeof (*dst));
	bcopy(src->ifra_name, dst->ifra_name, sizeof (dst->ifra_name));
	dst->ifra_addr = src->ifra_addr;
	dst->ifra_dstaddr = src->ifra_dstaddr;
	dst->ifra_prefixmask = src->ifra_prefixmask;
	dst->ifra_flags = src->ifra_flags;
	dst->ifra_lifetime.ia6t_expire = src->ifra_lifetime.ia6t_expire;
	dst->ifra_lifetime.ia6t_preferred = src->ifra_lifetime.ia6t_preferred;
	dst->ifra_lifetime.ia6t_vltime = src->ifra_lifetime.ia6t_vltime;
	dst->ifra_lifetime.ia6t_pltime = src->ifra_lifetime.ia6t_pltime;
}

static struct in6_aliasreq *
in6_aliasreq_to_native(void *data, int data_is_64, struct in6_aliasreq *dst)
{
#if defined(__LP64__)
	if (data_is_64)
		dst = data;
	else
		in6_aliasreq_32_to_64((struct in6_aliasreq_32 *)data,
		    (struct in6_aliasreq_64 *)dst);
#else
	if (data_is_64)
		in6_aliasreq_64_to_32((struct in6_aliasreq_64 *)data,
		    (struct in6_aliasreq_32 *)dst);
	else
		dst = data;
#endif /* __LP64__ */
	return (dst);
}

#define ifa2ia6(ifa)	((struct in6_ifaddr *)(ifa))
#define ia62ifa(ia6)	(&((ia6)->ia_ifa))

int
in6_control(struct socket *so, u_long cmd, caddr_t data, struct ifnet *ifp,
    struct proc *p)
{
	struct	in6_ifreq *ifr = (struct in6_ifreq *)data;
	struct	in6_ifaddr *ia = NULL;
	struct	in6_aliasreq sifra;
	struct	in6_aliasreq *ifra = NULL;
	struct sockaddr_in6 *sa6;
	int index, privileged, error = 0;
	struct timeval timenow;
	int p64 = proc_is64bit(p);

	getmicrotime(&timenow);

	privileged = (proc_suser(p) == 0);
#if MROUTING
	switch (cmd) {
	case SIOCGETSGCNT_IN6:
	case SIOCGETMIFCNT_IN6_32:
	case SIOCGETMIFCNT_IN6_64:
		return (mrt6_ioctl(cmd, data));
	}
#endif
	if (ifp == NULL)
		return (EOPNOTSUPP);

	switch (cmd) {
	case SIOCAUTOCONF_START:
	case SIOCAUTOCONF_STOP:
	case SIOCLL_START_32:
	case SIOCLL_START_64:
	case SIOCLL_STOP:
	case SIOCPROTOATTACH_IN6_32:
	case SIOCPROTOATTACH_IN6_64:
	case SIOCPROTODETACH_IN6:
                if (!privileged)
                        return (EPERM);
		break;
	case SIOCSNDFLUSH_IN6:
	case SIOCSPFXFLUSH_IN6:
	case SIOCSRTRFLUSH_IN6:
	case SIOCSDEFIFACE_IN6_32:
	case SIOCSDEFIFACE_IN6_64:
	case SIOCSIFINFO_FLAGS:
		if (!privileged)
			return (EPERM);
		/* fall through */
	case OSIOCGIFINFO_IN6:
	case SIOCGIFINFO_IN6:
	case SIOCGDRLST_IN6_32:
	case SIOCGDRLST_IN6_64:
	case SIOCGPRLST_IN6_32:
	case SIOCGPRLST_IN6_64:
	case SIOCGNBRINFO_IN6_32:
	case SIOCGNBRINFO_IN6_64:
	case SIOCGDEFIFACE_IN6_32:
	case SIOCGDEFIFACE_IN6_64:
		return (nd6_ioctl(cmd, data, ifp));
	}

	switch (cmd) {
	case SIOCSIFPREFIX_IN6:
	case SIOCDIFPREFIX_IN6:
	case SIOCAIFPREFIX_IN6:
	case SIOCCIFPREFIX_IN6:
	case SIOCSGIFPREFIX_IN6:
	case SIOCGIFPREFIX_IN6:
		log(LOG_NOTICE,
		    "prefix ioctls are now invalidated. "
		    "please use ifconfig.\n");
		return (EOPNOTSUPP);
	}

	switch (cmd) {
	case SIOCSSCOPE6:
		if (!privileged)
			return (EPERM);
		return (scope6_set(ifp, ifr->ifr_ifru.ifru_scope_id));
		/* NOTREACHED */

	case SIOCGSCOPE6:
		return (scope6_get(ifp, ifr->ifr_ifru.ifru_scope_id));
		/* NOTREACHED */

	case SIOCGSCOPE6DEF:
		return (scope6_get_default(ifr->ifr_ifru.ifru_scope_id));
	}

	switch (cmd) {
	case SIOCALIFADDR:
	case SIOCDLIFADDR:
		if (!privileged)
			return(EPERM);
		/* fall through */
	case SIOCGLIFADDR:
		return (in6_lifaddr_ioctl(so, cmd, data, ifp, p));
	}

	/*
	 * Point ifra and sa6 to the right places depending on the command.
	 */
	switch (cmd) {
	case SIOCLL_START_32:
	case SIOCAIFADDR_IN6_32:
		/*
		 * Convert user ifra to the kernel form, when appropriate.
		 * This allows the conversion between different data models
		 * to be centralized, so that it can be passed around to other
		 * routines that are expecting the kernel form.
		 */
		ifra = in6_aliasreq_to_native(data, 0, &sifra);
		sa6 = (struct sockaddr_in6 *)&ifra->ifra_addr;
		break;

	case SIOCLL_START_64:
	case SIOCAIFADDR_IN6_64:
		ifra = in6_aliasreq_to_native(data, 1, &sifra);
		sa6 = (struct sockaddr_in6 *)&ifra->ifra_addr;
		break;

	case SIOCSIFADDR_IN6:		/* deprecated */
	case SIOCGIFADDR_IN6:
	case SIOCSIFDSTADDR_IN6:	/* deprecated */
	case SIOCSIFNETMASK_IN6:	/* deprecated */
	case SIOCGIFDSTADDR_IN6:
	case SIOCGIFNETMASK_IN6:
	case SIOCDIFADDR_IN6:
	case SIOCGIFPSRCADDR_IN6:
	case SIOCGIFPDSTADDR_IN6:
	case SIOCGIFAFLAG_IN6:
	case SIOCGIFALIFETIME_IN6:
	case SIOCSIFALIFETIME_IN6:
	case SIOCGIFSTAT_IN6:
	case SIOCGIFSTAT_ICMP6:
		sa6 = &ifr->ifr_addr;
		break;

	default:
		sa6 = NULL;
		break;
	}

	switch (cmd) {

	case SIOCAUTOCONF_START:
		ifnet_lock_exclusive(ifp);
		ifp->if_eflags |= IFEF_ACCEPT_RTADVD;
		ifnet_lock_done(ifp);
		return (0);
		/* NOTREACHED */

	case SIOCAUTOCONF_STOP: {
		struct in6_ifaddr *nia = NULL;

		ifnet_lock_exclusive(ifp);
		ifp->if_eflags &= ~IFEF_ACCEPT_RTADVD;
		ifnet_lock_done(ifp);

		/* nuke prefix list.  this may try to remove some ifaddrs as well */
		in6_purgeprefix(ifp);

		/* removed autoconfigured address from interface */
		lck_mtx_lock(nd6_mutex);
		for (ia = in6_ifaddrs; ia != NULL; ia = nia) {
			nia = ia->ia_next;
			if (ia->ia_ifa.ifa_ifp != ifp)
				continue;
			if (ia->ia6_flags & IN6_IFF_AUTOCONF)
				in6_purgeaddr(&ia->ia_ifa, 1);
		}
		lck_mtx_unlock(nd6_mutex);
		return (0);
	}

	case SIOCLL_START_32:
	case SIOCLL_START_64:
		/*
		 * NOTE: All the interface specific DLIL attachements should
		 * be done here.  They are currently done in in6_ifattach()
		 * for the interfaces that need it.
		 */
		if (((ifp->if_type == IFT_PPP) || ((ifp->if_eflags & IFEF_NOAUTOIPV6LL) != 0))  &&
		    ifra->ifra_addr.sin6_family == AF_INET6 &&
		    ifra->ifra_dstaddr.sin6_family == AF_INET6) {
			/* some interfaces may provide LinkLocal addresses */
			error = in6_if_up(ifp, ifra);
		} else {
			error = in6_if_up(ifp, 0);
		}
		return (error);
		/* NOTREACHED */

	case SIOCLL_STOP: {
		struct in6_ifaddr *nia = NULL;

		/* removed link local addresses from interface */

		lck_mtx_lock(nd6_mutex);
		for (ia = in6_ifaddrs; ia != NULL; ia = nia) {
			nia = ia->ia_next;
			if (ia->ia_ifa.ifa_ifp != ifp)
				continue;
			if (IN6_IS_ADDR_LINKLOCAL(&ia->ia_addr.sin6_addr))
				in6_purgeaddr(&ia->ia_ifa, 1);
		}
		lck_mtx_unlock(nd6_mutex);
		return (0);
	}

	case SIOCPROTOATTACH_IN6_32:
	case SIOCPROTOATTACH_IN6_64:
		if ((error = proto_plumb(PF_INET6, ifp)))
			printf("SIOCPROTOATTACH_IN6: %s "
				   "error=%d\n", if_name(ifp), error);
		return (error);
		/* NOTREACHED */

	case SIOCPROTODETACH_IN6:
		/* Cleanup interface routes and addresses */
		in6_purgeif(ifp);

		if ((error = proto_unplumb(PF_INET6, ifp)))
			printf("SIOCPROTODETACH_IN6: %s error=%d\n",
			    if_name(ifp), error);
		return (error);
	}

	/*
	 * Find address for this interface, if it exists; depending
	 * on the ioctl command, sa6 points to the address in ifra/ifr.
	 */
	if (sa6 != NULL && sa6->sin6_family == AF_INET6) {
		if (IN6_IS_ADDR_LINKLOCAL(&sa6->sin6_addr)) {
			if (sa6->sin6_addr.s6_addr16[1] == 0) {
				/* link ID is not embedded by the user */
				sa6->sin6_addr.s6_addr16[1] =
				    htons(ifp->if_index);
			} else if (sa6->sin6_addr.s6_addr16[1] !=
			    htons(ifp->if_index)) {
				return (EINVAL); /* link ID contradicts */
			}
			if (sa6->sin6_scope_id) {
				if (sa6->sin6_scope_id !=
				    (u_int32_t)ifp->if_index)
					return (EINVAL);
				sa6->sin6_scope_id = 0; /* XXX: good way? */
			}
		}
		ia = in6ifa_ifpwithaddr(ifp, &sa6->sin6_addr);
	} else {
		ia = NULL;
	}

	switch (cmd) {
	case SIOCSIFADDR_IN6:
	case SIOCSIFDSTADDR_IN6:
	case SIOCSIFNETMASK_IN6:
		/*
		 * Since IPv6 allows a node to assign multiple addresses
		 * on a single interface, SIOCSIFxxx ioctls are not suitable
		 * and should be unused.
		 */
		/* we decided to obsolete this command (20000704) */
		error = EINVAL;
		goto ioctl_cleanup;

	case SIOCDIFADDR_IN6:
		/*
		 * for IPv4, we look for existing in_ifaddr here to allow
		 * "ifconfig if0 delete" to remove first IPv4 address on the
		 * interface.  For IPv6, as the spec allow multiple interface
		 * address from the day one, we consider "remove the first one"
		 * semantics to be not preferable.
		 */
		if (ia == NULL) {
			error = EADDRNOTAVAIL;
			goto ioctl_cleanup;
		}
		/* FALLTHROUGH */
	case SIOCAIFADDR_IN6_32:
	case SIOCAIFADDR_IN6_64:
		/*
		 * We always require users to specify a valid IPv6 address for
		 * the corresponding operation.  Use "sa6" instead of "ifra"
		 * since SIOCDIFADDR_IN6 falls thru above.
		 */
		if (sa6->sin6_family != AF_INET6 ||
		    sa6->sin6_len != sizeof(struct sockaddr_in6)) {
			error = EAFNOSUPPORT;
			goto ioctl_cleanup;
		}
		if (!privileged) {
			error = EPERM;
			goto ioctl_cleanup;
		}

		break;

	case SIOCGIFADDR_IN6:
		/* This interface is basically deprecated. use SIOCGIFCONF. */
		/* fall through */
	case SIOCGIFAFLAG_IN6:
	case SIOCGIFNETMASK_IN6:
	case SIOCGIFDSTADDR_IN6:
	case SIOCGIFALIFETIME_IN6:
		/* must think again about its semantics */
		if (ia == NULL) {
			error = EADDRNOTAVAIL;
			goto ioctl_cleanup;
		}
		break;

	case SIOCSIFALIFETIME_IN6:
		if (!privileged) {
			error = EPERM;
			goto ioctl_cleanup;
		}
		if (ia == NULL) {
			error = EADDRNOTAVAIL;
			goto ioctl_cleanup;
		}
		/* sanity for overflow - beware unsigned */
		if (p64) {
			struct in6_addrlifetime_64 *lt;

			lt = (struct in6_addrlifetime_64 *)
			    &ifr->ifr_ifru.ifru_lifetime;
			if (lt->ia6t_vltime != ND6_INFINITE_LIFETIME
			 && lt->ia6t_vltime + timenow.tv_sec < timenow.tv_sec) {
				error = EINVAL;
				goto ioctl_cleanup;
			}
			if (lt->ia6t_pltime != ND6_INFINITE_LIFETIME
			 && lt->ia6t_pltime + timenow.tv_sec < timenow.tv_sec) {
				error = EINVAL;
				goto ioctl_cleanup;
			}
		} else {
			struct in6_addrlifetime_32 *lt;

			lt = (struct in6_addrlifetime_32 *)
			    &ifr->ifr_ifru.ifru_lifetime;
			if (lt->ia6t_vltime != ND6_INFINITE_LIFETIME
			 && lt->ia6t_vltime + timenow.tv_sec < timenow.tv_sec) {
				error = EINVAL;
				goto ioctl_cleanup;
			}
			if (lt->ia6t_pltime != ND6_INFINITE_LIFETIME
			 && lt->ia6t_pltime + timenow.tv_sec < timenow.tv_sec) {
				error = EINVAL;
				goto ioctl_cleanup;
			}
		}
		break;
	}

	switch (cmd) {
	case SIOCGIFADDR_IN6:
		ifr->ifr_addr = ia->ia_addr;
		break;

	case SIOCGIFDSTADDR_IN6:
		if ((ifp->if_flags & IFF_POINTOPOINT) == 0) {
			error = EINVAL;
			goto ioctl_cleanup;
		}
		/*
		 * XXX: should we check if ifa_dstaddr is NULL and return
		 * an error?
		 */
		ifr->ifr_dstaddr = ia->ia_dstaddr;
		break;

	case SIOCGIFNETMASK_IN6:
		ifr->ifr_addr = ia->ia_prefixmask;
		break;

	case SIOCGIFAFLAG_IN6:
		ifr->ifr_ifru.ifru_flags6 = ia->ia6_flags;
		break;

	case SIOCGIFSTAT_IN6:
		if (ifp == NULL) {
			error = EINVAL;
			goto ioctl_cleanup;
		}
		index = ifp->if_index;
		lck_mtx_lock(ip6_mutex);
		if (in6_ifstat == NULL || index >= in6_ifstatmax
		 || in6_ifstat[index] == NULL) {
			/* return EAFNOSUPPORT? */
			bzero(&ifr->ifr_ifru.ifru_stat,
			    sizeof (ifr->ifr_ifru.ifru_stat));
		} else {
			ifr->ifr_ifru.ifru_stat = *in6_ifstat[index];
		}
		lck_mtx_unlock(ip6_mutex);
		break;

	case SIOCGIFSTAT_ICMP6:
		if (ifp == NULL) {
			error = EINVAL;
			goto ioctl_cleanup;
		}
		index = ifp->if_index;
		lck_mtx_lock(ip6_mutex);
		if (icmp6_ifstat == NULL || index >= icmp6_ifstatmax ||
		    icmp6_ifstat[index] == NULL) {
			/* return EAFNOSUPPORT? */
			bzero(&ifr->ifr_ifru.ifru_stat,
			    sizeof (ifr->ifr_ifru.ifru_icmp6stat));
		} else {
			ifr->ifr_ifru.ifru_icmp6stat = *icmp6_ifstat[index];
		}
		lck_mtx_unlock(ip6_mutex);
		break;

	case SIOCGIFALIFETIME_IN6:
		if (p64) {
			struct in6_addrlifetime_64 *lt;

			lt = (struct in6_addrlifetime_64 *)
			    &ifr->ifr_ifru.ifru_lifetime;
			lt->ia6t_expire = ia->ia6_lifetime.ia6t_expire;
			lt->ia6t_preferred = ia->ia6_lifetime.ia6t_preferred;
			lt->ia6t_vltime = ia->ia6_lifetime.ia6t_vltime;
			lt->ia6t_pltime = ia->ia6_lifetime.ia6t_pltime;
		} else {
			struct in6_addrlifetime_32 *lt;

			lt = (struct in6_addrlifetime_32 *)
			    &ifr->ifr_ifru.ifru_lifetime;
			lt->ia6t_expire =
			    (uint32_t)ia->ia6_lifetime.ia6t_expire;
			lt->ia6t_preferred =
			    (uint32_t)ia->ia6_lifetime.ia6t_preferred;
			lt->ia6t_vltime =
			    (uint32_t)ia->ia6_lifetime.ia6t_vltime;
			lt->ia6t_pltime =
			    (uint32_t)ia->ia6_lifetime.ia6t_pltime;
		}
		break;

	case SIOCSIFALIFETIME_IN6:
		if (p64) {
			struct in6_addrlifetime_64 *lt;

			lt = (struct in6_addrlifetime_64 *)
			    &ifr->ifr_ifru.ifru_lifetime;
			ia->ia6_lifetime.ia6t_expire = lt->ia6t_expire;
			ia->ia6_lifetime.ia6t_preferred = lt->ia6t_preferred;
			ia->ia6_lifetime.ia6t_vltime = lt->ia6t_vltime;
			ia->ia6_lifetime.ia6t_pltime = lt->ia6t_pltime;
		} else {
			struct in6_addrlifetime_32 *lt;

			lt = (struct in6_addrlifetime_32 *)
			    &ifr->ifr_ifru.ifru_lifetime;
			ia->ia6_lifetime.ia6t_expire =
			    (uint32_t)lt->ia6t_expire;
			ia->ia6_lifetime.ia6t_preferred =
			    (uint32_t)lt->ia6t_preferred;
			ia->ia6_lifetime.ia6t_vltime = lt->ia6t_vltime;
			ia->ia6_lifetime.ia6t_pltime = lt->ia6t_pltime;
		}
		/* for sanity */
		if (ia->ia6_lifetime.ia6t_vltime != ND6_INFINITE_LIFETIME) {
			ia->ia6_lifetime.ia6t_expire =
				timenow.tv_sec + ia->ia6_lifetime.ia6t_vltime;
		} else
			ia->ia6_lifetime.ia6t_expire = 0;
		if (ia->ia6_lifetime.ia6t_pltime != ND6_INFINITE_LIFETIME) {
			ia->ia6_lifetime.ia6t_preferred =
				timenow.tv_sec + ia->ia6_lifetime.ia6t_pltime;
		} else
			ia->ia6_lifetime.ia6t_preferred = 0;
		break;

	case SIOCAIFADDR_IN6_32:
	case SIOCAIFADDR_IN6_64: {
		int i;
		struct nd_prefix pr0, *pr;

		/* Attempt to attache the protocol, in case it isn't attached */
		error = proto_plumb(PF_INET6, ifp);
		if (error) {
			if (error != EEXIST) {
				printf("SIOCAIFADDR_IN6: %s can't plumb "
				    "protocol error=%d\n", if_name(ifp), error);
				goto ioctl_cleanup;
			}

			/* Ignore, EEXIST */
			error = 0;
		} else {
			/* PF_INET6 wasn't previously attached */
			if ((error = in6_if_up(ifp, NULL)) != 0)
				goto ioctl_cleanup;
		}

		/*
		 * first, make or update the interface address structure,
		 * and link it to the list.
		 */
		if ((error = in6_update_ifa(ifp, ifra, ia, M_WAITOK)) != 0)
			goto ioctl_cleanup;

		/*
		 * then, make the prefix on-link on the interface.
		 * XXX: we'd rather create the prefix before the address, but
		 * we need at least one address to install the corresponding
		 * interface route, so we configure the address first.
		 */

		/*
		 * convert mask to prefix length (prefixmask has already
		 * been validated in in6_update_ifa().
		 */
		bzero(&pr0, sizeof(pr0));
		pr0.ndpr_ifp = ifp;
		pr0.ndpr_plen = in6_mask2len(&ifra->ifra_prefixmask.sin6_addr,
					     NULL);
		if (pr0.ndpr_plen == 128)
			break;	/* we don't need to install a host route. */
		pr0.ndpr_prefix = ifra->ifra_addr;
		pr0.ndpr_mask = ifra->ifra_prefixmask.sin6_addr;
		/* apply the mask for safety. */
		for (i = 0; i < 4; i++) {
			pr0.ndpr_prefix.sin6_addr.s6_addr32[i] &=
				ifra->ifra_prefixmask.sin6_addr.s6_addr32[i];
		}
		/*
		 * XXX: since we don't have an API to set prefix (not address)
		 * lifetimes, we just use the same lifetimes as addresses.
		 * The (temporarily) installed lifetimes can be overridden by
		 * later advertised RAs (when accept_rtadv is non 0), which is
		 * an intended behavior.
		 */
		pr0.ndpr_raf_onlink = 1; /* should be configurable? */
		pr0.ndpr_raf_auto =
			((ifra->ifra_flags & IN6_IFF_AUTOCONF) != 0);
		pr0.ndpr_vltime = ifra->ifra_lifetime.ia6t_vltime;
		pr0.ndpr_pltime = ifra->ifra_lifetime.ia6t_pltime;

		/* add the prefix if there's one. */
		if ((pr = nd6_prefix_lookup(&pr0)) == NULL) {
			/*
			 * nd6_prelist_add will install the corresponding
			 * interface route.
			 */
			if ((error = nd6_prelist_add(&pr0, NULL, &pr)) != 0)
				goto ioctl_cleanup;
			if (pr == NULL) {
				log(LOG_ERR, "nd6_prelist_add succedded but "
				    "no prefix\n");
				error = EINVAL;
				goto ioctl_cleanup;
			}
		}
		if (ia != NULL)
			ifafree(&ia->ia_ifa);
		if ((ia = in6ifa_ifpwithaddr(ifp, &ifra->ifra_addr.sin6_addr))
		    == NULL) {
			/* XXX: this should not happen! */
			log(LOG_ERR, "in6_control: addition succeeded, but"
			    " no ifaddr\n");
		} else {
			if ((ia->ia6_flags & IN6_IFF_AUTOCONF) != 0 &&
			    ia->ia6_ndpr == NULL) { /* new autoconfed addr */
				lck_mtx_lock(nd6_mutex);
				pr->ndpr_refcnt++;
				lck_mtx_unlock(nd6_mutex);
				ia->ia6_ndpr = pr;

				/*
				 * If this is the first autoconf address from
				 * the prefix, create a temporary address
				 * as well (when specified).
				 */
				if (ip6_use_tempaddr &&
				    pr->ndpr_refcnt == 1) {
					int e;
					if ((e = in6_tmpifadd(ia, 1,
					    M_WAITOK)) != 0) {
						log(LOG_NOTICE, "in6_control: "
						    "failed to create a "
						    "temporary address, "
						    "errno=%d\n",
						    e);
					}
				}
			}

			/*
			 * this might affect the status of autoconfigured
			 * addresses, that is, this address might make
			 * other addresses detached.
			 */
			pfxlist_onlink_check(0);
		}

		/* Drop use count held above during lookup/add */
		ndpr_rele(pr, FALSE);
#if PF
		pf_ifaddr_hook(ifp, cmd);
#endif /* PF */
		break;
	}

	case SIOCDIFADDR_IN6: {
		int i = 0;
		struct nd_prefix pr0, *pr;

		/*
		 * If the address being deleted is the only one that owns
		 * the corresponding prefix, expire the prefix as well.
		 * XXX: theoretically, we don't have to warry about such
		 * relationship, since we separate the address management
		 * and the prefix management.  We do this, however, to provide
		 * as much backward compatibility as possible in terms of
		 * the ioctl operation.
		 */
		bzero(&pr0, sizeof(pr0));
		pr0.ndpr_ifp = ifp;
		pr0.ndpr_plen = in6_mask2len(&ia->ia_prefixmask.sin6_addr,
					     NULL);
		if (pr0.ndpr_plen == 128)
			goto purgeaddr;
		pr0.ndpr_prefix = ia->ia_addr;
		pr0.ndpr_mask = ia->ia_prefixmask.sin6_addr;
		for (i = 0; i < 4; i++) {
			pr0.ndpr_prefix.sin6_addr.s6_addr32[i] &=
				ia->ia_prefixmask.sin6_addr.s6_addr32[i];
		}
		/*
		 * The logic of the following condition is a bit complicated.
		 * We expire the prefix when
		 * 1. the address obeys autoconfiguration and it is the
		 *    only owner of the associated prefix, or
		 * 2. the address does not obey autoconf and there is no
		 *    other owner of the prefix.
		 */
		if ((pr = nd6_prefix_lookup(&pr0)) != NULL &&
		    (((ia->ia6_flags & IN6_IFF_AUTOCONF) != 0 &&
		      pr->ndpr_refcnt == 1) ||
		     ((ia->ia6_flags & IN6_IFF_AUTOCONF) == 0 &&
		      pr->ndpr_refcnt == 0))) {
			pr->ndpr_expire = 1; /* XXX: just for expiration */
		}

		/* Drop use count held above during lookup */
		if (pr != NULL)
			ndpr_rele(pr, FALSE);

purgeaddr:
		in6_purgeaddr(&ia->ia_ifa, 0);
#if PF
		pf_ifaddr_hook(ifp, cmd);
#endif /* PF */
		break;
	}

	default:
		error = ifnet_ioctl(ifp, PF_INET6, cmd, data);
		goto ioctl_cleanup;
	}
ioctl_cleanup:
	if (ia != NULL)
		ifafree(&ia->ia_ifa);
	return (error);
}

/*
 * Update parameters of an IPv6 interface address.
 * If necessary, a new entry is created and linked into address chains.
 * This function is separated from in6_control().
 * XXX: should this be performed under splnet()?
 */
int
in6_update_ifa(ifp, ifra, ia, how)
	struct ifnet *ifp;
	struct in6_aliasreq *ifra;
	struct in6_ifaddr *ia;
	int how;
{
	int error = 0, hostIsNew = 0, plen = -1;
	struct in6_ifaddr *oia;
	struct sockaddr_in6 dst6;
	struct in6_addrlifetime *lt;
	struct timeval timenow;


	lck_mtx_assert(nd6_mutex, LCK_MTX_ASSERT_NOTOWNED);
	/* Validate parameters */
	if (ifp == NULL || ifra == NULL) /* this maybe redundant */
		return(EINVAL);

	/*
	 * The destination address for a p2p link must have a family
	 * of AF_UNSPEC or AF_INET6.
	 */
	if ((ifp->if_flags & IFF_POINTOPOINT) != 0 &&
	    ifra->ifra_dstaddr.sin6_family != AF_INET6 &&
	    ifra->ifra_dstaddr.sin6_family != AF_UNSPEC)
		return(EAFNOSUPPORT);
	/*
	 * validate ifra_prefixmask.  don't check sin6_family, netmask
	 * does not carry fields other than sin6_len.
	 */
	if (ifra->ifra_prefixmask.sin6_len > sizeof(struct sockaddr_in6))
		return(EINVAL);
	/*
	 * Set the address family value for the mask if it was not set.
	 * Radar 3899482.
	 */
	if (ifra->ifra_prefixmask.sin6_len == sizeof(struct sockaddr_in6) &&
		ifra->ifra_prefixmask.sin6_family == 0) {
		ifra->ifra_prefixmask.sin6_family = AF_INET6;
	}
	/*
	 * Because the IPv6 address architecture is classless, we require
	 * users to specify a (non 0) prefix length (mask) for a new address.
	 * We also require the prefix (when specified) mask is valid, and thus
	 * reject a non-consecutive mask.
	 */
	if (ia == NULL && ifra->ifra_prefixmask.sin6_len == 0)
		return(EINVAL);
	if (ifra->ifra_prefixmask.sin6_len != 0) {
		plen = in6_mask2len(&ifra->ifra_prefixmask.sin6_addr,
				    (u_char *)&ifra->ifra_prefixmask +
				    ifra->ifra_prefixmask.sin6_len);
		if (plen <= 0)
			return(EINVAL);
	}
	else {
		/*
		 * In this case, ia must not be NULL.  We just use its prefix
		 * length.
		 */
		plen = in6_mask2len(&ia->ia_prefixmask.sin6_addr, NULL);
	}
	/*
	 * If the destination address on a p2p interface is specified,
	 * and the address is a scoped one, validate/set the scope
	 * zone identifier.
	 */
	dst6 = ifra->ifra_dstaddr;
	if ((ifp->if_flags & (IFF_POINTOPOINT|IFF_LOOPBACK)) &&
	    (dst6.sin6_family == AF_INET6)) {
		int scopeid;

#ifndef SCOPEDROUTING
		if ((error = in6_recoverscope(&dst6,
					      &ifra->ifra_dstaddr.sin6_addr,
					      ifp)) != 0)
			return(error);
#endif
		scopeid = in6_addr2scopeid(ifp, &dst6.sin6_addr);
		if (dst6.sin6_scope_id == 0) /* user omit to specify the ID. */
			dst6.sin6_scope_id = scopeid;
		else if (dst6.sin6_scope_id != scopeid)
			return(EINVAL); /* scope ID mismatch. */
#ifndef SCOPEDROUTING
		if ((error = in6_embedscope(&dst6.sin6_addr, &dst6, NULL, NULL))
		    != 0)
			return(error);
		dst6.sin6_scope_id = 0; /* XXX */
#endif
	}
	/*
	 * The destination address can be specified only for a p2p or a
	 * loopback interface.  If specified, the corresponding prefix length
	 * must be 128.
	 */
	if (ifra->ifra_dstaddr.sin6_family == AF_INET6) {
		if ((ifp->if_flags & (IFF_POINTOPOINT|IFF_LOOPBACK)) == 0) {
			/* XXX: noisy message */
			log(LOG_INFO, "in6_update_ifa: a destination can be "
			    "specified for a p2p or a loopback IF only\n");
			return(EINVAL);
		}
		if (plen != 128) {
			/*
			 * The following message seems noisy, but we dare to
			 * add it for diagnosis.
			 */
			log(LOG_INFO, "in6_update_ifa: prefixlen must be 128 "
			    "when dstaddr is specified\n");
			return(EINVAL);
		}
	}
	/* lifetime consistency check */

	getmicrotime(&timenow);
	lt = &ifra->ifra_lifetime;
	if (lt->ia6t_vltime != ND6_INFINITE_LIFETIME
	    && lt->ia6t_vltime + timenow.tv_sec < timenow.tv_sec) {
		return EINVAL;
	}
	if (lt->ia6t_vltime == 0) {
		/*
		 * the following log might be noisy, but this is a typical
		 * configuration mistake or a tool's bug.
		 */
		log(LOG_INFO,
		    "in6_update_ifa: valid lifetime is 0 for %s\n",
		    ip6_sprintf(&ifra->ifra_addr.sin6_addr));
	}
	if (lt->ia6t_pltime != ND6_INFINITE_LIFETIME
	    && lt->ia6t_pltime + timenow.tv_sec < timenow.tv_sec) {
		return EINVAL;
	}

	/*
	 * If this is a new address, allocate a new ifaddr and link it
	 * into chains.
	 */
	if (ia == NULL) {
		hostIsNew = 1;
		/*
		 * in6_update_ifa() may be called in a process of a received
		 * RA; in such a case, we should call malloc with M_NOWAIT.
		 * The exception to this is during init time or as part of
		 * handling an ioctl, when we know it's okay to do M_WAITOK.
		 */
		ia = in6_ifaddr_alloc(how);
		if (ia == NULL)
			return ENOBUFS;
		/* Initialize the address and masks */
		ia->ia_ifa.ifa_addr = (struct sockaddr *)&ia->ia_addr;
		ia->ia_addr.sin6_family = AF_INET6;
		ia->ia_addr.sin6_len = sizeof(ia->ia_addr);
		if ((ifp->if_flags & (IFF_POINTOPOINT | IFF_LOOPBACK)) != 0) {
			/*
			 * XXX: some functions expect that ifa_dstaddr is not
			 * NULL for p2p interfaces.
			 */
			ia->ia_ifa.ifa_dstaddr
				= (struct sockaddr *)&ia->ia_dstaddr;
		} else {
			ia->ia_ifa.ifa_dstaddr = NULL;
		}
		ia->ia_ifa.ifa_netmask
			= (struct sockaddr *)&ia->ia_prefixmask;

		ia->ia_ifp = ifp;
		ifaref(&ia->ia_ifa);
		lck_mtx_lock(nd6_mutex);
		if ((oia = in6_ifaddrs) != NULL) {
			for ( ; oia->ia_next; oia = oia->ia_next)
				continue;
			oia->ia_next = ia;
		} else
			in6_ifaddrs = ia;
		lck_mtx_unlock(nd6_mutex);

		ifnet_lock_exclusive(ifp);
		if_attach_ifa(ifp, &ia->ia_ifa);
		ifnet_lock_done(ifp);
	}

	/* set prefix mask */
	if (ifra->ifra_prefixmask.sin6_len) {
		/*
		 * We prohibit changing the prefix length of an existing
		 * address, because
		 * + such an operation should be rare in IPv6, and
		 * + the operation would confuse prefix management.
		 */
		if (ia->ia_prefixmask.sin6_len &&
		    in6_mask2len(&ia->ia_prefixmask.sin6_addr, NULL) != plen) {
			log(LOG_INFO, "in6_update_ifa: the prefix length of an"
			    " existing (%s) address should not be changed\n",
			    ip6_sprintf(&ia->ia_addr.sin6_addr));
			error = EINVAL;
			goto unlink;
		}
		ia->ia_prefixmask = ifra->ifra_prefixmask;
	}

	/*
	 * If a new destination address is specified, scrub the old one and
	 * install the new destination.  Note that the interface must be
	 * p2p or loopback (see the check above.) 
	 */
	if (dst6.sin6_family == AF_INET6 &&
	    !IN6_ARE_ADDR_EQUAL(&dst6.sin6_addr,
				&ia->ia_dstaddr.sin6_addr)) {
		int e;

		if ((ia->ia_flags & IFA_ROUTE) != 0 &&
		    (e = rtinit(&(ia->ia_ifa), (int)RTM_DELETE, RTF_HOST))
		    != 0) {
			log(LOG_ERR, "in6_update_ifa: failed to remove "
			    "a route to the old destination: %s\n",
			    ip6_sprintf(&ia->ia_addr.sin6_addr));
			/* proceed anyway... */
		}
		else
			ia->ia_flags &= ~IFA_ROUTE;
		ia->ia_dstaddr = dst6;
	}

	/* reset the interface and routing table appropriately. */
	if ((error = in6_ifinit(ifp, ia, &ifra->ifra_addr, hostIsNew)) != 0)
		goto unlink;

	/*
	 * Beyond this point, we should call in6_purgeaddr upon an error,
	 * not just go to unlink. 
	 */

#if 0				/* disable this mechanism for now */
	/* update prefix list */
	if (hostIsNew &&
	    (ifra->ifra_flags & IN6_IFF_NOPFX) == 0) { /* XXX */
		int iilen;

		iilen = (sizeof(ia->ia_prefixmask.sin6_addr) << 3) - plen;
		if ((error = in6_prefix_add_ifid(iilen, ia)) != 0) {
			in6_purgeaddr((struct ifaddr *)ia, 0);
			return(error);
		}
	}
#endif

	if ((ifp->if_flags & IFF_MULTICAST) != 0) {
		struct sockaddr_in6 mltaddr, mltmask;
		struct in6_multi *in6m;

		if (hostIsNew) {
			/*
			 * join solicited multicast addr for new host id
			 */
			struct in6_addr llsol;
			bzero(&llsol, sizeof(struct in6_addr));
			llsol.s6_addr16[0] = htons(0xff02);
			llsol.s6_addr16[1] = htons(ifp->if_index);
			llsol.s6_addr32[1] = 0;
			llsol.s6_addr32[2] = htonl(1);
			llsol.s6_addr32[3] =
				ifra->ifra_addr.sin6_addr.s6_addr32[3];
			llsol.s6_addr8[12] = 0xff;
			(void)in6_addmulti(&llsol, ifp, &error, 0);
			if (error != 0) {
				log(LOG_WARNING,
				    "in6_update_ifa: addmulti failed for "
				    "%s on %s (errno=%d)\n",
				    ip6_sprintf(&llsol), if_name(ifp),
				    error);
				in6_purgeaddr((struct ifaddr *)ia, 0);
				return(error);
			}
		}

		bzero(&mltmask, sizeof(mltmask));
		mltmask.sin6_len = sizeof(struct sockaddr_in6);
		mltmask.sin6_family = AF_INET6;
		mltmask.sin6_addr = in6mask32;

		/*
		 * join link-local all-nodes address
		 */
		bzero(&mltaddr, sizeof(mltaddr));
		mltaddr.sin6_len = sizeof(struct sockaddr_in6);
		mltaddr.sin6_family = AF_INET6;
		mltaddr.sin6_addr = in6addr_linklocal_allnodes;
		mltaddr.sin6_addr.s6_addr16[1] = htons(ifp->if_index);

		ifnet_lock_shared(ifp);
		IN6_LOOKUP_MULTI(mltaddr.sin6_addr, ifp, in6m);
		ifnet_lock_done(ifp);
		if (in6m == NULL) {
			rtrequest(RTM_ADD,
				  (struct sockaddr *)&mltaddr,
				  (struct sockaddr *)&ia->ia_addr,
				  (struct sockaddr *)&mltmask,
				  RTF_UP|RTF_CLONING,  /* xxx */
				  (struct rtentry **)0);
			(void)in6_addmulti(&mltaddr.sin6_addr, ifp, &error, 0);
			if (error != 0) {
				log(LOG_WARNING,
				    "in6_update_ifa: addmulti failed for "
				    "%s on %s (errno=%d)\n",
				    ip6_sprintf(&mltaddr.sin6_addr), 
				    if_name(ifp), error);
			}
		}

		/*
		 * join node information group address
		 */
#define hostnamelen	strlen(hostname)
		if (in6_nigroup(ifp, hostname, hostnamelen, &mltaddr.sin6_addr)
		    == 0) {
		    ifnet_lock_shared(ifp);
			IN6_LOOKUP_MULTI(mltaddr.sin6_addr, ifp, in6m);
			ifnet_lock_done(ifp);
			if (in6m == NULL && ia != NULL) {
				(void)in6_addmulti(&mltaddr.sin6_addr,
				    ifp, &error, 0);
				if (error != 0) {
					log(LOG_WARNING, "in6_update_ifa: "
					    "addmulti failed for "
					    "%s on %s (errno=%d)\n",
					    ip6_sprintf(&mltaddr.sin6_addr), 
					    if_name(ifp), error);
				}
			}
		}
#undef hostnamelen

		/*
		 * join node-local all-nodes address, on loopback.
		 * XXX: since "node-local" is obsoleted by interface-local,
		 *      we have to join the group on every interface with
		 *      some interface-boundary restriction.
		 */
		if (ifp->if_flags & IFF_LOOPBACK) {
			struct in6_ifaddr *ia_loop;

			struct in6_addr loop6 = in6addr_loopback;
			ia_loop = in6ifa_ifpwithaddr(ifp, &loop6);

			mltaddr.sin6_addr = in6addr_nodelocal_allnodes;

			ifnet_lock_shared(ifp);
			IN6_LOOKUP_MULTI(mltaddr.sin6_addr, ifp, in6m);
			ifnet_lock_done(ifp);
			if (in6m == NULL && ia_loop != NULL) {
				rtrequest(RTM_ADD,
					  (struct sockaddr *)&mltaddr,
					  (struct sockaddr *)&ia_loop->ia_addr,
					  (struct sockaddr *)&mltmask,
					  RTF_UP,
					  (struct rtentry **)0);
				(void)in6_addmulti(&mltaddr.sin6_addr, ifp,
						   &error, 0);
				if (error != 0) {
					log(LOG_WARNING, "in6_update_ifa: "
					    "addmulti failed for %s on %s "
					    "(errno=%d)\n",
					    ip6_sprintf(&mltaddr.sin6_addr), 
					    if_name(ifp), error);
				}
			}
			if (ia_loop != NULL)
				ifafree(&ia_loop->ia_ifa);
		}
	}

	ia->ia6_flags = ifra->ifra_flags;
	ia->ia6_flags &= ~IN6_IFF_DUPLICATED;	/*safety*/
	ia->ia6_flags &= ~IN6_IFF_NODAD;	/* Mobile IPv6 */

	ia->ia6_lifetime = ifra->ifra_lifetime;
	/* for sanity */
	if (ia->ia6_lifetime.ia6t_vltime != ND6_INFINITE_LIFETIME) {
		ia->ia6_lifetime.ia6t_expire =
			timenow.tv_sec + ia->ia6_lifetime.ia6t_vltime;
	} else
		ia->ia6_lifetime.ia6t_expire = 0;
	if (ia->ia6_lifetime.ia6t_pltime != ND6_INFINITE_LIFETIME) {
		ia->ia6_lifetime.ia6t_preferred =
			timenow.tv_sec + ia->ia6_lifetime.ia6t_pltime;
	} else
		ia->ia6_lifetime.ia6t_preferred = 0;

	/*
	 * make sure to initialize ND6 information.  this is to workaround
	 * issues with interfaces with IPv6 addresses, which have never brought
	 * up.  We are assuming that it is safe to nd6_ifattach multiple times.
	 */
	if ((error = nd6_ifattach(ifp)) != 0)
		return error;

	/*
	 * Perform DAD, if needed.
	 * XXX It may be of use, if we can administratively
	 * disable DAD.
	 */
	if (in6if_do_dad(ifp) && (ifra->ifra_flags & IN6_IFF_NODAD) == 0) {
		ia->ia6_flags |= IN6_IFF_TENTATIVE;
		nd6_dad_start((struct ifaddr *)ia, NULL);
	}

	return(error);

  unlink:
	/*
	 * XXX: if a change of an existing address failed, keep the entry
	 * anyway.
	 */
	if (hostIsNew)
		in6_unlink_ifa(ia, ifp, 0);
	return(error);
}

void
in6_purgeaddr(
	struct ifaddr *ifa, int nd6_locked)
{
	struct ifnet *ifp = ifa->ifa_ifp;
	struct in6_ifaddr *ia = (struct in6_ifaddr *) ifa;

	/* stop DAD processing */
	nd6_dad_stop(ifa);

	/*
	 * delete route to the destination of the address being purged.
	 * The interface must be p2p or loopback in this case.
	 */
	if ((ia->ia_flags & IFA_ROUTE) != 0 && ia->ia_dstaddr.sin6_len != 0) {
		int e;

		if ((e = rtinit(&(ia->ia_ifa), (int)RTM_DELETE, RTF_HOST))
		    != 0) {
			log(LOG_ERR, "in6_purgeaddr: failed to remove "
			    "a route to the p2p destination: %s on %s, "
			    "errno=%d\n",
			    ip6_sprintf(&ia->ia_addr.sin6_addr), if_name(ifp),
			    e);
			/* proceed anyway... */
		}
		else
			ia->ia_flags &= ~IFA_ROUTE;
	}

	/* Remove ownaddr's loopback rtentry, if it exists. */
	in6_ifremloop(&(ia->ia_ifa), nd6_locked);

	if (ifp->if_flags & IFF_MULTICAST) {
		/*
		 * delete solicited multicast addr for deleting host id
		 */
		struct in6_multi *in6m;
		struct in6_addr llsol;
		bzero(&llsol, sizeof(struct in6_addr));
		llsol.s6_addr16[0] = htons(0xff02);
		llsol.s6_addr16[1] = htons(ifp->if_index);
		llsol.s6_addr32[1] = 0;
		llsol.s6_addr32[2] = htonl(1);
		llsol.s6_addr32[3] =
			ia->ia_addr.sin6_addr.s6_addr32[3];
		llsol.s6_addr8[12] = 0xff;

		ifnet_lock_shared(ifp);
		IN6_LOOKUP_MULTI(llsol, ifp, in6m);
		ifnet_lock_done(ifp);
		if (in6m)
			in6_delmulti(in6m, nd6_locked);
	}

	in6_unlink_ifa(ia, ifp, nd6_locked);
	in6_post_msg(ifp, KEV_INET6_ADDR_DELETED, ia);
}

static void
in6_unlink_ifa(ia, ifp, nd6_locked)
	struct in6_ifaddr *ia;
	struct ifnet *ifp;
	int nd6_locked;
{
	int plen, iilen;
	struct in6_ifaddr *oia;

	ifnet_lock_exclusive(ifp);
	if_detach_ifa(ifp, &ia->ia_ifa);
	ifnet_lock_done(ifp);

	if (!nd6_locked)
		lck_mtx_lock(nd6_mutex);
	oia = ia;
	if (oia == (ia = in6_ifaddrs)) 
		in6_ifaddrs = ia->ia_next;
	else {
		while (ia->ia_next && (ia->ia_next != oia))
			ia = ia->ia_next;
		if (ia->ia_next)
			ia->ia_next = oia->ia_next;
		else {
			/* search failed */
			printf("Couldn't unlink in6_ifaddr from in6_ifaddr\n");
		}
	}
	if (oia->ia6_ifpr) {	/* check for safety */
		plen = in6_mask2len(&oia->ia_prefixmask.sin6_addr, NULL);
		iilen = (sizeof(oia->ia_prefixmask.sin6_addr) << 3) - plen;
		in6_prefix_remove_ifid(iilen, oia);
	}

	/*
	 * When an autoconfigured address is being removed, release the
	 * reference to the base prefix.  Also, since the release might
	 * affect the status of other (detached) addresses, call
	 * pfxlist_onlink_check().
	 */
	if ((oia->ia6_flags & IN6_IFF_AUTOCONF) != 0) {
		if (oia->ia6_ndpr == NULL) {
			log(LOG_NOTICE, "in6_unlink_ifa: autoconf'ed address "
			    "%p has no prefix\n", oia);
		} else {
			oia->ia6_ndpr->ndpr_refcnt--;
			oia->ia6_flags &= ~IN6_IFF_AUTOCONF;
			oia->ia6_ndpr = NULL;
		}

		pfxlist_onlink_check(1);
	}
	if (!nd6_locked)
		lck_mtx_unlock(nd6_mutex);


	/*
	 * release another refcnt for the link from in6_ifaddrs.
	 * Note that we should decrement the refcnt at least once for all *BSD.
	 */
	ifafree(&oia->ia_ifa);

}

void
in6_purgeif(ifp)
	struct ifnet *ifp;
{
	struct in6_ifaddr *ia, *nia = NULL;

	if (ifp == NULL || &ifp->if_addrlist == NULL)
		return;
	
	lck_mtx_lock(nd6_mutex);
	for (ia = in6_ifaddrs; ia != NULL; ia = nia)
	{
		nia = ia->ia_next;
		if (ia->ia_ifa.ifa_ifp != ifp)
			continue;
		in6_purgeaddr(&ia->ia_ifa, 1);
	}
	lck_mtx_unlock(nd6_mutex);

	in6_ifdetach(ifp);
}

/*
 * SIOC[GAD]LIFADDR.
 *	SIOCGLIFADDR: get first address. (?)
 *	SIOCGLIFADDR with IFLR_PREFIX:
 *		get first address that matches the specified prefix.
 *	SIOCALIFADDR: add the specified address.
 *	SIOCALIFADDR with IFLR_PREFIX:
 *		add the specified prefix, filling hostaddr part from
 *		the first link-local address.  prefixlen must be <= 64.
 *	SIOCDLIFADDR: delete the specified address.
 *	SIOCDLIFADDR with IFLR_PREFIX:
 *		delete the first address that matches the specified prefix.
 * return values:
 *	EINVAL on invalid parameters
 *	EADDRNOTAVAIL on prefix match failed/specified address not found
 *	other values may be returned from in6_ioctl()
 *
 * NOTE: SIOCALIFADDR(with IFLR_PREFIX set) allows prefixlen less than 64.
 * this is to accomodate address naming scheme other than RFC2374,
 * in the future.
 * RFC2373 defines interface id to be 64bit, but it allows non-RFC2374
 * address encoding scheme. (see figure on page 8)
 */
static int
in6_lifaddr_ioctl(struct socket *so, u_long cmd, caddr_t data,
    struct ifnet *ifp, struct proc *p)
{
	struct if_laddrreq *iflr = (struct if_laddrreq *)data;
	struct ifaddr *ifa = NULL;
	struct sockaddr *sa;
	int p64 = proc_is64bit(p);

	/* sanity checks */
	if (!data || !ifp) {
		panic("invalid argument to in6_lifaddr_ioctl");
		/*NOTRECHED*/
	}

	switch (cmd) {
	case SIOCGLIFADDR:
		/* address must be specified on GET with IFLR_PREFIX */
		if ((iflr->flags & IFLR_PREFIX) == 0)
			break;
		/* FALLTHROUGH */
	case SIOCALIFADDR:
	case SIOCDLIFADDR:
		/* address must be specified on ADD and DELETE */
		sa = (struct sockaddr *)&iflr->addr;
		if (sa->sa_family != AF_INET6)
			return EINVAL;
		if (sa->sa_len != sizeof(struct sockaddr_in6))
			return EINVAL;
		/* XXX need improvement */
		sa = (struct sockaddr *)&iflr->dstaddr;
		if (sa->sa_family && sa->sa_family != AF_INET6)
			return EINVAL;
		if (sa->sa_len && sa->sa_len != sizeof(struct sockaddr_in6))
			return EINVAL;
		break;
	default: /* shouldn't happen */
#if 0
		panic("invalid cmd to in6_lifaddr_ioctl");
		/* NOTREACHED */
#else
		return EOPNOTSUPP;
#endif
	}
	if (sizeof(struct in6_addr) * 8 < iflr->prefixlen)
		return EINVAL;

	switch (cmd) {
	case SIOCALIFADDR:
	    {
		struct in6_aliasreq ifra;
		struct in6_addr hostaddr;
		int prefixlen;
		int hostid_found = 0;

		if ((iflr->flags & IFLR_PREFIX) != 0) {
			struct sockaddr_in6 *sin6;

			/*
			 * hostaddr is to fill in the hostaddr part of the
			 * address.  hostaddr points to the first link-local
			 * address attached to the interface.
			 */
			ifa = (struct ifaddr *)in6ifa_ifpforlinklocal(ifp, 0);
			if (!ifa)
				return EADDRNOTAVAIL;
			hostaddr = *IFA_IN6(ifa);
			hostid_found = 1;
			ifafree(ifa);
			ifa = NULL;

		 	/* prefixlen must be <= 64. */
			if (64 < iflr->prefixlen)
				return EINVAL;
			prefixlen = iflr->prefixlen;

			/* hostaddr part must be zero. */
			sin6 = (struct sockaddr_in6 *)&iflr->addr;
			if (sin6->sin6_addr.s6_addr32[2] != 0
			 || sin6->sin6_addr.s6_addr32[3] != 0) {
				return EINVAL;
			}
		} else
			prefixlen = iflr->prefixlen;

		/* copy args to in6_aliasreq, perform ioctl(SIOCAIFADDR_IN6). */
		bzero(&ifra, sizeof(ifra));
		bcopy(iflr->iflr_name, ifra.ifra_name, sizeof(ifra.ifra_name));

		bcopy(&iflr->addr, &ifra.ifra_addr,
			((struct sockaddr *)&iflr->addr)->sa_len);
		if (hostid_found) {
			/* fill in hostaddr part */
			ifra.ifra_addr.sin6_addr.s6_addr32[2] =
				hostaddr.s6_addr32[2];
			ifra.ifra_addr.sin6_addr.s6_addr32[3] =
				hostaddr.s6_addr32[3];
		}

		if (((struct sockaddr *)&iflr->dstaddr)->sa_family) {	/*XXX*/
			bcopy(&iflr->dstaddr, &ifra.ifra_dstaddr,
				((struct sockaddr *)&iflr->dstaddr)->sa_len);
			if (hostid_found) {
				ifra.ifra_dstaddr.sin6_addr.s6_addr32[2] =
					hostaddr.s6_addr32[2];
				ifra.ifra_dstaddr.sin6_addr.s6_addr32[3] =
					hostaddr.s6_addr32[3];
			}
		}

		ifra.ifra_prefixmask.sin6_len = sizeof(struct sockaddr_in6);
		in6_len2mask(&ifra.ifra_prefixmask.sin6_addr, prefixlen);

		ifra.ifra_flags = iflr->flags & ~IFLR_PREFIX;
		if (!p64) {
#if defined(__LP64__)
			struct in6_aliasreq_32 ifra_32;
			/*
			 * Use 32-bit ioctl and structure for 32-bit process.
			 */
			in6_aliasreq_64_to_32((struct in6_aliasreq_64 *)&ifra,
			    &ifra_32);
			return (in6_control(so, SIOCAIFADDR_IN6_32,
			    (caddr_t)&ifra_32, ifp, p));
#else
			return (in6_control(so, SIOCAIFADDR_IN6,
			    (caddr_t)&ifra, ifp, p));
#endif /* __LP64__ */
		} else {
#if defined(__LP64__)
			return (in6_control(so, SIOCAIFADDR_IN6,
			    (caddr_t)&ifra, ifp, p));
#else
			struct in6_aliasreq_64 ifra_64;
			/*
			 * Use 64-bit ioctl and structure for 64-bit process.
			 */
			in6_aliasreq_32_to_64((struct in6_aliasreq_32 *)&ifra,
			    &ifra_64);
			return (in6_control(so, SIOCAIFADDR_IN6_64,
			    (caddr_t)&ifra_64, ifp, p));
#endif /* __LP64__ */
		}
		/* NOTREACHED */
	    }
	case SIOCGLIFADDR:
	case SIOCDLIFADDR:
	    {
		struct in6_ifaddr *ia;
		struct in6_addr mask, candidate, match;
		struct sockaddr_in6 *sin6;
		int cmp;

		bzero(&mask, sizeof(mask));
		if (iflr->flags & IFLR_PREFIX) {
			/* lookup a prefix rather than address. */
			in6_len2mask(&mask, iflr->prefixlen);

			sin6 = (struct sockaddr_in6 *)&iflr->addr;
			bcopy(&sin6->sin6_addr, &match, sizeof(match));
			match.s6_addr32[0] &= mask.s6_addr32[0];
			match.s6_addr32[1] &= mask.s6_addr32[1];
			match.s6_addr32[2] &= mask.s6_addr32[2];
			match.s6_addr32[3] &= mask.s6_addr32[3];

			/* if you set extra bits, that's wrong */
			if (bcmp(&match, &sin6->sin6_addr, sizeof(match)))
				return EINVAL;

			cmp = 1;
		} else {
			if (cmd == SIOCGLIFADDR) {
				/* on getting an address, take the 1st match */
				cmp = 0;	/* XXX */
			} else {
				/* on deleting an address, do exact match */
				in6_len2mask(&mask, 128);
				sin6 = (struct sockaddr_in6 *)&iflr->addr;
				bcopy(&sin6->sin6_addr, &match, sizeof(match));

				cmp = 1;
			}
		}

		ifnet_lock_shared(ifp);
		TAILQ_FOREACH(ifa, &ifp->if_addrlist, ifa_list)
		{
			if (ifa->ifa_addr->sa_family != AF_INET6)
				continue;
			if (!cmp)
				break;

			bcopy(IFA_IN6(ifa), &candidate, sizeof(candidate));
#ifndef SCOPEDROUTING
			/*
			 * XXX: this is adhoc, but is necessary to allow
			 * a user to specify fe80::/64 (not /10) for a
			 * link-local address.
			 */
			if (IN6_IS_ADDR_LINKLOCAL(&candidate))
				candidate.s6_addr16[1] = 0;
#endif
			candidate.s6_addr32[0] &= mask.s6_addr32[0];
			candidate.s6_addr32[1] &= mask.s6_addr32[1];
			candidate.s6_addr32[2] &= mask.s6_addr32[2];
			candidate.s6_addr32[3] &= mask.s6_addr32[3];
			if (IN6_ARE_ADDR_EQUAL(&candidate, &match))
				break;
		}
		ifnet_lock_done(ifp);
		if (!ifa)
			return EADDRNOTAVAIL;
		ia = ifa2ia6(ifa);

		if (cmd == SIOCGLIFADDR) {
#ifndef SCOPEDROUTING
			struct sockaddr_in6 *s6;
#endif

			/* fill in the if_laddrreq structure */
			bcopy(&ia->ia_addr, &iflr->addr, ia->ia_addr.sin6_len);
#ifndef SCOPEDROUTING		/* XXX see above */
			s6 = (struct sockaddr_in6 *)&iflr->addr;
			if (IN6_IS_ADDR_LINKLOCAL(&s6->sin6_addr)) {
				s6->sin6_addr.s6_addr16[1] = 0;
				s6->sin6_scope_id =
					in6_addr2scopeid(ifp, &s6->sin6_addr);
			}
#endif
			if ((ifp->if_flags & IFF_POINTOPOINT) != 0) {
				bcopy(&ia->ia_dstaddr, &iflr->dstaddr,
					ia->ia_dstaddr.sin6_len);
#ifndef SCOPEDROUTING		/* XXX see above */
				s6 = (struct sockaddr_in6 *)&iflr->dstaddr;
				if (IN6_IS_ADDR_LINKLOCAL(&s6->sin6_addr)) {
					s6->sin6_addr.s6_addr16[1] = 0;
					s6->sin6_scope_id =
						in6_addr2scopeid(ifp,
								 &s6->sin6_addr);
				}
#endif
			} else
				bzero(&iflr->dstaddr, sizeof(iflr->dstaddr));

			iflr->prefixlen =
				in6_mask2len(&ia->ia_prefixmask.sin6_addr,
					     NULL);

			iflr->flags = ia->ia6_flags;	/* XXX */

			return 0;
		} else {
			struct in6_aliasreq ifra;

			/* fill in6_aliasreq and do ioctl(SIOCDIFADDR_IN6) */
			bzero(&ifra, sizeof(ifra));
			bcopy(iflr->iflr_name, ifra.ifra_name,
				sizeof(ifra.ifra_name));

			bcopy(&ia->ia_addr, &ifra.ifra_addr,
				ia->ia_addr.sin6_len);
			if ((ifp->if_flags & IFF_POINTOPOINT) != 0) {
				bcopy(&ia->ia_dstaddr, &ifra.ifra_dstaddr,
					ia->ia_dstaddr.sin6_len);
			} else {
				bzero(&ifra.ifra_dstaddr,
				    sizeof(ifra.ifra_dstaddr));
			}
			bcopy(&ia->ia_prefixmask, &ifra.ifra_dstaddr,
				ia->ia_prefixmask.sin6_len);

			ifra.ifra_flags = ia->ia6_flags;
			if (!p64) {
#if defined(__LP64__)
				struct in6_aliasreq_32 ifra_32;
				/*
				 * Use 32-bit structure for 32-bit process.
				 * SIOCDIFADDR_IN6 is encoded with in6_ifreq,
				 * so it stays the same since the size does
				 * not change.  The data part of the ioctl,
				 * however, is of a different structure, i.e.
				 * in6_aliasreq.
				 */
				in6_aliasreq_64_to_32(
				    (struct in6_aliasreq_64 *)&ifra, &ifra_32);
				return (in6_control(so, SIOCDIFADDR_IN6,
				    (caddr_t)&ifra_32, ifp, p));
#else
				return (in6_control(so, SIOCDIFADDR_IN6,
				    (caddr_t)&ifra, ifp, p));
#endif /* __LP64__ */
			} else {
#if defined(__LP64__)
				return (in6_control(so, SIOCDIFADDR_IN6,
				    (caddr_t)&ifra, ifp, p));
#else
				struct in6_aliasreq_64 ifra_64;
				/*
				 * Use 64-bit structure for 64-bit process.
				 * SIOCDIFADDR_IN6 is encoded with in6_ifreq,
				 * so it stays the same since the size does
				 * not change.  The data part of the ioctl,
				 * however, is of a different structure, i.e.
				 * in6_aliasreq.
				 */
				in6_aliasreq_32_to_64(
				    (struct in6_aliasreq_32 *)&ifra, &ifra_64);
				return (in6_control(so, SIOCDIFADDR_IN6,
				    (caddr_t)&ifra_64, ifp, p));
#endif /* __LP64__ */
			}
			/* NOTREACHED */
		}
	    }
	}

	return EOPNOTSUPP;	/* just for safety */
}

/*
 * Initialize an interface's intetnet6 address
 * and routing table entry.
 */
static int
in6_ifinit(ifp, ia, sin6, newhost)
	struct ifnet *ifp;
	struct in6_ifaddr *ia;
	struct sockaddr_in6 *sin6;
	int newhost;
{
	int	error = 0, plen, ifacount = 0;
	struct ifaddr *ifa;

	/*
	 * Give the interface a chance to initialize
	 * if this is its first address,
	 * and to validate the address if necessary.
	 */
	ifnet_lock_shared(ifp);
	TAILQ_FOREACH(ifa, &ifp->if_addrlist, ifa_list)
	{
		if (ifa->ifa_addr == NULL)
			continue;	/* just for safety */
		if (ifa->ifa_addr->sa_family != AF_INET6)
			continue;
		ifacount++;
	}
	ifnet_lock_done(ifp);

	ia->ia_addr = *sin6;


	if (ifacount <= 1 && 
	    (error = ifnet_ioctl(ifp, PF_INET6, SIOCSIFADDR, ia))) {
		if (error) {
			return(error);
		}
	}

	ia->ia_ifa.ifa_metric = ifp->if_metric;

	/* we could do in(6)_socktrim here, but just omit it at this moment. */

	/*
	 * Special case:
	 * If the destination address is specified for a point-to-point
	 * interface, install a route to the destination as an interface
	 * direct route.
	 */
	plen = in6_mask2len(&ia->ia_prefixmask.sin6_addr, NULL); /* XXX */
	if (plen == 128 && ia->ia_dstaddr.sin6_family == AF_INET6) {
		if ((error = rtinit(&(ia->ia_ifa), (int)RTM_ADD,
				    RTF_UP | RTF_HOST)) != 0)
			return(error);
		ia->ia_flags |= IFA_ROUTE;
	}
	if (plen < 128) {
		/*
		 * The RTF_CLONING flag is necessary for in6_is_ifloop_auto().
		 */
		ia->ia_ifa.ifa_flags |= RTF_CLONING;
	}

	/* Add ownaddr as loopback rtentry, if necessary (ex. on p2p link). */
	if (newhost) {
		/* set the rtrequest function to create llinfo */
		ia->ia_ifa.ifa_rtrequest = nd6_rtrequest;
		in6_ifaddloop(&(ia->ia_ifa));
	}

	return(error);
}

/*
 * Add an address to the list of IP6 multicast addresses for a
 * given interface.
 */
struct	in6_multi *
in6_addmulti(maddr6, ifp, errorp, nd6_locked)
	struct in6_addr *maddr6;
	struct ifnet *ifp;
	int *errorp;
	int nd6_locked;
{
	struct	in6_multi *in6m;
	struct sockaddr_in6 sin6;
	struct ifmultiaddr *ifma;

	*errorp = 0;

	/*
	 * Call generic routine to add membership or increment
	 * refcount.  It wants addresses in the form of a sockaddr,
	 * so we build one here (being careful to zero the unused bytes).
	 */
	bzero(&sin6, sizeof sin6);
	sin6.sin6_family = AF_INET6;
	sin6.sin6_len = sizeof sin6;
	sin6.sin6_addr = *maddr6;
	*errorp = if_addmulti(ifp, (struct sockaddr *)&sin6, &ifma);
	if (*errorp) {
		return 0;
	}

	/*
	 * If ifma->ifma_protospec is null, then if_addmulti() created
	 * a new record.  Otherwise, we are done.
	 */
	if (ifma->ifma_protospec != 0)
		return ifma->ifma_protospec;

	/* XXX - if_addmulti uses M_WAITOK.  Can this really be called
	   at interrupt time?  If so, need to fix if_addmulti. XXX */
	in6m = (struct in6_multi *)_MALLOC(sizeof(*in6m), M_IPMADDR, M_NOWAIT);
	if (in6m == NULL) {
		return (NULL);
	}

	bzero(in6m, sizeof *in6m);
	in6m->in6m_addr = *maddr6;
	in6m->in6m_ifp = ifp;
	in6m->in6m_ifma = ifma;
	ifma->ifma_protospec = in6m;
	if (nd6_locked == 0)
		lck_mtx_lock(nd6_mutex);
	LIST_INSERT_HEAD(&in6_multihead, in6m, in6m_entry);
	if (nd6_locked == 0)
		lck_mtx_unlock(nd6_mutex);

	/*
	 * Let MLD6 know that we have joined a new IP6 multicast
	 * group.
	 */
	mld6_start_listening(in6m);
	return(in6m);
}

/*
 * Delete a multicast address record.
 */
void
in6_delmulti(
	struct in6_multi *in6m, int nd6locked)
{
	struct ifmultiaddr *ifma = in6m->in6m_ifma;

	if (ifma && ifma->ifma_usecount == 1) {
		/*
		 * No remaining claims to this record; let MLD6 know
		 * that we are leaving the multicast group.
		 */
		mld6_stop_listening(in6m);
		ifma->ifma_protospec = 0;
		if (nd6locked == 0)
			lck_mtx_lock(nd6_mutex);
		LIST_REMOVE(in6m, in6m_entry);
		if (nd6locked == 0)
			lck_mtx_unlock(nd6_mutex);
		FREE(in6m, M_IPMADDR);
	}
	/* XXX - should be separate API for when we have an ifma? */
	if (ifma) {
		if_delmultiaddr(ifma, 0);
		ifma_release(ifma);
	}
}

/*
 * Find an IPv6 interface link-local address specific to an interface.
 */
struct in6_ifaddr *
in6ifa_ifpforlinklocal(ifp, ignoreflags)
	struct ifnet *ifp;
	int ignoreflags;
{
	struct ifaddr *ifa;

	ifnet_lock_shared(ifp);
	TAILQ_FOREACH(ifa, &ifp->if_addrlist, ifa_list)
	{
		if (ifa->ifa_addr == NULL)
			continue;	/* just for safety */
		if (ifa->ifa_addr->sa_family != AF_INET6)
			continue;
		if (IN6_IS_ADDR_LINKLOCAL(IFA_IN6(ifa))) {
			if ((((struct in6_ifaddr *)ifa)->ia6_flags &
			     ignoreflags) != 0)
				continue;
			break;
		}
	}
	if (ifa != NULL)
		ifaref(ifa);
	ifnet_lock_done(ifp);

	return((struct in6_ifaddr *)ifa);
}

/*
 * find the internet address corresponding to a given interface and address.
 */
struct in6_ifaddr *
in6ifa_ifpwithaddr(ifp, addr)
	struct ifnet *ifp;
	struct in6_addr *addr;
{
	struct ifaddr *ifa;

	ifnet_lock_shared(ifp);
	TAILQ_FOREACH(ifa, &ifp->if_addrlist, ifa_list)
	{
		if (ifa->ifa_addr == NULL)
			continue;	/* just for safety */
		if (ifa->ifa_addr->sa_family != AF_INET6)
			continue;
		if (IN6_ARE_ADDR_EQUAL(addr, IFA_IN6(ifa)))
			break;
	}
	if (ifa != NULL)
		ifaref(ifa);
	ifnet_lock_done(ifp);

	return((struct in6_ifaddr *)ifa);
}

/*
 * Convert IP6 address to printable (loggable) representation.
 */
static char digits[] = "0123456789abcdef";
static int ip6round = 0;
char *
ip6_sprintf(addr)
	const struct in6_addr *addr;
{
	static char ip6buf[8][48];
	int i;
	char *cp;
	const u_short *a = (const u_short *)addr;
	const u_char *d;
	int dcolon = 0;

	ip6round = (ip6round + 1) & 7;
	cp = ip6buf[ip6round];

	for (i = 0; i < 8; i++) {
		if (dcolon == 1) {
			if (*a == 0) {
				if (i == 7)
					*cp++ = ':';
				a++;
				continue;
			} else
				dcolon = 2;
		}
		if (*a == 0) {
			if (dcolon == 0 && *(a + 1) == 0) {
				if (i == 0)
					*cp++ = ':';
				*cp++ = ':';
				dcolon = 1;
			} else {
				*cp++ = '0';
				*cp++ = ':';
			}
			a++;
			continue;
		}
		d = (const u_char *)a;
		*cp++ = digits[*d >> 4];
		*cp++ = digits[*d++ & 0xf];
		*cp++ = digits[*d >> 4];
		*cp++ = digits[*d & 0xf];
		*cp++ = ':';
		a++;
	}
	*--cp = 0;
	return(ip6buf[ip6round]);
}

int
in6addr_local(struct in6_addr *in6)
{
	struct rtentry *rt;
	struct sockaddr_in6 sin6;
	int local = 0;

	if (IN6_IS_ADDR_LOOPBACK(in6) || IN6_IS_ADDR_LINKLOCAL(in6))
		return (1);

	sin6.sin6_family = AF_INET6;
	sin6.sin6_len = sizeof (sin6);
	bcopy(in6, &sin6.sin6_addr, sizeof (*in6));
	rt = rtalloc1((struct sockaddr *)&sin6, 0, 0);

	if (rt != NULL) {
		RT_LOCK_SPIN(rt);
		if (rt->rt_gateway->sa_family == AF_LINK)
			local = 1;
		RT_UNLOCK(rt);
		rtfree(rt);
	} else {
		local = in6_localaddr(in6);
	}
	return (local);
}

int
in6_localaddr(in6)
	struct in6_addr *in6;
{
	struct in6_ifaddr *ia;

	if (IN6_IS_ADDR_LOOPBACK(in6) || IN6_IS_ADDR_LINKLOCAL(in6))
		return 1;

	lck_mtx_lock(nd6_mutex);
	for (ia = in6_ifaddrs; ia; ia = ia->ia_next)
		if (IN6_ARE_MASKED_ADDR_EQUAL(in6, &ia->ia_addr.sin6_addr,
					      &ia->ia_prefixmask.sin6_addr)) {
			lck_mtx_unlock(nd6_mutex);
			return 1;
		}

	lck_mtx_unlock(nd6_mutex);
	return (0);
}

int
in6_is_addr_deprecated(sa6)
	struct sockaddr_in6 *sa6;
{
	struct in6_ifaddr *ia;

	lck_mtx_lock(nd6_mutex);
	for (ia = in6_ifaddrs; ia; ia = ia->ia_next) {
		if (IN6_ARE_ADDR_EQUAL(&ia->ia_addr.sin6_addr,
				       &sa6->sin6_addr) &&
#if SCOPEDROUTING
		    ia->ia_addr.sin6_scope_id == sa6->sin6_scope_id &&
#endif
		    (ia->ia6_flags & IN6_IFF_DEPRECATED) != 0) {
			lck_mtx_unlock(nd6_mutex);
			return(1); /* true */
		}

		/* XXX: do we still have to go thru the rest of the list? */
	}

	lck_mtx_unlock(nd6_mutex);
	return(0);		/* false */
}

/*
 * return length of part which dst and src are equal
 * hard coding...
 */
int
in6_matchlen(src, dst)
struct in6_addr *src, *dst;
{
	int match = 0;
	u_char *s = (u_char *)src, *d = (u_char *)dst;
	u_char *lim = s + 16, r;

	while (s < lim)
		if ((r = (*d++ ^ *s++)) != 0) {
			while (r < 128) {
				match++;
				r <<= 1;
			}
			break;
		} else
			match += 8;
	return match;
}

/* XXX: to be scope conscious */
int
in6_are_prefix_equal(p1, p2, len)
	struct in6_addr *p1, *p2;
	int len;
{
	int bytelen, bitlen;

	/* sanity check */
	if (0 > len || len > 128) {
		log(LOG_ERR, "in6_are_prefix_equal: invalid prefix length(%d)\n",
		    len);
		return(0);
	}

	bytelen = len / 8;
	bitlen = len % 8;

	if (bcmp(&p1->s6_addr, &p2->s6_addr, bytelen))
		return(0);
	if (bitlen != 0 && 
	    p1->s6_addr[bytelen] >> (8 - bitlen) !=
	    p2->s6_addr[bytelen] >> (8 - bitlen))
		return(0);

	return(1);
}

void
in6_prefixlen2mask(maskp, len)
	struct in6_addr *maskp;
	int len;
{
	u_char maskarray[8] = {0x80, 0xc0, 0xe0, 0xf0, 0xf8, 0xfc, 0xfe, 0xff};
	int bytelen, bitlen, i;

	/* sanity check */
	if (0 > len || len > 128) {
		log(LOG_ERR, "in6_prefixlen2mask: invalid prefix length(%d)\n",
		    len);
		return;
	}

	bzero(maskp, sizeof(*maskp));
	bytelen = len / 8;
	bitlen = len % 8;
	for (i = 0; i < bytelen; i++)
		maskp->s6_addr[i] = 0xff;
	if (bitlen)
		maskp->s6_addr[bytelen] = maskarray[bitlen - 1];
}

/*
 * return the best address out of the same scope
 */
struct in6_ifaddr *
in6_ifawithscope(
	struct ifnet *oifp,
	struct in6_addr *dst)
{
	int dst_scope =	in6_addrscope(dst), src_scope, best_scope = 0;
	int blen = -1;
	struct ifaddr *ifa;
	struct ifnet *ifp;
	struct in6_ifaddr *ifa_best = NULL;
	
	if (oifp == NULL) {
#if 0
		printf("in6_ifawithscope: output interface is not specified\n");
#endif
		return(NULL);
	}

	/*
	 * We search for all addresses on all interfaces from the beginning.
	 * Comparing an interface with the outgoing interface will be done
	 * only at the final stage of tiebreaking.
	 */
	ifnet_head_lock_shared();
	TAILQ_FOREACH(ifp, &ifnet_head, if_list) {
		/*
		 * We can never take an address that breaks the scope zone
		 * of the destination.
		 */
		if (in6_addr2scopeid(ifp, dst) != in6_addr2scopeid(oifp, dst))
			continue;

		ifnet_lock_shared(ifp);
		TAILQ_FOREACH(ifa, &ifp->if_addrlist, ifa_list)
		{
			int tlen = -1, dscopecmp, bscopecmp, matchcmp;

			if (ifa->ifa_addr->sa_family != AF_INET6)
				continue;

			src_scope = in6_addrscope(IFA_IN6(ifa));

			/*
			 * Don't use an address before completing DAD
			 * nor a duplicated address.
			 */
			if (((struct in6_ifaddr *)ifa)->ia6_flags &
			    IN6_IFF_NOTREADY)
				continue;

			/* XXX: is there any case to allow anycasts? */
			if (((struct in6_ifaddr *)ifa)->ia6_flags &
			    IN6_IFF_ANYCAST)
				continue;

			if (((struct in6_ifaddr *)ifa)->ia6_flags &
			    IN6_IFF_DETACHED)
				continue;

			/*
			 * If this is the first address we find,
			 * keep it anyway.
			 */
			if (ifa_best == NULL)
				goto replace;

			/*
			 * ifa_best is never NULL beyond this line except
			 * within the block labeled "replace".
			 */

			/*
			 * If ifa_best has a smaller scope than dst and
			 * the current address has a larger one than
			 * (or equal to) dst, always replace ifa_best.
			 * Also, if the current address has a smaller scope
			 * than dst, ignore it unless ifa_best also has a
			 * smaller scope.
			 * Consequently, after the two if-clause below,
			 * the followings must be satisfied:
			 * (scope(src) < scope(dst) &&
			 *  scope(best) < scope(dst))
			 *  OR
			 * (scope(best) >= scope(dst) &&
			 *  scope(src) >= scope(dst))
			 */
			if (IN6_ARE_SCOPE_CMP(best_scope, dst_scope) < 0 &&
			    IN6_ARE_SCOPE_CMP(src_scope, dst_scope) >= 0)
				goto replace; /* (A) */
			if (IN6_ARE_SCOPE_CMP(src_scope, dst_scope) < 0 &&
			    IN6_ARE_SCOPE_CMP(best_scope, dst_scope) >= 0)
				continue; /* (B) */

			/*
			 * A deprecated address SHOULD NOT be used in new
			 * communications if an alternate (non-deprecated)
			 * address is available and has sufficient scope.
			 * RFC 2462, Section 5.5.4.
			 */
			if (((struct in6_ifaddr *)ifa)->ia6_flags &
			    IN6_IFF_DEPRECATED) {
				/*
				 * Ignore any deprecated addresses if
				 * specified by configuration.
				 */
				if (!ip6_use_deprecated)
					continue;

				/*
				 * If we have already found a non-deprecated
				 * candidate, just ignore deprecated addresses.
				 */
				if ((ifa_best->ia6_flags & IN6_IFF_DEPRECATED)
				    == 0)
					continue;
			}

			/*
			 * A non-deprecated address is always preferred
			 * to a deprecated one regardless of scopes and
			 * address matching (Note invariants ensured by the
			 * conditions (A) and (B) above.)
			 */
			if ((ifa_best->ia6_flags & IN6_IFF_DEPRECATED) &&
			    (((struct in6_ifaddr *)ifa)->ia6_flags &
			     IN6_IFF_DEPRECATED) == 0)
				goto replace;

			/*
			 * When we use temporary addresses described in
			 * RFC 3041, we prefer temporary addresses to
			 * public autoconf addresses.  Again, note the
			 * invariants from (A) and (B).  Also note that we
			 * don't have any preference between static addresses
			 * and autoconf addresses (despite of whether or not
			 * the latter is temporary or public.)
			 */
			if (ip6_use_tempaddr) {
				struct in6_ifaddr *ifat;

				ifat = (struct in6_ifaddr *)ifa;
				if ((ifa_best->ia6_flags &
				     (IN6_IFF_AUTOCONF|IN6_IFF_TEMPORARY))
				     == IN6_IFF_AUTOCONF &&
				    (ifat->ia6_flags &
				     (IN6_IFF_AUTOCONF|IN6_IFF_TEMPORARY))
				     == (IN6_IFF_AUTOCONF|IN6_IFF_TEMPORARY)) {
					goto replace;
				}
				if ((ifa_best->ia6_flags &
				     (IN6_IFF_AUTOCONF|IN6_IFF_TEMPORARY))
				    == (IN6_IFF_AUTOCONF|IN6_IFF_TEMPORARY) &&
				    (ifat->ia6_flags &
				     (IN6_IFF_AUTOCONF|IN6_IFF_TEMPORARY))
				     == IN6_IFF_AUTOCONF) {
					continue;
				}
			}

			/*
			 * At this point, we have two cases:
			 * 1. we are looking at a non-deprecated address,
			 *    and ifa_best is also non-deprecated.
			 * 2. we are looking at a deprecated address,
			 *    and ifa_best is also deprecated.
			 * Also, we do not have to consider a case where
			 * the scope of if_best is larger(smaller) than dst and
			 * the scope of the current address is smaller(larger)
			 * than dst. Such a case has already been covered.
			 * Tiebreaking is done according to the following
			 * items:
			 * - the scope comparison between the address and
			 *   dst (dscopecmp)
			 * - the scope comparison between the address and
			 *   ifa_best (bscopecmp)
			 * - if the address match dst longer than ifa_best
			 *   (matchcmp)
			 * - if the address is on the outgoing I/F (outI/F)
			 *
			 * Roughly speaking, the selection policy is
			 * - the most important item is scope. The same scope
			 *   is best. Then search for a larger scope.
			 *   Smaller scopes are the last resort.
			 * - A deprecated address is chosen only when we have
			 *   no address that has an enough scope, but is
			 *   prefered to any addresses of smaller scopes
			 *   (this must be already done above.)
			 * - addresses on the outgoing I/F are preferred to
			 *   ones on other interfaces if none of above
			 *   tiebreaks.  In the table below, the column "bI"
			 *   means if the best_ifa is on the outgoing
			 *   interface, and the column "sI" means if the ifa
			 *   is on the outgoing interface.
			 * - If there is no other reasons to choose one,
			 *   longest address match against dst is considered.
			 *
			 * The precise decision table is as follows:
			 * dscopecmp bscopecmp    match  bI oI | replace?
			 *       N/A     equal      N/A   Y  N |   No (1)
			 *       N/A     equal      N/A   N  Y |  Yes (2)
			 *       N/A     equal   larger    N/A |  Yes (3)
			 *       N/A     equal  !larger    N/A |   No (4)
			 *    larger    larger      N/A    N/A |   No (5)
			 *    larger   smaller      N/A    N/A |  Yes (6)
			 *   smaller    larger      N/A    N/A |  Yes (7)
			 *   smaller   smaller      N/A    N/A |   No (8)
			 *     equal   smaller      N/A    N/A |  Yes (9)
			 *     equal    larger       (already done at A above)
			 */
			dscopecmp = IN6_ARE_SCOPE_CMP(src_scope, dst_scope);
			bscopecmp = IN6_ARE_SCOPE_CMP(src_scope, best_scope);

			if (bscopecmp == 0) {
				struct ifnet *bifp = ifa_best->ia_ifp;

				if (bifp == oifp && ifp != oifp) /* (1) */
					continue;
				if (bifp != oifp && ifp == oifp) /* (2) */
					goto replace;

				/*
				 * Both bifp and ifp are on the outgoing
				 * interface, or both two are on a different
				 * interface from the outgoing I/F.
				 * now we need address matching against dst
				 * for tiebreaking.
				 */
				tlen = in6_matchlen(IFA_IN6(ifa), dst);
				matchcmp = tlen - blen;
				if (matchcmp > 0) /* (3) */
					goto replace;
				continue; /* (4) */
			}
			if (dscopecmp > 0) {
				if (bscopecmp > 0) /* (5) */
					continue;
				goto replace; /* (6) */
			}
			if (dscopecmp < 0) {
				if (bscopecmp > 0) /* (7) */
					goto replace;
				continue; /* (8) */
			}

			/* now dscopecmp must be 0 */
			if (bscopecmp < 0)
				goto replace; /* (9) */

		  replace:
		  	ifaref(ifa);
		  	if (ifa_best)
		  		ifafree(&ifa_best->ia_ifa);
			ifa_best = (struct in6_ifaddr *)ifa;
			blen = tlen >= 0 ? tlen :
				in6_matchlen(IFA_IN6(ifa), dst);
			best_scope = in6_addrscope(&ifa_best->ia_addr.sin6_addr);
		}
		ifnet_lock_done(ifp);
	}
	ifnet_head_done();

	/* count statistics for future improvements */
	if (ifa_best == NULL)
		ip6stat.ip6s_sources_none++;
	else {
		if (oifp == ifa_best->ia_ifp)
			ip6stat.ip6s_sources_sameif[best_scope]++;
		else
			ip6stat.ip6s_sources_otherif[best_scope]++;

		if (best_scope == dst_scope)
			ip6stat.ip6s_sources_samescope[best_scope]++;
		else
			ip6stat.ip6s_sources_otherscope[best_scope]++;

		if ((ifa_best->ia6_flags & IN6_IFF_DEPRECATED) != 0)
			ip6stat.ip6s_sources_deprecated[best_scope]++;
	}

	return(ifa_best);
}

/*
 * return the best address out of the same scope. if no address was
 * found, return the first valid address from designated IF.
 */
struct in6_ifaddr *
in6_ifawithifp(
	struct ifnet *ifp,
	struct in6_addr *dst)
{
	int dst_scope =	in6_addrscope(dst), blen = -1, tlen;
	struct ifaddr *ifa;
	struct in6_ifaddr *besta = 0;
	struct in6_ifaddr *dep[2];	/* last-resort: deprecated */

	dep[0] = dep[1] = NULL;

	/*
	 * We first look for addresses in the same scope.
	 * If there is one, return it.
	 * If two or more, return one which matches the dst longest.
	 * If none, return one of global addresses assigned other ifs.
	 */
	ifnet_lock_shared(ifp);
	TAILQ_FOREACH(ifa, &ifp->if_addrlist, ifa_list)
	{
		if (ifa->ifa_addr->sa_family != AF_INET6)
			continue;
		if (((struct in6_ifaddr *)ifa)->ia6_flags & IN6_IFF_ANYCAST)
			continue; /* XXX: is there any case to allow anycast? */
		if (((struct in6_ifaddr *)ifa)->ia6_flags & IN6_IFF_NOTREADY)
			continue; /* don't use this interface */
		if (((struct in6_ifaddr *)ifa)->ia6_flags & IN6_IFF_DETACHED)
			continue;
		if (((struct in6_ifaddr *)ifa)->ia6_flags & IN6_IFF_DEPRECATED) {
			if (ip6_use_deprecated) {
				if (dep[0] != NULL)
					ifafree(&dep[0]->ia_ifa);
				dep[0] = (struct in6_ifaddr *)ifa;
				ifaref(ifa);
			}
			continue;
		}

		if (dst_scope == in6_addrscope(IFA_IN6(ifa))) {
			/*
			 * call in6_matchlen() as few as possible
			 */
			if (besta) {
				if (blen == -1)
					blen = in6_matchlen(&besta->ia_addr.sin6_addr, dst);
				tlen = in6_matchlen(IFA_IN6(ifa), dst);
				if (tlen > blen) {
					blen = tlen;
					besta = (struct in6_ifaddr *)ifa;
				}
			} else
				besta = (struct in6_ifaddr *)ifa;
		}
	}
	if (besta) {
		ifaref(&besta->ia_ifa);
		ifnet_lock_done(ifp);
		if (dep[0] != NULL)
			ifafree(&dep[0]->ia_ifa);
		return(besta);
	}

	TAILQ_FOREACH(ifa, &ifp->if_addrlist, ifa_list)
	{
		if (ifa->ifa_addr->sa_family != AF_INET6)
			continue;
		if (((struct in6_ifaddr *)ifa)->ia6_flags & IN6_IFF_ANYCAST)
			continue; /* XXX: is there any case to allow anycast? */
		if (((struct in6_ifaddr *)ifa)->ia6_flags & IN6_IFF_NOTREADY)
			continue; /* don't use this interface */
		if (((struct in6_ifaddr *)ifa)->ia6_flags & IN6_IFF_DETACHED)
			continue;
		if (((struct in6_ifaddr *)ifa)->ia6_flags & IN6_IFF_DEPRECATED) {
			if (ip6_use_deprecated) {
				if (dep[1] != NULL)
					ifafree(&dep[1]->ia_ifa);
				dep[1] = (struct in6_ifaddr *)ifa;
				ifaref(ifa);
			}
			continue;
		}
		if (ifa != NULL)
			ifaref(ifa);
		ifnet_lock_done(ifp);
		if (dep[0] != NULL)
			ifafree(&dep[0]->ia_ifa);
		if (dep[1] != NULL)
			ifafree(&dep[1]->ia_ifa);
		return (struct in6_ifaddr *)ifa;
	}
	ifnet_lock_done(ifp);

	/* use the last-resort values, that are, deprecated addresses */
	if (dep[0]) {
		if (dep[1] != NULL)
			ifafree(&dep[1]->ia_ifa);
		return dep[0];
	}
	if (dep[1])
		return dep[1];

	return NULL;
}

/*
 * perform DAD when interface becomes IFF_UP.
 */
int
in6_if_up(
	struct ifnet *ifp,
	struct in6_aliasreq *ifra)
{
	struct ifaddr *ifa;
	struct in6_ifaddr *ia;
	int dad_delay;		/* delay ticks before DAD output */
	int error;

	if (!in6_init2done)
		return ENXIO;

	/*
	 * special cases, like 6to4, are handled in in6_ifattach
	 */
	error = in6_ifattach(ifp, NULL, ifra);
	if (error != 0)
		return error;

	dad_delay = 0;
	ifnet_lock_exclusive(ifp);
	TAILQ_FOREACH(ifa, &ifp->if_addrlist, ifa_list)
	{
		if (ifa->ifa_addr->sa_family != AF_INET6)
			continue;
		ia = (struct in6_ifaddr *)ifa;
		if (ia->ia6_flags & IN6_IFF_TENTATIVE)
			nd6_dad_start(ifa, &dad_delay);
	}
	ifnet_lock_done(ifp);

	return 0;
}

int
in6if_do_dad(
	struct ifnet *ifp)
{
	if ((ifp->if_flags & IFF_LOOPBACK) != 0)
		return(0);

	switch (ifp->if_type) {
#if IFT_DUMMY
	case IFT_DUMMY:
#endif
	case IFT_FAITH:
		/*
		 * These interfaces do not have the IFF_LOOPBACK flag,
		 * but loop packets back.  We do not have to do DAD on such
		 * interfaces.  We should even omit it, because loop-backed
		 * NS would confuse the DAD procedure.
		 */
		return(0);
	default:
		/*
		 * Our DAD routine requires the interface up and running.
		 * However, some interfaces can be up before the RUNNING
		 * status.  Additionaly, users may try to assign addresses
		 * before the interface becomes up (or running).
		 * We simply skip DAD in such a case as a work around.
		 * XXX: we should rather mark "tentative" on such addresses,
		 * and do DAD after the interface becomes ready.
		 */
		if ((ifp->if_flags & (IFF_UP|IFF_RUNNING)) !=
		    (IFF_UP|IFF_RUNNING))
			return(0);

		return(1);
	}
}

/*
 * Calculate max IPv6 MTU through all the interfaces and store it
 * to in6_maxmtu.
 */
void
in6_setmaxmtu()
{
	u_int32_t maxmtu = 0;
	struct ifnet *ifp;

	ifnet_head_lock_shared();
	TAILQ_FOREACH(ifp, &ifnet_head, if_list) {
		lck_rw_lock_shared(nd_if_rwlock);
		if ((ifp->if_flags & IFF_LOOPBACK) == 0 &&
		    IN6_LINKMTU(ifp) > maxmtu)
			maxmtu = IN6_LINKMTU(ifp);
		lck_rw_done(nd_if_rwlock);
	}
	ifnet_head_done();
	if (maxmtu)	/* update only when maxmtu is positive */
		in6_maxmtu = maxmtu;
}

/*
 * Convert sockaddr_in6 to sockaddr_in.  Original sockaddr_in6 must be
 * v4 mapped addr or v4 compat addr
 */
void
in6_sin6_2_sin(struct sockaddr_in *sin, struct sockaddr_in6 *sin6)
{
	bzero(sin, sizeof(*sin));
	sin->sin_len = sizeof(struct sockaddr_in);
	sin->sin_family = AF_INET;
	sin->sin_port = sin6->sin6_port;
	sin->sin_addr.s_addr = sin6->sin6_addr.s6_addr32[3];	
}

/* Convert sockaddr_in to sockaddr_in6 in v4 mapped addr format. */
void
in6_sin_2_v4mapsin6(struct sockaddr_in *sin, struct sockaddr_in6 *sin6)
{
	bzero(sin6, sizeof(*sin6));
	sin6->sin6_len = sizeof(struct sockaddr_in6);
	sin6->sin6_family = AF_INET6;
	sin6->sin6_port = sin->sin_port;
	sin6->sin6_addr.s6_addr32[0] = 0;
	sin6->sin6_addr.s6_addr32[1] = 0;
	sin6->sin6_addr.s6_addr32[2] = IPV6_ADDR_INT32_SMP;
	sin6->sin6_addr.s6_addr32[3] = sin->sin_addr.s_addr;
}

/* Convert sockaddr_in6 into sockaddr_in. */
void
in6_sin6_2_sin_in_sock(struct sockaddr *nam)
{
	struct sockaddr_in *sin_p;
	struct sockaddr_in6 sin6;

	/*
	 * Save original sockaddr_in6 addr and convert it
	 * to sockaddr_in.
	 */
	sin6 = *(struct sockaddr_in6 *)nam;
	sin_p = (struct sockaddr_in *)nam;
	in6_sin6_2_sin(sin_p, &sin6);
}

/* Convert sockaddr_in into sockaddr_in6 in v4 mapped addr format. */
int
in6_sin_2_v4mapsin6_in_sock(struct sockaddr **nam)
{
	struct sockaddr_in *sin_p;
	struct sockaddr_in6 *sin6_p;

	MALLOC(sin6_p, struct sockaddr_in6 *, sizeof *sin6_p, M_SONAME,
	       M_WAITOK);
	if (sin6_p == NULL)
		return ENOBUFS;
	sin_p = (struct sockaddr_in *)*nam;
	in6_sin_2_v4mapsin6(sin_p, sin6_p);
	FREE(*nam, M_SONAME);
	*nam = (struct sockaddr *)sin6_p;

	return 0;
}

/*
 * Posts in6_event_data message kernel events.
 *
 * To get the same size of kev_in6_data between ILP32 and LP64 data models
 * we are using a special version of the in6_addrlifetime structure that 
 * uses only 32 bits fields to be compatible with Leopard, and that 
 * are large enough to span 68 years.
 */
void
in6_post_msg(struct ifnet *ifp, u_int32_t event_code, struct in6_ifaddr *ifa)
{
	struct kev_msg        ev_msg;
	struct kev_in6_data   in6_event_data;

	ev_msg.vendor_code    = KEV_VENDOR_APPLE;
	ev_msg.kev_class      = KEV_NETWORK_CLASS;
	ev_msg.kev_subclass   = KEV_INET6_SUBCLASS;
	ev_msg.event_code     = event_code;

	in6_event_data.ia_addr         = ifa->ia_addr;
	in6_event_data.ia_net          = ifa->ia_net;
	in6_event_data.ia_dstaddr      = ifa->ia_dstaddr;
	in6_event_data.ia_prefixmask   = ifa->ia_prefixmask;
	in6_event_data.ia_plen	       = ifa->ia_plen;
	in6_event_data.ia6_flags       = (u_int32_t)ifa->ia6_flags;

	in6_event_data.ia_lifetime.ia6t_expire =
	    ifa->ia6_lifetime.ia6t_expire;
	in6_event_data.ia_lifetime.ia6t_preferred =
	    ifa->ia6_lifetime.ia6t_preferred;
	in6_event_data.ia_lifetime.ia6t_vltime =
	    ifa->ia6_lifetime.ia6t_vltime;
	in6_event_data.ia_lifetime.ia6t_pltime =
	    ifa->ia6_lifetime.ia6t_pltime;

	if (ifp != NULL) {
		strncpy(&in6_event_data.link_data.if_name[0],
		    ifp->if_name, IFNAMSIZ);
		in6_event_data.link_data.if_family = ifp->if_family;
		in6_event_data.link_data.if_unit  = (u_int32_t) ifp->if_unit;
	}

	ev_msg.dv[0].data_ptr    = &in6_event_data;
	ev_msg.dv[0].data_length = sizeof (in6_event_data);
	ev_msg.dv[1].data_length = 0;

	kev_post_msg(&ev_msg);
}

/*
 * Called as part of ip6_init
 */
void
in6_ifaddr_init(void)
{
	PE_parse_boot_argn("ifa_debug", &in6ifa_debug, sizeof (in6ifa_debug));

	in6ifa_size = (in6ifa_debug == 0) ? sizeof (struct in6_ifaddr) :
	    sizeof (struct in6_ifaddr_dbg);

	in6ifa_zone = zinit(in6ifa_size, IN6IFA_ZONE_MAX * in6ifa_size,
	    0, IN6IFA_ZONE_NAME);
	if (in6ifa_zone == NULL)
		panic("%s: failed allocating %s", __func__, IN6IFA_ZONE_NAME);

	zone_change(in6ifa_zone, Z_EXPAND, TRUE);
}

static struct in6_ifaddr *
in6_ifaddr_alloc(int how)
{
	struct in6_ifaddr *in6ifa;

	in6ifa = (how == M_WAITOK) ? zalloc(in6ifa_zone) :
	    zalloc_noblock(in6ifa_zone);
	if (in6ifa != NULL) {
		bzero(in6ifa, in6ifa_size);
		in6ifa->ia_ifa.ifa_free = in6_ifaddr_free;
		in6ifa->ia_ifa.ifa_debug |= IFD_ALLOC;
		if (in6ifa_debug != 0) {
			struct in6_ifaddr_dbg *in6ifa_dbg =
			    (struct in6_ifaddr_dbg *)in6ifa;
			in6ifa->ia_ifa.ifa_debug |= IFD_DEBUG;
			in6ifa->ia_ifa.ifa_trace = in6_ifaddr_trace;
			ctrace_record(&in6ifa_dbg->in6ifa_alloc);
		}
	}
	return (in6ifa);
}

static void
in6_ifaddr_free(struct ifaddr *ifa)
{
	if (ifa->ifa_refcnt != 0)
		panic("%s: ifa %p bad ref cnt", __func__, ifa);
	if (!(ifa->ifa_debug & IFD_ALLOC))
		panic("%s: ifa %p cannot be freed", __func__, ifa);

	if (ifa->ifa_debug & IFD_DEBUG) {
		struct in6_ifaddr_dbg *in6ifa_dbg =
		    (struct in6_ifaddr_dbg *)ifa;
		ctrace_record(&in6ifa_dbg->in6ifa_free);
		bcopy(&in6ifa_dbg->in6ifa, &in6ifa_dbg->in6ifa_old,
		    sizeof (struct in6_ifaddr));
	}
	bzero(ifa, sizeof (struct in6_ifaddr));
	zfree(in6ifa_zone, ifa);
}

static void
in6_ifaddr_trace(struct ifaddr *ifa, int refhold)
{
	struct in6_ifaddr_dbg *in6ifa_dbg = (struct in6_ifaddr_dbg *)ifa;
	ctrace_t *tr;
	u_int32_t idx;
	u_int16_t *cnt;

	if (!(ifa->ifa_debug & IFD_DEBUG))
		panic("%s: ifa %p has no debug structure", __func__, ifa);

	if (refhold) {
		cnt = &in6ifa_dbg->in6ifa_refhold_cnt;
		tr = in6ifa_dbg->in6ifa_refhold;
	} else {
		cnt = &in6ifa_dbg->in6ifa_refrele_cnt;
		tr = in6ifa_dbg->in6ifa_refrele;
	}

	idx = OSAddAtomic16(1, (volatile SInt16 *)cnt) % CTRACE_HIST_SIZE;
	ctrace_record(&tr[idx]);
}

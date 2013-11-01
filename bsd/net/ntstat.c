/*
 * Copyright (c) 2010-2013 Apple Inc. All rights reserved.
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

#include <sys/param.h>
#include <sys/types.h>
#include <sys/kpi_mbuf.h>
#include <sys/socket.h>
#include <sys/kern_control.h>
#include <sys/mcache.h>
#include <sys/socketvar.h>
#include <sys/sysctl.h>
#include <sys/queue.h>
#include <sys/priv.h>

#include <kern/clock.h>
#include <kern/debug.h>

#include <libkern/libkern.h>
#include <libkern/OSMalloc.h>
#include <libkern/OSAtomic.h>
#include <libkern/locks.h>

#include <net/if.h>
#include <net/if_var.h>
#include <net/if_types.h>
#include <net/route.h>
#include <net/ntstat.h>

#include <netinet/ip_var.h>
#include <netinet/in_pcb.h>
#include <netinet/in_var.h>
#include <netinet/tcp.h>
#include <netinet/tcp_var.h>
#include <netinet/tcp_fsm.h>
#include <netinet/udp.h>
#include <netinet/udp_var.h>
#include <netinet6/in6_pcb.h>
#include <netinet6/in6_var.h>

__private_extern__ int	nstat_collect = 1;
SYSCTL_INT(_net, OID_AUTO, statistics, CTLFLAG_RW | CTLFLAG_LOCKED,
    &nstat_collect, 0, "Collect detailed statistics");

static int nstat_privcheck = 0;
SYSCTL_INT(_net, OID_AUTO, statistics_privcheck, CTLFLAG_RW | CTLFLAG_LOCKED,
    &nstat_privcheck, 0, "Entitlement check");

enum
{
	NSTAT_FLAG_CLEANUP	= (1 << 0),
	NSTAT_FLAG_REQCOUNTS	= (1 << 1),
	NSTAT_FLAG_REQDESCS	= (1 << 2)
};

typedef struct nstat_control_state
{
	struct nstat_control_state	*ncs_next;
	u_int32_t			ncs_watching;
	decl_lck_mtx_data(, mtx);
	kern_ctl_ref			ncs_kctl;
	u_int32_t			ncs_unit;
	nstat_src_ref_t			ncs_next_srcref;
	struct nstat_src		*ncs_srcs;
	u_int32_t			ncs_flags;
} nstat_control_state;

typedef struct nstat_provider
{
	struct nstat_provider	*next;
	nstat_provider_id_t		nstat_provider_id;
	size_t					nstat_descriptor_length;
	errno_t					(*nstat_lookup)(const void *data, u_int32_t length, nstat_provider_cookie_t *out_cookie);
	int						(*nstat_gone)(nstat_provider_cookie_t cookie);
	errno_t					(*nstat_counts)(nstat_provider_cookie_t cookie, struct nstat_counts *out_counts, int *out_gone);
	errno_t					(*nstat_watcher_add)(nstat_control_state *state);
	void					(*nstat_watcher_remove)(nstat_control_state *state);
	errno_t					(*nstat_copy_descriptor)(nstat_provider_cookie_t cookie, void *data, u_int32_t len);
	void					(*nstat_release)(nstat_provider_cookie_t cookie, boolean_t locked);
} nstat_provider;


typedef struct nstat_src
{
	struct nstat_src		*next;
	nstat_src_ref_t			srcref;
	nstat_provider			*provider;
	nstat_provider_cookie_t		cookie;
	uint32_t			filter;
} nstat_src;

static errno_t		nstat_control_send_counts(nstat_control_state *,
    			    nstat_src *, unsigned long long, int *); 
static int		nstat_control_send_description(nstat_control_state *state, nstat_src *src, u_int64_t context);
static errno_t		nstat_control_send_removed(nstat_control_state *, nstat_src *);
static void		nstat_control_cleanup_source(nstat_control_state *state, nstat_src *src,
    				boolean_t);

static u_int32_t	nstat_udp_watchers = 0;
static u_int32_t	nstat_tcp_watchers = 0;

static void nstat_control_register(void);

static volatile OSMallocTag	nstat_malloc_tag = NULL;
static nstat_control_state	*nstat_controls = NULL;
static uint64_t				nstat_idle_time = 0;
static decl_lck_mtx_data(, nstat_mtx);

static void
nstat_copy_sa_out(
	const struct sockaddr	*src,
	struct sockaddr			*dst,
	int						maxlen)
{
	if (src->sa_len > maxlen) return;
	
	bcopy(src, dst, src->sa_len);
	if (src->sa_family == AF_INET6 &&
		src->sa_len >= sizeof(struct sockaddr_in6))
	{
		struct sockaddr_in6	*sin6 = (struct sockaddr_in6*)(void *)dst;
		if (IN6_IS_SCOPE_EMBED(&sin6->sin6_addr))
		{
			if (sin6->sin6_scope_id == 0)
				sin6->sin6_scope_id = ntohs(sin6->sin6_addr.__u6_addr.__u6_addr16[1]);
			sin6->sin6_addr.__u6_addr.__u6_addr16[1] = 0;
		}
	}
}

static void
nstat_ip_to_sockaddr(
	const struct in_addr	*ip,
	u_int16_t				port,
	struct sockaddr_in		*sin,
	u_int32_t				maxlen)
{
	if (maxlen < sizeof(struct sockaddr_in))
		return;
	
	sin->sin_family = AF_INET;
	sin->sin_len = sizeof(*sin);
	sin->sin_port = port;
	sin->sin_addr = *ip;
}

static void
nstat_ip6_to_sockaddr(
	const struct in6_addr	*ip6,
	u_int16_t				port,
	struct sockaddr_in6		*sin6,
	u_int32_t				maxlen)
{
	if (maxlen < sizeof(struct sockaddr_in6))
		return;
	
	sin6->sin6_family = AF_INET6;
	sin6->sin6_len = sizeof(*sin6);
	sin6->sin6_port = port;
	sin6->sin6_addr = *ip6;
	if (IN6_IS_SCOPE_EMBED(&sin6->sin6_addr))
	{
		sin6->sin6_scope_id = ntohs(sin6->sin6_addr.__u6_addr.__u6_addr16[1]);
		sin6->sin6_addr.__u6_addr.__u6_addr16[1] = 0;
	}
}

#pragma mark -- Network Statistic Providers --

static errno_t nstat_control_source_add(u_int64_t context, nstat_control_state *state, nstat_provider *provider, nstat_provider_cookie_t cookie);
struct nstat_provider	*nstat_providers = NULL;

static struct nstat_provider*
nstat_find_provider_by_id(
	nstat_provider_id_t	id)
{
	struct nstat_provider	*provider;
	
	for (provider = nstat_providers; provider != NULL; provider = provider->next)
	{
		if (provider->nstat_provider_id == id)
			break;
	}
	
	return provider;
}

static errno_t
nstat_lookup_entry(
	nstat_provider_id_t		id,
	const void				*data,
	u_int32_t				length,
	nstat_provider			**out_provider,
	nstat_provider_cookie_t	*out_cookie)
{
	*out_provider = nstat_find_provider_by_id(id);
	if (*out_provider == NULL)
	{
		return ENOENT;
	}
	
	return (*out_provider)->nstat_lookup(data, length, out_cookie);
}

static void nstat_init_route_provider(void);
static void nstat_init_tcp_provider(void);
static void nstat_init_udp_provider(void);
static void nstat_init_ifnet_provider(void);

__private_extern__ void
nstat_init(void)
{
	if (nstat_malloc_tag != NULL) return;
	
	OSMallocTag tag = OSMalloc_Tagalloc(NET_STAT_CONTROL_NAME, OSMT_DEFAULT);
	if (!OSCompareAndSwapPtr(NULL, tag, &nstat_malloc_tag))
	{
		OSMalloc_Tagfree(tag);
		tag = nstat_malloc_tag;
	}
	else
	{
		// we need to initialize other things, we do it here as this code path will only be hit once;
		nstat_init_route_provider();
		nstat_init_tcp_provider();
		nstat_init_udp_provider();
		nstat_init_ifnet_provider();
		nstat_control_register();
	}
}

#pragma mark -- Aligned Buffer Allocation --

struct align_header
{
	u_int32_t	offset;
	u_int32_t	length;
};

static void*
nstat_malloc_aligned(
	u_int32_t	length,
	u_int8_t	alignment,
	OSMallocTag	tag)
{
	struct align_header	*hdr = NULL;
	u_int32_t	size = length + sizeof(*hdr) + alignment - 1;
	
	u_int8_t	*buffer = OSMalloc(size, tag);
	if (buffer == NULL) return NULL;
	
	u_int8_t	*aligned = buffer + sizeof(*hdr);
	aligned = (u_int8_t*)P2ROUNDUP(aligned, alignment);
	
	hdr = (struct align_header*)(void *)(aligned - sizeof(*hdr));
	hdr->offset = aligned - buffer;
	hdr->length = size;
	
	return aligned;
}

static void
nstat_free_aligned(
	void		*buffer,
	OSMallocTag	tag)
{
	struct align_header *hdr = (struct align_header*)(void *)((u_int8_t*)buffer - sizeof(*hdr));
	OSFree(((char*)buffer) - hdr->offset, hdr->length, tag);
}

#pragma mark -- Route Provider --

static nstat_provider	nstat_route_provider;

static errno_t
nstat_route_lookup(
	const void				*data,
	u_int32_t 				length,
	nstat_provider_cookie_t	*out_cookie)
{
	// rt_lookup doesn't take const params but it doesn't modify the parameters for
	// the lookup. So...we use a union to eliminate the warning.
	union
	{
		struct sockaddr *sa;
		const struct sockaddr *const_sa;
	} dst, mask;
	
	const nstat_route_add_param	*param = (const nstat_route_add_param*)data;
	*out_cookie = NULL;
	
	if (length < sizeof(*param))
	{
		return EINVAL;
	}
	
	if (param->dst.v4.sin_family == 0 ||
		param->dst.v4.sin_family > AF_MAX ||
		(param->mask.v4.sin_family != 0 && param->mask.v4.sin_family != param->dst.v4.sin_family))
	{
		return EINVAL;
	}
	
	if (param->dst.v4.sin_len > sizeof(param->dst) ||
		(param->mask.v4.sin_family && param->mask.v4.sin_len > sizeof(param->mask.v4.sin_len)))
	{
		return EINVAL;
	}
	
	// TBD: Need to validate length of sockaddr for different families?
	dst.const_sa = (const struct sockaddr*)&param->dst;
	mask.const_sa = param->mask.v4.sin_family ? (const struct sockaddr*)&param->mask : NULL;
	
	struct radix_node_head	*rnh = rt_tables[dst.sa->sa_family];
	if (rnh == NULL) return EAFNOSUPPORT;
	
	lck_mtx_lock(rnh_lock);
	struct rtentry *rt = rt_lookup(TRUE, dst.sa, mask.sa, rnh, param->ifindex);
	lck_mtx_unlock(rnh_lock);
	
	if (rt) *out_cookie = (nstat_provider_cookie_t)rt;
	
	return rt ? 0 : ENOENT;
}

static int
nstat_route_gone(
	nstat_provider_cookie_t	cookie)
{
	struct rtentry		*rt = (struct rtentry*)cookie;
	return ((rt->rt_flags & RTF_UP) == 0) ? 1 : 0;
}

static errno_t
nstat_route_counts(
	nstat_provider_cookie_t	cookie,
	struct nstat_counts		*out_counts,
	int						*out_gone)
{
	struct rtentry		*rt = (struct rtentry*)cookie;
	struct nstat_counts	*rt_stats = rt->rt_stats;
	
	*out_gone = 0;
	
	if ((rt->rt_flags & RTF_UP) == 0) *out_gone = 1;
	
	if (rt_stats)
	{
		atomic_get_64(out_counts->nstat_rxpackets, &rt_stats->nstat_rxpackets);
		atomic_get_64(out_counts->nstat_rxbytes, &rt_stats->nstat_rxbytes);
		atomic_get_64(out_counts->nstat_txpackets, &rt_stats->nstat_txpackets);
		atomic_get_64(out_counts->nstat_txbytes, &rt_stats->nstat_txbytes);
		out_counts->nstat_rxduplicatebytes = rt_stats->nstat_rxduplicatebytes;
		out_counts->nstat_rxoutoforderbytes = rt_stats->nstat_rxoutoforderbytes;
		out_counts->nstat_txretransmit = rt_stats->nstat_txretransmit;
		out_counts->nstat_connectattempts = rt_stats->nstat_connectattempts;
		out_counts->nstat_connectsuccesses = rt_stats->nstat_connectsuccesses;
		out_counts->nstat_min_rtt = rt_stats->nstat_min_rtt;
		out_counts->nstat_avg_rtt = rt_stats->nstat_avg_rtt;
		out_counts->nstat_var_rtt = rt_stats->nstat_var_rtt;
		out_counts->nstat_cell_rxbytes = out_counts->nstat_cell_txbytes = 0;
	}
	else
		bzero(out_counts, sizeof(*out_counts));
	
	return 0;
}

static void
nstat_route_release(
	nstat_provider_cookie_t cookie,
	__unused int locked)
{
	rtfree((struct rtentry*)cookie);
}

static u_int32_t	nstat_route_watchers = 0;

static int
nstat_route_walktree_add(
	struct radix_node	*rn,
	void				*context)
{
	errno_t	result = 0;
	struct rtentry *rt = (struct rtentry *)rn;
	nstat_control_state	*state	= (nstat_control_state*)context;

	lck_mtx_assert(rnh_lock, LCK_MTX_ASSERT_OWNED);

	/* RTF_UP can't change while rnh_lock is held */
	if ((rt->rt_flags & RTF_UP) != 0)
	{
		/* Clear RTPRF_OURS if the route is still usable */
		RT_LOCK(rt);
		if (rt_validate(rt)) {
			RT_ADDREF_LOCKED(rt);
			RT_UNLOCK(rt);
		} else {
			RT_UNLOCK(rt);
			rt = NULL;
		}

		/* Otherwise if RTF_CONDEMNED, treat it as if it were down */
		if (rt == NULL)
			return (0);

		result = nstat_control_source_add(0, state, &nstat_route_provider, rt);
		if (result != 0)
			rtfree_locked(rt);
	}
	
	return result;
}

static errno_t
nstat_route_add_watcher(
	nstat_control_state	*state)
{
	int i;
	errno_t result = 0;
	OSIncrementAtomic(&nstat_route_watchers);
	
	lck_mtx_lock(rnh_lock);
	for (i = 1; i < AF_MAX; i++)
	{
		struct radix_node_head *rnh;
		rnh = rt_tables[i];
		if (!rnh) continue;
		
		result = rnh->rnh_walktree(rnh, nstat_route_walktree_add, state);
		if (result != 0)
		{
			break;
		}
	}
	lck_mtx_unlock(rnh_lock);
	
	return result;
}

__private_extern__ void
nstat_route_new_entry(
	struct rtentry	*rt)
{
	if (nstat_route_watchers == 0)
		return;
	
	lck_mtx_lock(&nstat_mtx);
	if ((rt->rt_flags & RTF_UP) != 0)
	{
		nstat_control_state	*state;
		for (state = nstat_controls; state; state = state->ncs_next)
		{
			if ((state->ncs_watching & (1 << NSTAT_PROVIDER_ROUTE)) != 0)
			{
				// this client is watching routes
				// acquire a reference for the route
				RT_ADDREF(rt);
				
				// add the source, if that fails, release the reference
				if (nstat_control_source_add(0, state, &nstat_route_provider, rt) != 0)
					RT_REMREF(rt);
			}
		}
	}
	lck_mtx_unlock(&nstat_mtx);
}

static void
nstat_route_remove_watcher(
	__unused nstat_control_state	*state)
{
	OSDecrementAtomic(&nstat_route_watchers);
}

static errno_t
nstat_route_copy_descriptor(
	nstat_provider_cookie_t	cookie,
	void					*data,
	u_int32_t				len)
{
	nstat_route_descriptor	*desc = (nstat_route_descriptor*)data;
	if (len < sizeof(*desc))
	{
		return EINVAL;
	}
	bzero(desc, sizeof(*desc));
	
	struct rtentry	*rt = (struct rtentry*)cookie;
	desc->id = (uintptr_t)rt;
	desc->parent_id = (uintptr_t)rt->rt_parent;
	desc->gateway_id = (uintptr_t)rt->rt_gwroute;

	
	// key/dest
	struct sockaddr	*sa;
	if ((sa = rt_key(rt)))
		nstat_copy_sa_out(sa, &desc->dst.sa, sizeof(desc->dst));
	
	// mask
	if ((sa = rt_mask(rt)) && sa->sa_len <= sizeof(desc->mask))
		memcpy(&desc->mask, sa, sa->sa_len);
	
	// gateway
	if ((sa = rt->rt_gateway))
		nstat_copy_sa_out(sa, &desc->gateway.sa, sizeof(desc->gateway));
	
	if (rt->rt_ifp)
		desc->ifindex = rt->rt_ifp->if_index;
	
	desc->flags = rt->rt_flags;
	
	return 0;
}

static void
nstat_init_route_provider(void)
{
	bzero(&nstat_route_provider, sizeof(nstat_route_provider));
	nstat_route_provider.nstat_descriptor_length = sizeof(nstat_route_descriptor);
	nstat_route_provider.nstat_provider_id = NSTAT_PROVIDER_ROUTE;
	nstat_route_provider.nstat_lookup = nstat_route_lookup;
	nstat_route_provider.nstat_gone = nstat_route_gone;
	nstat_route_provider.nstat_counts = nstat_route_counts;
	nstat_route_provider.nstat_release = nstat_route_release;
	nstat_route_provider.nstat_watcher_add = nstat_route_add_watcher;
	nstat_route_provider.nstat_watcher_remove = nstat_route_remove_watcher;
	nstat_route_provider.nstat_copy_descriptor = nstat_route_copy_descriptor;
	nstat_route_provider.next = nstat_providers;
	nstat_providers = &nstat_route_provider;
}

#pragma mark -- Route Collection --

static struct nstat_counts*
nstat_route_attach(
	struct rtentry	*rte)
{
	struct nstat_counts *result = rte->rt_stats;
	if (result) return result;
	
	if (nstat_malloc_tag == NULL) nstat_init();
	
	result = nstat_malloc_aligned(sizeof(*result), sizeof(u_int64_t), nstat_malloc_tag);
	if (!result) return result;
	
	bzero(result, sizeof(*result));
	
	if (!OSCompareAndSwapPtr(NULL, result, &rte->rt_stats))
	{
		nstat_free_aligned(result, nstat_malloc_tag);
		result = rte->rt_stats;
	}
	
	return result;
}

__private_extern__ void
nstat_route_detach(
	struct rtentry	*rte)
{
	if (rte->rt_stats)
	{
		nstat_free_aligned(rte->rt_stats, nstat_malloc_tag);
		rte->rt_stats = NULL;
	}
}

__private_extern__ void
nstat_route_connect_attempt(
	struct rtentry	*rte)
{
	while (rte)
	{
		struct nstat_counts*	stats = nstat_route_attach(rte);
		if (stats)
		{
			OSIncrementAtomic(&stats->nstat_connectattempts);
		}
		
		rte = rte->rt_parent;
	}
}

__private_extern__ void
nstat_route_connect_success(
	struct rtentry	*rte)
{
	// This route
	while (rte)
	{
		struct nstat_counts*	stats = nstat_route_attach(rte);
		if (stats)
		{
			OSIncrementAtomic(&stats->nstat_connectsuccesses);
		}
		
		rte = rte->rt_parent;
	}
}

__private_extern__ void
nstat_route_tx(
	struct rtentry	*rte,
	u_int32_t		packets,
	u_int32_t		bytes,
	u_int32_t		flags)
{
	while (rte)
	{
		struct nstat_counts*	stats = nstat_route_attach(rte);
		if (stats)
		{
			if ((flags & NSTAT_TX_FLAG_RETRANSMIT) != 0)
			{
				OSAddAtomic(bytes, &stats->nstat_txretransmit);
			}
			else
			{
				OSAddAtomic64((SInt64)packets, (SInt64*)&stats->nstat_txpackets);
				OSAddAtomic64((SInt64)bytes, (SInt64*)&stats->nstat_txbytes);
			}
		}
		
		rte = rte->rt_parent;
	}
}

__private_extern__ void
nstat_route_rx(
	struct rtentry	*rte,
	u_int32_t		packets,
	u_int32_t		bytes,
	u_int32_t		flags)
{
	while (rte)
	{
		struct nstat_counts*	stats = nstat_route_attach(rte);
		if (stats)
		{
			if (flags == 0)
			{
				OSAddAtomic64((SInt64)packets, (SInt64*)&stats->nstat_rxpackets);
				OSAddAtomic64((SInt64)bytes, (SInt64*)&stats->nstat_rxbytes);
			}
			else
			{
				if (flags & NSTAT_RX_FLAG_OUT_OF_ORDER)
					OSAddAtomic(bytes, &stats->nstat_rxoutoforderbytes);
				if (flags & NSTAT_RX_FLAG_DUPLICATE)
					OSAddAtomic(bytes, &stats->nstat_rxduplicatebytes);
			}
		}
		
		rte = rte->rt_parent;
	}
}

__private_extern__ void
nstat_route_rtt(
	struct rtentry	*rte,
	u_int32_t		rtt,
	u_int32_t		rtt_var)
{
	const int32_t	factor = 8;
	
	while (rte)
	{
		struct nstat_counts*	stats = nstat_route_attach(rte);
		if (stats)
		{
			int32_t	oldrtt;
			int32_t	newrtt;
			
			// average
			do
			{
				oldrtt = stats->nstat_avg_rtt;
				if (oldrtt == 0)
				{
					newrtt = rtt;
				}
				else
				{
					newrtt = oldrtt - (oldrtt - (int32_t)rtt) / factor;
				}
				if (oldrtt == newrtt) break;
			} while (!OSCompareAndSwap(oldrtt, newrtt, &stats->nstat_avg_rtt));
			
			// minimum
			do
			{
				oldrtt = stats->nstat_min_rtt;
				if (oldrtt != 0 && oldrtt < (int32_t)rtt)
				{
					break;
				}
			} while (!OSCompareAndSwap(oldrtt, rtt, &stats->nstat_min_rtt));
			
			// variance
			do
			{
				oldrtt = stats->nstat_var_rtt;
				if (oldrtt == 0)
				{
					newrtt = rtt_var;
				}
				else
				{
					newrtt = oldrtt - (oldrtt - (int32_t)rtt_var) / factor;
				}
				if (oldrtt == newrtt) break;
			} while (!OSCompareAndSwap(oldrtt, newrtt, &stats->nstat_var_rtt));
		}
		
		rte = rte->rt_parent;
	}
}


#pragma mark -- TCP Provider --

/*
 * Due to the way the kernel deallocates a process (the process structure
 * might be gone by the time we get the PCB detach notification),
 * we need to cache the process name. Without this, proc_name() would
 * return null and the process name would never be sent to userland.
 */
struct nstat_tcpudp_cookie {
	struct inpcb 	*inp;
	char		pname[MAXCOMLEN+1];
};

static struct nstat_tcpudp_cookie *
nstat_tcpudp_cookie_alloc(
    struct inpcb *inp,
    bool	  ref)
{
	struct nstat_tcpudp_cookie *cookie;

	cookie = OSMalloc(sizeof(*cookie), nstat_malloc_tag);
	if (cookie == NULL)
		return NULL;
	if (ref && in_pcb_checkstate(inp, WNT_ACQUIRE, 0) == WNT_STOPUSING)
	{
		OSFree(cookie, sizeof(*cookie), nstat_malloc_tag);
		return NULL;
	}
	bzero(cookie, sizeof(*cookie));
	cookie->inp = inp;
	proc_name(inp->inp_socket->last_pid, cookie->pname,
	    sizeof(cookie->pname));

	return cookie;
}

static void
nstat_tcpudp_cookie_release(
    struct nstat_tcpudp_cookie *cookie,
    int				inplock)
{
	in_pcb_checkstate(cookie->inp, WNT_RELEASE, inplock);
	OSFree(cookie, sizeof(*cookie), nstat_malloc_tag);
}

static nstat_provider	nstat_tcp_provider;

static errno_t
nstat_tcpudp_lookup(
	struct inpcbinfo	*inpinfo,
	const void		*data,
	u_int32_t 		length,
	nstat_provider_cookie_t	*out_cookie)
{
	struct inpcb *inp = NULL;

	// parameter validation
	const nstat_tcp_add_param	*param = (const nstat_tcp_add_param*)data;
	if (length < sizeof(*param))
	{
		return EINVAL;
	}
	
	// src and dst must match
	if (param->remote.v4.sin_family != 0 &&
		param->remote.v4.sin_family != param->local.v4.sin_family)
	{
		return EINVAL;
	}
	
	
	switch (param->local.v4.sin_family)
	{
		case AF_INET:
		{
			if (param->local.v4.sin_len != sizeof(param->local.v4) ||
		  		(param->remote.v4.sin_family != 0 &&
		  		 param->remote.v4.sin_len != sizeof(param->remote.v4)))
		  	{
				return EINVAL;
		  	}
		  	
			inp = in_pcblookup_hash(inpinfo, param->remote.v4.sin_addr, param->remote.v4.sin_port,
						param->local.v4.sin_addr, param->local.v4.sin_port, 1, NULL);
		}
		break;
		
#if INET6
		case AF_INET6:
		{
			union
			{
				const struct in6_addr 	*in6c;
				struct in6_addr			*in6;
			} local, remote;
			
			if (param->local.v6.sin6_len != sizeof(param->local.v6) ||
		  		(param->remote.v6.sin6_family != 0 &&
				 param->remote.v6.sin6_len != sizeof(param->remote.v6)))
			{
				return EINVAL;
			}
			
			local.in6c = &param->local.v6.sin6_addr;
			remote.in6c = &param->remote.v6.sin6_addr;
			
			inp = in6_pcblookup_hash(inpinfo, remote.in6, param->remote.v6.sin6_port,
						local.in6, param->local.v6.sin6_port, 1, NULL);
		}
		break;
#endif
		
		default:
			return EINVAL;
	}
	
	if (inp == NULL)
		return ENOENT;
	
	// At this point we have a ref to the inpcb
	*out_cookie = nstat_tcpudp_cookie_alloc(inp, false);
	if (*out_cookie == NULL)
		in_pcb_checkstate(inp, WNT_RELEASE, 0);

	return 0;
}

static errno_t
nstat_tcp_lookup(
	const void				*data,
	u_int32_t 				length,
	nstat_provider_cookie_t	*out_cookie)
{
	return nstat_tcpudp_lookup(&tcbinfo, data, length, out_cookie);
}

static int
nstat_tcp_gone(
	nstat_provider_cookie_t	cookie)
{
	struct nstat_tcpudp_cookie *tucookie =
	    (struct nstat_tcpudp_cookie *)cookie;
	struct inpcb *inp;
	struct tcpcb *tp;
	
	return (!(inp = tucookie->inp) ||
	        !(tp = intotcpcb(inp)) ||
		inp->inp_state == INPCB_STATE_DEAD ||
		tp->t_state == TCPS_TIME_WAIT) ? 1 : 0;
}

static errno_t
nstat_tcp_counts(
	nstat_provider_cookie_t	cookie,
	struct nstat_counts		*out_counts,
	int						*out_gone)
{
	struct nstat_tcpudp_cookie *tucookie =
	    (struct nstat_tcpudp_cookie *)cookie;
	struct inpcb *inp;

	bzero(out_counts, sizeof(*out_counts));
	
	*out_gone = 0;
	
	// if the pcb is in the dead state, we should stop using it
	if (nstat_tcp_gone(cookie))
	{
		*out_gone = 1;
		if (!(inp = tucookie->inp) || !intotcpcb(inp))
			return EINVAL;
	} 
	inp = tucookie->inp;
	struct tcpcb *tp = intotcpcb(inp);
	
	atomic_get_64(out_counts->nstat_rxpackets, &inp->inp_stat->rxpackets);
	atomic_get_64(out_counts->nstat_rxbytes, &inp->inp_stat->rxbytes);
	atomic_get_64(out_counts->nstat_txpackets, &inp->inp_stat->txpackets);
	atomic_get_64(out_counts->nstat_txbytes, &inp->inp_stat->txbytes);
	out_counts->nstat_rxduplicatebytes = tp->t_stat.rxduplicatebytes;
	out_counts->nstat_rxoutoforderbytes = tp->t_stat.rxoutoforderbytes;
	out_counts->nstat_txretransmit = tp->t_stat.txretransmitbytes;
	out_counts->nstat_connectattempts = tp->t_state >= TCPS_SYN_SENT ? 1 : 0;
	out_counts->nstat_connectsuccesses = tp->t_state >= TCPS_ESTABLISHED ? 1 : 0;
	out_counts->nstat_avg_rtt = tp->t_srtt;
	out_counts->nstat_min_rtt = tp->t_rttbest;
	out_counts->nstat_var_rtt = tp->t_rttvar;
	if (out_counts->nstat_avg_rtt < out_counts->nstat_min_rtt)
		out_counts->nstat_min_rtt = out_counts->nstat_avg_rtt;
	atomic_get_64(out_counts->nstat_cell_rxbytes, &inp->inp_cstat->rxbytes);
	atomic_get_64(out_counts->nstat_cell_txbytes, &inp->inp_cstat->txbytes);
	atomic_get_64(out_counts->nstat_wifi_rxbytes, &inp->inp_wstat->rxbytes);
	atomic_get_64(out_counts->nstat_wifi_txbytes, &inp->inp_wstat->txbytes);
	
	return 0;
}

static void
nstat_tcp_release(
	nstat_provider_cookie_t	cookie,
	int locked)
{
	struct nstat_tcpudp_cookie *tucookie =
	    (struct nstat_tcpudp_cookie *)cookie;

	nstat_tcpudp_cookie_release(tucookie, locked);
}

static errno_t
nstat_tcp_add_watcher(
	nstat_control_state	*state)
{
	OSIncrementAtomic(&nstat_tcp_watchers);
	
	lck_rw_lock_shared(tcbinfo.ipi_lock);
	
	// Add all current tcp inpcbs. Ignore those in timewait
	struct inpcb *inp;
	struct nstat_tcpudp_cookie *cookie;
	for (inp = LIST_FIRST(tcbinfo.ipi_listhead); inp; inp = LIST_NEXT(inp, inp_list))
	{
		cookie = nstat_tcpudp_cookie_alloc(inp, true);
		if (cookie == NULL)
			continue;
		if (nstat_control_source_add(0, state, &nstat_tcp_provider,
		    cookie) != 0)
		{
			nstat_tcpudp_cookie_release(cookie, false);
			break;
		}
	}
	
	lck_rw_done(tcbinfo.ipi_lock);
	
	return 0;
}

static void
nstat_tcp_remove_watcher(
	__unused nstat_control_state	*state)
{
	OSDecrementAtomic(&nstat_tcp_watchers);
}

__private_extern__ void
nstat_tcp_new_pcb(
	struct inpcb	*inp)
{
	struct nstat_tcpudp_cookie *cookie;

	if (nstat_tcp_watchers == 0)
		return;
	
	lck_mtx_lock(&nstat_mtx);
	nstat_control_state	*state;
	for (state = nstat_controls; state; state = state->ncs_next)
	{
		if ((state->ncs_watching & (1 << NSTAT_PROVIDER_TCP)) != 0)
		{
			// this client is watching tcp
			// acquire a reference for it
			cookie = nstat_tcpudp_cookie_alloc(inp, true);
			if (cookie == NULL)
				continue;
			// add the source, if that fails, release the reference
			if (nstat_control_source_add(0, state,
			    &nstat_tcp_provider, cookie) != 0)
			{
				nstat_tcpudp_cookie_release(cookie, false);
				break;
			}
		}
	}
	lck_mtx_unlock(&nstat_mtx);
}

__private_extern__ void
nstat_pcb_detach(struct inpcb *inp)
{
	nstat_control_state *state;
	nstat_src *src, *prevsrc;
	nstat_src *dead_list = NULL;
	struct nstat_tcpudp_cookie *tucookie;

	if (inp == NULL || (nstat_tcp_watchers == 0 && nstat_udp_watchers == 0))
		return;

	lck_mtx_lock(&nstat_mtx);
	for (state = nstat_controls; state; state = state->ncs_next) {
		lck_mtx_lock(&state->mtx);
		for (prevsrc = NULL, src = state->ncs_srcs; src;
		    prevsrc = src, src = src->next) 
		{
			tucookie = (struct nstat_tcpudp_cookie *)src->cookie;
			if (tucookie->inp == inp)
				break;
		}

		if (src) {
			// send one last counts notification
			nstat_control_send_counts(state, src, 0, NULL);

			// send a last description
			nstat_control_send_description(state, src, 0);

			// send the source removed notification
			nstat_control_send_removed(state, src);

			if (prevsrc)
				prevsrc->next = src->next;
			else
				state->ncs_srcs = src->next;

			src->next = dead_list;
			dead_list = src;
		}
		lck_mtx_unlock(&state->mtx);
	}
	lck_mtx_unlock(&nstat_mtx);

	while (dead_list) {
		src = dead_list;
		dead_list = src->next;

		nstat_control_cleanup_source(NULL, src, TRUE);
	}
}

static errno_t
nstat_tcp_copy_descriptor(
	nstat_provider_cookie_t	cookie,
	void			*data,
	u_int32_t		len)
{
	if (len < sizeof(nstat_tcp_descriptor))
	{
		return EINVAL;
	}

	if (nstat_tcp_gone(cookie))
		return EINVAL;
	
	nstat_tcp_descriptor	*desc = (nstat_tcp_descriptor*)data;
	struct nstat_tcpudp_cookie *tucookie =
	    (struct nstat_tcpudp_cookie *)cookie;
	struct inpcb		*inp = tucookie->inp;
	struct tcpcb		*tp = intotcpcb(inp);
	bzero(desc, sizeof(*desc));
	
	if (inp->inp_vflag & INP_IPV6)
	{
		nstat_ip6_to_sockaddr(&inp->in6p_laddr, inp->inp_lport,
			&desc->local.v6, sizeof(desc->local));
		nstat_ip6_to_sockaddr(&inp->in6p_faddr, inp->inp_fport,
			&desc->remote.v6, sizeof(desc->remote));
	}
	else if (inp->inp_vflag & INP_IPV4)
	{
		nstat_ip_to_sockaddr(&inp->inp_laddr, inp->inp_lport,
			&desc->local.v4, sizeof(desc->local));
		nstat_ip_to_sockaddr(&inp->inp_faddr, inp->inp_fport,
			&desc->remote.v4, sizeof(desc->remote));
	}
	
	desc->state = intotcpcb(inp)->t_state;
	desc->ifindex = (inp->inp_last_outifp == NULL) ? 0 :
	    inp->inp_last_outifp->if_index;
	
	// danger - not locked, values could be bogus
	desc->txunacked = tp->snd_max - tp->snd_una;
	desc->txwindow = tp->snd_wnd;
	desc->txcwindow = tp->snd_cwnd;
	
	struct socket *so = inp->inp_socket;
	if (so)
	{
		// TBD - take the socket lock around these to make sure
		// they're in sync?
		desc->upid = so->last_upid;
		desc->pid = so->last_pid;
		desc->traffic_class = so->so_traffic_class;
		proc_name(desc->pid, desc->pname, sizeof(desc->pname));
		if (desc->pname == NULL || desc->pname[0] == 0)
		{
			strlcpy(desc->pname, tucookie->pname,
			    sizeof(desc->pname));
		}
		else
		{
			desc->pname[sizeof(desc->pname) - 1] = 0;
			strlcpy(tucookie->pname, desc->pname,
			    sizeof(tucookie->pname));
		}
		memcpy(desc->uuid, so->last_uuid, sizeof(so->last_uuid));
		if (so->so_flags & SOF_DELEGATED) {
			desc->eupid = so->e_upid;
			desc->epid = so->e_pid;
			memcpy(desc->euuid, so->e_uuid, sizeof(so->e_uuid));
		} else {
			desc->eupid = desc->upid;
			desc->epid = desc->pid;
			memcpy(desc->euuid, desc->uuid, sizeof(desc->uuid));
		}
		desc->sndbufsize = so->so_snd.sb_hiwat;
		desc->sndbufused = so->so_snd.sb_cc;
		desc->rcvbufsize = so->so_rcv.sb_hiwat;
		desc->rcvbufused = so->so_rcv.sb_cc;
	}
	
	return 0;
}

static void
nstat_init_tcp_provider(void)
{
	bzero(&nstat_tcp_provider, sizeof(nstat_tcp_provider));
	nstat_tcp_provider.nstat_descriptor_length = sizeof(nstat_tcp_descriptor);
	nstat_tcp_provider.nstat_provider_id = NSTAT_PROVIDER_TCP;
	nstat_tcp_provider.nstat_lookup = nstat_tcp_lookup;
	nstat_tcp_provider.nstat_gone = nstat_tcp_gone;
	nstat_tcp_provider.nstat_counts = nstat_tcp_counts;
	nstat_tcp_provider.nstat_release = nstat_tcp_release;
	nstat_tcp_provider.nstat_watcher_add = nstat_tcp_add_watcher;
	nstat_tcp_provider.nstat_watcher_remove = nstat_tcp_remove_watcher;
	nstat_tcp_provider.nstat_copy_descriptor = nstat_tcp_copy_descriptor;
	nstat_tcp_provider.next = nstat_providers;
	nstat_providers = &nstat_tcp_provider;
}

#pragma mark -- UDP Provider --

static nstat_provider	nstat_udp_provider;

static errno_t
nstat_udp_lookup(
	const void				*data,
	u_int32_t 				length,
	nstat_provider_cookie_t	*out_cookie)
{
	return nstat_tcpudp_lookup(&udbinfo, data, length, out_cookie);
}

static int
nstat_udp_gone(
	nstat_provider_cookie_t	cookie)
{
	struct nstat_tcpudp_cookie *tucookie =
	    (struct nstat_tcpudp_cookie *)cookie;
	struct inpcb *inp;

	return (!(inp = tucookie->inp) ||
		inp->inp_state == INPCB_STATE_DEAD) ? 1 : 0;
}

static errno_t
nstat_udp_counts(
	nstat_provider_cookie_t	cookie,
	struct nstat_counts	*out_counts,
	int			*out_gone)
{
	struct nstat_tcpudp_cookie *tucookie =
	    (struct nstat_tcpudp_cookie *)cookie;
	
	*out_gone = 0;
	
	// if the pcb is in the dead state, we should stop using it
	if (nstat_udp_gone(cookie))
	{
		*out_gone = 1;
		if (!tucookie->inp)
			return EINVAL;
	}
	struct inpcb *inp = tucookie->inp;
	
	atomic_get_64(out_counts->nstat_rxpackets, &inp->inp_stat->rxpackets);
	atomic_get_64(out_counts->nstat_rxbytes, &inp->inp_stat->rxbytes);
	atomic_get_64(out_counts->nstat_txpackets, &inp->inp_stat->txpackets);
	atomic_get_64(out_counts->nstat_txbytes, &inp->inp_stat->txbytes);
	atomic_get_64(out_counts->nstat_cell_rxbytes, &inp->inp_cstat->rxbytes);
	atomic_get_64(out_counts->nstat_cell_txbytes, &inp->inp_cstat->txbytes);
	atomic_get_64(out_counts->nstat_wifi_rxbytes, &inp->inp_wstat->rxbytes);
	atomic_get_64(out_counts->nstat_wifi_txbytes, &inp->inp_wstat->txbytes);
	
	return 0;
}

static void
nstat_udp_release(
	nstat_provider_cookie_t	cookie,
	int locked)
{
	struct nstat_tcpudp_cookie *tucookie =
	    (struct nstat_tcpudp_cookie *)cookie;

	nstat_tcpudp_cookie_release(tucookie, locked);
}

static errno_t
nstat_udp_add_watcher(
	nstat_control_state	*state)
{
	struct inpcb *inp;
	struct nstat_tcpudp_cookie *cookie;

	OSIncrementAtomic(&nstat_udp_watchers);
	
	lck_rw_lock_shared(udbinfo.ipi_lock);
	// Add all current UDP inpcbs.
	for (inp = LIST_FIRST(udbinfo.ipi_listhead); inp; inp = LIST_NEXT(inp, inp_list))
	{
		cookie = nstat_tcpudp_cookie_alloc(inp, true);
		if (cookie == NULL)
			continue;
		if (nstat_control_source_add(0, state, &nstat_udp_provider,
		    cookie) != 0)
		{
			nstat_tcpudp_cookie_release(cookie, false);
			break;
		}
	}
	
	lck_rw_done(udbinfo.ipi_lock);
	
	return 0;
}

static void
nstat_udp_remove_watcher(
	__unused nstat_control_state	*state)
{
	OSDecrementAtomic(&nstat_udp_watchers);
}

__private_extern__ void
nstat_udp_new_pcb(
	struct inpcb	*inp)
{
	struct nstat_tcpudp_cookie *cookie;

	if (nstat_udp_watchers == 0)
		return;
	
	lck_mtx_lock(&nstat_mtx);
	nstat_control_state	*state;
	for (state = nstat_controls; state; state = state->ncs_next)
	{
		if ((state->ncs_watching & (1 << NSTAT_PROVIDER_UDP)) != 0)
		{
			// this client is watching tcp
			// acquire a reference for it
			cookie = nstat_tcpudp_cookie_alloc(inp, true);
			if (cookie == NULL)
				continue;
			// add the source, if that fails, release the reference
			if (nstat_control_source_add(0, state, 
			    &nstat_udp_provider, cookie) != 0)
			{
				nstat_tcpudp_cookie_release(cookie, false);
				break;
			}
		}
	}
	lck_mtx_unlock(&nstat_mtx);
}

static errno_t
nstat_udp_copy_descriptor(
	nstat_provider_cookie_t	cookie,
	void					*data,
	u_int32_t				len)
{
	if (len < sizeof(nstat_udp_descriptor))
	{
		return EINVAL;
	}
	
	if (nstat_udp_gone(cookie))
		return EINVAL;

	struct nstat_tcpudp_cookie	*tucookie =
	    (struct nstat_tcpudp_cookie *)cookie;
	nstat_udp_descriptor		*desc = (nstat_udp_descriptor*)data;
	struct inpcb 			*inp = tucookie->inp;

	bzero(desc, sizeof(*desc));
	
	if (inp->inp_vflag & INP_IPV6)
	{
		nstat_ip6_to_sockaddr(&inp->in6p_laddr, inp->inp_lport,
			&desc->local.v6, sizeof(desc->local));
		nstat_ip6_to_sockaddr(&inp->in6p_faddr, inp->inp_fport,
			&desc->remote.v6, sizeof(desc->remote));
	}
	else if (inp->inp_vflag & INP_IPV4)
	{
		nstat_ip_to_sockaddr(&inp->inp_laddr, inp->inp_lport,
			&desc->local.v4, sizeof(desc->local));
		nstat_ip_to_sockaddr(&inp->inp_faddr, inp->inp_fport,
			&desc->remote.v4, sizeof(desc->remote));
	}
	
	desc->ifindex = (inp->inp_last_outifp == NULL) ? 0 :
	    inp->inp_last_outifp->if_index;
		
	struct socket *so = inp->inp_socket;
	if (so)
	{
		// TBD - take the socket lock around these to make sure
		// they're in sync?
		desc->upid = so->last_upid;
		desc->pid = so->last_pid;
		proc_name(desc->pid, desc->pname, sizeof(desc->pname));
		if (desc->pname == NULL || desc->pname[0] == 0)
		{
			strlcpy(desc->pname, tucookie->pname,
			    sizeof(desc->pname));
		}
		else
		{
			desc->pname[sizeof(desc->pname) - 1] = 0;
			strlcpy(tucookie->pname, desc->pname,
			    sizeof(tucookie->pname));
		}
		memcpy(desc->uuid, so->last_uuid, sizeof(so->last_uuid));
		if (so->so_flags & SOF_DELEGATED) {
			desc->eupid = so->e_upid;
			desc->epid = so->e_pid;
			memcpy(desc->euuid, so->e_uuid, sizeof(so->e_uuid));
		} else {
			desc->eupid = desc->upid;
			desc->epid = desc->pid;
			memcpy(desc->euuid, desc->uuid, sizeof(desc->uuid));
		}
		desc->rcvbufsize = so->so_rcv.sb_hiwat;
		desc->rcvbufused = so->so_rcv.sb_cc;
		desc->traffic_class = so->so_traffic_class;
	}
	
	return 0;
}

static void
nstat_init_udp_provider(void)
{
	bzero(&nstat_udp_provider, sizeof(nstat_udp_provider));
	nstat_udp_provider.nstat_provider_id = NSTAT_PROVIDER_UDP;
	nstat_udp_provider.nstat_descriptor_length = sizeof(nstat_udp_descriptor);
	nstat_udp_provider.nstat_lookup = nstat_udp_lookup;
	nstat_udp_provider.nstat_gone = nstat_udp_gone;
	nstat_udp_provider.nstat_counts = nstat_udp_counts;
	nstat_udp_provider.nstat_watcher_add = nstat_udp_add_watcher;
	nstat_udp_provider.nstat_watcher_remove = nstat_udp_remove_watcher;
	nstat_udp_provider.nstat_copy_descriptor = nstat_udp_copy_descriptor;
	nstat_udp_provider.nstat_release = nstat_udp_release;
	nstat_udp_provider.next = nstat_providers;
	nstat_providers = &nstat_udp_provider;
}

#pragma mark -- ifnet Provider --

static nstat_provider	nstat_ifnet_provider;

/*
 * We store a pointer to the ifnet and the original threshold
 * requested by the client.
 */
struct nstat_ifnet_cookie
{
	struct ifnet 	*ifp;
	uint64_t	threshold;
};

static errno_t
nstat_ifnet_lookup(
	const void		*data,
	u_int32_t 		length,
	nstat_provider_cookie_t	*out_cookie)
{
	const nstat_ifnet_add_param *param = (nstat_ifnet_add_param *)data;
	struct ifnet *ifp;
	boolean_t changed = FALSE;
	nstat_control_state *state;
	nstat_src *src;
	struct nstat_ifnet_cookie *cookie;

	if (length < sizeof(*param) || param->threshold < 1024*1024)
		return EINVAL;
	if (nstat_privcheck != 0) {
		errno_t result = priv_check_cred(kauth_cred_get(), 
		    PRIV_NET_PRIVILEGED_NETWORK_STATISTICS, 0);
		if (result != 0)
			return result;
	}
	cookie = OSMalloc(sizeof(*cookie), nstat_malloc_tag);
	if (cookie == NULL)
		return ENOMEM;
	bzero(cookie, sizeof(*cookie));

	ifnet_head_lock_shared();
	TAILQ_FOREACH(ifp, &ifnet_head, if_link)
	{
		ifnet_lock_exclusive(ifp);
		if (ifp->if_index == param->ifindex)
		{
			cookie->ifp = ifp;
			cookie->threshold = param->threshold;
			*out_cookie = cookie;
			if (!ifp->if_data_threshold ||
			    ifp->if_data_threshold > param->threshold)
			{
				changed = TRUE;
				ifp->if_data_threshold = param->threshold;
			}
			ifnet_lock_done(ifp);
			ifnet_reference(ifp);
			break;
		}
		ifnet_lock_done(ifp);
	}
	ifnet_head_done();

	/*
	 * When we change the threshold to something smaller, we notify
	 * all of our clients with a description message.
	 * We won't send a message to the client we are currently serving
	 * because it has no `ifnet source' yet.
	 */
	if (changed)
	{
		lck_mtx_lock(&nstat_mtx);
		for (state = nstat_controls; state; state = state->ncs_next)
		{
			lck_mtx_lock(&state->mtx);
			for (src = state->ncs_srcs; src; src = src->next)
			{
				if (src->provider != &nstat_ifnet_provider)
					continue;
				nstat_control_send_description(state, src, 0);
			}
			lck_mtx_unlock(&state->mtx);
		}
		lck_mtx_unlock(&nstat_mtx);
	}
	if (cookie->ifp == NULL)
		OSFree(cookie, sizeof(*cookie), nstat_malloc_tag);

	return ifp ? 0 : EINVAL;
}

static int
nstat_ifnet_gone(
	nstat_provider_cookie_t	cookie)
{
	struct ifnet *ifp;
	struct nstat_ifnet_cookie *ifcookie =
	    (struct nstat_ifnet_cookie *)cookie;

	ifnet_head_lock_shared();
	TAILQ_FOREACH(ifp, &ifnet_head, if_link)
	{
		if (ifp == ifcookie->ifp)
			break;
	}
	ifnet_head_done();

	return ifp ? 0 : 1;
}

static errno_t
nstat_ifnet_counts(
	nstat_provider_cookie_t	cookie,
	struct nstat_counts	*out_counts,
	int			*out_gone)
{
	struct nstat_ifnet_cookie *ifcookie =
	    (struct nstat_ifnet_cookie *)cookie;
	struct ifnet *ifp = ifcookie->ifp;

	*out_gone = 0;
	
	// if the ifnet is gone, we should stop using it
	if (nstat_ifnet_gone(cookie))
	{
		*out_gone = 1;
		return EINVAL;
	}

	bzero(out_counts, sizeof(*out_counts));
	out_counts->nstat_rxpackets = ifp->if_ipackets;
	out_counts->nstat_rxbytes = ifp->if_ibytes;
	out_counts->nstat_txpackets = ifp->if_opackets;
	out_counts->nstat_txbytes = ifp->if_obytes;
	out_counts->nstat_cell_rxbytes = out_counts->nstat_cell_txbytes = 0;

	return 0;
}

static void
nstat_ifnet_release(
	nstat_provider_cookie_t	cookie,
	__unused int 		locked)
{
	struct nstat_ifnet_cookie *ifcookie;
	struct ifnet *ifp;
	nstat_control_state *state;
	nstat_src *src;
	uint64_t minthreshold = UINT64_MAX;

	/*
	 * Find all the clients that requested a threshold
	 * for this ifnet and re-calculate if_data_threshold.
	 */
	lck_mtx_lock(&nstat_mtx);
	for (state = nstat_controls; state; state = state->ncs_next)
	{
		lck_mtx_lock(&state->mtx);
		for (src = state->ncs_srcs; src; src = src->next)
		{
			/* Skip the provider we are about to detach. */
			if (src->provider != &nstat_ifnet_provider ||
			    src->cookie == cookie)
				continue;
	    		ifcookie = (struct nstat_ifnet_cookie *)src->cookie;
			if (ifcookie->threshold < minthreshold)
				minthreshold = ifcookie->threshold; 
		}
		lck_mtx_unlock(&state->mtx);
	}
	lck_mtx_unlock(&nstat_mtx);
	/*
	 * Reset if_data_threshold or disable it.
	 */
	ifcookie = (struct nstat_ifnet_cookie *)cookie;
	ifp = ifcookie->ifp;
	if (ifnet_is_attached(ifp, 1)) {
		ifnet_lock_exclusive(ifp);
		if (minthreshold == UINT64_MAX)
			ifp->if_data_threshold = 0;
		else
			ifp->if_data_threshold = minthreshold;
		ifnet_lock_done(ifp);
		ifnet_decr_iorefcnt(ifp);                               
	}
	ifnet_release(ifp);
	OSFree(ifcookie, sizeof(*ifcookie), nstat_malloc_tag);
}

static errno_t
nstat_ifnet_copy_descriptor(
	nstat_provider_cookie_t	cookie,
	void			*data,
	u_int32_t		len)
{	
	nstat_ifnet_descriptor *desc = (nstat_ifnet_descriptor *)data;
	struct nstat_ifnet_cookie *ifcookie =
	    (struct nstat_ifnet_cookie *)cookie;
	struct ifnet *ifp = ifcookie->ifp;

	if (len < sizeof(nstat_ifnet_descriptor))
		return EINVAL;
	
	if (nstat_ifnet_gone(cookie))
		return EINVAL;

	bzero(desc, sizeof(*desc));
	ifnet_lock_shared(ifp);
	strlcpy(desc->name, ifp->if_xname, sizeof(desc->name));
	desc->ifindex = ifp->if_index;
	desc->threshold = ifp->if_data_threshold;
	desc->type = ifp->if_type;
	if (ifp->if_desc.ifd_len < sizeof(desc->description))
		memcpy(desc->description, ifp->if_desc.ifd_desc,
	    	    sizeof(desc->description));
	ifnet_lock_done(ifp);
	
	return 0;
}

static void
nstat_init_ifnet_provider(void)
{
	bzero(&nstat_ifnet_provider, sizeof(nstat_ifnet_provider));
	nstat_ifnet_provider.nstat_provider_id = NSTAT_PROVIDER_IFNET;
	nstat_ifnet_provider.nstat_descriptor_length = sizeof(nstat_ifnet_descriptor);
	nstat_ifnet_provider.nstat_lookup = nstat_ifnet_lookup;
	nstat_ifnet_provider.nstat_gone = nstat_ifnet_gone;
	nstat_ifnet_provider.nstat_counts = nstat_ifnet_counts;
	nstat_ifnet_provider.nstat_watcher_add = NULL;
	nstat_ifnet_provider.nstat_watcher_remove = NULL;
	nstat_ifnet_provider.nstat_copy_descriptor = nstat_ifnet_copy_descriptor;
	nstat_ifnet_provider.nstat_release = nstat_ifnet_release;
	nstat_ifnet_provider.next = nstat_providers;
	nstat_providers = &nstat_ifnet_provider;
}

__private_extern__ void
nstat_ifnet_threshold_reached(unsigned int ifindex)
{
	nstat_control_state *state;
	nstat_src *src;
	struct ifnet *ifp;
	struct nstat_ifnet_cookie *ifcookie;

	lck_mtx_lock(&nstat_mtx);
	for (state = nstat_controls; state; state = state->ncs_next)
	{
		lck_mtx_lock(&state->mtx);
		for (src = state->ncs_srcs; src; src = src->next)
		{
			if (src->provider != &nstat_ifnet_provider)
				continue;
			ifcookie = (struct nstat_ifnet_cookie *)src->cookie;
			ifp = ifcookie->ifp;
			if (ifp->if_index != ifindex)
				continue;
			nstat_control_send_counts(state, src, 0, NULL);
		}
		lck_mtx_unlock(&state->mtx);
	}
	lck_mtx_unlock(&nstat_mtx);
}

#pragma mark -- Kernel Control Socket --

static kern_ctl_ref	nstat_ctlref = NULL;
static lck_grp_t	*nstat_lck_grp = NULL;

static errno_t	nstat_control_connect(kern_ctl_ref kctl, struct sockaddr_ctl *sac, void **uinfo);
static errno_t	nstat_control_disconnect(kern_ctl_ref kctl, u_int32_t unit, void *uinfo);
static errno_t	nstat_control_send(kern_ctl_ref kctl, u_int32_t unit, void *uinfo, mbuf_t m, int flags);



static void*
nstat_idle_check(
	__unused thread_call_param_t p0,
	__unused thread_call_param_t p1)
{
	lck_mtx_lock(&nstat_mtx);
	
	nstat_idle_time = 0;
	
	nstat_control_state *control;
	nstat_src	*dead = NULL;
	nstat_src	*dead_list = NULL;
	for (control = nstat_controls; control; control = control->ncs_next)
	{
		lck_mtx_lock(&control->mtx);
		nstat_src	**srcpp = &control->ncs_srcs;
		
		if (!(control->ncs_flags & NSTAT_FLAG_REQCOUNTS))
		{
			while(*srcpp != NULL)
			{
				if ((*srcpp)->provider->nstat_gone((*srcpp)->cookie))
				{
					// Pull it off the list
					dead = *srcpp;
					*srcpp = (*srcpp)->next;
					
					// send one last counts notification
					nstat_control_send_counts(control, dead,
					    0, NULL);
						
					// send a last description
					nstat_control_send_description(control, dead, 0);
					
					// send the source removed notification
					nstat_control_send_removed(control, dead);
					
					// Put this on the list to release later
					dead->next = dead_list;
					dead_list = dead;
				}
				else
				{
					srcpp = &(*srcpp)->next;
				}
			}
		}
		control->ncs_flags &= ~NSTAT_FLAG_REQCOUNTS;
		lck_mtx_unlock(&control->mtx);
	}
	
	if (nstat_controls)
	{
		clock_interval_to_deadline(60, NSEC_PER_SEC, &nstat_idle_time);
		thread_call_func_delayed((thread_call_func_t)nstat_idle_check, NULL, nstat_idle_time);
	}
	
	lck_mtx_unlock(&nstat_mtx);
	
	// Release the sources now that we aren't holding lots of locks
	while (dead_list)
	{
		dead = dead_list;
		dead_list = dead->next;
		
		nstat_control_cleanup_source(NULL, dead, FALSE);
	}
	
	return NULL;
}

static void
nstat_control_register(void)
{
	// Create our lock group first
	lck_grp_attr_t	*grp_attr = lck_grp_attr_alloc_init();
	lck_grp_attr_setdefault(grp_attr);
	nstat_lck_grp = lck_grp_alloc_init("network statistics kctl", grp_attr);
	lck_grp_attr_free(grp_attr);
	
	lck_mtx_init(&nstat_mtx, nstat_lck_grp, NULL);
	
	// Register the control
	struct kern_ctl_reg	nstat_control;
	bzero(&nstat_control, sizeof(nstat_control));	
	strlcpy(nstat_control.ctl_name, NET_STAT_CONTROL_NAME, sizeof(nstat_control.ctl_name));
	nstat_control.ctl_connect = nstat_control_connect;
	nstat_control.ctl_disconnect = nstat_control_disconnect;
	nstat_control.ctl_send = nstat_control_send;
	
	ctl_register(&nstat_control, &nstat_ctlref);
}

static void
nstat_control_cleanup_source(
	nstat_control_state	*state,
	struct nstat_src	*src,
	boolean_t 		locked)
{
	if (state)
		nstat_control_send_removed(state, src);
	
	// Cleanup the source if we found it.
	src->provider->nstat_release(src->cookie, locked);
	OSFree(src, sizeof(*src), nstat_malloc_tag);
}

static errno_t
nstat_control_connect(
	kern_ctl_ref		kctl,
	struct sockaddr_ctl	*sac,
	void				**uinfo)
{
	nstat_control_state	*state = OSMalloc(sizeof(*state), nstat_malloc_tag);
	if (state == NULL) return ENOMEM;
	
	bzero(state, sizeof(*state));
	lck_mtx_init(&state->mtx, nstat_lck_grp, NULL);
	state->ncs_kctl = kctl;
	state->ncs_unit = sac->sc_unit;
	state->ncs_flags = NSTAT_FLAG_REQCOUNTS;
	*uinfo = state;
	
	lck_mtx_lock(&nstat_mtx);
	state->ncs_next = nstat_controls;
	nstat_controls = state;
	
	if (nstat_idle_time == 0)
	{
		clock_interval_to_deadline(60, NSEC_PER_SEC, &nstat_idle_time);
		thread_call_func_delayed((thread_call_func_t)nstat_idle_check, NULL, nstat_idle_time);
	}
	
	lck_mtx_unlock(&nstat_mtx);
	
	return 0;
}

static errno_t
nstat_control_disconnect(
	__unused kern_ctl_ref	kctl,
	__unused u_int32_t		unit,
	void			*uinfo)
{
	u_int32_t	watching;
	nstat_control_state	*state = (nstat_control_state*)uinfo;
	
	// pull it out of the global list of states
	lck_mtx_lock(&nstat_mtx);
	nstat_control_state	**statepp;
	for (statepp = &nstat_controls; *statepp; statepp = &(*statepp)->ncs_next)
	{
		if (*statepp == state)
		{
			*statepp = state->ncs_next;
			break;
		}
	}
	lck_mtx_unlock(&nstat_mtx);
	
	lck_mtx_lock(&state->mtx);
	// Stop watching for sources
	nstat_provider	*provider;
	watching = state->ncs_watching;
	state->ncs_watching = 0;
	for (provider = nstat_providers; provider && watching;  provider = provider->next)
	{
		if ((watching & (1 << provider->nstat_provider_id)) != 0)
		{
			watching &= ~(1 << provider->nstat_provider_id);
			provider->nstat_watcher_remove(state);
		}
	}
	
	// set cleanup flags
	state->ncs_flags |= NSTAT_FLAG_CLEANUP;
	
	// Copy out the list of sources
	nstat_src	*srcs = state->ncs_srcs;
	state->ncs_srcs = NULL;
	lck_mtx_unlock(&state->mtx);
	
	while (srcs)
	{
		nstat_src	*src;
		
		// pull it out of the list
		src = srcs;
		srcs = src->next;
		
		// clean it up
		nstat_control_cleanup_source(NULL, src, FALSE);
	}
	lck_mtx_destroy(&state->mtx, nstat_lck_grp);
	OSFree(state, sizeof(*state), nstat_malloc_tag);
	
	return 0;
}

static nstat_src_ref_t
nstat_control_next_src_ref(
	nstat_control_state	*state)
{
	int i = 0;
	nstat_src_ref_t	toReturn = NSTAT_SRC_REF_INVALID;
	
	for (i = 0; i < 1000 && toReturn == NSTAT_SRC_REF_INVALID; i++)
	{
		if (state->ncs_next_srcref == NSTAT_SRC_REF_INVALID ||
			state->ncs_next_srcref == NSTAT_SRC_REF_ALL)
		{
			state->ncs_next_srcref = 1;
		}
		
		nstat_src	*src;
		for (src = state->ncs_srcs; src; src = src->next)
		{
			if (src->srcref == state->ncs_next_srcref)
				break;
		}
		
		if (src == NULL) toReturn = state->ncs_next_srcref;
		state->ncs_next_srcref++;
	}
	
	return toReturn;
}

static errno_t
nstat_control_send_counts(
	nstat_control_state	*state,
	nstat_src		*src,
	unsigned long long	context,
	int *gone)
{ 	
	nstat_msg_src_counts counts;
	int localgone = 0;
	errno_t result = 0;

	counts.hdr.type = NSTAT_MSG_TYPE_SRC_COUNTS;
	counts.hdr.context = context;
	counts.srcref = src->srcref;
	bzero(&counts.counts, sizeof(counts.counts));
	if (src->provider->nstat_counts(src->cookie, &counts.counts,
	    &localgone) == 0) {
		if ((src->filter & NSTAT_FILTER_NOZEROBYTES) &&
		    counts.counts.nstat_rxbytes == 0 && 
		    counts.counts.nstat_txbytes == 0)
			result = EAGAIN;
		else
			result = ctl_enqueuedata(state->ncs_kctl,
			    state->ncs_unit, &counts, sizeof(counts),
			    CTL_DATA_EOR);
	}
	if (gone)
		*gone = localgone;
	return result;
}

static int
nstat_control_send_description(
	nstat_control_state	*state,
	nstat_src			*src,
	u_int64_t			context)
{
	// Provider doesn't support getting the descriptor? Done.
	if (src->provider->nstat_descriptor_length == 0 ||
		src->provider->nstat_copy_descriptor == NULL)
	{
		return EOPNOTSUPP;
	}
	
	// Allocate storage for the descriptor message
	mbuf_t			msg;
	unsigned int	one = 1;
	u_int32_t		size = offsetof(nstat_msg_src_description, data) + src->provider->nstat_descriptor_length;
	if (mbuf_allocpacket(MBUF_WAITOK, size, &one, &msg) != 0)
	{
		return ENOMEM;
	}
	
	nstat_msg_src_description	*desc = (nstat_msg_src_description*)mbuf_data(msg);
	mbuf_setlen(msg, size);
	mbuf_pkthdr_setlen(msg, mbuf_len(msg));
	
	// Query the provider for the provider specific bits
	errno_t	result = src->provider->nstat_copy_descriptor(src->cookie, desc->data, src->provider->nstat_descriptor_length);
	
	if (result != 0)
	{
		mbuf_freem(msg);
		return result;
	}
	
	desc->hdr.context = context;
	desc->hdr.type = NSTAT_MSG_TYPE_SRC_DESC;
	desc->srcref = src->srcref;
	desc->provider = src->provider->nstat_provider_id;
	
	result = ctl_enqueuembuf(state->ncs_kctl, state->ncs_unit, msg, CTL_DATA_EOR);
	if (result != 0)
	{
		mbuf_freem(msg);
	}
	
	return result;
}

static errno_t
nstat_control_send_removed(
	nstat_control_state	*state,
	nstat_src		*src)
{
	nstat_msg_src_removed removed;
	errno_t result;

	removed.hdr.type = NSTAT_MSG_TYPE_SRC_REMOVED;
	removed.hdr.context = 0;
	removed.srcref = src->srcref;
	result = ctl_enqueuedata(state->ncs_kctl, state->ncs_unit, &removed,
	    sizeof(removed), CTL_DATA_EOR);

	return result;
}

static errno_t
nstat_control_handle_add_request(
	nstat_control_state	*state,
	mbuf_t				m)
{
	errno_t	result;

	// Verify the header fits in the first mbuf
	if (mbuf_len(m) < offsetof(nstat_msg_add_src_req, param))
	{
		return EINVAL;
	}
	
	// Calculate the length of the parameter field
	int32_t	paramlength = mbuf_pkthdr_len(m) - offsetof(nstat_msg_add_src_req, param);
	if (paramlength < 0 || paramlength > 2 * 1024)
	{
		return EINVAL;
	}
	
	nstat_provider			*provider;
	nstat_provider_cookie_t	cookie;
	nstat_msg_add_src_req	*req = mbuf_data(m);
	if (mbuf_pkthdr_len(m) > mbuf_len(m))
	{
		// parameter is too large, we need to make a contiguous copy
		void	*data = OSMalloc(paramlength, nstat_malloc_tag);
		
		if (!data) return ENOMEM;
		result = mbuf_copydata(m, offsetof(nstat_msg_add_src_req, param), paramlength, data);
		if (result == 0)
			result = nstat_lookup_entry(req->provider, data, paramlength, &provider, &cookie);
		OSFree(data, paramlength, nstat_malloc_tag);
	}
	else
	{
		result = nstat_lookup_entry(req->provider, (void*)&req->param, paramlength, &provider, &cookie);
	}
	
	if (result != 0)
	{
		return result;
	}
	
	result = nstat_control_source_add(req->hdr.context, state, provider, cookie);
	if (result != 0)
		provider->nstat_release(cookie, 0);
	
	return result;
}

static errno_t
nstat_control_handle_add_all(
	nstat_control_state	*state,
	mbuf_t				m)
{
	errno_t	result = 0;
	
	// Verify the header fits in the first mbuf
	if (mbuf_len(m) < sizeof(nstat_msg_add_all_srcs))
	{
		return EINVAL;
	}
	
	nstat_msg_add_all_srcs	*req = mbuf_data(m);
	nstat_provider			*provider = nstat_find_provider_by_id(req->provider);
	
	if (!provider) return ENOENT;
	if (provider->nstat_watcher_add == NULL) return ENOTSUP;
	
	if (nstat_privcheck != 0) {
		result = priv_check_cred(kauth_cred_get(), 
		    PRIV_NET_PRIVILEGED_NETWORK_STATISTICS, 0);
		if (result != 0)
			return result;
	}

	// Make sure we don't add the provider twice
	lck_mtx_lock(&state->mtx);
	if ((state->ncs_watching & (1 << provider->nstat_provider_id)) != 0)
		result = EALREADY;
	state->ncs_watching |= (1 << provider->nstat_provider_id);
	lck_mtx_unlock(&state->mtx);
	if (result != 0) return result;

	result = provider->nstat_watcher_add(state);
	if (result != 0)
	{
		lck_mtx_lock(&state->mtx);
		state->ncs_watching &= ~(1 << provider->nstat_provider_id);
		lck_mtx_unlock(&state->mtx);
	}
	
	if (result == 0)
	{
		// Notify the client
		nstat_msg_hdr	success;
		success.context = req->hdr.context;
		success.type = NSTAT_MSG_TYPE_SUCCESS;
		success.pad = 0;
		ctl_enqueuedata(state->ncs_kctl, state->ncs_unit, &success, sizeof(success), CTL_DATA_EOR);
	}
	
	return result;
}

static errno_t
nstat_control_source_add(
	u_int64_t				context,
	nstat_control_state		*state,
	nstat_provider			*provider,
	nstat_provider_cookie_t	cookie)
{
	// Fill out source added message
	mbuf_t					msg = NULL;
	unsigned int			one = 1;
	
	if (mbuf_allocpacket(MBUF_WAITOK, sizeof(nstat_msg_src_added), &one, &msg) != 0)
		return ENOMEM;
	
	mbuf_setlen(msg, sizeof(nstat_msg_src_added));
	mbuf_pkthdr_setlen(msg, mbuf_len(msg));
	nstat_msg_src_added	*add = mbuf_data(msg);
	bzero(add, sizeof(*add));
	add->hdr.type = NSTAT_MSG_TYPE_SRC_ADDED;
	add->hdr.context = context;
	add->provider = provider->nstat_provider_id;
	
	// Allocate storage for the source
	nstat_src	*src = OSMalloc(sizeof(*src), nstat_malloc_tag);
	if (src == NULL)
	{
		mbuf_freem(msg);
		return ENOMEM;
	}
	
	// Fill in the source, including picking an unused source ref
	lck_mtx_lock(&state->mtx);
	
	add->srcref = src->srcref = nstat_control_next_src_ref(state);
	if (state->ncs_flags & NSTAT_FLAG_CLEANUP || src->srcref == NSTAT_SRC_REF_INVALID)
	{
		lck_mtx_unlock(&state->mtx);
		OSFree(src, sizeof(*src), nstat_malloc_tag);
		mbuf_freem(msg);
		return EINVAL;
	}
	src->provider = provider;
	src->cookie = cookie;
	src->filter = 0;
	
	// send the source added message
	errno_t result = ctl_enqueuembuf(state->ncs_kctl, state->ncs_unit, msg, CTL_DATA_EOR);
	if (result != 0)
	{
		lck_mtx_unlock(&state->mtx);
		OSFree(src, sizeof(*src), nstat_malloc_tag);
		mbuf_freem(msg);
		return result;
	}
	
	// Put the 	source in the list
	src->next = state->ncs_srcs;
	state->ncs_srcs = src;
	
	// send the description message
	// not useful as the source is often not complete
//	nstat_control_send_description(state, src, 0);
	
	lck_mtx_unlock(&state->mtx);
	
	return 0;
}

static errno_t
nstat_control_handle_remove_request(
	nstat_control_state	*state,
	mbuf_t				m)
{
	nstat_src_ref_t			srcref = NSTAT_SRC_REF_INVALID;
	
	if (mbuf_copydata(m, offsetof(nstat_msg_rem_src_req, srcref), sizeof(srcref), &srcref) != 0)
	{
		return EINVAL;
	}
	
	lck_mtx_lock(&state->mtx);
	
	// Remove this source as we look for it
	nstat_src	**nextp;
	nstat_src	*src = NULL;
	for (nextp = &state->ncs_srcs; *nextp; nextp = &(*nextp)->next)
	{
		if ((*nextp)->srcref == srcref)
		{
			src = *nextp;
			*nextp = src->next;
			break;
		}
	}
	
	lck_mtx_unlock(&state->mtx);
	
	if (src) nstat_control_cleanup_source(state, src, FALSE);
	
	return src ? 0 : ENOENT;
}

static errno_t
nstat_control_handle_query_request(
	nstat_control_state	*state,
	mbuf_t				m)
{
	// TBD: handle this from another thread so we can enqueue a lot of data
	// As written, if a client requests query all, this function will be 
	// called from their send of the request message. We will attempt to write
	// responses and succeed until the buffer fills up. Since the clients thread
	// is blocked on send, it won't be reading unless the client has two threads
	// using this socket, one for read and one for write. Two threads probably
	// won't work with this code anyhow since we don't have proper locking in
	// place yet.
	nstat_src				*dead_srcs = NULL;
	errno_t					result = ENOENT;
	nstat_msg_query_src_req	req;
	if (mbuf_copydata(m, 0, sizeof(req), &req) != 0)
	{
		return EINVAL;
	}
	
	lck_mtx_lock(&state->mtx);
	if (req.srcref == NSTAT_SRC_REF_ALL)
		state->ncs_flags |= NSTAT_FLAG_REQCOUNTS;
	nstat_src	**srcpp = &state->ncs_srcs;
	while (*srcpp != NULL)
	{
		int	gone;
		gone = 0;
		
		// XXX ignore IFACE types?
		if (req.srcref == NSTAT_SRC_REF_ALL ||
			(*srcpp)->srcref == req.srcref)
		{
			result = nstat_control_send_counts(state, *srcpp,
			    req.hdr.context, &gone);
			
			// If the counts message failed to enqueue then we should clear our flag so
			// that a client doesn't miss anything on idle cleanup.
			if (result != 0)
				state->ncs_flags &= ~NSTAT_FLAG_REQCOUNTS;
			
			if (gone)
			{
				// send one last descriptor message so client may see last state

				nstat_control_send_description(state, *srcpp,
				    0);
				
				// pull src out of the list
				nstat_src	*src = *srcpp;
				*srcpp = src->next;
				
				src->next = dead_srcs;
				dead_srcs = src;
			}
			
			if (req.srcref != NSTAT_SRC_REF_ALL)
				break;
		}
		
		if (!gone)
			srcpp = &(*srcpp)->next;
	}
	lck_mtx_unlock(&state->mtx);
	
	while (dead_srcs)
	{
		nstat_src	*src;
		
		src = dead_srcs;
		dead_srcs = src->next;
		
		// release src and send notification
		nstat_control_cleanup_source(state, src, FALSE);
	}
	
	if (req.srcref == NSTAT_SRC_REF_ALL)
	{
		nstat_msg_hdr	success;
		success.context = req.hdr.context;
		success.type = NSTAT_MSG_TYPE_SUCCESS;
		success.pad = 0;
		ctl_enqueuedata(state->ncs_kctl, state->ncs_unit, &success, sizeof(success), CTL_DATA_EOR);
		result = 0;
	}
	
	return result;
}

static errno_t
nstat_control_handle_get_src_description(
	nstat_control_state	*state,
	mbuf_t			m)
{
	nstat_msg_get_src_description	req;
	errno_t result;
	nstat_src *src;

	if (mbuf_copydata(m, 0, sizeof(req), &req) != 0)
	{
		return EINVAL;
	}
	
	lck_mtx_lock(&state->mtx);
	if (req.srcref == NSTAT_SRC_REF_ALL)
		state->ncs_flags |= NSTAT_FLAG_REQDESCS;
	for (src = state->ncs_srcs; src; src = src->next)
		if (req.srcref == NSTAT_SRC_REF_ALL ||
		    src->srcref == req.srcref)
		{
			result = nstat_control_send_description(state, src,
			    req.hdr.context);
			if (result != 0)
				state->ncs_flags &= ~NSTAT_FLAG_REQDESCS;
			if (req.srcref != NSTAT_SRC_REF_ALL)
				break;
		}
	lck_mtx_unlock(&state->mtx);
	if (req.srcref != NSTAT_SRC_REF_ALL && src == NULL)
		result = ENOENT;
	else if (req.srcref == NSTAT_SRC_REF_ALL)
	{
		nstat_msg_hdr	success;
		success.context = req.hdr.context;
		success.type = NSTAT_MSG_TYPE_SUCCESS;
		success.pad = 0;
		ctl_enqueuedata(state->ncs_kctl, state->ncs_unit, &success, sizeof(success), CTL_DATA_EOR);
		result = 0;
	}
	
	return result;
}

static errno_t
nstat_control_handle_set_filter(
    nstat_control_state		*state,
    mbuf_t			m)
{
	nstat_msg_set_filter req;
	nstat_src *src;

	if (mbuf_copydata(m, 0, sizeof(req), &req) != 0)
		return EINVAL;
	if (req.srcref == NSTAT_SRC_REF_ALL ||
	    req.srcref == NSTAT_SRC_REF_INVALID)
		return EINVAL;

	lck_mtx_lock(&state->mtx);
	for (src = state->ncs_srcs; src; src = src->next)
		if (req.srcref == src->srcref)
		{
			src->filter = req.filter;
			break;
		}
	lck_mtx_unlock(&state->mtx);
	if (src == NULL)
		return ENOENT;

	return 0;

}

static errno_t
nstat_control_send(
	kern_ctl_ref	kctl,
	u_int32_t		unit,
	void	*uinfo,
	mbuf_t			m,
	__unused int	flags)
{
	nstat_control_state	*state = (nstat_control_state*)uinfo;
	struct nstat_msg_hdr	*hdr;
	struct nstat_msg_hdr	storage;
	errno_t					result = 0;
	
	if (mbuf_pkthdr_len(m) < sizeof(hdr))
	{
		// Is this the right thing to do?
		mbuf_freem(m);
		return EINVAL;
	}
	
	if (mbuf_len(m) >= sizeof(*hdr))
	{
		hdr = mbuf_data(m);
	}
	else
	{
		mbuf_copydata(m, 0, sizeof(storage), &storage);
		hdr = &storage;
	}
	
	switch (hdr->type)
	{
		case NSTAT_MSG_TYPE_ADD_SRC:
			result = nstat_control_handle_add_request(state, m);
			break;
		
		case NSTAT_MSG_TYPE_ADD_ALL_SRCS:
			result = nstat_control_handle_add_all(state, m);
			break;
		
		case NSTAT_MSG_TYPE_REM_SRC:
			result = nstat_control_handle_remove_request(state, m);
			break;
		
		case NSTAT_MSG_TYPE_QUERY_SRC:
			result = nstat_control_handle_query_request(state, m);
			break;
		
		case NSTAT_MSG_TYPE_GET_SRC_DESC:
			result = nstat_control_handle_get_src_description(state, m);
			break;

		case NSTAT_MSG_TYPE_SET_FILTER:
			result = nstat_control_handle_set_filter(state, m);
			break;

		default:
			result = EINVAL;
			break;
	}
	
	if (result != 0)
	{
		struct nstat_msg_error	err;
		
		err.hdr.type = NSTAT_MSG_TYPE_ERROR;
		err.hdr.context = hdr->context;
		err.error = result;
		
		result = ctl_enqueuedata(kctl, unit, &err, sizeof(err), CTL_DATA_EOR);
	}
	
	mbuf_freem(m);
	
	return result;
}

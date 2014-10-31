/*
 * Copyright (c) 2008-2014 Apple Inc. All rights reserved.
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



/* ----------------------------------------------------------------------------------
Application of kernel control for interface creation

Theory of operation:
utun (user tunnel) acts as glue between kernel control sockets and network interfaces. 
This kernel control will register an interface for every client that connects. 
---------------------------------------------------------------------------------- */

#include <sys/systm.h>
#include <sys/kern_control.h>
#include <net/kpi_protocol.h>
#include <net/kpi_interface.h>
#include <sys/socket.h>
#include <net/if.h>
#include <net/if_types.h>
#include <net/bpf.h>
#include <net/if_utun.h>
#include <libkern/OSMalloc.h>
#include <libkern/OSAtomic.h>
#include <sys/mbuf.h> 
#include <sys/sockio.h>
#include <netinet/in.h>
#include <netinet6/in6_var.h>
#include <netinet6/in6_var.h>
#include <sys/kauth.h>


/* Kernel Control functions */
static errno_t	utun_ctl_connect(kern_ctl_ref kctlref, struct sockaddr_ctl *sac,
								 void **unitinfo);
static errno_t	utun_ctl_disconnect(kern_ctl_ref kctlref, u_int32_t unit,
									void *unitinfo);
static errno_t	utun_ctl_send(kern_ctl_ref kctlref, u_int32_t unit,
							   void *unitinfo, mbuf_t m, int flags);
static errno_t	utun_ctl_getopt(kern_ctl_ref kctlref, u_int32_t unit, void *unitinfo,
								 int opt, void *data, size_t *len);
static errno_t	utun_ctl_setopt(kern_ctl_ref kctlref, u_int32_t unit, void *unitinfo,
								 int opt, void *data, size_t len);
static void		utun_ctl_rcvd(kern_ctl_ref kctlref, u_int32_t unit, void *unitinfo,
								int flags);

/* Network Interface functions */
static void     utun_start(ifnet_t interface);
static errno_t	utun_output(ifnet_t interface, mbuf_t data);
static errno_t	utun_demux(ifnet_t interface, mbuf_t data, char *frame_header,
						   protocol_family_t *protocol);
static errno_t	utun_framer(ifnet_t interface, mbuf_t *packet,
    const struct sockaddr *dest, const char *desk_linkaddr,
    const char *frame_type, u_int32_t *prepend_len, u_int32_t *postpend_len);
static errno_t	utun_add_proto(ifnet_t interface, protocol_family_t protocol,
							   const struct ifnet_demux_desc *demux_array,
							   u_int32_t demux_count);
static errno_t	utun_del_proto(ifnet_t interface, protocol_family_t protocol);
static errno_t	utun_ioctl(ifnet_t interface, u_long cmd, void *data);
static void		utun_detached(ifnet_t interface);

/* Protocol handlers */
static errno_t	utun_attach_proto(ifnet_t interface, protocol_family_t proto);
static errno_t	utun_proto_input(ifnet_t interface, protocol_family_t protocol,
								 mbuf_t m, char *frame_header);
static errno_t utun_proto_pre_output(ifnet_t interface, protocol_family_t protocol, 
					 mbuf_t *packet, const struct sockaddr *dest, void *route,
					 char *frame_type, char *link_layer_dest);
__private_extern__ errno_t utun_pkt_input (struct utun_pcb *pcb, mbuf_t m);

static kern_ctl_ref	utun_kctlref;
static u_int32_t	utun_family;
static OSMallocTag	utun_malloc_tag;
static SInt32		utun_ifcount = 0;

/* Prepend length */
void*
utun_alloc(size_t size)
{
	size_t	*mem = OSMalloc(size + sizeof(size_t), utun_malloc_tag);
	
	if (mem) {
		*mem = size + sizeof(size_t);
		mem++;
	}
	
	return (void*)mem;
}

void
utun_free(void *ptr)
{
	size_t	*size = ptr;
	size--;
	OSFree(size, *size, utun_malloc_tag);
}

errno_t
utun_register_control(void)
{
	struct kern_ctl_reg	kern_ctl;
	errno_t				result = 0;
	
	/* Create a tag to allocate memory */
	utun_malloc_tag = OSMalloc_Tagalloc(UTUN_CONTROL_NAME, OSMT_DEFAULT);
	
	/* Find a unique value for our interface family */
	result = mbuf_tag_id_find(UTUN_CONTROL_NAME, &utun_family);
	if (result != 0) {
		printf("utun_register_control - mbuf_tag_id_find_internal failed: %d\n", result);
		return result;
	}
	
	bzero(&kern_ctl, sizeof(kern_ctl));
	strlcpy(kern_ctl.ctl_name, UTUN_CONTROL_NAME, sizeof(kern_ctl.ctl_name));
	kern_ctl.ctl_name[sizeof(kern_ctl.ctl_name) - 1] = 0;
	kern_ctl.ctl_flags = CTL_FLAG_PRIVILEGED | CTL_FLAG_REG_EXTENDED; /* Require root */
	kern_ctl.ctl_sendsize = 512 * 1024;
	kern_ctl.ctl_recvsize = 512 * 1024;
	kern_ctl.ctl_connect = utun_ctl_connect;
	kern_ctl.ctl_disconnect = utun_ctl_disconnect;
	kern_ctl.ctl_send = utun_ctl_send;
	kern_ctl.ctl_setopt = utun_ctl_setopt;
	kern_ctl.ctl_getopt = utun_ctl_getopt;
	kern_ctl.ctl_rcvd = utun_ctl_rcvd;

	utun_ctl_init_crypto();

	result = ctl_register(&kern_ctl, &utun_kctlref);
	if (result != 0) {
		printf("utun_register_control - ctl_register failed: %d\n", result);
		return result;
	}
	
	/* Register the protocol plumbers */
	if ((result = proto_register_plumber(PF_INET, utun_family,
										 utun_attach_proto, NULL)) != 0) {
		printf("utun_register_control - proto_register_plumber(PF_INET, %d) failed: %d\n",
			   utun_family, result);
		ctl_deregister(utun_kctlref);
		return result;
	}
	
	/* Register the protocol plumbers */
	if ((result = proto_register_plumber(PF_INET6, utun_family,
										 utun_attach_proto, NULL)) != 0) {
		proto_unregister_plumber(PF_INET, utun_family);
		ctl_deregister(utun_kctlref);
		printf("utun_register_control - proto_register_plumber(PF_INET6, %d) failed: %d\n",
			   utun_family, result);
		return result;
	}
	
	return 0;
}

/* Kernel control functions */

static errno_t
utun_ctl_connect(
	kern_ctl_ref		kctlref,
	struct sockaddr_ctl	*sac, 
	void				**unitinfo)
{
	struct ifnet_init_eparams	utun_init;
	struct utun_pcb				*pcb;
	errno_t						result;
	struct ifnet_stats_param 	stats;
	
	/* kernel control allocates, interface frees */
	pcb = utun_alloc(sizeof(*pcb));
	if (pcb == NULL)
		return ENOMEM;
	
	/* Setup the protocol control block */
	bzero(pcb, sizeof(*pcb));
	*unitinfo = pcb;
	pcb->utun_ctlref = kctlref;
	pcb->utun_unit = sac->sc_unit;
	pcb->utun_pending_packets = 0;
	pcb->utun_max_pending_packets = 1;
	
	printf("utun_ctl_connect: creating interface utun%d\n", pcb->utun_unit - 1);

	/* Create the interface */
	bzero(&utun_init, sizeof(utun_init));
	utun_init.ver = IFNET_INIT_CURRENT_VERSION;
	utun_init.len = sizeof (utun_init);
	utun_init.name = "utun";
	utun_init.start = utun_start;
	utun_init.unit = pcb->utun_unit - 1;
	utun_init.family = utun_family;
	utun_init.type = IFT_OTHER;
	utun_init.demux = utun_demux;
	utun_init.framer_extended = utun_framer;
	utun_init.add_proto = utun_add_proto;
	utun_init.del_proto = utun_del_proto;
	utun_init.softc = pcb;
	utun_init.ioctl = utun_ioctl;
	utun_init.detach = utun_detached;
	
	result = ifnet_allocate_extended(&utun_init, &pcb->utun_ifp);
	if (result != 0) {
		printf("utun_ctl_connect - ifnet_allocate failed: %d\n", result);
		utun_free(pcb);
		return result;
	}
	OSIncrementAtomic(&utun_ifcount);
	
	/* Set flags and additional information. */
	ifnet_set_mtu(pcb->utun_ifp, 1500);
	ifnet_set_flags(pcb->utun_ifp, IFF_UP | IFF_MULTICAST | IFF_POINTOPOINT, 0xffff);

	/* The interface must generate its own IPv6 LinkLocal address,
	 * if possible following the recommendation of RFC2472 to the 64bit interface ID
	 */
	ifnet_set_eflags(pcb->utun_ifp, IFEF_NOAUTOIPV6LL, IFEF_NOAUTOIPV6LL);
	
	/* Reset the stats in case as the interface may have been recycled */
	bzero(&stats, sizeof(struct ifnet_stats_param));
	ifnet_set_stat(pcb->utun_ifp, &stats);

	/* Attach the interface */
	result = ifnet_attach(pcb->utun_ifp, NULL);
	if (result != 0) {
		printf("utun_ctl_connect - ifnet_allocate failed: %d\n", result);
		ifnet_release(pcb->utun_ifp);
		utun_free(pcb);
	}
	
	/* Attach to bpf */
	if (result == 0)
		bpfattach(pcb->utun_ifp, DLT_NULL, 4);
	
	/* The interfaces resoures allocated, mark it as running */
	if (result == 0)
		ifnet_set_flags(pcb->utun_ifp, IFF_RUNNING, IFF_RUNNING);
	
	return result;
}

static errno_t
utun_detach_ip(
	ifnet_t				interface,
	protocol_family_t	protocol,
	socket_t			pf_socket)
{
	errno_t result = EPROTONOSUPPORT;
	
	/* Attempt a detach */
	if (protocol == PF_INET) {
		struct ifreq	ifr;
		
		bzero(&ifr, sizeof(ifr));
		snprintf(ifr.ifr_name, sizeof(ifr.ifr_name), "%s%d",
				 ifnet_name(interface), ifnet_unit(interface));
		
		result = sock_ioctl(pf_socket, SIOCPROTODETACH, &ifr);
	}
	else if (protocol == PF_INET6) {
		struct in6_ifreq	ifr6;
		
		bzero(&ifr6, sizeof(ifr6));
		snprintf(ifr6.ifr_name, sizeof(ifr6.ifr_name), "%s%d",
				 ifnet_name(interface), ifnet_unit(interface));
		
		result = sock_ioctl(pf_socket, SIOCPROTODETACH_IN6, &ifr6);
	}
	
	return result;
}

static void
utun_remove_address(
	ifnet_t				interface,
	protocol_family_t	protocol,
	ifaddr_t			address,
	socket_t			pf_socket)
{
	errno_t result = 0;
	
	/* Attempt a detach */
	if (protocol == PF_INET) {
		struct ifreq	ifr;
		
		bzero(&ifr, sizeof(ifr));
		snprintf(ifr.ifr_name, sizeof(ifr.ifr_name), "%s%d",
				 ifnet_name(interface), ifnet_unit(interface));
		result = ifaddr_address(address, &ifr.ifr_addr, sizeof(ifr.ifr_addr));
		if (result != 0) {
			printf("utun_remove_address - ifaddr_address failed: %d", result);
		}
		else {
			result = sock_ioctl(pf_socket, SIOCDIFADDR, &ifr);
			if (result != 0) {
				printf("utun_remove_address - SIOCDIFADDR failed: %d", result);
			}
		}
	}
	else if (protocol == PF_INET6) {
		struct in6_ifreq	ifr6;
		
		bzero(&ifr6, sizeof(ifr6));
		snprintf(ifr6.ifr_name, sizeof(ifr6.ifr_name), "%s%d",
				 ifnet_name(interface), ifnet_unit(interface));
		result = ifaddr_address(address, (struct sockaddr*)&ifr6.ifr_addr,
								sizeof(ifr6.ifr_addr));
		if (result != 0) {
			printf("utun_remove_address - ifaddr_address failed (v6): %d",
				   result);
		}
		else {
			result = sock_ioctl(pf_socket, SIOCDIFADDR_IN6, &ifr6);
			if (result != 0) {
				printf("utun_remove_address - SIOCDIFADDR_IN6 failed: %d",
					   result);
			}
		}
	}
}

static void
utun_cleanup_family(
	ifnet_t				interface,
	protocol_family_t	protocol)
{
	errno_t		result = 0;
	socket_t	pf_socket = NULL;
	ifaddr_t	*addresses = NULL;
	int			i;
	
	if (protocol != PF_INET && protocol != PF_INET6) {
		printf("utun_cleanup_family - invalid protocol family %d\n", protocol);
		return;
	}
	
	/* Create a socket for removing addresses and detaching the protocol */
	result = sock_socket(protocol, SOCK_DGRAM, 0, NULL, NULL, &pf_socket);
	if (result != 0) {
		if (result != EAFNOSUPPORT)
			printf("utun_cleanup_family - failed to create %s socket: %d\n",
				protocol == PF_INET ? "IP" : "IPv6", result);
		goto cleanup;
	}
	
        /* always set SS_PRIV, we want to close and detach regardless */
        sock_setpriv(pf_socket, 1);

	result = utun_detach_ip(interface, protocol, pf_socket);
	if (result == 0 || result == ENXIO) {
		/* We are done! We either detached or weren't attached. */
		goto cleanup;
	}
	else if (result != EBUSY) {
		/* Uh, not really sure what happened here... */
		printf("utun_cleanup_family - utun_detach_ip failed: %d\n", result);
		goto cleanup;
	}
	
	/*
	 * At this point, we received an EBUSY error. This means there are
	 * addresses attached. We should detach them and then try again.
	 */
	result = ifnet_get_address_list_family(interface, &addresses, protocol);
	if (result != 0) {
		printf("fnet_get_address_list_family(%s%d, 0xblah, %s) - failed: %d\n",
			ifnet_name(interface), ifnet_unit(interface), 
			protocol == PF_INET ? "PF_INET" : "PF_INET6", result);
		goto cleanup;
	}
	
	for (i = 0; addresses[i] != 0; i++) {
		utun_remove_address(interface, protocol, addresses[i], pf_socket);
	}
	ifnet_free_address_list(addresses);
	addresses = NULL;
	
	/*
	 * The addresses should be gone, we should try the remove again.
	 */
	result = utun_detach_ip(interface, protocol, pf_socket);
	if (result != 0 && result != ENXIO) {
		printf("utun_cleanup_family - utun_detach_ip failed: %d\n", result);
	}
	
cleanup:
	if (pf_socket != NULL)
		sock_close(pf_socket);
	
	if (addresses != NULL)
		ifnet_free_address_list(addresses);
}

static errno_t
utun_ctl_disconnect(
	__unused kern_ctl_ref	kctlref,
	__unused u_int32_t		unit,
	void					*unitinfo)
{
	struct utun_pcb	*pcb = unitinfo;
	ifnet_t			ifp = pcb->utun_ifp;
	errno_t			result = 0;

	utun_cleanup_crypto(pcb);

	pcb->utun_ctlref = NULL;
	pcb->utun_unit = 0;
	
	/*
	 * We want to do everything in our power to ensure that the interface
	 * really goes away when the socket is closed. We must remove IP/IPv6
	 * addresses and detach the protocols. Finally, we can remove and
	 * release the interface.
	 */
	utun_cleanup_family(ifp, AF_INET);
	utun_cleanup_family(ifp, AF_INET6);
	
	if ((result = ifnet_detach(ifp)) != 0) {
		printf("utun_ctl_disconnect - ifnet_detach failed: %d\n", result);
	}
	
	if ((result = ifnet_release(ifp)) != 0) {
		printf("utun_ctl_disconnect - ifnet_release failed: %d\n", result);
	}
	
	return 0;
}	

static errno_t
utun_ctl_send(
	__unused kern_ctl_ref	kctlref,
	__unused u_int32_t		unit,
	void					*unitinfo,
	mbuf_t					m,
	__unused int			flags)
{
	/*
	 * The userland ABI requires the first four bytes have the protocol family 
	 * in network byte order: swap them
	 */
	if (m_pktlen(m) >= 4)
		*(protocol_family_t *)mbuf_data(m) = ntohl(*(protocol_family_t *)mbuf_data(m));
	else
		printf("%s - unexpected short mbuf pkt len %d\n", __func__, m_pktlen(m) );

	return utun_pkt_input((struct utun_pcb *)unitinfo, m);
}

static errno_t
utun_ctl_setopt(
	__unused kern_ctl_ref	kctlref,
	__unused u_int32_t		unit, 
	void					*unitinfo,
	int						opt, 
	void					*data, 
	size_t					len)
{
	struct utun_pcb			*pcb = unitinfo;
	errno_t					result = 0;
	
	/* check for privileges for privileged options */
	switch (opt) {
		case UTUN_OPT_FLAGS:
		case UTUN_OPT_EXT_IFDATA_STATS:
		case UTUN_OPT_SET_DELEGATE_INTERFACE:
			if (kauth_cred_issuser(kauth_cred_get()) == 0) {
				return EPERM;
			}
			break;
	}

	switch (opt) {
		case UTUN_OPT_FLAGS:
			if (len != sizeof(u_int32_t))
				result = EMSGSIZE;
			else
				pcb->utun_flags = *(u_int32_t *)data;
			break;

		case UTUN_OPT_ENABLE_CRYPTO:
			result = utun_ctl_enable_crypto(kctlref, unit, unitinfo, opt, data, len);
			break;

		case UTUN_OPT_CONFIG_CRYPTO_KEYS:
			result = utun_ctl_config_crypto_keys(kctlref, unit, unitinfo, opt, data, len);
			break;

		case UTUN_OPT_UNCONFIG_CRYPTO_KEYS:
			result = utun_ctl_unconfig_crypto_keys(kctlref, unit, unitinfo, opt, data, len);
			break;

		case UTUN_OPT_DISABLE_CRYPTO:
			result = utun_ctl_disable_crypto(kctlref, unit, unitinfo, opt, data, len);
			break;

		case UTUN_OPT_STOP_CRYPTO_DATA_TRAFFIC:
			result = utun_ctl_stop_crypto_data_traffic(kctlref, unit, unitinfo, opt, data, len);
			break;

		case UTUN_OPT_START_CRYPTO_DATA_TRAFFIC:
			result = utun_ctl_start_crypto_data_traffic(kctlref, unit, unitinfo, opt, data, len);
			break;

		case UTUN_OPT_CONFIG_CRYPTO_FRAMER:
			result = utun_ctl_config_crypto_framer(kctlref, unit, unitinfo, opt, data, len);
			break;

		case UTUN_OPT_UNCONFIG_CRYPTO_FRAMER:
			result = utun_ctl_unconfig_crypto_framer(kctlref, unit, unitinfo, opt, data, len);
			break;

		case UTUN_OPT_EXT_IFDATA_STATS:
			if (len != sizeof(int)) {
				result = EMSGSIZE;
				break;
			}
			pcb->utun_ext_ifdata_stats = (*(int *)data) ? 1 : 0;
			break;
			
		case UTUN_OPT_INC_IFDATA_STATS_IN:
		case UTUN_OPT_INC_IFDATA_STATS_OUT: {
			struct utun_stats_param *utsp = (struct utun_stats_param *)data;
			
			if (utsp == NULL || len < sizeof(struct utun_stats_param)) {
				result = EINVAL;
				break;
			}
			if (!pcb->utun_ext_ifdata_stats) {
				result = EINVAL;
				break;
			}
			if (opt == UTUN_OPT_INC_IFDATA_STATS_IN)
				ifnet_stat_increment_in(pcb->utun_ifp, utsp->utsp_packets, 
					utsp->utsp_bytes, utsp->utsp_errors);
			else
				ifnet_stat_increment_out(pcb->utun_ifp, utsp->utsp_packets, 
					utsp->utsp_bytes, utsp->utsp_errors);
			break;
		}
		case UTUN_OPT_SET_DELEGATE_INTERFACE: {
			ifnet_t		del_ifp = NULL;
			char            name[IFNAMSIZ];

			if (len > IFNAMSIZ - 1) {
				result = EMSGSIZE;
				break;
			}
			if (len != 0) {    /* if len==0, del_ifp will be NULL causing the delegate to be removed */
				bcopy(data, name, len);
				name[len] = 0;
				result = ifnet_find_by_name(name, &del_ifp);
			}
			if (result == 0) {
				result = ifnet_set_delegate(pcb->utun_ifp, del_ifp);
				if (del_ifp)
					ifnet_release(del_ifp);            
			}
			break;
		}
		case UTUN_OPT_MAX_PENDING_PACKETS: {
			u_int32_t max_pending_packets = 0;
			if (len != sizeof(u_int32_t)) {
				result = EMSGSIZE;
				break;
			}
			max_pending_packets = *(u_int32_t *)data;
			if (max_pending_packets == 0) {
				result = EINVAL;
				break;
			}
			pcb->utun_max_pending_packets = max_pending_packets;
			break;
		}
		default: {
			result = ENOPROTOOPT;
			break;
		}
	}

	return result;
}

static errno_t
utun_ctl_getopt(
	__unused kern_ctl_ref	kctlref,
	__unused u_int32_t		unit, 
	void					*unitinfo,
	int						opt, 
	void					*data, 
	size_t					*len)
{
	struct utun_pcb			*pcb = unitinfo;
	errno_t					result = 0;
	
	switch (opt) {
		case UTUN_OPT_FLAGS:
			if (*len != sizeof(u_int32_t))
				result = EMSGSIZE;
			else
				*(u_int32_t *)data = pcb->utun_flags;
			break;

		case UTUN_OPT_EXT_IFDATA_STATS:
			if (*len != sizeof(int))
				result = EMSGSIZE;
			else
				*(int *)data = (pcb->utun_ext_ifdata_stats) ? 1 : 0;
			break;
		
		case UTUN_OPT_IFNAME:
			*len = snprintf(data, *len, "%s%d", ifnet_name(pcb->utun_ifp), ifnet_unit(pcb->utun_ifp)) + 1;
			break;

		case UTUN_OPT_GENERATE_CRYPTO_KEYS_IDX:
			result = utun_ctl_generate_crypto_keys_idx(kctlref, unit, unitinfo, opt, data, len);
			break;
		case UTUN_OPT_MAX_PENDING_PACKETS: {
			*len = sizeof(u_int32_t);
			*((u_int32_t *)data) = pcb->utun_max_pending_packets;
			break;
		}
		default:
			result = ENOPROTOOPT;
			break;
	}
	
	return result;
}

static void
utun_ctl_rcvd(kern_ctl_ref kctlref, u_int32_t unit, void *unitinfo, int flags)
{
#pragma unused(kctlref, unit, flags)
	bool reenable_output = false;
	struct utun_pcb *pcb = unitinfo;
	if (pcb == NULL) {
		return;
	}
	ifnet_lock_exclusive(pcb->utun_ifp);
	if (pcb->utun_pending_packets > 0) {
		pcb->utun_pending_packets--;
		if (pcb->utun_pending_packets < pcb->utun_max_pending_packets) {
			reenable_output = true;
		}
	}
	
	if (reenable_output) {
		errno_t error = ifnet_enable_output(pcb->utun_ifp);
		if (error != 0) {
			printf("utun_ctl_rcvd: ifnet_enable_output returned error %d\n", error);
		}
	}
	ifnet_lock_done(pcb->utun_ifp);
}

/* Network Interface functions */
static void
utun_start(ifnet_t interface)
{
	mbuf_t data;
	struct utun_pcb*pcb = ifnet_softc(interface);
	for (;;) {
		bool can_accept_packets = true;
		ifnet_lock_shared(pcb->utun_ifp);
		can_accept_packets = (pcb->utun_pending_packets < pcb->utun_max_pending_packets);
		if (!can_accept_packets && pcb->utun_ctlref) {
			u_int32_t difference = 0;
			if (ctl_getenqueuereadable(pcb->utun_ctlref, pcb->utun_unit, &difference) == 0) {
				if (difference > 0) {
					// If the low-water mark has not yet been reached, we still need to enqueue data
					// into the buffer
					can_accept_packets = true;
				}
			}
		}
		if (!can_accept_packets) {
			errno_t error = ifnet_disable_output(interface);
			if (error != 0) {
				printf("utun_start: ifnet_disable_output returned error %d\n", error);
			}
			ifnet_lock_done(pcb->utun_ifp);
			break;
		}
		ifnet_lock_done(pcb->utun_ifp);
		if (ifnet_dequeue(interface, &data) != 0)
			break;
		if (utun_output(interface, data) != 0)
			break;
	}
}

static errno_t
utun_output(
			   ifnet_t	interface,
			   mbuf_t	data)
{
	struct utun_pcb	*pcb = ifnet_softc(interface);
	errno_t			result;
	
	if (m_pktlen(data) >= 4) {
		bpf_tap_out(pcb->utun_ifp, DLT_NULL, data, 0, 0);
	}
	
	if (pcb->utun_flags & UTUN_FLAGS_NO_OUTPUT) {
		/* flush data */
		mbuf_freem(data);
		return 0;
	}

	// otherwise, fall thru to ctl_enqueumbuf
	if (pcb->utun_ctlref) {
		int	length;

		// only pass packets to utun-crypto if crypto is enabled and 'suspend data traffic' is not.
		if ((pcb->utun_flags & (UTUN_FLAGS_CRYPTO | UTUN_FLAGS_CRYPTO_STOP_DATA_TRAFFIC)) == UTUN_FLAGS_CRYPTO) {
			if (utun_pkt_crypto_output(pcb, &data) == 0) {
				return 0;
			}
		}

		/*
		 * The ABI requires the protocol in network byte order
		 */
		if (m_pktlen(data) >= 4)
			*(u_int32_t *)mbuf_data(data) = htonl(*(u_int32_t *)mbuf_data(data));

		length = mbuf_pkthdr_len(data);
		// Increment packet count optimistically
		ifnet_lock_exclusive(pcb->utun_ifp);
		pcb->utun_pending_packets++;
		ifnet_lock_done(pcb->utun_ifp);
		result = ctl_enqueuembuf(pcb->utun_ctlref, pcb->utun_unit, data, CTL_DATA_EOR);
		if (result != 0) {
			// Decrement packet count if errored
			ifnet_lock_exclusive(pcb->utun_ifp);
			pcb->utun_pending_packets--;
			ifnet_lock_done(pcb->utun_ifp);
			mbuf_freem(data);
			printf("utun_output - ctl_enqueuembuf failed: %d\n", result);

			ifnet_stat_increment_out(interface, 0, 0, 1);
		}
		else {
			if (!pcb->utun_ext_ifdata_stats)
				ifnet_stat_increment_out(interface, 1, length, 0);
		}
	}
	else 
		mbuf_freem(data);
	
	return 0;
}

static errno_t
utun_demux(
	__unused ifnet_t	interface,
	mbuf_t				data,
	__unused char		*frame_header,
	protocol_family_t	*protocol)
{
	
	while (data != NULL && mbuf_len(data) < 1) {
		data = mbuf_next(data);
	}
	
	if (data == NULL)
		return ENOENT;
	
	*protocol = *(u_int32_t *)mbuf_data(data);
	return 0;
}

static errno_t
utun_framer(
		   __unused ifnet_t				interface,
		   mbuf_t				*packet,
			__unused const struct sockaddr *dest, 
			__unused const char *desk_linkaddr,
			const char *frame_type,
			u_int32_t *prepend_len, 
			u_int32_t *postpend_len)
{
    if (mbuf_prepend(packet, sizeof(protocol_family_t), MBUF_DONTWAIT) != 0) {
		printf("utun_framer - ifnet_output prepend failed\n");

		ifnet_stat_increment_out(interface, 0, 0, 1);

		// just	return, because the buffer was freed in mbuf_prepend
        return EJUSTRETURN;	
    }
	if (prepend_len != NULL)
		*prepend_len = sizeof(protocol_family_t);
	if (postpend_len != NULL)
		*postpend_len = 0;
	
    // place protocol number at the beginning of the mbuf
    *(protocol_family_t *)mbuf_data(*packet) = *(protocol_family_t *)(uintptr_t)(size_t)frame_type;
    
    return 0;
}

static errno_t
utun_add_proto(
	__unused ifnet_t						interface,
	protocol_family_t						protocol,
	__unused const struct ifnet_demux_desc	*demux_array,
	__unused u_int32_t						demux_count)
{
	switch(protocol) {
		case PF_INET:
			return 0;
		case PF_INET6:
			return 0;
		default:
			break;
	}
	
	return ENOPROTOOPT;
}

static errno_t
utun_del_proto(
	__unused ifnet_t 			interface,
	__unused protocol_family_t	protocol)
{
	return 0;
}

static errno_t
utun_ioctl(
	ifnet_t		interface,
	u_long		command,
	void		*data)
{
	errno_t	result = 0;
	
	switch(command) {
		case SIOCSIFMTU:
			ifnet_set_mtu(interface, ((struct ifreq*)data)->ifr_mtu);
			break;
			
		case SIOCSIFFLAGS:
			/* ifioctl() takes care of it */
			break;
			
		default:
			result = EOPNOTSUPP;
	}
	
	return result;
}

static void
utun_detached(
	ifnet_t	interface)
{
	struct utun_pcb	*pcb = ifnet_softc(interface);
	
	utun_free(pcb);
	
	OSDecrementAtomic(&utun_ifcount);
}

/* Protocol Handlers */

static errno_t
utun_proto_input(
	__unused ifnet_t	interface,
	protocol_family_t	protocol,
	mbuf_t				m,
	__unused char		*frame_header)
{
	
	// remove protocol family first
	mbuf_adj(m, sizeof(u_int32_t));
	
	if (proto_input(protocol, m) != 0)
		m_freem(m);
	
	return 0;
}

static errno_t
utun_proto_pre_output(
	__unused ifnet_t	interface,
	protocol_family_t	protocol,
	__unused mbuf_t		*packet,
	__unused const struct sockaddr *dest,
	__unused void *route, 
	__unused char *frame_type, 
	__unused char *link_layer_dest)
{
	
	*(protocol_family_t *)(void *)frame_type = protocol;
		return 0;
}

static errno_t
utun_attach_proto(
	ifnet_t				interface,
	protocol_family_t	protocol)
{
	struct ifnet_attach_proto_param	proto;
	errno_t							result;
	
	bzero(&proto, sizeof(proto));
	proto.input = utun_proto_input;
	proto.pre_output = utun_proto_pre_output;

	result = ifnet_attach_protocol(interface, protocol, &proto);
	if (result != 0 && result != EEXIST) {
		printf("utun_attach_inet - ifnet_attach_protocol %d failed: %d\n",
			protocol, result);
	}
	
	return result;
}

errno_t
utun_pkt_input (struct utun_pcb *pcb, mbuf_t m)
{
	errno_t	result;
	protocol_family_t protocol = 0;

	mbuf_pkthdr_setrcvif(m, pcb->utun_ifp);

	if (m_pktlen(m) >= 4)  {
		protocol = *(u_int32_t *)mbuf_data(m);
	
		bpf_tap_in(pcb->utun_ifp, DLT_NULL, m, 0, 0);
	}
	if (pcb->utun_flags & UTUN_FLAGS_NO_INPUT) {
		/* flush data */
		mbuf_freem(m);
		return 0;
	}

	// quick exit for keepalive packets
	if (protocol == AF_UTUN && pcb->utun_flags & UTUN_FLAGS_CRYPTO) {
		if (utun_pkt_crypto_output(pcb, &m) == 0) {
			return 0;
		}
		printf("%s: utun_pkt_crypto_output failed, flags %x\n", __FUNCTION__, pcb->utun_flags);
		return EINVAL;
	}

	if (!pcb->utun_ext_ifdata_stats) {
		struct ifnet_stat_increment_param	incs;
		
		bzero(&incs, sizeof(incs));
		incs.packets_in = 1;
		incs.bytes_in = mbuf_pkthdr_len(m);
		result = ifnet_input(pcb->utun_ifp, m, &incs);
	} else {
		result = ifnet_input(pcb->utun_ifp, m, NULL);
	}
	if (result != 0) {
		ifnet_stat_increment_in(pcb->utun_ifp, 0, 0, 1);
		
		printf("%s - ifnet_input failed: %d\n", __FUNCTION__, result);
		mbuf_freem(m);
	}

	return 0;
}

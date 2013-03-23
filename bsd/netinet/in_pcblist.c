/*
 * Copyright (c) 2010 Apple Inc. All rights reserved.
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
 * Copyright (c) 1982, 1986, 1990, 1993
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
 */

#include <sys/types.h>
#include <sys/malloc.h>
#include <sys/socket.h>
#include <sys/socketvar.h>
#include <sys/protosw.h>
#include <sys/domain.h>
#include <sys/kernel.h>
#include <sys/sysctl.h>
#include <sys/dtrace.h>

#include <net/route.h>

#include <netinet/in.h>
#include <netinet/in_pcb.h>
#include <netinet/ip_var.h>

#include <netinet/udp.h>
#include <netinet/udp_var.h>

#include <netinet/tcp.h>
#include <netinet/tcp_fsm.h>
#include <netinet/tcp_seq.h>
#include <netinet/tcp_timer.h>
#include <netinet/tcp_var.h>

#ifndef ROUNDUP64
#define ROUNDUP64(x) P2ROUNDUP((x), sizeof(u_int64_t))
#endif

#ifndef ADVANCE64
#define ADVANCE64(p, n) (void*)((char *)(p) + ROUNDUP64(n))
#endif


void sotoxsocket_n(struct socket *, struct xsocket_n *);
void sbtoxsockbuf_n(struct sockbuf *, struct xsockbuf_n *);
void sbtoxsockstat_n(struct socket *, struct xsockstat_n *);
void inpcb_to_xinpcb_n(struct inpcb *, struct xinpcb_n *);
void tcpcb_to_xtcpcb_n(struct tcpcb *, struct xtcpcb_n *);

__private_extern__ void
sotoxsocket_n(struct socket *so, struct xsocket_n *xso)
{
	xso->xso_len = sizeof(struct xsocket_n);
	xso->xso_kind = XSO_SOCKET;

	if (so != NULL) {
		xso->xso_so = (u_int64_t)(uintptr_t)so;
		xso->so_type = so->so_type;
		xso->so_options = so->so_options;
		xso->so_linger = so->so_linger;
		xso->so_state = so->so_state;
		xso->so_pcb = (u_int64_t)(uintptr_t)so->so_pcb;
		if (so->so_proto) {
			xso->xso_protocol = so->so_proto->pr_protocol;
			xso->xso_family = so->so_proto->pr_domain->dom_family;
		} else {
			xso->xso_protocol = xso->xso_family = 0;
		}
		xso->so_qlen = so->so_qlen;
		xso->so_incqlen = so->so_incqlen;
		xso->so_qlimit = so->so_qlimit;
		xso->so_timeo = so->so_timeo;
		xso->so_error = so->so_error;
		xso->so_pgid = so->so_pgid;
		xso->so_oobmark = so->so_oobmark;
		xso->so_uid = so->so_uid;
	}
}

__private_extern__ void
sbtoxsockbuf_n(struct sockbuf *sb, struct xsockbuf_n *xsb)
{
	xsb->xsb_len = sizeof(struct xsockbuf_n);
	xsb->xsb_kind = (sb->sb_flags & SB_RECV) ? XSO_RCVBUF : XSO_SNDBUF;

	if (sb != NULL) {
		xsb->sb_cc = sb->sb_cc;
		xsb->sb_hiwat = sb->sb_hiwat;
		xsb->sb_mbcnt = sb->sb_mbcnt;
		xsb->sb_mbmax = sb->sb_mbmax;
		xsb->sb_lowat = sb->sb_lowat;
		xsb->sb_flags = sb->sb_flags;
		xsb->sb_timeo = (short)
			(sb->sb_timeo.tv_sec * hz) + sb->sb_timeo.tv_usec / tick;
		if (xsb->sb_timeo == 0 && sb->sb_timeo.tv_usec != 0)
			xsb->sb_timeo = 1;
	}
}

__private_extern__ void
sbtoxsockstat_n(struct socket *so, struct xsockstat_n *xst)
{
	int i;

	xst->xst_len = sizeof(struct xsockstat_n);
	xst->xst_kind = XSO_STATS;
	
	for (i = 0; i < SO_TC_STATS_MAX; i++) {
		xst->xst_tc_stats[i].rxpackets = so->so_tc_stats[i].rxpackets;
		xst->xst_tc_stats[i].rxbytes = so->so_tc_stats[i].rxbytes;
		xst->xst_tc_stats[i].txpackets = so->so_tc_stats[i].txpackets;
		xst->xst_tc_stats[i].txbytes = so->so_tc_stats[i].txbytes;
	}
}

__private_extern__ void
inpcb_to_xinpcb_n(struct inpcb *inp, struct xinpcb_n *xinp)
{
	xinp->xi_len = sizeof(struct xinpcb_n);
	xinp->xi_kind = XSO_INPCB;
	xinp->xi_inpp = (u_int64_t)(uintptr_t)inp;
	xinp->inp_fport = inp->inp_fport;
	xinp->inp_lport = inp->inp_lport;
	xinp->inp_ppcb = (u_int64_t)(uintptr_t)inp->inp_ppcb;
	xinp->inp_gencnt = inp->inp_gencnt;
	xinp->inp_flags = inp->inp_flags;
	xinp->inp_flow = inp->inp_flow;
	xinp->inp_vflag = inp->inp_vflag;
	xinp->inp_ip_ttl = inp->inp_ip_ttl;
	xinp->inp_ip_p = inp->inp_ip_p;
	xinp->inp_dependfaddr.inp6_foreign = inp->inp_dependfaddr.inp6_foreign;
	xinp->inp_dependladdr.inp6_local = inp->inp_dependladdr.inp6_local;
	xinp->inp_depend4.inp4_ip_tos = inp->inp_depend4.inp4_ip_tos;
	xinp->inp_depend6.inp6_hlim = inp->inp_depend6.inp6_hlim;
	xinp->inp_depend6.inp6_cksum = inp->inp_depend6.inp6_cksum;
	xinp->inp_depend6.inp6_ifindex = inp->inp_depend6.inp6_ifindex;
	xinp->inp_depend6.inp6_hops = inp->inp_depend6.inp6_hops;
}

__private_extern__ void
tcpcb_to_xtcpcb_n(struct tcpcb *tp, struct xtcpcb_n *xt)
{
	int i;

	xt->xt_len = sizeof(struct xtcpcb_n);
	xt->xt_kind = XSO_TCPCB;

	xt->t_segq = (u_int32_t)(uintptr_t)tp->t_segq.lh_first;
	xt->t_dupacks = tp->t_dupacks;
	for (i = 0; i < TCPT_NTIMERS_EXT; i++)
		xt->t_timer[i] = tp->t_timer[i];
	xt->t_state = tp->t_state;
	xt->t_flags = tp->t_flags;
	xt->t_force = tp->t_force;
	xt->snd_una = tp->snd_una;
	xt->snd_max = tp->snd_max;
	xt->snd_nxt = tp->snd_nxt;
	xt->snd_up = tp->snd_up;
	xt->snd_wl1 = tp->snd_wl1;
	xt->snd_wl2 = tp->snd_wl2;
	xt->iss = tp->iss;
	xt->irs = tp->irs;
	xt->rcv_nxt = tp->rcv_nxt;
	xt->rcv_adv = tp->rcv_adv;
	xt->rcv_wnd = tp->rcv_wnd;
	xt->rcv_up = tp->rcv_up;
	xt->snd_wnd = tp->snd_wnd;
	xt->snd_cwnd = tp->snd_cwnd;
	xt->snd_ssthresh = tp->snd_ssthresh;
	xt->t_maxopd = tp->t_maxopd;
	xt->t_rcvtime = tp->t_rcvtime;
	xt->t_starttime = tp->t_starttime;
	xt->t_rtttime = tp->t_rtttime;
	xt->t_rtseq = tp->t_rtseq;
	xt->t_rxtcur = tp->t_rxtcur;
	xt->t_maxseg = tp->t_maxseg;
	xt->t_srtt = tp->t_srtt;
	xt->t_rttvar = tp->t_rttvar;
	xt->t_rxtshift = tp->t_rxtshift;
	xt->t_rttmin = tp->t_rttmin;
	xt->t_rttupdated = tp->t_rttupdated;
	xt->max_sndwnd = tp->max_sndwnd;
	xt->t_softerror = tp->t_softerror;
	xt->t_oobflags = tp->t_oobflags;
	xt->t_iobc = tp->t_iobc;
	xt->snd_scale = tp->snd_scale;
	xt->rcv_scale = tp->rcv_scale;
	xt->request_r_scale = tp->request_r_scale;
	xt->requested_s_scale = tp->requested_s_scale;
	xt->ts_recent = tp->ts_recent;
	xt->ts_recent_age = tp->ts_recent_age;
	xt->last_ack_sent = tp->last_ack_sent;
	xt->cc_send = tp->cc_send;
	xt->cc_recv = tp->cc_recv;
	xt->snd_recover = tp->snd_recover;
	xt->snd_cwnd_prev = tp->snd_cwnd_prev;
	xt->snd_ssthresh_prev = tp->snd_ssthresh_prev;
	xt->t_badrxtwin = tp->t_badrxtwin;
}

__private_extern__ int
get_pcblist_n(short proto, struct sysctl_req *req, struct inpcbinfo *pcbinfo)
{
	int error = 0;
	int i, n;
	struct inpcb *inp, **inp_list = NULL;
	inp_gen_t gencnt;
	struct xinpgen xig;
	void *buf = NULL;
	size_t item_size = ROUNDUP64(sizeof(struct xinpcb_n)) +  
						ROUNDUP64(sizeof(struct xsocket_n)) +
						2 * ROUNDUP64(sizeof(struct xsockbuf_n)) + 
						ROUNDUP64(sizeof(struct xsockstat_n));

	if (proto == IPPROTO_TCP)
		item_size += ROUNDUP64(sizeof(struct xtcpcb_n));

	/*
	 * The process of preparing the PCB list is too time-consuming and
	 * resource-intensive to repeat twice on every request.
	 */
	lck_rw_lock_exclusive(pcbinfo->mtx);
	if (req->oldptr == USER_ADDR_NULL) {
			n = pcbinfo->ipi_count;
			req->oldidx = 2 * (sizeof xig)
					+ (n + n/8) * item_size;
			goto done;
	}

	if (req->newptr != USER_ADDR_NULL) {
			error = EPERM;
			goto done;
	}

	/*
	 * OK, now we're committed to doing something.
	 */
	gencnt = pcbinfo->ipi_gencnt;
	n = pcbinfo->ipi_count;

	bzero(&xig, sizeof(xig));
	xig.xig_len = sizeof xig;
	xig.xig_count = n;
	xig.xig_gen = gencnt;
	xig.xig_sogen = so_gencnt;
	error = SYSCTL_OUT(req, &xig, sizeof xig);
	if (error) {
			goto done;
	}
    /*
     * We are done if there is no pcb
     */
    if (n == 0) {
			goto done;
    }

	buf = _MALLOC(item_size, M_TEMP, M_WAITOK);
	if (buf == 0) {
			error = ENOMEM;
			goto done;
	}

	inp_list = _MALLOC(n * sizeof *inp_list, M_TEMP, M_WAITOK);
	if (inp_list == 0) {
			error = ENOMEM;
			goto done;
	}

	for (inp = pcbinfo->listhead->lh_first, i = 0; inp && i < n;
		 inp = inp->inp_list.le_next) {
			if (inp->inp_gencnt <= gencnt && inp->inp_state != INPCB_STATE_DEAD)
					inp_list[i++] = inp;
	}
	n = i;

	error = 0;
	for (i = 0; i < n; i++) {
		inp = inp_list[i];
		if (inp->inp_gencnt <= gencnt && inp->inp_state != INPCB_STATE_DEAD) {
			struct xinpcb_n *xi = (struct xinpcb_n *)buf;
			struct xsocket_n *xso = (struct xsocket_n *)ADVANCE64(xi, sizeof(*xi));
			struct xsockbuf_n *xsbrcv = (struct xsockbuf_n *)ADVANCE64(xso, sizeof(*xso));
			struct xsockbuf_n *xsbsnd = (struct xsockbuf_n *)ADVANCE64(xsbrcv, sizeof(*xsbrcv));
			struct xsockstat_n *xsostats = (struct xsockstat_n *)ADVANCE64(xsbsnd, sizeof(*xsbsnd));
			
			bzero(buf, item_size);

			inpcb_to_xinpcb_n(inp, xi);
			sotoxsocket_n(inp->inp_socket, xso);
			sbtoxsockbuf_n(inp->inp_socket ? &inp->inp_socket->so_rcv : NULL, xsbrcv);
			sbtoxsockbuf_n(inp->inp_socket ? &inp->inp_socket->so_snd : NULL, xsbsnd);
			sbtoxsockstat_n(inp->inp_socket, xsostats);
			if (proto == IPPROTO_TCP) {
				struct  xtcpcb_n *xt = (struct xtcpcb_n *)ADVANCE64(xsostats, sizeof(*xsostats));
				
				/*
				 * inp->inp_ppcb, can only be NULL on
				 * an initialization race window.
				 * No need to lock.
				 */
				if (inp->inp_ppcb == NULL)
					continue;
				
				tcpcb_to_xtcpcb_n((struct tcpcb *)inp->inp_ppcb, xt);
			}
			error = SYSCTL_OUT(req, buf, item_size);
		}
	}
	if (!error) {
		/*
		 * Give the user an updated idea of our state.
		 * If the generation differs from what we told
		 * her before, she knows that something happened
		 * while we were processing this request, and it
		 * might be necessary to retry.
		 */
		bzero(&xig, sizeof(xig));
		xig.xig_len = sizeof xig;
		xig.xig_gen = pcbinfo->ipi_gencnt;
		xig.xig_sogen = so_gencnt;
		xig.xig_count = pcbinfo->ipi_count;
		error = SYSCTL_OUT(req, &xig, sizeof xig);
	}
done:
	lck_rw_done(pcbinfo->mtx);
	if (inp_list)
		FREE(inp_list, M_TEMP);
	if (buf)
		FREE(buf, M_TEMP);
	return error;
}


/*
 * Copyright (c) 2000-2011 Apple Computer, Inc. All rights reserved.
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
 *	@(#)tcp.h	8.1 (Berkeley) 6/10/93
 * $FreeBSD: src/sys/netinet/tcp.h,v 1.13.2.3 2001/03/01 22:08:42 jlemon Exp $
 */

#ifndef _NETINET_TCP_H_
#define _NETINET_TCP_H_
#include <sys/appleapiopts.h>
#include <sys/_types.h>
#include <machine/endian.h>

#if !defined(_POSIX_C_SOURCE) || defined(_DARWIN_C_SOURCE)
typedef	__uint32_t tcp_seq;
typedef __uint32_t tcp_cc;		/* connection count per rfc1644 */

#define tcp6_seq	tcp_seq	/* for KAME src sync over BSD*'s */
#define tcp6hdr		tcphdr	/* for KAME src sync over BSD*'s */

/*
 * TCP header.
 * Per RFC 793, September, 1981.
 */
struct tcphdr {
	unsigned short	th_sport;	/* source port */
	unsigned short	th_dport;	/* destination port */
	tcp_seq	th_seq;			/* sequence number */
	tcp_seq	th_ack;			/* acknowledgement number */
#if __DARWIN_BYTE_ORDER == __DARWIN_LITTLE_ENDIAN
	unsigned int	th_x2:4,	/* (unused) */
			th_off:4;	/* data offset */
#endif
#if __DARWIN_BYTE_ORDER == __DARWIN_BIG_ENDIAN
	unsigned int	th_off:4,	/* data offset */
			th_x2:4;	/* (unused) */
#endif
	unsigned char	th_flags;
#define	TH_FIN	0x01
#define	TH_SYN	0x02
#define	TH_RST	0x04
#define	TH_PUSH	0x08
#define	TH_ACK	0x10
#define	TH_URG	0x20
#define	TH_ECE	0x40
#define	TH_CWR	0x80
#define	TH_FLAGS	(TH_FIN|TH_SYN|TH_RST|TH_ACK|TH_URG|TH_ECE|TH_CWR)

	unsigned short	th_win;		/* window */
	unsigned short	th_sum;		/* checksum */
	unsigned short	th_urp;		/* urgent pointer */
};

#define	TCPOPT_EOL		0
#define	TCPOPT_NOP		1
#define	TCPOPT_MAXSEG		2
#define TCPOLEN_MAXSEG		4
#define TCPOPT_WINDOW		3
#define TCPOLEN_WINDOW		3
#define TCPOPT_SACK_PERMITTED	4		/* Experimental */
#define TCPOLEN_SACK_PERMITTED	2
#define TCPOPT_SACK		5		/* Experimental */
#define TCPOLEN_SACK		8		/* len of sack block */
#define TCPOPT_TIMESTAMP	8
#define TCPOLEN_TIMESTAMP	10
#define TCPOLEN_TSTAMP_APPA		(TCPOLEN_TIMESTAMP+2) /* appendix A */
#define TCPOPT_TSTAMP_HDR		\
    (TCPOPT_NOP<<24|TCPOPT_NOP<<16|TCPOPT_TIMESTAMP<<8|TCPOLEN_TIMESTAMP)

#define	MAX_TCPOPTLEN		40	/* Absolute maximum TCP options len */

#define	TCPOPT_CC		11		/* CC options: RFC-1644 */
#define TCPOPT_CCNEW		12
#define TCPOPT_CCECHO		13
#define	   TCPOLEN_CC			6
#define	   TCPOLEN_CC_APPA		(TCPOLEN_CC+2)
#define	   TCPOPT_CC_HDR(ccopt)		\
    (TCPOPT_NOP<<24|TCPOPT_NOP<<16|(ccopt)<<8|TCPOLEN_CC)
#define	TCPOPT_SIGNATURE		19	/* Keyed MD5: RFC 2385 */
#define	   TCPOLEN_SIGNATURE		18

/* Option definitions */
#define TCPOPT_SACK_PERMIT_HDR	\
(TCPOPT_NOP<<24|TCPOPT_NOP<<16|TCPOPT_SACK_PERMITTED<<8|TCPOLEN_SACK_PERMITTED)
#define	TCPOPT_SACK_HDR		(TCPOPT_NOP<<24|TCPOPT_NOP<<16|TCPOPT_SACK<<8)
/* Miscellaneous constants */
#define	MAX_SACK_BLKS	6	/* Max # SACK blocks stored at sender side */
#define	TCP_MAX_SACK	3	/* MAX # SACKs sent in any segment */


/*
 * Default maximum segment size for TCP.
 * With an IP MTU of 576, this is 536,
 * but 512 is probably more convenient.
 * This should be defined as MIN(512, IP_MSS - sizeof (struct tcpiphdr)).
 */
#define	TCP_MSS	512

/*
 * TCP_MINMSS is defined to be 216 which is fine for the smallest
 * link MTU (256 bytes, SLIP interface) in the Internet.
 * However it is very unlikely to come across such low MTU interfaces
 * these days (anno dato 2004).
 * Probably it can be set to 512 without ill effects. But we play safe.
 * See tcp_subr.c tcp_minmss SYSCTL declaration for more comments.
 * Setting this to "0" disables the minmss check.
 */
#define	TCP_MINMSS 216

/*
 * TCP_MINMSSOVERLOAD is defined to be 1000 which should cover any type
 * of interactive TCP session.
 * See tcp_subr.c tcp_minmssoverload SYSCTL declaration and tcp_input.c
 * for more comments.
 * Setting this to "0" disables the minmssoverload check.
 */
#define	TCP_MINMSSOVERLOAD 1000

/*
 * Default maximum segment size for TCP6.
 * With an IP6 MSS of 1280, this is 1220,
 * but 1024 is probably more convenient. (xxx kazu in doubt)
 * This should be defined as MIN(1024, IP6_MSS - sizeof (struct tcpip6hdr))
 */
#define	TCP6_MSS	1024

#define	TCP_MAXWIN	65535	/* largest value for (unscaled) window */
#define	TTCP_CLIENT_SND_WND	4096	/* dflt send window for T/TCP client */

#define TCP_MAX_WINSHIFT	14	/* maximum window shift */

#define TCP_MAXHLEN	(0xf<<2)	/* max length of header in bytes */
#define TCP_MAXOLEN	(TCP_MAXHLEN - sizeof(struct tcphdr))
					/* max space left for options */
#endif /* (_POSIX_C_SOURCE && !_DARWIN_C_SOURCE) */

/*
 * User-settable options (used with setsockopt).
 */
#define	TCP_NODELAY             0x01    /* don't delay send to coalesce packets */
#if !defined(_POSIX_C_SOURCE) || defined(_DARWIN_C_SOURCE)
#define	TCP_MAXSEG              0x02    /* set maximum segment size */
#define TCP_NOPUSH              0x04    /* don't push last block of write */
#define TCP_NOOPT               0x08    /* don't use TCP options */
#define TCP_KEEPALIVE           0x10    /* idle time used when SO_KEEPALIVE is enabled */
#define TCP_CONNECTIONTIMEOUT   0x20    /* connection timeout */
#define PERSIST_TIMEOUT		0x40	/* time after which a connection in
					 *  persist timeout will terminate.
					 *  see draft-ananth-tcpm-persist-02.txt
					 */
#define TCP_RXT_CONNDROPTIME	0x80	/* time after which tcp retransmissions will be 
					 * stopped and the connection will be dropped
					 */
#define TCP_RXT_FINDROP	0x100	/* when this option is set, drop a connection 
					 * after retransmitting the FIN 3 times. It will
					 * prevent holding too many mbufs in socket 
					 * buffer queues.
					 */
#ifdef PRIVATE
#define	TCP_INFO				0x200	/* retrieve tcp_info structure */

/*
 * The TCP_INFO socket option comes from the Linux 2.6 TCP API, and permits
 * the caller to query certain information about the state of a TCP
 * connection.  We provide an overlapping set of fields with the Linux
 * implementation, but since this is a fixed size structure, room has been
 * left for growth.  In order to maximize potential future compatibility with
 * the Linux API, the same variable names and order have been adopted, and
 * padding left to make room for omitted fields in case they are added later.
 *
 * XXX: This is currently an unstable ABI/API, in that it is expected to
 * change.
 */
#pragma pack(4)

#define	TCPI_OPT_TIMESTAMPS	0x01
#define	TCPI_OPT_SACK		0x02
#define	TCPI_OPT_WSCALE		0x04
#define	TCPI_OPT_ECN		0x08

struct tcp_info {
	u_int8_t	tcpi_state;			/* TCP FSM state. */
	u_int8_t	tcpi_options;		/* Options enabled on conn. */
	u_int8_t	tcpi_snd_wscale;	/* RFC1323 send shift value. */
	u_int8_t	tcpi_rcv_wscale;	/* RFC1323 recv shift value. */

	u_int32_t	tcpi_snd_mss;		/* Max segment size for send. */
	u_int32_t	tcpi_rcv_mss;		/* Max segment size for receive. */

	u_int32_t	tcpi_snd_ssthresh;	/* Slow start threshold. */
	u_int32_t	tcpi_snd_cwnd;		/* Send congestion window. */

	u_int32_t	tcpi_rcv_space;		/* Advertised recv window. */

	u_int32_t	tcpi_snd_wnd;		/* Advertised send window. */
	u_int32_t	tcpi_snd_bwnd;		/* Bandwidth send window. */
	u_int32_t	tcpi_snd_nxt;		/* Next egress seqno */
	u_int32_t	tcpi_rcv_nxt;		/* Next ingress seqno */
	
	int32_t		tcpi_last_outif;	/* if_index of interface used to send last */
};

/*
 * Note that IPv6 link local addresses should have the appropriate scope ID
 */

struct info_tuple {
	u_int8_t	itpl_proto;
	union {
		struct sockaddr		_itpl_sa;
		struct sockaddr_in	_itpl_sin;
		struct sockaddr_in6	_itpl_sin6;
	} itpl_localaddr;
	union {
		struct sockaddr		_itpl_sa;
		struct sockaddr_in	_itpl_sin;
		struct sockaddr_in6	_itpl_sin6;
	} itpl_remoteaddr;
};

#define itpl_local_sa		itpl_localaddr._itpl_sa
#define itpl_local_sin 		itpl_localaddr._itpl_sin
#define itpl_local_sin6		itpl_localaddr._itpl_sin6
#define itpl_remote_sa 		itpl_remoteaddr._itpl_sa
#define itpl_remote_sin		itpl_remoteaddr._itpl_sin
#define itpl_remote_sin6	itpl_remoteaddr._itpl_sin6

#pragma pack()

#endif /* PRIVATE */
#endif /* (_POSIX_C_SOURCE && !_DARWIN_C_SOURCE) */

#endif

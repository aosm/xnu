/*
 * Copyright (c) 2000-2009 Apple Inc. All rights reserved.
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
 * Copyright (c) 1988, 1992, 1993
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
 *	@(#)in_cksum.c	8.1 (Berkeley) 6/10/93
 */

#include <sys/param.h>
#include <sys/mbuf.h>
#include <sys/kdebug.h>
#include <kern/debug.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <libkern/libkern.h>

#define DBG_FNC_IN_CKSUM	NETDBG_CODE(DBG_NETIP, (3 << 8))

/*
 * Checksum routine for Internet Protocol family headers (Portable Version).
 *
 * This routine is very heavily used in the network
 * code and should be modified for each CPU to be as fast as possible.
 */

union s_util {
        char    c[2];
        u_short s;
};

union l_util {
        u_int16_t s[2];
        u_int32_t l;
};

union q_util {
        u_int16_t s[4];
        u_int32_t l[2];
        u_int64_t q;
};

#define ADDCARRY(x)  do { if (x > 65535) { x -= 65535; } } while (0)

#define REDUCE32                                                          \
    {                                                                     \
        q_util.q = sum;                                                   \
        sum = q_util.s[0] + q_util.s[1] + q_util.s[2] + q_util.s[3];      \
    }
#define REDUCE16                                                          \
    {                                                                     \
        q_util.q = sum;                                                   \
        l_util.l = q_util.s[0] + q_util.s[1] + q_util.s[2] + q_util.s[3]; \
        sum = l_util.s[0] + l_util.s[1];                                  \
        ADDCARRY(sum);                                                    \
    }

#define REDUCE {l_util.l = sum; sum = l_util.s[0] + l_util.s[1]; ADDCARRY(sum);}

u_int16_t inet_cksum_simple(struct mbuf *, int);

u_int16_t
inet_cksum_simple(struct mbuf *m, int len)
{
	return (inet_cksum(m, 0, 0, len));
}

u_short
in_addword(u_short a, u_short b)
{
        union l_util l_util;
	u_int32_t sum = a + b;

	REDUCE;
	return (sum);
}

u_short
in_pseudo(u_int a, u_int b, u_int c)
{
        u_int64_t sum;
        union q_util q_util;
        union l_util l_util;

        sum = (u_int64_t) a + b + c;
        REDUCE16;
        return (sum);

}


u_int16_t
inet_cksum(struct mbuf *m, unsigned int nxt, unsigned int skip,
    unsigned int len)
{
	u_short *w;
	u_int32_t sum = 0;
	int mlen = 0;
	int byte_swapped = 0;
	union s_util s_util;
	union l_util l_util;

	KERNEL_DEBUG(DBG_FNC_IN_CKSUM | DBG_FUNC_START, len,0,0,0,0);

	/* sanity check */
	if ((m->m_flags & M_PKTHDR) && m->m_pkthdr.len < skip + len) {
		panic("inet_cksum: mbuf len (%d) < off+len (%d+%d)\n",
		    m->m_pkthdr.len, skip, len);
	}

	/* include pseudo header checksum? */
	if (nxt != 0) {
		struct ip *iph;

		if (m->m_len < sizeof (struct ip))
			panic("inet_cksum: bad mbuf chain");

		iph = mtod(m, struct ip *);
		sum = in_pseudo(iph->ip_src.s_addr, iph->ip_dst.s_addr,
		    htonl(len + nxt));
	}

	if (skip != 0) {
		for (; skip && m; m = m->m_next) {
			if (m->m_len > skip) {
				mlen = m->m_len - skip;
				w = (u_short *)(m->m_data+skip);
				goto skip_start;
			} else {
				skip -= m->m_len;
			}
		}
	}
	for (;m && len; m = m->m_next) {
		if (m->m_len == 0)
			continue;
		w = mtod(m, u_short *);

		if (mlen == -1) {
			/*
			 * The first byte of this mbuf is the continuation
			 * of a word spanning between this mbuf and the
			 * last mbuf.
			 *
			 * s_util.c[0] is already saved when scanning previous
			 * mbuf.
			 */
			s_util.c[1] = *(char *)w;
			sum += s_util.s;
			w = (u_short *)((char *)w + 1);
			mlen = m->m_len - 1;
			len--;
		} else {
			mlen = m->m_len;
		}
skip_start:
		if (len < mlen)
			mlen = len;

		len -= mlen;
		/*
		 * Force to even boundary.
		 */
		if ((1 & (uintptr_t) w) && (mlen > 0)) {
			REDUCE;
			sum <<= 8;
			s_util.c[0] = *(u_char *)w;
			w = (u_short *)((char *)w + 1);
			mlen--;
			byte_swapped = 1;
		}
		/*
		 * Unroll the loop to make overhead from
		 * branches &c small.
		 */
		while ((mlen -= 32) >= 0) {
			sum += w[0]; sum += w[1]; sum += w[2]; sum += w[3];
			sum += w[4]; sum += w[5]; sum += w[6]; sum += w[7];
			sum += w[8]; sum += w[9]; sum += w[10]; sum += w[11];
			sum += w[12]; sum += w[13]; sum += w[14]; sum += w[15];
			w += 16;
		}
		mlen += 32;
		while ((mlen -= 8) >= 0) {
			sum += w[0]; sum += w[1]; sum += w[2]; sum += w[3];
			w += 4;
		}
		mlen += 8;
		if (mlen == 0 && byte_swapped == 0)
			continue;
		REDUCE;
		while ((mlen -= 2) >= 0) {
			sum += *w++;
		}
		if (byte_swapped) {
			REDUCE;
			sum <<= 8;
			byte_swapped = 0;
			if (mlen == -1) {
				s_util.c[1] = *(char *)w;
				sum += s_util.s;
				mlen = 0;
			} else
				mlen = -1;
		} else if (mlen == -1)
			s_util.c[0] = *(char *)w;
	}
	if (len)
		printf("cksum: out of data by %d\n", len);
	if (mlen == -1) {
		/* The last mbuf has odd # of bytes. Follow the
		   standard (the odd byte may be shifted left by 8 bits
		   or not as determined by endian-ness of the machine) */
		s_util.c[1] = 0;
		sum += s_util.s;
	}
	REDUCE;
	KERNEL_DEBUG(DBG_FNC_IN_CKSUM | DBG_FUNC_END, 0,0,0,0,0);
	return (~sum & 0xffff);
}


/*
 * Copyright (c) 2000 Apple Computer, Inc. All rights reserved.
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
 *	Copyright (c) 1988, 1989 Apple Computer, Inc. 
 */

#ifndef _NETAT_RTMP_H_
#define _NETAT_RTMP_H_
#include <sys/appleapiopts.h>

#ifdef __APPLE_API_OBSOLETE

/* Changed 03-22-94 for router support  LD */

/* RTMP function codes */
#define RTMP_REQ_FUNC1		0x01	/* RTMP request function code=1 */
#define RTMP_REQ_FUNC2		0x02	/* Route Data Req with Split Horizon */
#define RTMP_REQ_FUNC3		0x03	/* Route Data Req no Split Horizon */


#define RTMP_ROUTER_AGE		50	/* Number of seconds to age router */

/* RTMP response and data packet format */

typedef struct {
        at_net  	at_rtmp_this_net;
        u_char      	at_rtmp_id_length;
        u_char      	at_rtmp_id[1];
} at_rtmp;

/* RTMP network/distance data tuples */

#define RTMP_TUPLE_SIZE		3

/* Extended AppleTalk tuple can be thought of as two of
 * these tuples back to back.
 */

#define RTMP_RANGE_FLAG 0x80
#define RTMP_DISTANCE   0x0f

typedef struct {
	at_net		at_rtmp_net;
	unsigned char	at_rtmp_data;
} at_rtmp_tuple;

#ifdef KERNEL_PRIVATE

void rtmp_purge(at_ifaddr_t *);
void rtmp_shutdown(void);
void rtmp_input (gbuf_t *, at_ifaddr_t *);
void RouterError(short, short);
void rtmp_init(void);
int zip_control (at_ifaddr_t *, int);
void routershutdown(void);
void trackrouter_rem_if(at_ifaddr_t *);
char	upshift8(char);
void ZIPwakeup(at_ifaddr_t *, int);

int elap_dataput(gbuf_t *, at_ifaddr_t *, u_char, char *);

#endif /* KERNEL_PRIVATE */

#endif /* __APPLE_API_OBSOLETE */
#endif /* _NETAT_RTMP_H_ */

/*
 * Copyright (c) 2004-2007 Apple Inc. All rights reserved.
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
 * @OSF_COPYRIGHT@
 */
/*
 * @APPLE_FREE_COPYRIGHT@
 */
/*
 *	File:		etimer.h
 *	Purpose:	Routines for handling the machine independent
 *				real-time clock.
 */

#ifndef _ETIMER_H_
#define _ETIMER_H_

#define EndOfAllTime	0xFFFFFFFFFFFFFFFFULL

/* extern void rtclock_intr(int inuser, uint64_t iaddr);  - this is currently MD */
typedef	void (*etimer_intr_t)(int, uint64_t);

extern int setTimerReq(void);
extern void etimer_intr(int inuser, uint64_t iaddr);

extern void etimer_set_deadline(uint64_t deadline);
extern int setPop(uint64_t time);

extern void etimer_resync_deadlines(void);

extern uint32_t rtclock_tick_interval;

#if 0 /* this is currently still MD */
#pragma pack(push,4)
struct rtclock_timer_t  {
	uint64_t		deadline;
	uint32_t
	/*boolean_t*/	is_set:1,
					has_expired:1,
					:0;
};
#pragma pack(pop)
typedef struct rtclock_timer_t rtclock_timer_t;
#endif /* MD */


#endif /* _ETIMER_H_ */

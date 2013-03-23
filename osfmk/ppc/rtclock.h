/*
 * Copyright (c) 2004-2005 Apple Computer, Inc. All rights reserved.
 *
 * @APPLE_LICENSE_HEADER_START@
 * 
 * The contents of this file constitute Original Code as defined in and
 * are subject to the Apple Public Source License Version 1.1 (the
 * "License").  You may not use this file except in compliance with the
 * License.  Please obtain a copy of the License at
 * http://www.apple.com/publicsource and read it before using this file.
 * 
 * This Original Code and all software distributed under the License are
 * distributed on an "AS IS" basis, WITHOUT WARRANTY OF ANY KIND, EITHER
 * EXPRESS OR IMPLIED, AND APPLE HEREBY DISCLAIMS ALL SUCH WARRANTIES,
 * INCLUDING WITHOUT LIMITATION, ANY WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE OR NON-INFRINGEMENT.  Please see the
 * License for the specific language governing rights and limitations
 * under the License.
 * 
 * @APPLE_LICENSE_HEADER_END@
 */
/*
 * @OSF_COPYRIGHT@
 */
/*
 * @APPLE_FREE_COPYRIGHT@
 */
/*
 *	File:		rtclock.h
 *	Purpose:	Routines for handling the machine dependent
 *				real-time clock.
 */

#ifndef _PPC_RTCLOCK_H_
#define _PPC_RTCLOCK_H_

#define EndOfAllTime	0xFFFFFFFFFFFFFFFFULL

extern void rtclock_intr(struct savearea *ssp);
extern int setTimerReq(void);

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

#endif /* _PPC_RTCLOCK_H_ */

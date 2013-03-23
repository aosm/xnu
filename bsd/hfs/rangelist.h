/*
 * Copyright (c) 2001 Apple Computer, Inc. All rights reserved.
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

#include <sys/types.h>
#include <sys/queue.h>

enum rl_overlaptype {
    RL_NOOVERLAP = 0,		/* 0 */
    RL_MATCHINGOVERLAP,		/* 1 */
    RL_OVERLAPCONTAINSRANGE,	/* 2 */
    RL_OVERLAPISCONTAINED,	/* 3 */
    RL_OVERLAPSTARTSBEFORE,	/* 4 */
    RL_OVERLAPENDSAFTER		/* 5 */
};

#define RL_INFINITY ((off_t)-1)

CIRCLEQ_HEAD(rl_head, rl_entry);

struct rl_entry {
    CIRCLEQ_ENTRY(rl_entry) rl_link;
    off_t rl_start;
    off_t rl_end;
};

__BEGIN_DECLS
void rl_init(struct rl_head *rangelist);
void rl_add(off_t start, off_t end, struct rl_head *rangelist);
void rl_remove(off_t start, off_t end, struct rl_head *rangelist);
enum rl_overlaptype rl_scan(struct rl_head *rangelist,
							off_t start,
							off_t end,
							struct rl_entry **overlap);
__END_DECLS

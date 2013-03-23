/*
 * Copyright (c) 2000 Apple Computer, Inc. All rights reserved.
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
 * 
 */

#ifndef _MACHINE_RPC_H_
#define _MACHINE_RPC_H_

#if     ETAP_EVENT_MONITOR
#define ETAP_EXCEPTION_PROBE(_f, _th, _ex, _sysnum)             \
        if (_ex == EXC_SYSCALL) {                               \
                ETAP_PROBE_DATA(ETAP_P_SYSCALL_UNIX,            \
                                _f,                             \
                                _th,                            \
                                _sysnum,                        \
                                sizeof(int));                   \
        }
#else   /* ETAP_EVENT_MONITOR */
#define ETAP_EXCEPTION_PROBE(_f, _th, _ex, _sysnum)
#endif  /* ETAP_EVENT_MONITOR */

#endif /* _MACHINE_RPC_H_ */



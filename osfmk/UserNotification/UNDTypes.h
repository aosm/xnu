/*
 * Copyright (c) 2000-2005 Apple Computer, Inc. All rights reserved.
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

#ifndef __USERNOTIFICATION_UNDTYPES_H
#define __USERNOTIFICATION_UNDTYPES_H

#include <sys/appleapiopts.h>

#ifdef __APPLE_API_PRIVATE

#include <mach/mach_types.h>

typedef char *UNDMessage;
typedef char *UNDLabel;
typedef char *UNDKey;
typedef char *UNDPath;

/*
 * serialized key's, list delimiters, ...
 *	(sent as out-of-line data in a message)
 */
typedef const char * xmlData_t;

#ifdef KERNEL_PRIVATE
#ifdef MACH_KERNEL_PRIVATE

/*
 * UNDReply definition - used to dispatch UserNotification
 * replies back to the in-kernel client.
 */
typedef struct UNDReply *UNDReplyRef;

#include <sys/cdefs.h>
__BEGIN_DECLS
extern UNDReplyRef convert_port_to_UNDReply(mach_port_t);
__END_DECLS

#else /* !MACH_KERNEL_PRIVATE */

typedef struct __UNDReply__ *UNDReplyRef;

#endif /* !MACH_KERNEL_PRIVATE */

#else /* ! KERNEL_PRIVATE */

typedef mach_port_t UNDReplyRef;

#endif /* ! KERNEL_PRIVATE */

#define UND_REPLY_NULL ((UNDReplyRef)0)
#define XML_DATA_NULL	((xmlData_t)0)

#endif  /* __APPLE_API_PRIVATE */

#endif	/* __USERNOTIFICATION_UNDTPES_H */


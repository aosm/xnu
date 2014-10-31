/*
 * Copyright (c) 2008 Apple Inc. All rights reserved.
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

#ifndef	_FSGETPATH_H_
#define _FSGETPATH_H_

#include <sys/types.h>
#include <sys/mount.h>

#ifdef __APPLE_API_PRIVATE

#ifndef KERNEL
__BEGIN_DECLS

#include <sys/syscall.h>
#include <unistd.h>

/*
 * Obtain the full pathname of a file system object by id.
 *
 * This is a private SPI used by the File Manager.
 *
 * ssize_t fsgetpath_np(char *restrict buf, size_t bufsize, fsid_t fsid, uint64_t objid);
 */
#define	fsgetpath(buf, bufsize, fsid, objid)  \
	(ssize_t)syscall(SYS_fsgetpath, buf, (size_t)bufsize, fsid, (uint64_t)objid)

/*
 * openbyid_np: open a file given a file system id and a file system object id 
 * 
 * fsid :	value corresponding to getattlist ATTR_CMN_FSID attribute, or
 *			value of stat's st.st_dev ; set fsid = {st.st_dev, 0} 
 *
 * objid: value (link id/node id) corresponding to getattlist ATTR_CMN_OBJID 
 *		  attribute , or
 *		  value of stat's st.st_ino (node id); set objid =  st.st_ino
 *
 * For hfs the value of getattlist ATTR_CMN_FSID is a link id which uniquely identifies a
 * parent in the case of hard linked files; this allows unique path access validation.
 * Not all file systems support getattrlist ATTR_CMN_OBJID (link id).
 * A node id does not uniquely identify a parent in the case of hard linked files and may 
 * resolve to a path for which access validation can fail.
 */
int openbyid_np(fsid_t* fsid, fsobj_id_t* objid, int flags);

__END_DECLS
#endif /* KERNEL */

#endif /* __APPLE_API_PRIVATE */

#endif /* !_FSGETPATH_H_ */

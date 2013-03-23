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
 */

#if 0
extern boolean_t	iopb_check_mapping(
					thread_t	thread,
					device_t	device);
extern kern_return_t	i386_io_port_add(
					thread_t	thread,
					device_t	device);
extern kern_return_t	i386_io_port_remove(
					thread_t	thread,
					device_t	device);
extern kern_return_t	i386_io_port_list(
					thread_t	thread,
					device_t	** list,
					unsigned int	* list_count);
extern void		iopb_init(void);
extern iopb_tss_t	iopb_create(void);
extern void		iopb_destroy(
					iopb_tss_t	iopb);
#endif

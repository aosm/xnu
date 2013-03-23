/*
 * Copyright (c) 2003-2004 Apple Computer, Inc. All rights reserved.
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

#define __KPI__
#include <sys/kernel.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/socketvar.h>
#include <sys/param.h>
#include <sys/proc.h>
#include <sys/errno.h>
#include <sys/malloc.h>
#include <sys/protosw.h>
#include <sys/domain.h>
#include <sys/mbuf.h>
#include <sys/fcntl.h>
#include <sys/filio.h>
#include <sys/uio_internal.h>
#include <kern/lock.h>

extern void	*memcpy(void *, const void *, size_t);
extern int soclose_locked(struct socket *so);

errno_t sock_send_internal(
	socket_t			sock,
	const struct msghdr	*msg,
	mbuf_t				data,
	int					flags,
	size_t				*sentlen);



errno_t
sock_accept(
	socket_t		sock,
	struct sockaddr	*from,
	int				fromlen,
	int				flags,
	sock_upcall		callback,
	void*			cookie,
	socket_t		*new_sock)
{
	struct sockaddr *sa;
	struct socket *new_so;
	lck_mtx_t *mutex_held;
	int 	dosocklock;
	errno_t	error = 0;
	
	if (sock == NULL || new_sock == NULL) return EINVAL;
	socket_lock(sock, 1);
	if ((sock->so_options & SO_ACCEPTCONN) == 0) {
		socket_unlock(sock, 1);
	       	return EINVAL;
	}
	if ((flags & ~(MSG_DONTWAIT)) != 0) {
		socket_unlock(sock, 1);
		return ENOTSUP;
	}
	if (((flags & MSG_DONTWAIT) != 0 || (sock->so_state & SS_NBIO) != 0) &&
		sock->so_comp.tqh_first == NULL) {
		socket_unlock(sock, 1);
		return EWOULDBLOCK;
	}

	if (sock->so_proto->pr_getlock != NULL)  {
		mutex_held = (*sock->so_proto->pr_getlock)(sock, 0);
		dosocklock = 1;
	}
	else {
		mutex_held = sock->so_proto->pr_domain->dom_mtx;
		dosocklock = 0;
	}
	
	while (TAILQ_EMPTY(&sock->so_comp) && sock->so_error == 0) {
		if (sock->so_state & SS_CANTRCVMORE) {
			sock->so_error = ECONNABORTED;
			break;
		}
		error = msleep((caddr_t)&sock->so_timeo, mutex_held, PSOCK | PCATCH, "sock_accept", 0);
		if (error) {
			socket_unlock(sock, 1);
			return (error);
		}
	}
	if (sock->so_error) {
		error = sock->so_error;
		sock->so_error = 0;
		socket_unlock(sock, 1);
		return (error);
	}
	
	new_so = TAILQ_FIRST(&sock->so_comp);
	TAILQ_REMOVE(&sock->so_comp, new_so, so_list);
	sock->so_qlen--;
	socket_unlock(sock, 1);	/* release the head */

	if (dosocklock)	{
		lck_mtx_assert(new_so->so_proto->pr_getlock(new_so, 0),
					   LCK_MTX_ASSERT_NOTOWNED);
		socket_lock(new_so, 1);
	}
	
	new_so->so_state &= ~SS_COMP;
	new_so->so_head = NULL;
	soacceptlock(new_so, &sa, 0);
	
	if (callback) {
		new_so->so_upcall = callback;
		new_so->so_upcallarg = cookie;
		new_so->so_rcv.sb_flags |= SB_UPCALL;
	}
	
	if (sa && from)
	{
		if (fromlen > sa->sa_len) fromlen = sa->sa_len;
		memcpy(from, sa, fromlen);
	}
	if (sa) FREE(sa, M_SONAME);
	*new_sock = new_so;
	if (dosocklock)	
		socket_unlock(new_so, 1);
	return error;
}

errno_t
sock_bind(
	socket_t				sock,
	const struct sockaddr	*to)
{
	if (sock == NULL || to == NULL) return EINVAL;
	
	return sobind(sock, (struct sockaddr*)to);
}

errno_t
sock_connect(
	socket_t				sock,
	const struct sockaddr	*to,
	int						flags)
{
	int	error = 0;
	lck_mtx_t *mutex_held;
	
	if (sock == NULL || to == NULL) return EINVAL;

	socket_lock(sock, 1);

	if ((sock->so_state & SS_ISCONNECTING) &&
		((sock->so_state & SS_NBIO) != 0 ||
		 (flags & MSG_DONTWAIT) != 0)) {
		socket_unlock(sock, 1);
		return EALREADY;
	}
	error = soconnectlock(sock, (struct sockaddr*)to, 0);
	if (!error) {
		if ((sock->so_state & SS_ISCONNECTING) &&
			((sock->so_state & SS_NBIO) != 0 || (flags & MSG_DONTWAIT) != 0)) {
			socket_unlock(sock, 1);
			return EINPROGRESS;
		}
		
		if (sock->so_proto->pr_getlock != NULL)  
			mutex_held = (*sock->so_proto->pr_getlock)(sock, 0);
		else 
               		mutex_held = sock->so_proto->pr_domain->dom_mtx;

		while ((sock->so_state & SS_ISCONNECTING) && sock->so_error == 0) {
			error = msleep((caddr_t)&sock->so_timeo, mutex_held, PSOCK | PCATCH,
				"sock_connect", 0);
			if (error)
				break;
		}
		
		if (error == 0) {
			error = sock->so_error;
			sock->so_error = 0;
		}
	}
	else {
		sock->so_state &= ~SS_ISCONNECTING;
	}
	socket_unlock(sock, 1);
	return error;
}

errno_t
sock_connectwait(
	socket_t				sock,
	const struct timeval	*tv)
{
	lck_mtx_t * mutex_held;
	errno_t	retval = 0;
	struct timespec ts;
	
	socket_lock(sock, 1);
	
	// Check if we're already connected or if we've already errored out
	if ((sock->so_state & SS_ISCONNECTING) == 0 || sock->so_error) {
		if (sock->so_error) {
			retval = sock->so_error;
			sock->so_error = 0;
		}
		else {
			if ((sock->so_state & SS_ISCONNECTED) != 0)
				retval = 0;
			else
				retval = EINVAL;
		}
		goto done;
	}
	
	// copied translation from timeval to hertz from SO_RCVTIMEO handling
	if (tv->tv_sec < 0 || tv->tv_sec > SHRT_MAX / hz ||
		tv->tv_usec < 0 || tv->tv_usec >= 1000000) {
		retval = EDOM;
		goto done;
	}
	
	ts.tv_sec = tv->tv_sec;
	ts.tv_nsec = (tv->tv_usec * NSEC_PER_USEC);
	if ( (ts.tv_sec + (ts.tv_nsec/NSEC_PER_SEC))/100  >  SHRT_MAX)  {
		retval = EDOM;
		goto done;
	}
	
	if (sock->so_proto->pr_getlock != NULL)  
		mutex_held = (*sock->so_proto->pr_getlock)(sock, 0);
	else 
          	mutex_held = sock->so_proto->pr_domain->dom_mtx;

	msleep((caddr_t)&sock->so_timeo, mutex_held, PSOCK, "sock_connectwait", &ts);
	
	// Check if we're still waiting to connect
	if ((sock->so_state & SS_ISCONNECTING) && sock->so_error == 0) {
		retval = EINPROGRESS;
		goto done;
	}
	
	if (sock->so_error) {
		retval = sock->so_error;
		sock->so_error = 0;
	}
	
done:
	socket_unlock(sock, 1);
	return retval;
}

errno_t
sock_nointerrupt(
	socket_t	sock,
	int			on)
{
	socket_lock(sock, 1);

	if (on) {
		sock->so_rcv.sb_flags |= SB_NOINTR; // This isn't safe
		sock->so_snd.sb_flags |= SB_NOINTR; // This isn't safe
	}
	else {
		sock->so_rcv.sb_flags &= ~SB_NOINTR; // This isn't safe
		sock->so_snd.sb_flags &= ~SB_NOINTR; // This isn't safe
	}

	socket_unlock(sock, 1);

	return 0;
}

errno_t
sock_getpeername(
	socket_t		sock,
	struct sockaddr	*peername,
	int				peernamelen)
{
	int				error = 0;
	struct sockaddr	*sa = NULL;
	
	if (sock == NULL || peername == NULL || peernamelen < 0) return EINVAL;
	socket_lock(sock, 1);
	if ((sock->so_state & (SS_ISCONNECTED|SS_ISCONFIRMING)) == 0) {
		socket_unlock(sock, 1);
		return ENOTCONN;
	}
	error = sock->so_proto->pr_usrreqs->pru_peeraddr(sock, &sa);
	if (!error)
	{
		if (peernamelen > sa->sa_len) peernamelen = sa->sa_len;
		memcpy(peername, sa, peernamelen);
	}
	if (sa) FREE(sa, M_SONAME);
	socket_unlock(sock, 1);
	return error;
}

errno_t
sock_getsockname(
	socket_t		sock,
	struct sockaddr	*sockname,
	int				socknamelen)
{
	int				error = 0;
	struct sockaddr	*sa = NULL;
	
	if (sock == NULL || sockname == NULL || socknamelen < 0) return EINVAL;
	socket_lock(sock, 1);
	error = sock->so_proto->pr_usrreqs->pru_sockaddr(sock, &sa);
	if (!error)
	{
		if (socknamelen > sa->sa_len) socknamelen = sa->sa_len;
		memcpy(sockname, sa, socknamelen);
	}
	if (sa) FREE(sa, M_SONAME);
	socket_unlock(sock, 1);
	return error;
}

errno_t
sock_getsockopt(
	socket_t	sock,
	int			level,
	int			optname,
	void		*optval,
	int			*optlen)
{
	int				error = 0;
	struct sockopt	sopt;
	
	if (sock == NULL || optval == NULL || optlen == NULL) return EINVAL;
	sopt.sopt_dir = SOPT_GET;
	sopt.sopt_level = level;
	sopt.sopt_name = optname;
	sopt.sopt_val = CAST_USER_ADDR_T(optval); 
	sopt.sopt_valsize = *optlen;
	sopt.sopt_p = NULL;
	error = sogetopt(sock, &sopt); /* will lock socket */
	if (error == 0) *optlen = sopt.sopt_valsize;
	return error;
}

errno_t
sock_ioctl(
	socket_t	sock,
	unsigned long request,
	void *argp)
{
	return soioctl(sock, request, argp, NULL); /* will lock socket */
}

errno_t
sock_setsockopt(
	socket_t	sock,
	int			level,
	int			optname,
	const void	*optval,
	int			optlen)
{
	struct sockopt	sopt;
	
	if (sock == NULL || optval == NULL) return EINVAL;
	sopt.sopt_dir = SOPT_SET;
	sopt.sopt_level = level;
	sopt.sopt_name = optname;
	sopt.sopt_val = CAST_USER_ADDR_T(optval);
	sopt.sopt_valsize = optlen;
	sopt.sopt_p = NULL;
	return sosetopt(sock, &sopt); /* will lock socket */
}

errno_t
sock_listen(
	socket_t	sock,
	int			backlog)
{
	if (sock == NULL) return EINVAL;
	return solisten(sock, backlog); /* will lock socket */
}

static errno_t
sock_receive_internal(
	socket_t		sock,
	struct msghdr		*msg,
	mbuf_t			*data,
	int			flags,
	size_t			*recvdlen)
{
	uio_t 		auio;
	struct mbuf	*control = NULL;
	int			error = 0;
	int			length = 0;
	struct sockaddr	*fromsa;
	char		uio_buf[ UIO_SIZEOF((msg != NULL) ? msg->msg_iovlen : 0) ];
	
	if (sock == NULL) return EINVAL;
	
	auio = uio_createwithbuffer(((msg != NULL) ? msg->msg_iovlen : 0), 
								  0, UIO_SYSSPACE, UIO_READ, 
								  &uio_buf[0], sizeof(uio_buf));
	if (msg && data == NULL) {
		int i;
		struct iovec_32 *tempp = (struct iovec_32 *) msg->msg_iov;
		
		for (i = 0; i < msg->msg_iovlen; i++) {
			uio_addiov(auio, CAST_USER_ADDR_T((tempp + i)->iov_base), (tempp + i)->iov_len);
		}
		if (uio_resid(auio) < 0) return EINVAL;
	}
	else {
		uio_setresid(auio, (uio_resid(auio) + *recvdlen));
	}
	length = uio_resid(auio);
	
	if (recvdlen)
		*recvdlen = 0;
	
	if (msg && msg->msg_control) {
		if ((size_t)msg->msg_controllen < sizeof(struct cmsghdr)) return EINVAL;
		if ((size_t)msg->msg_controllen > MLEN) return EINVAL;
		control = m_get(M_NOWAIT, MT_CONTROL);
		if (control == NULL) return ENOMEM;
		memcpy(mtod(control, caddr_t), msg->msg_control, msg->msg_controllen);
		control->m_len = msg->msg_controllen;
	}

	/* let pru_soreceive handle the socket locking */	
	error = sock->so_proto->pr_usrreqs->pru_soreceive(sock, &fromsa, auio,
				data, control ? &control : NULL, &flags);
	if (error) goto cleanup;
	
	if (recvdlen)
		*recvdlen = length - uio_resid(auio);
	if (msg) {
		msg->msg_flags = flags;
		
		if (msg->msg_name)
		{
			int salen;
			salen = msg->msg_namelen;
			if (msg->msg_namelen > 0 && fromsa != 0)
			{
				salen = MIN(salen, fromsa->sa_len);
				memcpy(msg->msg_name, fromsa,
					msg->msg_namelen > fromsa->sa_len ? fromsa->sa_len :  msg->msg_namelen);
			}
		}
		
		if (msg->msg_control)
		{
			struct mbuf*	m = control;
			u_char*			ctlbuf = msg->msg_control;
			int				clen = msg->msg_controllen;
			msg->msg_controllen = 0;
			
			while (m && clen > 0)
			{
				unsigned int tocopy;
				if (clen >= m->m_len)
				{
					tocopy = m->m_len;
				}
				else
				{
					msg->msg_flags |= MSG_CTRUNC;
					tocopy = clen;
				}
				memcpy(ctlbuf, mtod(m, caddr_t), tocopy);
				ctlbuf += tocopy;
				clen -= tocopy;
				m = m->m_next;
			}
			msg->msg_controllen = (u_int32_t)ctlbuf - (u_int32_t)msg->msg_control;
		}
	}

cleanup:
	if (control) m_freem(control);
	if (fromsa) FREE(fromsa, M_SONAME);
	return error;
}

errno_t
sock_receive(
	socket_t		sock,
	struct msghdr	*msg,
	int				flags,
	size_t			*recvdlen)
{
	if ((msg == NULL) ||
		(msg->msg_iovlen < 1) ||
		(msg->msg_iov[0].iov_len == 0) ||
		(msg->msg_iov[0].iov_base == NULL))
		return EINVAL;
	return sock_receive_internal(sock, msg, NULL, flags, recvdlen);
}

errno_t
sock_receivembuf(
	socket_t		sock,
	struct msghdr	*msg,
	mbuf_t			*data,
	int				flags,
	size_t			*recvlen)
{
	if (data == NULL || recvlen == 0 || *recvlen <= 0 || (msg &&
		(msg->msg_iov != NULL || msg->msg_iovlen != 0)))
		return EINVAL;
	return sock_receive_internal(sock, msg, data, flags, recvlen);
}

errno_t
sock_send_internal(
	socket_t			sock,
	const struct msghdr	*msg,
	mbuf_t				data,
	int					flags,
	size_t				*sentlen)
{
	uio_t			auio = NULL;
	struct mbuf		*control = NULL;
	int				error = 0;
	int				datalen = 0;
	char			uio_buf[ UIO_SIZEOF((msg != NULL ? msg->msg_iovlen : 1)) ];
	
	if (sock == NULL) {
		error = EINVAL;
		goto errorout;
	}
	
	if (data == 0 && msg != NULL) {
		struct iovec_32 *tempp = (struct iovec_32 *) msg->msg_iov;

		auio = uio_createwithbuffer(msg->msg_iovlen, 0, UIO_SYSSPACE, UIO_WRITE, 
								  &uio_buf[0], sizeof(uio_buf));
		if (tempp != NULL)
		{
			int i;
			
			for (i = 0; i < msg->msg_iovlen; i++) {
				uio_addiov(auio, CAST_USER_ADDR_T((tempp + i)->iov_base), (tempp + i)->iov_len);
			}
			
			if (uio_resid(auio) < 0) {
				error = EINVAL;
				goto errorout;
			}
		}
	}
	
	if (sentlen)
		*sentlen = 0;
	
	if (auio)
		datalen = uio_resid(auio);
	else
		datalen = data->m_pkthdr.len;
	
	if (msg && msg->msg_control)
	{
		if ((size_t)msg->msg_controllen < sizeof(struct cmsghdr)) return EINVAL;
		if ((size_t)msg->msg_controllen > MLEN) return EINVAL;
		control = m_get(M_NOWAIT, MT_CONTROL);
		if (control == NULL) {
			error = ENOMEM;
			goto errorout;
		}
		memcpy(mtod(control, caddr_t), msg->msg_control, msg->msg_controllen);
		control->m_len = msg->msg_controllen;
	}
	
	error = sock->so_proto->pr_usrreqs->pru_sosend(sock, msg != NULL ?
	    (struct sockaddr*)msg->msg_name : NULL, auio, data, control, flags);

	/*
	 * Residual data is possible in the case of IO vectors but not
	 * in the mbuf case since the latter is treated as atomic send.
	 * If pru_sosend() consumed a portion of the iovecs data and
	 * the error returned is transient, treat it as success; this
	 * is consistent with sendit() behavior.
	 */
	if (auio != NULL && uio_resid(auio) != datalen &&
	    (error == ERESTART || error == EINTR || error == EWOULDBLOCK))
		error = 0;

	if (error == 0 && sentlen != NULL) {
		if (auio != NULL)
			*sentlen = datalen - uio_resid(auio);
		else
			*sentlen = datalen;
	}
	
	return error;

/*
 * In cases where we detect an error before returning, we need to
 * free the mbuf chain if there is one. sosend (and pru_sosend) will
 * free the mbuf chain if they encounter an error.
 */
errorout:
	if (control)
		m_freem(control);
	if (data)
		m_freem(data);
	if (sentlen)
		*sentlen = 0;
	return error;
}

errno_t
sock_send(
	socket_t			sock,
	const struct msghdr	*msg,
	int					flags,
	size_t				*sentlen)
{
	if (msg == NULL || msg->msg_iov == NULL || msg->msg_iovlen < 1)
		return EINVAL;
	return sock_send_internal(sock, msg, NULL, flags, sentlen);
}

errno_t
sock_sendmbuf(
	socket_t			sock,
	const struct msghdr	*msg,
	mbuf_t				data,
	int					flags,
	size_t				*sentlen)
{
	if (data == NULL || (msg &&
		(msg->msg_iov != NULL || msg->msg_iovlen != 0))) {
		if (data)
			m_freem(data);
		return EINVAL;
	}
	return sock_send_internal(sock, msg, data, flags, sentlen);
}

errno_t
sock_shutdown(
	socket_t	sock,
	int			how)
{
	if (sock == NULL) return EINVAL;
	return soshutdown(sock, how);
}

typedef	void	(*so_upcall)(struct socket *sock, void* arg, int waitf);

errno_t
sock_socket(
	int				domain,
	int				type,
	int				protocol,
	sock_upcall		callback,
	void*			context,
	socket_t		*new_so)
{
	int	error = 0;
	if (new_so == NULL) return EINVAL;
	/* socreate will create an initial so_count */
	error = socreate(domain, new_so, type, protocol);
	if (error == 0 && callback)
	{
		(*new_so)->so_rcv.sb_flags |= SB_UPCALL;
		(*new_so)->so_upcall = (so_upcall)callback;
		(*new_so)->so_upcallarg = context;
	}
	return error;
}

void
sock_close(
	socket_t	sock)
{
	if (sock == NULL) return;
	soclose(sock);
}

/* Do we want this to be APPLE_PRIVATE API?: YES (LD 12/23/04)*/
void
sock_retain(
	socket_t	sock)
{
	if (sock == NULL) return;
	socket_lock(sock, 1);
	sock->so_retaincnt++;
	sock->so_usecount++;	/* add extra reference for holding the socket */
	socket_unlock(sock, 1);
}

/* Do we want this to be APPLE_PRIVATE API? */
void
sock_release(
	socket_t	sock)
{
	if (sock == NULL) return;
	socket_lock(sock, 1);
	sock->so_retaincnt--;
	if (sock->so_retaincnt < 0)
		panic("sock_release: negative retain count for sock=%x cnt=%x\n",
			sock, sock->so_retaincnt);
	if ((sock->so_retaincnt == 0) && (sock->so_usecount == 2))
		soclose_locked(sock); /* close socket only if the FD is not holding it */
	else
		sock->so_usecount--;	/* remove extra reference holding the socket */
	socket_unlock(sock, 1);
}

errno_t
sock_setpriv(
	socket_t	sock,
	int			on)
{
	if (sock == NULL) return EINVAL;
	socket_lock(sock, 1);
	if (on)
	{
		sock->so_state |= SS_PRIV;
	}
	else
	{
		sock->so_state &= ~SS_PRIV;
	}
	socket_unlock(sock, 1);
	return 0;
}

int
sock_isconnected(
	socket_t sock)
{
	int retval;
	socket_lock(sock, 1);
	retval = (sock->so_state & SS_ISCONNECTED) != 0;
	socket_unlock(sock, 1);
	return (retval);
}

int
sock_isnonblocking(
	socket_t sock)
{
	int retval;
	socket_lock(sock, 1);
	retval = (sock->so_state & SS_NBIO) != 0;
	socket_unlock(sock, 1);
	return (retval);
}

errno_t
sock_gettype(
	socket_t	sock,
	int			*outDomain,
	int			*outType,
	int			*outProtocol)
{
	socket_lock(sock, 1);
	if (outDomain)
		*outDomain = sock->so_proto->pr_domain->dom_family;
	if (outType)
		*outType = sock->so_type;
	if (outProtocol)
		*outProtocol = sock->so_proto->pr_protocol;
	socket_unlock(sock, 1);
	return 0;
}

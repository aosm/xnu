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
 *	Copyright (c) 1995 Apple Computer, Inc. 
 *
 *  Change Log:
 *    Created February 20, 1995 by Tuyen Nguyen
 *    Modified for MP, 1996 by Tuyen Nguyen
 *   Modified, March 17, 1997 by Tuyen Nguyen for MacOSX.
 */

#include <sys/errno.h>
#include <sys/types.h>
#include <sys/param.h>
#include <machine/spl.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/proc.h>
#include <sys/filedesc.h>
#include <sys/fcntl.h>
#include <sys/mbuf.h>
#include <sys/ioctl.h>
#include <sys/malloc.h>
#include <sys/socket.h>
#include <sys/socketvar.h>

#include <net/if.h>

#include <netat/appletalk.h>
#include <netat/sysglue.h>
#include <netat/at_pcb.h>
#include <netat/atp.h>
#include <netat/ddp.h>
#include <netat/asp.h>
#include <netat/at_var.h>
#include <netat/debug.h>

static int loop_cnt;
#define CHK_LOOP(str) { \
  if (loop_cnt++ > 100) { \
     kprintf("%s", str); \
     break; \
  } \
}

#define atpBDSsize    (sizeof(struct atpBDS)*ATP_TRESP_MAX)
#define aspCMDsize    (atpBDSsize+sizeof(struct atp_set_default)+TOTAL_ATP_HDR_SIZE)
#define SCBS_PER_BLK  16
#define TICKS_PER_SEC HZ
#define SESS_TMO_RES  2
#define DEF_SESS_TMO  120
#define NEXT_SEQ_NUM(x) (x = (x == 65535) ? 0 : (x + 1))
#define MAX_RCV_CNT   5
#define BAD_REMADDR(addr) \
	( (*(long *)&scb->rem_addr != *(long *)&addr) \
	&& ((scb->rem_addr.net != addr.net) \
		|| (scb->rem_addr.node != addr.node)) )

int  ASPputmsg();
int  ASPgetmsg();
void asp_init();
void asp_ack_reply();
void asp_nak_reply();
void asp_clock();
void asp_clock_funnel();
int  asp_open();
int  asp_close();
int  asp_wput();
void atp_retry_req();
StaticProc asp_scb_t *asp_find_scb();
StaticProc asp_scb_t *asp_scb_alloc();

StaticProc void asp_putnext();
StaticProc void asp_iocack();
StaticProc void asp_iocnak();
StaticProc void asp_dequeue_scb();
StaticProc void asp_scb_free();
StaticProc void asp_timout();
StaticProc void asp_untimout();
StaticProc void asp_hangup();
StaticProc void asp_send_tickle();
StaticProc void asp_send_tickle_funnel();
StaticProc void asp_accept();
StaticProc int  asp_send_req();

extern at_ifaddr_t *ifID_home;
extern int atp_pidM[];
extern gref_t *atp_inputQ[];
gbuf_t *scb_resource_m = 0;
unsigned char asp_inpC[256];
asp_scb_t *asp_scbQ[256];

static at_retry_t asp_def_retry = {2, -1, 1};
static unsigned char scb_tmo_cnt;
asp_scb_t *scb_used_list;
static asp_scb_t *scb_tmo_list;
asp_scb_t *scb_free_list;
atlock_t aspall_lock, asptmo_lock;

int
asp_readable(gref)
	gref_t *gref;
{
	return (((asp_scb_t *)gref->info)->sess_ioc ? 1 : 0);
}

void
asp_init()
{
	scb_tmo_cnt = 1;
	scb_tmo_list = 0;
	scb_used_list = 0;
	scb_free_list = 0;
	bzero(asp_inpC, sizeof(asp_inpC));
	bzero(asp_scbQ, sizeof(asp_scbQ));
}

/*
 * the open routine allocates a state structure
 */
int asp_open(gref)
	gref_t *gref;
{
	int s;
	asp_scb_t *scb;

	/*
	 * if no asp structure available, return failure
	 */
	if ((scb = asp_scb_alloc()) == 0)
	    return ENOBUFS;

	/*
	 * initialize the gref data structure
	 */
	gref->info = (void *)scb;
	gref->readable = asp_readable;

	/*
	 * initialize the scb data structure
	 */
	scb->dflag = 1;
	scb->magic_num = 222;
	scb->state = ASPSTATE_Idle;
	scb->pid = gref->pid;
	scb->gref = gref;
	scb->session_timer = DEF_SESS_TMO;
	scb->cmd_retry = asp_def_retry;
	ATDISABLE(s, aspall_lock);
	if ((scb->next_scb = scb_used_list) != 0)
		scb->next_scb->prev_scb = scb;
	scb_used_list = scb;
	ATENABLE(s, aspall_lock);

	/*
	 * return success
	 */
	dPrintf(D_M_ASP, D_L_INFO, ("asp_open: pid=%d\n", scb->pid));
	return 0;
} /* asp_open */

/*
 * the close routine frees all the data structures
 */
int
asp_close(gref)
	gref_t *gref;
{
	int s;
	unsigned char sock_num;
	asp_scb_t *scb, *new_scb;
	gbuf_t *m;

	scb = (asp_scb_t *)gref->info;
	dPrintf(D_M_ASP, D_L_INFO, ("asp_close: loc=%d\n",
		scb->loc_addr.socket));

	if (scb->pid && scb->sess_ioc && (scb->dflag != 1)) {
		/*
		 * send the CloseSess response to peer
		 */
		if (gbuf_type(scb->sess_ioc) != MSG_PROTO) {
			ATDISABLE(s, scb->lock);
			m = scb->sess_ioc;
			scb->sess_ioc = gbuf_next(m);
			ATENABLE(s, scb->lock);
			atp_send_rsp(scb->gref, m, TRUE);
		}
	}

	if (scb->atp_state) {
		sock_num = scb->loc_addr.socket;
		ATDISABLE(s, aspall_lock);
		if ((scb->dflag != 1) && scb->stat_msg) {
			untimeout(atp_retry_req, scb->stat_msg);
			gbuf_freem(scb->stat_msg);
			scb->stat_msg = 0;
		}
		if (asp_scbQ[sock_num]->next_scb == 0) {
			asp_scbQ[sock_num] = 0;
			asp_inpC[sock_num] = 0;
			ATENABLE(s, aspall_lock);
			dPrintf(D_M_ASP, D_L_INFO,
			("         : atp_close(), loc=%d\n", scb->loc_addr.socket));
			atp_close(gref, 0);
		} else {
			asp_inpC[sock_num]--;
			if (scb == asp_scbQ[sock_num]) {
				new_scb = scb->next_scb;
				new_scb->prev_scb = 0;
				asp_scbQ[sock_num] = new_scb;
				new_scb->atp_state->atp_gref = new_scb->gref;
				new_scb->atp_state->pid = new_scb->pid;
				atp_inputQ[sock_num] = new_scb->gref;
			} else {
				if ((scb->prev_scb->next_scb = scb->next_scb) != 0)
					scb->next_scb->prev_scb = scb->prev_scb;
			}
			scb->next_scb = 0;
			ATENABLE(s, aspall_lock);
		}
	} else
		asp_dequeue_scb(scb);

	/*
	 * free all allocated blocks if any
	 */
	ATDISABLE(s, scb->lock);
	if (scb->stat_msg) {
		gbuf_freem(scb->stat_msg);
		scb->stat_msg = 0;
	}
	if (scb->sess_ioc) {
		gbuf_freel(scb->sess_ioc);
		scb->sess_ioc = 0;
	}
	if (scb->req_msgq) {
		gbuf_freel(scb->req_msgq);
		scb->req_msgq = 0;
	}

	scb->rem_addr.node = 0;
	ATENABLE(s, scb->lock);

	/*
	 * stop all timers
	 */
	scb->tmo_cnt = 0;
	asp_untimout(asp_hangup, scb);
	untimeout(asp_send_tickle_funnel, (void *)scb); /* added for 2225395 */

	/*
	 * free the asp session control block
	 */
	scb->state = ASPSTATE_Close;
	asp_scb_free(scb);
	return 0;
} /* asp_close */

static char *aspStateStr(state)
     int state;
{
  return ((state==ASPSTATE_Close)? "Close":
	  (state==ASPSTATE_Idle)? "Idle":
	  (state==ASPSTATE_WaitingForGetStatusRsp)? "GetStatusRsp":
	  (state==ASPSTATE_WaitingForOpenSessRsp)? "OpenSessRsp":
	  (state==ASPSTATE_WaitingForCommandRsp)? "CmdRsp":
	  (state==ASPSTATE_WaitingForWriteContinue)? "WriteCont":
	  (state==ASPSTATE_WaitingForWriteRsp)? "WriteRsp":
	  (state==ASPSTATE_WaitingForWriteContinueRsp)? "WriteContRsp":
	  (state==ASPSTATE_WaitingForCloseSessRsp)? "CloseSessRsp":
	  "unknown");
}

static char *aspCmdStr(aspCmd)
     int aspCmd;
{ 
return ((aspCmd==ASPFUNC_CloseSess)? "CloseSess":
	(aspCmd==ASPFUNC_Command)? "Command":
	(aspCmd==ASPFUNC_GetStatus)? "GetStatus":
	(aspCmd==ASPFUNC_OpenSess)? "OpenSess":
	(aspCmd==ASPFUNC_Tickle)? "Tickle":
	(aspCmd==ASPFUNC_Write)? "Write":
	(aspCmd==ASPFUNC_WriteContinue)? "WriteContinue":
	(aspCmd==ASPFUNC_Attention)? "Attention":
	(aspCmd==ASPFUNC_CmdReply)? "CmdReply": "unknown");
}

static char *aspIOCStr(aspIOC)
     int aspIOC;
{
return (
	(aspIOC==ASPIOC_ClientBind)? "ClientBind":
	(aspIOC==ASPIOC_CloseSession)? "CloseSession":
	(aspIOC==ASPIOC_GetLocEntity)? "GetLocEntity":
	(aspIOC==ASPIOC_GetRemEntity)? "GetRemEntity":
	(aspIOC==ASPIOC_GetSession)? "GetSession":
	(aspIOC==ASPIOC_GetStatus)? "GetStatus":
	(aspIOC==ASPIOC_ListenerBind)? "ListenerBind":
	(aspIOC==ASPIOC_OpenSession)? "OpenSession":
	(aspIOC==ASPIOC_StatusBlock)? "StatusBlock":
	(aspIOC==ASPIOC_SetPid)? "SetPid":
	(aspIOC==ASPIOC_GetSessId)? "GetSessId":
	(aspIOC==ASPIOC_EnableSelect)? "EnableSelect":
	(aspIOC==ASPIOC_Look)? "Look":
	"unknown"
	);
}

#ifdef AT_MBUF_TRACE

static char mbuf_str[100];
char *mbuf_totals() 
{
  sprintf(mbuf_str, 
	  /*
	  "dat = %d, prot = %d, ioc = %d, err = %d, hu = %d, ack = %d, nak = %d, ctl = %d",
	  */
	  "dat = %d, prot = %d, ioc = %d, ctl = %d",
	  mbstat.m_mtypes[MSG_DATA], mbstat.m_mtypes[MSG_PROTO], mbstat.m_mtypes[MSG_IOCTL],
	  /*
	  mbstat.m_mtypes[MSG_ERROR], mbstat.m_mtypes[MSG_HANGUP], mbstat.m_mtypes[MSG_IOCACK],
	  mbstat.m_mtypes[MSG_IOCNAK], 
	  */
	  mbstat.m_mtypes[MSG_CTL]);
  return(&mbuf_str[0]);
}

void trace_beg(str, m)
     char *str;
     gbuf_t *m;
{
	int i = 0, j = 0;
	gbuf_t *mdata, *mchain;

	if (m)
	  for (i = 0, j = 0, mdata = m, mchain = m; mdata; i++) {
	    mdata = gbuf_cont(mdata);
	    if (!mdata && mchain) {
	      mdata = gbuf_next(mchain);
	      mchain = mdata;
	      j++;
	    }
	  }
	dPrintf(D_M_ASP, D_L_TRACE,
		("%s: %s, m# = %d, c# = %d\n", str, mbuf_totals(), i, j));
}

void trace_end(str)
     char *str;
{
	dPrintf(D_M_ASP, D_L_TRACE,
		("  %s: %s\n", str, mbuf_totals()));
}
#endif AT_MBUF_TRACE

/*
 * the write routine
 */
int asp_wput(gref, m)
	gref_t *gref;
	gbuf_t *m;
{
	int s, err;
	unsigned char sockSav, sock_num;
	gbuf_t *mioc, *mdata;
	ioc_t *iocbp;
	asp_scb_t *scb, *server_scb, *curr_scb;
	at_inet_t *addr;
	asp_word_t aw;
	union asp_primitives *primitives;
	asp_status_cmd_t *status_cmd;
	asp_open_cmd_t *open_cmd;
	at_retry_t Retry;

	scb = (asp_scb_t *)gref->info;
	if (scb->dflag == 0) {
		atp_wput(gref, m);
		return 0;
	}

	if (gbuf_type(m) != MSG_IOCTL) {
		dPrintf(D_M_ASP, D_L_WARNING,
			("asp_wput: UNKNOWN message, type=%d\n", 
			 gbuf_type(m)));
		gbuf_freem(m);
		return 0;
	}

	mioc = m;
	iocbp = (ioc_t *)gbuf_rptr(mioc);

	dPrintf(D_M_ASP_LOW, D_L_INFO,
		("asp_wput: %s, loc=%d, state=%s\n", 
		 aspIOCStr(iocbp->ioc_cmd), scb->loc_addr.socket, 
		 aspStateStr(scb->state)));

	switch (iocbp->ioc_cmd) {
	case ASPIOC_CloseSession:
		if ((scb->state == ASPSTATE_Close) || (scb->rem_addr.node == 0))
			break;

		Retry.retries = 3;
		Retry.interval = 1;
		aw.func = ASPFUNC_CloseSess;
		aw.param1 = scb->sess_id;
		aw.param2 = 0;
		iocbp->ioc_private = (void *)scb;
		scb->ioc_wait = (unsigned char)(iocbp->ioc_cmd & 0xff);
		iocbp->ioc_cmd = AT_ATP_ISSUE_REQUEST;
		asp_send_req(gref, mioc, &scb->rem_addr, &Retry, &aw, 
			     0, ASPSTATE_WaitingForCloseSessRsp, 0x01);
		return 0;

	case ASPIOC_ClientBind:
		/*
		 * open an ATP channel
		 */
		if ((err = atp_open(gref, 0)) != 0) {
			asp_iocnak(gref, mioc, err);
			return 0;
		}
		scb->atp_state = (atp_state_t *)gref->info;
		scb->atp_state->pid = scb->pid;
		/*
		 * bind to any available socket
		 */
		scb->dflag = 2;
		sockSav = scb->dflag;
		if ((sock_num = (at_socket)atp_bind(gref, 0, &sockSav)) == 0) {
			scb->atp_state = (atp_state_t *)0;
			atp_close(gref, 0);
			gref->info = (void *)scb;
			asp_iocnak(gref, mioc, EINVAL);
			return 0;
		}
		gref->info = (void *)scb;
		asp_dequeue_scb(scb);
		scb->atp_state->dflag = scb->dflag;
		scb->loc_addr.socket = sock_num;
		asp_scbQ[sock_num] = scb;
		asp_inpC[sock_num]++;
		atp_pidM[sock_num] = 0;
		break;

	case ASPIOC_ListenerBind:
		/*
		 * open an ATP channel
		 */
		if ((err = atp_open(gref, 0)) != 0) {
			asp_iocnak(gref, mioc, err);
			return 0;
		}
		scb->atp_state = (atp_state_t *)gref->info;
		scb->atp_state->pid = scb->pid;
		/*
		 * bind to any available socket
		 */
		if ((sock_num = (at_socket)atp_bind(gref, 0, 0)) == 0) {
			scb->atp_state = (atp_state_t *)0;
			atp_close(gref, 0);
			gref->info = (void *)scb;
			asp_iocnak(gref, mioc, EINVAL);
			return 0;
		}
		gref->info = (void *)scb;
		asp_dequeue_scb(scb);
		scb->atp_state->dflag = scb->dflag;
		scb->loc_addr.socket = sock_num;
		asp_scbQ[sock_num] = scb;
		asp_inpC[sock_num]++;
		if (gbuf_cont(mioc))
			*(at_inet_t *)gbuf_rptr(gbuf_cont(mioc)) = scb->loc_addr;
		break;

	case ASPIOC_GetLocEntity:
		if ((gbuf_cont(mioc) == 0) || (scb->atp_state == 0)) {
			asp_iocnak(gref, mioc, EPROTO);
			return 0;
		}
		*(at_inet_t *)gbuf_rptr(gbuf_cont(mioc)) = scb->loc_addr;
		break;

	case ASPIOC_GetRemEntity:
		if ((gbuf_cont(mioc) == 0) || (scb->atp_state == 0)) {
			asp_iocnak(gref, mioc, EPROTO);
			return 0;
		}
		*(at_inet_t *)gbuf_rptr(gbuf_cont(mioc)) = scb->rem_addr;
		break;

	case ASPIOC_GetSession:
		if ((mdata = gbuf_cont(mioc)) == 0) {
			asp_iocnak(gref, mioc, EPROTO);
			return 0;
		}
		addr = (at_inet_t *)gbuf_rptr(mdata);
		scb->tickle_interval = (unsigned short)addr->node;
		scb->session_timer = addr->net;
		server_scb = asp_scbQ[addr->socket];
/*### LD 10/28/97: changed to make sure we're not accessing a null server_scb */
		if (server_scb == 0) {
			asp_iocnak(gref, mioc, EPROTO);
			return 0;
		}
		if (server_scb->sess_ioc == 0) {
			asp_iocnak(gref, mioc, EPROTO);
			return 0;
		}

		/*
		 * open an ATP channel
		 */
		if ((err = atp_open(gref, 0)) != 0) {
			gref->info = (void *)scb;
			asp_iocnak(gref, mioc, err);
			return 0;
		}
		scb->atp_state = (atp_state_t *)gref->info;
		scb->atp_state->pid = scb->pid;
		/*
		 * bind to any available socket
		 */
		scb->dflag = 3;
		sockSav = scb->dflag;
		if ((sock_num = (at_socket)atp_bind(gref, 0, &sockSav)) == 0) {
			atp_close(gref, 0);
			asp_dequeue_scb(scb);
			ATDISABLE(s, aspall_lock);
			sock_num = sockSav;
			scb->loc_addr.socket = sock_num;
			for (curr_scb = asp_scbQ[sock_num];
				curr_scb->next_scb; curr_scb = curr_scb->next_scb) ;
			scb->prev_scb = curr_scb;
			curr_scb->next_scb = scb;
			scb->atp_state = curr_scb->atp_state;
			ATENABLE(s, aspall_lock);
		} else {
			asp_dequeue_scb(scb);
			ATDISABLE(s, aspall_lock);
			scb->loc_addr.socket = sock_num;
			asp_scbQ[sock_num] = scb;
			scb->atp_state->dflag = scb->dflag;
			ATENABLE(s, aspall_lock);
		}
		gref->info = (void *)scb;
		asp_inpC[sock_num]++;
		gbuf_cont(mioc) = 0;
		asp_accept(server_scb, scb, mdata);
		break;

	case ASPIOC_GetStatus:
		if ((mdata = gbuf_cont(mioc)) == 0) {
			asp_iocnak(gref, mioc, EINVAL);
			return 0;
		}
		gbuf_cont(mioc) = 0;
		status_cmd = (asp_status_cmd_t *)gbuf_rptr(mdata);
		aw.func = ASPFUNC_GetStatus;
		aw.param1 = 0;
		aw.param2 = 0;
		scb->ioc_wait = (unsigned char)(iocbp->ioc_cmd & 0xff);
		iocbp->ioc_cmd = AT_ATP_ISSUE_REQUEST_DEF;
		/* bms:  make sure this is an ALO request */
		asp_send_req(gref, mioc, &status_cmd->SLSEntityIdentifier,
			     &status_cmd->Retry, &aw, 0, ASPSTATE_WaitingForGetStatusRsp, 0xff);
		gbuf_freeb(mdata);
		return 0;

	case ASPIOC_OpenSession:
		if ((mdata = gbuf_cont(mioc)) == 0) {
			asp_iocnak(gref, mioc, EINVAL);
			return 0;
		}
		gbuf_cont(mioc) = 0;
		open_cmd = (asp_open_cmd_t *)gbuf_rptr(mdata);
		scb->svc_addr = open_cmd->SLSEntityIdentifier;
		scb->rem_addr = scb->svc_addr;
		scb->rem_node = scb->rem_addr.node;
		scb->rem_addr.node = 0;
		scb->tickle_interval = open_cmd->TickleInterval;
		scb->session_timer = open_cmd->SessionTimer;
		aw.func = ASPFUNC_OpenSess;
		aw.param1 = scb->loc_addr.socket;
		aw.param2 = ASP_Version;
		scb->ioc_wait = (unsigned char)(iocbp->ioc_cmd & 0xff);
		iocbp->ioc_cmd = AT_ATP_ISSUE_REQUEST_DEF;
		asp_send_req(gref, mioc, &open_cmd->SLSEntityIdentifier,
			     &open_cmd->Retry, &aw, 1, ASPSTATE_WaitingForOpenSessRsp, 0x01);
		gbuf_freeb(mdata);
		return 0;

	case ASPIOC_StatusBlock:
		/*
		 * save the server status block
		 */
	  if (scb->stat_msg)
			gbuf_freem(scb->stat_msg);
	  scb->stat_msg = gbuf_cont(mioc);
	  gbuf_cont(mioc) = 0;
	  break;

	  /* *** Does scb->pid get used in a packet header,
		 and if so is it in ASP, or in ATP? 
		 If not, do we need this call for anything?
		 (cap does currently use it in _ANS code.)
	     *** */
	case ASPIOC_SetPid:
		if (gbuf_cont(mioc) == 0) {
			asp_iocnak(gref, mioc, EINVAL);
			return 0;
		}
		scb->pid = *(int *)gbuf_rptr(gbuf_cont(mioc));
		break;

	case ASPIOC_GetSessId:
		if (gbuf_cont(mioc) == 0) {
			asp_iocnak(gref, mioc, EINVAL);
			return 0;
		}
		*(gref_t **)gbuf_rptr(gbuf_cont(mioc)) = gref;
		break;

	case ASPIOC_Look:
		if (gbuf_cont(mioc) == 0) {
			asp_iocnak(gref, mioc, EINVAL);
			return 0;
		}
		if (scb->sess_ioc) {
			primitives = (union asp_primitives *)gbuf_rptr(scb->sess_ioc);
			if (primitives->Primitive == ASPFUNC_CmdReply)
				*(int *)gbuf_rptr(gbuf_cont(mioc)) = 0;
			else
				*(int *)gbuf_rptr(gbuf_cont(mioc)) = 1;
		} else
			*(int *)gbuf_rptr(gbuf_cont(mioc)) = -1;
		break;

	case DDP_IOC_GET_CFG:
		{
		struct atp_state *atp = (struct atp_state *)gref->info;
		if (atp->dflag)
			atp = atp->atp_msgq;
					
		if (gbuf_cont(mioc) == 0) {
			asp_iocnak(gref, mioc, EINVAL);
			return 0;
		}
		/* *** borrowed from ddp_proto.c to handle DDP_IOC_GET_CFG
		       on atp fd *** */
		scb->state = ASPSTATE_Idle;
		{
		/* *** was ddp_get_cfg() *** */
		  ddp_addr_t *cfgp = 
		    (ddp_addr_t *)gbuf_rptr(gbuf_cont(mioc));
		  cfgp->inet.net = ifID_home->ifThisNode.s_net;
		  cfgp->inet.node = ifID_home->ifThisNode.s_node;
		  cfgp->inet.socket = atp->atp_socket_no;
		  cfgp->ddptype = DDP_ATP;
		}
		gbuf_wset(gbuf_cont(mioc), sizeof(at_inet_t));
		}
		break;

	default:
		asp_iocnak(gref, mioc, EINVAL);
		return 0;
	}

	asp_iocack(gref, mioc);
	return 0;
} /* asp_wput */

/*
 * send request routine
 */
StaticProc int
asp_send_req(gref, mioc, dest, retry, awp, xo, state, bitmap)
	gref_t *gref;
	gbuf_t *mioc;
	at_inet_t *dest;
	at_retry_t *retry;
	asp_word_t *awp;
	unsigned char xo;
	unsigned char state;
	unsigned char bitmap;
{
	int i;
	gbuf_t *mdata;
	ioc_t *iocbp;
	struct atp_set_default *sd;
	at_ddp_t *ddp;
	at_atp_t *atp;
	struct atpBDS *atpBDS;
	asp_scb_t *scb = (asp_scb_t *)gref->info;

	/*
	 * allocate an ATP buffer for the request
	 */
	if ((gbuf_cont(mioc) = gbuf_alloc(aspCMDsize, PRI_MED)) == 0) {
		if (awp->func == ASPFUNC_Tickle)
			gbuf_freem(mioc);
		else
			asp_iocnak(gref, mioc, ENOBUFS);
		dPrintf(D_M_ASP, D_L_WARNING,
		("asp_send_req: ENOBUFS, loc=%d\n", scb->loc_addr.socket));

		return -1;
	}
	mdata = gbuf_cont(mioc);
	iocbp = (ioc_t *)gbuf_rptr(mioc);

	/*
	 * build the request
	 */
	atpBDS = (struct atpBDS *)gbuf_rptr(mdata);
	gbuf_wset(mdata,atpBDSsize);
	for (i=0; i < ATP_TRESP_MAX; i++) {
		*(unsigned long  *)atpBDS[i].bdsBuffAddr = 1;
		*(unsigned short *)atpBDS[i].bdsBuffSz = ATP_DATA_SIZE;
	}
	sd = (struct atp_set_default *)gbuf_wptr(mdata);
	gbuf_winc(mdata,sizeof(struct atp_set_default));
	sd->def_retries = (retry->retries == -1) ?
		ATP_INFINITE_RETRIES : retry->retries;
	sd->def_rate = retry->interval*TICKS_PER_SEC;
	sd->def_BDSlen = atpBDSsize;
	ddp = (at_ddp_t *)gbuf_wptr(mdata);
	NET_ASSIGN(ddp->src_net, scb->loc_addr.net);
	ddp->src_node = scb->loc_addr.node;
	NET_ASSIGN(ddp->dst_net, dest->net);
	ddp->dst_node = dest->node;
	ddp->dst_socket = dest->socket;
	UAS_ASSIGN(ddp->checksum, 0);
	atp = ATP_ATP_HDR(gbuf_wptr(mdata));
	atp->xo = xo;
	atp->xo_relt = xo;
	atp->bitmap = bitmap;
	gbuf_winc(mdata,TOTAL_ATP_HDR_SIZE);
	*(asp_word_t *)atp->user_bytes = *awp;
	iocbp->ioc_count = gbuf_len(mdata);
	iocbp->ioc_rval = 0;

	/*
	 * send the request
	 */
	scb->state = state;
	dPrintf(D_M_ASP, D_L_INFO,
		("asp_send_req: %s, loc=%d, rem= %d, len=%d, state=%s\n",
		 aspCmdStr(awp->func),
		 scb->loc_addr.socket, ddp->dst_socket, iocbp->ioc_count,
		 aspStateStr(scb->state)));

	atp_send_req(gref, mioc);
	return 0;
}

/*
 * send tickle routine - funnelled version
 */
StaticProc void
asp_send_tickle_funnel(scb)
	asp_scb_t *scb;
{
        thread_funnel_set(network_flock, TRUE);
	asp_send_tickle(scb);
        thread_funnel_set(network_flock, FALSE);
}


/*
 * send tickle routine
 */
StaticProc void
asp_send_tickle(scb)
	asp_scb_t *scb;
{
	gbuf_t *mioc;
	at_retry_t retry;
	asp_word_t aw;
	at_inet_t *dest;


	/*
	 * make sure the connection is still there
	 */
	if (scb->rem_addr.node == 0) {
		return;
        }

	if ((mioc = gbuf_alloc(sizeof(ioc_t), PRI_HI)) == 0) {
		dPrintf(D_M_ASP, D_L_WARNING,
		("asp_send_tickle: ENOBUFS 0, loc=%d, rem=%d\n",
			scb->loc_addr.socket,scb->rem_addr.socket));
		timeout(asp_send_tickle_funnel, (void *)scb, 10);
		return;
	}
	gbuf_wset(mioc,sizeof(ioc_t));
	gbuf_set_type(mioc, MSG_IOCTL);

	dest = scb->svc_addr.node ?
		(at_inet_t *)&scb->svc_addr : (at_inet_t *)&scb->rem_addr;
	retry.interval = scb->tickle_interval;
	retry.retries  = -1;
	retry.backoff  = 1;
	aw.func = ASPFUNC_Tickle;
	aw.param1 = scb->sess_id;
	aw.param2 = 0;
	((ioc_t *)gbuf_rptr(mioc))->ioc_cr = (void *)scb;
	((ioc_t *)gbuf_rptr(mioc))->ioc_cmd = AT_ATP_ISSUE_REQUEST_TICKLE;

	if (asp_send_req(scb->gref, mioc, dest, &retry, &aw, 0, scb->state, 0)) {
		dPrintf(D_M_ASP, D_L_WARNING,
			("asp_send_tickle: ENOBUFS 1, loc=%d, rem=%d\n",
			 scb->loc_addr.socket,scb->rem_addr.socket));

		timeout(asp_send_tickle_funnel, (void *)scb, 10);
		return;
	}
}

/*
 * accept connection routine
 */
StaticProc void
asp_accept(scb, sess_scb, m)
	asp_scb_t *scb;
	asp_scb_t *sess_scb;
	gbuf_t *m;
{
	int s;
	gbuf_t *mdata;
	at_ddp_t *ddp;
	at_atp_t *atp;
	asp_word_t *awp;
	at_inet_t rem_addr;

	mdata = scb->sess_ioc;
	ddp = (at_ddp_t *)gbuf_rptr(mdata);
	atp = (at_atp_t *)(gbuf_rptr(mdata) + DDP_X_HDR_SIZE);
	rem_addr.net = NET_VALUE(ddp->src_net);
	rem_addr.node = ddp->src_node;
	rem_addr.socket = ddp->src_socket;
	awp = (asp_word_t *)atp->user_bytes;

	sess_scb->loc_addr.net = NET_VALUE(ddp->dst_net);
	sess_scb->loc_addr.node = ddp->dst_node;
	NET_ASSIGN(ddp->src_net, sess_scb->loc_addr.net);
	ddp->src_node = sess_scb->loc_addr.node;
	NET_ASSIGN(ddp->dst_net, rem_addr.net);
	ddp->dst_node = rem_addr.node;
	ddp->dst_socket = rem_addr.socket;

	sess_scb->sess_id = sess_scb->loc_addr.socket;
	sess_scb->rem_socket = rem_addr.socket;
	sess_scb->rem_addr = rem_addr;
	sess_scb->rem_addr.socket = awp->param1;
	sess_scb->reply_socket = sess_scb->rem_addr.socket;
	awp->func = sess_scb->loc_addr.socket;
	awp->param1 = sess_scb->sess_id;
	awp->param2 = 0;
	gbuf_freeb(m);
	ATDISABLE(s, scb->lock);
	scb->sess_ioc = gbuf_next(mdata);
	ATENABLE(s, scb->lock);
	gbuf_next(mdata) = 0;
	asp_timout(asp_hangup, sess_scb, sess_scb->session_timer);
	atp_send_rsp(scb->gref, mdata, TRUE);
	asp_send_tickle(sess_scb);
	dPrintf(D_M_ASP, D_L_INFO,
		("asp_accept: ACCEPT connect request, loc=%d, rem=%x.%x.%d\n",
		sess_scb->loc_addr.socket,
		sess_scb->rem_addr.net,
		sess_scb->rem_addr.node,sess_scb->rem_addr.socket));
} /* asp_accept */

/*
 * timer routine - funneled version
 */
void asp_clock_funnel(arg)
	void *arg;
{
        thread_funnel_set(network_flock, TRUE);
	asp_clock(arg);
        thread_funnel_set(network_flock, FALSE);
}

/*
 * timer routine
 */
void asp_clock(arg)
	void *arg;
{
	int s;
	asp_scb_t *scb;
	void (*tmo_func)();

	ATDISABLE(s, asptmo_lock);
	if (scb_tmo_list)
		scb_tmo_list->tmo_delta--;
	while (((scb = scb_tmo_list) != 0) && (scb_tmo_list->tmo_delta == 0)) {
		if ((scb_tmo_list = scb->next_tmo) != 0)
			scb_tmo_list->prev_tmo = 0;
		if ((tmo_func = scb->tmo_func) != 0) {
			scb->tmo_func = 0;
			ATENABLE(s, asptmo_lock);
			(*tmo_func)(scb);
			ATDISABLE(s, asptmo_lock);
		}
	}
	ATENABLE(s, asptmo_lock);

	if (++scb_tmo_cnt == 0) scb_tmo_cnt++;
	timeout(asp_clock_funnel, (void *)arg, (1<<SESS_TMO_RES)*TICKS_PER_SEC);
        
}

/*
 * ACK reply routine
 */
void
asp_ack_reply(gref, mioc)
	register gref_t *gref;
	register gbuf_t *mioc;
{
	int s;
	int len, msize, nbds;
	register gbuf_t *mdata, *m, *mx;
	struct atpBDS *atpBDS;
	at_ddp_t *ddp;
	at_atp_t *atp;
	register asp_scb_t *scb, *sess_scb;
	register ioc_t *iocbp;
	register asp_word_t *awp;
	register asp_command_ind_t *command_ind;
	register asp_cmdreply_ind_t *cmdreply_ind;
	at_inet_t rem_addr;

	iocbp = (ioc_t *)gbuf_rptr(mioc);

	if (iocbp->ioc_cmd == AT_ATP_ISSUE_REQUEST_TICKLE) {
		/*
		 * ignore the ack for the tickle request
		 */
		scb = (asp_scb_t *)iocbp->ioc_cr;
		scb->tickle_tid = (unsigned short)iocbp->ioc_rval;
		gbuf_freem(mioc);
		return;
	}

	scb = (asp_scb_t *)gref->info;
	if (scb == 0) {
		gbuf_freem(mioc);
		return;
	}

	if (iocbp->ioc_cmd == AT_ATP_GET_POLL) {
		/*
		 * if no data, just drop the request
		 */
		if ((mdata = gbuf_cont(mioc)) == 0) {
			gbuf_freeb(mioc);
			return;
		}

		gbuf_set_type(mioc, MSG_IOCTL);
		ddp = (at_ddp_t *)gbuf_rptr(mdata);
		gbuf_rinc(mdata,DDP_X_HDR_SIZE);
		atp = (at_atp_t *)gbuf_rptr(mdata);
		gbuf_rinc(mdata,ATP_HDR_SIZE);
		rem_addr.net = NET_VALUE(ddp->src_net);
		rem_addr.node = ddp->src_node;
		rem_addr.socket = ddp->src_socket;
		awp = (asp_word_t *)atp->user_bytes;

		if (scb->next_scb) {
			/*
			 * find the responsible scb
			 */
			if ((scb = asp_find_scb(scb->loc_addr.socket, &rem_addr)) == 0) {
				gbuf_freem(mioc);
				return;
			}
		}
		dPrintf(D_M_ASP, D_L_INFO,
			("asp_ack_reply: %s, loc=%d, rem=%x.%x.%d\n",
			aspCmdStr(awp->func),scb->loc_addr.socket,
			NET_VALUE(ddp->src_net) ,ddp->src_node,ddp->src_socket));

		if (scb->rem_addr.node)
			asp_untimout(asp_hangup, scb);

		switch (awp->func) {
		case ASPFUNC_GetStatus:
			/*
			 * ignore if this is not a server socket
			 */
			mx = 0;
			if ((scb->dflag != 1) || (scb->stat_msg
					&& ((mx = gbuf_dupb(scb->stat_msg)) == 0)))
				break;
			gbuf_freeb(mioc);

			/*
			 * send the status block
			 */
			if (gbuf_cont(mdata)) {
				gbuf_freem(gbuf_cont(mdata));
				gbuf_cont(mdata) = 0;
			}
			gbuf_rdec(mdata,TOTAL_ATP_HDR_SIZE);
			if ((m = gbuf_alloc( (TOTAL_ATP_HDR_SIZE+atpBDSsize), PRI_MED)) == 0) {
				gbuf_freem(mdata);
				gbuf_freeb(mx);
				goto l_done;
			}
			bcopy(gbuf_rptr(mdata), gbuf_rptr(m), TOTAL_ATP_HDR_SIZE);
			gbuf_freeb(mdata);
			mdata = m;
			ddp = (at_ddp_t *)gbuf_rptr(mdata);
			gbuf_wset(mdata,DDP_X_HDR_SIZE);
			atp = (at_atp_t *)gbuf_wptr(mdata);
			gbuf_winc(mdata,ATP_HDR_SIZE);
			awp = (asp_word_t *)atp->user_bytes;
			NET_NET(ddp->src_net, ddp->dst_net);
			ddp->src_node = ddp->dst_node;
			NET_ASSIGN(ddp->dst_net, rem_addr.net);
			ddp->dst_node = rem_addr.node;
			ddp->dst_socket = rem_addr.socket;
			UAS_ASSIGN(ddp->checksum, 0);
			atpBDS = (struct atpBDS *)gbuf_wptr(mdata);
			msize = mx ? gbuf_msgsize(mx) : 0;
			for (nbds=0; (nbds < ATP_TRESP_MAX) && (msize > 0); nbds++) {
				len = msize < ATP_DATA_SIZE ? msize : ATP_DATA_SIZE;
				msize -= ATP_DATA_SIZE;
				*(long *)atpBDS[nbds].bdsUserData = 0;
				UAL_ASSIGN(atpBDS[nbds].bdsBuffAddr, 1);
				UAS_ASSIGN(atpBDS[nbds].bdsBuffSz, len);
			}
			UAS_ASSIGN(atpBDS[0].bdsDataSz, nbds);
			gbuf_winc(mdata,atpBDSsize);
			gbuf_cont(mdata) = mx;
			atp_send_rsp(gref, mdata, FALSE);
			goto l_done;

		case ASPFUNC_OpenSess:
			/*
			 * ignore if server is not ready
			 */
			if ((scb->dflag != 1) || (scb->stat_msg == 0))
				break;
			gbuf_freeb(mioc);

			if (gbuf_cont(mdata)) {
				gbuf_freem(gbuf_cont(mdata));
				gbuf_cont(mdata) = 0;
			}
			gbuf_rdec(mdata,TOTAL_ATP_HDR_SIZE);
			gbuf_wset(mdata,TOTAL_ATP_HDR_SIZE);
			if (awp->param2 != ASP_Version) {
				/*
				 * bad version number, send the OpenSession response
				 */
				awp->func = 0;
				awp->param1 = 0;
				awp->param2 = (unsigned short)ASPERR_BadVersNum;
				dPrintf(D_M_ASP, D_L_INFO,
					("             : version=%d\n",
					ASPERR_BadVersNum));

				NET_NET(ddp->src_net, ddp->dst_net);
				ddp->src_node = ddp->dst_node;
				NET_ASSIGN(ddp->dst_net, rem_addr.net);
				ddp->dst_node = rem_addr.node;
				ddp->dst_socket = rem_addr.socket;
				atp_send_rsp(gref, mdata, FALSE);
				return;
			}

			/*
			 * queue the connection request
			 */
			ATDISABLE(s, scb->lock);
			gbuf_next(mdata) = 0;
			if ((m = scb->sess_ioc) == 0) {
				scb->sess_ioc = mdata;
				if (scb->get_wait)
					thread_wakeup(&scb->event);
				else
					atalk_notify_sel(gref);
			} else {
				while (gbuf_next(m))
					m = gbuf_next(m);
				gbuf_next(m) = mdata;
			}
			ATENABLE(s, scb->lock);
			dPrintf(D_M_ASP, D_L_INFO,
				("             : QUEUE connect request\n"));

			return;

		case ASPFUNC_Command:
		case ASPFUNC_Write:
			if ( (scb->sess_id != awp->param1)
			     || (scb->rcv_seq_num != awp->param2)
			     || BAD_REMADDR(rem_addr) ) {
				char era[8], ra[8];
				sprintf(era,"%d.%d", scb->rem_addr.node,scb->rem_addr.socket);
				sprintf(ra,"%d.%d", rem_addr.node,rem_addr.socket);
				dPrintf(D_M_ASP, D_L_WARNING,
					("             : DROP, id=%d,esn=%d,sn=%d,erem=%s,rem=%s\n",
					scb->sess_id,scb->rcv_seq_num,awp->param2,era,ra));
				gbuf_cont(mioc) = 0;
				gbuf_rdec(mdata,TOTAL_ATP_HDR_SIZE);
				atp_drop_req(gref, mdata);
				break;
			}
			scb->reply_socket = rem_addr.socket;
			if (awp->func == ASPFUNC_Write)
				scb->wrt_seq_num = scb->rcv_seq_num;
			NEXT_SEQ_NUM(scb->rcv_seq_num);
			gbuf_set_type(mioc, MSG_PROTO);
			gbuf_wset(mioc,sizeof(asp_command_ind_t));
			command_ind = (asp_command_ind_t *)gbuf_rptr(mioc);
			command_ind->Primitive = (int)awp->func;
			command_ind->ReqRefNum =
				*(unsigned short *)atp->tid;
			command_ind->ReqType = awp->func;

			mdata = gbuf_strip(mdata);
			gbuf_cont(mioc) = mdata;
			ATDISABLE(s, scb->lock);
			if (scb->req_flag) {
				if ((mx = scb->req_msgq) != 0) {
					while (gbuf_next(mx))
						mx = gbuf_next(mx);
					gbuf_next(mx) = mioc;
				} else
					scb->req_msgq = mioc;
				ATENABLE(s, scb->lock);
			} else {
				scb->req_flag = 1;
				ATENABLE(s, scb->lock);
				asp_putnext(scb->gref, mioc);
			}
			goto l_done;

		case ASPFUNC_WriteContinue:
			if ( (scb->sess_id != awp->param1)
			     || (scb->snd_seq_num != awp->param2)
			     || BAD_REMADDR(rem_addr) ) {
				break;
			}
			scb->reply_socket = rem_addr.socket;
			gbuf_set_type(mioc, MSG_PROTO);
			gbuf_wset(mioc,sizeof(asp_command_ind_t));
			command_ind = (asp_command_ind_t *)gbuf_rptr(mioc);
			command_ind->Primitive = (int)awp->func;
			command_ind->ReqRefNum =
				*(unsigned short *)atp->tid;
			command_ind->ReqType = awp->func;

			mdata = gbuf_strip(mdata);
			gbuf_cont(mioc) = mdata;
			asp_putnext(scb->gref, mioc);
			goto l_done;

		case ASPFUNC_Tickle:
			if (scb->stat_msg) {
				sess_scb = asp_scbQ[awp->param1];
				if (sess_scb && sess_scb->next_scb)
					sess_scb = asp_find_scb(
						sess_scb->loc_addr.socket, &rem_addr);
				if (sess_scb) {
				if (sess_scb->rem_addr.node)
					asp_untimout(asp_hangup, sess_scb);
				if (sess_scb->rem_addr.node)
					asp_timout(asp_hangup, sess_scb, sess_scb->session_timer);
				}
			}
			dPrintf(D_M_ASP, D_L_INFO,
				("             : Tickle, %d -> %d, id=%d\n",
				ddp->src_socket,ddp->dst_socket,awp->param1));
			break;

		case ASPFUNC_CloseSess:
			if ( (scb->sess_id != awp->param1)
			     || (scb->state == ASPSTATE_Close)
			     || (scb->state == ASPSTATE_WaitingForCloseSessRsp)
			     || (scb->rem_addr.net != rem_addr.net)
			     || (scb->rem_addr.node != rem_addr.node) ) {
				dPrintf(D_M_ASP, D_L_INFO,
					("             : CLOSE retry, loc=%d, rem=%x.%x.%d\n",
					scb->loc_addr.socket,
					scb->rem_addr.net,
					scb->rem_addr.node,
					scb->rem_addr.socket));

				break;
			}
			gbuf_freeb(mioc);

			/*
			 * build the CloseSess response to be sent to peer
			 * when the session is closed by the user.
			 */
			if (gbuf_cont(mdata)) {
				gbuf_freem(gbuf_cont(mdata));
				gbuf_cont(mdata) = 0;
			}
			gbuf_rdec(mdata,TOTAL_ATP_HDR_SIZE);
			gbuf_wset(mdata,TOTAL_ATP_HDR_SIZE);
			NET_NET(ddp->src_net, ddp->dst_net);
			ddp->src_node = ddp->dst_node;
			NET_ASSIGN(ddp->dst_net, rem_addr.net);
			ddp->dst_node = rem_addr.node;
			ddp->dst_socket = rem_addr.socket;
			awp->func = 0;
			awp->param1 = 0;
			awp->param2 = 0;
			dPrintf(D_M_ASP,D_L_INFO,
				("             : CLOSE, loc=%d, rem=%x.%x.%d\n",
				scb->loc_addr.socket,
				scb->rem_addr.net,
				scb->rem_addr.node,
				scb->rem_addr.socket));

			gbuf_next(mdata) = 0;
			ATDISABLE(s, scb->lock);
			if (scb->sess_ioc)
				gbuf_freel(scb->sess_ioc);
			scb->sess_ioc = mdata;
			scb->state = ASPSTATE_Close;
			ATENABLE(s, scb->lock);

			/*
			 * notify upstream of the CloseSess from peer
			 */
			asp_hangup(scb);
			return;

		case ASPFUNC_Attention:
			if ( (scb->sess_id != awp->param1)
			     || (scb->rem_addr.net != rem_addr.net)
			     || (scb->rem_addr.node != rem_addr.node) ) {
				break;
			}
			gbuf_set_type(mioc, MSG_PROTO);
			gbuf_wset(mioc,sizeof(asp_command_ind_t));
			command_ind = (asp_command_ind_t *)gbuf_rptr(mioc);
			command_ind->Primitive = (int)awp->func;
			command_ind->ReqRefNum =
				*(unsigned short *)atp->tid;
			command_ind->ReqType = awp->func;
			scb->attn_tid = *(unsigned short *)atp->tid;
			scb->attn_flag = 1;
			gbuf_rdec(mdata,2); /* attention code */

			mdata = gbuf_strip(mdata);
			gbuf_cont(mioc) = mdata;
			asp_putnext(scb->gref, mioc);
			goto l_done;

		default:
			dPrintf(D_M_ASP, D_L_WARNING,
				("             : UNKNOWN func, func=%d\n",
				awp->func));

			break;
		}
	}

	else if (iocbp->ioc_cmd == AT_ATP_REQUEST_COMPLETE) {
		if (scb->next_scb) {
			/*
			 * find the responsible scb
			 */
			scb = (asp_scb_t *)iocbp->ioc_private;
			if ((scb == 0) || (scb->magic_num != 222)) {
				dPrintf(D_M_ASP, D_L_ERROR,
					("asp_ack_reply: CAN'T find scb 1\n"));
				gbuf_freem(mioc);
				return;
			}
		}
		dPrintf(D_M_ASP, D_L_INFO,
			("asp_ack_reply: RSP, loc=%d, rem=%x.%x.%d, state=%s\n",
			scb->loc_addr.socket,
			scb->rem_addr.net,
			scb->rem_addr.node,
			scb->rem_addr.socket,
			aspStateStr(scb->state)));

		switch (scb->state) {
		case ASPSTATE_Close:
		case ASPSTATE_Idle:
			scb->rem_addr.node = 0;
			gbuf_freem(mioc);
			ATDISABLE(s, scb->lock);
			if (scb->get_wait)
				thread_wakeup(&scb->event);
			else
				atalk_notify_sel(gref);
			ATENABLE(s, scb->lock);
			return;

		case ASPSTATE_WaitingForGetStatusRsp:
			scb->ioc_wait = 0;
			scb->state = ASPSTATE_Idle;
			mx = gbuf_cont(mioc);
			gbuf_cont(mioc) = 0;
			mdata = gbuf_cont(mx);
			gbuf_cont(mx) = 0;
			iocbp->ioc_cmd = ASPIOC_GetStatus;
			iocbp->ioc_count = 0;
			iocbp->ioc_rval = mdata ? gbuf_msgsize(mdata) : 0;
			gbuf_freeb(mx);
			atalk_putnext(gref, mioc);
			atalk_putnext(gref, mdata);
			return;

		case ASPSTATE_WaitingForOpenSessRsp:
			scb->ioc_wait = 0;
			scb->state = ASPSTATE_Idle;
			mx = gbuf_cont(mioc);
			gbuf_cont(mioc) = 0;
			if (gbuf_cont(mx)) {
				gbuf_freem(gbuf_cont(mx));
				gbuf_cont(mx) = 0;
			}
			iocbp->ioc_cmd = ASPIOC_OpenSession;
			iocbp->ioc_rval = 0;
			iocbp->ioc_count = 0;
			atpBDS = (struct atpBDS *)gbuf_rptr(mx);
			awp = (asp_word_t *)atpBDS->bdsUserData;
			if (awp->param2) {
				gbuf_freeb(mx);
				asp_iocnak(gref, mioc, ECONNREFUSED);
			} else {
				scb->rem_addr.node = scb->rem_node;
				scb->rem_addr.socket = awp->func;
				/* bms:  need to set the reply_socket for client side too.
				This makes ALO atten replies sent by the client work. */
				scb->reply_socket = scb->rem_addr.socket;
				scb->sess_id = awp->param1;
				gbuf_freeb(mx);
				atalk_putnext(gref, mioc);
				asp_timout(asp_hangup, scb, scb->session_timer);
				asp_send_tickle(scb);
				dPrintf(D_M_ASP, D_L_INFO,
					("asp_ack_reply: CONNECT, loc=%d, rem=%x.%x.%d\n",
					scb->loc_addr.socket,
					scb->rem_addr.net,
					scb->rem_addr.node,
					scb->rem_addr.socket));
			}
			return;

		case ASPSTATE_WaitingForCommandRsp:
		case ASPSTATE_WaitingForWriteRsp:
		case ASPSTATE_WaitingForWriteContinueRsp:
			if (scb->rem_addr.node)
				asp_untimout(asp_hangup, scb);
			NEXT_SEQ_NUM(scb->snd_seq_num);
			scb->state = ASPSTATE_Idle;
			gbuf_set_type(mioc, MSG_PROTO);
			mx = gbuf_cont(mioc);
			mdata = gbuf_cont(mx);
			gbuf_cont(mioc) = mdata;
			atpBDS = (struct atpBDS *)gbuf_rptr(mx);
			cmdreply_ind = (asp_cmdreply_ind_t *)gbuf_rptr(mioc);
			cmdreply_ind->Primitive = ASPFUNC_CmdReply;
			cmdreply_ind->CmdResult = *(int *)atpBDS->bdsUserData;
			gbuf_wset(mioc,sizeof(asp_cmdreply_ind_t));
			gbuf_freeb(mx);
			asp_putnext(scb->gref, mioc);
			goto l_done;

		case ASPSTATE_WaitingForCloseSessRsp:
			scb->ioc_wait = 0;
			scb->state = ASPSTATE_Close;
			scb->rem_addr.node = 0;
			iocbp->ioc_cmd = ASPIOC_CloseSession;
			iocbp->ioc_rval = 0;
			if (gbuf_cont(mioc)) {
				gbuf_freem(gbuf_cont(mioc));
				gbuf_cont(mioc) = 0;
			}
			atalk_putnext(scb->gref, mioc);
			atp_cancel_req(scb->gref, (unsigned int)scb->tickle_tid);
			scb->tickle_tid = 0;
			return;

		default:
			dPrintf(D_M_ASP, D_L_WARNING,
			("             : UNKNOWN state, state=%s\n", 
			 aspStateStr(scb->state)));
			break;
		}
	}

	else {
		if (scb->next_scb) {
			/*
			 * find the responsible scb
			 */
			scb = (asp_scb_t *)iocbp->ioc_cr;
			if ((scb == 0) || (scb->magic_num != 222)) {
				dPrintf(D_M_ASP, D_L_ERROR,
					("asp_ack_reply: CAN'T find scb 2\n"));
				gbuf_freem(mioc);
				return;
			}
		}

		switch (scb->state) {
		case ASPSTATE_Close:
			scb->rem_addr.node = 0;
			break;
		}
	}

	if (mioc != 0)
		gbuf_freem(mioc);

l_done:
	if (scb->rem_addr.node)
		asp_timout(asp_hangup, scb, scb->session_timer);
} /* asp_ack_reply */

/*
 * NAK reply routine
 */
void
asp_nak_reply(gref, mioc)
	register gref_t *gref;
	register gbuf_t *mioc;
{
	register asp_scb_t *scb;
	register ioc_t *iocbp;

	iocbp = (ioc_t *)gbuf_rptr(mioc);

	if (iocbp->ioc_cmd == AT_ATP_ISSUE_REQUEST_TICKLE) {
		/*
		 * no tickle, close session
		 */
		scb = (asp_scb_t *)iocbp->ioc_cr;
		gbuf_freem(mioc);
		asp_hangup(scb);
		dPrintf(D_M_ASP, D_L_WARNING,
			("tickle_nak: loc=%d, rem=%x.%x.%d, state=%s\n",
			scb->loc_addr.socket,
			scb->rem_addr.net,
			scb->rem_addr.node,
			scb->rem_addr.socket,
			aspStateStr(scb->state)));

		return;
	}

	scb = (asp_scb_t *)gref->info;
	if (scb == 0) {
		gbuf_freem(mioc);
		return;
	}

	if (iocbp->ioc_cmd == AT_ATP_REQUEST_COMPLETE) {
		if (scb->next_scb) {
			/*
			 * find the responsible scb
			 */
			scb = (asp_scb_t *)iocbp->ioc_private;
			if ((scb == 0) || (scb->magic_num != 222)) {
				dPrintf(D_M_ASP, D_L_ERROR,
					("asp_nak_reply: CAN'T find scb 1\n"));
				gbuf_freem(mioc);
				return;
			}
		}
		dPrintf(D_M_ASP, D_L_WARNING,
			("asp_nak_reply: RSP, loc=%d, rem=%x.%x.%d, state=%s\n",
			scb->loc_addr.socket,
			scb->rem_addr.net,
			scb->rem_addr.node,
			scb->rem_addr.socket,
			aspStateStr(scb->state)));

		switch (scb->state) {
		case ASPSTATE_WaitingForGetStatusRsp:
			iocbp->ioc_cmd = ASPIOC_GetStatus;
			break;

		case ASPSTATE_WaitingForOpenSessRsp:
			iocbp->ioc_cmd = ASPIOC_OpenSession;
			break;

		case ASPSTATE_WaitingForCommandRsp:
		case ASPSTATE_WaitingForWriteRsp:
		case ASPSTATE_WaitingForWriteContinueRsp:
			scb->state = ASPSTATE_Idle;

			/* last remaining use of MSG_ERROR */
			gbuf_set_type(mioc, MSG_ERROR);
			*gbuf_rptr(mioc) = (u_char)EPROTO;
			gbuf_wset(mioc, 1);
			if (gbuf_cont(mioc)) {
				gbuf_freem(gbuf_cont(mioc));
				gbuf_cont(mioc) = 0;
			}

			asp_putnext(scb->gref, mioc);
			return;

		case ASPSTATE_WaitingForCloseSessRsp:
			scb->state = ASPSTATE_Close;
			/* fall through */
		case ASPSTATE_Close: /* new for PR-2296832 */
			scb->rem_addr.node = 0;
			iocbp->ioc_cmd = ASPIOC_CloseSession;
			iocbp->ioc_rval = 0;
			if (gbuf_cont(mioc)) {
				gbuf_freem(gbuf_cont(mioc));
				gbuf_cont(mioc) = 0;
			}
			gbuf_set_type(mioc, MSG_IOCACK);
			atalk_putnext(scb->gref, mioc);
			return;

		default:
			gbuf_freem(mioc);
			return;
		}
		scb->state = ASPSTATE_Idle;
		atalk_putnext(gref, mioc);
	}

	else {
		if (scb->next_scb) {
			/*
			 * find the responsible scb
			 */
			scb = (asp_scb_t *)iocbp->ioc_cr;
			if ((scb == 0) || (scb->magic_num != 222)) {
				dPrintf(D_M_ASP, D_L_ERROR,
					("asp_nak_reply: CAN'T find scb 2\n"));
				gbuf_freem(mioc);
				return;
			}
		}

		switch (scb->state) {
		case ASPSTATE_Close:
			scb->rem_addr.node = 0;
			break;
		}

		gbuf_freem(mioc);
	}
} /* asp_nak_reply */

/*
 * delete scb from the use list
 */
StaticProc void
asp_dequeue_scb(scb)
	asp_scb_t *scb;
{
	int s;

	ATDISABLE(s, aspall_lock);
	if (scb == scb_used_list) {
		if ((scb_used_list = scb->next_scb) != 0)
			scb->next_scb->prev_scb = 0;
	} else {
		if ((scb->prev_scb->next_scb = scb->next_scb) != 0)
			scb->next_scb->prev_scb = scb->prev_scb;
	}
	ATENABLE(s, aspall_lock);

	scb->next_scb = 0;
	scb->prev_scb = 0;
}

/*
 * find scb routine
 */
StaticProc asp_scb_t *
asp_find_scb(sock_num, rem_addr)
	unsigned char sock_num;
	at_inet_t *rem_addr;
{
	int s;
	asp_scb_t *scb;
	asp_scb_t *alt_scb = 0;

	ATDISABLE(s, aspall_lock);
	for (scb = asp_scbQ[sock_num]; scb; scb = scb->next_scb) {
		if ((scb->rem_addr.net == rem_addr->net)
			&& (scb->rem_addr.node == rem_addr->node)) {
			if ((scb->rem_addr.socket == rem_addr->socket)
					|| (scb->rem_socket == rem_addr->socket))
				break;
			else if (alt_scb == 0)
				alt_scb = scb;
		}
	}

	if ((scb == 0) && ((scb = alt_scb) == 0)) {
		dPrintf(D_M_ASP, D_L_ERROR,
			("asp_find_scb: CAN'T find scb, loc=%d, rem=%x.%x.%d\n",
			sock_num,
			rem_addr->net,
			rem_addr->node,
			rem_addr->socket));
	}
	ATENABLE(s, aspall_lock);

	return scb;
}

/*
 * timout routine
 */
StaticProc void
asp_timout(func, scb, seconds)
	void (*func)();
	register asp_scb_t *scb;
	int seconds;
{
	int s;
	unsigned char sum;
	register asp_scb_t *curr_scb, *prev_scb;

	if (scb->tmo_func)
		return;

	scb->tmo_func = func;
	scb->tmo_delta = (seconds>>SESS_TMO_RES);
	scb->tmo_cnt = scb_tmo_cnt;

	ATDISABLE(s, asptmo_lock);
	if (scb_tmo_list == 0) {
		scb->next_tmo = scb->prev_tmo = 0;
		scb_tmo_list = scb;
		ATENABLE(s, asptmo_lock);
		return;
	}

	prev_scb = 0;
	curr_scb = scb_tmo_list;
	sum = 0;

	while (1) {
		sum += curr_scb->tmo_delta;
		if (sum > scb->tmo_delta) {
			sum -= curr_scb->tmo_delta;
			scb->tmo_delta -= sum;
			curr_scb->tmo_delta -= scb->tmo_delta;
			break;
		}
		prev_scb = curr_scb;
		if ((curr_scb = curr_scb->next_tmo) == 0) {
			scb->tmo_delta -= sum;
			break;
		}
	}

	if (prev_scb) {
		scb->prev_tmo = prev_scb;
		if ((scb->next_tmo = prev_scb->next_tmo) != 0)
			prev_scb->next_tmo->prev_tmo = scb;
		prev_scb->next_tmo = scb;
	} else {
		scb->prev_tmo = 0;
		scb->next_tmo = scb_tmo_list;
		scb_tmo_list->prev_tmo = scb;
		scb_tmo_list = scb;
	}
	ATENABLE(s, asptmo_lock);
}

/*
 * untimout routine
 */
StaticProc void
asp_untimout(func, scb)
	void (*func)();
	register asp_scb_t *scb;
{
	int s;

	if ((scb->tmo_cnt == scb_tmo_cnt) || (scb->tmo_func == 0))
		return;

	ATDISABLE(s, asptmo_lock);
	if (scb_tmo_list == scb) {
		if ((scb_tmo_list = scb->next_tmo) != 0) {
			scb_tmo_list->prev_tmo = 0;
			scb->next_tmo->tmo_delta += scb->tmo_delta;
		}
	} else if (scb->prev_tmo) {
		if ((scb->prev_tmo->next_tmo = scb->next_tmo) != 0) {
			scb->next_tmo->prev_tmo = scb->prev_tmo;
			scb->next_tmo->tmo_delta += scb->tmo_delta;
		}
		scb->prev_tmo = 0;
	}
	scb->tmo_func = 0;
	ATENABLE(s, asptmo_lock);
}

/*
 * hangup routine
 */
StaticProc void
asp_hangup(scb)
	asp_scb_t *scb;
{
	int s;

	/*
	 * set the state to Close
	 */
	ATDISABLE(s, scb->lock);
	scb->state = ASPSTATE_Close;
	if (scb->tickle_tid) {
		atp_cancel_req(scb->gref, (unsigned int)scb->tickle_tid);
		scb->tickle_tid = 0;
	}

	/*
	 * notify upstream of the hangup
	 */
	if (scb->rem_addr.node) {
		if (scb->get_wait) {
			thread_wakeup(&scb->event);
			ATENABLE(s, scb->lock);
		} else {
			ATENABLE(s, scb->lock);
			atalk_notify_sel(scb->gref);
		}
	} else
		ATENABLE(s, scb->lock);
}

StaticProc void
asp_iocack(gref, mioc)
	gref_t *gref;
	gbuf_t *mioc;
{
	if (gbuf_cont(mioc))
		((ioc_t *)gbuf_rptr(mioc))->ioc_count = gbuf_msgsize(gbuf_cont(mioc));
	else
		((ioc_t *)gbuf_rptr(mioc))->ioc_count = 0;

	gbuf_set_type(mioc, MSG_IOCACK);
	atalk_putnext(gref, mioc);
}

StaticProc void
asp_iocnak(gref, mioc, err)
	gref_t *gref;
	gbuf_t *mioc;
	int err;
{
	((ioc_t *)gbuf_rptr(mioc))->ioc_count = 0;
	if (err == 0)
		err = ENXIO;
	((ioc_t *)gbuf_rptr(mioc))->ioc_error = err;
	((ioc_t *)gbuf_rptr(mioc))->ioc_rval = -1;
	if (gbuf_cont(mioc)) {
		gbuf_freem(gbuf_cont(mioc));
		gbuf_cont(mioc) = 0;
	}

	gbuf_set_type(mioc, MSG_IOCNAK);
	atalk_putnext(gref, mioc);
}

/*
 * the alloc scb routine
 */
StaticProc asp_scb_t *
asp_scb_alloc()
{
	int s, i;
	gbuf_t *m;
	asp_scb_t *scb, *scb_array;

	ATDISABLE(s, aspall_lock);
	if (scb_free_list == 0) {
		if ((m = gbuf_alloc(SCBS_PER_BLK*sizeof(asp_scb_t), PRI_MED)) == 0)
		{
			ATENABLE(s, aspall_lock);
			return (asp_scb_t *)0;
		}
		bzero((char *)gbuf_rptr(m), SCBS_PER_BLK*sizeof(asp_scb_t));
		gbuf_cont(m) = scb_resource_m;
		scb_resource_m = m;
		scb_array = (asp_scb_t *)gbuf_rptr(m);
		for (i=0; i < SCBS_PER_BLK-1; i++)
			scb_array[i].next_scb = (asp_scb_t *)&scb_array[i+1];
		scb_array[i].next_scb = 0;
		scb_free_list = (asp_scb_t *)&scb_array[0];
	}

	scb = scb_free_list;
	scb_free_list = scb->next_scb;
	ATENABLE(s, aspall_lock);
	ATLOCKINIT(scb->lock);
	ATLOCKINIT(scb->delay_lock);
	ATEVENTINIT(scb->event);
	ATEVENTINIT(scb->delay_event);

	return scb;
}

/*
 * the free scb routine
 */
StaticProc void
asp_scb_free(scb)
	asp_scb_t *scb;
{
	int s;

	bzero((char *)scb, sizeof(asp_scb_t));
	ATDISABLE(s, aspall_lock);
	scb->next_scb = scb_free_list;
	scb_free_list = scb;
	ATENABLE(s, aspall_lock);
}

/*
 * routine to pass up receive data
 */
StaticProc void
asp_putnext(gref, mproto)
	gref_t *gref;
	gbuf_t *mproto;
{
	int s;
	gbuf_t *m;
	asp_scb_t *scb;

	scb = (asp_scb_t *)gref->info;

	/*
	 * queue the message.
	 */
	ATDISABLE(s, scb->lock);
	gbuf_next(mproto) = 0;
	if ((m = scb->sess_ioc) == 0)
		scb->sess_ioc = mproto;
	else {
		while (gbuf_next(m))
			m = gbuf_next(m);
		gbuf_next(m) = mproto;
	}
	scb->rcv_cnt++;
	if (scb->rcv_cnt >= MAX_RCV_CNT)
		scb->snd_stop = 1;

	if (scb->get_wait) {
		thread_wakeup(&scb->event);
		ATENABLE(s, scb->lock);
	} else if (mproto == scb->sess_ioc) {
		ATENABLE(s, scb->lock);
		atalk_notify_sel(gref);
	} else
		ATENABLE(s, scb->lock);
} /* asp_putnext */

/*
 * The following two routines are direct entries from system
 * calls to allow fast sending and recving of ASP data.
 */

/* in ASPputmsg we expect:

    ASPFUNC_CmdReply
    ASPFUNC_Attention
    ASPFUNC_Command
    ASPFUNC_Write
    ASPFUNC_WriteContinue
    
    bms:  Make this callable from the kernel.
    If mreq != NULL, then must be called from kernel space and the following apply:
    1)  *mreq is data to be sent already in mbuf chains.
    2)  datptr->len = size of data
*/

int ASPputmsg(gref_t *gref, strbuf_t *ctlptr, strbuf_t *datptr, gbuf_t *mreq, int flags, int *errp)
{
    int s, i, err, len;
    gbuf_t *mioc, *mdata, *mx;
    ioc_t *iocbp;
    strbuf_t ctlbuf;
    strbuf_t datbuf;
    asp_scb_t *scb;
    int nbds, result, msize, Primitive;
    unsigned char *wptr;
    struct atp_set_default *sd;
    at_ddp_t *ddp;
    at_atp_t *atp;
    struct atpBDS *atpBDS;
    asp_word_t *awp;
    union asp_primitives *primitives;
    unsigned short tid;
    
    if ((scb = (asp_scb_t *)gref->info) == 0) {
		dPrintf(D_M_ASP, D_L_ERROR,
			("ASPputmsg: stale handle=0x%x, pid=%d\n",
			(u_int) gref, gref->pid));

        *errp = EINVAL;
        return -1;
    }

    if (scb->state == ASPSTATE_Close)
        return 0;
    if (scb->snd_stop) {
        *errp = EAGAIN;
        return -1;
    }

    /*
     * copy in the control and data info
     */
     if (mreq != NULL) {
        /* being called from kernel space */
        bcopy (ctlptr, &ctlbuf, sizeof (strbuf_t));
        bcopy (datptr, &datbuf, sizeof (strbuf_t));
     } else {
        /* being called from user space */
        if ((err = copyin((caddr_t)ctlptr, (caddr_t)&ctlbuf, sizeof(ctlbuf))) != 0)
            goto l_err;
        if ((err = copyin((caddr_t)datptr, (caddr_t)&datbuf, sizeof(datbuf))) != 0)
            goto l_err;
     }

    /*
     * allocate buffer and copy in the control content
     */
    if (!(mioc = gbuf_alloc_wait(ctlbuf.len, TRUE))) {
        /* error return should not be possible */
        err = ENOBUFS;
        goto l_err;
    }
    gbuf_set_type(mioc, MSG_IOCTL); /* for later, in ATP */
    gbuf_wset(mioc, ctlbuf.len);
    
    if (mreq != NULL) {
        /* being called from kernel space */
        bcopy (ctlbuf.buf, gbuf_rptr(mioc), ctlbuf.len);
    } else {
        /* being called from user space */
        if ((err = copyin((caddr_t)ctlbuf.buf, (caddr_t)gbuf_rptr(mioc), ctlbuf.len)) != 0) {
            gbuf_freem(mioc);
            goto l_err;
        }
    }

    iocbp = (ioc_t *)gbuf_rptr(mioc);
    primitives = (union asp_primitives *)gbuf_rptr(mioc);
    Primitive = primitives->Primitive;
	dPrintf(D_M_ASP, D_L_INFO,
		("ASPputmsg: %s\n", aspCmdStr(Primitive)));

    /*
     * allocate buffer and copy in the data content
     */
    len = (Primitive == ASPFUNC_CmdReply) ? 0 : aspCMDsize;
    
    if (!(mdata = gbuf_alloc_wait(datbuf.len+len, TRUE))) {
        /* error return should not be possible */
        err = ENOBUFS;
        gbuf_freem(mioc);
        goto l_err;
    }
    gbuf_wset(mdata, (datbuf.len+len));
    gbuf_cont(mioc) = mdata;
    
    if (mreq != NULL) {
        /* being called from kernel space */
        gbuf_t *tmp = mreq;
        unsigned long offset = 0;
        
        /* copy afp cmd data from the passed in mbufs to mdata.  I cant
        chain mreq to mdata since the rest of this code assumes
        just one big mbuf with space in front for the BDS */
        offset = len;
        while (tmp != NULL) {
            bcopy (gbuf_rptr(tmp), (gbuf_rptr(mdata) + offset), gbuf_len(tmp));
            offset += gbuf_len(tmp);
            tmp = gbuf_cont(tmp);           /* on to next mbuf in chain */
        }
        
        /* all data copied out of mreq so free it */
        gbuf_freem(mreq);
    } else {
        /* being called from user space */
        if ((err = copyin((caddr_t)datbuf.buf,
                  (caddr_t)(gbuf_rptr(mdata)+len), datbuf.len)) != 0) {
            gbuf_freem(mioc);
            goto l_err;
        }
    }

    switch (Primitive) {

    case ASPFUNC_Command:
    case ASPFUNC_Write:
    case ASPFUNC_WriteContinue:
    case ASPFUNC_Attention:
        /*
         * build the command/write/write_continue request
         */
        wptr = gbuf_rptr(mdata);
        atpBDS = (struct atpBDS *)wptr;
        wptr += atpBDSsize;
        for (i=0; i < ATP_TRESP_MAX; i++) {
            *(unsigned long  *)atpBDS[i].bdsBuffAddr = 1;
            *(unsigned short *)atpBDS[i].bdsBuffSz = ATP_DATA_SIZE;
        }
        sd = (struct atp_set_default *)wptr;
        wptr += sizeof(struct atp_set_default);
        sd->def_retries = (scb->cmd_retry.retries == -1) ?
          ATP_INFINITE_RETRIES : scb->cmd_retry.retries;
        sd->def_rate = scb->cmd_retry.interval*TICKS_PER_SEC;
        sd->def_BDSlen = atpBDSsize;
        ddp = (at_ddp_t *)wptr;
        NET_ASSIGN(ddp->src_net, scb->loc_addr.net);
        ddp->src_node = scb->loc_addr.node;
        NET_ASSIGN(ddp->dst_net, scb->rem_addr.net);
        ddp->dst_node = scb->rem_addr.node;
        ddp->dst_socket = scb->rem_addr.socket;
        UAS_ASSIGN(ddp->checksum, 0);
        atp = ATP_ATP_HDR(wptr);
        wptr += TOTAL_ATP_HDR_SIZE;
        atp->xo = 1;
        atp->xo_relt = 1;
        atp->bitmap = 0xff;
        awp = (asp_word_t *)atp->user_bytes;
        awp->func = (unsigned char)Primitive;
        awp->param1 = scb->sess_id;
        awp->param2 = scb->snd_seq_num;
        iocbp->ioc_private = (void *)scb;
        iocbp->ioc_count = gbuf_len(mdata);
        iocbp->ioc_rval = 0;
        iocbp->ioc_cmd = AT_ATP_ISSUE_REQUEST_DEF;

        /*
         * send the command/write/write_continue/attention request
         */
        ATDISABLE(s, scb->lock);
        switch (awp->func) {
        case ASPFUNC_Command:
            scb->state = ASPSTATE_WaitingForCommandRsp;
            break;
        case ASPFUNC_Write:
            scb->state = ASPSTATE_WaitingForWriteRsp;
            break;
        case ASPFUNC_WriteContinue:
            scb->state = ASPSTATE_WaitingForWriteContinueRsp;
            awp->param2 = scb->wrt_seq_num;
            break;
        case ASPFUNC_Attention:
            scb->state = ASPSTATE_WaitingForCommandRsp;
            atp->xo = 0;
            atp->xo_relt = 0;
            atp->bitmap = 0x01;
            gbuf_wdec(mdata,2);
            awp->param2 = *(unsigned short *)gbuf_wptr(mdata);
            break;
        }
        ATENABLE(s, scb->lock);
        dPrintf(D_M_ASP,D_L_INFO,
            ("ASPputmsg: %s, loc=%d, rem=%x.%x.%d\n",
             (awp->func == ASPFUNC_Command ? "CommandReq" :
              awp->func == ASPFUNC_Write ? "WriteReq" :
              awp->func == ASPFUNC_WriteContinue ? "WriteContinue" :
              "AttentionReq"),scb->loc_addr.socket,
             NET_VALUE(ddp->dst_net),ddp->dst_node,ddp->dst_socket));
        atp_send_req(gref, mioc);
        return 0;

    case ASPFUNC_CmdReply:

        ATDISABLE(s, scb->lock);
        if (scb->req_msgq) {
            mx = scb->req_msgq;
            scb->req_msgq = gbuf_next(mx);
            gbuf_next(mx) = 0;
            ATENABLE(s, scb->lock);
            asp_putnext(scb->gref, mx);
        } else {
            scb->req_flag = 0;
            ATENABLE(s, scb->lock);
        }
        result = primitives->CmdReplyReq.CmdResult;
        tid = primitives->CmdReplyReq.ReqRefNum;

        /* Re-use the original mioc mbuf to send the response. */
        gbuf_rinc(mioc,sizeof(void *));
        gbuf_wset(mioc,0);
        ddp = (at_ddp_t *)gbuf_wptr(mioc);
        gbuf_winc(mioc,DDP_X_HDR_SIZE);
        atp = (at_atp_t *)gbuf_wptr(mioc);
        gbuf_winc(mioc,ATP_HDR_SIZE);
        NET_ASSIGN(ddp->src_net, scb->loc_addr.net);
        ddp->src_node = scb->loc_addr.node;
        NET_ASSIGN(ddp->dst_net, scb->rem_addr.net);
        ddp->dst_node = scb->rem_addr.node;
        ddp->dst_socket = scb->reply_socket;
        ddp->type = DDP_ATP;
        UAS_ASSIGN(ddp->checksum, 0);
        UAS_ASSIGN(atp->tid, tid);
        if (scb->attn_flag && (tid == scb->attn_tid)) {
           scb->attn_flag = 0;
            atp->xo = 0;
            atp->xo_relt = 0;
        } else {
            atp->xo = 1;
            atp->xo_relt = 1;
        }
        atpBDS = (struct atpBDS *)gbuf_wptr(mioc);
        msize = mdata ? gbuf_msgsize(mdata) : 0;
       for (nbds=0; (nbds < ATP_TRESP_MAX) && (msize > 0); nbds++) {
            len = msize < ATP_DATA_SIZE ? msize : ATP_DATA_SIZE;
            msize -= ATP_DATA_SIZE;
            *(long *)atpBDS[nbds].bdsUserData = 0;
            UAL_ASSIGN(atpBDS[nbds].bdsBuffAddr, 1);
            UAS_ASSIGN(atpBDS[nbds].bdsBuffSz, len);
        }
        UAS_ASSIGN(atpBDS[0].bdsDataSz, nbds);
        *(long *)atpBDS[0].bdsUserData = (long)result;
        *(long *)atp->user_bytes = (long)result;
        gbuf_winc(mioc,atpBDSsize);
		dPrintf(D_M_ASP, D_L_INFO,
			("ASPputmsg: ATP CmdReplyReq, loc=%d, state=%s, msgsize = %d, result = %d, tid = %d\n",
			 scb->loc_addr.socket, aspStateStr(scb->state), 
			 (mdata ? gbuf_msgsize(mdata) : 0), result, tid));
        atp_send_rsp(gref, mioc, TRUE);
        return 0;
    }

    /* Not an expected ASPFUNC */
    gbuf_freem(mioc);
    err = EOPNOTSUPP;

l_err:
    *errp = err;
    return -1;
} /* ASPputmsg */


/* bms:  make this callable from kernel.  reply date is passed back as a mbuf chain in *mreply  */
int ASPgetmsg(gref_t *gref, strbuf_t *ctlptr, strbuf_t *datptr, gbuf_t **mreply, int *flags, int *errp)
{
    int err, s, len, sum, rval;
    gbuf_t *mproto, *mdata;
    strbuf_t ctlbuf;
    strbuf_t datbuf;
    asp_scb_t *scb;
    unsigned char get_wait;

    if ((scb = (asp_scb_t *)gref->info) == 0) {
		dPrintf(D_M_ASP, D_L_ERROR,
			("ASPgetmsg: stale handle=0x%x, pid=%d\n",
			(u_int) gref, gref->pid));

		*errp = EINVAL;
		return -1;
	}

    ATDISABLE(s, scb->lock);
    if (scb->state == ASPSTATE_Close) {
        ATENABLE(s, scb->lock);
        return 0;
    }

    /*
     * get receive data
     */
    while ((mproto = scb->sess_ioc) == 0) {
        scb->get_wait = 1;
        err = tsleep(&scb->event, PSOCK | PCATCH, "aspgetmsg", 0);
        if (err != 0) {
            scb->get_wait = 0;
            ATENABLE(s, scb->lock);
            *errp = err;
            return -1;
        }
        if (scb->state == ASPSTATE_Close) {
            scb->get_wait = 0;
            ATENABLE(s, scb->lock);
            return 0;
        }
    }
    get_wait = scb->get_wait;
    scb->get_wait = 0;
    if ((ctlptr == 0) && (datptr == 0)) {
        ATENABLE(s, scb->lock);
        return 0;
    }
    scb->sess_ioc = gbuf_next(mproto);
    mdata = gbuf_cont(mproto);
    ATENABLE(s, scb->lock);

    /* last remaining use of MSG_ERROR */
    if (gbuf_type(mproto) == MSG_ERROR) {
        err = (int)gbuf_rptr(mproto)[0];
        goto l_err;
    }

    /*
     * copy in the control and data info
     */
    if (mreply != NULL) {
        /* called from kernel space */
        bcopy (ctlptr, &ctlbuf, sizeof(ctlbuf));
        bcopy (datptr, &datbuf, sizeof(datbuf));
    } else {
        /* called from user space */
        if ((err = copyin((caddr_t)ctlptr,
                (caddr_t)&ctlbuf, sizeof(ctlbuf))) != 0)
            goto l_err;
        if ((err = copyin((caddr_t)datptr,
                (caddr_t)&datbuf, sizeof(datbuf))) != 0)
            goto l_err;
    }
    if ((datbuf.maxlen < 0) || (datbuf.maxlen < gbuf_msgsize(mdata))) {
        ATDISABLE(s, scb->lock);
        gbuf_next(mproto) = scb->sess_ioc;
        scb->sess_ioc = mproto;
        ATENABLE(s, scb->lock);
        return MOREDATA;
    }

    if (get_wait == 0) {
        /*
         * this is a hack to support the select() call.
         * we're not supposed to dequeue messages in the Streams 
         * head's read queue this way; but there is no better way.
         */
        ATDISABLE(s, scb->lock);
        if (scb->sess_ioc == 0) {
            ATENABLE(s, scb->lock);
        } else {
            ATENABLE(s, scb->lock);
            atalk_notify_sel(gref); 
        }
    }

    /*
     * copy out the control content and info
     */
    ctlbuf.len = gbuf_len(mproto);

    if (mreply != NULL) {
        /* called from kernel space */
        bcopy (gbuf_rptr(mproto), ctlbuf.buf, ctlbuf.len);
        bcopy (&ctlbuf, ctlptr, sizeof(ctlbuf));
    } else {
        /* called from user space */
        if ((err = copyout((caddr_t)gbuf_rptr(mproto),
                (caddr_t)ctlbuf.buf, ctlbuf.len)) != 0)
            goto l_err;
        if ((err = copyout((caddr_t)&ctlbuf,
                (caddr_t)ctlptr, sizeof(ctlbuf))) != 0)
            goto l_err;
    }

    /*
     * copy out the data content and info
     */
    for (rval = 0, sum = 0; mdata && (rval == 0); mdata = gbuf_cont(mdata)) 
    {
        len = gbuf_len(mdata);
        if (len) {
            if ((len + sum) > datbuf.maxlen) {
                len = datbuf.maxlen - sum;
                rval = MOREDATA;
            }
            
            if (mreply == NULL) {
                /* called from user space */
                if ((err = copyout((caddr_t)gbuf_rptr(mdata), (caddr_t)&datbuf.buf[sum], len)) != 0)
                    goto l_err;
            }
            sum += len;
        }
    }
    datbuf.len = sum;
    if (mreply != NULL) {
        /* called from kernel space */
        bcopy (&datbuf, datptr, sizeof(datbuf));
    } else {
        /* called from user space */
        if ((err = copyout((caddr_t)&datbuf, (caddr_t)datptr, sizeof(datbuf))) != 0)
            goto l_err;
    }
    
    if (mreply != NULL) {
        /* called from kernel space */
        /* return the reply data in mbufs, so dont free them.  
        Just free the proto info */
        mdata = gbuf_cont(mproto);
        *mreply = mdata;
        gbuf_cont(mproto) = NULL;
        gbuf_freem(mproto);
    } else {
        /* called from user space */
        gbuf_freem(mproto);
    }

    ATDISABLE(s, scb->lock);
    if (scb->sess_ioc)
        scb->rcv_cnt--;
    else {
        scb->rcv_cnt = 0;
        scb->snd_stop = 0;
    }
    ATENABLE(s, scb->lock);
    return rval;

l_err:
    ATDISABLE(s, scb->lock);
    gbuf_next(mproto) = scb->sess_ioc;
    scb->sess_ioc = mproto;
    ATENABLE(s, scb->lock);
    *errp = err;
    return -1;
}

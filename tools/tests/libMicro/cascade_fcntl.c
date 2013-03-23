/*
 * CDDL HEADER START
 *
 * The contents of this file are subject to the terms
 * of the Common Development and Distribution License
 * (the "License").  You may not use this file except
 * in compliance with the License.
 *
 * You can obtain a copy of the license at
 * src/OPENSOLARIS.LICENSE
 * or http://www.opensolaris.org/os/licensing.
 * See the License for the specific language governing
 * permissions and limitations under the License.
 *
 * When distributing Covered Code, include this CDDL
 * HEADER in each file and include the License file at
 * usr/src/OPENSOLARIS.LICENSE.  If applicable,
 * add the following below this CDDL HEADER, with the
 * fields enclosed by brackets "[]" replaced with your
 * own identifying information: Portions Copyright [yyyy]
 * [name of copyright owner]
 *
 * CDDL HEADER END
 */

/*
 * Copyright 2005 Sun Microsystems, Inc.  All rights reserved.
 * Use is subject to license terms.
 */

/*
 * The "cascade" test case is a multiprocess/multithread batten-passing model
 * using lock primitives alone for synchronisation. Threads are arranged in a
 * ring. Each thread has two locks of its own on which it blocks, and is able
 * to manipulate the two locks belonging to the thread which follows it in the
 * ring.
 *
 * The number of threads (nthreads) is specified by the generic libMicro -P/-T
 * options. With nthreads == 1 (the default) the uncontended case can be timed.
 *
 * The main logic is generic and allows any simple blocking API to be tested.
 * The API-specific component is clearly indicated.
 */

#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <fcntl.h>

#include "libmicro.h"

typedef struct {
	int			ts_once;
	int			ts_id;
	int			ts_us0;		/* our lock indices */
	int			ts_us1;
	int			ts_them0;	/* their lock indices */
	int			ts_them1;
} tsd_t;

static int			nthreads;

/*
 * API-specific code BEGINS here
 */

#define	DEFD			"/private/tmp"

static char			*optd = DEFD;
static int 			file;
static int 			nlocks;

int
benchmark_init()
{
	lm_tsdsize = sizeof (tsd_t);

	(void) sprintf(lm_optstr, "d:");

	lm_defN = "cscd_fcntl";

	(void) sprintf(lm_usage,
	    "       [-d directory for temp file (default %s)]\n"
	    "notes: thread cascade using fcntl region locking\n",
	    DEFD);

	return (0);
}

int
benchmark_optswitch(int opt, char *optarg)
{
	switch (opt) {
	case 'd':
		optd = optarg;
		break;
	default:
		return (-1);
	}
	return (0);
}

int
benchmark_initrun()
{
	int			errors = 0;
	char			fname[1024];

	nthreads = lm_optP * lm_optT;
	nlocks = nthreads * 2;

	(void) sprintf(fname, "%s/cascade.%ld", optd, getpid());

	file = open(fname, O_CREAT | O_TRUNC | O_RDWR, 0600);
	if (file == -1) {
		errors++;
	}

	if (unlink(fname)) {
		errors++;
	}

	if (ftruncate(file, nlocks * 3) == -1) {
		errors++;
	}

	return (errors);
}

int
block(int index)
{
	struct flock		fl;

	fl.l_type = F_WRLCK;
	fl.l_whence = SEEK_SET;
	fl.l_start = index;
	fl.l_len = 1;
	return (fcntl(file, F_SETLKW, &fl) == -1);
}

int
unblock(int index)
{
	struct flock		fl;

	fl.l_type = F_UNLCK;
	fl.l_whence = SEEK_SET;
	fl.l_start = index;
	fl.l_len = 1;
	return (fcntl(file, F_SETLK, &fl) == -1);
}

/*
 * API-specific code ENDS here
 */

int
benchmark_initbatch(void *tsd)
{
	tsd_t			*ts = (tsd_t *)tsd;
	int			e = 0;

	if (ts->ts_once == 0) {
		int		us, them;

#if !defined(__APPLE__)
        us = (getpindex() * lm_optT) + gettindex();
#else
        us = gettsdindex(tsd);
#endif /* __APPLE__ */
	
		them = (us + 1) % (lm_optP * lm_optT);

		ts->ts_id = us;

		/* lock index asignment for us and them */
		ts->ts_us0 = (us * 4);
		ts->ts_us1 = (us * 4) + 2;
		if (us < nthreads - 1) {
			/* straight-thru connection to them */
			ts->ts_them0 = (them * 4);
			ts->ts_them1 = (them * 4) + 2;
		} else {
			/* cross-over connection to them */
			ts->ts_them0 = (them * 4) + 2;
			ts->ts_them1 = (them * 4);
		}

		ts->ts_once = 1;
	}

	/* block their first move */
	e += block(ts->ts_them0);

	return (e);
}

int
benchmark(void *tsd, result_t *res)
{
	tsd_t			*ts = (tsd_t *)tsd;
	int			i;
	int			e = 0;

	/* wait to be unblocked (id == 0 will not block) */
	e += block(ts->ts_us0);

	for (i = 0; i < lm_optB; i += 2) {
		/* allow them to block us again */
		e += unblock(ts->ts_us0);

		/* block their next + 1 move */
		e += block(ts->ts_them1);

		/* unblock their next move */
		e += unblock(ts->ts_them0);

		/* wait for them to unblock us */
		e += block(ts->ts_us1);

		/* repeat with locks reversed */
		e += unblock(ts->ts_us1);
		e += block(ts->ts_them0);
		e += unblock(ts->ts_them1);
		e += block(ts->ts_us0);
	}

	/* finish batch with nothing blocked */
	e += unblock(ts->ts_them0);
	e += unblock(ts->ts_us0);

	res->re_count = i;
	res->re_errors = e;

	return (0);
}

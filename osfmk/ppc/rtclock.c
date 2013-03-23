/*
 * Copyright (c) 2000 Apple Computer, Inc. All rights reserved.
 *
 * @APPLE_LICENSE_HEADER_START@
 * 
 * Copyright (c) 1999-2003 Apple Computer, Inc.  All Rights Reserved.
 * 
 * This file contains Original Code and/or Modifications of Original Code
 * as defined in and that are subject to the Apple Public Source License
 * Version 2.0 (the 'License'). You may not use this file except in
 * compliance with the License. Please obtain a copy of the License at
 * http://www.opensource.apple.com/apsl/ and read it before using this
 * file.
 * 
 * The Original Code and all software distributed under the License are
 * distributed on an 'AS IS' basis, WITHOUT WARRANTY OF ANY KIND, EITHER
 * EXPRESS OR IMPLIED, AND APPLE HEREBY DISCLAIMS ALL SUCH WARRANTIES,
 * INCLUDING WITHOUT LIMITATION, ANY WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE, QUIET ENJOYMENT OR NON-INFRINGEMENT.
 * Please see the License for the specific language governing rights and
 * limitations under the License.
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
 *	File:		rtclock.c
 *	Purpose:	Routines for handling the machine dependent
 *				real-time clock.
 */

#include <libkern/OSTypes.h>

#include <mach/mach_types.h>

#include <kern/clock.h>
#include <kern/thread.h>
#include <kern/macro_help.h>
#include <kern/spl.h>

#include <machine/mach_param.h>	/* HZ */
#include <ppc/proc_reg.h>

#include <pexpert/pexpert.h>

#include <sys/kdebug.h>

int		sysclk_config(void);

int		sysclk_init(void);

kern_return_t	sysclk_gettime(
	mach_timespec_t			*cur_time);

kern_return_t	sysclk_getattr(
	clock_flavor_t			flavor,
	clock_attr_t			attr,
	mach_msg_type_number_t	*count);

void		sysclk_setalarm(
	mach_timespec_t			*deadline);

struct clock_ops sysclk_ops = {
	sysclk_config,			sysclk_init,
	sysclk_gettime,			0,
	sysclk_getattr,			0,
	sysclk_setalarm,
};

int		calend_config(void);

int		calend_init(void);

kern_return_t	calend_gettime(
	mach_timespec_t			*cur_time);

kern_return_t	calend_settime(
	mach_timespec_t			*cur_time);

kern_return_t	calend_getattr(
	clock_flavor_t			flavor,
	clock_attr_t			attr,
	mach_msg_type_number_t	*count);

struct clock_ops calend_ops = {
	calend_config,			calend_init,
	calend_gettime,			calend_settime,
	calend_getattr,			0,
	0,
};

/* local data declarations */

static struct rtclock {
	mach_timespec_t		calend_offset;
	boolean_t			calend_is_set;

	mach_timebase_info_data_t	timebase_const;

	struct rtclock_timer {
		uint64_t			deadline;
		boolean_t			is_set;
	}					timer[NCPUS];

	clock_timer_func_t	timer_expire;

	timer_call_data_t	alarm_timer;

	/* debugging */
	uint64_t			last_abstime[NCPUS];
	int					last_decr[NCPUS];

	decl_simple_lock_data(,lock)	/* real-time clock device lock */
} rtclock;

static boolean_t		rtclock_initialized;

static uint64_t			rtclock_tick_deadline[NCPUS];
static uint64_t		 	rtclock_tick_interval;

static void		timespec_to_absolutetime(
							mach_timespec_t		timespec,
							uint64_t			*result);

static int		deadline_to_decrementer(
							uint64_t			deadline,
							uint64_t			now);

static void		rtclock_alarm_timer(
					timer_call_param_t		p0,
					timer_call_param_t		p1);

/* global data declarations */

#define RTC_TICKPERIOD	(NSEC_PER_SEC / HZ)

#define DECREMENTER_MAX		0x7FFFFFFFUL
#define DECREMENTER_MIN		0xAUL

natural_t		rtclock_decrementer_min;

/*
 *	Macros to lock/unlock real-time clock device.
 */
#define LOCK_RTC(s)					\
MACRO_BEGIN							\
	(s) = splclock();				\
	simple_lock(&rtclock.lock);		\
MACRO_END

#define UNLOCK_RTC(s)				\
MACRO_BEGIN							\
	simple_unlock(&rtclock.lock);	\
	splx(s);						\
MACRO_END

static void
timebase_callback(
	struct timebase_freq_t	*freq)
{
	natural_t	numer, denom;
	int			n;
	spl_t		s;

	denom = freq->timebase_num;
	n = 9;
	while (!(denom % 10)) {
		if (n < 1)
			break;
		denom /= 10;
		n--;
	}

	numer = freq->timebase_den;
	while (n-- > 0) {
		numer *= 10;
	}

	LOCK_RTC(s);
	rtclock.timebase_const.numer = numer;
	rtclock.timebase_const.denom = denom;
	UNLOCK_RTC(s);
}

/*
 * Configure the real-time clock device.
 */
int
sysclk_config(void)
{
	if (cpu_number() != master_cpu)
		return(1);

	timer_call_setup(&rtclock.alarm_timer, rtclock_alarm_timer, NULL);

	simple_lock_init(&rtclock.lock, ETAP_MISC_RT_CLOCK);

	PE_register_timebase_callback(timebase_callback);

	return (1);
}

/*
 * Initialize the system clock device.
 */
int
sysclk_init(void)
{
	uint64_t		abstime;
	int				decr, mycpu = cpu_number();

	if (mycpu != master_cpu) {
		if (rtclock_initialized == FALSE) {
			panic("sysclk_init on cpu %d, rtc not initialized\n", mycpu);
		}
		/* Set decrementer and hence our next tick due */
		clock_get_uptime(&abstime);
		rtclock_tick_deadline[mycpu] = abstime;
		rtclock_tick_deadline[mycpu] += rtclock_tick_interval;
		decr = deadline_to_decrementer(rtclock_tick_deadline[mycpu], abstime);
		mtdec(decr);
		rtclock.last_decr[mycpu] = decr;

		return(1);
	}

	/*
	 * Initialize non-zero clock structure values.
	 */
	clock_interval_to_absolutetime_interval(RTC_TICKPERIOD, 1,
												&rtclock_tick_interval);
	/* Set decrementer and our next tick due */
	clock_get_uptime(&abstime);
	rtclock_tick_deadline[mycpu] = abstime;
	rtclock_tick_deadline[mycpu] += rtclock_tick_interval;
	decr = deadline_to_decrementer(rtclock_tick_deadline[mycpu], abstime);
	mtdec(decr);
	rtclock.last_decr[mycpu] = decr;

	rtclock_initialized = TRUE;

	return (1);
}

#define UnsignedWide_to_scalar(x)	(*(uint64_t *)(x))
#define scalar_to_UnsignedWide(x)	(*(UnsignedWide *)(x))

/*
 * Perform a full 64 bit by 32 bit unsigned multiply,
 * yielding a 96 bit product.  The most significant
 * portion of the product is returned as a 64 bit
 * quantity, with the lower portion as a 32 bit word.
 */
static void
umul_64by32(
	UnsignedWide 		now64,
	uint32_t			mult32,
	UnsignedWide		*result64,
	uint32_t			*result32)
{
	uint32_t			mid, mid2;

	asm volatile("	mullw %0,%1,%2" :
				 			"=r" (*result32) :
					 			"r" (now64.lo), "r" (mult32));

	asm volatile("	mullw %0,%1,%2" :
							"=r" (mid2) :
					 			"r" (now64.hi), "r" (mult32));
	asm volatile("	mulhwu %0,%1,%2" :
							"=r" (mid) :
					 			"r" (now64.lo), "r" (mult32));

	asm volatile("	mulhwu %0,%1,%2" :
							"=r" (result64->hi) :
					 			"r" (now64.hi), "r" (mult32));

	asm volatile("	addc %0,%2,%3;
					addze %1,%4" :
							"=r" (result64->lo), "=r" (result64->hi) :
								"r" (mid), "r" (mid2), "1" (result64->hi));
}

/*
 * Perform a partial 64 bit by 32 bit unsigned multiply,
 * yielding a 64 bit product.  Only the least significant
 * 64 bits of the product are calculated and returned.
 */
static void
umul_64by32to64(
	UnsignedWide		now64,
	uint32_t			mult32,
	UnsignedWide		*result64)
{
	uint32_t			mid, mid2;

	asm volatile("	mullw %0,%1,%2" :
				 			"=r" (result64->lo) :
					 			"r" (now64.lo), "r" (mult32));

	asm volatile("	mullw %0,%1,%2" :
							"=r" (mid2) :
					 			"r" (now64.hi), "r" (mult32));
	asm volatile("	mulhwu %0,%1,%2" :
							"=r" (mid) :
					 			"r" (now64.lo), "r" (mult32));

	asm volatile("	add %0,%1,%2" :
							"=r" (result64->hi) :
								"r" (mid), "r" (mid2));
}

/*
 * Perform an unsigned division of a 96 bit value
 * by a 32 bit value, yielding a 96 bit quotient.
 * The most significant portion of the product is
 * returned as a 64 bit quantity, with the lower
 * portion as a 32 bit word.
 */
static void
udiv_96by32(
	UnsignedWide	now64,
	uint32_t		now32,
	uint32_t		div32,
	UnsignedWide	*result64,
	uint32_t		*result32)
{
	UnsignedWide	t64;

	if (now64.hi > 0 || now64.lo >= div32) {
		UnsignedWide_to_scalar(result64) =
							UnsignedWide_to_scalar(&now64) / div32;

		umul_64by32to64(*result64, div32, &t64);

		UnsignedWide_to_scalar(&t64) =
				UnsignedWide_to_scalar(&now64) - UnsignedWide_to_scalar(&t64);

		*result32 =	(((uint64_t)t64.lo << 32) | now32) / div32;
	}
	else {
		UnsignedWide_to_scalar(result64) =
					(((uint64_t)now64.lo << 32) | now32) / div32;

		*result32 = result64->lo;
		result64->lo = result64->hi;
		result64->hi = 0;
	}
}

/*
 * Perform an unsigned division of a 96 bit value
 * by a 32 bit value, yielding a 64 bit quotient.
 * Any higher order bits of the quotient are simply
 * discarded.
 */
static void
udiv_96by32to64(
	UnsignedWide	now64,
	uint32_t		now32,
	uint32_t		div32,
	UnsignedWide	*result64)
{
	UnsignedWide	t64;

	if (now64.hi > 0 || now64.lo >= div32) {
		UnsignedWide_to_scalar(result64) =
						UnsignedWide_to_scalar(&now64) / div32;

		umul_64by32to64(*result64, div32, &t64);

		UnsignedWide_to_scalar(&t64) =
				UnsignedWide_to_scalar(&now64) - UnsignedWide_to_scalar(&t64);

		result64->hi = result64->lo;
		result64->lo = (((uint64_t)t64.lo << 32) | now32) / div32;
	}
	else {
		UnsignedWide_to_scalar(result64) =
						(((uint64_t)now64.lo << 32) | now32) / div32;
	}
}

/*
 * Perform an unsigned division of a 96 bit value
 * by a 32 bit value, yielding a 32 bit quotient,
 * and a 32 bit remainder.  Any higher order bits
 * of the quotient are simply discarded.
 */
static void
udiv_96by32to32and32(
	UnsignedWide	now64,
	uint32_t		now32,
	uint32_t		div32,
	uint32_t		*result32,
	uint32_t		*remain32)
{
	UnsignedWide	t64, u64;

	if (now64.hi > 0 || now64.lo >= div32) {
		UnsignedWide_to_scalar(&t64) =
							UnsignedWide_to_scalar(&now64) / div32;

		umul_64by32to64(t64, div32, &t64);

		UnsignedWide_to_scalar(&t64) =
			UnsignedWide_to_scalar(&now64) - UnsignedWide_to_scalar(&t64);

		UnsignedWide_to_scalar(&t64) = ((uint64_t)t64.lo << 32) | now32;

		UnsignedWide_to_scalar(&u64) =
							UnsignedWide_to_scalar(&t64) / div32;

		*result32 = u64.lo;

		umul_64by32to64(u64, div32, &u64);

		*remain32 = UnsignedWide_to_scalar(&t64) -
									UnsignedWide_to_scalar(&u64);
	}
	else {
		UnsignedWide_to_scalar(&t64) = ((uint64_t)now64.lo << 32) | now32;

		UnsignedWide_to_scalar(&u64) =
							UnsignedWide_to_scalar(&t64) / div32;

		*result32 =	 u64.lo;

		umul_64by32to64(u64, div32, &u64);

		*remain32 =	UnsignedWide_to_scalar(&t64) -
									UnsignedWide_to_scalar(&u64);
	}
}

/*
 * Get the clock device time. This routine is responsible
 * for converting the device's machine dependent time value
 * into a canonical mach_timespec_t value.
 *
 * SMP configurations - *the processor clocks are synchronised*
 */
kern_return_t
sysclk_gettime_internal(
	mach_timespec_t	*time)	/* OUT */
{
	UnsignedWide		now;
	UnsignedWide		t64;
	uint32_t			t32;
	uint32_t			numer, denom;

	numer = rtclock.timebase_const.numer;
	denom = rtclock.timebase_const.denom;

	clock_get_uptime((uint64_t *)&now);

	umul_64by32(now, numer, &t64, &t32);

	udiv_96by32(t64, t32, denom, &t64, &t32);

	udiv_96by32to32and32(t64, t32, NSEC_PER_SEC,
								&time->tv_sec, &time->tv_nsec);

	return (KERN_SUCCESS);
}

kern_return_t
sysclk_gettime(
	mach_timespec_t	*time)	/* OUT */
{
	UnsignedWide		now;
	UnsignedWide		t64;
	uint32_t			t32;
	uint32_t			numer, denom;
	spl_t				s;

	LOCK_RTC(s);
	numer = rtclock.timebase_const.numer;
	denom = rtclock.timebase_const.denom;
	UNLOCK_RTC(s);

	clock_get_uptime((uint64_t *)&now);

	umul_64by32(now, numer, &t64, &t32);

	udiv_96by32(t64, t32, denom, &t64, &t32);

	udiv_96by32to32and32(t64, t32, NSEC_PER_SEC,
								&time->tv_sec, &time->tv_nsec);

	return (KERN_SUCCESS);
}

/*
 * Get clock device attributes.
 */
kern_return_t
sysclk_getattr(
	clock_flavor_t		flavor,
	clock_attr_t		attr,		/* OUT */
	mach_msg_type_number_t	*count)		/* IN/OUT */
{
	spl_t	s;

	if (*count != 1)
		return (KERN_FAILURE);
	switch (flavor) {

	case CLOCK_GET_TIME_RES:	/* >0 res */
	case CLOCK_ALARM_CURRES:	/* =0 no alarm */
	case CLOCK_ALARM_MINRES:
	case CLOCK_ALARM_MAXRES:
		LOCK_RTC(s);
		*(clock_res_t *) attr = RTC_TICKPERIOD;
		UNLOCK_RTC(s);
		break;

	default:
		return (KERN_INVALID_VALUE);
	}
	return (KERN_SUCCESS);
}

/*
 * Set deadline for the next alarm on the clock device. This call
 * always resets the time to deliver an alarm for the clock.
 */
void
sysclk_setalarm(
	mach_timespec_t		*deadline)
{
	uint64_t			abstime;

	timespec_to_absolutetime(*deadline, &abstime);
	timer_call_enter(&rtclock.alarm_timer, abstime);
}

/*
 * Configure the calendar clock.
 */
int
calend_config(void)
{
	return (1);
}

/*
 * Initialize the calendar clock.
 */
int
calend_init(void)
{
	if (cpu_number() != master_cpu)
		return(1);

	return (1);
}

/*
 * Get the current clock time.
 */
kern_return_t
calend_gettime(
	mach_timespec_t	*curr_time)	/* OUT */
{
	spl_t		s;

	LOCK_RTC(s);
	if (!rtclock.calend_is_set) {
		UNLOCK_RTC(s);
		return (KERN_FAILURE);
	}

	(void) sysclk_gettime_internal(curr_time);
	ADD_MACH_TIMESPEC(curr_time, &rtclock.calend_offset);
	UNLOCK_RTC(s);

	return (KERN_SUCCESS);
}

/*
 * Set the current clock time.
 */
kern_return_t
calend_settime(
	mach_timespec_t	*new_time)
{
	mach_timespec_t	curr_time;
	spl_t		s;

	LOCK_RTC(s);
	(void) sysclk_gettime_internal(&curr_time);
	rtclock.calend_offset = *new_time;
	SUB_MACH_TIMESPEC(&rtclock.calend_offset, &curr_time);
	rtclock.calend_is_set = TRUE;
	UNLOCK_RTC(s);

	PESetGMTTimeOfDay(new_time->tv_sec);

	return (KERN_SUCCESS);
}

/*
 * Get clock device attributes.
 */
kern_return_t
calend_getattr(
	clock_flavor_t		flavor,
	clock_attr_t		attr,		/* OUT */
	mach_msg_type_number_t	*count)		/* IN/OUT */
{
	spl_t	s;

	if (*count != 1)
		return (KERN_FAILURE);
	switch (flavor) {

	case CLOCK_GET_TIME_RES:	/* >0 res */
		LOCK_RTC(s);
		*(clock_res_t *) attr = RTC_TICKPERIOD;
		UNLOCK_RTC(s);
		break;

	case CLOCK_ALARM_CURRES:	/* =0 no alarm */
	case CLOCK_ALARM_MINRES:
	case CLOCK_ALARM_MAXRES:
		*(clock_res_t *) attr = 0;
		break;

	default:
		return (KERN_INVALID_VALUE);
	}
	return (KERN_SUCCESS);
}

void
clock_adjust_calendar(
	clock_res_t	nsec)
{
	spl_t		s;

	LOCK_RTC(s);
	if (rtclock.calend_is_set)
		ADD_MACH_TIMESPEC_NSEC(&rtclock.calend_offset, nsec);
	UNLOCK_RTC(s);
}

void
clock_initialize_calendar(void)
{
	mach_timespec_t		curr_time;
	long				seconds = PEGetGMTTimeOfDay();
	spl_t				s;

	LOCK_RTC(s);
	(void) sysclk_gettime_internal(&curr_time);
	if (curr_time.tv_nsec < 500*USEC_PER_SEC)
		rtclock.calend_offset.tv_sec = seconds;
	else
		rtclock.calend_offset.tv_sec = seconds + 1;
	rtclock.calend_offset.tv_nsec = 0;
	SUB_MACH_TIMESPEC(&rtclock.calend_offset, &curr_time);
	rtclock.calend_is_set = TRUE;
	UNLOCK_RTC(s);
}

mach_timespec_t
clock_get_calendar_offset(void)
{
	mach_timespec_t	result = MACH_TIMESPEC_ZERO;
	spl_t		s;

	LOCK_RTC(s);
	if (rtclock.calend_is_set)
		result = rtclock.calend_offset;
	UNLOCK_RTC(s);

	return (result);
}

void
clock_timebase_info(
	mach_timebase_info_t	info)
{
	spl_t	s;

	LOCK_RTC(s);
	*info = rtclock.timebase_const;
	UNLOCK_RTC(s);
}	

void
clock_set_timer_deadline(
	uint64_t				deadline)
{
	uint64_t				abstime;
	int						decr, mycpu;
	struct rtclock_timer	*mytimer;
	spl_t					s;

	s = splclock();
	mycpu = cpu_number();
	mytimer = &rtclock.timer[mycpu];
	clock_get_uptime(&abstime);
	rtclock.last_abstime[mycpu] = abstime;
	mytimer->deadline = deadline;
	mytimer->is_set = TRUE;
	if (	mytimer->deadline < rtclock_tick_deadline[mycpu]		) {
		decr = deadline_to_decrementer(mytimer->deadline, abstime);
		if (	rtclock_decrementer_min != 0				&&
				rtclock_decrementer_min < (natural_t)decr		)
			decr = rtclock_decrementer_min;

		mtdec(decr);
		rtclock.last_decr[mycpu] = decr;

		KERNEL_DEBUG_CONSTANT(MACHDBG_CODE(DBG_MACH_EXCP_DECI, 1)
							  | DBG_FUNC_NONE, decr, 2, 0, 0, 0);
	}
	splx(s);
}

void
clock_set_timer_func(
	clock_timer_func_t		func)
{
	spl_t		s;

	LOCK_RTC(s);
	if (rtclock.timer_expire == NULL)
		rtclock.timer_expire = func;
	UNLOCK_RTC(s);
}

/*
 * Reset the clock device. This causes the realtime clock
 * device to reload its mode and count value (frequency).
 */
void
rtclock_reset(void)
{
	return;
}

/*
 * Real-time clock device interrupt.
 */
void
rtclock_intr(
	int						device,
	struct savearea			*ssp,
	spl_t					old_spl)
{
	uint64_t				abstime;
	int						decr[3], mycpu = cpu_number();
	struct rtclock_timer	*mytimer = &rtclock.timer[mycpu];

	/*
	 * We may receive interrupts too early, we must reject them.
	 */
	if (rtclock_initialized == FALSE) {
		mtdec(DECREMENTER_MAX);		/* Max the decrementer if not init */
		return;
	}

	decr[1] = decr[2] = DECREMENTER_MAX;

	clock_get_uptime(&abstime);
	rtclock.last_abstime[mycpu] = abstime;
	if (	rtclock_tick_deadline[mycpu] <= abstime		) {
		clock_deadline_for_periodic_event(rtclock_tick_interval, abstime,
										  		&rtclock_tick_deadline[mycpu]);
		hertz_tick(USER_MODE(ssp->save_srr1), ssp->save_srr0);
	}

	clock_get_uptime(&abstime);
	rtclock.last_abstime[mycpu] = abstime;
	if (	mytimer->is_set					&&
			mytimer->deadline <= abstime		) {
		mytimer->is_set = FALSE;
		(*rtclock.timer_expire)(abstime);
	}

	clock_get_uptime(&abstime);
	rtclock.last_abstime[mycpu] = abstime;
	decr[1] = deadline_to_decrementer(rtclock_tick_deadline[mycpu], abstime);

	if (mytimer->is_set)
		decr[2] = deadline_to_decrementer(mytimer->deadline, abstime);

	if (decr[1] > decr[2])
		decr[1] = decr[2];

	if (	rtclock_decrementer_min != 0					&&
			rtclock_decrementer_min < (natural_t)decr[1]		)
		decr[1] = rtclock_decrementer_min;

	mtdec(decr[1]);
	rtclock.last_decr[mycpu] = decr[1];

	KERNEL_DEBUG_CONSTANT(MACHDBG_CODE(DBG_MACH_EXCP_DECI, 1)
						  | DBG_FUNC_NONE, decr[1], 3, 0, 0, 0);
}

static void
rtclock_alarm_timer(
	timer_call_param_t		p0,
	timer_call_param_t		p1)
{
	mach_timespec_t		timestamp;

	(void) sysclk_gettime(&timestamp);

	clock_alarm_intr(SYSTEM_CLOCK, &timestamp);
}

void
clock_get_uptime(
	uint64_t		*result0)
{
	UnsignedWide	*result = (UnsignedWide *)result0;
	uint32_t		hi, lo, hic;

	do {
		asm volatile("	mftbu %0" : "=r" (hi));
		asm volatile("	mftb %0" : "=r" (lo));
		asm volatile("	mftbu %0" : "=r" (hic));
	} while (hic != hi);

	result->lo = lo;
	result->hi = hi;
}

static int
deadline_to_decrementer(
	uint64_t			deadline,
	uint64_t			now)
{
	uint64_t			delt;

	if (deadline <= now)
		return DECREMENTER_MIN;
	else {
		delt = deadline - now;
		return (delt >= (DECREMENTER_MAX + 1))? DECREMENTER_MAX:
				((delt >= (DECREMENTER_MIN + 1))? (delt - 1): DECREMENTER_MIN);
	}
}

static void
timespec_to_absolutetime(
	mach_timespec_t			timespec,
	uint64_t				*result0)
{
	UnsignedWide			*result = (UnsignedWide *)result0;
	UnsignedWide			t64;
	uint32_t				t32;
	uint32_t				numer, denom;
	spl_t					s;

	LOCK_RTC(s);
	numer = rtclock.timebase_const.numer;
	denom = rtclock.timebase_const.denom;
	UNLOCK_RTC(s);

	asm volatile("	mullw %0,%1,%2" :
							"=r" (t64.lo) :
								"r" (timespec.tv_sec), "r" (NSEC_PER_SEC));

	asm volatile("	mulhwu %0,%1,%2" :
							"=r" (t64.hi) :
								"r" (timespec.tv_sec), "r" (NSEC_PER_SEC));

	UnsignedWide_to_scalar(&t64) += timespec.tv_nsec;

	umul_64by32(t64, denom, &t64, &t32);

	udiv_96by32(t64, t32, numer, &t64, &t32);

	result->hi = t64.lo;
	result->lo = t32;
}

void
clock_interval_to_deadline(
	uint32_t			interval,
	uint32_t			scale_factor,
	uint64_t			*result)
{
	uint64_t			abstime;

	clock_get_uptime(result);

	clock_interval_to_absolutetime_interval(interval, scale_factor, &abstime);

	*result += abstime;
}

void
clock_interval_to_absolutetime_interval(
	uint32_t			interval,
	uint32_t			scale_factor,
	uint64_t			*result0)
{
	UnsignedWide		*result = (UnsignedWide *)result0;
	UnsignedWide		t64;
	uint32_t			t32;
	uint32_t			numer, denom;
	spl_t				s;

	LOCK_RTC(s);
	numer = rtclock.timebase_const.numer;
	denom = rtclock.timebase_const.denom;
	UNLOCK_RTC(s);

	asm volatile("	mullw %0,%1,%2" :
							"=r" (t64.lo) :
				 				"r" (interval), "r" (scale_factor));
	asm volatile("	mulhwu %0,%1,%2" :
							"=r" (t64.hi) :
				 				"r" (interval), "r" (scale_factor));

	umul_64by32(t64, denom, &t64, &t32);

	udiv_96by32(t64, t32, numer, &t64, &t32);

	result->hi = t64.lo;
	result->lo = t32;
}

void
clock_absolutetime_interval_to_deadline(
	uint64_t			abstime,
	uint64_t			*result)
{
	clock_get_uptime(result);

	*result += abstime;
}

void
absolutetime_to_nanoseconds(
	uint64_t			abstime,
	uint64_t			*result)
{
	UnsignedWide		t64;
	uint32_t			t32;
	uint32_t			numer, denom;
	spl_t				s;

	LOCK_RTC(s);
	numer = rtclock.timebase_const.numer;
	denom = rtclock.timebase_const.denom;
	UNLOCK_RTC(s);

	UnsignedWide_to_scalar(&t64) = abstime;

	umul_64by32(t64, numer, &t64, &t32);

	udiv_96by32to64(t64, t32, denom, (void *)result);
}

void
nanoseconds_to_absolutetime(
	uint64_t			nanoseconds,
	uint64_t			*result)
{
	UnsignedWide		t64;
	uint32_t			t32;
	uint32_t			numer, denom;
	spl_t				s;

	LOCK_RTC(s);
	numer = rtclock.timebase_const.numer;
	denom = rtclock.timebase_const.denom;
	UNLOCK_RTC(s);

	UnsignedWide_to_scalar(&t64) = nanoseconds;

	umul_64by32(t64, denom, &t64, &t32);

	udiv_96by32to64(t64, t32, numer, (void *)result);
}

/*
 * Spin-loop delay primitives.
 */
void
delay_for_interval(
	uint32_t		interval,
	uint32_t		scale_factor)
{
	uint64_t		now, end;

	clock_interval_to_deadline(interval, scale_factor, &end);

	do {
		clock_get_uptime(&now);
	} while (now < end);
}

void
clock_delay_until(
	uint64_t		deadline)
{
	uint64_t		now;

	do {
		clock_get_uptime(&now);
	} while (now < deadline);
}

void
delay(
	int		usec)
{
	delay_for_interval((usec < 0)? -usec: usec, NSEC_PER_USEC);
}

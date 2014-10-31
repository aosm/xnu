/*
 * Copyright (c) 2000-2013 Apple Inc. All rights reserved.
 *
 * @Apple_LICENSE_HEADER_START@
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
 * @APPLE_OSREFERENCE_LICENSE_HEADER_END@
 */


#include <machine/spl.h>

#include <sys/errno.h>
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/proc_internal.h>
#include <sys/vm.h>
#include <sys/sysctl.h>
#include <sys/kdebug.h>
#include <sys/sysproto.h>
#include <sys/bsdtask_info.h>
#include <sys/random.h>

#define HZ      100
#include <mach/clock_types.h>
#include <mach/mach_types.h>
#include <mach/mach_time.h>
#include <machine/machine_routines.h>

#if defined(__i386__) || defined(__x86_64__)
#include <i386/rtclock_protos.h>
#include <i386/mp.h>
#include <i386/machine_routines.h>
#endif

#include <kern/clock.h>

#include <kern/thread.h>
#include <kern/task.h>
#include <kern/debug.h>
#include <kern/kalloc.h>
#include <kern/cpu_data.h>
#include <kern/assert.h>
#include <kern/telemetry.h>
#include <vm/vm_kern.h>
#include <sys/lock.h>

#include <sys/malloc.h>
#include <sys/mcache.h>
#include <sys/kauth.h>

#include <sys/vnode.h>
#include <sys/vnode_internal.h>
#include <sys/fcntl.h>
#include <sys/file_internal.h>
#include <sys/ubc.h>
#include <sys/param.h>			/* for isset() */

#include <mach/mach_host.h>		/* for host_info() */
#include <libkern/OSAtomic.h>

#include <machine/pal_routines.h>

/*
 * IOP(s)
 *
 * https://coreoswiki.apple.com/wiki/pages/U6z3i0q9/Consistent_Logging_Implementers_Guide.html
 *
 * IOP(s) are auxiliary cores that want to participate in kdebug event logging.
 * They are registered dynamically. Each is assigned a cpu_id at registration.
 *
 * NOTE: IOP trace events may not use the same clock hardware as "normal"
 * cpus. There is an effort made to synchronize the IOP timebase with the
 * AP, but it should be understood that there may be discrepancies.
 *
 * Once registered, an IOP is permanent, it cannot be unloaded/unregistered.
 * The current implementation depends on this for thread safety.
 *
 * New registrations occur by allocating an kd_iop struct and assigning
 * a provisional cpu_id of list_head->cpu_id + 1. Then a CAS to claim the
 * list_head pointer resolves any races.
 *
 * You may safely walk the kd_iops list at any time, without holding locks.
 *
 * When allocating buffers, the current kd_iops head is captured. Any operations
 * that depend on the buffer state (such as flushing IOP traces on reads,
 * etc.) should use the captured list head. This will allow registrations to
 * take place while trace is in use.
 */

typedef struct kd_iop {
	kd_callback_t	callback;
	uint32_t	cpu_id;
	uint64_t	last_timestamp; /* Prevent timer rollback */
	struct kd_iop*	next;
} kd_iop_t;

static kd_iop_t* kd_iops = NULL;

/* XXX should have prototypes, but Mach does not provide one */
void task_act_iterate_wth_args(task_t, void(*)(thread_t, void *), void *);
int cpu_number(void);	/* XXX <machine/...> include path broken */

/* XXX should probably be static, but it's debugging code... */
int kdbg_read(user_addr_t, size_t *, vnode_t, vfs_context_t);
void kdbg_control_chud(int, void *);
int kdbg_control(int *, u_int, user_addr_t, size_t *);
int kdbg_readcpumap(user_addr_t, size_t *);
int kdbg_readcurcpumap(user_addr_t, size_t *);
int kdbg_readthrmap(user_addr_t, size_t *, vnode_t, vfs_context_t);
int kdbg_readcurthrmap(user_addr_t, size_t *);
int kdbg_getreg(kd_regtype *);
int kdbg_setreg(kd_regtype *);
int kdbg_setrtcdec(kd_regtype *);
int kdbg_setpidex(kd_regtype *);
int kdbg_setpid(kd_regtype *);
void kdbg_thrmap_init(void);
int kdbg_reinit(boolean_t);
int kdbg_bootstrap(boolean_t);

int kdbg_cpumap_init_internal(kd_iop_t* iops, uint32_t cpu_count, uint8_t** cpumap, uint32_t* cpumap_size);
kd_threadmap* kdbg_thrmap_init_internal(unsigned int count, unsigned int *mapsize, unsigned int *mapcount);

static int kdbg_enable_typefilter(void);
static int kdbg_disable_typefilter(void);

static int create_buffers(boolean_t);
static void delete_buffers(void);

extern void IOSleep(int);

/* trace enable status */
unsigned int kdebug_enable = 0;

/* A static buffer to record events prior to the start of regular logging */
#define	KD_EARLY_BUFFER_MAX	 64
static kd_buf		kd_early_buffer[KD_EARLY_BUFFER_MAX];
static int		kd_early_index = 0;
static boolean_t	kd_early_overflow = FALSE;

#define SLOW_NOLOG	0x01
#define SLOW_CHECKS	0x02
#define SLOW_ENTROPY	0x04			/* Obsolescent */
#define SLOW_CHUD	0x08

#define EVENTS_PER_STORAGE_UNIT		2048
#define MIN_STORAGE_UNITS_PER_CPU	4

#define POINTER_FROM_KDS_PTR(x) (&kd_bufs[x.buffer_index].kdsb_addr[x.offset])

union kds_ptr {
	struct {
		uint32_t buffer_index:21;
		uint16_t offset:11;
	};
	uint32_t raw;
};

struct kd_storage {
	union	kds_ptr kds_next;
	uint32_t kds_bufindx;
	uint32_t kds_bufcnt;
	uint32_t kds_readlast;
	boolean_t kds_lostevents;
	uint64_t  kds_timestamp;

	kd_buf	kds_records[EVENTS_PER_STORAGE_UNIT];
};

#define MAX_BUFFER_SIZE			(1024 * 1024 * 128)
#define N_STORAGE_UNITS_PER_BUFFER	(MAX_BUFFER_SIZE / sizeof(struct kd_storage))

struct kd_storage_buffers {
	struct	kd_storage	*kdsb_addr;
	uint32_t		kdsb_size;
};

#define KDS_PTR_NULL 0xffffffff
struct kd_storage_buffers *kd_bufs = NULL;
int	n_storage_units = 0;
int	n_storage_buffers = 0;
int	n_storage_threshold = 0;
int	kds_waiter = 0;

#pragma pack(0)
struct kd_bufinfo {
	union  kds_ptr kd_list_head;
	union  kds_ptr kd_list_tail;
	boolean_t kd_lostevents;
	uint32_t _pad;
	uint64_t kd_prev_timebase;
	uint32_t num_bufs;
} __attribute__(( aligned(MAX_CPU_CACHE_LINE_SIZE) ));

struct kd_ctrl_page_t {
	union kds_ptr kds_free_list;
	uint32_t enabled	:1;
	uint32_t _pad0		:31;
	int			kds_inuse_count;
	uint32_t kdebug_flags;
	uint32_t kdebug_slowcheck;
	/*
	 * The number of kd_bufinfo structs allocated may not match the current
	 * number of active cpus. We capture the iops list head at initialization
	 * which we could use to calculate the number of cpus we allocated data for,
	 * unless it happens to be null. To avoid that case, we explicitly also
	 * capture a cpu count.
	 */
	kd_iop_t* kdebug_iops;
	uint32_t kdebug_cpus;
} kd_ctrl_page = { .kds_free_list = {.raw = KDS_PTR_NULL}, .kdebug_slowcheck = SLOW_NOLOG };

#pragma pack()

struct kd_bufinfo *kdbip = NULL;

#define KDCOPYBUF_COUNT	8192
#define KDCOPYBUF_SIZE	(KDCOPYBUF_COUNT * sizeof(kd_buf))
kd_buf *kdcopybuf = NULL;

boolean_t kdlog_bg_trace = FALSE;
boolean_t kdlog_bg_trace_running = FALSE;
unsigned int bg_nkdbufs = 0;

unsigned int nkdbufs = 0;
unsigned int kdlog_beg=0;
unsigned int kdlog_end=0;
unsigned int kdlog_value1=0;
unsigned int kdlog_value2=0;
unsigned int kdlog_value3=0;
unsigned int kdlog_value4=0;

static lck_spin_t * kdw_spin_lock;
static lck_spin_t * kds_spin_lock;
static lck_mtx_t  * kd_trace_mtx_sysctl;
static lck_grp_t  * kd_trace_mtx_sysctl_grp;
static lck_attr_t * kd_trace_mtx_sysctl_attr;
static lck_grp_attr_t   *kd_trace_mtx_sysctl_grp_attr;

static lck_grp_t       *stackshot_subsys_lck_grp;
static lck_grp_attr_t  *stackshot_subsys_lck_grp_attr;
static lck_attr_t      *stackshot_subsys_lck_attr;
static lck_mtx_t        stackshot_subsys_mutex;

void *stackshot_snapbuf = NULL;

int
stack_snapshot2(pid_t pid, user_addr_t tracebuf, uint32_t tracebuf_size, uint32_t flags, uint32_t dispatch_offset, int32_t *retval);

int  
stack_snapshot_from_kernel(pid_t pid, void *buf, uint32_t size, uint32_t flags, unsigned *bytesTraced);
extern void
kdp_snapshot_preflight(int pid, void  *tracebuf, uint32_t tracebuf_size, uint32_t flags, uint32_t dispatch_offset);

extern int
kdp_stack_snapshot_geterror(void);
extern unsigned int
kdp_stack_snapshot_bytes_traced(void);

kd_threadmap *kd_mapptr = 0;
unsigned int kd_mapsize = 0;
unsigned int kd_mapcount = 0;

off_t	RAW_file_offset = 0;
int	RAW_file_written = 0;

#define	RAW_FLUSH_SIZE	(2 * 1024 * 1024)

pid_t global_state_pid = -1;       /* Used to control exclusive use of kd_buffer */

#define DBG_FUNC_MASK	0xfffffffc

/*  TODO: move to kdebug.h */
#define CLASS_MASK      0xff000000
#define CLASS_OFFSET    24
#define SUBCLASS_MASK   0x00ff0000
#define SUBCLASS_OFFSET 16
#define CSC_MASK        0xffff0000	/*  class and subclass mask */
#define CSC_OFFSET      SUBCLASS_OFFSET

#define EXTRACT_CLASS(debugid)          ( (uint8_t) ( ((debugid) & CLASS_MASK   ) >> CLASS_OFFSET    ) )
#define EXTRACT_SUBCLASS(debugid)       ( (uint8_t) ( ((debugid) & SUBCLASS_MASK) >> SUBCLASS_OFFSET ) )
#define EXTRACT_CSC(debugid)            ( (uint16_t)( ((debugid) & CSC_MASK     ) >> CSC_OFFSET      ) )

#define INTERRUPT	0x01050000
#define MACH_vmfault	0x01300008
#define BSC_SysCall	0x040c0000
#define MACH_SysCall	0x010c0000
#define DBG_SCALL_MASK	0xffff0000


/* task to string structure */
struct tts
{
  task_t    task;            /* from procs task */
  pid_t     pid;             /* from procs p_pid  */
  char      task_comm[20];   /* from procs p_comm */
};

typedef struct tts tts_t;

struct krt
{
	kd_threadmap *map;    /* pointer to the map buffer */
	int count;
	int maxcount;
	struct tts *atts;
};

typedef struct krt krt_t;

/* This is for the CHUD toolkit call */
typedef void (*kd_chudhook_fn) (uint32_t debugid, uintptr_t arg1,
				uintptr_t arg2, uintptr_t arg3,
				uintptr_t arg4, uintptr_t arg5);

volatile kd_chudhook_fn kdebug_chudhook = 0;   /* pointer to CHUD toolkit function */

__private_extern__ void stackshot_lock_init( void );

static uint8_t *type_filter_bitmap;

/*
 * This allows kperf to swap out the global state pid when kperf ownership is
 * passed from one process to another. It checks the old global state pid so
 * that kperf can't accidentally steal control of trace when a non-kperf trace user has
 * control of trace.
 */
void
kdbg_swap_global_state_pid(pid_t old_pid, pid_t new_pid);

void
kdbg_swap_global_state_pid(pid_t old_pid, pid_t new_pid)
{
	if (!(kd_ctrl_page.kdebug_flags & KDBG_LOCKINIT))
		return;

	lck_mtx_lock(kd_trace_mtx_sysctl);

	if (old_pid == global_state_pid)
		global_state_pid = new_pid;

	lck_mtx_unlock(kd_trace_mtx_sysctl);
}

static uint32_t
kdbg_cpu_count(boolean_t early_trace)
{
	if (early_trace) {
		/*
		 * we've started tracing before the IOKit has even
		 * started running... just use the static max value
		 */
		return max_ncpus;
	}

	host_basic_info_data_t hinfo;
	mach_msg_type_number_t count = HOST_BASIC_INFO_COUNT;
	host_info((host_t)1 /* BSD_HOST */, HOST_BASIC_INFO, (host_info_t)&hinfo, &count);
	assert(hinfo.logical_cpu_max > 0);
	return hinfo.logical_cpu_max;
}

#if MACH_ASSERT
#endif /* MACH_ASSERT */

static void
kdbg_iop_list_callback(kd_iop_t* iop, kd_callback_type type, void* arg)
{
	while (iop) {
		iop->callback.func(iop->callback.context, type, arg);
		iop = iop->next;
	}
}

static void
kdbg_set_tracing_enabled(boolean_t enabled, uint32_t trace_type)
{
	int s = ml_set_interrupts_enabled(FALSE);
	lck_spin_lock(kds_spin_lock);

	if (enabled) {
		kdebug_enable |= trace_type;
		kd_ctrl_page.kdebug_slowcheck &= ~SLOW_NOLOG;
		kd_ctrl_page.enabled = 1;
	} else {
		kdebug_enable &= ~(KDEBUG_ENABLE_TRACE|KDEBUG_ENABLE_PPT);
		kd_ctrl_page.kdebug_slowcheck |= SLOW_NOLOG;
		kd_ctrl_page.enabled = 0;
	}
	lck_spin_unlock(kds_spin_lock);
	ml_set_interrupts_enabled(s);

	if (enabled) {
		kdbg_iop_list_callback(kd_ctrl_page.kdebug_iops, KD_CALLBACK_KDEBUG_ENABLED, NULL);
	} else {
		/*
		 * If you do not flush the IOP trace buffers, they can linger
		 * for a considerable period; consider code which disables and
		 * deallocates without a final sync flush.
		 */
		kdbg_iop_list_callback(kd_ctrl_page.kdebug_iops, KD_CALLBACK_KDEBUG_DISABLED, NULL);
		kdbg_iop_list_callback(kd_ctrl_page.kdebug_iops, KD_CALLBACK_SYNC_FLUSH, NULL);
	}
}

static void
kdbg_set_flags(int slowflag, int enableflag, boolean_t enabled)
{
	int s = ml_set_interrupts_enabled(FALSE);
	lck_spin_lock(kds_spin_lock);

	if (enabled) {
		kd_ctrl_page.kdebug_slowcheck |= slowflag;
		kdebug_enable |= enableflag;
	} else {
		kd_ctrl_page.kdebug_slowcheck &= ~slowflag;
		kdebug_enable &= ~enableflag;
	}
        
	lck_spin_unlock(kds_spin_lock);
	ml_set_interrupts_enabled(s);
}

void
disable_wrap(uint32_t *old_slowcheck, uint32_t *old_flags)
{
	int s = ml_set_interrupts_enabled(FALSE);
	lck_spin_lock(kds_spin_lock);

	*old_slowcheck = kd_ctrl_page.kdebug_slowcheck;
	*old_flags = kd_ctrl_page.kdebug_flags;

	kd_ctrl_page.kdebug_flags &= ~KDBG_WRAPPED;
	kd_ctrl_page.kdebug_flags |= KDBG_NOWRAP;

	lck_spin_unlock(kds_spin_lock);
	ml_set_interrupts_enabled(s);
}

void
enable_wrap(uint32_t old_slowcheck, boolean_t lostevents)
{
	int s = ml_set_interrupts_enabled(FALSE);
	lck_spin_lock(kds_spin_lock);

	kd_ctrl_page.kdebug_flags &= ~KDBG_NOWRAP;

	if ( !(old_slowcheck & SLOW_NOLOG))
		kd_ctrl_page.kdebug_slowcheck &= ~SLOW_NOLOG;

	if (lostevents == TRUE)
		kd_ctrl_page.kdebug_flags |= KDBG_WRAPPED;

	lck_spin_unlock(kds_spin_lock);
	ml_set_interrupts_enabled(s);
}

static int
create_buffers(boolean_t early_trace)
{
        int	i;
	int	p_buffer_size;
	int	f_buffer_size;
	int	f_buffers;
	int	error = 0;

	/*
	 * For the duration of this allocation, trace code will only reference
	 * kdebug_iops. Any iops registered after this enabling will not be
	 * messaged until the buffers are reallocated.
	 *
	 * TLDR; Must read kd_iops once and only once!
	 */
	kd_ctrl_page.kdebug_iops = kd_iops;


	/*
	 * If the list is valid, it is sorted, newest -> oldest. Each iop entry
	 * has a cpu_id of "the older entry + 1", so the highest cpu_id will
	 * be the list head + 1.
	 */

	kd_ctrl_page.kdebug_cpus = kd_ctrl_page.kdebug_iops ? kd_ctrl_page.kdebug_iops->cpu_id + 1 : kdbg_cpu_count(early_trace);

	if (kmem_alloc(kernel_map, (vm_offset_t *)&kdbip, sizeof(struct kd_bufinfo) * kd_ctrl_page.kdebug_cpus) != KERN_SUCCESS) {
		error = ENOSPC;
		goto out;
	}

	if (nkdbufs < (kd_ctrl_page.kdebug_cpus * EVENTS_PER_STORAGE_UNIT * MIN_STORAGE_UNITS_PER_CPU))
		n_storage_units = kd_ctrl_page.kdebug_cpus * MIN_STORAGE_UNITS_PER_CPU;
	else
		n_storage_units = nkdbufs / EVENTS_PER_STORAGE_UNIT;

	nkdbufs = n_storage_units * EVENTS_PER_STORAGE_UNIT;

	f_buffers = n_storage_units / N_STORAGE_UNITS_PER_BUFFER;
	n_storage_buffers = f_buffers;

	f_buffer_size = N_STORAGE_UNITS_PER_BUFFER * sizeof(struct kd_storage);
	p_buffer_size = (n_storage_units % N_STORAGE_UNITS_PER_BUFFER) * sizeof(struct kd_storage);

	if (p_buffer_size)
		n_storage_buffers++;

	kd_bufs = NULL;

	if (kdcopybuf == 0) {
	        if (kmem_alloc(kernel_map, (vm_offset_t *)&kdcopybuf, (vm_size_t)KDCOPYBUF_SIZE) != KERN_SUCCESS) {
			error = ENOSPC;
			goto out;
		}
	}
	if (kmem_alloc(kernel_map, (vm_offset_t *)&kd_bufs, (vm_size_t)(n_storage_buffers * sizeof(struct kd_storage_buffers))) != KERN_SUCCESS) {
		error = ENOSPC;
		goto out;
	}
	bzero(kd_bufs, n_storage_buffers * sizeof(struct kd_storage_buffers));

	for (i = 0; i < f_buffers; i++) {
		if (kmem_alloc(kernel_map, (vm_offset_t *)&kd_bufs[i].kdsb_addr, (vm_size_t)f_buffer_size) != KERN_SUCCESS) {
			error = ENOSPC;
			goto out;
		}
		bzero(kd_bufs[i].kdsb_addr, f_buffer_size);

		kd_bufs[i].kdsb_size = f_buffer_size;
	}
	if (p_buffer_size) {
		if (kmem_alloc(kernel_map, (vm_offset_t *)&kd_bufs[i].kdsb_addr, (vm_size_t)p_buffer_size) != KERN_SUCCESS) {
			error = ENOSPC;
			goto out;
		}
		bzero(kd_bufs[i].kdsb_addr, p_buffer_size);

		kd_bufs[i].kdsb_size = p_buffer_size;
	}
	n_storage_units = 0;

	for (i = 0; i < n_storage_buffers; i++) {
		struct kd_storage *kds;
		int	n_elements;
		int	n;

		n_elements = kd_bufs[i].kdsb_size / sizeof(struct kd_storage);
		kds = kd_bufs[i].kdsb_addr;

		for (n = 0; n < n_elements; n++) {
			kds[n].kds_next.buffer_index = kd_ctrl_page.kds_free_list.buffer_index;
			kds[n].kds_next.offset = kd_ctrl_page.kds_free_list.offset;

			kd_ctrl_page.kds_free_list.buffer_index = i;
			kd_ctrl_page.kds_free_list.offset = n;
		}
		n_storage_units += n_elements;
	}

	bzero((char *)kdbip, sizeof(struct kd_bufinfo) * kd_ctrl_page.kdebug_cpus);

	for (i = 0; i < (int)kd_ctrl_page.kdebug_cpus; i++) {
		kdbip[i].kd_list_head.raw = KDS_PTR_NULL;
		kdbip[i].kd_list_tail.raw = KDS_PTR_NULL;
		kdbip[i].kd_lostevents = FALSE;
		kdbip[i].num_bufs = 0;
	}
        
	kd_ctrl_page.kdebug_flags |= KDBG_BUFINIT;

	kd_ctrl_page.kds_inuse_count = 0;
	n_storage_threshold = n_storage_units / 2;
out:
	if (error)
		delete_buffers();

	return(error);
}

static void
delete_buffers(void)
{
	int i;
	
	if (kd_bufs) {
		for (i = 0; i < n_storage_buffers; i++) {
			if (kd_bufs[i].kdsb_addr) {
				kmem_free(kernel_map, (vm_offset_t)kd_bufs[i].kdsb_addr, (vm_size_t)kd_bufs[i].kdsb_size);
			}
		}
		kmem_free(kernel_map, (vm_offset_t)kd_bufs, (vm_size_t)(n_storage_buffers * sizeof(struct kd_storage_buffers)));

		kd_bufs = NULL;
		n_storage_buffers = 0;
	}
	if (kdcopybuf) {
		kmem_free(kernel_map, (vm_offset_t)kdcopybuf, KDCOPYBUF_SIZE);

		kdcopybuf = NULL;
	}
	kd_ctrl_page.kds_free_list.raw = KDS_PTR_NULL;

	if (kdbip) {
		kmem_free(kernel_map, (vm_offset_t)kdbip, sizeof(struct kd_bufinfo) * kd_ctrl_page.kdebug_cpus);
		
		kdbip = NULL;
	}
        kd_ctrl_page.kdebug_iops = NULL;
	kd_ctrl_page.kdebug_cpus = 0;
	kd_ctrl_page.kdebug_flags &= ~KDBG_BUFINIT;
}

void
release_storage_unit(int cpu, uint32_t kdsp_raw)
{
	int s = 0;
	struct	kd_storage *kdsp_actual;
	struct kd_bufinfo *kdbp;
	union kds_ptr kdsp;

	kdsp.raw = kdsp_raw;

	s = ml_set_interrupts_enabled(FALSE);
	lck_spin_lock(kds_spin_lock);

	kdbp = &kdbip[cpu];

	if (kdsp.raw == kdbp->kd_list_head.raw) {
		/*
		 * it's possible for the storage unit pointed to
		 * by kdsp to have already been stolen... so
		 * check to see if it's still the head of the list
		 * now that we're behind the lock that protects 
		 * adding and removing from the queue...
		 * since we only ever release and steal units from
		 * that position, if it's no longer the head
		 * we having nothing to do in this context
		 */
		kdsp_actual = POINTER_FROM_KDS_PTR(kdsp);
		kdbp->kd_list_head = kdsp_actual->kds_next;

		kdsp_actual->kds_next = kd_ctrl_page.kds_free_list;
		kd_ctrl_page.kds_free_list = kdsp;

		kd_ctrl_page.kds_inuse_count--;
	}
	lck_spin_unlock(kds_spin_lock);
	ml_set_interrupts_enabled(s);
}


boolean_t
allocate_storage_unit(int cpu)
{
	union	kds_ptr kdsp;
	struct	kd_storage *kdsp_actual, *kdsp_next_actual;
	struct  kd_bufinfo *kdbp, *kdbp_vict, *kdbp_try;
	uint64_t	oldest_ts, ts;
	boolean_t	retval = TRUE;
	int			s = 0;
		
	s = ml_set_interrupts_enabled(FALSE);
	lck_spin_lock(kds_spin_lock);

	kdbp = &kdbip[cpu];

	/* If someone beat us to the allocate, return success */
	if (kdbp->kd_list_tail.raw != KDS_PTR_NULL) {
		kdsp_actual = POINTER_FROM_KDS_PTR(kdbp->kd_list_tail);

		if (kdsp_actual->kds_bufindx < EVENTS_PER_STORAGE_UNIT)
			goto out;
	}
	
	if ((kdsp = kd_ctrl_page.kds_free_list).raw != KDS_PTR_NULL) {
		kdsp_actual = POINTER_FROM_KDS_PTR(kdsp);
		kd_ctrl_page.kds_free_list = kdsp_actual->kds_next;

		kd_ctrl_page.kds_inuse_count++;
	} else {
		if (kd_ctrl_page.kdebug_flags & KDBG_NOWRAP) {
			kd_ctrl_page.kdebug_slowcheck |= SLOW_NOLOG;
			kdbp->kd_lostevents = TRUE;
			retval = FALSE;
			goto out;
		}
		kdbp_vict = NULL;
		oldest_ts = (uint64_t)-1;

		for (kdbp_try = &kdbip[0]; kdbp_try < &kdbip[kd_ctrl_page.kdebug_cpus]; kdbp_try++) {

			if (kdbp_try->kd_list_head.raw == KDS_PTR_NULL) {
				/*
				 * no storage unit to steal
				 */
				continue;
			}

			kdsp_actual = POINTER_FROM_KDS_PTR(kdbp_try->kd_list_head);

			if (kdsp_actual->kds_bufcnt < EVENTS_PER_STORAGE_UNIT) {
				/*
				 * make sure we don't steal the storage unit
				 * being actively recorded to...  need to
				 * move on because we don't want an out-of-order
				 * set of events showing up later
				 */
				continue;
			}
			ts = kdbg_get_timestamp(&kdsp_actual->kds_records[0]);

			if (ts < oldest_ts) {
				/*
				 * when 'wrapping', we want to steal the
				 * storage unit that has the 'earliest' time
				 * associated with it (first event time)
				 */
				oldest_ts = ts;
				kdbp_vict = kdbp_try;
			}
		}
		if (kdbp_vict == NULL) {
			kdebug_enable = 0;
			kd_ctrl_page.enabled = 0;
			retval = FALSE;
			goto out;
		}
		kdsp = kdbp_vict->kd_list_head;
		kdsp_actual = POINTER_FROM_KDS_PTR(kdsp);
		kdbp_vict->kd_list_head = kdsp_actual->kds_next;

		if (kdbp_vict->kd_list_head.raw != KDS_PTR_NULL) {
			kdsp_next_actual = POINTER_FROM_KDS_PTR(kdbp_vict->kd_list_head);
			kdsp_next_actual->kds_lostevents = TRUE;
		} else
			kdbp_vict->kd_lostevents = TRUE;

		kd_ctrl_page.kdebug_flags |= KDBG_WRAPPED;
	}
	kdsp_actual->kds_timestamp = mach_absolute_time();
	kdsp_actual->kds_next.raw = KDS_PTR_NULL;
	kdsp_actual->kds_bufcnt	  = 0;
	kdsp_actual->kds_readlast = 0;

	kdsp_actual->kds_lostevents = kdbp->kd_lostevents;
	kdbp->kd_lostevents = FALSE;
	kdsp_actual->kds_bufindx  = 0;

	if (kdbp->kd_list_head.raw == KDS_PTR_NULL)
		kdbp->kd_list_head = kdsp;
	else
		POINTER_FROM_KDS_PTR(kdbp->kd_list_tail)->kds_next = kdsp;
	kdbp->kd_list_tail = kdsp;
out:
	lck_spin_unlock(kds_spin_lock);
	ml_set_interrupts_enabled(s);

	return (retval);
}

int
kernel_debug_register_callback(kd_callback_t callback)
{
	kd_iop_t* iop;
	if (kmem_alloc(kernel_map, (vm_offset_t *)&iop, sizeof(kd_iop_t)) == KERN_SUCCESS) {
		memcpy(&iop->callback, &callback, sizeof(kd_callback_t));
		
		/*
		 * <rdar://problem/13351477> Some IOP clients are not providing a name.
		 *
		 * Remove when fixed.
		 */
		{
			boolean_t is_valid_name = FALSE;
			for (uint32_t length=0; length<sizeof(callback.iop_name); ++length) {
				/* This is roughly isprintable(c) */
				if (callback.iop_name[length] > 0x20 && callback.iop_name[length] < 0x7F)
					continue;
				if (callback.iop_name[length] == 0) {
					if (length)
						is_valid_name = TRUE;
					break;
				}
			}
			
			if (!is_valid_name) {
				strlcpy(iop->callback.iop_name, "IOP-???", sizeof(iop->callback.iop_name));
			}
		}

		iop->last_timestamp = 0;
		
		do {
			/*
			 * We use two pieces of state, the old list head
			 * pointer, and the value of old_list_head->cpu_id.
			 * If we read kd_iops more than once, it can change
			 * between reads.
			 *
			 * TLDR; Must not read kd_iops more than once per loop.
			 */
			iop->next = kd_iops;
			iop->cpu_id = iop->next ? (iop->next->cpu_id+1) : kdbg_cpu_count(FALSE);

			/*
			 * Header says OSCompareAndSwapPtr has a memory barrier
			 */
		} while (!OSCompareAndSwapPtr(iop->next, iop, (void* volatile*)&kd_iops));

		return iop->cpu_id;
	}

	return 0;
}

void
kernel_debug_enter(
	uint32_t	coreid,
	uint32_t	debugid,
	uint64_t	timestamp,
	uintptr_t	arg1,
	uintptr_t	arg2,
	uintptr_t	arg3,
	uintptr_t	arg4,
	uintptr_t	threadid
	)
{
	uint32_t	bindx;
	kd_buf		*kd;
	struct kd_bufinfo *kdbp;
	struct kd_storage *kdsp_actual;
	union  kds_ptr kds_raw;

	if (kd_ctrl_page.kdebug_slowcheck) {

		if ( (kd_ctrl_page.kdebug_slowcheck & SLOW_NOLOG) || !(kdebug_enable & (KDEBUG_ENABLE_TRACE|KDEBUG_ENABLE_PPT)))
			goto out1;
	
		if (kd_ctrl_page.kdebug_flags & KDBG_TYPEFILTER_CHECK) {
			if (isset(type_filter_bitmap, EXTRACT_CSC(debugid))) 
				goto record_event;
			goto out1;
		}
		else if (kd_ctrl_page.kdebug_flags & KDBG_RANGECHECK) {
			if (debugid >= kdlog_beg && debugid <= kdlog_end)
				goto record_event;
			goto out1;
		}
		else if (kd_ctrl_page.kdebug_flags & KDBG_VALCHECK) {
			if ((debugid & DBG_FUNC_MASK) != kdlog_value1 &&
				(debugid & DBG_FUNC_MASK) != kdlog_value2 &&
				(debugid & DBG_FUNC_MASK) != kdlog_value3 &&
				(debugid & DBG_FUNC_MASK) != kdlog_value4)
				goto out1;
		}
	}
	
record_event:

	disable_preemption();

	if (kd_ctrl_page.enabled == 0)
		goto out;

	kdbp = &kdbip[coreid];
	timestamp &= KDBG_TIMESTAMP_MASK;

retry_q:
	kds_raw = kdbp->kd_list_tail;

	if (kds_raw.raw != KDS_PTR_NULL) {
		kdsp_actual = POINTER_FROM_KDS_PTR(kds_raw);
		bindx = kdsp_actual->kds_bufindx;
	} else
		kdsp_actual = NULL;
	
	if (kdsp_actual == NULL || bindx >= EVENTS_PER_STORAGE_UNIT) {
		if (allocate_storage_unit(coreid) == FALSE) {
			/*
			 * this can only happen if wrapping
			 * has been disabled
			 */
			goto out;
		}
		goto retry_q;
	}
	if ( !OSCompareAndSwap(bindx, bindx + 1, &kdsp_actual->kds_bufindx))
		goto retry_q;

	// IOP entries can be allocated before xnu allocates and inits the buffer
	if (timestamp < kdsp_actual->kds_timestamp)
		kdsp_actual->kds_timestamp = timestamp;

	kd = &kdsp_actual->kds_records[bindx];

	kd->debugid = debugid;
	kd->arg1 = arg1;
	kd->arg2 = arg2;
	kd->arg3 = arg3;
	kd->arg4 = arg4;
	kd->arg5 = threadid;
	          
	kdbg_set_timestamp_and_cpu(kd, timestamp, coreid);

	OSAddAtomic(1, &kdsp_actual->kds_bufcnt);
out:
	enable_preemption();
out1:
	if ((kds_waiter && kd_ctrl_page.kds_inuse_count >= n_storage_threshold)) {
		boolean_t need_kds_wakeup = FALSE;
		int	s;

		/*
		 * try to take the lock here to synchronize with the
		 * waiter entering the blocked state... use the try
		 * mode to prevent deadlocks caused by re-entering this
		 * routine due to various trace points triggered in the
		 * lck_spin_sleep_xxxx routines used to actually enter
		 * our wait condition... no problem if we fail,
		 * there will be lots of additional events coming in that
		 * will eventually succeed in grabbing this lock
		 */
		s = ml_set_interrupts_enabled(FALSE);

		if (lck_spin_try_lock(kdw_spin_lock)) {

			if (kds_waiter && kd_ctrl_page.kds_inuse_count >= n_storage_threshold) {
				kds_waiter = 0;
				need_kds_wakeup = TRUE;
			}
			lck_spin_unlock(kdw_spin_lock);

			ml_set_interrupts_enabled(s);
			
			if (need_kds_wakeup == TRUE)
				wakeup(&kds_waiter);
		}
	}
}



void
kernel_debug_internal(
	uint32_t	debugid,
	uintptr_t	arg1,
	uintptr_t	arg2,
	uintptr_t	arg3,
	uintptr_t	arg4,
	uintptr_t	arg5);

__attribute__((always_inline)) void
kernel_debug_internal(
	uint32_t	debugid,
	uintptr_t	arg1,
	uintptr_t	arg2,
	uintptr_t	arg3,
	uintptr_t	arg4,
	uintptr_t	arg5)
{
	struct proc 	*curproc;
	uint64_t 	now;
	uint32_t	bindx;
	boolean_t	s;
	kd_buf		*kd;
	int		cpu;
	struct kd_bufinfo *kdbp;
	struct kd_storage *kdsp_actual;
	union  kds_ptr kds_raw;

	

	if (kd_ctrl_page.kdebug_slowcheck) {

		if (kdebug_enable & KDEBUG_ENABLE_CHUD) {
			kd_chudhook_fn chudhook;
			/*
			 * Mask interrupts to minimize the interval across
			 * which the driver providing the hook could be
			 * unloaded.
			 */
			s = ml_set_interrupts_enabled(FALSE);
			chudhook = kdebug_chudhook;
			if (chudhook)
				chudhook(debugid, arg1, arg2, arg3, arg4, arg5);
			ml_set_interrupts_enabled(s);
		}
		if ( (kd_ctrl_page.kdebug_slowcheck & SLOW_NOLOG) || !(kdebug_enable & (KDEBUG_ENABLE_TRACE|KDEBUG_ENABLE_PPT)))
			goto out1;
	
		if ( !ml_at_interrupt_context()) {
			if (kd_ctrl_page.kdebug_flags & KDBG_PIDCHECK) {
				/*
				 * If kdebug flag is not set for current proc, return
				 */
				curproc = current_proc();

				if ((curproc && !(curproc->p_kdebug)) &&
				    ((debugid & 0xffff0000) != (MACHDBG_CODE(DBG_MACH_SCHED, 0) | DBG_FUNC_NONE)) &&
				      (debugid >> 24 != DBG_TRACE))
					goto out1;
			}
			else if (kd_ctrl_page.kdebug_flags & KDBG_PIDEXCLUDE) {
				/*
				 * If kdebug flag is set for current proc, return
				 */
				curproc = current_proc();

				if ((curproc && curproc->p_kdebug) &&
				    ((debugid & 0xffff0000) != (MACHDBG_CODE(DBG_MACH_SCHED, 0) | DBG_FUNC_NONE)) &&
				      (debugid >> 24 != DBG_TRACE))
					goto out1;
			}
		}

		if (kd_ctrl_page.kdebug_flags & KDBG_TYPEFILTER_CHECK) {
			/* Always record trace system info */
			if (EXTRACT_CLASS(debugid) == DBG_TRACE)
				goto record_event;

			if (isset(type_filter_bitmap, EXTRACT_CSC(debugid))) 
				goto record_event;
			goto out1;
		}
		else if (kd_ctrl_page.kdebug_flags & KDBG_RANGECHECK) {
			/* Always record trace system info */
			if (EXTRACT_CLASS(debugid) == DBG_TRACE)
				goto record_event;
				
			if (debugid < kdlog_beg || debugid > kdlog_end)
				goto out1;
		}
		else if (kd_ctrl_page.kdebug_flags & KDBG_VALCHECK) {
			/* Always record trace system info */
			if (EXTRACT_CLASS(debugid) == DBG_TRACE)
				goto record_event;
		
			if ((debugid & DBG_FUNC_MASK) != kdlog_value1 &&
			    (debugid & DBG_FUNC_MASK) != kdlog_value2 &&
			    (debugid & DBG_FUNC_MASK) != kdlog_value3 &&
			    (debugid & DBG_FUNC_MASK) != kdlog_value4)
				goto out1;
		}
	}
record_event:
	disable_preemption();

	if (kd_ctrl_page.enabled == 0)
		goto out;

	cpu = cpu_number();
	kdbp = &kdbip[cpu];
retry_q:
	kds_raw = kdbp->kd_list_tail;

	if (kds_raw.raw != KDS_PTR_NULL) {
		kdsp_actual = POINTER_FROM_KDS_PTR(kds_raw);
		bindx = kdsp_actual->kds_bufindx;
	} else
		kdsp_actual = NULL;
	
	if (kdsp_actual == NULL || bindx >= EVENTS_PER_STORAGE_UNIT) {
		if (allocate_storage_unit(cpu) == FALSE) {
			/*
			 * this can only happen if wrapping
			 * has been disabled
			 */
			goto out;
		}
		goto retry_q;
	}
	now = mach_absolute_time() & KDBG_TIMESTAMP_MASK;

	if ( !OSCompareAndSwap(bindx, bindx + 1, &kdsp_actual->kds_bufindx))
		goto retry_q;

	kd = &kdsp_actual->kds_records[bindx];

	kd->debugid = debugid;
	kd->arg1 = arg1;
	kd->arg2 = arg2;
	kd->arg3 = arg3;
	kd->arg4 = arg4;
	kd->arg5 = arg5;
	          
	kdbg_set_timestamp_and_cpu(kd, now, cpu);

	OSAddAtomic(1, &kdsp_actual->kds_bufcnt);
out:
	enable_preemption();
out1:
	if (kds_waiter && kd_ctrl_page.kds_inuse_count >= n_storage_threshold) {
		uint32_t	etype;
		uint32_t	stype;
		
		etype = debugid & DBG_FUNC_MASK;
		stype = debugid & DBG_SCALL_MASK;

		if (etype == INTERRUPT || etype == MACH_vmfault ||
		    stype == BSC_SysCall || stype == MACH_SysCall) {

			boolean_t need_kds_wakeup = FALSE;

			/*
			 * try to take the lock here to synchronize with the
			 * waiter entering the blocked state... use the try
			 * mode to prevent deadlocks caused by re-entering this
			 * routine due to various trace points triggered in the
			 * lck_spin_sleep_xxxx routines used to actually enter
			 * one of our 2 wait conditions... no problem if we fail,
			 * there will be lots of additional events coming in that
			 * will eventually succeed in grabbing this lock
			 */
			s = ml_set_interrupts_enabled(FALSE);

			if (lck_spin_try_lock(kdw_spin_lock)) {

				if (kds_waiter && kd_ctrl_page.kds_inuse_count >= n_storage_threshold) {
					kds_waiter = 0;
					need_kds_wakeup = TRUE;
				}
				lck_spin_unlock(kdw_spin_lock);
			}
			ml_set_interrupts_enabled(s);
			
			if (need_kds_wakeup == TRUE)
				wakeup(&kds_waiter);
		}
	}
}

void
kernel_debug(
	uint32_t	debugid,
	uintptr_t	arg1,
	uintptr_t	arg2,
	uintptr_t	arg3,
	uintptr_t	arg4,
	__unused uintptr_t arg5)
{
	kernel_debug_internal(debugid, arg1, arg2, arg3, arg4, (uintptr_t)thread_tid(current_thread()));
}

void
kernel_debug1(
	uint32_t	debugid,
	uintptr_t	arg1,
	uintptr_t	arg2,
	uintptr_t	arg3,
	uintptr_t	arg4,
	uintptr_t	arg5)
{
	kernel_debug_internal(debugid, arg1, arg2, arg3, arg4, arg5);
}

void
kernel_debug_string(const char *message)
{
	uintptr_t arg[4] = {0, 0, 0, 0};

	/* Stuff the message string in the args and log it. */
        strncpy((char *)arg, message, MIN(sizeof(arg), strlen(message)));
	KERNEL_DEBUG_EARLY(
		(TRACEDBG_CODE(DBG_TRACE_INFO, 4)) | DBG_FUNC_NONE,
		arg[0], arg[1], arg[2], arg[3]);
}

extern int	master_cpu;		/* MACH_KERNEL_PRIVATE */
/*
 * Used prior to start_kern_tracing() being called.
 * Log temporarily into a static buffer.
 */
void
kernel_debug_early(
	uint32_t	debugid,
	uintptr_t	arg1,
	uintptr_t	arg2,
	uintptr_t	arg3,
	uintptr_t	arg4)
{
	/* If tracing is already initialized, use it */
	if (nkdbufs)
		KERNEL_DEBUG_CONSTANT(debugid, arg1, arg2, arg3, arg4, 0);

	/* Do nothing if the buffer is full or we're not on the boot cpu */ 
	kd_early_overflow = kd_early_index >= KD_EARLY_BUFFER_MAX;
	if (kd_early_overflow ||
	    cpu_number() != master_cpu)
		return;

	kd_early_buffer[kd_early_index].debugid = debugid;
	kd_early_buffer[kd_early_index].timestamp = mach_absolute_time();
	kd_early_buffer[kd_early_index].arg1 = arg1;
	kd_early_buffer[kd_early_index].arg2 = arg2;
	kd_early_buffer[kd_early_index].arg3 = arg3;
	kd_early_buffer[kd_early_index].arg4 = arg4;
	kd_early_buffer[kd_early_index].arg5 = 0;
	kd_early_index++;
}

/*
 * Transfer the contents of the temporary buffer into the trace buffers.
 * Precede that by logging the rebase time (offset) - the TSC-based time (in ns)
 * when mach_absolute_time is set to 0.
 */
static void
kernel_debug_early_end(void)
{
	int	i;

	if (cpu_number() != master_cpu)
		panic("kernel_debug_early_end() not call on boot processor");

	/* Fake sentinel marking the start of kernel time relative to TSC */
	kernel_debug_enter(
		0,
		(TRACEDBG_CODE(DBG_TRACE_INFO, 1)) | DBG_FUNC_NONE,
		0,
		(uint32_t)(tsc_rebase_abs_time >> 32),
		(uint32_t)tsc_rebase_abs_time,
		0,
		0,
		0);
	for (i = 0; i < kd_early_index; i++) {
		kernel_debug_enter(
			0,
			kd_early_buffer[i].debugid,
			kd_early_buffer[i].timestamp,
			kd_early_buffer[i].arg1,
			kd_early_buffer[i].arg2,
			kd_early_buffer[i].arg3,
			kd_early_buffer[i].arg4,
			0);
	}

	/* Cut events-lost event on overflow */
	if (kd_early_overflow)
		KERNEL_DEBUG_CONSTANT(
			TRACEDBG_CODE(DBG_TRACE_INFO, 2), 0, 0, 0, 0, 0);

	/* This trace marks the start of kernel tracing */
	kernel_debug_string("early trace done");
}

/*
 * Support syscall SYS_kdebug_trace
 */
int
kdebug_trace(__unused struct proc *p, struct kdebug_trace_args *uap, __unused int32_t *retval)
{
	if ( __probable(kdebug_enable == 0) )
		return(0);
 
	kernel_debug_internal(uap->code, uap->arg1, uap->arg2, uap->arg3, uap->arg4, (uintptr_t)thread_tid(current_thread()));

	return(0);
}


static void
kdbg_lock_init(void)
{
	if (kd_ctrl_page.kdebug_flags & KDBG_LOCKINIT)
		return;
	
	/*
	 * allocate lock group attribute and group
	 */
	kd_trace_mtx_sysctl_grp_attr = lck_grp_attr_alloc_init();
	kd_trace_mtx_sysctl_grp = lck_grp_alloc_init("kdebug", kd_trace_mtx_sysctl_grp_attr);
		
	/*
	 * allocate the lock attribute
	 */
	kd_trace_mtx_sysctl_attr = lck_attr_alloc_init();


	/*
	 * allocate and initialize mutex's
	 */
	kd_trace_mtx_sysctl = lck_mtx_alloc_init(kd_trace_mtx_sysctl_grp, kd_trace_mtx_sysctl_attr);
	kds_spin_lock = lck_spin_alloc_init(kd_trace_mtx_sysctl_grp, kd_trace_mtx_sysctl_attr);
	kdw_spin_lock = lck_spin_alloc_init(kd_trace_mtx_sysctl_grp, kd_trace_mtx_sysctl_attr);

	kd_ctrl_page.kdebug_flags |= KDBG_LOCKINIT;
}


int
kdbg_bootstrap(boolean_t early_trace)
{
        kd_ctrl_page.kdebug_flags &= ~KDBG_WRAPPED;

	return (create_buffers(early_trace));
}

int
kdbg_reinit(boolean_t early_trace)
{
	int ret = 0;

	/*
	 * Disable trace collecting
	 * First make sure we're not in
	 * the middle of cutting a trace
	 */
	kdbg_set_tracing_enabled(FALSE, KDEBUG_ENABLE_TRACE);

	/*
	 * make sure the SLOW_NOLOG is seen
	 * by everyone that might be trying
	 * to cut a trace..
	 */
	IOSleep(100);

	delete_buffers();

	if ((kd_ctrl_page.kdebug_flags & KDBG_MAPINIT) && kd_mapsize && kd_mapptr) {
		kmem_free(kernel_map, (vm_offset_t)kd_mapptr, kd_mapsize);
		kd_ctrl_page.kdebug_flags &= ~KDBG_MAPINIT;
		kd_mapsize = 0;
		kd_mapptr = (kd_threadmap *) 0;
		kd_mapcount = 0;
	}  
	ret = kdbg_bootstrap(early_trace);

	RAW_file_offset = 0;
	RAW_file_written = 0;

	return(ret);
}

void
kdbg_trace_data(struct proc *proc, long *arg_pid)
{
	if (!proc)
		*arg_pid = 0;
	else
		*arg_pid = proc->p_pid;
}


void
kdbg_trace_string(struct proc *proc, long *arg1, long *arg2, long *arg3, long *arg4)
{
	char *dbg_nameptr; 
	int dbg_namelen;
	long dbg_parms[4];

	if (!proc) {
		*arg1 = 0;
		*arg2 = 0;
		*arg3 = 0;
		*arg4 = 0;
		return;
	}
	/*
	 * Collect the pathname for tracing
	 */
	dbg_nameptr = proc->p_comm;
	dbg_namelen = (int)strlen(proc->p_comm);
	dbg_parms[0]=0L;
	dbg_parms[1]=0L;
	dbg_parms[2]=0L;
	dbg_parms[3]=0L;
  
	if(dbg_namelen > (int)sizeof(dbg_parms))
		dbg_namelen = (int)sizeof(dbg_parms);
    
	strncpy((char *)dbg_parms, dbg_nameptr, dbg_namelen);

	*arg1=dbg_parms[0];
	*arg2=dbg_parms[1];
	*arg3=dbg_parms[2];
	*arg4=dbg_parms[3];
}

static void
kdbg_resolve_map(thread_t th_act, void *opaque)
{
	kd_threadmap *mapptr;
	krt_t *t = (krt_t *)opaque;

	if (t->count < t->maxcount) {
		mapptr = &t->map[t->count];
		mapptr->thread  = (uintptr_t)thread_tid(th_act);

		(void) strlcpy (mapptr->command, t->atts->task_comm,
				sizeof(t->atts->task_comm));
		/*
		 * Some kernel threads have no associated pid.
		 * We still need to mark the entry as valid.
		 */
		if (t->atts->pid)
			mapptr->valid = t->atts->pid;
		else
			mapptr->valid = 1;

		t->count++;
	}
}

/*
 *
 * Writes a cpumap for the given iops_list/cpu_count to the provided buffer.
 *
 * You may provide a buffer and size, or if you set the buffer to NULL, a
 * buffer of sufficient size will be allocated.
 *
 * If you provide a buffer and it is too small, sets cpumap_size to the number
 * of bytes required and returns EINVAL.
 *
 * On success, if you provided a buffer, cpumap_size is set to the number of
 * bytes written. If you did not provide a buffer, cpumap is set to the newly
 * allocated buffer and cpumap_size is set to the number of bytes allocated.
 *
 * NOTE: It may seem redundant to pass both iops and a cpu_count.
 *
 * We may be reporting data from "now", or from the "past".
 *
 * The "now" data would be for something like kdbg_readcurcpumap().
 * The "past" data would be for kdbg_readcpumap().
 *
 * If we do not pass both iops and cpu_count, and iops is NULL, this function
 * will need to read "now" state to get the number of cpus, which would be in
 * error if we were reporting "past" state.
 */

int
kdbg_cpumap_init_internal(kd_iop_t* iops, uint32_t cpu_count, uint8_t** cpumap, uint32_t* cpumap_size)
{
	assert(cpumap);
	assert(cpumap_size);
	assert(cpu_count);
	assert(!iops || iops->cpu_id + 1 == cpu_count);

	uint32_t bytes_needed = sizeof(kd_cpumap_header) + cpu_count * sizeof(kd_cpumap);
	uint32_t bytes_available = *cpumap_size;
	*cpumap_size = bytes_needed;
	
	if (*cpumap == NULL) {
		if (kmem_alloc(kernel_map, (vm_offset_t*)cpumap, (vm_size_t)*cpumap_size) != KERN_SUCCESS) {
			return ENOMEM;
		}
	} else if (bytes_available < bytes_needed) {
		return EINVAL;
	}

	kd_cpumap_header* header = (kd_cpumap_header*)(uintptr_t)*cpumap;

	header->version_no = RAW_VERSION1;
	header->cpu_count = cpu_count;

	kd_cpumap* cpus = (kd_cpumap*)&header[1];

	int32_t index = cpu_count - 1;
	while (iops) {
		cpus[index].cpu_id = iops->cpu_id;
		cpus[index].flags = KDBG_CPUMAP_IS_IOP;
		bzero(cpus[index].name, sizeof(cpus->name));
		strlcpy(cpus[index].name, iops->callback.iop_name, sizeof(cpus->name));
		
		iops = iops->next;
		index--;
	}
	
	while (index >= 0) {
		cpus[index].cpu_id = index;
		cpus[index].flags = 0;
		bzero(cpus[index].name, sizeof(cpus->name));
		strlcpy(cpus[index].name, "AP", sizeof(cpus->name));

		index--;
	}
	
	return KERN_SUCCESS;
}

void
kdbg_thrmap_init(void)
{
        if (kd_ctrl_page.kdebug_flags & KDBG_MAPINIT)
		return;

	kd_mapptr = kdbg_thrmap_init_internal(0, &kd_mapsize, &kd_mapcount);

	if (kd_mapptr)
		kd_ctrl_page.kdebug_flags |= KDBG_MAPINIT;
}


kd_threadmap* kdbg_thrmap_init_internal(unsigned int count, unsigned int *mapsize, unsigned int *mapcount)
{
	kd_threadmap	*mapptr;
	struct proc	*p;
	struct krt	akrt;
	int		tts_count;    /* number of task-to-string structures */
	struct tts	*tts_mapptr;
	unsigned int	tts_mapsize = 0;
	int		i;
	vm_offset_t	kaddr;

	/*
	 * need to use PROC_SCANPROCLIST with proc_iterate
	 */
	proc_list_lock();

	/*
	 * Calculate the sizes of map buffers
	 */
	for (p = allproc.lh_first, *mapcount=0, tts_count=0; p; p = p->p_list.le_next) {
		*mapcount += get_task_numacts((task_t)p->task);
		tts_count++;
	}
	proc_list_unlock();

	/*
	 * The proc count could change during buffer allocation,
	 * so introduce a small fudge factor to bump up the
	 * buffer sizes. This gives new tasks some chance of 
	 * making into the tables.  Bump up by 25%.
	 */
	*mapcount += *mapcount/4;
	tts_count += tts_count/4;

	*mapsize = *mapcount * sizeof(kd_threadmap);

	if (count && count < *mapcount)
		return (0);

	if ((kmem_alloc(kernel_map, &kaddr, (vm_size_t)*mapsize) == KERN_SUCCESS)) {
		bzero((void *)kaddr, *mapsize);
		mapptr = (kd_threadmap *)kaddr;
	} else
		return (0);

	tts_mapsize = tts_count * sizeof(struct tts);

	if ((kmem_alloc(kernel_map, &kaddr, (vm_size_t)tts_mapsize) == KERN_SUCCESS)) {
		bzero((void *)kaddr, tts_mapsize);
		tts_mapptr = (struct tts *)kaddr;
	} else {
		kmem_free(kernel_map, (vm_offset_t)mapptr, *mapsize);

		return (0);
	}
	/* 
	 * We need to save the procs command string
	 * and take a reference for each task associated
	 * with a valid process
	 */

	proc_list_lock();

	/*
	 * should use proc_iterate
	 */
	for (p = allproc.lh_first, i=0; p && i < tts_count; p = p->p_list.le_next) {
		if (p->p_lflag & P_LEXIT)
			continue;

		if (p->task) {
			task_reference(p->task);
			tts_mapptr[i].task = p->task;
			tts_mapptr[i].pid  = p->p_pid;
			(void)strlcpy(tts_mapptr[i].task_comm, p->p_comm, sizeof(tts_mapptr[i].task_comm));
			i++;
		}
	}
	tts_count = i;

	proc_list_unlock();

	/*
	 * Initialize thread map data
	 */
	akrt.map = mapptr;
	akrt.count = 0;
	akrt.maxcount = *mapcount;
	    
	for (i = 0; i < tts_count; i++) {
		akrt.atts = &tts_mapptr[i];
		task_act_iterate_wth_args(tts_mapptr[i].task, kdbg_resolve_map, &akrt);
		task_deallocate((task_t) tts_mapptr[i].task);
	}
	kmem_free(kernel_map, (vm_offset_t)tts_mapptr, tts_mapsize);

	*mapcount = akrt.count;

	return (mapptr);
}

static void
kdbg_clear(void)
{
        /*
	 * Clean up the trace buffer
	 * First make sure we're not in
	 * the middle of cutting a trace
	 */
	kdbg_set_tracing_enabled(FALSE, KDEBUG_ENABLE_TRACE);

	/*
	 * make sure the SLOW_NOLOG is seen
	 * by everyone that might be trying
	 * to cut a trace..
	 */
	IOSleep(100);

        global_state_pid = -1;
	kd_ctrl_page.kdebug_flags &= (unsigned int)~KDBG_CKTYPES;
	kd_ctrl_page.kdebug_flags &= ~(KDBG_NOWRAP | KDBG_RANGECHECK | KDBG_VALCHECK);
	kd_ctrl_page.kdebug_flags &= ~(KDBG_PIDCHECK | KDBG_PIDEXCLUDE);
	
	kdbg_disable_typefilter();

	delete_buffers();
	nkdbufs	= 0;

	/* Clean up the thread map buffer */
	kd_ctrl_page.kdebug_flags &= ~KDBG_MAPINIT;
	if (kd_mapptr) {
		kmem_free(kernel_map, (vm_offset_t)kd_mapptr, kd_mapsize);
		kd_mapptr = (kd_threadmap *) 0;
	}
	kd_mapsize = 0;
	kd_mapcount = 0;

	RAW_file_offset = 0;
	RAW_file_written = 0;
}

int
kdbg_setpid(kd_regtype *kdr)
{
	pid_t pid;
	int flag, ret=0;
	struct proc *p;

	pid = (pid_t)kdr->value1;
	flag = (int)kdr->value2;

	if (pid > 0) {
		if ((p = proc_find(pid)) == NULL)
			ret = ESRCH;
		else {
			if (flag == 1) {
				/*
				 * turn on pid check for this and all pids
				 */
				kd_ctrl_page.kdebug_flags |= KDBG_PIDCHECK;
				kd_ctrl_page.kdebug_flags &= ~KDBG_PIDEXCLUDE;
				kdbg_set_flags(SLOW_CHECKS, 0, TRUE);

				p->p_kdebug = 1;
			} else {
				/*
				 * turn off pid check for this pid value
				 * Don't turn off all pid checking though
				 *
				 * kd_ctrl_page.kdebug_flags &= ~KDBG_PIDCHECK;
				 */   
				p->p_kdebug = 0;
			}
			proc_rele(p);
		}
	}
	else
		ret = EINVAL;

	return(ret);
}

/* This is for pid exclusion in the trace buffer */
int
kdbg_setpidex(kd_regtype *kdr)
{
	pid_t pid;
	int flag, ret=0;
	struct proc *p;

	pid = (pid_t)kdr->value1;
	flag = (int)kdr->value2;

	if (pid > 0) {
		if ((p = proc_find(pid)) == NULL)
			ret = ESRCH;
		else {
			if (flag == 1) {
				/*
				 * turn on pid exclusion
				 */
				kd_ctrl_page.kdebug_flags |= KDBG_PIDEXCLUDE;
				kd_ctrl_page.kdebug_flags &= ~KDBG_PIDCHECK;
				kdbg_set_flags(SLOW_CHECKS, 0, TRUE);

				p->p_kdebug = 1;
			}
			else {
				/*
				 * turn off pid exclusion for this pid value
				 * Don't turn off all pid exclusion though
				 *
				 * kd_ctrl_page.kdebug_flags &= ~KDBG_PIDEXCLUDE;
				 */   
				p->p_kdebug = 0;
			}
			proc_rele(p);
		}
	} else
		ret = EINVAL;

	return(ret);
}


/*
 * This is for setting a maximum decrementer value
 */
int
kdbg_setrtcdec(kd_regtype *kdr)
{
	int ret = 0;
	natural_t decval;

	decval = (natural_t)kdr->value1;

	if (decval && decval < KDBG_MINRTCDEC)
		ret = EINVAL;
	else
		ret = ENOTSUP;

	return(ret);
}

int
kdbg_enable_typefilter(void)
{
	if (kd_ctrl_page.kdebug_flags & KDBG_TYPEFILTER_CHECK) {
		/* free the old filter */
		kdbg_disable_typefilter();
	}
	
	if (kmem_alloc(kernel_map, (vm_offset_t *)&type_filter_bitmap, KDBG_TYPEFILTER_BITMAP_SIZE) != KERN_SUCCESS) {
		return ENOSPC;
	}
	
	bzero(type_filter_bitmap, KDBG_TYPEFILTER_BITMAP_SIZE);

	/* Turn off range and value checks */
	kd_ctrl_page.kdebug_flags &= ~(KDBG_RANGECHECK | KDBG_VALCHECK);
	
	/* Enable filter checking */
	kd_ctrl_page.kdebug_flags |= KDBG_TYPEFILTER_CHECK;
	kdbg_set_flags(SLOW_CHECKS, 0, TRUE);
	return 0;
}

int
kdbg_disable_typefilter(void)
{
	/*  Disable filter checking */	
	kd_ctrl_page.kdebug_flags &= ~KDBG_TYPEFILTER_CHECK;
	
	/*  Turn off slow checks unless pid checks are using them */
	if ( (kd_ctrl_page.kdebug_flags & (KDBG_PIDCHECK | KDBG_PIDEXCLUDE)) )
		kdbg_set_flags(SLOW_CHECKS, 0, TRUE);
	else
		kdbg_set_flags(SLOW_CHECKS, 0, FALSE);
	
	if(type_filter_bitmap == NULL)
		return 0;

	vm_offset_t old_bitmap = (vm_offset_t)type_filter_bitmap;
	type_filter_bitmap = NULL;

	kmem_free(kernel_map, old_bitmap, KDBG_TYPEFILTER_BITMAP_SIZE);
	return 0;
}

int
kdbg_setreg(kd_regtype * kdr)
{
	int ret=0;
	unsigned int val_1, val_2, val;
	switch (kdr->type) {
	
	case KDBG_CLASSTYPE :
		val_1 = (kdr->value1 & 0xff);
		val_2 = (kdr->value2 & 0xff);
		kdlog_beg = (val_1<<24);
		kdlog_end = (val_2<<24);
		kd_ctrl_page.kdebug_flags &= (unsigned int)~KDBG_CKTYPES;
		kd_ctrl_page.kdebug_flags &= ~KDBG_VALCHECK;       /* Turn off specific value check  */
		kd_ctrl_page.kdebug_flags |= (KDBG_RANGECHECK | KDBG_CLASSTYPE);
		kdbg_set_flags(SLOW_CHECKS, 0, TRUE);
		break;
	case KDBG_SUBCLSTYPE :
		val_1 = (kdr->value1 & 0xff);
		val_2 = (kdr->value2 & 0xff);
		val = val_2 + 1;
		kdlog_beg = ((val_1<<24) | (val_2 << 16));
		kdlog_end = ((val_1<<24) | (val << 16));
		kd_ctrl_page.kdebug_flags &= (unsigned int)~KDBG_CKTYPES;
		kd_ctrl_page.kdebug_flags &= ~KDBG_VALCHECK;       /* Turn off specific value check  */
		kd_ctrl_page.kdebug_flags |= (KDBG_RANGECHECK | KDBG_SUBCLSTYPE);
		kdbg_set_flags(SLOW_CHECKS, 0, TRUE);
		break;
	case KDBG_RANGETYPE :
		kdlog_beg = (kdr->value1);
		kdlog_end = (kdr->value2);
		kd_ctrl_page.kdebug_flags &= (unsigned int)~KDBG_CKTYPES;
		kd_ctrl_page.kdebug_flags &= ~KDBG_VALCHECK;       /* Turn off specific value check  */
		kd_ctrl_page.kdebug_flags |= (KDBG_RANGECHECK | KDBG_RANGETYPE);
		kdbg_set_flags(SLOW_CHECKS, 0, TRUE);
		break;
	case KDBG_VALCHECK:
		kdlog_value1 = (kdr->value1);
		kdlog_value2 = (kdr->value2);
		kdlog_value3 = (kdr->value3);
		kdlog_value4 = (kdr->value4);
		kd_ctrl_page.kdebug_flags &= (unsigned int)~KDBG_CKTYPES;
		kd_ctrl_page.kdebug_flags &= ~KDBG_RANGECHECK;    /* Turn off range check */
		kd_ctrl_page.kdebug_flags |= KDBG_VALCHECK;       /* Turn on specific value check  */
		kdbg_set_flags(SLOW_CHECKS, 0, TRUE);
		break;
	case KDBG_TYPENONE :
		kd_ctrl_page.kdebug_flags &= (unsigned int)~KDBG_CKTYPES;

		if ( (kd_ctrl_page.kdebug_flags & (KDBG_RANGECHECK | KDBG_VALCHECK   | 
						   KDBG_PIDCHECK   | KDBG_PIDEXCLUDE | 
						   KDBG_TYPEFILTER_CHECK)) )
			kdbg_set_flags(SLOW_CHECKS, 0, TRUE);
		else
			kdbg_set_flags(SLOW_CHECKS, 0, FALSE);

		kdlog_beg = 0;
		kdlog_end = 0;
		break;
	default :
		ret = EINVAL;
		break;
	}
	return(ret);
}

int
kdbg_getreg(__unused kd_regtype * kdr)
{
#if 0	
	int i,j, ret=0;
	unsigned int val_1, val_2, val;

	switch (kdr->type) {
	case KDBG_CLASSTYPE :
		val_1 = (kdr->value1 & 0xff);
		val_2 = val_1 + 1;
		kdlog_beg = (val_1<<24);
		kdlog_end = (val_2<<24);
		kd_ctrl_page.kdebug_flags &= (unsigned int)~KDBG_CKTYPES;
		kd_ctrl_page.kdebug_flags |= (KDBG_RANGECHECK | KDBG_CLASSTYPE);
		break;
	case KDBG_SUBCLSTYPE :
		val_1 = (kdr->value1 & 0xff);
		val_2 = (kdr->value2 & 0xff);
		val = val_2 + 1;
		kdlog_beg = ((val_1<<24) | (val_2 << 16));
		kdlog_end = ((val_1<<24) | (val << 16));
		kd_ctrl_page.kdebug_flags &= (unsigned int)~KDBG_CKTYPES;
		kd_ctrl_page.kdebug_flags |= (KDBG_RANGECHECK | KDBG_SUBCLSTYPE);
		break;
	case KDBG_RANGETYPE :
		kdlog_beg = (kdr->value1);
		kdlog_end = (kdr->value2);
		kd_ctrl_page.kdebug_flags &= (unsigned int)~KDBG_CKTYPES;
		kd_ctrl_page.kdebug_flags |= (KDBG_RANGECHECK | KDBG_RANGETYPE);
		break;
	case KDBG_TYPENONE :
		kd_ctrl_page.kdebug_flags &= (unsigned int)~KDBG_CKTYPES;
		kdlog_beg = 0;
		kdlog_end = 0;
		break;
	default :
		ret = EINVAL;
		break;
	}
#endif /* 0 */
	return(EINVAL);
}

int
kdbg_readcpumap(user_addr_t user_cpumap, size_t *user_cpumap_size)
{
	uint8_t* cpumap = NULL;
	uint32_t cpumap_size = 0;
	int ret = KERN_SUCCESS;

	if (kd_ctrl_page.kdebug_flags & KDBG_BUFINIT) {
		if (kdbg_cpumap_init_internal(kd_ctrl_page.kdebug_iops, kd_ctrl_page.kdebug_cpus, &cpumap, &cpumap_size) == KERN_SUCCESS) {
			if (user_cpumap) {
				size_t bytes_to_copy = (*user_cpumap_size >= cpumap_size) ? cpumap_size : *user_cpumap_size;
				if (copyout(cpumap, user_cpumap, (size_t)bytes_to_copy)) {
					ret = EFAULT;
				}
			}
			*user_cpumap_size = cpumap_size;
			kmem_free(kernel_map, (vm_offset_t)cpumap, cpumap_size);
		} else
			ret = EINVAL;
	} else
		ret = EINVAL;

	return (ret);
}

int
kdbg_readcurthrmap(user_addr_t buffer, size_t *bufsize)
{
	kd_threadmap *mapptr;
	unsigned int mapsize;
	unsigned int mapcount;
	unsigned int count = 0;
	int ret = 0;

	count = *bufsize/sizeof(kd_threadmap);
	*bufsize = 0;

	if ( (mapptr = kdbg_thrmap_init_internal(count, &mapsize, &mapcount)) ) {
		if (copyout(mapptr, buffer, mapcount * sizeof(kd_threadmap)))
			ret = EFAULT;
		else
			*bufsize = (mapcount * sizeof(kd_threadmap));

		kmem_free(kernel_map, (vm_offset_t)mapptr, mapsize);
	} else
		ret = EINVAL;

	return (ret);
}

int
kdbg_readthrmap(user_addr_t buffer, size_t *number, vnode_t vp, vfs_context_t ctx)
{
	int avail = *number;
	int ret = 0;
	uint32_t count = 0;
	unsigned int mapsize;

	count = avail/sizeof (kd_threadmap);

	mapsize = kd_mapcount * sizeof(kd_threadmap);

	if (count && (count <= kd_mapcount))
	{
		if ((kd_ctrl_page.kdebug_flags & KDBG_MAPINIT) && kd_mapsize && kd_mapptr)
		{
			if (*number < mapsize)
				ret = EINVAL;
			else
			{
				if (vp)
				{
					RAW_header	header;
					clock_sec_t	secs;
					clock_usec_t	usecs;
					char	*pad_buf;
					uint32_t pad_size;
					uint32_t extra_thread_count = 0;
					uint32_t cpumap_size;
					
					/*
					 * To write a RAW_VERSION1+ file, we
					 * must embed a cpumap in the "padding"
					 * used to page align the events folloing
					 * the threadmap. If the threadmap happens
					 * to not require enough padding, we
					 * artificially increase its footprint
					 * until it needs enough padding.
					 */

					pad_size = PAGE_SIZE - ((sizeof(RAW_header) + (count * sizeof(kd_threadmap))) & PAGE_MASK_64);
					cpumap_size = sizeof(kd_cpumap_header) + kd_ctrl_page.kdebug_cpus * sizeof(kd_cpumap);

					if (cpumap_size > pad_size) {
						/* Force an overflow onto the next page, we get a full page of padding */
						extra_thread_count = (pad_size / sizeof(kd_threadmap)) + 1;
					}

					header.version_no = RAW_VERSION1;
					header.thread_count = count + extra_thread_count;

					clock_get_calendar_microtime(&secs, &usecs);
					header.TOD_secs = secs;
					header.TOD_usecs = usecs;

					ret = vn_rdwr(UIO_WRITE, vp, (caddr_t)&header, sizeof(RAW_header), RAW_file_offset,
						      UIO_SYSSPACE, IO_NODELOCKED|IO_UNIT, vfs_context_ucred(ctx), (int *) 0, vfs_context_proc(ctx));
					if (ret)
						goto write_error;
					RAW_file_offset += sizeof(RAW_header);

					ret = vn_rdwr(UIO_WRITE, vp, (caddr_t)kd_mapptr, mapsize, RAW_file_offset,
						      UIO_SYSSPACE, IO_NODELOCKED|IO_UNIT, vfs_context_ucred(ctx), (int *) 0, vfs_context_proc(ctx));
					if (ret)
						goto write_error;
					RAW_file_offset += mapsize;

					if (extra_thread_count) {
						pad_size = extra_thread_count * sizeof(kd_threadmap);
						pad_buf = (char *)kalloc(pad_size);
						memset(pad_buf, 0, pad_size);

						ret = vn_rdwr(UIO_WRITE, vp, (caddr_t)pad_buf, pad_size, RAW_file_offset,
							      UIO_SYSSPACE, IO_NODELOCKED|IO_UNIT, vfs_context_ucred(ctx), (int *) 0, vfs_context_proc(ctx));
						kfree(pad_buf, pad_size);

						if (ret)
							goto write_error;
						RAW_file_offset += pad_size;

					}
					
					pad_size = PAGE_SIZE - (RAW_file_offset & PAGE_MASK_64);
					if (pad_size) {
						pad_buf = (char *)kalloc(pad_size);
						memset(pad_buf, 0, pad_size);

						/*
						 * embed a cpumap in the padding bytes.
						 * older code will skip this.
						 * newer code will know how to read it.
						 */
						uint32_t temp = pad_size;
						if (kdbg_cpumap_init_internal(kd_ctrl_page.kdebug_iops, kd_ctrl_page.kdebug_cpus, (uint8_t**)&pad_buf, &temp) != KERN_SUCCESS) {
							memset(pad_buf, 0, pad_size);
						}

						ret = vn_rdwr(UIO_WRITE, vp, (caddr_t)pad_buf, pad_size, RAW_file_offset,
							      UIO_SYSSPACE, IO_NODELOCKED|IO_UNIT, vfs_context_ucred(ctx), (int *) 0, vfs_context_proc(ctx));
						kfree(pad_buf, pad_size);

						if (ret)
							goto write_error;
						RAW_file_offset += pad_size;
					}
					RAW_file_written += sizeof(RAW_header) + mapsize + pad_size;

				} else {
					if (copyout(kd_mapptr, buffer, mapsize))
						ret = EINVAL;
				}
			}
		}
		else
			ret = EINVAL;
	}
	else
		ret = EINVAL;

	if (ret && vp)
	{
		count = 0;

		vn_rdwr(UIO_WRITE, vp, (caddr_t)&count, sizeof(uint32_t), RAW_file_offset,
			UIO_SYSSPACE, IO_NODELOCKED|IO_UNIT, vfs_context_ucred(ctx), (int *) 0, vfs_context_proc(ctx));
		RAW_file_offset += sizeof(uint32_t);
		RAW_file_written += sizeof(uint32_t);
	}
write_error:
	if ((kd_ctrl_page.kdebug_flags & KDBG_MAPINIT) && kd_mapsize && kd_mapptr)
	{
		kmem_free(kernel_map, (vm_offset_t)kd_mapptr, kd_mapsize);
		kd_ctrl_page.kdebug_flags &= ~KDBG_MAPINIT;
		kd_mapsize = 0;
		kd_mapptr = (kd_threadmap *) 0;
		kd_mapcount = 0;
	}  
	return(ret);
}


static int
kdbg_set_nkdbufs(unsigned int value)
{
        /*
	 * We allow a maximum buffer size of 50% of either ram or max mapped address, whichever is smaller
	 * 'value' is the desired number of trace entries
	 */
        unsigned int max_entries = (sane_size/2) / sizeof(kd_buf);

	if (value <= max_entries)
		return (value);
	else
		return (max_entries);
}


static int
kdbg_enable_bg_trace(void)
{
	int ret = 0;

	if (kdlog_bg_trace == TRUE && kdlog_bg_trace_running == FALSE && n_storage_buffers == 0) {
		nkdbufs = bg_nkdbufs;
		ret = kdbg_reinit(FALSE);
		if (0 == ret) {
			kdbg_set_tracing_enabled(TRUE, KDEBUG_ENABLE_TRACE);
			kdlog_bg_trace_running = TRUE;
		}
	}
	return ret;
}

static void
kdbg_disable_bg_trace(void)
{
	if (kdlog_bg_trace_running == TRUE) {
		kdlog_bg_trace_running = FALSE;
		kdbg_clear();		
	}
}



/*
 * This function is provided for the CHUD toolkit only.
 *    int val:
 *        zero disables kdebug_chudhook function call
 *        non-zero enables kdebug_chudhook function call
 *    char *fn:
 *        address of the enabled kdebug_chudhook function
*/

void
kdbg_control_chud(int val, void *fn)
{
	kdbg_lock_init();
    
	if (val) {
		/* enable chudhook */
		kdebug_chudhook = fn;
		kdbg_set_flags(SLOW_CHUD, KDEBUG_ENABLE_CHUD, TRUE);
	}
	else {
		/* disable chudhook */
		kdbg_set_flags(SLOW_CHUD, KDEBUG_ENABLE_CHUD, FALSE);
		kdebug_chudhook = 0;
	}
}

	
int
kdbg_control(int *name, u_int namelen, user_addr_t where, size_t *sizep)
{
	int ret = 0;
	size_t size = *sizep;
	unsigned int value = 0;
	kd_regtype kd_Reg;
	kbufinfo_t kd_bufinfo;
	pid_t curpid;
	proc_t p, curproc;

	if (name[0] == KERN_KDGETENTROPY ||
		name[0] == KERN_KDWRITETR ||
		name[0] == KERN_KDWRITEMAP ||
		name[0] == KERN_KDEFLAGS ||
		name[0] == KERN_KDDFLAGS ||
		name[0] == KERN_KDENABLE ||
		name[0] == KERN_KDENABLE_BG_TRACE ||
		name[0] == KERN_KDSETBUF) {
		
		if ( namelen < 2 )
			return(EINVAL);
		value = name[1];
	}
	
	kdbg_lock_init();

	if ( !(kd_ctrl_page.kdebug_flags & KDBG_LOCKINIT))
		return(ENOSPC);

	lck_mtx_lock(kd_trace_mtx_sysctl);

	switch(name[0]) {
		case KERN_KDGETBUF:
			/*
			 * Does not alter the global_state_pid
			 * This is a passive request.
			 */
			if (size < sizeof(kd_bufinfo.nkdbufs)) {
				/*
				 * There is not enough room to return even
				 * the first element of the info structure.
				 */
				ret = EINVAL;
				goto out;
			}
			kd_bufinfo.nkdbufs = nkdbufs;
			kd_bufinfo.nkdthreads = kd_mapcount;
			
			if ( (kd_ctrl_page.kdebug_slowcheck & SLOW_NOLOG) )
				kd_bufinfo.nolog = 1;
			else
				kd_bufinfo.nolog = 0;
			
			kd_bufinfo.flags = kd_ctrl_page.kdebug_flags;
#if defined(__LP64__)
			kd_bufinfo.flags |= KDBG_LP64;
#endif
			kd_bufinfo.bufid = global_state_pid;
			
			if (size >= sizeof(kd_bufinfo)) {
				/*
				 * Provide all the info we have
				 */
				if (copyout(&kd_bufinfo, where, sizeof(kd_bufinfo)))
					ret = EINVAL;
			} else {
				/*
				 * For backwards compatibility, only provide
				 * as much info as there is room for.
				 */
				if (copyout(&kd_bufinfo, where, size))
					ret = EINVAL;
			}
			goto out;
			
		case KERN_KDGETENTROPY: {
			/* Obsolescent - just fake with a random buffer */
			char	*buffer = (char *) kalloc(size);
			read_frandom((void *) buffer, size);
			ret = copyout(buffer, where, size);
			kfree(buffer, size);
			goto out;
		}
			
		case KERN_KDENABLE_BG_TRACE:
			bg_nkdbufs = kdbg_set_nkdbufs(value);
			kdlog_bg_trace = TRUE;
			ret = kdbg_enable_bg_trace();
			goto out;
			
		case KERN_KDDISABLE_BG_TRACE:
			kdlog_bg_trace = FALSE;
			kdbg_disable_bg_trace();
			goto out;
	}
	
	if ((curproc = current_proc()) != NULL)
		curpid = curproc->p_pid;
	else {
		ret = ESRCH;
		goto out;
	}
	if (global_state_pid == -1)
		global_state_pid = curpid;
	else if (global_state_pid != curpid) {
		if ((p = proc_find(global_state_pid)) == NULL) {
			/*
			 * The global pid no longer exists
			 */
			global_state_pid = curpid;
		} else {
			/*
			 * The global pid exists, deny this request
			 */
			proc_rele(p);

			ret = EBUSY;
			goto out;
		}
	}

	switch(name[0]) {
		case KERN_KDEFLAGS:
			kdbg_disable_bg_trace();

			value &= KDBG_USERFLAGS;
			kd_ctrl_page.kdebug_flags |= value;
			break;
		case KERN_KDDFLAGS:
			kdbg_disable_bg_trace();

			value &= KDBG_USERFLAGS;
			kd_ctrl_page.kdebug_flags &= ~value;
			break;
		case KERN_KDENABLE:
			/*
			 * Enable tracing mechanism.  Two types:
			 * KDEBUG_TRACE is the standard one,
			 * and KDEBUG_PPT which is a carefully
			 * chosen subset to avoid performance impact.
			 */
			if (value) {
				/*
				 * enable only if buffer is initialized
				 */
				if (!(kd_ctrl_page.kdebug_flags & KDBG_BUFINIT) || 
				    !(value == KDEBUG_ENABLE_TRACE || value == KDEBUG_ENABLE_PPT)) {
					ret = EINVAL;
					break;
				}
				kdbg_thrmap_init();

				kdbg_set_tracing_enabled(TRUE, value);
			}
			else
			{
				kdbg_set_tracing_enabled(FALSE, 0);
			}
			break;
		case KERN_KDSETBUF:
			kdbg_disable_bg_trace();

			nkdbufs = kdbg_set_nkdbufs(value);
			break;
		case KERN_KDSETUP:
			kdbg_disable_bg_trace();

			ret = kdbg_reinit(FALSE);
			break;
		case KERN_KDREMOVE:
			kdbg_clear();
			ret = kdbg_enable_bg_trace();
			break;
		case KERN_KDSETREG:
			if(size < sizeof(kd_regtype)) {
				ret = EINVAL;
				break;
			}
			if (copyin(where, &kd_Reg, sizeof(kd_regtype))) {
				ret = EINVAL;
				break;
			}
			kdbg_disable_bg_trace();

			ret = kdbg_setreg(&kd_Reg);
			break;
		case KERN_KDGETREG:
			if (size < sizeof(kd_regtype)) {
				ret = EINVAL;
				break;
			}
			ret = kdbg_getreg(&kd_Reg);
			if (copyout(&kd_Reg, where, sizeof(kd_regtype))) {
				ret = EINVAL;
			}
			kdbg_disable_bg_trace();

			break;
		case KERN_KDREADTR:
			ret = kdbg_read(where, sizep, NULL, NULL);
			break;
		case KERN_KDWRITETR:
		case KERN_KDWRITEMAP:
		{
			struct	vfs_context context;
			struct	fileproc *fp;
			size_t	number;
			vnode_t	vp;
			int	fd;

			kdbg_disable_bg_trace();

			if (name[0] == KERN_KDWRITETR) {
				int s;
				int wait_result = THREAD_AWAKENED;
				u_int64_t abstime;
				u_int64_t ns;

				if (*sizep) {
					ns = ((u_int64_t)*sizep) * (u_int64_t)(1000 * 1000);
					nanoseconds_to_absolutetime(ns,  &abstime );
					clock_absolutetime_interval_to_deadline( abstime, &abstime );
				} else
					abstime = 0;

				s = ml_set_interrupts_enabled(FALSE);
				lck_spin_lock(kdw_spin_lock);

				while (wait_result == THREAD_AWAKENED && kd_ctrl_page.kds_inuse_count < n_storage_threshold) {

					kds_waiter = 1;

					if (abstime)
						wait_result = lck_spin_sleep_deadline(kdw_spin_lock, 0, &kds_waiter, THREAD_ABORTSAFE, abstime);
					else
						wait_result = lck_spin_sleep(kdw_spin_lock, 0, &kds_waiter, THREAD_ABORTSAFE);
					
					kds_waiter = 0;
				}
				lck_spin_unlock(kdw_spin_lock);
				ml_set_interrupts_enabled(s);
			}
			p = current_proc();
			fd = value;

			proc_fdlock(p);
			if ( (ret = fp_lookup(p, fd, &fp, 1)) ) {
				proc_fdunlock(p);
				break;
			}
			context.vc_thread = current_thread();
			context.vc_ucred = fp->f_fglob->fg_cred;

			if (FILEGLOB_DTYPE(fp->f_fglob) != DTYPE_VNODE) {
				fp_drop(p, fd, fp, 1);
				proc_fdunlock(p);

				ret = EBADF;
				break;
			}
			vp = (struct vnode *)fp->f_fglob->fg_data;
			proc_fdunlock(p);

			if ((ret = vnode_getwithref(vp)) == 0) {
				RAW_file_offset = fp->f_fglob->fg_offset;
				if (name[0] == KERN_KDWRITETR) {
					number = nkdbufs * sizeof(kd_buf);

					KERNEL_DEBUG_CONSTANT((TRACEDBG_CODE(DBG_TRACE_INFO, 3)) | DBG_FUNC_START, 0, 0, 0, 0, 0);
					ret = kdbg_read(0, &number, vp, &context);
					KERNEL_DEBUG_CONSTANT((TRACEDBG_CODE(DBG_TRACE_INFO, 3)) | DBG_FUNC_END, number, 0, 0, 0, 0);

					*sizep = number;
				} else {
					number = kd_mapcount * sizeof(kd_threadmap);
					kdbg_readthrmap(0, &number, vp, &context);
				}
				fp->f_fglob->fg_offset = RAW_file_offset;
				vnode_put(vp);
			}
			fp_drop(p, fd, fp, 0);

			break;
		}
		case KERN_KDBUFWAIT:
		{
			/* WRITETR lite -- just block until there's data */
			int s;
			int wait_result = THREAD_AWAKENED;
			u_int64_t abstime;
			u_int64_t ns;
			size_t	number = 0;

			kdbg_disable_bg_trace();


			if (*sizep) {
				ns = ((u_int64_t)*sizep) * (u_int64_t)(1000 * 1000);
				nanoseconds_to_absolutetime(ns,  &abstime );
				clock_absolutetime_interval_to_deadline( abstime, &abstime );
			} else
				abstime = 0;

			s = ml_set_interrupts_enabled(FALSE);
			if( !s )
				panic("trying to wait with interrupts off");
			lck_spin_lock(kdw_spin_lock);

			/* drop the mutex so don't exclude others from
			 * accessing trace
			 */
			lck_mtx_unlock(kd_trace_mtx_sysctl);

			while (wait_result == THREAD_AWAKENED &&
				kd_ctrl_page.kds_inuse_count < n_storage_threshold) {

				kds_waiter = 1;

				if (abstime)
					wait_result = lck_spin_sleep_deadline(kdw_spin_lock, 0, &kds_waiter, THREAD_ABORTSAFE, abstime);
				else
					wait_result = lck_spin_sleep(kdw_spin_lock, 0, &kds_waiter, THREAD_ABORTSAFE);
				
				kds_waiter = 0;
			}

			/* check the count under the spinlock */
			number = (kd_ctrl_page.kds_inuse_count >= n_storage_threshold);

			lck_spin_unlock(kdw_spin_lock);
			ml_set_interrupts_enabled(s);

			/* pick the mutex back up again */
			lck_mtx_lock(kd_trace_mtx_sysctl);

			/* write out whether we've exceeded the threshold */
			*sizep = number;
			break;
		}
		case KERN_KDPIDTR:
			if (size < sizeof(kd_regtype)) {
				ret = EINVAL;
				break;
			}
			if (copyin(where, &kd_Reg, sizeof(kd_regtype))) {
				ret = EINVAL;
				break;
			}
			kdbg_disable_bg_trace();

			ret = kdbg_setpid(&kd_Reg);
			break;
		case KERN_KDPIDEX:
			if (size < sizeof(kd_regtype)) {
				ret = EINVAL;
				break;
			}
			if (copyin(where, &kd_Reg, sizeof(kd_regtype))) {
				ret = EINVAL;
				break;
			}
			kdbg_disable_bg_trace();

			ret = kdbg_setpidex(&kd_Reg);
			break;
		case KERN_KDCPUMAP:
			ret = kdbg_readcpumap(where, sizep);
			break;
		case KERN_KDTHRMAP:
			ret = kdbg_readthrmap(where, sizep, NULL, NULL);
			break;
		case KERN_KDREADCURTHRMAP:
			ret = kdbg_readcurthrmap(where, sizep);
			break;
		case KERN_KDSETRTCDEC:
			if (size < sizeof(kd_regtype)) {
				ret = EINVAL;
				break;
			}
			if (copyin(where, &kd_Reg, sizeof(kd_regtype))) {
				ret = EINVAL;
				break;
			}
			kdbg_disable_bg_trace();

			ret = kdbg_setrtcdec(&kd_Reg);
			break;
		case KERN_KDSET_TYPEFILTER:
			kdbg_disable_bg_trace();

			if ((kd_ctrl_page.kdebug_flags & KDBG_TYPEFILTER_CHECK) == 0){
				if ((ret = kdbg_enable_typefilter()))
					break;
			}

			if (size != KDBG_TYPEFILTER_BITMAP_SIZE) {
				ret = EINVAL;
				break;
			}

			if (copyin(where, type_filter_bitmap, KDBG_TYPEFILTER_BITMAP_SIZE)) {
				ret = EINVAL;
				break;
			}
			kdbg_iop_list_callback(kd_ctrl_page.kdebug_iops, KD_CALLBACK_TYPEFILTER_CHANGED, type_filter_bitmap);
			break;
		default:
			ret = EINVAL;
	}
out:
	lck_mtx_unlock(kd_trace_mtx_sysctl);

	return(ret);
}


/*
 * This code can run for the most part concurrently with kernel_debug_internal()...
 * 'release_storage_unit' will take the kds_spin_lock which may cause us to briefly
 * synchronize with the recording side of this puzzle... otherwise, we are able to
 * move through the lists w/o use of any locks
 */
int
kdbg_read(user_addr_t buffer, size_t *number, vnode_t vp, vfs_context_t ctx)
{
	unsigned int count;
	unsigned int cpu, min_cpu;
	uint64_t  mintime, t, barrier = 0;
	int error = 0;
	kd_buf *tempbuf;
	uint32_t rcursor;
	kd_buf lostevent;
	union kds_ptr kdsp;
	struct kd_storage *kdsp_actual;
	struct kd_bufinfo *kdbp;
	struct kd_bufinfo *min_kdbp;
	uint32_t tempbuf_count;
	uint32_t tempbuf_number;
	uint32_t old_kdebug_flags;
	uint32_t old_kdebug_slowcheck;
	boolean_t lostevents = FALSE;
	boolean_t out_of_events = FALSE;

	count = *number/sizeof(kd_buf);
	*number = 0;

	if (count == 0 || !(kd_ctrl_page.kdebug_flags & KDBG_BUFINIT) || kdcopybuf == 0)
		return EINVAL;

	memset(&lostevent, 0, sizeof(lostevent));
	lostevent.debugid = TRACEDBG_CODE(DBG_TRACE_INFO, 2);

	/* Capture timestamp. Only sort events that have occured before the timestamp.
	 * Since the iop is being flushed here, its possible that events occur on the AP
	 * while running live tracing. If we are disabled, no new events should 
	 * occur on the AP.
	*/
	
	if (kd_ctrl_page.enabled)
	{
		// timestamp is non-zero value
		barrier = mach_absolute_time() & KDBG_TIMESTAMP_MASK;
	}
	
	// Request each IOP to provide us with up to date entries before merging buffers together.
	kdbg_iop_list_callback(kd_ctrl_page.kdebug_iops, KD_CALLBACK_SYNC_FLUSH, NULL);

	/*
	 * because we hold kd_trace_mtx_sysctl, no other control threads can 
	 * be playing with kdebug_flags... the code that cuts new events could
	 * be running, but it grabs kds_spin_lock if it needs to acquire a new
	 * storage chunk which is where it examines kdebug_flags... it its adding
	 * to the same chunk we're reading from, no problem... 
	 */

	disable_wrap(&old_kdebug_slowcheck, &old_kdebug_flags);

	if (count > nkdbufs)
		count = nkdbufs;

	if ((tempbuf_count = count) > KDCOPYBUF_COUNT)
	        tempbuf_count = KDCOPYBUF_COUNT;

	while (count) {
		tempbuf = kdcopybuf;
		tempbuf_number = 0;

		// While space
		while (tempbuf_count) {
			mintime = 0xffffffffffffffffULL;
			min_kdbp = NULL;
			min_cpu = 0;

			// Check all CPUs
			for (cpu = 0, kdbp = &kdbip[0]; cpu < kd_ctrl_page.kdebug_cpus; cpu++, kdbp++) {

				// Find one with raw data
				if ((kdsp = kdbp->kd_list_head).raw == KDS_PTR_NULL)
				        continue;
				/* Debugging aid: maintain a copy of the "kdsp"
				 * index.
				 */
				volatile union kds_ptr kdsp_shadow;

				kdsp_shadow = kdsp;

				// Get from cpu data to buffer header to buffer
				kdsp_actual = POINTER_FROM_KDS_PTR(kdsp);

				volatile struct kd_storage *kdsp_actual_shadow;

				kdsp_actual_shadow = kdsp_actual;

				// See if there are actual data left in this buffer
				rcursor = kdsp_actual->kds_readlast;

				if (rcursor == kdsp_actual->kds_bufindx)
					continue;

				t = kdbg_get_timestamp(&kdsp_actual->kds_records[rcursor]);

				if ((t > barrier) && (barrier > 0)) {
					/* 
					 * Need to wait to flush iop again before we 
					 * sort any more data from the buffers
					*/
					out_of_events = TRUE;
					break;
				}	
				if (t < kdsp_actual->kds_timestamp) {
					/*
					 * indicates we've not yet completed filling
					 * in this event...
					 * this should only occur when we're looking
					 * at the buf that the record head is utilizing
					 * we'll pick these events up on the next
					 * call to kdbg_read
					 * we bail at this point so that we don't
					 * get an out-of-order timestream by continuing
					 * to read events from the other CPUs' timestream(s)
					 */
					out_of_events = TRUE;
					break;
				}
				if (t < mintime) {
				        mintime = t;
					min_kdbp = kdbp;
					min_cpu = cpu;
				}
			}
			if (min_kdbp == NULL || out_of_events == TRUE) {
				/*
				 * all buffers ran empty
				 */
				out_of_events = TRUE;
				break;
			}

			// Get data
			kdsp = min_kdbp->kd_list_head;
			kdsp_actual = POINTER_FROM_KDS_PTR(kdsp);

			if (kdsp_actual->kds_lostevents == TRUE) {
				kdbg_set_timestamp_and_cpu(&lostevent, kdsp_actual->kds_records[kdsp_actual->kds_readlast].timestamp, min_cpu);
				*tempbuf = lostevent;
				
				kdsp_actual->kds_lostevents = FALSE;
				lostevents = TRUE;

				goto nextevent;
			}

			// Copy into buffer
			*tempbuf = kdsp_actual->kds_records[kdsp_actual->kds_readlast++];

			if (kdsp_actual->kds_readlast == EVENTS_PER_STORAGE_UNIT)
				release_storage_unit(min_cpu, kdsp.raw);

			/*
			 * Watch for out of order timestamps
			 */	
			if (mintime < min_kdbp->kd_prev_timebase) {
				/*
				 * if so, use the previous timestamp + 1 cycle
				 */
				min_kdbp->kd_prev_timebase++;
				kdbg_set_timestamp_and_cpu(tempbuf, min_kdbp->kd_prev_timebase, kdbg_get_cpu(tempbuf));
			} else
				min_kdbp->kd_prev_timebase = mintime;
nextevent:
			tempbuf_count--;
			tempbuf_number++;
			tempbuf++;

			if ((RAW_file_written += sizeof(kd_buf)) >= RAW_FLUSH_SIZE)
				break;
		}
		if (tempbuf_number) {

			if (vp) {
				error = vn_rdwr(UIO_WRITE, vp, (caddr_t)kdcopybuf, tempbuf_number * sizeof(kd_buf), RAW_file_offset,
						UIO_SYSSPACE, IO_NODELOCKED|IO_UNIT, vfs_context_ucred(ctx), (int *) 0, vfs_context_proc(ctx));

				RAW_file_offset += (tempbuf_number * sizeof(kd_buf));
	
				if (RAW_file_written >= RAW_FLUSH_SIZE) {
					cluster_push(vp, 0);

					RAW_file_written = 0;
				}
			} else {
				error = copyout(kdcopybuf, buffer, tempbuf_number * sizeof(kd_buf));
				buffer += (tempbuf_number * sizeof(kd_buf));
			}
			if (error) {
				*number = 0;
				error = EINVAL;
				break;
			}
			count   -= tempbuf_number;
			*number += tempbuf_number;
		}
		if (out_of_events == TRUE)
		       /*
			* all trace buffers are empty
			*/
		        break;

		if ((tempbuf_count = count) > KDCOPYBUF_COUNT)
		        tempbuf_count = KDCOPYBUF_COUNT;
	}
	if ( !(old_kdebug_flags & KDBG_NOWRAP)) {
		enable_wrap(old_kdebug_slowcheck, lostevents);
	}
	return (error);
}


unsigned char *getProcName(struct proc *proc);
unsigned char *getProcName(struct proc *proc) {

	return (unsigned char *) &proc->p_comm;	/* Return pointer to the proc name */

}

#define STACKSHOT_SUBSYS_LOCK() lck_mtx_lock(&stackshot_subsys_mutex)
#define STACKSHOT_SUBSYS_UNLOCK() lck_mtx_unlock(&stackshot_subsys_mutex)
#if defined(__i386__) || defined (__x86_64__)
#define TRAP_DEBUGGER __asm__ volatile("int3");
#else
#error No TRAP_DEBUGGER definition for this architecture
#endif

#define SANE_TRACEBUF_SIZE (8 * 1024 * 1024)
#define SANE_BOOTPROFILE_TRACEBUF_SIZE (64 * 1024 * 1024)

/* Initialize the mutex governing access to the stack snapshot subsystem */
__private_extern__ void
stackshot_lock_init( void )
{
	stackshot_subsys_lck_grp_attr = lck_grp_attr_alloc_init();

	stackshot_subsys_lck_grp = lck_grp_alloc_init("stackshot_subsys_lock", stackshot_subsys_lck_grp_attr);

	stackshot_subsys_lck_attr = lck_attr_alloc_init();

	lck_mtx_init(&stackshot_subsys_mutex, stackshot_subsys_lck_grp, stackshot_subsys_lck_attr);
}

/*
 * stack_snapshot:   Obtains a coherent set of stack traces for all threads
 *		     on the system, tracing both kernel and user stacks
 *		     where available. Uses machine specific trace routines
 *		     for ppc, ppc64 and x86.
 * Inputs:	     uap->pid - process id of process to be traced, or -1
 *		     for the entire system
 *		     uap->tracebuf - address of the user space destination
 *		     buffer 
 *		     uap->tracebuf_size - size of the user space trace buffer
 *		     uap->options - various options, including the maximum
 *		     number of frames to trace.
 * Outputs:	     EPERM if the caller is not privileged
 *		     EINVAL if the supplied trace buffer isn't sanely sized
 *		     ENOMEM if we don't have enough memory to satisfy the
 *		     request
 *		     ENOENT if the target pid isn't found
 *		     ENOSPC if the supplied buffer is insufficient
 *		     *retval contains the number of bytes traced, if successful
 *		     and -1 otherwise. If the request failed due to
 *		     tracebuffer exhaustion, we copyout as much as possible.
 */
int
stack_snapshot(struct proc *p, register struct stack_snapshot_args *uap, int32_t *retval) {
	int error = 0;

	if ((error = suser(kauth_cred_get(), &p->p_acflag)))
                return(error);

	return stack_snapshot2(uap->pid, uap->tracebuf, uap->tracebuf_size,
	    uap->flags, uap->dispatch_offset, retval);
}

int  
stack_snapshot_from_kernel(pid_t pid, void *buf, uint32_t size, uint32_t flags, unsigned *bytesTraced)
{
	int error = 0;
	boolean_t istate;

	if ((buf == NULL) || (size <= 0) || (bytesTraced == NULL)) {
		return -1;
	}

	/* cap in individual stackshot to SANE_TRACEBUF_SIZE */
	if (size > SANE_TRACEBUF_SIZE) {
		size = SANE_TRACEBUF_SIZE;
	}

/* Serialize tracing */	
	STACKSHOT_SUBSYS_LOCK();
	istate = ml_set_interrupts_enabled(FALSE);


/* Preload trace parameters*/	
	kdp_snapshot_preflight(pid, buf, size, flags, 0);

/* Trap to the debugger to obtain a coherent stack snapshot; this populates
 * the trace buffer
 */
	TRAP_DEBUGGER;

	ml_set_interrupts_enabled(istate);

	*bytesTraced = kdp_stack_snapshot_bytes_traced();

	error = kdp_stack_snapshot_geterror();
	
	STACKSHOT_SUBSYS_UNLOCK();

    return error;

}

int
stack_snapshot2(pid_t pid, user_addr_t tracebuf, uint32_t tracebuf_size, uint32_t flags, uint32_t dispatch_offset, int32_t *retval)
{
	boolean_t istate;
	int error = 0;
	unsigned bytesTraced = 0;

#if CONFIG_TELEMETRY
	if (flags & STACKSHOT_GLOBAL_MICROSTACKSHOT_ENABLE) {
		telemetry_global_ctl(1);
		*retval = 0;
		return (0);
	} else if (flags & STACKSHOT_GLOBAL_MICROSTACKSHOT_DISABLE) {
		telemetry_global_ctl(0);
		*retval = 0;
		return (0);
	}

	if (flags & STACKSHOT_WINDOWED_MICROSTACKSHOTS_ENABLE) {
		error = telemetry_enable_window();

		if (error != KERN_SUCCESS) {
			/* We are probably out of memory */
			*retval = -1;
			return ENOMEM;
		}

		*retval = 0;
		return (0);
	} else if (flags & STACKSHOT_WINDOWED_MICROSTACKSHOTS_DISABLE) {
		telemetry_disable_window();
		*retval = 0;
		return (0);
	}
#endif

	*retval = -1;
/* Serialize tracing */	
	STACKSHOT_SUBSYS_LOCK();
	
	if (tracebuf_size <= 0) {
		error = EINVAL;
		goto error_exit;
	}

#if CONFIG_TELEMETRY
	if (flags & STACKSHOT_GET_MICROSTACKSHOT) {

		if (tracebuf_size > SANE_TRACEBUF_SIZE) {
			error = EINVAL;
			goto error_exit;
		}

		bytesTraced = tracebuf_size;
		error = telemetry_gather(tracebuf, &bytesTraced, 
		                         (flags & STACKSHOT_SET_MICROSTACKSHOT_MARK) ? TRUE : FALSE);
		if (error == KERN_NO_SPACE) {
			error = ENOSPC;
		}

		*retval = (int)bytesTraced;
		goto error_exit;
	}

	if (flags & STACKSHOT_GET_WINDOWED_MICROSTACKSHOTS) {

		if (tracebuf_size > SANE_TRACEBUF_SIZE) {
			error = EINVAL;
			goto error_exit;
		}

		bytesTraced = tracebuf_size;
		error = telemetry_gather_windowed(tracebuf, &bytesTraced);
		if (error == KERN_NO_SPACE) {
			error = ENOSPC;
		}

		*retval = (int)bytesTraced;
		goto error_exit;
	}

	if (flags & STACKSHOT_GET_BOOT_PROFILE) {

		if (tracebuf_size > SANE_BOOTPROFILE_TRACEBUF_SIZE) {
			error = EINVAL;
			goto error_exit;
		}

		bytesTraced = tracebuf_size;
		error = bootprofile_gather(tracebuf, &bytesTraced);
		if (error == KERN_NO_SPACE) {
			error = ENOSPC;
		}

		*retval = (int)bytesTraced;
		goto error_exit;
	}
#endif

	if (tracebuf_size > SANE_TRACEBUF_SIZE) {
		error = EINVAL;
		goto error_exit;
	}

	assert(stackshot_snapbuf == NULL);
	if (kmem_alloc_kobject(kernel_map, (vm_offset_t *)&stackshot_snapbuf, tracebuf_size) != KERN_SUCCESS) {
		error = ENOMEM;
		goto error_exit;
	}

	if (panic_active()) {
		error = ENOMEM;
		goto error_exit;
	}

	istate = ml_set_interrupts_enabled(FALSE);
/* Preload trace parameters*/	
	kdp_snapshot_preflight(pid, stackshot_snapbuf, tracebuf_size, flags, dispatch_offset);

/* Trap to the debugger to obtain a coherent stack snapshot; this populates
 * the trace buffer
 */

	TRAP_DEBUGGER;

	ml_set_interrupts_enabled(istate);

	bytesTraced = kdp_stack_snapshot_bytes_traced();
			
	if (bytesTraced > 0) {
		if ((error = copyout(stackshot_snapbuf, tracebuf,
			((bytesTraced < tracebuf_size) ?
			    bytesTraced : tracebuf_size))))
			goto error_exit;
		*retval = bytesTraced;
	}
	else {
		error = ENOENT;
		goto error_exit;
	}

	error = kdp_stack_snapshot_geterror();
	if (error == -1) {
		error = ENOSPC;
		*retval = -1;
		goto error_exit;
	}

error_exit:
	if (stackshot_snapbuf != NULL)
		kmem_free(kernel_map, (vm_offset_t) stackshot_snapbuf, tracebuf_size);
	stackshot_snapbuf = NULL;
	STACKSHOT_SUBSYS_UNLOCK();
	return error;
}

void
start_kern_tracing(unsigned int new_nkdbufs, boolean_t need_map)
{

	if (!new_nkdbufs)
		return;
	nkdbufs = kdbg_set_nkdbufs(new_nkdbufs);
	kdbg_lock_init();

	kernel_debug_string("start_kern_tracing");

	if (0 == kdbg_reinit(TRUE)) {

		if (need_map == TRUE) {
			uint32_t old1, old2;

			kdbg_thrmap_init();

			disable_wrap(&old1, &old2);
		}

		/* Hold off interrupts until the early traces are cut */
		boolean_t	s = ml_set_interrupts_enabled(FALSE);

		kdbg_set_tracing_enabled(TRUE, KDEBUG_ENABLE_TRACE);

		/*
		 * Transfer all very early events from the static buffer
		 * into the real buffers.
		 */
		kernel_debug_early_end();
	
		ml_set_interrupts_enabled(s);

		printf("kernel tracing started\n");
	} else {
		printf("error from kdbg_reinit,kernel tracing not started\n");
	}
}

void
start_kern_tracing_with_typefilter(unsigned int new_nkdbufs,
		                   boolean_t need_map,
		                   unsigned int typefilter)
{
	/* startup tracing */
	start_kern_tracing(new_nkdbufs, need_map);

	/* check that tracing was actually enabled */
	if (!(kdebug_enable & KDEBUG_ENABLE_TRACE))
		return;

	/* setup the typefiltering */
	if (0 == kdbg_enable_typefilter())
		setbit(type_filter_bitmap, typefilter & (CSC_MASK >> CSC_OFFSET));
}

void
kdbg_dump_trace_to_file(const char *filename)
{
	vfs_context_t	ctx;
	vnode_t		vp;
	int		error;
	size_t		number;


	if ( !(kdebug_enable & KDEBUG_ENABLE_TRACE))
		return;

        if (global_state_pid != -1) {
		if ((proc_find(global_state_pid)) != NULL) {
			/*
			 * The global pid exists, we're running
			 * due to fs_usage, latency, etc...
			 * don't cut the panic/shutdown trace file
			 * Disable tracing from this point to avoid
			 * perturbing state.
			 */
			kdebug_enable = 0;
			kd_ctrl_page.enabled = 0;
			return;
		}
	}
	KERNEL_DEBUG_CONSTANT((TRACEDBG_CODE(DBG_TRACE_INFO, 0)) | DBG_FUNC_NONE, 0, 0, 0, 0, 0);

	kdebug_enable = 0;
	kd_ctrl_page.enabled = 0;

	ctx = vfs_context_kernel();

	if ((error = vnode_open(filename, (O_CREAT | FWRITE | O_NOFOLLOW), 0600, 0, &vp, ctx)))
		return;

	number = kd_mapcount * sizeof(kd_threadmap);
	kdbg_readthrmap(0, &number, vp, ctx);

	number = nkdbufs*sizeof(kd_buf);
	kdbg_read(0, &number, vp, ctx);
	
	vnode_close(vp, FWRITE, ctx);

	sync(current_proc(), (void *)NULL, (int *)NULL);
}

/* Helper function for filling in the BSD name for an address space
 * Defined here because the machine bindings know only Mach threads
 * and nothing about BSD processes.
 *
 * FIXME: need to grab a lock during this?
 */
void kdbg_get_task_name(char* name_buf, int len, task_t task)
{
	proc_t proc;
	
	/* Note: we can't use thread->task (and functions that rely on it) here 
	 * because it hasn't been initialized yet when this function is called.
	 * We use the explicitly-passed task parameter instead.
	 */
	proc = get_bsdtask_info(task);
	if (proc != PROC_NULL)
		snprintf(name_buf, len, "%s/%d", proc->p_comm, proc->p_pid);
	else
		snprintf(name_buf, len, "%p [!bsd]", task);
}

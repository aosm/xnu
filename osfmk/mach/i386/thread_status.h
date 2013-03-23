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
/*
 * @OSF_COPYRIGHT@
 */
/* 
 * Mach Operating System
 * Copyright (c) 1991,1990,1989 Carnegie Mellon University
 * All Rights Reserved.
 * 
 * Permission to use, copy, modify and distribute this software and its
 * documentation is hereby granted, provided that both the copyright
 * notice and this permission notice appear in all copies of the
 * software, derivative works or modified versions, and any portions
 * thereof, and that both notices appear in supporting documentation.
 * 
 * CARNEGIE MELLON ALLOWS FREE USE OF THIS SOFTWARE IN ITS "AS IS"
 * CONDITION.  CARNEGIE MELLON DISCLAIMS ANY LIABILITY OF ANY KIND FOR
 * ANY DAMAGES WHATSOEVER RESULTING FROM THE USE OF THIS SOFTWARE.
 * 
 * Carnegie Mellon requests users of this software to return to
 * 
 *  Software Distribution Coordinator  or  Software.Distribution@CS.CMU.EDU
 *  School of Computer Science
 *  Carnegie Mellon University
 *  Pittsburgh PA 15213-3890
 * 
 * any improvements or extensions that they make and grant Carnegie Mellon
 * the rights to redistribute these changes.
 */
/*
 */
/*
 *	File:	thread_status.h
 *	Author:	Avadis Tevanian, Jr.
 *	Date:	1985
 *
 *	This file contains the structure definitions for the thread
 *	state as applied to I386 processors.
 */

#ifndef	_MACH_I386_THREAD_STATUS_H_
#define _MACH_I386_THREAD_STATUS_H_

#include <mach/message.h>
#include <mach/i386/fp_reg.h>
#include <mach/i386/thread_state.h>
#include <architecture/i386/frame.h>	/* FIXME */
#include <architecture/i386/fpu.h>	/* FIXME */
/*
 *	i386_thread_state	this is the structure that is exported
 *				to user threads for use in status/mutate
 *				calls.  This structure should never
 *				change.
 *
 *	i386_float_state	exported to use threads for access to 
 *				floating point registers. Try not to 
 *				change this one, either.
 *
 *	i386_isa_port_map_state	exported to user threads to allow
 *				selective in/out operations
 *
 * 	i386_v86_assist_state 
 *
 *	thread_syscall_state 
 */

/*     THREAD_STATE_FLAVOR_LIST 0 */
#define i386_NEW_THREAD_STATE	1	/* used to be i386_THREAD_STATE */
#define i386_FLOAT_STATE	2
#define i386_ISA_PORT_MAP_STATE	3
#define i386_V86_ASSIST_STATE	4
#define i386_REGS_SEGS_STATE	5
#define THREAD_SYSCALL_STATE	6
#define THREAD_STATE_NONE	7
#define i386_SAVED_STATE	8


/*
 * x86-64 compatibility
 * THREAD_STATE_FLAVOR_LIST 0
 * 	these are the supported flavors
 */
#define x86_THREAD_STATE32		1
#define x86_FLOAT_STATE32		2
#define x86_EXCEPTION_STATE32		3
#define x86_THREAD_STATE64		4
#define x86_FLOAT_STATE64		5
#define x86_EXCEPTION_STATE64		6
#define x86_THREAD_STATE		7
#define x86_FLOAT_STATE			8
#define x86_EXCEPTION_STATE		9
#define x86_DEBUG_STATE32		10
#define x86_DEBUG_STATE64		11
#define x86_DEBUG_STATE			12



/*
 * VALID_THREAD_STATE_FLAVOR is a platform specific macro that when passed
 * an exception flavor will return if that is a defined flavor for that
 * platform. The macro must be manually updated to include all of the valid
 * exception flavors as defined above.
 */
#define VALID_THREAD_STATE_FLAVOR(x)            \
        ((x == i386_NEW_THREAD_STATE)        || \
	 (x == i386_FLOAT_STATE)             || \
	 (x == i386_ISA_PORT_MAP_STATE)      || \
	 (x == i386_V86_ASSIST_STATE)        || \
	 (x == i386_REGS_SEGS_STATE)         || \
	 (x == THREAD_SYSCALL_STATE)         || \
	 (x == THREAD_STATE_NONE)            || \
	 (x == i386_SAVED_STATE))


/*
 * x86-64 compatibility
 */
struct x86_state_hdr {
    int 	flavor;
    int		count;
};
typedef struct x86_state_hdr x86_state_hdr_t;


/*
 * This structure is used for both
 * i386_THREAD_STATE and i386_REGS_SEGS_STATE.
 */
struct i386_new_thread_state {
	unsigned int	gs;
	unsigned int	fs;
	unsigned int	es;
	unsigned int	ds;
	unsigned int	edi;
	unsigned int	esi;
	unsigned int	ebp;
	unsigned int	esp;
	unsigned int	ebx;
	unsigned int	edx;
	unsigned int	ecx;
	unsigned int	eax;
	unsigned int	eip;
	unsigned int	cs;
	unsigned int	efl;
	unsigned int	uesp;
	unsigned int	ss;
};
#define i386_NEW_THREAD_STATE_COUNT	((mach_msg_type_number_t) \
		(sizeof (struct i386_new_thread_state)/sizeof(unsigned int)))

/*
 * Subset of saved state stored by processor on kernel-to-kernel
 * trap.  (Used by ddb to examine state guaranteed to be present
 * on all traps into debugger.)
 */
struct i386_saved_state_from_kernel {
	unsigned int	gs;
	unsigned int	fs;
	unsigned int	es;
	unsigned int	ds;
	unsigned int	edi;
	unsigned int	esi;
	unsigned int	ebp;
	unsigned int	esp;		/* kernel esp stored by pusha -
					   we save cr2 here later */
	unsigned int	ebx;
	unsigned int	edx;
	unsigned int	ecx;
	unsigned int	eax;
	unsigned int	trapno;
	unsigned int	err;
	unsigned int	eip;
	unsigned int	cs;
	unsigned int	efl;
};

/*
 * The format in which thread state is saved by Mach on this machine.  This
 * state flavor is most efficient for exception RPC's to kernel-loaded
 * servers, because copying can be avoided:
 */
struct i386_saved_state {
	unsigned int	gs;
	unsigned int	fs;
	unsigned int	es;
	unsigned int	ds;
	unsigned int	edi;
	unsigned int	esi;
	unsigned int	ebp;
	unsigned int	esp;		/* kernel esp stored by pusha -
					   we save cr2 here later */
	unsigned int	ebx;
	unsigned int	edx;
	unsigned int	ecx;
	unsigned int	eax;
	unsigned int	trapno;
	unsigned int	err;
	unsigned int	eip;
	unsigned int	cs;
	unsigned int	efl;
	unsigned int	uesp;
	unsigned int	ss;
	struct v86_segs {
	    unsigned int v86_es;	/* virtual 8086 segment registers */
	    unsigned int v86_ds;
	    unsigned int v86_fs;
	    unsigned int v86_gs;
	} v86_segs;
#define i386_SAVED_ARGV_COUNT	7
	unsigned int	argv_status;	/* Boolean flag indicating whether or
					 * not Mach copied in the args */
	unsigned int	argv[i386_SAVED_ARGV_COUNT];
					/* The return address, and the first several
					 * function call args from the stack, for
					 * efficient syscall exceptions */
};
#define i386_SAVED_STATE_COUNT	((mach_msg_type_number_t) \
	(sizeof (struct i386_saved_state)/sizeof(unsigned int)))
#define i386_REGS_SEGS_STATE_COUNT	i386_SAVED_STATE_COUNT

/*
 * Machine-independent way for servers and Mach's exception mechanism to
 * choose the most efficient state flavor for exception RPC's:
 */
#define MACHINE_THREAD_STATE		i386_SAVED_STATE
#define MACHINE_THREAD_STATE_COUNT	144

/*
 * Largest state on this machine:
 * (be sure mach/machine/thread_state.h matches!)
 */
#define THREAD_MACHINE_STATE_MAX	THREAD_STATE_MAX

/* 
 * Floating point state.
 *
 * fpkind tells in what way floating point operations are supported.  
 * See the values for fp_kind in <mach/i386/fp_reg.h>.
 * 
 * If the kind is FP_NO, then calls to set the state will fail, and 
 * thread_getstatus will return garbage for the rest of the state.
 * If "initialized" is false, then the rest of the state is garbage.  
 * Clients can set "initialized" to false to force the coprocessor to 
 * be reset.
 * "exc_status" is non-zero if the thread has noticed (but not 
 * proceeded from) a coprocessor exception.  It contains the status 
 * word with the exception bits set.  The status word in "fp_status" 
 * will have the exception bits turned off.  If an exception bit in 
 * "fp_status" is turned on, then "exc_status" should be zero.  This 
 * happens when the coprocessor exception is noticed after the system 
 * has context switched to some other thread.
 * 
 * If kind is FP_387, then "state" is a i387_state.  Other kinds might
 * also use i387_state, but somebody will have to verify it (XXX).
 * Note that the registers are ordered from top-of-stack down, not
 * according to physical register number.
 */

/*
 * x86-64 compatibility
 */
/* defn of 80bit x87 FPU or MMX register  */
struct mmst_reg {
	char	mmst_reg[10];
	char	mmst_rsrv[6];
};


/* defn of 128 bit XMM regs */
struct xmm_reg {
	char		xmm_reg[16];
};

#define FP_STATE_BYTES 512

struct i386_float_state {
	int		fpkind;			/* FP_NO..FP_387 (readonly) */
	int		initialized;
	unsigned char	hw_state[FP_STATE_BYTES]; /* actual "hardware" state */
	int		exc_status;		/* exception status (readonly) */
};
#define i386_FLOAT_STATE_COUNT ((mach_msg_type_number_t) \
		(sizeof(struct i386_float_state)/sizeof(unsigned int)))

/*
 * x86-64 compatibility
 */
typedef struct {
	int 			fpu_reserved[2];
	fp_control_t		fpu_fcw;			/* x87 FPU control word */
	fp_status_t		fpu_fsw;			/* x87 FPU status word */
	uint8_t			fpu_ftw;			/* x87 FPU tag word */
	uint8_t			fpu_rsrv1;			/* reserved */ 
	uint16_t		fpu_fop;			/* x87 FPU Opcode */
	uint32_t		fpu_ip;				/* x87 FPU Instruction Pointer offset */
	uint16_t		fpu_cs;				/* x87 FPU Instruction Pointer Selector */
	uint16_t		fpu_rsrv2;			/* reserved */
	uint32_t		fpu_dp;				/* x87 FPU Instruction Operand(Data) Pointer offset */
	uint16_t		fpu_ds;				/* x87 FPU Instruction Operand(Data) Pointer Selector */
	uint16_t		fpu_rsrv3;			/* reserved */
	uint32_t		fpu_mxcsr;			/* MXCSR Register state */
	uint32_t		fpu_mxcsrmask;		/* MXCSR mask */
	struct mmst_reg	fpu_stmm0;		/* ST0/MM0   */
	struct mmst_reg	fpu_stmm1;		/* ST1/MM1  */
	struct mmst_reg	fpu_stmm2;		/* ST2/MM2  */
	struct mmst_reg	fpu_stmm3;		/* ST3/MM3  */
	struct mmst_reg	fpu_stmm4;		/* ST4/MM4  */
	struct mmst_reg	fpu_stmm5;		/* ST5/MM5  */
	struct mmst_reg	fpu_stmm6;		/* ST6/MM6  */
	struct mmst_reg	fpu_stmm7;		/* ST7/MM7  */
	struct xmm_reg	fpu_xmm0;		/* XMM 0  */
	struct xmm_reg	fpu_xmm1;		/* XMM 1  */
	struct xmm_reg	fpu_xmm2;		/* XMM 2  */
	struct xmm_reg	fpu_xmm3;		/* XMM 3  */
	struct xmm_reg	fpu_xmm4;		/* XMM 4  */
	struct xmm_reg	fpu_xmm5;		/* XMM 5  */
	struct xmm_reg	fpu_xmm6;		/* XMM 6  */
	struct xmm_reg	fpu_xmm7;		/* XMM 7  */
	char			fpu_rsrv4[14*16];	/* reserved */
	int 			fpu_reserved1;
} i386_float_state_t;


#define FP_old_STATE_BYTES ((mach_msg_type_number_t) \
	(sizeof (struct i386_fp_save) + sizeof (struct i386_fp_regs)))

struct i386_old_float_state {
	int		fpkind;			/* FP_NO..FP_387 (readonly) */
	int		initialized;
	unsigned char	hw_state[FP_old_STATE_BYTES]; /* actual "hardware" state */
	int		exc_status;		/* exception status (readonly) */
};
#define i386_old_FLOAT_STATE_COUNT ((mach_msg_type_number_t) \
		(sizeof(struct i386_old_float_state)/sizeof(unsigned int)))


#define PORT_MAP_BITS 0x400
struct i386_isa_port_map_state {
	unsigned char	pm[PORT_MAP_BITS>>3];
};

#define i386_ISA_PORT_MAP_STATE_COUNT ((mach_msg_type_number_t) \
		(sizeof(struct i386_isa_port_map_state)/sizeof(unsigned int)))

/*
 * V8086 assist supplies a pointer to an interrupt
 * descriptor table in task space.
 */
struct i386_v86_assist_state {
	unsigned int	int_table;	/* interrupt table address */
	int		int_count;	/* interrupt table size */
};

struct v86_interrupt_table {
	unsigned int	count;	/* count of pending interrupts */
	unsigned short	mask;	/* ignore this interrupt if true */
	unsigned short	vec;	/* vector to take */
};

#define	i386_V86_ASSIST_STATE_COUNT ((mach_msg_type_number_t) \
	    (sizeof(struct i386_v86_assist_state)/sizeof(unsigned int)))

struct thread_syscall_state {
	unsigned eax;
	unsigned edx;
	unsigned efl;
	unsigned eip;
	unsigned esp;
};

#define i386_THREAD_SYSCALL_STATE_COUNT ((mach_msg_type_number_t) \
		(sizeof(struct thread_syscall_state) / sizeof(unsigned int)))

/*
 * Main thread state consists of
 * general registers, segment registers,
 * eip and eflags.
 */

#define i386_THREAD_STATE	-1

typedef struct {
    unsigned int	eax;
    unsigned int	ebx;
    unsigned int	ecx;
    unsigned int	edx;
    unsigned int	edi;
    unsigned int	esi;
    unsigned int	ebp;
    unsigned int	esp;
    unsigned int	ss;
    unsigned int	eflags;
    unsigned int	eip;
    unsigned int	cs;
    unsigned int	ds;
    unsigned int	es;
    unsigned int	fs;
    unsigned int	gs;
} i386_thread_state_t;

#define i386_THREAD_STATE_COUNT	((mach_msg_type_number_t) \
    ( sizeof (i386_thread_state_t) / sizeof (int) ))

/*
 * x86-64 compatibility
 */
typedef i386_thread_state_t x86_thread_state32_t;
#define x86_THREAD_STATE32_COUNT ((mach_msg_type_number_t) \
    ( sizeof (x86_thread_state32_t) / sizeof (int) ))




struct x86_thread_state64 {
    uint64_t		rax;
    uint64_t		rbx;
    uint64_t		rcx;
    uint64_t		rdx;
    uint64_t		rdi;
    uint64_t		rsi;
    uint64_t		rbp;
    uint64_t		rsp;
    uint64_t		r8;
    uint64_t		r9;
    uint64_t		r10;
    uint64_t		r11;
    uint64_t		r12;
    uint64_t		r13;
    uint64_t		r14;
    uint64_t		r15;
    uint64_t		rip;
    uint64_t		rflags;
    uint64_t		cs;
    uint64_t		fs;
    uint64_t		gs;
} ;


typedef struct x86_thread_state64 x86_thread_state64_t;
#define x86_THREAD_STATE64_COUNT	((mach_msg_type_number_t) \
    ( sizeof (x86_thread_state64_t) / sizeof (int) ))




struct x86_thread_state {
    x86_state_hdr_t		tsh;
    union {
        x86_thread_state32_t	ts32;
        x86_thread_state64_t	ts64;
    } uts;
} ;


typedef struct x86_thread_state x86_thread_state_t;
#define x86_THREAD_STATE_COUNT	((mach_msg_type_number_t) \
    ( sizeof (x86_thread_state_t) / sizeof (int) ))



/*
 * Default segment register values.
 */
    
#define USER_CODE_SELECTOR	0x0017
#define USER_DATA_SELECTOR	0x001f
#define KERN_CODE_SELECTOR	0x0008
#define KERN_DATA_SELECTOR	0x0010

/*
 * Thread floating point state
 * includes FPU environment as
 * well as the register stack.
 */
 
#define i386_THREAD_FPSTATE	-2

typedef struct {
    fp_env_t		environ;
    fp_stack_t		stack;
} i386_thread_fpstate_t;

#define i386_THREAD_FPSTATE_COUNT ((mach_msg_type_number_t)	\
    ( sizeof (i386_thread_fpstate_t) / sizeof (int) ))


/*
 * x86-64 compatibility
 */
typedef i386_float_state_t x86_float_state32_t;
#define x86_FLOAT_STATE32_COUNT ((mach_msg_type_number_t) \
		(sizeof(x86_float_state32_t)/sizeof(unsigned int)))
	 

struct x86_float_state64 {
	int 			fpu_reserved[2];
	fp_control_t		fpu_fcw;			/* x87 FPU control word */
	fp_status_t		fpu_fsw;			/* x87 FPU status word */
	uint8_t			fpu_ftw;			/* x87 FPU tag word */
	uint8_t			fpu_rsrv1;			/* reserved */ 
	uint16_t		fpu_fop;			/* x87 FPU Opcode */
	uint32_t		fpu_ip;				/* x87 FPU Instruction Pointer offset */
	uint16_t		fpu_cs;				/* x87 FPU Instruction Pointer Selector */
	uint16_t		fpu_rsrv2;			/* reserved */
	uint32_t		fpu_dp;				/* x87 FPU Instruction Operand(Data) Pointer offset */
	uint16_t		fpu_ds;				/* x87 FPU Instruction Operand(Data) Pointer Selector */
	uint16_t		fpu_rsrv3;			/* reserved */
	uint32_t		fpu_mxcsr;			/* MXCSR Register state */
	uint32_t		fpu_mxcsrmask;		/* MXCSR mask */
	struct mmst_reg	fpu_stmm0;		/* ST0/MM0   */
	struct mmst_reg	fpu_stmm1;		/* ST1/MM1  */
	struct mmst_reg	fpu_stmm2;		/* ST2/MM2  */
	struct mmst_reg	fpu_stmm3;		/* ST3/MM3  */
	struct mmst_reg	fpu_stmm4;		/* ST4/MM4  */
	struct mmst_reg	fpu_stmm5;		/* ST5/MM5  */
	struct mmst_reg	fpu_stmm6;		/* ST6/MM6  */
	struct mmst_reg	fpu_stmm7;		/* ST7/MM7  */
	struct xmm_reg	fpu_xmm0;		/* XMM 0  */
	struct xmm_reg	fpu_xmm1;		/* XMM 1  */
	struct xmm_reg	fpu_xmm2;		/* XMM 2  */
	struct xmm_reg	fpu_xmm3;		/* XMM 3  */
	struct xmm_reg	fpu_xmm4;		/* XMM 4  */
	struct xmm_reg	fpu_xmm5;		/* XMM 5  */
	struct xmm_reg	fpu_xmm6;		/* XMM 6  */
	struct xmm_reg	fpu_xmm7;		/* XMM 7  */
	struct xmm_reg	fpu_xmm8;		/* XMM 8  */
	struct xmm_reg	fpu_xmm9;		/* XMM 9  */
	struct xmm_reg	fpu_xmm10;		/* XMM 10  */
	struct xmm_reg	fpu_xmm11;		/* XMM 11 */
	struct xmm_reg	fpu_xmm12;		/* XMM 12  */
	struct xmm_reg	fpu_xmm13;		/* XMM 13  */
	struct xmm_reg	fpu_xmm14;		/* XMM 14  */
	struct xmm_reg	fpu_xmm15;		/* XMM 15  */
	char			fpu_rsrv4[6*16];	/* reserved */
	int 			fpu_reserved1;
};

typedef struct x86_float_state64 x86_float_state64_t;
#define x86_FLOAT_STATE64_COUNT ((mach_msg_type_number_t) \
		(sizeof(x86_float_state64_t)/sizeof(unsigned int)))
		
	 


struct x86_float_state {
    x86_state_hdr_t		fsh;
    union {
        x86_float_state32_t	fs32;
        x86_float_state64_t	fs64;
    } ufs;
} ;


typedef struct x86_float_state x86_float_state_t;
#define x86_FLOAT_STATE_COUNT	((mach_msg_type_number_t) \
    ( sizeof (x86_float_state_t) / sizeof (int) ))



/*
 * Extra state that may be
 * useful to exception handlers.
 */

#define i386_THREAD_EXCEPTSTATE	-3

typedef struct {
    unsigned int	trapno;
    err_code_t		err;
} i386_thread_exceptstate_t;

#define i386_THREAD_EXCEPTSTATE_COUNT	((mach_msg_type_number_t) \
    ( sizeof (i386_thread_exceptstate_t) / sizeof (int) ))


/*
 * x86-64 compatibility
 */
struct i386_exception_state {
    unsigned int	trapno;
    unsigned int	err;
    unsigned int	faultvaddr;
};

typedef struct i386_exception_state x86_exception_state32_t;
#define x86_EXCEPTION_STATE32_COUNT	((mach_msg_type_number_t) \
    ( sizeof (x86_exception_state32_t) / sizeof (int) ))

struct x86_debug_state32 {
	unsigned int dr0;
	unsigned int dr1;
	unsigned int dr2;
	unsigned int dr3;
	unsigned int dr4;
	unsigned int dr5;
	unsigned int dr6;
	unsigned int dr7;
};

typedef struct x86_debug_state32 x86_debug_state32_t;
#define x86_DEBUG_STATE32_COUNT       ((mach_msg_type_number_t) \
	( sizeof (x86_debug_state32_t) / sizeof (int) ))
#define X86_DEBUG_STATE32_COUNT x86_DEBUG_STATE32_COUNT


struct x86_exception_state64 {
    unsigned int	trapno;
    unsigned int	err;
    uint64_t		faultvaddr;
};

typedef struct x86_exception_state64 x86_exception_state64_t;
#define x86_EXCEPTION_STATE64_COUNT	((mach_msg_type_number_t) \
    ( sizeof (x86_exception_state64_t) / sizeof (int) ))


struct x86_debug_state64 {
	uint64_t dr0;
	uint64_t dr1;
	uint64_t dr2;
	uint64_t dr3;
	uint64_t dr4;
	uint64_t dr5;
	uint64_t dr6;
	uint64_t dr7;
};


typedef struct x86_debug_state64 x86_debug_state64_t;
#define x86_DEBUG_STATE64_COUNT	((mach_msg_type_number_t) \
    ( sizeof (x86_debug_state64_t) / sizeof (int) ))

#define X86_DEBUG_STATE64_COUNT x86_DEBUG_STATE64_COUNT



struct x86_exception_state {
    x86_state_hdr_t		esh;
    union {
        x86_exception_state32_t	es32;
        x86_exception_state64_t	es64;
    } ues;
} ;


typedef struct x86_exception_state x86_exception_state_t;
#define x86_EXCEPTION_STATE_COUNT	((mach_msg_type_number_t) \
    ( sizeof (x86_exception_state_t) / sizeof (int) ))

struct x86_debug_state {
	x86_state_hdr_t			dsh;
	union {
		x86_debug_state32_t	ds32;
		x86_debug_state64_t	ds64;
	} uds;
};



typedef struct x86_debug_state x86_debug_state_t;
#define x86_DEBUG_STATE_COUNT ((mach_msg_type_number_t) \
		(sizeof(x86_debug_state_t)/sizeof(unsigned int)))


/*
 * Per-thread variable used
 * to store 'self' id for cthreads.
 */
 
#define i386_THREAD_CTHREADSTATE	-4
 
typedef struct {
    unsigned int	self;
} i386_thread_cthreadstate_t;

#define i386_THREAD_CTHREADSTATE_COUNT	((mach_msg_type_number_t) \
    ( sizeof (i386_thread_cthreadstate_t) / sizeof (int) ))

#endif	/* _MACH_I386_THREAD_STATUS_H_ */

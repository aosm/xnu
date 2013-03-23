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
#include <ppc/asm.h>
#include <ppc/proc_reg.h>
#include <cpus.h>
#include <assym.s>
#include <debug.h>
#include <mach/ppc/vm_param.h>
#include <ppc/exception.h>
	
    
/*
 * ml_set_physical()		 	-- turn off DR and (if 64-bit) turn SF on
 *								   it is assumed that pf64Bit is already in cr6
 * ml_set_physical_get_ffs() 	-- turn DR off, SF on, and get feature flags 
 * ml_set_physical_disabled()	-- turn DR and EE off, SF on, get feature flags
 * ml_set_translation_off()		-- turn DR, IR, and EE off, SF on, get feature flags
 *
 * Callable only from assembler, these return:
 *	 r2 -- new MSR
 *	r11 -- old MSR
 *	r10 -- feature flags (pf64Bit etc, ie SPRG 2)
 *	cr6 -- feature flags 24-27, ie pf64Bit, pf128Byte, and pf32Byte
 *
 * Uses r0 and r2.  ml_set_translation_off also uses r3 and cr5.
 */

        .align	4
        .globl	EXT(ml_set_translation_off)
LEXT(ml_set_translation_off)
        mfsprg	r10,2						// get feature flags
       	li		r0,0						; Clear this
        mtcrf	0x02,r10					// move pf64Bit etc to cr6
        ori		r0,r0,lo16(MASK(MSR_EE)+MASK(MSR_FP)+MASK(MSR_IR)+MASK(MSR_DR)) // turn off all 4
        mfmsr	r11							// get MSR
		oris	r0,r0,hi16(MASK(MSR_VEC))	// Turn off vector too
        mtcrf	0x04,r10					// move pfNoMSRir etc to cr5
        andc	r2,r11,r0					// turn off EE, IR, and DR
        bt++	pf64Bitb,ml_set_physical_64	// skip if 64-bit (only they take the hint)
        bf		pfNoMSRirb,ml_set_physical_32	// skip if we can load MSR directly
        li		r0,loadMSR					// Get the MSR setter SC
        mr		r3,r2						// copy new MSR to r2
        sc									// Set it
        blr
        
		.align	4
		.globl	EXT(ml_set_physical_disabled)

LEXT(ml_set_physical_disabled)
		li		r0,0						; Clear
        mfsprg	r10,2						// get feature flags
        ori		r0,r0,lo16(MASK(MSR_EE))	// turn EE and fp off
        mtcrf	0x02,r10					// move pf64Bit etc to cr6
        b		ml_set_physical_join

		.align	5
		.globl	EXT(ml_set_physical_get_ffs)

LEXT(ml_set_physical_get_ffs)
        mfsprg	r10,2						// get feature flags
        mtcrf	0x02,r10					// move pf64Bit etc to cr6

		.globl	EXT(ml_set_physical)
LEXT(ml_set_physical)

        li		r0,0						// do not turn off interrupts

ml_set_physical_join:
		oris	r0,r0,hi16(MASK(MSR_VEC))	// Always gonna turn of vectors
        mfmsr	r11							// get MSR
        ori		r0,r0,lo16(MASK(MSR_DR)+MASK(MSR_FP))	// always turn off DR and FP bit
        andc	r2,r11,r0					// turn off DR and maybe EE
        bt++	pf64Bitb,ml_set_physical_64	// skip if 64-bit (only they take the hint)
ml_set_physical_32:
        mtmsr	r2							// turn off translation
        isync
        blr
        
ml_set_physical_64:
        li		r0,1						// get a 1 to slam into SF
        rldimi	r2,r0,63,MSR_SF_BIT			// set SF bit (bit 0)
        mtmsrd	r2							// set 64-bit mode, turn off data relocation
        isync								// synchronize
        blr
    

/*
 * ml_restore(old_MSR)
 *
 * Callable only from assembler, restores the MSR in r11 saved by ml_set_physical.
 * We assume cr6 and r11 are as set by ml_set_physical, ie:
 *	cr6 - pf64Bit flag (feature flags 24-27)
 *	r11 - old MSR
 */
 
		.align	5
		.globl	EXT(ml_restore)

LEXT(ml_restore)
        bt++	pf64Bitb,ml_restore_64		// handle 64-bit cpus (only they take the hint)
        mtmsr	r11							// restore a 32-bit MSR
        isync
        blr
        
ml_restore_64:
        mtmsrd	r11							// restore a 64-bit MSR
        isync
        blr

    
/* PCI config cycle probing
 *
 *	boolean_t ml_probe_read(vm_offset_t paddr, unsigned int *val)
 *
 *	Read the memory location at physical address paddr.
 *  This is a part of a device probe, so there is a good chance we will
 *  have a machine check here. So we have to be able to handle that.
 *  We assume that machine checks are enabled both in MSR and HIDs
 */

;			Force a line boundry here
			.align	5
			.globl	EXT(ml_probe_read)

LEXT(ml_probe_read)

			mfsprg	r9,2							; Get feature flags
			
			rlwinm.	r0,r9,0,pf64Bitb,pf64Bitb		; Are we on a 64-bit machine?
			rlwinm	r3,r3,0,0,31					; Clean up for 64-bit machines
			bne++	mpr64bit						; Go do this the 64-bit way...

mpr32bit:	lis		r8,hi16(MASK(MSR_VEC))			; Get the vector flag
			mfmsr	r0								; Save the current MSR
			ori		r8,r8,lo16(MASK(MSR_FP))		; Add the FP flag

			neg		r10,r3							; Number of bytes to end of page
			andc	r0,r0,r8						; Clear VEC and FP
			rlwinm.	r10,r10,0,20,31					; Clear excess junk and test for page bndry
			ori		r8,r8,lo16(MASK(MSR_EE)|MASK(MSR_IR)|MASK(MSR_DR))		; Drop EE, IR, and DR
			mr		r12,r3							; Save the load address
			andc	r2,r0,r8						; Clear VEC, FP, and EE
			mtcrf	0x04,r9							; Set the features			
			cmplwi	cr1,r10,4						; At least 4 bytes left in page?
			beq-	mprdoit							; We are right on the boundary...
			li		r3,0
			bltlr-	cr1								; No, just return failure...

mprdoit:

			bt		pfNoMSRirb,mprNoMSR				; No MSR...

			mtmsr	r2								; Translation and all off
			isync									; Toss prefetch
			b		mprNoMSRx
			
mprNoMSR:	
			mr		r5,r0
			li		r0,loadMSR						; Get the MSR setter SC
			mr		r3,r2							; Get new MSR
			sc										; Set it
			mr		r0,r5
			li		r3,0
mprNoMSRx:

			mfspr		r6, hid0					; Get a copy of hid0
			
			rlwinm.		r5, r9, 0, pfNoMuMMCKb, pfNoMuMMCKb		; Check for NoMuMMCK
			bne		mprNoMuM
			
			rlwinm		r5, r6, 0, ice+1, ice-1				; Turn off L1 I-Cache
			mtspr		hid0, r5
			isync								; Wait for I-Cache off
			rlwinm		r5, r6, 0, mum+1, mum-1				; Turn off MuM w/ I-Cache on
			mtspr		hid0, r5
mprNoMuM:

;
;			We need to insure that there is no more than 1 BAT register that
;			can get a hit. There could be repercussions beyond the ken
;			of mortal man. It is best not to tempt fate.
;

;			Note: we will reload these from the shadow BATs later

			li		r10,0							; Clear a register
			
			sync									; Make sure all is well

			mtdbatu	1,r10							; Invalidate DBAT 1 
			mtdbatu	2,r10							; Invalidate DBAT 2 
			mtdbatu	3,r10							; Invalidate DBAT 3  
			
			rlwinm	r10,r12,0,0,14					; Round down to a 128k boundary
			ori		r11,r10,0x32					; Set uncached, coherent, R/W
			ori		r10,r10,2						; Make the upper half (128k, valid supervisor)
			mtdbatl	0,r11							; Set lower BAT first
			mtdbatu	0,r10							; Now the upper
			sync									; Just make sure
			
			dcbf	0,r12							; Make sure we kill the cache to avoid paradoxes
			sync
			
			ori		r11,r2,lo16(MASK(MSR_DR))		; Turn on data translation
			mtmsr	r11								; Do it for real
			isync									; Make sure of it
			
			eieio									; Make sure of all previous accesses
			sync									; Make sure it is all caught up
			
			lwz		r11,0(r12)						; Get it and maybe machine check here
			
			eieio									; Make sure of ordering again
			sync									; Get caught up yet again
			isync									; Do not go further till we are here
			
			mtmsr	r2								; Turn translation back off
			isync
			
			lis		r10,hi16(EXT(shadow_BAT)+shdDBAT)	; Get shadow address
			ori		r10,r10,lo16(EXT(shadow_BAT)+shdDBAT)	; Get shadow address
			
			lwz		r5,0(r10)						; Pick up DBAT 0 high
			lwz		r6,4(r10)						; Pick up DBAT 0 low
			lwz		r7,8(r10)						; Pick up DBAT 1 high
			lwz		r8,16(r10)						; Pick up DBAT 2 high
			lwz		r9,24(r10)						; Pick up DBAT 3 high
			
			mtdbatu	0,r5							; Restore DBAT 0 high
			mtdbatl	0,r6							; Restore DBAT 0 low
			mtdbatu	1,r7							; Restore DBAT 1 high
			mtdbatu	2,r8							; Restore DBAT 2 high
			mtdbatu	3,r9							; Restore DBAT 3 high 
			sync
			
			li		r3,1							; We made it
			
			mtmsr	r0								; Restore translation and exceptions
			isync									; Toss speculations
			
			stw		r11,0(r4)						; Save the loaded value
			blr										; Return...
			
;			Force a line boundry here. This means we will be able to check addresses better
			.align	5
			.globl	EXT(ml_probe_read_mck)
LEXT(ml_probe_read_mck)

    
/* PCI config cycle probing - 64-bit
 *
 *	boolean_t ml_probe_read_64(addr64_t paddr, unsigned int *val)
 *
 *	Read the memory location at physical address paddr.
 *  This is a part of a device probe, so there is a good chance we will
 *  have a machine check here. So we have to be able to handle that.
 *  We assume that machine checks are enabled both in MSR and HIDs
 */

;			Force a line boundry here
			.align	6
			.globl	EXT(ml_probe_read_64)

LEXT(ml_probe_read_64)

			mfsprg	r9,2							; Get feature flags
			rlwinm	r3,r3,0,1,0						; Copy low 32 bits to top 32
			rlwinm.	r0,r9,0,pf64Bitb,pf64Bitb		; Are we on a 64-bit machine?
			rlwimi	r3,r4,0,0,31					; Insert low part of 64-bit address in bottom 32 bits			
			
			mr		r4,r5							; Move result to common register
			beq--	mpr32bit						; Go do this the 32-bit way...

mpr64bit:	andi.	r0,r3,3							; Check if we are on a word boundary
			li		r0,0							; Clear the EE bit (and everything else for that matter)
			bne--	mprFail							; Boundary not good...
			mfmsr	r11								; Get the MSR
			mtmsrd	r0,1							; Set the EE bit only (do not care about RI)
			rlwinm	r11,r11,0,MSR_EE_BIT,MSR_EE_BIT	; Isolate just the EE bit
			mfmsr	r10								; Refresh our view of the MSR (VMX/FP may have changed)
			or		r12,r10,r11						; Turn on EE if on before we turned it off
			ori		r0,r0,lo16(MASK(MSR_IR)|MASK(MSR_DR))	; Get the IR and DR bits
			li		r2,1							; Get a 1
			sldi	r2,r2,63						; Get the 64-bit bit
			andc	r10,r10,r0						; Clear IR and DR
			or		r10,r10,r2						; Set 64-bit
			
			li		r0,1							; Get a 1
			mtmsrd	r10								; Translation and EE off, 64-bit on
			isync			
			
			sldi	r0,r0,32+8						; Get the right bit to inhibit caching

			mfspr	r8,hid4							; Get HID4
			or		r2,r8,r0						; Set bit to make real accesses cache-inhibited
			sync									; Sync up
			mtspr	hid4,r2							; Make real accesses cache-inhibited
			isync									; Toss prefetches
			
			lis		r7,0xE000						; Get the unlikeliest ESID possible
			srdi	r7,r7,1							; Make 0x7FFFFFFFF0000000
			slbie	r7								; Make sure the ERAT is cleared 
			
			sync
			isync

			eieio									; Make sure of all previous accesses
			
			lwz		r11,0(r3)						; Get it and maybe machine check here
			
			eieio									; Make sure of ordering again
			sync									; Get caught up yet again
			isync									; Do not go further till we are here

			sync									; Sync up
			mtspr	hid4,r8							; Make real accesses not cache-inhibited
			isync									; Toss prefetches

			lis		r7,0xE000						; Get the unlikeliest ESID possible
			srdi	r7,r7,1							; Make 0x7FFFFFFFF0000000
			slbie	r7								; Make sure the ERAT is cleared 

			mtmsrd	r12								; Restore entry MSR
			isync
			
			stw		r11,0(r4)						; Pass back the result
			li		r3,1							; Indicate success
			blr										; Leave...

mprFail:	li		r3,0							; Set failure
			blr										; Leave...

;			Force a line boundry here. This means we will be able to check addresses better
			.align	6
			.globl	EXT(ml_probe_read_mck_64)
LEXT(ml_probe_read_mck_64)


/* Read physical address byte
 *
 *	unsigned int ml_phys_read_byte(vm_offset_t paddr)
 *	unsigned int ml_phys_read_byte_64(addr64_t paddr)
 *
 *	Read the byte at physical address paddr. Memory should not be cache inhibited.
 */

;			Force a line boundry here

			.align	5
			.globl	EXT(ml_phys_read_byte_64)

LEXT(ml_phys_read_byte_64)

			rlwinm	r3,r3,0,1,0						; Copy low 32 bits to top 32
			rlwimi	r3,r4,0,0,31					; Insert low part of 64-bit address in bottom 32 bits
            b		ml_phys_read_byte_join			

			.globl	EXT(ml_phys_read_byte)

LEXT(ml_phys_read_byte)
            rlwinm   r3,r3,0,0,31    				; truncate address to 32-bits
ml_phys_read_byte_join:								; r3 = address to read (reg64_t)
			mflr	r11								; Save the return
			bl		rdwrpre							; Get set up, translation/interrupts off, 64-bit on, etc.
			
			lbz		r3,0(r3)						; Get the byte
			b		rdwrpost						; Clean up and leave...


/* Read physical address half word
 *
 *	unsigned int ml_phys_read_half(vm_offset_t paddr)
 *	unsigned int ml_phys_read_half_64(addr64_t paddr)
 *
 *	Read the half word at physical address paddr. Memory should not be cache inhibited.
 */

;			Force a line boundry here

			.align	5
			.globl	EXT(ml_phys_read_half_64)

LEXT(ml_phys_read_half_64)

			rlwinm	r3,r3,0,1,0						; Copy low 32 bits to top 32
			rlwimi	r3,r4,0,0,31					; Insert low part of 64-bit address in bottom 32 bits
            b		ml_phys_read_half_join		

			.globl	EXT(ml_phys_read_half)

LEXT(ml_phys_read_half)
            rlwinm   r3,r3,0,0,31    				; truncate address to 32-bits
ml_phys_read_half_join:								; r3 = address to read (reg64_t)
			mflr	r11								; Save the return
			bl		rdwrpre							; Get set up, translation/interrupts off, 64-bit on, etc.
			
			lhz		r3,0(r3)						; Get the half word
			b		rdwrpost						; Clean up and leave...


/* Read physical address word
 *
 *	unsigned int ml_phys_read(vm_offset_t paddr)
 *	unsigned int ml_phys_read_64(addr64_t paddr)
 *	unsigned int ml_phys_read_word(vm_offset_t paddr)
 *	unsigned int ml_phys_read_word_64(addr64_t paddr)
 *
 *	Read the word at physical address paddr. Memory should not be cache inhibited.
 */

;			Force a line boundry here

			.align	5
			.globl	EXT(ml_phys_read_64)
			.globl	EXT(ml_phys_read_word_64)

LEXT(ml_phys_read_64)
LEXT(ml_phys_read_word_64)

			rlwinm	r3,r3,0,1,0						; Copy low 32 bits to top 32
			rlwimi	r3,r4,0,0,31					; Insert low part of 64-bit address in bottom 32 bits
            b		ml_phys_read_word_join		

			.globl	EXT(ml_phys_read)
			.globl	EXT(ml_phys_read_word)

LEXT(ml_phys_read)
LEXT(ml_phys_read_word)
            rlwinm   r3,r3,0,0,31    				; truncate address to 32-bits
ml_phys_read_word_join:								; r3 = address to read (reg64_t)
			mflr	r11								; Save the return
			bl		rdwrpre							; Get set up, translation/interrupts off, 64-bit on, etc.
			
			lwz		r3,0(r3)						; Get the word
			b		rdwrpost						; Clean up and leave...


/* Read physical address double word
 *
 *	unsigned long long ml_phys_read_double(vm_offset_t paddr)
 *	unsigned long long ml_phys_read_double_64(addr64_t paddr)
 *
 *	Read the double word at physical address paddr. Memory should not be cache inhibited.
 */

;			Force a line boundry here

			.align	5
			.globl	EXT(ml_phys_read_double_64)

LEXT(ml_phys_read_double_64)

			rlwinm	r3,r3,0,1,0						; Copy low 32 bits to top 32
			rlwimi	r3,r4,0,0,31					; Insert low part of 64-bit address in bottom 32 bits			
            b		ml_phys_read_double_join		

			.globl	EXT(ml_phys_read_double)

LEXT(ml_phys_read_double)
            rlwinm   r3,r3,0,0,31    				; truncate address to 32-bits
ml_phys_read_double_join:							; r3 = address to read (reg64_t)
			mflr	r11								; Save the return
			bl		rdwrpre							; Get set up, translation/interrupts off, 64-bit on, etc.
			
			lwz		r4,4(r3)						; Get the low word
			lwz		r3,0(r3)						; Get the high word
			b		rdwrpost						; Clean up and leave...


/* Write physical address byte
 *
 *	void ml_phys_write_byte(vm_offset_t paddr, unsigned int data)
 *	void ml_phys_write_byte_64(addr64_t paddr, unsigned int data)
 *
 *	Write the byte at physical address paddr. Memory should not be cache inhibited.
 */

			.align	5
			.globl	EXT(ml_phys_write_byte_64)

LEXT(ml_phys_write_byte_64)

			rlwinm	r3,r3,0,1,0						; Copy low 32 bits to top 32
			rlwimi	r3,r4,0,0,31					; Insert low part of 64-bit address in bottom 32 bits			
			mr		r4,r5							; Copy over the data
            b		ml_phys_write_byte_join

			.globl	EXT(ml_phys_write_byte)

LEXT(ml_phys_write_byte)
            rlwinm   r3,r3,0,0,31    				; truncate address to 32-bits
ml_phys_write_byte_join:							; r3 = address to write (reg64_t), r4 = data
			mflr	r11								; Save the return
			bl		rdwrpre							; Get set up, translation/interrupts off, 64-bit on, etc.
			
			stb		r4,0(r3)						; Set the byte
			b		rdwrpost						; Clean up and leave...


/* Write physical address half word
 *
 *	void ml_phys_write_half(vm_offset_t paddr, unsigned int data)
 *	void ml_phys_write_half_64(addr64_t paddr, unsigned int data)
 *
 *	Write the half word at physical address paddr. Memory should not be cache inhibited.
 */

			.align	5
			.globl	EXT(ml_phys_write_half_64)

LEXT(ml_phys_write_half_64)

			rlwinm	r3,r3,0,1,0						; Copy low 32 bits to top 32
			rlwimi	r3,r4,0,0,31					; Insert low part of 64-bit address in bottom 32 bits			
			mr		r4,r5							; Copy over the data
            b		ml_phys_write_half_join

			.globl	EXT(ml_phys_write_half)

LEXT(ml_phys_write_half)
            rlwinm   r3,r3,0,0,31    				; truncate address to 32-bits
ml_phys_write_half_join:							; r3 = address to write (reg64_t), r4 = data
			mflr	r11								; Save the return
			bl		rdwrpre							; Get set up, translation/interrupts off, 64-bit on, etc.
			
			sth		r4,0(r3)						; Set the half word
			b		rdwrpost						; Clean up and leave...


/* Write physical address word
 *
 *	void ml_phys_write(vm_offset_t paddr, unsigned int data)
 *	void ml_phys_write_64(addr64_t paddr, unsigned int data)
 *	void ml_phys_write_word(vm_offset_t paddr, unsigned int data)
 *	void ml_phys_write_word_64(addr64_t paddr, unsigned int data)
 *
 *	Write the word at physical address paddr. Memory should not be cache inhibited.
 */

			.align	5
			.globl	EXT(ml_phys_write_64)
			.globl	EXT(ml_phys_write_word_64)

LEXT(ml_phys_write_64)
LEXT(ml_phys_write_word_64)

			rlwinm	r3,r3,0,1,0						; Copy low 32 bits to top 32
			rlwimi	r3,r4,0,0,31					; Insert low part of 64-bit address in bottom 32 bits			
			mr		r4,r5							; Copy over the data
            b		ml_phys_write_word_join

			.globl	EXT(ml_phys_write)
			.globl	EXT(ml_phys_write_word)

LEXT(ml_phys_write)
LEXT(ml_phys_write_word)
            rlwinm   r3,r3,0,0,31    				; truncate address to 32-bits
ml_phys_write_word_join:							; r3 = address to write (reg64_t), r4 = data
			mflr	r11								; Save the return
			bl		rdwrpre							; Get set up, translation/interrupts off, 64-bit on, etc.
			
			stw		r4,0(r3)						; Set the word
			b		rdwrpost						; Clean up and leave...


/* Write physical address double word
 *
 *	void ml_phys_write_double(vm_offset_t paddr, unsigned long long data)
 *	void ml_phys_write_double_64(addr64_t paddr, unsigned long long data)
 *
 *	Write the double word at physical address paddr. Memory should not be cache inhibited.
 */

			.align	5
			.globl	EXT(ml_phys_write_double_64)

LEXT(ml_phys_write_double_64)

			rlwinm	r3,r3,0,1,0						; Copy low 32 bits to top 32
			rlwimi	r3,r4,0,0,31					; Insert low part of 64-bit address in bottom 32 bits			
			mr		r4,r5							; Copy over the high data
			mr		r5,r6							; Copy over the low data
            b		ml_phys_write_double_join

			.globl	EXT(ml_phys_write_double)

LEXT(ml_phys_write_double)
            rlwinm   r3,r3,0,0,31    				; truncate address to 32-bits
ml_phys_write_double_join:							; r3 = address to write (reg64_t), r4,r5 = data (long long)
			mflr	r11								; Save the return
			bl		rdwrpre							; Get set up, translation/interrupts off, 64-bit on, etc.
			
			stw		r4,0(r3)						; Set the high word
			stw		r5,4(r3)						; Set the low word
			b		rdwrpost						; Clean up and leave...


			.align	5

rdwrpre:	mfsprg	r12,2							; Get feature flags 
			lis		r8,hi16(MASK(MSR_VEC))			; Get the vector flag
			mfmsr	r10								; Save the MSR 
			ori		r8,r8,lo16(MASK(MSR_FP))		; Add the FP flag
			mtcrf	0x02,r12						; move pf64Bit
			andc	r10,r10,r8						; Clear VEC and FP
			ori		r9,r8,lo16(MASK(MSR_EE)|MASK(MSR_IR)|MASK(MSR_DR))		; Drop EE, DR, and IR
			li		r2,1							; Prepare for 64 bit
			andc	r9,r10,r9						; Clear VEC, FP, DR, and EE
			bf--	pf64Bitb,rdwrpre32				; Join 32-bit code...
			
			srdi	r7,r3,31						; Get a 1 if address is in I/O memory
			rldimi	r9,r2,63,MSR_SF_BIT				; set SF bit (bit 0)
			cmpldi	cr7,r7,1						; Is source in I/O memory?
			mtmsrd	r9								; set 64-bit mode, turn off EE, DR, and IR
			isync									; synchronize

			sldi	r0,r2,32+8						; Get the right bit to turn off caching
			
			bnelr++	cr7								; We are not in the I/O area, all ready...
			
			mfspr	r8,hid4							; Get HID4
			or		r2,r8,r0						; Set bit to make real accesses cache-inhibited
			sync									; Sync up
			mtspr	hid4,r2							; Make real accesses cache-inhibited
			isync									; Toss prefetches
			
			lis		r7,0xE000						; Get the unlikeliest ESID possible
			srdi	r7,r7,1							; Make 0x7FFFFFFFF0000000
			slbie	r7								; Make sure the ERAT is cleared 
			
			sync
			isync
			blr										; Finally,  all ready...
	
			.align	5
			
rdwrpre32:	rlwimi	r9,r10,0,MSR_IR_BIT,MSR_IR_BIT	; Leave the IR bit unchanged
			mtmsr	r9								; Drop EE, DR, and leave IR unchanged
			isync
			blr										; All set up, leave...
			
			.align	5
			
rdwrpost:	mtlr	r11								; Restore the return
			bt++	pf64Bitb,rdwrpost64				; Join 64-bit code...
			
			mtmsr	r10								; Restore entry MSR (sans FP and VEC)
			isync
			blr										; Leave...
			
rdwrpost64:	bne++	cr7,rdwrpcok					; Skip enabling real mode caching if we did not change it...

			sync									; Sync up
			mtspr	hid4,r8							; Make real accesses not cache-inhibited
			isync									; Toss prefetches

			lis		r7,0xE000						; Get the unlikeliest ESID possible
			srdi	r7,r7,1							; Make 0x7FFFFFFFF0000000
			slbie	r7								; Make sure the ERAT is cleared 

rdwrpcok:	mtmsrd	r10								; Restore entry MSR (sans FP and VEC)
			isync
			blr										; Leave...


/* set interrupts enabled or disabled
 *
 *	boolean_t set_interrupts_enabled(boolean_t enable)
 *
 *	Set EE bit to "enable" and return old value as boolean
 */

;			Force a line boundry here
			.align  5
			.globl  EXT(ml_set_interrupts_enabled)
 
LEXT(ml_set_interrupts_enabled)

			andi.   r4,r3,1							; Are we turning interruptions on?
			lis		r0,hi16(MASK(MSR_VEC))			; Get vector enable
			mfmsr	r5								; Get the current MSR
			ori		r0,r0,lo16(MASK(MSR_EE)|MASK(MSR_FP))	; Get float enable and EE enable
			rlwinm	r3,r5,17,31,31					; Set return value
			andc	r5,r5,r0						; Force VEC and FP off
			bne	    CheckPreemption					; Interrupts going on, check ASTs...

			mtmsr   r5                              ; Slam diable (always going disabled here)
			isync									; Need this because FP/Vec might go off
			blr

			.align	5

CheckPreemption:
			mfsprg	r7,0
			ori		r5,r5,lo16(MASK(MSR_EE))		; Turn on the enable
			lwz		r8,PP_NEED_AST(r7)				; Get pointer to AST flags
			mfsprg	r9,1							; Get current activation
			li		r6,AST_URGENT					; Get the type we will preempt for 
			lwz		r7,ACT_PREEMPT_CNT(r9)			; Get preemption count
			lwz		r8,0(r8)						; Get AST flags
			lis		r0,hi16(DoPreemptCall)			; High part of Preempt FW call
			cmpwi	cr1,r7,0						; Are preemptions masked off?
			and.	r8,r8,r6						; Are we urgent?
			crorc	cr1_eq,cr0_eq,cr1_eq			; Remember if preemptions are masked or not urgent
			ori		r0,r0,lo16(DoPreemptCall)   	; Bottome of FW call

			mtmsr	r5								; Restore the MSR now, before we can preempt
			isync									; Need this because FP/Vec might go off

			beqlr++	cr1								; Return if no premption...
			sc										; Preempt
			blr

/*  Emulate a decremeter exception
 *
 *	void machine_clock_assist(void)
 *
 */

;			Force a line boundry here
			.align  5
			.globl  EXT(machine_clock_assist)
 
LEXT(machine_clock_assist)

			mfsprg	r7,0
			lwz		r4,PP_INTS_ENABLED(r7)
			mr.		r4,r4
			bnelr+	cr0
			b	EXT(CreateFakeDEC)

/*  Set machine into idle power-saving mode. 
 *
 *	void machine_idle_ppc(void)
 *
 *	We will use the PPC NAP or DOZE for this. 
 *	This call always returns.  Must be called with spllo (i.e., interruptions
 *	enabled).
 *
 */

;			Force a line boundry here
			.align	5
			.globl	EXT(machine_idle_ppc)

LEXT(machine_idle_ppc)

			lis		r0,hi16(MASK(MSR_VEC))			; Get the vector flag
			mfmsr	r3								; Save the MSR 
			ori		r0,r0,lo16(MASK(MSR_FP))		; Add the FP flag
			andc	r3,r3,r0						; Clear VEC and FP
			ori		r0,r0,lo16(MASK(MSR_EE))		; Drop EE also
			andc	r5,r3,r0						; Clear VEC, FP, DR, and EE

			mtmsr	r5								; Hold up interruptions for now
			isync									; May have messed with fp/vec
			mfsprg	r12,0							; Get the per_proc_info
			mfsprg	r11,2							; Get CPU specific features
			mfspr	r6,hid0							; Get the current power-saving mode
			mtcrf	0xC7,r11						; Get the facility flags

			lis		r4,hi16(napm)					; Assume we can nap
			bt		pfWillNapb,yesnap				; Yeah, nap is ok...
			
			lis		r4,hi16(dozem)					; Assume we can doze
			bt		pfCanDozeb,yesnap				; We can sleep or doze one this machine...
			
			ori		r3,r3,lo16(MASK(MSR_EE))		; Flip on EE
			mtmsr	r3								; Turn interruptions back on
			blr										; Leave...

yesnap:		mftbu	r9								; Get the upper timebase
			mftb	r7								; Get the lower timebase
			mftbu	r8								; Get the upper one again
			cmplw	r9,r8							; Did the top tick?
			bne-	yesnap							; Yeah, need to get it again...
			stw		r8,napStamp(r12)				; Set high order time stamp
			stw		r7,napStamp+4(r12)				; Set low order nap stamp

			rlwinm.	r7,r11,0,pfNoL2PFNapb,pfNoL2PFNapb	; Turn off L2 Prefetch before nap?
			beq	miL2PFok

			mfspr	r7,msscr0						; Get currect MSSCR0 value
			rlwinm	r7,r7,0,0,l2pfes-1				; Disable L2 Prefetch
			mtspr	msscr0,r7						; Updates MSSCR0 value
			sync
			isync

miL2PFok:
			rlwinm.	r7,r11,0,pfSlowNapb,pfSlowNapb	; Should nap at slow speed?
			beq	minoslownap

			mfspr	r7,hid1							; Get current HID1 value
			oris	r7,r7,hi16(hid1psm)				; Select PLL1
			mtspr	hid1,r7							; Update HID1 value

minoslownap:

;
;			We have to open up interruptions here because book 4 says that we should
;			turn on only the POW bit and that we should have interrupts enabled
;			The interrupt handler will detect that nap or doze is set if an interrupt
;			is taken and set everything up to return directly to machine_idle_ret.
;			So, make sure everything we need there is already set up...
;

			lis		r10,hi16(dozem|napm|sleepm)		; Mask of power management bits
		
			bf--	pf64Bitb,mipNSF1				; skip if 32-bit...
			
			sldi	r4,r4,32						; Position the flags
			sldi	r10,r10,32						; Position the masks

		
mipNSF1:	andc	r6,r6,r10						; Clean up the old power bits		

			ori		r7,r5,lo16(MASK(MSR_EE))		; Flip on EE
			or		r6,r6,r4						; Set nap or doze
			oris	r5,r7,hi16(MASK(MSR_POW))		; Turn on power management in next MSR
			
			sync
			mtspr	hid0,r6							; Set up the HID for nap/doze
			mfspr	r6,hid0							; Yes, this is silly, keep it here
			mfspr	r6,hid0							; Yes, this is a duplicate, keep it here
			mfspr	r6,hid0							; Yes, this is a duplicate, keep it here
			mfspr	r6,hid0							; Yes, this is a duplicate, keep it here
			mfspr	r6,hid0							; Yes, this is a duplicate, keep it here
			mfspr	r6,hid0							; Yes, this is a duplicate, keep it here
			isync									; Make sure it is set
		
			mtmsr	r7								; Enable for interrupts
			rlwinm.	r11,r11,0,pfAltivecb,pfAltivecb	; Do we have altivec?
			beq-	minovec							; No...
			dssall									; Stop the streams before we nap/doze

minovec:
			bf--	pf64Bitb,mipowloop				; skip if 32-bit...
			
			li		r3,0x10							; Fancy nap threashold is 0x10 ticks
			mftb	r8								; Get the low half of the time base
			mfdec	r4								; Get the decrementer ticks
			cmplw	r4,r3							; Less than threashold?
			blt		mipowloop
			
			mtdec	r3								; Load decrimenter with threshold
			isync									; and make sure,
			mfdec	r3								; really sure, it gets there
			
			rlwinm	r6,r5,0,MSR_EE_BIT+1,MSR_EE_BIT-1	; Clear out the EE bit
			sync									; Make sure queues are clear
			mtmsr	r6								; Set MSR with EE off but POW on
			isync									; Make sure this takes before we proceed
			
			mftb	r9								; Get the low half of the time base
			sub		r9,r9,r8						; Get the number of ticks spent waiting
			sub		r4,r4,r9						; Adjust the decrementer value
			
			mtdec	r4								; Load decrimenter with the rest of the timeout
			isync									; and make sure,
			mfdec	r4								; really sure, it gets there
			
mipowloop:
			sync									; Make sure queues are clear
			mtmsr	r5								; Nap or doze, MSR with POW and EE set
			isync									; Make sure this takes before we proceed
			b		mipowloop						; loop if POW does not take

;
;			Note that the interrupt handler will turn off the nap/doze bits in the hid.
;			Also remember that the interrupt handler will force return to here whenever
;			the nap/doze bits are set.
;
			.globl	EXT(machine_idle_ret)
LEXT(machine_idle_ret)
			mtmsr	r7								; Make sure the MSR is what we want
			isync									; In case we turn on translation
			
			blr										; Return...

/*  Put machine to sleep. 
 *	This call never returns. We always exit sleep via a soft reset.
 *	All external interruptions must be drained at this point and disabled.
 *
 *	void ml_ppc_sleep(void)
 *
 *	We will use the PPC SLEEP for this. 
 *
 *	There is one bit of hackery in here: we need to enable for
 *	interruptions when we go to sleep and there may be a pending
 *	decrimenter rupt.  So we make the decrimenter 0x7FFFFFFF and enable for
 *	interruptions. The decrimenter rupt vector recognizes this and returns
 *	directly back here.
 *
 */

;			Force a line boundry here
			.align	5
			.globl	EXT(ml_ppc_sleep)

LEXT(ml_ppc_sleep)

#if 0
			mfmsr	r5								; Hack to spin instead of sleep 
			rlwinm	r5,r5,0,MSR_DR_BIT+1,MSR_IR_BIT-1	; Turn off translation	
			rlwinm	r5,r5,0,MSR_EE_BIT+1,MSR_EE_BIT-1	; Turn off interruptions
			mtmsr	r5								; No talking
			isync
			
deadsleep:	addi	r3,r3,1							; Make analyzer happy
			addi	r3,r3,1
			addi	r3,r3,1
			b		deadsleep						; Die the death of 1000 joys...
#endif	
			
			mfsprg	r12,0							; Get the per_proc_info
			mfspr	r4,hid0							; Get the current power-saving mode
			eqv		r10,r10,r10						; Get all foxes
			mfsprg	r11,2							; Get CPU specific features

			rlwinm.	r5,r11,0,pfNoL2PFNapb,pfNoL2PFNapb	; Turn off L2 Prefetch before sleep?
			beq	mpsL2PFok

			mfspr	r5,msscr0						; Get currect MSSCR0 value
			rlwinm	r5,r5,0,0,l2pfes-1				; Disable L2 Prefetch
			mtspr	msscr0,r5						; Updates MSSCR0 value
			sync
			isync

mpsL2PFok:
			rlwinm.	r5,r11,0,pf64Bitb,pf64Bitb		; PM bits are shifted on 64bit systems.
			bne		mpsPF64bit

			rlwinm	r4,r4,0,sleep+1,doze-1			; Clear all possible power-saving modes (not DPM though)
			oris	r4,r4,hi16(sleepm)				; Set sleep
			b		mpsClearDEC

mpsPF64bit:
			lis		r5, hi16(dozem|napm|sleepm)		; Clear all possible power-saving modes (not DPM though)
			sldi	r5, r5, 32
			andc	r4, r4, r5
			lis		r5, hi16(napm)					; Set sleep
			sldi	r5, r5, 32
			or		r4, r4, r5

mpsClearDEC:
			mfmsr	r5								; Get the current MSR
			rlwinm	r10,r10,0,1,31					; Make 0x7FFFFFFF
			mtdec	r10								; Load decrimenter with 0x7FFFFFFF
			isync									; and make sure,
			mfdec	r9								; really sure, it gets there
			
			mtcrf	0x07,r11						; Get the cache flags, etc

			rlwinm	r5,r5,0,MSR_DR_BIT+1,MSR_IR_BIT-1	; Turn off translation		
;
;			Note that we need translation off before we set the HID to sleep.  Otherwise
;			we will ignore any PTE misses that occur and cause an infinite loop.
;
			bt		pfNoMSRirb,mpsNoMSR				; No MSR...

			mtmsr	r5								; Translation off
			isync									; Toss prefetch
			b		mpsNoMSRx
			
mpsNoMSR:	
			li		r0,loadMSR						; Get the MSR setter SC
			mr		r3,r5							; Get new MSR
			sc										; Set it
mpsNoMSRx:

			ori		r3,r5,lo16(MASK(MSR_EE))		; Flip on EE
			sync
			mtspr	hid0,r4							; Set up the HID to sleep
			mfspr	r4,hid0							; Yes, this is silly, keep it here
			mfspr	r4,hid0							; Yes, this is a duplicate, keep it here
			mfspr	r4,hid0							; Yes, this is a duplicate, keep it here
			mfspr	r4,hid0							; Yes, this is a duplicate, keep it here
			mfspr	r4,hid0							; Yes, this is a duplicate, keep it here
			mfspr	r4,hid0							; Yes, this is a duplicate, keep it here

			mtmsr	r3								; Enable for interrupts to drain decrimenter
				
			add		r6,r4,r5						; Just waste time
			add		r6,r6,r4						; A bit more
			add		r6,r6,r5						; A bit more

			mtmsr	r5								; Interruptions back off
			isync									; Toss prefetch

;
;			We are here with translation off, interrupts off, all possible
;			interruptions drained off, and a decrimenter that will not pop.
;

			bl		EXT(cacheInit)					; Clear out the caches.  This will leave them on
			bl		EXT(cacheDisable)				; Turn off all caches
			
			mfmsr	r5								; Get the current MSR
			oris	r5,r5,hi16(MASK(MSR_POW))		; Turn on power management in next MSR
													; Leave EE off because power goes off shortly
			mfsprg	r12,0							; Get the per_proc_info
			li		r10,PP_CPU_FLAGS
			lhz		r11,PP_CPU_FLAGS(r12)			; Get the flags
			ori		r11,r11,SleepState				; Marked SleepState
			sth		r11,PP_CPU_FLAGS(r12)			; Set the flags
			dcbf	r10,r12
			
			mfsprg	r11,2							; Get CPU specific features
			rlwinm.	r0,r11,0,pf64Bitb,pf64Bitb		; Test for 64 bit processor
			eqv		r4,r4,r4						; Get all foxes
			rlwinm	r4,r4,0,1,31					; Make 0x7FFFFFFF
			beq		slSleepNow						; skip if 32-bit...
			li		r3, 0x4000						; Cause decrimenter to roll over soon
			mtdec	r3								; Load decrimenter with 0x00004000
			isync									; and make sure,
			mfdec	r3								; really sure, it gets there
			
slSleepNow:
			sync									; Sync it all up
			mtmsr	r5								; Do sleep with interruptions enabled
			isync									; Take a pill
			mtdec	r4								; Load decrimenter with 0x7FFFFFFF
			isync									; and make sure,
			mfdec	r3								; really sure, it gets there
			b		slSleepNow						; Go back to sleep if we wake up...
			


/*  Initialize all caches including the TLBs
 *
 *	void cacheInit(void)
 *
 *	This is used to force the caches to an initial clean state.  First, we 
 *	check if the cache is on, if so, we need to flush the contents to memory.
 *	Then we invalidate the L1. Next, we configure and invalidate the L2 etc.
 *	Finally we turn on all of the caches
 *
 *	Note that if translation is not disabled when this is called, the TLB will not
 *	be completely clear after return.
 *
 */

;			Force a line boundry here
			.align	5
			.globl	EXT(cacheInit)

LEXT(cacheInit)

			mfsprg	r12,0							; Get the per_proc_info
			mfspr	r9,hid0							; Get the current power-saving mode
			
			mfsprg	r11,2							; Get CPU specific features
			mfmsr	r7								; Get the current MSR
			rlwinm	r7,r7,0,MSR_FP_BIT+1,MSR_FP_BIT-1	; Force floating point off
			rlwinm	r7,r7,0,MSR_VEC_BIT+1,MSR_VEC_BIT-1	; Force vectors off
			rlwimi	r11,r11,pfLClckb+1,31,31		; Move pfLClck to another position (to keep from using non-volatile CRs)
			rlwinm	r5,r7,0,MSR_DR_BIT+1,MSR_IR_BIT-1	; Turn off translation		
			rlwinm	r5,r5,0,MSR_EE_BIT+1,MSR_EE_BIT-1	; Turn off interruptions
			mtcrf	0x87,r11						; Get the feature flags
			lis		r10,hi16(dozem|napm|sleepm|dpmm)	; Mask of power management bits
			bf--	pf64Bitb,cIniNSF1				; Skip if 32-bit...
			
			sldi	r10,r10,32						; Position the masks

cIniNSF1:	andc	r4,r9,r10						; Clean up the old power bits		
			mtspr	hid0,r4							; Set up the HID
			mfspr	r4,hid0							; Yes, this is silly, keep it here
			mfspr	r4,hid0							; Yes, this is a duplicate, keep it here
			mfspr	r4,hid0							; Yes, this is a duplicate, keep it here
			mfspr	r4,hid0							; Yes, this is a duplicate, keep it here
			mfspr	r4,hid0							; Yes, this is a duplicate, keep it here
			mfspr	r4,hid0							; Yes, this is a duplicate, keep it here

			bt		pfNoMSRirb,ciNoMSR				; No MSR...

			mtmsr	r5								; Translation and all off
			isync									; Toss prefetch
			b		ciNoMSRx
			
ciNoMSR:	
			li		r0,loadMSR						; Get the MSR setter SC
			mr		r3,r5							; Get new MSR
			sc										; Set it
ciNoMSRx:
			
			bf		pfAltivecb,cinoDSS				; No Altivec here...
			
			dssall									; Stop streams
			sync

cinoDSS:	li		r5,tlbieLock					; Get the TLBIE lock
			li		r0,128							; Get number of TLB entries
			
			li		r6,0							; Start at 0
			bf--	pf64Bitb,citlbhang				; Skip if 32-bit...
			li		r0,1024							; Get the number of TLB entries

citlbhang:	lwarx	r2,0,r5							; Get the TLBIE lock
			mr.		r2,r2							; Is it locked?
			bne-	citlbhang						; It is locked, go wait...
			stwcx.	r0,0,r5							; Try to get it
			bne-	citlbhang						; We was beat...

			mtctr	r0								; Set the CTR
			
cipurgeTLB:	tlbie	r6								; Purge this entry
			addi	r6,r6,4096						; Next page
			bdnz	cipurgeTLB						; Do them all...
			
			mtcrf	0x80,r11						; Set SMP capability
			sync									; Make sure all TLB purges are done
			eieio									; Order, order in the court
			
			bf		pfSMPcapb,cinoSMP				; SMP incapable...
			
			tlbsync									; Sync all TLBs
			sync
			isync
			
			bf--	pf64Bitb,cinoSMP				; Skip if 32-bit...
			ptesync									; Wait for quiet again
			sync
			
cinoSMP:	stw		r2,tlbieLock(0)					; Unlock TLBIE lock

			bt++	pf64Bitb,cin64					; Skip if 64-bit...

			rlwinm.	r0,r9,0,ice,dce					; Were either of the level 1s on?
			beq-	cinoL1							; No, no need to flush...
			
            rlwinm.	r0,r11,0,pfL1fab,pfL1fab		; do we have L1 flush assist?
			beq		ciswdl1							; If no hw flush assist, go do by software...
			
			mfspr	r8,msscr0						; Get the memory system control register
			oris	r8,r8,hi16(dl1hwfm)				; Turn on the hardware flush request
			
			mtspr	msscr0,r8						; Start the flush operation
			
ciwdl1f:	mfspr	r8,msscr0						; Get the control register again
			
			rlwinm.	r8,r8,0,dl1hwf,dl1hwf			; Has the flush request been reset yet?
			bne		ciwdl1f							; No, flush is still in progress...
			b		ciinvdl1						; Go invalidate l1...
			
;
;			We need to either make this very complicated or to use ROM for
;			the flush.  The problem is that if during the following sequence a
;			snoop occurs that invalidates one of the lines in the cache, the
;			PLRU sequence will be altered making it possible to miss lines
;			during the flush.  So, we either need to dedicate an area of RAM
;			to each processor, lock use of a RAM area, or use ROM.  ROM is
;			by far the easiest. Note that this is not an issue for machines
;			that have harware flush assists.
;

ciswdl1:	lwz		r0,pfl1dSize(r12)				; Get the level 1 cache size
					
			bf		31,cisnlck						; Skip if pfLClck not set...
			
			mfspr	r4,msscr0						; ?
			rlwinm	r6,r4,0,0,l2pfes-1				; ?
			mtspr	msscr0,r6						; Set it
			sync
			isync
			
			mfspr	r8,ldstcr						; Save the LDSTCR
			li		r2,1							; Get a mask of 0x01
			lis		r3,0xFFF0						; Point to ROM
			rlwinm	r11,r0,29,3,31					; Get the amount of memory to handle all indexes

			li		r6,0							; Start here
			
cisiniflsh:	dcbf	r6,r3							; Flush each line of the range we use
			addi	r6,r6,32						; Bump to the next
			cmplw	r6,r0							; Have we reached the end?
			blt+	cisiniflsh						; Nope, continue initial flush...
			
			sync									; Make sure it is done
	
			addi	r11,r11,-1						; Get mask for index wrap	
			li		r6,0							; Get starting offset
						
cislckit:	not		r5,r2							; Lock all but 1 way
			rlwimi	r5,r8,0,0,23					; Build LDSTCR
			mtspr	ldstcr,r5						; Lock a way
			sync									; Clear out memory accesses
			isync									; Wait for all
			
			
cistouch:	lwzx	r10,r3,r6						; Pick up some trash
			addi	r6,r6,32						; Go to the next index
			and.	r0,r6,r11						; See if we are about to do next index
			bne+	cistouch						; Nope, do more...
			
			sync									; Make sure it is all done
			isync									
			
			sub		r6,r6,r11						; Back up to start + 1
			addi	r6,r6,-1						; Get it right
			
cisflush:	dcbf	r3,r6							; Flush everything out
			addi	r6,r6,32						; Go to the next index
			and.	r0,r6,r11						; See if we are about to do next index
			bne+	cisflush						; Nope, do more...

			sync									; Make sure it is all done
			isync									
			
			
			rlwinm.	r2,r2,1,24,31					; Shift to next way
			bne+	cislckit						; Do this for all ways...

			mtspr	ldstcr,r8						; Slam back to original
			sync
			isync
			
			mtspr	msscr0,r4						; ?
			sync
			isync

			b		cinoL1							; Go on to level 2...
			

cisnlck:	rlwinm	r2,r0,0,1,30					; Double cache size
			add		r0,r0,r2						; Get 3 times cache size
			rlwinm	r0,r0,26,6,31					; Get 3/2 number of cache lines
			lis		r3,0xFFF0						; Dead recon ROM address for now
			mtctr	r0								; Number of lines to flush

ciswfldl1a:	lwz		r2,0(r3)						; Flush anything else
			addi	r3,r3,32						; Next line
			bdnz	ciswfldl1a						; Flush the lot...
			
ciinvdl1:	sync									; Make sure all flushes have been committed

			mfspr	r8,hid0							; Get the HID0 bits
			rlwinm	r8,r8,0,dce+1,ice-1				; Clear cache enables
			mtspr	hid0,r8							; and turn off L1 cache
			sync									; Make sure all is done
			isync

			ori		r8,r8,lo16(icem|dcem|icfim|dcfim)	; Set the HID0 bits for enable, and invalidate
			sync
			isync										
			
			mtspr	hid0,r8							; Start the invalidate and turn on cache	
			rlwinm	r8,r8,0,dcfi+1,icfi-1			; Turn off the invalidate bits
			mtspr	hid0,r8							; Turn off the invalidate (needed for some older machines)
			sync

			
cinoL1:
;
;			Flush and disable the level 2
;
            mfsprg	r10,2							; need to check 2 features we did not put in CR
            rlwinm.	r0,r10,0,pfL2b,pfL2b			; do we have L2?
			beq		cinol2							; No level 2 cache to flush

			mfspr	r8,l2cr							; Get the L2CR
			lwz		r3,pfl2cr(r12)					; Get the L2CR value
			rlwinm.		r0,r8,0,l2e,l2e					; Was the L2 enabled?
			bne		ciflushl2					; Yes, force flush
			cmplwi		r8, 0						; Was the L2 all the way off?
			beq		ciinvdl2					; Yes, force invalidate
			lis		r0,hi16(l2sizm|l2clkm|l2ramm|l2ohm)	; Get confiuration bits
			xor		r2,r8,r3						; Get changing bits?
			ori		r0,r0,lo16(l2slm|l2dfm|l2bypm)	; More config bits
			and.	r0,r0,r2						; Did any change?
			bne-	ciinvdl2						; Yes, just invalidate and get PLL synced...		
			
ciflushl2:
            rlwinm.	r0,r10,0,pfL2fab,pfL2fab		; hardware-assisted L2 flush?
			beq		ciswfl2							; Flush not in hardware...
			
			mr		r10,r8							; Take a copy now
			
			bf		31,cinol2lck					; Skip if pfLClck not set...
			
			oris	r10,r10,hi16(l2ionlym|l2donlym)	; Set both instruction- and data-only
			sync
			mtspr	l2cr,r10						; Lock out the cache
			sync
			isync
			
cinol2lck:	ori		r10,r10,lo16(l2hwfm)			; Request flush
			sync									; Make sure everything is done
			
			mtspr	l2cr,r10						; Request flush
			
cihwfl2:	mfspr	r10,l2cr						; Get back the L2CR
			rlwinm.	r10,r10,0,l2hwf,l2hwf			; Is the flush over?
			bne+	cihwfl2							; Nope, keep going...
			b		ciinvdl2						; Flush done, go invalidate L2...
			
ciswfl2:
			lwz		r0,pfl2Size(r12)				; Get the L2 size
			oris	r2,r8,hi16(l2dom)				; Set L2 to data only mode

			b		ciswfl2doa					; Branch to next line...

			.align  5
ciswfl2doc:
			mtspr	l2cr,r2							; Disable L2
			sync
			isync
			b		ciswfl2dod					; It is off, go invalidate it...

ciswfl2doa:
			b		ciswfl2dob					; Branch to next...

ciswfl2dob:
			sync								; Finish memory stuff
			isync								; Stop speculation
			b		ciswfl2doc					; Jump back up and turn on data only...
ciswfl2dod:
			rlwinm	r0,r0,27,5,31					; Get the number of lines
			lis		r10,0xFFF0						; Dead recon ROM for now
			mtctr	r0								; Set the number of lines
			
ciswfldl2a:	lwz		r0,0(r10)						; Load something to flush something
			addi	r10,r10,32						; Next line
			bdnz	ciswfldl2a						; Do the lot...
			
ciinvdl2:	rlwinm	r8,r3,0,l2e+1,31				; Clear the enable bit
			b		cinla							; Branch to next line...

			.align  5
cinlc:		mtspr	l2cr,r8							; Disable L2
			sync
			isync
			b		ciinvl2							; It is off, go invalidate it...
			
cinla:		b		cinlb							; Branch to next...

cinlb:		sync									; Finish memory stuff
			isync									; Stop speculation
			b		cinlc							; Jump back up and turn off cache...
			
ciinvl2:	sync
			isync

			cmplwi	r3, 0							; Should the L2 be all the way off?
			beq	cinol2							; Yes, done with L2

			oris	r2,r8,hi16(l2im)				; Get the invalidate flag set
			
			mtspr	l2cr,r2							; Start the invalidate
			sync
			isync
ciinvdl2a:	mfspr	r2,l2cr							; Get the L2CR
            mfsprg	r0,2							; need to check a feature in "non-volatile" set
            rlwinm.	r0,r0,0,pfL2ib,pfL2ib			; flush in HW?
			beq		ciinvdl2b						; Flush not in hardware...
			rlwinm.	r2,r2,0,l2i,l2i					; Is the invalidate still going?
			bne+	ciinvdl2a						; Assume so, this will take a looong time...
			sync
			b		cinol2							; No level 2 cache to flush
ciinvdl2b:
			rlwinm.	r2,r2,0,l2ip,l2ip				; Is the invalidate still going?
			bne+	ciinvdl2a						; Assume so, this will take a looong time...
			sync
			mtspr	l2cr,r8							; Turn off the invalidate request
			
cinol2:
			
;
;			Flush and enable the level 3
;
			bf		pfL3b,cinol3					; No level 3 cache to flush

			mfspr	r8,l3cr							; Get the L3CR
			lwz		r3,pfl3cr(r12)					; Get the L3CR value
			rlwinm.		r0,r8,0,l3e,l3e					; Was the L3 enabled?
			bne		ciflushl3					; Yes, force flush
			cmplwi		r8, 0						; Was the L3 all the way off?
			beq		ciinvdl3					; Yes, force invalidate
			lis		r0,hi16(l3pem|l3sizm|l3dxm|l3clkm|l3spom|l3ckspm)	; Get configuration bits
			xor		r2,r8,r3						; Get changing bits?
			ori		r0,r0,lo16(l3pspm|l3repm|l3rtm|l3cyam|l3dmemm|l3dmsizm)	; More config bits
			and.	r0,r0,r2						; Did any change?
			bne-	ciinvdl3						; Yes, just invalidate and get PLL synced...
			
ciflushl3:
			sync									; 7450 book says do this even though not needed
			mr		r10,r8							; Take a copy now
			
			bf		31,cinol3lck					; Skip if pfL23lck not set...
			
			oris	r10,r10,hi16(l3iom)				; Set instruction-only
			ori		r10,r10,lo16(l3donlym)			; Set data-only
			sync
			mtspr	l3cr,r10						; Lock out the cache
			sync
			isync
			
cinol3lck:	ori		r10,r10,lo16(l3hwfm)			; Request flush
			sync									; Make sure everything is done
			
			mtspr	l3cr,r10						; Request flush
			
cihwfl3:	mfspr	r10,l3cr						; Get back the L3CR
			rlwinm.	r10,r10,0,l3hwf,l3hwf			; Is the flush over?
			bne+	cihwfl3							; Nope, keep going...

ciinvdl3:	rlwinm	r8,r3,0,l3e+1,31				; Clear the enable bit
			sync									; Make sure of life, liberty, and justice
			mtspr	l3cr,r8							; Disable L3
			sync

			cmplwi	r3, 0							; Should the L3 be all the way off?
			beq	cinol3							; Yes, done with L3

			ori		r8,r8,lo16(l3im)				; Get the invalidate flag set

			mtspr	l3cr,r8							; Start the invalidate

ciinvdl3b:	mfspr	r8,l3cr							; Get the L3CR
			rlwinm.	r8,r8,0,l3i,l3i					; Is the invalidate still going?
			bne+	ciinvdl3b						; Assume so...
			sync

			lwz	r10, pfBootConfig(r12)					; ?
			rlwinm.	r10, r10, 24, 28, 31					; ?
			beq	ciinvdl3nopdet						; ?
			
			mfspr	r8,l3pdet						; ?
			srw	r2, r8, r10						; ?
			rlwimi	r2, r8, 0, 24, 31					; ?
			subfic	r10, r10, 32						; ?
			li	r8, -1							; ?
			ori	r2, r2, 0x0080						; ?
			slw	r8, r8, r10						; ?
			or	r8, r2, r8						; ?
			mtspr	l3pdet, r8						; ?
			isync

ciinvdl3nopdet:
			mfspr	r8,l3cr							; Get the L3CR
			rlwinm	r8,r8,0,l3clken+1,l3clken-1		; Clear the clock enable bit
			mtspr	l3cr,r8							; Disable the clock

			li		r2,128							; ?
ciinvdl3c:	addi	r2,r2,-1						; ?
			cmplwi	r2,0							; ?
			bne+	ciinvdl3c

			mfspr	r10,msssr0						; ?
			rlwinm	r10,r10,0,vgL3TAG+1,vgL3TAG-1	; ?
			mtspr	msssr0,r10						; ?
			sync

			mtspr	l3cr,r3							; Enable it as desired
			sync
cinol3:
            mfsprg	r0,2							; need to check a feature in "non-volatile" set
            rlwinm.	r0,r0,0,pfL2b,pfL2b				; is there an L2 cache?
			beq		cinol2a							; No level 2 cache to enable

			lwz		r3,pfl2cr(r12)					; Get the L2CR value
			cmplwi		r3, 0						; Should the L2 be all the way off?
			beq		cinol2a							: Yes, done with L2
			mtspr	l2cr,r3							; Enable it as desired
			sync

;
;			Invalidate and turn on L1s
;

cinol2a:	
			bt		31,cinoexit						; Skip if pfLClck set...

			rlwinm	r8,r9,0,dce+1,ice-1				; Clear the I- and D- cache enables
			mtspr	hid0,r8							; Turn off dem caches
			sync
			
			ori		r8,r9,lo16(icem|dcem|icfim|dcfim)	; Set the HID0 bits for enable, and invalidate
			rlwinm	r9,r8,0,dcfi+1,icfi-1			; Turn off the invalidate bits
			sync
			isync											

			mtspr	hid0,r8							; Start the invalidate and turn on L1 cache	

cinoexit:	mtspr	hid0,r9							; Turn off the invalidate (needed for some older machines) and restore entry conditions
			sync
			mtmsr	r7								; Restore MSR to entry
			isync
			blr										; Return...


;
;			Handle 64-bit architecture
;			This processor can not run without caches, so we just push everything out
;			and flush.  It will be relativily clean afterwards
;
			
			.align	5
			
cin64:		
			mfspr	r10,hid1						; Save hid1
			mfspr	r4,hid4							; Save hid4
			mr		r12,r10							; Really save hid1
			mr		r11,r4							; Get a working copy of hid4

			li		r0,0							; Get a 0
			eqv		r2,r2,r2						; Get all foxes
			
			rldimi	r10,r0,55,7						; Clear I$ prefetch bits (7:8)
			
			isync
			mtspr	hid1,r10						; Stick it
			mtspr	hid1,r10						; Stick it again
			isync

			rldimi	r11,r2,38,25					; Disable D$ prefetch (25:25)
			
			sync
			mtspr	hid4,r11						; Stick it
			isync

			li		r3,8							; Set bit 28+32
			sldi	r3,r3,32						; Make it bit 28
			or		r3,r3,r11						; Turn on the flash invalidate L1D$
			
			oris	r5,r11,0x0600					; Set disable L1D$ bits		
			sync
			mtspr	hid4,r3							; Invalidate
			isync
	
			mtspr	hid4,r5							; Un-invalidate and disable L1D$
			isync
			
			lis		r8,GUSModeReg					; Get the GUS mode ring address
			mfsprg	r0,2							; Get the feature flags
			ori		r8,r8,0x8000					; Set to read data
			rlwinm.	r0,r0,pfSCOMFixUpb+1,31,31		; Set shift if we need a fix me up

			sync

			mtspr	scomc,r8						; Request the GUS mode
			mfspr	r11,scomd						; Get the GUS mode
			mfspr	r8,scomc						; Get back the status (we just ignore it)
			sync
			isync							

			sld		r11,r11,r0						; Fix up if needed

			ori		r6,r11,lo16(GUSMdmapen)			; Set the bit that means direct L2 cache address
			lis		r8,GUSModeReg					; Get GUS mode register address
				
			sync

			mtspr	scomd,r6						; Set that we want direct L2 mode
			mtspr	scomc,r8						; Tell GUS we want direct L2 mode
			mfspr	r3,scomc						; Get back the status
			sync
			isync							

			li		r3,0							; Clear start point
		
cflushlp:	lis		r6,0x0040						; Pick 4MB line as our target
			or		r6,r6,r3						; Put in the line offset
			lwz		r5,0(r6)						; Load a line
			addis	r6,r6,8							; Roll bit 42:44
			lwz		r5,0(r6)						; Load a line
			addis	r6,r6,8							; Roll bit 42:44
			lwz		r5,0(r6)						; Load a line
			addis	r6,r6,8							; Roll bit 42:44
			lwz		r5,0(r6)						; Load a line
			addis	r6,r6,8							; Roll bit 42:44
			lwz		r5,0(r6)						; Load a line
			addis	r6,r6,8							; Roll bit 42:44
			lwz		r5,0(r6)						; Load a line
			addis	r6,r6,8							; Roll bit 42:44
			lwz		r5,0(r6)						; Load a line
			addis	r6,r6,8							; Roll bit 42:44
			lwz		r5,0(r6)						; Load a line

			addi	r3,r3,128						; Next line
			andis.	r5,r3,8							; Have we done enough?
			beq++	cflushlp						; Not yet...
			
			sync

			lis		r6,0x0040						; Pick 4MB line as our target

cflushx:	dcbf	0,r6							; Flush line and invalidate
			addi	r6,r6,128						; Next line
			andis.	r5,r6,0x0080					; Have we done enough?
			beq++	cflushx							; Keep on flushing...

			mr		r3,r10							; Copy current hid1
			rldimi	r3,r2,54,9						; Set force icbi match mode
			
			li		r6,0							; Set start if ICBI range
			isync
			mtspr	hid1,r3							; Stick it
			mtspr	hid1,r3							; Stick it again
			isync

cflicbi:	icbi	0,r6							; Kill I$
			addi	r6,r6,128						; Next line
			andis.	r5,r6,1							; Have we done them all?
			beq++	cflicbi							; Not yet...

			lis		r8,GUSModeReg					; Get GUS mode register address
				
			sync

			mtspr	scomd,r11						; Set that we do not want direct mode
			mtspr	scomc,r8						; Tell GUS we do not want direct mode
			mfspr	r3,scomc						; Get back the status
			sync
			isync							

			isync
			mtspr	hid0,r9							; Restore entry hid0
			mfspr	r9,hid0							; Yes, this is silly, keep it here
			mfspr	r9,hid0							; Yes, this is a duplicate, keep it here
			mfspr	r9,hid0							; Yes, this is a duplicate, keep it here
			mfspr	r9,hid0							; Yes, this is a duplicate, keep it here
			mfspr	r9,hid0							; Yes, this is a duplicate, keep it here
			mfspr	r9,hid0							; Yes, this is a duplicate, keep it here
			isync

			isync
			mtspr	hid1,r12						; Restore entry hid1
			mtspr	hid1,r12						; Stick it again
			isync
		
			sync
			mtspr	hid4,r4							; Restore entry hid4
			isync

			sync
			mtmsr	r7								; Restore MSR to entry
			isync
			blr										; Return...
			
			

/*  Disables all caches
 *
 *	void cacheDisable(void)
 *
 *	Turns off all caches on the processor. They are not flushed.
 *
 */

;			Force a line boundry here
			.align	5
			.globl	EXT(cacheDisable)

LEXT(cacheDisable)

			mfsprg	r11,2							; Get CPU specific features
			mtcrf	0x83,r11						; Set feature flags
			
			bf		pfAltivecb,cdNoAlt				; No vectors...
			
			dssall									; Stop streams
			
cdNoAlt:	sync
			
			btlr	pf64Bitb						; No way to disable a 64-bit machine...
			
			mfspr	r5,hid0							; Get the hid
			rlwinm	r5,r5,0,dce+1,ice-1				; Clear the I- and D- cache enables
			mtspr	hid0,r5							; Turn off dem caches
			sync

            rlwinm.	r0,r11,0,pfL2b,pfL2b			; is there an L2?
			beq		cdNoL2							; Skip if no L2...

			mfspr	r5,l2cr							; Get the L2
			rlwinm	r5,r5,0,l2e+1,31				; Turn off enable bit

			b		cinlaa							; Branch to next line...

			.align  5
cinlcc:		mtspr	l2cr,r5							; Disable L2
			sync
			isync
			b		cdNoL2							; It is off, we are done...
			
cinlaa:		b		cinlbb							; Branch to next...

cinlbb:		sync									; Finish memory stuff
			isync									; Stop speculation
			b		cinlcc							; Jump back up and turn off cache...

cdNoL2:

			bf		pfL3b,cdNoL3					; Skip down if no L3...
			
			mfspr	r5,l3cr							; Get the L3
			rlwinm	r5,r5,0,l3e+1,31				; Turn off enable bit
			rlwinm	r5,r5,0,l3clken+1,l3clken-1		; Turn off cache enable bit
			mtspr	l3cr,r5							; Disable the caches
			sync
			
cdNoL3:
			blr										; Leave...


/*  Initialize processor thermal monitoring  
 *	void ml_thrm_init(void)
 *
 *	Build initial TAU registers and start them all going.
 *	We ca not do this at initial start up because we need to have the processor frequency first.
 *	And just why is this in assembler when it does not have to be?? Cause I am just too 
 *	lazy to open up a "C" file, thats why.
 */

;			Force a line boundry here
			.align	5
			.globl	EXT(ml_thrm_init)

LEXT(ml_thrm_init)

			mfsprg	r12,0							; Get the per_proc blok
			lis		r11,hi16(EXT(gPEClockFrequencyInfo))	; Get top of processor information
			mfsprg	r10,2							; Get CPU specific features
			ori		r11,r11,lo16(EXT(gPEClockFrequencyInfo))	; Get bottom of processor information
			mtcrf	0x40,r10						; Get the installed features

			li		r3,lo16(thrmtidm|thrmvm)		; Set for lower-than thermal event at 0 degrees
			bflr	pfThermalb						; No thermal monitoring on this cpu
			mtspr	thrm1,r3						; Do it

			lwz		r3,thrmthrottleTemp(r12)		; Get our throttle temprature
			rlwinm	r3,r3,31-thrmthre,thrmthrs,thrmthre	; Position it
			ori		r3,r3,lo16(thrmvm)				; Set for higher-than event 
			mtspr	thrm2,r3						; Set it

			lis		r4,hi16(1000000)				; Top of million
;
;			Note: some CPU manuals say this is processor clocks, some say bus rate.  The latter
;			makes more sense because otherwise we can not get over about 400MHz.
#if 0
			lwz		r3,PECFIcpurate(r11)				; Get the processor speed
#else
			lwz		r3,PECFIbusrate(r11)				; Get the bus speed
#endif
			ori		r4,r4,lo16(1000000)				; Bottom of million
			lis		r7,hi16(thrmsitvm>>1)			; Get top of highest possible value
			divwu	r3,r3,r4						; Get number of cycles per microseconds
			ori		r7,r7,lo16(thrmsitvm>>1)		; Get the bottom of the highest possible value
			addi	r3,r3,1							; Insure we have enough
			mulli	r3,r3,20						; Get 20 microseconds worth of cycles
			cmplw	r3,r7							; Check against max
			ble+	smallenuf						; It is ok...
			mr		r3,r7							; Saturate
			
smallenuf:	rlwinm	r3,r3,31-thrmsitve,thrmsitvs,thrmsitve	; Position			
			ori		r3,r3,lo16(thrmem)				; Enable with at least 20micro sec sample
			stw		r3,thrm3val(r12)				; Save this in case we need it later
			mtspr	thrm3,r3						; Do it
			blr


/*  Set thermal monitor bounds 
 *	void ml_thrm_set(unsigned int low, unsigned int high)
 *
 *	Set TAU to interrupt below low and above high.  A value of
 *	zero disables interruptions in that direction.
 */

;			Force a line boundry here
			.align	5
			.globl	EXT(ml_thrm_set)

LEXT(ml_thrm_set)

			mfmsr	r0								; Get the MSR
			rlwinm	r0,r0,0,MSR_FP_BIT+1,MSR_FP_BIT-1	; Force floating point off
			rlwinm	r0,r0,0,MSR_VEC_BIT+1,MSR_VEC_BIT-1	; Force vectors off
			rlwinm	r6,r0,0,MSR_EE_BIT+1,MSR_EE_BIT-1	; Clear EE bit
			mtmsr	r6
			isync

			mfsprg	r12,0							; Get the per_proc blok

			rlwinm.	r6,r3,31-thrmthre,thrmthrs,thrmthre	; Position it and see if enabled
			mfsprg	r9,2							; Get CPU specific features
			stw		r3,thrmlowTemp(r12)				; Set the low temprature
			mtcrf	0x40,r9							; See if we can thermal this machine
			rlwinm	r9,r9,(((31-thrmtie)+(pfThermIntb+1))&31),thrmtie,thrmtie	; Set interrupt enable if this machine can handle it
			bf		pfThermalb,tsetcant				; No can do...
			beq		tsetlowo						; We are setting the low off...
			ori		r6,r6,lo16(thrmtidm|thrmvm)		; Set the lower-than and valid bit
			or		r6,r6,r9						; Set interruption request if supported

tsetlowo:	mtspr	thrm1,r6						; Cram the register
			
			rlwinm.	r6,r4,31-thrmthre,thrmthrs,thrmthre	; Position it and see if enabled
			stw		r4,thrmhighTemp(r12)			; Set the high temprature
			beq		tsethigho						; We are setting the high off...
			ori		r6,r6,lo16(thrmvm)				; Set valid bit
			or		r6,r6,r9						; Set interruption request if supported

tsethigho:	mtspr	thrm2,r6						; Cram the register

tsetcant:	mtmsr	r0								; Reenable interruptions
			blr										; Leave...

/*  Read processor temprature  
 *	unsigned int ml_read_temp(void)
 *
 */

;			Force a line boundry here
			.align	5
			.globl	EXT(ml_read_temp)

LEXT(ml_read_temp)

			mfmsr	r9								; Save the MSR
			li		r5,15							; Starting point for ranging (start at 15 so we do not overflow)
			rlwinm	r9,r9,0,MSR_FP_BIT+1,MSR_FP_BIT-1	; Force floating point off
			rlwinm	r9,r9,0,MSR_VEC_BIT+1,MSR_VEC_BIT-1	; Force vectors off
			rlwinm	r8,r9,0,MSR_EE_BIT+1,MSR_EE_BIT-1	; Turn off interruptions
			mfsprg	r7,2							; Get CPU specific features
			mtmsr	r8								; Do not allow interruptions
			mtcrf	0x40,r7							; See if we can thermal this machine
			bf		pfThermalb,thrmcant				; No can do...

			mfspr	r11,thrm1						; Save thrm1

thrmrange:	rlwinm	r4,r5,31-thrmthre,thrmthrs,thrmthre	; Position it
			ori		r4,r4,lo16(thrmtidm|thrmvm)		; Flip on the valid bit and make comparision for less than

			mtspr	thrm1,r4						; Set the test value
			
thrmreada:	mfspr	r3,thrm1						; Get the thermal register back
			rlwinm.	r0,r3,0,thrmtiv,thrmtiv			; Has it settled yet?
			beq+	thrmreada						; Nope...

			rlwinm.	r0,r3,0,thrmtin,thrmtin			; Are we still under the threshold?
			bne		thrmsearch						; No, we went over...

			addi	r5,r5,16						; Start by trying every 16 degrees
			cmplwi	r5,127							; Have we hit the max?
			blt-	thrmrange						; Got some more to do...

thrmsearch:	rlwinm	r4,r5,31-thrmthre,thrmthrs,thrmthre	; Position it
			ori		r4,r4,lo16(thrmtidm|thrmvm)		; Flip on the valid bit and make comparision for less than
			
			mtspr	thrm1,r4						; Set the test value
			
thrmread:	mfspr	r3,thrm1						; Get the thermal register back
			rlwinm.	r0,r3,0,thrmtiv,thrmtiv			; Has it settled yet?
			beq+	thrmread						; Nope...
			
			rlwinm.	r0,r3,0,thrmtin,thrmtin			; Are we still under the threshold?
			beq		thrmdone						; No, we hit it...
			addic.	r5,r5,-1						; Go down a degree
			bge+	thrmsearch						; Try again (until we are below freezing)...
			
thrmdone:	addi	r3,r5,1							; Return the temprature (bump it up to make it correct)
			mtspr	thrm1,r11						; Restore the thermal register
			mtmsr	r9								; Re-enable interruptions
			blr										; Leave...
			
thrmcant:	eqv		r3,r3,r3						; Return bogus temprature because we can not read it
			mtmsr	r9								; Re-enable interruptions
			blr										; Leave...

/*  Throttle processor speed up or down
 *	unsigned int ml_throttle(unsigned int step)
 *
 *	Returns old speed and sets new.  Both step and return are values from 0 to
 *	255 that define number of throttle steps, 0 being off and "ictcfim" is max * 2.
 *
 */

;			Force a line boundry here
			.align	5
			.globl	EXT(ml_throttle)

LEXT(ml_throttle)

			mfmsr	r9								; Save the MSR
			rlwinm	r9,r9,0,MSR_FP_BIT+1,MSR_FP_BIT-1	; Force floating point off
			rlwinm	r9,r9,0,MSR_VEC_BIT+1,MSR_VEC_BIT-1	; Force vectors off
			rlwinm	r8,r9,0,MSR_EE_BIT+1,MSR_EE_BIT-1	; Turn off interruptions
			cmplwi	r3,lo16(ictcfim>>1)				; See if we are going too far					
			mtmsr	r8								; Do not allow interruptions	
			isync		
			ble+	throtok							; Throttle value is ok...
			li		r3,lo16(ictcfim>>1)				; Set max

throtok:	rlwinm.	r4,r3,1,ictcfib,ictcfie			; Set the throttle
			beq		throtoff						; Skip if we are turning it off...
			ori		r4,r4,lo16(thrmvm)				; Turn on the valid bit
			
throtoff:	mfspr	r3,ictc							; Get the old throttle
			mtspr	ictc,r4							; Set the new
			rlwinm	r3,r3,31,1,31					; Shift throttle value over
			mtmsr	r9								; Restore interruptions
			blr										; Return...

/*
**      ml_get_timebase()
**
**      Entry   - R3 contains pointer to 64 bit structure.
**
**      Exit    - 64 bit structure filled in.
**
*/
;			Force a line boundry here
			.align	5
			.globl	EXT(ml_get_timebase)

LEXT(ml_get_timebase)

loop:
			mftbu   r4
			mftb    r5
			mftbu   r6
			cmpw    r6, r4
			bne-    loop
			
			stw     r4, 0(r3)
			stw     r5, 4(r3)
			
			blr

/*
 *		unsigned int cpu_number(void)
 *
 *			Returns the current cpu number. 
 */

			.align	5
			.globl	EXT(cpu_number)

LEXT(cpu_number)
			mfsprg	r4,0							; Get per-proc block
			lhz		r3,PP_CPU_NUMBER(r4)			; Get CPU number 
			blr										; Return...


/*
 *		void set_machine_current_act(thread_act_t)
 *
 *			Set the current activation
 */
			.align	5
			.globl	EXT(set_machine_current_act)

LEXT(set_machine_current_act)

			mtsprg	1,r3							; Set spr1 with the active thread
			blr										; Return...

/*
 *		thread_t current_act(void)
 *		thread_t current_thread(void)
 *
 *
 *			Return the current thread for outside components.
 */
			.align	5
			.globl	EXT(current_act)
			.globl	EXT(current_thread)

LEXT(current_act)
LEXT(current_thread)

			mfsprg	r3,1
			blr

			.align	5
			.globl	EXT(clock_get_uptime)
LEXT(clock_get_uptime)
1:			mftbu	r9
			mftb	r0
			mftbu	r11
			cmpw	r11,r9
			bne-	1b
			stw		r0,4(r3)
			stw		r9,0(r3)
			blr

		
			.align	5
			.globl	EXT(mach_absolute_time)
LEXT(mach_absolute_time)
1:			mftbu	r3
			mftb	r4
			mftbu	r0
			cmpw	r0,r3
			bne-	1b  
			blr

/*
**      ml_sense_nmi()
**
*/
;			Force a line boundry here
			.align	5
			.globl	EXT(ml_sense_nmi)

LEXT(ml_sense_nmi)

			blr										; Leave...

/*
**      ml_set_processor_speed()
**
*/
;			Force a line boundry here
			.align	5
			.globl	EXT(ml_set_processor_speed)

LEXT(ml_set_processor_speed)
			mfsprg	r5, 0									; Get the per_proc_info

			lwz		r6, pfPowerModes(r5)					; Get the supported power modes

			rlwinm.	r0, r6, 0, pmDualPLLb, pmDualPLLb		; Is DualPLL supported?
			bne		spsDPLL

			rlwinm.	r0, r6, 0, pmPowerTuneb, pmPowerTuneb	; Is DualPLL supported?
			bne		spsPowerTune

			b		spsDone									; No supported power modes

spsDPLL:
			cmpli	cr0, r3, 0								; Turn off BTIC before low speed
			beq		spsDPLL1
			mfspr	r4, hid0								; Get the current hid0 value
			rlwinm	r4, r4, 0, btic+1, btic-1				; Clear the BTIC bit
			sync
			mtspr	hid0, r4								; Set the new hid0 value
			isync
			sync

spsDPLL1:
			mfspr	r4, hid1								; Get the current PLL settings
			rlwimi  r4, r3, 31-hid1ps, hid1ps, hid1ps		; Copy the PLL Select bit
			stw		r4, pfHID1(r5)							; Save the new hid1 value
			mtspr	hid1, r4								; Select desired PLL

			cmpli	cr0, r3, 0								; Restore BTIC after high speed
			bne		spsDone
			lwz		r4, pfHID0(r5)							; Load the hid0 value
			sync
			mtspr	hid0, r4								; Set the hid0 value
			isync
			sync
			b		spsDone

spsPowerTune:
			b		spsDone

spsDone:
			blr

/*
**      ml_set_processor_voltage()
**
*/
;			Force a line boundry here
			.align	5
			.globl	EXT(ml_set_processor_voltage)

LEXT(ml_set_processor_voltage)
			mfsprg	r5, 0									; Get the per_proc_info

			lwz		r6, pfPowerModes(r5)					; Get the supported power modes

			rlwinm.	r0, r6, 0, pmDPLLVminb, pmDPLLVminb		; Is DPLL Vmin supported
			beq		spvDone

			mfspr	r4, hid2								; Get HID2 value
			rlwimi	r4, r3, 31-hid2vmin, hid2vmin, hid2vmin	; Insert the voltage mode bit
			mtspr	hid2, r4								; Set the voltage mode
			sync											; Make sure it is done

spvDone:
			blr

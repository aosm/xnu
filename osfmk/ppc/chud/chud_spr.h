/*
 * Copyright (c) 2003 Apple Computer, Inc. All rights reserved.
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

#ifndef _CHUD_SPR_H_
#define _CHUD_SPR_H_

/* PPC SPRs - 32-bit and 64-bit implementations */
#define chud_ppc_srr0		26
#define chud_ppc_srr1		27
#define chud_ppc_dsisr		18
#define chud_ppc_dar		19
#define chud_ppc_dec		22
#define chud_ppc_sdr1		25
#define chud_ppc_sprg0		272
#define chud_ppc_sprg1		273
#define chud_ppc_sprg2		274
#define chud_ppc_sprg3		275
#define chud_ppc_ear		282
#define chud_ppc_tbl		284
#define chud_ppc_tbu		285
#define chud_ppc_pvr		287
#define chud_ppc_ibat0u		528
#define chud_ppc_ibat0l		529
#define chud_ppc_ibat1u		530
#define chud_ppc_ibat1l		531
#define chud_ppc_ibat2u		532
#define chud_ppc_ibat2l		533
#define chud_ppc_ibat3u		534
#define chud_ppc_ibat3l		535
#define chud_ppc_dbat0u		536
#define chud_ppc_dbat0l		537
#define chud_ppc_dbat1u		538
#define chud_ppc_dbat1l		539
#define chud_ppc_dbat2u		540
#define chud_ppc_dbat2l		541
#define chud_ppc_dbat3u		542
#define chud_ppc_dbat3l		543
#define chud_ppc_dabr		1013
#define chud_ppc_msr		10000	/* FAKE */

/* PPC SPRs - 32-bit implementations */
#define chud_ppc32_sr0		20000	/* FAKE */
#define chud_ppc32_sr1		20001	/* FAKE */
#define chud_ppc32_sr2		20002	/* FAKE */
#define chud_ppc32_sr3		20003	/* FAKE */
#define chud_ppc32_sr4		20004	/* FAKE */
#define chud_ppc32_sr5		20005	/* FAKE */
#define chud_ppc32_sr6		20006	/* FAKE */
#define chud_ppc32_sr7		20007	/* FAKE */
#define chud_ppc32_sr8		20008	/* FAKE */
#define chud_ppc32_sr9		20009	/* FAKE */
#define chud_ppc32_sr10		20010	/* FAKE */
#define chud_ppc32_sr11		20011	/* FAKE */
#define chud_ppc32_sr12		20012	/* FAKE */
#define chud_ppc32_sr13		20013	/* FAKE */
#define chud_ppc32_sr14		20014	/* FAKE */
#define chud_ppc32_sr15		20015	/* FAKE */

/* PPC SPRs - 64-bit implementations */
#define chud_ppc64_asr		280

/* PPC SPRs - 750/750CX/750CXe/750FX Specific */
#define chud_750_upmc1		937
#define chud_750_upmc2		938
#define chud_750_upmc3		941
#define chud_750_upmc4		942
#define chud_750_mmcr0		952
#define chud_750_pmc1		953
#define chud_750_pmc2		954
#define chud_750_sia		955
#define chud_750_mmcr1		956
#define chud_750_pmc3		957
#define chud_750_pmc4		958
#define chud_750_hid0		1008
#define chud_750_hid1		1009
#define chud_750_iabr		1010
#define chud_750_l2cr		1017
#define chud_750_ictc		1019
#define chud_750_thrm1		1020
#define chud_750_thrm2		1021
#define chud_750_thrm3		1022
#define chud_750fx_ibat4u	560 /* 750FX only */
#define chud_750fx_ibat4l	561 /* 750FX only */
#define chud_750fx_ibat5u	562 /* 750FX only */
#define chud_750fx_ibat5l	563 /* 750FX only */
#define chud_750fx_ibat6u	564 /* 750FX only */
#define chud_750fx_ibat6l	565 /* 750FX only */
#define chud_750fx_ibat7u	566 /* 750FX only */
#define chud_750fx_ibat7l	567 /* 750FX only */
#define chud_750fx_dbat4u	568 /* 750FX only */
#define chud_750fx_dbat4l	569 /* 750FX only */
#define chud_750fx_dbat5u	570 /* 750FX only */
#define chud_750fx_dbat5l	571 /* 750FX only */
#define chud_750fx_dbat6u	572 /* 750FX only */
#define chud_750fx_dbat6l	573 /* 750FX only */
#define chud_750fx_dbat7u	574 /* 750FX only */
#define chud_750fx_dbat7l	575 /* 750FX only */
#define chud_750fx_hid2		1016  /* 750FX only */

/* PPC SPRs - 7400/7410 Specific */
#define chud_7400_upmc1		937
#define chud_7400_upmc2		938
#define chud_7400_upmc3		941
#define chud_7400_upmc4		942
#define chud_7400_mmcr2		944
#define chud_7400_bamr		951
#define chud_7400_mmcr0		952
#define chud_7400_pmc1		953
#define chud_7400_pmc2		954
#define chud_7400_siar		955 
#define chud_7400_mmcr1		956
#define chud_7400_pmc3		957
#define chud_7400_pmc4		958
#define chud_7400_sda		959
#define chud_7400_hid0		1008
#define chud_7400_hid1		1009
#define chud_7400_iabr		1010
#define chud_7400_msscr0	1014
#define chud_7410_l2pmcr	1016 /* 7410 only */
#define chud_7400_l2cr		1017
#define chud_7400_ictc		1019
#define chud_7400_thrm1		1020
#define chud_7400_thrm2		1021
#define chud_7400_thrm3		1022
#define chud_7400_pir		1023

/* PPC SPRs - 7450/7455 Specific */
#define chud_7455_sprg4		276 /* 7455 only */
#define chud_7455_sprg5		277 /* 7455 only */
#define chud_7455_sprg6		278 /* 7455 only */
#define chud_7455_sprg7		279 /* 7455 only */
#define chud_7455_ibat4u	560 /* 7455 only */
#define chud_7455_ibat4l	561 /* 7455 only */
#define chud_7455_ibat5u	562 /* 7455 only */
#define chud_7455_ibat5l	563 /* 7455 only */
#define chud_7455_ibat6u	564 /* 7455 only */
#define chud_7455_ibat6l	565 /* 7455 only */
#define chud_7455_ibat7u	566 /* 7455 only */
#define chud_7455_ibat7l	567 /* 7455 only */
#define chud_7455_dbat4u	568 /* 7455 only */
#define chud_7455_dbat4l	569 /* 7455 only */
#define chud_7455_dbat5u	570 /* 7455 only */
#define chud_7455_dbat5l	571 /* 7455 only */
#define chud_7455_dbat6u	572 /* 7455 only */
#define chud_7455_dbat6l	573 /* 7455 only */
#define chud_7455_dbat7u	574 /* 7455 only */
#define chud_7455_dbat7l	575 /* 7455 only */
#define chud_7450_upmc5		929
#define chud_7450_upmc6		930
#define chud_7450_upmc1		937
#define chud_7450_upmc2		938
#define chud_7450_upmc3		941
#define chud_7450_upmc4		942
#define chud_7450_mmcr2		944
#define chud_7450_pmc5		945
#define chud_7450_pmc6		946
#define chud_7450_bamr		951
#define chud_7450_mmcr0		952
#define chud_7450_pmc1		953
#define chud_7450_pmc2		954
#define chud_7450_siar		955 
#define chud_7450_mmcr1		956
#define chud_7450_pmc3		957
#define chud_7450_pmc4		958
#define chud_7450_tlbmiss	980
#define chud_7450_ptehi		981
#define chud_7450_ptelo		982
#define chud_7450_l3pm		983
#define chud_7450_hid0		1008
#define chud_7450_hid1		1009
#define chud_7450_iabr		1010
#define chud_7450_ldstdb	1012
#define chud_7450_msscr0	1014
#define chud_7450_msssr0	1015
#define chud_7450_ldstcr	1016
#define chud_7450_l2cr		1017
#define chud_7450_l3cr		1018
#define chud_7450_ictc		1019
#define chud_7450_ictrl		1011
#define chud_7450_thrm1		1020
#define chud_7450_thrm2		1021
#define chud_7450_thrm3		1022
#define chud_7450_pir		1023

/* PPC SPRs - 970 Specific */
#define chud_970_vrsave		256
#define chud_970_ummcra		770
#define chud_970_upmc1		771
#define chud_970_upmc2		772
#define chud_970_upmc3		773
#define chud_970_upmc4		774
#define chud_970_upmc5		775
#define chud_970_upmc6		776
#define chud_970_upmc7		777
#define chud_970_upmc8		778
#define chud_970_ummcr0		779
#define chud_970_usiar		780
#define chud_970_usdar		781
#define chud_970_ummcr1		782
#define chud_970_uimc		783
#define chud_970_mmcra		786
#define chud_970_pmc1		787
#define chud_970_pmc2		788
#define chud_970_pmc3		789
#define chud_970_pmc4		790
#define chud_970_pmc5		791
#define chud_970_pmc6		792
#define chud_970_pmc7		793
#define chud_970_pmc8		794
#define chud_970_mmcr0		795
#define chud_970_siar		796
#define chud_970_sdar		797
#define chud_970_mmcr1		798
#define chud_970_imc		799

/* PPC SPRs - 7400/7410 Specific */
#define chud_7400_msscr1	1015

/* PPC SPRs - 64-bit implementations */
#define chud_ppc64_accr		29
#define chud_ppc64_ctrl		152

/* PPC SPRs - 970 Specific */
#define chud_970_scomc		276
#define chud_970_scomd		277
#define chud_970_hsprg0		304
#define chud_970_hsprg1		305
#define chud_970_hdec		310
#define chud_970_hior		311
#define chud_970_rmor		312
#define chud_970_hrmor		313
#define chud_970_hsrr0		314
#define chud_970_hsrr1		315
#define chud_970_lpcr		318
#define chud_970_lpidr		319
#define chud_970_trig0		976
#define chud_970_trig1		977
#define chud_970_trig2		978
#define chud_970_hid0		1008
#define chud_970_hid1		1009
#define chud_970_hid4		1012
#define chud_970_hid5		1014
#define chud_970_dabrx		1015
#define chud_970_trace		1022
#define chud_970_pir		1023

#endif // _CHUD_SPR_H_

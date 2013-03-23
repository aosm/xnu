/*
 *  ccdes.h
 *  corecrypto
 *
 *  Created by Fabrice Gautier on 12/20/10.
 *  Copyright 2010 Apple, Inc. All rights reserved.
 *
 */


#ifndef _CORECRYPTO_CCDES_H_
#define _CORECRYPTO_CCDES_H_

#include <corecrypto/ccmode.h>

#define CCDES_BLOCK_SIZE 8
#define CCDES_KEY_SIZE 8

extern const struct ccmode_ecb ccdes_ltc_ecb_decrypt_mode;
extern const struct ccmode_ecb ccdes_ltc_ecb_encrypt_mode;

extern const struct ccmode_ecb ccdes3_ltc_ecb_decrypt_mode;
extern const struct ccmode_ecb ccdes3_ltc_ecb_encrypt_mode;
extern const struct ccmode_ecb ccdes168_ltc_ecb_encrypt_mode;

const struct ccmode_ecb *ccdes_ecb_decrypt_mode(void);
const struct ccmode_ecb *ccdes_ecb_encrypt_mode(void);

const struct ccmode_cbc *ccdes_cbc_decrypt_mode(void);
const struct ccmode_cbc *ccdes_cbc_encrypt_mode(void);

const struct ccmode_cfb *ccdes_cfb_decrypt_mode(void);
const struct ccmode_cfb *ccdes_cfb_encrypt_mode(void);

const struct ccmode_cfb8 *ccdes_cfb8_decrypt_mode(void);
const struct ccmode_cfb8 *ccdes_cfb8_encrypt_mode(void);

const struct ccmode_ctr *ccdes_ctr_crypt_mode(void);

const struct ccmode_ofb *ccdes_ofb_crypt_mode(void);


const struct ccmode_ecb *ccdes3_ecb_decrypt_mode(void);
const struct ccmode_ecb *ccdes3_ecb_encrypt_mode(void);

const struct ccmode_cbc *ccdes3_cbc_decrypt_mode(void);
const struct ccmode_cbc *ccdes3_cbc_encrypt_mode(void);

const struct ccmode_cfb *ccdes3_cfb_decrypt_mode(void);
const struct ccmode_cfb *ccdes3_cfb_encrypt_mode(void);

const struct ccmode_cfb8 *ccdes3_cfb8_decrypt_mode(void);
const struct ccmode_cfb8 *ccdes3_cfb8_encrypt_mode(void);

const struct ccmode_ctr *ccdes3_ctr_crypt_mode(void);

const struct ccmode_ofb *ccdes3_ofb_crypt_mode(void);

int ccdes_key_is_weak( void *key, unsigned long  length);
void ccdes_key_set_odd_parity(void *key, unsigned long length);

uint32_t
ccdes_cbc_cksum(void *in, void *out, unsigned long length,
                void *key, unsigned long keylen, void *ivec);


#endif /* _CORECRYPTO_CCDES_H_ */

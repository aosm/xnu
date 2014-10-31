/*
 *  cchmac.h
 *  corecrypto
 *
 *  Created by Michael Brouwer on 12/7/10.
 *  Copyright 2010,2011 Apple Inc. All rights reserved.
 *
 */

#ifndef _CORECRYPTO_CCHMAC_H_
#define _CORECRYPTO_CCHMAC_H_

#include <corecrypto/cc.h>
#include <corecrypto/ccdigest.h>

/* An hmac_ctx_t is normally allocated as an array of these. */
struct cchmac_ctx {
    uint8_t b[8];
} __attribute__((aligned(8)));

typedef union {
    struct cchmac_ctx *hdr;
    ccdigest_ctx_t digest;
} cchmac_ctx_t __attribute__((transparent_union));

#define cchmac_ctx_size(STATE_SIZE, BLOCK_SIZE)  (ccdigest_ctx_size(STATE_SIZE, BLOCK_SIZE) + (STATE_SIZE))
#define cchmac_di_size(_di_)  (cchmac_ctx_size((_di_)->state_size, (_di_)->block_size))

#define cchmac_ctx_n(STATE_SIZE, BLOCK_SIZE)  ccn_nof_size(cchmac_ctx_size((STATE_SIZE), (BLOCK_SIZE)))

#define cchmac_ctx_decl(STATE_SIZE, BLOCK_SIZE, _name_) cc_ctx_decl(struct cchmac_ctx, cchmac_ctx_size(STATE_SIZE, BLOCK_SIZE), _name_)
#define cchmac_ctx_clear(STATE_SIZE, BLOCK_SIZE, _name_) cc_zero(cchmac_ctx_size(STATE_SIZE, BLOCK_SIZE), _name_)
#define cchmac_di_decl(_di_, _name_) cchmac_ctx_decl((_di_)->state_size, (_di_)->block_size, _name_)
#define cchmac_di_clear(_di_, _name_) cchmac_ctx_clear((_di_)->state_size, (_di_)->block_size, _name_)

/* Return a ccdigest_ctx_t which can be accesed with the macros in ccdigest.h */
#define cchmac_digest_ctx(_di_, HC)    (((cchmac_ctx_t)(HC)).digest)

/* Accesors for ostate fields, this is all cchmac_ctx_t adds to the ccdigest_ctx_t. */
#define cchmac_ostate(_di_, HC)    ((struct ccdigest_state *)(((cchmac_ctx_t)(HC)).hdr->b + ccdigest_di_size(_di_)))
#define cchmac_ostate8(_di_, HC)   (ccdigest_u8(cchmac_ostate(_di_, HC)))
#define cchmac_ostate32(_di_, HC)  (ccdigest_u32(cchmac_ostate(_di_, HC)))
#define cchmac_ostate64(_di_, HC)  (ccdigest_u64(cchmac_ostate(_di_, HC)))
#define cchmac_ostateccn(_di_, HC) (ccdigest_ccn(cchmac_ostate(_di_, HC)))

/* Convenience accessors for ccdigest_ctx_t fields. */
#define cchmac_istate(_di_, HC)    ccdigest_state(_di_, ((cchmac_ctx_t)(HC)).digest)
#define cchmac_istate8(_di_, HC)   ccdigest_u8(cchmac_istate(_di_, HC))
#define cchmac_istate32(_di_, HC)  ccdigest_u32(cchmac_istate(_di_, HC))
#define cchmac_istate64(_di_, HC)  ccdigest_u64(cchmac_istate(_di_, HC))
#define cchmac_istateccn(_di_, HC) ccdigest_ccn(cchmac_istate(_di_, HC))
#define cchmac_data(_di_, HC)      ccdigest_data(_di_, ((cchmac_ctx_t)(HC)).digest)
#define cchmac_num(_di_, HC)       ccdigest_num(_di_, ((cchmac_ctx_t)(HC)).digest)
#define cchmac_nbits(_di_, HC)     ccdigest_nbits(_di_, ((cchmac_ctx_t)(HC)).digest)

void cchmac_init(const struct ccdigest_info *di, cchmac_ctx_t ctx,
                 unsigned long key_len, const void *key);
void cchmac_update(const struct ccdigest_info *di, cchmac_ctx_t ctx,
                   unsigned long data_len, const void *data);
void cchmac_final(const struct ccdigest_info *di, cchmac_ctx_t ctx,
                  unsigned char *mac);

void cchmac(const struct ccdigest_info *di, unsigned long key_len,
            const void *key, unsigned long data_len, const void *data,
            unsigned char *mac);

/* Test functions */

struct cchmac_test_input {
    const struct ccdigest_info *di;
    unsigned long key_len;
    const void *key;
    unsigned long data_len;
    const void *data;
    unsigned long mac_len;
    const void *expected_mac;
};

int cchmac_test(const struct cchmac_test_input *input);
int cchmac_test_chunks(const struct cchmac_test_input *input, unsigned long chunk_size);


#endif /* _CORECRYPTO_CCHMAC_H_ */

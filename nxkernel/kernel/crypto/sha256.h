/*
 * sha256.h - SHA-256 (FIPS 180-4), HMAC-SHA256 (RFC 2104), PBKDF2-HMAC-SHA256 (RFC 8018)
 *
 * 쓰는 곳: 사용자 인증(kernel/security/auth.c)이 roi:/users 의 비밀번호 해시를 검증할 때.
 * 정확성은 자체 점검(selftest)이 표준 테스트 벡터로 확인한다.
 */
#ifndef KERNEL_CRYPTO_SHA256_H
#define KERNEL_CRYPTO_SHA256_H

#include "nyxis.h"

#define SHA256_DIGEST_SIZE 32U
#define SHA256_BLOCK_SIZE  64U

typedef struct {
    u32 state[8];
    u64 total_len;           /* 지금까지 넣은 바이트 수 */
    u8  buf[SHA256_BLOCK_SIZE];
    u32 buf_len;
} sha256_ctx_t;

void sha256_init(sha256_ctx_t *ctx);
void sha256_update(sha256_ctx_t *ctx, const void *data, usize len);
void sha256_final(sha256_ctx_t *ctx, u8 out[SHA256_DIGEST_SIZE]);
void sha256(const void *data, usize len, u8 out[SHA256_DIGEST_SIZE]);

void hmac_sha256(const void *key, usize key_len, const void *msg, usize msg_len, u8 out[SHA256_DIGEST_SIZE]);

/* PBKDF2-HMAC-SHA256. out_len 바이트를 만든다 (iterations >= 1). */
void pbkdf2_hmac_sha256(const void *password, usize password_len,
                        const void *salt, usize salt_len,
                        u32 iterations, u8 *out, usize out_len);

/* 길이 len 의 두 버퍼를 데이터에 무관한 시간으로 비교한다 (같으면 true) */
bool crypto_equal_ct(const void *a, const void *b, usize len);

/* 비밀 데이터를 지운다 (컴파일러가 없애지 못하도록 volatile 로 쓴다) */
void crypto_wipe(void *p, usize len);

#endif /* KERNEL_CRYPTO_SHA256_H */

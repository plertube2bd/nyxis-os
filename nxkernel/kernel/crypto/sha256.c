/*
 * sha256.c - SHA-256 / HMAC-SHA256 / PBKDF2-HMAC-SHA256 (설계는 sha256.h 참고)
 */

#include "kernel/crypto/sha256.h"
#include "memory.h"

static const u32 K[64] = {
    0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U, 0x3956c25bU, 0x59f111f1U, 0x923f82a4U, 0xab1c5ed5U,
    0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U, 0x72be5d74U, 0x80deb1feU, 0x9bdc06a7U, 0xc19bf174U,
    0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU, 0x2de92c6fU, 0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU,
    0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U, 0xc6e00bf3U, 0xd5a79147U, 0x06ca6351U, 0x14292967U,
    0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU, 0x53380d13U, 0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U,
    0xa2bfe8a1U, 0xa81a664bU, 0xc24b8b70U, 0xc76c51a3U, 0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U,
    0x19a4c116U, 0x1e376c08U, 0x2748774cU, 0x34b0bcb5U, 0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU, 0x682e6ff3U,
    0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U, 0x90befffaU, 0xa4506cebU, 0xbef9a3f7U, 0xc67178f2U
};

static u32 rotr(u32 x, u32 n)
{
    return (x >> n) | (x << (32U - n));
}

static void compress(u32 st[8], const u8 *blk)
{
    u32 w[64];
    u32 a, b, c, d, e, f, g, h;
    u32 i;

    for (i = 0; i < 16; i++)
        w[i] = ((u32)blk[i * 4] << 24) | ((u32)blk[i * 4 + 1] << 16) | ((u32)blk[i * 4 + 2] << 8) | (u32)blk[i * 4 + 3];
    for (i = 16; i < 64; i++) {
        u32 s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
        u32 s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);

        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }

    a = st[0]; b = st[1]; c = st[2]; d = st[3];
    e = st[4]; f = st[5]; g = st[6]; h = st[7];

    for (i = 0; i < 64; i++) {
        u32 S1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
        u32 ch = (e & f) ^ (~e & g);
        u32 t1 = h + S1 + ch + K[i] + w[i];
        u32 S0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
        u32 mj = (a & b) ^ (a & c) ^ (b & c);
        u32 t2 = S0 + mj;

        h = g; g = f; f = e; e = d + t1;
        d = c; c = b; b = a; a = t1 + t2;
    }

    st[0] += a; st[1] += b; st[2] += c; st[3] += d;
    st[4] += e; st[5] += f; st[6] += g; st[7] += h;
}

void sha256_init(sha256_ctx_t *ctx)
{
    ctx->state[0] = 0x6a09e667U; ctx->state[1] = 0xbb67ae85U;
    ctx->state[2] = 0x3c6ef372U; ctx->state[3] = 0xa54ff53aU;
    ctx->state[4] = 0x510e527fU; ctx->state[5] = 0x9b05688cU;
    ctx->state[6] = 0x1f83d9abU; ctx->state[7] = 0x5be0cd19U;
    ctx->total_len = 0;
    ctx->buf_len = 0;
}

void sha256_update(sha256_ctx_t *ctx, const void *data, usize len)
{
    const u8 *p = (const u8 *)data;

    ctx->total_len += len;
    while (len > 0) {
        usize take = SHA256_BLOCK_SIZE - ctx->buf_len;

        if (take > len)
            take = len;
        memcpy(ctx->buf + ctx->buf_len, p, take);
        ctx->buf_len += (u32)take;
        p += take;
        len -= take;
        if (ctx->buf_len == SHA256_BLOCK_SIZE) {
            compress(ctx->state, ctx->buf);
            ctx->buf_len = 0;
        }
    }
}

void sha256_final(sha256_ctx_t *ctx, u8 out[SHA256_DIGEST_SIZE])
{
    u64 bits = ctx->total_len * 8U;
    u32 i;

    ctx->buf[ctx->buf_len++] = 0x80;
    if (ctx->buf_len > 56) {
        while (ctx->buf_len < SHA256_BLOCK_SIZE)
            ctx->buf[ctx->buf_len++] = 0;
        compress(ctx->state, ctx->buf);
        ctx->buf_len = 0;
    }
    while (ctx->buf_len < 56)
        ctx->buf[ctx->buf_len++] = 0;
    for (i = 0; i < 8; i++)
        ctx->buf[56 + i] = (u8)(bits >> (56 - 8 * i));
    compress(ctx->state, ctx->buf);

    for (i = 0; i < 8; i++) {
        out[i * 4]     = (u8)(ctx->state[i] >> 24);
        out[i * 4 + 1] = (u8)(ctx->state[i] >> 16);
        out[i * 4 + 2] = (u8)(ctx->state[i] >> 8);
        out[i * 4 + 3] = (u8)(ctx->state[i]);
    }
    crypto_wipe(ctx, sizeof(*ctx));
}

void sha256(const void *data, usize len, u8 out[SHA256_DIGEST_SIZE])
{
    sha256_ctx_t ctx;

    sha256_init(&ctx);
    sha256_update(&ctx, data, len);
    sha256_final(&ctx, out);
}

/* 키 패드를 미리 넣은 내부/외부 상태 (PBKDF2 반복마다 키를 다시 해시하지 않도록) */
typedef struct {
    sha256_ctx_t inner;
    sha256_ctx_t outer;
} hmac_keyed_t;

static void hmac_prepare(hmac_keyed_t *k, const void *key, usize key_len)
{
    u8 kb[SHA256_BLOCK_SIZE];
    u8 pad[SHA256_BLOCK_SIZE];
    u32 i;

    memset(kb, 0, sizeof(kb));
    if (key_len > SHA256_BLOCK_SIZE)
        sha256(key, key_len, kb);
    else
        memcpy(kb, key, key_len);

    for (i = 0; i < SHA256_BLOCK_SIZE; i++)
        pad[i] = (u8)(kb[i] ^ 0x36U);
    sha256_init(&k->inner);
    sha256_update(&k->inner, pad, sizeof(pad));

    for (i = 0; i < SHA256_BLOCK_SIZE; i++)
        pad[i] = (u8)(kb[i] ^ 0x5cU);
    sha256_init(&k->outer);
    sha256_update(&k->outer, pad, sizeof(pad));

    crypto_wipe(kb, sizeof(kb));
    crypto_wipe(pad, sizeof(pad));
}

static void hmac_run(const hmac_keyed_t *k, const void *msg, usize msg_len, u8 out[SHA256_DIGEST_SIZE])
{
    sha256_ctx_t c;
    u8 ih[SHA256_DIGEST_SIZE];

    c = k->inner;
    sha256_update(&c, msg, msg_len);
    sha256_final(&c, ih);
    c = k->outer;
    sha256_update(&c, ih, sizeof(ih));
    sha256_final(&c, out);
    crypto_wipe(ih, sizeof(ih));
}

void hmac_sha256(const void *key, usize key_len, const void *msg, usize msg_len, u8 out[SHA256_DIGEST_SIZE])
{
    hmac_keyed_t k;

    hmac_prepare(&k, key, key_len);
    hmac_run(&k, msg, msg_len, out);
    crypto_wipe(&k, sizeof(k));
}

void pbkdf2_hmac_sha256(const void *password, usize password_len,
                        const void *salt, usize salt_len,
                        u32 iterations, u8 *out, usize out_len)
{
    hmac_keyed_t k;
    sha256_ctx_t c;
    u8 u[SHA256_DIGEST_SIZE];
    u8 t[SHA256_DIGEST_SIZE];
    u8 ctr[4];
    u32 block = 1;
    u32 it;
    u32 i;

    hmac_prepare(&k, password, password_len);

    while (out_len > 0) {
        usize take = out_len < SHA256_DIGEST_SIZE ? out_len : SHA256_DIGEST_SIZE;

        ctr[0] = (u8)(block >> 24);
        ctr[1] = (u8)(block >> 16);
        ctr[2] = (u8)(block >> 8);
        ctr[3] = (u8)block;

        /* U1 = HMAC(P, S || INT(block)) */
        c = k.inner;
        sha256_update(&c, salt, salt_len);
        sha256_update(&c, ctr, sizeof(ctr));
        sha256_final(&c, u);
        c = k.outer;
        sha256_update(&c, u, sizeof(u));
        sha256_final(&c, u);
        memcpy(t, u, sizeof(t));

        for (it = 1; it < iterations; it++) {
            hmac_run(&k, u, sizeof(u), u);
            for (i = 0; i < SHA256_DIGEST_SIZE; i++)
                t[i] ^= u[i];
        }

        memcpy(out, t, take);
        out += take;
        out_len -= take;
        block++;
    }

    crypto_wipe(&k, sizeof(k));
    crypto_wipe(u, sizeof(u));
    crypto_wipe(t, sizeof(t));
}

bool crypto_equal_ct(const void *a, const void *b, usize len)
{
    const u8 *x = (const u8 *)a;
    const u8 *y = (const u8 *)b;
    u8 diff = 0;
    usize i;

    for (i = 0; i < len; i++)
        diff |= (u8)(x[i] ^ y[i]);
    return diff == 0 ? true : false;
}

void crypto_wipe(void *p, usize len)
{
    volatile u8 *v = (volatile u8 *)p;

    while (len--)
        *v++ = 0;
}

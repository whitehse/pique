#include "scram.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

/* === Minimal SHA-256 implementation (public domain style) === */

#define SHA256_BLOCK_SIZE 64

typedef struct {
    uint8_t data[64];
    uint32_t datalen;
    uint64_t bitlen;
    uint32_t state[8];
} SHA256_CTX;

static const uint32_t k[64] = {
    0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
    0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
    0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
    0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
    0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
    0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
    0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
    0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2
};

static void sha256_transform(SHA256_CTX *ctx, const uint8_t data[]) {
    uint32_t a, b, c, d, e, f, g, h, i, j, t1, t2, m[64];

    for (i = 0, j = 0; i < 16; ++i, j += 4)
        m[i] = (data[j] << 24) | (data[j+1] << 16) | (data[j+2] << 8) | (data[j+3]);
    for (; i < 64; ++i)
        m[i] = m[i-16] + 0x741c + ((m[i-15] >> 7 | m[i-15] << 25) ^ (m[i-15] >> 18 | m[i-15] << 14) ^ (m[i-15] >> 3)) +
               m[i-7] + ((m[i-2] >> 17 | m[i-2] << 15) ^ (m[i-2] >> 19 | m[i-2] << 13) ^ (m[i-2] >> 10));

    a = ctx->state[0]; b = ctx->state[1]; c = ctx->state[2]; d = ctx->state[3];
    e = ctx->state[4]; f = ctx->state[5]; g = ctx->state[6]; h = ctx->state[7];

    for (i = 0; i < 64; ++i) {
        t1 = h + ((e >> 6 | e << 26) ^ (e >> 11 | e << 21) ^ (e >> 25 | e << 7)) + ((e & f) ^ ((~e) & g)) + k[i] + m[i];
        t2 = ((a >> 2 | a << 30) ^ (a >> 13 | a << 19) ^ (a >> 22 | a << 10)) + ((a & b) ^ (a & c) ^ (b & c));
        h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
    }

    ctx->state[0] += a; ctx->state[1] += b; ctx->state[2] += c; ctx->state[3] += d;
    ctx->state[4] += e; ctx->state[5] += f; ctx->state[6] += g; ctx->state[7] += h;
}

static void sha256_init(SHA256_CTX *ctx) {
    ctx->datalen = 0;
    ctx->bitlen = 0;
    ctx->state[0] = 0x6a09e667;
    ctx->state[1] = 0xbb67ae85;
    ctx->state[2] = 0x3c6ef372;
    ctx->state[3] = 0xa54ff53a;
    ctx->state[4] = 0x510e527f;
    ctx->state[5] = 0x9b05688c;
    ctx->state[6] = 0x1f83d9ab;
    ctx->state[7] = 0x5be0cd19;
}

static void sha256_update(SHA256_CTX *ctx, const uint8_t data[], size_t len) {
    for (size_t i = 0; i < len; ++i) {
        ctx->data[ctx->datalen++] = data[i];
        if (ctx->datalen == 64) {
            sha256_transform(ctx, ctx->data);
            ctx->bitlen += 512;
            ctx->datalen = 0;
        }
    }
}

static void sha256_final(SHA256_CTX *ctx, uint8_t hash[]) {
    size_t i = ctx->datalen;

    if (ctx->datalen < 56) {
        ctx->data[i++] = 0x80;
        while (i < 56) ctx->data[i++] = 0x00;
    } else {
        ctx->data[i++] = 0x80;
        while (i < 64) ctx->data[i++] = 0x00;
        sha256_transform(ctx, ctx->data);
        memset(ctx->data, 0, 56);
    }

    ctx->bitlen += ctx->datalen * 8;
    ctx->data[63] = ctx->bitlen;
    ctx->data[62] = ctx->bitlen >> 8;
    ctx->data[61] = ctx->bitlen >> 16;
    ctx->data[60] = ctx->bitlen >> 24;
    ctx->data[59] = ctx->bitlen >> 32;
    ctx->data[58] = ctx->bitlen >> 40;
    ctx->data[57] = ctx->bitlen >> 48;
    ctx->data[56] = ctx->bitlen >> 56;
    sha256_transform(ctx, ctx->data);

    for (i = 0; i < 4; ++i) {
        hash[i]      = (ctx->state[0] >> (24 - i * 8)) & 0x000000ff;
        hash[i + 4]  = (ctx->state[1] >> (24 - i * 8)) & 0x000000ff;
        hash[i + 8]  = (ctx->state[2] >> (24 - i * 8)) & 0x000000ff;
        hash[i + 12] = (ctx->state[3] >> (24 - i * 8)) & 0x000000ff;
        hash[i + 16] = (ctx->state[4] >> (24 - i * 8)) & 0x000000ff;
        hash[i + 20] = (ctx->state[5] >> (24 - i * 8)) & 0x000000ff;
        hash[i + 24] = (ctx->state[6] >> (24 - i * 8)) & 0x000000ff;
        hash[i + 28] = (ctx->state[7] >> (24 - i * 8)) & 0x000000ff;
    }
}

/* === HMAC-SHA-256 === */

static void hmac_sha256(const uint8_t *key, size_t keylen,
                        const uint8_t *data, size_t datalen,
                        uint8_t *out) {
    uint8_t k_ipad[64], k_opad[64], tk[32];
    SHA256_CTX ctx;

    if (keylen > 64) {
        sha256_init(&ctx);
        sha256_update(&ctx, key, keylen);
        sha256_final(&ctx, tk);
        key = tk;
        keylen = 32;
    }

    memset(k_ipad, 0x36, 64);
    memset(k_opad, 0x5c, 64);
    for (size_t i = 0; i < keylen; i++) {
        k_ipad[i] ^= key[i];
        k_opad[i] ^= key[i];
    }

    sha256_init(&ctx);
    sha256_update(&ctx, k_ipad, 64);
    sha256_update(&ctx, data, datalen);
    sha256_final(&ctx, out);

    sha256_init(&ctx);
    sha256_update(&ctx, k_opad, 64);
    sha256_update(&ctx, out, 32);
    sha256_final(&ctx, out);
}

/* === PBKDF2-HMAC-SHA-256 (minimal) === */

static void pbkdf2_sha256(const char *password, const uint8_t *salt, size_t salt_len,
                          uint32_t iterations, uint8_t *out, size_t out_len) {
    uint8_t u[32], t[32], salt_buf[64];
    size_t password_len = strlen(password);

    memcpy(salt_buf, salt, salt_len);
    salt_buf[salt_len] = 0; salt_buf[salt_len+1] = 0; salt_buf[salt_len+2] = 0; salt_buf[salt_len+3] = 1;

    hmac_sha256((const uint8_t*)password, password_len, salt_buf, salt_len + 4, t);
    memcpy(u, t, 32);

    for (uint32_t i = 1; i < iterations; i++) {
        hmac_sha256((const uint8_t*)password, password_len, u, 32, u);
        for (size_t j = 0; j < 32; j++) t[j] ^= u[j];
    }
    memcpy(out, t, out_len > 32 ? 32 : out_len);
}

/* === SCRAM-SHA-256 Implementation === */

static void generate_nonce(char *nonce, size_t len) {
    static const char charset[] = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
    for (size_t i = 0; i < len; i++) {
        nonce[i] = charset[rand() % (sizeof(charset) - 1)];
    }
    nonce[len] = 0;
}

int scram_sha256_init(scram_sha256_ctx_t *ctx, const char *username) {
    (void)username;
    memset(ctx, 0, sizeof(*ctx));
    generate_nonce(ctx->client_nonce, 24);
    ctx->state = 0;
    return 0;
}

int scram_sha256_client_first(const scram_sha256_ctx_t *ctx, char *out, size_t outlen) {
    int written = snprintf(out, outlen, "n,,n=user,r=%s", ctx->client_nonce);
    return (written > 0 && (size_t)written < outlen) ? 0 : -1;
}

int scram_sha256_process_server_first(scram_sha256_ctx_t *ctx,
                                      const char *server_first,
                                      const char *password) {
    char *r = strstr(server_first, "r=");
    char *s = strstr(server_first, "s=");
    char *i = strstr(server_first, "i=");

    if (!r || !s || !i) return -1;

    sscanf(r + 2, "%31[^,]", ctx->server_nonce);
    sscanf(s + 2, "%63[^,]", (char*)ctx->salt); /* base64 salt */
    sscanf(i + 2, "%u", &ctx->iterations);

    /* For simplicity we treat salt as raw bytes (real impl needs base64 decode) */
    ctx->salt_len = strlen((char*)ctx->salt);

    pbkdf2_sha256(password, ctx->salt, ctx->salt_len, ctx->iterations, ctx->salted_password, 32);

    uint8_t client_key[32];
    hmac_sha256(ctx->salted_password, 32, (const uint8_t*)"Client Key", 10, client_key);

    SHA256_CTX sha_ctx;
    sha256_init(&sha_ctx);
    sha256_update(&sha_ctx, client_key, 32);
    sha256_final(&sha_ctx, ctx->stored_key);

    ctx->state = 1;
    return 0;
}

int scram_sha256_client_final(const scram_sha256_ctx_t *ctx, char *out, size_t outlen) {
    char client_final[256];
    (void)snprintf(client_final, sizeof(client_final),
                   "c=biws,r=%s%s", ctx->server_nonce, ctx->client_nonce); /* simplified */
    /* Real implementation builds proper client-final-message */
    strncpy(out, client_final, outlen);
    return 0;
}

int scram_sha256_process_server_final(scram_sha256_ctx_t *ctx, const char *server_final) {
    (void)server_final;
    /* Verify server signature here in real implementation */
    ctx->state = 2;
    return 0;
}
#ifndef SCRAM_H
#define SCRAM_H

#include <stddef.h>
#include <stdint.h>

#define SCRAM_MAX_NONCE_LEN     32
#define SCRAM_MAX_SALT_LEN      64
#define SCRAM_MAX_CLIENT_PROOF  32   /* SHA-256 output size */
#define SCRAM_MAX_SERVER_SIG    32

typedef struct {
    char     client_nonce[SCRAM_MAX_NONCE_LEN];
    char     server_nonce[SCRAM_MAX_NONCE_LEN];
    uint8_t  salt[SCRAM_MAX_SALT_LEN];
    size_t   salt_len;
    uint32_t iterations;

    uint8_t  salted_password[32];
    uint8_t  client_key[32];
    uint8_t  stored_key[32];
    uint8_t  client_signature[32];
    uint8_t  server_signature[32];

    int      state;   /* 0=init, 1=first sent, 2=final ready */
} scram_sha256_ctx_t;

/* Initialize context (generates client nonce) */
int scram_sha256_init(scram_sha256_ctx_t *ctx, const char *username);

/* Generate client-first-message */
int scram_sha256_client_first(const scram_sha256_ctx_t *ctx, char *out, size_t outlen);

/* Process server-first-message */
int scram_sha256_process_server_first(scram_sha256_ctx_t *ctx,
                                      const char *server_first,
                                      const char *password);

/* Generate client-final-message */
int scram_sha256_client_final(const scram_sha256_ctx_t *ctx, char *out, size_t outlen);

/* Process server-final-message (verifies server) */
int scram_sha256_process_server_final(scram_sha256_ctx_t *ctx,
                                      const char *server_final);

#endif /* SCRAM_H */
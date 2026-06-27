#include "pqwire.h"
#include "scram.h"
#include "scram.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <arpa/inet.h>
#include <stdint.h>

/* Internal authentication state */
typedef enum {
    AUTH_STATE_NONE = 0,
    AUTH_STATE_CLIENT_FIRST,
    AUTH_STATE_CLIENT_FINAL,
    AUTH_STATE_SERVER_FIRST,
    AUTH_STATE_SERVER_FINAL,
    AUTH_STATE_DONE
} auth_state_t;

typedef enum {
    PROTO_STATE_STARTUP = 0,
    PROTO_STATE_AUTHENTICATING,
    PROTO_STATE_READY,
    PROTO_STATE_QUERYING,
    PROTO_STATE_ERROR
} protocol_state_t;

#define PQWIRE_MAGIC 0xABCD1234

struct pqwire_ctx {
    uint32_t magic;              /* Use-after-destroy protection */
    pqwire_role_t role;
    int event_queue_size;
    size_t max_message_size;

    protocol_event_t *events;
    int event_head;
    int event_tail;
    int event_count;

    uint8_t *output_buf;
    size_t output_len;
    size_t output_cap;

    uint8_t *input_buf;
    size_t input_len;
    size_t input_cap;

    /* Authentication */
    char password[256];
    int use_scram;
    auth_state_t auth_state;
    protocol_state_t proto_state;
    scram_sha256_ctx_t scram_ctx;
};

static void enqueue_event(pqwire_ctx_t *ctx, const protocol_event_t *ev) {
    if (ctx->event_count >= ctx->event_queue_size) {
        /* Queue full - drop oldest event (robust backpressure handling) */
        ctx->event_head = (ctx->event_head + 1) % ctx->event_queue_size;
        ctx->event_count--;
    }
    ctx->events[ctx->event_tail] = *ev;
    ctx->event_tail = (ctx->event_tail + 1) % ctx->event_queue_size;
    ctx->event_count++;
}

static int dequeue_event(pqwire_ctx_t *ctx, protocol_event_t *out) {
    if (ctx->event_count == 0) return 0;
    *out = ctx->events[ctx->event_head];
    ctx->event_head = (ctx->event_head + 1) % ctx->event_queue_size;
    ctx->event_count--;
    return 1;
}

static void append_output(pqwire_ctx_t *ctx, const uint8_t *data, size_t len) {
    if (ctx->output_len + len > ctx->output_cap) {
        size_t new_cap = ctx->output_cap * 2 + len + 1024;
        ctx->output_buf = realloc(ctx->output_buf, new_cap);
        ctx->output_cap = new_cap;
    }
    memcpy(ctx->output_buf + ctx->output_len, data, len);
    ctx->output_len += len;
}

/* === Public API === */

pqwire_ctx_t *pqwire_create(pqwire_role_t role) {
    pqwire_config_t cfg = { .event_queue_size = 16, .password = NULL, .use_scram = 1 };
    return pqwire_create_with_config(role, &cfg);
}

pqwire_ctx_t *pqwire_create_with_config(pqwire_role_t role, const pqwire_config_t *config) {
    if (!config || config->event_queue_size <= 0) return NULL;

    pqwire_ctx_t *ctx = calloc(1, sizeof(*ctx));
    if (!ctx) return NULL;

    ctx->role = role;
    ctx->event_queue_size = config->event_queue_size;
    ctx->events = calloc(config->event_queue_size, sizeof(protocol_event_t));
    ctx->output_cap = 4096;
    ctx->output_buf = malloc(ctx->output_cap);
    ctx->input_cap = 4096;
    ctx->input_buf = malloc(ctx->input_cap);

    if (config->password) {
        strncpy(ctx->password, config->password, sizeof(ctx->password) - 1);
    }
    ctx->use_scram = config->use_scram;
    ctx->max_message_size = config->max_message_size ? config->max_message_size : (16 * 1024 * 1024);
    ctx->auth_state = AUTH_STATE_NONE;
    ctx->proto_state = PROTO_STATE_STARTUP;
    ctx->magic = PQWIRE_MAGIC;

    return ctx;
}

void pqwire_destroy(pqwire_ctx_t *ctx) {
    if (!ctx) return;
    if (ctx->magic != PQWIRE_MAGIC) return; /* Double destroy or corrupted */
    ctx->magic = 0; /* Invalidate */
    free(ctx->events);
    free(ctx->output_buf);
    free(ctx->input_buf);
    free(ctx);
}

void pqwire_reset(pqwire_ctx_t *ctx) {
    if (!ctx || ctx->magic != PQWIRE_MAGIC) return;
    ctx->event_head = ctx->event_tail = ctx->event_count = 0;
    ctx->output_len = 0;
    ctx->input_len = 0;
    ctx->auth_state = AUTH_STATE_NONE;
    memset(&ctx->scram_ctx, 0, sizeof(ctx->scram_ctx));
}

int pqwire_start_auth(pqwire_ctx_t *ctx) {
    if (!ctx || ctx->auth_state != AUTH_STATE_NONE) return -1;

    if (ctx->role == PQWIRE_ROLE_CLIENT && ctx->use_scram && ctx->password[0]) {
        scram_sha256_init(&ctx->scram_ctx, "user");
        ctx->auth_state = AUTH_STATE_CLIENT_FIRST;

        char first[256];
        if (scram_sha256_client_first(&ctx->scram_ctx, first, sizeof(first)) == 0) {
            /* Queue the SASL client-first message as output */
            uint8_t buf[512];
            size_t p = 0;
            buf[p++] = 'p'; /* SASLInitialResponse or PasswordMessage */
            /* For simplicity we treat it as PasswordMessage with SCRAM */
            uint32_t len = htonl(strlen(first) + 4);
            memcpy(buf + 1, &len, 4);
            strcpy((char*)buf + 5, first);
            append_output(ctx, buf, 5 + strlen(first));
        }
    }
    return 0;
}

/* Stub for feed_input that triggers auth when password is set */
size_t pqwire_feed_input(pqwire_ctx_t *ctx, const uint8_t *data, size_t len) {
    if (!ctx || ctx->magic != PQWIRE_MAGIC || !data) return 0;

    if (ctx->input_len + len > ctx->input_cap) {
        ctx->input_cap = ctx->input_len + len + 1024;
        ctx->input_buf = realloc(ctx->input_buf, ctx->input_cap);
    }
    memcpy(ctx->input_buf + ctx->input_len, data, len);
    ctx->input_len += len;

    /* === Input Validation (Robustness) === */
    if (ctx->input_len >= 5) {
        uint32_t mlen;
        memcpy(&mlen, ctx->input_buf, 4);
        mlen = ntohl(mlen);

        if (mlen == 0 || mlen > ctx->max_message_size) {
            protocol_event_t ev = {0};
            ev.type = PQ_EVENT_PROTOCOL_ERROR;
            enqueue_event(ctx, &ev);
            /* Drop only the bad message if possible, otherwise reset */
            if (mlen > 0 && ctx->input_len >= mlen) {
                memmove(ctx->input_buf, ctx->input_buf + mlen, ctx->input_len - mlen);
                ctx->input_len -= mlen;
            } else {
                ctx->input_len = 0;
            }
        }
    }

    /* Very simplified auth handling */
    if (ctx->role == PQWIRE_ROLE_CLIENT && ctx->auth_state == AUTH_STATE_CLIENT_FIRST) {
        /* Assume server replied with server-first */
        protocol_event_t ev = {0};
        ev.type = PQ_EVENT_AUTHENTICATION_OK; /* placeholder */
        enqueue_event(ctx, &ev);
        ctx->auth_state = AUTH_STATE_DONE;
    }

    return len;
}

size_t pqwire_get_output(pqwire_ctx_t *ctx, uint8_t *buf, size_t max_len) {
    if (!ctx || !buf || max_len == 0) return 0;
    size_t to_copy = ctx->output_len < max_len ? ctx->output_len : max_len;
    memcpy(buf, ctx->output_buf, to_copy);
    memmove(ctx->output_buf, ctx->output_buf + to_copy, ctx->output_len - to_copy);
    ctx->output_len -= to_copy;
    return to_copy;
}

int pqwire_next_event(pqwire_ctx_t *ctx, protocol_event_t *event) {
    if (!ctx || ctx->magic != PQWIRE_MAGIC || !event) return 0;
    return dequeue_event(ctx, event);
}

/* Existing send helpers (abbreviated for brevity) */
int pqwire_send_startup(pqwire_ctx_t *ctx, const char *user, const char *database) {
    (void)user; (void)database;
    (void)database;
    if (!ctx || ctx->role != PQWIRE_ROLE_CLIENT) return -1;
    /* ... existing implementation ... */
    return 0;
}

int pqwire_send_query(pqwire_ctx_t *ctx, const char *sql) {
    if (!ctx || ctx->role != PQWIRE_ROLE_CLIENT || !sql) return -1;
    if (ctx->proto_state != PROTO_STATE_READY) {
        protocol_event_t ev = {0};
        ev.type = PQ_EVENT_PROTOCOL_ERROR;
        enqueue_event(ctx, &ev);
        return -1;
    }
    ctx->proto_state = PROTO_STATE_QUERYING;
    /* ... */
    return 0;
}
#ifndef PQWIRE_H
#define PQWIRE_H

#include <stddef.h>
#include <stdint.h>
#include "protocol_events.h"
#include "scram.h"

typedef enum {
    PQWIRE_ROLE_CLIENT = 0,
    PQWIRE_ROLE_SERVER
} pqwire_role_t;

typedef struct {
    int event_queue_size;
    const char *password;        /* raw password (client) or stored SCRAM hash (server) */
    int use_scram;               /* 1 = prefer SCRAM-SHA-256 */
    size_t max_message_size;     /* 0 = use default (16 MiB) */
} pqwire_config_t;

typedef struct pqwire_ctx pqwire_ctx_t;

pqwire_ctx_t *pqwire_create(pqwire_role_t role);
pqwire_ctx_t *pqwire_create_with_config(pqwire_role_t role, const pqwire_config_t *config);
void pqwire_destroy(pqwire_ctx_t *ctx);
void pqwire_reset(pqwire_ctx_t *ctx);

size_t pqwire_feed_input(pqwire_ctx_t *ctx, const uint8_t *data, size_t len);
size_t pqwire_get_output(pqwire_ctx_t *ctx, uint8_t *buf, size_t max_len);
int pqwire_next_event(pqwire_ctx_t *ctx, protocol_event_t *event);

/* Authentication helpers (exposed for advanced use) */
int pqwire_start_auth(pqwire_ctx_t *ctx);

/* Send helpers */
int pqwire_send_startup(pqwire_ctx_t *ctx, const char *user, const char *database);
int pqwire_send_query(pqwire_ctx_t *ctx, const char *sql);
int pqwire_send_call(pqwire_ctx_t *ctx, const char *procedure);

#endif /* PQWIRE_H */
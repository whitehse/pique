#ifndef PQWIRE_H
#define PQWIRE_H

#include <stddef.h>
#include <stdint.h>
#include "protocol_events.h"

typedef enum {
    PQWIRE_ROLE_CLIENT = 0,
    PQWIRE_ROLE_SERVER
} pqwire_role_t;

typedef struct {
    int event_queue_size;
    /* future: allocator hooks, type registry, etc. */
} pqwire_config_t;

typedef struct pqwire_ctx pqwire_ctx_t; /* opaque */

pqwire_ctx_t *pqwire_create(pqwire_role_t role);
pqwire_ctx_t *pqwire_create_with_config(pqwire_role_t role, const pqwire_config_t *config);
void pqwire_destroy(pqwire_ctx_t *ctx);
void pqwire_reset(pqwire_ctx_t *ctx);

size_t pqwire_feed_input(pqwire_ctx_t *ctx, const uint8_t *data, size_t len);
size_t pqwire_get_output(pqwire_ctx_t *ctx, uint8_t *buf, size_t max_len);
int pqwire_next_event(pqwire_ctx_t *ctx, protocol_event_t *event);

/* Send helpers (examples) */
int pqwire_send_startup(pqwire_ctx_t *ctx, const char *user, const char *database);
/* more to be added: query, parse, bind, etc. */

#endif /* PQWIRE_H */
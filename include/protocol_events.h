#ifndef PROTOCOL_EVENTS_H
#define PROTOCOL_EVENTS_H

#include <stddef.h>
#include <stdint.h>

typedef enum {
    PROTOCOL_EVENT_NONE = 0,
    /* PostgreSQL wire events to be defined */
    PQ_EVENT_STARTUP,
    PQ_EVENT_AUTHENTICATION,
    PQ_EVENT_ROW_DESCRIPTION,
    PQ_EVENT_DATA_ROW,
    PQ_EVENT_COMMAND_COMPLETE,
    PQ_EVENT_ERROR,
    /* ... */
    PQ_EVENT_MAX
} protocol_event_type_t;

typedef struct {
    protocol_event_type_t type;
    union {
        struct {
            const uint8_t *data;
            size_t len;
            /* additional fields for row metadata, OIDs, binary flag, etc. */
        } row;
        /* other event payloads */
    } payload;
} protocol_event_t;

#endif /* PROTOCOL_EVENTS_H */

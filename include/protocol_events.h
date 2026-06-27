#ifndef PROTOCOL_EVENTS_H
#define PROTOCOL_EVENTS_H

#include <stddef.h>
#include <stdint.h>

typedef enum {
    PROTOCOL_EVENT_NONE = 0,

    /* Startup / Authentication */
    PQ_EVENT_STARTUP,           /* Client -> Server */
    PQ_EVENT_AUTHENTICATION_OK, /* Server -> Client */
    PQ_EVENT_READY_FOR_QUERY,   /* Server -> Client */

    /* Query protocol */
    PQ_EVENT_QUERY,             /* Client -> Server (simple query) */
    PQ_EVENT_ROW_DESCRIPTION,   /* Server -> Client */
    PQ_EVENT_DATA_ROW,          /* Server -> Client (text or binary) */
    PQ_EVENT_COMMAND_COMPLETE,  /* Server -> Client */
    PQ_EVENT_ERROR,             /* Either direction */
    PQ_EVENT_PROTOCOL_ERROR,    /* Protocol violation detected */

    PQ_EVENT_MAX
} protocol_event_type_t;

/* Column metadata for RowDescription */
typedef struct {
    const char *name;
    uint32_t oid;           /* PostgreSQL type OID */
    int16_t format;         /* 0 = text, 1 = binary */
} pq_column_desc_t;

/* DataRow payload (binary or text) — supports multiple columns */
#define PQWIRE_MAX_ROW_COLUMNS 16

typedef struct {
    const uint8_t *data[PQWIRE_MAX_ROW_COLUMNS];
    size_t         len[PQWIRE_MAX_ROW_COLUMNS];
    int16_t        format[PQWIRE_MAX_ROW_COLUMNS];
    uint32_t       oid[PQWIRE_MAX_ROW_COLUMNS];
    size_t         n_columns;
} pq_data_row_t;

typedef struct {
    protocol_event_type_t type;
    union {
        struct {
            const char *user;
            const char *database;
            /* parameters would go here in full impl */
        } startup;

        struct {
            const char *sql;
        } query;

        struct {
            const pq_column_desc_t *columns;
            size_t column_count;
        } row_desc;

        pq_data_row_t data_row;

        struct {
            const char *tag; /* e.g. "SELECT 1" or "INSERT 0 1" */
        } command_complete;

        struct {
            const char *message;
        } error;
    } payload;
} protocol_event_t;

#endif /* PROTOCOL_EVENTS_H */
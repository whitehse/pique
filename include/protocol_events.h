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
    PQ_EVENT_ERROR_RESPONSE,    /* Server ErrorResponse */
    PQ_EVENT_NOTICE_RESPONSE,   /* Server NoticeResponse */

    /* Extended Query Protocol — frontend (client → server) */
    PQ_EVENT_PARSE,             /* Parse ('P') */
    PQ_EVENT_BIND,              /* Bind ('B') */
    PQ_EVENT_DESCRIBE,          /* Describe ('D') */
    PQ_EVENT_EXECUTE,           /* Execute ('E') */
    PQ_EVENT_CLOSE,             /* Close ('C') */
    PQ_EVENT_SYNC,              /* Sync ('S') */
    PQ_EVENT_FLUSH,             /* Flush ('H') */
    PQ_EVENT_TERMINATE,         /* Terminate ('X') */

    /* Extended Query Protocol — backend responses */
    PQ_EVENT_PARSE_COMPLETE,
    PQ_EVENT_BIND_COMPLETE,
    PQ_EVENT_NO_DATA,
    PQ_EVENT_PORTAL_SUSPENDED,
    PQ_EVENT_CLOSE_COMPLETE,
    PQ_EVENT_PARAMETER_STATUS,   /* Server ParameterStatus */
    PQ_EVENT_NOTIFICATION,       /* NotificationResponse (LISTEN/NOTIFY) */

    /* COPY protocol */
    PQ_EVENT_COPY_IN_RESPONSE,
    PQ_EVENT_COPY_OUT_RESPONSE,
    PQ_EVENT_COPY_BOTH_RESPONSE,
    PQ_EVENT_COPY_DATA,
    PQ_EVENT_COPY_DONE,
    PQ_EVENT_COPY_FAIL,

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

/* Rich Error / Notice structure */
typedef struct {
    char severity[32];      /* 'S' field */
    char code[8];           /* 'C' field (SQLSTATE) */
    char message[256];      /* 'M' field */
    char detail[256];       /* 'D' field */
    char hint[256];         /* 'H' field */
    int  position;          /* 'P' field */
} pq_error_t;

/* NotificationResponse payload */
typedef struct {
    int32_t pid;
    char    channel[64];
    char    payload[256];
} pq_notification_t;

/* Limits for extended-query event payloads (caller may retain copies) */
#define PQWIRE_MAX_STMT_NAME   64
#define PQWIRE_MAX_PORTAL_NAME 64
#define PQWIRE_MAX_QUERY_LEN   4096
#define PQWIRE_MAX_BIND_PARAMS 32
#define PQWIRE_MAX_PARAM_TYPES 16

/** Parse ('P') payload — statement name, query string, param type OIDs. */
typedef struct {
    char     statement[PQWIRE_MAX_STMT_NAME];
    char     query[PQWIRE_MAX_QUERY_LEN];
    uint16_t n_param_types;
    uint32_t param_type_oids[PQWIRE_MAX_PARAM_TYPES];
} pq_parse_t;

/** Single Bind parameter view (points into input buffer lifetime of event). */
typedef struct {
    int32_t        length;   /* -1 = NULL */
    const uint8_t *data;     /* NULL if length < 0 */
    int16_t        format;   /* 0 = text, 1 = binary (from formats array) */
} pq_bind_param_view_t;

/**
 * Bind ('B') payload — portal/statement names and parameter matrix.
 * Parameter data pointers are valid only until the next feed_input that
 * overwrites the context input buffer; copy immediately if caching.
 */
typedef struct {
    char     portal[PQWIRE_MAX_PORTAL_NAME];
    char     statement[PQWIRE_MAX_STMT_NAME];
    uint16_t n_formats;
    int16_t  formats[PQWIRE_MAX_BIND_PARAMS];
    uint16_t n_params;
    pq_bind_param_view_t params[PQWIRE_MAX_BIND_PARAMS];
    uint16_t n_result_formats;
    int16_t  result_formats[PQWIRE_MAX_BIND_PARAMS];
    /* Raw Bind body after names (for zero-copy rewrite / forward). */
    const uint8_t *raw_body;
    size_t         raw_body_len;
} pq_bind_t;

/** Describe ('D') — type 'S' statement or 'P' portal. */
typedef struct {
    char target;  /* 'S' or 'P' */
    char name[PQWIRE_MAX_STMT_NAME];
} pq_describe_t;

/** Execute ('E') */
typedef struct {
    char     portal[PQWIRE_MAX_PORTAL_NAME];
    uint32_t max_rows;
} pq_execute_t;

/** Close ('C') */
typedef struct {
    char target;  /* 'S' or 'P' */
    char name[PQWIRE_MAX_STMT_NAME];
} pq_close_t;

typedef struct {
    protocol_event_type_t type;
    union {
        struct {
            const char *user;
            const char *database;
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
            const char *tag;
        } command_complete;

        pq_error_t error;
        pq_notification_t notification;

        pq_parse_t    parse;
        pq_bind_t     bind;
        pq_describe_t describe;
        pq_execute_t  execute;
        pq_close_t    close_msg;

        /* Generic payload for CopyData, ParameterStatus, raw frames, etc. */
        struct {
            const uint8_t *data;
            size_t len;
        } raw;
    } payload;
} protocol_event_t;

#endif /* PROTOCOL_EVENTS_H */

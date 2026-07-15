#ifndef PQWIRE_H
#define PQWIRE_H

#include <stddef.h>
#include <stdint.h>
#include "protocol_events.h"
#include "scram.h"

#ifdef __cplusplus
extern "C" {
#endif

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

/* ── lifecycle ───────────────────────────────────────────────────────── */

pqwire_ctx_t *pqwire_create(pqwire_role_t role);
pqwire_ctx_t *pqwire_create_with_config(pqwire_role_t role, const pqwire_config_t *config);
void pqwire_destroy(pqwire_ctx_t *ctx);
void pqwire_reset(pqwire_ctx_t *ctx);

/* ── I/O ─────────────────────────────────────────────────────────────── */

size_t pqwire_feed_input(pqwire_ctx_t *ctx, const uint8_t *data, size_t len);
size_t pqwire_get_output(pqwire_ctx_t *ctx, uint8_t *buf, size_t max_len);
int pqwire_next_event(pqwire_ctx_t *ctx, protocol_event_t *event);

/** Current protocol phase (startup / auth / ready / querying / error). */
int pqwire_current_state(const pqwire_ctx_t *ctx);

/* ── authentication helpers ──────────────────────────────────────────── */

int pqwire_start_auth(pqwire_ctx_t *ctx);

/* ── frontend (client-role) send helpers ─────────────────────────────── */

int pqwire_send_startup(pqwire_ctx_t *ctx, const char *user, const char *database);
int pqwire_send_query(pqwire_ctx_t *ctx, const char *sql);
int pqwire_send_call(pqwire_ctx_t *ctx, const char *procedure);
int pqwire_send_terminate(pqwire_ctx_t *ctx);

/* Extended Query Protocol — frontend */
int pqwire_send_parse(pqwire_ctx_t *ctx, const char *statement, const char *query,
                      const uint32_t *param_type_oids, uint16_t n_param_types);
int pqwire_send_bind(pqwire_ctx_t *ctx, const char *portal, const char *statement,
                     const int16_t *param_formats, uint16_t n_param_formats,
                     const int32_t *param_lengths, const uint8_t *const *param_values,
                     uint16_t n_params,
                     const int16_t *result_formats, uint16_t n_result_formats);
int pqwire_send_describe(pqwire_ctx_t *ctx, char target /* 'S' or 'P' */, const char *name);
int pqwire_send_execute(pqwire_ctx_t *ctx, const char *portal, uint32_t max_rows);
int pqwire_send_close(pqwire_ctx_t *ctx, char target /* 'S' or 'P' */, const char *name);
int pqwire_send_sync(pqwire_ctx_t *ctx);
int pqwire_send_flush(pqwire_ctx_t *ctx);

/**
 * Convenience: emit unnamed Parse + Bind + Execute + Sync for one pipeline
 * step (proxy rewrite engine). statement/portal names are forced to "".
 */
int pqwire_send_unnamed_pipeline(pqwire_ctx_t *ctx, const char *query,
                                 const uint32_t *param_type_oids, uint16_t n_param_types,
                                 const int16_t *param_formats, uint16_t n_param_formats,
                                 const int32_t *param_lengths, const uint8_t *const *param_values,
                                 uint16_t n_params,
                                 const int16_t *result_formats, uint16_t n_result_formats,
                                 uint32_t max_rows);

/* ── backend (server-role) send helpers ──────────────────────────────── */

int pqwire_send_auth_ok(pqwire_ctx_t *ctx);
int pqwire_send_ready_for_query(pqwire_ctx_t *ctx);
int pqwire_send_row_description(pqwire_ctx_t *ctx, const pq_column_desc_t *cols, size_t n_cols);
int pqwire_send_data_row(pqwire_ctx_t *ctx, const uint8_t *data, size_t len,
                         int16_t format, uint32_t oid);
int pqwire_send_data_row_multi(pqwire_ctx_t *ctx,
                               const int32_t *lengths, const uint8_t *const *values,
                               uint16_t n_cols);
int pqwire_send_command_complete(pqwire_ctx_t *ctx, const char *tag);
int pqwire_send_error_response(pqwire_ctx_t *ctx, const char *severity,
                               const char *sqlstate, const char *message);
int pqwire_send_parse_complete(pqwire_ctx_t *ctx);
int pqwire_send_bind_complete(pqwire_ctx_t *ctx);
int pqwire_send_close_complete(pqwire_ctx_t *ctx);
int pqwire_send_no_data(pqwire_ctx_t *ctx);

/* ── Bind parameter builders (proxy identity injection) ──────────────── */

/**
 * Owned parameter slot for building or rewriting Bind messages.
 * When ownership is transferred into a bind builder, free with
 * pqwire_param_clear().
 */
typedef struct {
    int32_t  length;     /* -1 = NULL */
    uint8_t *data;       /* malloc'd when length >= 0 and data owned */
    int      owned;      /* 1 if data was allocated by helpers */
    int16_t  format;     /* 0 text, 1 binary */
} pqwire_param_t;

void pqwire_param_clear(pqwire_param_t *p);

/** Set text parameter (copies value). Returns 0 on success. */
int pqwire_param_set_text(pqwire_param_t *p, const char *text);

/** Set binary parameter (copies bytes). Returns 0 on success. */
int pqwire_param_set_binary(pqwire_param_t *p, const uint8_t *data, size_t len);

/** Set NULL parameter. */
void pqwire_param_set_null(pqwire_param_t *p);

/**
 * Overwrite one parameter in a parsed Bind view into an owned array.
 * Used by identity-grouped proxies: inject verified router_id into slot N.
 *
 * @param bind       Parsed Bind event payload (views into feed buffer)
 * @param out_params Caller-allocated array of size at least bind->n_params
 * @param slot       Parameter index to overwrite (0-based)
 * @param identity   Verified identity string (e.g. mTLS router_id)
 * @param as_binary  0 = text format, 1 = binary (UTF-8 bytes with binary format)
 * @return 0 on success, -1 on error
 *
 * On success, out_params holds copies of all parameters; caller must clear each.
 */
int pqwire_bind_inject_identity(const pq_bind_t *bind, pqwire_param_t *out_params,
                                uint16_t slot, const char *identity, int as_binary);

/**
 * Emit a Bind using owned parameters (portal/statement from caller).
 * Formats are taken from each param's format field.
 */
int pqwire_send_bind_params(pqwire_ctx_t *ctx, const char *portal, const char *statement,
                            const pqwire_param_t *params, uint16_t n_params,
                            const int16_t *result_formats, uint16_t n_result_formats);

/* ── statement cache types (caller-owned; library does not store) ─────── */

/**
 * Lightweight prepared-statement description for per-frontend caches.
 * The L7 proxy stores these when intercepting Parse; library only defines
 * the shape so apps and tests share one layout.
 */
typedef struct {
    char     name[PQWIRE_MAX_STMT_NAME];
    char     query[PQWIRE_MAX_QUERY_LEN];
    uint16_t n_param_types;
    uint32_t param_type_oids[PQWIRE_MAX_PARAM_TYPES];
    int16_t  identity_param_slot; /* -1 = unset; app policy for router_id slot */
} pqwire_prepared_stmt_t;

/** Fill a prepared_stmt from a Parse event. Returns 0 on success. */
int pqwire_prepared_from_parse(const pq_parse_t *parse, pqwire_prepared_stmt_t *out);

/* ── Mid-pipeline recovery (ErrorResponse → ReadyForQuery) ───────────── */

/**
 * Peek a complete PostgreSQL message (type + int32 length + body).
 * @return 0 on success, -1 if buffer is incomplete or malformed.
 */
int pqwire_msg_peek(const uint8_t *buf, size_t len, char *type_out, size_t *total_out);

/**
 * Status while draining a backend pipeline until ReadyForQuery.
 * Proxies use this to know whether the pipeline failed and when it is clean.
 */
typedef struct {
    int        saw_error;     /* ErrorResponse ('E') observed */
    int        saw_rfq;       /* ReadyForQuery ('Z') observed */
    int        complete;      /* same as saw_rfq (pipeline finished) */
    int        n_msgs;        /* messages observed */
    pq_error_t last_error;    /* last ErrorResponse fields (if saw_error) */
} pqwire_pipeline_status_t;

void pqwire_pipeline_status_init(pqwire_pipeline_status_t *st);

/**
 * Observe one complete backend message and update recovery status.
 * Does not allocate. Parses ErrorResponse fields into st->last_error.
 * @return 1 if ReadyForQuery was this message, 0 otherwise, -1 on bad frame.
 */
int pqwire_pipeline_observe_msg(pqwire_pipeline_status_t *st,
                                const uint8_t *msg, size_t len);

/**
 * Feed a complete backend message into a CLIENT-role context, drain events,
 * and update pipeline status. On ReadyForQuery, marks the context READY so
 * the next pipeline can start without a full pqwire_reset (which would drop
 * authentication state).
 *
 * @return 0 on success, -1 on hard error.
 */
int pqwire_pipeline_feed_backend_msg(pqwire_ctx_t *client_role,
                                     pqwire_pipeline_status_t *st,
                                     const uint8_t *msg, size_t len);

/**
 * Mark protocol phase READY without clearing auth / output buffers.
 * Used after ReadyForQuery mid-session so pooled backends stay usable.
 */
void pqwire_note_ready(pqwire_ctx_t *ctx);

/**
 * Whether a backend message type should be filtered from the frontend
 * during an identity-proxy pipeline (ParameterStatus, Notice, BackendKey,
 * and optionally Parse/Bind complete when the frontend already got locals).
 */
int pqwire_pipeline_filter_backend_type(char type, int skip_parse_bind_complete);

#ifdef __cplusplus
}
#endif

#endif /* PQWIRE_H */

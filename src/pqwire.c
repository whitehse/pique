/**
 * @file pqwire.c
 * @brief PostgreSQL wire protocol state machine (pure plumbing).
 *
 * Syscall-free, callback-free. Parses and serializes PDUs; caller owns I/O.
 */

#include "pqwire.h"

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

#define PQWIRE_MAGIC 0xABCD1234u
#define PQ_PROTO_3_0 0x00030000u

struct pqwire_ctx {
    uint32_t magic;
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

    /* Scratch for event string lifetimes (startup/query tags) */
    char user_scratch[128];
    char db_scratch[128];
    char sql_scratch[PQWIRE_MAX_QUERY_LEN];
    char tag_scratch[64];
    pq_column_desc_t col_scratch[PQWIRE_MAX_ROW_COLUMNS];
    char col_name_scratch[PQWIRE_MAX_ROW_COLUMNS][64];

    /*
     * DataRow column bytes must outlive feed_input's memmove of the
     * framing buffer. Arena is reset when the event queue drains empty.
     */
#define PQWIRE_ROW_ARENA 65536
    uint8_t row_arena[PQWIRE_ROW_ARENA];
    size_t  row_arena_used;

    char password[256];
    int use_scram;
    auth_state_t auth_state;
    protocol_state_t proto_state;
    scram_sha256_ctx_t scram_ctx;

    int startup_seen; /* server: have we seen StartupMessage */
};

/* ── ring helpers ────────────────────────────────────────────────────── */

static void enqueue_event(pqwire_ctx_t *ctx, const protocol_event_t *ev)
{
    if (ctx->event_count >= ctx->event_queue_size) {
        ctx->event_head = (ctx->event_head + 1) % ctx->event_queue_size;
        ctx->event_count--;
    }
    ctx->events[ctx->event_tail] = *ev;
    ctx->event_tail = (ctx->event_tail + 1) % ctx->event_queue_size;
    ctx->event_count++;
}

static int dequeue_event(pqwire_ctx_t *ctx, protocol_event_t *out)
{
    if (ctx->event_count == 0) {
        return 0;
    }
    *out = ctx->events[ctx->event_head];
    ctx->event_head = (ctx->event_head + 1) % ctx->event_queue_size;
    ctx->event_count--;
    if (ctx->event_count == 0) {
        /* Safe to reuse DataRow arena once no queued events hold pointers. */
        ctx->row_arena_used = 0;
    }
    return 1;
}

static int ensure_output(pqwire_ctx_t *ctx, size_t need)
{
    if (ctx->output_len + need <= ctx->output_cap) {
        return 0;
    }
    size_t new_cap = ctx->output_cap * 2 + need + 1024;
    uint8_t *p = realloc(ctx->output_buf, new_cap);
    if (!p) {
        return -1;
    }
    ctx->output_buf = p;
    ctx->output_cap = new_cap;
    return 0;
}

static void append_output(pqwire_ctx_t *ctx, const uint8_t *data, size_t len)
{
    if (ensure_output(ctx, len) != 0) {
        return;
    }
    memcpy(ctx->output_buf + ctx->output_len, data, len);
    ctx->output_len += len;
}

static void wr32_be(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)((v >> 24) & 0xFF);
    p[1] = (uint8_t)((v >> 16) & 0xFF);
    p[2] = (uint8_t)((v >> 8) & 0xFF);
    p[3] = (uint8_t)(v & 0xFF);
}

static void wr16_be(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)((v >> 8) & 0xFF);
    p[1] = (uint8_t)(v & 0xFF);
}

static uint32_t rd32_be(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static uint16_t rd16_be(const uint8_t *p)
{
    return (uint16_t)(((uint16_t)p[0] << 8) | (uint16_t)p[1]);
}

/** Append typed message: type + int32 length (payload+4) + payload */
static int emit_msg(pqwire_ctx_t *ctx, char type, const uint8_t *payload, size_t plen)
{
    uint8_t hdr[5];
    uint32_t len = (uint32_t)(plen + 4);
    hdr[0] = (uint8_t)type;
    wr32_be(hdr + 1, len);
    append_output(ctx, hdr, 5);
    if (plen && payload) {
        append_output(ctx, payload, plen);
    }
    return 0;
}

static void copy_cstr(char *dst, size_t dstsz, const char *src)
{
    if (!dst || dstsz == 0) {
        return;
    }
    if (!src) {
        dst[0] = '\0';
        return;
    }
    strncpy(dst, src, dstsz - 1);
    dst[dstsz - 1] = '\0';
}

/* ── lifecycle ───────────────────────────────────────────────────────── */

pqwire_ctx_t *pqwire_create(pqwire_role_t role)
{
    pqwire_config_t cfg = {
        .event_queue_size = 16,
        .password = NULL,
        .use_scram = 1,
        .max_message_size = 0
    };
    return pqwire_create_with_config(role, &cfg);
}

pqwire_ctx_t *pqwire_create_with_config(pqwire_role_t role, const pqwire_config_t *config)
{
    if (!config || config->event_queue_size <= 0) {
        return NULL;
    }

    pqwire_ctx_t *ctx = calloc(1, sizeof(*ctx));
    if (!ctx) {
        return NULL;
    }

    ctx->role = role;
    ctx->event_queue_size = config->event_queue_size;
    ctx->events = calloc((size_t)config->event_queue_size, sizeof(protocol_event_t));
    ctx->output_cap = 4096;
    ctx->output_buf = malloc(ctx->output_cap);
    ctx->input_cap = 4096;
    ctx->input_buf = malloc(ctx->input_cap);

    if (!ctx->events || !ctx->output_buf || !ctx->input_buf) {
        free(ctx->events);
        free(ctx->output_buf);
        free(ctx->input_buf);
        free(ctx);
        return NULL;
    }

    if (config->password) {
        strncpy(ctx->password, config->password, sizeof(ctx->password) - 1);
    }
    ctx->use_scram = config->use_scram;
    ctx->max_message_size = config->max_message_size ? config->max_message_size
                                                     : (16u * 1024u * 1024u);
    ctx->auth_state = AUTH_STATE_NONE;
    ctx->proto_state = PROTO_STATE_STARTUP;
    ctx->magic = PQWIRE_MAGIC;
    ctx->startup_seen = 0;

    return ctx;
}

void pqwire_destroy(pqwire_ctx_t *ctx)
{
    if (!ctx) {
        return;
    }
    if (ctx->magic != PQWIRE_MAGIC) {
        return;
    }
    ctx->magic = 0;
    free(ctx->events);
    free(ctx->output_buf);
    free(ctx->input_buf);
    free(ctx);
}

void pqwire_reset(pqwire_ctx_t *ctx)
{
    if (!ctx || ctx->magic != PQWIRE_MAGIC) {
        return;
    }
    ctx->event_head = ctx->event_tail = ctx->event_count = 0;
    ctx->output_len = 0;
    ctx->input_len = 0;
    ctx->auth_state = AUTH_STATE_NONE;
    ctx->proto_state = PROTO_STATE_STARTUP;
    ctx->startup_seen = 0;
    memset(&ctx->scram_ctx, 0, sizeof(ctx->scram_ctx));
}

int pqwire_current_state(const pqwire_ctx_t *ctx)
{
    if (!ctx || ctx->magic != PQWIRE_MAGIC) {
        return -1;
    }
    return (int)ctx->proto_state;
}

/* ── auth ────────────────────────────────────────────────────────────── */

int pqwire_start_auth(pqwire_ctx_t *ctx)
{
    if (!ctx || ctx->magic != PQWIRE_MAGIC || ctx->auth_state != AUTH_STATE_NONE) {
        return -1;
    }

    if (ctx->role == PQWIRE_ROLE_CLIENT && ctx->use_scram && ctx->password[0]) {
        scram_sha256_init(&ctx->scram_ctx, "user");
        ctx->auth_state = AUTH_STATE_CLIENT_FIRST;

        char first[256];
        if (scram_sha256_client_first(&ctx->scram_ctx, first, sizeof(first)) == 0) {
            uint8_t buf[512];
            size_t p = 0;
            size_t flen = strlen(first);
            buf[p++] = 'p';
            wr32_be(buf + 1, (uint32_t)(4 + flen + 1));
            memcpy(buf + 5, first, flen + 1);
            append_output(ctx, buf, 5 + flen + 1);
        }
    }
    return 0;
}

/* ── parse helpers ───────────────────────────────────────────────────── */

static const char *read_cstr(const uint8_t *p, size_t avail, size_t *consumed)
{
    size_t i;
    for (i = 0; i < avail; i++) {
        if (p[i] == 0) {
            *consumed = i + 1;
            return (const char *)p;
        }
    }
    *consumed = 0;
    return NULL;
}

static void parse_startup_body(pqwire_ctx_t *ctx, const uint8_t *body, size_t body_len)
{
    /* body starts at protocol version (4) then key\0value\0 ... \0 */
    size_t off = 4;
    protocol_event_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = PQ_EVENT_STARTUP;
    ctx->user_scratch[0] = '\0';
    ctx->db_scratch[0] = '\0';

    while (off + 1 < body_len) {
        size_t c1 = 0, c2 = 0;
        const char *key = read_cstr(body + off, body_len - off, &c1);
        if (!key || c1 <= 1) {
            break;
        }
        off += c1;
        const char *val = read_cstr(body + off, body_len - off, &c2);
        if (!val) {
            break;
        }
        off += c2;
        if (strcmp(key, "user") == 0) {
            copy_cstr(ctx->user_scratch, sizeof(ctx->user_scratch), val);
        } else if (strcmp(key, "database") == 0) {
            copy_cstr(ctx->db_scratch, sizeof(ctx->db_scratch), val);
        }
    }

    ev.payload.startup.user = ctx->user_scratch;
    ev.payload.startup.database = ctx->db_scratch;
    enqueue_event(ctx, &ev);
    ctx->startup_seen = 1;
    if (ctx->role == PQWIRE_ROLE_SERVER) {
        ctx->proto_state = PROTO_STATE_AUTHENTICATING;
    }
}

static void parse_parse_msg(pqwire_ctx_t *ctx, const uint8_t *body, size_t body_len)
{
    protocol_event_t ev;
    size_t off = 0, c = 0;
    const char *name, *query;
    memset(&ev, 0, sizeof(ev));
    ev.type = PQ_EVENT_PARSE;

    name = read_cstr(body + off, body_len - off, &c);
    if (!name) {
        return;
    }
    copy_cstr(ev.payload.parse.statement, sizeof(ev.payload.parse.statement), name);
    off += c;

    query = read_cstr(body + off, body_len - off, &c);
    if (!query) {
        return;
    }
    copy_cstr(ev.payload.parse.query, sizeof(ev.payload.parse.query), query);
    off += c;

    if (off + 2 <= body_len) {
        uint16_t n = rd16_be(body + off);
        uint16_t i;
        off += 2;
        if (n > PQWIRE_MAX_PARAM_TYPES) {
            n = PQWIRE_MAX_PARAM_TYPES;
        }
        ev.payload.parse.n_param_types = n;
        for (i = 0; i < n && off + 4 <= body_len; i++) {
            ev.payload.parse.param_type_oids[i] = rd32_be(body + off);
            off += 4;
        }
    }
    enqueue_event(ctx, &ev);
}

static void parse_bind_msg(pqwire_ctx_t *ctx, const uint8_t *body, size_t body_len)
{
    protocol_event_t ev;
    size_t off = 0, c = 0;
    const char *portal, *stmt;
    uint16_t i, n;
    memset(&ev, 0, sizeof(ev));
    ev.type = PQ_EVENT_BIND;
    ev.payload.bind.raw_body = body;
    ev.payload.bind.raw_body_len = body_len;

    portal = read_cstr(body + off, body_len - off, &c);
    if (!portal) {
        return;
    }
    copy_cstr(ev.payload.bind.portal, sizeof(ev.payload.bind.portal), portal);
    off += c;

    stmt = read_cstr(body + off, body_len - off, &c);
    if (!stmt) {
        return;
    }
    copy_cstr(ev.payload.bind.statement, sizeof(ev.payload.bind.statement), stmt);
    off += c;

    if (off + 2 > body_len) {
        return;
    }
    n = rd16_be(body + off);
    off += 2;
    if (n > PQWIRE_MAX_BIND_PARAMS) {
        n = PQWIRE_MAX_BIND_PARAMS;
    }
    ev.payload.bind.n_formats = n;
    for (i = 0; i < n && off + 2 <= body_len; i++) {
        ev.payload.bind.formats[i] = (int16_t)rd16_be(body + off);
        off += 2;
    }

    if (off + 2 > body_len) {
        return;
    }
    n = rd16_be(body + off);
    off += 2;
    if (n > PQWIRE_MAX_BIND_PARAMS) {
        n = PQWIRE_MAX_BIND_PARAMS;
    }
    ev.payload.bind.n_params = n;
    for (i = 0; i < n && off + 4 <= body_len; i++) {
        int32_t plen = (int32_t)rd32_be(body + off);
        off += 4;
        ev.payload.bind.params[i].length = plen;
        if (plen < 0) {
            ev.payload.bind.params[i].data = NULL;
        } else {
            if (off + (size_t)plen > body_len) {
                return;
            }
            ev.payload.bind.params[i].data = body + off;
            off += (size_t)plen;
        }
        if (ev.payload.bind.n_formats == 0) {
            ev.payload.bind.params[i].format = 0;
        } else if (ev.payload.bind.n_formats == 1) {
            ev.payload.bind.params[i].format = ev.payload.bind.formats[0];
        } else {
            ev.payload.bind.params[i].format =
                (i < ev.payload.bind.n_formats) ? ev.payload.bind.formats[i] : 0;
        }
    }

    if (off + 2 <= body_len) {
        n = rd16_be(body + off);
        off += 2;
        if (n > PQWIRE_MAX_BIND_PARAMS) {
            n = PQWIRE_MAX_BIND_PARAMS;
        }
        ev.payload.bind.n_result_formats = n;
        for (i = 0; i < n && off + 2 <= body_len; i++) {
            ev.payload.bind.result_formats[i] = (int16_t)rd16_be(body + off);
            off += 2;
        }
    }

    enqueue_event(ctx, &ev);
}

static void parse_describe_msg(pqwire_ctx_t *ctx, const uint8_t *body, size_t body_len)
{
    protocol_event_t ev;
    size_t c = 0;
    const char *name;
    memset(&ev, 0, sizeof(ev));
    if (body_len < 1) {
        return;
    }
    ev.type = PQ_EVENT_DESCRIBE;
    ev.payload.describe.target = (char)body[0];
    name = read_cstr(body + 1, body_len - 1, &c);
    if (name) {
        copy_cstr(ev.payload.describe.name, sizeof(ev.payload.describe.name), name);
    }
    enqueue_event(ctx, &ev);
}

static void parse_execute_msg(pqwire_ctx_t *ctx, const uint8_t *body, size_t body_len)
{
    protocol_event_t ev;
    size_t off = 0, c = 0;
    const char *portal;
    memset(&ev, 0, sizeof(ev));
    ev.type = PQ_EVENT_EXECUTE;
    portal = read_cstr(body + off, body_len - off, &c);
    if (!portal) {
        return;
    }
    copy_cstr(ev.payload.execute.portal, sizeof(ev.payload.execute.portal), portal);
    off += c;
    if (off + 4 <= body_len) {
        ev.payload.execute.max_rows = rd32_be(body + off);
    }
    enqueue_event(ctx, &ev);
}

static void parse_close_msg(pqwire_ctx_t *ctx, const uint8_t *body, size_t body_len)
{
    protocol_event_t ev;
    size_t c = 0;
    const char *name;
    memset(&ev, 0, sizeof(ev));
    if (body_len < 1) {
        return;
    }
    ev.type = PQ_EVENT_CLOSE;
    ev.payload.close_msg.target = (char)body[0];
    name = read_cstr(body + 1, body_len - 1, &c);
    if (name) {
        copy_cstr(ev.payload.close_msg.name, sizeof(ev.payload.close_msg.name), name);
    }
    enqueue_event(ctx, &ev);
}

static void parse_query_msg(pqwire_ctx_t *ctx, const uint8_t *body, size_t body_len)
{
    protocol_event_t ev;
    size_t c = 0;
    const char *sql = read_cstr(body, body_len, &c);
    memset(&ev, 0, sizeof(ev));
    ev.type = PQ_EVENT_QUERY;
    if (sql) {
        copy_cstr(ctx->sql_scratch, sizeof(ctx->sql_scratch), sql);
    } else {
        ctx->sql_scratch[0] = '\0';
    }
    ev.payload.query.sql = ctx->sql_scratch;
    enqueue_event(ctx, &ev);
    ctx->proto_state = PROTO_STATE_QUERYING;
}

static void parse_error_response(pqwire_ctx_t *ctx, const uint8_t *body, size_t body_len)
{
    protocol_event_t ev;
    size_t off = 0;
    memset(&ev, 0, sizeof(ev));
    ev.type = PQ_EVENT_ERROR_RESPONSE;
    while (off < body_len && body[off] != 0) {
        char field = (char)body[off++];
        size_t c = 0;
        const char *val = read_cstr(body + off, body_len - off, &c);
        if (!val) {
            break;
        }
        off += c;
        switch (field) {
        case 'S':
            copy_cstr(ev.payload.error.severity, sizeof(ev.payload.error.severity), val);
            break;
        case 'C':
            copy_cstr(ev.payload.error.code, sizeof(ev.payload.error.code), val);
            break;
        case 'M':
            copy_cstr(ev.payload.error.message, sizeof(ev.payload.error.message), val);
            break;
        case 'D':
            copy_cstr(ev.payload.error.detail, sizeof(ev.payload.error.detail), val);
            break;
        case 'H':
            copy_cstr(ev.payload.error.hint, sizeof(ev.payload.error.hint), val);
            break;
        case 'P':
            ev.payload.error.position = atoi(val);
            break;
        default:
            break;
        }
    }
    enqueue_event(ctx, &ev);
}

static void parse_row_description(pqwire_ctx_t *ctx, const uint8_t *body, size_t body_len)
{
    protocol_event_t ev;
    size_t off = 0;
    uint16_t n, i;
    memset(&ev, 0, sizeof(ev));
    if (body_len < 2) {
        return;
    }
    n = rd16_be(body);
    off = 2;
    if (n > PQWIRE_MAX_ROW_COLUMNS) {
        n = PQWIRE_MAX_ROW_COLUMNS;
    }
    for (i = 0; i < n && off < body_len; i++) {
        size_t c = 0;
        const char *name = read_cstr(body + off, body_len - off, &c);
        if (!name) {
            break;
        }
        copy_cstr(ctx->col_name_scratch[i], sizeof(ctx->col_name_scratch[i]), name);
        off += c;
        /* tableoid(4) colattr(2) typeoid(4) typlen(2) typmod(4) format(2) */
        if (off + 18 > body_len) {
            break;
        }
        ctx->col_scratch[i].name = ctx->col_name_scratch[i];
        ctx->col_scratch[i].oid = rd32_be(body + off + 6);
        ctx->col_scratch[i].format = (int16_t)rd16_be(body + off + 16);
        off += 18;
    }
    ev.type = PQ_EVENT_ROW_DESCRIPTION;
    ev.payload.row_desc.columns = ctx->col_scratch;
    ev.payload.row_desc.column_count = i;
    enqueue_event(ctx, &ev);
}

static void parse_data_row(pqwire_ctx_t *ctx, const uint8_t *body, size_t body_len)
{
    protocol_event_t ev;
    size_t off = 0;
    uint16_t n, i;
    memset(&ev, 0, sizeof(ev));
    if (body_len < 2) {
        return;
    }
    n = rd16_be(body);
    off = 2;
    if (n > PQWIRE_MAX_ROW_COLUMNS) {
        n = PQWIRE_MAX_ROW_COLUMNS;
    }
    ev.type = PQ_EVENT_DATA_ROW;
    for (i = 0; i < n && off + 4 <= body_len; i++) {
        int32_t plen = (int32_t)rd32_be(body + off);
        off += 4;
        if (plen < 0) {
            ev.payload.data_row.data[i] = NULL;
            ev.payload.data_row.len[i] = 0;
        } else {
            size_t need = (size_t)plen;
            if (off + need > body_len) {
                break;
            }
            /* Copy out of framing buffer before memmove in feed_input. */
            if (ctx->row_arena_used + need > PQWIRE_ROW_ARENA) {
                /* Drop column if arena exhausted (oversized row). */
                ev.payload.data_row.data[i] = NULL;
                ev.payload.data_row.len[i] = 0;
            } else {
                uint8_t *dst = ctx->row_arena + ctx->row_arena_used;
                memcpy(dst, body + off, need);
                ctx->row_arena_used += need;
                ev.payload.data_row.data[i] = dst;
                ev.payload.data_row.len[i] = need;
            }
            off += need;
        }
        ev.payload.data_row.format[i] = 0;
        ev.payload.data_row.oid[i] = 0;
    }
    ev.payload.data_row.n_columns = i;
    enqueue_event(ctx, &ev);
}

static void parse_command_complete(pqwire_ctx_t *ctx, const uint8_t *body, size_t body_len)
{
    protocol_event_t ev;
    size_t c = 0;
    const char *tag = read_cstr(body, body_len, &c);
    memset(&ev, 0, sizeof(ev));
    ev.type = PQ_EVENT_COMMAND_COMPLETE;
    if (tag) {
        copy_cstr(ctx->tag_scratch, sizeof(ctx->tag_scratch), tag);
    } else {
        ctx->tag_scratch[0] = '\0';
    }
    ev.payload.command_complete.tag = ctx->tag_scratch;
    enqueue_event(ctx, &ev);
    ctx->proto_state = PROTO_STATE_READY;
}

/* NotificationResponse ('A'): int32 pid | channel\0 | payload\0 */
static void parse_notification_response(pqwire_ctx_t *ctx, const uint8_t *body,
                                        size_t body_len)
{
    protocol_event_t ev;
    size_t off = 0;
    const char *channel;
    const char *payload;
    size_t c = 0;
    size_t plen;

    (void)ctx;
    memset(&ev, 0, sizeof(ev));
    ev.type = PQ_EVENT_NOTIFICATION;
    if (body_len < 4) {
        return;
    }
    ev.payload.notification.pid = (int32_t)rd32_be(body);
    off = 4;
    channel = read_cstr(body + off, body_len - off, &c);
    if (!channel) {
        return;
    }
    off += c;
    copy_cstr(ev.payload.notification.channel,
              sizeof(ev.payload.notification.channel), channel);
    payload = read_cstr(body + off, body_len - off, &c);
    if (!payload) {
        payload = "";
        c = 1;
    }
    plen = c > 0 ? c - 1 : 0; /* exclude NUL counted by read_cstr if present */
    /* read_cstr returns pointer into body and advances past NUL; length of
     * string is strlen. Prefer strlen for safety. */
    plen = 0;
    while (payload[plen] != '\0' && plen < PQWIRE_NOTIFY_PAYLOAD_MAX) {
        plen++;
    }
    if (plen > PQWIRE_NOTIFY_PAYLOAD_MAX) {
        plen = PQWIRE_NOTIFY_PAYLOAD_MAX;
    }
    memcpy(ev.payload.notification.payload, payload, plen);
    ev.payload.notification.payload[plen] = '\0';
    ev.payload.notification.payload_len = plen;
    enqueue_event(ctx, &ev);
}

/* Message dispatch is role-aware. */
static void handle_message(pqwire_ctx_t *ctx, char type, const uint8_t *body, size_t body_len)
{
    protocol_event_t ev;
    memset(&ev, 0, sizeof(ev));

    if (ctx->role == PQWIRE_ROLE_SERVER) {
        /* Messages from frontend (client) */
        switch (type) {
        case 'Q': parse_query_msg(ctx, body, body_len); break;
        case 'P': parse_parse_msg(ctx, body, body_len); break;
        case 'B': parse_bind_msg(ctx, body, body_len); break;
        case 'D': parse_describe_msg(ctx, body, body_len); break;
        case 'E': parse_execute_msg(ctx, body, body_len); break;
        case 'C': parse_close_msg(ctx, body, body_len); break;
        case 'S':
            ev.type = PQ_EVENT_SYNC;
            enqueue_event(ctx, &ev);
            break;
        case 'H':
            ev.type = PQ_EVENT_FLUSH;
            enqueue_event(ctx, &ev);
            break;
        case 'X':
            ev.type = PQ_EVENT_TERMINATE;
            enqueue_event(ctx, &ev);
            break;
        default:
            break;
        }
        return;
    }

    /* Messages from backend (server) — client role */
    switch (type) {
    case 'R':
        if (body_len >= 4 && rd32_be(body) == 0) {
            ev.type = PQ_EVENT_AUTHENTICATION_OK;
            enqueue_event(ctx, &ev);
            ctx->auth_state = AUTH_STATE_DONE;
        }
        break;
    case 'Z':
        ev.type = PQ_EVENT_READY_FOR_QUERY;
        enqueue_event(ctx, &ev);
        ctx->proto_state = PROTO_STATE_READY;
        break;
    case 'T': parse_row_description(ctx, body, body_len); break;
    case 'D': parse_data_row(ctx, body, body_len); break;
    case 'C': parse_command_complete(ctx, body, body_len); break;
    case 'E': parse_error_response(ctx, body, body_len); break;
    case '1':
        ev.type = PQ_EVENT_PARSE_COMPLETE;
        enqueue_event(ctx, &ev);
        break;
    case '2':
        ev.type = PQ_EVENT_BIND_COMPLETE;
        enqueue_event(ctx, &ev);
        break;
    case '3':
        ev.type = PQ_EVENT_CLOSE_COMPLETE;
        enqueue_event(ctx, &ev);
        break;
    case 'n':
        ev.type = PQ_EVENT_NO_DATA;
        enqueue_event(ctx, &ev);
        break;
    case 's':
        ev.type = PQ_EVENT_PORTAL_SUSPENDED;
        enqueue_event(ctx, &ev);
        break;
    case 'S':
        ev.type = PQ_EVENT_PARAMETER_STATUS;
        ev.payload.raw.data = body;
        ev.payload.raw.len = body_len;
        enqueue_event(ctx, &ev);
        break;
    case 'N':
        ev.type = PQ_EVENT_NOTICE_RESPONSE;
        enqueue_event(ctx, &ev);
        break;
    case 'A':
        parse_notification_response(ctx, body, body_len);
        break;
    case 'K':
        /* BackendKeyData — ignore for client LISTEN path */
        break;
    default:
        break;
    }
}

size_t pqwire_feed_input(pqwire_ctx_t *ctx, const uint8_t *data, size_t len)
{
    size_t consumed_total = 0;

    if (!ctx || ctx->magic != PQWIRE_MAGIC || !data) {
        return 0;
    }

    if (ctx->input_len + len > ctx->input_cap) {
        size_t new_cap = ctx->input_len + len + 1024;
        uint8_t *p = realloc(ctx->input_buf, new_cap);
        if (!p) {
            return 0;
        }
        ctx->input_buf = p;
        ctx->input_cap = new_cap;
    }
    memcpy(ctx->input_buf + ctx->input_len, data, len);
    ctx->input_len += len;
    consumed_total = len;

    for (;;) {
        /* StartupMessage (no type byte) when server hasn't seen startup */
        if (ctx->role == PQWIRE_ROLE_SERVER && !ctx->startup_seen) {
            if (ctx->input_len < 4) {
                break;
            }
            uint32_t mlen = rd32_be(ctx->input_buf);
            if (mlen < 8 || mlen > ctx->max_message_size) {
                protocol_event_t ev = {0};
                ev.type = PQ_EVENT_PROTOCOL_ERROR;
                enqueue_event(ctx, &ev);
                ctx->input_len = 0;
                break;
            }
            if (ctx->input_len < mlen) {
                break;
            }
            /* body after length field */
            parse_startup_body(ctx, ctx->input_buf + 4, mlen - 4);
            memmove(ctx->input_buf, ctx->input_buf + mlen, ctx->input_len - mlen);
            ctx->input_len -= mlen;
            continue;
        }

        /* Typed messages: type(1) + len(4) + payload */
        if (ctx->input_len < 5) {
            break;
        }
        {
            char type = (char)ctx->input_buf[0];
            uint32_t mlen = rd32_be(ctx->input_buf + 1);
            size_t total;

            if (mlen < 4 || mlen > ctx->max_message_size) {
                protocol_event_t ev = {0};
                ev.type = PQ_EVENT_PROTOCOL_ERROR;
                enqueue_event(ctx, &ev);
                if (mlen > 0 && ctx->input_len >= (size_t)mlen + 1) {
                    memmove(ctx->input_buf, ctx->input_buf + 1 + mlen,
                            ctx->input_len - 1 - mlen);
                    ctx->input_len -= (1 + mlen);
                } else {
                    ctx->input_len = 0;
                }
                continue;
            }
            total = 1u + (size_t)mlen;
            if (ctx->input_len < total) {
                break;
            }
            handle_message(ctx, type, ctx->input_buf + 5, (size_t)mlen - 4);
            memmove(ctx->input_buf, ctx->input_buf + total, ctx->input_len - total);
            ctx->input_len -= total;
        }
    }

    return consumed_total;
}

size_t pqwire_get_output(pqwire_ctx_t *ctx, uint8_t *buf, size_t max_len)
{
    if (!ctx || !buf || max_len == 0) {
        return 0;
    }
    size_t to_copy = ctx->output_len < max_len ? ctx->output_len : max_len;
    memcpy(buf, ctx->output_buf, to_copy);
    memmove(ctx->output_buf, ctx->output_buf + to_copy, ctx->output_len - to_copy);
    ctx->output_len -= to_copy;
    return to_copy;
}

int pqwire_next_event(pqwire_ctx_t *ctx, protocol_event_t *event)
{
    if (!ctx || ctx->magic != PQWIRE_MAGIC || !event) {
        return 0;
    }
    return dequeue_event(ctx, event);
}

/* ── send helpers ────────────────────────────────────────────────────── */

int pqwire_send_startup(pqwire_ctx_t *ctx, const char *user, const char *database)
{
    uint8_t buf[512];
    size_t p = 4; /* leave room for length */
    if (!ctx || ctx->magic != PQWIRE_MAGIC || ctx->role != PQWIRE_ROLE_CLIENT) {
        return -1;
    }
    if (!user) {
        user = "";
    }
    if (!database) {
        database = "";
    }
    wr32_be(buf + p, PQ_PROTO_3_0);
    p += 4;
    memcpy(buf + p, "user", 5);
    p += 5;
    size_t ulen = strlen(user) + 1;
    if (p + ulen >= sizeof(buf)) {
        return -1;
    }
    memcpy(buf + p, user, ulen);
    p += ulen;
    memcpy(buf + p, "database", 9);
    p += 9;
    size_t dlen = strlen(database) + 1;
    if (p + dlen + 1 >= sizeof(buf)) {
        return -1;
    }
    memcpy(buf + p, database, dlen);
    p += dlen;
    buf[p++] = 0; /* terminator */
    wr32_be(buf, (uint32_t)p);
    append_output(ctx, buf, p);
    return 0;
}

int pqwire_send_query(pqwire_ctx_t *ctx, const char *sql)
{
    if (!ctx || ctx->magic != PQWIRE_MAGIC || !sql) {
        return -1;
    }
    size_t slen = strlen(sql) + 1;
    return emit_msg(ctx, 'Q', (const uint8_t *)sql, slen);
}

int pqwire_send_call(pqwire_ctx_t *ctx, const char *procedure)
{
    char sql[512];
    if (!procedure) {
        return -1;
    }
    snprintf(sql, sizeof(sql), "CALL %s", procedure);
    return pqwire_send_query(ctx, sql);
}

int pqwire_send_terminate(pqwire_ctx_t *ctx)
{
    if (!ctx || ctx->magic != PQWIRE_MAGIC) {
        return -1;
    }
    return emit_msg(ctx, 'X', NULL, 0);
}

int pqwire_send_parse(pqwire_ctx_t *ctx, const char *statement, const char *query,
                      const uint32_t *param_type_oids, uint16_t n_param_types)
{
    uint8_t buf[PQWIRE_MAX_QUERY_LEN + 256];
    size_t p = 0;
    uint16_t i;
    if (!ctx || ctx->magic != PQWIRE_MAGIC || !query) {
        return -1;
    }
    if (!statement) {
        statement = "";
    }
    size_t nlen = strlen(statement) + 1;
    size_t qlen = strlen(query) + 1;
    if (nlen + qlen + 2 + (size_t)n_param_types * 4 > sizeof(buf)) {
        return -1;
    }
    memcpy(buf + p, statement, nlen);
    p += nlen;
    memcpy(buf + p, query, qlen);
    p += qlen;
    wr16_be(buf + p, n_param_types);
    p += 2;
    for (i = 0; i < n_param_types; i++) {
        wr32_be(buf + p, param_type_oids ? param_type_oids[i] : 0);
        p += 4;
    }
    return emit_msg(ctx, 'P', buf, p);
}

int pqwire_send_bind(pqwire_ctx_t *ctx, const char *portal, const char *statement,
                     const int16_t *param_formats, uint16_t n_param_formats,
                     const int32_t *param_lengths, const uint8_t *const *param_values,
                     uint16_t n_params,
                     const int16_t *result_formats, uint16_t n_result_formats)
{
    /* Build on heap for large param sets */
    size_t cap = 64 + (size_t)n_params * 256;
    uint8_t *buf;
    size_t p = 0;
    uint16_t i;
    int rc;

    if (!ctx || ctx->magic != PQWIRE_MAGIC) {
        return -1;
    }
    if (!portal) {
        portal = "";
    }
    if (!statement) {
        statement = "";
    }
    buf = malloc(cap);
    if (!buf) {
        return -1;
    }

    {
        size_t nlen = strlen(portal) + 1;
        size_t slen = strlen(statement) + 1;
        if (p + nlen + slen > cap) {
            free(buf);
            return -1;
        }
        memcpy(buf + p, portal, nlen);
        p += nlen;
        memcpy(buf + p, statement, slen);
        p += slen;
    }

    if (p + 2 + (size_t)n_param_formats * 2 > cap) {
        free(buf);
        return -1;
    }
    wr16_be(buf + p, n_param_formats);
    p += 2;
    for (i = 0; i < n_param_formats; i++) {
        wr16_be(buf + p, (uint16_t)(param_formats ? param_formats[i] : 0));
        p += 2;
    }

    if (p + 2 > cap) {
        free(buf);
        return -1;
    }
    wr16_be(buf + p, n_params);
    p += 2;
    for (i = 0; i < n_params; i++) {
        int32_t plen = param_lengths ? param_lengths[i] : -1;
        if (p + 4 > cap) {
            free(buf);
            return -1;
        }
        wr32_be(buf + p, (uint32_t)plen);
        p += 4;
        if (plen > 0) {
            if (p + (size_t)plen > cap) {
                size_t ncap = p + (size_t)plen + 1024;
                uint8_t *nb = realloc(buf, ncap);
                if (!nb) {
                    free(buf);
                    return -1;
                }
                buf = nb;
                cap = ncap;
            }
            if (param_values && param_values[i]) {
                memcpy(buf + p, param_values[i], (size_t)plen);
            }
            p += (size_t)plen;
        }
    }

    if (p + 2 + (size_t)n_result_formats * 2 > cap) {
        free(buf);
        return -1;
    }
    wr16_be(buf + p, n_result_formats);
    p += 2;
    for (i = 0; i < n_result_formats; i++) {
        wr16_be(buf + p, (uint16_t)(result_formats ? result_formats[i] : 0));
        p += 2;
    }

    rc = emit_msg(ctx, 'B', buf, p);
    free(buf);
    return rc;
}

int pqwire_send_describe(pqwire_ctx_t *ctx, char target, const char *name)
{
    uint8_t buf[PQWIRE_MAX_STMT_NAME + 2];
    size_t nlen;
    if (!ctx || ctx->magic != PQWIRE_MAGIC) {
        return -1;
    }
    if (!name) {
        name = "";
    }
    nlen = strlen(name) + 1;
    if (1 + nlen > sizeof(buf)) {
        return -1;
    }
    buf[0] = (uint8_t)target;
    memcpy(buf + 1, name, nlen);
    return emit_msg(ctx, 'D', buf, 1 + nlen);
}

int pqwire_send_execute(pqwire_ctx_t *ctx, const char *portal, uint32_t max_rows)
{
    uint8_t buf[PQWIRE_MAX_PORTAL_NAME + 8];
    size_t nlen, p = 0;
    if (!ctx || ctx->magic != PQWIRE_MAGIC) {
        return -1;
    }
    if (!portal) {
        portal = "";
    }
    nlen = strlen(portal) + 1;
    if (nlen + 4 > sizeof(buf)) {
        return -1;
    }
    memcpy(buf, portal, nlen);
    p = nlen;
    wr32_be(buf + p, max_rows);
    p += 4;
    return emit_msg(ctx, 'E', buf, p);
}

int pqwire_send_close(pqwire_ctx_t *ctx, char target, const char *name)
{
    uint8_t buf[PQWIRE_MAX_STMT_NAME + 2];
    size_t nlen;
    if (!ctx || ctx->magic != PQWIRE_MAGIC) {
        return -1;
    }
    if (!name) {
        name = "";
    }
    nlen = strlen(name) + 1;
    if (1 + nlen > sizeof(buf)) {
        return -1;
    }
    buf[0] = (uint8_t)target;
    memcpy(buf + 1, name, nlen);
    return emit_msg(ctx, 'C', buf, 1 + nlen);
}

int pqwire_send_sync(pqwire_ctx_t *ctx)
{
    if (!ctx || ctx->magic != PQWIRE_MAGIC) {
        return -1;
    }
    return emit_msg(ctx, 'S', NULL, 0);
}

int pqwire_send_flush(pqwire_ctx_t *ctx)
{
    if (!ctx || ctx->magic != PQWIRE_MAGIC) {
        return -1;
    }
    return emit_msg(ctx, 'H', NULL, 0);
}

int pqwire_send_unnamed_pipeline(pqwire_ctx_t *ctx, const char *query,
                                 const uint32_t *param_type_oids, uint16_t n_param_types,
                                 const int16_t *param_formats, uint16_t n_param_formats,
                                 const int32_t *param_lengths, const uint8_t *const *param_values,
                                 uint16_t n_params,
                                 const int16_t *result_formats, uint16_t n_result_formats,
                                 uint32_t max_rows)
{
    if (pqwire_send_parse(ctx, "", query, param_type_oids, n_param_types) != 0) {
        return -1;
    }
    if (pqwire_send_bind(ctx, "", "", param_formats, n_param_formats,
                         param_lengths, param_values, n_params,
                         result_formats, n_result_formats) != 0) {
        return -1;
    }
    if (pqwire_send_execute(ctx, "", max_rows) != 0) {
        return -1;
    }
    return pqwire_send_sync(ctx);
}

int pqwire_send_auth_ok(pqwire_ctx_t *ctx)
{
    uint8_t body[4];
    if (!ctx || ctx->magic != PQWIRE_MAGIC) {
        return -1;
    }
    wr32_be(body, 0); /* AuthenticationOk */
    return emit_msg(ctx, 'R', body, 4);
}

int pqwire_send_ready_for_query(pqwire_ctx_t *ctx)
{
    uint8_t body[1] = {'I'}; /* idle */
    if (!ctx || ctx->magic != PQWIRE_MAGIC) {
        return -1;
    }
    ctx->proto_state = PROTO_STATE_READY;
    return emit_msg(ctx, 'Z', body, 1);
}

int pqwire_send_row_description(pqwire_ctx_t *ctx, const pq_column_desc_t *cols, size_t n_cols)
{
    uint8_t buf[2048];
    size_t p = 0;
    size_t i;
    if (!ctx || ctx->magic != PQWIRE_MAGIC || !cols) {
        return -1;
    }
    if (n_cols > PQWIRE_MAX_ROW_COLUMNS) {
        n_cols = PQWIRE_MAX_ROW_COLUMNS;
    }
    wr16_be(buf + p, (uint16_t)n_cols);
    p += 2;
    for (i = 0; i < n_cols; i++) {
        const char *name = cols[i].name ? cols[i].name : "";
        size_t nlen = strlen(name) + 1;
        if (p + nlen + 18 > sizeof(buf)) {
            return -1;
        }
        memcpy(buf + p, name, nlen);
        p += nlen;
        wr32_be(buf + p, 0);      /* table OID */
        p += 4;
        wr16_be(buf + p, 0);      /* column attr */
        p += 2;
        wr32_be(buf + p, cols[i].oid);
        p += 4;
        wr16_be(buf + p, 4);      /* typlen placeholder */
        p += 2;
        wr32_be(buf + p, (uint32_t)-1); /* typmod */
        p += 4;
        wr16_be(buf + p, (uint16_t)cols[i].format);
        p += 2;
    }
    return emit_msg(ctx, 'T', buf, p);
}

int pqwire_send_data_row(pqwire_ctx_t *ctx, const uint8_t *data, size_t len,
                         int16_t format, uint32_t oid)
{
    uint8_t buf[4096];
    size_t p = 0;
    (void)format;
    (void)oid;
    if (!ctx || ctx->magic != PQWIRE_MAGIC) {
        return -1;
    }
    wr16_be(buf + p, 1);
    p += 2;
    if (!data) {
        wr32_be(buf + p, (uint32_t)-1);
        p += 4;
    } else {
        if (p + 4 + len > sizeof(buf)) {
            return -1;
        }
        wr32_be(buf + p, (uint32_t)len);
        p += 4;
        memcpy(buf + p, data, len);
        p += len;
    }
    return emit_msg(ctx, 'D', buf, p);
}

int pqwire_send_data_row_multi(pqwire_ctx_t *ctx,
                               const int32_t *lengths, const uint8_t *const *values,
                               uint16_t n_cols)
{
    size_t cap = 64 + (size_t)n_cols * 256;
    uint8_t *buf;
    size_t p = 0;
    uint16_t i;
    int rc;
    if (!ctx || ctx->magic != PQWIRE_MAGIC) {
        return -1;
    }
    buf = malloc(cap);
    if (!buf) {
        return -1;
    }
    wr16_be(buf + p, n_cols);
    p += 2;
    for (i = 0; i < n_cols; i++) {
        int32_t plen = lengths ? lengths[i] : -1;
        if (p + 4 > cap) {
            free(buf);
            return -1;
        }
        wr32_be(buf + p, (uint32_t)plen);
        p += 4;
        if (plen > 0 && values && values[i]) {
            if (p + (size_t)plen > cap) {
                size_t ncap = p + (size_t)plen + 1024;
                uint8_t *nb = realloc(buf, ncap);
                if (!nb) {
                    free(buf);
                    return -1;
                }
                buf = nb;
                cap = ncap;
            }
            memcpy(buf + p, values[i], (size_t)plen);
            p += (size_t)plen;
        }
    }
    rc = emit_msg(ctx, 'D', buf, p);
    free(buf);
    return rc;
}

int pqwire_send_command_complete(pqwire_ctx_t *ctx, const char *tag)
{
    if (!ctx || ctx->magic != PQWIRE_MAGIC || !tag) {
        return -1;
    }
    return emit_msg(ctx, 'C', (const uint8_t *)tag, strlen(tag) + 1);
}

int pqwire_send_error_response(pqwire_ctx_t *ctx, const char *severity,
                               const char *sqlstate, const char *message)
{
    uint8_t buf[512];
    size_t p = 0;
    if (!ctx || ctx->magic != PQWIRE_MAGIC) {
        return -1;
    }
    if (!severity) {
        severity = "ERROR";
    }
    if (!sqlstate) {
        sqlstate = "XX000";
    }
    if (!message) {
        message = "error";
    }
#define PUT_FIELD(code, str) do { \
    size_t _l = strlen(str) + 1; \
    if (p + 1 + _l >= sizeof(buf)) return -1; \
    buf[p++] = (uint8_t)(code); \
    memcpy(buf + p, (str), _l); \
    p += _l; \
} while (0)
    PUT_FIELD('S', severity);
    PUT_FIELD('C', sqlstate);
    PUT_FIELD('M', message);
#undef PUT_FIELD
    if (p + 1 >= sizeof(buf)) {
        return -1;
    }
    buf[p++] = 0;
    return emit_msg(ctx, 'E', buf, p);
}

int pqwire_send_parse_complete(pqwire_ctx_t *ctx)
{
    if (!ctx || ctx->magic != PQWIRE_MAGIC) {
        return -1;
    }
    return emit_msg(ctx, '1', NULL, 0);
}

int pqwire_send_bind_complete(pqwire_ctx_t *ctx)
{
    if (!ctx || ctx->magic != PQWIRE_MAGIC) {
        return -1;
    }
    return emit_msg(ctx, '2', NULL, 0);
}

int pqwire_send_close_complete(pqwire_ctx_t *ctx)
{
    if (!ctx || ctx->magic != PQWIRE_MAGIC) {
        return -1;
    }
    return emit_msg(ctx, '3', NULL, 0);
}

int pqwire_send_no_data(pqwire_ctx_t *ctx)
{
    if (!ctx || ctx->magic != PQWIRE_MAGIC) {
        return -1;
    }
    return emit_msg(ctx, 'n', NULL, 0);
}

/* ── Mid-pipeline recovery ───────────────────────────────────────────── */

int pqwire_msg_peek(const uint8_t *buf, size_t len, char *type_out, size_t *total_out)
{
    uint32_t mlen;
    size_t total;
    if (!buf || len < 5 || !type_out || !total_out) {
        return -1;
    }
    mlen = ((uint32_t)buf[1] << 24) | ((uint32_t)buf[2] << 16) |
           ((uint32_t)buf[3] << 8) | (uint32_t)buf[4];
    if (mlen < 4) {
        return -1;
    }
    total = 1u + (size_t)mlen;
    if (total > len) {
        return -1; /* incomplete */
    }
    *type_out = (char)buf[0];
    *total_out = total;
    return 0;
}

void pqwire_pipeline_status_init(pqwire_pipeline_status_t *st)
{
    if (!st) {
        return;
    }
    memset(st, 0, sizeof(*st));
}

static void parse_error_fields(const uint8_t *body, size_t body_len, pq_error_t *err)
{
    size_t off = 0;
    if (!body || !err) {
        return;
    }
    memset(err, 0, sizeof(*err));
    while (off < body_len && body[off] != 0) {
        char field = (char)body[off++];
        size_t c = 0;
        const char *val = read_cstr(body + off, body_len - off, &c);
        if (!val) {
            break;
        }
        off += c;
        switch (field) {
        case 'S':
            copy_cstr(err->severity, sizeof(err->severity), val);
            break;
        case 'C':
            copy_cstr(err->code, sizeof(err->code), val);
            break;
        case 'M':
            copy_cstr(err->message, sizeof(err->message), val);
            break;
        case 'D':
            copy_cstr(err->detail, sizeof(err->detail), val);
            break;
        case 'H':
            copy_cstr(err->hint, sizeof(err->hint), val);
            break;
        case 'P':
            err->position = atoi(val);
            break;
        default:
            break;
        }
    }
}

int pqwire_pipeline_observe_msg(pqwire_pipeline_status_t *st,
                                const uint8_t *msg, size_t len)
{
    char type;
    size_t total;
    if (!st || !msg) {
        return -1;
    }
    if (pqwire_msg_peek(msg, len, &type, &total) != 0 || total != len) {
        return -1;
    }
    st->n_msgs++;
    if (type == 'E' && total >= 5) {
        st->saw_error = 1;
        parse_error_fields(msg + 5, total - 5, &st->last_error);
    } else if (type == 'Z') {
        st->saw_rfq = 1;
        st->complete = 1;
        return 1;
    }
    return 0;
}

void pqwire_note_ready(pqwire_ctx_t *ctx)
{
    if (!ctx || ctx->magic != PQWIRE_MAGIC) {
        return;
    }
    ctx->proto_state = PROTO_STATE_READY;
}

int pqwire_pipeline_feed_backend_msg(pqwire_ctx_t *client_role,
                                     pqwire_pipeline_status_t *st,
                                     const uint8_t *msg, size_t len)
{
    protocol_event_t ev;
    int rfq;

    if (!client_role || client_role->magic != PQWIRE_MAGIC || !st || !msg || len == 0) {
        return -1;
    }
    rfq = pqwire_pipeline_observe_msg(st, msg, len);
    if (rfq < 0) {
        return -1;
    }
    (void)pqwire_feed_input(client_role, msg, len);
    while (pqwire_next_event(client_role, &ev) == 1) {
        /* Events already reflected in st via observe; drain for cleanliness. */
        (void)ev;
    }
    if (rfq == 1) {
        pqwire_note_ready(client_role);
    }
    return 0;
}

int pqwire_pipeline_filter_backend_type(char type, int skip_parse_bind_complete)
{
    if (skip_parse_bind_complete && (type == '1' || type == '2')) {
        return 1;
    }
    /* ParameterStatus, NoticeResponse, BackendKeyData */
    if (type == 'S' || type == 'N' || type == 'K') {
        return 1;
    }
    return 0;
}

/* ── param helpers ───────────────────────────────────────────────────── */

void pqwire_param_clear(pqwire_param_t *p)
{
    if (!p) {
        return;
    }
    if (p->owned && p->data) {
        free(p->data);
    }
    p->data = NULL;
    p->length = -1;
    p->owned = 0;
    p->format = 0;
}

int pqwire_param_set_text(pqwire_param_t *p, const char *text)
{
    size_t len;
    uint8_t *d;
    if (!p || !text) {
        return -1;
    }
    pqwire_param_clear(p);
    len = strlen(text);
    d = malloc(len);
    if (!d && len > 0) {
        return -1;
    }
    if (len > 0) {
        memcpy(d, text, len);
    }
    p->data = d;
    p->length = (int32_t)len;
    p->owned = 1;
    p->format = 0;
    return 0;
}

int pqwire_param_set_binary(pqwire_param_t *p, const uint8_t *data, size_t len)
{
    uint8_t *d;
    if (!p) {
        return -1;
    }
    pqwire_param_clear(p);
    if (len > 0 && !data) {
        return -1;
    }
    d = len ? malloc(len) : NULL;
    if (len && !d) {
        return -1;
    }
    if (len) {
        memcpy(d, data, len);
    }
    p->data = d;
    p->length = (int32_t)len;
    p->owned = 1;
    p->format = 1;
    return 0;
}

void pqwire_param_set_null(pqwire_param_t *p)
{
    if (!p) {
        return;
    }
    pqwire_param_clear(p);
    p->length = -1;
    p->format = 0;
}

int pqwire_bind_inject_identity(const pq_bind_t *bind, pqwire_param_t *out_params,
                                uint16_t slot, const char *identity, int as_binary)
{
    uint16_t i;
    if (!bind || !out_params || !identity) {
        return -1;
    }
    if (slot >= bind->n_params && bind->n_params > 0) {
        return -1;
    }
    /* If n_params is 0, allow injecting into a synthetic single-param bind? reject. */
    if (bind->n_params == 0) {
        return -1;
    }
    for (i = 0; i < bind->n_params; i++) {
        const pq_bind_param_view_t *v = &bind->params[i];
        pqwire_param_clear(&out_params[i]);
        if (i == slot) {
            if (as_binary) {
                if (pqwire_param_set_binary(&out_params[i],
                                            (const uint8_t *)identity,
                                            strlen(identity)) != 0) {
                    return -1;
                }
            } else {
                if (pqwire_param_set_text(&out_params[i], identity) != 0) {
                    return -1;
                }
            }
        } else if (v->length < 0) {
            pqwire_param_set_null(&out_params[i]);
            out_params[i].format = v->format;
        } else {
            if (v->format == 1) {
                if (pqwire_param_set_binary(&out_params[i], v->data, (size_t)v->length) != 0) {
                    return -1;
                }
            } else {
                /* text: copy as text (not necessarily NUL-terminated on wire) */
                char *tmp = malloc((size_t)v->length + 1);
                if (!tmp) {
                    return -1;
                }
                memcpy(tmp, v->data, (size_t)v->length);
                tmp[v->length] = '\0';
                if (pqwire_param_set_text(&out_params[i], tmp) != 0) {
                    free(tmp);
                    return -1;
                }
                free(tmp);
            }
        }
    }
    return 0;
}

int pqwire_send_bind_params(pqwire_ctx_t *ctx, const char *portal, const char *statement,
                            const pqwire_param_t *params, uint16_t n_params,
                            const int16_t *result_formats, uint16_t n_result_formats)
{
    int16_t formats[PQWIRE_MAX_BIND_PARAMS];
    int32_t lengths[PQWIRE_MAX_BIND_PARAMS];
    const uint8_t *values[PQWIRE_MAX_BIND_PARAMS];
    uint16_t i;
    if (!params && n_params > 0) {
        return -1;
    }
    if (n_params > PQWIRE_MAX_BIND_PARAMS) {
        return -1;
    }
    for (i = 0; i < n_params; i++) {
        formats[i] = params[i].format;
        lengths[i] = params[i].length;
        values[i] = params[i].data;
    }
    return pqwire_send_bind(ctx, portal, statement,
                            formats, n_params,
                            lengths, values, n_params,
                            result_formats, n_result_formats);
}

int pqwire_bind_rewrite_identity_zerocopy(const pq_bind_t *bind,
                                          const char *portal, const char *statement,
                                          uint16_t slot, const char *identity,
                                          int as_binary,
                                          uint8_t *out, size_t out_cap, size_t *out_len)
{
    size_t p = 0;
    size_t plen_ident;
    uint16_t i;
    uint16_t n_fmt;
    int16_t formats[PQWIRE_MAX_BIND_PARAMS];

    if (!bind || !identity || !out || !out_len) {
        return -1;
    }
    if (bind->n_params == 0 || slot >= bind->n_params) {
        return -1;
    }
    if (!portal) {
        portal = "";
    }
    if (!statement) {
        statement = "";
    }
    plen_ident = strlen(identity);

    /* Estimate: type(1)+len(4)+names+formats+params+result formats */
    {
        size_t need = 5 + strlen(portal) + 1 + strlen(statement) + 1 +
                      2 + (size_t)bind->n_params * 2 +
                      2 + (size_t)bind->n_params * (4 + 64) +
                      2 + (size_t)bind->n_result_formats * 2 + plen_ident + 64;
        if (need > out_cap) {
            /* still try exact build */
        }
    }

#define PUT_BYTES(src, n) do { \
    if (p + (n) > out_cap) return -1; \
    if ((n) > 0) memcpy(out + p, (src), (n)); \
    p += (n); \
} while (0)
#define PUT_U16(v) do { \
    if (p + 2 > out_cap) return -1; \
    wr16_be(out + p, (uint16_t)(v)); \
    p += 2; \
} while (0)
#define PUT_U32(v) do { \
    if (p + 4 > out_cap) return -1; \
    wr32_be(out + p, (uint32_t)(v)); \
    p += 4; \
} while (0)

    /* Leave room for 'B' + int32 len; fill later */
    if (p + 5 > out_cap) {
        return -1;
    }
    p = 5;

    PUT_BYTES(portal, strlen(portal) + 1);
    PUT_BYTES(statement, strlen(statement) + 1);

    /* Per-param formats: prefer one format per param; force identity to text/bin */
    n_fmt = bind->n_params;
    for (i = 0; i < n_fmt; i++) {
        if (i == slot) {
            formats[i] = as_binary ? 1 : 0;
        } else if (bind->n_formats == 0) {
            formats[i] = 0;
        } else if (bind->n_formats == 1) {
            formats[i] = bind->formats[0];
        } else {
            formats[i] = (i < bind->n_formats) ? bind->formats[i] : 0;
        }
    }
    PUT_U16(n_fmt);
    for (i = 0; i < n_fmt; i++) {
        PUT_U16((uint16_t)formats[i]);
    }

    PUT_U16(bind->n_params);
    for (i = 0; i < bind->n_params; i++) {
        if (i == slot) {
            PUT_U32((uint32_t)plen_ident);
            PUT_BYTES(identity, plen_ident);
        } else {
            const pq_bind_param_view_t *v = &bind->params[i];
            if (v->length < 0) {
                PUT_U32(0xFFFFFFFFu); /* -1 NULL */
            } else {
                PUT_U32((uint32_t)v->length);
                PUT_BYTES(v->data, (size_t)v->length);
            }
        }
    }

    PUT_U16(bind->n_result_formats);
    for (i = 0; i < bind->n_result_formats; i++) {
        PUT_U16((uint16_t)bind->result_formats[i]);
    }

    /* Header */
    out[0] = (uint8_t)'B';
    wr32_be(out + 1, (uint32_t)(p - 1)); /* length includes itself, excludes type */
    *out_len = p;
#undef PUT_BYTES
#undef PUT_U16
#undef PUT_U32
    return 0;
}

int pqwire_send_bind_rewrite_identity(pqwire_ctx_t *ctx, const pq_bind_t *bind,
                                      const char *portal, const char *statement,
                                      uint16_t slot, const char *identity,
                                      int as_binary)
{
    uint8_t buf[8192];
    size_t n = 0;
    if (!ctx || ctx->magic != PQWIRE_MAGIC) {
        return -1;
    }
    if (pqwire_bind_rewrite_identity_zerocopy(bind, portal, statement, slot,
                                              identity, as_binary,
                                              buf, sizeof(buf), &n) != 0) {
        return -1;
    }
    append_output(ctx, buf, n);
    return 0;
}

int pqwire_prepared_from_parse(const pq_parse_t *parse, pqwire_prepared_stmt_t *out)
{
    if (!parse || !out) {
        return -1;
    }
    memset(out, 0, sizeof(*out));
    copy_cstr(out->name, sizeof(out->name), parse->statement);
    copy_cstr(out->query, sizeof(out->query), parse->query);
    out->n_param_types = parse->n_param_types;
    if (out->n_param_types > PQWIRE_MAX_PARAM_TYPES) {
        out->n_param_types = PQWIRE_MAX_PARAM_TYPES;
    }
    memcpy(out->param_type_oids, parse->param_type_oids,
           sizeof(uint32_t) * out->n_param_types);
    out->identity_param_slot = -1;
    return 0;
}

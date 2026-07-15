/**
 * Dialectic test: mid-pipeline ErrorResponse → discard until ReadyForQuery.
 */

#include "pqwire.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static size_t write_startup(uint8_t *buf, size_t cap)
{
    size_t p = 4;
    if (cap < 64) {
        return 0;
    }
    buf[p++] = 0;
    buf[p++] = 3;
    buf[p++] = 0;
    buf[p++] = 0;
    memcpy(buf + p, "user", 5);
    p += 5;
    memcpy(buf + p, "u", 2);
    p += 2;
    memcpy(buf + p, "database", 9);
    p += 9;
    memcpy(buf + p, "d", 2);
    p += 2;
    buf[p++] = 0;
    buf[0] = (uint8_t)((p >> 24) & 0xFF);
    buf[1] = (uint8_t)((p >> 16) & 0xFF);
    buf[2] = (uint8_t)((p >> 8) & 0xFF);
    buf[3] = (uint8_t)(p & 0xFF);
    return p;
}

static void test_msg_peek(void)
{
    pqwire_ctx_t *srv = pqwire_create(PQWIRE_ROLE_SERVER);
    uint8_t buf[512];
    size_t n;
    char type;
    size_t total;

    assert(srv);
    assert(pqwire_send_error_response(srv, "ERROR", "23505", "duplicate key") == 0);
    assert(pqwire_send_ready_for_query(srv) == 0);
    n = pqwire_get_output(srv, buf, sizeof(buf));
    assert(n >= 5);
    assert(pqwire_msg_peek(buf, n, &type, &total) == 0);
    assert(type == 'E');
    assert(total >= 5 && total <= n);
    assert(pqwire_msg_peek(buf, 4, &type, &total) != 0); /* incomplete */

    assert(pqwire_pipeline_filter_backend_type('S', 0) == 1);
    assert(pqwire_pipeline_filter_backend_type('1', 1) == 1);
    assert(pqwire_pipeline_filter_backend_type('1', 0) == 0);
    assert(pqwire_pipeline_filter_backend_type('E', 1) == 0);
    assert(pqwire_pipeline_filter_backend_type('Z', 1) == 0);

    pqwire_destroy(srv);
    printf("  PASS: msg_peek + filter\n");
}

static void test_observe_error_then_rfq(void)
{
    pqwire_ctx_t *srv = pqwire_create(PQWIRE_ROLE_SERVER);
    pqwire_pipeline_status_t st;
    uint8_t buf[1024];
    size_t n, off;
    char type;
    size_t total;

    assert(srv);
    pqwire_pipeline_status_init(&st);
    assert(pqwire_send_parse_complete(srv) == 0);
    assert(pqwire_send_error_response(srv, "ERROR", "42P01",
                                      "relation \"missing\" does not exist") == 0);
    assert(pqwire_send_ready_for_query(srv) == 0);
    n = pqwire_get_output(srv, buf, sizeof(buf));
    assert(n > 0);

    off = 0;
    while (off < n) {
        int rc;
        assert(pqwire_msg_peek(buf + off, n - off, &type, &total) == 0);
        rc = pqwire_pipeline_observe_msg(&st, buf + off, total);
        assert(rc >= 0);
        off += total;
        if (rc == 1) {
            break;
        }
    }
    assert(st.saw_error == 1);
    assert(st.saw_rfq == 1);
    assert(st.complete == 1);
    assert(strcmp(st.last_error.code, "42P01") == 0);
    assert(strstr(st.last_error.message, "missing") != NULL);

    pqwire_destroy(srv);
    printf("  PASS: observe ErrorResponse then RFQ\n");
}

static void test_feed_backend_marks_ready(void)
{
    pqwire_ctx_t *client = pqwire_create(PQWIRE_ROLE_CLIENT);
    pqwire_ctx_t *server = pqwire_create(PQWIRE_ROLE_SERVER);
    pqwire_pipeline_status_t st;
    uint8_t startup[64], buf[1024];
    size_t sn, n, off;
    protocol_event_t ev;
    char type;
    size_t total;

    assert(client && server);
    sn = write_startup(startup, sizeof(startup));
    assert(sn > 0);
    /* Drive server past startup so it can emit RFQ; client role feeds responses. */
    pqwire_feed_input(server, startup, sn);
    while (pqwire_next_event(server, &ev) == 1) {
    }

    pqwire_pipeline_status_init(&st);
    assert(pqwire_send_error_response(server, "ERROR", "57014", "canceling") == 0);
    assert(pqwire_send_ready_for_query(server) == 0);
    n = pqwire_get_output(server, buf, sizeof(buf));
    assert(n > 0);

    off = 0;
    while (off < n) {
        assert(pqwire_msg_peek(buf + off, n - off, &type, &total) == 0);
        assert(pqwire_pipeline_feed_backend_msg(client, &st, buf + off, total) == 0);
        off += total;
    }
    assert(st.saw_error == 1);
    assert(st.complete == 1);
    /* READY = 2 in internal enum; current_state returns proto_state int */
    assert(pqwire_current_state(client) == 2); /* PROTO_STATE_READY */

    pqwire_destroy(client);
    pqwire_destroy(server);
    printf("  PASS: feed_backend_msg notes READY\n");
}

int main(void)
{
    test_msg_peek();
    test_observe_error_then_rfq();
    test_feed_backend_marks_ready();
    printf("pipeline recover tests PASSED\n");
    return 0;
}

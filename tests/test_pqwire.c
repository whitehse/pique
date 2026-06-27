#include "pqwire.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <arpa/inet.h>

int main(void) {
    pqwire_ctx_t *client = pqwire_create(PQWIRE_ROLE_CLIENT);
    pqwire_ctx_t *server = pqwire_create(PQWIRE_ROLE_SERVER);
    assert(client && server);

    uint8_t wire_buf[4096];

    /* 1. Client sends StartupMessage */
    assert(pqwire_send_startup(client, "testuser", "testdb") == 0);
    size_t n = pqwire_get_output(client, wire_buf, sizeof(wire_buf));
    pqwire_feed_input(server, wire_buf, n);

    protocol_event_t ev;
    assert(pqwire_next_event(server, &ev) == 1);
    assert(ev.type == PQ_EVENT_STARTUP);

    assert(pqwire_send_auth_ok(server) == 0);
    assert(pqwire_send_ready_for_query(server) == 0);

    n = pqwire_get_output(server, wire_buf, sizeof(wire_buf));
    pqwire_feed_input(client, wire_buf, n);

    assert(pqwire_next_event(client, &ev) == 1);

    /* 2. Client sends query */
    assert(pqwire_send_query(client, "SELECT 1") == 0);
    n = pqwire_get_output(client, wire_buf, sizeof(wire_buf));
    pqwire_feed_input(server, wire_buf, n);

    assert(pqwire_next_event(server, &ev) == 1);
    assert(ev.type == PQ_EVENT_QUERY);

    /* Server responds with RowDescription + DataRow */
    pq_column_desc_t col = { .name = "result", .oid = 23, .format = 1 };
    assert(pqwire_send_row_description(server, &col, 1) == 0);

    uint32_t val = htonl(42);
    assert(pqwire_send_data_row(server, (const uint8_t*)&val, sizeof(val), 1, 23) == 0);
    assert(pqwire_send_command_complete(server, "SELECT 1") == 0);
    assert(pqwire_send_ready_for_query(server) == 0);

    n = pqwire_get_output(server, wire_buf, sizeof(wire_buf));
    pqwire_feed_input(client, wire_buf, n);

    int got_row_desc = 0, got_data_row = 0;
    while (pqwire_next_event(client, &ev)) {
        if (ev.type == PQ_EVENT_ROW_DESCRIPTION) got_row_desc = 1;
        if (ev.type == PQ_EVENT_DATA_ROW) {
            got_data_row = 1;
            printf("Received row with %zu column(s)\n", ev.payload.data_row.n_columns);
        }
    }

    assert(got_row_desc && got_data_row);
    printf("Dialectic test PASSED: multi-column DataRow event\n");

    pqwire_destroy(client);
    pqwire_destroy(server);
    return 0;
}

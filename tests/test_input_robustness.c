#include "pqwire.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <arpa/inet.h>

int main(void) {
    pqwire_config_t cfg = {
        .event_queue_size = 8,
        .max_message_size = 1024,   /* small limit for testing */
        .password = NULL,
        .use_scram = 0
    };

    pqwire_ctx_t *ctx = pqwire_create_with_config(PQWIRE_ROLE_CLIENT, &cfg);
    assert(ctx != NULL);

    /* Send a clearly oversized message (length = 10000 > limit) */
    uint8_t bad_msg[16];
    uint32_t fake_len = htonl(10000);
    memcpy(bad_msg, &fake_len, 4);
    memcpy(bad_msg + 4, "QSELECT 1", 9);

    size_t consumed = pqwire_feed_input(ctx, bad_msg, sizeof(bad_msg));

    protocol_event_t ev;
    int got_error = 0;
    while (pqwire_next_event(ctx, &ev)) {
        if (ev.type == PQ_EVENT_ERROR) {
            got_error = 1;
        }
    }

    assert(got_error == 1);
    printf("Input robustness test PASSED (oversized message rejected)\n");

    pqwire_destroy(ctx);
    return 0;
}
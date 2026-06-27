#include "pqwire.h"
#include <stdint.h>
#include <stdlib.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    pqwire_config_t cfg = {
        .event_queue_size = 16,
        .max_message_size = 1024 * 1024,   // 1 MiB limit during fuzzing
        .password = NULL,
        .use_scram = 0
    };

    pqwire_ctx_t *ctx = pqwire_create_with_config(PQWIRE_ROLE_CLIENT, &cfg);
    if (!ctx) return 0;

    // Feed the fuzzer-generated data into the library
    pqwire_feed_input(ctx, data, size);

    // Drain all events to exercise as much code as possible
    protocol_event_t ev;
    while (pqwire_next_event(ctx, &ev)) {
        // Consume events (the fuzzer cares about crashes, not return values)
    }

    pqwire_destroy(ctx);
    return 0;
}
#include "pqwire.h"
#include "mitm.h"
#include <stdint.h>
#include <stdlib.h>

/*
 * MITM-based adversarial fuzzer for libpqwire.
 * This fuzzer creates a client and server, then uses the MITM
 * to mutate traffic between them.
 */
int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (size == 0) return 0;

    pqwire_config_t cfg = {
        .event_queue_size = 32,
        .max_message_size = 2 * 1024 * 1024,
        .password = NULL,
        .use_scram = 0
    };

    pqwire_ctx_t *client = pqwire_create_with_config(PQWIRE_ROLE_CLIENT, &cfg);
    pqwire_ctx_t *server = pqwire_create_with_config(PQWIRE_ROLE_SERVER, &cfg);

    if (!client || !server) {
        if (client) pqwire_destroy(client);
        if (server) pqwire_destroy(server);
        return 0;
    }

    mitm_ctx_t *mitm = mitm_create(client, server);
    if (!mitm) {
        pqwire_destroy(client);
        pqwire_destroy(server);
        return 0;
    }

    // Enable some aggressive attacks
    mitm_enable_attack(mitm, MITM_ATTACK_DROP_MESSAGE);
    mitm_enable_attack(mitm, MITM_ATTACK_CORRUPT_LENGTH);
    mitm_enable_attack(mitm, MITM_ATTACK_FLIP_BITS);

    // Feed initial data from fuzzer into the client
    pqwire_feed_input(client, data, size);

    // Run several MITM steps to exercise mutation paths
    for (int i = 0; i < 8; i++) {
        mitm_step(mitm);
    }

    // Drain events from both sides
    protocol_event_t ev;
    while (pqwire_next_event(client, &ev)) {}
    while (pqwire_next_event(server, &ev)) {}

    mitm_destroy(mitm);
    pqwire_destroy(client);
    pqwire_destroy(server);

    return 0;
}
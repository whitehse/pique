#include "mitm.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

struct mitm_ctx {
    pqwire_ctx_t *client;
    pqwire_ctx_t *server;
    int enabled_attacks[16];
    int attack_count;
};

mitm_ctx_t *mitm_create(pqwire_ctx_t *client, pqwire_ctx_t *server) {
    if (!client || !server) return NULL;

    mitm_ctx_t *mitm = calloc(1, sizeof(*mitm));
    if (!mitm) return NULL;

    mitm->client = client;
    mitm->server = server;
    mitm->attack_count = 0;

    return mitm;
}

void mitm_destroy(mitm_ctx_t *mitm) {
    if (mitm) free(mitm);
}

int mitm_step(mitm_ctx_t *mitm) {
    if (!mitm) return -1;

    uint8_t buf[4096];
    size_t n;

    /* Move data client -> server */
    n = pqwire_get_output(mitm->client, buf, sizeof(buf));
    if (n > 0) {
        /* TODO: Apply mutations based on enabled attacks */
        pqwire_feed_input(mitm->server, buf, n);
    }

    /* Move data server -> client */
    n = pqwire_get_output(mitm->server, buf, sizeof(buf));
    if (n > 0) {
        /* TODO: Apply mutations based on enabled attacks */
        pqwire_feed_input(mitm->client, buf, n);
    }

    return 0;
}

void mitm_enable_attack(mitm_ctx_t *mitm, int attack_id) {
    if (!mitm || mitm->attack_count >= 16) return;
    mitm->enabled_attacks[mitm->attack_count++] = attack_id;
}

void mitm_disable_attack(mitm_ctx_t *mitm, int attack_id) {
    if (!mitm) return;
    for (int i = 0; i < mitm->attack_count; i++) {
        if (mitm->enabled_attacks[i] == attack_id) {
            /* Simple removal by shifting */
            for (int j = i; j < mitm->attack_count - 1; j++) {
                mitm->enabled_attacks[j] = mitm->enabled_attacks[j + 1];
            }
            mitm->attack_count--;
            break;
        }
    }
}
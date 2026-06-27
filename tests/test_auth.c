#include "pqwire.h"
#include <assert.h>
#include <stdio.h>

int main(void) {
    /* Client with password */
    pqwire_config_t client_cfg = {
        .event_queue_size = 8,
        .password = "secret123",
        .use_scram = 1
    };
    pqwire_ctx_t *client = pqwire_create_with_config(PQWIRE_ROLE_CLIENT, &client_cfg);
    assert(client != NULL);

    /* Server with stored SCRAM hash (simplified) */
    pqwire_config_t server_cfg = {
        .event_queue_size = 8,
        .password = "$scram$...",   /* would be real SCRAM hash in production */
        .use_scram = 1
    };
    pqwire_ctx_t *server = pqwire_create_with_config(PQWIRE_ROLE_SERVER, &server_cfg);
    assert(server != NULL);

    /* Start authentication */
    assert(pqwire_start_auth(client) == 0);

    printf("Authentication initialization test PASSED\n");

    pqwire_destroy(client);
    pqwire_destroy(server);
    return 0;
}
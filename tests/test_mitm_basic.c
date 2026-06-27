#include "pqwire.h"
#include "mitm.h"
#include <assert.h>
#include <stdio.h>

int main(void) {
    pqwire_ctx_t *client = pqwire_create(PQWIRE_ROLE_CLIENT);
    pqwire_ctx_t *server = pqwire_create(PQWIRE_ROLE_SERVER);

    mitm_ctx_t *mitm = mitm_create(client, server);
    assert(mitm != NULL);

    /* Basic smoke test */
    mitm_enable_attack(mitm, MITM_ATTACK_DROP_MESSAGE);
    int ret = mitm_step(mitm);
    assert(ret == 0);

    mitm_destroy(mitm);
    pqwire_destroy(client);
    pqwire_destroy(server);

    printf("MITM basic test PASSED\n");
    return 0;
}
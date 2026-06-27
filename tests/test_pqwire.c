#include "pqwire.h"
#include <assert.h>
#include <stdio.h>

int main(void) {
    pqwire_ctx_t *client = pqwire_create(PQWIRE_ROLE_CLIENT);
    assert(client != NULL);
    pqwire_destroy(client);

    printf("Basic create/destroy test passed.\n");
    return 0;
}

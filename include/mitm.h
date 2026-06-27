#ifndef MITM_H
#define MITM_H

#include "pqwire.h"

/* Nefarious Man-in-the-Middle testing harness */

typedef struct mitm_ctx mitm_ctx_t;

/* Create a MITM between client and server contexts */
mitm_ctx_t *mitm_create(pqwire_ctx_t *client, pqwire_ctx_t *server);

/* Destroy the MITM */
void mitm_destroy(mitm_ctx_t *mitm);

/* Run one step of MITM processing (move data between client and server with possible mutation) */
int mitm_step(mitm_ctx_t *mitm);

/* Enable/disable specific attack strategies */
void mitm_enable_attack(mitm_ctx_t *mitm, int attack_id);
void mitm_disable_attack(mitm_ctx_t *mitm, int attack_id);

/* Built-in attack strategies */
#define MITM_ATTACK_DROP_MESSAGE      1
#define MITM_ATTACK_CORRUPT_LENGTH    2
#define MITM_ATTACK_FLIP_BITS         3
#define MITM_ATTACK_REORDER_MESSAGES  4
#define MITM_ATTACK_INJECT_GARBAGE    5

#endif /* MITM_H */
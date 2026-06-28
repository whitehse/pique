#define _GNU_SOURCE
/*
 * SPDX-License-Identifier: CC0-1.0
 * rtos_pqwire.c — Illustrative skeleton for RTOS / real-time integration.
 *
 * NOTE: This file will NOT compile or run on a standard Linux host without
 * an RTOS SDK (FreeRTOS, Zephyr, etc.). Provided only to document the
 * pattern required by ADR 012.
 *
 * Emphasis: bounded WCET, no unbounded allocations or loops in library calls,
 * re-entrancy for ISRs or high-priority tasks, priority-aware usage at app level.
 */

#include <stdio.h>
#include <string.h>
#include "pqwire.h"

/* RTOS headers (not on host) */
#if defined(FREERTOS) || defined(ZEPHYR) || defined(RTOS_EXAMPLE)
#include "freertos/FreeRTOS.h" /* or zephyr/kernel.h etc. */
#else
#error "This example requires an RTOS environment (see ADR 012)."
#endif

#define BUF_SIZE 8192

static void pqwire_rtos_task(void *arg) {
    pqwire_ctx_t *ctx = (pqwire_ctx_t *)arg;
    uint8_t in[BUF_SIZE], out[BUF_SIZE];

    for (;;) {
        /* Receive from RTOS queue / ISR-safe buffer */
        /* ... */

        size_t consumed = pqwire_feed_input(ctx, in, /*len*/0);
        (void)consumed;

        protocol_event_t ev;
        while (pqwire_next_event(ctx, &ev) == 1) {
            /* process; must be bounded time */
        }

        size_t n = pqwire_get_output(ctx, out, sizeof(out));
        if (n > 0) {
            /* transmit; respect RT priorities */
        }

        /* RTOS yield / delay until next tick or event */
    }
}

#define _GNU_SOURCE
/*
 * SPDX-License-Identifier: CC0-1.0
 * coroutine_pqwire.c — Illustrative skeleton for coroutine / cooperative multitasking.
 *
 * NOTE: This file will NOT compile or run on a standard Linux host without a
 * coroutine library (e.g. libcoro, or manual ucontext/setjmp). Provided only
 * to document the pattern mandated by ADR 012.
 *
 * Key requirement: the state machine must be yield-friendly and re-entrant.
 * All entry points (feed_input, next_event, get_output) are expected to be
 * safe to call from within a coroutine and to return quickly for yielding.
 */

#include <stdio.h>
#include <string.h>
#include "pqwire.h"

/* Coroutine primitives would come from the chosen library */
#if defined(CORO_EXAMPLE)
#include "coro.h"   /* placeholder */
#else
#error "This example requires a coroutine runtime (see ADR 012)."
#endif

#define BUF_SIZE 8192

static void pqwire_coroutine(void *arg) {
    pqwire_ctx_t *ctx = (pqwire_ctx_t *)arg;
    uint8_t in[BUF_SIZE], out[BUF_SIZE];
    size_t len;

    /* Example yield-friendly loop: feed, process events, get output, yield */
    /* ... coroutine yield points here ... */
    pqwire_feed_input(ctx, in, len);

    protocol_event_t ev;
    while (pqwire_next_event(ctx, &ev) == 1) {
        /* handle event; yield if needed */
    }

    size_t n = pqwire_get_output(ctx, out, sizeof(out));
    if (n > 0) {
        /* send output; yield */
    }
}

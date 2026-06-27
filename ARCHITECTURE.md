# ARCHITECTURE.md — libpqwire

## Core Principle

This is a **pure state machine** implementation of the PostgreSQL wire protocol (frontend/backend messaging), with support for binary data transfer including PostGIS geospatial types. No function pointers for callbacks, no direct system calls, no hidden allocations beyond what's explicitly requested via config. The design follows the "plumbing" philosophy (ADR 006): the library is a thin PDU parser/serializer that emits structured events and unencapsulated data; the calling application owns all policy, decisions, and side effects. Data is passed via memory buffers; the library has no knowledge of io_uring, libuv, libev, or any event loop.

## Module Boundaries

- `include/pqwire.h` + `src/pqwire.c` — Core PostgreSQL wire protocol state machine (client and server roles). Opaque context, buffer-in/buffer-out.
- `include/protocol_events.h` — Shared event structures (`protocol_event_t`) used for explicit, queueable output.
- Future: separate modules if needed for extensions, but core remains focused on wire protocol.
- All modules follow identical patterns: `*_create` / `*_create_with_config`, `*_feed_input`, `*_next_event`, `*_get_output`, `*_destroy`, `*_reset`.

## Configuration and Initialization (ADR 006 / User Preference)

- Libraries use config structs at creation time for configurability (e.g., `pqwire_config_t { int event_queue_size; }`).
- Default constructors (`pqwire_create`) use sensible defaults (queue size 8).
- `*_create_with_config` returns NULL on invalid config (queue_size <= 0) or allocation failure — caller must check.
- This enables event queue sizing, future allocator hooks, etc., without hard-coded defaults inside the library.

## Event-Driven Plumbing Model (ADR 006)

- Primary output mechanism: `int *_next_event(ctx, protocol_event_t *event)` — returns 1 if an event was dequeued and filled, 0 otherwise.
- Events deliver unencapsulated data (e.g., row data with type OIDs, binary payloads for geometry, etc.).
- No auto-responses or policy decisions inside the library. Parsed data is returned raw for the caller to act upon.
- Send helpers queue messages for output; they return 0 on success, -1 on error.
- `*_get_output` returns bytes copied (0 if none).
- `*_feed_input` returns bytes consumed; internally enqueues protocol events.

## Invariants

- The library never blocks, never calls `read`/`write`/`socket` (malloc/realloc used internally only where unavoidable for correctness; no I/O syscalls).
- All I/O and event loop integration is the responsibility of the caller. Caller feeds contiguous byte buffers into `*_feed_input` and drains via `*_get_output`.
- State transitions are deterministic. Error handling via events or state queries.
- Dialectic testing: all tests use paired client/server contexts exchanging buffers in-memory.
- Binary format support: library understands PostgreSQL binary row formats, type OIDs (including PostGIS geometry OID), and provides typed access or raw binary for application to decode (e.g., via PostGIS or liblwgeom).

## Deliberate Absences (by design)

- No TLS/crypto handling (caller manages if needed via STARTTLS or external).
- No dynamic policy, authentication logic, or query execution — pure wire plumbing.
- No callbacks — progress is always pull-driven via next_event / state queries.
- No knowledge of specific event loops or syscalls.

## Entry Points (Common Across Modules)

- Creation: `*_create(role)`, `*_create_with_config(role, config)` → ctx or NULL
- Teardown: `*_destroy(ctx)`, `*_reset(ctx)`
- I/O: `size_t *_feed_input(ctx, data, len)`, `size_t *_get_output(ctx, buf, max_len)`
- Events: `int *_next_event(ctx, protocol_event_t *event)` → 1/0
- State: `*_current_state(ctx)`
- Protocol-specific sends and helpers for startup, query, parse, bind, execute, row descriptions, data rows (text/binary), etc.

## How the State Machine Works

Each module maintains explicit states for frontend/backend messaging (Startup, Authentication, Query, Parse/Bind/Execute, Copy, etc.). Input bytes are parsed into messages, events enqueued (e.g., ROW_DESCRIPTION, DATA_ROW with binary payload), output messages serialized on send calls. Caller drives: feed → next_event loop until no more events, drain output, repeat. Supports edge-triggered and completion-based loops.

## Binary Data and Type Handling

- Supports both text and binary formats for DataRow messages.
- Maintains knowledge of common PostgreSQL types (int2, int4, int8, float4, float8, text, bytea, timestamp, etc.) and their OIDs.
- For binary rows: provides access to raw binary data per column, with OID information so caller (or extensions) can decode, including PostGIS geometry (typically OID 17001 or registered).
- No embedded PostGIS library; raw binary + type metadata emitted for application to process (e.g., via WKB parsing or PostGIS).

## Future Growth

- Full message coverage, error handling, prepared statements, COPY protocol, notifications.
- Configurable type OID registry or hooks for custom types including PostGIS variants.
- Event queue backpressure handling, more config options.
- When core is extended, address edge cases with documentation and manpage updates.

## Documentation and Manpages

See `docs/` and `man/man3/` (installed as section 3 manpages) for C API details, return codes, and calling conventions. All public functions document NULL checks, return values (0 success / -1 error / bytes / 1 event), and event semantics.

This architecture guarantees pure byte-buffer testability, maximum reusability as plumbing, and strict adherence to no-syscall / no-callback rules. Strict module separation maintained; new protocol features added via extension of core or sibling patterns if needed.

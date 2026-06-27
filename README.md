# libpqwire

A pure C, system-call-free, callback-free state machine implementation of the PostgreSQL wire protocol.

## Overview

`libpqwire` is a thin "plumbing" library for the PostgreSQL wire protocol (v3). It parses and serializes protocol messages but makes no decisions and performs no I/O. All networking and policy decisions are left to the calling application.

The library is designed to be driven by any event loop (io_uring, libuv, libev, epoll, etc.) and supports both client and server roles.

## Features

- **Pure C11** — No C++ features, minimal dependencies
- **System-call free** — No sockets, no blocking, no hidden allocations
- **Callback free** — Progress is driven via `feed_input` / `next_event`
- **Binary row support** — Multi-column `DataRow` with NULL handling
- **SCRAM-SHA-256 authentication** — Vendored minimal implementation
- **State machine guards** — Protocol violation detection
- **Configurable initialization** — Password, max message size, event queue size
- **Extended query protocol scaffolding** — Parse / Bind / Execute / Sync
- **Stored procedure support** — `CALL` statements and prepared statements
- **Agent-ready** — Full documentation scaffold (`AGENTS.md`, `ARCHITECTURE.md`, ADRs)

## Licensing

- **Core library** (`src/`, `include/`, build system, documentation): **MIT License**
- **Example programs** (`examples/`): **CC0 1.0 Universal** (public domain)

See `LICENSE` for the full MIT text.

## Building

```bash
mkdir build
cd build
cmake ..
cmake --build .
```

### Running Tests

```bash
ctest --test-dir build --output-on-failure
```

## Basic Usage (Client)

```c
#include "pqwire.h"

pqwire_config_t cfg = {
    .event_queue_size = 16,
    .password = "mysecret",
    .use_scram = 1,
    .max_message_size = 16 * 1024 * 1024
};

pqwire_ctx_t *ctx = pqwire_create_with_config(PQWIRE_ROLE_CLIENT, &cfg);

// Send startup
pqwire_send_startup(ctx, "myuser", "mydb");

// Feed network data and process events...
```

See `tests/` for complete examples of client/server dialectic testing.

## Documentation

- `AGENTS.md` — Agent entry point and build commands
- `ARCHITECTURE.md` — Architecture, invariants, and design decisions
- `docs/decisions/` — Architecture Decision Records (ADRs)

## Contributing

This project follows a strict "core as plumbing" philosophy. All changes must:

- Remain system-call free and callback free
- Pass `ctest`
- Update relevant documentation when architecture changes

See `AGENTS.md` for the full contribution guidelines.

## Status

This is an early but functional implementation focused on robustness, binary data handling, and long-lived connections. Further work is planned for full extended query protocol support and richer error handling.
# libpqwire

Pure C state-machine PostgreSQL wire protocol library.

- System-call free
- Callback free
- Designed for callers that own the event loop (io_uring, libuv, libev, etc.)
- Agent-ready from day one (AGENTS.md + full scaffold)
- Supports binary row data including PostGIS geospatial types

See AGENTS.md for how to build, test, and contribute with coding agents.

## Licensing

- **Core library** (`src/`, `include/`, build system, documentation): MIT License (see `LICENSE`)
- **Example programs** (`examples/`): CC0 1.0 Universal (public domain) — see `examples/LICENSE`

This allows maximum freedom when copying the example harnesses into other projects.

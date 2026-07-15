# ADR 013: Embedded Lua Scripting for Policy and Automation

**Date**: 2026-06-30
**Status**: Accepted
**Deciders**: Project maintainers (Hermes agent + human review)

## Context

libpqwire exposes PostgreSQL wire protocol operations as a pure C state machine (ADR 006). The calling application drives the library through `pqwire_feed_input()`, `pqwire_next_event()`, and `pqwire_get_output()` with no system calls, no callbacks, and no embedded I/O inside the library. The library produces structured events — authentication challenges, parameter status messages, ready-for-query, row descriptions, data rows, command completion, error responses, notifications — and consumes output buffers. All policy and decision-making lives in the application.

This separation is powerful for embedding, testing, and C integration (ADR 010). However, deploying a functional PostgreSQL client requires substantial application-level logic: connection state management, authentication flow handling, query routing, result-set processing, prepared-statement lifecycle, listen/notify dispatch, copy-mode management, and error recovery. Writing this in C is verbose, slow to iterate, and difficult to modify at runtime.

Lua is the dominant embedded scripting language for exactly this class of problem: small, fast, coroutine-native, system-call-free when sandboxed, and designed for embedding in C host applications.

## Decision

libpqwire shall support integration with an embedded Lua scripting layer for policy and automation. The following design principles shall govern this integration:

### Core as Plumbing Remains

The library itself shall **not** embed Lua. Its responsibilities remain exactly as specified in ADR 006: parse/serialize PostgreSQL wire-protocol messages, emit structured events, and accept output buffers. The library exposes its state and domain objects through a well-defined introspection and mutation API. The calling application wires this API to a Lua virtual machine.

### Domain Objects Exposed to Lua

The following libpqwire objects and data shall be inspectable and manipulatable from Lua scripts through the calling application's bridge layer:

- **Connections**: connection state, backend PID, secret key, transaction status, protocol version
- **Authentication**: auth type (none, cleartext, MD5, SASL), challenge data, completion state
- **Queries**: SQL text, parameter values, parameter formats, result format (text/binary)
- **Result sets**: row description (column names, types, OIDs), data rows (field values), row count
- **Prepared statements**: statement name, parameter types, result description
- **Command completion**: command tag (INSERT, SELECT, etc.), affected row count
- **Error responses**: severity, SQLSTATE code, message, detail, hint, position
- **Notifications**: channel name, payload, backend PID (LISTEN/NOTIFY)
- **Parameter status**: name, value (server_version, client_encoding, etc.)
- **Copy mode**: copy-in/copy-out state, format, column definitions

The library provides the data. Lua provides the policy.

### Coroutine-Based Non-Blocking Execution

Lua coroutines shall map naturally to libpqwire's event-driven model. A Lua script may yield at event boundaries — after `pqwire_next_event()` delivers an event, or after `pqwire_feed_input()` completes a data injection — and resume when new data arrives from the library. No blocking shall occur. No OS threads shall be introduced. The calling application drives coroutine resumption from the same event loop that drives `pqwire_feed_input()` and `pqwire_next_event()`.

### Event Loop and io_uring Compatibility

The Lua integration layer shall work within an unspecified event loop (ADR 002) or io_uring (ADR 012). Lua coroutine scheduling shall be driven by the same event loop that drives libpqwire's state machine. The integration introduces no additional I/O multiplexing, no threading, and no blocking operations.

### Minimal System Calls

Both the library and the Lua integration layer shall avoid system calls. Pure computation, buffer manipulation, and state transitions only. I/O boundaries remain at the application level, exactly as specified in AGENTS.md.

### AI Harness Integration

This pattern serves a long-term vision. A future AI agent harness will use libpqwire as a building block. Lua serves as the deterministic control plane: operators program business logic, safety constraints, query policies, data-access guardrails, and operational rules in Lua scripts. The AI agent can read and modify Lua state to influence database interaction behavior — within bounds the Lua scripts themselves enforce.

### Design for Sibling Libraries

This pattern applies across all sibling libraries (libhttp2, libnetconf, librest, libyaml, libdiscord, libslack). Each library shall expose its domain objects to Lua through a consistent interface shape. Together, they form a programmable, testable, AI-accessible substrate for protocol automation.

## Rationale

- Lua was purpose-built for embedding in C. The Lua VM is ~200KB, system-call-free when sandboxed, and supports coroutines natively.
- Keeping Lua out of the core library preserves ADR 006 purity: libpqwire remains protocol plumbing that can be tested, fuzzed, and embedded without Lua as a dependency.
- Exposing domain objects (rather than raw byte buffers) to Lua gives scripts structured, validated data to work with, while preserving the library's role as the source of truth.
- Coroutine yield/resume at event boundaries matches libpqwire's pull-based event model exactly: no polling, no busy-waiting, no thread synchronization.
- The AI harness integration is speculative but the Lua control plane is valuable on its own regardless of AI involvement.

## Consequences

- libpqwire shall define a clear C API for querying connection, authentication, query, result-set, prepared-statement, error, notification, parameter, and copy state from the library's internal representation.
- The calling application (not the library) shall own the Lua VM, the coroutine scheduler, and the bridge code that maps C API calls to Lua tables and userdata.
- Documentation shall include reference examples showing Lua scripts consuming libpqwire events through a host bridge — not linked into the library itself.
- Future ADRs or PRs that add new domain objects or events must consider Lua-side inspectability as a design goal.
- `AGENTS.md` and `ARCHITECTURE.md` must document the Lua integration boundary as an architectural concern.
- Protocol event structs shall remain trivially bridgeable to Lua userdata or tables, consistent with sibling library conventions.

## Verification

- The library's own test suite continues to pass without Lua as a build dependency.
- Reference bridge example (in the application or `examples/`) shall demonstrate: receiving a `protocol_event_t` in C, pushing relevant fields into a Lua table, resuming a Lua coroutine, and collecting an action decision (e.g., issue a follow-up query, retry authentication, dispatch a notification).
- Valgrind tests verify no leaks when creating and destroying a Lua VM bridge alongside a `pqwire_ctx_t`.
- Coroutine scheduling shall be exercised: a Lua script that yields on row data events and resumes when query completion arrives, producing follow-up output through the library without blocking.

This decision establishes libpqwire as the data source and liblua as the policy brain — a clear, testable boundary between protocol mechanics and application logic.

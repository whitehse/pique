# Documentation Index — libpqwire

This directory contains layered documentation for humans and coding agents.

- [DOMAIN.md](DOMAIN.md) — PostgreSQL wire protocol domain glossary, workflows, and terminology.
- [decisions/](decisions/) — Architecture Decision Records (ADRs) recording major choices.
  - 001-agent-ready-documentation.md — Why this repo adopted the agent-ready scaffold.
  - 002-event-loop-compatibility.md — Requirement to remain compatible with io_uring, libev, libuv, epoll.
  - 003-testing-fuzzing-valgrind.md — Mandatory testing, fuzzing, and Valgrind policy.
  - 004-dialectic-client-server-testing.md — Client + server symmetry and paired in-memory testing.
  - 006-core-library-as-plumbing-pdu-stack.md — Strict plumbing / PDU parser philosophy.
  - 007-documentation-and-manpage-updates.md — Documentation must be updated with every API change.
  - 008-c-only-examples-and-codebase.md — Strict C11 requirement (no C++).
  - 009-consistent-protocol-interfaces.md — Unified event-driven interface pattern.
  - 010-c-interfaces-and-language-bindings.md — Opaque types + FFI-friendly design principles.
  - 011-nefarious-mitm-adversarial-testing.md — Nefarious Man-in-the-Middle adversarial testing harness.
  - 012-extended-event-loop-and-real-time-compatibility.md — Extended compatibility for ESP-IDF, coroutines, and real-time/RTOS environments (extends ADR 002).

See also root-level AGENTS.md and ARCHITECTURE.md for entry points and codemap.
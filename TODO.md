# TODO.md — pique (libpqwire)

## App-driven (pqproxy L7 mTLS identity proxy)

- [x] Extended Query frontend events: PARSE, BIND, DESCRIBE, EXECUTE, CLOSE, SYNC, FLUSH
- [x] Extended Query send helpers (parse/bind/execute/sync/close/describe/flush)
- [x] Unnamed pipeline helper `pqwire_send_unnamed_pipeline`
- [x] Bind parameter ownership helpers + `pqwire_bind_inject_identity`
- [x] `pqwire_prepared_stmt_t` caller-owned statement cache shape
- [x] Server send helpers used by dialectic tests (auth_ok, RFQ, row desc, data row, command complete)
- [ ] Per-connection statement/portal name tables inside library (optional; proxy may keep its own)
- [ ] Zero-copy Bind rewrite without full re-serialize (slice-based output)
- [ ] COPY protocol full duplex for bulk paths
- [ ] Dialectic test: Parse intercept → ParseComplete local → Bind inject → unnamed pipeline
- [x] ErrorResponse mid-pipeline recovery helpers (`pqwire_pipeline_status_t`, observe/feed, note_ready)
- [ ] Document dual-context proxy pattern (frontend SERVER + backend CLIENT) in ARCHITECTURE.md

## Core robustness

- [ ] Complete SCRAM server-side verification path
- [ ] Startup parameter table (application_name, etc.)
- [ ] Fuzz corpus for extended query frames
- [ ] Wire up remaining tests in CMakeLists (`test_auth`, `test_input_robustness`, `test_mitm_basic`)

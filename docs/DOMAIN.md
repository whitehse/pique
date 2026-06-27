# DOMAIN.md — libpqwire

## PostgreSQL Wire Protocol Overview

The PostgreSQL wire protocol is the message-based protocol used between frontend (client) and backend (server) for communication over TCP or Unix sockets. It supports startup, authentication, simple and extended query protocols, COPY, notifications, etc. Messages are length-prefixed with a type byte (or ' ' for some).

Key phases:
- Startup: SSL request, StartupMessage with user/db/parameters.
- Authentication: various methods (cleartext, MD5, SASL, etc.).
- Query: Simple Query or Parse/Bind/Describe/Execute/Sync.
- Data transfer: RowDescription + DataRow (text or binary format).
- Binary format: uses network byte order, type-specific encoding (e.g., int32 as 4 bytes big-endian).

## Terminology

- Frontend / Backend messages
- StartupMessage, AuthenticationOk/Request, PasswordMessage, Query, Parse, Bind, Execute, Sync, ReadyForQuery, RowDescription, DataRow, CommandComplete, ErrorResponse, NoticeResponse, etc.
- OID: Object Identifier for data types (e.g., 23 for int4, 17001 or custom for geometry).
- Format codes: 0=text, 1=binary per column or overall.
- PostGIS: Extends PostgreSQL with geospatial types; geometry stored as binary (WKB or EWKB) in DataRow when binary format requested.

## Workflows

- Client connects, sends startup, handles auth, sends queries, receives results (text or binary rows).
- Binary rows allow efficient transfer of numeric, bytea, geometry without text parsing overhead.
- Library emits events like PQ_EVENT_ROW_DESCRIPTION, PQ_EVENT_DATA_ROW (with column metadata + binary payload pointer), allowing caller to process without copying where possible.

## PostGIS Integration Notes

- PostGIS geometry type OID is typically registered dynamically but often 17001 for 'geometry'.
- Binary transfer uses Well-Known Binary (WKB) or Extended WKB (EWKB) with SRID.
- Library provides OID and raw binary buffer; decoding (to WKT, GeoJSON, or internal structs) is application responsibility or via optional PostGIS linkage outside core.
- Supports detecting binary format requests and emitting typed binary events.

This file is seeded and should be expanded with human review of real workflows and edge cases.
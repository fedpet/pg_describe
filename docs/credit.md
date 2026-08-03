---
id: credit
title: Credit
sidebar_label: Credit
---

The idea is taken from [**pgTyped**](https://github.com/adelsz/pgtyped) by
[Adel Salakh](https://github.com/adelsz), which demonstrated that you can
generate honest TypeScript types from a live PostgreSQL database rather than
from a hand-maintained model of it. [`sqlc`](https://sqlc.dev) and
[`sqlx`](https://github.com/launchbadge/sqlx) do the same for Go and Rust.

Those tools ask the server the same question from the client side: they send the
extended query protocol's `Parse` and `Describe` messages and deliberately never
send `Bind` or `Execute`. The server replies with `ParameterDescription` and
`RowDescription`, and the tool follows up with catalog queries to turn type OIDs
into names and `attnotnull` flags.

`pg_describe` asks from inside the server instead. It calls
`parse_analyze_varparams` — the very function `exec_parse_message` calls to
serve a `Parse` message with no declared parameter types — so both projects run
the same PostgreSQL code. One of them just has a shorter path to it.

## What that changes

| | pgTyped / sqlc / sqlx | pg_describe |
|---|---|---|
| Implementation | Wire protocol code in the client, per language | One `SELECT` |
| Round trips per query | Several | One |
| Query files | Rewritten dialect (`:paramName`) | Real SQL (`$1`), runs in psql as-is |
| `LEFT JOIN` nullability | Wrong — reads `attnotnull` alone | Correct |
| Needs an extension installed | No | **Yes** |

## When to use pgTyped instead

That last row is the real cost, and it is decisive on managed Postgres. On RDS,
Cloud SQL and most hosted providers you cannot install extensions at all, and
`pg_describe` is simply unavailable to you.

pgTyped works there, it is mature, it is actively maintained, and it has a
larger feature surface than this project — watch mode, queries embedded in TS
files, and more. If you cannot install an extension, use it. This project is for
when you can, and wants the nullability answer to be right.

# pg_describe

[![CI](https://github.com/sajonaro/pg_describe/actions/workflows/ci.yml/badge.svg)](https://github.com/sajonaro/pg_describe/actions/workflows/ci.yml)
[![PGXN](https://img.shields.io/badge/PGXN-pg__describe-blue)](https://pgxn.org/dist/pg_describe/)
[![npm](https://img.shields.io/npm/v/pg-describe-gen)](https://www.npmjs.com/package/pg-describe-gen)
[![PostgreSQL 17 | 18](https://img.shields.io/badge/PostgreSQL-17%20%7C%2018-336791)](https://www.postgresql.org/)
[![License: MIT](https://img.shields.io/badge/License-MIT-green.svg)](LICENSE)

**pg_describe reports what a query would return, without running it.** That now
includes recursive shapes for JSON assembled by PostgreSQL constructors.

```sql
SELECT * FROM pg_describe('SELECT id, email FROM users WHERE id = $1');
```
```
  kind  | ord | name  | type_name | source_table | base_not_null | result_not_null
--------+-----+-------+-----------+--------------+---------------+-----------------
 param  |   1 |       | integer   |              |               |
 column |   1 | id    | integer   | users        | t             | t
 column |   2 | email | text      | users        | t             | t
```

## Features

- **Nothing is executed** — parse and analysis only, so describing
  `DELETE FROM orders WHERE id = $1` is safe.
- **Parameter types are inferred**; the query text never declares one.
- **Result columns as the wire protocol sees them** — name, type OID and SQL
  type name, with `ORDER BY`-only columns excluded.
- **Outer-join-aware nullability**: a column on the nullable side of a
  `LEFT JOIN` is reported nullable even when it is declared `NOT NULL`. Tools
  that read `attnotnull` alone get this wrong.
- **Column provenance** — the source table and column behind each result field.
- **Structural JSON results** — nested objects and arrays built in SQL retain
  scalar types, provenance, and nullability for code generators.
- **One round trip.** It is an ordinary `SELECT`, callable from any client in
  any language, with no wire-protocol code to write.
- **Privileges are enforced**, so it cannot be used to read a schema the caller
  has no rights to.
- **TypeScript code generation** from plain `.sql` files, included.

## Documentation

Full documentation: **https://sajonaro.github.io/pg_describe/**

## Getting started

```bash
git clone https://github.com/sajonaro/pg_describe
cd pg_describe
docker compose up -d          # PGPORT=5433 docker compose up -d  if 5432 is taken

psql -h localhost -U postgres -d pg_describe_demo \
     -c "SELECT * FROM pg_describe('SELECT 1 AS n')"
```

Or install it into a database you already have:

```bash
pgxn install pg_describe
```
```sql
CREATE EXTENSION pg_describe;
```

Needs PostgreSQL 17 or 18 (16 will probably work, untested) and the ability to install
extensions — see [Installation](docs/installation.md). For TypeScript types:

```bash
npm install --save-dev pg-describe-gen
```

## Example

Write plain SQL. Native `$1`, no dialect, so the file runs in `psql` as it is:

```sql
-- queries/orders.sql
-- @name ListRecentOrders
-- Orders with their customer, if any.
SELECT o.id, o.total, c.email
FROM orders o
LEFT JOIN customers c ON c.id = o.customer_id
WHERE o.placed_at >= $1;
```

```bash
npx pg-describe-gen
```

Get types the database itself vouches for:

```typescript
export interface ListRecentOrdersParams {
  p1: Date
}

export interface ListRecentOrdersRow {
  id: string            // orders.id       — bigint arrives as a string
  total: string         // orders.total    — numeric arrives as a string
  email: string | null  // customers.email — NOT NULL, but LEFT JOINed
}

/**
 * Orders with their customer, if any.
 */
export async function listRecentOrders(
  client: ClientBase,
  params: ListRecentOrdersParams,
): Promise<ListRecentOrdersRow[]>
```

`email` is nullable because the join can null-extend it, not because the schema
says so — that is the difference. Add `pg-describe-gen --check` to CI and a
migration that changes what a query returns fails the pull request instead of
the deploy.

The [end-to-end example](docs/end-to-end-example.md) walks the whole loop, and
[`examples/typescript`](examples/typescript) is it, runnable.

## Resources

- [Getting started](docs/getting-started.md) — a database with the extension in one command
- [End-to-end example](docs/end-to-end-example.md) — schema to failing build
- [Queries in SQL files](docs/queries-in-sql-files.md) — how query files are written
- [CLI usage and configuration](docs/cli-configuration.md) — `pg-describe-gen`
- [Nullability](docs/nullability.md) — what `result_not_null` means and where it stops
- [Function reference](docs/function-reference.md) — the full signature and semantics
- [Permissions](docs/permissions.md) — why the function checks privileges itself
- [How it works](docs/how-it-works.md) — the parser, the join tree walk, the internals

## Repository

```
src/pg_describe.c        the extension
test/                    pg_regress suite, 29 assertions
packages/codegen/        pg-describe-gen, published to npm
examples/typescript/     runnable example, generated output committed
docs/                    documentation source
website/                 Docusaurus site that renders docs/
```

`packages/*` and `examples/*` are npm workspaces: `npm install` at the root
wires the example to the local generator. See [Contributing](docs/contributing.md)
for the test and docs workflows.

## Project state

The extension and the generator are complete and tested on every commit, but
this is young. The API of `pg_describe(text)` is what the TypeScript generator
depends on and is not expected to change; the generator's config file may
still gain keys. Issues and pull requests are welcome.

## Credit

The idea is taken from [pgTyped](https://github.com/adelsz/pgtyped) by
[Adel Salakh](https://github.com/adelsz), which demonstrated that TypeScript
types for SQL can come from a live database rather than a hand-maintained model
of it. [`sqlc`](https://sqlc.dev) and [`sqlx`](https://github.com/launchbadge/sqlx)
do the same for Go and Rust.

pgTyped asks the server the same question over the wire protocol, from the
client, and needs no extension — which is what you want on RDS, Cloud SQL and
most managed Postgres, where `pg_describe` cannot be installed at all. It is
mature, actively maintained and has a larger feature surface than this project.
`pg_describe` moves the question into the server for the cases where you can
install an extension: one `SELECT` instead of a wire-protocol implementation per
language, and nullability that accounts for outer joins. See
[Credit](docs/credit.md).

## License

MIT © 2026-present, see [LICENSE](LICENSE).

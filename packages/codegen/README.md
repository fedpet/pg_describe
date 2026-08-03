# pg-describe-gen

[![npm](https://img.shields.io/npm/v/pg-describe-gen)](https://www.npmjs.com/package/pg-describe-gen)
[![License: MIT](https://img.shields.io/badge/License-MIT-green.svg)](https://github.com/sajonaro/pg_describe/blob/main/LICENSE)

**Generate TypeScript types from plain `.sql` files, using the
[`pg_describe`](https://github.com/sajonaro/pg_describe) PostgreSQL extension.**

Query files stay valid SQL — native `$1` placeholders, no dialect — so the file
you generate types from is a file you can paste straight into `psql`. Types come
from the database itself: each statement is parsed and analysed by PostgreSQL,
and none of them is executed.

## Features

- **Types the database vouches for**, not a hand-maintained model of it.
- **Outer-join-aware nullability**: a column on the nullable side of a
  `LEFT JOIN` is typed `T | null` even when it is declared `NOT NULL` — the case
  most generators get wrong.
- **Inferred parameter types.** Nothing declares them.
- **`--check` mode for CI**: exit 1 when the committed output no longer matches
  the database, so a breaking migration fails the pull request, not the deploy.
- **SQL comments become JSDoc**, so a query documents itself at the call site.
- **Configurable type mapping** with defaults that match what node-postgres
  actually returns.

## Documentation

Full documentation: **https://sajonaro.github.io/pg_describe/**

- [CLI usage and configuration](https://sajonaro.github.io/pg_describe/cli-configuration)
- [Queries in SQL files](https://sajonaro.github.io/pg_describe/queries-in-sql-files)
- [Generated code](https://sajonaro.github.io/pg_describe/generated-code)
- [Type mapping](https://sajonaro.github.io/pg_describe/type-mapping)
- [Nullability](https://sajonaro.github.io/pg_describe/nullability)

## Requires

- PostgreSQL with `pg_describe` installed (`CREATE EXTENSION pg_describe;`)
- Node 18+

## Getting started

```bash
npm install --save-dev pg-describe-gen
```

**1. Write queries, annotated with a name.**

```sql
-- queries/orders.sql

-- @name ListRecentOrders
-- Orders with their customer, if any.
SELECT o.id, o.total, c.email
FROM orders o
LEFT JOIN customers c ON c.id = o.customer_id
WHERE o.placed_at >= $1;
```

**2. Configure.**

```json
// pg-describe.json
{
  "queries": "queries",
  "output": "src/generated/queries.ts"
}
```

Connection settings are deliberately not in this file — the generator uses
node-postgres, which reads `DATABASE_URL` or the standard `PGHOST` / `PGPORT` /
`PGUSER` / `PGPASSWORD` / `PGDATABASE` variables, so credentials stay out of a
file you commit.

**3. Generate.**

```bash
npx pg-describe-gen
```

```typescript
export interface ListRecentOrdersParams {
  p1: Date
}

export interface ListRecentOrdersRow {
  id: string            // orders.id
  total: string         // orders.total
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

**4. Check it in CI.**

```bash
npx pg-describe-gen --check
```

[`examples/typescript`](https://github.com/sajonaro/pg_describe/tree/main/examples/typescript)
is a complete runnable project.

## Credit

The idea is taken from [pgTyped](https://github.com/adelsz/pgtyped) by
[Adel Salakh](https://github.com/adelsz), which demonstrated that TypeScript
types for SQL can come from a live PostgreSQL database rather than from a
hand-maintained model of it. [`sqlc`](https://sqlc.dev) and
[`sqlx`](https://github.com/launchbadge/sqlx) do the same for Go and Rust.

pgTyped asks the server the same question over the wire protocol, from the
client, and needs no extension — which is what you want on RDS, Cloud SQL and
most managed Postgres, where `pg_describe` cannot be installed at all. This
generator moves the question into the server: one `SELECT` per query, real SQL
in the query files, and outer-join-aware nullability.

## License

MIT

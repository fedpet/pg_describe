---
id: end-to-end-example
title: End-to-end example
sidebar_label: End-to-end example
---

From a schema to a build that fails when the schema drifts. This is
[`examples/typescript`](https://github.com/sajonaro/pg_describe/tree/main/examples/typescript)
in the repository — a runnable project with the generated output committed, so
you can read the result without running anything.

Every command below runs against the Docker database from
[Getting started](getting-started.md).

## 1. A schema with a nullable relationship

```sql
CREATE TABLE customers (
    id    serial PRIMARY KEY,
    email text NOT NULL,
    vip   boolean NOT NULL DEFAULT false
);

CREATE TABLE orders (
    id          bigserial PRIMARY KEY,
    customer_id int REFERENCES customers,      -- NULL for guest checkout
    placed_at   timestamptz NOT NULL DEFAULT now(),
    total       numeric(10,2) NOT NULL,
    note        text
);
```

Two details do the work. `orders.customer_id` is nullable because guests can
check out, which makes `LEFT JOIN` the honest way to list orders with their
customer. And `total` is `numeric`, because money in a float is a bug.

## 2. Queries, in plain SQL

```sql
-- queries/orders.sql

-- @name ListRecentOrders
-- Orders with their customer, if any.
SELECT o.id, o.placed_at, o.total, o.note, c.email, c.vip
FROM orders o
LEFT JOIN customers c ON c.id = o.customer_id
WHERE o.placed_at >= $1
ORDER BY o.placed_at DESC;

-- @name DeleteOrdersBefore
DELETE FROM orders WHERE placed_at < $1;
```

There is no dialect here: native `$1`, standard SQL, one `-- @name` comment per
statement to name the generated function. Paste either statement into `psql`,
supply a parameter, and it runs — the file you generate types from is the file
you test with. See [Queries in SQL files](queries-in-sql-files.md).

## 3. Point the generator at them

```json
// pg-describe.json
{
  "queries": "queries",
  "output": "src/generated/queries.ts"
}
```

`queries` is scanned recursively for `.sql` files. Connection settings are
deliberately absent — the generator uses node-postgres, which reads
`DATABASE_URL` or the standard `PGHOST` / `PGPORT` / `PGUSER` / `PGPASSWORD` /
`PGDATABASE` variables, so credentials stay out of a file you commit.

## 4. Generate

```bash
export PGHOST=localhost PGPORT=5432 PGUSER=postgres \
       PGPASSWORD=postgres PGDATABASE=pg_describe_demo

npx pg-describe-gen
```

The generator sends each statement to `pg_describe` in its own round trip,
collecting failures rather than stopping at the first, and writes:

```typescript
export interface ListRecentOrdersParams {
  p1: Date
}

export interface ListRecentOrdersRow {
  id: string            // orders.id
  placed_at: Date       // orders.placed_at
  total: string         // orders.total
  note: string | null   // orders.note
  email: string | null  // customers.email
  vip: boolean | null   // customers.vip
}

/**
 * Orders with their customer, if any.
 */
export async function listRecentOrders(
  client: ClientBase,
  params: ListRecentOrdersParams,
): Promise<ListRecentOrdersRow[]>

/** ... */
export async function deleteOrdersBefore(
  client: ClientBase,
  params: DeleteOrdersBeforeParams,
): Promise<number>
```

Four things in there came from the database, not from a guess:

- **`email` and `vip` are nullable** even though both columns are declared
  `NOT NULL`, because the join can null-extend them. This is the case that
  motivated the extension.
- **`id` and `total` are `string`** — `bigint` does not fit in a JS number and
  `numeric` is arbitrary precision, so node-postgres returns both as strings and
  the generated type matches what you will actually hold at run time.
- **`p1` is `Date`**, inferred from `placed_at >= $1`. Nothing declared it.
- **`deleteOrdersBefore` returns `Promise<number>`** — the affected row count —
  because a statement with no `RETURNING` describes no columns.

## 5. Use it

```typescript
import { listRecentOrders } from './generated/queries.ts'

const orders = await listRecentOrders(client, { p1: new Date('2000-01-01') })

for (const order of orders) {
  const who: string = order.email ?? '(guest)'
  console.log(`  #${order.id}  ${order.total}  ${who}`)
}
```

```
recent orders
  #1  99.95  ada@example.com
  #2  12.00  grace@example.com
  #3  42.50  (guest)
```

Delete the `?? '(guest)'` and the build stops:

```
error TS2322: Type 'string | null' is not assignable to type 'string'.
```

That third order is a real guest checkout, and a generator that read
`attnotnull` alone would have typed `email` as `string` and let it through to a
null at run time.

## 6. Gate CI on it

```bash
npx pg-describe-gen --check
```

Exit 0 when the committed file matches what the database says, 1 when it does
not. Prove it by drifting the schema:

```bash
psql -c 'ALTER TABLE customers ALTER COLUMN vip DROP NOT NULL;'

npx pg-describe-gen --check
# src/generated/queries.ts is out of date. Run pg-describe-gen to regenerate it.
```

A migration that changes what a query returns now fails the build, in the pull
request, instead of the deploy. That is the loop the project exists to close.

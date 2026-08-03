# pg-describe-gen

Generate TypeScript types from plain `.sql` files, using the
[`pg_describe`](https://github.com/sajonaro/pg_describe) PostgreSQL extension.

Your query files stay **valid SQL** — native `$1` placeholders, no dialect to
learn — so the file you generate types from is a file you can paste straight
into `psql`.

## Requires

- PostgreSQL with `pg_describe` installed (`CREATE EXTENSION pg_describe;`)
- Node 18+

## Install

```bash
npm install --save-dev pg-describe-gen
```

## Use

**1. Write queries, annotated with a name.**

```sql
-- queries/orders.sql

-- @name ListRecentOrders
-- Orders with their customer, if any.
SELECT o.id, o.total, c.email
FROM orders o
LEFT JOIN customers c ON c.id = o.customer_id
WHERE o.placed_at >= $1;

-- @name CreateOrder
INSERT INTO orders (customer_id, total) VALUES ($1, $2)
RETURNING id, placed_at;
```

The comment block between `@name` and the statement becomes the generated
function's JSDoc. Several queries per file are fine.

**2. Configure.**

```json
// pg-describe.json
{
  "queries": "queries",
  "output": "src/generated/queries.ts"
}
```

`queries` is a directory, scanned recursively for `.sql` files.

Connection settings are deliberately *not* in this file — the generator uses
node-postgres, which reads `DATABASE_URL` or the standard `PGHOST` / `PGPORT` /
`PGUSER` / `PGPASSWORD` / `PGDATABASE` variables. Credentials stay out of a file
you want to commit.

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
  email: string | null  // customers.email
}

/**
 * Orders with their customer, if any.
 */
export async function listRecentOrders(
  client: ClientBase,
  params: ListRecentOrdersParams,
): Promise<ListRecentOrdersRow[]>
```

A statement with no `RETURNING` generates a function returning `Promise<number>`
— the affected row count — instead of a row array.

## In CI

```bash
npx pg-describe-gen --check
```

Exits 1 if the committed output no longer matches what the database says. A
schema change that breaks a query then fails the build instead of the deploy.

## Type mapping

| PostgreSQL | TypeScript | |
|---|---|---|
| `smallint`, `integer`, `real`, `double precision` | `number` | |
| `bigint` | `string` | an int8 does not fit in a JS number; node-postgres returns a string |
| `numeric`, `decimal` | `string` | arbitrary precision, and floats are wrong for money |
| `boolean` | `boolean` | |
| `text`, `varchar`, `char`, `uuid`, `citext`, `inet`, `interval` | `string` | |
| `date`, `timestamp`, `timestamptz` | `Date` | |
| `time`, `time with time zone` | `string` | no `Date` equivalent |
| `bytea` | `Buffer` | |
| `json`, `jsonb` | `unknown` | narrow it yourself |
| `T[]` | `T[]` | any dimension |
| anything else | `unknown` | with a warning |

Enums, domains and user-defined types fall through to `unknown`. Override
anything with a `types` block:

```json
{
  "queries": "queries",
  "output": "src/generated/queries.ts",
  "types": {
    "order_status": "'pending' | 'shipped' | 'cancelled'",
    "bigint": "bigint"
  }
}
```

## Nullability

The generator uses `pg_describe`'s `result_not_null`, which accounts for outer
joins rather than reading `attnotnull` alone. A column on the nullable side of a
`LEFT JOIN` is typed `T | null` even when its source column is `NOT NULL` —
which is the case most generators get wrong.

Where nullability is unknown (an expression column, a set operation), the
generator emits `T | null`. Over-declaring a null costs one impossible check;
under-declaring one costs a crash.

Parameters are declared non-null. Passing `null` is legal SQL, but it is almost
always a bug rather than an intent, and `| null` on every parameter makes the
generated types tiresome.

## License

MIT

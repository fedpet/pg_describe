# TypeScript example

Schema, queries, committed generated output, and a demo that connects to a real
database. The narrated version of this is the
[end-to-end example](../../docs/end-to-end-example.md) in the documentation.

## Run

From the repository root:

```bash
docker compose up -d --build          # PGPORT=5433 ... if 5432 is taken

npm install                           # workspace root: wires this example to packages/codegen
npm run build

docker compose exec -T db psql -U postgres -d pg_describe_demo -q \
  < examples/typescript/schema.sql

export PGHOST=localhost PGPORT=5432 PGUSER=postgres \
       PGPASSWORD=postgres PGDATABASE=pg_describe_demo

npm run generate
npm run typecheck
npm run demo
```

```
recent orders
  #1  99.95  ada@example.com
  #2  12.00  grace@example.com
  #3  42.50  (guest)

lookup
  ada@example.com vip=true

daily totals
  2026-08-03  n=3  154.45
```

Those scripts are workspace-aware, so `npm run demo` from the root and
`npm run demo` from this directory do the same thing.

## Files

[`queries/orders.sql`](queries/orders.sql) — five queries, all valid SQL. Paste
any of them into psql with parameters and they run.

[`src/generated/queries.ts`](src/generated/queries.ts) — committed so it can be
read without running anything:

- `email: string | null` in `ListRecentOrdersRow`. `customers.email` is
  `NOT NULL`, but the query `LEFT JOIN`s it, so a guest order yields NULL.
- `id: string`, `total: string` — `bigint` and `numeric` come back as strings
  from node-postgres.
- `DeleteOrdersBefore` returns `Promise<number>`, having no `RETURNING`.

[`src/demo.ts`](src/demo.ts) line 39:

```typescript
const who: string = order.email ?? '(guest)'
```

Remove the `?? '(guest)'` and `npm run typecheck` fails:

```
error TS2322: Type 'string | null' is not assignable to type 'string'.
```

## CI gate

```bash
npm run check
```

Exit 0 when the committed file matches the database, 1 when it does not:

```bash
docker compose -f ../../docker-compose.yml exec -T db \
  psql -U postgres -d pg_describe_demo -c 'ALTER TABLE customers ALTER COLUMN vip DROP NOT NULL;'

npm run check
# src/generated/queries.ts is out of date. Run pg-describe-gen to regenerate it.
```

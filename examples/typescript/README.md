# TypeScript example

Schema, queries, committed generated output, and a demo that connects to a real
database.

## Run

From the repository root:

```bash
docker compose up -d --build          # PGPORT=5433 ... if 5432 is taken

cd packages/codegen && npm install && npm run build && cd -
cd examples/typescript && npm install

export PGHOST=localhost PGPORT=5432 PGUSER=postgres \
       PGPASSWORD=postgres PGDATABASE=pg_describe_demo

docker compose -f ../../docker-compose.yml exec -T db \
  psql -U postgres -d pg_describe_demo -q < schema.sql

npx pg-describe-gen
npx tsc --noEmit
node src/demo.ts
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

Remove the `?? '(guest)'` and `npx tsc --noEmit` fails:

```
error TS2322: Type 'string | null' is not assignable to type 'string'.
```

## CI gate

```bash
npx pg-describe-gen --check
```

Exit 0 when the committed file matches the database, 1 when it does not:

```bash
docker compose -f ../../docker-compose.yml exec -T db \
  psql -U postgres -d pg_describe_demo -c 'ALTER TABLE customers ALTER COLUMN vip DROP NOT NULL;'

npx pg-describe-gen --check
# src/generated/queries.ts is out of date. Run pg-describe-gen to regenerate it.
```

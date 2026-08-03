# TypeScript example

A complete, runnable project: schema, queries, committed generated output, and a
demo that connects to a real database and prints real rows.

## Run it

From the repository root:

```bash
docker compose up -d --build          # PGPORT=5433 ... if 5432 is taken

cd packages/codegen && npm install && npm run build && cd -
cd examples/typescript && npm install

export PGHOST=localhost PGPORT=5432 PGUSER=postgres \
       PGPASSWORD=postgres PGDATABASE=pg_describe_demo

docker compose -f ../../docker-compose.yml exec -T db \
  psql -U postgres -d pg_describe_demo -q < schema.sql

npx pg-describe-gen        # regenerates src/generated/queries.ts
npx tsc --noEmit           # typechecks
node src/demo.ts           # runs
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

## What to look at

**[`queries/orders.sql`](queries/orders.sql)** — five queries, all valid SQL.
Copy any of them into `psql`, supply the parameters, and they run. There is no
dialect and no rewriting step between what you test and what you ship.

**[`src/generated/queries.ts`](src/generated/queries.ts)** — committed on
purpose, so you can read the output without running anything. Three things in it
are worth noticing:

- `email: string | null` in `ListRecentOrdersRow`. `customers.email` is declared
  `NOT NULL`, but the query `LEFT JOIN`s it, so a guest order produces NULL. A
  generator reading `attnotnull` alone types this `string` and is wrong.
- `id: string` and `total: string`. `bigint` and `numeric` are returned as
  strings by node-postgres, because neither survives a JavaScript number.
- `DeleteOrdersBefore` returns `Promise<number>`, not a row array, because it has
  no `RETURNING`.

**[`src/demo.ts`](src/demo.ts)** — line 39 is the point of the whole exercise:

```typescript
const who: string = order.email ?? '(guest)'
```

Delete the `?? '(guest)'` and `npx tsc --noEmit` fails with

```
error TS2322: Type 'string | null' is not assignable to type 'string'.
```

That is a production null-dereference caught at compile time, from a NULL the
schema alone does not predict.

## The CI gate

```bash
npx pg-describe-gen --check
```

Exits 0 when the committed file matches the database, 1 when it does not. Try
it:

```bash
docker compose -f ../../docker-compose.yml exec -T db \
  psql -U postgres -d pg_describe_demo -c 'ALTER TABLE customers ALTER COLUMN vip DROP NOT NULL;'

npx pg-describe-gen --check
# src/generated/queries.ts is out of date. Run pg-describe-gen to regenerate it.
# exit 1
```

Put that in CI and a schema change that breaks a query fails the build rather
than the deploy.

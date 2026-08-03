---
id: getting-started
title: Getting started
sidebar_label: Getting started
---

## A database with the extension, in one command

```bash
git clone https://github.com/sajonaro/pg_describe
cd pg_describe
docker compose up -d          # PGPORT=5433 docker compose up -d  if 5432 is taken
```

The image builds the extension and creates it in the demo database and in
`template1`, so any database you create afterwards has it too.

```bash
psql -h localhost -U postgres -d pg_describe_demo \
     -c "SELECT * FROM pg_describe('SELECT 1 AS n')"
```

Other ways to install — PGXN, building from source, version requirements — are
in [Installation](installation.md).

## Calling the function

It is a set-returning function: one row per parameter, then one row per result
column.

### What are this statement's parameters?

```sql
SELECT ord, type_name
FROM pg_describe('UPDATE users SET email = $2 WHERE id = $1')
WHERE kind = 'param';
```
```
 ord | type_name
-----+-----------
   1 | integer
   2 | text
```

`$2` appears in the text before `$1`, and the numbering still follows the
parameter rather than the position. Note also that nothing ran: no row was
updated.

### What does it return?

```sql
SELECT ord, name, type_name, result_not_null
FROM pg_describe('INSERT INTO orders (customer_id, total) VALUES ($1, $2)
                  RETURNING id, placed_at')
WHERE kind = 'column';
```
```
 ord |   name    |        type_name         | result_not_null
-----+-----------+--------------------------+-----------------
   1 | id        | bigint                   | t
   2 | placed_at | timestamp with time zone | t
```

`INSERT`/`UPDATE`/`DELETE` describe their `RETURNING` list. Without one they
describe no columns at all, which is how a caller distinguishes "returns rows"
from "returns a count".

### Where did each column come from, and can it be NULL?

```sql
SELECT ord, name, source_table::text, base_not_null, result_not_null
FROM pg_describe('SELECT o.id, c.email FROM orders o
                  LEFT JOIN customers c ON c.id = o.customer_id');
```
```
 ord | name  | source_table | base_not_null | result_not_null
-----+-------+--------------+---------------+-----------------
   1 | id    | orders       | t             | t
   2 | email | customers    | t             | f
```

This is the case worth understanding before you build anything on top of the
function — see [Nullability](nullability.md).

## Errors

A parse or analysis failure is an ordinary PostgreSQL error, with the caret
pointing inside your query rather than at the `pg_describe(` call:

```sql
SELECT name, type_name FROM pg_describe($$SELECT id, emial FROM users$$);
```
```
ERROR:  column "emial" does not exist
LINE 1: SELECT id, emial FROM users
                   ^
HINT:  Perhaps you meant to reference the column "users.email".
QUERY:  SELECT id, emial FROM users
```

Errors abort the surrounding transaction, so a tool describing many queries
should send each in its own round trip and collect the failures rather than
stopping at the first. That is what `pg-describe-gen` does.

## Generating TypeScript

```bash
npm install --save-dev pg-describe-gen
```

```json
// pg-describe.json
{
  "queries": "queries",
  "output": "src/generated/queries.ts"
}
```

```bash
npx pg-describe-gen
```

The [end-to-end example](end-to-end-example.md) walks the whole loop — schema,
queries, generated types, and a build that fails when the schema drifts.

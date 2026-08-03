---
id: overview
title: Overview
sidebar_label: Overview
slug: /
---

`pg_describe` is a PostgreSQL extension that answers one question: **what would
this query return, if I ran it?**

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

Nothing is executed. The extension runs PostgreSQL's parser and analyser and
stops before the executor, so describing `DELETE FROM orders WHERE id = $1` is
safe — and reports that `$1` is an `integer`. Parameter types are inferred; the
query text never declares one.

## What one call tells you

- **The type of every `$n`**, inferred from context. Nothing is declared: the
  analyser works out that `$1` in `WHERE id = $1` is an `integer` because that
  is what `id` is.
- **The name and type of every result column**, exactly as the wire protocol's
  `RowDescription` would report them — `ORDER BY`-only columns excluded,
  `character varying(10)` keeping its typmod.
- **Where each column came from**: `source_table` and `source_column`, for plain
  column references. Expressions get a type but no provenance.
- **Whether each result column can actually be NULL**, which is not the same as
  whether the underlying column is `NOT NULL`. See [Nullability](nullability.md).

`SELECT` describes its select list; `INSERT`/`UPDATE`/`DELETE` describe their
`RETURNING` list; utility statements describe no columns. Nothing is planned and
nothing is executed, so describing a `SELECT` over a billion rows costs a
catalog lookup, not a scan.

## The problem it solves

Every application that talks to PostgreSQL keeps a second, informal copy of the
database's shape: the structs, interfaces or classes that rows are read into.
That copy is written by hand, and nothing keeps it honest. `ALTER TABLE orders
ALTER COLUMN note DROP NOT NULL` changes the database; it does not change the
`note: string` in your code. The mismatch surfaces in production.

There are three ways to know a query's real shape:

1. **Read the schema and reason about it yourself.** This is what hand-written
   types are. It goes stale silently, and it gets nullability wrong.
2. **Run the query and look at what comes back.** Correct, but you cannot do it
   at build time for a `DELETE`, and it needs the right rows to exist before the
   answer means anything.
3. **Ask the server to analyse the query without running it.** This is what the
   extended query protocol's `Parse`/`Describe` exchange does, and what this
   extension exposes as an ordinary function call.

So `pg_describe` is infrastructure for build-time tooling: type generators,
linters that fail CI when a query no longer matches the schema, editor
integrations, migration checks that answer "which of our 300 queries does this
`ALTER TABLE` break?", and anything else that needs a query's contract without
its side effects.

[`pg-describe-gen`](end-to-end-example.md) is the first consumer: it turns a
directory of plain `.sql` files into typed TypeScript.

## Why nullability is the interesting part

`attnotnull` says whether a *source column* is declared `NOT NULL`. Whether a
*result column* can be NULL is a different question, and an outer join is where
the two come apart:

```
 ord | name  | source_table | base_not_null | result_not_null
-----+-------+--------------+---------------+-----------------
   1 | id    | orders       | t             | t
   2 | email | customers    | t             | f
```

`customers.email` is `NOT NULL` — `base_not_null` says so, correctly — and the
result column is NULL anyway for an order with no customer. A tool reading
`attnotnull` alone types that field non-nullable and hands you a
null-dereference on the first guest order.

`result_not_null` is the answer to the question you actually have.
[Nullability](nullability.md) explains how it is computed and where it stops.

## Where to go next

- [Features](features.md) — the list, if you are comparing tools
- [Getting started](getting-started.md) — a working database in one command
- [End-to-end example](end-to-end-example.md) — schema to failing build, in full
- [The pg_describe function](function-reference.md) — the reference

## Credit

The idea is taken from [pgTyped](https://github.com/adelsz/pgtyped) by
[Adel Salakh](https://github.com/adelsz). See [Credit](credit.md).

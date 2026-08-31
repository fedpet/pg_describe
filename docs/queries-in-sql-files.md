---
id: queries-in-sql-files
title: Queries in SQL files
sidebar_label: Queries in SQL files
---

`pg-describe-gen` reads plain `.sql` files. There is no dialect: placeholders
are PostgreSQL's own `$1`, `$2`, and a file of queries is a file `psql` will
run.

## Naming a query

One `-- @name` comment above each statement. The name becomes the generated
interfaces and function:

```sql
-- @name ListRecentOrders
SELECT o.id, o.total FROM orders o WHERE o.placed_at >= $1;
```

```typescript
listRecentOrders(client, params: ListRecentOrdersParams): Promise<ListRecentOrdersRow[]>
```

`@name` is required. Text before the first annotation is treated as a file
header and ignored, so an unannotated statement is simply not generated. If the
whole queries directory yields no annotated statement, that is an error — it is
almost always a misconfigured `queries` path.

Several queries per file are fine, and the directory is scanned recursively, so
organise files however suits the codebase. Names become exported identifiers,
so they must be unique across all files; a duplicate is reported with both file
names rather than silently overwriting.

## Documentation comments

Comment lines between `@name` and the statement become the generated function's
JSDoc:

```sql
-- @name ListRecentOrders
-- Orders with their customer, if any. Guest orders have no customer, so
-- `email` comes back null.
SELECT o.id, c.email
FROM orders o
LEFT JOIN customers c ON c.id = o.customer_id
WHERE o.placed_at >= $1;
```

```typescript
/**
 * Orders with their customer, if any. Guest orders have no customer, so
 * `email` comes back null.
 */
export async function listRecentOrders(/* ... */)
```

The query documents itself at the call site, in the editor, without a second
copy of the explanation living in TypeScript.

## Parameters

Nothing declares a parameter type. The analyser infers each one from where it
appears, and the generator names them `p1`, `p2`, … in a params interface:

```sql
-- @name CreateOrder
INSERT INTO orders (customer_id, total, note)
VALUES ($1, $2, $3)
RETURNING id, placed_at, total;
```

```typescript
export interface CreateOrderParams {
  p1: number
  p2: string
  p3: string
}
```

Parameters are declared non-null. Passing `null` is legal SQL, but it is almost
always a bug rather than an intent, and `| null` on every parameter makes the
generated types tiresome to use.

## Statements that return nothing

A statement with no `RETURNING` describes no columns, so the generated function
returns the affected row count instead of a row array:

```sql
-- @name DeleteOrdersBefore
DELETE FROM orders WHERE placed_at < $1;
```

```typescript
export async function deleteOrdersBefore(
  client: ClientBase,
  params: DeleteOrdersBeforeParams,
): Promise<number>
```

Add `RETURNING id` and it becomes `Promise<DeleteOrdersBeforeRow[]>` on the next
generate — which is exactly the kind of change `--check` catches in CI when only
one of the two files was updated.

## Expression columns

An expression has no source column, so pg_describe reports no provenance for
it. Nullability is a separate question, and one it answers from the expression
itself:

```sql
-- @name DailyOrderTotals
SELECT date_trunc('day', o.placed_at) AS day,
       count(*)                       AS order_count,
       sum(o.total)                   AS revenue
FROM orders o
GROUP BY 1;
```

```typescript
export interface DailyOrderTotalsRow {
  day: Date
  order_count: string
  revenue: string | null
}
```

Three expressions, three different answers. `date_trunc` is strict and
`placed_at` is `NOT NULL`, so `day` cannot be NULL. `count()` returns 0 rather
than NULL over no rows. `sum()` over an empty group really is NULL, so
`revenue` keeps the union -- correct rather than merely cautious.

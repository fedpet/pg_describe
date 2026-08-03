---
id: nullability
title: Nullability
sidebar_label: Nullability
---

This is the reason the extension exists rather than a catalog query.

## Two different questions

`pg_attribute.attnotnull` says whether a **source column** is declared
`NOT NULL`. Whether a **result column** can be NULL is a different question, and
an outer join is where the two come apart:

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

`customers.email` is declared `NOT NULL`, and `base_not_null` reports that
correctly. The result column is NULL anyway for an order with no customer,
because the join null-extends the whole right side when it finds no match.

A tool reading `attnotnull` alone types that field non-nullable and hands you a
null-dereference on the first guest order. Outer joins are not an exotic corner;
they are most of the reporting queries anyone writes.

**Use `result_not_null`.** `base_not_null` is exposed because it is occasionally
useful to know the difference — for a migration report, say — not because you
should type against it.

## How it is computed

By walking the query's join tree, recursing into both arms of each join before
applying that join's own rule, and collecting every base relation beneath a
null-extended side. That handles the nesting a per-join implementation gets
wrong:

```
a LEFT JOIN (b JOIN c)       b and c are BOTH nullable
(a LEFT JOIN b) JOIN c       only b is nullable
a LEFT JOIN (b LEFT JOIN c)  b and c
```

In the first case, an outer join that finds no match null-extends its entire
right subtree at once, so the inner join between `b` and `c` is irrelevant. All
three shapes are in the regression suite; an implementation inspecting only each
join's immediate arms passes the second and fails the first.

`FULL JOIN` nulls both sides. `RIGHT JOIN` nulls the left.

## Grouping sets

`GROUP BY ROLLUP`, `CUBE` and `GROUPING SETS` null-extend grouping columns in
their super-aggregate rows — the total row of a rollup has NULL where the
grouping column was. When a query uses them, no provenance-bearing column is
reported non-null.

## What is not modelled

All of these report nullable or unknown, so all of them fail safe:

- **Set operations.** `UNION` branches are subquery range table entries, so
  target-list `Var`s do not resolve to a base relation and no flags are
  reported.
- **Subqueries and CTEs.** Same reason.
- **`CHECK` constraints, partial indexes, `WHERE x IS NOT NULL`.** Predicates
  that guarantee non-nullness are not consulted, so a column that cannot be NULL
  in practice may still be reported nullable.
- **Expressions.** `upper(x)`, `count(*)` and literals have a type but no source
  column, so both flags are NULL, meaning unknown.

NULL in `result_not_null` means *unknown*, not *not null*. Treat it as nullable.

## In generated TypeScript

`pg-describe-gen` follows the same rule: `result_not_null = t` becomes `T`,
anything else becomes `T | null`.

```typescript
export interface ListRecentOrdersRow {
  id: string            // orders.id       — NOT NULL, and not outer-joined
  email: string | null  // customers.email — NOT NULL, but LEFT JOINed
}
```

Over-declaring a null costs one impossible check. Under-declaring one costs a
crash. When the analysis is uncertain, it picks the cheap mistake.

Parameters are the exception: they are declared non-null. Passing `null` is
legal SQL, but it is almost always a bug rather than an intent, and `| null` on
every parameter makes the generated types tiresome to use.

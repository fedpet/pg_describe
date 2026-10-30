# Nullability

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
- **Expressions that are not provable.** See below: an expression has no source
  column, so `base_not_null` is always NULL for one, but `result_not_null` is
  answered where it can be.

NULL in `result_not_null` means *unknown*, not *not null*. Treat it as nullable.

## Expressions

An expression has no source column, so `base_not_null` is NULL for one. Its
`result_not_null` is answered where the answer follows from the expression
itself:

| | |
| --- | --- |
| Literals | Non-null unless the literal is NULL. |
| Functions and operators | Shared semantic proofs establish non-null results. Known null-preserving functions such as `upper(x)` require non-null arguments; clocks such as `now()` are always non-null. Strictness alone is insufficient. |
| `COALESCE` | Non-null when any argument is. |
| `CASE` | Non-null when every arm is, `ELSE` included. A `CASE` with no `ELSE` yields NULL when nothing matches. |
| `count()` | Always non-null: it returns 0 over no rows, where every other aggregate returns NULL. |
| `IS NULL`, `IS TRUE`, `IS DISTINCT FROM` | Always non-null: they answer a question about a value, including about NULL. |
| `ARRAY[...]`, `ROW(...)` | Always non-null: constructing a value never yields NULL. |
| Anything else | Unknown. |

The proofs use catalog-resolved identities and expression semantics. They reach
the underlying columns, so an outer join still wins.

```sql
SELECT upper(b.b_val) FROM a LEFT JOIN b ON b.a_id = a.id
```

`b_val` is `NOT NULL` and `upper` is strict, but the join null-extends the
column, so the expression is *not* reported non-null. An implementation that
reasoned about the function without reaching the column would claim non-null
here and hand back a crash. That case is in the regression suite.

Two shapes worth naming because they look provable and are not. A
zero-argument function needs a semantic guarantee; strictness says nothing
about its result. And `NULLIF` shares the shape of an
operator but is the opposite case: it returns NULL precisely when its arguments
are equal, however non-null they are.

A grouped expression is answered as any other. From v18 the parser puts a
grouping step between the target list and the range table, so `GROUP BY
date_trunc('day', ts)` leaves a reference to that step where earlier versions
left the expression itself; pg_describe resolves through it and asks the same
question of the same expression, so the answer does not depend on which server
you ask. Under `GROUP BY ROLLUP` it stays unknown, because a super-aggregate
row nulls the grouped column whatever produced it.

## Consuming the metadata

Only `result_not_null = true` proves that a result cannot be NULL. Treat both
false and unknown as nullable when generating types. Parameter rows do not prove
that a parameter is non-null; consumers choose their own parameter policy.


## Function bodies and bulk introspection

Query description and `pg_describe_functions` share the same function proofs.
A proof is a sufficient condition over non-null input arguments, never a claim
that a failed proof means the function returns NULL. Unknown stays nullable.

Built-in facts identify exact overloads in `pg_catalog`, so a user function or
another overload cannot inherit a guarantee from its name. SQL bodies can prove
additional conditions through parameters, constants, CASE, COALESCE and nested
calls. STRICT adds the requirement that every argument is non-null; it never
establishes a result proof by itself. Stored default expressions are analyzed
without executing them when positional arguments are omitted.

Body inference requires a single scalar SELECT with a guaranteed result row
and a target of the declared result type. A NOT NULL target over a potentially
empty query does not prove the function non-null. Return coercions, named calls,
security-definer functions, functions with local settings, opaque languages,
unsupported expressions and recursive dependencies remain conservative.
String SQL bodies use PostgreSQL's parameter hooks. Expected analysis failures
are isolated in an internal subtransaction; cancellation and unexpected failures
still propagate. Neither discovery nor proof inference plans or executes a body.

Proof depth and formula size are bounded. Reaching a bound loses precision,
never correctness. Function summaries are cached only within an analysis call,
so catalog changes cannot leave persistent stale semantic facts.

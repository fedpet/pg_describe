---
id: function-reference
title: The pg_describe function
sidebar_label: Function reference
---

```sql
pg_describe(sql text) RETURNS TABLE (
  kind            text,      -- 'param' | 'column'
  ord             int,       -- $1..$n, or column 1..n
  name            text,      -- output column name; NULL for params
  type_oid        oid,
  type_name       text,      -- 'integer', 'character varying(10)'
  source_table    regclass,  -- NULL unless the column is a plain column reference
  source_column   text,
  base_not_null   boolean,   -- attnotnull on the source column
  result_not_null boolean,   -- can this result column be NULL. Use this one.
  result_shape    jsonb      -- recursive shape for constructed JSON/JSONB
)
```

Declared `VOLATILE STRICT`, and deliberately **not** `PARALLEL SAFE`: the answer
depends on catalog contents another session can change, the call takes locks,
and parse analysis is not something to invite into a parallel worker.

## Rows

One row per parameter, then one row per result column.

| `kind` | `ord` | `name` |
|---|---|---|
| `param` | the `$n` number | NULL |
| `column` | position in the result, from 1 | the output column name |

Parameter rows carry only `type_oid` and `type_name`; the remaining columns are
NULL.

## JSON result shapes

`result_shape` is NULL for non-JSON columns. For JSON and JSONB it has four
recursive variants: `unknown`, `scalar`, `array`, and `object`. Scalar leaves
retain the PostgreSQL type, provenance, base nullability, and result
nullability. Arrays carry their element shape. Objects carry ordered named
fields, optionality, and an `additional` value shape for dynamic keys.

Stored JSON columns, parameters, path/query functions, and other values whose
contents cannot be known without executing the statement report `unknown`.
JSON produced by PostgreSQL's constructors is described recursively, including
SQL arrays and composite values embedded in it.

## Which columns are described

| Statement | Columns |
|---|---|
| `SELECT` | the select list |
| `INSERT` / `UPDATE` / `DELETE` | the `RETURNING` list; none without one |
| Utility statements (`CREATE TABLE`, `VACUUM`, …) | none |

`resjunk` entries are skipped, so a sort key that `ORDER BY` added for an
unselected column does not appear — matching what `RowDescription` reports over
the wire.

## Provenance

`source_table` and `source_column` are populated only when the target-list entry
is a plain column reference: a `Var` with `varlevelsup = 0` and `varattno > 0`.

Everything else — expressions, function calls, literals, whole-row references,
system columns, correlated references to an enclosing query — gets a name and a
type but no provenance, and both nullability flags come back NULL.

**NULL means unknown, not "not null".** Treat it as nullable. See
[Nullability](nullability.md).

## Behaviour and cost

- **One statement per call**, as a `Parse` message carries exactly one:
  `ERROR: pg_describe expects exactly one statement, got 2`.
- **A NULL argument returns the empty set**, without entering the C function at
  all — describing "no query" is not an error, it is nothing.
- **Nothing is executed.** Rewrite, planning and execution are never reached;
  see [How it works](how-it-works.md).
- **Locks.** Analysis opens every referenced relation with `AccessShareLock`,
  held until the end of the transaction. Describing is cheap but neither free
  nor side-effect-free: describing ten thousand queries in one transaction holds
  ten thousand locks.
- **Errors abort the transaction.** They are ordinary PostgreSQL errors and are
  not caught, so a caller describing many statements should send each in its own
  transaction and collect the failures.
- **Privileges are enforced** before any row is returned. See
  [Permissions](permissions.md).

## Error positions

A parse error's caret is relocated into the described statement, so it points at
your SQL rather than at the `pg_describe(` call:

```
ERROR:  column "emial" does not exist
LINE 1: SELECT id, emial FROM users
                   ^
HINT:  Perhaps you meant to reference the column "users.email".
QUERY:  SELECT id, emial FROM users
```

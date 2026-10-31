# The pg_describe function

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
  result_shape    jsonb,     -- recursive shape for constructed JSON/JSONB
  array_dimensions integer, -- provable SQL array rank; NULL if unknown
  array_element_not_null boolean -- provable element nullability; NULL if unknown
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

## SQL array facts

`array_dimensions` and `array_element_not_null` describe SQL arrays. NULL means
unknown; rank zero is the empty array. PostgreSQL's type OID and SQL type spelling
do not encode rank, and a column declared `integer[][]` can store flat or nested
values. Constructors, constant arrays, `ARRAY(SELECT)`, `array_agg`, and compatible
CASE/COALESCE arms carry provable facts.

A validated, enforced table or domain CHECK in this canonical form proves a flat
array without NULL elements; the whole value can still be NULL:

```sql
CHECK (CASE WHEN value IS NULL THEN TRUE
  WHEN cardinality(value) = 0 THEN TRUE
  WHEN array_ndims(value) = 1 THEN array_position(value, NULL) IS NULL
  ELSE FALSE END)
```

Use the column name for `value` in table constraints, or `VALUE` in domain
constraints. Unknown-rank SQL arrays converted to JSON have an unknown shape.

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
type but no provenance, and base nullability comes back NULL. Result nullability is inferred separately
for recognized expressions; see [Nullability](nullability.md).

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


## Bulk routine discovery

`pg_describe_functions(schemas text[] DEFAULT NULL)` returns every routine in
the selected schemas for which the caller has schema USAGE and function EXECUTE.
NULL selects all accessible schemas; an empty array selects none. Procedures,
aggregates, window functions, set-returning functions and unsupported types stay
in the result, even when no non-null proof is available. Consumers choose which
signatures they can represent. Row order is unspecified.

Each row contains the routine OID, exact schema and name, kind, input type OIDs,
return type OID, set-returning flag, default argument count, variadic element
OID (zero when absent), strictness, catalog argument names and default-expression
non-null facts. Argument names retain the catalog's complete names array,
including OUT arguments where present. `all_argument_types` and `argument_modes`
retain the catalog arrays needed to pair these names with input/output types;
NULL means the routine has only ordinary IN arguments. `volatility` is the
catalog's immutable/stable/volatile code. OIDs are database-local identities and
must not be embedded into generated runtime code.

`nonnull_when` is a JSON array of sufficient clauses. Each clause contains
one-based input argument positions whose non-nullness suffices together;
any proven clause proves the result non-null:

- `[]`: no proof.
- `[[]]`: always non-null on successful scalar evaluation.
- `[[1,2]]`: non-null when both inputs are non-null.
- `[[1],[2]]`: non-null when either input is non-null.

`default_not_null` follows the trailing defaulted input arguments. A true entry
proves that omitted argument's default non-null. A false entry means unproven.
Function conditions refer to the complete positional signature, including
arguments with defaults. The strict flag is independent of the result proof.

The bulk call shares dependency analysis across routines. It does not invoke
functions, infer guarantees from sample values, or persist an analysis cache.


## Statement and finite-value facts (2.2)

`pg_describe_facts(sql text)` returns a JSON object with the outer `command`,
optional `copy` direction/transport/format, and an ordered `columns` array. Each
column carries `ord`, `values`, and `element_values`. A null set is unknown;
an empty array is a proved empty set of non-null values. SQL NULL remains
separate and is described by the ordinary nullability metadata.

`pg_describe_type_facts(type_oid oid)` reports `values`, `element_values`, and
`element_type_values` from validated, enforced domain constraints. It requires
USAGE on the type. Finite facts cover direct text/varchar/enum equality and
constant-array membership, compatible CHECK intersections, and text/enum array
containment. Unknown predicates remain unknown; opaque functions and
nondeterministic collations never establish exact string-label sets.

CHECK accepts UNKNOWN, so a membership CHECK with a NULL alternative establishes
no scalar finite set. WHERE accepts only TRUE and can discard that alternative.
Only facts shared by every OR branch survive; AND intersects available facts.
Traditional inheritance does not inherit NO INHERIT proofs, and foreign-table
constraints are not treated as locally enforced facts. Positive WHERE facts
never prove a non-null result for an empty-input aggregate or a repeated volatile
expression. JSON scalar shapes carry the same optional `values` proof.

CALL output columns now follow PostgreSQL's procedure result descriptor,
including OUT/INOUT names and types. No procedure is invoked. Dynamic anonymous
record output without a static descriptor remains outside this contract.
COPY metadata describes the parsed transport, not a promise that runtime
privileges, paths, input bytes, or table constraints will permit execution.

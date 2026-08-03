# pg_describe

Ask PostgreSQL what a query would return, without running it.

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

Nothing is executed — the extension runs PostgreSQL's parser and analyser and
stops before the executor. Describing `DELETE FROM orders WHERE id = $1` is
safe, and reports that `$1` is an `integer`. Parameter types are inferred; the
query text never declares one.

Idea taken from [pgTyped](https://github.com/adelsz/pgtyped), which does this
from the client by sending `Parse`/`Describe` over the wire protocol and never
sending `Bind`. `pg_describe` asks the same question from inside the server.

## Why

| | pgTyped / sqlc / sqlx | pg_describe |
|---|---|---|
| Implementation | Wire protocol code in the client, per language | One `SELECT` |
| Round trips per query | Several | One |
| Query files | Rewritten dialect (`:paramName`) | Real SQL (`$1`), runs in psql as-is |
| `LEFT JOIN` nullability | Wrong — reads `attnotnull` alone | Correct |
| Needs an extension installed | No | **Yes** |

That last row is the real cost. On RDS, Cloud SQL and most managed Postgres you
cannot install extensions, and pgTyped is the option that works.

## Nullability

`attnotnull` says whether a *source column* is `NOT NULL`. That is a different
question from whether a *result column* can be NULL:

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

`customers.email` is `NOT NULL`, and the result column is NULL anyway for an
order with no customer. Tools reading `attnotnull` alone type that field
non-nullable.

`result_not_null` comes from walking the query's join tree, which also covers
nesting:

```
a LEFT JOIN (b JOIN c)    ->  b and c both nullable
(a LEFT JOIN b) JOIN c    ->  only b nullable
```

`GROUP BY ROLLUP`/`CUBE`/`GROUPING SETS` are handled the same way. Where the
analysis is uncertain it reports nullable.

Not modelled: set operations, subqueries and CTEs (no provenance, so no flags),
and `CHECK` constraints or `WHERE x IS NOT NULL`. All fail safe.

## Install

### Docker

```bash
git clone https://github.com/sajonaro/pg_describe
cd pg_describe
docker compose up -d          # PGPORT=5433 docker compose up -d  if 5432 is taken
psql -h localhost -U postgres -d pg_describe_demo \
     -c "SELECT * FROM pg_describe('SELECT 1 AS n')"
```

The image builds the extension and creates it in the demo database and
`template1`.

### PGXN

```bash
pgxn install pg_describe
```

```sql
CREATE EXTENSION pg_describe;
```

Building needs the server headers (`postgresql-server-dev-17` on Debian,
`postgresql17-devel` on RHEL) and a C compiler. The `Makefile` is standard PGXS.

**PostgreSQL 17**, tested in CI on every commit. The code needs
`Query->rteperminfos`, which is PG16+, so 16 will probably work — untested.

## The function

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
  result_not_null boolean    -- can this result column be NULL. Use this one.
)
```

- `SELECT` describes its select list. `INSERT`/`UPDATE`/`DELETE` describe their
  `RETURNING` list, or nothing without one. Utility statements describe no columns.
- Expressions (`upper(x)`, `count(*)`, literals) have a type but no provenance:
  `source_table`, `source_column` and both flags are NULL. NULL means unknown;
  treat it as nullable.
- One statement per call.
- Analysis takes `AccessShareLock` on every referenced relation, held to end of
  transaction.

### Permissions

Parse analysis does not check privileges — it records them and leaves
enforcement to the executor, which is never reached here. `pg_describe` performs
the check itself before returning any row, mirroring `ExecCheckPermissions`:
relation-level rights, then per-column rights, honouring `checkAsUser`, failing
through `aclcheck_error`. `GRANT SELECT (email) ON users` lets a role describe
`SELECT email FROM users` but not `SELECT note FROM users`.

Without that check the function would expose every table's structure to any
caller. It still parses arbitrary SQL, so grant `EXECUTE` deliberately.

## TypeScript

[`pg-describe-gen`](packages/codegen) generates types from plain `.sql` files.
Query files stay valid SQL — native `$1`, no dialect.

```sql
-- queries/orders.sql
-- @name ListRecentOrders
-- Orders with their customer, if any.
SELECT o.id, o.total, c.email
FROM orders o
LEFT JOIN customers c ON c.id = o.customer_id
WHERE o.placed_at >= $1;
```

```bash
npx pg-describe-gen
```

```typescript
export interface ListRecentOrdersParams {
  p1: Date
}

export interface ListRecentOrdersRow {
  id: string            // orders.id       — bigint
  total: string         // orders.total    — numeric
  email: string | null  // customers.email — LEFT JOIN
}

/**
 * Orders with their customer, if any.
 */
export async function listRecentOrders(
  client: ClientBase,
  params: ListRecentOrdersParams,
): Promise<ListRecentOrdersRow[]>
```

`bigint` and `numeric` map to `string` because that is what node-postgres
returns. SQL comments become JSDoc.

```bash
pg-describe-gen --check    # exit 1 if the generated file is out of date
```

[`examples/typescript`](examples/typescript) is a complete runnable project.

## How it works

```c
raw   = pg_parse_query(sql);                       /* text -> RawStmt */
query = parse_analyze_varparams(rawstmt, sql,      /* RawStmt -> Query */
                                &types, &n, NULL);
/* stop: rewrite, plan and execute are never called */
```

`parse_analyze_varparams` is what `exec_parse_message` calls to serve a `Parse`
message with no declared parameter types.

[`docs/how-it-works.md`](docs/how-it-works.md) covers the `Query` tree, the join
tree walk, the permission check and error-position handling.

## Development

```bash
docker compose up -d --build
docker compose cp ./test db:/src/pg_describe/
docker compose exec db bash -lc \
  'cd /src/pg_describe && PGUSER=postgres PGHOST=/var/run/postgresql \
   PGDATABASE=contrib_regression make installcheck'
```

29 assertions: parameter inference, provenance, join shapes and nesting,
grouping sets, statement shapes, errors, permissions.

## License

MIT.

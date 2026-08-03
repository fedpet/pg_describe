# pg_describe

**Ask PostgreSQL what a query would return — without running it.**

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

Nothing was executed. The extension runs PostgreSQL's own parser and analyser,
reads what they worked out, and stops before the executor starts — so
describing `DELETE FROM orders WHERE id = $1` is perfectly safe, and tells you
that `$1` is an `integer`.

That inference is the interesting part. Nothing in the query text says
`integer`; the analyser worked it out from the column `$1` is compared against.

---

## Credit where it is due

This is not a new idea. [**pgTyped**](https://github.com/adelsz/pgtyped) by
[Adel Salakh](https://github.com/adelsz) is the project that demonstrated you
can generate honest TypeScript types from a live PostgreSQL database, and its
approach is the direct inspiration for this extension. [`sqlc`](https://sqlc.dev)
and [`sqlx`](https://github.com/launchbadge/sqlx) do the same for Go and Rust.

What those tools do is send the extended query protocol's `Parse` and
`Describe` messages and deliberately never send `Bind` or `Execute`. The server
replies with `ParameterDescription` and `RowDescription`, and the tool follows
up with catalog queries to turn type OIDs into names and `attnotnull` flags.

`pg_describe` asks the same question from inside the server instead. **If you
cannot install extensions — RDS, Cloud SQL, most managed Postgres — pgTyped is
the tool you want, and it is genuinely excellent.** This project is for when you
can.

## Why bother, if pgTyped already works

| | pgTyped / sqlc / sqlx | pg_describe |
|---|---|---|
| Where the logic lives | A wire-protocol implementation in the client | The server |
| Reimplemented per language | Yes — once for TS, Go, Rust, … | No. It is one `SELECT` |
| Round trips per query | Several (describe + catalog lookups) | One |
| Query file format | Rewritten dialect (`:paramName`) | Real SQL (`$1`) — runs in psql as-is |
| Works through PgBouncer | Depends on mode | Yes, it is an ordinary query |
| **`LEFT JOIN` nullability** | **Wrong** — reads `attnotnull` alone | **Correct** |
| Needs to install an extension | No | **Yes** — the real cost |

### The nullability thing

This is the part worth caring about.

`attnotnull` tells you whether a *source column* is `NOT NULL`. That is not the
same question as whether a *result column* can be NULL, and an outer join is
where the two come apart:

```sql
SELECT kind, ord, name, type_name, source_table::text, base_not_null, result_not_null
FROM pg_describe('SELECT o.id, c.email
                    FROM orders o
                    LEFT JOIN customers c ON c.id = o.customer_id
                   WHERE o.placed_at >= $1');
```
```
  kind  | ord | name  |        type_name         | source_table | base_not_null | result_not_null
--------+-----+-------+--------------------------+--------------+---------------+-----------------
 param  |   1 |       | timestamp with time zone |              |               |
 column |   1 | id    | bigint                   | orders       | t             | t
 column |   2 | email | text                     | customers    | t             | f
```

`customers.email` is declared `NOT NULL`. `base_not_null` says `t`, correctly.
And the result column is NULL anyway for any order without a customer, which is
what `result_not_null = f` reports.

Every tool that reads `attnotnull` alone types that field as non-nullable, and
is wrong. `LEFT JOIN` is not an exotic corner — it is in a large share of real
reporting queries, and this is precisely the class of bug the whole exercise was
meant to prevent.

`pg_describe` computes `result_not_null` by walking the query's join tree, which
also handles the nesting that trips up naive implementations:

```sql
-- a LEFT JOIN (b JOIN c)    -> b and c are BOTH nullable
-- (a LEFT JOIN b) JOIN c    -> only b is nullable
```

It also catches `GROUP BY ROLLUP`/`CUBE`/`GROUPING SETS`, which null-extend
grouping columns in the super-aggregate rows. Where the analysis cannot be sure,
it reports nullable — over-declaring a null costs you one impossible check,
under-declaring it costs you a production crash.

## Install

### Docker (fastest way to try it)

```bash
git clone https://github.com/sajonaro/pg_describe
cd pg_describe
docker compose up -d          # PGPORT=5433 docker compose up -d  if 5432 is taken
psql -h localhost -U postgres -d pg_describe_demo \
     -c "SELECT * FROM pg_describe('SELECT 1 AS n')"
```

The image builds the extension and creates it in the demo database and in
`template1`, so any database you create afterwards has it too.

### PGXN

```bash
pgxn install pg_describe
```

Then, in each database that needs it:

```sql
CREATE EXTENSION pg_describe;
```

Building from the PGXN tarball needs the PostgreSQL server headers
(`postgresql-server-dev-17` on Debian/Ubuntu, `postgresql17-devel` on RHEL) and
a C compiler. The `Makefile` is standard PGXS, so `make && make install` works
against whatever `pg_config` is on your `PATH`.

**Requirements: PostgreSQL 17.** That is what is tested, in CI, on every commit.
The code depends on `Query->rteperminfos`, which is PostgreSQL 16+, so 16 will
very likely work — but "likely" is not "tested", so it is not claimed.

## The function

```sql
pg_describe(sql text) RETURNS TABLE (
  kind            text,      -- 'param' | 'column'
  ord             int,       -- $1..$n, or column 1..n
  name            text,      -- output column name; NULL for params
  type_oid        oid,
  type_name       text,      -- SQL spelling: 'integer', 'character varying(10)'
  source_table    regclass,  -- NULL unless the column is a plain column reference
  source_column   text,
  base_not_null   boolean,   -- attnotnull on the source column
  result_not_null boolean    -- can THIS result column be NULL. Use this one.
)
```

- **Parameters are inferred.** Nothing needs declaring.
- **`SELECT`** describes its select list; **`INSERT`/`UPDATE`/`DELETE`** describe
  their `RETURNING` list, or no columns without one; **utility statements**
  describe no columns.
- **Expressions** (`upper(x)`, `count(*)`, literals) have a type but no
  provenance, so `source_table`, `source_column` and both nullability flags are
  NULL. NULL means *unknown*, and consumers should treat it as nullable.
- **Exactly one statement** per call.
- **Permissions are enforced.** See below — this matters more than it looks.

### Security

Parse analysis does *not* check privileges. It records what would need checking
and leaves enforcement to the executor, which `pg_describe` never reaches. A
naive implementation of this function would therefore hand the column names,
types and structure of every table in the database to anyone who could call it —
a schema leak, which is usually the first thing an attacker wants.

`pg_describe` performs the check itself before returning any row, mirroring
`ExecCheckPermissions`: relation-level rights first, then per-column rights
where those are missing, honouring `checkAsUser` so views behave correctly, and
failing through `aclcheck_error` so the message and SQLSTATE are identical to
what the query itself would have raised. `GRANT SELECT (email) ON users` lets a
role describe `SELECT email FROM users` and not `SELECT note FROM users`, which
is what the regression suite asserts.

It is still a function that parses arbitrary SQL, so treat `EXECUTE` on it as a
privilege worth granting deliberately.

### Cost

Analysis opens every referenced relation with `AccessShareLock`, held until the
transaction ends. Describing is cheap but neither free nor side-effect-free;
describing ten thousand queries in one transaction holds ten thousand locks.

## TypeScript

[`pg-describe-gen`](packages/codegen) turns plain `.sql` files into typed
TypeScript. Your query files stay valid SQL — native `$1` placeholders, no
dialect — so the file you generate types from is a file you can paste into
`psql`.

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
  id: string           // orders.id      — bigint: string, not number
  total: string        // orders.total   — numeric: string, not number
  email: string | null // customers.email — LEFT JOIN. pgTyped says `string`.
}

/**
 * Orders with their customer, if any.
 */
export async function listRecentOrders(
  client: ClientBase,
  params: ListRecentOrdersParams,
): Promise<ListRecentOrdersRow[]>
```

Three things there are worth noticing, and all three are places generators
commonly get it wrong:

- **`email` is nullable**, from the join tree, not the schema.
- **`bigint` and `numeric` map to `string`**, because that is what
  node-postgres actually returns — an `int8` does not survive a JavaScript
  number, and `numeric` exists precisely because floats are wrong for money.
- **The SQL comment became the JSDoc**, so the query documents itself at the
  call site.

### As a CI gate

```bash
pg-describe-gen --check    # exit 1 if the generated file is out of date
```

Put that in CI and a schema change that breaks a query fails the build instead
of the deploy. That is the entire point of the exercise.

See [`examples/typescript`](examples/typescript) for a complete, runnable
project — schema, queries, committed generated output and a demo that connects
to the Docker database and prints real rows.

## How it works

The short version:

```c
raw   = pg_parse_query(sql);                    /* text -> RawStmt: grammar only */
query = parse_analyze_varparams(rawstmt, sql,   /* RawStmt -> Query: names,      */
                                &types, &n, NULL); /*   types, inferred params   */
/* ------------------------- stop here ------------------------- */
/* pg_rewrite_query / pg_plan_query / ExecutorRun: never called   */
```

`parse_analyze_varparams` is the function `exec_parse_message` calls to serve a
`Parse` message with no declared parameter types — which is to say `pg_describe`
and pgTyped are calling the same code. One of them just has a shorter path to it.

[`docs/how-it-works.md`](docs/how-it-works.md) is the long version: the pipeline,
the `Query` tree, the join-tree walk, the permission check, and the error-position
relocation.

## Development

```bash
docker compose up -d --build                       # build image, start server
docker compose cp ./test db:/src/pg_describe/      # push the test suite
docker compose exec db bash -lc \
  'cd /src/pg_describe && PGUSER=postgres PGHOST=/var/run/postgresql \
   PGDATABASE=contrib_regression make installcheck'
```

The suite is 29 assertions and covers parameter inference, provenance, all the
join shapes including the two nesting cases, grouping sets, statement shapes,
error handling and four permission scenarios.

## License

MIT. See [LICENSE](LICENSE).

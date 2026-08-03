---
id: features
title: Features
sidebar_label: Features
---

- **Nothing is executed.** Parse and analysis only; the executor is never
  reached. Describing `DELETE FROM orders WHERE id = $1` deletes nothing.
- **Parameter types are inferred.** `$1` in `WHERE placed_at >= $1` is reported
  as `timestamptz` without anything in the query text declaring it.
- **Result columns come back as the wire protocol sees them** — name, type OID
  and SQL type name including typmod, with `ORDER BY`-only columns excluded just
  as `RowDescription` excludes them.
- **Outer-join-aware nullability.** `result_not_null` is computed by walking the
  query's join tree, so a column on the nullable side of a `LEFT JOIN` is
  reported nullable even when it is declared `NOT NULL`. Nesting
  (`a LEFT JOIN (b JOIN c)`) and `GROUP BY ROLLUP`/`CUBE`/`GROUPING SETS` are
  handled; everything uncertain fails safe. See [Nullability](nullability.md).
- **Column provenance.** `source_table` and `source_column` for plain column
  references, so a generator can name the origin of every field.
- **One round trip.** It is an ordinary `SELECT`, so it works through
  connection poolers and from any client in any language, with no wire-protocol
  code to write.
- **Privileges are enforced.** The function performs the permission check that
  the executor would have performed, so it cannot be used to read the schema of
  tables you have no rights to. See [Permissions](permissions.md).
- **Errors point at your query.** A parse error's caret is relocated into the
  described statement rather than the `pg_describe(` call.

## TypeScript code generation

[`pg-describe-gen`](cli-configuration.md) is bundled:

- **Query files are plain SQL.** Native `$1` placeholders, no dialect — the file
  you generate types from is the file you can paste into `psql`.
- **Typed parameters, rows and functions** generated per query, with SQL
  comments carried through as JSDoc.
- **`--check` mode** for CI: exit 1 when the committed output no longer matches
  what the database says, so a breaking migration fails the pull request rather
  than the deploy.
- **Configurable type mapping**, with sensible defaults — `bigint` and `numeric`
  map to `string` because that is what node-postgres returns. See
  [Type mapping](type-mapping.md).

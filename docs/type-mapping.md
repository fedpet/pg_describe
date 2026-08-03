---
id: type-mapping
title: Type mapping
sidebar_label: Type mapping
---

How `pg-describe-gen` turns a PostgreSQL type name into a TypeScript type.

| PostgreSQL | TypeScript | |
|---|---|---|
| `smallint`, `integer`, `real`, `double precision` | `number` | |
| `bigint` | `string` | an int8 does not fit in a JS number; node-postgres returns a string |
| `numeric`, `decimal` | `string` | arbitrary precision, and floats are wrong for money |
| `boolean` | `boolean` | |
| `text`, `varchar`, `char`, `uuid`, `citext`, `inet`, `interval` | `string` | |
| `date`, `timestamp`, `timestamptz` | `Date` | |
| `time`, `time with time zone` | `string` | no `Date` equivalent |
| `bytea` | `Buffer` | |
| `json`, `jsonb` | `unknown` | narrow it yourself |
| `T[]` | `T[]` | any dimension |
| anything else | `unknown` | with a warning |

The mapping targets **what node-postgres actually returns at run time**, not
what the SQL type resembles. `bigint` and `numeric` arriving as strings is the
case that surprises people, and typing them `number` would be a lie the compiler
would then help you build on.

## Overriding

Enums, domains and user-defined types fall through to `unknown` with a warning.
Map them, or override any built-in, with a `types` block in the config:

```json
{
  "queries": "queries",
  "output": "src/generated/queries.ts",
  "types": {
    "order_status": "'pending' | 'shipped' | 'cancelled'",
    "bigint": "bigint",
    "jsonb": "Record<string, unknown>"
  }
}
```

Keys are PostgreSQL type names as `pg_describe` reports them in `type_name`; the
quickest way to get the exact spelling is to run the query and look:

```sql
SELECT DISTINCT type_name FROM pg_describe($$SELECT * FROM orders$$);
```

Values are emitted verbatim into the generated file, so any TypeScript type
expression works — a union, an imported type name, a branded type. Nothing
validates it beyond `tsc` on the generated output.

If you map `bigint` to the JS `bigint`, remember to configure node-postgres to
parse int8 accordingly; the generator describes types, it does not install
parsers.

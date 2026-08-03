---
id: cli-configuration
title: CLI usage and configuration
sidebar_label: CLI usage and configuration
---

```bash
npm install --save-dev pg-describe-gen
```

Requires Node 18+ and a database with the `pg_describe` extension installed. The
generator checks for the extension on connect and fails with an actionable
message rather than letting every query fail with "function pg_describe(unknown)
does not exist".

## Usage

```
pg-describe-gen [options]

Options:
  -c, --config <path>   Config file (default: pg-describe.json)
      --check           Do not write. Exit 1 if the output is out of date.
  -h, --help            Show this message
```

## The config file

```json
// pg-describe.json
{
  "queries": "queries",
  "output": "src/generated/queries.ts"
}
```

| Key | Required | Meaning |
|---|---|---|
| `queries` | yes | Directory scanned recursively for `.sql` files |
| `output` | yes | File the generated TypeScript is written to |
| `types` | no | Type mapping overrides — see [Type mapping](type-mapping.md) |

Paths are resolved relative to the config file, not the working directory, so
`pg-describe-gen -c packages/api/pg-describe.json` works from the repository
root.

## Connection settings

Deliberately not in the config file. The generator uses node-postgres, which
reads:

- `DATABASE_URL`, or
- the standard `PGHOST` / `PGPORT` / `PGUSER` / `PGPASSWORD` / `PGDATABASE`
  variables

Credentials stay out of a file you want to commit, and the generator connects
the same way the rest of your application does.

```bash
export DATABASE_URL=postgres://postgres:postgres@localhost:5432/pg_describe_demo
npx pg-describe-gen
```

## What a run does

1. Loads the config and discovers every annotated query under `queries`.
2. Connects, and verifies the extension is installed.
3. Sends each statement to `pg_describe` in its own round trip. Nothing is
   executed. A statement that fails to analyse is reported with its file and
   line, and the run continues, so one broken query does not hide the other
   nineteen.
4. Exits 1 if any query failed.
5. Writes the output file, creating parent directories as needed, and reports
   how many queries it covers.

A PostgreSQL type with no TypeScript mapping is emitted as `unknown` and warned
about by name, pointing at the `types` config block:

```
warning: no TypeScript mapping for PostgreSQL type "order_status"; used `unknown`.
         Add it to "types" in your config to fix this.
```

## Checking in CI

```bash
npx pg-describe-gen --check
```

Writes nothing. Exits 0 when the committed output matches what the database
says, and 1 when it does not:

```
src/generated/queries.ts is out of date. Run pg-describe-gen to regenerate it.
```

A missing output file is reported separately, as `does not exist`.

This is the point of the tool in a pipeline. A migration that changes what a
query returns fails the pull request, next to the migration that caused it,
rather than the deploy. A CI job needs a database with the schema applied and
the extension installed — the repository's own
[workflow](https://github.com/sajonaro/pg_describe/blob/main/.github/workflows/ci.yml)
does exactly this with the project's Docker image.

## Programmatic use

The package also exports its pieces, if you want to build something other than a
TypeScript file — a linter, a migration impact report, an editor integration:

```typescript
import { describe, discover, emit, loadConfig } from 'pg-describe-gen'
```

`describe(client, sql)` is the thin one: it runs `pg_describe` and returns
`{ params, columns }`.

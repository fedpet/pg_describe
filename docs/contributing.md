---
id: contributing
title: Contributing
sidebar_label: Contributing
---

## Repository layout

```
src/pg_describe.c        the extension
sql/                     the extension's SQL script
test/                    pg_regress suite: sql/ and expected/
packages/codegen/        pg-describe-gen, published to npm
examples/typescript/     runnable example, generated output committed
docs/                    documentation source
website/                 Docusaurus site that renders docs/
```

`packages/*` and `examples/*` are npm workspaces, so a single `npm install` at
the repository root wires the example to the local generator. `website/` is
deliberately outside the workspaces, so building the generator does not pull a
documentation toolchain.

## The extension

```bash
docker compose up -d --build
docker compose cp ./test db:/src/pg_describe/
docker compose exec db bash -lc \
  'cd /src/pg_describe && PGUSER=postgres PGHOST=/var/run/postgresql \
   PGDATABASE=contrib_regression make installcheck'
```

29 assertions covering parameter inference, provenance, join shapes and their
nesting, grouping sets, statement shapes, error handling and four permission
scenarios. `regression.diffs` holds the failure detail.

A change to the analysis belongs with a test in `test/sql/pg_describe.sql` and
its expected output — the suite is the specification of what the flags mean.

## The generator

```bash
npm install            # from the repository root
npm run build

docker compose up -d --build
docker compose exec -T db psql -U postgres -d pg_describe_demo -q \
  < examples/typescript/schema.sql

export PGHOST=localhost PGPORT=5432 PGUSER=postgres \
       PGPASSWORD=postgres PGDATABASE=pg_describe_demo

npm run check          # committed output still matches the database
npm run typecheck
npm run demo
```

`npm run check` is the assertion that matters: regenerating against a live
database must reproduce the committed file byte for byte. It fails if the
extension's output changes, if the emitter changes, or if the schema drifts.
When a change is meant to alter the output, run `npm run generate` and commit
the result as part of the same change.

## The documentation

Pages live in `docs/` as plain Markdown with Docusaurus frontmatter, and render
on GitHub as they are. The site is built separately:

```bash
npm --prefix website install
npm run docs           # local dev server with hot reload
npm run docs:build     # production build into website/build
```

New pages need an entry in `website/sidebars.ts`.

## CI

Every push and pull request builds the extension image, runs `pg_regress`,
builds the generator, regenerates the example against a live database and
type-checks it, and builds the documentation site. The site deploys to GitHub
Pages from `main`.

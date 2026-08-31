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

53 assertions covering parameter inference, provenance, expression
nullability, join shapes and their nesting, grouping sets, statement shapes,
error handling and four permission scenarios. `regression.diffs` holds the failure detail.

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

## Releasing

Both halves ship from one tag.

```bash
# bump packages/codegen/package.json to the new version first, then:
git tag -a v1.1.0 -m "pg_describe 1.1.0"
git push origin v1.1.0
```

The tag does two things:

- **npm.** `.github/workflows/publish-npm.yml` publishes `pg-describe-gen` via
  trusted publishing — GitHub mints a short-lived OIDC token, npm accepts it
  because the package's trusted publisher names this repository and that
  workflow file, and provenance is attached automatically. No token exists to
  leak. The job refuses to run if the tag and `package.json` disagree, and skips
  cleanly if the version is already on the registry.
- **PGXN.** Build the distribution archive and upload it at
  https://manager.pgxn.org/ — this step is manual, since PGXN has no API token
  flow here.

  ```bash
  git archive --format=zip --prefix=pg_describe-1.1.0/ -o pg_describe-1.1.0.zip v1.1.0
  ```

  `website/` is `export-ignore`d, so the archive carries the extension and its
  tests rather than the documentation toolchain.

A release also needs `pg_describe.control`, `META.json` and the versioned file
in `sql/` to agree on the number, and a matching
`sql/pg_describe--<old>--<new>.sql` upgrade script when the SQL interface
changes.

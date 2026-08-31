---
id: installation
title: Installation
sidebar_label: Installation
---

## Requirements

**PostgreSQL 17 or 18.** Both are what CI builds and tests on every commit. The
code depends on `Query->rteperminfos`, which is PostgreSQL 16+, so 16 will
probably work — but "probably" is not "tested", so it is not claimed.

The two servers differ in one way that matters here. From v18 the parser
interposes an `RTE_GROUP` range table entry between the target list and the
range table whenever a query groups, so a grouping column resolves to the
grouping step rather than to the relation it came from. `pg_describe` resolves
through it, which is why a grouped column reports the same provenance and
nullability on both servers.

You need to be able to install extensions in the target database. On RDS, Cloud
SQL and most managed Postgres you cannot; see [Credit](credit.md) for the tool
that works there.

## Docker

The fastest way to get a server with the extension already loaded:

```bash
git clone https://github.com/sajonaro/pg_describe
cd pg_describe
docker compose up -d          # PGPORT=5433 docker compose up -d  if 5432 is taken
```

The image builds the extension and runs `CREATE EXTENSION` in the demo database
and in `template1`, so databases created later inherit it.

## PGXN

```bash
pgxn install pg_describe
```

Then, in each database that needs it:

```sql
CREATE EXTENSION pg_describe;
```

## From source

Building needs the PostgreSQL server headers and a C compiler:

| Distribution | Package |
|---|---|
| Debian / Ubuntu | `postgresql-server-dev-17` |
| RHEL / Fedora | `postgresql17-devel` |

The `Makefile` is standard PGXS, so it builds against whatever `pg_config` is on
your `PATH`:

```bash
make
sudo make install
```

```sql
CREATE EXTENSION pg_describe;
```

To run the regression suite against a server you control:

```bash
PGUSER=postgres PGDATABASE=contrib_regression make installcheck
```

## The TypeScript generator

`pg-describe-gen` is published separately on npm and needs Node 18+ and a
database with the extension installed:

```bash
npm install --save-dev pg-describe-gen
```

See [CLI usage and configuration](cli-configuration.md).

# pg_describe

[![CI](https://github.com/fedpet/pg_describe/actions/workflows/ci.yml/badge.svg)](https://github.com/fedpet/pg_describe/actions/workflows/ci.yml)
[![PostgreSQL 18](https://img.shields.io/badge/PostgreSQL-18-336791)](https://www.postgresql.org/)
[![License: MIT](https://img.shields.io/badge/License-MIT-green.svg)](LICENSE)

**pg_describe reports what a query would return, without running it.**

An independently maintained fork of
[sajonaro/pg_describe](https://github.com/sajonaro/pg_describe), focused on
language-neutral query metadata for code generators and developer tools.
Supported and tested on PostgreSQL 18.

```sql
SELECT ord, name, type_name, result_not_null
FROM pg_describe('SELECT id, email FROM users WHERE id = $1');
```

The extension uses PostgreSQL's parser and analyser. It reports parameter and
result types, source-column provenance, outer-join-aware nullability, recursive
JSON shapes, and provable SQL array rank and element nullability. Rewrite,
planning and execution are never reached.
`pg_describe_functions` discovers built-in and user routines in bulk and reports
symbolic non-null guarantees using the same function analysis as query
description. Statement/COPY transport metadata and finite value proofs are exposed
through `pg_describe_facts` and `pg_describe_type_facts`. Access privileges are checked before metadata is returned.

## Get started

```bash
git clone https://github.com/fedpet/pg_describe
cd pg_describe
docker compose up -d --build  # PGPORT=5433 if 5432 is taken

psql -h localhost -U postgres -d pg_describe_demo \
  -c "SELECT * FROM pg_describe('SELECT 1 AS n')"
```

To build against an existing PostgreSQL installation:

```bash
make
sudo make install
```

```sql
CREATE EXTENSION pg_describe;
```

See [Installation](docs/installation.md) for requirements and upgrades.

## Documentation

- [Overview](docs/overview.md)
- [Getting started](docs/getting-started.md)
- [Function reference](docs/function-reference.md)
- [Nullability](docs/nullability.md)
- [Permissions](docs/permissions.md)
- [How it works](docs/how-it-works.md)
- [Contributing and releases](docs/contributing.md)
- [Changelog](CHANGELOG.md)

The extension is callable from any PostgreSQL client. Consumers own their code
generation and language-specific type mapping.

## Credit and license

Originally created by [sajonaro](https://github.com/sajonaro/pg_describe).
Inspired by [pgTyped](https://github.com/adelsz/pgtyped),
[sqlc](https://sqlc.dev), and [sqlx](https://github.com/launchbadge/sqlx).
See [Credit](docs/credit.md) and [LICENSE](LICENSE).

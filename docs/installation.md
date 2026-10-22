# Installation

## Requirements

Supported and tested on **PostgreSQL 18**. Building requires a C compiler,
PostgreSQL server headers and PGXS. The extension must be compiled against the
server major version where it will run.

You need permission to install extensions in the target database. Check your
provider's extension policy when using a managed database.

## Docker

```bash
git clone https://github.com/fedpet/pg_describe
cd pg_describe
docker compose up -d --build  # PGPORT=5433 if 5432 is taken
```

The image builds the extension and creates it in the demo database and in
`template1`, so databases created later inherit it. The image is a development
server with regression-test tooling; it is not a prebuilt production artifact.

## From source

Download a versioned source archive from
[GitHub releases](https://github.com/fedpet/pg_describe/releases), or check out
its tag. The source tree can also be built directly during development.

Install the server headers:

| Distribution | Package |
|---|---|
| Debian / Ubuntu | `postgresql-server-dev-18` |
| RHEL / Fedora | `postgresql18-devel` |

The Makefile uses the `pg_config` on your PATH. To select a specific installation:

```bash
make PG_CONFIG=/path/to/pg_config
sudo make PG_CONFIG=/path/to/pg_config install
```

In each database that needs it:

```sql
CREATE EXTENSION pg_describe;
```

It can also be installed in a dedicated schema:

```sql
CREATE SCHEMA extensions;
CREATE EXTENSION pg_describe SCHEMA extensions;
```

## Upgrading

Install the new extension files, then reconnect before updating each database:

```sql
ALTER EXTENSION pg_describe UPDATE TO '2.0.0';
```

Version 2 supports upgrades from 1.0.0 and 1.1.0. Its return type adds metadata,
so the update recreates `pg_describe(text)`. Views or other database objects that
depend on the old function must be migrated explicitly; the script does not use
`CASCADE`. Custom function comments and execution grants must be reapplied.
Perform the update and any permission changes in one transaction if access to
the function is restricted.

Long-lived sessions can retain a previously loaded shared library. Recycle those
connections after replacing the extension files.

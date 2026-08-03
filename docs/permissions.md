---
id: permissions
title: Permissions
sidebar_label: Permissions
---

## Why the function has to check anything

Parse analysis does not check privileges. It records what would need checking in
`Query->rteperminfos` and leaves enforcement to the executor — which
`pg_describe` never reaches.

A naive implementation would therefore hand the column names, types and
relationships of every table in the database to anyone who could call it. Schema
disclosure is usually the first thing an attacker wants.

## What it does instead

`pg_describe` performs the check itself, before returning any row, mirroring
`ExecCheckPermissions`:

- **Relation-level rights first**, via `pg_class_aclmask`.
- **Column-level rights** where relation-level rights are absent — only a
  missing `SELECT` can be rescued by a column grant, which is why the specific
  missing bits matter.
- **`checkAsUser` is honoured**, so a view is checked against its owner's
  privileges, as it would be when executed.
- **Failure goes through `aclcheck_error`**, so the message, SQLSTATE and object
  naming are identical to what running the query itself would have raised.

```sql
GRANT SELECT (email) ON users TO reporting;
```

```sql
-- as reporting
SELECT * FROM pg_describe('SELECT email FROM users');
-- describes the query

SELECT * FROM pg_describe('SELECT note FROM users');
-- ERROR:  permission denied for table users

SELECT * FROM pg_describe('SELECT * FROM users');
-- ERROR:  permission denied for table users
```

`SELECT *` fails where `SELECT email` succeeds because a whole-row reference is
not satisfied by any column-level grant. A query touching no table at all —
`SELECT 1 + 1` — needs no table rights and is described for anyone who can call
the function. All of these are in the regression suite.

## Granting EXECUTE

It is still a function that parses arbitrary SQL on the server. The permission
check means it cannot be used to read a schema the caller has no rights to, but
`EXECUTE` on it is a privilege worth granting deliberately rather than leaving
to `PUBLIC` out of habit.

For a code generator, the role it connects as needs `SELECT` on the tables its
queries read — which it would need anyway to run them.

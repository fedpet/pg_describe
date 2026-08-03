---
id: how-it-works
title: How it works
sidebar_label: How it works
---

# How pg_describe works

## Stopping the pipeline

PostgreSQL's stages are ordinary C functions; nothing requires calling all of
them.

```c
List  *raw   = pg_parse_query(sql);
Query *query = parse_analyze_varparams(rawstmt, sql, &param_types, &num_params, NULL);
/* rewrite, plan, execute: never called */
```

`pg_parse_query` is grammar only — no catalog access, no name resolution. It
parses `SELECT * FROM nonexistent` without complaint.

`parse_analyze_varparams` resolves names against the catalog, types every
expression, and infers a type for each `$n`. The `varparams` variant takes an
empty parameter array and grows one, filling each type from the context the
parameter appears in; the `fixedparams` sibling is used when a client declares
its types. `exec_parse_message` calls the former to serve a `Parse` message with
an empty type list, so a client sending `Parse`/`Describe` runs this same code.

## Reading the Query

| Wire protocol | `Query` field |
|---|---|
| `ParameterDescription.oid[]` | `param_types[0..num_params)` |
| `RowDescription.name`, `.typeOID` | `targetList` → `TargetEntry->resname`, `exprType(tle->expr)` |
| `RowDescription.tableOID`, `.columnAttrNumber` | `IsA(tle->expr, Var)` → `rt_fetch(var->varno, query->rtable)` |
| catalog query for `attnotnull` | `SearchSysCache2(ATTNUM, ...)` |

The syscache lookup replaces a network round trip with a hash lookup over a
relation analysis has already locked.

| `commandType` | Columns |
|---|---|
| `CMD_SELECT` | `targetList` |
| `CMD_INSERT` / `CMD_UPDATE` / `CMD_DELETE` | `returningList`, empty without `RETURNING` |
| `CMD_UTILITY` | none; parked un-analysed in `utilityStmt` |

Two filters apply. `resjunk` entries are skipped — the sort key `ORDER BY` adds
for an unselected column is in the list but not the result, and
`RowDescription` omits it too. Provenance requires a bare `Var` with
`varlevelsup == 0` and `varattno > 0`; expressions, correlated references to an
enclosing query, whole-row references and system columns get a type but no
source.

## The join tree walk

```c
static void
find_nullable(Node *jtnode, Bitmapset **nullable)
{
    if (IsA(jtnode, FromExpr))
        foreach(lc, ((FromExpr *) jtnode)->fromlist)
            find_nullable((Node *) lfirst(lc), nullable);

    else if (IsA(jtnode, JoinExpr))
    {
        JoinExpr *j = (JoinExpr *) jtnode;

        find_nullable(j->larg, nullable);
        find_nullable(j->rarg, nullable);

        if (j->jointype == JOIN_LEFT || j->jointype == JOIN_FULL)
            collect_rtindexes(j->rarg, nullable);

        if (j->jointype == JOIN_RIGHT || j->jointype == JOIN_FULL)
            collect_rtindexes(j->larg, nullable);
    }
}
```

`collect_rtindexes` gathers every base relation beneath a node. Combined with
recursing into both arms before applying this node's rule, that handles nesting:

```
a LEFT JOIN (b JOIN c)       b and c. If the outer join finds no match the whole
                             right subtree is null-extended at once, so the
                             inner join between them is irrelevant.
(a LEFT JOIN b) JOIN c       b only.
a LEFT JOIN (b LEFT JOIN c)  b and c.
```

All three are in the regression suite; an implementation inspecting only each
join's immediate arms passes the second and fails the first.

`GROUP BY ROLLUP`, `CUBE` and `GROUPING SETS` null-extend grouping columns in
super-aggregate rows. When `query->groupingSets` is non-empty, no
provenance-bearing column is reported non-null.

### Not modelled

- **Set operations.** `UNION` branches are subquery RTEs, so target-list `Var`s
  do not resolve to a base relation and no flags are reported.
- **Subqueries and CTEs.** Same.
- **`CHECK` constraints, partial indexes, `WHERE x IS NOT NULL`.** Predicates
  that guarantee non-nullness are not consulted.

All report nullable or unknown, so all fail safe.

## Permissions

Parse analysis records privilege requirements in `Query->rteperminfos` and
leaves enforcement to `ExecutorStart`, which is never reached. Skipping the
check would expose every column name, type and relationship in the database to
any caller.

```c
userid = OidIsValid(perminfo->checkAsUser) ? perminfo->checkAsUser : GetUserId();

remaining = perminfo->requiredPerms &
    ~pg_class_aclmask(relid, userid, perminfo->requiredPerms, ACLMASK_ALL);
```

Four details:

- **`checkAsUser`** is set when the rights to check are not the current role's —
  a view runs with its owner's privileges.
- **`aclmask`, not `aclcheck`**: only a missing `SELECT` can be rescued by
  column-level grants, so the specific missing bits matter.
- **Column-level grants** are checked when relation-level rights are absent.
- **`selectedCols` is offset** by `FirstLowInvalidHeapAttributeNumber` (`-7`),
  since system columns have negative attnums and `Bitmapset` members cannot.
  `InvalidAttrNumber` after the shift means whole-row, which no column grant
  satisfies — hence `SELECT *` failing where `SELECT email` succeeds.

Failure goes through `aclcheck_error`, so message, SQLSTATE and object naming
match what the query itself would have raised.

## Error positions

A parse error's cursor position is measured against the string being parsed.
Unchanged, the client draws the caret against the outer statement:

```
ERROR:  syntax error at or near "WHERE"
LINE 1: SELECT * FROM pg_describe('SELECT FROM WHERE');
                    ^
```

An error context callback runs while the error is being built and can still edit
its fields, so the position is re-reported as internal, against the inner query:

```c
static void
describe_error_cb(void *arg)
{
    int pos = geterrposition();

    if (pos > 0)
    {
        errposition(0);
        internalerrposition(pos);
        internalerrquery((const char *) arg);
    }
}
```

```
ERROR:  syntax error at or near "WHERE"
LINE 1: SELECT FROM WHERE
                    ^
QUERY:  SELECT FROM WHERE
```

## Errors abort the transaction

Analysis errors are not caught. `pg-describe-gen` therefore describes each query
independently and collects failures rather than stopping at the first.

Catching them in the extension needs `BeginInternalSubTransaction` with the
matching `RollbackAndReleaseCurrentSubTransaction` and saved
`CurrentMemoryContext` / `CurrentResourceOwner`. A bare `PG_TRY`/`PG_CATCH`
returns control without undoing locks, buffer pins or aborted transaction state.

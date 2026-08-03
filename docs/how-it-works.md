# How pg_describe works

## The pipeline is separately callable

PostgreSQL turns SQL text into rows in stages:

```
text ──► parse ──► analyse ──► rewrite ──► plan ──► execute ──► rows
```

Most descriptions of this present it as a sequence you ride from end to end.
The thing that makes this extension possible is that **the stages are ordinary C
functions and nothing obliges you to call all of them**:

```c
List  *raw   = pg_parse_query(sql);
Query *query = parse_analyze_varparams(rawstmt, sql, &param_types, &num_params, NULL);
/* stop */
```

Two stages, very different animals.

**`pg_parse_query` is pure grammar.** It touches no catalog and resolves no
name. It will happily parse `SELECT * FROM table_that_does_not_exist`. Errors
here are syntax errors and nothing else.

**`parse_analyze_varparams` is where everything happens.** It resolves every
name against the catalog, assigns a type to every expression, and infers a type
for each `$n`.

That last part is the whole trick. The function has a sibling,
`pg_analyze_and_rewrite_fixedparams`, used when a client *declares* its parameter
types. The `varparams` variant is used when it does not: you hand it an empty
parameter array and it grows one, filling in each type from the context the
parameter appears in. It is what `exec_parse_message` calls to serve a `Parse`
message with an empty type list.

So when a client sends `Parse` + `Describe` and never sends `Bind`, the server
runs exactly this code. `pg_describe` calls it directly instead of asking for it
over a socket.

## What comes back

| Wire protocol | `Query` field |
|---|---|
| `ParameterDescription.oid[]` | `param_types[0..num_params)` |
| `RowDescription.name`, `.typeOID` | `targetList` → `TargetEntry->resname`, `exprType(tle->expr)` |
| `RowDescription.tableOID`, `.columnAttrNumber` | `IsA(tle->expr, Var)` → `rt_fetch(var->varno, query->rtable)` |
| a catalog query for `attnotnull` | `SearchSysCache2(ATTNUM, ...)` |

That last row is the efficiency argument. A client pays a network round trip to
read `pg_attribute`; in the backend it is a hash lookup in memory the process
already holds, over a relation the analysis has already opened and locked.

### Where the columns live

| `commandType` | Columns |
|---|---|
| `CMD_SELECT` | `targetList` |
| `CMD_INSERT` / `CMD_UPDATE` / `CMD_DELETE` | `returningList` — empty without `RETURNING` |
| `CMD_UTILITY` | none; the statement is parked un-analysed in `utilityStmt` |

Two filters apply to the target list. **`resjunk` entries are skipped** — the
sort key `ORDER BY` adds for a column you did not select is in the list but not
in the result, and `RowDescription` does not report it either. **Provenance
requires a bare `Var`** whose `varlevelsup` is 0 and whose `varattno` is
positive; anything else (an expression, a correlated reference to an enclosing
query, a whole-row or system column) gets a type but no source.

## Nullability: the join tree walk

`attnotnull` answers "is this *source column* declared `NOT NULL`". A code
generator needs the answer to "can this *result column* be NULL", and an outer
join separates them.

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

        find_nullable(j->larg, nullable);      /* nested outer joins first */
        find_nullable(j->rarg, nullable);

        if (j->jointype == JOIN_LEFT || j->jointype == JOIN_FULL)
            collect_rtindexes(j->rarg, nullable);

        if (j->jointype == JOIN_RIGHT || j->jointype == JOIN_FULL)
            collect_rtindexes(j->larg, nullable);
    }
}
```

`collect_rtindexes` gathers **every** base relation beneath a node, however
deep. That, plus recursing into both arms before applying this node's own rule,
is what makes nesting come out right:

```
a LEFT JOIN (b JOIN c)     b and c are both nullable.
                           The inner join between them is irrelevant: if the
                           outer join finds no match, the whole right subtree is
                           null-extended at once.

(a LEFT JOIN b) JOIN c     Only b. c is inner-joined onto the result afterwards.

a LEFT JOIN (b LEFT JOIN c)  b and c. Nesting compounds.
```

All three are in the regression suite, because an implementation that only looks
at the immediate arms of each join passes the second and fails the first.

A second null source gets the same treatment: `GROUP BY ROLLUP`, `CUBE` and
`GROUPING SETS` emit super-aggregate rows in which the grouping columns are
NULL, no matter how `NOT NULL` the underlying column is. When
`query->groupingSets` is non-empty, no provenance-bearing column is reported as
guaranteed non-null.

The rule throughout is to err toward nullable. Over-declaring costs a consumer
one impossible check; under-declaring costs them a crash.

### What it does not model

Honest limits, so nobody is surprised:

- **`UNION` and friends.** Set operations put their branches in subquery RTEs,
  so target-list `Var`s do not resolve to a base relation and no provenance is
  reported at all — the columns come back with NULL nullability, which consumers
  treat as nullable. Conservative, not precise.
- **Subqueries and CTEs.** Same: a column selected out of a subquery has no base
  relation, so no flags.
- **`CHECK` constraints, partial indexes, `WHERE x IS NOT NULL`.** A predicate
  that guarantees non-nullness in practice is not consulted. `result_not_null`
  can be `false` for a column that never actually contains NULL.

All three fail safe.

## Permissions

Parse analysis does not check privileges. It records the requirement in
`Query->rteperminfos` and leaves enforcement to `ExecutorStart`, which
`pg_describe` never reaches.

Skipping the check would not leak data. It would leak the **schema** — every
column name, type and relationship in the database, to anyone who can call the
function.

So the extension does the check itself, mirroring `ExecCheckOneRelPerms`:

```c
userid = OidIsValid(perminfo->checkAsUser) ? perminfo->checkAsUser : GetUserId();

remaining = perminfo->requiredPerms &
    ~pg_class_aclmask(relid, userid, perminfo->requiredPerms, ACLMASK_ALL);
```

Four details, each a bug if you get it wrong:

**`checkAsUser`** is set when the rights to check are not the current role's — a
view runs with its owner's privileges. Ignoring it denies access the executor
would allow.

**`aclmask`, not `aclcheck`**, because we need to know *which* required bits are
missing: only a missing `SELECT` can be rescued by column-level grants.

**Column-level grants are real.** `GRANT SELECT (email) ON users TO app` is a
common way to hold partial access, so a relation-level denial falls back to
checking each column in `selectedCols`.

**`selectedCols` is offset** by `FirstLowInvalidHeapAttributeNumber` (`-7`),
because system columns have negative attnums and a `Bitmapset` member cannot be
negative. Forget the offset and you check the wrong column — silently, and in
the permissive direction. An attribute number of `InvalidAttrNumber` after the
shift means "the whole row", which no column grant satisfies, and is why
`SELECT *` correctly fails where `SELECT email` succeeds.

Failure goes through `aclcheck_error`, so the message, the SQLSTATE and the
object naming match what the query itself would have produced. A caller should
not be able to tell this check apart from the executor's.

## Error positions

A parse error carries a cursor position measured against the string being
parsed. Report it unchanged and the client draws the caret against the *outer*
statement, where it lands on nothing:

```
ERROR:  syntax error at or near "WHERE"
LINE 1: SELECT * FROM pg_describe('SELECT FROM WHERE');
                    ^
```

SPI has the same problem and solves it with an error context callback, which
runs while an error is being built — after `ereport` has decided to raise, before
the message is finalised — and can still edit the fields:

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

The position is re-reported as an *internal* one against the inner query, which
clients render as a separate `QUERY:` line:

```
ERROR:  syntax error at or near "WHERE"
LINE 1: SELECT FROM WHERE
                    ^
QUERY:  SELECT FROM WHERE
```

## Errors abort the transaction

`pg_describe` does not catch analysis errors. For interactive use that is
correct. For a generator describing two hundred queries, one typo kills the
batch — which is why `pg-describe-gen` describes each query and collects the
failures rather than stopping at the first.

Catching them inside the extension is harder than it looks. A bare
`PG_TRY`/`PG_CATCH` gives control back but undoes nothing: locks taken, buffer
pins and the aborted transaction state are all still there, and the session
limps on to fail confusingly later. Doing it properly needs
`BeginInternalSubTransaction` with the matching
`RollbackAndReleaseCurrentSubTransaction`, plus saving and restoring
`CurrentMemoryContext` and `CurrentResourceOwner`. That is a plausible future
`pg_describe_safe()`; it is deliberately not what the plain function does.

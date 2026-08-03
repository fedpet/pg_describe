/*
 * pg_describe -- report what a query WOULD return, without running it.
 *
 * The mechanism is the front half of the server's own query pipeline:
 *
 *     pg_parse_query()           raw text -> RawStmt      (grammar only)
 *     parse_analyze_varparams()  RawStmt  -> Query        (names, types, params)
 *     ------------------------ we stop here ------------------------
 *     pg_rewrite_query() / pg_plan_query() / ExecutorRun()  (never reached)
 *
 * parse_analyze_varparams() is the function the wire protocol's Parse message
 * uses when the client declares no parameter types, which is why $1 comes back
 * with a real type instead of an error: the analyser infers it from the context
 * the parameter is used in.
 *
 * Clients have been able to ask this question for twenty years, by sending
 * Parse + Describe and never sending Bind or Execute. Doing it from inside the
 * server instead means one ordinary SELECT answers it, for any driver in any
 * language, with no wire-protocol code anywhere.
 *
 * Three parts of this file are load-bearing and easy to get wrong:
 *
 *   check_permissions()  Parse analysis does NOT check privileges -- it records
 *                        them and leaves enforcement to the executor, which we
 *                        never reach. Without this, pg_describe would hand the
 *                        schema of every table to anyone who can call it.
 *
 *   find_nullable()      attnotnull describes the SOURCE column. Under an outer
 *                        join the RESULT column can be NULL anyway. This is the
 *                        walk that tells the two apart, and it is the reason
 *                        this extension generates correct types where tools
 *                        reading attnotnull alone do not.
 *
 *   describe_error_cb()  parse errors carry a cursor position measured against
 *                        the inner query; without relocating it the caret is
 *                        drawn against the outer call, pointing at nothing.
 */
#include "postgres.h"
#include "fmgr.h"

#include "funcapi.h"

#include "access/htup_details.h"
#include "access/sysattr.h"
#include "catalog/objectaddress.h"
#include "catalog/pg_attribute.h"
#include "miscadmin.h"
#include "nodes/bitmapset.h"
#include "nodes/nodeFuncs.h"
#include "nodes/parsenodes.h"
#include "parser/analyze.h"
#include "parser/parsetree.h"
#include "tcop/tcopprot.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/syscache.h"

PG_MODULE_MAGIC;

/* The nine columns declared by RETURNS TABLE, in order. */
#define PD_COL_KIND             0
#define PD_COL_ORD              1
#define PD_COL_NAME             2
#define PD_COL_TYPE_OID         3
#define PD_COL_TYPE_NAME        4
#define PD_COL_SOURCE_TABLE     5
#define PD_COL_SOURCE_COLUMN    6
#define PD_COL_BASE_NOT_NULL    7
#define PD_COL_RESULT_NOT_NULL  8
#define PD_NCOLS                9

/*
 * Nullability flags are tri-state: true, false, or unknown. Unknown is a real
 * and distinct answer -- an expression column has no attnotnull to report, and
 * saying "false" there would be a claim we have not earned. Consumers should
 * treat unknown as nullable; the reference code generator does.
 */
#define PD_UNKNOWN            (-1)

/* ------------------------------------------------------------------------
 * Output
 * --------------------------------------------------------------------- */

static void
emit_row(ReturnSetInfo *rsinfo,
         const char *kind,
         int ord,
         const char *name,
         Oid typid,
         Oid source_table,
         const char *source_column,
         int base_not_null,
         int result_not_null)
{
    Datum values[PD_NCOLS];
    bool  nulls[PD_NCOLS];

    memset(nulls, true, sizeof(nulls));

    values[PD_COL_KIND] = PointerGetDatum(cstring_to_text(kind));
    nulls[PD_COL_KIND] = false;

    values[PD_COL_ORD] = Int32GetDatum(ord);
    nulls[PD_COL_ORD] = false;

    if (name != NULL)
    {
        values[PD_COL_NAME] = PointerGetDatum(cstring_to_text(name));
        nulls[PD_COL_NAME] = false;
    }

    if (OidIsValid(typid))
    {
        values[PD_COL_TYPE_OID] = ObjectIdGetDatum(typid);
        nulls[PD_COL_TYPE_OID] = false;

        /*
         * format_type_be prints the SQL spelling ("integer", "character
         * varying(10)") rather than the catalog spelling ("int4", "varchar"),
         * and schema-qualifies only when the type is not on the search_path.
         * That makes the output something a generator can map, not just
         * something a human can read.
         */
        values[PD_COL_TYPE_NAME] =
            PointerGetDatum(cstring_to_text(format_type_be(typid)));
        nulls[PD_COL_TYPE_NAME] = false;
    }

    if (OidIsValid(source_table))
    {
        values[PD_COL_SOURCE_TABLE] = ObjectIdGetDatum(source_table);
        nulls[PD_COL_SOURCE_TABLE] = false;
    }

    if (source_column != NULL)
    {
        values[PD_COL_SOURCE_COLUMN] = PointerGetDatum(cstring_to_text(source_column));
        nulls[PD_COL_SOURCE_COLUMN] = false;
    }

    if (base_not_null != PD_UNKNOWN)
    {
        values[PD_COL_BASE_NOT_NULL] = BoolGetDatum(base_not_null != 0);
        nulls[PD_COL_BASE_NOT_NULL] = false;
    }

    if (result_not_null != PD_UNKNOWN)
    {
        values[PD_COL_RESULT_NOT_NULL] = BoolGetDatum(result_not_null != 0);
        nulls[PD_COL_RESULT_NOT_NULL] = false;
    }

    tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, values, nulls);
}

/* ------------------------------------------------------------------------
 * Outer-join nullability
 *
 * This is what pg_describe has that tools reading attnotnull alone do not.
 * --------------------------------------------------------------------- */

/*
 * Every base-relation range table index at or below a jointree node.
 *
 * A JoinExpr also has an rtindex of its own -- the RTE for the join's alias --
 * but that is not a base relation and no target-list Var we report provenance
 * for refers to it, so it is deliberately not collected.
 */
static void
collect_rtindexes(Node *jtnode, Bitmapset **result)
{
    if (jtnode == NULL)
        return;

    if (IsA(jtnode, RangeTblRef))
    {
        *result = bms_add_member(*result, ((RangeTblRef *) jtnode)->rtindex);
    }
    else if (IsA(jtnode, FromExpr))
    {
        ListCell *lc;

        foreach(lc, ((FromExpr *) jtnode)->fromlist)
            collect_rtindexes((Node *) lfirst(lc), result);
    }
    else if (IsA(jtnode, JoinExpr))
    {
        JoinExpr *j = (JoinExpr *) jtnode;

        collect_rtindexes(j->larg, result);
        collect_rtindexes(j->rarg, result);
    }
}

/*
 * Range table indexes whose columns can be NULL in the result because they sit
 * on the nullable side of an outer join.
 *
 * The recursion is what makes nesting work, and nesting is where naive
 * implementations break. Two cases to hold in mind:
 *
 *   a LEFT JOIN (b JOIN c)   -- b AND c are both nullable. The inner join
 *                               between them is irrelevant: if the outer join
 *                               finds no match, the whole right subtree is
 *                               null-extended at once.
 *
 *   (a LEFT JOIN b) JOIN c   -- only b is nullable. c is joined afterwards and
 *                               inner-joined at that.
 *
 * Recursing into both arms BEFORE applying this node's own rule is what gets
 * the first case right: collect_rtindexes() sweeps the entire nullable subtree,
 * however deep, while the recursive calls catch outer joins nested inside it.
 */
static void
find_nullable(Node *jtnode, Bitmapset **nullable)
{
    if (jtnode == NULL)
        return;

    if (IsA(jtnode, FromExpr))
    {
        ListCell *lc;

        foreach(lc, ((FromExpr *) jtnode)->fromlist)
            find_nullable((Node *) lfirst(lc), nullable);
    }
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

/* ------------------------------------------------------------------------
 * Error position relocation
 * --------------------------------------------------------------------- */

/*
 * Without this, an error in the described query reports a cursor position
 * measured against the inner text but drawn against the OUTER statement:
 *
 *     ERROR:  syntax error at or near "WHERE"
 *     LINE 1: SELECT * FROM pg_describe('SELECT FROM WHERE');
 *                         ^
 *
 * The caret lands on whatever happens to sit at that offset of the call. SPI
 * has the same problem and solves it the same way: clear the ordinary position
 * and re-report it as an INTERNAL position against the inner query, which
 * clients render as a separate QUERY: line.
 */
static void
describe_error_cb(void *arg)
{
    const char *sql = (const char *) arg;
    int         pos = geterrposition();

    if (pos > 0)
    {
        errposition(0);
        internalerrposition(pos);
        internalerrquery(sql);
    }
}

/* ------------------------------------------------------------------------
 * Permissions
 * --------------------------------------------------------------------- */

/*
 * Do the check the executor would have done.
 *
 * Parse analysis resolves names and types but does not check whether the
 * calling role may read what it resolved -- it only records the requirement, in
 * Query->rteperminfos. ExecutorStart enforces it via ExecCheckPermissions, and
 * we never get there.
 *
 * Skipping this would not leak data, it would leak the SCHEMA: the column
 * names, types and structure of every table in the database, to any role that
 * can call the function. That is usually the first thing an attacker wants.
 *
 * The order below mirrors ExecCheckOneRelPerms: relation-level rights first,
 * then per-column rights where those are missing, because GRANT SELECT (col) is
 * a real and common way to hold partial access.
 */
static void
check_permissions(Query *query)
{
    ListCell *lc;

    foreach(lc, query->rteperminfos)
    {
        RTEPermissionInfo *perminfo = lfirst_node(RTEPermissionInfo, lc);
        Oid      relid = perminfo->relid;
        Oid      userid;
        AclMode  remaining;

        if (perminfo->requiredPerms == 0)
            continue;

        /*
         * checkAsUser is set when the rights to check are not the current
         * role's -- a view executes with its owner's privileges, and this is
         * where that indirection is recorded. Ignoring it would deny access the
         * executor would have allowed.
         */
        userid = OidIsValid(perminfo->checkAsUser) ? perminfo->checkAsUser : GetUserId();

        /*
         * aclmask rather than aclcheck: we need to know WHICH required bits are
         * missing, because only a missing SELECT can be rescued by column-level
         * grants below.
         */
        remaining = perminfo->requiredPerms &
            ~pg_class_aclmask(relid, userid, perminfo->requiredPerms, ACLMASK_ALL);

        if (remaining == 0)
            continue;

        if (remaining == ACL_SELECT)
        {
            bool all_cols_ok = true;
            int  col = -1;

            /*
             * selectedCols members are attribute numbers OFFSET by
             * FirstLowInvalidHeapAttributeNumber (-7), so that system columns,
             * whose attnums are negative, fit in a Bitmapset. Forgetting the
             * offset checks the wrong column -- silently, and in the permissive
             * direction.
             */
            while ((col = bms_next_member(perminfo->selectedCols, col)) >= 0)
            {
                AttrNumber attno = col + FirstLowInvalidHeapAttributeNumber;

                /* Attribute 0 is the whole row; no column grant satisfies it. */
                if (attno == InvalidAttrNumber)
                {
                    all_cols_ok = false;
                    break;
                }

                if (pg_attribute_aclcheck(relid, attno, userid, ACL_SELECT) != ACLCHECK_OK)
                {
                    all_cols_ok = false;
                    break;
                }
            }

            if (all_cols_ok)
                continue;
        }

        /*
         * aclcheck_error, not a hand-rolled ereport: the message, the SQLSTATE
         * and the object naming then match what the query itself would have
         * produced. A caller should not be able to tell this check apart from
         * the executor's.
         */
        aclcheck_error(ACLCHECK_NO_PRIV,
                       get_relkind_objtype(get_rel_relkind(relid)),
                       get_rel_name(relid));
    }
}

/* ------------------------------------------------------------------------
 * Columns
 * --------------------------------------------------------------------- */

/*
 * `tlist` is the analysed target list: targetList for a SELECT, returningList
 * for INSERT/UPDATE/DELETE. Both are lists of TargetEntry and read identically.
 */
static void
describe_columns(ReturnSetInfo *rsinfo, Query *query, List *tlist)
{
    ListCell  *lc;
    Bitmapset *nullable = NULL;
    bool       grouping_sets;
    int        ord = 0;

    find_nullable((Node *) query->jointree, &nullable);

    /*
     * GROUP BY ROLLUP / CUBE / GROUPING SETS emits super-aggregate rows in
     * which the grouping columns are NULL, no matter how NOT NULL the
     * underlying column is. It is the same class of false positive as the outer
     * join, from a different direction, and tools that read attnotnull alone
     * miss it too.
     *
     * The treatment here is deliberately blunt -- if the query has grouping
     * sets, no provenance-bearing column is reported as guaranteed non-null.
     * Being too cautious costs a consumer one impossible null check; being too
     * confident costs them a production crash.
     */
    grouping_sets = (query->groupingSets != NIL);

    foreach(lc, tlist)
    {
        TargetEntry *tle = lfirst_node(TargetEntry, lc);
        Oid          source_table = InvalidOid;
        char        *source_column = NULL;
        int          base_not_null = PD_UNKNOWN;
        int          result_not_null = PD_UNKNOWN;

        /*
         * resjunk entries are in the list but not in the result: the sort key
         * ORDER BY adds for a column you did not select, the ctid a FOR UPDATE
         * needs. RowDescription does not report them and neither do we.
         */
        if (tle->resjunk)
            continue;

        ord++;

        /*
         * Provenance is meaningful only when the output column IS a column: a
         * bare Var into a real relation. upper(email), count(*) and literals
         * have a type but no source, and RowDescription agrees -- it reports
         * tableOID 0 for exactly these.
         */
        if (IsA(tle->expr, Var))
        {
            Var *var = (Var *) tle->expr;

            /*
             * varlevelsup > 0 refers to an ENCLOSING query's range table, so
             * rt_fetch against this one would silently read the wrong entry and
             * report provenance pointing at the wrong table. varattno <= 0 is a
             * whole-row reference or a system column, neither of which has the
             * pg_attribute row we are about to read.
             */
            if (var->varlevelsup == 0 && var->varattno > 0)
            {
                RangeTblEntry *rte = rt_fetch(var->varno, query->rtable);

                if (rte->rtekind == RTE_RELATION)
                {
                    HeapTuple atup = SearchSysCache2(ATTNUM,
                                                     ObjectIdGetDatum(rte->relid),
                                                     Int16GetDatum(var->varattno));

                    /*
                     * The syscache, not a query against pg_attribute. A client
                     * doing this from outside pays a round trip; in here it is
                     * a hash lookup in memory the backend already holds, over a
                     * relation the analysis above has already locked.
                     */
                    if (HeapTupleIsValid(atup))
                    {
                        Form_pg_attribute att = (Form_pg_attribute) GETSTRUCT(atup);

                        source_table = rte->relid;
                        source_column = pstrdup(NameStr(att->attname));
                        base_not_null = att->attnotnull ? 1 : 0;

                        /*
                         * The distinction this extension exists for. A column
                         * is non-null in the RESULT only if it is non-null at
                         * the source AND nothing downstream can null-extend it.
                         */
                        if (base_not_null == 0)
                            result_not_null = 0;
                        else if (bms_is_member(var->varno, nullable) || grouping_sets)
                            result_not_null = 0;
                        else
                            result_not_null = 1;

                        ReleaseSysCache(atup);
                    }
                }
            }
        }

        emit_row(rsinfo, "column", ord, tle->resname,
                 exprType((Node *) tle->expr),
                 source_table, source_column, base_not_null, result_not_null);
    }
}

/* ------------------------------------------------------------------------
 * Entry point
 * --------------------------------------------------------------------- */

PG_FUNCTION_INFO_V1(pg_describe);

Datum
pg_describe(PG_FUNCTION_ARGS)
{
    ReturnSetInfo       *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
    char                *sql = text_to_cstring(PG_GETARG_TEXT_PP(0));
    ErrorContextCallback errcallback;
    List                *raw;
    RawStmt             *rawstmt;
    Query               *query;
    Oid                 *param_types = NULL;
    int                  num_params = 0;
    int                  i;

    /*
     * Materialize mode: a describe result is a handful of rows, so building the
     * whole set costs nothing and the code is far clearer than the
     * value-per-call protocol would be.
     */
    InitMaterializedSRF(fcinfo, 0);

    /* Report parse errors against the inner query, not against the call. */
    errcallback.callback = describe_error_cb;
    errcallback.arg = (void *) sql;
    errcallback.previous = error_context_stack;
    error_context_stack = &errcallback;

    /*
     * Stage one: grammar. Pure function of the text -- no catalog access, no
     * name resolution. It will happily parse a SELECT against a table that does
     * not exist. Errors here are syntax errors and nothing else.
     */
    raw = pg_parse_query(sql);

    /*
     * A Parse message carries exactly one statement, and so do we. Accepting
     * several would mean picking one to describe, and there is no defensible
     * pick.
     */
    if (list_length(raw) != 1)
        ereport(ERROR,
                (errcode(ERRCODE_SYNTAX_ERROR),
                 errmsg("pg_describe expects exactly one statement, got %d",
                        list_length(raw))));

    rawstmt = linitial_node(RawStmt, raw);

    /*
     * Stage two, and the point of the extension.
     *
     * "varparams" is the difference from the fixedparams sibling: we hand it an
     * empty parameter array and it grows one, inferring each $n's type from the
     * context the parameter appears in. `WHERE id = $1` against an integer
     * column makes $1 an integer. That is precisely what a client reads out of
     * a ParameterDescription message.
     *
     * Note the invisible side effect: this resolves every name against the
     * catalog and opens every referenced relation with AccessShareLock, held
     * until the transaction ends. Describing is cheap, but neither free nor
     * side-effect-free.
     */
    query = parse_analyze_varparams(rawstmt, sql, &param_types, &num_params, NULL);

    /*
     * Past the last thing that can raise a positioned parse error. On the error
     * path this pop never runs and does not need to -- ereport unwinds the
     * callback stack along with everything else.
     */
    error_context_stack = errcallback.previous;

    /* Earn the right to talk about those relations before talking about them. */
    check_permissions(query);

    /* Parameters, in $1..$n order: a ParameterDescription, assembled locally. */
    for (i = 0; i < num_params; i++)
        emit_row(rsinfo, "param", i + 1, NULL, param_types[i],
                 InvalidOid, NULL, PD_UNKNOWN, PD_UNKNOWN);

    /*
     * Where the result columns live depends on the statement:
     *
     *   SELECT                    -> targetList
     *   INSERT/UPDATE/DELETE      -> returningList, empty without RETURNING
     *   utility (CREATE TABLE...) -> neither. Utility statements are parked
     *                                un-analysed in utilityStmt; they have no
     *                                result columns, and reporting none is a
     *                                better answer than an error.
     */
    if (query->commandType == CMD_SELECT)
        describe_columns(rsinfo, query, query->targetList);
    else if (query->commandType != CMD_UTILITY)
        describe_columns(rsinfo, query, query->returningList);

    /* Materialize mode returns no Datum; the rows are already in the store. */
    PG_RETURN_NULL();
}

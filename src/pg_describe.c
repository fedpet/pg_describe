/*
 * pg_describe -- report what a query would return, without running it.
 *
 *     pg_parse_query()           text    -> RawStmt   (grammar only)
 *     parse_analyze_varparams()  RawStmt -> Query     (names, types, params)
 *     rewrite / plan / execute                        (never called)
 *
 * parse_analyze_varparams() is what exec_parse_message() calls to serve a Parse
 * message with no declared parameter types, so $1 comes back typed rather than
 * rejected: the analyser infers it from the context the parameter appears in.
 *
 * Three functions here are load-bearing:
 *
 *   check_permissions()  Parse analysis does not check privileges; it records
 *                        them for the executor, which is never reached. Without
 *                        this the function exposes every table's structure.
 *   find_nullable()      attnotnull describes the source column, not the result
 *                        column. Outer joins separate the two.
 *   describe_error_cb()  relocates a parse error's cursor position onto the
 *                        inner query.
 */
#include "postgres.h"
#include "fmgr.h"

#include "funcapi.h"

#include "access/htup_details.h"
#include "access/sysattr.h"
#include "catalog/objectaddress.h"
#include "catalog/pg_attribute.h"
#include "catalog/pg_namespace.h"
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
 * Nullability flags are tri-state: true, false, unknown. An expression column
 * has no attnotnull to report, so unknown is a distinct answer from false.
 * Consumers treat unknown as nullable.
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
 * --------------------------------------------------------------------- */

/*
 * Every base-relation range table index at or below a jointree node. A
 * JoinExpr's own rtindex (the RTE for its alias) is not a base relation and no
 * Var we report provenance for refers to it, so it is skipped.
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
 * Range table indexes that sit on the nullable side of an outer join.
 *
 *   a LEFT JOIN (b JOIN c)   b and c both. No match null-extends the whole
 *                            right subtree, so the inner join is irrelevant.
 *   (a LEFT JOIN b) JOIN c   b only.
 *
 * Recursing into both arms before applying this node's rule is what gets the
 * first case right: collect_rtindexes() sweeps the entire nullable subtree
 * while the recursive calls catch outer joins nested inside it.
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
 * A parse error's position is measured against the inner query but drawn
 * against the outer statement, so the caret lands on nothing. Clear the
 * ordinary position and re-report it as internal, against the inner query;
 * clients render that as a separate QUERY: line. Same approach as SPI.
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
 * The check ExecutorStart would have done via ExecCheckPermissions. Parse
 * analysis only records the requirement in Query->rteperminfos; without this,
 * any caller could read every table's structure.
 *
 * Order mirrors ExecCheckOneRelPerms: relation-level rights first, then
 * per-column rights where those are missing, since GRANT SELECT (col) is a
 * common way to hold partial access.
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
             * selectedCols members are offset by
             * FirstLowInvalidHeapAttributeNumber (-7) so that system columns,
             * whose attnums are negative, fit in a Bitmapset. Missing the
             * offset checks the wrong column, permissively.
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
         * aclcheck_error rather than a hand-rolled ereport, so message,
         * SQLSTATE and object naming match what the query itself would raise.
         */
        aclcheck_error(ACLCHECK_NO_PRIV,
                       get_relkind_objtype(get_rel_relkind(relid)),
                       get_rel_name(relid));
    }
}

/* ------------------------------------------------------------------------
 * Nullability of an expression
 * --------------------------------------------------------------------- */

#if PG_VERSION_NUM >= 180000

/*
 * The expression a Var into the grouping step stands for, or NULL if the Var
 * does not point at one.
 *
 * From v18 the parser interposes an RTE_GROUP between the target list and the
 * range table whenever a query groups. A grouped column in the target list is
 * then a Var into that RTE rather than into the relation it came from, and
 * rte->groupexprs holds the expression each one stands for. Everything that
 * wants to know what a grouped column really is goes through here, so that
 * provenance and nullability cannot disagree about it.
 */
static Node *
grouping_step_expr(Query *query, Var *var)
{
    RangeTblEntry *rte;

    if (var->varlevelsup != 0 || var->varattno <= 0)
        return NULL;

    rte = rt_fetch(var->varno, query->rtable);

    if (rte->rtekind != RTE_GROUP ||
        var->varattno > list_length(rte->groupexprs))
        return NULL;

    return (Node *) list_nth(rte->groupexprs, var->varattno - 1);
}
#endif

/*
 * Resolve a target-list Var to the relation column it came from.
 *
 * Returns false when there is none to name: a correlated reference, a whole-row
 * or system column, or a Var into anything that is not a plain relation.
 */
static bool
resolve_var_column(Query *query, Var *var, Oid *relid, AttrNumber *attno,
                   Index *varno)
{
    RangeTblEntry *rte;

#if PG_VERSION_NUM >= 180000

    /*
     * Resolve through the grouping step, which is what lets a grouped column
     * keep the provenance and the attnotnull it reported before v18.
     *
     * A loop rather than one step: nothing promises the expression behind a
     * grouping entry is not itself a Var needing the same treatment. Anything
     * that is not a Var ends the walk without provenance -- the right answer
     * for GROUP BY upper(email), whose output has no source column to name.
     * Its nullability is a separate question, and expr_is_not_null answers it
     * by recursing into that same expression.
     */
    for (;;)
    {
        Node *gexpr = grouping_step_expr(query, var);

        if (gexpr == NULL)
            break;
        if (!IsA(gexpr, Var))
            return false;

        var = (Var *) gexpr;
    }
#endif

    /*
     * varlevelsup > 0 refers to an enclosing query's range table, so rt_fetch
     * here would read the wrong entry. varattno <= 0 is a whole-row reference
     * or a system column, with no pg_attribute row to consult.
     */
    if (var->varlevelsup != 0 || var->varattno <= 0)
        return false;

    rte = rt_fetch(var->varno, query->rtable);

    if (rte->rtekind != RTE_RELATION)
        return false;

    *relid = rte->relid;
    *attno = var->varattno;
    *varno = var->varno;
    return true;
}

/* Does this column's relation declare it NOT NULL? */
static bool
column_is_not_null(Oid relid, AttrNumber attno)
{
    HeapTuple atup;
    bool      notnull;

    /*
     * Syscache rather than a query against pg_attribute: a hash lookup over a
     * relation analysis has already locked.
     */
    atup = SearchSysCache2(ATTNUM, ObjectIdGetDatum(relid), Int16GetDatum(attno));
    if (!HeapTupleIsValid(atup))
        return false;

    notnull = ((Form_pg_attribute) GETSTRUCT(atup))->attnotnull;
    ReleaseSysCache(atup);
    return notnull;
}

/*
 * count() is the one aggregate that never returns NULL: over no rows at all it
 * returns 0, where every other aggregate returns NULL. Matched by name in
 * pg_catalog rather than by a hardcoded OID, so that a user-defined "count"
 * in another schema is not mistaken for it.
 */
static bool
is_count_aggregate(Oid aggfnoid)
{
    char *name = get_func_name(aggfnoid);
    bool  result;

    if (name == NULL)
        return false;

    result = (strcmp(name, "count") == 0 &&
              get_func_namespace(aggfnoid) == PG_CATALOG_NAMESPACE);
    pfree(name);
    return result;
}

static bool expr_is_not_null(Node *node, Query *query, Bitmapset *nullable,
                             bool grouping_sets);

/* Are every one of these expressions provably non-null? */
static bool
args_are_not_null(List *args, Query *query, Bitmapset *nullable,
                  bool grouping_sets)
{
    ListCell *lc;

    /*
     * Vacuous truth would be wrong here. Strictness says a function returns
     * NULL when an argument is NULL; with no arguments it says nothing at all
     * about the result, so a zero-argument function is never proven non-null
     * this way.
     */
    if (args == NIL)
        return false;

    foreach(lc, args)
    {
        if (!expr_is_not_null((Node *) lfirst(lc), query, nullable, grouping_sets))
            return false;
    }
    return true;
}

/*
 * Can this expression be proven never to evaluate to NULL?
 *
 * False means "not proven", never "can be NULL" -- so every node kind that is
 * not understood falls through to the default and is reported unknown. That is
 * the direction that matters: over-declaring a null costs one impossible check
 * in the caller, under-declaring one costs a crash.
 *
 * The reasoning is the planner's own, not a heuristic. Strictness is recorded
 * per function in pg_proc, and everything else here is a property of the node
 * type rather than a guess about it.
 */
static bool
expr_is_not_null(Node *node, Query *query, Bitmapset *nullable,
                 bool grouping_sets)
{
    if (node == NULL)
        return false;

    /* Expressions nest arbitrarily deeply; this is the standard guard. */
    check_stack_depth();

    switch (nodeTag(node))
    {
        case T_Const:
            return !((Const *) node)->constisnull;

        case T_Var:
        {
            Oid         relid;
            AttrNumber  attno;
            Index       varno;

#if PG_VERSION_NUM >= 180000

            /*
             * A Var into the grouping step stands for the expression grouped
             * by, so that expression is what has to be proven. Without this,
             * GROUP BY date_trunc('day', ts) over a NOT NULL column answers
             * differently on v18 than on v17, where the target list holds the
             * expression directly and this case is never reached.
             *
             * Not under grouping sets: a super-aggregate row nulls the grouped
             * column however non-null the expression behind it, which is the
             * same reason the plain column below is not proven there either.
             */
            if (!grouping_sets)
            {
                Node *gexpr = grouping_step_expr(query, (Var *) node);

                if (gexpr != NULL)
                    return expr_is_not_null(gexpr, query, nullable,
                                            grouping_sets);
            }
#endif

            if (!resolve_var_column(query, (Var *) node, &relid, &attno, &varno))
                return false;

            /*
             * Non-null in the result only if non-null at the source and nothing
             * downstream can null-extend it.
             */
            return column_is_not_null(relid, attno) &&
                   !bms_is_member(varno, nullable) &&
                   !grouping_sets;
        }

        /* NULL only when every argument is NULL, so one non-null suffices. */
        case T_CoalesceExpr:
        {
            ListCell *lc;

            foreach(lc, ((CoalesceExpr *) node)->args)
            {
                if (expr_is_not_null((Node *) lfirst(lc), query, nullable,
                                     grouping_sets))
                    return true;
            }
            return false;
        }

        /*
         * Every arm must be non-null, the ELSE included. A CASE with no ELSE
         * yields NULL when nothing matches, so its absence settles it.
         */
        case T_CaseExpr:
        {
            CaseExpr *caseexpr = (CaseExpr *) node;
            ListCell *lc;

            if (!expr_is_not_null((Node *) caseexpr->defresult, query, nullable,
                                  grouping_sets))
                return false;

            foreach(lc, caseexpr->args)
            {
                CaseWhen *when = lfirst_node(CaseWhen, lc);

                if (!expr_is_not_null((Node *) when->result, query, nullable,
                                      grouping_sets))
                    return false;
            }
            return true;
        }

        /* A strict function returns NULL only when an argument is NULL. */
        case T_FuncExpr:
        {
            FuncExpr *func = (FuncExpr *) node;

            return func_strict(func->funcid) &&
                   args_are_not_null(func->args, query, nullable, grouping_sets);
        }

        case T_OpExpr:
        {
            OpExpr *op = (OpExpr *) node;

            return func_strict(op->opfuncid) &&
                   args_are_not_null(op->args, query, nullable, grouping_sets);
        }

        /* IS DISTINCT FROM is exactly the null-aware comparison: never NULL. */
        case T_DistinctExpr:
            return true;

        /*
         * NULLIF shares OpExpr's shape but is the opposite case: it returns
         * NULL precisely when its arguments are equal, however non-null they
         * are.
         */
        case T_NullIfExpr:
            return false;

        /*
         * AND, OR and NOT are three-valued, but only NULL input produces NULL
         * output, so non-null arguments give a non-null result.
         */
        case T_BoolExpr:
            return args_are_not_null(((BoolExpr *) node)->args, query, nullable,
                                     grouping_sets);

        /* Tests answer true or false about a value, including about NULL. */
        case T_NullTest:
        case T_BooleanTest:
            return true;

        /* Constructing an array or a row yields a value, never NULL. */
        case T_ArrayExpr:
        case T_RowExpr:
            return true;

        /* Wrappers that change the label or representation, not the value. */
        case T_RelabelType:
            return expr_is_not_null((Node *) ((RelabelType *) node)->arg, query,
                                    nullable, grouping_sets);
        case T_CoerceViaIO:
            return expr_is_not_null((Node *) ((CoerceViaIO *) node)->arg, query,
                                    nullable, grouping_sets);
        case T_ArrayCoerceExpr:
            return expr_is_not_null((Node *) ((ArrayCoerceExpr *) node)->arg,
                                    query, nullable, grouping_sets);
        case T_CollateExpr:
            return expr_is_not_null((Node *) ((CollateExpr *) node)->arg, query,
                                    nullable, grouping_sets);
        case T_CoerceToDomain:
            return expr_is_not_null((Node *) ((CoerceToDomain *) node)->arg,
                                    query, nullable, grouping_sets);

        /*
         * Only count(). Every other aggregate returns NULL over no rows, and
         * over a group whose values are all NULL. Grouping sets do not change
         * this: they null-extend grouping columns, not aggregates.
         */
        case T_Aggref:
            return is_count_aggregate(((Aggref *) node)->aggfnoid);

        case T_WindowFunc:
            return is_count_aggregate(((WindowFunc *) node)->winfnoid);

        default:
            return false;
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
     * which grouping columns are NULL regardless of attnotnull. Treated
     * bluntly: with grouping sets, nothing is reported guaranteed non-null.
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
         * resjunk entries are in the list but not the result: an ORDER BY sort
         * key for an unselected column, the ctid FOR UPDATE needs.
         */
        if (tle->resjunk)
            continue;

        ord++;

        /*
         * Provenance applies only to a bare Var into a real relation.
         * upper(email), count(*) and literals have a type but no source;
         * RowDescription reports tableOID 0 for these too.
         */
        if (IsA(tle->expr, Var))
        {
            Oid        relid;
            AttrNumber attno;
            Index      varno;

            if (resolve_var_column(query, (Var *) tle->expr, &relid, &attno,
                                   &varno))
            {
                source_table = relid;
                source_column = get_attname(relid, attno, true);
                base_not_null = column_is_not_null(relid, attno) ? 1 : 0;
            }
        }

        /*
         * Nullability is asked of every expression, not just of columns. A
         * column-backed one that is not proven non-null is definitely nullable
         * -- the source and every join above it are known. Anything else is
         * only reported when proven, because not proven is not the same as
         * nullable, and unknown is the answer that fails safe.
         */
        if (expr_is_not_null((Node *) tle->expr, query, nullable, grouping_sets))
            result_not_null = 1;
        else if (source_table != InvalidOid)
            result_not_null = 0;

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

    /* Materialize mode: a describe result is a handful of rows. */
    InitMaterializedSRF(fcinfo, 0);

    /* Report parse errors against the inner query, not against the call. */
    errcallback.callback = describe_error_cb;
    errcallback.arg = (void *) sql;
    errcallback.previous = error_context_stack;
    error_context_stack = &errcallback;

    /*
     * Grammar only: no catalog access, no name resolution. Parses a SELECT
     * against a nonexistent table without complaint.
     */
    raw = pg_parse_query(sql);

    /* A Parse message carries exactly one statement, and so do we. */
    if (list_length(raw) != 1)
        ereport(ERROR,
                (errcode(ERRCODE_SYNTAX_ERROR),
                 errmsg("pg_describe expects exactly one statement, got %d",
                        list_length(raw))));

    rawstmt = linitial_node(RawStmt, raw);

    /*
     * varparams, not fixedparams: we pass an empty parameter array and it grows
     * one, inferring each $n from context. `WHERE id = $1` against an integer
     * column makes $1 an integer.
     *
     * Side effect worth knowing: this opens every referenced relation with
     * AccessShareLock, held until the transaction ends.
     */
    query = parse_analyze_varparams(rawstmt, sql, &param_types, &num_params, NULL);

    /*
     * Past the last positioned parse error. On the error path this pop never
     * runs and need not: ereport unwinds the callback stack.
     */
    error_context_stack = errcallback.previous;

    /* Check privileges before reporting anything about those relations. */
    check_permissions(query);

    /* Parameters, in $1..$n order. */
    for (i = 0; i < num_params; i++)
        emit_row(rsinfo, "param", i + 1, NULL, param_types[i],
                 InvalidOid, NULL, PD_UNKNOWN, PD_UNKNOWN);

    /*
     *   SELECT               -> targetList
     *   INSERT/UPDATE/DELETE -> returningList, empty without RETURNING
     *   utility              -> neither; parked un-analysed in utilityStmt, so
     *                           no result columns rather than an error.
     */
    if (query->commandType == CMD_SELECT)
        describe_columns(rsinfo, query, query->targetList);
    else if (query->commandType != CMD_UTILITY)
        describe_columns(rsinfo, query, query->returningList);

    /* Materialize mode returns no Datum; the rows are already in the store. */
    PG_RETURN_NULL();
}

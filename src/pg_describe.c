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
#include "catalog/pg_type_d.h"
#include "miscadmin.h"
#include "nodes/bitmapset.h"
#include "nodes/nodeFuncs.h"
#include "nodes/parsenodes.h"
#include "parser/analyze.h"
#include "parser/parsetree.h"
#include "tcop/tcopprot.h"
#include "utils/acl.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/json.h"
#include "utils/jsonb.h"
#include "utils/lsyscache.h"
#include "utils/syscache.h"
#include "utils/typcache.h"

PG_MODULE_MAGIC;

/* The ten columns declared by RETURNS TABLE, in order. */
#define PD_COL_KIND             0
#define PD_COL_ORD              1
#define PD_COL_NAME             2
#define PD_COL_TYPE_OID         3
#define PD_COL_TYPE_NAME        4
#define PD_COL_SOURCE_TABLE     5
#define PD_COL_SOURCE_COLUMN    6
#define PD_COL_BASE_NOT_NULL    7
#define PD_COL_RESULT_NOT_NULL  8
#define PD_COL_RESULT_SHAPE     9
#define PD_NCOLS                10

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
         int result_not_null,
         Jsonb *result_shape)
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

    if (result_shape != NULL)
    {
        values[PD_COL_RESULT_SHAPE] = JsonbPGetDatum(result_shape);
        nulls[PD_COL_RESULT_SHAPE] = false;
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
 * Filter-aware JSON shape analysis
 * --------------------------------------------------------------------- */

typedef enum JsonShapeKind
{
    PD_SHAPE_UNKNOWN,
    PD_SHAPE_SCALAR,
    PD_SHAPE_ARRAY,
    PD_SHAPE_OBJECT
} JsonShapeKind;

typedef struct JsonShape JsonShape;

typedef struct JsonShapeField
{
    char      *name;
    bool       optional;
    JsonShape *shape;
} JsonShapeField;

struct JsonShape
{
    JsonShapeKind kind;
    int           result_not_null;
    bool          neutral;

    /* scalar */
    char         *runtime_kind;
    Oid           type_oid;
    Oid           source_table;
    char         *source_column;
    int           base_not_null;

    /* array */
    JsonShape    *element;

    /* object */
    List         *fields;
    JsonShape    *additional;
};

static JsonShape *describe_json_value(Node *node, Query *query,
                                      Bitmapset *nullable,
                                      bool grouping_sets);

static JsonShape *
new_shape(JsonShapeKind kind)
{
    JsonShape *shape = palloc0(sizeof(JsonShape));

    shape->kind = kind;
    shape->result_not_null = PD_UNKNOWN;
    shape->base_not_null = PD_UNKNOWN;
    shape->type_oid = InvalidOid;
    shape->source_table = InvalidOid;
    return shape;
}

static JsonShape *
unknown_shape(void)
{
    return new_shape(PD_SHAPE_UNKNOWN);
}

/* PostgreSQL-owned functions only. A same-named function in public is data. */
static bool
pg_function_named(Oid funcid, const char *name)
{
    char *actual;
    bool  result;

    if (get_func_namespace(funcid) != PG_CATALOG_NAMESPACE)
        return false;

    actual = get_func_name(funcid);
    if (actual == NULL)
        return false;
    result = strcmp(actual, name) == 0;
    pfree(actual);
    return result;
}

static bool
pg_function_in(Oid funcid, const char *const *names, int count)
{
    int i;

    for (i = 0; i < count; i++)
        if (pg_function_named(funcid, names[i]))
            return true;
    return false;
}

/*
 * Facts which must hold for a boolean expression to be true. They are relation
 * presence facts here: exact column non-nullness is still represented by the
 * expression itself, while presence is what removes outer-join extension.
 */
static void
nonnull_expr_relations(Node *node, Bitmapset **facts)
{
    if (node == NULL)
        return;

    switch (nodeTag(node))
    {
        case T_Var:
        {
            Var *var = (Var *) node;

            if (var->varlevelsup == 0 && var->varattno > 0)
                *facts = bms_add_member(*facts, var->varno);
            break;
        }
        case T_RelabelType:
            nonnull_expr_relations((Node *) ((RelabelType *) node)->arg, facts);
            break;
        case T_CollateExpr:
            nonnull_expr_relations((Node *) ((CollateExpr *) node)->arg, facts);
            break;
        case T_CoerceViaIO:
            nonnull_expr_relations((Node *) ((CoerceViaIO *) node)->arg, facts);
            break;
        case T_CoerceToDomain:
            nonnull_expr_relations((Node *) ((CoerceToDomain *) node)->arg, facts);
            break;
        case T_FuncExpr:
        {
            FuncExpr *func = (FuncExpr *) node;
            ListCell *lc;

            if (!func_strict(func->funcid))
                break;
            foreach(lc, func->args)
                nonnull_expr_relations((Node *) lfirst(lc), facts);
            break;
        }
        case T_OpExpr:
        {
            OpExpr  *op = (OpExpr *) node;
            ListCell *lc;

            if (!func_strict(op->opfuncid))
                break;
            foreach(lc, op->args)
                nonnull_expr_relations((Node *) lfirst(lc), facts);
            break;
        }
        default:
            break;
    }
}

static void
true_expr_relations(Node *node, Bitmapset **facts)
{
    if (node == NULL)
        return;

    if (IsA(node, NullTest))
    {
        NullTest *test = (NullTest *) node;

        if (test->nulltesttype == IS_NOT_NULL)
            nonnull_expr_relations((Node *) test->arg, facts);
        return;
    }

    if (IsA(node, BoolExpr))
    {
        BoolExpr *boolean = (BoolExpr *) node;
        ListCell *lc;

        if (boolean->boolop == AND_EXPR)
        {
            foreach(lc, boolean->args)
                true_expr_relations((Node *) lfirst(lc), facts);
        }
        else if (boolean->boolop == OR_EXPR)
        {
            Bitmapset *common = NULL;
            bool       first = true;

            foreach(lc, boolean->args)
            {
                Bitmapset *branch = NULL;

                true_expr_relations((Node *) lfirst(lc), &branch);
                if (first)
                {
                    common = branch;
                    first = false;
                }
                else
                    common = bms_intersect(common, branch);
            }
            *facts = bms_union(*facts, common);
        }
        return;
    }

    /* A true strict boolean result proves every argument non-null. */
    nonnull_expr_relations(node, facts);
}

static bool
arm_is_present(Node *arm, Bitmapset *facts)
{
    Bitmapset *members = NULL;
    bool       result;

    collect_rtindexes(arm, &members);
    result = bms_overlap(members, facts);
    bms_free(members);
    return result;
}

static bool
refine_matched_joins(Node *node, Bitmapset **facts, List **matched)
{
    bool changed = false;

    if (node == NULL)
        return false;
    if (IsA(node, FromExpr))
    {
        ListCell *lc;

        foreach(lc, ((FromExpr *) node)->fromlist)
            changed |= refine_matched_joins((Node *) lfirst(lc), facts, matched);
    }
    else if (IsA(node, JoinExpr))
    {
        JoinExpr *join = (JoinExpr *) node;
        bool      left_present;
        bool      right_present;
        bool      is_matched;

        changed |= refine_matched_joins(join->larg, facts, matched);
        changed |= refine_matched_joins(join->rarg, facts, matched);

        left_present = arm_is_present(join->larg, *facts);
        right_present = arm_is_present(join->rarg, *facts);
        is_matched = join->jointype == JOIN_LEFT ? right_present :
                     join->jointype == JOIN_RIGHT ? left_present :
                     join->jointype == JOIN_FULL ? left_present && right_present :
                     false;

        if (is_matched && !list_member_ptr(*matched, join))
        {
            *matched = lappend(*matched, join);
            true_expr_relations((Node *) join->quals, facts);
            changed = true;
        }
    }
    return changed;
}

static void
find_nullable_except_matched(Node *node, List *matched, Bitmapset **nullable)
{
    if (node == NULL)
        return;
    if (IsA(node, FromExpr))
    {
        ListCell *lc;

        foreach(lc, ((FromExpr *) node)->fromlist)
            find_nullable_except_matched((Node *) lfirst(lc), matched, nullable);
    }
    else if (IsA(node, JoinExpr))
    {
        JoinExpr *join = (JoinExpr *) node;

        find_nullable_except_matched(join->larg, matched, nullable);
        find_nullable_except_matched(join->rarg, matched, nullable);

        if (list_member_ptr(matched, join))
            return;
        if (join->jointype == JOIN_LEFT || join->jointype == JOIN_FULL)
            collect_rtindexes(join->rarg, nullable);
        if (join->jointype == JOIN_RIGHT || join->jointype == JOIN_FULL)
            collect_rtindexes(join->larg, nullable);
    }
}

static Bitmapset *
nullable_for_aggregate(Query *query, Node *filter)
{
    Bitmapset *facts = NULL;
    Bitmapset *nullable = NULL;
    List      *matched = NIL;

    true_expr_relations(filter, &facts);
    while (refine_matched_joins((Node *) query->jointree, &facts, &matched))
        ;
    find_nullable_except_matched((Node *) query->jointree, matched, &nullable);
    return nullable;
}

static const char *
json_runtime_kind(Oid typid)
{
    typid = getBaseType(typid);

    if (typid == BOOLOID)
        return "boolean";
    switch (typid)
    {
        case INT2OID:
        case INT4OID:
        case INT8OID:
        case OIDOID:
        case FLOAT4OID:
        case FLOAT8OID:
        case NUMERICOID:
            return "number";
        default:
            return "string";
    }
}

static JsonShape *
scalar_shape(Node *node, Query *query, Bitmapset *nullable,
             bool grouping_sets)
{
    JsonShape *shape = new_shape(PD_SHAPE_SCALAR);
    Oid        typid = exprType(node);

    shape->runtime_kind = pstrdup(json_runtime_kind(typid));
    shape->type_oid = typid;
    shape->result_not_null = expr_is_not_null(node, query, nullable,
                                              grouping_sets) ? 1 : 0;

    if (IsA(node, Var))
    {
        Oid        relid;
        AttrNumber attno;
        Index      varno;

        if (resolve_var_column(query, (Var *) node, &relid, &attno, &varno))
        {
            shape->source_table = relid;
            shape->source_column = get_attname(relid, attno, true);
            shape->base_not_null = column_is_not_null(relid, attno) ? 1 : 0;
        }
    }
    return shape;
}

static JsonShape *
merge_shapes(JsonShape *left, JsonShape *right)
{
    JsonShape *merged;

    if (left == NULL)
        return right;
    if (right == NULL)
        return left;
    if (left->neutral)
        return right;
    if (right->neutral)
        return left;
    if (left->kind == PD_SHAPE_UNKNOWN || right->kind == PD_SHAPE_UNKNOWN ||
        left->kind != right->kind)
        return unknown_shape();

    merged = new_shape(left->kind);
    merged->result_not_null =
        left->result_not_null == 1 && right->result_not_null == 1 ? 1 : 0;

    if (left->kind == PD_SHAPE_SCALAR)
    {
        if (strcmp(left->runtime_kind, right->runtime_kind) != 0)
            return unknown_shape();
        merged->runtime_kind = pstrdup(left->runtime_kind);
        merged->type_oid = left->type_oid == right->type_oid ?
                           left->type_oid : InvalidOid;
        merged->source_table = left->source_table == right->source_table ?
                               left->source_table : InvalidOid;
        if (left->source_column != NULL && right->source_column != NULL &&
            strcmp(left->source_column, right->source_column) == 0)
            merged->source_column = pstrdup(left->source_column);
        merged->base_not_null = left->base_not_null == right->base_not_null ?
                                left->base_not_null : PD_UNKNOWN;
    }
    else if (left->kind == PD_SHAPE_ARRAY)
    {
        merged->element = merge_shapes(left->element, right->element);
    }
    else
    {
        ListCell *lc;
        ListCell *rc;

        if (list_length(left->fields) != list_length(right->fields))
            return unknown_shape();
        forboth(lc, left->fields, rc, right->fields)
        {
            JsonShapeField *lf = (JsonShapeField *) lfirst(lc);
            JsonShapeField *rf = (JsonShapeField *) lfirst(rc);
            JsonShapeField *field;

            if (strcmp(lf->name, rf->name) != 0)
                return unknown_shape();
            field = palloc0(sizeof(JsonShapeField));
            field->name = pstrdup(lf->name);
            field->optional = lf->optional || rf->optional;
            field->shape = merge_shapes(lf->shape, rf->shape);
            merged->fields = lappend(merged->fields, field);
        }
        merged->additional = merge_shapes(left->additional, right->additional);
    }
    return merged;
}

static Node *
unwrap_json_value(Node *node)
{
#if PG_VERSION_NUM >= 180000
    if (IsA(node, JsonValueExpr))
    {
        JsonValueExpr *value = (JsonValueExpr *) node;

        return value->raw_expr != NULL ? (Node *) value->raw_expr :
               (Node *) value->formatted_expr;
    }
#endif
    return node;
}

static char *
constant_string(Node *node)
{
    Const       *constant;
    Oid          output;
    bool         variable_length;

    node = unwrap_json_value(node);
    while (node != NULL &&
           (IsA(node, RelabelType) || IsA(node, CoerceViaIO) ||
            IsA(node, CoerceToDomain)))
    {
        if (IsA(node, RelabelType))
            node = (Node *) ((RelabelType *) node)->arg;
        else if (IsA(node, CoerceViaIO))
            node = (Node *) ((CoerceViaIO *) node)->arg;
        else
            node = (Node *) ((CoerceToDomain *) node)->arg;
    }
    if (node == NULL || !IsA(node, Const))
        return NULL;
    constant = (Const *) node;
    if (constant->constisnull)
        return NULL;
    getTypeOutputInfo(constant->consttype, &output, &variable_length);
    return OidOutputFunctionCall(output, constant->constvalue);
}

static JsonShape *
array_shape_from_element(JsonShape *element, int not_null)
{
    JsonShape *shape = new_shape(PD_SHAPE_ARRAY);

    shape->element = element == NULL ? unknown_shape() : element;
    shape->result_not_null = not_null;
    return shape;
}

static JsonShape *
sql_array_shape(Node *node, Query *query, Bitmapset *nullable,
                bool grouping_sets)
{
    Oid        element_type = get_element_type(getBaseType(exprType(node)));
    JsonShape *element;
    JsonShape *shape;

    if (!OidIsValid(element_type))
        return unknown_shape();

    element = new_shape(PD_SHAPE_SCALAR);
    element->runtime_kind = pstrdup(json_runtime_kind(element_type));
    element->type_oid = element_type;
    /* PostgreSQL arrays are exposed by Bun as T[], without element nulls. */
    element->result_not_null = 1;
    shape = array_shape_from_element(element,
        expr_is_not_null(node, query, nullable, grouping_sets) ? 1 : 0);
    return shape;
}

static JsonShape *
json_object_from_args(List *args, bool absent_on_null, Query *query,
                      Bitmapset *nullable, bool grouping_sets)
{
    JsonShape *object = new_shape(PD_SHAPE_OBJECT);
    ListCell  *lc;
    int        index = 0;
    char      *key = NULL;

    object->result_not_null = 1;
    foreach(lc, args)
    {
        Node *arg = unwrap_json_value((Node *) lfirst(lc));

        if ((index++ % 2) == 0)
        {
            key = constant_string(arg);
            continue;
        }
        if (key != NULL)
        {
            JsonShapeField *field = palloc0(sizeof(JsonShapeField));

            field->name = key;
            field->optional = absent_on_null;
            field->shape = describe_json_value(arg, query, nullable,
                                               grouping_sets);
            if (absent_on_null && field->shape->result_not_null != 1)
                field->shape->result_not_null = 1;
            object->fields = lappend(object->fields, field);
            key = NULL;
        }
        else
        {
            JsonShape *value = describe_json_value(arg, query, nullable,
                                                   grouping_sets);

            if (absent_on_null && value->result_not_null != 1)
                value->result_not_null = 1;
            object->additional = merge_shapes(object->additional, value);
        }
    }
    return object;
}

static JsonShape *
json_array_from_args(List *args, bool absent_on_null, Query *query,
                     Bitmapset *nullable, bool grouping_sets)
{
    JsonShape *element = NULL;
    ListCell  *lc;

    foreach(lc, args)
    {
        JsonShape *value = describe_json_value(
            unwrap_json_value((Node *) lfirst(lc)), query, nullable,
            grouping_sets);

        if (absent_on_null && value->result_not_null != 1)
            value->result_not_null = 1;
        element = merge_shapes(element, value);
    }
    if (element == NULL)
    {
        element = unknown_shape();
        element->neutral = true;
    }
    return array_shape_from_element(element, 1);
}

static Node *
first_aggregate_argument(Aggref *aggregate)
{
    ListCell *lc;

    foreach(lc, aggregate->args)
    {
        TargetEntry *entry = lfirst_node(TargetEntry, lc);

        if (!entry->resjunk)
            return (Node *) entry->expr;
    }
    return NULL;
}

static JsonShape *
describe_json_aggregate(Aggref *aggregate, Query *query,
                        bool grouping_sets)
{
    static const char *const array_names[] = {
        "json_agg", "jsonb_agg", "json_agg_strict", "jsonb_agg_strict"
    };
    static const char *const object_names[] = {
        "json_object_agg", "jsonb_object_agg",
        "json_object_agg_strict", "jsonb_object_agg_strict",
        "json_object_agg_unique", "jsonb_object_agg_unique",
        "json_object_agg_unique_strict", "jsonb_object_agg_unique_strict"
    };
    Bitmapset *nullable;
    Node      *argument;
    JsonShape *shape;

    if (!pg_function_in(aggregate->aggfnoid, array_names,
                        lengthof(array_names)) &&
        !pg_function_in(aggregate->aggfnoid, object_names,
                        lengthof(object_names)))
        return NULL;

    nullable = nullable_for_aggregate(query, (Node *) aggregate->aggfilter);
    argument = first_aggregate_argument(aggregate);

    if (pg_function_in(aggregate->aggfnoid, array_names,
                       lengthof(array_names)))
    {
        bool strict = pg_function_named(aggregate->aggfnoid, "json_agg_strict") ||
                      pg_function_named(aggregate->aggfnoid, "jsonb_agg_strict");
        JsonShape *element = describe_json_value(argument, query, nullable,
                                                 grouping_sets);

        if (strict && element->result_not_null != 1)
            element->result_not_null = 1;
        shape = array_shape_from_element(element, 0);
    }
    else
    {
        List       *values = NIL;
        ListCell   *lc;
        JsonShape  *value = unknown_shape();
        JsonShape  *object = new_shape(PD_SHAPE_OBJECT);
        char       *function_name = get_func_name(aggregate->aggfnoid);
        bool        strict = function_name != NULL &&
                             strstr(function_name, "strict") != NULL;
        char       *key = NULL;
        int         seen = 0;

        foreach(lc, aggregate->args)
        {
            TargetEntry *entry = lfirst_node(TargetEntry, lc);

            if (!entry->resjunk)
                values = lappend(values, entry->expr);
        }
        foreach(lc, values)
        {
            if (seen == 0)
                key = constant_string((Node *) lfirst(lc));
            else if (seen == 1)
                value = describe_json_value((Node *) lfirst(lc), query,
                                            nullable, grouping_sets);
            seen++;
        }
        if (strict && value->result_not_null != 1)
            value->result_not_null = 1;
        if (key == NULL)
            object->additional = value;
        else
        {
            JsonShapeField *field = palloc0(sizeof(JsonShapeField));

            field->name = key;
            field->optional = false;
            field->shape = value;
            object->fields = lappend(object->fields, field);
        }
        object->result_not_null = 0;
        shape = object;
        if (function_name != NULL)
            pfree(function_name);
    }
    return shape;
}

static JsonShape *
describe_json_function(FuncExpr *func, Query *query, Bitmapset *nullable,
                       bool grouping_sets)
{
    if (pg_function_named(func->funcid, "json_build_object") ||
        pg_function_named(func->funcid, "jsonb_build_object"))
        return json_object_from_args(func->args, false, query, nullable,
                                     grouping_sets);
    if (pg_function_named(func->funcid, "json_build_array") ||
        pg_function_named(func->funcid, "jsonb_build_array"))
        return json_array_from_args(func->args, false, query, nullable,
                                    grouping_sets);
    if (pg_function_named(func->funcid, "to_json") ||
        pg_function_named(func->funcid, "to_jsonb") ||
        pg_function_named(func->funcid, "array_to_json") ||
        pg_function_named(func->funcid, "row_to_json"))
        return func->args == NIL ? unknown_shape() :
               describe_json_value((Node *) linitial(func->args), query,
                                   nullable, grouping_sets);
    if (pg_function_named(func->funcid, "json_object") ||
        pg_function_named(func->funcid, "jsonb_object"))
    {
        JsonShape *object = new_shape(PD_SHAPE_OBJECT);
        JsonShape *value = new_shape(PD_SHAPE_SCALAR);

        value->runtime_kind = pstrdup("string");
        value->type_oid = TEXTOID;
        value->result_not_null = 1;
        object->additional = value;
        object->result_not_null = 1;
        return object;
    }
    return NULL;
}

#if PG_VERSION_NUM >= 180000
static JsonShape *
describe_json_constructor(JsonConstructorExpr *constructor, Query *query,
                          Bitmapset *nullable, bool grouping_sets)
{
    switch (constructor->type)
    {
        case JSCTOR_JSON_OBJECT:
            return json_object_from_args(constructor->args,
                                         constructor->absent_on_null,
                                         query, nullable, grouping_sets);
        case JSCTOR_JSON_ARRAY:
            return json_array_from_args(constructor->args,
                                        constructor->absent_on_null,
                                        query, nullable, grouping_sets);
        case JSCTOR_JSON_OBJECTAGG:
        case JSCTOR_JSON_ARRAYAGG:
            if (constructor->func != NULL && IsA(constructor->func, Aggref))
                return describe_json_aggregate((Aggref *) constructor->func,
                                               query, grouping_sets);
            return unknown_shape();
        case JSCTOR_JSON_SCALAR:
            return constructor->args == NIL ? unknown_shape() :
                   describe_json_value((Node *) linitial(constructor->args),
                                       query, nullable, grouping_sets);
        case JSCTOR_JSON_PARSE:
            return unknown_shape();
        case JSCTOR_JSON_SERIALIZE:
            /* Serialization remains JSON only with a JSON RETURNING type. */
            return getBaseType(exprType((Node *) constructor)) == JSONOID ||
                   getBaseType(exprType((Node *) constructor)) == JSONBOID ?
                   unknown_shape() : NULL;
    }
    return unknown_shape();
}
#endif

static JsonShape *
describe_json_constant(Const *constant)
{
    JsonShape *shape;
    char      *text;

    if (constant->constisnull)
    {
        shape = new_shape(PD_SHAPE_SCALAR);
        shape->runtime_kind = pstrdup("null");
        shape->type_oid = constant->consttype;
        shape->result_not_null = 0;
        return shape;
    }

    text = constant_string((Node *) constant);
    if (text != NULL && strcmp(text, "[]") == 0)
    {
        shape = array_shape_from_element(unknown_shape(), 1);
        shape->neutral = true;
        shape->element->neutral = true;
        return shape;
    }
    if (text != NULL && strcmp(text, "{}") == 0)
    {
        shape = new_shape(PD_SHAPE_OBJECT);
        shape->result_not_null = 1;
        shape->neutral = true;
        return shape;
    }
    if (text != NULL &&
        (strcmp(text, "true") == 0 || strcmp(text, "false") == 0 ||
         strcmp(text, "null") == 0 || text[0] == '"' ||
         text[0] == '-' || (text[0] >= '0' && text[0] <= '9')))
    {
        shape = new_shape(PD_SHAPE_SCALAR);
        shape->runtime_kind = pstrdup(
            strcmp(text, "true") == 0 || strcmp(text, "false") == 0 ?
            "boolean" : strcmp(text, "null") == 0 ? "null" :
            text[0] == '"' ? "string" : "number");
        shape->type_oid = constant->consttype;
        shape->result_not_null = strcmp(text, "null") == 0 ? 0 : 1;
        return shape;
    }
    return unknown_shape();
}

static JsonShape *
describe_row(RowExpr *row, Query *query, Bitmapset *nullable,
             bool grouping_sets)
{
    JsonShape *object = new_shape(PD_SHAPE_OBJECT);
    ListCell  *arg_cell;
    ListCell  *name_cell;
    int        position = 1;

    object->result_not_null = 1;
    forboth(arg_cell, row->args, name_cell, row->colnames)
    {
        JsonShapeField *field = palloc0(sizeof(JsonShapeField));
        Node           *name_node = (Node *) lfirst(name_cell);

        field->name = IsA(name_node, String) ?
                      pstrdup(strVal(name_node)) :
                      psprintf("f%d", position);
        field->shape = describe_json_value((Node *) lfirst(arg_cell), query,
                                           nullable, grouping_sets);
        object->fields = lappend(object->fields, field);
        position++;
    }
    return object;
}

static JsonShape *
describe_json_value(Node *node, Query *query, Bitmapset *nullable,
                    bool grouping_sets)
{
    Oid base_type;

    if (node == NULL)
        return unknown_shape();
    check_stack_depth();
    node = unwrap_json_value(node);
    base_type = getBaseType(exprType(node));

    switch (nodeTag(node))
    {
        case T_Const:
            if (base_type == JSONOID || base_type == JSONBOID)
                return describe_json_constant((Const *) node);
            break;
        case T_FuncExpr:
        {
            JsonShape *function = describe_json_function((FuncExpr *) node,
                                                         query, nullable,
                                                         grouping_sets);
            if (function != NULL)
                return function;
            break;
        }
        case T_Aggref:
        {
            JsonShape *aggregate = describe_json_aggregate((Aggref *) node,
                                                           query,
                                                           grouping_sets);
            if (aggregate != NULL)
                return aggregate;
            break;
        }
        case T_CoalesceExpr:
        {
            JsonShape *shape = NULL;
            ListCell  *lc;

            foreach(lc, ((CoalesceExpr *) node)->args)
                shape = merge_shapes(shape,
                    describe_json_value((Node *) lfirst(lc), query, nullable,
                                        grouping_sets));
            if (shape != NULL && expr_is_not_null(node, query, nullable,
                                                  grouping_sets))
                shape->result_not_null = 1;
            return shape == NULL ? unknown_shape() : shape;
        }
        case T_CaseExpr:
        {
            CaseExpr *caseexpr = (CaseExpr *) node;
            JsonShape *shape = describe_json_value(
                (Node *) caseexpr->defresult, query, nullable, grouping_sets);
            ListCell *lc;

            foreach(lc, caseexpr->args)
                shape = merge_shapes(shape, describe_json_value(
                    (Node *) lfirst_node(CaseWhen, lc)->result, query,
                    nullable, grouping_sets));
            return shape;
        }
        case T_RowExpr:
            return describe_row((RowExpr *) node, query, nullable,
                                grouping_sets);
        case T_RelabelType:
            return describe_json_value((Node *) ((RelabelType *) node)->arg,
                                       query, nullable, grouping_sets);
        case T_CoerceViaIO:
            return describe_json_value((Node *) ((CoerceViaIO *) node)->arg,
                                       query, nullable, grouping_sets);
        case T_CoerceToDomain:
            return describe_json_value((Node *) ((CoerceToDomain *) node)->arg,
                                       query, nullable, grouping_sets);
#if PG_VERSION_NUM >= 180000
        case T_JsonConstructorExpr:
            return describe_json_constructor((JsonConstructorExpr *) node,
                                             query, nullable, grouping_sets);
#endif
        default:
            break;
    }

    if (base_type == JSONOID || base_type == JSONBOID)
        return unknown_shape();
    if (OidIsValid(get_element_type(base_type)))
        return sql_array_shape(node, query, nullable, grouping_sets);
    return scalar_shape(node, query, nullable, grouping_sets);
}

static void
append_json_nullable(StringInfo buffer, const char *name, int value)
{
    appendStringInfo(buffer, ",\"%s\":", name);
    if (value == PD_UNKNOWN)
        appendStringInfoString(buffer, "null");
    else
        appendStringInfoString(buffer, value != 0 ? "true" : "false");
}

static void
append_json_string_property(StringInfo buffer, const char *name,
                            const char *value)
{
    appendStringInfo(buffer, ",\"%s\":", name);
    if (value == NULL)
        appendStringInfoString(buffer, "null");
    else
        escape_json(buffer, value);
}

static void append_shape_json(StringInfo buffer, JsonShape *shape);

static void
append_shape_json(StringInfo buffer, JsonShape *shape)
{
    if (shape == NULL || shape->kind == PD_SHAPE_UNKNOWN)
    {
        appendStringInfoString(buffer, "{\"kind\":\"unknown\"}");
        return;
    }
    if (shape->kind == PD_SHAPE_SCALAR)
    {
        appendStringInfo(buffer,
            "{\"kind\":\"scalar\",\"runtime\":\"%s\",\"type_oid\":",
            shape->runtime_kind);
        if (OidIsValid(shape->type_oid))
            appendStringInfo(buffer, "%u", shape->type_oid);
        else
            appendStringInfoString(buffer, "null");
        append_json_string_property(buffer, "type_name",
            OidIsValid(shape->type_oid) ? format_type_be(shape->type_oid) : NULL);
        appendStringInfoString(buffer, ",\"source_table\":");
        if (OidIsValid(shape->source_table))
            appendStringInfo(buffer, "%u", shape->source_table);
        else
            appendStringInfoString(buffer, "null");
        append_json_string_property(buffer, "source_schema",
            OidIsValid(shape->source_table) ?
            get_namespace_name(get_rel_namespace(shape->source_table)) : NULL);
        append_json_string_property(buffer, "source_relation",
            OidIsValid(shape->source_table) ?
            get_rel_name(shape->source_table) : NULL);
        append_json_string_property(buffer, "source_column",
                                    shape->source_column);
        append_json_nullable(buffer, "base_not_null", shape->base_not_null);
        append_json_nullable(buffer, "result_not_null",
                             shape->result_not_null);
        appendStringInfoChar(buffer, '}');
        return;
    }
    if (shape->kind == PD_SHAPE_ARRAY)
    {
        appendStringInfoString(buffer, "{\"kind\":\"array\"");
        append_json_nullable(buffer, "result_not_null", shape->result_not_null);
        appendStringInfoString(buffer, ",\"element\":");
        append_shape_json(buffer, shape->element);
        appendStringInfoChar(buffer, '}');
        return;
    }

    appendStringInfoString(buffer, "{\"kind\":\"object\"");
    append_json_nullable(buffer, "result_not_null", shape->result_not_null);
    appendStringInfoString(buffer, ",\"fields\":[");
    if (shape->fields != NIL)
    {
        ListCell *lc;
        bool      first = true;

        foreach(lc, shape->fields)
        {
            JsonShapeField *field = (JsonShapeField *) lfirst(lc);

            if (!first)
                appendStringInfoChar(buffer, ',');
            first = false;
            appendStringInfoString(buffer, "{\"name\":");
            escape_json(buffer, field->name);
            appendStringInfo(buffer, ",\"optional\":%s,\"shape\":",
                             field->optional ? "true" : "false");
            append_shape_json(buffer, field->shape);
            appendStringInfoChar(buffer, '}');
        }
    }
    appendStringInfoString(buffer, "],\"additional\":");
    if (shape->additional == NULL)
        appendStringInfoString(buffer, "null");
    else
        append_shape_json(buffer, shape->additional);
    appendStringInfoChar(buffer, '}');
}

static Jsonb *
json_shape_for_expression(Node *node, Query *query, Bitmapset *nullable,
                          bool grouping_sets)
{
    Oid         base_type = getBaseType(exprType(node));
    JsonShape  *shape;
    StringInfoData json;
    Datum       parsed;

    if (base_type != JSONOID && base_type != JSONBOID)
        return NULL;
    shape = describe_json_value(node, query, nullable, grouping_sets);
    initStringInfo(&json);
    append_shape_json(&json, shape);
    parsed = DirectFunctionCall1(jsonb_in, CStringGetDatum(json.data));
    pfree(json.data);
    return DatumGetJsonbP(parsed);
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
        Jsonb       *result_shape;

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

        result_shape = json_shape_for_expression((Node *) tle->expr, query,
                                                 nullable, grouping_sets);

        emit_row(rsinfo, "column", ord, tle->resname,
                 exprType((Node *) tle->expr),
                 source_table, source_column, base_not_null, result_not_null,
                 result_shape);
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
                 InvalidOid, NULL, PD_UNKNOWN, PD_UNKNOWN, NULL);

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

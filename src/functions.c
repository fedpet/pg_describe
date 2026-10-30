/* Function catalog and reusable, symbolic non-null proofs. No function executes. */
#include "postgres.h"
#include "fmgr.h"
#include "funcapi.h"
#include "access/htup_details.h"
#include "access/heapam.h"
#include "access/xact.h"
#include "access/table.h"
#include "access/genam.h"
#include "catalog/pg_language_d.h"
#include "catalog/pg_namespace.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_type.h"
#include "executor/functions.h"
#include "miscadmin.h"
#include "nodes/bitmapset.h"
#include "nodes/nodeFuncs.h"
#include "nodes/readfuncs.h"
#include "parser/analyze.h"
#include "tcop/tcopprot.h"
#include "utils/acl.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/hsearch.h"
#include "utils/jsonb.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/resowner.h"
#include "utils/syscache.h"
#include "functions.h"

/* DNF: [] = unknown; [[]] = always; [[1,2],[3]] = (arg1 AND arg2) OR arg3.
 * Bounding both depth and formula growth makes recursive/large bodies safe. */
#define PD_PROOF_LIMIT 64
#define PD_DEPTH_LIMIT 32

typedef struct FunctionProof
{
    Oid function;
    bool visiting;
    List *condition;
} FunctionProof;

typedef struct FunctionAnalysis
{
    HTAB *proofs;
    MemoryContext context;
    int depth;
} FunctionAnalysis;

static List *function_condition(Oid function, FunctionAnalysis *analysis);
static List *expression_condition(Node *node, FunctionAnalysis *analysis);

static FunctionAnalysis
new_analysis(void)
{
    HASHCTL ctl = {0};
    FunctionAnalysis analysis;
    ctl.keysize = sizeof(Oid);
    ctl.entrysize = sizeof(FunctionProof);
    ctl.hcxt = CurrentMemoryContext;
    analysis.proofs = hash_create("pg_describe function proofs", 128, &ctl,
                                 HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
    analysis.context = CurrentMemoryContext;
    analysis.depth = 0;
    return analysis;
}

static List *
proof_always(void)
{
    return list_make1(NULL);
}

static List *
proof_add(List *proof, Bitmapset *clause)
{
    ListCell *lc;
    List *result = NIL;
    foreach(lc, proof)
        if (bms_is_subset(lfirst(lc), clause))
            return proof;
    if (list_length(proof) >= PD_PROOF_LIMIT)
        return proof;
    foreach(lc, proof)
        if (!bms_is_subset(clause, lfirst(lc)))
            result = lappend(result, lfirst(lc));
    return lappend(result, clause);
}

/* Parse analysis leaves omitted defaults for the planner to expand. Reuse their
 * stored expression trees without planning or executing them. */
static List *
complete_arguments(Oid function, List *arguments)
{
    HeapTuple tuple = SearchSysCache1(PROCOID, ObjectIdGetDatum(function));
    Form_pg_proc proc;
    int missing;
    bool isnull;
    Datum value;
    List *defaults;
    if (!HeapTupleIsValid(tuple))
        return arguments;
    proc = (Form_pg_proc) GETSTRUCT(tuple);
    missing = proc->pronargs - list_length(arguments);
    if (missing <= 0 || missing > proc->pronargdefaults)
    {
        ReleaseSysCache(tuple);
        return arguments;
    }
    value = SysCacheGetAttr(PROCOID, tuple, Anum_pg_proc_proargdefaults, &isnull);
    defaults = isnull ? NIL : stringToNode(TextDatumGetCString(value));
    arguments = list_concat(list_copy(arguments),
        list_copy_tail(defaults, list_length(defaults) - missing));
    ReleaseSysCache(tuple);
    return arguments;
}

static List *
proof_or(List *left, List *right)
{
    List *result = list_copy(left);
    ListCell *lc;
    foreach(lc, right)
        result = proof_add(result, lfirst(lc));
    return result;
}

static List *
proof_and(List *left, List *right)
{
    List *result = NIL;
    ListCell *l;
    ListCell *r;
    foreach(l, left)
        foreach(r, right)
            result = proof_add(result, bms_union(lfirst(l), lfirst(r)));
    return result;
}

static List *
substitute_condition(List *condition, List *arguments,
                     FunctionAnalysis *analysis)
{
    List *result = NIL;
    ListCell *lc;
    foreach(lc, condition)
    {
        Bitmapset *clause = lfirst(lc);
        List *substituted = proof_always();
        int argument = -1;
        while ((argument = bms_next_member(clause, argument)) >= 0)
        {
            if (argument == 0 || argument > list_length(arguments))
            {
                substituted = NIL;
                break;
            }
            substituted = proof_and(substituted,
                expression_condition(list_nth(arguments, argument - 1), analysis));
        }
        result = proof_or(result, substituted);
    }
    return result;
}

static Oid
signature_oid(const char *signature, Form_pg_proc proc)
{
    LOCAL_FCINFO(call, 1);
    Datum result;
    size_t length = strlen(NameStr(proc->proname));
    char *qualified;
    if (strncmp(signature, NameStr(proc->proname), length) != 0 ||
        signature[length] != '(')
        return InvalidOid;
    qualified = psprintf("pg_catalog.%s", signature);
    InitFunctionCallInfoData(*call, NULL, 1, InvalidOid, NULL, NULL);
    call->args[0].value = CStringGetTextDatum(qualified);
    call->args[0].isnull = false;
    result = to_regprocedure(call);
    return call->isnull ? InvalidOid : DatumGetObjectId(result);
}

/* PostgreSQL's numeric-to-numeric cast family preserves non-null values.
 * JSON conversions deliberately do not participate: JSON null is SQL NULL. */
static bool
numeric_type(Oid type)
{
    return type == INT2OID || type == INT4OID || type == INT8OID ||
           type == FLOAT4OID || type == FLOAT8OID || type == NUMERICOID;
}

static bool
numeric_cast(Form_pg_proc proc)
{
    static const struct { Oid type; const char *name; } types[] = {
        {INT2OID, "int2"}, {INT4OID, "int4"}, {INT8OID, "int8"},
        {FLOAT4OID, "float4"}, {FLOAT8OID, "float8"}, {NUMERICOID, "numeric"}
    };
    int i;
    if (proc->pronargs != 1 || !numeric_type(proc->proargtypes.values[0]))
        return false;
    for (i = 0; i < lengthof(types); i++)
        if (proc->prorettype == types[i].type &&
            strcmp(NameStr(proc->proname), types[i].name) == 0)
            return true;
    return false;
}

/* Exact overload identities: functions of the same name need not share facts. */
static List *
builtin_condition(Oid function, Form_pg_proc proc)
{
    static const char *const strict_signatures[] = {
        "upper(text)", "lower(text)", "length(text)", "char_length(text)",
        "btrim(text)", "btrim(text,text)", "ltrim(text)", "ltrim(text,text)",
        "rtrim(text)", "rtrim(text,text)", "textcat(text,text)",
        "date_trunc(text,timestamp)", "date_trunc(text,timestamptz)",
        "date_trunc(text,timestamptz,text)", "date_trunc(text,interval)",
        "abs(int2)", "abs(int4)", "abs(int8)", "abs(float4)", "abs(float8)",
        "abs(numeric)", "round(numeric)", "round(numeric,int4)", "round(float8)",
        "numeric(numeric,int4)", "text(bool)", "text(\"char\")", "text(bpchar)",
        "text(inet)", "text(name)", "text(xml)", "varchar(varchar,int4,bool)",
        "varchar(name)",
        "int2eq(int2,int2)", "int2ne(int2,int2)", "int2lt(int2,int2)",
        "int2le(int2,int2)", "int2gt(int2,int2)", "int2ge(int2,int2)",
        "int4eq(int4,int4)", "int4ne(int4,int4)", "int4lt(int4,int4)",
        "int4le(int4,int4)", "int4gt(int4,int4)", "int4ge(int4,int4)",
        "int8eq(int8,int8)", "int8ne(int8,int8)", "int8lt(int8,int8)",
        "int8le(int8,int8)", "int8gt(int8,int8)", "int8ge(int8,int8)",
        "texteq(text,text)", "textne(text,text)", "text_lt(text,text)",
        "text_le(text,text)", "text_gt(text,text)", "text_ge(text,text)",
        "uuid_eq(uuid,uuid)", "uuid_ne(uuid,uuid)", "uuid_lt(uuid,uuid)",
        "uuid_le(uuid,uuid)", "uuid_gt(uuid,uuid)", "uuid_ge(uuid,uuid)",
        "int4pl(int4,int4)", "int4mi(int4,int4)", "int4mul(int4,int4)",
        "int4div(int4,int4)", "int8pl(int8,int8)", "int8mi(int8,int8)",
        "booland_statefunc(bool,bool)", "boolor_statefunc(bool,bool)",
        "booleq(bool,bool)", "boolne(bool,bool)",
        "date(timestamp)", "date(timestamptz)", "timestamp(date)",
        "timestamp(timestamptz)", "timestamptz(date)", "timestamptz(timestamp)"
    };
    static const char *const always_signatures[] = {
        "count()", "count(\"any\")", "now()", "transaction_timestamp()", "statement_timestamp()",
        "clock_timestamp()", "timeofday()", "random()", "gen_random_uuid()",
        "uuidv4()", "uuidv7()", "uuidv7(interval)", "concat(\"any\")", "concat_ws(text,\"any\")"
    };
    int i;
    if (proc->pronamespace != PG_CATALOG_NAMESPACE)
        return NIL;
    if (numeric_cast(proc))
        return list_make1(bms_make_singleton(1));
    for (i = 0; i < lengthof(always_signatures); i++)
    {
        Oid known = signature_oid(always_signatures[i], proc);
        if (OidIsValid(known) && known == function)
        {
            /* uuidv7's shift and concat_ws's separator can be NULL. */
            if ((strcmp(NameStr(proc->proname), "uuidv7") == 0 && proc->pronargs > 0) ||
                strcmp(NameStr(proc->proname), "concat_ws") == 0)
                return list_make1(bms_make_singleton(1));
            return proof_always();
        }
    }
    for (i = 0; i < lengthof(strict_signatures); i++)
    {
        Oid known = signature_oid(strict_signatures[i], proc);
        if (OidIsValid(known) && known == function)
        {
            Bitmapset *all = NULL;
            int arg;
            for (arg = 1; arg <= proc->pronargs; arg++)
                all = bms_add_member(all, arg);
            return list_make1(all);
        }
    }
    return NIL;
}

static List *
expression_condition(Node *node, FunctionAnalysis *analysis)
{
    ListCell *lc;
    List *result;
    if (node == NULL)
        return NIL;
    check_stack_depth();
    switch (nodeTag(node))
    {
        case T_Const:
            return ((Const *) node)->constisnull ? NIL : proof_always();
        case T_Param:
        {
            Param *param = (Param *) node;
            return param->paramkind == PARAM_EXTERN && param->paramid > 0 ?
                list_make1(bms_make_singleton(param->paramid)) : NIL;
        }
        case T_FuncExpr:
        {
            FuncExpr *func = (FuncExpr *) node;
            if (func->funcretset)
                return NIL;
            foreach(lc, func->args)
                if (IsA(lfirst(lc), NamedArgExpr))
                    return NIL;
            return substitute_condition(function_condition(func->funcid, analysis),
                                        complete_arguments(func->funcid, func->args), analysis);
        }
        case T_OpExpr:
        {
            OpExpr *op = (OpExpr *) node;
            if (op->opretset)
                return NIL;
            return substitute_condition(function_condition(op->opfuncid, analysis),
                                        complete_arguments(op->opfuncid, op->args), analysis);
        }
        case T_CoalesceExpr:
            result = NIL;
            foreach(lc, ((CoalesceExpr *) node)->args)
                result = proof_or(result, expression_condition(lfirst(lc), analysis));
            return result;
        case T_CaseExpr:
            result = expression_condition((Node *) ((CaseExpr *) node)->defresult, analysis);
            foreach(lc, ((CaseExpr *) node)->args)
                result = proof_and(result, expression_condition(
                    (Node *) lfirst_node(CaseWhen, lc)->result, analysis));
            return result;
        case T_BoolExpr:
            result = proof_always();
            foreach(lc, ((BoolExpr *) node)->args)
                result = proof_and(result, expression_condition(lfirst(lc), analysis));
            return result;
        case T_NullTest:
        case T_BooleanTest:
        case T_DistinctExpr:
        case T_ArrayExpr:
        case T_RowExpr:
        case T_SQLValueFunction:
            return proof_always();
        case T_RelabelType:
            return expression_condition((Node *) ((RelabelType *) node)->arg, analysis);
        case T_CollateExpr:
            return expression_condition((Node *) ((CollateExpr *) node)->arg, analysis);
        default:
            return NIL;
    }
}

/* Only guaranteed one-row SELECTs establish a scalar function's result proof.
 * A NOT NULL target over a possibly empty relation is insufficient. */
static List *
query_condition(Query *query, FunctionAnalysis *analysis, Oid return_type)
{
    TargetEntry *target = NULL;
    ListCell *lc;
    if (query->commandType != CMD_SELECT || query->setOperations != NULL ||
        query->hasTargetSRFs || query->hasAggs || query->groupClause != NIL ||
        query->groupingSets != NIL || query->havingQual != NULL ||
        query->limitCount != NULL || query->limitOffset != NULL ||
        query->jointree == NULL || query->jointree->fromlist != NIL ||
        query->jointree->quals != NULL)
        return NIL;
    foreach(lc, query->targetList)
    {
        TargetEntry *entry = lfirst_node(TargetEntry, lc);
        if (!entry->resjunk)
        {
            if (target != NULL)
                return NIL;
            target = entry;
        }
    }
    return target == NULL || exprType((Node *) target->expr) != return_type ? NIL
        : expression_condition((Node *) target->expr, analysis);
}

/* Parse string bodies using Postgres's own SQL-function parameter hooks. A body
 * may no longer resolve after schema/search_path changes. Isolate expected parse
 * failures, restoring the transaction, locks and resource owner on every path. */
static List *
string_body_condition(HeapTuple tuple, FunctionAnalysis *analysis)
{
    MemoryContext outer = CurrentMemoryContext;
    ResourceOwner owner = CurrentResourceOwner;
    List *volatile result = NIL;
    BeginInternalSubTransaction(NULL);
    PG_TRY();
    {
        char *source = TextDatumGetCString(SysCacheGetAttrNotNull(
            PROCOID, tuple, Anum_pg_proc_prosrc));
        List *raw = pg_parse_query(source);
        if (list_length(raw) == 1)
        {
            SQLFunctionParseInfoPtr info = prepare_sql_fn_parse_info(tuple, NULL, InvalidOid);
            Query *query = parse_analyze_withcb(linitial_node(RawStmt, raw), source,
                (ParserSetupHook) sql_fn_parser_setup, info, NULL);
            List *condition = query_condition(query, analysis, ((Form_pg_proc) GETSTRUCT(tuple))->prorettype);
            ListCell *lc;
            MemoryContextSwitchTo(outer);
            foreach(lc, condition)
                result = lappend(result, bms_copy(lfirst(lc)));
        }
        ReleaseCurrentSubTransaction();
        MemoryContextSwitchTo(outer);
        CurrentResourceOwner = owner;
    }
    PG_CATCH();
    {
        ErrorData *error;
        MemoryContextSwitchTo(outer);
        error = CopyErrorData();
        FlushErrorState();
        RollbackAndReleaseCurrentSubTransaction();
        MemoryContextSwitchTo(outer);
        CurrentResourceOwner = owner;
        if (ERRCODE_TO_CATEGORY(error->sqlerrcode) != ERRCODE_SYNTAX_ERROR_OR_ACCESS_RULE_VIOLATION &&
            error->sqlerrcode != ERRCODE_FEATURE_NOT_SUPPORTED &&
            error->sqlerrcode != ERRCODE_INVALID_FUNCTION_DEFINITION &&
            error->sqlerrcode != ERRCODE_INDETERMINATE_DATATYPE)
            ReThrowError(error);
        FreeErrorData(error);
        result = NIL;
    }
    PG_END_TRY();
    return result;
}

static List *
function_condition(Oid function, FunctionAnalysis *analysis)
{
    FunctionProof *proof;
    bool found;
    HeapTuple tuple;
    Form_pg_proc proc;
    List *condition;
    bool isnull;
    Datum body;
    MemoryContext context = CurrentMemoryContext;
    if (analysis->depth >= PD_DEPTH_LIMIT)
        return NIL;
    proof = hash_search(analysis->proofs, &function, HASH_ENTER, &found);
    if (found)
        return proof->visiting ? NIL : proof->condition;
    proof->visiting = true;
    proof->condition = NIL;
    tuple = SearchSysCache1(PROCOID, ObjectIdGetDatum(function));
    if (!HeapTupleIsValid(tuple))
        return NIL;
    proc = (Form_pg_proc) GETSTRUCT(tuple);
    condition = builtin_condition(function, proc);
    analysis->depth++;
    if (condition == NIL && proc->prokind == PROKIND_FUNCTION &&
        !proc->proretset && proc->prolang == SQLlanguageId &&
        !proc->prosecdef && heap_attisnull(tuple, Anum_pg_proc_proconfig, NULL))
    {
        body = SysCacheGetAttr(PROCOID, tuple, Anum_pg_proc_prosqlbody, &isnull);
        if (!isnull)
        {
            Node *node = stringToNode(TextDatumGetCString(body));
            if (IsA(node, Query))
                condition = query_condition((Query *) node, analysis, proc->prorettype);
            else if (IsA(node, List))
            {
                List *queries = linitial_node(List, (List *) node);
                if (list_length(queries) == 1 && IsA(linitial(queries), Query))
                    condition = query_condition(linitial_node(Query, queries), analysis, proc->prorettype);
            }
        }
        else
            condition = string_body_condition(tuple, analysis);
        if (proc->proisstrict)
        {
            Bitmapset *all = NULL;
            int arg;
            MemoryContextSwitchTo(context);
            for (arg = 1; arg <= proc->pronargs; arg++)
                all = bms_add_member(all, arg);
            condition = proof_and(condition, list_make1(all));
        }
    }
    analysis->depth--;
    ReleaseSysCache(tuple);
    {
        ListCell *lc;
        MemoryContext previous = MemoryContextSwitchTo(analysis->context);
        foreach(lc, condition)
            proof->condition = lappend(proof->condition, bms_copy(lfirst(lc)));
        MemoryContextSwitchTo(previous);
    }
    proof->visiting = false;
    return proof->condition;
}

bool
pd_function_result_not_null(Oid function, List *argument_facts)
{
    FunctionAnalysis analysis = new_analysis();
    List *condition = function_condition(function, &analysis);
    ListCell *lc;
    List *placeholders = NIL;
    List *completed;
    int i;
    for (i = 0; i < list_length(argument_facts); i++)
        placeholders = lappend(placeholders, NULL);
    completed = complete_arguments(function, placeholders);
    for (i = list_length(argument_facts); i < list_length(completed); i++)
    {
        List *proof = expression_condition(list_nth(completed, i), &analysis);
        ListCell *entry;
        bool always = false;
        foreach(entry, proof)
            if (bms_is_empty(lfirst(entry))) always = true;
        argument_facts = lappend_int(argument_facts, always);
    }
    foreach(lc, condition)
    {
        Bitmapset *clause = lfirst(lc);
        bool proven = true;
        int arg = -1;
        while ((arg = bms_next_member(clause, arg)) >= 0)
            if (arg == 0 || arg > list_length(argument_facts) ||
                !list_nth_int(argument_facts, arg - 1))
                proven = false;
        if (proven)
            return true;
    }
    return false;
}

static Jsonb *
condition_json(List *condition)
{
    StringInfoData json;
    ListCell *lc;
    bool first = true;
    initStringInfo(&json);
    appendStringInfoChar(&json, '[');
    foreach(lc, condition)
    {
        Bitmapset *clause = lfirst(lc);
        int arg = -1;
        bool first_arg = true;
        if (!first) appendStringInfoChar(&json, ',');
        first = false;
        appendStringInfoChar(&json, '[');
        while ((arg = bms_next_member(clause, arg)) >= 0)
        {
            if (!first_arg) appendStringInfoChar(&json, ',');
            first_arg = false;
            appendStringInfo(&json, "%d", arg);
        }
        appendStringInfoChar(&json, ']');
    }
    appendStringInfoChar(&json, ']');
    return DatumGetJsonbP(DirectFunctionCall1(jsonb_in, CStringGetDatum(json.data)));
}

PG_FUNCTION_INFO_V1(pg_describe_functions);
Datum
pg_describe_functions(PG_FUNCTION_ARGS)
{
    ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
    ArrayType *schemas = PG_ARGISNULL(0) ? NULL : PG_GETARG_ARRAYTYPE_P(0);
    Datum *schema_values = NULL;
    bool *schema_nulls = NULL;
    int schema_count = 0;
    Relation catalog;
    SysScanDesc scan;
    HeapTuple tuple;
    FunctionAnalysis analysis = new_analysis();
    InitMaterializedSRF(fcinfo, MAT_SRF_USE_EXPECTED_DESC);
    if (schemas != NULL)
        deconstruct_array(schemas, TEXTOID, -1, false, TYPALIGN_INT,
                          &schema_values, &schema_nulls, &schema_count);
    catalog = table_open(ProcedureRelationId, AccessShareLock);
    scan = systable_beginscan(catalog, InvalidOid, false, NULL, 0, NULL);
    while (HeapTupleIsValid(tuple = systable_getnext(scan)))
    {
        Form_pg_proc proc = (Form_pg_proc) GETSTRUCT(tuple);
        char *schema = get_namespace_name(proc->pronamespace);
        Datum values[16];
        bool nulls[16] = {false};
        bool selected = schemas == NULL;
        int i;
        List *condition;
        Datum *argtypes;
        Datum defaults;
        bool defaults_null;
        List *default_facts = NIL;
        for (i = 0; i < schema_count; i++)
            if (!schema_nulls[i] && strcmp(schema, TextDatumGetCString(schema_values[i])) == 0)
                selected = true;
        if (!selected ||
            object_aclcheck(NamespaceRelationId, proc->pronamespace, GetUserId(), ACL_USAGE) != ACLCHECK_OK ||
            object_aclcheck(ProcedureRelationId, proc->oid, GetUserId(), ACL_EXECUTE) != ACLCHECK_OK)
            continue;
        CHECK_FOR_INTERRUPTS();
        condition = function_condition(proc->oid, &analysis);
        argtypes = palloc(sizeof(Datum) * proc->pronargs);
        for (i = 0; i < proc->pronargs; i++)
            argtypes[i] = ObjectIdGetDatum(proc->proargtypes.values[i]);
        defaults = SysCacheGetAttr(PROCOID, tuple, Anum_pg_proc_proargdefaults, &defaults_null);
        if (!defaults_null)
        {
            List *expressions = stringToNode(TextDatumGetCString(defaults));
            ListCell *lc;
            foreach(lc, expressions)
            {
                List *proof = expression_condition(lfirst(lc), &analysis);
                ListCell *clause;
                bool always = false;
                foreach(clause, proof)
                    if (bms_is_empty(lfirst(clause))) always = true;
                default_facts = lappend_int(default_facts, always);
            }
        }
        values[0] = ObjectIdGetDatum(proc->oid);
        values[1] = CStringGetTextDatum(schema);
        values[2] = CStringGetTextDatum(NameStr(proc->proname));
        values[3] = CStringGetTextDatum(proc->prokind == PROKIND_FUNCTION ? "function" :
            proc->prokind == PROKIND_AGGREGATE ? "aggregate" :
            proc->prokind == PROKIND_WINDOW ? "window" : "procedure");
        values[4] = PointerGetDatum(construct_array(argtypes, proc->pronargs, OIDOID,
                                                  sizeof(Oid), true, TYPALIGN_INT));
        values[5] = ObjectIdGetDatum(proc->prorettype);
        values[6] = BoolGetDatum(proc->proretset);
        values[7] = Int32GetDatum(proc->pronargdefaults);
        values[8] = ObjectIdGetDatum(proc->provariadic);
        values[9] = BoolGetDatum(proc->proisstrict);
        values[10] = JsonbPGetDatum(condition_json(condition));
        {
            Datum *facts = palloc(sizeof(Datum) * list_length(default_facts));
            for (i = 0; i < list_length(default_facts); i++)
                facts[i] = BoolGetDatum(list_nth_int(default_facts, i));
            values[11] = PointerGetDatum(construct_array(facts, list_length(default_facts),
                BOOLOID, 1, true, TYPALIGN_CHAR));
        }
        {
            Datum names = SysCacheGetAttr(PROCOID, tuple, Anum_pg_proc_proargnames, &nulls[12]);
            values[12] = names;
        }
        values[13] = SysCacheGetAttr(PROCOID, tuple, Anum_pg_proc_proallargtypes, &nulls[13]);
        values[14] = SysCacheGetAttr(PROCOID, tuple, Anum_pg_proc_proargmodes, &nulls[14]);
        values[15] = CharGetDatum(proc->provolatile);
        tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, values, nulls);
    }
    systable_endscan(scan);
    table_close(catalog, AccessShareLock);
    PG_RETURN_NULL();
}

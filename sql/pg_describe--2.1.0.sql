\echo Use "CREATE EXTENSION pg_describe" to load this file. \quit

-- pg_describe(sql) -> one row per parameter, then one row per result column.
--
-- result_shape is a recursive JSON protocol for JSON/JSONB construction
-- expressions which parse analysis can resolve. It is NULL for non-JSON
-- columns, and {"kind":"unknown"} for JSON whose contents are not statically
-- knowable. Database drivers continue to perform their native JSON decoding;
-- this metadata is for generators only.
CREATE FUNCTION pg_describe(sql text)
RETURNS TABLE (kind            text,
               ord             int,
               name            text,
               type_oid        oid,
               type_name       text,
               source_table    regclass,
               source_column   text,
               base_not_null   boolean,
               result_not_null boolean,
               result_shape    jsonb,
               array_dimensions integer,
               array_element_not_null boolean)
AS 'MODULE_PATHNAME', 'pg_describe'
LANGUAGE C VOLATILE STRICT;

COMMENT ON FUNCTION pg_describe(text) IS
    'Parse and analyse a statement without executing it; report inferred '
    'parameter and result types, provenance, outer-join-aware nullability, '
    'recursive JSON shapes, and provable array rank and element nullability.';

-- Complete routine discovery, including conservative symbolic non-null proofs.
CREATE FUNCTION pg_describe_functions(schemas text[] DEFAULT NULL)
RETURNS TABLE (
    function_oid oid,
    schema_name text,
    function_name text,
    kind text,
    argument_types oid[],
    return_type oid,
    returns_set boolean,
    default_arguments integer,
    variadic_type oid,
    strict boolean,
    nonnull_when jsonb,
    default_not_null boolean[],
    argument_names text[],
    all_argument_types oid[],
    argument_modes "char"[],
    volatility "char"
)
AS 'MODULE_PATHNAME', 'pg_describe_functions'
LANGUAGE C VOLATILE;

COMMENT ON FUNCTION pg_describe_functions(text[]) IS
    'Discover callable routines in selected schemas without execution; report '
    'signatures and non-null conditions as an OR of ANDs of one-based argument indexes.';

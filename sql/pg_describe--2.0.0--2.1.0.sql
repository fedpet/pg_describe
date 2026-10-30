
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

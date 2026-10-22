\echo Use "CREATE EXTENSION pg_describe" to load this file. \quit

-- pg_describe(sql) -> one row per parameter, then one row per result column.
--
-- result_shape is a recursive JSON protocol for JSON/JSONB construction
-- expressions which parse analysis can resolve. It is NULL for non-JSON
-- columns, and {"kind":"unknown"} for JSON whose contents are not statically
-- knowable. Bun and other drivers continue to perform their native JSON
-- decoding; this metadata is for generators only.
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
               result_shape    jsonb)
AS 'MODULE_PATHNAME', 'pg_describe'
LANGUAGE C VOLATILE STRICT;

COMMENT ON FUNCTION pg_describe(text) IS
    'Parse and analyse a statement without executing it; report inferred '
    'parameter and result types, provenance, outer-join-aware nullability, '
    'and recursive shapes for statically analyzable JSON constructors.';

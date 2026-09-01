-- A RETURNS TABLE row type cannot be changed by CREATE OR REPLACE.
DROP FUNCTION pg_describe(text);

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

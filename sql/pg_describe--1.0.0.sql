\echo Use "CREATE EXTENSION pg_describe" to load this file. \quit

-- pg_describe(sql) -> one row per parameter, then one row per result column.
--
-- The OUT parameters spelled out by RETURNS TABLE are what give the C code its
-- result descriptor at run time.
--
-- VOLATILE: the answer depends on catalog contents, which another session can
-- change underneath us, and the call takes locks. Deliberately NOT marked
-- PARALLEL SAFE -- there is no reason to describe a query inside a parallel
-- worker, and parse analysis is not something to invite into one.
--
-- STRICT: a NULL argument returns the empty set without entering the C function
-- at all. Describing "no query" is not an error, it is nothing.
CREATE FUNCTION pg_describe(sql text)
RETURNS TABLE (kind            text,
               ord             int,
               name            text,
               type_oid        oid,
               type_name       text,
               source_table    regclass,
               source_column   text,
               base_not_null   boolean,
               result_not_null boolean)
AS 'MODULE_PATHNAME', 'pg_describe'
LANGUAGE C VOLATILE STRICT;

COMMENT ON FUNCTION pg_describe(text) IS
    'Parse and analyse a statement without executing it; report inferred '
    'parameter types and result column types, with source provenance and '
    'outer-join-aware nullability.';

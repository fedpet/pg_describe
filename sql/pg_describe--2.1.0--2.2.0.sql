
CREATE FUNCTION pg_describe_facts(sql text) RETURNS jsonb
AS 'MODULE_PATHNAME', 'pg_describe_facts' LANGUAGE C VOLATILE STRICT;
COMMENT ON FUNCTION pg_describe_facts(text) IS
  'Describe outer statement/COPY transport and finite result-value proofs without executing the statement.';
CREATE FUNCTION pg_describe_type_facts(type_oid oid) RETURNS jsonb
AS 'MODULE_PATHNAME', 'pg_describe_type_facts' LANGUAGE C VOLATILE STRICT;
COMMENT ON FUNCTION pg_describe_type_facts(oid) IS
  'Report finite domain/array-element value proofs from validated enforced constraints.';

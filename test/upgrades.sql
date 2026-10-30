\set ON_ERROR_STOP on
SET client_min_messages = warning;
BEGIN;

DROP EXTENSION IF EXISTS pg_describe;
CREATE SCHEMA upgrade_check;
CREATE EXTENSION pg_describe SCHEMA upgrade_check;

-- Capture the installed function and representative metadata, including both
-- fields added in v2 and the recursive JSON protocol.
CREATE TEMP TABLE fresh_definition AS
SELECT pg_get_functiondef('upgrade_check.pg_describe(text)'::regprocedure) AS definition;
CREATE TEMP TABLE fresh_function_definition AS
SELECT pg_get_functiondef('upgrade_check.pg_describe_functions(text[])'::regprocedure) AS definition;
CREATE TEMP TABLE fresh_functions AS
SELECT function_name, argument_types, return_type, nonnull_when
FROM upgrade_check.pg_describe_functions(ARRAY['pg_catalog'])
WHERE function_name IN ('now', 'upper', 'uuidv7');
CREATE TEMP TABLE fresh_result AS
SELECT * FROM upgrade_check.pg_describe(
  'SELECT ARRAY[1, 2] AS ids, jsonb_build_object(''id'', 1) AS doc');

DO $$
BEGIN
  IF NOT EXISTS (
    SELECT FROM fresh_result
    WHERE name = 'ids' AND array_dimensions = 1 AND array_element_not_null
  ) OR NOT EXISTS (
    SELECT FROM fresh_result
    WHERE name = 'doc' AND result_shape ->> 'kind' = 'object'
  ) THEN
    RAISE EXCEPTION 'Fresh installation is missing v2 metadata';
  END IF;
END;
$$;

CREATE FUNCTION pg_temp.assert_upgrade() RETURNS void LANGUAGE plpgsql AS $$
BEGIN
  IF (SELECT extversion FROM pg_extension WHERE extname = 'pg_describe') <> '2.1.0'
    OR pg_get_functiondef('upgrade_check.pg_describe(text)'::regprocedure)
       IS DISTINCT FROM (SELECT definition FROM fresh_definition)
    OR pg_get_functiondef('upgrade_check.pg_describe_functions(text[])'::regprocedure)
       IS DISTINCT FROM (SELECT definition FROM fresh_function_definition)
    OR EXISTS (
      (SELECT * FROM fresh_functions EXCEPT ALL
       SELECT function_name, argument_types, return_type, nonnull_when
       FROM upgrade_check.pg_describe_functions(ARRAY['pg_catalog'])
       WHERE function_name IN ('now', 'upper', 'uuidv7'))
      UNION ALL
      (SELECT function_name, argument_types, return_type, nonnull_when
       FROM upgrade_check.pg_describe_functions(ARRAY['pg_catalog'])
       WHERE function_name IN ('now', 'upper', 'uuidv7')
       EXCEPT ALL SELECT * FROM fresh_functions)
    )
    OR EXISTS (
      (SELECT * FROM fresh_result EXCEPT ALL
       SELECT * FROM upgrade_check.pg_describe(
         'SELECT ARRAY[1, 2] AS ids, jsonb_build_object(''id'', 1) AS doc'))
      UNION ALL
      (SELECT * FROM upgrade_check.pg_describe(
         'SELECT ARRAY[1, 2] AS ids, jsonb_build_object(''id'', 1) AS doc')
       EXCEPT ALL SELECT * FROM fresh_result)
    ) THEN
    RAISE EXCEPTION 'Upgraded installation differs from a fresh installation';
  END IF;
END;
$$;

DROP EXTENSION pg_describe;
CREATE EXTENSION pg_describe SCHEMA upgrade_check VERSION '1.0.0';
ALTER EXTENSION pg_describe UPDATE TO '2.1.0';
SELECT pg_temp.assert_upgrade();

DROP EXTENSION pg_describe;
CREATE EXTENSION pg_describe SCHEMA upgrade_check VERSION '1.1.0';
ALTER EXTENSION pg_describe UPDATE TO '2.1.0';
SELECT pg_temp.assert_upgrade();

DROP EXTENSION pg_describe;
CREATE EXTENSION pg_describe SCHEMA upgrade_check VERSION '1.0.0';
ALTER EXTENSION pg_describe UPDATE TO '1.1.0';
ALTER EXTENSION pg_describe UPDATE TO '2.1.0';
SELECT pg_temp.assert_upgrade();

DROP EXTENSION pg_describe;
CREATE EXTENSION pg_describe SCHEMA upgrade_check VERSION '2.0.0';
ALTER EXTENSION pg_describe UPDATE TO '2.1.0';
SELECT pg_temp.assert_upgrade();

ROLLBACK;

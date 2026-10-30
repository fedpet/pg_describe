-- Bulk discovery includes every routine kind, without executing any of them.
CREATE SCHEMA function_tests;
CREATE FUNCTION function_tests.identity_text(value text) RETURNS text LANGUAGE sql RETURN value;
CREATE FUNCTION function_tests.fallback(a text, b text) RETURNS text LANGUAGE sql RETURN coalesce(a,b);
CREATE FUNCTION function_tests.always_text(a text) RETURNS text LANGUAGE sql RETURN coalesce(a,'fallback');
CREATE FUNCTION function_tests.strict_fallback(a text) RETURNS text LANGUAGE sql STRICT RETURN coalesce(a,'fallback');
CREATE FUNCTION function_tests.string_body(value text) RETURNS text LANGUAGE sql AS 'SELECT upper(value)';
CREATE FUNCTION function_tests.nested(value text) RETURNS text LANGUAGE sql RETURN function_tests.string_body(value);
CREATE FUNCTION function_tests.conditional(value text, flag boolean) RETURNS text LANGUAGE sql RETURN CASE WHEN flag THEN value ELSE 'fallback' END;
CREATE FUNCTION function_tests.defaulted(value text DEFAULT 'fallback') RETURNS text LANGUAGE sql RETURN value;
CREATE FUNCTION function_tests.named_defaults(a text DEFAULT NULL, b text DEFAULT 'fallback') RETURNS text LANGUAGE sql RETURN b;
CREATE TYPE function_tests.return_enum AS ENUM ('value');
CREATE FUNCTION function_tests.null_cast(value text) RETURNS function_tests.return_enum LANGUAGE sql STRICT RETURN NULL::function_tests.return_enum;
CREATE CAST (text AS function_tests.return_enum) WITH FUNCTION function_tests.null_cast(text) AS ASSIGNMENT;
CREATE FUNCTION function_tests.coerced() RETURNS function_tests.return_enum LANGUAGE sql AS 'SELECT ''value''::text';
CREATE FUNCTION function_tests.maybe_empty(value text) RETURNS text LANGUAGE sql RETURN (SELECT value WHERE false);
CREATE FUNCTION function_tests.filtered(value text) RETURNS text LANGUAGE sql AS 'SELECT value WHERE false';
CREATE FUNCTION function_tests.opaque(value text) RETURNS text LANGUAGE plpgsql STRICT AS $$ BEGIN RETURN NULL; END $$;
CREATE FUNCTION function_tests.dangerous() RETURNS integer LANGUAGE plpgsql AS $$ BEGIN RAISE EXCEPTION 'must never execute'; END $$;
SET check_function_bodies = false;
CREATE FUNCTION function_tests.unresolvable() RETURNS integer LANGUAGE sql AS 'SELECT nonexistent_column';
RESET check_function_bodies;
CREATE FUNCTION function_tests.recursive(value text) RETURNS text LANGUAGE sql AS 'SELECT function_tests.recursive(value)';
CREATE FUNCTION function_tests.upper(value text) RETURNS text LANGUAGE sql RETURN NULL::text;
CREATE FUNCTION function_tests.identity_text(value integer) RETURNS integer LANGUAGE sql RETURN value;
CREATE FUNCTION function_tests.set_values() RETURNS SETOF integer LANGUAGE sql RETURN 1;
CREATE FUNCTION function_tests.outputs(a integer, OUT label text, OUT amount integer) RETURNS record LANGUAGE sql AS 'SELECT a::text, a';
CREATE PROCEDURE function_tests.action() LANGUAGE sql AS 'SELECT 1';

SELECT function_name, argument_types::regtype[], kind, returns_set, nonnull_when,
       default_arguments, default_not_null
FROM pg_describe_functions(ARRAY['function_tests'])
ORDER BY function_name, argument_types::regtype[]::text;

SELECT argument_types::regtype[], all_argument_types::regtype[], argument_modes,
       argument_names, volatility
FROM pg_describe_functions(ARRAY['function_tests']) WHERE function_name = 'outputs';

SELECT count(*) = 0 AS empty_selection FROM pg_describe_functions(ARRAY[]::text[]);
SELECT count(*) > 3000 AS includes_builtins FROM pg_describe_functions();
SELECT function_name, argument_types::regtype[], nonnull_when
FROM pg_describe_functions(ARRAY['pg_catalog'])
WHERE function_name IN ('now','upper','uuidv7')
ORDER BY function_name, argument_types::regtype[]::text;

-- The same proofs drive ordinary SQL query description.
SELECT name, result_not_null
FROM pg_describe($q$SELECT
  function_tests.identity_text('hello') AS identity,
  function_tests.string_body('hello') AS string_body,
  function_tests.nested('hello') AS nested,
  function_tests.fallback(NULL,'hello') AS fallback,
  function_tests.always_text(NULL) AS always,
  function_tests.strict_fallback(NULL) AS strict,
  function_tests.defaulted() AS defaulted,
  function_tests.named_defaults(b => NULL) AS named,
  function_tests.coerced() AS coerced,
  function_tests.filtered('hello') AS empty,
  function_tests.opaque('hello') AS opaque,
  function_tests.upper('hello') AS shadow,
  function_tests.recursive('hello') AS recursive,
  1.25::numeric::float8 AS numeric_cast,
  'null'::jsonb::float8 AS json_cast,
  now() AS clock$q$)
WHERE kind = 'column' ORDER BY name;

-- Catalog discovery honours namespace USAGE and function EXECUTE permissions.
CREATE ROLE function_reader;
GRANT USAGE ON SCHEMA function_tests TO function_reader;
REVOKE EXECUTE ON FUNCTION function_tests.dangerous() FROM PUBLIC;
SET ROLE function_reader;
SELECT count(*) = 0 AS hidden_without_execute
FROM pg_describe_functions(ARRAY['function_tests']) WHERE function_name = 'dangerous';
RESET ROLE;
REVOKE USAGE ON SCHEMA function_tests FROM function_reader;
SET ROLE function_reader;
SELECT count(*) = 0 AS hidden_without_usage FROM pg_describe_functions(ARRAY['function_tests']);
RESET ROLE;
DROP OWNED BY function_reader;
DROP ROLE function_reader;
SET client_min_messages = warning;
DROP SCHEMA function_tests CASCADE;
RESET client_min_messages;

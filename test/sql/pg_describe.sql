CREATE EXTENSION pg_describe;

-- Fixtures. Every table has a NOT NULL column, so that any nullability the
-- tests observe comes from the query shape rather than from the schema.
CREATE TABLE a (id int PRIMARY KEY, a_val text NOT NULL);
CREATE TABLE b (id int PRIMARY KEY, a_id int, b_val text NOT NULL);
CREATE TABLE c (id int PRIMARY KEY, b_id int, c_val text NOT NULL);
CREATE TABLE users (id int PRIMARY KEY, email text NOT NULL, note text);

-- ===========================================================================
-- Parameters: inferred, with nothing declared.
-- ===========================================================================

SELECT count(*) = 2 AS ok
FROM pg_describe('SELECT id FROM users WHERE id = $1 AND email LIKE $2')
WHERE kind = 'param';

SELECT array_agg(type_name ORDER BY ord) = ARRAY['integer','text'] AS ok
FROM pg_describe('SELECT id FROM users WHERE id = $1 AND email LIKE $2')
WHERE kind = 'param';

SELECT count(*) = 0 AS ok
FROM pg_describe('SELECT id FROM users') WHERE kind = 'param';

-- ===========================================================================
-- Result columns, names, types, provenance.
-- ===========================================================================

SELECT array_agg(name ORDER BY ord) = ARRAY['id','email']
   AND array_agg(type_name ORDER BY ord) = ARRAY['integer','text'] AS ok
FROM pg_describe('SELECT id, email FROM users') WHERE kind = 'column';

-- An alias renames the output column but not its source.
SELECT name = 'who' AND source_column = 'email' AS ok
FROM pg_describe('SELECT email AS who FROM users') WHERE kind = 'column';

-- ORDER BY over an unselected column adds a resjunk entry, which is in the
-- target list but not in the result. It must not be reported.
SELECT count(*) = 1 AS ok
FROM pg_describe('SELECT email FROM users ORDER BY id') WHERE kind = 'column';

-- An expression has a type but no source, and no opinion on nullability.
SELECT source_table IS NULL AND source_column IS NULL
   AND base_not_null IS NULL AND result_not_null IS NULL
   AND type_name = 'text' AS ok
FROM pg_describe('SELECT upper(email) FROM users') WHERE kind = 'column';

-- ===========================================================================
-- Nullability. This is what pg_describe has that attnotnull alone does not.
-- ===========================================================================

-- Baseline: no join. Both flags agree.
SELECT base_not_null AND result_not_null AS ok
FROM pg_describe('SELECT email FROM users') WHERE kind = 'column';

-- A nullable column is nullable both ways.
SELECT base_not_null = false AND result_not_null = false AS ok
FROM pg_describe('SELECT note FROM users') WHERE kind = 'column';

-- Inner join: nothing can null-extend, so a NOT NULL column stays non-null.
SELECT base_not_null AND result_not_null AS ok
FROM pg_describe('SELECT b.b_val FROM a JOIN b ON b.a_id = a.id')
WHERE kind = 'column';

-- LEFT JOIN, nullable side. THE case. attnotnull still says t; the result
-- column can be NULL anyway, and result_not_null says so.
SELECT base_not_null AND NOT result_not_null AS ok
FROM pg_describe('SELECT b.b_val FROM a LEFT JOIN b ON b.a_id = a.id')
WHERE kind = 'column';

-- LEFT JOIN, preserved side: still non-null.
SELECT base_not_null AND result_not_null AS ok
FROM pg_describe('SELECT a.a_val FROM a LEFT JOIN b ON b.a_id = a.id')
WHERE kind = 'column';

-- RIGHT JOIN nulls the LEFT side.
SELECT array_agg(result_not_null ORDER BY ord) = ARRAY[false, true] AS ok
FROM pg_describe('SELECT a.a_val, b.b_val FROM a RIGHT JOIN b ON b.a_id = a.id')
WHERE kind = 'column';

-- FULL JOIN nulls both sides.
SELECT array_agg(result_not_null ORDER BY ord) = ARRAY[false, false] AS ok
FROM pg_describe('SELECT a.a_val, b.b_val FROM a FULL JOIN b ON b.a_id = a.id')
WHERE kind = 'column';

-- Nesting, case 1: the inner join sits INSIDE the nullable side, so BOTH of
-- its relations are null-extended together when the outer join finds no match.
-- Implementations that only look at the immediate arms of each join get this
-- wrong and report c_val as non-null.
SELECT array_agg(result_not_null ORDER BY ord) = ARRAY[true, false, false] AS ok
FROM pg_describe(
  'SELECT a.a_val, b.b_val, c.c_val
     FROM a LEFT JOIN (b JOIN c ON c.b_id = b.id) ON b.a_id = a.id')
WHERE kind = 'column';

-- Nesting, case 2: same three tables, different shape. The outer join happens
-- first and c is inner-joined onto the result, so only b is nullable.
SELECT array_agg(result_not_null ORDER BY ord) = ARRAY[true, false, true] AS ok
FROM pg_describe(
  'SELECT a.a_val, b.b_val, c.c_val
     FROM (a LEFT JOIN b ON b.a_id = a.id) JOIN c ON c.b_id = b.id')
WHERE kind = 'column';

-- Nesting, case 3: an outer join inside an outer join. Everything downstream
-- of the outermost nullable side is nullable.
SELECT array_agg(result_not_null ORDER BY ord) = ARRAY[true, false, false] AS ok
FROM pg_describe(
  'SELECT a.a_val, b.b_val, c.c_val
     FROM a LEFT JOIN (b LEFT JOIN c ON c.b_id = b.id) ON b.a_id = a.id')
WHERE kind = 'column';

-- GROUP BY ROLLUP emits super-aggregate rows in which the grouping column is
-- NULL, however NOT NULL the underlying column is. Same class of false
-- positive, different cause.
SELECT base_not_null AND NOT result_not_null AS ok
FROM pg_describe('SELECT email, count(*) FROM users GROUP BY ROLLUP(email)')
WHERE kind = 'column' AND ord = 1;

-- Plain GROUP BY does not null-extend anything.
SELECT base_not_null AND result_not_null AS ok
FROM pg_describe('SELECT email, count(*) FROM users GROUP BY email')
WHERE kind = 'column' AND ord = 1;

-- The claim is not merely internally consistent -- it matches what the server
-- actually does. Expected to be f, which is exactly what result_not_null said
-- and what base_not_null did not.
INSERT INTO a VALUES (1, 'only-a');
SELECT bool_or(b.b_val IS NOT NULL) AS ok
FROM a LEFT JOIN b ON b.a_id = a.id;
DELETE FROM a;

-- ===========================================================================
-- Statement shapes.
-- ===========================================================================

SELECT array_agg(name ORDER BY ord) = ARRAY['id','email'] AS ok
FROM pg_describe('INSERT INTO users (id, email) VALUES ($1, $2) RETURNING id, email')
WHERE kind = 'column';

SELECT array_agg(type_name ORDER BY ord) = ARRAY['integer','text'] AS ok
FROM pg_describe('INSERT INTO users (id, email) VALUES ($1, $2) RETURNING id, email')
WHERE kind = 'param';

SELECT name = 'note' AND source_table = 'users'::regclass AS ok
FROM pg_describe('UPDATE users SET note = $1 WHERE id = $2 RETURNING note')
WHERE kind = 'column';

-- No RETURNING: parameters, but no result columns.
SELECT count(*) = 0 AS ok
FROM pg_describe('DELETE FROM users WHERE id = $1') WHERE kind = 'column';

-- Utility statements have no result columns, and nothing is created.
SELECT count(*) = 0 AS ok FROM pg_describe('CREATE TABLE nope (x int)');
SELECT to_regclass('nope') IS NULL AS ok;

-- STRICT: NULL in, no rows out, C function never entered.
SELECT count(*) = 0 AS ok FROM pg_describe(NULL);

-- ===========================================================================
-- Errors. Note the QUERY: line -- the caret is drawn against the inner query.
-- ===========================================================================

SELECT * FROM pg_describe('SELECT 1; SELECT 2');
SELECT * FROM pg_describe('SELECT FROM WHERE');
SELECT * FROM pg_describe('SELECT * FROM no_such_table');

-- ===========================================================================
-- Permissions. Parse analysis records what needs checking and leaves it to the
-- executor, which pg_describe never reaches -- so it checks for itself.
-- ===========================================================================

CREATE ROLE pd_nobody LOGIN;
GRANT USAGE ON SCHEMA public TO pd_nobody;
GRANT EXECUTE ON FUNCTION pg_describe(text) TO pd_nobody;

SET ROLE pd_nobody;

-- No grant at all: refused, with the executor's own message.
SELECT * FROM pg_describe('SELECT email FROM users');

-- A query touching no table needs no table rights.
SELECT count(*) = 1 AS ok
FROM pg_describe('SELECT $1::int + 1') WHERE kind = 'column';

RESET ROLE;

-- A column-level grant is enough for a query that stays inside it.
GRANT SELECT (email) ON users TO pd_nobody;
SET ROLE pd_nobody;

SELECT source_column = 'email' AS ok
FROM pg_describe('SELECT email FROM users') WHERE kind = 'column';

-- ...but not for a column outside it.
SELECT * FROM pg_describe('SELECT note FROM users');

-- SELECT * needs the whole row, which no column grant satisfies.
SELECT * FROM pg_describe('SELECT * FROM users');

RESET ROLE;

REVOKE ALL ON users FROM pd_nobody;
REVOKE ALL ON SCHEMA public FROM pd_nobody;
REVOKE ALL ON FUNCTION pg_describe(text) FROM pd_nobody;
DROP ROLE pd_nobody;

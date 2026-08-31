-- Every query in this file is valid SQL. Paste any of them into psql, supply
-- the parameters, and they run. That is the point: pg_describe reads real SQL,
-- so there is no dialect to learn and no rewriting step between what you test
-- and what you ship.

-- @name ListRecentOrders
-- LEFT JOIN, because customer_id is nullable for guest checkout. This is the
-- query that separates pg_describe from attnotnull-only generators: email is
-- declared NOT NULL on customers, but it comes back NULL for a guest order.
SELECT o.id,
       o.placed_at,
       o.total,
       o.note,
       c.email,
       c.vip
FROM orders o
LEFT JOIN customers c ON c.id = o.customer_id
WHERE o.placed_at >= $1
ORDER BY o.placed_at DESC;

-- @name FindCustomerByEmail
-- Nothing here declares a type. $1 is inferred as text from the comparison.
SELECT id, email, vip
FROM customers
WHERE email = $1;

-- @name CreateOrder
-- RETURNING has a target list of its own, and that is what gets described.
INSERT INTO orders (customer_id, total, note)
VALUES ($1, $2, $3)
RETURNING id, placed_at, total;

-- @name DeleteOrdersBefore
-- No RETURNING, so no result columns. The generated function returns the
-- number of rows affected instead of a row array.
DELETE FROM orders
WHERE placed_at < $1;

-- @name DailyOrderTotals
-- Three expression columns, three different answers. None of them has a source
-- column, so none has provenance — but nullability does not follow provenance:
-- date_trunc is strict over a NOT NULL column and count() never returns NULL,
-- while sum() over an empty group really is NULL and stays nullable.
SELECT date_trunc('day', o.placed_at) AS day,
       count(*)                       AS order_count,
       sum(o.total)                   AS revenue
FROM orders o
GROUP BY 1
ORDER BY 1 DESC;

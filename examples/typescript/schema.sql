-- Demo schema for the pg_describe TypeScript example.
--
-- Two details here exist to make the generated types interesting:
--
--   orders.customer_id is NULLABLE, because guest checkout is a thing. That
--   makes LEFT JOIN the honest way to list orders with their customer, which
--   is exactly where attnotnull-only type generators produce wrong types.
--
--   orders.total is numeric, not float. Money in a float is a bug, and numeric
--   is why the generated field is `string` rather than `number`.

DROP TABLE IF EXISTS orders;
DROP TABLE IF EXISTS customers;

CREATE TABLE customers (
    id    serial PRIMARY KEY,
    email text NOT NULL,
    vip   boolean NOT NULL DEFAULT false
);

CREATE TABLE orders (
    id          bigserial PRIMARY KEY,
    customer_id int REFERENCES customers,      -- NULL for guest checkout
    placed_at   timestamptz NOT NULL DEFAULT now(),
    total       numeric(10,2) NOT NULL,
    note        text
);

INSERT INTO customers (email, vip) VALUES
    ('ada@example.com', true),
    ('grace@example.com', false);

INSERT INTO orders (customer_id, total, note) VALUES
    (1, 99.95, 'gift wrap'),
    (2, 12.00, NULL),
    (NULL, 42.50, 'guest checkout');

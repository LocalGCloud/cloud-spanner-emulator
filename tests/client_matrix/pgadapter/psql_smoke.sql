\set ON_ERROR_STOP off
select version();
CREATE TABLE psql_t (id bigint NOT NULL PRIMARY KEY, name varchar, amount numeric, data jsonb);
INSERT INTO psql_t VALUES (1, 'a', 1.5, '{"x":1}'), (2, 'b', 2.5, '{"y":2}');
PREPARE q(bigint) AS SELECT id, name, amount, data FROM psql_t WHERE id = $1;
EXECUTE q(2);
BEGIN;
UPDATE psql_t SET name = 'a2' WHERE id = 1;
COMMIT;
BEGIN;
INSERT INTO psql_t VALUES (9, 'z', 0, null);
ROLLBACK;
SELECT id, name FROM psql_t ORDER BY id;
INSERT INTO psql_t VALUES (1, 'dup', 0, null);
\d psql_t
\dt

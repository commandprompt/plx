/* plx 2.0.0 -> 3.0.0
 *
 * 3.0.0 makes the extension non-relocatable (relocatable = false in
 * plx.control). Generated code now names the extension's own objects
 * (plx_strbuild, plx_sb_append) qualified with the schema plx is installed in,
 * so a function with its own SET search_path still finds them and an object of
 * the same name earlier on the caller's path cannot stand in for them. Moving
 * the extension with ALTER EXTENSION ... SET SCHEMA would leave those stored
 * names pointing at the old schema, so it is no longer allowed.
 *
 * PostgreSQL records relocatability in pg_extension when the extension is
 * created or updated, which is why this otherwise empty update exists: running
 * it is what records the new setting for an existing installation.
 *
 * Functions already in the catalog keep the unqualified names they were
 * generated with; they pick up the qualified names the next time their DDL is
 * run. See the 3.0.0 entry in CHANGELOG.md. */

\echo Use "ALTER EXTENSION plx UPDATE TO '3.0.0'" to load this file. \quit

/*
 * plx_strbuild gets binary I/O and becomes collatable, as in plx--3.0.0.sql.
 * Without a collation, ORDER BY, DISTINCT and GROUP BY on it failed, and
 * without SEND and RECEIVE so did binary COPY.
 */
CREATE FUNCTION plx_sb_recv(internal) RETURNS plx_strbuild
	AS '$libdir/plx', 'plx_sb_recv' LANGUAGE C IMMUTABLE STRICT;
CREATE FUNCTION plx_sb_send(plx_strbuild) RETURNS bytea
	AS '$libdir/plx', 'plx_sb_send' LANGUAGE C IMMUTABLE STRICT;

ALTER TYPE plx_strbuild SET (RECEIVE = plx_sb_recv, SEND = plx_sb_send);

/*
 * ALTER TYPE cannot make a type collatable, so set the collation in the
 * catalog, the way CREATE TYPE ... COLLATABLE = true records it. A fresh
 * install gives the default collation to plx_strbuild and to every type
 * built on it: its array type, a domain over any of these (also a domain
 * over a domain), and the array type of any of these. Each statement
 * below lists that whole set in plx_sb_types, by following typbasetype and
 * typarray until nothing new turns up. Existing table and composite-type
 * columns of these types get the collation too, and so do range types over
 * them, index keys and partition keys. A view created before the update
 * keeps no collation in its stored query until it is re-created.
 */
WITH RECURSIVE plx_sb_types(oid) AS (
	SELECT 'plx_strbuild'::pg_catalog.regtype::pg_catalog.oid
	UNION
	SELECT t.oid
	FROM plx_sb_types s JOIN pg_catalog.pg_type b ON b.oid = s.oid
	JOIN pg_catalog.pg_type t
	  ON t.oid = b.typarray OR (t.typtype = 'd' AND t.typbasetype = b.oid)
)
UPDATE pg_catalog.pg_type SET typcollation = 100
WHERE typcollation = 0 AND oid IN (SELECT oid FROM plx_sb_types);

WITH RECURSIVE plx_sb_types(oid) AS (
	SELECT 'plx_strbuild'::pg_catalog.regtype::pg_catalog.oid
	UNION
	SELECT t.oid
	FROM plx_sb_types s JOIN pg_catalog.pg_type b ON b.oid = s.oid
	JOIN pg_catalog.pg_type t
	  ON t.oid = b.typarray OR (t.typtype = 'd' AND t.typbasetype = b.oid)
)
UPDATE pg_catalog.pg_attribute SET attcollation = 100
WHERE attcollation = 0 AND atttypid IN (SELECT oid FROM plx_sb_types)
  AND attrelid NOT IN (SELECT indexrelid FROM pg_catalog.pg_index);

WITH RECURSIVE plx_sb_types(oid) AS (
	SELECT 'plx_strbuild'::pg_catalog.regtype::pg_catalog.oid
	UNION
	SELECT t.oid
	FROM plx_sb_types s JOIN pg_catalog.pg_type b ON b.oid = s.oid
	JOIN pg_catalog.pg_type t
	  ON t.oid = b.typarray OR (t.typtype = 'd' AND t.typbasetype = b.oid)
)
UPDATE pg_catalog.pg_range SET rngcollation = 100
WHERE rngcollation = 0 AND rngsubtype IN (SELECT oid FROM plx_sb_types);

/*
 * Index keys and partition keys keep one collation per key in
 * pg_index.indcollation and pg_partitioned_table.partcollation. Without it,
 * a hash index or a hash partition cannot hash these values, and a list or
 * range partition cannot compare its bounds. Give the default collation to
 * every key whose type is in plx_sb_types. A key on a column takes the
 * column's type. A key on an expression takes the type the expression
 * gives: the query below reads it from a subquery that returns no rows, so
 * the expression is never run. The column of an index key gets the same
 * collation as the key, and an INCLUDE column keeps none, as on a fresh
 * install. Rewriting the pg_class row of a partitioned table makes every
 * session read its new partition key.
 */
DO $plx_keys$
DECLARE
	sb pg_catalog.oid[];
	r record;
	colls pg_catalog.oid[];
	k int;
	e int;
	typ pg_catalog.oid;
	changed boolean;
	probe pg_catalog.oid;
BEGIN
	WITH RECURSIVE plx_sb_types(oid) AS (
		SELECT 'plx_strbuild'::pg_catalog.regtype::pg_catalog.oid
		UNION
		SELECT t.oid
		FROM plx_sb_types s JOIN pg_catalog.pg_type b ON b.oid = s.oid
		JOIN pg_catalog.pg_type t
		  ON t.oid = b.typarray OR (t.typtype = 'd' AND t.typbasetype = b.oid)
	)
	SELECT pg_catalog.array_agg(oid) INTO sb FROM plx_sb_types;

	FOR r IN
		SELECT 'i' AS kind, i.indexrelid AS rel, i.indrelid AS tbl,
		       i.indnkeyatts AS n, i.indkey::pg_catalog.int2[] AS keys,
		       i.indcollation::pg_catalog.oid[] AS colls,
		       pg_catalog.pg_get_expr(i.indexprs, i.indrelid) AS exprs
		FROM pg_catalog.pg_index i
		JOIN pg_catalog.pg_class c ON c.oid = i.indrelid
		-- another session's temporary table cannot be read, and goes away
		-- with that session
		WHERE NOT pg_catalog.pg_is_other_temp_schema(c.relnamespace)
		UNION ALL
		SELECT 'p', p.partrelid, p.partrelid,
		       p.partnatts, p.partattrs::pg_catalog.int2[],
		       p.partcollation::pg_catalog.oid[],
		       pg_catalog.pg_get_expr(p.partexprs, p.partrelid)
		FROM pg_catalog.pg_partitioned_table p
		JOIN pg_catalog.pg_class c ON c.oid = p.partrelid
		WHERE NOT pg_catalog.pg_is_other_temp_schema(c.relnamespace)
	LOOP
		colls := r.colls;
		changed := false;
		probe := NULL;
		e := 0;
		FOR k IN 0 .. r.n - 1 LOOP
			IF r.keys[k] = 0 THEN
				e := e + 1;
			END IF;
			CONTINUE WHEN colls[k] <> 0;
			IF r.keys[k] <> 0 THEN
				SELECT a.atttypid INTO typ FROM pg_catalog.pg_attribute a
				WHERE a.attrelid = r.tbl AND a.attnum = r.keys[k];
			ELSE
				-- The type of an expression key comes from a temporary view
				-- over the key expressions. CREATE VIEW parses them but
				-- neither plans nor runs them. A query would be planned, and
				-- the planner runs an IMMUTABLE function with constant
				-- arguments, so a table owner's function would run as the
				-- superuser who runs this update.
				IF probe IS NULL THEN
					EXECUTE pg_catalog.format(
						'CREATE TEMPORARY VIEW plx_key_probe (%s) AS SELECT %s FROM %s',
						(SELECT pg_catalog.string_agg('e' || j, ', ')
						 FROM pg_catalog.generate_series(1,
						      (SELECT pg_catalog.count(*)::int
						       FROM pg_catalog.unnest(r.keys) u
						       WHERE u = 0)) j),
						r.exprs, r.tbl::pg_catalog.regclass);
					probe := 'pg_temp.plx_key_probe'::pg_catalog.regclass;
				END IF;
				SELECT a.atttypid INTO typ FROM pg_catalog.pg_attribute a
				WHERE a.attrelid = probe AND a.attnum = e;
			END IF;
			IF typ = ANY (sb) THEN
				colls[k] := 100;
				changed := true;
			END IF;
		END LOOP;
		IF probe IS NOT NULL THEN
			DROP VIEW pg_temp.plx_key_probe;
		END IF;
		CONTINUE WHEN NOT changed;
		IF r.kind = 'i' THEN
			UPDATE pg_catalog.pg_index
			SET indcollation = pg_catalog.array_to_string(colls, ' ')::pg_catalog.oidvector
			WHERE indexrelid = r.rel;
			UPDATE pg_catalog.pg_attribute a SET attcollation = colls[a.attnum - 1]
			WHERE a.attrelid = r.rel AND a.attnum BETWEEN 1 AND r.n;
		ELSE
			UPDATE pg_catalog.pg_partitioned_table
			SET partcollation = pg_catalog.array_to_string(colls, ' ')::pg_catalog.oidvector
			WHERE partrelid = r.rel;
			UPDATE pg_catalog.pg_class SET relname = relname WHERE oid = r.rel;
		END IF;
	END LOOP;
END
$plx_keys$;

/*
 * The append operators of every dialect except plxcobol now call a STRICT
 * declaration of plx_sb_append, so a NULL operand gives NULL, as || does.
 */
CREATE FUNCTION plx_sb_append_strict(plx_strbuild, text) RETURNS plx_strbuild
	AS '$libdir/plx', 'plx_sb_append' LANGUAGE C IMMUTABLE STRICT
	SUPPORT plx_sb_append_support;

/*
 * plxtsql converts a value whose type it cannot see to an integer type with
 * one of these: a numeric, real or double precision value is truncated toward
 * zero, as SQL Server does, and any other value takes the explicit cast.
 * plxtsql passes the value as ROW(value), so the argument is always of
 * type record and matches this signature exactly: an overload that a role
 * with CREATE on this schema adds cannot capture the call.
 */
CREATE FUNCTION plx_tsql_int2(record) RETURNS smallint
	AS '$libdir/plx', 'plx_tsql_toint' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;
CREATE FUNCTION plx_tsql_int4(record) RETURNS integer
	AS '$libdir/plx', 'plx_tsql_toint' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;
CREATE FUNCTION plx_tsql_int8(record) RETURNS bigint
	AS '$libdir/plx', 'plx_tsql_toint' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

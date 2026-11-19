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
 * catalog, the way CREATE TYPE ... COLLATABLE = true records it: the default
 * collation on the type, its array type, and domains over either. Existing
 * table and composite-type columns of these types get it too. A view
 * created before the update keeps no collation in its stored query until
 * it is re-created.
 */
UPDATE pg_catalog.pg_type SET typcollation = 100
WHERE typcollation = 0
  AND (oid IN ('plx_strbuild'::pg_catalog.regtype,
               'plx_strbuild[]'::pg_catalog.regtype)
       OR (typtype = 'd'
           AND typbasetype IN ('plx_strbuild'::pg_catalog.regtype,
                               'plx_strbuild[]'::pg_catalog.regtype)));

UPDATE pg_catalog.pg_attribute a SET attcollation = 100
FROM pg_catalog.pg_type t
WHERE a.atttypid = t.oid AND a.attcollation = 0 AND t.typcollation = 100
  AND (t.oid IN ('plx_strbuild'::pg_catalog.regtype,
                 'plx_strbuild[]'::pg_catalog.regtype)
       OR (t.typtype = 'd'
           AND t.typbasetype IN ('plx_strbuild'::pg_catalog.regtype,
                                 'plx_strbuild[]'::pg_catalog.regtype)));

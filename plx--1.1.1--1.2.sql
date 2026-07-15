/* plx 1.1.1 -> 1.2: add the plxplsql (Oracle PL/SQL) dialect */

/* PL/SQL dialect (Oracle) validator + inline handler live in plx.so */
CREATE FUNCTION plx_plsql_validator(oid)
	RETURNS void
	AS '$libdir/plx', 'plx_plsql_validator'
	LANGUAGE C STRICT;

CREATE FUNCTION plx_plsql_inline_handler(internal)
	RETURNS void
	AS '$libdir/plx', 'plx_plsql_inline_handler'
	LANGUAGE C;

CREATE TRUSTED LANGUAGE plxplsql
	HANDLER plx_call_handler
	INLINE plx_plsql_inline_handler
	VALIDATOR plx_plsql_validator;

COMMENT ON LANGUAGE plxplsql IS 'plx Oracle PL/SQL dialect (transpiles to plpgsql)';

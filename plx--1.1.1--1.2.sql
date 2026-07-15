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

/* TypeScript dialect validator + inline handler live in plx.so */
CREATE FUNCTION plx_ts_validator(oid)
	RETURNS void
	AS '$libdir/plx', 'plx_ts_validator'
	LANGUAGE C STRICT;

CREATE FUNCTION plx_ts_inline_handler(internal)
	RETURNS void
	AS '$libdir/plx', 'plx_ts_inline_handler'
	LANGUAGE C;

CREATE TRUSTED LANGUAGE plxts
	HANDLER plx_call_handler
	INLINE plx_ts_inline_handler
	VALIDATOR plx_ts_validator;

COMMENT ON LANGUAGE plxts IS 'plx TypeScript dialect (transpiles to plpgsql)';

/* T-SQL dialect (Transact-SQL) validator + inline handler live in plx.so */
CREATE FUNCTION plx_tsql_validator(oid)
	RETURNS void
	AS '$libdir/plx', 'plx_tsql_validator'
	LANGUAGE C STRICT;

CREATE FUNCTION plx_tsql_inline_handler(internal)
	RETURNS void
	AS '$libdir/plx', 'plx_tsql_inline_handler'
	LANGUAGE C;

CREATE TRUSTED LANGUAGE plxtsql
	HANDLER plx_call_handler
	INLINE plx_tsql_inline_handler
	VALIDATOR plx_tsql_validator;

COMMENT ON LANGUAGE plxtsql IS 'plx Transact-SQL (SQL Server) dialect (transpiles to plpgsql)';

/* Go dialect validator + inline handler live in plx.so */
CREATE FUNCTION plx_go_validator(oid)
	RETURNS void
	AS '$libdir/plx', 'plx_go_validator'
	LANGUAGE C STRICT;

CREATE FUNCTION plx_go_inline_handler(internal)
	RETURNS void
	AS '$libdir/plx', 'plx_go_inline_handler'
	LANGUAGE C;

CREATE TRUSTED LANGUAGE plxgo
	HANDLER plx_call_handler
	INLINE plx_go_inline_handler
	VALIDATOR plx_go_validator;

COMMENT ON LANGUAGE plxgo IS 'plx Go dialect (transpiles to plpgsql)';

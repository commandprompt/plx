/* plx 1.0: registers the plxruby and plxphp languages (bodies transpile to plpgsql) */

\echo Use "CREATE EXTENSION plx" to load this file. \quit

/*
 * Runtime call handler = plpgsql's OWN handler. Binding a fresh pg_proc row to
 * plpgsql.so's exported symbol is legal (CreateProceduralLanguage only checks
 * the handler RETURNS language_handler). Execution is therefore 100% stock
 * plpgsql; plx contributes zero call-path C.
 */
CREATE FUNCTION plx_call_handler()
	RETURNS language_handler
	AS '$libdir/plpgsql', 'plpgsql_call_handler'
	LANGUAGE C;

/* Ruby dialect validator + inline handler live in plx.so */
CREATE FUNCTION plx_ruby_validator(oid)
	RETURNS void
	AS '$libdir/plx', 'plx_ruby_validator'
	LANGUAGE C STRICT;

CREATE FUNCTION plx_ruby_inline_handler(internal)
	RETURNS void
	AS '$libdir/plx', 'plx_ruby_inline_handler'
	LANGUAGE C;

CREATE TRUSTED LANGUAGE plxruby
	HANDLER plx_call_handler
	INLINE plx_ruby_inline_handler
	VALIDATOR plx_ruby_validator;

COMMENT ON LANGUAGE plxruby IS 'plx Ruby dialect (transpiles to plpgsql)';

/* PHP dialect */
CREATE FUNCTION plx_php_validator(oid)
	RETURNS void
	AS '$libdir/plx', 'plx_php_validator'
	LANGUAGE C STRICT;

CREATE FUNCTION plx_php_inline_handler(internal)
	RETURNS void
	AS '$libdir/plx', 'plx_php_inline_handler'
	LANGUAGE C;

CREATE TRUSTED LANGUAGE plxphp
	HANDLER plx_call_handler
	INLINE plx_php_inline_handler
	VALIDATOR plx_php_validator;

COMMENT ON LANGUAGE plxphp IS 'plx PHP dialect (transpiles to plpgsql)';

/* JavaScript dialect */
CREATE FUNCTION plx_js_validator(oid)
	RETURNS void
	AS '$libdir/plx', 'plx_js_validator'
	LANGUAGE C STRICT;

CREATE FUNCTION plx_js_inline_handler(internal)
	RETURNS void
	AS '$libdir/plx', 'plx_js_inline_handler'
	LANGUAGE C;

CREATE TRUSTED LANGUAGE plxjs
	HANDLER plx_call_handler
	INLINE plx_js_inline_handler
	VALIDATOR plx_js_validator;

COMMENT ON LANGUAGE plxjs IS 'plx JavaScript dialect (transpiles to plpgsql)';

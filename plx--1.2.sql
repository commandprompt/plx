/* plx 1.0: registers the plx dialect languages and the string-builder type */

\echo Use "CREATE EXTENSION plx" to load this file. \quit

/*
 * plx_strbuild: an expanded-object string builder. Its flattened form is a
 * text-compatible varlena, so it casts to and from text without a function.
 * plx_sb_append grows the buffer in place across a loop (amortized O(1)),
 * avoiding the quadratic cost of `s := s || 'x'` on plain text.
 */
CREATE TYPE plx_strbuild;

CREATE FUNCTION plx_sb_in(cstring) RETURNS plx_strbuild
	AS '$libdir/plx', 'plx_sb_in' LANGUAGE C IMMUTABLE STRICT;
CREATE FUNCTION plx_sb_out(plx_strbuild) RETURNS cstring
	AS '$libdir/plx', 'plx_sb_out' LANGUAGE C IMMUTABLE STRICT;

CREATE TYPE plx_strbuild (
	INPUT = plx_sb_in,
	OUTPUT = plx_sb_out,
	LIKE = text,
	STORAGE = extended
);

CREATE FUNCTION plx_sb_append_support(internal) RETURNS internal
	AS '$libdir/plx', 'plx_sb_append_support' LANGUAGE C;

CREATE FUNCTION plx_sb_append(plx_strbuild, text) RETURNS plx_strbuild
	AS '$libdir/plx', 'plx_sb_append' LANGUAGE C IMMUTABLE
	SUPPORT plx_sb_append_support;

-- plx_strbuild is implicitly usable as text (so it works in RETURN, string
-- functions, and concatenation). text becomes a builder only in assignment
-- context, which avoids operator ambiguity such as `builder || text`.
CREATE CAST (plx_strbuild AS text) WITHOUT FUNCTION AS IMPLICIT;
CREATE CAST (text AS plx_strbuild) WITHOUT FUNCTION AS ASSIGNMENT;

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

/* Python dialect */
CREATE FUNCTION plx_py_validator(oid)
	RETURNS void
	AS '$libdir/plx', 'plx_py_validator'
	LANGUAGE C STRICT;

CREATE FUNCTION plx_py_inline_handler(internal)
	RETURNS void
	AS '$libdir/plx', 'plx_py_inline_handler'
	LANGUAGE C;

CREATE TRUSTED LANGUAGE plxpython3
	HANDLER plx_call_handler
	INLINE plx_py_inline_handler
	VALIDATOR plx_py_validator;

COMMENT ON LANGUAGE plxpython3 IS 'plx Python dialect (transpiles to plpgsql)';

/* COBOL dialect (ISO/IEC 1989:2023) validator + inline handler live in plx.so */
CREATE FUNCTION plx_cob_validator(oid)
	RETURNS void
	AS '$libdir/plx', 'plx_cob_validator'
	LANGUAGE C STRICT;

CREATE FUNCTION plx_cob_inline_handler(internal)
	RETURNS void
	AS '$libdir/plx', 'plx_cob_inline_handler'
	LANGUAGE C;

CREATE TRUSTED LANGUAGE plxcobol
	HANDLER plx_call_handler
	INLINE plx_cob_inline_handler
	VALIDATOR plx_cob_validator;

COMMENT ON LANGUAGE plxcobol IS 'plx COBOL dialect (ISO/IEC 1989:2023, transpiles to plpgsql)';

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

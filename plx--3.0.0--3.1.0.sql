/* plx 3.0.0 -> 3.1.0: adds the plxcsharp and plxlua languages. Nothing
 * that exists in 3.0.0 changes. */

\echo Use "ALTER EXTENSION plx UPDATE TO '3.1.0'" to load this file. \quit

/* C# dialect validator + inline handler live in plx.so */
CREATE FUNCTION plx_csharp_validator(oid)
	RETURNS void
	AS '$libdir/plx', 'plx_csharp_validator'
	LANGUAGE C STRICT;

CREATE FUNCTION plx_csharp_inline_handler(internal)
	RETURNS void
	AS '$libdir/plx', 'plx_csharp_inline_handler'
	LANGUAGE C;

CREATE TRUSTED LANGUAGE plxcsharp
	HANDLER plx_call_handler
	INLINE plx_csharp_inline_handler
	VALIDATOR plx_csharp_validator;

COMMENT ON LANGUAGE plxcsharp IS 'plx C# dialect (transpiles to plpgsql)';

/* Lua dialect validator + inline handler live in plx.so */
CREATE FUNCTION plx_lua_validator(oid)
	RETURNS void
	AS '$libdir/plx', 'plx_lua_validator'
	LANGUAGE C STRICT;

CREATE FUNCTION plx_lua_inline_handler(internal)
	RETURNS void
	AS '$libdir/plx', 'plx_lua_inline_handler'
	LANGUAGE C;

CREATE TRUSTED LANGUAGE plxlua
	HANDLER plx_call_handler
	INLINE plx_lua_inline_handler
	VALIDATOR plx_lua_validator;

COMMENT ON LANGUAGE plxlua IS 'plx Lua dialect (transpiles to plpgsql)';

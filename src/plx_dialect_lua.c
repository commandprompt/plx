/*
 * plx_dialect_lua.c - the "plxlua" dialect (Lua).
 *
 * SCAFFOLD: registration only. The front end is not implemented yet; every
 * body is rejected with a clear error until it is.
 */
#include "postgres.h"

#include "fmgr.h"
#include "utils/memutils.h"

#include "plx.h"
#include "plx_int.h"
#include "plx_engine.h"

PG_FUNCTION_INFO_V1(plx_lua_validator);
PG_FUNCTION_INFO_V1(plx_lua_inline_handler);

static void
lua_parse_body(Ctx *cx)
{
	ereport(ERROR,
			(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			 errmsg("plxlua: this dialect is not implemented yet")));
}

static const PlxSurface lua_surface = {
	.lanname = "plxlua",
	.block_style = PLX_BLK_KEYWORD_END,
	.flags = PLX_TRUSTED,
	.parse_body = lua_parse_body,
};

static char *
lua_transpile(const char *src, const PlxFuncMeta *meta)
{
	return plx_transpile(src, meta, &lua_surface, CurrentMemoryContext);
}

const PlxDialect plx_lua_dialect = {
	.abi_version = PLX_ABI_VERSION,
	.lanname = "plxlua",
	.transpile = lua_transpile,
	.flags = PLX_TRUSTED,
};

Datum
plx_lua_validator(PG_FUNCTION_ARGS)
{
	return plx_generic_validator(fcinfo, &plx_lua_dialect);
}

Datum
plx_lua_inline_handler(PG_FUNCTION_ARGS)
{
	return plx_generic_inline_handler(fcinfo, &plx_lua_dialect);
}

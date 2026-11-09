/*
 * plx_dialect_csharp.c - the "plxcsharp" dialect (C#).
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

PG_FUNCTION_INFO_V1(plx_csharp_validator);
PG_FUNCTION_INFO_V1(plx_csharp_inline_handler);

static void
csharp_parse_body(Ctx *cx)
{
	ereport(ERROR,
			(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			 errmsg("plxcsharp: this dialect is not implemented yet")));
}

static const PlxSurface csharp_surface = {
	.lanname = "plxcsharp",
	.block_style = PLX_BLK_BRACE,
	.flags = PLX_TRUSTED,
	.parse_body = csharp_parse_body,
};

static char *
csharp_transpile(const char *src, const PlxFuncMeta *meta)
{
	return plx_transpile(src, meta, &csharp_surface, CurrentMemoryContext);
}

const PlxDialect plx_csharp_dialect = {
	.abi_version = PLX_ABI_VERSION,
	.lanname = "plxcsharp",
	.transpile = csharp_transpile,
	.flags = PLX_TRUSTED,
};

Datum
plx_csharp_validator(PG_FUNCTION_ARGS)
{
	return plx_generic_validator(fcinfo, &plx_csharp_dialect);
}

Datum
plx_csharp_inline_handler(PG_FUNCTION_ARGS)
{
	return plx_generic_inline_handler(fcinfo, &plx_csharp_dialect);
}

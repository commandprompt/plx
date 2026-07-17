/*
 * plx_dialect_ts.c - the "plxts" dialect (TypeScript).
 *
 * TypeScript is the plxjs (JavaScript) surface plus type annotations: a
 * declaration "let x: T = e" carries the type on the variable. The TS
 * preprocessor (ts_types, in plx_parse_brace.c) rewrites those "id: T"
 * annotations into the JS leading-colon-colon block-comment form and maps the
 * TypeScript type to a SQL type, then the shared brace parser does the rest.
 * Everything else is plxjs.
 */
#include "postgres.h"

#include "fmgr.h"
#include "utils/memutils.h"

#include "plx.h"
#include "plx_int.h"
#include "plx_engine.h"

PG_FUNCTION_INFO_V1(plx_ts_validator);
PG_FUNCTION_INFO_V1(plx_ts_inline_handler);

static const PlxKwSpell ts_kws[] = {
	{"if", KW_IF}, {"else", KW_ELSE},
	{"while", KW_WHILE}, {"for", KW_FOR}, {"of", KW_OF},
	{"return", KW_RETURN}, {"break", KW_BREAK}, {"continue", KW_NEXT},
	{"try", KW_BEGIN}, {"catch", KW_RESCUE}, {"finally", KW_ENSURE},
	{"throw", KW_RAISE}, {"function", KW_DEF}, {"switch", KW_CASE},
	{"case", KW_WHEN}, {"default", KW_ELSE},
	{"let", KW_LET}, {"const", KW_LET}, {"var", KW_LET},
	{"true", KW_TRUE}, {"false", KW_FALSE}, {"null", KW_NIL},
	{"emit", KW_EMIT}, {"return_next", KW_RETURN_NEXT},
};

static const PlxSurface ts_surface = {
	.lanname = "plxts",
	.block_style = PLX_BLK_BRACE,
	.stmt_semicolon = true,
	.var_sigil = 0,
	.cmt_hash = false,
	.cmt_slash = true,
	.cmt_block = true,
	.type_ann = NULL,
	.interp_quote = '`',
	.interp_hashbrace = false,
	.interp_dollar = false,
	.interp_dollarbrace = true,
	.concat_op = 0,
	.ts_types = true,			/* rewrite "id: T" type annotations */
	.kws = ts_kws,
	.nkws = lengthof(ts_kws),
	.excs = NULL,
	.nexcs = 0,
	.flags = PLX_TRUSTED,
	.parse_body = plx_brace_parse_body,
};

static char *
ts_transpile(const char *src, const PlxFuncMeta *meta)
{
	return plx_transpile(src, meta, &ts_surface, CurrentMemoryContext);
}

const PlxDialect plx_ts_dialect = {
	.abi_version = PLX_ABI_VERSION,
	.lanname = "plxts",
	.transpile = ts_transpile,
	.flags = PLX_TRUSTED,
};

Datum
plx_ts_validator(PG_FUNCTION_ARGS)
{
	return plx_generic_validator(fcinfo, &plx_ts_dialect);
}

Datum
plx_ts_inline_handler(PG_FUNCTION_ARGS)
{
	return plx_generic_inline_handler(fcinfo, &plx_ts_dialect);
}

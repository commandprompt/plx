/*
 * plx_dialect_js.c - the "plxjs" dialect (latest ECMAScript).
 *
 * JavaScript surface: brace-delimited blocks, no variable sigil, let/const/var
 * declarations, template-literal interpolation with backticks and ${expr},
 * // and block comments, for-of over query(), C-style for, try/catch/finally,
 * throw. The shared transpiler in plx_transpile.c does the lowering to plpgsql.
 */
#include "postgres.h"

#include "fmgr.h"
#include "utils/memutils.h"

#include "plx.h"
#include "plx_int.h"

PG_FUNCTION_INFO_V1(plx_js_validator);
PG_FUNCTION_INFO_V1(plx_js_inline_handler);

static const PlxKwSpell js_kws[] = {
	{"if", KW_IF}, {"else", KW_ELSE},
	{"while", KW_WHILE}, {"for", KW_FOR}, {"of", KW_OF},
	{"return", KW_RETURN}, {"break", KW_BREAK}, {"continue", KW_NEXT},
	{"try", KW_BEGIN}, {"catch", KW_RESCUE}, {"finally", KW_ENSURE},
	{"throw", KW_RAISE}, {"function", KW_DEF}, {"switch", KW_CASE},
	{"let", KW_LET}, {"const", KW_LET}, {"var", KW_LET},
	{"true", KW_TRUE}, {"false", KW_FALSE}, {"null", KW_NIL},
	{"emit", KW_EMIT}, {"return_next", KW_RETURN_NEXT},
};

static const PlxSurface js_surface = {
	.lanname = "plxjs",
	.block_style = PLX_BLK_BRACE,
	.stmt_semicolon = true,
	.var_sigil = 0,
	.cmt_hash = false,
	.cmt_slash = true,
	.cmt_block = true,
	.type_ann = NULL,			/* annotations via a leading-colon-colon block comment */
	.interp_quote = '`',		/* template literals: `...${expr}...` */
	.interp_hashbrace = false,
	.interp_dollar = false,
	.interp_dollarbrace = true,
	.concat_op = 0,				/* use template literals for concatenation */
	.kws = js_kws,
	.nkws = lengthof(js_kws),
	.excs = NULL,
	.nexcs = 0,
	.flags = PLX_TRUSTED,
};

static char *
js_transpile(const char *src, const PlxFuncMeta *meta)
{
	return plx_transpile(src, meta, &js_surface, CurrentMemoryContext);
}

const PlxDialect plx_js_dialect = {
	.abi_version = PLX_ABI_VERSION,
	.lanname = "plxjs",
	.transpile = js_transpile,
	.flags = PLX_TRUSTED,
};

Datum
plx_js_validator(PG_FUNCTION_ARGS)
{
	return plx_generic_validator(fcinfo, &plx_js_dialect);
}

Datum
plx_js_inline_handler(PG_FUNCTION_ARGS)
{
	return plx_generic_inline_handler(fcinfo, &plx_js_dialect);
}

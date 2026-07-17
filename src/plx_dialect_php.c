/*
 * plx_dialect_php.c - the "plxphp" dialect.
 *
 * PHP surface (brace-delimited blocks, '$' variables, '.' concatenation,
 * "$var"/"{$expr}" interpolation, //, #, and block comments, try/catch/finally,
 * throw). The brace parser is shared with plxjs/plxts in plx_parse_brace.c; the
 * dialect-neutral engine (lowering to plpgsql) lives in plx_transpile.c.
 */
#include "postgres.h"

#include "fmgr.h"
#include "utils/memutils.h"

#include "plx.h"
#include "plx_int.h"
#include "plx_engine.h"

PG_FUNCTION_INFO_V1(plx_php_validator);
PG_FUNCTION_INFO_V1(plx_php_inline_handler);

static const PlxKwSpell php_kws[] = {
	{"if", KW_IF}, {"elseif", KW_ELSIF}, {"else", KW_ELSE},
	{"while", KW_WHILE}, {"for", KW_FOR}, {"foreach", KW_FOREACH}, {"as", KW_AS},
	{"return", KW_RETURN}, {"break", KW_BREAK}, {"continue", KW_NEXT},
	{"try", KW_BEGIN}, {"catch", KW_RESCUE}, {"finally", KW_ENSURE},
	{"throw", KW_RAISE}, {"function", KW_DEF}, {"switch", KW_CASE},
	{"case", KW_WHEN}, {"default", KW_ELSE},
	{"true", KW_TRUE}, {"false", KW_FALSE}, {"null", KW_NIL},
	{"emit", KW_EMIT}, {"return_next", KW_RETURN_NEXT},
};

static const PlxSurface php_surface = {
	.lanname = "plxphp",
	.block_style = PLX_BLK_BRACE,
	.stmt_semicolon = true,
	.var_sigil = '$',
	.cmt_hash = true,
	.cmt_slash = true,
	.cmt_block = true,
	.type_ann = NULL,			/* annotations via a leading-colon-colon block comment */
	.sq_is_raw = true,			/* PHP '...' is raw (backslashes literal) */
	.interp_quote = '"',
	.interp_hashbrace = false,
	.interp_dollar = true,
	.interp_dollarbrace = false,
	.concat_op = '.',
	.kws = php_kws,
	.nkws = lengthof(php_kws),
	.excs = NULL,
	.nexcs = 0,
	.flags = PLX_TRUSTED,
	.parse_body = plx_brace_parse_body,
};

static char *
php_transpile(const char *src, const PlxFuncMeta *meta)
{
	return plx_transpile(src, meta, &php_surface, CurrentMemoryContext);
}

const PlxDialect plx_php_dialect = {
	.abi_version = PLX_ABI_VERSION,
	.lanname = "plxphp",
	.transpile = php_transpile,
	.flags = PLX_TRUSTED,
};

Datum
plx_php_validator(PG_FUNCTION_ARGS)
{
	return plx_generic_validator(fcinfo, &plx_php_dialect);
}

Datum
plx_php_inline_handler(PG_FUNCTION_ARGS)
{
	return plx_generic_inline_handler(fcinfo, &plx_php_dialect);
}

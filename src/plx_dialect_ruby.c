/*
 * plx_dialect_ruby.c — the "plxruby" dialect.
 *
 * Supplies the Ruby dialect name + trampolines. The actual Ruby->plpgsql
 * lowering lives in the shared plx_transpile.c; this file just forwards.
 * Adding PHP (or any dialect) is a near-copy with a different surface.
 */
#include "postgres.h"

#include "fmgr.h"
#include "utils/memutils.h"

#include "plx.h"
#include "plx_int.h"

PG_FUNCTION_INFO_V1(plx_ruby_validator);
PG_FUNCTION_INFO_V1(plx_ruby_inline_handler);

static const PlxKwSpell ruby_kws[] = {
	{"if", KW_IF}, {"unless", KW_UNLESS}, {"elsif", KW_ELSIF}, {"else", KW_ELSE},
	{"then", KW_THEN}, {"end", KW_END}, {"while", KW_WHILE}, {"until", KW_UNTIL},
	{"for", KW_FOR}, {"in", KW_IN}, {"do", KW_DO}, {"begin", KW_BEGIN},
	{"rescue", KW_RESCUE}, {"ensure", KW_ENSURE}, {"case", KW_CASE}, {"when", KW_WHEN},
	{"return", KW_RETURN}, {"next", KW_NEXT}, {"break", KW_BREAK}, {"raise", KW_RAISE},
	{"and", KW_AND}, {"or", KW_OR}, {"not", KW_NOT}, {"emit", KW_EMIT},
	{"return_next", KW_RETURN_NEXT}, {"loop", KW_LOOP}, {"def", KW_DEF},
	{"nil", KW_NIL}, {"true", KW_TRUE}, {"false", KW_FALSE},
};

static const PlxSurface ruby_surface = {
	.lanname = "plxruby",
	.block_style = PLX_BLK_KEYWORD_END,
	.stmt_semicolon = false,
	.var_sigil = 0,
	.cmt_hash = true,
	.cmt_slash = false,
	.cmt_block = false,
	.type_ann = "#::",
	.interp_quote = '"',
	.interp_hashbrace = true,
	.interp_dollar = false,
	.interp_dollarbrace = false,
	.concat_op = 0,
	.kws = ruby_kws,
	.nkws = lengthof(ruby_kws),
	.excs = NULL,
	.nexcs = 0,
	.flags = PLX_TRUSTED,
};

static char *
ruby_transpile(const char *src, const PlxFuncMeta *meta)
{
	return plx_transpile(src, meta, &ruby_surface, CurrentMemoryContext);
}

const PlxDialect plx_ruby_dialect = {
	.abi_version = PLX_ABI_VERSION,
	.lanname = "plxruby",
	.transpile = ruby_transpile,
	.flags = PLX_TRUSTED,
};

Datum
plx_ruby_validator(PG_FUNCTION_ARGS)
{
	return plx_generic_validator(fcinfo, &plx_ruby_dialect);
}

Datum
plx_ruby_inline_handler(PG_FUNCTION_ARGS)
{
	return plx_generic_inline_handler(fcinfo, &plx_ruby_dialect);
}

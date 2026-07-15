/*
 * plx_dialect_go.c - the "plxgo" dialect (Go).
 *
 * Go is a braced C-family language, but its lexer inserts semicolons at line
 * ends (ASI), uses parenless if/for headers, `:=` short declarations with type
 * inference, `for ... range`, and a switch without fall-through. Those differ
 * enough from the shared brace parser that plxgo has its own tokenizer and
 * recursive-descent front end in plx_transpile.c (block_style GO); this surface
 * is a marker. The shared keyword table is unused by the Go path.
 */
#include "postgres.h"

#include "fmgr.h"
#include "utils/memutils.h"

#include "plx.h"
#include "plx_int.h"

PG_FUNCTION_INFO_V1(plx_go_validator);
PG_FUNCTION_INFO_V1(plx_go_inline_handler);

static const PlxSurface go_surface = {
	.lanname = "plxgo",
	.block_style = PLX_BLK_GO,
	.stmt_semicolon = true,
	.var_sigil = 0,
	.cmt_hash = false,
	.cmt_slash = true,
	.cmt_block = true,
	.type_ann = NULL,
	.interp_quote = 0,
	.interp_hashbrace = false,
	.interp_dollar = false,
	.interp_dollarbrace = false,
	.fstrings = false,
	.concat_op = 0,
	.kws = NULL,
	.nkws = 0,
	.excs = NULL,
	.nexcs = 0,
	.flags = PLX_TRUSTED,
};

static char *
go_transpile_entry(const char *src, const PlxFuncMeta *meta)
{
	return plx_transpile(src, meta, &go_surface, CurrentMemoryContext);
}

const PlxDialect plx_go_dialect = {
	.abi_version = PLX_ABI_VERSION,
	.lanname = "plxgo",
	.transpile = go_transpile_entry,
	.flags = PLX_TRUSTED,
};

Datum
plx_go_validator(PG_FUNCTION_ARGS)
{
	return plx_generic_validator(fcinfo, &plx_go_dialect);
}

Datum
plx_go_inline_handler(PG_FUNCTION_ARGS)
{
	return plx_generic_inline_handler(fcinfo, &plx_go_dialect);
}

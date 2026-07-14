/*
 * plx_dialect_python.c - the "plxpython3" dialect.
 *
 * Python surface: indentation-based blocks (INDENT/DEDENT), no variable sigil,
 * f-string interpolation with {expr}, # line comments, if/elif/else, while,
 * for-in over range()/query()/array, try/except/finally, raise. The shared
 * transpiler in plx_transpile.c does the lowering to plpgsql.
 */
#include "postgres.h"

#include "fmgr.h"
#include "utils/memutils.h"

#include "plx.h"
#include "plx_int.h"

PG_FUNCTION_INFO_V1(plx_py_validator);
PG_FUNCTION_INFO_V1(plx_py_inline_handler);

static const PlxKwSpell py_kws[] = {
	{"if", KW_IF}, {"elif", KW_ELSIF}, {"else", KW_ELSE},
	{"while", KW_WHILE}, {"for", KW_FOR}, {"in", KW_IN},
	{"return", KW_RETURN}, {"break", KW_BREAK}, {"continue", KW_NEXT},
	{"try", KW_BEGIN}, {"except", KW_RESCUE}, {"finally", KW_ENSURE}, {"as", KW_AS},
	{"raise", KW_RAISE}, {"def", KW_DEF}, {"class", KW_DEF}, {"lambda", KW_DEF},
	{"match", KW_CASE}, {"pass", KW_PASS},
	{"and", KW_AND}, {"or", KW_OR}, {"not", KW_NOT},
	{"None", KW_NIL}, {"True", KW_TRUE}, {"False", KW_FALSE},
	{"emit", KW_EMIT}, {"return_next", KW_RETURN_NEXT},
};

static const PlxSurface py_surface = {
	.lanname = "plxpython3",
	.block_style = PLX_BLK_INDENT,
	.stmt_semicolon = false,
	.var_sigil = 0,
	.cmt_hash = true,
	.cmt_slash = false,
	.cmt_block = false,
	.type_ann = "#::",			/* Python type annotation: x = 0  #:: integer */
	.interp_quote = 0,
	.interp_hashbrace = false,
	.interp_dollar = false,
	.interp_dollarbrace = false,
	.fstrings = true,
	.concat_op = 0,
	.kws = py_kws,
	.nkws = lengthof(py_kws),
	.excs = NULL,
	.nexcs = 0,
	.flags = PLX_TRUSTED,
};

static char *
py_transpile(const char *src, const PlxFuncMeta *meta)
{
	return plx_transpile(src, meta, &py_surface, CurrentMemoryContext);
}

const PlxDialect plx_py_dialect = {
	.abi_version = PLX_ABI_VERSION,
	.lanname = "plxpython3",
	.transpile = py_transpile,
	.flags = PLX_TRUSTED,
};

Datum
plx_py_validator(PG_FUNCTION_ARGS)
{
	return plx_generic_validator(fcinfo, &plx_py_dialect);
}

Datum
plx_py_inline_handler(PG_FUNCTION_ARGS)
{
	return plx_generic_inline_handler(fcinfo, &plx_py_dialect);
}

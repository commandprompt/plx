/*
 * plx_dialect_plsql.c - the "plxplsql" dialect (Oracle PL/SQL).
 *
 * PL/SQL and plpgsql are both Ada-descended, so most PL/SQL is already valid
 * plpgsql. The front end in plx_transpile.c (PLX_BLK_PLSQL) is a layout-
 * preserving token rewriter that translates the Oracle-specific spellings
 * (NUMBER/VARCHAR2/..., DBMS_OUTPUT.PUT_LINE, RAISE_APPLICATION_ERROR, EXECUTE
 * IMMEDIATE, FROM DUAL, NVL, seq.NEXTVAL, SYSDATE) and emits the body directly,
 * since PL/SQL already carries its own DECLARE/BEGIN/END structure.
 */
#include "postgres.h"

#include "fmgr.h"
#include "utils/memutils.h"

#include "plx.h"
#include "plx_int.h"

PG_FUNCTION_INFO_V1(plx_plsql_validator);
PG_FUNCTION_INFO_V1(plx_plsql_inline_handler);

static const PlxSurface plsql_surface = {
	.lanname = "plxplsql",
	.block_style = PLX_BLK_PLSQL,
	.stmt_semicolon = true,
	.var_sigil = 0,
	.cmt_hash = false,
	.cmt_slash = false,
	.cmt_block = false,
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
plsql_transpile_body(const char *src, const PlxFuncMeta *meta)
{
	return plx_transpile(src, meta, &plsql_surface, CurrentMemoryContext);
}

const PlxDialect plx_plsql_dialect = {
	.abi_version = PLX_ABI_VERSION,
	.lanname = "plxplsql",
	.transpile = plsql_transpile_body,
	.flags = PLX_TRUSTED,
};

Datum
plx_plsql_validator(PG_FUNCTION_ARGS)
{
	return plx_generic_validator(fcinfo, &plx_plsql_dialect);
}

Datum
plx_plsql_inline_handler(PG_FUNCTION_ARGS)
{
	return plx_generic_inline_handler(fcinfo, &plx_plsql_dialect);
}

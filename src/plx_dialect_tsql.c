/*
 * plx_dialect_tsql.c - the "plxtsql" dialect (Transact-SQL: SQL Server / Sybase).
 *
 * T-SQL needs real restructuring to become plpgsql: @local variables and inline
 * DECLARE must be hoisted, IF/WHILE bodies use BEGIN..END rather than
 * THEN..END IF / LOOP..END LOOP, and TRY/CATCH becomes an EXCEPTION block. Like
 * COBOL it therefore has its own tokenizer and recursive-descent front end in
 * plx_transpile.c (block_style TSQL); this surface is a marker. The shared
 * keyword table is unused by the T-SQL path but kept for consistency.
 */
#include "postgres.h"

#include "fmgr.h"
#include "utils/memutils.h"

#include "plx.h"
#include "plx_int.h"

PG_FUNCTION_INFO_V1(plx_tsql_validator);
PG_FUNCTION_INFO_V1(plx_tsql_inline_handler);

static const PlxSurface tsql_surface = {
	.lanname = "plxtsql",
	.block_style = PLX_BLK_TSQL,
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
tsql_transpile_entry(const char *src, const PlxFuncMeta *meta)
{
	return plx_transpile(src, meta, &tsql_surface, CurrentMemoryContext);
}

const PlxDialect plx_tsql_dialect = {
	.abi_version = PLX_ABI_VERSION,
	.lanname = "plxtsql",
	.transpile = tsql_transpile_entry,
	.flags = PLX_TRUSTED,
};

Datum
plx_tsql_validator(PG_FUNCTION_ARGS)
{
	return plx_generic_validator(fcinfo, &plx_tsql_dialect);
}

Datum
plx_tsql_inline_handler(PG_FUNCTION_ARGS)
{
	return plx_generic_inline_handler(fcinfo, &plx_tsql_dialect);
}

/*
 * plx_core.c — plx core: dialect registry + generic validator and
 * inline (DO-block) handler shared by every dialect.
 *
 * Key architectural fact (verified via readelf on plpgsql.so, PG 18.4):
 *   plpgsql_call_handler / _inline_handler / _validator / _compile are the
 *   only GLOBAL (cross-.so callable) entry points; the executor internals
 *   (plpgsql_exec_function, plpgsql_compile_inline, the scanner, plpgsql_Datums)
 *   are LOCAL and cannot be linked from another module.  So plexcellent never
 *   touches them: it transpiles a dialect body to plpgsql text at DDL time and
 *   routes execution through plpgsql's own GLOBAL fmgr handlers.
 */
#include "postgres.h"

#include "fmgr.h"
#include "funcapi.h"
#include "access/htup_details.h"
#include "access/table.h"
#include "access/xact.h"
#include "catalog/indexing.h"
#include "catalog/pg_language.h"
#include "catalog/pg_proc.h"
#include "commands/proclang.h"
#include "nodes/parsenodes.h"
#include "utils/builtins.h"
#include "utils/rel.h"
#include "utils/syscache.h"

#include "plx.h"

PG_MODULE_MAGIC;

/* Built-in dialects registered at module load */
extern const PlxDialect plx_ruby_dialect;
extern const PlxDialect plx_php_dialect;
extern const PlxDialect plx_js_dialect;
extern const PlxDialect plx_py_dialect;
extern const PlxDialect plx_cobol_dialect;
extern const PlxDialect plx_plsql_dialect;

/* ---- dialect registry ------------------------------------------------- */

#define PLX_MAX_DIALECTS 32
static const PlxDialect *plx_dialects[PLX_MAX_DIALECTS];
static int	plx_ndialects = 0;

void
plx_register_dialect(const PlxDialect *d)
{
	int			i;

	if (d->abi_version != PLX_ABI_VERSION)
		ereport(ERROR,
				(errmsg("plx: dialect \"%s\" has ABI %u, core expects %u",
						d->lanname ? d->lanname : "(unnamed)",
						d->abi_version, (uint32) PLX_ABI_VERSION)));

	for (i = 0; i < plx_ndialects; i++)
		if (strcmp(plx_dialects[i]->lanname, d->lanname) == 0)
			return;				/* idempotent */

	if (plx_ndialects >= PLX_MAX_DIALECTS)
		ereport(ERROR, (errmsg("plx: too many dialects registered")));

	plx_dialects[plx_ndialects++] = d;
	elog(DEBUG1, "plx: registered dialect \"%s\"", d->lanname);
}

const PlxDialect *
plx_lookup_dialect(const char *lanname)
{
	int			i;

	for (i = 0; i < plx_ndialects; i++)
		if (strcmp(plx_dialects[i]->lanname, lanname) == 0)
			return plx_dialects[i];
	return NULL;
}

/* ---- generic validator (DDL time) ------------------------------------- */

/* Populate PlxFuncMeta from a pg_proc tuple (args incl. OUT/TABLE columns). */
static void
build_meta(PlxFuncMeta *m, HeapTuple tup, Oid funcoid)
{
	Form_pg_proc pf = (Form_pg_proc) GETSTRUCT(tup);

	memset(m, 0, sizeof(*m));
	m->funcoid = funcoid;
	m->rettype = pf->prorettype;
	m->retset = pf->proretset;
	m->prokind = pf->prokind;
	m->nargs = get_func_arg_info(tup, &m->argtypes, &m->argnames, &m->argmodes);
}

/*
 * Runs from ProcedureCreate() on every CREATE/REPLACE FUNCTION and on restore.
 * Transpiles the dialect body to plpgsql and rewrites pg_proc.prosrc in place
 * (Strategy A). lanplcallfoid stays plpgsql's own handler, so runtime is 100%
 * stock plpgsql. Idempotent via the plx sentinel prefix.
 */
Datum
plx_generic_validator(FunctionCallInfo fcinfo, const PlxDialect *d)
{
	Oid			funcoid = PG_GETARG_OID(0);
	HeapTuple	tuple;
	Datum		prosrcdatum;
	bool		isnull;
	char	   *rubysrc;

	/* Respect check_function_bodies / permission gating. */
	if (!CheckFunctionValidatorAccess(fcinfo->flinfo->fn_oid, funcoid))
		PG_RETURN_VOID();

	tuple = SearchSysCacheCopy1(PROCOID, ObjectIdGetDatum(funcoid));
	if (!HeapTupleIsValid(tuple))
		elog(ERROR, "plx: cache lookup failed for function %u", funcoid);

	prosrcdatum = SysCacheGetAttr(PROCOID, tuple, Anum_pg_proc_prosrc, &isnull);
	if (isnull)
		elog(ERROR, "plx: null prosrc for function %u", funcoid);
	rubysrc = TextDatumGetCString(prosrcdatum);

	if (!plx_has_sentinel(rubysrc))
	{
		PlxFuncMeta meta;
		char	   *plpgsql;
		Relation	rel;
		HeapTuple	newtup;
		Datum		values[Natts_pg_proc];
		bool		nulls[Natts_pg_proc];
		bool		repl[Natts_pg_proc];

		build_meta(&meta, tuple, funcoid);
		plpgsql = d->transpile(rubysrc, &meta);

		memset(nulls, false, sizeof(nulls));
		memset(repl, false, sizeof(repl));
		values[Anum_pg_proc_prosrc - 1] = CStringGetTextDatum(plpgsql);
		repl[Anum_pg_proc_prosrc - 1] = true;

		rel = table_open(ProcedureRelationId, RowExclusiveLock);
		newtup = heap_modify_tuple(tuple, RelationGetDescr(rel), values, nulls, repl);
		CatalogTupleUpdate(rel, &newtup->t_self, newtup);
		table_close(rel, RowExclusiveLock);
		CommandCounterIncrement();
	}

	PG_RETURN_VOID();
}

/* ---- generic inline (DO block) handler -------------------------------- */

/*
 * Transpile the anonymous block, then delegate to plpgsql's own (GLOBAL)
 * inline handler via its laninline OID.  We never call the LOCAL
 * plpgsql_compile_inline directly.
 */
Datum
plx_generic_inline_handler(FunctionCallInfo fcinfo, const PlxDialect *d)
{
	InlineCodeBlock *codeblock = castNode(InlineCodeBlock,
										  (Node *) PG_GETARG_POINTER(0));
	char	   *transpiled;
	InlineCodeBlock *cb2;
	Oid			plpgsql_lang;
	HeapTuple	langtup;
	Form_pg_language langform;
	Oid			inline_oid;

	transpiled = d->transpile(codeblock->source_text, NULL);

	plpgsql_lang = get_language_oid("plpgsql", false);
	langtup = SearchSysCache1(LANGOID, ObjectIdGetDatum(plpgsql_lang));
	if (!HeapTupleIsValid(langtup))
		elog(ERROR, "plx: cache lookup failed for language plpgsql");
	langform = (Form_pg_language) GETSTRUCT(langtup);
	inline_oid = langform->laninline;
	ReleaseSysCache(langtup);

	if (!OidIsValid(inline_oid))
		ereport(ERROR, (errmsg("plx: plpgsql has no inline handler")));

	cb2 = makeNode(InlineCodeBlock);
	cb2->source_text = transpiled;
	cb2->langOid = plpgsql_lang;
	cb2->langIsTrusted = codeblock->langIsTrusted;
	cb2->atomic = codeblock->atomic;

	return OidFunctionCall1(inline_oid, PointerGetDatum(cb2));
}

/* ---- module init ------------------------------------------------------ */

void
_PG_init(void)
{
	/* Ensure plpgsql's GLOBAL symbols are resolvable for later phases. */
	load_file("$libdir/plpgsql", false);

	/* Register built-in dialects. */
	plx_register_dialect(&plx_ruby_dialect);
	plx_register_dialect(&plx_php_dialect);
	plx_register_dialect(&plx_js_dialect);
	plx_register_dialect(&plx_py_dialect);
	plx_register_dialect(&plx_cobol_dialect);
	plx_register_dialect(&plx_plsql_dialect);
}

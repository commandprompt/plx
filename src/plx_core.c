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
#include "catalog/pg_type.h"
#include "commands/event_trigger.h"
#include "commands/proclang.h"
#include "commands/trigger.h"
#include "executor/spi.h"
#include "nodes/parsenodes.h"
#include "miscadmin.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/guc.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/syscache.h"

#include "plx.h"

/* PostgreSQL 13 names the event_trigger type's OID EVTTRIGGEROID */
#if PG_VERSION_NUM < 140000
#define EVENT_TRIGGEROID EVTTRIGGEROID
#endif

PG_MODULE_MAGIC;

/* Built-in dialect descriptors are declared in plx.h (registered below). */

/* ---- dialect registry ------------------------------------------------- */

#define PLX_MAX_DIALECTS 32
static const PlxDialect *plx_dialects[PLX_MAX_DIALECTS];

Oid			plx_ext_namespace = InvalidOid;
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

/* ---- plpgsql privilege gate ------------------------------------------- */

/*
 * A plx function is stored and executed as plpgsql, so a role allowed to use a
 * plx language could otherwise run plpgsql after USAGE on plpgsql was revoked
 * from it. Require USAGE on plpgsql as well, at the same points plpgsql itself
 * checks it: when a function is created (the validator) and for a DO block
 * (the inline handler). As with plpgsql, functions that already exist keep
 * running after a REVOKE. Superusers pass the check as usual.
 */
static void
plx_require_plpgsql_usage(const PlxDialect *d)
{
	Oid			plpgsql_lang = get_language_oid("plpgsql", false);
	AclResult	aclresult;

#if PG_VERSION_NUM >= 160000
	aclresult = object_aclcheck(LanguageRelationId, plpgsql_lang, GetUserId(),
								ACL_USAGE);
#else
	aclresult = pg_language_aclcheck(plpgsql_lang, GetUserId(), ACL_USAGE);
#endif
	if (aclresult != ACLCHECK_OK)
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("permission denied for language plpgsql"),
				 errdetail("Language %s is executed as plpgsql, so using it requires USAGE on plpgsql.",
						   d->lanname)));
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
 * plpgsql's own validator cannot be called for a plx function:
 * CheckFunctionValidatorAccess, which it calls first, requires the validator
 * to belong to the function's language. So repeat what plpgsql_validator
 * (pl_handler.c) checks. The pseudo-type checks run always, as in plpgsql.
 * Check the signature before transpiling, as plpgsql does before compiling.
 */
static void
plx_check_signature(HeapTuple tup, const PlxDialect *d,
					bool *is_dml_trigger, bool *is_event_trigger)
{
	Form_pg_proc pf = (Form_pg_proc) GETSTRUCT(tup);
	Oid		   *argtypes;
	char	  **argnames;
	char	   *argmodes;
	int			nargs;
	int			i;

	*is_dml_trigger = false;
	*is_event_trigger = false;

	/* no pseudo-type result except trigger, event_trigger, record, void,
	 * or a polymorphic type */
	if (get_typtype(pf->prorettype) == TYPTYPE_PSEUDO)
	{
		if (pf->prorettype == TRIGGEROID)
			*is_dml_trigger = true;
		else if (pf->prorettype == EVENT_TRIGGEROID)
			*is_event_trigger = true;
		else if (pf->prorettype != RECORDOID &&
				 pf->prorettype != VOIDOID &&
				 !IsPolymorphicType(pf->prorettype))
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("%s functions cannot return type %s",
							d->lanname, format_type_be(pf->prorettype))));
	}

	/* no pseudo-type argument (IN or OUT) except record or polymorphic */
	nargs = get_func_arg_info(tup, &argtypes, &argnames, &argmodes);
	for (i = 0; i < nargs; i++)
	{
		if (get_typtype(argtypes[i]) == TYPTYPE_PSEUDO &&
			argtypes[i] != RECORDOID &&
			!IsPolymorphicType(argtypes[i]))
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("%s functions cannot accept type %s",
							d->lanname, format_type_be(argtypes[i]))));
	}
}

/* plpgsql_compile from plpgsql.so; see plpgsql.h */
typedef void *(*plx_plpgsql_compile_fn) (FunctionCallInfo fcinfo,
										 bool forValidator);

/*
 * Test-compile the stored plpgsql the way plpgsql_validator does when
 * check_function_bodies is on. This finds plpgsql syntax errors and the
 * checks plpgsql makes only while compiling, such as trigger functions with
 * declared arguments. plpgsql_compile is an exported symbol of plpgsql.so,
 * so look it up there rather than link against it. Compiling for the
 * validator caches the function just as plpgsql's own validator does; the
 * cache entry is keyed to the pg_proc row we just wrote, so a call compiles
 * that same text.
 */
static void
plx_test_compile(Oid funcoid, bool is_dml_trigger, bool is_event_trigger)
{
	static plx_plpgsql_compile_fn compile_fn = NULL;
	LOCAL_FCINFO(fake_fcinfo, 0);
	FmgrInfo	flinfo;
	TriggerData trigdata;
	EventTriggerData etrigdata;
	int			rc;

	if (compile_fn == NULL)
		compile_fn = (plx_plpgsql_compile_fn)
			load_external_function("$libdir/plpgsql", "plpgsql_compile",
								   true, NULL);

	SPI_connect();

	MemSet(fake_fcinfo, 0, SizeForFunctionCallInfo(0));
	MemSet(&flinfo, 0, sizeof(flinfo));
	fake_fcinfo->flinfo = &flinfo;
	flinfo.fn_oid = funcoid;
	flinfo.fn_mcxt = CurrentMemoryContext;
	if (is_dml_trigger)
	{
		MemSet(&trigdata, 0, sizeof(trigdata));
		trigdata.type = T_TriggerData;
		fake_fcinfo->context = (Node *) &trigdata;
	}
	else if (is_event_trigger)
	{
		MemSet(&etrigdata, 0, sizeof(etrigdata));
		etrigdata.type = T_EventTriggerData;
		fake_fcinfo->context = (Node *) &etrigdata;
	}

	(void) compile_fn(fake_fcinfo, true);

	if ((rc = SPI_finish()) != SPI_OK_FINISH)
		elog(ERROR, "SPI_finish failed: %s", SPI_result_code_string(rc));
}

/*
 * Runs from ProcedureCreate() on every CREATE/REPLACE FUNCTION and on restore.
 * Transpiles the dialect body to plpgsql and rewrites pg_proc.prosrc in place
 * (Strategy A). lanplcallfoid stays plpgsql's own handler, so runtime is 100%
 * stock plpgsql. Idempotent via the plx sentinel prefix. Then makes the
 * checks plpgsql's validator would make on the stored plpgsql.
 */
Datum
plx_generic_validator(FunctionCallInfo fcinfo, const PlxDialect *d)
{
	Oid			funcoid = PG_GETARG_OID(0);
	HeapTuple	tuple;
	Datum		prosrcdatum;
	bool		isnull;
	char	   *rubysrc;
	bool		is_dml_trigger;
	bool		is_event_trigger;

	/* Respect check_function_bodies / permission gating. */
	if (!CheckFunctionValidatorAccess(fcinfo->flinfo->fn_oid, funcoid))
		PG_RETURN_VOID();

	/* before the sentinel check, so a pre-transpiled body is gated too */
	plx_require_plpgsql_usage(d);

	tuple = SearchSysCacheCopy1(PROCOID, ObjectIdGetDatum(funcoid));
	if (!HeapTupleIsValid(tuple))
		elog(ERROR, "plx: cache lookup failed for function %u", funcoid);

	plx_check_signature(tuple, d, &is_dml_trigger, &is_event_trigger);

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
		/* the validator is an extension member, so it lives in our schema */
		plx_ext_namespace = get_func_namespace(fcinfo->flinfo->fn_oid);
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

	/* as plpgsql does, compile only when check_function_bodies is on */
	if (check_function_bodies)
		plx_test_compile(funcoid, is_dml_trigger, is_event_trigger);

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

	plx_ext_namespace = get_func_namespace(fcinfo->flinfo->fn_oid);

	plx_require_plpgsql_usage(d);

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

/* PostgreSQL 16 and later declare _PG_init in fmgr.h; declare it on 13-15. */
#if PG_VERSION_NUM < 160000
void		_PG_init(void);
#endif

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
	plx_register_dialect(&plx_ts_dialect);
	plx_register_dialect(&plx_tsql_dialect);
	plx_register_dialect(&plx_go_dialect);
}

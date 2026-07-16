/*
 * plx.h — shared ABI for plx (marketing: "plx — plExcellent"), a
 * dialect-pluggable procedural language over plpgsql.
 *
 * A "dialect" is an alternate front-end SYNTAX (Ruby, PHP, ...). The engine
 * that compiles and executes every function is stock plpgsql; a dialect only
 * transpiles its body to plpgsql text at DDL time (see ARCHITECTURE.md).
 */
#ifndef PLX_H
#define PLX_H

#include "postgres.h"
#include "fmgr.h"

#define PLX_ABI_VERSION 1

/* Dialect capability flags */
#define PLX_TRUSTED 0x01

/*
 * Metadata about the function being transpiled, handed to a dialect's
 * transpile() callback so it can legalize argument names, RETURN forms, etc.
 * (Fully populated from the DDL validator; may be NULL for inline/DO blocks.)
 */
typedef struct PlxFuncMeta
{
	int			nargs;
	Oid		   *argtypes;
	char	  **argnames;
	char	   *argmodes;
	Oid			rettype;
	bool		retset;			/* proretset: legalizes RETURN NEXT/QUERY */
	char		prokind;		/* 'f' | 'p' | 'w' */
	Oid			funcoid;
} PlxFuncMeta;

/*
 * A registered dialect. lanname is the stable registry key (matches the
 * CREATE LANGUAGE name, e.g. "plruby"). transpile() returns palloc'd plpgsql
 * source text.
 */
typedef struct PlxDialect
{
	uint32		abi_version;	/* must equal PLX_ABI_VERSION */
	const char *lanname;
	char	   *(*transpile) (const char *src, const PlxFuncMeta *meta);
	int			flags;
} PlxDialect;

/* Registry (implemented in plx_core.c) */
extern void plx_register_dialect(const PlxDialect *d);
extern const PlxDialect *plx_lookup_dialect(const char *lanname);

/*
 * Built-in dialect descriptors. Declared here (rather than only in plx_core.c)
 * so each defining translation unit sees a prior declaration, satisfying
 * -Wmissing-variable-declarations.
 */
extern const PlxDialect plx_ruby_dialect;
extern const PlxDialect plx_php_dialect;
extern const PlxDialect plx_js_dialect;
extern const PlxDialect plx_py_dialect;
extern const PlxDialect plx_cobol_dialect;
extern const PlxDialect plx_plsql_dialect;
extern const PlxDialect plx_ts_dialect;
extern const PlxDialect plx_tsql_dialect;
extern const PlxDialect plx_go_dialect;

/* Generic entry points; per-dialect trampolines forward to these */
extern Datum plx_generic_validator(FunctionCallInfo fcinfo, const PlxDialect *d);
extern Datum plx_generic_inline_handler(FunctionCallInfo fcinfo, const PlxDialect *d);

/* True if prosrc already carries the plx sentinel (already transpiled). */
extern bool plx_has_sentinel(const char *prosrc);

/* (plx_transpile + PlxSurface live in plx_int.h, shared with the transpiler.) */

#endif							/* PLX_H */

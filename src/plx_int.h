/*
 * plx_int.h — internal interface shared between the transpiler core
 * (plx_transpile.c) and the per-dialect surface definitions
 * (plx_dialect_ruby.c, plx_dialect_php.c, ...).
 *
 * A PlxSurface is pure configuration: it tells the shared lexer + parser how a
 * particular dialect spells its keywords, delimits blocks, writes comments and
 * string interpolation, sigils its variables, and concatenates strings. The
 * lowering to plpgsql (expression rewriting, DECLARE-hoisting, type inference,
 * emission, intrinsics) is dialect-neutral and lives entirely in plx_transpile.c.
 */
#ifndef PLX_INT_H
#define PLX_INT_H

#include "postgres.h"
#include "plx.h"					/* PlxFuncMeta */

/* Canonical statement/expression keywords. Dialects map their own spellings to
 * these (e.g. PHP "elseif" -> KW_ELSIF, "catch" -> KW_RESCUE). */
typedef enum
{
	KW_NONE, KW_IF, KW_UNLESS, KW_ELSIF, KW_ELSE, KW_THEN, KW_END, KW_WHILE,
	KW_UNTIL, KW_FOR, KW_FOREACH, KW_IN, KW_AS, KW_OF, KW_DO, KW_BEGIN, KW_RESCUE,
	KW_ENSURE, KW_CASE, KW_WHEN, KW_RETURN, KW_NEXT, KW_BREAK, KW_RAISE,
	KW_AND, KW_OR, KW_NOT, KW_EMIT, KW_RETURN_NEXT, KW_LOOP, KW_DEF, KW_LET,
	KW_NIL, KW_TRUE, KW_FALSE
} Kw;

typedef struct { const char *w; Kw k; } PlxKwSpell;
typedef struct { const char *cls; const char *cond; } PlxExcMap;

typedef enum
{
	PLX_BLK_KEYWORD_END,			/* Ruby: if ... end */
	PLX_BLK_BRACE,					/* PHP/JS: if (...) { ... } */
	PLX_BLK_INDENT					/* Python: if ...: <indent> */
} PlxBlockStyle;

typedef struct PlxSurface
{
	const char *lanname;			/* "plruby", "plphp", ... */
	PlxBlockStyle block_style;
	bool		stmt_semicolon;		/* statements end at ';' (else newline) */
	char		var_sigil;			/* '$' for PHP, 0 for none */
	bool		cmt_hash;			/* '#' line comment */
	bool		cmt_slash;			/* '//' line comment */
	bool		cmt_block;			/* C-style block comment */
	const char *type_ann;			/* type annotation lead, e.g. "#::" or "::" */
	char		interp_quote;		/* the quote char that interpolates ('"' or '`') */
	bool		interp_hashbrace;	/* "...#{expr}..." interpolation (Ruby) */
	bool		interp_dollar;		/* "...$var..." / "...{$expr}..." interpolation (PHP) */
	bool		interp_dollarbrace;	/* "...${expr}..." interpolation (JS template) */
	char		concat_op;			/* string concat operator char ('.' PHP), 0=none */
	const PlxKwSpell *kws;
	int			nkws;
	const PlxExcMap *excs;
	int			nexcs;
	int			flags;				/* PLX_TRUSTED, ... */
} PlxSurface;

/* Transpile a dialect body to a fully assembled plpgsql function body. */
extern char *plx_transpile(const char *body, const PlxFuncMeta *meta,
						   const PlxSurface *surf, MemoryContext scratch);
extern bool plx_has_sentinel(const char *prosrc);

#endif							/* PLX_INT_H */

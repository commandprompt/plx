/*
 * plx_dialect_lua.c - the "plxlua" dialect (Lua).
 *
 * Lua is a keyword-`end` language like Ruby, but its statements are free
 * form (newlines are not significant), its blocks are matched by keyword
 * (`do`/`then` ... `end`, `repeat` ... `until`), its locals are block scoped,
 * and its expression grammar (`..`, `~=`, `#`, table constructors, method
 * calls, the value-returning `and`/`or`) is foreign to the shared text
 * rewriter. So plxlua runs on the shared lexer (with the Lua lexemes switched
 * on in its surface) but brings its own statement parser and its own
 * expression translator, a precedence-climbing parser that emits SQL with the
 * parentheses SQL's precedence needs. The translator is also installed as the
 * surface's rewrite_expr hook, so the shared intrinsics (perform, execute,
 * return_query, call, the cursor calls, raise) and plx_sql_arg are reused
 * unchanged: every expression they rewrite comes back here.
 *
 * Block scoping is kept faithful by renaming: a local that would collide
 * with a visible variable of another scope (shadowing), or with a parameter,
 * or with an earlier variable of a different type, gets a fresh plpgsql name.
 */
#include "postgres.h"

#include "catalog/pg_type.h"
#include "common/keywords.h"
#include "fmgr.h"
#include "lib/stringinfo.h"
#include "mb/pg_wchar.h"
#include "miscadmin.h"
#include "parser/scansup.h"
#include "utils/builtins.h"
#include "utils/hsearch.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"

#include "plx.h"
#include "plx_int.h"
#include "plx_engine.h"

PG_FUNCTION_INFO_V1(plx_lua_validator);
PG_FUNCTION_INFO_V1(plx_lua_inline_handler);

static const PlxKwSpell lua_kws[] = {
	{"if", KW_IF}, {"elseif", KW_ELSIF}, {"else", KW_ELSE}, {"then", KW_THEN},
	{"end", KW_END}, {"while", KW_WHILE}, {"do", KW_DO}, {"for", KW_FOR},
	{"in", KW_IN}, {"repeat", KW_LOOP}, {"until", KW_UNTIL},
	{"return", KW_RETURN}, {"break", KW_BREAK}, {"local", KW_LET},
	{"function", KW_DEF}, {"and", KW_AND}, {"or", KW_OR}, {"not", KW_NOT},
	{"nil", KW_NIL}, {"true", KW_TRUE}, {"false", KW_FALSE},
	{"emit", KW_EMIT}, {"return_next", KW_RETURN_NEXT},
};

/* SQL operator precedence, loosest first (see the PostgreSQL manual, 4.1.6) */
enum
{
	SP_OR = 1, SP_AND, SP_NOT, SP_IS, SP_CMP, SP_OTHER, SP_ADD, SP_MUL,
	SP_POW, SP_UNARY, SP_ATOM
};

/* Lua binary operators and their (left, right) priorities from lparser.c */
enum
{
	OP_NONE, OP_OR, OP_AND, OP_LT, OP_GT, OP_LE, OP_GE, OP_NE, OP_EQ,
	OP_BOR, OP_BXOR, OP_BAND, OP_SHL, OP_SHR, OP_CONCAT, OP_ADD, OP_SUB,
	OP_MUL, OP_DIV, OP_IDIV, OP_MOD, OP_POW
};

static const struct
{
	int			left,
				right;
}			lua_prio[] = {
	{0, 0},
	{1, 1}, {2, 2},
	{3, 3}, {3, 3}, {3, 3}, {3, 3}, {3, 3}, {3, 3},
	{4, 4}, {5, 5}, {6, 6}, {7, 7}, {7, 7},
	/* '..' is right associative in Lua; concatenation is associative, so it
	 * is parsed left to right here to keep a long chain from recursing */
	{9, 9},
	{10, 10}, {10, 10},
	{11, 11}, {11, 11}, {11, 11}, {11, 11},
	{14, 13},
};

#define LUA_UNARY_PRIO 12

/* fields of a pcall error variable, read with GET STACKED DIAGNOSTICS */
static const struct
{
	const char *f;
	const char *item;
}			lua_errf[] = {
	{"sqlstate", "RETURNED_SQLSTATE"}, {"detail", "PG_EXCEPTION_DETAIL"},
	{"hint", "PG_EXCEPTION_HINT"}, {"constraint", "CONSTRAINT_NAME"},
	{"column", "COLUMN_NAME"}, {"table", "TABLE_NAME"},
	{"schema", "SCHEMA_NAME"}, {"datatype", "PG_DATATYPE_NAME"},
	{"context", "PG_EXCEPTION_CONTEXT"},
};

#define LUA_ERRF_SQLSTATE 0x01
#define LUA_ERRF_DETAIL   0x02
#define LUA_ERRF_HINT     0x04

#define LUA_MARK '\x02'			/* pcall field-capture marker; rejected in source */

/* ------------------------------------------------------------------ state */

typedef struct LuaBind
{
	char	   *lname;			/* Lua name (case-sensitive) */
	char	   *pgname;			/* plpgsql variable name */
	PlxLocal2  *loc;			/* the DECLAREd local, or NULL for an implicit
								 * FOR variable */
	const char *typ;			/* type of an implicit FOR variable */
	bool		isconst;		/* <const>: assignment is rejected */
	int			errid;			/* > 0: a pcall error variable */
	int			level;			/* scope level it was declared in */
	struct LuaBind *shadow;		/* previous visible binding, same Lua name */
	struct LuaBind *pgshadow;	/* previous visible binding, same plpgsql name */
	struct LuaBind *snext;		/* next binding in the same scope */
} LuaBind;

typedef struct LuaScope
{
	LuaBind    *binds;
	int			level;
	struct LuaScope *outer;
} LuaScope;

enum
{
	LOOP_WHILE, LOOP_FOR, LOOP_REPEAT, LOOP_FUNC
};

typedef struct LuaLoop
{
	int			kind;
	int			id;				/* __plx_lp_N / __plx_rb_N / __plx_pc_N */
	char	   *cont_label;		/* ::label:: ending the body: goto = continue */
	char	   *brk_label;		/* ::label:: right after the loop: goto = break */
	bool		need_label;		/* a goto in a nested loop names this loop */
	bool		rb_used;		/* repeat: a continue jumped to the until test */
	bool		pc_ret;			/* pcall: a return left the protected block */
	char	   *pc_ok;			/* pcall: variables set on success */
	char	   *pc_err;
	int			pc_errid;
	struct LuaLoop *outer;
} LuaLoop;

typedef struct LuaErrVar
{
	int			id;
	int			mask;			/* bits (of lua_errf) of the fields that are read */
} LuaErrVar;

typedef struct LuaEnt
{
	char		key[NAMEDATALEN];
	LuaBind    *top;
} LuaEnt;

typedef struct LuaErrEnt
{
	char		key[NAMEDATALEN];
	LuaErrVar  *ev;
} LuaErrEnt;

typedef struct Lua
{
	Ctx		   *cx;
	int			ntok;
	int		   *match;			/* block opener <-> closer token index */
	bool	   *labelok;		/* a recognized ::label:: statement */
	HTAB	   *names;			/* Lua name -> visible binding */
	HTAB	   *pgnames;		/* plpgsql name -> visible binding */
	HTAB	   *errhash;		/* plpgsql name -> LuaErrVar */
	LuaScope   *scope;
	LuaScope   *fscope;			/* the function's own scope */
	LuaLoop    *loops;
	int			inloop;			/* loops and protected blocks enclosing us */
	int			seq;			/* generated-name counter */
	LuaErrVar **errv;
	int			nerr,
				errcap;
	bool		markers;
	bool		collect;		/* recording referenced variables */
	char	  **refs;
	int			nrefs,
				refcap;
} Lua;

/* A translated expression. */
typedef struct LuaV
{
	char	   *s;				/* SQL text */
	int			prec;			/* SP_* of its top-level operator */
	const char *op;				/* the SP_OTHER operator, for regrouping */
	char	   *bs;				/* rendering as a condition, when it differs */
	int			bprec;
	const char *typ;			/* plpgsql type when known */
	char	   *strval;			/* a string literal's value */
	bool		isbool;			/* known boolean */
	bool		istext;			/* known text */
	bool		lit;			/* a literal (number, string, boolean, nil, table
								 * of literals) */
	bool		isnil;
	bool		isfalse;
	bool		tbl;			/* a table constructor */
	bool		rec;			/* a record */
	bool		neg;			/* text starts with '-' */
	bool		call;			/* a function call */
	bool		concat;			/* a .. chain */
	bool		simple;			/* a variable, field or literal: may repeat */
	bool		chain;			/* a name, field or subscript chain (or a
								 * parenthesized expression): can take a
								 * suffix as is */
	bool		coalesce;		/* COALESCE(...) made from 'x or default' */
	struct LuaV *and_a,
			   *and_b;			/* an 'and', kept for 'a and b or c' */
	int			and_plen;		/* a chained 'and': its left operand is the
								 * first and_plen bytes of bs (and_a NULL) */
	bool		case_open;		/* CASE from 'a and x or ...' that a further
								 * 'or' extends (another arm, or the ELSE) */
	int			shifts;			/* shift subqueries nested in this value */
	int			nest;			/* parenthesis nesting of a left-associative
								 * chain, bounded by PLX_MAX_DEPTH */

	/*
	 * A left-associative chain (a + b + c, a .. b .. c, x and y and z, x or
	 * y or z) grows its text in place, so a long chain costs linear rather
	 * than quadratic time. When set, s (bs) is sbuf->data (bbuf->data).
	 */
	StringInfo	sbuf;
	StringInfo	bbuf;
} LuaV;

/* Expression translator cursor. */
typedef struct LuaX
{
	Lua		   *L;
	Ctx		   *cx;
	Tok		   *t;
	int			p,
				e;
	int			loff;			/* line offset (re-lexed hook text) */
	bool		diag;			/* rendering a message */
} LuaX;

static Tok	lua_eof;			/* zero-initialized: kind T_EOF */

static void lua_block(Lua *L, int ind);
static void lua_stmt(Lua *L, int ind);
static LuaV *x_subexpr(LuaX *x, int limit);
static LuaV *x_table(LuaX *x);
static LuaV *x_tobool(LuaX *x, LuaV *v);

/* ------------------------------------------------------------- utilities */

static bool
tk_kw(Tok *k, Kw w)
{
	return k->kind == T_KW && k->kw == w;
}

static bool
tk_name(Tok *k, const char *w)
{
	return k->kind == T_IDENT && plx_name_eq(k, w);
}

static int
hexval(char c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	if (c >= 'A' && c <= 'F')
		return c - 'A' + 10;
	return -1;
}

/* a short, printable description of a token for an error message */
static char *
tok_desc(Tok *k)
{
	if (k->kind == T_EOF)
		return pstrdup("end of input");
	if (k->kind == T_NEWLINE)
		return pstrdup("end of line");
	if (k->kind == T_TYPEANN)
		return pstrdup("type annotation");
	if (!k->s || k->len <= 0)
		return pstrdup("token");
	{
		/*
		 * Cut on a character boundary, so the message is valid in the server
		 * encoding. A token that is part of a multibyte character (the lexer
		 * splits a non-ASCII byte off as its own token) shows the whole
		 * character; the body holds it in full.
		 */
		int			n = pg_mbcliplen(k->s, k->len, Min(k->len, 40));

		if (n == 0)
			n = pg_mblen(k->s);
		return psprintf("'%.*s'", n, k->s);
	}
}

/* ------------------------------------------------------------------ types */

/* lower-cased type name without a modifier or array suffix */
static char *
typ_base(const char *t)
{
	char	   *b;
	int			n = 0;

	while (*t == ' ')
		t++;
	b = palloc(strlen(t) + 1);
	while (*t && *t != '(' && *t != '[')
		b[n++] = pg_tolower((unsigned char) *t++);
	while (n > 0 && b[n - 1] == ' ')
		n--;
	b[n] = '\0';
	return b;
}

static bool
typ_is_array(const char *t)
{
	return t && (strchr(t, '[') != NULL || strstr(t, " ARRAY") != NULL ||
				 strstr(t, " array") != NULL);
}

static bool
typ_is_text(const char *t)
{
	char	   *b;

	if (!t || typ_is_array(t))
		return false;
	b = typ_base(t);
	return strcmp(b, "text") == 0 || strncmp(b, "varchar", 7) == 0 ||
		strncmp(b, "character", 9) == 0 || strncmp(b, "char", 4) == 0 ||
		strcmp(b, "bpchar") == 0 || strcmp(b, "name") == 0 ||
		strcmp(b, "citext") == 0;
}

static bool
typ_is_bool(const char *t)
{
	char	   *b;

	if (!t || typ_is_array(t))
		return false;
	b = typ_base(t);
	return strcmp(b, "boolean") == 0 || strcmp(b, "bool") == 0;
}

/* 1 integer, 2 bigint, 3 numeric, 4 double precision; 0 not a number */
static int
typ_rank(const char *t)
{
	char	   *b;

	if (!t || typ_is_array(t))
		return 0;
	b = typ_base(t);
	if (strcmp(b, "integer") == 0 || strcmp(b, "int") == 0 ||
		strcmp(b, "int4") == 0 || strcmp(b, "smallint") == 0 ||
		strcmp(b, "int2") == 0)
		return 1;
	if (strcmp(b, "bigint") == 0 || strcmp(b, "int8") == 0)
		return 2;
	if (strcmp(b, "numeric") == 0 || strcmp(b, "decimal") == 0)
		return 3;
	if (strcmp(b, "double precision") == 0 || strcmp(b, "float8") == 0 ||
		strcmp(b, "real") == 0 || strcmp(b, "float4") == 0 ||
		strcmp(b, "float") == 0)
		return 4;
	return 0;
}

static const char *
rank_typ(int r)
{
	switch (r)
	{
		case 1:
			return "integer";
		case 2:
			return "bigint";
		case 3:
			return "numeric";
		case 4:
			return "double precision";
		default:
			return NULL;
	}
}

static bool
typ_is_stringy(const char *t)
{
	return t && (typ_is_text(t) || strstr(t, "plx_strbuild") != NULL);
}

/* element type of an array type text ("integer[]" -> "integer") */
static char *
typ_elem(const char *t)
{
	char	   *b;
	int			n;

	if (!t || !strchr(t, '['))
		return NULL;
	b = pstrdup(t);
	n = (int) (strchr(b, '[') - b);
	while (n > 0 && b[n - 1] == ' ')
		n--;
	b[n] = '\0';
	return n > 0 ? b : NULL;
}

/* ------------------------------------------------------------- parameters */

/*
 * Index of the parameter named name, or -1. A parameter spelled exactly so
 * wins; otherwise the one that matches ignoring case, as SQL folds an
 * unquoted name (userId is the parameter userid). Two parameters that match
 * only ignoring case ("X" and "x" for the name X1) are ambiguous.
 */
static int
lua_param(Ctx *cx, const char *name, int len)
{
	int			i,
				found = -1,
				nfound = 0;

	if (!cx->meta || !cx->meta->argnames)
		return -1;
	for (i = 0; i < cx->meta->nargs; i++)
	{
		const char *an = cx->meta->argnames[i];

		if (!an || !an[0] || (int) strlen(an) != len)
			continue;
		if (strncmp(an, name, len) == 0)
			return i;
		if (pg_strncasecmp(an, name, len) == 0)
		{
			found = i;
			nfound++;
		}
	}
	if (nfound > 1)
		plx_err(cx, cx->t[cx->pos].line,
				"\"%.*s\" matches more than one parameter that differ only in case; spell it as the parameter is declared",
				len, name);
	return found;
}

/*
 * Does any parameter have the plpgsql name of name (ignoring case)? *same is
 * set when one of them is spelled exactly lname. Never raises: a new name
 * only has to avoid the parameters, not choose between them.
 */
static bool
lua_param_ci(Ctx *cx, const char *name, int len, const char *lname, bool *same)
{
	int			i;
	bool		any = false;

	*same = false;
	if (!cx->meta || !cx->meta->argnames)
		return false;
	for (i = 0; i < cx->meta->nargs; i++)
	{
		const char *an = cx->meta->argnames[i];

		if (an && an[0] && (int) strlen(an) == len && pg_strncasecmp(an, name, len) == 0)
		{
			any = true;
			if (strcmp(an, lname) == 0)
				*same = true;
		}
	}
	return any;
}

/* the parameter named name, as SQL text: its catalog name, quoted if needed */
static char *
lua_param_ref(Ctx *cx, const char *name, int len)
{
	int			i = lua_param(cx, name, len);

	return pstrdup(quote_identifier(cx->meta->argnames[i]));
}

static Oid
lua_param_oid(Ctx *cx, int i)
{
	if (i < 0 || !cx->meta || !cx->meta->argtypes)
		return InvalidOid;
	return cx->meta->argtypes[i];
}

/* the parameter's type as a declarable type name, or NULL */
static const char *
lua_param_typ(Ctx *cx, int i)
{
	Oid			o = lua_param_oid(cx, i);

	if (!OidIsValid(o) || IsPolymorphicType(o) || o == RECORDOID ||
		get_typtype(o) == TYPTYPE_PSEUDO)
		return NULL;
	return format_type_be(o);
}

/* the OUT (and INOUT, TABLE) parameters: their count, filling idx[] */
static int
lua_outs(Ctx *cx, int *idx, int max)
{
	int			i,
				n = 0;

	if (!cx->meta || !cx->meta->argmodes)
		return 0;
	for (i = 0; i < cx->meta->nargs; i++)
	{
		char		m = cx->meta->argmodes[i];

		if (m == 'o' || m == 'b' || m == 't')
		{
			if (n < max)
				idx[n] = i;
			n++;
		}
	}
	return n;
}

static const char *
lua_param_name(Ctx *cx, int i)
{
	const char *an = cx->meta && cx->meta->argnames ? cx->meta->argnames[i] : NULL;

	if (!an || !an[0])
		plx_err(cx, cx->t[cx->pos].line,
				"OUT parameter %d has no name, so it cannot be assigned", i + 1);
	return an;
}

/* ---------------------------------------------------------- bindings */

static HTAB *
lua_htab(const char *nm, Size entsize)
{
	HASHCTL		ctl;

	ctl.keysize = NAMEDATALEN;
	ctl.entrysize = entsize;
	ctl.hcxt = CurrentMemoryContext;
#if PG_VERSION_NUM >= 140000
	return hash_create(nm, 64, &ctl, HASH_ELEM | HASH_STRINGS | HASH_CONTEXT);
#else
	return hash_create(nm, 64, &ctl, HASH_ELEM | HASH_CONTEXT);
#endif
}

static void
lua_key_exact(const char *name, int len, char *key)
{
	memset(key, 0, NAMEDATALEN);
	memcpy(key, name, Min(len, NAMEDATALEN - 1));
}

/* the key plpgsql itself uses for an unquoted identifier */
static void
lua_key_pg(const char *name, char *key)
{
	char	   *k = downcase_truncate_identifier(name, (int) strlen(name), false);

	memset(key, 0, NAMEDATALEN);
	strlcpy(key, k, NAMEDATALEN);
	pfree(k);
}

static LuaBind *
lua_lookup(Lua *L, const char *name, int len)
{
	char		key[NAMEDATALEN];
	LuaEnt	   *e;

	if (len <= 0 || len >= NAMEDATALEN)
		return NULL;
	lua_key_exact(name, len, key);
	e = hash_search(L->names, key, HASH_FIND, NULL);
	return e ? e->top : NULL;
}

static LuaBind *
lua_pg_visible(Lua *L, const char *pgname)
{
	char		key[NAMEDATALEN];
	LuaEnt	   *e;

	lua_key_pg(pgname, key);
	e = hash_search(L->pgnames, key, HASH_FIND, NULL);
	return e ? e->top : NULL;
}

static void
lua_scope_push(Lua *L)
{
	LuaScope   *s = palloc0(sizeof(LuaScope));

	s->outer = L->scope;
	s->level = L->scope ? L->scope->level + 1 : 0;
	L->scope = s;
}

static void
lua_scope_pop(Lua *L)
{
	LuaScope   *s = L->scope;
	LuaBind    *b;

	for (b = s->binds; b; b = b->snext)
	{
		char		key[NAMEDATALEN];
		LuaEnt	   *e;

		lua_key_exact(b->lname, (int) strlen(b->lname), key);
		e = hash_search(L->names, key, HASH_FIND, NULL);
		if (e && e->top == b)
			e->top = b->shadow;
		lua_key_pg(b->pgname, key);
		e = hash_search(L->pgnames, key, HASH_FIND, NULL);
		if (e && e->top == b)
			e->top = b->pgshadow;
	}
	L->scope = s->outer;
}

/* make a binding visible in scope s */
static void
lua_activate(Lua *L, LuaBind *b, LuaScope *s)
{
	char		key[NAMEDATALEN];
	LuaEnt	   *e;
	bool		found;

	lua_key_exact(b->lname, (int) strlen(b->lname), key);
	e = hash_search(L->names, key, HASH_ENTER, &found);
	if (!found)
		e->top = NULL;
	b->shadow = e->top;
	e->top = b;
	lua_key_pg(b->pgname, key);
	e = hash_search(L->pgnames, key, HASH_ENTER, &found);
	if (!found)
		e->top = NULL;
	b->pgshadow = e->top;
	e->top = b;
	b->level = s->level;
	b->snext = s->binds;
	s->binds = b;
}

/* may an existing local hold a variable of this type? */
static bool
lua_compatible(PlxLocal2 *l, const char *typ, bool rec)
{
	if (l->is_const)
		return false;
	if (rec)
		return l->is_record || !l->typ;
	if (l->is_record)
		return !typ;
	if (!typ || !l->typ)
		return true;
	return pg_strcasecmp(l->typ, typ) == 0;
}

/*
 * A name plpgsql cannot take as a variable: a reserved or type/function-name
 * SQL keyword (user, order, left, ...) or a plpgsql reserved word (loop, by,
 * strict, ...). A Lua local with such a name gets a different plpgsql name.
 */
static bool
lua_bad_name(const char *name)
{
	static const char *const plreserved[] = {
		"all", "begin", "by", "case", "declare", "else", "end", "execute",
		"for", "foreach", "from", "if", "in", "into", "loop", "not", "null",
		"or", "strict", "then", "to", "using", "when", "while", NULL
	};
	int			kw = ScanKeywordLookup(name, &ScanKeywords);
	int			i;

	if (kw >= 0 && (ScanKeywordCategories[kw] == RESERVED_KEYWORD ||
					ScanKeywordCategories[kw] == TYPE_FUNC_NAME_KEYWORD))
		return true;
	for (i = 0; plreserved[i]; i++)
		if (pg_strcasecmp(name, plreserved[i]) == 0)
			return true;
	return false;
}

static bool
lua_in_list(char **list, int n, const char *name)
{
	int			i;

	for (i = 0; i < n; i++)
		if (pg_strcasecmp(list[i], name) == 0)
			return true;
	return false;
}

/*
 * Choose the plpgsql variable for a new Lua local named name, in scope s.
 *
 * The plain name is used when nothing visible or earlier gets in the way. A
 * variable of the same name declared earlier in the same scope, or in a scope
 * that has ended, is reused when its type agrees (the old one can no longer
 * be reached). Otherwise, because plpgsql has one flat namespace and folds
 * case, a new name is made: for shadowing a variable of an enclosing scope or
 * a parameter, for a different type, or for a name that differs only in case.
 *
 * A FOR variable is declared by plpgsql's FOR itself and scoped to its loop,
 * so it only has to avoid names that differ from it in Lua but not in
 * plpgsql. avoid lists plpgsql variables that must not be reused (read by the
 * right-hand side of the same multiple assignment).
 */
static LuaBind *
lua_declare(Lua *L, const char *name, int len, const char *typ, bool rec,
			bool forvar, LuaScope *s, char **avoid, int navoid,
			int line, bool *isnew)
{
	Ctx		   *cx = L->cx;
	char	   *lname;
	char	   *pg = NULL;
	PlxLocal2  *loc = NULL;
	int			k;

	*isnew = false;
	if (len >= NAMEDATALEN)
		plx_err(cx, line, "identifier \"%.*s...\" is too long", 20, name);
	if (len >= 5 && strncmp(name, "__plx", 5) == 0)
		plx_err(cx, line, "names beginning with __plx are reserved");
	lname = pnstrdup(name, len);

	for (k = 1; k <= 1000 && !pg; k++)
	{
		char	   *cand;
		LuaBind    *v;
		bool		pany,
					psame;

		CHECK_FOR_INTERRUPTS();
		if (k == 1)
			cand = lname;
		else
		{
			char		sfx[16];
			int			sl,
						bl;

			snprintf(sfx, sizeof(sfx), "_%d", k);
			sl = (int) strlen(sfx);
			bl = Min(len, NAMEDATALEN - 1 - sl);
			cand = psprintf("%.*s%s", bl, name, sfx);
		}
		if (lua_bad_name(cand))
			continue;
		v = lua_pg_visible(L, cand);
		pany = lua_param_ci(cx, cand, (int) strlen(cand), lname, &psame);
		if (forvar)
		{
			if (v && strcmp(v->lname, lname) != 0)
				continue;
			if (pany && !psame)
				continue;
			pg = cand;
			break;
		}
		if (v)
		{
			if (strcmp(v->lname, lname) == 0 && v->level == s->level && v->loc &&
				lua_compatible(v->loc, typ, rec) &&
				!lua_in_list(avoid, navoid, cand))
			{
				pg = cand;
				loc = v->loc;
			}
			continue;
		}
		if (pany)
			continue;
		loc = plx_local_find(cx, cand, (int) strlen(cand));
		if (loc)
		{
			if (lua_compatible(loc, typ, rec) && !lua_in_list(avoid, navoid, cand))
				pg = loc->name;		/* spelled as it was declared */
			else
				loc = NULL;
			continue;
		}
		pg = cand;
	}
	if (!pg)
		plx_err(cx, line, "too many declarations of \"%s\"", lname);

	{
		LuaBind    *b = palloc0(sizeof(LuaBind));

		b->lname = lname;
		b->pgname = pg;
		if (forvar)
			b->typ = "integer";
		else
		{
			if (!loc)
			{
				loc = plx_local_add(cx, pg, (int) strlen(pg));
				*isnew = true;
			}
			if (rec && !loc->typ)
				loc->is_record = true;
			else if (typ && !loc->typ && !loc->is_record)
				loc->typ = pstrdup(typ);
			b->loc = loc;
		}
		return b;
	}
}

static const char *
lua_bind_typ(LuaBind *b)
{
	return b->loc ? b->loc->typ : b->typ;
}

/* a generated local, __plx_<what>_<n> */
static char *
lua_hidden(Lua *L, const char *what, const char *typ)
{
	char	   *nm = psprintf("__plx_%s_%d", what, ++L->seq);
	PlxLocal2  *l = plx_local_add(L->cx, nm, (int) strlen(nm));

	l->typ = pstrdup(typ);
	return nm;
}

static void
lua_ref(Lua *L, const char *name)
{
	if (!L->collect)
		return;
	if (L->nrefs >= L->refcap)
	{
		L->refcap = L->refcap ? L->refcap * 2 : 16;
		L->refs = L->refs ? repalloc(L->refs, L->refcap * sizeof(char *))
			: palloc(L->refcap * sizeof(char *));
	}
	L->refs[L->nrefs++] = pstrdup(name);
}

static void
lua_refs_begin(Lua *L)
{
	L->collect = true;
	L->nrefs = 0;
	L->refcap = 0;
	L->refs = NULL;
}

static char **
lua_refs_take(Lua *L, int *n)
{
	char	  **r = L->refs;

	*n = L->nrefs;
	L->collect = false;
	L->refs = NULL;
	L->nrefs = L->refcap = 0;
	return r;
}

static LuaErrVar *
lua_errvar(Lua *L, const char *pgname)
{
	char		key[NAMEDATALEN];
	LuaErrEnt  *e;
	bool		found;

	lua_key_pg(pgname, key);
	e = hash_search(L->errhash, key, HASH_ENTER, &found);
	if (!found)
	{
		LuaErrVar  *ev = palloc0(sizeof(LuaErrVar));

		if (L->nerr >= L->errcap)
		{
			L->errcap = L->errcap ? L->errcap * 2 : 8;
			L->errv = L->errv ? repalloc(L->errv, L->errcap * sizeof(LuaErrVar *))
				: palloc(L->errcap * sizeof(LuaErrVar *));
		}
		ev->id = L->nerr + 1;
		L->errv[L->nerr++] = ev;
		e->ev = ev;
	}
	return e->ev;
}

static char *
lua_errf_name(int id, int f)
{
	return psprintf("__plx_e%d_%s", id, lua_errf[f].f);
}

/* ---------------------------------------------------------- string values */

static void
lua_put_byte(Ctx *cx, StringInfo b, int v, int line)
{
	if (v == 0)
		plx_err(cx, line, "a string cannot contain a NUL character in PostgreSQL");
	appendStringInfoChar(b, (char) v);
}

/* The value of a Lua string token: "..." or '...' with Lua's escapes, or a
 * long string [[...]] / [==[...]==]. */
static char *
lua_str_decode(Ctx *cx, const char *s, int len, int line)
{
	StringInfoData b;
	const char *p,
			   *e = s + len;

	initStringInfo(&b);
	if (len >= 2 && s[0] == '[')
	{
		int			lvl = 0,
					k;
		const char *ce;

		p = s + 1;
		while (p < e && *p == '=')
		{
			lvl++;
			p++;
		}
		if (p >= e || *p != '[')
			plx_err(cx, line, "malformed long string");
		p++;
		if (e - p < lvl + 2)
			plx_err(cx, line, "unfinished long string");
		ce = e - (lvl + 2);
		if (ce[0] != ']' || ce[lvl + 1] != ']')
			plx_err(cx, line, "unfinished long string");
		for (k = 1; k <= lvl; k++)
			if (ce[k] != '=')
				plx_err(cx, line, "unfinished long string");
		/* a line break right after the opening bracket is not content */
		if (p < ce && (*p == '\n' || *p == '\r'))
		{
			char		c = *p++;

			if (p < ce && (*p == '\n' || *p == '\r') && *p != c)
				p++;
		}
		while (p < ce)
		{
			if (*p == '\n' || *p == '\r')
			{
				char		c = *p++;

				if (p < ce && (*p == '\n' || *p == '\r') && *p != c)
					p++;
				appendStringInfoChar(&b, '\n');
				continue;
			}
			appendStringInfoChar(&b, *p++);
		}
	}
	else if (len >= 1 && (s[0] == '"' || s[0] == '\''))
	{
		char		q = s[0];
		bool		closed = false;

		p = s + 1;
		while (p < e)
		{
			char		c = *p;

			if (c == q)
			{
				closed = (p == e - 1);
				break;
			}
			if (c == '\n' || c == '\r')
				plx_err(cx, line, "unfinished string (a line break must be written \\n)");
			if (c != '\\')
			{
				appendStringInfoChar(&b, c);
				p++;
				continue;
			}
			p++;
			if (p >= e)
				break;
			c = *p++;
			switch (c)
			{
				case 'a':
					appendStringInfoChar(&b, '\a');
					break;
				case 'b':
					appendStringInfoChar(&b, '\b');
					break;
				case 'f':
					appendStringInfoChar(&b, '\f');
					break;
				case 'n':
					appendStringInfoChar(&b, '\n');
					break;
				case 'r':
					appendStringInfoChar(&b, '\r');
					break;
				case 't':
					appendStringInfoChar(&b, '\t');
					break;
				case 'v':
					appendStringInfoChar(&b, '\v');
					break;
				case '\\':
				case '"':
				case '\'':
					appendStringInfoChar(&b, c);
					break;
				case '\n':
				case '\r':
					appendStringInfoChar(&b, '\n');
					if (p < e && (*p == '\n' || *p == '\r') && *p != c)
						p++;
					break;
				case 'z':
					while (p < e && (*p == ' ' || *p == '\t' || *p == '\n' ||
									 *p == '\r' || *p == '\v' || *p == '\f'))
						p++;
					break;
				case 'x':
					{
						int			v = 0,
									k;

						for (k = 0; k < 2; k++)
						{
							int			d = (p < e) ? hexval(*p) : -1;

							if (d < 0)
								plx_err(cx, line, "hexadecimal digit expected in a \\x escape");
							v = v * 16 + d;
							p++;
						}
						lua_put_byte(cx, &b, v, line);
						break;
					}
				case 'u':
					{
						uint32		cp = 0;
						int			nd = 0;
						unsigned char u8[MAX_UNICODE_EQUIVALENT_STRING + 1];

						if (p >= e || *p != '{')
							plx_err(cx, line, "missing '{' in a \\u{xxxx} escape");
						p++;
						while (p < e && hexval(*p) >= 0)
						{
							cp = cp * 16 + hexval(*p);
							if (cp > 0x10FFFF)
								plx_err(cx, line, "UTF-8 value too large in a \\u escape");
							p++;
							nd++;
						}
						if (nd == 0 || p >= e || *p != '}')
							plx_err(cx, line, "malformed \\u{xxxx} escape");
						p++;
						if (cp == 0)
							plx_err(cx, line, "a string cannot contain a NUL character in PostgreSQL");
						if (cp >= 0xD800 && cp <= 0xDFFF)
							plx_err(cx, line, "a \\u escape cannot name a surrogate");
						/* the character in the database encoding, as plxcsharp does */
						memset(u8, 0, sizeof(u8));
						pg_unicode_to_server((pg_wchar) cp, u8);
						appendStringInfoString(&b, (char *) u8);
						break;
					}
				default:
					if (c >= '0' && c <= '9')
					{
						int			v = c - '0',
									k;

						for (k = 1; k < 3 && p < e && *p >= '0' && *p <= '9'; k++)
							v = v * 10 + (*p++ - '0');
						if (v > 255)
							plx_err(cx, line, "decimal escape too large");
						lua_put_byte(cx, &b, v, line);
					}
					else
						plx_err(cx, line, "invalid escape sequence '\\%c'",
								(c >= 32 && c < 127) ? c : '?');
			}
		}
		if (!closed)
			plx_err(cx, line, "unfinished string");
	}
	else
		plx_err(cx, line, "malformed string");

	if (!pg_verifymbstr(b.data, b.len, true))
		plx_err(cx, line, "string literal is not valid in the database encoding");
	return b.data;
}

/* A string value as a SQL literal, E'' when it holds a backslash or a
 * control character. */
static char *
lua_sql_literal(const char *v)
{
	StringInfoData o;
	const unsigned char *c;
	bool		esc = false;

	for (c = (const unsigned char *) v; *c; c++)
		if (*c == '\\' || *c < 0x20 || *c == 0x7f)
		{
			esc = true;
			break;
		}
	initStringInfo(&o);
	if (esc)
		appendStringInfoChar(&o, 'E');
	appendStringInfoChar(&o, '\'');
	for (c = (const unsigned char *) v; *c; c++)
	{
		if (*c == '\'')
			appendStringInfoString(&o, "''");
		else if (!esc)
			appendStringInfoChar(&o, (char) *c);
		else if (*c == '\\')
			appendStringInfoString(&o, "\\\\");
		else if (*c == '\n')
			appendStringInfoString(&o, "\\n");
		else if (*c == '\t')
			appendStringInfoString(&o, "\\t");
		else if (*c == '\r')
			appendStringInfoString(&o, "\\r");
		else if (*c == '\b')
			appendStringInfoString(&o, "\\b");
		else if (*c == '\f')
			appendStringInfoString(&o, "\\f");
		else if (*c < 0x20 || *c == 0x7f)
			appendStringInfo(&o, "\\x%02X", *c);
		else
			appendStringInfoChar(&o, (char) *c);
	}
	appendStringInfoChar(&o, '\'');
	return o.data;
}

/* PlxSurface.str_value: the SQL text a string literal holds */
static char *
lua_str_value_hook(Ctx *cx, const char *tok, int len)
{
	int			line = (cx->t && cx->pos >= 0) ? cx->t[cx->pos].line : 0;
	char	   *v = lua_str_decode(cx, tok, len, line);

	if (strchr(v, '\x01') || strchr(v, LUA_MARK))
		plx_err(cx, line, "SQL text cannot contain the control characters \\1 or \\2");
	return v;
}

/* ------------------------------------------------------- translated values */

static LuaV *
lv_new(char *s, int prec)
{
	LuaV	   *v = palloc0(sizeof(LuaV));

	v->s = s;
	v->prec = prec;
	return v;
}

static char *
lv_wrap(LuaV *v, bool cond)
{
	return cond ? psprintf("(%s)", v->s) : v->s;
}

static bool
lv_atomic(LuaV *v)
{
	return v->prec >= SP_ATOM && !v->neg;
}

/* v cast to a type: x::t, or (x)::t */
static char *
lv_cast(LuaV *v, const char *t)
{
	return psprintf(lv_atomic(v) ? "%s::%s" : "(%s)::%s", v->s, t);
}

static bool
lv_known_text(LuaV *v)
{
	return v->istext || typ_is_text(v->typ);
}

/* v as a text argument of a function call */
static char *
lv_text_arg(LuaV *v)
{
	if (lv_known_text(v))
		return v->s;
	return lv_cast(v, "text");
}

/* a literal that Lua considers true (anything but nil and false) */
static bool
lv_truthy_lit(LuaV *v)
{
	return v->lit && !v->isnil && !v->isfalse;
}

/* known to hold a value that is not a boolean */
static bool
lv_nonbool(LuaV *v)
{
	if (v->isbool || v->rec || v->isnil)
		return false;
	return v->istext || v->tbl || (v->typ && !typ_is_bool(v->typ)) ||
		(v->lit && !v->isfalse);
}

/*
 * Append sep and more to a growing text field (cur, which is (*buf)->data
 * once the buffer exists), first dropping trim bytes from its end. Returns
 * the new text. Amortized cost is the length of what is appended.
 */
static char *
lv_app(StringInfo *buf, char *cur, int trim, const char *sep, const char *more)
{
	if (!*buf)
	{
		*buf = makeStringInfo();
		appendStringInfoString(*buf, cur);
	}
	if (trim > 0 && (*buf)->len >= trim)
	{
		(*buf)->len -= trim;
		(*buf)->data[(*buf)->len] = '\0';
	}
	appendStringInfoString(*buf, sep);
	appendStringInfoString(*buf, more);
	return (*buf)->data;
}

static int
lv_len(StringInfo buf, const char *s)
{
	return buf ? buf->len : (int) strlen(s);
}

static LuaV *
lv_paren(LuaV *in)
{
	LuaV	   *v = palloc(sizeof(LuaV));

	*v = *in;
	v->s = psprintf("(%s)", in->s);
	v->prec = SP_ATOM;
	v->neg = false;
	v->chain = true;
	v->op = NULL;
	/* a copy must not grow the original's buffers */
	v->sbuf = v->bbuf = NULL;
	v->and_plen = 0;
	v->case_open = false;		/* (a and x or b and y) is complete */
	v->coalesce = false;
	v->nest = in->nest + 1;
	if (in->and_plen)
	{
		/* keep the chained 'and' operand reachable for 'a and b or c' */
		LuaV	   *a = lv_new(pnstrdup(in->bs, in->and_plen), SP_AND);

		a->isbool = true;
		v->and_a = a;
	}
	if (in->bs)
	{
		v->bs = psprintf("(%s)", in->bs);
		v->bprec = SP_ATOM;
	}
	return v;
}

/* ------------------------------------------------------ expression cursor */

static Tok *
x_peek(LuaX *x)
{
	while (x->p < x->e && x->t[x->p].kind == T_NEWLINE)
		x->p++;
	return (x->p < x->e) ? &x->t[x->p] : &lua_eof;
}

static int
x_line(LuaX *x)
{
	int			i = (x->p < x->e) ? x->p : x->e - 1;

	if (i < 0)
		i = 0;
	return x->t[i].line + x->loff;
}

pg_noreturn static void x_err(LuaX *x, const char *fmt,...) pg_attribute_printf(2, 3);

static void
x_err(LuaX *x, const char *fmt,...)
{
	char		buf[512];
	va_list		ap;

	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	plx_err(x->cx, x_line(x), "%s", buf);
}

static bool
x_callstart(Tok *k)
{
	return k->kind == T_LPAREN || k->kind == T_STRING || k->kind == T_LBRACE;
}

static int
x_binop(Tok *k)
{
	if (k->kind == T_KW)
		return k->kw == KW_OR ? OP_OR : k->kw == KW_AND ? OP_AND : OP_NONE;
	if (k->kind != T_OP)
		return OP_NONE;
	if (k->len == 1)
		switch (k->s[0])
		{
			case '<':
				return OP_LT;
			case '>':
				return OP_GT;
			case '|':
				return OP_BOR;
			case '~':
				return OP_BXOR;
			case '&':
				return OP_BAND;
			case '+':
				return OP_ADD;
			case '-':
				return OP_SUB;
			case '*':
				return OP_MUL;
			case '/':
				return OP_DIV;
			case '%':
				return OP_MOD;
			case '^':
				return OP_POW;
			default:
				return OP_NONE;
		}
	if (plx_tok_is(k, "=="))
		return OP_EQ;
	if (plx_tok_is(k, "~="))
		return OP_NE;
	if (plx_tok_is(k, "<="))
		return OP_LE;
	if (plx_tok_is(k, ">="))
		return OP_GE;
	if (plx_tok_is(k, "<<"))
		return OP_SHL;
	if (plx_tok_is(k, ">>"))
		return OP_SHR;
	if (plx_tok_is(k, ".."))
		return OP_CONCAT;
	if (plx_tok_is(k, "//"))
		return OP_IDIV;
	return OP_NONE;
}

/* ---------------------------------------------------------------- atoms */

static LuaV *
x_number(LuaX *x, Tok *k)
{
	char	   *txt = pnstrdup(k->s, k->len);
	LuaV	   *v;
	const char *typ = "numeric";

	if (strchr(txt, '_'))
		x_err(x, "malformed number '%s'", txt);
	if (k->kind == T_INT)
	{
		if (txt[0] == '0' && (txt[1] == 'x' || txt[1] == 'X'))
		{
			uint64		val = 0;
			int			nd = 0;
			const char *c;

			for (c = txt + 2; *c; c++)
			{
				int			d = hexval(*c);

				if (d < 0)
					x_err(x, "malformed number '%s'", txt);
				if (val > (PG_UINT64_MAX >> 4))
					x_err(x, "integer literal out of range: %s", txt);
				val = val * 16 + d;
				nd++;
			}
			if (nd == 0 || val > (uint64) PG_INT64_MAX)
				x_err(x, "integer literal out of range: %s", txt);
			txt = psprintf(UINT64_FORMAT, val);
		}
		else if (txt[0] == '0' && txt[1] && !(txt[1] >= '0' && txt[1] <= '9'))
			x_err(x, "malformed number '%s'", txt);
		/* a decimal integer: integer, bigint, or (past 64 bits) numeric */
		{
			const char *c = txt;
			int			nd;

			while (*c == '0' && c[1])
				c++;
			nd = (int) strlen(c);
			if (nd < 10 || (nd == 10 && strcmp(c, "2147483647") <= 0))
				typ = "integer";
			else if (nd < 19 || (nd == 19 && strcmp(c, "9223372036854775807") <= 0))
				typ = "bigint";
			else
				typ = "numeric";
		}
	}
	else
	{
		int			l = (int) strlen(txt);
		char		last = l > 0 ? txt[l - 1] : '\0';

		if (last == 'e' || last == 'E' || last == '+' || last == '-' ||
			strchr(txt, 'x') || strchr(txt, 'X'))
			x_err(x, "malformed number '%s'", txt);
	}
	v = lv_new(txt, SP_ATOM);
	v->typ = typ;
	v->lit = v->simple = v->chain = true;
	return v;
}

static LuaV *
x_string(LuaX *x, Tok *k)
{
	char	   *val = lua_str_decode(x->cx, k->s, k->len, k->line + x->loff);
	LuaV	   *v = lv_new(lua_sql_literal(val), SP_ATOM);

	v->strval = val;
	v->typ = "text";
	v->istext = v->lit = v->simple = true;
	return v;
}

static LuaV *
x_simple(LuaX *x)
{
	Tok		   *k = x_peek(x);
	LuaV	   *v;

	switch (k->kind)
	{
		case T_INT:
		case T_FLOAT:
			x->p++;
			return x_number(x, k);
		case T_STRING:
			x->p++;
			return x_string(x, k);
		case T_LBRACE:
			return x_table(x);
		case T_KW:
			if (k->kw == KW_NIL)
			{
				x->p++;
				v = lv_new(pstrdup("NULL"), SP_ATOM);
				v->lit = v->isnil = v->simple = true;
				return v;
			}
			if (k->kw == KW_TRUE || k->kw == KW_FALSE)
			{
				x->p++;
				v = lv_new(pstrdup(k->kw == KW_TRUE ? "true" : "false"), SP_ATOM);
				v->typ = "boolean";
				v->lit = v->isbool = v->simple = true;
				v->isfalse = (k->kw == KW_FALSE);
				return v;
			}
			if (k->kw == KW_DEF)
				x_err(x, "functions are not supported (only the pcall(function() ... end) form)");
			x_err(x, "unexpected %s in an expression", tok_desc(k));
		case T_OP:
			if (plx_tok_is(k, "..."))
				x_err(x, "varargs (...) are not supported");
			break;
		case T_TYPEANN:
			x_err(x, "a type annotation is only allowed after a local declaration");
		default:
			break;
	}
	return NULL;				/* caller parses a suffixed expression */
}

/* {e1, e2, ...} -> ARRAY[...]; keyed fields (maps) are rejected */
static LuaV *
x_table(LuaX *x)
{
	StringInfoData o;
	int			n = 0;
	const char *et = NULL;
	bool		same = true,
				alllit = true;
	LuaV	   *v;

	x->p++;						/* { */
	initStringInfo(&o);
	for (;;)
	{
		Tok		   *k = x_peek(x);
		LuaV	   *el;

		if (k->kind == T_RBRACE)
			break;
		if (k->kind == T_LBRACKET ||
			(k->kind == T_IDENT && x->p + 1 < x->e &&
			 plx_tok_is(&x->t[x->p + 1], "=")))
			x_err(x, "a table with keys is a map, which is not supported; only an array constructor {a, b, c} is");
		el = x_subexpr(x, 0);
		if (n)
			appendStringInfoString(&o, ", ");
		appendStringInfoString(&o, el->s);
		if (!el->lit)
			alllit = false;
		if (n == 0)
			et = el->typ;
		else if (!el->isnil && (!et || !el->typ || pg_strcasecmp(et, el->typ) != 0))
		{
			int			r1 = typ_rank(et),
						r2 = typ_rank(el->typ);

			if (r1 && r2)
				et = rank_typ(Max(r1, r2));
			else
				same = false;
		}
		n++;
		k = x_peek(x);
		if (k->kind == T_COMMA || k->kind == T_SEMI)
		{
			x->p++;
			continue;
		}
		if (k->kind == T_RBRACE)
			break;
		x_err(x, "'}' expected in a table constructor, found %s", tok_desc(k));
	}
	x->p++;						/* } */
	if (n == 0)
	{
		v = lv_new(pstrdup("'{}'"), SP_ATOM);
		v->tbl = v->lit = v->simple = true;
		return v;
	}
	v = lv_new(psprintf("ARRAY[%s]", o.data), SP_ATOM);
	v->tbl = true;
	v->lit = alllit;
	if (same && et)
		v->typ = typ_is_array(et) ? pstrdup(et) : psprintf("%s[]", et);
	return v;
}

/* ------------------------------------------------- string library helpers */

/* is v an integer literal? (value in *out) */
static bool
lv_intlit(LuaV *v, int64 *out)
{
	const char *c;
	bool		neg = false;
	int64		r = 0;

	if (!v->lit || typ_rank(v->typ) < 1 || typ_rank(v->typ) > 2)
		return false;
	c = v->s;
	if (*c == '-')
	{
		neg = true;
		c++;
	}
	for (; *c; c++)
	{
		if (*c < '0' || *c > '9' || r > PG_INT64_MAX / 10 - 10)
			return false;
		r = r * 10 + (*c - '0');
	}
	*out = neg ? -r : r;
	return true;
}

/* s:sub(i [, j]) with Lua's index rules (negative counts from the end) */
static LuaV *
x_sub(LuaX *x, LuaV *s, LuaV *i, LuaV *j)
{
	int64		ii = 0,
				jj = -1;
	bool		ik = lv_intlit(i, &ii),
				jk = j ? lv_intlit(j, &jj) : true;
	char	   *st = lv_text_arg(s);
	char	   *r = NULL;
	LuaV	   *v;

	if (ik && jk)
	{
		if (ii == 0)
			ii = 1;
		if (ii > 0 && jj == -1)
			r = psprintf("substr(%s, " INT64_FORMAT ")", st, ii);
		else if (ii > 0 && jj > 0)
			r = (jj >= ii) ? psprintf("substr(%s, " INT64_FORMAT ", " INT64_FORMAT ")",
									  st, ii, jj - ii + 1)
				: psprintf("substr(%s, 1, 0)", st);
		else if (ii < 0 && jj == -1)
			r = psprintf("right(%s, " INT64_FORMAT ")", st, -ii);
	}
	if (!r)
	{
		/* general case: i' = max(i < 0 ? len + i + 1 : i, 1),
		 * j' = j < 0 ? len + j + 1 : j, substr(s, i', max(j' - i' + 1, 0)) */
		bool		inl = s->simple && i->simple && (!j || j->simple);
		const char *S = inl ? st : "__plx_s";
		const char *I = inl ? lv_wrap(i, i->prec < SP_ATOM) : "__plx_i";
		const char *J = inl ? (j ? lv_wrap(j, j->prec < SP_ATOM) : "-1") : "__plx_j";
		char	   *ip = psprintf("GREATEST(CASE WHEN %s < 0 THEN length(%s) + %s + 1 ELSE %s END, 1)",
								  I, S, I, I);
		char	   *jp = psprintf("CASE WHEN %s < 0 THEN length(%s) + %s + 1 ELSE %s END",
								  J, S, J, J);
		char	   *f = psprintf("substr(%s, %s, GREATEST(%s - %s + 1, 0))", S, ip, jp, ip);

		if (inl)
			r = f;
		else
			r = psprintf("(SELECT %s FROM (SELECT %s AS __plx_s, %s AS __plx_i, %s AS __plx_j) AS __plx_sub)",
						 f, st, i->s, j ? j->s : "-1");
	}
	v = lv_new(r, SP_ATOM);
	v->typ = "text";
	v->istext = v->call = true;
	return v;
}

/* pad a rendered string.format piece to a width without truncating it */
static char *
x_pad(LuaV *arg, const char *piece, int width, bool left)
{
	const char *fn = left ? "rpad" : "lpad";

	if (arg->simple)
		return psprintf("%s(%s, GREATEST(%d, length(%s)))", fn, piece, width, piece);
	return psprintf("(SELECT %s(__plx_v, GREATEST(%d, length(__plx_v))) FROM (SELECT %s AS __plx_v) AS __plx_f)",
					fn, width, piece);
}

/*
 * string.format with a literal format, lowered to a || chain so that a NULL
 * operand propagates the way SQL concatenation does (format() would render it
 * as an empty string). In a message each operand falls back to ''.
 */
static LuaV *
x_format(LuaX *x, LuaV *fmtv, LuaV **args, int nargs)
{
	const char *f;
	StringInfoData out,
				lit;
	int			ai = 0,
				parts = 0;
	LuaV	   *v;

	if (!fmtv->strval)
		x_err(x, "string.format needs a literal format string");
	f = fmtv->strval;
	initStringInfo(&out);
	initStringInfo(&lit);
	while (*f)
	{
		bool		left = false;
		int			width = 0,
					prec = -1;
		char		conv;
		LuaV	   *a;
		char	   *piece;

		if (*f != '%')
		{
			appendStringInfoChar(&lit, *f++);
			continue;
		}
		f++;
		if (*f == '%')
		{
			appendStringInfoChar(&lit, '%');
			f++;
			continue;
		}
		while (*f == '-' || *f == '0' || *f == '+' || *f == ' ' || *f == '#')
		{
			if (*f != '-')
				x_err(x, "string.format: the '%c' flag is not supported", *f);
			left = true;
			f++;
		}
		while (*f >= '0' && *f <= '9')
		{
			width = width * 10 + (*f++ - '0');
			if (width > 999)
				x_err(x, "string.format: width too large");
		}
		if (*f == '.')
		{
			f++;
			prec = 0;
			while (*f >= '0' && *f <= '9')
			{
				prec = prec * 10 + (*f++ - '0');
				if (prec > 999)
					x_err(x, "string.format: precision too large");
			}
		}
		conv = *f;
		if (!conv)
			x_err(x, "string.format: incomplete directive at the end of the format");
		f++;
		if (ai >= nargs)
			x_err(x, "string.format: more directives than arguments");
		a = args[ai++];
		switch (conv)
		{
			case 's':
				piece = lv_text_arg(a);
				if (prec >= 0)
					piece = psprintf("left(%s, %d)", piece, prec);
				break;
			case 'd':
			case 'i':
				if (prec >= 0)
					x_err(x, "string.format: a precision with %%%c is not supported", conv);
				if (typ_rank(a->typ) == 1 || typ_rank(a->typ) == 2)
					piece = lv_cast(a, "text");
				else
					piece = psprintf("%s::text", lv_cast(a, "bigint"));
				break;
			case 'f':
			case 'F':
				piece = psprintf("round(%s, %d)::text", lv_cast(a, "numeric"),
								 prec >= 0 ? prec : 6);
				break;
			case 'x':
			case 'X':
				if (prec >= 0)
					x_err(x, "string.format: a precision with %%%c is not supported", conv);
				piece = psprintf(conv == 'x' ? "to_hex(%s)" : "upper(to_hex(%s))",
								 lv_cast(a, "bigint"));
				break;
			case 'c':
				piece = psprintf("chr(%s)", a->s);
				break;
			default:
				x_err(x, "string.format: the %%%c directive is not supported (use %%s, %%d, %%i, %%f, %%x, %%X or %%c)",
					  (conv >= 32 && conv < 127) ? conv : '?');
		}
		if (width > 0)
			piece = x_pad(a, piece, width, left);
		if (x->diag && !a->lit)
			piece = psprintf("COALESCE(%s, '')", piece);
		if (lit.len)
		{
			if (parts++)
				appendStringInfoString(&out, " || ");
			appendStringInfoString(&out, lua_sql_literal(lit.data));
			resetStringInfo(&lit);
		}
		if (parts++)
			appendStringInfoString(&out, " || ");
		appendStringInfoString(&out, piece);
	}
	if (ai < nargs)
		x_err(x, "string.format: more arguments than directives");
	if (lit.len || parts == 0)
	{
		if (parts++)
			appendStringInfoString(&out, " || ");
		appendStringInfoString(&out, lua_sql_literal(lit.data));
	}
	v = lv_new(out.data, parts > 1 ? SP_OTHER : SP_ATOM);
	v->op = "||";
	v->typ = "text";
	v->istext = v->concat = true;
	return v;
}

/* string.<fn>(args) and s:<fn>(args); args[0] is the string */
static LuaV *
x_strfunc(LuaX *x, Tok *fn, LuaV **args, int n)
{
	LuaV	   *v = NULL;
	char	   *nm = pnstrdup(fn->s, fn->len);

	if (strcmp(nm, "upper") == 0 || strcmp(nm, "lower") == 0 ||
		strcmp(nm, "reverse") == 0 || strcmp(nm, "len") == 0)
	{
		if (n != 1)
			x_err(x, "string.%s takes one argument", nm);
		if (strcmp(nm, "len") == 0)
		{
			v = lv_new(psprintf("length(%s)", lv_text_arg(args[0])), SP_ATOM);
			v->typ = "integer";
		}
		else
		{
			v = lv_new(psprintf("%s(%s)", nm, lv_text_arg(args[0])), SP_ATOM);
			v->typ = "text";
			v->istext = true;
		}
	}
	else if (strcmp(nm, "sub") == 0)
	{
		if (n < 2 || n > 3)
			x_err(x, "string.sub takes a string, a start and an optional end");
		return x_sub(x, args[0], args[1], n == 3 ? args[2] : NULL);
	}
	else if (strcmp(nm, "rep") == 0)
	{
		if (n != 2)
			x_err(x, "string.rep takes a string and a count (a separator is not supported)");
		v = lv_new(psprintf("repeat(%s, %s)", lv_text_arg(args[0]), args[1]->s), SP_ATOM);
		v->typ = "text";
		v->istext = true;
	}
	else if (strcmp(nm, "byte") == 0)
	{
		if (n < 1 || n > 2)
			x_err(x, "string.byte takes a string and an optional position");
		/*
		 * ascii('') is 0, and text cannot hold a NUL, so 0 means there is
		 * no character at that position: Lua returns nil there.
		 */
		if (n == 1)
			v = lv_new(psprintf("NULLIF(ascii(%s), 0)", lv_text_arg(args[0])), SP_ATOM);
		else
			v = lv_new(psprintf("NULLIF(ascii(%s), 0)", x_sub(x, args[0], args[1], args[1])->s), SP_ATOM);
		v->typ = "integer";
	}
	else if (strcmp(nm, "char") == 0)
	{
		StringInfoData o;
		int			i;

		if (n < 1)
			x_err(x, "string.char needs at least one code");
		initStringInfo(&o);
		for (i = 0; i < n; i++)
			appendStringInfo(&o, "%schr(%s)", i ? " || " : "", args[i]->s);
		v = lv_new(o.data, n > 1 ? SP_OTHER : SP_ATOM);
		v->op = "||";
		v->typ = "text";
		v->istext = true;
		v->concat = (n > 1);
	}
	else if (strcmp(nm, "format") == 0)
	{
		if (n < 1)
			x_err(x, "string.format needs a format string");
		return x_format(x, args[0], args + 1, n - 1);
	}
	else if (strcmp(nm, "find") == 0 || strcmp(nm, "match") == 0 ||
			 strcmp(nm, "gmatch") == 0 || strcmp(nm, "gsub") == 0)
		x_err(x, "Lua patterns (string.%s) are not supported; use strpos(), regexp_match() or regexp_replace()", nm);
	else
		x_err(x, "string.%s is not supported", nm);
	v->call = true;
	return v;
}

/* ------------------------------------------------------------ calls */

static LuaV **
x_args(LuaX *x, int *n)
{
	Tok		   *k = x_peek(x);
	LuaV	  **a = NULL;
	int			cap = 0;

	*n = 0;
	if (k->kind == T_STRING)
	{
		a = palloc(sizeof(LuaV *));
		x->p++;
		a[0] = x_string(x, k);
		*n = 1;
		return a;
	}
	if (k->kind == T_LBRACE)
	{
		a = palloc(sizeof(LuaV *));
		a[0] = x_table(x);
		*n = 1;
		return a;
	}
	if (k->kind != T_LPAREN)
		x_err(x, "'(' expected, found %s", tok_desc(k));
	x->p++;
	if (x_peek(x)->kind == T_RPAREN)
	{
		x->p++;
		return a;
	}
	for (;;)
	{
		LuaV	   *v = x_subexpr(x, 0);

		if (*n >= cap)
		{
			cap = cap ? cap * 2 : 4;
			a = a ? repalloc(a, cap * sizeof(LuaV *)) : palloc(cap * sizeof(LuaV *));
		}
		a[(*n)++] = v;
		k = x_peek(x);
		if (k->kind == T_COMMA)
		{
			x->p++;
			continue;
		}
		if (k->kind == T_RPAREN)
		{
			x->p++;
			break;
		}
		x_err(x, "',' or ')' expected in an argument list, found %s", tok_desc(k));
	}
	return a;
}

static char *
lv_join(LuaV **a, int n)
{
	StringInfoData o;
	int			i;

	initStringInfo(&o);
	for (i = 0; i < n; i++)
	{
		if (i)
			appendStringInfoString(&o, ", ");
		appendStringInfoString(&o, a[i]->s);
	}
	return o.data;
}

static bool
lua_is_libns(Tok *k)
{
	static const char *const ns[] = {
		"string", "math", "table", "os", "io", "coroutine", "utf8", "debug",
		"package", "bit32", "_G", "_ENV", NULL
	};
	int			i;

	if (k->kind != T_IDENT)
		return false;
	for (i = 0; ns[i]; i++)
		if (plx_name_eq(k, ns[i]))
			return true;
	return false;
}

/* numeric result type of a math function of v */
static const char *
x_math_typ(LuaV *v)
{
	int			r = typ_rank(v->typ);

	return r == 3 ? "numeric" : r ? "double precision" : NULL;
}

/* string.X / math.X / table.X; x->p is after the namespace name */
static LuaV *
x_library(LuaX *x, Tok *ns)
{
	Tok		   *m;
	char	   *mn;
	LuaV	   *v = NULL;
	LuaV	  **a;
	int			n;

	if (x_peek(x)->kind != T_DOT)
		x_err(x, "\"%.*s\" is a Lua library, not a value", ns->len, ns->s);
	x->p++;
	m = x_peek(x);
	if (m->kind != T_IDENT)
		x_err(x, "a library function name is expected after '%.*s.'", ns->len, ns->s);
	x->p++;
	mn = pnstrdup(m->s, m->len);

	if (plx_name_eq(ns, "string"))
	{
		if (!x_callstart(x_peek(x)))
			x_err(x, "string.%s must be called", mn);
		a = x_args(x, &n);
		return x_strfunc(x, m, a, n);
	}
	if (plx_name_eq(ns, "math"))
	{
		if (strcmp(mn, "huge") == 0)
		{
			v = lv_new(pstrdup("'Infinity'::double precision"), SP_ATOM);
			v->typ = "double precision";
			v->lit = true;
			return v;
		}
		if (strcmp(mn, "pi") == 0)
		{
			v = lv_new(pstrdup("pi()"), SP_ATOM);
			v->typ = "double precision";
			return v;
		}
		if (strcmp(mn, "maxinteger") == 0 || strcmp(mn, "mininteger") == 0)
		{
			bool		mx = (mn[1] == 'a');

			v = lv_new(pstrdup(mx ? "9223372036854775807" : "(-9223372036854775807 - 1)"),
					   SP_ATOM);
			v->typ = "bigint";
			v->lit = mx;
			return v;
		}
		if (!x_callstart(x_peek(x)))
			x_err(x, "math.%s must be called", mn);
		a = x_args(x, &n);
		if (strcmp(mn, "floor") == 0 || strcmp(mn, "ceil") == 0 ||
			strcmp(mn, "abs") == 0 || strcmp(mn, "sqrt") == 0 ||
			strcmp(mn, "exp") == 0)
		{
			if (n != 1)
				x_err(x, "math.%s takes one argument", mn);
			v = lv_new(psprintf("%s(%s)", mn, a[0]->s), SP_ATOM);
			if (strcmp(mn, "abs") == 0)
				v->typ = typ_rank(a[0]->typ) ? a[0]->typ : NULL;
			else
				v->typ = x_math_typ(a[0]);
		}
		else if (strcmp(mn, "max") == 0 || strcmp(mn, "min") == 0)
		{
			int			i,
						r = 0;

			if (n < 1)
				x_err(x, "math.%s needs at least one argument", mn);
			for (i = 0; i < n; i++)
				r = (r < 0 || !typ_rank(a[i]->typ)) ? -1 : Max(r, typ_rank(a[i]->typ));
			v = lv_new(psprintf("%s(%s)", mn[1] == 'a' ? "GREATEST" : "LEAST", lv_join(a, n)),
					   SP_ATOM);
			v->typ = r > 0 ? rank_typ(r) : NULL;
		}
		else if (strcmp(mn, "fmod") == 0)
		{
			if (n != 2)
				x_err(x, "math.fmod takes two arguments");
			v = lv_new(psprintf("mod(%s, %s)", a[0]->s, a[1]->s), SP_ATOM);
			if (typ_rank(a[0]->typ) && typ_rank(a[1]->typ))
				v->typ = rank_typ(Max(typ_rank(a[0]->typ), typ_rank(a[1]->typ)));
		}
		else if (strcmp(mn, "log") == 0)
		{
			if (n == 1)
			{
				v = lv_new(psprintf("ln(%s)", a[0]->s), SP_ATOM);
				v->typ = x_math_typ(a[0]);
			}
			else if (n == 2)
			{
				v = lv_new(psprintf("log(%s, %s)", lv_cast(a[1], "numeric"),
									lv_cast(a[0], "numeric")), SP_ATOM);
				v->typ = "numeric";
			}
			else
				x_err(x, "math.log takes a value and an optional base");
		}
		else
			x_err(x, "math.%s is not supported", mn);
		v->call = true;
		return v;
	}
	if (plx_name_eq(ns, "table"))
	{
		if (strcmp(mn, "insert") == 0 || strcmp(mn, "remove") == 0)
			x_err(x, "table.%s is a statement, not a value", mn);
		if (strcmp(mn, "concat") != 0)
			x_err(x, "table.%s is not supported", mn);
		if (!x_callstart(x_peek(x)))
			x_err(x, "table.concat must be called");
		a = x_args(x, &n);
		if (n < 1 || n == 3 || n > 4)
			x_err(x, "table.concat takes an array, an optional separator, and an optional start and end");
		if (n == 4)
			v = lv_new(psprintf("array_to_string(%s[%s:%s], %s)",
								lv_wrap(a[0], !a[0]->chain), a[2]->s, a[3]->s, a[1]->s),
					   SP_ATOM);
		else
			v = lv_new(psprintf("array_to_string(%s, %s)", a[0]->s,
								n >= 2 ? a[1]->s : "''"), SP_ATOM);
		v->typ = "text";
		v->istext = v->call = true;
		return v;
	}
	x_err(x, "the Lua %.*s library is not available in plxlua", ns->len, ns->s);
}

/* a call of a named function that is not a variable */
static LuaV *
x_namecall(LuaX *x, Tok *nm)
{
	static const char *const stmts[] = {
		"perform", "execute", "return_query", "call", "commit", "rollback",
		"close_cursor", "move_cursor", "raise", "print", "error", "assert",
		"pcall", "xpcall", NULL
	};
	static const char *const assigned[] = {
		"fetch_one", "fetch_one!", "open_cursor", "fetch_from", "row_count", NULL
	};
	static const char *const unsupported[] = {
		"type", "select", "next", "rawget", "rawset", "rawequal", "rawlen",
		"setmetatable", "getmetatable", "require", "load", "loadstring",
		"dofile", "loadfile", "collectgarbage", "unpack", NULL
	};
	LuaV	  **a;
	LuaV	   *v;
	int			n,
				i;

	for (i = 0; stmts[i]; i++)
		if (plx_name_eq(nm, stmts[i]))
			x_err(x, "%s() is a statement, not a value", stmts[i]);
	for (i = 0; assigned[i]; i++)
		if (plx_name_eq(nm, assigned[i]))
			x_err(x, "%s() must be the whole right-hand side of an assignment, e.g. local r = %s(...)",
				  assigned[i], assigned[i]);
	for (i = 0; unsupported[i]; i++)
		if (plx_name_eq(nm, unsupported[i]))
			x_err(x, "%s() is not supported", unsupported[i]);
	if (plx_name_eq(nm, "query"))
		x_err(x, "query() is only valid as a for loop iterator: for r in query(...) do");
	if (plx_name_eq(nm, "ipairs") || plx_name_eq(nm, "pairs"))
		x_err(x, "%.*s() is only valid in a for loop", nm->len, nm->s);
	if (nm->s[nm->len - 1] == '!' || nm->s[nm->len - 1] == '?')
		x_err(x, "unexpected %s", tok_desc(nm));

	a = x_args(x, &n);
	if (plx_name_eq(nm, "tostring"))
	{
		if (n != 1)
			x_err(x, "tostring takes one argument");
		v = lv_new(lv_cast(a[0], "text"), SP_ATOM);
		v->typ = "text";
		v->istext = true;
		v->simple = a[0]->simple;
	}
	else if (plx_name_eq(nm, "tonumber"))
	{
		if (n != 1)
			x_err(x, "tonumber with a base is not supported");
		v = lv_new(lv_cast(a[0], "numeric"), SP_ATOM);
		v->typ = "numeric";
		v->simple = a[0]->simple;
	}
	else if (plx_name_eq(nm, "found"))
	{
		if (n != 0)
			x_err(x, "found() takes no arguments");
		v = lv_new(pstrdup("FOUND"), SP_ATOM);
		v->typ = "boolean";
		v->isbool = v->simple = true;
	}
	else
	{
		/* any other function is a SQL function, passed through */
		v = lv_new(psprintf("%.*s(%s)", nm->len, nm->s, lv_join(a, n)), SP_ATOM);
		v->call = true;
	}
	return v;
}

/* after a name: (. IDENT)+ followed by a call? (a schema-qualified call) */
static bool
x_qualified_call(LuaX *x)
{
	int			i = x->p;

	while (i + 1 < x->e && x->t[i].kind == T_DOT && x->t[i + 1].kind == T_IDENT)
		i += 2;
	return i > x->p && i < x->e && x_callstart(&x->t[i]);
}

/* err.field of a pcall error variable */
static LuaV *
x_errfield(LuaX *x, LuaBind *b, Tok *f)
{
	LuaV	   *v;
	int			i;

	if (plx_name_eq(f, "message"))
	{
		v = lv_new(pstrdup(b->pgname), SP_ATOM);
		v->typ = "text";
		v->istext = v->simple = v->chain = true;
		return v;
	}
	for (i = 0; i < (int) lengthof(lua_errf); i++)
		if (plx_name_eq(f, lua_errf[i].f))
		{
			x->L->errv[b->errid - 1]->mask |= (1 << i);
			v = lv_new(lua_errf_name(b->errid, i), SP_ATOM);
			v->typ = "text";
			v->istext = v->simple = v->chain = true;
			return v;
		}
	x_err(x, "unknown error field \"%.*s\" (use message, sqlstate, detail, hint, constraint, column, table, schema, datatype or context)",
		  f->len, f->s);
}

static LuaV *
x_name(LuaX *x, Tok *k, LuaBind *b)
{
	LuaV	   *v;

	if (b)
	{
		v = lv_new(pstrdup(b->pgname), SP_ATOM);
		v->typ = lua_bind_typ(b);
		v->rec = b->loc && b->loc->is_record;
	}
	else
	{
		int			pi = lua_param(x->cx, k->s, k->len);

		v = lv_new(pnstrdup(k->s, k->len), SP_ATOM);
		if (pi >= 0)
			v->typ = lua_param_typ(x->cx, pi);
		lua_ref(x->L, v->s);
		/* a parameter as the catalog spells it, quoted if needed */
		if (pi >= 0)
			v->s = pstrdup(quote_identifier(x->cx->meta->argnames[pi]));
		v->simple = v->chain = true;
		v->isbool = typ_is_bool(v->typ);
		v->istext = typ_is_text(v->typ);
		return v;
	}
	lua_ref(x->L, v->s);
	v->simple = v->chain = true;
	v->isbool = typ_is_bool(v->typ);
	v->istext = typ_is_text(v->typ);
	return v;
}

/*
 * v followed by a field (sep ".") or subscript (sep "["): extended in place
 * when v is a name chain, so a long chain costs linear time; anything else
 * is parenthesized first, which counts as one level of nesting.
 */
static LuaV *
lv_suffix(LuaX *x, LuaV *v, const char *sep, char *more, bool simple)
{
	if (!v->chain || v->prec < SP_ATOM || v->neg)
	{
		LuaV	   *w = lv_new(psprintf("(%s)", v->s), SP_ATOM);

		w->nest = v->nest + 1;
		if (w->nest > PLX_MAX_DEPTH)
			x_err(x, "expression nested too deeply");
		v = w;
	}
	else if (v->lit || v->strval || v->call || v->typ || v->coalesce ||
			 v->and_a || v->and_plen || v->bs)
	{
		/* a fresh node for the access: none of v's properties carry over */
		LuaV	   *w = lv_new(v->s, SP_ATOM);

		w->sbuf = v->sbuf;
		w->nest = v->nest;
		v = w;
	}
	v->s = lv_app(&v->sbuf, v->s, 0, sep, more);
	v->chain = true;
	v->simple = simple;
	v->isbool = v->istext = v->rec = false;
	v->typ = NULL;
	return v;
}

/* primary expression and its suffixes (.f, [i], :m(...)) */
static LuaV *
x_suffixed(LuaX *x)
{
	Tok		   *k = x_peek(x);
	LuaV	   *v;
	LuaBind    *base = NULL;

	if (k->kind == T_IDENT)
	{
		LuaBind    *b = lua_lookup(x->L, k->s, k->len);
		bool		param = (!b && lua_param(x->cx, k->s, k->len) >= 0);
		Tok		   *nx;

		x->p++;
		nx = x_peek(x);
		if (!b && !param && lua_is_libns(k))
			v = x_library(x, k);
		else if (!b && !param && x_callstart(nx))
			v = x_namecall(x, k);
		else if (!b && !param && nx->kind == T_DOT && x_qualified_call(x))
		{
			StringInfoData q;
			LuaV	  **a;
			int			n;

			initStringInfo(&q);
			appendBinaryStringInfo(&q, k->s, k->len);
			while (x->t[x->p].kind == T_DOT)
			{
				appendStringInfo(&q, ".%.*s", x->t[x->p + 1].len, x->t[x->p + 1].s);
				x->p += 2;
			}
			a = x_args(x, &n);
			v = lv_new(psprintf("%s(%s)", q.data, lv_join(a, n)), SP_ATOM);
			v->call = true;
		}
		else
		{
			if ((b || param) && x_callstart(nx))
				x_err(x, "\"%.*s\" is a variable, not a function", k->len, k->s);
			if (k->s[k->len - 1] == '!' || k->s[k->len - 1] == '?')
				x_err(x, "unexpected %s", tok_desc(k));
			v = x_name(x, k, b);
			base = b;
		}
	}
	else if (k->kind == T_LPAREN)
	{
		LuaV	   *in;

		x->p++;
		in = x_subexpr(x, 0);
		if (x_peek(x)->kind != T_RPAREN)
			x_err(x, "')' expected, found %s", tok_desc(x_peek(x)));
		x->p++;
		v = lv_paren(in);
	}
	else
		x_err(x, "unexpected %s in an expression", tok_desc(k));

	for (;;)
	{
		k = x_peek(x);
		if (k->kind == T_DOT)
		{
			Tok		   *f;

			x->p++;
			f = x_peek(x);
			if (f->kind != T_IDENT)
				x_err(x, "a field name is expected after '.'");
			x->p++;
			if (base && base->errid)
				v = x_errfield(x, base, f);
			else
				v = lv_suffix(x, v, ".", pnstrdup(f->s, f->len), v->simple);
		}
		else if (k->kind == T_LBRACKET)
		{
			LuaV	   *in;
			bool		simple = v->simple;

			x->p++;
			in = x_subexpr(x, 0);
			if (x_peek(x)->kind != T_RBRACKET)
				x_err(x, "']' expected, found %s", tok_desc(x_peek(x)));
			x->p++;
			if (in->strval)		/* r["col"] is the field r.col */
				v = lv_suffix(x, v, ".", (char *) quote_identifier(in->strval), simple);
			else
				v = lv_suffix(x, v, "[", psprintf("%s]", in->s), simple && in->simple);
		}
		else if (plx_tok_is(k, ":"))
		{
			Tok		   *m;
			LuaV	  **a,
					  **all;
			int			n;

			x->p++;
			m = x_peek(x);
			if (m->kind != T_IDENT)
				x_err(x, "a method name is expected after ':'");
			x->p++;
			if (!x_callstart(x_peek(x)))
				x_err(x, "method %.*s must be called", m->len, m->s);
			a = x_args(x, &n);
			all = palloc((n + 1) * sizeof(LuaV *));
			all[0] = v;
			if (n)
				memcpy(all + 1, a, n * sizeof(LuaV *));
			v = x_strfunc(x, m, all, n + 1);
			v->nest = all[0]->nest + 1;
			if (v->nest > PLX_MAX_DEPTH)
				x_err(x, "expression nested too deeply");
		}
		else if (x_callstart(k))
			x_err(x, "only a named function can be called");
		else
			break;
		base = NULL;
	}
	return v;
}

/* ------------------------------------------------------------ operators */

/* Lua truthiness as a SQL condition */
static LuaV *
x_tobool(LuaX *x, LuaV *v)
{
	LuaV	   *r;

	if (v->bs)
	{
		r = lv_new(v->bs, v->bprec);
		r->isbool = true;
		return r;
	}
	if (v->isbool)
		return v;
	if (v->isnil)
	{
		r = lv_new(pstrdup("false"), SP_ATOM);
		r->isbool = r->lit = r->isfalse = true;
		return r;
	}
	if (v->rec)
		x_err(x, "a record cannot be used as a condition; test found() after fetching it");
	if (v->lit)
	{
		r = lv_new(pstrdup("true"), SP_ATOM);
		r->isbool = r->lit = true;
		return r;
	}
	if (lv_nonbool(v))
	{
		r = lv_new(psprintf("%s IS NOT NULL", lv_wrap(v, v->prec < SP_OTHER)), SP_IS);
		r->isbool = true;
		return r;
	}
	return v;
}

static LuaV *
x_unary(LuaX *x, char u, LuaV *a)
{
	LuaV	   *v;

	switch (u)
	{
		case '-':
			if (a->lit && typ_rank(a->typ) && a->s[0] != '-')
			{
				v = lv_new(psprintf("-%s", a->s), SP_ATOM);
				v->typ = a->typ;
				v->lit = v->simple = v->neg = true;
				return v;
			}
			v = lv_new(psprintf("-%s", lv_wrap(a, a->prec < SP_UNARY || a->neg)), SP_UNARY);
			v->typ = typ_rank(a->typ) ? a->typ : NULL;
			v->neg = true;
			return v;
		case '~':
			v = lv_new(psprintf("~%s", lv_wrap(a, a->prec < SP_UNARY || a->neg)), SP_UNARY);
			v->typ = typ_rank(a->typ) ? a->typ : NULL;
			return v;
		case '#':
			if (a->tbl || typ_is_array(a->typ))
				v = lv_new(psprintf("cardinality(%s)", a->s), SP_ATOM);
			else
				v = lv_new(psprintf("length(%s)", a->s), SP_ATOM);
			v->typ = "integer";
			v->call = true;
			v->simple = a->simple;
			return v;
		default:				/* not */
			if (a->isnil)
			{
				v = lv_new(pstrdup("true"), SP_ATOM);
				v->isbool = v->lit = true;
				return v;
			}
			if (!a->bs && !a->isbool && !a->lit && lv_nonbool(a))
				v = lv_new(psprintf("%s IS NULL", lv_wrap(a, a->prec < SP_OTHER)), SP_IS);
			else
			{
				LuaV	   *b = x_tobool(x, a);

				v = lv_new(psprintf("NOT %s", lv_wrap(b, b->prec < SP_NOT)), SP_NOT);
			}
			v->typ = "boolean";
			v->isbool = true;
			return v;
	}
}

/* one operand of a || chain */
static char *
x_concat_piece(LuaX *x, LuaV *v)
{
	char	   *p;

	if (v->concat)
		return v->prec < SP_OTHER ? psprintf("(%s)", v->s) : v->s;
	if (lv_known_text(v))
		p = lv_wrap(v, v->prec < SP_OTHER);
	else
		p = lv_cast(v, "text");
	if (x->diag && !v->strval)
		p = psprintf("COALESCE(%s, '')", p);
	return p;
}

/* the nesting depth a combination produces; a chain that has to wrap its
 * left operand at every step is bounded like any other nesting */
static void
lv_nest(LuaX *x, LuaV *v, LuaV *l, bool lw, LuaV *r, bool rw)
{
	int			n = l->nest + (lw ? 1 : 0);

	if (r && r->nest + (rw ? 1 : 0) > n)
		n = r->nest + (rw ? 1 : 0);
	v->nest = n;
	if (n > PLX_MAX_DEPTH)
		x_err(x, "expression nested too deeply");
}

/* is v an 'and' whose right operand is a literal Lua considers true? */
static bool
lv_and_lit(LuaV *v)
{
	return (v->and_a || v->and_plen) && v->and_b && lv_truthy_lit(v->and_b);
}

/* the condition (left operand) of such an 'and' */
static const char *
lv_and_cond(LuaX *x, LuaV *v)
{
	return v->and_a ? x_tobool(x, v->and_a)->s : pnstrdup(v->bs, v->and_plen);
}

/*
 * The value an operand of 'or' stands for. An 'and' whose right operand is a
 * value that is not a boolean is that value when its condition holds, else
 * nil: Lua's  c and v  is v exactly when c is true and v is not nil.
 */
static char *
lv_value(LuaX *x, LuaV *v)
{
	if ((v->and_a || v->and_plen) && v->and_b && lv_nonbool(v->and_b))
		return psprintf("CASE WHEN %s THEN %s END", lv_and_cond(x, v), v->and_b->s);
	return v->s;
}

static LuaV *
x_logic(LuaX *x, int op, LuaV *l, LuaV *r)
{
	LuaV	   *rb = x_tobool(x, r);
	LuaV	   *lb;
	LuaV	   *v;

	if (op == OP_AND)
	{
		char	   *rt = lv_wrap(rb, rb->prec < SP_AND);

		if (l->prec == SP_AND && l->bs == l->s)
		{
			/* a and b and c: extend the chain in place */
			int			plen = lv_len(l->sbuf, l->s);

			l->s = l->bs = lv_app(&l->sbuf, l->s, 0, " AND ", rt);
			l->and_a = NULL;
			l->and_plen = plen;
			l->and_b = r;
			lv_nest(x, l, l, false, rb, rb->prec < SP_AND);
			return l;
		}
		lb = x_tobool(x, l);
		v = lv_new(psprintf("%s AND %s", lv_wrap(lb, lb->prec < SP_AND), rt), SP_AND);
		v->bs = v->s;
		v->bprec = SP_AND;
		v->isbool = true;
		v->typ = "boolean";
		v->and_a = l;
		v->and_b = r;
		lv_nest(x, v, lb, lb->prec < SP_AND, rb, rb->prec < SP_AND);
		return v;
	}

	/* or */
	if (l->case_open)
	{
		/* a and x or b and y [or c]: one more arm, or the ELSE */
		if (lv_and_lit(r))
			l->s = lv_app(&l->sbuf, l->s, 4, " WHEN ",
						  psprintf("%s THEN %s END", lv_and_cond(x, r), r->and_b->s));
		else
		{
			l->s = lv_app(&l->sbuf, l->s, 4, " ELSE ", psprintf("%s END", lv_value(x, r)));
			l->case_open = false;
		}
		{
			LuaV	   *rv = lv_and_lit(r) ? r->and_b : r;

			l->bs = lv_app(&l->bbuf, l->bs, 0, " OR ", rb->s);
			l->bprec = SP_OR;
			if (!l->typ)
				l->typ = rv->typ;
			l->isbool = l->isbool && rv->isbool;
			l->istext = l->istext && (rv->istext || rv->isnil);
		}
		lv_nest(x, l, l, false, r, false);
		return l;
	}
	if ((l->and_a || l->and_plen) && l->and_b && l->and_b->lit &&
		(l->and_b->isnil || l->and_b->isfalse))
	{
		/*
		 * c and false or d, c and nil or d: "c and false" is false or nil
		 * whatever c is, so the value is d.
		 */
		return r;
	}
	if (lv_and_lit(l))
	{
		/* c and a or b, Lua's conditional expression */
		v = lv_new(psprintf("CASE WHEN %s THEN %s END", lv_and_cond(x, l), l->and_b->s),
				   SP_ATOM);
		v->case_open = true;
		v->bs = pstrdup(l->bs);		/* the ELSE or next arm adds " OR ..." */
		v->bprec = SP_AND;
		v->typ = l->and_b->typ;
		v->isbool = l->and_b->isbool;
		v->istext = l->and_b->istext;
		lv_nest(x, v, l, true, NULL, false);
		/* now the ELSE, or a further arm */
		return x_logic(x, OP_OR, v, r);
	}
	if (l->coalesce)
	{
		/* x or y or default: extend the COALESCE */
		l->s = lv_app(&l->sbuf, l->s, 1, ", ", psprintf("%s)", lv_value(x, r)));
		l->bs = lv_app(&l->bbuf, l->bs, 0, " OR ", rb->s);
		if (!l->typ)
			l->typ = r->typ;
		lv_nest(x, l, l, false, r, false);
		return l;
	}
	if ((l->and_a || l->and_plen) && l->and_b && lv_nonbool(l->and_b))
	{
		/* c and v or d (v not a literal): v when c holds and v is not nil */
		lb = x_tobool(x, l);
		v = lv_new(psprintf("COALESCE(%s, %s)", lv_value(x, l), lv_value(x, r)), SP_ATOM);
		v->bs = psprintf("%s OR %s", lb->s, rb->s);
		v->bprec = SP_OR;
		v->coalesce = true;
		v->typ = l->and_b->typ ? l->and_b->typ : r->typ;
		v->istext = l->and_b->istext;
		v->call = true;
		lv_nest(x, v, l, true, r, true);
		return v;
	}
	if (!l->isbool && (lv_nonbool(l) || (r->lit && !r->isbool && !r->isnil)))
	{
		/* x or default: the first value that is not nil */
		lb = x_tobool(x, l);
		v = lv_new(psprintf("COALESCE(%s, %s)", l->s, lv_value(x, r)), SP_ATOM);
		v->bs = psprintf("%s OR %s", lb->s, rb->s);
		v->bprec = SP_OR;
		v->coalesce = true;
		v->typ = l->typ ? l->typ : r->typ;
		v->istext = l->istext || (r->istext && !l->typ);
		v->call = true;
		lv_nest(x, v, l, true, r, true);
		return v;
	}
	if (l->prec == SP_OR && l->bs == l->s)
	{
		/* a or b or c: extend the chain in place */
		l->s = l->bs = lv_app(&l->sbuf, l->s, 0, " OR ", rb->s);
		lv_nest(x, l, l, false, rb, false);
		return l;
	}
	lb = x_tobool(x, l);
	v = lv_new(psprintf("%s OR %s", lb->s, rb->s), SP_OR);
	v->bs = v->s;
	v->bprec = SP_OR;
	v->isbool = true;
	v->typ = "boolean";
	lv_nest(x, v, lb, false, rb, false);
	return v;
}

static LuaV *
x_binary(LuaX *x, int op, LuaV *l, LuaV *r)
{
	LuaV	   *v;
	const char *so = NULL;
	int			sp = 0;
	bool		lw,
				rw;

	switch (op)
	{
		case OP_OR:
		case OP_AND:
			return x_logic(x, op, l, r);
		case OP_EQ:
		case OP_NE:
			if (r->isnil || l->isnil)
			{
				LuaV	   *o = r->isnil ? l : r;

				lw = o->prec < SP_OTHER;
				v = lv_new(psprintf("%s IS %sNULL", lv_wrap(o, lw),
									op == OP_EQ ? "" : "NOT "), SP_IS);
				v->isbool = true;
				v->typ = "boolean";
				lv_nest(x, v, o, lw, NULL, false);
				return v;
			}
			so = (op == OP_EQ) ? "=" : "<>";
			/* FALLTHROUGH */
		case OP_LT:
		case OP_GT:
		case OP_LE:
		case OP_GE:
			if (!so)
				so = op == OP_LT ? "<" : op == OP_GT ? ">" : op == OP_LE ? "<=" : ">=";
			lw = l->prec <= SP_CMP;
			rw = r->prec <= SP_CMP;
			v = lv_new(psprintf("%s %s %s", lv_wrap(l, lw), so, lv_wrap(r, rw)), SP_CMP);
			v->isbool = true;
			v->typ = "boolean";
			lv_nest(x, v, l, lw, r, rw);
			return v;
		case OP_CONCAT:
			{
				char	   *rp = x_concat_piece(x, r);

				if (l->concat && l->prec == SP_OTHER)
				{
					/* a .. b .. c: extend the chain in place */
					l->s = lv_app(&l->sbuf, l->s, 0, " || ", rp);
					lv_nest(x, l, l, false, r, true);
					return l;
				}
				v = lv_new(psprintf("%s || %s", x_concat_piece(x, l), rp), SP_OTHER);
				v->op = "||";
				v->typ = "text";
				v->istext = v->concat = true;
				lv_nest(x, v, l, true, r, true);
				return v;
			}
		case OP_IDIV:
			{
				int			r1 = typ_rank(l->typ),
							r2 = typ_rank(r->typ);
				char	   *f;

				rw = r->prec <= SP_MUL;
				f = psprintf("floor(%s / %s)", lv_cast(l, "numeric"), lv_wrap(r, rw));
				if ((r1 == 1 || r1 == 2) && (r2 == 1 || r2 == 2))
				{
					v = lv_new(psprintf("%s::%s", f, rank_typ(Max(r1, r2))), SP_ATOM);
					v->typ = rank_typ(Max(r1, r2));
				}
				else
				{
					v = lv_new(f, SP_ATOM);
					v->typ = "numeric";
				}
				v->call = true;
				lv_nest(x, v, l, true, r, rw);
				return v;
			}
		case OP_POW:
			lw = l->prec < SP_POW || l->neg;
			rw = r->prec <= SP_POW;
			v = lv_new(psprintf("%s ^ %s", lv_wrap(l, lw), lv_wrap(r, rw)), SP_POW);
			if (typ_rank(l->typ) && typ_rank(r->typ))
				v->typ = (typ_rank(l->typ) == 3 || typ_rank(r->typ) == 3)
					? "numeric" : "double precision";
			lv_nest(x, v, l, lw, r, rw);
			return v;
		case OP_ADD:
			so = "+";
			sp = SP_ADD;
			break;
		case OP_SUB:
			so = "-";
			sp = SP_ADD;
			break;
		case OP_MUL:
			so = "*";
			sp = SP_MUL;
			break;
		case OP_DIV:
			so = "/";
			sp = SP_MUL;
			break;
		case OP_MOD:
			so = "%";
			sp = SP_MUL;
			break;
		case OP_SHL:
		case OP_SHR:
			{
				/*
				 * Lua shifts 64-bit integers logically: >> fills with
				 * zeros, a count of 64 or more gives 0, and a negative
				 * count shifts the other way. SQL's << and >> keep the
				 * operand's width, shift >> arithmetically and take the
				 * count modulo the width. The subquery names each operand
				 * once, so nested shifts do not repeat their operands.
				 */
				const char *cnt = op == OP_SHL ? "%s" : "-(%s)";

				/*
				 * Both operands are read as numeric first: Lua raises
				 * "number has no integer representation" for a fraction,
				 * where a cast would round, and a count beyond +-64 gives 0
				 * whatever its size. Nested shift subqueries cost plpgsql
				 * more than linear time to plan, so at most 64 nest, as in
				 * plxgo.
				 */
				v = lv_new(psprintf("(SELECT CASE WHEN __plx_sa IS NULL OR __plx_sc IS NULL THEN NULL::bigint"
									" WHEN __plx_sa <> trunc(__plx_sa) OR __plx_sc <> trunc(__plx_sc)"
									/* the empty substr keeps the planner from folding
									 * the cast, which would raise in every case */
									" THEN ('plxlua: number has no integer representation'"
									" || substr(__plx_sa::text, 1, 0))::bigint"
									" WHEN __plx_sc <= -64 OR __plx_sc >= 64 THEN 0::bigint"
									" WHEN __plx_sc >= 0 THEN __plx_sa::bigint << __plx_sc::integer"
									" ELSE (__plx_sa::bigint >> (-__plx_sc)::integer)"
									" & ~((-1)::bigint << (64 + __plx_sc)::integer) END"
									" FROM (SELECT (%s)::numeric AS __plx_sa, (%s)::numeric AS __plx_sc"
									" OFFSET 0) AS __plx_s)",
									l->s, psprintf(cnt, r->s)),
						   SP_ATOM);
				v->typ = "bigint";
				v->shifts = Max(l->shifts, r->shifts) + 1;
				if (v->shifts > 64)
					x_err(x, "too many shifts nested in one expression; split it into several statements");
				lv_nest(x, v, l, false, r, false);
				return v;
			}
		default:
			/* bitwise: all at SQL's "any other operator" level */
			so = op == OP_BOR ? "|" : op == OP_BXOR ? "#" : op == OP_BAND ? "&" :
				op == OP_SHL ? "<<" : ">>";
			rw = r->prec <= SP_OTHER;
			if (l->prec == SP_OTHER && !l->concat && l->op && strcmp(l->op, so) == 0)
			{
				/* a | b | c: extend the chain in place */
				l->s = lv_app(&l->sbuf, l->s, 0, psprintf(" %s ", so), lv_wrap(r, rw));
				if (!(typ_rank(r->typ) && typ_rank(r->typ) <= 2))
					l->typ = NULL;
				lv_nest(x, l, l, false, r, rw);
				return l;
			}
			lw = l->prec < SP_OTHER ||
				(l->prec == SP_OTHER && (!l->op || strcmp(l->op, so) != 0));
			v = lv_new(psprintf("%s %s %s", lv_wrap(l, lw), so, lv_wrap(r, rw)), SP_OTHER);
			v->op = so;
			if (typ_rank(l->typ) && typ_rank(l->typ) <= 2 &&
				typ_rank(r->typ) && typ_rank(r->typ) <= 2)
				v->typ = rank_typ(Max(typ_rank(l->typ), typ_rank(r->typ)));
			lv_nest(x, v, l, lw, r, rw);
			return v;
	}
	rw = r->prec <= sp;
	if (l->prec == sp)
	{
		/* a + b - c, a * b / c: extend the chain in place */
		l->s = lv_app(&l->sbuf, l->s, 0, psprintf(" %s ", so), lv_wrap(r, rw));
		l->typ = (typ_rank(l->typ) && typ_rank(r->typ))
			? rank_typ(Max(typ_rank(l->typ), typ_rank(r->typ))) : NULL;
		lv_nest(x, l, l, false, r, rw);
		return l;
	}
	lw = l->prec < sp;
	v = lv_new(psprintf("%s %s %s", lv_wrap(l, lw), so, lv_wrap(r, rw)), sp);
	if (typ_rank(l->typ) && typ_rank(r->typ))
		v->typ = rank_typ(Max(typ_rank(l->typ), typ_rank(r->typ)));
	lv_nest(x, v, l, lw, r, rw);
	return v;
}

static LuaV *
x_subexpr(LuaX *x, int limit)
{
	LuaV	   *v;
	Tok		   *k;

	check_stack_depth();
	CHECK_FOR_INTERRUPTS();
	if (++x->cx->depth > PLX_MAX_DEPTH)
		x_err(x, "expression nested too deeply");
	k = x_peek(x);
	if (tk_kw(k, KW_NOT) || plx_tok_is(k, "-") || plx_tok_is(k, "#") ||
		plx_tok_is(k, "~"))
	{
		char		u = (k->kind == T_KW) ? '!' : k->s[0];
		LuaV	   *a;

		x->p++;
		a = x_subexpr(x, LUA_UNARY_PRIO);
		v = x_unary(x, u, a);
	}
	else
	{
		v = x_simple(x);
		if (!v)
			v = x_suffixed(x);
	}
	for (;;)
	{
		int			op;
		LuaV	   *r;

		k = x_peek(x);
		op = x_binop(k);
		if (op == OP_NONE || lua_prio[op].left <= limit)
			break;
		x->p++;
		r = x_subexpr(x, lua_prio[op].right);
		v = x_binary(x, op, v, r);
	}
	x->cx->depth--;
	return v;
}

/* translate tokens [a, b) of the function body */
static LuaV *
lua_expr(Lua *L, int a, int b, bool diag)
{
	LuaX		x;
	LuaV	   *v;

	memset(&x, 0, sizeof(x));
	x.L = L;
	x.cx = L->cx;
	x.t = L->cx->t;
	x.p = a;
	x.e = b;
	x.diag = diag;
	if (x_peek(&x)->kind == T_EOF)
		plx_err(L->cx, L->cx->t[a].line, "an expression is expected");
	v = x_subexpr(&x, 0);
	if (x_peek(&x)->kind != T_EOF)
		x_err(&x, "unexpected %s", tok_desc(x_peek(&x)));
	return v;
}

static char *
lua_cond(Lua *L, int a, int b)
{
	LuaX		x;

	memset(&x, 0, sizeof(x));
	x.L = L;
	x.cx = L->cx;
	x.t = L->cx->t;
	x.p = a;
	x.e = b;
	return x_tobool(&x, lua_expr(L, a, b, false))->s;
}

/*
 * PlxSurface.rewrite_expr: an expression the shared emitter extracted as
 * source text (an intrinsic's argument). Re-lexed with the Lua lexer and
 * translated in the current scope.
 */
static char *
lua_rewrite_hook(Ctx *cx, const char *s, int len, bool boolctx)
{
	Lua		   *L = (Lua *) cx->fe;
	const char *sb = cx->body;
	Tok		   *st = cx->t;
	int			sp = cx->pos;
	int			line = (st && sp >= 0) ? st[sp].line : 0;
	LuaX		x;
	LuaV	   *v;
	int			n;

	if (!L)
		elog(ERROR, "plxlua: expression rewriter called outside the front end");
	cx->body = pnstrdup(s, len);
	plx_lex(cx);
	memset(&x, 0, sizeof(x));
	x.t = cx->t;
	cx->body = sb;
	cx->t = st;
	cx->pos = sp;
	for (n = 0; x.t[n].kind != T_EOF; n++)
		;
	x.L = L;
	x.cx = cx;
	x.e = n;
	x.loff = line > 0 ? line - 1 : 0;
	x.diag = cx->diag_msg;
	if (x_peek(&x)->kind == T_EOF)
		plx_err(cx, line, "an expression is expected");
	v = x_subexpr(&x, 0);
	if (x_peek(&x)->kind != T_EOF)
		x_err(&x, "unexpected %s", tok_desc(x_peek(&x)));
	if (boolctx)
		v = x_tobool(&x, v);
	return v->s;
}

/* ------------------------------------------------------ statement helpers */

static bool
lua_ends_operand(Tok *k)
{
	switch (k->kind)
	{
		case T_IDENT:
		case T_INT:
		case T_FLOAT:
		case T_STRING:
		case T_RPAREN:
		case T_RBRACKET:
		case T_RBRACE:
			return true;
		case T_KW:
			return k->kw == KW_NIL || k->kw == KW_TRUE || k->kw == KW_FALSE;
		case T_OP:
			return plx_tok_is(k, "...");
		default:
			return false;
	}
}

static bool
lua_stmt_kw(Kw w)
{
	switch (w)
	{
		case KW_END:
		case KW_ELSE:
		case KW_ELSIF:
		case KW_UNTIL:
		case KW_THEN:
		case KW_DO:
		case KW_LET:
		case KW_IF:
		case KW_WHILE:
		case KW_FOR:
		case KW_LOOP:
		case KW_RETURN:
		case KW_BREAK:
		case KW_EMIT:
		case KW_RETURN_NEXT:
			return true;
		default:
			return false;
	}
}

/* a line that starts with this token continues the previous statement */
static bool
lua_continues(Tok *k)
{
	if (k->kind == T_OP)
		return !plx_tok_is(k, "::");
	if (k->kind == T_DOT)
		return true;
	return k->kind == T_KW && (k->kw == KW_AND || k->kw == KW_OR);
}

/*
 * End of the statement starting at from. Lua statements are free form: one
 * ends at ';', at a line break that is not followed by a binary operator, at
 * a keyword that starts or closes a block, after a type annotation, or where
 * a name follows a complete operand ("x = 1 y = 2"). Brackets nest, and a
 * function body inside them (pcall) is stepped over whole.
 */
static int
lua_stmt_end(Lua *L, int from)
{
	Tok		   *t = L->cx->t;
	int			i,
				depth = 0;
	bool		prev = false;

	for (i = from;; i++)
	{
		Tok		   *k = &t[i];

		CHECK_FOR_INTERRUPTS();
		if (k->kind == T_EOF)
			return i;
		if (depth == 0 && i > from)
		{
			if (k->kind == T_SEMI)
				return i;
			if (k->kind == T_NEWLINE)
			{
				int			j = i + 1;

				while (t[j].kind == T_NEWLINE)
					j++;
				if (lua_continues(&t[j]))
				{
					i = j - 1;
					continue;
				}
				return i;
			}
			if (k->kind == T_KW && lua_stmt_kw(k->kw))
				return i;
			if (k->kind == T_KW && k->kw == KW_DEF)
				plx_err(L->cx, k->line,
						"functions are not supported (only the pcall(function() ... end) form)");
			if (plx_tok_is(k, "::"))
				return i;
			if (prev && (k->kind == T_IDENT || k->kind == T_INT || k->kind == T_FLOAT ||
						 (k->kind == T_KW && (k->kw == KW_NIL || k->kw == KW_TRUE ||
											  k->kw == KW_FALSE || k->kw == KW_NOT))))
				return i;
		}
		if (depth == 0 && k->kind == T_TYPEANN)
			return i + 1;
		switch (k->kind)
		{
			case T_LPAREN:
			case T_LBRACKET:
			case T_LBRACE:
				depth++;
				break;
			case T_RPAREN:
			case T_RBRACKET:
			case T_RBRACE:
				if (depth == 0)
					return i;
				depth--;
				break;
			case T_KW:
				if (k->kw == KW_DEF && depth > 0 && L->match[i] > i)
				{
					i = L->match[i];
					k = &t[i];
				}
				break;
			default:
				break;
		}
		prev = lua_ends_operand(k);
	}
}

/* index of keyword want at bracket depth 0 from from; a header that runs
 * into another statement is an error */
static int
lua_find_kw(Lua *L, int from, Kw want, const char *what, int line)
{
	Tok		   *t = L->cx->t;
	int			i,
				depth = 0;

	for (i = from;; i++)
	{
		Tok		   *k = &t[i];

		CHECK_FOR_INTERRUPTS();
		if (k->kind == T_EOF)
			plx_err(L->cx, line, "'%s' expected", what);
		if (k->kind == T_LPAREN || k->kind == T_LBRACKET || k->kind == T_LBRACE)
			depth++;
		else if (k->kind == T_RPAREN || k->kind == T_RBRACKET || k->kind == T_RBRACE)
		{
			if (--depth < 0)
				plx_err(L->cx, k->line, "unexpected %s", tok_desc(k));
		}
		else if (k->kind == T_KW && k->kw == KW_DEF && depth > 0 && L->match[i] > i)
			i = L->match[i];
		else if (depth == 0 && k->kind == T_KW)
		{
			if (k->kw == want)
				return i;
			if (lua_stmt_kw(k->kw) || k->kw == KW_DEF)
				plx_err(L->cx, line, "'%s' expected before %s", what, tok_desc(k));
		}
	}
}

/* split [a, b) at top-level commas into at most max ranges; returns count */
static int
lua_split(Lua *L, int a, int b, int *as, int *ae, int max)
{
	Tok		   *t = L->cx->t;
	int			i,
				depth = 0,
				n = 0,
				s = a;

	for (i = a; i <= b; i++)
	{
		if (i < b)
		{
			TokKind		k = t[i].kind;

			if (k == T_LPAREN || k == T_LBRACKET || k == T_LBRACE)
				depth++;
			else if (k == T_RPAREN || k == T_RBRACKET || k == T_RBRACE)
				depth--;
			else if (k == T_KW && t[i].kw == KW_DEF && L->match[i] > i && L->match[i] < b)
				i = L->match[i];
		}
		if (i == b || (depth == 0 && t[i].kind == T_COMMA))
		{
			if (n >= max)
				plx_err(L->cx, t[a].line, "too many values in one statement (maximum %d)", max);
			as[n] = s;
			ae[n] = i;
			n++;
			s = i + 1;
		}
	}
	return n;
}

static void
lua_expect(Lua *L, Kw w, const char *what, int line)
{
	Ctx		   *cx = L->cx;

	if (!tk_kw(&cx->t[cx->pos], w))
		plx_err(cx, line, "'%s' expected, found %s", what, tok_desc(&cx->t[cx->pos]));
	cx->pos++;
}

/* parse a block into a separate buffer */
static char *
lua_capture(Lua *L, int ind)
{
	Ctx		   *cx = L->cx;
	StringInfoData saved = cx->out;
	char	   *r;

	initStringInfo(&cx->out);
	lua_block(L, ind);
	r = cx->out.data;
	cx->out = saved;
	return r;
}

static bool
lua_toplevel(Lua *L)
{
	return L->scope == L->fscope && L->inloop == 0;
}

static void
lua_emit_assign(Lua *L, int ind, const char *lhs, const char *rhs)
{
	plx_indent(&L->cx->out, ind);
	appendStringInfo(&L->cx->out, "%s := %s;\n", lhs, rhs);
}

/* the arguments of a statement call NAME(...) / NAME "s" / NAME {...};
 * name is the token index of NAME, b the end of the statement */
static int
lua_call_args(Lua *L, int name, int b, int *as, int *ae, int max)
{
	Ctx		   *cx = L->cx;
	Tok		   *t = cx->t;
	int			k = name + 1,
				after,
				n;

	if (t[k].kind == T_LPAREN)
	{
		n = plx_parse_args(cx, name, as, ae, max, &after);
		if (after != b)
			plx_err(cx, t[name].line, "unexpected %s after %.*s(...)",
					tok_desc(&t[after]), t[name].len, t[name].s);
		return n;
	}
	if (t[k].kind == T_STRING && k + 1 == b)
	{
		as[0] = k;
		ae[0] = k + 1;
		return 1;
	}
	if (t[k].kind == T_LBRACE)
	{
		int			i,
					depth = 0;

		for (i = k; i < b; i++)
		{
			if (t[i].kind == T_LBRACE)
				depth++;
			else if (t[i].kind == T_RBRACE && --depth == 0)
				break;
		}
		if (i + 1 == b)
		{
			as[0] = k;
			ae[0] = b;
			return 1;
		}
	}
	plx_err(cx, t[name].line, "'(' expected after %.*s", t[name].len, t[name].s);
}

/* ------------------------------------------------- assignment intrinsics */

enum
{
	RI_NONE, RI_FETCH, RI_OPEN, RI_FETCHFROM, RI_ROWCOUNT
};

typedef struct LuaRI
{
	int			kind;
	bool		strict;
	bool		dyn;
	char	   *sql;
	char	   *binds;
	char	   *cur;
} LuaRI;

/* Is [ra, rb) exactly fetch_one(...), fetch_one!(...), open_cursor(...),
 * fetch_from(...) or row_count()? If so translate its arguments now (before
 * the target is declared, as Lua scoping requires) into *ri. */
static int
lua_rhs_intrinsic(Lua *L, int ra, int rb, LuaRI *ri)
{
	Ctx		   *cx = L->cx;
	Tok		   *t = cx->t;
	int			as[16],
				ae[16],
				after,
				n;

	memset(ri, 0, sizeof(*ri));
	if (rb - ra < 3 || t[ra].kind != T_IDENT || t[ra + 1].kind != T_LPAREN)
		return RI_NONE;
	if (tk_name(&t[ra], "fetch_one") || tk_name(&t[ra], "fetch_one!"))
	{
		ri->kind = RI_FETCH;
		ri->strict = tk_name(&t[ra], "fetch_one!");
	}
	else if (tk_name(&t[ra], "open_cursor"))
		ri->kind = RI_OPEN;
	else if (tk_name(&t[ra], "fetch_from"))
		ri->kind = RI_FETCHFROM;
	else if (tk_name(&t[ra], "row_count"))
		ri->kind = RI_ROWCOUNT;
	else
		return RI_NONE;
	n = plx_parse_args(cx, ra, as, ae, 16, &after);
	if (after != rb)
		return (ri->kind = RI_NONE);
	cx->pos = ra;
	switch (ri->kind)
	{
		case RI_FETCH:
		case RI_OPEN:
			if (n < 1)
				plx_err(cx, t[ra].line, "%.*s requires a SQL argument", t[ra].len, t[ra].s);
			ri->dyn = plx_sql_arg(cx, as, ae, n, false, &ri->sql, &ri->binds);
			break;
		case RI_FETCHFROM:
			if (n != 1)
				plx_err(cx, t[ra].line, "fetch_from takes a cursor");
			ri->cur = lua_expr(L, as[0], ae[0], false)->s;
			break;
		default:
			if (n != 0)
				plx_err(cx, t[ra].line, "row_count() takes no arguments");
			break;
	}
	return ri->kind;
}

static const char *
lua_ri_typ(LuaRI *ri, bool *rec)
{
	*rec = (ri->kind == RI_FETCH || ri->kind == RI_FETCHFROM);
	return ri->kind == RI_OPEN ? "refcursor" : ri->kind == RI_ROWCOUNT ? "bigint" : NULL;
}

static void
lua_ri_emit(Lua *L, int ind, const char *lhs, LuaRI *ri)
{
	Ctx		   *cx = L->cx;
	StringInfo	o = &cx->out;

	plx_indent(o, ind);
	switch (ri->kind)
	{
		case RI_FETCH:
			if (!ri->dyn)
				appendStringInfo(o, "SELECT * INTO %s%s FROM (%s) AS __plx_fo_%d;\n",
								 ri->strict ? "STRICT " : "", lhs, ri->sql, ++cx->subq);
			else
				appendStringInfo(o, "EXECUTE %s INTO %s%s%s%s;\n", ri->sql,
								 ri->strict ? "STRICT " : "", lhs,
								 ri->binds ? " USING " : "", ri->binds ? ri->binds : "");
			break;
		case RI_OPEN:
			if (!ri->dyn)
				appendStringInfo(o, "OPEN %s FOR %s;\n", lhs, ri->sql);
			else
				appendStringInfo(o, "OPEN %s FOR EXECUTE %s%s%s;\n", lhs, ri->sql,
								 ri->binds ? " USING " : "", ri->binds ? ri->binds : "");
			break;
		case RI_FETCHFROM:
			appendStringInfo(o, "FETCH FROM %s INTO %s;\n", ri->cur, lhs);
			break;
		default:
			appendStringInfo(o, "GET DIAGNOSTICS %s = ROW_COUNT;\n", lhs);
			break;
	}
}

/* give an untyped local the type of the value assigned to it */
static void
lua_learn_type(LuaBind *b, const char *typ, bool rec)
{
	if (!b || !b->loc || b->loc->typ || b->loc->is_record)
		return;
	if (rec)
		b->loc->is_record = true;
	else if (typ)
		b->loc->typ = pstrdup(typ);
}

/*
 * Assign val[i] to tgt[i] as one Lua multiple assignment: every value is
 * computed before any target changes. Direct assignment in order is kept
 * when no later value or target reads an earlier target; otherwise each value
 * goes through a typed temporary, which needs every target's type.
 */
static void
lua_parallel(Lua *L, int ind, int n, char **tgt, const char **tgtyp,
			 char **base, char ***trefs, int *ntrefs,
			 char **val, char ***vrefs, int *nvrefs, int line)
{
	bool		sound = true;
	int			i,
				j;

	for (i = 0; i < n && sound; i++)
		for (j = i + 1; j < n && sound; j++)
			if (lua_in_list(vrefs[j], nvrefs[j], base[i]) ||
				lua_in_list(trefs[j], ntrefs[j], base[i]))
				sound = false;
	if (sound)
	{
		for (i = 0; i < n; i++)
			lua_emit_assign(L, ind, tgt[i], val[i]);
		return;
	}
	for (i = 0; i < n; i++)
		if (!tgtyp[i])
			plx_err(L->cx, line,
					"this multiple assignment reads a variable it also assigns, and the type of \"%s\" is not known; assign the values one at a time",
					tgt[i]);
	{
		char	  **tmp = palloc(n * sizeof(char *));

		for (i = 0; i < n; i++)
		{
			tmp[i] = lua_hidden(L, "t", tgtyp[i]);
			lua_emit_assign(L, ind, tmp[i], val[i]);
		}
		for (i = 0; i < n; i++)
			lua_emit_assign(L, ind, tgt[i], tmp[i]);
	}
}

/* ---------------------------------------------------------------- pcall */

/* t[pc] is "pcall": check the pcall(function() ... end) shape and return the
 * index just past its closing parenthesis */
static int
lua_pcall_shape(Lua *L, int pc)
{
	Tok		   *t = L->cx->t;
	int			fend;

	if (t[pc + 1].kind != T_LPAREN || !tk_kw(&t[pc + 2], KW_DEF))
		plx_err(L->cx, t[pc].line,
				"pcall is only supported as pcall(function() ... end)");
	if (t[pc + 3].kind != T_LPAREN || t[pc + 4].kind != T_RPAREN)
		plx_err(L->cx, t[pc].line,
				"the function given to pcall cannot take parameters");
	fend = L->match[pc + 2];
	if (fend < 0)
		plx_err(L->cx, t[pc].line, "'end' expected to close the function given to pcall");
	if (t[fend + 1].kind != T_RPAREN)
		plx_err(L->cx, t[fend].line,
				"pcall takes only the function (extra arguments are not supported)");
	return fend + 2;
}

/* the success assignments of a protected block (also run by a return) */
static void
lua_pc_success(Lua *L, LuaLoop *f, int ind)
{
	StringInfo	o = &L->cx->out;

	if (f->pc_ok)
		lua_emit_assign(L, ind, f->pc_ok, "true");
	if (f->pc_err)
	{
		lua_emit_assign(L, ind, f->pc_err, "NULL");
		plx_indent(o, ind);
		appendStringInfo(o, "%cS%d%c\n", LUA_MARK, f->pc_errid, LUA_MARK);
		L->markers = true;
	}
}

/*
 * pcall(function() BODY end) starting at token pc. okpg/errpg are the
 * plpgsql variables receiving the status and the error message (either may
 * be NULL). The caller makes new bindings visible after this returns,
 * because a Lua local is not in scope inside its own initializer.
 */
static void
lua_pcall(Lua *L, int ind, int pc, const char *okpg, const char *errpg)
{
	Ctx		   *cx = L->cx;
	Tok		   *t = cx->t;
	int			end = lua_pcall_shape(L, pc);
	int			fend = end - 2;
	LuaLoop    *f = palloc0(sizeof(LuaLoop));
	char	   *body;
	StringInfo	o;

	f->kind = LOOP_FUNC;
	f->id = ++L->seq;
	f->pc_ok = okpg ? pstrdup(okpg) : NULL;
	f->pc_err = errpg ? pstrdup(errpg) : NULL;
	if (errpg)
		f->pc_errid = lua_errvar(L, errpg)->id;
	f->outer = L->loops;
	L->loops = f;
	L->inloop++;
	lua_scope_push(L);
	cx->pos = pc + 5;
	body = lua_capture(L, ind + 1);
	if (cx->pos != fend)
		plx_err(cx, t[cx->pos].line, "'end' expected to close the function given to pcall, found %s",
				tok_desc(&t[cx->pos]));
	{
		/* the success path, appended to the body */
		StringInfoData saved = cx->out;

		initStringInfo(&cx->out);
		appendStringInfoString(&cx->out, body);
		pfree(body);
		lua_pc_success(L, f, ind + 1);
		body = cx->out.data;
		cx->out = saved;
	}
	lua_scope_pop(L);
	L->inloop--;
	L->loops = f->outer;

	o = &cx->out;
	if (f->pc_ret)
	{
		plx_indent(o, ind);
		appendStringInfo(o, "<<__plx_pc_%d>>\n", f->id);
	}
	plx_indent(o, ind);
	appendStringInfoString(o, "BEGIN\n");
	appendStringInfoString(o, body);
	pfree(body);
	plx_indent(o, ind);
	/* OTHERS leaves out a failed ASSERT, which Lua's pcall catches like any
	 * other error; a query cancel stays uncaught */
	appendStringInfoString(o, "EXCEPTION WHEN OTHERS OR assert_failure THEN\n");
	if (f->pc_ok)
		lua_emit_assign(L, ind + 1, f->pc_ok, "false");
	if (f->pc_err)
	{
		lua_emit_assign(L, ind + 1, f->pc_err, "SQLERRM");
		plx_indent(o, ind + 1);
		appendStringInfo(o, "%cH%d%c\n", LUA_MARK, f->pc_errid, LUA_MARK);
		L->markers = true;
	}
	if (!f->pc_ok && !f->pc_err)
	{
		plx_indent(o, ind + 1);
		appendStringInfoString(o, "NULL;\n");
	}
	plx_indent(o, ind);
	appendStringInfoString(o, "END;\n");
	cx->pos = end;
}

/* ------------------------------------------------------------- statements */

/* local n1 [<attr>], n2 ... [= e1, e2 ...] [--:: type] */
static void
lua_local(Lua *L, int ind)
{
	Ctx		   *cx = L->cx;
	Tok		   *t = cx->t;
	int			a = cx->pos,
				b,
				rb,
				i,
				eq = -1;
	int			line = t[a].line;
	int			nm[64],
				nnames = 0;
	bool		isconst[64];
	int			as[64],
				ae[64],
				nrhs = 0;
	const char *ann = NULL;

	if (tk_kw(&t[a + 1], KW_DEF))
		plx_err(cx, line, "local function definitions are not supported");
	b = lua_stmt_end(L, a);
	rb = b;
	if (rb > a && t[rb - 1].kind == T_TYPEANN)
	{
		rb--;
		ann = pnstrdup(t[rb].ann, t[rb].annlen);
		if (!ann[0])
			plx_err(cx, line, "a type annotation needs a type");
	}
	i = a + 1;
	for (;;)
	{
		if (i >= rb || t[i].kind != T_IDENT)
			plx_err(cx, line, "local expects a variable name, found %s", tok_desc(&t[i]));
		if (nnames >= 64)
			plx_err(cx, line, "too many variables in one local statement");
		nm[nnames] = i;
		isconst[nnames] = false;
		i++;
		if (i + 2 < rb && plx_tok_is(&t[i], "<") && t[i + 1].kind == T_IDENT &&
			plx_tok_is(&t[i + 2], ">"))
		{
			if (tk_name(&t[i + 1], "const"))
				isconst[nnames] = true;
			else if (tk_name(&t[i + 1], "close"))
				plx_err(cx, line, "the <close> attribute is not supported");
			else
				plx_err(cx, line, "unknown attribute <%.*s>", t[i + 1].len, t[i + 1].s);
			i += 3;
		}
		nnames++;
		if (i < rb && t[i].kind == T_COMMA)
		{
			i++;
			continue;
		}
		break;
	}
	if (i < rb)
	{
		if (!plx_tok_is(&t[i], "="))
			plx_err(cx, line, "unexpected %s in a local declaration", tok_desc(&t[i]));
		eq = i;
		if (eq + 1 >= rb)
			plx_err(cx, line, "an expression is expected after '='");
		nrhs = lua_split(L, eq + 1, rb, as, ae, 64);
	}
	if (nrhs > nnames)
		plx_err(cx, line, "more values than variables in a local declaration");

	/* special right-hand sides */
	if (nrhs == 1 && tk_name(&t[as[0]], "pcall") && t[as[0] + 1].kind == T_LPAREN)
	{
		bool		isnew;
		LuaBind    *ok,
				   *err = NULL;

		if (nnames > 2)
			plx_err(cx, line, "pcall returns a status and an error message (two variables at most)");
		if (lua_pcall_shape(L, as[0]) != ae[0])
			plx_err(cx, line, "unexpected tokens after pcall(...)");
		ok = lua_declare(L, t[nm[0]].s, t[nm[0]].len, ann ? ann : "boolean", false,
						 false, L->scope, NULL, 0, line, &isnew);
		if (nnames == 2)
			err = lua_declare(L, t[nm[1]].s, t[nm[1]].len, "text", false,
							  false, L->scope, NULL, 0, line, &isnew);
		lua_pcall(L, ind, as[0], ok->pgname, err ? err->pgname : NULL);
		if (err)
			err->errid = lua_errvar(L, err->pgname)->id;
		lua_activate(L, ok, L->scope);
		ok->isconst = isconst[0];
		if (err)
		{
			lua_activate(L, err, L->scope);
			err->isconst = isconst[1];
		}
		cx->pos = b;
		return;
	}
	if (nrhs == 1 && nnames == 1)
	{
		LuaRI		ri;

		if (lua_rhs_intrinsic(L, as[0], ae[0], &ri))
		{
			bool		rec,
						isnew;
			const char *typ = lua_ri_typ(&ri, &rec);
			LuaBind    *bd;

			if (ann)
			{
				typ = ann;
				rec = false;
			}
			bd = lua_declare(L, t[nm[0]].s, t[nm[0]].len, typ, rec, false, L->scope,
							 NULL, 0, line, &isnew);
			lua_activate(L, bd, L->scope);
			bd->isconst = isconst[0];
			lua_ri_emit(L, ind, bd->pgname, &ri);
			cx->pos = b;
			return;
		}
	}

	/* ordinary values: computed before the new locals come into scope */
	{
		LuaV	   *vals[64];
		char	  **refs = NULL;
		int			nrefs = 0;
		bool		top = lua_toplevel(L);

		if (nnames > 1)
			lua_refs_begin(L);
		cx->pos = a;
		for (i = 0; i < nrhs; i++)
			vals[i] = lua_expr(L, as[i], ae[i], false);
		if (nnames > 1)
			refs = lua_refs_take(L, &nrefs);
		for (i = 0; i < nnames; i++)
		{
			LuaV	   *v = (i < nrhs) ? vals[i] : NULL;
			const char *typ = ann ? ann : (v ? v->typ : NULL);
			bool		rec = !ann && v && v->rec;
			bool		isnew;
			LuaBind    *bd;

			bd = lua_declare(L, t[nm[i]].s, t[nm[i]].len, typ, rec, false, L->scope,
							 refs, nrefs, line, &isnew);
			lua_activate(L, bd, L->scope);
			bd->isconst = isconst[i];
			if (!v)
			{
				if (!(isnew && top))
					lua_emit_assign(L, ind, bd->pgname, "NULL");
				continue;
			}
			if (isnew && top && v->lit && !v->isnil && bd->loc->typ && !bd->loc->is_record)
			{
				/* a literal first value at the top level folds into DECLARE;
				 * a <const> one becomes a plpgsql CONSTANT */
				bd->loc->init = v->s;
				if (isconst[i])
					bd->loc->is_const = true;
				continue;
			}
			lua_emit_assign(L, ind, bd->pgname, v->s);
		}
	}
	cx->pos = b;
}

/* validate and translate an assignment target [a, b) */
static LuaV *
lua_lvalue(Lua *L, int a, int b, LuaBind **basep)
{
	Ctx		   *cx = L->cx;
	Tok		   *t = cx->t;
	int			i = a + 1;
	LuaBind    *base;

	if (t[a].kind != T_IDENT || lua_is_libns(&t[a]))
		plx_err(cx, t[a].line, "cannot assign to this expression");
	while (i < b)
	{
		if (t[i].kind == T_DOT && i + 1 < b && t[i + 1].kind == T_IDENT)
			i += 2;
		else if (t[i].kind == T_LBRACKET)
		{
			int			depth = 0;

			for (; i < b; i++)
			{
				if (t[i].kind == T_LBRACKET || t[i].kind == T_LPAREN || t[i].kind == T_LBRACE)
					depth++;
				else if ((t[i].kind == T_RBRACKET || t[i].kind == T_RPAREN ||
						  t[i].kind == T_RBRACE) && --depth == 0)
					break;
			}
			if (i >= b)
				plx_err(cx, t[a].line, "']' expected in an assignment target");
			i++;
		}
		else
			plx_err(cx, t[a].line, "cannot assign to this expression");
	}
	base = lua_lookup(L, t[a].s, t[a].len);
	if (base && base->isconst)
		plx_err(cx, t[a].line, "attempt to assign to const variable \"%s\"", base->lname);
	if (base && base->errid && b > a + 1)
		plx_err(cx, t[a].line, "the fields of a pcall error cannot be assigned");
	*basep = base;
	return lua_expr(L, a, b, false);
}

/* is [ra, rb) the string-building idiom  s = s .. rest ? */
static int
lua_builder_rest(Lua *L, LuaBind *b, int ra, int rb)
{
	Tok		   *t = L->cx->t;
	int			i,
				depth = 0;

	if (!b || !b->loc || b->isconst || !typ_is_stringy(b->loc->typ) || rb - ra < 3)
		return -1;
	if (t[ra].kind != T_IDENT || !plx_name_eq(&t[ra], b->lname) ||
		!plx_tok_is(&t[ra + 1], ".."))
		return -1;
	/* nothing in the rest may bind more loosely than '..' */
	for (i = ra + 2; i < rb; i++)
	{
		Tok		   *k = &t[i];

		if (k->kind == T_LPAREN || k->kind == T_LBRACKET || k->kind == T_LBRACE)
			depth++;
		else if (k->kind == T_RPAREN || k->kind == T_RBRACKET || k->kind == T_RBRACE)
			depth--;
		else if (depth == 0)
		{
			int			op = x_binop(k);

			if (op != OP_NONE && op != OP_CONCAT &&
				lua_prio[op].left < lua_prio[OP_CONCAT].left)
				return -1;
		}
	}
	return ra + 2;
}

/* targets = values */
static void
lua_assign(Lua *L, int a, int eq, int b, int ind)
{
	Ctx		   *cx = L->cx;
	Tok		   *t = cx->t;
	int			line = t[a].line;
	int			rb = b;
	const char *ann = NULL;
	int			ta[64],
				te[64],
				nt,
				as[64],
				ae[64],
				nrhs;
	int			i;

	if (rb > eq && t[rb - 1].kind == T_TYPEANN)
	{
		rb--;
		ann = pnstrdup(t[rb].ann, t[rb].annlen);
	}
	if (eq + 1 >= rb)
		plx_err(cx, line, "an expression is expected after '='");
	nt = lua_split(L, a, eq, ta, te, 64);
	nrhs = lua_split(L, eq + 1, rb, as, ae, 64);
	if (nrhs > nt)
		plx_err(cx, line, "more values than variables in an assignment");
	for (i = 0; i < nt; i++)
		if (ta[i] >= te[i])
			plx_err(cx, line, "a variable is expected before ','");

	/* ok, err = pcall(function() ... end) */
	if (nrhs == 1 && tk_name(&t[as[0]], "pcall") && t[as[0] + 1].kind == T_LPAREN)
	{
		char	   *pg[2] = {NULL, NULL};
		LuaBind    *imp[2] = {NULL, NULL};

		if (nt > 2)
			plx_err(cx, line, "pcall returns a status and an error message (two variables at most)");
		if (lua_pcall_shape(L, as[0]) != ae[0])
			plx_err(cx, line, "unexpected tokens after pcall(...)");
		for (i = 0; i < nt; i++)
		{
			LuaBind    *bd;
			bool		isnew;

			if (te[i] != ta[i] + 1 || t[ta[i]].kind != T_IDENT)
				plx_err(cx, line, "pcall results can only be assigned to variables");
			bd = lua_lookup(L, t[ta[i]].s, t[ta[i]].len);
			if (bd)
			{
				if (bd->isconst)
					plx_err(cx, line, "attempt to assign to const variable \"%s\"", bd->lname);
				pg[i] = bd->pgname;
				lua_learn_type(bd, i == 0 ? "boolean" : "text", false);
				if (i == 1)
					bd->errid = lua_errvar(L, bd->pgname)->id;
			}
			else if (lua_param(cx, t[ta[i]].s, t[ta[i]].len) >= 0)
				pg[i] = lua_param_ref(cx, t[ta[i]].s, t[ta[i]].len);
			else
			{
				imp[i] = lua_declare(L, t[ta[i]].s, t[ta[i]].len,
									 i == 0 ? "boolean" : "text", false, false,
									 L->fscope, NULL, 0, line, &isnew);
				pg[i] = imp[i]->pgname;
			}
		}
		lua_pcall(L, ind, as[0], pg[0], nt == 2 ? pg[1] : NULL);
		for (i = 0; i < nt; i++)
			if (imp[i])
			{
				if (i == 1)
					imp[i]->errid = lua_errvar(L, imp[i]->pgname)->id;
				lua_activate(L, imp[i], L->fscope);
			}
		cx->pos = b;
		return;
	}

	if (nt == 1 && nrhs == 1 && te[0] == ta[0] + 1)
	{
		/* one plain variable */
		Tok		   *nmk = &t[a];
		LuaBind    *bd = lua_lookup(L, nmk->s, nmk->len);
		bool		param = !bd && lua_param(cx, nmk->s, nmk->len) >= 0;
		LuaRI		ri;
		int			rest;

		if (nmk->kind != T_IDENT || (!bd && !param && lua_is_libns(nmk)))
			plx_err(cx, line, "cannot assign to this expression");
		if (nmk->s[nmk->len - 1] == '!' || nmk->s[nmk->len - 1] == '?')
			plx_err(cx, line, "unexpected %s", tok_desc(nmk));
		if (bd && bd->isconst)
			plx_err(cx, line, "attempt to assign to const variable \"%s\"", bd->lname);
		if (ann && bd && bd->loc)
		{
			if (!bd->loc->typ && !bd->loc->is_record)
				bd->loc->typ = pstrdup(ann);
			else if (!bd->loc->typ || pg_strcasecmp(bd->loc->typ, ann) != 0)
				plx_err(cx, line, "\"%s\" already has a type", bd->lname);
		}
		else if (ann && param)
			plx_err(cx, line, "cannot annotate parameter \"%.*s\"", nmk->len, nmk->s);

		if (lua_rhs_intrinsic(L, as[0], ae[0], &ri))
		{
			bool		rec,
						isnew;
			const char *typ = lua_ri_typ(&ri, &rec);
			const char *lhs;

			if (ann)
			{
				typ = ann;
				rec = false;
			}
			if (bd)
			{
				lua_learn_type(bd, typ, rec);
				lhs = bd->pgname;
			}
			else if (param)
				lhs = lua_param_ref(cx, nmk->s, nmk->len);
			else
			{
				bd = lua_declare(L, nmk->s, nmk->len, typ, rec, false, L->fscope,
								 NULL, 0, line, &isnew);
				lua_activate(L, bd, L->fscope);
				lhs = bd->pgname;
			}
			lua_ri_emit(L, ind, lhs, &ri);
			cx->pos = b;
			return;
		}
		if ((rest = lua_builder_rest(L, bd, as[0], ae[0])) >= 0)
		{
			LuaV	   *v;

			cx->pos = a;
			v = lua_expr(L, rest, ae[0], false);
			bd->loc->typ = plx_ext_object("plx_strbuild");
			plx_indent(&cx->out, ind);
			appendStringInfo(&cx->out, "%s := %s(%s, %s);\n", bd->pgname,
							 plx_ext_object("plx_sb_append"), bd->pgname, lv_text_arg(v));
			cx->pos = b;
			return;
		}
		{
			LuaV	   *v;

			cx->pos = a;
			v = lua_expr(L, as[0], ae[0], false);
			if (bd)
			{
				lua_learn_type(bd, v->typ, v->rec);
				lua_emit_assign(L, ind, bd->pgname, v->s);
			}
			else if (param)
				lua_emit_assign(L, ind, lua_param_ref(cx, nmk->s, nmk->len), v->s);
			else
			{
				/* an undeclared name: a local of the whole function */
				bool		isnew;

				bd = lua_declare(L, nmk->s, nmk->len, ann ? ann : v->typ, !ann && v->rec,
								 false, L->fscope, NULL, 0, line, &isnew);
				lua_activate(L, bd, L->fscope);
				if (isnew && lua_toplevel(L) && v->lit && !v->isnil && bd->loc->typ)
					bd->loc->init = v->s;
				else
					lua_emit_assign(L, ind, bd->pgname, v->s);
			}
		}
		cx->pos = b;
		return;
	}

	if (ann)
		plx_err(cx, line, "a type annotation is only allowed on a single variable");

	/* field / element targets, and multiple assignment */
	{
		char	   *tgt[64],
				   *base[64],
				   *val[64];
		const char *tgtyp[64];
		char	  **trefs[64],
				  **vrefs[64];
		int			ntrefs[64],
					nvrefs[64];
		LuaBind    *imp[64];
		LuaV	   *vals[64];

		cx->pos = a;
		for (i = 0; i < nrhs; i++)
		{
			lua_refs_begin(L);
			vals[i] = lua_expr(L, as[i], ae[i], false);
			vrefs[i] = lua_refs_take(L, &nvrefs[i]);
			val[i] = vals[i]->s;
		}
		for (; i < nt; i++)
		{
			vals[i] = NULL;
			val[i] = "NULL";
			vrefs[i] = NULL;
			nvrefs[i] = 0;
		}
		for (i = 0; i < nt; i++)
		{
			imp[i] = NULL;
			if (te[i] == ta[i] + 1)
			{
				Tok		   *nmk = &t[ta[i]];
				LuaBind    *bd;

				if (nmk->kind != T_IDENT)
					plx_err(cx, line, "cannot assign to this expression");
				bd = lua_lookup(L, nmk->s, nmk->len);
				if (bd)
				{
					if (bd->isconst)
						plx_err(cx, line, "attempt to assign to const variable \"%s\"", bd->lname);
					if (vals[i])
						lua_learn_type(bd, vals[i]->typ, vals[i]->rec);
					tgt[i] = bd->pgname;
					tgtyp[i] = lua_bind_typ(bd);
				}
				else if (lua_param(cx, nmk->s, nmk->len) >= 0)
				{
					tgt[i] = lua_param_ref(cx, nmk->s, nmk->len);
					tgtyp[i] = lua_param_typ(cx, lua_param(cx, nmk->s, nmk->len));
				}
				else if (lua_is_libns(nmk))
					plx_err(cx, line, "cannot assign to this expression");
				else
				{
					bool		isnew;

					imp[i] = lua_declare(L, nmk->s, nmk->len, vals[i] ? vals[i]->typ : NULL,
										 vals[i] && vals[i]->rec, false, L->fscope,
										 NULL, 0, line, &isnew);
					tgt[i] = imp[i]->pgname;
					tgtyp[i] = lua_bind_typ(imp[i]);
				}
				base[i] = tgt[i];
				trefs[i] = NULL;
				ntrefs[i] = 0;
			}
			else
			{
				LuaBind    *bd;
				LuaV	   *lv;

				lua_refs_begin(L);
				lv = lua_lvalue(L, ta[i], te[i], &bd);
				trefs[i] = lua_refs_take(L, &ntrefs[i]);
				/* the base is assigned into, not read: only the subscripts
				 * and the rest of the target count as reads */
				if (ntrefs[i] > 0)
				{
					trefs[i]++;
					ntrefs[i]--;
				}
				tgt[i] = lv->s;
				tgtyp[i] = NULL;
				base[i] = bd ? bd->pgname : pnstrdup(t[ta[i]].s, t[ta[i]].len);
			}
		}
		for (i = 0; i < nt; i++)
			if (imp[i])
				lua_activate(L, imp[i], L->fscope);
		lua_parallel(L, ind, nt, tgt, tgtyp, base, trefs, ntrefs, val, vrefs, nvrefs, line);
	}
	cx->pos = b;
}

static void
lua_if(Lua *L, int ind)
{
	Ctx		   *cx = L->cx;
	Tok		   *t = cx->t;
	StringInfo	o;
	int			a = cx->pos,
				line = t[a].line,
				th,
				ca;
	bool		neg = false;

	th = lua_find_kw(L, a + 1, KW_THEN, "then", line);
	ca = a + 1;
	if (tk_kw(&t[ca], KW_NOT))
	{
		neg = true;
		ca++;
	}
	if (tk_name(&t[ca], "pcall") && t[ca + 1].kind == T_LPAREN &&
		lua_pcall_shape(L, ca) == th)
	{
		/* if [not] pcall(function() ... end) then */
		char	   *ok = lua_hidden(L, "ok", "boolean");

		lua_pcall(L, ind, ca, ok, NULL);
		o = &cx->out;
		plx_indent(o, ind);
		appendStringInfo(o, "IF %s%s THEN\n", neg ? "NOT " : "", ok);
	}
	else
	{
		char	   *c;

		cx->pos = a;
		c = lua_cond(L, a + 1, th);
		o = &cx->out;
		plx_indent(o, ind);
		appendStringInfo(o, "IF %s THEN\n", c);
	}
	cx->pos = th + 1;
	lua_scope_push(L);
	lua_block(L, ind + 1);
	lua_scope_pop(L);
	while (tk_kw(&t[cx->pos], KW_ELSIF))
	{
		int			e = cx->pos;
		int			eth = lua_find_kw(L, e + 1, KW_THEN, "then", t[e].line);
		char	   *c = lua_cond(L, e + 1, eth);

		plx_indent(o, ind);
		appendStringInfo(o, "ELSIF %s THEN\n", c);
		cx->pos = eth + 1;
		lua_scope_push(L);
		lua_block(L, ind + 1);
		lua_scope_pop(L);
	}
	if (tk_kw(&t[cx->pos], KW_ELSE))
	{
		plx_indent(o, ind);
		appendStringInfoString(o, "ELSE\n");
		cx->pos++;
		lua_scope_push(L);
		lua_block(L, ind + 1);
		lua_scope_pop(L);
	}
	lua_expect(L, KW_END, "end", line);
	plx_indent(o, ind);
	appendStringInfoString(o, "END IF;\n");
}

/* Open a loop. opener is its do (or repeat) token; after is the token where
 * a break-target label would stand. Records the goto idioms' labels. */
static LuaLoop *
lua_loop_push(Lua *L, int kind, int opener, int after)
{
	Ctx		   *cx = L->cx;
	Tok		   *t = cx->t;
	LuaLoop    *lp = palloc0(sizeof(LuaLoop));
	int			closer = L->match[opener];
	int			j;

	if (closer < 0)
		plx_err(cx, t[opener].line, "'%s' expected to close the loop",
				kind == LOOP_REPEAT ? "until" : "end");
	lp->kind = kind;
	lp->id = ++L->seq;
	/* continue idiom: ::name:: as the last statement of the body */
	j = closer - 1;
	while (j > opener && (t[j].kind == T_NEWLINE || t[j].kind == T_SEMI))
		j--;
	if (j - 2 > opener && plx_tok_is(&t[j], "::") && t[j - 1].kind == T_IDENT &&
		plx_tok_is(&t[j - 2], "::"))
	{
		lp->cont_label = pnstrdup(t[j - 1].s, t[j - 1].len);
		L->labelok[j - 2] = true;
	}
	/* break target: ::name:: right after the loop */
	if (after >= 0 && after < L->ntok)
	{
		j = after;
		while (j < L->ntok && (t[j].kind == T_NEWLINE || t[j].kind == T_SEMI))
			j++;
		if (j + 2 < L->ntok && plx_tok_is(&t[j], "::") && t[j + 1].kind == T_IDENT &&
			plx_tok_is(&t[j + 2], "::"))
		{
			lp->brk_label = pnstrdup(t[j + 1].s, t[j + 1].len);
			L->labelok[j] = true;
		}
	}
	lp->outer = L->loops;
	L->loops = lp;
	L->inloop++;
	cx->loopdepth++;
	return lp;
}

static void
lua_loop_pop(Lua *L)
{
	L->loops = L->loops->outer;
	L->inloop--;
	L->cx->loopdepth--;
}

/* emit a finished loop: optional label, header, prologue, body, footer */
static void
lua_loop_emit(Lua *L, int ind, LuaLoop *lp, const char *header,
			  const char *prologue, const char *body)
{
	StringInfo	o = &L->cx->out;

	if (lp->need_label)
	{
		char	   *lbl = psprintf("__plx_lp_%d", lp->id);

		plx_indent(o, ind);
		appendStringInfo(o, "<<%s>>\n", lbl);
		plx_label_note(L->cx, lbl, (int) strlen(lbl));
	}
	plx_indent(o, ind);
	appendStringInfo(o, "%s\n", header);
	if (prologue)
		appendStringInfoString(o, prologue);
	appendStringInfoString(o, body);
	plx_indent(o, ind);
	appendStringInfoString(o, "END LOOP;\n");
}

static void
lua_while(Lua *L, int ind)
{
	Ctx		   *cx = L->cx;
	Tok		   *t = cx->t;
	int			a = cx->pos,
				line = t[a].line;
	int			dok = lua_find_kw(L, a + 1, KW_DO, "do", line);
	char	   *c;
	char	   *body;
	LuaLoop    *lp;

	cx->pos = a;
	c = lua_cond(L, a + 1, dok);
	lp = lua_loop_push(L, LOOP_WHILE, dok, L->match[dok] >= 0 ? L->match[dok] + 1 : -1);
	lua_scope_push(L);
	cx->pos = dok + 1;
	body = lua_capture(L, ind + 1);
	lua_expect(L, KW_END, "end", line);
	lua_scope_pop(L);
	lua_loop_pop(L);
	lua_loop_emit(L, ind, lp, psprintf("WHILE %s LOOP", c), NULL, body);
	pfree(body);
}

static void
lua_repeat(Lua *L, int ind)
{
	Ctx		   *cx = L->cx;
	Tok		   *t = cx->t;
	int			a = cx->pos,
				line = t[a].line;
	int			u = L->match[a],
				ue;
	char	   *body,
			   *c;
	LuaLoop    *lp;
	StringInfoData full;

	if (u < 0)
		plx_err(cx, line, "'until' expected to close the repeat");
	ue = lua_stmt_end(L, u + 1);
	lp = lua_loop_push(L, LOOP_REPEAT, a, ue);
	lua_scope_push(L);
	cx->pos = a + 1;
	body = lua_capture(L, ind + 1);
	if (cx->pos != u)
		plx_err(cx, t[cx->pos].line, "'until' expected, found %s", tok_desc(&t[cx->pos]));
	if (u + 1 >= ue)
		plx_err(cx, t[u].line, "until requires a condition");
	/* the condition sees the body's locals */
	cx->pos = u;
	c = lua_cond(L, u + 1, ue);
	lua_scope_pop(L);
	lua_loop_pop(L);
	initStringInfo(&full);
	if (lp->rb_used)
	{
		plx_indent(&full, ind + 1);
		appendStringInfo(&full, "<<__plx_rb_%d>>\n", lp->id);
		plx_indent(&full, ind + 1);
		appendStringInfoString(&full, "BEGIN\n");
		/*
		 * Not re-indented: a newline inside a string literal in the body
		 * would gain the indentation and change the value.
		 */
		appendStringInfoString(&full, body);
		plx_indent(&full, ind + 1);
		appendStringInfoString(&full, "END;\n");
	}
	else
		appendStringInfoString(&full, body);
	pfree(body);
	plx_indent(&full, ind + 1);
	appendStringInfo(&full, "EXIT WHEN %s;\n", c);
	lua_loop_emit(L, ind, lp, "LOOP", NULL, full.data);
	pfree(full.data);
	cx->pos = ue;
}

/* an integer literal step (with an optional minus); *val gets it */
static bool
lua_int_step(Tok *t, int a, int b, int64 *val)
{
	bool		neg = false;
	const char *c;
	int64		r = 0;

	if (b - a == 2 && plx_tok_is(&t[a], "-"))
	{
		neg = true;
		a++;
	}
	if (b - a != 1 || t[a].kind != T_INT || t[a].len > 18)
		return false;
	for (c = t[a].s; c < t[a].s + t[a].len; c++)
	{
		if (*c < '0' || *c > '9')
			return false;
		r = r * 10 + (*c - '0');
	}
	*val = neg ? -r : r;
	return true;
}

/* for v = e1, e2 [, e3] do ... end */
static void
lua_numfor(Lua *L, int ind, int a, int dok)
{
	Ctx		   *cx = L->cx;
	Tok		   *t = cx->t;
	int			line = t[a].line;
	int			as[4],
				ae[4],
				n;
	LuaV	   *e1,
			   *e2,
			   *e3 = NULL;
	int64		step = 1;
	bool		litstep;
	bool		plain;
	LuaLoop    *lp;
	LuaBind    *bd;
	bool		isnew;
	char	   *header,
			   *prologue = NULL,
			   *body;

	n = lua_split(L, a + 3, dok, as, ae, 4);
	if (n < 2 || n > 3)
		plx_err(cx, line, "a numeric for takes a start, a limit and an optional step");
	cx->pos = a;
	e1 = lua_expr(L, as[0], ae[0], false);
	e2 = lua_expr(L, as[1], ae[1], false);
	if (n == 3)
		e3 = lua_expr(L, as[2], ae[2], false);
	litstep = (n == 2) || lua_int_step(t, as[2], ae[2], &step);
	if (litstep && step == 0)
		plx_err(cx, line, "'for' step is zero");
	plain = litstep && !(typ_rank(e1->typ) >= 3);

	lp = lua_loop_push(L, LOOP_FOR, dok, L->match[dok] >= 0 ? L->match[dok] + 1 : -1);
	lua_scope_push(L);
	if (plain)
	{
		int			r2 = typ_rank(e2->typ);
		const char *hi = e2->s;

		if (r2 != 1 && r2 != 2)
			hi = psprintf("%s(%s)", step > 0 ? "floor" : "ceil", e2->s);
		bd = lua_declare(L, t[a + 1].s, t[a + 1].len, NULL, false, true, L->scope,
						 NULL, 0, line, &isnew);
		lua_activate(L, bd, L->scope);
		if (step > 0)
			header = (step == 1)
				? psprintf("FOR %s IN %s..%s LOOP", bd->pgname, e1->s, hi)
				: psprintf("FOR %s IN %s..%s BY " INT64_FORMAT " LOOP", bd->pgname, e1->s, hi, step);
		else
			header = (step == -1)
				? psprintf("FOR %s IN REVERSE %s..%s LOOP", bd->pgname, e1->s, hi)
				: psprintf("FOR %s IN REVERSE %s..%s BY " INT64_FORMAT " LOOP",
						   bd->pgname, e1->s, hi, -step);
	}
	else
	{
		/* general: count k = 0 .. floor((limit - start) / step) and derive
		 * v = start + k * step, which handles a step of either sign */
		int			r1 = typ_rank(e1->typ),
					r3 = e3 ? typ_rank(e3->typ) : 1;
		const char *ty = ((r1 == 1 || r1 == 2) && (r3 == 1 || r3 == 2))
			? rank_typ(Max(r1, r3)) : "numeric";
		char	   *sa = lua_hidden(L, "a", ty);
		char	   *ss = lua_hidden(L, "s", ty);
		char	   *k = psprintf("__plx_k_%d", ++L->seq);
		StringInfoData pro;

		lua_emit_assign(L, ind, sa, e1->s);
		lua_emit_assign(L, ind, ss, e3 ? e3->s : "1");
		bd = lua_declare(L, t[a + 1].s, t[a + 1].len, ty, false, false, L->scope,
						 NULL, 0, line, &isnew);
		lua_activate(L, bd, L->scope);
		header = psprintf("FOR %s IN 0..floor(((%s) - %s)::numeric / %s) LOOP",
						  k, e2->s, sa, ss);
		initStringInfo(&pro);
		plx_indent(&pro, ind + 1);
		appendStringInfo(&pro, "%s := %s + %s * %s;\n", bd->pgname, sa, k, ss);
		prologue = pro.data;
	}
	cx->pos = dok + 1;
	body = lua_capture(L, ind + 1);
	lua_expect(L, KW_END, "end", line);
	lua_scope_pop(L);
	lua_loop_pop(L);
	lua_loop_emit(L, ind, lp, header, prologue, body);
	pfree(body);
}

/* for names in iterator do ... end */
/*
 * Does generated plpgsql body assign the variable named name: a line
 * "name := ...", or name in the target list after INTO?
 */
static bool
lua_body_assigns(const char *body, const char *name)
{
	size_t		nl = strlen(name);
	const char *p = body;

	while (*p)
	{
		const char *eol = strchr(p, '\n');
		const char *q = p,
				   *in;

		if (!eol)
			eol = p + strlen(p);
		CHECK_FOR_INTERRUPTS();
		while (q < eol && (*q == ' ' || *q == '\t'))
			q++;
		if ((size_t) (eol - q) > nl + 3 && strncmp(q, name, nl) == 0 &&
			strncmp(q + nl, " := ", 4) == 0)
			return true;
		for (in = q; (in = strstr(in, " INTO ")) != NULL && in < eol; in += 6)
		{
			const char *r = in + 6;

			if (strncmp(r, "STRICT ", 7) == 0)
				r += 7;

			for (;;)
			{
				const char *e = r;

				while (e < eol && *e != ',' && *e != ' ' && *e != ';')
					e++;
				if ((size_t) (e - r) == nl && strncmp(r, name, nl) == 0)
					return true;
				if (e < eol && *e == ',')
				{
					r = e + 1;
					while (r < eol && *r == ' ')
						r++;
					continue;
				}
				break;
			}
		}
		p = *eol ? eol + 1 : eol;
	}
	return false;
}

static void
lua_genfor(Lua *L, int ind, int a, int dok)
{
	Ctx		   *cx = L->cx;
	Tok		   *t = cx->t;
	int			line = t[a].line;
	int			nm[2],
				nn = 0,
				i = a + 1,
				in,
				it;
	const char *ann = NULL;
	int			bodystart = dok + 1;
	LuaLoop    *lp = NULL;
	char	   *header = NULL,
			   *prologue = NULL,
			   *body;
	bool		isnew;
	/* ipairs over a variable: the live loop's parts */
	char	   *dk = NULL;
	const char *darr = NULL,
			   *dtyp = NULL;
	LuaBind    *dib = NULL,
			   *dvb = NULL;

	for (;;)
	{
		if (t[i].kind != T_IDENT)
			plx_err(cx, line, "for expects a variable name, found %s", tok_desc(&t[i]));
		if (nn >= 2)
			plx_err(cx, line, "a for loop takes at most two variables");
		nm[nn++] = i++;
		if (t[i].kind == T_COMMA)
		{
			i++;
			continue;
		}
		break;
	}
	if (!tk_kw(&t[i], KW_IN))
		plx_err(cx, line, "'in' or '=' expected in a for loop, found %s", tok_desc(&t[i]));
	in = i;
	it = in + 1;
	if (t[dok + 1].kind == T_TYPEANN)
	{
		ann = pnstrdup(t[dok + 1].ann, t[dok + 1].annlen);
		bodystart = dok + 2;
	}
	if (it >= dok || t[it].kind != T_IDENT || t[it + 1].kind != T_LPAREN)
		plx_err(cx, line, "for ... in expects query(...) or ipairs(...)");

	if (tk_name(&t[it], "query"))
	{
		int			as[16],
					ae[16],
					after,
					n;
		char	   *sql,
				   *binds;
		bool		dyn;
		LuaBind    *bd;

		if (nn != 1)
			plx_err(cx, line, "a query loop takes one row variable");
		n = plx_parse_args(cx, it, as, ae, 16, &after);
		if (after != dok)
			plx_err(cx, line, "unexpected %s after query(...)", tok_desc(&t[after]));
		if (n < 1)
			plx_err(cx, line, "query requires a SQL argument");
		cx->pos = it;
		dyn = plx_sql_arg(cx, as, ae, n, false, &sql, &binds);
		lp = lua_loop_push(L, LOOP_FOR, dok, L->match[dok] >= 0 ? L->match[dok] + 1 : -1);
		lua_scope_push(L);
		bd = lua_declare(L, t[nm[0]].s, t[nm[0]].len, ann, !ann, false, L->scope,
						 NULL, 0, line, &isnew);
		lua_activate(L, bd, L->scope);
		if (!dyn)
			header = psprintf("FOR %s IN %s LOOP", bd->pgname, sql);
		else
			header = psprintf("FOR %s IN EXECUTE %s%s%s LOOP", bd->pgname, sql,
							  binds ? " USING " : "", binds ? binds : "");
	}
	else if (tk_name(&t[it], "ipairs"))
	{
		int			as[4],
					ae[4],
					after,
					n;
		LuaV	   *arr;
		char	   *et = NULL;
		char	   *k = NULL;
		bool		dyn;
		bool		idx = (nn == 2 && !tk_name(&t[nm[0]], "_"));

		n = plx_parse_args(cx, it, as, ae, 4, &after);
		if (after != dok || n != 1)
			plx_err(cx, line, "ipairs takes one array");
		cx->pos = it;
		arr = lua_expr(L, as[0], ae[0], false);
		/*
		 * Lua's ipairs reads t[i] afresh on each step, so elements the body
		 * appends or changes are seen. That is visible only when the array
		 * is a variable the body can change: then the loop re-reads its
		 * length and the element on each step. Any other array expression
		 * makes a new array that nothing else can reach, and FOR/FOREACH
		 * over it gives the same result.
		 */
		dyn = (ae[0] == as[0] + 1 && t[as[0]].kind == T_IDENT);
		if (dyn)
		{
			StringInfo	o = &cx->out;

			k = lua_hidden(L, "k", "integer");
			lua_emit_assign(L, ind, k, "0");
			/* ipairs(nil) is an error in Lua, as FOREACH over NULL is */
			plx_indent(o, ind);
			appendStringInfo(o, "IF %s IS NULL THEN RAISE EXCEPTION USING ERRCODE = '22004', "
							 "MESSAGE = 'ipairs expects an array, got nil'; END IF;\n", arr->s);
		}
		lp = lua_loop_push(L, LOOP_FOR, dok, L->match[dok] >= 0 ? L->match[dok] + 1 : -1);
		lua_scope_push(L);
		if (dyn)
		{
			LuaBind    *ib = NULL,
					   *vb = NULL;

			if (nn == 2)
			{
				if (ann)
					et = (char *) ann;
				else if (arr->typ)
					et = typ_elem(arr->typ);
				if (!et && !lua_lookup(L, t[as[0]].s, t[as[0]].len))
				{
					Oid			o = lua_param_oid(cx, lua_param(cx, t[as[0]].s, t[as[0]].len));
					Oid			el = OidIsValid(o) ? get_element_type(o) : InvalidOid;

					if (OidIsValid(el))
						et = format_type_be(el);
				}
				if (!et)
					plx_err(cx, line,
							"cannot infer the element type of the array; annotate it after do, e.g. for i, v in ipairs(a) do --:: integer");
			}
			if (nn == 1 || idx)
			{
				ib = lua_declare(L, t[nm[0]].s, t[nm[0]].len, "integer", false, false,
								 L->scope, NULL, 0, line, &isnew);
				lua_activate(L, ib, L->scope);
			}
			if (nn == 2)
			{
				vb = lua_declare(L, t[nm[1]].s, t[nm[1]].len, et, false, false, L->scope,
								 NULL, 0, line, &isnew);
				lua_activate(L, vb, L->scope);
			}
			header = pstrdup("LOOP");
			/* the prologue is written once the body is known (see below) */
			dk = k;
			darr = arr->s;
			dtyp = arr->typ ? arr->typ :
				lua_param_typ(cx, lua_param(cx, t[as[0]].s, t[as[0]].len));
			dib = ib;
			dvb = vb;
		}
		else if (nn == 1)
		{
			/* for i in ipairs(a): the indexes */
			LuaBind    *bd = lua_declare(L, t[nm[0]].s, t[nm[0]].len, NULL, false, true,
										 L->scope, NULL, 0, line, &isnew);

			lua_activate(L, bd, L->scope);
			header = psprintf("FOR %s IN 1..cardinality(%s) LOOP", bd->pgname, arr->s);
		}
		else
		{
			LuaBind    *vb,
					   *ib = NULL;

			if (ann)
				et = (char *) ann;
			else if (arr->typ)
				et = typ_elem(arr->typ);
			if (!et && t[as[0]].kind == T_IDENT && ae[0] == as[0] + 1 &&
				!lua_lookup(L, t[as[0]].s, t[as[0]].len))
			{
				Oid			o = lua_param_oid(cx, lua_param(cx, t[as[0]].s, t[as[0]].len));
				Oid			el = OidIsValid(o) ? get_element_type(o) : InvalidOid;

				if (OidIsValid(el))
					et = format_type_be(el);
			}
			if (!et)
				plx_err(cx, line,
						"cannot infer the element type of the array; annotate it after do, e.g. for i, v in ipairs(a) do --:: integer");
			if (idx)
			{
				ib = lua_declare(L, t[nm[0]].s, t[nm[0]].len, "integer", false, false,
								 L->scope, NULL, 0, line, &isnew);
				lua_activate(L, ib, L->scope);
				lua_emit_assign(L, ind, ib->pgname, "0");
			}
			vb = lua_declare(L, t[nm[1]].s, t[nm[1]].len, et, false, false, L->scope,
							 NULL, 0, line, &isnew);
			lua_activate(L, vb, L->scope);
			header = psprintf("FOREACH %s IN ARRAY %s LOOP", vb->pgname, arr->s);
			if (ib)
			{
				StringInfoData pro;

				initStringInfo(&pro);
				plx_indent(&pro, ind + 1);
				appendStringInfo(&pro, "%s := %s + 1;\n", ib->pgname, ib->pgname);
				prologue = pro.data;
			}
		}
	}
	else if (tk_name(&t[it], "pairs"))
		plx_err(cx, line, "pairs() iterates a table as a map, which is not supported; use ipairs() for an array");
	else
		plx_err(cx, line, "for ... in expects query(...) or ipairs(...)");

	cx->pos = bodystart;
	body = lua_capture(L, ind + 1);
	if (dk)
	{
		/*
		 * Lua's ipairs keeps the table it was given: a body that assigns the
		 * variable itself (t = {...}, t = nil, t, a = ...) does not change
		 * the loop. The generated body shows every such assignment as
		 * "t := ..." or "INTO ..., t"; a local of the same name declared in
		 * the body has another plpgsql name. Then the loop walks a copy made
		 * at the start.
		 */
		const char *src = darr;
		StringInfoData pro;

		if (lua_body_assigns(body, darr))
		{
			if (!dtyp)
				plx_err(cx, line, "cannot infer the type of the array the loop body assigns; annotate it where it is declared");
			src = lua_hidden(L, "a", dtyp);
			lua_emit_assign(L, ind, src, darr);
		}
		initStringInfo(&pro);
		plx_indent(&pro, ind + 1);
		appendStringInfo(&pro, "%s := %s + 1;\n", dk, dk);
		plx_indent(&pro, ind + 1);
		appendStringInfo(&pro, "EXIT WHEN %s > coalesce(cardinality(%s), 0);\n", dk, src);
		if (dib)
		{
			plx_indent(&pro, ind + 1);
			appendStringInfo(&pro, "%s := %s;\n", dib->pgname, dk);
		}
		if (dvb)
		{
			plx_indent(&pro, ind + 1);
			appendStringInfo(&pro, "%s := %s[%s];\n", dvb->pgname, src, dk);
		}
		prologue = pro.data;
	}
	lua_expect(L, KW_END, "end", line);
	lua_scope_pop(L);
	lua_loop_pop(L);
	lua_loop_emit(L, ind, lp, header, prologue, body);
	pfree(body);
}

static void
lua_for(Lua *L, int ind)
{
	Ctx		   *cx = L->cx;
	Tok		   *t = cx->t;
	int			a = cx->pos;
	int			dok = lua_find_kw(L, a + 1, KW_DO, "do", t[a].line);

	if (t[a + 1].kind == T_IDENT && plx_tok_is(&t[a + 2], "="))
		lua_numfor(L, ind, a, dok);
	else
		lua_genfor(L, ind, a, dok);
}

/* return [values] */
static void
lua_return(Lua *L, int ind)
{
	Ctx		   *cx = L->cx;
	Tok		   *t = cx->t;
	int			a = cx->pos,
				line = t[a].line;
	int			b = lua_stmt_end(L, a);
	int			as[64],
				ae[64],
				n = 0;
	LuaLoop    *f;

	if (b > a + 1)
		n = lua_split(L, a + 1, b, as, ae, 64);
	for (f = L->loops; f; f = f->outer)
		if (f->kind == LOOP_FUNC)
			break;
	cx->pos = a;
	if (f)
	{
		if (n > 0)
			plx_err(cx, line, "returning a value from the function given to pcall is not supported");
		lua_pc_success(L, f, ind);
		plx_indent(&cx->out, ind);
		appendStringInfo(&cx->out, "EXIT __plx_pc_%d;\n", f->id);
		f->pc_ret = true;
	}
	else if (n == 0)
		plx_emit_exit(cx, &cx->out, ind, PLX_EXIT_RETURN, NULL, NULL, NULL);
	else if (cx->retset)
		plx_err(cx, line, "use emit(...) to produce rows in a set-returning function, and a bare return to end it");
	else
	{
		int			oidx[64];
		int			nout = lua_outs(cx, oidx, 64);

		if (nout > 0)
		{
			char	   *tgt[64],
					   *val[64];
			const char *tgtyp[64];
			char	  **trefs[64],
					  **vrefs[64];
			int			ntrefs[64],
						nvrefs[64],
						i;

			if (n != nout)
				plx_err(cx, line, "return gives %d values, but the function has %d OUT parameters",
						n, nout);
			for (i = 0; i < n; i++)
			{
				lua_refs_begin(L);
				val[i] = lua_expr(L, as[i], ae[i], false)->s;
				vrefs[i] = lua_refs_take(L, &nvrefs[i]);
				tgt[i] = (char *) quote_identifier(lua_param_name(cx, oidx[i]));
				tgtyp[i] = lua_param_typ(cx, oidx[i]);
				trefs[i] = NULL;
				ntrefs[i] = 0;
			}
			lua_parallel(L, ind, n, tgt, tgtyp, tgt, trefs, ntrefs, val, vrefs, nvrefs, line);
			plx_emit_exit(cx, &cx->out, ind, PLX_EXIT_RETURN, NULL, NULL, NULL);
		}
		else if (cx->meta && cx->meta->prokind == 'p')
			plx_err(cx, line, "a procedure cannot return a value");
		else if (n > 1)
			plx_err(cx, line, "multiple return values are not supported (declare OUT parameters to return several values)");
		else
			plx_emit_exit(cx, &cx->out, ind, PLX_EXIT_RETURN, NULL,
						  lua_expr(L, as[0], ae[0], false)->s, NULL);
	}
	cx->pos = b;
}

/* emit(...) / return_next(...) */
static void
lua_emit(Lua *L, int ind)
{
	Ctx		   *cx = L->cx;
	Tok		   *t = cx->t;
	int			a = cx->pos,
				line = t[a].line;
	int			b = lua_stmt_end(L, a);
	int			as[64],
				ae[64],
				n = 0,
				after;
	int			oidx[64];
	int			nout = lua_outs(cx, oidx, 64);

	if (!cx->retset)
		plx_err(cx, line, "emit() is only valid in a set-returning function");
	if (t[a + 1].kind == T_LPAREN)
	{
		n = plx_parse_args(cx, a, as, ae, 64, &after);
		if (after != b)
			plx_err(cx, line, "unexpected %s after emit(...)", tok_desc(&t[after]));
	}
	else if (b > a + 1)
	{
		as[0] = a + 1;
		ae[0] = b;
		n = 1;
	}
	cx->pos = a;
	if (n > 0 && nout > 0)
	{
		char	   *tgt[64],
				   *val[64];
		const char *tgtyp[64];
		char	  **trefs[64],
				  **vrefs[64];
		int			ntrefs[64],
					nvrefs[64],
					i;

		if (n != nout)
			plx_err(cx, line, "emit() takes %d values (one per OUT column) or none", nout);
		for (i = 0; i < n; i++)
		{
			lua_refs_begin(L);
			val[i] = lua_expr(L, as[i], ae[i], false)->s;
			vrefs[i] = lua_refs_take(L, &nvrefs[i]);
			tgt[i] = (char *) quote_identifier(lua_param_name(cx, oidx[i]));
			tgtyp[i] = lua_param_typ(cx, oidx[i]);
			trefs[i] = NULL;
			ntrefs[i] = 0;
		}
		lua_parallel(L, ind, n, tgt, tgtyp, tgt, trefs, ntrefs, val, vrefs, nvrefs, line);
		n = 0;
	}
	if (n > 1)
		plx_err(cx, line, "emit() takes one value");
	plx_indent(&cx->out, ind);
	if (n == 0)
		appendStringInfoString(&cx->out, "RETURN NEXT;\n");
	else
		appendStringInfo(&cx->out, "RETURN NEXT %s;\n", lua_expr(L, as[0], ae[0], false)->s);
	cx->pos = b;
}

static void
lua_break(Lua *L, int ind)
{
	Ctx		   *cx = L->cx;
	int			a = cx->pos;
	int			b = lua_stmt_end(L, a);

	if (b != a + 1)
		plx_err(cx, cx->t[a].line, "unexpected %s after break", tok_desc(&cx->t[a + 1]));
	if (!L->loops || L->loops->kind == LOOP_FUNC)
		plx_err(cx, cx->t[a].line, "break outside a loop");
	plx_emit_exit(cx, &cx->out, ind, PLX_EXIT_BREAK, NULL, NULL, NULL);
	cx->pos = b;
}

/* goto name: only the loop idioms (continue, and break to after a loop) */
static void
lua_goto(Lua *L, int ind)
{
	Ctx		   *cx = L->cx;
	Tok		   *t = cx->t;
	int			a = cx->pos,
				line = t[a].line;
	LuaLoop    *lp;
	bool		innermost = true;
	char	   *nm;

	if (t[a + 1].kind != T_IDENT)
		plx_err(cx, line, "goto expects a label name");
	nm = pnstrdup(t[a + 1].s, t[a + 1].len);
	for (lp = L->loops; lp && lp->kind != LOOP_FUNC; lp = lp->outer, innermost = false)
	{
		StringInfo	o = &cx->out;

		if (lp->cont_label && strcmp(lp->cont_label, nm) == 0)
		{
			plx_indent(o, ind);
			if (lp->kind == LOOP_REPEAT)
			{
				/* to the until test: leave the body block */
				lp->rb_used = true;
				appendStringInfo(o, "EXIT __plx_rb_%d;\n", lp->id);
			}
			else if (innermost)
				appendStringInfoString(o, "CONTINUE;\n");
			else
			{
				lp->need_label = true;
				appendStringInfo(o, "CONTINUE __plx_lp_%d;\n", lp->id);
			}
			cx->pos = a + 2;
			return;
		}
		if (lp->brk_label && strcmp(lp->brk_label, nm) == 0)
		{
			plx_indent(o, ind);
			if (innermost)
				appendStringInfoString(o, "EXIT;\n");
			else
			{
				lp->need_label = true;
				appendStringInfo(o, "EXIT __plx_lp_%d;\n", lp->id);
			}
			cx->pos = a + 2;
			return;
		}
	}
	plx_err(cx, line,
			"goto %s: goto is only supported for continue (::%s:: as the last statement of the loop body) and for break (::%s:: right after the loop)",
			nm, nm, nm);
}

/* print(...) -> RAISE NOTICE */
static void
lua_print(Lua *L, int a, int b, int ind)
{
	Ctx		   *cx = L->cx;
	int			as[64],
				ae[64],
				n,
				i;
	StringInfoData fmt,
				args;

	n = lua_call_args(L, a, b, as, ae, 64);
	cx->pos = a;
	if (n == 0)
	{
		plx_indent(&cx->out, ind);
		appendStringInfoString(&cx->out, "RAISE NOTICE '';\n");
		return;
	}
	initStringInfo(&fmt);
	initStringInfo(&args);
	for (i = 0; i < n; i++)
	{
		appendStringInfoString(&fmt, i ? "\\t%" : "%");
		appendStringInfo(&args, ", %s", lua_expr(L, as[i], ae[i], true)->s);
	}
	plx_indent(&cx->out, ind);
	appendStringInfo(&cx->out, "RAISE NOTICE %s'%s'%s;\n", n > 1 ? "E" : "", fmt.data, args.data);
}

/* error(msg [, level]) / error(err) / error{...} -> RAISE EXCEPTION */
static void
lua_error(Lua *L, int a, int b, int ind)
{
	Ctx		   *cx = L->cx;
	Tok		   *t = cx->t;
	int			as[4],
				ae[4],
				n;
	StringInfo	o = &cx->out;
	LuaBind    *eb = NULL;

	n = lua_call_args(L, a, b, as, ae, 4);
	cx->pos = a;
	if (n < 1)
		plx_err(cx, t[a].line, "error() requires a message");
	if (n > 2)
		plx_err(cx, t[a].line, "error() takes a message and an optional level");
	if (ae[0] == as[0] + 1 && t[as[0]].kind == T_IDENT)
		eb = lua_lookup(L, t[as[0]].s, t[as[0]].len);
	if (eb && eb->errid)
	{
		/* error(err) after pcall: raise it again with its SQLSTATE */
		int			id = eb->errid;
		char	   *st = lua_errf_name(id, 0),
				   *dt = lua_errf_name(id, 1),
				   *ht = lua_errf_name(id, 2);
		const char *m = eb->pgname;

		L->errv[id - 1]->mask |= LUA_ERRF_SQLSTATE | LUA_ERRF_DETAIL | LUA_ERRF_HINT;
		plx_indent(o, ind);
		appendStringInfo(o, "IF %s <> '' AND %s <> '' THEN RAISE EXCEPTION USING ERRCODE = %s, MESSAGE = %s, DETAIL = %s, HINT = %s;\n",
						 dt, ht, st, m, dt, ht);
		plx_indent(o, ind);
		appendStringInfo(o, "ELSIF %s <> '' THEN RAISE EXCEPTION USING ERRCODE = %s, MESSAGE = %s, DETAIL = %s;\n",
						 dt, st, m, dt);
		plx_indent(o, ind);
		appendStringInfo(o, "ELSIF %s <> '' THEN RAISE EXCEPTION USING ERRCODE = %s, MESSAGE = %s, HINT = %s;\n",
						 ht, st, m, ht);
		plx_indent(o, ind);
		appendStringInfo(o, "ELSE RAISE EXCEPTION USING ERRCODE = %s, MESSAGE = %s;\n", st, m);
		plx_indent(o, ind);
		appendStringInfoString(o, "END IF;\n");
		return;
	}
	if (t[as[0]].kind == T_LBRACE)
	{
		/* error{message = ..., sqlstate = ..., detail = ..., hint = ...} */
		static const struct
		{
			const char *k;
			const char *opt;
		}			keys[] = {
			{"message", "MESSAGE"}, {"sqlstate", "ERRCODE"}, {"errcode", "ERRCODE"},
			{"detail", "DETAIL"}, {"hint", "HINT"}, {"column", "COLUMN"},
			{"constraint", "CONSTRAINT"}, {"datatype", "DATATYPE"},
			{"table", "TABLE"}, {"schema", "SCHEMA"},
		};
		int			fa[16],
					fe[16],
					nf,
					i,
					k;
		StringInfoData using;
		int			seen = 0;

		if (ae[0] - as[0] < 3 || t[ae[0] - 1].kind != T_RBRACE)
			plx_err(cx, t[a].line, "error{...} needs at least a message");
		nf = lua_split(L, as[0] + 1, ae[0] - 1, fa, fe, 16);
		initStringInfo(&using);
		for (i = 0; i < nf; i++)
		{
			const char *opt = NULL;

			if (fe[i] == fa[i] && i == nf - 1)
				continue;		/* a trailing separator */
			if (fe[i] - fa[i] < 3 || t[fa[i]].kind != T_IDENT ||
				!plx_tok_is(&t[fa[i] + 1], "="))
				plx_err(cx, t[a].line, "error{...} takes key = value fields");
			for (k = 0; k < (int) lengthof(keys); k++)
				if (plx_name_eq(&t[fa[i]], keys[k].k))
				{
					opt = keys[k].opt;
					if (seen & (1 << k))
						plx_err(cx, t[a].line, "error{...}: \"%s\" is given twice", keys[k].k);
					seen |= 1 << k;
					break;
				}
			if (!opt)
				plx_err(cx, t[a].line, "error{...}: unknown field \"%.*s\" (use message, sqlstate, detail, hint, column, constraint, datatype, table or schema)",
						t[fa[i]].len, t[fa[i]].s);
			if (using.len)
				appendStringInfoString(&using, ", ");
			appendStringInfo(&using, "%s = %s", opt,
							 lua_expr(L, fa[i] + 2, fe[i], strcmp(opt, "MESSAGE") == 0)->s);
		}
		if (!using.len)
			plx_err(cx, t[a].line, "error{...} needs at least a message");
		plx_indent(o, ind);
		appendStringInfo(o, "RAISE EXCEPTION USING %s;\n", using.data);
		return;
	}
	plx_indent(o, ind);
	appendStringInfo(o, "RAISE EXCEPTION '%%', %s;\n", lua_expr(L, as[0], ae[0], true)->s);
}

static void
lua_assert(Lua *L, int a, int b, int ind)
{
	Ctx		   *cx = L->cx;
	int			as[4],
				ae[4],
				n;
	char	   *c;

	n = lua_call_args(L, a, b, as, ae, 4);
	cx->pos = a;
	if (n < 1 || n > 2)
		plx_err(cx, cx->t[a].line, "assert takes a condition and an optional message");
	c = lua_cond(L, as[0], ae[0]);
	plx_indent(&cx->out, ind);
	if (n == 2)
		appendStringInfo(&cx->out, "ASSERT %s, %s;\n", c, lua_expr(L, as[1], ae[1], true)->s);
	else
		appendStringInfo(&cx->out, "ASSERT %s;\n", c);
}

/* table.insert(t, [pos,] v) / table.remove(t [, pos]) */
static void
lua_table_stmt(Lua *L, int a, int b, int ind)
{
	Ctx		   *cx = L->cx;
	Tok		   *t = cx->t;
	int			as[4],
				ae[4],
				n,
				after;
	bool		ins = tk_name(&t[a + 2], "insert");
	LuaBind    *bd;
	LuaV	   *tv;
	char	   *T,
			   *Ts;

	n = plx_parse_args(cx, a + 2, as, ae, 4, &after);
	if (after != b)
		plx_err(cx, t[a].line, "unexpected %s after table.%s(...)", tok_desc(&t[after]),
				ins ? "insert" : "remove");
	cx->pos = a;
	if (ins ? (n < 2 || n > 3) : (n < 1 || n > 2))
		plx_err(cx, t[a].line, "%s", ins ? "table.insert takes an array, an optional position and a value"
				: "table.remove takes an array and an optional position");
	tv = lua_lvalue(L, as[0], ae[0], &bd);
	T = tv->s;
	Ts = (ae[0] == as[0] + 1) ? T : psprintf("(%s)", T);
	if (ins)
	{
		LuaV	   *v = lua_expr(L, as[n - 1], ae[n - 1], false);

		if (bd && bd->loc && !bd->loc->typ && v->typ && !typ_is_array(v->typ))
			bd->loc->typ = psprintf("%s[]", v->typ);
		if (n == 2)
			lua_emit_assign(L, ind, T, psprintf("array_append(%s, %s)", T, v->s));
		else
		{
			LuaV	   *p = lua_expr(L, as[1], ae[1], false);
			char	   *pp = lv_wrap(p, p->prec < SP_ADD);

			lua_emit_assign(L, ind, T, psprintf("array_cat(array_append(%s[:%s - 1], %s), %s[%s:])",
												Ts, pp, v->s, Ts, pp));
		}
	}
	else if (n == 1)
		lua_emit_assign(L, ind, T, psprintf("%s[:cardinality(%s) - 1]", Ts, T));
	else
	{
		LuaV	   *p = lua_expr(L, as[1], ae[1], false);
		char	   *pp = lv_wrap(p, p->prec < SP_ADD);

		lua_emit_assign(L, ind, T, psprintf("array_cat(%s[:%s - 1], %s[%s + 1:])", Ts, pp, Ts, pp));
	}
}

/* a statement that starts with a name: an assignment or a call */
static void
lua_namestmt(Lua *L, int ind)
{
	static const char *const delegated[] = {
		"perform", "execute", "return_query", "raise", "call", "commit",
		"rollback", "close_cursor", "move_cursor", NULL
	};
	Ctx		   *cx = L->cx;
	Tok		   *t = cx->t;
	int			a = cx->pos,
				b = lua_stmt_end(L, a),
				i,
				depth = 0,
				eq = -1;
	Tok		   *t0 = &t[a];

	for (i = a; i < b; i++)
	{
		TokKind		k = t[i].kind;

		if (k == T_LPAREN || k == T_LBRACKET || k == T_LBRACE)
			depth++;
		else if (k == T_RPAREN || k == T_RBRACKET || k == T_RBRACE)
			depth--;
		else if (k == T_KW && t[i].kw == KW_DEF && L->match[i] > i && L->match[i] < b)
			i = L->match[i];	/* step over a function body (pcall) */
		else if (depth == 0 && plx_tok_is(&t[i], "="))
		{
			eq = i;
			break;
		}
	}
	if (eq >= 0)
	{
		if (eq == a)
			plx_err(cx, t0->line, "a variable is expected before '='");
		lua_assign(L, a, eq, b, ind);
		return;
	}
	if (b > a && t[b - 1].kind == T_TYPEANN)
		plx_err(cx, t0->line, "a type annotation is only allowed on a local declaration or an assignment");

	if (tk_name(t0, "pcall") && !lua_lookup(L, t0->s, t0->len))
	{
		if (lua_pcall_shape(L, a) != b)
			plx_err(cx, t0->line, "unexpected tokens after pcall(...)");
		lua_pcall(L, ind, a, NULL, NULL);
		cx->pos = b;
		return;
	}
	if (tk_name(t0, "print") && !lua_lookup(L, t0->s, t0->len))
		lua_print(L, a, b, ind);
	else if (tk_name(t0, "error") && !lua_lookup(L, t0->s, t0->len))
		lua_error(L, a, b, ind);
	else if (tk_name(t0, "assert") && !lua_lookup(L, t0->s, t0->len))
		lua_assert(L, a, b, ind);
	else if (tk_name(t0, "table") && !lua_lookup(L, t0->s, t0->len) &&
			 t[a + 1].kind == T_DOT &&
			 (tk_name(&t[a + 2], "insert") || tk_name(&t[a + 2], "remove")) &&
			 t[a + 3].kind == T_LPAREN)
		lua_table_stmt(L, a, b, ind);
	else
	{
		for (i = 0; delegated[i]; i++)
			if (tk_name(t0, delegated[i]))
				break;
		if (delegated[i] && t[a + 1].kind == T_LPAREN && !lua_lookup(L, t0->s, t0->len))
		{
			int			as[16],
						ae[16],
						after;

			(void) plx_parse_args(cx, a, as, ae, 16, &after);
			if (after != b)
				plx_err(cx, t0->line, "unexpected %s after %.*s(...)",
						tok_desc(&t[after]), t0->len, t0->s);
			cx->pos = a;
			plx_emit_core(cx, a, b, ind, false);
		}
		else
		{
			/* any other call: PERFORM it (plpgsql cannot discard a value
			 * implicitly) */
			LuaV	   *v;

			cx->pos = a;
			v = lua_expr(L, a, b, false);
			if (!v->call)
				plx_err(cx, t0->line, "unsupported statement (an expression is not a statement)");
			plx_indent(&cx->out, ind);
			appendStringInfo(&cx->out, "PERFORM %s;\n", v->s);
		}
	}
	cx->pos = b;
}

static void
lua_stmt_inner(Lua *L, int ind)
{
	Ctx		   *cx = L->cx;
	Tok		   *t = cx->t;
	Tok		   *k = &t[cx->pos];

	switch (k->kind)
	{
		case T_KW:
			switch (k->kw)
			{
				case KW_LET:
					lua_local(L, ind);
					return;
				case KW_IF:
					lua_if(L, ind);
					return;
				case KW_WHILE:
					lua_while(L, ind);
					return;
				case KW_FOR:
					lua_for(L, ind);
					return;
				case KW_LOOP:
					lua_repeat(L, ind);
					return;
				case KW_DO:
					if (L->match[cx->pos] < 0)
						plx_err(cx, k->line, "'end' expected to close the do block");
					cx->pos++;
					lua_scope_push(L);
					lua_block(L, ind);
					lua_scope_pop(L);
					lua_expect(L, KW_END, "end", k->line);
					return;
				case KW_RETURN:
					lua_return(L, ind);
					return;
				case KW_BREAK:
					lua_break(L, ind);
					return;
				case KW_EMIT:
				case KW_RETURN_NEXT:
					lua_emit(L, ind);
					return;
				case KW_DEF:
					plx_err(cx, k->line,
							"function definitions are not supported (only the pcall(function() ... end) form)");
				default:
					plx_err(cx, k->line, "unexpected %s", tok_desc(k));
			}
		case T_IDENT:
			if (tk_name(k, "goto") && t[cx->pos + 1].kind == T_IDENT)
			{
				lua_goto(L, ind);
				return;
			}
			lua_namestmt(L, ind);
			return;
		case T_OP:
			if (plx_tok_is(k, "::"))
			{
				if (!L->labelok[cx->pos] || t[cx->pos + 1].kind != T_IDENT ||
					!plx_tok_is(&t[cx->pos + 2], "::"))
					plx_err(cx, k->line,
							"labels are only supported for the loop idioms: ::continue:: as the last statement of a loop body, or a break target right after a loop");
				cx->pos += 3;
				return;
			}
			plx_err(cx, k->line, "unexpected %s", tok_desc(k));
		case T_TYPEANN:
			plx_err(cx, k->line, "a type annotation must follow a local declaration, e.g. local x --:: numeric");
		case T_LPAREN:
			plx_err(cx, k->line, "a statement cannot start with '('");
		default:
			plx_err(cx, k->line, "unexpected %s", tok_desc(k));
	}
}

static void
lua_stmt(Lua *L, int ind)
{
	Ctx		   *cx = L->cx;

	check_stack_depth();
	CHECK_FOR_INTERRUPTS();
	if (++cx->depth > PLX_MAX_DEPTH)
		plx_err(cx, cx->t[cx->pos].line, "statements nested too deeply");
	lua_stmt_inner(L, ind);
	cx->depth--;
}

/* statements up to a block closer (end, else, elseif, until) or the end */
static void
lua_block(Lua *L, int ind)
{
	Ctx		   *cx = L->cx;

	for (;;)
	{
		Tok		   *k;

		plx_skip_seps(cx);
		k = &cx->t[cx->pos];
		if (k->kind == T_EOF)
			return;
		if (k->kind == T_KW && (k->kw == KW_END || k->kw == KW_ELSE ||
								k->kw == KW_ELSIF || k->kw == KW_UNTIL))
			return;
		lua_stmt(L, ind);
	}
}

/* pair every block opener (if, do, function, repeat) with its closer */
static void
lua_match_blocks(Lua *L)
{
	Tok		   *t = L->cx->t;
	int		   *stack,
				sp = 0,
				cap = 64,
				i;

	L->match = palloc(L->ntok * sizeof(int));
	stack = palloc(cap * sizeof(int));
	for (i = 0; i < L->ntok; i++)
	{
		L->match[i] = -1;
		if (t[i].kind != T_KW)
			continue;
		switch (t[i].kw)
		{
			case KW_IF:
			case KW_DO:
			case KW_DEF:
			case KW_LOOP:
				if (sp >= cap)
				{
					cap *= 2;
					stack = repalloc(stack, cap * sizeof(int));
				}
				stack[sp++] = i;
				break;
			case KW_END:
				if (sp > 0 && t[stack[sp - 1]].kw != KW_LOOP)
				{
					int			o = stack[--sp];

					L->match[o] = i;
					L->match[i] = o;
				}
				break;
			case KW_UNTIL:
				if (sp > 0 && t[stack[sp - 1]].kw == KW_LOOP)
				{
					int			o = stack[--sp];

					L->match[o] = i;
					L->match[i] = o;
				}
				break;
			default:
				break;
		}
	}
}

/* expand the pcall field-capture markers now that every read is known */
static void
lua_resolve_markers(Lua *L)
{
	Ctx		   *cx = L->cx;
	StringInfoData o;
	const char *p;
	int			i,
				f;

	if (!L->markers)
		return;
	for (i = 0; i < L->nerr; i++)
		for (f = 0; f < (int) lengthof(lua_errf); f++)
			if (L->errv[i]->mask & (1 << f))
			{
				char	   *nm = lua_errf_name(L->errv[i]->id, f);
				PlxLocal2  *l = plx_local_add(cx, nm, (int) strlen(nm));

				l->typ = "text";
			}
	initStringInfo(&o);
	p = cx->out.data;
	while (*p)
	{
		const char *q;
		char		kind;
		int			id;
		LuaErrVar  *ev;
		StringInfoData r;

		if (*p != LUA_MARK)
		{
			appendStringInfoChar(&o, *p++);
			continue;
		}
		q = strchr(p + 1, LUA_MARK);
		kind = p[1];
		id = atoi(p + 2);
		if (!q || (kind != 'S' && kind != 'H') || id < 1 || id > L->nerr)
			elog(ERROR, "plxlua: malformed pcall marker");
		ev = L->errv[id - 1];
		initStringInfo(&r);
		for (f = 0; f < (int) lengthof(lua_errf); f++)
			if (ev->mask & (1 << f))
			{
				char	   *nm = lua_errf_name(id, f);

				if (kind == 'S')
					appendStringInfo(&r, "%s%s := NULL;", r.len ? " " : "", nm);
				else
					appendStringInfo(&r, "%s%s = %s", r.len ? ", " : "GET STACKED DIAGNOSTICS ",
									 nm, lua_errf[f].item);
			}
		if (kind == 'H' && r.len)
			appendStringInfoChar(&r, ';');
		p = q + 1;
		if (r.len == 0)
		{
			/* nothing is read: drop the marker's line */
			while (o.len > 0 && o.data[o.len - 1] == ' ')
				o.len--;
			o.data[o.len] = '\0';
			if (*p == '\n')
				p++;
		}
		else
			appendStringInfoString(&o, r.data);
	}
	cx->out = o;
}

static void
lua_parse_body(Ctx *cx)
{
	Lua		   *L = palloc0(sizeof(Lua));
	int			n;

	if (strchr(cx->body, LUA_MARK))
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("plxlua: function body contains the control character U+0002")));
	L->cx = cx;
	cx->fe = L;
	plx_lex(cx);
	for (n = 0; cx->t[n].kind != T_EOF; n++)
		;
	L->ntok = n + 1;
	lua_match_blocks(L);
	L->labelok = palloc0(L->ntok * sizeof(bool));
	L->names = lua_htab("plxlua names", sizeof(LuaEnt));
	L->pgnames = lua_htab("plxlua plpgsql names", sizeof(LuaEnt));
	L->errhash = lua_htab("plxlua error variables", sizeof(LuaErrEnt));
	lua_scope_push(L);
	L->fscope = L->scope;

	lua_block(L, 1);
	if (cx->t[cx->pos].kind != T_EOF)
		plx_err(cx, cx->t[cx->pos].line, "unexpected %s", tok_desc(&cx->t[cx->pos]));
	lua_resolve_markers(L);
}

static const PlxSurface lua_surface = {
	.lanname = "plxlua",
	.block_style = PLX_BLK_KEYWORD_END,
	.stmt_semicolon = false,
	.var_sigil = 0,
	.cmt_hash = false,
	.cmt_slash = false,
	.cmt_block = false,
	.type_ann = "--::",			/* local total = 0 --:: numeric */
	.sq_is_raw = false,
	.interp_quote = 0,			/* Lua has no string interpolation */
	.concat_op = 0,
	.cmt_dashdash = true,
	.long_brackets = true,
	.lua_ops = true,
	.own_name_case = true,
	.kws = lua_kws,
	.nkws = lengthof(lua_kws),
	.excs = NULL,
	.nexcs = 0,
	.flags = PLX_TRUSTED,
	.parse_body = lua_parse_body,
	.rewrite_expr = lua_rewrite_hook,
	.str_value = lua_str_value_hook,
};

static char *
lua_transpile(const char *src, const PlxFuncMeta *meta)
{
	return plx_transpile(src, meta, &lua_surface, CurrentMemoryContext);
}

const PlxDialect plx_lua_dialect = {
	.abi_version = PLX_ABI_VERSION,
	.lanname = "plxlua",
	.transpile = lua_transpile,
	.flags = PLX_TRUSTED,
};

Datum
plx_lua_validator(PG_FUNCTION_ARGS)
{
	return plx_generic_validator(fcinfo, &plx_lua_dialect);
}

Datum
plx_lua_inline_handler(PG_FUNCTION_ARGS)
{
	return plx_generic_inline_handler(fcinfo, &plx_lua_dialect);
}

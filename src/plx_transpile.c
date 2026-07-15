/*
 * plx_transpile.c — plx shared Ruby->plpgsql transpiler (M3a vertical slice).
 *
 * Philosophy (see TRANSPILER.md): we do NOT parse Ruby expressions. plpgsql
 * expressions ARE SQL expressions. This is a statement-level restructurer: a
 * byte lexer isolates strings/comments/interpolation so structure keywords are
 * never confused with string bytes; a recursive walk lowers each recognized
 * construct to plpgsql, hoisting typed DECLAREs and rewriting a small fixed set
 * of operators/interpolations, passing everything else through verbatim.
 *
 * M3a covers: assignment (+ DECLARE-hoist/type-inference/const-folding),
 * if/elsif/else/unless (+ modifier), while/until (+ modifier), integer for,
 * loop..end, return, emit/return_next (SETOF), next/break (+ modifier), raise.
 * Anything outside the subset is rejected with a precise error at CREATE time.
 */
#include "postgres.h"

#include "fmgr.h"
#include "lib/stringinfo.h"
#include "mb/pg_wchar.h"
#include "utils/builtins.h"

#include "plx.h"
#include "plx_int.h"

/*
 * Cross-version shims. pg_noreturn (added in PG16) must be written as the very
 * first token of the declaration, before the storage class: write
 * "pg_noreturn static void f()", not "static pg_noreturn void f()". On PG20+
 * (C23) it expands to the standard [[noreturn]] attribute, whose placement is
 * strict; on older releases we define it as the GCC/clang attribute, which is
 * position-tolerant, so the same prefix spelling compiles everywhere.
 */
#ifndef pg_noreturn
#define pg_noreturn __attribute__((noreturn))
#endif

/* ---------------------------------------------------------------- tokens */

typedef enum
{
	T_EOF, T_NEWLINE, T_SEMI, T_IDENT, T_KW, T_INT, T_FLOAT, T_STRING,
	T_OP, T_LPAREN, T_RPAREN, T_LBRACKET, T_RBRACKET, T_LBRACE, T_RBRACE,
	T_PIPE, T_COMMA, T_DOT, T_TYPEANN, T_INDENT, T_DEDENT, T_ERROR
} TokKind;

/* Kw enum now lives in plx_int.h (shared with dialect surfaces). */

typedef struct
{
	TokKind		kind;
	Kw			kw;
	const char *s;				/* pointer into source body */
	int			len;
	int			line;
	bool		sq;				/* single-quoted string */
	bool		fstr;			/* Python f-string (interpolating) */
	char		quote;			/* opening quote char: ' " or ` */
	const char *ann;			/* TYPEANN: type text start */
	int			annlen;
} Tok;

/* ---------------------------------------------------------------- symtab */

typedef struct PlxLocal2
{
	char	   *name;			/* lower-cased key */
	char	   *typ;			/* plpgsql type text, or NULL if RECORD */
	bool		is_record;
	bool		is_const;		/* CONSTANT declaration */
	char	   *init;			/* folded initializer text, or NULL */
	struct PlxLocal2 *next;
} PlxLocal2;

typedef struct
{
	const char *body;
	Tok		   *t;
	int			nt, pos;
	const PlxFuncMeta *meta;
	const PlxSurface *surf;
	StringInfoData out;			/* emitted BEGIN..END body */
	PlxLocal2  *locals, *ltail;
	int			loopdepth;
	int			depth;			/* recursion depth guard */
	int			handlerdepth;	/* inside a rescue handler body */
	int			subq;			/* __plx_fo_N / __plx_p_N counter */
	const char *exc_var;		/* current rescue exception var name */
	int			exc_varlen;
	int			diag_mask;		/* stacked-diagnostics fields used in this handler */
	bool		retset;
	MemoryContext mcx;
} Ctx;

/* forward decls */
#define PLX_MAX_DEPTH 500		/* recursion cap (parsers + expression rewriter) */

/* stacked-diagnostics field bits (e.detail, e.hint, ...) */
#define PLX_DIAG_DETAIL     0x01
#define PLX_DIAG_HINT       0x02
#define PLX_DIAG_CONSTRAINT 0x04
#define PLX_DIAG_COLUMN     0x08
#define PLX_DIAG_TABLE      0x10
#define PLX_DIAG_SCHEMA     0x20
#define PLX_DIAG_DATATYPE   0x40

pg_noreturn static void plx_err(Ctx *cx, int line, const char *fmt,...) pg_attribute_printf(3, 4);
static char *rewrite_expr(Ctx *cx, const char *s, int len, bool boolctx);
static char *rewrite_expr_inner(Ctx *cx, const char *s, int len, bool boolctx);
static char *rw_range(Ctx *cx, int a, int b, bool boolctx);
static char *span_text(Ctx *cx, int a, int b);
static void parse_stmt(Ctx *cx, int indent, bool toplevel);
static void parse_stmt_inner(Ctx *cx, int indent, bool toplevel);
static void parse_brace_stmt_inner(Ctx *cx, int ind, bool toplevel);
static void parse_block(Ctx *cx, int indent);
static void parse_iter(Ctx *cx, int a, int do_pos, int e, int ind);
static void parse_begin(Ctx *cx, int ind);
static void parse_case(Ctx *cx, int ind);
static void emit_core(Ctx *cx, int a, int b, int ind, bool toplevel);
static void parse_brace_program(Ctx *cx);
static void parse_brace_block(Ctx *cx, int ind);
static void parse_switch_brace(Ctx *cx, int ind);
static void parse_py_program(Ctx *cx);
static void parse_py_block(Ctx *cx, int ind);
static void parse_py_stmt(Ctx *cx, int ind, bool toplevel);
static void parse_py_stmt_inner(Ctx *cx, int ind, bool toplevel);
static void parse_brace_stmt(Ctx *cx, int ind, bool toplevel);

/* ---------------------------------------------------------------- errors */

pg_noreturn static void
plx_err(Ctx *cx, int line, const char *fmt,...)
{
	char		buf[512];
	va_list		ap;

	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	ereport(ERROR,
			(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			 errmsg("%s: %s", cx->surf->lanname, buf),
			 errdetail_internal("at %s source line %d", cx->surf->lanname, line)));
}

/* recursion-guarded wrappers: cap total parser/rewriter depth to avoid stack
 * overflow on hostile deeply-nested input. */
static char *
rewrite_expr(Ctx *cx, const char *s, int len, bool boolctx)
{
	char	   *r;

	if (++cx->depth > PLX_MAX_DEPTH)
		plx_err(cx, 0, "expression nested too deeply");
	r = rewrite_expr_inner(cx, s, len, boolctx);
	cx->depth--;
	return r;
}

static void
parse_stmt(Ctx *cx, int indent, bool toplevel)
{
	if (++cx->depth > PLX_MAX_DEPTH)
		plx_err(cx, cx->t[cx->pos].line, "statements nested too deeply");
	parse_stmt_inner(cx, indent, toplevel);
	cx->depth--;
}

static void
parse_brace_stmt(Ctx *cx, int ind, bool toplevel)
{
	if (++cx->depth > PLX_MAX_DEPTH)
		plx_err(cx, cx->t[cx->pos].line, "statements nested too deeply");
	parse_brace_stmt_inner(cx, ind, toplevel);
	cx->depth--;
}

static void
parse_py_stmt(Ctx *cx, int ind, bool toplevel)
{
	if (++cx->depth > PLX_MAX_DEPTH)
		plx_err(cx, cx->t[cx->pos].line, "statements nested too deeply");
	parse_py_stmt_inner(cx, ind, toplevel);
	cx->depth--;
}

/* ---------------------------------------------------------------- lexer */

static Kw
kw_lookup(const PlxSurface *surf, const char *s, int len)
{
	int			i;

	for (i = 0; i < surf->nkws; i++)
		if ((int) strlen(surf->kws[i].w) == len &&
			strncmp(surf->kws[i].w, s, len) == 0)
			return surf->kws[i].k;
	return KW_NONE;
}

static bool
is_ident_start(char c)
{
	return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}
static bool
is_ident(char c)
{
	return is_ident_start(c) || (c >= '0' && c <= '9');
}

/* previous significant token cannot end an expression -> newline suppressed */
static bool
suppress_newline(Tok *t, int n)
{
	Tok		   *p;

	if (n == 0)
		return true;
	p = &t[n - 1];
	switch (p->kind)
	{
		case T_OP:
		case T_COMMA:
		case T_DOT:
		case T_LPAREN:
		case T_LBRACKET:
		case T_LBRACE:
			return true;
			/* NB: T_PIPE is NOT a continuation — `do |v|` must end the line
			 * so the block header is separated from the first body statement. */
		case T_KW:
			return (p->kw == KW_AND || p->kw == KW_OR || p->kw == KW_NOT);
		default:
			return false;
	}
}

static void
lex(Ctx *cx)
{
	const PlxSurface *surf = cx->surf;
	const char *p = cx->body;
	const char *end = p + strlen(p);
	int			line = 1;
	int			paren = 0;
	int			cap = 256, n = 0;
	Tok		   *t = palloc(sizeof(Tok) * cap);
	bool		indent_mode = (surf->block_style == PLX_BLK_INDENT);
	int			indent_stack[128];
	int			indent_sp = 0;

#define PUSH(k) do { \
		if (n >= cap) { cap *= 2; t = repalloc(t, sizeof(Tok) * cap); } \
		memset(&t[n], 0, sizeof(Tok)); \
		t[n].kind = (k); t[n].line = line; t[n].s = tokstart; \
		t[n].len = (int) (p - tokstart); n++; \
	} while (0)
#define PUSH0(k) do { \
		if (n >= cap) { cap *= 2; t = repalloc(t, sizeof(Tok) * cap); } \
		memset(&t[n], 0, sizeof(Tok)); \
		t[n].kind = (k); t[n].line = line; t[n].s = p; t[n].len = 0; n++; \
	} while (0)

	/* Python: set the base indentation from the first non-blank line. */
	if (indent_mode)
	{
		const char *q = p;
		int			col = 0;

		for (;;)
		{
			col = 0;
			while (*q == ' ' || *q == '\t')
			{
				col += (*q == '\t') ? 8 : 1;
				q++;
			}
			if (*q == '\n')
			{
				q++;
				continue;
			}
			if (*q == '#')
			{
				while (*q && *q != '\n')
					q++;
				continue;
			}
			break;
		}
		indent_stack[0] = col;
	}

	while (p < end)
	{
		const char *tokstart = p;

		if (*p == ' ' || *p == '\t' || *p == '\r')
		{
			p++;
			continue;
		}
		if (*p == '\n')
		{
			p++;
			line++;
			/* In Python only brackets continue a line; elsewhere a trailing
			 * operator (Ruby) continues it. */
			if (paren > 0 || (!indent_mode && suppress_newline(t, n)))
				continue;
			PUSH0(T_NEWLINE);
			if (indent_mode)
			{
				/* find the next real line's indentation, skipping blanks/comments */
				const char *q = p;
				int			col = 0;

				for (;;)
				{
					col = 0;
					while (*q == ' ' || *q == '\t')
					{
						col += (*q == '\t') ? 8 : 1;
						q++;
					}
					if (q < end && *q == '\n')
					{
						q++;
						line++;
						continue;
					}
					if (q < end && *q == '#')
					{
						while (q < end && *q != '\n')
							q++;
						continue;
					}
					break;		/* real line, or EOF */
				}
				if (q >= end)
					continue;	/* trailing dedents handled at EOF */
				if (col > indent_stack[indent_sp])
				{
					if (indent_sp < 126)
						indent_stack[++indent_sp] = col;
					PUSH0(T_INDENT);
				}
				else
				{
					while (indent_sp > 0 && col < indent_stack[indent_sp])
					{
						indent_sp--;
						PUSH0(T_DEDENT);
					}
				}
			}
			continue;
		}
		/* type annotation via a line lead, e.g. Ruby "#::" */
		if (surf->type_ann && strncmp(p, surf->type_ann, strlen(surf->type_ann)) == 0)
		{
			const char *a;
			int			al;

			p += strlen(surf->type_ann);
			while (*p == ' ' || *p == '\t')
				p++;
			a = p;
			while (p < end && *p != '\n')
				p++;
			al = (int) (p - a);
			while (al > 0 && (a[al - 1] == ' ' || a[al - 1] == '\t' || a[al - 1] == '\r'))
				al--;
			if (n >= cap) { cap *= 2; t = repalloc(t, sizeof(Tok) * cap); }
			memset(&t[n], 0, sizeof(Tok));
			t[n].kind = T_TYPEANN; t[n].line = line; t[n].ann = a; t[n].annlen = al;
			n++;
			continue;
		}
		/* block comment, plus the leading-colon-colon type-annotation form */
		if (surf->cmt_block && p[0] == '/' && p[1] == '*')
		{
			const char *q = p + 2;
			const char *close;

			while (*q == ' ' || *q == '\t')
				q++;
			if (q[0] == ':' && q[1] == ':')
			{
				const char *a = q + 2;
				int			al;

				while (*a == ' ' || *a == '\t')
					a++;
				close = strstr(a, "*/");
				if (!close)
					close = end;
				al = (int) (close - a);
				while (al > 0 && (a[al - 1] == ' ' || a[al - 1] == '\t'))
					al--;
				if (n >= cap) { cap *= 2; t = repalloc(t, sizeof(Tok) * cap); }
				memset(&t[n], 0, sizeof(Tok));
				t[n].kind = T_TYPEANN; t[n].line = line; t[n].ann = a; t[n].annlen = al;
				n++;
				p = (close < end) ? close + 2 : end;
				continue;
			}
			close = strstr(p + 2, "*/");
			p = close ? close + 2 : end;
			continue;
		}
		if (surf->cmt_hash && *p == '#')
		{
			while (p < end && *p != '\n')
				p++;
			continue;
		}
		if (surf->cmt_slash && p[0] == '/' && p[1] == '/')
		{
			while (p < end && *p != '\n')
				p++;
			continue;
		}
		/* variable sigil ($foo -> identifier "foo") */
		if (surf->var_sigil && *p == surf->var_sigil && is_ident_start(p[1]))
		{
			p++;
			tokstart = p;
			while (p < end && is_ident(*p))
				p++;
			PUSH(T_IDENT);
			continue;
		}
		/* Python f-string: f"..." / f'...' (interpolating) */
		if (surf->fstrings && (*p == 'f' || *p == 'F') &&
			(p[1] == '"' || p[1] == '\''))
		{
			char		q = p[1];

			p += 2;
			while (p < end && *p != q)
			{
				if (*p == '\\' && p + 1 < end)
					p++;
				p++;
			}
			if (p < end)
				p++;
			PUSH(T_STRING);		/* token spans f"..."; consumers skip the 'f' */
			t[n - 1].sq = (q == '\'');
			t[n - 1].quote = q;
			t[n - 1].fstr = true;
			continue;
		}
		if (is_ident_start(*p))
		{
			Kw			k;

			while (p < end && is_ident(*p))
				p++;
			if (*p == '?' || *p == '!')
				p++;			/* ruby method suffix */
			k = kw_lookup(surf, tokstart, (int) (p - tokstart));
			if (k != KW_NONE)
			{
				PUSH(T_KW);
				t[n - 1].kw = k;
			}
			else
				PUSH(T_IDENT);
			continue;
		}
		if (*p >= '0' && *p <= '9')
		{
			bool		isflt = false;

			while (p < end && ((*p >= '0' && *p <= '9') || *p == '_'))
				p++;
			if (*p == '.' && p[1] != '.' && (p[1] >= '0' && p[1] <= '9'))
			{
				isflt = true;
				p++;
				while (p < end && ((*p >= '0' && *p <= '9') || *p == '_'))
					p++;
			}
			if (*p == 'e' || *p == 'E')
			{
				isflt = true;
				p++;
				if (*p == '+' || *p == '-')
					p++;
				while (p < end && *p >= '0' && *p <= '9')
					p++;
			}
			PUSH(isflt ? T_FLOAT : T_INT);
			continue;
		}
		if (*p == '\'' || *p == '"' || *p == '`')
		{
			char		q = *p;

			p++;
			while (p < end && *p != q)
			{
				if (*p == '\\' && p + 1 < end)
					p++;
				p++;
			}
			if (p < end)
				p++;			/* closing quote */
			PUSH(T_STRING);
			t[n - 1].sq = (q == '\'');
			t[n - 1].quote = q;
			continue;
		}
		switch (*p)
		{
			case '(': p++; PUSH(T_LPAREN); paren++; continue;
			case ')': p++; PUSH(T_RPAREN); if (paren > 0) paren--; continue;
			case '[': p++; PUSH(T_LBRACKET); paren++; continue;
			case ']': p++; PUSH(T_RBRACKET); if (paren > 0) paren--; continue;
			case '{': p++; PUSH(T_LBRACE); continue;
			case '}': p++; PUSH(T_RBRACE); continue;
			case ',': p++; PUSH(T_COMMA); continue;
			case ';': p++; PUSH(T_SEMI); continue;
			case '.':
				if (p[1] == '.')
				{
					p += 2;
					if (*p == '.')
						p++;
					PUSH(T_OP);	/* range .. or ... */
					continue;
				}
				p++;
				PUSH(T_DOT);
				continue;
			default: break;
		}
		if (*p == '|')
		{
			p++;
			if (*p == '|')
			{
				p++;
				PUSH(T_OP);
			}
			else
				PUSH(T_PIPE);
			continue;
		}
		/* operator run */
		if (strchr("=!<>+-*/%&?:~^", *p))
		{
			while (p < end && strchr("=!<>+-*/%&?:~^", *p))
				p++;
			PUSH(T_OP);
			continue;
		}
		p++;
		PUSH(T_ERROR);
	}
	/* trailing newline, closing dedents, EOF */
	{
		const char *tokstart = p;

		if (n >= cap) { cap *= 2; t = repalloc(t, sizeof(Tok) * cap); }
		memset(&t[n], 0, sizeof(Tok));
		t[n].kind = T_NEWLINE; t[n].line = line; t[n].s = tokstart; n++;
		while (indent_mode && indent_sp > 0)
		{
			indent_sp--;
			PUSH0(T_DEDENT);
		}
		memset(&t[n], 0, sizeof(Tok));
		t[n].kind = T_EOF; t[n].line = line; t[n].s = tokstart; n++;
	}
	cx->t = t;
	cx->nt = n;
	cx->pos = 0;
#undef PUSH
}

/* ---------------------------------------------------------------- helpers */

static bool
tok_is(Tok *tk, const char *op)
{
	return tk->kind == T_OP && (int) strlen(op) == tk->len &&
		strncmp(tk->s, op, tk->len) == 0;
}

static bool
name_eq(Tok *tk, const char *w)
{
	int			l = (int) strlen(w);

	return tk->len == l && strncmp(tk->s, w, l) == 0;
}

/* is a declared type text-like (so string append applies)? */
static bool
is_stringy(const char *typ)
{
	if (!typ)
		return false;
	return (strcasestr(typ, "text") != NULL ||
			strcasestr(typ, "char") != NULL ||
			strcasestr(typ, "plx_strbuild") != NULL);
}

static void
skip_seps(Ctx *cx)
{
	while (cx->t[cx->pos].kind == T_NEWLINE || cx->t[cx->pos].kind == T_SEMI)
		cx->pos++;
}

/* case-insensitive name compare against a param list */
static bool
is_param(Ctx *cx, const char *name, int len)
{
	int			i;

	if (!cx->meta)
		return false;
	for (i = 0; i < cx->meta->nargs; i++)
	{
		const char *an = cx->meta->argnames ? cx->meta->argnames[i] : NULL;

		if (an && (int) strlen(an) == len && pg_strncasecmp(an, name, len) == 0)
			return true;
	}
	return false;
}

static PlxLocal2 *
local_find(Ctx *cx, const char *name, int len)
{
	PlxLocal2  *l;

	for (l = cx->locals; l; l = l->next)
		if ((int) strlen(l->name) == len && pg_strncasecmp(l->name, name, len) == 0)
			return l;
	return NULL;
}

static PlxLocal2 *
local_add(Ctx *cx, const char *name, int len)
{
	PlxLocal2  *l = palloc0(sizeof(PlxLocal2));

	l->name = pnstrdup(name, len);
	if (cx->ltail)
		cx->ltail->next = l;
	else
		cx->locals = l;
	cx->ltail = l;
	return l;
}

/* ---------------------------------------------------------------- string decode */

/*
 * If an interpolation starts at s within a double-quoted string body [s,e), set
 * exs/exl to the (raw) expression text (sigils included; rewrite strips them)
 * and next to just past the interpolation, and return true.
 *   Ruby: #{expr}   PHP: {$expr} and $var
 */
static bool
interp_at(const PlxSurface *surf, bool fstr, const char *s, const char *e,
		  const char **exs, int *exl, const char **next)
{
	if (fstr && s < e && s[0] == '{' && !(s + 1 < e && s[1] == '{'))	/* f-string {expr} */
	{
		int			depth = 1;
		const char *x = s + 1, *q = x;

		while (q < e && depth > 0)
		{
			if (*q == '{')
				depth++;
			else if (*q == '}')
			{
				depth--;
				if (!depth)
					break;
			}
			q++;
		}
		*exs = x;
		*exl = (int) (q - x);
		*next = (q < e) ? q + 1 : q;
		return true;
	}
	if (surf->interp_hashbrace && s + 1 < e && s[0] == '#' && s[1] == '{')
	{
		int			depth = 1;
		const char *x = s + 2, *q = x;

		while (q < e && depth > 0)
		{
			if (*q == '{')
				depth++;
			else if (*q == '}')
			{
				depth--;
				if (!depth)
					break;
			}
			q++;
		}
		*exs = x;
		*exl = (int) (q - x);
		*next = (q < e) ? q + 1 : q;
		return true;
	}
	if (surf->interp_dollarbrace && s + 1 < e && s[0] == '$' && s[1] == '{')	/* ${expr} (JS) */
	{
		int			depth = 1;
		const char *x = s + 2, *q = x;

		while (q < e && depth > 0)
		{
			if (*q == '{')
				depth++;
			else if (*q == '}')
			{
				depth--;
				if (!depth)
					break;
			}
			q++;
		}
		*exs = x;
		*exl = (int) (q - x);
		*next = (q < e) ? q + 1 : q;
		return true;
	}
	if (surf->interp_dollar)
	{
		if (s + 1 < e && s[0] == '{' && s[1] == '$')	/* {$expr} */
		{
			int			depth = 1;
			const char *x = s + 1, *q = x;

			while (q < e && depth > 0)
			{
				if (*q == '{')
					depth++;
				else if (*q == '}')
				{
					depth--;
					if (!depth)
						break;
				}
				q++;
			}
			*exs = x;
			*exl = (int) (q - x);
			*next = (q < e) ? q + 1 : q;
			return true;
		}
		if (s + 1 < e && s[0] == '$' && is_ident_start(s[1]))	/* $var */
		{
			const char *q = s + 1;

			while (q < e && is_ident(*q))
				q++;
			*exs = s;
			*exl = (int) (q - s);
			*next = q;
			return true;
		}
	}
	return false;
}

/* Emit a dialect string token (with quotes) as a plpgsql value expression. */
static void
emit_string_value(Ctx *cx, Tok *tk, StringInfo out)
{
	const char *s = tk->s + (tk->fstr ? 2 : 1);	/* skip [f]"opening quote */
	const char *e = tk->s + tk->len - 1;	/* before closing quote */
	StringInfoData lit;
	bool		need_e = false;
	int			nparts = 0;

	initStringInfo(&lit);

	/* We build parts manually to control E-string prefixing. */
	while (s < e)
	{
		const char *exs, *nx;
		int			exl;
		bool		can_interp = tk->fstr ||
			(cx->surf->interp_quote && tk->quote == cx->surf->interp_quote);

		/* f-string brace escapes: {{ -> {  and }} -> } */
		if (tk->fstr && ((s[0] == '{' && s + 1 < e && s[1] == '{') ||
						 (s[0] == '}' && s + 1 < e && s[1] == '}')))
		{
			appendStringInfoChar(&lit, s[0]);
			s += 2;
			continue;
		}
		if (can_interp && interp_at(cx->surf, tk->fstr, s, e, &exs, &exl, &nx))
		{
			char	   *rw;

			/* flush current literal chunk */
			if (lit.len > 0)
			{
				if (nparts++)
					appendStringInfoString(out, " || ");
				if (need_e)
					appendStringInfoChar(out, 'E');
				appendStringInfoChar(out, '\'');
				appendStringInfoString(out, lit.data);
				appendStringInfoChar(out, '\'');
				resetStringInfo(&lit);
				need_e = false;
			}
			rw = rewrite_expr(cx, exs, exl, false);
			s = nx;
			if (nparts++)
				appendStringInfoString(out, " || ");
			/*
			 * COALESCE to '' so an interpolated NULL renders as an empty string
			 * rather than propagating NULL through || and annihilating the whole
			 * string. This matches how the source languages render nil/null in
			 * interpolation (a value, never the absence of the whole string).
			 */
			appendStringInfo(out, "COALESCE((%s)::text, '')", rw);
			continue;
		}
		if (s[0] == '\\' && s + 1 < e)
		{
			char		c = s[1];

			s += 2;
			switch (c)
			{
				case 'n': appendStringInfoString(&lit, "\\n"); need_e = true; break;
				case 't': appendStringInfoString(&lit, "\\t"); need_e = true; break;
				case 'r': appendStringInfoString(&lit, "\\r"); need_e = true; break;
				case '\\': appendStringInfoString(&lit, "\\\\"); need_e = true; break;
				case '\'': appendStringInfoString(&lit, "''"); break;
				case '"': appendStringInfoChar(&lit, '"'); break;
				default: appendStringInfoChar(&lit, c); break;
			}
			continue;
		}
		if (s[0] == '\'')
		{
			appendStringInfoString(&lit, "''");
			s++;
			continue;
		}
		appendStringInfoChar(&lit, *s);
		s++;
	}
	/* trailing literal chunk (or the whole empty string) */
	if (lit.len > 0 || nparts == 0)
	{
		if (nparts++)
			appendStringInfoString(out, " || ");
		if (need_e)
			appendStringInfoChar(out, 'E');
		appendStringInfoChar(out, '\'');
		appendStringInfoString(out, lit.data);
		appendStringInfoChar(out, '\'');
	}
	pfree(lit.data);
#undef FLUSH_LIT
}

/* ---------------------------------------------------------------- expr rewrite */

/* find a top-level ternary '?' (depth 0, not in string). Returns index or -1. */
static int
find_ternary_q(const char *s, int len)
{
	int			i, depth = 0;

	for (i = 0; i < len; i++)
	{
		char		c = s[i];

		if (c == '\'' || c == '"')
		{
			char		q = c;

			i++;
			while (i < len && s[i] != q)
			{
				if (s[i] == '\\')
					i++;
				i++;
			}
			continue;
		}
		if (c == '(' || c == '[')
			depth++;
		else if (c == ')' || c == ']')
			depth--;
		else if (c == '?' && depth == 0)
			return i;
	}
	return -1;
}

/* find matching ':' for a ternary at depth 0 after position q */
static int
find_ternary_colon(const char *s, int len, int q)
{
	int			i, depth = 0, tern = 0;

	for (i = q + 1; i < len; i++)
	{
		char		c = s[i];

		if (c == '\'' || c == '"')
		{
			char		qq = c;

			i++;
			while (i < len && s[i] != qq)
			{
				if (s[i] == '\\')
					i++;
				i++;
			}
			continue;
		}
		if (c == '(' || c == '[')
			depth++;
		else if (c == ')' || c == ']')
			depth--;
		else if (depth == 0)
		{
			if (c == '?')
				tern++;
			else if (c == ':' && i + 1 < len && s[i + 1] != ':' &&
					 (i == 0 || s[i - 1] != ':'))
			{
				if (tern == 0)
					return i;
				tern--;
			}
		}
	}
	return -1;
}

/*
 * rewrite_expr: single left-to-right pass over source; honors strings; never
 * re-scans its own output. boolctx selects OR vs || and negation handling.
 */
static char *
rewrite_expr_inner(Ctx *cx, const char *s, int len, bool boolctx)
{
	const PlxSurface *surf = cx->surf;
	StringInfoData out;
	int			i;

	/* trim */
	while (len > 0 && (s[0] == ' ' || s[0] == '\t' || s[0] == '\n' || s[0] == '\r'))
	{
		s++;
		len--;
	}
	while (len > 0 && (s[len - 1] == ' ' || s[len - 1] == '\t' ||
					   s[len - 1] == '\n' || s[len - 1] == '\r'))
		len--;

	/* ternary first */
	{
		int			q = find_ternary_q(s, len);

		if (q >= 0)
		{
			int			c = find_ternary_colon(s, len, q);

			if (c > q)
			{
				char	   *cond;
				char	   *a;
				char	   *b;
				char	   *r;
				int			ta = q + 1,
							tn = c - q - 1;

				/* a ?: b (empty THEN arm, the elvis operator) would emit
				 * "CASE WHEN cond THEN  ELSE b END" -> a plpgsql syntax error */
				while (tn > 0 && (s[ta] == ' ' || s[ta] == '\t' ||
								  s[ta] == '\n' || s[ta] == '\r'))
				{
					ta++;
					tn--;
				}
				if (tn == 0)
					plx_err(cx, cx->t[cx->pos].line,
							"empty branch in ?: (the elvis operator is not supported; write a ? a : b)");
				cond = rewrite_expr(cx, s, q, true);
				a = rewrite_expr(cx, s + q + 1, c - q - 1, false);
				b = rewrite_expr(cx, s + c + 1, len - c - 1, false);
				r = psprintf("CASE WHEN %s THEN %s ELSE %s END", cond, a, b);
				return r;
			}
		}
	}

	initStringInfo(&out);
	i = 0;
	while (i < len)
	{
		char		c = s[i];

		/* strip variable sigil ($foo -> foo) */
		if (surf->var_sigil && c == surf->var_sigil && i + 1 < len && is_ident_start(s[i + 1]))
		{
			i++;
			continue;
		}
		/* string-concatenation operator (e.g. PHP '.') -> SQL || */
		if (surf->concat_op && c == surf->concat_op &&
			!(i + 1 < len && s[i + 1] >= '0' && s[i + 1] <= '9'))
		{
			appendStringInfoString(&out, " || ");
			i++;
			continue;
		}
		if (c == ' ' || c == '\t' || c == '\n' || c == '\r')
		{
			appendStringInfoChar(&out, ' ');
			while (i < len && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r'))
				i++;
			continue;
		}
		/*
		 * A SQL string-literal prefix (E'...', B'...', X'...', U&'...') passed
		 * through from a SQL expression: emit the prefix and the literal
		 * verbatim so we do not re-quote it (e.g. E'x' must not become EE'x').
		 */
		if ((c == 'E' || c == 'e' || c == 'B' || c == 'b' || c == 'X' || c == 'x') &&
			i + 1 < len && s[i + 1] == '\'' &&
			(i == 0 || !is_ident(s[i - 1])))
		{
			int			j = i + 1;

			appendStringInfoChar(&out, c);
			j++;				/* opening quote */
			appendStringInfoChar(&out, '\'');
			while (j < len && s[j] != '\'')
			{
				if (s[j] == '\\' && j + 1 < len)
				{
					appendStringInfoChar(&out, s[j]);
					j++;
				}
				appendStringInfoChar(&out, s[j]);
				j++;
			}
			if (j < len)
			{
				appendStringInfoChar(&out, '\'');
				j++;
			}
			i = j;
			continue;
		}
		if (c == '\'' || c == '"' || c == '`')
		{
			/* build a synthetic token to reuse emit_string_value */
			Tok			st;
			int			j = i + 1;

			while (j < len && s[j] != c)
			{
				if (s[j] == '\\')
					j++;
				j++;
			}
			if (j < len)
				j++;
			memset(&st, 0, sizeof(st));
			st.kind = T_STRING;
			st.s = s + i;
			st.len = j - i;
			st.sq = (c == '\'');
			st.quote = c;
			emit_string_value(cx, &st, &out);
			i = j;
			continue;
		}
		/* Python f-string in expression position: f"...{expr}..." */
		if (surf->fstrings && (c == 'f' || c == 'F') && i + 1 < len &&
			(s[i + 1] == '"' || s[i + 1] == '\''))
		{
			Tok			st;
			char		q = s[i + 1];
			int			j = i + 2;

			while (j < len && s[j] != q)
			{
				if (s[j] == '\\')
					j++;
				j++;
			}
			if (j < len)
				j++;
			memset(&st, 0, sizeof(st));
			st.kind = T_STRING;
			st.s = s + i;
			st.len = j - i;
			st.sq = (q == '\'');
			st.quote = q;
			st.fstr = true;
			emit_string_value(cx, &st, &out);
			i = j;
			continue;
		}
		if (is_ident_start(c))
		{
			int			j = i;
			int			wl;

			while (j < len && is_ident(s[j]))
				j++;
			wl = j - i;

			/* Python conversions: str(x)/int(x)/float(x) -> (x)::type */
			if (surf->fstrings && j < len && s[j] == '(' &&
				((wl == 3 && strncmp(s + i, "str", 3) == 0) ||
				 (wl == 3 && strncmp(s + i, "int", 3) == 0) ||
				 (wl == 5 && strncmp(s + i, "float", 5) == 0)))
			{
				const char *cast = (s[i] == 's') ? "text" :
					(s[i] == 'i') ? "integer" : "double precision";
				int			depth = 1,
							k = j + 1,
							argstart = j + 1;
				char	   *inner;

				while (k < len && depth > 0)
				{
					if (s[k] == '(')
						depth++;
					else if (s[k] == ')')
					{
						depth--;
						if (!depth)
							break;
					}
					k++;
				}
				inner = rewrite_expr(cx, s + argstart, k - argstart, false);
				appendStringInfo(&out, "(%s)::%s", inner, cast);
				i = (k < len) ? k + 1 : k;
				continue;
			}

			/* exception accessor via "." (Ruby) or "->" (PHP):
			 * e.message / $e->message -> SQLERRM ; .sqlstate/.code -> SQLSTATE */
			if (cx->exc_var && wl == cx->exc_varlen &&
				strncmp(s + i, cx->exc_var, wl) == 0)
			{
				int			sep = 0;

				if (j < len && s[j] == '.')
					sep = 1;
				else if (j + 1 < len && s[j] == '-' && s[j + 1] == '>')
					sep = 2;
				if (sep)
				{
					const char *fld = s + j + sep;
					int			f = j + sep, fl;

					while (f < len && is_ident(s[f]))
						f++;
					fl = f - (j + sep);
					if (fl == 7 && strncmp(fld, "message", 7) == 0)
					{
						appendStringInfoString(&out, "SQLERRM");
						i = f;
						continue;
					}
					if ((fl == 8 && strncmp(fld, "sqlstate", 8) == 0) ||
						(fl == 4 && strncmp(fld, "code", 4) == 0))
					{
						appendStringInfoString(&out, "SQLSTATE");
						i = f;
						continue;
					}
					/* stacked-diagnostics fields -> a temp filled by GET STACKED
					 * DIAGNOSTICS at the handler top (see parse_begin/parse_try) */
					{
						static const struct { const char *fld; int bit; } dtab[] = {
							{"detail", PLX_DIAG_DETAIL}, {"hint", PLX_DIAG_HINT},
							{"constraint", PLX_DIAG_CONSTRAINT}, {"column", PLX_DIAG_COLUMN},
							{"table", PLX_DIAG_TABLE}, {"schema", PLX_DIAG_SCHEMA},
							{"datatype", PLX_DIAG_DATATYPE},
						};
						int			k;
						bool		matched = false;

						for (k = 0; k < (int) (sizeof(dtab) / sizeof(dtab[0])); k++)
							if ((int) strlen(dtab[k].fld) == fl &&
								strncmp(fld, dtab[k].fld, fl) == 0)
							{
								char		tmp[64];

								cx->diag_mask |= dtab[k].bit;
								snprintf(tmp, sizeof(tmp), "__plx_%s", dtab[k].fld);
								if (!local_find(cx, tmp, (int) strlen(tmp)))
								{
									PlxLocal2  *tl = local_add(cx, tmp, (int) strlen(tmp));

									tl->typ = pstrdup("text");
								}
								appendStringInfoString(&out, tmp);
								i = f;
								matched = true;
								break;
							}
						if (matched)
							continue;
					}
				}
			}

			/* found() / found? -> the plpgsql FOUND special variable */
			if (wl == 5 && strncmp(s + i, "found", 5) == 0 &&
				((j < len && s[j] == '?') ||
				 (j + 1 < len && s[j] == '(' && s[j + 1] == ')')))
			{
				appendStringInfoString(&out, "FOUND");
				i = j + (s[j] == '?' ? 1 : 2);
				continue;
			}
			if ((wl == 3 && strncmp(s + i, "nil", 3) == 0) ||
				(wl == 4 && strncmp(s + i, "null", 4) == 0) ||
				(wl == 4 && strncmp(s + i, "None", 4) == 0) ||
				(wl == 9 && strncmp(s + i, "undefined", 9) == 0))
				appendStringInfoString(&out, "NULL");
			else if ((wl == 4 && strncmp(s + i, "true", 4) == 0) ||
					 (wl == 4 && strncmp(s + i, "True", 4) == 0))
				appendStringInfoString(&out, "true");
			else if ((wl == 5 && strncmp(s + i, "false", 5) == 0) ||
					 (wl == 5 && strncmp(s + i, "False", 5) == 0))
				appendStringInfoString(&out, "false");
			else if (wl == 3 && strncmp(s + i, "and", 3) == 0)
				appendStringInfoString(&out, "AND");
			else if (wl == 2 && strncmp(s + i, "or", 2) == 0)
				appendStringInfoString(&out, "OR");
			else if (wl == 3 && strncmp(s + i, "not", 3) == 0)
				appendStringInfoString(&out, "NOT ");
			else
				appendBinaryStringInfo(&out, s + i, wl);
			i = j;

			/* record subscript: r[:id] / r['id'] / r["id"] -> r.id */
			if (i < len && s[i] == '[')
			{
				int			k = i + 1;

				if (k < len && (s[k] == ':' || s[k] == '\'' || s[k] == '"'))
				{
					char		q = s[k];
					int			fs, fe;

					k++;
					fs = k;
					if (q == ':')
					{
						while (k < len && is_ident(s[k]))
							k++;
						fe = k;
					}
					else
					{
						while (k < len && s[k] != q)
							k++;
						fe = k;
						if (k < len)
							k++;
					}
					if (k < len && s[k] == ']' && fe > fs)
					{
						appendStringInfoChar(&out, '.');
						appendBinaryStringInfo(&out, s + fs, fe - fs);
						i = k + 1;
					}
				}
			}
			continue;
		}
		if (c >= '0' && c <= '9')
		{
			while (i < len && ((s[i] >= '0' && s[i] <= '9') || s[i] == '.' ||
							   s[i] == 'e' || s[i] == 'E' || s[i] == '_' ||
							   ((s[i] == '+' || s[i] == '-') && i > 0 &&
								(s[i - 1] == 'e' || s[i - 1] == 'E'))))
			{
				if (s[i] != '_')
					appendStringInfoChar(&out, s[i]);
				i++;
			}
			continue;
		}
		/* .to_i / .to_s / .to_f casts */
		if (c == '.' && i + 4 <= len && strncmp(s + i, ".to_", 4) == 0)
		{
			char		k = (i + 4 < len) ? s[i + 4] : '?';

			if (k == 'i') { appendStringInfoString(&out, "::integer"); i += 5; continue; }
			if (k == 's') { appendStringInfoString(&out, "::text"); i += 5; continue; }
			if (k == 'f') { appendStringInfoString(&out, "::double precision"); i += 5; continue; }
		}
		/* operator runs */
		if (strchr("=!<>+-*/%&|?:~^", c))
		{
			int			j = i;

			while (j < len && strchr("=!<>+-*/%&|?:~^", s[j]))
				j++;
			{
				int			ol = j - i;
				const char *op = s + i;

				/* -> record/object access (PHP) */
				if (ol == 2 && strncmp(op, "->", 2) == 0)
				{
					appendStringInfoChar(&out, '.');
					i = j;
					continue;
				}
				/* equality/inequality: ==, !=, ===, !== ; null-aware vs nil/null */
				if ((ol == 2 && (strncmp(op, "==", 2) == 0 || strncmp(op, "!=", 2) == 0)) ||
					(ol == 3 && (strncmp(op, "===", 3) == 0 || strncmp(op, "!==", 3) == 0)))
				{
					bool		eq = (op[0] == '=');
					int			k = j;

					while (k < len && (s[k] == ' ' || s[k] == '\t'))
						k++;
					if ((k + 3 <= len && strncmp(s + k, "nil", 3) == 0 &&
						 (k + 3 == len || !is_ident(s[k + 3]))) ||
						(k + 4 <= len && strncmp(s + k, "null", 4) == 0 &&
						 (k + 4 == len || !is_ident(s[k + 4]))) ||
						(k + 4 <= len && strncmp(s + k, "None", 4) == 0 &&
						 (k + 4 == len || !is_ident(s[k + 4]))))
					{
						appendStringInfoString(&out, eq ? " IS NULL" : " IS NOT NULL");
						i = k + (s[k + 1] == 'i' ? 3 : 4);
						continue;
					}
					appendStringInfoString(&out, eq ? " = " : " <> ");
					i = j;
					continue;
				}
				if (ol == 2 && strncmp(op, "&&", 2) == 0)
				{
					appendStringInfoString(&out, " AND ");
					i = j;
					continue;
				}
				if (ol == 2 && strncmp(op, "||", 2) == 0)
				{
					appendStringInfoString(&out, boolctx ? " OR " : " || ");
					i = j;
					continue;
				}
				if (ol == 2 && strncmp(op, "**", 2) == 0)
				{
					appendStringInfoChar(&out, '^');
					i = j;
					continue;
				}
				if (ol == 1 && op[0] == '!')
				{
					appendStringInfoString(&out, "NOT ");
					i = j;
					continue;
				}
				appendBinaryStringInfo(&out, op, ol);
				i = j;
				continue;
			}
		}
		appendStringInfoChar(&out, c);
		i++;
	}
	return out.data;
}

/* source text spanning tokens [a, b); empty (b <= a) yields "" */
static char *
span_text(Ctx *cx, int a, int b)
{
	const char *st, *en;

	if (b <= a)
		return pstrdup("");
	st = cx->t[a].s;
	en = cx->t[b - 1].s + cx->t[b - 1].len;
	if (en < st)
		return pstrdup("");
	return pnstrdup(st, (int) (en - st));
}

/* ---------------------------------------------------------------- type infer */

/* Is the token range [a,b) a single constant literal? returns type or NULL */
static const char *
const_literal_type(Ctx *cx, int a, int b)
{
	if (b - a != 1)
		return NULL;
	switch (cx->t[a].kind)
	{
		case T_INT: return "integer";
		case T_FLOAT: return "numeric";
		case T_STRING: return "text";
		case T_KW:
			if (cx->t[a].kw == KW_TRUE || cx->t[a].kw == KW_FALSE)
				return "boolean";
			return NULL;
		default: return NULL;
	}
}

/* ---------------------------------------------------------------- indent */

static void
indent(StringInfo o, int n)
{
	int			i;

	for (i = 0; i < n; i++)
		appendStringInfoString(o, "  ");
}

/* ---------------------------------------------------------------- leaves */

/* find end of current logical statement: index of NEWLINE/SEMI/EOF */
static int
stmt_end(Ctx *cx, int from)
{
	int			i = from;

	while (cx->t[i].kind != T_NEWLINE && cx->t[i].kind != T_SEMI &&
		   cx->t[i].kind != T_EOF)
		i++;
	return i;
}

/* find a trailing modifier keyword (if/unless/while/until) at top paren level */
static int
find_modifier(Ctx *cx, int a, int b)
{
	int			i, depth = 0;

	for (i = a + 1; i < b; i++)
	{
		Tok		   *tk = &cx->t[i];

		if (tk->kind == T_LPAREN || tk->kind == T_LBRACKET)
			depth++;
		else if (tk->kind == T_RPAREN || tk->kind == T_RBRACKET)
			depth--;
		else if (depth == 0 && tk->kind == T_KW &&
				 (tk->kw == KW_IF || tk->kw == KW_UNLESS ||
				  tk->kw == KW_WHILE || tk->kw == KW_UNTIL))
			return i;
	}
	return -1;
}

/* ---------------------------------------------------------------- intrinsics */

/* Parse NAME ( a, b, ... ). 'a' indexes NAME. Fills arg token ranges; returns
 * count; *after = index after the ')'. */
static int
parse_args(Ctx *cx, int a, int *as, int *ae, int maxargs, int *after)
{
	int			p = a + 1;
	int			depth, n = 0, s;

	if (cx->t[p].kind != T_LPAREN)
		plx_err(cx, cx->t[a].line, "expected '(' after %.*s", cx->t[a].len, cx->t[a].s);
	p++;
	s = p;
	depth = 1;
	while (cx->t[p].kind != T_EOF)
	{
		TokKind		k = cx->t[p].kind;

		if (k == T_LPAREN || k == T_LBRACKET)
			depth++;
		else if (k == T_RBRACKET)
			depth--;
		else if (k == T_RPAREN)
		{
			depth--;
			if (depth == 0)
				break;
		}
		else if (k == T_COMMA && depth == 1)
		{
			if (n < maxargs) { as[n] = s; ae[n] = p; }
			n++;
			s = p + 1;
		}
		p++;
	}
	if (cx->t[p].kind != T_RPAREN)
		plx_err(cx, cx->t[a].line, "unterminated argument list");
	if (p > s)
	{
		if (n < maxargs) { as[n] = s; ae[n] = p; }
		n++;
	}
	/* callers iterate i < n over fixed as[]/ae[] arrays sized maxargs; refuse
	 * an over-long list rather than let them read past the array end */
	if (n > maxargs)
		plx_err(cx, cx->t[a].line, "too many arguments (maximum %d)", maxargs);
	*after = p + 1;
	return n;
}

/* Emit a Ruby string token as raw SQL text (interp #{e} -> rewritten expr,
 * inline; no surrounding quotes). Used for static query/perform/fetch SQL. */
static void
emit_string_as_sql(Ctx *cx, Tok *tk, StringInfo out)
{
	const char *s = tk->s + (tk->fstr ? 2 : 1);
	const char *e = tk->s + tk->len - 1;

	bool		can_interp = tk->fstr ||
		(cx->surf->interp_quote && tk->quote == cx->surf->interp_quote);

	while (s < e)
	{
		const char *exs, *nx;
		int			exl;

		if (tk->fstr && ((s[0] == '{' && s + 1 < e && s[1] == '{') ||
						 (s[0] == '}' && s + 1 < e && s[1] == '}')))
		{
			appendStringInfoChar(out, s[0]);
			s += 2;
			continue;
		}
		if (can_interp && interp_at(cx->surf, tk->fstr, s, e, &exs, &exl, &nx))
		{
			appendStringInfoString(out, rewrite_expr(cx, exs, exl, false));
			s = nx;
			continue;
		}
		if (s[0] == '\\' && s + 1 < e && (s[1] == '"' || s[1] == '\\'))
		{
			appendStringInfoChar(out, s[1]);
			s += 2;
			continue;
		}
		appendStringInfoChar(out, *s);
		s++;
	}
}

/* first non-space word of a SQL string is a row-returning verb? */
static bool
sql_is_row_returning(Tok *tk)
{
	const char *s = tk->s + 1;
	const char *e = tk->s + tk->len - 1;
	static const char *verbs[] = {"select", "with", "values", "table", NULL};
	int			i;

	while (s < e && (*s == ' ' || *s == '\t' || *s == '\n' || *s == '('))
		s++;
	for (i = 0; verbs[i]; i++)
	{
		int			l = (int) strlen(verbs[i]);

		if (e - s >= l && pg_strncasecmp(s, verbs[i], l) == 0)
			return true;
	}
	return false;
}

static const char *
exc_class_to_condition(const char *s, int len)
{
	static const struct { const char *cls; const char *cond; } m[] = {
		{"PG::UniqueViolation", "unique_violation"},
		{"PG::ForeignKeyViolation", "foreign_key_violation"},
		{"PG::NotNullViolation", "not_null_violation"},
		{"PG::CheckViolation", "check_violation"},
		{"PG::NoDataFound", "no_data_found"},
		{"PG::RaiseException", "raise_exception"},
		{"ZeroDivisionError", "division_by_zero"},
	};
	int			i;

	for (i = 0; i < (int) (sizeof(m) / sizeof(m[0])); i++)
		if ((int) strlen(m[i].cls) == len && strncmp(m[i].cls, s, len) == 0)
			return m[i].cond;
	return NULL;
}

/* USING binds text from arg ranges [1..n); NULL if none */
static char *
binds_text(Ctx *cx, int *as, int *ae, int nargs)
{
	StringInfoData b;
	int			i;

	if (nargs <= 1)
		return NULL;
	initStringInfo(&b);
	for (i = 1; i < nargs; i++)
	{
		char	   *st = span_text(cx, as[i], ae[i]);

		if (i > 1)
			appendStringInfoString(&b, ", ");
		appendStringInfoString(&b, rewrite_expr(cx, st, (int) strlen(st), false));
	}
	return b.data;
}

static bool
arg_is_string_literal(Ctx *cx, int as, int ae)
{
	return (ae - as == 1) && cx->t[as].kind == T_STRING;
}

/* execute(SQL[, binds]) -> EXECUTE <val> [USING ...]; */
static void
emit_execute(Ctx *cx, int a, int ind)
{
	int			as[16], ae[16], after, n;
	char	   *sqlv, *binds, *st;

	n = parse_args(cx, a, as, ae, 16, &after);
	if (n < 1)
		plx_err(cx, cx->t[a].line, "execute requires a SQL argument");
	st = span_text(cx, as[0], ae[0]);
	sqlv = rewrite_expr(cx, st, (int) strlen(st), false);
	binds = binds_text(cx, as, ae, n);
	indent(&cx->out, ind);
	if (binds)
		appendStringInfo(&cx->out, "EXECUTE %s USING %s;\n", sqlv, binds);
	else
		appendStringInfo(&cx->out, "EXECUTE %s;\n", sqlv);
}

/* lhs = open_cursor(SQL[, binds]) -> declare lhs refcursor; OPEN lhs FOR ...; */
static void
emit_open_cursor(Ctx *cx, Tok *lhs, int a, int ind)
{
	int			as[16], ae[16], after, n;

	if (!is_param(cx, lhs->s, lhs->len))
	{
		PlxLocal2  *l = local_find(cx, lhs->s, lhs->len);

		if (!l)
			l = local_add(cx, lhs->s, lhs->len);
		if (!l->typ)
			l->typ = pstrdup("refcursor");
	}
	n = parse_args(cx, a, as, ae, 16, &after);
	if (n < 1)
		plx_err(cx, cx->t[a].line, "open_cursor requires a SQL argument");
	indent(&cx->out, ind);
	if (n == 1 && arg_is_string_literal(cx, as[0], ae[0]))
	{
		appendStringInfo(&cx->out, "OPEN %.*s FOR ", lhs->len, lhs->s);
		emit_string_as_sql(cx, &cx->t[as[0]], &cx->out);
		appendStringInfoString(&cx->out, ";\n");
	}
	else
	{
		char	   *sqlv = rw_range(cx, as[0], ae[0], false);
		char	   *binds = binds_text(cx, as, ae, n);

		if (binds)
			appendStringInfo(&cx->out, "OPEN %.*s FOR EXECUTE %s USING %s;\n",
							 lhs->len, lhs->s, sqlv, binds);
		else
			appendStringInfo(&cx->out, "OPEN %.*s FOR EXECUTE %s;\n",
							 lhs->len, lhs->s, sqlv);
	}
}

/* lhs = fetch_from(cursor) -> declare lhs RECORD; FETCH FROM cursor INTO lhs; */
static void
emit_fetch_from(Ctx *cx, Tok *lhs, int a, int ind)
{
	int			as[4], ae[4], after, n;

	if (!is_param(cx, lhs->s, lhs->len))
	{
		PlxLocal2  *l = local_find(cx, lhs->s, lhs->len);

		if (!l)
			l = local_add(cx, lhs->s, lhs->len);
		l->is_record = true;
	}
	n = parse_args(cx, a, as, ae, 4, &after);
	if (n < 1)
		plx_err(cx, cx->t[a].line, "fetch_from requires a cursor");
	indent(&cx->out, ind);
	appendStringInfo(&cx->out, "FETCH FROM %s INTO %.*s;\n",
					 rw_range(cx, as[0], ae[0], false), lhs->len, lhs->s);
}

/* perform(SQL) -> DML verbatim, or PERFORM * FROM (SQL) __plx_p_N; */
static void
emit_perform(Ctx *cx, int a, int ind)
{
	int			as[16], ae[16], after, n;

	n = parse_args(cx, a, as, ae, 16, &after);
	if (n < 1)
		plx_err(cx, cx->t[a].line, "perform requires a SQL argument");
	if (!arg_is_string_literal(cx, as[0], ae[0]))
		plx_err(cx, cx->t[a].line, "perform currently requires a string-literal SQL");
	indent(&cx->out, ind);
	if (sql_is_row_returning(&cx->t[as[0]]))
	{
		appendStringInfoString(&cx->out, "PERFORM * FROM (");
		emit_string_as_sql(cx, &cx->t[as[0]], &cx->out);
		appendStringInfo(&cx->out, ") AS __plx_p_%d;\n", ++cx->subq);
	}
	else
	{
		emit_string_as_sql(cx, &cx->t[as[0]], &cx->out);
		appendStringInfoString(&cx->out, ";\n");
	}
}

/* return_query(SQL[, binds]) */
static void
emit_return_query(Ctx *cx, int a, int ind)
{
	int			as[16], ae[16], after, n;

	if (!cx->retset)
		plx_err(cx, cx->t[a].line, "return_query is only valid in a set-returning function");
	n = parse_args(cx, a, as, ae, 16, &after);
	if (n < 1)
		plx_err(cx, cx->t[a].line, "return_query requires a SQL argument");
	indent(&cx->out, ind);
	if (n == 1 && arg_is_string_literal(cx, as[0], ae[0]))
	{
		appendStringInfoString(&cx->out, "RETURN QUERY ");
		emit_string_as_sql(cx, &cx->t[as[0]], &cx->out);
		appendStringInfoString(&cx->out, ";\n");
	}
	else
	{
		char	   *st = span_text(cx, as[0], ae[0]);
		char	   *sqlv = rewrite_expr(cx, st, (int) strlen(st), false);
		char	   *binds = binds_text(cx, as, ae, n);

		if (binds)
			appendStringInfo(&cx->out, "RETURN QUERY EXECUTE %s USING %s;\n", sqlv, binds);
		else
			appendStringInfo(&cx->out, "RETURN QUERY EXECUTE %s;\n", sqlv);
	}
}

/* lhs = fetch_one[!](SQL[, binds]) -> SELECT * INTO [STRICT] lhs FROM (...) */
static void
emit_fetch(Ctx *cx, Tok *lhs, int a, int ind, bool strict)
{
	int			as[16], ae[16], after, n;

	if (!is_param(cx, lhs->s, lhs->len))
	{
		PlxLocal2  *l = local_find(cx, lhs->s, lhs->len);

		if (!l)
			l = local_add(cx, lhs->s, lhs->len);
		l->is_record = true;
	}
	n = parse_args(cx, a, as, ae, 16, &after);
	if (n < 1)
		plx_err(cx, cx->t[a].line, "fetch_one requires a SQL argument");
	indent(&cx->out, ind);
	if (n == 1 && arg_is_string_literal(cx, as[0], ae[0]))
	{
		appendStringInfo(&cx->out, "SELECT * INTO %s%.*s FROM (",
						 strict ? "STRICT " : "", lhs->len, lhs->s);
		emit_string_as_sql(cx, &cx->t[as[0]], &cx->out);
		appendStringInfo(&cx->out, ") AS __plx_fo_%d;\n", ++cx->subq);
	}
	else
	{
		char	   *st = span_text(cx, as[0], ae[0]);
		char	   *sqlv = rewrite_expr(cx, st, (int) strlen(st), false);
		char	   *binds = binds_text(cx, as, ae, n);

		if (binds)
			appendStringInfo(&cx->out, "EXECUTE %s INTO %s%.*s USING %s;\n",
							 sqlv, strict ? "STRICT " : "", lhs->len, lhs->s, binds);
		else
			appendStringInfo(&cx->out, "EXECUTE %s INTO %s%.*s;\n",
							 sqlv, strict ? "STRICT " : "", lhs->len, lhs->s);
	}
}

static const char *
map_raise_option(const char *s, int len)
{
	static const struct { const char *k; const char *o; } m[] = {
		{"errcode", "ERRCODE"}, {"detail", "DETAIL"}, {"hint", "HINT"},
		{"message", "MESSAGE"}, {"column", "COLUMN"}, {"constraint", "CONSTRAINT"},
	};
	int			i;

	for (i = 0; i < (int) (sizeof(m) / sizeof(m[0])); i++)
		if ((int) strlen(m[i].k) == len && pg_strncasecmp(m[i].k, s, len) == 0)
			return m[i].o;
	return NULL;
}

/* raise(msg) / raise("level", msg) call form (used where raise is not a keyword) */
static void
emit_raise_call(Ctx *cx, int a, int ind)
{
	int			as[8], ae[8], after, n, mi = 0;
	const char *level = "EXCEPTION";
	char	   *mt, *msg;

	n = parse_args(cx, a, as, ae, 8, &after);
	if (n < 1)
		plx_err(cx, cx->t[a].line, "raise requires a message");
	if (n >= 2 && arg_is_string_literal(cx, as[0], ae[0]))
	{
		static const struct { const char *w; const char *lv; } lv[] = {
			{"notice", "NOTICE"}, {"warning", "WARNING"}, {"info", "INFO"},
			{"log", "LOG"}, {"debug", "DEBUG"}, {"exception", "EXCEPTION"},
		};
		Tok		   *st = &cx->t[as[0]];
		const char *c = st->s + 1;
		int			cl = st->len - 2, k;

		for (k = 0; k < (int) (sizeof(lv) / sizeof(lv[0])); k++)
			if ((int) strlen(lv[k].w) == cl && pg_strncasecmp(lv[k].w, c, cl) == 0)
			{
				level = lv[k].lv;
				mi = 1;
				break;
			}
	}
	mt = span_text(cx, as[mi], ae[mi]);
	msg = rewrite_expr(cx, mt, (int) strlen(mt), false);
	indent(&cx->out, ind);
	appendStringInfo(&cx->out, "RAISE %s '%%', %s;\n", level, msg);
}

/* emit a raise statement from token range [a,b) */
static void
emit_raise(Ctx *cx, int a, int b, int ind)
{
	StringInfo	o = &cx->out;
	const char *level = "EXCEPTION";
	int			p = a + 1;		/* after 'raise' */
	char	   *msg = NULL;
	int			mstart, mend;
	StringInfoData using;

	if (p >= b)
	{
		if (cx->handlerdepth == 0)
			plx_err(cx, cx->t[a].line, "bare 'raise' (re-raise) is only valid inside a rescue handler");
		indent(o, ind);
		appendStringInfoString(o, "RAISE;\n");
		return;
	}
	/* LEVEL: form -> IDENT/KW ':' */
	if ((cx->t[p].kind == T_IDENT || cx->t[p].kind == T_KW) &&
		p + 1 < b && tok_is(&cx->t[p + 1], ":"))
	{
		static const struct { const char *w; const char *lv; } lv[] = {
			{"notice", "NOTICE"}, {"warning", "WARNING"}, {"info", "INFO"},
			{"log", "LOG"}, {"debug", "DEBUG"}, {"exception", "EXCEPTION"},
		};
		int			i;
		int			wl = cx->t[p].len;
		const char *w = cx->t[p].s;

		for (i = 0; i < (int) (sizeof(lv) / sizeof(lv[0])); i++)
			if ((int) strlen(lv[i].w) == wl && pg_strncasecmp(lv[i].w, w, wl) == 0)
			{
				level = lv[i].lv;
				p += 2;
				break;
			}
	}
	mstart = p;
	mend = b;
	{
		int			i, depth = 0;

		for (i = p; i < b; i++)
		{
			if (cx->t[i].kind == T_LPAREN)
				depth++;
			else if (cx->t[i].kind == T_RPAREN)
				depth--;
			else if (depth == 0 && cx->t[i].kind == T_COMMA)
			{
				mend = i;
				break;
			}
		}
	}
	if (mstart >= mend)
		plx_err(cx, cx->t[a].line, "raise requires a message");
	{
		char	   *mt = span_text(cx, mstart, mend);

		msg = rewrite_expr(cx, mt, (int) strlen(mt), false);
	}
	/* options: , key: value ... -> USING KEY = value */
	initStringInfo(&using);
	{
		int			p2 = mend;

		while (p2 < b && cx->t[p2].kind == T_COMMA)
		{
			int			ka = p2 + 1;
			int			va, ve, i, depth = 0;
			const char *opt;
			char	   *vt, *vv;

			if (!((cx->t[ka].kind == T_IDENT || cx->t[ka].kind == T_KW) &&
				  ka + 1 < b && tok_is(&cx->t[ka + 1], ":")))
				plx_err(cx, cx->t[a].line, "malformed raise option");
			opt = map_raise_option(cx->t[ka].s, cx->t[ka].len);
			if (!opt)
				plx_err(cx, cx->t[ka].line, "unknown raise option \"%.*s\"",
						cx->t[ka].len, cx->t[ka].s);
			va = ka + 2;
			ve = b;
			for (i = va; i < b; i++)
			{
				if (cx->t[i].kind == T_LPAREN)
					depth++;
				else if (cx->t[i].kind == T_RPAREN)
					depth--;
				else if (depth == 0 && cx->t[i].kind == T_COMMA)
				{
					ve = i;
					break;
				}
			}
			vt = span_text(cx, va, ve);
			vv = rewrite_expr(cx, vt, (int) strlen(vt), false);
			if (using.len)
				appendStringInfoString(&using, ", ");
			appendStringInfo(&using, "%s = %s", opt, vv);
			p2 = ve;
		}
	}
	indent(o, ind);
	if (using.len)
		appendStringInfo(o, "RAISE %s '%%', %s USING %s;\n", level, msg, using.data);
	else
		appendStringInfo(o, "RAISE %s '%%', %s;\n", level, msg);
}

/* emit the core (non-block) statement given range [a,b); returns nothing */
static void
emit_core(Ctx *cx, int a, int b, int ind, bool toplevel)
{
	StringInfo	o = &cx->out;
	Tok		   *t0 = &cx->t[a];

	/* JS/PHP variable declaration keyword (let/const/var): strip and continue */
	if (t0->kind == T_KW && t0->kw == KW_LET && a + 1 < b)
	{
		a++;
		t0 = &cx->t[a];
	}

	/* statement-level intrinsics */
	if (t0->kind == T_IDENT && a + 1 < b && cx->t[a + 1].kind == T_LPAREN)
	{
		if (name_eq(t0, "perform"))
		{
			emit_perform(cx, a, ind);
			return;
		}
		if (name_eq(t0, "execute"))
		{
			emit_execute(cx, a, ind);
			return;
		}
		if (name_eq(t0, "return_query"))
		{
			emit_return_query(cx, a, ind);
			return;
		}
		if (name_eq(t0, "raise"))	/* raise(msg) / raise("level", msg) call form */
		{
			emit_raise_call(cx, a, ind);
			return;
		}
		if (name_eq(t0, "assert"))	/* assert(cond[, msg]) -> ASSERT cond[, msg]; */
		{
			int			as[4], ae[4], after, n;

			n = parse_args(cx, a, as, ae, 4, &after);
			if (n < 1)
				plx_err(cx, t0->line, "assert requires a condition");
			indent(o, ind);
			appendStringInfo(o, "ASSERT %s", rw_range(cx, as[0], ae[0], true));
			if (n >= 2)
				appendStringInfo(o, ", %s", rw_range(cx, as[1], ae[1], false));
			appendStringInfoString(o, ";\n");
			return;
		}
		if (name_eq(t0, "call"))	/* call("proc", a, b) -> CALL proc(a, b); */
		{
			int			as[16], ae[16], after, n, i;

			n = parse_args(cx, a, as, ae, 16, &after);
			if (n < 1)
				plx_err(cx, t0->line, "call requires a procedure name");
			indent(o, ind);
			appendStringInfoString(o, "CALL ");
			if (arg_is_string_literal(cx, as[0], ae[0]))
			{
				Tok		   *pn = &cx->t[as[0]];

				appendBinaryStringInfo(o, pn->s + 1, pn->len - 2);	/* unquote */
			}
			else
				appendStringInfoString(o, rw_range(cx, as[0], ae[0], false));
			appendStringInfoChar(o, '(');
			for (i = 1; i < n; i++)
				appendStringInfo(o, "%s%s", i > 1 ? ", " : "", rw_range(cx, as[i], ae[i], false));
			appendStringInfoString(o, ");\n");
			return;
		}
		if (name_eq(t0, "commit") || name_eq(t0, "rollback"))
		{
			indent(o, ind);
			appendStringInfo(o, "%s;\n", name_eq(t0, "commit") ? "COMMIT" : "ROLLBACK");
			return;
		}
		if (name_eq(t0, "close_cursor"))	/* close_cursor(c) -> CLOSE c; */
		{
			int			as[4], ae[4], after, n;

			n = parse_args(cx, a, as, ae, 4, &after);
			if (n < 1)
				plx_err(cx, t0->line, "close_cursor requires a cursor");
			indent(o, ind);
			appendStringInfo(o, "CLOSE %s;\n", rw_range(cx, as[0], ae[0], false));
			return;
		}
		if (name_eq(t0, "move_cursor"))	/* move_cursor(c[, n]) -> MOVE [FORWARD n] FROM c; */
		{
			int			as[4], ae[4], after, n;

			n = parse_args(cx, a, as, ae, 4, &after);
			if (n < 1)
				plx_err(cx, t0->line, "move_cursor requires a cursor");
			indent(o, ind);
			if (n >= 2)
				appendStringInfo(o, "MOVE FORWARD %s FROM %s;\n",
								 rw_range(cx, as[1], ae[1], false), rw_range(cx, as[0], ae[0], false));
			else
				appendStringInfo(o, "MOVE FROM %s;\n", rw_range(cx, as[0], ae[0], false));
			return;
		}
	}

	/* return */
	if (t0->kind == T_KW && t0->kw == KW_RETURN)
	{
		if (a + 1 >= b)
		{
			indent(o, ind);
			appendStringInfoString(o, "RETURN;\n");
		}
		else
		{
			char	   *e = rewrite_expr(cx, span_text(cx, a + 1, b),
										 (int) strlen(span_text(cx, a + 1, b)), false);

			if (cx->retset)
				plx_err(cx, t0->line, "use emit/return_next in a set-returning function, not 'return <value>'");
			indent(o, ind);
			appendStringInfo(o, "RETURN %s;\n", e);
		}
		return;
	}
	/* emit / return_next  (bare, `emit e`, or PHP-style `emit(e)`) */
	if (t0->kind == T_KW && (t0->kw == KW_EMIT || t0->kw == KW_RETURN_NEXT))
	{
		int			ra = a + 1, rb = b;

		if (!cx->retset)
			plx_err(cx, t0->line, "emit/return_next is only valid in a set-returning function");
		if (rb > ra && cx->t[ra].kind == T_LPAREN && cx->t[rb - 1].kind == T_RPAREN)
		{
			ra++;
			rb--;
		}
		indent(o, ind);
		if (ra >= rb)
			appendStringInfoString(o, "RETURN NEXT;\n");
		else
		{
			char	   *st = span_text(cx, ra, rb);

			appendStringInfo(o, "RETURN NEXT %s;\n", rewrite_expr(cx, st, (int) strlen(st), false));
		}
		return;
	}
	/* next / break, optionally with a loop label */
	if (t0->kind == T_KW && (t0->kw == KW_NEXT || t0->kw == KW_BREAK))
	{
		const char *kw = (t0->kw == KW_NEXT) ? "CONTINUE" : "EXIT";

		if (cx->loopdepth == 0)
			plx_err(cx, t0->line, "%s outside a loop", t0->kw == KW_NEXT ? "next" : "break");
		indent(o, ind);
		if (a + 1 < b && cx->t[a + 1].kind == T_IDENT)
			appendStringInfo(o, "%s %.*s;\n", kw, cx->t[a + 1].len, cx->t[a + 1].s);
		else
			appendStringInfo(o, "%s;\n", kw);
		return;
	}
	/* raise */
	if (t0->kind == T_KW && t0->kw == KW_RAISE)
	{
		emit_raise(cx, a, b, ind);
		return;
	}
	/* standalone annotation:  name #:: T   (a=IDENT, then TYPEANN) */
	if (t0->kind == T_IDENT && a + 1 < b && cx->t[a + 1].kind == T_TYPEANN)
	{
		PlxLocal2  *l;

		if (is_param(cx, t0->s, t0->len))
			plx_err(cx, t0->line, "cannot annotate parameter \"%.*s\"", t0->len, t0->s);
		l = local_find(cx, t0->s, t0->len);
		if (!l)
			l = local_add(cx, t0->s, t0->len);
		l->typ = pnstrdup(cx->t[a + 1].ann, cx->t[a + 1].annlen);
		return;
	}
	/*
	 * String-accumulation append operators lower to the string builder:
	 *   Ruby  s << x     (when s is a string)
	 *   PHP   $s .= x
	 *   JS/Py s += x     (when s is a string)
	 * become  s := plx_sb_append(s, (x));  with s declared plx_strbuild, whose
	 * append is amortized O(1). A numeric += is left alone.
	 */
	if (t0->kind == T_IDENT && !is_param(cx, t0->s, t0->len))
	{
		int			app_rhs = -1;
		PlxLocal2  *l = local_find(cx, t0->s, t0->len);

		if (a + 1 < b && tok_is(&cx->t[a + 1], "<<") && l && is_stringy(l->typ))
			app_rhs = a + 2;	/* Ruby shovel on a string */
		else if (a + 2 < b && cx->t[a + 1].kind == T_DOT && tok_is(&cx->t[a + 2], "="))
			app_rhs = a + 3;	/* PHP .= */
		else if (a + 1 < b && tok_is(&cx->t[a + 1], "+=") && l && is_stringy(l->typ))
			app_rhs = a + 2;	/* += on a string */

		if (app_rhs >= 0)
		{
			int			rb = b;
			char	   *rhs,
					   *r;

			/* drop a trailing annotation token if present */
			if (rb > app_rhs && cx->t[rb - 1].kind == T_TYPEANN)
				rb--;
			if (app_rhs >= rb)
				plx_err(cx, t0->line, "append requires a value on the right-hand side");
			if (!l)
				l = local_add(cx, t0->s, t0->len);
			if (l->is_const)
				plx_err(cx, t0->line, "cannot append to a constant");
			l->typ = pstrdup("plx_strbuild");
			rhs = span_text(cx, app_rhs, rb);
			r = rewrite_expr(cx, rhs, (int) strlen(rhs), false);
			indent(o, ind);
			/* cast the appended value to text (int, numeric, etc. render) */
			appendStringInfo(o, "%.*s := plx_sb_append(%.*s, (%s)::text);\n",
							 t0->len, t0->s, t0->len, t0->s, r);
			return;
		}
	}

	/* assignment:  IDENT (= | += | -= | *= | /= | %=) RHS [#:: T].
	 * A leading "->" (PHP object access, e.g. $NEW->col = e) is not a simple
	 * assignment; fall through to the qualified-lvalue handler below. */
	if (t0->kind == T_IDENT && a + 1 < b && cx->t[a + 1].kind == T_OP &&
		!(cx->t[a + 1].len == 2 && cx->t[a + 1].s[0] == '-' && cx->t[a + 1].s[1] == '>'))
	{
		Tok		   *op = &cx->t[a + 1];
		bool		compound = false;
		char		cop = 0;
		int			rhs_a = a + 2;
		int			rhs_b = b;
		const char *anntyp = NULL;
		int			annlen = 0;
		bool		wantconst = false;

		/* lhs = fetch_one[!](...) -> SELECT ... INTO */
		if (tok_is(op, "=") && a + 2 < b && cx->t[a + 2].kind == T_IDENT &&
			(name_eq(&cx->t[a + 2], "fetch_one") || name_eq(&cx->t[a + 2], "fetch_one!")) &&
			a + 3 < b && cx->t[a + 3].kind == T_LPAREN)
		{
			emit_fetch(cx, t0, a + 2, ind, name_eq(&cx->t[a + 2], "fetch_one!"));
			return;
		}
		/* lhs = open_cursor(...) / fetch_from(...) */
		if (tok_is(op, "=") && a + 2 < b && cx->t[a + 2].kind == T_IDENT &&
			name_eq(&cx->t[a + 2], "open_cursor") &&
			a + 3 < b && cx->t[a + 3].kind == T_LPAREN)
		{
			emit_open_cursor(cx, t0, a + 2, ind);
			return;
		}
		if (tok_is(op, "=") && a + 2 < b && cx->t[a + 2].kind == T_IDENT &&
			name_eq(&cx->t[a + 2], "fetch_from") &&
			a + 3 < b && cx->t[a + 3].kind == T_LPAREN)
		{
			emit_fetch_from(cx, t0, a + 2, ind);
			return;
		}
		/* lhs = row_count() -> GET DIAGNOSTICS lhs = ROW_COUNT; */
		if (tok_is(op, "=") && a + 2 < b && cx->t[a + 2].kind == T_IDENT &&
			name_eq(&cx->t[a + 2], "row_count") &&
			a + 3 < b && cx->t[a + 3].kind == T_LPAREN)
		{
			if (!is_param(cx, t0->s, t0->len))
			{
				PlxLocal2  *l = local_find(cx, t0->s, t0->len);

				if (!l)
					l = local_add(cx, t0->s, t0->len);
				if (!l->typ)
					l->typ = pstrdup("bigint");
			}
			indent(o, ind);
			appendStringInfo(o, "GET DIAGNOSTICS %.*s = ROW_COUNT;\n", t0->len, t0->s);
			return;
		}

		if (tok_is(op, "="))
			;
		else if (op->len == 2 && op->s[1] == '=' && strchr("+-*/%", op->s[0]))
		{
			compound = true;
			cop = op->s[0];
		}
		else
			plx_err(cx, t0->line, "unsupported operator in statement");

		/* trailing annotation (may carry a trailing 'const'/'constant' marker) */
		if (rhs_b > rhs_a && cx->t[rhs_b - 1].kind == T_TYPEANN)
		{
			anntyp = cx->t[rhs_b - 1].ann;
			annlen = cx->t[rhs_b - 1].annlen;
			rhs_b--;
			/* strip a trailing const/constant word -> mark CONSTANT */
			{
				int			k = annlen;

				while (k > 0 && (anntyp[k - 1] == ' ' || anntyp[k - 1] == '\t'))
					k--;
				if ((k >= 8 && pg_strncasecmp(anntyp + k - 8, "constant", 8) == 0 &&
					 (k == 8 || anntyp[k - 9] == ' ')) ||
					(k >= 5 && pg_strncasecmp(anntyp + k - 5, "const", 5) == 0 &&
					 (k == 5 || anntyp[k - 6] == ' ')))
				{
					wantconst = true;
					annlen = (k >= 8 && pg_strncasecmp(anntyp + k - 8, "constant", 8) == 0) ? k - 8 : k - 5;
					while (annlen > 0 && (anntyp[annlen - 1] == ' ' || anntyp[annlen - 1] == '\t'))
						annlen--;
				}
			}
		}
		if (rhs_a >= rhs_b)
			plx_err(cx, t0->line, "assignment requires a value on the right-hand side");
		if (is_param(cx, t0->s, t0->len))
		{
			/* assigning to an OUT/param: plain := (never hoist) */
			char	   *rhs = span_text(cx, rhs_a, rhs_b);
			char	   *r = rewrite_expr(cx, rhs, (int) strlen(rhs), false);

			indent(o, ind);
			if (compound)
				appendStringInfo(o, "%.*s := %.*s %c (%s);\n", t0->len, t0->s,
								 t0->len, t0->s, cop, r);
			else
				appendStringInfo(o, "%.*s := %s;\n", t0->len, t0->s, r);
			return;
		}
		{
			PlxLocal2  *l = local_find(cx, t0->s, t0->len);
			bool		firstseen = (l == NULL);
			const char *littyp = const_literal_type(cx, rhs_a, rhs_b);

			if (!l)
				l = local_add(cx, t0->s, t0->len);
			if (anntyp && !l->typ)
				l->typ = pnstrdup(anntyp, annlen);

			/* CONSTANT: initializer lives in the DECLARE; no runtime assignment */
			if (wantconst)
			{
				char	   *rhs = span_text(cx, rhs_a, rhs_b);

				if (!firstseen)
					plx_err(cx, t0->line, "cannot reassign constant \"%.*s\"", t0->len, t0->s);
				if (compound)
					plx_err(cx, t0->line, "constant cannot use a compound assignment");
				if (!l->typ && littyp)
					l->typ = pstrdup(littyp);
				if (!l->typ)
					plx_err(cx, t0->line, "constant \"%.*s\" requires a type annotation", t0->len, t0->s);
				l->is_const = true;
				l->init = rewrite_expr(cx, rhs, (int) strlen(rhs), false);
				return;
			}

			/* fold: top-level, first assignment, constant literal RHS */
			if (toplevel && firstseen && !compound && littyp && !l->is_record)
			{
				char	   *rhs = span_text(cx, rhs_a, rhs_b);

				if (!l->typ)
					l->typ = pstrdup(littyp);
				l->init = rewrite_expr(cx, rhs, (int) strlen(rhs), false);
				return;			/* statement folded into DECLARE; emit nothing */
			}
			/* otherwise infer type if still unknown */
			if (!l->typ && littyp)
				l->typ = pstrdup(littyp);
			{
				char	   *rhs = span_text(cx, rhs_a, rhs_b);
				char	   *r = rewrite_expr(cx, rhs, (int) strlen(rhs), false);

				indent(o, ind);
				if (compound)
					appendStringInfo(o, "%.*s := %.*s %c (%s);\n", t0->len, t0->s,
									 t0->len, t0->s, cop, r);
				else
					appendStringInfo(o, "%.*s := %s;\n", t0->len, t0->s, r);
			}
			return;
		}
	}

	/*
	 * Qualified / subscripted lvalue assignment: NEW.field = e, r.col = e,
	 * arr[i] = e. Find a top-level single '=' and emit lhs := rhs. This enables
	 * trigger functions (assigning to NEW/OLD fields) and element assignment.
	 */
	{
		int			i, depth = 0, eq = -1;

		for (i = a; i < b; i++)
		{
			TokKind		k = cx->t[i].kind;

			if (k == T_LPAREN || k == T_LBRACKET || k == T_LBRACE)
				depth++;
			else if (k == T_RPAREN || k == T_RBRACKET || k == T_RBRACE)
				depth--;
			else if (depth == 0 && tok_is(&cx->t[i], "="))
			{
				eq = i;
				break;
			}
		}
		if (eq > a && eq + 1 < b && t0->kind == T_IDENT &&
			(cx->t[a + 1].kind == T_DOT || cx->t[a + 1].kind == T_LBRACKET ||
			 (cx->t[a + 1].kind == T_OP && cx->t[a + 1].len == 2 &&
			  cx->t[a + 1].s[0] == '-' && cx->t[a + 1].s[1] == '>')))
		{
			char	   *lhs = rw_range(cx, a, eq, false);
			char	   *rhs = rw_range(cx, eq + 1, b, false);

			indent(o, ind);
			appendStringInfo(o, "%s := %s;\n", lhs, rhs);
			return;
		}
	}

	plx_err(cx, t0->line, "unsupported statement");
}

/* emit a leaf statement (range [a,b)) applying any trailing modifier */
static void
emit_leaf(Ctx *cx, int a, int b, int ind, bool toplevel)
{
	StringInfo	o = &cx->out;
	int			m = find_modifier(cx, a, b);
	Tok		   *t0 = &cx->t[a];

	if (m < 0)
	{
		emit_core(cx, a, b, ind, toplevel);
		return;
	}
	{
		Kw			mk = cx->t[m].kw;
		char	   *cond = rewrite_expr(cx, span_text(cx, m + 1, b),
										(int) strlen(span_text(cx, m + 1, b)), true);
		bool		neg = (mk == KW_UNLESS || mk == KW_UNTIL);

		/* next/break modifiers -> WHEN (optionally with a loop label) */
		if (t0->kind == T_KW && (t0->kw == KW_NEXT || t0->kw == KW_BREAK))
		{
			char		lbl[NAMEDATALEN + 1] = "";

			if (cx->loopdepth == 0)
				plx_err(cx, t0->line, "%s outside a loop", t0->kw == KW_NEXT ? "next" : "break");
			if (a + 1 < m && cx->t[a + 1].kind == T_IDENT)
				snprintf(lbl, sizeof(lbl), " %.*s", cx->t[a + 1].len, cx->t[a + 1].s);
			indent(o, ind);
			appendStringInfo(o, "%s%s WHEN %s%s%s;\n",
							 t0->kw == KW_NEXT ? "CONTINUE" : "EXIT", lbl,
							 neg ? "NOT (" : "", cond, neg ? ")" : "");
			return;
		}
		if (mk == KW_IF || mk == KW_UNLESS)
		{
			indent(o, ind);
			appendStringInfo(o, "IF %s%s%s THEN\n", neg ? "NOT (" : "", cond, neg ? ")" : "");
			emit_core(cx, a, m, ind + 1, false);
			indent(o, ind);
			appendStringInfoString(o, "END IF;\n");
		}
		else					/* while / until */
		{
			indent(o, ind);
			appendStringInfo(o, "WHILE %s%s%s LOOP\n", neg ? "NOT (" : "", cond, neg ? ")" : "");
			cx->loopdepth++;
			emit_core(cx, a, m, ind + 1, false);
			cx->loopdepth--;
			indent(o, ind);
			appendStringInfoString(o, "END LOOP;\n");
		}
	}
}

/* ---------------------------------------------------------------- blocks */

static void
parse_if(Ctx *cx, int ind)
{
	StringInfo	o = &cx->out;
	int			start = cx->pos;
	Tok		   *hd = &cx->t[start];
	bool		neg = (hd->kw == KW_UNLESS);
	int			e = stmt_end(cx, start);
	int			cond_a = start + 1;
	int			cond_b = e;
	char	   *cond;

	/* optional trailing 'then' */
	if (cond_b > cond_a && cx->t[cond_b - 1].kind == T_KW && cx->t[cond_b - 1].kw == KW_THEN)
		cond_b--;
	cond = rewrite_expr(cx, span_text(cx, cond_a, cond_b),
						(int) strlen(span_text(cx, cond_a, cond_b)), true);
	indent(o, ind);
	appendStringInfo(o, "IF %s%s%s THEN\n", neg ? "NOT (" : "", cond, neg ? ")" : "");
	cx->pos = e;
	parse_block(cx, ind + 1);
	/* handle elsif/else chain */
	while (cx->t[cx->pos].kind == T_KW &&
		   (cx->t[cx->pos].kw == KW_ELSIF || cx->t[cx->pos].kw == KW_ELSE))
	{
		if (cx->t[cx->pos].kw == KW_ELSIF)
		{
			int			es = cx->pos;
			int			ee = stmt_end(cx, es);
			int			ca = es + 1, cb = ee;
			char	   *ec;

			if (cb > ca && cx->t[cb - 1].kind == T_KW && cx->t[cb - 1].kw == KW_THEN)
				cb--;
			ec = rewrite_expr(cx, span_text(cx, ca, cb),
							  (int) strlen(span_text(cx, ca, cb)), true);
			indent(o, ind);
			appendStringInfo(o, "ELSIF %s THEN\n", ec);
			cx->pos = ee;
			parse_block(cx, ind + 1);
		}
		else
		{
			indent(o, ind);
			appendStringInfoString(o, "ELSE\n");
			cx->pos++;			/* consume else */
			parse_block(cx, ind + 1);
			break;
		}
	}
	if (cx->t[cx->pos].kind == T_KW && cx->t[cx->pos].kw == KW_END)
		cx->pos++;
	else
		plx_err(cx, hd->line, "unterminated if/unless block");
	indent(o, ind);
	appendStringInfoString(o, "END IF;\n");
}

static void
parse_while(Ctx *cx, int ind)
{
	StringInfo	o = &cx->out;
	int			start = cx->pos;
	Tok		   *hd = &cx->t[start];
	bool		neg = (hd->kw == KW_UNTIL);
	int			e = stmt_end(cx, start);
	int			ca = start + 1, cb = e;
	char	   *cond;

	if (cb > ca && cx->t[cb - 1].kind == T_KW && cx->t[cb - 1].kw == KW_DO)
		cb--;
	cond = rewrite_expr(cx, span_text(cx, ca, cb),
						(int) strlen(span_text(cx, ca, cb)), true);
	indent(o, ind);
	appendStringInfo(o, "WHILE %s%s%s LOOP\n", neg ? "NOT (" : "", cond, neg ? ")" : "");
	cx->pos = e;
	cx->loopdepth++;
	parse_block(cx, ind + 1);
	cx->loopdepth--;
	if (cx->t[cx->pos].kind == T_KW && cx->t[cx->pos].kw == KW_END)
		cx->pos++;
	else
		plx_err(cx, hd->line, "unterminated while/until block");
	indent(o, ind);
	appendStringInfoString(o, "END LOOP;\n");
}

static void
parse_loop(Ctx *cx, int ind)
{
	StringInfo	o = &cx->out;
	int			start = cx->pos;
	int			e = stmt_end(cx, start);

	indent(o, ind);
	appendStringInfoString(o, "LOOP\n");
	cx->pos = e;
	cx->loopdepth++;
	parse_block(cx, ind + 1);
	cx->loopdepth--;
	if (cx->t[cx->pos].kind == T_KW && cx->t[cx->pos].kw == KW_END)
		cx->pos++;
	else
		plx_err(cx, cx->t[start].line, "unterminated loop block");
	indent(o, ind);
	appendStringInfoString(o, "END LOOP;\n");
}

/* integer for:  for V in LO..HI  |  for V in LO...HI */
static void
parse_for(Ctx *cx, int ind)
{
	StringInfo	o = &cx->out;
	int			start = cx->pos;
	Tok		   *hd = &cx->t[start];
	int			e = stmt_end(cx, start);
	int			p = start + 1;
	int			var_a, var_b;
	int			in_at = -1, range_at = -1;
	bool		exclusive = false;
	int			i;
	char	   *lo, *hi;
	int			hi_a, hi_b;

	if (cx->t[p].kind != T_IDENT)
		plx_err(cx, hd->line, "for loop expects a variable name");
	var_a = p;
	var_b = p + 1;
	if (!(cx->t[p + 1].kind == T_KW && cx->t[p + 1].kw == KW_IN))
		plx_err(cx, hd->line, "for loop expects 'in'");
	in_at = p + 1;
	/* find the range operator .. or ... at top level */
	for (i = in_at + 1; i < e; i++)
	{
		if (tok_is(&cx->t[i], "..") || (cx->t[i].kind == T_OP && cx->t[i].len >= 2 &&
										strncmp(cx->t[i].s, "..", 2) == 0))
		{
			range_at = i;
			exclusive = (cx->t[i].len == 3);	/* ... */
			break;
		}
		/* a lone DOT DOT sequence (lexer may split) */
	}
	if (range_at < 0)
		plx_err(cx, hd->line, "for loop requires an integer range LO..HI (only integer ranges supported in M3a)");
	/* trailing 'do' */
	if (e > in_at && cx->t[e - 1].kind == T_KW && cx->t[e - 1].kw == KW_DO)
		e--;
	lo = rewrite_expr(cx, span_text(cx, in_at + 1, range_at),
					  (int) strlen(span_text(cx, in_at + 1, range_at)), false);
	hi_a = range_at + 1;
	hi_b = e;
	hi = rewrite_expr(cx, span_text(cx, hi_a, hi_b),
					  (int) strlen(span_text(cx, hi_a, hi_b)), false);
	indent(o, ind);
	if (exclusive)
		appendStringInfo(o, "FOR %.*s IN %s..(%s - 1) LOOP\n",
						 cx->t[var_a].len, cx->t[var_a].s, lo, hi);
	else
		appendStringInfo(o, "FOR %.*s IN %s..%s LOOP\n",
						 cx->t[var_a].len, cx->t[var_a].s, lo, hi);
	(void) var_b;
	cx->pos = e;
	/* skip the 'do' token if we trimmed it */
	if (cx->t[cx->pos].kind == T_KW && cx->t[cx->pos].kw == KW_DO)
		cx->pos++;
	cx->loopdepth++;
	parse_block(cx, ind + 1);
	cx->loopdepth--;
	if (cx->t[cx->pos].kind == T_KW && cx->t[cx->pos].kw == KW_END)
		cx->pos++;
	else
		plx_err(cx, hd->line, "unterminated for block");
	indent(o, ind);
	appendStringInfoString(o, "END LOOP;\n");
}

/* capture a parse_block into a string (temporarily swaps cx->out) */
static char *
capture_block(Ctx *cx, int ind)
{
	StringInfoData saved = cx->out;
	char	   *r;

	initStringInfo(&cx->out);
	parse_block(cx, ind);
	r = cx->out.data;
	cx->out = saved;
	return r;
}

/* iterator block: query(...).each do |row| ... end  |  (LO..HI).each do |v| ... end */
static void
parse_iter(Ctx *cx, int a, int do_pos, int e, int ind)
{
	StringInfo	o = &cx->out;
	int			each_at = -1;
	bool		with_index = false;
	int			recv_end;
	int			vp = do_pos + 1;
	int			var1_a = -1, var2_a = -1;
	int			i;

	for (i = a + 1; i < do_pos; i++)
		if (cx->t[i].kind == T_IDENT &&
			(name_eq(&cx->t[i], "each") || name_eq(&cx->t[i], "each_with_index")) &&
			cx->t[i - 1].kind == T_DOT)
		{
			each_at = i;
			with_index = name_eq(&cx->t[i], "each_with_index");
		}
	if (each_at < 0)
		plx_err(cx, cx->t[a].line, "unsupported iterator (use query(...).each or (LO..HI).each)");
	recv_end = each_at - 1;

	if (cx->t[vp].kind != T_PIPE || cx->t[vp + 1].kind != T_IDENT)
		plx_err(cx, cx->t[a].line, "iterator block requires |var|");
	var1_a = vp + 1;
	vp += 2;
	if (cx->t[vp].kind == T_COMMA)
	{
		if (cx->t[vp + 1].kind != T_IDENT)
			plx_err(cx, cx->t[a].line, "iterator block index requires a name");
		var2_a = vp + 1;
		vp += 2;
	}
	if (cx->t[vp].kind != T_PIPE)
		plx_err(cx, cx->t[a].line, "iterator block variable list not closed with |");

	if (cx->t[a].kind == T_IDENT && name_eq(&cx->t[a], "query") &&
		cx->t[a + 1].kind == T_LPAREN)
	{
		int			as[16], ae[16], after, n;

		n = parse_args(cx, a, as, ae, 16, &after);
		if (n < 1)
			plx_err(cx, cx->t[a].line, "query requires a SQL argument");
		if (!is_param(cx, cx->t[var1_a].s, cx->t[var1_a].len))
		{
			PlxLocal2  *l = local_find(cx, cx->t[var1_a].s, cx->t[var1_a].len);

			if (!l)
				l = local_add(cx, cx->t[var1_a].s, cx->t[var1_a].len);
			l->is_record = true;
		}
		if (with_index)
		{
			PlxLocal2  *li;

			if (var2_a < 0)
				plx_err(cx, cx->t[a].line, "each_with_index requires |row, idx|");
			li = local_find(cx, cx->t[var2_a].s, cx->t[var2_a].len);
			if (!li)
				li = local_add(cx, cx->t[var2_a].s, cx->t[var2_a].len);
			if (!li->typ)
				li->typ = pstrdup("integer");
			indent(o, ind);
			appendStringInfo(o, "%.*s := -1;\n", cx->t[var2_a].len, cx->t[var2_a].s);
		}
		indent(o, ind);
		if (n == 1 && arg_is_string_literal(cx, as[0], ae[0]))
		{
			appendStringInfo(o, "FOR %.*s IN ", cx->t[var1_a].len, cx->t[var1_a].s);
			emit_string_as_sql(cx, &cx->t[as[0]], o);
			appendStringInfoString(o, " LOOP\n");
		}
		else
		{
			char	   *st = span_text(cx, as[0], ae[0]);
			char	   *sqlv = rewrite_expr(cx, st, (int) strlen(st), false);
			char	   *binds = binds_text(cx, as, ae, n);

			if (binds)
				appendStringInfo(o, "FOR %.*s IN EXECUTE %s USING %s LOOP\n",
								 cx->t[var1_a].len, cx->t[var1_a].s, sqlv, binds);
			else
				appendStringInfo(o, "FOR %.*s IN EXECUTE %s LOOP\n",
								 cx->t[var1_a].len, cx->t[var1_a].s, sqlv);
		}
		cx->pos = e;
		cx->loopdepth++;
		if (with_index)
		{
			indent(o, ind + 1);
			appendStringInfo(o, "%.*s := %.*s + 1;\n",
							 cx->t[var2_a].len, cx->t[var2_a].s,
							 cx->t[var2_a].len, cx->t[var2_a].s);
		}
		parse_block(cx, ind + 1);
		cx->loopdepth--;
	}
	else if (cx->t[a].kind == T_LPAREN)
	{
		int			rng = -1;
		bool		excl = false;
		int			lo_a = a + 1, lo_b, hi_a, hi_b = recv_end - 1;
		char	   *lo, *hi, *lst, *hst;

		for (i = a + 1; i < recv_end; i++)
			if (cx->t[i].kind == T_OP && cx->t[i].len >= 2 &&
				strncmp(cx->t[i].s, "..", 2) == 0)
			{
				rng = i;
				excl = (cx->t[i].len == 3);
				break;
			}
		if (rng < 0)
			plx_err(cx, cx->t[a].line, "range iterator requires LO..HI");
		lo_b = rng;
		hi_a = rng + 1;
		lst = span_text(cx, lo_a, lo_b);
		hst = span_text(cx, hi_a, hi_b);
		lo = rewrite_expr(cx, lst, (int) strlen(lst), false);
		hi = rewrite_expr(cx, hst, (int) strlen(hst), false);
		indent(o, ind);
		if (excl)
			appendStringInfo(o, "FOR %.*s IN %s..(%s - 1) LOOP\n",
							 cx->t[var1_a].len, cx->t[var1_a].s, lo, hi);
		else
			appendStringInfo(o, "FOR %.*s IN %s..%s LOOP\n",
							 cx->t[var1_a].len, cx->t[var1_a].s, lo, hi);
		cx->pos = e;
		cx->loopdepth++;
		parse_block(cx, ind + 1);
		cx->loopdepth--;
	}
	else
	{
		/* FOREACH over an array expression: arr.each do |v| ... end */
		char	   *arr = span_text(cx, a, recv_end);
		char	   *arrx = rewrite_expr(cx, arr, (int) strlen(arr), false);
		PlxLocal2  *lv;

		if (is_param(cx, cx->t[var1_a].s, cx->t[var1_a].len))
			plx_err(cx, cx->t[a].line, "foreach-array loop variable must be a local, not a parameter");
		lv = local_find(cx, cx->t[var1_a].s, cx->t[var1_a].len);
		if (!lv || !lv->typ)
			plx_err(cx, cx->t[a].line,
					"foreach over an array requires the loop variable to be annotated with its element type before the loop, e.g. \"%.*s #:: int\"",
					cx->t[var1_a].len, cx->t[var1_a].s);
		indent(o, ind);
		appendStringInfo(o, "FOREACH %.*s IN ARRAY %s%s LOOP\n",
						 cx->t[var1_a].len, cx->t[var1_a].s,
						 arrx[0] == '[' ? "ARRAY" : "", arrx);
		cx->pos = e;
		cx->loopdepth++;
		parse_block(cx, ind + 1);
		cx->loopdepth--;
	}

	skip_seps(cx);
	if (cx->t[cx->pos].kind == T_KW && cx->t[cx->pos].kw == KW_END)
		cx->pos++;
	else
		plx_err(cx, cx->t[a].line, "unterminated iterator block");
	indent(o, ind);
	appendStringInfoString(o, "END LOOP;\n");
}

/* GET STACKED DIAGNOSTICS lines for the fields used in a handler (see diag_mask) */
static char *
diag_prefix(int mask, int ind)
{
	static const struct { int bit; const char *fld; const char *item; } t[] = {
		{PLX_DIAG_DETAIL, "detail", "PG_EXCEPTION_DETAIL"},
		{PLX_DIAG_HINT, "hint", "PG_EXCEPTION_HINT"},
		{PLX_DIAG_CONSTRAINT, "constraint", "CONSTRAINT_NAME"},
		{PLX_DIAG_COLUMN, "column", "COLUMN_NAME"},
		{PLX_DIAG_TABLE, "table", "TABLE_NAME"},
		{PLX_DIAG_SCHEMA, "schema", "SCHEMA_NAME"},
		{PLX_DIAG_DATATYPE, "datatype", "PG_DATATYPE_NAME"},
	};
	StringInfoData p;
	int			i;

	if (!mask)
		return pstrdup("");
	initStringInfo(&p);
	for (i = 0; i < (int) (sizeof(t) / sizeof(t[0])); i++)
		if (mask & t[i].bit)
		{
			appendStringInfoSpaces(&p, ind * 2);
			appendStringInfo(&p, "GET STACKED DIAGNOSTICS __plx_%s = %s;\n", t[i].fld, t[i].item);
		}
	return p.data;
}

/* begin [body] [rescue [Class] [=> e] handler]* [ensure body] end */
static void
parse_begin(Ctx *cx, int ind)
{
	StringInfo	o = &cx->out;
	int			bl = cx->t[cx->pos].line;
	char	   *body;
	StringInfoData arms;
	int			nrescue = 0;
	char	   *ensure_body = NULL;

	cx->pos++;					/* consume 'begin' */
	body = capture_block(cx, ind + 1);

	initStringInfo(&arms);
	while (cx->t[cx->pos].kind == T_KW && cx->t[cx->pos].kw == KW_RESCUE)
	{
		int			rs = cx->pos;
		int			re = stmt_end(cx, rs);
		const char *cond = "OTHERS";
		int			arrow = -1, i;
		int			ev_a = -1;
		const char *save_ev = cx->exc_var;
		int			save_evl = cx->exc_varlen;
		int			cls_a, cls_b;
		char	   *hb;

		for (i = rs + 1; i < re; i++)
			if (tok_is(&cx->t[i], "=>"))
			{
				arrow = i;
				break;
			}
		if (arrow >= 0 && arrow + 1 < re && cx->t[arrow + 1].kind == T_IDENT)
			ev_a = arrow + 1;
		cls_a = rs + 1;
		cls_b = (arrow >= 0) ? arrow : re;
		if (cls_b > cls_a)
		{
			char	   *clstext = span_text(cx, cls_a, cls_b);
			const char *c = exc_class_to_condition(clstext, (int) strlen(clstext));

			if (!c)
				plx_err(cx, cx->t[rs].line, "unsupported exception class \"%s\"", clstext);
			cond = c;
		}
		cx->pos = re;
		if (ev_a >= 0)
		{
			cx->exc_var = cx->t[ev_a].s;
			cx->exc_varlen = cx->t[ev_a].len;
		}
		{
			int			save_diag = cx->diag_mask;
			char	   *pfx;

			cx->diag_mask = 0;
			cx->handlerdepth++;
			hb = capture_block(cx, ind + 2);
			cx->handlerdepth--;
			pfx = diag_prefix(cx->diag_mask, ind + 2);
			cx->diag_mask = save_diag;
			cx->exc_var = save_ev;
			cx->exc_varlen = save_evl;

			appendStringInfoSpaces(&arms, (ind + 1) * 2);
			appendStringInfo(&arms, "WHEN %s THEN\n", cond);
			appendStringInfoString(&arms, pfx);
			appendStringInfoString(&arms, hb);
		}
		nrescue++;
	}

	if (cx->t[cx->pos].kind == T_KW && cx->t[cx->pos].kw == KW_ENSURE)
	{
		cx->pos++;
		ensure_body = capture_block(cx, ind + 1);
	}

	if (cx->t[cx->pos].kind == T_KW && cx->t[cx->pos].kw == KW_END)
		cx->pos++;
	else
		plx_err(cx, bl, "unterminated begin block");

	if (!ensure_body)
	{
		indent(o, ind);
		appendStringInfoString(o, "BEGIN\n");
		appendStringInfoString(o, body);
		if (nrescue)
		{
			indent(o, ind);
			appendStringInfoString(o, "EXCEPTION\n");
			appendStringInfoString(o, arms.data);
		}
		indent(o, ind);
		appendStringInfoString(o, "END;\n");
	}
	else
	{
		indent(o, ind);
		appendStringInfoString(o, "BEGIN\n");
		indent(o, ind + 1);
		appendStringInfoString(o, "BEGIN\n");
		appendStringInfoString(o, body);
		if (nrescue)
		{
			indent(o, ind + 1);
			appendStringInfoString(o, "EXCEPTION\n");
			appendStringInfoString(o, arms.data);
		}
		indent(o, ind + 1);
		appendStringInfoString(o, "END;\n");
		appendStringInfoString(o, ensure_body);
		indent(o, ind);
		appendStringInfoString(o, "EXCEPTION WHEN OTHERS THEN\n");
		appendStringInfoString(o, ensure_body);
		indent(o, ind + 1);
		appendStringInfoString(o, "RAISE;\n");
		indent(o, ind);
		appendStringInfoString(o, "END;\n");
	}
}

/* rewrite a comma-separated value list (for CASE WHEN v1, v2) */
static char *
rewrite_value_list(Ctx *cx, int a, int b)
{
	StringInfoData out;
	int			i, seg = a, depth = 0, first = 1;

	initStringInfo(&out);
	for (i = a; i <= b; i++)
	{
		if (i < b && (cx->t[i].kind == T_LPAREN || cx->t[i].kind == T_LBRACKET))
			depth++;
		else if (i < b && (cx->t[i].kind == T_RPAREN || cx->t[i].kind == T_RBRACKET))
			depth--;
		if (i == b || (depth == 0 && cx->t[i].kind == T_COMMA))
		{
			char	   *st = span_text(cx, seg, i);

			if (!first)
				appendStringInfoString(&out, ", ");
			appendStringInfoString(&out, rewrite_expr(cx, st, (int) strlen(st), false));
			first = 0;
			seg = i + 1;
		}
	}
	return out.data;
}

/*
 * Ruby case/when -> plpgsql CASE. With a subject expression it is a simple CASE
 * (WHEN value-list); without one it is a searched CASE (WHEN condition).
 */
static void
parse_case(Ctx *cx, int ind)
{
	StringInfo	o = &cx->out;
	int			start = cx->pos;
	int			hdln = cx->t[start].line;
	int			e = stmt_end(cx, start);
	bool		simple = (e > start + 1);
	char	   *subj = NULL;

	if (simple)
	{
		char	   *st = span_text(cx, start + 1, e);

		subj = rewrite_expr(cx, st, (int) strlen(st), false);
	}
	cx->pos = e;
	skip_seps(cx);

	indent(o, ind);
	if (simple)
		appendStringInfo(o, "CASE %s\n", subj);
	else
		appendStringInfoString(o, "CASE\n");

	if (!(cx->t[cx->pos].kind == T_KW && cx->t[cx->pos].kw == KW_WHEN))
		plx_err(cx, hdln, "case requires at least one 'when'");

	while (cx->t[cx->pos].kind == T_KW && cx->t[cx->pos].kw == KW_WHEN)
	{
		int			ws = cx->pos;
		int			we = stmt_end(cx, ws);
		int			va = ws + 1, vb = we;
		char	   *vals;

		if (vb > va && cx->t[vb - 1].kind == T_KW && cx->t[vb - 1].kw == KW_THEN)
			vb--;
		if (va >= vb)
			plx_err(cx, cx->t[ws].line, "when requires a value or condition");
		if (simple)
			vals = rewrite_value_list(cx, va, vb);
		else
		{
			char	   *st = span_text(cx, va, vb);

			vals = rewrite_expr(cx, st, (int) strlen(st), true);
		}
		indent(o, ind + 1);
		appendStringInfo(o, "WHEN %s THEN\n", vals);
		cx->pos = we;
		parse_block(cx, ind + 2);
	}
	if (cx->t[cx->pos].kind == T_KW && cx->t[cx->pos].kw == KW_ELSE)
	{
		indent(o, ind + 1);
		appendStringInfoString(o, "ELSE\n");
		cx->pos++;
		parse_block(cx, ind + 2);
	}
	if (cx->t[cx->pos].kind == T_KW && cx->t[cx->pos].kw == KW_END)
		cx->pos++;
	else
		plx_err(cx, hdln, "unterminated case block");
	indent(o, ind);
	appendStringInfoString(o, "END CASE;\n");
}

/* parse statements until a closer keyword or EOF (does not consume closer) */
static void
parse_block(Ctx *cx, int ind)
{
	for (;;)
	{
		Tok		   *tk;

		skip_seps(cx);
		tk = &cx->t[cx->pos];
		if (tk->kind == T_EOF)
			return;
		if (tk->kind == T_KW &&
			(tk->kw == KW_END || tk->kw == KW_ELSIF || tk->kw == KW_ELSE ||
			 tk->kw == KW_WHEN || tk->kw == KW_RESCUE || tk->kw == KW_ENSURE))
			return;
		parse_stmt(cx, ind, false);
	}
}

static void
parse_stmt_inner(Ctx *cx, int ind, bool toplevel)
{
	Tok		   *tk;

	skip_seps(cx);
	tk = &cx->t[cx->pos];
	if (tk->kind == T_EOF)
		return;
	/* loop label:  name: for i in ... / name: while ... / name: loop do */
	if (tk->kind == T_IDENT && tok_is(&cx->t[cx->pos + 1], ":") &&
		cx->t[cx->pos + 2].kind == T_KW &&
		(cx->t[cx->pos + 2].kw == KW_FOR || cx->t[cx->pos + 2].kw == KW_WHILE ||
		 cx->t[cx->pos + 2].kw == KW_UNTIL || cx->t[cx->pos + 2].kw == KW_LOOP))
	{
		indent(&cx->out, ind);
		appendStringInfo(&cx->out, "<<%.*s>>\n", tk->len, tk->s);
		cx->pos += 2;
		tk = &cx->t[cx->pos];
	}
	if (tk->kind == T_KW)
	{
		switch (tk->kw)
		{
			case KW_IF:
			case KW_UNLESS:
				/* could be a block opener OR a leaf w/ modifier — decide:
				 * modifier form has the opener keyword NOT at stmt start. Here
				 * tk IS at stmt start, so it's a block opener. */
				parse_if(cx, ind);
				return;
			case KW_WHILE:
			case KW_UNTIL:
				parse_while(cx, ind);
				return;
			case KW_FOR:
				parse_for(cx, ind);
				return;
			case KW_LOOP:
				parse_loop(cx, ind);
				return;
			case KW_DEF:
				plx_err(cx, tk->line, "nested 'def' is not supported");
				return;
			case KW_CASE:
				parse_case(cx, ind);
				return;
			case KW_BEGIN:
				parse_begin(cx, ind);
				return;
			default:
				break;
		}
	}
	/* leaf statement, or an iterator block (`... .each do |v| ... end`) */
	{
		int			a = cx->pos;
		int			b = stmt_end(cx, a);
		int			i, do_pos = -1, depth = 0;

		for (i = a; i < b; i++)
		{
			if (cx->t[i].kind == T_LPAREN || cx->t[i].kind == T_LBRACKET)
				depth++;
			else if (cx->t[i].kind == T_RPAREN || cx->t[i].kind == T_RBRACKET)
				depth--;
			else if (depth == 0 && cx->t[i].kind == T_KW && cx->t[i].kw == KW_DO)
			{
				do_pos = i;
				break;
			}
		}
		if (do_pos >= 0)
			parse_iter(cx, a, do_pos, b, ind);
		else
		{
			emit_leaf(cx, a, b, ind, toplevel);
			cx->pos = b;
		}
	}
}

/* ---------------------------------------------------------------- brace parser (PHP/JS) */

/* expects cx->pos at '('; returns inner token range [*a,*b); leaves pos past ')' */
static void
paren_group(Ctx *cx, int *a, int *b)
{
	int			p = cx->pos, depth;

	if (cx->t[p].kind != T_LPAREN)
		plx_err(cx, cx->t[p].line, "expected '('");
	p++;
	*a = p;
	depth = 1;
	while (cx->t[p].kind != T_EOF && depth > 0)
	{
		if (cx->t[p].kind == T_LPAREN)
			depth++;
		else if (cx->t[p].kind == T_RPAREN)
		{
			depth--;
			if (!depth)
				break;
		}
		p++;
	}
	if (cx->t[p].kind != T_RPAREN)
		plx_err(cx, cx->t[cx->pos].line, "unterminated '('");
	*b = p;
	cx->pos = p + 1;
}

static char *
rw_range(Ctx *cx, int a, int b, bool boolctx)
{
	char	   *s = span_text(cx, a, b);

	return rewrite_expr(cx, s, (int) strlen(s), boolctx);
}

/* find next top-level ';' from a (paren/bracket/brace depth 0) */
static int
brace_stmt_end(Ctx *cx, int a)
{
	int			i = a, depth = 0;

	while (cx->t[i].kind != T_EOF)
	{
		TokKind		k = cx->t[i].kind;

		if (k == T_LPAREN || k == T_LBRACKET || k == T_LBRACE)
			depth++;
		else if (k == T_RPAREN || k == T_RBRACKET || k == T_RBRACE)
		{
			if (depth == 0)
				break;
			depth--;
		}
		else if (k == T_SEMI && depth == 0)
			break;
		i++;
	}
	return i;
}

static void
parse_if_brace(Ctx *cx, int ind)
{
	StringInfo	o = &cx->out;
	int			ca, cb;

	cx->pos++;					/* if */
	paren_group(cx, &ca, &cb);
	indent(o, ind);
	appendStringInfo(o, "IF %s THEN\n", rw_range(cx, ca, cb, true));
	parse_brace_block(cx, ind + 1);

	for (;;)
	{
		bool		is_elseif, is_else_if;

		skip_seps(cx);
		is_elseif = (cx->t[cx->pos].kind == T_KW && cx->t[cx->pos].kw == KW_ELSIF);
		is_else_if = (cx->t[cx->pos].kind == T_KW && cx->t[cx->pos].kw == KW_ELSE &&
					  cx->t[cx->pos + 1].kind == T_KW && cx->t[cx->pos + 1].kw == KW_IF);

		if (!is_elseif && !is_else_if)
			break;
		cx->pos += is_else_if ? 2 : 1;
		paren_group(cx, &ca, &cb);
		indent(o, ind);
		appendStringInfo(o, "ELSIF %s THEN\n", rw_range(cx, ca, cb, true));
		parse_brace_block(cx, ind + 1);
	}
	if (cx->t[cx->pos].kind == T_KW && cx->t[cx->pos].kw == KW_ELSE)
	{
		cx->pos++;
		indent(o, ind);
		appendStringInfoString(o, "ELSE\n");
		parse_brace_block(cx, ind + 1);
	}
	indent(o, ind);
	appendStringInfoString(o, "END IF;\n");
}

static void
parse_while_brace(Ctx *cx, int ind)
{
	StringInfo	o = &cx->out;
	int			ca, cb;

	cx->pos++;
	paren_group(cx, &ca, &cb);
	indent(o, ind);
	appendStringInfo(o, "WHILE %s LOOP\n", rw_range(cx, ca, cb, true));
	cx->loopdepth++;
	parse_brace_block(cx, ind + 1);
	cx->loopdepth--;
	indent(o, ind);
	appendStringInfoString(o, "END LOOP;\n");
}

/* emit `FOR <var> IN <query(...)> LOOP <body> END LOOP;` for a query receiver */
static void
brace_query_loop(Ctx *cx, Tok *var, int ra, int rb, int ind)
{
	StringInfo	o = &cx->out;
	int			sa[16], se[16], after, n;
	int			savedpos = cx->pos;

	/* array receiver (not query(...)) -> FOREACH v IN ARRAY expr LOOP */
	if (!(cx->t[ra].kind == T_IDENT && name_eq(&cx->t[ra], "query") &&
		  ra + 1 < rb && cx->t[ra + 1].kind == T_LPAREN))
	{
		char	   *arrx = rw_range(cx, ra, rb, false);
		PlxLocal2  *lv;

		if (is_param(cx, var->s, var->len))
			plx_err(cx, var->line, "foreach-array loop variable must be a local, not a parameter");
		lv = local_find(cx, var->s, var->len);
		if (!lv || !lv->typ)
			plx_err(cx, var->line,
					"foreach over an array requires the loop variable to be annotated with its element type before the loop");
		indent(o, ind);
		appendStringInfo(o, "FOREACH %.*s IN ARRAY %s%s LOOP\n",
						 var->len, var->s, arrx[0] == '[' ? "ARRAY" : "", arrx);
		cx->loopdepth++;
		parse_brace_block(cx, ind + 1);
		cx->loopdepth--;
		indent(o, ind);
		appendStringInfoString(o, "END LOOP;\n");
		return;
	}
	cx->pos = ra;
	n = parse_args(cx, ra, sa, se, 16, &after);
	cx->pos = savedpos;
	if (!is_param(cx, var->s, var->len))
	{
		PlxLocal2  *l = local_find(cx, var->s, var->len);

		if (!l)
			l = local_add(cx, var->s, var->len);
		l->is_record = true;
	}
	indent(o, ind);
	if (n == 1 && arg_is_string_literal(cx, sa[0], se[0]))
	{
		appendStringInfo(o, "FOR %.*s IN ", var->len, var->s);
		emit_string_as_sql(cx, &cx->t[sa[0]], o);
		appendStringInfoString(o, " LOOP\n");
	}
	else
	{
		char	   *sqlv = rw_range(cx, sa[0], se[0], false);
		char	   *binds = binds_text(cx, sa, se, n);

		if (binds)
			appendStringInfo(o, "FOR %.*s IN EXECUTE %s USING %s LOOP\n",
							 var->len, var->s, sqlv, binds);
		else
			appendStringInfo(o, "FOR %.*s IN EXECUTE %s LOOP\n",
							 var->len, var->s, sqlv);
	}
	cx->loopdepth++;
	parse_brace_block(cx, ind + 1);
	cx->loopdepth--;
	indent(o, ind);
	appendStringInfoString(o, "END LOOP;\n");
}

/* C-style counting for, or JS `for (v of query(...))` */
static void
parse_for_brace(Ctx *cx, int ind)
{
	StringInfo	o = &cx->out;
	int			ca, cb;
	int			s1 = -1, s2 = -1, of_at = -1, i, depth = 0;
	int			line = cx->t[cx->pos].line;

	cx->pos++;
	paren_group(cx, &ca, &cb);
	/* strip a leading let/const/var declaration keyword */
	if (cx->t[ca].kind == T_KW && cx->t[ca].kw == KW_LET)
		ca++;
	/* scan for top-level ';' (C-for) and KW_OF (for-of) */
	for (i = ca; i < cb; i++)
	{
		if (cx->t[i].kind == T_LPAREN || cx->t[i].kind == T_LBRACKET)
			depth++;
		else if (cx->t[i].kind == T_RPAREN || cx->t[i].kind == T_RBRACKET)
			depth--;
		else if (depth == 0 && cx->t[i].kind == T_SEMI)
		{
			if (s1 < 0)
				s1 = i;
			else
				s2 = i;
		}
		else if (depth == 0 && cx->t[i].kind == T_KW && cx->t[i].kw == KW_OF)
			of_at = i;
	}
	if (of_at >= 0)
	{
		if (cx->t[ca].kind != T_IDENT)
			plx_err(cx, line, "for-of requires a loop variable");
		brace_query_loop(cx, &cx->t[ca], of_at + 1, cb, ind);
		return;
	}
	if (s1 < 0 || s2 < 0)
		plx_err(cx, line, "for must be a counting loop 'for (v = LO; v < HI; v++)' or 'for (v of query(...))'");
	/* init: IDENT = LO ; cond: IDENT </<= HI ; incr: IDENT ++ | += K */
	{
		int			ia = ca, ib = s1, cca = s1 + 1, ccb = s2, xa = s2 + 1, xb = cb;
		char	   *var;
		bool		lt;
		char	   *lo, *hi, *step = NULL;

		if (!(cx->t[ia].kind == T_IDENT && ib - ia >= 3 && tok_is(&cx->t[ia + 1], "=")))
			plx_err(cx, line, "for-loop init must be 'v = LO'");
		var = pnstrdup(cx->t[ia].s, cx->t[ia].len);
		lo = rw_range(cx, ia + 2, ib, false);
		/* cond: var < HI or var <= HI */
		if (!(cx->t[cca].kind == T_IDENT && (tok_is(&cx->t[cca + 1], "<") || tok_is(&cx->t[cca + 1], "<="))))
			plx_err(cx, line, "for-loop condition must be 'v < HI' or 'v <= HI'");
		lt = tok_is(&cx->t[cca + 1], "<");
		hi = rw_range(cx, cca + 2, ccb, false);
		/* incr: var++ or var += K or var = var + K */
		if (cx->t[xa].kind == T_IDENT && tok_is(&cx->t[xa + 1], "++"))
			step = NULL;
		else if (cx->t[xa].kind == T_IDENT && cx->t[xa + 1].kind == T_OP &&
				 cx->t[xa + 1].len == 2 && cx->t[xa + 1].s[0] == '+' && cx->t[xa + 1].s[1] == '=')
			step = rw_range(cx, xa + 2, xb, false);
		else
			plx_err(cx, line, "for-loop step must be 'v++' or 'v += K'");
		indent(o, ind);
		if (lt)
			appendStringInfo(o, "FOR %s IN %s..(%s - 1)%s%s LOOP\n", var, lo, hi,
							 step ? " BY " : "", step ? step : "");
		else
			appendStringInfo(o, "FOR %s IN %s..%s%s%s LOOP\n", var, lo, hi,
							 step ? " BY " : "", step ? step : "");
	}
	cx->loopdepth++;
	parse_brace_block(cx, ind + 1);
	cx->loopdepth--;
	indent(o, ind);
	appendStringInfoString(o, "END LOOP;\n");
}

/* foreach (query(...) as row) { ... } -> FOR row IN <sql> LOOP */
static void
parse_foreach_brace(Ctx *cx, int ind)
{
	int			ca, cb, as_at = -1, i, depth = 0;
	int			line = cx->t[cx->pos].line;
	int			rv;

	cx->pos++;
	paren_group(cx, &ca, &cb);
	for (i = ca; i < cb; i++)
	{
		if (cx->t[i].kind == T_LPAREN)
			depth++;
		else if (cx->t[i].kind == T_RPAREN)
			depth--;
		else if (depth == 0 && cx->t[i].kind == T_KW && cx->t[i].kw == KW_AS)
		{
			as_at = i;
			break;
		}
	}
	if (as_at < 0)
		plx_err(cx, line, "foreach requires 'as'");
	/* row var: last IDENT before ')' (skip a key form -> take value var) */
	rv = cb - 1;
	while (rv > as_at && cx->t[rv].kind != T_IDENT)
		rv--;
	if (rv <= as_at)
		plx_err(cx, line, "foreach requires a row variable");
	brace_query_loop(cx, &cx->t[rv], ca, as_at, ind);
}

/* try { } catch (Class $e) { } [catch...] [finally { }] */
static void
parse_try_brace(Ctx *cx, int ind)
{
	StringInfo	o = &cx->out;
	int			line = cx->t[cx->pos].line;
	char	   *body;
	StringInfoData arms;
	int			nrescue = 0;
	char	   *ensure_body = NULL;

	cx->pos++;					/* try */
	{
		StringInfoData saved = cx->out;

		initStringInfo(&cx->out);
		parse_brace_block(cx, ind + 1);
		body = cx->out.data;
		cx->out = saved;
	}

	initStringInfo(&arms);
	skip_seps(cx);
	while (cx->t[cx->pos].kind == T_KW && cx->t[cx->pos].kw == KW_RESCUE)	/* catch */
	{
		int			ca, cb, i, ev_a = -1;
		const char *cond = "OTHERS";
		const char *save_ev = cx->exc_var;
		int			save_evl = cx->exc_varlen;
		char	   *hb;
		StringInfoData saved;

		cx->pos++;
		paren_group(cx, &ca, &cb);
		/* catch (\Class $e): last IDENT is $e; the class tokens precede it */
		for (i = cb - 1; i >= ca; i--)
			if (cx->t[i].kind == T_IDENT)
			{
				ev_a = i;
				break;
			}
		if (ev_a > ca)
		{
			char	   *cls = span_text(cx, ca, ev_a);
			const char *c;

			/* strip leading backslash/namespace for a bare class name */
			while (*cls == '\\' || *cls == ' ')
				cls++;
			c = exc_class_to_condition(cls, (int) strlen(cls));
			if (c)
				cond = c;		/* else OTHERS (e.g. \Exception) */
		}
		if (ev_a >= 0)
		{
			cx->exc_var = cx->t[ev_a].s;
			cx->exc_varlen = cx->t[ev_a].len;
		}
		{
			int			save_diag = cx->diag_mask;
			char	   *pfx;

			cx->diag_mask = 0;
			cx->handlerdepth++;
			saved = cx->out;
			initStringInfo(&cx->out);
			parse_brace_block(cx, ind + 2);
			hb = cx->out.data;
			cx->out = saved;
			cx->handlerdepth--;
			pfx = diag_prefix(cx->diag_mask, ind + 2);
			cx->diag_mask = save_diag;
			cx->exc_var = save_ev;
			cx->exc_varlen = save_evl;

			appendStringInfoSpaces(&arms, (ind + 1) * 2);
			appendStringInfo(&arms, "WHEN %s THEN\n", cond);
			appendStringInfoString(&arms, pfx);
			appendStringInfoString(&arms, hb);
		}
		nrescue++;
		skip_seps(cx);
	}
	if (cx->t[cx->pos].kind == T_KW && cx->t[cx->pos].kw == KW_ENSURE)	/* finally */
	{
		StringInfoData saved = cx->out;

		cx->pos++;
		initStringInfo(&cx->out);
		parse_brace_block(cx, ind + 1);
		ensure_body = cx->out.data;
		cx->out = saved;
	}

	if (!ensure_body)
	{
		indent(o, ind);
		appendStringInfoString(o, "BEGIN\n");
		appendStringInfoString(o, body);
		if (nrescue)
		{
			indent(o, ind);
			appendStringInfoString(o, "EXCEPTION\n");
			appendStringInfoString(o, arms.data);
		}
		indent(o, ind);
		appendStringInfoString(o, "END;\n");
	}
	else
	{
		indent(o, ind);
		appendStringInfoString(o, "BEGIN\n");
		indent(o, ind + 1);
		appendStringInfoString(o, "BEGIN\n");
		appendStringInfoString(o, body);
		if (nrescue)
		{
			indent(o, ind + 1);
			appendStringInfoString(o, "EXCEPTION\n");
			appendStringInfoString(o, arms.data);
		}
		indent(o, ind + 1);
		appendStringInfoString(o, "END;\n");
		appendStringInfoString(o, ensure_body);
		indent(o, ind);
		appendStringInfoString(o, "EXCEPTION WHEN OTHERS THEN\n");
		appendStringInfoString(o, ensure_body);
		indent(o, ind + 1);
		appendStringInfoString(o, "RAISE;\n");
		indent(o, ind);
		appendStringInfoString(o, "END;\n");
	}
	(void) line;
}

/* throw new <Class>(msg[, ...]) -> RAISE EXCEPTION '%', msg; */
static void
emit_php_throw(Ctx *cx, int a, int b, int ind)
{
	int			p = a + 1;		/* after 'throw' */
	int			call = -1, i;

	/* find the '(' of the exception constructor */
	for (i = p; i < b; i++)
		if (cx->t[i].kind == T_LPAREN)
		{
			call = i - 1;		/* the class-name token precedes '(' */
			break;
		}
	if (call < 0)
		plx_err(cx, cx->t[a].line, "throw requires 'new Exception(...)'");
	{
		int			sa[16], se[16], after, n;
		int			savedpos = cx->pos;

		cx->pos = call;
		n = parse_args(cx, call, sa, se, 16, &after);
		cx->pos = savedpos;
		indent(&cx->out, ind);
		if (n >= 1)
			appendStringInfo(&cx->out, "RAISE EXCEPTION '%%', %s;\n",
							 rw_range(cx, sa[0], se[0], false));
		else
			appendStringInfoString(&cx->out, "RAISE EXCEPTION 'exception';\n");
	}
}

/* switch (expr) { case v: ...; break; [case..] default: ... } -> CASE ... END CASE */
static void
parse_switch_brace(Ctx *cx, int ind)
{
	StringInfo	o = &cx->out;
	int			line = cx->t[cx->pos].line;
	int			ca, cb;
	int			when_count = 0;		/* plpgsql CASE needs >= 1 WHEN, ELSE last */
	bool		seen_default = false;

	cx->pos++;					/* switch */
	paren_group(cx, &ca, &cb);
	indent(o, ind);
	appendStringInfo(o, "CASE %s\n", rw_range(cx, ca, cb, false));
	if (cx->t[cx->pos].kind != T_LBRACE)
		plx_err(cx, line, "switch requires a '{' block");
	cx->pos++;

	for (;;)
	{
		StringInfoData vals;
		bool		is_default = false;
		int			nvals = 0;

		skip_seps(cx);
		if (cx->t[cx->pos].kind == T_RBRACE)
		{
			cx->pos++;
			break;
		}
		if (cx->t[cx->pos].kind == T_EOF)
			plx_err(cx, line, "unterminated switch block");

		/* collect one or more consecutive case/default labels */
		initStringInfo(&vals);
		for (;;)
		{
			if (cx->t[cx->pos].kind == T_KW && cx->t[cx->pos].kw == KW_WHEN)	/* case */
			{
				int			va = ++cx->pos, ve = va;

				while (cx->t[ve].kind != T_EOF && !tok_is(&cx->t[ve], ":"))
					ve++;
				if (cx->t[ve].kind == T_EOF)
					plx_err(cx, line, "case label requires ':'");
				if (nvals++)
					appendStringInfoString(&vals, ", ");
				appendStringInfoString(&vals, rw_range(cx, va, ve, false));
				cx->pos = ve + 1;
			}
			else if (cx->t[cx->pos].kind == T_KW && cx->t[cx->pos].kw == KW_ELSE)	/* default */
			{
				cx->pos++;
				if (!tok_is(&cx->t[cx->pos], ":"))
					plx_err(cx, line, "default label requires ':'");
				cx->pos++;
				is_default = true;
			}
			else
				break;
			skip_seps(cx);
			if (!(cx->t[cx->pos].kind == T_KW &&
				  (cx->t[cx->pos].kw == KW_WHEN || cx->t[cx->pos].kw == KW_ELSE)))
				break;
		}
		if (!nvals && !is_default)
			plx_err(cx, line, "switch body must consist of case/default labels");

		indent(o, ind + 1);
		if (is_default)
		{
			seen_default = true;
			appendStringInfoString(o, "ELSE\n");
		}
		else
		{
			/* plpgsql requires every WHEN before the ELSE (default) */
			if (seen_default)
				plx_err(cx, line,
						"a case after default is not supported; put default last");
			when_count++;
			appendStringInfo(o, "WHEN %s THEN\n", vals.data);
		}

		/*
		 * Body: statements until break / next label / '}'. An arm may end with
		 * an explicit 'break' or with a terminating statement (return/throw);
		 * reaching the next label with neither is genuine C fall-through, which
		 * plpgsql CASE cannot represent, so it is rejected.
		 */
		{
			bool		terminated = false;

			for (;;)
			{
				Tok		   *st;

				skip_seps(cx);
				st = &cx->t[cx->pos];
				if (st->kind == T_KW && st->kw == KW_BREAK)
				{
					cx->pos++;
					skip_seps(cx);
					terminated = true;
					break;
				}
				if (st->kind == T_RBRACE)
					break;
				if (st->kind == T_KW && (st->kw == KW_WHEN || st->kw == KW_ELSE))
				{
					if (!terminated)
						plx_err(cx, st->line,
								"switch fall-through is not supported; end each case with 'break' or 'return'");
					break;
				}
				if (st->kind == T_EOF)
					plx_err(cx, line, "unterminated switch block");
				terminated = (st->kind == T_KW && (st->kw == KW_RETURN || st->kw == KW_RAISE));
				parse_brace_stmt(cx, ind + 2, false);
			}
		}
	}
	if (when_count == 0)
		plx_err(cx, line,
				"switch needs at least one case (a switch with only default is not supported)");
	indent(o, ind);
	appendStringInfoString(o, "END CASE;\n");
}

static void
parse_brace_stmt_inner(Ctx *cx, int ind, bool toplevel)
{
	Tok		   *tk;

	skip_seps(cx);
	tk = &cx->t[cx->pos];
	if (tk->kind == T_EOF || tk->kind == T_RBRACE)
		return;
	/* loop label:  name: for (...) {...}  /  name: while (...) {...} */
	if (tk->kind == T_IDENT && tok_is(&cx->t[cx->pos + 1], ":") &&
		cx->t[cx->pos + 2].kind == T_KW &&
		(cx->t[cx->pos + 2].kw == KW_FOR || cx->t[cx->pos + 2].kw == KW_WHILE))
	{
		indent(&cx->out, ind);
		appendStringInfo(&cx->out, "<<%.*s>>\n", tk->len, tk->s);
		cx->pos += 2;			/* consume name and ':' */
		tk = &cx->t[cx->pos];
	}
	if (tk->kind == T_KW)
	{
		switch (tk->kw)
		{
			case KW_IF:
				parse_if_brace(cx, ind);
				return;
			case KW_WHILE:
				parse_while_brace(cx, ind);
				return;
			case KW_FOR:
				parse_for_brace(cx, ind);
				return;
			case KW_FOREACH:
				parse_foreach_brace(cx, ind);
				return;
			case KW_BEGIN:		/* try */
				parse_try_brace(cx, ind);
				return;
			case KW_DEF:
				plx_err(cx, tk->line, "nested function definitions are not supported");
				return;
			case KW_CASE:
				parse_switch_brace(cx, ind);
				return;
			default:
				break;
		}
	}
	/* leaf statement ending at ';' */
	{
		int			a = cx->pos;
		int			b = brace_stmt_end(cx, a);

		if (tk->kind == T_KW && tk->kw == KW_RAISE)	/* throw */
			emit_php_throw(cx, a, b, ind);
		else
			emit_core(cx, a, b, ind, toplevel);
		cx->pos = (cx->t[b].kind == T_SEMI) ? b + 1 : b;
	}
}

static void
parse_brace_program(Ctx *cx)
{
	for (;;)
	{
		skip_seps(cx);
		if (cx->t[cx->pos].kind == T_EOF)
			break;
		if (cx->t[cx->pos].kind == T_RBRACE)
			plx_err(cx, cx->t[cx->pos].line, "unexpected '}'");
		parse_brace_stmt(cx, 1, true);
	}
}

static void
parse_brace_block(Ctx *cx, int ind)
{
	if (cx->t[cx->pos].kind != T_LBRACE)
		plx_err(cx, cx->t[cx->pos].line, "expected '{'");
	cx->pos++;
	for (;;)
	{
		skip_seps(cx);
		if (cx->t[cx->pos].kind == T_RBRACE)
		{
			cx->pos++;
			return;
		}
		if (cx->t[cx->pos].kind == T_EOF)
			plx_err(cx, cx->t[cx->pos].line, "unterminated '{' block");
		parse_brace_stmt(cx, ind, false);
	}
}

/* ---------------------------------------------------------------- python (indent) parser */

/* index of the ':' that ends a compound-statement header (bracket depth 0) */
static int
py_header_colon(Ctx *cx, int from)
{
	int			i = from, depth = 0;

	while (cx->t[i].kind != T_EOF && cx->t[i].kind != T_NEWLINE)
	{
		TokKind		k = cx->t[i].kind;

		if (k == T_LPAREN || k == T_LBRACKET || k == T_LBRACE)
			depth++;
		else if (k == T_RPAREN || k == T_RBRACKET || k == T_RBRACE)
			depth--;
		else if (depth == 0 && tok_is(&cx->t[i], ":"))
			return i;
		i++;
	}
	return -1;
}

/* consume NEWLINE INDENT <statements> DEDENT */
static void
parse_py_block(Ctx *cx, int ind)
{
	skip_seps(cx);
	if (cx->t[cx->pos].kind != T_INDENT)
		plx_err(cx, cx->t[cx->pos].line, "expected an indented block");
	cx->pos++;
	for (;;)
	{
		skip_seps(cx);
		if (cx->t[cx->pos].kind == T_DEDENT)
		{
			cx->pos++;
			return;
		}
		if (cx->t[cx->pos].kind == T_EOF)
			return;
		parse_py_stmt(cx, ind, false);
	}
}

static void
parse_py_if(Ctx *cx, int ind)
{
	StringInfo	o = &cx->out;
	int			hd = cx->t[cx->pos].line;
	int			colon;

	colon = py_header_colon(cx, cx->pos + 1);
	if (colon < 0)
		plx_err(cx, hd, "if header requires ':'");
	indent(o, ind);
	appendStringInfo(o, "IF %s THEN\n", rw_range(cx, cx->pos + 1, colon, true));
	cx->pos = colon + 1;
	parse_py_block(cx, ind + 1);
	while (cx->t[cx->pos].kind == T_KW && cx->t[cx->pos].kw == KW_ELSIF)	/* elif */
	{
		colon = py_header_colon(cx, cx->pos + 1);
		if (colon < 0)
			plx_err(cx, cx->t[cx->pos].line, "elif header requires ':'");
		indent(o, ind);
		appendStringInfo(o, "ELSIF %s THEN\n", rw_range(cx, cx->pos + 1, colon, true));
		cx->pos = colon + 1;
		parse_py_block(cx, ind + 1);
	}
	if (cx->t[cx->pos].kind == T_KW && cx->t[cx->pos].kw == KW_ELSE)
	{
		colon = py_header_colon(cx, cx->pos + 1);
		if (colon < 0)
			plx_err(cx, cx->t[cx->pos].line, "else header requires ':'");
		indent(o, ind);
		appendStringInfoString(o, "ELSE\n");
		cx->pos = colon + 1;
		parse_py_block(cx, ind + 1);
	}
	indent(o, ind);
	appendStringInfoString(o, "END IF;\n");
}

static void
parse_py_while(Ctx *cx, int ind)
{
	StringInfo	o = &cx->out;
	int			hd = cx->t[cx->pos].line;
	int			colon = py_header_colon(cx, cx->pos + 1);

	if (colon < 0)
		plx_err(cx, hd, "while header requires ':'");
	indent(o, ind);
	appendStringInfo(o, "WHILE %s LOOP\n", rw_range(cx, cx->pos + 1, colon, true));
	cx->pos = colon + 1;
	cx->loopdepth++;
	parse_py_block(cx, ind + 1);
	cx->loopdepth--;
	indent(o, ind);
	appendStringInfoString(o, "END LOOP;\n");
}

/* for VAR in <range(...)|query(...)|array>: */
static void
parse_py_for(Ctx *cx, int ind)
{
	StringInfo	o = &cx->out;
	int			hd = cx->t[cx->pos].line;
	int			colon = py_header_colon(cx, cx->pos + 1);
	int			var, in_at, ia, ib;

	if (colon < 0)
		plx_err(cx, hd, "for header requires ':'");
	var = cx->pos + 1;
	if (cx->t[var].kind != T_IDENT)
		plx_err(cx, hd, "for requires a loop variable");
	in_at = var + 1;
	if (!(cx->t[in_at].kind == T_KW && cx->t[in_at].kw == KW_IN))
		plx_err(cx, hd, "for requires 'in'");
	ia = in_at + 1;
	ib = colon;

	if (cx->t[ia].kind == T_IDENT && name_eq(&cx->t[ia], "range") &&
		cx->t[ia + 1].kind == T_LPAREN)
	{
		int			as[8], ae[8], after, n;
		char	   *lo, *hi, *step = NULL;

		n = parse_args(cx, ia, as, ae, 8, &after);
		if (n == 1)
		{
			lo = pstrdup("0");
			hi = rw_range(cx, as[0], ae[0], false);
		}
		else if (n >= 2)
		{
			lo = rw_range(cx, as[0], ae[0], false);
			hi = rw_range(cx, as[1], ae[1], false);
			if (n >= 3)
				step = rw_range(cx, as[2], ae[2], false);
		}
		else
			plx_err(cx, hd, "range requires 1 to 3 arguments");
		indent(o, ind);
		appendStringInfo(o, "FOR %.*s IN %s..(%s - 1)%s%s LOOP\n",
						 cx->t[var].len, cx->t[var].s, lo, hi,
						 step ? " BY " : "", step ? step : "");
		cx->pos = colon + 1;
		cx->loopdepth++;
		parse_py_block(cx, ind + 1);
		cx->loopdepth--;
	}
	else if (cx->t[ia].kind == T_IDENT && name_eq(&cx->t[ia], "query") &&
			 cx->t[ia + 1].kind == T_LPAREN)
	{
		int			as[16], ae[16], after, n;

		n = parse_args(cx, ia, as, ae, 16, &after);
		if (!is_param(cx, cx->t[var].s, cx->t[var].len))
		{
			PlxLocal2  *l = local_find(cx, cx->t[var].s, cx->t[var].len);

			if (!l)
				l = local_add(cx, cx->t[var].s, cx->t[var].len);
			l->is_record = true;
		}
		indent(o, ind);
		if (n == 1 && arg_is_string_literal(cx, as[0], ae[0]))
		{
			appendStringInfo(o, "FOR %.*s IN ", cx->t[var].len, cx->t[var].s);
			emit_string_as_sql(cx, &cx->t[as[0]], o);
			appendStringInfoString(o, " LOOP\n");
		}
		else
		{
			char	   *sqlv = rw_range(cx, as[0], ae[0], false);
			char	   *binds = binds_text(cx, as, ae, n);

			if (binds)
				appendStringInfo(o, "FOR %.*s IN EXECUTE %s USING %s LOOP\n",
								 cx->t[var].len, cx->t[var].s, sqlv, binds);
			else
				appendStringInfo(o, "FOR %.*s IN EXECUTE %s LOOP\n",
								 cx->t[var].len, cx->t[var].s, sqlv);
		}
		cx->pos = colon + 1;
		cx->loopdepth++;
		parse_py_block(cx, ind + 1);
		cx->loopdepth--;
	}
	else
	{
		/* FOREACH over an array */
		char	   *arrx = rw_range(cx, ia, ib, false);
		PlxLocal2  *lv;

		if (is_param(cx, cx->t[var].s, cx->t[var].len))
			plx_err(cx, hd, "foreach-array loop variable must be a local, not a parameter");
		lv = local_find(cx, cx->t[var].s, cx->t[var].len);
		if (!lv || !lv->typ)
			plx_err(cx, hd, "iterating an array requires the loop variable to be annotated with its element type before the loop");
		indent(o, ind);
		appendStringInfo(o, "FOREACH %.*s IN ARRAY %s%s LOOP\n",
						 cx->t[var].len, cx->t[var].s, arrx[0] == '[' ? "ARRAY" : "", arrx);
		cx->pos = colon + 1;
		cx->loopdepth++;
		parse_py_block(cx, ind + 1);
		cx->loopdepth--;
	}
	indent(o, ind);
	appendStringInfoString(o, "END LOOP;\n");
}

/* try: <body> except [Class as e]: <handler> ... [finally: <body>] */
static void
parse_py_try(Ctx *cx, int ind)
{
	StringInfo	o = &cx->out;
	int			hd = cx->t[cx->pos].line;
	int			colon;
	char	   *body;
	StringInfoData saved,
				arms;
	int			nrescue = 0;
	char	   *ensure_body = NULL;

	colon = py_header_colon(cx, cx->pos + 1);
	if (colon < 0)
		plx_err(cx, hd, "try requires ':'");
	cx->pos = colon + 1;
	saved = cx->out;
	initStringInfo(&cx->out);
	parse_py_block(cx, ind + 1);
	body = cx->out.data;
	cx->out = saved;

	initStringInfo(&arms);
	while (cx->t[cx->pos].kind == T_KW && cx->t[cx->pos].kw == KW_RESCUE)	/* except */
	{
		int			es = cx->pos;
		const char *cond = "OTHERS";
		int			ev_a = -1,
					as_at = -1,
					i;
		const char *save_ev = cx->exc_var;
		int			save_evl = cx->exc_varlen;
		int			save_diag;
		char	   *hb,
				   *pfx;

		colon = py_header_colon(cx, es + 1);
		if (colon < 0)
			plx_err(cx, cx->t[es].line, "except requires ':'");
		/* except Class as e:  or  except Class:  or  except: */
		for (i = es + 1; i < colon; i++)
			if (cx->t[i].kind == T_KW && cx->t[i].kw == KW_AS)
			{
				as_at = i;
				break;
			}
		if (as_at >= 0 && as_at + 1 < colon && cx->t[as_at + 1].kind == T_IDENT)
			ev_a = as_at + 1;
		{
			int			cls_b = (as_at >= 0) ? as_at : colon;

			if (cls_b > es + 1)
			{
				char	   *cls = span_text(cx, es + 1, cls_b);
				const char *c = exc_class_to_condition(cls, (int) strlen(cls));

				if (c)
					cond = c;
			}
		}
		cx->pos = colon + 1;
		if (ev_a >= 0)
		{
			cx->exc_var = cx->t[ev_a].s;
			cx->exc_varlen = cx->t[ev_a].len;
		}
		save_diag = cx->diag_mask;
		cx->diag_mask = 0;
		cx->handlerdepth++;
		saved = cx->out;
		initStringInfo(&cx->out);
		parse_py_block(cx, ind + 2);
		hb = cx->out.data;
		cx->out = saved;
		cx->handlerdepth--;
		pfx = diag_prefix(cx->diag_mask, ind + 2);
		cx->diag_mask = save_diag;
		cx->exc_var = save_ev;
		cx->exc_varlen = save_evl;

		appendStringInfoSpaces(&arms, (ind + 1) * 2);
		appendStringInfo(&arms, "WHEN %s THEN\n", cond);
		appendStringInfoString(&arms, pfx);
		appendStringInfoString(&arms, hb);
		nrescue++;
	}
	if (cx->t[cx->pos].kind == T_KW && cx->t[cx->pos].kw == KW_ENSURE)	/* finally */
	{
		colon = py_header_colon(cx, cx->pos + 1);
		if (colon < 0)
			plx_err(cx, cx->t[cx->pos].line, "finally requires ':'");
		cx->pos = colon + 1;
		saved = cx->out;
		initStringInfo(&cx->out);
		parse_py_block(cx, ind + 1);
		ensure_body = cx->out.data;
		cx->out = saved;
	}

	if (!ensure_body)
	{
		indent(o, ind);
		appendStringInfoString(o, "BEGIN\n");
		appendStringInfoString(o, body);
		if (nrescue)
		{
			indent(o, ind);
			appendStringInfoString(o, "EXCEPTION\n");
			appendStringInfoString(o, arms.data);
		}
		indent(o, ind);
		appendStringInfoString(o, "END;\n");
	}
	else
	{
		indent(o, ind);
		appendStringInfoString(o, "BEGIN\n");
		indent(o, ind + 1);
		appendStringInfoString(o, "BEGIN\n");
		appendStringInfoString(o, body);
		if (nrescue)
		{
			indent(o, ind + 1);
			appendStringInfoString(o, "EXCEPTION\n");
			appendStringInfoString(o, arms.data);
		}
		indent(o, ind + 1);
		appendStringInfoString(o, "END;\n");
		appendStringInfoString(o, ensure_body);
		indent(o, ind);
		appendStringInfoString(o, "EXCEPTION WHEN OTHERS THEN\n");
		appendStringInfoString(o, ensure_body);
		indent(o, ind + 1);
		appendStringInfoString(o, "RAISE;\n");
		indent(o, ind);
		appendStringInfoString(o, "END;\n");
	}
}

/* raise / raise Class("msg") */
static void
emit_py_raise(Ctx *cx, int a, int b, int ind)
{
	int			call = -1, i;

	if (a + 1 >= b)				/* bare raise (re-raise) */
	{
		if (cx->handlerdepth == 0)
			plx_err(cx, cx->t[a].line, "bare 'raise' is only valid inside an except handler");
		indent(&cx->out, ind);
		appendStringInfoString(&cx->out, "RAISE;\n");
		return;
	}
	/* raise(msg) / raise("level", msg): the call form emits a leveled RAISE */
	if (cx->t[a + 1].kind == T_LPAREN)
	{
		emit_raise_call(cx, a, ind);
		return;
	}
	for (i = a + 1; i < b; i++)
		if (cx->t[i].kind == T_LPAREN)
		{
			call = i - 1;
			break;
		}
	if (call < 0)
		plx_err(cx, cx->t[a].line, "raise requires an exception, e.g. raise ValueError(\"msg\")");
	{
		int			sa[8], se[8], after, n;

		n = parse_args(cx, call, sa, se, 8, &after);
		indent(&cx->out, ind);
		if (n >= 1)
			appendStringInfo(&cx->out, "RAISE EXCEPTION '%%', %s;\n",
							 rw_range(cx, sa[0], se[0], false));
		else
			appendStringInfoString(&cx->out, "RAISE EXCEPTION 'exception';\n");
	}
}

static void
parse_py_stmt_inner(Ctx *cx, int ind, bool toplevel)
{
	Tok		   *tk;

	skip_seps(cx);
	tk = &cx->t[cx->pos];
	if (tk->kind == T_EOF || tk->kind == T_DEDENT)
		return;
	/* loop label:  name: for ... / name: while ... */
	if (tk->kind == T_IDENT && tok_is(&cx->t[cx->pos + 1], ":") &&
		cx->t[cx->pos + 2].kind == T_KW &&
		(cx->t[cx->pos + 2].kw == KW_FOR || cx->t[cx->pos + 2].kw == KW_WHILE))
	{
		indent(&cx->out, ind);
		appendStringInfo(&cx->out, "<<%.*s>>\n", tk->len, tk->s);
		cx->pos += 2;
		tk = &cx->t[cx->pos];
	}
	if (tk->kind == T_KW)
	{
		switch (tk->kw)
		{
			case KW_IF:
				parse_py_if(cx, ind);
				return;
			case KW_WHILE:
				parse_py_while(cx, ind);
				return;
			case KW_FOR:
				parse_py_for(cx, ind);
				return;
			case KW_BEGIN:		/* try */
				parse_py_try(cx, ind);
				return;
			case KW_PASS:
				cx->pos = stmt_end(cx, cx->pos);
				return;
			case KW_DEF:
				plx_err(cx, tk->line, "def is not supported");
				return;
			case KW_CASE:
				plx_err(cx, tk->line, "match/case is not supported (use if/elif)");
				return;
			default:
				break;
		}
	}
	/* leaf statement (ends at NEWLINE) */
	{
		int			a = cx->pos;
		int			b = stmt_end(cx, a);
		int			i, depth = 0;

		/* a top-level if/else inside a leaf is a conditional expression
		 * (a if c else b), which has no plpgsql lowering */
		for (i = a; i < b; i++)
		{
			TokKind		k = cx->t[i].kind;

			if (k == T_LPAREN || k == T_LBRACKET || k == T_LBRACE)
				depth++;
			else if (k == T_RPAREN || k == T_RBRACKET || k == T_RBRACE)
				depth--;
			else if (depth == 0 && cx->t[i].kind == T_KW &&
					 (cx->t[i].kw == KW_IF || cx->t[i].kw == KW_ELSE))
				plx_err(cx, cx->t[i].line,
						"the conditional expression \"a if c else b\" is not supported; use an if statement");
		}

		if (tk->kind == T_KW && tk->kw == KW_RAISE)
			emit_py_raise(cx, a, b, ind);
		else if (tk->kind == T_IDENT && name_eq(tk, "assert") &&
				 !(cx->t[a + 1].kind == T_LPAREN))
		{
			/* Python statement form: assert cond[, msg] */
			int			ai, adepth = 0, comma = -1;

			for (ai = a + 1; ai < b; ai++)
			{
				if (cx->t[ai].kind == T_LPAREN || cx->t[ai].kind == T_LBRACKET)
					adepth++;
				else if (cx->t[ai].kind == T_RPAREN || cx->t[ai].kind == T_RBRACKET)
					adepth--;
				else if (adepth == 0 && cx->t[ai].kind == T_COMMA)
				{
					comma = ai;
					break;
				}
			}
			indent(&cx->out, ind);
			if (comma >= 0)
				appendStringInfo(&cx->out, "ASSERT %s, %s;\n",
								 rw_range(cx, a + 1, comma, true), rw_range(cx, comma + 1, b, false));
			else
				appendStringInfo(&cx->out, "ASSERT %s;\n", rw_range(cx, a + 1, b, true));
		}
		else
			emit_core(cx, a, b, ind, toplevel);
		cx->pos = b;
	}
}

static void
parse_py_program(Ctx *cx)
{
	for (;;)
	{
		skip_seps(cx);
		if (cx->t[cx->pos].kind == T_EOF)
			break;
		if (cx->t[cx->pos].kind == T_INDENT || cx->t[cx->pos].kind == T_DEDENT)
		{
			cx->pos++;			/* stray indentation token at top level */
			continue;
		}
		parse_py_stmt(cx, 1, true);
	}
}

/* ======================================================================== */
/* COBOL front end (plxcobol, ISO/IEC 1989:2023).                           */
/*                                                                          */
/* COBOL is verb-driven and free-format. It does not fit the shared byte    */
/* lexer's expression model (hyphenated words, period sentence terminators, */
/* PICTURE clauses), so this is a self-contained tokenizer + recursive-     */
/* descent parser that emits plpgsql directly into cx->out and declarations */
/* into cx->locals. Compound statements use explicit COBOL 2023 scope       */
/* terminators (END-IF, END-PERFORM, END-EVALUATE). Data names are mapped   */
/* to plpgsql identifiers: lower-cased, hyphens become underscores.         */
/* ======================================================================== */

typedef enum
{
	CB_EOF, CB_WORD, CB_NUM, CB_STR, CB_PERIOD, CB_LP, CB_RP, CB_OP
} CbKind;

typedef struct
{
	CbKind		kind;
	const char *s;
	int			len;
	int			line;
	bool		sp;				/* whitespace preceded this token */
} CbTok;

typedef struct
{
	Ctx		   *cx;
	CbTok	   *t;
	int			nt, pos;
} Cb;

/* case-insensitive: is token tk exactly the reserved word w? */
static bool
cob_ci(CbTok *tk, const char *w)
{
	int			l = (int) strlen(w);

	return tk->kind == CB_WORD && tk->len == l && pg_strncasecmp(tk->s, w, l) == 0;
}

/* map a COBOL data name to a plpgsql identifier: lower-case, '-' -> '_' */
static char *
cob_map(const char *s, int len)
{
	char	   *r = palloc(len + 1);
	int			i;

	for (i = 0; i < len; i++)
	{
		char		c = s[i];

		if (c == '-')
			c = '_';
		else if (c >= 'A' && c <= 'Z')
			c = c - 'A' + 'a';
		r[i] = c;
	}
	r[len] = '\0';
	return r;
}

static CbTok *
cob_cur(Cb *cb)
{
	return &cb->t[cb->pos];
}

pg_noreturn static void
cob_err(Cb *cb, const char *msg)
{
	plx_err(cb->cx, cb->t[cb->pos].line, "%s", msg);
}

static void
cob_lex(Cb *cb, const char *body)
{
	const char *p = body,
			   *end = body + strlen(body);
	int			line = 1,
				cap = 256,
				n = 0,
				paren = 0;
	CbTok	   *t = palloc(sizeof(CbTok) * cap);
	bool		sp = true;

#define CBPUSH(k, st, ln) do { \
		if (n >= cap) { cap *= 2; t = repalloc(t, sizeof(CbTok) * cap); } \
		t[n].kind = (k); t[n].s = (st); t[n].len = (ln); \
		t[n].line = line; t[n].sp = sp; n++; sp = false; \
	} while (0)

	while (p < end)
	{
		if (*p == '\n')
		{
			line++;
			p++;
			sp = true;
			continue;
		}
		if (*p == ' ' || *p == '\t' || *p == '\r' || *p == ';')
		{
			p++;
			sp = true;
			continue;
		}
		if (*p == ',')
		{
			/* COBOL uses ',' as an optional separator between operands; strip it
			 * at statement level, but keep it inside parentheses so multi-argument
			 * SQL function calls (mod(a, b)) survive into the expression. */
			if (paren > 0)
			{
				CBPUSH(CB_OP, p, 1);
				p++;
				continue;
			}
			p++;
			sp = true;
			continue;
		}
		if (p[0] == '*' && p + 1 < end && p[1] == '>')		/* *> line comment */
		{
			while (p < end && *p != '\n')
				p++;
			continue;
		}
		if (p[0] == '>' && p + 1 < end && p[1] == '>')		/* >> directive line */
		{
			while (p < end && *p != '\n')
				p++;
			continue;
		}
		if (*p == '"' || *p == '\'')				/* string literal */
		{
			char		q = *p;
			const char *st = p;
			bool		closed = false;

			p++;
			while (p < end)
			{
				if (*p == q)
				{
					if (p + 1 < end && p[1] == q)	/* doubled quote */
					{
						p += 2;
						continue;
					}
					p++;
					closed = true;
					break;
				}
				if (*p == '\n')
					line++;
				p++;
			}
			if (!closed)
				plx_err(cb->cx, line, "unterminated string literal");
			CBPUSH(CB_STR, st, (int) (p - st));
			continue;
		}
		if (*p >= '0' && *p <= '9')					/* numeric literal / level */
		{
			const char *st = p;

			while (p < end && *p >= '0' && *p <= '9')
				p++;
			if (p + 1 < end && *p == '.' && p[1] >= '0' && p[1] <= '9')
			{
				p++;
				while (p < end && *p >= '0' && *p <= '9')
					p++;
			}
			CBPUSH(CB_NUM, st, (int) (p - st));
			continue;
		}
		if ((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') || *p == '_')
		{											/* COBOL word (may contain '-', '.') */
			const char *st = p;

			while (p < end)
			{
				char		c = *p;

				if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
					(c >= '0' && c <= '9') || c == '_' || c == '-')
				{
					p++;
					continue;
				}
				/* '.' inside a word (qualified name row.field), not a period */
				if (c == '.' && p + 1 < end &&
					(((p[1] >= 'A' && p[1] <= 'Z') || (p[1] >= 'a' && p[1] <= 'z') ||
					  (p[1] >= '0' && p[1] <= '9') || p[1] == '_')))
				{
					p++;
					continue;
				}
				break;
			}
			while (p > st && p[-1] == '-')			/* trailing '-' is not part */
				p--;
			CBPUSH(CB_WORD, st, (int) (p - st));
			continue;
		}
		if (*p == '.')
		{
			CBPUSH(CB_PERIOD, p, 1);
			p++;
			continue;
		}
		if (*p == '(')
		{
			CBPUSH(CB_LP, p, 1);
			p++;
			paren++;
			continue;
		}
		if (*p == ')')
		{
			CBPUSH(CB_RP, p, 1);
			if (paren > 0)
				paren--;
			p++;
			continue;
		}
		if (p[0] == '*' && p + 1 < end && p[1] == '*')		/* ** exponent */
		{
			CBPUSH(CB_OP, p, 2);
			p += 2;
			continue;
		}
		if (p + 1 < end && ((p[0] == '<' && p[1] == '>') ||
							(p[0] == '<' && p[1] == '=') ||
							(p[0] == '>' && p[1] == '=')))
		{
			CBPUSH(CB_OP, p, 2);
			p += 2;
			continue;
		}
		if (*p == '+' || *p == '-' || *p == '*' || *p == '/' || *p == '=' ||
			*p == '<' || *p == '>' || *p == ':' || *p == '%')
		{
			CBPUSH(CB_OP, p, 1);
			p++;
			continue;
		}
		p++;										/* skip anything else */
	}
	CBPUSH(CB_EOF, p, 0);
	cb->t = t;
	cb->nt = n;
	cb->pos = 0;
#undef CBPUSH
}

/* the COBOL statement verbs (a value expression never contains one) */
static bool
cob_is_verb(CbTok *tk)
{
	static const char *v[] = {
		"MOVE", "COMPUTE", "ADD", "SUBTRACT", "MULTIPLY", "DIVIDE", "IF",
		"EVALUATE", "PERFORM", "DISPLAY", "RAISE", "ASSERT", "GOBACK", "RETURN",
		"CONTINUE", "EXIT", "CALL", "EXECUTE", "COMMIT", "ROLLBACK",
		"RETURN-NEXT", "RETURN-QUERY", "GET", "BEGIN-TRY",
		"OPEN-CURSOR", "FETCH-CURSOR", "CLOSE-CURSOR", "MOVE-CURSOR",
		"STRING-APPEND", NULL
	};
	int			i;

	if (tk->kind != CB_WORD)
		return false;
	for (i = 0; v[i]; i++)
		if (cob_ci(tk, v[i]))
			return true;
	return false;
}

/* words that close a block or clause */
static bool
cob_is_close(CbTok *tk)
{
	static const char *c[] = {
		"ELSE", "WHEN", "END-IF", "END-PERFORM", "END-EVALUATE", "END-EXEC",
		"END-TRY", NULL
	};
	int			i;

	if (tk->kind != CB_WORD)
		return false;
	for (i = 0; c[i]; i++)
		if (cob_ci(tk, c[i]))
			return true;
	return false;
}

/* emit a COBOL string literal token as a SQL single-quoted literal */
static void
cob_emit_str(CbTok *tk, StringInfo out)
{
	char		q = tk->s[0];
	const char *s = tk->s + 1,
			   *e = tk->s + tk->len - 1;

	appendStringInfoChar(out, '\'');
	while (s < e)
	{
		if (*s == q && s + 1 < e && s[1] == q)		/* COBOL doubled quote */
		{
			if (q == '\'')
				appendStringInfoString(out, "''");
			else
				appendStringInfoChar(out, '"');
			s += 2;
			continue;
		}
		if (*s == '\'')								/* escape for SQL */
			appendStringInfoString(out, "''");
		else
			appendStringInfoChar(out, *s);
		s++;
	}
	appendStringInfoChar(out, '\'');
}

/* raw contents of a COBOL string literal (quotes stripped, doubles collapsed),
 * for inlining as SQL text in a FOR ... IN <sql> loop or RETURN QUERY */
static char *
cob_str_contents(CbTok *tk)
{
	StringInfoData s;
	char		q = tk->s[0];
	const char *p = tk->s + 1,
			   *e = tk->s + tk->len - 1;

	initStringInfo(&s);
	while (p < e)
	{
		if (*p == q && p + 1 < e && p[1] == q)
		{
			appendStringInfoChar(&s, q);
			p += 2;
			continue;
		}
		appendStringInfoChar(&s, *p);
		p++;
	}
	return s.data;
}

/* a figurative constant -> SQL text, or NULL if not one */
static const char *
cob_figurative(CbTok *tk)
{
	if (cob_ci(tk, "ZERO") || cob_ci(tk, "ZEROS") || cob_ci(tk, "ZEROES"))
		return "0";
	if (cob_ci(tk, "SPACE") || cob_ci(tk, "SPACES"))
		return "''";
	if (cob_ci(tk, "NULL") || cob_ci(tk, "NULLS"))
		return "NULL";
	if (cob_ci(tk, "TRUE"))
		return "true";
	if (cob_ci(tk, "FALSE"))
		return "false";
	return NULL;
}

/* append with a leading space when needed */
static void
cob_sp(StringInfo out)
{
	if (out->len && out->data[out->len - 1] != ' ' && out->data[out->len - 1] != '(')
		appendStringInfoChar(out, ' ');
}

/* index of the ')' matching the '(' at index lp, within [.,limit), or -1 */
static int
cob_match_lp(Cb *cb, int lp, int limit)
{
	int			depth = 0,
				i;

	for (i = lp; i < limit; i++)
	{
		if (cb->t[i].kind == CB_LP)
			depth++;
		else if (cb->t[i].kind == CB_RP)
		{
			depth--;
			if (depth == 0)
				return i;
		}
		else if (cb->t[i].kind == CB_EOF)
			break;
	}
	return -1;
}

/* is `name` a declared array local (its plpgsql type ends with "[]")? */
static bool
cob_is_array(Cb *cb, const char *name)
{
	PlxLocal2  *l = local_find(cb->cx, name, strlen(name));
	int			tl;

	if (!l || !l->typ)
		return false;
	tl = (int) strlen(l->typ);
	return tl >= 2 && l->typ[tl - 2] == '[' && l->typ[tl - 1] == ']';
}

/*
 * Emit tokens [a,b) as a SQL expression / condition. Handles COBOL relational
 * words (EQUAL TO, GREATER THAN [OR EQUAL TO], ...), NOT combinations, the IS
 * noise word, figurative constants, ** -> ^, data-name mapping, and array
 * subscripts (WS-ARR(I) -> ws_arr[i]).
 */
static void
cob_emit_range(Cb *cb, int a, int b, StringInfo out)
{
	CbTok	   *t = cb->t;
	int			i = a;

	while (i < b)
	{
		CbTok	   *tk = &t[i];
		const char *fig;

		if (tk->kind == CB_WORD)
		{
			if (cob_ci(tk, "IS"))
			{
				/* keep "IS" before NULL / NOT NULL; else it is noise */
				CbTok	   *nx = (i + 1 < b) ? &t[i + 1] : NULL;

				if (nx && (cob_ci(nx, "NULL") || cob_ci(nx, "NULLS") ||
						   (cob_ci(nx, "NOT") && i + 2 < b &&
							(cob_ci(&t[i + 2], "NULL") || cob_ci(&t[i + 2], "NULLS")))))
				{
					cob_sp(out);
					appendStringInfoString(out, "IS");
				}
				i++;
				continue;
			}
			if (cob_ci(tk, "AND") || cob_ci(tk, "OR"))
			{
				cob_sp(out);
				appendStringInfo(out, "%s", cob_ci(tk, "AND") ? "AND" : "OR");
				i++;
				continue;
			}
			if (cob_ci(tk, "NOT"))
			{
				CbTok	   *nx = (i + 1 < b) ? &t[i + 1] : NULL;

				if (nx && (cob_ci(nx, "EQUAL") || cob_ci(nx, "EQUALS") ||
						   (nx->kind == CB_OP && nx->len == 1 && nx->s[0] == '=')))
				{
					cob_sp(out);
					appendStringInfoString(out, "<>");
					i += 2;
					if (i < b && cob_ci(&t[i], "TO"))
						i++;
					continue;
				}
				if (nx && (cob_ci(nx, "GREATER") ||
						   (nx->kind == CB_OP && nx->len == 1 && nx->s[0] == '>')))
				{
					cob_sp(out);
					appendStringInfoString(out, "<=");
					i += 2;
					if (i < b && cob_ci(&t[i], "THAN"))
						i++;
					continue;
				}
				if (nx && (cob_ci(nx, "LESS") ||
						   (nx->kind == CB_OP && nx->len == 1 && nx->s[0] == '<')))
				{
					cob_sp(out);
					appendStringInfoString(out, ">=");
					i += 2;
					if (i < b && cob_ci(&t[i], "THAN"))
						i++;
					continue;
				}
				cob_sp(out);
				appendStringInfoString(out, "NOT");
				i++;
				continue;
			}
			if (cob_ci(tk, "EQUAL") || cob_ci(tk, "EQUALS"))
			{
				cob_sp(out);
				appendStringInfoChar(out, '=');
				i++;
				if (i < b && cob_ci(&t[i], "TO"))
					i++;
				continue;
			}
			if (cob_ci(tk, "GREATER") || cob_ci(tk, "LESS"))
			{
				bool		gt = cob_ci(tk, "GREATER");

				i++;
				if (i < b && cob_ci(&t[i], "THAN"))
					i++;
				if (i + 1 < b && cob_ci(&t[i], "OR") &&
					cob_ci(&t[i + 1], "EQUAL"))
				{
					i += 2;
					if (i < b && cob_ci(&t[i], "TO"))
						i++;
					cob_sp(out);
					appendStringInfoString(out, gt ? ">=" : "<=");
				}
				else
				{
					cob_sp(out);
					appendStringInfoChar(out, gt ? '>' : '<');
				}
				continue;
			}
			fig = cob_figurative(tk);
			if (fig)
			{
				cob_sp(out);
				appendStringInfoString(out, fig);
				i++;
				continue;
			}
			/* ordinary data name, or an array subscript WS-ARR(I) -> ws_arr[i] */
			cob_sp(out);
			{
				char	   *nm = cob_map(tk->s, tk->len);

				if (i + 1 < b && t[i + 1].kind == CB_LP && cob_is_array(cb, nm))
				{
					int			close = cob_match_lp(cb, i + 1, b);

					if (close > 0 && close < b)
					{
						appendStringInfoString(out, nm);
						appendStringInfoChar(out, '[');
						cob_emit_range(cb, i + 2, close, out);
						appendStringInfoChar(out, ']');
						i = close + 1;
						continue;
					}
				}
				appendStringInfoString(out, nm);
			}
			i++;
			continue;
		}
		if (tk->kind == CB_NUM)
		{
			cob_sp(out);
			appendBinaryStringInfo(out, tk->s, tk->len);
			i++;
			continue;
		}
		if (tk->kind == CB_STR)
		{
			cob_sp(out);
			cob_emit_str(tk, out);
			i++;
			continue;
		}
		if (tk->kind == CB_LP)
		{
			cob_sp(out);
			appendStringInfoChar(out, '(');
			i++;
			continue;
		}
		if (tk->kind == CB_RP)
		{
			appendStringInfoChar(out, ')');
			i++;
			continue;
		}
		if (tk->kind == CB_OP)
		{
			cob_sp(out);
			if (tk->len == 2 && tk->s[0] == '*' && tk->s[1] == '*')
				appendStringInfoChar(out, '^');
			else
				appendBinaryStringInfo(out, tk->s, tk->len);
			i++;
			continue;
		}
		i++;
	}
}

/* true if the current token stops a value scan */
static bool
cob_value_stop(Cb *cb, const char *const *stops, int nstops)
{
	CbTok	   *tk = cob_cur(cb);
	int			i;

	if (tk->kind == CB_EOF || tk->kind == CB_PERIOD)
		return true;
	if (cob_is_close(tk) || cob_is_verb(tk))
		return true;
	for (i = 0; i < nstops; i++)
		if (cob_ci(tk, stops[i]))
			return true;
	return false;
}

/* scan and emit a value/condition until a stop token; returns false if empty */
static bool
cob_value(Cb *cb, StringInfo out, const char *const *stops, int nstops)
{
	int			start = cb->pos;

	while (!cob_value_stop(cb, stops, nstops))
		cb->pos++;
	if (cb->pos == start)
		return false;
	cob_emit_range(cb, start, cb->pos, out);
	return true;
}

/* emit a single elementary operand (identifier / number / figurative), with
 * optional leading sign; returns palloc'd SQL text, or NULL if none */
static char *
cob_operand(Cb *cb)
{
	StringInfoData s;
	CbTok	   *tk = cob_cur(cb);
	bool		neg = false;

	if (tk->kind == CB_OP && tk->len == 1 && (tk->s[0] == '-' || tk->s[0] == '+'))
	{
		neg = (tk->s[0] == '-');
		cb->pos++;
		tk = cob_cur(cb);
	}
	initStringInfo(&s);
	if (tk->kind == CB_NUM)
		appendBinaryStringInfo(&s, tk->s, tk->len);
	else if (tk->kind == CB_STR)
		cob_emit_str(tk, &s);
	else if (tk->kind == CB_WORD)
	{
		const char *fig = cob_figurative(tk);

		if (fig)
			appendStringInfoString(&s, fig);
		else
			appendStringInfoString(&s, cob_map(tk->s, tk->len));
	}
	else
		return NULL;
	cb->pos++;
	if (neg)
	{
		StringInfoData r;

		initStringInfo(&r);
		appendStringInfo(&r, "(- %s)", s.data);
		return r.data;
	}
	return s.data;
}

/* --------- PICTURE / declaration handling --------- */

static int
cob_pic_repeat(const char *p, int len, int *ip)
{
	int			i = *ip,
				n = 0;

	if (i < len && p[i] == '(')
	{
		i++;
		while (i < len && p[i] >= '0' && p[i] <= '9')
		{
			n = n * 10 + (p[i++] - '0');
			if (n > 1000000)			/* clamp: no real type is this wide */
				n = 1000000;
		}
		if (i < len && p[i] == ')')
			i++;
		*ip = i;
		return n;
	}
	return 1;
}

/* map a PICTURE string (+ optional USAGE word) to a plpgsql type */
static char *
cob_type_from_pic(Cb *cb, const char *pic, int len, const char *usage, int ulen)
{
	int			i = 0,
				intdig = 0,
				scale = 0,
				strlen_ = 0;
	bool		isstr = false,
				sawv = false;
	char		buf[64];

	while (i < len)
	{
		char		c = pic[i];

		if (c >= 'a' && c <= 'z')
			c = c - 'a' + 'A';
		if (c == 'S')
		{
			i++;
			continue;
		}
		if (c == 'V' || c == '.')
		{
			sawv = true;
			i++;
			continue;
		}
		if (c == 'X' || c == 'A')
		{
			int			r;

			isstr = true;
			i++;
			r = cob_pic_repeat(pic, len, &i);
			strlen_ += r;
			if (strlen_ > 1000000)			/* clamp the running total, not just each group */
				strlen_ = 1000000;
			continue;
		}
		if (c == '9')
		{
			int			r;

			i++;
			r = cob_pic_repeat(pic, len, &i);
			if (sawv)
				scale += r;
			else
				intdig += r;
			if (intdig > 1000000)
				intdig = 1000000;
			if (scale > 1000000)
				scale = 1000000;
			continue;
		}
		i++;									/* Z, comma, etc.: ignore */
	}

	if (usage)
	{
		if ((ulen == 6 && pg_strncasecmp(usage, "COMP-1", 6) == 0))
			return pstrdup("real");
		if ((ulen == 6 && pg_strncasecmp(usage, "COMP-2", 6) == 0))
			return pstrdup("double precision");
	}

	if (isstr)
	{
		if (strlen_ <= 0)
			strlen_ = 1;
		snprintf(buf, sizeof(buf), "varchar(%d)", strlen_);
		return pstrdup(buf);
	}
	if (sawv || scale > 0)
	{
		int			prec = intdig + scale;

		if (prec <= 0)
			prec = scale + 1;
		snprintf(buf, sizeof(buf), "numeric(%d,%d)", prec, scale);
		return pstrdup(buf);
	}
	if (intdig == 0)
		cob_err(cb, "PICTURE clause has no digits or characters");
	if (intdig <= 9)
		return pstrdup("integer");
	if (intdig <= 18)
		return pstrdup("bigint");
	snprintf(buf, sizeof(buf), "numeric(%d)", intdig);
	return pstrdup(buf);
}

/* parse one WORKING-STORAGE level entry (up to its terminating period) */
static void
cob_decl(Cb *cb)
{
	CbTok	   *nm;
	PlxLocal2  *l;
	char	   *mapped;
	const char *usage = NULL;
	int			ulen = 0;
	bool		is_array = false;

	/* level number */
	if (cob_cur(cb)->kind != CB_NUM)
		cob_err(cb, "expected a level number in WORKING-STORAGE");
	cb->pos++;
	/* data name */
	nm = cob_cur(cb);
	if (nm->kind != CB_WORD)
		cob_err(cb, "expected a data name after the level number");
	mapped = cob_map(nm->s, nm->len);
	cb->pos++;

	if (local_find(cb->cx, mapped, strlen(mapped)))
		cob_err(cb, "duplicate data name in WORKING-STORAGE");
	l = local_add(cb->cx, mapped, strlen(mapped));

	while (cob_cur(cb)->kind != CB_PERIOD && cob_cur(cb)->kind != CB_EOF)
	{
		CbTok	   *tk = cob_cur(cb);

		if (cob_ci(tk, "PIC") || cob_ci(tk, "PICTURE"))
		{
			const char *rs, *re;

			cb->pos++;
			if (cob_ci(cob_cur(cb), "IS"))
				cb->pos++;
			if (cob_cur(cb)->kind == CB_EOF || cob_cur(cb)->kind == CB_PERIOD)
				cob_err(cb, "missing PICTURE string");
			rs = cob_cur(cb)->s;
			re = cob_cur(cb)->s + cob_cur(cb)->len;
			cb->pos++;
			while (!cob_cur(cb)->sp && cob_cur(cb)->kind != CB_EOF &&
				   cob_cur(cb)->kind != CB_PERIOD)
			{
				re = cob_cur(cb)->s + cob_cur(cb)->len;
				cb->pos++;
			}
			l->typ = cob_type_from_pic(cb, rs, (int) (re - rs), usage, ulen);
		}
		else if (cob_ci(tk, "USAGE"))
		{
			cb->pos++;
			if (cob_ci(cob_cur(cb), "IS"))
				cb->pos++;
			if (cob_cur(cb)->kind == CB_EOF || cob_cur(cb)->kind == CB_PERIOD)
				cob_err(cb, "USAGE requires a value");
			usage = cob_cur(cb)->s;
			ulen = cob_cur(cb)->len;
			cb->pos++;
			/* if PIC already set and usage is float, override */
			if (l->typ && ulen == 6 && pg_strncasecmp(usage, "COMP-1", 6) == 0)
				l->typ = pstrdup("real");
			else if (l->typ && ulen == 6 && pg_strncasecmp(usage, "COMP-2", 6) == 0)
				l->typ = pstrdup("double precision");
		}
		else if (cob_ci(tk, "VALUE"))
		{
			StringInfoData v;
			CbTok	   *vt;
			const char *fig;

			cb->pos++;
			if (cob_ci(cob_cur(cb), "IS"))
				cb->pos++;
			vt = cob_cur(cb);
			initStringInfo(&v);
			fig = (vt->kind == CB_WORD) ? cob_figurative(vt) : NULL;
			if (fig)
			{
				if (strcmp(fig, "''") != 0 || true)
					appendStringInfoString(&v, fig);
				cb->pos++;
				l->init = v.data;
			}
			else if (vt->kind == CB_NUM)
			{
				appendBinaryStringInfo(&v, vt->s, vt->len);
				cb->pos++;
				l->init = v.data;
			}
			else if (vt->kind == CB_STR)
			{
				cob_emit_str(vt, &v);
				cb->pos++;
				l->init = v.data;
			}
			else
				cob_err(cb, "unsupported VALUE literal");
		}
		else if (cob_ci(tk, "CONSTANT"))
		{
			StringInfoData v;
			CbTok	   *vt;

			cb->pos++;
			if (cob_ci(cob_cur(cb), "AS"))
				cb->pos++;
			vt = cob_cur(cb);
			initStringInfo(&v);
			cob_value(cb, &v, NULL, 0);
			l->init = v.data;
			l->is_const = true;
			if (!l->typ)
			{
				if (vt->kind == CB_STR)
					l->typ = pstrdup("text");
				else if (vt->kind == CB_NUM &&
						 memchr(vt->s, '.', vt->len) != NULL)
					l->typ = pstrdup("numeric");
				else
					l->typ = pstrdup("integer");
			}
		}
		else if (cob_ci(tk, "TYPE"))
		{
			const char *rs, *re;

			cb->pos++;
			rs = cob_cur(cb)->s;
			re = rs;
			while (cob_cur(cb)->kind != CB_PERIOD && cob_cur(cb)->kind != CB_EOF)
			{
				re = cob_cur(cb)->s + cob_cur(cb)->len;
				cb->pos++;
			}
			if (re == rs)
				cob_err(cb, "missing type after TYPE");
			{
				char	   *ty = pnstrdup(rs, (int) (re - rs));

				if (pg_strcasecmp(ty, "RECORD") == 0)
					l->is_record = true;
				else
					l->typ = ty;
			}
		}
		else if (cob_ci(tk, "OCCURS"))
		{
			/* OCCURS n [TIMES] -> the item is a PostgreSQL array of its element
			 * type (the fixed bound is not enforced) */
			cb->pos++;
			if (cob_cur(cb)->kind != CB_NUM)
				cob_err(cb, "OCCURS requires a count");
			cb->pos++;
			if (cob_ci(cob_cur(cb), "TIMES"))
				cb->pos++;
			is_array = true;
		}
		else
			cob_err(cb, "unexpected clause in a WORKING-STORAGE entry");
	}
	if (cob_cur(cb)->kind == CB_PERIOD)
		cb->pos++;

	if (is_array)
	{
		if (!l->typ)
			cob_err(cb, "an OCCURS item needs a PIC or TYPE for its element type");
		l->typ = psprintf("%s[]", l->typ);
	}
	if (!l->typ && !l->is_record)
		cob_err(cb, "data item needs a PIC, TYPE, or CONSTANT clause");
}

/* --------- statements --------- */

static void cob_block(Cb *cb, int ind);
static void cob_stmt(Cb *cb, int ind);

/* expect and consume a specific closing word */
static void
cob_expect(Cb *cb, const char *w)
{
	if (!cob_ci(cob_cur(cb), w))
		cob_err(cb, psprintf("expected %s", w));
	cb->pos++;
}

/* read a receiving field (a data name, or an array element WS-ARR(I)) as a
 * plpgsql lvalue string, consuming its tokens; NULL if the current token is not
 * a data name */
static char *
cob_target(Cb *cb)
{
	char	   *nm;

	if (cob_cur(cb)->kind != CB_WORD)
		return NULL;
	nm = cob_map(cob_cur(cb)->s, cob_cur(cb)->len);
	cb->pos++;
	if (cob_cur(cb)->kind == CB_LP && cob_is_array(cb, nm))
	{
		int			lp = cb->pos;
		int			close = cob_match_lp(cb, lp, cb->nt);

		if (close > lp)
		{
			StringInfoData s;

			initStringInfo(&s);
			appendStringInfo(&s, "%s[", nm);
			cob_emit_range(cb, lp + 1, close, &s);
			appendStringInfoChar(&s, ']');
			cb->pos = close + 1;
			return s.data;
		}
	}
	return nm;
}

static void
cob_move(Cb *cb, int ind)
{
	StringInfoData v;
	static const char *stops[] = {"TO"};

	cb->pos++;								/* MOVE */
	initStringInfo(&v);
	if (!cob_value(cb, &v, stops, 1))
		cob_err(cb, "MOVE requires a sending value");
	cob_expect(cb, "TO");
	if (cob_cur(cb)->kind != CB_WORD)
		cob_err(cb, "MOVE requires at least one receiving field");
	while (cob_cur(cb)->kind == CB_WORD && !cob_is_verb(cob_cur(cb)) &&
		   !cob_is_close(cob_cur(cb)))
	{
		char	   *tgt = cob_target(cb);

		indent(&cb->cx->out, ind);
		appendStringInfo(&cb->cx->out, "%s := %s;\n", tgt, v.data);
	}
}

static void
cob_compute(Cb *cb, int ind)
{
	char	   *tgts[16];
	int			ntg = 0;
	StringInfoData v;
	int			k;

	cb->pos++;								/* COMPUTE */
	while (cob_cur(cb)->kind == CB_WORD)
	{
		if (ntg >= 16)
			cob_err(cb, "too many COMPUTE receivers");
		tgts[ntg++] = cob_target(cb);
	}
	if (ntg == 0)
		cob_err(cb, "COMPUTE requires a receiving field");
	if (!(cob_cur(cb)->kind == CB_OP && cob_cur(cb)->len == 1 &&
		  cob_cur(cb)->s[0] == '='))
		cob_err(cb, "COMPUTE requires '='");
	cb->pos++;
	initStringInfo(&v);
	if (!cob_value(cb, &v, NULL, 0))
		cob_err(cb, "COMPUTE requires an expression");
	for (k = 0; k < ntg; k++)
	{
		indent(&cb->cx->out, ind);
		appendStringInfo(&cb->cx->out, "%s := (%s );\n", tgts[k], v.data);
	}
}

/*
 * ADD / SUBTRACT / MULTIPLY / DIVIDE.
 *   ADD a b ... TO recv [GIVING g]        recv|g := recv + (a+b+...)
 *   ADD a b ... GIVING g                  g := (a+b+...)
 *   SUBTRACT a b ... FROM recv [GIVING g] recv|g := recv - (a+b+...)
 *   MULTIPLY a BY recv [GIVING g]         recv|g := recv * a
 *   DIVIDE a INTO recv [GIVING g]         recv|g := recv / a
 *   DIVIDE a BY b GIVING g                g := a / b
 */
static void
cob_arith(Cb *cb, int ind)
{
	CbTok	   *verb = cob_cur(cb);
	char	   *recv,
			   *giv = NULL;

	cb->pos++;

	if (cob_ci(verb, "ADD") || cob_ci(verb, "SUBTRACT"))
	{
		bool		is_add = cob_ci(verb, "ADD");
		StringInfoData sum;
		int			n = 0;

		/* operand list until TO / FROM / GIVING (ADD/SUBTRACT take many) */
		initStringInfo(&sum);
		for (;;)
		{
			char	   *op;

			if (cob_ci(cob_cur(cb), "TO") || cob_ci(cob_cur(cb), "FROM") ||
				cob_ci(cob_cur(cb), "GIVING") || cob_value_stop(cb, NULL, 0))
				break;
			op = cob_operand(cb);
			if (!op)
				break;
			if (n++)
				appendStringInfoString(&sum, " + ");
			appendStringInfo(&sum, "%s", op);
		}
		if (n == 0)
			cob_err(cb, "arithmetic verb requires an operand");

		if (cob_ci(cob_cur(cb), is_add ? "TO" : "FROM"))
		{
			cb->pos++;
			recv = cob_operand(cb);
			if (!recv)
				cob_err(cb, "arithmetic verb requires a receiving field");
			if (cob_ci(cob_cur(cb), "GIVING"))
			{
				cb->pos++;
				giv = cob_operand(cb);
				if (!giv)
					cob_err(cb, "GIVING requires a receiving field");
			}
			indent(&cb->cx->out, ind);
			appendStringInfo(&cb->cx->out, "%s := (%s %c (%s));\n",
							 giv ? giv : recv, recv, is_add ? '+' : '-', sum.data);
		}
		else if (is_add && cob_ci(cob_cur(cb), "GIVING"))
		{
			cb->pos++;
			giv = cob_operand(cb);
			if (!giv)
				cob_err(cb, "GIVING requires a receiving field");
			indent(&cb->cx->out, ind);
			appendStringInfo(&cb->cx->out, "%s := (%s);\n", giv, sum.data);
		}
		else
			cob_err(cb, is_add ? "ADD requires TO or GIVING"
					: "SUBTRACT requires FROM");
		return;
	}

	/* MULTIPLY / DIVIDE take a single source operand */
	{
		char	   *src = cob_operand(cb);
		const char *op;

		if (!src)
			cob_err(cb, "arithmetic verb requires an operand");
		if (cob_ci(verb, "MULTIPLY"))
		{
			op = "*";
			cob_expect(cb, "BY");
		}
		else						/* DIVIDE */
		{
			if (cob_ci(cob_cur(cb), "INTO"))
				op = "/into";
			else if (cob_ci(cob_cur(cb), "BY"))
				op = "/by";
			else
				cob_err(cb, "DIVIDE requires INTO or BY");
			cb->pos++;
		}
		recv = cob_operand(cb);
		if (!recv)
			cob_err(cb, "arithmetic verb requires a receiving field");
		if (cob_ci(cob_cur(cb), "GIVING"))
		{
			cb->pos++;
			giv = cob_operand(cb);
			if (!giv)
				cob_err(cb, "GIVING requires a receiving field");
		}
		indent(&cb->cx->out, ind);
		if (strcmp(op, "*") == 0)
			appendStringInfo(&cb->cx->out, "%s := (%s * %s);\n",
							 giv ? giv : recv, recv, src);
		else if (strcmp(op, "/into") == 0)
			appendStringInfo(&cb->cx->out, "%s := (%s / %s);\n",
							 giv ? giv : recv, recv, src);
		else					/* /by : DIVIDE a BY b GIVING g -> g := a / b */
			appendStringInfo(&cb->cx->out, "%s := (%s / %s);\n",
							 giv ? giv : recv, src, recv);
	}
}

static void
cob_if(Cb *cb, int ind)
{
	StringInfoData c;
	static const char *stops[] = {"THEN"};

	cb->pos++;								/* IF */
	initStringInfo(&c);
	if (!cob_value(cb, &c, stops, 1))
		cob_err(cb, "IF requires a condition");
	if (cob_ci(cob_cur(cb), "THEN"))
		cb->pos++;
	indent(&cb->cx->out, ind);
	appendStringInfo(&cb->cx->out, "IF %s THEN\n", c.data);
	cob_block(cb, ind + 1);
	if (cob_ci(cob_cur(cb), "ELSE"))
	{
		cb->pos++;
		indent(&cb->cx->out, ind);
		appendStringInfoString(&cb->cx->out, "ELSE\n");
		cob_block(cb, ind + 1);
	}
	cob_expect(cb, "END-IF");
	indent(&cb->cx->out, ind);
	appendStringInfoString(&cb->cx->out, "END IF;\n");
}

static void
cob_evaluate(Cb *cb, int ind)
{
	bool		searched;

	cb->pos++;								/* EVALUATE */
	searched = cob_ci(cob_cur(cb), "TRUE");
	if (searched)
	{
		cb->pos++;
		indent(&cb->cx->out, ind);
		appendStringInfoString(&cb->cx->out, "CASE\n");
	}
	else
	{
		StringInfoData s;
		static const char *stops[] = {"WHEN"};

		initStringInfo(&s);
		if (!cob_value(cb, &s, stops, 1))
			cob_err(cb, "EVALUATE requires a subject");
		indent(&cb->cx->out, ind);
		appendStringInfo(&cb->cx->out, "CASE %s\n", s.data);
	}

	while (cob_ci(cob_cur(cb), "WHEN"))
	{
		if (cob_ci(&cb->t[cb->pos + 1], "OTHER"))
		{
			cb->pos += 2;
			indent(&cb->cx->out, ind + 1);
			appendStringInfoString(&cb->cx->out, "ELSE\n");
			cob_block(cb, ind + 2);
			break;
		}
		if (searched)
		{
			StringInfoData c;

			cb->pos++;
			initStringInfo(&c);
			if (!cob_value(cb, &c, NULL, 0))
				cob_err(cb, "WHEN requires a condition");
			indent(&cb->cx->out, ind + 1);
			appendStringInfo(&cb->cx->out, "WHEN %s THEN\n", c.data);
			cob_block(cb, ind + 2);
		}
		else
		{
			StringInfoData vals;
			bool		first = true;

			initStringInfo(&vals);
			/* stacked WHEN v1 WHEN v2 ... share the following statements */
			while (cob_ci(cob_cur(cb), "WHEN") &&
				   !cob_ci(&cb->t[cb->pos + 1], "OTHER"))
			{
				StringInfoData one;

				cb->pos++;
				initStringInfo(&one);
				if (!cob_value(cb, &one, NULL, 0))
					cob_err(cb, "WHEN requires a value");
				if (!first)
					appendStringInfoString(&vals, ", ");
				appendStringInfo(&vals, "%s", one.data);
				first = false;
			}
			indent(&cb->cx->out, ind + 1);
			appendStringInfo(&cb->cx->out, "WHEN %s THEN\n", vals.data);
			cob_block(cb, ind + 2);
		}
	}
	cob_expect(cb, "END-EVALUATE");
	indent(&cb->cx->out, ind);
	appendStringInfoString(&cb->cx->out, "END CASE;\n");
}

/*
 * True if the UNTIL condition starting at `from` (the token after the control
 * variable's comparison operator) is a *simple* "var op bound", i.e. the bound
 * runs to the loop body with no logical connector or second comparison. A
 * compound UNTIL (v > n AND done = 1) must use the general WHILE, not the
 * integer-FOR fast path (which would otherwise fold the connector into the FOR
 * bound and emit wrong output).
 */
static bool
cob_until_simple(Cb *cb, int from)
{
	int			i,
				depth = 0;

	for (i = from; i < cb->nt; i++)
	{
		CbTok	   *t = &cb->t[i];

		if (t->kind == CB_LP)
			depth++;
		else if (t->kind == CB_RP)
			depth--;
		else if (t->kind == CB_PERIOD || t->kind == CB_EOF)
			break;
		else if (depth == 0 && (cob_is_verb(t) || cob_ci(t, "END-PERFORM")))
			break;					/* the loop body / terminator begins here */
		else if (depth == 0 &&
				 (cob_ci(t, "AND") || cob_ci(t, "OR") || cob_ci(t, "NOT") ||
				  cob_ci(t, "EQUAL") || cob_ci(t, "EQUALS") ||
				  cob_ci(t, "GREATER") || cob_ci(t, "LESS") ||
				  (t->kind == CB_OP && t->len == 1 &&
				   (t->s[0] == '<' || t->s[0] == '=' || t->s[0] == '>'))))
			return false;			/* a connector or second comparison */
	}
	return true;
}

static void
cob_perform(Cb *cb, int ind)
{
	cb->pos++;								/* PERFORM */

	if (cob_ci(cob_cur(cb), "UNTIL"))
	{
		StringInfoData c;

		cb->pos++;
		initStringInfo(&c);
		if (!cob_value(cb, &c, NULL, 0))
			cob_err(cb, "PERFORM UNTIL requires a condition");
		indent(&cb->cx->out, ind);
		appendStringInfo(&cb->cx->out, "WHILE NOT (%s ) LOOP\n", c.data);
		cob_block(cb, ind + 1);
		cob_expect(cb, "END-PERFORM");
		indent(&cb->cx->out, ind);
		appendStringInfoString(&cb->cx->out, "END LOOP;\n");
	}
	else if (cob_ci(cob_cur(cb), "VARYING"))
	{
		char	   *var;
		StringInfoData from,
					by,
					c;
		static const char *s_by[] = {"BY"};
		static const char *s_until[] = {"UNTIL"};

		cb->pos++;
		if (cob_cur(cb)->kind != CB_WORD)
			cob_err(cb, "PERFORM VARYING requires a control variable");
		var = cob_map(cob_cur(cb)->s, cob_cur(cb)->len);
		cb->pos++;
		cob_expect(cb, "FROM");
		initStringInfo(&from);
		cob_value(cb, &from, s_by, 1);
		cob_expect(cb, "BY");
		initStringInfo(&by);
		cob_value(cb, &by, s_until, 1);
		cob_expect(cb, "UNTIL");

		/*
		 * Fast path: PERFORM VARYING v FROM a BY 1 UNTIL v > b (or >= b) is an
		 * integer FOR loop, which plpgsql runs faster than a WHILE with a manual
		 * increment. Requires BY 1 and a condition "<var> > b" / "<var> >= b".
		 */
		{
			char	   *bytrim = by.data;
			CbTok	   *c0 = cob_cur(cb);
			/* c1 only valid when c0 is not EOF (EOF is the last token) */
			CbTok	   *c1 = (c0->kind != CB_EOF) ? &cb->t[cb->pos + 1] : c0;
			bool		gt = (c1->kind == CB_OP && c1->len == 1 && c1->s[0] == '>');
			bool		ge = (c1->kind == CB_OP && c1->len == 2 &&
							  c1->s[0] == '>' && c1->s[1] == '=');

			while (*bytrim == ' ')
				bytrim++;
			if (strcmp(bytrim, "1") == 0 && c0->kind == CB_WORD && (gt || ge) &&
				strcmp(cob_map(c0->s, c0->len), var) == 0 &&
				cob_until_simple(cb, cb->pos + 2))
			{
				StringInfoData bound;

				cb->pos += 2;			/* control variable and the operator */
				initStringInfo(&bound);
				cob_value(cb, &bound, NULL, 0);
				indent(&cb->cx->out, ind);
				if (gt)
					appendStringInfo(&cb->cx->out, "FOR %s IN (%s )..(%s ) LOOP\n",
									 var, from.data, bound.data);
				else
					appendStringInfo(&cb->cx->out, "FOR %s IN (%s )..((%s ) - 1) LOOP\n",
									 var, from.data, bound.data);
				cob_block(cb, ind + 1);
				cob_expect(cb, "END-PERFORM");
				indent(&cb->cx->out, ind);
				appendStringInfoString(&cb->cx->out, "END LOOP;\n");
				return;
			}
		}

		/* general PERFORM VARYING -> WHILE with a manual step */
		initStringInfo(&c);
		cob_value(cb, &c, NULL, 0);
		indent(&cb->cx->out, ind);
		appendStringInfo(&cb->cx->out, "%s := (%s );\n", var, from.data);
		indent(&cb->cx->out, ind);
		appendStringInfo(&cb->cx->out, "WHILE NOT (%s ) LOOP\n", c.data);
		cob_block(cb, ind + 1);
		indent(&cb->cx->out, ind + 1);
		appendStringInfo(&cb->cx->out, "%s := %s + (%s );\n", var, var, by.data);
		cob_expect(cb, "END-PERFORM");
		indent(&cb->cx->out, ind);
		appendStringInfoString(&cb->cx->out, "END LOOP;\n");
	}
	else if (cob_is_verb(cob_cur(cb)))
	{
		/* inline PERFORM ... END-PERFORM (no clause): unconditional loop */
		indent(&cb->cx->out, ind);
		appendStringInfoString(&cb->cx->out, "LOOP\n");
		cob_block(cb, ind + 1);
		cob_expect(cb, "END-PERFORM");
		indent(&cb->cx->out, ind);
		appendStringInfoString(&cb->cx->out, "END LOOP;\n");
	}
	else if (cob_cur(cb)->kind == CB_WORD && cob_ci(&cb->t[cb->pos + 1], "OVER"))
	{
		/* PERFORM <row> OVER "<sql>" [USING ...] ... END-PERFORM   (query loop)
		 * PERFORM <v>   OVER ARRAY <expr>       ... END-PERFORM   (array loop) */
		char	   *var = cob_map(cob_cur(cb)->s, cob_cur(cb)->len);

		cb->pos += 2;					/* <var> OVER */
		if (cob_ci(cob_cur(cb), "ARRAY"))
		{
			StringInfoData e;

			cb->pos++;
			initStringInfo(&e);
			if (!cob_value(cb, &e, NULL, 0))
				cob_err(cb, "PERFORM ... OVER ARRAY requires an array expression");
			indent(&cb->cx->out, ind);
			appendStringInfo(&cb->cx->out, "FOREACH %s IN ARRAY (%s ) LOOP\n", var, e.data);
		}
		else
		{
			PlxLocal2  *row;
			static const char *stops[] = {"USING"};

			row = local_find(cb->cx, var, strlen(var));
			if (!row)
			{
				row = local_add(cb->cx, var, strlen(var));
				row->is_record = true;
			}
			indent(&cb->cx->out, ind);
			if (cob_cur(cb)->kind == CB_STR &&
				!cob_ci(&cb->t[cb->pos + 1], "USING"))
			{
				/* literal SQL: inline it */
				char	   *sql = cob_str_contents(cob_cur(cb));

				cb->pos++;
				appendStringInfo(&cb->cx->out, "FOR %s IN %s LOOP\n", var, sql);
			}
			else
			{
				/* dynamic SQL / with binds: FOR ... IN EXECUTE <expr> [USING ...] */
				StringInfoData sql,
							using;
				int			nusing = 0;

				initStringInfo(&sql);
				cob_value(cb, &sql, stops, 1);
				initStringInfo(&using);
				if (cob_ci(cob_cur(cb), "USING"))
				{
					cb->pos++;
					while (!cob_value_stop(cb, NULL, 0))
					{
						char	   *op = cob_operand(cb);

						if (!op)
							break;
						if (nusing++)
							appendStringInfoString(&using, ", ");
						appendStringInfo(&using, "%s", op);
					}
				}
				appendStringInfo(&cb->cx->out, "FOR %s IN EXECUTE %s", var, sql.data);
				if (nusing)
					appendStringInfo(&cb->cx->out, " USING %s", using.data);
				appendStringInfoString(&cb->cx->out, " LOOP\n");
			}
		}
		cob_block(cb, ind + 1);
		cob_expect(cb, "END-PERFORM");
		indent(&cb->cx->out, ind);
		appendStringInfoString(&cb->cx->out, "END LOOP;\n");
	}
	else
	{
		/* PERFORM <count> TIMES */
		StringInfoData n;
		static const char *stops[] = {"TIMES"};
		int			id = cb->cx->subq++;

		initStringInfo(&n);
		if (!cob_value(cb, &n, stops, 1) || !cob_ci(cob_cur(cb), "TIMES"))
			cob_err(cb, "PERFORM of a paragraph is not supported; use inline PERFORM ... END-PERFORM");
		cb->pos++;						/* TIMES */
		indent(&cb->cx->out, ind);
		appendStringInfo(&cb->cx->out, "FOR __plx_cob_%d IN 1..(%s ) LOOP\n", id, n.data);
		cob_block(cb, ind + 1);
		cob_expect(cb, "END-PERFORM");
		indent(&cb->cx->out, ind);
		appendStringInfoString(&cb->cx->out, "END LOOP;\n");
	}
}

static void
cob_display(Cb *cb, int ind)
{
	StringInfoData args;
	int			nargs = 0;
	static const char *stops[] = {"UPON", "WITH"};

	cb->pos++;								/* DISPLAY */
	initStringInfo(&args);
	while (!cob_value_stop(cb, stops, 2))
	{
		char	   *op = cob_operand(cb);

		if (!op)
			break;
		if (nargs++)
			appendStringInfoString(&args, ", ");
		appendStringInfo(&args, "%s", op);
	}
	/* consume trailing UPON <dev> / WITH NO ADVANCING noise up to period */
	while (cob_cur(cb)->kind != CB_PERIOD && cob_cur(cb)->kind != CB_EOF &&
		   !cob_is_verb(cob_cur(cb)) && !cob_is_close(cob_cur(cb)))
		cb->pos++;
	if (nargs == 0)
		cob_err(cb, "DISPLAY requires at least one operand");
	indent(&cb->cx->out, ind);
	appendStringInfo(&cb->cx->out, "RAISE NOTICE '%%', concat(%s);\n", args.data);
}

static void
cob_raise(Cb *cb, int ind)
{
	CbTok	   *lvl;
	const char *level = "EXCEPTION";
	StringInfoData msg;
	char	   *code = NULL;
	static const char *stops[] = {"SQLSTATE"};

	cb->pos++;								/* RAISE */
	lvl = cob_cur(cb);
	if (cob_ci(lvl, "EXCEPTION") || cob_ci(lvl, "ERROR"))
	{
		level = "EXCEPTION";
		cb->pos++;
	}
	else if (cob_ci(lvl, "NOTICE"))
	{
		level = "NOTICE";
		cb->pos++;
	}
	else if (cob_ci(lvl, "WARNING"))
	{
		level = "WARNING";
		cb->pos++;
	}
	else if (cob_ci(lvl, "INFO"))
	{
		level = "INFO";
		cb->pos++;
	}
	else if (cob_ci(lvl, "LOG"))
	{
		level = "LOG";
		cb->pos++;
	}
	else if (cob_ci(lvl, "DEBUG"))
	{
		level = "DEBUG";
		cb->pos++;
	}
	initStringInfo(&msg);
	if (!cob_value(cb, &msg, stops, 1))
		cob_err(cb, "RAISE requires a message");
	if (cob_ci(cob_cur(cb), "SQLSTATE"))
	{
		cb->pos++;
		if (cob_cur(cb)->kind != CB_STR)
			cob_err(cb, "SQLSTATE requires a string code");
		{
			StringInfoData cs;

			initStringInfo(&cs);
			cob_emit_str(cob_cur(cb), &cs);
			code = cs.data;
			cb->pos++;
		}
	}
	indent(&cb->cx->out, ind);
	if (code)
		appendStringInfo(&cb->cx->out, "RAISE %s '%%',%s USING ERRCODE = %s;\n",
						 level, msg.data, code);
	else
		appendStringInfo(&cb->cx->out, "RAISE %s '%%',%s;\n", level, msg.data);
}

static void
cob_assert(Cb *cb, int ind)
{
	StringInfoData c;

	cb->pos++;								/* ASSERT */
	initStringInfo(&c);
	if (!cob_value(cb, &c, NULL, 0))
		cob_err(cb, "ASSERT requires a condition");
	indent(&cb->cx->out, ind);
	appendStringInfo(&cb->cx->out, "ASSERT %s;\n", c.data);
}

static void
cob_return(Cb *cb, int ind)
{
	bool		goback = cob_ci(cob_cur(cb), "GOBACK");
	StringInfoData v;

	cb->pos++;								/* GOBACK / RETURN */
	if (goback && cob_ci(cob_cur(cb), "RETURNING"))
		cb->pos++;
	initStringInfo(&v);
	if (cob_value(cb, &v, NULL, 0))
	{
		indent(&cb->cx->out, ind);
		appendStringInfo(&cb->cx->out, "RETURN %s;\n", v.data);
	}
	else
	{
		indent(&cb->cx->out, ind);
		appendStringInfoString(&cb->cx->out, "RETURN;\n");
	}
}

static void
cob_exit(Cb *cb, int ind)
{
	cb->pos++;								/* EXIT */
	if (!cob_ci(cob_cur(cb), "PERFORM"))
		cob_err(cb, "only EXIT PERFORM [CYCLE] is supported");
	cb->pos++;
	indent(&cb->cx->out, ind);
	if (cob_ci(cob_cur(cb), "CYCLE"))
	{
		cb->pos++;
		appendStringInfoString(&cb->cx->out, "CONTINUE;\n");
	}
	else
		appendStringInfoString(&cb->cx->out, "EXIT;\n");
}

static void
cob_call(Cb *cb, int ind)
{
	char	   *name;
	StringInfoData args;
	int			nargs = 0;

	cb->pos++;								/* CALL */
	if (cob_cur(cb)->kind == CB_STR)
	{
		CbTok	   *tk = cob_cur(cb);

		name = pnstrdup(tk->s + 1, tk->len - 2);
		cb->pos++;
	}
	else if (cob_cur(cb)->kind == CB_WORD)
	{
		name = cob_map(cob_cur(cb)->s, cob_cur(cb)->len);
		cb->pos++;
	}
	else
		cob_err(cb, "CALL requires a procedure name");
	initStringInfo(&args);
	if (cob_ci(cob_cur(cb), "USING"))
	{
		cb->pos++;
		while (!cob_value_stop(cb, NULL, 0))
		{
			char	   *op = cob_operand(cb);

			if (!op)
				break;
			if (nargs++)
				appendStringInfoString(&args, ", ");
			appendStringInfo(&args, "%s", op);
		}
	}
	indent(&cb->cx->out, ind);
	appendStringInfo(&cb->cx->out, "CALL %s(%s);\n", name, args.data);
}

static void
cob_execute(Cb *cb, int ind)
{
	StringInfoData sql,
				into,
				using;
	int			nusing = 0;
	static const char *stops[] = {"USING", "INTO"};

	cb->pos++;								/* EXECUTE */
	initStringInfo(&sql);
	if (!cob_value(cb, &sql, stops, 2))
		cob_err(cb, "EXECUTE requires a command string");
	initStringInfo(&into);
	initStringInfo(&using);
	/* INTO and USING may appear in either order */
	for (;;)
	{
		if (cob_ci(cob_cur(cb), "INTO"))
		{
			int			nt = 0;

			cb->pos++;
			while (cob_cur(cb)->kind == CB_WORD && !cob_is_verb(cob_cur(cb)) &&
				   !cob_is_close(cob_cur(cb)) && !cob_ci(cob_cur(cb), "USING"))
			{
				if (nt++)
					appendStringInfoString(&into, ", ");
				appendStringInfoString(&into, cob_map(cob_cur(cb)->s, cob_cur(cb)->len));
				cb->pos++;
			}
		}
		else if (cob_ci(cob_cur(cb), "USING"))
		{
			static const char *ustops[] = {"INTO"};

			cb->pos++;
			while (!cob_value_stop(cb, ustops, 1))
			{
				char	   *op = cob_operand(cb);

				if (!op)
					break;
				if (nusing++)
					appendStringInfoString(&using, ", ");
				appendStringInfo(&using, "%s", op);
			}
		}
		else
			break;
	}
	indent(&cb->cx->out, ind);
	appendStringInfo(&cb->cx->out, "EXECUTE %s", sql.data);
	if (into.len)
		appendStringInfo(&cb->cx->out, " INTO %s", into.data);
	if (nusing)
		appendStringInfo(&cb->cx->out, " USING %s", using.data);
	appendStringInfoString(&cb->cx->out, ";\n");
}

static void
cob_return_next(Cb *cb, int ind)
{
	StringInfoData v;

	cb->pos++;								/* RETURN-NEXT */
	initStringInfo(&v);
	if (!cob_value(cb, &v, NULL, 0))
		cob_err(cb, "RETURN-NEXT requires a value");
	indent(&cb->cx->out, ind);
	appendStringInfo(&cb->cx->out, "RETURN NEXT %s;\n", v.data);
}

static void
cob_return_query(Cb *cb, int ind)
{
	static const char *stops[] = {"USING"};

	cb->pos++;								/* RETURN-QUERY */
	indent(&cb->cx->out, ind);
	if (cob_cur(cb)->kind == CB_STR && !cob_ci(&cb->t[cb->pos + 1], "USING"))
	{
		char	   *sql = cob_str_contents(cob_cur(cb));

		cb->pos++;
		appendStringInfo(&cb->cx->out, "RETURN QUERY %s;\n", sql);
	}
	else
	{
		StringInfoData sql,
					using;
		int			nusing = 0;

		initStringInfo(&sql);
		if (!cob_value(cb, &sql, stops, 1))
			cob_err(cb, "RETURN-QUERY requires a command string");
		initStringInfo(&using);
		if (cob_ci(cob_cur(cb), "USING"))
		{
			cb->pos++;
			while (!cob_value_stop(cb, NULL, 0))
			{
				char	   *op = cob_operand(cb);

				if (!op)
					break;
				if (nusing++)
					appendStringInfoString(&using, ", ");
				appendStringInfo(&using, "%s", op);
			}
		}
		appendStringInfo(&cb->cx->out, "RETURN QUERY EXECUTE %s", sql.data);
		if (nusing)
			appendStringInfo(&cb->cx->out, " USING %s", using.data);
		appendStringInfoString(&cb->cx->out, ";\n");
	}
}

static void
cob_get(Cb *cb, int ind)
{
	char	   *var;
	const char *field;
	bool		stacked = false;
	CbTok	   *f;

	cb->pos++;								/* GET */
	f = cob_cur(cb);
	if (cob_ci(f, "ROW-COUNT"))
		field = "ROW_COUNT";
	else if (cob_ci(f, "MESSAGE"))
	{
		field = "MESSAGE_TEXT";
		stacked = true;
	}
	else if (cob_ci(f, "DETAIL"))
	{
		field = "PG_EXCEPTION_DETAIL";
		stacked = true;
	}
	else if (cob_ci(f, "HINT"))
	{
		field = "PG_EXCEPTION_HINT";
		stacked = true;
	}
	else if (cob_ci(f, "SQLSTATE"))
	{
		field = "RETURNED_SQLSTATE";
		stacked = true;
	}
	else if (cob_ci(f, "CONTEXT"))
	{
		field = "PG_EXCEPTION_CONTEXT";
		stacked = true;
	}
	else
		cob_err(cb, "GET expects ROW-COUNT, MESSAGE, DETAIL, HINT, SQLSTATE, or CONTEXT");
	cb->pos++;
	if (cob_ci(cob_cur(cb), "INTO"))
		cb->pos++;
	if (cob_cur(cb)->kind != CB_WORD)
		cob_err(cb, "GET requires a receiving field");
	var = cob_map(cob_cur(cb)->s, cob_cur(cb)->len);
	cb->pos++;
	indent(&cb->cx->out, ind);
	appendStringInfo(&cb->cx->out, "GET %sDIAGNOSTICS %s = %s;\n",
					 stacked ? "STACKED " : "", var, field);
}

/* BEGIN-TRY ... [WHEN <cond>|OTHER ...] END-TRY  ->  BEGIN..EXCEPTION..END */
static void
cob_try(Cb *cb, int ind)
{
	cb->pos++;								/* BEGIN-TRY */
	indent(&cb->cx->out, ind);
	appendStringInfoString(&cb->cx->out, "BEGIN\n");
	cob_block(cb, ind + 1);
	if (cob_ci(cob_cur(cb), "WHEN"))
	{
		indent(&cb->cx->out, ind);
		appendStringInfoString(&cb->cx->out, "EXCEPTION\n");
		while (cob_ci(cob_cur(cb), "WHEN"))
		{
			char	   *cond;

			cb->pos++;
			if (cob_ci(cob_cur(cb), "OTHER") || cob_ci(cob_cur(cb), "OTHERS"))
			{
				cond = "OTHERS";
				cb->pos++;
			}
			else if (cob_cur(cb)->kind == CB_WORD)
			{
				cond = cob_map(cob_cur(cb)->s, cob_cur(cb)->len);
				cb->pos++;
			}
			else
				cob_err(cb, "WHEN requires an exception condition name or OTHER");
			indent(&cb->cx->out, ind + 1);
			appendStringInfo(&cb->cx->out, "WHEN %s THEN\n", cond);
			cob_block(cb, ind + 2);
		}
	}
	cob_expect(cb, "END-TRY");
	indent(&cb->cx->out, ind);
	appendStringInfoString(&cb->cx->out, "END;\n");
}

/* declare a name as a local of a given kind if it is not a parameter */
static void
cob_declare_kind(Cb *cb, const char *name, const char *typ, bool is_record)
{
	PlxLocal2  *l;

	if (is_param(cb->cx, name, strlen(name)))
		return;
	l = local_find(cb->cx, name, strlen(name));
	if (!l)
		l = local_add(cb->cx, name, strlen(name));
	if (is_record)
		l->is_record = true;
	else if (!l->typ)
		l->typ = pstrdup(typ);
}

static void
cob_open_cursor(Cb *cb, int ind)
{
	char	   *c;

	cb->pos++;								/* OPEN-CURSOR */
	if (cob_cur(cb)->kind != CB_WORD)
		cob_err(cb, "OPEN-CURSOR requires a cursor name");
	c = cob_map(cob_cur(cb)->s, cob_cur(cb)->len);
	cb->pos++;
	cob_declare_kind(cb, c, "refcursor", false);
	cob_expect(cb, "FOR");
	indent(&cb->cx->out, ind);
	if (cob_cur(cb)->kind == CB_STR && !cob_ci(&cb->t[cb->pos + 1], "USING"))
	{
		char	   *sql = cob_str_contents(cob_cur(cb));

		cb->pos++;
		appendStringInfo(&cb->cx->out, "OPEN %s FOR %s;\n", c, sql);
	}
	else
	{
		StringInfoData sql,
					using;
		int			nusing = 0;
		static const char *stops[] = {"USING"};

		initStringInfo(&sql);
		cob_value(cb, &sql, stops, 1);
		initStringInfo(&using);
		if (cob_ci(cob_cur(cb), "USING"))
		{
			cb->pos++;
			while (!cob_value_stop(cb, NULL, 0))
			{
				char	   *op = cob_operand(cb);

				if (!op)
					break;
				if (nusing++)
					appendStringInfoString(&using, ", ");
				appendStringInfo(&using, "%s", op);
			}
		}
		appendStringInfo(&cb->cx->out, "OPEN %s FOR EXECUTE %s", c, sql.data);
		if (nusing)
			appendStringInfo(&cb->cx->out, " USING %s", using.data);
		appendStringInfoString(&cb->cx->out, ";\n");
	}
}

static void
cob_fetch_cursor(Cb *cb, int ind)
{
	char	   *c,
			   *row;

	cb->pos++;								/* FETCH-CURSOR */
	if (cob_cur(cb)->kind != CB_WORD)
		cob_err(cb, "FETCH-CURSOR requires a cursor name");
	c = cob_map(cob_cur(cb)->s, cob_cur(cb)->len);
	cb->pos++;
	cob_expect(cb, "INTO");
	if (cob_cur(cb)->kind != CB_WORD)
		cob_err(cb, "FETCH-CURSOR requires a receiving record");
	row = cob_map(cob_cur(cb)->s, cob_cur(cb)->len);
	cb->pos++;
	cob_declare_kind(cb, row, NULL, true);
	indent(&cb->cx->out, ind);
	appendStringInfo(&cb->cx->out, "FETCH FROM %s INTO %s;\n", c, row);
}

static void
cob_close_cursor(Cb *cb, int ind)
{
	char	   *c;

	cb->pos++;								/* CLOSE-CURSOR */
	if (cob_cur(cb)->kind != CB_WORD)
		cob_err(cb, "CLOSE-CURSOR requires a cursor name");
	c = cob_map(cob_cur(cb)->s, cob_cur(cb)->len);
	cb->pos++;
	indent(&cb->cx->out, ind);
	appendStringInfo(&cb->cx->out, "CLOSE %s;\n", c);
}

static void
cob_move_cursor(Cb *cb, int ind)
{
	char	   *c;
	char	   *n = NULL;

	cb->pos++;								/* MOVE-CURSOR */
	if (cob_cur(cb)->kind != CB_WORD)
		cob_err(cb, "MOVE-CURSOR requires a cursor name");
	c = cob_map(cob_cur(cb)->s, cob_cur(cb)->len);
	cb->pos++;
	if (!cob_value_stop(cb, NULL, 0))
		n = cob_operand(cb);
	indent(&cb->cx->out, ind);
	appendStringInfo(&cb->cx->out, "MOVE FORWARD %s FROM %s;\n", n ? n : "1", c);
}

/* STRING-APPEND <expr> TO <var>: lower to the plx_strbuild string builder
 * (amortized-O(1) append), the COBOL counterpart of the other dialects'
 * append operators. */
static void
cob_string_append(Cb *cb, int ind)
{
	StringInfoData v;
	char	   *tgt;
	PlxLocal2  *l;
	static const char *stops[] = {"TO"};

	cb->pos++;								/* STRING-APPEND */
	initStringInfo(&v);
	if (!cob_value(cb, &v, stops, 1))
		cob_err(cb, "STRING-APPEND requires a value");
	cob_expect(cb, "TO");
	if (cob_cur(cb)->kind != CB_WORD)
		cob_err(cb, "STRING-APPEND requires a receiving field");
	tgt = cob_map(cob_cur(cb)->s, cob_cur(cb)->len);
	cb->pos++;
	l = local_find(cb->cx, tgt, strlen(tgt));
	if (l && !l->is_record)
		l->typ = pstrdup("plx_strbuild");	/* the accumulator becomes a builder */
	indent(&cb->cx->out, ind);
	appendStringInfo(&cb->cx->out, "%s := plx_sb_append(%s, (%s )::text);\n",
					 tgt, tgt, v.data);
}

static void
cob_stmt(Cb *cb, int ind)
{
	CbTok	   *tk = cob_cur(cb);

	if (cob_ci(tk, "MOVE"))
		cob_move(cb, ind);
	else if (cob_ci(tk, "STRING-APPEND"))
		cob_string_append(cb, ind);
	else if (cob_ci(tk, "RETURN-NEXT"))
		cob_return_next(cb, ind);
	else if (cob_ci(tk, "RETURN-QUERY"))
		cob_return_query(cb, ind);
	else if (cob_ci(tk, "GET"))
		cob_get(cb, ind);
	else if (cob_ci(tk, "BEGIN-TRY"))
		cob_try(cb, ind);
	else if (cob_ci(tk, "OPEN-CURSOR"))
		cob_open_cursor(cb, ind);
	else if (cob_ci(tk, "FETCH-CURSOR"))
		cob_fetch_cursor(cb, ind);
	else if (cob_ci(tk, "CLOSE-CURSOR"))
		cob_close_cursor(cb, ind);
	else if (cob_ci(tk, "MOVE-CURSOR"))
		cob_move_cursor(cb, ind);
	else if (cob_ci(tk, "COMPUTE"))
		cob_compute(cb, ind);
	else if (cob_ci(tk, "ADD") || cob_ci(tk, "SUBTRACT") ||
			 cob_ci(tk, "MULTIPLY") || cob_ci(tk, "DIVIDE"))
		cob_arith(cb, ind);
	else if (cob_ci(tk, "IF"))
		cob_if(cb, ind);
	else if (cob_ci(tk, "EVALUATE"))
		cob_evaluate(cb, ind);
	else if (cob_ci(tk, "PERFORM"))
		cob_perform(cb, ind);
	else if (cob_ci(tk, "DISPLAY"))
		cob_display(cb, ind);
	else if (cob_ci(tk, "RAISE"))
		cob_raise(cb, ind);
	else if (cob_ci(tk, "ASSERT"))
		cob_assert(cb, ind);
	else if (cob_ci(tk, "GOBACK") || cob_ci(tk, "RETURN"))
		cob_return(cb, ind);
	else if (cob_ci(tk, "CONTINUE"))
	{
		cb->pos++;
		indent(&cb->cx->out, ind);
		appendStringInfoString(&cb->cx->out, "NULL;\n");
	}
	else if (cob_ci(tk, "EXIT"))
		cob_exit(cb, ind);
	else if (cob_ci(tk, "CALL"))
		cob_call(cb, ind);
	else if (cob_ci(tk, "EXECUTE"))
		cob_execute(cb, ind);
	else if (cob_ci(tk, "COMMIT"))
	{
		cb->pos++;
		indent(&cb->cx->out, ind);
		appendStringInfoString(&cb->cx->out, "COMMIT;\n");
	}
	else if (cob_ci(tk, "ROLLBACK"))
	{
		cb->pos++;
		indent(&cb->cx->out, ind);
		appendStringInfoString(&cb->cx->out, "ROLLBACK;\n");
	}
	else
		cob_err(cb, psprintf("unexpected COBOL statement \"%.*s\"", tk->len, tk->s));
}

/* parse a statement list until a close word / EOF; periods separate sentences */
static void
cob_block(Cb *cb, int ind)
{
	if (++cb->cx->depth > PLX_MAX_DEPTH)
		cob_err(cb, "statements nested too deeply");
	for (;;)
	{
		while (cob_cur(cb)->kind == CB_PERIOD)
			cb->pos++;
		if (cob_cur(cb)->kind == CB_EOF || cob_is_close(cob_cur(cb)))
			break;
		cob_stmt(cb, ind);
	}
	cb->cx->depth--;
}

static void
cobol_transpile(Ctx *cx)
{
	Cb			cb;

	memset(&cb, 0, sizeof(cb));
	cb.cx = cx;
	cob_lex(&cb, cx->body);

	while (cob_cur(&cb)->kind == CB_PERIOD)
		cb.pos++;

	/* optional DATA DIVISION. */
	if (cob_ci(cob_cur(&cb), "DATA") && cob_ci(&cb.t[cb.pos + 1], "DIVISION"))
	{
		cb.pos += 2;
		if (cob_cur(&cb)->kind == CB_PERIOD)
			cb.pos++;
	}
	/* optional WORKING-STORAGE SECTION. + level entries */
	if (cob_ci(cob_cur(&cb), "WORKING-STORAGE") &&
		cob_ci(&cb.t[cb.pos + 1], "SECTION"))
	{
		cb.pos += 2;
		if (cob_cur(&cb)->kind == CB_PERIOD)
			cb.pos++;
		while (cob_cur(&cb)->kind == CB_NUM)
			cob_decl(&cb);
	}
	/* optional PROCEDURE DIVISION [USING ...]. */
	if (cob_ci(cob_cur(&cb), "PROCEDURE") && cob_ci(&cb.t[cb.pos + 1], "DIVISION"))
	{
		cb.pos += 2;
		while (cob_cur(&cb)->kind != CB_PERIOD && cob_cur(&cb)->kind != CB_EOF)
			cb.pos++;
		if (cob_cur(&cb)->kind == CB_PERIOD)
			cb.pos++;
	}

	cob_block(&cb, 1);
	if (cob_cur(&cb)->kind != CB_EOF)
		cob_err(&cb, "unexpected text after the procedure body");
}

/* ======================================================================== */
/* PL/SQL front end (plxplsql, Oracle).                                     */
/*                                                                          */
/* PL/SQL and plpgsql are both Ada-descended, so most of the language       */
/* (DECLARE/BEGIN/EXCEPTION/END, IF/ELSIF, LOOP, CASE, assignment (:=), ||, */
/* cursors, %TYPE) is already valid plpgsql and passes through verbatim.    */
/* This is a token rewriter that preserves the source layout and applies a  */
/* small set of Oracle->Postgres translations: type names (NUMBER,          */
/* VARCHAR2, ...), DBMS_OUTPUT.PUT_LINE, RAISE_APPLICATION_ERROR, EXECUTE    */
/* IMMEDIATE, FROM DUAL, NVL, seq.NEXTVAL, SYSDATE. The body already carries */
/* its own DECLARE/BEGIN/END, so plxplsql skips the plx wrapper (assemble).  */
/* ======================================================================== */

typedef enum
{
	PL_EOF, PL_WORD, PL_NUM, PL_STR, PL_PUNCT
} PlKind;

typedef struct
{
	PlKind		kind;
	const char *s;
	int			len;
	const char *pre;			/* verbatim whitespace + comments before token */
	int			prelen;
} PlTok;

typedef struct
{
	Ctx		   *cx;
	PlTok	   *t;
	int			nt;
} Pl;

static bool
pl_ci(PlTok *tk, const char *w)
{
	int			l = (int) strlen(w);

	return tk->kind == PL_WORD && tk->len == l && pg_strncasecmp(tk->s, w, l) == 0;
}

static bool
pl_punct(PlTok *tk, char c)
{
	return tk->kind == PL_PUNCT && tk->len == 1 && tk->s[0] == c;
}

static bool
pl_word_char(char c)
{
	return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
		(c >= '0' && c <= '9') || c == '_' || c == '$' || c == '#';
}

static void
pl_lex(Pl *pl, const char *body)
{
	const char *p = body,
			   *end = body + strlen(body);
	int			cap = 256,
				n = 0;
	PlTok	   *t = palloc(sizeof(PlTok) * cap);

#define PLPUSH(k, st, ln, pr, prl) do { \
		if (n >= cap) { cap *= 2; t = repalloc(t, sizeof(PlTok) * cap); } \
		t[n].kind = (k); t[n].s = (st); t[n].len = (ln); \
		t[n].pre = (pr); t[n].prelen = (prl); n++; \
	} while (0)

	while (p < end)
	{
		const char *pre = p;

		/* accumulate leading trivia: whitespace and comments */
		for (;;)
		{
			if (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')
				p++;
			else if (p[0] == '-' && p + 1 < end && p[1] == '-')
			{
				while (p < end && *p != '\n')
					p++;
			}
			else if (p[0] == '/' && p + 1 < end && p[1] == '*')
			{
				p += 2;
				while (p + 1 < end && !(p[0] == '*' && p[1] == '/'))
					p++;
				p += 2;
				if (p > end)
					p = end;
			}
			else
				break;
		}
		if (p >= end)
		{
			/* trailing trivia attaches to EOF */
			PLPUSH(PL_EOF, p, 0, pre, (int) (p - pre));
			break;
		}

		{
			const char *st = p;
			int			prelen = (int) (st - pre);

			if (*p == '\'')					/* string literal */
			{
				p++;
				while (p < end)
				{
					if (*p == '\'')
					{
						if (p + 1 < end && p[1] == '\'')
						{
							p += 2;
							continue;
						}
						p++;
						break;
					}
					p++;
				}
				PLPUSH(PL_STR, st, (int) (p - st), pre, prelen);
			}
			else if (*p == '"')				/* quoted identifier */
			{
				p++;
				while (p < end && *p != '"')
					p++;
				if (p < end)
					p++;
				PLPUSH(PL_WORD, st, (int) (p - st), pre, prelen);
			}
			else if ((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') || *p == '_')
			{
				while (p < end && pl_word_char(*p))
					p++;
				PLPUSH(PL_WORD, st, (int) (p - st), pre, prelen);
			}
			else if (*p >= '0' && *p <= '9')
			{
				while (p < end && ((*p >= '0' && *p <= '9') || *p == '.' || *p == 'e' ||
								   *p == 'E' || *p == '+' || *p == '-'))
				{
					/* stop a '.' that begins '..' (range) or is not part of a number */
					if (*p == '.' && p + 1 < end && p[1] == '.')
						break;
					if ((*p == '+' || *p == '-') && !(p > st && (p[-1] == 'e' || p[-1] == 'E')))
						break;
					p++;
				}
				PLPUSH(PL_NUM, st, (int) (p - st), pre, prelen);
			}
			else
			{
				/* punctuation / operators, multi-char first */
				int			l = 1;

				if (p + 1 < end)
				{
					if ((p[0] == ':' && p[1] == '=') ||
						(p[0] == '|' && p[1] == '|') ||
						(p[0] == '<' && (p[1] == '=' || p[1] == '>')) ||
						(p[0] == '>' && p[1] == '=') ||
						(p[0] == '!' && p[1] == '=') ||
						(p[0] == '.' && p[1] == '.'))
						l = 2;
				}
				PLPUSH(PL_PUNCT, st, l, pre, prelen);
				p += l;
			}
		}
	}
	if (n == 0 || t[n - 1].kind != PL_EOF)
		PLPUSH(PL_EOF, p, 0, p, 0);
	pl->t = t;
	pl->nt = n;
#undef PLPUSH
}

/* Oracle scalar type -> PostgreSQL type, or NULL if not a mapped type word */
static const char *
pl_type_map(PlTok *tk)
{
	static const struct { const char *o; const char *pg; } m[] = {
		{"NUMBER", "numeric"}, {"VARCHAR2", "varchar"}, {"NVARCHAR2", "varchar"},
		{"PLS_INTEGER", "integer"}, {"BINARY_INTEGER", "integer"},
		{"SIMPLE_INTEGER", "integer"}, {"BINARY_FLOAT", "real"},
		{"BINARY_DOUBLE", "double precision"}, {"CLOB", "text"}, {"NCLOB", "text"},
		{"LONG", "text"}, {"BLOB", "bytea"}, {"RAW", "bytea"}, {"NUMBER", "numeric"},
	};
	int			i;

	for (i = 0; i < (int) (sizeof(m) / sizeof(m[0])); i++)
		if (pl_ci(tk, m[i].o))
			return m[i].pg;
	return NULL;
}

/* single-word function/value substitution (safe to apply anywhere), or NULL */
static const char *
pl_word_map(PlTok *tk)
{
	if (pl_ci(tk, "NVL"))
		return "coalesce";
	if (pl_ci(tk, "SYSDATE"))
		return "LOCALTIMESTAMP";
	if (pl_ci(tk, "SYSTIMESTAMP"))
		return "clock_timestamp()";
	return NULL;
}

/* index of the ')' matching the '(' at index lp, or -1 */
static int
pl_match_paren(Pl *pl, int lp)
{
	int			depth = 0,
				i;

	for (i = lp; i < pl->nt; i++)
	{
		if (pl_punct(&pl->t[i], '('))
			depth++;
		else if (pl_punct(&pl->t[i], ')'))
		{
			depth--;
			if (depth == 0)
				return i;
		}
		else if (pl->t[i].kind == PL_EOF)
			break;
	}
	return -1;
}

/* raw source text spanning tokens [a, b) (text only, not leading trivia of a) */
static void
pl_emit_raw(Pl *pl, int a, int b, StringInfo out)
{
	if (a >= b)
		return;
	/* from the start of token a's text to the end of token b-1's text */
	appendBinaryStringInfo(out, pl->t[a].s,
						   (int) (pl->t[b - 1].s + pl->t[b - 1].len - pl->t[a].s));
}

static void
plsql_transpile(Ctx *cx)
{
	Pl			pl;
	StringInfo	out = &cx->out;
	int			i,
				first;
	bool		need_declare;
	bool		in_decls = true;	/* type names map only before the first BEGIN */

	memset(&pl, 0, sizeof(pl));
	pl.cx = cx;
	pl_lex(&pl, cx->body);

	/* find the first real token; if the body does not already open with BEGIN or
	 * DECLARE, its leading declarations need a DECLARE keyword */
	for (first = 0; first < pl.nt && pl.t[first].kind == PL_EOF; first++)
		 /* skip (only EOF means empty) */ ;
	need_declare = (pl.nt > 0 && pl.t[0].kind != PL_EOF &&
					!pl_ci(&pl.t[0], "BEGIN") && !pl_ci(&pl.t[0], "DECLARE"));
	if (need_declare)
		appendStringInfoString(out, "DECLARE");

	i = 0;
	while (i < pl.nt && pl.t[i].kind != PL_EOF)
	{
		PlTok	   *t = &pl.t[i];

		/* leading trivia is always preserved verbatim */
		if (t->prelen)
			appendBinaryStringInfo(out, t->pre, t->prelen);

		if (t->kind == PL_WORD)
		{
			const char *repl;

			/* leaving the declaration section: stop mapping type names, so a body
			 * reference like "SELECT number FROM t" is not rewritten */
			if (in_decls && pl_ci(t, "BEGIN"))
				in_decls = false;

			/* EXECUTE IMMEDIATE -> EXECUTE */
			if (pl_ci(t, "EXECUTE") && i + 1 < pl.nt && pl_ci(&pl.t[i + 1], "IMMEDIATE"))
			{
				appendStringInfoString(out, "EXECUTE");
				i += 2;
				continue;
			}
			/* FROM DUAL -> (nothing) */
			if (pl_ci(t, "FROM") && i + 1 < pl.nt && pl_ci(&pl.t[i + 1], "DUAL"))
			{
				i += 2;
				continue;
			}
			/* CURSOR name IS -> name CURSOR FOR */
			if (pl_ci(t, "CURSOR") && i + 2 < pl.nt && pl.t[i + 1].kind == PL_WORD &&
				pl_ci(&pl.t[i + 2], "IS"))
			{
				appendBinaryStringInfo(out, pl.t[i + 1].s, pl.t[i + 1].len);
				appendStringInfoString(out, " CURSOR FOR");
				i += 3;
				continue;
			}
			/* DBMS_OUTPUT.PUT_LINE(x) / .PUT(x) -> RAISE NOTICE '%', (x) */
			if (pl_ci(t, "DBMS_OUTPUT") && i + 3 < pl.nt && pl_punct(&pl.t[i + 1], '.') &&
				(pl_ci(&pl.t[i + 2], "PUT_LINE") || pl_ci(&pl.t[i + 2], "PUT")) &&
				pl_punct(&pl.t[i + 3], '('))
			{
				int			close = pl_match_paren(&pl, i + 3);

				if (close > 0)
				{
					appendStringInfoString(out, "RAISE NOTICE '%', (");
					pl_emit_raw(&pl, i + 4, close, out);
					appendStringInfoChar(out, ')');
					i = close + 1;
					continue;
				}
			}
			/* RAISE_APPLICATION_ERROR(num, msg) -> RAISE EXCEPTION '%', (msg) */
			if (pl_ci(t, "RAISE_APPLICATION_ERROR") && i + 1 < pl.nt &&
				pl_punct(&pl.t[i + 1], '('))
			{
				int			close = pl_match_paren(&pl, i + 1);
				int			comma = -1,
							depth = 0,
							k;

				for (k = i + 2; close > 0 && k < close; k++)
				{
					if (pl_punct(&pl.t[k], '('))
						depth++;
					else if (pl_punct(&pl.t[k], ')'))
						depth--;
					else if (depth == 0 && pl_punct(&pl.t[k], ','))
					{
						comma = k;
						break;
					}
				}
				if (close > 0 && comma > 0)
				{
					int			mend = close;
					int			d2 = 0,
								k2;

					/* message runs to the next top-level comma or the ')' */
					for (k2 = comma + 1; k2 < close; k2++)
					{
						if (pl_punct(&pl.t[k2], '('))
							d2++;
						else if (pl_punct(&pl.t[k2], ')'))
							d2--;
						else if (d2 == 0 && pl_punct(&pl.t[k2], ','))
						{
							mend = k2;
							break;
						}
					}
					appendStringInfoString(out, "RAISE EXCEPTION '%', (");
					pl_emit_raw(&pl, comma + 1, mend, out);
					appendStringInfoChar(out, ')');
					i = close + 1;
					continue;
				}
			}
			/* seq.NEXTVAL / seq.CURRVAL -> nextval('seq') / currval('seq') */
			if (i + 2 < pl.nt && pl_punct(&pl.t[i + 1], '.') &&
				(pl_ci(&pl.t[i + 2], "NEXTVAL") || pl_ci(&pl.t[i + 2], "CURRVAL")))
			{
				appendStringInfo(out, "%s('%.*s')",
								 pl_ci(&pl.t[i + 2], "NEXTVAL") ? "nextval" : "currval",
								 t->len, t->s);
				i += 3;
				continue;
			}
			/* type name (declaration section only), then function/value maps */
			repl = in_decls ? pl_type_map(t) : NULL;
			if (!repl)
				repl = pl_word_map(t);
			if (repl)
				appendStringInfoString(out, repl);
			else
				appendBinaryStringInfo(out, t->s, t->len);
			i++;
			continue;
		}

		/* strings, numbers, punctuation: verbatim */
		appendBinaryStringInfo(out, t->s, t->len);
		i++;
	}

	/* trailing trivia (attached to EOF) */
	if (pl.nt > 0 && pl.t[pl.nt - 1].kind == PL_EOF && pl.t[pl.nt - 1].prelen)
		appendBinaryStringInfo(out, pl.t[pl.nt - 1].pre, pl.t[pl.nt - 1].prelen);
}

/* ======================================================================== */
/* TypeScript preprocessing (plxts).                                        */
/*                                                                          */
/* plxts is the plxjs (brace) dialect plus TypeScript type annotations. A   */
/* declaration "let x: T = e" is rewritten to the JS leading-colon-colon     */
/* block-comment annotation the shared parser already understands, with T    */
/* mapped from a TypeScript type to a SQL type. Only declaration annotations */
/* are touched (after let/const/var), so ternary and label colons are left   */
/* alone. The original TypeScript is what gets embedded for debugging.       */
/* ======================================================================== */

/* map a TypeScript type to a SQL type (palloc'd result) */
static char *
ts_map_type(const char *s, int len)
{
	StringInfoData r;
	bool		arr = false;

	while (len > 0 && (*s == ' ' || *s == '\t'))
	{
		s++;
		len--;
	}
	while (len > 0 && (s[len - 1] == ' ' || s[len - 1] == '\t'))
		len--;

	/* union: "T | null" / "T | undefined" -> first non-null member */
	{
		int			depth = 0,
					i;

		for (i = 0; i < len; i++)
		{
			char		c = s[i];

			if (c == '<' || c == '(' || c == '[' || c == '{')
				depth++;
			else if (c == '>' || c == ')' || c == ']' || c == '}')
				depth--;
			else if (depth == 0 && c == '|')
			{
				int			la = i + 1,
							lb = len;
				const char *rest = s + la;
				int			restlen = lb - la;

				/* left member */
				if (!((i == 4 && pg_strncasecmp(s, "null", 4) == 0) ||
					  (i == 9 && pg_strncasecmp(s, "undefined", 9) == 0)))
					return ts_map_type(s, i);
				return ts_map_type(rest, restlen);
			}
		}
	}

	/* trailing [] -> array */
	if (len >= 2 && s[len - 2] == '[' && s[len - 1] == ']')
	{
		arr = true;
		len -= 2;
		while (len > 0 && (s[len - 1] == ' ' || s[len - 1] == '\t'))
			len--;
	}

	initStringInfo(&r);
	if (len == 6 && pg_strncasecmp(s, "number", 6) == 0)
		appendStringInfoString(&r, "numeric");
	else if (len == 6 && pg_strncasecmp(s, "string", 6) == 0)
		appendStringInfoString(&r, "text");
	else if (len == 7 && pg_strncasecmp(s, "boolean", 7) == 0)
		appendStringInfoString(&r, "boolean");
	else if (len == 6 && pg_strncasecmp(s, "bigint", 6) == 0)
		appendStringInfoString(&r, "bigint");
	else
		appendBinaryStringInfo(&r, s, len);	/* pass through a SQL type verbatim */
	if (arr)
		appendStringInfoString(&r, "[]");
	return r.data;
}

/* copy a quoted string / template literal starting at *pp verbatim into out */
static void
ts_copy_string(const char **pp, const char *end, StringInfo out)
{
	const char *p = *pp;
	char		q = *p;

	appendStringInfoChar(out, *p);
	p++;
	if (q == '`')
	{
		int			brace = 0;

		while (p < end)
		{
			if (*p == '\\' && p + 1 < end)
			{
				appendBinaryStringInfo(out, p, 2);
				p += 2;
				continue;
			}
			if (brace == 0 && *p == '`')
			{
				appendStringInfoChar(out, *p);
				p++;
				break;
			}
			if (*p == '$' && p + 1 < end && p[1] == '{')
			{
				appendBinaryStringInfo(out, p, 2);
				p += 2;
				brace++;
				continue;
			}
			if (brace > 0 && *p == '}')
				brace--;
			appendStringInfoChar(out, *p);
			p++;
		}
	}
	else
	{
		while (p < end)
		{
			if (*p == '\\' && p + 1 < end)
			{
				appendBinaryStringInfo(out, p, 2);
				p += 2;
				continue;
			}
			appendStringInfoChar(out, *p);
			if (*p == q)
			{
				p++;
				break;
			}
			p++;
		}
	}
	*pp = p;
}

/* advance *pp past a string / template literal (no output) */
static void
ts_skip_string(const char **pp, const char *end)
{
	const char *p = *pp;
	char		q = *p++;
	int			brace = 0;

	while (p < end)
	{
		if (*p == '\\' && p + 1 < end)
		{
			p += 2;
			continue;
		}
		if (q == '`' && *p == '$' && p + 1 < end && p[1] == '{')
		{
			p += 2;
			brace++;
			continue;
		}
		if (q == '`' && brace > 0 && *p == '}')
		{
			brace--;
			p++;
			continue;
		}
		if (brace == 0 && *p == q)
		{
			p++;
			break;
		}
		p++;
	}
	*pp = p;
}

static char *
ts_preprocess(const char *src)
{
	const char *p = src,
			   *end = src + strlen(src);
	StringInfoData out;
	int			pdepth = 0;			/* parenthesis nesting */
	int			for_depth = -1;		/* paren depth of an open for(...) header */
	bool		last_for = false;	/* previous significant word was "for" */

	initStringInfo(&out);
	while (p < end)
	{
		if (*p == '\'' || *p == '"' || *p == '`')
		{
			last_for = false;
			ts_copy_string(&p, end, &out);
			continue;
		}
		if (p[0] == '/' && p + 1 < end && p[1] == '/')
		{
			while (p < end && *p != '\n')
				appendStringInfoChar(&out, *p++);
			continue;
		}
		if (p[0] == '/' && p + 1 < end && p[1] == '*')
		{
			appendBinaryStringInfo(&out, p, 2);
			p += 2;
			while (p < end && !(p[0] == '*' && p + 1 < end && p[1] == '/'))
				appendStringInfoChar(&out, *p++);
			if (p < end)
			{
				appendBinaryStringInfo(&out, p, 2);
				p += 2;
			}
			continue;
		}
		if (*p == '(')
		{
			pdepth++;
			if (last_for)
				for_depth = pdepth;
			last_for = false;
			appendStringInfoChar(&out, *p++);
			continue;
		}
		if (*p == ')')
		{
			if (pdepth == for_depth)
				for_depth = -1;
			if (pdepth > 0)
				pdepth--;
			last_for = false;
			appendStringInfoChar(&out, *p++);
			continue;
		}
		if (is_ident_start(*p))
		{
			const char *ws = p;			/* word start */

			while (p < end && is_ident(*p))
				p++;
			appendBinaryStringInfo(&out, ws, (int) (p - ws));

			/* let / const / var  IDENT  : TYPE  ->  a JS block-comment annotation
			 * (dropped inside a for(...) header, where the counter is integer) */
			if ((p - ws == 3 && (strncmp(ws, "let", 3) == 0 || strncmp(ws, "var", 3) == 0)) ||
				(p - ws == 5 && strncmp(ws, "const", 5) == 0))
			{
				const char *q = p;
				const char *ide;

				while (q < end && (*q == ' ' || *q == '\t'))
					q++;
				if (q < end && is_ident_start(*q))
				{
					const char *r;

					while (q < end && is_ident(*q))
						q++;
					ide = q;
					r = q;
					while (r < end && (*r == ' ' || *r == '\t'))
						r++;
					if (r < end && *r == ':')
					{
						const char *ts,
								   *te;
						char	   *mapped;
						int			depth = 0;

						appendBinaryStringInfo(&out, p, (int) (ide - p));	/* "let x" */
						r++;			/* skip ':' */
						while (r < end && (*r == ' ' || *r == '\t'))
							r++;
						ts = r;
						while (r < end)	/* read the type up to = ; , ) or newline */
						{
							char		c = *r;

							if (c == '<' || c == '(' || c == '[' || c == '{')
								depth++;
							else if (c == '>' || c == ')' || c == ']' || c == '}')
								depth--;
							else if (depth <= 0 && (c == '=' || c == ';' || c == ',' ||
													c == ')' || c == '\n'))
								break;
							r++;
						}
						te = r;
						mapped = ts_map_type(ts, (int) (te - ts));

						if (te < end && *te == '=')
						{
							/* has a value: emit "= value", then the annotation, so
							 * plxjs sees the annotation trailing the value */
							const char *v = te;
							int			vd = 0;

							while (v < end)
							{
								char		c = *v;

								if (c == '\'' || c == '"' || c == '`')
								{
									ts_skip_string(&v, end);
									continue;
								}
								if (c == '/' && v + 1 < end && v[1] == '/')
								{
									while (v < end && *v != '\n')
										v++;
									continue;
								}
								if (c == '(' || c == '[' || c == '{')
									vd++;
								else if (c == ')' || c == ']' || c == '}')
								{
									if (vd == 0)
										break;
									vd--;
								}
								else if (vd == 0 && (c == ';' || c == ','))
									break;
								v++;
							}
							appendBinaryStringInfo(&out, te, (int) (v - te));
							if (for_depth < 0)
								appendStringInfo(&out, " /*:: %s */", mapped);
							p = v;
						}
						else
						{
							/* no value: annotate the declaration directly */
							if (for_depth < 0)
								appendStringInfo(&out, " /*:: %s */", mapped);
							else
								appendStringInfoChar(&out, ' ');
							p = te;
						}
						last_for = false;
						continue;
					}
				}
			}
			last_for = (p - ws == 3 && strncmp(ws, "for", 3) == 0);
			continue;
		}
		if (*p != ' ' && *p != '\t' && *p != '\n' && *p != '\r')
			last_for = false;
		appendStringInfoChar(&out, *p);
		p++;
	}
	return out.data;
}

/* ---------------------------------------------------------------- assemble */

/* Self-contained base64 encoder (the server pg_b64_encode signature differs
 * across major versions; this keeps plx source-compatible from PG13 up). */
static char *
b64_encode_body(const char *body)
{
	static const char b64[] =
		"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
	const unsigned char *src = (const unsigned char *) body;
	int			srclen = (int) strlen(body);
	char	   *dst = palloc((size_t) (srclen + 2) / 3 * 4 + 1);
	char	   *o = dst;
	int			i = 0;

	while (i + 3 <= srclen)
	{
		unsigned int v = (src[i] << 16) | (src[i + 1] << 8) | src[i + 2];

		*o++ = b64[(v >> 18) & 0x3f];
		*o++ = b64[(v >> 12) & 0x3f];
		*o++ = b64[(v >> 6) & 0x3f];
		*o++ = b64[v & 0x3f];
		i += 3;
	}
	if (srclen - i == 1)
	{
		unsigned int v = src[i] << 16;

		*o++ = b64[(v >> 18) & 0x3f];
		*o++ = b64[(v >> 12) & 0x3f];
		*o++ = '=';
		*o++ = '=';
	}
	else if (srclen - i == 2)
	{
		unsigned int v = (src[i] << 16) | (src[i + 1] << 8);

		*o++ = b64[(v >> 18) & 0x3f];
		*o++ = b64[(v >> 12) & 0x3f];
		*o++ = b64[(v >> 6) & 0x3f];
		*o++ = '=';
	}
	*o = '\0';
	return dst;
}

/* ======================================================================== */
/* T-SQL front end (plxtsql, Transact-SQL: SQL Server / Sybase).            */
/*                                                                          */
/* T-SQL differs from plpgsql in ways that need real restructuring, so like */
/* COBOL it uses its own tokenizer and recursive-descent parser and emits   */
/* plpgsql directly:                                                        */
/*   - @local variables (and inline DECLARE anywhere) -> hoisted DECLARE    */
/*     block; @x references drop the sigil.                                 */
/*   - SET @x = e / SET @x += e            -> x := e; / x := x + (e);        */
/*   - SELECT @x = e [, ...] [FROM ...]    -> x := e;  or  SELECT ... INTO   */
/*   - IF cond stmt|BEGIN..END [ELSE ...]  -> IF cond THEN ... END IF;       */
/*   - WHILE cond stmt|BEGIN..END          -> WHILE cond LOOP ... END LOOP;  */
/*   - BEGIN TRY..END TRY BEGIN CATCH..END CATCH -> BEGIN..EXCEPTION..END    */
/*   - PRINT e -> RAISE NOTICE; RAISERROR/THROW -> RAISE EXCEPTION;          */
/*   - a library of type names (INT, NVARCHAR(n), DATETIME, ...) and         */
/*     functions (ISNULL, LEN, GETDATE, IIF, CONVERT, ...).                  */
/* Populates cx->locals + cx->out; the shared assemble path wraps it.       */
/* ======================================================================== */

typedef enum
{
	TQ_EOF, TQ_WORD, TQ_VAR, TQ_GVAR, TQ_NUM, TQ_STR, TQ_OP,
	TQ_LP, TQ_RP, TQ_COMMA, TQ_SEMI, TQ_DOT
} TqKind;

typedef struct
{
	TqKind		kind;
	const char *s;
	int			len;
	int			line;
} TqTok;

typedef struct
{
	Ctx		   *cx;
	TqTok	   *t;
	int			nt, pos;
} Tq;

static void tq_stmt(Tq *tq, int ind);
static void tq_block(Tq *tq, int ind);
static void tq_body(Tq *tq, int ind);
static void tq_emit_range(Tq *tq, int a, int b, StringInfo out);

static TqTok *
tq_cur(Tq *tq)
{
	return &tq->t[tq->pos];
}

/* case-insensitive: is token tk exactly the word w? */
static bool
tq_ci(TqTok *tk, const char *w)
{
	int			l = (int) strlen(w);

	return tk->kind == TQ_WORD && tk->len == l && pg_strncasecmp(tk->s, w, l) == 0;
}

pg_noreturn static void
tq_err(Tq *tq, const char *msg)
{
	plx_err(tq->cx, tq->t[tq->pos].line, "%s", msg);
}

/* lower-cased identifier from a token's raw text (for @vars and their decls) */
static char *
tq_ident(const char *s, int len)
{
	char	   *r = palloc(len + 1);
	int			i;

	for (i = 0; i < len; i++)
	{
		char		c = s[i];

		r[i] = (c >= 'A' && c <= 'Z') ? (c - 'A' + 'a') : c;
	}
	r[len] = '\0';
	return r;
}

static void
tq_lex(Tq *tq, const char *body)
{
	const char *p = body,
			   *end = body + strlen(body);
	int			line = 1,
				cap = 256,
				n = 0;
	TqTok	   *t = palloc(sizeof(TqTok) * cap);

#define TQPUSH(k, st, ln) do { \
		if (n >= cap) { cap *= 2; t = repalloc(t, sizeof(TqTok) * cap); } \
		t[n].kind = (k); t[n].s = (st); t[n].len = (ln); t[n].line = line; n++; \
	} while (0)
#define TQALPHA(c) (((c) >= 'A' && (c) <= 'Z') || ((c) >= 'a' && (c) <= 'z') || (c) == '_')
#define TQDIGIT(c) ((c) >= '0' && (c) <= '9')

	while (p < end)
	{
		if (*p == '\n')
		{
			line++;
			p++;
			continue;
		}
		if (*p == ' ' || *p == '\t' || *p == '\r')
		{
			p++;
			continue;
		}
		if (p[0] == '-' && p + 1 < end && p[1] == '-')	/* -- line comment */
		{
			while (p < end && *p != '\n')
				p++;
			continue;
		}
		if (p[0] == '/' && p + 1 < end && p[1] == '*')	/* block comment, nestable */
		{
			int			d = 1;

			p += 2;
			while (p < end && d > 0)
			{
				if (p + 1 < end && p[0] == '/' && p[1] == '*')
				{
					d++;
					p += 2;
				}
				else if (p + 1 < end && p[0] == '*' && p[1] == '/')
				{
					d--;
					p += 2;
				}
				else
				{
					if (*p == '\n')
						line++;
					p++;
				}
			}
			continue;
		}
		if (*p == '@')									/* @@global or @local */
		{
			if (p + 1 < end && p[1] == '@')
			{
				const char *st = p + 2,
						   *q = st;

				while (q < end && (TQALPHA(*q) || TQDIGIT(*q)))
					q++;
				TQPUSH(TQ_GVAR, st, (int) (q - st));
				p = q;
				continue;
			}
			else
			{
				const char *st = p + 1,
						   *q = st;

				while (q < end && (TQALPHA(*q) || TQDIGIT(*q) || *q == '@' || *q == '#' || *q == '$'))
					q++;
				TQPUSH(TQ_VAR, st, (int) (q - st));
				p = q;
				continue;
			}
		}
		if (*p == '[')									/* [delimited identifier] */
		{
			const char *st = p + 1,
					   *q = st;

			while (q < end && *q != ']')
				q++;
			TQPUSH(TQ_WORD, st, (int) (q - st));
			p = (q < end) ? q + 1 : q;
			continue;
		}
		if (*p == '"')									/* "quoted identifier" */
		{
			const char *st = p + 1,
					   *q = st;

			while (q < end && *q != '"')
				q++;
			TQPUSH(TQ_WORD, st, (int) (q - st));
			p = (q < end) ? q + 1 : q;
			continue;
		}
		if (*p == 'N' && p + 1 < end && p[1] == '\'')	/* N'unicode' -> drop N */
			p++;
		if (*p == '\'')									/* string literal */
		{
			const char *st = p;
			bool		closed = false;

			p++;
			while (p < end)
			{
				if (*p == '\'')
				{
					if (p + 1 < end && p[1] == '\'')	/* doubled quote */
					{
						p += 2;
						continue;
					}
					p++;
					closed = true;
					break;
				}
				if (*p == '\n')
					line++;
				p++;
			}
			if (!closed)
				plx_err(tq->cx, line, "unterminated string literal");
			TQPUSH(TQ_STR, st, (int) (p - st));
			continue;
		}
		if (TQALPHA(*p) || *p == '#')					/* word / #temp */
		{
			const char *st = p;

			while (p < end && (TQALPHA(*p) || TQDIGIT(*p) || *p == '#' || *p == '$'))
				p++;
			TQPUSH(TQ_WORD, st, (int) (p - st));
			continue;
		}
		if (TQDIGIT(*p) || (*p == '.' && p + 1 < end && TQDIGIT(p[1])))
		{
			const char *st = p;

			while (p < end && (TQDIGIT(*p) || *p == '.' ||
							   ((*p == 'e' || *p == 'E') && p + 1 < end) ||
							   *p == 'x' || *p == 'X' ||
							   (TQALPHA(*p) && st[0] == '0' && (st[1] == 'x' || st[1] == 'X'))))
				p++;
			TQPUSH(TQ_NUM, st, (int) (p - st));
			continue;
		}
		if (*p == '(')
		{
			TQPUSH(TQ_LP, p, 1);
			p++;
			continue;
		}
		if (*p == ')')
		{
			TQPUSH(TQ_RP, p, 1);
			p++;
			continue;
		}
		if (*p == ',')
		{
			TQPUSH(TQ_COMMA, p, 1);
			p++;
			continue;
		}
		if (*p == ';')
		{
			TQPUSH(TQ_SEMI, p, 1);
			p++;
			continue;
		}
		if (*p == '.')
		{
			TQPUSH(TQ_DOT, p, 1);
			p++;
			continue;
		}
		{
			/* two-character operators */
			static const char *ops2[] = {
				"<=", ">=", "<>", "!=", "!<", "!>",
				"+=", "-=", "*=", "/=", "%=", "||", "::", NULL
			};
			int			k;
			bool		matched = false;

			for (k = 0; ops2[k]; k++)
				if (p + 1 < end && p[0] == ops2[k][0] && p[1] == ops2[k][1])
				{
					TQPUSH(TQ_OP, ops2[k], 2);
					p += 2;
					matched = true;
					break;
				}
			if (matched)
				continue;
		}
		TQPUSH(TQ_OP, p, 1);							/* single-char operator */
		p++;
	}
	TQPUSH(TQ_EOF, end, 0);
	tq->t = t;
	tq->nt = n;
	tq->pos = 0;
#undef TQPUSH
#undef TQALPHA
#undef TQDIGIT
}

/* T-SQL statement-starting keywords (bound a condition/value scan) */
static bool
tq_is_stmt_kw(TqTok *tk)
{
	static const char *k[] = {
		"SELECT", "SET", "UPDATE", "INSERT", "DELETE", "MERGE", "PRINT",
		"RETURN", "IF", "WHILE", "EXEC", "EXECUTE", "DECLARE", "THROW",
		"RAISERROR", "BREAK", "CONTINUE", "WAITFOR", "GOTO", "TRUNCATE",
		"CREATE", "DROP", "ALTER", "OPEN", "CLOSE", "FETCH", "COMMIT",
		"ROLLBACK", NULL
	};
	int			i;

	if (tk->kind != TQ_WORD)
		return false;
	for (i = 0; k[i]; i++)
		if (tq_ci(tk, k[i]))
			return true;
	return false;
}

/* append with a leading space when needed */
static void
tq_sp(StringInfo out)
{
	char		last = out->len ? out->data[out->len - 1] : ' ';

	if (out->len && last != ' ' && last != '(' && last != '.')
		appendStringInfoChar(out, ' ');
}

/* emit a T-SQL string literal (already 'quoted', ''-escaped) as SQL text */
static void
tq_emit_str(TqTok *tk, StringInfo out)
{
	/* T-SQL single-quoted strings use '' for an embedded quote, exactly like
	 * SQL, so the literal passes through verbatim. */
	appendBinaryStringInfo(out, tk->s, tk->len);
}

/* map a T-SQL base type name to a PostgreSQL type; NULL if unknown */
static const char *
tq_base_type(const char *s, int len)
{
	struct
	{
		const char *tsql;
		const char *pg;
	}			m[] = {
		{"int", "integer"}, {"integer", "integer"},
		{"bigint", "bigint"}, {"smallint", "smallint"}, {"tinyint", "smallint"},
		{"bit", "boolean"},
		{"decimal", "numeric"}, {"numeric", "numeric"}, {"dec", "numeric"},
		{"money", "numeric(19,4)"}, {"smallmoney", "numeric(10,4)"},
		{"float", "double precision"}, {"real", "real"},
		{"date", "date"}, {"datetime", "timestamp"}, {"datetime2", "timestamp"},
		{"smalldatetime", "timestamp"}, {"datetimeoffset", "timestamptz"},
		{"time", "time"},
		{"char", "char"}, {"nchar", "char"},
		{"varchar", "varchar"}, {"nvarchar", "varchar"},
		{"text", "text"}, {"ntext", "text"}, {"sysname", "varchar"},
		{"uniqueidentifier", "uuid"}, {"xml", "xml"},
		{"binary", "bytea"}, {"varbinary", "bytea"}, {"image", "bytea"},
		{NULL, NULL}
	};
	int			i;

	for (i = 0; m[i].tsql; i++)
		if ((int) strlen(m[i].tsql) == len && pg_strncasecmp(m[i].tsql, s, len) == 0)
			return m[i].pg;
	return NULL;
}

/* read a type reference from the stream into dest, consuming its tokens */
static void
tq_read_type(Tq *tq, StringInfo dest)
{
	TqTok	   *w = tq_cur(tq);
	const char *base;
	bool		is_char = false,
				is_bin = false;

	if (w->kind != TQ_WORD)
		tq_err(tq, "expected a type name");
	base = tq_base_type(w->s, w->len);
	is_char = (base && (strcmp(base, "varchar") == 0 || strcmp(base, "char") == 0));
	is_bin = (base && strcmp(base, "bytea") == 0);
	if (base)
		appendStringInfoString(dest, base);
	else
	{
		/* unknown -> pass the identifier through (user-defined or PG type) */
		char	   *id = tq_ident(w->s, w->len);

		appendStringInfoString(dest, id);
	}
	tq->pos++;
	if (tq_cur(tq)->kind == TQ_LP)
	{
		TqTok	   *inner = &tq->t[tq->pos + 1];

		if (inner->kind == TQ_WORD && tq_ci(inner, "MAX"))
		{
			/* VARCHAR(MAX)/NVARCHAR(MAX) -> text; VARBINARY(MAX) -> bytea */
			dest->len = 0;
			dest->data[0] = '\0';
			appendStringInfoString(dest, is_bin ? "bytea" : "text");
			while (tq_cur(tq)->kind != TQ_RP && tq_cur(tq)->kind != TQ_EOF)
				tq->pos++;
			if (tq_cur(tq)->kind == TQ_RP)
				tq->pos++;
		}
		else if (base && !is_char && strcmp(base, "numeric") != 0)
		{
			/* fixed-width type carries no length in PG; drop the () */
			while (tq_cur(tq)->kind != TQ_RP && tq_cur(tq)->kind != TQ_EOF)
				tq->pos++;
			if (tq_cur(tq)->kind == TQ_RP)
				tq->pos++;
		}
		else
		{
			/* char/numeric width passes through: (n) or (p,s) */
			appendStringInfoChar(dest, '(');
			tq->pos++;
			while (tq_cur(tq)->kind != TQ_RP && tq_cur(tq)->kind != TQ_EOF)
			{
				TqTok	   *x = tq_cur(tq);

				if (x->kind == TQ_COMMA)
					appendStringInfoChar(dest, ',');
				else
					appendBinaryStringInfo(dest, x->s, x->len);
				tq->pos++;
			}
			appendStringInfoChar(dest, ')');
			if (tq_cur(tq)->kind == TQ_RP)
				tq->pos++;
		}
	}
}

/* map a type reference spanning tokens [a,b) into dest (for CONVERT) */
static void
tq_type_from_range(Tq *tq, int a, int b, StringInfo dest)
{
	TqTok	   *w;
	const char *base;

	if (a >= b || tq->t[a].kind != TQ_WORD)
	{
		appendStringInfoString(dest, "text");
		return;
	}
	w = &tq->t[a];
	base = tq_base_type(w->s, w->len);
	appendStringInfoString(dest, base ? base : tq_ident(w->s, w->len));
	if (base && (pg_strncasecmp(w->s, "char", 4) == 0 ||
				 pg_strncasecmp(w->s, "varc", 4) == 0 ||
				 pg_strncasecmp(w->s, "nvar", 4) == 0 ||
				 pg_strncasecmp(w->s, "ncha", 4) == 0 ||
				 strcmp(base, "numeric") == 0) &&
		a + 1 < b && tq->t[a + 1].kind == TQ_LP)
	{
		int			i = a + 1;

		while (i < b && tq->t[i].kind != TQ_RP)
		{
			if (tq->t[i].kind == TQ_COMMA)
				appendStringInfoChar(dest, ',');
			else if (tq->t[i].kind != TQ_LP)
				appendBinaryStringInfo(dest, tq->t[i].s, tq->t[i].len);
			else
				appendStringInfoChar(dest, '(');
			i++;
		}
		appendStringInfoChar(dest, ')');
	}
}

/* index of the ')' matching the '(' at lp, within [.,limit), or -1 */
static int
tq_match_lp(Tq *tq, int lp, int limit)
{
	int			depth = 0,
				i;

	for (i = lp; i < limit; i++)
	{
		if (tq->t[i].kind == TQ_LP)
			depth++;
		else if (tq->t[i].kind == TQ_RP && --depth == 0)
			return i;
	}
	return -1;
}

/* emit a function-call rename for a WORD at index i (followed by '('), or
 * return false to fall through to the generic path. On success sets *ni. */
static bool
tq_emit_call(Tq *tq, int i, int b, StringInfo out, int *ni)
{
	TqTok	   *tk = &tq->t[i];
	int			lp = i + 1,
				close;

	if (lp >= b || tq->t[lp].kind != TQ_LP)
		return false;
	close = tq_match_lp(tq, lp, b);
	if (close < 0)
		return false;

	/* CONVERT(type, expr [, style]) -> CAST(expr AS type) */
	if (tq_ci(tk, "CONVERT"))
	{
		int			c1 = lp + 1,
					depth = 0,
					comma = -1,
					comma2 = -1,
					j;
		StringInfoData ty;

		for (j = lp + 1; j < close; j++)
		{
			if (tq->t[j].kind == TQ_LP)
				depth++;
			else if (tq->t[j].kind == TQ_RP)
				depth--;
			else if (tq->t[j].kind == TQ_COMMA && depth == 0)
			{
				if (comma < 0)
					comma = j;
				else if (comma2 < 0)
					comma2 = j;
			}
		}
		if (comma < 0)
			return false;
		initStringInfo(&ty);
		tq_type_from_range(tq, c1, comma, &ty);
		tq_sp(out);
		appendStringInfoString(out, "CAST(");
		tq_emit_range(tq, comma + 1, comma2 > 0 ? comma2 : close, out);
		appendStringInfo(out, " AS %s)", ty.data);
		*ni = close + 1;
		return true;
	}

	/* IIF(cond, a, b) -> CASE WHEN cond THEN a ELSE b END */
	if (tq_ci(tk, "IIF"))
	{
		int			depth = 0,
					c1 = -1,
					c2 = -1,
					j;

		for (j = lp + 1; j < close; j++)
		{
			if (tq->t[j].kind == TQ_LP)
				depth++;
			else if (tq->t[j].kind == TQ_RP)
				depth--;
			else if (tq->t[j].kind == TQ_COMMA && depth == 0)
			{
				if (c1 < 0)
					c1 = j;
				else if (c2 < 0)
					c2 = j;
			}
		}
		if (c1 < 0 || c2 < 0)
			return false;
		tq_sp(out);
		appendStringInfoString(out, "CASE WHEN");
		tq_emit_range(tq, lp + 1, c1, out);
		appendStringInfoString(out, " THEN");
		tq_emit_range(tq, c1 + 1, c2, out);
		appendStringInfoString(out, " ELSE");
		tq_emit_range(tq, c2 + 1, close, out);
		appendStringInfoString(out, " END");
		*ni = close + 1;
		return true;
	}

	/* ERROR_MESSAGE() -> SQLERRM (no parens) */
	if (tq_ci(tk, "ERROR_MESSAGE") && close == lp + 1)
	{
		tq_sp(out);
		appendStringInfoString(out, "SQLERRM");
		*ni = close + 1;
		return true;
	}

	/* simple 1:1 renames keeping the argument list */
	{
		struct
		{
			const char *tsql;
			const char *pg;
		}			r[] = {
			{"isnull", "coalesce"}, {"len", "length"},
			{"datalength", "octet_length"}, {"getdate", "now"},
			{"sysdatetime", "now"}, {"getutcdate", "now"},
			{"newid", "gen_random_uuid"}, {"ceiling", "ceil"},
			{"charindex", NULL}, {NULL, NULL}
		};
		int			k;

		for (k = 0; r[k].tsql; k++)
			if (tq_ci(tk, r[k].tsql))
			{
				/* CHARINDEX(sub, str) -> strpos(str, sub): swap the two args */
				if (pg_strcasecmp(r[k].tsql, "charindex") == 0)
				{
					int			depth = 0,
								comma = -1,
								j;

					for (j = lp + 1; j < close; j++)
					{
						if (tq->t[j].kind == TQ_LP)
							depth++;
						else if (tq->t[j].kind == TQ_RP)
							depth--;
						else if (tq->t[j].kind == TQ_COMMA && depth == 0 && comma < 0)
							comma = j;
					}
					if (comma < 0)
						return false;
					tq_sp(out);
					appendStringInfoString(out, "strpos(");
					tq_emit_range(tq, comma + 1, close, out);
					appendStringInfoString(out, ",");
					tq_emit_range(tq, lp + 1, comma, out);
					appendStringInfoChar(out, ')');
					*ni = close + 1;
					return true;
				}
				tq_sp(out);
				appendStringInfoString(out, r[k].pg);
				appendStringInfoChar(out, '(');
				tq_emit_range(tq, lp + 1, close, out);
				appendStringInfoChar(out, ')');
				*ni = close + 1;
				return true;
			}
	}
	return false;
}

/* emit tokens [a,b) as a plpgsql expression */
static void
tq_emit_range(Tq *tq, int a, int b, StringInfo out)
{
	int			i = a;

	while (i < b)
	{
		TqTok	   *tk = &tq->t[i];

		if (tk->kind == TQ_VAR)
		{
			tq_sp(out);
			appendStringInfoString(out, tq_ident(tk->s, tk->len));
			i++;
			continue;
		}
		if (tk->kind == TQ_GVAR)
		{
			if (tk->len == 8 && pg_strncasecmp(tk->s, "IDENTITY", 8) == 0)
			{
				tq_sp(out);
				appendStringInfoString(out, "lastval()");
				i++;
				continue;
			}
			plx_err(tq->cx, tk->line,
					"the T-SQL global variable @@%.*s is not supported",
					tk->len, tk->s);
		}
		if (tk->kind == TQ_WORD)
		{
			int			ni;

			if (tq_emit_call(tq, i, b, out, &ni))
			{
				i = ni;
				continue;
			}
			/* GETDATE with no arg list still means now() */
			if (tq_ci(tk, "GETDATE") || tq_ci(tk, "SYSDATETIME"))
			{
				tq_sp(out);
				appendStringInfoString(out, "now()");
				i++;
				continue;
			}
			tq_sp(out);
			appendBinaryStringInfo(out, tk->s, tk->len);
			i++;
			continue;
		}
		if (tk->kind == TQ_NUM)
		{
			tq_sp(out);
			appendBinaryStringInfo(out, tk->s, tk->len);
			i++;
			continue;
		}
		if (tk->kind == TQ_STR)
		{
			tq_sp(out);
			tq_emit_str(tk, out);
			i++;
			continue;
		}
		if (tk->kind == TQ_LP)
		{
			tq_sp(out);
			appendStringInfoChar(out, '(');
			i++;
			continue;
		}
		if (tk->kind == TQ_RP)
		{
			appendStringInfoChar(out, ')');
			i++;
			continue;
		}
		if (tk->kind == TQ_COMMA)
		{
			appendStringInfoChar(out, ',');
			i++;
			continue;
		}
		if (tk->kind == TQ_DOT)
		{
			appendStringInfoChar(out, '.');
			i++;
			continue;
		}
		if (tk->kind == TQ_OP)
		{
			tq_sp(out);
			if (tk->len == 2 && tk->s[0] == '!' && tk->s[1] == '=')
				appendStringInfoString(out, "<>");
			else
				appendBinaryStringInfo(out, tk->s, tk->len);
			i++;
			continue;
		}
		i++;
	}
}

/* end of an expression/raw statement: first depth-0 ; EOF END or ELSE */
static int
tq_value_end(Tq *tq, int from)
{
	int			i = from,
				depth = 0;

	while (i < tq->nt)
	{
		TqTok	   *tk = &tq->t[i];

		if (tk->kind == TQ_LP)
			depth++;
		else if (tk->kind == TQ_RP)
		{
			if (depth > 0)
				depth--;
		}
		else if (depth == 0)
		{
			if (tk->kind == TQ_SEMI || tk->kind == TQ_EOF ||
				tq_ci(tk, "END") || tq_ci(tk, "ELSE"))
				return i;
		}
		i++;
	}
	return tq->nt - 1;
}

/* end of an IF/WHILE condition: where the body (a keyword or BEGIN) begins */
static int
tq_cond_end(Tq *tq, int from)
{
	int			i = from,
				depth = 0;

	while (i < tq->nt)
	{
		TqTok	   *tk = &tq->t[i];

		if (tk->kind == TQ_LP)
			depth++;
		else if (tk->kind == TQ_RP)
		{
			if (depth > 0)
				depth--;
		}
		else if (depth == 0)
		{
			if (tk->kind == TQ_SEMI || tk->kind == TQ_EOF ||
				tq_ci(tk, "END") || tq_ci(tk, "ELSE") ||
				tq_ci(tk, "BEGIN") || tq_is_stmt_kw(tk))
				return i;
		}
		i++;
	}
	return tq->nt - 1;
}

static void
tq_eat_semi(Tq *tq)
{
	while (tq_cur(tq)->kind == TQ_SEMI)
		tq->pos++;
}

/* DECLARE @a T [= e] [, @b ...] */
static void
tq_declare(Tq *tq, int ind)
{
	tq->pos++;								/* DECLARE */
	for (;;)
	{
		TqTok	   *v = tq_cur(tq);
		char	   *name;
		PlxLocal2  *l;
		StringInfoData ty;

		if (v->kind != TQ_VAR)
			tq_err(tq, "expected an @variable after DECLARE");
		name = tq_ident(v->s, v->len);
		tq->pos++;
		if (tq_ci(tq_cur(tq), "AS"))			/* DECLARE @c AS int (rare) */
			tq->pos++;
		if (tq_ci(tq_cur(tq), "TABLE") || tq_ci(tq_cur(tq), "CURSOR"))
			tq_err(tq, "DECLARE of a TABLE variable or CURSOR is not supported");
		initStringInfo(&ty);
		tq_read_type(tq, &ty);
		l = local_add(tq->cx, name, (int) strlen(name));
		l->typ = ty.data;
		if (tq_cur(tq)->kind == TQ_OP && tq_cur(tq)->len == 1 && tq_cur(tq)->s[0] == '=')
		{
			int			e0 = tq->pos + 1,
						e1;

			tq->pos++;
			e1 = tq->pos;
			/* value runs to a top-level comma or the statement terminator */
			{
				int			depth = 0;

				while (e1 < tq->nt)
				{
					TqTok	   *x = &tq->t[e1];

					if (x->kind == TQ_LP)
						depth++;
					else if (x->kind == TQ_RP && depth > 0)
						depth--;
					else if (depth == 0 &&
							 (x->kind == TQ_COMMA || x->kind == TQ_SEMI ||
							  x->kind == TQ_EOF || tq_ci(x, "END")))
						break;
					e1++;
				}
			}
			indent(&tq->cx->out, ind);
			appendStringInfo(&tq->cx->out, "%s :=", name);
			tq_emit_range(tq, e0, e1, &tq->cx->out);
			appendStringInfoString(&tq->cx->out, ";\n");
			tq->pos = e1;
		}
		if (tq_cur(tq)->kind == TQ_COMMA)
		{
			tq->pos++;
			continue;
		}
		break;
	}
	tq_eat_semi(tq);
}

/* SET @x = e | SET @x += e | SET <option> ... (ignored) */
static void
tq_set(Tq *tq, int ind)
{
	TqTok	   *v;
	char	   *name;
	TqTok	   *op;
	int			e0,
				e1;
	char		arith = 0;

	tq->pos++;								/* SET */
	v = tq_cur(tq);
	if (v->kind != TQ_VAR)
	{
		/*
		 * "SET <qualified> = e" assigns to a field, e.g. SET NEW.col = e in a
		 * trigger; emit "target := e". A session option (SET NOCOUNT ON, SET
		 * XACT_ABORT ON, ...) has no top-level '=' with a qualified target and
		 * is ignored.
		 */
		int			i,
					depth = 0,
					eq = -1;
		bool		qualified = false;

		e0 = tq->pos;
		e1 = tq_value_end(tq, e0);
		for (i = e0; i < e1; i++)
		{
			TqTok	   *t = &tq->t[i];

			if (t->kind == TQ_LP)
				depth++;
			else if (t->kind == TQ_RP)
			{
				if (depth > 0)
					depth--;
			}
			else if (depth == 0 && eq < 0)
			{
				if (t->kind == TQ_DOT)
					qualified = true;
				else if (t->kind == TQ_OP && t->len == 1 && t->s[0] == '=')
					eq = i;
			}
		}
		if (eq > e0 && qualified)
		{
			indent(&tq->cx->out, ind);
			tq_emit_range(tq, e0, eq, &tq->cx->out);
			appendStringInfoString(&tq->cx->out, " :=");
			tq_emit_range(tq, eq + 1, e1, &tq->cx->out);
			appendStringInfoString(&tq->cx->out, ";\n");
		}
		/* otherwise a session option: ignored */
		tq->pos = e1;
		tq_eat_semi(tq);
		return;
	}
	name = tq_ident(v->s, v->len);
	tq->pos++;
	op = tq_cur(tq);
	if (op->kind == TQ_OP && op->len == 2 && op->s[1] == '=' &&
		strchr("+-*/%", op->s[0]))
		arith = op->s[0];
	else if (!(op->kind == TQ_OP && op->len == 1 && op->s[0] == '='))
		tq_err(tq, "expected = after SET @variable");
	tq->pos++;
	e0 = tq->pos;
	e1 = tq_value_end(tq, e0);
	if (e1 <= e0)
		tq_err(tq, "SET is missing a value expression");
	indent(&tq->cx->out, ind);
	if (arith)
	{
		appendStringInfo(&tq->cx->out, "%s := %s %c (", name, name, arith);
		tq_emit_range(tq, e0, e1, &tq->cx->out);
		appendStringInfoString(&tq->cx->out, ");\n");
	}
	else
	{
		appendStringInfo(&tq->cx->out, "%s :=", name);
		tq_emit_range(tq, e0, e1, &tq->cx->out);
		appendStringInfoString(&tq->cx->out, ";\n");
	}
	tq->pos = e1;
	tq_eat_semi(tq);
}

/* SELECT @x = e [, ...] [FROM ...] | SELECT ... (query) */
static void
tq_select(Tq *tq, int ind)
{
	int			save = tq->pos;

	tq->pos++;								/* SELECT */
	/* assignment form: first meaningful token is @v followed by = */
	if (tq_cur(tq)->kind == TQ_VAR &&
		tq->t[tq->pos + 1].kind == TQ_OP && tq->t[tq->pos + 1].len == 1 &&
		tq->t[tq->pos + 1].s[0] == '=')
	{
		char	   *names[64];
		int			estart[64],
					eend[64],
					nassign = 0;
		int			frompos = -1;

		for (;;)
		{
			TqTok	   *v = tq_cur(tq);
			int			depth = 0,
						e0,
						e1;

			if (v->kind != TQ_VAR || nassign >= 64)
				break;
			names[nassign] = tq_ident(v->s, v->len);
			tq->pos += 2;				/* @v = */
			e0 = tq->pos;
			e1 = e0;
			while (e1 < tq->nt)
			{
				TqTok	   *x = &tq->t[e1];

				if (x->kind == TQ_LP)
					depth++;
				else if (x->kind == TQ_RP && depth > 0)
					depth--;
				else if (depth == 0)
				{
					if (x->kind == TQ_COMMA || x->kind == TQ_SEMI ||
						x->kind == TQ_EOF || tq_ci(x, "END") || tq_ci(x, "ELSE"))
						break;
					if (tq_ci(x, "FROM") || tq_ci(x, "WHERE") ||
						tq_ci(x, "INTO") || tq_ci(x, "GROUP") ||
						tq_ci(x, "ORDER") || tq_ci(x, "HAVING"))
					{
						frompos = e1;
						break;
					}
				}
				e1++;
			}
			estart[nassign] = e0;
			eend[nassign] = e1;
			nassign++;
			tq->pos = e1;
			if (frompos >= 0)
				break;
			if (tq_cur(tq)->kind == TQ_COMMA)
			{
				tq->pos++;
				continue;
			}
			break;
		}
		if (frompos >= 0)
		{
			int			k,
						rend = tq_value_end(tq, frompos);

			indent(&tq->cx->out, ind);
			appendStringInfoString(&tq->cx->out, "SELECT");
			for (k = 0; k < nassign; k++)
			{
				if (k)
					appendStringInfoChar(&tq->cx->out, ',');
				tq_emit_range(tq, estart[k], eend[k], &tq->cx->out);
			}
			appendStringInfoString(&tq->cx->out, " INTO");
			for (k = 0; k < nassign; k++)
				appendStringInfo(&tq->cx->out, "%s %s", k ? "," : "", names[k]);
			tq_emit_range(tq, frompos, rend, &tq->cx->out);
			appendStringInfoString(&tq->cx->out, ";\n");
			tq->pos = rend;
		}
		else
		{
			int			k;

			for (k = 0; k < nassign; k++)
			{
				indent(&tq->cx->out, ind);
				appendStringInfo(&tq->cx->out, "%s :=", names[k]);
				tq_emit_range(tq, estart[k], eend[k], &tq->cx->out);
				appendStringInfoString(&tq->cx->out, ";\n");
			}
		}
		tq_eat_semi(tq);
		return;
	}

	/* a plain query: RETURN QUERY if set-returning, else PERFORM */
	{
		int			rend = tq_value_end(tq, save);

		indent(&tq->cx->out, ind);
		if (tq->cx->retset)
		{
			appendStringInfoString(&tq->cx->out, "RETURN QUERY SELECT");
			tq_emit_range(tq, save + 1, rend, &tq->cx->out);
		}
		else
		{
			appendStringInfoString(&tq->cx->out, "PERFORM");
			tq_emit_range(tq, save + 1, rend, &tq->cx->out);
		}
		appendStringInfoString(&tq->cx->out, ";\n");
		tq->pos = rend;
		tq_eat_semi(tq);
	}
}

/* IF cond body [ELSE body] */
static void
tq_if(Tq *tq, int ind)
{
	int			c0,
				c1;

	tq->pos++;								/* IF */
	c0 = tq->pos;
	c1 = tq_cond_end(tq, c0);
	indent(&tq->cx->out, ind);
	appendStringInfoString(&tq->cx->out, "IF");
	tq_emit_range(tq, c0, c1, &tq->cx->out);
	appendStringInfoString(&tq->cx->out, " THEN\n");
	tq->pos = c1;
	tq_body(tq, ind + 1);
	tq_eat_semi(tq);
	if (tq_ci(tq_cur(tq), "ELSE"))
	{
		tq->pos++;
		indent(&tq->cx->out, ind);
		appendStringInfoString(&tq->cx->out, "ELSE\n");
		tq_body(tq, ind + 1);
		tq_eat_semi(tq);
	}
	indent(&tq->cx->out, ind);
	appendStringInfoString(&tq->cx->out, "END IF;\n");
}

/* WHILE cond body */
static void
tq_while(Tq *tq, int ind)
{
	int			c0,
				c1;

	tq->pos++;								/* WHILE */
	c0 = tq->pos;
	c1 = tq_cond_end(tq, c0);
	indent(&tq->cx->out, ind);
	appendStringInfoString(&tq->cx->out, "WHILE");
	tq_emit_range(tq, c0, c1, &tq->cx->out);
	appendStringInfoString(&tq->cx->out, " LOOP\n");
	tq->pos = c1;
	tq->cx->loopdepth++;
	tq_body(tq, ind + 1);
	tq->cx->loopdepth--;
	tq_eat_semi(tq);
	indent(&tq->cx->out, ind);
	appendStringInfoString(&tq->cx->out, "END LOOP;\n");
}

/* PRINT e -> RAISE NOTICE '%', e; */
static void
tq_print(Tq *tq, int ind)
{
	int			e0,
				e1;

	tq->pos++;								/* PRINT */
	e0 = tq->pos;
	e1 = tq_value_end(tq, e0);
	indent(&tq->cx->out, ind);
	if (e1 > e0)
	{
		appendStringInfoString(&tq->cx->out, "RAISE NOTICE '%',");
		tq_emit_range(tq, e0, e1, &tq->cx->out);
		appendStringInfoString(&tq->cx->out, ";\n");
	}
	else
		appendStringInfoString(&tq->cx->out, "RAISE NOTICE '';\n");
	tq->pos = e1;
	tq_eat_semi(tq);
}

/* RETURN [e] */
static void
tq_return(Tq *tq, int ind)
{
	int			e0,
				e1;

	tq->pos++;								/* RETURN */
	e0 = tq->pos;
	e1 = tq_value_end(tq, e0);
	indent(&tq->cx->out, ind);
	if (e1 > e0)
	{
		appendStringInfoString(&tq->cx->out, "RETURN");
		tq_emit_range(tq, e0, e1, &tq->cx->out);
		appendStringInfoString(&tq->cx->out, ";\n");
	}
	else
		appendStringInfoString(&tq->cx->out, "RETURN;\n");
	tq->pos = e1;
	tq_eat_semi(tq);
}

/* the message argument of RAISERROR(msg, sev, state, ...) or THROW n, msg, s */
static void
tq_raise_msg(Tq *tq, int msg0, int msg1, int ind)
{
	indent(&tq->cx->out, ind);
	if (msg1 <= msg0)
	{
		appendStringInfoString(&tq->cx->out, "RAISE EXCEPTION 'error';\n");
		return;
	}
	appendStringInfoString(&tq->cx->out, "RAISE EXCEPTION '%',");
	tq_emit_range(tq, msg0, msg1, &tq->cx->out);
	appendStringInfoString(&tq->cx->out, ";\n");
}

/* RAISERROR ( msg , severity , state [, args] ) */
static void
tq_raiserror(Tq *tq, int ind)
{
	int			lp,
				close,
				depth = 0,
				comma = -1,
				j;

	tq->pos++;								/* RAISERROR */
	lp = tq->pos;
	if (tq_cur(tq)->kind != TQ_LP)
		tq_err(tq, "expected ( after RAISERROR");
	close = tq_match_lp(tq, lp, tq->nt);
	if (close < 0)
		tq_err(tq, "unbalanced ( in RAISERROR");
	for (j = lp + 1; j < close; j++)
	{
		if (tq->t[j].kind == TQ_LP)
			depth++;
		else if (tq->t[j].kind == TQ_RP)
			depth--;
		else if (tq->t[j].kind == TQ_COMMA && depth == 0)
		{
			comma = j;
			break;
		}
	}
	tq_raise_msg(tq, lp + 1, comma > 0 ? comma : close, ind);
	tq->pos = close + 1;
	tq_eat_semi(tq);
}

/* THROW [errnum, message, state] */
static void
tq_throw(Tq *tq, int ind)
{
	int			e0,
				e1;

	tq->pos++;								/* THROW */
	e0 = tq->pos;
	e1 = tq_value_end(tq, e0);
	if (e1 == e0)							/* bare THROW; re-raise */
	{
		indent(&tq->cx->out, ind);
		appendStringInfoString(&tq->cx->out, "RAISE;\n");
	}
	else
	{
		int			depth = 0,
					c1 = -1,
					c2 = -1,
					j;

		for (j = e0; j < e1; j++)
		{
			if (tq->t[j].kind == TQ_LP)
				depth++;
			else if (tq->t[j].kind == TQ_RP)
				depth--;
			else if (tq->t[j].kind == TQ_COMMA && depth == 0)
			{
				if (c1 < 0)
					c1 = j;
				else if (c2 < 0)
					c2 = j;
			}
		}
		if (c1 >= 0 && c2 >= 0)
			tq_raise_msg(tq, c1 + 1, c2, ind);
		else
			tq_raise_msg(tq, e0, e1, ind);
	}
	tq->pos = e1;
	tq_eat_semi(tq);
}

/* EXEC(sql) / EXECUTE(sql) -> EXECUTE sql; */
static void
tq_exec(Tq *tq, int ind)
{
	int			lp,
				close;

	tq->pos++;								/* EXEC / EXECUTE */
	lp = tq->pos;
	if (tq_cur(tq)->kind != TQ_LP)
		tq_err(tq, "EXEC of a stored procedure is not supported; use EXEC('<sql>') for dynamic SQL");
	close = tq_match_lp(tq, lp, tq->nt);
	if (close < 0)
		tq_err(tq, "unbalanced ( in EXEC");
	indent(&tq->cx->out, ind);
	appendStringInfoString(&tq->cx->out, "EXECUTE");
	tq_emit_range(tq, lp + 1, close, &tq->cx->out);
	appendStringInfoString(&tq->cx->out, ";\n");
	tq->pos = close + 1;
	tq_eat_semi(tq);
}

/* BEGIN TRY .. END TRY BEGIN CATCH .. END CATCH -> BEGIN .. EXCEPTION .. END */
static void
tq_try(Tq *tq, int ind)
{
	tq->pos += 2;							/* BEGIN TRY */
	indent(&tq->cx->out, ind);
	appendStringInfoString(&tq->cx->out, "BEGIN\n");
	while (!(tq_ci(tq_cur(tq), "END") && tq_ci(&tq->t[tq->pos + 1], "TRY")) &&
		   tq_cur(tq)->kind != TQ_EOF)
		tq_stmt(tq, ind + 1);
	if (tq_cur(tq)->kind == TQ_EOF)
		tq_err(tq, "missing END TRY");
	tq->pos += 2;							/* END TRY */
	tq_eat_semi(tq);
	if (!(tq_ci(tq_cur(tq), "BEGIN") && tq_ci(&tq->t[tq->pos + 1], "CATCH")))
		tq_err(tq, "expected BEGIN CATCH after END TRY");
	tq->pos += 2;							/* BEGIN CATCH */
	indent(&tq->cx->out, ind);
	appendStringInfoString(&tq->cx->out, "EXCEPTION WHEN OTHERS THEN\n");
	while (!(tq_ci(tq_cur(tq), "END") && tq_ci(&tq->t[tq->pos + 1], "CATCH")) &&
		   tq_cur(tq)->kind != TQ_EOF)
		tq_stmt(tq, ind + 1);
	if (tq_cur(tq)->kind == TQ_EOF)
		tq_err(tq, "missing END CATCH");
	tq->pos += 2;							/* END CATCH */
	tq_eat_semi(tq);
	indent(&tq->cx->out, ind);
	appendStringInfoString(&tq->cx->out, "END;\n");
}

/* a raw SQL statement (INSERT/UPDATE/DELETE/MERGE/...) passed through */
static void
tq_raw(Tq *tq, int ind)
{
	int			e0 = tq->pos,
				e1 = tq_value_end(tq, e0);

	/* an empty range means a stray terminator keyword (e.g. a dangling ELSE)
	 * reached statement position; error rather than spin without advancing */
	if (e1 == e0)
		tq_err(tq, "unexpected keyword in statement position");
	indent(&tq->cx->out, ind);
	tq_emit_range(tq, e0, e1, &tq->cx->out);
	appendStringInfoString(&tq->cx->out, ";\n");
	tq->pos = e1;
	tq_eat_semi(tq);
}

/* one statement */
static void
tq_stmt(Tq *tq, int ind)
{
	TqTok	   *tk = tq_cur(tq);

	if (++tq->cx->depth > PLX_MAX_DEPTH)
		tq_err(tq, "statement nested too deeply");

	if (tk->kind == TQ_SEMI)
	{
		tq->pos++;
		tq->cx->depth--;
		return;
	}
	if (tq_ci(tk, "DECLARE"))
		tq_declare(tq, ind);
	else if (tq_ci(tk, "SET"))
		tq_set(tq, ind);
	else if (tq_ci(tk, "SELECT"))
		tq_select(tq, ind);
	else if (tq_ci(tk, "IF"))
		tq_if(tq, ind);
	else if (tq_ci(tk, "WHILE"))
		tq_while(tq, ind);
	else if (tq_ci(tk, "PRINT"))
		tq_print(tq, ind);
	else if (tq_ci(tk, "RETURN"))
		tq_return(tq, ind);
	else if (tq_ci(tk, "RAISERROR"))
		tq_raiserror(tq, ind);
	else if (tq_ci(tk, "THROW"))
		tq_throw(tq, ind);
	else if (tq_ci(tk, "EXEC") || tq_ci(tk, "EXECUTE"))
		tq_exec(tq, ind);
	else if (tq_ci(tk, "BREAK"))
	{
		tq->pos++;
		indent(&tq->cx->out, ind);
		appendStringInfoString(&tq->cx->out, "EXIT;\n");
		tq_eat_semi(tq);
	}
	else if (tq_ci(tk, "CONTINUE"))
	{
		tq->pos++;
		indent(&tq->cx->out, ind);
		appendStringInfoString(&tq->cx->out, "CONTINUE;\n");
		tq_eat_semi(tq);
	}
	else if (tq_ci(tk, "BEGIN") && tq_ci(&tq->t[tq->pos + 1], "TRY"))
		tq_try(tq, ind);
	else if (tq_ci(tk, "BEGIN"))
	{
		/* a bare BEGIN..END grouping: parse the inner statements in place */
		if (tq_ci(&tq->t[tq->pos + 1], "TRAN") ||
			tq_ci(&tq->t[tq->pos + 1], "TRANSACTION"))
			tq_err(tq, "transaction control (BEGIN TRAN) is not allowed in a function");
		tq->pos++;
		tq_block(tq, ind);
		if (!tq_ci(tq_cur(tq), "END"))
			tq_err(tq, "missing END for BEGIN");
		tq->pos++;
		tq_eat_semi(tq);
	}
	else if (tq_ci(tk, "COMMIT") || tq_ci(tk, "ROLLBACK"))
		tq_err(tq, "transaction control is not allowed in a function");
	else if (tq_ci(tk, "GOTO") || tq_ci(tk, "WAITFOR"))
		tq_err(tq, "GOTO and WAITFOR are not supported");
	else
		tq_raw(tq, ind);

	tq->cx->depth--;
}

/* a body after IF/WHILE: a BEGIN..END block or a single statement */
static void
tq_body(Tq *tq, int ind)
{
	if (tq_ci(tq_cur(tq), "BEGIN") && !tq_ci(&tq->t[tq->pos + 1], "TRY"))
	{
		tq->pos++;							/* BEGIN */
		tq_block(tq, ind);
		if (!tq_ci(tq_cur(tq), "END"))
			tq_err(tq, "missing END for BEGIN");
		tq->pos++;
	}
	else
		tq_stmt(tq, ind);
}

/* statements until END or EOF (caller consumes END) */
static void
tq_block(Tq *tq, int ind)
{
	while (tq_cur(tq)->kind != TQ_EOF && !tq_ci(tq_cur(tq), "END"))
		tq_stmt(tq, ind);
}

static void
tsql_transpile(Ctx *cx)
{
	Tq			tq;

	memset(&tq, 0, sizeof(tq));
	tq.cx = cx;
	tq_lex(&tq, cx->body);
	tq_eat_semi(&tq);

	/* an outer BEGIN..END wrapper is unwrapped (its locals hoist anyway) */
	if (tq_ci(tq_cur(&tq), "BEGIN") &&
		!tq_ci(&tq.t[tq.pos + 1], "TRY") &&
		!tq_ci(&tq.t[tq.pos + 1], "TRAN") &&
		!tq_ci(&tq.t[tq.pos + 1], "TRANSACTION"))
	{
		int			save = tq.pos;

		tq.pos++;
		tq_block(&tq, 1);
		if (tq_ci(tq_cur(&tq), "END"))
		{
			tq.pos++;
			tq_eat_semi(&tq);
			if (tq_cur(&tq)->kind == TQ_EOF)
				return;
		}
		/* not a whole-body wrapper: reparse from the top as a statement list */
		tq.pos = save;
		cx->out.len = 0;
		cx->out.data[0] = '\0';
		cx->locals = cx->ltail = NULL;
	}
	while (tq_cur(&tq)->kind != TQ_EOF)
	{
		if (tq_ci(tq_cur(&tq), "END"))
			tq_err(&tq, "unexpected END");
		tq_stmt(&tq, 1);
	}
}

/* ======================================================================== */

/* ======================================================================== */
/* Go front end (plxgo).                                                    */
/*                                                                          */
/* Go is a C-family braced language but differs from plpgsql enough to need */
/* its own tokenizer and recursive-descent parser (own lexer with automatic */
/* semicolon insertion). It emits plpgsql directly, populating cx->locals + */
/* cx->out for the shared assemble path:                                    */
/*   - var x T [= e] / x := e (type inferred) / a, b := x, y -> DECLARE hoist*/
/*   - =, +=, ..., ++, -- assignment                                        */
/*   - if cond { } [else if] [else]  -> IF cond THEN ... END IF;            */
/*   - for {} / for cond {} / for i:=a; c; p {} / for _,v := range e {}      */
/*   - switch (tag and tagless) -> IF/ELSIF/ELSE                            */
/*   - return, break, continue, panic(); fmt.Println -> RAISE NOTICE        */
/*   - emit()/execute()/range query() SQL bridge; a stdlib + type library.  */
/* ======================================================================== */

typedef enum
{
	GO_EOF, GO_WORD, GO_NUM, GO_STR, GO_OP, GO_LP, GO_RP,
	GO_LBRACE, GO_RBRACE, GO_LBRACK, GO_RBRACK, GO_COMMA, GO_SEMI, GO_DOT
} GoKind;

typedef struct
{
	GoKind		kind;
	const char *s;
	int			len;
	int			line;
} GoTok;

typedef struct
{
	Ctx		   *cx;
	GoTok	   *t;
	int			nt, pos;
} Go;

static void go_stmt(Go *g, int ind);
static void go_block(Go *g, int ind);
static void go_emit_range(Go *g, int a, int b, StringInfo out);

static GoTok *
go_cur(Go *g)
{
	return &g->t[g->pos];
}

static bool
go_ci(GoTok *tk, const char *w)
{
	int			l = (int) strlen(w);

	/* Go keywords/identifiers are case-sensitive */
	return tk->kind == GO_WORD && tk->len == l && strncmp(tk->s, w, l) == 0;
}

pg_noreturn static void
go_err(Go *g, const char *msg)
{
	plx_err(g->cx, g->t[g->pos].line, "%s", msg);
}

static char *
go_ident(const char *s, int len)
{
	return pnstrdup(s, len);
}

static void
go_sp(StringInfo out)
{
	if (out->len && out->data[out->len - 1] != ' ' && out->data[out->len - 1] != '(')
		appendStringInfoChar(out, ' ');
}

/* token kind permits Go automatic-semicolon-insertion at a line break */
static bool
go_asi_kind(GoTok *tk)
{
	switch (tk->kind)
	{
		case GO_WORD:
		case GO_NUM:
		case GO_STR:
		case GO_RP:
		case GO_RBRACK:
		case GO_RBRACE:
			return true;
		case GO_OP:
			return (tk->len == 2 && (tk->s[0] == '+' || tk->s[0] == '-') &&
					tk->s[1] == tk->s[0]);		/* ++ or -- */
		default:
			return false;
	}
}

static void
go_lex(Go *g, const char *body)
{
	const char *p = body,
			   *end = body + strlen(body);
	int			line = 1,
				cap = 256,
				n = 0;
	GoTok	   *t = palloc(sizeof(GoTok) * cap);

#define GOPUSH(k, st, ln) do { \
		if (n >= cap) { cap *= 2; t = repalloc(t, sizeof(GoTok) * cap); } \
		t[n].kind = (k); t[n].s = (st); t[n].len = (ln); t[n].line = line; n++; \
	} while (0)
#define GOALPHA(c) (((c) >= 'A' && (c) <= 'Z') || ((c) >= 'a' && (c) <= 'z') || (c) == '_')
#define GODIGIT(c) ((c) >= '0' && (c) <= '9')

	while (p < end)
	{
		if (*p == '\n')
		{
			if (n > 0 && go_asi_kind(&t[n - 1]))		/* ASI */
			{
				GOPUSH(GO_SEMI, p, 1);
			}
			line++;
			p++;
			continue;
		}
		if (*p == ' ' || *p == '\t' || *p == '\r')
		{
			p++;
			continue;
		}
		if (p[0] == '/' && p + 1 < end && p[1] == '/')	/* // line comment */
		{
			while (p < end && *p != '\n')
				p++;
			continue;
		}
		if (p[0] == '/' && p + 1 < end && p[1] == '*')	/* block comment */
		{
			p += 2;
			while (p + 1 < end && !(p[0] == '*' && p[1] == '/'))
			{
				if (*p == '\n')
					line++;
				p++;
			}
			p += 2;
			continue;
		}
		if (*p == '"')									/* interpreted string */
		{
			const char *st = p;
			bool		closed = false;

			p++;
			while (p < end)
			{
				if (*p == '\\' && p + 1 < end)
				{
					p += 2;
					continue;
				}
				if (*p == '"')
				{
					p++;
					closed = true;
					break;
				}
				if (*p == '\n')
					break;
				p++;
			}
			if (!closed)
				plx_err(g->cx, line, "unterminated string literal");
			GOPUSH(GO_STR, st, (int) (p - st));
			continue;
		}
		if (*p == '`')									/* raw string */
		{
			const char *st = p;

			p++;
			while (p < end && *p != '`')
			{
				if (*p == '\n')
					line++;
				p++;
			}
			if (p >= end)
				plx_err(g->cx, line, "unterminated raw string literal");
			p++;
			GOPUSH(GO_STR, st, (int) (p - st));
			continue;
		}
		if (*p == '\'')									/* rune literal */
		{
			const char *st = p;

			p++;
			while (p < end && *p != '\'')
			{
				if (*p == '\\' && p + 1 < end)
					p++;
				p++;
			}
			if (p < end)
				p++;
			GOPUSH(GO_STR, st, (int) (p - st));		/* treated as a string */
			continue;
		}
		if (GOALPHA(*p))
		{
			const char *st = p;

			while (p < end && (GOALPHA(*p) || GODIGIT(*p)))
				p++;
			GOPUSH(GO_WORD, st, (int) (p - st));
			continue;
		}
		if (GODIGIT(*p))
		{
			const char *st = p;

			while (p < end && (GODIGIT(*p) || *p == '.' || *p == '_' ||
							   *p == 'x' || *p == 'X' ||
							   ((*p >= 'a' && *p <= 'f') || (*p >= 'A' && *p <= 'F')) ||
							   (( *p == '+' || *p == '-') && (p[-1] == 'e' || p[-1] == 'E'))))
				p++;
			GOPUSH(GO_NUM, st, (int) (p - st));
			continue;
		}
		if (*p == '(')
		{
			GOPUSH(GO_LP, p, 1);
			p++;
			continue;
		}
		if (*p == ')')
		{
			GOPUSH(GO_RP, p, 1);
			p++;
			continue;
		}
		if (*p == '{')
		{
			GOPUSH(GO_LBRACE, p, 1);
			p++;
			continue;
		}
		if (*p == '}')
		{
			GOPUSH(GO_RBRACE, p, 1);
			p++;
			continue;
		}
		if (*p == '[')
		{
			GOPUSH(GO_LBRACK, p, 1);
			p++;
			continue;
		}
		if (*p == ']')
		{
			GOPUSH(GO_RBRACK, p, 1);
			p++;
			continue;
		}
		if (*p == ',')
		{
			GOPUSH(GO_COMMA, p, 1);
			p++;
			continue;
		}
		if (*p == ';')
		{
			GOPUSH(GO_SEMI, p, 1);
			p++;
			continue;
		}
		if (*p == '.' && !(p + 1 < end && GODIGIT(p[1])))
		{
			if (p + 2 < end && p[1] == '.' && p[2] == '.')	/* ... */
			{
				GOPUSH(GO_OP, p, 3);
				p += 3;
			}
			else
			{
				GOPUSH(GO_DOT, p, 1);
				p++;
			}
			continue;
		}
		{
			static const char *ops3[] = {"&^=", "<<=", ">>=", "...", NULL};
			static const char *ops2[] = {
				":=", "==", "!=", "<=", ">=", "&&", "||", "+=", "-=",
				"*=", "/=", "%=", "++", "--", "<<", ">>", "&^", "<-",
				"&=", "|=", "^=", NULL
			};
			int			k;
			bool		matched = false;

			for (k = 0; ops3[k]; k++)
				if (p + 2 < end && strncmp(p, ops3[k], 3) == 0)
				{
					GOPUSH(GO_OP, ops3[k], 3);
					p += 3;
					matched = true;
					break;
				}
			if (matched)
				continue;
			for (k = 0; ops2[k]; k++)
				if (p + 1 < end && p[0] == ops2[k][0] && p[1] == ops2[k][1])
				{
					GOPUSH(GO_OP, ops2[k], 2);
					p += 2;
					matched = true;
					break;
				}
			if (matched)
				continue;
		}
		GOPUSH(GO_OP, p, 1);
		p++;
	}
	if (n > 0 && go_asi_kind(&t[n - 1]))				/* final ASI */
		GOPUSH(GO_SEMI, end, 0);
	GOPUSH(GO_EOF, end, 0);
	g->t = t;
	g->nt = n;
	g->pos = 0;
#undef GOPUSH
#undef GOALPHA
#undef GODIGIT
}

/* emit a Go string/rune literal as a SQL string literal */
static void
go_emit_str(GoTok *tk, StringInfo out)
{
	char		q = tk->s[0];
	const char *s = tk->s + 1,
			   *e = tk->s + tk->len - 1;

	appendStringInfoChar(out, '\'');
	if (q == '`')									/* raw: no escapes */
	{
		while (s < e)
		{
			if (*s == '\'')
				appendStringInfoString(out, "''");
			else
				appendStringInfoChar(out, *s);
			s++;
		}
	}
	else											/* "..." or '...': decode escapes */
	{
		while (s < e)
		{
			if (*s == '\\' && s + 1 < e)
			{
				s++;
				switch (*s)
				{
					case 'n': appendStringInfoChar(out, '\n'); break;
					case 't': appendStringInfoChar(out, '\t'); break;
					case 'r': appendStringInfoChar(out, '\r'); break;
					case '\\': appendStringInfoChar(out, '\\'); break;
					case '\'': appendStringInfoString(out, "''"); break;
					case '"': appendStringInfoChar(out, '"'); break;
					default: appendStringInfoChar(out, *s); break;
				}
				s++;
				continue;
			}
			if (*s == '\'')
				appendStringInfoString(out, "''");
			else
				appendStringInfoChar(out, *s);
			s++;
		}
	}
	appendStringInfoChar(out, '\'');
}

/* map a Go base type name to a PostgreSQL type; NULL if unknown */
static const char *
go_base_type(const char *s, int len)
{
	struct
	{
		const char *go;
		const char *pg;
	}			m[] = {
		{"int", "integer"}, {"int8", "smallint"}, {"int16", "smallint"},
		{"int32", "integer"}, {"int64", "bigint"}, {"rune", "integer"},
		{"uint", "bigint"}, {"uint8", "smallint"}, {"byte", "smallint"},
		{"uint16", "integer"}, {"uint32", "bigint"}, {"uint64", "bigint"},
		{"float32", "real"}, {"float64", "double precision"},
		{"string", "text"}, {"bool", "boolean"},
		{NULL, NULL}
	};
	int			i;

	for (i = 0; m[i].go; i++)
		if ((int) strlen(m[i].go) == len && strncmp(m[i].go, s, len) == 0)
			return m[i].pg;
	return NULL;
}

/* read a Go type reference from the stream into dest, consuming its tokens */
static void
go_read_type(Go *g, StringInfo dest)
{
	bool		slice = false;
	GoTok	   *w;
	const char *base;

	while (go_cur(g)->kind == GO_OP && go_cur(g)->len == 1 && go_cur(g)->s[0] == '*')
		g->pos++;								/* pointer: drop the * */
	if (go_cur(g)->kind == GO_LBRACK)			/* []T or [N]T -> T[] */
	{
		g->pos++;
		while (go_cur(g)->kind != GO_RBRACK && go_cur(g)->kind != GO_EOF)
			g->pos++;							/* skip an optional array length */
		if (go_cur(g)->kind == GO_RBRACK)
			g->pos++;
		slice = true;
	}
	w = go_cur(g);
	if (w->kind != GO_WORD)
		go_err(g, "expected a type name");
	if (go_ci(w, "map") || go_ci(w, "chan") || go_ci(w, "func") ||
		go_ci(w, "interface") || go_ci(w, "struct"))
		go_err(g, "map, chan, func, interface, and struct types are not supported");
	base = go_base_type(w->s, w->len);
	if (base)
		appendStringInfoString(dest, base);
	else if (g->t[g->pos + 1].kind == GO_DOT && go_ci(w, "time") &&
			 go_ci(&g->t[g->pos + 2], "Time"))
	{
		appendStringInfoString(dest, "timestamp");
		g->pos += 2;
	}
	else
		appendStringInfoString(dest, go_ident(w->s, w->len));	/* named/PG type */
	g->pos++;
	if (slice)
		appendStringInfoString(dest, "[]");
}

/* infer a PostgreSQL type from an initializer range [a,b); NULL if unknown */
static const char *
go_infer_type(Go *g, int a, int b)
{
	if (b - a == 1)
	{
		GoTok	   *tk = &g->t[a];

		if (tk->kind == GO_NUM)
		{
			int			i;

			for (i = 0; i < tk->len; i++)
				if (tk->s[i] == '.' ||
					((tk->s[i] == 'e' || tk->s[i] == 'E') && tk->len > 2 && tk->s[1] != 'x'))
					return "double precision";
			return "integer";
		}
		if (tk->kind == GO_STR)
			return "text";
		if (go_ci(tk, "true") || go_ci(tk, "false"))
			return "boolean";
		return NULL;
	}
	/* []T{...} array literal */
	if (g->t[a].kind == GO_LBRACK && g->t[a + 1].kind == GO_RBRACK &&
		g->t[a + 2].kind == GO_WORD)
	{
		const char *el = go_base_type(g->t[a + 2].s, g->t[a + 2].len);

		if (el)
			return psprintf("%s[]", el);
	}
	/* len(...) / cap(...) -> integer */
	if (go_ci(&g->t[a], "len") || go_ci(&g->t[a], "cap"))
		return "integer";
	/* a type conversion T(x) whose T is a base type */
	if (g->t[a].kind == GO_WORD && g->t[a + 1].kind == GO_LP)
	{
		const char *tc = go_base_type(g->t[a].s, g->t[a].len);

		if (tc)
			return tc;
	}
	return NULL;
}

/* index of the ')' / '}' / ']' matching the opener at idx, within [.,limit) */
static int
go_match(Go *g, int idx, int limit)
{
	int			depth = 0,
				i;

	for (i = idx; i < limit; i++)
	{
		GoKind		k = g->t[i].kind;

		if (k == GO_LP || k == GO_LBRACE || k == GO_LBRACK)
			depth++;
		else if (k == GO_RP || k == GO_RBRACE || k == GO_RBRACK)
			if (--depth == 0)
				return i;
	}
	return -1;
}

/* a stdlib / builtin / type-conversion call at index i; sets *ni on success */
static bool
go_emit_call(Go *g, int i, int b, StringInfo out, int *ni)
{
	GoTok	   *tk = &g->t[i];
	int			lp,
				close;
	const char *conv;

	/* qualified call: pkg.Method(...) */
	if (tk->kind == GO_WORD && i + 2 < b && g->t[i + 1].kind == GO_DOT &&
		g->t[i + 2].kind == GO_WORD && i + 3 < b && g->t[i + 3].kind == GO_LP)
	{
		GoTok	   *pkg = tk,
				   *meth = &g->t[i + 2];
		struct
		{
			const char *pkg;
			const char *meth;
			const char *pg;
		}			r[] = {
			{"strings", "ToUpper", "upper"}, {"strings", "ToLower", "lower"},
			{"strings", "TrimSpace", "btrim"}, {"strings", "ReplaceAll", "replace"},
			{"math", "Abs", "abs"}, {"math", "Floor", "floor"},
			{"math", "Ceil", "ceil"}, {"math", "Sqrt", "sqrt"},
			{"math", "Pow", "power"}, {"math", "Max", "greatest"},
			{"math", "Min", "least"}, {"math", "Mod", "mod"},
			{"strconv", "Itoa", NULL}, {"strconv", "Atoi", NULL},
			{"time", "Now", "now"}, {"fmt", "Sprintf", "format"},
			{NULL, NULL, NULL}
		};
		int			k;

		lp = i + 3;
		close = go_match(g, lp, b);
		if (close < 0)
			return false;
		for (k = 0; r[k].pkg; k++)
			if ((int) strlen(r[k].pkg) == pkg->len &&
				strncmp(r[k].pkg, pkg->s, pkg->len) == 0 &&
				(int) strlen(r[k].meth) == meth->len &&
				strncmp(r[k].meth, meth->s, meth->len) == 0)
			{
				/* strconv.Itoa(x) -> (x)::text ; strconv.Atoi(x) -> (x)::integer */
				if (r[k].pg == NULL)
				{
					bool		toint = (meth->len == 4);	/* "Atoi" */

					go_sp(out);
					appendStringInfoChar(out, '(');
					go_emit_range(g, lp + 1, close, out);
					appendStringInfo(out, ")::%s", toint ? "integer" : "text");
					*ni = close + 1;
					return true;
				}
				/* strings.Contains(s, sub) -> (strpos(s, sub) > 0) */
				go_sp(out);
				appendStringInfoString(out, r[k].pg);
				appendStringInfoChar(out, '(');
				go_emit_range(g, lp + 1, close, out);
				appendStringInfoChar(out, ')');
				*ni = close + 1;
				return true;
			}
		if (go_ci(pkg, "strings") && go_ci(meth, "Contains"))
		{
			int			depth = 0,
						comma = -1,
						j;

			for (j = lp + 1; j < close; j++)
			{
				if (g->t[j].kind == GO_LP)
					depth++;
				else if (g->t[j].kind == GO_RP)
					depth--;
				else if (g->t[j].kind == GO_COMMA && depth == 0 && comma < 0)
					comma = j;
			}
			if (comma < 0)
				return false;
			go_sp(out);
			appendStringInfoString(out, "(strpos(");
			go_emit_range(g, lp + 1, comma, out);
			appendStringInfoChar(out, ',');
			go_emit_range(g, comma + 1, close, out);
			appendStringInfoString(out, ") > 0)");
			*ni = close + 1;
			return true;
		}
		return false;
	}

	if (tk->kind != GO_WORD || i + 1 >= b || g->t[i + 1].kind != GO_LP)
		return false;
	lp = i + 1;
	close = go_match(g, lp, b);
	if (close < 0)
		return false;

	/* type conversion T(x) -> (x)::T for a base type T */
	conv = go_base_type(tk->s, tk->len);
	if (conv)
	{
		go_sp(out);
		appendStringInfoChar(out, '(');
		go_emit_range(g, lp + 1, close, out);
		appendStringInfo(out, ")::%s", conv);
		*ni = close + 1;
		return true;
	}
	/* len(x): element count for a slice local, else character length */
	if (go_ci(tk, "len"))
	{
		PlxLocal2  *a = (close == lp + 2 && g->t[lp + 1].kind == GO_WORD)
			? local_find(g->cx, g->t[lp + 1].s, g->t[lp + 1].len) : NULL;
		bool		isarr = a && a->typ && strlen(a->typ) > 2 &&
			strcmp(a->typ + strlen(a->typ) - 2, "[]") == 0;

		go_sp(out);
		appendStringInfoString(out, isarr ? "coalesce(cardinality(" : "length(");
		go_emit_range(g, lp + 1, close, out);
		appendStringInfoString(out, isarr ? "), 0)" : ")");
		*ni = close + 1;
		return true;
	}
	if (go_ci(tk, "append"))
	{
		go_sp(out);
		appendStringInfoString(out, "array_append(");
		go_emit_range(g, lp + 1, close, out);
		appendStringInfoChar(out, ')');
		*ni = close + 1;
		return true;
	}
	return false;
}

/* emit tokens [a,b) as a plpgsql expression */
static void
go_emit_range(Go *g, int a, int b, StringInfo out)
{
	int			i = a;

	while (i < b)
	{
		GoTok	   *tk = &g->t[i];
		int			ni;

		/* []T{ ... } array literal -> ARRAY[ ... ] */
		if (tk->kind == GO_LBRACK && i + 1 < b && g->t[i + 1].kind == GO_RBRACK &&
			i + 2 < b && g->t[i + 2].kind == GO_WORD &&
			i + 3 < b && g->t[i + 3].kind == GO_LBRACE)
		{
			int			close = go_match(g, i + 3, b);

			if (close > 0)
			{
				go_sp(out);
				appendStringInfoString(out, "ARRAY[");
				go_emit_range(g, i + 4, close, out);
				appendStringInfoChar(out, ']');
				i = close + 1;
				continue;
			}
		}
		if (tk->kind == GO_WORD)
		{
			if (go_emit_call(g, i, b, out, &ni))
			{
				i = ni;
				continue;
			}
			if (go_ci(tk, "nil"))
			{
				go_sp(out);
				appendStringInfoString(out, "NULL");
				i++;
				continue;
			}
			go_sp(out);
			appendBinaryStringInfo(out, tk->s, tk->len);
			i++;
			continue;
		}
		if (tk->kind == GO_NUM)
		{
			go_sp(out);
			appendBinaryStringInfo(out, tk->s, tk->len);
			i++;
			continue;
		}
		if (tk->kind == GO_STR)
		{
			go_sp(out);
			go_emit_str(tk, out);
			i++;
			continue;
		}
		if (tk->kind == GO_LP)
		{
			go_sp(out);
			appendStringInfoChar(out, '(');
			i++;
			continue;
		}
		if (tk->kind == GO_RP)
		{
			appendStringInfoChar(out, ')');
			i++;
			continue;
		}
		if (tk->kind == GO_LBRACK)
		{
			/*
			 * A subscript a[e] on a value: Go slices are 0-based but PostgreSQL
			 * arrays are 1-based, so emit a[(e) + 1]. (An empty [] is a slice
			 * literal, handled above; a [ opening at the very start of a range
			 * is not a subscript.)
			 */
			if (i > a && i + 1 < b && g->t[i + 1].kind != GO_RBRACK &&
				(g->t[i - 1].kind == GO_WORD || g->t[i - 1].kind == GO_RP ||
				 g->t[i - 1].kind == GO_RBRACK))
			{
				int			close = go_match(g, i, b);

				if (close > 0)
				{
					appendStringInfoString(out, "[(");
					go_emit_range(g, i + 1, close, out);
					appendStringInfoString(out, ") + 1]");
					i = close + 1;
					continue;
				}
			}
			appendStringInfoChar(out, '[');
			i++;
			continue;
		}
		if (tk->kind == GO_RBRACK)
		{
			appendStringInfoChar(out, ']');
			i++;
			continue;
		}
		if (tk->kind == GO_COMMA)
		{
			appendStringInfoChar(out, ',');
			i++;
			continue;
		}
		if (tk->kind == GO_DOT)
		{
			appendStringInfoChar(out, '.');
			i++;
			continue;
		}
		if (tk->kind == GO_OP)
		{
			if (tk->len == 2 && tk->s[0] == '=' && tk->s[1] == '=')
			{
				go_sp(out);
				appendStringInfoChar(out, '=');
			}
			else if (tk->len == 2 && tk->s[0] == '!' && tk->s[1] == '=')
			{
				go_sp(out);
				appendStringInfoString(out, "<>");
			}
			else if (tk->len == 2 && tk->s[0] == '&' && tk->s[1] == '&')
			{
				go_sp(out);
				appendStringInfoString(out, "AND");
			}
			else if (tk->len == 2 && tk->s[0] == '|' && tk->s[1] == '|')
			{
				go_sp(out);
				appendStringInfoString(out, "OR");
			}
			else if (tk->len == 1 && tk->s[0] == '!')
			{
				go_sp(out);
				appendStringInfoString(out, "NOT ");
			}
			else
			{
				go_sp(out);
				appendBinaryStringInfo(out, tk->s, tk->len);
			}
			i++;
			continue;
		}
		i++;
	}
}

/* end of a simple statement: the next top-level ; (brackets/braces nest) */
static int
go_stmt_end(Go *g, int from)
{
	int			i = from,
				depth = 0;

	while (i < g->nt)
	{
		GoKind		k = g->t[i].kind;

		if (k == GO_LP || k == GO_LBRACE || k == GO_LBRACK)
			depth++;
		else if (k == GO_RP || k == GO_RBRACE || k == GO_RBRACK)
		{
			if (depth == 0)
				return i;
			depth--;
		}
		else if (depth == 0 && (k == GO_SEMI || k == GO_EOF))
			return i;
		i++;
	}
	return g->nt - 1;
}

/* end of an if/for/switch header: the first top-level { that opens the body */
static int
go_header_end(Go *g, int from)
{
	int			i = from,
				depth = 0;

	while (i < g->nt)
	{
		GoKind		k = g->t[i].kind;

		if (k == GO_LP || k == GO_LBRACK)
			depth++;
		else if (k == GO_RP || k == GO_RBRACK)
			depth--;
		else if (depth == 0 && k == GO_LBRACE)
			return i;
		else if (depth == 0 && k == GO_EOF)
			return i;
		i++;
	}
	return g->nt - 1;
}

static void
go_eat_semi(Go *g)
{
	while (go_cur(g)->kind == GO_SEMI)
		g->pos++;
}

/* declare a name with an inferred/explicit type and optional initializer */
static void
go_declare_one(Go *g, const char *name, StringInfo typ, int e0, int e1, bool isconst, int ind)
{
	PlxLocal2  *l = local_add(g->cx, name, (int) strlen(name));

	if (typ && typ->len)
		l->typ = pstrdup(typ->data);
	else if (e1 > e0)
	{
		const char *inf = go_infer_type(g, e0, e1);

		if (!inf)
			plx_err(g->cx, g->t[e0].line,
					"cannot infer a PostgreSQL type for \"%s\"; declare it with an explicit type (var %s T)",
					name, name);
		l->typ = pstrdup(inf);
	}
	/* a const with no initializer would leave l->init NULL (invalid) */
	if (isconst && e1 <= e0)
		plx_err(g->cx, g->t[e0 < g->nt ? e0 : g->nt - 1].line,
				"a constant needs a value (const %s = ...)", name);
	l->is_const = isconst;
	if (e1 > e0)
	{
		if (isconst)
		{
			/* a constant's value lives in the DECLARE, not a runtime assignment */
			StringInfoData v;

			initStringInfo(&v);
			go_emit_range(g, e0, e1, &v);
			l->init = v.data;
		}
		else
		{
			indent(&g->cx->out, ind);
			appendStringInfo(&g->cx->out, "%s :=", name);
			go_emit_range(g, e0, e1, &g->cx->out);
			appendStringInfoString(&g->cx->out, ";\n");
		}
	}
}

/* var x T [= e]  |  var x = e  |  var ( ... )  |  const ... */
static void
go_var(Go *g, int ind, bool isconst)
{
	g->pos++;								/* var / const */
	if (go_cur(g)->kind == GO_LP)			/* grouped var ( ... ) */
	{
		g->pos++;
		go_eat_semi(g);
		while (go_cur(g)->kind != GO_RP && go_cur(g)->kind != GO_EOF)
		{
			GoTok	   *nm = go_cur(g);
			StringInfoData ty;
			int			e0 = -1,
						e1 = -1;

			if (nm->kind != GO_WORD)
				go_err(g, "expected a name in var block");
			g->pos++;
			initStringInfo(&ty);
			if (go_cur(g)->kind != GO_OP || go_cur(g)->s[0] != '=')
				go_read_type(g, &ty);
			if (go_cur(g)->kind == GO_OP && go_cur(g)->len == 1 && go_cur(g)->s[0] == '=')
			{
				g->pos++;
				e0 = g->pos;
				e1 = go_stmt_end(g, e0);
				g->pos = e1;
			}
			go_declare_one(g, go_ident(nm->s, nm->len), &ty, e0 < 0 ? 0 : e0,
						   e1 < 0 ? 0 : e1, isconst, ind);
			go_eat_semi(g);
		}
		if (go_cur(g)->kind == GO_RP)
			g->pos++;
		go_eat_semi(g);
		return;
	}
	{
		GoTok	   *nm = go_cur(g);
		StringInfoData ty;
		int			e0 = 0,
					e1 = 0;

		if (nm->kind != GO_WORD)
			go_err(g, "expected a name after var");
		g->pos++;
		initStringInfo(&ty);
		if (!(go_cur(g)->kind == GO_OP && go_cur(g)->len == 1 && go_cur(g)->s[0] == '='))
			go_read_type(g, &ty);
		if (go_cur(g)->kind == GO_OP && go_cur(g)->len == 1 && go_cur(g)->s[0] == '=')
		{
			g->pos++;
			e0 = g->pos;
			e1 = go_stmt_end(g, e0);
			g->pos = e1;
		}
		go_declare_one(g, go_ident(nm->s, nm->len), &ty, e0, e1, isconst, ind);
		go_eat_semi(g);
	}
}

/* an assignment, short declaration, ++/--, or expression statement */
static void
go_simple(Go *g, int ind, int stop)
{
	int			e0 = g->pos,
				e1 = (stop >= 0) ? stop : go_stmt_end(g, e0);
	int			i,
				depth = 0,
				opidx = -1;
	char		opc = 0;
	bool		shortdecl = false;

	/* an empty statement range means a stray closer/terminator (e.g. ')' or ']')
	 * reached statement position; error rather than spin without advancing */
	if (e1 <= e0)
		go_err(g, "unexpected token");

	/* find the top-level assignment operator, if any (each is a single token) */
	for (i = e0; i < e1; i++)
	{
		GoKind		k = g->t[i].kind;

		if (k == GO_LP || k == GO_LBRACE || k == GO_LBRACK)
			depth++;
		else if (k == GO_RP || k == GO_RBRACE || k == GO_RBRACK)
			depth--;
		else if (depth == 0 && k == GO_OP)
		{
			GoTok	   *o = &g->t[i];

			if (o->len == 2 && o->s[0] == ':' && o->s[1] == '=')
			{
				opidx = i;
				shortdecl = true;
				break;
			}
			if (o->len == 1 && o->s[0] == '=')
			{
				opidx = i;
				break;
			}
			if (o->len == 2 && o->s[1] == '=' && strchr("+-*/%", o->s[0]))
			{
				opidx = i;
				opc = o->s[0];
				break;
			}
		}
	}

	/* ++ / -- */
	if (opidx < 0 && e1 - e0 >= 2 && g->t[e1 - 1].kind == GO_OP &&
		g->t[e1 - 1].len == 2 &&
		(g->t[e1 - 1].s[0] == '+' || g->t[e1 - 1].s[0] == '-') &&
		g->t[e1 - 1].s[1] == g->t[e1 - 1].s[0])
	{
		indent(&g->cx->out, ind);
		go_emit_range(g, e0, e1 - 1, &g->cx->out);
		appendStringInfoString(&g->cx->out, " := ");
		go_emit_range(g, e0, e1 - 1, &g->cx->out);
		appendStringInfo(&g->cx->out, " %c 1;\n", g->t[e1 - 1].s[0]);
		g->pos = e1;
		return;
	}

	if (opidx < 0)
	{
		/* an expression statement: a call for its effect */
		GoTok	   *h = &g->t[e0];

		indent(&g->cx->out, ind);
		if (go_ci(h, "panic"))
		{
			int			lp = e0 + 1,
						close = (g->t[lp].kind == GO_LP) ? go_match(g, lp, e1) : -1;

			if (close > lp + 1)
			{
				appendStringInfoString(&g->cx->out, "RAISE EXCEPTION '%',");
				go_emit_range(g, lp + 1, close, &g->cx->out);
				appendStringInfoString(&g->cx->out, ";\n");
			}
			else
				appendStringInfoString(&g->cx->out, "RAISE EXCEPTION 'panic';\n");
		}
		else if (go_ci(h, "fmt") && g->t[e0 + 1].kind == GO_DOT)
		{
			GoTok	   *m = &g->t[e0 + 2];
			int			lp = e0 + 3,
						close = (lp < g->nt && g->t[lp].kind == GO_LP)
			? go_match(g, lp, e1) : -1;
			int			args0 = lp + 1;

			/* Printf/Sprintf: the first argument is the format string; the rest
			 * are the values to raise (SQL RAISE has no printf verbs, so the
			 * format's literal text and directives are dropped) */
			if (close > lp + 1 && (go_ci(m, "Printf") || go_ci(m, "Sprintf")))
			{
				int			d = 0,
							comma = -1,
							j;

				for (j = lp + 1; j < close; j++)
				{
					if (g->t[j].kind == GO_LP)
						d++;
					else if (g->t[j].kind == GO_RP)
						d--;
					else if (g->t[j].kind == GO_COMMA && d == 0 && comma < 0)
						comma = j;
				}
				if (comma > 0)
					args0 = comma + 1;
			}
			if (close > args0)
			{
				/* one % placeholder per top-level argument */
				int			d = 0,
							nargs = 1,
							j,
							k;

				for (j = args0; j < close; j++)
				{
					GoKind		kk = g->t[j].kind;

					if (kk == GO_LP || kk == GO_LBRACK || kk == GO_LBRACE)
						d++;
					else if (kk == GO_RP || kk == GO_RBRACK || kk == GO_RBRACE)
						d--;
					else if (kk == GO_COMMA && d == 0)
						nargs++;
				}
				appendStringInfoString(&g->cx->out, "RAISE NOTICE '");
				for (k = 0; k < nargs; k++)
					appendStringInfoString(&g->cx->out, k ? " %" : "%");
				appendStringInfoString(&g->cx->out, "',");
				go_emit_range(g, args0, close, &g->cx->out);
				appendStringInfoString(&g->cx->out, ";\n");
			}
			else
				appendStringInfoString(&g->cx->out, "RAISE NOTICE '';\n");
		}
		else if (go_ci(h, "emit"))
		{
			int			lp = e0 + 1,
						close = (g->t[lp].kind == GO_LP) ? go_match(g, lp, e1) : -1;

			g->cx->retset = true;
			if (close > lp + 1)
			{
				appendStringInfoString(&g->cx->out, "RETURN NEXT");
				go_emit_range(g, lp + 1, close, &g->cx->out);
				appendStringInfoString(&g->cx->out, ";\n");
			}
			else
				appendStringInfoString(&g->cx->out, "RETURN NEXT;\n");
		}
		else if (go_ci(h, "execute") || go_ci(h, "perform"))
		{
			int			lp = e0 + 1,
						close = (g->t[lp].kind == GO_LP) ? go_match(g, lp, e1) : -1;

			appendStringInfoString(&g->cx->out,
								   go_ci(h, "execute") ? "EXECUTE" : "PERFORM");
			if (close > 0)
				go_emit_range(g, lp + 1, close, &g->cx->out);
			appendStringInfoString(&g->cx->out, ";\n");
		}
		else
		{
			appendStringInfoString(&g->cx->out, "PERFORM");
			go_emit_range(g, e0, e1, &g->cx->out);
			appendStringInfoString(&g->cx->out, ";\n");
		}
		g->pos = e1;
		return;
	}

	/* short declaration: declare each new name (types inferred) */
	if (shortdecl)
	{
		int			names[32],
					nnames = 0,
					j,
					d = 0;

		for (j = e0; j < opidx; j++)
		{
			if (g->t[j].kind == GO_WORD && nnames < 32)
				names[nnames++] = j;
		}
		/* single target: infer from the whole RHS */
		if (nnames == 1)
		{
			char	   *nm = go_ident(g->t[names[0]].s, g->t[names[0]].len);

			if (!local_find(g->cx, nm, (int) strlen(nm)))
				go_declare_one(g, nm, NULL, opidx + 1, e1, false, ind);
			else
			{
				indent(&g->cx->out, ind);
				appendStringInfo(&g->cx->out, "%s :=", nm);
				go_emit_range(g, opidx + 1, e1, &g->cx->out);
				appendStringInfoString(&g->cx->out, ";\n");
			}
			g->pos = e1;
			return;
		}
		/* multiple targets a, b := x, y : split RHS on top-level commas.
		 * rhs holds (start,end) pairs, so it needs 2 ints per target. */
		{
			int			rhs[64],
						nrhs = 0,
						start = opidx + 1;

			d = 0;
			for (j = opidx + 1; j <= e1; j++)
			{
				GoKind		k = (j < e1) ? g->t[j].kind : GO_COMMA;

				if (k == GO_LP || k == GO_LBRACE || k == GO_LBRACK)
					d++;
				else if (k == GO_RP || k == GO_RBRACE || k == GO_RBRACK)
					d--;
				else if (k == GO_COMMA && d == 0 && nrhs < 32)
				{
					rhs[nrhs * 2] = start;
					rhs[nrhs * 2 + 1] = j;
					nrhs++;
					start = j + 1;
				}
			}
			for (j = 0; j < nnames && j < nrhs; j++)
			{
				char	   *nm = go_ident(g->t[names[j]].s, g->t[names[j]].len);

				if (!local_find(g->cx, nm, (int) strlen(nm)))
					go_declare_one(g, nm, NULL, rhs[j * 2], rhs[j * 2 + 1], false, ind);
				else
				{
					indent(&g->cx->out, ind);
					appendStringInfo(&g->cx->out, "%s :=", nm);
					go_emit_range(g, rhs[j * 2], rhs[j * 2 + 1], &g->cx->out);
					appendStringInfoString(&g->cx->out, ";\n");
				}
			}
		}
		g->pos = e1;
		return;
	}

	/* parallel assignment a, b = x, y : SELECT x, y INTO a, b (RHS eval first) */
	if (!opc)
	{
		int			d = 0,
					j,
					commas = 0;

		for (j = e0; j < opidx; j++)
		{
			GoKind		k = g->t[j].kind;

			if (k == GO_LP || k == GO_LBRACK || k == GO_LBRACE)
				d++;
			else if (k == GO_RP || k == GO_RBRACK || k == GO_RBRACE)
				d--;
			else if (k == GO_COMMA && d == 0)
				commas++;
		}
		if (commas > 0)
		{
			indent(&g->cx->out, ind);
			appendStringInfoString(&g->cx->out, "SELECT");
			go_emit_range(g, opidx + 1, e1, &g->cx->out);
			appendStringInfoString(&g->cx->out, " INTO");
			go_emit_range(g, e0, opidx, &g->cx->out);
			appendStringInfoString(&g->cx->out, ";\n");
			g->pos = e1;
			return;
		}
	}

	/* plain assignment: LHS <op> RHS */
	indent(&g->cx->out, ind);
	go_emit_range(g, e0, opidx, &g->cx->out);
	appendStringInfoString(&g->cx->out, " :=");
	if (opc)
	{
		go_emit_range(g, e0, opidx, &g->cx->out);
		appendStringInfo(&g->cx->out, " %c (", opc);
		go_emit_range(g, opidx + 1, e1, &g->cx->out);
		appendStringInfoString(&g->cx->out, ");\n");
	}
	else
	{
		go_emit_range(g, opidx + 1, e1, &g->cx->out);
		appendStringInfoString(&g->cx->out, ";\n");
	}
	g->pos = e1;
}

/* if [init;] cond { } [else if ...] [else { }] */
static void
go_if(Go *g, int ind)
{
	int			hb,
				c0,
				semi = -1,
				i,
				depth = 0;

	/* guard the else-if chain, which recurses into go_if directly (bypassing
	 * go_stmt's depth counter) */
	if (++g->cx->depth > PLX_MAX_DEPTH)
		go_err(g, "if/else nested too deeply");

	g->pos++;								/* if */
	hb = go_header_end(g, g->pos);
	/* an optional init statement precedes a top-level ; in the header */
	for (i = g->pos; i < hb; i++)
	{
		GoKind		k = g->t[i].kind;

		if (k == GO_LP || k == GO_LBRACK)
			depth++;
		else if (k == GO_RP || k == GO_RBRACK)
			depth--;
		else if (depth == 0 && k == GO_SEMI)
			semi = i;
	}
	if (semi >= 0)
	{
		go_simple(g, ind, semi);
		g->pos = semi + 1;
	}
	c0 = g->pos;
	indent(&g->cx->out, ind);
	appendStringInfoString(&g->cx->out, "IF");
	go_emit_range(g, c0, hb, &g->cx->out);
	appendStringInfoString(&g->cx->out, " THEN\n");
	g->pos = hb;
	go_block(g, ind + 1);
	go_eat_semi(g);
	if (go_ci(go_cur(g), "else"))
	{
		g->pos++;
		if (go_ci(go_cur(g), "if"))
		{
			indent(&g->cx->out, ind);
			appendStringInfoString(&g->cx->out, "ELSE\n");
			go_if(g, ind + 1);
		}
		else
		{
			indent(&g->cx->out, ind);
			appendStringInfoString(&g->cx->out, "ELSE\n");
			go_block(g, ind + 1);
		}
		go_eat_semi(g);
	}
	indent(&g->cx->out, ind);
	appendStringInfoString(&g->cx->out, "END IF;\n");
	g->cx->depth--;
}

/* for { } | for cond { } | for init; cond; post { } | for k,v := range e { } */
static void
go_for(Go *g, int ind)
{
	int			hb,
				h0,
				semi1 = -1,
				semi2 = -1,
				i,
				depth = 0,
				rangepos = -1;

	g->pos++;								/* for */
	h0 = g->pos;
	hb = go_header_end(g, g->pos);
	for (i = h0; i < hb; i++)
	{
		GoKind		k = g->t[i].kind;

		if (k == GO_LP || k == GO_LBRACK || k == GO_LBRACE)
			depth++;
		else if (k == GO_RP || k == GO_RBRACK || k == GO_RBRACE)
			depth--;
		else if (depth == 0 && k == GO_SEMI)
		{
			if (semi1 < 0)
				semi1 = i;
			else
				semi2 = i;
		}
		else if (depth == 0 && go_ci(&g->t[i], "range"))
			rangepos = i;
	}

	/* for k, v := range e  -> FOREACH / FOR ... IN */
	if (rangepos >= 0)
	{
		int			assign = -1,
					j;
		int			rs = rangepos + 1;		/* the range expression start */
		char	   *valname = NULL,
				   *idxname = NULL;

		for (j = h0; j < rangepos; j++)
			if (g->t[j].kind == GO_OP &&
				((g->t[j].len == 2 && g->t[j].s[0] == ':' && g->t[j].s[1] == '=') ||
				 (g->t[j].len == 1 && g->t[j].s[0] == '=')))
				assign = j;
		if (assign > h0)
		{
			/* names between h0 and assign: one (value) or two (index, value) */
			int			nm[4],
						nn = 0;

			for (j = h0; j < assign; j++)
				if (g->t[j].kind == GO_WORD && nn < 4)
					nm[nn++] = j;
			/* Go: a single var is the index; two vars are (index, value) */
			if (nn == 1)
			{
				if (!(g->t[nm[0]].len == 1 && g->t[nm[0]].s[0] == '_'))
					idxname = go_ident(g->t[nm[0]].s, g->t[nm[0]].len);
			}
			else if (nn >= 2)
			{
				if (!(g->t[nm[0]].len == 1 && g->t[nm[0]].s[0] == '_'))
					idxname = go_ident(g->t[nm[0]].s, g->t[nm[0]].len);
				if (!(g->t[nm[1]].len == 1 && g->t[nm[1]].s[0] == '_'))
					valname = go_ident(g->t[nm[1]].s, g->t[nm[1]].len);
			}
		}
		/* range query("...") -> FOR row IN <sql> LOOP */
		if (go_ci(&g->t[rs], "query") && g->t[rs + 1].kind == GO_LP)
		{
			int			close = go_match(g, rs + 1, hb);
			char	   *rowname = valname ? valname : idxname;

			if (!rowname)
				go_err(g, "range over query() needs a row variable (for _, row := range query(...))");
			if (!local_find(g->cx, rowname, (int) strlen(rowname)))
			{
				PlxLocal2  *l = local_add(g->cx, rowname, (int) strlen(rowname));

				l->is_record = true;
			}
			indent(&g->cx->out, ind);
			appendStringInfo(&g->cx->out, "FOR %s IN EXECUTE", rowname);
			go_emit_range(g, rs + 2, close, &g->cx->out);
			appendStringInfoString(&g->cx->out, " LOOP\n");
			g->pos = hb;
			g->cx->loopdepth++;
			go_block(g, ind + 1);
			g->cx->loopdepth--;
			indent(&g->cx->out, ind);
			appendStringInfoString(&g->cx->out, "END LOOP;\n");
			go_eat_semi(g);
			return;
		}
		/* both index and value: FOREACH exposes only the value, so reject */
		if (idxname && valname)
			go_err(g, "for index, value := range is not supported; use for _, v := range (value) or for i := range (index)");
		/* single var: the index -> FOR i IN 0 .. n-1 (Go 1.22 integer range) */
		if (idxname)
		{
			if (!local_find(g->cx, idxname, (int) strlen(idxname)))
			{
				PlxLocal2  *l = local_add(g->cx, idxname, (int) strlen(idxname));

				l->typ = pstrdup("integer");
			}
			indent(&g->cx->out, ind);
			appendStringInfo(&g->cx->out, "FOR %s IN 0 .. (", idxname);
			go_emit_range(g, rs, hb, &g->cx->out);
			appendStringInfoString(&g->cx->out, ") - 1 LOOP\n");
			g->pos = hb;
			g->cx->loopdepth++;
			go_block(g, ind + 1);
			g->cx->loopdepth--;
			indent(&g->cx->out, ind);
			appendStringInfoString(&g->cx->out, "END LOOP;\n");
			go_eat_semi(g);
			return;
		}
		/* range over a slice -> FOREACH v IN ARRAY e */
		if (!valname)
			go_err(g, "range needs a value variable (for _, v := range s)");
		if (!local_find(g->cx, valname, (int) strlen(valname)))
		{
			/* infer the element type from a plain slice local (var s []T) */
			PlxLocal2  *arr = (rs + 1 == hb && g->t[rs].kind == GO_WORD)
				? local_find(g->cx, g->t[rs].s, g->t[rs].len) : NULL;

			if (arr && arr->typ)
			{
				int			tl = (int) strlen(arr->typ);

				if (tl > 2 && strcmp(arr->typ + tl - 2, "[]") == 0)
				{
					PlxLocal2  *l = local_add(g->cx, valname, (int) strlen(valname));

					l->typ = pnstrdup(arr->typ, tl - 2);
				}
				else
					go_err(g, "declare the range value variable with var before the loop");
			}
			else
				go_err(g, "declare the range value variable with var before the loop");
		}
		indent(&g->cx->out, ind);
		appendStringInfo(&g->cx->out, "FOREACH %s IN ARRAY", valname);
		go_emit_range(g, rs, hb, &g->cx->out);
		appendStringInfoString(&g->cx->out, " LOOP\n");
		g->pos = hb;
		g->cx->loopdepth++;
		go_block(g, ind + 1);
		g->cx->loopdepth--;
		indent(&g->cx->out, ind);
		appendStringInfoString(&g->cx->out, "END LOOP;\n");
		go_eat_semi(g);
		return;
	}

	/* for init; cond; post { } */
	if (semi1 >= 0 && semi2 >= 0)
	{
		/*
		 * The canonical counting loop -- for i := A; i < B; i++  (or <=, or
		 * i += S) -- becomes a plpgsql integer FOR. This is not just tidier:
		 * plpgsql CONTINUE jumps to the loop's own increment, whereas in a
		 * hand-built WHILE it would skip the post statement and loop forever.
		 */
		int			ivar = -1;
		bool		inclusive = false;
		char		dir = 0;			/* 'u' counts up (++/+=), 'd' down (--/-=) */
		char		condc = 0;			/* comparison direction: '<' or '>' */
		int			step0 = -1,
					step1 = -1;

		/* init: <var> :=|= <A> */
		if (g->t[h0].kind == GO_WORD && h0 + 2 <= semi1 &&
			g->t[h0 + 1].kind == GO_OP &&
			((g->t[h0 + 1].len == 2 && g->t[h0 + 1].s[0] == ':' && g->t[h0 + 1].s[1] == '=') ||
			 (g->t[h0 + 1].len == 1 && g->t[h0 + 1].s[0] == '=')))
			ivar = h0;
		/* post: <var>++ / <var>-- / <var> += S / <var> -= S sets the direction */
		if (ivar >= 0 && g->t[semi2 + 1].kind == GO_WORD &&
			g->t[semi2 + 1].len == g->t[ivar].len &&
			strncmp(g->t[semi2 + 1].s, g->t[ivar].s, g->t[ivar].len) == 0 &&
			g->t[semi2 + 2].kind == GO_OP && g->t[semi2 + 2].len == 2)
		{
			GoTok	   *po = &g->t[semi2 + 2];

			if (po->s[0] == '+' && po->s[1] == '+')
				dir = 'u';
			else if (po->s[0] == '-' && po->s[1] == '-')
				dir = 'd';
			else if (po->s[0] == '+' && po->s[1] == '=')
			{
				dir = 'u';
				step0 = semi2 + 3;
				step1 = hb;
			}
			else if (po->s[0] == '-' && po->s[1] == '=')
			{
				dir = 'd';
				step0 = semi2 + 3;
				step1 = hb;
			}
		}
		if (!dir)
			ivar = -1;
		/* cond: <var> </<= (up) or >/>= (down), comparison immediately after var */
		if (ivar >= 0 && g->t[semi1 + 1].kind == GO_WORD &&
			g->t[semi1 + 1].len == g->t[ivar].len &&
			strncmp(g->t[semi1 + 1].s, g->t[ivar].s, g->t[ivar].len) == 0 &&
			g->t[semi1 + 2].kind == GO_OP &&
			(g->t[semi1 + 2].s[0] == '<' || g->t[semi1 + 2].s[0] == '>') &&
			(g->t[semi1 + 2].len == 1 ||
			 (g->t[semi1 + 2].len == 2 && g->t[semi1 + 2].s[1] == '=')))
		{
			condc = g->t[semi1 + 2].s[0];
			inclusive = (g->t[semi1 + 2].len == 2);
		}
		else
			ivar = -1;
		/* the comparison must agree with the increment direction */
		if (ivar >= 0 && ((dir == 'u' && condc != '<') || (dir == 'd' && condc != '>')))
			ivar = -1;

		if (ivar >= 0)
		{
			indent(&g->cx->out, ind);
			appendStringInfo(&g->cx->out, "FOR %s IN%s", go_ident(g->t[ivar].s, g->t[ivar].len),
							 dir == 'd' ? " REVERSE" : "");
			go_emit_range(g, ivar + 2, semi1, &g->cx->out);		/* A (loop start) */
			appendStringInfoString(&g->cx->out, " ..");
			if (inclusive)
				go_emit_range(g, semi1 + 3, semi2, &g->cx->out);	/* B (bound) */
			else
			{
				appendStringInfoString(&g->cx->out, " (");
				go_emit_range(g, semi1 + 3, semi2, &g->cx->out);	/* B */
				appendStringInfo(&g->cx->out, ") %c 1", dir == 'd' ? '+' : '-');
			}
			if (step0 >= 0)
			{
				if (step1 <= step0)
					go_err(g, "for loop step (+=/-=) is missing its amount");
				appendStringInfoString(&g->cx->out, " BY");
				go_emit_range(g, step0, step1, &g->cx->out);
			}
			appendStringInfoString(&g->cx->out, " LOOP\n");
			g->pos = hb;
			g->cx->loopdepth++;
			go_block(g, ind + 1);
			g->cx->loopdepth--;
			indent(&g->cx->out, ind);
			appendStringInfoString(&g->cx->out, "END LOOP;\n");
			go_eat_semi(g);
			return;
		}
	}

	/* general C-style for -> [init;] WHILE cond LOOP body post; END LOOP.
	 * (continue re-tests the condition without running the post statement, as
	 * in plpgsql; use the counting form above when that matters.) */
	if (semi1 >= 0)
	{
		if (semi1 > h0)					/* init statement */
		{
			go_simple(g, ind, semi1);
		}
		g->pos = hb;
		indent(&g->cx->out, ind);
		appendStringInfoString(&g->cx->out, "WHILE");
		if (semi2 > semi1 + 1)
			go_emit_range(g, semi1 + 1, semi2, &g->cx->out);
		else
			appendStringInfoString(&g->cx->out, " true");
		appendStringInfoString(&g->cx->out, " LOOP\n");
		go_block(g, ind + 1);
		if (semi2 >= 0 && hb > semi2 + 1)		/* post statement */
		{
			int			save = g->pos;

			g->pos = semi2 + 1;
			go_simple(g, ind + 1, hb);
			g->pos = save;
		}
		indent(&g->cx->out, ind);
		appendStringInfoString(&g->cx->out, "END LOOP;\n");
		go_eat_semi(g);
		return;
	}

	/* for { } (infinite) or for cond { } (while) */
	indent(&g->cx->out, ind);
	if (hb > h0)
	{
		appendStringInfoString(&g->cx->out, "WHILE");
		go_emit_range(g, h0, hb, &g->cx->out);
		appendStringInfoString(&g->cx->out, " LOOP\n");
	}
	else
		appendStringInfoString(&g->cx->out, "LOOP\n");
	g->pos = hb;
	g->cx->loopdepth++;
	go_block(g, ind + 1);
	g->cx->loopdepth--;
	indent(&g->cx->out, ind);
	appendStringInfoString(&g->cx->out, "END LOOP;\n");
	go_eat_semi(g);
}

/* switch [tag] { case a, b: ...; default: ... } -> IF/ELSIF/ELSE */
static void
go_switch(Go *g, int ind)
{
	int			hb,
				t0,
				tag0 = -1,
				tag1 = -1;
	bool		first = true,
				had_default = false;

	g->pos++;								/* switch */
	t0 = g->pos;
	hb = go_header_end(g, g->pos);
	if (hb > t0)							/* a switch tag is present */
	{
		tag0 = t0;
		tag1 = hb;
	}
	if (go_cur(g)->kind != GO_LBRACE && g->t[hb].kind == GO_LBRACE)
		g->pos = hb;
	if (go_cur(g)->kind != GO_LBRACE)
		go_err(g, "expected { after switch");
	g->pos++;								/* { */
	go_eat_semi(g);
	while (go_cur(g)->kind != GO_RBRACE && go_cur(g)->kind != GO_EOF)
	{
		bool		is_default = go_ci(go_cur(g), "default");

		if (!go_ci(go_cur(g), "case") && !is_default)
			go_err(g, "expected case or default in switch");
		g->pos++;						/* case / default */
		if (!is_default)
		{
			/* case values up to the ':' ; multiple values are OR-ed */
			int			cv0 = g->pos,
						colon = -1,
						j,
						depth = 0;

			for (j = g->pos; j < g->nt; j++)
			{
				GoKind		k = g->t[j].kind;

				if (k == GO_LP || k == GO_LBRACK || k == GO_LBRACE)
					depth++;
				else if (k == GO_RP || k == GO_RBRACK || k == GO_RBRACE)
					depth--;
				else if (depth == 0 && k == GO_OP && g->t[j].len == 1 && g->t[j].s[0] == ':')
				{
					colon = j;
					break;
				}
			}
			if (colon < 0)
				go_err(g, "expected : after case");
			indent(&g->cx->out, ind);
			appendStringInfoString(&g->cx->out, first ? "IF" : "ELSIF");
			/* split the case values on top-level commas */
			{
				int			s = cv0,
							d = 0,
							nc = 0;

				for (j = cv0; j <= colon; j++)
				{
					GoKind		k = (j < colon) ? g->t[j].kind : GO_COMMA;

					if (k == GO_LP || k == GO_LBRACK)
						d++;
					else if (k == GO_RP || k == GO_RBRACK)
						d--;
					else if (k == GO_COMMA && d == 0)
					{
						if (nc)
							appendStringInfoString(&g->cx->out, " OR");
						if (tag0 >= 0)
						{
							appendStringInfoChar(&g->cx->out, ' ');
							go_emit_range(g, tag0, tag1, &g->cx->out);
							appendStringInfoString(&g->cx->out, " =");
						}
						go_emit_range(g, s, j, &g->cx->out);
						s = j + 1;
						nc++;
					}
				}
			}
			appendStringInfoString(&g->cx->out, " THEN\n");
			g->pos = colon + 1;
			first = false;
		}
		else
		{
			int			colon = -1,
						j;

			for (j = g->pos; j < g->nt; j++)
				if (g->t[j].kind == GO_OP && g->t[j].len == 1 && g->t[j].s[0] == ':')
				{
					colon = j;
					break;
				}
			if (colon < 0)
				go_err(g, "expected : after default");
			indent(&g->cx->out, ind);
			appendStringInfoString(&g->cx->out, first ? "IF true THEN\n" : "ELSE\n");
			g->pos = colon + 1;
			had_default = true;
			first = false;
		}
		go_eat_semi(g);
		/* the case body runs until the next case/default or the closing } */
		while (!go_ci(go_cur(g), "case") && !go_ci(go_cur(g), "default") &&
			   go_cur(g)->kind != GO_RBRACE && go_cur(g)->kind != GO_EOF)
		{
			if (go_ci(go_cur(g), "fallthrough"))
				go_err(g, "fallthrough is not supported");
			go_stmt(g, ind + 1);
		}
	}
	if (go_cur(g)->kind == GO_RBRACE)
		g->pos++;
	go_eat_semi(g);
	if (!first)
	{
		(void) had_default;
		indent(&g->cx->out, ind);
		appendStringInfoString(&g->cx->out, "END IF;\n");
	}
}

/* return [expr] */
static void
go_return(Go *g, int ind)
{
	int			e0,
				e1;

	g->pos++;								/* return */
	e0 = g->pos;
	e1 = go_stmt_end(g, e0);
	indent(&g->cx->out, ind);
	if (e1 > e0)
	{
		if (g->cx->retset)
			appendStringInfoString(&g->cx->out, "RETURN QUERY SELECT");
		else
			appendStringInfoString(&g->cx->out, "RETURN");
		go_emit_range(g, e0, e1, &g->cx->out);
		appendStringInfoString(&g->cx->out, ";\n");
	}
	else
		appendStringInfoString(&g->cx->out, "RETURN;\n");
	g->pos = e1;
}

static void
go_stmt(Go *g, int ind)
{
	GoTok	   *tk = go_cur(g);

	if (++g->cx->depth > PLX_MAX_DEPTH)
		go_err(g, "statement nested too deeply");

	if (tk->kind == GO_SEMI)
	{
		g->pos++;
		g->cx->depth--;
		return;
	}
	if (tk->kind == GO_LBRACE)			/* a bare block: inline its statements */
	{
		go_block(g, ind);
		go_eat_semi(g);
	}
	else if (go_ci(tk, "var"))
		go_var(g, ind, false);
	else if (go_ci(tk, "const"))
		go_var(g, ind, true);
	else if (go_ci(tk, "if"))
		go_if(g, ind);
	else if (go_ci(tk, "for"))
		go_for(g, ind);
	else if (go_ci(tk, "switch"))
		go_switch(g, ind);
	else if (go_ci(tk, "return"))
		go_return(g, ind);
	else if (go_ci(tk, "break"))
	{
		g->pos++;
		indent(&g->cx->out, ind);
		appendStringInfoString(&g->cx->out, "EXIT;\n");
		go_eat_semi(g);
	}
	else if (go_ci(tk, "continue"))
	{
		g->pos++;
		indent(&g->cx->out, ind);
		appendStringInfoString(&g->cx->out, "CONTINUE;\n");
		go_eat_semi(g);
	}
	else if (go_ci(tk, "func"))
		go_err(g, "nested function definitions are not supported");
	else if (go_ci(tk, "go") || go_ci(tk, "defer") || go_ci(tk, "select") ||
			 go_ci(tk, "goto"))
		go_err(g, "go, defer, select, and goto are not supported");
	else
	{
		go_simple(g, ind, -1);
		go_eat_semi(g);
	}

	g->cx->depth--;
}

/* a brace-delimited block: { statements } */
static void
go_block(Go *g, int ind)
{
	if (go_cur(g)->kind != GO_LBRACE)
		go_err(g, "expected {");
	g->pos++;
	go_eat_semi(g);
	while (go_cur(g)->kind != GO_RBRACE && go_cur(g)->kind != GO_EOF)
		go_stmt(g, ind);
	if (go_cur(g)->kind != GO_RBRACE)
		go_err(g, "missing }");
	g->pos++;
}

static void
go_transpile(Ctx *cx)
{
	Go			g;

	memset(&g, 0, sizeof(g));
	g.cx = cx;
	go_lex(&g, cx->body);
	go_eat_semi(&g);

	/* an outer { ... } wrapping the whole body is unwrapped */
	if (go_cur(&g)->kind == GO_LBRACE)
	{
		int			close = go_match(&g, g.pos, g.nt);

		if (close == g.nt - 2 || (close > 0 && g.t[close + 1].kind == GO_SEMI &&
								  close + 2 >= g.nt - 1))
		{
			g.pos++;
			while (go_cur(&g)->kind != GO_RBRACE && go_cur(&g)->kind != GO_EOF)
				go_stmt(&g, 1);
			return;
		}
	}
	while (go_cur(&g)->kind != GO_EOF)
	{
		if (go_cur(&g)->kind == GO_RBRACE)
			go_err(&g, "unexpected }");
		go_stmt(&g, 1);
	}
}

/* ======================================================================== */

bool
plx_has_sentinel(const char *prosrc)
{
	while (*prosrc == ' ' || *prosrc == '\n' || *prosrc == '\t' || *prosrc == '\r')
		prosrc++;
	return strncmp(prosrc, "/*plx:v1:", 9) == 0;
}

static unsigned int
djb2(const char *s)
{
	unsigned int h = 5381;

	while (*s)
		h = ((h << 5) + h) + (unsigned char) *s++;
	return h;
}

char *
plx_transpile(const char *body, const PlxFuncMeta *meta, const PlxSurface *surf,
			  MemoryContext scratch)
{
	Ctx			cx;
	StringInfoData asm_;
	PlxLocal2  *l;

	memset(&cx, 0, sizeof(cx));
	cx.body = body;
	cx.meta = meta;
	cx.surf = surf;
	cx.retset = meta ? meta->retset : false;
	cx.mcx = scratch;
	initStringInfo(&cx.out);

	if (surf->block_style == PLX_BLK_COBOL)
	{
		cobol_transpile(&cx);
		goto assemble;
	}
	if (surf->block_style == PLX_BLK_TSQL)
	{
		tsql_transpile(&cx);
		goto assemble;
	}
	if (surf->block_style == PLX_BLK_GO)
	{
		go_transpile(&cx);
		goto assemble;
	}
	if (surf->block_style == PLX_BLK_PLSQL)
	{
		/* PL/SQL already carries its own DECLARE/BEGIN/END; emit it directly */
		plsql_transpile(&cx);
		initStringInfo(&asm_);
		appendStringInfo(&asm_, "/*plx:v1:%s:%08x*/\n", surf->lanname, djb2(body));
		appendStringInfoString(&asm_, cx.out.data);
		if (asm_.len == 0 || asm_.data[asm_.len - 1] != '\n')
			appendStringInfoChar(&asm_, '\n');
		appendStringInfo(&asm_, "/*plx-orig:b64$%s$plx-orig*/\n", b64_encode_body(body));
		return asm_.data;
	}

	/* TypeScript: rewrite "id: T" annotations before lexing (the original TS is
	 * still embedded via the unchanged `body`). */
	if (surf->ts_types)
		cx.body = ts_preprocess(body);

	lex(&cx);

	if (surf->block_style == PLX_BLK_BRACE)
		parse_brace_program(&cx);
	else if (surf->block_style == PLX_BLK_INDENT)
		parse_py_program(&cx);
	else
	{
		/* keyword-end (Ruby) top-level walk */
		for (;;)
		{
			skip_seps(&cx);
			if (cx.t[cx.pos].kind == T_EOF)
				break;
			if (cx.t[cx.pos].kind == T_KW &&
				(cx.t[cx.pos].kw == KW_END || cx.t[cx.pos].kw == KW_ELSIF ||
				 cx.t[cx.pos].kw == KW_ELSE || cx.t[cx.pos].kw == KW_WHEN ||
				 cx.t[cx.pos].kw == KW_RESCUE || cx.t[cx.pos].kw == KW_ENSURE))
				plx_err(&cx, cx.t[cx.pos].line, "unexpected '%.*s'",
						cx.t[cx.pos].len, cx.t[cx.pos].s);
			parse_stmt(&cx, 1, true);
		}
	}

assemble:
	/* assemble: sentinel + DECLARE + BEGIN + body + END + embedded original */
	initStringInfo(&asm_);
	appendStringInfo(&asm_, "/*plx:v1:%s:%08x*/\n", surf->lanname, djb2(body));
	if (cx.locals)
	{
		appendStringInfoString(&asm_, "DECLARE\n");
		for (l = cx.locals; l; l = l->next)
		{
			if (l->is_record)
				appendStringInfo(&asm_, "  %s RECORD;\n", l->name);
			else if (!l->typ)
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("%s: cannot infer a PostgreSQL type for local variable \"%s\"",
								surf->lanname, l->name),
						 errhint("add an explicit type annotation for \"%s\"", l->name)));
			else if (l->is_const)
				appendStringInfo(&asm_, "  %s CONSTANT %s := %s;\n", l->name, l->typ, l->init);
			else if (l->init)
				appendStringInfo(&asm_, "  %s %s := %s;\n", l->name, l->typ, l->init);
			else
				appendStringInfo(&asm_, "  %s %s;\n", l->name, l->typ);
		}
	}
	appendStringInfoString(&asm_, "BEGIN\n");
	appendStringInfoString(&asm_, cx.out.data);
	appendStringInfoString(&asm_, "END;\n");
	appendStringInfo(&asm_, "/*plx-orig:b64$%s$plx-orig*/\n", b64_encode_body(body));

	return asm_.data;
}

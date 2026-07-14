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
#include "common/base64.h"
#include "utils/builtins.h"

#include "plx.h"
#include "plx_int.h"

/* ---------------------------------------------------------------- tokens */

typedef enum
{
	T_EOF, T_NEWLINE, T_SEMI, T_IDENT, T_KW, T_INT, T_FLOAT, T_STRING,
	T_OP, T_LPAREN, T_RPAREN, T_LBRACKET, T_RBRACKET, T_LBRACE, T_RBRACE,
	T_PIPE, T_COMMA, T_DOT, T_TYPEANN, T_ERROR
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
	bool		retset;
	MemoryContext mcx;
} Ctx;

/* forward decls */
#define PLX_MAX_DEPTH 500		/* recursion cap (parsers + expression rewriter) */

static pg_noreturn void plx_err(Ctx *cx, int line, const char *fmt,...) pg_attribute_printf(3, 4);
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
static void parse_brace_stmt(Ctx *cx, int ind, bool toplevel);

/* ---------------------------------------------------------------- errors */

static pg_noreturn void
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

#define PUSH(k) do { \
		if (n >= cap) { cap *= 2; t = repalloc(t, sizeof(Tok) * cap); } \
		memset(&t[n], 0, sizeof(Tok)); \
		t[n].kind = (k); t[n].line = line; t[n].s = tokstart; \
		t[n].len = (int) (p - tokstart); n++; \
	} while (0)

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
			if (!suppress_newline(t, n))
			{
				tokstart = p - 1;
				if (n >= cap) { cap *= 2; t = repalloc(t, sizeof(Tok) * cap); }
				memset(&t[n], 0, sizeof(Tok));
				t[n].kind = T_NEWLINE; t[n].line = line; t[n].s = tokstart; t[n].len = 1;
				n++;
			}
			line++;
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
	/* trailing newline + EOF */
	{
		const char *tokstart = p;

		if (n >= cap) { cap *= 2; t = repalloc(t, sizeof(Tok) * cap); }
		memset(&t[n], 0, sizeof(Tok));
		t[n].kind = T_NEWLINE; t[n].line = line; t[n].s = tokstart; n++;
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
interp_at(const PlxSurface *surf, const char *s, const char *e,
		  const char **exs, int *exl, const char **next)
{
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
	const char *s = tk->s + 1;	/* skip opening quote */
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

		if (tk->quote == cx->surf->interp_quote && interp_at(cx->surf, s, e, &exs, &exl, &nx))
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
			appendStringInfo(out, "(%s)::text", rw);
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
				char	   *cond = rewrite_expr(cx, s, q, true);
				char	   *a = rewrite_expr(cx, s + q + 1, c - q - 1, false);
				char	   *b = rewrite_expr(cx, s + c + 1, len - c - 1, false);
				char	   *r;

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
		if (is_ident_start(c))
		{
			int			j = i;
			int			wl;

			while (j < len && is_ident(s[j]))
				j++;
			wl = j - i;

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
				(wl == 9 && strncmp(s + i, "undefined", 9) == 0))
				appendStringInfoString(&out, "NULL");
			else if (wl == 4 && strncmp(s + i, "true", 4) == 0)
				appendStringInfoString(&out, "true");
			else if (wl == 5 && strncmp(s + i, "false", 5) == 0)
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
	*after = p + 1;
	return n;
}

/* Emit a Ruby string token as raw SQL text (interp #{e} -> rewritten expr,
 * inline; no surrounding quotes). Used for static query/perform/fetch SQL. */
static void
emit_string_as_sql(Ctx *cx, Tok *tk, StringInfo out)
{
	const char *s = tk->s + 1;
	const char *e = tk->s + tk->len - 1;

	while (s < e)
	{
		const char *exs, *nx;
		int			exl;

		if (tk->quote == cx->surf->interp_quote && interp_at(cx->surf, s, e, &exs, &exl, &nx))
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
	/* next / break */
	if (t0->kind == T_KW && (t0->kw == KW_NEXT || t0->kw == KW_BREAK))
	{
		if (cx->loopdepth == 0)
			plx_err(cx, t0->line, "%s outside a loop", t0->kw == KW_NEXT ? "next" : "break");
		indent(o, ind);
		appendStringInfoString(o, t0->kw == KW_NEXT ? "CONTINUE;\n" : "EXIT;\n");
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
	/* assignment:  IDENT (= | += | -= | *= | /= | %=) RHS [#:: T] */
	if (t0->kind == T_IDENT && a + 1 < b && cx->t[a + 1].kind == T_OP)
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

		/* next/break modifiers -> WHEN */
		if (t0->kind == T_KW && (t0->kw == KW_NEXT || t0->kw == KW_BREAK))
		{
			if (cx->loopdepth == 0)
				plx_err(cx, t0->line, "%s outside a loop", t0->kw == KW_NEXT ? "next" : "break");
			indent(o, ind);
			appendStringInfo(o, "%s WHEN %s%s%s;\n",
							 t0->kw == KW_NEXT ? "CONTINUE" : "EXIT",
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
		cx->handlerdepth++;
		hb = capture_block(cx, ind + 2);
		cx->handlerdepth--;
		cx->exc_var = save_ev;
		cx->exc_varlen = save_evl;

		appendStringInfoSpaces(&arms, (ind + 1) * 2);
		appendStringInfo(&arms, "WHEN %s THEN\n", cond);
		appendStringInfoString(&arms, hb);
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
		cx->handlerdepth++;
		saved = cx->out;
		initStringInfo(&cx->out);
		parse_brace_block(cx, ind + 2);
		hb = cx->out.data;
		cx->out = saved;
		cx->handlerdepth--;
		cx->exc_var = save_ev;
		cx->exc_varlen = save_evl;

		appendStringInfoSpaces(&arms, (ind + 1) * 2);
		appendStringInfo(&arms, "WHEN %s THEN\n", cond);
		appendStringInfoString(&arms, hb);
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
			appendStringInfoString(o, "ELSE\n");
		else
			appendStringInfo(o, "WHEN %s THEN\n", vals.data);

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

/* ---------------------------------------------------------------- assemble */

static char *
b64_encode_body(const char *body)
{
	int			srclen = (int) strlen(body);
	int			dstlen = pg_b64_enc_len(srclen);
	char	   *dst = palloc(dstlen + 1);
	int			r = pg_b64_encode((const uint8 *) body, srclen, dst, dstlen);

	if (r < 0)
		elog(ERROR, "plx: base64 encode failed");
	dst[r] = '\0';
	return dst;
}

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

	lex(&cx);

	if (surf->block_style == PLX_BLK_BRACE)
		parse_brace_program(&cx);
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

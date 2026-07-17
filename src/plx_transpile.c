/*
 * plx_transpile.c — plx dialect-neutral transpiler engine.
 *
 * This file is the shared engine that every dialect front end calls into: the
 * byte lexer (plx_lex, used by the text families), the expression rewriter
 * (plx_rewrite_expr), the leaf-statement emitter and intrinsics (plx_emit_core /
 * plx_emit_leaf), the symbol table, string/interpolation decoding, and the final
 * assemble (plx_transpile). The engine has no per-dialect branching: plx_transpile
 * dispatches through the PlxSurface.parse_body vtable method, and each dialect's
 * lexing/parsing/emission lives in its own translation unit (plx_dialect_*.c,
 * plx_parse_brace.c). The engine API is declared in plx_engine.h.
 *
 * Philosophy (see TRANSPILER.md): we do NOT parse expressions as an AST. plpgsql
 * expressions ARE SQL expressions. This is a statement-level restructurer: the
 * byte lexer isolates strings/comments/interpolation so structure keywords are
 * never confused with string bytes; each dialect's recursive walk lowers its
 * recognized constructs to plpgsql, hoisting typed DECLAREs and rewriting a small
 * fixed set of operators/interpolations, passing everything else through verbatim.
 * Anything outside the supported subset is rejected with a precise error at
 * CREATE time.
 */
#include "postgres.h"

#include "fmgr.h"
#include "lib/stringinfo.h"
#include "mb/pg_wchar.h"
#include "miscadmin.h"
#include "utils/builtins.h"

#include "plx.h"
#include "plx_int.h"
#include "plx_engine.h"			/* Tok, PlxLocal2, Ctx, PLX_MAX_DEPTH, engine API */

/*
 * Token, symbol-table, and context types plus the shared engine entry points
 * live in plx_engine.h. The stacked-diagnostics bits below are engine-private
 * (only emit_string_value() and plx_diag_prefix() consume them).
 */

/* forward decls (dialect-local parsers; engine entry points are in plx_engine.h) */
static char *rewrite_expr_inner(Ctx *cx, const char *s, int len, bool boolctx);

/* ---------------------------------------------------------------- errors */

pg_noreturn void
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
 * overflow on hostile deeply-nested input. The fixed PLX_MAX_DEPTH count is a
 * coarse backstop; check_stack_depth() is the real guard, since a single large
 * parse_stmt_inner frame can exhaust the C stack (honoring max_stack_depth) well
 * before 500 levels — without it a few hundred nested blocks crash the backend
 * with SIGSEGV instead of raising a clean error. */
char *
plx_rewrite_expr(Ctx *cx, const char *s, int len, bool boolctx)
{
	char	   *r;

	check_stack_depth();
	if (++cx->depth > PLX_MAX_DEPTH)
		plx_err(cx, 0, "expression nested too deeply");
	r = rewrite_expr_inner(cx, s, len, boolctx);
	cx->depth--;
	return r;
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

bool
plx_is_ident_start(char c)
{
	return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}
bool
plx_is_ident(char c)
{
	return plx_is_ident_start(c) || (c >= '0' && c <= '9');
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

void
plx_lex(Ctx *cx)
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
					/*
					 * Raise a clean error at the stack limit rather than emitting
					 * a T_INDENT with no matching stack entry: past the cap the
					 * unbalanced INDENT/DEDENT counts mis-nest every enclosing
					 * block. indent_stack has lengthof() slots (indices 0..N-1).
					 */
					if (indent_sp >= (int) lengthof(indent_stack) - 1)
						plx_err(cx, line, "indentation nested too deeply");
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
		if (surf->var_sigil && *p == surf->var_sigil && plx_is_ident_start(p[1]))
		{
			p++;
			tokstart = p;
			while (p < end && plx_is_ident(*p))
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
		if (plx_is_ident_start(*p))
		{
			Kw			k;

			while (p < end && plx_is_ident(*p))
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

			/*
			 * Non-decimal integer literal (0x.. / 0o.. / 0b..): consume the whole
			 * literal as one T_INT so it is not split into '0' + an identifier
			 * ('xFF'), which would emit two adjacent tokens and break the SQL.
			 * The decimal conversion happens in rewrite_expr at emit time.
			 */
			if (*p == '0' && p + 1 < end &&
				(p[1] == 'x' || p[1] == 'X' || p[1] == 'o' || p[1] == 'O' ||
				 p[1] == 'b' || p[1] == 'B'))
			{
				char		pfx = p[1];

				p += 2;
				if (pfx == 'x' || pfx == 'X')
					while (p < end && (((*p >= '0' && *p <= '9') ||
										(*p >= 'a' && *p <= 'f') ||
										(*p >= 'A' && *p <= 'F')) || *p == '_'))
						p++;
				else if (pfx == 'o' || pfx == 'O')
					while (p < end && ((*p >= '0' && *p <= '7') || *p == '_'))
						p++;
				else
					while (p < end && (*p == '0' || *p == '1' || *p == '_'))
						p++;
				PUSH(T_INT);
				continue;
			}

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
		/* T_EOF needs the same capacity guard as every other token write; without
		 * it a source that lexes to exactly cap-1 tokens overflows t[] by one. */
		if (n >= cap) { cap *= 2; t = repalloc(t, sizeof(Tok) * cap); }
		memset(&t[n], 0, sizeof(Tok));
		t[n].kind = T_EOF; t[n].line = line; t[n].s = tokstart; n++;
	}
	cx->t = t;
	cx->pos = 0;
#undef PUSH
}

/* ---------------------------------------------------------------- helpers */

bool
plx_tok_is(Tok *tk, const char *op)
{
	return tk->kind == T_OP && (int) strlen(op) == tk->len &&
		strncmp(tk->s, op, tk->len) == 0;
}

bool
plx_name_eq(Tok *tk, const char *w)
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

void
plx_skip_seps(Ctx *cx)
{
	while (cx->t[cx->pos].kind == T_NEWLINE || cx->t[cx->pos].kind == T_SEMI)
		cx->pos++;
}

/* case-insensitive name compare against a param list */
bool
plx_is_param(Ctx *cx, const char *name, int len)
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

PlxLocal2 *
plx_local_find(Ctx *cx, const char *name, int len)
{
	PlxLocal2  *l;

	for (l = cx->locals; l; l = l->next)
		if ((int) strlen(l->name) == len && pg_strncasecmp(l->name, name, len) == 0)
			return l;
	return NULL;
}

PlxLocal2 *
plx_local_add(Ctx *cx, const char *name, int len)
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
		if (s + 1 < e && s[0] == '$' && s[1] == '{')	/* ${name} (PHP curly) */
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
		if (s + 1 < e && s[0] == '$' && plx_is_ident_start(s[1]))	/* $var */
		{
			const char *q = s + 1;

			while (q < e && plx_is_ident(*q))
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
		/* raw single-quoted string (Ruby/PHP): backslashes are literal */
		bool		raw = tk->sq && cx->surf->sq_is_raw;

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
			rw = plx_rewrite_expr(cx, exs, exl, false);
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
		if (raw && s[0] == '\\' && s + 1 < e)
		{
			/*
			 * Raw single-quoted literal: only \\ and \' are special; every other
			 * backslash sequence is two literal characters (Ruby/PHP semantics),
			 * so '\n' stays a backslash + 'n', not a newline. Emitted verbatim
			 * (no E'' prefix) under standard_conforming_strings.
			 */
			char		c = s[1];

			s += 2;
			switch (c)
			{
				case '\\': appendStringInfoChar(&lit, '\\'); break;
				case '\'': appendStringInfoString(&lit, "''"); break;
				default:
					appendStringInfoChar(&lit, '\\');
					appendStringInfoChar(&lit, c);
					break;
			}
			continue;
		}
		if (!raw && s[0] == '\\' && s + 1 < e)
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
				cond = plx_rewrite_expr(cx, s, q, true);
				a = plx_rewrite_expr(cx, s + q + 1, c - q - 1, false);
				b = plx_rewrite_expr(cx, s + c + 1, len - c - 1, false);
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
		if (surf->var_sigil && c == surf->var_sigil && i + 1 < len && plx_is_ident_start(s[i + 1]))
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
			(i == 0 || !plx_is_ident(s[i - 1])))
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
		if (plx_is_ident_start(c))
		{
			int			j = i;
			int			wl;

			while (j < len && plx_is_ident(s[j]))
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
				inner = plx_rewrite_expr(cx, s + argstart, k - argstart, false);
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

					while (f < len && plx_is_ident(s[f]))
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
								if (!plx_local_find(cx, tmp, (int) strlen(tmp)))
								{
									PlxLocal2  *tl = plx_local_add(cx, tmp, (int) strlen(tmp));

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
						while (k < len && plx_is_ident(s[k]))
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
			/*
			 * Non-decimal integer literal (0x.. / 0o.. / 0b.., with optional '_'
			 * group separators): convert to a decimal literal. plpgsql only
			 * accepts non-decimal integer literals on PG16+, and plx supports
			 * PG13+, so emitting decimal keeps generated code portable.
			 */
			if (s[i] == '0' && i + 1 < len &&
				(s[i + 1] == 'x' || s[i + 1] == 'X' || s[i + 1] == 'o' ||
				 s[i + 1] == 'O' || s[i + 1] == 'b' || s[i + 1] == 'B'))
			{
				char		pfx = s[i + 1];
				int			base = (pfx == 'x' || pfx == 'X') ? 16 :
					(pfx == 'o' || pfx == 'O') ? 8 : 2;
				uint64		val = 0;
				bool		ovf = false;
				int			start = i;
				int			ndig = 0;

				i += 2;
				for (; i < len; i++)
				{
					char		d = s[i];
					int			dv;

					if (d == '_')
						continue;
					if (d >= '0' && d <= '9')
						dv = d - '0';
					else if (d >= 'a' && d <= 'f')
						dv = 10 + (d - 'a');
					else if (d >= 'A' && d <= 'F')
						dv = 10 + (d - 'A');
					else
						break;
					if (dv >= base)
						break;
					if (val > (UINT64_MAX - dv) / base)
						ovf = true;
					val = val * base + dv;
					ndig++;
				}
				/*
				 * Overflow (> 64-bit): raise a clean error rather than
				 * emitting the raw 0x.. text, which is invalid plpgsql on
				 * PG13-15 and would only surface as an opaque parse failure
				 * in the generated function body.
				 */
				if (ovf)
					plx_err(cx, 0, "integer literal out of range: %.*s",
							i - start, s + start);
				/* no digits (malformed): leave the source text untouched */
				if (ndig == 0)
					appendBinaryStringInfo(&out, s + start, i - start);
				else
					appendStringInfo(&out, UINT64_FORMAT, val);
				continue;
			}
			/* decimal integer part */
			while (i < len && ((s[i] >= '0' && s[i] <= '9') || s[i] == '_'))
			{
				if (s[i] != '_')
					appendStringInfoChar(&out, s[i]);
				i++;
			}
			/* fractional part: a single '.' followed by a digit — never '..'
			 * (a range/slice operator), so dotted range syntax is left intact */
			if (i + 1 < len && s[i] == '.' && s[i + 1] >= '0' && s[i + 1] <= '9')
			{
				appendStringInfoChar(&out, '.');
				i++;
				while (i < len && ((s[i] >= '0' && s[i] <= '9') || s[i] == '_'))
				{
					if (s[i] != '_')
						appendStringInfoChar(&out, s[i]);
					i++;
				}
			}
			/* exponent */
			if (i < len && (s[i] == 'e' || s[i] == 'E'))
			{
				appendStringInfoChar(&out, s[i]);
				i++;
				if (i < len && (s[i] == '+' || s[i] == '-'))
				{
					appendStringInfoChar(&out, s[i]);
					i++;
				}
				while (i < len && s[i] >= '0' && s[i] <= '9')
				{
					appendStringInfoChar(&out, s[i]);
					i++;
				}
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
						 (k + 3 == len || !plx_is_ident(s[k + 3]))) ||
						(k + 4 <= len && strncmp(s + k, "null", 4) == 0 &&
						 (k + 4 == len || !plx_is_ident(s[k + 4]))) ||
						(k + 4 <= len && strncmp(s + k, "None", 4) == 0 &&
						 (k + 4 == len || !plx_is_ident(s[k + 4]))))
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
char *
plx_span_text(Ctx *cx, int a, int b)
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

void
plx_indent(StringInfo o, int n)
{
	int			i;

	for (i = 0; i < n; i++)
		appendStringInfoString(o, "  ");
}

/* ---------------------------------------------------------------- leaves */

/* find end of current logical statement: index of NEWLINE/SEMI/EOF */
int
plx_stmt_end(Ctx *cx, int from)
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
int
plx_parse_args(Ctx *cx, int a, int *as, int *ae, int maxargs, int *after)
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

/* GET STACKED DIAGNOSTICS lines for the fields used in a handler (see diag_mask) */
char *
plx_diag_prefix(int mask, int ind)
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

/* Emit a Ruby string token as raw SQL text (interp #{e} -> rewritten expr,
 * inline; no surrounding quotes). Used for static query/perform/fetch SQL. */
void
plx_emit_string_as_sql(Ctx *cx, Tok *tk, StringInfo out)
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
			appendStringInfoString(out, plx_rewrite_expr(cx, exs, exl, false));
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

const char *
plx_exc_class_to_condition(const char *s, int len)
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
char *
plx_binds_text(Ctx *cx, int *as, int *ae, int nargs)
{
	StringInfoData b;
	int			i;

	if (nargs <= 1)
		return NULL;
	initStringInfo(&b);
	for (i = 1; i < nargs; i++)
	{
		char	   *st = plx_span_text(cx, as[i], ae[i]);

		if (i > 1)
			appendStringInfoString(&b, ", ");
		appendStringInfoString(&b, plx_rewrite_expr(cx, st, (int) strlen(st), false));
	}
	return b.data;
}

bool
plx_arg_is_string_literal(Ctx *cx, int as, int ae)
{
	return (ae - as == 1) && cx->t[as].kind == T_STRING;
}

/* execute(SQL[, binds]) -> EXECUTE <val> [USING ...]; */
static void
emit_execute(Ctx *cx, int a, int ind)
{
	int			as[16], ae[16], after, n;
	char	   *sqlv, *binds, *st;

	n = plx_parse_args(cx, a, as, ae, 16, &after);
	if (n < 1)
		plx_err(cx, cx->t[a].line, "execute requires a SQL argument");
	st = plx_span_text(cx, as[0], ae[0]);
	sqlv = plx_rewrite_expr(cx, st, (int) strlen(st), false);
	binds = plx_binds_text(cx, as, ae, n);
	plx_indent(&cx->out, ind);
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

	if (!plx_is_param(cx, lhs->s, lhs->len))
	{
		PlxLocal2  *l = plx_local_find(cx, lhs->s, lhs->len);

		if (!l)
			l = plx_local_add(cx, lhs->s, lhs->len);
		if (!l->typ)
			l->typ = pstrdup("refcursor");
	}
	n = plx_parse_args(cx, a, as, ae, 16, &after);
	if (n < 1)
		plx_err(cx, cx->t[a].line, "open_cursor requires a SQL argument");
	plx_indent(&cx->out, ind);
	if (n == 1 && plx_arg_is_string_literal(cx, as[0], ae[0]))
	{
		appendStringInfo(&cx->out, "OPEN %.*s FOR ", lhs->len, lhs->s);
		plx_emit_string_as_sql(cx, &cx->t[as[0]], &cx->out);
		appendStringInfoString(&cx->out, ";\n");
	}
	else
	{
		char	   *sqlv = plx_rw_range(cx, as[0], ae[0], false);
		char	   *binds = plx_binds_text(cx, as, ae, n);

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

	if (!plx_is_param(cx, lhs->s, lhs->len))
	{
		PlxLocal2  *l = plx_local_find(cx, lhs->s, lhs->len);

		if (!l)
			l = plx_local_add(cx, lhs->s, lhs->len);
		l->is_record = true;
	}
	n = plx_parse_args(cx, a, as, ae, 4, &after);
	if (n < 1)
		plx_err(cx, cx->t[a].line, "fetch_from requires a cursor");
	plx_indent(&cx->out, ind);
	appendStringInfo(&cx->out, "FETCH FROM %s INTO %.*s;\n",
					 plx_rw_range(cx, as[0], ae[0], false), lhs->len, lhs->s);
}

/* perform(SQL) -> DML verbatim, or PERFORM * FROM (SQL) __plx_p_N; */
static void
emit_perform(Ctx *cx, int a, int ind)
{
	int			as[16], ae[16], after, n;

	n = plx_parse_args(cx, a, as, ae, 16, &after);
	if (n < 1)
		plx_err(cx, cx->t[a].line, "perform requires a SQL argument");
	if (!plx_arg_is_string_literal(cx, as[0], ae[0]))
		plx_err(cx, cx->t[a].line, "perform currently requires a string-literal SQL");
	plx_indent(&cx->out, ind);
	if (sql_is_row_returning(&cx->t[as[0]]))
	{
		appendStringInfoString(&cx->out, "PERFORM * FROM (");
		plx_emit_string_as_sql(cx, &cx->t[as[0]], &cx->out);
		appendStringInfo(&cx->out, ") AS __plx_p_%d;\n", ++cx->subq);
	}
	else
	{
		plx_emit_string_as_sql(cx, &cx->t[as[0]], &cx->out);
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
	n = plx_parse_args(cx, a, as, ae, 16, &after);
	if (n < 1)
		plx_err(cx, cx->t[a].line, "return_query requires a SQL argument");
	plx_indent(&cx->out, ind);
	if (n == 1 && plx_arg_is_string_literal(cx, as[0], ae[0]))
	{
		appendStringInfoString(&cx->out, "RETURN QUERY ");
		plx_emit_string_as_sql(cx, &cx->t[as[0]], &cx->out);
		appendStringInfoString(&cx->out, ";\n");
	}
	else
	{
		char	   *st = plx_span_text(cx, as[0], ae[0]);
		char	   *sqlv = plx_rewrite_expr(cx, st, (int) strlen(st), false);
		char	   *binds = plx_binds_text(cx, as, ae, n);

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

	if (!plx_is_param(cx, lhs->s, lhs->len))
	{
		PlxLocal2  *l = plx_local_find(cx, lhs->s, lhs->len);

		if (!l)
			l = plx_local_add(cx, lhs->s, lhs->len);
		l->is_record = true;
	}
	n = plx_parse_args(cx, a, as, ae, 16, &after);
	if (n < 1)
		plx_err(cx, cx->t[a].line, "fetch_one requires a SQL argument");
	plx_indent(&cx->out, ind);
	if (n == 1 && plx_arg_is_string_literal(cx, as[0], ae[0]))
	{
		appendStringInfo(&cx->out, "SELECT * INTO %s%.*s FROM (",
						 strict ? "STRICT " : "", lhs->len, lhs->s);
		plx_emit_string_as_sql(cx, &cx->t[as[0]], &cx->out);
		appendStringInfo(&cx->out, ") AS __plx_fo_%d;\n", ++cx->subq);
	}
	else
	{
		char	   *st = plx_span_text(cx, as[0], ae[0]);
		char	   *sqlv = plx_rewrite_expr(cx, st, (int) strlen(st), false);
		char	   *binds = plx_binds_text(cx, as, ae, n);

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
void
plx_emit_raise_call(Ctx *cx, int a, int ind)
{
	int			as[8], ae[8], after, n, mi = 0;
	const char *level = "EXCEPTION";
	char	   *mt, *msg;

	n = plx_parse_args(cx, a, as, ae, 8, &after);
	if (n < 1)
		plx_err(cx, cx->t[a].line, "raise requires a message");
	if (n >= 2 && plx_arg_is_string_literal(cx, as[0], ae[0]))
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
	mt = plx_span_text(cx, as[mi], ae[mi]);
	msg = plx_rewrite_expr(cx, mt, (int) strlen(mt), false);
	plx_indent(&cx->out, ind);
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
		plx_indent(o, ind);
		appendStringInfoString(o, "RAISE;\n");
		return;
	}
	/* LEVEL: form -> IDENT/KW ':' */
	if ((cx->t[p].kind == T_IDENT || cx->t[p].kind == T_KW) &&
		p + 1 < b && plx_tok_is(&cx->t[p + 1], ":"))
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
		char	   *mt = plx_span_text(cx, mstart, mend);

		msg = plx_rewrite_expr(cx, mt, (int) strlen(mt), false);
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
				  ka + 1 < b && plx_tok_is(&cx->t[ka + 1], ":")))
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
			vt = plx_span_text(cx, va, ve);
			vv = plx_rewrite_expr(cx, vt, (int) strlen(vt), false);
			if (using.len)
				appendStringInfoString(&using, ", ");
			appendStringInfo(&using, "%s = %s", opt, vv);
			p2 = ve;
		}
	}
	plx_indent(o, ind);
	if (using.len)
		appendStringInfo(o, "RAISE %s '%%', %s USING %s;\n", level, msg, using.data);
	else
		appendStringInfo(o, "RAISE %s '%%', %s;\n", level, msg);
}

/* emit the core (non-block) statement given range [a,b); returns nothing */
void
plx_emit_core(Ctx *cx, int a, int b, int ind, bool toplevel)
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
		if (plx_name_eq(t0, "perform"))
		{
			emit_perform(cx, a, ind);
			return;
		}
		if (plx_name_eq(t0, "execute"))
		{
			emit_execute(cx, a, ind);
			return;
		}
		if (plx_name_eq(t0, "return_query"))
		{
			emit_return_query(cx, a, ind);
			return;
		}
		if (plx_name_eq(t0, "raise"))	/* raise(msg) / raise("level", msg) call form */
		{
			plx_emit_raise_call(cx, a, ind);
			return;
		}
		if (plx_name_eq(t0, "assert"))	/* assert(cond[, msg]) -> ASSERT cond[, msg]; */
		{
			int			as[4], ae[4], after, n;

			n = plx_parse_args(cx, a, as, ae, 4, &after);
			if (n < 1)
				plx_err(cx, t0->line, "assert requires a condition");
			plx_indent(o, ind);
			appendStringInfo(o, "ASSERT %s", plx_rw_range(cx, as[0], ae[0], true));
			if (n >= 2)
				appendStringInfo(o, ", %s", plx_rw_range(cx, as[1], ae[1], false));
			appendStringInfoString(o, ";\n");
			return;
		}
		if (plx_name_eq(t0, "call"))	/* call("proc", a, b) -> CALL proc(a, b); */
		{
			int			as[16], ae[16], after, n, i;

			n = plx_parse_args(cx, a, as, ae, 16, &after);
			if (n < 1)
				plx_err(cx, t0->line, "call requires a procedure name");
			plx_indent(o, ind);
			appendStringInfoString(o, "CALL ");
			if (plx_arg_is_string_literal(cx, as[0], ae[0]))
			{
				Tok		   *pn = &cx->t[as[0]];

				appendBinaryStringInfo(o, pn->s + 1, pn->len - 2);	/* unquote */
			}
			else
				appendStringInfoString(o, plx_rw_range(cx, as[0], ae[0], false));
			appendStringInfoChar(o, '(');
			for (i = 1; i < n; i++)
				appendStringInfo(o, "%s%s", i > 1 ? ", " : "", plx_rw_range(cx, as[i], ae[i], false));
			appendStringInfoString(o, ");\n");
			return;
		}
		if (plx_name_eq(t0, "commit") || plx_name_eq(t0, "rollback"))
		{
			plx_indent(o, ind);
			appendStringInfo(o, "%s;\n", plx_name_eq(t0, "commit") ? "COMMIT" : "ROLLBACK");
			return;
		}
		if (plx_name_eq(t0, "close_cursor"))	/* close_cursor(c) -> CLOSE c; */
		{
			int			as[4], ae[4], after, n;

			n = plx_parse_args(cx, a, as, ae, 4, &after);
			if (n < 1)
				plx_err(cx, t0->line, "close_cursor requires a cursor");
			plx_indent(o, ind);
			appendStringInfo(o, "CLOSE %s;\n", plx_rw_range(cx, as[0], ae[0], false));
			return;
		}
		if (plx_name_eq(t0, "move_cursor"))	/* move_cursor(c[, n]) -> MOVE [FORWARD n] FROM c; */
		{
			int			as[4], ae[4], after, n;

			n = plx_parse_args(cx, a, as, ae, 4, &after);
			if (n < 1)
				plx_err(cx, t0->line, "move_cursor requires a cursor");
			plx_indent(o, ind);
			if (n >= 2)
				appendStringInfo(o, "MOVE FORWARD %s FROM %s;\n",
								 plx_rw_range(cx, as[1], ae[1], false), plx_rw_range(cx, as[0], ae[0], false));
			else
				appendStringInfo(o, "MOVE FROM %s;\n", plx_rw_range(cx, as[0], ae[0], false));
			return;
		}
	}

	/* return */
	if (t0->kind == T_KW && t0->kw == KW_RETURN)
	{
		if (a + 1 >= b)
		{
			plx_indent(o, ind);
			appendStringInfoString(o, "RETURN;\n");
		}
		else
		{
			char	   *e = plx_rewrite_expr(cx, plx_span_text(cx, a + 1, b),
										 (int) strlen(plx_span_text(cx, a + 1, b)), false);

			if (cx->retset)
				plx_err(cx, t0->line, "use emit/return_next in a set-returning function, not 'return <value>'");
			plx_indent(o, ind);
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
		plx_indent(o, ind);
		if (ra >= rb)
			appendStringInfoString(o, "RETURN NEXT;\n");
		else
		{
			char	   *st = plx_span_text(cx, ra, rb);

			appendStringInfo(o, "RETURN NEXT %s;\n", plx_rewrite_expr(cx, st, (int) strlen(st), false));
		}
		return;
	}
	/* next / break, optionally with a loop label */
	if (t0->kind == T_KW && (t0->kw == KW_NEXT || t0->kw == KW_BREAK))
	{
		const char *kw = (t0->kw == KW_NEXT) ? "CONTINUE" : "EXIT";

		if (cx->loopdepth == 0)
			plx_err(cx, t0->line, "%s outside a loop", t0->kw == KW_NEXT ? "next" : "break");
		plx_indent(o, ind);
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

		if (plx_is_param(cx, t0->s, t0->len))
			plx_err(cx, t0->line, "cannot annotate parameter \"%.*s\"", t0->len, t0->s);
		l = plx_local_find(cx, t0->s, t0->len);
		if (!l)
			l = plx_local_add(cx, t0->s, t0->len);
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
	if (t0->kind == T_IDENT && !plx_is_param(cx, t0->s, t0->len))
	{
		int			app_rhs = -1;
		PlxLocal2  *l = plx_local_find(cx, t0->s, t0->len);

		if (a + 1 < b && plx_tok_is(&cx->t[a + 1], "<<") && l && is_stringy(l->typ))
			app_rhs = a + 2;	/* Ruby shovel on a string */
		else if (a + 2 < b && cx->t[a + 1].kind == T_DOT && plx_tok_is(&cx->t[a + 2], "="))
			app_rhs = a + 3;	/* PHP .= */
		else if (a + 1 < b && plx_tok_is(&cx->t[a + 1], "+=") && l && is_stringy(l->typ))
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
				l = plx_local_add(cx, t0->s, t0->len);
			if (l->is_const)
				plx_err(cx, t0->line, "cannot append to a constant");
			l->typ = pstrdup("plx_strbuild");
			rhs = plx_span_text(cx, app_rhs, rb);
			r = plx_rewrite_expr(cx, rhs, (int) strlen(rhs), false);
			plx_indent(o, ind);
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
		if (plx_tok_is(op, "=") && a + 2 < b && cx->t[a + 2].kind == T_IDENT &&
			(plx_name_eq(&cx->t[a + 2], "fetch_one") || plx_name_eq(&cx->t[a + 2], "fetch_one!")) &&
			a + 3 < b && cx->t[a + 3].kind == T_LPAREN)
		{
			emit_fetch(cx, t0, a + 2, ind, plx_name_eq(&cx->t[a + 2], "fetch_one!"));
			return;
		}
		/* lhs = open_cursor(...) / fetch_from(...) */
		if (plx_tok_is(op, "=") && a + 2 < b && cx->t[a + 2].kind == T_IDENT &&
			plx_name_eq(&cx->t[a + 2], "open_cursor") &&
			a + 3 < b && cx->t[a + 3].kind == T_LPAREN)
		{
			emit_open_cursor(cx, t0, a + 2, ind);
			return;
		}
		if (plx_tok_is(op, "=") && a + 2 < b && cx->t[a + 2].kind == T_IDENT &&
			plx_name_eq(&cx->t[a + 2], "fetch_from") &&
			a + 3 < b && cx->t[a + 3].kind == T_LPAREN)
		{
			emit_fetch_from(cx, t0, a + 2, ind);
			return;
		}
		/* lhs = row_count() -> GET DIAGNOSTICS lhs = ROW_COUNT; */
		if (plx_tok_is(op, "=") && a + 2 < b && cx->t[a + 2].kind == T_IDENT &&
			plx_name_eq(&cx->t[a + 2], "row_count") &&
			a + 3 < b && cx->t[a + 3].kind == T_LPAREN)
		{
			if (!plx_is_param(cx, t0->s, t0->len))
			{
				PlxLocal2  *l = plx_local_find(cx, t0->s, t0->len);

				if (!l)
					l = plx_local_add(cx, t0->s, t0->len);
				if (!l->typ)
					l->typ = pstrdup("bigint");
			}
			plx_indent(o, ind);
			appendStringInfo(o, "GET DIAGNOSTICS %.*s = ROW_COUNT;\n", t0->len, t0->s);
			return;
		}

		if (plx_tok_is(op, "="))
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
		if (plx_is_param(cx, t0->s, t0->len))
		{
			/* assigning to an OUT/param: plain := (never hoist) */
			char	   *rhs = plx_span_text(cx, rhs_a, rhs_b);
			char	   *r = plx_rewrite_expr(cx, rhs, (int) strlen(rhs), false);

			plx_indent(o, ind);
			if (compound)
				appendStringInfo(o, "%.*s := %.*s %c (%s);\n", t0->len, t0->s,
								 t0->len, t0->s, cop, r);
			else
				appendStringInfo(o, "%.*s := %s;\n", t0->len, t0->s, r);
			return;
		}
		{
			PlxLocal2  *l = plx_local_find(cx, t0->s, t0->len);
			bool		firstseen = (l == NULL);
			const char *littyp = const_literal_type(cx, rhs_a, rhs_b);

			if (!l)
				l = plx_local_add(cx, t0->s, t0->len);
			if (anntyp && !l->typ)
				l->typ = pnstrdup(anntyp, annlen);

			/* CONSTANT: initializer lives in the DECLARE; no runtime assignment */
			if (wantconst)
			{
				char	   *rhs = plx_span_text(cx, rhs_a, rhs_b);

				if (!firstseen)
					plx_err(cx, t0->line, "cannot reassign constant \"%.*s\"", t0->len, t0->s);
				if (compound)
					plx_err(cx, t0->line, "constant cannot use a compound assignment");
				if (!l->typ && littyp)
					l->typ = pstrdup(littyp);
				if (!l->typ)
					plx_err(cx, t0->line, "constant \"%.*s\" requires a type annotation", t0->len, t0->s);
				l->is_const = true;
				l->init = plx_rewrite_expr(cx, rhs, (int) strlen(rhs), false);
				return;
			}

			/* fold: top-level, first assignment, constant literal RHS */
			if (toplevel && firstseen && !compound && littyp && !l->is_record)
			{
				char	   *rhs = plx_span_text(cx, rhs_a, rhs_b);

				if (!l->typ)
					l->typ = pstrdup(littyp);
				l->init = plx_rewrite_expr(cx, rhs, (int) strlen(rhs), false);
				return;			/* statement folded into DECLARE; emit nothing */
			}
			/* otherwise infer type if still unknown */
			if (!l->typ && littyp)
				l->typ = pstrdup(littyp);
			{
				char	   *rhs = plx_span_text(cx, rhs_a, rhs_b);
				char	   *r = plx_rewrite_expr(cx, rhs, (int) strlen(rhs), false);

				plx_indent(o, ind);
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
			else if (depth == 0 && plx_tok_is(&cx->t[i], "="))
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
			char	   *lhs = plx_rw_range(cx, a, eq, false);
			char	   *rhs = plx_rw_range(cx, eq + 1, b, false);

			plx_indent(o, ind);
			appendStringInfo(o, "%s := %s;\n", lhs, rhs);
			return;
		}
	}

	plx_err(cx, t0->line, "unsupported statement");
}

/* emit a leaf statement (range [a,b)) applying any trailing modifier */
void
plx_emit_leaf(Ctx *cx, int a, int b, int ind, bool toplevel)
{
	StringInfo	o = &cx->out;
	int			m = find_modifier(cx, a, b);
	Tok		   *t0 = &cx->t[a];

	if (m < 0)
	{
		plx_emit_core(cx, a, b, ind, toplevel);
		return;
	}
	{
		Kw			mk = cx->t[m].kw;
		char	   *ct = plx_span_text(cx, m + 1, b);
		char	   *cond = plx_rewrite_expr(cx, ct, (int) strlen(ct), true);
		bool		neg = (mk == KW_UNLESS || mk == KW_UNTIL);

		/* next/break modifiers -> WHEN (optionally with a loop label) */
		if (t0->kind == T_KW && (t0->kw == KW_NEXT || t0->kw == KW_BREAK))
		{
			char		lbl[NAMEDATALEN + 1] = "";

			if (cx->loopdepth == 0)
				plx_err(cx, t0->line, "%s outside a loop", t0->kw == KW_NEXT ? "next" : "break");
			if (a + 1 < m && cx->t[a + 1].kind == T_IDENT)
				snprintf(lbl, sizeof(lbl), " %.*s", cx->t[a + 1].len, cx->t[a + 1].s);
			plx_indent(o, ind);
			appendStringInfo(o, "%s%s WHEN %s%s%s;\n",
							 t0->kw == KW_NEXT ? "CONTINUE" : "EXIT", lbl,
							 neg ? "NOT (" : "", cond, neg ? ")" : "");
			return;
		}
		if (mk == KW_IF || mk == KW_UNLESS)
		{
			plx_indent(o, ind);
			appendStringInfo(o, "IF %s%s%s THEN\n", neg ? "NOT (" : "", cond, neg ? ")" : "");
			plx_emit_core(cx, a, m, ind + 1, false);
			plx_indent(o, ind);
			appendStringInfoString(o, "END IF;\n");
		}
		else					/* while / until */
		{
			plx_indent(o, ind);
			appendStringInfo(o, "WHILE %s%s%s LOOP\n", neg ? "NOT (" : "", cond, neg ? ")" : "");
			cx->loopdepth++;
			plx_emit_core(cx, a, m, ind + 1, false);
			cx->loopdepth--;
			plx_indent(o, ind);
			appendStringInfoString(o, "END LOOP;\n");
		}
	}
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

	/* Run the dialect front end (the vtable method): fills cx.out. */
	surf->parse_body(&cx);

	/* assemble: sentinel + DECLARE + BEGIN + body + END + embedded original */
	initStringInfo(&asm_);
	appendStringInfo(&asm_, "/*plx:v1:%s:%08x*/\n", surf->lanname, djb2(body));

	if (surf->self_contained_block)
	{
		/* PL/SQL already carries its own DECLARE/BEGIN/END; emit it directly. */
		appendStringInfoString(&asm_, cx.out.data);
		if (asm_.len == 0 || asm_.data[asm_.len - 1] != '\n')
			appendStringInfoChar(&asm_, '\n');
	}
	else
	{
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
	}
	appendStringInfo(&asm_, "/*plx-orig:b64$%s$plx-orig*/\n", b64_encode_body(body));

	return asm_.data;
}

/*
 * plx_dialect_go.c - the "plxgo" dialect (Go).
 *
 * Go is a braced C-family language, but its lexer inserts semicolons at line
 * ends (ASI), uses parenless if/for headers, `:=` short declarations with type
 * inference, `for ... range`, and a switch without fall-through. Those differ
 * enough from the shared brace parser that plxgo has its own tokenizer and
 * recursive-descent front end (below, wired via PlxSurface.parse_body); the
 * dialect-neutral engine (lexer, expression rewriter, leaf emitter, assemble)
 * lives in plx_transpile.c. The shared keyword table is unused by the Go path.
 */
#include "postgres.h"

#include "catalog/pg_type.h"
#include "fmgr.h"
#include "lib/stringinfo.h"
#include "mb/pg_wchar.h"
#include "miscadmin.h"
#include "utils/memutils.h"

#include "plx.h"
#include "plx_int.h"
#include "plx_engine.h"

PG_FUNCTION_INFO_V1(plx_go_validator);
PG_FUNCTION_INFO_V1(plx_go_inline_handler);

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
	/*
	 * Label of the block that holds the body of the innermost enclosing
	 * three-clause for loop, or NULL inside any other loop. continue leaves
	 * that block (EXIT label) so the loop's post statement still runs.
	 */
	const char *cont_label;
	/*
	 * Label of the block around the innermost enclosing switch, or NULL inside
	 * a loop nested in it: break inside a switch leaves only the switch.
	 */
	const char *brk_label;
	/*
	 * Length of the array type go_read_type last read ([N]T with N a plain
	 * decimal number), or -1 for a slice or any other type.
	 */
	int			arrlen;
	/*
	 * mt[i] is the index of the bracket that closes the opener at i, or -1.
	 * go_lex builds it once, so go_match takes constant time.
	 */
	int		   *mt;
	/*
	 * The operand typing pass stores the kind of each bracketed expression in
	 * kmemo[], keyed by the index of its '('. An entry counts only while
	 * kgen[] equals gen. gen moves on at each new top-level expression,
	 * because the kind of a name can change between statements. edepth
	 * counts the levels of expression emission and typing, so 0 means a
	 * top-level expression.
	 */
	char	   *kmemo;
	int		   *kgen;
	int			gen;
	int			edepth;
	/*
	 * guards counts the shift guards (see GO_SHL_OPEN) open around the code
	 * being emitted. Each is a subquery, and PostgreSQL needs memory that
	 * grows faster than linearly to plan subqueries nested in one another.
	 */
	int			guards;
} Go;

static void go_stmt(Go *g, int ind);
static void go_block(Go *g, int ind);
static void go_emit_range(Go *g, int a, int b, StringInfo out);
static char go_expr_kind(Go *g, int a, int b);

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

/*
 * Go is case-sensitive, plpgsql is not: i and I are one name to plpgsql.
 * Pass every name the body declares or reads through the engine's spelling
 * check, which rejects a second spelling of a local or a parameter.
 */
static void
go_check_name(Go *g, const char *s, int len)
{
	(void) plx_local_find(g->cx, s, len);
	(void) plx_is_param(g->cx, s, len);
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

/* value of a hex digit, or -1 */
static int
go_hexval(char c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	if (c >= 'A' && c <= 'F')
		return c - 'A' + 10;
	return -1;
}

/*
 * A Go number literal st[0..len) as SQL text, or a plx error when Go would
 * reject it. Integer literals in base 16 (0x), 8 (0o or a leading 0) and 2
 * (0b) become decimal: PostgreSQL before 16 reads 0x1F as 0 followed by a
 * column alias, and 017 as decimal. Digit separators (_) are dropped.
 * Floats keep their text; hexadecimal floats and imaginary literals are not
 * supported.
 */
static char *
go_number(Go *g, const char *st, int len, int line)
{
	char	   *d = palloc(len + 1);
	int			n = 0,
				i,
				base = 10,
				from = 0;
	uint64		v = 0;
	bool		isfloat = false;

	for (i = 0; i < len; i++)				/* drop digit separators */
		if (st[i] != '_')
			d[n++] = st[i];
	d[n] = '\0';
	if (n > 1 && d[0] == '0' && (d[1] == 'x' || d[1] == 'X'))
		base = 16, from = 2;
	else if (n > 1 && d[0] == '0' && (d[1] == 'o' || d[1] == 'O'))
		base = 8, from = 2;
	else if (n > 1 && d[0] == '0' && (d[1] == 'b' || d[1] == 'B'))
		base = 2, from = 2;
	else
	{
		for (i = 0; i < n; i++)
			if (d[i] == '.' || d[i] == 'e' || d[i] == 'E')
				isfloat = true;
		if (isfloat)
		{
			/* digits, one '.', and an exponent with an optional sign */
			bool		dot = false,
						exp = false,
						digit = false;

			for (i = 0; i < n; i++)
			{
				char		c = d[i];

				if (c >= '0' && c <= '9')
					digit = true;
				else if (c == '.' && !dot && !exp)
					dot = true;
				else if ((c == 'e' || c == 'E') && !exp && digit &&
						 i + 1 < n)
				{
					exp = true;
					if (d[i + 1] == '+' || d[i + 1] == '-')
						i++;
					if (i + 1 >= n)
						break;
				}
				else
					break;
			}
			if (i < n || !digit || (n > 0 && (d[n - 1] == 'e' || d[n - 1] == 'E' ||
											  d[n - 1] == '+' || d[n - 1] == '-')))
				plx_err(g->cx, line, "invalid number literal \"%s\"", pnstrdup(st, len));
			return d;
		}
		if (n > 1 && d[0] == '0')
			base = 8, from = 1;			/* legacy octal: 017 */
	}
	if (from >= n)
		plx_err(g->cx, line, "invalid number literal \"%s\"", pnstrdup(st, len));
	for (i = from; i < n; i++)
	{
		int			dv = go_hexval(d[i]);

		if (dv < 0 || dv >= base)
			plx_err(g->cx, line, "invalid number literal \"%s\"", pnstrdup(st, len));
		if (v > (PG_UINT64_MAX - dv) / base)
			plx_err(g->cx, line, "number literal \"%s\" is too large", pnstrdup(st, len));
		v = v * base + dv;
	}
	if (base == 10)
		return d;
	return psprintf(UINT64_FORMAT, v);
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
		CHECK_FOR_INTERRUPTS();
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
			/*
			 * Take the whole run of letters, digits and dots, so that a
			 * letter Go would reject (12abc) is an error rather than an
			 * identifier glued to the number. A sign continues the literal
			 * only after a decimal exponent: in 0x1e+2 the e is a digit.
			 */
			const char *st = p;
			bool		hex = (p + 1 < end && p[0] == '0' && (p[1] == 'x' || p[1] == 'X'));
			char	   *txt;

			while (p < end && (GOALPHA(*p) || GODIGIT(*p) || *p == '.' ||
							   (!hex && (*p == '+' || *p == '-') &&
								(p[-1] == 'e' || p[-1] == 'E'))))
				p++;
			txt = go_number(g, st, (int) (p - st), line);
			GOPUSH(GO_NUM, txt, (int) strlen(txt));
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

	/* the bracket match table: one pass with a stack of open brackets */
	{
		int		   *stack = palloc(sizeof(int) * n);
		int			sp = 0,
					i;

		g->mt = palloc(sizeof(int) * n);
		for (i = 0; i < n; i++)
		{
			GoKind		k = t[i].kind;

			g->mt[i] = -1;
			if (k == GO_LP || k == GO_LBRACE || k == GO_LBRACK)
				stack[sp++] = i;
			else if ((k == GO_RP || k == GO_RBRACE || k == GO_RBRACK) && sp > 0)
				g->mt[stack[--sp]] = i;
		}
		pfree(stack);
		g->kmemo = palloc0(n);
		g->kgen = palloc0(sizeof(int) * n);
		g->gen = 1;
	}
#undef GOPUSH
#undef GOALPHA
#undef GODIGIT
}

/* append one decoded byte to a SQL literal, doubling a quote */
static void
go_str_byte(Go *g, GoTok *tk, StringInfo out, unsigned int c)
{
	if (c == 0)
		plx_err(g->cx, tk->line, "a string literal cannot contain a NUL character");
	if (c == '\'')
		appendStringInfoString(out, "''");
	else
		appendStringInfoChar(out, (char) c);
}

/* append one Unicode code point to a SQL literal, in the server encoding */
static void
go_str_cp(Go *g, GoTok *tk, StringInfo out, pg_wchar cp)
{
	unsigned char buf[64];

	if (cp < 0x80)
	{
		go_str_byte(g, tk, out, cp);
		return;
	}
	pg_unicode_to_server(cp, buf);
	appendStringInfoString(out, (char *) buf);
}

/*
 * emit a Go string/rune literal as a SQL string literal, decoding every Go
 * escape: \a \b \f \n \r \t \v \\ \' \", \xHH and \NNN (one byte each in a
 * string, a code point in a rune, as in Go), and \uHHHH and \UHHHHHHHH (a
 * Unicode code point, in the server encoding). Go
 * rejects any other escape, and so does plx. A decoded quote is doubled, so
 * an escape cannot end the SQL literal.
 */
static void
go_emit_str(Go *g, GoTok *tk, StringInfo out)
{
	char		q = tk->s[0];
	const char *s = tk->s + 1,
			   *e = tk->s + tk->len - 1;
	int			start;

	appendStringInfoChar(out, '\'');
	start = out->len;
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
					case 'a': appendStringInfoChar(out, '\a'); break;
					case 'b': appendStringInfoChar(out, '\b'); break;
					case 'f': appendStringInfoChar(out, '\f'); break;
					case 'v': appendStringInfoChar(out, '\v'); break;
					case '\\': appendStringInfoChar(out, '\\'); break;
					case '\'': appendStringInfoString(out, "''"); break;
					case '"': appendStringInfoChar(out, '"'); break;
					case 'x':
						if (s + 2 >= e || go_hexval(s[1]) < 0 || go_hexval(s[2]) < 0)
							plx_err(g->cx, tk->line, "\\x in a string literal needs two hexadecimal digits");
						if (q == '\'')
							go_str_cp(g, tk, out, go_hexval(s[1]) * 16 + go_hexval(s[2]));
						else
							go_str_byte(g, tk, out, go_hexval(s[1]) * 16 + go_hexval(s[2]));
						s += 2;
						break;
					case '0': case '1': case '2': case '3':
					case '4': case '5': case '6': case '7':
						{
							unsigned int v;

							if (s + 2 >= e || s[1] < '0' || s[1] > '7' ||
								s[2] < '0' || s[2] > '7')
								plx_err(g->cx, tk->line, "an octal escape in a string literal needs three octal digits");
							v = (s[0] - '0') * 64 + (s[1] - '0') * 8 + (s[2] - '0');
							if (v > 255)
								plx_err(g->cx, tk->line, "octal escape value in a string literal is above 255");
							if (q == '\'')
								go_str_cp(g, tk, out, v);
							else
								go_str_byte(g, tk, out, v);
							s += 2;
						}
						break;
					case 'u':
					case 'U':
						{
							int			n = (*s == 'u') ? 4 : 8,
										k;
							pg_wchar	cp = 0;

							if (s + n >= e)
								plx_err(g->cx, tk->line, "\\u needs four and \\U eight hexadecimal digits");
							for (k = 1; k <= n; k++)
							{
								if (go_hexval(s[k]) < 0)
									plx_err(g->cx, tk->line, "\\u needs four and \\U eight hexadecimal digits");
								cp = cp * 16 + go_hexval(s[k]);
							}
							if (cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF))
								plx_err(g->cx, tk->line, "invalid Unicode code point in a string literal");
							go_str_cp(g, tk, out, cp);
							s += n;
						}
						break;
					default:
						plx_err(g->cx, tk->line, "unknown escape sequence in a string literal");
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
		/* \x and octal escapes write single bytes; the result must still be text */
		if (!pg_verifymbstr(out->data + start, out->len - start, true))
			plx_err(g->cx, tk->line, "a string literal's escapes do not form valid text in the database encoding");
	}
	appendStringInfoChar(out, '\'');
}

/* true for a rune literal token ('x'), as opposed to a string literal */
static bool
go_is_rune(const GoTok *tk)
{
	return tk->kind == GO_STR && tk->s[0] == '\'';
}

/*
 * The value of a rune literal: its Unicode code point, as in Go, where a rune
 * is an int32. The literal is decoded as go_emit_str decodes it, and must
 * hold exactly one character.
 */
static int32
go_rune_value(Go *g, GoTok *tk)
{
	StringInfoData raw;
	char	   *c;
	int			n = 0,
				i;
	int32		cp;

	initStringInfo(&raw);
	go_emit_str(g, tk, &raw);	/* a quoted SQL literal, escapes decoded */
	c = palloc(raw.len);
	for (i = 1; i + 1 < raw.len; i++)	/* drop the quotes, undouble '' */
	{
		c[n++] = raw.data[i];
		if (raw.data[i] == '\'')
			i++;
	}
	c[n] = '\0';
	if (n == 0 || pg_mbstrlen_with_len(c, n) != 1)
		plx_err(g->cx, tk->line, "a rune literal must hold exactly one character");
	if ((unsigned char) c[0] < 0x80)
		cp = (unsigned char) c[0];
	else
	{
		unsigned char *u = (unsigned char *) pg_server_to_any(c, n, PG_UTF8);

		cp = (int32) utf8_to_unicode(u);
	}
	pfree(raw.data);
	pfree(c);
	return cp;
}

/*
 * Reject a format string that uses an explicit argument index (%[n]d). No
 * lowering models it: each would consume '[' as the verb, or take the
 * arguments in order, and print the wrong values. fn names the fmt function
 * for the message. Every path that reads a format string calls this.
 */
static void
go_check_argindex(Go *g, GoTok *tk, const char *fn)
{
	StringInfoData raw;
	int			i;

	initStringInfo(&raw);
	go_emit_str(g, tk, &raw);	/* quoted, escapes decoded */
	for (i = 1; i + 1 < raw.len; i++)
	{
		int			j = i + 1;

		if (raw.data[i] != '%')
			continue;
		if (raw.data[j] == '%')
		{
			i++;
			continue;
		}
		while (j + 1 < raw.len && strchr("-+ #0123456789.", raw.data[j]))
			j++;
		if (raw.data[j] == '[')
			plx_err(g->cx, tk->line,
					"fmt.%s explicit argument indexes (%%[n]) are not supported", fn);
	}
	pfree(raw.data);
}

/* emit a Go format string as one SQL format() accepts.

 * Go's verbs all render their operand as text, which is what format()'s %s
 * does, so every directive becomes %s. format() takes only a '-' flag and a
 * width, so Go's other flags and its precision field are dropped: the operand
 * still appears, just without the padding Go would have applied. A doubled
 * A doubled %% passes through, and a '%' that starts no directive is escaped
 * to %%, so it reaches the caller as a literal percent instead of tripping
 * format() at run time.
 */
static void
go_emit_format_str(Go *g, GoTok *tk, StringInfo out)
{
	StringInfoData raw;
	int			i;

	initStringInfo(&raw);
	go_emit_str(g, tk, &raw);	/* a quoted SQL literal, escapes decoded */

	for (i = 0; i < raw.len; i++)
	{
		char		c = raw.data[i];
		bool		dash = false;
		int			j,
					wstart,
					wlen;

		if (c != '%')
		{
			appendStringInfoChar(out, c);
			continue;
		}
		if (i + 1 < raw.len && raw.data[i + 1] == '%')
		{
			appendStringInfoString(out, "%%");
			i++;
			continue;
		}

		j = i + 1;
		while (j < raw.len && (raw.data[j] == '-' || raw.data[j] == '+' ||
							   raw.data[j] == ' ' || raw.data[j] == '#' ||
							   raw.data[j] == '0'))
		{
			if (raw.data[j] == '-')
				dash = true;
			j++;
		}
		wstart = j;
		while (j < raw.len && raw.data[j] >= '0' && raw.data[j] <= '9')
			j++;
		wlen = j - wstart;
		if (j < raw.len && raw.data[j] == '.')
		{
			j++;
			while (j < raw.len && raw.data[j] >= '0' && raw.data[j] <= '9')
				j++;
		}

		/*
		 * A * width or precision takes its value from an argument, which
		 * format() cannot model: every later directive would take the wrong
		 * argument.
		 */
		if (j < raw.len && raw.data[j] == '*')
			plx_err(g->cx, tk->line,
					"fmt.Sprintf with a * width or precision needs exactly one argument for each directive and each *, and at most 16");
		if (j >= raw.len ||
			!((raw.data[j] >= 'a' && raw.data[j] <= 'z') ||
			  (raw.data[j] >= 'A' && raw.data[j] <= 'Z')))
		{
			/* starts no directive: escape it, since a lone '%' is itself
			 * an error to format() */
			appendStringInfoString(out, "%%");
			continue;
		}

		/*
		 * format() renders every operand as text, which would print a rune's
		 * number for %c, not its character.
		 */
		if (raw.data[j] == 'c')
			plx_err(g->cx, tk->line,
					"fmt.Sprintf with %%c needs exactly one argument for each directive, and at most 16");
		appendStringInfoChar(out, '%');
		if (dash)
			appendStringInfoChar(out, '-');
		if (wlen > 0)
			appendBinaryStringInfo(out, raw.data + wstart, wlen);
		appendStringInfoChar(out, 's');
		i = j;
	}
	pfree(raw.data);
}

/*
 * Index of the verb of the fmt directive whose '%' is at raw->data[pct], after
 * skipping flags, width and precision (each may be a '*' that takes its value
 * from an argument); -1 when that character is not an ASCII letter (or the
 * directive runs into the closing quote).
 */
static int
go_sprintf_verb(StringInfo raw, int pct)
{
	int			j = pct + 1;

	while (j + 1 < raw->len && (raw->data[j] == '-' || raw->data[j] == '+' ||
								raw->data[j] == ' ' || raw->data[j] == '#' ||
								raw->data[j] == '0'))
		j++;
	if (j + 1 < raw->len && raw->data[j] == '*')
		j++;					/* the width comes from an argument */
	else
		while (j + 1 < raw->len && raw->data[j] >= '0' && raw->data[j] <= '9')
			j++;
	if (j + 1 < raw->len && raw->data[j] == '.')
	{
		j++;
		if (j + 1 < raw->len && raw->data[j] == '*')
			j++;				/* the precision comes from an argument */
		else
			while (j + 1 < raw->len && raw->data[j] >= '0' && raw->data[j] <= '9')
				j++;
	}
	if (j + 1 >= raw->len)
		return -1;
	if (!((raw->data[j] >= 'a' && raw->data[j] <= 'z') ||
		  (raw->data[j] >= 'A' && raw->data[j] <= 'Z')))
		return -1;
	return j;
}

/*
 * Emit the argument [a,b) of a fmt directive. For %c, Go prints the character
 * whose code point the integer argument holds (a rune), so wrap it in chr().
 */
static void
go_emit_verb_arg(Go *g, int a, int b, bool isc, StringInfo out)
{
	if (!isc)
	{
		go_emit_range(g, a, b, out);
		return;
	}
	appendStringInfoString(out, "chr((");
	go_emit_range(g, a, b, out);
	appendStringInfoString(out, ")::integer)");
}

/* fmt.Sprintf with a literal format string, lowered to a || chain.

 * format() renders a NULL operand as the empty string and cannot be made to do
 * otherwise, so a Sprintf lowered to format() silently turns a missing value
 * into a plausible-looking one. Concatenating instead lets NULL propagate the
 * way SQL || does. Width survives as lpad/rpad; the operand still renders as
 * text, so Go's representation verbs are unaffected by the change.
 *
 * A diagnostic message keeps a COALESCE per operand, so one NULL cannot swallow
 * the message. Returns false when the directives and arguments do not line up,
 * leaving the caller to fall back to format().
 */
static bool
go_emit_sprintf_concat(Go *g, int lp, int close, StringInfo out)
{
	StringInfoData raw,
				lit;
	int			as[16],
				ae[16];
	int			nargs = 0,
				ndir = 0,
				ai = 0,
				i,
				d,
				v,
				seg;
	bool		first = true;
	bool		diag = g->cx->diag_msg;

	/* arguments follow the format literal and its comma */
	i = lp + 2;
	if (i < close && g->t[i].kind == GO_COMMA)
	{
		i++;
		seg = i;
		d = 0;
		for (; i < close; i++)
		{
			GoKind		k = g->t[i].kind;

			if (k == GO_LP || k == GO_LBRACK || k == GO_LBRACE)
				d++;
			else if (k == GO_RP || k == GO_RBRACK || k == GO_RBRACE)
				d--;
			else if (k == GO_COMMA && d == 0)
			{
				if (nargs >= 16)
					return false;
				as[nargs] = seg;
				ae[nargs] = i;
				nargs++;
				seg = i + 1;
			}
		}
		if (seg < close)
		{
			if (nargs >= 16)
				return false;
			as[nargs] = seg;
			ae[nargs] = close;
			nargs++;
		}
	}

	initStringInfo(&raw);
	go_emit_str(g, &g->t[lp + 1], &raw);	/* quoted, escapes decoded */

	/* count directives so a mismatch can fall back before emitting anything */
	for (i = 1; i + 1 < raw.len; i++)
	{
		if (raw.data[i] != '%')
			continue;
		if (raw.data[i + 1] == '%')
		{
			i++;
			continue;
		}
		/*
		 * The verb must be a letter. raw is already a SQL literal ('
		 * doubled), so a quote here would otherwise be consumed as the verb
		 * and leave half of a '' pair unescaped in the output; fall back to
		 * the format() path, which validates verbs, instead.
		 */
		if ((v = go_sprintf_verb(&raw, i)) < 0)
		{
			pfree(raw.data);
			return false;
		}
		ndir++;
		for (d = i + 1; d < v; d++)	/* a * width or precision takes one */
			if (raw.data[d] == '*')
				ndir++;
	}
	if (ndir != nargs)
	{
		pfree(raw.data);
		return false;
	}

	initStringInfo(&lit);
	go_sp(out);
	appendStringInfoChar(out, '(');
	for (i = 1; i + 1 < raw.len; i++)
	{
		char		c = raw.data[i];
		bool		dash = false,
					isc,
					starw = false;
		int			j,
					wstart,
					wlen,
					wa = -1;

		if (c != '%')
		{
			appendStringInfoChar(&lit, c);
			continue;
		}
		if (raw.data[i + 1] == '%')
		{
			appendStringInfoChar(&lit, '%');	/* a literal percent needs no
												 * escape outside format() */
			i++;
			continue;
		}

		j = i + 1;
		while (j + 1 < raw.len && (raw.data[j] == '-' || raw.data[j] == '+' ||
								   raw.data[j] == ' ' || raw.data[j] == '#' ||
								   raw.data[j] == '0'))
		{
			if (raw.data[j] == '-')
				dash = true;
			j++;
		}
		wstart = j;
		if (j + 1 < raw.len && raw.data[j] == '*')
		{
			starw = true;
			wa = ai++;			/* the width is the next argument */
			j++;
		}
		else
			while (j + 1 < raw.len && raw.data[j] >= '0' && raw.data[j] <= '9')
				j++;
		wlen = j - wstart;
		if (j + 1 < raw.len && raw.data[j] == '.')
		{
			j++;
			if (j + 1 < raw.len && raw.data[j] == '*')
			{
				/* precision is dropped (see above), but it takes an argument */
				ai++;
				j++;
			}
			else
				while (j + 1 < raw.len && raw.data[j] >= '0' && raw.data[j] <= '9')
					j++;
		}

		/* flush the literal run before the value */
		if (lit.len > 0)
		{
			if (!first)
				appendStringInfoString(out, " || ");
			appendStringInfoChar(out, '\'');
			appendBinaryStringInfo(out, lit.data, lit.len);
			appendStringInfoChar(out, '\'');
			resetStringInfo(&lit);
			first = false;
		}

		if (!first)
			appendStringInfoString(out, " || ");
		if (diag)
			appendStringInfoString(out, "COALESCE(");
		isc = (raw.data[j] == 'c');
		if (starw)
		{
			/*
			 * A width from an argument: as in Go, a negative width pads on
			 * the right, like the '-' flag. Value and width are evaluated
			 * once, in a subquery.
			 */
			appendStringInfoString(out, "(SELECT CASE WHEN __plx_n < 0 THEN "
								   "rpad(__plx_v, greatest(-__plx_n, length(__plx_v))) ELSE ");
			appendStringInfoString(out, dash ? "rpad" : "lpad");
			appendStringInfoString(out, "(__plx_v, greatest(__plx_n, length(__plx_v))) END "
								   "FROM (VALUES ((");
			go_emit_verb_arg(g, as[ai], ae[ai], isc, out);
			appendStringInfoString(out, ")::text, (");
			go_emit_range(g, as[wa], ae[wa], out);
			appendStringInfoString(out, ")::integer)) AS __plx_w(__plx_v, __plx_n))");
		}
		else if (wlen == 0)
		{
			appendStringInfoChar(out, '(');
			go_emit_verb_arg(g, as[ai], ae[ai], isc, out);
			appendStringInfoString(out, ")::text");
		}
		else if (ae[ai] - as[ai] == 1 && g->t[as[ai]].kind != GO_LP)
		{
			/*
			 * A Go width is a minimum and never cuts, but lpad/rpad cut a
			 * longer string to the width, so pad to greatest(width, length).
			 * A single-token operand can safely be written twice.
			 */
			appendStringInfoString(out, dash ? "rpad((" : "lpad((");
			go_emit_verb_arg(g, as[ai], ae[ai], isc, out);
			appendStringInfoString(out, ")::text, greatest(");
			appendBinaryStringInfo(out, raw.data + wstart, wlen);
			appendStringInfoString(out, ", length((");
			go_emit_verb_arg(g, as[ai], ae[ai], isc, out);
			appendStringInfoString(out, ")::text)))");
		}
		else
		{
			/* any other operand is evaluated once, in a subquery */
			appendStringInfoString(out, dash ? "(SELECT rpad(__plx_v, greatest("
								   : "(SELECT lpad(__plx_v, greatest(");
			appendBinaryStringInfo(out, raw.data + wstart, wlen);
			appendStringInfoString(out, ", length(__plx_v))) FROM (VALUES ((");
			go_emit_verb_arg(g, as[ai], ae[ai], isc, out);
			appendStringInfoString(out, ")::text)) AS __plx_w(__plx_v))");
		}
		if (diag)
			appendStringInfoString(out, ", '')");
		first = false;
		ai++;
		i = j;
	}
	if (lit.len > 0)
	{
		if (!first)
			appendStringInfoString(out, " || ");
		appendStringInfoChar(out, '\'');
		appendBinaryStringInfo(out, lit.data, lit.len);
		appendStringInfoChar(out, '\'');
		first = false;
	}
	if (first)								/* an empty format string */
		appendStringInfoString(out, "''");
	appendStringInfoChar(out, ')');
	pfree(raw.data);
	pfree(lit.data);
	return true;
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
	g->arrlen = -1;
	if (go_cur(g)->kind == GO_LBRACK)			/* []T or [N]T -> T[] */
	{
		GoTok	   *n;

		g->pos++;
		n = go_cur(g);
		if (n->kind == GO_NUM && g->t[g->pos + 1].kind == GO_RBRACK &&
			n->len <= 9 && strspn(n->s, "0123456789") >= (size_t) n->len)
			g->arrlen = atoi(pnstrdup(n->s, n->len));
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

/* true when a Go numeric literal is a float (has a '.' or a decimal exponent) */
static bool
go_num_is_float(const GoTok *tk)
{
	int			i;

	if (tk->len > 1 && tk->s[0] == '0' && (tk->s[1] == 'x' || tk->s[1] == 'X'))
		return false;			/* hex: 'e' and 'E' are digits */
	for (i = 0; i < tk->len; i++)
		if (tk->s[i] == '.' || tk->s[i] == 'e' || tk->s[i] == 'E')
			return true;
	return false;
}

/* infer a PostgreSQL type from an initializer range [a,b); NULL if unknown */
static const char *
go_infer_type(Go *g, int a, int b)
{
	if (b - a == 1)
	{
		GoTok	   *tk = &g->t[a];

		if (tk->kind == GO_NUM)
			return go_num_is_float(tk) ? "double precision" : "integer";
		if (tk->kind == GO_STR)
			return go_is_rune(tk) ? "integer" : "text";
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
	/* a call that returns a string, or a concatenation */
	if (go_expr_kind(g, a, b) == 's')
		return "text";
	return NULL;
}

/*
 * index of the ')' / '}' / ']' matching the opener at idx, within [.,limit).
 * Brackets of any kind count alike. For an opener, the table go_lex built
 * gives the same answer as the scan below, in constant time.
 */
static int
go_match(Go *g, int idx, int limit)
{
	int			depth = 0,
				i;
	GoKind		k0;

	if (idx >= limit)
		return -1;
	k0 = g->t[idx].kind;
	if (k0 == GO_LP || k0 == GO_LBRACE || k0 == GO_LBRACK)
		return (g->mt[idx] >= 0 && g->mt[idx] < limit) ? g->mt[idx] : -1;
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

/*
 * What the operand [a,b) of an integer conversion is known to be: 'i' for an
 * integer (an integer literal, or a local or parameter of an integer type),
 * 'f' for a value trunc() handles exactly (a float or numeric literal, local
 * or parameter), and 0 when plx cannot tell (an expression, a call, a row
 * field). A row field carries no Go type, so it can be anything.
 */
static char
go_operand_kind(Go *g, int a, int b)
{
	GoTok	   *tk = &g->t[a];
	PlxLocal2  *l;
	int			i;

	if (b - a != 1)
		return 0;
	if (tk->kind == GO_NUM)
		return go_num_is_float(tk) ? 'f' : 'i';
	if (go_is_rune(tk))
		return 'i';
	if (tk->kind != GO_WORD)
		return 0;
	l = plx_local_find(g->cx, tk->s, tk->len);
	if (l)
	{
		if (!l->typ)
			return 0;
		if (strcmp(l->typ, "integer") == 0 || strcmp(l->typ, "smallint") == 0 ||
			strcmp(l->typ, "bigint") == 0)
			return 'i';
		if (strcmp(l->typ, "double precision") == 0 ||
			strcmp(l->typ, "real") == 0 || strcmp(l->typ, "numeric") == 0)
			return 'f';
		return 0;
	}
	if (!g->cx->meta || !g->cx->meta->argnames)
		return 0;
	for (i = 0; i < g->cx->meta->nargs; i++)
	{
		const char *an = g->cx->meta->argnames[i];

		if (an && (int) strlen(an) == tk->len && strncmp(an, tk->s, tk->len) == 0)
		{
			Oid			t = g->cx->meta->argtypes[i];

			if (t == INT2OID || t == INT4OID || t == INT8OID)
				return 'i';
			if (t == FLOAT4OID || t == FLOAT8OID || t == NUMERICOID)
				return 'f';
			return 0;
		}
	}
	return 0;
}

static bool go_op_is(const GoTok *tk, const char *op);
static int	go_binop_prec(const GoTok *tk);
static int	go_split_binops(Go *g, int a, int b, int *ops);
static char go_name_kind(Go *g, const GoTok *tk);
static void go_kind_enter(Go *g);
static void go_kind_leave(Go *g);
static char go_int_class(Go *g, int a, int b);

/* go_int_class of a value of the PostgreSQL type typ */
static char
go_typ_int_class(const char *typ)
{
	if (!typ)
		return 0;
	if (strcmp(typ, "integer") == 0 || strcmp(typ, "smallint") == 0 ||
		strcmp(typ, "bigint") == 0)
		return 'i';
	if (strcmp(typ, "real") == 0 || strcmp(typ, "double precision") == 0 ||
		strcmp(typ, "numeric") == 0 || strcmp(typ, "boolean") == 0)
		return 'x';
	return 0;
}

/*
 * go_int_class of one operand [a,b) with no top-level binary operator: a
 * literal, a name, a bracketed expression, a slice element, a conversion or a
 * call plx maps.
 */
static char
go_int_operand_class(Go *g, int a, int b)
{
	GoTok	   *tk = &g->t[a];
	char		c;

	c = go_operand_kind(g, a, b);
	if (c)
		return c == 'i' ? 'i' : 'x';
	if (b - a == 1)
	{
		if (go_ci(tk, "true") || go_ci(tk, "false"))
			return 'x';
		/* go_operand_kind has typed the integer and float names already */
		if (tk->kind == GO_WORD && go_name_kind(g, tk) == 'n')
			return 'x';
		return 0;
	}
	if (go_op_is(tk, "!"))
		return 'x';
	/* a bracketed expression */
	if (tk->kind == GO_LP && go_match(g, a, b) == b - 1)
	{
		go_kind_enter(g);
		c = go_int_class(g, a + 1, b - 1);
		go_kind_leave(g);
		return c;
	}
	/* an element s[i] of a slice local */
	if (tk->kind == GO_WORD && g->t[a + 1].kind == GO_LBRACK &&
		go_match(g, a + 1, b) == b - 1)
	{
		PlxLocal2  *l = plx_local_find(g->cx, tk->s, tk->len);
		int			tl = (l && l->typ) ? (int) strlen(l->typ) : 0;

		if (tl > 2 && strcmp(l->typ + tl - 2, "[]") == 0)
			return go_typ_int_class(pnstrdup(l->typ, tl - 2));
		return 0;
	}
	/* pkg.Func(...): only the calls plx maps */
	if (tk->kind == GO_WORD && b - a >= 5 && g->t[a + 1].kind == GO_DOT &&
		g->t[a + 2].kind == GO_WORD && g->t[a + 3].kind == GO_LP &&
		go_match(g, a + 3, b) == b - 1)
	{
		GoTok	   *m = &g->t[a + 2];

		if (go_ci(tk, "strconv") && go_ci(m, "Atoi"))
			return 'i';
		if (go_ci(tk, "strings") && go_ci(m, "Contains"))
			return 'x';
		if (go_ci(tk, "math"))
			return 'x';
		return 0;
	}
	/* T(x) for a base type T, and len(x) / cap(x) */
	if (tk->kind == GO_WORD && g->t[a + 1].kind == GO_LP &&
		go_match(g, a + 1, b) == b - 1)
	{
		const char *conv = go_base_type(tk->s, tk->len);

		if (conv)
			return go_typ_int_class(conv);
		if (go_ci(tk, "len") || go_ci(tk, "cap"))
			return 'i';
	}
	return 0;
}

/*
 * Whether [a,b) is an integer, which string(x) needs to know: 'i' when plx can
 * see it is an integer, 'x' when it can see it is a float, a numeric or a
 * bool, and 0 when it cannot tell (a row field, an unmapped call). Operands
 * joined by arithmetic or bitwise operators are an integer when all of them
 * are, and a float, numeric or bool when any of them is.
 */
static char
go_int_class(Go *g, int a, int b)
{
	int		   *ops;
	int			n,
				k;
	char		r = 'i';

	/* unary signs and ^ keep the class */
	while (a < b && (go_op_is(&g->t[a], "-") || go_op_is(&g->t[a], "+") ||
					 go_op_is(&g->t[a], "^")))
		a++;
	if (b <= a)
		return 0;
	n = go_split_binops(g, a, b, NULL);
	if (n < 0)
		return 0;
	if (n == 0)
		return go_int_operand_class(g, a, b);
	ops = palloc(sizeof(int) * n);
	go_split_binops(g, a, b, ops);
	for (k = 0; k < n; k++)
		if (go_binop_prec(&g->t[ops[k]]) <= 3)
			r = 'x';			/* a comparison, && or ||: a bool */
	for (k = 0; k <= n && r != 'x'; k++)
	{
		char		c = go_int_class(g, k == 0 ? a : ops[k - 1] + 1,
									 k == n ? b : ops[k]);

		if (c != 'i')
			r = c;
	}
	pfree(ops);
	return r;
}

/* a stdlib / builtin / type-conversion call at index i; sets *ni on success */
static bool
go_emit_call(Go *g, int i, int b, StringInfo out, int *ni)
{
	GoTok	   *tk = &g->t[i];
	int			lp,
				close;
	const char *conv;
	char		strc = 0;		/* go_int_class of the operand of string(x) */

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

		/* fmt.Sprintf in expression position: reproduce the format string,
		 * rewriting Go's verbs into the ones SQL format() understands */
		if (go_ci(pkg, "fmt") && go_ci(meth, "Sprintf") &&
			close > lp + 1 && g->t[lp + 1].kind == GO_STR)
		{
			go_check_argindex(g, &g->t[lp + 1], "Sprintf");
			if (go_emit_sprintf_concat(g, lp, close, out))
			{
				*ni = close + 1;
				return true;
			}
			go_sp(out);
			appendStringInfoString(out, "format(");
			go_emit_format_str(g, &g->t[lp + 1], out);
			if (lp + 2 < close)
				go_emit_range(g, lp + 2, close, out);
			appendStringInfoChar(out, ')');
			*ni = close + 1;
			return true;
		}

		for (k = 0; r[k].pkg; k++)
			if ((int) strlen(r[k].pkg) == pkg->len &&
				strncmp(r[k].pkg, pkg->s, pkg->len) == 0 &&
				(int) strlen(r[k].meth) == meth->len &&
				strncmp(r[k].meth, meth->s, meth->len) == 0)
			{
				/* strconv.Itoa(x) -> (x)::text ; strconv.Atoi(x) -> (x)::integer */
				if (r[k].pg == NULL)
				{
					bool		toint = (strncmp(meth->s, "Atoi", 4) == 0);

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

	/*
	 * type conversion T(x) -> (x)::T for a base type T. Go converts a float
	 * to an integer type by truncating toward zero, but a SQL cast rounds, so
	 * an integer conversion of an operand not known to be an integer goes
	 * through trunc() first.
	 */
	conv = go_base_type(tk->s, tk->len);
	/*
	 * string(x) of an integer (such as a rune) is the character with that
	 * code point in Go, not its decimal digits. Go rejects string(x) of a
	 * float or a bool, and so does plx, rather than print a character for it.
	 */
	if (conv && strcmp(conv, "text") == 0)
		strc = go_int_class(g, lp + 1, close);
	if (strc == 'x')
		plx_err(g->cx, tk->line, "string(x) of a float, numeric or bool value "
				"does not compile in Go; format it with fmt.Sprintf");
	if (strc == 'i')
	{
		go_sp(out);
		appendStringInfoString(out, "chr((");
		go_emit_range(g, lp + 1, close, out);
		appendStringInfoString(out, ")::integer)");
		*ni = close + 1;
		return true;
	}
	if (conv)
	{
		char		kind = 'i';

		if (strcmp(conv, "integer") == 0 || strcmp(conv, "smallint") == 0 ||
			strcmp(conv, "bigint") == 0)
			kind = go_operand_kind(g, lp + 1, close);
		go_sp(out);
		if (kind == 0)
		{
			/*
			 * The operand's type is known only at run time. Evaluate it once
			 * and choose by its type: an integer casts exactly, a float
			 * truncates as a float, and anything else (numeric, text)
			 * truncates as numeric, which is exact at any size.
			 */
			appendStringInfoString(out, "(SELECT CASE WHEN pg_typeof(__plx_v) IN "
								   "('smallint'::regtype, 'integer'::regtype, 'bigint'::regtype) ");
			appendStringInfo(out, "THEN __plx_v::%s WHEN pg_typeof(__plx_v) IN "
							 "('real'::regtype, 'double precision'::regtype) ", conv);
			appendStringInfo(out, "THEN trunc(__plx_v::double precision)::%s "
							 "ELSE trunc(__plx_v::numeric)::%s END FROM (VALUES ((", conv, conv);
			go_emit_range(g, lp + 1, close, out);
			appendStringInfoString(out, "))) AS __plx_n(__plx_v))");
			*ni = close + 1;
			return true;
		}
		if (kind == 'f')
			appendStringInfoString(out, "trunc");
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
			? plx_local_find(g->cx, g->t[lp + 1].s, g->t[lp + 1].len) : NULL;
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

/* token ends an operand, so an operator after it is binary */
static bool
go_operand_end(const GoTok *tk)
{
	return tk->kind == GO_WORD || tk->kind == GO_NUM || tk->kind == GO_STR ||
		tk->kind == GO_RP || tk->kind == GO_RBRACK || tk->kind == GO_RBRACE;
}

static bool
go_op_is(const GoTok *tk, const char *op)
{
	int			l = (int) strlen(op);

	return tk->kind == GO_OP && tk->len == l && strncmp(tk->s, op, l) == 0;
}

/* Go precedence of a binary operator token (5 binds tightest); 0 if none */
static int
go_binop_prec(const GoTok *tk)
{
	static const char *const p5[] = {"*", "/", "%", "<<", ">>", "&", "&^", NULL};
	static const char *const p4[] = {"+", "-", "|", "^", NULL};
	static const char *const p3[] = {"==", "!=", "<", "<=", ">", ">=", NULL};
	int			k;

	for (k = 0; p5[k]; k++)
		if (go_op_is(tk, p5[k]))
			return 5;
	for (k = 0; p4[k]; k++)
		if (go_op_is(tk, p4[k]))
			return 4;
	for (k = 0; p3[k]; k++)
		if (go_op_is(tk, p3[k]))
			return 3;
	if (go_op_is(tk, "&&"))
		return 2;
	if (go_op_is(tk, "||"))
		return 1;
	return 0;
}

static bool
go_is_bitop(const GoTok *tk)
{
	return go_op_is(tk, "<<") || go_op_is(tk, ">>") || go_op_is(tk, "&") ||
		go_op_is(tk, "|") || go_op_is(tk, "^") || go_op_is(tk, "&^");
}

/*
 * True when [a,b) uses a binary bitwise operator outside any bracket. SQL
 * ranks << >> & | below + and -, while Go ranks << >> & &^ with * and | ^
 * with +; such a range is emitted fully parenthesized (go_emit_prec).
 */
static bool
go_needs_prec(Go *g, int a, int b)
{
	int			i,
				depth = 0;

	for (i = a; i < b; i++)
	{
		GoKind		k = g->t[i].kind;

		if (k == GO_LP || k == GO_LBRACK || k == GO_LBRACE)
			depth++;
		else if (k == GO_RP || k == GO_RBRACK || k == GO_RBRACE)
			depth--;
		else if (depth == 0 && i > a && go_is_bitop(&g->t[i]) &&
				 go_operand_end(&g->t[i - 1]))
			return true;
	}
	return false;
}

/*
 * SQL precedence class of a Go binary operator. Operators of one class
 * associate to the left in SQL as in Go, so a run of them needs no brackets
 * of its own. 'c' (comparison) never chains in SQL, so it gets brackets each
 * time.
 */
static char
go_sql_class(const GoTok *tk)
{
	if (go_op_is(tk, "*") || go_op_is(tk, "/") || go_op_is(tk, "%"))
		return 'm';
	if (go_op_is(tk, "+") || go_op_is(tk, "-"))
		return 'a';
	if (go_op_is(tk, "&&"))
		return 'A';
	if (go_op_is(tk, "||"))
		return 'O';
	if (go_binop_prec(tk) == 3)
		return 'c';
	return 'o';					/* << >> & &^ | ^: SQL's "other" operators */
}

/*
 * [a,b) is an integer constant expression: integer and rune literals joined by
 * arithmetic and bitwise operators, with unary signs and brackets. It holds no
 * name, call or float, so Go types it by its context.
 */
static bool
go_int_const(Go *g, int a, int b)
{
	static const char *const ops[] = {
		"+", "-", "*", "/", "%", "<<", ">>", "&", "|", "^", "&^", NULL
	};
	bool		lit = false;
	int			i,
				k;

	for (i = a; i < b; i++)
	{
		GoTok	   *tk = &g->t[i];

		CHECK_FOR_INTERRUPTS();
		if ((tk->kind == GO_NUM && !go_num_is_float(tk)) || go_is_rune(tk))
		{
			lit = true;
			continue;
		}
		if (tk->kind == GO_LP || tk->kind == GO_RP)
			continue;
		for (k = 0; ops[k]; k++)
			if (go_op_is(tk, ops[k]))
				break;
		if (!ops[k])
			return false;
	}
	return lit;
}

/*
 * A shift whose count is not a small literal is emitted as a subquery that
 * evaluates the value (__plx_a) and the count (__plx_c) once each and gives
 * Go's result for every count. SQL takes the count mod 64 for a bigint and
 * mod 32 for a smallint or integer, so 1 << 64 would be 1; Go gives 0 for <<,
 * and 0 or -1 for >>. A negative count panics in Go, and fails here. plx maps
 * Go's 64-bit int to a 32-bit integer, so a non-zero integer shifted left by
 * 32 or more fails with "integer out of range", as any shift that leaves the
 * range of an int does. Neither error arm can be folded into an error while
 * the planner simplifies the CASE, even with a constant value and count, as
 * custom plans have: each one depends on || or pg_typeof(), which are stable.
 * GO_SHIFT_WIDTH is the width of the value in bits. OFFSET 0 keeps the
 * planner from pulling the subquery up, which would copy the value into each
 * place __plx_a appears: shifts nested n deep would then grow as 6^n.
 */
#define GO_MAX_GUARDS 64
#define GO_GUARDS_MSG "too many shifts by a count that is not a literal, nested in one another; split the expression"
#define GO_SHIFT_WIDTH "CASE WHEN pg_typeof(__plx_a) = 'bigint'::regtype THEN 64 ELSE 32 END"
static const char GO_SHL_OPEN[] =
"(SELECT CASE WHEN __plx_c IS NULL THEN NULL"
" WHEN __plx_c < 0 THEN ('plxgo: negative shift amount ' || __plx_c)::integer"
" WHEN __plx_c < " GO_SHIFT_WIDTH " THEN __plx_a << __plx_c::integer"
" WHEN __plx_a = 0 OR pg_typeof(__plx_a) = 'bigint'::regtype THEN __plx_a & 0"
" ELSE (__plx_a::bigint << " GO_SHIFT_WIDTH ")::integer"
" END FROM (SELECT (";
static const char GO_SHR_OPEN[] =
"(SELECT CASE WHEN __plx_c IS NULL THEN NULL"
" WHEN __plx_c < 0 THEN ('plxgo: negative shift amount ' || __plx_c)::integer"
" ELSE __plx_a >> least(__plx_c, " GO_SHIFT_WIDTH " - 1)::integer"
" END FROM (SELECT (";
static const char GO_SHIFT_MID[] = ") AS __plx_a, (";
static const char GO_SHIFT_CLOSE[] = ") AS __plx_c OFFSET 0) AS __plx_s)";

/*
 * [a,b) is a decimal literal below 32, a shift count that SQL takes as Go
 * does for a value of any width, so the shift needs no guard.
 */
static bool
go_small_count(Go *g, int a, int b)
{
	GoTok	   *tk = &g->t[a];

	if (b - a != 1 || tk->kind != GO_NUM || tk->len > 2 ||
		(tk->len == 2 && tk->s[0] == '0'))
		return false;
	if (tk->s[0] < '0' || tk->s[0] > '9' ||
		(tk->len == 2 && (tk->s[1] < '0' || tk->s[1] > '9')))
		return false;
	return atoi(pnstrdup(tk->s, tk->len)) < 32;
}

/*
 * Emit operands lo..hi of a split binary expression, nesting by Go
 * precedence: os/oe bound each operand's tokens and op[k] is the operator
 * between operands k and k+1. Each Go level goes in brackets, with an inner
 * bracket wherever the SQL class changes, since Go ranks + - | ^ alike but
 * SQL does not: a | b + c becomes ((a | b) + c). Each level recurses only into
 * tighter levels, so the recursion is at most five deep per bracket.
 */
static void
go_emit_binary(Go *g, const int *os, const int *oe, const int *op, int lo, int hi,
			   StringInfo out)
{
	int			minp = 99,
				k,
				m = 0,
				j;
	int		   *sp;
	bool	   *brk;
	char	   *guard;			/* '<' or '>' for a guarded shift, else 0 */
	int			nguard = 0;

	if (lo == hi)
	{
		go_emit_range(g, os[lo], oe[lo], out);
		return;
	}
	for (k = lo; k < hi; k++)
		minp = Min(minp, go_binop_prec(&g->t[op[k]]));
	sp = palloc(sizeof(int) * (hi - lo));
	brk = palloc0(sizeof(bool) * (hi - lo));
	guard = palloc0(hi - lo);
	for (k = lo; k < hi; k++)
		if (go_binop_prec(&g->t[op[k]]) == minp)
			sp[m++] = k;
	for (j = 0; j < m; j++)
	{
		GoTok	   *o = &g->t[op[sp[j]]];

		if (j > 0)
		{
			char		c = go_sql_class(o);

			brk[j] = (c == 'c' || c != go_sql_class(&g->t[op[sp[j - 1]]]));
		}
		if ((go_op_is(o, "<<") || go_op_is(o, ">>")) &&
			!go_small_count(g, os[sp[j] + 1], oe[j + 1 < m ? sp[j + 1] : hi]))
		{
			guard[j] = o->s[0];
			nguard++;
		}
	}
	/* each guard nests a subquery around the shifts before it */
	if (g->guards + nguard > GO_MAX_GUARDS)
		go_err(g, GO_GUARDS_MSG);
	g->guards += nguard;
	go_sp(out);
	appendStringInfoChar(out, '(');
	/*
	 * Open the bracket or guard that ends at each operator. The one that ends
	 * first is innermost, so they open from the last operator back.
	 */
	for (j = m - 1; j >= 0; j--)
	{
		if (guard[j])
			appendStringInfoString(out, guard[j] == '<' ? GO_SHL_OPEN : GO_SHR_OPEN);
		else if (j > 0 && brk[j])
			appendStringInfoChar(out, '(');
	}
	/*
	 * An integer constant shifted by << or >>: Go gives the untyped constant
	 * the type of its context, and Go's int64 is 64 bits wide. SQL types the
	 * literal as a 32-bit integer and takes the shift count mod 32, so 1 << 32
	 * is 1. Shift a bigint instead; a 32-bit target that cannot hold the
	 * result then fails with "integer out of range" instead of wrapping. The
	 * constant can be an expression, as in 2 * 1 << 40, which Go reads as
	 * (2 * 1) << 40: making its first operand a bigint makes it all bigint.
	 * Shifts bind tightest, with * and &, so they are all at this level.
	 */
	for (j = 0; j < m; j++)
		if (go_op_is(&g->t[op[sp[j]]], "<<") || go_op_is(&g->t[op[sp[j]]], ">>"))
			break;
	if (j < m && lo == sp[0] && go_int_const(g, os[lo], oe[sp[j]]))
	{
		go_sp(out);
		appendStringInfoChar(out, '(');
		go_emit_range(g, os[lo], oe[lo], out);
		appendStringInfoString(out, ")::bigint");
	}
	else
		go_emit_binary(g, os, oe, op, lo, sp[0], out);
	for (j = 0; j < m; j++)
	{
		GoTok	   *o = &g->t[op[sp[j]]];
		bool		andnot = go_op_is(o, "&^");

		if (guard[j])
		{
			appendStringInfoString(out, GO_SHIFT_MID);
			go_emit_binary(g, os, oe, op, sp[j] + 1, j + 1 < m ? sp[j + 1] : hi, out);
			appendStringInfoString(out, GO_SHIFT_CLOSE);
			continue;
		}
		if (j > 0 && brk[j])
			appendStringInfoChar(out, ')');
		go_sp(out);
		if (go_op_is(o, "=="))
			appendStringInfoChar(out, '=');
		else if (go_op_is(o, "!="))
			appendStringInfoString(out, "<>");
		else if (go_op_is(o, "&&"))
			appendStringInfoString(out, "AND");
		else if (go_op_is(o, "||"))
			appendStringInfoString(out, "OR");
		else if (go_op_is(o, "^"))
			appendStringInfoChar(out, '#');		/* XOR: SQL's ^ is a power */
		else if (andnot)
			appendStringInfoString(out, "& ~(");	/* AND NOT */
		else
			appendBinaryStringInfo(out, o->s, o->len);
		go_emit_binary(g, os, oe, op, sp[j] + 1, j + 1 < m ? sp[j + 1] : hi, out);
		if (andnot)
			appendStringInfoChar(out, ')');
	}
	appendStringInfoChar(out, ')');
	g->guards -= nguard;
	pfree(sp);
	pfree(brk);
	pfree(guard);
}

/*
 * Emit [a,b), which go_needs_prec flagged, fully parenthesized by Go
 * precedence. A top-level comma list (call arguments, slice elements) is
 * emitted element by element. Returns false, emitting nothing, when the
 * range holds an operator this does not model; the caller then emits the
 * tokens as written.
 */
static bool
go_emit_prec(Go *g, int a, int b, StringInfo out)
{
	int			i,
				depth = 0,
				n = 0,
				start = a;
	int		   *os,
			   *oe,
			   *op;

	for (i = a; i < b; i++)
	{
		GoKind		k = g->t[i].kind;

		if (k == GO_LP || k == GO_LBRACK || k == GO_LBRACE)
			depth++;
		else if (k == GO_RP || k == GO_RBRACK || k == GO_RBRACE)
			depth--;
		else if (depth == 0 && k == GO_COMMA)
		{
			int			s = a,
						j;

			/* a comma list: emit each element on its own */
			depth = 0;
			for (j = a; j <= b; j++)
			{
				GoKind		kk = (j < b) ? g->t[j].kind : GO_COMMA;

				if (kk == GO_LP || kk == GO_LBRACK || kk == GO_LBRACE)
					depth++;
				else if (kk == GO_RP || kk == GO_RBRACK || kk == GO_RBRACE)
					depth--;
				else if (depth == 0 && kk == GO_COMMA)
				{
					if (s > a)
						appendStringInfoChar(out, ',');
					go_emit_range(g, s, j, out);
					s = j + 1;
				}
			}
			return true;
		}
	}

	os = palloc(sizeof(int) * (b - a + 1));
	oe = palloc(sizeof(int) * (b - a + 1));
	op = palloc(sizeof(int) * (b - a + 1));
	depth = 0;
	for (i = a; i < b; i++)
	{
		GoKind		k = g->t[i].kind;

		if (k == GO_LP || k == GO_LBRACK || k == GO_LBRACE)
			depth++;
		else if (k == GO_RP || k == GO_RBRACK || k == GO_RBRACE)
			depth--;
		else if (depth == 0 && k == GO_OP && i > start &&
				 go_operand_end(&g->t[i - 1]))
		{
			if (go_binop_prec(&g->t[i]) == 0)
			{
				pfree(os);
				pfree(oe);
				pfree(op);
				return false;
			}
			os[n] = start;
			oe[n] = i;
			op[n] = i;
			n++;
			start = i + 1;
		}
	}
	if (start >= b)						/* dangling operator: emit as written */
	{
		pfree(os);
		pfree(oe);
		pfree(op);
		return false;
	}
	os[n] = start;
	oe[n] = b;
	go_emit_binary(g, os, oe, op, 0, n, out);
	pfree(os);
	pfree(oe);
	pfree(op);
	return true;
}

/*
 * End (exclusive) of the operand of a unary operator that starts at i: any
 * further unary operators, then a name, number, string or bracketed group,
 * then any field selections, calls and subscripts. -1 when the tokens form no
 * operand this recognises.
 */
static int
go_unary_operand_end(Go *g, int i, int b)
{
	while (i < b && (go_op_is(&g->t[i], "-") || go_op_is(&g->t[i], "+") ||
					 go_op_is(&g->t[i], "^") || go_op_is(&g->t[i], "!")))
		i++;
	if (i >= b)
		return -1;
	if (g->t[i].kind == GO_LP)
	{
		i = go_match(g, i, b);
		if (i < 0)
			return -1;
		i++;
	}
	else if (g->t[i].kind == GO_WORD || g->t[i].kind == GO_NUM ||
			 g->t[i].kind == GO_STR)
		i++;
	else
		return -1;
	while (i < b)
	{
		if (g->t[i].kind == GO_DOT && i + 1 < b && g->t[i + 1].kind == GO_WORD)
			i += 2;
		else if (g->t[i].kind == GO_LP || g->t[i].kind == GO_LBRACK)
		{
			int			m = go_match(g, i, b);

			if (m < 0)
				return -1;
			i = m + 1;
		}
		else
			break;
	}
	return i;
}

/* emit tokens [a,b) as a plpgsql expression, token by token */
static void
go_emit_tokens(Go *g, int a, int b, StringInfo out)
{
	int			i = a;

	while (i < b)
	{
		GoTok	   *tk = &g->t[i];
		int			ni;

		CHECK_FOR_INTERRUPTS();
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
			/* a field name after a dot and a function name before ( are not variables */
			if ((i == 0 || g->t[i - 1].kind != GO_DOT) &&
				(i + 1 >= b || g->t[i + 1].kind != GO_LP))
				go_check_name(g, tk->s, tk->len);
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
			if (go_is_rune(tk))		/* a rune is an integer, its code point */
				appendStringInfo(out, "%d", (int) go_rune_value(g, tk));
			else
				go_emit_str(g, tk, out);
			i++;
			continue;
		}
		if (tk->kind == GO_LP)
		{
			/* a bracketed group is an expression of its own (precedence) */
			int			close = go_match(g, i, b);

			go_sp(out);
			appendStringInfoChar(out, '(');
			if (close > 0)
			{
				go_emit_range(g, i + 1, close, out);
				appendStringInfoChar(out, ')');
				i = close + 1;
				continue;
			}
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
			else if (tk->len == 1 && tk->s[0] == '^')
			{
				/* Go's XOR, or its unary bitwise NOT; SQL's ^ is a power */
				bool		binary = (i > a && go_operand_end(&g->t[i - 1]));
				int			pe = binary ? -1 : go_unary_operand_end(g, i + 1, b);

				go_sp(out);
				if (pe > i + 1)
				{
					/*
					 * Go's unary operators bind tighter than any binary one,
					 * but SQL's prefix ~ binds looser than + and *, so
					 * bracket ^x with its operand: ^5 + 1 is (~ 5) + 1.
					 */
					appendStringInfoString(out, "(~");
					go_emit_range(g, i + 1, pe, out);
					appendStringInfoChar(out, ')');
					i = pe;
					continue;
				}
				appendStringInfoChar(out, binary ? '#' : '~');
			}
			else if (tk->len == 2 && tk->s[0] == '&' && tk->s[1] == '^')
			{
				go_sp(out);
				appendStringInfoString(out, "& ~");
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

/*
 * String concatenation with +.
 *
 * Go has no implicit conversions, so + with a string operand is always string
 * concatenation, and both operands are strings. Where plx can see that every
 * operand of a run of + is a string, it writes the + as SQL ||, which
 * propagates NULL the way plx's interpolation does. Where it cannot see that,
 * + stays +.
 */

#define GO_STRNUM_MSG "invalid operation: + between a string and a number; " \
	"Go does not convert between them, so use strconv.Itoa or fmt.Sprintf"

/* 's' for a string type, 'n' for a numeric or boolean type, 0 otherwise */
static char
go_type_kind(const char *typ)
{
	static const char *const num[] = {
		"integer", "smallint", "bigint", "real", "double precision",
		"numeric", "boolean", NULL
	};
	int			k;

	if (!typ)
		return 0;
	if (strcmp(typ, "text") == 0 || strcmp(typ, "varchar") == 0 ||
		strcmp(typ, "character varying") == 0)
		return 's';
	for (k = 0; num[k]; k++)
		if (strcmp(typ, num[k]) == 0)
			return 'n';
	return 0;
}

/* the kind (as go_type_kind) of a local or parameter name; 0 if neither */
static char
go_name_kind(Go *g, const GoTok *tk)
{
	PlxLocal2  *l = plx_local_find(g->cx, tk->s, tk->len);
	int			i;

	if (l)
		return go_type_kind(l->typ);
	if (!g->cx->meta || !g->cx->meta->argnames)
		return 0;
	for (i = 0; i < g->cx->meta->nargs; i++)
	{
		const char *an = g->cx->meta->argnames[i];

		if (an && (int) strlen(an) == tk->len && strncmp(an, tk->s, tk->len) == 0)
		{
			switch (g->cx->meta->argtypes[i])
			{
				case TEXTOID:
				case VARCHAROID:
					return 's';
				case INT2OID:
				case INT4OID:
				case INT8OID:
				case FLOAT4OID:
				case FLOAT8OID:
				case NUMERICOID:
				case BOOLOID:
					return 'n';
				default:
					return 0;
			}
		}
	}
	return 0;
}

/*
 * Guard for each level of recursion in the operand typing pass. It shares the
 * emitter's depth count, so typing stops at the same nesting limit as
 * emission, before it does the work for the deeper levels.
 */
static void
go_kind_enter(Go *g)
{
	check_stack_depth();
	CHECK_FOR_INTERRUPTS();
	if (++g->cx->depth > PLX_MAX_DEPTH)
		go_err(g, "expression nested too deeply");
	g->edepth++;
}

static void
go_kind_leave(Go *g)
{
	g->edepth--;
	g->cx->depth--;
}

/*
 * The kind of a single operand [a,b): 's' when plx can see it is a string, 'n'
 * when it can see it is a number or a bool, and 0 when it cannot tell (a row
 * field, an unmapped call, an expression).
 */
static char
go_operand_type(Go *g, int a, int b)
{
	GoTok	   *tk;
	bool		signed_op = false;

	/* unary signs: a signed operand is a number or nothing plx can type */
	while (a < b && (go_op_is(&g->t[a], "-") || go_op_is(&g->t[a], "+")))
	{
		CHECK_FOR_INTERRUPTS();
		signed_op = true;
		a++;
	}
	if (signed_op)
		return go_operand_type(g, a, b) == 'n' ? 'n' : 0;
	if (b <= a)
		return 0;
	tk = &g->t[a];
	if (b - a == 1)
	{
		if (tk->kind == GO_STR)				/* a rune literal is a number */
			return tk->s[0] == '\'' ? 'n' : 's';
		if (tk->kind == GO_NUM || go_ci(tk, "true") || go_ci(tk, "false"))
			return 'n';
		if (tk->kind == GO_WORD)
			return go_name_kind(g, tk);
		return 0;
	}
	/* a bracketed expression, typed once per top-level expression */
	if (tk->kind == GO_LP && go_match(g, a, b) == b - 1)
	{
		if (g->kgen[a] != g->gen)
		{
			go_kind_enter(g);
			g->kmemo[a] = go_expr_kind(g, a + 1, b - 1);
			g->kgen[a] = g->gen;
			go_kind_leave(g);
		}
		return g->kmemo[a];
	}
	/* an element s[i] of a slice local */
	if (tk->kind == GO_WORD && g->t[a + 1].kind == GO_LBRACK &&
		go_match(g, a + 1, b) == b - 1)
	{
		PlxLocal2  *l = plx_local_find(g->cx, tk->s, tk->len);
		int			tl = (l && l->typ) ? (int) strlen(l->typ) : 0;

		if (tl > 2 && strcmp(l->typ + tl - 2, "[]") == 0)
			return go_type_kind(pnstrdup(l->typ, tl - 2));
		return 0;
	}
	/* pkg.Func(...): only the calls plx maps */
	if (tk->kind == GO_WORD && b - a >= 5 && g->t[a + 1].kind == GO_DOT &&
		g->t[a + 2].kind == GO_WORD && g->t[a + 3].kind == GO_LP &&
		go_match(g, a + 3, b) == b - 1)
	{
		GoTok	   *m = &g->t[a + 2];

		if (go_ci(tk, "strconv"))
			return go_ci(m, "Itoa") ? 's' : go_ci(m, "Atoi") ? 'n' : 0;
		if (go_ci(tk, "fmt"))
			return go_ci(m, "Sprintf") ? 's' : 0;
		if (go_ci(tk, "strings"))
			return (go_ci(m, "ToUpper") || go_ci(m, "ToLower") ||
					go_ci(m, "TrimSpace") || go_ci(m, "ReplaceAll")) ? 's' :
				go_ci(m, "Contains") ? 'n' : 0;
		if (go_ci(tk, "math"))
			return (go_ci(m, "Abs") || go_ci(m, "Floor") || go_ci(m, "Ceil") ||
					go_ci(m, "Sqrt") || go_ci(m, "Pow") || go_ci(m, "Max") ||
					go_ci(m, "Min") || go_ci(m, "Mod")) ? 'n' : 0;
		return 0;
	}
	/* T(x) for a base type T, and len(x) / cap(x) */
	if (tk->kind == GO_WORD && g->t[a + 1].kind == GO_LP &&
		go_match(g, a + 1, b) == b - 1)
	{
		const char *conv = go_base_type(tk->s, tk->len);

		if (conv)
			return go_type_kind(conv);
		if (go_ci(tk, "len") || go_ci(tk, "cap"))
			return 'n';
	}
	return 0;
}

/*
 * Split [a,b) at its top-level binary operators: ops[] receives their token
 * indexes, or with ops NULL they are only counted. Returns the count, or -1
 * when the range holds a top-level comma or an operator that is not a Go
 * binary operator (such as : in a slice). A bracketed group that closes
 * inside the range is skipped in one step, so the cost is the number of
 * top-level tokens, not the length of the range.
 */
static int
go_split_binops(Go *g, int a, int b, int *ops)
{
	int			i,
				depth = 0,
				n = 0;

	for (i = a; i < b; i++)
	{
		GoKind		k = g->t[i].kind;

		CHECK_FOR_INTERRUPTS();
		if (depth == 0 && (k == GO_LP || k == GO_LBRACK || k == GO_LBRACE) &&
			g->mt[i] >= 0 && g->mt[i] < b)
			i = g->mt[i];		/* the closer leaves depth at 0 */
		else if (k == GO_LP || k == GO_LBRACK || k == GO_LBRACE)
			depth++;
		else if (k == GO_RP || k == GO_RBRACK || k == GO_RBRACE)
			depth--;
		else if (depth == 0 && k == GO_COMMA)
			return -1;
		else if (depth == 0 && k == GO_OP && i > a && go_operand_end(&g->t[i - 1]))
		{
			if (go_binop_prec(&g->t[i]) == 0)
				return -1;
			if (ops)
				ops[n] = i;
			n++;
		}
	}
	return n;
}

/*
 * The kind of the operands a..b joined by + at ops[lo..hi): 's' when all are
 * strings, 'n' when all are numbers, 0 otherwise. A string and a number
 * together is a Go compile error, and plx rejects it too.
 *
 * One string beside an operand plx cannot type (an array, jsonb, char(n), a
 * row field, a raw SQL call) is 0, not 's'. Written as ||, such a run would
 * mean array append, jsonb merge or a silent cast to text, where Go rejects
 * the program. Left as +, it fails loudly instead.
 */
static char
go_plus_run_kind(Go *g, int a, int b, const int *ops, int lo, int hi)
{
	bool		str = false,
				num = false,
				unknown = false;
	int			k;

	for (k = lo; k <= hi; k++)
	{
		int			s = (k == lo) ? a : ops[k - 1] + 1,
					e = (k == hi) ? b : ops[k];
		char		c = go_operand_type(g, s, e);

		if (c == 's')
			str = true;
		else if (c == 'n')
			num = true;
		else
			unknown = true;
	}
	if (str && num)
		plx_err(g->cx, g->t[ops[lo]].line, "%s", GO_STRNUM_MSG);
	if (unknown)
		return 0;
	return str ? 's' : num ? 'n' : 0;
}

/*
 * The kind of the expression [a,b): an operand, or operands joined only by +.
 * 0 for anything else.
 */
static char
go_expr_kind(Go *g, int a, int b)
{
	int		   *ops;
	int			n,
				k;
	char		r = 0;

	if (b <= a)
		return 0;
	/* a new top-level expression starts a new generation of Go.kmemo */
	if (g->edepth == 0)
		g->gen++;
	n = go_split_binops(g, a, b, NULL);
	if (n <= 0)
		return n == 0 ? go_operand_type(g, a, b) : 0;
	ops = palloc(sizeof(int) * n);
	n = go_split_binops(g, a, b, ops);
	for (k = 0; k < n; k++)
		if (!go_op_is(&g->t[ops[k]], "+"))
			break;
	if (k == n)
		r = go_plus_run_kind(g, a, b, ops, 0, n);
	pfree(ops);
	return r;
}

/*
 * Emit [a,b) with each run of + whose operands are all strings written as ||.
 * The range is cut at the operators that bind looser than + in Go (the
 * comparisons, && and ||), which SQL also ranks below ||. A run that joins
 * its operands only with + and whose operands are all strings becomes
 * ((x) || (y)) || (z), left to right as Go evaluates it; every other piece is
 * emitted in brackets as before. A top-level comma list is handled element by
 * element. Returns false, emitting nothing, when no run is all strings, so
 * the caller emits the range as it always did. With out NULL it only says
 * whether it would emit anything, and emits nothing: that costs typing only,
 * so a probe of each element of a nested comma list does not emit the
 * element twice per level.
 */
static bool
go_emit_strcat(Go *g, int a, int b, StringInfo out)
{
	int		   *ops,
			   *cut;
	char	   *isstr;
	int			n,
				ncut = 0,
				k,
				lo;
	bool		any = false;

	if (b <= a)
		return false;
	n = go_split_binops(g, a, b, NULL);
	if (n < 0)
	{
		/* a comma list: lower its elements one by one, if any needs it */
		int			depth = 0,
					s = a,
					i,
					ne = 0;
		int		   *es,
				   *ee;

		/* count the top-level commas first, to size es[] and ee[] */
		for (i = a; i < b; i++)
		{
			GoKind		kk = g->t[i].kind;

			CHECK_FOR_INTERRUPTS();
			if (depth == 0 && (kk == GO_LP || kk == GO_LBRACK || kk == GO_LBRACE) &&
				g->mt[i] >= 0 && g->mt[i] < b)
				i = g->mt[i];	/* skip a closed group in one step */
			else if (kk == GO_LP || kk == GO_LBRACK || kk == GO_LBRACE)
				depth++;
			else if (kk == GO_RP || kk == GO_RBRACK || kk == GO_RBRACE)
				depth--;
			else if (depth == 0 && kk == GO_COMMA)
				ne++;
		}
		es = palloc(sizeof(int) * (ne + 1));
		ee = palloc(sizeof(int) * (ne + 1));
		ne = 0;
		depth = 0;
		for (i = a; i <= b; i++)
		{
			GoKind		kk = (i < b) ? g->t[i].kind : GO_COMMA;

			CHECK_FOR_INTERRUPTS();
			if (depth == 0 && i < b &&
				(kk == GO_LP || kk == GO_LBRACK || kk == GO_LBRACE) &&
				g->mt[i] >= 0 && g->mt[i] < b)
				i = g->mt[i];	/* skip a closed group in one step */
			else if (kk == GO_LP || kk == GO_LBRACK || kk == GO_LBRACE)
				depth++;
			else if (kk == GO_RP || kk == GO_RBRACK || kk == GO_RBRACE)
				depth--;
			else if (depth == 0 && kk == GO_COMMA)
			{
				es[ne] = s;
				ee[ne] = i;
				ne++;
				s = i + 1;
			}
		}
		if (ne < 2)
			return false;			/* no comma: an operator plx does not model */
		for (k = 0; k < ne && !any; k++)
			any = go_emit_strcat(g, es[k], ee[k], NULL);
		if (!any || out == NULL)
			return any;
		for (k = 0; k < ne; k++)
		{
			if (k > 0)
				appendStringInfoChar(out, ',');
			go_emit_range(g, es[k], ee[k], out);
		}
		return true;
	}
	if (n == 0)
		return false;
	ops = palloc(sizeof(int) * n);
	n = go_split_binops(g, a, b, ops);

	/* cut at the operators looser than + */
	cut = palloc(sizeof(int) * (n + 2));
	isstr = palloc(n + 1);
	cut[ncut++] = -1;
	for (k = 0; k < n; k++)
		if (go_binop_prec(&g->t[ops[k]]) < 4)
			cut[ncut++] = k;
	cut[ncut] = n;

	/* each piece runs over operands cut[j]+1 .. cut[j+1] */
	for (k = 0; k < ncut; k++)
	{
		int			ol = cut[k] + 1,
					oh = cut[k + 1],
					s = (ol == 0) ? a : ops[ol - 1] + 1,
					e = (oh == n) ? b : ops[oh],
					j;

		isstr[k] = false;
		if (oh == ol)
			continue;			/* one operand, no + */
		for (j = ol; j < oh; j++)
			if (!go_op_is(&g->t[ops[j]], "+"))
				break;
		if (j == oh && go_plus_run_kind(g, s, e, ops, ol, oh) == 's')
			isstr[k] = any = true;
	}
	if (!any || out == NULL)
	{
		pfree(ops);
		pfree(cut);
		pfree(isstr);
		return any;
	}

	for (k = 0; k < ncut; k++)
	{
		int			ol = cut[k] + 1,
					oh = cut[k + 1],
					s = (ol == 0) ? a : ops[ol - 1] + 1,
					e = (oh == n) ? b : ops[oh];

		if (k > 0)
		{
			GoTok	   *o = &g->t[ops[cut[k]]];

			go_sp(out);
			if (go_op_is(o, "=="))
				appendStringInfoChar(out, '=');
			else if (go_op_is(o, "!="))
				appendStringInfoString(out, "<>");
			else if (go_op_is(o, "&&"))
				appendStringInfoString(out, "AND");
			else if (go_op_is(o, "||"))
				appendStringInfoString(out, "OR");
			else
				appendBinaryStringInfo(out, o->s, o->len);
		}
		go_sp(out);
		if (!isstr[k])
		{
			if (ncut > 1)
				appendStringInfoChar(out, '(');
			go_emit_range(g, s, e, out);
			if (ncut > 1)
				appendStringInfoChar(out, ')');
			continue;
		}
		for (lo = ol; lo < oh; lo++)
			appendStringInfoChar(out, '(');
		appendStringInfoChar(out, '(');
		go_emit_range(g, s, ops[ol], out);
		appendStringInfoChar(out, ')');
		for (lo = ol; lo < oh; lo++)
		{
			appendStringInfoString(out, " || (");
			go_emit_range(g, ops[lo] + 1, lo + 1 < oh ? ops[lo + 1] : e, out);
			appendStringInfoString(out, "))");
		}
	}
	pfree(ops);
	pfree(cut);
	pfree(isstr);
	return true;
}

/* emit tokens [a,b) as a plpgsql expression */
static void
go_emit_range_inner(Go *g, int a, int b, StringInfo out)
{
	if (go_emit_strcat(g, a, b, out))
		return;
	if (go_needs_prec(g, a, b) && go_emit_prec(g, a, b, out))
		return;
	go_emit_tokens(g, a, b, out);
}

/*
 * Recursion guard for the expression emitter. Nested calls, subscripts and
 * literals recurse through here once per level; without check_stack_depth()
 * hostile nesting overflows the C stack and crashes the backend (SIGSEGV).
 */
static void
go_emit_range(Go *g, int a, int b, StringInfo out)
{
	check_stack_depth();
	CHECK_FOR_INTERRUPTS();
	if (g->edepth == 0)
		g->gen++;				/* a new top-level expression: see Go.kmemo */
	if (++g->cx->depth > PLX_MAX_DEPTH)
		go_err(g, "expression nested too deeply");
	g->edepth++;
	go_emit_range_inner(g, a, b, out);
	g->edepth--;
	g->cx->depth--;
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

/*
 * Go's zero value for a declared type, as a SQL literal. NULL when SQL has no
 * literal for it: a nil slice is already modelled by NULL, and a PostgreSQL
 * type named in the body has no Go zero value.
 */
static const char *
go_zero_value(const char *typ)
{
	if (strcmp(typ, "integer") == 0 || strcmp(typ, "smallint") == 0 ||
		strcmp(typ, "bigint") == 0 || strcmp(typ, "real") == 0 ||
		strcmp(typ, "double precision") == 0)
		return "0";
	if (strcmp(typ, "text") == 0)
		return "''";
	if (strcmp(typ, "boolean") == 0)
		return "false";
	return NULL;
}

/* declare a name with an inferred/explicit type and optional initializer */
static void
go_declare_one(Go *g, const char *name, StringInfo typ, int e0, int e1, bool isconst, int ind)
{
	PlxLocal2  *l;

	go_check_name(g, name, (int) strlen(name));
	l = plx_local_add(g->cx, name, (int) strlen(name));

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
			plx_indent(&g->cx->out, ind);
			appendStringInfo(&g->cx->out, "%s :=", name);
			go_emit_range(g, e0, e1, &g->cx->out);
			appendStringInfoString(&g->cx->out, ";\n");
		}
	}
	else if (!isconst && l->typ)
	{
		/*
		 * Go starts a variable declared without a value at its type's zero
		 * value, and does so each time the declaration runs: a var inside a
		 * loop body is reset on every iteration. So assign the zero value at
		 * the declaration, not in the hoisted DECLARE.
		 */
		const char *zero = go_zero_value(l->typ);
		int			tl = (int) strlen(l->typ);

		if (zero)
		{
			plx_indent(&g->cx->out, ind);
			appendStringInfo(&g->cx->out, "%s := %s;\n", name, zero);
		}
		else if (g->arrlen >= 0 && tl > 2 && strcmp(l->typ + tl - 2, "[]") == 0)
		{
			/* [N]T: Go's zero value is N zero elements, not a nil slice */
			char	   *el = pnstrdup(l->typ, tl - 2);

			zero = go_zero_value(el);
			if (zero)
			{
				plx_indent(&g->cx->out, ind);
				appendStringInfo(&g->cx->out, "%s := array_fill((%s)::%s, ARRAY[%d]);\n",
								 name, zero, el, g->arrlen);
			}
		}
	}
	g->arrlen = -1;
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
	const char *opc = NULL;		/* SQL operator of a compound assignment */
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
				opc = pnstrdup(o->s, 1);
				break;
			}
			/* bitwise compound assignments; Go's ^ is SQL's #, &^ is & ~ */
			if (go_op_is(o, "<<=") || go_op_is(o, ">>=") ||
				go_op_is(o, "&=") || go_op_is(o, "|="))
			{
				opidx = i;
				opc = pnstrdup(o->s, o->len - 1);
				break;
			}
			if (go_op_is(o, "^="))
			{
				opidx = i;
				opc = "#";
				break;
			}
			if (go_op_is(o, "&^="))
			{
				opidx = i;
				opc = "& ~";
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
		plx_indent(&g->cx->out, ind);
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

		plx_indent(&g->cx->out, ind);
		if (go_ci(h, "panic"))
		{
			int			lp = e0 + 1,
						close = (g->t[lp].kind == GO_LP) ? go_match(g, lp, e1) : -1;

			if (close > lp + 1)
			{
				appendStringInfoString(&g->cx->out, "RAISE EXCEPTION '%',");
				g->cx->diag_msg = true;
				go_emit_range(g, lp + 1, close, &g->cx->out);
				g->cx->diag_msg = false;
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

				if (g->t[lp + 1].kind == GO_STR)
					go_check_argindex(g, &g->t[lp + 1],
									  go_ci(m, "Printf") ? "Printf" : "Sprintf");

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

			if (!plx_local_find(g->cx, nm, (int) strlen(nm)))
				go_declare_one(g, nm, NULL, opidx + 1, e1, false, ind);
			else
			{
				plx_indent(&g->cx->out, ind);
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

				if (!plx_local_find(g->cx, nm, (int) strlen(nm)))
					go_declare_one(g, nm, NULL, rhs[j * 2], rhs[j * 2 + 1], false, ind);
				else
				{
					plx_indent(&g->cx->out, ind);
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
			plx_indent(&g->cx->out, ind);
			appendStringInfoString(&g->cx->out, "SELECT");
			go_emit_range(g, opidx + 1, e1, &g->cx->out);
			appendStringInfoString(&g->cx->out, " INTO");
			go_emit_range(g, e0, opidx, &g->cx->out);
			appendStringInfoString(&g->cx->out, ";\n");
			g->pos = e1;
			return;
		}
	}

	/*
	 * s += x on a string appends. plxgo string locals are text, so a plain ||
	 * is enough (no string builder), and it propagates NULL as + chains do.
	 * When plx cannot see that x is a string, the append stays s := s + (x),
	 * which fails loudly; || would cast a number or a jsonb value to text,
	 * where Go rejects the program.
	 */
	if (opc && strcmp(opc, "+") == 0 && go_expr_kind(g, e0, opidx) == 's')
	{
		char		rk = go_expr_kind(g, opidx + 1, e1);

		if (rk == 'n')
			plx_err(g->cx, g->t[opidx].line, "%s", GO_STRNUM_MSG);
		if (rk == 's')
		{
			plx_indent(&g->cx->out, ind);
			go_emit_range(g, e0, opidx, &g->cx->out);
			appendStringInfoString(&g->cx->out, " :=");
			go_emit_range(g, e0, opidx, &g->cx->out);
			appendStringInfoString(&g->cx->out, " || (");
			go_emit_range(g, opidx + 1, e1, &g->cx->out);
			appendStringInfoString(&g->cx->out, ");\n");
			g->pos = e1;
			return;
		}
	}

	/* plain assignment: LHS <op> RHS */
	plx_indent(&g->cx->out, ind);
	go_emit_range(g, e0, opidx, &g->cx->out);
	appendStringInfoString(&g->cx->out, " :=");
	if (opc && (strcmp(opc, "<<") == 0 || strcmp(opc, ">>") == 0) &&
		!go_small_count(g, opidx + 1, e1))
	{
		/* x <<= n: the shift takes Go's result for every count */
		if (++g->guards > GO_MAX_GUARDS)
			go_err(g, GO_GUARDS_MSG);
		appendStringInfoString(&g->cx->out, opc[0] == '<' ? GO_SHL_OPEN : GO_SHR_OPEN);
		go_emit_range(g, e0, opidx, &g->cx->out);
		appendStringInfoString(&g->cx->out, GO_SHIFT_MID);
		go_emit_range(g, opidx + 1, e1, &g->cx->out);
		appendStringInfoString(&g->cx->out, GO_SHIFT_CLOSE);
		g->guards--;
		appendStringInfoString(&g->cx->out, ";\n");
	}
	else if (opc)
	{
		go_emit_range(g, e0, opidx, &g->cx->out);
		appendStringInfo(&g->cx->out, " %s (", opc);
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
	plx_indent(&g->cx->out, ind);
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
			plx_indent(&g->cx->out, ind);
			appendStringInfoString(&g->cx->out, "ELSE\n");
			go_if(g, ind + 1);
		}
		else
		{
			plx_indent(&g->cx->out, ind);
			appendStringInfoString(&g->cx->out, "ELSE\n");
			go_block(g, ind + 1);
		}
		go_eat_semi(g);
	}
	plx_indent(&g->cx->out, ind);
	appendStringInfoString(&g->cx->out, "END IF;\n");
	g->cx->depth--;
}

/*
 * The body of a loop whose continue plpgsql's CONTINUE already gets right
 * (range, condition-only and infinite loops): hide any enclosing three-clause
 * loop's continue label, and any enclosing switch's break label, while it is
 * parsed.
 */
static void
go_loop_body(Go *g, int ind)
{
	const char *save_label = g->cont_label;
	const char *save_brk = g->brk_label;

	g->cont_label = NULL;
	g->brk_label = NULL;
	g->cx->loopdepth++;
	go_block(g, ind);
	g->cx->loopdepth--;
	g->cont_label = save_label;
	g->brk_label = save_brk;
}

/*
 * The PostgreSQL type of a three-clause for loop's variable, from its start
 * expression [a,b); NULL when it cannot be told. Go gives an arithmetic
 * expression the type of its typed operand (untyped constants adapt to it),
 * so the first plain variable in the expression decides, as "name%TYPE".
 */
static char *
go_loop_var_type(Go *g, int a, int b)
{
	const char *inf = go_infer_type(g, a, b);
	bool		isfloat = false;
	int			i;

	if (inf)
		return pstrdup(inf);
	for (i = a; i < b; i++)
	{
		GoTok	   *tk = &g->t[i];

		if (tk->kind == GO_WORD)
		{
			if (i + 1 < b && g->t[i + 1].kind == GO_LBRACK)
			{
				/* an element of a slice declared in the body: its element type */
				PlxLocal2  *l = plx_local_find(g->cx, tk->s, tk->len);
				int			tl = (l && l->typ) ? (int) strlen(l->typ) : 0;

				if (tl > 2 && strcmp(l->typ + tl - 2, "[]") == 0)
					return pnstrdup(l->typ, tl - 2);
				return NULL;
			}
			if (i + 1 < b && (g->t[i + 1].kind == GO_LP ||
							  g->t[i + 1].kind == GO_DOT))
				return NULL;	/* a call or a field: type unknown */
			if (go_ci(tk, "true") || go_ci(tk, "false") || go_ci(tk, "nil"))
				return NULL;
			return psprintf("%s%%TYPE", go_ident(tk->s, tk->len));
		}
		if (tk->kind == GO_NUM)
			isfloat = isfloat || go_num_is_float(tk);
		else if (tk->kind != GO_OP && tk->kind != GO_LP && tk->kind != GO_RP)
			return NULL;
	}
	return pstrdup(isfloat ? "double precision" : "integer");
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
			go_check_name(g, rowname, (int) strlen(rowname));
			if (!plx_local_find(g->cx, rowname, (int) strlen(rowname)))
			{
				PlxLocal2  *l = plx_local_add(g->cx, rowname, (int) strlen(rowname));

				l->is_record = true;
			}
			plx_indent(&g->cx->out, ind);
			appendStringInfo(&g->cx->out, "FOR %s IN EXECUTE", rowname);
			go_emit_range(g, rs + 2, close, &g->cx->out);
			appendStringInfoString(&g->cx->out, " LOOP\n");
			g->pos = hb;
			go_loop_body(g, ind + 1);
			plx_indent(&g->cx->out, ind);
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
			go_check_name(g, idxname, (int) strlen(idxname));
			if (!plx_local_find(g->cx, idxname, (int) strlen(idxname)))
			{
				PlxLocal2  *l = plx_local_add(g->cx, idxname, (int) strlen(idxname));

				l->typ = pstrdup("integer");
			}
			plx_indent(&g->cx->out, ind);
			appendStringInfo(&g->cx->out, "FOR %s IN 0 .. (", idxname);
			go_emit_range(g, rs, hb, &g->cx->out);
			appendStringInfoString(&g->cx->out, ") - 1 LOOP\n");
			g->pos = hb;
			go_loop_body(g, ind + 1);
			plx_indent(&g->cx->out, ind);
			appendStringInfoString(&g->cx->out, "END LOOP;\n");
			go_eat_semi(g);
			return;
		}
		/* range over a slice -> FOREACH v IN ARRAY e */
		if (!valname)
			go_err(g, "range needs a value variable (for _, v := range s)");
		go_check_name(g, valname, (int) strlen(valname));
		if (!plx_local_find(g->cx, valname, (int) strlen(valname)))
		{
			/* infer the element type from a plain slice local (var s []T) */
			PlxLocal2  *arr = (rs + 1 == hb && g->t[rs].kind == GO_WORD)
				? plx_local_find(g->cx, g->t[rs].s, g->t[rs].len) : NULL;

			if (arr && arr->typ)
			{
				int			tl = (int) strlen(arr->typ);

				if (tl > 2 && strcmp(arr->typ + tl - 2, "[]") == 0)
				{
					PlxLocal2  *l = plx_local_add(g->cx, valname, (int) strlen(valname));

					l->typ = pnstrdup(arr->typ, tl - 2);
				}
				else
					go_err(g, "declare the range value variable with var before the loop");
			}
			else
				go_err(g, "declare the range value variable with var before the loop");
		}
		plx_indent(&g->cx->out, ind);
		appendStringInfo(&g->cx->out, "FOREACH %s IN ARRAY", valname);
		go_emit_range(g, rs, hb, &g->cx->out);
		appendStringInfoString(&g->cx->out, " LOOP\n");
		g->pos = hb;
		go_loop_body(g, ind + 1);
		plx_indent(&g->cx->out, ind);
		appendStringInfoString(&g->cx->out, "END LOOP;\n");
		go_eat_semi(g);
		return;
	}

	/*
	 * for init; cond; post { } becomes
	 *
	 *     [DECLARE v T := A; BEGIN]  when init is "v := A": v belongs to the loop
	 *     [init;]                    any other init statement
	 *     WHILE cond LOOP
	 *         [<<__plx_cN>> BEGIN] body [END;]
	 *         post;
	 *     END LOOP;
	 *     [END;]
	 *
	 * A plpgsql integer FOR is not equivalent: it evaluates its bound once,
	 * keeps its own counter (so the body cannot move the loop variable, and a
	 * variable declared before the loop is never updated), and rounds a float
	 * start to an integer. Go re-tests cond before every iteration. continue
	 * leaves the labelled body block, so post still runs.
	 */
	if (semi1 >= 0)
	{
		const char *save_label = g->cont_label;
		const char *save_brk = g->brk_label;
		char	   *label = NULL;
		char	   *lvar = NULL;
		int			bodyend = go_match(g, hb, g->nt);
		int			bind = ind;
		int			j;

		if (g->t[h0].kind == GO_WORD && h0 + 2 < semi1 &&
			g->t[h0 + 1].kind == GO_OP && g->t[h0 + 1].len == 2 &&
			g->t[h0 + 1].s[0] == ':' && g->t[h0 + 1].s[1] == '=')
		{
			char	   *ltyp;

			/*
			 * The DECLARE below hides any local or parameter that plpgsql
			 * folds to the same name, so reject one spelled another way.
			 */
			plx_check_loop_var(g->cx, g->t[h0].s, g->t[h0].len);
			lvar = go_ident(g->t[h0].s, g->t[h0].len);
			ltyp = go_loop_var_type(g, h0 + 2, semi1);
			if (!ltyp)
				plx_err(g->cx, g->t[h0].line,
						"cannot infer a PostgreSQL type for loop variable \"%s\"; declare it before the loop (var %s T) and write for %s = ...",
						lvar, lvar, lvar);
			/*
			 * The start value is the DECLARE default: plpgsql evaluates it
			 * before the new variable is in scope, so for i := i reads the
			 * outer i, as in Go.
			 */
			plx_indent(&g->cx->out, ind);
			appendStringInfo(&g->cx->out, "DECLARE\n");
			plx_indent(&g->cx->out, ind + 1);
			appendStringInfo(&g->cx->out, "%s %s :=", lvar, ltyp);
			go_emit_range(g, h0 + 2, semi1, &g->cx->out);
			appendStringInfoString(&g->cx->out, ";\n");
			plx_indent(&g->cx->out, ind);
			appendStringInfoString(&g->cx->out, "BEGIN\n");
			bind = ind + 1;
		}
		else if (semi1 > h0)				/* any other init statement */
			go_simple(g, bind, semi1);

		g->pos = hb;
		plx_indent(&g->cx->out, bind);
		appendStringInfoString(&g->cx->out, "WHILE");
		if (semi2 > semi1 + 1)
			go_emit_range(g, semi1 + 1, semi2, &g->cx->out);
		else
			appendStringInfoString(&g->cx->out, " true");
		appendStringInfoString(&g->cx->out, " LOOP\n");

		/* a continue anywhere in the body needs the labelled block */
		for (j = hb; j < bodyend; j++)
			if (go_ci(&g->t[j], "continue"))
			{
				label = psprintf("__plx_c%d", ++g->cx->subq);
				break;
			}
		g->cx->loopdepth++;
		g->brk_label = NULL;
		if (label)
		{
			plx_indent(&g->cx->out, bind + 1);
			appendStringInfo(&g->cx->out, "<<%s>>\n", label);
			plx_indent(&g->cx->out, bind + 1);
			appendStringInfoString(&g->cx->out, "BEGIN\n");
			g->cont_label = label;
			go_block(g, bind + 2);
			g->cont_label = save_label;
			plx_indent(&g->cx->out, bind + 1);
			appendStringInfoString(&g->cx->out, "END;\n");
		}
		else
		{
			g->cont_label = NULL;
			go_block(g, bind + 1);
			g->cont_label = save_label;
		}
		g->cx->loopdepth--;
		g->brk_label = save_brk;
		if (semi2 >= 0 && hb > semi2 + 1)		/* post statement */
		{
			int			save = g->pos;

			g->pos = semi2 + 1;
			go_simple(g, bind + 1, hb);
			g->pos = save;
		}
		plx_indent(&g->cx->out, bind);
		appendStringInfoString(&g->cx->out, "END LOOP;\n");
		if (lvar)
		{
			plx_indent(&g->cx->out, ind);
			appendStringInfoString(&g->cx->out, "END;\n");
		}
		go_eat_semi(g);
		return;
	}

	/* for { } (infinite) or for cond { } (while) */
	plx_indent(&g->cx->out, ind);
	if (hb > h0)
	{
		appendStringInfoString(&g->cx->out, "WHILE");
		go_emit_range(g, h0, hb, &g->cx->out);
		appendStringInfoString(&g->cx->out, " LOOP\n");
	}
	else
		appendStringInfoString(&g->cx->out, "LOOP\n");
	g->pos = hb;
	go_loop_body(g, ind + 1);
	plx_indent(&g->cx->out, ind);
	appendStringInfoString(&g->cx->out, "END LOOP;\n");
	go_eat_semi(g);
}

/*
 * switch tag { case a, b: ...; default: ... } -> CASE tag WHEN a, b ... END CASE
 * switch { case c: ...; default: ... }          -> IF/ELSIF/ELSE
 *
 * Go evaluates the tag once. plpgsql's simple CASE does too, and compares it
 * with each WHEN value as a whole (tag IN (a, b)), so `case a || b` means
 * tag = (a OR b), as in Go. A CASE with no match and no ELSE raises
 * case_not_found, so a tagged switch always gets an ELSE.
 *
 * Go runs default only when no case matches, wherever default stands, so its
 * body is emitted into a buffer of its own and appended as the final ELSE.
 * break inside a switch leaves only the switch: when the switch holds a
 * break, the chain goes in a labelled block and break lowers to EXIT label.
 */
static void
go_switch(Go *g, int ind)
{
	int			hb,
				t0,
				tag0 = -1,
				tag1 = -1,
				close,
				sind = ind,
				j;
	bool		first = true,
				had_default = false;
	char	   *label = NULL;
	const char *save_brk = g->brk_label;
	StringInfoData defbuf;

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
	close = go_match(g, g->pos, g->nt);
	for (j = g->pos; close > 0 && j < close; j++)
		if (go_ci(&g->t[j], "break"))
		{
			label = psprintf("__plx_s%d", ++g->cx->subq);
			break;
		}
	if (label)
	{
		plx_indent(&g->cx->out, ind);
		appendStringInfo(&g->cx->out, "<<%s>>\n", label);
		plx_indent(&g->cx->out, ind);
		appendStringInfoString(&g->cx->out, "BEGIN\n");
		sind = ind + 1;
	}
	g->brk_label = label;
	initStringInfo(&defbuf);
	g->pos++;								/* { */
	go_eat_semi(g);
	while (go_cur(g)->kind != GO_RBRACE && go_cur(g)->kind != GO_EOF)
	{
		bool		is_default = go_ci(go_cur(g), "default");
		StringInfoData save_out;

		if (!go_ci(go_cur(g), "case") && !is_default)
			go_err(g, "expected case or default in switch");
		g->pos++;						/* case / default */
		if (!is_default)
		{
			/* case values up to the ':' ; multiple values are OR-ed */
			int			cv0 = g->pos,
						colon = -1,
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
			if (tag0 >= 0 && first)
			{
				plx_indent(&g->cx->out, sind);
				appendStringInfoString(&g->cx->out, "CASE (");
				go_emit_range(g, tag0, tag1, &g->cx->out);
				appendStringInfoString(&g->cx->out, ")\n");
			}
			plx_indent(&g->cx->out, sind);
			appendStringInfoString(&g->cx->out,
								   tag0 >= 0 ? "WHEN" : first ? "IF" : "ELSIF");
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
							appendStringInfoString(&g->cx->out,
												   tag0 >= 0 ? "," : " OR");
						appendStringInfoString(&g->cx->out, " (");
						go_emit_range(g, s, j, &g->cx->out);
						appendStringInfoChar(&g->cx->out, ')');
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
			int			colon = -1;

			if (had_default)
				go_err(g, "multiple defaults in switch");
			for (j = g->pos; j < g->nt; j++)
				if (g->t[j].kind == GO_OP && g->t[j].len == 1 && g->t[j].s[0] == ':')
				{
					colon = j;
					break;
				}
			if (colon < 0)
				go_err(g, "expected : after default");
			g->pos = colon + 1;
			had_default = true;
		}
		go_eat_semi(g);
		/* the default body goes to its own buffer, emitted last */
		if (is_default)
		{
			save_out = g->cx->out;
			g->cx->out = defbuf;
		}
		/* the case body runs until the next case/default or the closing } */
		while (!go_ci(go_cur(g), "case") && !go_ci(go_cur(g), "default") &&
			   go_cur(g)->kind != GO_RBRACE && go_cur(g)->kind != GO_EOF)
		{
			if (go_ci(go_cur(g), "fallthrough"))
				go_err(g, "fallthrough is not supported");
			go_stmt(g, sind + 1);
		}
		if (is_default)
		{
			defbuf = g->cx->out;
			g->cx->out = save_out;
		}
	}
	if (go_cur(g)->kind == GO_RBRACE)
		g->pos++;
	go_eat_semi(g);
	if (first && tag0 >= 0 && had_default)
	{
		/* no case: Go still evaluates the tag once, then runs default */
		plx_indent(&g->cx->out, sind);
		appendStringInfoString(&g->cx->out, "PERFORM (");
		go_emit_range(g, tag0, tag1, &g->cx->out);
		appendStringInfoString(&g->cx->out, ");\n");
	}
	if (first && had_default)
	{
		plx_indent(&g->cx->out, sind);
		appendStringInfoString(&g->cx->out, "IF true THEN\n");
		appendBinaryStringInfo(&g->cx->out, defbuf.data, defbuf.len);
		plx_indent(&g->cx->out, sind);
		appendStringInfoString(&g->cx->out, "END IF;\n");
	}
	else if (!first && tag0 >= 0)
	{
		plx_indent(&g->cx->out, sind);
		appendStringInfoString(&g->cx->out, "ELSE\n");
		if (had_default)
			appendBinaryStringInfo(&g->cx->out, defbuf.data, defbuf.len);
		else
		{
			plx_indent(&g->cx->out, sind + 1);
			appendStringInfoString(&g->cx->out, "NULL;\n");
		}
		plx_indent(&g->cx->out, sind);
		appendStringInfoString(&g->cx->out, "END CASE;\n");
	}
	else if (!first)
	{
		if (had_default)
		{
			plx_indent(&g->cx->out, sind);
			appendStringInfoString(&g->cx->out, "ELSE\n");
			appendBinaryStringInfo(&g->cx->out, defbuf.data, defbuf.len);
		}
		plx_indent(&g->cx->out, sind);
		appendStringInfoString(&g->cx->out, "END IF;\n");
	}
	pfree(defbuf.data);
	if (label)
	{
		plx_indent(&g->cx->out, ind);
		appendStringInfoString(&g->cx->out, "END;\n");
	}
	g->brk_label = save_brk;
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
	plx_indent(&g->cx->out, ind);
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
		plx_indent(&g->cx->out, ind);
		if (g->brk_label)		/* leave the switch, not a loop around it */
			appendStringInfo(&g->cx->out, "EXIT %s;\n", g->brk_label);
		else
			appendStringInfoString(&g->cx->out, "EXIT;\n");
		go_eat_semi(g);
	}
	else if (go_ci(tk, "continue"))
	{
		g->pos++;
		plx_indent(&g->cx->out, ind);
		if (g->cont_label)		/* leave the body block; post still runs */
			appendStringInfo(&g->cx->out, "EXIT %s;\n", g->cont_label);
		else
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
plx_go_parse_body(Ctx *cx)
{
	Go			g;

	memset(&g, 0, sizeof(g));
	g.cx = cx;
	g.arrlen = -1;
	go_lex(&g, cx->body);

	/*
	 * make(...) has no lowering. Left alone it reaches plpgsql as written, and
	 * the error names something else (a syntax error at "[", a type that
	 * cannot be inferred, or a case clash with the type name), so reject it
	 * here first. A field or method named make (x.make(...)) is not the
	 * builtin.
	 */
	{
		int			i;

		for (i = 0; i + 1 < g.nt; i++)
			if (go_ci(&g.t[i], "make") && g.t[i + 1].kind == GO_LP &&
				(i == 0 || g.t[i - 1].kind != GO_DOT))
				plx_err(cx, g.t[i].line,
						"make is not supported; use a slice literal such as []int{1, 2}, or array_fill(0, []int{n}) for n zeros");
	}
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

static const PlxSurface go_surface = {
	.lanname = "plxgo",
	.block_style = PLX_BLK_GO,
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
	.parse_body = plx_go_parse_body,
};

static char *
go_transpile_entry(const char *src, const PlxFuncMeta *meta)
{
	return plx_transpile(src, meta, &go_surface, CurrentMemoryContext);
}

const PlxDialect plx_go_dialect = {
	.abi_version = PLX_ABI_VERSION,
	.lanname = "plxgo",
	.transpile = go_transpile_entry,
	.flags = PLX_TRUSTED,
};

Datum
plx_go_validator(PG_FUNCTION_ARGS)
{
	return plx_generic_validator(fcinfo, &plx_go_dialect);
}

Datum
plx_go_inline_handler(PG_FUNCTION_ARGS)
{
	return plx_generic_inline_handler(fcinfo, &plx_go_dialect);
}

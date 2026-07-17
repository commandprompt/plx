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

#include "fmgr.h"
#include "lib/stringinfo.h"
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
	PlxLocal2  *l = plx_local_add(g->cx, name, (int) strlen(name));

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

	/* plain assignment: LHS <op> RHS */
	plx_indent(&g->cx->out, ind);
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
			g->cx->loopdepth++;
			go_block(g, ind + 1);
			g->cx->loopdepth--;
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
			g->cx->loopdepth++;
			go_block(g, ind + 1);
			g->cx->loopdepth--;
			plx_indent(&g->cx->out, ind);
			appendStringInfoString(&g->cx->out, "END LOOP;\n");
			go_eat_semi(g);
			return;
		}
		/* range over a slice -> FOREACH v IN ARRAY e */
		if (!valname)
			go_err(g, "range needs a value variable (for _, v := range s)");
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
		g->cx->loopdepth++;
		go_block(g, ind + 1);
		g->cx->loopdepth--;
		plx_indent(&g->cx->out, ind);
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
			plx_indent(&g->cx->out, ind);
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
			plx_indent(&g->cx->out, ind);
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
		plx_indent(&g->cx->out, ind);
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
		plx_indent(&g->cx->out, ind);
		appendStringInfoString(&g->cx->out, "END LOOP;\n");
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
	g->cx->loopdepth++;
	go_block(g, ind + 1);
	g->cx->loopdepth--;
	plx_indent(&g->cx->out, ind);
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
			plx_indent(&g->cx->out, ind);
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
			plx_indent(&g->cx->out, ind);
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
		plx_indent(&g->cx->out, ind);
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
		appendStringInfoString(&g->cx->out, "EXIT;\n");
		go_eat_semi(g);
	}
	else if (go_ci(tk, "continue"))
	{
		g->pos++;
		plx_indent(&g->cx->out, ind);
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

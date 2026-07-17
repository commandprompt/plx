/*
 * plx_dialect_cobol.c - the "plxcobol" dialect (ISO/IEC 1989:2023, COBOL 2023).
 *
 * COBOL is verb-driven and free-format (COBOL 2023). Unlike the other dialects
 * it does not fit the shared byte lexer's expression model (hyphenated words,
 * period sentence terminators, PICTURE clauses), so the COBOL front end (below)
 * uses its own tokenizer and recursive-descent parser and emits plpgsql
 * directly; the dialect-neutral engine lives in plx_transpile.c. The keyword
 * table is unused by the COBOL path but kept for consistency.
 */
#include "postgres.h"

#include "fmgr.h"
#include "lib/stringinfo.h"
#include "miscadmin.h"
#include "utils/memutils.h"

#include "plx.h"
#include "plx_int.h"
#include "plx_engine.h"

PG_FUNCTION_INFO_V1(plx_cob_validator);
PG_FUNCTION_INFO_V1(plx_cob_inline_handler);

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
	PlxLocal2  *l = plx_local_find(cb->cx, name, strlen(name));
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

	if (plx_local_find(cb->cx, mapped, strlen(mapped)))
		cob_err(cb, "duplicate data name in WORKING-STORAGE");
	l = plx_local_add(cb->cx, mapped, strlen(mapped));

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

		plx_indent(&cb->cx->out, ind);
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
		plx_indent(&cb->cx->out, ind);
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
			plx_indent(&cb->cx->out, ind);
			appendStringInfo(&cb->cx->out, "%s := (%s %c (%s));\n",
							 giv ? giv : recv, recv, is_add ? '+' : '-', sum.data);
		}
		else if (is_add && cob_ci(cob_cur(cb), "GIVING"))
		{
			cb->pos++;
			giv = cob_operand(cb);
			if (!giv)
				cob_err(cb, "GIVING requires a receiving field");
			plx_indent(&cb->cx->out, ind);
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
		plx_indent(&cb->cx->out, ind);
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
	plx_indent(&cb->cx->out, ind);
	appendStringInfo(&cb->cx->out, "IF %s THEN\n", c.data);
	cob_block(cb, ind + 1);
	if (cob_ci(cob_cur(cb), "ELSE"))
	{
		cb->pos++;
		plx_indent(&cb->cx->out, ind);
		appendStringInfoString(&cb->cx->out, "ELSE\n");
		cob_block(cb, ind + 1);
	}
	cob_expect(cb, "END-IF");
	plx_indent(&cb->cx->out, ind);
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
		plx_indent(&cb->cx->out, ind);
		appendStringInfoString(&cb->cx->out, "CASE\n");
	}
	else
	{
		StringInfoData s;
		static const char *stops[] = {"WHEN"};

		initStringInfo(&s);
		if (!cob_value(cb, &s, stops, 1))
			cob_err(cb, "EVALUATE requires a subject");
		plx_indent(&cb->cx->out, ind);
		appendStringInfo(&cb->cx->out, "CASE %s\n", s.data);
	}

	while (cob_ci(cob_cur(cb), "WHEN"))
	{
		if (cob_ci(&cb->t[cb->pos + 1], "OTHER"))
		{
			cb->pos += 2;
			plx_indent(&cb->cx->out, ind + 1);
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
			plx_indent(&cb->cx->out, ind + 1);
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
			plx_indent(&cb->cx->out, ind + 1);
			appendStringInfo(&cb->cx->out, "WHEN %s THEN\n", vals.data);
			cob_block(cb, ind + 2);
		}
	}
	cob_expect(cb, "END-EVALUATE");
	plx_indent(&cb->cx->out, ind);
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
		plx_indent(&cb->cx->out, ind);
		appendStringInfo(&cb->cx->out, "WHILE NOT (%s ) LOOP\n", c.data);
		cob_block(cb, ind + 1);
		cob_expect(cb, "END-PERFORM");
		plx_indent(&cb->cx->out, ind);
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
				plx_indent(&cb->cx->out, ind);
				if (gt)
					appendStringInfo(&cb->cx->out, "FOR %s IN (%s )..(%s ) LOOP\n",
									 var, from.data, bound.data);
				else
					appendStringInfo(&cb->cx->out, "FOR %s IN (%s )..((%s ) - 1) LOOP\n",
									 var, from.data, bound.data);
				cob_block(cb, ind + 1);
				cob_expect(cb, "END-PERFORM");
				plx_indent(&cb->cx->out, ind);
				appendStringInfoString(&cb->cx->out, "END LOOP;\n");
				return;
			}
		}

		/* general PERFORM VARYING -> WHILE with a manual step */
		initStringInfo(&c);
		cob_value(cb, &c, NULL, 0);
		plx_indent(&cb->cx->out, ind);
		appendStringInfo(&cb->cx->out, "%s := (%s );\n", var, from.data);
		plx_indent(&cb->cx->out, ind);
		appendStringInfo(&cb->cx->out, "WHILE NOT (%s ) LOOP\n", c.data);
		cob_block(cb, ind + 1);
		plx_indent(&cb->cx->out, ind + 1);
		appendStringInfo(&cb->cx->out, "%s := %s + (%s );\n", var, var, by.data);
		cob_expect(cb, "END-PERFORM");
		plx_indent(&cb->cx->out, ind);
		appendStringInfoString(&cb->cx->out, "END LOOP;\n");
	}
	else if (cob_is_verb(cob_cur(cb)))
	{
		/* inline PERFORM ... END-PERFORM (no clause): unconditional loop */
		plx_indent(&cb->cx->out, ind);
		appendStringInfoString(&cb->cx->out, "LOOP\n");
		cob_block(cb, ind + 1);
		cob_expect(cb, "END-PERFORM");
		plx_indent(&cb->cx->out, ind);
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
			plx_indent(&cb->cx->out, ind);
			appendStringInfo(&cb->cx->out, "FOREACH %s IN ARRAY (%s ) LOOP\n", var, e.data);
		}
		else
		{
			PlxLocal2  *row;
			static const char *stops[] = {"USING"};

			row = plx_local_find(cb->cx, var, strlen(var));
			if (!row)
			{
				row = plx_local_add(cb->cx, var, strlen(var));
				row->is_record = true;
			}
			plx_indent(&cb->cx->out, ind);
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
		plx_indent(&cb->cx->out, ind);
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
		plx_indent(&cb->cx->out, ind);
		appendStringInfo(&cb->cx->out, "FOR __plx_cob_%d IN 1..(%s ) LOOP\n", id, n.data);
		cob_block(cb, ind + 1);
		cob_expect(cb, "END-PERFORM");
		plx_indent(&cb->cx->out, ind);
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
	plx_indent(&cb->cx->out, ind);
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
	plx_indent(&cb->cx->out, ind);
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
	plx_indent(&cb->cx->out, ind);
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
		plx_indent(&cb->cx->out, ind);
		appendStringInfo(&cb->cx->out, "RETURN %s;\n", v.data);
	}
	else
	{
		plx_indent(&cb->cx->out, ind);
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
	plx_indent(&cb->cx->out, ind);
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
	plx_indent(&cb->cx->out, ind);
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
	plx_indent(&cb->cx->out, ind);
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
	plx_indent(&cb->cx->out, ind);
	appendStringInfo(&cb->cx->out, "RETURN NEXT %s;\n", v.data);
}

static void
cob_return_query(Cb *cb, int ind)
{
	static const char *stops[] = {"USING"};

	cb->pos++;								/* RETURN-QUERY */
	plx_indent(&cb->cx->out, ind);
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
	plx_indent(&cb->cx->out, ind);
	appendStringInfo(&cb->cx->out, "GET %sDIAGNOSTICS %s = %s;\n",
					 stacked ? "STACKED " : "", var, field);
}

/* BEGIN-TRY ... [WHEN <cond>|OTHER ...] END-TRY  ->  BEGIN..EXCEPTION..END */
static void
cob_try(Cb *cb, int ind)
{
	cb->pos++;								/* BEGIN-TRY */
	plx_indent(&cb->cx->out, ind);
	appendStringInfoString(&cb->cx->out, "BEGIN\n");
	cob_block(cb, ind + 1);
	if (cob_ci(cob_cur(cb), "WHEN"))
	{
		plx_indent(&cb->cx->out, ind);
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
			plx_indent(&cb->cx->out, ind + 1);
			appendStringInfo(&cb->cx->out, "WHEN %s THEN\n", cond);
			cob_block(cb, ind + 2);
		}
	}
	cob_expect(cb, "END-TRY");
	plx_indent(&cb->cx->out, ind);
	appendStringInfoString(&cb->cx->out, "END;\n");
}

/* declare a name as a local of a given kind if it is not a parameter */
static void
cob_declare_kind(Cb *cb, const char *name, const char *typ, bool is_record)
{
	PlxLocal2  *l;

	if (plx_is_param(cb->cx, name, strlen(name)))
		return;
	l = plx_local_find(cb->cx, name, strlen(name));
	if (!l)
		l = plx_local_add(cb->cx, name, strlen(name));
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
	plx_indent(&cb->cx->out, ind);
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
	plx_indent(&cb->cx->out, ind);
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
	plx_indent(&cb->cx->out, ind);
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
	plx_indent(&cb->cx->out, ind);
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
	l = plx_local_find(cb->cx, tgt, strlen(tgt));
	if (l && !l->is_record)
		l->typ = pstrdup("plx_strbuild");	/* the accumulator becomes a builder */
	plx_indent(&cb->cx->out, ind);
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
		plx_indent(&cb->cx->out, ind);
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
		plx_indent(&cb->cx->out, ind);
		appendStringInfoString(&cb->cx->out, "COMMIT;\n");
	}
	else if (cob_ci(tk, "ROLLBACK"))
	{
		cb->pos++;
		plx_indent(&cb->cx->out, ind);
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

void
plx_cobol_parse_body(Ctx *cx)
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

static const PlxSurface cob_surface = {
	.lanname = "plxcobol",
	.block_style = PLX_BLK_COBOL,
	.stmt_semicolon = false,
	.var_sigil = 0,
	.cmt_hash = false,
	.cmt_slash = false,
	.cmt_block = false,
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
	.parse_body = plx_cobol_parse_body,
};

static char *
cob_transpile(const char *src, const PlxFuncMeta *meta)
{
	return plx_transpile(src, meta, &cob_surface, CurrentMemoryContext);
}

const PlxDialect plx_cobol_dialect = {
	.abi_version = PLX_ABI_VERSION,
	.lanname = "plxcobol",
	.transpile = cob_transpile,
	.flags = PLX_TRUSTED,
};

Datum
plx_cob_validator(PG_FUNCTION_ARGS)
{
	return plx_generic_validator(fcinfo, &plx_cobol_dialect);
}

Datum
plx_cob_inline_handler(PG_FUNCTION_ARGS)
{
	return plx_generic_inline_handler(fcinfo, &plx_cobol_dialect);
}

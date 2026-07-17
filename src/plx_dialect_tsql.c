/*
 * plx_dialect_tsql.c - the "plxtsql" dialect (Transact-SQL: SQL Server / Sybase).
 *
 * T-SQL needs real restructuring to become plpgsql: @local variables and inline
 * DECLARE must be hoisted, IF/WHILE bodies use BEGIN..END rather than
 * THEN..END IF / LOOP..END LOOP, and TRY/CATCH becomes an EXCEPTION block. Like
 * COBOL it therefore has its own tokenizer and recursive-descent front end
 * (below); the dialect-neutral engine lives in plx_transpile.c. The shared
 * keyword table is unused by the T-SQL path but kept for consistency.
 */
#include "postgres.h"

#include "fmgr.h"
#include "lib/stringinfo.h"
#include "miscadmin.h"
#include "utils/memutils.h"

#include "plx.h"
#include "plx_int.h"
#include "plx_engine.h"

PG_FUNCTION_INFO_V1(plx_tsql_validator);
PG_FUNCTION_INFO_V1(plx_tsql_inline_handler);

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
		l = plx_local_add(tq->cx, name, (int) strlen(name));
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
			plx_indent(&tq->cx->out, ind);
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
			plx_indent(&tq->cx->out, ind);
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
	plx_indent(&tq->cx->out, ind);
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

			plx_indent(&tq->cx->out, ind);
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
				plx_indent(&tq->cx->out, ind);
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

		plx_indent(&tq->cx->out, ind);
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
	plx_indent(&tq->cx->out, ind);
	appendStringInfoString(&tq->cx->out, "IF");
	tq_emit_range(tq, c0, c1, &tq->cx->out);
	appendStringInfoString(&tq->cx->out, " THEN\n");
	tq->pos = c1;
	tq_body(tq, ind + 1);
	tq_eat_semi(tq);
	if (tq_ci(tq_cur(tq), "ELSE"))
	{
		tq->pos++;
		plx_indent(&tq->cx->out, ind);
		appendStringInfoString(&tq->cx->out, "ELSE\n");
		tq_body(tq, ind + 1);
		tq_eat_semi(tq);
	}
	plx_indent(&tq->cx->out, ind);
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
	plx_indent(&tq->cx->out, ind);
	appendStringInfoString(&tq->cx->out, "WHILE");
	tq_emit_range(tq, c0, c1, &tq->cx->out);
	appendStringInfoString(&tq->cx->out, " LOOP\n");
	tq->pos = c1;
	tq->cx->loopdepth++;
	tq_body(tq, ind + 1);
	tq->cx->loopdepth--;
	tq_eat_semi(tq);
	plx_indent(&tq->cx->out, ind);
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
	plx_indent(&tq->cx->out, ind);
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
	plx_indent(&tq->cx->out, ind);
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
	plx_indent(&tq->cx->out, ind);
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
		plx_indent(&tq->cx->out, ind);
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
	plx_indent(&tq->cx->out, ind);
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
	plx_indent(&tq->cx->out, ind);
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
	plx_indent(&tq->cx->out, ind);
	appendStringInfoString(&tq->cx->out, "EXCEPTION WHEN OTHERS THEN\n");
	while (!(tq_ci(tq_cur(tq), "END") && tq_ci(&tq->t[tq->pos + 1], "CATCH")) &&
		   tq_cur(tq)->kind != TQ_EOF)
		tq_stmt(tq, ind + 1);
	if (tq_cur(tq)->kind == TQ_EOF)
		tq_err(tq, "missing END CATCH");
	tq->pos += 2;							/* END CATCH */
	tq_eat_semi(tq);
	plx_indent(&tq->cx->out, ind);
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
	plx_indent(&tq->cx->out, ind);
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
		plx_indent(&tq->cx->out, ind);
		appendStringInfoString(&tq->cx->out, "EXIT;\n");
		tq_eat_semi(tq);
	}
	else if (tq_ci(tk, "CONTINUE"))
	{
		tq->pos++;
		plx_indent(&tq->cx->out, ind);
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

void
plx_tsql_parse_body(Ctx *cx)
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

static const PlxSurface tsql_surface = {
	.lanname = "plxtsql",
	.block_style = PLX_BLK_TSQL,
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
	.parse_body = plx_tsql_parse_body,
};

static char *
tsql_transpile_entry(const char *src, const PlxFuncMeta *meta)
{
	return plx_transpile(src, meta, &tsql_surface, CurrentMemoryContext);
}

const PlxDialect plx_tsql_dialect = {
	.abi_version = PLX_ABI_VERSION,
	.lanname = "plxtsql",
	.transpile = tsql_transpile_entry,
	.flags = PLX_TRUSTED,
};

Datum
plx_tsql_validator(PG_FUNCTION_ARGS)
{
	return plx_generic_validator(fcinfo, &plx_tsql_dialect);
}

Datum
plx_tsql_inline_handler(PG_FUNCTION_ARGS)
{
	return plx_generic_inline_handler(fcinfo, &plx_tsql_dialect);
}

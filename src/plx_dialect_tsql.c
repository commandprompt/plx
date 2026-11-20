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

#include "catalog/pg_type.h"
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
/*     (x := x || (e) when @x has a string type; + between strings -> ||) */
/*   - SELECT @x = e [, ...] [FROM ...]    -> x := e;  or  SELECT ... INTO   */
/*   - IF cond stmt|BEGIN..END [ELSE ...]  -> IF cond THEN ... END IF;       */
/*   - WHILE cond stmt|BEGIN..END          -> WHILE cond LOOP ... END LOOP;  */
/*   - BEGIN TRY..END TRY BEGIN CATCH..END CATCH -> BEGIN..EXCEPTION..END    */
/*   - PRINT e -> RAISE NOTICE; THROW -> RAISE EXCEPTION; RAISERROR ->       */
/*     RAISE NOTICE (severity 10 or lower) or RAISE EXCEPTION;               */
/*   - a library of type names (INT, NVARCHAR(n), DATETIME, ...) and         */
/*     functions (ISNULL, LEN, GETDATE, IIF, CONVERT, ...).                  */
/* Populates cx->locals + cx->out; the shared assemble path wraps it.       */
/* ======================================================================== */

typedef enum
{
	TQ_EOF, TQ_WORD, TQ_VAR, TQ_GVAR, TQ_NUM, TQ_STR, TQ_OP,
	TQ_LP, TQ_RP, TQ_COMMA, TQ_SEMI, TQ_DOT,
	TQ_QIDENT					/* [delimited] or "quoted" identifier */
} TqKind;

typedef struct
{
	TqKind		kind;
	const char *s;
	int			len;
	int			line;
	char		q;				/* TQ_QIDENT: opening delimiter, '[' or '"' */
} TqTok;

typedef struct
{
	Ctx		   *cx;
	TqTok	   *t;
	int			nt, pos;
	int			nsel;			/* hidden SELECT-assignment loop records */
	int		   *match;			/* match[i]: the ')' matching '(' at i, or -1 */
	int		   *comma1;			/* comma1[i]: first top-level ',' after '(' at i, or -1 */
	int		   *comma2;			/* comma2[i]: second top-level ',' after '(' at i, or -1 */
	bool	   *cat;			/* cat[i]: the '+' at i concatenates; emit || */
	int			emitdepth;		/* tq_emit_range nesting; 0 = a top-level range */
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
		t[n].kind = (k); t[n].s = (st); t[n].len = (ln); t[n].line = line; \
		t[n].q = 0; n++; \
	} while (0)
#define TQALPHA(c) (((c) >= 'A' && (c) <= 'Z') || ((c) >= 'a' && (c) <= 'z') || (c) == '_')
#define TQDIGIT(c) ((c) >= '0' && (c) <= '9')

	while (p < end)
	{
		CHECK_FOR_INTERRUPTS();
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
		if (*p == '[' || *p == '"')						/* [delimited] / "quoted" identifier */
		{
			/* the inner text keeps its doubled closing delimiter (]] or "");
			 * tq_emit_qident undoes it when it writes the PostgreSQL form */
			char		close = (*p == '[') ? ']' : '"';
			const char *st = p + 1,
					   *q = st;
			bool		closed = false;

			while (q < end)
			{
				if (*q == close)
				{
					if (q + 1 < end && q[1] == close)
					{
						q += 2;
						continue;
					}
					closed = true;
					break;
				}
				if (*q == '\n')
					line++;
				q++;
			}
			if (!closed)
				plx_err(tq->cx, line, "unterminated delimited identifier");
			TQPUSH(TQ_QIDENT, st, (int) (q - st));
			t[n - 1].q = *p;
			p = q + 1;
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

/*
 * emit a delimited identifier as a PostgreSQL quoted identifier. T-SQL
 * [a]]b] is the name a]b and "a""b" is a"b; PostgreSQL doubles an embedded
 * '"', so the name keeps its spaces and reserved words. A [bracketed] name is
 * folded to lower case first: T-SQL names are case insensitive under the
 * default collation, so [Orders] must reach a PostgreSQL table created as
 * orders. A "quoted" name keeps its case, as in PostgreSQL.
 */
static void
tq_emit_qident(TqTok *tk, StringInfo out)
{
	char		close = (tk->q == '[') ? ']' : '"';
	int			i;

	appendStringInfoChar(out, '"');
	for (i = 0; i < tk->len; i++)
	{
		char		c = tk->s[i];

		if (c == close && i + 1 < tk->len && tk->s[i + 1] == close)
			i++;				/* doubled closing delimiter: one character */
		if (tk->q == '[' && c >= 'A' && c <= 'Z')
			c = c - 'A' + 'a';
		if (c == '"')
			appendStringInfoChar(out, '"');
		appendStringInfoChar(out, c);
	}
	appendStringInfoChar(out, '"');
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

	if (w->kind != TQ_WORD && w->kind != TQ_QIDENT)
		tq_err(tq, "expected a type name");
	base = tq_base_type(w->s, w->len);
	is_char = (base && (strcmp(base, "varchar") == 0 || strcmp(base, "char") == 0));
	is_bin = (base && strcmp(base, "bytea") == 0);
	if (base)
		appendStringInfoString(dest, base);
	else if (w->kind == TQ_QIDENT)
		tq_emit_qident(w, dest);
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

	if (a >= b || (tq->t[a].kind != TQ_WORD && tq->t[a].kind != TQ_QIDENT))
	{
		appendStringInfoString(dest, "text");
		return;
	}
	w = &tq->t[a];
	base = tq_base_type(w->s, w->len);
	if (base && a + 2 < b && tq->t[a + 1].kind == TQ_LP && tq_ci(&tq->t[a + 2], "MAX"))
	{
		/* VARCHAR(MAX)/NVARCHAR(MAX) -> text; VARBINARY(MAX) -> bytea */
		appendStringInfoString(dest, strcmp(base, "bytea") == 0 ? "bytea" : "text");
		return;
	}
	if (base)
		appendStringInfoString(dest, base);
	else if (w->kind == TQ_QIDENT)
		tq_emit_qident(w, dest);
	else
		appendStringInfoString(dest, tq_ident(w->s, w->len));
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

/*
 * Pair every '(' with its ')' once, after lexing. A forward scan from each
 * '(' (the old tq_match_lp) made unbalanced input such as f(f(f(... cost
 * O(n^2): every call name rescanned to the end of the range. The same pass
 * records the first two top-level commas inside each '(', so CONVERT, IIF and
 * CHARINDEX need no scan of their argument list either: nested calls such as
 * IIF(IIF(IIF(...))) with no comma failed one by one and rescanned each range.
 */
static void
tq_pair_parens(Tq *tq)
{
	int		   *stack = palloc(sizeof(int) * (tq->nt + 1));
	int			sp = 0,
				i;

	tq->match = palloc(sizeof(int) * (tq->nt + 1));
	tq->comma1 = palloc(sizeof(int) * (tq->nt + 1));
	tq->comma2 = palloc(sizeof(int) * (tq->nt + 1));
	tq->cat = palloc0(sizeof(bool) * (tq->nt + 1));
	for (i = 0; i < tq->nt; i++)
	{
		CHECK_FOR_INTERRUPTS();
		tq->match[i] = -1;
		tq->comma1[i] = -1;
		tq->comma2[i] = -1;
		if (tq->t[i].kind == TQ_LP)
			stack[sp++] = i;
		else if (tq->t[i].kind == TQ_RP && sp > 0)
			tq->match[stack[--sp]] = i;
		else if (tq->t[i].kind == TQ_COMMA && sp > 0)
		{
			int			top = stack[sp - 1];

			if (tq->comma1[top] < 0)
				tq->comma1[top] = i;
			else if (tq->comma2[top] < 0)
				tq->comma2[top] = i;
		}
	}
	pfree(stack);
}

/* index of the AS that ends the expression of CAST(e AS type), or -1 */
static int
tq_cast_as(Tq *tq, int lp, int close)
{
	int			i,
				depth = 0;

	for (i = lp + 1; i < close; i++)
	{
		TqTok	   *x = &tq->t[i];

		if (x->kind == TQ_LP)
			depth++;
		else if (x->kind == TQ_RP)
			depth--;
		else if (depth == 0 && tq_ci(x, "AS"))
			return i;
	}
	return -1;
}

/*
 * map the type of CAST(e AS type), the tokens [a,b), into dest with the table
 * CONVERT and DECLARE use. Return false to keep the type as written, which
 * suits a PostgreSQL type name. A T-SQL type whose PostgreSQL type would not
 * keep SQL Server's value is kept too, so the call fails instead of returning
 * a different value: tinyint and smallmoney have a smaller range than smallint
 * and numeric(10,4), binary(n) and varbinary(n) cut the value to n bytes,
 * smalldatetime rounds to the minute, and sysname is nvarchar(128).
 */
static bool
tq_cast_type(Tq *tq, int a, int b, StringInfo dest)
{
	static const char *mapped[] = {
		"nvarchar", "varchar", "ntext", "bit", "money", "datetime",
		"datetime2", "uniqueidentifier", "image", "varbinary", NULL
	};
	TqTok	   *w = &tq->t[a];
	TqTok	   *arg = NULL;
	const char *base;
	bool		is_max;
	int			i;

	if (a >= b || w->kind != TQ_WORD)
		return false;
	if (b - a == 4 && tq->t[a + 1].kind == TQ_LP && tq->t[b - 1].kind == TQ_RP)
		arg = &tq->t[a + 2];
	else if (b - a != 1)
		return false;
	for (i = 0; mapped[i]; i++)
		if (tq_ci(w, mapped[i]))
			break;
	if (mapped[i] == NULL)
		return false;
	base = tq_base_type(w->s, w->len);
	is_max = (arg != NULL && tq_ci(arg, "MAX"));
	if (arg && !is_max && arg->kind != TQ_NUM)
		return false;

	if (tq_ci(w, "varchar") || tq_ci(w, "varbinary"))
	{
		/* only (MAX): varchar(n) is PostgreSQL's own, varbinary(n) cuts */
		if (!is_max)
			return false;
		appendStringInfoString(dest, strcmp(base, "bytea") == 0 ? "bytea" : "text");
		return true;
	}
	if (tq_ci(w, "nvarchar"))
	{
		if (is_max)
			appendStringInfoString(dest, "text");
		else if (arg)
			appendStringInfo(dest, "varchar(%.*s)", arg->len, arg->s);
		else
			appendStringInfoString(dest, "varchar(30)");	/* CAST's default */
		return true;
	}
	if (tq_ci(w, "datetime2") && arg && !is_max && arg->len == 1 &&
		arg->s[0] >= '0' && arg->s[0] <= '7')
	{
		/* datetime2(n) keeps n fractional digits; PostgreSQL keeps 6 at most */
		if (arg->s[0] == '7')
			appendStringInfoString(dest, "timestamp");
		else
			appendStringInfo(dest, "timestamp(%c)", arg->s[0]);
		return true;
	}
	if (arg)
		return false;
	appendStringInfoString(dest, base);
	return true;
}

/* index of the ')' matching the '(' at lp, within [.,limit), or -1 */
static int
tq_match_lp(Tq *tq, int lp, int limit)
{
	int			close = tq->match[lp];

	return (close >= 0 && close < limit) ? close : -1;
}

/*
 * T-SQL + concatenates when its operands are strings and adds when they are
 * numbers; PostgreSQL has no + for strings and spells concatenation ||. plx
 * has no expression types, so it classifies each operand of a chain
 * a + b + c from what it can see:
 *   TC_STR  a local or parameter declared with a variable-length string
 *           type, CAST or CONVERT to one, a string function, ISNULL whose first
 *           argument is a string local or literal, or COALESCE whose every
 *           argument is a known string;
 *   TC_LIT  a string literal;
 *   TC_NUM  a number, a numeric local or parameter, CAST or CONVERT to a
 *           numeric type, or a product (a * b binds tighter than +);
 *   TC_UNK  anything else (a column, another call, a parenthesized expression).
 * TC_STR and TC_LIT are known strings. A chain concatenates only when every
 * operand is a known string and no -, &, | or ^ follows it; then each + becomes
 * ||. Anything else keeps every + as +, so a string next to anything else fails
 * at run time ("operator does not exist") instead of giving a wrong value.
 * Requiring every operand, not only the two beside one +, keeps the grouping:
 * T-SQL reads a + b + c as (a + b) + c, but PostgreSQL binds + tighter than ||,
 * so a || b + c would mean a || (b + c).
 */
typedef enum
{
	TC_UNK, TC_NUM, TC_LIT, TC_STR
} TqCls;

/*
 * class of a plpgsql type text from DECLARE (varchar(10), text, integer, ...).
 * char(n) is not a string here: SQL Server keeps its trailing spaces when it
 * concatenates, PostgreSQL's || drops them, so || would give a different value.
 */
static TqCls
tq_type_class(const char *typ)
{
	static const char *str[] = {"varchar", "text", NULL};
	static const char *num[] = {"integer", "bigint", "smallint", "numeric",
	"real", "double precision", NULL};
	int			len,
				i;

	if (typ == NULL)
		return TC_UNK;
	len = strcspn(typ, "(");
	for (i = 0; str[i]; i++)
		if ((int) strlen(str[i]) == len && strncmp(typ, str[i], len) == 0)
			return TC_STR;
	for (i = 0; num[i]; i++)
		if ((int) strlen(num[i]) == len && strncmp(typ, num[i], len) == 0)
			return TC_NUM;
	return TC_UNK;
}

/* class of an @variable: a DECLAREd local, else a parameter, else unknown */
static TqCls
tq_var_class(Tq *tq, TqTok *tk)
{
	char	   *name = tq_ident(tk->s, tk->len);
	PlxLocal2  *l = plx_local_find(tq->cx, name, tk->len);
	const PlxFuncMeta *m = tq->cx->meta;
	int			i;

	if (l)
		return l->is_record ? TC_UNK : tq_type_class(l->typ);
	if (m == NULL || m->argnames == NULL || m->argtypes == NULL)
		return TC_UNK;
	for (i = 0; i < m->nargs; i++)
	{
		const char *an = m->argnames[i];

		if (an && (int) strlen(an) == tk->len &&
			pg_strncasecmp(an, tk->s, tk->len) == 0)
		{
			switch (m->argtypes[i])
			{
				case VARCHAROID:
				case TEXTOID:
					return TC_STR;
				case INT2OID:
				case INT4OID:
				case INT8OID:
				case NUMERICOID:
				case FLOAT4OID:
				case FLOAT8OID:
					return TC_NUM;
				default:
					return TC_UNK;
			}
		}
	}
	return TC_UNK;
}

/* class of a T-SQL type name token (CAST/CONVERT target) */
static TqCls
tq_typetok_class(TqTok *tk)
{
	const char *base;

	if (tk->kind != TQ_WORD)
		return TC_UNK;
	base = tq_base_type(tk->s, tk->len);
	return base ? tq_type_class(base) : TC_UNK;
}

static bool tq_known_str(Tq *tq, int a, int b);

/* class of the call whose name is at i and whose ')' is at close */
static TqCls
tq_call_class(Tq *tq, int i, int close)
{
	static const char *strfn[] = {
		"UPPER", "LOWER", "LTRIM", "RTRIM", "TRIM", "SUBSTRING", "LEFT",
		"RIGHT", "REPLACE", "REPLICATE", "CONCAT", "STUFF", NULL
	};
	TqTok	   *tk = &tq->t[i];
	TqTok	   *a1 = &tq->t[i + 2];
	int			k;

	if (tq_ci(tk, "CONVERT"))
		return tq_typetok_class(a1);
	if (tq_ci(tk, "CAST"))
	{
		/* CAST(e AS type) or CAST(e AS type(n)): the type sits before ')' */
		if (close - 2 > i + 1 && tq_ci(&tq->t[close - 2], "AS"))
			return tq_typetok_class(&tq->t[close - 1]);
		if (close - 5 > i + 1 && tq->t[close - 1].kind == TQ_RP &&
			tq->t[close - 3].kind == TQ_LP && tq_ci(&tq->t[close - 5], "AS"))
			return tq_typetok_class(&tq->t[close - 4]);
		return TC_UNK;
	}
	if (tq_ci(tk, "ISNULL"))
	{
		/* ISNULL takes the type of its first argument */
		if (a1->kind == TQ_STR ||
			(a1->kind == TQ_VAR && tq_var_class(tq, a1) == TC_STR))
			return TC_STR;
		return TC_UNK;
	}
	if (tq_ci(tk, "COALESCE"))
	{
		/*
		 * COALESCE takes the type of its highest-precedence argument, and
		 * PostgreSQL resolves COALESCE('1', 2) to integer: it is a known string
		 * only when every argument is one.
		 */
		int			a = i + 2,
					j;

		for (j = a; j <= close; j++)
		{
			if (j == close || tq->t[j].kind == TQ_COMMA)
			{
				if (!tq_known_str(tq, a, j))
					return TC_UNK;
				a = j + 1;
			}
			else if (tq->t[j].kind == TQ_LP && tq->match[j] > j &&
					 tq->match[j] < close)
				j = tq->match[j];
		}
		return TC_STR;
	}
	for (k = 0; strfn[k]; k++)
		if (tq_ci(tk, strfn[k]))
			return TC_STR;
	return TC_UNK;
}

/* a word that cannot be (or start) an operand of + */
static bool
tq_operand_kw(TqTok *tk)
{
	static const char *k[] = {
		"AND", "OR", "NOT", "IS", "IN", "LIKE", "BETWEEN", "CASE", "WHEN",
		"THEN", "ELSE", "END", "AS", "FROM", "WHERE", "ESCAPE", "COLLATE",
		"EXISTS", "ON", "BY", "HAVING", "TOP", "DISTINCT", "ALL", "ANY",
		"SOME", "UNION", "INTO", "VALUES", "BEGIN", NULL
	};
	int			i;

	for (i = 0; k[i]; i++)
		if (tq_ci(tk, k[i]))
			return true;
	return tq_is_stmt_kw(tk);
}

/* one operand without a product: end index past it, or -1; sets *cls */
static int
tq_operand1(Tq *tq, int i, int b, TqCls *cls)
{
	TqTok	   *tk;
	int			e,
				close;

	if (i >= b)
		return -1;
	tk = &tq->t[i];
	*cls = TC_UNK;
	switch (tk->kind)
	{
		case TQ_VAR:
			*cls = tq_var_class(tq, tk);
			return i + 1;
		case TQ_GVAR:
			return i + 1;
		case TQ_NUM:
			*cls = TC_NUM;
			return i + 1;
		case TQ_STR:
			*cls = TC_LIT;
			return i + 1;
		case TQ_LP:
			close = tq_match_lp(tq, i, b);
			return close < 0 ? -1 : close + 1;
		case TQ_WORD:
		case TQ_QIDENT:
			if (tk->kind == TQ_WORD && tq_operand_kw(tk))
				return -1;
			e = i + 1;
			while (e + 1 < b && tq->t[e].kind == TQ_DOT &&
				   (tq->t[e + 1].kind == TQ_WORD || tq->t[e + 1].kind == TQ_QIDENT))
				e += 2;
			if (e < b && tq->t[e].kind == TQ_LP)
			{
				close = tq_match_lp(tq, e, b);
				if (close < 0)
					return -1;
				if (e == i + 1 && tk->kind == TQ_WORD)
					*cls = tq_call_class(tq, i, close);
				e = close + 1;
			}
			return e;
		default:
			return -1;
	}
}

/* one operand of +, a product counting as one numeric operand */
static int
tq_operand(Tq *tq, int i, int b, TqCls *cls)
{
	int			e = tq_operand1(tq, i, b, cls);

	while (e >= 0 && e < b && tq->t[e].kind == TQ_OP && tq->t[e].len == 1 &&
		   strchr("*/%", tq->t[e].s[0]))
	{
		TqCls		c;
		int			n = tq_operand1(tq, e + 1, b, &c);

		if (n < 0)
			break;
		e = n;
		*cls = TC_NUM;
	}
	return e;
}

static bool
tq_is_plus(TqTok *tk)
{
	return tk->kind == TQ_OP && tk->len == 1 && tk->s[0] == '+';
}

/* can an operand chain start at i, judged by the token before it? */
static bool
tq_chain_start(Tq *tq, int i)
{
	TqTok	   *p;

	if (i == 0)
		return true;
	p = &tq->t[i - 1];
	switch (p->kind)
	{
		case TQ_LP:
		case TQ_COMMA:
		case TQ_SEMI:
			return true;
		case TQ_WORD:
			/* not the argument list of a call such as CONVERT(...) */
			return tq->t[i].kind != TQ_LP || tq_operand_kw(p);
		case TQ_OP:
			return !(p->len == 1 && strchr("+-*/%~&|^", p->s[0]));
		default:
			return false;
	}
}

/*
 * Walk the operand chain a + b + ... starting at i. Returns the index past its
 * last operand (or -1 when no operand starts at i), sets *n to the number of
 * operands and *all_str to whether every one is a known string (see TqCls).
 */
static int
tq_chain_scan(Tq *tq, int i, int b, int *n, bool *all_str)
{
	int			e = i,
				end = -1;
	TqCls		c;

	*n = 0;
	*all_str = true;
	for (;;)
	{
		e = tq_operand(tq, e, b, &c);
		if (e < 0)
			break;
		end = e;
		(*n)++;
		*all_str &= (c == TC_STR || c == TC_LIT);
		if (e >= b || !tq_is_plus(&tq->t[e]))
			break;
		e++;
	}
	return end;
}

/* is [a,b) a known string: one operand, or a + chain of them (see TqCls)? */
static bool
tq_known_str(Tq *tq, int a, int b)
{
	int			n;
	bool		all_str;

	return tq_chain_scan(tq, a, b, &n, &all_str) == b && n >= 1 && all_str;
}

/* decide each + of the operand chain starting at i (see TqCls) */
static void
tq_chain(Tq *tq, int i, int b)
{
	int			n,
				e;
	bool		all_str;
	TqTok	   *nx;

	e = tq_chain_scan(tq, i, b, &n, &all_str);
	if (n < 2 || !all_str)
		return;
	/* an operator after the chain that binds as + does would regroup it */
	nx = e < b ? &tq->t[e] : NULL;
	if (nx && (tq_is_plus(nx) ||
			   (nx->kind == TQ_OP && nx->len == 1 && strchr("-*/%&|^", nx->s[0]))))
		return;
	for (e = i; e >= 0 && e < b;)
	{
		TqCls		c;

		e = tq_operand(tq, e, b, &c);
		if (e < 0 || e >= b || !tq_is_plus(&tq->t[e]))
			break;
		tq->cat[e] = true;
		e++;
	}
}

/* decide every + in the top-level expression [a,b) */
static void
tq_mark_concat(Tq *tq, int a, int b)
{
	int			i;

	for (i = a; i < b; i++)
	{
		CHECK_FOR_INTERRUPTS();
		if (tq_chain_start(tq, i))
			tq_chain(tq, i, b);
	}
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
					comma = tq->comma1[lp],
					comma2 = tq->comma2[lp];
		StringInfoData ty;

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

	/* CAST(e AS type): map a T-SQL type name as CONVERT does */
	if (tq_ci(tk, "CAST"))
	{
		int			as = tq_cast_as(tq, lp, close);
		StringInfoData ty;

		if (as < 0)
			return false;
		initStringInfo(&ty);
		if (!tq_cast_type(tq, as + 1, close, &ty))
			return false;
		tq_sp(out);
		appendStringInfoString(out, "CAST(");
		tq_emit_range(tq, lp + 1, as, out);
		appendStringInfo(out, " AS %s)", ty.data);
		*ni = close + 1;
		return true;
	}

	/* IIF(cond, a, b) -> CASE WHEN cond THEN a ELSE b END */
	if (tq_ci(tk, "IIF"))
	{
		int			c1 = tq->comma1[lp],
					c2 = tq->comma2[lp];

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

	/* LEN(x) -> length(rtrim(x)): T-SQL LEN ignores trailing spaces */
	if (tq_ci(tk, "LEN"))
	{
		tq_sp(out);
		appendStringInfoString(out, "length(rtrim(");
		tq_emit_range(tq, lp + 1, close, out);
		appendStringInfoString(out, "))");
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
			{"isnull", "coalesce"},
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
					int			comma = tq->comma1[lp];

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
tq_emit_range_inner(Tq *tq, int a, int b, StringInfo out)
{
	int			i = a;

	while (i < b)
	{
		TqTok	   *tk = &tq->t[i];

		CHECK_FOR_INTERRUPTS();
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
		if (tk->kind == TQ_QIDENT)
		{
			tq_sp(out);
			tq_emit_qident(tk, out);
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
		if (tk->kind == TQ_OP && tk->len == 1 && tk->s[0] == '~')
		{
			/*
			 * T-SQL binds unary ~ tighter than any other operator; PostgreSQL
			 * binds prefix ~ looser than + and *. Apply it to its operand only:
			 * ~a + b becomes (~a) + b. A run of ~ nests through the recursion.
			 */
			TqCls		c;
			int			e = i + 1;

			while (e < b && tq->t[e].kind == TQ_OP && tq->t[e].len == 1 &&
				   tq->t[e].s[0] == '~')
				e++;
			e = tq_operand1(tq, e, b, &c);
			if (e > i + 1)
			{
				tq_sp(out);
				appendStringInfoString(out, "(~");
				tq_emit_range(tq, i + 1, e, out);
				appendStringInfoChar(out, ')');
				i = e;
				continue;
			}
		}
		if (tk->kind == TQ_OP)
		{
			tq_sp(out);
			if (tk->len == 2 && tk->s[0] == '!' && tk->s[1] == '=')
				appendStringInfoString(out, "<>");
			else if (tk->len == 1 && tk->s[0] == '^')
				appendStringInfoChar(out, '#');	/* T-SQL XOR; PG ^ is power */
			else if (tq->cat[i])
				appendStringInfoString(out, "||");	/* string + (see TqCls) */
			else
				appendBinaryStringInfo(out, tk->s, tk->len);
			i++;
			continue;
		}
		i++;
	}
}

/*
 * Recursion guard for the expression emitter. Nested calls, subscripts and
 * literals recurse through here once per level; without check_stack_depth()
 * hostile nesting overflows the C stack and crashes the backend (SIGSEGV).
 */
static void
tq_emit_range(Tq *tq, int a, int b, StringInfo out)
{
	check_stack_depth();
	if (++tq->cx->depth > PLX_MAX_DEPTH)
		tq_err(tq, "expression nested too deeply");
	if (tq->emitdepth++ == 0)
		tq_mark_concat(tq, a, b);
	tq_emit_range_inner(tq, a, b, out);
	tq->emitdepth--;
	tq->cx->depth--;
}

/*
 * CASE ... END nesting for the expression scanners below. T-SQL has CASE only
 * as an expression, so every CASE opens one and its END closes it. Returns
 * true when tk opens, closes or sits inside a CASE, so the caller must not
 * take an END, ELSE, BEGIN or statement keyword there as the end of the
 * expression. ';' and EOF still end it.
 */
static bool
tq_in_case(TqTok *tk, int *cdepth)
{
	if (tq_ci(tk, "CASE"))
	{
		(*cdepth)++;
		return true;
	}
	if (*cdepth > 0)
	{
		if (tq_ci(tk, "END"))
			(*cdepth)--;
		return tk->kind != TQ_SEMI && tk->kind != TQ_EOF;
	}
	return false;
}

/* end of an expression/raw statement: first depth-0 ; EOF END or ELSE */
static int
tq_value_end(Tq *tq, int from)
{
	int			i = from,
				depth = 0,
				cdepth = 0;

	while (i < tq->nt)
	{
		TqTok	   *tk = &tq->t[i];

		CHECK_FOR_INTERRUPTS();
		if (tk->kind == TQ_LP)
			depth++;
		else if (tk->kind == TQ_RP)
		{
			if (depth > 0)
				depth--;
		}
		else if (depth == 0 && !tq_in_case(tk, &cdepth))
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
				depth = 0,
				cdepth = 0;

	while (i < tq->nt)
	{
		TqTok	   *tk = &tq->t[i];

		CHECK_FOR_INTERRUPTS();
		if (tk->kind == TQ_LP)
			depth++;
		else if (tk->kind == TQ_RP)
		{
			if (depth > 0)
				depth--;
		}
		else if (depth == 0 && !tq_in_case(tk, &cdepth))
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
				int			depth = 0,
							cdepth = 0;

				while (e1 < tq->nt)
				{
					TqTok	   *x = &tq->t[e1];

					if (x->kind == TQ_LP)
						depth++;
					else if (x->kind == TQ_RP && depth > 0)
						depth--;
					else if (depth == 0 && !tq_in_case(x, &cdepth) &&
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
	if (arith == '+' && tq_var_class(tq, v) == TC_STR && tq_known_str(tq, e0, e1))
	{
		/*
		 * += of a known string onto a string variable concatenates in T-SQL.
		 * With anything else, such as a number, SQL Server adds; plx keeps +,
		 * which fails at run time rather than concatenate.
		 */
		appendStringInfo(&tq->cx->out, "%s := %s || (", name, name);
		tq_emit_range(tq, e0, e1, &tq->cx->out);
		appendStringInfoString(&tq->cx->out, ");\n");
	}
	else if (arith)
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

/* qsort/bsearch comparator for an array of C strings */
static int
tq_cmp_name(const void *a, const void *b)
{
	return strcmp(*(char *const *) a, *(char *const *) b);
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
		int			cap = 8,
					nassign = 0;
		char	  **names = palloc(sizeof(char *) * cap);
		int		   *estart = palloc(sizeof(int) * cap),
				   *eend = palloc(sizeof(int) * cap);
		int			frompos = -1;

		for (;;)
		{
			TqTok	   *v = tq_cur(tq);
			int			depth = 0,
						cdepth = 0,
						e0,
						e1;

			/* each list entry must be "@v =": a VAR is never the EOF token, so
			 * the token after it exists */
			if (v->kind != TQ_VAR ||
				!(tq->t[tq->pos + 1].kind == TQ_OP && tq->t[tq->pos + 1].len == 1 &&
				  tq->t[tq->pos + 1].s[0] == '='))
				tq_err(tq, "expected @variable = expression in the SELECT assignment list");
			if (nassign >= cap)
			{
				cap *= 2;
				names = repalloc(names, sizeof(char *) * cap);
				estart = repalloc(estart, sizeof(int) * cap);
				eend = repalloc(eend, sizeof(int) * cap);
			}
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
				else if (depth == 0 && !tq_in_case(x, &cdepth))
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
			/*
			 * T-SQL runs the assignments once per row: the variables end up
			 * holding the last row, and keep their values when no row
			 * matches. A plpgsql FOR loop over the query that assigns the
			 * variables in its body does exactly that, where SELECT INTO
			 * would take the first row and set NULL on no row. An expression that reads
			 * one of the assigned variables (the accumulate idiom
			 * SELECT @t = @t + v) cannot be lowered this way, because the
			 * query sees the values from before the loop; reject it rather
			 * than compute a different result.
			 */
			int			k,
						j,
						rend = tq_value_end(tq, frompos);
			char	   *rec;
			char	  **sorted = palloc(sizeof(char *) * nassign);

			/* sorted copy of the targets, so each @ref is one bsearch */
			memcpy(sorted, names, sizeof(char *) * nassign);
			qsort(sorted, nassign, sizeof(char *), tq_cmp_name);
			for (k = 0; k < nassign; k++)
				for (j = estart[k]; j < eend[k]; j++)
				{
					char	   *ref;

					if (tq->t[j].kind != TQ_VAR)
						continue;
					ref = tq_ident(tq->t[j].s, tq->t[j].len);
					if (bsearch(&ref, sorted, nassign, sizeof(char *), tq_cmp_name))
						plx_err(tq->cx, tq->t[j].line,
								"SELECT @%s = ... FROM reads @%s in its own assignment list; use an aggregate such as SUM() or string_agg() instead",
								names[k], ref);
					pfree(ref);
				}
			pfree(sorted);

			/* loop over a hidden record (the quoted mixed-case name cannot
			 * clash with a lower-cased @variable) and assign in the body: a
			 * plpgsql FOR loop sets its own target to NULL when no row comes
			 * back, so the T-SQL variables must not be the loop target */
			rec = psprintf("\"TqSel%d\"", ++tq->nsel);
			plx_local_add(tq->cx, rec, (int) strlen(rec))->is_record = true;
			plx_indent(&tq->cx->out, ind);
			appendStringInfo(&tq->cx->out, "FOR %s IN SELECT", rec);
			for (k = 0; k < nassign; k++)
			{
				if (k)
					appendStringInfoChar(&tq->cx->out, ',');
				tq_emit_range(tq, estart[k], eend[k], &tq->cx->out);
				appendStringInfo(&tq->cx->out, " AS c%d", k + 1);
			}
			tq_emit_range(tq, frompos, rend, &tq->cx->out);
			appendStringInfoString(&tq->cx->out, " LOOP\n");
			for (k = 0; k < nassign; k++)
			{
				plx_indent(&tq->cx->out, ind + 1);
				appendStringInfo(&tq->cx->out, "%s := %s.c%d;\n", names[k], rec, k + 1);
			}
			plx_indent(&tq->cx->out, ind);
			appendStringInfoString(&tq->cx->out, "END LOOP;\n");
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
				c1,
				k;

	tq->pos++;								/* IF */
	c0 = tq->pos;
	c1 = tq_cond_end(tq, c0);
	/* plpgsql ends an IF condition at its first THEN, so a condition that
	 * holds a CASE ... THEN goes in parentheses. IIF( becomes CASE ... THEN
	 * when it is emitted, so it counts too. */
	for (k = c0; k < c1; k++)
		if (tq_ci(&tq->t[k], "CASE") ||
			(tq_ci(&tq->t[k], "IIF") && k + 1 < c1 && tq->t[k + 1].kind == TQ_LP))
			break;
	plx_indent(&tq->cx->out, ind);
	appendStringInfoString(&tq->cx->out, k < c1 ? "IF (" : "IF");
	tq_emit_range(tq, c0, c1, &tq->cx->out);
	appendStringInfoString(&tq->cx->out, k < c1 ? ") THEN\n" : " THEN\n");
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

/* the message argument of RAISERROR(msg, sev, state, ...) or THROW n, msg, s,
 * raised at level ("NOTICE", "LOG" or "EXCEPTION") */
static void
tq_raise_msg(Tq *tq, int msg0, int msg1, int ind, const char *level)
{
	plx_indent(&tq->cx->out, ind);
	if (msg1 <= msg0)
	{
		appendStringInfo(&tq->cx->out, "RAISE %s 'error';\n", level);
		return;
	}
	appendStringInfo(&tq->cx->out, "RAISE %s '%%',", level);
	tq_emit_range(tq, msg0, msg1, &tq->cx->out);
	appendStringInfoString(&tq->cx->out, ";\n");
}

/* how a RAISERROR severity argument [a,b) decides the RAISE level */
typedef enum
{
	TQ_SEV_NOTICE,				/* literal 10 or lower (but not -1) */
	TQ_SEV_EXCEPTION,			/* literal 11 or higher, -1, ERROR_SEVERITY() */
	TQ_SEV_RUNTIME				/* any other expression: test it at run time */
} TqSev;

static TqSev
tq_severity(Tq *tq, int a, int b)
{
	bool		neg = false;
	long		v = 0;
	int			k;

	if (a >= b)
		return TQ_SEV_EXCEPTION;		/* no severity (malformed): as before */

	/* ERROR_SEVERITY(): only an error reaches a CATCH block, so the caught
	 * severity is always 11 or higher (and PostgreSQL has no such function) */
	if (b - a == 3 && tq_ci(&tq->t[a], "ERROR_SEVERITY") &&
		tq->t[a + 1].kind == TQ_LP && tq->t[a + 2].kind == TQ_RP)
		return TQ_SEV_EXCEPTION;

	/* an integer literal with an optional unary minus */
	if (tq->t[a].kind == TQ_OP && tq->t[a].len == 1 && tq->t[a].s[0] == '-')
	{
		neg = true;
		a++;
	}
	if (b - a != 1 || tq->t[a].kind != TQ_NUM)
		return TQ_SEV_RUNTIME;
	for (k = 0; k < tq->t[a].len; k++)
	{
		char		c = tq->t[a].s[k];

		if (c < '0' || c > '9')
			return TQ_SEV_RUNTIME;
		if (v < 1000000)		/* cap: only the comparison with 10 matters */
			v = v * 10 + (c - '0');
	}
	if (neg)
		v = -v;
	/* -1 means the severity stored for the message, which plx cannot look
	 * up, so it fails safe; any other negative counts as 0 */
	if (v == -1)
		return TQ_SEV_EXCEPTION;
	return v <= 10 ? TQ_SEV_NOTICE : TQ_SEV_EXCEPTION;
}

/*
 * RAISERROR ( msg , severity , state [, args] ) [WITH option [, ...]]
 *
 * As in SQL Server, a severity of 10 or lower is an informational message
 * (RAISE NOTICE, which a CATCH block does not catch) and 11 or higher raises
 * an error. WITH NOWAIT and WITH SETERROR need no translation; WITH LOG adds
 * RAISE LOG.
 */
static void
tq_raiserror(Tq *tq, int ind)
{
	int			lp,
				close,
				depth = 0,
				c1 = -1,
				c2 = -1,
				msg1,
				j;
	bool		want_log = false;
	TqSev		sev;

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
			if (c1 < 0)
				c1 = j;
			else
			{
				c2 = j;
				break;
			}
		}
	}
	msg1 = c1 > 0 ? c1 : close;
	sev = c1 > 0 ? tq_severity(tq, c1 + 1, c2 > 0 ? c2 : close) : TQ_SEV_EXCEPTION;
	tq->pos = close + 1;

	/* WITH NOWAIT | LOG | SETERROR [, ...]: never left as a stray statement */
	if (tq_ci(tq_cur(tq), "WITH"))
	{
		tq->pos++;
		for (;;)
		{
			TqTok	   *o = tq_cur(tq);

			if (o->kind != TQ_WORD)
				tq_err(tq, "expected option after WITH in RAISERROR");
			if (tq_ci(o, "LOG"))
				want_log = true;
			else if (!tq_ci(o, "NOWAIT") && !tq_ci(o, "SETERROR"))
				plx_err(tq->cx, o->line, "unknown RAISERROR option: %.*s",
						o->len, o->s);
			tq->pos++;
			if (tq_cur(tq)->kind != TQ_COMMA)
				break;
			tq->pos++;
		}
	}

	/* WITH LOG: the message expression is emitted (and evaluated) a second
	 * time; harmless for the literals and variables RAISERROR takes */
	if (want_log)
		tq_raise_msg(tq, lp + 1, msg1, ind, "LOG");
	if (sev == TQ_SEV_RUNTIME)
	{
		int			s0 = c1 + 1,
					s1 = c2 > 0 ? c2 : close;

		/* NULL fails both tests and raises, like -1 */
		plx_indent(&tq->cx->out, ind);
		appendStringInfoString(&tq->cx->out, "IF (");
		tq_emit_range(tq, s0, s1, &tq->cx->out);
		appendStringInfoString(&tq->cx->out, ") <= 10 AND (");
		tq_emit_range(tq, s0, s1, &tq->cx->out);
		appendStringInfoString(&tq->cx->out, ") <> -1 THEN\n");
		tq_raise_msg(tq, lp + 1, msg1, ind + 1, "NOTICE");
		plx_indent(&tq->cx->out, ind);
		appendStringInfoString(&tq->cx->out, "ELSE\n");
		tq_raise_msg(tq, lp + 1, msg1, ind + 1, "EXCEPTION");
		plx_indent(&tq->cx->out, ind);
		appendStringInfoString(&tq->cx->out, "END IF;\n");
	}
	else
		tq_raise_msg(tq, lp + 1, msg1, ind,
					 sev == TQ_SEV_NOTICE ? "NOTICE" : "EXCEPTION");
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
			tq_raise_msg(tq, c1 + 1, c2, ind, "EXCEPTION");
		else
			tq_raise_msg(tq, e0, e1, ind, "EXCEPTION");
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

static void
plx_tsql_parse_body(Ctx *cx)
{
	Tq			tq;

	memset(&tq, 0, sizeof(tq));
	tq.cx = cx;
	tq_lex(&tq, cx->body);
	tq_pair_parens(&tq);
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
		plx_locals_reset(cx);
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

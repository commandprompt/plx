/*
 * plx_dialect_plsql.c - the "plxplsql" dialect (Oracle PL/SQL).
 *
 * PL/SQL and plpgsql are both Ada-descended, so most PL/SQL is already valid
 * plpgsql. The front end (below) is a layout-preserving token rewriter that
 * translates the Oracle-specific spellings
 * (NUMBER/VARCHAR2/..., DBMS_OUTPUT.PUT_LINE, RAISE_APPLICATION_ERROR, EXECUTE
 * IMMEDIATE, FROM DUAL, NVL, seq.NEXTVAL, SYSDATE) and emits the body directly,
 * since PL/SQL already carries its own DECLARE/BEGIN/END structure.
 */
#include "postgres.h"

#include "fmgr.h"
#include "lib/stringinfo.h"
#include "miscadmin.h"
#include "utils/memutils.h"

#include "plx.h"
#include "plx_int.h"
#include "plx_engine.h"

PG_FUNCTION_INFO_V1(plx_plsql_validator);
PG_FUNCTION_INFO_V1(plx_plsql_inline_handler);

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

void
plx_plsql_parse_body(Ctx *cx)
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

static const PlxSurface plsql_surface = {
	.lanname = "plxplsql",
	.block_style = PLX_BLK_PLSQL,
	.stmt_semicolon = true,
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
	.parse_body = plx_plsql_parse_body,
	.self_contained_block = true,
};

static char *
plsql_transpile_body(const char *src, const PlxFuncMeta *meta)
{
	return plx_transpile(src, meta, &plsql_surface, CurrentMemoryContext);
}

const PlxDialect plx_plsql_dialect = {
	.abi_version = PLX_ABI_VERSION,
	.lanname = "plxplsql",
	.transpile = plsql_transpile_body,
	.flags = PLX_TRUSTED,
};

Datum
plx_plsql_validator(PG_FUNCTION_ARGS)
{
	return plx_generic_validator(fcinfo, &plx_plsql_dialect);
}

Datum
plx_plsql_inline_handler(PG_FUNCTION_ARGS)
{
	return plx_generic_inline_handler(fcinfo, &plx_plsql_dialect);
}

/*
 * plx_dialect_csharp.c - the "plxcsharp" dialect (C#).
 *
 * C# is a brace-family language, but it puts the type first in declarations,
 * writes strings in several forms (regular "..", verbatim @"..", raw
 * """..""", and interpolated $".."), and has statements with no plxjs
 * spelling: do/while, a switch whose break leaves the switch, catch clauses
 * with exception classes and when-filters, and throw. The front end runs in
 * two stages, both in this file:
 *
 *   Stage 1 is a C# scanner and recursive-descent parser. It reads the body
 *   and writes a normalized program: statements in one fixed brace form, and
 *   every expression already lowered to SQL text. Types are resolved there,
 *   so string '+' becomes '||' while numeric '+' stays '+', '==' becomes '=',
 *   members such as s.Length or Math.Max become SQL functions, interpolated
 *   strings become f-strings (which the engine binds as SQL template values),
 *   and a declaration becomes a 'let' carrying a type annotation comment.
 *
 *   Stage 2 lexes the normalized program with the shared engine lexer and
 *   lowers its statements to plpgsql. It owns the statements whose C#
 *   semantics differ from the shared brace parser (switch, do/while, the
 *   general for loop with continue, catch, throw) and hands every leaf
 *   statement to the engine (plx_emit_core), so assignments, the SQL
 *   intrinsics, return, try/finally and string building behave exactly as in
 *   the other dialects.
 *
 * The original C# is what gets embedded in pg_proc.prosrc for recovery.
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

PG_FUNCTION_INFO_V1(plx_csharp_validator);
PG_FUNCTION_INFO_V1(plx_csharp_inline_handler);

/* ======================================================================== */
/* Stage 1: tokens                                                          */
/* ======================================================================== */

typedef enum
{
	CT_EOF, CT_ID, CT_NUM, CT_STR, CT_ISTR, CT_CHAR, CT_OP
} CsKind;

typedef struct CsPart CsPart;

typedef struct CsTok
{
	CsKind		kind;
	const char *s;				/* source text (CT_ID: the name, without '@') */
	int			len;
	int			line;
	bool		atid;			/* CT_ID written @name: never a keyword */
	char	   *val;			/* CT_STR/CT_CHAR: decoded bytes; CT_NUM: SQL text */
	int			vlen;
	CsPart	   *parts;			/* CT_ISTR */
	int			nparts;
	bool		real;			/* CT_NUM: a real literal (fraction, exponent, suffix) */
	char		suffix;			/* CT_NUM: 0, 'L', 'U', 'W' (ulong), 'F', 'D', 'M' */
} CsTok;

struct CsPart
{
	bool		hole;
	char	   *lit;			/* literal run (decoded) */
	int			litlen;
	CsTok	   *toks;			/* hole expression, CT_EOF terminated */
	int			ntoks;
	char	   *fmt;			/* format specifier after ':', or NULL */
	bool		align;			/* an ",alignment" was written */
	int			line;
};

typedef struct
{
	CsTok	   *t;
	int			n,
				cap;
} CsTokVec;

static CsTok *
tv_push(CsTokVec *v)
{
	if (v->n >= v->cap)
	{
		v->cap = v->cap ? v->cap * 2 : 64;
		v->t = v->t ? repalloc(v->t, sizeof(CsTok) * v->cap)
			: palloc(sizeof(CsTok) * v->cap);
	}
	memset(&v->t[v->n], 0, sizeof(CsTok));
	return &v->t[v->n++];
}

typedef struct
{
	Ctx		   *cx;
	const char *p;
	const char *end;
	int			line;
	int			depth;			/* nested interpolation holes */
} CsLex;

pg_noreturn static void
lex_err(CsLex *L, const char *msg)
{
	plx_err(L->cx, L->line, "%s", msg);
}

static bool
cs_ident_start(char c)
{
	return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

static bool
cs_ident_char(char c)
{
	return cs_ident_start(c) || (c >= '0' && c <= '9');
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

/* Append Unicode code point c to buf in the server encoding. */
static void
append_codepoint(CsLex *L, StringInfo buf, pg_wchar c)
{
	unsigned char tmp[MAX_UNICODE_EQUIVALENT_STRING + 1];

	if (c == 0)
		lex_err(L, "a NUL character cannot be stored in a PostgreSQL string");
	if (c == 1)
		lex_err(L, "the control character U+0001 is not allowed in a string");
	if (c >= 0xD800 && c <= 0xDFFF)
		lex_err(L, "invalid Unicode escape: a lone surrogate");
	if (c > 0x10FFFF)
		lex_err(L, "invalid Unicode escape: beyond U+10FFFF");
	if (c < 0x80)
	{
		appendStringInfoChar(buf, (char) c);
		return;
	}
	pg_unicode_to_server(c, tmp);
	appendStringInfoString(buf, (char *) tmp);
}

/* read exactly n hex digits at L->p (advancing); -1 if they are not there */
static long
read_hex(CsLex *L, int n)
{
	long		v = 0;
	int			i;

	for (i = 0; i < n; i++)
	{
		int			h;

		if (L->p >= L->end || (h = hexval(*L->p)) < 0)
			return -1;
		v = v * 16 + h;
		L->p++;
	}
	return v;
}

/* decode one C# escape sequence; L->p is at the backslash */
static void
decode_escape(CsLex *L, StringInfo buf)
{
	char		c;

	L->p++;						/* backslash */
	if (L->p >= L->end)
		lex_err(L, "unterminated string literal");
	c = *L->p++;
	switch (c)
	{
		case '\'': appendStringInfoChar(buf, '\''); return;
		case '"': appendStringInfoChar(buf, '"'); return;
		case '\\': appendStringInfoChar(buf, '\\'); return;
		case '0': append_codepoint(L, buf, 0); return;
		case 'a': appendStringInfoChar(buf, '\a'); return;
		case 'b': appendStringInfoChar(buf, '\b'); return;
		case 'e': appendStringInfoChar(buf, '\x1b'); return;
		case 'f': appendStringInfoChar(buf, '\f'); return;
		case 'n': appendStringInfoChar(buf, '\n'); return;
		case 'r': appendStringInfoChar(buf, '\r'); return;
		case 't': appendStringInfoChar(buf, '\t'); return;
		case 'v': appendStringInfoChar(buf, '\v'); return;
		case 'x':
			{
				long		v = 0;
				int			n = 0;

				while (n < 4 && L->p < L->end && hexval(*L->p) >= 0)
				{
					v = v * 16 + hexval(*L->p);
					L->p++;
					n++;
				}
				if (n == 0)
					lex_err(L, "invalid \\x escape: expected hex digits");
				append_codepoint(L, buf, (pg_wchar) v);
				return;
			}
		case 'u':
		case 'U':
			{
				long		v = read_hex(L, c == 'u' ? 4 : 8);

				if (v < 0)
					lex_err(L, c == 'u' ? "invalid \\u escape: expected 4 hex digits"
							: "invalid \\U escape: expected 8 hex digits");
				/* a UTF-16 surrogate pair written as two \u escapes */
				if (v >= 0xD800 && v <= 0xDBFF && L->end - L->p >= 6 &&
					L->p[0] == '\\' && L->p[1] == 'u')
				{
					const char *save = L->p;
					long		lo;

					L->p += 2;
					lo = read_hex(L, 4);
					if (lo >= 0xDC00 && lo <= 0xDFFF)
						v = 0x10000 + ((v - 0xD800) << 10) + (lo - 0xDC00);
					else
						L->p = save;
				}
				append_codepoint(L, buf, (pg_wchar) v);
				return;
			}
		default:
			lex_err(L, "unrecognized escape sequence in a string literal");
	}
}

static void cs_lex_run(CsLex *L, CsTokVec *v, int hole_braces, char *stop);

/*
 * Lex one interpolation hole; L->p is just past the opening brace(s). The
 * hole ends at nbraces closing braces at nesting depth 0.
 */
static void
lex_hole(CsLex *L, CsPart *part, int nbraces)
{
	CsTokVec	hv = {0};
	char		stop = 0;
	int			i;

	check_stack_depth();
	if (++L->depth > PLX_MAX_DEPTH)
		lex_err(L, "interpolated strings nested too deeply");
	part->hole = true;
	part->line = L->line;
	cs_lex_run(L, &hv, nbraces, &stop);
	if (stop == ',')
	{
		/* an alignment: rejected later with a clear message */
		part->align = true;
		L->p++;
		while (L->p < L->end && *L->p != '}' && *L->p != ':' && *L->p != '\n')
			L->p++;
		stop = (L->p < L->end) ? *L->p : 0;
	}
	if (stop == ':')
	{
		const char *fs;

		L->p++;
		fs = L->p;
		while (L->p < L->end && *L->p != '}' && *L->p != '\n' && *L->p != '"')
			L->p++;
		part->fmt = pnstrdup(fs, (int) (L->p - fs));
	}
	for (i = 0; i < nbraces; i++)
	{
		if (L->p >= L->end || *L->p != '}')
			lex_err(L, "unterminated interpolation hole: expected '}'");
		L->p++;
	}
	part->toks = hv.t;
	part->ntoks = hv.n;
	L->depth--;
}

typedef struct
{
	CsPart	   *p;
	int			n,
				cap;
} CsPartVec;

static CsPart *
pv_push(CsPartVec *v)
{
	if (v->n >= v->cap)
	{
		v->cap = v->cap ? v->cap * 2 : 8;
		v->p = v->p ? repalloc(v->p, sizeof(CsPart) * v->cap)
			: palloc(sizeof(CsPart) * v->cap);
	}
	memset(&v->p[v->n], 0, sizeof(CsPart));
	return &v->p[v->n++];
}

static void
flush_lit(CsPartVec *pv, StringInfo lit)
{
	CsPart	   *pt;

	if (lit->len == 0)
		return;
	pt = pv_push(pv);
	pt->lit = pnstrdup(lit->data, lit->len);
	pt->litlen = lit->len;
	resetStringInfo(lit);
}

/* finish an interpolated string token: one with no hole is a plain string */
static void
finish_istr(CsTok *t, CsPartVec *pv, StringInfo lit)
{
	int			i;
	bool		holes = false;

	flush_lit(pv, lit);
	for (i = 0; i < pv->n; i++)
		if (pv->p[i].hole)
			holes = true;
	if (!holes)
	{
		StringInfoData all;

		initStringInfo(&all);
		for (i = 0; i < pv->n; i++)
			appendBinaryStringInfo(&all, pv->p[i].lit, pv->p[i].litlen);
		t->kind = CT_STR;
		t->val = all.data;
		t->vlen = all.len;
		return;
	}
	t->kind = CT_ISTR;
	t->parts = pv->p;
	t->nparts = pv->n;
}

/* regular "..." or interpolated $"..." (escapes); L->p at the opening quote */
static void
lex_regular(CsLex *L, CsTok *t, bool interp)
{
	StringInfoData lit;
	CsPartVec	pv = {0};

	initStringInfo(&lit);
	L->p++;
	for (;;)
	{
		char		c;

		if (L->p >= L->end)
			lex_err(L, "unterminated string literal");
		c = *L->p;
		if (c == '"')
		{
			L->p++;
			break;
		}
		if (c == '\n')
			lex_err(L, "newline in a string literal (use a verbatim @\"...\" or raw \"\"\"...\"\"\" string for text over several lines)");
		if (c == '\\')
		{
			decode_escape(L, &lit);
			continue;
		}
		if (interp && c == '{')
		{
			if (L->p + 1 < L->end && L->p[1] == '{')
			{
				appendStringInfoChar(&lit, '{');
				L->p += 2;
				continue;
			}
			flush_lit(&pv, &lit);
			L->p++;
			lex_hole(L, pv_push(&pv), 1);
			continue;
		}
		if (interp && c == '}')
		{
			if (L->p + 1 < L->end && L->p[1] == '}')
			{
				appendStringInfoChar(&lit, '}');
				L->p += 2;
				continue;
			}
			lex_err(L, "a '}' in an interpolated string must be doubled ('}}')");
		}
		appendStringInfoChar(&lit, c);
		L->p++;
	}
	if (interp)
		finish_istr(t, &pv, &lit);
	else
	{
		t->kind = CT_STR;
		t->val = lit.data;
		t->vlen = lit.len;
	}
}

/* verbatim @"..." or $@"...": "" is a quote, backslashes are literal */
static void
lex_verbatim(CsLex *L, CsTok *t, bool interp)
{
	StringInfoData lit;
	CsPartVec	pv = {0};

	initStringInfo(&lit);
	L->p++;						/* the opening quote (the prefix is skipped) */
	for (;;)
	{
		char		c;

		if (L->p >= L->end)
			lex_err(L, "unterminated verbatim string literal");
		c = *L->p;
		if (c == '"')
		{
			if (L->p + 1 < L->end && L->p[1] == '"')
			{
				appendStringInfoChar(&lit, '"');
				L->p += 2;
				continue;
			}
			L->p++;
			break;
		}
		if (c == '\n')
			L->line++;
		if (interp && c == '{')
		{
			if (L->p + 1 < L->end && L->p[1] == '{')
			{
				appendStringInfoChar(&lit, '{');
				L->p += 2;
				continue;
			}
			flush_lit(&pv, &lit);
			L->p++;
			lex_hole(L, pv_push(&pv), 1);
			continue;
		}
		if (interp && c == '}')
		{
			if (L->p + 1 < L->end && L->p[1] == '}')
			{
				appendStringInfoChar(&lit, '}');
				L->p += 2;
				continue;
			}
			lex_err(L, "a '}' in an interpolated string must be doubled ('}}')");
		}
		appendStringInfoChar(&lit, c);
		L->p++;
	}
	if (interp)
		finish_istr(t, &pv, &lit);
	else
	{
		t->kind = CT_STR;
		t->val = lit.data;
		t->vlen = lit.len;
	}
}

/*
 * The content of a raw string, from L->p to e, with holes when nd > 0 (a
 * hole opens with nd braces). In the multi-line form each line is stripped
 * of the closing line's indentation, and lines are joined with '\n'.
 */
static void
raw_content(CsLex *L, CsPartVec *pv, StringInfo lit, int nd,
			const char *e, const char *indent, int indlen, bool multiline)
{
	bool		bol = multiline;

	while (L->p < e)
	{
		char		c;

		CHECK_FOR_INTERRUPTS();
		if (bol)
		{
			/* a whitespace-only line may be shorter than the indentation */
			const char *q = L->p;

			while (q < e && (*q == ' ' || *q == '\t'))
				q++;
			if (q >= e || *q == '\n' || *q == '\r')
				L->p = q;
			else
			{
				if (e - L->p < indlen || memcmp(L->p, indent, indlen) != 0)
					lex_err(L, "each line of a raw string literal must start with the closing line's indentation");
				L->p += indlen;
			}
			bol = false;
			continue;
		}
		c = *L->p;
		if (c == '\r' && L->p + 1 < e && L->p[1] == '\n')
		{
			L->p++;
			continue;
		}
		if (c == '\n')
		{
			L->line++;
			L->p++;
			/* the line break before the closing line is not content */
			if (L->p < e)
				appendStringInfoChar(lit, '\n');
			bol = true;
			continue;
		}
		if (nd > 0 && (c == '{' || c == '}'))
		{
			int			k = 0;

			while (L->p + k < e && L->p[k] == c)
				k++;
			if (k < nd)
			{
				appendBinaryStringInfo(lit, L->p, k);
				L->p += k;
				continue;
			}
			if (c == '}' || k >= 2 * nd)
				lex_err(L, "too many braces in a row in a raw interpolated string");
			appendBinaryStringInfo(lit, L->p, k - nd);
			L->p += k;
			flush_lit(pv, lit);
			lex_hole(L, pv_push(pv), nd);
			if (L->p > e)
				lex_err(L, "an interpolation hole runs past the end of its raw string");
			continue;
		}
		appendStringInfoChar(lit, c);
		L->p++;
	}
}

/* raw """...""" (nd = 0) or $"""...""" with nd dollar signs; L->p at the quotes */
static void
lex_raw(CsLex *L, CsTok *t, int nd)
{
	int			q = 0;
	const char *a;
	StringInfoData lit;
	CsPartVec	pv = {0};

	initStringInfo(&lit);
	while (L->p + q < L->end && L->p[q] == '"')
		q++;
	L->p += q;
	a = L->p;
	while (a < L->end && (*a == ' ' || *a == '\t' || *a == '\r'))
		a++;
	if (a < L->end && *a == '\n')
	{
		/* multi-line: the closing line is whitespace followed by q quotes */
		const char *ls = a + 1;
		const char *close = NULL;
		int			indlen = 0;

		while (ls < L->end)
		{
			const char *w = ls;
			int			k = 0;

			CHECK_FOR_INTERRUPTS();
			while (w < L->end && (*w == ' ' || *w == '\t'))
				w++;
			while (w + k < L->end && w[k] == '"')
				k++;
			if (k >= q)
			{
				if (k > q)
					lex_err(L, "a raw string literal's closing quotes must match its opening quotes");
				close = ls;
				indlen = (int) (w - ls);
				break;
			}
			while (ls < L->end && *ls != '\n')
				ls++;
			if (ls < L->end)
				ls++;
		}
		if (!close)
			lex_err(L, "unterminated raw string literal");
		L->line++;
		L->p = a + 1;
		raw_content(L, &pv, &lit, nd, close, close, indlen, true);
		L->p = close + indlen + q;
	}
	else
	{
		/* single line: the content runs up to q quotes on the same line */
		const char *e = L->p;

		while (e < L->end && *e != '\n')
		{
			int			k = 0;

			while (e + k < L->end && e[k] == '"')
				k++;
			if (k >= q)
				break;
			e += k ? k : 1;
		}
		if (e >= L->end || *e == '\n')
			lex_err(L, "a single-line raw string literal must close on the same line");
		raw_content(L, &pv, &lit, nd, e, NULL, 0, false);
		L->p = e + q;
	}
	if (nd > 0)
		finish_istr(t, &pv, &lit);
	else
	{
		t->kind = CT_STR;
		t->val = pnstrdup(lit.data, lit.len);
		t->vlen = lit.len;
	}
}

static void
lex_char(CsLex *L, CsTok *t)
{
	StringInfoData v;

	initStringInfo(&v);
	L->p++;
	if (L->p >= L->end || *L->p == '\'' || *L->p == '\n')
		lex_err(L, "empty or unterminated character literal");
	if (*L->p == '\\')
		decode_escape(L, &v);
	else
	{
		int			l = pg_mblen(L->p);

		if (l < 1 || L->end - L->p < l)
			lex_err(L, "invalid character literal");
		appendBinaryStringInfo(&v, L->p, l);
		L->p += l;
	}
	if (L->p >= L->end || *L->p != '\'')
		lex_err(L, "a character literal holds exactly one character (use \"...\" for a string)");
	L->p++;
	t->kind = CT_CHAR;
	t->val = v.data;
	t->vlen = v.len;
}

static void
lex_number(CsLex *L, CsTok *t)
{
	const char *p = L->p;
	StringInfoData d;
	bool		real = false;
	char		suf = 0;

	initStringInfo(&d);
	if (p[0] == '0' && p + 1 < L->end &&
		(p[1] == 'x' || p[1] == 'X' || p[1] == 'b' || p[1] == 'B'))
	{
		int			base = (p[1] == 'x' || p[1] == 'X') ? 16 : 2;
		uint64		v = 0;
		int			nd = 0;

		p += 2;
		for (; p < L->end; p++)
		{
			int			h;

			if (*p == '_')
				continue;
			h = hexval(*p);
			if (h < 0 || h >= base)
				break;
			if (v > (PG_UINT64_MAX - h) / base)
				lex_err(L, "integer literal is too large");
			v = v * base + h;
			nd++;
		}
		if (nd == 0)
			lex_err(L, "invalid numeric literal");
		appendStringInfo(&d, UINT64_FORMAT, v);
	}
	else
	{
		while (p < L->end && ((*p >= '0' && *p <= '9') || *p == '_'))
		{
			if (*p != '_')
				appendStringInfoChar(&d, *p);
			p++;
		}
		if (p + 1 < L->end && *p == '.' && p[1] >= '0' && p[1] <= '9')
		{
			real = true;
			if (d.len == 0)
				appendStringInfoChar(&d, '0');
			appendStringInfoChar(&d, '.');
			p++;
			while (p < L->end && ((*p >= '0' && *p <= '9') || *p == '_'))
			{
				if (*p != '_')
					appendStringInfoChar(&d, *p);
				p++;
			}
		}
		if (p < L->end && (*p == 'e' || *p == 'E'))
		{
			const char *q = p + 1;

			if (q < L->end && (*q == '+' || *q == '-'))
				q++;
			if (q < L->end && *q >= '0' && *q <= '9')
			{
				real = true;
				appendStringInfoChar(&d, 'e');
				p++;
				if (*p == '+' || *p == '-')
					appendStringInfoChar(&d, *p++);
				while (p < L->end && *p >= '0' && *p <= '9')
					appendStringInfoChar(&d, *p++);
			}
		}
	}
	if (p < L->end)
	{
		char		c = *p;

		if (c == 'u' || c == 'U')
		{
			p++;
			suf = 'U';
			if (p < L->end && (*p == 'l' || *p == 'L'))
			{
				p++;
				suf = 'W';
			}
		}
		else if (c == 'l' || c == 'L')
		{
			p++;
			suf = 'L';
			if (p < L->end && (*p == 'u' || *p == 'U'))
			{
				p++;
				suf = 'W';
			}
		}
		else if (c == 'f' || c == 'F' || c == 'd' || c == 'D' || c == 'm' || c == 'M')
		{
			p++;
			suf = (c == 'f' || c == 'F') ? 'F' : (c == 'd' || c == 'D') ? 'D' : 'M';
			real = true;
		}
	}
	if (p < L->end && (cs_ident_char(*p) || IS_HIGHBIT_SET(*p)))
		lex_err(L, "invalid numeric literal");
	if ((suf == 'U' || suf == 'L' || suf == 'W') && real)
		lex_err(L, "an integer suffix cannot follow a real literal");
	t->kind = CT_NUM;
	t->val = d.data;
	t->vlen = d.len;
	t->real = real;
	t->suffix = suf;
	L->p = p;
}

/* multi-character operators, longest first */
static const char *const cs_ops[] = {
	">>>=", "<<=", ">>=", "?" "?=", ">>>",
	"=>", "==", "!=", "<=", ">=", "&&", "||", "??", "?.", "++", "--",
	"+=", "-=", "*=", "/=", "%=", "&=", "|=", "^=", "->", "::", "<<", ">>",
	"..", NULL
};

/*
 * Lex tokens into v. At the top level (hole_braces == 0) lexing runs to the
 * end of input. In a hole it stops at a depth-0 '}' (the end of the hole),
 * ':' (a format specifier) or ',' (an alignment), leaving L->p on that
 * character and reporting it in *stop. A CT_EOF token always ends v.
 */
static void
cs_lex_run(CsLex *L, CsTokVec *v, int hole_braces, char *stop)
{
	int			depth = 0;
	CsTok	   *t;

	for (;;)
	{
		const char *p = L->p;
		char		c;
		int			i;

		CHECK_FOR_INTERRUPTS();
		if (p >= L->end)
		{
			if (hole_braces)
				lex_err(L, "unterminated interpolated string");
			break;
		}
		c = *p;
		if (c == '\n')
		{
			L->line++;
			L->p++;
			continue;
		}
		if (c == ' ' || c == '\t' || c == '\r' || c == '\f' || c == '\v')
		{
			L->p++;
			continue;
		}
		if (c == '/' && p + 1 < L->end && p[1] == '/')
		{
			while (L->p < L->end && *L->p != '\n')
				L->p++;
			continue;
		}
		if (c == '/' && p + 1 < L->end && p[1] == '*')
		{
			L->p += 2;
			for (;;)
			{
				if (L->end - L->p < 2)
					lex_err(L, "unterminated comment");
				if (L->p[0] == '*' && L->p[1] == '/')
				{
					L->p += 2;
					break;
				}
				if (*L->p == '\n')
					L->line++;
				L->p++;
			}
			continue;
		}
		if (hole_braces && depth == 0 &&
			(c == '}' || c == ',' || (c == ':' && !(p + 1 < L->end && p[1] == ':'))))
		{
			*stop = c;
			break;
		}
		if (c == '#')
			lex_err(L, "preprocessor directives and '#' are not supported");

		t = tv_push(v);
		t->line = L->line;
		t->s = p;

		/* strings */
		if (c == '@' && p + 1 < L->end && p[1] == '"')
		{
			L->p++;
			lex_verbatim(L, t, false);
			continue;
		}
		if (c == '@' && p + 2 < L->end && p[1] == '$' && p[2] == '"')
		{
			L->p += 2;
			lex_verbatim(L, t, true);
			continue;
		}
		if (c == '$')
		{
			const char *q = p;
			int			nd = 0;

			while (q < L->end && *q == '$')
			{
				nd++;
				q++;
			}
			if (nd == 1 && q + 1 < L->end && q[0] == '@' && q[1] == '"')
			{
				L->p = q + 1;
				lex_verbatim(L, t, true);
				continue;
			}
			if (L->end - q >= 3 && q[0] == '"' && q[1] == '"' && q[2] == '"')
			{
				L->p = q;
				lex_raw(L, t, nd);
				continue;
			}
			if (nd == 1 && q < L->end && *q == '"')
			{
				L->p = q;
				lex_regular(L, t, true);
				continue;
			}
			lex_err(L, "unexpected '$' (an interpolated string is written $\"...\")");
		}
		if (c == '"')
		{
			if (L->end - p >= 3 && p[1] == '"' && p[2] == '"')
				lex_raw(L, t, 0);
			else
				lex_regular(L, t, false);
			continue;
		}
		if (c == '\'')
		{
			lex_char(L, t);
			continue;
		}
		/* identifiers, and @verbatim identifiers */
		if (cs_ident_start(c) || (c == '@' && p + 1 < L->end && cs_ident_start(p[1])))
		{
			if (c == '@')
			{
				t->atid = true;
				p++;
			}
			t->s = p;
			while (p < L->end && cs_ident_char(*p))
				p++;
			if (p < L->end && IS_HIGHBIT_SET(*p))
				lex_err(L, "non-ASCII identifiers are not supported");
			t->kind = CT_ID;
			t->len = (int) (p - t->s);
			L->p = p;
			continue;
		}
		if ((c >= '0' && c <= '9') ||
			(c == '.' && p + 1 < L->end && p[1] >= '0' && p[1] <= '9'))
		{
			lex_number(L, t);
			t->len = (int) (L->p - t->s);
			continue;
		}
		if (IS_HIGHBIT_SET(c))
			lex_err(L, "non-ASCII characters are only allowed inside strings and comments");
		/* operators */
		t->kind = CT_OP;
		for (i = 0; cs_ops[i]; i++)
		{
			int			ol = (int) strlen(cs_ops[i]);

			if (L->end - p >= ol && memcmp(p, cs_ops[i], ol) == 0)
			{
				/* "?." before a digit is '?' and a real literal (a ? .5 : 1) */
				if (ol == 2 && p[0] == '?' && p[1] == '.' &&
					p + 2 < L->end && p[2] >= '0' && p[2] <= '9')
					continue;
				t->len = ol;
				break;
			}
		}
		if (!cs_ops[i])
		{
			if (!strchr("+-*/%&|^!~=<>?:.,;()[]{}", c))
			{
				if ((unsigned char) c < 0x20 || c == 0x7f)
					lex_err(L, "unexpected control character");
				lex_err(L, "unexpected character");
			}
			t->len = 1;
		}
		L->p += t->len;
		if (hole_braces)
		{
			if (c == '(' || c == '[' || c == '{')
				depth++;
			else if ((c == ')' || c == ']' || c == '}') && depth > 0)
				depth--;
		}
	}
	t = tv_push(v);
	t->kind = CT_EOF;
	t->line = L->line;
	t->s = L->p;
}

/* ======================================================================== */
/* Stage 1: static types and the symbol table                              */
/* ======================================================================== */

typedef enum
{
	TY_UNKNOWN, TY_NULL, TY_STRING, TY_CHAR, TY_BOOL, TY_INT, TY_NUM,
	TY_ARRAY, TY_RECORD, TY_OTHER
} CsTyK;

typedef struct
{
	CsTyK		k;
	const char *sql;			/* PostgreSQL type text, or NULL */
} CsTy;

static CsTy
mkty(CsTyK k, const char *sql)
{
	CsTy		t;

	t.k = k;
	t.sql = sql;
	return t;
}

#define TY_UNK		mkty(TY_UNKNOWN, NULL)
#define TY_TEXT		mkty(TY_STRING, "text")
#define TY_BOOLEAN	mkty(TY_BOOL, "boolean")
#define TY_INTEGER	mkty(TY_INT, "integer")

/* classify a PostgreSQL type name */
static CsTyK
sql_type_kind(const char *s)
{
	static const struct
	{
		const char *n;
		CsTyK		k;
	}			tab[] = {
		{"text", TY_STRING}, {"varchar", TY_STRING}, {"character varying", TY_STRING},
		{"char", TY_STRING}, {"character", TY_STRING}, {"bpchar", TY_STRING},
		{"name", TY_STRING}, {"citext", TY_STRING},
		{"boolean", TY_BOOL}, {"bool", TY_BOOL},
		{"integer", TY_INT}, {"int", TY_INT}, {"int4", TY_INT}, {"bigint", TY_INT},
		{"int8", TY_INT}, {"smallint", TY_INT}, {"int2", TY_INT},
		{"numeric", TY_NUM}, {"decimal", TY_NUM}, {"double precision", TY_NUM},
		{"float8", TY_NUM}, {"real", TY_NUM}, {"float4", TY_NUM}, {"float", TY_NUM},
		{"record", TY_RECORD},
	};
	int			len = (int) strlen(s);
	int			bl,
				i;

	if (len >= 2 && s[len - 2] == '[' && s[len - 1] == ']')
		return TY_ARRAY;
	for (bl = 0; bl < len && s[bl] != '('; bl++)
		;
	while (bl > 0 && s[bl - 1] == ' ')
		bl--;
	for (i = 0; i < (int) lengthof(tab); i++)
		if ((int) strlen(tab[i].n) == bl && pg_strncasecmp(tab[i].n, s, bl) == 0)
			return tab[i].k;
	return TY_OTHER;
}

static CsTy
ty_from_sql(const char *sql)
{
	return mkty(sql_type_kind(sql), sql);
}

/* element type of an array type */
static CsTy
ty_elem(CsTy a)
{
	int			len;

	if (a.k != TY_ARRAY || !a.sql)
		return TY_UNK;
	len = (int) strlen(a.sql);
	if (len < 3)
		return TY_UNK;
	return ty_from_sql(pnstrdup(a.sql, len - 2));
}

static CsTy
ty_from_oid(Oid t)
{
	Oid			b;

	if (!OidIsValid(t) || IsPolymorphicType(t))
		return TY_UNK;
	b = getBaseType(t);
	switch (b)
	{
		case TEXTOID:
		case VARCHAROID:
		case BPCHAROID:
		case NAMEOID:
			return mkty(TY_STRING, format_type_be(t));
		case BOOLOID:
			return TY_BOOLEAN;
		case INT2OID:
		case INT4OID:
		case INT8OID:
			return mkty(TY_INT, format_type_be(b));
		case NUMERICOID:
		case FLOAT4OID:
		case FLOAT8OID:
			return mkty(TY_NUM, format_type_be(b));
		case RECORDOID:
			return mkty(TY_RECORD, NULL);
		default:
			break;
	}
	if (OidIsValid(get_element_type(b)))
		return mkty(TY_ARRAY, format_type_be(b));
	if (get_typtype(b) == TYPTYPE_COMPOSITE)
		return mkty(TY_RECORD, format_type_be(t));
	return mkty(TY_OTHER, format_type_be(t));
}

static bool
ty_stringish(CsTy t)
{
	return t.k == TY_STRING || t.k == TY_CHAR;
}

static bool
ty_numeric(CsTy t)
{
	return t.k == TY_INT || t.k == TY_NUM;
}

/* result type of arithmetic on a and b */
static CsTy
ty_arith(CsTy a, CsTy b)
{
	if (a.k == TY_NUM || b.k == TY_NUM)
	{
		if ((a.sql && strcmp(a.sql, "double precision") == 0) ||
			(b.sql && strcmp(b.sql, "double precision") == 0))
			return mkty(TY_NUM, "double precision");
		if (a.k == TY_NUM && b.k == TY_NUM && a.sql && b.sql &&
			strcmp(a.sql, "real") == 0 && strcmp(b.sql, "real") == 0)
			return mkty(TY_NUM, "real");
		if ((a.k == TY_NUM || a.k == TY_INT) && (b.k == TY_NUM || b.k == TY_INT))
			return mkty(TY_NUM, "numeric");
		return TY_UNK;
	}
	if (a.k == TY_INT && b.k == TY_INT)
	{
		if ((a.sql && strcmp(a.sql, "bigint") == 0) || (b.sql && strcmp(b.sql, "bigint") == 0))
			return mkty(TY_INT, "bigint");
		return TY_INTEGER;
	}
	return TY_UNK;
}

/* type of a ternary/switch result from two arms */
static CsTy
ty_merge(CsTy a, CsTy b)
{
	if (a.k == TY_NULL)
		return b;
	if (b.k == TY_NULL)
		return a;
	if (ty_numeric(a) && ty_numeric(b))
		return (a.k == b.k && a.sql && b.sql && strcmp(a.sql, b.sql) == 0) ? a : ty_arith(a, b);
	if (ty_stringish(a) && ty_stringish(b))
		return (a.k == TY_CHAR && b.k == TY_CHAR) ? a : TY_TEXT;
	if (a.k == b.k)
		return a;
	return TY_UNK;
}

typedef struct CsSym
{
	char	   *name;			/* as written */
	CsTy		ty;
	bool		is_const;
	bool		is_sb;			/* a StringBuilder */
} CsSym;

typedef struct
{
	char		key[NAMEDATALEN];
	void	   *val;
} CsEnt;

static HTAB *
cs_hash_new(const char *name)
{
	HASHCTL		ctl;

	memset(&ctl, 0, sizeof(ctl));
	ctl.keysize = NAMEDATALEN;
	ctl.entrysize = sizeof(CsEnt);
	ctl.hcxt = CurrentMemoryContext;
#if PG_VERSION_NUM >= 140000
	return hash_create(name, 64, &ctl, HASH_ELEM | HASH_STRINGS | HASH_CONTEXT);
#else
	return hash_create(name, 64, &ctl, HASH_ELEM | HASH_CONTEXT);
#endif
}

static void
cs_key(const char *name, int len, char *key)
{
	char	   *k = downcase_truncate_identifier(name, len, false);

	memset(key, 0, NAMEDATALEN);
	strlcpy(key, k, NAMEDATALEN);
	pfree(k);
}

static void *
cs_hash_get(HTAB *h, const char *name, int len)
{
	char		key[NAMEDATALEN];
	CsEnt	   *e;

	cs_key(name, len, key);
	e = hash_search(h, key, HASH_FIND, NULL);
	return e ? e->val : NULL;
}

static void
cs_hash_put(HTAB *h, const char *name, int len, void *val)
{
	char		key[NAMEDATALEN];
	bool		found;
	CsEnt	   *e;

	cs_key(name, len, key);
	e = hash_search(h, key, HASH_ENTER, &found);
	e->val = val;
}

/* ======================================================================== */
/* Stage 1: parser state                                                    */
/* ======================================================================== */

typedef struct CsCatch
{
	const char *var;			/* exception variable, or NULL */
	int			len;
	struct CsCatch *outer;
} CsCatch;

typedef struct CsP
{
	Ctx		   *cx;
	CsTok	   *t;
	int			nt;
	int			pos;
	StringInfo	o;				/* current output buffer */
	int			out_line;		/* the line the engine lexer will be on */
	int			depth;
	HTAB	   *syms;			/* locals */
	HTAB	   *appended;		/* string locals that are '+=' targets */
	CsCatch    *catches;		/* enclosing catch blocks, innermost first */
	HTAB	   *asg;			/* variable -> number of its last assignment */
	int			nasg;			/* assignments so far */
	bool		allow_postinc;	/* parsing a statement's target */
	bool		retset;
	bool		trigger;
	bool		evtrigger;
} CsP;

static CsTok *
tok_at(CsP *P, int i)
{
	if (i >= P->nt)
		i = P->nt - 1;
	if (i < 0)
		i = 0;
	return &P->t[i];
}

#define cur(P)		tok_at((P), (P)->pos)
#define peek(P, k)	tok_at((P), (P)->pos + (k))

static void
advance(CsP *P)
{
	if (P->t[P->pos].kind != CT_EOF)
		P->pos++;
}

static bool
op_is(CsTok *t, const char *op)
{
	int			l = (int) strlen(op);

	return t->kind == CT_OP && t->len == l && memcmp(t->s, op, l) == 0;
}

static bool
name_is(CsTok *t, const char *w)
{
	int			l = (int) strlen(w);

	return t->kind == CT_ID && t->len == l && memcmp(t->s, w, l) == 0;
}

static bool
kw_is(CsTok *t, const char *w)
{
	return !t->atid && name_is(t, w);
}

/* a short description of a token, for messages */
static char *
tokdesc(CsTok *t)
{
	switch (t->kind)
	{
		case CT_EOF:
			return "the end of the body";
		case CT_STR:
		case CT_ISTR:
			return "a string";
		case CT_CHAR:
			return "a character literal";
		default:
			return psprintf("'%.*s'", Min(t->len, 40), t->s);
	}
}

pg_noreturn static void perr(CsP *P, const char *fmt,...) pg_attribute_printf(2, 3);

static void
perr(CsP *P, const char *fmt,...)
{
	char		buf[400];
	va_list		ap;

	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	plx_err(P->cx, cur(P)->line, "%s", buf);
}

static void
expect(CsP *P, const char *op)
{
	if (!op_is(cur(P), op))
		perr(P, "expected '%s' near %s", op, tokdesc(cur(P)));
	advance(P);
}

static void
enter(CsP *P)
{
	check_stack_depth();
	CHECK_FOR_INTERRUPTS();
	if (++P->depth > PLX_MAX_DEPTH)
		perr(P, "nested too deeply");
}

#define leave(P) ((P)->depth--)

static const char *const cs_reserved[] = {
	"abstract", "as", "base", "bool", "break", "byte", "case", "catch", "char",
	"checked", "class", "const", "continue", "decimal", "default", "delegate",
	"do", "double", "else", "enum", "event", "explicit", "extern", "false",
	"finally", "fixed", "float", "for", "foreach", "goto", "if", "implicit",
	"in", "int", "interface", "internal", "is", "lock", "long", "namespace",
	"new", "null", "object", "operator", "out", "override", "params", "private",
	"protected", "public", "readonly", "ref", "return", "sbyte", "sealed",
	"short", "sizeof", "stackalloc", "static", "string", "struct", "switch",
	"this", "throw", "true", "try", "typeof", "uint", "ulong", "unchecked",
	"unsafe", "ushort", "using", "virtual", "void", "volatile", "while", NULL
};

static bool
is_reserved(CsTok *t)
{
	int			i;

	if (t->kind != CT_ID || t->atid)
		return false;
	for (i = 0; cs_reserved[i]; i++)
		if (kw_is(t, cs_reserved[i]))
			return true;
	return false;
}

/* C# predefined type keywords */
static const char *const cs_type_kws[] = {
	"bool", "byte", "char", "decimal", "double", "float", "int", "long",
	"object", "sbyte", "short", "string", "uint", "ulong", "ushort", NULL
};

static bool
is_type_kw(CsTok *t)
{
	int			i;

	if (t->kind != CT_ID || t->atid)
		return false;
	for (i = 0; cs_type_kws[i]; i++)
		if (kw_is(t, cs_type_kws[i]))
			return true;
	return false;
}

/*
 * Words of the normalized program that stage 2 and the engine treat as
 * keywords; a name spelled like one (even as @name) would be misread there.
 */
static const char *const surf_words[] = {
	"if", "else", "while", "do", "for", "foreach", "in", "switch", "case",
	"default", "break", "continue", "return", "try", "catch", "finally",
	"throw", "let", "true", "false", "null", "emit", "return_next", NULL
};

static void
check_name(CsP *P, CsTok *t)
{
	int			i;

	for (i = 0; surf_words[i]; i++)
		if (name_is(t, surf_words[i]))
			perr(P, "'%s' cannot be used as a name in plxcsharp", surf_words[i]);
	if (t->len >= 5 && pg_strncasecmp(t->s, "__plx", 5) == 0)
		perr(P, "names starting with __plx are reserved for generated code");
}

/* a declared local's name must also work as a plpgsql variable */
static void
check_local_name(CsP *P, CsTok *t)
{
	static const char *const plpgsql_reserved[] = {
		"all", "begin", "by", "case", "declare", "else", "end", "execute", "for",
		"foreach", "from", "if", "in", "into", "loop", "not", "null", "or",
		"strict", "then", "to", "using", "when", "while", NULL
	};
	/* identifiers the shared expression rewriter maps to SQL keywords */
	static const char *const mapped[] = {
		"nil", "None", "undefined", "True", "False", "and", "or", "not", NULL
	};
	char	   *low;
	int			kw,
				i;

	check_name(P, t);
	for (i = 0; mapped[i]; i++)
		if (name_is(t, mapped[i]))
			perr(P, "'%s' cannot be used as a variable name in plxcsharp", mapped[i]);
	low = downcase_truncate_identifier(t->s, t->len, false);
	for (i = 0; plpgsql_reserved[i]; i++)
		if (strcmp(low, plpgsql_reserved[i]) == 0)
			perr(P, "'%.*s' is a reserved word in PL/pgSQL; choose another variable name",
				 t->len, t->s);
	/* a reserved or type/function-name keyword cannot be a column reference,
	 * so plpgsql cannot read it as a variable */
	kw = ScanKeywordLookup(low, &ScanKeywords);
	if (kw >= 0 && (ScanKeywordCategories[kw] == RESERVED_KEYWORD ||
					ScanKeywordCategories[kw] == TYPE_FUNC_NAME_KEYWORD))
		perr(P, "'%.*s' is a reserved word in PostgreSQL; choose another variable name",
			 t->len, t->s);
}

static CsSym *
sym_find(CsP *P, const char *name, int len)
{
	return P->syms ? (CsSym *) cs_hash_get(P->syms, name, len) : NULL;
}

static bool
ty_same(CsTy a, CsTy b)
{
	if (a.sql && b.sql)
		return pg_strcasecmp(a.sql, b.sql) == 0;
	return a.k == b.k && !a.sql && !b.sql;
}

static CsSym *
sym_declare(CsP *P, CsTok *nt, CsTy ty, bool is_const, bool is_sb)
{
	CsSym	   *s;

	if (!P->syms)
		P->syms = cs_hash_new("plxcsharp locals");
	s = sym_find(P, nt->s, nt->len);
	if (s)
	{
		if ((int) strlen(s->name) != nt->len || memcmp(s->name, nt->s, nt->len) != 0)
			perr(P, "'%.*s' and '%s' are the same name in PostgreSQL, which ignores case; rename one",
				 nt->len, nt->s, s->name);
		if (!ty_same(s->ty, ty) || s->is_sb != is_sb)
			perr(P, "'%s' is declared again with a different type (%s, earlier %s); locals are function-scoped in plxcsharp, so give it another name",
				 s->name, ty.sql ? ty.sql : "record", s->ty.sql ? s->ty.sql : "record");
		if (s->is_const || is_const)
			perr(P, "constant '%s' is declared twice", s->name);
		return s;
	}
	if (plx_is_param(P->cx, nt->s, nt->len))
		perr(P, "local '%.*s' has the name of a parameter (PostgreSQL ignores case in names)",
			 nt->len, nt->s);
	s = palloc0(sizeof(CsSym));
	s->name = pnstrdup(nt->s, nt->len);
	s->ty = ty;
	s->is_const = is_const;
	s->is_sb = is_sb;
	cs_hash_put(P->syms, nt->s, nt->len, s);
	return s;
}

static CsTy
param_ty(CsP *P, const char *name, int len)
{
	const PlxFuncMeta *m = P->cx->meta;
	int			i;

	if (!m)
		return TY_UNK;
	for (i = 0; i < m->nargs; i++)
	{
		const char *an = m->argnames ? m->argnames[i] : NULL;

		if (an && (int) strlen(an) == len && pg_strncasecmp(an, name, len) == 0)
			return m->argtypes ? ty_from_oid(m->argtypes[i]) : TY_UNK;
	}
	return TY_UNK;
}

/*
 * Record an assignment to a variable. Each name keeps the number of its last
 * assignment, so "assigned since the loop began" is one lookup.
 */
static void
note_asg(CsP *P, const char *name)
{
	if (!name)
		return;
	if (!P->asg)
		P->asg = cs_hash_new("plxcsharp assignments");
	P->nasg++;
	cs_hash_put(P->asg, name, (int) strlen(name), (void *) (intptr_t) P->nasg);
}

/* ======================================================================== */
/* Stage 1: expression trees                                                */
/* ======================================================================== */

/* SQL precedence of an emitted form (higher binds tighter) */
#define PR_OR		1
#define PR_AND		2
#define PR_NOT		3
#define PR_IS		4
#define PR_CMP		5
#define PR_OTHER	7
#define PR_ADD		8
#define PR_MUL		9
#define PR_UNARY	11
#define PR_CAST		12
#define PR_ATOM		13

typedef enum
{
	N_TEXT, N_IDENT, N_STR, N_FSTR, N_SEQ, N_CHAIN
} CsNk;

typedef struct CsNode CsNode;

typedef struct
{
	const char *text;			/* literal text; in a chain, the operator before the kid */
	CsNode	   *kid;
	int			minprec;		/* the kid is parenthesized below this precedence */
} CsItem;

struct CsNode
{
	CsNk		k;
	CsTy		ty;
	int			prec;
	const char *text;			/* N_TEXT; N_IDENT: the name */
	char	   *sval;			/* N_STR */
	int			slen;
	CsItem	   *items;			/* N_SEQ, N_FSTR (text: literal, kid: hole), N_CHAIN */
	int			nitems,
				icap;
	bool		nonassoc;		/* N_CHAIN of comparison operators */
	bool		sealed;			/* used as an operand: never extended in place */
	bool		has_call;		/* contains a call passed through to SQL */
	bool		builder;		/* N_IDENT naming a string builder (needs ::text) */
	bool		lvalue;			/* being emitted as an assignment target */
	bool		assignable;		/* variable, field, or array element */
	bool		is_null;		/* the null literal */
	bool		is_path;		/* name or name.field chain */
	bool		empty_coll;		/* [] or {} awaiting a declared array type */
	bool		intlit;			/* N_TEXT: a non-negative integer literal */
	const char *intrin;			/* fetch_one, query, ...: valid only on its own */
	const char *name;			/* base variable (for loop analysis) */
	int			line;
};

static CsNode *
nd_new(CsP *P, CsNk k, int prec, CsTy t)
{
	CsNode	   *n = palloc0(sizeof(CsNode));

	n->k = k;
	n->prec = prec;
	n->ty = t;
	n->line = cur(P)->line;
	return n;
}

static CsNode *
nd_text(CsP *P, const char *text, int prec, CsTy t)
{
	CsNode	   *n = nd_new(P, N_TEXT, prec, t);

	n->text = text;
	return n;
}

static CsNode *
nd_str(CsP *P, const char *s, int len, CsTyK k)
{
	CsNode	   *n = nd_new(P, N_STR, PR_ATOM, mkty(k, "text"));

	n->sval = pnstrdup(s, len);
	n->slen = len;
	return n;
}

static void
nd_push(CsNode *n, const char *text, CsNode *kid, int minprec)
{
	if (n->nitems >= n->icap)
	{
		n->icap = n->icap ? n->icap * 2 : 4;
		n->items = n->items ? repalloc(n->items, sizeof(CsItem) * n->icap)
			: palloc(sizeof(CsItem) * n->icap);
	}
	n->items[n->nitems].text = text;
	n->items[n->nitems].kid = kid;
	n->items[n->nitems].minprec = minprec;
	n->nitems++;
}

/* a node becomes an operand: intrinsics cannot be one */
static void
nd_use(CsP *P, CsNode *kid)
{
	if (kid->intrin)
		plx_err(P->cx, kid->line,
				"%s(...) must be used on its own, for example var r = %s(...);",
				kid->intrin, kid->intrin);
	if (kid->empty_coll)
		plx_err(P->cx, kid->line,
				"an empty collection needs a declared array type (int[] a = [];)");
	kid->sealed = true;
}

static void
seq_kid(CsP *P, CsNode *n, CsNode *kid, int minprec)
{
	nd_use(P, kid);
	nd_push(n, NULL, kid, minprec);
	if (kid->has_call)
		n->has_call = true;
}

/*
 * Build a node from a template. %N inserts the Nth argument in argument
 * position (no parentheses added), @N as an operand of a cast or postfix
 * (parenthesized unless an atom or cast), ^N as an operand of IS / a
 * comparison, !N as the operand of NOT.
 */
static CsNode *
tpl(CsP *P, int prec, CsTy ty, const char *fmt,...)
{
	CsNode	   *n = nd_new(P, N_SEQ, prec, ty);
	CsNode	   *kids[9];
	int			maxk = 0;
	const char *f;
	StringInfoData lit;
	va_list		ap;

	for (f = fmt; *f; f++)
		if ((*f == '%' || *f == '@' || *f == '^' || *f == '!') &&
			f[1] >= '1' && f[1] <= '9' && f[1] - '0' > maxk)
			maxk = f[1] - '0';
	va_start(ap, fmt);
	{
		int			i;

		for (i = 0; i < maxk; i++)
			kids[i] = va_arg(ap, CsNode *);
	}
	va_end(ap);

	initStringInfo(&lit);
	for (f = fmt; *f; f++)
	{
		if ((*f == '%' || *f == '@' || *f == '^' || *f == '!') &&
			f[1] >= '1' && f[1] <= '9')
		{
			int			mp = (*f == '%') ? 0 : (*f == '@') ? PR_CAST :
				(*f == '^') ? PR_CMP : PR_NOT;

			if (lit.len)
			{
				nd_push(n, pstrdup(lit.data), NULL, 0);
				resetStringInfo(&lit);
			}
			seq_kid(P, n, kids[f[1] - '1'], mp);
			f++;
			continue;
		}
		appendStringInfoChar(&lit, *f);
	}
	if (lit.len)
		nd_push(n, pstrdup(lit.data), NULL, 0);
	return n;
}

static CsNode *
paren(CsP *P, CsNode *e)
{
	CsNode	   *n = tpl(P, PR_ATOM, e->ty, "(%1)", e);

	n->name = e->name;
	return n;
}

/* ======================================================================== */
/* Stage 1: emitter                                                         */
/* ======================================================================== */

typedef struct
{
	CsP		   *P;
	StringInfo	out;
	int			q;				/* 0: top level; 1: inside an f-string hole */
	int			depth;
} CsEm;

static void em(CsEm *E, CsNode *n, int minprec);

/*
 * String content for a literal quoted with quote. Inside an f-string hole
 * (q >= 1) the enclosing hole is delimited by braces counted without regard
 * to quotes, so a brace in a nested string cannot be represented.
 */
static void
em_strbody(CsEm *E, CsNode *n, const char *s, int len, char quote, bool fbraces)
{
	int			i;

	for (i = 0; i < len; i++)
	{
		char		c = s[i];

		if (c == '\\')
			appendStringInfoString(E->out, "\\\\");
		else if (c == '"')
			appendStringInfoString(E->out, "\\\"");
		else if (c == '\'' && quote == '\'')
			appendStringInfoString(E->out, "\\'");
		else if (c == '{' || c == '}')
		{
			if (E->q >= 1)
				plx_err(E->P->cx, n->line,
						"a brace inside a string inside an interpolation hole is not supported; move the string to a variable");
			appendStringInfoChar(E->out, c);
			if (fbraces)
				appendStringInfoChar(E->out, c);
		}
		else
			appendStringInfoChar(E->out, c);
	}
}

static void
em(CsEm *E, CsNode *n, int minprec)
{
	bool		wrap = n->prec < minprec;
	int			i;

	check_stack_depth();
	if (++E->depth > PLX_MAX_DEPTH)
		plx_err(E->P->cx, n->line, "expression nested too deeply");
	if (wrap)
		appendStringInfoChar(E->out, '(');
	switch (n->k)
	{
		case N_TEXT:
			appendStringInfoString(E->out, n->text);
			break;
		case N_IDENT:
			if (n->builder && !n->lvalue)
				appendStringInfo(E->out, "(%s)::text", n->text);
			else
				appendStringInfoString(E->out, n->text);
			break;
		case N_STR:
			{
				char		qc = E->q ? '\'' : '"';

				if (E->q >= 2)
					plx_err(E->P->cx, n->line,
							"a string literal nested this deep inside interpolated strings is not supported; use a variable");
				appendStringInfoChar(E->out, qc);
				em_strbody(E, n, n->sval, n->slen, qc, false);
				appendStringInfoChar(E->out, qc);
				break;
			}
		case N_FSTR:
			{
				char		qc = E->q ? '\'' : '"';

				if (E->q >= 2)
					plx_err(E->P->cx, n->line,
							"interpolated strings nested more than two deep are not supported");
				appendStringInfoChar(E->out, 'f');
				appendStringInfoChar(E->out, qc);
				for (i = 0; i < n->nitems; i++)
				{
					if (n->items[i].kid)
					{
						appendStringInfoChar(E->out, '{');
						E->q++;
						em(E, n->items[i].kid, 0);
						E->q--;
						appendStringInfoChar(E->out, '}');
					}
					else
						em_strbody(E, n, n->items[i].text, (int) strlen(n->items[i].text),
								   qc, true);
				}
				appendStringInfoChar(E->out, qc);
				break;
			}
		case N_SEQ:
			for (i = 0; i < n->nitems; i++)
			{
				if (n->items[i].kid)
					em(E, n->items[i].kid, n->items[i].minprec);
				else
					appendStringInfoString(E->out, n->items[i].text);
			}
			break;
		case N_CHAIN:
			{
				int			p = n->prec;

				/* ((a = b) = c) = d: a non-associative chain nests to the left */
				if (n->nonassoc)
					for (i = 2; i < n->nitems; i++)
						appendStringInfoChar(E->out, '(');
				for (i = 0; i < n->nitems; i++)
				{
					if (i > 0)
						appendStringInfo(E->out, " %s ", n->items[i].text);
					em(E, n->items[i].kid, (i == 0 && !n->nonassoc) ? p : p + 1);
					if (n->nonassoc && i >= 1 && i < n->nitems - 1)
						appendStringInfoChar(E->out, ')');
				}
				break;
			}
	}
	if (wrap)
		appendStringInfoChar(E->out, ')');
	E->depth--;
}

static void
out_s(CsP *P, const char *s)
{
	appendStringInfoString(P->o, s);
}

static void
out_node(CsP *P, CsNode *n)
{
	CsEm		E;

	E.P = P;
	E.out = P->o;
	E.q = 0;
	E.depth = 0;
	em(&E, n, 0);
}

/* emit an assignment target (a builder variable without its ::text cast) */
static void
out_lvalue(CsP *P, CsNode *n)
{
	n->lvalue = true;
	out_node(P, n);
	n->lvalue = false;
}

static void
out_sync(CsP *P, int line)
{
	while (P->out_line < line)
	{
		appendStringInfoChar(P->o, '\n');
		P->out_line++;
	}
}

/* ======================================================================== */
/* Stage 1: binary operators                                                */
/* ======================================================================== */

static CsNode *
chain_new(CsP *P, int prec, bool nonassoc, CsNode *first, CsTy t)
{
	CsNode	   *n = nd_new(P, N_CHAIN, prec, t);

	n->nonassoc = nonassoc;
	nd_use(P, first);
	nd_push(n, NULL, first, 0);
	n->has_call = first->has_call;
	n->line = first->line;
	return n;
}

static void
chain_add(CsP *P, CsNode *c, const char *op, CsNode *kid)
{
	nd_use(P, kid);
	nd_push(c, op, kid, 0);
	if (kid->has_call)
		c->has_call = true;
}

/*
 * Combine left OP right, where OP is a C# binary operator. Operand types pick
 * the SQL operator: '+' on a string is '||', '&' on booleans is AND, and so
 * on. A chain of the same SQL precedence is extended in place, so a long
 * a + b + c + ... stays one flat node (linear to build and to emit).
 */
static CsNode *
bin_step(CsP *P, CsNode *left, const char *op, CsNode *right)
{
	const char *sqlop;
	int			prec;
	bool		nonassoc = false;
	CsTy		rt;
	CsTy		lt = left->ty,
				ty2 = right->ty;

	if (strcmp(op, "+") == 0)
	{
		if (ty_stringish(lt) || ty_stringish(ty2))
		{
			if ((lt.k == TY_CHAR && (ty2.k == TY_CHAR || ty_numeric(ty2))) ||
				(ty2.k == TY_CHAR && ty_numeric(lt)))
				perr(P, "arithmetic on char values is not supported (C# would add character codes); convert with (int)c or concatenate strings");
			sqlop = "||";
			prec = PR_OTHER;
			rt = TY_TEXT;
		}
		else
		{
			sqlop = "+";
			prec = PR_ADD;
			rt = ty_arith(lt, ty2);
		}
	}
	else if (strcmp(op, "-") == 0 || strcmp(op, "*") == 0 ||
			 strcmp(op, "/") == 0 || strcmp(op, "%") == 0)
	{
		if (ty_stringish(lt) || ty_stringish(ty2))
			perr(P, "'%s' cannot be applied to a string", op);
		sqlop = op;
		prec = (op[0] == '-') ? PR_ADD : PR_MUL;
		rt = ty_arith(lt, ty2);
	}
	else if (strcmp(op, "<<") == 0 || strcmp(op, ">>") == 0)
	{
		sqlop = op;
		prec = PR_OTHER;
		rt = (lt.k == TY_INT) ? lt : TY_UNK;
	}
	else if (strcmp(op, "<") == 0 || strcmp(op, ">") == 0 ||
			 strcmp(op, "<=") == 0 || strcmp(op, ">=") == 0)
	{
		sqlop = op;
		prec = PR_CMP;
		nonassoc = true;
		rt = TY_BOOLEAN;
	}
	else if (strcmp(op, "==") == 0 || strcmp(op, "!=") == 0)
	{
		bool		eq = (op[0] == '=');

		if (right->is_null || left->is_null)
		{
			CsNode	   *x = right->is_null ? left : right;

			if (x->is_null)
				return nd_text(P, eq ? "true" : "false", PR_ATOM, TY_BOOLEAN);
			return tpl(P, PR_IS, TY_BOOLEAN, eq ? "^1 IS NULL" : "^1 IS NOT NULL", x);
		}
		sqlop = eq ? "=" : "<>";
		prec = PR_CMP;
		nonassoc = true;
		rt = TY_BOOLEAN;
	}
	else if (strcmp(op, "&") == 0 || strcmp(op, "|") == 0 || strcmp(op, "^") == 0)
	{
		if (lt.k == TY_BOOL && ty2.k == TY_BOOL)
		{
			sqlop = (op[0] == '&') ? "AND" : (op[0] == '|') ? "OR" : "<>";
			prec = (op[0] == '&') ? PR_AND : (op[0] == '|') ? PR_OR : PR_CMP;
			nonassoc = (op[0] == '^');
			rt = TY_BOOLEAN;
		}
		else
		{
			sqlop = (op[0] == '^') ? "#" : op;
			prec = PR_OTHER;
			rt = (lt.k == TY_INT) ? lt : TY_UNK;
		}
	}
	else if (strcmp(op, "&&") == 0)
	{
		sqlop = "AND";
		prec = PR_AND;
		rt = TY_BOOLEAN;
	}
	else if (strcmp(op, "||") == 0)
	{
		sqlop = "OR";
		prec = PR_OR;
		rt = TY_BOOLEAN;
	}
	else
		perr(P, "the operator '%s' is not supported", op);

	if (left->k == N_CHAIN && !left->sealed && left->prec == prec &&
		left->nonassoc == nonassoc)
	{
		chain_add(P, left, sqlop, right);
		left->ty = rt;
		return left;
	}
	left = chain_new(P, prec, nonassoc, left, rt);
	chain_add(P, left, sqlop, right);
	return left;
}

/* idx + 1 (C# arrays and strings are 0-based, PostgreSQL's are 1-based) */
static CsNode *
plus_one(CsP *P, CsNode *idx)
{
	if (idx->k == N_TEXT && idx->intlit)
	{
		long		v = strtol(idx->text, NULL, 10);

		if (v >= 0 && v < 1000000000L)
		{
			CsNode	   *n = nd_text(P, psprintf("%ld", v + 1), PR_ATOM, TY_INTEGER);

			n->intlit = true;
			return n;
		}
	}
	return bin_step(P, idx, "+", nd_text(P, "1", PR_ATOM, TY_INTEGER));
}

/* ======================================================================== */
/* Stage 1: expressions                                                     */
/* ======================================================================== */

static CsNode *parse_expr(CsP *P);
static CsNode *parse_unary(CsP *P);
static CsNode *parse_level(CsP *P, int lvl);

typedef struct
{
	CsNode	  **a;
	CsTok	  **names;			/* named-argument names (NULL when positional) */
	int			n,
				cap;
} CsArgs;

static void
parse_args(CsP *P, CsArgs *args)
{
	memset(args, 0, sizeof(*args));
	expect(P, "(");
	if (op_is(cur(P), ")"))
	{
		advance(P);
		return;
	}
	for (;;)
	{
		CsTok	   *nm = NULL;
		CsNode	   *e;

		CHECK_FOR_INTERRUPTS();
		if (cur(P)->kind == CT_ID && op_is(peek(P, 1), ":"))
		{
			nm = cur(P);
			advance(P);
			advance(P);
		}
		if (kw_is(cur(P), "ref") || kw_is(cur(P), "out") || kw_is(cur(P), "in"))
			perr(P, "ref, out and in arguments are not supported");
		e = parse_expr(P);
		if (args->n >= args->cap)
		{
			args->cap = args->cap ? args->cap * 2 : 4;
			args->a = args->a ? repalloc(args->a, sizeof(CsNode *) * args->cap)
				: palloc(sizeof(CsNode *) * args->cap);
			args->names = args->names ? repalloc(args->names, sizeof(CsTok *) * args->cap)
				: palloc(sizeof(CsTok *) * args->cap);
		}
		args->a[args->n] = e;
		args->names[args->n] = nm;
		args->n++;
		if (op_is(cur(P), ","))
		{
			advance(P);
			continue;
		}
		break;
	}
	expect(P, ")");
}

static void
args_positional(CsP *P, CsArgs *a, const char *what)
{
	int			i;

	for (i = 0; i < a->n; i++)
		if (a->names[i])
			perr(P, "named arguments are not supported for %s", what);
}

/* name(arg, ...) passed through to SQL; named arguments use SQL's name => value */
static CsNode *
call_node(CsP *P, const char *fname, CsArgs *a, CsTy ty)
{
	CsNode	   *n = nd_new(P, N_SEQ, PR_ATOM, ty);
	int			i;

	nd_push(n, psprintf("%s(", fname), NULL, 0);
	for (i = 0; i < a->n; i++)
	{
		if (i > 0)
			nd_push(n, ", ", NULL, 0);
		if (a->names[i])
			nd_push(n, psprintf("%.*s => ", a->names[i]->len, a->names[i]->s), NULL, 0);
		seq_kid(P, n, a->a[i], 0);
	}
	nd_push(n, ")", NULL, 0);
	return n;
}

static CsNode *
num_node(CsP *P, CsTok *t)
{
	const char *v = t->val;
	CsNode	   *n;

	switch (t->suffix)
	{
		case 'L':
		case 'U':
			return nd_text(P, psprintf("%s::bigint", v), PR_CAST, mkty(TY_INT, "bigint"));
		case 'W':
			return nd_text(P, psprintf("%s::numeric", v), PR_CAST, mkty(TY_NUM, "numeric"));
		case 'F':
			return nd_text(P, psprintf("%s::real", v), PR_CAST, mkty(TY_NUM, "real"));
		case 'D':
			return nd_text(P, psprintf("%s::double precision", v), PR_CAST,
						   mkty(TY_NUM, "double precision"));
		case 'M':
			return nd_text(P, v, PR_ATOM, mkty(TY_NUM, "numeric"));
		default:
			break;
	}
	if (t->real)
		return nd_text(P, v, PR_ATOM, mkty(TY_NUM, "numeric"));
	if (t->vlen < 10 || (t->vlen == 10 && strcmp(v, "2147483647") <= 0))
		n = nd_text(P, v, PR_ATOM, TY_INTEGER);
	else if (t->vlen < 19 || (t->vlen == 19 && strcmp(v, "9223372036854775807") <= 0))
		n = nd_text(P, v, PR_ATOM, mkty(TY_INT, "bigint"));
	else
		n = nd_text(P, v, PR_ATOM, mkty(TY_NUM, "numeric"));
	n->intlit = true;
	return n;
}

/* {x:F2} -> round(x, 2); other format specifiers are refused */
static CsNode *
map_format(CsP *P, CsNode *e, const char *fmt, int line)
{
	const char *f = fmt;
	int			n = 2;

	while (*f == ' ')
		f++;
	if (*f == 'F' || *f == 'f')
	{
		const char *d = f + 1;
		int			dl = 0;
		const char *r;

		while (d[dl] >= '0' && d[dl] <= '9')
			dl++;
		r = d + dl;
		while (*r == ' ')
			r++;
		if (*r == '\0' && dl <= 2)
		{
			if (dl)
				n = atoi(d);
			return tpl(P, PR_ATOM, mkty(TY_NUM, "numeric"),
					   psprintf("round((%%1)::numeric, %d)", n), e);
		}
	}
	plx_err(P->cx, line,
			"the format specifier ':%s' is not supported; format the value with to_char() inside the braces",
			fmt);
}

static CsNode *
parse_fstr(CsP *P, CsTok *t)
{
	CsNode	   *n = nd_new(P, N_FSTR, PR_ATOM, TY_TEXT);
	int			i;

	for (i = 0; i < t->nparts; i++)
	{
		CsPart	   *pt = &t->parts[i];
		CsTok	   *st = P->t;
		int			sn = P->nt,
					sp = P->pos;
		CsNode	   *e;

		if (!pt->hole)
		{
			nd_push(n, pt->lit, NULL, 0);
			continue;
		}
		if (pt->align)
			plx_err(P->cx, pt->line,
					"alignment in an interpolation hole ({x,10}) is not supported; use lpad() or rpad()");
		if (pt->ntoks <= 1)
			plx_err(P->cx, pt->line, "empty interpolation hole");
		P->t = pt->toks;
		P->nt = pt->ntoks;
		P->pos = 0;
		e = parse_expr(P);
		if (cur(P)->kind != CT_EOF)
			perr(P, "unexpected %s in an interpolation hole", tokdesc(cur(P)));
		P->t = st;
		P->nt = sn;
		P->pos = sp;
		if (pt->fmt)
			e = map_format(P, e, pt->fmt, pt->line);
		seq_kid(P, n, e, 0);
		if (e->has_call)
			n->has_call = true;
	}
	return n;
}

/* string.Format / Console.WriteLine composite format -> SQL format() */
static CsNode *
format_node(CsP *P, CsNode *fmt, CsNode **a, int na)
{
	StringInfoData f;
	CsNode	   *n;
	int			i;

	if (fmt->k != N_STR)
		perr(P, "a composite format needs a literal format string; use an interpolated string $\"...\" instead");
	initStringInfo(&f);
	for (i = 0; i < fmt->slen; i++)
	{
		char		c = fmt->sval[i];

		if (c == '{' && i + 1 < fmt->slen && fmt->sval[i + 1] == '{')
		{
			appendStringInfoChar(&f, '{');
			i++;
		}
		else if (c == '}' && i + 1 < fmt->slen && fmt->sval[i + 1] == '}')
		{
			appendStringInfoChar(&f, '}');
			i++;
		}
		else if (c == '{')
		{
			int			idx = 0,
						nd = 0,
						width = 0,
						wd = 0;
			bool		left = false;

			i++;
			while (i < fmt->slen && fmt->sval[i] >= '0' && fmt->sval[i] <= '9' && nd < 4)
			{
				idx = idx * 10 + (fmt->sval[i] - '0');
				i++;
				nd++;
			}
			if (nd == 0)
				perr(P, "invalid format item in a composite format string");
			if (i < fmt->slen && fmt->sval[i] == ',')
			{
				i++;
				if (i < fmt->slen && fmt->sval[i] == '-')
				{
					left = true;
					i++;
				}
				while (i < fmt->slen && fmt->sval[i] >= '0' && fmt->sval[i] <= '9' && wd < 4)
				{
					width = width * 10 + (fmt->sval[i] - '0');
					i++;
					wd++;
				}
			}
			if (i < fmt->slen && fmt->sval[i] == ':')
				perr(P, "format specifiers in a composite format ({0:...}) are not supported; use to_char()");
			if (i >= fmt->slen || fmt->sval[i] != '}')
				perr(P, "invalid format item in a composite format string");
			if (idx >= na)
				perr(P, "format item {%d} has no matching argument", idx);
			if (wd)
				appendStringInfo(&f, "%%%d$%s%ds", idx + 1, left ? "-" : "", width);
			else
				appendStringInfo(&f, "%%%d$s", idx + 1);
		}
		else if (c == '}')
			perr(P, "a '}' in a composite format string must be doubled ('}}')");
		else if (c == '%')
			appendStringInfoString(&f, "%%");
		else
			appendStringInfoChar(&f, c);
	}
	n = nd_new(P, N_SEQ, PR_ATOM, TY_TEXT);
	nd_push(n, "format(", NULL, 0);
	seq_kid(P, n, nd_str(P, f.data, f.len, TY_STRING), 0);
	for (i = 0; i < na; i++)
	{
		nd_push(n, ", ", NULL, 0);
		seq_kid(P, n, a[i], 0);
	}
	nd_push(n, ")", NULL, 0);
	return n;
}

/* result type of a few common SQL functions (for '+' and var inference) */
static CsTy
sqlfunc_ty(const char *s, int len)
{
	static const char *const strf[] = {
		"upper", "lower", "btrim", "ltrim", "rtrim", "concat", "concat_ws",
		"format", "substr", "substring", "replace", "lpad", "rpad", "to_char",
		"repeat", "reverse", "initcap", "left", "right", "md5", "quote_ident",
		"quote_literal", "quote_nullable", "chr", "translate", "overlay",
		"split_part", "array_to_string", NULL
	};
	static const char *const intf[] = {
		"length", "char_length", "octet_length", "strpos", "cardinality",
		"array_length", "ascii", NULL
	};
	char	   *low = downcase_truncate_identifier(s, len, false);
	int			i;

	for (i = 0; strf[i]; i++)
		if (strcmp(low, strf[i]) == 0)
			return TY_TEXT;
	for (i = 0; intf[i]; i++)
		if (strcmp(low, intf[i]) == 0)
			return TY_INTEGER;
	if (strcmp(low, "starts_with") == 0)
		return TY_BOOLEAN;
	return TY_UNK;
}

/* the SQL type a C# type name maps to (NULL when it is not one) */
typedef struct
{
	const char *cs;
	const char *sql;
	CsTyK		k;
} CsTypeMap;

static const CsTypeMap cs_typemap[] = {
	{"int", "integer", TY_INT}, {"Int32", "integer", TY_INT},
	{"long", "bigint", TY_INT}, {"Int64", "bigint", TY_INT},
	{"short", "smallint", TY_INT}, {"Int16", "smallint", TY_INT},
	{"byte", "smallint", TY_INT}, {"Byte", "smallint", TY_INT},
	{"sbyte", "smallint", TY_INT}, {"SByte", "smallint", TY_INT},
	{"ushort", "integer", TY_INT}, {"UInt16", "integer", TY_INT},
	{"uint", "bigint", TY_INT}, {"UInt32", "bigint", TY_INT},
	{"ulong", "numeric", TY_NUM}, {"UInt64", "numeric", TY_NUM},
	{"bool", "boolean", TY_BOOL}, {"Boolean", "boolean", TY_BOOL},
	{"string", "text", TY_STRING}, {"String", "text", TY_STRING},
	{"char", "text", TY_CHAR}, {"Char", "text", TY_CHAR},
	{"double", "double precision", TY_NUM}, {"Double", "double precision", TY_NUM},
	{"float", "real", TY_NUM}, {"Single", "real", TY_NUM},
	{"decimal", "numeric", TY_NUM}, {"Decimal", "numeric", TY_NUM},
	{"DateTime", "timestamp", TY_OTHER}, {"DateTimeOffset", "timestamptz", TY_OTHER},
	{"DateOnly", "date", TY_OTHER}, {"TimeOnly", "time", TY_OTHER},
	{"TimeSpan", "interval", TY_OTHER}, {"Guid", "uuid", TY_OTHER},
	{NULL, NULL, TY_UNKNOWN}
};

static const CsTypeMap *
cs_type_lookup(const char *s, int len)
{
	int			i;

	if (len > 7 && strncmp(s, "System.", 7) == 0)
	{
		s += 7;
		len -= 7;
	}
	for (i = 0; cs_typemap[i].cs; i++)
		if ((int) strlen(cs_typemap[i].cs) == len && memcmp(cs_typemap[i].cs, s, len) == 0)
			return &cs_typemap[i];
	return NULL;
}

/* a parsed type */
typedef struct
{
	CsTy		ty;
	bool		is_var;
	bool		is_sb;			/* StringBuilder */
	bool		mapped;			/* a C# type with a PostgreSQL mapping */
} CsType;

/*
 * The index just past a type that starts at token i, or -1. A type is a C#
 * type keyword, or a (dotted) name with an optional (n[, m]) modifier,
 * followed by an optional '?' and array brackets.
 */
static int
type_end(CsP *P, int i)
{
	CsTok	   *t = tok_at(P, i);

	if (t->kind != CT_ID || (is_reserved(t) && !is_type_kw(t)))
		return -1;
	i++;
	if (!is_type_kw(t))
	{
		while (op_is(tok_at(P, i), ".") && tok_at(P, i + 1)->kind == CT_ID)
			i += 2;
		if (op_is(tok_at(P, i), "(") && tok_at(P, i + 1)->kind == CT_NUM)
		{
			int			j = i + 1;

			for (;;)
			{
				if (tok_at(P, j)->kind != CT_NUM)
					return -1;
				j++;
				if (op_is(tok_at(P, j), ")"))
					break;
				if (!op_is(tok_at(P, j), ","))
					return -1;
				j++;
			}
			i = j + 1;
		}
	}
	if (op_is(tok_at(P, i), "?"))
		i++;
	while (op_is(tok_at(P, i), "[") && op_is(tok_at(P, i + 1), "]"))
		i += 2;
	if (op_is(tok_at(P, i), "?"))
		i++;
	return i;
}

static void
parse_type(CsP *P, CsType *ti)
{
	CsTok	   *t = cur(P);
	StringInfoData name;
	const CsTypeMap *m;
	int			arrays = 0;

	memset(ti, 0, sizeof(*ti));
	if (kw_is(t, "var"))
	{
		ti->is_var = true;
		ti->ty = TY_UNK;
		advance(P);
		return;
	}
	if (t->kind != CT_ID || (is_reserved(t) && !is_type_kw(t)))
		perr(P, "expected a type near %s", tokdesc(t));
	if (kw_is(t, "object") || name_is(t, "dynamic"))
		perr(P, "'%.*s' has no PostgreSQL equivalent; declare a specific type", t->len, t->s);
	initStringInfo(&name);
	appendBinaryStringInfo(&name, t->s, t->len);
	advance(P);
	if (!is_type_kw(t))
	{
		while (op_is(cur(P), ".") && peek(P, 1)->kind == CT_ID)
		{
			appendStringInfoChar(&name, '.');
			appendBinaryStringInfo(&name, peek(P, 1)->s, peek(P, 1)->len);
			advance(P);
			advance(P);
		}
		if (op_is(cur(P), "<"))
			perr(P, "generic types are not supported");
	}
	m = cs_type_lookup(name.data, name.len);
	if (m)
	{
		ti->ty = mkty(m->k, m->sql);
		ti->mapped = true;
	}
	else if (strcmp(name.data, "StringBuilder") == 0 ||
			 strcmp(name.data, "System.Text.StringBuilder") == 0 ||
			 strcmp(name.data, "Text.StringBuilder") == 0)
	{
		ti->is_sb = true;
		ti->ty = TY_TEXT;
	}
	else
	{
		/* not a C# type: a PostgreSQL type name, used as written */
		if (op_is(cur(P), "("))
		{
			appendStringInfoChar(&name, '(');
			advance(P);
			for (;;)
			{
				if (cur(P)->kind != CT_NUM || cur(P)->real || cur(P)->suffix)
					perr(P, "a type modifier must be an integer");
				appendStringInfoString(&name, cur(P)->val);
				advance(P);
				if (op_is(cur(P), ")"))
					break;
				expect(P, ",");
				appendStringInfoChar(&name, ',');
			}
			advance(P);
			appendStringInfoChar(&name, ')');
		}
		ti->ty = ty_from_sql(name.data);
	}
	if (op_is(cur(P), "?"))
		advance(P);
	while (op_is(cur(P), "["))
	{
		if (op_is(peek(P, 1), ","))
			perr(P, "multi-dimensional arrays are not supported");
		if (!op_is(peek(P, 1), "]"))
			perr(P, "an array type is written T[]");
		advance(P);
		advance(P);
		arrays++;
	}
	if (op_is(cur(P), "?"))
		advance(P);
	if (arrays > 1)
		perr(P, "jagged arrays (T[][]) are not supported");
	if (arrays == 1)
	{
		if (ti->is_sb)
			perr(P, "arrays of StringBuilder are not supported");
		if (m && strcmp(m->cs, "byte") == 0)
			ti->ty = mkty(TY_OTHER, "bytea");
		else
			ti->ty = mkty(TY_ARRAY, psprintf("%s[]", ti->ty.sql));
	}
}

/* (T)x */
static CsNode *
cast_node(CsP *P, CsType *ti, CsNode *e)
{
	const char *sql = ti->ty.sql;

	if (ti->is_sb)
		perr(P, "a cast to StringBuilder is not supported");
	if (ti->ty.k == TY_INT)
	{
		if (e->ty.k == TY_CHAR)
			return tpl(P, PR_CAST, ti->ty, psprintf("(ascii(%%1))::%s", sql), e);
		if (e->ty.k == TY_STRING)
			perr(P, "a string cannot be cast to a number; use int.Parse(s)");
		if (e->ty.k == TY_INT)
			return tpl(P, PR_CAST, ti->ty, psprintf("(%%1)::%s", sql), e);
		/* C# truncates a real toward zero; a SQL cast rounds */
		return tpl(P, PR_CAST, ti->ty, psprintf("(trunc(%%1))::%s", sql), e);
	}
	if (ti->ty.k == TY_CHAR)
	{
		if (ty_numeric(e->ty))
			return tpl(P, PR_ATOM, ti->ty, "chr(%1)", e);
		return tpl(P, PR_CAST, ti->ty, "(%1)::text", e);
	}
	return tpl(P, PR_CAST, ti->ty, psprintf("(%%1)::%s", sql), e);
}

/* r.name (field access) */
static CsNode *
field_node(CsP *P, CsNode *recv, const char *name, int len)
{
	CsNode	   *n = nd_new(P, N_SEQ, PR_ATOM, TY_UNK);

	if (recv->is_path)
		seq_kid(P, n, recv, 0);
	else
	{
		nd_push(n, "(", NULL, 0);
		seq_kid(P, n, recv, 0);
		nd_push(n, ")", NULL, 0);
	}
	nd_push(n, psprintf(".%.*s", len, name), NULL, 0);
	n->assignable = recv->assignable || recv->is_path;
	n->is_path = recv->is_path;
	n->name = recv->name;
	return n;
}

static bool
all_ident_chars(const char *s, int len)
{
	int			i;

	if (len == 0 || !cs_ident_start(s[0]))
		return false;
	for (i = 1; i < len; i++)
		if (!cs_ident_char(s[i]))
			return false;
	return true;
}

/* recv[idx] */
static CsNode *
elem_node(CsP *P, CsNode *recv, CsNode *idx)
{
	CsNode	   *n;

	if (idx->k == N_STR)
	{
		/* row["col"]: a field of a record */
		if (!all_ident_chars(idx->sval, idx->slen))
			perr(P, "a field name in r[\"...\"] must be a plain identifier");
		if (ty_stringish(recv->ty) || recv->ty.k == TY_ARRAY)
			perr(P, "a string index needs a record");
		return field_node(P, recv, idx->sval, idx->slen);
	}
	if (ty_stringish(recv->ty))
		return tpl(P, PR_ATOM, mkty(TY_CHAR, "text"), "substr(%1, %2, 1)", recv, plus_one(P, idx));
	n = nd_new(P, N_SEQ, PR_ATOM,
			   recv->ty.k == TY_ARRAY ? ty_elem(recv->ty) : TY_UNK);
	if (recv->is_path)
		seq_kid(P, n, recv, 0);
	else
	{
		nd_push(n, "(", NULL, 0);
		seq_kid(P, n, recv, 0);
		nd_push(n, ")", NULL, 0);
	}
	nd_push(n, "[", NULL, 0);
	/* TG_ARGV is already 0-based in plpgsql */
	if (recv->k == N_IDENT && pg_strcasecmp(recv->text, "TG_ARGV") == 0)
		seq_kid(P, n, idx, 0);
	else
		seq_kid(P, n, plus_one(P, idx), 0);
	nd_push(n, "]", NULL, 0);
	n->assignable = true;
	n->name = recv->name;
	return n;
}

static bool
mname_is(CsTok *m, const char *w)
{
	return name_is(m, w);
}

static void
argc_is(CsP *P, CsTok *m, CsArgs *a, int lo, int hi)
{
	if (a->n < lo || a->n > hi)
		plx_err(P->cx, m->line, "wrong number of arguments for %.*s", m->len, m->s);
	args_positional(P, a, psprintf("%.*s", m->len, m->s));
}

/* recv.Member / recv.Method(args) on an expression */
static CsNode *
inst_member(CsP *P, CsNode *recv, CsTok *m, bool call, CsArgs *a)
{
	CsTyK		rk = recv->ty.k;
	bool		strlike = (rk == TY_STRING || rk == TY_CHAR || rk == TY_UNKNOWN ||
						   rk == TY_RECORD || rk == TY_OTHER);

	nd_use(P, recv);
	if (recv->k == N_IDENT && recv->builder)
	{
		if (call && mname_is(m, "ToString") && a->n == 0)
			return recv;
		if (!call && mname_is(m, "Length"))
			return tpl(P, PR_ATOM, TY_INTEGER, "length(%1)", recv);
		if (call && (mname_is(m, "Append") || mname_is(m, "AppendLine") ||
					 mname_is(m, "AppendFormat") || mname_is(m, "Clear")))
			perr(P, "%.*s.%.*s(...) can only be used as a statement",
				 (int) strlen(recv->text), recv->text, m->len, m->s);
	}
	if (rk == TY_ARRAY)
	{
		if (!call && mname_is(m, "Length"))
			return tpl(P, PR_ATOM, TY_INTEGER, "cardinality(%1)", recv);
		if (call && mname_is(m, "Contains"))
		{
			argc_is(P, m, a, 1, 1);
			return tpl(P, PR_ATOM, TY_BOOLEAN, "(@2 = ANY(%1))", recv, a->a[0]);
		}
	}
	if (call && mname_is(m, "ToString"))
	{
		argc_is(P, m, a, 0, 1);
		if (a->n == 1)
		{
			if (a->a[0]->k != N_STR)
				perr(P, "ToString(format) needs a literal format");
			return map_format(P, recv, a->a[0]->sval, m->line);
		}
		return tpl(P, PR_CAST, TY_TEXT, "(%1)::text", recv);
	}
	if (call && mname_is(m, "Equals"))
	{
		argc_is(P, m, a, 1, 1);
		return tpl(P, PR_ATOM, TY_BOOLEAN, "(@1 = @2)", recv, a->a[0]);
	}
	if (strlike)
	{
		if (!call && mname_is(m, "Length"))
			return tpl(P, PR_ATOM, TY_INTEGER, "length(%1)", recv);
		if (call)
		{
			const char *f = NULL;

			if (mname_is(m, "ToUpper") || mname_is(m, "ToUpperInvariant"))
				f = "upper";
			else if (mname_is(m, "ToLower") || mname_is(m, "ToLowerInvariant"))
				f = "lower";
			if (f)
			{
				argc_is(P, m, a, 0, 0);
				return tpl(P, PR_ATOM, TY_TEXT, psprintf("%s(%%1)", f), recv);
			}
			if (mname_is(m, "Trim"))
				f = "btrim";
			else if (mname_is(m, "TrimStart"))
				f = "ltrim";
			else if (mname_is(m, "TrimEnd"))
				f = "rtrim";
			if (f)
			{
				argc_is(P, m, a, 0, 1);
				if (a->n == 1)
					return tpl(P, PR_ATOM, TY_TEXT, psprintf("%s(%%1, %%2)", f), recv, a->a[0]);
				return tpl(P, PR_ATOM, TY_TEXT, psprintf("%s(%%1)", f), recv);
			}
			if (mname_is(m, "Contains"))
			{
				argc_is(P, m, a, 1, 1);
				return tpl(P, PR_ATOM, TY_BOOLEAN, "(strpos(%1, %2) > 0)", recv, a->a[0]);
			}
			if (mname_is(m, "StartsWith"))
			{
				argc_is(P, m, a, 1, 1);
				return tpl(P, PR_ATOM, TY_BOOLEAN, "starts_with(%1, %2)", recv, a->a[0]);
			}
			if (mname_is(m, "EndsWith"))
			{
				argc_is(P, m, a, 1, 1);
				/* each operand once: a receiver repeated in the SQL could grow
				 * the output exponentially through nested calls */
				return tpl(P, PR_ATOM, TY_BOOLEAN, "(strpos(reverse(%1), reverse(%2)) = 1)",
						   recv, a->a[0]);
			}
			if (mname_is(m, "IndexOf"))
			{
				argc_is(P, m, a, 1, 1);
				return tpl(P, PR_ATOM, TY_INTEGER, "(strpos(%1, %2) - 1)", recv, a->a[0]);
			}
			if (mname_is(m, "Substring"))
			{
				argc_is(P, m, a, 1, 2);
				if (a->n == 1)
					return tpl(P, PR_ATOM, TY_TEXT, "substr(%1, %2)", recv, plus_one(P, a->a[0]));
				return tpl(P, PR_ATOM, TY_TEXT, "substr(%1, %2, %3)", recv,
						   plus_one(P, a->a[0]), a->a[1]);
			}
			if (mname_is(m, "Replace"))
			{
				argc_is(P, m, a, 2, 2);
				return tpl(P, PR_ATOM, TY_TEXT, "replace(%1, %2, %3)", recv, a->a[0], a->a[1]);
			}
			if (mname_is(m, "PadLeft") || mname_is(m, "PadRight"))
			{
				const char *pf = mname_is(m, "PadLeft") ? "lpad" : "rpad";

				argc_is(P, m, a, 1, 2);
				/* the receiver appears twice in the SQL, so it must be simple */
				if (!(recv->k == N_IDENT || recv->k == N_STR ||
					  (recv->k == N_TEXT && recv->is_path) ||
					  (recv->k == N_SEQ && recv->is_path)))
					perr(P, "%.*s needs a variable or a field as its receiver; store the value in a variable first",
						 m->len, m->s);
				/* C# never truncates; lpad/rpad do, so pad to at least the length */
				if (a->n == 1)
					return tpl(P, PR_ATOM, TY_TEXT,
							   psprintf("%s(%%1, greatest(%%2, length(%%1)))", pf),
							   recv, a->a[0]);
				return tpl(P, PR_ATOM, TY_TEXT,
						   psprintf("%s(%%1, greatest(%%2, length(%%1)), %%3)", pf),
						   recv, a->a[0], a->a[1]);
			}
			if (mname_is(m, "Split"))
			{
				argc_is(P, m, a, 1, 1);
				return tpl(P, PR_ATOM, mkty(TY_ARRAY, "text[]"), "string_to_array(%1, %2)",
						   recv, a->a[0]);
			}
		}
	}
	/* date and time members */
	if (rk == TY_OTHER || rk == TY_UNKNOWN || rk == TY_RECORD)
	{
		static const struct
		{
			const char *m;
			const char *f;
		}			parts[] = {
			{"Year", "year"}, {"Month", "month"}, {"Day", "day"}, {"Hour", "hour"},
			{"Minute", "minute"}, {"DayOfYear", "doy"}, {"DayOfWeek", "dow"},
		};
		static const struct
		{
			const char *m;
			const char *unit;
		}			adds[] = {
			{"AddDays", "1 day"}, {"AddHours", "1 hour"}, {"AddMinutes", "1 minute"},
			{"AddSeconds", "1 second"}, {"AddMonths", "1 month"}, {"AddYears", "1 year"},
		};
		int			i;

		if (!call)
		{
			for (i = 0; i < (int) lengthof(parts); i++)
				if (mname_is(m, parts[i].m))
					return tpl(P, PR_CAST, TY_INTEGER,
							   psprintf("(extract(%s from %%1))::integer", parts[i].f), recv);
			if (mname_is(m, "Second"))
				return tpl(P, PR_CAST, TY_INTEGER, "(floor(extract(second from %1)))::integer", recv);
			if (mname_is(m, "Date"))
				return tpl(P, PR_ATOM, mkty(TY_OTHER, "timestamp"), "date_trunc(%2, %1)",
						   recv, nd_str(P, "day", 3, TY_STRING));
		}
		else
			for (i = 0; i < (int) lengthof(adds); i++)
				if (mname_is(m, adds[i].m))
				{
					argc_is(P, m, a, 1, 1);
					return tpl(P, PR_ATOM, recv->ty.k == TY_OTHER ? recv->ty : TY_UNK,
							   "(@1 + @2 * (%3)::interval)", recv, a->a[0],
							   nd_str(P, adds[i].unit, (int) strlen(adds[i].unit), TY_STRING));
				}
	}
	if (call)
		perr(P, "the method '%.*s' is not supported here (plxcsharp maps a small set of .NET members; call a SQL function instead)",
			 m->len, m->s);
	if (rk == TY_STRING || rk == TY_CHAR || rk == TY_ARRAY || rk == TY_BOOL ||
		rk == TY_INT || rk == TY_NUM)
		perr(P, "the member '%.*s' is not supported on this value", m->len, m->s);
	return field_node(P, recv, m->s, m->len);
}

/* static classes whose members are mapped */
static const char *const cs_static_classes[] = {
	"Math", "Convert", "DateTime", "DateTimeOffset", "Guid", "Environment",
	"Console", "Debug", "PostgresErrorCodes", "String", "Int16", "Int32",
	"Int64", "Double", "Single", "Decimal", "Boolean", "Byte", "Char", NULL
};

static bool
is_static_class(CsTok *t)
{
	int			i;

	if (is_type_kw(t))
		return true;
	if (t->kind != CT_ID)
		return false;
	if (cs_type_lookup(t->s, t->len))
		return true;			/* DateOnly.Parse, TimeSpan.Parse, ... */
	for (i = 0; cs_static_classes[i]; i++)
		if (name_is(t, cs_static_classes[i]))
			return true;
	return false;
}

static CsNode *
map_static(CsP *P, CsTok *ct, CsTok *m, bool call, CsArgs *a)
{
	char	   *c = pnstrdup(ct->s, ct->len);
	const CsTypeMap *tm = cs_type_lookup(c, ct->len);

	if (strcmp(c, "String") == 0)
		c = "string";
	if (call)
		args_positional(P, a, psprintf("%s.%.*s", c, m->len, m->s));

	if (strcmp(c, "string") == 0)
	{
		if (call && mname_is(m, "IsNullOrEmpty"))
		{
			argc_is(P, m, a, 1, 1);
			return tpl(P, PR_ATOM, TY_BOOLEAN, "(COALESCE(%1, %2) = %2)", a->a[0],
					   nd_str(P, "", 0, TY_STRING));
		}
		if (call && mname_is(m, "IsNullOrWhiteSpace"))
		{
			argc_is(P, m, a, 1, 1);
			return tpl(P, PR_ATOM, TY_BOOLEAN, "(COALESCE(btrim(%1, %2), %3) = %3)", a->a[0],
					   nd_str(P, " \t\r\n\f\v", 6, TY_STRING), nd_str(P, "", 0, TY_STRING));
		}
		if (call && mname_is(m, "Join"))
		{
			if (a->n < 2)
				perr(P, "string.Join needs a separator and values");
			if (a->n == 2 && (a->a[1]->ty.k == TY_ARRAY || a->a[1]->ty.k == TY_UNKNOWN))
				return tpl(P, PR_ATOM, TY_TEXT, "array_to_string(%2, %1, %3)", a->a[0],
						   a->a[1], nd_str(P, "", 0, TY_STRING));
			{
				CsNode	   *arr = nd_new(P, N_SEQ, PR_ATOM, mkty(TY_ARRAY, "text[]"));
				int			i;

				nd_push(arr, "ARRAY[", NULL, 0);
				for (i = 1; i < a->n; i++)
				{
					if (i > 1)
						nd_push(arr, ", ", NULL, 0);
					seq_kid(P, arr, tpl(P, PR_CAST, TY_TEXT, "(%1)::text", a->a[i]), 0);
				}
				nd_push(arr, "]", NULL, 0);
				return tpl(P, PR_ATOM, TY_TEXT, "array_to_string(%2, %1, %3)", a->a[0],
						   arr, nd_str(P, "", 0, TY_STRING));
			}
		}
		if (call && mname_is(m, "Concat"))
		{
			if (a->n < 1)
				perr(P, "string.Concat needs values");
			return call_node(P, "concat", a, TY_TEXT);
		}
		if (call && mname_is(m, "Format"))
		{
			if (a->n < 1)
				perr(P, "string.Format needs a format string");
			return format_node(P, a->a[0], a->a + 1, a->n - 1);
		}
		if (call && mname_is(m, "Equals"))
		{
			argc_is(P, m, a, 2, 2);
			return tpl(P, PR_ATOM, TY_BOOLEAN, "(@1 = @2)", a->a[0], a->a[1]);
		}
		if (!call && mname_is(m, "Empty"))
			return nd_str(P, "", 0, TY_STRING);
	}
	else if (strcmp(c, "Math") == 0)
	{
		const char *f = NULL;

		if (!call && mname_is(m, "PI"))
			return nd_text(P, "pi()", PR_ATOM, mkty(TY_NUM, "double precision"));
		if (!call && mname_is(m, "E"))
			return nd_text(P, "exp(1)", PR_ATOM, mkty(TY_NUM, "numeric"));
		if (call && (mname_is(m, "Max") || mname_is(m, "Min")))
		{
			argc_is(P, m, a, 2, 2);
			return tpl(P, PR_ATOM, ty_arith(a->a[0]->ty, a->a[1]->ty),
					   mname_is(m, "Max") ? "GREATEST(%1, %2)" : "LEAST(%1, %2)",
					   a->a[0], a->a[1]);
		}
		if (call && mname_is(m, "Abs"))
		{
			argc_is(P, m, a, 1, 1);
			return tpl(P, PR_ATOM, a->a[0]->ty, "abs(%1)", a->a[0]);
		}
		if (call && mname_is(m, "Sign"))
		{
			argc_is(P, m, a, 1, 1);
			return tpl(P, PR_CAST, TY_INTEGER, "(sign(%1))::integer", a->a[0]);
		}
		if (call && mname_is(m, "Round"))
		{
			argc_is(P, m, a, 1, 2);
			if (a->n == 2)
				return tpl(P, PR_ATOM, mkty(TY_NUM, "numeric"), "round((%1)::numeric, %2)",
						   a->a[0], a->a[1]);
			return tpl(P, PR_ATOM, a->a[0]->ty.k == TY_NUM ? a->a[0]->ty : mkty(TY_NUM, "numeric"),
					   "round(%1)", a->a[0]);
		}
		if (call && mname_is(m, "Floor"))
			f = "floor";
		else if (call && mname_is(m, "Ceiling"))
			f = "ceil";
		else if (call && mname_is(m, "Truncate"))
			f = "trunc";
		if (f)
		{
			argc_is(P, m, a, 1, 1);
			return tpl(P, PR_ATOM, a->a[0]->ty.k == TY_NUM ? a->a[0]->ty : mkty(TY_NUM, "numeric"),
					   psprintf("%s(%%1)", f), a->a[0]);
		}
		if (call && mname_is(m, "Sqrt"))
			f = "sqrt";
		else if (call && mname_is(m, "Exp"))
			f = "exp";
		else if (call && mname_is(m, "Log10"))
			f = "log";
		if (f)
		{
			argc_is(P, m, a, 1, 1);
			return tpl(P, PR_ATOM, mkty(TY_NUM, "double precision"),
					   psprintf("%s(%%1)", f), a->a[0]);
		}
		if (call && mname_is(m, "Log"))
		{
			argc_is(P, m, a, 1, 2);
			if (a->n == 2)
				return tpl(P, PR_ATOM, mkty(TY_NUM, "numeric"), "log(%2, %1)", a->a[0], a->a[1]);
			return tpl(P, PR_ATOM, mkty(TY_NUM, "double precision"), "ln(%1)", a->a[0]);
		}
		if (call && mname_is(m, "Pow"))
		{
			argc_is(P, m, a, 2, 2);
			return tpl(P, PR_ATOM, mkty(TY_NUM, "double precision"), "power(%1, %2)",
					   a->a[0], a->a[1]);
		}
	}
	else if (strcmp(c, "Convert") == 0)
	{
		static const struct
		{
			const char *m;
			const char *sql;
			CsTyK		k;
		}			conv[] = {
			{"ToInt32", "integer", TY_INT}, {"ToInt64", "bigint", TY_INT},
			{"ToInt16", "smallint", TY_INT}, {"ToDouble", "double precision", TY_NUM},
			{"ToSingle", "real", TY_NUM}, {"ToDecimal", "numeric", TY_NUM},
			{"ToString", "text", TY_STRING}, {"ToBoolean", "boolean", TY_BOOL},
		};
		int			i;

		for (i = 0; call && i < (int) lengthof(conv); i++)
			if (mname_is(m, conv[i].m))
			{
				argc_is(P, m, a, 1, 1);
				return tpl(P, PR_CAST, mkty(conv[i].k, conv[i].sql),
						   psprintf("(%%1)::%s", conv[i].sql), a->a[0]);
			}
	}
	else if (strcmp(c, "DateTime") == 0 || strcmp(c, "DateTimeOffset") == 0)
	{
		bool		off = (c[8] == 'O');
		CsTy		t = mkty(TY_OTHER, off ? "timestamptz" : "timestamp");

		if (!call && mname_is(m, "Now"))
			return nd_text(P, off ? "now()" : "localtimestamp", PR_ATOM, t);
		if (!call && mname_is(m, "UtcNow"))
			return off ? nd_text(P, "now()", PR_ATOM, t)
				: tpl(P, PR_ATOM, t, "timezone(%1, now())", nd_str(P, "UTC", 3, TY_STRING));
		if (!call && !off && mname_is(m, "Today"))
			return nd_text(P, "(current_date)::timestamp", PR_CAST, t);
		if (call && mname_is(m, "Parse"))
		{
			argc_is(P, m, a, 1, 1);
			return tpl(P, PR_CAST, t, psprintf("(%%1)::%s", t.sql), a->a[0]);
		}
	}
	else if (strcmp(c, "Guid") == 0)
	{
		CsTy		t = mkty(TY_OTHER, "uuid");

		if (call && mname_is(m, "NewGuid"))
		{
			argc_is(P, m, a, 0, 0);
			return nd_text(P, "gen_random_uuid()", PR_ATOM, t);
		}
		if (call && mname_is(m, "Parse"))
		{
			argc_is(P, m, a, 1, 1);
			return tpl(P, PR_CAST, t, "(%1)::uuid", a->a[0]);
		}
		if (!call && mname_is(m, "Empty"))
			return tpl(P, PR_CAST, t, "(%1)::uuid",
					   nd_str(P, "00000000-0000-0000-0000-000000000000", 36, TY_STRING));
	}
	else if (strcmp(c, "Environment") == 0)
	{
		if (!call && mname_is(m, "NewLine"))
			return nd_str(P, "\n", 1, TY_STRING);
	}
	else if (strcmp(c, "Console") == 0 || strcmp(c, "Debug") == 0)
		perr(P, "%s.%.*s can only be used as a statement", c, m->len, m->s);
	else if (strcmp(c, "PostgresErrorCodes") == 0)
		perr(P, "PostgresErrorCodes can only be used in a catch filter (catch (PostgresException e) when (e.SqlState == PostgresErrorCodes.UniqueViolation))");
	else if (strcmp(c, "char") == 0 || strcmp(c, "Char") == 0)
	{
		static const struct
		{
			const char *m;
			const char *re;
		}			cls[] = {
			{"IsDigit", "^[0-9]$"}, {"IsLetter", "^[[:alpha:]]$"},
			{"IsWhiteSpace", "^[[:space:]]$"}, {"IsLetterOrDigit", "^[[:alnum:]]$"},
		};
		int			i;

		for (i = 0; call && i < (int) lengthof(cls); i++)
			if (mname_is(m, cls[i].m))
			{
				argc_is(P, m, a, 1, 1);
				return tpl(P, PR_ATOM, TY_BOOLEAN, "(@1 ~ %2)", a->a[0],
						   nd_str(P, cls[i].re, (int) strlen(cls[i].re), TY_STRING));
			}
		if (call && (mname_is(m, "ToUpper") || mname_is(m, "ToLower")))
		{
			argc_is(P, m, a, 1, 1);
			return tpl(P, PR_ATOM, mkty(TY_CHAR, "text"),
					   mname_is(m, "ToUpper") ? "upper(%1)" : "lower(%1)", a->a[0]);
		}
	}
	if (tm && tm->k != TY_CHAR && tm->k != TY_STRING)
	{
		/* int.Parse, long.MaxValue, ... */
		if (call && mname_is(m, "Parse"))
		{
			argc_is(P, m, a, 1, 1);
			return tpl(P, PR_CAST, mkty(tm->k, tm->sql), psprintf("(%%1)::%s", tm->sql), a->a[0]);
		}
		if (!call && (mname_is(m, "MaxValue") || mname_is(m, "MinValue")))
		{
			static const struct
			{
				const char *t;
				const char *mx;
				const char *mn;
			}			lim[] = {
				{"integer", "2147483647", "-2147483648"},
				{"bigint", "9223372036854775807", "-9223372036854775808"},
				{"smallint", "32767", "-32768"},
			};
			int			i;

			for (i = 0; i < (int) lengthof(lim); i++)
				if (strcmp(tm->sql, lim[i].t) == 0 &&
					(strcmp(tm->cs, "byte") != 0 && strcmp(tm->cs, "Byte") != 0 &&
					 strcmp(tm->cs, "sbyte") != 0 && strcmp(tm->cs, "SByte") != 0 &&
					 strcmp(tm->cs, "ushort") != 0 && strcmp(tm->cs, "UInt16") != 0 &&
					 strcmp(tm->cs, "uint") != 0 && strcmp(tm->cs, "UInt32") != 0))
				{
					bool		mx = mname_is(m, "MaxValue");

					return nd_text(P, mx ? lim[i].mx : lim[i].mn,
								   mx ? PR_ATOM : PR_UNARY, mkty(tm->k, tm->sql));
				}
		}
		if (call && mname_is(m, "TryParse"))
			perr(P, "%s.TryParse is not supported (it needs an out argument); use %s.Parse inside try/catch",
				 c, c);
	}
	perr(P, "%s.%.*s is not supported", c, m->len, m->s);
	return NULL;				/* keep compiler quiet */
}

/* new[] { ... }, new T[] { ... }, new T[n] */
static CsNode *
array_lit(CsP *P, const char *open, const char *close, const char *elemsql)
{
	CsNode	   *n = nd_new(P, N_SEQ, PR_ATOM, TY_UNK);
	CsTy		et = TY_UNK;
	int			cnt = 0;

	expect(P, open);
	nd_push(n, "ARRAY[", NULL, 0);
	while (!op_is(cur(P), close))
	{
		CsNode	   *e;

		CHECK_FOR_INTERRUPTS();
		if (cnt > 0)
			nd_push(n, ", ", NULL, 0);
		e = parse_expr(P);
		if (et.k == TY_UNKNOWN || et.k == TY_NULL)
			et = e->ty;
		seq_kid(P, n, e, 0);
		cnt++;
		if (!op_is(cur(P), ","))
			break;
		advance(P);
	}
	expect(P, close);
	if (elemsql)
	{
		nd_push(n, psprintf("]::%s[]", elemsql), NULL, 0);
		n->prec = PR_CAST;
		n->ty = mkty(TY_ARRAY, psprintf("%s[]", elemsql));
		return n;
	}
	nd_push(n, "]", NULL, 0);
	if (cnt == 0)
	{
		n->empty_coll = true;
		return n;
	}
	n->ty = (et.sql && et.k != TY_ARRAY) ? mkty(TY_ARRAY, psprintf("%s[]", et.sql))
		: mkty(TY_ARRAY, NULL);
	return n;
}

static CsNode *
parse_new(CsP *P)
{
	advance(P);					/* new */
	if (op_is(cur(P), "[") && op_is(peek(P, 1), "]"))
	{
		advance(P);
		advance(P);
		return array_lit(P, "{", "}", NULL);
	}
	if (op_is(cur(P), "{"))
		perr(P, "anonymous types are not supported");
	if (cur(P)->kind != CT_ID)
		perr(P, "expected a type after new");
	if (op_is(peek(P, 1), "(") || op_is(peek(P, 1), "{") ||
		(op_is(peek(P, 1), ".") && !is_type_kw(cur(P))))
	{
		int			e = type_end(P, P->pos);

		if (e > 0 && op_is(tok_at(P, e), "("))
			perr(P, "'new %.*s(...)' is not supported here (only arrays, new StringBuilder() in a declaration, and new ...Exception(...) after throw)",
				 cur(P)->len, cur(P)->s);
		if (e > 0 && op_is(tok_at(P, e), "{"))
			perr(P, "object initializers are not supported");
	}
	/* the element type, without array brackets */
	{
		CsTok	   *t = cur(P);
		const CsTypeMap *m;
		char	   *esql;
		CsTyK		ek;

		if (is_reserved(t) && !is_type_kw(t))
			perr(P, "expected a type after new");
		advance(P);
		m = cs_type_lookup(t->s, t->len);
		if (m)
		{
			esql = (char *) m->sql;
			ek = m->k;
		}
		else
		{
			esql = pnstrdup(t->s, t->len);
			ek = sql_type_kind(esql);
		}
		if (op_is(cur(P), "?"))
			advance(P);
		if (!op_is(cur(P), "["))
			perr(P, "'new %s' is not supported here (only arrays, new StringBuilder() in a declaration, and new ...Exception(...) after throw)",
				 esql);
		if (op_is(peek(P, 1), "]"))
		{
			advance(P);
			advance(P);
			if (op_is(cur(P), "["))
				perr(P, "jagged arrays (T[][]) are not supported");
			if (!op_is(cur(P), "{"))
				perr(P, "an array creation needs an initializer or a size");
			return array_lit(P, "{", "}", esql);
		}
		advance(P);				/* [ */
		{
			CsNode	   *size = parse_expr(P);
			const char *dflt;

			if (op_is(cur(P), ","))
				perr(P, "multi-dimensional arrays are not supported");
			expect(P, "]");
			if (op_is(cur(P), "{"))
				return array_lit(P, "{", "}", esql);
			dflt = (ek == TY_INT || ek == TY_NUM) ? "0" : (ek == TY_BOOL) ? "false" : "NULL";
			return tpl(P, PR_ATOM, mkty(TY_ARRAY, psprintf("%s[]", esql)),
					   psprintf("array_fill(%s::%s, ARRAY[%%1])", dflt, esql), size);
		}
	}
}

/* catch-variable accessors: e.Message -> e.message, which the engine maps */
static CsNode *
exc_member(CsP *P, CsCatch *c, CsTok *m)
{
	static const struct
	{
		const char *cs;
		const char *plx;
	}			map[] = {
		{"Message", "message"}, {"MessageText", "message"}, {"SqlState", "sqlstate"},
		{"Detail", "detail"}, {"Hint", "hint"}, {"ConstraintName", "constraint"},
		{"ColumnName", "column"}, {"TableName", "table"}, {"SchemaName", "schema"},
		{"DataTypeName", "datatype"},
	};
	int			i;

	for (i = 0; i < (int) lengthof(map); i++)
		if (mname_is(m, map[i].cs))
			return nd_text(P, psprintf("%.*s.%s", c->len, c->var, map[i].plx), PR_ATOM, TY_TEXT);
	perr(P, "the exception property '%.*s' is not supported (use Message, SqlState, Detail, Hint, ConstraintName, ColumnName, TableName, SchemaName or DataTypeName)",
		 m->len, m->s);
	return NULL;
}

static CsCatch *
catch_lookup(CsP *P, CsTok *t, bool *shadowed)
{
	CsCatch    *c;
	bool		first = true;

	*shadowed = false;
	for (c = P->catches; c; c = c->outer)
	{
		if (c->var && c->len == t->len && memcmp(c->var, t->s, t->len) == 0)
		{
			*shadowed = !first;
			return c;
		}
		first = false;
	}
	return NULL;
}

static CsNode *
parse_ident_primary(CsP *P)
{
	CsTok	   *t = cur(P);
	CsTok	   *nx = peek(P, 1);
	CsSym	   *s;
	CsCatch    *c;
	bool		shadowed;

	if (!t->atid)
	{
		if (kw_is(t, "true") || kw_is(t, "false"))
		{
			advance(P);
			return nd_text(P, kw_is(t, "true") ? "true" : "false", PR_ATOM, TY_BOOLEAN);
		}
		if (kw_is(t, "null"))
		{
			CsNode	   *n;

			advance(P);
			n = nd_text(P, "NULL", PR_ATOM, mkty(TY_NULL, NULL));
			n->is_null = true;
			return n;
		}
		if (kw_is(t, "new"))
			return parse_new(P);
		if (kw_is(t, "nameof") && op_is(nx, "("))
		{
			CsTok	   *last = NULL;

			advance(P);
			advance(P);
			while (!op_is(cur(P), ")"))
			{
				if (cur(P)->kind == CT_ID)
					last = cur(P);
				else if (!op_is(cur(P), "."))
					perr(P, "nameof needs a name");
				advance(P);
			}
			advance(P);
			if (!last)
				perr(P, "nameof needs a name");
			return nd_str(P, last->s, last->len, TY_STRING);
		}
		if (kw_is(t, "default"))
			perr(P, "default is not supported; write the value");
		if (kw_is(t, "typeof") || kw_is(t, "sizeof") || kw_is(t, "checked") ||
			kw_is(t, "unchecked") || kw_is(t, "stackalloc") || kw_is(t, "this") ||
			kw_is(t, "base") || kw_is(t, "delegate") || name_is(t, "await") ||
			kw_is(t, "throw") || name_is(t, "async"))
			perr(P, "'%.*s' is not supported in plxcsharp", t->len, t->s);
		if (is_static_class(t) && op_is(nx, ".") && !sym_find(P, t->s, t->len) &&
			!plx_is_param(P->cx, t->s, t->len))
		{
			CsTok	   *m;
			CsArgs		a;
			bool		call;

			advance(P);
			advance(P);
			m = cur(P);
			if (m->kind != CT_ID)
				perr(P, "expected a member name after '.'");
			advance(P);
			call = op_is(cur(P), "(");
			memset(&a, 0, sizeof(a));
			if (call)
				parse_args(P, &a);
			return map_static(P, t, m, call, &a);
		}
		if (is_type_kw(t) && peek(P, 1)->kind == CT_ID)
			perr(P, "lambda expressions and declarations inside an expression are not supported");
		if (is_reserved(t))
			perr(P, "unexpected keyword '%.*s' in an expression", t->len, t->s);
		if (op_is(nx, "(") || (name_is(t, "fetch_one") && op_is(nx, "!") &&
							   op_is(peek(P, 2), "(")))
		{
			static const char *const valued[] = {
				"query", "fetch_one", "open_cursor", "fetch_from", "row_count", NULL
			};
			static const char *const stmts[] = {
				"execute", "perform", "return_query", "raise", "call", "commit",
				"rollback", "close_cursor", "move_cursor", "emit", "return_next",
				"assert", NULL
			};
			int			i;
			CsArgs		a;
			bool		strict = op_is(nx, "!");

			for (i = 0; valued[i]; i++)
				if (name_is(t, valued[i]))
				{
					CsNode	   *n;

					advance(P);
					if (strict)
						advance(P);
					parse_args(P, &a);
					args_positional(P, &a, valued[i]);
					n = call_node(P, strict ? "fetch_one!" : valued[i], &a, TY_UNK);
					n->has_call = false;
					n->intrin = strict ? "fetch_one!" : valued[i];
					return n;
				}
			if (strict)
				perr(P, "unexpected '!'");
			for (i = 0; stmts[i]; i++)
				if (name_is(t, stmts[i]))
					perr(P, "%s(...) is a statement and has no value", stmts[i]);
			if (name_is(t, "found"))
			{
				advance(P);
				parse_args(P, &a);
				if (a.n)
					perr(P, "found() takes no arguments");
				return nd_text(P, "found()", PR_ATOM, TY_BOOLEAN);
			}
			check_name(P, t);
			if (sym_find(P, t->s, t->len) || plx_is_param(P->cx, t->s, t->len))
				perr(P, "'%.*s' is a variable, not a function", t->len, t->s);
			advance(P);
			parse_args(P, &a);
			{
				CsNode	   *n = call_node(P, pnstrdup(t->s, t->len), &a,
										  sqlfunc_ty(t->s, t->len));

				if (!name_is(t, "ident") && !name_is(t, "literal") && !name_is(t, "raw"))
					n->has_call = true;
				return n;
			}
		}
	}
	check_name(P, t);
	if (op_is(nx, "=>"))
		perr(P, "lambda expressions are not supported");
	c = catch_lookup(P, t, &shadowed);
	if (c)
	{
		if (shadowed)
			perr(P, "the exception variable '%.*s' of an enclosing catch is not available inside a nested catch block",
				 t->len, t->s);
		advance(P);
		if (!op_is(cur(P), ".") || peek(P, 1)->kind != CT_ID)
			perr(P, "use a property of the exception, for example %.*s.Message",
				 t->len, t->s);
		advance(P);
		{
			CsTok	   *m = cur(P);

			advance(P);
			if (op_is(cur(P), "("))
			{
				CsArgs		a;

				parse_args(P, &a);
				if (mname_is(m, "ToString") && a.n == 0)
					return nd_text(P, psprintf("%.*s.message", c->len, c->var), PR_ATOM, TY_TEXT);
				perr(P, "methods of the exception are not supported");
			}
			return exc_member(P, c, m);
		}
	}
	advance(P);
	s = sym_find(P, t->s, t->len);
	if (s)
	{
		CsNode	   *n = nd_new(P, N_IDENT, PR_ATOM, s->ty);

		if ((int) strlen(s->name) != t->len || memcmp(s->name, t->s, t->len) != 0)
			perr(P, "'%.*s' and '%s' are the same name in PostgreSQL, which ignores case; use one spelling",
				 t->len, t->s, s->name);
		n->text = s->name;
		n->name = s->name;
		n->assignable = !s->is_const;
		n->is_path = true;
		n->builder = (s->ty.k == TY_STRING &&
					  (s->is_sb || (P->appended && cs_hash_get(P->appended, t->s, t->len))));
		return n;
	}
	if (plx_is_param(P->cx, t->s, t->len) ||
		((P->trigger) && (name_is(t, "NEW") || name_is(t, "OLD"))) ||
		((P->trigger || P->evtrigger) && t->len > 3 && strncmp(t->s, "TG_", 3) == 0))
	{
		CsNode	   *n;
		CsTy		pt;

		if (name_is(t, "NEW") || name_is(t, "OLD"))
			pt = mkty(TY_RECORD, NULL);
		else if (name_is(t, "TG_NARGS"))
			pt = TY_INTEGER;
		else if (name_is(t, "TG_ARGV"))
			pt = mkty(TY_ARRAY, "text[]");
		else if (strncmp(t->s, "TG_", 3) == 0 && !plx_is_param(P->cx, t->s, t->len))
			pt = TY_TEXT;
		else
			pt = param_ty(P, t->s, t->len);
		n = nd_new(P, N_IDENT, PR_ATOM, pt);
		n->text = pnstrdup(t->s, t->len);
		n->name = n->text;
		n->assignable = true;
		n->is_path = true;
		return n;
	}
	/* anything else passes through to SQL (current_date, a column alias, ...) */
	{
		CsNode	   *n = nd_text(P, pnstrdup(t->s, t->len), PR_ATOM, TY_UNK);

		n->is_path = true;
		return n;
	}
}

static CsNode *
parse_primary(CsP *P)
{
	CsTok	   *t = cur(P);

	switch (t->kind)
	{
		case CT_NUM:
			advance(P);
			return num_node(P, t);
		case CT_STR:
			advance(P);
			return nd_str(P, t->val, t->vlen, TY_STRING);
		case CT_CHAR:
			advance(P);
			return nd_str(P, t->val, t->vlen, TY_CHAR);
		case CT_ISTR:
			advance(P);
			return parse_fstr(P, t);
		case CT_ID:
			return parse_ident_primary(P);
		case CT_OP:
			if (op_is(t, "("))
			{
				CsNode	   *e;

				advance(P);
				e = parse_expr(P);
				if (op_is(cur(P), ","))
					perr(P, "tuples are not supported");
				expect(P, ")");
				if (op_is(cur(P), "=>"))
					perr(P, "lambda expressions are not supported");
				return paren(P, e);
			}
			if (op_is(t, "["))
				return array_lit(P, "[", "]", NULL);
			break;
		default:
			break;
	}
	perr(P, "unexpected %s in an expression", tokdesc(t));
	return NULL;
}

static CsNode *
parse_postfix(CsP *P)
{
	CsNode	   *n = parse_primary(P);

	for (;;)
	{
		CsTok	   *t = cur(P);

		CHECK_FOR_INTERRUPTS();
		if (op_is(t, ".") || op_is(t, "?."))
		{
			CsTok	   *m;
			CsArgs		a;
			bool		call;

			advance(P);
			m = cur(P);
			if (m->kind != CT_ID)
				perr(P, "expected a member name after '.'");
			advance(P);
			call = op_is(cur(P), "(");
			memset(&a, 0, sizeof(a));
			if (call)
				parse_args(P, &a);
			if (n->k == N_TEXT && n->is_path && !n->intrin && n->ty.k == TY_UNKNOWN)
			{
				/* an unknown name: schema.function(...) or a qualified name */
				if (call)
				{
					CsNode	   *cn = call_node(P, psprintf("%s.%.*s", n->text, m->len, m->s),
											   &a, TY_UNK);

					cn->has_call = true;
					n = cn;
				}
				else
				{
					n = nd_text(P, psprintf("%s.%.*s", n->text, m->len, m->s), PR_ATOM, TY_UNK);
					n->is_path = true;
				}
				continue;
			}
			n = inst_member(P, n, m, call, &a);
			continue;
		}
		if (op_is(t, "[") || (op_is(t, "?") && op_is(peek(P, 1), "[")))
		{
			CsNode	   *idx;

			if (op_is(t, "?"))
				advance(P);
			advance(P);
			nd_use(P, n);
			idx = parse_expr(P);
			if (op_is(cur(P), ","))
				perr(P, "multi-dimensional indexing is not supported");
			expect(P, "]");
			n = elem_node(P, n, idx);
			continue;
		}
		if (op_is(t, "!"))
		{
			/* null-forgiving x! has no runtime effect */
			advance(P);
			continue;
		}
		if ((op_is(t, "++") || op_is(t, "--")) && !P->allow_postinc)
			perr(P, "++ and -- can only be used as statements");
		if (op_is(t, "("))
			perr(P, "calling the result of an expression is not supported");
		break;
	}
	return n;
}

/* is the '(' at P->pos the start of a cast (T)x? */
static bool
is_cast(CsP *P)
{
	CsTok	   *t = peek(P, 1);
	int			e;
	CsTok	   *after;

	if (t->kind != CT_ID)
		return false;
	e = type_end(P, P->pos + 1);
	if (e < 0 || !op_is(tok_at(P, e), ")"))
		return false;
	if (is_type_kw(t))
		return true;
	if (!cs_type_lookup(t->s, t->len) || op_is(tok_at(P, P->pos + 2), "."))
		return false;
	after = tok_at(P, e + 1);
	return after->kind == CT_ID || after->kind == CT_NUM || after->kind == CT_STR ||
		after->kind == CT_ISTR || after->kind == CT_CHAR || op_is(after, "(");
}

static CsNode *
parse_unary(CsP *P)
{
	CsTok	   *t = cur(P);
	CsNode	   *r;

	enter(P);
	if (op_is(t, "!"))
	{
		CsNode	   *k;

		advance(P);
		k = parse_unary(P);
		r = tpl(P, PR_NOT, TY_BOOLEAN, "NOT !1", k);
	}
	else if (op_is(t, "-"))
	{
		CsNode	   *k;

		advance(P);
		k = parse_unary(P);
		if (k->k == N_TEXT && k->intlit)
			r = nd_text(P, psprintf("-%s", k->text), PR_UNARY, k->ty);
		else if (ty_stringish(k->ty))
			perr(P, "unary '-' cannot be applied to a string");
		else
			r = tpl(P, PR_UNARY, k->ty, "-@1", k);
	}
	else if (op_is(t, "+"))
	{
		advance(P);
		r = parse_unary(P);
	}
	else if (op_is(t, "~"))
	{
		CsNode	   *k;

		advance(P);
		k = parse_unary(P);
		r = tpl(P, PR_ATOM, k->ty.k == TY_INT ? k->ty : TY_UNK, "~(%1)", k);
	}
	else if (op_is(t, "++") || op_is(t, "--"))
		perr(P, "++ and -- can only be used as statements");
	else if (op_is(t, "^") || op_is(t, ".."))
		perr(P, "ranges and indexes from the end are not supported");
	else if (op_is(t, "&") || op_is(t, "*"))
		perr(P, "pointers are not supported");
	else if (op_is(t, "(") && is_cast(P))
	{
		CsType		ti;
		CsNode	   *k;

		advance(P);
		parse_type(P, &ti);
		expect(P, ")");
		k = parse_unary(P);
		r = cast_node(P, &ti, k);
	}
	else
		r = parse_postfix(P);
	leave(P);
	return r;
}

/* x switch { pattern => value, ... } */
static CsNode *
parse_switch_expr(CsP *P, CsNode *subj)
{
	typedef struct
	{
		int			kind;		/* 0 const, 1 discard, 2 null, 3 not null, 4 relational */
		const char *rop;
		CsNode	   *v;
		CsNode	   *guard;
		CsNode	   *res;
	} Arm;
	Arm		   *arms = NULL;
	int			n = 0,
				cap = 0,
				i;
	bool		simple = true;
	CsNode	   *out;
	CsTy		rt = mkty(TY_NULL, NULL);

	advance(P);					/* switch */
	expect(P, "{");
	while (!op_is(cur(P), "}"))
	{
		Arm			a;

		CHECK_FOR_INTERRUPTS();
		memset(&a, 0, sizeof(a));
		if (n > 0 && arms[n - 1].kind == 1)
			perr(P, "the discard arm (_) must be the last arm");
		if (name_is(cur(P), "_"))
		{
			a.kind = 1;
			advance(P);
		}
		else if (kw_is(cur(P), "null"))
		{
			a.kind = 2;
			advance(P);
			simple = false;
		}
		else if (name_is(cur(P), "not") && kw_is(peek(P, 1), "null"))
		{
			a.kind = 3;
			advance(P);
			advance(P);
			simple = false;
		}
		else if (op_is(cur(P), "<") || op_is(cur(P), "<=") ||
				 op_is(cur(P), ">") || op_is(cur(P), ">="))
		{
			a.kind = 4;
			a.rop = pnstrdup(cur(P)->s, cur(P)->len);
			advance(P);
			a.v = parse_level(P, 11);
			simple = false;
		}
		else if (kw_is(cur(P), "var") || name_is(cur(P), "not") ||
				 (cur(P)->kind == CT_ID && peek(P, 1)->kind == CT_ID))
			perr(P, "this switch pattern is not supported (use constants, _, null, not null, or < <= > >=)");
		else
			a.v = parse_level(P, 11);
		if (name_is(cur(P), "or") || name_is(cur(P), "and"))
			perr(P, "pattern combinators (or, and) are not supported in a switch expression");
		if (name_is(cur(P), "when"))
		{
			advance(P);
			a.guard = parse_expr(P);
			simple = false;
		}
		expect(P, "=>");
		a.res = parse_expr(P);
		rt = ty_merge(rt, a.res->ty);
		if (n >= cap)
		{
			cap = cap ? cap * 2 : 4;
			arms = arms ? repalloc(arms, sizeof(Arm) * cap) : palloc(sizeof(Arm) * cap);
		}
		arms[n++] = a;
		if (!op_is(cur(P), ","))
			break;
		advance(P);
	}
	expect(P, "}");
	if (n == 0)
		perr(P, "a switch expression needs at least one arm");
	if (!simple && subj->k != N_IDENT && subj->k != N_TEXT && subj->k != N_STR)
		perr(P, "a switch expression with null, relational or when patterns needs a variable as its subject");
	out = nd_new(P, N_SEQ, PR_ATOM, rt.k == TY_NULL ? TY_UNK : rt);
	nd_push(out, "CASE ", NULL, 0);
	if (simple)
	{
		seq_kid(P, out, subj, 0);
		nd_push(out, " ", NULL, 0);
	}
	for (i = 0; i < n; i++)
	{
		Arm		   *a = &arms[i];
		CsNode	   *cond = NULL;

		if (a->kind == 1 && !a->guard)
		{
			nd_push(out, "ELSE ", NULL, 0);
			seq_kid(P, out, a->res, 0);
			nd_push(out, " ", NULL, 0);
			continue;
		}
		nd_push(out, "WHEN ", NULL, 0);
		if (simple)
			cond = a->v;
		else
		{
			switch (a->kind)
			{
				case 0:
					cond = bin_step(P, subj, "==", a->v);
					break;
				case 2:
					cond = tpl(P, PR_IS, TY_BOOLEAN, "^1 IS NULL", subj);
					break;
				case 3:
					cond = tpl(P, PR_IS, TY_BOOLEAN, "^1 IS NOT NULL", subj);
					break;
				case 4:
					cond = bin_step(P, subj, a->rop, a->v);
					break;
				default:
					break;
			}
			if (a->guard)
				cond = cond ? bin_step(P, cond, "&&", a->guard) : a->guard;
		}
		seq_kid(P, out, cond, 0);
		nd_push(out, " THEN ", NULL, 0);
		seq_kid(P, out, a->res, 0);
		nd_push(out, " ", NULL, 0);
	}
	nd_push(out, "END", NULL, 0);
	return out;
}

static CsNode *
parse_switchx(CsP *P)
{
	CsNode	   *u = parse_unary(P);

	while (kw_is(cur(P), "switch") && op_is(peek(P, 1), "{"))
		u = parse_switch_expr(P, u);
	return u;
}

static int
binlvl(CsTok *t)
{
	if (t->kind != CT_OP)
		return (kw_is(t, "is") || kw_is(t, "as")) ? 10 : 0;
	if (op_is(t, "||"))
		return 4;
	if (op_is(t, "&&"))
		return 5;
	if (op_is(t, "|"))
		return 6;
	if (op_is(t, "^"))
		return 7;
	if (op_is(t, "&"))
		return 8;
	if (op_is(t, "==") || op_is(t, "!="))
		return 9;
	if (op_is(t, "<") || op_is(t, ">") || op_is(t, "<=") || op_is(t, ">="))
		return 10;
	if (op_is(t, "<<") || op_is(t, ">>") || op_is(t, ">>>"))
		return 11;
	if (op_is(t, "+") || op_is(t, "-"))
		return 12;
	if (op_is(t, "*") || op_is(t, "/") || op_is(t, "%"))
		return 13;
	return 0;
}

static CsNode *
parse_level(CsP *P, int lvl)
{
	CsNode	   *left;

	if (lvl > 13)
		return parse_switchx(P);
	check_stack_depth();
	left = parse_level(P, lvl + 1);
	while (binlvl(cur(P)) == lvl)
	{
		CsTok	   *op = cur(P);
		char	   *ops;
		CsNode	   *right;

		CHECK_FOR_INTERRUPTS();
		if (kw_is(op, "as"))
			perr(P, "the 'as' operator is not supported");
		if (op_is(op, ">>>"))
			perr(P, "the '>>>' operator is not supported");
		if (kw_is(op, "is"))
		{
			advance(P);
			if (kw_is(cur(P), "null"))
			{
				advance(P);
				left = tpl(P, PR_IS, TY_BOOLEAN, "^1 IS NULL", left);
			}
			else if (name_is(cur(P), "not") && kw_is(peek(P, 1), "null"))
			{
				advance(P);
				advance(P);
				left = tpl(P, PR_IS, TY_BOOLEAN, "^1 IS NOT NULL", left);
			}
			else
				perr(P, "only the 'is null' and 'is not null' patterns are supported");
			continue;
		}
		ops = pnstrdup(op->s, op->len);
		advance(P);
		right = parse_level(P, lvl + 1);
		left = bin_step(P, left, ops, right);
	}
	return left;
}

static CsNode *
parse_coalesce(CsP *P)
{
	CsNode	   *l = parse_level(P, 4);

	if (op_is(cur(P), "??"))
	{
		CsNode	   *r;

		advance(P);
		r = parse_coalesce(P);
		return tpl(P, PR_ATOM, ty_merge(l->ty, r->ty), "COALESCE(%1, %2)", l, r);
	}
	return l;
}

static CsNode *
parse_expr(CsP *P)
{
	CsNode	   *c;

	enter(P);
	c = parse_coalesce(P);
	if (op_is(cur(P), "?"))
	{
		CsNode	   *a,
				   *b;

		advance(P);
		a = parse_expr(P);
		expect(P, ":");
		b = parse_expr(P);
		c = tpl(P, PR_ATOM, ty_merge(a->ty, b->ty), "CASE WHEN %1 THEN %2 ELSE %3 END", c, a, b);
	}
	if (op_is(cur(P), "=") || op_is(cur(P), "+=") || op_is(cur(P), "-="))
		perr(P, "an assignment cannot be used inside an expression");
	leave(P);
	return c;
}

/* ======================================================================== */
/* Stage 1: statements                                                      */
/* ======================================================================== */

static void st_stmt(CsP *P);
static void st_block(CsP *P);

/* a condition must be a bool in C#; refuse the obviously wrong ones */
static void
check_cond(CsP *P, CsNode *c)
{
	CsTyK		k = c->ty.k;

	if (k == TY_STRING || k == TY_CHAR || k == TY_INT || k == TY_NUM || k == TY_ARRAY)
		plx_err(P->cx, c->line, "a condition must be a bool expression");
}

static CsNode *
parse_cond_paren(CsP *P)
{
	CsNode	   *c;

	expect(P, "(");
	c = parse_expr(P);
	nd_use(P, c);
	check_cond(P, c);
	expect(P, ")");
	return c;
}

static void
st_embedded(CsP *P)
{
	if (op_is(cur(P), "{"))
		st_block(P);
	else
	{
		out_s(P, "{");
		st_stmt(P);
		out_s(P, " }");
	}
}

static void
st_block(CsP *P)
{
	int			line = cur(P)->line;

	expect(P, "{");
	out_s(P, "{");
	while (!op_is(cur(P), "}"))
	{
		if (cur(P)->kind == CT_EOF)
			plx_err(P->cx, line, "unterminated block: the '{' on line %d has no matching '}'", line);
		st_stmt(P);
	}
	out_sync(P, cur(P)->line);
	out_s(P, " }");
	advance(P);
}

/* an assignment-like statement expression */
typedef struct
{
	int			k;				/* 0 =, 1 op=, 2 ++/--, 3 ??= */
	CsNode	   *lhs;
	CsNode	   *rhs;
	char	   *op;				/* op=: the binary operator */
	bool		inc;
	int			line;
} CsSE;

static bool
parse_se(CsP *P, CsSE *se)
{
	static const char *const compound[] = {
		"+=", "-=", "*=", "/=", "%=", "&=", "|=", "^=", "<<=", ">>=", NULL
	};
	CsTok	   *t = cur(P);
	int			i;

	memset(se, 0, sizeof(*se));
	se->line = t->line;
	if (op_is(t, "++") || op_is(t, "--"))
	{
		se->k = 2;
		se->inc = op_is(t, "++");
		advance(P);
		se->lhs = parse_postfix(P);
	}
	else
	{
		P->allow_postinc = true;
		se->lhs = parse_postfix(P);
		P->allow_postinc = false;
		t = cur(P);
		if (op_is(t, "="))
		{
			advance(P);
			se->k = 0;
			se->rhs = parse_expr(P);
		}
		else if (op_is(t, "?" "?="))
		{
			advance(P);
			se->k = 3;
			se->rhs = parse_expr(P);
		}
		else if (op_is(t, "++") || op_is(t, "--"))
		{
			se->k = 2;
			se->inc = op_is(t, "++");
			advance(P);
		}
		else
		{
			for (i = 0; compound[i]; i++)
				if (op_is(t, compound[i]))
					break;
			if (!compound[i])
			{
				if (op_is(t, ">>>="))
					perr(P, "the '>>>=' operator is not supported");
				return false;
			}
			se->k = 1;
			se->op = pnstrdup(t->s, t->len - 1);
			advance(P);
			se->rhs = parse_expr(P);
		}
	}
	if (!se->lhs->assignable)
	{
		CsNode	   *l = se->lhs;

		if (l->k == N_TEXT && l->is_path && l->ty.k == TY_UNKNOWN && !strchr(l->text, '.'))
			plx_err(P->cx, se->line, "'%s' is not declared", l->text);
		if (l->k == N_IDENT)
			plx_err(P->cx, se->line, "cannot assign to the constant '%s'", l->text);
		plx_err(P->cx, se->line, "this expression cannot be assigned to");
	}
	note_asg(P, se->lhs->name);
	return true;
}

static void
render_se(CsP *P, CsSE *se)
{
	CsNode	   *l = se->lhs;
	CsNode	   *c;

	switch (se->k)
	{
		case 0:
			if (se->rhs->intrin)
			{
				if (l->k != N_IDENT)
					plx_err(P->cx, se->line, "%s(...) can only be assigned to a variable",
							se->rhs->intrin);
				if (strcmp(se->rhs->intrin, "query") == 0)
					plx_err(P->cx, se->line, "query(...) can only be iterated with foreach");
				c = se->rhs;
			}
			else if (se->rhs->empty_coll)
			{
				if (l->ty.k != TY_ARRAY || !l->ty.sql)
					plx_err(P->cx, se->line, "an empty collection needs a declared array type");
				c = nd_text(P, psprintf("ARRAY[]::%s", l->ty.sql), PR_CAST, l->ty);
			}
			else
			{
				nd_use(P, se->rhs);
				c = se->rhs;
			}
			break;
		case 1:
			nd_use(P, se->rhs);
			if (strcmp(se->op, "+") == 0 && l->k == N_IDENT)
			{
				CsSym	   *s = sym_find(P, l->text, (int) strlen(l->text));

				/* string += x on a local: the engine's string builder */
				if (s && s->ty.k == TY_STRING)
				{
					out_s(P, " ");
					out_lvalue(P, l);
					out_s(P, " += ");
					out_node(P, se->rhs);
					return;
				}
			}
			c = bin_step(P, l, se->op, se->rhs);
			break;
		case 2:
			if (ty_stringish(l->ty))
				plx_err(P->cx, se->line, "++ and -- need a number");
			c = bin_step(P, l, se->inc ? "+" : "-", nd_text(P, "1", PR_ATOM, TY_INTEGER));
			break;
		default:
			nd_use(P, se->rhs);
			c = tpl(P, PR_ATOM, l->ty, "COALESCE(%1, %2)", l, se->rhs);
			break;
	}
	out_s(P, " ");
	out_lvalue(P, l);
	out_s(P, " = ");
	out_node(P, c);
}

static void
emit_let(CsP *P, const char *name, CsNode *init, const char *sqlt, bool is_const)
{
	out_s(P, " let ");
	out_s(P, name);
	if (init)
	{
		out_s(P, " = ");
		/* parenthesized so the engine does not fold it into DECLARE, where the
		 * values it interpolates are not assigned yet */
		if (init->k == N_FSTR)
			out_s(P, "(");
		out_node(P, init);
		if (init->k == N_FSTR)
			out_s(P, ")");
	}
	if (sqlt)
		appendStringInfo(P->o, " /*:: %s%s */", sqlt, is_const ? " const" : "");
	out_s(P, ";");
}

static void
decl_init(CsP *P, CsTok *nt, CsType *ti, CsNode *init, bool is_const)
{
	char	   *name = pnstrdup(nt->s, nt->len);

	if (init->empty_coll)
	{
		if (ti->ty.k != TY_ARRAY || !ti->ty.sql)
			perr(P, "an empty collection needs a declared array type (int[] a = [];)");
		init = nd_text(P, psprintf("ARRAY[]::%s", ti->ty.sql), PR_CAST, ti->ty);
	}
	if (init->intrin)
	{
		const char *f = init->intrin;
		bool		typed = !ti->is_var &&
			!(ti->ty.sql && pg_strcasecmp(ti->ty.sql, "record") == 0);

		if (is_const)
			perr(P, "a const cannot hold the result of %s(...)", f);
		if (strcmp(f, "query") == 0)
			perr(P, "query(...) can only be iterated with foreach");
		if (strcmp(f, "open_cursor") == 0)
		{
			sym_declare(P, nt, mkty(TY_OTHER, "refcursor"), false, false);
			emit_let(P, name, init, NULL, false);
			return;
		}
		if (typed)
		{
			/* a typed target: declared first, then filled (see s2_typed_fetch) */
			sym_declare(P, nt, ti->ty, false, false);
			emit_let(P, name, NULL, ti->ty.sql, false);
			out_s(P, " ");
			out_s(P, name);
			out_s(P, " = ");
			out_node(P, init);
			out_s(P, ";");
			return;
		}
		sym_declare(P, nt, strcmp(f, "row_count") == 0 ? mkty(TY_INT, "bigint")
					: mkty(TY_RECORD, NULL), false, false);
		emit_let(P, name, init, NULL, false);
		return;
	}
	nd_use(P, init);
	if (ti->is_var)
	{
		CsTy		t = init->ty;

		if (t.k == TY_NULL)
			perr(P, "cannot infer a type from null; declare '%s' with a type", name);
		if (t.k == TY_RECORD && !t.sql)
			t = mkty(TY_RECORD, "record");
		if (!t.sql)
			perr(P, "cannot infer the type of '%s'; declare it with a type, for example int %s = ...",
				 name, name);
		sym_declare(P, nt, t, false, false);
		emit_let(P, name, init, t.sql, false);
		return;
	}
	sym_declare(P, nt, ti->ty, is_const, false);
	emit_let(P, name, init, ti->ty.sql, is_const);
}

static void
decl_sb(CsP *P, CsTok *nt)
{
	CsNode	   *init = NULL;
	char	   *name = pnstrdup(nt->s, nt->len);

	if (op_is(cur(P), "="))
	{
		CsArgs		a;

		advance(P);
		if (!kw_is(cur(P), "new"))
			perr(P, "a StringBuilder is created with new StringBuilder()");
		advance(P);
		while (cur(P)->kind == CT_ID && op_is(peek(P, 1), "."))
		{
			advance(P);
			advance(P);
		}
		if (!name_is(cur(P), "StringBuilder"))
			perr(P, "a StringBuilder is created with new StringBuilder()");
		advance(P);
		parse_args(P, &a);
		args_positional(P, &a, "new StringBuilder");
		if (a.n > 1)
			perr(P, "new StringBuilder(...) takes at most one argument here");
		if (a.n == 1 && a.a[0]->ty.k != TY_INT)
			init = a.a[0];
	}
	sym_declare(P, nt, TY_TEXT, false, true);
	if (!P->appended)
		P->appended = cs_hash_new("plxcsharp builders");
	cs_hash_put(P->appended, nt->s, nt->len, (void *) 1);
	if (!init)
		init = nd_str(P, "", 0, TY_STRING);
	else
		nd_use(P, init);
	emit_let(P, name, init, "text", false);
}

/* is the statement at P->pos a local declaration? */
static bool
looks_like_decl(CsP *P)
{
	CsTok	   *t = cur(P);
	int			e;
	CsTok	   *n;

	if (kw_is(t, "var") && peek(P, 1)->kind == CT_ID && !is_reserved(peek(P, 1)))
		return true;
	if (t->kind == CT_ID && op_is(peek(P, 1), "<") && peek(P, 2)->kind == CT_ID &&
		(op_is(peek(P, 3), ">") || op_is(peek(P, 3), ",") || op_is(peek(P, 3), ">>")))
		perr(P, "generic types are not supported");
	e = type_end(P, P->pos);
	if (e < 0)
		return false;
	n = tok_at(P, e);
	if (n->kind == CT_ID && !is_reserved(n))
	{
		CsTok	   *nn = tok_at(P, e + 1);

		if (op_is(nn, "=") || op_is(nn, ";") || op_is(nn, ","))
			return true;
		if (op_is(nn, "("))
			perr(P, "local functions are not supported");
		if (kw_is(nn, "in"))
			return false;
	}
	return false;
}

static void
st_decl(CsP *P, bool is_const)
{
	CsType		ti;

	if (is_const)
		advance(P);
	parse_type(P, &ti);
	if (is_const && (ti.is_var || ti.is_sb))
		perr(P, "a const needs a type such as int or string");
	for (;;)
	{
		CsTok	   *nt = cur(P);

		if (nt->kind != CT_ID || is_reserved(nt))
			perr(P, "expected a variable name near %s", tokdesc(nt));
		check_local_name(P, nt);
		advance(P);
		if (ti.is_sb || (ti.is_var && op_is(cur(P), "=") && kw_is(peek(P, 1), "new") &&
						 name_is(peek(P, 2), "StringBuilder")))
			decl_sb(P, nt);
		else if (op_is(cur(P), "="))
		{
			CsNode	   *init;

			advance(P);
			if (op_is(cur(P), "{"))
			{
				if (ti.ty.k != TY_ARRAY || !ti.ty.sql)
					perr(P, "an array initializer {...} needs an array type (int[] a = {1, 2};)");
				init = array_lit(P, "{", "}", pnstrdup(ti.ty.sql, (int) strlen(ti.ty.sql) - 2));
			}
			else
				init = parse_expr(P);
			decl_init(P, nt, &ti, init, is_const);
		}
		else
		{
			if (ti.is_var)
				perr(P, "var needs an initializer");
			if (is_const)
				perr(P, "a const needs a value");
			sym_declare(P, nt, ti.ty, false, false);
			emit_let(P, pnstrdup(nt->s, nt->len), NULL, ti.ty.sql ? ti.ty.sql : "record", false);
		}
		if (!op_is(cur(P), ","))
			break;
		advance(P);
	}
	expect(P, ";");
}

static void
st_args_out(CsP *P, CsArgs *a)
{
	int			i;

	for (i = 0; i < a->n; i++)
	{
		if (i)
			out_s(P, ", ");
		nd_use(P, a->a[i]);
		out_node(P, a->a[i]);
	}
}

static const char *const stmt_intrinsics[] = {
	"execute", "perform", "return_query", "raise", "call", "commit", "rollback",
	"close_cursor", "move_cursor", "emit", "return_next", "assert", NULL
};

static void
st_intrinsic(CsP *P, const char *name)
{
	CsArgs		a;

	advance(P);
	parse_args(P, &a);
	args_positional(P, &a, name);
	if ((strcmp(name, "emit") == 0 || strcmp(name, "return_next") == 0) && !P->retset)
		perr(P, "%s(...) is only valid in a set-returning function", name);
	if (strcmp(name, "assert") == 0 && (a.n < 1 || a.n > 2))
		perr(P, "assert takes a condition and an optional message");
	out_s(P, " ");
	out_s(P, name);
	out_s(P, "(");
	st_args_out(P, &a);
	out_s(P, ");");
	expect(P, ";");
}

static void
st_console(CsP *P)
{
	CsArgs		a;

	advance(P);
	advance(P);
	advance(P);
	parse_args(P, &a);
	args_positional(P, &a, "Console.WriteLine");
	out_s(P, " raise(\"notice\", ");
	if (a.n == 0)
		out_s(P, "\"\"");
	else if (a.n == 1)
	{
		nd_use(P, a.a[0]);
		out_node(P, a.a[0]);
	}
	else
		out_node(P, format_node(P, a.a[0], a.a + 1, a.n - 1));
	out_s(P, ");");
	expect(P, ";");
}

static void
st_debug_assert(CsP *P)
{
	CsArgs		a;

	advance(P);
	advance(P);
	advance(P);
	parse_args(P, &a);
	args_positional(P, &a, "Debug.Assert");
	if (a.n < 1 || a.n > 2)
		perr(P, "Debug.Assert takes a condition and an optional message");
	out_s(P, " assert(");
	st_args_out(P, &a);
	out_s(P, ");");
	expect(P, ";");
}

/* sb.Append(x).AppendLine(y)...; */
static void
st_builder(CsP *P, CsSym *s)
{
	advance(P);
	while (op_is(cur(P), "."))
	{
		CsTok	   *m;
		CsArgs		a;

		advance(P);
		m = cur(P);
		if (m->kind != CT_ID)
			perr(P, "expected a StringBuilder method");
		advance(P);
		parse_args(P, &a);
		args_positional(P, &a, "StringBuilder methods");
		note_asg(P, s->name);
		if (mname_is(m, "Append"))
		{
			if (a.n != 1)
				perr(P, "Append takes one argument here");
			nd_use(P, a.a[0]);
			appendStringInfo(P->o, " %s += ", s->name);
			out_node(P, a.a[0]);
			out_s(P, ";");
		}
		else if (mname_is(m, "AppendLine"))
		{
			if (a.n > 1)
				perr(P, "AppendLine takes at most one argument");
			if (a.n == 1)
			{
				nd_use(P, a.a[0]);
				appendStringInfo(P->o, " %s += ", s->name);
				out_node(P, a.a[0]);
				out_s(P, ";");
			}
			appendStringInfo(P->o, " %s += ", s->name);
			out_node(P, nd_str(P, "\n", 1, TY_STRING));
			out_s(P, ";");
		}
		else if (mname_is(m, "AppendFormat"))
		{
			if (a.n < 1)
				perr(P, "AppendFormat needs a format string");
			appendStringInfo(P->o, " %s += ", s->name);
			out_node(P, format_node(P, a.a[0], a.a + 1, a.n - 1));
			out_s(P, ";");
		}
		else if (mname_is(m, "Clear"))
		{
			if (a.n != 0)
				perr(P, "Clear takes no arguments");
			appendStringInfo(P->o, " %s = \"\";", s->name);
		}
		else
			perr(P, "StringBuilder.%.*s is not supported (Append, AppendLine, AppendFormat, Clear, ToString and Length are)",
				 m->len, m->s);
	}
	expect(P, ";");
}

static void
st_exprstmt(CsP *P)
{
	CsTok	   *t = cur(P);
	CsTok	   *t1 = peek(P, 1);
	CsSE		se;
	int			i;

	if (t->kind == CT_ID && !t->atid && op_is(t1, "("))
		for (i = 0; stmt_intrinsics[i]; i++)
			if (name_is(t, stmt_intrinsics[i]))
			{
				st_intrinsic(P, stmt_intrinsics[i]);
				return;
			}
	if (name_is(t, "Console") && op_is(t1, ".") && op_is(peek(P, 3), "(") &&
		(name_is(peek(P, 2), "WriteLine") || name_is(peek(P, 2), "Write")) &&
		!sym_find(P, t->s, t->len))
	{
		st_console(P);
		return;
	}
	if (name_is(t, "Debug") && op_is(t1, ".") && name_is(peek(P, 2), "Assert") &&
		op_is(peek(P, 3), "(") && !sym_find(P, t->s, t->len))
	{
		st_debug_assert(P);
		return;
	}
	if (t->kind == CT_ID && op_is(t1, ".") && peek(P, 2)->kind == CT_ID &&
		op_is(peek(P, 3), "("))
	{
		CsSym	   *s = sym_find(P, t->s, t->len);

		if (s && s->is_sb)
		{
			st_builder(P, s);
			return;
		}
	}
	if (!parse_se(P, &se))
		perr(P, "only assignments, ++, --, and the plx statement intrinsics can be used as statements (to call a SQL function for its side effects, use perform(\"SELECT f(...)\"))");
	render_se(P, &se);
	out_s(P, ";");
	expect(P, ";");
}

static void
st_if(CsP *P)
{
	for (;;)
	{
		CsNode	   *c;

		advance(P);				/* if */
		c = parse_cond_paren(P);
		out_s(P, " if (");
		out_node(P, c);
		out_s(P, ") ");
		st_embedded(P);
		if (!kw_is(cur(P), "else"))
			break;
		out_sync(P, cur(P)->line);
		advance(P);
		out_s(P, " else");
		if (!kw_is(cur(P), "if"))
		{
			out_s(P, " ");
			st_embedded(P);
			break;
		}
		/* else if: continue the chain without recursion */
	}
}

static void
st_while(CsP *P)
{
	CsNode	   *c;

	advance(P);
	c = parse_cond_paren(P);
	out_s(P, " while (");
	out_node(P, c);
	out_s(P, ") ");
	st_embedded(P);
}

static void
st_do(CsP *P)
{
	CsNode	   *c;

	advance(P);
	out_s(P, " do ");
	st_embedded(P);
	if (!kw_is(cur(P), "while"))
		perr(P, "expected 'while' after the body of do");
	out_sync(P, cur(P)->line);
	advance(P);
	c = parse_cond_paren(P);
	expect(P, ";");
	out_s(P, " while (");
	out_node(P, c);
	out_s(P, ");");
}

static bool
assigned_since(CsP *P, int mark, const char *name)
{
	void	   *v = P->asg ? cs_hash_get(P->asg, name, (int) strlen(name)) : NULL;

	return v && (int) (intptr_t) v > mark;
}

/* does a variable named in n get assigned after mark? */
static bool
idents_assigned(CsP *P, int mark, CsNode *n)
{
	int			i;

	check_stack_depth();
	if (n->k == N_IDENT && assigned_since(P, mark, n->text))
		return true;
	for (i = 0; i < n->nitems; i++)
		if (n->items[i].kid && idents_assigned(P, mark, n->items[i].kid))
			return true;
	return false;
}

/*
 * for (init; cond; iter) body. A plain counting loop over an int becomes a
 * plpgsql integer FOR (normalized as "for (i = LO; i < HI; i += K)"); any
 * other shape becomes the general form "__plx_for (cond; iter, ...)" with
 * its initializers emitted before it, which stage 2 lowers so that continue
 * still runs the iterators.
 */
static void
st_for(CsP *P)
{
	StringInfoData pre,
				body;
	StringInfo	saved;
	CsTok	   *ivar = NULL;
	CsNode	   *ilo = NULL;
	CsType		iti;
	int			ndecl = 0;
	bool		init_decl = false;
	CsNode	   *cond = NULL;
	CsSE	   *its = NULL;
	int			nits = 0,
				itcap = 0,
				mark,
				i;

	memset(&iti, 0, sizeof(iti));
	initStringInfo(&pre);
	initStringInfo(&body);
	advance(P);
	expect(P, "(");
	saved = P->o;
	P->o = &pre;
	if (!op_is(cur(P), ";"))
	{
		if (looks_like_decl(P))
		{
			init_decl = true;
			parse_type(P, &iti);
			if (iti.is_sb)
				perr(P, "a StringBuilder cannot be declared in a for header");
			for (;;)
			{
				CsTok	   *nt = cur(P);
				CsNode	   *init;

				if (nt->kind != CT_ID || is_reserved(nt))
					perr(P, "expected a loop variable name");
				check_local_name(P, nt);
				advance(P);
				expect(P, "=");
				init = parse_expr(P);
				if (init->intrin)
					nd_use(P, init);
				decl_init(P, nt, &iti, init, false);
				if (++ndecl == 1)
				{
					ivar = nt;
					ilo = init;
				}
				if (!op_is(cur(P), ","))
					break;
				advance(P);
			}
		}
		else
			for (;;)
			{
				CsSE		se;

				if (!parse_se(P, &se))
					perr(P, "a for initializer must be a declaration or an assignment");
				render_se(P, &se);
				out_s(P, ";");
				if (!op_is(cur(P), ","))
					break;
				advance(P);
			}
	}
	P->o = saved;
	expect(P, ";");
	if (!op_is(cur(P), ";"))
	{
		cond = parse_expr(P);
		nd_use(P, cond);
		check_cond(P, cond);
	}
	expect(P, ";");
	while (!op_is(cur(P), ")"))
	{
		if (nits >= itcap)
		{
			itcap = itcap ? itcap * 2 : 4;
			its = its ? repalloc(its, sizeof(CsSE) * itcap) : palloc(sizeof(CsSE) * itcap);
		}
		if (!parse_se(P, &its[nits]))
			perr(P, "a for iterator must be an assignment, ++ or --");
		nits++;
		if (!op_is(cur(P), ","))
			break;
		advance(P);
	}
	expect(P, ")");
	mark = P->nasg;
	P->o = &body;
	st_embedded(P);
	P->o = saved;

	if (init_decl && ndecl == 1 && cond && nits == 1)
	{
		CsSym	   *vs = sym_find(P, ivar->s, ivar->len);
		CsSE	   *it = &its[0];
		const char *cmp = NULL;
		CsNode	   *hi = NULL;
		const char *k = NULL;
		bool		up = false;

		if (vs && vs->ty.k == TY_INT && vs->ty.sql &&
			(strcmp(vs->ty.sql, "integer") == 0 || strcmp(vs->ty.sql, "smallint") == 0) &&
			cond->k == N_CHAIN && cond->nitems == 2 &&
			cond->items[0].kid->k == N_IDENT &&
			strcmp(cond->items[0].kid->text, vs->name) == 0)
		{
			cmp = cond->items[1].text;
			hi = cond->items[1].kid;
			if (strcmp(cmp, "<") != 0 && strcmp(cmp, "<=") != 0 &&
				strcmp(cmp, ">") != 0 && strcmp(cmp, ">=") != 0)
				cmp = NULL;
		}
		if (cmp && it->lhs->k == N_IDENT && strcmp(it->lhs->text, vs->name) == 0)
		{
			if (it->k == 2)
			{
				k = "1";
				up = it->inc;
			}
			else if (it->k == 1 && (strcmp(it->op, "+") == 0 || strcmp(it->op, "-") == 0) &&
					 it->rhs->k == N_TEXT && it->rhs->intlit && strcmp(it->rhs->text, "0") != 0)
			{
				k = it->rhs->text;
				up = (it->op[0] == '+');
			}
		}
		if (k && ((up && cmp[0] == '<') || (!up && cmp[0] == '>')) && !hi->has_call &&
			!assigned_since(P, mark, vs->name) && !idents_assigned(P, mark, hi))
		{
			appendStringInfo(P->o, " for (%s = ", vs->name);
			out_node(P, ilo);
			appendStringInfo(P->o, "; %s %s ", vs->name, cmp);
			out_node(P, hi);
			appendStringInfo(P->o, "; %s %s %s) ", vs->name, up ? "+=" : "-=", k);
			appendBinaryStringInfo(P->o, body.data, body.len);
			return;
		}
	}
	appendBinaryStringInfo(P->o, pre.data, pre.len);
	out_s(P, " __plx_for (");
	if (cond)
		out_node(P, cond);
	else
		out_s(P, "true");
	out_s(P, ";");
	for (i = 0; i < nits; i++)
	{
		if (i)
			out_s(P, ",");
		render_se(P, &its[i]);
	}
	out_s(P, ") ");
	appendBinaryStringInfo(P->o, body.data, body.len);
}

static void
st_foreach(CsP *P)
{
	CsType		ti;
	CsTok	   *nt;
	CsNode	   *e;
	char	   *name;

	advance(P);
	expect(P, "(");
	if (kw_is(cur(P), "var") && op_is(peek(P, 1), "("))
		perr(P, "deconstruction in foreach is not supported");
	parse_type(P, &ti);
	if (ti.is_sb)
		perr(P, "a StringBuilder cannot be a foreach variable");
	nt = cur(P);
	if (nt->kind != CT_ID || is_reserved(nt))
		perr(P, "expected a loop variable name");
	check_local_name(P, nt);
	advance(P);
	if (!kw_is(cur(P), "in"))
		perr(P, "expected 'in' in foreach");
	advance(P);
	e = parse_expr(P);
	expect(P, ")");
	name = pnstrdup(nt->s, nt->len);
	if (e->intrin && strcmp(e->intrin, "query") == 0)
	{
		if (!ti.is_var && ti.ty.sql && pg_strcasecmp(ti.ty.sql, "record") != 0)
		{
			sym_declare(P, nt, ti.ty, false, false);
			emit_let(P, name, NULL, ti.ty.sql, false);
		}
		else
			sym_declare(P, nt, mkty(TY_RECORD, NULL), false, false);
	}
	else
	{
		CsTy		et;

		nd_use(P, e);
		if (ty_stringish(e->ty))
		{
			e = tpl(P, PR_ATOM, mkty(TY_ARRAY, "text[]"), "string_to_array(%1, NULL)", e);
			et = mkty(TY_CHAR, "text");
		}
		else
			et = ty_elem(e->ty);
		if (!ti.is_var)
			et = ti.ty;
		if (!et.sql)
			perr(P, "foreach over this value needs an explicit element type, for example foreach (int v in arr)");
		sym_declare(P, nt, et, false, false);
		emit_let(P, name, NULL, et.sql, false);
	}
	appendStringInfo(P->o, " foreach (%s in ", name);
	out_node(P, e);
	out_s(P, ") ");
	st_embedded(P);
}

static void
st_switch(CsP *P)
{
	CsNode	   *subj;
	bool		labelled = false;

	advance(P);
	expect(P, "(");
	subj = parse_expr(P);
	nd_use(P, subj);
	expect(P, ")");
	expect(P, "{");
	out_s(P, " switch (");
	out_node(P, subj);
	out_s(P, ") {");
	while (!op_is(cur(P), "}"))
	{
		CsTok	   *t = cur(P);

		if (t->kind == CT_EOF)
			perr(P, "unterminated switch block");
		out_sync(P, t->line);
		if (kw_is(t, "case"))
		{
			CsNode	   *v;
			CsTok	   *n = peek(P, 1);

			advance(P);
			if (kw_is(n, "null"))
				perr(P, "case null is not supported; test x == null before the switch");
			if (op_is(n, "<") || op_is(n, "<=") || op_is(n, ">") || op_is(n, ">="))
				perr(P, "relational case patterns are not supported; use if/else");
			if (kw_is(n, "var") || name_is(n, "not") ||
				(n->kind == CT_ID && peek(P, 1)->kind == CT_ID && !kw_is(peek(P, 1), "when")))
				perr(P, "type patterns in case labels are not supported");
			v = parse_expr(P);
			nd_use(P, v);
			if (name_is(cur(P), "when"))
				perr(P, "case guards (when) are not supported");
			if (name_is(cur(P), "or") || name_is(cur(P), "and"))
				perr(P, "pattern combinators (or, and) are not supported; stack case labels instead");
			expect(P, ":");
			out_s(P, " case ");
			out_node(P, v);
			out_s(P, " :");
			labelled = true;
		}
		else if (kw_is(t, "default"))
		{
			advance(P);
			expect(P, ":");
			out_s(P, " default :");
			labelled = true;
		}
		else
		{
			if (!labelled)
				perr(P, "a switch section must start with a case or default label");
			st_stmt(P);
		}
	}
	out_sync(P, cur(P)->line);
	out_s(P, " }");
	advance(P);
}

static const struct
{
	const char *cls;
	const char *cond;
}			cs_excmap[] = {
	{"Exception", "OTHERS"}, {"SystemException", "OTHERS"},
	{"PostgresException", "OTHERS"}, {"NpgsqlException", "OTHERS"},
	{"DbException", "OTHERS"},
	{"DivideByZeroException", "division_by_zero"},
	{"OverflowException", "numeric_value_out_of_range"},
	{"FormatException", "invalid_text_representation"},
	{"ArgumentNullException", "null_value_not_allowed"},
};

static const char *
exc_class_cond(CsP *P, const char *s, int len, bool for_catch)
{
	int			i;

	for (i = 0; i < (int) lengthof(cs_excmap); i++)
		if ((int) strlen(cs_excmap[i].cls) == len && memcmp(cs_excmap[i].cls, s, len) == 0)
			return cs_excmap[i].cond;
	if (for_catch)
		perr(P, "catch (%.*s) is not supported; catch Exception or PostgresException (optionally with a when filter on e.SqlState), or one of the mapped exception types",
			 len, s);
	if (len >= 9 && memcmp(s + len - 9, "Exception", 9) == 0)
		return "OTHERS";
	perr(P, "throw needs an exception type (a name ending in Exception)");
	return NULL;
}

/* read a (dotted) type name; return its last component */
static CsTok *
read_class(CsP *P)
{
	CsTok	   *last = cur(P);

	if (last->kind != CT_ID)
		perr(P, "expected an exception type near %s", tokdesc(last));
	advance(P);
	while (op_is(cur(P), ".") && peek(P, 1)->kind == CT_ID)
	{
		advance(P);
		last = cur(P);
		advance(P);
	}
	if (op_is(cur(P), "<"))
		perr(P, "generic types are not supported");
	return last;
}

static void
append_snake(StringInfo out, const char *s, int len)
{
	int			i;

	for (i = 0; i < len; i++)
	{
		char		c = s[i];

		if (c >= 'A' && c <= 'Z')
		{
			if (i > 0 && ((s[i - 1] >= 'a' && s[i - 1] <= 'z') ||
						  (s[i - 1] >= '0' && s[i - 1] <= '9') ||
						  (s[i - 1] >= 'A' && s[i - 1] <= 'Z' && i + 1 < len &&
						   s[i + 1] >= 'a' && s[i + 1] <= 'z')))
				appendStringInfoChar(out, '_');
			appendStringInfoChar(out, (char) (c - 'A' + 'a'));
		}
		else
			appendStringInfoChar(out, c);
	}
}

static bool
sqlstate_ref(CsP *P, CsTok *var)
{
	CsTok	   *t = cur(P);

	return t->kind == CT_ID && t->len == var->len && memcmp(t->s, var->s, var->len) == 0 &&
		op_is(peek(P, 1), ".") && name_is(peek(P, 2), "SqlState");
}

static void
filter_code(CsP *P, StringInfo out)
{
	CsTok	   *t = cur(P);

	if (t->kind == CT_STR)
	{
		int			i;

		if (t->vlen != 5)
			perr(P, "a SQLSTATE code has five characters");
		for (i = 0; i < 5; i++)
			if (!((t->val[i] >= '0' && t->val[i] <= '9') || (t->val[i] >= 'A' && t->val[i] <= 'Z')))
				perr(P, "a SQLSTATE code has five digits or capital letters");
		appendStringInfo(out, "SQLSTATE '%s'", t->val);
		advance(P);
		return;
	}
	if (name_is(t, "PostgresErrorCodes") && op_is(peek(P, 1), ".") && peek(P, 2)->kind == CT_ID)
	{
		append_snake(out, peek(P, 2)->s, peek(P, 2)->len);
		advance(P);
		advance(P);
		advance(P);
		return;
	}
	perr(P, "a catch filter compares e.SqlState with a five-character SQLSTATE string or a PostgresErrorCodes name");
}

static void filter_or(CsP *P, CsTok *var, StringInfo out);

static void
filter_term(CsP *P, CsTok *var, StringInfo out)
{
	enter(P);
	if (op_is(cur(P), "("))
	{
		advance(P);
		filter_or(P, var, out);
		expect(P, ")");
	}
	else if (sqlstate_ref(P, var))
	{
		advance(P);
		advance(P);
		advance(P);
		if (op_is(cur(P), "=="))
		{
			advance(P);
			filter_code(P, out);
		}
		else if (kw_is(cur(P), "is"))
		{
			advance(P);
			filter_code(P, out);
			while (name_is(cur(P), "or"))
			{
				advance(P);
				appendStringInfoString(out, " OR ");
				filter_code(P, out);
			}
		}
		else
			perr(P, "a catch filter compares e.SqlState with == or is");
	}
	else
	{
		filter_code(P, out);
		expect(P, "==");
		if (!sqlstate_ref(P, var))
			perr(P, "a catch filter compares e.SqlState with a SQLSTATE code");
		advance(P);
		advance(P);
		advance(P);
	}
	leave(P);
}

static void
filter_or(CsP *P, CsTok *var, StringInfo out)
{
	filter_term(P, var, out);
	while (op_is(cur(P), "||"))
	{
		advance(P);
		appendStringInfoString(out, " OR ");
		filter_term(P, var, out);
	}
}

static void
st_try(CsP *P)
{
	int			ncatch = 0;
	bool		fin = false;
	bool		general = false;

	advance(P);
	if (!op_is(cur(P), "{"))
		perr(P, "expected '{' after try");
	out_s(P, " try ");
	st_block(P);
	while (kw_is(cur(P), "catch"))
	{
		CsTok	   *var = NULL;
		const char *cond = "OTHERS";
		bool		filtered = false;
		CsCatch		cf;

		out_sync(P, cur(P)->line);
		advance(P);
		if (general)
			perr(P, "a catch clause after a general catch can never run");
		if (op_is(cur(P), "("))
		{
			CsTok	   *cls;

			advance(P);
			cls = read_class(P);
			if (cur(P)->kind == CT_ID)
			{
				var = cur(P);
				check_name(P, var);
				advance(P);
			}
			expect(P, ")");
			cond = exc_class_cond(P, cls->s, cls->len, true);
		}
		if (name_is(cur(P), "when"))
		{
			StringInfoData fo;

			if (strcmp(cond, "OTHERS") != 0)
				perr(P, "a when filter is only supported on catch (Exception e) or catch (PostgresException e)");
			if (!var)
				perr(P, "a catch filter needs a named exception variable: catch (PostgresException e) when (e.SqlState == \"23505\")");
			advance(P);
			initStringInfo(&fo);
			expect(P, "(");
			filter_or(P, var, &fo);
			expect(P, ")");
			cond = fo.data;
			filtered = true;
		}
		if (!filtered && strcmp(cond, "OTHERS") == 0)
			general = true;
		appendStringInfo(P->o, " catch (\"%s\"", cond);
		if (var)
			appendStringInfo(P->o, " %.*s", var->len, var->s);
		out_s(P, ") ");
		cf.var = var ? var->s : NULL;
		cf.len = var ? var->len : 0;
		cf.outer = P->catches;
		P->catches = &cf;
		if (!op_is(cur(P), "{"))
			perr(P, "expected '{' after catch");
		st_block(P);
		P->catches = cf.outer;
		ncatch++;
	}
	if (kw_is(cur(P), "finally"))
	{
		out_sync(P, cur(P)->line);
		advance(P);
		if (!op_is(cur(P), "{"))
			perr(P, "expected '{' after finally");
		out_s(P, " finally ");
		st_block(P);
		fin = true;
	}
	if (!ncatch && !fin)
		perr(P, "try needs a catch or a finally");
}

static void
st_throw(CsP *P)
{
	CsTok	   *cls;
	CsNode	   *slot[6] = {NULL, NULL, NULL, NULL, NULL, NULL};
	bool		inner = false;
	int			npos = 0,
				i;
	bool		pgexc;
	const char *cond = NULL;

	advance(P);
	if (op_is(cur(P), ";"))
	{
		if (!P->catches)
			perr(P, "throw; (rethrow) is only valid inside a catch block");
		advance(P);
		out_s(P, " throw;");
		return;
	}
	if (P->catches && P->catches->var && cur(P)->kind == CT_ID &&
		cur(P)->len == P->catches->len && memcmp(cur(P)->s, P->catches->var, cur(P)->len) == 0 &&
		op_is(peek(P, 1), ";"))
	{
		advance(P);
		advance(P);
		out_s(P, " throw;");
		return;
	}
	if (!kw_is(cur(P), "new"))
		perr(P, "throw needs new SomeException(...), or a bare throw; inside a catch block");
	advance(P);
	cls = read_class(P);
	pgexc = name_is(cls, "PostgresException");
	if (!pgexc)
	{
		cond = exc_class_cond(P, cls->s, cls->len, false);
		if (strcmp(cond, "OTHERS") == 0)
			cond = NULL;
	}
	expect(P, "(");
	while (!op_is(cur(P), ")"))
	{
		int			slotno = npos;

		CHECK_FOR_INTERRUPTS();
		if (cur(P)->kind == CT_ID && op_is(peek(P, 1), ":"))
		{
			static const char *const names[] = {
				"messageText", "severity", "invariantSeverity", "sqlState", "detail", "hint"
			};

			if (!pgexc)
				perr(P, "named arguments are only supported for PostgresException");
			for (slotno = 0; slotno < 6; slotno++)
				if (name_is(cur(P), names[slotno]))
					break;
			if (slotno == 6)
				perr(P, "unknown PostgresException argument '%.*s'", cur(P)->len, cur(P)->s);
			advance(P);
			advance(P);
		}
		else
			npos++;
		if (slotno >= 6)
			perr(P, "too many arguments for the exception");
		if (!pgexc && slotno == 1 && P->catches && P->catches->var &&
			cur(P)->kind == CT_ID && cur(P)->len == P->catches->len &&
			memcmp(cur(P)->s, P->catches->var, cur(P)->len) == 0 &&
			(op_is(peek(P, 1), ")") || op_is(peek(P, 1), ",")))
		{
			/* new XException(message, e): the caught error becomes the detail */
			inner = true;
			advance(P);
		}
		else
		{
			if (!pgexc && slotno >= 1)
				perr(P, "an exception takes a message and, inside a catch, the caught exception (new Exception(message, e))");
			slot[slotno] = parse_expr(P);
			nd_use(P, slot[slotno]);
		}
		if (!op_is(cur(P), ","))
			break;
		advance(P);
	}
	expect(P, ")");
	expect(P, ";");
	if (pgexc && !slot[3])
		perr(P, "new PostgresException(...) needs its sqlState argument (messageText, severity, invariantSeverity, sqlState)");
	for (i = 4; i < 6; i++)
		if (slot[i] && slot[i]->is_null)
			slot[i] = NULL;
	out_s(P, " throw(");
	if (pgexc)
		out_node(P, slot[3]);
	else if (cond)
		out_node(P, nd_str(P, cond, (int) strlen(cond), TY_STRING));
	else
		out_s(P, "null");
	out_s(P, ", ");
	if (slot[0])
		out_node(P, slot[0]);
	else
		out_node(P, nd_str(P, cls->s, cls->len, TY_STRING));
	out_s(P, ", ");
	if (inner)
		appendStringInfo(P->o, "%.*s.message", P->catches->len, P->catches->var);
	else if (slot[4])
		out_node(P, slot[4]);
	else
		out_s(P, "null");
	out_s(P, ", ");
	if (slot[5])
		out_node(P, slot[5]);
	else
		out_s(P, "null");
	out_s(P, ");");
}

static void
st_return(CsP *P)
{
	advance(P);
	if (op_is(cur(P), ";"))
	{
		advance(P);
		out_s(P, " return;");
		return;
	}
	if (P->retset)
		perr(P, "a set-returning function produces rows with yield return; end it with a bare return;");
	{
		CsNode	   *e = parse_expr(P);

		nd_use(P, e);
		expect(P, ";");
		out_s(P, " return ");
		out_node(P, e);
		out_s(P, ";");
	}
}

static void
st_yield(CsP *P)
{
	advance(P);
	if (kw_is(cur(P), "return"))
	{
		CsNode	   *e;

		advance(P);
		if (!P->retset)
			perr(P, "yield return is only valid in a set-returning function (RETURNS SETOF ...)");
		e = parse_expr(P);
		nd_use(P, e);
		expect(P, ";");
		out_s(P, " emit ");
		out_node(P, e);
		out_s(P, ";");
		return;
	}
	if (kw_is(cur(P), "break"))
	{
		advance(P);
		expect(P, ";");
		out_s(P, " return;");
		return;
	}
	perr(P, "expected yield return or yield break");
}

static void
st_stmt(CsP *P)
{
	CsTok	   *t = cur(P);

	enter(P);
	out_sync(P, t->line);
	if (t->kind == CT_EOF)
		perr(P, "unexpected end of the function body");
	if (op_is(t, "{"))
	{
		out_s(P, " ");
		st_block(P);
		leave(P);
		return;
	}
	if (op_is(t, ";"))
	{
		advance(P);
		leave(P);
		return;
	}
	if (op_is(t, "}") || op_is(t, ")") || op_is(t, "]"))
		perr(P, "unexpected '%.*s'", t->len, t->s);
	if (t->kind == CT_ID && !t->atid)
	{
		static const char *const unsupported[] = {
			"goto", "lock", "using", "fixed", "unsafe", "checked", "unchecked",
			"static", "public", "private", "protected", "internal", "class",
			"struct", "enum", "interface", "namespace", "delegate", "event",
			"abstract", "sealed", "virtual", "override", "extern", "readonly",
			"volatile", "operator", "implicit", "explicit", "void", NULL
		};
		int			i;

		if (kw_is(t, "if"))
			st_if(P);
		else if (kw_is(t, "while"))
			st_while(P);
		else if (kw_is(t, "do"))
			st_do(P);
		else if (kw_is(t, "for"))
			st_for(P);
		else if (kw_is(t, "foreach"))
			st_foreach(P);
		else if (kw_is(t, "switch"))
			st_switch(P);
		else if (kw_is(t, "try"))
			st_try(P);
		else if (kw_is(t, "throw"))
			st_throw(P);
		else if (kw_is(t, "return"))
			st_return(P);
		else if (kw_is(t, "break") || kw_is(t, "continue"))
		{
			bool		br = kw_is(t, "break");

			advance(P);
			expect(P, ";");
			out_s(P, br ? " break;" : " continue;");
		}
		else if (name_is(t, "yield") && (kw_is(peek(P, 1), "return") || kw_is(peek(P, 1), "break")))
			st_yield(P);
		else if (kw_is(t, "const"))
			st_decl(P, true);
		else if (kw_is(t, "else"))
			perr(P, "else without a matching if");
		else if (kw_is(t, "case") || kw_is(t, "default"))
			perr(P, "a case label outside a switch");
		else if (kw_is(t, "catch") || kw_is(t, "finally"))
			perr(P, "%.*s without a matching try", t->len, t->s);
		else if (name_is(t, "async") || name_is(t, "await") || name_is(t, "record"))
			perr(P, "'%.*s' is not supported in plxcsharp", t->len, t->s);
		else
		{
			for (i = 0; unsupported[i]; i++)
				if (kw_is(t, unsupported[i]))
					perr(P, "'%s' is not supported in plxcsharp", unsupported[i]);
			goto simple;
		}
		leave(P);
		return;
	}
simple:
	if (t->kind == CT_ID && op_is(peek(P, 1), ":") && !is_reserved(t))
		perr(P, "labels and goto are not supported");
	if (looks_like_decl(P))
		st_decl(P, false);
	else
		st_exprstmt(P);
	leave(P);
}

/* names that are '+=' targets anywhere in the body (they become builders) */
static void
prescan_appends(CsP *P)
{
	int			i;

	for (i = 0; i + 1 < P->nt; i++)
		if (P->t[i].kind == CT_ID && op_is(&P->t[i + 1], "+=") &&
			(i == 0 || !op_is(&P->t[i - 1], ".")))
		{
			if (!P->appended)
				P->appended = cs_hash_new("plxcsharp builders");
			cs_hash_put(P->appended, P->t[i].s, P->t[i].len, (void *) 1);
		}
}

/* Stage 1: C# source -> the normalized program */
static char *
cs_stage1(Ctx *cx)
{
	CsP			P;
	CsLex		L;
	CsTokVec	v = {0};
	StringInfoData out;
	char		stop = 0;

	memset(&P, 0, sizeof(P));
	memset(&L, 0, sizeof(L));
	L.cx = cx;
	L.p = cx->body;
	L.end = cx->body + strlen(cx->body);
	L.line = 1;
	cs_lex_run(&L, &v, 0, &stop);

	P.cx = cx;
	P.t = v.t;
	P.nt = v.n;
	initStringInfo(&out);
	P.o = &out;
	P.out_line = 1;
	if (cx->meta)
	{
		P.retset = cx->meta->retset;
		P.trigger = (cx->meta->rettype == TRIGGEROID);
		P.evtrigger = (cx->meta->rettype == EVENT_TRIGGEROID);
	}
	prescan_appends(&P);
	while (cur(&P)->kind != CT_EOF)
		st_stmt(&P);
	appendStringInfoChar(&out, '\n');
	return out.data;
}

/* ======================================================================== */
/* Stage 2: the normalized program to plpgsql                               */
/* ======================================================================== */

/*
 * C# break leaves the innermost loop or switch; continue the innermost loop.
 * A frame per loop and switch tells break/continue where they go.
 */
typedef struct CsLoop
{
	bool		is_switch;
	int			id;				/* switch: block label __plx_sw_<id> */
	bool		cont_used;		/* a continue targets this loop */
	bool		brk_used;		/* switch: a nested break needs the block label */
	struct CsLoop *outer;
} CsLoop;

typedef struct
{
	CsLoop	   *loops;
	int			seq;
} CsS2;

static bool s2_stmt(Ctx *cx, CsS2 *S, int ind, bool top);

#define s2_cur(cx) (&(cx)->t[(cx)->pos])

static void
s2_skipnl(Ctx *cx)
{
	while (cx->t[cx->pos].kind == T_NEWLINE)
		cx->pos++;
}

static bool
s2_kw(Tok *t, const char *w)
{
	return t->kind == T_KW && plx_name_eq(t, w);
}

/* expects '(' at cx->pos; returns the inner range and leaves pos past ')' */
static void
s2_paren(Ctx *cx, int *a, int *b)
{
	int			p = cx->pos,
				depth = 1;

	if (cx->t[p].kind != T_LPAREN)
		plx_err(cx, cx->t[p].line, "expected '('");
	p++;
	*a = p;
	while (cx->t[p].kind != T_EOF)
	{
		if (cx->t[p].kind == T_LPAREN)
			depth++;
		else if (cx->t[p].kind == T_RPAREN && --depth == 0)
			break;
		p++;
	}
	if (cx->t[p].kind != T_RPAREN)
		plx_err(cx, cx->t[cx->pos].line, "unterminated '('");
	*b = p;
	cx->pos = p + 1;
}

/* first token of kind k at nesting depth 0 in [a, b), or b */
static int
s2_find(Ctx *cx, int a, int b, TokKind k)
{
	int			i,
				depth = 0;

	for (i = a; i < b; i++)
	{
		TokKind		tk = cx->t[i].kind;

		if (tk == T_LPAREN || tk == T_LBRACKET || tk == T_LBRACE)
			depth++;
		else if (tk == T_RPAREN || tk == T_RBRACKET || tk == T_RBRACE)
			depth--;
		else if (depth == 0 && tk == k)
			return i;
	}
	return b;
}

/* end of the leaf statement starting at a: the next top-level ';' */
static int
s2_stmt_end(Ctx *cx, int a)
{
	int			i = a,
				depth = 0;

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

static char *
s2_rw(Ctx *cx, int a, int b)
{
	return plx_rw_range(cx, a, b, false);
}

static bool
s2_block(Ctx *cx, CsS2 *S, int ind)
{
	bool		term = false;

	s2_skipnl(cx);
	if (s2_cur(cx)->kind != T_LBRACE)
		plx_err(cx, s2_cur(cx)->line, "expected '{'");
	cx->pos++;
	for (;;)
	{
		plx_skip_seps(cx);
		if (s2_cur(cx)->kind == T_RBRACE)
		{
			cx->pos++;
			return term;
		}
		if (s2_cur(cx)->kind == T_EOF)
			plx_err(cx, s2_cur(cx)->line, "unterminated '{' block");
		term = s2_stmt(cx, S, ind, false);
	}
}

/* parse a block into a separate buffer */
static char *
s2_block_text(Ctx *cx, CsS2 *S, int ind, bool *term)
{
	StringInfoData saved = cx->out;
	char	   *r;
	bool		t;

	initStringInfo(&cx->out);
	t = s2_block(cx, S, ind);
	r = cx->out.data;
	cx->out = saved;
	if (term)
		*term = t;
	return r;
}

/* a loop body, with its frame pushed */
static char *
s2_loop_body(Ctx *cx, CsS2 *S, int ind, CsLoop *fr)
{
	char	   *r;

	memset(fr, 0, sizeof(*fr));
	fr->outer = S->loops;
	S->loops = fr;
	cx->loopdepth++;
	r = s2_block_text(cx, S, ind, NULL);
	cx->loopdepth--;
	S->loops = fr->outer;
	return r;
}

static char *
s2_flag(Ctx *cx, CsS2 *S, const char *kind)
{
	char	   *nm = psprintf("__plx_%s_%d", kind, ++S->seq);
	PlxLocal2  *l = plx_local_add(cx, nm, (int) strlen(nm));

	l->typ = "boolean";
	return nm;
}

static bool
s2_if(Ctx *cx, CsS2 *S, int ind)
{
	StringInfo	o = &cx->out;
	int			a,
				b;
	bool		all,
				has_else = false;

	cx->pos++;
	s2_paren(cx, &a, &b);
	plx_indent(o, ind);
	appendStringInfo(o, "IF %s THEN\n", s2_rw(cx, a, b));
	all = s2_block(cx, S, ind + 1);
	for (;;)
	{
		s2_skipnl(cx);
		if (!s2_kw(s2_cur(cx), "else"))
			break;
		cx->pos++;
		s2_skipnl(cx);
		if (s2_cur(cx)->kind == T_KW && s2_cur(cx)->kw == KW_IF)
		{
			cx->pos++;
			s2_paren(cx, &a, &b);
			plx_indent(o, ind);
			appendStringInfo(o, "ELSIF %s THEN\n", s2_rw(cx, a, b));
			if (!s2_block(cx, S, ind + 1))
				all = false;
			continue;
		}
		plx_indent(o, ind);
		appendStringInfoString(o, "ELSE\n");
		if (!s2_block(cx, S, ind + 1))
			all = false;
		has_else = true;
		break;
	}
	plx_indent(o, ind);
	appendStringInfoString(o, "END IF;\n");
	return all && has_else;
}

static bool
s2_while(Ctx *cx, CsS2 *S, int ind)
{
	StringInfo	o = &cx->out;
	int			a,
				b;
	char	   *cond,
			   *body;
	CsLoop		fr;

	cx->pos++;
	s2_paren(cx, &a, &b);
	cond = s2_rw(cx, a, b);
	body = s2_loop_body(cx, S, ind + 1, &fr);
	plx_indent(o, ind);
	appendStringInfo(o, "WHILE %s LOOP\n", cond);
	appendStringInfoString(o, body);
	plx_indent(o, ind);
	appendStringInfoString(o, "END LOOP;\n");
	return false;
}

/*
 * do body while (cond); with no continue: LOOP body EXIT WHEN cond fails.
 * A continue must reach the test, so then a flag skips it on the first pass
 * and the test moves to the top of the loop.
 */
static bool
s2_do(Ctx *cx, CsS2 *S, int ind)
{
	StringInfo	o = &cx->out;
	int			a,
				b;
	char	   *cond,
			   *body;
	CsLoop		fr;

	cx->pos++;
	body = s2_loop_body(cx, S, ind + 1, &fr);
	s2_skipnl(cx);
	if (!(s2_cur(cx)->kind == T_KW && s2_cur(cx)->kw == KW_WHILE))
		plx_err(cx, s2_cur(cx)->line, "expected 'while' after do");
	cx->pos++;
	s2_paren(cx, &a, &b);
	cond = s2_rw(cx, a, b);
	if (s2_cur(cx)->kind == T_SEMI)
		cx->pos++;
	if (!fr.cont_used)
	{
		plx_indent(o, ind);
		appendStringInfoString(o, "LOOP\n");
		appendStringInfoString(o, body);
		plx_indent(o, ind + 1);
		appendStringInfo(o, "EXIT WHEN (%s) IS NOT TRUE;\n", cond);
	}
	else
	{
		char	   *flag = s2_flag(cx, S, "do");

		plx_indent(o, ind);
		appendStringInfo(o, "%s := false;\n", flag);
		plx_indent(o, ind);
		appendStringInfoString(o, "LOOP\n");
		plx_indent(o, ind + 1);
		appendStringInfo(o, "IF %s THEN\n", flag);
		plx_indent(o, ind + 2);
		appendStringInfo(o, "EXIT WHEN (%s) IS NOT TRUE;\n", cond);
		plx_indent(o, ind + 1);
		appendStringInfoString(o, "END IF;\n");
		plx_indent(o, ind + 1);
		appendStringInfo(o, "%s := true;\n", flag);
		appendStringInfoString(o, body);
	}
	plx_indent(o, ind);
	appendStringInfoString(o, "END LOOP;\n");
	return false;
}

/* for (i = LO; i CMP HI; i += K): a plpgsql integer FOR */
static bool
s2_for(Ctx *cx, CsS2 *S, int ind)
{
	StringInfo	o = &cx->out;
	int			a,
				b,
				s1,
				s2,
				line = s2_cur(cx)->line;
	Tok		   *v,
			   *cmp,
			   *step;
	char	   *lo,
			   *hi,
			   *k,
			   *body;
	bool		down;
	CsLoop		fr;

	cx->pos++;
	s2_paren(cx, &a, &b);
	s1 = s2_find(cx, a, b, T_SEMI);
	s2 = s2_find(cx, s1 + 1, b, T_SEMI);
	if (s1 >= b || s2 >= b || cx->t[a].kind != T_IDENT || !plx_tok_is(&cx->t[a + 1], "=") ||
		cx->t[s1 + 1].kind != T_IDENT || cx->t[s1 + 2].kind != T_OP ||
		cx->t[s2 + 1].kind != T_IDENT ||
		!(plx_tok_is(&cx->t[s2 + 2], "+=") || plx_tok_is(&cx->t[s2 + 2], "-=")))
		plx_err(cx, line, "malformed counting loop");
	v = &cx->t[a];
	cmp = &cx->t[s1 + 2];
	step = &cx->t[s2 + 2];
	lo = s2_rw(cx, a + 2, s1);
	hi = s2_rw(cx, s1 + 3, s2);
	k = s2_rw(cx, s2 + 3, b);
	down = plx_tok_is(step, "-=");
	body = s2_loop_body(cx, S, ind + 1, &fr);
	plx_indent(o, ind);
	appendStringInfo(o, "FOR %.*s IN %s%s..", v->len, v->s, down ? "REVERSE " : "", lo);
	if (plx_tok_is(cmp, "<"))
		appendStringInfo(o, "(%s) - 1", hi);
	else if (plx_tok_is(cmp, ">"))
		appendStringInfo(o, "(%s) + 1", hi);
	else
		appendStringInfoString(o, hi);
	if (strcmp(k, "1") != 0)
		appendStringInfo(o, " BY %s", k);
	appendStringInfoString(o, " LOOP\n");
	appendStringInfoString(o, body);
	plx_indent(o, ind);
	appendStringInfoString(o, "END LOOP;\n");
	return false;
}

/* the iterator statements of a general for, as plpgsql */
static char *
s2_steps(Ctx *cx, int a, int b, int ind)
{
	StringInfoData saved = cx->out;
	char	   *r;

	initStringInfo(&cx->out);
	while (a < b)
	{
		int			e = s2_find(cx, a, b, T_COMMA);

		if (e > a)
			plx_emit_core(cx, a, e, ind, false);
		a = e + 1;
	}
	r = cx->out.data;
	cx->out = saved;
	return r;
}

/*
 * __plx_for (cond; step, ...) body: WHILE cond LOOP body steps END LOOP. A
 * continue must still run the steps, so then a flag runs them at the top of
 * every pass but the first.
 */
static bool
s2_genfor(Ctx *cx, CsS2 *S, int ind)
{
	StringInfo	o = &cx->out;
	int			a,
				b,
				s1;
	char	   *cond,
			   *body;
	bool		always,
				nosteps;
	CsLoop		fr;

	cx->pos++;
	s2_paren(cx, &a, &b);
	s1 = s2_find(cx, a, b, T_SEMI);
	if (s1 >= b)
		plx_err(cx, s2_cur(cx)->line, "malformed for loop");
	cond = s2_rw(cx, a, s1);
	always = (strcmp(cond, "true") == 0);
	nosteps = (s1 + 1 >= b);
	body = s2_loop_body(cx, S, ind + 1, &fr);
	if (nosteps || !fr.cont_used)
	{
		plx_indent(o, ind);
		if (always)
			appendStringInfoString(o, "LOOP\n");
		else
			appendStringInfo(o, "WHILE %s LOOP\n", cond);
		appendStringInfoString(o, body);
		if (!nosteps)
			appendStringInfoString(o, s2_steps(cx, s1 + 1, b, ind + 1));
	}
	else
	{
		char	   *flag = s2_flag(cx, S, "for");

		plx_indent(o, ind);
		appendStringInfo(o, "%s := false;\n", flag);
		plx_indent(o, ind);
		appendStringInfoString(o, "LOOP\n");
		plx_indent(o, ind + 1);
		appendStringInfo(o, "IF %s THEN\n", flag);
		appendStringInfoString(o, s2_steps(cx, s1 + 1, b, ind + 2));
		plx_indent(o, ind + 1);
		appendStringInfoString(o, "END IF;\n");
		plx_indent(o, ind + 1);
		appendStringInfo(o, "%s := true;\n", flag);
		if (!always)
		{
			plx_indent(o, ind + 1);
			appendStringInfo(o, "EXIT WHEN (%s) IS NOT TRUE;\n", cond);
		}
		appendStringInfoString(o, body);
	}
	plx_indent(o, ind);
	appendStringInfoString(o, "END LOOP;\n");
	return false;
}

/* foreach (v in query(...)) / foreach (v in array) */
static bool
s2_foreach(Ctx *cx, CsS2 *S, int ind)
{
	StringInfo	o = &cx->out;
	StringInfoData head;
	int			a,
				b,
				ra;
	Tok		   *v;
	char	   *body;
	CsLoop		fr;

	cx->pos++;
	s2_paren(cx, &a, &b);
	v = &cx->t[a];
	if (v->kind != T_IDENT || !(cx->t[a + 1].kind == T_KW && cx->t[a + 1].kw == KW_IN))
		plx_err(cx, v->line, "malformed foreach");
	ra = a + 2;
	initStringInfo(&head);
	if (cx->t[ra].kind == T_IDENT && plx_name_eq(&cx->t[ra], "query") &&
		cx->t[ra + 1].kind == T_LPAREN)
	{
		int			sa[16],
					se[16],
					after,
					n;
		char	   *sqlv,
				   *binds;

		n = plx_parse_args(cx, ra, sa, se, 16, &after);
		if (after != b)
			plx_err(cx, v->line, "unexpected text after query(...) in foreach");
		if (n < 1)
			plx_err(cx, v->line, "query requires a SQL argument");
		if (!plx_is_param(cx, v->s, v->len))
		{
			PlxLocal2  *l = plx_local_find(cx, v->s, v->len);

			if (!l)
				l = plx_local_add(cx, v->s, v->len);
			if (!l->typ)
				l->is_record = true;
		}
		if (!plx_sql_arg(cx, sa, se, n, false, &sqlv, &binds))
			appendStringInfo(&head, "FOR %.*s IN %s LOOP\n", v->len, v->s, sqlv);
		else if (binds)
			appendStringInfo(&head, "FOR %.*s IN EXECUTE %s USING %s LOOP\n",
							 v->len, v->s, sqlv, binds);
		else
			appendStringInfo(&head, "FOR %.*s IN EXECUTE %s LOOP\n", v->len, v->s, sqlv);
	}
	else
	{
		PlxLocal2  *l = plx_local_find(cx, v->s, v->len);

		if (!l || !l->typ)
			plx_err(cx, v->line, "foreach over an array needs a typed loop variable");
		appendStringInfo(&head, "FOREACH %.*s IN ARRAY %s LOOP\n", v->len, v->s,
						 s2_rw(cx, ra, b));
	}
	body = s2_loop_body(cx, S, ind + 1, &fr);
	plx_indent(o, ind);
	appendStringInfoString(o, head.data);
	appendStringInfoString(o, body);
	plx_indent(o, ind);
	appendStringInfoString(o, "END LOOP;\n");
	return false;
}

/*
 * The statements of one switch section. A break at section level ends the
 * section (it is not emitted); a block directly in the section is section
 * level too, so "case 1: { ...; break; }" works. Returns whether the section
 * ends in a statement that leaves it.
 */
static bool
s2_arm(Ctx *cx, CsS2 *S, int ind)
{
	bool		term = false;

	check_stack_depth();
	if (++cx->depth > PLX_MAX_DEPTH)
		plx_err(cx, s2_cur(cx)->line, "statements nested too deeply");
	for (;;)
	{
		Tok		   *t;

		plx_skip_seps(cx);
		t = s2_cur(cx);
		if (t->kind == T_EOF || t->kind == T_RBRACE ||
			s2_kw(t, "case") || s2_kw(t, "default"))
			break;
		if (t->kind == T_KW && t->kw == KW_BREAK && cx->t[cx->pos + 1].kind == T_SEMI)
		{
			cx->pos += 2;
			term = true;
			continue;
		}
		if (t->kind == T_LBRACE)
		{
			cx->pos++;
			term = s2_arm(cx, S, ind);
			plx_skip_seps(cx);
			if (s2_cur(cx)->kind != T_RBRACE)
				plx_err(cx, s2_cur(cx)->line, "expected '}'");
			cx->pos++;
			continue;
		}
		term = s2_stmt(cx, S, ind, false);
	}
	cx->depth--;
	return term;
}

static bool
s2_switch(Ctx *cx, CsS2 *S, int ind)
{
	StringInfo	o = &cx->out;
	int			a,
				b,
				line = s2_cur(cx)->line;
	char	   *subj;
	CsLoop		fr;
	StringInfoData whens,
				dflt;
	bool		has_default = false;
	int			nwhen = 0;

	cx->pos++;
	s2_paren(cx, &a, &b);
	subj = s2_rw(cx, a, b);
	s2_skipnl(cx);
	if (s2_cur(cx)->kind != T_LBRACE)
		plx_err(cx, line, "switch requires a '{' block");
	cx->pos++;
	memset(&fr, 0, sizeof(fr));
	fr.is_switch = true;
	fr.id = ++S->seq;
	fr.outer = S->loops;
	S->loops = &fr;
	{
		char	   *lab = psprintf("__plx_sw_%d", fr.id);

		plx_label_note(cx, lab, (int) strlen(lab));
	}
	initStringInfo(&whens);
	initStringInfo(&dflt);
	for (;;)
	{
		StringInfoData vals;
		StringInfoData saved;
		bool		is_default = false,
					term;
		int			nvals = 0;
		char	   *body;

		plx_skip_seps(cx);
		if (s2_cur(cx)->kind == T_RBRACE)
		{
			cx->pos++;
			break;
		}
		if (s2_cur(cx)->kind == T_EOF)
			plx_err(cx, line, "unterminated switch block");
		initStringInfo(&vals);
		for (;;)
		{
			Tok		   *t = s2_cur(cx);

			if (s2_kw(t, "case"))
			{
				int			va = ++cx->pos,
							ve = va,
							depth = 0;

				while (cx->t[ve].kind != T_EOF &&
					   !(depth == 0 && plx_tok_is(&cx->t[ve], ":")))
				{
					if (cx->t[ve].kind == T_LPAREN || cx->t[ve].kind == T_LBRACKET)
						depth++;
					else if (cx->t[ve].kind == T_RPAREN || cx->t[ve].kind == T_RBRACKET)
						depth--;
					ve++;
				}
				if (cx->t[ve].kind == T_EOF)
					plx_err(cx, t->line, "case label requires ':'");
				if (nvals++)
					appendStringInfoString(&vals, ", ");
				appendStringInfoString(&vals, s2_rw(cx, va, ve));
				cx->pos = ve + 1;
			}
			else if (s2_kw(t, "default"))
			{
				cx->pos++;
				if (!plx_tok_is(s2_cur(cx), ":"))
					plx_err(cx, t->line, "default label requires ':'");
				cx->pos++;
				is_default = true;
			}
			else
				break;
			plx_skip_seps(cx);
		}
		if (!nvals && !is_default)
			plx_err(cx, s2_cur(cx)->line, "a switch section must start with a case or default label");
		if (is_default && has_default)
			plx_err(cx, s2_cur(cx)->line, "a switch can have only one default section");
		saved = cx->out;
		initStringInfo(&cx->out);
		term = s2_arm(cx, S, ind + 2);
		body = cx->out.data;
		cx->out = saved;
		plx_skip_seps(cx);
		if (!term && s2_cur(cx)->kind != T_RBRACE)
			plx_err(cx, s2_cur(cx)->line,
					"control cannot fall through from one case to another; end the section with break, return, throw or continue");
		if (is_default)
		{
			has_default = true;
			appendStringInfoString(&dflt, body);
		}
		else
		{
			nwhen++;
			plx_indent(&whens, ind + 1);
			appendStringInfo(&whens, "WHEN %s THEN\n", vals.data);
			appendStringInfoString(&whens, body);
		}
	}
	S->loops = fr.outer;
	if (fr.brk_used)
	{
		plx_indent(o, ind);
		appendStringInfo(o, "<<__plx_sw_%d>>\n", fr.id);
		plx_indent(o, ind);
		appendStringInfoString(o, "BEGIN\n");
	}
	if (nwhen)
	{
		plx_indent(o, ind);
		appendStringInfo(o, "CASE %s\n", subj);
		appendStringInfoString(o, whens.data);
		plx_indent(o, ind + 1);
		appendStringInfoString(o, "ELSE\n");
		if (dflt.len)
			appendStringInfoString(o, dflt.data);
		else
		{
			/* no default: a value matching no case does nothing (plpgsql
			 * CASE would raise case_not_found) */
			plx_indent(o, ind + 2);
			appendStringInfoString(o, "NULL;\n");
		}
		plx_indent(o, ind);
		appendStringInfoString(o, "END CASE;\n");
	}
	else
		appendStringInfoString(o, dflt.data);
	if (fr.brk_used)
	{
		plx_indent(o, ind);
		appendStringInfoString(o, "END;\n");
	}
	return false;
}

static bool
s2_try(Ctx *cx, CsS2 *S, int ind)
{
	StringInfo	o = &cx->out;
	char	   *body;
	StringInfoData arms;
	int			nrescue = 0;
	char	   *ensure = NULL;
	PlxFin	   *fin;
	bool		term,
				all;

	cx->pos++;
	fin = plx_fin_push(cx);
	body = s2_block_text(cx, S, ind + 1, &term);
	all = term;
	initStringInfo(&arms);
	for (;;)
	{
		int			ca,
					cb;
		const char *save_ev = cx->exc_var;
		int			save_evl = cx->exc_varlen;
		int			save_diag = cx->diag_mask;
		char	   *cond,
				   *hb,
				   *pfx;

		s2_skipnl(cx);
		if (!(s2_cur(cx)->kind == T_KW && s2_cur(cx)->kw == KW_RESCUE))
			break;
		cx->pos++;
		s2_paren(cx, &ca, &cb);
		if (cx->t[ca].kind != T_STRING || cx->t[ca].len < 2)
			plx_err(cx, cx->t[ca].line, "malformed catch");
		cond = pnstrdup(cx->t[ca].s + 1, cx->t[ca].len - 2);
		if (ca + 1 < cb && cx->t[ca + 1].kind == T_IDENT)
		{
			cx->exc_var = cx->t[ca + 1].s;
			cx->exc_varlen = cx->t[ca + 1].len;
		}
		else
		{
			cx->exc_var = NULL;
			cx->exc_varlen = 0;
		}
		cx->diag_mask = 0;
		cx->handlerdepth++;
		hb = s2_block_text(cx, S, ind + 2, &term);
		cx->handlerdepth--;
		pfx = plx_diag_prefix(cx->diag_mask, ind + 2);
		cx->diag_mask = save_diag;
		cx->exc_var = save_ev;
		cx->exc_varlen = save_evl;
		appendStringInfoSpaces(&arms, (ind + 1) * 2);
		appendStringInfo(&arms, "WHEN %s THEN\n", cond);
		appendStringInfoString(&arms, pfx);
		appendStringInfoString(&arms, hb);
		nrescue++;
		if (!term)
			all = false;
	}
	plx_fin_pop(cx, fin);
	s2_skipnl(cx);
	if (s2_cur(cx)->kind == T_KW && s2_cur(cx)->kw == KW_ENSURE)
	{
		cx->pos++;
		ensure = s2_block_text(cx, S, ind + 1, &term);
		if (term)
			all = true;
	}
	plx_emit_try(cx, o, ind, body, nrescue, arms.data, ensure, fin);
	return all;
}

/* throw; / throw(errcode, message, detail, hint); */
static bool
s2_throw(Ctx *cx, int ind)
{
	StringInfo	o = &cx->out;
	int			a = cx->pos;

	if (cx->t[a + 1].kind == T_SEMI)
	{
		if (cx->handlerdepth == 0)
			plx_err(cx, cx->t[a].line, "throw; is only valid inside a catch block");
		plx_indent(o, ind);
		appendStringInfoString(o, "RAISE;\n");
		cx->pos = a + 2;
		return true;
	}
	{
		int			as[4],
					ae[4],
					after,
					n;
		char	   *code = NULL,
				   *msg,
				   *det = NULL,
				   *hint = NULL;
		StringInfoData u;

#define S2_ISNULL(i) (ae[i] - as[i] == 1 && cx->t[as[i]].kind == T_KW && \
					  cx->t[as[i]].kw == KW_NIL)
		n = plx_parse_args(cx, a, as, ae, 4, &after);
		if (n != 4)
			plx_err(cx, cx->t[a].line, "malformed throw");
		if (!S2_ISNULL(0))
			code = s2_rw(cx, as[0], ae[0]);
		cx->diag_msg = true;
		msg = s2_rw(cx, as[1], ae[1]);
		if (!S2_ISNULL(2))
			det = s2_rw(cx, as[2], ae[2]);
		if (!S2_ISNULL(3))
			hint = s2_rw(cx, as[3], ae[3]);
		cx->diag_msg = false;
#undef S2_ISNULL
		initStringInfo(&u);
		if (code)
			appendStringInfo(&u, "ERRCODE = %s", code);
		if (det)
			appendStringInfo(&u, "%sDETAIL = COALESCE((%s)::text, '')", u.len ? ", " : "", det);
		if (hint)
			appendStringInfo(&u, "%sHINT = COALESCE((%s)::text, '')", u.len ? ", " : "", hint);
		plx_indent(o, ind);
		appendStringInfo(o, "RAISE EXCEPTION '%%', %s%s%s;\n", msg, u.len ? " USING " : "", u.data);
		cx->pos = after;
		if (cx->t[cx->pos].kind == T_SEMI)
			cx->pos++;
		return true;
	}
}

static bool
s2_break(Ctx *cx, CsS2 *S, int ind, bool cont)
{
	CsLoop	   *l = S->loops;
	int			line = s2_cur(cx)->line;

	if (cont)
	{
		while (l && l->is_switch)
			l = l->outer;
		if (!l)
			plx_err(cx, line, "continue outside a loop");
		l->cont_used = true;
		plx_emit_exit(cx, &cx->out, ind, PLX_EXIT_CONTINUE, NULL, NULL, NULL);
	}
	else
	{
		if (!l)
			plx_err(cx, line, "break outside a loop or switch");
		if (l->is_switch)
		{
			/* a break below section level: leave the switch's labelled block */
			l->brk_used = true;
			plx_emit_exit(cx, &cx->out, ind, PLX_EXIT_BREAK,
						  psprintf("__plx_sw_%d", l->id), NULL, NULL);
		}
		else
			plx_emit_exit(cx, &cx->out, ind, PLX_EXIT_BREAK, NULL, NULL, NULL);
	}
	cx->pos++;
	if (s2_cur(cx)->kind == T_SEMI)
		cx->pos++;
	return true;
}

/* assert(cond[, msg]): the shared intrinsic, without its boolean-context rewrite */
static void
s2_assert(Ctx *cx, int a, int ind)
{
	int			as[4],
				ae[4],
				after,
				n;

	n = plx_parse_args(cx, a, as, ae, 4, &after);
	if (n < 1)
		plx_err(cx, cx->t[a].line, "assert requires a condition");
	plx_indent(&cx->out, ind);
	appendStringInfo(&cx->out, "ASSERT %s", s2_rw(cx, as[0], ae[0]));
	if (n >= 2)
		appendStringInfo(&cx->out, ", %s", s2_rw(cx, as[1], ae[1]));
	appendStringInfoString(&cx->out, ";\n");
}

/*
 * x = fetch_one(...) / fetch_from(c) into a local declared with a type (a
 * scalar or a row type). The engine's form would redeclare x as a record.
 */
static bool
s2_typed_fetch(Ctx *cx, int a, int b, int ind)
{
	Tok		   *t0 = &cx->t[a];
	Tok		   *fn;
	PlxLocal2  *l;
	bool		strict,
				from;
	int			as[16],
				ae[16],
				after,
				n;

	if (!(t0->kind == T_IDENT && a + 3 < b && plx_tok_is(&cx->t[a + 1], "=") &&
		  cx->t[a + 2].kind == T_IDENT && cx->t[a + 3].kind == T_LPAREN))
		return false;
	fn = &cx->t[a + 2];
	strict = plx_name_eq(fn, "fetch_one!");
	from = plx_name_eq(fn, "fetch_from");
	if (!strict && !from && !plx_name_eq(fn, "fetch_one"))
		return false;
	l = plx_local_find(cx, t0->s, t0->len);
	if (!l || !l->typ || l->is_record)
		return false;
	n = plx_parse_args(cx, a + 2, as, ae, 16, &after);
	if (n < 1)
		plx_err(cx, t0->line, "%.*s requires an argument", fn->len, fn->s);
	plx_indent(&cx->out, ind);
	if (from)
	{
		appendStringInfo(&cx->out, "FETCH FROM %s INTO %.*s;\n",
						 s2_rw(cx, as[0], ae[0]), t0->len, t0->s);
		return true;
	}
	{
		char	   *sqlv,
				   *binds;

		if (!plx_sql_arg(cx, as, ae, n, false, &sqlv, &binds))
			appendStringInfo(&cx->out, "SELECT * INTO %s%.*s FROM (%s) AS __plx_fo_%d;\n",
							 strict ? "STRICT " : "", t0->len, t0->s, sqlv, ++cx->subq);
		else if (binds)
			appendStringInfo(&cx->out, "EXECUTE %s INTO %s%.*s USING %s;\n",
							 sqlv, strict ? "STRICT " : "", t0->len, t0->s, binds);
		else
			appendStringInfo(&cx->out, "EXECUTE %s INTO %s%.*s;\n",
							 sqlv, strict ? "STRICT " : "", t0->len, t0->s);
	}
	return true;
}

/* one statement; returns whether it always leaves (return/throw/break/continue) */
static bool
s2_stmt(Ctx *cx, CsS2 *S, int ind, bool top)
{
	Tok		   *tk;
	bool		r = false;

	check_stack_depth();
	if (++cx->depth > PLX_MAX_DEPTH)
		plx_err(cx, cx->t[cx->pos].line, "statements nested too deeply");
	plx_skip_seps(cx);
	tk = s2_cur(cx);
	if (tk->kind == T_EOF || tk->kind == T_RBRACE)
		;
	else if (tk->kind == T_LBRACE)
		r = s2_block(cx, S, ind);
	else if (tk->kind == T_KW && tk->kw == KW_IF)
		r = s2_if(cx, S, ind);
	else if (tk->kind == T_KW && tk->kw == KW_WHILE)
		r = s2_while(cx, S, ind);
	else if (tk->kind == T_KW && tk->kw == KW_DO)
		r = s2_do(cx, S, ind);
	else if (tk->kind == T_KW && tk->kw == KW_FOR)
		r = s2_for(cx, S, ind);
	else if (tk->kind == T_IDENT && plx_name_eq(tk, "__plx_for"))
		r = s2_genfor(cx, S, ind);
	else if (tk->kind == T_KW && tk->kw == KW_FOREACH)
		r = s2_foreach(cx, S, ind);
	else if (tk->kind == T_KW && tk->kw == KW_CASE)
		r = s2_switch(cx, S, ind);
	else if (tk->kind == T_KW && tk->kw == KW_BEGIN)
		r = s2_try(cx, S, ind);
	else if (tk->kind == T_KW && tk->kw == KW_RAISE)
		r = s2_throw(cx, ind);
	else if (tk->kind == T_KW && (tk->kw == KW_BREAK || tk->kw == KW_NEXT))
		r = s2_break(cx, S, ind, tk->kw == KW_NEXT);
	else
	{
		int			a = cx->pos,
					b = s2_stmt_end(cx, a);

		if (tk->kind == T_IDENT && plx_name_eq(tk, "assert") && cx->t[a + 1].kind == T_LPAREN)
			s2_assert(cx, a, ind);
		else if (!s2_typed_fetch(cx, a, b, ind))
			plx_emit_core(cx, a, b, ind, top);
		r = (tk->kind == T_KW && tk->kw == KW_RETURN);
		cx->pos = (cx->t[b].kind == T_SEMI) ? b + 1 : b;
	}
	cx->depth--;
	return r;
}

static void
s2_program(Ctx *cx, CsS2 *S)
{
	for (;;)
	{
		plx_skip_seps(cx);
		if (s2_cur(cx)->kind == T_EOF)
			break;
		if (s2_cur(cx)->kind == T_RBRACE)
			plx_err(cx, s2_cur(cx)->line, "unexpected '}'");
		s2_stmt(cx, S, 1, true);
	}
}

/* ======================================================================== */
/* Surface                                                                  */
/* ======================================================================== */

static void
csharp_parse_body(Ctx *cx)
{
	CsS2		S;

	memset(&S, 0, sizeof(S));
	cx->body = cs_stage1(cx);
	elog(DEBUG1, "plxcsharp normalized program:\n%s", cx->body);
	plx_lex(cx);
	s2_program(cx, &S);
}

/* keywords of the normalized program (stage 2 and the engine read these) */
static const PlxKwSpell csharp_kws[] = {
	{"if", KW_IF}, {"else", KW_ELSE}, {"while", KW_WHILE}, {"do", KW_DO},
	{"for", KW_FOR}, {"foreach", KW_FOREACH}, {"in", KW_IN},
	{"switch", KW_CASE}, {"case", KW_WHEN}, {"default", KW_ELSE},
	{"break", KW_BREAK}, {"continue", KW_NEXT}, {"return", KW_RETURN},
	{"try", KW_BEGIN}, {"catch", KW_RESCUE}, {"finally", KW_ENSURE},
	{"throw", KW_RAISE}, {"let", KW_LET},
	{"true", KW_TRUE}, {"false", KW_FALSE}, {"null", KW_NIL},
	{"emit", KW_EMIT}, {"return_next", KW_RETURN_NEXT},
};

static const PlxSurface csharp_surface = {
	.lanname = "plxcsharp",
	.block_style = PLX_BLK_BRACE,
	.stmt_semicolon = true,
	.var_sigil = 0,
	.cmt_hash = false,
	.cmt_slash = false,
	.cmt_block = true,			/* type annotations in the normalized program */
	.type_ann = NULL,
	.interp_quote = 0,
	.fstrings = true,			/* interpolated strings are lowered to f-strings */
	.concat_op = 0,
	.kws = csharp_kws,
	.nkws = lengthof(csharp_kws),
	.excs = NULL,
	.nexcs = 0,
	.flags = PLX_TRUSTED,
	.parse_body = csharp_parse_body,
};

static char *
csharp_transpile(const char *src, const PlxFuncMeta *meta)
{
	return plx_transpile(src, meta, &csharp_surface, CurrentMemoryContext);
}

const PlxDialect plx_csharp_dialect = {
	.abi_version = PLX_ABI_VERSION,
	.lanname = "plxcsharp",
	.transpile = csharp_transpile,
	.flags = PLX_TRUSTED,
};

Datum
plx_csharp_validator(PG_FUNCTION_ARGS)
{
	return plx_generic_validator(fcinfo, &plx_csharp_dialect);
}

Datum
plx_csharp_inline_handler(PG_FUNCTION_ARGS)
{
	return plx_generic_inline_handler(fcinfo, &plx_csharp_dialect);
}

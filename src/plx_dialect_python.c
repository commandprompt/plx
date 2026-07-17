/*
 * plx_dialect_python.c - the "plxpython3" dialect.
 *
 * Python surface: indentation-based blocks (INDENT/DEDENT), no variable sigil,
 * f-string interpolation with {expr}, # line comments, if/elif/else, while,
 * for-in over range()/query()/array, try/except/finally, raise. The indent
 * parser is in this file; the dialect-neutral engine (lowering to plpgsql)
 * lives in plx_transpile.c.
 */
#include "postgres.h"

#include "fmgr.h"
#include "lib/stringinfo.h"
#include "miscadmin.h"
#include "utils/memutils.h"

#include "plx.h"
#include "plx_int.h"
#include "plx_engine.h"

PG_FUNCTION_INFO_V1(plx_py_validator);
PG_FUNCTION_INFO_V1(plx_py_inline_handler);

static const PlxKwSpell py_kws[] = {
	{"if", KW_IF}, {"elif", KW_ELSIF}, {"else", KW_ELSE},
	{"while", KW_WHILE}, {"for", KW_FOR}, {"in", KW_IN},
	{"return", KW_RETURN}, {"break", KW_BREAK}, {"continue", KW_NEXT},
	{"try", KW_BEGIN}, {"except", KW_RESCUE}, {"finally", KW_ENSURE}, {"as", KW_AS},
	{"raise", KW_RAISE}, {"def", KW_DEF}, {"class", KW_DEF}, {"lambda", KW_DEF},
	{"match", KW_CASE}, {"pass", KW_PASS},
	{"and", KW_AND}, {"or", KW_OR}, {"not", KW_NOT},
	{"None", KW_NIL}, {"True", KW_TRUE}, {"False", KW_FALSE},
	{"emit", KW_EMIT}, {"return_next", KW_RETURN_NEXT},
};

/* ---- Python (indent) front end. Parser + recursion guard + parse_body. ---- */
static void parse_py_stmt(Ctx *cx, int ind, bool toplevel);
static void parse_py_stmt_inner(Ctx *cx, int ind, bool toplevel);
static void parse_py_block(Ctx *cx, int ind);
static void parse_py_program(Ctx *cx);

static void
parse_py_stmt(Ctx *cx, int ind, bool toplevel)
{
	check_stack_depth();
	if (++cx->depth > PLX_MAX_DEPTH)
		plx_err(cx, cx->t[cx->pos].line, "statements nested too deeply");
	parse_py_stmt_inner(cx, ind, toplevel);
	cx->depth--;
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
		else if (depth == 0 && plx_tok_is(&cx->t[i], ":"))
			return i;
		i++;
	}
	return -1;
}

/* consume NEWLINE INDENT <statements> DEDENT */
static void
parse_py_block(Ctx *cx, int ind)
{
	plx_skip_seps(cx);
	if (cx->t[cx->pos].kind != T_INDENT)
		plx_err(cx, cx->t[cx->pos].line, "expected an indented block");
	cx->pos++;
	for (;;)
	{
		plx_skip_seps(cx);
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
	plx_indent(o, ind);
	appendStringInfo(o, "IF %s THEN\n", plx_rw_range(cx, cx->pos + 1, colon, true));
	cx->pos = colon + 1;
	parse_py_block(cx, ind + 1);
	while (cx->t[cx->pos].kind == T_KW && cx->t[cx->pos].kw == KW_ELSIF)	/* elif */
	{
		colon = py_header_colon(cx, cx->pos + 1);
		if (colon < 0)
			plx_err(cx, cx->t[cx->pos].line, "elif header requires ':'");
		plx_indent(o, ind);
		appendStringInfo(o, "ELSIF %s THEN\n", plx_rw_range(cx, cx->pos + 1, colon, true));
		cx->pos = colon + 1;
		parse_py_block(cx, ind + 1);
	}
	if (cx->t[cx->pos].kind == T_KW && cx->t[cx->pos].kw == KW_ELSE)
	{
		colon = py_header_colon(cx, cx->pos + 1);
		if (colon < 0)
			plx_err(cx, cx->t[cx->pos].line, "else header requires ':'");
		plx_indent(o, ind);
		appendStringInfoString(o, "ELSE\n");
		cx->pos = colon + 1;
		parse_py_block(cx, ind + 1);
	}
	plx_indent(o, ind);
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
	plx_indent(o, ind);
	appendStringInfo(o, "WHILE %s LOOP\n", plx_rw_range(cx, cx->pos + 1, colon, true));
	cx->pos = colon + 1;
	cx->loopdepth++;
	parse_py_block(cx, ind + 1);
	cx->loopdepth--;
	plx_indent(o, ind);
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

	if (cx->t[ia].kind == T_IDENT && plx_name_eq(&cx->t[ia], "range") &&
		cx->t[ia + 1].kind == T_LPAREN)
	{
		int			as[8], ae[8], after, n;
		char	   *lo, *hi, *step = NULL;

		n = plx_parse_args(cx, ia, as, ae, 8, &after);
		if (n == 1)
		{
			lo = pstrdup("0");
			hi = plx_rw_range(cx, as[0], ae[0], false);
		}
		else if (n >= 2)
		{
			lo = plx_rw_range(cx, as[0], ae[0], false);
			hi = plx_rw_range(cx, as[1], ae[1], false);
			if (n >= 3)
				step = plx_rw_range(cx, as[2], ae[2], false);
		}
		else
			plx_err(cx, hd, "range requires 1 to 3 arguments");
		plx_indent(o, ind);
		appendStringInfo(o, "FOR %.*s IN %s..(%s - 1)%s%s LOOP\n",
						 cx->t[var].len, cx->t[var].s, lo, hi,
						 step ? " BY " : "", step ? step : "");
		cx->pos = colon + 1;
		cx->loopdepth++;
		parse_py_block(cx, ind + 1);
		cx->loopdepth--;
	}
	else if (cx->t[ia].kind == T_IDENT && plx_name_eq(&cx->t[ia], "query") &&
			 cx->t[ia + 1].kind == T_LPAREN)
	{
		int			as[16], ae[16], after, n;

		n = plx_parse_args(cx, ia, as, ae, 16, &after);
		if (!plx_is_param(cx, cx->t[var].s, cx->t[var].len))
		{
			PlxLocal2  *l = plx_local_find(cx, cx->t[var].s, cx->t[var].len);

			if (!l)
				l = plx_local_add(cx, cx->t[var].s, cx->t[var].len);
			l->is_record = true;
		}
		plx_indent(o, ind);
		if (n == 1 && plx_arg_is_string_literal(cx, as[0], ae[0]))
		{
			appendStringInfo(o, "FOR %.*s IN ", cx->t[var].len, cx->t[var].s);
			plx_emit_string_as_sql(cx, &cx->t[as[0]], o);
			appendStringInfoString(o, " LOOP\n");
		}
		else
		{
			char	   *sqlv = plx_rw_range(cx, as[0], ae[0], false);
			char	   *binds = plx_binds_text(cx, as, ae, n);

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
		char	   *arrx = plx_rw_range(cx, ia, ib, false);
		PlxLocal2  *lv;

		if (plx_is_param(cx, cx->t[var].s, cx->t[var].len))
			plx_err(cx, hd, "foreach-array loop variable must be a local, not a parameter");
		lv = plx_local_find(cx, cx->t[var].s, cx->t[var].len);
		if (!lv || !lv->typ)
			plx_err(cx, hd, "iterating an array requires the loop variable to be annotated with its element type before the loop");
		plx_indent(o, ind);
		appendStringInfo(o, "FOREACH %.*s IN ARRAY %s%s LOOP\n",
						 cx->t[var].len, cx->t[var].s, arrx[0] == '[' ? "ARRAY" : "", arrx);
		cx->pos = colon + 1;
		cx->loopdepth++;
		parse_py_block(cx, ind + 1);
		cx->loopdepth--;
	}
	plx_indent(o, ind);
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
				char	   *cls = plx_span_text(cx, es + 1, cls_b);
				const char *c = plx_exc_class_to_condition(cls, (int) strlen(cls));

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
		pfx = plx_diag_prefix(cx->diag_mask, ind + 2);
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
		plx_indent(o, ind);
		appendStringInfoString(o, "BEGIN\n");
		appendStringInfoString(o, body);
		if (nrescue)
		{
			plx_indent(o, ind);
			appendStringInfoString(o, "EXCEPTION\n");
			appendStringInfoString(o, arms.data);
		}
		plx_indent(o, ind);
		appendStringInfoString(o, "END;\n");
	}
	else
	{
		plx_indent(o, ind);
		appendStringInfoString(o, "BEGIN\n");
		plx_indent(o, ind + 1);
		appendStringInfoString(o, "BEGIN\n");
		appendStringInfoString(o, body);
		if (nrescue)
		{
			plx_indent(o, ind + 1);
			appendStringInfoString(o, "EXCEPTION\n");
			appendStringInfoString(o, arms.data);
		}
		plx_indent(o, ind + 1);
		appendStringInfoString(o, "END;\n");
		appendStringInfoString(o, ensure_body);
		plx_indent(o, ind);
		appendStringInfoString(o, "EXCEPTION WHEN OTHERS THEN\n");
		appendStringInfoString(o, ensure_body);
		plx_indent(o, ind + 1);
		appendStringInfoString(o, "RAISE;\n");
		plx_indent(o, ind);
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
		plx_indent(&cx->out, ind);
		appendStringInfoString(&cx->out, "RAISE;\n");
		return;
	}
	/* raise(msg) / raise("level", msg): the call form emits a leveled RAISE */
	if (cx->t[a + 1].kind == T_LPAREN)
	{
		plx_emit_raise_call(cx, a, ind);
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

		n = plx_parse_args(cx, call, sa, se, 8, &after);
		plx_indent(&cx->out, ind);
		if (n >= 1)
			appendStringInfo(&cx->out, "RAISE EXCEPTION '%%', %s;\n",
							 plx_rw_range(cx, sa[0], se[0], false));
		else
			appendStringInfoString(&cx->out, "RAISE EXCEPTION 'exception';\n");
	}
}

static void
parse_py_stmt_inner(Ctx *cx, int ind, bool toplevel)
{
	Tok		   *tk;

	plx_skip_seps(cx);
	tk = &cx->t[cx->pos];
	if (tk->kind == T_EOF || tk->kind == T_DEDENT)
		return;
	/* loop label:  name: for ... / name: while ... */
	if (tk->kind == T_IDENT && plx_tok_is(&cx->t[cx->pos + 1], ":") &&
		cx->t[cx->pos + 2].kind == T_KW &&
		(cx->t[cx->pos + 2].kw == KW_FOR || cx->t[cx->pos + 2].kw == KW_WHILE))
	{
		plx_indent(&cx->out, ind);
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
				cx->pos = plx_stmt_end(cx, cx->pos);
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
		int			b = plx_stmt_end(cx, a);
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
		else if (tk->kind == T_IDENT && plx_name_eq(tk, "assert") &&
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
			plx_indent(&cx->out, ind);
			if (comma >= 0)
				appendStringInfo(&cx->out, "ASSERT %s, %s;\n",
								 plx_rw_range(cx, a + 1, comma, true), plx_rw_range(cx, comma + 1, b, false));
			else
				appendStringInfo(&cx->out, "ASSERT %s;\n", plx_rw_range(cx, a + 1, b, true));
		}
		else
			plx_emit_core(cx, a, b, ind, toplevel);
		cx->pos = b;
	}
}

static void
parse_py_program(Ctx *cx)
{
	for (;;)
	{
		plx_skip_seps(cx);
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




static void
plx_python_parse_body(Ctx *cx)
{
	plx_lex(cx);
	parse_py_program(cx);
}


static const PlxSurface py_surface = {
	.lanname = "plxpython3",
	.block_style = PLX_BLK_INDENT,
	.stmt_semicolon = false,
	.var_sigil = 0,
	.cmt_hash = true,
	.cmt_slash = false,
	.cmt_block = false,
	.type_ann = "#::",			/* Python type annotation: x = 0  #:: integer */
	.interp_quote = 0,
	.interp_hashbrace = false,
	.interp_dollar = false,
	.interp_dollarbrace = false,
	.fstrings = true,
	.concat_op = 0,
	.kws = py_kws,
	.nkws = lengthof(py_kws),
	.excs = NULL,
	.nexcs = 0,
	.flags = PLX_TRUSTED,
	.parse_body = plx_python_parse_body,
};

static char *
py_transpile(const char *src, const PlxFuncMeta *meta)
{
	return plx_transpile(src, meta, &py_surface, CurrentMemoryContext);
}

const PlxDialect plx_py_dialect = {
	.abi_version = PLX_ABI_VERSION,
	.lanname = "plxpython3",
	.transpile = py_transpile,
	.flags = PLX_TRUSTED,
};

Datum
plx_py_validator(PG_FUNCTION_ARGS)
{
	return plx_generic_validator(fcinfo, &plx_py_dialect);
}

Datum
plx_py_inline_handler(PG_FUNCTION_ARGS)
{
	return plx_generic_inline_handler(fcinfo, &plx_py_dialect);
}

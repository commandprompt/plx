/*
 * plx_dialect_ruby.c — the "plxruby" dialect.
 *
 * Supplies the Ruby dialect name + trampolines. The actual Ruby->plpgsql
 * lowering lives in the shared plx_transpile.c; this file just forwards.
 * Adding PHP (or any dialect) is a near-copy with a different surface.
 */
#include "postgres.h"

#include "fmgr.h"
#include "lib/stringinfo.h"
#include "miscadmin.h"
#include "utils/memutils.h"

#include "plx.h"
#include "plx_int.h"
#include "plx_engine.h"

PG_FUNCTION_INFO_V1(plx_ruby_validator);
PG_FUNCTION_INFO_V1(plx_ruby_inline_handler);

static const PlxKwSpell ruby_kws[] = {
	{"if", KW_IF}, {"unless", KW_UNLESS}, {"elsif", KW_ELSIF}, {"else", KW_ELSE},
	{"then", KW_THEN}, {"end", KW_END}, {"while", KW_WHILE}, {"until", KW_UNTIL},
	{"for", KW_FOR}, {"in", KW_IN}, {"do", KW_DO}, {"begin", KW_BEGIN},
	{"rescue", KW_RESCUE}, {"ensure", KW_ENSURE}, {"case", KW_CASE}, {"when", KW_WHEN},
	{"return", KW_RETURN}, {"next", KW_NEXT}, {"break", KW_BREAK}, {"raise", KW_RAISE},
	{"and", KW_AND}, {"or", KW_OR}, {"not", KW_NOT}, {"emit", KW_EMIT},
	{"return_next", KW_RETURN_NEXT}, {"loop", KW_LOOP}, {"def", KW_DEF},
	{"nil", KW_NIL}, {"true", KW_TRUE}, {"false", KW_FALSE},
};

/* ---- Ruby (keyword-end) front end. Parser + recursion guard + parse_body. --- */
static void parse_stmt(Ctx *cx, int indent, bool toplevel);
static void parse_stmt_inner(Ctx *cx, int ind, bool toplevel);
static void parse_block(Ctx *cx, int ind);
static void parse_iter(Ctx *cx, int a, int do_pos, int e, int ind);
static void parse_begin(Ctx *cx, int ind);
static void parse_case(Ctx *cx, int ind);

static void
parse_stmt(Ctx *cx, int indent, bool toplevel)
{
	check_stack_depth();
	if (++cx->depth > PLX_MAX_DEPTH)
		plx_err(cx, cx->t[cx->pos].line, "statements nested too deeply");
	parse_stmt_inner(cx, indent, toplevel);
	cx->depth--;
}

/* ---------------------------------------------------------------- blocks */

static void
parse_if(Ctx *cx, int ind)
{
	StringInfo	o = &cx->out;
	int			start = cx->pos;
	Tok		   *hd = &cx->t[start];
	bool		neg = (hd->kw == KW_UNLESS);
	int			e = plx_stmt_end(cx, start);
	int			cond_a = start + 1;
	int			cond_b = e;
	char	   *cond;

	/* optional trailing 'then' */
	if (cond_b > cond_a && cx->t[cond_b - 1].kind == T_KW && cx->t[cond_b - 1].kw == KW_THEN)
		cond_b--;
	cond = plx_rewrite_expr(cx, plx_span_text(cx, cond_a, cond_b),
						(int) strlen(plx_span_text(cx, cond_a, cond_b)), true);
	plx_indent(o, ind);
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
			int			ee = plx_stmt_end(cx, es);
			int			ca = es + 1, cb = ee;
			char	   *ec;

			if (cb > ca && cx->t[cb - 1].kind == T_KW && cx->t[cb - 1].kw == KW_THEN)
				cb--;
			ec = plx_rewrite_expr(cx, plx_span_text(cx, ca, cb),
							  (int) strlen(plx_span_text(cx, ca, cb)), true);
			plx_indent(o, ind);
			appendStringInfo(o, "ELSIF %s THEN\n", ec);
			cx->pos = ee;
			parse_block(cx, ind + 1);
		}
		else
		{
			plx_indent(o, ind);
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
	plx_indent(o, ind);
	appendStringInfoString(o, "END IF;\n");
}

static void
parse_while(Ctx *cx, int ind)
{
	StringInfo	o = &cx->out;
	int			start = cx->pos;
	Tok		   *hd = &cx->t[start];
	bool		neg = (hd->kw == KW_UNTIL);
	int			e = plx_stmt_end(cx, start);
	int			ca = start + 1, cb = e;
	char	   *cond;

	if (cb > ca && cx->t[cb - 1].kind == T_KW && cx->t[cb - 1].kw == KW_DO)
		cb--;
	cond = plx_rewrite_expr(cx, plx_span_text(cx, ca, cb),
						(int) strlen(plx_span_text(cx, ca, cb)), true);
	plx_indent(o, ind);
	appendStringInfo(o, "WHILE %s%s%s LOOP\n", neg ? "NOT (" : "", cond, neg ? ")" : "");
	cx->pos = e;
	cx->loopdepth++;
	parse_block(cx, ind + 1);
	cx->loopdepth--;
	if (cx->t[cx->pos].kind == T_KW && cx->t[cx->pos].kw == KW_END)
		cx->pos++;
	else
		plx_err(cx, hd->line, "unterminated while/until block");
	plx_indent(o, ind);
	appendStringInfoString(o, "END LOOP;\n");
}

static void
parse_loop(Ctx *cx, int ind)
{
	StringInfo	o = &cx->out;
	int			start = cx->pos;
	int			e = plx_stmt_end(cx, start);

	plx_indent(o, ind);
	appendStringInfoString(o, "LOOP\n");
	cx->pos = e;
	cx->loopdepth++;
	parse_block(cx, ind + 1);
	cx->loopdepth--;
	if (cx->t[cx->pos].kind == T_KW && cx->t[cx->pos].kw == KW_END)
		cx->pos++;
	else
		plx_err(cx, cx->t[start].line, "unterminated loop block");
	plx_indent(o, ind);
	appendStringInfoString(o, "END LOOP;\n");
}

/* integer for:  for V in LO..HI  |  for V in LO...HI */
static void
parse_for(Ctx *cx, int ind)
{
	StringInfo	o = &cx->out;
	int			start = cx->pos;
	Tok		   *hd = &cx->t[start];
	int			e = plx_stmt_end(cx, start);
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
		if (plx_tok_is(&cx->t[i], "..") || (cx->t[i].kind == T_OP && cx->t[i].len >= 2 &&
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
	lo = plx_rewrite_expr(cx, plx_span_text(cx, in_at + 1, range_at),
					  (int) strlen(plx_span_text(cx, in_at + 1, range_at)), false);
	hi_a = range_at + 1;
	hi_b = e;
	hi = plx_rewrite_expr(cx, plx_span_text(cx, hi_a, hi_b),
					  (int) strlen(plx_span_text(cx, hi_a, hi_b)), false);
	plx_indent(o, ind);
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
	plx_indent(o, ind);
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
			(plx_name_eq(&cx->t[i], "each") || plx_name_eq(&cx->t[i], "each_with_index")) &&
			cx->t[i - 1].kind == T_DOT)
		{
			each_at = i;
			with_index = plx_name_eq(&cx->t[i], "each_with_index");
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

	if (cx->t[a].kind == T_IDENT && plx_name_eq(&cx->t[a], "query") &&
		cx->t[a + 1].kind == T_LPAREN)
	{
		int			as[16], ae[16], after, n;

		n = plx_parse_args(cx, a, as, ae, 16, &after);
		if (n < 1)
			plx_err(cx, cx->t[a].line, "query requires a SQL argument");
		if (!plx_is_param(cx, cx->t[var1_a].s, cx->t[var1_a].len))
		{
			PlxLocal2  *l = plx_local_find(cx, cx->t[var1_a].s, cx->t[var1_a].len);

			if (!l)
				l = plx_local_add(cx, cx->t[var1_a].s, cx->t[var1_a].len);
			l->is_record = true;
		}
		if (with_index)
		{
			PlxLocal2  *li;

			if (var2_a < 0)
				plx_err(cx, cx->t[a].line, "each_with_index requires |row, idx|");
			li = plx_local_find(cx, cx->t[var2_a].s, cx->t[var2_a].len);
			if (!li)
				li = plx_local_add(cx, cx->t[var2_a].s, cx->t[var2_a].len);
			if (!li->typ)
				li->typ = pstrdup("integer");
			plx_indent(o, ind);
			appendStringInfo(o, "%.*s := -1;\n", cx->t[var2_a].len, cx->t[var2_a].s);
		}
		plx_indent(o, ind);
		if (n == 1 && plx_arg_is_string_literal(cx, as[0], ae[0]))
		{
			appendStringInfo(o, "FOR %.*s IN ", cx->t[var1_a].len, cx->t[var1_a].s);
			plx_emit_string_as_sql(cx, &cx->t[as[0]], o);
			appendStringInfoString(o, " LOOP\n");
		}
		else
		{
			char	   *st = plx_span_text(cx, as[0], ae[0]);
			char	   *sqlv = plx_rewrite_expr(cx, st, (int) strlen(st), false);
			char	   *binds = plx_binds_text(cx, as, ae, n);

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
			plx_indent(o, ind + 1);
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
		lst = plx_span_text(cx, lo_a, lo_b);
		hst = plx_span_text(cx, hi_a, hi_b);
		lo = plx_rewrite_expr(cx, lst, (int) strlen(lst), false);
		hi = plx_rewrite_expr(cx, hst, (int) strlen(hst), false);
		plx_indent(o, ind);
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
		char	   *arr = plx_span_text(cx, a, recv_end);
		char	   *arrx = plx_rewrite_expr(cx, arr, (int) strlen(arr), false);
		PlxLocal2  *lv;

		if (plx_is_param(cx, cx->t[var1_a].s, cx->t[var1_a].len))
			plx_err(cx, cx->t[a].line, "foreach-array loop variable must be a local, not a parameter");
		lv = plx_local_find(cx, cx->t[var1_a].s, cx->t[var1_a].len);
		if (!lv || !lv->typ)
			plx_err(cx, cx->t[a].line,
					"foreach over an array requires the loop variable to be annotated with its element type before the loop, e.g. \"%.*s #:: int\"",
					cx->t[var1_a].len, cx->t[var1_a].s);
		plx_indent(o, ind);
		appendStringInfo(o, "FOREACH %.*s IN ARRAY %s%s LOOP\n",
						 cx->t[var1_a].len, cx->t[var1_a].s,
						 arrx[0] == '[' ? "ARRAY" : "", arrx);
		cx->pos = e;
		cx->loopdepth++;
		parse_block(cx, ind + 1);
		cx->loopdepth--;
	}

	plx_skip_seps(cx);
	if (cx->t[cx->pos].kind == T_KW && cx->t[cx->pos].kw == KW_END)
		cx->pos++;
	else
		plx_err(cx, cx->t[a].line, "unterminated iterator block");
	plx_indent(o, ind);
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
		int			re = plx_stmt_end(cx, rs);
		const char *cond = "OTHERS";
		int			arrow = -1, i;
		int			ev_a = -1;
		const char *save_ev = cx->exc_var;
		int			save_evl = cx->exc_varlen;
		int			cls_a, cls_b;
		char	   *hb;

		for (i = rs + 1; i < re; i++)
			if (plx_tok_is(&cx->t[i], "=>"))
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
			char	   *clstext = plx_span_text(cx, cls_a, cls_b);
			const char *c = plx_exc_class_to_condition(clstext, (int) strlen(clstext));

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
		{
			int			save_diag = cx->diag_mask;
			char	   *pfx;

			cx->diag_mask = 0;
			cx->handlerdepth++;
			hb = capture_block(cx, ind + 2);
			cx->handlerdepth--;
			pfx = plx_diag_prefix(cx->diag_mask, ind + 2);
			cx->diag_mask = save_diag;
			cx->exc_var = save_ev;
			cx->exc_varlen = save_evl;

			appendStringInfoSpaces(&arms, (ind + 1) * 2);
			appendStringInfo(&arms, "WHEN %s THEN\n", cond);
			appendStringInfoString(&arms, pfx);
			appendStringInfoString(&arms, hb);
		}
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
			char	   *st = plx_span_text(cx, seg, i);

			if (!first)
				appendStringInfoString(&out, ", ");
			appendStringInfoString(&out, plx_rewrite_expr(cx, st, (int) strlen(st), false));
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
	int			e = plx_stmt_end(cx, start);
	bool		simple = (e > start + 1);
	char	   *subj = NULL;

	if (simple)
	{
		char	   *st = plx_span_text(cx, start + 1, e);

		subj = plx_rewrite_expr(cx, st, (int) strlen(st), false);
	}
	cx->pos = e;
	plx_skip_seps(cx);

	plx_indent(o, ind);
	if (simple)
		appendStringInfo(o, "CASE %s\n", subj);
	else
		appendStringInfoString(o, "CASE\n");

	if (!(cx->t[cx->pos].kind == T_KW && cx->t[cx->pos].kw == KW_WHEN))
		plx_err(cx, hdln, "case requires at least one 'when'");

	while (cx->t[cx->pos].kind == T_KW && cx->t[cx->pos].kw == KW_WHEN)
	{
		int			ws = cx->pos;
		int			we = plx_stmt_end(cx, ws);
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
			char	   *st = plx_span_text(cx, va, vb);

			vals = plx_rewrite_expr(cx, st, (int) strlen(st), true);
		}
		plx_indent(o, ind + 1);
		appendStringInfo(o, "WHEN %s THEN\n", vals);
		cx->pos = we;
		parse_block(cx, ind + 2);
	}
	if (cx->t[cx->pos].kind == T_KW && cx->t[cx->pos].kw == KW_ELSE)
	{
		plx_indent(o, ind + 1);
		appendStringInfoString(o, "ELSE\n");
		cx->pos++;
		parse_block(cx, ind + 2);
	}
	if (cx->t[cx->pos].kind == T_KW && cx->t[cx->pos].kw == KW_END)
		cx->pos++;
	else
		plx_err(cx, hdln, "unterminated case block");
	plx_indent(o, ind);
	appendStringInfoString(o, "END CASE;\n");
}

/* parse statements until a closer keyword or EOF (does not consume closer) */
static void
parse_block(Ctx *cx, int ind)
{
	for (;;)
	{
		Tok		   *tk;

		plx_skip_seps(cx);
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

	plx_skip_seps(cx);
	tk = &cx->t[cx->pos];
	if (tk->kind == T_EOF)
		return;
	/* loop label:  name: for i in ... / name: while ... / name: loop do */
	if (tk->kind == T_IDENT && plx_tok_is(&cx->t[cx->pos + 1], ":") &&
		cx->t[cx->pos + 2].kind == T_KW &&
		(cx->t[cx->pos + 2].kw == KW_FOR || cx->t[cx->pos + 2].kw == KW_WHILE ||
		 cx->t[cx->pos + 2].kw == KW_UNTIL || cx->t[cx->pos + 2].kw == KW_LOOP))
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
		int			b = plx_stmt_end(cx, a);
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
			plx_emit_leaf(cx, a, b, ind, toplevel);
			cx->pos = b;
		}
	}
}


void
plx_ruby_parse_body(Ctx *cx)
{
	plx_lex(cx);
	/* keyword-end (Ruby) top-level walk */
	for (;;)
	{
		plx_skip_seps(cx);
		if (cx->t[cx->pos].kind == T_EOF)
			break;
		if (cx->t[cx->pos].kind == T_KW &&
			(cx->t[cx->pos].kw == KW_END || cx->t[cx->pos].kw == KW_ELSIF ||
			 cx->t[cx->pos].kw == KW_ELSE || cx->t[cx->pos].kw == KW_WHEN ||
			 cx->t[cx->pos].kw == KW_RESCUE || cx->t[cx->pos].kw == KW_ENSURE))
			plx_err(cx, cx->t[cx->pos].line, "unexpected '%.*s'",
					cx->t[cx->pos].len, cx->t[cx->pos].s);
		parse_stmt(cx, 1, true);
	}
}


static const PlxSurface ruby_surface = {
	.lanname = "plxruby",
	.block_style = PLX_BLK_KEYWORD_END,
	.stmt_semicolon = false,
	.var_sigil = 0,
	.cmt_hash = true,
	.cmt_slash = false,
	.cmt_block = false,
	.type_ann = "#::",
	.sq_is_raw = true,			/* Ruby '...' is raw (backslashes literal) */
	.interp_quote = '"',
	.interp_hashbrace = true,
	.interp_dollar = false,
	.interp_dollarbrace = false,
	.concat_op = 0,
	.kws = ruby_kws,
	.nkws = lengthof(ruby_kws),
	.excs = NULL,
	.nexcs = 0,
	.flags = PLX_TRUSTED,
	.parse_body = plx_ruby_parse_body,
};

static char *
ruby_transpile(const char *src, const PlxFuncMeta *meta)
{
	return plx_transpile(src, meta, &ruby_surface, CurrentMemoryContext);
}

const PlxDialect plx_ruby_dialect = {
	.abi_version = PLX_ABI_VERSION,
	.lanname = "plxruby",
	.transpile = ruby_transpile,
	.flags = PLX_TRUSTED,
};

Datum
plx_ruby_validator(PG_FUNCTION_ARGS)
{
	return plx_generic_validator(fcinfo, &plx_ruby_dialect);
}

Datum
plx_ruby_inline_handler(PG_FUNCTION_ARGS)
{
	return plx_generic_inline_handler(fcinfo, &plx_ruby_dialect);
}

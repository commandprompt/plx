/*
 * plx_parse_brace.c — the brace-family front end (plxphp, plxjs, plxts).
 *
 * PHP, JavaScript, and TypeScript share one C-style brace parser: `if (...) { }`,
 * `for`/`while`/`foreach`, `switch`, try/catch. TypeScript additionally maps
 * "let x: T = e" type annotations to the shared parser's colon-colon form
 * (ts_preprocess). The three dialects' PlxSurface.parse_body all point at
 * plx_brace_parse_body(); the surfaces themselves live in plx_dialect_php.c /
 * plx_dialect_js.c / plx_dialect_ts.c. The dialect-neutral engine (lexer,
 * expression rewriter, leaf emitter, symbol table, assemble) is in
 * plx_transpile.c and reached through the plx_* entry points in plx_engine.h.
 */
#include "postgres.h"

#include "fmgr.h"
#include "lib/stringinfo.h"
#include "miscadmin.h"
#include "utils/memutils.h"

#include "plx.h"
#include "plx_int.h"
#include "plx_engine.h"

/* forward decls (brace-family parsers) */
static void parse_brace_stmt(Ctx *cx, int ind, bool toplevel);
static void parse_brace_stmt_inner(Ctx *cx, int ind, bool toplevel);
static void parse_brace_program(Ctx *cx);
static void parse_brace_block(Ctx *cx, int ind);
static void parse_switch_brace(Ctx *cx, int ind);
static void paren_group(Ctx *cx, int *a, int *b);

static void
parse_brace_stmt(Ctx *cx, int ind, bool toplevel)
{
	check_stack_depth();
	if (++cx->depth > PLX_MAX_DEPTH)
		plx_err(cx, cx->t[cx->pos].line, "statements nested too deeply");
	parse_brace_stmt_inner(cx, ind, toplevel);
	cx->depth--;
}

/* ---------------------------------------------------------------- brace parser (PHP/JS) */

/* expects cx->pos at '('; returns inner token range [*a,*b); leaves pos past ')' */
static void
paren_group(Ctx *cx, int *a, int *b)
{
	int			p = cx->pos, depth;

	if (cx->t[p].kind != T_LPAREN)
		plx_err(cx, cx->t[p].line, "expected '('");
	p++;
	*a = p;
	depth = 1;
	while (cx->t[p].kind != T_EOF && depth > 0)
	{
		if (cx->t[p].kind == T_LPAREN)
			depth++;
		else if (cx->t[p].kind == T_RPAREN)
		{
			depth--;
			if (!depth)
				break;
		}
		p++;
	}
	if (cx->t[p].kind != T_RPAREN)
		plx_err(cx, cx->t[cx->pos].line, "unterminated '('");
	*b = p;
	cx->pos = p + 1;
}

char *
plx_rw_range(Ctx *cx, int a, int b, bool boolctx)
{
	char	   *s = plx_span_text(cx, a, b);

	return plx_rewrite_expr(cx, s, (int) strlen(s), boolctx);
}

/* find next top-level ';' from a (paren/bracket/brace depth 0) */
static int
brace_stmt_end(Ctx *cx, int a)
{
	int			i = a, depth = 0;

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

static void
parse_if_brace(Ctx *cx, int ind)
{
	StringInfo	o = &cx->out;
	int			ca, cb;

	cx->pos++;					/* if */
	paren_group(cx, &ca, &cb);
	plx_indent(o, ind);
	appendStringInfo(o, "IF %s THEN\n", plx_rw_range(cx, ca, cb, true));
	parse_brace_block(cx, ind + 1);

	for (;;)
	{
		bool		is_elseif, is_else_if;

		plx_skip_seps(cx);
		is_elseif = (cx->t[cx->pos].kind == T_KW && cx->t[cx->pos].kw == KW_ELSIF);
		is_else_if = (cx->t[cx->pos].kind == T_KW && cx->t[cx->pos].kw == KW_ELSE &&
					  cx->t[cx->pos + 1].kind == T_KW && cx->t[cx->pos + 1].kw == KW_IF);

		if (!is_elseif && !is_else_if)
			break;
		cx->pos += is_else_if ? 2 : 1;
		paren_group(cx, &ca, &cb);
		plx_indent(o, ind);
		appendStringInfo(o, "ELSIF %s THEN\n", plx_rw_range(cx, ca, cb, true));
		parse_brace_block(cx, ind + 1);
	}
	if (cx->t[cx->pos].kind == T_KW && cx->t[cx->pos].kw == KW_ELSE)
	{
		cx->pos++;
		plx_indent(o, ind);
		appendStringInfoString(o, "ELSE\n");
		parse_brace_block(cx, ind + 1);
	}
	plx_indent(o, ind);
	appendStringInfoString(o, "END IF;\n");
}

static void
parse_while_brace(Ctx *cx, int ind)
{
	StringInfo	o = &cx->out;
	int			ca, cb;

	cx->pos++;
	paren_group(cx, &ca, &cb);
	plx_indent(o, ind);
	appendStringInfo(o, "WHILE %s LOOP\n", plx_rw_range(cx, ca, cb, true));
	cx->loopdepth++;
	parse_brace_block(cx, ind + 1);
	cx->loopdepth--;
	plx_indent(o, ind);
	appendStringInfoString(o, "END LOOP;\n");
}

/* emit `FOR <var> IN <query(...)> LOOP <body> END LOOP;` for a query receiver */
static void
brace_query_loop(Ctx *cx, Tok *var, int ra, int rb, int ind)
{
	StringInfo	o = &cx->out;
	int			sa[16], se[16], after, n;
	int			savedpos = cx->pos;

	/* array receiver (not query(...)) -> FOREACH v IN ARRAY expr LOOP */
	if (!(cx->t[ra].kind == T_IDENT && plx_name_eq(&cx->t[ra], "query") &&
		  ra + 1 < rb && cx->t[ra + 1].kind == T_LPAREN))
	{
		char	   *arrx = plx_rw_range(cx, ra, rb, false);
		PlxLocal2  *lv;

		if (plx_is_param(cx, var->s, var->len))
			plx_err(cx, var->line, "foreach-array loop variable must be a local, not a parameter");
		lv = plx_local_find(cx, var->s, var->len);
		if (!lv || !lv->typ)
			plx_err(cx, var->line,
					"foreach over an array requires the loop variable to be annotated with its element type before the loop");
		plx_indent(o, ind);
		appendStringInfo(o, "FOREACH %.*s IN ARRAY %s%s LOOP\n",
						 var->len, var->s, arrx[0] == '[' ? "ARRAY" : "", arrx);
		cx->loopdepth++;
		parse_brace_block(cx, ind + 1);
		cx->loopdepth--;
		plx_indent(o, ind);
		appendStringInfoString(o, "END LOOP;\n");
		return;
	}
	cx->pos = ra;
	n = plx_parse_args(cx, ra, sa, se, 16, &after);
	cx->pos = savedpos;
	if (!plx_is_param(cx, var->s, var->len))
	{
		PlxLocal2  *l = plx_local_find(cx, var->s, var->len);

		if (!l)
			l = plx_local_add(cx, var->s, var->len);
		l->is_record = true;
	}
	plx_indent(o, ind);
	if (n == 1 && plx_arg_is_string_literal(cx, sa[0], se[0]))
	{
		appendStringInfo(o, "FOR %.*s IN ", var->len, var->s);
		plx_emit_string_as_sql(cx, &cx->t[sa[0]], o);
		appendStringInfoString(o, " LOOP\n");
	}
	else
	{
		char	   *sqlv = plx_rw_range(cx, sa[0], se[0], false);
		char	   *binds = plx_binds_text(cx, sa, se, n);

		if (binds)
			appendStringInfo(o, "FOR %.*s IN EXECUTE %s USING %s LOOP\n",
							 var->len, var->s, sqlv, binds);
		else
			appendStringInfo(o, "FOR %.*s IN EXECUTE %s LOOP\n",
							 var->len, var->s, sqlv);
	}
	cx->loopdepth++;
	parse_brace_block(cx, ind + 1);
	cx->loopdepth--;
	plx_indent(o, ind);
	appendStringInfoString(o, "END LOOP;\n");
}

/* C-style counting for, or JS `for (v of query(...))` */
static void
parse_for_brace(Ctx *cx, int ind)
{
	StringInfo	o = &cx->out;
	int			ca, cb;
	int			s1 = -1, s2 = -1, of_at = -1, i, depth = 0;
	int			line = cx->t[cx->pos].line;

	cx->pos++;
	paren_group(cx, &ca, &cb);
	/* strip a leading let/const/var declaration keyword */
	if (cx->t[ca].kind == T_KW && cx->t[ca].kw == KW_LET)
		ca++;
	/* scan for top-level ';' (C-for) and KW_OF (for-of) */
	for (i = ca; i < cb; i++)
	{
		if (cx->t[i].kind == T_LPAREN || cx->t[i].kind == T_LBRACKET)
			depth++;
		else if (cx->t[i].kind == T_RPAREN || cx->t[i].kind == T_RBRACKET)
			depth--;
		else if (depth == 0 && cx->t[i].kind == T_SEMI)
		{
			if (s1 < 0)
				s1 = i;
			else
				s2 = i;
		}
		else if (depth == 0 && cx->t[i].kind == T_KW && cx->t[i].kw == KW_OF)
			of_at = i;
	}
	if (of_at >= 0)
	{
		if (cx->t[ca].kind != T_IDENT)
			plx_err(cx, line, "for-of requires a loop variable");
		brace_query_loop(cx, &cx->t[ca], of_at + 1, cb, ind);
		return;
	}
	if (s1 < 0 || s2 < 0)
		plx_err(cx, line, "for must be a counting loop 'for (v = LO; v < HI; v++)' or 'for (v of query(...))'");
	/* init: IDENT = LO ; cond: IDENT </<= HI ; incr: IDENT ++ | += K */
	{
		int			ia = ca, ib = s1, cca = s1 + 1, ccb = s2, xa = s2 + 1, xb = cb;
		char	   *var;
		bool		lt;
		char	   *lo, *hi, *step = NULL;

		if (!(cx->t[ia].kind == T_IDENT && ib - ia >= 3 && plx_tok_is(&cx->t[ia + 1], "=")))
			plx_err(cx, line, "for-loop init must be 'v = LO'");
		var = pnstrdup(cx->t[ia].s, cx->t[ia].len);
		lo = plx_rw_range(cx, ia + 2, ib, false);
		/* cond: var < HI or var <= HI */
		if (!(cx->t[cca].kind == T_IDENT && (plx_tok_is(&cx->t[cca + 1], "<") || plx_tok_is(&cx->t[cca + 1], "<="))))
			plx_err(cx, line, "for-loop condition must be 'v < HI' or 'v <= HI'");
		lt = plx_tok_is(&cx->t[cca + 1], "<");
		hi = plx_rw_range(cx, cca + 2, ccb, false);
		/* incr: var++ or var += K or var = var + K */
		if (cx->t[xa].kind == T_IDENT && plx_tok_is(&cx->t[xa + 1], "++"))
			step = NULL;
		else if (cx->t[xa].kind == T_IDENT && cx->t[xa + 1].kind == T_OP &&
				 cx->t[xa + 1].len == 2 && cx->t[xa + 1].s[0] == '+' && cx->t[xa + 1].s[1] == '=')
			step = plx_rw_range(cx, xa + 2, xb, false);
		else
			plx_err(cx, line, "for-loop step must be 'v++' or 'v += K'");
		plx_indent(o, ind);
		if (lt)
			appendStringInfo(o, "FOR %s IN %s..(%s - 1)%s%s LOOP\n", var, lo, hi,
							 step ? " BY " : "", step ? step : "");
		else
			appendStringInfo(o, "FOR %s IN %s..%s%s%s LOOP\n", var, lo, hi,
							 step ? " BY " : "", step ? step : "");
	}
	cx->loopdepth++;
	parse_brace_block(cx, ind + 1);
	cx->loopdepth--;
	plx_indent(o, ind);
	appendStringInfoString(o, "END LOOP;\n");
}

/* foreach (query(...) as row) { ... } -> FOR row IN <sql> LOOP */
static void
parse_foreach_brace(Ctx *cx, int ind)
{
	int			ca, cb, as_at = -1, i, depth = 0;
	int			line = cx->t[cx->pos].line;
	int			rv;

	cx->pos++;
	paren_group(cx, &ca, &cb);
	for (i = ca; i < cb; i++)
	{
		if (cx->t[i].kind == T_LPAREN)
			depth++;
		else if (cx->t[i].kind == T_RPAREN)
			depth--;
		else if (depth == 0 && cx->t[i].kind == T_KW && cx->t[i].kw == KW_AS)
		{
			as_at = i;
			break;
		}
	}
	if (as_at < 0)
		plx_err(cx, line, "foreach requires 'as'");
	/* row var: last IDENT before ')' (skip a key form -> take value var) */
	rv = cb - 1;
	while (rv > as_at && cx->t[rv].kind != T_IDENT)
		rv--;
	if (rv <= as_at)
		plx_err(cx, line, "foreach requires a row variable");
	brace_query_loop(cx, &cx->t[rv], ca, as_at, ind);
}

/* try { } catch (Class $e) { } [catch...] [finally { }] */
static void
parse_try_brace(Ctx *cx, int ind)
{
	StringInfo	o = &cx->out;
	int			line = cx->t[cx->pos].line;
	char	   *body;
	StringInfoData arms;
	int			nrescue = 0;
	char	   *ensure_body = NULL;

	cx->pos++;					/* try */
	{
		StringInfoData saved = cx->out;

		initStringInfo(&cx->out);
		parse_brace_block(cx, ind + 1);
		body = cx->out.data;
		cx->out = saved;
	}

	initStringInfo(&arms);
	plx_skip_seps(cx);
	while (cx->t[cx->pos].kind == T_KW && cx->t[cx->pos].kw == KW_RESCUE)	/* catch */
	{
		int			ca, cb, i, ev_a = -1;
		const char *cond = "OTHERS";
		const char *save_ev = cx->exc_var;
		int			save_evl = cx->exc_varlen;
		char	   *hb;
		StringInfoData saved;

		cx->pos++;
		paren_group(cx, &ca, &cb);
		/* catch (\Class $e): last IDENT is $e; the class tokens precede it */
		for (i = cb - 1; i >= ca; i--)
			if (cx->t[i].kind == T_IDENT)
			{
				ev_a = i;
				break;
			}
		if (ev_a > ca)
		{
			char	   *cls = plx_span_text(cx, ca, ev_a);
			const char *c;

			/* strip leading backslash/namespace for a bare class name */
			while (*cls == '\\' || *cls == ' ')
				cls++;
			c = plx_exc_class_to_condition(cls, (int) strlen(cls));
			if (c)
				cond = c;		/* else OTHERS (e.g. \Exception) */
		}
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
			saved = cx->out;
			initStringInfo(&cx->out);
			parse_brace_block(cx, ind + 2);
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
		}
		nrescue++;
		plx_skip_seps(cx);
	}
	if (cx->t[cx->pos].kind == T_KW && cx->t[cx->pos].kw == KW_ENSURE)	/* finally */
	{
		StringInfoData saved = cx->out;

		cx->pos++;
		initStringInfo(&cx->out);
		parse_brace_block(cx, ind + 1);
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
	(void) line;
}

/* throw new <Class>(msg[, ...]) -> RAISE EXCEPTION '%', msg; */
static void
emit_php_throw(Ctx *cx, int a, int b, int ind)
{
	int			p = a + 1;		/* after 'throw' */
	int			call = -1, i;

	/* find the '(' of the exception constructor */
	for (i = p; i < b; i++)
		if (cx->t[i].kind == T_LPAREN)
		{
			call = i - 1;		/* the class-name token precedes '(' */
			break;
		}
	if (call < 0)
		plx_err(cx, cx->t[a].line, "throw requires 'new Exception(...)'");
	{
		int			sa[16], se[16], after, n;
		int			savedpos = cx->pos;

		cx->pos = call;
		n = plx_parse_args(cx, call, sa, se, 16, &after);
		cx->pos = savedpos;
		plx_indent(&cx->out, ind);
		if (n >= 1)
			appendStringInfo(&cx->out, "RAISE EXCEPTION '%%', %s;\n",
							 plx_rw_range(cx, sa[0], se[0], false));
		else
			appendStringInfoString(&cx->out, "RAISE EXCEPTION 'exception';\n");
	}
}

/* switch (expr) { case v: ...; break; [case..] default: ... } -> CASE ... END CASE */
static void
parse_switch_brace(Ctx *cx, int ind)
{
	StringInfo	o = &cx->out;
	int			line = cx->t[cx->pos].line;
	int			ca, cb;
	int			when_count = 0;		/* plpgsql CASE needs >= 1 WHEN, ELSE last */
	bool		seen_default = false;

	cx->pos++;					/* switch */
	paren_group(cx, &ca, &cb);
	plx_indent(o, ind);
	appendStringInfo(o, "CASE %s\n", plx_rw_range(cx, ca, cb, false));
	if (cx->t[cx->pos].kind != T_LBRACE)
		plx_err(cx, line, "switch requires a '{' block");
	cx->pos++;

	for (;;)
	{
		StringInfoData vals;
		bool		is_default = false;
		int			nvals = 0;

		plx_skip_seps(cx);
		if (cx->t[cx->pos].kind == T_RBRACE)
		{
			cx->pos++;
			break;
		}
		if (cx->t[cx->pos].kind == T_EOF)
			plx_err(cx, line, "unterminated switch block");

		/* collect one or more consecutive case/default labels */
		initStringInfo(&vals);
		for (;;)
		{
			if (cx->t[cx->pos].kind == T_KW && cx->t[cx->pos].kw == KW_WHEN)	/* case */
			{
				int			va = ++cx->pos, ve = va;

				while (cx->t[ve].kind != T_EOF && !plx_tok_is(&cx->t[ve], ":"))
					ve++;
				if (cx->t[ve].kind == T_EOF)
					plx_err(cx, line, "case label requires ':'");
				if (nvals++)
					appendStringInfoString(&vals, ", ");
				appendStringInfoString(&vals, plx_rw_range(cx, va, ve, false));
				cx->pos = ve + 1;
			}
			else if (cx->t[cx->pos].kind == T_KW && cx->t[cx->pos].kw == KW_ELSE)	/* default */
			{
				cx->pos++;
				if (!plx_tok_is(&cx->t[cx->pos], ":"))
					plx_err(cx, line, "default label requires ':'");
				cx->pos++;
				is_default = true;
			}
			else
				break;
			plx_skip_seps(cx);
			if (!(cx->t[cx->pos].kind == T_KW &&
				  (cx->t[cx->pos].kw == KW_WHEN || cx->t[cx->pos].kw == KW_ELSE)))
				break;
		}
		if (!nvals && !is_default)
			plx_err(cx, line, "switch body must consist of case/default labels");

		plx_indent(o, ind + 1);
		if (is_default)
		{
			seen_default = true;
			appendStringInfoString(o, "ELSE\n");
		}
		else
		{
			/* plpgsql requires every WHEN before the ELSE (default) */
			if (seen_default)
				plx_err(cx, line,
						"a case after default is not supported; put default last");
			when_count++;
			appendStringInfo(o, "WHEN %s THEN\n", vals.data);
		}

		/*
		 * Body: statements until break / next label / '}'. An arm may end with
		 * an explicit 'break' or with a terminating statement (return/throw);
		 * reaching the next label with neither is genuine C fall-through, which
		 * plpgsql CASE cannot represent, so it is rejected.
		 */
		{
			bool		terminated = false;

			for (;;)
			{
				Tok		   *st;

				plx_skip_seps(cx);
				st = &cx->t[cx->pos];
				if (st->kind == T_KW && st->kw == KW_BREAK)
				{
					cx->pos++;
					plx_skip_seps(cx);
					terminated = true;
					break;
				}
				if (st->kind == T_RBRACE)
					break;
				if (st->kind == T_KW && (st->kw == KW_WHEN || st->kw == KW_ELSE))
				{
					if (!terminated)
						plx_err(cx, st->line,
								"switch fall-through is not supported; end each case with 'break' or 'return'");
					break;
				}
				if (st->kind == T_EOF)
					plx_err(cx, line, "unterminated switch block");
				terminated = (st->kind == T_KW && (st->kw == KW_RETURN || st->kw == KW_RAISE));
				parse_brace_stmt(cx, ind + 2, false);
			}
		}
	}
	if (when_count == 0)
		plx_err(cx, line,
				"switch needs at least one case (a switch with only default is not supported)");
	plx_indent(o, ind);
	appendStringInfoString(o, "END CASE;\n");
}

static void
parse_brace_stmt_inner(Ctx *cx, int ind, bool toplevel)
{
	Tok		   *tk;

	plx_skip_seps(cx);
	tk = &cx->t[cx->pos];
	if (tk->kind == T_EOF || tk->kind == T_RBRACE)
		return;
	/* loop label:  name: for (...) {...}  /  name: while (...) {...} */
	if (tk->kind == T_IDENT && plx_tok_is(&cx->t[cx->pos + 1], ":") &&
		cx->t[cx->pos + 2].kind == T_KW &&
		(cx->t[cx->pos + 2].kw == KW_FOR || cx->t[cx->pos + 2].kw == KW_WHILE))
	{
		plx_indent(&cx->out, ind);
		appendStringInfo(&cx->out, "<<%.*s>>\n", tk->len, tk->s);
		cx->pos += 2;			/* consume name and ':' */
		tk = &cx->t[cx->pos];
	}
	if (tk->kind == T_KW)
	{
		switch (tk->kw)
		{
			case KW_IF:
				parse_if_brace(cx, ind);
				return;
			case KW_WHILE:
				parse_while_brace(cx, ind);
				return;
			case KW_FOR:
				parse_for_brace(cx, ind);
				return;
			case KW_FOREACH:
				parse_foreach_brace(cx, ind);
				return;
			case KW_BEGIN:		/* try */
				parse_try_brace(cx, ind);
				return;
			case KW_DEF:
				plx_err(cx, tk->line, "nested function definitions are not supported");
				return;
			case KW_CASE:
				parse_switch_brace(cx, ind);
				return;
			default:
				break;
		}
	}
	/* leaf statement ending at ';' */
	{
		int			a = cx->pos;
		int			b = brace_stmt_end(cx, a);

		if (tk->kind == T_KW && tk->kw == KW_RAISE)	/* throw */
			emit_php_throw(cx, a, b, ind);
		else
			plx_emit_core(cx, a, b, ind, toplevel);
		cx->pos = (cx->t[b].kind == T_SEMI) ? b + 1 : b;
	}
}

static void
parse_brace_program(Ctx *cx)
{
	for (;;)
	{
		plx_skip_seps(cx);
		if (cx->t[cx->pos].kind == T_EOF)
			break;
		if (cx->t[cx->pos].kind == T_RBRACE)
			plx_err(cx, cx->t[cx->pos].line, "unexpected '}'");
		parse_brace_stmt(cx, 1, true);
	}
}

static void
parse_brace_block(Ctx *cx, int ind)
{
	if (cx->t[cx->pos].kind != T_LBRACE)
		plx_err(cx, cx->t[cx->pos].line, "expected '{'");
	cx->pos++;
	for (;;)
	{
		plx_skip_seps(cx);
		if (cx->t[cx->pos].kind == T_RBRACE)
		{
			cx->pos++;
			return;
		}
		if (cx->t[cx->pos].kind == T_EOF)
			plx_err(cx, cx->t[cx->pos].line, "unterminated '{' block");
		parse_brace_stmt(cx, ind, false);
	}
}

/* ======================================================================== */
/* TypeScript preprocessing (plxts).                                        */
/*                                                                          */
/* plxts is the plxjs (brace) dialect plus TypeScript type annotations. A   */
/* declaration "let x: T = e" is rewritten to the JS leading-colon-colon     */
/* block-comment annotation the shared parser already understands, with T    */
/* mapped from a TypeScript type to a SQL type. Only declaration annotations */
/* are touched (after let/const/var), so ternary and label colons are left   */
/* alone. The original TypeScript is what gets embedded for debugging.       */
/* ======================================================================== */

/* map a TypeScript type to a SQL type (palloc'd result) */
static char *
ts_map_type(const char *s, int len)
{
	StringInfoData r;
	bool		arr = false;

	while (len > 0 && (*s == ' ' || *s == '\t'))
	{
		s++;
		len--;
	}
	while (len > 0 && (s[len - 1] == ' ' || s[len - 1] == '\t'))
		len--;

	/* union: "T | null" / "T | undefined" -> first non-null member */
	{
		int			depth = 0,
					i;

		for (i = 0; i < len; i++)
		{
			char		c = s[i];

			if (c == '<' || c == '(' || c == '[' || c == '{')
				depth++;
			else if (c == '>' || c == ')' || c == ']' || c == '}')
				depth--;
			else if (depth == 0 && c == '|')
			{
				int			la = i + 1,
							lb = len;
				const char *rest = s + la;
				int			restlen = lb - la;

				/* left member */
				if (!((i == 4 && pg_strncasecmp(s, "null", 4) == 0) ||
					  (i == 9 && pg_strncasecmp(s, "undefined", 9) == 0)))
					return ts_map_type(s, i);
				return ts_map_type(rest, restlen);
			}
		}
	}

	/* trailing [] -> array */
	if (len >= 2 && s[len - 2] == '[' && s[len - 1] == ']')
	{
		arr = true;
		len -= 2;
		while (len > 0 && (s[len - 1] == ' ' || s[len - 1] == '\t'))
			len--;
	}

	initStringInfo(&r);
	if (len == 6 && pg_strncasecmp(s, "number", 6) == 0)
		appendStringInfoString(&r, "numeric");
	else if (len == 6 && pg_strncasecmp(s, "string", 6) == 0)
		appendStringInfoString(&r, "text");
	else if (len == 7 && pg_strncasecmp(s, "boolean", 7) == 0)
		appendStringInfoString(&r, "boolean");
	else if (len == 6 && pg_strncasecmp(s, "bigint", 6) == 0)
		appendStringInfoString(&r, "bigint");
	else
		appendBinaryStringInfo(&r, s, len);	/* pass through a SQL type verbatim */
	if (arr)
		appendStringInfoString(&r, "[]");
	return r.data;
}

/* copy a quoted string / template literal starting at *pp verbatim into out */
static void
ts_copy_string(const char **pp, const char *end, StringInfo out)
{
	const char *p = *pp;
	char		q = *p;

	appendStringInfoChar(out, *p);
	p++;
	if (q == '`')
	{
		int			brace = 0;

		while (p < end)
		{
			if (*p == '\\' && p + 1 < end)
			{
				appendBinaryStringInfo(out, p, 2);
				p += 2;
				continue;
			}
			if (brace == 0 && *p == '`')
			{
				appendStringInfoChar(out, *p);
				p++;
				break;
			}
			if (*p == '$' && p + 1 < end && p[1] == '{')
			{
				appendBinaryStringInfo(out, p, 2);
				p += 2;
				brace++;
				continue;
			}
			if (brace > 0 && *p == '}')
				brace--;
			appendStringInfoChar(out, *p);
			p++;
		}
	}
	else
	{
		while (p < end)
		{
			if (*p == '\\' && p + 1 < end)
			{
				appendBinaryStringInfo(out, p, 2);
				p += 2;
				continue;
			}
			appendStringInfoChar(out, *p);
			if (*p == q)
			{
				p++;
				break;
			}
			p++;
		}
	}
	*pp = p;
}

/* advance *pp past a string / template literal (no output) */
static void
ts_skip_string(const char **pp, const char *end)
{
	const char *p = *pp;
	char		q = *p++;
	int			brace = 0;

	while (p < end)
	{
		if (*p == '\\' && p + 1 < end)
		{
			p += 2;
			continue;
		}
		if (q == '`' && *p == '$' && p + 1 < end && p[1] == '{')
		{
			p += 2;
			brace++;
			continue;
		}
		if (q == '`' && brace > 0 && *p == '}')
		{
			brace--;
			p++;
			continue;
		}
		if (brace == 0 && *p == q)
		{
			p++;
			break;
		}
		p++;
	}
	*pp = p;
}

static char *
ts_preprocess(const char *src)
{
	const char *p = src,
			   *end = src + strlen(src);
	StringInfoData out;
	int			pdepth = 0;			/* parenthesis nesting */
	int			for_depth = -1;		/* paren depth of an open for(...) header */
	bool		last_for = false;	/* previous significant word was "for" */

	initStringInfo(&out);
	while (p < end)
	{
		if (*p == '\'' || *p == '"' || *p == '`')
		{
			last_for = false;
			ts_copy_string(&p, end, &out);
			continue;
		}
		if (p[0] == '/' && p + 1 < end && p[1] == '/')
		{
			while (p < end && *p != '\n')
				appendStringInfoChar(&out, *p++);
			continue;
		}
		if (p[0] == '/' && p + 1 < end && p[1] == '*')
		{
			appendBinaryStringInfo(&out, p, 2);
			p += 2;
			while (p < end && !(p[0] == '*' && p + 1 < end && p[1] == '/'))
				appendStringInfoChar(&out, *p++);
			if (p < end)
			{
				appendBinaryStringInfo(&out, p, 2);
				p += 2;
			}
			continue;
		}
		if (*p == '(')
		{
			pdepth++;
			if (last_for)
				for_depth = pdepth;
			last_for = false;
			appendStringInfoChar(&out, *p++);
			continue;
		}
		if (*p == ')')
		{
			if (pdepth == for_depth)
				for_depth = -1;
			if (pdepth > 0)
				pdepth--;
			last_for = false;
			appendStringInfoChar(&out, *p++);
			continue;
		}
		if (plx_is_ident_start(*p))
		{
			const char *ws = p;			/* word start */

			while (p < end && plx_is_ident(*p))
				p++;
			appendBinaryStringInfo(&out, ws, (int) (p - ws));

			/* let / const / var  IDENT  : TYPE  ->  a JS block-comment annotation
			 * (dropped inside a for(...) header, where the counter is integer) */
			if ((p - ws == 3 && (strncmp(ws, "let", 3) == 0 || strncmp(ws, "var", 3) == 0)) ||
				(p - ws == 5 && strncmp(ws, "const", 5) == 0))
			{
				const char *q = p;
				const char *ide;

				while (q < end && (*q == ' ' || *q == '\t'))
					q++;
				if (q < end && plx_is_ident_start(*q))
				{
					const char *r;

					while (q < end && plx_is_ident(*q))
						q++;
					ide = q;
					r = q;
					while (r < end && (*r == ' ' || *r == '\t'))
						r++;
					if (r < end && *r == ':')
					{
						const char *ts,
								   *te;
						char	   *mapped;
						int			depth = 0;

						appendBinaryStringInfo(&out, p, (int) (ide - p));	/* "let x" */
						r++;			/* skip ':' */
						while (r < end && (*r == ' ' || *r == '\t'))
							r++;
						ts = r;
						while (r < end)	/* read the type up to = ; , ) or newline */
						{
							char		c = *r;

							if (c == '<' || c == '(' || c == '[' || c == '{')
								depth++;
							else if (c == '>' || c == ')' || c == ']' || c == '}')
								depth--;
							else if (depth <= 0 && (c == '=' || c == ';' || c == ',' ||
													c == ')' || c == '\n'))
								break;
							r++;
						}
						te = r;
						mapped = ts_map_type(ts, (int) (te - ts));

						if (te < end && *te == '=')
						{
							/* has a value: emit "= value", then the annotation, so
							 * plxjs sees the annotation trailing the value */
							const char *v = te;
							int			vd = 0;

							while (v < end)
							{
								char		c = *v;

								if (c == '\'' || c == '"' || c == '`')
								{
									ts_skip_string(&v, end);
									continue;
								}
								if (c == '/' && v + 1 < end && v[1] == '/')
								{
									while (v < end && *v != '\n')
										v++;
									continue;
								}
								if (c == '(' || c == '[' || c == '{')
									vd++;
								else if (c == ')' || c == ']' || c == '}')
								{
									if (vd == 0)
										break;
									vd--;
								}
								else if (vd == 0 && (c == ';' || c == ','))
									break;
								v++;
							}
							appendBinaryStringInfo(&out, te, (int) (v - te));
							if (for_depth < 0)
								appendStringInfo(&out, " /*:: %s */", mapped);
							p = v;
						}
						else
						{
							/* no value: annotate the declaration directly */
							if (for_depth < 0)
								appendStringInfo(&out, " /*:: %s */", mapped);
							else
								appendStringInfoChar(&out, ' ');
							p = te;
						}
						last_for = false;
						continue;
					}
				}
			}
			last_for = (p - ws == 3 && strncmp(ws, "for", 3) == 0);
			continue;
		}
		if (*p != ' ' && *p != '\t' && *p != '\n' && *p != '\r')
			last_for = false;
		appendStringInfoChar(&out, *p);
		p++;
	}
	return out.data;
}


void
plx_brace_parse_body(Ctx *cx)
{
	/* TypeScript: rewrite "id: T" annotations before lexing (the original TS is
	 * still embedded via the unchanged body). */
	if (cx->surf->ts_types)
		cx->body = ts_preprocess(cx->body);
	plx_lex(cx);
	parse_brace_program(cx);
}



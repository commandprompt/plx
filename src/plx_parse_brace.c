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

/*
 * The loops and switches being parsed, innermost first. A break nested in a
 * switch arm (in an if or try, not inside a loop opened in the arm) leaves
 * the switch, and PHP's break N / continue N count both kinds. A construct
 * gets a plpgsql label only when such an exit needs one; the label is
 * inserted at the construct's start offset when the construct closes, so code
 * without these exits is emitted as before. plx_brace_parse_body clears the
 * stack, so an error raised mid-parse leaves nothing behind.
 */
typedef struct BraceBrk
{
	Ctx		   *cx;
	bool		is_switch;
	bool		user_label;		/* label came from the source (name: for) */
	int			loopdepth;		/* cx->loopdepth inside the construct */
	int			fin_id;			/* try-with-finally frame of the construct */
	int			start;			/* cx->out offset of the construct */
	char	   *label;			/* plpgsql label, or NULL while none is needed */
	struct BraceBrk *outer;
} BraceBrk;

static BraceBrk *cur_brk = NULL;
static char *pending_label = NULL;	/* source label for the next loop */

static void
brk_begin(Ctx *cx, BraceBrk *b, bool is_switch, int start)
{
	b->cx = cx;
	b->is_switch = is_switch;
	b->loopdepth = cx->loopdepth + (is_switch ? 0 : 1);
	b->fin_id = cx->fin ? cx->fin->id : 0;
	b->start = start;
	b->label = NULL;
	b->user_label = false;
	if (!is_switch && pending_label)
	{
		b->label = pending_label;
		b->user_label = true;
	}
	pending_label = NULL;
	b->outer = cur_brk;
	cur_brk = b;
}

/* the label an exit to b uses, registered so plx_emit_exit routes it
 * through any finally between the exit and b */
static const char *
brk_label(Ctx *cx, BraceBrk *b)
{
	PlxLabel   *l = palloc(sizeof(PlxLabel));

	if (!b->label)
		b->label = psprintf("__plx_%s_%d", b->is_switch ? "sw" : "lp", ++cx->subq);
	l->name = b->label;
	l->fin_id = b->fin_id;
	l->next = cx->labels;
	cx->labels = l;
	return b->label;
}

static void
brk_end(Ctx *cx, BraceBrk *b, int ind)
{
	StringInfo	o = &cx->out;
	StringInfoData w;

	cur_brk = b->outer;
	if (!b->label || b->user_label)
		return;
	/* a switch becomes <<l>> BEGIN CASE ... END CASE; END; a loop <<l>> LOOP */
	initStringInfo(&w);
	appendBinaryStringInfo(&w, o->data, b->start);
	plx_indent(&w, ind);
	appendStringInfo(&w, "<<%s>>\n", b->label);
	if (b->is_switch)
	{
		plx_indent(&w, ind);
		appendStringInfoString(&w, "BEGIN\n");
	}
	appendBinaryStringInfo(&w, o->data + b->start, o->len - b->start);
	if (b->is_switch)
	{
		plx_indent(&w, ind);
		appendStringInfoString(&w, "END;\n");
	}
	pfree(o->data);
	*o = w;
}

/* the body and END LOOP of a loop whose header starts at offset start */
static void
brace_loop_body(Ctx *cx, int ind, int start)
{
	BraceBrk	b;

	brk_begin(cx, &b, false, start);
	cx->loopdepth++;
	parse_brace_block(cx, ind + 1);
	cx->loopdepth--;
	plx_indent(&cx->out, ind);
	appendStringInfoString(&cx->out, "END LOOP;\n");
	brk_end(cx, &b, ind);
}

/*
 * break / continue with a number (a+1 is the T_INT). PHP counts the
 * enclosing loops and switches; continue that lands on a switch means break
 * in PHP, so it is rejected rather than guessed.
 */
static void
emit_leveled_exit(Ctx *cx, int a, int b, int ind)
{
	Tok		   *kw = &cx->t[a], *num = &cx->t[a + 1];
	bool		is_break = (kw->kw == KW_BREAK);
	const char *word = is_break ? "break" : "continue";
	BraceBrk   *t;
	int			n = 0, have = 0, i;

	if (cx->surf->var_sigil != '$')
		plx_err(cx, kw->line, "%s takes a loop label, not a number", word);
	if (b != a + 2)
		plx_err(cx, kw->line, "%s N must be followed by ';'", word);
	for (i = 0; i < num->len; i++)
	{
		if (num->s[i] < '0' || num->s[i] > '9' || n > 1000)
			plx_err(cx, kw->line, "%s level must be a small positive integer", word);
		n = n * 10 + (num->s[i] - '0');
	}
	if (n < 1)
		plx_err(cx, kw->line, "%s %d is not allowed; the level must be at least 1", word, n);
	for (t = cur_brk; t && t->cx == cx; t = t->outer)
		have++;
	if (n > have)
		plx_err(cx, kw->line, "%s %d needs %d enclosing loops or switches, but there are %d",
				word, n, n, have);
	for (t = cur_brk, i = 1; i < n; i++)
		t = t->outer;
	if (n == 1 && !t->is_switch)
	{
		/* the innermost loop: a plain EXIT / CONTINUE, as for a bare one */
		plx_emit_exit(cx, &cx->out, ind,
					  is_break ? PLX_EXIT_BREAK : PLX_EXIT_CONTINUE, NULL, NULL, NULL);
		return;
	}
	if (n == 1 && !is_break)
	{
		/* continue 1 in a switch: the enclosing loop, as for a bare continue */
		if (cx->loopdepth == 0)
			plx_err(cx, kw->line, "continue outside a loop");
		plx_emit_exit(cx, &cx->out, ind, PLX_EXIT_CONTINUE, NULL, NULL, NULL);
		return;
	}
	if (t->is_switch && !is_break)
		plx_err(cx, kw->line, "continue %d targets a switch; write break %d", n, n);
	plx_emit_exit(cx, &cx->out, ind, is_break ? PLX_EXIT_BREAK : PLX_EXIT_CONTINUE,
				  brk_label(cx, t), NULL, NULL);
}

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

	int			start;

	cx->pos++;
	paren_group(cx, &ca, &cb);
	start = o->len;
	plx_indent(o, ind);
	appendStringInfo(o, "WHILE %s LOOP\n", plx_rw_range(cx, ca, cb, true));
	brace_loop_body(cx, ind, start);
}

/* emit `FOR <var> IN <query(...)> LOOP <body> END LOOP;` for a query receiver */
static void
brace_query_loop(Ctx *cx, Tok *var, int ra, int rb, int ind)
{
	StringInfo	o = &cx->out;
	int			sa[16], se[16], after, n;
	int			savedpos = cx->pos;
	int			start;

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
		start = o->len;
		plx_indent(o, ind);
		appendStringInfo(o, "FOREACH %.*s IN ARRAY %s%s LOOP\n",
						 var->len, var->s, arrx[0] == '[' ? "ARRAY" : "", arrx);
		brace_loop_body(cx, ind, start);
		return;
	}
	cx->pos = ra;
	n = plx_parse_args(cx, ra, sa, se, 16, &after);
	cx->pos = savedpos;
	if (n < 1)					/* sa[0]/se[0] are unset when n == 0 */
		plx_err(cx, cx->t[ra].line, "query requires a SQL argument");
	if (!plx_is_param(cx, var->s, var->len))
	{
		PlxLocal2  *l = plx_local_find(cx, var->s, var->len);

		if (!l)
			l = plx_local_add(cx, var->s, var->len);
		l->is_record = true;
	}
	start = o->len;
	plx_indent(o, ind);
	{
		char	   *sqlv, *binds;

		if (!plx_sql_arg(cx, sa, se, n, false, &sqlv, &binds))
			appendStringInfo(o, "FOR %.*s IN %s LOOP\n", var->len, var->s, sqlv);
		else if (binds)
			appendStringInfo(o, "FOR %.*s IN EXECUTE %s USING %s LOOP\n",
							 var->len, var->s, sqlv, binds);
		else
			appendStringInfo(o, "FOR %.*s IN EXECUTE %s LOOP\n",
							 var->len, var->s, sqlv);
	}
	brace_loop_body(cx, ind, start);
}

/* ---- counting-for checks (see parse_for_brace) ---- */

static bool
same_ident(Tok *a, Tok *b)
{
	return a->kind == T_IDENT && b->kind == T_IDENT && a->len == b->len &&
		strncmp(a->s, b->s, a->len) == 0;
}

/* an operator that compares, as opposed to one that assigns */
static bool
op_is_comparison(Tok *t)
{
	static const char *const cmp[] = {"==", "===", "!=", "!==", "<=", ">=", "<=>", NULL};
	int			i;

	for (i = 0; cmp[i]; i++)
		if (plx_tok_is(t, cmp[i]))
			return true;
	return false;
}

/* = += .= =- ... (any operator holding '=' that is not a comparison) */
static bool
op_assigns(Tok *t)
{
	return t->kind == T_OP && memchr(t->s, '=', t->len) && !op_is_comparison(t);
}

static bool
op_incdec(Tok *t)
{
	return t->kind == T_OP && t->len >= 2 &&
		(strncmp(t->s, "++", 2) == 0 || strncmp(t->s, "--", 2) == 0);
}

static bool
op_ends_incdec(Tok *t)
{
	return t->kind == T_OP && t->len >= 2 &&
		(strncmp(t->s + t->len - 2, "++", 2) == 0 ||
		 strncmp(t->s + t->len - 2, "--", 2) == 0);
}

/*
 * Does [a, b) hold, outside parentheses and brackets, anything that makes it
 * more than one value: a comparison, a logical operator, an assignment, an
 * increment, a ternary or a comma?
 */
static bool
range_not_single_value(Ctx *cx, int a, int b)
{
	int			i, depth = 0;

	for (i = a; i < b; i++)
	{
		Tok		   *t = &cx->t[i];

		if (t->kind == T_LPAREN || t->kind == T_LBRACKET)
			depth++;
		else if (t->kind == T_RPAREN || t->kind == T_RBRACKET)
			depth--;
		else if (depth > 0)
			continue;
		else if (t->kind == T_COMMA || t->kind == T_PIPE)
			return true;
		else if (t->kind == T_KW &&
				 (t->kw == KW_AND || t->kw == KW_OR || t->kw == KW_NOT))
			return true;
		else if (t->kind == T_OP &&
				 (op_is_comparison(t) || op_assigns(t) || op_incdec(t) ||
				  plx_tok_is(t, "<") || plx_tok_is(t, ">") ||
				  plx_tok_is(t, "&&") || plx_tok_is(t, "||") ||
				  plx_tok_is(t, "?") || plx_tok_is(t, ":") || plx_tok_is(t, "??") ||
				  plx_tok_is(t, "!")))
			return true;
	}
	return false;
}

/*
 * Does [a, b) assign the variable named by tok i? Counted: name = ..,
 * name op= .., name++, ++name, and PHP's append name[] = ... An element
 * assignment name[k] = .. does not change the variable's length or bound.
 */
static bool
range_assigns(Ctx *cx, int a, int b, Tok *name)
{
	int			i;

	for (i = a; i < b; i++)
	{
		Tok		   *n;

		if (!same_ident(&cx->t[i], name))
			continue;
		if (i > a && op_ends_incdec(&cx->t[i - 1]))
			return true;
		n = &cx->t[i + 1];
		if (op_assigns(n) || op_incdec(n))
			return true;
		if (n->kind == T_LBRACKET && cx->t[i + 2].kind == T_RBRACKET &&
			op_assigns(&cx->t[i + 3]))
			return true;
	}
	return false;
}

/*
 * Is the variable read after the loop that ends before token from? A read
 * counts unless an assignment at the loop's own block level (a plain
 * "v = ..", a later "for (v = ..", or "as v") comes first. Conservative: a
 * read anywhere after the loop counts.
 */
static bool
read_after_loop(Ctx *cx, int from, Tok *v)
{
	int			i, depth = 0;

	for (i = from; cx->t[i].kind != T_EOF; i++)
	{
		Tok		   *t = &cx->t[i];

		if (t->kind == T_LBRACE)
			depth++;
		else if (t->kind == T_RBRACE)
			depth = depth > 0 ? depth - 1 : 0;
		if (!same_ident(t, v))
			continue;
		if (depth == 0 && i > 0)
		{
			Tok		   *pv = &cx->t[i - 1];

			if (pv->kind == T_KW && pv->kw == KW_AS)
				return false;
			if (plx_tok_is(&cx->t[i + 1], "=") &&
				(pv->kind == T_SEMI || pv->kind == T_NEWLINE ||
				 pv->kind == T_LBRACE || pv->kind == T_RBRACE ||
				 (pv->kind == T_LPAREN && i > 1 && cx->t[i - 2].kind == T_KW &&
				  cx->t[i - 2].kw == KW_FOR)))
				return false;
		}
		return true;
	}
	return false;
}

/* C-style counting for, or JS `for (v of query(...))` */
static void
parse_for_brace(Ctx *cx, int ind)
{
	StringInfo	o = &cx->out;
	int			ca, cb;
	int			s1 = -1, s2 = -1, of_at = -1, i, depth = 0;
	int			line = cx->t[cx->pos].line;
	int			start;
	bool		has_let = false;

	cx->pos++;
	paren_group(cx, &ca, &cb);
	/* strip a leading let/const/var declaration keyword */
	if (cx->t[ca].kind == T_KW && cx->t[ca].kw == KW_LET)
	{
		ca++;
		has_let = true;
	}
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

		/*
		 * plpgsql's integer FOR evaluates LO, HI and K once, keeps its own
		 * copy of the variable, and drops it when the loop ends. Lower only
		 * a loop where that gives the source language's result.
		 */
		{
			Tok		   *v = &cx->t[ia];
			int			ba, be, bdepth = 0;

			if (!same_ident(&cx->t[cca], v))
				plx_err(cx, line, "for-loop condition must test the loop variable \"%s\"; use a while loop for other conditions", var);
			if (!same_ident(&cx->t[xa], v))
				plx_err(cx, line, "for-loop step must change the loop variable \"%s\"", var);
			if (step ? range_not_single_value(cx, xa + 2, xb) : xb != xa + 2)
				plx_err(cx, line, "for-loop step must be 'v++' or 'v += K'");
			if (range_not_single_value(cx, cca + 2, ccb))
				plx_err(cx, line, "for-loop bound must be a single value, not a condition; use a while loop");
			if (range_not_single_value(cx, ia + 2, ib))
				plx_err(cx, line, "for-loop init must be 'v = LO'");
			/* the body: from '{' to its matching '}' */
			ba = cx->pos;
			if (cx->t[ba].kind != T_LBRACE)
				plx_err(cx, line, "expected '{'");
			for (be = ba; cx->t[be].kind != T_EOF; be++)
			{
				if (cx->t[be].kind == T_LBRACE)
					bdepth++;
				else if (cx->t[be].kind == T_RBRACE && --bdepth == 0)
					break;
			}
			if (range_assigns(cx, ba, be, v))
				plx_err(cx, line, "the for-loop body assigns the loop variable \"%s\"; use a while loop", var);
			for (i = cca + 2; i < xb; i++)
				if (cx->t[i].kind == T_IDENT && (i < ccb || i >= xa + 2) &&
					range_assigns(cx, ba, be, &cx->t[i]))
					plx_err(cx, line, "the for-loop body assigns \"%.*s\", which the loop bound or step uses; a counting for evaluates them once, so use a while loop",
							cx->t[i].len, cx->t[i].s);
			if (!has_let && cx->t[be].kind == T_RBRACE && read_after_loop(cx, be + 1, v))
				plx_err(cx, line, "the loop variable \"%s\" is read after the for loop, but a counting for does not keep its final value; use a while loop", var);
		}
		start = o->len;
		plx_indent(o, ind);
		if (lt)
			appendStringInfo(o, "FOR %s IN %s..(%s - 1)%s%s LOOP\n", var, lo, hi,
							 step ? " BY " : "", step ? step : "");
		else
			appendStringInfo(o, "FOR %s IN %s..%s%s%s LOOP\n", var, lo, hi,
							 step ? " BY " : "", step ? step : "");
	}
	brace_loop_body(cx, ind, start);
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

/*
 * The plpgsql condition for one catch class, the tokens [a, b).
 *   Exception, Throwable, Error (an optional leading \)  -> OTHERS
 *   a class plx_exc_class_to_condition knows, or DivisionByZeroError
 *   PG::CamelCase -> camel_case; plpgsql rejects an unknown condition name
 *     when it compiles the function
 * Any other class is an error: catching every error in its place would hide
 * the errors the handler was not written for.
 */
static const char *
catch_class_condition(Ctx *cx, int a, int b, int line)
{
	StringInfoData cls;
	const char *p, *c;
	int			i;

	initStringInfo(&cls);
	for (i = a; i < b; i++)
		if (cx->t[i].s)
			appendBinaryStringInfo(&cls, cx->t[i].s, cx->t[i].len);
	p = cls.data;
	while (*p == '\\')
		p++;
	if (*p == '\0')
		plx_err(cx, line, "catch requires an exception class before '|'");
	if (strcmp(p, "Exception") == 0 || strcmp(p, "Throwable") == 0 ||
		strcmp(p, "Error") == 0)
		return "OTHERS";
	if (strcmp(p, "DivisionByZeroError") == 0)
		return "division_by_zero";
	c = plx_exc_class_to_condition(p, (int) strlen(p));
	if (c)
		return c;
	if (strncmp(p, "PG::", 4) == 0 && p[4] >= 'A' && p[4] <= 'Z')
	{
		StringInfoData cond;
		const char *q;

		initStringInfo(&cond);
		for (q = p + 4; *q; q++)
		{
			if (*q >= 'A' && *q <= 'Z')
			{
				if (q > p + 4)
					appendStringInfoChar(&cond, '_');
				appendStringInfoChar(&cond, (char) (*q - 'A' + 'a'));
			}
			else if ((*q >= 'a' && *q <= 'z') || (*q >= '0' && *q <= '9'))
				appendStringInfoChar(&cond, *q);
			else
				break;
		}
		if (*q == '\0')
			return cond.data;
	}
	plx_err(cx, line, "unsupported exception class \"%s\"", p);
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
	PlxFin	   *fin;

	cx->pos++;					/* try */
	fin = plx_fin_push(cx);
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
		/* TypeScript catch (e: unknown): drop the type annotation, so the
		 * variable, not the type name, binds the exception */
		for (i = ca; i < cb; i++)
			if (plx_tok_is(&cx->t[i], ":"))
			{
				cb = i;
				break;
			}
		/* catch (\Class $e): last IDENT is $e; the class tokens precede it */
		for (i = cb - 1; i >= ca; i--)
			if (cx->t[i].kind == T_IDENT)
			{
				ev_a = i;
				break;
			}
		if (ev_a > ca)
		{
			/* catch (A | B $e) -> WHEN a OR b; a catch-all member wins */
			StringInfoData cl;
			int			s = ca;
			bool		all = false;

			initStringInfo(&cl);
			for (i = ca; i <= ev_a; i++)
				if (i == ev_a || cx->t[i].kind == T_PIPE)
				{
					const char *c = catch_class_condition(cx, s, i, cx->t[ca].line);

					if (strcmp(c, "OTHERS") == 0)
						all = true;
					appendStringInfo(&cl, "%s%s", cl.len ? " OR " : "", c);
					s = i + 1;
				}
			if (!all)
				cond = cl.data;
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

			plx_indent(&arms, ind + 1);
			appendStringInfo(&arms, "WHEN %s THEN\n", cond);
			appendStringInfoString(&arms, pfx);
			appendStringInfoString(&arms, hb);
			pfree(hb);
		}
		nrescue++;
		plx_skip_seps(cx);
	}
	plx_fin_pop(cx, fin);
	if (cx->t[cx->pos].kind == T_KW && cx->t[cx->pos].kw == KW_ENSURE)	/* finally */
	{
		StringInfoData saved = cx->out;

		cx->pos++;
		initStringInfo(&cx->out);
		parse_brace_block(cx, ind + 1);
		ensure_body = cx->out.data;
		cx->out = saved;
	}

	plx_emit_try(cx, o, ind, body, nrescue, arms.data, ensure_body, fin);
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
	/*
	 * Only [new] Class(args) is supported: before the '(' a class name
	 * (identifiers and PHP namespace backslashes), and nothing after the
	 * ')'. Any other expression would lose everything but the first call's
	 * first argument.
	 */
	if (call < p || cx->t[call].kind != T_IDENT)
		plx_err(cx, cx->t[a].line, "throw requires 'new Exception(...)'");
	for (i = p; i < call; i++)
		if (cx->t[i].kind != T_IDENT &&
			!(cx->t[i].len == 1 && cx->t[i].s && cx->t[i].s[0] == '\\'))
			plx_err(cx, cx->t[a].line, "throw requires 'new Exception(...)'");
	{
		int			sa[16], se[16], after, n;
		int			savedpos = cx->pos;

		cx->pos = call;
		n = plx_parse_args(cx, call, sa, se, 16, &after);
		cx->pos = savedpos;
		if (after != b)
			plx_err(cx, cx->t[a].line, "throw requires 'new Exception(...)'");
		plx_indent(&cx->out, ind);
		if (n >= 1)
		{
			char	   *m;

			cx->diag_msg = true;
			m = plx_rw_range(cx, sa[0], se[0], false);
			cx->diag_msg = false;
			appendStringInfo(&cx->out, "RAISE EXCEPTION '%%', %s;\n", m);
		}
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
	int			start = o->len;
	BraceBrk	sw;
	PlxLabel   *mark = cx->labels;

	cx->pos++;					/* switch */
	paren_group(cx, &ca, &cb);
	plx_indent(o, ind);
	appendStringInfo(o, "CASE %s\n", plx_rw_range(cx, ca, cb, false));
	if (cx->t[cx->pos].kind != T_LBRACE)
		plx_err(cx, line, "switch requires a '{' block");
	cx->pos++;
	brk_begin(cx, &sw, true, start);

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
				/* a bare break ends the arm; break with a label is a statement */
				if (st->kind == T_KW && st->kw == KW_BREAK &&
					cx->t[cx->pos + 1].kind != T_IDENT &&
					cx->t[cx->pos + 1].kind != T_INT)
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
				terminated = (st->kind == T_KW &&
							  (st->kw == KW_RETURN || st->kw == KW_RAISE ||
							   st->kw == KW_BREAK || st->kw == KW_NEXT));
				parse_brace_stmt(cx, ind + 2, false);
			}
		}
	}
	if (when_count == 0)
		plx_err(cx, line,
				"switch needs at least one case (a switch with only default is not supported)");
	/* no default arm: falling through is a no-op in the source language, but a
	 * plpgsql CASE with no match raises case_not_found */
	if (!seen_default)
	{
		plx_indent(o, ind + 1);
		appendStringInfoString(o, "ELSE\n");
		plx_indent(o, ind + 2);
		appendStringInfoString(o, "NULL;\n");
	}
	plx_indent(o, ind);
	appendStringInfoString(o, "END CASE;\n");
	cx->labels = mark;
	brk_end(cx, &sw, ind);
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
		PlxLabel   *mark = cx->labels;

		plx_indent(&cx->out, ind);
		appendStringInfo(&cx->out, "<<%.*s>>\n", tk->len, tk->s);
		plx_label_note(cx, tk->s, tk->len);
		cx->pos += 2;			/* consume name and ':' */
		pending_label = pnstrdup(tk->s, tk->len);
		/* the label is in scope only inside its loop: a later loop may
		 * reuse the name, and a break must not resolve to a closed one */
		parse_brace_stmt_inner(cx, ind, toplevel);
		pending_label = NULL;
		cx->labels = mark;
		return;
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
		else if (tk->kind == T_KW && (tk->kw == KW_BREAK || tk->kw == KW_NEXT) &&
				 cx->t[a + 1].kind == T_INT)
			emit_leveled_exit(cx, a, b, ind);
		else if (tk->kind == T_KW && tk->kw == KW_BREAK && b == a + 1 &&
				 cur_brk && cur_brk->cx == cx && cur_brk->is_switch &&
				 cur_brk->loopdepth == cx->loopdepth)
		{
			/* the innermost breakable construct is a switch: leave it */
			plx_emit_exit(cx, &cx->out, ind, PLX_EXIT_BREAK, brk_label(cx, cur_brk),
						  NULL, NULL);
		}
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
	cur_brk = NULL;
	pending_label = NULL;
	plx_lex(cx);
	parse_brace_program(cx);
}



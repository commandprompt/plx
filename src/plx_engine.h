/*
 * plx_engine.h — internal interface to the dialect-neutral transpiler engine.
 *
 * The shared engine (lexer, expression rewriter, leaf-statement emitter, symbol
 * table, string/interpolation decoding, and the final assemble) lives in
 * plx_transpile.c. Per-dialect front ends (plx_dialect_*.c, plx_parse_brace.c)
 * implement PlxSurface.parse_body and call back into the engine through the
 * `plx_*` entry points declared here. This header defines the shared token,
 * symbol-table, and context types those front ends operate on.
 */
#ifndef PLX_ENGINE_H
#define PLX_ENGINE_H

#include "lib/stringinfo.h"

#include "plx.h"
#include "plx_int.h"					/* Kw, PlxSurface, PlxFuncMeta */

/*
 * Cross-version shim. pg_noreturn (added in PG16) must be written as the very
 * first token of the declaration, before the storage class. On PG20+ (C23) it
 * expands to the standard [[noreturn]] attribute (strict placement); on older
 * releases it is the position-tolerant GCC/clang attribute.
 */
#ifndef pg_noreturn
#define pg_noreturn __attribute__((noreturn))
#endif

#define PLX_MAX_DEPTH 500			/* recursion cap (parsers + expr rewriter) */

/* stacked-diagnostics field bits (e.detail, e.hint, ...); used by the engine's
 * diagnostics emitter and by the dialect rescue/try parsers. */
#define PLX_DIAG_DETAIL     0x01
#define PLX_DIAG_HINT       0x02
#define PLX_DIAG_CONSTRAINT 0x04
#define PLX_DIAG_COLUMN     0x08
#define PLX_DIAG_TABLE      0x10
#define PLX_DIAG_SCHEMA     0x20
#define PLX_DIAG_DATATYPE   0x40

/* ---------------------------------------------------------------- tokens */

typedef enum
{
	T_EOF, T_NEWLINE, T_SEMI, T_IDENT, T_KW, T_INT, T_FLOAT, T_STRING,
	T_OP, T_LPAREN, T_RPAREN, T_LBRACKET, T_RBRACKET, T_LBRACE, T_RBRACE,
	T_PIPE, T_COMMA, T_DOT, T_TYPEANN, T_INDENT, T_DEDENT, T_ERROR
} TokKind;

typedef struct
{
	TokKind		kind;
	Kw			kw;
	const char *s;				/* pointer into source body */
	int			len;
	int			line;
	bool		sq;				/* single-quoted string */
	bool		fstr;			/* Python f-string (interpolating) */
	char		quote;			/* opening quote char: ' " or ` */
	const char *ann;			/* TYPEANN: type text start */
	int			annlen;
} Tok;

/* ---------------------------------------------------------------- symtab */

typedef struct PlxLocal2
{
	char	   *name;			/* lower-cased key */
	char	   *typ;			/* plpgsql type text, or NULL if RECORD */
	bool		is_record;
	bool		is_const;		/* CONSTANT declaration */
	char	   *init;			/* folded initializer text, or NULL */
	struct PlxLocal2 *next;
} PlxLocal2;

/* ---------------------------------------------------------------- context */

/*
 * The per-transpile context, threaded (as the "self" argument) through the
 * engine and every dialect front end. Mutable run state lives here; the
 * immutable per-dialect descriptor is cx->surf (see PlxSurface in plx_int.h).
 */
typedef struct PlxCtx
{
	const char *body;
	Tok		   *t;
	int			pos;
	const PlxFuncMeta *meta;
	const PlxSurface *surf;
	StringInfoData out;			/* emitted BEGIN..END body */
	PlxLocal2  *locals, *ltail;
	int			loopdepth;
	int			depth;			/* recursion depth guard */
	int			handlerdepth;	/* inside a rescue handler body */
	int			subq;			/* __plx_fo_N / __plx_p_N counter */
	const char *exc_var;		/* current rescue exception var name */
	int			exc_varlen;
	int			diag_mask;		/* stacked-diagnostics fields used in this handler */
	bool		retset;
	MemoryContext mcx;			/* caller-supplied scratch context (reserved) */
} Ctx;

/* ------------------------------------------------------ engine entry points */

/* Errors */
pg_noreturn void plx_err(Ctx *cx, int line, const char *fmt,...) pg_attribute_printf(3, 4);

/* Lexer (shared by the text-family front ends) */
void plx_lex(Ctx *cx);

/* Expression rewriting */
char *plx_rewrite_expr(Ctx *cx, const char *s, int len, bool boolctx);
char *plx_rw_range(Ctx *cx, int a, int b, bool boolctx);
char *plx_span_text(Ctx *cx, int a, int b);

/* Leaf-statement emission + intrinsics */
void plx_emit_core(Ctx *cx, int a, int b, int ind, bool toplevel);
void plx_emit_leaf(Ctx *cx, int a, int b, int ind, bool toplevel);
void plx_emit_raise_call(Ctx *cx, int a, int ind);
void plx_emit_string_as_sql(Ctx *cx, Tok *tk, StringInfo out);
int plx_parse_args(Ctx *cx, int a, int *as, int *ae, int maxargs, int *after);
char *plx_binds_text(Ctx *cx, int *as, int *ae, int nargs);
bool plx_arg_is_string_literal(Ctx *cx, int as, int ae);
const char *plx_exc_class_to_condition(const char *s, int len);
char *plx_diag_prefix(int mask, int ind);

/* Token / span / symbol-table helpers */
void plx_skip_seps(Ctx *cx);
bool plx_is_ident(char c);
bool plx_is_ident_start(char c);
bool plx_tok_is(Tok *tk, const char *op);
bool plx_name_eq(Tok *tk, const char *w);
int plx_stmt_end(Ctx *cx, int from);
void plx_indent(StringInfo o, int n);
PlxLocal2 *plx_local_add(Ctx *cx, const char *name, int len);
PlxLocal2 *plx_local_find(Ctx *cx, const char *name, int len);
bool plx_is_param(Ctx *cx, const char *name, int len);

/* ---------------------------------------------- dialect front ends (vtable) */
/* Each dialect wires a parse_body into its PlxSurface (see plx_dialect_*.c);
 * the function transforms cx->body into cx->out. Only the brace front end is
 * shared across translation units (php / js / ts) and so needs a prototype
 * here; every other parse_body is static to its own dialect file. */
void plx_brace_parse_body(Ctx *cx);		/* php / js / ts (ts sets ts_types) */

#endif							/* PLX_ENGINE_H */

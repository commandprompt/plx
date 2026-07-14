# plx Transpiler: Design Specification

> This is the original design specification for the Ruby transpiler, written
> before implementation. It documents the approach and the reasoning behind it.
> The extension as built follows this design, with these differences: the
> product name is plx (not "plexcellent"); the transpiler was generalized to a
> `PlxSurface` so that PHP, JavaScript, and Python reuse the shared framework
> (see [ARCHITECTURE.md](ARCHITECTURE.md)); and everything links into a single
> `plx.so` rather than separate per-dialect modules. For the as-built
> architecture and the per-dialect behavior, see ARCHITECTURE.md and the dialect
> chapters. This file is kept as a design reference.

This is the specification for the Ruby surface. The transpiler runs at
`CREATE FUNCTION` time inside `plx_generic_validator`, rewrites `pg_proc.prosrc`
from the Ruby dialect to canonical plpgsql text, and leaves
`pg_language.lanplcallfoid` pointing at plpgsql's `plpgsql_call_handler`, so
run time is plpgsql.

---

## 1. Overview & the statement-restructurer philosophy

We do **not** write a Ruby parser. plpgsql expressions *are* SQL expressions, so the transpiler is a **statement-level restructurer**:

1. It isolates *syntactic containers* (strings, comments, `#{}` interpolations, heredocs, brackets) with a byte-level lexer so that a keyword/brace/`end` inside a string is never mistaken for structure and vice-versa.
2. It finds statement boundaries and block open/close, building a `PlxBlock` tree.
3. It lowers each recognized construct to plpgsql, hoisting typed `DECLARE`s, rewriting a **small fixed set** of tokens/operators/interpolations, and otherwise passing expression text through to plpgsql/SQL verbatim.

**Non-negotiable invariant that drives every fix below:** rewriting, interpolation-splicing, quote-escaping, and `||`→`OR` mapping are all performed in **one left-to-right pass over the SOURCE tokens**, honoring the same opaque-container state the lexer used. Output bytes the transpiler generates (e.g. a concat `||`) are **never re-scanned**. Origin (source-authored vs transpiler-generated) is the sole discriminator for the `||` concat-vs-`OR` ambiguity. Any multi-token predicate the rewriter produces is **self-parenthesized** before substitution into an outer slot.

Semantics are pinned to SQL/plpgsql three-valued logic, with a small set of Ruby-fidelity adjustments (nil-interpolates-as-`''`, null-safe `==`, falsey-aware negation) documented in §6.

Final assembly (Strategy A):

```
/*plx:v1:plruby:<bodyhash>*/
[DECLARE
  <decls...>
]
BEGIN
  <emitted body>
END;
/*plx-orig:b64$<base64(original ruby body)>$plx-orig*/
```

---

## 2. Lexer / scanner spec

### 2.1 Token & lexer C structs (`plx_scan.h`)

```c
typedef enum {
  PLX_TOK_EOF, PLX_TOK_NEWLINE, PLX_TOK_SEMI, PLX_TOK_IDENT, PLX_TOK_KEYWORD,
  PLX_TOK_INT, PLX_TOK_FLOAT, PLX_TOK_STRING, PLX_TOK_SYMBOL, PLX_TOK_OP,
  PLX_TOK_LPAREN, PLX_TOK_RPAREN, PLX_TOK_LBRACKET, PLX_TOK_RBRACKET,
  PLX_TOK_LBRACE, PLX_TOK_RBRACE, PLX_TOK_PIPE, PLX_TOK_COMMA, PLX_TOK_DOT,
  PLX_TOK_TYPEANN, PLX_TOK_COMMENT, PLX_TOK_HEREDOC, PLX_TOK_ERROR
} PlxTokKind;

typedef enum {
  KW_IF, KW_UNLESS, KW_ELSIF, KW_ELSE, KW_THEN, KW_END, KW_WHILE, KW_UNTIL,
  KW_FOR, KW_IN, KW_DO, KW_BEGIN, KW_RESCUE, KW_ENSURE, KW_CASE, KW_WHEN,
  KW_RETURN, KW_NEXT, KW_BREAK, KW_RAISE, KW_AND, KW_OR, KW_NOT, KW_EMIT,
  KW_RETURN_NEXT, KW_PERFORM, KW_EXECUTE, KW_DEF, KW_NIL, KW_TRUE, KW_FALSE,
  KW_LOOP, KW_NONE
} PlxKw;

typedef struct { const char *start; int len; } PlxInterp;

typedef struct {
  PlxTokKind kind; PlxKw kw;
  const char *start; int len;        /* pointer INTO source body, no copy */
  int32 line, col;
  bool  stmt_start;                  /* first significant token of a logical stmt */
  bool  sq_string;                   /* STRING: single-quoted (no interp) vs double */
  int32 interp_first, interp_count;  /* index into PlxLexer.interps */
} PlxToken;

typedef struct {
  char  term[NAMEDATALEN];
  bool  squiggly, dash, interp;      /* <<~ / <<- ; bare/"IDENT" interp, 'IDENT' no interp */
  int32 tok_index;                   /* the HEREDOC placeholder token to backfill */
} PlxHeredoc;

typedef struct {
  const char *src, *p, *end;
  int32 line, col, paren_depth, prev_sig;   /* prev_sig = index of prev significant tok */
  PlxToken *toks; int32 ntoks, cap;
  PlxInterp *interps; int32 ninterps, icap;
  PlxHeredoc pending[MAX_PENDING_HD]; int32 npending;
  MemoryContext mcx;
} PlxLexer;

typedef struct { int line, col; char *msg; } PlxError;
```

### 2.2 Pass 1 — byte-level lexer (`plx_lex`)

Skip inline whitespace (space/tab, **not** newline). Dispatch on current char:

- **letter/`_`** — scan `[A-Za-z_][A-Za-z0-9_]*` plus a trailing `?`/`!`. Keyword-table `bsearch` → `PLX_TOK_KEYWORD` with a `PlxKw`, else `PLX_TOK_IDENT`.
- **digit** — scan number. **Range guard:** on hitting `.`, peek `p[1]`; if it is also `.`, STOP the number (emit `INT`, let `..`/`...` become an OP). A single `.` before a digit is a float fraction. Underscores in numeric literals (`1_000`) are consumed and stripped at rewrite. Record whether the literal exceeds int4 range for the type-inference pass.
- **`'`** — single-quoted string. Only `\\` and `\'` are escapes; **everything else, including `#{`, is inert literal text.** One opaque `PLX_TOK_STRING` with `sq_string=true`, `interp_count=0`.
- **`"`** — double-quoted string. Escapes `\" \\ \n \t \r \0 \e \s \a \b \f \v \nnn \xHH \uXXXX`. **`\#{` (backslash before `#{`) suppresses interpolation** — the two chars `#{` become literal content. On an *unescaped* `#{`, push an interp context and scan a **balanced** sub-expression to the matching `}`: brace counter starts at 1, `++` on `{`, `--` on `}`, but only when not inside a nested string; nested `"..."`/`'...'` and nested `#{}` are honored. Record each interp `(start,len)` span. Empty/whitespace-only `#{}` is recorded with `len==0` (dropped later). One opaque `PLX_TOK_STRING`, `sq_string=false`, carrying its span list.
- **`%`** — percent-literal only in **value position** (prev significant token is OP/`(`/`,`/keyword/terminator) and `p[1]∈{q,Q,w,W,i,I}` with a delimiter following: `%q`=single semantics, `%Q`=double/interp, bracket delimiters `()[]{}<>` nest. `%w`/`%i` (word/symbol arrays) are lexed as an opaque STRING but flagged unsupported → rejected in lowering. Otherwise `%` is the modulo OP.
- **`<`** — `<<~IDENT` / `<<-IDENT` / `<<IDENT` / `<<"IDENT"` / `<<'IDENT'` in value position ⇒ heredoc: register a pending `PlxHeredoc`, emit a `PLX_TOK_HEREDOC` placeholder; capture the body lazily at the next newline. Otherwise `<`/`<<`/`<=` OP. **Descope fallback:** if heredoc support is disabled, DETECT-AND-REJECT with a precise error — **never fall through and mis-scan.**
- **`#`** — if `p[1]==':' && p[2]==':'` ⇒ `PLX_TOK_TYPEANN`, capturing the rest of the line as type text (tested **before** plain comment). Else `#`..EOL is a `PLX_TOK_COMMENT` (dropped; its newline preserved). This dispatch runs **only in code position** — a `#` inside a string/interp/heredoc is already consumed as string bytes above, so a bare `#` inside `"50% off # ends soon"` is never a comment.
- **`\n`** — candidate `PLX_TOK_NEWLINE`; apply suppression (§2.3). Then drain any pending heredocs, matching the terminator at line-start (`<<~` strips common leading indent; `<<-` allows an indented terminator).
- **`;`** — `PLX_TOK_SEMI` (hard separator, outside strings/comments/brackets).
- **`( ) [ ] { } , .`** — punctuation; maintain `paren_depth` over `()`+`[]`. `{`/`}` get their own kinds; disambiguated in pass 2.
- **`|`** — `PLX_TOK_PIPE`; adjacent `||` still lexes as one OP run.
- **`:`** — `PLX_TOK_SYMBOL` when followed by an ident char AND prev token is not a value; else OP (ternary `:`).
- **operator chars** `= ! < > + - * / & ? ~ ^` — scan the maximal run into one `PLX_TOK_OP` (`==`,`!=`,`&&`,`||`,`<=`,`>=`,`..`,`...`,`=>`,`<<`,`**` come through whole).
- **else** — record first error, `PLX_TOK_ERROR`.

After each terminator, set `stmt_start=true` on the next significant token. **`stmt_start` bookkeeping is a first-class correctness surface** (opener vs modifier depends on it) and must be re-asserted correctly after a suppressed newline and after a heredoc body drain.

### 2.3 Newline suppression (the single audited table)

A physical `\n` is DROPPED (statement continues) iff **any** of:
1. `paren_depth > 0`;
2. prev significant token **cannot end an expression**: a binary/prefix OP (`+ - * / % = == != < > <= >= && || .. ... => . , & | ^ << ** ? :`), a `,`, a `.`, an open `( [ {`, a `PIPE`, or keyword `and`/`or`/`not`;
3. a trailing backslash `\` precedes the newline;
4. leading-continuation: peeking past whitespace/comments on the **next** line, its first significant token is `.`, `&&`, or `||`.

Otherwise emit `PLX_TOK_NEWLINE`. `;` is always a separator. The cannot-end set is one `static const` classifier keyed on the OP token bytes plus a short `strcmp` for `..`/`...`/`=>`.

### 2.4 Pass 2 — block builder (`plx_build_blocks`)

Push `BLK_TOPLEVEL`. Walk tokens, cutting statements at `NEWLINE`/`SEMI` in the current frame. Classify each statement's leading `stmt_start` token:

- **Openers** (`if unless while until for begin case`, and `loop` immediately followed by `do`) → create a `PlxStmt`, push a matching `PlxBlock` frame.
- **`do`** at an iterator statement's tail (`....each do`, `for ... do`, `loop do`) and a **`{` in iterator position** → open an ITER body block; an immediately-following `|...|` captures the loop var name(s) onto the frame.
- **Mid-block continuers** `elsif else then when in rescue ensure` → must match the top frame's kind; close the current arm and open the next (alt chain), not a new frame.
- **`end`** → pop the top frame (generic; kind kept for diagnostics). **`}`** → pop, erroring unless the top is a brace-block.
- **Modifier** `if/unless/while/until` NOT at `stmt_start` → left inside the statement span, no frame. `stmt_start` is the sole discriminator.
- **`def` anywhere** → hard error (no nested defs). **Bare `{`** not in iterator position and not `#{` → hash literal → precise "hash/`{}` literal unsupported" error.
- everything else → leaf statement handed to the rewriter+lowering.

At EOF, a non-`[TOPLEVEL]` stack → `ereport` "unterminated `<kind>` block opened at line N"; a stray `end`/`}` → matching-close error at its own line.

Every token carries a 1-based dialect line; every block records `open_line` — feeding the plpgsql→dialect source map and line-accurate errors.

---

## 3. Statement IR & parse strategy

```c
typedef enum {
  PLX_ASSIGN, PLX_IF, PLX_WHILE, PLX_FORINT, PLX_FORQUERY, PLX_LOOP,
  PLX_FETCH, PLX_PERFORM, PLX_EXECUTE, PLX_RETURN, PLX_RETURN_NEXT,
  PLX_RETURN_QUERY, PLX_RAISE, PLX_NEXT, PLX_BREAK, PLX_EXCEPTION
} PlxNodeKind;

typedef struct PlxNode {
  PlxNodeKind kind; int ruby_line;
  char *a, *b, *c, *d;      /* pre-rewritten slots, meaning per-kind */
  bool neg, strict, dynamic, reverse, exclusive; /* unless/until; fetch_one!; EXECUTE; downto/neg-step; ... */
  char *step;               /* FORINT BY expr, NULL if none */
  List *body;               /* List* of PlxNode */
  List *arms;               /* PlxArm* (IF elsif/else; EXCEPTION when-clauses) */
  List *using_args;         /* char* bind exprs for dynamic forms */
} PlxNode;

typedef struct PlxArm { char *cond_or_condname; List *body; bool is_else; } PlxArm;
```

`unless`→`PLX_IF` with `neg`; `until`→`PLX_WHILE` with `neg`; `loop do`→`PLX_LOOP`.

**Parse pipeline** (all statement-level; no Ruby expression parser):

1. `plx_lex` → token vector.
2. `plx_build_blocks` → `PlxBlock` tree (keyword-driven frame stack; recognizes `.each`/`.each_with_index`/`loop do`/modifier forms; `.each`-family recognized by matching `… .each[_with_index]? do |vars|` or `… .each { |vars| … }` at a statement tail).
3. Each recognized construct calls a `plx_lower_*` builder that (a) runs `plx_rewrite_expr` on every expression slot, (b) registers `PlxLocal`s in the symtab, (c) validates subset membership → `ereport(ERROR, errcode(ERRCODE_FEATURE_NOT_SUPPORTED), …)` on anything out of scope.
4. `plx_emit` walks the tree to a `StringInfo`; `plx_emit_declare` writes the preamble.

The parse ctx carries a `loop_depth` and a `handler_depth` counter (for `next`/`break`/bare-`raise` legality) and an **active-loop-var stack** (for the sibling-loop rule, §5).

---

## 4. Complete lowering table (every adversarial fix applied)

Emission mechanics: 2-space indent per level, body at level 1. Leaf: `<indent><text>;\n` (node supplies text sans `;`; emitter appends exactly one). Compound opener line ends in the plpgsql keyword (`THEN`/`LOOP`) with **no** `;`; children at indent+1; closers `END IF;`/`END LOOP;`/`END;`. Subquery aliases `__plx_fo_N`/`__plx_p_N` use a monotonic counter.

### PLX_ASSIGN — `LHS := RHS;`
`x = e` → `x := e';`. Compound `x += e` → `x := x + (e');` (same for `-= *= /= %=`). Multi-assign `a, b = e1, e2` → `SELECT e1', e2' INTO a, b;` (simultaneous → `a,b=b,a` swaps correctly). LHS must be a bare local or an OUT/TABLE-column param (from meta); assigning to `row.field` is rejected. **`||=`/`&&=` are rejected** (no faithful three-valued lowering) — message points at the explicit form, which itself lowers per the value-context `||` rule (§6). RHS-intrinsics (`fetch_one`, `query`, `emit`, …) are recognized **first** and routed to their own node; they are never assignments.

### PLX_IF — if/elsif/else, unless, modifier-if
```
IF C' THEN
  <arm0>
ELSIF C2' THEN
  <arm1>
ELSE
  <armN>
END IF;
```
`unless C` → `IF (C') IS NOT TRUE THEN … END IF;` (**not** `NOT (C')` — matches Ruby falsey nil/false; see §6). Modifier `STMT if C` → `IF C' THEN STMT; END IF;`. Positive `if C` uses `C'` directly (NULL correctly skips the THEN arm). Condition must be boolean; **no truthiness coercion** — a bare non-boolean condition is a CREATE-time reject (§6/§7), not `IF <int>`.

### PLX_WHILE — while/until
`while C` → `WHILE C' LOOP … END LOOP;`. `until C` → `WHILE (C') IS NOT TRUE LOOP … END LOOP;`. Modifiers `STMT while C` / `STMT until C` expand to the full loop. `begin…end while` (do-while) is rejected.

### PLX_LOOP — `loop do … end`
```
LOOP
  …
END LOOP;
```
Requires a reachable `break`/`return` (not statically enforced).

### PLX_FORINT — integer for
Surfaces: `for v in LO..HI`, `for v in LO...HI`, `(LO..HI).each do |v|`, `(LO...HI).each`, `HI.downto(LO) do |v|`, `LO.upto(HI) do |v|`, `(LO..HI).step(k).each`, `LO.step(HI,k)`.

```
FOR v IN [REVERSE] LO'..HI' [BY k'] LOOP … END LOOP;
```

Fixes folded in:
- **Exclusive `...` is a property of the range node**, applied on every path: `LO'..(HI' - 1)`. So `(0...9).step(3)` → `FOR v IN 0..(9 - 1) BY 3 LOOP` (yields 0,3,6 — endpoint 9 correctly excluded). Guard the `(HI-1)` rewrite against INT_MIN underflow.
- `downto` → `REVERSE HI'..LO'`. `upto` → ascending.
- **`.step(k)` with a literal negative k** → `FOR v IN REVERSE LO'..HI' BY (-k) LOOP` (Numeric#step limit is inclusive; no `-1`). Never emit `BY <negative>`.
- **`.step(k)` with a non-literal k** → emit ascending `BY k'` and **document that k must be `>0` at runtime** (plpgsql cannot pick direction dynamically); optionally a CREATE-time NOTICE.
- **Fractional step literal** (`2.5`) → reject: `plruby: fractional step in .step(...) is not supported (integer FOR loops only)`.
- **Endless/beginless range** (`1..`, `..5`) → reject.
- **Non-integer literal endpoint** (`1.0..5.0`) → reject (integer FOR only).
- **Literal endpoint outside int4 range** (`1..10000000000`) → reject at CREATE (plpgsql loop var is int4) rather than deferring to a runtime overflow.

Loop var `v` is loop-local, plpgsql-declared, **not hoisted**; endpoints pass through to plpgsql. `v` referenced **outside** the loop body → reject `plruby: for-loop variable "v" is not visible after the loop` (Ruby's `for` leak is intentionally dropped and detected, not silently mis-emitted).

### PLX_FORQUERY — query iteration
`query(SQL[, args]).each do |row|` / `.each_with_index do |row, idx|`.

**Static** (SQL is a string literal, no bind args). Interpolation `#{name}` inside the literal is spliced as a **bare plpgsql NAME reference** *only when it is in value position*:
```
FOR row IN SELECT id, amount FROM orders WHERE cust = cid LOOP … END LOOP;
```
**Identifier-position interpolation is undecidable without a SQL parse and MUST NOT be silently name-ref'd** (`#{tbl}` as a relation reads the literal name `tbl`, not the variable). Require the dynamic path (`format('… %I …', tbl)` via `execute`/explicit) or reject; document the restriction.

**Dynamic** (extra bind args or non-literal SQL):
```
FOR row IN EXECUTE <SQL-expr'> USING a', b' LOOP … END LOOP;
```

- **`row` is ALWAYS hoisted as `row RECORD;`** (mandatory — plpgsql auto-declares only *integer* FOR vars; a query FOR needs a pre-declared record var, else `"row" is not a known variable`). Register `row` in the active-loop-var stack for the body.
- `each_with_index`: the index var is a **synthesized local** routed through the symtab (`idx integer;`), registered in the active-loop stack and checked against params. Emit `idx := -1;` immediately before the FOR and inject `idx := idx + 1;` as the **very first** body statement (above any `next if` guard) → 0-based, matching Ruby.
- Field access `row.col`, `row[:col]`, `row['col']` all normalize to `row.col`.
- Nested/overlapping loops need distinct row names (checked against the active stack only — sequential sibling loops may reuse `row`, §5).

### PLX_FETCH — `fetch_one` → SELECT INTO
**Static:** wrap the user SQL so INTO needs no SQL parse:
```
SELECT * INTO [STRICT] <targets> FROM ( <SQL-spliced> ) AS __plx_fo_1;
```
**Dynamic** (bind args / non-literal): `EXECUTE <SQL-expr'> INTO [STRICT] <targets> USING args';`.

`fetch_one` non-STRICT (0 rows → all-NULL, extra rows → first); `fetch_one!` STRICT. Whole-row target hoists `RECORD`; scalar destructure targets need annotation/inference. Set-returning DML (`INSERT … RETURNING`) as the user SQL must use the dynamic/EXECUTE path (the subquery wrap fails on it). `<fetch-target>.nil?` (row-presence) lowers to **`NOT FOUND`** evaluated right after the INTO — not `IS NULL` (which is true only when *every* column is NULL; §6).

### PLX_PERFORM — run SQL, discard (static, literal, no binds)
DML (INSERT/UPDATE/DELETE/MERGE) → verbatim `…;`. Row-returning (SELECT/WITH/VALUES/TABLE) → `PERFORM * FROM ( <SQL> ) AS __plx_p_1;`. Value-position interpolation → name reference (identifier position → reject/dynamic, as FORQUERY).

### PLX_EXECUTE — dynamic SQL, discard
`execute(SQL_expr[, a, b])` → `EXECUTE <SQL-expr'> [USING a', b'];`. Always dynamic. Interpolation `#{e}` → runtime concat `|| COALESCE((e)::text,'')` (Ruby nil→''; also prevents `EXECUTE NULL`). User owns injection safety; recommend `USING`/`format(%I,%L)`.

### PLX_RETURN
`return` → `RETURN;`. `return e` → `RETURN e';`. In a SETOF function `return e` is rejected (use `emit`/`return_next`/`return_query`); bare `return` → `RETURN;`. `return a, b` with matching OUT params → `out1 := a'; out2 := b'; RETURN;`. **No implicit last-expression return** — a scalar function must end with explicit `return e`.

### PLX_RETURN_NEXT — emit / return_next
Bare `emit`/`return_next` → `RETURN NEXT;`. `emit(e)`/`return_next e` → `RETURN NEXT e';`. Requires `meta.retset`, else hard error.

### PLX_RETURN_QUERY
Static: `RETURN QUERY <SQL-spliced>;`. Dynamic: `RETURN QUERY EXECUTE <SQL-expr'> USING args';`. Requires `meta.retset`.

### PLX_RAISE
Surfaces: `raise "msg"` (→EXCEPTION); `raise LEVEL: "msg"` with `LEVEL∈{notice,warning,info,log,debug,exception}`; option keys `errcode: detail: hint: column: constraint: message:`; class form `raise PG::UniqueViolation, "m"`; **bare `raise`**.

**Message building — the RAISE fixes:**
- The message is split into literal chunks + interpolation spans **first**. `'`→`''` and `%`→`%%` escaping is applied to **literal chunks only**; interpolation expressions are `plx_rewrite_expr`'d and emitted **verbatim** as ordered arguments (their internal `'` and `%`, e.g. `a % b` modulo or `f('x')`, are never touched).
- **Placeholder adjacent to a literal `%`** cannot be represented positionally (a `%`-run parses pairs-first). When any interpolation is immediately followed by a literal `%` — and, safely, whenever a literal `%` co-occurs with interpolation — **lower the whole message through the single-placeholder concat form** instead:
  ```
  RAISE NOTICE '%', 'discount ' || COALESCE((pct)::text,'') || '%';
  ```
  This eliminates all `%`-ordering hazards. Literal-`%`-then-placeholder (`"done %#{n}"`) is safe under greedy literal-first parsing and may keep the `%`/arg form. A no-interpolation message with a literal `%` **still** escapes `%`→`%%` (else `too few parameters for RAISE`).
- **USING option values** (`detail:`/`hint:`/`column:`/`constraint:`/`errcode:`/`message:`) are plain scalar expressions with **no** format substitution: interpolation in them lowers via the ordinary `||`+COALESCE concat path, never `%`-placeholders.
  ```
  RAISE EXCEPTION 'bad row'
    USING DETAIL = 'value was ' || COALESCE((v)::text,''),
          HINT   = 'id='        || COALESCE((id)::text,'');
  ```
- **`message:` is mutually exclusive with a positional message** (plpgsql: `RAISE option already specified: MESSAGE`). Positional present + `message:` → reject. Only `message:` (no positional) → drive it through the format/`%` path as *the* message.
- **Bare `raise`** → `RAISE;` (re-raise); legal only inside a rescue/EXCEPTION handler (`handler_depth > 0`), else a hard CREATE-time error mirroring plpgsql.
- **Exception class** → condition via a fixed table (`PG::UniqueViolation`→`unique_violation`, `PG::ForeignKeyViolation`→`foreign_key_violation`, `ZeroDivisionError`→`division_by_zero`, `PG::NoDataFound`→`no_data_found`, …). A class **not** in the table (`RuntimeError`, custom) → either reject with a precise "unsupported exception class" error or map to a defined default `ERRCODE = 'P0001'`; **never** pass the class name through as a condition. A raw string first arg is taken as a condition name / 5-char SQLSTATE.

### PLX_NEXT / PLX_BREAK
`next`→`CONTINUE;`, `break`→`EXIT;`. Modifiers: `next if C`→`CONTINUE WHEN C';`; `break if C`→`EXIT WHEN C';`; `next unless C`→`CONTINUE WHEN (C') IS NOT TRUE;`; `break unless C`→`EXIT WHEN (C') IS NOT TRUE;`. Must be inside a loop (`loop_depth>0`), else hard error. `break <expr>`, labeled break, `redo`, `retry` rejected.

### PLX_EXCEPTION — begin/rescue/ensure
Without `ensure`:
```
BEGIN
  <BODY>
EXCEPTION
  WHEN <cond1> THEN <handler1>
  WHEN OTHERS THEN <handlerN>
END;
```
With `ensure` (runs exactly once on both paths):
```
BEGIN
  BEGIN
    <BODY>
  EXCEPTION
    WHEN <cond> THEN <handler>
  END;
  <ENSURE>                 -- normal / handled path
EXCEPTION WHEN OTHERS THEN
  <ENSURE>                 -- unhandled / handler-raised path
  RAISE;
END;
```
`rescue => e` / bare `rescue` → `WHEN OTHERS THEN`. `e.message`→`SQLERRM`; `e.sqlstate`/`e.code`→`SQLSTATE`; `e.detail`/`e.hint`/`e.column`/`e.constraint` inject `GET STACKED DIAGNOSTICS __plx_e_detail = PG_EXCEPTION_DETAIL, …;` at handler top, refs rewritten to those temps. **Every synthesized temp (`__plx_e_*`) is routed through the symtab** so it gets a DECLARE line and participates in dedup/collision (§5). A bare `begin…end` (no rescue/ensure) is inlined (locals are hoisted globally, so no new scope needed). Handler body raises `handler_depth`.

---

## 5. DECLARE-hoisting & type inference

### 5.1 Scope & symtab
One flat, function-level `DECLARE`. All hoisted locals go to the outermost DECLARE even when first assigned in a nested `begin`/branch/loop (matches Ruby method-scope). Consequence: no block-local shadowing; a name = one variable.

```c
typedef enum { PLX_T_UNRESOLVED, PLX_T_NAMED, PLX_T_RECORD } PlxTypeKind;
typedef struct PlxLocal {
  char *name;               /* case-folded canonical key */
  char *orig_name;          /* first source spelling, for diagnostics */
  PlxTypeKind kind;
  char *typtext;            /* annotation string or inferred keyword */
  Oid typoid; int32 typmod; /* diagnostics only */
  int  first_line;
  char *init_literal;       /* foldable top-level constant init, else NULL */
  bool from_annotation, synthesized;
  struct PlxLocal *next;    /* insertion order → stable DECLARE order */
} PlxLocal;
typedef struct PlxSymtab {
  PlxLocal *head, *tail;
  HTAB *byname, *params;    /* case-folded name → PlxLocal* / param sentinel */
  List *active_loopvars;    /* stack of currently-open loop var names */
} PlxSymtab;
```

### 5.2 Seeding params (never hoist)
From `PlxFuncMeta->argnames[i]` for **all** argmodes (IN/OUT/INOUT/VARIADIC/TABLE). **Skip NULL/empty argnames** (`f(int, OUT numeric)` has unnamed args) — guard every `hash_search` key against NULL (a NULL key into a `HASH_STRINGS` dynahash segfaults). Case-fold with `downcase_truncate_identifier` (63-byte `NAMEDATALEN` truncation) to match plpgsql exactly.

### 5.3 Collection walk
Single recursive descent. On a **loop node**: push its var(s) onto `active_loopvars`, recurse, pop on close.
- Integer FOR var → registered active but **not hoisted** (plpgsql auto-declares int loop var).
- Query FOR record var → registered active **and** hoisted as `RECORD` (mandatory).
- `each_with_index` counter and every `__plx_e_*` temp → synthesized locals, hoisted and collision-checked like any other.

On an **assignment node**: classify LHS. Qualified (`row.f`) / subscripted (`a[i]`) → store into existing base, skip. In `params` or currently-active loop var → plain `:=`, skip. Else `hash_search(byname, HASH_ENTER)`: new → build `PlxLocal`, append to chain, resolve type; existing UNRESOLVED → try to resolve; annotation conflict → hard error. Standalone `v #:: T` → same create path with `from_annotation=true`.

### 5.4 Loop-var scoping fixes
- **Sibling (sequential, non-overlapping) loops may reuse a name** (`|row|` twice). The distinct-name error fires **only** for a name already on the `active_loopvars` stack (lexically nested/open). Sibling loops share one `row RECORD;` declaration.
- **`bound` is body-scoped.** After a loop closes, its var is a normal hoist candidate again. A post-loop `row = 5 #:: integer` is a legitimate scalar local; a flat plpgsql scope cannot hold both `row RECORD` and `row integer`, so detect the conflicting reuse and **reject** with a rename message (never silently drop the annotation or emit `row := 5` against a RECORD).

### 5.5 Type resolution (first-satisfied wins, source order)
1. **Annotation** `#:: T` (authoritative; overrides inference). Validate with `parseTypeString(str, &typoid, &typmod, escontext)` under an `ErrorSaveContext` → clean plruby ereport on unknown type. Keep the original string as `typtext` so `numeric(10,2)`/`text[]`/`myschema.t` survive. Two conflicting annotations for one name → hard error.
2. **Literal/obvious-cast inference** from the first RHS (raw tokens, no SQL eval; narrow whitelist): int-literal in int4 → `integer` (else `bigint`); decimal/exponent → `numeric` (**never float8**, documented divergence from Ruby Float); string → `text`; `true`/`false` → `boolean`; top-level cast tail `(e)::T` / `CAST(e AS T)` → `T` (validated); uniform array literal `[1,2]`→`integer[]`, `["a"]`→`text[]`. Bare `nil`/`NULL` → no type.
3. **Record inference:** whole-row `v = fetch_one(...)` → `RECORD`. A whole-row fetch target **forces RECORD and overrides** a scalar literal/annotation for the same name; a name used both as scalar and whole-row-fetch target → **reject** a precise conflict at CREATE (never emit `SELECT * INTO <scalar>`). Scalar-destructure targets are not typeable without a SQL parse → require annotation/literal, else fall to (4).
4. **Hard error** → `ereport(ERROR)` naming the var and first-assignment line: `plruby: cannot infer a PostgreSQL type for local variable "label" (first assigned at line 3); add an annotation, e.g. label = ... #:: text`.

### 5.6 Collision checks
- **Case-fold / truncation collision:** before folding, if two *distinct* source spellings (`userId`, `userid`, or names differing only past byte 63) fold to the same key → `ereport`: `identifiers "userId" and "userid" are indistinguishable to plpgsql (case-insensitive / 63-byte truncation); rename`. Never silently merge case-distinct Ruby locals.
- **Loop-var vs param:** for every loop var (primary record var AND `each_with_index` counter), if its name is in `params` → `ereport`: `plruby: loop variable "total" collides with an OUT parameter of the same name; rename it`. Hoisted locals can't collide with params (params excluded from hoisting by construction).
- **Reserved-prefix:** synthesized names use a reserved prefix (`__plx_`); any user identifier with that prefix → reject.

### 5.7 Emit
Empty symtab → no `DECLARE` keyword. Else `DECLARE\n` then one line per local in insertion order: NAMED → `  name typtext[ := init];`; RECORD → `  name RECORD;`. **Initializer folding** only for a top-level, first-reference, constant-literal assignment (drop the statement); otherwise DECLARE without init and keep the `:=` (always-safe default = no fold). Record the preamble line count as the constant source-map offset. `parseTypeString`/`format_type_be`/`downcase_truncate_identifier` are global backend symbols; `plpgsql_build_datatype` is not needed (we emit type *text*).

---

## 6. Expression rewrite rules & accepted divergences

`plx_rewrite_expr(char *src, ctx, boolean_context)` → `char *`, a **single left-to-right pass over source tokens** honoring string/`#{}`/bracket state. It **never re-scans its own output** (the `||`/`OR` invariant). Inside a **single-quoted** string, NOTHING is rewritten — no `#{}` splice, no operator/keyword rewrite, no `+`→`||`; only `'`→`''` for re-emission. `#{}` is an interpolation **only** in double-quoted strings/heredocs.

### 6.1 String literals — decode then re-encode (never memcpy source bytes)
Ruby string escape alphabet ≠ plpgsql literal alphabet. Always **fully decode** the Ruby token to its abstract character sequence, then **re-encode** for the chosen target literal:
- **Single-quoted** Ruby string: resolve `\\`→`\`, `\'`→`'`; all other bytes literal. Emit as plpgsql `'...'` with `'`→`''`. (`'a\\b'` = Ruby `a\b` → plpgsql `'a\b'`, one backslash.)
- **Double-quoted** Ruby string / bare-or-`"`-quoted heredoc: decode `\n \t \r \0 \e \\ \" \xHH \uXXXX …` to actual control bytes. If any control byte or backslash is present, emit an **escape string** `E'...'` (re-escaping `'`→`''` and `\`→`\\`); else a plain `'...'`. `standard_conforming_strings=on` means `'...\n...'` is a literal backslash-n — so `"line1\nline2"` **must** become `E'line1\nline2'`, not `'line1\nline2'`. `"C:\\tmp"` (Ruby value `C:\tmp`) → `E'C:\\tmp'`.

### 6.2 Interpolation `"lit#{e}lit2"`
Split into literal chunks + interp spans first. Each literal chunk is decoded/escaped per §6.1. Each interp expression is **recursively `plx_rewrite_expr`'d** (nested `"..."` inside `#{}` must themselves be lowered — never spliced verbatim) and wrapped to match Ruby nil→`''`:
```
'lit' || COALESCE((e')::text, '') || 'lit2'
```
A lone `#{e}` → `COALESCE((e')::text,'')`. **Empty/whitespace `#{}` → dropped** (never `()::text`). `\#{` → literal `#{` in the chunk. `'`→`''` and `%` are applied to literal chunks only, never to interp expressions. (Equivalently the whole interpolation may lower to `concat('lit', e', 'lit2')`, which ignores NULLs; pick one and be consistent.) **Documented divergence:** interpolation follows Ruby (nil→`''`), diverging from raw SQL `||` NULL-propagation; the RAISE `%`-path renders NULL as `<NULL>` unless COALESCE'd — reconcile or document.

### 6.3 Operators — boolean vs value context
Context (condition slot vs value/RHS slot) is known to the emitter; the split is decidable.

- `==` → **`IS NOT DISTINCT FROM`** (null-safe, matches Ruby `nil==nil`→true, `5==nil`→false). `!=` → **`IS DISTINCT FROM`**. **Literal-nil special case first** (token-level, before the generic rewrite): `E == nil` / `nil == E` → `((E') IS NULL)`; `E != nil` / `nil != E` → `((E') IS NOT NULL)`. (A perf-minded variant may keep `=`/`<>` when neither operand can be NULL, but null-safe is the default.)
- `&&`/`||`/`!` and keyword `and`/`or`/`not`:
  - **Boolean context** (if/unless/while/until tests, ternary test): `&&`/`and`→` AND `, `||`/`or`→` OR `, `!`/`not`→ negation. **Negation uses `(x) IS NOT TRUE`, not `NOT (x)`** — TRUE for both FALSE and NULL, matching Ruby falsey (`unless nil` runs). So `unless C`→`IF (C') IS NOT TRUE`, `next unless C`→`CONTINUE WHEN (C') IS NOT TRUE`, `!arr.include?(x)`→`(x = ANY(arr)) IS NOT TRUE`.
  - **Value/RHS context**: `a || b` → `COALESCE(a', b')` (Ruby nil-default; document that SQL can't distinguish stored FALSE from NULL, so `false || b` won't pick b). `a && b` value use → reject with a precise message pointing at an explicit ternary, or (boolean operands) `CASE WHEN (a') IS NOT TRUE THEN a' ELSE b' END`. Keyword `and`/`or` in value position → **reject** (their below-`=` precedence and `x or return` control-flow can't be faithfully lowered). This is why `||=`/`&&=` are rejected and the suggested `x = x || y` lowers to `COALESCE`.
- `nil`→`NULL`, `true`/`false`→`TRUE`/`FALSE`.
- Ternary `C ? A : B` → `CASE WHEN C' THEN A' ELSE B' END` (recursive).
- Ranges/membership (self-parenthesized): `(a..b).include?(x)`→`(x BETWEEN a' AND b')`; **`(a...b).include?(x)`→`(x >= a' AND x < b')`** (parenthesized — unparenthesized re-associates against outer operators); `arr.include?(x)`→`(x = ANY(arr'))`.
- `**`→`power(a,b)`; `1_000`→`1000`; casts `x.to_i/.to_s/.to_f`→`(x)::integer/::text/::double precision`.
- **`+` stays SQL numeric `+`** — the literal-adjacency `+`→`||` heuristic is **dropped** (it misfires both ways: `first + last` two text vars stay `+` and error at runtime; `amount + "0"` numeric becomes string concat). Interpolation is the sanctioned concatenation path.
- **`<<` is NOT blanket-rewritten** to `||` — PG `<<` is integer bit-shift, and the shovel's in-place mutation has no bare-statement plpgsql form. Leave `<<` verbatim (bit-shift) or reject; string/array building goes through interpolation or explicit `x = x || y`.
- `%` modulo, `::` casts, function calls, column refs, arithmetic → pass through verbatim to SQL.

### 6.4 Truthiness — no coercion, reject non-boolean conditions
Ruby treats `0`, `""`, `[]` as truthy — unrepresentable by any single SQL coercion. A bare non-boolean condition (`if rows_affected`, `return unless result`) is **rejected at CREATE**: `plruby: condition must be a boolean expression; Ruby truthiness is not emulated (0, "", [] are truthy in Ruby) — write an explicit comparison such as x != 0 or !x.nil?`. Do not pass `IF <int>`/`NOT (<text>)` to plpgsql.

### 6.5 Accepted, documented divergences
- Decimal literals → `numeric`, not `float8`.
- `for`/`.each` loop var is loop-local (Ruby `for` leak dropped; out-of-loop reference rejected).
- `||`/`&&` value context is COALESCE/CASE, not Ruby's exact false-vs-nil truthiness (SQL can't see stored FALSE as falsey).
- Condition NULL semantics resolved via `IS NOT TRUE` on negated forms and null-safe `==`/`!=`; positive `if`/`while` still treat NULL as false (plpgsql).
- `.nil?` on a fetch target means row-presence (`NOT FOUND`); on a scalar means `IS NULL`.

---

## 7. Error handling & precise `ereport`

Two error channels:
1. **Lexer/block errors** flow through `PlxError{line,col,msg}` from `plx_lex`/`plx_build_blocks`; the caller raises:
   ```c
   ereport(ERROR,
     (errcode(ERRCODE_SYNTAX_ERROR),
      errmsg("plruby: %s", err.msg),
      errdetail_internal("at line %d", err.line)));
   ```
2. **Out-of-subset / lowering rejects** raised in `plx_lower_*` with `ERRCODE_FEATURE_NOT_SUPPORTED` (or `ERRCODE_SYNTAX_ERROR` for malformed input), each carrying the offending `ruby_line`.

Hard-reject catalog (all at CREATE time, precise messages, real line numbers): `def`/nested defs; hash `{}` literal; `case/when`; `%w`/`%i` and unrecognized `%`-forms; `begin…end while`; objects/classes/gems; `redo`/`retry`/labeled break/`break <expr>`; `||=`/`&&=`; keyword `and`/`or` in value position; `&&` value use (unless CASE-lowerable); non-boolean condition; endless/beginless range; non-integer FOR endpoint/step; fractional `.step`; int4-overflow literal bound; for-var referenced after loop; loop-var vs OUT-param collision; case-fold/truncation identifier collision; unresolvable local type; conflicting annotations; scalar-vs-record type conflict; whole-row fetch into scalar; unmapped exception class; `message:` + positional message both present; bare `raise` outside handler; `next`/`break` outside loop; identifier-position static interpolation; assignment to `row.field`; reserved `__plx_` prefix in user code; unterminated/stray block.

**Source map (ARCHITECTURE §7):** `plx_emit` records `(plpgsql_line → ruby_line)` per node plus the constant DECLARE-preamble offset, so a *runtime* plpgsql error (an expression the scanner deliberately didn't validate) maps back to the user's real Ruby line.

---

## 8. C file / function layout & validator integration

### 8.1 Files
- `plx_scan.h` / `plx_scan.c` — shared framework: enums, `PlxToken`/`PlxLexer`/`PlxBlock`/`PlxStmt`, `plx_lex`, `plx_build_blocks`. Dialect-neutral; consumes a `PlxSurface`.
- `plx_transpile.h` / `plx_transpile.c` — shared framework: `PlxNode`/`PlxArm`/`PlxLocal`/`PlxSymtab`, `plx_rewrite_expr`, `plx_infer_type`, the `plx_lower_*` builders, `plx_emit`, `plx_emit_declare`, and the public entry point `plx_transpile`.
- `plx_dialect_ruby.c` — the Ruby `PlxSurface` (keyword table, block-form matchers, intrinsic names, sigils `#{`/`#::`, exception-class map) and the thin `ruby_validator` wired into `CREATE FUNCTION`.

### 8.2 Public API
```c
typedef struct PlxSurface {
  const PlxKwEntry *kwtab; int nkw;            /* sorted {spelling, PlxKw} */
  const char *interp_open;                     /* "#{" */ const char interp_close; /* '}' */
  const char *typeann_sigil;                   /* "#::" */
  const char *comment_lead;                    /* "#" */
  const PlxIntrinsic *intrinsics; int nintr;   /* query/fetch_one/perform/... */
  const PlxExcMap *exc_map; int nexc;          /* PG::UniqueViolation -> unique_violation */
  /* glyph flavors: single/double quote rules, heredoc enable, %-literal set */
} PlxSurface;

typedef struct PlxFuncMeta {
  Oid rettype; bool retset; char prokind;
  int nargs; char **argnames; Oid *argtypes; char *argmodes;
} PlxFuncMeta;

/* one call does the whole job; returns palloc'd plpgsql text (assembled) */
char *plx_transpile(const char *ruby_body, const PlxFuncMeta *meta,
                    const PlxSurface *surf, MemoryContext scratch);
```

`plx_transpile` internally: build `PlxParseCtx` → `plx_lex` → `plx_build_blocks` → recursive lower (rewrite + symtab + subset check) → resolve/validate types → assemble `sentinel + DECLARE + BEGIN + body + END + embedded-orig`. Everything palloc'd in `scratch`, reset after.

### 8.3 Validator integration (rewrite `pg_proc.prosrc`)
`ruby_validator(PG_FUNCTION_ARGS)` (registered as the language validator; call handler stays `plpgsql_call_handler`):
```c
Oid funcoid = PG_GETARG_OID(0);
HeapTuple tup = SearchSysCacheCopy1(PROCOID, ObjectIdGetDatum(funcoid));
Form_pg_proc pf = (Form_pg_proc) GETSTRUCT(tup);
/* idempotency: skip if prosrc already begins with the /*plx:v1:...*/ sentinel */
char *rubysrc = TextDatumGetCString(SysCacheGetAttr(PROCOID, tup, Anum_pg_proc_prosrc, &isnull));
if (!has_sentinel(rubysrc)) {
    PlxFuncMeta meta; build_meta(&meta, tup);          /* get_func_arg_info + prorettype/proretset/prokind */
    MemoryContext scratch = AllocSetContextCreate(CurrentMemoryContext, "plx", ...);
    char *plpgsql = plx_transpile(rubysrc, &meta, &PlxRubySurface, scratch);
    /* replace prosrc */
    Datum values[Natts_pg_proc]; bool nulls[Natts_pg_proc], repl[Natts_pg_proc];
    memset(repl, 0, sizeof repl);
    values[Anum_pg_proc_prosrc - 1] = CStringGetTextDatum(plpgsql);
    repl[Anum_pg_proc_prosrc - 1] = true; nulls[Anum_pg_proc_prosrc - 1] = false;
    Relation rel = table_open(ProcedureRelationId, RowExclusiveLock);
    HeapTuple newtup = heap_modify_tuple(tup, RelationGetDescr(rel), values, nulls, repl);
    CatalogTupleUpdate(rel, &newtup->t_self, newtup);
    table_close(rel, RowExclusiveLock);
    CommandCounterIncrement();
    MemoryContextDelete(scratch);
}
/* then let plpgsql's own validator compile-check the rewritten body (optional but recommended) */
```
Because `lanplcallfoid` is `plpgsql_call_handler`, the stored (now-plpgsql) `prosrc` executes on stock plpgsql. Re-running `CREATE OR REPLACE` re-supplies Ruby; the sentinel check makes a second validator pass idempotent (never double-transpile already-plpgsql text).

### 8.4 Embedded original (Strategy A) — safe encoding
The verbatim-splice of user bytes into a `/* … */` comment is unsafe: any `*/` in the body (in a string, `a */ b`, etc.) closes the comment early and corrupts `prosrc`; the `$plx-orig$` sentinel can likewise be forged. **Base64-encode the whole original body** and wrap:
```
/*plx-orig:b64$<base64>$plx-orig*/
```
Base64's alphabet contains no `*`, `/`, or `$`, so the terminator and sentinel are unforgeable. (Alternative: escape every `*/`→`*\/` and sentinel occurrences on write, reverse on read — base64 is simpler and total.) A recovery tool decodes this to show the user their original Ruby.

### 8.5 Ruby `PlxSurface` (`plx_dialect_ruby.c`)
Supplies keyword spellings (`if elsif unless while until for begin rescue ensure end next break return loop do case when then in and or not`), block-form matchers (`.each`, `.each_with_index`, `do |..|`, `loop do`, `{ |..| }`), intrinsics (`query fetch_one fetch_one! perform execute emit return_next return_query raise`), sigils `#{ }` and `#::`, and the exception-class→condition map. Nothing else is Ruby-specific.

---

## 9. Second dialect (PHP) reuse

**Shared verbatim (`plx_scan.c` + `plx_transpile.c`):** the two-pass scanner framework, block builder, statement IR, all `plx_lower_*` builders, `plx_rewrite_expr` (the operator/interpolation/range/ternary machinery), DECLARE-hoisting + type inference, `plx_emit`, source map, embedded-original, and the validator integration.

**Dialect-specific (a new `plx_dialect_php.c` `PlxSurface`):**
- keyword spellings (`if/elseif/else/endif` or brace-blocks, `while`, `foreach`, `function` reject, …);
- comment lexer + **type-annotation sigil** (PHP spelling `/*:: text */` instead of `#::`) — the shared framework consumes "a `::`-tagged type string in a trailing dialect comment"; the dialect supplies only its comment lexer;
- string/quote flavors (`"$var"` interpolation vs `'...'` literal), heredoc/nowdoc (`<<<EOT`/`<<<'EOT'`) mapped onto the same pending-heredoc queue;
- intrinsic names and the exception-class→condition map.

A PHP function reuses every `plx_*` function and swaps only `PlxSurface`. Divergence policy (nil-interp, null-safe `==`, non-boolean-condition reject) lives in the shared layer and applies uniformly.

---

## 10. M3 implementation order, checkpoints & corpus gates

### Build order
1. **`plx_scan.c` pass 1 (lexer).** Token vector for adversarial inputs. **Checkpoint C1:** golden token-dump for: string-with-`end`, `1..n` vs `1.5`, `#::`, `\#{`, single-vs-double-quote interp gating, `%`-literal vs modulo, `<<IDENT` heredoc, leading-dot continuation, `50% off # ends soon`.
2. **`plx_scan.c` pass 2 (block builder).** **Checkpoint C2:** pretty-printed block tree for opener-vs-modifier, `.each do |row|`, `(1..n).each { |i| }`, nested/sibling loops, `begin/rescue/ensure`, unterminated-block error line.
3. **DECLARE-hoisting + type inference (`plx_transpile.c` symtab).** **Checkpoint C3:** DECLARE goldens incl. annotation, literal inference, RECORD hoist, param exclusion (with unnamed args → no crash), case-fold collision reject, unresolved-type reject, loop-var-vs-OUT reject.
4. **`plx_rewrite_expr`.** **Checkpoint C4:** single-pass origin invariant (source `||`→`OR` in bool context, generated concat `||` untouched); interpolation decode/COALESCE; nested interpolation recursion; null-safe `==`/`!=` + literal-nil; ternary→CASE; parenthesized ranges; `+`/`<<` left verbatim.
5. **`plx_lower_*` builders + `plx_emit`.** **Checkpoint C5:** every node kind's golden plpgsql (real plpgsql, diffed).
6. **Assembly + validator wiring + embedded-original base64 + idempotency.** **Checkpoint C6:** live `CREATE FUNCTION` → inspect `pg_proc.prosrc` → execute on stock plpgsql → assert results; re-run `CREATE OR REPLACE` (no double-transpile).
7. **Source map + error-line accuracy.** **Checkpoint C7:** a runtime plpgsql error maps to the correct Ruby line.

### Corpus categories that gate "done"
1. **Scanner adversarial:** strings containing `end`/keywords; `1..n`/`1...n`/floats; `#::` vs `#`; `\#{`; single vs double quote interp; heredocs (`<<~`/`<<-`/`<<`, quoted/unquoted); brace block vs hash literal; leading-dot chains; `#` inside strings.
2. **String/interp/quoting:** nil-interp→`''` (COALESCE); `\n`/`\t`→E-string; backslash decode/re-encode; `it's #{n} o'clock`; empty `#{}`; nested `"…#{…}…"`; single-quoted-string inertness; `*/` and sentinel in the original body (base64 round-trip).
3. **`||`/concat/`+`/`<<`:** source `||` in condition→`OR`; value `a || b`→`COALESCE`; interpolation concat never re-scanned; `+` stays numeric; `<<` verbatim/reject; `and`/`or` value-position reject.
4. **NULL / three-valued:** `x == nil`→`IS NULL`; `a == b` null-safe; `unless`/`until`/`!`→`IS NOT TRUE`; `.nil?` on fetch→`NOT FOUND`; non-boolean condition reject; negated membership.
5. **Ranges / off-by-one:** inclusive/exclusive; `downto`/`upto`; `.step` positive/negative-literal/non-literal/fractional; endless/beginless reject; float endpoint reject; int4-overflow reject; exclusive-membership parenthesization.
6. **DECLARE / scoping:** annotation vs inference; RECORD hoist (query FOR); each_with_index counter + collision; unnamed args (no segfault); sibling-vs-nested loop names; post-loop name reuse; scalar-vs-record conflict; case/truncation collision; synthesized `__plx_e_*` declared.
7. **RAISE:** placeholder-adjacent-`%`→concat form; `%`/`'` escaping literal-chunks-only; USING-option interpolation→concat; `message:`-vs-positional exclusivity; bare `raise`/re-raise handler-depth; unmapped exception class; literal-`%` no-interp message.
8. **Full functions (end-to-end on live PG 18.4):** scalar and SETOF; query iteration; fetch_one/`!`; perform/execute static+dynamic; begin/rescue/ensure with diagnostics; loops with next/break; multi-assign swap — each CREATEd, its `prosrc` inspected, and executed for correct results.

"Done" = every category's golden files match and the category-8 functions produce correct runtime results on stock plpgsql, with all §7 rejects firing at CREATE time with accurate Ruby line numbers.
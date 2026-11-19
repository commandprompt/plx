# plxgo: the Go dialect

plxgo lets you write PostgreSQL functions with Go syntax. At `CREATE FUNCTION`
time plx transpiles the body to plpgsql and stores the plpgsql in
`pg_proc.prosrc`. The function then runs on the standard plpgsql interpreter.

Go is a braced C-family language, but it differs from plpgsql enough that plxgo
is a restructuring front end with its own tokenizer (including Go's automatic
semicolon insertion) and parser. It handles Go's parenless `if`/`for` headers,
`:=` short declarations with type inference, `for ... range`, and the
no-fallthrough `switch`.

## Setup

```sql
CREATE EXTENSION plx;
```

## Function basics

The body is a sequence of Go statements. Semicolons are optional, exactly as in
Go (the lexer inserts them at line ends). Local variables are declared with
`var name type`, `var name = value`, or the short form `name := value`; every
declaration is hoisted into a plpgsql `DECLARE` block.

```sql
CREATE FUNCTION grade(score int) RETURNS text LANGUAGE plxgo AS $$
	if score >= 90 {
		return "A"
	} else if score >= 80 {
		return "B"
	}
	return "F"
$$;
```

### Function signatures use PostgreSQL types

The **body** is Go, but the function **signature** (the parameter and return
types in `CREATE FUNCTION`) is parsed by PostgreSQL before plx sees the body, so
it uses PostgreSQL type names: write `RETURNS bigint`, and refer to the
parameter by name in the body (`score` above). Inside the body, Go type names in
declarations are translated (see below).

## Go to PostgreSQL translations

### Declarations and assignment

| Go | plpgsql |
| --- | --- |
| `var x int` | hoisted to `DECLARE x integer;`, plus `x := 0;` (Go's zero value) |
| `var x int = 5` | `x integer;` plus `x := 5;` |
| `var x = 5` / `x := 5` | type inferred (`integer`); `x := 5;` |
| `var ( a int; b string )` | both hoisted |
| `const pi = 3.14` | `pi CONSTANT double precision := 3.14;` |
| `x = e` | `x := e;` |
| `x += e` (also `-= *= /= %= <<= >>= &= \|= ^= &^=`) | `x := x + (e);` (`^=` uses `#`, `&^=` uses `& ~`) |
| `x++` / `x--` | `x := x + 1;` / `x := x - 1;` |
| `a, b = x, y` | `SELECT x, y INTO a, b;` (parallel, so swaps work) |
| `a, b := 1, "x"` | declares `a` (integer) and `b` (text), then assigns; each value must have an inferable type |

A `var` with no value starts at Go's zero value, as in Go: `0` for the
integer and float types, `""` for `string`, and `false` for `bool`. plx assigns
the zero value where the declaration stands, so a `var` inside a loop body is
reset on every iteration. A slice declared without a value is `NULL`, which
matches Go's nil slice (`len` of it is 0). Any other type, such as `time.Time`
or a PostgreSQL type name, starts as `NULL`.

`:=` infers the type from a literal (`int`/`float`/`string`/`bool`), an array
literal, `len(...)`, or a type conversion `T(x)`. If the type cannot be inferred
(for example `x := someCall()`), declare it explicitly with `var x T`.

### Control flow

| Go | plpgsql |
| --- | --- |
| `if cond { ... }` | `IF cond THEN ... END IF;` |
| `if init; cond { ... }` | the init statement, then the `IF` |
| `if ... { } else if ... { } else { }` | `IF ... ELSE IF ... ELSE ... END IF;` |
| `for { ... }` | `LOOP ... END LOOP;` |
| `for cond { ... }` | `WHILE cond LOOP ... END LOOP;` |
| `for init; cond; post { ... }` | `init; WHILE cond LOOP ... post; END LOOP;` (see below) |
| `for i := range n { ... }` | `FOR i IN 0 .. (n) - 1 LOOP ...` (integer range) |
| `for _, v := range slice { ... }` | `FOREACH v IN ARRAY slice LOOP ...` |
| `for _, row := range query("...") { ... }` | `FOR row IN EXECUTE '...' LOOP ...` |
| `break` / `continue` | `EXIT;` / `CONTINUE;` (in a three-clause `for`, `continue` leaves the body block; in a `switch`, `break` leaves the switch; see below) |
| `return e` | `RETURN e;` |

A three-clause `for` keeps Go's meaning. plx tests the condition before every
pass and runs the post statement after the body, so the body may change the
loop variable or the bound (`for i := 0; i < len(s); i++` sees elements that the
body appends). `continue` jumps to the post statement, not past it: plx wraps
the body in a labelled block and lowers `continue` to `EXIT` of that block.

When the init statement declares the loop variable (`for i := A; ...`), the
variable belongs to the loop, as in Go: plx declares it in a block around the
loop, so it does not change a variable of the same name outside the loop. `A`
is the variable's default value in that block, so `A` may read an outer
variable of the same name (`for i := i; ...` starts at the outer `i`). Its
type comes from `A`: a literal or one of the forms `:=` infers (below), else
the type of the first variable in `A` (`for i := n - 1; ...` gives `i` the
type of `n`). An element of a slice declared in the body gives the slice's
element type (`for i := s[0]; ...`). If the first name in `A` is a function
call, a row field (`r.n`) or an element of a slice parameter, plx cannot tell the
type and rejects the function; declare the variable before the loop
(`var i int`) and write `for i = A; ...`. With `for i = A; ...` the loop updates that variable, so it
holds its final value after the loop.

In a `range` loop over an integer, the single variable is the index. Over a
slice, use `for _, v := range s`. The value variable's type is inferred from the
slice's declared element type; when `s` is a parameter, declare `v` with `var`
before the loop. `for i := range s` and `for i, v := range s` over a slice are
not supported; walk a slice by index with a counting loop
(`for i := 0; i < len(s); i++`).

### switch

A tagged `switch x { ... }` becomes a plpgsql `CASE x WHEN ... END CASE`, and a
tagless `switch { ... }` (boolean cases) becomes an `IF`/`ELSIF`/`ELSE` chain.
Go has no implicit fall-through, and neither form falls through. As in Go, the
tag is evaluated once, and each case value is compared with it as a whole, so
`case a || b:` matches when the tag equals `a || b`. Comma-separated case values
match if any one matches. `default` runs only when no case matches, wherever it
stands in the switch: plx always emits it as the final `ELSE`.

`break` inside a switch leaves the switch, not a loop around it. plx puts such
a switch in a labelled block and lowers `break` to `EXIT` of that block. A
`break` in a loop nested inside the switch still leaves only that loop.

```sql
CREATE FUNCTION classify(n int) RETURNS text LANGUAGE plxgo AS $$
	switch {
	case n > 0:
		return "positive"
	case n < 0:
		return "negative"
	default:
		return "zero"
	}
$$;
```

### Operators, literals, and builtins

| Go | plpgsql |
| --- | --- |
| `==`, `!=` | `=`, `<>` |
| `&&`, `\|\|`, `!x` | `AND`, `OR`, `NOT x` |
| `a ^ b`, `^x`, `a &^ b` | `a # b` (XOR), `(~x)`, `a & ~(b)` |
| `<<`, `>>`, `&`, `\|` | the same operators, bracketed to keep Go's precedence |
| `nil` | `NULL` |
| `"..."`, `` `...` ``, `'x'` | SQL string literal (escapes decoded, see below) |
| `0x1F`, `0o17`, `017`, `0b101`, `1_000` | the decimal value (`31`, `15`, `15`, `5`, `1000`) |
| `[]int{1, 2, 3}` | `ARRAY[1, 2, 3]` |
| `len(x)` | `length(x)` (text) or `coalesce(cardinality(x), 0)` (a slice declared in the body) |
| `append(s, x)` | `array_append(s, x)` |
| `a[i]` (subscript) | `a[(i) + 1]` (see below) |
| `int(x)`, `int64(x)`, `float64(x)`, `string(x)` | `(x)::integer` or `trunc(x)::integer` / the same for `bigint` / `(x)::double precision` / `(x)::text` (see below) |
| `panic(m)` | `RAISE EXCEPTION '%', m;` |
| `fmt.Println(x)` / `fmt.Printf(f, x)` | `RAISE NOTICE '%', x;` |

plx decodes every escape Go defines in an interpreted string or rune literal:
`\a \b \f \n \r \t \v \\ \' \"`, `\xHH` and `\NNN` (one byte each), and
`\uHHHH` and `\UHHHHHHHH` (a Unicode character, stored in the database
encoding). As in Go, any other escape is an error. A literal that would contain
a NUL character, or whose `\x` or octal bytes do not form valid text in the
database encoding, is rejected, because PostgreSQL text cannot hold it.

Integer literals in hexadecimal, octal and binary, and literals with `_`
separators, become plain decimal numbers, so they mean the same on every
supported PostgreSQL version. A number with letters Go does not allow (such as
`12abc`), a hexadecimal float and an imaginary literal are rejected.

A conversion to an integer type truncates toward zero, as in Go: `int(3.7)` is
3 and `int(-2.7)` is -2. A SQL cast would round instead. plx casts an integer
literal, or a local or parameter of an integer type, directly. For a float or
`numeric` literal, local or parameter, it calls `trunc` first. For any other
operand, such as an expression or a row field, the type is known only when the
function runs. plx evaluates the operand once and then chooses by its type: an
integer casts exactly, a float goes through `trunc`, and any other value, such
as `numeric` or `text`, goes through `trunc` as `numeric`. So `int64(n + 1)`
and `int64(r.id)` stay exact beyond 2^53.

SQL ranks `<<`, `>>`, `&` and `|` below `+` and `-`, while Go ranks `<<`, `>>`,
`&` and `&^` with `*`, and `|` and `^` with `+`. When an expression uses a
bitwise operator, plx brackets it by Go's rules, so `1 << 2 + 1` is
`(1 << 2) + 1`, which is 5, as in Go. A unary `^x` binds tighter than any
binary operator in Go, but SQL's prefix `~` binds looser than `+` and `*`, so
plx brackets it with its operand: `^y + 1` is `(~y) + 1`, which is `-y`.

### Slices and indexing

A slice literal `[]T{...}` becomes a PostgreSQL `ARRAY[...]`. Go slices are
0-based while PostgreSQL arrays are 1-based, so a subscript `a[i]` is rewritten
to `a[(i) + 1]`. Indexing therefore follows Go semantics: `a[0]` is the first
element, and `len(a)` and `a[i]` agree on 0-based positions, so
`for i := 0; i < len(a); i++` walks a slice by index. `len` and `range`
recognize only slices declared in the body; copy an array parameter into one
first (`var a []int = p`). Go slice expressions (`a[i:j]`) are not translated.

`fmt.Println`/`fmt.Printf` used as a statement raise a `NOTICE` with one `%`
placeholder per value argument (space-separated); there the format string's
literal text and directives are not reproduced, since SQL `RAISE` has no printf
verbs. A format string with an explicit argument index (`%[1]d`) is rejected
here too, because the values would print in the wrong order.

`fmt.Sprintf` in an expression becomes a SQL concatenation, with the format
string's literal text reproduced between the operands:

```go
fmt.Sprintf("user %s has %d items", nm, n)
```
```sql
'user ' || (nm)::text || ' has ' || (n)::text || ' items'
```

Concatenating rather than calling `format()` is what lets a NULL operand
propagate, so the whole string is NULL instead of the operand quietly rendering
as empty. A message built for `panic` is the exception and keeps each operand's
empty-string fallback, so one NULL cannot swallow the message.

Every verb renders its operand as text. A `-` flag and a width are kept and
become `rpad`/`lpad`, so `%-8d` still pads to eight columns. As in Go, the width
is a minimum: an operand longer than the width prints in full. An explicit
argument index such as `%[1]d` is not supported, and plx rejects the function. Go's other flags
and its precision field are dropped, so `%.2f` prints the operand's full text
rather than rounding it to two decimal places. Verbs that change an operand's
representation rather than its padding are affected the same way: `%x`, `%o`,
`%b`, `%e` and `%q` all render the operand's text, so `fmt.Sprintf("%x", 255)`
yields `255` and not `ff`.
A doubled `%%` stays a literal `%`. A `%` that starts no directive is also
rendered as a literal percent, but it makes plx fall back to `format()`, where
a NULL operand renders as empty instead of propagating; write `%%` instead.

### Types (in declarations)

| Go | PostgreSQL |
| --- | --- |
| `int`, `int32`, `rune` | `integer` |
| `int64`, `uint`, `uint32`, `uint64` | `bigint` |
| `int8`, `int16`, `uint8`, `byte` | `smallint` |
| `float32`, `float64` | `real`, `double precision` |
| `string` | `text` |
| `bool` | `boolean` |
| `[]T` / `[N]T` | `T[]` (a PostgreSQL array) |
| `time.Time` | `timestamp` |
| `*T` (pointer) | `T` (the pointer is dropped) |

An unrecognized type name is passed through, so PostgreSQL type names also work.

### Standard library

A subset of the standard library is mapped: `strings.ToUpper`/`ToLower`/
`TrimSpace`/`ReplaceAll`/`Contains`, `math.Abs`/`Floor`/`Ceil`/`Sqrt`/`Pow`/
`Max`/`Min`/`Mod`, `strconv.Itoa` (integer to text) and `strconv.Atoi` (text
to integer), and `time.Now`. Anything not mapped is
emitted as written, so a PostgreSQL function of the same name still works.

## SQL access

plxgo provides small SQL intrinsics, mirroring the other dialects:

| Go | plpgsql |
| --- | --- |
| `emit(x)` | `RETURN NEXT x;` (for a set-returning function) |
| `execute("...")` | `EXECUTE '...';` |
| `perform(expr)` | `PERFORM expr;` (write the call as Go, for example `perform(nextval("seq"))`; a quoted string is evaluated as a literal and does nothing) |
| `for _, row := range query("...") { ... }` | `FOR row IN EXECUTE '...' LOOP ...` |

```sql
CREATE FUNCTION squares(n int) RETURNS SETOF int LANGUAGE plxgo AS $$
	for i := range n {
		emit(i * i)
	}
$$;
```

## Not supported

- Goroutines and channels (`go`, `chan`, `select`, `<-`), `defer`, and `goto`.
- Nested function definitions and closures (`func`).
- `map`, `chan`, `struct`, and `interface` types (declare data with SQL types or
  arrays instead).
- `switch` `fallthrough`.
- Transaction control. `commit()` and `rollback()` are accepted when a procedure
  is created but fail when it runs (`function commit() does not exist`).
- String concatenation with `+`. Go overloads `+` for numeric addition and
  string concatenation; plx cannot tell which is meant without type information,
  so it leaves `+` as `+`. `||` is logical OR in plxgo, so use
  `fmt.Sprintf("%s%s", a, b)` for text concatenation (a NULL operand makes the
  result NULL).
- The full `fmt`/`strings`/`math`/`strconv` packages; only the subset above is
  translated.

See [PARITY.md](PARITY.md) for the per-dialect feature matrix and
[USERGUIDE.md](USERGUIDE.md) for cross-dialect examples.

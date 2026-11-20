-- Regression tests for bugs found by the 2026-11-19 audit (fix_go).
-- Each case names its audit finding (A<n>).
CREATE EXTENSION IF NOT EXISTS plx;
SET client_min_messages = warning;

-- A26: var x T with no initializer starts at Go's zero value, not NULL
CREATE FUNCTION fg26_sum(a int[]) RETURNS int LANGUAGE plxgo AS $$
 var s []int = a
 var total int
 for _, v := range s {
   total += v
 }
 return total
$$;
SELECT fg26_sum(ARRAY[1,2,3]) AS go_6;
CREATE FUNCTION fg26_flag() RETURNS text LANGUAGE plxgo AS $$
 var done bool
 if !done {
   return "not done"
 }
 return "done"
$$;
SELECT fg26_flag() AS go_not_done;
CREATE FUNCTION fg26_kinds() RETURNS text LANGUAGE plxgo AS $$
 var s string
 var f float64
 var (
   n int64
   b bool
 )
 s = fmt.Sprintf("%sx", s)
 return fmt.Sprintf("%s|%v|%v|%v", s, f, n, b)
$$;
SELECT fg26_kinds() AS go_x_0_0_false;
CREATE FUNCTION fg26_loop() RETURNS int LANGUAGE plxgo AS $$
 last := 0
 for i := 0; i < 3; i++ {
   var c int
   c += 1
   last = c
 }
 return last
$$;
SELECT fg26_loop() AS go_1;
-- a fixed-size array [N]T starts as N zero values; a slice stays nil
CREATE FUNCTION fg26_array() RETURNS text LANGUAGE plxgo AS $$
 var e [3]int
 var t [2]string
 var b [2]bool
 var s []int
 return fmt.Sprintf("%d|%d|%d|%s|%v|%d|%d", e[2], len(e), len(t), t[1], b[0], len(b), len(s))
$$;
SELECT fg26_array() AS go_0_3_2_empty_false_2_0;

-- A27: a counting for loop re-tests its bound, honours body writes to the
-- loop variable, updates a variable declared before the loop, and keeps a
-- float loop variable a float
CREATE FUNCTION fg27_append() RETURNS int LANGUAGE plxgo AS $$
 var s []int = []int{1}
 for i := 0; i < len(s); i++ {
   if i < 3 { s = append(s, i) }
 }
 return len(s)
$$;
SELECT fg27_append() AS go_4;
CREATE FUNCTION fg27_bodywrite() RETURNS int LANGUAGE plxgo AS $$
 var c int = 0
 for i := 0; i < 10; i++ {
   if i == 2 { i = 7 }
   c++
 }
 return c
$$;
SELECT fg27_bodywrite() AS go_5;
CREATE FUNCTION fg27_outer() RETURNS int LANGUAGE plxgo AS $$
 var i int
 for i = 0; i < 10; i++ {
 }
 return i
$$;
SELECT fg27_outer() AS go_10;
CREATE FUNCTION fg27_float() RETURNS int LANGUAGE plxgo AS $$
 c := 0
 for f := 0.5; f < 3; f++ {
   c++
 }
 return c
$$;
SELECT fg27_float() AS go_3;
-- continue still runs the post statement, in a counting loop and in any
-- other three-clause loop; a nested loop's continue stays its own
CREATE FUNCTION fg27_continue(n int) RETURNS int LANGUAGE plxgo AS $$
 sum := 0
 for i := n - 1; i >= 0; i-- {
   if i % 2 == 0 { continue }
   sum += i
 }
 for j := 1; j < 100; j = j * 2 {
   if j == 4 { continue }
   for k := 0; k < 3; k++ {
     if k == 1 { continue }
     sum += 1000
   }
 }
 return sum
$$;
SET statement_timeout = '10s';
SELECT fg27_continue(6) AS go_12009;
RESET statement_timeout;
-- the loop variable is scoped to the loop, as in Go
CREATE FUNCTION fg27_scope() RETURNS text LANGUAGE plxgo AS $$
 i := "outer"
 n := 0
 for i := 0; i < 3; i++ {
   n += i
 }
 return fmt.Sprintf("%s %d", i, n)
$$;
SELECT fg27_scope() AS go_outer_3;
-- the start value may read an outer variable of the same name
CREATE FUNCTION fg27_shadow() RETURNS text LANGUAGE plxgo AS $$
 i := 2
 c := 0
 for i := i; i < 5; i++ {
   c++
 }
 for i := i * 2; i < 6; i++ {
   c += 10
 }
 return fmt.Sprintf("%d %d", c, i)
$$;
SELECT fg27_shadow() AS go_23_2;
-- the start value may be an element of a slice declared in the body; a row
-- field has no known type, so it is rejected with a clear message
CREATE FUNCTION fg27_elem(a int[]) RETURNS int LANGUAGE plxgo AS $$
 var s []int = a
 c := 0
 for i := s[0] + 1; i < 5; i++ {
   c++
 }
 return c
$$;
SELECT fg27_elem(ARRAY[1]) AS go_3;
CREATE FUNCTION fg27_field() RETURNS int LANGUAGE plxgo AS $$
 c := 0
 for _, r := range query("SELECT 1 AS n") {
   for i := r.n; i < 5; i++ {
     c++
   }
 }
 return c
$$;

-- A28: switch runs default only when no case matches, wherever default
-- stands, and break inside a switch leaves only the switch
CREATE FUNCTION fg28_first(x int) RETURNS text LANGUAGE plxgo AS $$
 switch x {
 default:
   return "d"
 case 1:
   return "one"
 }
$$;
SELECT fg28_first(1) AS go_one, fg28_first(5) AS go_d;
CREATE FUNCTION fg28_middle(x int) RETURNS text LANGUAGE plxgo AS $$
 switch x {
 case 1:
   return "one"
 default:
   return "d"
 case 2:
   return "two"
 }
$$;
SELECT fg28_middle(2) AS go_two, fg28_middle(1) AS go_one, fg28_middle(9) AS go_d;
CREATE FUNCTION fg28_break() RETURNS int LANGUAGE plxgo AS $$
 var n int = 0
 for i := 0; i < 5; i++ {
   switch i {
   case 2:
     break
   }
   n = n + 1
 }
 return n
$$;
SELECT fg28_break() AS go_5;
CREATE FUNCTION fg28_noloop(x int) RETURNS text LANGUAGE plxgo AS $$
 r := "start"
 switch {
 case x > 0:
   r = "pos"
   if x > 10 { break }
   r = "small pos"
 }
 return r
$$;
SELECT fg28_noloop(5) AS go_small_pos, fg28_noloop(50) AS go_pos;
-- a break in a loop inside a switch still leaves only that loop
CREATE FUNCTION fg28_inner() RETURNS int LANGUAGE plxgo AS $$
 n := 0
 switch {
 case true:
   for {
     n++
     if n == 3 { break }
   }
   n += 10
 }
 return n
$$;
SELECT fg28_inner() AS go_13;
-- a second default is rejected, as Go rejects it
CREATE FUNCTION fg28_twodefaults(x int) RETURNS text LANGUAGE plxgo AS $$
 switch x {
 default:
   return "a"
 default:
   return "b"
 }
$$;
-- a tagless switch also runs default last, wherever it stands
CREATE FUNCTION fg28_tagless(x int) RETURNS text LANGUAGE plxgo AS $$
 switch {
 default:
   return "d"
 case x == 1:
   return "one"
 }
$$;
SELECT fg28_tagless(1) AS go_one, fg28_tagless(5) AS go_d;
CREATE FUNCTION fg28_tagless_mid(x int) RETURNS text LANGUAGE plxgo AS $$
 switch {
 case x == 1:
   return "one"
 default:
   return "d"
 case x == 2:
   return "two"
 }
$$;
SELECT fg28_tagless_mid(2) AS go_two, fg28_tagless_mid(1) AS go_one, fg28_tagless_mid(9) AS go_d;

-- A29: a switch tag is evaluated once, and each case value is compared as a
-- whole
CREATE SEQUENCE fg29_seq;
CREATE FUNCTION fg29_tag() RETURNS text LANGUAGE plxgo AS $$
 switch nextval("fg29_seq") % 3 {
 case 5:
   return "five"
 case 6:
   return "six"
 case 7:
   return "seven"
 }
 return "none"
$$;
SELECT setval('fg29_seq', 1);
SELECT fg29_tag() AS go_none;
SELECT currval('fg29_seq') AS go_2;
CREATE FUNCTION fg29_or(flag bool, a bool, b bool) RETURNS text LANGUAGE plxgo AS $$
 switch flag {
 case a || b:
   return "match"
 }
 return "nomatch"
$$;
SELECT fg29_or(false, false, true) AS go_nomatch, fg29_or(true, false, true) AS go_match;
-- a tagged switch with no matching case and no default does nothing
CREATE FUNCTION fg29_nomatch(x int) RETURNS text LANGUAGE plxgo AS $$
 r := "none"
 switch x + 1 {
 case 1, 2:
   r = "small"
 }
 return r
$$;
SELECT fg29_nomatch(1) AS go_small, fg29_nomatch(7) AS go_none;

-- A30: Go's bitwise operators keep Go's meaning and precedence
CREATE FUNCTION fg30_xor() RETURNS int LANGUAGE plxgo AS $$ return 5 ^ 3 $$;
SELECT fg30_xor() AS go_6;
CREATE FUNCTION fg30_shl() RETURNS int LANGUAGE plxgo AS $$ return 1 << 2 + 1 $$;
SELECT fg30_shl() AS go_5;
CREATE FUNCTION fg30_ops(a int, b int) RETURNS text LANGUAGE plxgo AS $$
 return fmt.Sprintf("%d %d %d %d %d %d %d", a & b + 1, a | b + 1, ^a, a &^ b, a >> 1 * 2, (a + b) << 1, foo(a ^ b))
$$;
CREATE FUNCTION foo(x int) RETURNS int LANGUAGE sql AS $$ SELECT x $$;
SELECT fg30_ops(12, 10) AS go_9_15_m13_4_12_44_6;
-- one operator per function, so a wrong result in one cannot hide behind
-- an error in another
CREATE FUNCTION fg30_p1(a int, b int) RETURNS int LANGUAGE plxgo AS $$ return a & b + 1 $$;
SELECT fg30_p1(12, 3) AS go_1;
CREATE FUNCTION fg30_p2(a int, b int) RETURNS int LANGUAGE plxgo AS $$ return a | b + 1 $$;
SELECT fg30_p2(12, 3) AS go_16;
CREATE FUNCTION fg30_p3(a int) RETURNS int LANGUAGE plxgo AS $$ return a >> 1 * 2 $$;
SELECT fg30_p3(12) AS go_12;
CREATE FUNCTION fg30_p4(a int, b int) RETURNS int LANGUAGE plxgo AS $$ return a &^ b + 1 $$;
SELECT fg30_p4(12, 3) AS go_13;
CREATE FUNCTION fg30_assign() RETURNS text LANGUAGE plxgo AS $$
 a := 6
 a &= 3
 b := 1
 b <<= 4
 c := 5
 c ^= 1
 d := 8
 d >>= 2
 e := 4
 e |= 1
 f := 15
 f &^= 5
 return fmt.Sprintf("%d %d %d %d %d %d", a, b, c, d, e, f)
$$;
SELECT fg30_assign() AS go_2_16_4_2_5_10;
-- a bitwise test inside a condition and a slice literal
CREATE FUNCTION fg30_cond(n int) RETURNS int[] LANGUAGE plxgo AS $$
 if n & 1 == 1 && n | 2 > 2 {
   return []int{n ^ 1, n << 1}
 }
 return []int{0}
$$;
SELECT fg30_cond(3) AS go_2_6, fg30_cond(4) AS go_0;
-- unary ^ binds tighter than any binary operator, as in Go
CREATE FUNCTION fg30_unary(y int, a int[]) RETURNS text LANGUAGE plxgo AS $$
 var s []int = a
 var x int = ^y + 1
 return fmt.Sprintf("%d %d %d %d %d", ^5 + 1, ^5 * 2, x, ^s[1] * 2, ^(y + 1) * 2)
$$;
SELECT fg30_unary(5, ARRAY[1,2]) AS go_m5_m12_m5_m6_m14;
CREATE FUNCTION fg30_unary_field() RETURNS int LANGUAGE plxgo AS $$
 for _, r := range query("SELECT 5 AS n") {
   return ^r.n + 1
 }
 return 0
$$;
SELECT fg30_unary_field() AS go_m5;

-- A31: int(x) and int64(x) of a float truncate toward zero, as in Go
CREATE FUNCTION fg31_int(f float8) RETURNS int LANGUAGE plxgo AS $$ return int(f) $$;
SELECT fg31_int(3.7) AS go_3, fg31_int(-2.7) AS go_m2, fg31_int(2.5) AS go_2;
CREATE FUNCTION fg31_int64(n numeric) RETURNS bigint LANGUAGE plxgo AS $$
 var x float64 = 9.99
 return int64(n) + int64(x) + int64(7.9)
$$;
SELECT fg31_int64(-5.5) AS go_11;
-- an integer operand converts exactly, even past 2^53
CREATE FUNCTION fg31_big(n bigint) RETURNS bigint LANGUAGE plxgo AS $$
 var m int64 = n
 return int64(n) - int64(m) + int64(n)
$$;
SELECT fg31_big(9007199254740993) AS go_9007199254740993;
-- a row field or an expression converts exactly when it is an integer,
-- truncates when it is a float or numeric, and a text field still converts
CREATE FUNCTION fg31_field() RETURNS text LANGUAGE plxgo AS $$
 for _, r := range query("SELECT 1152921504606846977::bigint AS id, '42'::text AS t, 3.7::float8 AS f, (-2.7)::real AS g, 1152921504606846977.9::numeric AS n") {
   return fmt.Sprintf("%d %d %d %d %d", int64(r.id), int(r.t), int(r.f), int(r.g), int64(r.n))
 }
 return ""
$$;
SELECT fg31_field() AS go_exact_42_3_m2_exact;
CREATE FUNCTION fg31_expr(n bigint) RETURNS bigint LANGUAGE plxgo AS $$ return int64(n + 1) $$;
SELECT fg31_expr(1152921504606846976) AS go_1152921504606846977;

-- A32: a fmt.Sprintf width pads but never truncates, and an explicit
-- argument index (%[n]) is rejected rather than mangled
CREATE FUNCTION fg32_w() RETURNS text LANGUAGE plxgo AS $$ return fmt.Sprintf("[%3d]", 12345) $$;
SELECT fg32_w() AS go_12345;
CREATE FUNCTION fg32_wp() RETURNS text LANGUAGE plxgo AS $$ return fmt.Sprintf("[%5.1f]", 3.14159) $$;
SELECT fg32_wp() AS go_untruncated;
CREATE FUNCTION fg32_pad(s text) RETURNS text LANGUAGE plxgo AS $$
 return fmt.Sprintf("[%6s|%-6s|%2s]", s, strings.ToLower(s), upper(s))
$$;
SELECT fg32_pad('ab') AS go_padded, fg32_pad('abcdefgh') AS go_whole, fg32_pad(NULL) IS NULL AS go_null;
CREATE FUNCTION fg32_idx() RETURNS text LANGUAGE plxgo AS $$ return fmt.Sprintf("%[1]d", 5) $$;
-- fmt.Printf, and a Sprintf with too many arguments for the || lowering,
-- reject an explicit argument index too
CREATE FUNCTION fg32_pfidx() RETURNS void LANGUAGE plxgo AS $$ fmt.Printf("%[2]d %[1]d\n", 1, 2) $$;
CREATE FUNCTION fg32_manyidx() RETURNS text LANGUAGE plxgo AS $$
 return fmt.Sprintf("%[2]d", 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17)
$$;

-- A33: every Go string escape keeps its value, and octal, hexadecimal and
-- binary integer literals keep theirs
CREATE FUNCTION fg33_esc() RETURNS text LANGUAGE plxgo AS $$ return "\u00e9|\U0001F600|\x41|\101|\b|\a\f\v|\x27\047\u0027" $$;
SELECT fg33_esc() = E'\u00e9|\U0001F600|A|A|\b|\007\f\013|''''''' AS go_true,
       length(fg33_esc()) AS go_len_17;
CREATE FUNCTION fg33_rune() RETURNS text LANGUAGE plxgo AS $$ return string('\x41') $$;
SELECT fg33_rune() AS go_a;
-- in a rune literal, \x and octal escapes are code points, not bytes
CREATE FUNCTION fg33_rune2() RETURNS text LANGUAGE plxgo AS $$
 return fmt.Sprintf("%c|%c|%s", '\xe9', '\351', "\xc3\xa9")
$$;
SELECT fg33_rune2() = E'\u00e9|\u00e9|\u00e9' AS go_true;
CREATE FUNCTION fg33_oct() RETURNS int LANGUAGE plxgo AS $$ return 017 $$;
SELECT fg33_oct() AS go_15;
CREATE FUNCTION fg33_oct2() RETURNS int LANGUAGE plxgo AS $$ return 0o17 + 0O1 $$;
SELECT fg33_oct2() AS go_16;
CREATE FUNCTION fg33_bases() RETURNS bigint LANGUAGE plxgo AS $$
 x := 0XE
 return int64(x) + 0x1F + 0b101 + 1_000 + 0xFF_FF
$$;
SELECT fg33_bases() AS go_66585;
-- a hexadecimal literal with an E digit is an integer, so / divides as integers
CREATE FUNCTION fg33_hexe() RETURNS text LANGUAGE plxgo AS $$
 x := 0XE
 return x / 4
$$;
SELECT fg33_hexe() AS go_3;
-- a literal Go rejects is rejected, not silently changed
CREATE FUNCTION fg33_badesc() RETURNS text LANGUAGE plxgo AS $$ return "\q" $$;
CREATE FUNCTION fg33_nul() RETURNS text LANGUAGE plxgo AS $$ return "a\x00b" $$;
CREATE FUNCTION fg33_badnum() RETURNS int LANGUAGE plxgo AS $$ return 09 $$;
CREATE FUNCTION fg33_suffix() RETURNS int LANGUAGE plxgo AS $$ return 12abc $$;

-- A5 (go-loopvar-case): a Go name that differs from a parameter only by case
-- is the same name to plpgsql, so it is rejected, as in the other dialects.
-- Before the fix, fg34_loop(100) returned 6 (Go gives 300).
CREATE FUNCTION fg34_loop(i int) RETURNS int LANGUAGE plxgo AS $$ s := 0
for I := 1; I <= 3; I++ { s += i }
return s $$;
CREATE FUNCTION fg34_range(i int) RETURNS int LANGUAGE plxgo AS $$ s := 0
for I := range 3 { s += i }
return s $$;
CREATE FUNCTION fg34_short(i int) RETURNS int LANGUAGE plxgo AS $$ I := 5
return i + I $$;
CREATE FUNCTION fg34_multi(i int) RETURNS int LANGUAGE plxgo AS $$ I, j := 5, 6
return i + I + j $$;
CREATE FUNCTION fg34_var(i int) RETURNS int LANGUAGE plxgo AS $$ var I int = 5
return i + I $$;
CREATE FUNCTION fg34_const(i int) RETURNS int LANGUAGE plxgo AS $$ const I = 5
return i + I $$;
-- the parameter read before the declaration
CREATE FUNCTION fg34_before(i int) RETURNS int LANGUAGE plxgo AS $$ var x int = i
I := 5
return x + I $$;
-- a local read with a second spelling
CREATE FUNCTION fg34_ref() RETURNS int LANGUAGE plxgo AS $$ a := 1
return A + 1 $$;
-- one spelling still works: a loop variable with the parameter's own name,
-- and a local that is the only spelling of an unused parameter's name
CREATE FUNCTION fg34_same(i int) RETURNS int LANGUAGE plxgo AS $$ s := 0
for i := 1; i <= 3; i++ { s += i }
return s $$;
SELECT fg34_same(100) AS go_6;
CREATE FUNCTION fg34_one(i int) RETURNS int LANGUAGE plxgo AS $$ var I int
I = 5
return I $$;
SELECT fg34_one(100) AS go_5;
-- a field name after a dot is not a variable
CREATE FUNCTION fg34_field(n int) RETURNS int LANGUAGE plxgo AS $$
 for _, r := range query("SELECT 2 AS N") { return n + r.N }
 return n $$;
SELECT fg34_field(1) AS go_3;
-- a function name is not a variable, so the name of a call is not checked
-- against the locals and parameters.  Before the fix both bodies were
-- rejected.
CREATE FUNCTION fg34_total(n int) RETURNS int LANGUAGE sql AS $$ SELECT n * 10 $$;
CREATE FUNCTION fg34_call(n int) RETURNS int LANGUAGE plxgo AS $$ var fg34_total int = Fg34_Total(n)
return fg34_total + 1 $$;
SELECT fg34_call(2) AS go_21;
CREATE FUNCTION fg34_helper(n int) RETURNS int LANGUAGE sql AS $$ SELECT n + 1 $$;
CREATE FUNCTION fg34_callp(FG34_HELPER int) RETURNS int LANGUAGE plxgo AS $$ return fg34_helper(FG34_HELPER) $$;
SELECT fg34_callp(2) AS go_3;
-- the arguments of a call are still checked
CREATE FUNCTION fg34_callarg(i int) RETURNS int LANGUAGE plxgo AS $$ var x int = i
return abs(X) $$;
-- the type in var x T and in []T{...} is not a variable
CREATE FUNCTION fg34_type(INT64 bigint) RETURNS bigint LANGUAGE plxgo AS $$ var x int64 = INT64
a := []int64{x, 2}
return a[0] $$;
SELECT fg34_type(4) AS go_4;

-- GH18: + with an operand plx knows is a string is concatenation (||)
CREATE FUNCTION fgh18_itoa(n int) RETURNS text LANGUAGE plxgo AS $$
	return strconv.Itoa(n) + "x"
$$;
SELECT fgh18_itoa(5) AS go_5x;
-- literals, a raw literal, a text parameter and a varchar parameter
CREATE FUNCTION fgh18_lit(s text, v varchar) RETURNS text LANGUAGE plxgo AS $$
	return "a" + `b` + s + v
$$;
SELECT fgh18_lit('c', 'd') AS go_abcd;
-- a declaration from a concatenation is typed text
CREATE FUNCTION fgh18_decl(n int) RETURNS text LANGUAGE plxgo AS $$
	msg := "n=" + strconv.Itoa(n)
	var t string = "<"
	return t + msg + ">"
$$;
SELECT fgh18_decl(7) AS go_n7;
-- a chain runs left to right: the addition inside the call stays addition
CREATE FUNCTION fgh18_chain(n int) RETURNS text LANGUAGE plxgo AS $$
	return "x" + strconv.Itoa(n + 1) + strconv.Itoa(n) + "y"
$$;
SELECT fgh18_chain(1) AS go_x21y;
-- in a comparison, in call arguments, and in a bracketed group
CREATE FUNCTION fgh18_ctx(s text) RETURNS text LANGUAGE plxgo AS $$
	if s + "x" == "ax" && len(s) == 1 {
		return strings.ToUpper(s + "b") + fmt.Sprintf("[%s]", s + "c") + ("d" + (s + "e"))
	}
	return "no"
$$;
SELECT fgh18_ctx('a') AS go_AB_ac_dae, fgh18_ctx('z') AS go_no;
-- calls that return a string, and string(x). string(n) of an integer is the
-- character with that code point, as in Go (PR 24 review [12])
CREATE FUNCTION fgh18_calls(n int) RETURNS text LANGUAGE plxgo AS $$
	return strings.TrimSpace("  z ") + strings.ToLower("Q") + string(n) + fmt.Sprintf("%d", n)
$$;
SELECT fgh18_calls(4) AS go_zq44;
-- an element of a []string local, in a slice literal and in call arguments
CREATE FUNCTION fgh18_slice(s text) RETURNS text LANGUAGE plxgo AS $$
	a := []string{s + "1", "b" + s}
	return a[1] + a[0]
$$;
SELECT fgh18_slice('z') AS go_bzz1;
-- a NULL operand makes the result NULL, as interpolation does
CREATE FUNCTION fgh18_null(s text) RETURNS text LANGUAGE plxgo AS $$
	return "x" + s
$$;
SELECT fgh18_null(NULL) IS NULL AS go_null;
-- + on numbers, and on row fields plx cannot type, stays addition
CREATE FUNCTION fgh18_num(n int) RETURNS int LANGUAGE plxgo AS $$
	for _, r := range query("SELECT 2 AS a, 3 AS b") {
		return n + 1 + r.a + r.b
	}
	return 0
$$;
SELECT fgh18_num(1) AS go_7;
-- a string and a number together is a Go compile error, so plx rejects it
CREATE FUNCTION fgh18_mix(n int) RETURNS text LANGUAGE plxgo AS $$
	return n + "1"
$$;
CREATE FUNCTION fgh18_mix2() RETURNS text LANGUAGE plxgo AS $$
	return "x" + 1
$$;
-- a string beside an operand plx cannot type keeps SQL +, so it still fails
-- loudly; || would mean array append, jsonb merge, or a silent cast here
\set VERBOSITY terse
CREATE FUNCTION fgh18_unk_arr(a int[]) RETURNS int[] LANGUAGE plxgo AS $$
	return a + "{9}"
$$;
-- fg_err(q) runs q and returns 'SQLSTATE: message' if it fails. The message
-- leaves out the error position, which differs between PostgreSQL 13
-- (plpgsql prefixes each expression with SELECT) and later versions.
CREATE FUNCTION fg_err(q text) RETURNS text LANGUAGE plpgsql AS $h$
BEGIN
  EXECUTE q;
  RETURN 'no error';
EXCEPTION WHEN OTHERS THEN
  RETURN SQLSTATE || ': ' || SQLERRM;
END $h$;
SELECT fg_err($q$SELECT fgh18_unk_arr('{1,2}')$q$) AS go_error;
CREATE FUNCTION fgh18_unk_json(j jsonb) RETURNS jsonb LANGUAGE plxgo AS $$
	return j + `{"b":2}`
$$;
SELECT fg_err($q$SELECT fgh18_unk_json('{"a":1}')$q$) AS go_error;
CREATE FUNCTION fgh18_unk_char(c char(4)) RETURNS text LANGUAGE plxgo AS $$
	return c + "x"
$$;
SELECT fg_err($q$SELECT fgh18_unk_char('ab')$q$) AS go_error;
CREATE FUNCTION fgh18_unk_call(s text) RETURNS text LANGUAGE plxgo AS $$
	return length(s) + "x"
$$;
SELECT fg_err($q$SELECT fgh18_unk_call('abc')$q$) AS go_error;
CREATE FUNCTION fgh18_unk_row() RETURNS text LANGUAGE plxgo AS $$
	for _, r := range query("SELECT 2 AS a, 3 AS b") {
		return "x" + r.a
	}
	return "none"
$$;
SELECT fg_err($q$SELECT fgh18_unk_row()$q$) AS go_error;
\set VERBOSITY default
-- such a sum is not a string, so it gives := no type
CREATE FUNCTION fgh18_unk_decl() RETURNS text LANGUAGE plxgo AS $$
	x := []int{1, 2}
	y := x + "{3}"
	return y
$$;
-- string(x) makes the operand a known string
CREATE FUNCTION fgh18_unk_conv() RETURNS text LANGUAGE plxgo AS $$
	for _, r := range query("SELECT 2 AS a, 3 AS b") {
		return string(r.a) + "-" + string(r.b)
	}
	return "none"
$$;
SELECT fgh18_unk_conv() AS go_2_3;

-- GH18: s += x on a string appends with ||
CREATE FUNCTION fgh18_acc(n int) RETURNS text LANGUAGE plxgo AS $$
	s := ""
	for i := 0; i < n; i++ {
		s += strconv.Itoa(i)
	}
	var t string
	t += "a"
	t += `b` + "c"
	return s + t
$$;
SELECT fgh18_acc(3) AS go_012abc;
-- a text parameter; a NULL operand makes the result NULL
CREATE FUNCTION fgh18_accp(p text, q text) RETURNS text LANGUAGE plxgo AS $$
	p += q
	return p
$$;
SELECT fgh18_accp('a', 'x') AS go_ax, fgh18_accp(NULL, 'x') IS NULL AS go_null,
       fgh18_accp('a', NULL) IS NULL AS go_null2;
-- appending a number to a string is a Go compile error
CREATE FUNCTION fgh18_accn() RETURNS text LANGUAGE plxgo AS $$
	s := "a"
	s += 1
	return s
$$;
-- appending an operand plx cannot type keeps s := s + (x), which fails
-- loudly; || would cast a number or a jsonb value to text
\set VERBOSITY terse
CREATE FUNCTION fgh18_accu_row() RETURNS text LANGUAGE plxgo AS $$
	s := "v"
	for _, r := range query("SELECT 2 AS a") {
		s += r.a
	}
	return s
$$;
SELECT fg_err($q$SELECT fgh18_accu_row()$q$) AS go_error;
CREATE FUNCTION fgh18_accu_call(x text) RETURNS text LANGUAGE plxgo AS $$
	s := "v"
	s += length(x)
	return s
$$;
SELECT fg_err($q$SELECT fgh18_accu_call('abc')$q$) AS go_error;
CREATE FUNCTION fgh18_accu_json(j jsonb) RETURNS text LANGUAGE plxgo AS $$
	s := "v"
	s += j
	return s
$$;
SELECT fg_err($q$SELECT fgh18_accu_json('{"a":1}')$q$) AS go_error;
\set VERBOSITY default

-- make() is not supported; plx says so at CREATE FUNCTION instead of failing
-- with a message about something else
CREATE FUNCTION fgmake_var(n int) RETURNS int LANGUAGE plxgo AS $$
	var s []int = make([]int, n)
	return len(s)
$$;
CREATE FUNCTION fgmake_decl(n int) RETURNS int LANGUAGE plxgo AS $$
	s := make([]int, n)
	return len(s)
$$;
CREATE FUNCTION fgmake_case() RETURNS int LANGUAGE plxgo AS $$
	Int := 3
	var s []int = make([]int, Int)
	return len(s)
$$;
-- the alternative the message suggests
CREATE FUNCTION fgmake_fill(n int) RETURNS int[] LANGUAGE plxgo AS $$
	var s []int = array_fill(0, []int{n})
	s[1] = 7
	return s
$$;
SELECT fgmake_fill(3) AS go_0_7_0;

-- PR 24 review [0]: typing nested string concatenation takes linear time,
-- stops at the nesting limit, and does not crash. fg_timed(q, lim) runs q
-- through fg_err and says whether it finished within lim. Before the fix, the
-- 6400-level input took about 40 s and ignored statement_timeout and
-- pg_cancel_backend, and a chain of 200000 unary signs crashed the backend.
CREATE FUNCTION fg_timed(q text, lim interval) RETURNS text LANGUAGE plpgsql AS $h$
DECLARE
  t0 timestamptz := clock_timestamp();
  r text;
BEGIN
  r := fg_err(q);
  RETURN CASE WHEN clock_timestamp() - t0 < lim THEN 'in time' ELSE 'too slow' END
    || ' / ' || r;
END $h$;
SET statement_timeout = '60s';
-- a long flat run inside nesting below the limit compiles, quickly
SELECT fg_timed('CREATE FUNCTION fgdos_wide() RETURNS int LANGUAGE plxgo AS '
  || quote_literal('return len(' || repeat('("x" + ', 200) || repeat('"x" + ', 3000)
  || '"a"' || repeat(')', 200) || ')'), '5s') AS go_wide;
SELECT fgdos_wide() AS go_3201;
-- the reviewer's input is rejected at the nesting limit, quickly
SELECT fg_timed('CREATE FUNCTION fgdos_nest() RETURNS int LANGUAGE plxgo AS '
  || quote_literal('return len(' || repeat('("x" + ', 6400) || '"a"'
  || repeat(')', 6400) || ')'), '5s') AS go_nest_6400;
-- a body of about 360 KB nested 40000 deep is rejected just as quickly
SELECT fg_timed('CREATE FUNCTION fgdos_nest() RETURNS int LANGUAGE plxgo AS '
  || quote_literal('return len(' || repeat('("x" + ', 40000) || '"a"'
  || repeat(')', 40000) || ')'), '5s') AS go_nest_40000;
-- a long chain of unary signs is typed without recursion
SELECT fg_timed('CREATE FUNCTION fgdos_unary() RETURNS int LANGUAGE plxgo AS '
  || quote_literal('x := 1 + ' || repeat('- ', 200000) || '1' || chr(10)
  || 'return x'), '5s') AS go_unary;
-- a string + inside the arguments of a nested call is emitted once, not
-- twice per level (24 levels took about 16 s before the fix)
SELECT fg_timed('CREATE FUNCTION fgdos_args(x text) RETURNS text LANGUAGE plxgo AS '
  || quote_literal('return ' || repeat('strings.ReplaceAll(x, "a", "a" + ', 24) || 'x'
  || repeat(')', 24)), '5s') AS go_args;
SELECT fgdos_args('b') AS go_b;
RESET statement_timeout;
-- PR 24 review [9]: an integer constant on the left of << or >> is shifted as
-- a 64-bit value, as Go gives it the type of its 64-bit context. SQL typed the
-- literal as a 32-bit integer and took the shift count mod 32.
CREATE FUNCTION fgsh_var(n int) RETURNS bigint LANGUAGE plxgo AS $$
 var mask int64 = 1 << n
 return mask
$$;
SELECT fgsh_var(31) AS go_2147483648, fgsh_var(32) AS go_4294967296,
       fgsh_var(35) AS go_34359738368;
CREATE FUNCTION fgsh_const() RETURNS text LANGUAGE plxgo AS $$
 var a int64 = 1 << 40
 var c int64 = 3
 c = c << 33
 var d int64 = -1 << 40
 var e int64 = (1 << 40) >> 39
 return fmt.Sprintf("%d %d %d %d", a, c, d, e)
$$;
SELECT fgsh_const() AS go_1099511627776_25769803776_m1099511627776_2;
-- a 32-bit int target that cannot hold the value fails loudly
CREATE FUNCTION fgsh_int(n int) RETURNS int LANGUAGE plxgo AS $$
 var m int = 1 << n
 return m
$$;
SELECT fgsh_int(4) AS go_16;
SELECT fg_err('SELECT fgsh_int(31)') AS go_out_of_range;
-- PR 24 review [12]: a rune literal is an integer, its code point, as in Go.
-- plx emitted it as a SQL string, so '7' became the number 7 and 'a' failed.
CREATE FUNCTION fgrn_arith(s text) RETURNS int LANGUAGE plxgo AS $$
 var d rune = '7'
 var n int = '7' + 1
 return d * 100 + n
$$;
SELECT fgrn_arith('x') AS go_5556;
CREATE FUNCTION fgrn_var() RETURNS int LANGUAGE plxgo AS $$
 var r rune = 'a'
 return r
$$;
SELECT fgrn_var() AS go_97;
CREATE FUNCTION fgrn_infer() RETURNS text LANGUAGE plxgo AS $$
 c := 'a'
 var d rune = c + 1
 return fmt.Sprintf("%v %v", c, d)
$$;
SELECT fgrn_infer() AS go_97_98;
CREATE FUNCTION fgrn_digit(s text) RETURNS int LANGUAGE plxgo AS $$
 return '7' - '0'
$$;
SELECT fgrn_digit('x') AS go_7;
-- string(r) and %c turn a rune back into its character
CREATE FUNCTION fgrn_char() RETURNS text LANGUAGE plxgo AS $$
 var r rune = 'a' + 1
 return fmt.Sprintf("%s|%c|%d", string(r), 'z', 'é')
$$;
SELECT fgrn_char() AS go_b_z_233;
-- a rune literal holds exactly one character
CREATE FUNCTION fgrn_two() RETURNS int LANGUAGE plxgo AS $$ return 'ab' $$;
CREATE FUNCTION fgrn_empty() RETURNS int LANGUAGE plxgo AS $$ return '' $$;
-- %c where the directives and arguments do not line up is rejected, since the
-- format() lowering would print the number
CREATE FUNCTION fgrn_cfall() RETURNS text LANGUAGE plxgo AS $$ return fmt.Sprintf("%c %d", 'a') $$;
-- PR 24 review [14]: a * width or precision in fmt.Sprintf takes its value
-- from the next argument, as in Go. plx printed the directive as text and
-- gave every later verb the wrong argument.
CREATE FUNCTION fgsp_star() RETURNS text LANGUAGE plxgo AS $$
 n := 6
 return fmt.Sprintf("[%*d|%s]", 5, 42, "x") + fmt.Sprintf("[%-*s|%d]", 4, "ab", 7) +
   fmt.Sprintf("[%*d]", -4, 7) + fmt.Sprintf("[%*s]", n, "ab") + fmt.Sprintf("[%*d]", 1, 123)
$$;
SELECT fgsp_star() AS go_star;
-- a * precision takes its argument too; the precision itself is dropped, as
-- for a written precision, so 3.5 prints in full
CREATE FUNCTION fgsp_prec() RETURNS text LANGUAGE plxgo AS $$
 return fmt.Sprintf("[%.*f|%d]", 2, 3.5, 9)
$$;
SELECT fgsp_prec() AS go_prec;
-- where the directives and arguments do not line up, * is rejected, since the
-- format() lowering cannot take a width from an argument
CREATE FUNCTION fgsp_starfall() RETURNS text LANGUAGE plxgo AS $$ return fmt.Sprintf("%*d %d", 5, 42) $$;
-- Review of the PR 24 fixes, problem 1: a rune literal on the left of a shift
-- is an integer constant too, so it is shifted as a 64-bit value. It was
-- shifted as a 32-bit integer, with the count taken mod 32.
CREATE FUNCTION fgsh_rune(n int) RETURNS text LANGUAGE plxgo AS $$
 var m int64 = 'a' << 40
 var k int64 = 'a' << n
 return fmt.Sprintf("%d %d", m, k)
$$;
SELECT fgsh_rune(40) AS go_rune_shift;
-- Review of the PR 24 fixes, problem 2: string(x) is a character only for an
-- integer x. Go rejects string(x) of a float or a bool, and so does plx at
-- CREATE FUNCTION. plx made each of these a character, or a run-time error.
CREATE FUNCTION fgstr_float(f float8) RETURNS text LANGUAGE plxgo AS $$ return string(f) $$;
CREATE FUNCTION fgstr_flit() RETURNS text LANGUAGE plxgo AS $$ return string(1.5) $$;
CREATE FUNCTION fgstr_bool(b bool) RETURNS text LANGUAGE plxgo AS $$ return string(b) $$;
CREATE FUNCTION fgstr_true() RETURNS text LANGUAGE plxgo AS $$ return string(true) $$;
CREATE FUNCTION fgstr_num(x numeric) RETURNS text LANGUAGE plxgo AS $$ return string(x + x) $$;
CREATE FUNCTION fgstr_conv(n int) RETURNS text LANGUAGE plxgo AS $$ return string(float64(n)) $$;
-- an integer operand still gives its character
CREATE FUNCTION fgstr_int(n int) RETURNS text LANGUAGE plxgo AS $$
 var r rune = 'a'
 return string(n) + string(r + 1) + string(int(66.9))
$$;
SELECT fgstr_int(65) AS go_abb;
-- Review of the PR 24 fixes, problem 4: string(x) of any integer expression
-- is its character, not only of a + of integers. string(n * 2) gave "66".
CREATE FUNCTION fgstr_expr(n int) RETURNS text LANGUAGE plxgo AS $$
 return string(n * 2) + string(n - 1) + string((n + 1) * 2) + string(n << 1) + string(n / 1)
$$;
SELECT fgstr_expr(33) AS go_chars;
-- a float in the expression makes it a float, which Go rejects here
CREATE FUNCTION fgstr_fexpr(n int, f float8) RETURNS text LANGUAGE plxgo AS $$ return string(n * f) $$;
-- Review of the PR 24 fixes, problem 3: the left operand of a shift is an
-- integer constant also when it is a constant expression or is in brackets.
-- Only a bare literal was shifted as a 64-bit value, so 2 * 1 << 40 gave 512
-- and (1) << 40 gave 256.
CREATE FUNCTION fgsh_cexpr() RETURNS text LANGUAGE plxgo AS $$
 var a int64 = 2 * 1 << 40
 var b int64 = (1) << 40
 var c int64 = (1 + 1) << 40
 var d int64 = -(1) << 40
 var e int64 = 'a' * 1 << 40
 return fmt.Sprintf("%d %d %d %d %d", a, b, c, d, e)
$$;
SELECT fgsh_cexpr() AS go_shifts;
-- Review of the PR 24 fixes, problem 3: a shift count at least as large as the
-- width of the value. Go gives 0 for <<, and 0 or -1 for >>. SQL takes the
-- count mod 64 for a bigint and mod 32 for an integer, so 1 << 64 gave 1.
CREATE FUNCTION fgsh_count(n int) RETURNS text LANGUAGE plxgo AS $$
 var a int64 = 1 << n
 var x int64 = -8
 var y int64 = 8
 var z int64 = 5
 z <<= n
 return fmt.Sprintf("%d %d %d %d", a, x >> n, y >> n, z)
$$;
SELECT fgsh_count(63) AS go_63, fgsh_count(64) AS go_64, fgsh_count(65) AS go_65;
SELECT fgsh_count(NULL) IS NULL AS go_null;
-- a negative count panics in Go; plx fails
SELECT fg_err('SELECT fgsh_count(-1)') AS go_negative;
-- an integer (32 bits in plx) shifted right by 32 or more is 0 or -1
CREATE FUNCTION fgsh_r32(v int, n int) RETURNS int LANGUAGE plxgo AS $$ return v >> n $$;
SELECT fgsh_r32(-8, 33) AS go_m1, fgsh_r32(8, 33) AS go_0, fgsh_r32(-8, 1) AS go_m4;
-- shifted left by 32 or more, a non-zero integer does not fit: Go's int is 64
-- bits wide, so plx fails as it does for any shift that leaves the range
CREATE FUNCTION fgsh_l32(v int, n int) RETURNS int LANGUAGE plxgo AS $$ return v << n $$;
SELECT fgsh_l32(1, 4) AS go_16, fgsh_l32(0, 40) AS go_0;
SELECT fg_err('SELECT fgsh_l32(1, 32)') AS go_out_of_range;
-- Each such shift is a subquery, and PostgreSQL needs memory that grows faster
-- than linearly to plan subqueries nested in one another: shifts nested 800
-- deep ran out of memory. plx allows 64 nested in one another.
CREATE FUNCTION fgsh_mk(k int, lv int) RETURNS text LANGUAGE sql AS $f$
  SELECT format('CREATE FUNCTION fgsh_many_%s_%s(n int) RETURNS bigint LANGUAGE plxgo AS %L',
                k, lv, 'var x int64 = 1' || chr(10) || 'return ' || repeat('(', lv) || 'x' ||
                repeat(repeat(' << n', k) || ')', lv))
$f$;
SELECT fg_err(fgsh_mk(64, 1)) AS go_no_error;
SELECT fgsh_many_64_1(0) AS go_1;
SELECT fg_err(fgsh_mk(65, 1)) AS go_too_many;
SELECT fg_err(fgsh_mk(100, 8)) AS go_too_many;
-- Review of the PR 24 fixes: plx evaluates some values once in a subquery (a
-- * width, a width of a call, an integer conversion of an expression). The
-- planner pulled each subquery up and copied the value into every place it
-- is used, so nesting them grew exponentially: 18 nested * widths took about
-- 14 s, and 8 nested int64() conversions ran out of memory.
DO $d$
BEGIN
  EXECUTE format('CREATE FUNCTION fgsq_star(n int) RETURNS text LANGUAGE plxgo AS %L',
    'return ' || repeat('fmt.Sprintf("%*s", n, ', 18) || '"a"' || repeat(')', 18));
  EXECUTE format('CREATE FUNCTION fgsq_width(x text) RETURNS text LANGUAGE plxgo AS %L',
    'return ' || repeat('fmt.Sprintf("%3s", strings.ToUpper(', 18) || 'x' || repeat('))', 18));
  EXECUTE format('CREATE FUNCTION fgsq_int(n bigint) RETURNS bigint LANGUAGE plxgo AS %L',
    'return ' || repeat('int64(', 8) || 'n + 1' || repeat(' + 1)', 8));
END $d$;
SET statement_timeout = '5s';
SELECT fg_timed('SELECT fgsq_star(2)', '2s') AS go_star;
SELECT fg_timed('SELECT fgsq_width(''a'')', '2s') AS go_width;
SELECT fg_timed('SELECT fgsq_int(2)', '2s') AS go_int;
RESET statement_timeout;
SELECT fgsq_star(2) AS go_star, fgsq_width('a') AS go_width, fgsq_int(2) AS go_11;
-- R2-D11 (PR 24 review r2 d1[1]): the variable of a three-clause for loop is
-- an integer while plx emits the loop body, so string(j) is a character, as
-- in Go (review [12] covered locals and parameters only). The name is
-- typed for the body only: after the loop a new j can hold a string.
CREATE FUNCTION fg_r2d11(p text) RETURNS text LANGUAGE plxgo AS $$
	i := 65
	var s string = string(i)
	for j := 66; j < 67; j++ {
		s = s + string(j)
	}
	for k := 67; k < 68; k++ {
		var kk int = k
		s = s + string(kk) + string(k + 0) + string(int(k))
	}
	j := "-"
	s = s + j + p
	return s
$$;
SELECT fg_r2d11('!') AS r;  -- Go: ABCCC-!
-- R2-C20 (PR 24 review r2 c2[0]): a named integer constant on the left of a
-- shift is an untyped constant in Go, like a literal, so it shifts as a
-- 64-bit value (review [9] covered literals only).
CREATE FUNCTION fg_r2c20() RETURNS text LANGUAGE plxgo AS $$
	const mask = 0xFF
	const one = 1
	var m int64 = mask << 28
	var n int64 = one << 40
	return fmt.Sprintf("%d %d", m, n)
$$;
SELECT fg_r2c20() AS r;  -- Go: 68451041280 1099511627776
-- R2-C21 (PR 24 review r2 c2[1]): plx maps Go's int to a 32-bit integer, so
-- an int shifted left out of that range fails with "integer out of range",
-- for every count, as plxgo.md says. Counts below 32 wrapped silently.
CREATE FUNCTION fg_r2c21(x int, k int) RETURNS bigint LANGUAGE plxgo AS $$
	var r int64 = int64(x << k)
	return r
$$;
SELECT fg_r2c21(3, 29) AS r;  -- 1610612736
SELECT fg_err($q$SELECT fg_r2c21(3, 30)$q$) AS r;  -- 22003: integer out of range
SELECT fg_err($q$SELECT fg_r2c21(1, 31)$q$) AS r;  -- 22003: integer out of range
SELECT fg_r2c21(-1, 31) AS r;  -- -2147483648 fits
CREATE FUNCTION fg_r2c21_lit(x int) RETURNS int LANGUAGE plxgo AS $$
	return x << 31
$$;
SELECT fg_err($q$SELECT fg_r2c21_lit(1)$q$) AS r;  -- 22003: integer out of range
CREATE FUNCTION fg_r2c21_big(x bigint, k int) RETURNS bigint LANGUAGE plxgo AS $$
	return x << k
$$;
SELECT fg_r2c21_big(3, 30) AS r;  -- 3221225472

-- R3-D30 (PR 24 review r3): a constant declared with a type has that type
-- (Go spec, Constant declarations); only an untyped constant takes its type
-- from the context of a shift. Round 2 treated every integer constant as
-- untyped and widened it to bigint, so a typed constant's overflow was
-- silently lost (int32: 6442450944, int16: 98304). int32 is integer, and an
-- integer shifted out of range fails (doc/plxgo.md); an int16 drops the bits
-- that leave its width.
CREATE FUNCTION fg_r3d30_i32() RETURNS bigint LANGUAGE plxgo AS $$
const m int32 = 3
k := 31
var a int64 = int64(m << k)
return a
$$;
\set VERBOSITY terse
SELECT fg_r3d30_i32() AS r;  -- integer out of range (Go: -2147483648)
\set VERBOSITY default
CREATE FUNCTION fg_r3d30() RETURNS text LANGUAGE plxgo AS $$
const h int16 = 3
const u = 3
k := 31
j := 15
var b int64 = int64(h << j)
var c int64 = u << k
return fmt.Sprintf("%d %d", b, c)
$$;
SELECT fg_r3d30() AS r;  -- Go: -32768 6442450944

-- R3-E30 (PR 24 review r3): the loop variable of a three-clause for is a new
-- int that shadows a parameter of the same name (Go spec, For statements),
-- so string(i) is the character with that code point.
CREATE FUNCTION fg_r3e30(i text) RETURNS text LANGUAGE plxgo AS $$
r := ""
for i := 65; i < 67; i++ {
	r += string(i)
}
return r + i
$$;
SELECT fg_r3e30('p') AS r;  -- Go: ABp

-- R3-G30 (PR 24 review r3): an int16 shifted left by 16 or more is 0 (Go
-- spec, Arithmetic operators: shifts behave as if shifted n times by 1).
-- A count of 32 or more failed with integer out of range.
CREATE FUNCTION fg_r3g30(n int) RETURNS text LANGUAGE plxgo AS $$
var a int16 = 1
var c int16 = a << n
return fmt.Sprintf("%d", c)
$$;
SELECT fg_r3g30(15) AS a, fg_r3g30(16) AS b, fg_r3g30(40) AS c;  -- Go: -32768 0 0

-- R4-D40 (PR 24 review r4): a loop variable that shadows an untyped
-- constant is a new int32 variable (Go spec, For statements), not the
-- constant. It kept the constant's untyped flag, so a << k was widened to
-- bigint: 2147483648 for 1 << 31, where an int32 that leaves its range
-- fails (doc/plxgo.md). An int16 loop variable drops the bits past 16.
CREATE FUNCTION fg_r4d40(k int) RETURNS text LANGUAGE plxgo AS $$
const a = 1
var s int64 = 0
for a := int32(1); a < 2; a++ {
	s = int64(a << k)
}
return fmt.Sprintf("%d", s)
$$;
\set VERBOSITY terse
SELECT fg_r4d40(31) AS r;  -- integer out of range (Go: -2147483648)
\set VERBOSITY default
CREATE FUNCTION fg_r4d40_16(k int) RETURNS text LANGUAGE plxgo AS $$
const a = 1
var s int64 = 0
for a := int16(1); a < 2; a++ {
	s = int64(a << k)
}
return fmt.Sprintf("%d %d", s, a << 40)
$$;
SELECT fg_r4d40_16(15) AS r;  -- Go: -32768 1099511627776

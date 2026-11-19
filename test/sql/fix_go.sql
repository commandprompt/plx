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
CREATE FUNCTION fg33_rune() RETURNS text LANGUAGE plxgo AS $$ return '\x41' $$;
SELECT fg33_rune() AS go_a;
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

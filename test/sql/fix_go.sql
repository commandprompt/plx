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

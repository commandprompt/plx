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

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

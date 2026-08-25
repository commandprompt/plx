-- plxgo regression tests (Go dialect)
CREATE EXTENSION IF NOT EXISTS plx;
SET client_min_messages = warning;

-- := type inference, C-style for, compound assignment, type conversion
CREATE FUNCTION g_fact(n int) RETURNS bigint LANGUAGE plxgo AS $$
	var acc int64 = 1
	for i := 1; i <= n; i++ {
		acc *= int64(i)
	}
	return acc
$$;
SELECT g_fact(0) AS f0, g_fact(5) AS f120, g_fact(10) AS f3628800;

-- if / else if / else, string returns (Go "..." -> SQL '...')
CREATE FUNCTION g_grade(score int) RETURNS text LANGUAGE plxgo AS $$
	if score >= 90 {
		return "A"
	} else if score >= 80 {
		return "B"
	} else {
		return "F"
	}
$$;
SELECT g_grade(95) AS a, g_grade(85) AS b, g_grade(50) AS f;

-- switch with a tag: case values (incl. comma lists) and default -> IF/ELSIF
CREATE FUNCTION g_day(n int) RETURNS text LANGUAGE plxgo AS $$
	switch n {
	case 0, 6:
		return "weekend"
	case 1, 2, 3, 4, 5:
		return "weekday"
	default:
		return "invalid"
	}
$$;
SELECT g_day(0) AS wknd, g_day(3) AS wkdy, g_day(9) AS inv;

-- tagless switch (each case is a boolean condition)
CREATE FUNCTION g_sign(n int) RETURNS text LANGUAGE plxgo AS $$
	switch {
	case n > 0:
		return "positive"
	case n < 0:
		return "negative"
	default:
		return "zero"
	}
$$;
SELECT g_sign(5) AS pos, g_sign(-3) AS neg, g_sign(0) AS zero;

-- slice: append, for-range over a slice (value type inferred from the slice), len()
CREATE FUNCTION g_sumsq(nums int) RETURNS int LANGUAGE plxgo AS $$
	var s []int
	for i := 1; i <= nums; i++ {
		s = append(s, i*i)
	}
	total := 0
	for _, v := range s {
		total += v
	}
	return total * 100 + len(s)
$$;
SELECT g_sumsq(3) AS should_be_1403;

-- continue in a counting for must still advance the loop variable (the loop
-- lowers to a plpgsql integer FOR, not a WHILE with a trailing increment)
CREATE FUNCTION g_skip(n int) RETURNS int LANGUAGE plxgo AS $$
	sum := 0
	for i := 1; i <= n; i++ {
		if i % 3 == 0 {
			continue
		}
		sum += i
	}
	return sum
$$;
SELECT g_skip(10) AS should_be_37;

-- a decrement counting loop lowers to FOR ... IN REVERSE
CREATE FUNCTION g_countdown(n int) RETURNS text LANGUAGE plxgo AS $$
	var b []int
	for i := n; i >= 1; i-- {
		b = append(b, i)
	}
	return array_to_string(b, ",")
$$;
SELECT g_countdown(4) AS should_be_4_3_2_1;

-- integer range (Go 1.22): for i := range n
CREATE FUNCTION g_count(n int) RETURNS int LANGUAGE plxgo AS $$
	total := 0
	for i := range n {
		total += i
	}
	return total
$$;
SELECT g_count(5) AS should_be_10;

-- infinite for with break, and a plain for-condition (while)
CREATE FUNCTION g_firstpow2(min int) RETURNS int LANGUAGE plxgo AS $$
	x := 1
	for {
		if x >= min {
			break
		}
		x *= 2
	}
	return x
$$;
SELECT g_firstpow2(100) AS should_be_128;

-- multiple short declaration and swap
CREATE FUNCTION g_gcd(a int, b int) RETURNS int LANGUAGE plxgo AS $$
	for b != 0 {
		a, b = b, a % b
	}
	return a
$$;
SELECT g_gcd(48, 36) AS should_be_12;

-- multiple-target short declaration (exercises the RHS pair table)
CREATE FUNCTION g_multi4() RETURNS int LANGUAGE plxgo AS $$
	a, b, c, d := 1, 2, 3, 4
	return a*1000 + b*100 + c*10 + d
$$;
SELECT g_multi4() AS should_be_1234;

-- panic -> RAISE EXCEPTION, fmt.Println -> RAISE NOTICE
CREATE FUNCTION g_safe(n int) RETURNS int LANGUAGE plxgo AS $$
	if n == 0 {
		panic("cannot be zero")
	}
	return 100 / n
$$;
SELECT g_safe(4) AS should_be_25;
SELECT g_safe(0) AS boom;

-- Go slices are 0-based; subscripts are rewritten to PostgreSQL's 1-based arrays
CREATE FUNCTION g_index(k int) RETURNS int LANGUAGE plxgo AS $$
	a := []int{10, 20, 30, 40}
	var sum int = a[0] + a[3]
	for i := range k {
		sum += a[i]
	}
	return sum
$$;
SELECT g_index(2) AS should_be_80;

-- fmt.Println / fmt.Printf: one % placeholder per value argument (empty is ok)
CREATE FUNCTION g_print() RETURNS int LANGUAGE plxgo AS $$
	fmt.Println("a", 1, true)
	fmt.Printf("%d-%d", 2, 3)
	fmt.Println()
	return 0
$$;
SELECT g_print() AS zero;

-- stdlib: strings.ToUpper returned directly (no + concatenation)
CREATE FUNCTION g_upper(s text) RETURNS text LANGUAGE plxgo AS $$
	return strings.ToUpper(s)
$$;
SELECT g_upper('hello') AS should_be_HELLO;

-- stdlib: math.Sqrt with float64()/int() conversions, len(), numeric result
CREATE FUNCTION g_lib(s text, n int) RETURNS int LANGUAGE plxgo AS $$
	root := int(math.Sqrt(float64(n)))
	return len(strings.ToUpper(s)) + root
$$;
SELECT g_lib('hi', 16) AS should_be_6;

-- set-returning via emit(), and a range over query()
CREATE FUNCTION g_series(n int) RETURNS SETOF int LANGUAGE plxgo AS $$
	for i := range n {
		emit(i * i)
	}
$$;
SELECT string_agg(g_series::text, ',') AS squares FROM g_series(4) AS g_series;

CREATE FUNCTION g_rowcount() RETURNS int LANGUAGE plxgo AS $$
	total := 0
	for _, r := range query("SELECT g FROM generate_series(1,5) AS g") {
		total += r.g
	}
	return total
$$;
SELECT g_rowcount() AS should_be_15;

-- const declaration
CREATE FUNCTION g_circle(r float8) RETURNS float8 LANGUAGE plxgo AS $$
	const pi = 3.14159
	return pi * r * r
$$;
SELECT round(g_circle(2.0)::numeric, 5) AS should_be_12_56636;

-- fmt.Sprintf in expression position: the format string is reproduced, and Go
-- verbs are rewritten to the specifiers SQL format() understands
CREATE FUNCTION g_sprintf(nm text, n int) RETURNS text LANGUAGE plxgo AS $$
	return fmt.Sprintf("user %s has %d items", nm, n)
$$;
SELECT g_sprintf('bob', 3) AS should_be_user_bob_has_3_items;

-- %v (Go's default verb), a doubled %% literal, and a width field
CREATE FUNCTION g_sprintf_verbs(n int) RETURNS text LANGUAGE plxgo AS $$
	return fmt.Sprintf("[%v] [%5d] [%-4d] 100%% done", n, n, n)
$$;
SELECT g_sprintf_verbs(7) AS verbs;

-- verbs with a precision field, which SQL format() has no equivalent for
CREATE FUNCTION g_sprintf_prec(x float8) RETURNS text LANGUAGE plxgo AS $$
	return fmt.Sprintf("%.2f|%8.3f", x, x)
$$;
SELECT g_sprintf_prec(1.5) AS prec;

-- a '%' that starts no directive reaches the caller as a literal percent
-- rather than tripping format() at run time
CREATE FUNCTION g_sprintf_bare_pct() RETURNS text LANGUAGE plxgo AS $$
	return fmt.Sprintf("100%")
$$;
SELECT g_sprintf_bare_pct() AS should_be_100_pct;

CREATE FUNCTION g_sprintf_pct_punct(n int) RETURNS text LANGUAGE plxgo AS $$
	return fmt.Sprintf("%d%!", n)
$$;
SELECT g_sprintf_pct_punct(50) AS pct_then_punct;

-- verbs that change an operand's representation render what %s renders
CREATE FUNCTION g_sprintf_repr(n int) RETURNS text LANGUAGE plxgo AS $$
	return fmt.Sprintf("%x|%o|%b|%q", n, n, n, n)
$$;
SELECT g_sprintf_repr(255) AS repr_verbs_are_text;

-- fmt.Sprintf concatenates, so a NULL operand propagates rather than rendering
-- as an empty string
CREATE FUNCTION g_sprintf_null(nm text, n int) RETURNS text LANGUAGE plxgo AS $$
	return fmt.Sprintf("user %s has %d items", nm, n)
$$;
SELECT g_sprintf_null('bob', 3) AS ok,
       g_sprintf_null(NULL, 3) IS NULL AS null_name,
       g_sprintf_null('bob', NULL) IS NULL AS null_count;

-- but a panic message keeps its literal text when an operand is NULL
CREATE FUNCTION g_panic_msg(who text) RETURNS int LANGUAGE plxgo AS $$
	panic(fmt.Sprintf("bad user %s here", who))
$$;
DO $d$ BEGIN PERFORM g_panic_msg(NULL);
EXCEPTION WHEN OTHERS THEN RAISE NOTICE 'caught: [%]', SQLERRM; END $d$;

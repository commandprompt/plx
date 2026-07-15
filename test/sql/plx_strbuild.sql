-- plx_strbuild correctness tests (performance is covered by the benchmark).
CREATE EXTENSION IF NOT EXISTS plx;
SET client_min_messages = warning;

-- build by appending; result and length are correct
CREATE FUNCTION sb1(n int) RETURNS text LANGUAGE plpgsql AS $$
DECLARE s plx_strbuild := ''; i int;
BEGIN FOR i IN 1..n LOOP s := plx_sb_append(s, 'x'); END LOOP; RETURN s; END; $$;
SELECT sb1(5), length(sb1(5));

-- start from a non-empty value; append mixed content
CREATE FUNCTION sb2() RETURNS text LANGUAGE plpgsql AS $$
DECLARE s plx_strbuild := 'start'; i int;
BEGIN FOR i IN 1..3 LOOP s := plx_sb_append(s, '-' || i::text); END LOOP; RETURN s; END; $$;
SELECT sb2();

-- a NULL accumulator starts empty (append is not strict)
CREATE FUNCTION sb3() RETURNS text LANGUAGE plpgsql AS $$
DECLARE s plx_strbuild;
BEGIN s := plx_sb_append(s, 'a'); s := plx_sb_append(s, 'b'); RETURN s; END; $$;
SELECT sb3();

-- a NULL suffix appends nothing
CREATE FUNCTION sb4() RETURNS text LANGUAGE plpgsql AS $$
DECLARE s plx_strbuild := 'x';
BEGIN s := plx_sb_append(s, NULL); s := plx_sb_append(s, 'y'); RETURN s; END; $$;
SELECT sb4();

-- reading the builder mid-loop (forces a flatten) does not corrupt it
CREATE FUNCTION sb5() RETURNS text LANGUAGE plpgsql AS $$
DECLARE s plx_strbuild := ''; i int;
BEGIN
  FOR i IN 1..4 LOOP s := plx_sb_append(s, length(s::text)::text); END LOOP;
  RETURN s;
END; $$;
SELECT sb5();

-- cast round trip text -> plx_strbuild -> text, including multibyte and quotes
SELECT ('caf'||chr(233)||' ''x''')::plx_strbuild::text AS roundtrip;

-- empty builder flattens to empty string, not NULL
CREATE FUNCTION sb6() RETURNS text LANGUAGE plpgsql AS $$
DECLARE s plx_strbuild := '';
BEGIN RETURN coalesce(s::text, '<null>') || '|end'; END; $$;
SELECT sb6();

-- implicitly usable where text is expected: function calls and comparison
SELECT upper('ab'::plx_strbuild) AS as_text_fn,
       ('ab'::plx_strbuild = 'ab') AS compares_equal;

-- loop string-accumulation is lowered to the builder in every dialect
CREATE FUNCTION acc_rb(n int) RETURNS text LANGUAGE plxruby AS $$
s = "" #:: text
for i in 1..n
  s << i.to_s
end
return s
$$;
SELECT acc_rb(5);
-- the lowered plpgsql uses plx_strbuild + plx_sb_append
SELECT regexp_replace(prosrc, '/\*plx-orig:[^*]*\*/\s*', '', 'g') FROM pg_proc WHERE proname = 'acc_rb';

CREATE FUNCTION acc_php(n int) RETURNS text LANGUAGE plxphp AS $$
$s = "" /*:: text */;
for ($i = 1; $i <= $n; $i++) { $s .= $i; }
return $s;
$$;
SELECT acc_php(5);

CREATE FUNCTION acc_js(n int) RETURNS text LANGUAGE plxjs AS $$
let s = "" /*:: text */;
for (let i = 1; i <= n; i++) { s += i; }
return s;
$$;
SELECT acc_js(5);

CREATE FUNCTION acc_py(n int) RETURNS text LANGUAGE plxpython3 AS $$
s = "" #:: text
for i in range(1, n + 1):
    s += str(i)
return s
$$;
SELECT acc_py(5);

-- a numeric += is left as numeric addition, not string append
CREATE FUNCTION acc_num(n int) RETURNS int LANGUAGE plxjs AS $$
let t = 0;
for (let i = 1; i <= n; i++) { t += i; }
return t;
$$;
SELECT acc_num(5);

-- large build exercises the repalloc doubling; verify exact content via md5
CREATE FUNCTION sb_big(n int) RETURNS text LANGUAGE plpgsql AS $$
DECLARE s plx_strbuild := ''; i int;
BEGIN FOR i IN 1..n LOOP s := plx_sb_append(s, 'abcde'); END LOOP;
  RETURN md5(s::text) || '/' || length(s::text); END; $$;
SELECT sb_big(10000) = md5(repeat('abcde', 10000)) || '/50000' AS big_ok;

-- appending the accumulator to itself (doubling) is safe
CREATE FUNCTION sb_self(n int) RETURNS int LANGUAGE plpgsql AS $$
DECLARE s plx_strbuild := 'a'; i int;
BEGIN FOR i IN 1..n LOOP s := plx_sb_append(s, s); END LOOP; RETURN length(s::text); END; $$;
SELECT sb_self(10) AS self_append_pow2;

-- a varchar-typed accumulator also lowers to the builder
CREATE FUNCTION acc_vc(n int) RETURNS text LANGUAGE plxruby AS $$
s = "" #:: varchar
for i in 1..n
  s << "z"
end
return s
$$;
SELECT acc_vc(4);

-- multibyte content is not corrupted across buffer growth
CREATE FUNCTION acc_mb(n int) RETURNS text LANGUAGE plxruby AS $$
s = "" #:: text
for i in 1..n
  s << "é中"
end
return length(s) || ':' || substring(s from 1 for 4)
$$;
SELECT acc_mb(50);

-- a plain assignment mid-build resets the accumulator correctly
CREATE FUNCTION acc_reset(n int) RETURNS text LANGUAGE plxruby AS $$
s = "" #:: text
for i in 1..n
  s << "a"
  if i == 2
    s = "R"
  end
end
return s
$$;
SELECT acc_reset(4);

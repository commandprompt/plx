-- plxcsharp regression tests (C# dialect)
CREATE EXTENSION IF NOT EXISTS plx;
SET client_min_messages = warning;

SELECT lanname, lanpltrusted FROM pg_language WHERE lanname = 'plxcsharp';

-- the generated plpgsql, without the sentinel line and the embedded original
CREATE FUNCTION cs_src(fn text) RETURNS text LANGUAGE sql AS $f$
  SELECT regexp_replace(prosrc, '^/\*plx:v1:[^*]*\*/\n|/\*plx-orig.*$', '', 'gs')
  FROM pg_proc WHERE proname = fn
$f$;

-- a CREATE FUNCTION that must fail cleanly: returns the error message
CREATE FUNCTION cs_try(body text) RETURNS text LANGUAGE plpgsql AS $f$
BEGIN
  EXECUTE format('CREATE OR REPLACE FUNCTION cs_probe() RETURNS text LANGUAGE plxcsharp AS %L', body);
  RETURN 'ok';
EXCEPTION WHEN OTHERS THEN
  RETURN SQLERRM;
END
$f$;

-- ---------------------------------------------------------------- basics

CREATE FUNCTION cs_add(a int, b int) RETURNS int LANGUAGE plxcsharp AS $$
return a + b * 2;
$$;
SELECT cs_add(3, 4);

-- if / else if / else, Allman braces and braceless bodies
CREATE FUNCTION cs_grade(score int) RETURNS text LANGUAGE plxcsharp AS $$
string grade;
if (score >= 90)
{
    grade = "A";
}
else if (score >= 80)
    grade = "B";
else
{
    grade = "F";
}
return grade;
$$;
SELECT cs_grade(95), cs_grade(85), cs_grade(10), cs_grade(NULL);
SELECT cs_src('cs_grade');

-- declarations: C# types map to PostgreSQL types; var infers from the value
CREATE FUNCTION cs_types() RETURNS text LANGUAGE plxcsharp AS $$
int i = 1;
long l = 2;
short sh = 3;
byte bt = 4;
bool b = true;
string s = "five";
char c = '6';
double d = 7.5;
float f = 8.5f;
decimal m = 9.25m;
DateTime dt = DateTime.Parse("2024-03-15 10:20:30");
DateOnly day = DateOnly.Parse("2024-03-15");
TimeSpan span = TimeSpan.Parse("01:02:03");
Guid g = Guid.Parse("a0eebc99-9c0b-4ef8-bb6d-6bb9bd380a11");
byte[] raw = null;
int[] arr = {1, 2, 3};
var v1 = 10;
var v2 = "text";
var v3 = 1.5;
var v4 = 5L;
int? maybe = null;
numeric(10,2) money = 12.345;
const int Max = 100;
return $"{i} {l} {sh} {bt} {b} {s} {c} {d} {f} {m} {dt} {day} {span} {g} {raw is null} {arr[0]} {v1} {v2} {v3} {v4} {maybe ?? -1} {money} {Max}";
$$;
SELECT cs_types();
SELECT cs_src('cs_types');

-- ---------------------------------------------------------------- strings

-- regular strings with C# escapes, verbatim @"..." (backslashes literal, ""
-- is a quote), raw """...""" (indentation stripped), char literals
CREATE FUNCTION cs_strings() RETURNS text LANGUAGE plxcsharp AS $$
string a = "tab\tquote\"back\\slash \u00e9 \x41";
string b = @"C:\temp\new ""quoted""";
string c = """
    SELECT "raw"
      indented
    """;
char ch = '\'';
return a + "|" + b + "|" + c + "|" + ch;
$$;
SELECT cs_strings();

-- interpolated $"...", $@"...", raw $"""...""" ($$ for literal braces),
-- {{ }} escapes, nested interpolation, the F format, and conditional holes
CREATE FUNCTION cs_interp(nm text, n int) RETURNS text LANGUAGE plxcsharp AS $cs$
string a = $"user {nm} has {n} items {{braces}}";
string b = $@"C:\{nm}\x";
string c = $$"""{literal} {{n * 2}}""";
string d = $"[{$"<{n}>"}] {(n > 1 ? "many" : "one")} {n / 3.0:F2}";
return a + "|" + b + "|" + c + "|" + d;
$cs$;
SELECT cs_interp('bob', 3);
-- an interpolated NULL makes the whole string NULL, as in the other dialects
SELECT cs_interp(NULL, 3) IS NULL AS null_propagates;

-- '+' is string concatenation when either side is a string, and numeric
-- addition otherwise; C# evaluates left to right
CREATE FUNCTION cs_plus(a int, b int, s text) RETURNS text LANGUAGE plxcsharp AS $$
string r = "sum=" + a + b;
string q = a + b + "=sum";
string t = s + (a + b);
string u = 'x' + s;
return r + ";" + q + ";" + t + ";" + u;
$$;
SELECT cs_plus(1, 2, 'x');
SELECT cs_src('cs_plus');

-- string members map to SQL functions; indexes are 0-based
CREATE FUNCTION cs_members(s text) RETURNS text LANGUAGE plxcsharp AS $$
string t = s.Trim().ToUpper();
return string.Join("/", new[] {
    t, t.Length.ToString(), t.Contains("ELL").ToString(), t.IndexOf("L").ToString(),
    t.Substring(1, 3), t.Substring(3), s.Replace("l", "L"), t.StartsWith("HE").ToString(),
    t.EndsWith("LO").ToString(), t.PadLeft(8, '*'), t.PadRight(7) + "|", t[0].ToString(),
    t.ToLower(), s.TrimStart(), s.TrimEnd() + "|", string.IsNullOrEmpty(s).ToString(),
    string.IsNullOrWhiteSpace("  ").ToString() });
$$;
SELECT cs_members('  hello ');

CREATE FUNCTION cs_strstatic(a text, b text) RETURNS text LANGUAGE plxcsharp AS $$
string[] parts = "a,b,c".Split(',');
return string.Format("{0}-{1,5}-{0}|{2,-4}|", a, b, 7) + string.Concat(a, null, b) + "|" +
       string.Join(",", parts) + "|" + string.Join("+", a, b, 3) + "|" + parts.Length +
       "|" + string.Empty + "|" + nameof(parts) + "|" + Environment.NewLine.Length;
$$;
SELECT cs_strstatic('x', 'y');

-- string += in a loop uses the plx_strbuild builder; so does StringBuilder
CREATE FUNCTION cs_build(n int) RETURNS text LANGUAGE plxcsharp AS $$
string s = "";
for (int i = 0; i < n; i++) s += i;
var sb = new StringBuilder();
for (int i = 0; i < n; i++)
{
    if (i > 0) sb.Append(", ");
    sb.Append(i).Append("!");
}
sb.AppendLine();
sb.AppendFormat("[{0}]", sb.Length);
return s + "|" + s.Length + "|" + sb.ToString();
$$;
SELECT cs_build(12);
SELECT cs_src('cs_build');

-- ---------------------------------------------------------------- operators

CREATE FUNCTION cs_ops(a int, s text) RETURNS text LANGUAGE plxcsharp AS $$
string label = a > 0 ? "pos" : a < 0 ? "neg" : "zero";
string nn = s ?? "none";
bool b1 = a == 1 || a != 2 && !(a > 100);
bool sIsNull = s == null;
bool hasValue = s is not null;
int bits = (a & 6) | (a ^ 3) << 1;
bool inRange = (a > 0) & (a < 10);
int t = (int)3.9;
int t2 = (int)-3.9;
double d = (double)a / 4;
long big = (long)a * 1000000000L;
int code = (int)'A';
char c2 = (char)66;
int? maybe = null;
maybe ??= a;
int x = a;
x += 2; x -= 1; x *= 3; x /= 2; x %= 5; x <<= 2; x >>= 1; x |= 1; x &= 7; x ^= 2;
x++; ++x; x--;
return $"{label}|{nn}|{b1}|{sIsNull}|{hasValue}|{bits}|{inRange}|{t}|{t2}|{d}|{big}|{code}|{c2}|{maybe}|{x}|{-a}|{~a}";
$$;
SELECT cs_ops(1, NULL);
SELECT cs_ops(-7, 'hi');

-- switch expressions: constant patterns become a simple CASE; relational,
-- null and when patterns a searched CASE
SELECT cs_try($b$int a = 1; string s = a switch { var x when x > 0 => "pos", _ => "neg" }; return s;$b$);
CREATE FUNCTION cs_switchx(a int) RETURNS text LANGUAGE plxcsharp AS $$
string s1 = a switch { 1 => "one", 2 => "two", _ => "other" };
string s2 = a switch { < 0 => "negative", 0 => "zero", > 100 => "huge", _ => "normal" };
string s3 = a switch { null => "null", not null when a > 50 => "big", _ => "small" };
return s1 + " " + s2 + " " + s3;
$$;
SELECT cs_switchx(1), cs_switchx(-5), cs_switchx(200);

-- Math, Convert, Parse, MaxValue, DateTime and Guid members
CREATE FUNCTION cs_bcl(x double precision) RETURNS text LANGUAGE plxcsharp AS $$
var dt = DateTime.Parse("2024-03-15 10:20:30.75");
Guid g = Guid.NewGuid();
return $"{Math.Abs(-x)} {Math.Max(3, 7)} {Math.Min(3, 7)} {Math.Floor(x)} {Math.Ceiling(x)} " +
       $"{Math.Round(x)} {Math.Round(2.345, 2)} {Math.Sqrt(16)} {Math.Pow(2, 10)} {Math.Sign(-x)} " +
       $"{Math.Truncate(-x)} {Math.Round(Math.PI, 4)} {Convert.ToInt32("42") + 1} {int.Parse("7") * 2} " +
       $"{double.Parse("1.5")} {int.MaxValue} {long.MinValue} {dt.Year} {dt.Month} {dt.Day} " +
       $"{dt.Hour} {dt.Minute} {dt.Second} {dt.AddDays(20).Month} {dt.AddHours(15).Day} {dt.Date} " +
       $"{g.ToString().Length} {Guid.Empty} {char.IsDigit('5')} {char.ToUpper('q')}";
$$;
SELECT cs_bcl(2.5);

-- ---------------------------------------------------------------- loops

-- a plain counting for loop becomes a plpgsql integer FOR (ascending,
-- descending, with a step); any other for loop keeps C# semantics, so
-- continue still runs the iterators
CREATE FUNCTION cs_for(n int) RETURNS text LANGUAGE plxcsharp AS $$
long up = 0, down = 0, step = 0, gen = 0;
for (int i = 1; i <= n; i++) up += i;
for (int i = n; i > 0; i--) down = down * 10 + i;
for (int i = 0; i < n; i += 2) step += i;
for (int i = 0, j = n; i < j; i++, j--)
{
    if (i == 1) continue;
    gen += i * 10 + j;
}
return $"{up} {down} {step} {gen}";
$$;
SELECT cs_for(5), cs_for(0);
SELECT cs_src('cs_for');

-- the bound changes inside the body: the general form re-tests it, as C# does
CREATE FUNCTION cs_for_bound(n int) RETURNS int LANGUAGE plxcsharp AS $$
int s = 0;
int m = n;
for (int i = 0; i < m; i++) { s += i; if (i == 2) m = 10; }
return s;
$$;
SELECT cs_for_bound(5);

-- while, do/while (runs at least once; continue reaches the test), break,
-- and an infinite for (;;)
CREATE FUNCTION cs_loops(n int) RETURNS text LANGUAGE plxcsharp AS $$
int w = 0, i = 0;
while (i < n) { i++; if (i == 3) continue; if (i > 6) break; w += i; }
int d = 0, k = 0;
do { k++; if (k % 2 == 0) continue; d += k; } while (k < n);
int once = 0;
do once++; while (false);
int f = 0;
for (;;) { f++; if (f >= 4) break; }
return $"{w} {d} {once} {f}";
$$;
SELECT cs_loops(5), cs_loops(10), cs_loops(0);
SELECT cs_src('cs_loops');

-- foreach over an array (element type from the array), over a string's
-- characters, and with an explicit element type
CREATE FUNCTION cs_foreach(a int[], s text) RETURNS text LANGUAGE plxcsharp AS $$
long total = 0;
foreach (var v in a) total += v;
int vowels = 0;
foreach (char c in s) { if ("aeiou".Contains(c)) vowels++; }
string joined = "";
foreach (string w in new[] { "x", "y", "z" }) joined += w;
int[] b = [10, 20, 30];
int[] e = [];
int[] filled = new int[3];
return $"{total} {vowels} {joined} {a[0]} {b[1]} {b.Length} {b.Contains(20)} {e.Length} {filled[2]}";
$$;
SELECT cs_foreach(ARRAY[4, 5, 6], 'education');

-- switch: stacked labels, a nested break leaves the switch (not the loop),
-- a block section, default anywhere, no default does nothing
CREATE FUNCTION cs_switch(n int) RETURNS text LANGUAGE plxcsharp AS $$
string r = "";
switch (n)
{
    default:
        return "many";
    case 1:
        r = "one";
        break;
    case 2:
    case 3:
        if (n == 3) { r = "three"; break; }
        r = "two";
        break;
    case 4:
    {
        r = "four";
        break;
    }
}
int hits = 0;
for (int i = 0; i < 5; i++)
{
    switch (i)
    {
        case 1: case 3: hits++; break;
        case 4: continue;
    }
    hits += 10;
}
return r + " " + hits;
$$;
SELECT cs_switch(1), cs_switch(2), cs_switch(3), cs_switch(4), cs_switch(9);
SELECT cs_src('cs_switch');

-- ---------------------------------------------------------------- SQL access

CREATE TABLE cs_orders(id int PRIMARY KEY, grp int, amount bigint);
INSERT INTO cs_orders VALUES (1, 1, 10), (2, 1, 20), (3, 1, 30), (4, 2, 5);

-- foreach over query(): an interpolated string is a SQL template whose holes
-- are bound as values, never spliced as SQL text; typed fetch_one targets
CREATE FUNCTION cs_query(g int) RETURNS text LANGUAGE plxcsharp AS $$
long total = 0;
foreach (var row in query($"SELECT amount FROM cs_orders WHERE grp = {g} ORDER BY id"))
{
    total += row.amount;
}
foreach (long amt in query("SELECT amount FROM cs_orders WHERE grp = $1", g)) total += amt;
var first = fetch_one($"SELECT id, amount FROM cs_orders WHERE grp = {g} ORDER BY id LIMIT 1");
long cnt = fetch_one($"SELECT count(*) FROM cs_orders WHERE grp = {g}");
perform($"UPDATE cs_orders SET amount = amount WHERE grp = {g}");
int rc = row_count();
bool hit = found();
return $"{total} first={first.id} cnt={cnt} rc={rc} found={hit} col={first["amount"]}";
$$;
SELECT cs_query(1), cs_query(2), cs_query(3);
SELECT cs_src('cs_query');

-- perform of a SELECT built with an interpolated string runs through PERFORM
CREATE FUNCTION cs_perform(g int) RETURNS int LANGUAGE plxcsharp AS $$
perform($"SELECT count(*) FROM cs_orders WHERE grp = {g}");
return 1;
$$;
SELECT cs_perform(1);
SELECT cs_src('cs_perform');

-- a value interpolated into a query is bound, so it cannot inject SQL
CREATE FUNCTION cs_lookup(name text) RETURNS bigint LANGUAGE plxcsharp AS $$
long n = fetch_one($"SELECT count(*) FROM cs_orders WHERE grp::text = '{name}'");
return n;
$$;
SELECT cs_lookup('1'), cs_lookup($x$1' OR '1' = '1$x$) AS injection_blocked;

-- string literals and interpolated strings joined with + are one template,
-- so a statement split across lines still binds its values
CREATE FUNCTION cs_split(name text) RETURNS bigint LANGUAGE plxcsharp AS $$
long n = fetch_one("SELECT count(*) " +
                   "FROM cs_orders " +
                   $"WHERE grp::text = '{name}'");
return n;
$$;
SELECT cs_split('1'), cs_split($x$1' OR '1' = '1$x$) AS injection_blocked;
SELECT cs_src('cs_split');

-- fetch_one! raises when there is no row
CREATE FUNCTION cs_strict(i int) RETURNS bigint LANGUAGE plxcsharp AS $$
var r = fetch_one!($"SELECT amount FROM cs_orders WHERE id = {i}");
return r.amount;
$$;
SELECT cs_strict(2);
SELECT cs_strict(99);

-- dynamic SQL: execute with bind arguments, ident() for identifiers
CREATE FUNCTION cs_dyn(col text, g int) RETURNS bigint LANGUAGE plxcsharp AS $$
execute("UPDATE cs_orders SET amount = amount + $1 WHERE id = $2", 0, 1);
long r = fetch_one($"SELECT sum({ident(col)}) FROM cs_orders WHERE grp = {g}");
return r;
$$;
SELECT cs_dyn('amount', 1);
SELECT cs_src('cs_dyn');

-- cursors
CREATE FUNCTION cs_cursor() RETURNS bigint LANGUAGE plxcsharp AS $$
long total = 0;
var c = open_cursor("SELECT amount FROM cs_orders ORDER BY id");
var row = fetch_from(c);
while (found())
{
    total += row.amount;
    row = fetch_from(c);
}
close_cursor(c);
return total;
$$;
SELECT cs_cursor();

-- a raw interpolated string keeps a multi-line query readable
CREATE FUNCTION cs_rawsql(g int) RETURNS bigint LANGUAGE plxcsharp AS $$
long total = 0;
foreach (var r in query($"""
    SELECT amount
    FROM cs_orders
    WHERE grp = {g}
    """))
{
    total += r.amount;
}
return total;
$$;
SELECT cs_rawsql(1);

-- set-returning functions: yield return, yield break, return_query, and
-- RETURNS TABLE with return_next()
CREATE FUNCTION cs_squares(n int) RETURNS SETOF int LANGUAGE plxcsharp AS $$
for (int i = 1; i <= n; i++)
{
    if (i > 4) yield break;
    yield return i * i;
}
$$;
SELECT * FROM cs_squares(10);

CREATE FUNCTION cs_rq(g int) RETURNS SETOF cs_orders LANGUAGE plxcsharp AS $$
return_query($"SELECT * FROM cs_orders WHERE grp = {g} ORDER BY id");
$$;
SELECT * FROM cs_rq(1);

CREATE FUNCTION cs_table(g int) RETURNS TABLE(id int, label text) LANGUAGE plxcsharp AS $$
foreach (cs_orders o in query($"SELECT * FROM cs_orders WHERE grp = {g} ORDER BY id"))
{
    id = o.id;
    label = $"#{o.id}: {o.amount}";
    return_next();
}
$$;
SELECT * FROM cs_table(1);

-- procedures can commit; a DO block is C# too
CREATE TABLE cs_log(msg text);
CREATE PROCEDURE cs_proc(n int) LANGUAGE plxcsharp AS $$
for (int i = 1; i <= n; i++)
{
    execute("INSERT INTO cs_log VALUES ($1)", $"row {i}");
    if (i % 2 == 0) commit();
}
$$;
CALL cs_proc(3);
SELECT * FROM cs_log ORDER BY msg;

SET client_min_messages = notice;
DO LANGUAGE plxcsharp $$
for (int i = 1; i <= 2; i++) Console.WriteLine($"do block {i}");
Console.WriteLine("{0} + {1} = {2}", 1, 2, 1 + 2);
Console.WriteLine();
raise("warning", "raised at the warning level");
$$;
SET client_min_messages = warning;

-- ---------------------------------------------------------------- errors

-- catch classes map to conditions; Exception and PostgresException catch
-- everything; finally runs on every path
SET client_min_messages = notice;
CREATE FUNCTION cs_div(a int, b int) RETURNS text LANGUAGE plxcsharp AS $$
try
{
    return (a / b).ToString();
}
catch (DivideByZeroException e)
{
    Console.WriteLine($"caught: {e.Message} ({e.SqlState})");
    return "div0";
}
catch (Exception e)
{
    return "other: " + e.Message;
}
finally
{
    Console.WriteLine("finally ran");
}
$$;
SELECT cs_div(7, 2), cs_div(1, 0);
SET client_min_messages = warning;

-- when filters on e.SqlState (string codes or PostgresErrorCodes names),
-- and the error's diagnostic properties
CREATE FUNCTION cs_insert(i int) RETURNS text LANGUAGE plxcsharp AS $$
try
{
    execute("INSERT INTO cs_orders VALUES ($1, 9, 0)", i);
    return "inserted";
}
catch (PostgresException e) when (e.SqlState == PostgresErrorCodes.UniqueViolation)
{
    return $"duplicate: {e.ConstraintName} on {e.TableName}";
}
catch (PostgresException e) when (e.SqlState == "23502" || e.SqlState is "22003" or "22P02")
{
    return $"{e.SqlState}: {e.ColumnName}";
}
$$;
SELECT cs_insert(1), cs_insert(100), cs_insert(NULL);
SELECT cs_src('cs_insert');

-- throw: the class picks the SQLSTATE (P0001 unless mapped);
-- PostgresException takes Npgsql's (messageText, severity, invariantSeverity,
-- sqlState) and optional detail/hint; an inner exception becomes the detail
CREATE FUNCTION cs_throw(v int) RETURNS int LANGUAGE plxcsharp AS $$
if (v < 0) throw new ArgumentException($"negative: {v}");
if (v == 0) throw new DivideByZeroException();
if (v == 1) throw new PostgresException("custom", "ERROR", "ERROR", "P0042", detail: "the detail", hint: "the hint");
try { return 10 / (v - 2); }
catch (DivideByZeroException e) { throw new InvalidOperationException("wrapped", e); }
$$;
SELECT cs_throw(5);
\set VERBOSITY verbose
SELECT cs_throw(-1);
SELECT cs_throw(0);
SELECT cs_throw(1);
SELECT cs_throw(2);
\set VERBOSITY default

-- throw; rethrows the caught error unchanged
CREATE FUNCTION cs_rethrow(v int) RETURNS int LANGUAGE plxcsharp AS $$
try { return 1 / v; }
catch (Exception e) { throw; }
$$;
SELECT cs_rethrow(0);

-- a return inside try still runs finally; Debug.Assert / assert
CREATE TABLE cs_audit(msg text);
CREATE FUNCTION cs_fin(n int) RETURNS int LANGUAGE plxcsharp AS $$
try
{
    if (n > 0) return n * 2;
    Debug.Assert(n == 0, "n must not be negative");
    return 0;
}
finally
{
    execute("INSERT INTO cs_audit VALUES ($1)", $"fin {n}");
}
$$;
SELECT cs_fin(4), cs_fin(0);
SELECT cs_fin(-1);
SELECT * FROM cs_audit ORDER BY msg;

-- ---------------------------------------------------------------- triggers

CREATE TABLE cs_t(id int, name text, tag text, n int DEFAULT 0);
CREATE FUNCTION cs_trg() RETURNS trigger LANGUAGE plxcsharp AS $$
if (TG_OP == "INSERT") NEW.tag = $"new {NEW.name.ToUpper()} {TG_ARGV[0]}";
else NEW.tag = $"upd from {OLD.name}";
NEW["n"] += 1;
return NEW;
$$;
CREATE TRIGGER cs_trg BEFORE INSERT OR UPDATE ON cs_t
  FOR EACH ROW EXECUTE FUNCTION cs_trg('arg0');
INSERT INTO cs_t(id, name) VALUES (1, 'ann');
SELECT * FROM cs_t;
UPDATE cs_t SET name = 'bob';
SELECT * FROM cs_t;

-- ---------------------------------------------------------------- rejected

\set VERBOSITY terse
CREATE FUNCTION cs_bad1() RETURNS int LANGUAGE plxcsharp AS $$ var x = foo(1); return x; $$;
CREATE FUNCTION cs_bad2() RETURNS int LANGUAGE plxcsharp AS $$ List<int> x = null; return 1; $$;
CREATE FUNCTION cs_bad3() RETURNS int LANGUAGE plxcsharp AS $$ int x = 1; switch (x) { case 1: x = 2; case 2: x = 3; break; } return x; $$;
CREATE FUNCTION cs_bad4() RETURNS int LANGUAGE plxcsharp AS $$ goto done; $$;
CREATE FUNCTION cs_bad5() RETURNS int LANGUAGE plxcsharp AS $$ var f = (int a) => a + 1; return 1; $$;
CREATE FUNCTION cs_bad6() RETURNS int LANGUAGE plxcsharp AS $$ int x = 1; return x.Foo(); $$;
CREATE FUNCTION cs_bad7() RETURNS int LANGUAGE plxcsharp AS $$ int x = 1; int X = 2; return x; $$;
CREATE FUNCTION cs_bad8() RETURNS int LANGUAGE plxcsharp AS $$ throw; $$;
CREATE FUNCTION cs_bad9() RETURNS int LANGUAGE plxcsharp AS $$ try { return 1; } catch (KeyNotFoundException e) { return 2; } $$;
CREATE FUNCTION cs_bad10() RETURNS int LANGUAGE plxcsharp AS $$ int x = 1; return x++; $$;
CREATE FUNCTION cs_bad11() RETURNS int LANGUAGE plxcsharp AS $$ char c = 'a'; return c + 1; $$;
CREATE FUNCTION cs_bad12() RETURNS text LANGUAGE plxcsharp AS $$ return $"{1:N2}"; $$;
CREATE FUNCTION cs_bad13() RETURNS int LANGUAGE plxcsharp AS $$ var r = fetch_one("SELECT 1") + 1; return 1; $$;
CREATE FUNCTION cs_bad14() RETURNS SETOF int LANGUAGE plxcsharp AS $$ return 1; $$;
CREATE FUNCTION cs_bad15() RETURNS int LANGUAGE plxcsharp AS $$ yield return 1; $$;
CREATE FUNCTION cs_bad16() RETURNS int LANGUAGE plxcsharp AS $$ int user = 1; return user; $$;
CREATE FUNCTION cs_bad17() RETURNS int LANGUAGE plxcsharp AS $$ undeclared = 5; return 1; $$;
CREATE FUNCTION cs_bad18(n int) RETURNS int LANGUAGE plxcsharp AS $$ int N = 5; return N; $$;
CREATE FUNCTION cs_bad19() RETURNS int LANGUAGE plxcsharp AS $$ Compute(1); return 1; $$;
CREATE FUNCTION cs_bad20() RETURNS int LANGUAGE plxcsharp AS $$ int x = 1; while (x) { } return 1; $$;
CREATE FUNCTION cs_bad21() RETURNS int LANGUAGE plxcsharp AS $$ int x = 1; string x = "a"; return 1; $$;
CREATE FUNCTION cs_bad22() RETURNS int LANGUAGE plxcsharp AS $$ var x = null; return 1; $$;
CREATE FUNCTION cs_bad23() RETURNS int LANGUAGE plxcsharp AS $$ try { return 1; } catch (Exception e) { return e; } $$;
CREATE FUNCTION cs_bad24() RETURNS int LANGUAGE plxcsharp AS $$ object o = 1; return 1; $$;
CREATE FUNCTION cs_bad25() RETURNS int LANGUAGE plxcsharp AS $$ int x = 1; x = y = 2; return 1; $$;
CREATE FUNCTION cs_bad26() RETURNS int LANGUAGE plxcsharp AS $$ int x = 1; switch (x) { case > 0: return 1; } return 0; $$;
CREATE FUNCTION cs_bad27() RETURNS int LANGUAGE plxcsharp AS $$ int x = 1; switch (x) { case null: return 1; } return 0; $$;
CREATE FUNCTION cs_bad28() RETURNS int LANGUAGE plxcsharp AS $$ public static int F() { return 1; } $$;
CREATE FUNCTION cs_bad29() RETURNS int LANGUAGE plxcsharp AS $$ int x = int.TryParse("1", out var y); return 1; $$;
CREATE FUNCTION cs_bad30() RETURNS int LANGUAGE plxcsharp AS $$ try { return 1; } catch (Exception e) when (e.Message.Contains("x")) { return 2; } $$;
CREATE FUNCTION cs_bad32() RETURNS int LANGUAGE plxcsharp AS $$ int a = 1; const int B = a + 1; return B; $$;
CREATE FUNCTION cs_bad33() RETURNS int LANGUAGE plxcsharp AS $$ int[] a = {1}; a[random_index()] += 1; return 1; $$;
CREATE FUNCTION cs_bad34() RETURNS int LANGUAGE plxcsharp AS $$ return "abc" switch { null => 0, _ => 1 }; $$;
\set VERBOSITY default
-- line numbers point at the C# source, also after a multi-line string
CREATE FUNCTION cs_bad31() RETURNS int LANGUAGE plxcsharp AS $$
string sql = @"SELECT 1
FROM t";
int a = 1;
a.Normalize();
$$;

-- ---------------------------------------------------------------- hostile input

SELECT cs_try('return "unterminated');
SELECT cs_try('return @"unterminated');
SELECT cs_try('return """' || chr(10) || 'no closing line');
SELECT cs_try('return $"{unterminated hole');
SELECT cs_try('/* unterminated comment');
SELECT cs_try('return "\u0001";');
SELECT cs_try('return "\uD800";');
SELECT cs_try('return 0x' || repeat('F', 40) || ';');
SELECT cs_try('#region x' || chr(10) || 'return 1;');
SELECT cs_try(repeat('if (true) { ', 2000) || 'return "x";' || repeat(' }', 2000));
SELECT cs_try('return ' || repeat('(', 2000) || '1' || repeat(')', 2000) || ';');
SELECT cs_try('return ' || repeat('$"{', 600) || '1' || repeat('}"', 600) || ';');
SELECT cs_try('return "a"' || repeat('.PadLeft(1)', 40) || ';');
SELECT cs_try(repeat('else ', 50));
SELECT cs_try(repeat('}', 50));
SELECT cs_try('return ' || repeat('-', 3000) || '1;');
-- long flat input transpiles in linear time
SELECT cs_try('return (' || repeat('1 + ', 20000) || '1).ToString();');
SELECT cs_try('string s = "";' || repeat(' s += "x";', 20000) || ' return s;');
SELECT cs_try('return ' || repeat('a ? b : ', 3000) || 'c;');
-- long dotted names and deep nesting produce output linear in the input
SELECT cs_try('int y = 0; y = q' || repeat('.a', 40000) || '; return 1;');
SELECT cs_try('int y = 0; ' || repeat('try { ', 400) || 'if (y == 0) return "x";' || repeat(' y = 1;', 2000) ||
              repeat(' } finally { y = 2; }', 400) || ' return "y";');
SELECT length(prosrc) < 2000000 AS linear FROM pg_proc WHERE proname = 'cs_probe';

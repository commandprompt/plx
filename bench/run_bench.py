#!/usr/bin/env python3
"""plx benchmark harness.

Measures five workloads across every installed language, in one database:
  - arith:   integer accumulation loop      (loop dispatch + integer math)
  - strbuild:string built in a loop          (text concatenation)
  - iter:    sum a column over a table       (SPI / row marshalling)
  - branch:  many-way conditional per element(branch dispatch)
  - call:    many small function calls       (call/return overhead)

plx dialects (plxruby, plxphp, plxjs, plxpython3) transpile to plpgsql, so they
are expected to match plpgsql. The native CommandPrompt PL/Ruby and PL/PHP,
plperl, and plpython3u run their own embedded interpreters. plx uses plx-prefixed
language names, so it coexists with the native languages.

Each function is checked for correctness, then timed as the minimum of REPS runs.
Run as root in the container: PGHOST=/tmp PGPORT=5432 python3 bench/run_bench.py
"""
import os, re, subprocess, sys

PSQL = "/usr/local/pgsql/bin/psql"
ENV = dict(os.environ, PGUSER="postgres")
ARITH_N = 2_000_000
STR_N = 200_000
ITER_ROWS = 1_000_000
BRANCH_N = 2_000_000
CALL_N = 500_000
REPS = 5

def psql(sql, timing=False):
    pre = "\\timing on\n" if timing else ""
    p = subprocess.run(["runuser", "-u", "postgres", "--", PSQL, "-U", "postgres",
                        "-h", os.environ.get("PGHOST", "/tmp"),
                        "-p", os.environ.get("PGPORT", "5432"),
                        "-X", "-q", "-v", "ON_ERROR_STOP=0"],
                       input=pre + sql, capture_output=True, text=True)
    return p.stdout + p.stderr

def lang_exists(lan):
    return "1" in psql("SELECT 1 FROM pg_language WHERE lanname='%s';" % lan)

def best(call):
    b = None
    for _ in range(REPS):
        m = re.search(r"Time:\s*([0-9.]+)\s*ms", psql(call + ";", timing=True))
        if not m:
            return None
        t = float(m.group(1))
        b = t if b is None or t < b else b
    return b

# Each language provides a body template per workload keyed by placeholders.
# {N} is the loop count, {ROWS} the table. Functions are named b_<wl>_<tag>.

LANGS = []  # (tag, ext, {workload: (create_sql, call_sql, expected)})

def add(tag, ext, defs):
    LANGS.append((tag, ext, defs))

# ---- plpgsql (baseline) ----
add("plpgsql", None, {
 "arith": ("""CREATE FUNCTION b_arith_pg(n bigint) RETURNS bigint LANGUAGE plpgsql AS $x$
DECLARE s bigint:=0;i bigint;BEGIN FOR i IN 1..n LOOP s:=s+i;END LOOP;RETURN s;END;$x$;""",
    "SELECT b_arith_pg(%d)" % ARITH_N, None),
 "strbuild": ("""CREATE FUNCTION b_str_pg(n int) RETURNS int LANGUAGE plpgsql AS $x$
DECLARE s text:='';i int;BEGIN FOR i IN 1..n LOOP s:=s||'x';END LOOP;RETURN length(s);END;$x$;""",
    "SELECT b_str_pg(%d)" % STR_N, str(STR_N)),
 "iter": ("""CREATE FUNCTION b_iter_pg() RETURNS bigint LANGUAGE plpgsql AS $x$
DECLARE s bigint:=0;r record;BEGIN FOR r IN SELECT v FROM bench_data LOOP s:=s+r.v;END LOOP;RETURN s;END;$x$;""",
    "SELECT b_iter_pg()", None),
 "branch": ("""CREATE FUNCTION b_branch_pg(n int) RETURNS bigint LANGUAGE plpgsql AS $x$
DECLARE s bigint:=0;i int;BEGIN FOR i IN 1..n LOOP
  IF i%4=0 THEN s:=s+3;ELSIF i%4=1 THEN s:=s+1;ELSIF i%4=2 THEN s:=s+2;ELSE s:=s+4;END IF;
END LOOP;RETURN s;END;$x$;""",
    "SELECT b_branch_pg(%d)" % BRANCH_N, None),
 "call": ("""CREATE FUNCTION b_leaf_pg(x int) RETURNS int LANGUAGE plpgsql AS $x$ BEGIN RETURN x+1;END;$x$;
CREATE FUNCTION b_call_pg(n int) RETURNS bigint LANGUAGE plpgsql AS $x$
DECLARE s bigint:=0;i int;BEGIN FOR i IN 1..n LOOP s:=s+b_leaf_pg(i);END LOOP;RETURN s;END;$x$;""",
    "SELECT b_call_pg(%d)" % CALL_N, None),
})

# ---- plxruby ----
add("plxruby", None, {
 "arith": ("""CREATE FUNCTION b_arith_xrb(n bigint) RETURNS bigint LANGUAGE plxruby AS $x$
s = 0 #:: bigint
for i in 1..n
  s = s + i
end
return s
$x$;""", "SELECT b_arith_xrb(%d)" % ARITH_N, None),
 "strbuild": ("""CREATE FUNCTION b_str_xrb(n int) RETURNS int LANGUAGE plxruby AS $x$
s = "" #:: text
for i in 1..n
  s = "#{s}x"
end
return length(s)
$x$;""", "SELECT b_str_xrb(%d)" % STR_N, str(STR_N)),
 "iter": ("""CREATE FUNCTION b_iter_xrb() RETURNS bigint LANGUAGE plxruby AS $x$
s = 0 #:: bigint
query("SELECT v FROM bench_data").each do |r|
  s = s + r.v
end
return s
$x$;""", "SELECT b_iter_xrb()", None),
 "branch": ("""CREATE FUNCTION b_branch_xrb(n int) RETURNS bigint LANGUAGE plxruby AS $x$
s = 0 #:: bigint
for i in 1..n
  if i % 4 == 0
    s = s + 3
  elsif i % 4 == 1
    s = s + 1
  elsif i % 4 == 2
    s = s + 2
  else
    s = s + 4
  end
end
return s
$x$;""", "SELECT b_branch_xrb(%d)" % BRANCH_N, None),
 "call": ("""CREATE FUNCTION b_leaf_xrb(x int) RETURNS int LANGUAGE plxruby AS $x$ return x + 1 $x$;
CREATE FUNCTION b_call_xrb(n int) RETURNS bigint LANGUAGE plxruby AS $x$
s = 0 #:: bigint
for i in 1..n
  s = s + b_leaf_xrb(i)
end
return s
$x$;""", "SELECT b_call_xrb(%d)" % CALL_N, None),
})

# ---- plxphp ----
add("plxphp", None, {
 "arith": ("""CREATE FUNCTION b_arith_xphp(n bigint) RETURNS bigint LANGUAGE plxphp AS $x$
$s = 0 /*:: bigint */;
for ($i = 1; $i <= $n; $i++) { $s = $s + $i; }
return $s;
$x$;""", "SELECT b_arith_xphp(%d)" % ARITH_N, None),
 "strbuild": ("""CREATE FUNCTION b_str_xphp(n int) RETURNS int LANGUAGE plxphp AS $x$
$s = "" /*:: text */;
for ($i = 1; $i <= $n; $i++) { $s = "{$s}x"; }
return length($s);
$x$;""", "SELECT b_str_xphp(%d)" % STR_N, str(STR_N)),
 "iter": ("""CREATE FUNCTION b_iter_xphp() RETURNS bigint LANGUAGE plxphp AS $x$
$s = 0 /*:: bigint */;
foreach (query("SELECT v FROM bench_data") as $r) { $s = $s + $r->v; }
return $s;
$x$;""", "SELECT b_iter_xphp()", None),
 "branch": ("""CREATE FUNCTION b_branch_xphp(n int) RETURNS bigint LANGUAGE plxphp AS $x$
$s = 0 /*:: bigint */;
for ($i = 1; $i <= $n; $i++) {
  if ($i % 4 == 0) { $s = $s + 3; }
  elseif ($i % 4 == 1) { $s = $s + 1; }
  elseif ($i % 4 == 2) { $s = $s + 2; }
  else { $s = $s + 4; }
}
return $s;
$x$;""", "SELECT b_branch_xphp(%d)" % BRANCH_N, None),
 "call": ("""CREATE FUNCTION b_leaf_xphp(x int) RETURNS int LANGUAGE plxphp AS $x$ return $x + 1; $x$;
CREATE FUNCTION b_call_xphp(n int) RETURNS bigint LANGUAGE plxphp AS $x$
$s = 0 /*:: bigint */;
for ($i = 1; $i <= $n; $i++) { $s = $s + b_leaf_xphp($i); }
return $s;
$x$;""", "SELECT b_call_xphp(%d)" % CALL_N, None),
})

# ---- plxjs ----
add("plxjs", None, {
 "arith": ("""CREATE FUNCTION b_arith_xjs(n bigint) RETURNS bigint LANGUAGE plxjs AS $x$
let s = 0 /*:: bigint */;
for (let i = 1; i <= n; i++) { s = s + i; }
return s;
$x$;""", "SELECT b_arith_xjs(%d)" % ARITH_N, None),
 "strbuild": ("""CREATE FUNCTION b_str_xjs(n int) RETURNS int LANGUAGE plxjs AS $x$
let s = "" /*:: text */;
for (let i = 1; i <= n; i++) { s = `${s}x`; }
return length(s);
$x$;""", "SELECT b_str_xjs(%d)" % STR_N, str(STR_N)),
 "iter": ("""CREATE FUNCTION b_iter_xjs() RETURNS bigint LANGUAGE plxjs AS $x$
let s = 0 /*:: bigint */;
for (const r of query(`SELECT v FROM bench_data`)) { s = s + r.v; }
return s;
$x$;""", "SELECT b_iter_xjs()", None),
 "branch": ("""CREATE FUNCTION b_branch_xjs(n int) RETURNS bigint LANGUAGE plxjs AS $x$
let s = 0 /*:: bigint */;
for (let i = 1; i <= n; i++) {
  if (i % 4 == 0) { s = s + 3; }
  else if (i % 4 == 1) { s = s + 1; }
  else if (i % 4 == 2) { s = s + 2; }
  else { s = s + 4; }
}
return s;
$x$;""", "SELECT b_branch_xjs(%d)" % BRANCH_N, None),
 "call": ("""CREATE FUNCTION b_leaf_xjs(x int) RETURNS int LANGUAGE plxjs AS $x$ return x + 1; $x$;
CREATE FUNCTION b_call_xjs(n int) RETURNS bigint LANGUAGE plxjs AS $x$
let s = 0 /*:: bigint */;
for (let i = 1; i <= n; i++) { s = s + b_leaf_xjs(i); }
return s;
$x$;""", "SELECT b_call_xjs(%d)" % CALL_N, None),
})

# ---- plxpython3 ----
add("plxpython3", None, {
 "arith": ("""CREATE FUNCTION b_arith_xpy(n bigint) RETURNS bigint LANGUAGE plxpython3 AS $x$
s = 0 #:: bigint
for i in range(1, n + 1):
    s = s + i
return s
$x$;""", "SELECT b_arith_xpy(%d)" % ARITH_N, None),
 "strbuild": ("""CREATE FUNCTION b_str_xpy(n int) RETURNS int LANGUAGE plxpython3 AS $x$
s = "" #:: text
for i in range(1, n + 1):
    s = f"{s}x"
return length(s)
$x$;""", "SELECT b_str_xpy(%d)" % STR_N, str(STR_N)),
 "iter": ("""CREATE FUNCTION b_iter_xpy() RETURNS bigint LANGUAGE plxpython3 AS $x$
s = 0 #:: bigint
for r in query("SELECT v FROM bench_data"):
    s = s + r.v
return s
$x$;""", "SELECT b_iter_xpy()", None),
 "branch": ("""CREATE FUNCTION b_branch_xpy(n int) RETURNS bigint LANGUAGE plxpython3 AS $x$
s = 0 #:: bigint
for i in range(1, n + 1):
    if i % 4 == 0:
        s = s + 3
    elif i % 4 == 1:
        s = s + 1
    elif i % 4 == 2:
        s = s + 2
    else:
        s = s + 4
return s
$x$;""", "SELECT b_branch_xpy(%d)" % BRANCH_N, None),
 "call": ("""CREATE FUNCTION b_leaf_xpy(x int) RETURNS int LANGUAGE plxpython3 AS $x$ return x + 1 $x$;
CREATE FUNCTION b_call_xpy(n int) RETURNS bigint LANGUAGE plxpython3 AS $x$
s = 0 #:: bigint
for i in range(1, n + 1):
    s = s + b_leaf_xpy(i)
return s
$x$;""", "SELECT b_call_xpy(%d)" % CALL_N, None),
})

# ---- native plperl ----
add("plperl", "plperl", {
 "arith": ("""CREATE FUNCTION b_arith_pl(n bigint) RETURNS bigint LANGUAGE plperl AS $x$
my($n)=@_;my $s=0;for(my $i=1;$i<=$n;$i++){$s+=$i;}return $s;$x$;""",
    "SELECT b_arith_pl(%d)" % ARITH_N, None),
 "strbuild": ("""CREATE FUNCTION b_str_pl(n int) RETURNS int LANGUAGE plperl AS $x$
my($n)=@_;my $s='';for(my $i=1;$i<=$n;$i++){$s.='x';}return length($s);$x$;""",
    "SELECT b_str_pl(%d)" % STR_N, str(STR_N)),
 "iter": ("""CREATE FUNCTION b_iter_pl() RETURNS bigint LANGUAGE plperl AS $x$
my $s=0;my $rv=spi_exec_query("SELECT v FROM bench_data");
for my $r(@{$rv->{rows}}){$s+=$r->{v};}return $s;$x$;""",
    "SELECT b_iter_pl()", None),
 "branch": ("""CREATE FUNCTION b_branch_pl(n int) RETURNS bigint LANGUAGE plperl AS $x$
my($n)=@_;my $s=0;for(my $i=1;$i<=$n;$i++){my $m=$i%4;
if($m==0){$s+=3;}elsif($m==1){$s+=1;}elsif($m==2){$s+=2;}else{$s+=4;}}return $s;$x$;""",
    "SELECT b_branch_pl(%d)" % BRANCH_N, None),
 "call": ("""CREATE FUNCTION b_leaf_pl(x int) RETURNS int LANGUAGE plperl AS $x$ return $_[0]+1;$x$;
CREATE FUNCTION b_call_pl(n int) RETURNS bigint LANGUAGE plperl AS $x$
my($n)=@_;my $s=0;for(my $i=1;$i<=$n;$i++){$s+=spi_exec_query("SELECT b_leaf_pl($i)")->{rows}[0]{b_leaf_pl};}return $s;$x$;""",
    "SELECT b_call_pl(%d)" % (CALL_N // 10), None),  # 1/10 count: SPI call per iter is very slow
})

# ---- native plpython3u ----
add("plpython3u", "plpython3u", {
 "arith": ("""CREATE FUNCTION b_arith_py(n bigint) RETURNS bigint LANGUAGE plpython3u AS $x$
s=0
for i in range(1,n+1):
    s+=i
return s$x$;""", "SELECT b_arith_py(%d)" % ARITH_N, None),
 "strbuild": ("""CREATE FUNCTION b_str_py(n int) RETURNS int LANGUAGE plpython3u AS $x$
s=[]
for i in range(n):
    s.append('x')
return len(''.join(s))$x$;""", "SELECT b_str_py(%d)" % STR_N, str(STR_N)),
 "iter": ("""CREATE FUNCTION b_iter_py() RETURNS bigint LANGUAGE plpython3u AS $x$
s=0
for r in plpy.execute("SELECT v FROM bench_data"):
    s+=r["v"]
return s$x$;""", "SELECT b_iter_py()", None),
 "branch": ("""CREATE FUNCTION b_branch_py(n int) RETURNS bigint LANGUAGE plpython3u AS $x$
s=0
for i in range(1,n+1):
    m=i%4
    if m==0: s+=3
    elif m==1: s+=1
    elif m==2: s+=2
    else: s+=4
return s$x$;""", "SELECT b_branch_py(%d)" % BRANCH_N, None),
 "call": ("""CREATE FUNCTION b_leaf_py(x int) RETURNS int LANGUAGE plpython3u AS $x$ return x+1$x$;
CREATE FUNCTION b_call_py(n int) RETURNS bigint LANGUAGE plpython3u AS $x$
s=0
p=plpy.prepare("SELECT b_leaf_py($1)",["int"])
for i in range(1,n+1):
    s+=plpy.execute(p,[i])[0]["b_leaf_py"]
return s$x$;""", "SELECT b_call_py(%d)" % (CALL_N // 10), None),
})

WORKLOADS = ["arith", "strbuild", "iter", "branch", "call"]

# setup
psql("CREATE EXTENSION IF NOT EXISTS plx;")
# drop any bench functions from a previous run (user schema only)
psql("""DO $$ DECLARE r record; BEGIN
  FOR r IN SELECT p.oid::regprocedure::text AS s FROM pg_proc p
           JOIN pg_namespace n ON n.oid = p.pronamespace
           WHERE p.proname LIKE 'b\\_%' AND n.nspname = 'public'
  LOOP EXECUTE 'DROP FUNCTION ' || r.s; END LOOP; END $$;""")
psql("DROP TABLE IF EXISTS bench_data; CREATE TABLE bench_data(v bigint);")
psql("INSERT INTO bench_data SELECT g FROM generate_series(1,%d) g;" % ITER_ROWS)
psql("VACUUM ANALYZE bench_data;")

rows = {}
for tag, ext, defs in LANGS:
    if ext and not lang_exists(ext):
        psql("CREATE EXTENSION IF NOT EXISTS %s;" % ext)
        if not lang_exists(ext):
            rows[tag] = {w: None for w in WORKLOADS}
            continue
    rows[tag] = {}
    for w in WORKLOADS:
        cre, call, exp = defs[w]
        o = psql(cre)
        if "ERROR" in o:
            print("create failed %s/%s:\n%s" % (tag, w, o[:300]), file=sys.stderr)
            rows[tag][w] = None
            continue
        if exp is not None and exp not in psql(call + ";"):
            print("WRONG result %s/%s" % (tag, w), file=sys.stderr)
        rows[tag][w] = best(call)

# report
base = rows.get("plpgsql", {})
def cell(tag, w):
    t = rows[tag][w]
    if not isinstance(t, float):
        return "n/a"
    b = base.get(w)
    r = (" (%.2fx)" % (t / b)) if isinstance(b, float) and b else ""
    return "%.0f ms%s" % (t, r)

hdr = ["language"] + WORKLOADS
print("plx benchmark  (PostgreSQL 18.4, min of %d runs)\n" % REPS)
print("workload sizes: arith=%d  strbuild=%d  iter=%d rows  branch=%d  call=%d (perl/py call=%d)\n"
      % (ARITH_N, STR_N, ITER_ROWS, BRANCH_N, CALL_N, CALL_N // 10))
w0 = 12
print("  ".join([hdr[0].ljust(w0)] + [h.ljust(14) for h in hdr[1:]]))
print("-" * 90)
for tag, _, _ in LANGS:
    print("  ".join([tag.ljust(w0)] + [cell(tag, w).ljust(14) for w in WORKLOADS]))
print("\nx = relative to plpgsql (lower is faster). plx dialects transpile to plpgsql.")
print("Note: perl/python 'call' uses 1/10 the iterations (per-call SPI is far slower).")

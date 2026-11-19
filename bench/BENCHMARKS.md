# plx Benchmarks

## Method

Five workloads were measured on PostgreSQL 18.4 in the development container
(8 vCPU, 8 GiB, Ubuntu 26.04):

- arith: accumulate the integers 1 to 2,000,000 in a loop (loop dispatch and
  integer arithmetic).
- strbuild: build a 200,000-character string one character at a time in a loop
  (text handling with the natural in-language idiom for each language). plpgsql
  uses `s := s || 'x'`. The plx dialects use their append operator (`s << 'x'`,
  `$s .= 'x'`, `s += 'x'`, and `STRING-APPEND 'x' TO S` in plxcobol), which plx
  lowers to the plx_strbuild builder.
- iter: sum a `bigint` column over a 1,000,000-row table (SPI and per-row
  marshalling).
- branch: a four-way conditional per element over 2,000,000 elements (branch
  dispatch).
- call: 500,000 calls to a small function (call and return overhead). For the
  embedded PLs (plperl, plpython3u, and native plruby and plphp) this uses 50,000
  iterations, because each call goes through SPI and is far slower; those cells
  are scaled to the smaller count.

Each function is written idiomatically in each language, checked for a correct
result, and called five times; the minimum wall-clock time (`psql \timing`) is
reported. The harness is `bench/run_bench.py`. All languages run in one database;
plx uses plx-prefixed language names, so it coexists with the native plruby,
plphp, plperl, and plpython3u.

plxruby, plxphp, plxjs, plxpython3, and plxcobol transpile to plpgsql at
`CREATE FUNCTION` time, so at run time they execute as plpgsql. plxts, plxgo,
plxplsql, and plxtsql were not benchmarked; they also execute as plpgsql. plperl and
plpython3u run their own embedded interpreters and retrieve rows through SPI.

## Results

Times are milliseconds; the multiplier is relative to plpgsql (lower is faster).

| language   | arith        | strbuild     | iter          | branch        | call          |
|------------|--------------|--------------|---------------|---------------|---------------|
| plpgsql    | 56 (1.00x)   | 1743 (1.00x) | 96 (1.00x)    | 161 (1.00x)   | 164 (1.00x)   |
| plxruby    | 56 (1.00x)   | 10 (0.01x)   | 112 (1.17x)   | 163 (1.01x)   | 157 (0.96x)   |
| plxphp     | 54 (0.97x)   | 10 (0.01x)   | 101 (1.06x)   | 174 (1.08x)   | 169 (1.03x)   |
| plxjs      | 58 (1.03x)   | 10 (0.01x)   | 105 (1.09x)   | 162 (1.00x)   | 169 (1.03x)   |
| plxpython3 | 57 (1.02x)   | 10 (0.01x)   | 103 (1.07x)   | 167 (1.03x)   | 159 (0.97x)   |
| plxcobol   | 55 (0.97x)   | 10 (0.01x)   | 104 (1.09x)   | 214 (1.32x)   | 170 (1.04x)   |
| plperl     | 44 (0.79x)   | 12 (0.01x)   | 539 (5.61x)   | 153 (0.95x)   | 300 (1.83x)   |
| plpython3u | 73 (1.31x)   | 17 (0.01x)   | 350 (3.64x)   | 109 (0.68x)   | 191 (1.16x)   |
| plruby     | 100 (1.79x)  | 53 (0.03x)   | 385 (4.00x)   | 145 (0.90x)   | 497 (3.03x)   |
| plphp      | 51 (0.90x)   | 15 (0.01x)   | 259 (2.69x)   | 64 (0.40x)    | 282 (1.72x)   |

The last two rows, plruby and plphp, are the third-party native PL/Ruby and
PL/PHP (see the build notes below); the plx dialects with the same names but a
`plx` prefix are the transpile-to-plpgsql implementations.

In the strbuild column, native plpgsql is the `s := s || 'x'` baseline and the
plx dialects use the builder. This is the intended native-versus-plx comparison:
the same accumulation idiom is 1743 ms in stock plpgsql and about 10 ms in the
plx dialects. The plx builder is also faster than the native PLs' own in-language
append on this workload (native plruby 53 ms, native plphp 15 ms), because it
mutates one expanded-object buffer in place rather than materializing an
interpreter string per step. These numbers are on PostgreSQL 18; the builder's
amortized-O(1) append requires PostgreSQL 18 (see the version note under String
building). On PostgreSQL 13 to 17 the plx strbuild column would match plpgsql
(both O(n^2)).

## Analysis

- The plx dialects match plpgsql within about 10 percent on the arith and call
  workloads, and within 17 percent on iter, because the stored function body is
  plpgsql. There is no run-time translation cost; the translation happens once at `CREATE FUNCTION`.
  The small spread among the dialects is measurement noise.

- plxcobol (branch): the one place plxcobol trails is the four-way branch
  (1.32x). It is idiomatic in COBOL to write the branch as `EVALUATE`, which
  lowers to a plpgsql `CASE` statement, whereas the other dialects and the
  plpgsql baseline use an `IF`/`ELSIF` chain. The `CASE` form is modestly slower
  here; on the other workloads plxcobol matches plpgsql (its `PERFORM VARYING`
  counting loop lowers to an integer `FOR`, and `STRING-APPEND` lowers to the
  string builder).

- Row iteration (iter): plpgsql, and therefore the plx dialects, are 2.7x to 5.6x
  faster than the embedded PLs (plperl, plpython3u, native plruby, native plphp).
  plpgsql streams rows through a cursor and reads columns directly, while the
  embedded interpreters copy each row into their own data structures.

- String building (strbuild): concatenating onto a text variable in a loop
  (`s := s || 'x'`) is quadratic in plpgsql, because text is immutable and each
  step rebuilds the whole string. This is the one workload where plx does not
  simply match plpgsql: plx ships an expanded-object string builder
  (`plx_strbuild`, see `../doc/` and `src/plx_strbuild.c`) whose append is
  amortized O(1), and the transpiler lowers the dialect append operators
  (`<<`, `.=`, `+=` on a string) onto it. The result is about 170x faster than
  the stock plpgsql idiom, and faster than the native PLs building the same
  string with their own append (native plruby 53 ms, native plphp 15 ms), which
  each materialize a fresh interpreter string per step. In hand-written plpgsql
  the same win is available by calling `plx_sb_append`, or by assembling text in
  SQL with `string_agg` over a set.

- Arithmetic (arith): the embedded interpreters vary. plperl is fastest (0.79x,
  native integer arithmetic), native plruby is slowest (1.79x) followed by
  plpython3u (1.31x), and plpgsql with the plx dialects sits between.

- Branching (branch): on this pure-CPU integer workload the embedded PLs are
  faster: native plphp (0.40x), plpython3u (0.68x), native plruby (0.90x) and
  plperl (0.95x). plpgsql and the plx dialects are at about 1.0x (plxcobol's
  `EVALUATE` at 1.32x).

- Call overhead (call): plpgsql and the plx dialects are fastest. The embedded
  PLs pay an SPI round trip per call and are slower even at one tenth the
  iteration count (native plruby is the slowest here, at 3.03x).

The transpile-to-plpgsql approach gives the plx dialects the performance profile
of plpgsql: strong on set-oriented and SQL-bound work, competitive on procedural
arithmetic and branching, and with no additional language runtime loaded into the
backend. In-loop string building, the one case where plpgsql is weak, is handled
by the string builder, which brings it on par with the embedded interpreters on
PostgreSQL 18 (on PostgreSQL 13 to 17 the builder is correct but not accelerated,
so that case remains O(n^2)).

## String builder: PostgreSQL 18 versus 17

The builder's in-place append relies on `SupportRequestModifyInPlace`, a planner
support-request kind added in PostgreSQL 18. On PostgreSQL 13 to 17 that request
does not exist, so each append flattens and copies the buffer and the total build
is O(n^2), the same shape as the plpgsql `s := s || 'x'` idiom. The result is
still correct; only the acceleration is absent. Building a string one character
at a time (min of 3 runs, same container):

PostgreSQL 17.10 (the builder tracks plpgsql, both O(n^2)):

| n       | plpgsql `\|\|` | plx builder |
|---------|--------------|-------------|
| 25,000  | 7.6 ms       | 8.8 ms      |
| 50,000  | 41.1 ms      | 55.8 ms     |
| 100,000 | 218.6 ms     | 318.9 ms    |
| 200,000 | 1082.2 ms    | 1040.8 ms   |

PostgreSQL 18.4 (the builder is linear, plpgsql stays quadratic):

| n       | plpgsql `\|\|` | plx builder |
|---------|--------------|-------------|
| 25,000  | 14.8 ms      | 1.0 ms      |
| 50,000  | 65.3 ms      | 2.1 ms      |
| 100,000 | 307.7 ms     | 4.2 ms      |
| 200,000 | 1464.6 ms    | 8.3 ms      |

On PostgreSQL 18 the builder's time doubles as n doubles (1.0, 2.1, 4.2, 8.3 ms),
confirming O(n); the plpgsql baseline roughly quadruples. On PostgreSQL 17 both
columns roughly quadruple per doubling of n, confirming that the builder is not
accelerated there. The full regression suite (including the string-builder
correctness tests) passes on both versions.

## Native PL/Ruby and PL/PHP build notes

The comparison against the third-party native PL/Ruby and PL/PHP is documented in
the build scripts. Both are the current CommandPrompt versions
(https://github.com/commandprompt/plruby, https://github.com/commandprompt/PL-php;
plruby 2.5, plphp 2.6). They build against PostgreSQL 18 with:

- PL/Ruby: `make PG_CONFIG=<pgconfig>`. On GCC 15 (Ubuntu 26.04) pass
  `COPT="-Wno-error=incompatible-pointer-types"` to demote the older
  `RUBY_METHOD_FUNC` cast warnings, which are valid at run time. Built against
  Ruby 3.3.
- PL/PHP: `make PG_CONFIG=<pgconfig>`. Links against the PHP embed SAPI, provided
  by the `libphp8.5-embed` package. Built against PHP 8.5.

## Reproducing

In the container:

```
PGHOST=/tmp PGPORT=5432 python3 bench/run_bench.py
```

Requires the `plx` extension and the comparison languages installed: PL/Perl and
PL/Python3 (`bench/build_native_pls.sh`), and optionally native PL/Ruby and
PL/PHP (`bench/try_native_ruby_php.sh`).

# plx Benchmarks

## Method

Five workloads were measured on PostgreSQL 18.4 in the development container
(8 vCPU, 8 GiB, Ubuntu 26.04):

- arith: accumulate the integers 1 to 2,000,000 in a loop (loop dispatch and
  integer arithmetic).
- strbuild: build a 200,000-character string one character at a time in a loop
  (text handling with the natural in-language idiom for each language).
- iter: sum a `bigint` column over a 1,000,000-row table (SPI and per-row
  marshalling).
- branch: a four-way conditional per element over 2,000,000 elements (branch
  dispatch).
- call: 500,000 calls to a small function (call and return overhead). For plperl
  and plpython3u this uses 50,000 iterations, because each call goes through SPI
  and is far slower; those two cells are scaled to the smaller count.

Each function is written idiomatically in each language, checked for a correct
result, and called five times; the minimum wall-clock time (`psql \timing`) is
reported. The harness is `bench/run_bench.py`. All languages run in one database;
plx uses plx-prefixed language names, so it coexists with the native plruby,
plphp, plperl, and plpython3u.

plxruby, plxphp, plxjs, and plxpython3 transpile to plpgsql at `CREATE FUNCTION`
time, so at run time they execute as plpgsql. plperl and plpython3u run their own
embedded interpreters and retrieve rows through SPI.

## Results

Times are milliseconds; the multiplier is relative to plpgsql (lower is faster).

| language   | arith        | strbuild     | iter          | branch        | call          |
|------------|--------------|--------------|---------------|---------------|---------------|
| plpgsql    | 53 (1.00x)   | 1710 (1.00x) | 95 (1.00x)    | 158 (1.00x)   | 166 (1.00x)   |
| plxruby    | 56 (1.06x)   | 1725 (1.01x) | 95 (1.00x)    | 156 (0.99x)   | 157 (0.95x)   |
| plxphp     | 54 (1.02x)   | 1741 (1.02x) | 99 (1.04x)    | 161 (1.02x)   | 160 (0.97x)   |
| plxjs      | 54 (1.03x)   | 1783 (1.04x) | 99 (1.04x)    | 171 (1.08x)   | 173 (1.04x)   |
| plxpython3 | 59 (1.11x)   | 1729 (1.01x) | 99 (1.05x)    | 157 (1.00x)   | 163 (0.98x)   |
| plperl     | 46 (0.86x)   | 12 (0.01x)   | 535 (5.63x)   | 159 (1.01x)   | 301 (1.82x)   |
| plpython3u | 73 (1.39x)   | 16 (0.01x)   | 351 (3.69x)   | 113 (0.72x)   | 181 (1.09x)   |

## Analysis

- The four plx dialects match plpgsql within about 11 percent on every workload,
  because the stored function body is plpgsql. There is no run-time translation
  cost; the translation happens once at `CREATE FUNCTION`. The small spread among
  the dialects is measurement noise.

- Row iteration (iter): plpgsql, and therefore the plx dialects, are 3.7x to 5.6x
  faster than plperl and plpython3u. plpgsql streams rows through a cursor and
  reads columns directly, while the embedded interpreters copy each row into
  their own data structures.

- String building (strbuild): this is a plpgsql weakness that plx inherits.
  Concatenating onto a text variable in a loop (`s := s || 'x'`) is quadratic,
  because each step rebuilds the whole string; plpgsql has no in-language string
  builder. plperl and plpython3u use an efficient append or list-join and are
  about 100x faster here. For text assembly over many pieces, build the string in
  SQL instead (for example `string_agg` over a set) rather than character by
  character in a loop.

- Arithmetic (arith): the embedded interpreters vary. plperl is fastest (native
  integer arithmetic), plpython3u is slowest, and plpgsql with the plx dialects
  sits between.

- Branching (branch): plpython3u is faster on this pure-CPU integer workload
  (0.72x); plperl and plpgsql with the plx dialects are close to each other.

- Call overhead (call): plpgsql and the plx dialects are fastest. plperl and
  plpython3u pay an SPI round trip per call and are slower even at one tenth the
  iteration count.

The transpile-to-plpgsql approach gives the plx dialects the performance profile
of plpgsql: strong on set-oriented and SQL-bound work, competitive on procedural
arithmetic and branching, weak on naive in-loop string building, and with no
additional language runtime loaded into the backend.

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

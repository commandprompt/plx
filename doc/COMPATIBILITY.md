# plx Compatibility

## Supported versions

plx supports PostgreSQL 13 through 18 (released), and builds and passes on the
19 and 20 development lines. The full pg_regress suite (one file per dialect
plus plx_features, plx_output, plx_strbuild, and plx_errors) passes on each of
PostgreSQL 13 through 18. CI (`.github/workflows/ci.yml`) runs `make
installcheck`, `make differentialcheck`, and `make metacheck` on PostgreSQL 13
through 18 from the PGDG packages on every push and pull request. The 19 and 20
results were checked manually against source builds and are not part of CI.

| PostgreSQL | Status | string builder |
|------------|--------|----------------|
| 13 | pass | correct, not accelerated |
| 14 | pass | correct, not accelerated |
| 15 | pass | correct, not accelerated |
| 16 | pass | correct, not accelerated |
| 17 | pass | correct, not accelerated |
| 18 | pass | accelerated (amortized O(1)) |
| 19 (beta) | pass (manual, from source) | accelerated (amortized O(1)) |
| 20 (devel) | pass (manual, from source) | accelerated (amortized O(1)) |

PostgreSQL 19 and 20 compile with a C23 toolchain (for example gcc 15), where
`pg_noreturn` becomes the standard `[[noreturn]]` attribute; plx writes it as the
first token of each declaration so it compiles across all of these versions.

## The string builder is accelerated only on PostgreSQL 18

plx ships a string builder (`plx_strbuild`) and lowers the dialect string-append
operators onto it, so building a string in a loop is amortized O(1) instead of
the O(n^2) of `s := s || 'x'`. The in-place append relies on
`SupportRequestModifyInPlace`, which was introduced in PostgreSQL 18. On
PostgreSQL 13 to 17 the builder produces correct results, but the append is
O(n^2), the same as plain concatenation, because those versions only let the
built-in array functions take a read-write expanded argument. The transpiler
lowers to the builder on every version (the results are always correct); the
speedup appears when running on PostgreSQL 18. See the
[benchmarks](https://github.com/commandprompt/plx/blob/master/bench/BENCHMARKS.md).

## Why the version range holds

plx binds each dialect language to plpgsql's own call handler, so a plx function
executes as plpgsql. This depends on the plpgsql handler symbols
(`plpgsql_call_handler`, `plpgsql_inline_handler`) being resolvable from another
loaded module. On PostgreSQL 13 to 17 the server is built with default symbol
visibility, so these symbols are global. On PostgreSQL 18 they remain global
because they are the fmgr entry points (declared `PGDLLEXPORT`). The catalog
model plx uses (a language whose `lanplcallfoid` points at plpgsql's handler,
with the dialect body transpiled to plpgsql and stored in `pg_proc.prosrc`) is
unchanged across these versions.

## Source portability

Two server APIs differ across the supported majors; plx handles both:

- `pg_noreturn` is a PostgreSQL 18 prefix specifier. On earlier versions plx
  defines it as the compiler noreturn attribute.
- `pg_b64_encode` changed signature across majors. plx uses a self-contained
  base64 encoder for the embedded original source, so it does not depend on the
  server function.

## Reproducing

CI is the reference: `.github/workflows/ci.yml` installs each PostgreSQL major
from PGDG, builds plx, and runs the suites listed above. To reproduce locally
from source, `test/pg_matrix.sh` builds PostgreSQL 13 to 17 from source, builds plx against
each `pg_config`, and runs the regression suite per version. Run it as root in a
build environment with the standard PostgreSQL build dependencies installed.

```
test/pg_matrix.sh
cat /root/pg_matrix_summary.txt
```

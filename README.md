# plx

plx is a PostgreSQL extension that lets you write stored functions in a Ruby,
PHP, JavaScript, or Python dialect. At `CREATE FUNCTION` time plx transpiles the
function body to plpgsql and stores the plpgsql in `pg_proc.prosrc`. At run time
the function is executed by the standard plpgsql interpreter. There is no
separate language runtime loaded into the backend.

The front end is dialect-pluggable. Four dialects are implemented, each with its
own chapter:

- `plxruby`: a Ruby dialect. See [doc/plxruby.md](doc/plxruby.md).
- `plxphp`: a PHP dialect. See [doc/plxphp.md](doc/plxphp.md).
- `plxjs`: a JavaScript dialect. See [doc/plxjs.md](doc/plxjs.md).
- `plxpython3`: a Python dialect. See [doc/plxpython3.md](doc/plxpython3.md).

Every plpgsql statement type is reachable from every dialect. See
[doc/PARITY.md](doc/PARITY.md) for the construct matrix.

The language names are prefixed with `plx` so the extension coexists with the
native PL/Ruby and PL/PHP languages in the same database.

## How it works

Each dialect provides a `PlxSurface` that describes its keywords, block style,
comment syntax, string interpolation, and variable sigil. A shared transpiler
lexes the body, restructures statements, hoists typed `DECLARE`s, rewrites a
fixed set of operators and interpolations, and passes the remaining expression
text through to plpgsql and SQL. The language's call handler is plpgsql's own
handler, so execution is plpgsql. See [doc/ARCHITECTURE.md](doc/ARCHITECTURE.md)
and [doc/TRANSPILER.md](doc/TRANSPILER.md).

## Requirements

- PostgreSQL 13 to 18.

plx is tested against PostgreSQL 13, 14, 15, 16, 17, and 18. The full regression
suite passes on each. See [doc/COMPATIBILITY.md](doc/COMPATIBILITY.md).

## Build and install

```
make PG_CONFIG=/path/to/pg_config
make install PG_CONFIG=/path/to/pg_config
```

Then, in a database:

```sql
CREATE EXTENSION plx;
```

## Usage

A `plxruby` function:

```sql
CREATE FUNCTION grade(score int) RETURNS text LANGUAGE plxruby AS $$
  grade #:: text
  if score >= 90
    grade = "A"
  elsif score >= 80
    grade = "B"
  else
    grade = "F"
  end
  return grade
$$;
```

The stored plpgsql (in `pg_proc.prosrc`) is:

```plpgsql
DECLARE
  grade text;
BEGIN
  IF score >= 90 THEN grade := 'A';
  ELSIF score >= 80 THEN grade := 'B';
  ELSE grade := 'F';
  END IF;
  RETURN grade;
END;
```

The same function in `plxphp`:

```sql
CREATE FUNCTION grade(score int) RETURNS text LANGUAGE plxphp AS $$
  $grade = "F";
  if ($score >= 90) { $grade = "A"; }
  elseif ($score >= 80) { $grade = "B"; }
  else { $grade = "F"; }
  return $grade;
$$;
```

And in `plxjs`:

```sql
CREATE FUNCTION grade(score int) RETURNS text LANGUAGE plxjs AS $$
  let grade = "F";
  if (score >= 90) { grade = "A"; }
  else if (score >= 80) { grade = "B"; }
  else { grade = "F"; }
  return grade;
$$;
```

## Documentation

- Per-dialect chapters: [plxruby](doc/plxruby.md), [plxphp](doc/plxphp.md),
  [plxjs](doc/plxjs.md). Each covers the full syntax, supported constructs with
  examples, semantic differences, and what is rejected.
- [doc/PARITY.md](doc/PARITY.md): the plpgsql construct parity matrix.
- [doc/COMPATIBILITY.md](doc/COMPATIBILITY.md): supported PostgreSQL versions.
- [doc/USERGUIDE.md](doc/USERGUIDE.md): examples from the PL/pgSQL manual shown in
  each dialect next to the plpgsql they produce.
- [doc/ARCHITECTURE.md](doc/ARCHITECTURE.md) and
  [doc/TRANSPILER.md](doc/TRANSPILER.md): design and transpiler specification.
- [bench/BENCHMARKS.md](bench/BENCHMARKS.md): performance across five workloads.

## Performance

Because functions execute as plpgsql, all four plx dialects match plpgsql
(within about 11 percent across five workloads) and inherit its performance
profile: 3.7x to 5.6x faster than the embedded-interpreter PLs on row iteration,
competitive on arithmetic and branching, and weak on naive in-loop string
building (a plpgsql limitation). Full results and method are in
[bench/BENCHMARKS.md](bench/BENCHMARKS.md).

## Layout

```
plx.control, plx--1.0.sql   extension control and install SQL
Makefile                    PGXS build
src/                        C sources (transpiler, dialect surfaces, PL handler)
doc/                        per-dialect chapters, architecture, transpiler, parity
test/                       pg_regress suite, corpus runner, fuzzer
bench/                      benchmark harness and results
```

## Tests

```
make installcheck            # pg_regress suite: plxruby, plxphp, plx_errors
python3 test/run_corpus.py   # additional 33-case Ruby corpus runner
```

`make installcheck` runs against an installed plx on a running server. The
expected output is in `test/expected/`.

## License

MIT. See [LICENSE](LICENSE).

# plx

plx is a PostgreSQL extension that lets you write stored functions in a Ruby or
PHP dialect. At `CREATE FUNCTION` time plx transpiles the function body to
plpgsql and stores the plpgsql in `pg_proc.prosrc`. At run time the function is
executed by the standard plpgsql interpreter. There is no separate language
runtime loaded into the backend.

The front end is dialect-pluggable. Two dialects are implemented:

- `plxruby`: a Ruby dialect. Passes a 33-case corpus.
- `plxphp`: a PHP dialect.

Two more are planned: `plxpython3` and `plxjs`.

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

- PostgreSQL 18.

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

More examples, including ones taken from the PL/pgSQL manual shown side by side
with the plpgsql they produce, are in [doc/USERGUIDE.md](doc/USERGUIDE.md).

## Supported constructs and limitations

See [doc/LIMITATIONS.md](doc/LIMITATIONS.md) for the supported constructs per
dialect, the constructs that are rejected at `CREATE FUNCTION` time, and the
semantic differences from the source languages.

## Performance

Because functions execute as plpgsql, plxruby and plxphp match plpgsql and are
faster than the native embedded-interpreter PLs on row-iteration workloads. See
[bench/BENCHMARKS.md](bench/BENCHMARKS.md).

## Layout

```
plx.control, plx--1.0.sql   extension control and install SQL
Makefile                    PGXS build
src/                        C sources (transpiler, dialect surfaces, PL handler)
doc/                        architecture, transpiler spec, limitations
test/                       Ruby corpus + runner, PHP smoke test
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

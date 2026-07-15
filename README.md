<p align="center">
  <img src="doc/plx-logo.svg" alt="plx" width="380">
</p>

<p align="center">
  <b>Write PostgreSQL functions in the language you already know.</b><br>
  Ruby, PHP, JavaScript, Python, or COBOL syntax, compiled to plpgsql at
  <code>CREATE FUNCTION</code> time.
</p>

<p align="center">
  <a href="https://github.com/commandprompt/plx/actions/workflows/ci.yml"><img src="https://github.com/commandprompt/plx/actions/workflows/ci.yml/badge.svg" alt="CI"></a>
</p>

---

## What plx is

plx is a PostgreSQL extension that lets you write stored functions and triggers
in a Ruby, PHP, JavaScript, Python, or COBOL dialect. When you run `CREATE FUNCTION`,
plx transpiles the body to plpgsql and stores that plpgsql in `pg_proc.prosrc`.
At run time the function is executed by PostgreSQL's own plpgsql interpreter.
There is no separate language runtime loaded into the backend, and nothing new to
run in production.

```sql
CREATE FUNCTION grade(score int) RETURNS text LANGUAGE plxruby AS $$
  return "A" if score >= 90
  return "B" if score >= 80
  return "F"
$$;
```

The front end is dialect-pluggable, and the set of dialects is growing. The
dialects available today are:

- `plxruby`: a Ruby dialect. See [doc/plxruby.md](doc/plxruby.md).
- `plxphp`: a PHP dialect. See [doc/plxphp.md](doc/plxphp.md).
- `plxjs`: a JavaScript dialect. See [doc/plxjs.md](doc/plxjs.md).
- `plxpython3`: a Python dialect. See [doc/plxpython3.md](doc/plxpython3.md).
- `plxcobol`: a COBOL dialect (ISO/IEC 1989:2023). See [doc/plxcobol.md](doc/plxcobol.md).

Every plpgsql statement type is reachable from every dialect. See
[doc/PARITY.md](doc/PARITY.md) for the construct matrix. The language names carry
a `plx` prefix, so the extension coexists with the native PL/Ruby and PL/PHP
languages in the same database.

## Why it exists

PostgreSQL rewards moving logic into the database: triggers, constraints,
set-returning functions, and cursors all run closest to the data. The standard
way to write that logic is plpgsql. plpgsql is fast and trusted, but its syntax
is unfamiliar to developers who spend their day in Ruby, PHP, JavaScript, or
Python, and that unfamiliarity is often enough to keep logic in the application
tier where it does not belong.

The usual alternative is an untrusted procedural language such as `plpython3u` or
`plperlu`. Those give you a familiar syntax, but at a cost: they load a full
language interpreter into the backend, most are untrusted and therefore
superuser-only, and every row they touch is marshalled across an SPI boundary
into the interpreter's own data structures.

plx takes a different position. A new language surface does not require a new
execution engine. plx changes only the syntax you write, not what runs:

- **It is still plpgsql.** The stored function body is plpgsql, executed by the
  plpgsql handler. You get plpgsql's performance and its safety as a trusted
  language, with no interpreter loaded into the backend.
- **Nothing is hidden.** The generated plpgsql is stored in `pg_proc.prosrc`,
  where you can read exactly what will run. plx embeds the original source as a
  comment so the function is idempotent to re-transpile, but the executable body
  is ordinary plpgsql you can inspect, `pg_dump`, and review.
- **The cost is paid once.** Translation happens at `CREATE FUNCTION` time, not
  per call. At run time there is no translation layer and no per-row marshalling
  beyond what plpgsql already does.

The goal is to meet developers where they are on syntax without changing what the
database actually executes.

## Who it is for

- Application developers who want to push logic into the database using syntax
  they already know, rather than learning plpgsql first.
- Teams standardizing on PostgreSQL who want triggers and functions written in a
  familiar dialect but running with plpgsql's performance and trust model.
- Anyone who wants the generated plpgsql to be visible and reviewable rather than
  executed by an opaque runtime.

## How it works

Each dialect provides a `PlxSurface` describing its keywords, block style, comment
syntax, string interpolation, and variable sigil. A shared transpiler lexes the
body, restructures statements, hoists typed `DECLARE`s, rewrites a fixed set of
operators and interpolations, and passes the remaining expression text through to
plpgsql and SQL unchanged. The call handler is plpgsql's own handler, so execution
is plpgsql. See [doc/ARCHITECTURE.md](doc/ARCHITECTURE.md) and
[doc/TRANSPILER.md](doc/TRANSPILER.md).

## Example

One function, written in three dialects, each producing the same plpgsql:

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

CREATE FUNCTION grade(score int) RETURNS text LANGUAGE plxphp AS $$
  if ($score >= 90) { $grade = "A"; }
  elseif ($score >= 80) { $grade = "B"; }
  else { $grade = "F"; }
  return $grade;
$$;

CREATE FUNCTION grade(score int) RETURNS text LANGUAGE plxjs AS $$
  let grade = "F";
  if (score >= 90) { grade = "A"; }
  else if (score >= 80) { grade = "B"; }
  else { grade = "F"; }
  return grade;
$$;
```

The stored plpgsql (in `pg_proc.prosrc`) for each is:

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

## Performance

Because functions execute as plpgsql, the plx dialects match plpgsql (within about
11 percent across five workloads) and inherit its performance profile: several
times faster than the embedded-interpreter PLs on row iteration, and competitive
on arithmetic, branching, and call overhead.

The one place plpgsql is weak is building a string in a loop: `s := s || 'x'` is
O(n^2), because text is immutable and each step copies the whole string. plx
addresses this with a string builder (`plx_strbuild`, an expanded-object type with
amortized-O(1) append) and lowers the dialect append operators (`s << x`,
`$s .= x`, `s += x` on a string) onto it. On PostgreSQL 18 this makes in-loop
string building about 170x faster than the plpgsql idiom, and faster than the
native PLs building the same string with their own append. The in-place
optimization relies on a PostgreSQL 18 planner feature
(`SupportRequestModifyInPlace`); on PostgreSQL 13 to 17 the builder is correct but
not accelerated, so that case stays O(n^2). Full results and method, including the
PostgreSQL 18-versus-17 scaling measurements, are in
[bench/BENCHMARKS.md](bench/BENCHMARKS.md).

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

## Documentation

- Per-dialect chapters: [plxruby](doc/plxruby.md), [plxphp](doc/plxphp.md),
  [plxjs](doc/plxjs.md), [plxpython3](doc/plxpython3.md),
  [plxcobol](doc/plxcobol.md). Each covers the full syntax, supported constructs
  with examples, semantic differences, and what is rejected.
- [doc/PARITY.md](doc/PARITY.md): the plpgsql construct parity matrix.
- [doc/COMPATIBILITY.md](doc/COMPATIBILITY.md): supported PostgreSQL versions.
- [doc/USERGUIDE.md](doc/USERGUIDE.md): examples from the PL/pgSQL manual shown in
  each dialect next to the plpgsql they produce.
- [doc/DEBUGGING.md](doc/DEBUGGING.md): correlating runtime errors to your
  source, and recovering the original dialect body.
- [doc/ARCHITECTURE.md](doc/ARCHITECTURE.md) and
  [doc/TRANSPILER.md](doc/TRANSPILER.md): design and transpiler specification.
- [bench/BENCHMARKS.md](bench/BENCHMARKS.md): performance across five workloads.
- [CHANGELOG.md](CHANGELOG.md): release history.

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
make installcheck            # pg_regress suite
python3 test/run_corpus.py   # additional Ruby corpus runner
```

`make installcheck` runs against an installed plx on a running server. The
expected output is in `test/expected/`.

## License

MIT. See [LICENSE](LICENSE).

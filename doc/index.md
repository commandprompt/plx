<p align="center">
  <img src="plx-logo.svg" alt="plx" width="360">
</p>

# plx

**Write PostgreSQL functions in the language you already know.** Ruby, PHP,
JavaScript, TypeScript, Python, Go, COBOL, Oracle PL/SQL, or Transact-SQL,
compiled to plpgsql at `CREATE FUNCTION` time.

plx is a PostgreSQL extension. When you run `CREATE FUNCTION ... LANGUAGE plx*`,
plx transpiles the body to plpgsql and stores that plpgsql in `pg_proc.prosrc`.
At run time the function is executed by PostgreSQL's own plpgsql interpreter.
There is no separate language runtime loaded into the backend, the generated
plpgsql is visible in the catalog, and every plpgsql construct is reachable from
every dialect.

```sql
CREATE EXTENSION plx;

CREATE FUNCTION grade(score int) RETURNS text LANGUAGE plxruby AS $$
  return "A" if score >= 90
  return "B" if score >= 80
  return "F"
$$;
```

## Dialects

The front end is dialect-pluggable, and the set is open-ended. The dialects
available today:

- [**plxruby**](plxruby.md) — a Ruby dialect
- [**plxphp**](plxphp.md) — a PHP dialect
- [**plxjs**](plxjs.md) — a JavaScript dialect
- [**plxts**](plxts.md) — a TypeScript dialect (plxjs plus type annotations)
- [**plxpython3**](plxpython3.md) — a Python dialect
- [**plxgo**](plxgo.md) — a Go dialect
- [**plxcobol**](plxcobol.md) — a COBOL dialect (ISO/IEC 1989:2023)
- [**plxplsql**](plxplsql.md) — an Oracle PL/SQL dialect
- [**plxtsql**](plxtsql.md) — a Transact-SQL (SQL Server) dialect

The [user guide](USERGUIDE.md) shows the same worked examples across dialects,
and [feature parity](PARITY.md) is the construct-by-construct matrix.

## Why plx

- **Trusted.** A plx function runs as plpgsql and can do exactly what plpgsql
  can, nothing more. There is no embedded interpreter, unlike the untrusted
  native PL/Ruby or PL/PHP.
- **No new runtime in production.** The catalog stores plain plpgsql; nothing
  new runs in the backend at execution time.
- **Familiar syntax, plpgsql performance.** Write in the dialect your team
  knows; run with plpgsql's execution and trust model.

## Install

Build from source against your PostgreSQL installation:

```sh
make && make install
```

Then, in the database:

```sql
CREATE EXTENSION plx;
```

Upgrading from an earlier install:

```sql
ALTER EXTENSION plx UPDATE TO '1.2';
```

plx supports PostgreSQL 13 through 18; see [compatibility](COMPATIBILITY.md).

## More

- [Architecture](ARCHITECTURE.md) and the [transpiler specification](TRANSPILER.md)
- [Debugging](DEBUGGING.md): correlating runtime errors back to your source
- [Source, issues, and releases on GitHub](https://github.com/commandprompt/plx)
- [Changelog](https://github.com/commandprompt/plx/blob/master/CHANGELOG.md)

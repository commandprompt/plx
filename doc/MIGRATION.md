# Migrating procedural code to PostgreSQL

Most teams meet plx in the middle of a migration. There is procedural code that
works, there is a deadline, and PostgreSQL speaks plpgsql. This page is about
that situation: what the realistic options are, where plx fits among them, and
where it does not.

## The situation plx is for

You are moving logic into PostgreSQL and the logic already exists somewhere
else. Four common shapes:

- **Oracle to PostgreSQL.** Packages and procedures written in PL/SQL, often
  large and often the part of the migration nobody wants to own. See
  [plxplsql](plxplsql.md).
- **SQL Server to PostgreSQL.** Stored procedures written in Transact-SQL, with
  `DECLARE @x`, `SET`, and `IIF` throughout. See [plxtsql](plxtsql.md).
- **Application logic moving into the database.** Validation, scoring, or
  reporting that lives in a Ruby, PHP, JavaScript, TypeScript, Python, or Go
  codebase and needs to run next to the data instead of a round trip away.
- **Mainframe modernisation.** Batch logic in COBOL, where the rules are the
  asset and rewriting them is the risk. See [plxcobol](plxcobol.md).

In all four the hard part is the same. The code is understood by the people who
wrote it, and a rewrite into an unfamiliar language is where the defects get
introduced.

## What plx actually does

`CREATE FUNCTION ... LANGUAGE plx*` transpiles the body to plpgsql at DDL time
and stores that plpgsql in `pg_proc.prosrc`. At run time PostgreSQL's own
plpgsql interpreter executes it. No language runtime is loaded into the
backend, and the generated plpgsql is ordinary catalog content you can read.

Two consequences matter for a migration. A body the dialect cannot parse is
rejected at `CREATE FUNCTION`, so that class of error is found at deploy time
rather than the first time a row reaches the function. Treat that as a filter
and not a guarantee: the generated plpgsql can still fail at execution the way
any plpgsql can, and a construct that transpiles cleanly can still mean
something different in SQL than it meant in the source language. Reading the
generated plpgsql, step 3 below, is what catches that. The second consequence
is that what you end up with is plpgsql, which is what you would have written
by hand.

## The alternatives

| Approach | What you write | Runtime in the backend | Result in the catalog |
| --- | --- | --- | --- |
| Rewrite by hand | plpgsql | none | plpgsql |
| plx | your dialect | none | plpgsql |
| An embedded PL (plv8, plpython3u, plperl, PL/Ruby, PL/PHP) | that language | that language's engine | that language |
| Leave the logic in the application | your dialect | none | nothing |

**Rewriting by hand** is the baseline, and for a handful of small functions it
is the right answer. It stops being the right answer at volume, because the
cost is linear in the number of functions and the risk sits with whoever is
least familiar with the original code.

**An embedded PL** gives you the real language, with its standard library and
its semantics. That is a genuine advantage plx does not offer: plx gives you a
dialect's syntax over SQL semantics, not the language itself. The cost is a
language runtime in every backend process, an operational dependency, and, for
`plpython3u` and `plperlu`, an untrusted language that only a superuser can
use. If you need actual Python or actual V8, use them; plx is not a substitute.

**Leaving the logic in the application** is a real option and often the correct
one. Move logic into the database when it needs to be transactional with the
data, when several applications must share it, or when the round trip is the
bottleneck. If none of those apply, the migration may not be necessary.

For Oracle specifically, `ora2pg` converts schema and PL/SQL to plpgsql, and
commercial Oracle-compatible distributions exist. These are complementary
rather than competing: `ora2pg` produces plpgsql you then own, while plxplsql
keeps the body in PL/SQL form in the catalog. Which you want depends on whether
your team would rather maintain plpgsql or PL/SQL from here on.

## What a migration looks like

1. **Install the extension and pick the dialect.** One dialect per source
   language. Nothing else changes about the database.
2. **Move functions across in their original form.** The body stays in the
   language it was written in, so review is done by the people who know the
   code, reading the code they know.
3. **Read the generated plpgsql.** It is in `pg_proc.prosrc`, one statement per
   source statement. This is the review step that catches a dialect construct
   meaning something different in SQL than it did at home, particularly around
   NULL.
4. **Test against the old system.** The generated plpgsql is ordinary
   PostgreSQL, so every tool you already use applies.
5. **Decide what to keep.** Some teams keep the dialect bodies because that is
   what their developers read. Others treat plx as the conversion step and move
   to plpgsql once the migration is done. Both are supported, and the next
   section is why.

## Leaving plx

Because the catalog holds plpgsql, plx is removable. The generated body of any
plx function can be recreated as a plain plpgsql function:

```sql
DO $do$
DECLARE
  body text;
BEGIN
  SELECT prosrc INTO body
    FROM pg_proc WHERE oid = 'grade(int)'::regprocedure;
  EXECUTE format(
    'CREATE FUNCTION grade_pg(score int) RETURNS text LANGUAGE plpgsql AS %L',
    body);
END;
$do$;
```

`grade_pg` is a normal plpgsql function with no dependency on the extension.
Applied across your functions, this is an exit: you keep the plpgsql and drop
plx. The original dialect source is preserved in a trailing comment inside the
generated body, so recreating it as plpgsql does not throw away the source
either. See [debugging](DEBUGGING.md) for the helper that decodes it.

An adoption decision you can reverse is a smaller decision. That is the main
argument for starting with plx during a migration rather than after one.

## When not to use plx

- **You need the language, not the syntax.** A dialect body is transpiled to
  plpgsql, so the language's standard library, object model, and runtime
  semantics are not available. [Gaps and limitations](LIMITATIONS.md) is the
  specific list per dialect, and it is worth reading before committing.
- **The expressions are the hard part.** Expressions are largely passed through
  to SQL. Where a dialect's operator means something different in SQL, notably
  `==` and `+` on strings, and NULL rather than falsy comparison, the
  translation is documented rather than emulated.
- **You have three functions to move.** Rewrite them.
- **The logic does not belong in the database.** plx makes the move cheaper,
  which is not the same as making it correct.

## Where to go next

- [User guide](USERGUIDE.md): the same worked examples across dialects.
- [Feature parity](PARITY.md): the construct-by-construct matrix against
  plpgsql.
- [Gaps and limitations](LIMITATIONS.md): what each dialect does not support.
- The cookbook for your dialect: [overview](cookbook/index.md).

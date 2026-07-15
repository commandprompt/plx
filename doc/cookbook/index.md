# Cookbooks

A cookbook of practical, runnable recipes for each plx dialect. Every recipe was
executed on PostgreSQL: plx transpiles the body to plpgsql and the standard
interpreter runs it, and the output shown is the real captured result.

Each cookbook follows the same menu, so you can compare how a task reads across
dialects: a scalar function, an accumulating loop, building a string, looping
over a query, a set-returning function, error handling, a trigger, dynamic SQL,
and a few idioms specific to the language.

| Dialect | Cookbook | Reference |
|---|---|---|
| Ruby | [Ruby cookbook](plxruby.md) | [plxruby](../plxruby.md) |
| PHP | [PHP cookbook](plxphp.md) | [plxphp](../plxphp.md) |
| JavaScript | [JavaScript cookbook](plxjs.md) | [plxjs](../plxjs.md) |
| TypeScript | [TypeScript cookbook](plxts.md) | [plxts](../plxts.md) |
| Python | [Python cookbook](plxpython3.md) | [plxpython3](../plxpython3.md) |
| Go | [Go cookbook](plxgo.md) | [plxgo](../plxgo.md) |
| COBOL | [COBOL cookbook](plxcobol.md) | [plxcobol](../plxcobol.md) |
| Oracle PL/SQL | [PL/SQL cookbook](plxplsql.md) | [plxplsql](../plxplsql.md) |
| Transact-SQL | [T-SQL cookbook](plxtsql.md) | [plxtsql](../plxtsql.md) |

For the same worked examples shown side by side across dialects, see the
[User guide](../USERGUIDE.md). For what each dialect cannot do, see
[Gaps and limitations](../LIMITATIONS.md).

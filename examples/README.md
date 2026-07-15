# plx examples

Runnable recipes. Each file is self-contained: it creates the extension and its
own tables, and can be run with psql:

```
psql -d yourdb -f examples/01_audit_trigger.sql
```

| File | Shows | Dialect(s) |
|---|---|---|
| [01_audit_trigger.sql](01_audit_trigger.sql) | a `BEFORE UPDATE` trigger stamping `updated_at` and a version counter | plxruby |
| [02_set_returning.sql](02_set_returning.sql) | `RETURNS TABLE` and `RETURNS SETOF` | plxpython3, plxts |
| [03_dynamic_and_cursor.sql](03_dynamic_and_cursor.sql) | dynamic SQL with binds and an explicit cursor | plxphp, plxplsql |

The same logic can be written in any dialect; see the per-dialect chapters under
[`doc/`](../doc) and the side-by-side [User Guide](../doc/USERGUIDE.md).

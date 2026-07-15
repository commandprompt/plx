# plxts: the TypeScript dialect

plxts lets you write PostgreSQL functions with TypeScript syntax. It is the
[plxjs](plxjs.md) (JavaScript) dialect plus TypeScript type annotations. At
`CREATE FUNCTION` time plx transpiles the body to plpgsql and stores the plpgsql
in `pg_proc.prosrc`. The function runs on the standard plpgsql interpreter.

Everything in [plxjs](plxjs.md) applies here (brace blocks, `let`/`const`/`var`,
template literals, `for`/`while`/`switch`, `try`/`catch`/`finally`, `for...of`
over `query()` and arrays). The only addition is type annotations.

## Setup

```sql
CREATE EXTENSION plx;
```

## Type annotations

A declaration carries its type after the variable name, as in TypeScript:

```sql
CREATE FUNCTION sum_to(n int) RETURNS bigint LANGUAGE plxts AS $$
let total: bigint = 0;
for (let i = 1; i <= n; i++) {
  total = total + i;
}
return total;
$$;
```

The annotation replaces the plxjs `/*:: type */` comment and gives the local its
plpgsql type. Both forms with and without an initializer are supported:

```sql
let count: bigint = 0;
let name: string;         // declared, assigned later
let rate: numeric(10,2);  // a SQL type is used verbatim
```

### Type mapping

TypeScript primitive types map to SQL types; a name that is not a known TS
primitive is emitted verbatim, so any PostgreSQL type name (or `%TYPE`) works.

| TypeScript | PostgreSQL |
|---|---|
| `number` | `numeric` |
| `string` | `text` |
| `boolean` | `boolean` |
| `bigint` | `bigint` |
| `T[]` | `T[]` (SQL array of the mapped element type) |
| `T \| null`, `T \| undefined` | the non-null member `T` |
| `integer`, `text`, `numeric(p,s)`, `date`, ... | verbatim SQL type |

The loop variable of a counting `for (let i: number = 0; ...)` is an integer in
plpgsql, so its annotation is dropped. Only a colon that follows a
`let`/`const`/`var` declaration is treated as an annotation; a ternary
(`c ? a : b`) or label colon is left alone.

## Everything else

All other constructs are exactly as in plxjs. For example, iterating a query:

```sql
CREATE FUNCTION order_total(g int) RETURNS bigint LANGUAGE plxts AS $$
let total: bigint = 0;
for (const r of query(`SELECT amount FROM orders WHERE grp = ${g}`)) {
  total = total + r.amount;
}
return total;
$$;
```

Iterating an array requires the element type on the variable's declaration (as
in plxjs):

```sql
let v: number = 0;
for (const v of values) {
  total = total + v;
}
```

## Semantic differences

These are intentional and inherited from plxjs (see [plxjs.md](plxjs.md)).

- Decimal literals infer `numeric`, not IEEE 754; `number` maps to `numeric`.
- Comparisons use SQL three-valued logic; `===`/`!==` behave like `==`/`!=`.
- The type system is not enforced at compile time: annotations only set the
  plpgsql declaration type. Structural/interface types, generics beyond `T[]`,
  and complex unions are not modeled; use a SQL type name for anything the table
  above does not cover.

## Not supported

Everything plxjs rejects (function/class/arrow definitions, object/array
literals as general values, non-`query`/array `for...of`, `switch`
fall-through), plus:

- TypeScript type declarations (`interface`, `type`, `enum`), generics, and
  type-only imports.
- Multiple annotated declarators in one statement
  (`let a: number = 1, b: string = 'x';`); use one declaration per statement.

See [PARITY.md](PARITY.md) and [plxjs.md](plxjs.md).

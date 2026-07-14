-- plx cross-cutting feature tests: things not tied to one dialect's syntax
-- (OUT params, triggers, record-field assignment, idempotency, string escaping,
-- semantic edge cases, and DO blocks in every dialect).
CREATE EXTENSION IF NOT EXISTS plx;
SET client_min_messages = notice;

-- OUT parameters
CREATE FUNCTION f_out(a int, b int, OUT sum int, OUT diff int) LANGUAGE plxruby AS $$
sum = a + b
diff = a - b
$$;
SELECT * FROM f_out(10, 3);

-- trigger function assigning to NEW.field (plxruby)
CREATE TABLE f_trg(id int, tag text);
CREATE FUNCTION f_trg_fn() RETURNS trigger LANGUAGE plxruby AS $$
NEW.tag = "set-#{NEW.id}"
return NEW
$$;
CREATE TRIGGER f_tr BEFORE INSERT ON f_trg FOR EACH ROW EXECUTE FUNCTION f_trg_fn();
INSERT INTO f_trg(id) VALUES (7);
SELECT tag FROM f_trg;

-- trigger function in plxpython3
CREATE TABLE f_trg2(id int, tag text);
CREATE FUNCTION f_trg_py() RETURNS trigger LANGUAGE plxpython3 AS $$
NEW.tag = f"py-{NEW.id}"
return NEW
$$;
CREATE TRIGGER f_tr2 BEFORE INSERT ON f_trg2 FOR EACH ROW EXECUTE FUNCTION f_trg_py();
INSERT INTO f_trg2(id) VALUES (9);
SELECT tag FROM f_trg2;

-- CREATE OR REPLACE is idempotent and re-transpiles the new body
CREATE FUNCTION f_idem() RETURNS int LANGUAGE plxruby AS $$ return 1 $$;
CREATE OR REPLACE FUNCTION f_idem() RETURNS int LANGUAGE plxruby AS $$ return 2 $$;
SELECT f_idem();
SELECT prosrc ~ '^/[*]plx:v1:' AS has_sentinel FROM pg_proc WHERE proname = 'f_idem';

-- string escaping: apostrophe inside interpolation
CREATE FUNCTION f_apos(n int) RETURNS text LANGUAGE plxruby AS $$
return "it's #{n} o'clock"
$$;
SELECT f_apos(9);

-- escape sequences lower to an E-string
CREATE FUNCTION f_esc() RETURNS boolean LANGUAGE plxruby AS $$
return "a\tb\nc" = E'a\tb\nc'
$$;
SELECT f_esc();

-- literal percent in a RAISE message is preserved
CREATE FUNCTION f_pct() RETURNS void LANGUAGE plxruby AS $$
raise notice: "50% done for #{1 + 1} items"
$$;
SELECT f_pct();

-- exclusive range yields the exact element set (built via interpolation, since
-- + stays numeric)
CREATE FUNCTION f_excl() RETURNS text LANGUAGE plxruby AS $$
out = "" #:: text
for i in 1...4
  out = "#{out}#{i}"
end
return out
$$;
SELECT f_excl();

-- SQL three-valued equality: == maps to plain = (not null-aware), so a
-- comparison involving NULL is unknown and the condition is not taken.
CREATE FUNCTION f_null(a int, b int) RETURNS text LANGUAGE plxruby AS $$
if a == b
  return "eq"
end
return "ne"
$$;
SELECT f_null(NULL, NULL) AS null_vs_null_is_ne,
       f_null(1, NULL) AS val_vs_null_is_ne,
       f_null(2, 2) AS eq;
-- the literal-nil form is the only null special case
CREATE FUNCTION f_isnil(a int) RETURNS text LANGUAGE plxruby AS $$
return a == nil ? "nil" : "notnil"
$$;
SELECT f_isnil(NULL) AS nil, f_isnil(5) AS notnil;

-- DO blocks in every dialect
DO LANGUAGE plxruby $$ raise notice: "ruby do #{2 * 3}" $$;
DO LANGUAGE plxphp $$ raise('notice', 'php do ' . (2 * 3)); $$;
DO LANGUAGE plxjs $$ raise("notice", `js do ${2 * 3}`); $$;
DO LANGUAGE plxpython3 $$
raise('notice', f'py do {2 * 3}')
$$;

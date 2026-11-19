-- Regression tests for bugs found by the 2026-11-19 audit (fix_brace).
-- Each case names its audit finding (A<n>).
-- A13: a for-of / foreach over query() with no arguments is rejected, not a crash
DO LANGUAGE plxjs $$ for (const r of query()) { } $$;
DO LANGUAGE plxts $$ for (const r of query()) { } $$;
DO LANGUAGE plxphp $$ foreach (query() as $r) { } $$;
SELECT 'A13 backend still alive' AS ok;
-- N-pyquery: a Python for over query() with no arguments is rejected, not a crash
DO LANGUAGE plxpython3 $$
for r in query():
    pass
$$;
SELECT 'N-pyquery backend still alive' AS ok;

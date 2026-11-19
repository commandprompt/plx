/* plx 2.0.0 -> 2.1.0: no catalog changes.
 *
 * 2.1.0 makes the extension non-relocatable (relocatable = false in
 * plx.control). Generated code now names the extension's own objects
 * (plx_strbuild, plx_sb_append) qualified with the schema plx is installed in,
 * so a function with its own SET search_path still finds them and an object of
 * the same name earlier on the caller's path cannot stand in for them. Moving
 * the extension with ALTER EXTENSION ... SET SCHEMA would leave those stored
 * names pointing at the old schema, so it is no longer allowed.
 *
 * PostgreSQL records relocatability in pg_extension when the extension is
 * created or updated, which is why this otherwise empty update exists: running
 * it is what records the new setting for an existing installation.
 *
 * Functions already in the catalog keep the unqualified names they were
 * generated with; they pick up the qualified names the next time their DDL is
 * run. See the 2.1.0 entry in CHANGELOG.md. */

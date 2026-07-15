/* plx 1.2 -> 1.2.1: no catalog changes.
 *
 * 1.2.1 is a code-only patch release (the loadable module, $libdir/plx): it
 * lets plx build on PostgreSQL 19 and 20 with a C23 toolchain by placing
 * pg_noreturn as the first token of each declaration. None of this changes the
 * SQL objects the extension defines, so this update script only advances the
 * recorded version. Installing the matching $libdir/plx is what delivers the
 * fix. */

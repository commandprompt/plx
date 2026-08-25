/* plx 1.3.2 -> 2.0.0: no catalog changes.
 *
 * 2.0.0 is a major release for a behaviour change in the loadable module
 * ($libdir/plx), not for a change to the SQL objects the extension defines, so
 * this update script only advances the recorded version.
 *
 * The behaviour change is that interpolating a NULL now propagates it rather
 * than rendering an empty string. It applies at CREATE FUNCTION time, when a
 * body is transpiled. Functions already in the catalog hold their previously
 * generated plpgsql and are unaffected by this update; they take the new
 * behaviour the next time their DDL is run. See the 2.0.0 entry in CHANGELOG.md
 * for how to find the functions this will change. */

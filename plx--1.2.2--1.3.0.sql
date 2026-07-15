/* plx 1.2.2 -> 1.3.0: no catalog changes.
 *
 * 1.3.0 adds plxtsql trigger row mutation (SET NEW.col = e), which lives entirely
 * in the loadable module ($libdir/plx). No SQL objects the extension defines
 * change, so this update script only advances the recorded version. Installing
 * the matching $libdir/plx is what delivers the new behavior. */

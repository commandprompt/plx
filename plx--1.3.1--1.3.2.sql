/* plx 1.3.1 -> 1.3.2: no catalog changes.
 *
 * 1.3.2 is a code-only patch: a plxgo fix in the shared transpiler (loadable
 * module $libdir/plx) for fmt.Sprintf emitting a format string SQL format()
 * rejects. No SQL objects the extension defines change, so this update script
 * only advances the recorded version. Installing the matching $libdir/plx is
 * what delivers the fix. */

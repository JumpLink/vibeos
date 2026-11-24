#!/bin/sh
# undefined-names.sh SOURCE_DIR TSC: fails when a module of the extension uses a name it neither declares nor imports.
#
# JavaScript finds such a name only when the line runs. keyguard.js once read SCREEN_W after its import of it was
# lost: the line runs only on a phone, so the tablet never met it, and on every phone the extension then failed to
# enable and the session fell back to GDM's login screen. TypeScript's checker, on the plain JavaScript, reports every
# such name (TS2304, TS2552) without running anything; its other complaints (GI has no types) are ignored.
set -u
out=$("$2" -p "$1/tests/tsconfig.json" 2>&1)
bad=$(printf '%s\n' "$out" | grep -E 'error TS(2304|2552):')
if [ -n "$bad" ]; then
	printf '%s\n' "$bad"
	exit 1
fi
# a configuration the checker could not read (TS5xxx, TS6xxx) or no file found (TS18003) checks nothing
broken=$(printf '%s\n' "$out" | grep -E 'error TS(5[0-9]{3}|6[0-9]{3}|18003):')
if [ -n "$broken" ]; then
	printf '%s\n' "$broken"
	exit 1
fi
echo "every name used is declared or imported"

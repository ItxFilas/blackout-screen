#!/bin/sh
# Self-extracting installer stub for blackout-screen. Everything below the
# __ARCHIVE_BELOW__ marker is a tar.gz payload appended by `make dist`; this
# part only extracts it to a temp dir and runs the real installer.
set -eu
MARKER_LINE=$(awk '/^__ARCHIVE_BELOW__$/ { print NR + 1; exit }' "$0")
TMPDIR=$(mktemp -d)
trap 'rm -rf "$TMPDIR"' EXIT
tail -n +"$MARKER_LINE" "$0" | tar xz -C "$TMPDIR"
cd "$TMPDIR/blackout-screen"
sh install.sh
exit 0
__ARCHIVE_BELOW__

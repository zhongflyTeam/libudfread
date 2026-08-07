#!/bin/sh
# Print a line-coverage summary for the unit tests.
#
# Usage: coverage.sh <builddir> [test]
#   <builddir>  meson build directory (must be configured with -Db_coverage=true)
#   [test]      test executable name (default: test_ecma167)
#
# Runs the test binary (fresh .gcda), then aggregates gcov output.
# No gcovr/lcov needed - plain gcov from the compiler.

set -eu
LC_ALL=C

builddir=${1:-}
testname=${2:-test_ecma167}

[ -n "$builddir" ] || { echo "usage: $0 <builddir> [test]" >&2; exit 2; }

pdir="$builddir/tests/$testname.p"
binary="$builddir/tests/$testname"

[ -d "$pdir" ] || { echo "error: $pdir not found (build configured with -Db_coverage=true?)" >&2; exit 1; }
[ -x "$binary" ] || { echo "error: $binary not found (run meson compile first)" >&2; exit 1; }

"$binary" >/dev/null

cd "$pdir"
for f in "$testname".c.gcno "$testname".c.gcda; do
    [ -f "$f" ] || { echo "error: no $f (build configured with -Db_coverage=true?)" >&2; exit 1; }
done
cp "$testname".c.gcno "$testname".gcno
cp "$testname".c.gcda "$testname".gcda

gcov -b -c -o . "$testname".c 2>/dev/null | awk '
/^File / {
    file = $2
    sub(/^./, "", file)   # strip leading quote
    sub(/.$/, "", file)   # strip trailing quote
    have_file = 0
    if (file ~ /^\//) next   # skip system headers
    n = split(file, a, "/")
    name = a[n]
    have_file = 1
}
/^Lines executed:/ {
    if (!have_file) next
    match($0, /[0-9.]+% of [0-9]+/)
    s = substr($0, RSTART, RLENGTH)
    split(s, b, "% of ")
    k++
    pct[k] = b[1]
    lines[k] = b[2]
    fname[k] = name
}
END {
    if (!k) {
        print "error: no coverage data (run the tests first?)" > "/dev/stderr"
        exit 1
    }
    # the last entry is the gcov aggregate line - use it as the total
    for (i = 1; i < k; i++)
        printf "%-28s %6.2f%%  (%s lines)\n", fname[i], pct[i], lines[i]
    printf "%-28s %6.2f%%  (%s lines)\n", "TOTAL", pct[k], lines[k]
}'
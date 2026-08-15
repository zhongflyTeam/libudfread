#!/bin/sh
# Print a line-coverage summary for the unit tests.
#
# Usage: coverage.sh <builddir> [test ...]
#   <builddir>  meson build directory (must be configured with -Db_coverage=true)
#   [test ...]  test executable names (default: all unit tests)
#
# Runs each test binary (fresh .gcda), then aggregates gcov output.
# No gcovr/lcov needed - plain gcov from the compiler.

set -eu
LC_ALL=C

builddir=${1:-}
shift || true

[ -n "$builddir" ] || { echo "usage: $0 <builddir> [test ...]" >&2; exit 2; }

if [ $# -eq 0 ]; then
    set -- test_ecma167 test_udf_volume
fi

for testname in "$@"; do
    pdir="$builddir/tests/$testname.p"
    binary="$builddir/tests/$testname"
    codefile="${testname#test_}.c"   # test_X.c targets X.c

    [ -d "$pdir" ] || { echo "error: $pdir not found (build configured with -Db_coverage=true?)" >&2; exit 1; }
    [ -x "$binary" ] || { echo "error: $binary not found (run meson compile first)" >&2; exit 1; }

    "$binary" >/dev/null

    (
        cd "$pdir"
        for f in "$testname".c.gcno "$testname".c.gcda; do
            [ -f "$f" ] || { echo "error: no $f (build configured with -Db_coverage=true?)" >&2; exit 1; }
        done
        cp "$testname".c.gcno "$testname".gcno
        cp "$testname".c.gcda "$testname".gcda

        echo "== $testname =="
        gcov -b -c -o . "$testname".c 2>/dev/null | awk -v codefile="$codefile" '
/^File / {
    file = $2
    sub(/^./, "", file)   # strip leading quote
    sub(/.$/, "", file)   # strip trailing quote
    have_file = 0
    if (file ~ /^\//) next   # skip system headers
    n = split(file, a, "/")
    name = a[n]
    have_file = 1
    seen_lines = 0
}
/^Lines executed:/ {
    if (!have_file || seen_lines) next   # skip the gcov aggregate line
    seen_lines = 1
    match($0, /[0-9.]+% of [0-9]+/)
    s = substr($0, RSTART, RLENGTH)
    split(s, b, "% of ")
    k++
    pct[k] = b[1]
    lines[k] = b[2]
    fname[k] = name
    if (file ~ /\/src\//) {   # library files only for the summary line
        src_total += b[2]
        src_exec += b[1] * b[2] / 100
        if (name == codefile) {   # the file this test targets
            lib_total += b[2]
            lib_exec += b[1] * b[2] / 100
        }
    }
}
END {
    if (!k) {
        print "error: no coverage data (run the tests first?)" > "/dev/stderr"
        exit 1
    }
    for (i = 1; i <= k; i++)
        printf "%-28s %6.2f%%  (%s lines)\n", fname[i], pct[i], lines[i]
    if (lib_total > 0)
        printf "%-28s %6.2f%%  (%s lines)\n", "TOTAL (under test)", lib_exec / lib_total * 100, lib_total
    else if (src_total > 0)
        printf "%-28s %6.2f%%  (%s lines)\n", "TOTAL (src)", src_exec / src_total * 100, src_total
    else
        printf "%-28s %6.2f%%  (%s lines)\n", "TOTAL", pct[k], lines[k]
}'
    )
done
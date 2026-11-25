#!/bin/sh
# Compare the working tree with git ref REF (make bench BENCH_REF=REF) and
# write to OUT the figures that came out too slow or too big. A figure only
# counts when a second run confirms it, since one run can be unlucky. Exits
# 0 whatever the figures are, and 2 when the benchmark could not run.
#
# CI runs this on several machines and fails only for a figure that is worse
# on all of them: on one machine alone, code layout can move a figure by 10%.
#
#   bench/regressions.sh REF OUT
set -u
ref=$1
out=$2
tmp=$(mktemp -d "${TMPDIR:-/tmp}/quip-bench-XXXXXX") || exit 2

# Run the comparison once, show it, and print the regressed figures' names.
attempt() {
    make bench BENCH_REF="$ref" >"$tmp/log" 2>&1
    cat "$tmp/log" >&2
    if ! grep -q ' old  *new ' "$tmp/log"; then
        echo "bench/regressions.sh: the benchmark did not run" >&2
        exit 2
    fi
    grep -- '<-- regression' "$tmp/log" | cut -c1-36 | sed 's/ *$//' | sort
}

attempt >"$tmp/first"
if [ -s "$tmp/first" ]; then
    echo "Running again to confirm:" >&2
    attempt >"$tmp/second"
    comm -12 "$tmp/first" "$tmp/second" >"$out"
else
    : >"$out"
fi
rm -f "$tmp/log" "$tmp/first" "$tmp/second"
rmdir "$tmp"
if [ -s "$out" ]; then
    echo "Worse than allowed here, against $ref:" >&2
    cat "$out" >&2
fi
exit 0

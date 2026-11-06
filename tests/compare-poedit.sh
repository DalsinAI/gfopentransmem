#!/bin/sh
# GFOpenTransMem against Poedit's own translation memory: the SQLite one in
# gfamigapoedit (src/tm/transmem.cpp, pinned below; fetched only to compare
# with, never built into the library), with Poedit's types stubbed as that
# repository's tests do. Random entries and queries (tests/gen_compare.py),
# the same answers expected: every hit, its order and its score to 6 places.
#   sh tests/compare-poedit.sh [SEEDS]     needs g++, git, libsqlite3-dev
# MIT, Copyright (c) 2026 AmiCradle Team.
set -eu
HERE=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
OUT=${OUT:-"$HERE/build-host"}
POEDIT_COMMIT=36c4af2b2b425b8340ee73857c105c27c042d644
P="$OUT/gfamigapoedit"
mkdir -p "$OUT"
if [ ! -d "$P/.git" ]; then
    mkdir -p "$P"; git -C "$P" init -q
    git -C "$P" fetch -q --depth 1 https://github.com/DalsinAI/gfamigapoedit "$POEDIT_COMMIT"
    git -C "$P" checkout -q FETCH_HEAD
fi
g++ -std=gnu++17 -O1 -w -I"$P/tests/tm-host-stubs" "$HERE/tests/poedit-compare.cpp" "$P/src/tm/transmem.cpp" \
    -lsqlite3 -o "$OUT/poedit-ref"
[ -x "$OUT/tmtool" ] || sh "$HERE/tests/run_host_tests.sh"
fail=0
for seed in ${*:-1 2 3 4 5}; do
    python3 "$HERE/tests/gen_compare.py" "$seed" > "$OUT/cmp$seed.txt"
    rm -rf "$OUT/cmpdata" "$OUT/cmp$seed.db"*; mkdir -p "$OUT/cmpdata"
    TMDIR="$OUT/cmpdata" HOME="$OUT/cmpdata" "$OUT/poedit-ref" "$OUT/cmp$seed.txt" > "$OUT/cmp$seed.ref"
    { printf 'open\t%s\nids\t0\n' "$OUT/cmp$seed.db"; cat "$OUT/cmp$seed.txt"; } | "$OUT/tmtool" \
        | grep -v '^open: ok$' | sed 's/^\(   [0-9.]*\|   sub:\)  .* -> /\1  -> /; s/ \[[^]]*\]$//' > "$OUT/cmp$seed.ours"
    sed -i 's/^\(   sub:\)  .* -> /\1  -> /' "$OUT/cmp$seed.ref"
    n=$(grep -c '^   ' "$OUT/cmp$seed.ref" || true)
    d=$(diff "$OUT/cmp$seed.ref" "$OUT/cmp$seed.ours" | grep -c '^[<>]' || true)
    echo "seed $seed: $n hits from Poedit's, $d lines differ"
    [ "$d" = 0 ] || { diff "$OUT/cmp$seed.ref" "$OUT/cmp$seed.ours" | head -20; fail=1; }
done
exit $fail

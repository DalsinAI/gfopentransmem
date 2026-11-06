#!/bin/sh
# TMTest through transmem.library on a 68040 under vamos (amitools): the
# answers must be the PC's, byte for byte, but for the times entries were
# made. Run after build-amiga.sh and tests/run_host_tests.sh.
#   sh tests/run_vamos.sh GFOPENCRYPTO_LIBRARY [SEEDS]
# vamos never frees a lock's key slot (LockManager._unregister_lock frees
# the wrong array), and it allows 1024: each transaction takes one, so the
# commands run in batches of 50 (a search can take three), a fresh vamos each.
# MIT, Copyright (c) 2026 AmiCradle Team.
set -eu
HERE=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
B=${OUT:-"$HERE/build-amiga"}
H=${HOST_OUT:-"$HERE/build-host"}
V="$B/vamos"
rm -rf "$V"; mkdir -p "$V/libs"
cp "$B/lib/transmem.library" "$1" "$V/libs/"
cp "$B/tests/TMTest" "$V/"
shift
fail=0
check() {   # name commands-file
    rm -f "$H/check.db"*
    (cd "$H" && sed "s|^open\t.*|open\tcheck.db|" "$2" | ./tmtool) > "$V/$1.host"
    : > "$V/$1.amiga"
    rm -f "$V/check.db"*
    sed "s|^open\t.*|open\tcheck.db|" "$2" > "$V/all.txt"
    head -1 "$V/all.txt" > "$V/open.txt"
    tail -n +2 "$V/all.txt" | split -l 50 - "$V/part."
    for p in "$V"/part.*; do
        cat "$V/open.txt" "$p" > "$V/batch.txt"
        (cd "$V" && vamos -C 68040 -m 65536 -H disable -V "vam:$V" -a LIBS:vam:libs TMTest batch.txt) >> "$V/$1.amiga" 2>"$V/vamos.err"
        rm -f "$p"
    done
    # each batch opens the database again; the times entries were made differ
    for f in "$V/$1.host" "$V/$1.amiga"; do
        grep -v '^open: ok$\|^words:' "$f" | sed 's/\t[0-9]*\t\([0-9a-f-]*\)$/\tT\t\1/' > "$f.cmp"
    done
    if diff "$V/$1.host.cmp" "$V/$1.amiga.cmp" > "$V/$1.diff"; then
        echo "$1: $(grep -c '' "$V/$1.amiga.cmp") lines, the same on the 68040 as on the PC"
    else
        echo "$1: differs"; head -20 "$V/$1.diff"; fail=1
    fi
}
check basic "$HERE/tests/basic.txt"
for seed in ${*:-1 2}; do
    python3 "$HERE/tests/gen_compare.py" "$seed" > "$V/gen$seed.txt"
    { printf 'open\tx\n'; cat "$V/gen$seed.txt"; printf 'export\n'; } > "$V/cmds$seed.txt"
    check "random$seed" "$V/cmds$seed.txt"
done
exit $fail

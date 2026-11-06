#!/bin/sh
# GFOpenTransMem PC tests: the core built for the PC (with AddressSanitizer
# when the compiler has it) on the PC's SQLite VFS, run on tests/basic.txt
# (Poedit's own TM test, the answers Poedit's TM gives) and compared with
# tests/basic.expected. tests/compare-poedit.sh goes further: random
# entries and queries against Poedit's TM itself.
# MIT, Copyright (c) 2026 AmiCradle Team.
set -eu
HERE=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
OUT=${OUT:-"$HERE/build-host"}
CC=${HOST_CC:-cc}
mkdir -p "$OUT"
. "$HERE/tools/sqlite-source.sh"
SQ=$(sqlite_source "$OUT")
SAN="-fsanitize=address,undefined"
echo 'int main(void){return 0;}' > "$OUT/san.c"
$CC $SAN -o "$OUT/san" "$OUT/san.c" 2>/dev/null || SAN=
if [ ! -f "$OUT/sqlite3.o" ] || [ "$SQ/sqlite3.c" -nt "$OUT/sqlite3.o" ]; then
    $CC -O2 -c -DSQLITE_THREADSAFE=0 -DSQLITE_OMIT_LOAD_EXTENSION -I"$SQ" "$SQ/sqlite3.c" -o "$OUT/sqlite3.o"
fi
$CC -O1 -g $SAN -Wall -Wextra -Werror -I"$SQ" -I"$HERE/src" -o "$OUT/tmtool" "$HERE/tests/tmtool.c" \
    "$HERE/src/tm_core.c" "$HERE/tests/host_sha1.c" "$OUT/sqlite3.o" -lm
rm -f "$OUT/basic.db" "$OUT/basic.db-journal"
(cd "$OUT" && ./tmtool "$HERE/tests/basic.txt") > "$OUT/basic.out"
if ! diff -u "$HERE/tests/basic.expected" "$OUT/basic.out"; then
    echo "tests/basic.txt: answers differ"; exit 1
fi
echo "tests/basic.txt: $(grep -c '^' "$OUT/basic.out") lines, as expected"

#!/bin/sh
# GFOpenTransMem for AmigaOS 3.x with an os32 GCC 16 stove (bebbo's
# amiga-gcc, GCC 16, libnix).
#   STOVE  the stove (default ~/AmiCradle/stoves/os32-gcc16); or PREFIX, a
#          toolchain folder with bin/m68k-amigaos-gcc (a GFOpenAmigaGCC
#          release), with NDK_INC naming OpenNDK's Include_H
#   OUT    the output directory (default build-amiga)
#   TARBALLS  where SOURCES' files are (default tarballs/; fetched if missing)
#   SQLITE_DIR  a folder with sqlite3.c and sqlite3.h to use instead (tests
#          only: releases are built from the pinned amalgamation)
#   HOST_TESTS=0  skip the PC tests (tests/run_host_tests.sh)
# Outputs: lib/transmem.library (copy to LIBS:), lib/libtransmem.a (the
# link stub: opens TransMemBase for you), c/TransMem (the command),
# tests/TMTest and tests/basic.txt, include/, docs/.
# The FPU check: the library is soft float (-m68040, the soft-float specs)
# and every floating point routine it calls goes through fpuauto, so it
# takes the FPU when there is one; the build fails otherwise. It also fails
# if the library needs anything from outside itself.
# MIT, Copyright (c) 2026 AmiCradle Team.
set -eu

HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
SPECS="$HERE/specs/m68k-soft-m68040.specs"
STOVE=${STOVE:-"$HOME/AmiCradle/stoves/os32-gcc16"}
P=${PREFIX:-"$STOVE/prefix"}
CC="$P/bin/m68k-amigaos-gcc"
AR="$P/bin/m68k-amigaos-ar"
NM="$P/bin/m68k-amigaos-nm"
OUT=${OUT:-"$HERE/build-amiga"}
TARBALLS=${TARBALLS:-"$HERE/tarballs"}
INC="-I$HERE/include -I$HERE/src"
[ -n "${NDK_INC:-}" ] && INC="$INC -I$NDK_INC"

. "$HERE/tools/sqlite-source.sh"
SQ=$(sqlite_source "$OUT")

if command -v python3 >/dev/null 2>&1; then
    python3 "$HERE/tools/gen_headers.py" --check
    python3 "$HERE/tools/gen_unicode.py" --check
fi
if [ "${HOST_TESTS:-1}" != 0 ] && command -v "${HOST_CC:-cc}" >/dev/null 2>&1; then
    OUT="$OUT/host" SQLITE_DIR="$SQ" sh "$HERE/tests/run_host_tests.sh"
fi

mkdir -p "$OUT/lib" "$OUT/obj" "$OUT/tests" "$OUT/docs" "$OUT/include" "$OUT/c"
cp -R "$HERE/include/." "$OUT/include/"
cp "$HERE/docs/transmem.library.md" "$OUT/docs/"
cp "$HERE/tests/basic.txt" "$OUT/tests/"

# as gfopenregex: no loops turned into memset/memcpy calls, address 0 is
# memory on an Amiga
CPU="-m68040 -specs=$SPECS -fno-tree-loop-distribute-patterns -fno-delete-null-pointer-checks -fomit-frame-pointer"
BASE="$CPU -mcrt=nix20"
CFLAGS="$BASE -Os -Wall -Wextra -Werror $INC -I$SQ"
# SQLite as gfopensqlite builds it (no WAL, no mmap, no extensions), plus:
# our own VFS, memory and mutexes (SQLITE_OS_OTHER, SQLITE_ZERO_MALLOC,
# tm_amiga.c), temporary tables in memory, and the parts a translation
# memory never uses left out.
SQFLAGS="$BASE -Os -I$SQ -DSQLITE_OS_OTHER=1 -DSQLITE_THREADSAFE=1 -DSQLITE_MUTEX_NOOP -DSQLITE_ZERO_MALLOC \
  -DSQLITE_OMIT_LOAD_EXTENSION -DSQLITE_OMIT_WAL -DSQLITE_MAX_MMAP_SIZE=0 -DSQLITE_DEFAULT_MEMSTATUS=0 \
  -DSQLITE_OMIT_DEPRECATED -DSQLITE_DQS=0 -DSQLITE_OMIT_SHARED_CACHE -DSQLITE_OMIT_LOCALTIME -DSQLITE_TEMP_STORE=3 \
  -DSQLITE_OMIT_PROGRESS_CALLBACK -DSQLITE_OMIT_DECLTYPE -DSQLITE_OMIT_UTF16 -DSQLITE_OMIT_JSON \
  -DSQLITE_OMIT_COMPILEOPTION_DIAGS -DSQLITE_OMIT_TCL_VARIABLE -DSQLITE_UNTESTABLE -DSQLITE_OMIT_AUTHORIZATION \
  -DSQLITE_OMIT_COMPLETE -DSQLITE_OMIT_GET_TABLE -DSQLITE_OMIT_INTROSPECTION_PRAGMAS"

O="$OUT/obj"
if [ ! -f "$O/sqlite3.o" ] || [ "$SQ/sqlite3.c" -nt "$O/sqlite3.o" ] || [ "$0" -nt "$O/sqlite3.o" ]; then
    echo "compiling SQLite ($(sed -n 's/^#define SQLITE_VERSION  *"\(.*\)"/\1/p' "$SQ/sqlite3.h"))"
    "$CC" $SQFLAGS -c "$SQ/sqlite3.c" -o "$O/sqlite3.o"
fi
"$CC" $CFLAGS -c "$HERE/src/tm_core.c" -o "$O/tm_core.o"
"$CC" $CFLAGS -fno-builtin -c "$HERE/library/tm_amiga.c" -o "$O/tm_amiga.o"
"$CC" $CFLAGS -fno-toplevel-reorder -c "$HERE/library/tm_lib.c" -o "$O/tm_lib.o"
"$CC" -m68040 -c "$HERE/library/tm_stack.S" -o "$O/tm_stack.o"

# fpuauto: one member per routine, as amicradle-guest builds libfpuauto.a
rm -f "$O"/fa_*.o "$O/libfpuauto.a"
"$CC" -m68040 -DFPUAUTO_COMMON -c "$HERE/fpuauto/fpuauto.S" -o "$O/fa_common.o"
for n in $(sed 's/^--wrap=//' "$HERE/fpuauto/fpuauto.wraps"); do
    "$CC" -m68040 -DR_$n -c "$HERE/fpuauto/fpuauto.S" -o "$O/fa_$n.o"
done
"$AR" rcs "$O/libfpuauto.a" "$O"/fa_*.o
WRAPS=$(sed 's/^/-Wl,/' "$HERE/fpuauto/fpuauto.wraps" | tr '\n' ' ')

# the FPU check, before linking: every floating point routine our objects
# call is one fpuauto wraps
FLOAT='___(add|sub|mul|div|neg|cmp|eq|ne|lt|le|gt|ge|unord)(s|d|x)f[23]$|___(fix|fixuns|float|floatun)(s|d|x)(i|d)f$|___extend|___trunc|^_sqrt$|^_exp$|^_log$'
UNWRAPPED=
for f in sqlite3 tm_core tm_amiga tm_lib; do
    for s in $("$NM" -u "$O/$f.o" | awk '{print $2}' | grep -E "$FLOAT" || true); do
        r=$(echo "$s" | sed 's/^_//')
        case "$r" in exp|log) continue ;; esac   # libnix's, built from the wrapped ones
        grep -qx -- "--wrap=$r" "$HERE/fpuauto/fpuauto.wraps" || UNWRAPPED="$UNWRAPPED $r"
    done
done
[ -z "$UNWRAPPED" ] || { echo "FPU check: not wrapped by fpuauto:$UNWRAPPED"; exit 1; }

# shellcheck disable=SC2086
"$CC" $BASE -nostartfiles -nostdlib -o "$OUT/lib/transmem.library" \
  "$O/tm_lib.o" "$O/tm_core.o" "$O/tm_amiga.o" "$O/tm_stack.o" "$O/sqlite3.o" \
  $WRAPS -L"$O" -lfpuauto -lm -lgcc -Wl,-Map,"$OUT/lib/transmem.library.map"
echo "$OUT/lib/transmem.library ($(wc -c < "$OUT/lib/transmem.library") bytes)"
if "$NM" -u "$OUT/lib/transmem.library" | grep . ; then
    echo "transmem.library has undefined symbols"; exit 1
fi
# libnix's maths only: nothing else of the C library may come in
if grep -E '^ *\.text +0x[0-9a-f]+ +0x[0-9a-f]+ .*libnix\.a' "$OUT/lib/transmem.library.map"; then
    echo "transmem.library links C library code"; exit 1
fi
n=$("$NM" "$OUT/lib/transmem.library" | grep -c ' ___wrap_' || true)
"$NM" "$OUT/lib/transmem.library" | grep -q ' ___fpuauto_check' || { echo "FPU check: fpuauto missing"; exit 1; }
echo "FPU check: soft float, $n routines through fpuauto (the FPU when present)"

CPROG="$CPU -Os -Wall -Wextra -noixemul $INC"
"$CC" $CPROG -c "$HERE/stub/transmem_stub.c" -o "$O/transmem_stub.o"
rm -f "$OUT/lib/libtransmem.a"
"$AR" rcs "$OUT/lib/libtransmem.a" "$O/transmem_stub.o"
"$CC" $CPROG -DTM_AMIGA -o "$OUT/tests/TMTest" "$HERE/tests/tmtool.c" -L"$OUT/lib" -ltransmem
echo "$OUT/tests/TMTest ($(wc -c < "$OUT/tests/TMTest") bytes)"
"$CC" $CPROG -o "$OUT/c/TransMem" "$HERE/command/transmem_cmd.c" -L"$OUT/lib" -ltransmem
echo "$OUT/c/TransMem ($(wc -c < "$OUT/c/TransMem") bytes)"
(cd "$OUT" && sha256sum lib/transmem.library lib/libtransmem.a c/TransMem tests/TMTest > SHA256SUMS)
cat "$OUT/SHA256SUMS"

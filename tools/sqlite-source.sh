# sqlite_source OUT: prints a folder holding sqlite3.c and sqlite3.h, the
# amalgamation SOURCES pins (gfopensqlite's version), unpacked into OUT.
# SQLITE_DIR names one to use instead (tests only). Sourced by the build
# scripts. MIT, Copyright (c) 2026 AmiCradle Team.
SQLITE_ZIP=sqlite-amalgamation-3530400.zip
SQLITE_URL=https://www.sqlite.org/2026/$SQLITE_ZIP
SQLITE_SHA=1e71ddf93849c6a6ecf58b827c0692073d2dd7ee40196158068f7b29f422e87d

sqlite_source() {
    if [ -n "${SQLITE_DIR:-}" ]; then
        echo "$SQLITE_DIR"; return
    fi
    t="${TARBALLS:-$HERE/tarballs}/$SQLITE_ZIP"
    if [ ! -f "$t" ]; then
        mkdir -p "$(dirname "$t")"
        curl -fsSL -o "$t.part" "$SQLITE_URL" >&2 && mv "$t.part" "$t"
    fi
    echo "$SQLITE_SHA  $t" | sha256sum -c - >/dev/null || { echo "SHA-256 mismatch: $t" >&2; exit 2; }
    d="$1/sqlite"
    if [ ! -f "$d/sqlite3.c" ]; then
        rm -rf "$d"; mkdir -p "$d"
        (cd "$d" && unzip -q -j "$t" '*/sqlite3.c' '*/sqlite3.h')
    fi
    echo "$d"
}

#!/bin/sh
# Packs a build-amiga.sh build as GFOpenTransMem-VERSION.lha and .zip with
# SHA256SUMS: Install-GFOpenTransMem (the Amiga Installer script, with its
# icon) beside a GFOpenTransMem drawer of Libs/transmem.library,
# C/TransMem, Tests/ (TMTest and its commands), SDK/ (Include, SFD, FD,
# Lib/libtransmem.a) and Docs/. tools/package.sh VERSION [OUT]
# MIT, Copyright (c) 2026 AmiCradle Team.
set -eu
HERE=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
V=$1
OUT=${2:-"$HERE/dist"}
B="$HERE/build-amiga"
N="GFOpenTransMem-$V"
TOP="$OUT/$N"
R="$TOP/GFOpenTransMem"
rm -rf "$TOP" "$OUT/$N.lha" "$OUT/$N.zip"
mkdir -p "$R/Libs" "$R/C" "$R/Tests" "$R/Docs" "$R/SDK/Include" "$R/SDK/SFD" "$R/SDK/FD" "$R/SDK/Lib"
cp "$B/lib/transmem.library" "$R/Libs/"
cp "$B/c/TransMem" "$R/C/"
cp "$B/tests/TMTest" "$R/Tests/"
cp "$HERE/tests/basic.txt" "$R/Tests/tm-basic.txt"
cp -R "$HERE/include/libraries" "$HERE/include/proto" "$HERE/include/clib" "$HERE/include/inline" "$R/SDK/Include/"
cp "$HERE/include/sfd/transmem_lib.sfd" "$R/SDK/SFD/"
cp "$HERE/include/fd/transmem_lib.fd" "$R/SDK/FD/"
cp "$B/lib/libtransmem.a" "$R/SDK/Lib/"
cp "$HERE/docs/transmem.library.md" "$HERE/README.md" "$HERE/LICENSE" "$HERE/THIRD_PARTY_NOTICES.md" "$R/Docs/"
COMMIT=$(git -C "$HERE" rev-parse --short HEAD 2>/dev/null || echo unknown)
cat > "$R/ReadMe.txt" <<TXT
GFOpenTransMem $V
transmem.library: a translation memory on SQLite for AmigaOS 3.x, as
Poedit's (the same searches, scores and ids). MIT licence; SQLite is
public domain. AmiCradle Team.

Install:   double-click Install-GFOpenTransMem (beside this drawer), or
           Copy Libs/transmem.library LIBS: and Copy C/TransMem C:
           Adding entries needs gfopencrypto.library 2.2 (GFOpenTLS 1.4).
Command:   TransMem DB=file [SRC=en] LANG=cs SEARCH text ... (TransMem ?)
Test:      CD GFOpenTransMem/Tests, then TMTest tm-basic.txt
           (the first answer: 1.000000  Open File -> Otevřít soubor)
Programs:  #include <proto/transmem.h>, link SDK/Lib/libtransmem.a
           (-ltransmem); see Docs/transmem.library.md.

Built from commit $COMMIT, 68040, soft float with the FPU when present.
TXT
sed "s/^(set #version \"[^\"]*\")/(set #version \"$V\")/; s/^; \$VER: Install-GFOpenTransMem [^ ]*/; \$VER: Install-GFOpenTransMem $V/" \
    "$HERE/package/Install-GFOpenTransMem" > "$TOP/Install-GFOpenTransMem"
cp "$HERE/package/Install-GFOpenTransMem.info" "$TOP/"
(cd "$TOP" && lha -aq "../$N.lha" GFOpenTransMem Install-GFOpenTransMem Install-GFOpenTransMem.info >/dev/null &&
 zip -qr "../$N.zip" GFOpenTransMem Install-GFOpenTransMem Install-GFOpenTransMem.info)
(cd "$OUT" && sha256sum "$N.lha" "$N.zip" > SHA256SUMS)
cat "$OUT/SHA256SUMS"

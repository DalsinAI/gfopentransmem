#!/usr/bin/env python3
"""Makes package/Install-GFOpenTransMem.info from an Installer project icon
(the Team's Install-*.info): same images, default tool SYS:System/Installer,
tooltypes for GFOpenTransMem. make_install_icon.py SOURCE.info OUT.info
MIT, Copyright (c) 2026 AmiCradle Team."""
import struct, sys

src = open(sys.argv[1], "rb").read()
tool = b"SYS:System/Installer\0"
i = src.index(struct.pack(">L", len(tool)) + tool)
j = i + 4 + len(tool)
count = struct.unpack(">L", src[j:j + 4])[0] // 4 - 1
k = j + 4
for _ in range(count):
    n = struct.unpack(">L", src[k:k + 4])[0]
    k += 4 + n
types = [b"APPNAME=GFOpenTransMem", b"MINUSER=NOVICE", b"DEFUSER=AVERAGE",
         b"!LOGFILE=T:Install-GFOpenTransMem.log"]
out = struct.pack(">L", (len(types) + 1) * 4)
for t in types:
    t += b"\0"
    out += struct.pack(">L", len(t)) + t
open(sys.argv[2], "wb").write(src[:j] + out + src[k:])

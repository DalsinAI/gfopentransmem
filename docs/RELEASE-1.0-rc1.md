GFOpenTransMem 1.0-rc1: transmem.library, a translation memory on SQLite for AmigaOS 3.x.

- Poedit's translation memory as a shared library: the same three searches (exact phrase, one word of slack, by words), the same scores, merging and entry ids.
- SQLite 3.53.4 (gfopensqlite's version) built in, its files on dos.library, a lock that a reset clears.
- Runs from a 4 KB Shell stack (its own stack when needed); soft float with the FPU when present (fpuauto).
- TransMem command: SEARCH, FIND, ADD, EXPORT, IMPORT, CLEAR, STATS.
- Installer script; SDK with headers, SFD, FD and libtransmem.a.
- Adding entries needs gfopencrypto.library 2.2 (GFOpenTLS 1.4).

Tested: the same answers as the Poedit port's own SQLite memory on 1,355 hits from random sets; the same answers on a 68040 (vamos) as on the PC. Not yet run on real hardware.

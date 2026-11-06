# transmem.library 1.0

A translation memory for AmigaOS 3.x: pairs of a source text and its
translation, kept in one SQLite database file, and searched the way
Poedit searches its own. `<proto/transmem.h>`, `-ltransmem`.

All text is UTF-8. Every call that answers a `LONG` answers an error code
(`TMERR_OK`, 0, when it worked); `TM_ErrorText` turns one into words.

## Opening and closing

```c
struct TMDatabase *TM_OpenDatabase(CONST_STRPTR name, LONG *error);
VOID TM_CloseDatabase(struct TMDatabase *db);
```

`name` is the database file, made if it isn't there (Poedit's is
`TranslationMemory/tm.sqlite` in its data drawer). A relative name is made
whole from the current directory when it is opened. `error` (may be NULL)
gets the reason when the answer is NULL: `TMERR_CORRUPT` for a file that
is not a translation memory or is damaged, `TMERR_CANTOPEN`, `TMERR_BUSY`
and so on. Closing commits what is not yet committed. A program that ends
without closing its databases has them closed when it closes the library.

Callers must be processes, not plain tasks. Any number of processes may
use one database at once; each call waits for the one before it. Two
programs may open the same file: the lock is gfopensqlite's, an exclusive
`Lock()` on `<file>.lck` while a statement runs, and a program that finds
it held waits up to 2 seconds, then gets `TMERR_BUSY`.

## Adding and deleting

```c
LONG TM_Insert(struct TMDatabase *db, CONST struct TMEntry *entry);
LONG TM_Delete(struct TMDatabase *db, CONST_STRPTR id);
LONG TM_DeleteAll(struct TMDatabase *db);
LONG TM_Commit(struct TMDatabase *db);
LONG TM_Rollback(struct TMDatabase *db);
LONG TM_MakeId(CONST struct TMEntry *entry, STRPTR buffer);
```

```c
struct TMEntry {
    CONST_STRPTR tme_SourceLang;  /* "en" */
    CONST_STRPTR tme_Lang;        /* "cs", "cs_CZ", "sr@latin" */
    CONST_STRPTR tme_Source;
    CONST_STRPTR tme_Translation;
    LONG tme_Created;             /* seconds since 1 January 1970; 0 is now */
    CONST_STRPTR tme_Id;          /* out only */
};
```

The first change starts a transaction; `TM_Commit` keeps the changes,
`TM_Rollback` drops them. An entry's id is Poedit's: a version 5 UUID
(SHA-1) of the two languages, the source and the translation, so adding
the same pair again replaces it. `TM_MakeId` writes that id (36
characters and a 0) into `buffer`. An entry with an empty language, or
with the same language on both sides, is not kept (and is not an error).

Ids need SHA-1, from gfopencrypto.library 2.2 or newer (GFOpenTLS 1.4),
opened at the first `TM_Insert`; without it `TM_Insert` and `TM_MakeId`
answer `TMERR_NOCRYPTO`. Searching needs nothing more.

## Searching

```c
LONG TM_Search(struct TMDatabase *db, CONST struct TMQuery *query, struct TMResults **results);
LONG TM_SearchSubstring(struct TMDatabase *db, CONST struct TMQuery *query, struct TMResults **results);
VOID TM_FreeResults(struct TMResults *results);
```

```c
struct TMQuery {
    CONST_STRPTR tmq_SourceLang;
    CONST_STRPTR tmq_Lang;        /* "cs" also finds "cs_*"; "cs_CZ" also finds "cs" */
    CONST_STRPTR tmq_Text;
    ULONG tmq_Max;                /* most results; 0 is 10 (TM_SearchSubstring: all) */
};
struct TMResult  { struct TMEntry tmr_Entry; LONG tmr_Score; };
struct TMResults { ULONG tmrs_Count; struct TMResult *tmrs_Results; };
```

`TM_Search` gives the best translations of `tmq_Text`, best first, with
`tmr_Score` from 0 to `TM_SCORE_ONE` (1000000, a perfect match). As
Poedit's translation memory does:

1. The exact phrase: entries with all its words, in its order.
2. If none, the phrase with one word of slack, scores times 0.8.
3. If none, by words: entries with all but three of its distinct words
   and no more than three words more or fewer, scores times 0.7.

A hit's score is the weight of the words it shares with the query (rarer
words weigh more: 1 + ln(entries / (entries with the word + 1)), squared),
times the share of the query's words it has, over the square root of its
own word count, relative to the best hit. A hit in the other form of the
language (cs for cs_CZ) counts 0.85. A source text the same as the query
scores 1; any other is cut by exp(-2 ln(length ratio)²), never above
0.95, and dropped if one is over three times as long as the other. Hits
with the same translation are merged (the best score stays), hits under
0.6 of the best and scores under 0.3 are dropped.

`TM_SearchSubstring` gives every entry whose source text holds
`tmq_Text`, case aside, among those the exact phrase search finds
(Poedit's "search substring").

Words are found as Lucene's StandardAnalyzer finds them: runs of letters,
digits and marks, in lower case; each CJK ideograph or kana is a word of
its own; an apostrophe inside a word stays in it; the English stop words
(a, an, and, are, as, at, be, but, by, for, if, in, into, is, it, no,
not, of, on, or, such, that, the, their, then, there, these, they, this,
to, was, will, with) are left out but keep their places. The character
tables are made from Unicode 15.1 (`tools/gen_unicode.py`).

The results are one block of memory; `TM_FreeResults` frees it.

## The rest

```c
LONG TM_ForEach(struct TMDatabase *db, struct Hook *hook);
LONG TM_GetStats(struct TMDatabase *db, struct TMStats *stats);
CONST_STRPTR TM_ErrorText(struct TMDatabase *db, LONG error);
```

`TM_ForEach` calls the hook for every entry, oldest first: `h_Entry` gets
a0 the hook, a2 the database and a1 a `struct TMEntry *` (valid during the
call). A hook that answers non-zero stops the walk. `TM_GetStats` fills
`tms_Entries` and `tms_FileSize` (the database file's size). `TM_ErrorText`
gives the words for an error code; for `TMERR_DB`, and with `db`, it is
SQLite's own message for that database's last error.

## The database

```sql
CREATE TABLE entries(id INTEGER PRIMARY KEY, uuid TEXT NOT NULL UNIQUE,
  srclang TEXT NOT NULL, lang TEXT NOT NULL, source TEXT NOT NULL,
  trans TEXT NOT NULL, created INTEGER NOT NULL, ntok INTEGER NOT NULL);
CREATE TABLE terms(term TEXT NOT NULL, entry INTEGER NOT NULL,
  pos INTEGER NOT NULL, PRIMARY KEY(term, entry, pos)) WITHOUT ROWID;
```

The same file the Poedit port's own SQLite memory (gfamigapoedit
`src/tm/transmem.cpp`) writes, so one made by either works with the other.

## Inside

- SQLite 3.53.4, the version gfopensqlite pins, is built in with its files
  on dos.library (Open, Read, Write, Seek, SetFileSize; AmigaDOS names),
  its memory from exec and its mutexes on SignalSemaphores. No WAL, no
  memory-mapped files, temporary tables in memory.
- A call runs on a stack of its own (96 KB) when the caller has less than
  48 KB free, so a Shell's 4 KB stack is enough.
- Soft float (the scores, SQLite's REAL) through mathieeedoubbas and
  mathieeedoubtrans, with fpuauto: on a 68040 or 68060 with its FPU the
  FPU does the work.
- No C library: the library needs exec, dos and the maths libraries, and
  gfopencrypto.library for `TM_Insert`.

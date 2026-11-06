#ifndef LIBRARIES_TRANSMEM_H
#define LIBRARIES_TRANSMEM_H
/* transmem.library (GFOpenTransMem): a translation memory on SQLite.
 * docs/transmem.library.md is the contract. All text is UTF-8.
 * MIT, Copyright (c) 2026 AmiCradle Team. */

#ifndef EXEC_TYPES_H
#include <exec/types.h>
#endif
#ifndef UTILITY_HOOKS_H
#include <utility/hooks.h>
#endif

#define TRANSMEMNAME "transmem.library"
#define TRANSMEM_VERSION 1

/* An open translation memory: one SQLite database file. */
struct TMDatabase;

/* One entry: a source text in one language and its translation. */
struct TMEntry {
	CONST_STRPTR tme_SourceLang;  /* "en"; a language code as Poedit writes it */
	CONST_STRPTR tme_Lang;        /* "cs", "cs_CZ", "sr@latin" */
	CONST_STRPTR tme_Source;      /* the source text */
	CONST_STRPTR tme_Translation; /* its translation */
	LONG tme_Created;             /* seconds since 1 January 1970; TM_Insert: 0 is now */
	CONST_STRPTR tme_Id;          /* out only: the entry's id, 36 characters (a UUID) */
};

/* What TM_Search and TM_SearchSubstring look for. */
struct TMQuery {
	CONST_STRPTR tmq_SourceLang;
	CONST_STRPTR tmq_Lang;        /* "cs" also finds "cs_*"; "cs_CZ" also finds "cs" */
	CONST_STRPTR tmq_Text;
	ULONG tmq_Max;                /* most results; 0 is 10 (TM_SearchSubstring: 0 is all) */
};

#define TM_SCORE_ONE 1000000      /* tmr_Score of a perfect match (1.0) */

struct TMResult {
	struct TMEntry tmr_Entry;
	LONG tmr_Score;               /* 0 to TM_SCORE_ONE; best first */
};

/* Results: one block, freed with TM_FreeResults. */
struct TMResults {
	ULONG tmrs_Count;
	struct TMResult *tmrs_Results;
};

struct TMStats {
	ULONG tms_Entries;
	ULONG tms_FileSize;           /* the database file, in bytes */
};

/* Error codes: every call that answers a LONG answers one of these. */
#define TMERR_OK        0
#define TMERR_NOMEMORY  1
#define TMERR_ARGS      2         /* a NULL pointer or a bad argument */
#define TMERR_CANTOPEN  3         /* the file can't be opened or made */
#define TMERR_CORRUPT   4         /* not a translation memory, or damaged */
#define TMERR_BUSY      5         /* another program holds the database */
#define TMERR_IO        6         /* a read or write failed */
#define TMERR_FULL      7         /* the disk is full */
#define TMERR_NOCRYPTO  8         /* TM_Insert needs gfopencrypto.library 2.2 (SHA-1) */
#define TMERR_DB        9         /* any other SQLite error: TM_ErrorText says which */

/* TM_ForEach's hook: h_Entry(hook, struct TMDatabase *, struct TMEntry *);
 * a hook that answers non-zero stops the walk. */

#endif

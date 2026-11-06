/* GFOpenTransMem core: the translation memory over the SQLite API, the same
 * on the Amiga (library/tm_lib.c) and on the PC (tests/tmtool.c).
 * MIT, Copyright (c) 2026 AmiCradle Team. */
#ifndef TM_CORE_H
#define TM_CORE_H

typedef unsigned char tm_u8;
typedef unsigned long tm_u32;

struct tm_db;

/* public structures, without exec/types.h (the same layout) */
struct tm_entry {
	const char *srclang, *lang, *source, *trans;
	long created;
	const char *id;
};
struct tm_result {
	struct tm_entry e;
	long score;
};
struct tm_results {
	unsigned long count;
	struct tm_result *items;
};
struct tm_stats {
	unsigned long entries, filesize;
};

/* from the caller: SHA-1 of n bytes (0 done, else an error code), and now
 * as seconds since 1970 */
int tm_sha1(const void *data, unsigned long n, tm_u8 out[20]);
long tm_now(void);

int tmc_open(const char *name, const char *vfs, struct tm_db **out);
void tmc_close(struct tm_db *db);
int tmc_insert(struct tm_db *db, const struct tm_entry *e);
int tmc_delete(struct tm_db *db, const char *id);
int tmc_delete_all(struct tm_db *db);
int tmc_commit(struct tm_db *db);
int tmc_rollback(struct tm_db *db);
int tmc_search(struct tm_db *db, const char *srclang, const char *lang, const char *text,
               unsigned long max, struct tm_results **out);
int tmc_search_substring(struct tm_db *db, const char *srclang, const char *lang,
                         const char *text, unsigned long max, struct tm_results **out);
void tmc_free_results(struct tm_results *r);
typedef int (*tm_each_fn)(void *ud, const struct tm_entry *e);
int tmc_foreach(struct tm_db *db, tm_each_fn fn, void *ud);
int tmc_stats(struct tm_db *db, struct tm_stats *st);
const char *tmc_error_text(struct tm_db *db, int code);

/* the words of a text, for tests: calls fn for each (UTF-8, its position) */
int tmc_words(const char *text, void (*fn)(void *ud, const char *w, unsigned long n, int pos), void *ud);
/* the id TM_Insert gives an entry: 36 characters and a 0 into out */
int tmc_make_id(const struct tm_entry *e, char out[37]);

#endif

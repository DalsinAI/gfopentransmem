/* GFOpenTransMem core: a translation memory on SQLite.
 *
 * One database file holds
 *   entries(id, uuid, srclang, lang, source, trans, created, ntok)
 *   terms(term, entry, pos)    the words of each source text
 * and a search is done the way Poedit's own translation memory does it:
 * the exact phrase first, then the phrase with one word of slack, then a
 * search by words; each hit is scored by the weight of its words (rarer
 * words weigh more), how many of them it has and how long it is, relative
 * to the best hit, with a penalty for a length unlike the query's.
 * Words are found as Lucene's StandardAnalyzer finds them: runs of letters,
 * digits and marks, lower case, every CJK ideograph or kana a word of its
 * own, English stop words left out but counted in the positions, an
 * apostrophe inside a word kept in it.
 *
 * Plain C over the SQLite API: the same code runs in transmem.library and
 * in the PC tests. Memory comes from sqlite3_malloc.
 * MIT, Copyright (c) 2026 AmiCradle Team. */
#include <string.h>
#include <math.h>
#include "sqlite3.h"
#include "tm_core.h"
#include "tm_unitab.h"

#define TMERR_OK        0
#define TMERR_NOMEMORY  1
#define TMERR_ARGS      2
#define TMERR_CANTOPEN  3
#define TMERR_CORRUPT   4
#define TMERR_BUSY      5
#define TMERR_IO        6
#define TMERR_FULL      7
#define TMERR_NOCRYPTO  8
#define TMERR_DB        9

/* Poedit's search constants */
#define MAX_RESULTS 10
#define QUERY_MAX_DOCS 500
#define QUALITY_THRESHOLD 0.6
#define MAX_ALLOWED_LENGTH_DIFFERENCE 3
#define SECONDARY_LANG_BOOST 0.85
#define SCORE_FLOOR 0.3

struct tm_db {
	sqlite3 *h;
	int in_tx;
	int last;                       /* the last call's TMERR_ */
	sqlite3_stmt *st_ins, *st_term, *st_delterms, *st_del, *st_count, *st_df;
};

static int map_rc(int rc)
{
	switch (rc & 0xff) {
	case SQLITE_OK: case SQLITE_DONE: case SQLITE_ROW: return TMERR_OK;
	case SQLITE_NOMEM: return TMERR_NOMEMORY;
	case SQLITE_CANTOPEN: case SQLITE_PERM: case SQLITE_READONLY: return TMERR_CANTOPEN;
	case SQLITE_CORRUPT: case SQLITE_NOTADB: return TMERR_CORRUPT;
	case SQLITE_BUSY: case SQLITE_LOCKED: return TMERR_BUSY;
	case SQLITE_IOERR: return TMERR_IO;
	case SQLITE_FULL: return TMERR_FULL;
	case SQLITE_MISUSE: case SQLITE_RANGE: case SQLITE_TOOBIG: return TMERR_ARGS;
	default: return TMERR_DB;
	}
}

static char *dup_n(const char *s, unsigned long n)
{
	char *p = (char *)sqlite3_malloc64(n + 1);
	if (p) { memcpy(p, s, n); p[n] = 0; }
	return p;
}

static char *dup(const char *s) { return dup_n(s ? s : "", s ? strlen(s) : 0); }

/* ---- Characters -------------------------------------------------------- */

/* one code point from UTF-8; bad bytes come out as themselves */
static tm_u32 next_char(const tm_u8 *s, unsigned long n, unsigned long *i)
{
	tm_u32 c = s[(*i)++], cp;
	int k, more;
	if (c < 0x80) return c;
	more = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : 0;
	cp = c & (0x3Fu >> more);
	for (k = 0; k < more && *i < n; k++) {
		tm_u32 d = s[*i];
		if ((d & 0xC0) != 0x80) break;
		cp = (cp << 6) | (d & 0x3F);
		(*i)++;
	}
	return cp;
}

static int in_ranges(const tm_u32 (*r)[2], int n, tm_u32 c)
{
	int lo = 0, hi = n - 1;
	while (lo <= hi) {
		int mid = (lo + hi) / 2;
		if (c < r[mid][0]) hi = mid - 1;
		else if (c > r[mid][1]) lo = mid + 1;
		else return 1;
	}
	return 0;
}

#define COUNT(a) ((int)(sizeof(a) / sizeof((a)[0])))

/* a word of its own: CJK ideographs, hiragana, katakana */
static int single_char_word(tm_u32 c)
{
	return (c >= 0x3040 && c <= 0x30FF) || (c >= 0x31F0 && c <= 0x31FF) ||
	       in_ranges(tm_ideo_ranges, COUNT(tm_ideo_ranges), c);
}

static int word_char(tm_u32 c)
{
	if (c < 0x80)
		return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
	return in_ranges(tm_word_ranges, COUNT(tm_word_ranges), c);
}

static tm_u32 lower(tm_u32 c)
{
	int k;
	if (c < 0x80) return (c >= 'A' && c <= 'Z') ? c + 32 : c;
	for (k = 0; k < COUNT(tm_case_runs) && tm_case_runs[k].first <= c; k++) {
		const struct tm_case_run *r = &tm_case_runs[k];
		if (c <= r->last && (c - r->first) % r->step == 0)
			return (tm_u32)((long)c + r->delta);
	}
	return c;
}

/* ---- Growing buffers --------------------------------------------------- */

struct buf { char *p; unsigned long n, cap; int bad; };

static void buf_grow(struct buf *b, unsigned long more)
{
	unsigned long want;
	char *q;
	if (b->bad || b->n + more <= b->cap) return;
	want = b->cap ? b->cap * 2 : 64;
	while (want < b->n + more) want *= 2;
	q = (char *)sqlite3_realloc64(b->p, want);
	if (!q) { b->bad = 1; return; }
	b->p = q;
	b->cap = want;
}

static void buf_add(struct buf *b, const void *d, unsigned long n)
{
	buf_grow(b, n);
	if (b->bad) return;
	memcpy(b->p + b->n, d, n);
	b->n += n;
}

static void put_char(struct buf *b, tm_u32 c)
{
	tm_u8 o[4];
	int n;
	if (c < 0x80) { o[0] = (tm_u8)c; n = 1; }
	else if (c < 0x800) { o[0] = (tm_u8)(0xC0 | (c >> 6)); o[1] = (tm_u8)(0x80 | (c & 0x3F)); n = 2; }
	else if (c < 0x10000) {
		o[0] = (tm_u8)(0xE0 | (c >> 12)); o[1] = (tm_u8)(0x80 | ((c >> 6) & 0x3F));
		o[2] = (tm_u8)(0x80 | (c & 0x3F)); n = 3;
	} else {
		o[0] = (tm_u8)(0xF0 | (c >> 18)); o[1] = (tm_u8)(0x80 | ((c >> 12) & 0x3F));
		o[2] = (tm_u8)(0x80 | ((c >> 6) & 0x3F)); o[3] = (tm_u8)(0x80 | (c & 0x3F)); n = 4;
	}
	buf_add(b, o, (unsigned long)n);
}

/* ---- Words ------------------------------------------------------------- */

struct tok { unsigned long off, len; int pos; };
struct toks {
	struct buf t;                   /* struct tok array */
	struct buf text;                /* the words, back to back */
	int n;
};

#define TOK(ts, k) (((struct tok *)(ts)->t.p)[k])
#define TOKTEXT(ts, k) ((ts)->text.p + TOK(ts, k).off)

static const char *const stop_words[] = {
	"a", "an", "and", "are", "as", "at", "be", "but", "by", "for", "if",
	"in", "into", "is", "it", "no", "not", "of", "on", "or", "such",
	"that", "the", "their", "then", "there", "these", "they", "this",
	"to", "was", "will", "with"
};

static int stop_word(const char *w, unsigned long n)
{
	int k;
	if (n > 5) return 0;
	for (k = 0; k < COUNT(stop_words); k++)
		if (strlen(stop_words[k]) == n && !memcmp(stop_words[k], w, n))
			return 1;
	return 0;
}

static void toks_free(struct toks *ts)
{
	sqlite3_free(ts->t.p);
	sqlite3_free(ts->text.p);
	memset(ts, 0, sizeof *ts);
}

struct tokenizer { struct toks *out; struct buf word; int pos; };

static void flush(struct tokenizer *z)
{
	struct tok t;
	if (!z->word.n) return;
	z->pos++;
	if (!stop_word(z->word.p, z->word.n)) {
		t.off = z->out->text.n;
		t.len = z->word.n;
		t.pos = z->pos;
		buf_add(&z->out->text, z->word.p, z->word.n);
		buf_add(&z->out->t, &t, sizeof t);
		z->out->n++;
	}
	z->word.n = 0;
}

/* 0 or TMERR_NOMEMORY */
static int tokenize(const char *text, struct toks *out)
{
	const tm_u8 *s = (const tm_u8 *)text;
	unsigned long n = strlen(text), i = 0;
	struct tokenizer z;
	int bad;
	memset(out, 0, sizeof *out);
	memset(&z, 0, sizeof z);
	z.out = out;
	z.pos = -1;
	while (i < n) {
		tm_u32 c = next_char(s, n, &i);
		if (single_char_word(c)) {
			flush(&z);
			put_char(&z.word, c);
			flush(&z);
		} else if (word_char(c)) {
			put_char(&z.word, lower(c));
		} else if ((c == '\'' || c == 0x2019) && z.word.n && i < n) {
			/* apostrophes inside a word stay in it ("don't"), as in Lucene */
			unsigned long j = i;
			tm_u32 d = next_char(s, n, &j);
			if (word_char(d) && !single_char_word(d))
				buf_add(&z.word, "'", 1);
			else
				flush(&z);
		} else {
			flush(&z);
		}
	}
	flush(&z);
	bad = z.word.bad || out->t.bad || out->text.bad;
	sqlite3_free(z.word.p);
	if (bad) { toks_free(out); return TMERR_NOMEMORY; }
	return TMERR_OK;
}

static int tok_eq(const struct toks *a, int i, const struct toks *b, int j)
{
	return TOK(a, i).len == TOK(b, j).len && !memcmp(TOKTEXT(a, i), TOKTEXT(b, j), TOK(a, i).len);
}

/* Lucene's phrase matching: the words in order, each at its position
 * relative to the first, give or take `slop` places in all. */
static int matches_phrase(const struct toks *q, const struct toks *doc, int slop)
{
	int i0, k, j;
	if (!q->n) return 0;
	for (i0 = 0; i0 < doc->n; i0++) {
		int used = 0, ok = 1;
		if (!tok_eq(q, 0, doc, i0)) continue;
		for (k = 1; k < q->n && ok; k++) {
			int want = TOK(doc, i0).pos + (TOK(q, k).pos - TOK(q, 0).pos), best = -1;
			for (j = 0; j < doc->n; j++)
				if (tok_eq(q, k, doc, j)) {
					int d = TOK(doc, j).pos - want;
					if (d < 0) d = -d;
					if (best < 0 || d < best) best = d;
				}
			if (best < 0 || used + best > slop) ok = 0;
			else used += best;
		}
		if (ok) return 1;
	}
	return 0;
}

/* the distinct words, in byte order (as Poedit's std::map keeps them) */
struct term { const char *w; unsigned long n; double idf; };

static int term_cmp(const struct term *a, const struct term *b)
{
	unsigned long n = a->n < b->n ? a->n : b->n;
	int c = memcmp(a->w, b->w, n);
	if (c) return c;
	return a->n < b->n ? -1 : a->n > b->n;
}

/* the distinct words of q in first-seen order (for the query) and in byte
 * order (for the weights); *count of each */
static int distinct_terms(const struct toks *q, struct term **seen, struct term **sorted, int *count)
{
	int k, j, n = 0;
	struct term *a = (struct term *)sqlite3_malloc64(sizeof(struct term) * (unsigned long)(q->n ? q->n : 1));
	struct term *b = (struct term *)sqlite3_malloc64(sizeof(struct term) * (unsigned long)(q->n ? q->n : 1));
	if (!a || !b) { sqlite3_free(a); sqlite3_free(b); return TMERR_NOMEMORY; }
	for (k = 0; k < q->n; k++) {
		struct term t;
		t.w = TOKTEXT(q, k); t.n = TOK(q, k).len; t.idf = 0;
		for (j = 0; j < n; j++)
			if (a[j].n == t.n && !memcmp(a[j].w, t.w, t.n)) break;
		if (j == n) a[n++] = t;
	}
	memcpy(b, a, sizeof(struct term) * (unsigned long)n);
	for (k = 1; k < n; k++) {         /* insertion sort: a few words */
		struct term t = b[k];
		for (j = k; j > 0 && term_cmp(&b[j - 1], &t) > 0; j--) b[j] = b[j - 1];
		b[j] = t;
	}
	*seen = a; *sorted = b; *count = n;
	return TMERR_OK;
}

/* ---- Database ---------------------------------------------------------- */

static int prep(struct tm_db *db, sqlite3_stmt **st, const char *sql)
{
	if (*st) { sqlite3_reset(*st); sqlite3_clear_bindings(*st); return SQLITE_OK; }
	return sqlite3_prepare_v2(db->h, sql, -1, st, 0);
}

static int fail(struct tm_db *db, int rc)
{
	db->last = map_rc(rc);
	return db->last;
}

static int done(struct tm_db *db, int err)
{
	db->last = err;
	return err;
}

static const char schema[] =
	"PRAGMA journal_mode=DELETE;"
	"PRAGMA synchronous=NORMAL;"
	"PRAGMA temp_store=MEMORY;"
	"CREATE TABLE IF NOT EXISTS entries("
	"  id INTEGER PRIMARY KEY,"
	"  uuid TEXT NOT NULL UNIQUE,"
	"  srclang TEXT NOT NULL,"
	"  lang TEXT NOT NULL,"
	"  source TEXT NOT NULL,"
	"  trans TEXT NOT NULL,"
	"  created INTEGER NOT NULL,"
	"  ntok INTEGER NOT NULL);"
	"CREATE INDEX IF NOT EXISTS entries_lang ON entries(srclang, lang);"
	"CREATE TABLE IF NOT EXISTS terms("
	"  term TEXT NOT NULL,"
	"  entry INTEGER NOT NULL,"
	"  pos INTEGER NOT NULL,"
	"  PRIMARY KEY(term, entry, pos)) WITHOUT ROWID;"
	"CREATE INDEX IF NOT EXISTS terms_entry ON terms(entry);";

static int count_entries(struct tm_db *db, sqlite3_int64 *n)
{
	int rc = prep(db, &db->st_count, "SELECT COUNT(*) FROM entries");
	if (rc != SQLITE_OK) return rc;
	rc = sqlite3_step(db->st_count);
	if (rc != SQLITE_ROW) return rc;
	*n = sqlite3_column_int64(db->st_count, 0);
	sqlite3_reset(db->st_count);
	return SQLITE_OK;
}

int tmc_open(const char *name, const char *vfs, struct tm_db **out)
{
	struct tm_db *db;
	sqlite3_int64 n;
	int rc;
	if (!name || !out) return TMERR_ARGS;
	*out = 0;
	rc = sqlite3_initialize();
	if (rc != SQLITE_OK) return map_rc(rc);
	db = (struct tm_db *)sqlite3_malloc64(sizeof *db);
	if (!db) return TMERR_NOMEMORY;
	memset(db, 0, sizeof *db);
	rc = sqlite3_open_v2(name, &db->h, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_NOMUTEX, vfs);
	if (rc == SQLITE_OK) {
		sqlite3_busy_timeout(db->h, 2000);
		rc = sqlite3_exec(db->h, schema, 0, 0, 0);
	}
	/* checks the file really is ours (a damaged one fails here) */
	if (rc == SQLITE_OK) rc = count_entries(db, &n);
	if (rc != SQLITE_OK) {
		int err = map_rc(rc);
		if (err == TMERR_DB) err = TMERR_CORRUPT;   /* a database, but not ours */
		tmc_close(db);
		return err;
	}
	*out = db;
	return TMERR_OK;
}

void tmc_close(struct tm_db *db)
{
	if (!db) return;
	/* uncommitted changes are committed on close */
	if (db->in_tx && db->h) sqlite3_exec(db->h, "COMMIT", 0, 0, 0);
	sqlite3_finalize(db->st_ins);
	sqlite3_finalize(db->st_term);
	sqlite3_finalize(db->st_delterms);
	sqlite3_finalize(db->st_del);
	sqlite3_finalize(db->st_count);
	sqlite3_finalize(db->st_df);
	sqlite3_close(db->h);
	sqlite3_free(db);
}

static int begin_if_needed(struct tm_db *db)
{
	int rc;
	if (db->in_tx) return SQLITE_OK;
	rc = sqlite3_exec(db->h, "BEGIN", 0, 0, 0);
	if (rc == SQLITE_OK) db->in_tx = 1;
	return rc;
}

int tmc_commit(struct tm_db *db)
{
	int rc;
	if (!db) return TMERR_ARGS;
	if (!db->in_tx) return done(db, TMERR_OK);
	rc = sqlite3_exec(db->h, "COMMIT", 0, 0, 0);
	if (rc == SQLITE_OK) db->in_tx = 0;
	return fail(db, rc);
}

int tmc_rollback(struct tm_db *db)
{
	int rc;
	if (!db) return TMERR_ARGS;
	if (!db->in_tx) return done(db, TMERR_OK);
	rc = sqlite3_exec(db->h, "ROLLBACK", 0, 0, 0);
	db->in_tx = 0;
	return fail(db, rc);
}

static int delete_id(struct tm_db *db, const char *id)
{
	int rc = prep(db, &db->st_delterms, "DELETE FROM terms WHERE entry IN (SELECT id FROM entries WHERE uuid=?1)");
	if (rc == SQLITE_OK) rc = sqlite3_bind_text(db->st_delterms, 1, id, -1, SQLITE_STATIC);
	if (rc == SQLITE_OK) rc = sqlite3_step(db->st_delterms);
	if (db->st_delterms) sqlite3_reset(db->st_delterms);
	if (rc != SQLITE_DONE) return rc;
	rc = prep(db, &db->st_del, "DELETE FROM entries WHERE uuid=?1");
	if (rc == SQLITE_OK) rc = sqlite3_bind_text(db->st_del, 1, id, -1, SQLITE_STATIC);
	if (rc == SQLITE_OK) rc = sqlite3_step(db->st_del);
	if (db->st_del) sqlite3_reset(db->st_del);
	return rc == SQLITE_DONE ? SQLITE_OK : rc;
}

int tmc_delete(struct tm_db *db, const char *id)
{
	int rc;
	if (!db || !id) return TMERR_ARGS;
	rc = begin_if_needed(db);
	if (rc == SQLITE_OK) rc = delete_id(db, id);
	return fail(db, rc);
}

int tmc_delete_all(struct tm_db *db)
{
	int rc;
	if (!db) return TMERR_ARGS;
	rc = begin_if_needed(db);
	if (rc == SQLITE_OK) rc = sqlite3_exec(db->h, "DELETE FROM terms; DELETE FROM entries;", 0, 0, 0);
	return fail(db, rc);
}

/* ---- Ids --------------------------------------------------------------- */

/* Poedit's: a name-based (SHA-1, version 5) UUID in its own namespace, of
 * srclang + lang + source + translation as wide characters, each code
 * point 4 bytes, low byte first (Boost's name_generator on std::wstring). */
static const tm_u8 id_namespace[16] = {
	0x6e, 0x3f, 0x73, 0xc5, 0x33, 0x3f, 0x41, 0x71,
	0x9d, 0x43, 0x95, 0x4c, 0x37, 0x2a, 0x8a, 0x02
};

static void add_wide(struct buf *b, const char *s)
{
	const tm_u8 *p = (const tm_u8 *)s;
	unsigned long n = strlen(s), i = 0;
	while (i < n) {
		tm_u32 c = next_char(p, n, &i);
		tm_u8 o[4];
		o[0] = (tm_u8)c; o[1] = (tm_u8)(c >> 8); o[2] = (tm_u8)(c >> 16); o[3] = (tm_u8)(c >> 24);
		buf_add(b, o, 4);
	}
}

int tmc_make_id(const struct tm_entry *e, char out[37])
{
	static const char hex[] = "0123456789abcdef";
	struct buf b;
	tm_u8 h[20];
	int k, j, err;
	if (!e || !e->srclang || !e->lang || !e->source || !e->trans) return TMERR_ARGS;
	memset(&b, 0, sizeof b);
	buf_add(&b, id_namespace, 16);
	add_wide(&b, e->srclang);
	add_wide(&b, e->lang);
	add_wide(&b, e->source);
	add_wide(&b, e->trans);
	if (b.bad) { sqlite3_free(b.p); return TMERR_NOMEMORY; }
	err = tm_sha1(b.p, b.n, h);
	sqlite3_free(b.p);
	if (err) return err;
	h[6] = (tm_u8)((h[6] & 0x0F) | 0x50);
	h[8] = (tm_u8)((h[8] & 0x3F) | 0x80);
	for (k = 0, j = 0; k < 16; k++) {
		if (k == 4 || k == 6 || k == 8 || k == 10) out[j++] = '-';
		out[j++] = hex[h[k] >> 4];
		out[j++] = hex[h[k] & 15];
	}
	out[j] = 0;
	return TMERR_OK;
}

/* ---- Insert ------------------------------------------------------------ */

int tmc_insert(struct tm_db *db, const struct tm_entry *e)
{
	struct toks ts;
	char id[37];
	sqlite3_int64 row;
	long created;
	int rc, err, k;
	if (!db || !e || !e->srclang || !e->lang || !e->source || !e->trans) return TMERR_ARGS;
	/* as Poedit: no languages, or the same one twice, is not kept */
	if (!*e->srclang || !*e->lang || !strcmp(e->srclang, e->lang)) return done(db, TMERR_OK);
	created = e->created ? e->created : tm_now();
	if ((err = tmc_make_id(e, id)) != TMERR_OK) return done(db, err);
	if ((err = tokenize(e->source, &ts)) != TMERR_OK) return done(db, err);

	rc = begin_if_needed(db);
	if (rc == SQLITE_OK) rc = delete_id(db, id);
	if (rc == SQLITE_OK)
		rc = prep(db, &db->st_ins, "INSERT INTO entries(uuid, srclang, lang, source, trans, created, ntok) "
		                           "VALUES(?1, ?2, ?3, ?4, ?5, ?6, ?7)");
	if (rc == SQLITE_OK) {
		sqlite3_bind_text(db->st_ins, 1, id, -1, SQLITE_STATIC);
		sqlite3_bind_text(db->st_ins, 2, e->srclang, -1, SQLITE_STATIC);
		sqlite3_bind_text(db->st_ins, 3, e->lang, -1, SQLITE_STATIC);
		sqlite3_bind_text(db->st_ins, 4, e->source, -1, SQLITE_STATIC);
		sqlite3_bind_text(db->st_ins, 5, e->trans, -1, SQLITE_STATIC);
		sqlite3_bind_int64(db->st_ins, 6, (sqlite3_int64)created);
		sqlite3_bind_int64(db->st_ins, 7, (sqlite3_int64)ts.n);
		rc = sqlite3_step(db->st_ins);
		sqlite3_reset(db->st_ins);
		sqlite3_clear_bindings(db->st_ins);
		rc = rc == SQLITE_DONE ? SQLITE_OK : rc;
	}
	row = sqlite3_last_insert_rowid(db->h);
	if (rc == SQLITE_OK)
		rc = prep(db, &db->st_term, "INSERT OR IGNORE INTO terms(term, entry, pos) VALUES(?1, ?2, ?3)");
	for (k = 0; rc == SQLITE_OK && k < ts.n; k++) {
		sqlite3_bind_text(db->st_term, 1, TOKTEXT(&ts, k), (int)TOK(&ts, k).len, SQLITE_STATIC);
		sqlite3_bind_int64(db->st_term, 2, row);
		sqlite3_bind_int64(db->st_term, 3, (sqlite3_int64)TOK(&ts, k).pos);
		rc = sqlite3_step(db->st_term);
		sqlite3_reset(db->st_term);
		rc = rc == SQLITE_DONE ? SQLITE_OK : rc;
	}
	if (db->st_term) sqlite3_clear_bindings(db->st_term);
	toks_free(&ts);
	return fail(db, rc);
}

/* ---- Search ------------------------------------------------------------ */

struct cand {
	char *id, *srclang, *lang, *source, *trans;
	long created;
	int ntok, secondary;
};

struct hit {                            /* one result being gathered */
	char *id, *srclang, *lang, *source, *trans;
	long created;
	double score;
};

struct hits { struct buf b; int n; };
#define HIT(hs, k) (((struct hit *)(hs)->b.p)[k])

static void hit_free(struct hit *h)
{
	sqlite3_free(h->id); sqlite3_free(h->srclang); sqlite3_free(h->lang);
	sqlite3_free(h->source); sqlite3_free(h->trans);
}

static void hits_free(struct hits *hs)
{
	int k;
	for (k = 0; k < hs->n; k++) hit_free(&HIT(hs, k));
	sqlite3_free(hs->b.p);
	memset(hs, 0, sizeof *hs);
}

static int hit_from(struct hit *h, const struct cand *c, double score)
{
	h->id = dup(c->id); h->srclang = dup(c->srclang); h->lang = dup(c->lang);
	h->source = dup(c->source); h->trans = dup(c->trans);
	h->created = c->created;
	h->score = score;
	if (!h->id || !h->srclang || !h->lang || !h->source || !h->trans) { hit_free(h); return TMERR_NOMEMORY; }
	return TMERR_OK;
}

/* Poedit's AddOrUpdateResult: hits with the same translation keep the
 * best score; merge 0 keeps every hit */
static int add_hit(struct hits *hs, const struct cand *c, double score, int merge)
{
	struct hit h;
	int k;
	if (merge)
		for (k = 0; k < hs->n; k++)
			if (!strcmp(HIT(hs, k).trans, c->trans)) {
				if (score > HIT(hs, k).score) {
					if (hit_from(&h, c, score)) return TMERR_NOMEMORY;
					hit_free(&HIT(hs, k));
					HIT(hs, k) = h;
				}
				return TMERR_OK;
			}
	if (hit_from(&h, c, score)) return TMERR_NOMEMORY;
	buf_add(&hs->b, &h, sizeof h);
	if (hs->b.bad) { hit_free(&h); return TMERR_NOMEMORY; }
	hs->n++;
	return TMERR_OK;
}

/* Poedit's postprocess_results: below 0.3 out, best first (stable), max */
static void postprocess(struct hits *hs, unsigned long max)
{
	int k, j, n = 0;
	for (k = 0; k < hs->n; k++) {
		if (HIT(hs, k).score < SCORE_FLOOR) hit_free(&HIT(hs, k));
		else HIT(hs, n++) = HIT(hs, k);
	}
	hs->n = n;
	for (k = 1; k < n; k++) {
		struct hit t = HIT(hs, k);
		for (j = k; j > 0 && HIT(hs, j - 1).score < t.score; j--) HIT(hs, j) = HIT(hs, j - 1);
		HIT(hs, j) = t;
	}
	while ((unsigned long)hs->n > max) hit_free(&HIT(hs, --hs->n));
	hs->b.n = sizeof(struct hit) * (unsigned long)hs->n;
}

static void cands_free(struct cand *c, int n)
{
	int k;
	for (k = 0; k < n; k++) {
		sqlite3_free(c[k].id); sqlite3_free(c[k].srclang); sqlite3_free(c[k].lang);
		sqlite3_free(c[k].source); sqlite3_free(c[k].trans);
	}
	sqlite3_free(c);
}

/* the short form of a language: "cs" for "cs_CZ" and "sr@latin" */
static unsigned long short_len(const char *lang)
{
	unsigned long n = 0;
	while (lang[n] && lang[n] != '_' && lang[n] != '@') n++;
	return n;
}

/* entries in srclang and lang (or its short form) that have at least
 * `need` of the words, most words matched first, then fewest words */
static int candidates(struct tm_db *db, const char *srclang, const char *lang,
                      const struct term *terms, int nterms, int need,
                      struct cand **out, int *nout)
{
	sqlite3_str *q;
	sqlite3_stmt *st = 0;
	char *sql;
	struct buf cb;
	unsigned long sl = short_len(lang);
	int is_short = lang[sl] == 0, rc, k, n = 0;

	*out = 0; *nout = 0;
	if (!nterms) return SQLITE_OK;
	if (need > nterms) need = nterms;

	q = sqlite3_str_new(db->h);
	sqlite3_str_appendall(q,
		"SELECT e.uuid, e.srclang, e.lang, e.source, e.trans, e.created, e.ntok "
		"FROM entries e JOIN ("
		"  SELECT entry, COUNT(DISTINCT term) AS n FROM terms WHERE term IN (");
	for (k = 0; k < nterms; k++) sqlite3_str_appendf(q, k ? ",?%d" : "?%d", k + 4);
	sqlite3_str_appendall(q, ") GROUP BY entry HAVING n >= ?3) m ON m.entry = e.id "
	                         "WHERE e.srclang = ?1 AND (e.lang = ?2 OR ");
	if (is_short) sqlite3_str_appendall(q, "e.lang LIKE ?2 || '\\_%' ESCAPE '\\'");
	else sqlite3_str_appendf(q, "e.lang = ?%d", nterms + 4);
	sqlite3_str_appendf(q, ") ORDER BY m.n DESC, e.ntok ASC LIMIT %d", QUERY_MAX_DOCS);
	sql = sqlite3_str_finish(q);
	if (!sql) return SQLITE_NOMEM;
	rc = sqlite3_prepare_v2(db->h, sql, -1, &st, 0);
	sqlite3_free(sql);
	if (rc != SQLITE_OK) return rc;

	sqlite3_bind_text(st, 1, srclang, -1, SQLITE_STATIC);
	sqlite3_bind_text(st, 2, lang, -1, SQLITE_STATIC);
	sqlite3_bind_int64(st, 3, need);
	for (k = 0; k < nterms; k++) sqlite3_bind_text(st, k + 4, terms[k].w, (int)terms[k].n, SQLITE_STATIC);
	if (!is_short) sqlite3_bind_text(st, nterms + 4, lang, (int)sl, SQLITE_STATIC);

	memset(&cb, 0, sizeof cb);
	while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
		struct cand c;
		c.id = dup((const char *)sqlite3_column_text(st, 0));
		c.srclang = dup((const char *)sqlite3_column_text(st, 1));
		c.lang = dup((const char *)sqlite3_column_text(st, 2));
		c.source = dup((const char *)sqlite3_column_text(st, 3));
		c.trans = dup((const char *)sqlite3_column_text(st, 4));
		c.created = (long)sqlite3_column_int64(st, 5);
		c.ntok = (int)sqlite3_column_int64(st, 6);
		c.secondary = c.lang && strcmp(c.lang, lang) != 0;
		if (c.id && c.srclang && c.lang && c.source && c.trans) buf_add(&cb, &c, sizeof c);
		if (cb.bad || !c.id || !c.srclang || !c.lang || !c.source || !c.trans) {
			sqlite3_free(c.id); sqlite3_free(c.srclang); sqlite3_free(c.lang);
			sqlite3_free(c.source); sqlite3_free(c.trans);
			rc = SQLITE_NOMEM;
			break;
		}
		n++;
	}
	sqlite3_finalize(st);
	if (rc != SQLITE_DONE) { cands_free((struct cand *)cb.p, n); return rc; }
	*out = (struct cand *)cb.p;
	*nout = n;
	return SQLITE_OK;
}

/* code points in UTF-8 text (bad bytes count one each) */
static unsigned long char_count(const char *s)
{
	const tm_u8 *p = (const tm_u8 *)s;
	unsigned long n = strlen(s), i = 0, c = 0;
	while (i < n) { next_char(p, n, &i); c++; }
	return c;
}

/* does hay hold needle, case aside? (as boost's ifind_first) */
static int contains_nocase(const char *hay, const char *needle)
{
	const tm_u8 *h = (const tm_u8 *)hay, *nd = (const tm_u8 *)needle;
	unsigned long hn = strlen(hay), nn = strlen(needle), i = 0;
	if (!nn) return 1;
	while (i < hn) {
		unsigned long a = i, b = 0;
		for (;;) {
			tm_u32 x, y;
			if (b >= nn) return 1;
			if (a >= hn) break;
			x = next_char(h, hn, &a);
			y = next_char(nd, nn, &b);
			if (lower(x) != lower(y)) break;
		}
		next_char(h, hn, &i);
	}
	return 0;
}

enum pass_kind { PASS_PHRASE, PASS_TERMS, PASS_SUBSTRING };

struct pass {
	enum pass_kind kind;
	int need, slop;
	double threshold, scaling;
	int nterms;                     /* the query's word count (PASS_TERMS) */
	const char *phrase;             /* PASS_SUBSTRING */
};

static int perform_search(struct tm_db *db, const char *srclang, const char *lang,
                          const struct toks *q, const char *exact, const struct pass *p,
                          struct hits *out)
{
	struct term *seen = 0, *sorted = 0;
	struct cand *cands = 0;
	double *raw = 0, all_weight = 0, max_raw = 0, total;
	sqlite3_int64 n;
	int nterms, ncand = 0, rc, err = TMERR_OK, k, j, i;
	unsigned long exact_len = char_count(exact);

	if ((err = distinct_terms(q, &seen, &sorted, &nterms)) != TMERR_OK) return err;
	rc = candidates(db, srclang, lang, seen, nterms, p->need, &cands, &ncand);
	if (rc != SQLITE_OK || !ncand) goto out;

	/* word weights: idf = 1 + ln(N / (df + 1)) */
	rc = count_entries(db, &n);
	if (rc != SQLITE_OK) goto out;
	total = (double)(n > 1 ? n : 1);
	rc = prep(db, &db->st_df, "SELECT COUNT(DISTINCT entry) FROM terms WHERE term=?1");
	for (k = 0; rc == SQLITE_OK && k < nterms; k++) {
		sqlite3_bind_text(db->st_df, 1, sorted[k].w, (int)sorted[k].n, SQLITE_STATIC);
		rc = sqlite3_step(db->st_df);
		if (rc == SQLITE_ROW) {
			sorted[k].idf = 1.0 + log(total / (double)(sqlite3_column_int64(db->st_df, 0) + 1));
			rc = SQLITE_OK;
		}
		sqlite3_reset(db->st_df);
	}
	if (rc != SQLITE_OK) goto out;
	for (k = 0; k < nterms; k++) all_weight += sorted[k].idf * sorted[k].idf;

	raw = (double *)sqlite3_malloc64(sizeof(double) * (unsigned long)ncand);
	if (!raw) { err = TMERR_NOMEMORY; goto out; }
	for (i = 0; i < ncand; i++) {
		struct toks doc;
		double w = 0, coord, r;
		int matched = 0;
		raw[i] = -1;                    /* not a hit */
		if ((err = tokenize(cands[i].source, &doc)) != TMERR_OK) goto out;
		if (p->kind != PASS_TERMS && !matches_phrase(q, &doc, p->slop)) { toks_free(&doc); continue; }
		for (k = 0; k < nterms; k++)
			for (j = 0; j < doc.n; j++)
				if (TOK(&doc, j).len == sorted[k].n && !memcmp(TOKTEXT(&doc, j), sorted[k].w, sorted[k].n)) {
					w += sorted[k].idf * sorted[k].idf;
					matched++;
					break;
				}
		coord = nterms ? (double)matched / (double)nterms : 0;
		r = (all_weight > 0 ? w / all_weight : 0) * coord / sqrt((double)(doc.n > 1 ? doc.n : 1));
		if (cands[i].secondary) r *= SECONDARY_LANG_BOOST;
		raw[i] = r;
		if (r > max_raw) max_raw = r;
		toks_free(&doc);
	}
	if (max_raw <= 0) goto out;

	for (i = 0; i < ncand && err == TMERR_OK; i++) {
		double score;
		if (raw[i] < 0) continue;
		score = raw[i] / max_raw;
		if (score < p->threshold) continue;
		if (!strcmp(cands[i].source, exact)) {
			score = 1.0;
		} else {
			/* a large difference in length is penalized; tiny against huge goes */
			double len1 = (double)(exact_len ? exact_len : 1);
			unsigned long l2 = char_count(cands[i].source);
			double len2 = (double)(l2 ? l2 : 1), big = len1 > len2 ? len1 : len2, small = len1 > len2 ? len2 : len1;
			double lr, penalty;
			if (big > 3.0 * small) continue;
			lr = log(big / small);
			penalty = exp(-2.0 * lr * lr);
			score *= (penalty < 0.95 ? penalty : 0.95) * p->scaling;
		}
		switch (p->kind) {
		case PASS_PHRASE:
			err = add_hit(out, &cands[i], score, 1);
			break;
		case PASS_TERMS: {
			int d = cands[i].ntok - p->nterms;
			if (d < 0) d = -d;
			if (d <= MAX_ALLOWED_LENGTH_DIFFERENCE) err = add_hit(out, &cands[i], score, 1);
			break;
		}
		case PASS_SUBSTRING:
			if (contains_nocase(cands[i].source, p->phrase)) err = add_hit(out, &cands[i], score, 0);
			break;
		}
	}
out:
	sqlite3_free(raw);
	if (cands) cands_free(cands, ncand);
	sqlite3_free(seen);
	sqlite3_free(sorted);
	if (err == TMERR_OK && rc != SQLITE_OK && rc != SQLITE_DONE) err = map_rc(rc);
	return err;
}

/* the hits as one block the caller frees with tmc_free_results */
static int pack(struct hits *hs, struct tm_results **out)
{
	unsigned long size = sizeof(struct tm_results) + sizeof(struct tm_result) * (unsigned long)hs->n;
	struct tm_results *r;
	char *s;
	int k;
	for (k = 0; k < hs->n; k++) {
		struct hit *h = &HIT(hs, k);
		size += strlen(h->id) + strlen(h->srclang) + strlen(h->lang) + strlen(h->source) + strlen(h->trans) + 5;
	}
	r = (struct tm_results *)sqlite3_malloc64(size);
	if (!r) return TMERR_NOMEMORY;
	r->count = (unsigned long)hs->n;
	r->items = (struct tm_result *)(r + 1);
	s = (char *)(r->items + hs->n);
#define COPY(dst, src) do { unsigned long l_ = strlen(src) + 1; memcpy(s, src, l_); dst = s; s += l_; } while (0)
	for (k = 0; k < hs->n; k++) {
		struct hit *h = &HIT(hs, k);
		struct tm_result *o = &r->items[k];
		COPY(o->e.id, h->id);
		COPY(o->e.srclang, h->srclang);
		COPY(o->e.lang, h->lang);
		COPY(o->e.source, h->source);
		COPY(o->e.trans, h->trans);
		o->e.created = h->created;
		o->score = (long)(h->score * 1000000.0 + 0.5);
	}
#undef COPY
	*out = r;
	return TMERR_OK;
}

int tmc_search(struct tm_db *db, const char *srclang, const char *lang, const char *text,
               unsigned long max, struct tm_results **out)
{
	struct toks q;
	struct hits hs;
	struct pass p;
	int err, k, j, distinct;
	if (!db || !srclang || !lang || !text || !out) return TMERR_ARGS;
	*out = 0;
	if (!max) max = MAX_RESULTS;
	memset(&hs, 0, sizeof hs);
	if ((err = tokenize(text, &q)) != TMERR_OK) return done(db, err);
	memset(&p, 0, sizeof p);
	if (q.n) {
		/* the exact phrase first */
		p.kind = PASS_PHRASE; p.need = q.n; p.slop = 0; p.threshold = QUALITY_THRESHOLD; p.scaling = 1.0;
		err = perform_search(db, srclang, lang, &q, text, &p, &hs);
		if (err == TMERR_OK) postprocess(&hs, max);
		/* then, if nothing was found, a bit sloppy */
		if (err == TMERR_OK && !hs.n) {
			p.slop = 1; p.scaling = 0.8;
			err = perform_search(db, srclang, lang, &q, text, &p, &hs);
			if (err == TMERR_OK) postprocess(&hs, max);
		}
		/* as the last resort, by words */
		if (err == TMERR_OK && !hs.n) {
			for (distinct = 0, k = 0; k < q.n; k++) {
				for (j = 0; j < k && !tok_eq(&q, j, &q, k); j++)
					;
				if (j == k) distinct++;
			}
			p.kind = PASS_TERMS; p.slop = 0; p.scaling = 0.7; p.nterms = q.n;
			p.need = distinct - MAX_ALLOWED_LENGTH_DIFFERENCE;
			if (p.need < 1) p.need = 1;
			err = perform_search(db, srclang, lang, &q, text, &p, &hs);
			if (err == TMERR_OK) postprocess(&hs, max);
		}
	}
	if (err == TMERR_OK) err = pack(&hs, out);
	hits_free(&hs);
	toks_free(&q);
	return done(db, err);
}

int tmc_search_substring(struct tm_db *db, const char *srclang, const char *lang,
                         const char *text, unsigned long max, struct tm_results **out)
{
	struct toks q;
	struct hits hs;
	struct pass p;
	int err;
	if (!db || !srclang || !lang || !text || !out) return TMERR_ARGS;
	*out = 0;
	memset(&hs, 0, sizeof hs);
	if ((err = tokenize(text, &q)) != TMERR_OK) return done(db, err);
	if (q.n) {
		memset(&p, 0, sizeof p);
		p.kind = PASS_SUBSTRING; p.need = q.n; p.slop = 0; p.threshold = 0.0; p.scaling = 1.0;
		p.phrase = text;
		err = perform_search(db, srclang, lang, &q, text, &p, &hs);
		while (err == TMERR_OK && max && (unsigned long)hs.n > max) hit_free(&HIT(&hs, --hs.n));
		hs.b.n = sizeof(struct hit) * (unsigned long)hs.n;
	}
	if (err == TMERR_OK) err = pack(&hs, out);
	hits_free(&hs);
	toks_free(&q);
	return done(db, err);
}

void tmc_free_results(struct tm_results *r)
{
	sqlite3_free(r);
}

int tmc_foreach(struct tm_db *db, tm_each_fn fn, void *ud)
{
	sqlite3_stmt *st = 0;
	int rc;
	if (!db || !fn) return TMERR_ARGS;
	rc = sqlite3_prepare_v2(db->h, "SELECT uuid, srclang, lang, source, trans, created FROM entries ORDER BY id",
	                        -1, &st, 0);
	if (rc != SQLITE_OK) return fail(db, rc);
	while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
		struct tm_entry e;
		e.id = (const char *)sqlite3_column_text(st, 0);
		e.srclang = (const char *)sqlite3_column_text(st, 1);
		e.lang = (const char *)sqlite3_column_text(st, 2);
		e.source = (const char *)sqlite3_column_text(st, 3);
		e.trans = (const char *)sqlite3_column_text(st, 4);
		e.created = (long)sqlite3_column_int64(st, 5);
		if (fn(ud, &e)) { rc = SQLITE_DONE; break; }
	}
	sqlite3_finalize(st);
	return fail(db, rc);
}

int tmc_stats(struct tm_db *db, struct tm_stats *s)
{
	sqlite3_stmt *st = 0;
	sqlite3_int64 n;
	int rc;
	if (!db || !s) return TMERR_ARGS;
	rc = count_entries(db, &n);
	if (rc != SQLITE_OK) return fail(db, rc);
	s->entries = (unsigned long)n;
	s->filesize = 0;
	rc = sqlite3_prepare_v2(db->h, "SELECT page_count * page_size FROM pragma_page_count(), pragma_page_size()",
	                        -1, &st, 0);
	if (rc == SQLITE_OK && (rc = sqlite3_step(st)) == SQLITE_ROW) {
		s->filesize = (unsigned long)sqlite3_column_int64(st, 0);
		rc = SQLITE_OK;
	}
	sqlite3_finalize(st);
	return fail(db, rc);
}

const char *tmc_error_text(struct tm_db *db, int code)
{
	if (db && code == TMERR_DB && db->h) return sqlite3_errmsg(db->h);
	switch (code) {
	case TMERR_OK: return "no error";
	case TMERR_NOMEMORY: return "not enough memory";
	case TMERR_ARGS: return "bad arguments";
	case TMERR_CANTOPEN: return "the translation memory can't be opened";
	case TMERR_CORRUPT: return "the translation memory is damaged, or not a translation memory";
	case TMERR_BUSY: return "the translation memory is in use";
	case TMERR_IO: return "a read or write failed";
	case TMERR_FULL: return "the disk is full";
	case TMERR_NOCRYPTO: return "gfopencrypto.library 2.2 (SHA-1) is needed";
	default: return db && db->h ? sqlite3_errmsg(db->h) : "database error";
	}
}

int tmc_words(const char *text, void (*fn)(void *ud, const char *w, unsigned long n, int pos), void *ud)
{
	struct toks ts;
	int k, err;
	if (!text || !fn) return TMERR_ARGS;
	if ((err = tokenize(text, &ts)) != TMERR_OK) return err;
	for (k = 0; k < ts.n; k++) fn(ud, TOKTEXT(&ts, k), TOK(&ts, k).len, TOK(&ts, k).pos);
	toks_free(&ts);
	return TMERR_OK;
}

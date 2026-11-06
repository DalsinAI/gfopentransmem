/* GFOpenTransMem test driver: one command per line on standard input,
 * fields split by tabs, answers on standard output. The same commands run
 * on the PC (the core with the PC's SQLite VFS, tests/run_host_tests.sh)
 * and on the Amiga (through transmem.library, TMTest: tests/tm_test.c).
 *   open NAME | close | insert SRC LANG SOURCE TRANS [CREATED] | commit |
 *   rollback | search SRC LANG TEXT [MAX] | sub SRC LANG TEXT | delete ID |
 *   deleteall | stats | export | words TEXT | id SRC LANG SOURCE TRANS |
 *   deletefirst SRC LANG TEXT (deletes the best hit, as Poedit's Delete)
 * MIT, Copyright (c) 2026 AmiCradle Team. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef TM_AMIGA
#include <proto/exec.h>
#include <proto/transmem.h>
#include <libraries/transmem.h>
#else
#include "../src/tm_core.h"
#endif

#ifdef TM_AMIGA
/* TransMemBase: opened by libtransmem.a */
#define DB struct TMDatabase
#define T_OPEN(n, e) TM_OpenDatabase((CONST_STRPTR)(n), (e))
#define T_CLOSE(d) TM_CloseDatabase(d)
#define T_ERR(d, c) ((const char *)TM_ErrorText((d), (c)))
typedef struct TMResults RESULTS;
#define R_N(r) ((r)->tmrs_Count)
#define R_TRANS(r, k) ((const char *)(r)->tmrs_Results[k].tmr_Entry.tme_Translation)
#define R_SRC(r, k) ((const char *)(r)->tmrs_Results[k].tmr_Entry.tme_Source)
#define R_LANG(r, k) ((const char *)(r)->tmrs_Results[k].tmr_Entry.tme_Lang)
#define R_ID(r, k) ((const char *)(r)->tmrs_Results[k].tmr_Entry.tme_Id)
#define R_SCORE(r, k) ((r)->tmrs_Results[k].tmr_Score)
#else
#define DB struct tm_db
typedef struct tm_results RESULTS;
#define R_N(r) ((r)->count)
#define R_TRANS(r, k) ((r)->items[k].e.trans)
#define R_SRC(r, k) ((r)->items[k].e.source)
#define R_LANG(r, k) ((r)->items[k].e.lang)
#define R_ID(r, k) ((r)->items[k].e.id)
#define R_SCORE(r, k) ((r)->items[k].score)
#endif

static DB *db;
static int show_ids = 1;

static char *field[8];
static int nfield;

static void split(char *s)
{
	nfield = 0;
	while (nfield < 8) {
		char *t = strchr(s, '\t');
		field[nfield++] = s;
		if (!t) break;
		*t = 0;
		s = t + 1;
	}
}

static const char *f(int k) { return k < nfield ? field[k] : ""; }

static void show_results(RESULTS *r, int scored)
{
	unsigned long k;
	for (k = 0; k < R_N(r); k++) {
		if (scored) printf("   %ld.%06ld", (long)R_SCORE(r, k) / 1000000, (long)R_SCORE(r, k) % 1000000);
		else printf("   sub:");
		printf("  %s -> %s [%s]", R_SRC(r, k), R_TRANS(r, k), R_LANG(r, k));
		if (show_ids) printf(" {%s}", R_ID(r, k));
		printf("\n");
	}
}

static void err(const char *what, long e)
{
	if (!e) return;
#ifdef TM_AMIGA
	printf("%s: error %ld (%s)\n", what, e, T_ERR(db, e));
#else
	printf("%s: error %ld (%s)\n", what, e, tmc_error_text(db, (int)e));
#endif
}

#ifdef TM_AMIGA
#define REG(r, decl) register decl __asm(#r)
static ULONG each(REG(a0, struct Hook *h), REG(a2, struct TMDatabase *d), REG(a1, struct TMEntry *e))
{
	(void)d;
	(*(long *)h->h_Data)++;
	printf("   %s\t%s\t%s\t%s\t%ld\t%s\n", (const char *)e->tme_SourceLang, (const char *)e->tme_Lang,
	       (const char *)e->tme_Source, (const char *)e->tme_Translation, (long)e->tme_Created, (const char *)e->tme_Id);
	return 0;
}
#else
static int each(void *ud, const struct tm_entry *e)
{
	(*(long *)ud)++;
	printf("   %s\t%s\t%s\t%s\t%ld\t%s\n", e->srclang, e->lang, e->source, e->trans, e->created, e->id);
	return 0;
}

static void word(void *ud, const char *w, unsigned long n, int pos)
{
	(void)ud;
	printf(" %.*s@%d", (int)n, w, pos);
}
#endif

static void command(char *line)
{
	const char *c;
	long e = 0;
	split(line);
	c = f(0);
	if (!*c || *c == '#') return;
	if (!strcmp(c, "echo")) { printf("%s\n", f(1)); return; }
	if (!strcmp(c, "ids")) { show_ids = atoi(f(1)); return; }
	if (!strcmp(c, "open")) {
#ifdef TM_AMIGA
		LONG le = 0;
		db = T_OPEN(f(1), &le);
		e = le;
#else
		e = tmc_open(f(1), 0, &db);
#endif
		printf("open: %s\n", db ? "ok" : "failed");
		err("open", e);
		return;
	}
	if (!db) { printf("%s: no database\n", c); return; }
	if (!strcmp(c, "close")) {
#ifdef TM_AMIGA
		T_CLOSE(db);
#else
		tmc_close(db);
#endif
		db = 0;
		printf("closed\n");
	} else if (!strcmp(c, "insert")) {
#ifdef TM_AMIGA
		struct TMEntry en;
		en.tme_SourceLang = (CONST_STRPTR)f(1); en.tme_Lang = (CONST_STRPTR)f(2);
		en.tme_Source = (CONST_STRPTR)f(3); en.tme_Translation = (CONST_STRPTR)f(4);
		en.tme_Created = atol(f(5)); en.tme_Id = 0;
		e = TM_Insert(db, &en);
#else
		struct tm_entry en;
		en.srclang = f(1); en.lang = f(2); en.source = f(3); en.trans = f(4);
		en.created = atol(f(5)); en.id = 0;
		e = tmc_insert(db, &en);
#endif
		err("insert", e);
	} else if (!strcmp(c, "commit")) {
#ifdef TM_AMIGA
		e = TM_Commit(db);
#else
		e = tmc_commit(db);
#endif
		err("commit", e);
	} else if (!strcmp(c, "rollback")) {
#ifdef TM_AMIGA
		e = TM_Rollback(db);
#else
		e = tmc_rollback(db);
#endif
		err("rollback", e);
	} else if (!strcmp(c, "search") || !strcmp(c, "sub") || !strcmp(c, "deletefirst")) {
		RESULTS *r = 0;
		int sub = !strcmp(c, "sub");
#ifdef TM_AMIGA
		struct TMQuery q;
		q.tmq_SourceLang = (CONST_STRPTR)f(1); q.tmq_Lang = (CONST_STRPTR)f(2);
		q.tmq_Text = (CONST_STRPTR)f(3); q.tmq_Max = (ULONG)atol(f(4));
		e = sub ? TM_SearchSubstring(db, &q, &r) : TM_Search(db, &q, &r);
#else
		e = sub ? tmc_search_substring(db, f(1), f(2), f(3), (unsigned long)atol(f(4)), &r)
		        : tmc_search(db, f(1), f(2), f(3), (unsigned long)atol(f(4)), &r);
#endif
		if (!strcmp(c, "deletefirst")) {
			if (r && R_N(r)) {
#ifdef TM_AMIGA
				e = TM_Delete(db, (CONST_STRPTR)R_ID(r, 0));
				if (!e) e = TM_Commit(db);
#else
				e = tmc_delete(db, R_ID(r, 0));
				if (!e) e = tmc_commit(db);
#endif
			}
		} else {
			printf("Q: %s\n", f(3));
			if (r) show_results(r, !sub);
		}
#ifdef TM_AMIGA
		if (r) TM_FreeResults(r);
#else
		if (r) tmc_free_results(r);
#endif
		err(c, e);
	} else if (!strcmp(c, "delete")) {
#ifdef TM_AMIGA
		e = TM_Delete(db, (CONST_STRPTR)f(1));
#else
		e = tmc_delete(db, f(1));
#endif
		err("delete", e);
	} else if (!strcmp(c, "deleteall")) {
#ifdef TM_AMIGA
		e = TM_DeleteAll(db);
#else
		e = tmc_delete_all(db);
#endif
		err("deleteall", e);
	} else if (!strcmp(c, "stats")) {
#ifdef TM_AMIGA
		struct TMStats s;
		e = TM_GetStats(db, &s);
		printf("entries %lu%s\n", (unsigned long)s.tms_Entries, s.tms_FileSize ? ", file size > 0" : "");
#else
		struct tm_stats s;
		e = tmc_stats(db, &s);
		printf("entries %lu%s\n", s.entries, s.filesize ? ", file size > 0" : "");
#endif
		err("stats", e);
	} else if (!strcmp(c, "export")) {
		long n = 0;
#ifdef TM_AMIGA
		struct Hook h;
		memset(&h, 0, sizeof h);
		h.h_Entry = (HOOKFUNC)(APTR)each;
		h.h_Data = &n;
		e = TM_ForEach(db, &h);
#else
		e = tmc_foreach(db, each, &n);
#endif
		printf("export %ld\n", n);
		err("export", e);
	} else if (!strcmp(c, "words")) {
#ifndef TM_AMIGA
		printf("words:");
		tmc_words(f(1), word, 0);
		printf("\n");
#endif
	} else if (!strcmp(c, "id")) {
		char id[37];
#ifdef TM_AMIGA
		struct TMEntry en;
		en.tme_SourceLang = (CONST_STRPTR)f(1); en.tme_Lang = (CONST_STRPTR)f(2);
		en.tme_Source = (CONST_STRPTR)f(3); en.tme_Translation = (CONST_STRPTR)f(4);
		e = TM_MakeId(&en, (STRPTR)id);
#else
		struct tm_entry en;
		en.srclang = f(1); en.lang = f(2); en.source = f(3); en.trans = f(4);
		e = tmc_make_id(&en, id);
#endif
		if (!e) printf("id %s\n", id);
		err("id", e);
	} else {
		printf("%s: unknown command\n", c);
	}
}

int main(int argc, char **argv)
{
	static char line[4096];
	FILE *in = stdin;
#ifdef TM_AMIGA
	if (!TransMemBase) {
		printf("TMTest: can't open transmem.library 1\n");
		return 20;
	}
#endif
	if (argc > 1 && !(in = fopen(argv[1], "r"))) {
		printf("can't open %s\n", argv[1]);
		return 20;
	}
	while (fgets(line, sizeof line, in)) {
		size_t n = strlen(line);
		while (n && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = 0;
		command(line);
	}
	if (db) {
#ifdef TM_AMIGA
		T_CLOSE(db);
#else
		tmc_close(db);
#endif
	}
	return 0;
}

/* TransMem: a translation memory from the shell, through transmem.library.
 *   TransMem DB=Work:tm.sqlite STATS
 *   TransMem DB=... SRC=en LANG=cs SEARCH "Open file"     best matches, scored
 *   TransMem DB=... SRC=en LANG=cs FIND file              entries holding the text
 *   TransMem DB=... SRC=en LANG=cs SOURCE="Open file" TRANS="Otevřít soubor" ADD
 *   TransMem DB=... EXPORT=RAM:tm.txt                     every entry, one a line
 *   TransMem DB=... IMPORT=RAM:tm.txt                     lines as EXPORT writes them
 *   TransMem DB=... CLEAR                                 delete every entry
 * Lines of EXPORT and IMPORT files: srclang, lang, source, translation,
 * created (seconds since 1970), split by tabs, in UTF-8. Text on the
 * command line and in the shell is ISO 8859-1 (the Amiga's own set) unless
 * UTF8 is given. Poedit keeps its memory in PROGDIR:... TranslationMemory/tm.sqlite.
 * MIT, Copyright (c) 2026 AmiCradle Team. */
#include <exec/types.h>
#include <exec/memory.h>
#include <dos/dos.h>
#include <dos/rdargs.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/transmem.h>
#include <string.h>

static const char version[] __attribute__((used)) = "$VER: TransMem 1.0 (06.11.2026) GFOpenTransMem, AmiCradle Team";

#define TEMPLATE "DB/A,SRC/K,LANG/K,SEARCH/K,FIND/K,SOURCE/K,TRANS/K,ADD/S,STATS/S,EXPORT/K,IMPORT/K,CLEAR/S,MAX/K/N,UTF8/S"
enum { A_DB, A_SRC, A_LANG, A_SEARCH, A_FIND, A_SOURCE, A_TRANS, A_ADD, A_STATS, A_EXPORT, A_IMPORT,
       A_CLEAR, A_MAX, A_UTF8, A_COUNT };

static int utf8_io;

/* ISO 8859-1 to UTF-8 (a new string, FreeVec it), or a copy when UTF8 */
static STRPTR to_utf8(CONST_STRPTR s)
{
	ULONG n = 0, k = 0;
	CONST_STRPTR p;
	STRPTR o;
	if (!s) return 0;
	for (p = s; *p; p++) n += (*p & 0x80) && !utf8_io ? 2 : 1;
	if (!(o = (STRPTR)AllocVec(n + 1, MEMF_ANY))) return 0;
	for (p = s; *p; p++) {
		UBYTE c = *p;
		if ((c & 0x80) && !utf8_io) { o[k++] = (UBYTE)(0xC0 | (c >> 6)); o[k++] = (UBYTE)(0x80 | (c & 0x3F)); }
		else o[k++] = c;
	}
	o[k] = 0;
	return o;
}

/* UTF-8 to the shell: ISO 8859-1, '?' for what it has not */
static void out(CONST_STRPTR s)
{
	UBYTE buf[256];
	ULONG k = 0;
	const UBYTE *p = (const UBYTE *)s;
	if (utf8_io) { PutStr(s); return; }
	while (*p) {
		ULONG c = *p++;
		if (c >= 0xC0) {
			int more = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : 1;
			c &= 0x3F >> more;
			while (more-- && (*p & 0xC0) == 0x80) c = (c << 6) | (*p++ & 0x3F);
			if (c > 0xFF) c = '?';
		} else if (c >= 0x80) c = '?';
		buf[k++] = (UBYTE)c;
		if (k == sizeof buf - 1) { buf[k] = 0; PutStr(buf); k = 0; }
	}
	buf[k] = 0;
	PutStr(buf);
}

static void score(LONG s)
{
	LONG v[2];
	v[0] = s / 10000;
	v[1] = s % 10000 / 100;
	VPrintf((CONST_STRPTR)"%3ld.%02ld%%  ", v);
}

static void fail(struct TMDatabase *db, CONST_STRPTR what, LONG err)
{
	PutStr((CONST_STRPTR)"TransMem: ");
	PutStr(what);
	PutStr((CONST_STRPTR)": ");
	PutStr(TM_ErrorText(db, err));
	PutStr((CONST_STRPTR)"\n");
}

#define REG(r, decl) register decl __asm(#r)

/* a hook: a0 the hook, a2 the database, a1 the entry */
static ULONG export_line(REG(a0, struct Hook *h), REG(a2, struct TMDatabase *db), REG(a1, struct TMEntry *e))
{
	BPTR fh = (BPTR)h->h_Data;
	LONG v[1];
	(void)db;
	FPuts(fh, e->tme_SourceLang); FPutC(fh, '\t');
	FPuts(fh, e->tme_Lang); FPutC(fh, '\t');
	FPuts(fh, e->tme_Source); FPutC(fh, '\t');
	FPuts(fh, e->tme_Translation); FPutC(fh, '\t');
	v[0] = e->tme_Created;
	VFPrintf(fh, (CONST_STRPTR)"%ld\n", v);
	return 0;
}

/* one line of an EXPORT file into its five fields; 0 if it has fewer than four */
static int fields(STRPTR line, STRPTR f[5])
{
	int n = 0;
	STRPTR p = line;
	ULONG len = strlen((char *)line);
	while (len && (line[len - 1] == '\n' || line[len - 1] == '\r')) line[--len] = 0;
	f[4] = (STRPTR)"0";
	while (n < 5) {
		f[n++] = p;
		while (*p && *p != '\t') p++;
		if (!*p) break;
		*p++ = 0;
	}
	return n >= 4;
}

static LONG atol_(CONST_STRPTR s)
{
	LONG v = 0;
	StrToLong(s, &v);
	return v;
}

int main(void)
{
	LONG a[A_COUNT];
	struct RDArgs *rd;
	struct TMDatabase *db;
	LONG err = 0, rc = RETURN_OK;
	STRPTR src = 0, lang = 0, text = 0, s2 = 0;

	memset(a, 0, sizeof a);
	if (!TransMemBase) {
		PutStr((CONST_STRPTR)"TransMem: needs transmem.library 1 in LIBS:\n");
		return RETURN_FAIL;
	}
	if (!(rd = ReadArgs((CONST_STRPTR)TEMPLATE, a, 0))) {
		PrintFault(IoErr(), (CONST_STRPTR)"TransMem");
		return RETURN_ERROR;
	}
	utf8_io = a[A_UTF8] != 0;
	if (!(db = TM_OpenDatabase((CONST_STRPTR)a[A_DB], &err))) {
		fail(0, (CONST_STRPTR)a[A_DB], err);
		FreeArgs(rd);
		return RETURN_ERROR;
	}
	src = to_utf8((CONST_STRPTR)(a[A_SRC] ? a[A_SRC] : (LONG)"en"));
	lang = to_utf8((CONST_STRPTR)a[A_LANG]);

	if (a[A_SEARCH] || a[A_FIND]) {
		struct TMQuery q;
		struct TMResults *r = 0;
		ULONG k;
		if (!lang) { PutStr((CONST_STRPTR)"TransMem: SEARCH and FIND need LANG\n"); rc = RETURN_ERROR; goto done; }
		text = to_utf8((CONST_STRPTR)(a[A_SEARCH] ? a[A_SEARCH] : a[A_FIND]));
		q.tmq_SourceLang = src; q.tmq_Lang = lang; q.tmq_Text = text;
		q.tmq_Max = a[A_MAX] ? *(ULONG *)a[A_MAX] : 0;
		err = a[A_SEARCH] ? TM_Search(db, &q, &r) : TM_SearchSubstring(db, &q, &r);
		if (err) { fail(db, (CONST_STRPTR)"search", err); rc = RETURN_ERROR; goto done; }
		for (k = 0; k < r->tmrs_Count; k++) {
			struct TMResult *x = &r->tmrs_Results[k];
			if (a[A_SEARCH]) score(x->tmr_Score);
			out(x->tmr_Entry.tme_Translation);
			PutStr((CONST_STRPTR)"  (");
			out(x->tmr_Entry.tme_Source);
			PutStr((CONST_STRPTR)", ");
			out(x->tmr_Entry.tme_Lang);
			PutStr((CONST_STRPTR)")\n");
		}
		if (!r->tmrs_Count) { PutStr((CONST_STRPTR)"no match\n"); rc = RETURN_WARN; }
		TM_FreeResults(r);
	} else if (a[A_ADD]) {
		struct TMEntry e;
		if (!lang || !a[A_SOURCE] || !a[A_TRANS]) {
			PutStr((CONST_STRPTR)"TransMem: ADD needs LANG, SOURCE and TRANS\n"); rc = RETURN_ERROR; goto done;
		}
		text = to_utf8((CONST_STRPTR)a[A_SOURCE]);
		s2 = to_utf8((CONST_STRPTR)a[A_TRANS]);
		e.tme_SourceLang = src; e.tme_Lang = lang; e.tme_Source = text; e.tme_Translation = s2;
		e.tme_Created = 0; e.tme_Id = 0;
		err = TM_Insert(db, &e);
		if (!err) err = TM_Commit(db);
		if (err) { fail(db, (CONST_STRPTR)"add", err); rc = RETURN_ERROR; }
	} else if (a[A_EXPORT]) {
		struct Hook h;
		BPTR fh = Open((CONST_STRPTR)a[A_EXPORT], MODE_NEWFILE);
		if (!fh) { PrintFault(IoErr(), (CONST_STRPTR)a[A_EXPORT]); rc = RETURN_ERROR; goto done; }
		memset(&h, 0, sizeof h);
		h.h_Entry = (HOOKFUNC)(APTR)export_line;
		h.h_Data = (APTR)fh;
		err = TM_ForEach(db, &h);
		Close(fh);
		if (err) { fail(db, (CONST_STRPTR)"export", err); rc = RETURN_ERROR; }
	} else if (a[A_IMPORT]) {
		static UBYTE line[8192];
		STRPTR f[5];
		LONG n = 0, v[1];
		BPTR fh = Open((CONST_STRPTR)a[A_IMPORT], MODE_OLDFILE);
		if (!fh) { PrintFault(IoErr(), (CONST_STRPTR)a[A_IMPORT]); rc = RETURN_ERROR; goto done; }
		while (!err && FGets(fh, line, sizeof line)) {
			struct TMEntry e;
			if (!fields(line, f)) continue;
			e.tme_SourceLang = f[0]; e.tme_Lang = f[1]; e.tme_Source = f[2]; e.tme_Translation = f[3];
			e.tme_Created = atol_(f[4]); e.tme_Id = 0;
			if (!(err = TM_Insert(db, &e))) n++;
		}
		Close(fh);
		if (!err) err = TM_Commit(db);
		else TM_Rollback(db);
		if (err) { fail(db, (CONST_STRPTR)"import", err); rc = RETURN_ERROR; }
		else { v[0] = n; VPrintf((CONST_STRPTR)"%ld entries imported\n", v); }
	} else if (a[A_CLEAR]) {
		err = TM_DeleteAll(db);
		if (!err) err = TM_Commit(db);
		if (err) { fail(db, (CONST_STRPTR)"clear", err); rc = RETURN_ERROR; }
	} else {
		struct TMStats st;
		LONG v[2];
		err = TM_GetStats(db, &st);
		if (err) { fail(db, (CONST_STRPTR)"stats", err); rc = RETURN_ERROR; goto done; }
		v[0] = st.tms_Entries;
		v[1] = st.tms_FileSize;
		VPrintf((CONST_STRPTR)"%ld entries, %ld bytes\n", v);
	}
done:
	TM_CloseDatabase(db);
	FreeVec(src); FreeVec(lang); FreeVec(text); FreeVec(s2);
	FreeArgs(rd);
	return rc;
}

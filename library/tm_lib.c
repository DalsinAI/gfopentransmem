/* transmem.library 1.0 (GFOpenTransMem): a translation memory on SQLite.
 * docs/transmem.library.md is the contract; the calls are src/tm_core.c
 * behind the entry points below, in include/sfd/transmem_lib.sfd's order.
 *   - SQLite is built in (gfopensqlite's version), with its files on
 *     dos.library and its memory and mutexes on exec (tm_amiga.c).
 *   - Each database has a SignalSemaphore: any number of processes may
 *     use one at once. Callers must be processes (dos.library does the
 *     file work on the caller's behalf).
 *   - A call that reaches SQLite runs on a stack of its own when the
 *     caller has less than TM_STACK_NEEDED free, so a 4 KB shell stack
 *     is enough.
 *   - Floating point (the scores, SQLite's REAL) is soft float through
 *     mathieeedoubbas/doubtrans, with fpuauto: the FPU when there is one.
 *   - A program that ends without closing its databases has them closed
 *     when it closes the library.
 * MIT, Copyright (c) 2026 AmiCradle Team. */
#include <exec/types.h>
#include <exec/resident.h>
#include <exec/libraries.h>
#include <exec/execbase.h>
#include <exec/memory.h>
#include <exec/semaphores.h>
#include <exec/tasks.h>
#include <dos/dos.h>
#include <dos/dosextens.h>
#include <utility/hooks.h>
#include <proto/exec.h>
#include <libraries/transmem.h>
#include "sqlite3.h"
#include "../src/tm_core.h"
#include "tm_amiga.h"

#define REG(r, decl) register decl __asm(#r)
#define LIB_VERSION 1
#define LIB_REVISION 0
#define TM_STACK_NEEDED 49152
#define TM_STACK_SIZE 98304

struct TMLibBase {
	struct Library lib;
	BPTR seglist;
};

struct ExecBase *SysBase;
struct DosLibrary *DOSBase;
struct Library *MathIeeeDoubBasBase, *MathIeeeDoubTransBase;
struct Library *MathIeeeSingBasBase, *MathIeeeSingTransBase;

static struct SignalSemaphore lib_sem;  /* the database list, the maths libraries, SQLite's start */
static struct MinList databases;
static int sqlite_ready;

struct TMDatabase {
	struct MinNode node;
	struct SignalSemaphore sem;
	struct Task *owner;             /* the task that opened it */
	struct tm_db *db;
};

extern LONG tm_onstack(LONG (*fn)(APTR), APTR arg, struct StackSwapStruct *sss);
extern ULONG tm_callhook(struct Hook *hook, APTR object, APTR message);

/* Run as a program, the library does nothing. This must stay the first code
 * in the file (-fno-toplevel-reorder). */
int start(void) { return -1; }

static const char lib_name[] = TRANSMEMNAME;
static const char lib_id[] =
	"transmem.library 1.0 (06.11.2026) GFOpenTransMem, AmiCradle Team\r\n";
static const char lib_ver[] __attribute__((used)) =
	"$VER: transmem.library 1.0 (06.11.2026) GFOpenTransMem, AmiCradle Team";

/* ---- A call on enough stack -------------------------------------------- */

static LONG run(LONG (*fn)(APTR), APTR arg)
{
	struct Task *me = FindTask(0);
	struct StackSwapStruct sss;
	ULONG here = (ULONG)&sss;
	UBYTE *stack;
	LONG r;
	if (here > (ULONG)me->tc_SPLower && here - (ULONG)me->tc_SPLower >= TM_STACK_NEEDED)
		return fn(arg);
	if (!(stack = (UBYTE *)AllocMem(TM_STACK_SIZE, MEMF_ANY)))
		return TMERR_NOMEMORY;
	sss.stk_Lower = stack;
	sss.stk_Upper = (ULONG)(stack + TM_STACK_SIZE);
	sss.stk_Pointer = (APTR)(stack + TM_STACK_SIZE);
	r = tm_onstack(fn, arg, &sss);
	FreeMem(stack, TM_STACK_SIZE);
	return r;
}

static int start_sqlite(void)
{
	int rc = SQLITE_OK;
	ObtainSemaphore(&lib_sem);
	if (!sqlite_ready) {
		rc = tm_amiga_config();
		if (rc == SQLITE_OK || rc == SQLITE_MISUSE) rc = sqlite3_initialize();
		if (rc == SQLITE_OK) sqlite_ready = 1;
	}
	ReleaseSemaphore(&lib_sem);
	return rc;
}

/* ---- The calls --------------------------------------------------------- */

struct args {
	struct TMDatabase *tdb;
	const void *a, *b;
	void *c;
	LONG n;
	LONG *err;
};

static LONG do_open(APTR v)
{
	struct args *x = (struct args *)v;
	struct TMDatabase *t;
	LONG err;
	if (start_sqlite() != SQLITE_OK) return TMERR_DB;
	if (!(t = (struct TMDatabase *)AllocMem(sizeof *t, MEMF_ANY | MEMF_CLEAR))) return TMERR_NOMEMORY;
	InitSemaphore(&t->sem);
	t->owner = FindTask(0);
	err = tmc_open((const char *)x->a, 0, &t->db);
	if (err) { FreeMem(t, sizeof *t); return err; }
	ObtainSemaphore(&lib_sem);
	AddTail((struct List *)&databases, (struct Node *)&t->node);
	ReleaseSemaphore(&lib_sem);
	x->tdb = t;
	return TMERR_OK;
}

static struct TMDatabase *L_OpenDatabase(REG(a0, CONST_STRPTR name), REG(a1, LONG *error))
{
	struct args x;
	LONG err;
	x.tdb = 0;
	x.a = name;
	err = name ? run(do_open, &x) : TMERR_ARGS;
	if (error) *error = err;
	return err ? 0 : x.tdb;
}

static LONG do_close(APTR v)
{
	struct TMDatabase *t = (struct TMDatabase *)v;
	ObtainSemaphore(&t->sem);
	tmc_close(t->db);
	t->db = 0;
	ReleaseSemaphore(&t->sem);
	return 0;
}

static void close_db(struct TMDatabase *t)
{
	ObtainSemaphore(&lib_sem);
	Remove((struct Node *)&t->node);
	ReleaseSemaphore(&lib_sem);
	run(do_close, t);
	FreeMem(t, sizeof *t);
}

static VOID L_CloseDatabase(REG(a0, struct TMDatabase *db))
{
	if (db) close_db(db);
}

enum op { OP_INSERT, OP_DELETE, OP_DELETEALL, OP_COMMIT, OP_ROLLBACK, OP_SEARCH, OP_SUBSTRING,
          OP_FOREACH, OP_STATS };

struct call { struct TMDatabase *t; enum op op; const void *a; void *b; };

static int each(void *ud, const struct tm_entry *e)
{
	struct call *c = (struct call *)ud;
	return tm_callhook((struct Hook *)c->a, c->t, (APTR)e) != 0;
}

static LONG do_call(APTR v)
{
	struct call *c = (struct call *)v;
	struct tm_db *db = c->t->db;
	const struct TMQuery *q = (const struct TMQuery *)c->a;
	LONG r;
	ObtainSemaphore(&c->t->sem);
	switch (c->op) {
	case OP_INSERT: r = tmc_insert(db, (const struct tm_entry *)c->a); break;
	case OP_DELETE: r = tmc_delete(db, (const char *)c->a); break;
	case OP_DELETEALL: r = tmc_delete_all(db); break;
	case OP_COMMIT: r = tmc_commit(db); break;
	case OP_ROLLBACK: r = tmc_rollback(db); break;
	case OP_SEARCH:
		r = tmc_search(db, (const char *)q->tmq_SourceLang, (const char *)q->tmq_Lang,
		               (const char *)q->tmq_Text, q->tmq_Max, (struct tm_results **)c->b);
		break;
	case OP_SUBSTRING:
		r = tmc_search_substring(db, (const char *)q->tmq_SourceLang, (const char *)q->tmq_Lang,
		                         (const char *)q->tmq_Text, q->tmq_Max, (struct tm_results **)c->b);
		break;
	case OP_FOREACH: r = tmc_foreach(db, each, c); break;
	case OP_STATS: r = tmc_stats(db, (struct tm_stats *)c->b); break;
	default: r = TMERR_ARGS;
	}
	ReleaseSemaphore(&c->t->sem);
	return r;
}

static LONG call(struct TMDatabase *t, enum op op, const void *a, void *b)
{
	struct call c;
	if (!t || !t->db) return TMERR_ARGS;
	c.t = t; c.op = op; c.a = a; c.b = b;
	return run(do_call, &c);
}

static LONG L_Insert(REG(a0, struct TMDatabase *db), REG(a1, const struct TMEntry *e))
{ return e ? call(db, OP_INSERT, e, 0) : TMERR_ARGS; }

static LONG L_Delete(REG(a0, struct TMDatabase *db), REG(a1, CONST_STRPTR id))
{ return id ? call(db, OP_DELETE, id, 0) : TMERR_ARGS; }

static LONG L_DeleteAll(REG(a0, struct TMDatabase *db)) { return call(db, OP_DELETEALL, 0, 0); }
static LONG L_Commit(REG(a0, struct TMDatabase *db)) { return call(db, OP_COMMIT, 0, 0); }
static LONG L_Rollback(REG(a0, struct TMDatabase *db)) { return call(db, OP_ROLLBACK, 0, 0); }

static LONG L_Search(REG(a0, struct TMDatabase *db), REG(a1, const struct TMQuery *q),
                     REG(a2, struct TMResults **res))
{
	if (res) *res = 0;
	if (!q || !res || !q->tmq_SourceLang || !q->tmq_Lang || !q->tmq_Text) return TMERR_ARGS;
	return call(db, OP_SEARCH, q, res);
}

static LONG L_SearchSubstring(REG(a0, struct TMDatabase *db), REG(a1, const struct TMQuery *q),
                              REG(a2, struct TMResults **res))
{
	if (res) *res = 0;
	if (!q || !res || !q->tmq_SourceLang || !q->tmq_Lang || !q->tmq_Text) return TMERR_ARGS;
	return call(db, OP_SUBSTRING, q, res);
}

static VOID L_FreeResults(REG(a0, struct TMResults *r))
{
	if (r) tmc_free_results((struct tm_results *)r);
}

static LONG L_ForEach(REG(a0, struct TMDatabase *db), REG(a1, struct Hook *hook))
{ return hook ? call(db, OP_FOREACH, hook, 0) : TMERR_ARGS; }

static LONG L_GetStats(REG(a0, struct TMDatabase *db), REG(a1, struct TMStats *s))
{ return s ? call(db, OP_STATS, 0, s) : TMERR_ARGS; }

static CONST_STRPTR L_ErrorText(REG(a0, struct TMDatabase *db), REG(d0, LONG err))
{
	return (CONST_STRPTR)tmc_error_text(db ? db->db : 0, (int)err);
}

static LONG do_makeid(APTR v)
{
	struct args *x = (struct args *)v;
	return tmc_make_id((const struct tm_entry *)x->a, (char *)x->c);
}

static LONG L_MakeId(REG(a0, const struct TMEntry *e), REG(a1, STRPTR buf))
{
	struct args x;
	if (!e || !buf) return TMERR_ARGS;
	if (start_sqlite() != SQLITE_OK) return TMERR_DB;   /* the core allocates through SQLite */
	x.a = e;
	x.c = buf;
	return run(do_makeid, &x);
}

/* ---- Library housekeeping ---------------------------------------------- */

static struct Library *lib_init(REG(d0, struct TMLibBase *base), REG(a0, BPTR seglist),
                                REG(a6, struct ExecBase *sys));
static struct Library *lib_open(REG(a6, struct TMLibBase *base));
static BPTR lib_close(REG(a6, struct TMLibBase *base));
static BPTR lib_expunge(REG(a6, struct TMLibBase *base));
static ULONG lib_null(void);

static const APTR lib_vectors[] = {
	(APTR)lib_open, (APTR)lib_close, (APTR)lib_expunge, (APTR)lib_null,
	(APTR)L_OpenDatabase, (APTR)L_CloseDatabase, (APTR)L_Insert, (APTR)L_Delete,
	(APTR)L_DeleteAll, (APTR)L_Commit, (APTR)L_Rollback, (APTR)L_Search,
	(APTR)L_SearchSubstring, (APTR)L_FreeResults, (APTR)L_ForEach, (APTR)L_GetStats,
	(APTR)L_ErrorText, (APTR)L_MakeId,
	(APTR)-1
};

static const struct {
	ULONG size;
	const APTR *vectors;
	APTR data;
	APTR init;
} lib_inittable = {
	sizeof(struct TMLibBase), lib_vectors, NULL, (APTR)lib_init
};

const struct Resident lib_romtag = {
	RTC_MATCHWORD, (struct Resident *)&lib_romtag, (APTR)(&lib_romtag + 1),
	RTF_AUTOINIT, LIB_VERSION, NT_LIBRARY, 0,
	(char *)lib_name, (char *)lib_id, (APTR)&lib_inittable
};

static struct Library *lib_init(REG(d0, struct TMLibBase *base), REG(a0, BPTR seglist),
                                REG(a6, struct ExecBase *sys))
{
	SysBase = sys;
	base->seglist = seglist;
	base->lib.lib_Node.ln_Type = NT_LIBRARY;
	base->lib.lib_Node.ln_Name = (char *)lib_name;
	base->lib.lib_Flags = LIBF_SUMUSED | LIBF_CHANGED;
	base->lib.lib_Version = LIB_VERSION;
	base->lib.lib_Revision = LIB_REVISION;
	base->lib.lib_IdString = (APTR)lib_id;
	InitSemaphore(&lib_sem);
	databases.mlh_Head = (struct MinNode *)&databases.mlh_Tail;
	databases.mlh_Tail = 0;
	databases.mlh_TailPred = (struct MinNode *)&databases.mlh_Head;
	tm_amiga_init();
	if (!(DOSBase = (struct DosLibrary *)OpenLibrary((CONST_STRPTR)"dos.library", 37))) {
		FreeMem((UBYTE *)base - base->lib.lib_NegSize, base->lib.lib_NegSize + base->lib.lib_PosSize);
		return 0;
	}
	return &base->lib;
}

static void close_maths(void)
{
	if (MathIeeeSingTransBase) CloseLibrary(MathIeeeSingTransBase);
	if (MathIeeeSingBasBase) CloseLibrary(MathIeeeSingBasBase);
	if (MathIeeeDoubTransBase) CloseLibrary(MathIeeeDoubTransBase);
	if (MathIeeeDoubBasBase) CloseLibrary(MathIeeeDoubBasBase);
	MathIeeeDoubBasBase = MathIeeeDoubTransBase = MathIeeeSingBasBase = MathIeeeSingTransBase = 0;
}

/* The maths libraries are opened at the first open, not in lib_init: the
 * transcendental ones may have to be loaded from LIBS:. */
static struct Library *lib_open(REG(a6, struct TMLibBase *base))
{
	struct Library *r = &base->lib;
	base->lib.lib_OpenCnt++;
	base->lib.lib_Flags &= ~LIBF_DELEXP;
	ObtainSemaphore(&lib_sem);
	if (!MathIeeeDoubBasBase) {
		MathIeeeDoubBasBase = OpenLibrary((CONST_STRPTR)"mathieeedoubbas.library", 37);
		MathIeeeDoubTransBase = OpenLibrary((CONST_STRPTR)"mathieeedoubtrans.library", 37);
		MathIeeeSingBasBase = OpenLibrary((CONST_STRPTR)"mathieeesingbas.library", 37);
		MathIeeeSingTransBase = OpenLibrary((CONST_STRPTR)"mathieeesingtrans.library", 37);
		if (!MathIeeeDoubBasBase || !MathIeeeDoubTransBase || !MathIeeeSingBasBase || !MathIeeeSingTransBase) {
			close_maths();
			r = 0;
		}
	}
	ReleaseSemaphore(&lib_sem);
	if (!r) base->lib.lib_OpenCnt--;
	return r;
}

/* the closing task's databases still open: closed now */
static void close_leftovers(void)
{
	struct Task *me = FindTask(0);
	struct TMDatabase *t;
	for (;;) {
		ObtainSemaphore(&lib_sem);
		for (t = (struct TMDatabase *)databases.mlh_Head; t->node.mln_Succ; t = (struct TMDatabase *)t->node.mln_Succ)
			if (t->owner == me) break;
		ReleaseSemaphore(&lib_sem);
		if (!t->node.mln_Succ) break;
		close_db(t);
	}
}

static BPTR lib_close(REG(a6, struct TMLibBase *base))
{
	close_leftovers();
	if (base->lib.lib_OpenCnt)
		base->lib.lib_OpenCnt--;
	if (base->lib.lib_OpenCnt == 0 && (base->lib.lib_Flags & LIBF_DELEXP))
		return lib_expunge(base);
	return 0;
}

static BPTR lib_expunge(REG(a6, struct TMLibBase *base))
{
	BPTR seglist;
	if (base->lib.lib_OpenCnt) {
		base->lib.lib_Flags |= LIBF_DELEXP;
		return 0;
	}
	seglist = base->seglist;
	Remove(&base->lib.lib_Node);
	if (sqlite_ready) sqlite3_shutdown();
	sqlite_ready = 0;
	tm_amiga_cleanup();
	close_maths();
	CloseLibrary((struct Library *)DOSBase);
	FreeMem((UBYTE *)base - base->lib.lib_NegSize,
	        base->lib.lib_NegSize + base->lib.lib_PosSize);
	return seglist;
}

static ULONG lib_null(void) { return 0; }

/* transmem.library: what SQLite and the core need from AmigaOS.
 *   memory   AllocMem with the size in front (8-byte aligned blocks)
 *   mutexes  SignalSemaphores
 *   files    the "amiga" VFS on dos.library: Open/Read/Write/Seek and
 *            SetFileSize, AmigaDOS names, and gfopensqlite's lock (an
 *            exclusive Lock() on "<database>.lck", gone after a reset)
 *   strings  the few C library calls SQLite makes
 *   SHA-1    gfopencrypto.library 2.2's OC_SHA1, opened at the first need
 *   clock    DateStamp
 * No C library: the library links nothing but this and SQLite.
 * MIT, Copyright (c) 2026 AmiCradle Team. */
#include <exec/types.h>
#include <exec/memory.h>
#include <exec/semaphores.h>
#include <exec/execbase.h>
#include <dos/dos.h>
#include <dos/dosextens.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include "sqlite3.h"
#include "../src/tm_core.h"
#include "tm_amiga.h"

extern struct ExecBase *SysBase;
extern struct DosLibrary *DOSBase;

/* ---- C library ---------------------------------------------------------- */

typedef __SIZE_TYPE__ size_t_;

void *memcpy(void *d, const void *s, size_t_ n)
{
	if (n) CopyMem((APTR)s, d, n);
	return d;
}

void *memmove(void *d, const void *s, size_t_ n)
{
	unsigned char *p = (unsigned char *)d;
	const unsigned char *q = (const unsigned char *)s;
	if (p == q || !n) return d;
	if (p < q || p >= q + n) { CopyMem((APTR)s, d, n); return d; }
	p += n; q += n;
	while (n--) *--p = *--q;
	return d;
}

/* libnix's string.h turns memmove and bcopy into this (src, dst, n) */
void __bcopz(const void *s, void *d, size_t_ n) { memmove(d, s, n); }

void *memset(void *d, int c, size_t_ n)
{
	unsigned char *p = (unsigned char *)d;
	while (n--) *p++ = (unsigned char)c;
	return d;
}

int memcmp(const void *a, const void *b, size_t_ n)
{
	const unsigned char *p = (const unsigned char *)a, *q = (const unsigned char *)b;
	for (; n; n--, p++, q++)
		if (*p != *q) return *p < *q ? -1 : 1;
	return 0;
}

size_t_ strlen(const char *s)
{
	const char *p = s;
	while (*p) p++;
	return (size_t_)(p - s);
}

int strcmp(const char *a, const char *b)
{
	while (*a && *a == *b) a++, b++;
	return (unsigned char)*a - (unsigned char)*b;
}

int strncmp(const char *a, const char *b, size_t_ n)
{
	for (; n; n--, a++, b++) {
		if (*a != *b) return (unsigned char)*a - (unsigned char)*b;
		if (!*a) break;
	}
	return 0;
}

char *strchr(const char *s, int c)
{
	for (;; s++) {
		if (*s == (char)c) return (char *)s;
		if (!*s) return 0;
	}
}

char *strrchr(const char *s, int c)
{
	const char *r = 0;
	for (;; s++) {
		if (*s == (char)c) r = s;
		if (!*s) return (char *)r;
	}
}

size_t_ strspn(const char *s, const char *set)
{
	size_t_ n = 0;
	while (s[n] && strchr(set, s[n])) n++;
	return n;
}

size_t_ strcspn(const char *s, const char *set)
{
	size_t_ n = 0;
	while (s[n] && !strchr(set, s[n])) n++;
	return n;
}

/* ---- Memory -------------------------------------------------------------- */

static void *m_malloc(int n)
{
	ULONG *p;
	if (n <= 0) return 0;
	p = (ULONG *)AllocMem((ULONG)n + 8, MEMF_ANY);
	if (!p) return 0;
	p[0] = (ULONG)n;
	return p + 2;
}

static void m_free(void *v)
{
	ULONG *p = (ULONG *)v;
	if (p) FreeMem(p - 2, p[-2] + 8);
}

static int m_size(void *v) { return v ? (int)((ULONG *)v)[-2] : 0; }

static void *m_realloc(void *v, int n)
{
	void *q;
	int old = m_size(v);
	if (n <= old && n > old / 2) return v;
	q = m_malloc(n);
	if (q && v) CopyMem(v, q, (ULONG)(old < n ? old : n));
	if (q) m_free(v);
	return q;
}

static int m_roundup(int n) { return (n + 7) & ~7; }
static int m_init(void *x) { (void)x; return SQLITE_OK; }
static void m_shutdown(void *x) { (void)x; }

static const sqlite3_mem_methods mem_methods = {
	m_malloc, m_free, m_realloc, m_size, m_roundup, m_init, m_shutdown, 0
};

/* ---- Mutexes ------------------------------------------------------------- */

struct sqlite3_mutex { struct SignalSemaphore sem; int id; };

#define N_STATIC 12                     /* SQLITE_MUTEX_STATIC_MAIN (2) .. STATIC_VFS3 (11), and two spare */
static struct sqlite3_mutex static_mutexes[N_STATIC];

static int mx_init(void)
{
	int k;
	for (k = 0; k < N_STATIC; k++) {
		memset(&static_mutexes[k], 0, sizeof static_mutexes[k]);
		InitSemaphore(&static_mutexes[k].sem);
		static_mutexes[k].id = k + 2;
	}
	return SQLITE_OK;
}

static int mx_end(void) { return SQLITE_OK; }

static sqlite3_mutex *mx_alloc(int id)
{
	struct sqlite3_mutex *m;
	if (id == SQLITE_MUTEX_FAST || id == SQLITE_MUTEX_RECURSIVE) {
		m = (struct sqlite3_mutex *)AllocMem(sizeof *m, MEMF_ANY | MEMF_CLEAR);
		if (!m) return 0;
		InitSemaphore(&m->sem);
		m->id = id;
		return m;
	}
	if (id - 2 < 0 || id - 2 >= N_STATIC) return 0;
	return &static_mutexes[id - 2];
}

static void mx_free(sqlite3_mutex *m)
{
	if (m && (m->id == SQLITE_MUTEX_FAST || m->id == SQLITE_MUTEX_RECURSIVE))
		FreeMem(m, sizeof *m);
}

static void mx_enter(sqlite3_mutex *m) { ObtainSemaphore(&m->sem); }
static int mx_try(sqlite3_mutex *m) { return AttemptSemaphore(&m->sem) ? SQLITE_OK : SQLITE_BUSY; }
static void mx_leave(sqlite3_mutex *m) { ReleaseSemaphore(&m->sem); }

static const sqlite3_mutex_methods mutex_methods = {
	mx_init, mx_end, mx_alloc, mx_free, mx_enter, mx_try, mx_leave, 0, 0
};

/* ---- Files --------------------------------------------------------------- */

struct afile {
	sqlite3_file base;
	BPTR fh;
	LONG pos;                       /* where the file handle is, or -1 */
	int lock_level;
	int main_db;
	BPTR lck;                       /* the exclusive Lock() on "<db>.lck", or 0 */
	char *del_name;                 /* deleted at close (SQLITE_OPEN_DELETEONCLOSE) */
	char *lck_name;                 /* "<db>.lck" */
};

static char *str_dup2(const char *a, const char *b)
{
	size_t_ n = strlen(a), m = strlen(b);
	char *p = (char *)sqlite3_malloc((int)(n + m + 1));
	if (p) { memcpy(p, a, n); memcpy(p + n, b, m + 1); }
	return p;
}

/* seek, 0 or -1; a place past the end is made by growing the file */
static int seek_to(struct afile *f, LONG off, int grow)
{
	if (f->pos == off) return 0;
	if (Seek(f->fh, off, OFFSET_BEGINNING) != -1) { f->pos = off; return 0; }
	f->pos = -1;
	if (!grow) return -1;
	if (SetFileSize(f->fh, off, OFFSET_BEGINNING) == -1) return -1;
	if (Seek(f->fh, off, OFFSET_BEGINNING) == -1) return -1;
	f->pos = off;
	return 0;
}

static int f_close(sqlite3_file *id)
{
	struct afile *f = (struct afile *)id;
	if (f->lck) { UnLock(f->lck); f->lck = 0; }
	if (f->fh) Close(f->fh);
	f->fh = 0;
	if (f->del_name) DeleteFile((STRPTR)f->del_name);
	sqlite3_free(f->del_name);
	sqlite3_free(f->lck_name);
	f->del_name = f->lck_name = 0;
	return SQLITE_OK;
}

static int f_read(sqlite3_file *id, void *buf, int amt, sqlite3_int64 off)
{
	struct afile *f = (struct afile *)id;
	LONG got = 0;
	if (off > 0x7fffffffL - amt) return SQLITE_IOERR_READ;
	if (seek_to(f, (LONG)off, 0) == 0) {
		got = Read(f->fh, buf, amt);
		if (got < 0) { f->pos = -1; return SQLITE_IOERR_READ; }
		f->pos += got;
	}
	if (got < amt) {
		memset((char *)buf + got, 0, (size_t_)(amt - got));
		return SQLITE_IOERR_SHORT_READ;
	}
	return SQLITE_OK;
}

static int f_write(sqlite3_file *id, const void *buf, int amt, sqlite3_int64 off)
{
	struct afile *f = (struct afile *)id;
	LONG put;
	if (off > 0x7fffffffL - amt) return SQLITE_FULL;
	if (seek_to(f, (LONG)off, 1) != 0) return IoErr() == ERROR_DISK_FULL ? SQLITE_FULL : SQLITE_IOERR_WRITE;
	put = Write(f->fh, (APTR)buf, amt);
	if (put != amt) {
		f->pos = -1;
		return IoErr() == ERROR_DISK_FULL ? SQLITE_FULL : SQLITE_IOERR_WRITE;
	}
	f->pos += put;
	return SQLITE_OK;
}

static int f_truncate(sqlite3_file *id, sqlite3_int64 size)
{
	struct afile *f = (struct afile *)id;
	f->pos = -1;
	return SetFileSize(f->fh, (LONG)size, OFFSET_BEGINNING) == -1 ? SQLITE_IOERR_TRUNCATE : SQLITE_OK;
}

static int f_sync(sqlite3_file *id, int flags) { (void)id; (void)flags; return SQLITE_OK; }

static int f_size(sqlite3_file *id, sqlite3_int64 *size)
{
	struct afile *f = (struct afile *)id;
	LONG end;
	f->pos = -1;
	if (Seek(f->fh, 0, OFFSET_END) == -1) return SQLITE_IOERR_FSTAT;
	end = Seek(f->fh, 0, OFFSET_CURRENT);
	if (end == -1) return SQLITE_IOERR_FSTAT;
	f->pos = end;
	*size = end;
	return SQLITE_OK;
}

/* gfopensqlite's lock: 0 held, 1 busy, -1 failed */
static int lck_take(struct afile *f)
{
	BPTR l = Lock((STRPTR)f->lck_name, EXCLUSIVE_LOCK);
	if (!l && IoErr() == ERROR_OBJECT_NOT_FOUND) {
		BPTR fh = Open((STRPTR)f->lck_name, MODE_READWRITE);  /* made once, never deleted */
		if (fh) {
			Close(fh);
			l = Lock((STRPTR)f->lck_name, EXCLUSIVE_LOCK);
		}
	}
	if (!l) return IoErr() == ERROR_OBJECT_IN_USE ? 1 : -1;
	f->lck = l;
	return 0;
}

static int f_lock(sqlite3_file *id, int level)
{
	struct afile *f = (struct afile *)id;
	if (!f->main_db) { f->lock_level = level; return SQLITE_OK; }
	if (!f->lck) {
		int r = lck_take(f);
		if (r == 1) return SQLITE_BUSY;
		if (r < 0) return SQLITE_IOERR_LOCK;
	}
	f->lock_level = level;
	return SQLITE_OK;
}

static int f_unlock(sqlite3_file *id, int level)
{
	struct afile *f = (struct afile *)id;
	f->lock_level = level;
	if (level == SQLITE_LOCK_NONE && f->lck) {
		UnLock(f->lck);
		f->lck = 0;
	}
	return SQLITE_OK;
}

static int f_reserved(sqlite3_file *id, int *out)
{
	struct afile *f = (struct afile *)id;
	*out = 0;
	if (!f->main_db) return SQLITE_OK;
	if (f->lck) { *out = f->lock_level >= SQLITE_LOCK_RESERVED; return SQLITE_OK; }
	switch (lck_take(f)) {
	case 0: UnLock(f->lck); f->lck = 0; break;
	case 1: *out = 1; break;
	default: return SQLITE_IOERR_CHECKRESERVEDLOCK;
	}
	return SQLITE_OK;
}

static int f_control(sqlite3_file *id, int op, void *arg) { (void)id; (void)op; (void)arg; return SQLITE_NOTFOUND; }
static int f_sector(sqlite3_file *id) { (void)id; return 512; }
static int f_caps(sqlite3_file *id) { (void)id; return 0; }

static const sqlite3_io_methods io_methods = {
	1, f_close, f_read, f_write, f_truncate, f_sync, f_size,
	f_lock, f_unlock, f_reserved, f_control, f_sector, f_caps,
	0, 0, 0, 0, 0, 0
};

static ULONG temp_count;

static int v_open(sqlite3_vfs *vfs, sqlite3_filename name, sqlite3_file *id, int flags, int *out_flags)
{
	struct afile *f = (struct afile *)id;
	char tmp[40];
	LONG mode;
	(void)vfs;
	memset(f, 0, sizeof *f);
	f->pos = -1;
	if (!name) {                    /* a temporary file */
		ULONG n = ++temp_count, k = 0, t = (ULONG)FindTask(0);
		static const char hex[] = "0123456789abcdef";
		memcpy(tmp, "T:transmem-", 11);
		for (k = 0; k < 8; k++) tmp[11 + k] = hex[(t >> (28 - 4 * k)) & 15];
		tmp[19] = '-';
		for (k = 0; k < 8; k++) tmp[20 + k] = hex[(n >> (28 - 4 * k)) & 15];
		tmp[28] = 0;
		name = tmp;
		flags |= SQLITE_OPEN_DELETEONCLOSE | SQLITE_OPEN_CREATE;
	}
	if (flags & SQLITE_OPEN_READONLY) mode = MODE_OLDFILE;
	else if (flags & SQLITE_OPEN_CREATE) {
		if (flags & SQLITE_OPEN_EXCLUSIVE) {
			BPTR l = Lock((STRPTR)name, SHARED_LOCK);
			if (l) { UnLock(l); return SQLITE_CANTOPEN; }
		}
		mode = MODE_READWRITE;
	} else mode = MODE_OLDFILE;
	f->fh = Open((STRPTR)name, mode);
	if (!f->fh) return SQLITE_CANTOPEN;
	if (flags & SQLITE_OPEN_DELETEONCLOSE) {
		if (!(f->del_name = str_dup2(name, ""))) { Close(f->fh); f->fh = 0; return SQLITE_NOMEM; }
	}
	if (flags & SQLITE_OPEN_MAIN_DB) {
		f->main_db = 1;
		if (!(f->lck_name = str_dup2(name, ".lck"))) { f_close(id); return SQLITE_NOMEM; }
	}
	f->base.pMethods = &io_methods;
	if (out_flags) *out_flags = flags;
	return SQLITE_OK;
}

static int v_delete(sqlite3_vfs *vfs, const char *name, int sync)
{
	(void)vfs; (void)sync;
	if (DeleteFile((STRPTR)name)) return SQLITE_OK;
	return IoErr() == ERROR_OBJECT_NOT_FOUND ? SQLITE_IOERR_DELETE_NOENT : SQLITE_IOERR_DELETE;
}

static int v_access(sqlite3_vfs *vfs, const char *name, int flags, int *out)
{
	BPTR l = Lock((STRPTR)name, SHARED_LOCK);
	(void)vfs; (void)flags;
	if (l) { UnLock(l); *out = 1; }
	else *out = IoErr() == ERROR_OBJECT_IN_USE;
	return SQLITE_OK;
}

/* A name with a volume or an assign ("DH1:x.db", "PROGDIR:tm.sqlite") is
 * whole already. A relative one gets its drawer's whole name
 * (NameFromLock), so the -journal stays beside the database whatever the
 * program's current directory is when it is written or rolled back. */
static int v_fullname(sqlite3_vfs *vfs, const char *name, int n, char *out)
{
	const char *colon = strchr(name, ':'), *file;
	char dir[512];
	BPTR l;
	size_t_ len = strlen(name), k;
	(void)vfs;
	if (len + 1 > (size_t_)n) return SQLITE_CANTOPEN;
	if (name[0] && !(colon && colon != name)) {
		file = (const char *)PathPart((STRPTR)name);
		k = (size_t_)(file - name);
		if (k < sizeof dir) {
			memcpy(dir, name, k);
			dir[k] = 0;
			if ((l = Lock((STRPTR)dir, ACCESS_READ))) {
				int ok = NameFromLock(l, (STRPTR)out, n) && AddPart((STRPTR)out, FilePart((STRPTR)name), n);
				UnLock(l);
				if (ok) return SQLITE_OK;
			}
		}
	}
	memcpy(out, name, len + 1);
	return SQLITE_OK;
}

static int v_random(sqlite3_vfs *vfs, int n, char *out)
{
	struct DateStamp ds;
	ULONG x;
	int k;
	(void)vfs;
	DateStamp(&ds);
	x = (ULONG)ds.ds_Days * 1440 * 3000 + (ULONG)ds.ds_Minute * 3000 + (ULONG)ds.ds_Tick;
	x ^= (ULONG)FindTask(0);
	for (k = 0; k < n; k++) {
		x = x * 1103515245UL + 12345UL;
		out[k] = (char)(x >> 16);
	}
	return n;
}

static int v_sleep(sqlite3_vfs *vfs, int us)
{
	LONG ticks = (us + 19999) / 20000;
	(void)vfs;
	Delay(ticks > 0 ? ticks : 1);
	return us;
}

/* milliseconds since the Julian day epoch; the Amiga's begins 1 Jan 1978 */
static int v_time64(sqlite3_vfs *vfs, sqlite3_int64 *out)
{
	struct DateStamp ds;
	(void)vfs;
	DateStamp(&ds);
	*out = (sqlite3_int64)211119220800000LL + (sqlite3_int64)ds.ds_Days * 86400000 +
	       (sqlite3_int64)ds.ds_Minute * 60000 + (sqlite3_int64)ds.ds_Tick * 20;
	return SQLITE_OK;
}

static int v_time(sqlite3_vfs *vfs, double *out)
{
	sqlite3_int64 ms;
	v_time64(vfs, &ms);
	*out = (double)ms / 86400000.0;
	return SQLITE_OK;
}

static int v_last_error(sqlite3_vfs *vfs, int n, char *buf) { (void)vfs; (void)n; (void)buf; return 0; }

static sqlite3_vfs amiga_vfs = {
	2, sizeof(struct afile), 512, 0, "amiga", 0,
	v_open, v_delete, v_access, v_fullname,
	0, 0, 0, 0,
	v_random, v_sleep, v_time, v_last_error, v_time64,
	0, 0, 0
};

int sqlite3_os_init(void) { return sqlite3_vfs_register(&amiga_vfs, 1); }
int sqlite3_os_end(void) { return SQLITE_OK; }

/* before sqlite3_initialize: our memory and mutexes */
int tm_amiga_config(void)
{
	int rc = sqlite3_config(SQLITE_CONFIG_MALLOC, &mem_methods);
	if (rc == SQLITE_OK) rc = sqlite3_config(SQLITE_CONFIG_MUTEX, &mutex_methods);
	return rc;
}

/* ---- SHA-1 and the clock ------------------------------------------------- */

struct Library *tm_crypto_base;
static struct SignalSemaphore crypto_sem;

void tm_amiga_init(void) { InitSemaphore(&crypto_sem); }

void tm_amiga_cleanup(void)
{
	if (tm_crypto_base) CloseLibrary(tm_crypto_base);
	tm_crypto_base = 0;
}

int tm_sha1(const void *data, unsigned long n, tm_u8 out[20])
{
	struct OCBufferHash req;
	struct Library *b;
	ObtainSemaphore(&crypto_sem);
	if (!tm_crypto_base) {
		b = OpenLibrary((CONST_STRPTR)"gfopencrypto.library", 2);
		if (b && b->lib_Version == 2 && b->lib_Revision < 2) { CloseLibrary(b); b = 0; }
		tm_crypto_base = b;
	}
	b = tm_crypto_base;
	ReleaseSemaphore(&crypto_sem);
	if (!b) return 8;               /* TMERR_NOCRYPTO */
	req.data = data;
	req.length = n;
	req.out = out;
	tm_oc_sha1(b, &req);
	return 0;
}

/* seconds since 1 January 1970 by the Amiga's clock (2922 days before 1978) */
long tm_now(void)
{
	struct DateStamp ds;
	DateStamp(&ds);
	return 252460800L + ds.ds_Days * 86400L + ds.ds_Minute * 60L + ds.ds_Tick / 50;
}

/* libtransmem.a: opens transmem.library 1 into TransMemBase before main()
 * and closes it after the program ends; the calls themselves are the
 * inline macros of <proto/transmem.h>. TransMemBase is NULL when the
 * library can't be opened: check it before the first call.
 * MIT, Copyright (c) 2026 AmiCradle Team. */
#include <exec/types.h>
#include <exec/libraries.h>
#include <proto/exec.h>
#include <stabs.h>
#include <libraries/transmem.h>

struct Library *TransMemBase;

static void __attribute__((used)) tm_stub_open(void)
{
	if (!TransMemBase)
		TransMemBase = OpenLibrary((CONST_STRPTR)TRANSMEMNAME, TRANSMEM_VERSION);
}
ADD2INIT(tm_stub_open, -50);

static void __attribute__((used)) tm_stub_close(void)
{
	if (TransMemBase) {
		CloseLibrary(TransMemBase);
		TransMemBase = 0;
	}
}
ADD2EXIT(tm_stub_close, -50);

/* transmem.library's AmigaOS layer (tm_amiga.c) and the one gfopencrypto
 * call it makes. MIT, Copyright (c) 2026 AmiCradle Team. */
#ifndef TM_AMIGA_H
#define TM_AMIGA_H

/* gfopencrypto.library 2.2: OC_SHA1(struct OCBufferHash *) at -174, a0 */
struct OCBufferHash {
	const void *data;
	ULONG length;
	UBYTE *out;
};

static inline void tm_oc_sha1(struct Library *base, struct OCBufferHash *req)
{
	register struct Library *a6 __asm("a6") = base;
	register struct OCBufferHash *a0 __asm("a0") = req;
	__asm volatile ("jsr a6@(-174:W)"
		: "+r"(a0)
		: "r"(a6)
		: "d0", "d1", "a1", "fp0", "fp1", "cc", "memory");
}

int tm_amiga_config(void);
void tm_amiga_init(void);
void tm_amiga_cleanup(void);

#endif

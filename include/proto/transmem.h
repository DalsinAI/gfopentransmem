/* transmem.library's proto header. MIT, Copyright (c) 2026 AmiCradle Team.
 * Link with -ltransmem (the stub opens TransMemBase for you), or open
 * transmem.library yourself into TransMemBase. */
#ifndef PROTO_TRANSMEM_H
#define PROTO_TRANSMEM_H

#include <exec/types.h>
#include <libraries/transmem.h>

#ifndef __NOLIBBASE__
extern struct Library *TransMemBase;
#endif
#if defined(__GNUC__) && !defined(TRANSMEM_NO_INLINE)
#include <inline/transmem.h>
#else
#include <clib/transmem_protos.h>
#endif

#endif

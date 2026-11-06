| Copyright (c) 2026 AmiCradle Team. MIT licence (see LICENSE at the repository root).
|
| fpuauto: one binary, the FPU when there is one.
|
| A soft-float program (-m68040, build/m68k-soft-m68040.specs) does its
| double and float maths through libnix's libm020 (__muldf3 is a call of
| IEEEDPMul in mathieeedoubbas.library) and libgcc (__fixdfsi, __floatsidf
| are integer code). Linked with --wrap for each routine (fpuauto.wraps),
| every call comes to ___wrap_<name>, a JMP abs.l that its first call
| points, once, at:
|
|   - the FPU body here, on a 68040 or 68060 with its FPU (AttnFlags
|     AFF_FPU40) that softfpu.library is not emulating (FDADD, FDMUL, FCMP,
|     FMOVE.L under round-to-zero...);
|   - otherwise ___real_<name>, the routine the program had before.
|
| An FPU body that meets a NaN, or a cast out of int range, goes to
| ___real_<name> too, so those answers stay the library's. The FPU
| instructions are the ones the maths libraries' own FPU bodies run, so
| the bits are theirs.
|
| The machine is looked at once per program, into __fpuauto_hw (1 FPU,
| -1 soft, 0 not yet). A test may set it to -1 before the first call to
| take the soft path anywhere.
|
| Assembled once per routine (-DR_<name>, one archive member each, so a
| program links only what it calls) and once with -DFPUAUTO_COMMON.

	.text
	.even

#ifdef FPUAUTO_COMMON
| ---- the look-up -------------------------------------------------------
	.globl	___fpuauto_check
___fpuauto_check:
	movem.l	d0-d2/a0-a1/a6,-(sp)
	move.l	4.w,a6
	moveq	#-1,d2
	btst	#6,297(a6)		| AttnFlags (296), low byte: AFF_FPU40
	beq.s	1f
	jsr	-132(a6)		| Forbid
	lea	378(a6),a0		| LibList
	lea	sfname(pc),a1
	jsr	-276(a6)		| FindName
	moveq	#1,d2
	tst.l	d0
	beq.s	2f
	move.l	d0,a0
	btst	#0,35(a0)		| sf_Flags (after struct Library): SFF_EMULATING
	beq.s	2f
	moveq	#-1,d2
2:	jsr	-138(a6)		| Permit
1:	move.b	d2,___fpuauto_hw
	movem.l	(sp)+,d0-d2/a0-a1/a6
	rts

| ---- a routine's first call: point its jump ----------------------------
| (4,sp) the jump's target field, (8,sp) the FPU body, (12,sp) the original
	.globl	___fpuauto_patch
___fpuauto_patch:
	movem.l	d0-d1/a0-a1/a6,-(sp)
	tst.b	___fpuauto_hw
	bne.s	1f
	bsr	___fpuauto_check
1:	move.l	(20+8,sp),d0
	tst.b	___fpuauto_hw
	bgt.s	2f
	move.l	(20+12,sp),d0
2:	move.l	(20+4,sp),a0
	move.l	d0,(a0)
	move.l	4.w,a6
	jsr	-636(a6)		| CacheClearU
	movem.l	(sp)+,d0-d1/a0-a1/a6
	rts

sfname:	.asciz	"softfpu.library"
	.even

	.data
	.globl	___fpuauto_hw
___fpuauto_hw:	.byte	0
	.even
	.text
#endif

| ---- one routine: the jump, its first call, and its FPU body (9:) -------
	.macro	WRAP name
	.globl	___wrap_\name
___wrap_\name:
	.short	0x4ef9			| JMP abs.l, pointed by the first call
	.long	7f
7:	pea	___real_\name
	pea	9f
	pea	___wrap_\name+2
	bsr	___fpuauto_patch
	lea	(12,sp),sp
	bra.s	___wrap_\name
8:	jmp	___real_\name		| the FPU body's way to the original
9:
	.endm

| double result in d0/d1 from fp0
	.macro	RETD
	fmove.d	fp0,-(sp)
	move.l	(sp)+,d0
	move.l	(sp)+,d1
	rts
	.endm

	.macro	DOP name, op
	WRAP	\name
	fmove.d	(4,sp),fp0
	\op	(12,sp),fp0
	fbun	8b
	RETD
	.endm

	.macro	SOP name, op
	WRAP	\name
	fmove.s	(4,sp),fp0
	\op	(8,sp),fp0
	fbun	8b
	fmove.s	fp0,d0
	rts
	.endm

| The compares: libnix gives every one of them IEEEDPCmp's -1, 0 or 1
| (and the library orders NaNs its own way: those go to it).
	.macro	CMPRES
	fbun	8b
	fbogt	1f
	fbolt	2f
	moveq	#0,d0
	rts
1:	moveq	#1,d0
	rts
2:	moveq	#-1,d0
	rts
	.endm
	.macro	DCMP name
	WRAP	\name
	fmove.d	(4,sp),fp0
	fcmp.d	(12,sp),fp0
	CMPRES
	.endm
	.macro	SCMP name
	WRAP	\name
	fmove.s	(4,sp),fp0
	fcmp.s	(8,sp),fp0
	CMPRES
	.endm

| double/float to int toward zero (FINTRZ is not a 68040 instruction);
| out of range or NaN goes to the original
	.macro	FIX name, fmt
	WRAP	\name
	fmove.\fmt (4,sp),fp0
	fcmp.d	(two31,pc),fp0
	fbuge	8b
	fcmp.d	(mtwo31m1,pc),fp0
	fbole	8b
	fmove.l	fpcr,d1
	move.l	d1,d0
	and.w	#0xffcf,d0
	or.w	#0x0010,d0		| RND: round to zero
	fmove.l	d0,fpcr
	fmove.l	fp0,d0
	fmove.l	d1,fpcr
	rts
	.even
two31:		.long	0x41E00000,0x00000000	| 2^31
mtwo31m1:	.long	0xC1E00000,0x00200000	| -(2^31 + 1)
	.endm

#ifdef R___adddf3
	DOP	__adddf3, fdadd.d
#endif
#ifdef R___subdf3
	DOP	__subdf3, fdsub.d
#endif
#ifdef R___muldf3
	DOP	__muldf3, fdmul.d
#endif
#ifdef R___divdf3
	DOP	__divdf3, fddiv.d
#endif
#ifdef R___addsf3
	SOP	__addsf3, fsadd.s
#endif
#ifdef R___subsf3
	SOP	__subsf3, fssub.s
#endif
#ifdef R___mulsf3
	SOP	__mulsf3, fsmul.s
#endif
#ifdef R___divsf3
	SOP	__divsf3, fsdiv.s
#endif
#ifdef R___cmpdf2
	DCMP	__cmpdf2
#endif
#ifdef R___eqdf2
	DCMP	__eqdf2
#endif
#ifdef R___nedf2
	DCMP	__nedf2
#endif
#ifdef R___ltdf2
	DCMP	__ltdf2
#endif
#ifdef R___ledf2
	DCMP	__ledf2
#endif
#ifdef R___gtdf2
	DCMP	__gtdf2
#endif
#ifdef R___gedf2
	DCMP	__gedf2
#endif
#ifdef R___cmpsf2
	SCMP	__cmpsf2
#endif
#ifdef R___eqsf2
	SCMP	__eqsf2
#endif
#ifdef R___nesf2
	SCMP	__nesf2
#endif
#ifdef R___ltsf2
	SCMP	__ltsf2
#endif
#ifdef R___lesf2
	SCMP	__lesf2
#endif
#ifdef R___gtsf2
	SCMP	__gtsf2
#endif
#ifdef R___gesf2
	SCMP	__gesf2
#endif
#ifdef R___fixdfsi
	FIX	__fixdfsi, d
#endif
#ifdef R___fixsfsi
	FIX	__fixsfsi, s
#endif
#ifdef R___floatsidf
	WRAP	__floatsidf
	fmove.l	(4,sp),fp0
	RETD
#endif
#ifdef R___floatsisf
	WRAP	__floatsisf
	fmove.l	(4,sp),fp0
	fmove.s	fp0,d0
	rts
#endif
#ifdef R___extendsfdf2
	WRAP	__extendsfdf2
	fmove.s	(4,sp),fp0
	fbun	8b
	RETD
#endif
#ifdef R___truncdfsf2
	WRAP	__truncdfsf2
	fmove.d	(4,sp),fp0
	fbun	8b
	fmove.s	fp0,d0
	rts
#endif
#ifdef R_sqrt
	WRAP	sqrt
	fsqrt.d	(4,sp),fp0		| extended, then FMOVE.D: IEEEDPSqrt's FPU body
	fbun	8b
	RETD
#endif

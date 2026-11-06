| transmem.library: a call on a stack of its own, and a hook call.
| MIT, Copyright (c) 2026 AmiCradle Team.

	.text
	.even

| LONG tm_onstack(LONG (*fn)(APTR), APTR arg, struct StackSwapStruct *sss)
| fn(arg) on the stack sss describes (exec StackSwap, -732), then back.
	.globl	_tm_onstack
_tm_onstack:
	movem.l	d2-d3/a2-a3/a6,-(sp)
	move.l	24(sp),a2		| fn
	move.l	28(sp),d2		| arg
	move.l	32(sp),a3		| sss
	move.l	4.w,a6
	move.l	a3,a0
	jsr	-732(a6)		| StackSwap: now on the new stack
	move.l	d2,-(sp)
	jsr	(a2)
	addq.l	#4,sp
	move.l	d0,d3
	move.l	4.w,a6
	move.l	a3,a0
	jsr	-732(a6)		| StackSwap: back
	move.l	d3,d0
	movem.l	(sp)+,d2-d3/a2-a3/a6
	rts

| ULONG tm_callhook(struct Hook *hook, APTR object, APTR message)
| as utility.library's CallHookPkt: h_Entry with a0 hook, a2 object, a1 message
	.globl	_tm_callhook
_tm_callhook:
	movem.l	a2/a6,-(sp)
	move.l	12(sp),a0
	move.l	16(sp),a2
	move.l	20(sp),a1
	move.l	8(a0),a6		| h_Entry
	jsr	(a6)
	movem.l	(sp)+,a2/a6
	rts

/*
HOST_EXCEPTION_ASM.S

The program's entry for CPU exceptions, in place of libnx's (a weak symbol
there, which only reports a crash and ends the program).

Horizon delivers a thread's exception (a data abort, say) by restarting the
thread at the program's entry point with x0 the exception's type and x1 the
address of the kernel's record of it (libnx's ThreadExceptionFrameA64: x0
to x8, lr, sp, pc, pstate and the fault's registers); hbloader and libnx's
start-up code pass that on to __libnx_exception_entry. svcReturnFromException
then puts back x0 to x8, lr, sp and pc from that record - as the handler
left them - and resumes the thread. Every other register must be as the
exception found it by then, so all that C code may change are saved here.

The handler (host_exception.c) either lets the thread go on, which runs the
instruction that faulted again (a write to a page the renderer watches,
writable now: host_memory.c), or points the record at its crash report.

The kernel handles one exception of a process at a time, so one stack does
for them all; the stack pointer the kernel supplies is a small one of its
own.
*/

#define FRAME_SIZE 704
#define FRAME_FLOATING_POINT_CONTROL 168
#define FRAME_VECTORS 192

	.text
	.align 2
	.global __libnx_exception_entry
	.type __libnx_exception_entry, %function
__libnx_exception_entry:
	adrp x2, host_exception_stack_top
	ldr x2, [x2, #:lo12:host_exception_stack_top]
	cbz x2, .Lunhandled
	sub sp, x2, #FRAME_SIZE

	stp x9, x10, [sp, #0]
	stp x11, x12, [sp, #16]
	stp x13, x14, [sp, #32]
	stp x15, x16, [sp, #48]
	stp x17, x18, [sp, #64]
	stp x19, x20, [sp, #80]
	stp x21, x22, [sp, #96]
	stp x23, x24, [sp, #112]
	stp x25, x26, [sp, #128]
	stp x27, x28, [sp, #144]
	str x29, [sp, #160]
	mrs x3, fpcr
	mrs x4, fpsr
	stp x3, x4, [sp, #FRAME_FLOATING_POINT_CONTROL]
	add x3, sp, #FRAME_VECTORS
	stp q0, q1, [x3, #0]
	stp q2, q3, [x3, #32]
	stp q4, q5, [x3, #64]
	stp q6, q7, [x3, #96]
	stp q8, q9, [x3, #128]
	stp q10, q11, [x3, #160]
	stp q12, q13, [x3, #192]
	stp q14, q15, [x3, #224]
	stp q16, q17, [x3, #256]
	stp q18, q19, [x3, #288]
	stp q20, q21, [x3, #320]
	stp q22, q23, [x3, #352]
	stp q24, q25, [x3, #384]
	stp q26, q27, [x3, #416]
	stp q28, q29, [x3, #448]
	stp q30, q31, [x3, #480]

	/* int host_exception_handler(uint32_t type, ThreadExceptionFrameA64 *record,
		struct host_exception_registers *registers) */
	mov x2, sp
	mov x29, #0
	bl host_exception_handler
	mov w5, w0

	add x3, sp, #FRAME_VECTORS
	ldp q0, q1, [x3, #0]
	ldp q2, q3, [x3, #32]
	ldp q4, q5, [x3, #64]
	ldp q6, q7, [x3, #96]
	ldp q8, q9, [x3, #128]
	ldp q10, q11, [x3, #160]
	ldp q12, q13, [x3, #192]
	ldp q14, q15, [x3, #224]
	ldp q16, q17, [x3, #256]
	ldp q18, q19, [x3, #288]
	ldp q20, q21, [x3, #320]
	ldp q22, q23, [x3, #352]
	ldp q24, q25, [x3, #384]
	ldp q26, q27, [x3, #416]
	ldp q28, q29, [x3, #448]
	ldp q30, q31, [x3, #480]
	ldp x3, x4, [sp, #FRAME_FLOATING_POINT_CONTROL]
	msr fpcr, x3
	msr fpsr, x4
	ldp x9, x10, [sp, #0]
	ldp x11, x12, [sp, #16]
	ldp x13, x14, [sp, #32]
	ldp x15, x16, [sp, #48]
	ldp x17, x18, [sp, #64]
	ldp x19, x20, [sp, #80]
	ldp x21, x22, [sp, #96]
	ldp x23, x24, [sp, #112]
	ldp x25, x26, [sp, #128]
	ldp x27, x28, [sp, #144]
	ldr x29, [sp, #160]

	cbz w5, .Lunhandled
	mov w0, wzr
	svc 0x28 /* svcReturnFromException */
	b .

.Lunhandled:
	/* the kernel ends the program (Atmosphere writes its crash report) */
	mov w0, #0xf801 /* KERNELRESULT(UnhandledUserInterrupt) */
	svc 0x28
	b .
	.size __libnx_exception_entry, . - __libnx_exception_entry

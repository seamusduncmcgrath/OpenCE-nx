/*
HOST_THREAD_ASM.S

void *host_call_on_stack(void *(*function)(void *), void *argument, void *stack_top)

Calls function(argument) with the stack pointer at stack_top, and comes back
to the caller's stack with its result. Guest (ILP32) code keeps stack
addresses in 32-bit registers, so each thread that runs it moves to a stack
in guest memory this way (host_thread.c): libnx puts the stacks it makes
high in the address space.
*/

	.text
	.align 2
	.global host_call_on_stack
	.type host_call_on_stack, %function
host_call_on_stack:
	stp x29, x30, [sp, #-32]!
	str x19, [sp, #16]
	mov x19, sp
	and x2, x2, #0xfffffffffffffff0
	mov sp, x2
	mov x9, x0
	mov x0, x1
	/* the first frame record on the new stack ends the chain */
	mov x29, #0
	blr x9
	mov sp, x19
	ldr x19, [sp, #16]
	ldp x29, x30, [sp], #32
	ret
	.size host_call_on_stack, . - host_call_on_stack

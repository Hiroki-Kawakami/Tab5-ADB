#!/usr/bin/env python3
"""Patch the FreeRTOS-Kernel GCC/Posix port for macOS (arm64).

Applied via FetchContent's PATCH_COMMAND. Idempotent: running it twice is a
no-op. Two independent fixes, each guarded by its own marker so they apply (and
self-skip) independently:

1. "macOS/arm64 workaround" — signal-mask deadlock.
   The port creates each task pthread from inside a critical section, where
   vPortEnterCritical() has masked *every* signal (sigfillset). On macOS/arm64,
   pthread_create() while all signals are blocked deadlocks inside
   libsystem_pthread once a previously-created task thread is parked in
   pthread_cond_wait — so vTaskStartScheduler() hangs creating the timer task
   and no task ever runs. Fix: spawn the task thread with a clean (empty) signal
   mask, then restore the caller's mask. Safe for this port: task wakeups use
   condition variables (event_signal), and the tick (SIGALRM) / resume signals
   are delivered directionally with pthread_kill() only to the *current* task —
   never to a task still being created — so the brief unmasked window cannot
   misdeliver a signal to the new thread before it parks in prvWaitForStart().

2. "macOS/arm64 stack-size fix" — sub-page task stack underflow.
   pxPortInitialiseStack() rounds the stack *end pointer* up to a page boundary.
   arm64 pages are 16 KB, but a FreeRTOS task stack (configMINIMAL_STACK_SIZE
   words x sizeof(StackType_t)) is typically smaller than one page (e.g. the
   idle task is ~2 KB). Rounding the end pointer then pushes it past the top of
   stack, so the `ulStackSize = (top - end)` subtraction underflows to a huge
   size_t and pthread_create() faults (EXC_BAD_ACCESS) trying to map a bogus
   stack. The failure is non-deterministic — it depends on the malloc alignment
   of each task's stack buffer. Fix: round the stack *size* up to a page
   instead, which can never underflow; the existing PTHREAD_STACK_MIN floor then
   gives every task at least a one-page host stack.
"""
import sys

PORT = "portable/ThirdParty/GCC/Posix/port.c"

HUNKS = [
    (
        "macOS/arm64 workaround",
        """    vPortEnterCritical();

    iRet = pthread_create( &thread->pthread, &xThreadAttributes,
                           prvWaitForStart, thread );

    if( iRet != 0 )
    {
        prvFatalError( "pthread_create", iRet );
    }

    vPortExitCritical();""",
        """    vPortEnterCritical();

    /* macOS/arm64 workaround: pthread_create() while every signal is masked
     * (vPortEnterCritical masks all) deadlocks in libsystem. Create the task
     * thread with a clean mask, then restore. Safe here: signals are delivered
     * directionally via pthread_kill to the current task only, so the new
     * thread cannot receive one before it parks in prvWaitForStart(). */
    {
        sigset_t xCleanMask, xSavedMask;
        sigemptyset( &xCleanMask );
        ( void ) pthread_sigmask( SIG_SETMASK, &xCleanMask, &xSavedMask );

        iRet = pthread_create( &thread->pthread, &xThreadAttributes,
                               prvWaitForStart, thread );

        ( void ) pthread_sigmask( SIG_SETMASK, &xSavedMask, NULL );
    }

    if( iRet != 0 )
    {
        prvFatalError( "pthread_create", iRet );
    }

    vPortExitCritical();""",
    ),
    (
        "macOS/arm64 stack-size fix",
        """    #ifdef __APPLE__
        pxEndOfStack = ( StackType_t * ) mach_vm_round_page( pxEndOfStack );
    #endif

    ulStackSize = ( size_t ) ( pxTopOfStack + 1 - pxEndOfStack ) * sizeof( *pxTopOfStack );

    #ifdef __APPLE__
        ulStackSize = mach_vm_trunc_page( ulStackSize );
    #endif""",
        """    ulStackSize = ( size_t ) ( pxTopOfStack + 1 - pxEndOfStack ) * sizeof( *pxTopOfStack );

    #ifdef __APPLE__
        /* macOS/arm64 stack-size fix: the upstream code rounds the stack *end
         * pointer* up to a page, but arm64 pages are 16 KB while a task stack is
         * often smaller than one page. That rounding pushes the end past the top
         * of stack, the subtraction above underflows to a huge size_t, and
         * pthread_create() faults mapping a bogus stack. Round the *size* up to
         * a page instead (never underflows); PTHREAD_STACK_MIN below is the
         * floor. */
        ulStackSize = ( size_t ) mach_vm_round_page( ulStackSize );
    #endif""",
    ),
]


def main() -> int:
    try:
        with open(PORT, "r") as f:
            src = f.read()
    except FileNotFoundError:
        print(f"freertos_posix_macos.py: {PORT} not found; skipping", file=sys.stderr)
        return 0

    changed = False
    for marker, old, new in HUNKS:
        if marker in src:
            continue  # already applied
        if old not in src:
            print(f"freertos_posix_macos.py: anchor for '{marker}' not found; "
                  "port layout changed", file=sys.stderr)
            return 1
        src = src.replace(old, new)
        changed = True
        print(f"freertos_posix_macos.py: applied '{marker}'", file=sys.stderr)

    if changed:
        with open(PORT, "w") as f:
            f.write(src)
    return 0


if __name__ == "__main__":
    sys.exit(main())

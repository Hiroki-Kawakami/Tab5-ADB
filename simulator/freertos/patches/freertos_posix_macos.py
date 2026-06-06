#!/usr/bin/env python3
"""Patch the FreeRTOS-Kernel GCC/Posix port for macOS (arm64).

Applied via FetchContent's PATCH_COMMAND. Idempotent: running it twice is a
no-op.

Problem: the POSIX port creates each task as a pthread from inside a critical
section, where vPortEnterCritical() has masked *every* signal (sigfillset).
On macOS/arm64, calling pthread_create() while all signals are blocked
deadlocks inside libsystem_pthread once a previously-created task thread is
parked in pthread_cond_wait — so vTaskStartScheduler() hangs while creating the
timer task and no task ever runs.

Fix: spawn the task thread with a clean (empty) signal mask, then restore the
caller's mask. This is safe for this port: task wakeups use condition variables
(event_signal), and the tick (SIGALRM) / resume signals are delivered
directionally with pthread_kill() only to the *current* task — never to a task
still being created — so the brief unmasked window cannot misdeliver a signal
to the new thread before it parks in prvWaitForStart().
"""
import sys

PORT = "portable/ThirdParty/GCC/Posix/port.c"

OLD = """    vPortEnterCritical();

    iRet = pthread_create( &thread->pthread, &xThreadAttributes,
                           prvWaitForStart, thread );

    if( iRet != 0 )
    {
        prvFatalError( "pthread_create", iRet );
    }

    vPortExitCritical();"""

NEW = """    vPortEnterCritical();

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

    vPortExitCritical();"""

MARKER = "macOS/arm64 workaround"


def main() -> int:
    try:
        with open(PORT, "r") as f:
            src = f.read()
    except FileNotFoundError:
        print(f"freertos_posix_macos.py: {PORT} not found; skipping", file=sys.stderr)
        return 0

    if MARKER in src:
        return 0  # already patched

    if OLD not in src:
        print("freertos_posix_macos.py: anchor not found; port layout changed",
              file=sys.stderr)
        return 1

    with open(PORT, "w") as f:
        f.write(src.replace(OLD, NEW))
    print("freertos_posix_macos.py: patched", PORT, file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())

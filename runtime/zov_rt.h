/*
 * zov_rt.h - the freestanding Zov runtime contract.
 *
 * Code compiled with `zovc --freestanding` has no libc and no startup
 * code of its own.  It expects the platform (kernel, bootloader, or a
 * test harness) to provide the symbols declared here.
 *
 * ABI notes:
 *   - This header is C99; include it from the C file that defines the
 *     hooks and compile that file with the same target flags as the
 *     generated Zov objects.
 *   - All pointers passed in are valid for the documented lifetime only.
 *     The runtime must not retain them unless it copies the contents.
 */

#ifndef ZOV_RT_H
#define ZOV_RT_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Required symbol: entry point.
 *
 * The compiler emits a definition of zov_kmain for the program's
 * `kmain` function.  The platform calls it once, after its own
 * initialization, from a context where the stack is usable.  The
 * signature has no arguments and no return value; freestanding Zov
 * programs report failure through the panic hook.
 */
void zov_kmain(void);

/*
 * Required symbol: panic hook.
 *
 * Called by generated runtime checks (bounds, slice ranges, integer
 * division by zero, forced unwrap, assert) when the program cannot
 * continue.  It must not return.
 *
 *   code  stable diagnostic code, never NULL ("E1001", "E1006", ...)
 *   msg   short human-readable description, never NULL
 *   file  Zov source file of the failing expression, never NULL
 *   line  1-based source line, or 0 when the site is not source-backed
 *   col   1-based source column, or 0 when the site is not source-backed
 *
 * A kernel typically formats this to a serial console or a log ring.
 * A minimal implementation may halt after copying `code` and `msg`
 * into platform diagnostics; it must not assume stdio, stderr, or a
 * terminal exists.
 */
void zov_panic(const char *code, const char *msg, const char *file,
               int line, int col);

/*
 * Allocation hooks.
 *
 * A freestanding program that owns a String (created with
 * String::fromowned) releases its buffer through zov_rt_free.  The
 * String growth helper uses zov_rt_realloc; current freestanding builds
 * reject the allocation operations that would reach it, so defining it
 * now is optional but keeps the ABI stable for future growth support.
 * Both follow malloc/realloc/free-compatible semantics:
 *
 *   zov_rt_realloc(NULL, n)  allocate n bytes, suitably aligned
 *   zov_rt_realloc(p, n)     grow/shrink, may move, preserves contents
 *   zov_rt_free(p)           release an earlier allocation (NULL is ok)
 *
 * String comparison and string match lowering call the platform's
 * memcmp; a program that never compares strings does not need it.
 */
void *zov_rt_realloc(void *ptr, size_t size);
void zov_rt_free(void *ptr);

#ifdef __cplusplus
}
#endif

#endif /* ZOV_RT_H */

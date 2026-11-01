/*
 * Example Zov freestanding runtime for tests and reference.
 *
 * This is NOT kernel code and does not claim to be: it links a
 * freestanding Zov object against a hosted C runtime so the panic hook,
 * entry point, and symbol contract can be exercised on a development
 * machine.  A real kernel implements the same declarations from
 * runtime/zov_rt.h against its own console and allocator.
 */

#include "../zov_rt.h"

#include <stdio.h>
#include <stdlib.h>

/* Hosted stand-in for a kernel entry shim: run the Zov program once. */
int main(void)
{
    zov_kmain();
    return 0;
}

/*
 * Panic hook: structured diagnostics first, then a human-readable line
 * when a terminal happens to exist.  A real freestanding runtime would
 * replace the fprintf with its own log sink.  Must not return.
 */
void zov_panic(const char *code, const char *msg, const char *file,
               int line, int col)
{
    fprintf(stderr, "Zov runtime error\n\nerror[%s]: %s\n  --> %s:%d:%d\n",
            code ? code : "E0902", msg ? msg : "(no message)",
            file ? file : "?", line, col);
    fflush(NULL);
    exit(101);
}

/* Reserved allocator hooks: malloc-compatible, currently unused by
 * generated freestanding code. */
void *zov_rt_realloc(void *ptr, size_t size)
{
    if (size == 0) {
        free(ptr);
        return NULL;
    }
    return realloc(ptr, size);
}

void zov_rt_free(void *ptr)
{
    free(ptr);
}

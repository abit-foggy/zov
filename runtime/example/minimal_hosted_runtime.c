/*
 * Minimal freestanding Zov runtime.
 *
 * Supplies only the symbols generated code actually requires: a host
 * entry shim for tests and the panic hook.  It deliberately omits the
 * reserved allocator hooks, proving they are optional at link time.
 *
 * This is not kernel code: a real platform replaces main() with its own
 * startup path and the loop below with its diagnostics sink.
 */

#include "../zov_rt.h"

/* Hosted stand-in for a kernel entry shim: run the Zov program once. */
int main(void)
{
    zov_kmain();
    return 0;
}

/* Required hook: generated checks call this and never return. */
void zov_panic(const char *code, const char *msg, const char *file,
               int line, int col)
{
    (void)code;
    (void)msg;
    (void)file;
    (void)line;
    (void)col;
    for (;;) {
    }
}

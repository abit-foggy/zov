# Zov freestanding runtime contract

`zovc --freestanding` emits C99 with no libc and no startup code of its
own. The platform supplies the symbols below; the compiler supplies the
program (including a definition of `zov_kmain`).

The interface is declared in [`zov_rt.h`](zov_rt.h).

## Required symbols

| Symbol | Signature | Called by |
|---|---|---|
| `zov_kmain` | `void zov_kmain(void)` | defined by the compiled Zov program; the platform calls it once |
| `zov_panic` | `void zov_panic(const char *code, const char *msg, const char *file, int line, int col)` | every generated runtime check |

`zov_panic` must not return. `code` is a stable diagnostic identifier
(`E1001` index out of bounds, `E1002` slice out of bounds, `E1003`
division by zero, `E1004` null unwrap, `E1005` error unwrap, `E1006`
failed assertion). `file`, `line`, and `col` identify the failing Zov
expression; `line`/`col` are `0` only when the check has no source site.

A kernel implementation typically formats the code and message into its
own log ring or serial console. It must not assume `stdio`, `stderr`,
`abort()`, or a terminal exists.

## Allocator hooks

```c
void *zov_rt_realloc(void *ptr, size_t size);
void  zov_rt_free(void *ptr);
```

These follow `realloc`/`free` semantics (suitably aligned results,
`free(NULL)` allowed, contents preserved on grow). A freestanding Zov
program uses them when it owns a `String`:

- `String::from_owned(data, len, cap)` is allowed in freestanding builds;
  the platform hands the program a buffer and `String.free()` releases it
  through `zov_rt_free`.
- `String::from`, string concatenation, `std::io`, `std::mem`,
  `std::vec`, `std::map`, and `std::arena` are rejected in freestanding
  mode with a diagnostic: they need an allocator or an operating system.
- `zov_rt_realloc` is part of the ABI and is used by the String growth
  helper; current freestanding programs cannot reach that helper because
  allocation from a `str` is rejected. Define both hooks so the object
  links now and future growth support needs no ABI change.

If a freestanding program compares strings (`==`, `<`, or a string
match), generated code calls the platform's `memcmp`, which therefore
must also be provided. Programs that use neither `String` nor `memcmp`
link against `zov_panic` alone.

## Minimal runtime

```c
#include "zov_rt.h"

void zov_panic(const char *code, const char *msg, const char *file,
               int line, int col)
{
    /* platform-specific reporting here */
    (void)code; (void)msg; (void)file; (void)line; (void)col;
    for (;;) { }
}
```

`runtime/example/hosted_runtime.c` is a host-linkable reference
implementation used by `tests/run.sh`; it adds a `main` shim so a
freestanding object can be executed on a development machine. It is not
kernel code. `runtime/example/minimal_hosted_runtime.c` is the same shim
with only the required `zov_panic` hook: linking against it proves the
reserved allocator symbols are optional.

## Building and linking

```sh
zovc --freestanding --emit-c kernel.zov > kernel.c
cc -std=c99 -pedantic-errors -ffreestanding -c kernel.c -o kernel.o
cc my_runtime.c kernel.o -o kernel.elf    # or your linker script
```

The generated object references only `zov_panic` (verified by `nm -u`
in the test suite). `zov_kmain` is defined by the object.

## Limits

- No startup code, stack setup, or linker script is provided.
- There is no general allocator API: `std::mem`, `std::vec`,
  `std::map`, and `std::arena` are hosted-only. A kernel can still wrap
  its own allocator as a C interface for Zov code.
- No hosted headers are emitted, and `String::from`, concatenation,
  `std::io`, `std::fs`, `std::env`, and `std::time` are rejected in
  freestanding mode with `E0306` instead of a link failure.
- `String::from_owned` is the only way to obtain an owned String; its
  buffer must satisfy the platform's `zov_rt_free` contract.
- This is not a claim of kernel readiness: it defines the boundary
  between a Zov program and the platform that runs it.

# Zov standard library environments

`zovc` compiles the same `.zov` sources for hosted programs and for
`--freestanding` kernels. This table records which modules are available
in each environment and what they depend on.

| Module | Hosted | Freestanding | Depends on |
|---|---|---|---|
| core language (`str` values, slices, `String::from_owned`) | yes | yes | nothing / platform buffer |
| `std::mem` | yes | no (`E0306`) | ISO C `calloc`/`realloc`/`free` |
| `std::vec` | yes | no (`E0306`) | `std::mem` |
| `std::map` | yes | no (`E0306`) | `std::mem` |
| `std::arena` | yes | no (`E0306`) | `std::mem` |
| `std::io` | yes | no (`E0306`) | ISO C `stdio.h`, POSIX `read` |
| `std::fs` | yes | no (`E0306`) | ISO C `stdio.h` |
| `std::env` | yes | no (`E0306`) | hosted process startup (argv) |
| `std::time` | yes | no (`E0306`) | POSIX `usleep` |
| `std::math` | yes | no (`E0306`) | ISO C `math.h` and `stdlib.h` |
| `std::str` | yes | no (`E0306`) | `std::vec` for `split_byte`/`lines` |

## Notes

- Strict C99 portability and operating-system portability are different
  claims. `std::fs` uses only ISO C, but `std::io` and `std::time` use
  POSIX functions; hosted builds define `_POSIX_C_SOURCE 200809L` for
  that reason.
- A freestanding build does not emit hosted headers for a module it
  rejects. The diagnostic is `E0306` at the first call site, for example
  `std::vec depends on std::mem and is unavailable in freestanding
  builds`.
- `std::str` is currently rejected as a whole because its splitting
  helpers allocate `Vec[str]`, even though `trim`, `starts_with`,
  `ends_with`, and `find_byte` allocate nothing. Splitting them into a
  separate pure module is a future cleanup.
- `std::math` mixes pure integer helpers (`min`, `max`, `clamp`, `abs`,
  `popcnt`) with functions backed by `<math.h>` and `rand()`. Module-level
  imports make the whole module hosted-only until those are split.
- A kernel can expose its own C functions with `use <header.h> as name;`
  and call them from Zov like any other C import. That is how a platform
  allocator or console is integrated today.
- Owned `String` buffers obtained from `String::from_owned` are released
  through `zov_rt_free`; see [runtime/README.md](../runtime/README.md).

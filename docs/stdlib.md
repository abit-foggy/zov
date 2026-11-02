# Zov standard library environments

`zovc` compiles the same `.zov` sources for hosted programs and for
`--freestanding` kernels. This table records which modules are available
in each environment and what they depend on.

| Module | Hosted | Freestanding | Depends on |
|---|---|---|---|
| core language (`str` values, slices, `String::fromowned`) | yes | yes | nothing / platform buffer |
| `std::mem` | yes | no (`E0306`) | ISO C `calloc`/`realloc`/`free` |
| `std::vec` | yes | no (`E0306`) | `std::mem` |
| `std::map` | yes | no (`E0306`) | `std::mem` |
| `std::arena` | yes | no (`E0306`) | `std::mem` |
| `std::io` | yes | no (`E0306`) | ISO C `stdio.h`, POSIX `read` |
| `std::fs` | yes | no (`E0306`) | `stdio.h`, POSIX `unistd.h`, `sys/stat.h`, `dirent.h` |
| `std::env` | yes | no (`E0306`) | hosted process startup, `stdlib.h`, `unistd.h` |
| `std::time` | yes | no (`E0306`) | POSIX `usleep` |
| `std::proc` | yes | no (`E0306`) | POSIX `fork`, `execvp`, `waitpid` |
| `std::math` | yes | partly: pure helpers yes, libm functions no (`E0306`) | pure Zov; `math.h`/`stdlib.h` only for hosted functions |
| `std::str` | yes | yes | nothing: zero-copy queries only |
| `std::strsplit` | yes | no (`E0306`) | `std::vec` for `splitbyte`/`lines` |

## Notes

- Strict C99 portability and operating-system portability are different
  claims. `std::fs` uses only ISO C, but `std::io`, `std::time`, and
  `std::env` use POSIX functions (`read`, `usleep`, `getcwd`, `chdir`,
  `setenv`); hosted builds define `_POSIX_C_SOURCE 200809L` for that
  reason.
- A freestanding build does not emit hosted headers for a module it
  rejects, nor for libm-backed `std::math` declarations that were skipped.
  The diagnostic is `E0306` at the first call site, for example
  `std::vec depends on std::mem and is unavailable in freestanding
  builds` or `std::math::pow uses the hosted C math library`.
- Standard-library names use vintage UNIX-style smooshed lowercase:
  `readline`, `splitbyte`, `startswith`, `getref`, `findref`, `freeptr`.
  No user-facing function in `lib/std` uses underscores.
- `std::str` is pure: `eq`, `startswith`, `endswith`, `findbyte`, and
  `trim` are zero-allocation sub-slice queries and work in kernels.
  Allocating `splitbyte` and `lines` live in `std::strsplit`, which is
  hosted-only because it returns `Vec[str]`.
- `std::math` keeps the established single-module API but gates each
  function. Pure helpers (`min`, `max`, `clamp`, `abs`, `sqrt`, `popcnt`,
  `nextpow2`) are checked and emitted in freestanding builds; `sqrt` uses
  a portable Newton iteration rather than libm. `randint` and the libm
  functions (`pow`, trig, rounding, `fmin`/`fmax`) report `E0306` at the
  call site, and their C headers are not emitted.
- A kernel can expose its own C functions with `use <header.h> as name;`
  and call them from Zov like any other C import. That is how a platform
  allocator or console is integrated today.
- `std::env` exposes `argscount`, `argat`, zero-copy `get`, owned
  `getstr`, `set`, `unset`, owned `cwd`, and `chdir`. `cwd` grows its
  buffer and returns `EnvNotFound` on failure. `std::fs` exposes
  `readstr` (owned `String`), `writestr`, `exists`, `isfile`, `isdir`,
  `mkdir`, `rename`, `remove`, and `rmdir`; every path is copied to a
  temporary NUL-terminated buffer and released before return.
- `std::proc::run(prog, args)` forks, `execvp`s the program with a
  temporary NUL-terminated argv, waits for the child, frees every
  temporary buffer, and returns the exit code (`128 + signal` when the
  child was signaled, or `ProcError` for fork/wait failure). It is
  hosted-only and reports `E0306` in freestanding builds.
- `Map[K, V]` supports owned `String` keys: keys hash and compare by
  bytes, insertion/replacement/removal/clear/free destroy owned keys and
  values, and the fallible `findref`/aborting `getref` lookups borrow the
  map for the lifetime of the returned reference.
- `Map[String, V]` adds zero-allocation `str` probes: `findstr`,
  `getstrref`, `containsstr`, `removestr`, and `takestr`. They hash and
  compare the borrowed bytes directly, so `$PATH`-style lookups never
  allocate a temporary `String`. `findstr` and `containsstr` are
  fallible/boolean; `getstrref` aborts on a missing key like `getref`.
- Canonical intrinsic spellings are smooshed lowercase:
  `String::fromowned` and `hashval`. `String::from_owned` and `hash_val`
  remain accepted as compatibility aliases.
- Module-private C imports (`use <unistd.h> as u;` inside `std::io`) are
  not visible to importers in hosted or freestanding builds.
- Owned `String` buffers obtained from `String::fromowned` are released
  through `zov_rt_free`; see [runtime/README.md](../runtime/README.md).
- Packaging and dependencies are managed via `sklad`; see [sklad.md](sklad.md)
  for package manifests (`zov.toml`), remote fetching, and caching.

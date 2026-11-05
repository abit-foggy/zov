# Zov (`zovc` & `sklad`)

Zov is a safety-hardened systems programming language that compiles to strict ISO C99. It provides owned resource tracking, borrow checking, exhaustive union matching, and zero-underscore UNIX standard library ergonomics without runtime bloat.

- **`zovc`** — 100% offline compiler, type/borrow checker, and local project driver (`init`, `check`, `build`, `run`).
- **`sklad`** — Networked package, dependency cache, and toolchain manager written in Zov, built on top of `zovc`.

## Specifications

| Property | Specification |
|---|---|
| **Current Version** | `0.9.0` (`edition = "2026"`) |
| **Backend Target** | Strict ISO C99 (`-std=c99 -pedantic-errors`) + TinyCC (`libtcc`) |
| **Modes** | Hosted (POSIX) and Freestanding (`--freestanding -nostdlib`) |
| **Memory Model** | Owned resources (`String`, `Vec`, `Map`, `Arena`), deterministic overwrite/drop cleanup, borrow checking (`&T`, `&mut T`) |
| **Naming Style** | Smooshed lowercase (`println`, `readline`, `startswith`, `splitbyte`, `getref`, `findref`) |
| **Supported Platforms** | Linux (`x86_64`, `aarch64`), macOS (`x86_64`, `aarch64`) |
| **License** | BSD-2-Clause |

## Installation

Install both `zovc` and `sklad` (Linux & macOS `x86_64` / `aarch64`):

```sh
curl -fsSL https://raw.githubusercontent.com/abit-foggy/zov/master/install.sh | sh
```

Install `zovc` only (offline compiler & stdlib) or `sklad` only:

```sh
# Install only offline zovc (compiler + stdlib + runtime)
curl -fsSL https://raw.githubusercontent.com/abit-foggy/zov/master/install.sh | sh -s -- --zovc-only

# Install only sklad (package & toolchain manager)
curl -fsSL https://raw.githubusercontent.com/abit-foggy/zov/master/install.sh | sh -s -- --sklad-only
```

### Build from Source

```sh
git clone https://github.com/abit-foggy/zov.git
cd zov
make
sudo make install
```

## Quick Start

### Offline Workflow (`zovc`)

```sh
zovc init hello
cd hello
zovc run
```

```zov
use std::io;

export fn main() i32 = {
    io::println("hello from zov");
    return 0;
};
```

### Package & Dependency Workflow (`sklad`)

```sh
sklad init myapp
cd myapp
sklad add mylib --git https://github.com/abit-foggy/mylib.git
sklad run
```

## Documentation

Planned Documentation: Official language reference, ownership manual, and standard library documentation site coming soon ([Link TBD]).

Local documentation is available in [docs/](docs/):
- [docs/ownership.md](docs/ownership.md) — Ownership, destruction, and borrow rules
- [docs/stdlib.md](docs/stdlib.md) — Hosted and freestanding standard library reference
- [docs/sklad.md](docs/sklad.md) — Package and toolchain management with sklad
- [runtime/README.md](runtime/README.md) — Freestanding runtime contract

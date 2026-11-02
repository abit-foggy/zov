# Zov Programming Language

Zov is a modern, statically typed systems programming language featuring strict memory safety, ownership and borrow checking, seamless C interoperability, and an offline-first architecture.

## Overview

- **`zovc`**: The 100% offline reference compiler and driver. Compiles single files or offline multi-module packages. Emits clean, strict ISO C99.
- **`sklad`**: The dogfooded package, cache, and toolchain manager written in Zov (`tools/sklad/main.zov`). Resolves remote Git repositories and URL tarballs, verifies cryptographic hashes, generates `sklad.lock`, and orchestrates builds with `zovc`.
- **`zov-lsp`**: Language Server Protocol implementation written in Zov (`tools/zov-lsp/main.zov`) powering IDE extensions (VS Code, Zed, CLion).

## Quick Start

### Building from Source

Prerequisites: a C99 compiler (`cc` or `gcc` or `clang`) and standard UNIX build tools.

```bash
make
```

This compiles:
1. `zovc` (the offline compiler)
2. `zov-lsp` (the language server, dogfooded by `zovc`)
3. `sklad` (the package manager, dogfooded by `zovc`)

### Running Tests

```bash
make test
```

### Installation

Install `zovc`, `sklad`, `zov-lsp`, standard libraries, and runtime headers:

```bash
sudo make install
```

Or perform a staged install without root:

```bash
make install PREFIX=/path/to/prefix
```

## Documentation

- [Package & Toolchain Manager (`sklad`)](docs/sklad.md): Details the architectural boundary between `zovc` and `sklad`, manifest schema (`zov.toml`), lockfiles (`sklad.lock`), and cache layout.
- [Standard Library](docs/stdlib.md): Guide to hosted and freestanding standard modules (`std::io`, `std::fs`, `std::vec`, etc.).
- [Ownership & Borrowing](docs/ownership.md): Affine type system, borrow rules, and lifetime semantics.
- [Runtime Contract](runtime/README.md): C runtime integration and memory management.

# Zov ownership and destruction

This document describes what the compiler currently enforces. It is not a
claim of Rust-level memory safety: there are no destructor traits, no
scope-exit cleanup, and no lifetime syntax.

## Value classes

| Kind | Copy or move | Owns resources | Notes |
|---|---|---|---|
| scalars, `bool`, `char`, enums | copy | no | plain C values |
| raw pointers, `&T`, `&mut T`, `str`, slices | copy | no | pointers/views; raw pointers are outside tracked ownership |
| `String` | move | yes, one heap buffer | `String::new`, `String::from`, `String::from_owned` |
| `resource struct` | move | yes, its fields | linear by declaration |
| struct with a linear field | move | yes | linearity is propagated by the compiler |
| tagged union with a linear member | move | yes, active payload | tag selects what is destructible |
| `Vec[T]`, `Map[K,V]`, `Arena` | move | yes | declared `resource struct` in `lib/std` |

`let b = a` moves a linear value (`E0503` on later use of `a`). Shared
references are copied; `&mut T` is linear. Passing by value moves, while
`&T` / `&mut T` parameters borrow. Returning a value moves it to the
caller; returning a reference to a local is rejected (`E0512`).

A direct field of a resource container may be moved out (a *partial
move*). The container then cannot be moved or freed as a whole
(`E0510`), but disjoint fields stay usable. Moving deeper than one field,
or moving a field out of a non-resource struct, is rejected (`E0511`).

## Assignment and reassignment

A plain `place = value` on an *initialized* owned place destroys the old
value before storing the new one. The compiler emits one per-type helper
(`drop` then store) so the destination is evaluated once and the release
happens in C99-sequenced order:

- locals, struct fields, and fields reached through `&mut T` are covered;
- tagged unions destroy only the active payload before the new tag/payload
  is written;
- the right-hand side is evaluated once, and self-assignment of a moved
  handle does not free the value being copied.

If the place is definitely uninitialized (moved or dropped), assignment
reinitializes it without trying to release the old contents (`E0503` and
`E0509` are cleared). Reassigning after a field was moved out of the whole
struct is rejected (`E0510`) because the remaining fields still own
resources.

A place that may be initialized is rejected (`E0517`): if one branch of an
`if` moves or frees a value and another leaves it live, a later overwrite
has no single safe action. Move/free on every branch, or restructure so
the assignment happens before the branch.

Assignment through a raw-pointer index is not automatically destructive
(the compiler does not track pointee ownership). Container APIs such as
`Vec.set`, `Vec.replace`, and `Map.insert` perform the release themselves.

## Explicit destruction

Zov has no scope-exit destructors. These operations release resources:

- `String.free()` returns the buffer; the handle is left empty.
- `mem::drop(place)` releases everything the place owns without
  releasing the place's own storage:
  - `String`: frees its buffer.
  - struct: recursively drops owned fields, unless the type has a
    by-value `free()` method, in which case that method is called.
  - union: switches on the tag and drops only the active payload.
  - array: drops each element.
  - trivial values (`i32`, `str`, slices, raw pointers): no-op.
  - `Vec` / `Map` / `Arena`: calls their `free()` method, which releases
    elements and backing storage.
  `mem::drop` requires a place (variable, field, index, or dereference);
  it rejects temporaries (`E0605`). If the place is reached through a
  reference (`*r`, `r.field`, `r.items[i]`), it is rejected with `E0605`:
  the owner would keep believing it still owns the resources, so a later
  `free()` could double-free. Drop the owning binding instead. Raw-pointer
  dereferences remain in the unchecked subset. The place is poisoned afterwards:
  use is `E0504`, a second drop is `E0509`, and dropping a moved value
  is `E0503`.
- `Vec.free()` drops every initialized element and the backing array.
  `Vec.clear()` drops elements but keeps the buffer.
  `Vec.pop()` and `Vec.remove(idx)` transfer the element to the caller.
  `Vec.replace(idx, val)` returns the old element.
  `Vec.set(idx, val)` drops the old element before storing the new one.
- `Map.free()` drops every occupied key/value pair and all tables.
  `Map.clear()` drops entries but keeps the tables. `Map.remove(key)`
  destroys the stored key and value; `Map.take(key)` returns the value.
  `Map.insert` drops the previous value when a key is replaced.
- `Arena.free()` releases the backing buffer. `Arena.reset()` only resets
  the bump offset; it destroys nothing.

Freeing or dropping while a borrow of the place is live is rejected
(`E0502`), as is freeing a moved field (`E0503`). Calling `free()` on a
field reached through a reference is rejected (`E0605`): the owner would
not know the field was released. Free the owning binding instead. A
deferred cleanup that frees a value pins it against moves (`E0515`).

## Structs

```zov
type Holder = resource struct {
    label: String,
    id: i32,
};
```

Moving a `Holder` transfers the owned label. Moving `h.label` out leaves
`h` partially moved: `h.id` stays readable, `h` cannot be moved or
dropped. `mem::drop(h)` frees the label exactly once.

A struct that contains a container but is itself not declared `resource`
is still linear, because a `Vec`/`Map` field is linear.

## Unions

```zov
type Result = (String | IoError);
```

`mem::drop(u)` reads the active tag and destroys only that payload.
Extracting a linear payload with `match` moves the union, so dropping the
union afterwards is rejected. Assigning a new variant to an initialized
union destroys the currently active payload first, so switching between
owned variants does not leak.

## Containers

`Vec[T]` uses a raw `*T` buffer, so the compiler cannot see element
ownership through the pointer. `Vec.free`/`clear` close that gap by
calling `mem::drop[T]` for each initialized element, which resolves the
element type at monomorphization. Nested containers work because a
`Vec`'s `free` method is itself a drop hook.

`Map[K,V]` destruction skips tombstones and copies values during growth
without dropping them (ownership moves to the new tables). Hash keys
currently support `str`, integers, and enums, not owned `String`
(`E0806`).

`Vec.get` and `Map.get` return a shallow copy of the stored value. That
is safe only for copyable element types. Calling `get()` when the element
type owns resources (`String`, `Vec`, `Map`, `Arena`, `&mut T`, or a
resource-containing struct/union) is rejected with `E0611`, because the
returned copy would alias the container's allocation.

Use `Vec.get_ref(idx)` / `Map.get_ref(key)` for borrowed reads: they
return `&T` / `&V`, keep the container borrowed while the reference is
live, and abort on an out-of-bounds/missing key. Use `pop`, `remove`,
`replace`, or `take` when ownership must move out of the container.
These APIs are deliberately distinct from `get()`: no safe path hands out
a second owner of a resource.

## Arenas

An arena owns one byte buffer. Values placed inside it keep their own
ownership rules: resetting or freeing the arena does not call
`mem::drop` on anything stored in the bump region. Drop owned values
first, then reset or free. `alloc_bytes` checks alignment and capacity
with `E1006` rather than wrapping the offset.

## Allocation

`std::mem` is hosted-only:

- `alloc[T](count)` zeroes memory and rejects `count * sizeof(T)`
  overflow (`E1006`).
- A zero count allocates one element, so the result is never null.
- `realloc_buf[T]` rejects size overflow and stores the result only after
  success, so a failed growth never loses the original pointer.
- Allocation failure aborts the process; there is no recoverable
  allocation-error type.

`String` growth checks `SIZE_MAX` overflow and raises `E1007` on
out-of-memory. The generated helpers use `realloc`/`free` in hosted builds
and `zov_rt_realloc`/`zov_rt_free` in freestanding builds.

## Hosted and freestanding

Hosted builds can use all of `std`. Freestanding builds reject
`std::mem`, `std::vec`, `std::map`, `std::arena`, `std::io`, `std::fs`,
`std::env`, `std::time`, and `std::strsplit` with `E0306` at the call
site, plus `String::from` and string concatenation. Pure `std::math`
helpers (`min`, `max`, `clamp`, `abs`, `sqrt`, `popcnt`, `nextpow2`) and
all of `std::str` (`trim`, `starts_with`, `ends_with`, `find_byte`,
`eq`) work freestanding; the libm-backed math functions and allocating
splitting helpers are gated individually (`math::pow`, `strsplit::lines`,
...). `String::from_owned` remains available: the platform provides the
buffer and `String.free()` calls the `zov_rt_free` hook. See
[runtime/README.md](../runtime/README.md).

## Known limits

- Overwrite destruction covers locals and struct fields. Assigning
  through a raw-pointer index does not know the element's ownership;
  prefer `Vec.set`/`replace` or index assignment only for trivial types.
- Dropping a raw-pointer value is a no-op: the pointer carries no
  ownership information. Dropping through `*p` is not checked against
  the allocation's real lifetime, so it belongs to the unsafe subset.
- The borrow checker tracks bindings, first-level fields, views, and
  explicit loans. It does not track individual container elements,
  arbitrary pointer provenance, or reallocations. Treat raw pointers and
  element references as unsafe.
- No destructor traits, scope-exit cleanup, or `defer`-based resource
  drop exists. `defer` only delays a call the program wrote.

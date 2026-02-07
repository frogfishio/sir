# `data:v1` — baseline data story (verifier-enforced)

`data:v1` is a **convention pack**: it does not add new mnemonics, but it gives
producers a single, portable “data story” for the most common higher-level
datatypes (bytes, UTF-8 strings, and C strings).

Unlike pure documentation, `data:v1` is **enforced by `sircc --verify-only`**:
if a stream enables `data:v1` via `meta.ext.features`, the canonical types MUST
be present and have the exact shapes described below.

## Canonical types

Enable the pack:

```json
{"ir":"sir-v1.0","k":"meta","ext":{"features":["data:v1"]}}
```

Then define these types (ids may be integers or strings):

### `bytes`

An owned or borrowed byte slice.

- `bytes = struct { data: ptr(i8), len: i64 }`
- `len` is a byte count.
- This is **not** NUL-terminated; embedded `0` bytes are allowed.

### `string.utf8`

A UTF-8 string slice.

- `string.utf8 = struct { data: ptr(i8), len: i64 }`
- Bytes are UTF-8 by convention. Under `data:v1`, the encoding is carried by the type name (`string.utf8`) and is not separately tagged in `meta`.
- Not NUL-terminated; embedded `0` bytes are allowed.

### `cstr`

A C string pointer (NUL-terminated bytes).

- `cstr = ptr(i8)` (a ptr type whose `of` is `i8`)
- The trailing `\0` is a **producer/host convention**, not validated by SIR.

## Interop guidance (portable, explicit)

- Prefer passing raw `(ptr(i8), len)` to “write-like” ABIs (`zi_write`, etc.).
- Only use `cstr` when calling ABIs that require NUL-terminated strings.
- Conversions between `string.utf8` and `cstr` should be explicit calls to a
  runtime helper (zABI / host-provided capability), not implicit compiler magic.

## Explicit conversions (no implicit magic)

`data:v1` intentionally avoids compiler-magic conversions. Producers must emit explicit conversions.

### String literals → `cstr`

For *literal* C strings, use the dedicated `cstr` node:

```json
{"ir":"sir-v1.0","k":"node","id":"lit","tag":"cstr","type_ref":"p:cstr","fields":{"value":"hello\\n"}}
```

This yields a `ptr(i8)` suitable for passing to C ABIs like `puts`.

Strict mode note:

- Under `data:v1` + `--verify-strict`, `cstr` nodes must set `type_ref` to the canonical `cstr` type.

### UTF-8 slices → `cstr` (owned)

For a runtime `string.utf8`/`bytes` slice, allocate `len+1`, copy bytes, and store a trailing `0`.

This is intentionally left as a **producer/library pattern** today (there is no blessed builtin helper
yet), because it depends on:

- an allocator capability (`zi_alloc` / `zi_free`, or libc `malloc` / `free`), and
- a lifetime policy (“who frees?”).

Recommended policy for integrators:

- Provide a small helper function in your runtime / prelude (e.g. `utf8_to_cstr_alloc`)
  that returns an owned `cstr` and requires the caller to free with the same allocator.

### `cstr` → UTF-8 slice

This also must be explicit (and is ABI-dependent):

- “borrowed slice”: compute `strlen` and return `{data, len}` (lifetime = underlying C memory)
- “owned slice”: allocate + copy (caller frees)

## Examples

- `src/sircc/examples/data_v1_ok.sir.jsonl` (positive)
- `src/sircc/examples/bad_data_v1_string_wrong_fields.sir.jsonl` (negative)

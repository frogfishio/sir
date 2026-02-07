# Target + layout contract (LLVM backend)

SIR is target-agnostic, but **code generation is not**. If you want deterministic binaries and
portable producer behavior, you need an explicit contract for:

- which target you are compiling for, and
- what in-memory layout rules you are allowed to assume.

This document states the contract `sircc` follows when compiling via LLVM, and what producers
should do to avoid “works on my machine” layout bugs.

---

## Pin the target (recommended for all integrators)

`sircc` uses the target triple from:

1) `meta.ext.target.triple` (if present), unless
2) overridden by CLI `--target-triple <triple>`

Optional target knobs:
- `meta.ext.target.cpu` (defaults to `"generic"`)
- `meta.ext.target.features` (LLVM feature string, target-specific)

Practical producer guidance:
- Always set `meta.ext.target.triple` in emitted streams.
- In CI / pipelines, run `sircc --require-pinned-triple` so “forgot to pin” fails fast.

You can inspect the active target with:
- `sircc --print-target`

---

## Pointer size / alignment

Under LLVM, pointer width and pointer alignment come from the selected target.

Implications for producers:
- Do not assume `ptr == i64` or `ptr == i32`.
- Use the pointer mnemonics when you need size/offset decisions:
  - `ptr.sizeof`
  - `ptr.alignof`
  - `ptr.to_i64` / `ptr.from_i64` (when you must round-trip; prefer avoiding int↔ptr casts unless required)

---

## Struct layout (`type.kind:"struct"`)

`sircc` lowers `struct` types to LLVM structs with the selected target’s ABI rules.

What you may assume:
- field order is preserved
- field offsets / padding follow the target ABI
- total `sizeof` and `alignof` follow the target ABI

What you should not assume without pinning:
- exact padding bytes
- `alignof` values
- field offsets

Producer guidance:
- If you need to take the address of a field, prefer *not* doing manual byte math in the producer.
- If you must do it today, only do it under a pinned target contract and treat offsets as target-specific.

---

## Array layout (`type.kind:"array"`)

Arrays are contiguous sequences of elements in LLVM.

`sircc` treats `type.kind:"array"` as ABI-defined by the target; producers may safely assume:
- element `i` is at `base + i * stride`, where `stride` is the element size rounded up to element alignment.

If you want element addressing, prefer:
- `ptr.offset` (scales by `sizeof(element_type)`)

---

## Sum layout (`type.kind:"sum"` via `adt:v1`)

For ADTs/sums, `sircc` enforces a deterministic layout contract (independent of language):

Representation is a struct:
- `tag: i32` at offset `0`
- payload bytes start at the lowest offset ≥ 4 that satisfies `payload_align`
- payload size is `max(sizeof(variant_payload))`
- total size is rounded up to `align = max(4, payload_align)`

This makes:
- tag offset stable (`0`)
- payload offset deterministic given the variant types + target ABI

Producers should:
- use `adt:*` mnemonics to construct and access sums
- avoid “peeking” into payload bytes manually

---

## Closure layout (`type.kind:"closure"` via `closure:v1`)

`sircc` lowers closures as a by-value aggregate:

```
{ code_ptr, env }
```

Where:
- `env` is the lowered representation of `type.env`
- `code_ptr` is a function pointer with derived signature:
  - `(env, callSig.params...) -> callSig.ret`

Producers should:
- treat closures as opaque values and use `closure.*` mnemonics
- avoid assuming field offsets

---

## Vector ABI notes (`type.kind:"vec"`)

LLVM vector lowering is target-ABI-defined.

One intentional `sircc` determinism rule:
- `vec(bool, N)` is represented as `<N x i8>` (0/1) rather than `<N x i1>`

Producers should:
- treat vectors as opaque values (use vector mnemonics), not byte layouts

---

## Load/store alignment (best practice)

Many memory ops accept an optional `align`:
- `load.*` / `store.*`

Producer guidance:
- always set `align` when you know it (e.g. from `ptr.alignof(T)` or from your own layout tables)
- if you don’t know, omit it and let `sircc` default conservatively

---

## Practical “don’t shoot yourself” checklist

- Pin `meta.ext.target.triple` for all non-toy programs.
- Use `ptr.offset` rather than manual `i64.mul + ptr.add` for element addressing.
- Avoid depending on struct field offsets unless the target is pinned and you’ve frozen a layout contract.
- Treat sums and closures as opaque; use the pack mnemonics.
- Prefer explicit alignment on loads/stores; don’t guess.


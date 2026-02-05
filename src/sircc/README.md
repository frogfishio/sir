# sircc

`sircc` is a WIP compiler from SIR JSONL (one JSON object per line) to native binaries via LLVM.

## Status

- Parses `sir-v1.0` JSONL records (one JSON object per line) with a small built-in JSON parser (plain C).
- Lowers a small subset of `k:"type"` + `k:"node"` to LLVM IR using the LLVM C API.
- Emits an object file with LLVM and links with `clang`.

## Example

- `cmake -S . -B build -G Ninja -DLLVM_DIR=$(brew --prefix llvm)/lib/cmake/llvm`
- `cmake --build build`
- `./build/src/sircc/sircc src/sircc/examples/add.sir.jsonl -o add`

## CLI

- `--verify-only` parses + validates only (no codegen).
- `--emit-llvm` writes textual LLVM IR to `-o`.
- `--emit-obj` writes an object file to `-o`.
- `--clang <path>` chooses the linker driver (default: `clang`).
- `--target-triple <triple>` overrides the target triple for object emission.

## `meta.ext` (sircc-defined conventions)

These are conventional keys used by `sircc` under the `k:"meta"` record’s `ext` object.

- `meta.ext.target.triple` (string): default target triple (overridden by `--target-triple`).
- `meta.ext.target.cpu` (string, optional): LLVM CPU string (defaults to `"generic"`).
- `meta.ext.target.features` (string, optional): LLVM target feature string (defaults to empty).
- `meta.ext.features` (array of strings): feature gates (e.g. `fun:v1`, `closure:v1`, `adt:v1`, `sem:v1`, `agg:v1`).

Note: this example’s `main` takes two `i32` arguments because we don’t yet model argv lowering; you can still inspect IR with `--emit-llvm`.

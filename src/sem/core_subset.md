# SEM runnable Core subset (Stage B)

This document freezes what **SEM can execute today** under `sem --run`.

It is intentionally **smaller than SIR v1.0**: SEM is the deterministic emulator + validator used for integrator testing, not the full compiler backend.

Authoritative support surface:

- `sem --print-support` (text)
- `sem --print-support --json` (machine-readable)

## Target model (SEM)

SEM executes with a pinned target model:

- pointers are 64-bit (`ptr` is 8 bytes, aligned to 8)
- little-endian constant materialization for multi-byte loads/stores

If/when we add `unit.target` interpretation to SEM, this doc becomes “the default pinned target” for runnable fixtures.

## What to use it for

SEM is the “fast loop”:

- validate your emitted `.sir.jsonl`
- run small programs deterministically (with caps) before shipping a native build via `sircc`

## Curated runnable examples (smoke suite)

These files cover the “compiler kit” Core shapes we expect frontends to emit early:

- Calls (direct): `src/sem/tests/fixtures/call_direct_internal.sir.jsonl`
- Calls (extern): `src/sircc/examples/hello_zabi25_write.sir.jsonl`
- Globals + structured consts: `src/sem/tests/fixtures/global_struct_const_struct_zero.sir.jsonl`
- Arrays + `ptr.offset`: `src/sircc/examples/global_array_const.sir.jsonl`
- CFG form basics: `src/sircc/examples/cfg_if.sir.jsonl`, `src/sircc/examples/cfg_join_phi.sir.jsonl`

Run one:

```
sem --run <path/to/file.sir.jsonl>
```

Batch-verify/run:

```
sem --check <file-or-dir>...
sem --check --check-run <file-or-dir>...
```

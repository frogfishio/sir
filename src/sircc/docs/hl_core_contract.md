# SIR-HL → SIR-Core: ownership, contracts, and plan

This document defines how we “take load off MIR” without turning `sircc` into a bespoke compiler for every source language.

The core idea is a **two-level SIR contract**:

- **SIR-Core**: a small, stable, executable subset. This is the hard boundary for codegen correctness and long-term stability.
- **SIR-HL (packs / intent)**: more expressive, but **deterministically lowered** into SIR-Core by a shared `sircc` legalizer pipeline.

If you meet the SIR-Core contract, `sircc` is “just a compiler”. If you emit SIR-HL, `sircc` becomes your shared lowering engine.

## Ownership model

### Frontend / generator (language side) owns

These are **language semantics** and must not be pushed into SIR:

- parsing, scoping, name resolution
- type checking / inference
- language-specific evaluation rules (unless encoded explicitly as SIR-HL intent)
- surface sugar decisions (e.g. Oberon designators, report-spec corner cases)

### `sircc` (SIR toolchain) owns

These are **portable compilation problems** that every language hits:

- lowering of *portable intent* (`sem:v1`) into CFG + core ops
- closure conversion *when encoded as portable closure pack* (`closure:v1`)
- ADT layout and tag/payload operations *when encoded as portable sum pack* (`adt:v1`)
- aggregate layout + initializers (`type.kind:"struct"`, `const.*`, globals) under a pinned target contract
- verification, diagnostics, and reproducible codegen contracts

### What “SIR takes care of” means

SIR can only “take care of it” if:

1. the construct is **language-agnostic**, and
2. the lowering is **fully specified and deterministic**, and
3. the lowering results in **SIR-Core only**.

If any of those is false, the construct stays frontend-owned.

## The contracts

### SIR-Core contract (stable, codegen boundary)

SIR-Core is the set that:

- is **fully verified** by `sircc --verify-only`
- is **fully codegenned** by `sircc` (LLVM backend)
- is expected to be **stable across languages** and long-lived

In this repo’s current terminology, SIR-Core is:

- **Base**: “Stage 3 base v1.0” (CFG, blocks/terms, loads/stores, integer/float ops, etc.)
- **Interop**: extern import/export rules (function + global symbols)
- **Pinned target contract**: layout/size/align/endian when reproducibility matters

And (because these are already deterministic + verified + codegenned) SIR-Core also includes the
currently-implemented “data-model” packs that do **not** require an extra legalizer stage:

- `agg:v1` (globals + structured constants)
- `fun:v1`
- `closure:v1`
- `adt:v1`
- `simd:v1`

In other words: if `sircc --verify-only` accepts it and `sircc` can codegen it without any additional
lowering pipeline, it belongs to the SIR-Core contract surface.

The exact support set is surfaced by:

- `sircc --print-support --format json` (authoritative)

### SIR-HL contract (expressive, but not directly codegen)

SIR-HL is anything beyond SIR-Core that:

- is gated by `meta.ext.features` (packs)
- **must** be accepted by the verifier only when the feature gate is enabled
- **must** be lowered by `sircc` into SIR-Core before codegen

SIR-HL comes in two shapes:

1) **Packs**: `agg:v1`, `fun:v1`, `closure:v1`, `adt:v1`, …
2) **Intent**: `sem:v1` (`sem.if`, `sem.and_sc`, `sem.or_sc`, `sem.match_sum`, …)

Current implementation note:

- Today, the only SIR-HL family that `sircc` *lowers away* is `sem:v1`.
- Other packs listed above are *feature-gated*, but are treated as SIR-Core for codegen (no extra legalizer stage).

## Blessed subsets (normative)

To avoid “split personality” lowering across frontends, we freeze two concrete subsets:

### Blessed SIR-HL subset (what AST emitters may generate)

SIR-HL is intentionally **small**. Today it is exactly:

- `sem:v1` intent nodes:
  - `sem.if`
  - `sem.and_sc`
  - `sem.or_sc`
  - `sem.match_sum` (requires `adt:v1`)

Constraints:

- SIR-HL nodes MUST be feature-gated via `meta.ext.features:["sem:v1"]`.
- SIR-HL nodes MUST be accepted by `sircc --verify-only` when the gate is present.
- SIR-HL nodes MUST be eliminated by `sircc --lower-hl` (output contains **no** `sem.*`).
- If a frontend needs more “intent” than the list above, it must either:
  - be added as a new `sem:*` mnemonic with deterministic lowering, or
  - remain frontend-owned (lower directly to Core).

### Blessed SIR-Core subset (what backends/codegen accept)

SIR-Core is the executable contract surface that `sircc` codegens directly (LLVM backend).

Normative boundary:

- A SIR stream is SIR-Core if it contains **no** `sem.*` nodes and passes `sircc --verify-only`.

At a high level, SIR-Core includes:

- Records: `meta`, `src`, `type`, `sym`, `node`
- Types:
  - base: `prim`, `ptr`, `array`, `fn`, `struct`
  - feature-gated additions: `vec` (`simd:v1`), `fun` (`fun:v1`), `closure` (`closure:v1`), `sum` (`adt:v1`)
- Nodes (families):
  - CFG + control flow: `fn`, `block`, `bparam`, `term.*`
  - locals/values: `param`, `name`, `let`, `const.*`
  - calls: `call`, `call.indirect`, `call.fun` (`fun:v1`), `call.closure` (`closure:v1`)
  - memory: `alloca*`, `load.*`, `store.*`, `mem.copy`, `mem.fill`
  - pointers: `ptr.*` (including symbol addresses via `ptr.sym`, with producer rules enforced)
  - globals/constants (`agg:v1`): `sym(kind=var|const)` + structured `const.*` and helpers like `cstr`
  - SIMD (`simd:v1`): `vec.*`, `load.vec`, `store.vec`
  - fun/closure/ADT packs: `fun.*`, `closure.*`, `adt.*` (all feature-gated)

Everything else is outside the blessed Core subset and must be rejected by the verifier unless/until a
feature gate and implementation exist.

## Interchange + versioning rules

### Inputs

- Primary interchange is `sir-v1.0` JSONL.
- Producers may use int or string ids; string ids are recommended for injected/derived nodes.

### Outputs (lowered)

We add a `sircc` lowering stage that produces a **lowered SIR-Core stream**:

- `sircc --lower-hl --emit-sir-core out.sir.jsonl in.sir.jsonl`
- output is still `sir-v1.0`, but guaranteed to satisfy the SIR-Core contract

This output becomes:

- canonical for debugging (“what did your HL mean?”)
- the unit of testing for optimizer/legalizer changes

Note: the current MVP implementation only lowers the “pure/val” cases of `sem:v1` (see P1), and will reject thunk-based `sem:*` nodes with a diagnostic.
Note: the current implementation supports thunk-based `sem:v1` lowering (CFG desugaring) and will also hoist nested `sem.*` used in expression positions into `let` bindings during lowering, so `sem.*` can appear under other expression nodes.

Normal compilation also runs the same HL→Core lowering in-memory before codegen (when `sem:v1` is enabled), so `--lower-hl` is a *debuggable* view of what the compiler will do anyway (not a separate semantics).

### Backward compatibility

- SIR-Core is backward compatible within a major version (v1.x).
- Packs/intent are feature-gated; adding a new pack does not affect old producers.
- Any change to a lowering rule is treated like an ABI change unless it is provably semantics-preserving under the Core contract.

## Diagnostics contract

When `sircc` rejects a SIR-HL construct, diagnostics must:

- name the **pack/feature gate** (e.g. “requires sem:v1”)
- explain the **required Core shape** (e.g. “extern calls must use decl.fn + call.indirect”)
- provide context lines when available (`--diag-context`)

## Why this helps MIR (what disappears)

If MIR emits SIR-HL instead of inventing its own “almost-Oberon” semantics:

- MIR no longer needs bespoke CFG expansion for if/and/or/match (`sem:v1` does it)
- MIR no longer needs ad-hoc closure layouts (`closure:v1` does it)
- MIR no longer needs bespoke sum/variant layouts (`adt:v1` does it)
- MIR can stop duplicating aggregate initializer logic (`agg:v1` does it)

MIR still owns language semantics, but it stops owning the “portable lowering grind”.

## Implementation plan (grouped, prioritized)

## AST → SIR “compiler kit” must-haves (avoid split-personality lowering)

This checklist is the “hard contract” work needed to switch from MIR to **AST → SIR (HL/Core) → sircc** without frontends reinventing semantics/ABI/layout piecemeal.

### P0 — Blockers (must be verifier-enforced)

- [x] Define and freeze the blessed subsets:
  - [x] One blessed **SIR-HL** surface subset (what AST emitters may generate) — `sem:v1` only
  - [x] One blessed **SIR-Core** executable subset (what backends/codegen accept) — “no `sem.*` + passes verifier”
  - [x] Provide a single gateway: `sircc --lower-hl --emit-sir-core` (HL → Core) and treat Core as the only stable codegen boundary
  - [x] Normal compilation runs HL→Core lowering in-memory (so `--lower-hl` is a debuggable view of the same semantics)
  - [x] Expand `--verify-strict` into “no best-effort” (examples implemented):
    - require explicit `fn.fields.linkage`
    - require callee+arg `type_ref` for `call.indirect` (and validate against `fields.sig`)
    - require explicit ptr `type_ref` for `ptr.from_i64`

- [ ] Target + layout contract (frontends must not guess):
  - [x] `--require-pinned-triple` and `--require-target-contract` for determinism
  - [x] Document exactly what is **layout-defined** vs **opaque** (e.g. `fun`, `closure`, `sum`)
  - [x] ABI rules are explicit for the LLVM backend (strict mode):
    - calling convention is the platform C ABI as implemented by LLVM (no explicit `cc` field yet)
    - byref is producer-owned: represent byref params as pointers
    - `--verify-strict` forbids aggregate by-value params/returns in `type.kind:"fn"` (pass pointers/out-params instead)
    - `--verify-strict` forbids varargs (`type.varargs:true`) for portability

- [x] Interop contract (imports + exports), documented and diagnostic-first:
  - [x] Imports: `decl.fn` + `call.indirect` pattern; `ptr.sym` producer rule enforced + actionable diagnostic
  - [x] Ordering clarified: forward refs allowed (decls before uses recommended for diagnostics)
  - [x] Exports: `fn.fields.name` is the exported symbol; `fn.fields.linkage:"public"` exports it; signature is the `type.kind:"fn"` referred by `fn.type_ref` (LLVM platform ABI)
  - [x] Varargs/byref/aggregates policy (current, LLVM backend):
    - varargs is supported via `type.varargs:true`, but producers should avoid it unless required and tested on the target
    - byref is producer-owned: represent byref params as pointers in the `type.kind:"fn"` signature
    - do not rely on struct/array by-value ABI; lower aggregates to pointers (and explicit copies) for portability until an ABI profile exists

- [x] Baseline data story (encoding + interop) as a pack (no handwaving):
  - [x] `data:v1` enforced by verifier (`bytes`, `string.utf8`, `cstr`)
  - [x] Decide how encoding is declared and freeze the rule (under `data:v1`, encoding is carried by canonical type names like `string.utf8`)
  - [x] Define required explicit conversions (e.g. `string.utf8` ⇄ `cstr`) as library/host calls (no implicit magic) — see `src/sircc/docs/data_v1.md`

- [x] Globals + constants that real languages need (no per-frontend folklore):
  - [x] Structured constants / aggregate initializers (arrays/structs) and global data symbols (`sym(kind=var|const)`)
  - [x] Deterministic global-init story for sums/ADTs (current state):
    - global sums are supported for **zero initialization** (`sym` initializer omitted, `value:{"t":"num","v":0}`, or `const.zero`), which yields tag=0 + zero payload
    - non-zero sum initializers must be constructed at runtime (until a dedicated constant constructor exists)
  - [x] Add strict verifier checks for common payloads (current state):
    - under `data:v1` + `--verify-strict`, `cstr` nodes must use the canonical `cstr` type via `node.type_ref`

- [ ] Semantic “intent” constructs so AST emitters stay dumb:
  - [x] `sem:v1` deterministic desugaring (`sem.if`, `sem.and_sc`, `sem.or_sc`, `sem.match_sum`)
  - [ ] Fill the remaining “structured control” intent gaps (high ROI; prevents every frontend re-inventing CFG plumbing):
    - [ ] loops: `sem.while` or `sem.loop` + `sem.break` + `sem.continue`
      - [ ] Specify continue target (header vs latch) and any value-flow rules (if supported)
    - [ ] expression-level conditional: `sem.cond(cond, then, else)` (ternary; guaranteed join)
    - [ ] integer multi-way branch: `sem.switch(scrutinee, cases, default)` (intent; lowers to `term.switch`)
    - [ ] scoped cleanup: `sem.defer` / `sem.scope(defers=[...])` (requires a precise lowering contract across all exits)

#### Proposed lowering contracts (draft, to be frozen before implementation)

These are the “shape” rules we should commit to so producers can rely on them.

- `sem.cond`: pure expression intent; equivalent to `sem.if` returning a value, but with a fixed ternary shape.
- `sem.switch`: intent-level `switch`/`case` that lowers to Core `term.switch` + join block parameters (similar to `sem.match_sum` lowering).
- loops:
  - `sem.while(cond, body)` lowers to Core blocks:
    - `header`: evaluates `cond` and `term.condbr` to `body` or `exit`
    - `body`: executes body statements and `term.br` to `header` by default
    - `exit`: loop exit
  - `sem.continue` targets `header` (re-evaluates condition), not “latch” (unless a separate `sem.for`/`sem.loop` contract is introduced).
  - `sem.break` targets `exit`.
- `sem.defer`:
  - requires a deterministic rewrite rule so defers run on: normal fallthrough/return, `break`, `continue`, and (later) unwind paths if `eh:*` exists.
  - we should likely stage this after loops/switch/cond, because it needs the strongest invariants and best diagnostics.

- [ ] Strict integration modes:
  - [x] `--verify-strict` exists
  - [x] Add a `--lower-strict` (ties to `--verify-strict`) so HL→Core lowering runs with strict validation expectations
  - [x] Make “strict” the recommended mode for integrators:
    - validate: `sircc --verify-strict --verify-only <input.sir.jsonl>`
    - lower: `sircc --lower-hl --lower-strict --emit-sir-core out.core.sir.jsonl <input.sir.jsonl>`

### P1 — Efficient emission (reduce boilerplate; keep emitters uniform)

- [x] Official preludes as “compiler kit batteries”:
  - [x] `--prelude <file>` and `--prelude-builtin data_v1|zabi25_min`
  - [x] Bundle preludes into `dist/lib/sircc/prelude`
  - [ ] Expand builtin preludes set (`core_types`, `c_abi`, etc.) and keep them versioned

- [x] Canonical lowering cookbook for AST emitters:
  - [x] “If AST has X, emit these SIR shapes” (vars, address-taken locals, short-circuit, switch, calls, VAR params, records/arrays) — see `src/sircc/docs/ast_to_sir_cookbook.md`
  - [x] Include “don’t do this” anti-patterns that cause split-personality lowering — see `src/sircc/docs/ast_to_sir_cookbook.md`

- [x] Module/link story (one consistent resolution model):
  - [x] Decide whether a SIR “module” is always a single JSONL stream, or needs a formal import mechanism (current: single stream after prelude inclusion)
  - [x] Document name-resolution + collision rules (symbols/types) and how they interact with preludes — see `src/sircc/docs/ast_to_sir_cookbook.md`

- [x] Diagnostics as a first-class integration surface:
  - [x] Stable diagnostic taxonomy (`code`) for producer errors; ensure diagnostics include actionable producer rules — see `src/sircc/docs/diagnostics.md`
  - [x] “Did you mean” suggestions for common mistakes are part of the diagnostics contract (feature gates, extern imports, strict-mode requirements)

### P2 — Inevitable widening (prevent future frontends from inventing ad-hoc semantics)

- [ ] Unsigned integer types with defined semantics:
  - [ ] Add `u8/u16/u32/u64` (or a principled alternative) and comparison/shift/div rules
  - [ ] Specify C interop mapping explicitly (ABI profile)

- [ ] Feature packs with frozen shapes (even if not implemented yet):
  - [ ] `atomics:v1` (ordering + RMW/CAS)
  - [ ] `eh:v1` (invoke/throw/resume/lpads)
  - [ ] `coro:v1` (start/resume/drop)
  - [ ] `gc:v1` (roots, barriers, safepoints)
  - [ ] Rule: if not implemented, verifier should still be able to “recognize and gate” (so producers don’t invent their own)

- [ ] Optimization boundary policy (avoid “emitters optimize differently”):
  - [ ] Decide whether AST emitters should emit already-canonical Core, or richer HL and rely on sircc to canonicalize
  - [ ] Add canonicalization passes where it removes degrees of freedom (prevents fingerprints and semantic drift)

### P0 — Make the contract real (tooling surface)

- [x] Add a legalizer entrypoint:
  - [x] `sircc --lower-hl` (runs packs/intent lowering only; no LLVM)
  - [x] `--emit-sir-core <path>` (writes lowered JSONL)
  - [x] `--lower-only` synonym for “no codegen”
- [ ] Ensure `--print-support` can report:
  - [ ] Core mnemonics
  - [ ] Supported packs/intent lowering
  - [ ] Unsupported-but-known mnemonics (so integrators can plan)
- [ ] Add `sircc --check` mode to optionally run:
  - [ ] `--check verify` (verify-only)
  - [ ] `--check lower` (verify + lower to core)
  - [ ] `--check build` (verify + lower + compile)

### P1 — `sem:v1` intent lowering (huge DX win)

- [x] MVP: lower the pure/val cases into Core expressions
  - [x] `sem.if` with `val/val` branches → `select`
  - [x] `sem.and_sc` / `sem.or_sc` with `rhs kind=val` → `bool.and` / `bool.or`
- [x] Full: implement `sem.if` lowering → CFG form (blocks/terms) + validate (required for thunk branches)
- [x] Full: implement `sem.and_sc` / `sem.or_sc` lowering → CFG with short-circuit (required for thunk branches)
- [x] Implement `sem.match_sum` lowering → `adt.tag` + `term.switch` + join args
- [x] Hoist nested `sem.*` used as operands (use-position) into `let` so lowering is uniform
- [x] Emit stable derived ids (`"sircc:lower:..."`) so producers can diff outputs
- [ ] Add golden tests:
  - [ ] input = sem intent
  - [ ] output = core CFG

### P2 — Aggregates + initializers as “stdlib enabler” (already mostly done)

- [ ] Lock the layout contract docs and verifier knobs:
  - [ ] require pinned target triple + layout keys (determinism)
- [ ] Make `agg:v1` lowering produce Core-only forms for global init

### P3 — Callables + ADTs as the “modern language unlock”

- [ ] `fun:v1` lowering to Core call forms (direct/indirect) with explicit signatures
- [ ] `closure:v1` lowering:
  - [ ] closure layout contract (code ptr + env ptr)
  - [ ] capture materialization rules
- [ ] `adt:v1` lowering:
  - [ ] sum layout contract (tag + payload)
  - [ ] `adt.make/tag/is/get` lowering into Core memory ops where possible

### P4 — Optimization hooks (optional, but planned)

- [ ] Emit optional “intent provenance” sidecar (JSONL) mapping lowered core regions → originating constructs
- [ ] Keep it best-effort and non-semantic (tooling only) until a versioned hint schema exists

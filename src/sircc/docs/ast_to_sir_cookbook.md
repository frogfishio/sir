## AST → SIR cookbook (blessed emission patterns)

This is the canonical “if your AST has X, emit these SIR shapes” guide for producers targeting `sircc`
(`sir-v1.0` JSONL).

Goals:

- minimize producer degrees of freedom (avoid “split personality” lowering across languages)
- keep SIR-Core as the stable executable boundary
- use SIR-HL only where `sircc` provides deterministic lowering (`sem:v1`)

Non-goals:

- language semantics (scope/typecheck/eval order rules) are producer-owned
- this is not a full reference spec for every mnemonic; use `sircc --print-support` for the full surface

### Recommended pipeline (integrator mode)

- validate: `sircc --verify-strict --verify-only <input.sir.jsonl>`
- lower intent (debuggable): `sircc --lower-hl --lower-strict --emit-sir-core out.core.sir.jsonl <input.sir.jsonl>` (or `--lower-only`)
- compile: `sircc --verify-strict <input.sir.jsonl> -o <out>`

## Module skeleton

Recommended ordering (not strictly required):

1. `meta` (enable feature gates and pin target contract when needed)
2. `src` records (optional, but improves diagnostics)
3. `type` records (including canonical `data:v1` types if you use them)
4. extern decls (`decl.fn`) and globals (`sym`)
5. functions (`fn`)

Minimal `meta`:

```json
{"ir":"sir-v1.0","k":"meta","producer":"your-compiler","unit":"your_unit","ext":{
  "features":["data:v1","sem:v1","agg:v1","fun:v1","closure:v1","adt:v1","simd:v1"]
}}
```

Notes:

- Only enable the features you actually use.
- If you need reproducible cross-machine artifacts, also pin the target (`meta.ext.target.*`) and use
  `sircc --require-pinned-triple --require-target-contract`.

## Variables, SSA, and mutation

SIR is primarily expression/SSA-shaped; mutation is explicit and goes through memory.

### Immutable local bindings (SSA)

Use `let` to bind a name, then `name` to reference it:

```json
{"ir":"sir-v1.0","k":"node","id":"c1","tag":"const.i32","type_ref":"t:i32","fields":{"value":1}}
{"ir":"sir-v1.0","k":"node","id":"b1","tag":"let","fields":{"name":"x","value":{"t":"ref","id":"c1"}}}
{"ir":"sir-v1.0","k":"node","id":"x","tag":"name","type_ref":"t:i32","fields":{"name":"x"}}
```

Rules:

- `let` is a statement (must appear in a block’s `stmts` list).
- `name` is an expression that resolves to the currently bound SSA value.

### Mutable locals (“var”)

Allocate a stack slot and load/store:

- allocate: `alloca.<ty>` (e.g. `alloca.i32`) or `alloca` with `fields.ty`
- initialize: `store.<ty>`
- read: `load.<ty>`

Pattern:

```json
{"ir":"sir-v1.0","k":"node","id":"slot","tag":"alloca.i32"}
{"ir":"sir-v1.0","k":"node","id":"init","tag":"store.i32","fields":{"addr":{"t":"ref","id":"slot"},"value":{"t":"ref","id":"c1"},"align":4}}
{"ir":"sir-v1.0","k":"node","id":"v","tag":"load.i32","type_ref":"t:i32","fields":{"addr":{"t":"ref","id":"slot"},"align":4}}
```

Guidance:

- Bind the slot pointer with `let` if you need to reference it by name.
- Prefer explicit alignment (when known) to avoid target surprises.

## Calls and interop (C ABI)

### Call a known internal function

Use `call` with a direct callee ref.

### Call an external C symbol (import)

Use `decl.fn` + `call.indirect` (producer rule).

```json
{"ir":"sir-v1.0","k":"type","id":"t_puts","kind":"fn","params":["t:pchar"],"ret":"t:i32"}
{"ir":"sir-v1.0","k":"node","id":"decl:puts","tag":"decl.fn","type_ref":"t_puts","fields":{"name":"puts"}}
{"ir":"sir-v1.0","k":"node","id":"s0","tag":"cstr","type_ref":"t:cstr","fields":{"value":"hello"}}
{"ir":"sir-v1.0","k":"node","id":"call0","tag":"call.indirect","type_ref":"t:i32","fields":{
  "sig":{"t":"ref","id":"t_puts"},
  "args":[{"t":"ref","id":"decl:puts"},{"t":"ref","id":"s0"}]
}}
```

Strict mode guidance:

- Prefer `--verify-strict` for integrators.
- Under strict, `call.indirect` requires callee/args to carry `type_ref` so the verifier can check types.

### Export a function for C to call

Emit a `fn` node with:

- `fields.name`: symbol name
- `fields.linkage:"public"`

Do not rely on aggregate-by-value ABI. In strict mode, aggregate by-value params/returns are rejected;
use pointers/out-params.

## Control flow

### If / short-circuit (recommended: `sem:v1`)

If you have AST-level intent and want to keep the producer dumb, emit:

- `sem.if`
- `sem.and_sc`, `sem.or_sc`

`sircc` lowers these deterministically to Core CFG.

### Switch / pattern dispatch (Core)

Use `term.switch` with const integer literals and explicit join blocks (`bparam`) for φ-like values.

### Switch / loops / defer (recommended: `sem:v1`)

- `sem.switch` lets you keep AST emitters dumb (sircc lowers to `term.switch` + join blocks).
- `sem.while` + `sem.break` + `sem.continue` let you avoid building CFG for loops in the frontend.
- `sem.defer` is supported as a **function-level** cleanup intent (MVP; body-form functions only).

If you *want* to hand-lower to Core, these remain the primitives:

- blocks + `term.br` / `term.condbr` / `term.switch`
- block parameters (`bparam`) for φ nodes

## Records and arrays (aggregates)

### Types

- arrays: `{"k":"type","kind":"array","of":<ty>,"len":<i64>}`
- structs: `{"k":"type","kind":"struct","fields":[{"name":...,"type_ref":...}, ...]}`

### Addressing

- array indexing: use `ptr.offset` with `fields.ty` set to the element type (scales by `sizeof(ty)`)
- struct field access: there is no `ptr.field`/`offsetof` mnemonic yet; producers must compute byte offsets
  under the pinned target contract and use `ptr.add` (or avoid field addressing until this is added)

### Structured constants and globals (`agg:v1`)

Use `sym(kind=var|const)` for globals. Supported constant constructors include:

- `const.zero`, `const.array`, `const.repeat`, `const.struct`

Sums/ADTs:

- zero-initialization is deterministic (`tag=0` + zero payload)
- non-zero sum initializers must be constructed at runtime (until a dedicated sum constant exists)

## Strings and bytes (`data:v1`)

Canonical types under `data:v1`:

- `bytes = {data:ptr(i8), len:i64}`
- `string.utf8 = {data:ptr(i8), len:i64}` (encoding carried by the type name)
- `cstr = ptr(i8)` (NUL-terminated by convention)

Rules:

- conversions are explicit; there is no implicit `string.utf8` ⇄ `cstr`
- under `data:v1` + `--verify-strict`, `cstr` nodes must set `type_ref` to the canonical `cstr` type

## Module/link story (current)

Today, a “module” is one JSONL stream after prelude inclusion.

- Use `--prelude` / `--prelude-builtin` to inject shared types/imports.
- There is no first-class “import module X” mechanism at the SIR level yet.
- Name collisions:
  - `ptr.sym` requires an in-module declaration (`fn`/`decl.fn` or `sym(kind=var|const)`), so missing/ambiguous names are verifier errors.
  - `type.name` is a convenience; type identity is by id. Under packs like `data:v1`, some names are canonical and must be unique.

## Anti-patterns (causes split-personality lowering)

- Emitting `sem:*` without the `sem:v1` feature gate.
- Mixing “call extern by `ptr.sym` unknown name” (rejected) instead of `decl.fn + call.indirect`.
- Using by-value aggregates in `type.kind:"fn"` signatures (portable behavior is target-dependent; strict mode rejects).
- Smuggling meaning into `id` numbers (use readable string ids; ids are identifiers, not semantics).
- Encoding strings in multiple incompatible ways in the same ecosystem (use `data:v1` canonical types).

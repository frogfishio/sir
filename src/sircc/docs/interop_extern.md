# Interop: importing and exporting C symbols (as implemented by `sircc`)

This document is the **SIR-side contract** for C ABI interop when compiling via LLVM.

If you are building a frontend (AST → SIR), treat this as “the rulebook” so you don’t
re-invent (or accidentally violate) verifier expectations.

Related:
- `src/sircc/docs/ast_to_sir_cookbook.md` (broader producer patterns)
- `src/sircc/docs/data_v1.md` (`cstr` + UTF-8 slices)

Examples:
- `src/sircc/examples/hello_world_puts.sir.jsonl` (extern import + call)
- `src/sircc/examples/interop_export_add2.sir.jsonl` (export a function)
- `src/sircc/examples/interop_export_global_i32.sir.jsonl` (export a global)

---

## Imports: call an external C function

Producer rule (verifier-friendly):
- **Declare extern functions with `decl.fn`.**
- **Call them with `call.indirect`.**

Minimal pattern (`puts(const char*) -> i32`):

```json
{"ir":"sir-v1.0","k":"type","id":"t:i8","kind":"prim","prim":"i8"}
{"ir":"sir-v1.0","k":"type","id":"t:p_i8","kind":"ptr","of":"t:i8"}
{"ir":"sir-v1.0","k":"type","id":"t:i32","kind":"prim","prim":"i32"}
{"ir":"sir-v1.0","k":"type","id":"t:puts_sig","kind":"fn","params":["t:p_i8"],"ret":"t:i32"}

{"ir":"sir-v1.0","k":"node","id":"n:puts","tag":"decl.fn","type_ref":"t:puts_sig","fields":{"name":"puts"}}
{"ir":"sir-v1.0","k":"node","id":"n:s","tag":"cstr","fields":{"value":"hello\\n"}}
{"ir":"sir-v1.0","k":"node","id":"n:call","tag":"call.indirect","type_ref":"t:i32","fields":{
  "sig":{"t":"ref","k":"type","id":"t:puts_sig"},
  "args":[{"t":"ref","k":"node","id":"n:puts"},{"t":"ref","k":"node","id":"n:s"}]
}}
```

Why not `ptr.sym("puts")`?
- `sircc` treats `ptr.sym` as “address-of a **known** in-module declaration” (a `fn` / `decl.fn` for functions, or a `sym(kind=var|const)` for globals).
- Calling an undeclared external symbol via `ptr.sym` is rejected early (before link-time) to keep producers honest.

If you need “call by symbol name”, emit a `decl.fn` and then call that.

---

## Imports: reference an external global symbol

You can declare an external global with a `sym` record and then take its address with `ptr.sym`.

Example: import `extern int g_counter;`

```json
{"ir":"sir-v1.0","k":"type","id":"t:i32","kind":"prim","prim":"i32"}
{"ir":"sir-v1.0","k":"type","id":"t:p_i32","kind":"ptr","of":"t:i32"}
{"ir":"sir-v1.0","k":"sym","id":"s:g_counter","name":"g_counter","kind":"var","linkage":"extern","type_ref":"t:i32"}

{"ir":"sir-v1.0","k":"node","id":"n:gptr","tag":"ptr.sym","type_ref":"t:p_i32","fields":{"name":"g_counter"}}
```

Notes:
- `linkage:"extern"` tells `sircc` not to require or emit an initializer.
- Addressing is still explicit: you will typically `load.i32` / `store.i32` via the pointer.

---

## Exports: make a SIR function callable from C

Emit a `fn` node with:
- `fields.name`: the exported linker symbol
- `fields.linkage:"public"`

Two supported `fn` forms exist:
- **body-form**: `fields.body` points at a single `block` node
- **CFG-form**: `fields.entry` + `fields.blocks[]` explicitly list blocks (useful for hand-built CFG)

Body-form example: export `sir_add2(i32,i32)->i32`

```json
{"ir":"sir-v1.0","k":"type","id":"t:i32","kind":"prim","prim":"i32"}
{"ir":"sir-v1.0","k":"type","id":"t:add2_sig","kind":"fn","params":["t:i32","t:i32"],"ret":"t:i32"}

{"ir":"sir-v1.0","k":"node","id":"p:a","tag":"param","type_ref":"t:i32","fields":{"name":"a"}}
{"ir":"sir-v1.0","k":"node","id":"p:b","tag":"param","type_ref":"t:i32","fields":{"name":"b"}}
{"ir":"sir-v1.0","k":"node","id":"n:a","tag":"name","type_ref":"t:i32","fields":{"name":"a"}}
{"ir":"sir-v1.0","k":"node","id":"n:b","tag":"name","type_ref":"t:i32","fields":{"name":"b"}}
{"ir":"sir-v1.0","k":"node","id":"n:sum","tag":"i32.add","type_ref":"t:i32","fields":{"args":[{"t":"ref","id":"n:a"},{"t":"ref","id":"n:b"}]}}
{"ir":"sir-v1.0","k":"node","id":"n:ret","tag":"term.ret","fields":{"value":{"t":"ref","id":"n:sum"}}}
{"ir":"sir-v1.0","k":"node","id":"b0","tag":"block","fields":{"stmts":[{"t":"ref","id":"n:ret"}]}}

{"ir":"sir-v1.0","k":"node","id":"fn:add2","tag":"fn","type_ref":"t:add2_sig","fields":{
  "name":"sir_add2",
  "linkage":"public",
  "params":[{"t":"ref","id":"p:a"},{"t":"ref","id":"p:b"}],
  "body":{"t":"ref","id":"b0"}
}}
```

Strict mode notes:
- under `--verify-strict`, `fn.fields.linkage` is required and must be `"local"` or `"public"` (even though the schema also lists `"extern"`).
- do not rely on platform ABI for aggregate-by-value params/returns; in strict mode, aggregate by-value is rejected (use pointers / out-params).

---

## Exports: define a global (C-accessible)

Use a `sym` record of kind `var` or `const` with `linkage:"public"`.

Example: export `int g = 41;`

```json
{"ir":"sir-v1.0","k":"type","id":"t:i32","kind":"prim","prim":"i32"}
{"ir":"sir-v1.0","k":"node","id":"n:init","tag":"const.i32","type_ref":"t:i32","fields":{"value":41}}
{"ir":"sir-v1.0","k":"sym","id":"s:g","name":"g","kind":"var","linkage":"public","type_ref":"t:i32","value":{"t":"ref","k":"node","id":"n:init"}}
```

Notes:
- `sym.value` initializers are intentionally conservative in `sircc`: either numeric zero-ish (`{"t":"num","v":0}`) or a `const.*` node of the same type.
- take the address via `ptr.sym("g")`, then `load.*`/`store.*`.

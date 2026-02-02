# TODO (sircc roadmap — active)

This is the **active** sircc roadmap. Keep it updated as features land.

Goal: a plain-C `sircc` binary that compiles `sir-v1.0` JSONL to native binaries via LLVM, with complete mnemonic coverage (see `schema/sir/v1.0/mnemonics.html`) and clear feature gates.

## Milestone 0 — Build + Dev UX

- [x] `sircc --version` and `sircc --help` (basic)
- [x] `--dump-records` (quick parse trace)
- [x] `--verify-only` (parse + validate + report, no codegen)
- [ ] Deterministic builds: print target triple + data layout (`--print-target`)

## Milestone 1 — Parser + Validator (spec-first correctness)

- [x] Parse all record kinds (`meta/src/diag/sym/type/node/ext/label/instr/dir`)
- [x] Preserve `src_ref` + `loc` in diagnostics (when present)
- [x] Closed-schema behavior for supported record kinds (reject unknown fields)
- [ ] Record order independence (forward refs with fixup pass)
- [x] Feature gates: parse `meta.ext.features`, reject feature-gated `instr.m`

## Milestone 2 — Core backend architecture

- [x] Node-frontend lowering (enough to exercise LLVM pipeline)
- [ ] Instr-frontend lowering (label/instr/dir → CFG + SSA) — required for full mnemonic completeness

## Milestone 3 — Mnemonics: Base (ungated)

### Implemented (node tags)

- [x] Integer ops: `i8/i16/i32/i64` add/sub/mul/and/or/xor/not/neg/shl/shr.s/shr.u/rotl/rotr
- [x] Integer compare: `iN.cmp.*` and `iN.eqz`
- [x] Integer min/max: `iN.min.s/min.u/max.s/max.u`
- [x] Integer div/rem: `iN.(div|rem).(s|u).(trap|sat)`
- [x] Bit twiddling: `iN.clz/ctz/popc`
- [x] Bool ops: `bool.not/bool.and/bool.or/bool.xor`
- [x] Select: `select` (validates bool cond + operand type match; optional `fields.ty`)
- [x] Conversions: `i<dst>.zext/sext/trunc.i<src>`
- [x] Float ops: `f32/f64` add/sub/mul/div/neg/abs/sqrt/min/max + compares + NaN canonicalization
- [x] Float/int conversions: `f32/f64.from_i{32,64}.{s,u}` and `i{32,64}.trunc_sat_f{32,64}.{s,u}`
- [x] Pointers: `ptr.sym`, `ptr.add/sub`, `ptr.cmp.eq/ne`, `ptr.to_i64`, `ptr.from_i64`
- [x] Layout: `ptr.offset`, `ptr.alignof`, `ptr.sizeof` (current layout is deterministic host-layout subset)
- [x] Memory: `load.*`/`store.*` for `i8/i16/i32/i64/f32/f64/ptr` (float loads/stores canonicalize NaNs; supports `align` + `vol`)
- [x] Memory: `mem.copy` (`overlap:"allow"` → memmove; `"disallow"` → runtime overlap check + deterministic trap)
- [x] Memory: `mem.fill`
- [x] Alloca: `alloca.<prim>` + `alloca` (mnemonic-style: `fields.ty` + `flags:{count,align,zero}`)
- [x] CFG terminators: `term.br`, `term.cbr`, `term.switch`, `term.ret`, `term.trap`, `term.unreachable` (+ block params via `bparam` PHIs)

### Next (base)

- [x] `alloca` runtime `count` (accepts i64 or node ref); `alloca` returns `ptr` (i8*) per table
- [x] `eff.fence` (ungated in the table): implemented with mode validation; `relaxed` is treated as no-op; others lower to LLVM fence
- [ ] Tighten type rules across the board (many ops rely on LLVM type inference today)

## Milestone 4 — Mnemonics: Feature-gated packages

- [ ] Atomics (`atomics:v1`)
- [ ] SIMD (`simd:v1`)
- [ ] ADT sums (`adt:v1`)
- [ ] Function values / closures / coroutines / EH / GC / sem packages (gated)

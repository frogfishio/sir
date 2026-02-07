# Diagnostics contract (`sircc`)

This document defines the *stable integration surface* for `sircc` diagnostics: error codes, JSON shape,
and how integrators should consume them.

## Formats

`sircc` supports:

- human text (default)
- JSON lines: `--diagnostics json`

When using JSON, `sircc` writes one JSON object per error to `stderr`.

## Stable error codes

Every `sircc` error should include a stable string `code` suitable for:

- unit tests (asserting specific failures)
- producer-side integration (mapping errors to “fix your emitter” rules)

Code namespace convention:

- `sircc.<domain>.<specific>` (examples: `sircc.feature.gate`, `sircc.ptr.sym.unknown`, `sircc.call.indirect.sig.bad`)

Guidance:

- Codes are part of the integration contract; avoid renaming them unless a major-version contract changes.
- Messages may improve over time; integrators should match on `code`, not `msg`.

## JSON diagnostic record shape

Example:

```json
{"ir":"sir-v1.0","k":"diag","level":"error","msg":"...","code":"sircc.feature.gate","about":{"k":"node","id":13,"tag":"sem.if"},"src_ref":7,"loc":{"unit":"foo.sir.jsonl","line":123,"col":1},"context":[{"line":121,"text":"..."},{"line":122,"text":"..."},{"line":123,"text":"..."}],"context_line":123}
```

Fields:

- `ir`: always `"sir-v1.0"`
- `k`: always `"diag"`
- `level`: currently `"error"`
- `msg`: human-readable error message (not stable)
- `code`: stable string error code (stable)
- `about`: best-effort record context:
  - `k`: record kind (e.g. `"node"`, `"type"`)
  - `id`: record id (internal dense id)
  - `tag`: best-effort tag (`node.tag`, etc.)
- `src_ref`: best-effort `src_ref` value when present
- `loc`: best-effort location (`unit`, `line`, `col`), derived from `loc` or `src_ref` or file line
- `context` / `context_line`: only present when `--diag-context N` is set; `context` is an array of nearby JSONL lines

Notes:

- JSON diagnostics are intended to be machine-consumable; do not parse `msg` for logic.
- `about.id` uses `sircc`’s internal dense ids; producers should use their own ids for cross-tool identity.

## “Did you mean” guidance

Where a diagnostic corresponds to a common producer mistake, `sircc` should include an actionable hint
in `msg` (examples: missing feature gate, extern import/export shapes, strict-mode requirements).


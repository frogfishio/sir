<!-- SPDX-FileCopyrightText: 2026 Frogfish -->
<!-- SPDX-License-Identifier: GPL-3.0-or-later -->

# Contributing to zasm

Thanks for contributing. This repo is intentionally small, stream-first, and hostile to bloat.

## Project shape

`zasm` is a two-stage toolchain:

- **`zas`** — zASM (text) → **JSONL IR** (one record per line) on stdout
- **`zld`** — JSONL IR → **WAT** (single module) on stdout
- **`zrun`** — local runner/harness (Wasmtime host) for tests + debugging

Design rule: **stage boundaries are stable**. If you change the JSONL IR, you are changing the contract.

## License and trademarks

- All contributions are accepted under **GPL-3.0-or-later** (see `LICENSE.md`).
- By submitting a PR, you agree your contribution may be redistributed under that license.
- **Trademarks:** “sir” and related marks are not granted under the GPL. Don’t use the project name/logo to market forks.

## Setup (macOS)

TODO:

### Required tools
TODO:

## Build
TODO:

## Quick pipeline

TODO:

## Run examples (local harness)

TODO:

## Tests

We prefer **golden tests**:

- input fixture → expected output bytes
- keep tests deterministic (no timestamps, no randomness)

## IR changes (JSONL contract)

If you need to change the IR schema:

- Update the schema in `schema/`.
- Update both `sirc` and `sircc` in the same PR.
- Add a migration note in the PR description.

Strong preference: **extend** rather than break. New fields should be optional when possible.

## Style

- C is C11.
- Keep code boring and readable.
- Avoid hidden global state.
- Errors should include `tool: message` and a source line if available.

## PR checklist

- [ ] Builds: `make sirc sircc`
- [ ] Tests: `make test`
- [ ] Added/updated examples if behavior changes
- [ ] IR/schema updated if needed
- [ ] No gratuitous refactors (separate PRs)

## Security

If you find a security issue (especially around parsing or bounds handling), please open a private report instead of posting a public issue.

---

Welcome aboard.
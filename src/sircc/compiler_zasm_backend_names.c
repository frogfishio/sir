// SPDX-FileCopyrightText: 2026 Frogfish
// SPDX-License-Identifier: GPL-3.0-or-later

#include "compiler_zasm_backend_helpers.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

static bool ensure_name_binding_cap(SirProgram* p, ZasmNameBinding** names, size_t* cap, size_t need) {
  if (!names || !cap) return false;
  if (*cap >= need) return true;
  size_t next = *cap ? *cap : 16;
  while (next < need) next *= 2;
  ZasmNameBinding* bigger = (ZasmNameBinding*)realloc(*names, next * sizeof(ZasmNameBinding));
  if (!bigger) {
    if (p) errf(p, "sircc: zasm: out of memory");
    return false;
  }
  *names = bigger;
  *cap = next;
  return true;
}

bool emit_bind_slot(
    SirProgram* p,
    ZasmNameBinding** names,
    size_t* name_len,
    size_t* name_cap,
    const char* bind_name,
    const char* slot_sym,
    int64_t slot_size_bytes) {
  if (!p || !names || !name_len || !name_cap || !bind_name || !slot_sym) return false;
  if (!ensure_name_binding_cap(p, names, name_cap, (*name_len) + 1)) return false;
  // Shadowing allowed; last binding wins.
  (*names)[(*name_len)++] = (ZasmNameBinding){
      .name = bind_name,
      .is_slot = true,
      .op = {.k = ZOP_SYM, .s = slot_sym, .n = slot_size_bytes},
      .slot_size_bytes = slot_size_bytes,
  };
  return true;
}

bool emit_bind_op(SirProgram* p, ZasmNameBinding** names, size_t* name_len, size_t* name_cap, const char* bind_name, ZasmOp op) {
  if (!p || !names || !name_len || !name_cap || !bind_name) return false;
  if (!ensure_name_binding_cap(p, names, name_cap, (*name_len) + 1)) return false;
  (*names)[(*name_len)++] = (ZasmNameBinding){
      .name = bind_name,
      .is_slot = false,
      .op = op,
      .slot_size_bytes = 0,
  };
  return true;
}


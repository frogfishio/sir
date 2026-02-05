// SPDX-FileCopyrightText: 2026 Frogfish
// SPDX-License-Identifier: GPL-3.0-or-later

#include "compiler_zasm_backend_helpers.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

const char* label_for_block(SirProgram* p, int64_t entry_id, int64_t block_id) {
  if (!p) return NULL;
  if (block_id == entry_id) return "zir_main";
  char buf[64];
  snprintf(buf, sizeof(buf), "b_%lld", (long long)block_id);
  return arena_strdup(&p->arena, buf);
}

const char* label_for_cbr_edge(SirProgram* p, int64_t term_id, const char* which) {
  if (!p || !which) return NULL;
  char buf[96];
  snprintf(buf, sizeof(buf), "cbr_%s_%lld", which, (long long)term_id);
  return arena_strdup(&p->arena, buf);
}

bool emit_cfg_branch_args(
    FILE* out,
    SirProgram* p,
    ZasmStr* strs,
    size_t strs_len,
    ZasmAlloca* allocas,
    size_t allocas_len,
    ZasmNameBinding* names,
    size_t name_len,
    ZasmBParamSlot** bps,
    size_t* bp_len,
    size_t* bp_cap,
    int64_t to_id,
    JsonValue* args,
    int64_t* line) {
  if (!args || args->type != JSON_ARRAY || args->v.arr.len == 0) return true;
  if (!out || !p || !bps || !bp_len || !bp_cap || !line) return false;

  NodeRec* to_blk = get_node(p, to_id);
  JsonValue* to_params = (to_blk && to_blk->fields) ? json_obj_get(to_blk->fields, "params") : NULL;
  if (!to_params || to_params->type != JSON_ARRAY || to_params->v.arr.len != args->v.arr.len) {
    errf(p, "sircc: zasm: branch args must match destination block params");
    return false;
  }

  for (size_t ai = 0; ai < args->v.arr.len; ai++) {
    int64_t arg_id = 0;
    int64_t param_id = 0;
    if (!parse_node_ref_id(p, args->v.arr.items[ai], &arg_id) || !parse_node_ref_id(p, to_params->v.arr.items[ai], &param_id)) {
      errf(p, "sircc: zasm: branch arg/param must be node refs");
      return false;
    }

    const char* slot_sym = NULL;
    int64_t slot_w = 0;
    for (size_t bi = 0; bi < *bp_len; bi++) {
      if ((*bps)[bi].node_id == param_id) {
        slot_sym = (*bps)[bi].sym;
        slot_w = (*bps)[bi].size_bytes;
        break;
      }
    }
    if (!slot_sym) {
      NodeRec* pn = get_node(p, param_id);
      slot_w = pn ? width_for_type_id(p, pn->type_ref) : 0;
      if (!slot_w) slot_w = 8;
      if (!ensure_bparam_slot(p, bps, bp_len, bp_cap, param_id, slot_w, &slot_sym)) {
        errf(p, "sircc: zasm: out of memory");
        return false;
      }
    }

    const char* reg = reg_for_width(slot_w);
    if (!reg) {
      errf(p, "sircc: zasm: unsupported bparam width %lld", (long long)slot_w);
      return false;
    }

    ZasmOp op = {0};
    if (!zasm_lower_value_to_op(p, strs, strs_len, allocas, allocas_len, names, name_len, *bps, *bp_len, arg_id, &op)) return false;
    if (op.k == ZOP_SLOT) {
      if (!emit_load_slot_to_reg(out, op.s, op.n, reg, (*line)++)) return false;
    } else {
      if (!emit_ld_reg_or_imm(out, reg, &op, (*line)++)) return false;
    }
    if (!emit_store_reg_to_slot(out, slot_sym, slot_w, reg, (*line)++)) return false;
  }

  return true;
}


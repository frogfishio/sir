// SPDX-FileCopyrightText: 2026 Frogfish
// SPDX-License-Identifier: GPL-3.0-or-later

#include "compiler_zasm_backend_helpers.h"
#include "compiler_zasm_regcache.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int64_t width_for_prim(const char* prim) {
  if (!prim) return 0;
  if (strcmp(prim, "i8") == 0 || strcmp(prim, "bool") == 0) return 1;
  if (strcmp(prim, "i16") == 0) return 2;
  if (strcmp(prim, "i32") == 0 || strcmp(prim, "f32") == 0) return 4;
  if (strcmp(prim, "i64") == 0 || strcmp(prim, "f64") == 0 || strcmp(prim, "ptr") == 0) return 8;
  return 0;
}

int64_t width_for_type_id(SirProgram* p, int64_t type_id) {
  if (!p) return 0;
  TypeRec* t = get_type(p, type_id);
  if (!t) return 0;
  switch (t->kind) {
    case TYPE_PRIM:
      return width_for_prim(t->prim);
    case TYPE_PTR:
      return 8;
    default:
      return 0;
  }
}

static const char* sym_for_bparam(SirProgram* p, int64_t bparam_id) {
  char buf[64];
  snprintf(buf, sizeof(buf), "bp_%lld", (long long)bparam_id);
  return arena_strdup(&p->arena, buf);
}

bool ensure_bparam_slot(
    SirProgram* p,
    ZasmBParamSlot** bps,
    size_t* bps_len,
    size_t* bps_cap,
    int64_t bparam_id,
    int64_t size_bytes,
    const char** out_sym) {
  if (!p || !bps || !bps_len || !bps_cap || !out_sym) return false;
  *out_sym = NULL;

  for (size_t i = 0; i < *bps_len; i++) {
    if ((*bps)[i].node_id == bparam_id) {
      *out_sym = (*bps)[i].sym;
      return true;
    }
  }

  if (*bps_len == *bps_cap) {
    size_t next = *bps_cap ? (*bps_cap * 2) : 16;
    ZasmBParamSlot* bigger = (ZasmBParamSlot*)realloc(*bps, next * sizeof(ZasmBParamSlot));
    if (!bigger) return false;
    *bps = bigger;
    *bps_cap = next;
  }

  const char* sym = sym_for_bparam(p, bparam_id);
  if (!sym) return false;
  (*bps)[(*bps_len)++] = (ZasmBParamSlot){.node_id = bparam_id, .sym = sym, .size_bytes = size_bytes};
  *out_sym = sym;
  return true;
}

bool add_temp_slot(
    SirProgram* p,
    ZasmTempSlot** slots,
    size_t* slots_len,
    size_t* slots_cap,
    int64_t id_hint,
    int64_t size_bytes,
    const char** out_sym) {
  if (!p || !slots || !slots_len || !slots_cap || !out_sym) return false;
  *out_sym = NULL;

  if (*slots_len == *slots_cap) {
    size_t next = *slots_cap ? (*slots_cap * 2) : 16;
    ZasmTempSlot* bigger = (ZasmTempSlot*)realloc(*slots, next * sizeof(ZasmTempSlot));
    if (!bigger) return false;
    *slots = bigger;
    *slots_cap = next;
  }

  char name_buf[96];
  snprintf(name_buf, sizeof(name_buf), "tmp_%lld", (long long)id_hint);
  char* sym = arena_strdup(&p->arena, name_buf);
  if (!sym) return false;

  (*slots)[(*slots_len)++] = (ZasmTempSlot){.sym = sym, .size_bytes = size_bytes};
  *out_sym = sym;
  return true;
}

bool emit_st64_slot_from_hl(FILE* out, const char* slot_sym, int64_t line_no) {
  if (!out || !slot_sym) return false;
  ZasmOp base = {.k = ZOP_SYM, .s = slot_sym};
  zasm_write_ir_k(out, "instr");
  fprintf(out, ",\"m\":\"ST64\",\"ops\":[");
  zasm_write_op_mem(out, &base, 0, 8);
  fprintf(out, ",");
  zasm_write_op_reg(out, "HL");
  fprintf(out, "]");
  zasm_write_loc(out, line_no);
  fprintf(out, "}\n");
  zasm_regcache_invalidate_slot(slot_sym, 8);
  return true;
}

bool emit_store_reg_to_slot(FILE* out, const char* slot_sym, int64_t size_bytes, const char* reg, int64_t line_no) {
  if (!out || !slot_sym || !reg) return false;
  const char* m = NULL;
  int64_t hint = 0;
  if (size_bytes == 1) {
    m = "ST8";
    hint = 1;
  } else if (size_bytes == 2) {
    m = "ST16";
    hint = 2;
  } else if (size_bytes == 4) {
    m = "ST32";
    hint = 4;
  } else if (size_bytes == 8) {
    m = "ST64";
    hint = 8;
  } else {
    return false;
  }

  ZasmOp base = {.k = ZOP_SYM, .s = slot_sym};
  zasm_write_ir_k(out, "instr");
  fprintf(out, ",\"m\":");
  json_write_escaped(out, m);
  fprintf(out, ",\"ops\":[");
  zasm_write_op_mem(out, &base, 0, hint);
  fprintf(out, ",");
  zasm_write_op_reg(out, reg);
  fprintf(out, "]");
  zasm_write_loc(out, line_no);
  fprintf(out, "}\n");
  zasm_regcache_invalidate_slot(slot_sym, size_bytes);
  return true;
}

const char* reg_for_width(int64_t width_bytes) {
  if (width_bytes == 1) return "A";
  // For now, keep 16/32/64-bit values in HL.
  if (width_bytes == 2 || width_bytes == 4 || width_bytes == 8) return "HL";
  return NULL;
}

bool emit_load_slot_to_reg(FILE* out, const char* slot_sym, int64_t width_bytes, const char* dst_reg, int64_t line_no) {
  if (!out || !slot_sym || !dst_reg) return false;
  if (zasm_regcache_matches_slot(dst_reg, slot_sym, width_bytes)) return true;
  ZasmOp base = {.k = ZOP_SYM, .s = slot_sym};

  const char* m = NULL;
  int64_t hint = 0;
  if (width_bytes == 1) {
    m = "LD8U";
    hint = 1;
  } else if (width_bytes == 2) {
    m = "LD16U";
    hint = 2;
  } else if (width_bytes == 4) {
    m = "LD32U64";
    hint = 4;
  } else if (width_bytes == 8) {
    m = "LD64";
    hint = 8;
  } else {
    return false;
  }

  zasm_write_ir_k(out, "instr");
  fprintf(out, ",\"m\":");
  json_write_escaped(out, m);
  fprintf(out, ",\"ops\":[");
  zasm_write_op_reg(out, dst_reg);
  fprintf(out, ",");
  zasm_write_op_mem(out, &base, 0, hint);
  fprintf(out, "]");
  zasm_write_loc(out, line_no);
  fprintf(out, "}\n");
  zasm_regcache_set_slot(dst_reg, slot_sym, width_bytes);
  return true;
}

bool emit_ld_reg_or_imm(FILE* out, const char* dst_reg, const ZasmOp* op, int64_t line_no) {
  if (!out || !dst_reg || !op) return false;
  zasm_regcache_invalidate_reg(dst_reg);
  zasm_write_ir_k(out, "instr");
  fprintf(out, ",\"m\":\"LD\",\"ops\":[");
  zasm_write_op_reg(out, dst_reg);
  fprintf(out, ",");
  if (!zasm_write_op(out, op)) return false;
  fprintf(out, "]");
  zasm_write_loc(out, line_no);
  fprintf(out, "}\n");
  return true;
}

bool emit_jr(FILE* out, const char* lbl, int64_t line_no) {
  if (!out || !lbl) return false;
  zasm_write_ir_k(out, "instr");
  fprintf(out, ",\"m\":\"JR\",\"ops\":[");
  zasm_write_op_lbl(out, lbl);
  fprintf(out, "]");
  zasm_write_loc(out, line_no);
  fprintf(out, "}\n");
  return true;
}

bool emit_jr_cond(FILE* out, const char* cond_sym, const char* lbl, int64_t line_no) {
  if (!out || !cond_sym || !lbl) return false;
  zasm_write_ir_k(out, "instr");
  fprintf(out, ",\"m\":\"JR\",\"ops\":[");
  zasm_write_op_sym(out, cond_sym);
  fprintf(out, ",");
  zasm_write_op_lbl(out, lbl);
  fprintf(out, "]");
  zasm_write_loc(out, line_no);
  fprintf(out, "}\n");
  return true;
}

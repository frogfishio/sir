// SPDX-FileCopyrightText: 2026 Frogfish
// SPDX-License-Identifier: GPL-3.0-or-later

#include "compiler_zasm_internal.h"

#include <stdio.h>

static int64_t g_zasm_record_id = 0;
static FILE* g_zasm_map_out = NULL;
static int64_t g_zasm_about_node_id = -1;
static const char* g_zasm_about_node_tag = NULL;

void zasm_reset_record_ids(void) { g_zasm_record_id = 0; }

void zasm_set_map_output(FILE* out) { g_zasm_map_out = out; }

void zasm_set_about_node(int64_t node_id, const char* node_tag) {
  g_zasm_about_node_id = node_id;
  g_zasm_about_node_tag = node_tag;
}

void zasm_clear_about(void) {
  g_zasm_about_node_id = -1;
  g_zasm_about_node_tag = NULL;
}

void zasm_write_ir_k(FILE* out, const char* k) {
  int64_t zid = g_zasm_record_id++;
  fprintf(out, "{\"ir\":\"zasm-v1.1\",\"k\":");
  json_write_escaped(out, k);
  fprintf(out, ",\"id\":%lld", (long long)zid);

  if (g_zasm_map_out) {
    fprintf(g_zasm_map_out, "{\"k\":\"zasm_map\",\"zid\":%lld", (long long)zid);
    if (k && *k) {
      fprintf(g_zasm_map_out, ",\"z_k\":");
      json_write_escaped(g_zasm_map_out, k);
    }
    if (g_zasm_about_node_id >= 0) {
      fprintf(g_zasm_map_out, ",\"sir_node\":%lld", (long long)g_zasm_about_node_id);
      if (g_zasm_about_node_tag && *g_zasm_about_node_tag) {
        fprintf(g_zasm_map_out, ",\"sir_tag\":");
        json_write_escaped(g_zasm_map_out, g_zasm_about_node_tag);
      }
    }
    fprintf(g_zasm_map_out, "}\n");
  }
}

void zasm_write_loc(FILE* out, int64_t line) { fprintf(out, ",\"loc\":{\"line\":%lld}", (long long)line); }

void zasm_write_op_reg(FILE* out, const char* r) {
  fprintf(out, "{\"t\":\"reg\",\"v\":");
  json_write_escaped(out, r);
  fprintf(out, "}");
}

void zasm_write_op_sym(FILE* out, const char* s) {
  fprintf(out, "{\"t\":\"sym\",\"v\":");
  json_write_escaped(out, s);
  fprintf(out, "}");
}

void zasm_write_op_lbl(FILE* out, const char* s) {
  fprintf(out, "{\"t\":\"lbl\",\"v\":");
  json_write_escaped(out, s);
  fprintf(out, "}");
}

void zasm_write_op_num(FILE* out, int64_t v) { fprintf(out, "{\"t\":\"num\",\"v\":%lld}", (long long)v); }

void zasm_write_op_str(FILE* out, const char* s) {
  fprintf(out, "{\"t\":\"str\",\"v\":");
  json_write_escaped(out, s ? s : "");
  fprintf(out, "}");
}

void zasm_write_op_mem(FILE* out, const ZasmOp* base, int64_t disp, int64_t size_hint) {
  fprintf(out, "{\"t\":\"mem\",\"base\":");
  if (base->k == ZOP_REG) {
    zasm_write_op_reg(out, base->s);
  } else {
    zasm_write_op_sym(out, base->s);
  }
  if (disp) fprintf(out, ",\"disp\":%lld", (long long)disp);
  if (size_hint) fprintf(out, ",\"size\":%lld", (long long)size_hint);
  fprintf(out, "}");
}

bool zasm_write_op(FILE* out, const ZasmOp* op) {
  if (!out || !op) return false;
  switch (op->k) {
    case ZOP_REG:
      zasm_write_op_reg(out, op->s);
      return true;
    case ZOP_SYM:
      zasm_write_op_sym(out, op->s);
      return true;
    case ZOP_LBL:
      zasm_write_op_lbl(out, op->s);
      return true;
    case ZOP_NUM:
      zasm_write_op_num(out, op->n);
      return true;
    case ZOP_SLOT:
      // Slots are not valid as direct operands; caller must materialize into a reg or mem operand.
      return false;
    default:
      return false;
  }
}

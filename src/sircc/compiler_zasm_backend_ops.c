// SPDX-FileCopyrightText: 2026 Frogfish
// SPDX-License-Identifier: GPL-3.0-or-later

#include "compiler_zasm_backend_helpers.h"
#include "compiler_zasm_regcache.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

const char* zasm_mnemonic_for_binop(const char* tag) {
  if (!tag) return NULL;

  // 32-bit ops.
  if (strcmp(tag, "i32.add") == 0) return "ADD";
  if (strcmp(tag, "i32.sub") == 0) return "SUB";
  if (strcmp(tag, "i32.mul") == 0) return "MUL";
  if (strcmp(tag, "i32.div.s") == 0) return "DIVS";
  if (strcmp(tag, "i32.div.u") == 0) return "DIVU";
  if (strcmp(tag, "i32.rem.s") == 0) return "REMS";
  if (strcmp(tag, "i32.rem.u") == 0) return "REMU";
  if (strcmp(tag, "i32.and") == 0) return "AND";
  if (strcmp(tag, "i32.or") == 0) return "OR";
  if (strcmp(tag, "i32.xor") == 0) return "XOR";
  if (strcmp(tag, "i32.shl") == 0) return "SLA";
  if (strcmp(tag, "i32.shr.s") == 0) return "SRA";
  if (strcmp(tag, "i32.shr.u") == 0) return "SRL";
  if (strcmp(tag, "i32.rotl") == 0) return "ROL";
  if (strcmp(tag, "i32.rotr") == 0) return "ROR";

  // 64-bit ops.
  if (strcmp(tag, "i64.add") == 0) return "ADD64";
  if (strcmp(tag, "i64.sub") == 0) return "SUB64";
  if (strcmp(tag, "i64.mul") == 0) return "MUL64";
  if (strcmp(tag, "i64.div.s") == 0) return "DIVS64";
  if (strcmp(tag, "i64.div.u") == 0) return "DIVU64";
  if (strcmp(tag, "i64.rem.s") == 0) return "REMS64";
  if (strcmp(tag, "i64.rem.u") == 0) return "REMU64";
  if (strcmp(tag, "i64.and") == 0) return "AND64";
  if (strcmp(tag, "i64.or") == 0) return "OR64";
  if (strcmp(tag, "i64.xor") == 0) return "XOR64";
  if (strcmp(tag, "i64.shl") == 0) return "SLA64";
  if (strcmp(tag, "i64.shr.s") == 0) return "SRA64";
  if (strcmp(tag, "i64.shr.u") == 0) return "SRL64";
  if (strcmp(tag, "i64.rotl") == 0) return "ROL64";
  if (strcmp(tag, "i64.rotr") == 0) return "ROR64";

  return NULL;
}

const char* zasm_mnemonic_for_unop(const char* tag) {
  if (!tag) return NULL;
  if (strcmp(tag, "i32.clz") == 0) return "CLZ";
  if (strcmp(tag, "i32.ctz") == 0) return "CTZ";
  if (strcmp(tag, "i32.popc") == 0) return "POPC";
  if (strcmp(tag, "i64.clz") == 0) return "CLZ64";
  if (strcmp(tag, "i64.ctz") == 0) return "CTZ64";
  if (strcmp(tag, "i64.popc") == 0) return "POPC64";
  return NULL;
}

const char* zasm_cmp_set_mnemonic_for_node_tag(const char* tag) {
  if (!tag) return NULL;
  const char* op = NULL;
  bool is64 = false;
  if (strncmp(tag, "i32.cmp.", 8) == 0) {
    op = tag + 8;
    is64 = false;
  } else if (strncmp(tag, "i64.cmp.", 8) == 0) {
    op = tag + 8;
    is64 = true;
  } else {
    return NULL;
  }

  if (strcmp(op, "eq") == 0) return is64 ? "EQ64" : "EQ";
  if (strcmp(op, "ne") == 0) return is64 ? "NE64" : "NE";

  if (strcmp(op, "slt") == 0) return is64 ? "LTS64" : "LTS";
  if (strcmp(op, "sle") == 0) return is64 ? "LES64" : "LES";
  if (strcmp(op, "sgt") == 0) return is64 ? "GTS64" : "GTS";
  if (strcmp(op, "sge") == 0) return is64 ? "GES64" : "GES";

  if (strcmp(op, "ult") == 0) return is64 ? "LTU64" : "LTU";
  if (strcmp(op, "ule") == 0) return is64 ? "LEU64" : "LEU";
  if (strcmp(op, "ugt") == 0) return is64 ? "GTU64" : "GTU";
  if (strcmp(op, "uge") == 0) return is64 ? "GEU64" : "GEU";

  return NULL;
}

bool emit_cp_hl(FILE* out, const ZasmOp* rhs, int64_t line_no) {
  if (!out || !rhs) return false;
  zasm_write_ir_k(out, "instr");
  fprintf(out, ",\"m\":\"CP\",\"ops\":[");
  zasm_write_op_reg(out, "HL");
  fprintf(out, ",");
  if (!zasm_write_op(out, rhs)) return false;
  fprintf(out, "]");
  zasm_write_loc(out, line_no);
  fprintf(out, "}\n");
  return true;
}

bool emit_cmp_set_hl(FILE* out, const char* mnemonic, const ZasmOp* rhs, int64_t line_no) {
  if (!out || !mnemonic || !rhs) return false;
  zasm_write_ir_k(out, "instr");
  fprintf(out, ",\"m\":");
  json_write_escaped(out, mnemonic);
  fprintf(out, ",\"ops\":[");
  zasm_write_op_reg(out, "HL");
  fprintf(out, ",");
  if (!zasm_write_op(out, rhs)) return false;
  fprintf(out, "]");
  zasm_write_loc(out, line_no);
  fprintf(out, "}\n");
  zasm_regcache_invalidate_reg("HL");
  return true;
}


// SPDX-FileCopyrightText: 2026 Frogfish
// SPDX-License-Identifier: GPL-3.0-or-later

#include "compiler_zasm_backend_helpers.h"
#include "compiler_zasm_regcache.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool emit_binop_into_hl(
    FILE* out,
    SirProgram* p,
    ZasmStr* strs,
    size_t strs_len,
    ZasmAlloca* allocas,
    size_t allocas_len,
    ZasmNameBinding* names,
    size_t names_len,
    ZasmBParamSlot* bps,
    size_t bps_len,
    NodeRec* vn,
    const char* m32,
    const char* m64,
    int64_t width_bytes,
    int64_t* io_line) {
  if (!out || !p || !vn || !vn->fields || !io_line) return false;
  JsonValue* args = json_obj_get(vn->fields, "args");
  if (!args || args->type != JSON_ARRAY || args->v.arr.len != 2) {
    errf(p, "sircc: zasm: %s node %lld requires args:[a,b]", vn->tag, (long long)vn->id);
    return false;
  }

  int64_t a_id = 0, b_id = 0;
  if (!parse_node_ref_id(p, args->v.arr.items[0], &a_id) || !parse_node_ref_id(p, args->v.arr.items[1], &b_id)) {
    errf(p, "sircc: zasm: %s node %lld args must be node refs", vn->tag, (long long)vn->id);
    return false;
  }

  ZasmOp a = {0};
  if (!zasm_lower_value_to_op(p, strs, strs_len, allocas, allocas_len, names, names_len, bps, bps_len, a_id, &a)) return false;
  if (a.k == ZOP_SLOT) {
    if (!emit_load_slot_to_reg(out, a.s, a.n, reg_for_width(width_bytes), (*io_line)++)) return false;
  } else {
    if (!emit_ld_reg_or_imm(out, reg_for_width(width_bytes), &a, (*io_line)++)) return false;
  }

  ZasmOp b = {0};
  if (!zasm_lower_value_to_op(p, strs, strs_len, allocas, allocas_len, names, names_len, bps, bps_len, b_id, &b)) return false;
  ZasmOp rhs = b;
  if (b.k == ZOP_SLOT) {
    if (a.k == ZOP_SLOT && a.s && b.s && strcmp(a.s, b.s) == 0 && a.n == b.n) {
      // Optional micro-opt: avoid a duplicate load when a and b are the same slot.
      rhs.k = ZOP_REG;
      rhs.s = "HL";
    } else {
      // Materialize RHS into DE for binary ops.
      if (!emit_load_slot_to_reg(out, b.s, b.n, "DE", (*io_line)++)) return false;
      rhs.k = ZOP_REG;
      rhs.s = "DE";
    }
  }

  zasm_write_ir_k(out, "instr");
  fprintf(out, ",\"m\":");
  json_write_escaped(out, (width_bytes == 8) ? m64 : m32);
  fprintf(out, ",\"ops\":[");
  zasm_write_op_reg(out, "HL");
  fprintf(out, ",");
  if (!zasm_write_op(out, &rhs)) return false;
  fprintf(out, "]");
  zasm_write_loc(out, (*io_line)++);
  fprintf(out, "}\n");
  zasm_regcache_invalidate_reg("HL");
  return true;
}

bool emit_zir_nonterm_stmt(
    FILE* out,
    SirProgram* p,
    ZasmStr* strs,
    size_t strs_len,
    ZasmAlloca* allocas,
    size_t allocas_len,
    ZasmNameBinding** names,
    size_t* name_len,
    size_t* name_cap,
    ZasmBParamSlot* bps,
    size_t bps_len,
    ZasmTempSlot** tmps,
    size_t* tmp_len,
    size_t* tmp_cap,
    NodeRec* s,
    int64_t* io_line) {
  if (!out || !p || !names || !name_len || !name_cap || !s || !io_line) return false;
  zasm_set_about_node(s->id, s->tag);

  if (strcmp(s->tag, "let") == 0) {
    // let: fields.name (str), fields.value (ref).
    const char* bind_name = NULL;
    JsonValue* nv = s->fields ? json_obj_get(s->fields, "name") : NULL;
    if (nv && nv->type == JSON_STRING) bind_name = nv->v.s;
    int64_t vid = 0;
    JsonValue* vv = s->fields ? json_obj_get(s->fields, "value") : NULL;
    if (!parse_node_ref_id(p, vv, &vid)) {
      errf(p, "sircc: zasm: let node %lld requires fields.value node ref", (long long)s->id);
      return false;
    }
    NodeRec* vn = get_node(p, vid);
    if (!vn) {
      errf(p, "sircc: zasm: let node %lld references unknown value node %lld", (long long)s->id, (long long)vid);
      return false;
    }

    // call statements.
    if (strcmp(vn->tag, "call") == 0) {
      if (!zasm_emit_call_stmt(out, p, strs, strs_len, allocas, allocas_len, *names, *name_len, bps, bps_len, vid, io_line)) return false;
      zasm_regcache_clear_all();

      if (bind_name && strcmp(bind_name, "_") != 0) {
        const char* slot_sym = NULL;
        if (!add_temp_slot(p, tmps, tmp_len, tmp_cap, s->id, 8, &slot_sym)) {
          errf(p, "sircc: zasm: out of memory");
          return false;
        }
        if (!emit_st64_slot_from_hl(out, slot_sym, (*io_line)++)) return false;
        if (!emit_bind_slot(p, names, name_len, name_cap, bind_name, slot_sym, 8)) return false;
      }
      return true;
    }

    if (strncmp(vn->tag, "load.", 5) == 0) {
      int64_t width = 0;
      const char* m = NULL;
      const char* dst_reg = NULL;
      if (strcmp(vn->tag, "load.i8") == 0) {
        width = 1;
        m = "LD8U";
        dst_reg = "A";
      } else if (strcmp(vn->tag, "load.i16") == 0) {
        width = 2;
        m = "LD16U";
        dst_reg = "HL";
      } else if (strcmp(vn->tag, "load.i32") == 0) {
        width = 4;
        m = "LD32U64";
        dst_reg = "HL";
      } else if (strcmp(vn->tag, "load.i64") == 0 || strcmp(vn->tag, "load.ptr") == 0) {
        width = 8;
        m = "LD64";
        dst_reg = "HL";
      } else {
        errf(p, "sircc: zasm: unsupported load '%s'", vn->tag);
        return false;
      }

      int64_t addr_id = 0;
      JsonValue* av = vn->fields ? json_obj_get(vn->fields, "addr") : NULL;
      if (!parse_node_ref_id(p, av, &addr_id)) {
        errf(p, "sircc: zasm: %s node %lld requires fields.addr node ref", vn->tag, (long long)vn->id);
        return false;
      }
      ZasmOp base = {0};
      int64_t disp = 0;
      zasm_regcache_clear_all();
      if (!zasm_emit_addr_to_mem(out, p, strs, strs_len, allocas, allocas_len, *names, *name_len, bps, bps_len, addr_id, &base, &disp, io_line))
        return false;

      zasm_write_ir_k(out, "instr");
      fprintf(out, ",\"m\":");
      json_write_escaped(out, m);
      fprintf(out, ",\"ops\":[");
      zasm_write_op_reg(out, dst_reg);
      fprintf(out, ",");
      zasm_write_op_mem(out, &base, disp, width);
      fprintf(out, "]");
      zasm_write_loc(out, (*io_line)++);
      fprintf(out, "}\n");
      zasm_regcache_invalidate_reg(dst_reg);

      if (bind_name && strcmp(bind_name, "_") != 0) {
        const char* slot_sym = NULL;
        if (!add_temp_slot(p, tmps, tmp_len, tmp_cap, s->id, width, &slot_sym)) {
          errf(p, "sircc: zasm: out of memory");
          return false;
        }
        if (!emit_store_reg_to_slot(out, slot_sym, width, dst_reg, (*io_line)++)) return false;
        if (!emit_bind_slot(p, names, name_len, name_cap, bind_name, slot_sym, width)) return false;
      }
      return true;
    }

    const char* m = zasm_mnemonic_for_binop(vn->tag);
    if (m) {
      int64_t width = 0;
      if (strncmp(vn->tag, "i32.", 4) == 0) width = 4;
      if (strncmp(vn->tag, "i64.", 4) == 0) width = 8;
      if (!width) {
        errf(p, "sircc: zasm: %s width unsupported", vn->tag);
        return false;
      }
      if (!bind_name || strcmp(bind_name, "_") == 0) {
        errf(p, "sircc: zasm: %s must be bound via let name", vn->tag);
        return false;
      }

      const char* m32 = (width == 4) ? m : NULL;
      const char* m64 = (width == 8) ? m : NULL;
      if (!emit_binop_into_hl(out, p, strs, strs_len, allocas, allocas_len, *names, *name_len, bps, bps_len, vn, m32, m64, width, io_line)) return false;

      const char* slot_sym = NULL;
      if (!add_temp_slot(p, tmps, tmp_len, tmp_cap, s->id, width, &slot_sym)) {
        errf(p, "sircc: zasm: out of memory");
        return false;
      }
      if (!emit_store_reg_to_slot(out, slot_sym, width, "HL", (*io_line)++)) return false;
      if (!emit_bind_slot(p, names, name_len, name_cap, bind_name, slot_sym, width)) return false;
      return true;
    }

    const char* um = zasm_mnemonic_for_unop(vn->tag);
    if (um) {
      int64_t width = 0;
      if (strncmp(vn->tag, "i32.", 4) == 0) width = 4;
      if (strncmp(vn->tag, "i64.", 4) == 0) width = 8;
      if (!width) {
        errf(p, "sircc: zasm: %s width unsupported", vn->tag);
        return false;
      }
      if (!bind_name || strcmp(bind_name, "_") == 0) {
        errf(p, "sircc: zasm: %s must be bound via let name", vn->tag);
        return false;
      }

      JsonValue* args = vn->fields ? json_obj_get(vn->fields, "args") : NULL;
      if (!args || args->type != JSON_ARRAY || args->v.arr.len != 1) {
        errf(p, "sircc: zasm: %s node %lld requires args:[x]", vn->tag, (long long)vn->id);
        return false;
      }
      int64_t x_id = 0;
      if (!parse_node_ref_id(p, args->v.arr.items[0], &x_id)) {
        errf(p, "sircc: zasm: %s node %lld arg must be node ref", vn->tag, (long long)vn->id);
        return false;
      }
      ZasmOp x = {0};
      if (!zasm_lower_value_to_op(p, strs, strs_len, allocas, allocas_len, *names, *name_len, bps, bps_len, x_id, &x)) return false;
      if (x.k == ZOP_SLOT) {
        if (!emit_load_slot_to_reg(out, x.s, x.n, "HL", (*io_line)++)) return false;
      } else {
        if (!emit_ld_reg_or_imm(out, "HL", &x, (*io_line)++)) return false;
      }

      zasm_write_ir_k(out, "instr");
      fprintf(out, ",\"m\":");
      json_write_escaped(out, um);
      fprintf(out, ",\"ops\":[");
      zasm_write_op_reg(out, "HL");
      fprintf(out, "]");
      zasm_write_loc(out, (*io_line)++);
      fprintf(out, "}\n");
      zasm_regcache_invalidate_reg("HL");

      const char* slot_sym = NULL;
      if (!add_temp_slot(p, tmps, tmp_len, tmp_cap, s->id, width, &slot_sym)) {
        errf(p, "sircc: zasm: out of memory");
        return false;
      }
      if (!emit_store_reg_to_slot(out, slot_sym, width, "HL", (*io_line)++)) return false;
      if (!emit_bind_slot(p, names, name_len, name_cap, bind_name, slot_sym, width)) return false;
      return true;
    }

    // Pure-ish binding of stable values (consts/symbols); no code emitted.
    if (bind_name && strcmp(bind_name, "_") != 0) {
      ZasmOp op = {0};
      if (!zasm_lower_value_to_op(p, strs, strs_len, allocas, allocas_len, *names, *name_len, bps, bps_len, vid, &op)) return false;
      if (!emit_bind_op(p, names, name_len, name_cap, bind_name, op)) return false;
    }
    return true;
  }

  if (strcmp(s->tag, "mem.fill") == 0) {
    zasm_regcache_clear_all();
    if (!zasm_emit_mem_fill_stmt(out, p, strs, strs_len, allocas, allocas_len, *names, *name_len, bps, bps_len, s, io_line)) return false;
    zasm_regcache_clear_all();
    return true;
  }

  if (strcmp(s->tag, "mem.copy") == 0) {
    zasm_regcache_clear_all();
    if (!zasm_emit_mem_copy_stmt(out, p, strs, strs_len, allocas, allocas_len, *names, *name_len, bps, bps_len, s, io_line)) return false;
    zasm_regcache_clear_all();
    return true;
  }

  if (strncmp(s->tag, "store.", 6) == 0) {
    zasm_regcache_clear_all();
    if (!zasm_emit_store_stmt(out, p, strs, strs_len, allocas, allocas_len, *names, *name_len, bps, bps_len, s, io_line)) return false;
    zasm_regcache_clear_all();
    return true;
  }

  errf(p, "sircc: zasm: unsupported stmt tag '%s' in zir_main", s->tag);
  return false;
}

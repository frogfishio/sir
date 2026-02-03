// SPDX-FileCopyrightText: 2026 Frogfish
// SPDX-License-Identifier: GPL-3.0-or-later

#include "compiler_internal.h"

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef enum {
  ZOP_NONE = 0,
  ZOP_REG,
  ZOP_SYM,
  ZOP_LBL,
  ZOP_NUM,
} ZasmOpKind;

typedef struct {
  ZasmOpKind k;
  const char* s;
  int64_t n;
} ZasmOp;

typedef struct {
  int64_t node_id;
  const char* sym;
  int64_t size_bytes;
} ZasmAlloca;

static void write_ir_k(FILE* out, const char* k) {
  fprintf(out, "{\"ir\":\"zasm-v1.1\",\"k\":");
  json_write_escaped(out, k);
}

static void write_loc(FILE* out, int64_t line) {
  fprintf(out, ",\"loc\":{\"line\":%lld}", (long long)line);
}

static void write_op_reg(FILE* out, const char* r) {
  fprintf(out, "{\"t\":\"reg\",\"v\":");
  json_write_escaped(out, r);
  fprintf(out, "}");
}

static void write_op_sym(FILE* out, const char* s) {
  fprintf(out, "{\"t\":\"sym\",\"v\":");
  json_write_escaped(out, s);
  fprintf(out, "}");
}

static void write_op_lbl(FILE* out, const char* s) {
  fprintf(out, "{\"t\":\"lbl\",\"v\":");
  json_write_escaped(out, s);
  fprintf(out, "}");
}

static void write_op_num(FILE* out, int64_t v) {
  fprintf(out, "{\"t\":\"num\",\"v\":%lld}", (long long)v);
}

static void write_op_str(FILE* out, const char* s) {
  fprintf(out, "{\"t\":\"str\",\"v\":");
  json_write_escaped(out, s ? s : "");
  fprintf(out, "}");
}

static void write_op_mem(FILE* out, const ZasmOp* base, int64_t disp, int64_t size_hint) {
  fprintf(out, "{\"t\":\"mem\",\"base\":");
  if (base->k == ZOP_REG) {
    write_op_reg(out, base->s);
  } else {
    write_op_sym(out, base->s);
  }
  if (disp) fprintf(out, ",\"disp\":%lld", (long long)disp);
  if (size_hint) fprintf(out, ",\"size\":%lld", (long long)size_hint);
  fprintf(out, "}");
}

static bool zasm_op_is_value(const ZasmOp* op) {
  if (!op) return false;
  return op->k == ZOP_REG || op->k == ZOP_SYM || op->k == ZOP_NUM;
}

static NodeRec* find_fn(SirProgram* p, const char* name) {
  if (!p || !name) return NULL;
  for (size_t i = 0; i < p->nodes_cap; i++) {
    NodeRec* n = p->nodes[i];
    if (!n) continue;
    if (strcmp(n->tag, "fn") != 0) continue;
    const char* nm = n->fields ? json_get_string(json_obj_get(n->fields, "name")) : NULL;
    if (nm && strcmp(nm, name) == 0) return n;
  }
  return NULL;
}

typedef struct {
  int64_t node_id;
  const char* sym;
  const char* value;
  int64_t len;
} ZasmStr;

static const char* sym_for_str(ZasmStr* strs, size_t n, int64_t node_id) {
  for (size_t i = 0; i < n; i++) {
    if (strs[i].node_id == node_id) return strs[i].sym;
  }
  return NULL;
}

static const char* sym_for_alloca(ZasmAlloca* as, size_t n, int64_t node_id) {
  for (size_t i = 0; i < n; i++) {
    if (as[i].node_id == node_id) return as[i].sym;
  }
  return NULL;
}

static bool collect_cstrs(SirProgram* p, ZasmStr** out_strs, size_t* out_len) {
  if (!p || !out_strs || !out_len) return false;
  *out_strs = NULL;
  *out_len = 0;

  size_t cap = 0;
  size_t len = 0;
  ZasmStr* strs = NULL;

  for (size_t i = 0; i < p->nodes_cap; i++) {
    NodeRec* n = p->nodes[i];
    if (!n) continue;
    if (strcmp(n->tag, "cstr") != 0) continue;
    if (!n->fields) continue;
    const char* s = json_get_string(json_obj_get(n->fields, "value"));
    if (!s) continue;

    if (len == cap) {
      size_t next = cap ? cap * 2 : 8;
      ZasmStr* bigger = (ZasmStr*)realloc(strs, next * sizeof(ZasmStr));
      if (!bigger) {
        free(strs);
        return false;
      }
      strs = bigger;
      cap = next;
    }

    char name_buf[64];
    snprintf(name_buf, sizeof(name_buf), "str_%lld", (long long)n->id);
    char* sym = arena_strdup(&p->arena, name_buf);
    if (!sym) {
      free(strs);
      return false;
    }

    // zasm STR must be null-free. JSON strings can't contain NUL anyway, but keep the check.
    for (const char* it = s; *it; it++) {
      if (*it == '\0') {
        free(strs);
        errf(p, "sircc: zasm STR cannot contain NUL bytes");
        return false;
      }
    }

    strs[len++] = (ZasmStr){
        .node_id = n->id,
        .sym = sym,
        .value = s,
        .len = (int64_t)strlen(s),
    };
  }

  *out_strs = strs;
  *out_len = len;
  return true;
}

static bool alloca_size_for_tag(const char* tag, int64_t* out_size_bytes) {
  if (!tag || !out_size_bytes) return false;
  *out_size_bytes = 0;

  const char* suffix = NULL;
  if (strncmp(tag, "alloca.", 7) == 0) suffix = tag + 7;
  if (!suffix || !*suffix) return false;

  if (strcmp(suffix, "i8") == 0) {
    *out_size_bytes = 1;
    return true;
  }
  if (strcmp(suffix, "i16") == 0) {
    *out_size_bytes = 2;
    return true;
  }
  if (strcmp(suffix, "i32") == 0 || strcmp(suffix, "f32") == 0) {
    *out_size_bytes = 4;
    return true;
  }
  if (strcmp(suffix, "i64") == 0 || strcmp(suffix, "f64") == 0 || strcmp(suffix, "ptr") == 0) {
    *out_size_bytes = 8;
    return true;
  }
  return false;
}

static bool collect_allocas(SirProgram* p, ZasmAlloca** out_as, size_t* out_len) {
  if (!p || !out_as || !out_len) return false;
  *out_as = NULL;
  *out_len = 0;

  size_t cap = 0;
  size_t len = 0;
  ZasmAlloca* as = NULL;

  for (size_t i = 0; i < p->nodes_cap; i++) {
    NodeRec* n = p->nodes[i];
    if (!n) continue;
    if (strncmp(n->tag, "alloca.", 7) != 0) continue;

    int64_t size_bytes = 0;
    if (!alloca_size_for_tag(n->tag, &size_bytes)) continue;

    if (len == cap) {
      size_t next = cap ? cap * 2 : 8;
      ZasmAlloca* bigger = (ZasmAlloca*)realloc(as, next * sizeof(ZasmAlloca));
      if (!bigger) {
        free(as);
        return false;
      }
      as = bigger;
      cap = next;
    }

    char name_buf[64];
    snprintf(name_buf, sizeof(name_buf), "alloc_%lld", (long long)n->id);
    char* sym = arena_strdup(&p->arena, name_buf);
    if (!sym) {
      free(as);
      return false;
    }

    as[len++] = (ZasmAlloca){
        .node_id = n->id,
        .sym = sym,
        .size_bytes = size_bytes,
    };
  }

  *out_as = as;
  *out_len = len;
  return true;
}

static bool collect_decl_fns(SirProgram* p, const char*** out_names, size_t* out_len) {
  if (!p || !out_names || !out_len) return false;
  *out_names = NULL;
  *out_len = 0;

  size_t cap = 0;
  size_t len = 0;
  const char** names = NULL;

  for (size_t i = 0; i < p->nodes_cap; i++) {
    NodeRec* n = p->nodes[i];
    if (!n) continue;
    if (strcmp(n->tag, "decl.fn") != 0) continue;
    const char* name = n->fields ? json_get_string(json_obj_get(n->fields, "name")) : NULL;
    if (!name) continue;

    bool seen = false;
    for (size_t j = 0; j < len; j++) {
      if (strcmp(names[j], name) == 0) {
        seen = true;
        break;
      }
    }
    if (seen) continue;

    if (len == cap) {
      size_t next = cap ? cap * 2 : 8;
      const char** bigger = (const char**)realloc(names, next * sizeof(const char*));
      if (!bigger) {
        free(names);
        return false;
      }
      names = bigger;
      cap = next;
    }
    names[len++] = name;
  }

  *out_names = names;
  *out_len = len;
  return true;
}

static bool lower_value_to_op(
    SirProgram* p,
    ZasmStr* strs,
    size_t strs_len,
    ZasmAlloca* allocas,
    size_t allocas_len,
    int64_t node_id,
    ZasmOp* out) {
  if (!p || !out) return false;
  *out = (ZasmOp){0};

  NodeRec* n = get_node(p, node_id);
  if (!n) {
    errf(p, "sircc: zasm: unknown node id %lld", (long long)node_id);
    return false;
  }

  if (strncmp(n->tag, "const.i", 7) == 0) {
    if (!n->fields) {
      errf(p, "sircc: zasm: %s node %lld missing fields", n->tag, (long long)node_id);
      return false;
    }
    int64_t v = 0;
    if (!must_i64(p, json_obj_get(n->fields, "value"), &v, "const.value")) return false;
    out->k = ZOP_NUM;
    out->n = v;
    return true;
  }

  if (strncmp(n->tag, "alloca.", 7) == 0) {
    const char* sym = sym_for_alloca(allocas, allocas_len, node_id);
    if (!sym) {
      errf(p, "sircc: zasm: missing alloca symbol mapping for node %lld", (long long)node_id);
      return false;
    }
    out->k = ZOP_SYM;
    out->s = sym;
    return true;
  }

  if (strcmp(n->tag, "cstr") == 0) {
    const char* sym = sym_for_str(strs, strs_len, node_id);
    if (!sym) {
      errf(p, "sircc: zasm: missing cstr symbol mapping for node %lld", (long long)node_id);
      return false;
    }
    out->k = ZOP_SYM;
    out->s = sym;
    return true;
  }

  if (strcmp(n->tag, "decl.fn") == 0) {
    const char* name = n->fields ? json_get_string(json_obj_get(n->fields, "name")) : NULL;
    if (!name) {
      errf(p, "sircc: zasm: decl.fn node %lld missing fields.name", (long long)node_id);
      return false;
    }
    out->k = ZOP_SYM;
    out->s = name;
    return true;
  }

  if (strcmp(n->tag, "ptr.sym") == 0) {
    const char* name = NULL;
    if (n->fields) name = json_get_string(json_obj_get(n->fields, "name"));
    if (!name) {
      errf(p, "sircc: zasm: ptr.sym node %lld missing fields.name", (long long)node_id);
      return false;
    }
    out->k = ZOP_SYM;
    out->s = name;
    return true;
  }

  if (strcmp(n->tag, "ptr.to_i64") == 0) {
    JsonValue* args = n->fields ? json_obj_get(n->fields, "args") : NULL;
    if (!args || args->type != JSON_ARRAY || args->v.arr.len != 1) {
      errf(p, "sircc: zasm: ptr.to_i64 node %lld requires args:[x]", (long long)node_id);
      return false;
    }
    int64_t x_id = 0;
    if (!parse_node_ref_id(args->v.arr.items[0], &x_id)) {
      errf(p, "sircc: zasm: ptr.to_i64 node %lld arg must be node ref", (long long)node_id);
      return false;
    }
    return lower_value_to_op(p, strs, strs_len, allocas, allocas_len, x_id, out);
  }

  if (strcmp(n->tag, "name") == 0) {
    // TODO: add real name binding and register allocation.
    const char* name = n->fields ? json_get_string(json_obj_get(n->fields, "name")) : NULL;
    errf(p, "sircc: zasm: name '%s' not supported yet (node %lld)", name ? name : "(null)", (long long)node_id);
    return false;
  }

  errf(p, "sircc: zasm: unsupported value node '%s' (node %lld)", n->tag, (long long)node_id);
  return false;
}

static bool write_op(FILE* out, const ZasmOp* op) {
  if (!out || !op) return false;
  switch (op->k) {
    case ZOP_REG:
      write_op_reg(out, op->s);
      return true;
    case ZOP_SYM:
      write_op_sym(out, op->s);
      return true;
    case ZOP_LBL:
      write_op_lbl(out, op->s);
      return true;
    case ZOP_NUM:
      write_op_num(out, op->n);
      return true;
    default:
      return false;
  }
}

static bool emit_call_stmt(
    FILE* out,
    SirProgram* p,
    ZasmStr* strs,
    size_t strs_len,
    ZasmAlloca* allocas,
    size_t allocas_len,
    int64_t call_id,
    int64_t line_no) {
  NodeRec* n = get_node(p, call_id);
  if (!n || !n->fields) return false;

  JsonValue* args = json_obj_get(n->fields, "args");
  if (!args || args->type != JSON_ARRAY || args->v.arr.len < 1) {
    errf(p, "sircc: zasm: %s node %lld missing args array", n->tag, (long long)call_id);
    return false;
  }

  // Callee is args[0] (node ref).
  int64_t callee_id = 0;
  if (!parse_node_ref_id(args->v.arr.items[0], &callee_id)) {
    errf(p, "sircc: zasm: %s node %lld args[0] must be node ref", n->tag, (long long)call_id);
    return false;
  }
  ZasmOp callee = {0};
  if (!lower_value_to_op(p, strs, strs_len, allocas, allocas_len, callee_id, &callee) || callee.k != ZOP_SYM) {
    errf(p, "sircc: zasm: %s node %lld callee must be a direct symbol (decl.fn/ptr.sym)", n->tag, (long long)call_id);
    return false;
  }

  write_ir_k(out, "instr");
  fprintf(out, ",\"m\":\"CALL\",\"ops\":[");
  write_op_sym(out, callee.s);

  // Remaining args become call operands (portable zir convention; lowerers/JIT can canonicalize).
  for (size_t i = 1; i < args->v.arr.len; i++) {
    int64_t aid = 0;
    if (!parse_node_ref_id(args->v.arr.items[i], &aid)) {
      errf(p, "sircc: zasm: %s node %lld arg[%zu] must be node ref", n->tag, (long long)call_id, i);
      return false;
    }
    ZasmOp op = {0};
    if (!lower_value_to_op(p, strs, strs_len, allocas, allocas_len, aid, &op) || !zasm_op_is_value(&op)) {
      errf(p, "sircc: zasm: %s node %lld arg[%zu] unsupported", n->tag, (long long)call_id, i);
      return false;
    }
    fprintf(out, ",");
    if (!write_op(out, &op)) return false;
  }
  fprintf(out, "]");
  write_loc(out, line_no);
  fprintf(out, "}\n");
  return true;
}

static bool emit_ld(FILE* out, const char* dst_reg, const ZasmOp* src, int64_t line_no) {
  if (!out || !dst_reg || !src) return false;
  write_ir_k(out, "instr");
  fprintf(out, ",\"m\":\"LD\",\"ops\":[");
  write_op_reg(out, dst_reg);
  fprintf(out, ",");
  if (!write_op(out, src)) return false;
  fprintf(out, "]");
  write_loc(out, line_no);
  fprintf(out, "}\n");
  return true;
}

static bool emit_store_stmt(FILE* out, SirProgram* p, ZasmStr* strs, size_t strs_len, ZasmAlloca* allocas, size_t allocas_len, NodeRec* s, int64_t line_no) {
  if (!out || !p || !s || !s->fields) return false;
  if (strcmp(s->tag, "store.i8") != 0) {
    errf(p, "sircc: zasm: unsupported store width '%s'", s->tag);
    return false;
  }

  int64_t addr_id = 0;
  int64_t value_id = 0;
  JsonValue* av = json_obj_get(s->fields, "addr");
  JsonValue* vv = json_obj_get(s->fields, "value");
  if (!parse_node_ref_id(av, &addr_id) || !parse_node_ref_id(vv, &value_id)) {
    errf(p, "sircc: zasm: %s node %lld requires fields.addr/value node refs", s->tag, (long long)s->id);
    return false;
  }

  ZasmOp addr = {0};
  if (!lower_value_to_op(p, strs, strs_len, allocas, allocas_len, addr_id, &addr) || addr.k != ZOP_SYM) {
    errf(p, "sircc: zasm: %s addr must be an alloca symbol (node %lld)", s->tag, (long long)addr_id);
    return false;
  }
  ZasmOp val = {0};
  if (!lower_value_to_op(p, strs, strs_len, allocas, allocas_len, value_id, &val) || val.k != ZOP_NUM) {
    errf(p, "sircc: zasm: %s value must be an immediate const (node %lld)", s->tag, (long long)value_id);
    return false;
  }

  ZasmOp byte = {.k = ZOP_NUM, .n = (uint8_t)val.n};
  if (!emit_ld(out, "A", &byte, line_no)) return false;

  write_ir_k(out, "instr");
  fprintf(out, ",\"m\":\"ST8\",\"ops\":[");
  write_op_mem(out, &addr, 0, 1);
  fprintf(out, ",");
  write_op_reg(out, "A");
  fprintf(out, "]");
  write_loc(out, line_no + 1);
  fprintf(out, "}\n");
  return true;
}

static bool emit_mem_fill_stmt(FILE* out, SirProgram* p, ZasmStr* strs, size_t strs_len, ZasmAlloca* allocas, size_t allocas_len, NodeRec* s, int64_t line_no) {
  if (!out || !p || !s || !s->fields) return false;
  JsonValue* args = json_obj_get(s->fields, "args");
  if (!args || args->type != JSON_ARRAY || args->v.arr.len != 3) {
    errf(p, "sircc: zasm: mem.fill node %lld requires args:[dst, byte, len]", (long long)s->id);
    return false;
  }

  int64_t dst_id = 0, byte_id = 0, len_id = 0;
  if (!parse_node_ref_id(args->v.arr.items[0], &dst_id) || !parse_node_ref_id(args->v.arr.items[1], &byte_id) ||
      !parse_node_ref_id(args->v.arr.items[2], &len_id)) {
    errf(p, "sircc: zasm: mem.fill node %lld args must be node refs", (long long)s->id);
    return false;
  }

  ZasmOp dst = {0};
  if (!lower_value_to_op(p, strs, strs_len, allocas, allocas_len, dst_id, &dst) || dst.k != ZOP_SYM) {
    errf(p, "sircc: zasm: mem.fill dst must be an alloca symbol (node %lld)", (long long)dst_id);
    return false;
  }
  ZasmOp byte = {0};
  if (!lower_value_to_op(p, strs, strs_len, allocas, allocas_len, byte_id, &byte) || byte.k != ZOP_NUM) {
    errf(p, "sircc: zasm: mem.fill byte must be an immediate const (node %lld)", (long long)byte_id);
    return false;
  }
  ZasmOp len = {0};
  if (!lower_value_to_op(p, strs, strs_len, allocas, allocas_len, len_id, &len) || len.k != ZOP_NUM) {
    errf(p, "sircc: zasm: mem.fill len must be an immediate const (node %lld)", (long long)len_id);
    return false;
  }

  if (!emit_ld(out, "HL", &dst, line_no)) return false;
  ZasmOp b8 = {.k = ZOP_NUM, .n = (uint8_t)byte.n};
  if (!emit_ld(out, "A", &b8, line_no + 1)) return false;
  if (!emit_ld(out, "BC", &len, line_no + 2)) return false;

  write_ir_k(out, "instr");
  fprintf(out, ",\"m\":\"FILL\",\"ops\":[]");
  write_loc(out, line_no + 3);
  fprintf(out, "}\n");
  return true;
}

static bool emit_mem_copy_stmt(FILE* out, SirProgram* p, ZasmStr* strs, size_t strs_len, ZasmAlloca* allocas, size_t allocas_len, NodeRec* s, int64_t line_no) {
  if (!out || !p || !s || !s->fields) return false;
  JsonValue* args = json_obj_get(s->fields, "args");
  if (!args || args->type != JSON_ARRAY || args->v.arr.len != 3) {
    errf(p, "sircc: zasm: mem.copy node %lld requires args:[dst, src, len]", (long long)s->id);
    return false;
  }

  int64_t dst_id = 0, src_id = 0, len_id = 0;
  if (!parse_node_ref_id(args->v.arr.items[0], &dst_id) || !parse_node_ref_id(args->v.arr.items[1], &src_id) ||
      !parse_node_ref_id(args->v.arr.items[2], &len_id)) {
    errf(p, "sircc: zasm: mem.copy node %lld args must be node refs", (long long)s->id);
    return false;
  }

  ZasmOp dst = {0};
  if (!lower_value_to_op(p, strs, strs_len, allocas, allocas_len, dst_id, &dst) || dst.k != ZOP_SYM) {
    errf(p, "sircc: zasm: mem.copy dst must be an alloca symbol (node %lld)", (long long)dst_id);
    return false;
  }
  ZasmOp src = {0};
  if (!lower_value_to_op(p, strs, strs_len, allocas, allocas_len, src_id, &src) || src.k != ZOP_SYM) {
    errf(p, "sircc: zasm: mem.copy src must be an alloca symbol (node %lld)", (long long)src_id);
    return false;
  }
  ZasmOp len = {0};
  if (!lower_value_to_op(p, strs, strs_len, allocas, allocas_len, len_id, &len) || len.k != ZOP_NUM) {
    errf(p, "sircc: zasm: mem.copy len must be an immediate const (node %lld)", (long long)len_id);
    return false;
  }

  if (!emit_ld(out, "DE", &dst, line_no)) return false;
  if (!emit_ld(out, "HL", &src, line_no + 1)) return false;
  if (!emit_ld(out, "BC", &len, line_no + 2)) return false;

  write_ir_k(out, "instr");
  fprintf(out, ",\"m\":\"LDIR\",\"ops\":[]");
  write_loc(out, line_no + 3);
  fprintf(out, "}\n");
  return true;
}

static bool emit_ret_value_to_hl(
    FILE* out,
    SirProgram* p,
    ZasmStr* strs,
    size_t strs_len,
    ZasmAlloca* allocas,
    size_t allocas_len,
    int64_t value_id,
    int64_t line_no) {
  if (!out || !p) return false;

  NodeRec* v = get_node(p, value_id);
  if (!v) {
    errf(p, "sircc: zasm: return references unknown node %lld", (long long)value_id);
    return false;
  }

  if (strcmp(v->tag, "i32.zext.i8") == 0) {
    JsonValue* args = v->fields ? json_obj_get(v->fields, "args") : NULL;
    if (!args || args->type != JSON_ARRAY || args->v.arr.len != 1) {
      errf(p, "sircc: zasm: i32.zext.i8 node %lld requires args:[x]", (long long)value_id);
      return false;
    }
    int64_t x_id = 0;
    if (!parse_node_ref_id(args->v.arr.items[0], &x_id)) {
      errf(p, "sircc: zasm: i32.zext.i8 node %lld arg must be node ref", (long long)value_id);
      return false;
    }
    NodeRec* x = get_node(p, x_id);
    if (!x) {
      errf(p, "sircc: zasm: i32.zext.i8 references unknown node %lld", (long long)x_id);
      return false;
    }

    if (strcmp(x->tag, "load.i8") == 0) {
      int64_t addr_id = 0;
      JsonValue* av = x->fields ? json_obj_get(x->fields, "addr") : NULL;
      if (!parse_node_ref_id(av, &addr_id)) {
        errf(p, "sircc: zasm: load.i8 node %lld requires fields.addr node ref", (long long)x_id);
        return false;
      }
      ZasmOp base = {0};
      if (!lower_value_to_op(p, strs, strs_len, allocas, allocas_len, addr_id, &base) || base.k != ZOP_SYM) {
        errf(p, "sircc: zasm: load.i8 addr must be an alloca symbol (node %lld)", (long long)addr_id);
        return false;
      }

      write_ir_k(out, "instr");
      fprintf(out, ",\"m\":\"LD8U\",\"ops\":[");
      write_op_reg(out, "HL");
      fprintf(out, ",");
      write_op_mem(out, &base, 0, 1);
      fprintf(out, "]");
      write_loc(out, line_no);
      fprintf(out, "}\n");
      return true;
    }

    // Fallback: if arg is an immediate i8 const, treat it as already zero-extended.
    ZasmOp op = {0};
    if (!lower_value_to_op(p, strs, strs_len, allocas, allocas_len, x_id, &op) || op.k != ZOP_NUM) {
      errf(p, "sircc: zasm: i32.zext.i8 arg must be load.i8 or const.i8 (node %lld)", (long long)x_id);
      return false;
    }
    ZasmOp z = {.k = ZOP_NUM, .n = (uint8_t)op.n};
    return emit_ld(out, "HL", &z, line_no);
  }

  if (strcmp(v->tag, "load.i8") == 0) {
    int64_t addr_id = 0;
    JsonValue* av = v->fields ? json_obj_get(v->fields, "addr") : NULL;
    if (!parse_node_ref_id(av, &addr_id)) {
      errf(p, "sircc: zasm: load.i8 node %lld requires fields.addr node ref", (long long)value_id);
      return false;
    }
    ZasmOp base = {0};
    if (!lower_value_to_op(p, strs, strs_len, allocas, allocas_len, addr_id, &base) || base.k != ZOP_SYM) {
      errf(p, "sircc: zasm: load.i8 addr must be an alloca symbol (node %lld)", (long long)addr_id);
      return false;
    }

    write_ir_k(out, "instr");
    fprintf(out, ",\"m\":\"LD8U\",\"ops\":[");
    write_op_reg(out, "HL");
    fprintf(out, ",");
    write_op_mem(out, &base, 0, 1);
    fprintf(out, "]");
    write_loc(out, line_no);
    fprintf(out, "}\n");
    return true;
  }

  // Trivial values: const, cstr, alloca, decl.fn, ptr.sym, ptr.to_i64.
  ZasmOp rop = {0};
  if (!lower_value_to_op(p, strs, strs_len, allocas, allocas_len, value_id, &rop)) return false;
  if (rop.k == ZOP_NUM || rop.k == ZOP_SYM) return emit_ld(out, "HL", &rop, line_no);
  if (rop.k == ZOP_REG) {
    if (!rop.s || strcmp(rop.s, "HL") == 0) return true;
    return emit_ld(out, "HL", &rop, line_no);
  }
  errf(p, "sircc: zasm: unsupported return value shape");
  return false;
}

bool emit_zasm_v11(SirProgram* p, const char* out_path) {
  if (!p || !out_path) return false;

  NodeRec* zir_main = find_fn(p, "zir_main");
  if (!zir_main) {
    errf(p, "sircc: --emit-zasm currently requires a function named 'zir_main'");
    return false;
  }

  ZasmStr* strs = NULL;
  size_t strs_len = 0;
  if (!collect_cstrs(p, &strs, &strs_len)) return false;

  ZasmAlloca* allocas = NULL;
  size_t allocas_len = 0;
  if (!collect_allocas(p, &allocas, &allocas_len)) {
    free(strs);
    return false;
  }

  const char** decls = NULL;
  size_t decls_len = 0;
  if (!collect_decl_fns(p, &decls, &decls_len)) {
    free(strs);
    free(allocas);
    return false;
  }

  FILE* out = fopen(out_path, "wb");
  if (!out) {
    free(strs);
    free(allocas);
    free(decls);
    errf(p, "sircc: failed to open output: %s", strerror(errno));
    return false;
  }

  int64_t line = 1;

  // meta (optional, but nice for tooling)
  write_ir_k(out, "meta");
  fprintf(out, ",\"producer\":\"sircc\"");
  if (p->unit_name) {
    fprintf(out, ",\"unit\":");
    json_write_escaped(out, p->unit_name);
  }
  write_loc(out, line++);
  fprintf(out, "}\n");

  // EXTERN all declared functions (best-effort module is "c").
  for (size_t i = 0; i < decls_len; i++) {
    write_ir_k(out, "dir");
    fprintf(out, ",\"d\":\"EXTERN\",\"args\":[");
    write_op_str(out, "c");
    fprintf(out, ",");
    write_op_str(out, decls[i]);
    fprintf(out, ",");
    write_op_sym(out, decls[i]);
    fprintf(out, "]");
    write_loc(out, line++);
    fprintf(out, "}\n");
  }

  // PUBLIC zir_main
  write_ir_k(out, "dir");
  fprintf(out, ",\"d\":\"PUBLIC\",\"args\":[");
  write_op_sym(out, "zir_main");
  fprintf(out, "]");
  write_loc(out, line++);
  fprintf(out, "}\n\n");

  // label zir_main
  write_ir_k(out, "label");
  fprintf(out, ",\"name\":\"zir_main\"");
  write_loc(out, line++);
  fprintf(out, "}\n");

  // Lower a small subset of the legacy SIR form: fn.fields.body is a block with stmts.
  JsonValue* bodyv = zir_main->fields ? json_obj_get(zir_main->fields, "body") : NULL;
  int64_t body_id = 0;
  if (!parse_node_ref_id(bodyv, &body_id)) {
    fclose(out);
    free(strs);
    free(decls);
    errf(p, "sircc: zasm: fn zir_main missing body ref");
    return false;
  }
  NodeRec* body = get_node(p, body_id);
  if (!body || strcmp(body->tag, "block") != 0 || !body->fields) {
    fclose(out);
    free(strs);
    free(decls);
    errf(p, "sircc: zasm: zir_main body must be a block node");
    return false;
  }
  JsonValue* stmts = json_obj_get(body->fields, "stmts");
  if (!stmts || stmts->type != JSON_ARRAY) {
    fclose(out);
    free(strs);
    free(decls);
    errf(p, "sircc: zasm: zir_main body block missing stmts array");
    return false;
  }

  for (size_t si = 0; si < stmts->v.arr.len; si++) {
    int64_t sid = 0;
    if (!parse_node_ref_id(stmts->v.arr.items[si], &sid)) {
      fclose(out);
      free(strs);
      free(decls);
      errf(p, "sircc: zasm: block stmt[%zu] must be node ref", si);
      return false;
    }
    NodeRec* s = get_node(p, sid);
    if (!s) {
      fclose(out);
      free(strs);
      free(decls);
      errf(p, "sircc: zasm: unknown stmt node %lld", (long long)sid);
      return false;
    }

    if (strcmp(s->tag, "let") == 0) {
      JsonValue* v = s->fields ? json_obj_get(s->fields, "value") : NULL;
      int64_t vid = 0;
      if (!parse_node_ref_id(v, &vid)) {
        fclose(out);
        free(strs);
        free(decls);
        errf(p, "sircc: zasm: let node %lld missing fields.value ref", (long long)sid);
        return false;
      }
      NodeRec* vn = get_node(p, vid);
      if (!vn) {
        fclose(out);
        free(strs);
        free(decls);
        errf(p, "sircc: zasm: let node %lld value references unknown node", (long long)sid);
        return false;
      }
      if (strcmp(vn->tag, "call") == 0 || strcmp(vn->tag, "call.indirect") == 0) {
        if (!emit_call_stmt(out, p, strs, strs_len, allocas, allocas_len, vid, line++)) {
          fclose(out);
          free(strs);
          free(allocas);
          free(decls);
          return false;
        }
      }
      continue;
    }

    if (strcmp(s->tag, "mem.fill") == 0) {
      if (!emit_mem_fill_stmt(out, p, strs, strs_len, allocas, allocas_len, s, line)) {
        fclose(out);
        free(strs);
        free(allocas);
        free(decls);
        return false;
      }
      line += 4;
      continue;
    }

    if (strcmp(s->tag, "mem.copy") == 0) {
      if (!emit_mem_copy_stmt(out, p, strs, strs_len, allocas, allocas_len, s, line)) {
        fclose(out);
        free(strs);
        free(allocas);
        free(decls);
        return false;
      }
      line += 4;
      continue;
    }

    if (strncmp(s->tag, "store.", 6) == 0) {
      if (!emit_store_stmt(out, p, strs, strs_len, allocas, allocas_len, s, line)) {
        fclose(out);
        free(strs);
        free(allocas);
        free(decls);
        return false;
      }
      line += 2;
      continue;
    }

    if (strcmp(s->tag, "term.ret") == 0 || strcmp(s->tag, "return") == 0) {
      JsonValue* rv = s->fields ? json_obj_get(s->fields, "value") : NULL;
      int64_t rid = 0;
      if (rv && parse_node_ref_id(rv, &rid)) {
        if (!emit_ret_value_to_hl(out, p, strs, strs_len, allocas, allocas_len, rid, line++)) {
          fclose(out);
          free(strs);
          free(allocas);
          free(decls);
          return false;
        }
      } else {
        // No value: default 0 in HL.
        write_ir_k(out, "instr");
        fprintf(out, ",\"m\":\"LD\",\"ops\":[");
        write_op_reg(out, "HL");
        fprintf(out, ",");
        write_op_num(out, 0);
        fprintf(out, "]");
        write_loc(out, line++);
        fprintf(out, "}\n");
      }

      write_ir_k(out, "instr");
      fprintf(out, ",\"m\":\"RET\",\"ops\":[]");
      write_loc(out, line++);
      fprintf(out, "}\n");
      break;
    }

    fclose(out);
    free(strs);
    free(allocas);
    free(decls);
    errf(p, "sircc: zasm: unsupported stmt tag '%s' in zir_main", s->tag);
    return false;
  }

  // Emit STR directives for any cstr nodes in the program.
  if (strs_len) fprintf(out, "\n");
  for (size_t i = 0; i < strs_len; i++) {
    write_ir_k(out, "dir");
    fprintf(out, ",\"d\":\"STR\",\"name\":");
    json_write_escaped(out, strs[i].sym);
    fprintf(out, ",\"args\":[");
    write_op_str(out, strs[i].value);
    fprintf(out, "]");
    write_loc(out, line++);
    fprintf(out, "}\n");
  }

  // Emit RESB directives for any simple alloca.* nodes in the program.
  if (allocas_len) fprintf(out, "\n");
  for (size_t i = 0; i < allocas_len; i++) {
    write_ir_k(out, "dir");
    fprintf(out, ",\"d\":\"RESB\",\"name\":");
    json_write_escaped(out, allocas[i].sym);
    fprintf(out, ",\"args\":[");
    write_op_num(out, allocas[i].size_bytes);
    fprintf(out, "]");
    write_loc(out, line++);
    fprintf(out, "}\n");
  }

  fclose(out);
  free(strs);
  free(allocas);
  free(decls);
  return true;
}

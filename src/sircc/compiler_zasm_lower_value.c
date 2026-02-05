// SPDX-FileCopyrightText: 2026 Frogfish
// SPDX-License-Identifier: GPL-3.0-or-later

#include "compiler_zasm_internal.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

bool zasm_lower_value_to_op(
    SirProgram* p,
    ZasmStr* strs,
    size_t strs_len,
    ZasmAlloca* allocas,
    size_t allocas_len,
    ZasmNameBinding* names,
    size_t names_len,
    ZasmBParamSlot* bps,
    size_t bps_len,
    int64_t node_id,
    ZasmOp* out) {
  if (!p || !out) return false;
  *out = (ZasmOp){0};

  NodeRec* n = get_node(p, node_id);
  if (!n) {
    zasm_err_node_codef(p, node_id, NULL, "sircc.zasm.node.unknown", "sircc: zasm: unknown node id %lld", (long long)node_id);
    return false;
  }

  if (strncmp(n->tag, "const.i", 7) == 0) {
    if (!n->fields) {
      zasm_err_node_codef(p, node_id, n->tag, "sircc.zasm.node.missing_fields", "sircc: zasm: %s node %lld missing fields", n->tag,
                          (long long)node_id);
      return false;
    }
    int64_t v = 0;
    if (!must_i64(p, json_obj_get(n->fields, "value"), &v, "const.value")) return false;
    out->k = ZOP_NUM;
    out->n = v;
    return true;
  }

  if (strncmp(n->tag, "alloca.", 7) == 0) {
    const char* sym = zasm_sym_for_alloca(allocas, allocas_len, node_id);
    if (!sym) {
      zasm_err_node_codef(p, node_id, n->tag, "sircc.zasm.mapping.missing",
                          "sircc: zasm: missing alloca symbol mapping for node %lld", (long long)node_id);
      return false;
    }
    out->k = ZOP_SYM;
    out->s = sym;
    return true;
  }

  if (strcmp(n->tag, "bparam") == 0) {
    for (size_t i = 0; i < bps_len; i++) {
      if (bps[i].node_id == node_id) {
        out->k = ZOP_SLOT;
        out->s = bps[i].sym;
        out->n = bps[i].size_bytes;
        return true;
      }
    }
    zasm_err_node_codef(p, node_id, n->tag, "sircc.zasm.mapping.missing", "sircc: zasm: missing bparam slot mapping for node %lld",
                        (long long)node_id);
    return false;
  }

  if (strcmp(n->tag, "cstr") == 0) {
    const char* sym = zasm_sym_for_str(strs, strs_len, node_id);
    if (!sym) {
      zasm_err_node_codef(p, node_id, n->tag, "sircc.zasm.mapping.missing", "sircc: zasm: missing cstr symbol mapping for node %lld",
                          (long long)node_id);
      return false;
    }
    out->k = ZOP_SYM;
    out->s = sym;
    return true;
  }

  if (strcmp(n->tag, "decl.fn") == 0) {
    const char* name = n->fields ? json_get_string(json_obj_get(n->fields, "name")) : NULL;
    if (!name) {
      zasm_err_node_codef(p, node_id, n->tag, "sircc.zasm.node.missing_field",
                          "sircc: zasm: decl.fn node %lld missing fields.name", (long long)node_id);
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
      zasm_err_node_codef(p, node_id, n->tag, "sircc.zasm.node.missing_field",
                          "sircc: zasm: ptr.sym node %lld missing fields.name", (long long)node_id);
      return false;
    }
    out->k = ZOP_SYM;
    out->s = name;
    return true;
  }

  if (strcmp(n->tag, "ptr.sizeof") == 0 || strcmp(n->tag, "ptr.alignof") == 0) {
    if (!n->fields) {
      zasm_err_node_codef(p, node_id, n->tag, "sircc.zasm.node.missing_fields", "sircc: zasm: %s node %lld missing fields", n->tag,
                          (long long)node_id);
      return false;
    }
    int64_t ty_id = 0;
    if (!parse_type_ref_id(p, json_obj_get(n->fields, "ty"), &ty_id)) {
      zasm_err_node_codef(p, node_id, n->tag, "sircc.zasm.node.missing_field", "sircc: zasm: %s node %lld missing fields.ty (type ref)",
                          n->tag, (long long)node_id);
      return false;
    }
    JsonValue* args = json_obj_get(n->fields, "args");
    if (!args || args->type != JSON_ARRAY || args->v.arr.len != 0) {
      zasm_err_node_codef(p, node_id, n->tag, "sircc.zasm.value.bad_args", "sircc: zasm: %s node %lld requires args:[]", n->tag,
                          (long long)node_id);
      return false;
    }

    int64_t size = 0;
    int64_t align = 0;
    if (!type_size_align(p, ty_id, &size, &align)) {
      zasm_err_node_codef(p, node_id, n->tag, "sircc.zasm.value.unsupported", "sircc: zasm: %s node %lld has invalid/unsized type %lld",
                          n->tag, (long long)node_id, (long long)ty_id);
      return false;
    }
    out->k = ZOP_NUM;
    out->n = (strcmp(n->tag, "ptr.sizeof") == 0) ? size : align;
    return true;
  }

  if (strncmp(n->tag, "i", 1) == 0) {
    // Limited support for integer cast mnemonics when they are constant-foldable.
    // Full arithmetic is handled in statement lowering (let-bound), not as pure value nodes.
    const char* dot = strchr(n->tag, '.');
    if (dot) {
      const char* op = dot + 1;
      if (strncmp(op, "zext.i", 6) == 0 || strncmp(op, "sext.i", 6) == 0 || strncmp(op, "trunc.i", 7) == 0) {
        JsonValue* args = n->fields ? json_obj_get(n->fields, "args") : NULL;
        if (!args || args->type != JSON_ARRAY || args->v.arr.len != 1) {
          zasm_err_node_codef(p, node_id, n->tag, "sircc.zasm.value.bad_args", "sircc: zasm: %s node %lld requires args:[x]", n->tag,
                              (long long)node_id);
          return false;
        }
        int64_t x_id = 0;
        if (!parse_node_ref_id(p, args->v.arr.items[0], &x_id)) {
          zasm_err_node_codef(p, node_id, n->tag, "sircc.zasm.value.bad_args", "sircc: zasm: %s node %lld arg must be node ref", n->tag,
                              (long long)node_id);
          return false;
        }
        ZasmOp x = {0};
        if (!zasm_lower_value_to_op(p, strs, strs_len, allocas, allocas_len, names, names_len, bps, bps_len, x_id, &x)) return false;
        if (x.k != ZOP_NUM) {
          zasm_err_node_codef(p, node_id, n->tag, "sircc.zasm.value.unsupported", "sircc: zasm: %s requires constant-foldable operand",
                              n->tag);
          return false;
        }

        int dst = 0;
        int src = 0;
        char dbuf[8] = {0};
        size_t dlen = (size_t)(dot - n->tag);
        if (dlen >= sizeof(dbuf)) dlen = sizeof(dbuf) - 1;
        memcpy(dbuf, n->tag, dlen);
        if (sscanf(dbuf, "i%d", &dst) != 1) dst = 0;
        const char* num = (strncmp(op, "trunc.i", 7) == 0) ? (op + 7) : (op + 6);
        if (sscanf(num, "%d", &src) != 1) src = 0;
        if (!(dst == 8 || dst == 16 || dst == 32 || dst == 64) || !(src == 8 || src == 16 || src == 32 || src == 64)) {
          zasm_err_node_codef(p, node_id, n->tag, "sircc.zasm.value.unsupported", "sircc: zasm: unsupported cast width in %s", n->tag);
          return false;
        }

        // Mask/truncate to src width first.
        uint64_t u = (uint64_t)x.n;
        if (src < 64) u &= ((1ULL << src) - 1ULL);

        bool is_sext = strncmp(op, "sext.i", 6) == 0;
        if (is_sext && src < 64) {
          uint64_t sign = 1ULL << (src - 1);
          if (u & sign) u |= (~0ULL) << src;
        }

        // Truncate to dst.
        if (dst < 64) u &= ((1ULL << dst) - 1ULL);

        out->k = ZOP_NUM;
        out->n = (int64_t)u;
        return true;
      }
    }
  }

  if (strcmp(n->tag, "ptr.to_i64") == 0) {
    JsonValue* args = n->fields ? json_obj_get(n->fields, "args") : NULL;
    if (!args || args->type != JSON_ARRAY || args->v.arr.len != 1) {
      zasm_err_node_codef(p, node_id, n->tag, "sircc.zasm.value.bad_args",
                          "sircc: zasm: ptr.to_i64 node %lld requires args:[x]", (long long)node_id);
      return false;
    }
    int64_t x_id = 0;
    if (!parse_node_ref_id(p, args->v.arr.items[0], &x_id)) {
      zasm_err_node_codef(p, node_id, n->tag, "sircc.zasm.value.bad_args",
                          "sircc: zasm: ptr.to_i64 node %lld arg must be node ref", (long long)node_id);
      return false;
    }
    return zasm_lower_value_to_op(p, strs, strs_len, allocas, allocas_len, names, names_len, bps, bps_len, x_id, out);
  }

  if (strcmp(n->tag, "name") == 0) {
    const char* name = n->fields ? json_get_string(json_obj_get(n->fields, "name")) : NULL;
    if (!name) {
      zasm_err_node_codef(p, node_id, n->tag, "sircc.zasm.node.missing_field", "sircc: zasm: name node %lld missing fields.name",
                          (long long)node_id);
      return false;
    }
    for (size_t i = 0; i < names_len; i++) {
      if (names[i].name && strcmp(names[i].name, name) == 0) {
        if (names[i].is_slot) {
          out->k = ZOP_SLOT;
          out->s = names[i].op.s;
          out->n = names[i].slot_size_bytes;
          return true;
        }
        *out = names[i].op;
        return true;
      }
    }
    zasm_err_node_codef(p, node_id, n->tag, "sircc.zasm.name.unknown", "sircc: zasm: unknown name '%s' (node %lld)", name,
                        (long long)node_id);
    return false;
  }

  zasm_err_node_codef(p, node_id, n->tag, "sircc.zasm.value.unsupported", "sircc: zasm: unsupported value node '%s' (node %lld)", n->tag,
                      (long long)node_id);
  return false;
}

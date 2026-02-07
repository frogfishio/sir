// SPDX-FileCopyrightText: 2026 Frogfish
// SPDX-License-Identifier: GPL-3.0-or-later

#include "compiler_lower_hl.h"

#include "compiler_internal.h"

#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

static void json_write_value(FILE* out, const JsonValue* v);

static void json_write_object(FILE* out, const JsonObject* obj) {
  fputc('{', out);
  if (obj) {
    for (size_t i = 0; i < obj->len; i++) {
      if (i) fputc(',', out);
      json_write_escaped(out, obj->items[i].key ? obj->items[i].key : "");
      fputc(':', out);
      json_write_value(out, obj->items[i].value);
    }
  }
  fputc('}', out);
}

static void json_write_array(FILE* out, const JsonArray* arr) {
  fputc('[', out);
  if (arr) {
    for (size_t i = 0; i < arr->len; i++) {
      if (i) fputc(',', out);
      json_write_value(out, arr->items[i]);
    }
  }
  fputc(']', out);
}

static void json_write_value(FILE* out, const JsonValue* v) {
  if (!out) return;
  if (!v) {
    fputs("null", out);
    return;
  }
  switch (v->type) {
    case JSON_NULL:
      fputs("null", out);
      return;
    case JSON_BOOL:
      fputs(v->v.b ? "true" : "false", out);
      return;
    case JSON_NUMBER:
      fprintf(out, "%lld", (long long)v->v.i);
      return;
    case JSON_STRING:
      json_write_escaped(out, v->v.s ? v->v.s : "");
      return;
    case JSON_ARRAY:
      json_write_array(out, &v->v.arr);
      return;
    case JSON_OBJECT:
      json_write_object(out, &v->v.obj);
      return;
    default:
      fputs("null", out);
      return;
  }
}

// Future: preserve original string ids by reverse-mapping through SirIdMap.

static JsonValue* jv_make(Arena* a, JsonType t) {
  JsonValue* v = (JsonValue*)arena_alloc(a, sizeof(*v));
  if (!v) return NULL;
  memset(v, 0, sizeof(*v));
  v->type = t;
  return v;
}

static JsonValue* jv_make_obj(Arena* a, size_t n) {
  JsonValue* v = jv_make(a, JSON_OBJECT);
  if (!v) return NULL;
  JsonObjectItem* items = NULL;
  if (n) {
    items = (JsonObjectItem*)arena_alloc(a, n * sizeof(*items));
    if (!items) return NULL;
    memset(items, 0, n * sizeof(*items));
  }
  v->v.obj.items = items;
  v->v.obj.len = n;
  return v;
}

static JsonValue* jv_make_arr(Arena* a, size_t n) {
  JsonValue* v = jv_make(a, JSON_ARRAY);
  if (!v) return NULL;
  JsonValue** items = NULL;
  if (n) {
    items = (JsonValue**)arena_alloc(a, n * sizeof(*items));
    if (!items) return NULL;
    memset(items, 0, n * sizeof(*items));
  }
  v->v.arr.items = items;
  v->v.arr.len = n;
  return v;
}

static bool lower_sem_if_to_select(SirProgram* p, NodeRec* n) {
  if (!p || !n || !n->fields) return false;
  JsonValue* args = json_obj_get(n->fields, "args");
  if (!args || args->type != JSON_ARRAY || args->v.arr.len != 3) return false;

  JsonValue* cond_ref = args->v.arr.items[0];
  JsonValue* br_then = args->v.arr.items[1];
  JsonValue* br_else = args->v.arr.items[2];
  if (!cond_ref || !br_then || !br_else) return false;
  if (br_then->type != JSON_OBJECT || br_else->type != JSON_OBJECT) return false;

  const char* k_then = json_get_string(json_obj_get(br_then, "kind"));
  const char* k_else = json_get_string(json_obj_get(br_else, "kind"));
  if (!k_then || !k_else) return false;

  if (strcmp(k_then, "val") != 0 || strcmp(k_else, "val") != 0) {
    SIRCC_ERR_NODE(p, n, "sircc.lower_hl.sem.if.thunk_unsupported",
                   "sircc: --lower-hl currently supports sem.if only when both branches are kind:'val'");
    return false;
  }

  JsonValue* v_then = json_obj_get(br_then, "v");
  JsonValue* v_else = json_obj_get(br_else, "v");
  if (!v_then || !v_else) return false;

  JsonValue* new_args = jv_make_arr(&p->arena, 3);
  if (!new_args) {
    bump_exit_code(p, SIRCC_EXIT_INTERNAL);
    SIRCC_ERR_NODE(p, n, "sircc.oom", "sircc: out of memory");
    return false;
  }
  new_args->v.arr.items[0] = cond_ref;
  new_args->v.arr.items[1] = v_then;
  new_args->v.arr.items[2] = v_else;

  JsonValue* new_fields = jv_make_obj(&p->arena, 1);
  if (!new_fields) {
    bump_exit_code(p, SIRCC_EXIT_INTERNAL);
    SIRCC_ERR_NODE(p, n, "sircc.oom", "sircc: out of memory");
    return false;
  }
  new_fields->v.obj.items[0].key = "args";
  new_fields->v.obj.items[0].value = new_args;

  n->tag = "select";
  n->fields = new_fields;
  return true;
}

static bool lower_sem_sc_to_bool_bin(SirProgram* p, NodeRec* n, bool is_and) {
  if (!p || !n || !n->fields) return false;
  JsonValue* args = json_obj_get(n->fields, "args");
  if (!args || args->type != JSON_ARRAY || args->v.arr.len != 2) return false;

  JsonValue* lhs_ref = args->v.arr.items[0];
  JsonValue* rhs_branch = args->v.arr.items[1];
  if (!lhs_ref || !rhs_branch) return false;
  if (rhs_branch->type != JSON_OBJECT) return false;

  const char* k_rhs = json_get_string(json_obj_get(rhs_branch, "kind"));
  if (!k_rhs) return false;
  if (strcmp(k_rhs, "val") != 0) {
    SIRCC_ERR_NODE(p, n, "sircc.lower_hl.sem.sc.thunk_unsupported",
                   "sircc: --lower-hl currently supports %s only when rhs is kind:'val'", is_and ? "sem.and_sc" : "sem.or_sc");
    return false;
  }

  JsonValue* v_rhs = json_obj_get(rhs_branch, "v");
  if (!v_rhs) return false;

  JsonValue* new_args = jv_make_arr(&p->arena, 2);
  if (!new_args) {
    bump_exit_code(p, SIRCC_EXIT_INTERNAL);
    SIRCC_ERR_NODE(p, n, "sircc.oom", "sircc: out of memory");
    return false;
  }
  new_args->v.arr.items[0] = lhs_ref;
  new_args->v.arr.items[1] = v_rhs;

  JsonValue* new_fields = jv_make_obj(&p->arena, 1);
  if (!new_fields) {
    bump_exit_code(p, SIRCC_EXIT_INTERNAL);
    SIRCC_ERR_NODE(p, n, "sircc.oom", "sircc: out of memory");
    return false;
  }
  new_fields->v.obj.items[0].key = "args";
  new_fields->v.obj.items[0].value = new_args;

  n->tag = is_and ? "bool.and" : "bool.or";
  n->fields = new_fields;
  return true;
}

static bool lower_sem_nodes(SirProgram* p) {
  if (!p) return false;
  if (!p->feat_sem_v1) return true;

  for (size_t i = 0; i < p->nodes_cap; i++) {
    NodeRec* n = p->nodes ? p->nodes[i] : NULL;
    if (!n || !n->tag) continue;
    if (strncmp(n->tag, "sem.", 4) != 0) continue;

    if (strcmp(n->tag, "sem.if") == 0) {
      if (!lower_sem_if_to_select(p, n)) return false;
      continue;
    }
    if (strcmp(n->tag, "sem.and_sc") == 0) {
      if (!lower_sem_sc_to_bool_bin(p, n, true)) return false;
      continue;
    }
    if (strcmp(n->tag, "sem.or_sc") == 0) {
      if (!lower_sem_sc_to_bool_bin(p, n, false)) return false;
      continue;
    }

    // Everything else requires real CFG desugaring in the legalizer.
    SIRCC_ERR_NODE(p, n, "sircc.lower_hl.sem.unsupported", "sircc: --lower-hl does not support lowering %s yet", n->tag);
    return false;
  }

  return true;
}

static const char* type_kind_str(TypeKind k) {
  switch (k) {
    case TYPE_PRIM:
      return "prim";
    case TYPE_PTR:
      return "ptr";
    case TYPE_ARRAY:
      return "array";
    case TYPE_FN:
      return "fn";
    case TYPE_STRUCT:
      return "struct";
    case TYPE_VEC:
      return "vec";
    case TYPE_FUN:
      return "fun";
    case TYPE_CLOSURE:
      return "closure";
    case TYPE_SUM:
      return "sum";
    default:
      return NULL;
  }
}

static void emit_features(FILE* out, const SirProgram* p) {
  bool first = true;
  fputc('[', out);
  if (p->feat_atomics_v1) {
    if (!first) fputc(',', out);
    json_write_escaped(out, "atomics:v1");
    first = false;
  }
  if (p->feat_simd_v1) {
    if (!first) fputc(',', out);
    json_write_escaped(out, "simd:v1");
    first = false;
  }
  if (p->feat_adt_v1) {
    if (!first) fputc(',', out);
    json_write_escaped(out, "adt:v1");
    first = false;
  }
  if (p->feat_fun_v1) {
    if (!first) fputc(',', out);
    json_write_escaped(out, "fun:v1");
    first = false;
  }
  if (p->feat_closure_v1) {
    if (!first) fputc(',', out);
    json_write_escaped(out, "closure:v1");
    first = false;
  }
  if (p->feat_coro_v1) {
    if (!first) fputc(',', out);
    json_write_escaped(out, "coro:v1");
    first = false;
  }
  if (p->feat_eh_v1) {
    if (!first) fputc(',', out);
    json_write_escaped(out, "eh:v1");
    first = false;
  }
  if (p->feat_gc_v1) {
    if (!first) fputc(',', out);
    json_write_escaped(out, "gc:v1");
    first = false;
  }
  if (p->feat_sem_v1) {
    if (!first) fputc(',', out);
    json_write_escaped(out, "sem:v1");
    first = false;
  }
  fputc(']', out);
}

static bool emit_meta(FILE* out, const SirProgram* p) {
  if (!out || !p) return false;
  fputs("{\"ir\":\"sir-v1.0\",\"k\":\"meta\",\"producer\":\"sircc-lower-hl\"", out);
  if (p->unit_name) {
    fputs(",\"unit\":", out);
    json_write_escaped(out, p->unit_name);
  }
  fputs(",\"ext\":{", out);

  fputs("\"features\":", out);
  emit_features(out, p);

  if (p->target_triple || p->target_cpu || p->target_features) {
    fputs(",\"target\":{", out);
    bool first = true;
    if (p->target_triple) {
      if (!first) fputc(',', out);
      fputs("\"triple\":", out);
      json_write_escaped(out, p->target_triple);
      first = false;
    }
    if (p->target_cpu) {
      if (!first) fputc(',', out);
      fputs("\"cpu\":", out);
      json_write_escaped(out, p->target_cpu);
      first = false;
    }
    if (p->target_features) {
      if (!first) fputc(',', out);
      fputs("\"features\":", out);
      json_write_escaped(out, p->target_features);
      first = false;
    }
    fputs("}", out);
  }

  fputs("}}\n", out);
  return true;
}

static bool emit_types(FILE* out, SirProgram* p) {
  if (!out || !p) return false;
  for (size_t i = 0; i < p->types_cap; i++) {
    TypeRec* t = p->types ? p->types[i] : NULL;
    if (!t) continue;
    const char* k = type_kind_str(t->kind);
    if (!k) continue;

    fputs("{\"ir\":\"sir-v1.0\",\"k\":\"type\",\"id\":", out);
    fprintf(out, "%lld", (long long)t->id);
    fputs(",\"kind\":", out);
    json_write_escaped(out, k);

    if (t->kind == TYPE_PRIM) {
      fputs(",\"prim\":", out);
      json_write_escaped(out, t->prim ? t->prim : "");
    } else if (t->kind == TYPE_PTR) {
      fputs(",\"of\":", out);
      fprintf(out, "%lld", (long long)t->of);
    } else if (t->kind == TYPE_ARRAY) {
      fputs(",\"of\":", out);
      fprintf(out, "%lld", (long long)t->of);
      fputs(",\"len\":", out);
      fprintf(out, "%lld", (long long)t->len);
    } else if (t->kind == TYPE_FN) {
      fputs(",\"params\":[", out);
      for (size_t pi = 0; pi < t->param_len; pi++) {
        if (pi) fputc(',', out);
        fprintf(out, "%lld", (long long)t->params[pi]);
      }
      fputs("],\"ret\":", out);
      fprintf(out, "%lld", (long long)t->ret);
      if (t->varargs) fputs(",\"varargs\":true", out);
    } else if (t->kind == TYPE_STRUCT) {
      fputs(",\"fields\":[", out);
      for (size_t fi = 0; fi < t->field_len; fi++) {
        if (fi) fputc(',', out);
        fputs("{\"name\":", out);
        json_write_escaped(out, t->fields[fi].name ? t->fields[fi].name : "");
        fputs(",\"type_ref\":", out);
        fprintf(out, "%lld", (long long)t->fields[fi].type_ref);
        fputs("}", out);
      }
      fputs("]", out);
    } else if (t->kind == TYPE_VEC) {
      fputs(",\"lane\":", out);
      fprintf(out, "%lld", (long long)t->lane_ty);
      fputs(",\"lanes\":", out);
      fprintf(out, "%lld", (long long)t->lanes);
    } else if (t->kind == TYPE_FUN) {
      fputs(",\"sig\":", out);
      fprintf(out, "%lld", (long long)t->sig);
    } else if (t->kind == TYPE_CLOSURE) {
      fputs(",\"call_sig\":", out);
      fprintf(out, "%lld", (long long)t->call_sig);
      fputs(",\"env_ty\":", out);
      fprintf(out, "%lld", (long long)t->env_ty);
    } else if (t->kind == TYPE_SUM) {
      fputs(",\"variants\":[", out);
      for (size_t vi = 0; vi < t->variant_len; vi++) {
        if (vi) fputc(',', out);
        fputs("{", out);
        bool first = true;
        if (t->variants[vi].name) {
          fputs("\"name\":", out);
          json_write_escaped(out, t->variants[vi].name);
          first = false;
        }
        if (t->variants[vi].ty) {
          if (!first) fputc(',', out);
          fputs("\"ty\":", out);
          fprintf(out, "%lld", (long long)t->variants[vi].ty);
        }
        fputs("}", out);
      }
      fputs("]", out);
    }

    fputs("}\n", out);
  }
  return true;
}

static bool emit_syms(FILE* out, SirProgram* p) {
  if (!out || !p) return false;
  for (size_t i = 0; i < p->syms_cap; i++) {
    SymRec* s = p->syms ? p->syms[i] : NULL;
    if (!s) continue;
    fputs("{\"ir\":\"sir-v1.0\",\"k\":\"sym\",\"id\":", out);
    fprintf(out, "%lld", (long long)s->id);
    if (s->name) {
      fputs(",\"name\":", out);
      json_write_escaped(out, s->name);
    }
    if (s->kind) {
      fputs(",\"kind\":", out);
      json_write_escaped(out, s->kind);
    }
    if (s->linkage) {
      fputs(",\"linkage\":", out);
      json_write_escaped(out, s->linkage);
    }
    if (s->type_ref) {
      fputs(",\"type_ref\":", out);
      fprintf(out, "%lld", (long long)s->type_ref);
    }
    if (s->value) {
      fputs(",\"value\":", out);
      json_write_value(out, s->value);
    }
    fputs("}\n", out);
  }
  return true;
}

static bool emit_nodes(FILE* out, SirProgram* p) {
  if (!out || !p) return false;
  for (size_t i = 0; i < p->nodes_cap; i++) {
    NodeRec* n = p->nodes ? p->nodes[i] : NULL;
    if (!n || !n->tag) continue;
    fputs("{\"ir\":\"sir-v1.0\",\"k\":\"node\",\"id\":", out);
    fprintf(out, "%lld", (long long)n->id);
    fputs(",\"tag\":", out);
    json_write_escaped(out, n->tag);
    if (n->type_ref) {
      fputs(",\"type_ref\":", out);
      fprintf(out, "%lld", (long long)n->type_ref);
    }
    if (n->fields) {
      fputs(",\"fields\":", out);
      json_write_value(out, n->fields);
    }
    fputs("}\n", out);
  }
  return true;
}

bool lower_hl_and_emit_sir_core(SirProgram* p, const char* out_path) {
  if (!p || !out_path || !out_path[0]) return false;

  if (!lower_sem_nodes(p)) return false;

  FILE* out = fopen(out_path, "wb");
  if (!out) {
    err_codef(p, "sircc.io.open_failed", "sircc: failed to open --emit-sir-core output: %s", strerror(errno));
    return false;
  }

  bool ok = true;
  ok = ok && emit_meta(out, p);
  ok = ok && emit_types(out, p);
  ok = ok && emit_syms(out, p);
  ok = ok && emit_nodes(out, p);
  fclose(out);
  return ok;
}

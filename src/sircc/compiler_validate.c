// SPDX-FileCopyrightText: 2026 Frogfish
// SPDX-License-Identifier: GPL-3.0-or-later

#include "compiler_internal.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static bool validate_cfg_fn(SirProgram* p, NodeRec* fn);

bool validate_program(SirProgram* p) {
  // Validate CFG-form functions even under --verify-only.
  for (size_t i = 0; i < p->nodes_cap; i++) {
    NodeRec* n = p->nodes[i];
    if (!n) continue;
    if (strcmp(n->tag, "fn") != 0) continue;
    if (!n->fields) continue;
    JsonValue* blocks = json_obj_get(n->fields, "blocks");
    JsonValue* entry = json_obj_get(n->fields, "entry");
    if (blocks || entry) {
      if (!validate_cfg_fn(p, n)) return false;
    }
  }

  // Feature gates for node-based streams (meta.ext.features can appear anywhere, so do this post-parse).
  if (p->feat_closure_v1 && !p->feat_fun_v1) {
    err_codef(p, "sircc.feature.dep", "sircc: feature closure:v1 requires fun:v1");
    return false;
  }

  for (size_t i = 0; i < p->types_cap; i++) {
    TypeRec* t = p->types[i];
    if (!t) continue;
    if (t->kind == TYPE_VEC && !p->feat_simd_v1) {
      SirDiagSaved saved = sir_diag_push(p, "type", t->id, "vec");
      err_codef(p, "sircc.feature.gate", "sircc: type kind 'vec' requires feature simd:v1 (enable via meta.ext.features)");
      sir_diag_pop(p, saved);
      return false;
    }
    if (t->kind == TYPE_FUN && !p->feat_fun_v1) {
      SirDiagSaved saved = sir_diag_push(p, "type", t->id, "fun");
      err_codef(p, "sircc.feature.gate", "sircc: type kind 'fun' requires feature fun:v1 (enable via meta.ext.features)");
      sir_diag_pop(p, saved);
      return false;
    }
    if (t->kind == TYPE_CLOSURE && !p->feat_closure_v1) {
      SirDiagSaved saved = sir_diag_push(p, "type", t->id, "closure");
      err_codef(p, "sircc.feature.gate", "sircc: type kind 'closure' requires feature closure:v1 (enable via meta.ext.features)");
      sir_diag_pop(p, saved);
      return false;
    }
    if (t->kind == TYPE_SUM && !p->feat_adt_v1) {
      SirDiagSaved saved = sir_diag_push(p, "type", t->id, "sum");
      err_codef(p, "sircc.feature.gate", "sircc: type kind 'sum' requires feature adt:v1 (enable via meta.ext.features)");
      sir_diag_pop(p, saved);
      return false;
    }

    if (t->kind == TYPE_VEC) {
      TypeRec* lane = get_type(p, t->lane_ty);
      if (!lane || lane->kind != TYPE_PRIM || !lane->prim) {
        SirDiagSaved saved = sir_diag_push(p, "type", t->id, "vec");
        err_codef(p, "sircc.type.vec.lane.bad", "sircc: type.vec lane must reference a primitive lane type");
        sir_diag_pop(p, saved);
        return false;
      }
      const char* lp = lane->prim;
      bool ok = (strcmp(lp, "i8") == 0 || strcmp(lp, "i16") == 0 || strcmp(lp, "i32") == 0 || strcmp(lp, "i64") == 0 ||
                 strcmp(lp, "f32") == 0 || strcmp(lp, "f64") == 0 || strcmp(lp, "bool") == 0 || strcmp(lp, "i1") == 0);
      if (!ok) {
        SirDiagSaved saved = sir_diag_push(p, "type", t->id, "vec");
        err_codef(p, "sircc.type.vec.lane.unsupported", "sircc: type.vec lane must be one of i8/i16/i32/i64/f32/f64/bool");
        sir_diag_pop(p, saved);
        return false;
      }
      if (t->lanes <= 0) {
        SirDiagSaved saved = sir_diag_push(p, "type", t->id, "vec");
        err_codef(p, "sircc.type.vec.lanes.bad", "sircc: type.vec lanes must be > 0");
        sir_diag_pop(p, saved);
        return false;
      }
    }
  }

  for (size_t i = 0; i < p->nodes_cap; i++) {
    NodeRec* n = p->nodes[i];
    if (!n) continue;
    if ((strncmp(n->tag, "vec.", 4) == 0 || strcmp(n->tag, "load.vec") == 0 || strcmp(n->tag, "store.vec") == 0) && !p->feat_simd_v1) {
      SirDiagSaved saved = sir_diag_push_node(p, n);
      err_codef(p, "sircc.feature.gate", "sircc: mnemonic '%s' requires feature simd:v1 (enable via meta.ext.features)", n->tag);
      sir_diag_pop(p, saved);
      return false;
    }
    if ((strcmp(n->tag, "call.fun") == 0 || strncmp(n->tag, "fun.", 4) == 0) && !p->feat_fun_v1) {
      SirDiagSaved saved = sir_diag_push_node(p, n);
      err_codef(p, "sircc.feature.gate", "sircc: mnemonic '%s' requires feature fun:v1 (enable via meta.ext.features)", n->tag);
      sir_diag_pop(p, saved);
      return false;
    }
    if ((strcmp(n->tag, "call.closure") == 0 || strncmp(n->tag, "closure.", 8) == 0) && !p->feat_closure_v1) {
      SirDiagSaved saved = sir_diag_push_node(p, n);
      err_codef(p, "sircc.feature.gate", "sircc: mnemonic '%s' requires feature closure:v1 (enable via meta.ext.features)", n->tag);
      sir_diag_pop(p, saved);
      return false;
    }
    if ((strncmp(n->tag, "adt.", 4) == 0) && !p->feat_adt_v1) {
      SirDiagSaved saved = sir_diag_push_node(p, n);
      err_codef(p, "sircc.feature.gate", "sircc: mnemonic '%s' requires feature adt:v1 (enable via meta.ext.features)", n->tag);
      sir_diag_pop(p, saved);
      return false;
    }
    if ((strncmp(n->tag, "sem.", 4) == 0) && !p->feat_sem_v1) {
      SirDiagSaved saved = sir_diag_push_node(p, n);
      err_codef(p, "sircc.feature.gate", "sircc: mnemonic '%s' requires feature sem:v1 (enable via meta.ext.features)", n->tag);
      sir_diag_pop(p, saved);
      return false;
    }
    if (strcmp(n->tag, "sem.match_sum") == 0 && p->feat_sem_v1 && !p->feat_adt_v1) {
      SirDiagSaved saved = sir_diag_push_node(p, n);
      err_codef(p, "sircc.feature.dep", "sircc: sem.match_sum requires adt:v1");
      sir_diag_pop(p, saved);
      return false;
    }
  }

  return true;
}

static size_t block_param_count(SirProgram* p, int64_t block_id) {
  NodeRec* b = get_node(p, block_id);
  if (!b || strcmp(b->tag, "block") != 0 || !b->fields) return 0;
  JsonValue* params = json_obj_get(b->fields, "params");
  if (!params) return 0;
  if (params->type != JSON_ARRAY) return (size_t)-1;
  return params->v.arr.len;
}

static bool validate_block_params(SirProgram* p, int64_t block_id) {
  NodeRec* b = get_node(p, block_id);
  if (!b || strcmp(b->tag, "block") != 0) {
    SirDiagSaved saved = sir_diag_push(p, "node", block_id, b ? b->tag : NULL);
    err_codef(p, "sircc.cfg.block.not_block", "sircc: block ref %lld is not a block node", (long long)block_id);
    sir_diag_pop(p, saved);
    return false;
  }
  JsonValue* params = b->fields ? json_obj_get(b->fields, "params") : NULL;
  if (!params) return true;
  if (params->type != JSON_ARRAY) {
    SirDiagSaved saved = sir_diag_push_node(p, b);
    err_codef(p, "sircc.cfg.block.params.not_array", "sircc: block %lld params must be an array", (long long)block_id);
    sir_diag_pop(p, saved);
    return false;
  }
  for (size_t i = 0; i < params->v.arr.len; i++) {
    int64_t pid = 0;
    if (!parse_node_ref_id(p, params->v.arr.items[i], &pid)) {
      SirDiagSaved saved = sir_diag_push_node(p, b);
      err_codef(p, "sircc.cfg.block.param.not_ref", "sircc: block %lld params[%zu] must be node refs", (long long)block_id, i);
      sir_diag_pop(p, saved);
      return false;
    }
    NodeRec* pn = get_node(p, pid);
    if (!pn || strcmp(pn->tag, "bparam") != 0) {
      SirDiagSaved saved = sir_diag_push_node(p, b);
      err_codef(p, "sircc.cfg.block.param.not_bparam", "sircc: block %lld params[%zu] must reference bparam nodes", (long long)block_id,
                i);
      sir_diag_pop(p, saved);
      return false;
    }
    if (pn->type_ref == 0) {
      SirDiagSaved saved = sir_diag_push_node(p, pn);
      err_codef(p, "sircc.cfg.bparam.missing_type", "sircc: bparam node %lld missing type_ref", (long long)pid);
      sir_diag_pop(p, saved);
      return false;
    }
  }
  return true;
}

static bool validate_branch_args(SirProgram* p, int64_t to_block_id, JsonValue* args) {
  size_t pc = block_param_count(p, to_block_id);
  if (pc == (size_t)-1) {
    NodeRec* b = get_node(p, to_block_id);
    SirDiagSaved saved = sir_diag_push(p, "node", to_block_id, b ? b->tag : NULL);
    err_codef(p, "sircc.cfg.block.params.not_array", "sircc: block %lld params must be an array", (long long)to_block_id);
    sir_diag_pop(p, saved);
    return false;
  }
  size_t ac = 0;
  if (args) {
    if (args->type != JSON_ARRAY) {
      err_codef(p, "sircc.cfg.branch.args.not_array", "sircc: branch args must be an array");
      return false;
    }
    ac = args->v.arr.len;
  }
  if (pc != ac) {
    NodeRec* b = get_node(p, to_block_id);
    SirDiagSaved saved = sir_diag_push(p, "node", to_block_id, b ? b->tag : NULL);
    err_codef(p, "sircc.cfg.branch.args.count_mismatch", "sircc: block %lld param/arg count mismatch (params=%zu, args=%zu)",
              (long long)to_block_id, pc, ac);
    sir_diag_pop(p, saved);
    return false;
  }
  for (size_t i = 0; i < ac; i++) {
    int64_t aid = 0;
    if (!parse_node_ref_id(p, args->v.arr.items[i], &aid)) {
      err_codef(p, "sircc.cfg.branch.arg.not_ref", "sircc: branch args[%zu] must be node refs", i);
      return false;
    }
    if (!get_node(p, aid)) {
      err_codef(p, "sircc.cfg.branch.arg.unknown_node", "sircc: branch args[%zu] references unknown node %lld", i, (long long)aid);
      return false;
    }
  }
  return true;
}

static bool validate_terminator(SirProgram* p, int64_t term_id) {
  NodeRec* t = get_node(p, term_id);
  if (!t) {
    SirDiagSaved saved = sir_diag_push(p, "node", term_id, NULL);
    err_codef(p, "sircc.cfg.term.unknown", "sircc: block terminator references unknown node %lld", (long long)term_id);
    sir_diag_pop(p, saved);
    return false;
  }
  SirDiagSaved saved = sir_diag_push_node(p, t);
  if (strncmp(t->tag, "term.", 5) != 0 && strcmp(t->tag, "return") != 0) {
    err_codef(p, "sircc.cfg.term.not_terminator", "sircc: block must end with a terminator (got '%s')", t->tag);
    sir_diag_pop(p, saved);
    return false;
  }

  if (strcmp(t->tag, "term.br") == 0) {
    if (!t->fields) {
      err_codef(p, "sircc.cfg.term.missing_fields", "sircc: term.br missing fields");
      sir_diag_pop(p, saved);
      return false;
    }
    int64_t to_id = 0;
    if (!parse_node_ref_id(p, json_obj_get(t->fields, "to"), &to_id)) {
      err_codef(p, "sircc.cfg.term.br.missing_to", "sircc: term.br missing to ref");
      sir_diag_pop(p, saved);
      return false;
    }
    if (!validate_block_params(p, to_id)) {
      sir_diag_pop(p, saved);
      return false;
    }
    bool ok = validate_branch_args(p, to_id, json_obj_get(t->fields, "args"));
    sir_diag_pop(p, saved);
    return ok;
  }

  if (strcmp(t->tag, "term.cbr") == 0 || strcmp(t->tag, "term.condbr") == 0) {
    if (!t->fields) {
      err_codef(p, "sircc.cfg.term.missing_fields", "sircc: %s missing fields", t->tag);
      sir_diag_pop(p, saved);
      return false;
    }
    int64_t cond_id = 0;
    if (!parse_node_ref_id(p, json_obj_get(t->fields, "cond"), &cond_id)) {
      err_codef(p, "sircc.cfg.term.cbr.missing_cond", "sircc: %s missing cond ref", t->tag);
      sir_diag_pop(p, saved);
      return false;
    }
    if (!get_node(p, cond_id)) {
      err_codef(p, "sircc.cfg.term.cbr.cond.unknown_node", "sircc: %s cond references unknown node %lld", t->tag, (long long)cond_id);
      sir_diag_pop(p, saved);
      return false;
    }
    JsonValue* thenb = json_obj_get(t->fields, "then");
    JsonValue* elseb = json_obj_get(t->fields, "else");
    if (!thenb || thenb->type != JSON_OBJECT || !elseb || elseb->type != JSON_OBJECT) {
      err_codef(p, "sircc.cfg.term.cbr.missing_branches", "sircc: %s requires then/else objects", t->tag);
      sir_diag_pop(p, saved);
      return false;
    }
    int64_t then_id = 0, else_id = 0;
    if (!parse_node_ref_id(p, json_obj_get(thenb, "to"), &then_id) || !parse_node_ref_id(p, json_obj_get(elseb, "to"), &else_id)) {
      err_codef(p, "sircc.cfg.term.cbr.missing_to", "sircc: %s then/else missing to ref", t->tag);
      sir_diag_pop(p, saved);
      return false;
    }
    if (!validate_block_params(p, then_id) || !validate_block_params(p, else_id)) {
      sir_diag_pop(p, saved);
      return false;
    }
    if (!validate_branch_args(p, then_id, json_obj_get(thenb, "args"))) {
      sir_diag_pop(p, saved);
      return false;
    }
    if (!validate_branch_args(p, else_id, json_obj_get(elseb, "args"))) {
      sir_diag_pop(p, saved);
      return false;
    }
    sir_diag_pop(p, saved);
    return true;
  }

  if (strcmp(t->tag, "term.switch") == 0) {
    if (!t->fields) {
      err_codef(p, "sircc.cfg.term.missing_fields", "sircc: term.switch missing fields");
      sir_diag_pop(p, saved);
      return false;
    }
    int64_t scrut_id = 0;
    if (!parse_node_ref_id(p, json_obj_get(t->fields, "scrut"), &scrut_id)) {
      err_codef(p, "sircc.cfg.term.switch.missing_scrut", "sircc: term.switch missing scrut ref");
      sir_diag_pop(p, saved);
      return false;
    }
    if (!get_node(p, scrut_id)) {
      err_codef(p, "sircc.cfg.term.switch.scrut.unknown_node", "sircc: term.switch scrut references unknown node %lld",
                (long long)scrut_id);
      sir_diag_pop(p, saved);
      return false;
    }
    JsonValue* def = json_obj_get(t->fields, "default");
    if (!def || def->type != JSON_OBJECT) {
      err_codef(p, "sircc.cfg.term.switch.missing_default", "sircc: term.switch missing default branch");
      sir_diag_pop(p, saved);
      return false;
    }
    int64_t def_id = 0;
    if (!parse_node_ref_id(p, json_obj_get(def, "to"), &def_id)) {
      err_codef(p, "sircc.cfg.term.switch.default.missing_to", "sircc: term.switch default missing to ref");
      sir_diag_pop(p, saved);
      return false;
    }
    if (!validate_block_params(p, def_id)) {
      sir_diag_pop(p, saved);
      return false;
    }
    if (!validate_branch_args(p, def_id, json_obj_get(def, "args"))) {
      sir_diag_pop(p, saved);
      return false;
    }
    JsonValue* cases = json_obj_get(t->fields, "cases");
    if (!cases || cases->type != JSON_ARRAY) {
      err_codef(p, "sircc.cfg.term.switch.cases.not_array", "sircc: term.switch missing cases array");
      sir_diag_pop(p, saved);
      return false;
    }
    for (size_t i = 0; i < cases->v.arr.len; i++) {
      JsonValue* c = cases->v.arr.items[i];
      if (!c || c->type != JSON_OBJECT) {
        err_codef(p, "sircc.cfg.term.switch.case.not_object", "sircc: term.switch case[%zu] must be object", i);
        sir_diag_pop(p, saved);
        return false;
      }
      int64_t to_id = 0;
      if (!parse_node_ref_id(p, json_obj_get(c, "to"), &to_id)) {
        err_codef(p, "sircc.cfg.term.switch.case.missing_to", "sircc: term.switch case[%zu] missing to ref", i);
        sir_diag_pop(p, saved);
        return false;
      }
      if (!validate_block_params(p, to_id)) {
        sir_diag_pop(p, saved);
        return false;
      }
      if (!validate_branch_args(p, to_id, json_obj_get(c, "args"))) {
        sir_diag_pop(p, saved);
        return false;
      }
      int64_t lit_id = 0;
      if (!parse_node_ref_id(p, json_obj_get(c, "lit"), &lit_id)) {
        err_codef(p, "sircc.cfg.term.switch.case.missing_lit", "sircc: term.switch case[%zu] missing lit ref", i);
        sir_diag_pop(p, saved);
        return false;
      }
      NodeRec* litn = get_node(p, lit_id);
      if (!litn || strncmp(litn->tag, "const.", 6) != 0) {
        err_codef(p, "sircc.cfg.term.switch.case.bad_lit", "sircc: term.switch case[%zu] lit must be const.* node", i);
        sir_diag_pop(p, saved);
        return false;
      }
    }
    sir_diag_pop(p, saved);
    return true;
  }

  sir_diag_pop(p, saved);
  return true;
}

static bool validate_cfg_fn(SirProgram* p, NodeRec* fn) {
  SirDiagSaved saved = sir_diag_push_node(p, fn);
  JsonValue* blocks = json_obj_get(fn->fields, "blocks");
  JsonValue* entry = json_obj_get(fn->fields, "entry");
  if (!blocks || blocks->type != JSON_ARRAY || !entry) {
    err_codef(p, "sircc.cfg.fn.missing_fields", "sircc: fn %lld CFG form requires fields.blocks (array) and fields.entry (ref)",
              (long long)fn->id);
    sir_diag_pop(p, saved);
    return false;
  }
  int64_t entry_id = 0;
  if (!parse_node_ref_id(p, entry, &entry_id)) {
    err_codef(p, "sircc.cfg.fn.entry.bad_ref", "sircc: fn %lld entry must be a block ref", (long long)fn->id);
    sir_diag_pop(p, saved);
    return false;
  }

  // mark blocks in this fn for quick membership
  unsigned char* in_fn = (unsigned char*)calloc(p->nodes_cap ? p->nodes_cap : 1, 1);
  if (!in_fn) {
    sir_diag_pop(p, saved);
    return false;
  }
  for (size_t i = 0; i < blocks->v.arr.len; i++) {
    int64_t bid = 0;
    if (!parse_node_ref_id(p, blocks->v.arr.items[i], &bid)) {
      err_codef(p, "sircc.cfg.fn.blocks.bad_ref", "sircc: fn %lld blocks[%zu] must be block refs", (long long)fn->id, i);
      free(in_fn);
      sir_diag_pop(p, saved);
      return false;
    }
    if (bid >= 0 && (size_t)bid < p->nodes_cap) in_fn[bid] = 1;
    if (!validate_block_params(p, bid)) {
      free(in_fn);
      sir_diag_pop(p, saved);
      return false;
    }
  }
  if (entry_id < 0 || (size_t)entry_id >= p->nodes_cap || !in_fn[entry_id]) {
    err_codef(p, "sircc.cfg.fn.entry.not_in_blocks", "sircc: fn %lld entry block %lld not in blocks list", (long long)fn->id,
              (long long)entry_id);
    free(in_fn);
    sir_diag_pop(p, saved);
    return false;
  }

  for (size_t i = 0; i < blocks->v.arr.len; i++) {
    int64_t bid = 0;
    (void)parse_node_ref_id(p, blocks->v.arr.items[i], &bid);
    NodeRec* b = get_node(p, bid);
    if (!b || strcmp(b->tag, "block") != 0) {
      SirDiagSaved bsaved = sir_diag_push(p, "node", bid, b ? b->tag : NULL);
      err_codef(p, "sircc.cfg.fn.blocks.not_block", "sircc: fn %lld blocks[%zu] references non-block %lld", (long long)fn->id, i,
                (long long)bid);
      sir_diag_pop(p, bsaved);
      free(in_fn);
      sir_diag_pop(p, saved);
      return false;
    }
    JsonValue* stmts = b->fields ? json_obj_get(b->fields, "stmts") : NULL;
    if (!stmts || stmts->type != JSON_ARRAY || stmts->v.arr.len == 0) {
      SirDiagSaved bsaved = sir_diag_push_node(p, b);
      err_codef(p, "sircc.cfg.block.stmts.not_array", "sircc: block %lld must have non-empty stmts array", (long long)bid);
      sir_diag_pop(p, bsaved);
      free(in_fn);
      sir_diag_pop(p, saved);
      return false;
    }
    for (size_t si = 0; si < stmts->v.arr.len; si++) {
      int64_t sid = 0;
      if (!parse_node_ref_id(p, stmts->v.arr.items[si], &sid)) {
        SirDiagSaved bsaved = sir_diag_push_node(p, b);
        err_codef(p, "sircc.cfg.block.stmt.not_ref", "sircc: block %lld stmts[%zu] must be node refs", (long long)bid, si);
        sir_diag_pop(p, bsaved);
        free(in_fn);
        sir_diag_pop(p, saved);
        return false;
      }
      NodeRec* sn = get_node(p, sid);
      if (!sn) {
        SirDiagSaved bsaved = sir_diag_push_node(p, b);
        err_codef(p, "sircc.cfg.block.stmt.unknown_node", "sircc: block %lld stmts[%zu] references unknown node %lld", (long long)bid, si,
                  (long long)sid);
        sir_diag_pop(p, bsaved);
        free(in_fn);
        sir_diag_pop(p, saved);
        return false;
      }
      bool is_term = (strncmp(sn->tag, "term.", 5) == 0) || (strcmp(sn->tag, "return") == 0);
      if (is_term && si + 1 != stmts->v.arr.len) {
        SirDiagSaved ssaved = sir_diag_push_node(p, sn);
        err_codef(p, "sircc.cfg.block.term.not_last", "sircc: block %lld has terminator before end (stmt %zu)", (long long)bid, si);
        sir_diag_pop(p, ssaved);
        free(in_fn);
        sir_diag_pop(p, saved);
        return false;
      }
      if (si + 1 == stmts->v.arr.len) {
        if (!is_term) {
          SirDiagSaved ssaved = sir_diag_push_node(p, sn);
          err_codef(p, "sircc.cfg.block.term.missing", "sircc: block %lld must end with a terminator (got '%s')", (long long)bid, sn->tag);
          sir_diag_pop(p, ssaved);
          free(in_fn);
          sir_diag_pop(p, saved);
          return false;
        }
        if (!validate_terminator(p, sid)) {
          free(in_fn);
          sir_diag_pop(p, saved);
          return false;
        }
      }
    }
  }

  free(in_fn);
  sir_diag_pop(p, saved);
  return true;
}

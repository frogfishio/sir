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
    errf(p, "sircc: block ref %lld is not a block node", (long long)block_id);
    return false;
  }
  JsonValue* params = b->fields ? json_obj_get(b->fields, "params") : NULL;
  if (!params) return true;
  if (params->type != JSON_ARRAY) {
    errf(p, "sircc: block %lld params must be an array", (long long)block_id);
    return false;
  }
  for (size_t i = 0; i < params->v.arr.len; i++) {
    int64_t pid = 0;
    if (!parse_node_ref_id(params->v.arr.items[i], &pid)) {
      errf(p, "sircc: block %lld params[%zu] must be node refs", (long long)block_id, i);
      return false;
    }
    NodeRec* pn = get_node(p, pid);
    if (!pn || strcmp(pn->tag, "bparam") != 0) {
      errf(p, "sircc: block %lld params[%zu] must reference bparam nodes", (long long)block_id, i);
      return false;
    }
    if (pn->type_ref == 0) {
      errf(p, "sircc: bparam node %lld missing type_ref", (long long)pid);
      return false;
    }
  }
  return true;
}

static bool validate_branch_args(SirProgram* p, int64_t to_block_id, JsonValue* args) {
  size_t pc = block_param_count(p, to_block_id);
  if (pc == (size_t)-1) {
    errf(p, "sircc: block %lld params must be an array", (long long)to_block_id);
    return false;
  }
  size_t ac = 0;
  if (args) {
    if (args->type != JSON_ARRAY) {
      errf(p, "sircc: branch args must be an array");
      return false;
    }
    ac = args->v.arr.len;
  }
  if (pc != ac) {
    errf(p, "sircc: block %lld param/arg count mismatch (params=%zu, args=%zu)", (long long)to_block_id, pc, ac);
    return false;
  }
  for (size_t i = 0; i < ac; i++) {
    int64_t aid = 0;
    if (!parse_node_ref_id(args->v.arr.items[i], &aid)) {
      errf(p, "sircc: branch args[%zu] must be node refs", i);
      return false;
    }
    if (!get_node(p, aid)) {
      errf(p, "sircc: branch args[%zu] references unknown node %lld", i, (long long)aid);
      return false;
    }
  }
  return true;
}

static bool validate_terminator(SirProgram* p, int64_t term_id) {
  NodeRec* t = get_node(p, term_id);
  if (!t) {
    errf(p, "sircc: block terminator references unknown node %lld", (long long)term_id);
    return false;
  }
  if (strncmp(t->tag, "term.", 5) != 0 && strcmp(t->tag, "return") != 0) {
    errf(p, "sircc: block must end with a terminator (got '%s')", t->tag);
    return false;
  }

  if (strcmp(t->tag, "term.br") == 0) {
    if (!t->fields) {
      errf(p, "sircc: term.br missing fields");
      return false;
    }
    int64_t to_id = 0;
    if (!parse_node_ref_id(json_obj_get(t->fields, "to"), &to_id)) {
      errf(p, "sircc: term.br missing to ref");
      return false;
    }
    if (!validate_block_params(p, to_id)) return false;
    return validate_branch_args(p, to_id, json_obj_get(t->fields, "args"));
  }

  if (strcmp(t->tag, "term.cbr") == 0 || strcmp(t->tag, "term.condbr") == 0) {
    if (!t->fields) {
      errf(p, "sircc: %s missing fields", t->tag);
      return false;
    }
    int64_t cond_id = 0;
    if (!parse_node_ref_id(json_obj_get(t->fields, "cond"), &cond_id)) {
      errf(p, "sircc: %s missing cond ref", t->tag);
      return false;
    }
    if (!get_node(p, cond_id)) {
      errf(p, "sircc: %s cond references unknown node %lld", t->tag, (long long)cond_id);
      return false;
    }
    JsonValue* thenb = json_obj_get(t->fields, "then");
    JsonValue* elseb = json_obj_get(t->fields, "else");
    if (!thenb || thenb->type != JSON_OBJECT || !elseb || elseb->type != JSON_OBJECT) {
      errf(p, "sircc: %s requires then/else objects", t->tag);
      return false;
    }
    int64_t then_id = 0, else_id = 0;
    if (!parse_node_ref_id(json_obj_get(thenb, "to"), &then_id) || !parse_node_ref_id(json_obj_get(elseb, "to"), &else_id)) {
      errf(p, "sircc: %s then/else missing to ref", t->tag);
      return false;
    }
    if (!validate_block_params(p, then_id) || !validate_block_params(p, else_id)) return false;
    if (!validate_branch_args(p, then_id, json_obj_get(thenb, "args"))) return false;
    if (!validate_branch_args(p, else_id, json_obj_get(elseb, "args"))) return false;
    return true;
  }

  if (strcmp(t->tag, "term.switch") == 0) {
    if (!t->fields) {
      errf(p, "sircc: term.switch missing fields");
      return false;
    }
    int64_t scrut_id = 0;
    if (!parse_node_ref_id(json_obj_get(t->fields, "scrut"), &scrut_id)) {
      errf(p, "sircc: term.switch missing scrut ref");
      return false;
    }
    if (!get_node(p, scrut_id)) {
      errf(p, "sircc: term.switch scrut references unknown node %lld", (long long)scrut_id);
      return false;
    }
    JsonValue* def = json_obj_get(t->fields, "default");
    if (!def || def->type != JSON_OBJECT) {
      errf(p, "sircc: term.switch missing default branch");
      return false;
    }
    int64_t def_id = 0;
    if (!parse_node_ref_id(json_obj_get(def, "to"), &def_id)) {
      errf(p, "sircc: term.switch default missing to ref");
      return false;
    }
    if (!validate_block_params(p, def_id)) return false;
    if (!validate_branch_args(p, def_id, json_obj_get(def, "args"))) return false;
    JsonValue* cases = json_obj_get(t->fields, "cases");
    if (!cases || cases->type != JSON_ARRAY) {
      errf(p, "sircc: term.switch missing cases array");
      return false;
    }
    for (size_t i = 0; i < cases->v.arr.len; i++) {
      JsonValue* c = cases->v.arr.items[i];
      if (!c || c->type != JSON_OBJECT) {
        errf(p, "sircc: term.switch case[%zu] must be object", i);
        return false;
      }
      int64_t to_id = 0;
      if (!parse_node_ref_id(json_obj_get(c, "to"), &to_id)) {
        errf(p, "sircc: term.switch case[%zu] missing to ref", i);
        return false;
      }
      if (!validate_block_params(p, to_id)) return false;
      if (!validate_branch_args(p, to_id, json_obj_get(c, "args"))) return false;
      int64_t lit_id = 0;
      if (!parse_node_ref_id(json_obj_get(c, "lit"), &lit_id)) {
        errf(p, "sircc: term.switch case[%zu] missing lit ref", i);
        return false;
      }
      NodeRec* litn = get_node(p, lit_id);
      if (!litn || strncmp(litn->tag, "const.", 6) != 0) {
        errf(p, "sircc: term.switch case[%zu] lit must be const.* node", i);
        return false;
      }
    }
    return true;
  }

  return true;
}

static bool validate_cfg_fn(SirProgram* p, NodeRec* fn) {
  JsonValue* blocks = json_obj_get(fn->fields, "blocks");
  JsonValue* entry = json_obj_get(fn->fields, "entry");
  if (!blocks || blocks->type != JSON_ARRAY || !entry) {
    errf(p, "sircc: fn %lld CFG form requires fields.blocks (array) and fields.entry (ref)", (long long)fn->id);
    return false;
  }
  int64_t entry_id = 0;
  if (!parse_node_ref_id(entry, &entry_id)) {
    errf(p, "sircc: fn %lld entry must be a block ref", (long long)fn->id);
    return false;
  }

  // mark blocks in this fn for quick membership
  unsigned char* in_fn = (unsigned char*)calloc(p->nodes_cap ? p->nodes_cap : 1, 1);
  if (!in_fn) return false;
  for (size_t i = 0; i < blocks->v.arr.len; i++) {
    int64_t bid = 0;
    if (!parse_node_ref_id(blocks->v.arr.items[i], &bid)) {
      errf(p, "sircc: fn %lld blocks[%zu] must be block refs", (long long)fn->id, i);
      free(in_fn);
      return false;
    }
    if (bid >= 0 && (size_t)bid < p->nodes_cap) in_fn[bid] = 1;
    if (!validate_block_params(p, bid)) {
      free(in_fn);
      return false;
    }
  }
  if (entry_id < 0 || (size_t)entry_id >= p->nodes_cap || !in_fn[entry_id]) {
    errf(p, "sircc: fn %lld entry block %lld not in blocks list", (long long)fn->id, (long long)entry_id);
    free(in_fn);
    return false;
  }

  for (size_t i = 0; i < blocks->v.arr.len; i++) {
    int64_t bid = 0;
    (void)parse_node_ref_id(blocks->v.arr.items[i], &bid);
    NodeRec* b = get_node(p, bid);
    if (!b || strcmp(b->tag, "block") != 0) {
      errf(p, "sircc: fn %lld blocks[%zu] references non-block %lld", (long long)fn->id, i, (long long)bid);
      free(in_fn);
      return false;
    }
    JsonValue* stmts = b->fields ? json_obj_get(b->fields, "stmts") : NULL;
    if (!stmts || stmts->type != JSON_ARRAY || stmts->v.arr.len == 0) {
      errf(p, "sircc: block %lld must have non-empty stmts array", (long long)bid);
      free(in_fn);
      return false;
    }
    for (size_t si = 0; si < stmts->v.arr.len; si++) {
      int64_t sid = 0;
      if (!parse_node_ref_id(stmts->v.arr.items[si], &sid)) {
        errf(p, "sircc: block %lld stmts[%zu] must be node refs", (long long)bid, si);
        free(in_fn);
        return false;
      }
      NodeRec* sn = get_node(p, sid);
      if (!sn) {
        errf(p, "sircc: block %lld stmts[%zu] references unknown node %lld", (long long)bid, si, (long long)sid);
        free(in_fn);
        return false;
      }
      bool is_term = (strncmp(sn->tag, "term.", 5) == 0) || (strcmp(sn->tag, "return") == 0);
      if (is_term && si + 1 != stmts->v.arr.len) {
        errf(p, "sircc: block %lld has terminator before end (stmt %zu)", (long long)bid, si);
        free(in_fn);
        return false;
      }
      if (si + 1 == stmts->v.arr.len) {
        if (!is_term) {
          errf(p, "sircc: block %lld must end with a terminator (got '%s')", (long long)bid, sn->tag);
          free(in_fn);
          return false;
        }
        if (!validate_terminator(p, sid)) {
          free(in_fn);
          return false;
        }
      }
    }
  }

  free(in_fn);
  return true;
}


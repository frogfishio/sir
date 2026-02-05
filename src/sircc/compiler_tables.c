// SPDX-FileCopyrightText: 2026 Frogfish
// SPDX-License-Identifier: GPL-3.0-or-later

#include "compiler_internal.h"

#include <string.h>

TypeRec* get_type(SirProgram* p, int64_t id) {
  if (id < 0 || (size_t)id >= p->types_cap) return NULL;
  return p->types[id];
}

SymRec* get_sym(SirProgram* p, int64_t id) {
  if (id < 0 || (size_t)id >= p->syms_cap) return NULL;
  return p->syms[id];
}

SymRec* find_sym_by_name(SirProgram* p, const char* name) {
  if (!p || !name) return NULL;
  for (size_t i = 0; i < p->syms_cap; i++) {
    SymRec* s = p->syms[i];
    if (!s || !s->name) continue;
    if (strcmp(s->name, name) == 0) return s;
  }
  return NULL;
}

NodeRec* get_node(SirProgram* p, int64_t id) {
  if (id < 0 || (size_t)id >= p->nodes_cap) return NULL;
  return p->nodes[id];
}

NodeRec* find_fn_node_by_name(SirProgram* p, const char* name) {
  if (!p || !name) return NULL;
  for (size_t i = 0; i < p->nodes_cap; i++) {
    NodeRec* n = p->nodes[i];
    if (!n || !n->tag || strcmp(n->tag, "fn") != 0 || !n->fields) continue;
    const char* nm = json_get_string(json_obj_get(n->fields, "name"));
    if (!nm) continue;
    if (strcmp(nm, name) == 0) return n;
  }
  return NULL;
}

NodeRec* find_decl_fn_node_by_name(SirProgram* p, const char* name) {
  if (!p || !name) return NULL;
  for (size_t i = 0; i < p->nodes_cap; i++) {
    NodeRec* n = p->nodes[i];
    if (!n || !n->tag || strcmp(n->tag, "decl.fn") != 0 || !n->fields) continue;
    const char* nm = json_get_string(json_obj_get(n->fields, "name"));
    if (!nm) continue;
    if (strcmp(nm, name) == 0) return n;
  }
  return NULL;
}

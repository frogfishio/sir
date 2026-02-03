// SPDX-FileCopyrightText: 2026 Frogfish
// SPDX-License-Identifier: GPL-3.0-or-later

#include "compiler_internal.h"

TypeRec* get_type(SirProgram* p, int64_t id) {
  if (id < 0 || (size_t)id >= p->types_cap) return NULL;
  return p->types[id];
}

NodeRec* get_node(SirProgram* p, int64_t id) {
  if (id < 0 || (size_t)id >= p->nodes_cap) return NULL;
  return p->nodes[id];
}


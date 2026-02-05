// SPDX-FileCopyrightText: 2026 Frogfish
// SPDX-License-Identifier: GPL-3.0-or-later

#include "compiler_zasm_regcache.h"

#include <string.h>

typedef struct {
  const char* hl_slot;
  int64_t hl_width;
  const char* de_slot;
  int64_t de_width;
} ZasmRegCache;

static ZasmRegCache g_regcache;

void zasm_regcache_init(void) { zasm_regcache_clear_all(); }

void zasm_regcache_clear_all(void) {
  g_regcache.hl_slot = NULL;
  g_regcache.hl_width = 0;
  g_regcache.de_slot = NULL;
  g_regcache.de_width = 0;
}

void zasm_regcache_invalidate_reg(const char* reg) {
  if (!reg) return;
  if (strcmp(reg, "HL") == 0) {
    g_regcache.hl_slot = NULL;
    g_regcache.hl_width = 0;
  } else if (strcmp(reg, "DE") == 0) {
    g_regcache.de_slot = NULL;
    g_regcache.de_width = 0;
  }
}

void zasm_regcache_invalidate_slot(const char* slot_sym, int64_t width_bytes) {
  if (!slot_sym) return;
  if (g_regcache.hl_slot && strcmp(g_regcache.hl_slot, slot_sym) == 0 && g_regcache.hl_width == width_bytes) {
    g_regcache.hl_slot = NULL;
    g_regcache.hl_width = 0;
  }
  if (g_regcache.de_slot && strcmp(g_regcache.de_slot, slot_sym) == 0 && g_regcache.de_width == width_bytes) {
    g_regcache.de_slot = NULL;
    g_regcache.de_width = 0;
  }
}

bool zasm_regcache_matches_slot(const char* reg, const char* slot_sym, int64_t width_bytes) {
  if (!reg || !slot_sym) return false;
  if (strcmp(reg, "HL") == 0) {
    return g_regcache.hl_slot && strcmp(g_regcache.hl_slot, slot_sym) == 0 && g_regcache.hl_width == width_bytes;
  }
  if (strcmp(reg, "DE") == 0) {
    return g_regcache.de_slot && strcmp(g_regcache.de_slot, slot_sym) == 0 && g_regcache.de_width == width_bytes;
  }
  return false;
}

void zasm_regcache_set_slot(const char* reg, const char* slot_sym, int64_t width_bytes) {
  if (!reg) return;
  if (strcmp(reg, "HL") == 0) {
    g_regcache.hl_slot = slot_sym;
    g_regcache.hl_width = width_bytes;
  } else if (strcmp(reg, "DE") == 0) {
    g_regcache.de_slot = slot_sym;
    g_regcache.de_width = width_bytes;
  }
}

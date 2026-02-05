// SPDX-FileCopyrightText: 2026 Frogfish
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <stdbool.h>
#include <stdint.h>

void zasm_regcache_init(void);
void zasm_regcache_clear_all(void);
void zasm_regcache_invalidate_reg(const char* reg);
void zasm_regcache_invalidate_slot(const char* slot_sym, int64_t width_bytes);
bool zasm_regcache_matches_slot(const char* reg, const char* slot_sym, int64_t width_bytes);
void zasm_regcache_set_slot(const char* reg, const char* slot_sym, int64_t width_bytes);

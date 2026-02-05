// SPDX-FileCopyrightText: 2026 Frogfish
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "compiler_zasm_internal.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

typedef struct {
  const char* sym;
  int64_t size_bytes;
} ZasmTempSlot;

int64_t width_for_type_id(SirProgram* p, int64_t type_id);

bool ensure_bparam_slot(
    SirProgram* p,
    ZasmBParamSlot** bps,
    size_t* bps_len,
    size_t* bps_cap,
    int64_t bparam_id,
    int64_t size_bytes,
    const char** out_sym);

bool add_temp_slot(
    SirProgram* p,
    ZasmTempSlot** slots,
    size_t* slots_len,
    size_t* slots_cap,
    int64_t id_hint,
    int64_t size_bytes,
    const char** out_sym);

bool emit_st64_slot_from_hl(FILE* out, const char* slot_sym, int64_t line_no);
bool emit_store_reg_to_slot(FILE* out, const char* slot_sym, int64_t size_bytes, const char* reg, int64_t line_no);
const char* reg_for_width(int64_t width_bytes);
bool emit_load_slot_to_reg(FILE* out, const char* slot_sym, int64_t width_bytes, const char* dst_reg, int64_t line_no);
bool emit_ld_reg_or_imm(FILE* out, const char* dst_reg, const ZasmOp* op, int64_t line_no);

bool emit_jr(FILE* out, const char* lbl, int64_t line_no);
bool emit_jr_cond(FILE* out, const char* cond_sym, const char* lbl, int64_t line_no);

const char* zasm_mnemonic_for_binop(const char* tag);
const char* zasm_mnemonic_for_unop(const char* tag);
const char* zasm_cmp_set_mnemonic_for_node_tag(const char* tag);
bool emit_cp_hl(FILE* out, const ZasmOp* rhs, int64_t line_no);
bool emit_cmp_set_hl(FILE* out, const char* mnemonic, const ZasmOp* rhs, int64_t line_no);

bool emit_bind_slot(
    SirProgram* p,
    ZasmNameBinding** names,
    size_t* name_len,
    size_t* name_cap,
    const char* bind_name,
    const char* slot_sym,
    int64_t slot_size_bytes);
bool emit_bind_op(SirProgram* p, ZasmNameBinding** names, size_t* name_len, size_t* name_cap, const char* bind_name, ZasmOp op);

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
    int64_t* io_line);

const char* label_for_block(SirProgram* p, int64_t entry_id, int64_t block_id);
const char* label_for_cbr_edge(SirProgram* p, int64_t term_id, const char* which);

bool emit_cfg_branch_args(
    FILE* out,
    SirProgram* p,
    ZasmStr* strs,
    size_t strs_len,
    ZasmAlloca* allocas,
    size_t allocas_len,
    ZasmNameBinding* names,
    size_t name_len,
    ZasmBParamSlot** bps,
    size_t* bp_len,
    size_t* bp_cap,
    int64_t to_id,
    JsonValue* args,
    int64_t* line);


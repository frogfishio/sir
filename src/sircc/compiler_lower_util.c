// SPDX-FileCopyrightText: 2026 Frogfish
// SPDX-License-Identifier: GPL-3.0-or-later

#include "compiler_lower_internal.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

LLVMValueRef canonical_qnan(FunctionCtx* f, LLVMTypeRef fty) {
  if (LLVMGetTypeKind(fty) == LLVMFloatTypeKind) {
    LLVMValueRef ib = LLVMConstInt(LLVMInt32TypeInContext(f->ctx), 0x7fc00000u, 0);
    return LLVMConstBitCast(ib, fty);
  }
  if (LLVMGetTypeKind(fty) == LLVMDoubleTypeKind) {
    LLVMValueRef ib = LLVMConstInt(LLVMInt64TypeInContext(f->ctx), 0x7ff8000000000000ULL, 0);
    return LLVMConstBitCast(ib, fty);
  }
  return LLVMGetUndef(fty);
}

LLVMValueRef canonicalize_float(FunctionCtx* f, LLVMValueRef v) {
  LLVMTypeRef ty = LLVMTypeOf(v);
  if (LLVMGetTypeKind(ty) != LLVMFloatTypeKind && LLVMGetTypeKind(ty) != LLVMDoubleTypeKind) return v;
  LLVMValueRef isnan = LLVMBuildFCmp(f->builder, LLVMRealUNO, v, v, "isnan");
  LLVMValueRef qnan = canonical_qnan(f, ty);
  return LLVMBuildSelect(f->builder, isnan, qnan, v, "canon");
}

void emit_trap_unreachable(FunctionCtx* f) {
  LLVMTypeRef v = LLVMVoidTypeInContext(f->ctx);
  LLVMValueRef fn = get_or_declare_intrinsic(f->mod, "llvm.trap", v, NULL, 0);
  LLVMBuildCall2(f->builder, LLVMGlobalGetValueType(fn), fn, NULL, 0, "");
  LLVMBuildUnreachable(f->builder);
}

bool emit_trap_if(FunctionCtx* f, LLVMValueRef cond) {
  if (!f || !f->builder || !f->fn) return false;
  if (!cond || LLVMGetTypeKind(LLVMTypeOf(cond)) != LLVMIntegerTypeKind || LLVMGetIntTypeWidth(LLVMTypeOf(cond)) != 1) return false;

  if (LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(f->builder))) return false;
  LLVMBasicBlockRef trap_bb = LLVMAppendBasicBlockInContext(f->ctx, f->fn, "trap");
  LLVMBasicBlockRef cont_bb = LLVMAppendBasicBlockInContext(f->ctx, f->fn, "cont");
  LLVMBuildCondBr(f->builder, cond, trap_bb, cont_bb);

  LLVMPositionBuilderAtEnd(f->builder, trap_bb);
  emit_trap_unreachable(f);

  LLVMPositionBuilderAtEnd(f->builder, cont_bb);
  return true;
}

bool emit_trap_if_misaligned(FunctionCtx* f, LLVMValueRef ptr, unsigned align) {
  if (!f || !ptr) return false;
  if (align <= 1) return true;
  if ((align & (align - 1u)) != 0u) {
    err_codef(f->p, "sircc.align.not_pow2", "sircc: align must be a power of two (got %u)", align);
    return false;
  }

  if (LLVMGetTypeKind(LLVMTypeOf(ptr)) != LLVMPointerTypeKind) {
    err_codef(f->p, "sircc.internal.align.ptr_required", "sircc: internal: alignment check requires ptr");
    return false;
  }

  unsigned ptr_bits = f->p->ptr_bits ? f->p->ptr_bits : (unsigned)(sizeof(void*) * 8u);
  LLVMTypeRef ip = LLVMIntTypeInContext(f->ctx, ptr_bits);
  LLVMValueRef addr = LLVMBuildPtrToInt(f->builder, ptr, ip, "addr.bits");
  LLVMValueRef mask = LLVMConstInt(ip, (unsigned long long)(align - 1u), 0);
  LLVMValueRef low = LLVMBuildAnd(f->builder, addr, mask, "addr.low");
  LLVMValueRef z = LLVMConstInt(ip, 0, 0);
  LLVMValueRef bad = LLVMBuildICmp(f->builder, LLVMIntNE, low, z, "misaligned");
  return emit_trap_if(f, bad);
}

bool bind_add(FunctionCtx* f, const char* name, LLVMValueRef v) {
  if (!name) return false;
  if (f->bind_len == f->bind_cap) {
    size_t next = f->bind_cap ? f->bind_cap * 2 : 16;
    Binding* bigger = (Binding*)realloc(f->binds, next * sizeof(Binding));
    if (!bigger) return false;
    f->binds = bigger;
    f->bind_cap = next;
  }
  f->binds[f->bind_len++] = (Binding){.name = name, .value = v};
  return true;
}

LLVMValueRef bind_get(FunctionCtx* f, const char* name) {
  for (size_t i = f->bind_len; i > 0; i--) {
    if (strcmp(f->binds[i - 1].name, name) == 0) return f->binds[i - 1].value;
  }
  return NULL;
}

size_t bind_mark(FunctionCtx* f) { return f ? f->bind_len : 0; }
void bind_restore(FunctionCtx* f, size_t mark) {
  if (!f) return;
  if (mark > f->bind_len) return;
  f->bind_len = mark;
}

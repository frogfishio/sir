// SPDX-FileCopyrightText: 2026 Frogfish
// SPDX-License-Identifier: GPL-3.0-or-later

#include "compiler_internal.h"

#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

LLVMTypeRef lower_type(SirProgram* p, LLVMContextRef ctx, int64_t id);

LLVMTypeRef lower_type_prim(LLVMContextRef ctx, const char* prim) {
  if (strcmp(prim, "i1") == 0 || strcmp(prim, "bool") == 0) return LLVMInt1TypeInContext(ctx);
  if (strcmp(prim, "i8") == 0) return LLVMInt8TypeInContext(ctx);
  if (strcmp(prim, "i16") == 0) return LLVMInt16TypeInContext(ctx);
  if (strcmp(prim, "i32") == 0) return LLVMInt32TypeInContext(ctx);
  if (strcmp(prim, "i64") == 0) return LLVMInt64TypeInContext(ctx);
  if (strcmp(prim, "f32") == 0) return LLVMFloatTypeInContext(ctx);
  if (strcmp(prim, "f64") == 0) return LLVMDoubleTypeInContext(ctx);
  if (strcmp(prim, "void") == 0) return LLVMVoidTypeInContext(ctx);
  return NULL;
}

bool type_size_align_rec(SirProgram* p, int64_t type_id, unsigned char* visiting, int64_t* out_size,
                                int64_t* out_align) {
  if (!p || !out_size || !out_align) return false;
  if (type_id < 0 || (size_t)type_id >= p->types_cap || !p->types[type_id]) return false;
  if (visiting && visiting[type_id]) return false;
  if (visiting) visiting[type_id] = 1;

  TypeRec* tr = p->types[type_id];
  int64_t size = 0;
  int64_t align = 0;
  switch (tr->kind) {
    case TYPE_PRIM:
      if (strcmp(tr->prim, "i1") == 0 || strcmp(tr->prim, "bool") == 0) {
        size = 1;
        align = p->align_i8 ? (int64_t)p->align_i8 : 1;
      } else if (strcmp(tr->prim, "i8") == 0) {
        size = 1;
        align = p->align_i8 ? (int64_t)p->align_i8 : 1;
      } else if (strcmp(tr->prim, "i16") == 0) {
        size = 2;
        align = p->align_i16 ? (int64_t)p->align_i16 : 2;
      } else if (strcmp(tr->prim, "i32") == 0) {
        size = 4;
        align = p->align_i32 ? (int64_t)p->align_i32 : 4;
      } else if (strcmp(tr->prim, "i64") == 0) {
        size = 8;
        align = p->align_i64 ? (int64_t)p->align_i64 : 8;
      } else if (strcmp(tr->prim, "f32") == 0) {
        size = 4;
        align = p->align_f32 ? (int64_t)p->align_f32 : 4;
      } else if (strcmp(tr->prim, "f64") == 0) {
        size = 8;
        align = p->align_f64 ? (int64_t)p->align_f64 : 8;
      } else if (strcmp(tr->prim, "void") == 0) {
        if (visiting) visiting[type_id] = 0;
        return false;
      } else {
        if (visiting) visiting[type_id] = 0;
        return false;
      }
      break;
    case TYPE_PTR:
      size = (int64_t)(p->ptr_bytes ? p->ptr_bytes : (unsigned)sizeof(void*));
      if (p->align_ptr) align = (int64_t)p->align_ptr;
      else align = (int64_t)(p->ptr_bytes ? p->ptr_bytes : (unsigned)sizeof(void*));
      break;
    case TYPE_ARRAY: {
      int64_t el_size = 0;
      int64_t el_align = 0;
      if (!type_size_align_rec(p, tr->of, visiting, &el_size, &el_align)) {
        if (visiting) visiting[type_id] = 0;
        return false;
      }
      if (el_align <= 0) {
        if (visiting) visiting[type_id] = 0;
        return false;
      }
      int64_t stride = el_size;
      int64_t rem = stride % el_align;
      if (rem) stride += (el_align - rem);
      if (tr->len < 0) {
        if (visiting) visiting[type_id] = 0;
        return false;
      }
      if (tr->len == 0) {
        size = 0;
        align = el_align;
        break;
      }
      if (stride != 0 && tr->len > INT64_MAX / stride) {
        if (visiting) visiting[type_id] = 0;
        return false;
      }
      size = stride * tr->len;
      align = el_align;
      break;
    }
    case TYPE_STRUCT: {
      int64_t off = 0;
      int64_t max_align = 1;
      for (size_t i = 0; i < tr->field_len; i++) {
        int64_t fsz = 0;
        int64_t fal = 0;
        if (!type_size_align_rec(p, tr->fields[i].type_ref, visiting, &fsz, &fal)) {
          if (visiting) visiting[type_id] = 0;
          return false;
        }
        if (fal <= 0) {
          if (visiting) visiting[type_id] = 0;
          return false;
        }
        if (fal > max_align) max_align = fal;
        int64_t rem = off % fal;
        if (rem) off += (fal - rem);
        if (fsz != 0 && off > INT64_MAX - fsz) {
          if (visiting) visiting[type_id] = 0;
          return false;
        }
        off += fsz;
      }
      if (max_align <= 0) {
        if (visiting) visiting[type_id] = 0;
        return false;
      }
      int64_t rem = off % max_align;
      if (rem) off += (max_align - rem);
      size = off;
      align = max_align;
      break;
    }
    case TYPE_FN:
    case TYPE_INVALID:
    default:
      if (visiting) visiting[type_id] = 0;
      return false;
  }

  if (visiting) visiting[type_id] = 0;
  if (size < 0 || align <= 0) return false;
  *out_size = size;
  *out_align = align;
  return true;
}

bool type_size_align(SirProgram* p, int64_t type_id, int64_t* out_size, int64_t* out_align) {
  if (!p || !out_size || !out_align) return false;
  if (type_id < 0 || (size_t)type_id >= p->types_cap || !p->types[type_id]) return false;
  unsigned char* visiting = (unsigned char*)calloc(p->types_cap ? p->types_cap : 1, 1);
  if (!visiting) return false;
  bool ok = type_size_align_rec(p, type_id, visiting, out_size, out_align);
  free(visiting);
  return ok;
}

LLVMValueRef get_or_declare_intrinsic(LLVMModuleRef mod, const char* name, LLVMTypeRef ret,
                                             LLVMTypeRef* params, unsigned param_count) {
  LLVMValueRef fn = LLVMGetNamedFunction(mod, name);
  if (fn) return fn;
  LLVMTypeRef fnty = LLVMFunctionType(ret, params, param_count, 0);
  fn = LLVMAddFunction(mod, name, fnty);
  LLVMSetLinkage(fn, LLVMExternalLinkage);
  return fn;
}

LLVMValueRef build_zext_or_trunc(LLVMBuilderRef b, LLVMValueRef v, LLVMTypeRef ty, const char* name) {
  if (!b || !v || !ty) return NULL;
  if (LLVMTypeOf(v) == ty) return v;
  LLVMTypeRef from_ty = LLVMTypeOf(v);
  if (LLVMGetTypeKind(from_ty) != LLVMIntegerTypeKind || LLVMGetTypeKind(ty) != LLVMIntegerTypeKind) {
    return LLVMBuildTruncOrBitCast(b, v, ty, name ? name : "");
  }
  unsigned from_w = LLVMGetIntTypeWidth(from_ty);
  unsigned to_w = LLVMGetIntTypeWidth(ty);
  if (from_w == to_w) return v;
  if (from_w < to_w) return LLVMBuildZExt(b, v, ty, name ? name : "");
  return LLVMBuildTrunc(b, v, ty, name ? name : "");
}

LLVMValueRef build_sext_or_trunc(LLVMBuilderRef b, LLVMValueRef v, LLVMTypeRef ty, const char* name) {
  if (!b || !v || !ty) return NULL;
  if (LLVMTypeOf(v) == ty) return v;
  LLVMTypeRef from_ty = LLVMTypeOf(v);
  if (LLVMGetTypeKind(from_ty) != LLVMIntegerTypeKind || LLVMGetTypeKind(ty) != LLVMIntegerTypeKind) {
    return LLVMBuildTruncOrBitCast(b, v, ty, name ? name : "");
  }
  unsigned from_w = LLVMGetIntTypeWidth(from_ty);
  unsigned to_w = LLVMGetIntTypeWidth(ty);
  if (from_w == to_w) return v;
  if (from_w < to_w) return LLVMBuildSExt(b, v, ty, name ? name : "");
  return LLVMBuildTrunc(b, v, ty, name ? name : "");
}

LLVMTypeRef lower_type(SirProgram* p, LLVMContextRef ctx, int64_t id) {
  TypeRec* tr = get_type(p, id);
  if (!tr) return NULL;
  if (tr->llvm) return tr->llvm;
  if (tr->resolving) return NULL;
  tr->resolving = true;

  LLVMTypeRef out = NULL;
  switch (tr->kind) {
    case TYPE_PRIM:
      out = lower_type_prim(ctx, tr->prim);
      break;
    case TYPE_PTR: {
      LLVMTypeRef of = lower_type(p, ctx, tr->of);
      if (of) out = LLVMPointerType(of, 0);
      break;
    }
    case TYPE_ARRAY: {
      LLVMTypeRef of = lower_type(p, ctx, tr->of);
      if (of && tr->len >= 0 && tr->len <= (int64_t)UINT_MAX) out = LLVMArrayType(of, (unsigned)tr->len);
      break;
    }
    case TYPE_FN: {
      LLVMTypeRef ret = lower_type(p, ctx, tr->ret);
      if (!ret) break;
      LLVMTypeRef* params = NULL;
      if (tr->param_len) {
        params = (LLVMTypeRef*)malloc(tr->param_len * sizeof(LLVMTypeRef));
        if (!params) break;
        for (size_t i = 0; i < tr->param_len; i++) {
          params[i] = lower_type(p, ctx, tr->params[i]);
          if (!params[i]) {
            free(params);
            params = NULL;
            break;
          }
        }
      }
      if (tr->param_len == 0 || params) {
        out = LLVMFunctionType(ret, params, (unsigned)tr->param_len, tr->varargs ? 1 : 0);
      }
      free(params);
      break;
    }
    case TYPE_STRUCT: {
      if (tr->field_len) {
        LLVMTypeRef* elts = (LLVMTypeRef*)malloc(tr->field_len * sizeof(LLVMTypeRef));
        if (!elts) break;
        bool ok = true;
        for (size_t i = 0; i < tr->field_len; i++) {
          elts[i] = lower_type(p, ctx, tr->fields[i].type_ref);
          if (!elts[i]) {
            ok = false;
            break;
          }
        }
        if (ok) {
          if (tr->name && *tr->name) {
            LLVMTypeRef st = LLVMStructCreateNamed(ctx, tr->name);
            LLVMStructSetBody(st, elts, (unsigned)tr->field_len, 0);
            out = st;
          } else {
            out = LLVMStructTypeInContext(ctx, elts, (unsigned)tr->field_len, 0);
          }
        }
        free(elts);
      } else {
        if (tr->name && *tr->name) {
          LLVMTypeRef st = LLVMStructCreateNamed(ctx, tr->name);
          LLVMStructSetBody(st, NULL, 0, 0);
          out = st;
        } else {
          out = LLVMStructTypeInContext(ctx, NULL, 0, 0);
        }
      }
      break;
    }
    default:
      break;
  }

  tr->llvm = out;
  tr->resolving = false;
  return out;
}

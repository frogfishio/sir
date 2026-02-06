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
    case TYPE_VEC: {
      int64_t lane_size = 0;
      int64_t lane_align = 0;
      if (!type_size_align_rec(p, tr->lane_ty, visiting, &lane_size, &lane_align)) {
        if (visiting) visiting[type_id] = 0;
        return false;
      }
      if (lane_size < 0 || lane_align <= 0) {
        if (visiting) visiting[type_id] = 0;
        return false;
      }
      if (tr->lanes <= 0) {
        if (visiting) visiting[type_id] = 0;
        return false;
      }
      if (lane_size != 0 && tr->lanes > INT64_MAX / lane_size) {
        if (visiting) visiting[type_id] = 0;
        return false;
      }
      size = lane_size * tr->lanes;
      align = lane_align;
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
    case TYPE_FUN:
      size = (int64_t)(p->ptr_bytes ? p->ptr_bytes : (unsigned)sizeof(void*));
      if (p->align_ptr) align = (int64_t)p->align_ptr;
      else align = (int64_t)(p->ptr_bytes ? p->ptr_bytes : (unsigned)sizeof(void*));
      break;
    case TYPE_CLOSURE: {
      // Represent closures as a by-value aggregate: { code_ptr, env }.
      int64_t off = 0;
      int64_t max_align = 1;

      int64_t code_sz = (int64_t)(p->ptr_bytes ? p->ptr_bytes : (unsigned)sizeof(void*));
      int64_t code_al = p->align_ptr ? (int64_t)p->align_ptr : code_sz;
      if (code_al <= 0) {
        if (visiting) visiting[type_id] = 0;
        return false;
      }
      if (code_al > max_align) max_align = code_al;
      int64_t rem0 = off % code_al;
      if (rem0) off += (code_al - rem0);
      off += code_sz;

      int64_t env_sz = 0;
      int64_t env_al = 0;
      if (!type_size_align_rec(p, tr->env_ty, visiting, &env_sz, &env_al)) {
        if (visiting) visiting[type_id] = 0;
        return false;
      }
      if (env_al <= 0) {
        if (visiting) visiting[type_id] = 0;
        return false;
      }
      if (env_al > max_align) max_align = env_al;
      int64_t rem1 = off % env_al;
      if (rem1) off += (env_al - rem1);
      if (env_sz != 0 && off > INT64_MAX - env_sz) {
        if (visiting) visiting[type_id] = 0;
        return false;
      }
      off += env_sz;

      int64_t rem = off % max_align;
      if (rem) off += (max_align - rem);
      size = off;
      align = max_align;
      break;
    }
    case TYPE_SUM: {
      // Layout contract (normative): { tag:i32, payload:bytes }, with payload at the lowest offset >=4
      // satisfying max payload alignment. Type alignment is max(4, max_payload_align). Padding/unused bytes are zero.
      int64_t payload_size = 0;
      int64_t payload_align = 1;
      for (size_t i = 0; i < tr->variant_len; i++) {
        int64_t vty = tr->variants[i].ty;
        if (vty == 0) continue;
        int64_t vsz = 0;
        int64_t val = 0;
        if (!type_size_align_rec(p, vty, visiting, &vsz, &val)) {
          if (visiting) visiting[type_id] = 0;
          return false;
        }
        if (vsz > payload_size) payload_size = vsz;
        if (val > payload_align) payload_align = val;
      }
      if (payload_align < 1) payload_align = 1;
      int64_t align_sum = payload_align;
      if (align_sum < 4) align_sum = 4;
      int64_t payload_off = 4;
      int64_t rem = payload_off % payload_align;
      if (rem) payload_off += (payload_align - rem);
      int64_t total = payload_off + payload_size;
      int64_t rem2 = total % align_sum;
      if (rem2) total += (align_sum - rem2);
      size = total;
      align = align_sum;
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
    case TYPE_VEC: {
      TypeRec* lane = get_type(p, tr->lane_ty);
      if (!lane || lane->kind != TYPE_PRIM || !lane->prim) break;
      LLVMTypeRef el = lower_type_prim(ctx, lane->prim);
      if (!el) break;
      // Deterministic bool vector ABI: represent vec(bool,N) as <N x i8> (0/1) rather than <N x i1>.
      if (strcmp(lane->prim, "bool") == 0 || strcmp(lane->prim, "i1") == 0) {
        el = LLVMInt8TypeInContext(ctx);
      }
      if (tr->lanes <= 0 || tr->lanes > (int64_t)UINT_MAX) break;
      out = LLVMVectorType(el, (unsigned)tr->lanes);
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
    case TYPE_FUN: {
      LLVMTypeRef sig = lower_type(p, ctx, tr->sig);
      if (sig && LLVMGetTypeKind(sig) == LLVMFunctionTypeKind) {
        out = LLVMPointerType(sig, 0);
      }
      break;
    }
    case TYPE_CLOSURE: {
      TypeRec* cs = get_type(p, tr->call_sig);
      if (!cs || cs->kind != TYPE_FN) break;
      LLVMTypeRef env = lower_type(p, ctx, tr->env_ty);
      if (!env) break;
      LLVMTypeRef ret = lower_type(p, ctx, cs->ret);
      if (!ret) break;

      size_t nparams = cs->param_len + 1;
      if (nparams > UINT_MAX) break;
      LLVMTypeRef* params = (LLVMTypeRef*)malloc(nparams * sizeof(LLVMTypeRef));
      if (!params) break;
      params[0] = env;
      bool ok = true;
      for (size_t i = 0; i < cs->param_len; i++) {
        params[i + 1] = lower_type(p, ctx, cs->params[i]);
        if (!params[i + 1]) {
          ok = false;
          break;
        }
      }
      LLVMTypeRef code_sig = NULL;
      if (ok) {
        code_sig = LLVMFunctionType(ret, params, (unsigned)nparams, cs->varargs ? 1 : 0);
      }
      free(params);
      if (!code_sig) break;

      LLVMTypeRef code_ptr = LLVMPointerType(code_sig, 0);
      LLVMTypeRef elts[2] = {code_ptr, env};

      if (tr->name && *tr->name) {
        LLVMTypeRef st = LLVMStructCreateNamed(ctx, tr->name);
        LLVMStructSetBody(st, elts, 2, 0);
        out = st;
      } else {
        out = LLVMStructTypeInContext(ctx, elts, 2, 0);
      }
      break;
    }
    case TYPE_SUM: {
      // Use an explicit padding field so the payload start offset is deterministic.
      int64_t payload_size = 0;
      int64_t payload_align = 1;
      for (size_t i = 0; i < tr->variant_len; i++) {
        int64_t vty = tr->variants[i].ty;
        if (vty == 0) continue;
        int64_t vsz = 0;
        int64_t val = 0;
        if (type_size_align(p, vty, &vsz, &val)) {
          if (vsz > payload_size) payload_size = vsz;
          if (val > payload_align) payload_align = val;
        }
      }
      if (payload_align < 1) payload_align = 1;
      int64_t payload_off = 4;
      int64_t rem = payload_off % payload_align;
      if (rem) payload_off += (payload_align - rem);
      int64_t pad = payload_off - 4;
      if (pad < 0) pad = 0;

      LLVMTypeRef i32 = LLVMInt32TypeInContext(ctx);
      LLVMTypeRef i8 = LLVMInt8TypeInContext(ctx);
      LLVMTypeRef pad_ty = LLVMArrayType(i8, (unsigned)pad);
      LLVMTypeRef payload_ty = LLVMArrayType(i8, (unsigned)(payload_size < 0 ? 0 : payload_size));

      LLVMTypeRef elts[3];
      unsigned n = 0;
      elts[n++] = i32;
      if (pad > 0) elts[n++] = pad_ty;
      elts[n++] = payload_ty;

      out = LLVMStructTypeInContext(ctx, elts, n, 0);
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

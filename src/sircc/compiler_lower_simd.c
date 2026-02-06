// SPDX-FileCopyrightText: 2026 Frogfish
// SPDX-License-Identifier: GPL-3.0-or-later

#include "compiler_lower_internal.h"

#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static bool is_vec_type(SirProgram* p, int64_t ty_id, TypeRec** out_vec, TypeRec** out_lane) {
  if (out_vec) *out_vec = NULL;
  if (out_lane) *out_lane = NULL;
  if (!p || ty_id == 0) return false;
  TypeRec* t = get_type(p, ty_id);
  if (!t || t->kind != TYPE_VEC || t->lane_ty == 0) return false;
  TypeRec* lane = get_type(p, t->lane_ty);
  if (!lane || lane->kind != TYPE_PRIM || !lane->prim) return false;
  if (out_vec) *out_vec = t;
  if (out_lane) *out_lane = lane;
  return true;
}

static bool lane_is_bool(TypeRec* lane) {
  if (!lane || lane->kind != TYPE_PRIM || !lane->prim) return false;
  return (strcmp(lane->prim, "bool") == 0 || strcmp(lane->prim, "i1") == 0);
}

static bool lane_is_float(TypeRec* lane) {
  if (!lane || lane->kind != TYPE_PRIM || !lane->prim) return false;
  return (strcmp(lane->prim, "f32") == 0 || strcmp(lane->prim, "f64") == 0);
}

static LLVMValueRef bool_to_i8(FunctionCtx* f, LLVMValueRef v) {
  if (!f || !v) return NULL;
  LLVMTypeRef vty = LLVMTypeOf(v);
  if (LLVMGetTypeKind(vty) == LLVMIntegerTypeKind && LLVMGetIntTypeWidth(vty) == 1) {
    return LLVMBuildZExt(f->builder, v, LLVMInt8TypeInContext(f->ctx), "b.i8");
  }
  if (LLVMGetTypeKind(vty) == LLVMIntegerTypeKind) {
    LLVMValueRef z = LLVMConstInt(vty, 0, 0);
    LLVMValueRef i1 = LLVMBuildICmp(f->builder, LLVMIntNE, v, z, "b.i1");
    return LLVMBuildZExt(f->builder, i1, LLVMInt8TypeInContext(f->ctx), "b.i8");
  }
  // If v isn't an int, let LLVM complain later via verifier; keep this path deterministic.
  return LLVMBuildTruncOrBitCast(f->builder, v, LLVMInt8TypeInContext(f->ctx), "b.i8");
}

static LLVMValueRef i8_to_bool(FunctionCtx* f, LLVMValueRef v) {
  if (!f || !v) return NULL;
  LLVMTypeRef i8 = LLVMInt8TypeInContext(f->ctx);
  if (LLVMTypeOf(v) != i8) v = LLVMBuildTruncOrBitCast(f->builder, v, i8, "b.tr");
  LLVMValueRef z = LLVMConstInt(i8, 0, 0);
  return LLVMBuildICmp(f->builder, LLVMIntNE, v, z, "b");
}

static bool emit_vec_idx_bounds_check(FunctionCtx* f, int64_t node_id, LLVMValueRef idx, int64_t lanes) {
  if (!f || !idx) return false;
  if (lanes <= 0 || lanes > INT_MAX) {
    err_codef(f->p, "sircc.vec.lanes.bad", "sircc: vec op node %lld has invalid lane count", (long long)node_id);
    return false;
  }

  LLVMTypeRef i32 = LLVMInt32TypeInContext(f->ctx);
  if (LLVMTypeOf(idx) != i32) {
    if (LLVMGetTypeKind(LLVMTypeOf(idx)) != LLVMIntegerTypeKind) {
      err_codef(f->p, "sircc.vec.idx.type_bad", "sircc: vec op node %lld idx must be i32", (long long)node_id);
      return false;
    }
    idx = LLVMBuildTruncOrBitCast(f->builder, idx, i32, "idx.i32");
  }

  LLVMValueRef zero = LLVMConstInt(i32, 0, 0);
  LLVMValueRef max = LLVMConstInt(i32, (unsigned long long)lanes, 0);

  LLVMValueRef neg = LLVMBuildICmp(f->builder, LLVMIntSLT, idx, zero, "idx.neg");
  LLVMValueRef oob = LLVMBuildICmp(f->builder, LLVMIntSGE, idx, max, "idx.oob");
  LLVMValueRef bad = LLVMBuildOr(f->builder, neg, oob, "idx.bad");
  return emit_trap_if(f, bad);
}

static LLVMValueRef canonicalize_float_vec(FunctionCtx* f, LLVMValueRef v, TypeRec* vec_ty, TypeRec* lane_ty) {
  if (!f || !v || !vec_ty || !lane_ty) return NULL;
  if (!lane_is_float(lane_ty)) return v;
  if (vec_ty->lanes <= 0 || vec_ty->lanes > INT_MAX) return NULL;

  LLVMTypeRef lane_llvm = lower_type_prim(f->ctx, lane_ty->prim);
  if (!lane_llvm) return NULL;

  // For f32/f64, canonicalize lane-wise by extract/canon/insert.
  LLVMValueRef out = v;
  for (int i = 0; i < (int)vec_ty->lanes; i++) {
    LLVMValueRef idx = LLVMConstInt(LLVMInt32TypeInContext(f->ctx), (unsigned long long)i, 0);
    LLVMValueRef lane = LLVMBuildExtractElement(f->builder, out, idx, "lane");
    if (LLVMTypeOf(lane) != lane_llvm) lane = LLVMBuildBitCast(f->builder, lane, lane_llvm, "lane.cast");
    lane = canonicalize_float(f, lane);
    out = LLVMBuildInsertElement(f->builder, out, lane, idx, "lane.set");
  }
  return out;
}

bool lower_expr_simd(FunctionCtx* f, int64_t node_id, NodeRec* n, LLVMValueRef* outp) {
  if (!f || !n || !outp) return false;

  if (strcmp(n->tag, "vec.splat") == 0) {
    if (n->type_ref == 0) {
      LOWER_ERR_NODE(f, n, "sircc.vec.splat.missing_type", "sircc: vec.splat node %lld missing type_ref (vec type)", (long long)node_id);
      return false;
    }
    TypeRec* vec = NULL;
    TypeRec* lane = NULL;
    if (!is_vec_type(f->p, n->type_ref, &vec, &lane)) {
      LOWER_ERR_NODE(f, n, "sircc.vec.splat.type.bad", "sircc: vec.splat node %lld type_ref must be a vec type", (long long)node_id);
      return false;
    }
    if (!n->fields) {
      LOWER_ERR_NODE(f, n, "sircc.vec.splat.missing_fields", "sircc: vec.splat node %lld missing fields", (long long)node_id);
      return false;
    }
    JsonValue* args = json_obj_get(n->fields, "args");
    if (!args || args->type != JSON_ARRAY || args->v.arr.len != 1) {
      LOWER_ERR_NODE(f, n, "sircc.vec.splat.args.bad", "sircc: vec.splat node %lld requires args:[x]", (long long)node_id);
      return false;
    }
    int64_t xid = 0;
    if (!parse_node_ref_id(f->p, args->v.arr.items[0], &xid)) {
      LOWER_ERR_NODE(f, n, "sircc.vec.splat.args.ref_bad", "sircc: vec.splat node %lld args[0] must be a node ref", (long long)node_id);
      return false;
    }
    LLVMValueRef x = lower_expr(f, xid);
    if (!x) return false;

    LLVMTypeRef vec_llvm = lower_type(f->p, f->ctx, n->type_ref);
    if (!vec_llvm || LLVMGetTypeKind(vec_llvm) != LLVMVectorTypeKind) {
      LOWER_ERR_NODE(f, n, "sircc.vec.splat.llvm_type.bad", "sircc: vec.splat node %lld has non-vector LLVM type", (long long)node_id);
      return false;
    }

    LLVMTypeRef lane_llvm = lower_type_prim(f->ctx, lane->prim);
    if (!lane_llvm) {
      LOWER_ERR_NODE(f, n, "sircc.vec.lane.unsupported", "sircc: vec.splat lane type unsupported");
      return false;
    }

    LLVMValueRef lane_v = x;
    if (lane_is_bool(lane)) {
      lane_v = bool_to_i8(f, x);
      lane_llvm = LLVMInt8TypeInContext(f->ctx);
    } else {
      if (LLVMTypeOf(lane_v) != lane_llvm) lane_v = LLVMBuildTruncOrBitCast(f->builder, lane_v, lane_llvm, "lane.cast");
      if (LLVMGetTypeKind(lane_llvm) == LLVMFloatTypeKind || LLVMGetTypeKind(lane_llvm) == LLVMDoubleTypeKind) {
        lane_v = canonicalize_float(f, lane_v);
      }
    }

    LLVMValueRef out = LLVMGetUndef(vec_llvm);
    for (int i = 0; i < (int)vec->lanes; i++) {
      LLVMValueRef idx = LLVMConstInt(LLVMInt32TypeInContext(f->ctx), (unsigned long long)i, 0);
      out = LLVMBuildInsertElement(f->builder, out, lane_v, idx, "splat");
    }
    *outp = out;
    return true;
  }

  if (strcmp(n->tag, "vec.extract") == 0) {
    if (!n->fields) {
      LOWER_ERR_NODE(f, n, "sircc.vec.extract.missing_fields", "sircc: vec.extract node %lld missing fields", (long long)node_id);
      return false;
    }
    JsonValue* args = json_obj_get(n->fields, "args");
    if (!args || args->type != JSON_ARRAY || args->v.arr.len != 2) {
      LOWER_ERR_NODE(f, n, "sircc.vec.extract.args.bad", "sircc: vec.extract node %lld requires args:[v, idx]", (long long)node_id);
      return false;
    }
    int64_t vid = 0, idxid = 0;
    if (!parse_node_ref_id(f->p, args->v.arr.items[0], &vid) || !parse_node_ref_id(f->p, args->v.arr.items[1], &idxid)) {
      LOWER_ERR_NODE(f, n, "sircc.vec.extract.args.ref_bad", "sircc: vec.extract node %lld args must be node refs", (long long)node_id);
      return false;
    }
    NodeRec* vn = get_node(f->p, vid);
    if (!vn || vn->type_ref == 0) {
      LOWER_ERR_NODE(f, n, "sircc.vec.extract.v.missing_type", "sircc: vec.extract node %lld v must have a vec type_ref", (long long)node_id);
      return false;
    }
    TypeRec* vec = NULL;
    TypeRec* lane = NULL;
    if (!is_vec_type(f->p, vn->type_ref, &vec, &lane)) {
      LOWER_ERR_NODE(f, n, "sircc.vec.extract.v.type.bad", "sircc: vec.extract node %lld v must be a vec", (long long)node_id);
      return false;
    }
    LLVMValueRef v = lower_expr(f, vid);
    LLVMValueRef idx = lower_expr(f, idxid);
    if (!v || !idx) return false;

    if (!emit_vec_idx_bounds_check(f, node_id, idx, vec->lanes)) return false;

    LLVMValueRef lane_idx = idx;
    if (LLVMTypeOf(lane_idx) != LLVMInt32TypeInContext(f->ctx)) {
      lane_idx = LLVMBuildTruncOrBitCast(f->builder, lane_idx, LLVMInt32TypeInContext(f->ctx), "idx.i32");
    }
    LLVMValueRef el = LLVMBuildExtractElement(f->builder, v, lane_idx, "extract");
    if (lane_is_bool(lane)) {
      *outp = i8_to_bool(f, el);
    } else {
      LLVMTypeRef want = lower_type_prim(f->ctx, lane->prim);
      if (!want) {
        LOWER_ERR_NODE(f, n, "sircc.vec.lane.unsupported", "sircc: vec.extract lane type unsupported");
        return false;
      }
      if (LLVMTypeOf(el) != want) el = LLVMBuildBitCast(f->builder, el, want, "lane.cast");
      if (LLVMGetTypeKind(want) == LLVMFloatTypeKind || LLVMGetTypeKind(want) == LLVMDoubleTypeKind) el = canonicalize_float(f, el);
      *outp = el;
    }
    return true;
  }

  if (strcmp(n->tag, "vec.replace") == 0) {
    if (n->type_ref == 0) {
      LOWER_ERR_NODE(f, n, "sircc.vec.replace.missing_type", "sircc: vec.replace node %lld missing type_ref (vec type)", (long long)node_id);
      return false;
    }
    TypeRec* vec = NULL;
    TypeRec* lane = NULL;
    if (!is_vec_type(f->p, n->type_ref, &vec, &lane)) {
      LOWER_ERR_NODE(f, n, "sircc.vec.replace.type.bad", "sircc: vec.replace node %lld type_ref must be a vec type", (long long)node_id);
      return false;
    }
    if (!n->fields) {
      LOWER_ERR_NODE(f, n, "sircc.vec.replace.missing_fields", "sircc: vec.replace node %lld missing fields", (long long)node_id);
      return false;
    }
    JsonValue* args = json_obj_get(n->fields, "args");
    if (!args || args->type != JSON_ARRAY || args->v.arr.len != 3) {
      LOWER_ERR_NODE(f, n, "sircc.vec.replace.args.bad", "sircc: vec.replace node %lld requires args:[v, idx, x]", (long long)node_id);
      return false;
    }
    int64_t vid = 0, idxid = 0, xid = 0;
    if (!parse_node_ref_id(f->p, args->v.arr.items[0], &vid) || !parse_node_ref_id(f->p, args->v.arr.items[1], &idxid) ||
        !parse_node_ref_id(f->p, args->v.arr.items[2], &xid)) {
      LOWER_ERR_NODE(f, n, "sircc.vec.replace.args.ref_bad", "sircc: vec.replace node %lld args must be node refs", (long long)node_id);
      return false;
    }
    LLVMValueRef v = lower_expr(f, vid);
    LLVMValueRef idx = lower_expr(f, idxid);
    LLVMValueRef x = lower_expr(f, xid);
    if (!v || !idx || !x) return false;

    if (!emit_vec_idx_bounds_check(f, node_id, idx, vec->lanes)) return false;

    LLVMValueRef lane_idx = idx;
    if (LLVMTypeOf(lane_idx) != LLVMInt32TypeInContext(f->ctx)) {
      lane_idx = LLVMBuildTruncOrBitCast(f->builder, lane_idx, LLVMInt32TypeInContext(f->ctx), "idx.i32");
    }

    LLVMTypeRef want_lane = lower_type_prim(f->ctx, lane->prim);
    if (!want_lane) {
      LOWER_ERR_NODE(f, n, "sircc.vec.lane.unsupported", "sircc: vec.replace lane type unsupported");
      return false;
    }

    LLVMValueRef lane_x = x;
    if (lane_is_bool(lane)) {
      lane_x = bool_to_i8(f, x);
    } else {
      if (LLVMTypeOf(lane_x) != want_lane) lane_x = LLVMBuildTruncOrBitCast(f->builder, lane_x, want_lane, "lane.cast");
      if (LLVMGetTypeKind(want_lane) == LLVMFloatTypeKind || LLVMGetTypeKind(want_lane) == LLVMDoubleTypeKind) lane_x = canonicalize_float(f, lane_x);
    }

    LLVMValueRef out = LLVMBuildInsertElement(f->builder, v, lane_x, lane_idx, "replace");
    if (lane_is_float(lane)) out = canonicalize_float_vec(f, out, vec, lane);
    *outp = out;
    return true;
  }

  if (strcmp(n->tag, "load.vec") == 0) {
    if (n->type_ref == 0) {
      LOWER_ERR_NODE(f, n, "sircc.load.vec.missing_type", "sircc: load.vec node %lld missing type_ref (vec type)", (long long)node_id);
      return false;
    }
    TypeRec* vec = NULL;
    TypeRec* lane = NULL;
    if (!is_vec_type(f->p, n->type_ref, &vec, &lane)) {
      LOWER_ERR_NODE(f, n, "sircc.load.vec.type.bad", "sircc: load.vec node %lld type_ref must be a vec type", (long long)node_id);
      return false;
    }
    if (!n->fields) {
      LOWER_ERR_NODE(f, n, "sircc.load.vec.missing_fields", "sircc: load.vec node %lld missing fields", (long long)node_id);
      return false;
    }
    int64_t aid = 0;
    if (!parse_node_ref_id(f->p, json_obj_get(n->fields, "addr"), &aid)) {
      LOWER_ERR_NODE(f, n, "sircc.load.vec.addr.ref_bad", "sircc: load.vec node %lld missing fields.addr ref", (long long)node_id);
      return false;
    }
    LLVMValueRef pval = lower_expr(f, aid);
    if (!pval) return false;
    LLVMTypeRef pty = LLVMTypeOf(pval);
    if (LLVMGetTypeKind(pty) != LLVMPointerTypeKind) {
      LOWER_ERR_NODE(f, n, "sircc.load.vec.addr.not_ptr", "sircc: load.vec requires pointer addr");
      return false;
    }

    LLVMTypeRef vec_llvm = lower_type(f->p, f->ctx, n->type_ref);
    if (!vec_llvm) return false;
    LLVMTypeRef want_ptr = LLVMPointerType(vec_llvm, 0);
    if (want_ptr != pty) pval = LLVMBuildBitCast(f->builder, pval, want_ptr, "ldv.cast");

    unsigned align = 1;
    JsonValue* alignv = json_obj_get(n->fields, "align");
    if (alignv) {
      int64_t a = 0;
      if (!json_get_i64(alignv, &a) || a <= 0 || a > (int64_t)UINT_MAX) {
        LOWER_ERR_NODE(f, n, "sircc.load.vec.align.bad", "sircc: load.vec node %lld align must be a positive integer", (long long)node_id);
        return false;
      }
      align = (unsigned)a;
    }
    if ((align & (align - 1u)) != 0u) {
      LOWER_ERR_NODE(f, n, "sircc.load.vec.align.not_pow2", "sircc: load.vec node %lld align must be a power of two", (long long)node_id);
      return false;
    }
    if (!emit_trap_if_misaligned(f, pval, align)) return false;

    LLVMValueRef out = LLVMBuildLoad2(f->builder, vec_llvm, pval, "loadv");
    LLVMSetAlignment(out, align);
    JsonValue* volv = json_obj_get(n->fields, "vol");
    if (volv && volv->type == JSON_BOOL) LLVMSetVolatile(out, volv->v.b ? 1 : 0);
    out = canonicalize_float_vec(f, out, vec, lane);
    *outp = out;
    return true;
  }

  if (strcmp(n->tag, "vec.bitcast") == 0) {
    if (!n->fields) {
      LOWER_ERR_NODE(f, n, "sircc.vec.bitcast.missing_fields", "sircc: vec.bitcast node %lld missing fields", (long long)node_id);
      return false;
    }
    int64_t from_id = 0;
    int64_t to_id = 0;
    if (!parse_type_ref_id(f->p, json_obj_get(n->fields, "from"), &from_id) || !parse_type_ref_id(f->p, json_obj_get(n->fields, "to"), &to_id)) {
      LOWER_ERR_NODE(f, n, "sircc.vec.bitcast.from_to.bad", "sircc: vec.bitcast node %lld requires fields.from and fields.to type refs",
                     (long long)node_id);
      return false;
    }
    TypeRec* from_vec = NULL;
    TypeRec* from_lane = NULL;
    TypeRec* to_vec = NULL;
    TypeRec* to_lane = NULL;
    if (!is_vec_type(f->p, from_id, &from_vec, &from_lane) || !is_vec_type(f->p, to_id, &to_vec, &to_lane)) {
      LOWER_ERR_NODE(f, n, "sircc.vec.bitcast.type.bad", "sircc: vec.bitcast node %lld from/to must be vec types", (long long)node_id);
      return false;
    }
    JsonValue* args = json_obj_get(n->fields, "args");
    if (!args || args->type != JSON_ARRAY || args->v.arr.len != 1) {
      LOWER_ERR_NODE(f, n, "sircc.vec.bitcast.args.bad", "sircc: vec.bitcast node %lld requires args:[v]", (long long)node_id);
      return false;
    }
    int64_t vid = 0;
    if (!parse_node_ref_id(f->p, args->v.arr.items[0], &vid)) {
      LOWER_ERR_NODE(f, n, "sircc.vec.bitcast.args.ref_bad", "sircc: vec.bitcast node %lld args[0] must be a node ref", (long long)node_id);
      return false;
    }
    LLVMValueRef v = lower_expr(f, vid);
    if (!v) return false;

    int64_t from_sz = 0, from_al = 0;
    int64_t to_sz = 0, to_al = 0;
    if (!type_size_align(f->p, from_id, &from_sz, &from_al) || !type_size_align(f->p, to_id, &to_sz, &to_al) || from_sz != to_sz) {
      LOWER_ERR_NODE(f, n, "sircc.vec.bitcast.size_mismatch",
                     "sircc: vec.bitcast node %lld requires sizeof(from)==sizeof(to) (from=%lld, to=%lld)", (long long)node_id,
                     (long long)from_sz, (long long)to_sz);
      return false;
    }

    LLVMTypeRef to_llvm = lower_type(f->p, f->ctx, to_id);
    if (!to_llvm) return false;
    LLVMValueRef out = LLVMBuildBitCast(f->builder, v, to_llvm, "vcast");
    out = canonicalize_float_vec(f, out, to_vec, to_lane);
    *outp = out;
    return true;
  }

  LOWER_ERR_NODE(f, n, "sircc.vec.mnemonic.unhandled", "sircc: unhandled simd mnemonic '%s'", n->tag);
  return false;
}

bool lower_stmt_simd(FunctionCtx* f, int64_t node_id, NodeRec* n) {
  if (!f || !n) return false;

  if (strcmp(n->tag, "store.vec") == 0) {
    if (!n->fields) {
      LOWER_ERR_NODE(f, n, "sircc.store.vec.missing_fields", "sircc: store.vec node %lld missing fields", (long long)node_id);
      return false;
    }
    int64_t aid = 0, vid = 0;
    if (!parse_node_ref_id(f->p, json_obj_get(n->fields, "addr"), &aid) || !parse_node_ref_id(f->p, json_obj_get(n->fields, "value"), &vid)) {
      LOWER_ERR_NODE(f, n, "sircc.store.vec.addr_value.ref_bad", "sircc: store.vec node %lld requires fields.addr and fields.value refs",
                     (long long)node_id);
      return false;
    }

    int64_t vec_ty_id = 0;
    NodeRec* vn = get_node(f->p, vid);
    if (vn && vn->type_ref) vec_ty_id = vn->type_ref;
    if (vec_ty_id == 0) {
      parse_type_ref_id(f->p, json_obj_get(n->fields, "ty"), &vec_ty_id);
    }
    if (vec_ty_id == 0) {
      LOWER_ERR_NODE(f, n, "sircc.store.vec.missing_type", "sircc: store.vec node %lld requires a vec type (value.type_ref or fields.ty)",
                     (long long)node_id);
      return false;
    }

    TypeRec* vec = NULL;
    TypeRec* lane = NULL;
    if (!is_vec_type(f->p, vec_ty_id, &vec, &lane)) {
      LOWER_ERR_NODE(f, n, "sircc.store.vec.type.bad", "sircc: store.vec node %lld vec type must be kind:'vec'", (long long)node_id);
      return false;
    }

    LLVMValueRef pval = lower_expr(f, aid);
    LLVMValueRef vval = lower_expr(f, vid);
    if (!pval || !vval) return false;
    LLVMTypeRef pty = LLVMTypeOf(pval);
    if (LLVMGetTypeKind(pty) != LLVMPointerTypeKind) {
      LOWER_ERR_NODE(f, n, "sircc.store.vec.addr.not_ptr", "sircc: store.vec requires pointer addr");
      return false;
    }

    LLVMTypeRef vec_llvm = lower_type(f->p, f->ctx, vec_ty_id);
    if (!vec_llvm) return false;
    LLVMTypeRef want_ptr = LLVMPointerType(vec_llvm, 0);
    if (want_ptr != pty) pval = LLVMBuildBitCast(f->builder, pval, want_ptr, "stv.cast");

    unsigned align = 1;
    JsonValue* alignv = json_obj_get(n->fields, "align");
    if (alignv) {
      int64_t a = 0;
      if (!json_get_i64(alignv, &a) || a <= 0 || a > (int64_t)UINT_MAX) {
        LOWER_ERR_NODE(f, n, "sircc.store.vec.align.bad", "sircc: store.vec node %lld align must be a positive integer", (long long)node_id);
        return false;
      }
      align = (unsigned)a;
    }
    if ((align & (align - 1u)) != 0u) {
      LOWER_ERR_NODE(f, n, "sircc.store.vec.align.not_pow2", "sircc: store.vec node %lld align must be a power of two", (long long)node_id);
      return false;
    }
    if (!emit_trap_if_misaligned(f, pval, align)) return false;

    vval = canonicalize_float_vec(f, vval, vec, lane);

    LLVMValueRef st = LLVMBuildStore(f->builder, vval, pval);
    LLVMSetAlignment(st, align);
    JsonValue* volv = json_obj_get(n->fields, "vol");
    if (volv && volv->type == JSON_BOOL) LLVMSetVolatile(st, volv->v.b ? 1 : 0);
    return true;
  }

  LOWER_ERR_NODE(f, n, "sircc.simd.stmt.unhandled", "sircc: unhandled simd stmt '%s'", n->tag);
  return false;
}

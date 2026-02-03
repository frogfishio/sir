// SPDX-FileCopyrightText: 2026 Frogfish
// SPDX-License-Identifier: GPL-3.0-or-later

#include "compiler_lower_internal.h"

#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

LLVMValueRef lower_expr(FunctionCtx* f, int64_t node_id) {
  NodeRec* n = get_node(f->p, node_id);
  if (!n) {
    errf(f->p, "sircc: unknown node id %lld", (long long)node_id);
    return NULL;
  }
  if ((strcmp(n->tag, "param") == 0 || strcmp(n->tag, "bparam") == 0) && n->llvm_value) {
    return n->llvm_value;
  }
  if (n->llvm_value) return n->llvm_value;
  if (n->resolving) {
    errf(f->p, "sircc: cyclic node reference at %lld", (long long)node_id);
    return NULL;
  }
  n->resolving = true;

  LLVMValueRef out = NULL;

  if (strcmp(n->tag, "name") == 0) {
    const char* name = NULL;
    if (n->fields) name = json_get_string(json_obj_get(n->fields, "name"));
    if (!name) {
      errf(f->p, "sircc: name node %lld missing fields.name", (long long)node_id);
      goto done;
    }
    out = bind_get(f, name);
    if (!out) errf(f->p, "sircc: unknown name '%s' in node %lld", name, (long long)node_id);
    goto done;
  }

  if (strcmp(n->tag, "decl.fn") == 0) {
    if (!n->fields) {
      errf(f->p, "sircc: decl.fn node %lld missing fields", (long long)node_id);
      goto done;
    }
    const char* name = json_get_string(json_obj_get(n->fields, "name"));
    if (!name || !is_ident(name)) {
      errf(f->p, "sircc: decl.fn node %lld requires fields.name Ident", (long long)node_id);
      goto done;
    }

    int64_t sig_id = n->type_ref;
    if (sig_id == 0) {
      if (!parse_type_ref_id(json_obj_get(n->fields, "sig"), &sig_id)) {
        errf(f->p, "sircc: decl.fn node %lld requires type_ref or fields.sig (fn type ref)", (long long)node_id);
        goto done;
      }
    }
    LLVMTypeRef fnty = lower_type(f->p, f->ctx, sig_id);
    if (!fnty || LLVMGetTypeKind(fnty) != LLVMFunctionTypeKind) {
      errf(f->p, "sircc: decl.fn node %lld signature must be a fn type (type %lld)", (long long)node_id, (long long)sig_id);
      goto done;
    }

    LLVMValueRef fn = LLVMGetNamedFunction(f->mod, name);
    if (!fn) {
      fn = LLVMAddFunction(f->mod, name, fnty);
      LLVMSetLinkage(fn, LLVMExternalLinkage);
    } else {
      LLVMTypeRef have = LLVMGlobalGetValueType(fn);
      if (have != fnty) {
        errf(f->p, "sircc: decl.fn '%s' type mismatch vs existing declaration/definition", name);
        goto done;
      }
    }
    out = fn;
    goto done;
  }

  if (strcmp(n->tag, "cstr") == 0) {
    if (!n->fields) {
      errf(f->p, "sircc: cstr node %lld missing fields", (long long)node_id);
      goto done;
    }
    const char* s = json_get_string(json_obj_get(n->fields, "value"));
    if (!s) {
      errf(f->p, "sircc: cstr node %lld requires fields.value string", (long long)node_id);
      goto done;
    }

    size_t len = strlen(s);
    LLVMValueRef init = LLVMConstStringInContext2(f->ctx, s, len, 0);
    LLVMTypeRef aty = LLVMTypeOf(init); // [len+1 x i8]

    char gname[64];
    snprintf(gname, sizeof(gname), ".str.%lld", (long long)node_id);
    LLVMValueRef g = LLVMGetNamedGlobal(f->mod, gname);
    if (!g) {
      g = LLVMAddGlobal(f->mod, aty, gname);
      LLVMSetInitializer(g, init);
      LLVMSetGlobalConstant(g, 1);
      LLVMSetLinkage(g, LLVMPrivateLinkage);
      LLVMSetUnnamedAddress(g, LLVMGlobalUnnamedAddr);
      LLVMSetAlignment(g, 1);
    }

    LLVMTypeRef i32 = LLVMInt32TypeInContext(f->ctx);
    LLVMValueRef idxs[2] = {LLVMConstInt(i32, 0, 0), LLVMConstInt(i32, 0, 0)};
    LLVMValueRef p = LLVMBuildInBoundsGEP2(f->builder, aty, g, idxs, 2, "cstr");

    LLVMTypeRef i8p = LLVMPointerType(LLVMInt8TypeInContext(f->ctx), 0);
    out = LLVMBuildBitCast(f->builder, p, i8p, "cstr.ptr");
    goto done;
  }

  if (strcmp(n->tag, "binop.add") == 0) {
    JsonValue* lhs = n->fields ? json_obj_get(n->fields, "lhs") : NULL;
    JsonValue* rhs = n->fields ? json_obj_get(n->fields, "rhs") : NULL;
    int64_t lhs_id = 0, rhs_id = 0;
    if (!parse_node_ref_id(lhs, &lhs_id) || !parse_node_ref_id(rhs, &rhs_id)) {
      errf(f->p, "sircc: binop.add node %lld missing lhs/rhs refs", (long long)node_id);
      goto done;
    }
    LLVMValueRef a = lower_expr(f, lhs_id);
    LLVMValueRef b = lower_expr(f, rhs_id);
    if (!a || !b) goto done;
    LLVMTypeRef ty = LLVMTypeOf(a);
    if (LLVMGetTypeKind(ty) == LLVMIntegerTypeKind) {
      out = LLVMBuildAdd(f->builder, a, b, "add");
    } else {
      out = LLVMBuildFAdd(f->builder, a, b, "addf");
    }
    goto done;
  }

  if (strncmp(n->tag, "i", 1) == 0) {
    // Mnemonic-style integer ops: i8.add, i16.sub, i32.mul, etc.
    const char* dot = strchr(n->tag, '.');
    if (dot) {
      char wbuf[8];
      size_t wlen = (size_t)(dot - n->tag);
      if (wlen < sizeof(wbuf)) {
        memcpy(wbuf, n->tag, wlen);
        wbuf[wlen] = 0;
        int width = 0;
          if (sscanf(wbuf, "i%d", &width) == 1 && (width == 8 || width == 16 || width == 32 || width == 64)) {
            const char* op = dot + 1;
          JsonValue* args = n->fields ? json_obj_get(n->fields, "args") : NULL;
          int64_t a_id = 0, b_id = 0;
          // Extract operands.
          LLVMValueRef a = NULL;
          LLVMValueRef b = NULL;

          if (args && args->type == JSON_ARRAY) {
            if (args->v.arr.len == 1) {
              if (!parse_node_ref_id(args->v.arr.items[0], &a_id)) {
                errf(f->p, "sircc: %s node %lld args must be node refs", n->tag, (long long)node_id);
                goto done;
              }
              a = lower_expr(f, a_id);
              if (!a) goto done;
            } else if (args->v.arr.len == 2) {
              if (!parse_node_ref_id(args->v.arr.items[0], &a_id) || !parse_node_ref_id(args->v.arr.items[1], &b_id)) {
                errf(f->p, "sircc: %s node %lld args must be node refs", n->tag, (long long)node_id);
                goto done;
              }
              a = lower_expr(f, a_id);
              b = lower_expr(f, b_id);
              if (!a || !b) goto done;
            } else {
              errf(f->p, "sircc: %s node %lld args must have arity 1 or 2", n->tag, (long long)node_id);
              goto done;
            }
          } else {
            // Back-compat: allow lhs/rhs form for binary operators.
            JsonValue* lhs = n->fields ? json_obj_get(n->fields, "lhs") : NULL;
            JsonValue* rhs = n->fields ? json_obj_get(n->fields, "rhs") : NULL;
            if (parse_node_ref_id(lhs, &a_id) && parse_node_ref_id(rhs, &b_id)) {
              a = lower_expr(f, a_id);
              b = lower_expr(f, b_id);
              if (!a || !b) goto done;
            } else {
              errf(f->p, "sircc: %s node %lld missing args", n->tag, (long long)node_id);
              goto done;
            }
          }

          // Lower ops.
          if (strcmp(op, "add") == 0) {
            out = LLVMBuildAdd(f->builder, a, b, "iadd");
            goto done;
          }
          if (strcmp(op, "sub") == 0) {
            out = LLVMBuildSub(f->builder, a, b, "isub");
            goto done;
          }
          if (strcmp(op, "mul") == 0) {
            out = LLVMBuildMul(f->builder, a, b, "imul");
            goto done;
          }
          if (strcmp(op, "and") == 0) {
            out = LLVMBuildAnd(f->builder, a, b, "iand");
            goto done;
          }
          if (strcmp(op, "or") == 0) {
            out = LLVMBuildOr(f->builder, a, b, "ior");
            goto done;
          }
          if (strcmp(op, "xor") == 0) {
            out = LLVMBuildXor(f->builder, a, b, "ixor");
            goto done;
          }
          if (strcmp(op, "not") == 0) {
            out = LLVMBuildNot(f->builder, a, "inot");
            goto done;
          }
          if (strcmp(op, "neg") == 0) {
            out = LLVMBuildNeg(f->builder, a, "ineg");
            goto done;
          }
          if (strcmp(op, "eqz") == 0) {
            if (b) {
              errf(f->p, "sircc: %s node %lld requires 1 arg", n->tag, (long long)node_id);
              goto done;
            }
            LLVMTypeRef aty = LLVMTypeOf(a);
            if (LLVMGetTypeKind(aty) != LLVMIntegerTypeKind || LLVMGetIntTypeWidth(aty) != (unsigned)width) {
              errf(f->p, "sircc: %s requires i%d operand", n->tag, width);
              goto done;
            }
            LLVMValueRef zero = LLVMConstInt(aty, 0, 0);
            out = LLVMBuildICmp(f->builder, LLVMIntEQ, a, zero, "eqz");
            goto done;
          }
          if (strcmp(op, "min.s") == 0 || strcmp(op, "min.u") == 0 || strcmp(op, "max.s") == 0 || strcmp(op, "max.u") == 0) {
            if (!b) {
              errf(f->p, "sircc: %s node %lld requires 2 args", n->tag, (long long)node_id);
              goto done;
            }
            LLVMTypeRef aty = LLVMTypeOf(a);
            LLVMTypeRef bty = LLVMTypeOf(b);
            if (LLVMGetTypeKind(aty) != LLVMIntegerTypeKind || LLVMGetTypeKind(bty) != LLVMIntegerTypeKind ||
                LLVMGetIntTypeWidth(aty) != (unsigned)width || LLVMGetIntTypeWidth(bty) != (unsigned)width) {
              errf(f->p, "sircc: %s requires i%d operands", n->tag, width);
              goto done;
            }
            bool is_min = (strncmp(op, "min.", 4) == 0);
            bool is_signed = (op[4] == 's');
            LLVMIntPredicate pred;
            if (is_min) pred = is_signed ? LLVMIntSLE : LLVMIntULE;
            else pred = is_signed ? LLVMIntSGE : LLVMIntUGE;
            LLVMValueRef cmp = LLVMBuildICmp(f->builder, pred, a, b, "minmax.cmp");
            out = LLVMBuildSelect(f->builder, cmp, a, b, "minmax");
            goto done;
          }
          if (strcmp(op, "shl") == 0 || strcmp(op, "shr.s") == 0 || strcmp(op, "shr.u") == 0) {
            if (!b) {
              errf(f->p, "sircc: %s node %lld requires 2 args", n->tag, (long long)node_id);
              goto done;
            }
            LLVMTypeRef xty = LLVMTypeOf(a);
            if (LLVMGetTypeKind(xty) != LLVMIntegerTypeKind) {
              errf(f->p, "sircc: %s node %lld requires integer lhs", n->tag, (long long)node_id);
              goto done;
            }

            LLVMTypeRef sty = LLVMTypeOf(b);
            if (LLVMGetTypeKind(sty) != LLVMIntegerTypeKind) {
              errf(f->p, "sircc: %s node %lld requires integer shift amount", n->tag, (long long)node_id);
              goto done;
            }

            LLVMValueRef shift = b;
            if (LLVMGetIntTypeWidth(sty) != LLVMGetIntTypeWidth(xty)) {
              shift = build_zext_or_trunc(f->builder, b, xty, "shift.cast");
            }
            unsigned mask = (unsigned)(width - 1);
            LLVMValueRef maskv = LLVMConstInt(xty, mask, 0);
            shift = LLVMBuildAnd(f->builder, shift, maskv, "shift.mask");

            if (strcmp(op, "shl") == 0) {
              out = LLVMBuildShl(f->builder, a, shift, "shl");
              goto done;
            }
            if (strcmp(op, "shr.s") == 0) {
              out = LLVMBuildAShr(f->builder, a, shift, "ashr");
              goto done;
            }
            out = LLVMBuildLShr(f->builder, a, shift, "lshr");
            goto done;
          }

          if (strcmp(op, "div.s.trap") == 0 || strcmp(op, "div.u.trap") == 0 || strcmp(op, "rem.s.trap") == 0 ||
              strcmp(op, "rem.u.trap") == 0) {
            if (!b) {
              errf(f->p, "sircc: %s node %lld requires 2 args", n->tag, (long long)node_id);
              goto done;
            }
            LLVMTypeRef aty = LLVMTypeOf(a);
            LLVMTypeRef bty = LLVMTypeOf(b);
            if (LLVMGetTypeKind(aty) != LLVMIntegerTypeKind || LLVMGetTypeKind(bty) != LLVMIntegerTypeKind ||
                LLVMGetIntTypeWidth(aty) != (unsigned)width || LLVMGetIntTypeWidth(bty) != (unsigned)width) {
              errf(f->p, "sircc: %s requires i%d operands", n->tag, width);
              goto done;
            }
            LLVMValueRef zero = LLVMConstInt(aty, 0, 0);
            LLVMValueRef b_is_zero = LLVMBuildICmp(f->builder, LLVMIntEQ, b, zero, "b.iszero");
            LLVMValueRef trap_cond = b_is_zero;

            bool is_div = (strncmp(op, "div.", 4) == 0);
            bool is_signed = (op[4] == 's');
            if (is_div && is_signed) {
              unsigned long long min_bits = 1ULL << (unsigned)(width - 1);
              LLVMValueRef minv = LLVMConstInt(aty, min_bits, 0);
              LLVMValueRef neg1 = LLVMConstAllOnes(aty);
              LLVMValueRef a_is_min = LLVMBuildICmp(f->builder, LLVMIntEQ, a, minv, "a.ismin");
              LLVMValueRef b_is_neg1 = LLVMBuildICmp(f->builder, LLVMIntEQ, b, neg1, "b.isneg1");
              LLVMValueRef ov = LLVMBuildAnd(f->builder, a_is_min, b_is_neg1, "div.ov");
              trap_cond = LLVMBuildOr(f->builder, trap_cond, ov, "trap.cond");
            }
            if (!emit_trap_if(f, trap_cond)) goto done;

            if (is_div) {
              out = is_signed ? LLVMBuildSDiv(f->builder, a, b, "div") : LLVMBuildUDiv(f->builder, a, b, "div");
            } else {
              out = is_signed ? LLVMBuildSRem(f->builder, a, b, "rem") : LLVMBuildURem(f->builder, a, b, "rem");
            }
            goto done;
          }

          if (strncmp(op, "trunc_sat_f", 11) == 0) {
            // iN.trunc_sat_f32.s / iN.trunc_sat_f32.u (and f64.*)
            if (!args || args->type != JSON_ARRAY || args->v.arr.len != 1) {
              errf(f->p, "sircc: %s node %lld requires args:[x]", n->tag, (long long)node_id);
              goto done;
            }
            int srcw = 0;
            char su = 0;
            if (sscanf(op, "trunc_sat_f%d.%c", &srcw, &su) != 2 || (srcw != 32 && srcw != 64) || (su != 's' && su != 'u')) {
              errf(f->p, "sircc: unsupported trunc_sat form '%s' in %s", op, n->tag);
              goto done;
            }
            int64_t x_id = 0;
            if (!parse_node_ref_id(args->v.arr.items[0], &x_id)) {
              errf(f->p, "sircc: %s node %lld arg must be node ref", n->tag, (long long)node_id);
              goto done;
            }
            LLVMValueRef x = lower_expr(f, x_id);
            if (!x) goto done;

            LLVMTypeRef ity = LLVMIntTypeInContext(f->ctx, (unsigned)width);
            LLVMTypeRef fty = (srcw == 32) ? LLVMFloatTypeInContext(f->ctx) : LLVMDoubleTypeInContext(f->ctx);
            if (LLVMTypeOf(x) != fty) {
              errf(f->p, "sircc: %s requires f%d operand", n->tag, srcw);
              goto done;
            }
            if (LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(f->builder))) goto done;

            LLVMBasicBlockRef bb_nan = LLVMAppendBasicBlockInContext(f->ctx, f->fn, "sat.nan");
            LLVMBasicBlockRef bb_chk1 = LLVMAppendBasicBlockInContext(f->ctx, f->fn, "sat.chk1");
            LLVMBasicBlockRef bb_min = LLVMAppendBasicBlockInContext(f->ctx, f->fn, "sat.min");
            LLVMBasicBlockRef bb_chk2 = LLVMAppendBasicBlockInContext(f->ctx, f->fn, "sat.chk2");
            LLVMBasicBlockRef bb_max = LLVMAppendBasicBlockInContext(f->ctx, f->fn, "sat.max");
            LLVMBasicBlockRef bb_conv = LLVMAppendBasicBlockInContext(f->ctx, f->fn, "sat.conv");
            LLVMBasicBlockRef bb_merge = LLVMAppendBasicBlockInContext(f->ctx, f->fn, "sat.merge");

            LLVMValueRef isnan = LLVMBuildFCmp(f->builder, LLVMRealUNO, x, x, "isnan");
            LLVMBuildCondBr(f->builder, isnan, bb_nan, bb_chk1);

            LLVMPositionBuilderAtEnd(f->builder, bb_nan);
            LLVMValueRef z = LLVMConstInt(ity, 0, 0);
            LLVMBuildBr(f->builder, bb_merge);

            LLVMPositionBuilderAtEnd(f->builder, bb_chk1);
            LLVMValueRef min_i = NULL;
            LLVMValueRef max_i = NULL;
            if (su == 's') {
              unsigned long long min_bits = 1ULL << (unsigned)(width - 1);
              min_i = LLVMConstInt(ity, min_bits, 0);
              max_i = LLVMConstInt(ity, min_bits - 1ULL, 0);
              LLVMValueRef min_f = LLVMBuildSIToFP(f->builder, min_i, fty, "min.f");
              LLVMValueRef too_low = LLVMBuildFCmp(f->builder, LLVMRealOLT, x, min_f, "too_low");
              LLVMBuildCondBr(f->builder, too_low, bb_min, bb_chk2);
            } else {
              min_i = LLVMConstInt(ity, 0, 0);
              max_i = LLVMConstAllOnes(ity);
              LLVMValueRef zf = LLVMConstReal(fty, 0.0);
              LLVMValueRef too_low = LLVMBuildFCmp(f->builder, LLVMRealOLE, x, zf, "too_low");
              LLVMBuildCondBr(f->builder, too_low, bb_min, bb_chk2);
            }

            LLVMPositionBuilderAtEnd(f->builder, bb_min);
            LLVMBuildBr(f->builder, bb_merge);

            LLVMPositionBuilderAtEnd(f->builder, bb_chk2);
            LLVMValueRef max_f = (su == 's') ? LLVMBuildSIToFP(f->builder, max_i, fty, "max.f") : LLVMBuildUIToFP(f->builder, max_i, fty, "max.f");
            LLVMValueRef too_high = LLVMBuildFCmp(f->builder, LLVMRealOGE, x, max_f, "too_high");
            LLVMBuildCondBr(f->builder, too_high, bb_max, bb_conv);

            LLVMPositionBuilderAtEnd(f->builder, bb_max);
            LLVMBuildBr(f->builder, bb_merge);

            LLVMPositionBuilderAtEnd(f->builder, bb_conv);
            LLVMValueRef conv = (su == 's') ? LLVMBuildFPToSI(f->builder, x, ity, "fptosi") : LLVMBuildFPToUI(f->builder, x, ity, "fptoui");
            LLVMBuildBr(f->builder, bb_merge);

            LLVMPositionBuilderAtEnd(f->builder, bb_merge);
            LLVMValueRef phi = LLVMBuildPhi(f->builder, ity, "trunc_sat");
            LLVMValueRef inc_vals[4] = {z, min_i, max_i, conv};
            LLVMBasicBlockRef inc_bbs[4] = {bb_nan, bb_min, bb_max, bb_conv};
            LLVMAddIncoming(phi, inc_vals, inc_bbs, 4);
            out = phi;
            goto done;
          }

          if (strcmp(op, "div.s.sat") == 0 || strcmp(op, "div.u.sat") == 0 || strcmp(op, "rem.s.sat") == 0 ||
              strcmp(op, "rem.u.sat") == 0) {
            if (!b) {
              errf(f->p, "sircc: %s node %lld requires 2 args", n->tag, (long long)node_id);
              goto done;
            }
            LLVMTypeRef aty = LLVMTypeOf(a);
            LLVMTypeRef bty = LLVMTypeOf(b);
            if (LLVMGetTypeKind(aty) != LLVMIntegerTypeKind || LLVMGetTypeKind(bty) != LLVMIntegerTypeKind ||
                LLVMGetIntTypeWidth(aty) != (unsigned)width || LLVMGetIntTypeWidth(bty) != (unsigned)width) {
              errf(f->p, "sircc: %s requires i%d operands", n->tag, width);
              goto done;
            }
            if (LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(f->builder))) goto done;

            bool is_div = (strncmp(op, "div.", 4) == 0);
            bool is_signed = (op[4] == 's');

            LLVMBasicBlockRef cur = LLVMGetInsertBlock(f->builder);
            LLVMBasicBlockRef bb_zero = LLVMAppendBasicBlockInContext(f->ctx, f->fn, "sat.zero");
            LLVMBasicBlockRef bb_chk = LLVMAppendBasicBlockInContext(f->ctx, f->fn, "sat.chk");
            LLVMBasicBlockRef bb_norm = LLVMAppendBasicBlockInContext(f->ctx, f->fn, "sat.norm");
            LLVMBasicBlockRef bb_over = NULL;
            LLVMBasicBlockRef bb_merge = LLVMAppendBasicBlockInContext(f->ctx, f->fn, "sat.merge");

            LLVMValueRef zero = LLVMConstInt(aty, 0, 0);
            LLVMValueRef b_is_zero = LLVMBuildICmp(f->builder, LLVMIntEQ, b, zero, "b.iszero");
            LLVMBuildCondBr(f->builder, b_is_zero, bb_zero, bb_chk);

            // b==0 case: result 0
            LLVMPositionBuilderAtEnd(f->builder, bb_zero);
            LLVMBuildBr(f->builder, bb_merge);

            // check overflow (signed div only), otherwise jump to normal
            LLVMPositionBuilderAtEnd(f->builder, bb_chk);
            if (is_div && is_signed) {
              bb_over = LLVMAppendBasicBlockInContext(f->ctx, f->fn, "sat.over");
              unsigned long long min_bits = 1ULL << (unsigned)(width - 1);
              LLVMValueRef minv = LLVMConstInt(aty, min_bits, 0);
              LLVMValueRef neg1 = LLVMConstAllOnes(aty);
              LLVMValueRef a_is_min = LLVMBuildICmp(f->builder, LLVMIntEQ, a, minv, "a.ismin");
              LLVMValueRef b_is_neg1 = LLVMBuildICmp(f->builder, LLVMIntEQ, b, neg1, "b.isneg1");
              LLVMValueRef ov = LLVMBuildAnd(f->builder, a_is_min, b_is_neg1, "div.ov");
              LLVMBuildCondBr(f->builder, ov, bb_over, bb_norm);

              LLVMPositionBuilderAtEnd(f->builder, bb_over);
              LLVMBuildBr(f->builder, bb_merge);
            } else {
              LLVMBuildBr(f->builder, bb_norm);
            }

            // normal division/rem
            LLVMPositionBuilderAtEnd(f->builder, bb_norm);
            LLVMValueRef norm = NULL;
            if (is_div) {
              norm = is_signed ? LLVMBuildSDiv(f->builder, a, b, "div") : LLVMBuildUDiv(f->builder, a, b, "div");
            } else {
              norm = is_signed ? LLVMBuildSRem(f->builder, a, b, "rem") : LLVMBuildURem(f->builder, a, b, "rem");
            }
            LLVMBuildBr(f->builder, bb_merge);

            // merge
            LLVMPositionBuilderAtEnd(f->builder, bb_merge);
            LLVMValueRef phi = LLVMBuildPhi(f->builder, aty, "sat");
            LLVMValueRef inc_vals[3];
            LLVMBasicBlockRef inc_bbs[3];
            unsigned inc_n = 0;
            inc_vals[inc_n] = zero;
            inc_bbs[inc_n] = bb_zero;
            inc_n++;
            if (bb_over) {
              unsigned long long min_bits = 1ULL << (unsigned)(width - 1);
              LLVMValueRef minv = LLVMConstInt(aty, min_bits, 0);
              inc_vals[inc_n] = minv;
              inc_bbs[inc_n] = bb_over;
              inc_n++;
            }
            inc_vals[inc_n] = norm;
            inc_bbs[inc_n] = bb_norm;
            inc_n++;
            LLVMAddIncoming(phi, inc_vals, inc_bbs, inc_n);
            (void)cur;
            out = phi;
            goto done;
          }

          if (strcmp(op, "rotl") == 0 || strcmp(op, "rotr") == 0) {
            if (!b) {
              errf(f->p, "sircc: %s node %lld requires 2 args", n->tag, (long long)node_id);
              goto done;
            }
            LLVMTypeRef xty = LLVMTypeOf(a);
            if (LLVMGetTypeKind(xty) != LLVMIntegerTypeKind) {
              errf(f->p, "sircc: %s node %lld requires integer lhs", n->tag, (long long)node_id);
              goto done;
            }
            LLVMTypeRef sty = LLVMTypeOf(b);
            if (LLVMGetTypeKind(sty) != LLVMIntegerTypeKind) {
              errf(f->p, "sircc: %s node %lld requires integer rotate amount", n->tag, (long long)node_id);
              goto done;
            }
            LLVMValueRef amt = b;
            if (LLVMGetIntTypeWidth(sty) != LLVMGetIntTypeWidth(xty)) {
              amt = build_zext_or_trunc(f->builder, b, xty, "rot.cast");
            }
            unsigned mask = (unsigned)(width - 1);
            LLVMValueRef maskv = LLVMConstInt(xty, mask, 0);
            amt = LLVMBuildAnd(f->builder, amt, maskv, "rot.mask");

            char full[32];
            snprintf(full, sizeof(full), "llvm.%s.i%d", (strcmp(op, "rotl") == 0) ? "fshl" : "fshr", width);
            LLVMTypeRef params[3] = {xty, xty, xty};
            LLVMValueRef fn = get_or_declare_intrinsic(f->mod, full, xty, params, 3);
            LLVMValueRef argv[3] = {a, a, amt};
            out = LLVMBuildCall2(f->builder, LLVMGlobalGetValueType(fn), fn, argv, 3, "rot");
            goto done;
          }

          if (strncmp(op, "cmp.", 4) == 0) {
            if (!b) {
              errf(f->p, "sircc: %s node %lld requires 2 args", n->tag, (long long)node_id);
              goto done;
            }
            const char* cc = op + 4;
            LLVMIntPredicate pred;
            if (strcmp(cc, "eq") == 0) pred = LLVMIntEQ;
            else if (strcmp(cc, "ne") == 0) pred = LLVMIntNE;
            else if (strcmp(cc, "slt") == 0) pred = LLVMIntSLT;
            else if (strcmp(cc, "sle") == 0) pred = LLVMIntSLE;
            else if (strcmp(cc, "sgt") == 0) pred = LLVMIntSGT;
            else if (strcmp(cc, "sge") == 0) pred = LLVMIntSGE;
            else if (strcmp(cc, "ult") == 0) pred = LLVMIntULT;
            else if (strcmp(cc, "ule") == 0) pred = LLVMIntULE;
            else if (strcmp(cc, "ugt") == 0) pred = LLVMIntUGT;
            else if (strcmp(cc, "uge") == 0) pred = LLVMIntUGE;
            else {
              errf(f->p, "sircc: unsupported integer compare '%s' in %s", cc, n->tag);
              goto done;
            }
            out = LLVMBuildICmp(f->builder, pred, a, b, "icmp");
            goto done;
          }

          if (strcmp(op, "clz") == 0 || strcmp(op, "ctz") == 0) {
            const char* iname = (strcmp(op, "clz") == 0) ? "llvm.ctlz" : "llvm.cttz";
            char full[32];
            snprintf(full, sizeof(full), "%s.i%d", iname, width);
            LLVMTypeRef ity = LLVMTypeOf(a);
            LLVMTypeRef i1 = LLVMInt1TypeInContext(f->ctx);
            LLVMTypeRef params[2] = {ity, i1};
            LLVMValueRef fn = get_or_declare_intrinsic(f->mod, full, ity, params, 2);
            LLVMValueRef argsv[2] = {a, LLVMConstInt(i1, 0, 0)};
            out = LLVMBuildCall2(f->builder, LLVMGlobalGetValueType(fn), fn, argsv, 2, op);
            goto done;
          }

          if (strcmp(op, "popc") == 0) {
            char full[32];
            snprintf(full, sizeof(full), "llvm.ctpop.i%d", width);
            LLVMTypeRef ity = LLVMTypeOf(a);
            LLVMTypeRef params[1] = {ity};
            LLVMValueRef fn = get_or_declare_intrinsic(f->mod, full, ity, params, 1);
            LLVMValueRef argsv[1] = {a};
            out = LLVMBuildCall2(f->builder, LLVMGlobalGetValueType(fn), fn, argsv, 1, "popc");
            goto done;
          }

          if (strncmp(op, "zext.i", 6) == 0 || strncmp(op, "sext.i", 6) == 0 || strncmp(op, "trunc.i", 7) == 0) {
            int src = 0;
            bool is_zext = strncmp(op, "zext.i", 6) == 0;
            bool is_sext = strncmp(op, "sext.i", 6) == 0;
            bool is_trunc = strncmp(op, "trunc.i", 7) == 0;
            const char* num = is_trunc ? (op + 7) : (op + 6);
            if (sscanf(num, "%d", &src) != 1 || !(src == 8 || src == 16 || src == 32 || src == 64)) {
              errf(f->p, "sircc: invalid cast mnemonic '%s'", n->tag);
              goto done;
            }

            if ((is_zext || is_sext) && width <= src) {
              errf(f->p, "sircc: %s requires dst width > src width", n->tag);
              goto done;
            }
            if (is_trunc && width >= src) {
              errf(f->p, "sircc: %s requires dst width < src width", n->tag);
              goto done;
            }

            LLVMTypeRef ity = LLVMTypeOf(a);
            if (LLVMGetTypeKind(ity) != LLVMIntegerTypeKind || (int)LLVMGetIntTypeWidth(ity) != src) {
              errf(f->p, "sircc: %s requires i%d operand", n->tag, src);
              goto done;
            }
            LLVMTypeRef dst = LLVMIntTypeInContext(f->ctx, (unsigned)width);
            if (is_zext) out = LLVMBuildZExt(f->builder, a, dst, "zext");
            else if (is_sext) out = LLVMBuildSExt(f->builder, a, dst, "sext");
            else out = LLVMBuildTrunc(f->builder, a, dst, "trunc");
            goto done;
          }
        }
      }
    }
  }

  if (strncmp(n->tag, "bool.", 5) == 0) {
    const char* op = n->tag + 5;
    JsonValue* args = n->fields ? json_obj_get(n->fields, "args") : NULL;
    if (!args || args->type != JSON_ARRAY) {
      errf(f->p, "sircc: %s node %lld missing args array", n->tag, (long long)node_id);
      goto done;
    }

    if (strcmp(op, "not") == 0) {
      if (args->v.arr.len != 1) {
        errf(f->p, "sircc: bool.not node %lld requires 1 arg", (long long)node_id);
        goto done;
      }
      int64_t x_id = 0;
      if (!parse_node_ref_id(args->v.arr.items[0], &x_id)) {
        errf(f->p, "sircc: bool.not node %lld arg must be node ref", (long long)node_id);
        goto done;
      }
      LLVMValueRef x = lower_expr(f, x_id);
      if (!x) goto done;
      out = LLVMBuildNot(f->builder, x, "bnot");
      goto done;
    }

    if (strcmp(op, "and") == 0 || strcmp(op, "or") == 0 || strcmp(op, "xor") == 0) {
      if (args->v.arr.len != 2) {
        errf(f->p, "sircc: bool.%s node %lld requires 2 args", op, (long long)node_id);
        goto done;
      }
      int64_t a_id = 0, b_id = 0;
      if (!parse_node_ref_id(args->v.arr.items[0], &a_id) || !parse_node_ref_id(args->v.arr.items[1], &b_id)) {
        errf(f->p, "sircc: bool.%s node %lld args must be node refs", op, (long long)node_id);
        goto done;
      }
      LLVMValueRef a = lower_expr(f, a_id);
      LLVMValueRef b = lower_expr(f, b_id);
      if (!a || !b) goto done;
      if (strcmp(op, "and") == 0) out = LLVMBuildAnd(f->builder, a, b, "band");
      else if (strcmp(op, "or") == 0) out = LLVMBuildOr(f->builder, a, b, "bor");
      else out = LLVMBuildXor(f->builder, a, b, "bxor");
      goto done;
    }
  }

  if (strcmp(n->tag, "select") == 0) {
    JsonValue* args = n->fields ? json_obj_get(n->fields, "args") : NULL;
    if (!args || args->type != JSON_ARRAY || args->v.arr.len != 3) {
      errf(f->p, "sircc: select node %lld requires args:[cond, then, else]", (long long)node_id);
      goto done;
    }
    int64_t ty_id = 0;
    bool has_ty = false;
    if (n->fields) {
      JsonValue* tyv = json_obj_get(n->fields, "ty");
      if (tyv && parse_type_ref_id(tyv, &ty_id)) has_ty = true;
    }
    int64_t c_id = 0, t_id = 0, e_id = 0;
    if (!parse_node_ref_id(args->v.arr.items[0], &c_id) || !parse_node_ref_id(args->v.arr.items[1], &t_id) ||
        !parse_node_ref_id(args->v.arr.items[2], &e_id)) {
      errf(f->p, "sircc: select node %lld args must be node refs", (long long)node_id);
      goto done;
    }
    LLVMValueRef c = lower_expr(f, c_id);
    LLVMValueRef tv = lower_expr(f, t_id);
    LLVMValueRef ev = lower_expr(f, e_id);
    if (!c || !tv || !ev) goto done;
    if (LLVMGetTypeKind(LLVMTypeOf(c)) != LLVMIntegerTypeKind || LLVMGetIntTypeWidth(LLVMTypeOf(c)) != 1) {
      errf(f->p, "sircc: select node %lld cond must be bool", (long long)node_id);
      goto done;
    }
    if (LLVMTypeOf(tv) != LLVMTypeOf(ev)) {
      errf(f->p, "sircc: select node %lld then/else types must match", (long long)node_id);
      goto done;
    }
    if (n->type_ref) {
      LLVMTypeRef want = lower_type(f->p, f->ctx, n->type_ref);
      if (!want || want != LLVMTypeOf(tv)) {
        errf(f->p, "sircc: select node %lld type_ref does not match operand type", (long long)node_id);
        goto done;
      }
    }
    if (has_ty) {
      LLVMTypeRef want = lower_type(f->p, f->ctx, ty_id);
      if (!want || want != LLVMTypeOf(tv)) {
        errf(f->p, "sircc: select node %lld ty does not match operand type", (long long)node_id);
        goto done;
      }
    }
    out = LLVMBuildSelect(f->builder, c, tv, ev, "select");
    goto done;
  }

  if (strcmp(n->tag, "call") == 0) {
    if (!n->fields) {
      errf(f->p, "sircc: call node %lld missing fields", (long long)node_id);
      goto done;
    }
    JsonValue* callee_v = json_obj_get(n->fields, "callee");
    int64_t callee_id = 0;
    if (!parse_node_ref_id(callee_v, &callee_id)) {
      errf(f->p, "sircc: call node %lld missing callee ref", (long long)node_id);
      goto done;
    }
    NodeRec* callee_n = get_node(f->p, callee_id);
    if (!callee_n || strcmp(callee_n->tag, "fn") != 0 || !callee_n->llvm_value) {
      errf(f->p, "sircc: call node %lld callee %lld is not a lowered fn", (long long)node_id, (long long)callee_id);
      goto done;
    }
    LLVMValueRef callee = callee_n->llvm_value;

    JsonValue* args = json_obj_get(n->fields, "args");
    if (!args || args->type != JSON_ARRAY) {
      errf(f->p, "sircc: call node %lld missing args array", (long long)node_id);
      goto done;
    }
    size_t argc = args->v.arr.len;
    LLVMValueRef* argv = NULL;
    if (argc) {
      argv = (LLVMValueRef*)malloc(argc * sizeof(LLVMValueRef));
      if (!argv) goto done;
      for (size_t i = 0; i < argc; i++) {
        int64_t aid = 0;
        if (!parse_node_ref_id(args->v.arr.items[i], &aid)) {
          errf(f->p, "sircc: call node %lld arg[%zu] must be node ref", (long long)node_id, i);
          free(argv);
          goto done;
        }
        argv[i] = lower_expr(f, aid);
        if (!argv[i]) {
          free(argv);
          goto done;
        }
      }
    }

    LLVMTypeRef callee_fty = LLVMGlobalGetValueType(callee);
    if (LLVMGetTypeKind(callee_fty) != LLVMFunctionTypeKind) {
      errf(f->p, "sircc: call node %lld callee is not a function pointer", (long long)node_id);
      free(argv);
      goto done;
    }

    unsigned param_count = LLVMCountParamTypes(callee_fty);
    bool is_varargs = LLVMIsFunctionVarArg(callee_fty) != 0;
    if (!is_varargs && (unsigned)argc != param_count) {
      errf(f->p, "sircc: call node %lld arg count mismatch (got %zu, want %u)", (long long)node_id, argc, param_count);
      free(argv);
      goto done;
    }
    if ((unsigned)argc < param_count) {
      errf(f->p, "sircc: call node %lld missing required args (got %zu, want >= %u)", (long long)node_id, argc, param_count);
      free(argv);
      goto done;
    }

    if (param_count) {
      LLVMTypeRef* params = (LLVMTypeRef*)malloc(param_count * sizeof(LLVMTypeRef));
      if (!params) {
        free(argv);
        goto done;
      }
      LLVMGetParamTypes(callee_fty, params);
      for (unsigned i = 0; i < param_count; i++) {
        LLVMTypeRef want = params[i];
        LLVMTypeRef got = LLVMTypeOf(argv[i]);
        if (want == got) continue;
        if (LLVMGetTypeKind(want) == LLVMPointerTypeKind && LLVMGetTypeKind(got) == LLVMPointerTypeKind) {
          argv[i] = LLVMBuildBitCast(f->builder, argv[i], want, "arg.cast");
          continue;
        }
        free(params);
        errf(f->p, "sircc: call node %lld arg[%u] type mismatch", (long long)node_id, i);
        free(argv);
        goto done;
      }
      free(params);
    }

    out = LLVMBuildCall2(f->builder, callee_fty, callee, argv, (unsigned)argc, "call");
    free(argv);
    if (out && n->type_ref) {
      LLVMTypeRef want = lower_type(f->p, f->ctx, n->type_ref);
      if (want && want != LLVMTypeOf(out)) {
        errf(f->p, "sircc: call node %lld return type does not match type_ref", (long long)node_id);
        out = NULL;
        goto done;
      }
    }
    goto done;
  }

  if (strcmp(n->tag, "call.indirect") == 0) {
    if (!n->fields) {
      errf(f->p, "sircc: call.indirect node %lld missing fields", (long long)node_id);
      goto done;
    }

    int64_t sig_id = 0;
    if (!parse_type_ref_id(json_obj_get(n->fields, "sig"), &sig_id)) {
      errf(f->p, "sircc: call.indirect node %lld missing fields.sig (fn type ref)", (long long)node_id);
      goto done;
    }
    LLVMTypeRef callee_fty = lower_type(f->p, f->ctx, sig_id);
    if (!callee_fty || LLVMGetTypeKind(callee_fty) != LLVMFunctionTypeKind) {
      errf(f->p, "sircc: call.indirect node %lld fields.sig must reference a fn type", (long long)node_id);
      goto done;
    }

    JsonValue* args = json_obj_get(n->fields, "args");
    if (!args || args->type != JSON_ARRAY || args->v.arr.len < 1) {
      errf(f->p, "sircc: call.indirect node %lld requires args:[callee_ptr, ...]", (long long)node_id);
      goto done;
    }

    int64_t callee_id = 0;
    if (!parse_node_ref_id(args->v.arr.items[0], &callee_id)) {
      errf(f->p, "sircc: call.indirect node %lld args[0] must be callee ptr ref", (long long)node_id);
      goto done;
    }
    LLVMValueRef callee = lower_expr(f, callee_id);
    if (!callee) goto done;
    if (LLVMGetTypeKind(LLVMTypeOf(callee)) != LLVMPointerTypeKind) {
      errf(f->p, "sircc: call.indirect node %lld callee must be a ptr", (long long)node_id);
      goto done;
    }

    size_t argc = args->v.arr.len - 1;
    LLVMValueRef* argv = NULL;
    if (argc) {
      argv = (LLVMValueRef*)malloc(argc * sizeof(LLVMValueRef));
      if (!argv) goto done;
      for (size_t i = 0; i < argc; i++) {
        int64_t aid = 0;
        if (!parse_node_ref_id(args->v.arr.items[i + 1], &aid)) {
          errf(f->p, "sircc: call.indirect node %lld arg[%zu] must be node ref", (long long)node_id, i);
          free(argv);
          goto done;
        }
        argv[i] = lower_expr(f, aid);
        if (!argv[i]) {
          free(argv);
          goto done;
        }
      }
    }

    unsigned param_count = LLVMCountParamTypes(callee_fty);
    bool is_varargs = LLVMIsFunctionVarArg(callee_fty) != 0;
    if (!is_varargs && (unsigned)argc != param_count) {
      errf(f->p, "sircc: call.indirect node %lld arg count mismatch (got %zu, want %u)", (long long)node_id, argc, param_count);
      free(argv);
      goto done;
    }
    if ((unsigned)argc < param_count) {
      errf(f->p, "sircc: call.indirect node %lld missing required args (got %zu, want >= %u)", (long long)node_id, argc, param_count);
      free(argv);
      goto done;
    }

    if (param_count) {
      LLVMTypeRef* params = (LLVMTypeRef*)malloc(param_count * sizeof(LLVMTypeRef));
      if (!params) {
        free(argv);
        goto done;
      }
      LLVMGetParamTypes(callee_fty, params);
      for (unsigned i = 0; i < param_count; i++) {
        LLVMTypeRef want = params[i];
        LLVMTypeRef got = LLVMTypeOf(argv[i]);
        if (want == got) continue;
        if (LLVMGetTypeKind(want) == LLVMPointerTypeKind && LLVMGetTypeKind(got) == LLVMPointerTypeKind) {
          argv[i] = LLVMBuildBitCast(f->builder, argv[i], want, "arg.cast");
          continue;
        }
        free(params);
        errf(f->p, "sircc: call.indirect node %lld arg[%u] type mismatch", (long long)node_id, i);
        free(argv);
        goto done;
      }
      free(params);
    }

    out = LLVMBuildCall2(f->builder, callee_fty, callee, argv, (unsigned)argc, "call");
    free(argv);

    if (out && n->type_ref) {
      LLVMTypeRef want = lower_type(f->p, f->ctx, n->type_ref);
      if (want && want != LLVMTypeOf(out)) {
        errf(f->p, "sircc: call.indirect node %lld return type does not match type_ref", (long long)node_id);
        out = NULL;
        goto done;
      }
    }

    goto done;
  }


  if (lower_expr_part_b(f, node_id, n, &out)) goto done;

  errf(f->p, "sircc: unsupported expr tag '%s' (node %lld)", n->tag, (long long)node_id);

done:
  n->llvm_value = out;
  n->resolving = false;
  return out;
}

// SPDX-FileCopyrightText: 2026 Frogfish
// SPDX-License-Identifier: GPL-3.0-or-later

#include "compiler_lower_internal.h"

#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

bool lower_expr_part_b(FunctionCtx* f, int64_t node_id, NodeRec* n, LLVMValueRef* outp) {
  (void)node_id;
  if (!f || !n || !outp) return false;
  LLVMValueRef out = NULL;

  if (strncmp(n->tag, "ptr.", 4) == 0) {
    const char* op = n->tag + 4;
    JsonValue* args = n->fields ? json_obj_get(n->fields, "args") : NULL;

    if (strcmp(op, "sym") == 0) {
      const char* name = NULL;
      if (n->fields) name = json_get_string(json_obj_get(n->fields, "name"));
      if (!name && args && args->type == JSON_ARRAY && args->v.arr.len == 1) {
        int64_t aid = 0;
        if (parse_node_ref_id(f->p, args->v.arr.items[0], &aid)) {
          NodeRec* an = get_node(f->p, aid);
          if (an && strcmp(an->tag, "name") == 0 && an->fields) {
            name = json_get_string(json_obj_get(an->fields, "name"));
          }
        }
      }
      if (!name) {
        errf(f->p, "sircc: ptr.sym node %lld requires fields.name or args:[name]", (long long)node_id);
        goto done;
      }
      LLVMValueRef fn = LLVMGetNamedFunction(f->mod, name);
      if (!fn) {
        errf(f->p, "sircc: ptr.sym references unknown function '%s'", name);
        goto done;
      }
      out = fn; // function values are pointers in LLVM
      goto done;
    }

    if (strcmp(op, "sizeof") == 0 || strcmp(op, "alignof") == 0 || strcmp(op, "offset") == 0) {
      if (!n->fields) {
        errf(f->p, "sircc: %s node %lld missing fields", n->tag, (long long)node_id);
        goto done;
      }
      int64_t ty_id = 0;
      if (!parse_type_ref_id(f->p, json_obj_get(n->fields, "ty"), &ty_id)) {
        errf(f->p, "sircc: %s node %lld missing fields.ty (type ref)", n->tag, (long long)node_id);
        goto done;
      }
      int64_t size = 0;
      int64_t align = 0;
      if (!type_size_align(f->p, ty_id, &size, &align)) {
        errf(f->p, "sircc: %s node %lld has invalid/unsized type %lld", n->tag, (long long)node_id, (long long)ty_id);
        goto done;
      }

      if (!args || args->type != JSON_ARRAY) {
        errf(f->p, "sircc: %s node %lld missing args array", n->tag, (long long)node_id);
        goto done;
      }

      if (strcmp(op, "sizeof") == 0) {
        if (args->v.arr.len != 0) {
          errf(f->p, "sircc: %s node %lld requires args:[]", n->tag, (long long)node_id);
          goto done;
        }
        out = LLVMConstInt(LLVMInt64TypeInContext(f->ctx), (unsigned long long)size, 0);
        goto done;
      }

      if (strcmp(op, "alignof") == 0) {
        if (args->v.arr.len != 0) {
          errf(f->p, "sircc: %s node %lld requires args:[]", n->tag, (long long)node_id);
          goto done;
        }
        out = LLVMConstInt(LLVMInt32TypeInContext(f->ctx), (unsigned long long)align, 0);
        goto done;
      }

      if (strcmp(op, "offset") == 0) {
        if (args->v.arr.len != 2) {
          errf(f->p, "sircc: %s node %lld requires args:[base,index]", n->tag, (long long)node_id);
          goto done;
        }
        int64_t base_id = 0, idx_id = 0;
        if (!parse_node_ref_id(f->p, args->v.arr.items[0], &base_id) || !parse_node_ref_id(f->p, args->v.arr.items[1], &idx_id)) {
          errf(f->p, "sircc: %s node %lld args must be node refs", n->tag, (long long)node_id);
          goto done;
        }
        LLVMValueRef base = lower_expr(f, base_id);
        LLVMValueRef idx = lower_expr(f, idx_id);
        if (!base || !idx) goto done;
        if (LLVMGetTypeKind(LLVMTypeOf(base)) != LLVMPointerTypeKind) {
          errf(f->p, "sircc: %s requires ptr base", n->tag);
          goto done;
        }
        if (LLVMGetTypeKind(LLVMTypeOf(idx)) != LLVMIntegerTypeKind || LLVMGetIntTypeWidth(LLVMTypeOf(idx)) != 64) {
          errf(f->p, "sircc: %s requires i64 index", n->tag);
          goto done;
        }

        unsigned ptr_bits = f->p->ptr_bits ? f->p->ptr_bits : (unsigned)(sizeof(void*) * 8u);
        LLVMTypeRef ip = LLVMIntTypeInContext(f->ctx, ptr_bits);
        LLVMValueRef base_bits = LLVMBuildPtrToInt(f->builder, base, ip, "base.bits");
        LLVMValueRef idx_bits = LLVMBuildTruncOrBitCast(f->builder, idx, ip, "idx.bits");
        LLVMValueRef scale = LLVMConstInt(ip, (unsigned long long)size, 0);
        LLVMValueRef off_bits = LLVMBuildMul(f->builder, idx_bits, scale, "off.bits");
        LLVMValueRef sum_bits = LLVMBuildAdd(f->builder, base_bits, off_bits, "addr.bits");
        out = LLVMBuildIntToPtr(f->builder, sum_bits, LLVMTypeOf(base), "ptr.off");
        goto done;
      }
    }

    if (!args || args->type != JSON_ARRAY) {
      errf(f->p, "sircc: %s node %lld missing args array", n->tag, (long long)node_id);
      goto done;
    }

    if (strcmp(op, "cmp.eq") == 0 || strcmp(op, "cmp.ne") == 0) {
      if (args->v.arr.len != 2) {
        errf(f->p, "sircc: %s node %lld requires 2 args", n->tag, (long long)node_id);
        goto done;
      }
      int64_t a_id = 0, b_id = 0;
      if (!parse_node_ref_id(f->p, args->v.arr.items[0], &a_id) || !parse_node_ref_id(f->p, args->v.arr.items[1], &b_id)) {
        errf(f->p, "sircc: %s node %lld args must be node refs", n->tag, (long long)node_id);
        goto done;
      }
      LLVMValueRef a = lower_expr(f, a_id);
      LLVMValueRef b = lower_expr(f, b_id);
      if (!a || !b) goto done;
      if (LLVMGetTypeKind(LLVMTypeOf(a)) == LLVMPointerTypeKind && LLVMGetTypeKind(LLVMTypeOf(b)) == LLVMPointerTypeKind &&
          LLVMTypeOf(a) != LLVMTypeOf(b)) {
        LLVMTypeRef i8p = LLVMPointerType(LLVMInt8TypeInContext(f->ctx), 0);
        a = LLVMBuildBitCast(f->builder, a, i8p, "pcmp.a");
        b = LLVMBuildBitCast(f->builder, b, i8p, "pcmp.b");
      }
      LLVMIntPredicate pred = (strcmp(op, "cmp.eq") == 0) ? LLVMIntEQ : LLVMIntNE;
      out = LLVMBuildICmp(f->builder, pred, a, b, "pcmp");
      goto done;
    }

    if (strcmp(op, "add") == 0 || strcmp(op, "sub") == 0) {
      if (args->v.arr.len != 2) {
        errf(f->p, "sircc: %s node %lld requires 2 args", n->tag, (long long)node_id);
        goto done;
      }
      int64_t p_id = 0, off_id = 0;
      if (!parse_node_ref_id(f->p, args->v.arr.items[0], &p_id) || !parse_node_ref_id(f->p, args->v.arr.items[1], &off_id)) {
        errf(f->p, "sircc: %s node %lld args must be node refs", n->tag, (long long)node_id);
        goto done;
      }
      LLVMValueRef pval = lower_expr(f, p_id);
      LLVMValueRef oval = lower_expr(f, off_id);
      if (!pval || !oval) goto done;
      LLVMTypeRef pty = LLVMTypeOf(pval);
      if (LLVMGetTypeKind(pty) != LLVMPointerTypeKind) {
        errf(f->p, "sircc: %s requires pointer lhs", n->tag);
        goto done;
      }
      if (LLVMGetTypeKind(LLVMTypeOf(oval)) != LLVMIntegerTypeKind) {
        errf(f->p, "sircc: %s requires integer byte offset rhs", n->tag);
        goto done;
      }
      LLVMTypeRef i8p = LLVMPointerType(LLVMInt8TypeInContext(f->ctx), 0);
      LLVMValueRef p8 = LLVMBuildBitCast(f->builder, pval, i8p, "p8");
      LLVMValueRef off = oval;
      LLVMTypeRef i64 = LLVMInt64TypeInContext(f->ctx);
      if (LLVMGetIntTypeWidth(LLVMTypeOf(off)) != 64) {
        off = build_sext_or_trunc(f->builder, off, i64, "off64");
      }
      if (strcmp(op, "sub") == 0) {
        off = LLVMBuildNeg(f->builder, off, "off.neg");
      }
      LLVMValueRef idx[1] = {off};
      LLVMValueRef gep = LLVMBuildGEP2(f->builder, LLVMInt8TypeInContext(f->ctx), p8, idx, 1, "p.gep");
      out = LLVMBuildBitCast(f->builder, gep, pty, "p.cast");
      goto done;
    }

    if (strcmp(op, "to_i64") == 0 || strcmp(op, "from_i64") == 0) {
      if (!args || args->type != JSON_ARRAY || args->v.arr.len != 1) {
        errf(f->p, "sircc: %s node %lld requires args:[x]", n->tag, (long long)node_id);
        goto done;
      }
      int64_t x_id = 0;
      if (!parse_node_ref_id(f->p, args->v.arr.items[0], &x_id)) {
        errf(f->p, "sircc: %s node %lld arg must be node ref", n->tag, (long long)node_id);
        goto done;
      }
      LLVMValueRef x = lower_expr(f, x_id);
      if (!x) goto done;

      LLVMTypeRef i64 = LLVMInt64TypeInContext(f->ctx);
      unsigned ptr_bits = f->p->ptr_bits ? f->p->ptr_bits : (unsigned)(sizeof(void*) * 8u);
      LLVMTypeRef ip = LLVMIntTypeInContext(f->ctx, ptr_bits);
      LLVMTypeRef pty = LLVMPointerType(LLVMInt8TypeInContext(f->ctx), 0);

      if (strcmp(op, "to_i64") == 0) {
        if (LLVMGetTypeKind(LLVMTypeOf(x)) != LLVMPointerTypeKind) {
          errf(f->p, "sircc: ptr.to_i64 requires ptr operand");
          goto done;
        }
        LLVMValueRef bits = LLVMBuildPtrToInt(f->builder, x, ip, "ptr.bits");
        out = build_zext_or_trunc(f->builder, bits, i64, "ptr.i64");
        goto done;
      }

      if (LLVMGetTypeKind(LLVMTypeOf(x)) != LLVMIntegerTypeKind || LLVMGetIntTypeWidth(LLVMTypeOf(x)) != 64) {
        errf(f->p, "sircc: ptr.from_i64 requires i64 operand");
        goto done;
      }
      LLVMValueRef bits = LLVMBuildTruncOrBitCast(f->builder, x, ip, "i64.ptrbits");
      out = LLVMBuildIntToPtr(f->builder, bits, pty, "ptr");
      goto done;
    }
  }

  if (strcmp(n->tag, "alloca") == 0) {
    if (!n->fields) {
      errf(f->p, "sircc: alloca node %lld missing fields", (long long)node_id);
      goto done;
    }
    int64_t ty_id = 0;
    if (!parse_type_ref_id(f->p, json_obj_get(n->fields, "ty"), &ty_id)) {
      errf(f->p, "sircc: alloca node %lld missing fields.ty (type ref)", (long long)node_id);
      goto done;
    }

    int64_t el_size = 0;
    int64_t el_align = 0;
    if (!type_size_align(f->p, ty_id, &el_size, &el_align)) {
      errf(f->p, "sircc: alloca node %lld has invalid/unsized element type %lld", (long long)node_id, (long long)ty_id);
      goto done;
    }

    LLVMTypeRef el = lower_type(f->p, f->ctx, ty_id);
    if (!el) {
      errf(f->p, "sircc: alloca node %lld has invalid element type %lld", (long long)node_id, (long long)ty_id);
      goto done;
    }

    // Parse flags: count?:i64, align?:i32, zero?:bool
    int64_t align_i64 = 0;
    bool align_present = false;
    bool zero_init = false;
    LLVMValueRef count_val = NULL;
    JsonValue* flags = json_obj_get(n->fields, "flags");
    if (flags && flags->type == JSON_OBJECT) {
      JsonValue* av = json_obj_get(flags, "align");
      if (av) {
        align_present = true;
        if (!json_get_i64(av, &align_i64)) {
          errf(f->p, "sircc: alloca node %lld flags.align must be an integer", (long long)node_id);
          goto done;
        }
      }
      JsonValue* zv = json_obj_get(flags, "zero");
      if (zv && zv->type == JSON_BOOL) zero_init = zv->v.b;
    }
    JsonValue* countv = (flags && flags->type == JSON_OBJECT) ? json_obj_get(flags, "count") : NULL;
    if (!countv) countv = json_obj_get(n->fields, "count");
    JsonValue* alignv = json_obj_get(n->fields, "align");
    if (alignv) {
      align_present = true;
      if (!json_get_i64(alignv, &align_i64)) {
        errf(f->p, "sircc: alloca node %lld align must be an integer", (long long)node_id);
        goto done;
      }
    }
    JsonValue* zerov = json_obj_get(n->fields, "zero");
    if (zerov && zerov->type == JSON_BOOL) zero_init = zerov->v.b;

    LLVMTypeRef i64 = LLVMInt64TypeInContext(f->ctx);
    if (!countv) {
      count_val = LLVMConstInt(i64, 1, 0);
    } else {
      int64_t c = 0;
      if (json_get_i64(countv, &c)) {
        if (c < 0) {
          errf(f->p, "sircc: alloca node %lld count must be >= 0", (long long)node_id);
          goto done;
        }
        count_val = LLVMConstInt(i64, (unsigned long long)c, 0);
      } else {
        int64_t cid = 0;
        if (!parse_node_ref_id(f->p, countv, &cid)) {
          errf(f->p, "sircc: alloca node %lld count must be i64 or node ref", (long long)node_id);
          goto done;
        }
        count_val = lower_expr(f, cid);
        if (!count_val) goto done;
        if (LLVMGetTypeKind(LLVMTypeOf(count_val)) != LLVMIntegerTypeKind) {
          errf(f->p, "sircc: alloca node %lld count ref must be integer", (long long)node_id);
          goto done;
        }
        if (LLVMGetIntTypeWidth(LLVMTypeOf(count_val)) != 64) {
          count_val = build_zext_or_trunc(f->builder, count_val, i64, "count.i64");
        }
      }
    }

    LLVMValueRef alloca_i = NULL;
    bool is_one = false;
    if (LLVMIsAConstantInt(count_val)) {
      unsigned long long z = LLVMConstIntGetZExtValue(count_val);
      is_one = (z == 1);
    }
    if (is_one) {
      alloca_i = LLVMBuildAlloca(f->builder, el, "alloca");
    } else {
      alloca_i = LLVMBuildArrayAlloca(f->builder, el, count_val, "alloca");
    }
    if (!alloca_i) goto done;

    unsigned align = 0;
    if (align_present) {
      if (align_i64 <= 0 || align_i64 > (int64_t)UINT_MAX) {
        errf(f->p, "sircc: alloca node %lld align must be > 0", (long long)node_id);
        goto done;
      }
      align = (unsigned)align_i64;
    } else if (el_align > 0) {
      align = (unsigned)el_align;
    }
    if (align) LLVMSetAlignment(alloca_i, align);

    if (zero_init) {
      LLVMTypeRef i8p = LLVMPointerType(LLVMInt8TypeInContext(f->ctx), 0);
      LLVMValueRef dst = LLVMBuildBitCast(f->builder, alloca_i, i8p, "alloca.i8p");
      LLVMValueRef byte = LLVMConstInt(LLVMInt8TypeInContext(f->ctx), 0, 0);
      LLVMValueRef bytes = LLVMConstInt(i64, (unsigned long long)el_size, 0);
      if (!is_one) bytes = LLVMBuildMul(f->builder, count_val, bytes, "alloca.bytes");
      LLVMBuildMemSet(f->builder, dst, byte, bytes, align ? align : 1);
    }

    // SIR mnemonic returns `ptr` (opaque). Represent as i8*.
    LLVMTypeRef i8p = LLVMPointerType(LLVMInt8TypeInContext(f->ctx), 0);
    alloca_i = LLVMBuildBitCast(f->builder, alloca_i, i8p, "alloca.ptr");

    out = alloca_i;
    goto done;
  }

  if (strncmp(n->tag, "alloca.", 7) == 0) {
    const char* tname = n->tag + 7;
    LLVMTypeRef el = NULL;
    if (strcmp(tname, "ptr") == 0) {
      el = LLVMPointerType(LLVMInt8TypeInContext(f->ctx), 0);
    } else {
      el = lower_type_prim(f->ctx, tname);
    }
    if (!el) {
      errf(f->p, "sircc: unsupported alloca type '%s'", tname);
      goto done;
    }
    out = LLVMBuildAlloca(f->builder, el, "alloca");
    goto done;
  }

  if (strncmp(n->tag, "load.", 5) == 0) {
    const char* tname = n->tag + 5;
    if (!n->fields) {
      errf(f->p, "sircc: %s node %lld missing fields", n->tag, (long long)node_id);
      goto done;
    }
    JsonValue* addr = json_obj_get(n->fields, "addr");
    int64_t aid = 0;
    if (!parse_node_ref_id(f->p, addr, &aid)) {
      errf(f->p, "sircc: %s node %lld missing fields.addr ref", n->tag, (long long)node_id);
      goto done;
    }
    LLVMValueRef pval = lower_expr(f, aid);
    if (!pval) goto done;
    LLVMTypeRef pty = LLVMTypeOf(pval);
    if (LLVMGetTypeKind(pty) != LLVMPointerTypeKind) {
      errf(f->p, "sircc: %s requires pointer addr", n->tag);
      goto done;
    }
    LLVMTypeRef el = NULL;
    if (strcmp(tname, "ptr") == 0) {
      el = LLVMPointerType(LLVMInt8TypeInContext(f->ctx), 0);
    } else {
      el = lower_type_prim(f->ctx, tname);
    }
    if (!el) {
      errf(f->p, "sircc: unsupported load type '%s'", tname);
      goto done;
    }
    LLVMTypeRef want_ptr = LLVMPointerType(el, 0);
    if (want_ptr != pty) {
      pval = LLVMBuildBitCast(f->builder, pval, want_ptr, "ld.cast");
    }
    JsonValue* alignv = json_obj_get(n->fields, "align");
    unsigned align = 1;
    if (alignv) {
      int64_t a = 0;
      if (!json_get_i64(alignv, &a)) {
        errf(f->p, "sircc: %s node %lld align must be an integer", n->tag, (long long)node_id);
        goto done;
      }
      if (a <= 0 || a > (int64_t)UINT_MAX) {
        errf(f->p, "sircc: %s node %lld align must be > 0", n->tag, (long long)node_id);
        goto done;
      }
      align = (unsigned)a;
    }
    if ((align & (align - 1u)) != 0u) {
      errf(f->p, "sircc: %s node %lld align must be a power of two", n->tag, (long long)node_id);
      goto done;
    }
    if (!emit_trap_if_misaligned(f, pval, align)) goto done;
    out = LLVMBuildLoad2(f->builder, el, pval, "load");
    LLVMSetAlignment(out, align);
    JsonValue* volv = json_obj_get(n->fields, "vol");
    if (volv && volv->type == JSON_BOOL) LLVMSetVolatile(out, volv->v.b ? 1 : 0);
    if (LLVMGetTypeKind(el) == LLVMFloatTypeKind || LLVMGetTypeKind(el) == LLVMDoubleTypeKind) {
      out = canonicalize_float(f, out);
    }
    goto done;
  }

  if (strncmp(n->tag, "f32.", 4) == 0 || strncmp(n->tag, "f64.", 4) == 0) {
    int width = (n->tag[1] == '3') ? 32 : 64;
    const char* op = n->tag + 4;

    JsonValue* args = n->fields ? json_obj_get(n->fields, "args") : NULL;
    if (!args || args->type != JSON_ARRAY) {
      errf(f->p, "sircc: %s node %lld missing args array", n->tag, (long long)node_id);
      goto done;
    }

    LLVMValueRef a = NULL;
    LLVMValueRef b = NULL;

    if (args->v.arr.len == 1) {
      int64_t a_id = 0;
      if (!parse_node_ref_id(f->p, args->v.arr.items[0], &a_id)) {
        errf(f->p, "sircc: %s node %lld args must be node refs", n->tag, (long long)node_id);
        goto done;
      }
      a = lower_expr(f, a_id);
      if (!a) goto done;
    } else if (args->v.arr.len == 2) {
      int64_t a_id = 0, b_id = 0;
      if (!parse_node_ref_id(f->p, args->v.arr.items[0], &a_id) || !parse_node_ref_id(f->p, args->v.arr.items[1], &b_id)) {
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

    // Conversions like f32.from_i32.s take integer operands, so handle those
    // before enforcing float operand types.
    if (strncmp(op, "from_i", 6) == 0) {
      if (!a || b) {
        errf(f->p, "sircc: %s requires args:[x]", n->tag);
        goto done;
      }
      int srcw = 0;
      char su = 0;
      if (sscanf(op, "from_i%d.%c", &srcw, &su) != 2 || (srcw != 32 && srcw != 64) || (su != 's' && su != 'u')) {
        errf(f->p, "sircc: unsupported int->float conversion '%s' in %s", op, n->tag);
        goto done;
      }
      if (LLVMGetTypeKind(LLVMTypeOf(a)) != LLVMIntegerTypeKind || LLVMGetIntTypeWidth(LLVMTypeOf(a)) != (unsigned)srcw) {
        errf(f->p, "sircc: %s requires i%d operand", n->tag, srcw);
        goto done;
      }
      LLVMTypeRef fty = (width == 32) ? LLVMFloatTypeInContext(f->ctx) : LLVMDoubleTypeInContext(f->ctx);
      out = (su == 's') ? LLVMBuildSIToFP(f->builder, a, fty, "sitofp") : LLVMBuildUIToFP(f->builder, a, fty, "uitofp");
      goto done;
    }

    LLVMTypeRef fty = LLVMTypeOf(a);
    if (width == 32 && LLVMGetTypeKind(fty) != LLVMFloatTypeKind) {
      errf(f->p, "sircc: %s expects f32 operands", n->tag);
      goto done;
    }
    if (width == 64 && LLVMGetTypeKind(fty) != LLVMDoubleTypeKind) {
      errf(f->p, "sircc: %s expects f64 operands", n->tag);
      goto done;
    }

    if (strcmp(op, "add") == 0) {
      if (!b) {
        errf(f->p, "sircc: %s requires 2 args", n->tag);
        goto done;
      }
      out = canonicalize_float(f, LLVMBuildFAdd(f->builder, a, b, "fadd"));
      goto done;
    }
    if (strcmp(op, "sub") == 0) {
      if (!b) {
        errf(f->p, "sircc: %s requires 2 args", n->tag);
        goto done;
      }
      out = canonicalize_float(f, LLVMBuildFSub(f->builder, a, b, "fsub"));
      goto done;
    }
    if (strcmp(op, "mul") == 0) {
      if (!b) {
        errf(f->p, "sircc: %s requires 2 args", n->tag);
        goto done;
      }
      out = canonicalize_float(f, LLVMBuildFMul(f->builder, a, b, "fmul"));
      goto done;
    }
    if (strcmp(op, "div") == 0) {
      if (!b) {
        errf(f->p, "sircc: %s requires 2 args", n->tag);
        goto done;
      }
      out = canonicalize_float(f, LLVMBuildFDiv(f->builder, a, b, "fdiv"));
      goto done;
    }
    if (strcmp(op, "neg") == 0) {
      out = canonicalize_float(f, LLVMBuildFNeg(f->builder, a, "fneg"));
      goto done;
    }
    if (strcmp(op, "abs") == 0) {
      char full[32];
      snprintf(full, sizeof(full), "llvm.fabs.f%d", width);
      LLVMTypeRef params[1] = {fty};
      LLVMValueRef fn = get_or_declare_intrinsic(f->mod, full, fty, params, 1);
      LLVMValueRef argsv[1] = {a};
      out = canonicalize_float(f, LLVMBuildCall2(f->builder, LLVMGlobalGetValueType(fn), fn, argsv, 1, "fabs"));
      goto done;
    }
    if (strcmp(op, "sqrt") == 0) {
      char full[32];
      snprintf(full, sizeof(full), "llvm.sqrt.f%d", width);
      LLVMTypeRef params[1] = {fty};
      LLVMValueRef fn = get_or_declare_intrinsic(f->mod, full, fty, params, 1);
      LLVMValueRef argsv[1] = {a};
      out = canonicalize_float(f, LLVMBuildCall2(f->builder, LLVMGlobalGetValueType(fn), fn, argsv, 1, "fsqrt"));
      goto done;
    }

    if (strcmp(op, "min") == 0 || strcmp(op, "max") == 0) {
      if (!b) {
        errf(f->p, "sircc: %s requires 2 args", n->tag);
        goto done;
      }
      LLVMValueRef isnan_a = LLVMBuildFCmp(f->builder, LLVMRealUNO, a, a, "isnan.a");
      LLVMValueRef isnan_b = LLVMBuildFCmp(f->builder, LLVMRealUNO, b, b, "isnan.b");
      LLVMValueRef anynan = LLVMBuildOr(f->builder, isnan_a, isnan_b, "isnan.any");
      LLVMValueRef qnan = canonical_qnan(f, fty);

      LLVMRealPredicate pred = (strcmp(op, "min") == 0) ? LLVMRealOLT : LLVMRealOGT;
      LLVMValueRef cmp = LLVMBuildFCmp(f->builder, pred, a, b, "fcmp");
      LLVMValueRef sel = LLVMBuildSelect(f->builder, cmp, a, b, "fsel");
      out = LLVMBuildSelect(f->builder, anynan, qnan, sel, "fminmax");
      goto done;
    }

    if (strncmp(op, "cmp.", 4) == 0) {
      if (!b) {
        errf(f->p, "sircc: %s requires 2 args", n->tag);
        goto done;
      }
      const char* cc = op + 4;
      LLVMRealPredicate pred;
      if (strcmp(cc, "oeq") == 0) pred = LLVMRealOEQ;
      else if (strcmp(cc, "one") == 0) pred = LLVMRealONE;
      else if (strcmp(cc, "olt") == 0) pred = LLVMRealOLT;
      else if (strcmp(cc, "ole") == 0) pred = LLVMRealOLE;
      else if (strcmp(cc, "ogt") == 0) pred = LLVMRealOGT;
      else if (strcmp(cc, "oge") == 0) pred = LLVMRealOGE;
      else if (strcmp(cc, "ueq") == 0) pred = LLVMRealUEQ;
      else if (strcmp(cc, "une") == 0) pred = LLVMRealUNE;
      else if (strcmp(cc, "ult") == 0) pred = LLVMRealULT;
      else if (strcmp(cc, "ule") == 0) pred = LLVMRealULE;
      else if (strcmp(cc, "ugt") == 0) pred = LLVMRealUGT;
      else if (strcmp(cc, "uge") == 0) pred = LLVMRealUGE;
      else {
        errf(f->p, "sircc: unsupported float compare '%s' in %s", cc, n->tag);
        goto done;
      }
      out = LLVMBuildFCmp(f->builder, pred, a, b, "fcmp");
      goto done;
    }

  }

  if (strncmp(n->tag, "const.", 6) == 0) {
    const char* tyname = n->tag + 6;
    if (!n->fields) goto done;
    LLVMTypeRef ty = lower_type_prim(f->ctx, tyname);
    if (!ty) {
      errf(f->p, "sircc: unsupported const type '%s'", tyname);
      goto done;
    }
    if (LLVMGetTypeKind(ty) == LLVMIntegerTypeKind) {
      int64_t value = 0;
      if (!must_i64(f->p, json_obj_get(n->fields, "value"), &value, "const.value")) goto done;
      out = LLVMConstInt(ty, (unsigned long long)value, 1);
      goto done;
    }
    if (LLVMGetTypeKind(ty) == LLVMFloatTypeKind || LLVMGetTypeKind(ty) == LLVMDoubleTypeKind) {
      // Prefer exact bit-pattern constants: fields.bits = "0x..." (hex).
      const char* bits = json_get_string(json_obj_get(n->fields, "bits"));
      if (!bits || strncmp(bits, "0x", 2) != 0) {
        errf(f->p, "sircc: const.%s requires fields.bits hex string (0x...)", tyname);
        goto done;
      }
      char* end = NULL;
      unsigned long long raw = strtoull(bits + 2, &end, 16);
      if (!end || *end != 0) {
        errf(f->p, "sircc: const.%s invalid bits '%s'", tyname, bits);
        goto done;
      }
      if (LLVMGetTypeKind(ty) == LLVMFloatTypeKind) {
        LLVMValueRef ib = LLVMConstInt(LLVMInt32TypeInContext(f->ctx), raw & 0xFFFFFFFFu, 0);
        out = LLVMConstBitCast(ib, ty);
      } else {
        LLVMValueRef ib = LLVMConstInt(LLVMInt64TypeInContext(f->ctx), raw, 0);
        out = LLVMConstBitCast(ib, ty);
      }
      goto done;
    }
  }

  return false;

done:
  *outp = out;
  return true;
}

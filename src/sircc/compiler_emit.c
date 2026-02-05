// SPDX-FileCopyrightText: 2026 Frogfish
// SPDX-License-Identifier: GPL-3.0-or-later

#include "compiler_internal.h"

#include <llvm-c/Analysis.h>
#include <llvm-c/Target.h>
#include <llvm-c/TargetMachine.h>

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void llvm_init_targets_once(void) {
  static int inited = 0;
  if (inited) return;
  // Avoid forcing linkage against every LLVM target backend. For the
  // "product" path (Milestone 3), initializing the native target is enough.
  // If/when we want a true cross-compiler build, we can add an opt-in mode
  // that links + initializes all targets.
  if (LLVMInitializeNativeTarget() != 0) {
    fprintf(stderr, "sircc: failed to initialize native LLVM target\n");
    exit(2);
  }
  if (LLVMInitializeNativeAsmPrinter() != 0) {
    fprintf(stderr, "sircc: failed to initialize native LLVM asm printer\n");
    exit(2);
  }
  // The parser isn't strictly required for object/exe emission, but is a cheap
  // init and keeps future tooling options open.
  (void)LLVMInitializeNativeAsmParser();
  inited = 1;
}

bool emit_module_ir(SirProgram* p, LLVMModuleRef mod, const char* out_path) {
  char* err = NULL;
  if (LLVMPrintModuleToFile(mod, out_path, &err) != 0) {
    err_codef(p, "sircc.llvm.emit_ir_failed", "sircc: failed to write LLVM IR: %s", err ? err : "(unknown)");
    LLVMDisposeMessage(err);
    return false;
  }
  return true;
}

bool init_target_for_module(SirProgram* p, LLVMModuleRef mod, const char* triple) {
  if (!p || !mod || !triple) return false;

  llvm_init_targets_once();

  char* err = NULL;
  LLVMTargetRef target = NULL;
  if (LLVMGetTargetFromTriple(triple, &target, &err) != 0) {
    err_codef(p, "sircc.llvm.triple.unsupported", "sircc: target triple '%s' unsupported: %s", triple, err ? err : "(unknown)");
    LLVMDisposeMessage(err);
    return false;
  }

  LLVMTargetMachineRef tm =
      LLVMCreateTargetMachine(target, triple, "generic", "", LLVMCodeGenLevelDefault, LLVMRelocDefault, LLVMCodeModelDefault);
  if (!tm) {
    err_codef(p, "sircc.llvm.target_machine.create_failed", "sircc: failed to create target machine");
    return false;
  }

  LLVMTargetDataRef td = LLVMCreateTargetDataLayout(tm);
  char* dl_str = LLVMCopyStringRepOfTargetData(td);
  LLVMSetTarget(mod, triple);
  LLVMSetDataLayout(mod, dl_str);

  LLVMContextRef ctx = LLVMContextCreate();
  LLVMTypeRef i8 = LLVMInt8TypeInContext(ctx);
  LLVMTypeRef i16 = LLVMInt16TypeInContext(ctx);
  LLVMTypeRef i32 = LLVMInt32TypeInContext(ctx);
  LLVMTypeRef i64 = LLVMInt64TypeInContext(ctx);
  LLVMTypeRef f32 = LLVMFloatTypeInContext(ctx);
  LLVMTypeRef f64 = LLVMDoubleTypeInContext(ctx);
  LLVMTypeRef ptr = LLVMPointerType(i8, 0);
  unsigned have_ptr_bytes = LLVMPointerSize(td);
  unsigned have_ptr_bits = have_ptr_bytes * 8u;
  bool have_big_endian = (dl_str && dl_str[0] == 'E');
  unsigned have_align_i8 = LLVMABIAlignmentOfType(td, i8);
  unsigned have_align_i16 = LLVMABIAlignmentOfType(td, i16);
  unsigned have_align_i32 = LLVMABIAlignmentOfType(td, i32);
  unsigned have_align_i64 = LLVMABIAlignmentOfType(td, i64);
  unsigned have_align_f32 = LLVMABIAlignmentOfType(td, f32);
  unsigned have_align_f64 = LLVMABIAlignmentOfType(td, f64);
  unsigned have_align_ptr = LLVMABIAlignmentOfType(td, ptr);
  LLVMContextDispose(ctx);

  if (p->target_ptrbits_override && p->ptr_bits && p->ptr_bits != have_ptr_bits) {
    err_codef(p, "sircc.target.ptrBits.mismatch", "sircc: meta.ext.target.ptrBits=%u does not match LLVM target ptrBits=%u",
              p->ptr_bits, have_ptr_bits);
    LLVMDisposeMessage(dl_str);
    LLVMDisposeTargetData(td);
    LLVMDisposeTargetMachine(tm);
    return false;
  }
  if (p->target_endian_override && p->target_big_endian != have_big_endian) {
    err_codef(p, "sircc.target.endian.mismatch", "sircc: meta.ext.target.endian does not match LLVM target endianness");
    LLVMDisposeMessage(dl_str);
    LLVMDisposeTargetData(td);
    LLVMDisposeTargetMachine(tm);
    return false;
  }
  if (p->target_structalign_override && p->struct_align && strcmp(p->struct_align, "max") != 0) {
    err_codef(p, "sircc.target.structAlign.unsupported", "sircc: structAlign '%s' is not supported yet (use 'max')", p->struct_align);
    LLVMDisposeMessage(dl_str);
    LLVMDisposeTargetData(td);
    LLVMDisposeTargetMachine(tm);
    return false;
  }
  if (p->target_intalign_override) {
    if (p->align_i8 && p->align_i8 != have_align_i8) goto align_mismatch;
    if (p->align_i16 && p->align_i16 != have_align_i16) goto align_mismatch;
    if (p->align_i32 && p->align_i32 != have_align_i32) goto align_mismatch;
    if (p->align_i64 && p->align_i64 != have_align_i64) goto align_mismatch;
    if (p->align_ptr && p->align_ptr != have_align_ptr) goto align_mismatch;
  }
  if (p->target_floatalign_override) {
    if (p->align_f32 && p->align_f32 != have_align_f32) goto align_mismatch;
    if (p->align_f64 && p->align_f64 != have_align_f64) goto align_mismatch;
  }

  // Adopt LLVM ABI values where the producer didn't provide an explicit contract.
  p->ptr_bytes = have_ptr_bytes;
  p->ptr_bits = have_ptr_bits;
  p->target_big_endian = have_big_endian;
  if (!p->struct_align) p->struct_align = "max";
  if (!p->align_i8) p->align_i8 = have_align_i8;
  if (!p->align_i16) p->align_i16 = have_align_i16;
  if (!p->align_i32) p->align_i32 = have_align_i32;
  if (!p->align_i64) p->align_i64 = have_align_i64;
  if (!p->align_f32) p->align_f32 = have_align_f32;
  if (!p->align_f64) p->align_f64 = have_align_f64;
  if (!p->align_ptr) p->align_ptr = have_align_ptr;

  LLVMDisposeMessage(dl_str);
  LLVMDisposeTargetData(td);
  LLVMDisposeTargetMachine(tm);
  return true;

align_mismatch:
  err_codef(p, "sircc.target.align.mismatch", "sircc: meta.ext.target.*Align does not match LLVM target ABI alignment");
  LLVMDisposeMessage(dl_str);
  LLVMDisposeTargetData(td);
  LLVMDisposeTargetMachine(tm);
  return false;
}

bool init_target_info(SirProgram* p, const char* triple) {
  if (!p || !triple) return false;
  llvm_init_targets_once();

  char* err = NULL;
  LLVMTargetRef target = NULL;
  if (LLVMGetTargetFromTriple(triple, &target, &err) != 0) {
    err_codef(p, "sircc.llvm.triple.unsupported", "sircc: target triple '%s' unsupported: %s", triple, err ? err : "(unknown)");
    LLVMDisposeMessage(err);
    return false;
  }

  LLVMTargetMachineRef tm =
      LLVMCreateTargetMachine(target, triple, "generic", "", LLVMCodeGenLevelDefault, LLVMRelocDefault, LLVMCodeModelDefault);
  if (!tm) {
    err_codef(p, "sircc.llvm.target_machine.create_failed", "sircc: failed to create target machine");
    return false;
  }

  LLVMTargetDataRef td = LLVMCreateTargetDataLayout(tm);
  char* dl_str = LLVMCopyStringRepOfTargetData(td);

  LLVMContextRef ctx = LLVMContextCreate();
  LLVMTypeRef i8 = LLVMInt8TypeInContext(ctx);
  LLVMTypeRef i16 = LLVMInt16TypeInContext(ctx);
  LLVMTypeRef i32 = LLVMInt32TypeInContext(ctx);
  LLVMTypeRef i64 = LLVMInt64TypeInContext(ctx);
  LLVMTypeRef f32 = LLVMFloatTypeInContext(ctx);
  LLVMTypeRef f64 = LLVMDoubleTypeInContext(ctx);
  LLVMTypeRef ptr = LLVMPointerType(i8, 0);
  unsigned have_ptr_bytes = LLVMPointerSize(td);
  unsigned have_ptr_bits = have_ptr_bytes * 8u;
  bool have_big_endian = (dl_str && dl_str[0] == 'E');
  unsigned have_align_i8 = LLVMABIAlignmentOfType(td, i8);
  unsigned have_align_i16 = LLVMABIAlignmentOfType(td, i16);
  unsigned have_align_i32 = LLVMABIAlignmentOfType(td, i32);
  unsigned have_align_i64 = LLVMABIAlignmentOfType(td, i64);
  unsigned have_align_f32 = LLVMABIAlignmentOfType(td, f32);
  unsigned have_align_f64 = LLVMABIAlignmentOfType(td, f64);
  unsigned have_align_ptr = LLVMABIAlignmentOfType(td, ptr);
  LLVMContextDispose(ctx);

  if (p->target_ptrbits_override && p->ptr_bits && p->ptr_bits != have_ptr_bits) {
    err_codef(p, "sircc.target.ptrBits.mismatch", "sircc: meta.ext.target.ptrBits=%u does not match LLVM target ptrBits=%u",
              p->ptr_bits, have_ptr_bits);
    LLVMDisposeMessage(dl_str);
    LLVMDisposeTargetData(td);
    LLVMDisposeTargetMachine(tm);
    return false;
  }
  if (p->target_endian_override && p->target_big_endian != have_big_endian) {
    err_codef(p, "sircc.target.endian.mismatch", "sircc: meta.ext.target.endian does not match LLVM target endianness");
    LLVMDisposeMessage(dl_str);
    LLVMDisposeTargetData(td);
    LLVMDisposeTargetMachine(tm);
    return false;
  }
  if (p->target_structalign_override && p->struct_align && strcmp(p->struct_align, "max") != 0) {
    err_codef(p, "sircc.target.structAlign.unsupported", "sircc: structAlign '%s' is not supported yet (use 'max')", p->struct_align);
    LLVMDisposeMessage(dl_str);
    LLVMDisposeTargetData(td);
    LLVMDisposeTargetMachine(tm);
    return false;
  }
  if (p->target_intalign_override) {
    if (p->align_i8 && p->align_i8 != have_align_i8) goto align_mismatch2;
    if (p->align_i16 && p->align_i16 != have_align_i16) goto align_mismatch2;
    if (p->align_i32 && p->align_i32 != have_align_i32) goto align_mismatch2;
    if (p->align_i64 && p->align_i64 != have_align_i64) goto align_mismatch2;
    if (p->align_ptr && p->align_ptr != have_align_ptr) goto align_mismatch2;
  }
  if (p->target_floatalign_override) {
    if (p->align_f32 && p->align_f32 != have_align_f32) goto align_mismatch2;
    if (p->align_f64 && p->align_f64 != have_align_f64) goto align_mismatch2;
  }

  p->ptr_bytes = have_ptr_bytes;
  p->ptr_bits = have_ptr_bits;
  p->target_big_endian = have_big_endian;
  if (!p->struct_align) p->struct_align = "max";
  if (!p->align_i8) p->align_i8 = have_align_i8;
  if (!p->align_i16) p->align_i16 = have_align_i16;
  if (!p->align_i32) p->align_i32 = have_align_i32;
  if (!p->align_i64) p->align_i64 = have_align_i64;
  if (!p->align_f32) p->align_f32 = have_align_f32;
  if (!p->align_f64) p->align_f64 = have_align_f64;
  if (!p->align_ptr) p->align_ptr = have_align_ptr;

  LLVMDisposeMessage(dl_str);
  LLVMDisposeTargetData(td);
  LLVMDisposeTargetMachine(tm);
  return true;

align_mismatch2:
  err_codef(p, "sircc.target.align.mismatch", "sircc: meta.ext.target.*Align does not match LLVM target ABI alignment");
  LLVMDisposeMessage(dl_str);
  LLVMDisposeTargetData(td);
  LLVMDisposeTargetMachine(tm);
  return false;
}

bool emit_module_obj(SirProgram* p, LLVMModuleRef mod, const char* triple, const char* out_path) {
  llvm_init_targets_once();

  char* err = NULL;
  const char* use_triple = triple ? triple : LLVMGetDefaultTargetTriple();
  LLVMTargetRef target = NULL;
  if (LLVMGetTargetFromTriple(use_triple, &target, &err) != 0) {
    err_codef(p, "sircc.llvm.triple.unsupported", "sircc: target triple '%s' unsupported: %s", use_triple, err ? err : "(unknown)");
    LLVMDisposeMessage(err);
    if (!triple) LLVMDisposeMessage((char*)use_triple);
    return false;
  }

  LLVMTargetMachineRef tm =
      LLVMCreateTargetMachine(target, use_triple, "generic", "", LLVMCodeGenLevelDefault, LLVMRelocDefault,
                              LLVMCodeModelDefault);
  if (!tm) {
    err_codef(p, "sircc.llvm.target_machine.create_failed", "sircc: failed to create target machine");
    if (!triple) LLVMDisposeMessage((char*)use_triple);
    return false;
  }

  LLVMTargetDataRef td = LLVMCreateTargetDataLayout(tm);
  char* dl_str = LLVMCopyStringRepOfTargetData(td);
  LLVMSetTarget(mod, use_triple);
  LLVMSetDataLayout(mod, dl_str);
  LLVMDisposeMessage(dl_str);
  LLVMDisposeTargetData(td);

  if (LLVMTargetMachineEmitToFile(tm, mod, (char*)out_path, LLVMObjectFile, &err) != 0) {
    err_codef(p, "sircc.llvm.emit_obj_failed", "sircc: failed to emit object: %s", err ? err : "(unknown)");
    LLVMDisposeMessage(err);
    LLVMDisposeTargetMachine(tm);
    if (!triple) LLVMDisposeMessage((char*)use_triple);
    return false;
  }

  LLVMDisposeTargetMachine(tm);
  if (!triple) LLVMDisposeMessage((char*)use_triple);
  return true;
}

bool sircc_print_target(const char* triple) {
  llvm_init_targets_once();

  char* err = NULL;
  const char* use_triple = triple ? triple : LLVMGetDefaultTargetTriple();
  LLVMTargetRef target = NULL;
  if (LLVMGetTargetFromTriple(use_triple, &target, &err) != 0) {
    errf(NULL, "sircc: target triple '%s' unsupported: %s", use_triple, err ? err : "(unknown)");
    LLVMDisposeMessage(err);
    if (!triple) LLVMDisposeMessage((char*)use_triple);
    return false;
  }

  LLVMTargetMachineRef tm =
      LLVMCreateTargetMachine(target, use_triple, "generic", "", LLVMCodeGenLevelDefault, LLVMRelocDefault, LLVMCodeModelDefault);
  if (!tm) {
    errf(NULL, "sircc: failed to create target machine");
    if (!triple) LLVMDisposeMessage((char*)use_triple);
    return false;
  }

  LLVMTargetDataRef td = LLVMCreateTargetDataLayout(tm);
  char* dl_str = LLVMCopyStringRepOfTargetData(td);

  unsigned ptr_bytes = LLVMPointerSize(td);
  unsigned ptr_bits = ptr_bytes * 8u;
  const char* endian = (dl_str && dl_str[0] == 'E') ? "big" : "little";

  printf("triple: %s\n", use_triple);
  printf("data_layout: %s\n", dl_str ? dl_str : "(null)");
  printf("endianness: %s\n", endian);
  printf("ptrBits: %u\n", ptr_bits);

  LLVMDisposeMessage(dl_str);
  LLVMDisposeTargetData(td);
  LLVMDisposeTargetMachine(tm);
  if (!triple) LLVMDisposeMessage((char*)use_triple);
  return true;
}

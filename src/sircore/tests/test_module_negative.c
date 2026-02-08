#include "sir_module.h"

#include <stdio.h>
#include <string.h>

static int fail(const char* msg) {
  fprintf(stderr, "sircore_unit: %s\n", msg);
  return 1;
}

static int expect_invalid(const sir_module_t* m) {
  char err[160];
  memset(err, 0, sizeof(err));
  if (sir_module_validate(m, err, sizeof(err))) {
    return fail("expected module to be invalid");
  }
  if (err[0] == '\0') {
    return fail("expected non-empty validate error");
  }

  sir_validate_diag_t d;
  memset(&d, 0, sizeof(d));
  if (sir_module_validate_ex(m, &d)) {
    return fail("expected module_validate_ex to fail");
  }
  if (!d.code || !d.code[0]) {
    return fail("expected non-empty validate_ex code");
  }
  if (d.message[0] == '\0') {
    return fail("expected non-empty validate_ex message");
  }
  return 0;
}

int main(void) {
  // Case 1: call_extern arg_count mismatch vs signature.
  {
    sir_module_builder_t* b = sir_mb_new();
    if (!b) return fail("sir_mb_new failed");

    const sir_type_id_t ty_i32 = sir_mb_type_prim(b, SIR_PRIM_I32);
    const sir_type_id_t ty_ptr = sir_mb_type_prim(b, SIR_PRIM_PTR);
    const sir_type_id_t ty_i64 = sir_mb_type_prim(b, SIR_PRIM_I64);
    if (!ty_i32 || !ty_ptr || !ty_i64) {
      sir_mb_free(b);
      return fail("sir_mb_type_prim failed");
    }

    const sir_type_id_t p[] = {ty_i32, ty_ptr, ty_i64};
    sir_sig_t sig = {.params = p, .param_count = 3, .results = NULL, .result_count = 0};
    const sir_sym_id_t sym = sir_mb_sym_extern_fn(b, "zi_write", sig);
    if (!sym) {
      sir_mb_free(b);
      return fail("sir_mb_sym_extern_fn failed");
    }

    const sir_func_id_t f = sir_mb_func_begin(b, "main");
    if (!f || !sir_mb_func_set_entry(b, f) || !sir_mb_func_set_value_count(b, f, 3)) {
      sir_mb_free(b);
      return fail("sir_mb_func_begin failed");
    }

    (void)sir_mb_emit_const_i32(b, f, 0, 1);
    (void)sir_mb_emit_const_null_ptr(b, f, 1);
    (void)sir_mb_emit_const_i64(b, f, 2, 0);

    const sir_val_id_t bad_args[] = {0, 1}; // missing len arg
    if (!sir_mb_emit_call_extern(b, f, sym, bad_args, 2)) {
      sir_mb_free(b);
      return fail("sir_mb_emit_call_extern failed");
    }
    (void)sir_mb_emit_exit(b, f, 0);

    sir_module_t* m = sir_mb_finalize(b);
    sir_mb_free(b);
    if (!m) return fail("sir_mb_finalize failed");
    const int rc = expect_invalid(m);
    sir_module_free(m);
    if (rc) return rc;
  }

  return 0;
}

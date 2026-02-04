# Expects:
#   -DSIRCC=<path to sircc>
#   -DLOWER=<path to ext/.../lower>
#   -DIRCHECK=<path to ext/.../ircheck>
#   -DZABI25_ROOT=<path to ext/... or dist/...>
#   -DINPUT=<path to .sir.jsonl>
#   -DOUT_PREFIX=<path prefix for build artifacts>
# Optional:
#   -DEXPECT_STDOUT=<substring to assert in both outputs>
#   -DCLANG=<path to clang> (default: clang)
#   -DZEM=<path to ext/.../zem> (enables PGO stage via --pgo-len-out)
#   -DSTRIP_MODE=<mode> (enables coverage-based strip + lower; e.g. uncovered-delete)

if(NOT DEFINED SIRCC)
  message(FATAL_ERROR "compare_llvm_vs_lower.cmake: missing -DSIRCC")
endif()
if(NOT DEFINED LOWER)
  message(FATAL_ERROR "compare_llvm_vs_lower.cmake: missing -DLOWER")
endif()
if(NOT DEFINED IRCHECK)
  message(FATAL_ERROR "compare_llvm_vs_lower.cmake: missing -DIRCHECK")
endif()
if(NOT DEFINED ZABI25_ROOT)
  message(FATAL_ERROR "compare_llvm_vs_lower.cmake: missing -DZABI25_ROOT")
endif()
if(NOT DEFINED INPUT)
  message(FATAL_ERROR "compare_llvm_vs_lower.cmake: missing -DINPUT")
endif()
if(NOT DEFINED OUT_PREFIX)
  message(FATAL_ERROR "compare_llvm_vs_lower.cmake: missing -DOUT_PREFIX")
endif()
if(NOT DEFINED CLANG)
  set(CLANG clang)
endif()

set(llvm_exe "${OUT_PREFIX}.llvm.exe")
set(zasm_jsonl "${OUT_PREFIX}.zasm.jsonl")
set(lower_obj "${OUT_PREFIX}.lower.o")
set(lower_exe "${OUT_PREFIX}.lower.exe")
set(runner_obj "${OUT_PREFIX}.runner.o")
set(pgo_jsonl "${OUT_PREFIX}.pgo_len.jsonl")
set(lower_pgo_obj "${OUT_PREFIX}.lower.pgo_len.o")
set(lower_pgo_exe "${OUT_PREFIX}.lower.pgo_len.exe")
set(cov_jsonl "${OUT_PREFIX}.coverage.jsonl")
set(stripped_zasm_jsonl "${OUT_PREFIX}.stripped.zasm.jsonl")
set(strip_stats_jsonl "${OUT_PREFIX}.strip_stats.jsonl")
set(lower_stripped_obj "${OUT_PREFIX}.lower.stripped.o")
set(lower_stripped_exe "${OUT_PREFIX}.lower.stripped.exe")

set(runner_c "${ZABI25_ROOT}/examples/host_shim/runner.c")
set(include_dir "${ZABI25_ROOT}/include")
set(libzing "${ZABI25_ROOT}/lib/libzingcore25.a")

execute_process(
  COMMAND "${SIRCC}" --runtime zabi25 --zabi25-root "${ZABI25_ROOT}" "${INPUT}" -o "${llvm_exe}"
  RESULT_VARIABLE rc_llvm
  OUTPUT_VARIABLE out_llvm_build
  ERROR_VARIABLE err_llvm_build
)
if(NOT rc_llvm EQUAL 0)
  message(FATAL_ERROR "sircc llvm build failed (rc=${rc_llvm})\n${out_llvm_build}\n${err_llvm_build}")
endif()

execute_process(
  COMMAND "${SIRCC}" "${INPUT}" -o "${zasm_jsonl}" --emit-zasm
  RESULT_VARIABLE rc_zasm
  OUTPUT_VARIABLE out_zasm
  ERROR_VARIABLE err_zasm
)
if(NOT rc_zasm EQUAL 0)
  message(FATAL_ERROR "sircc --emit-zasm failed (rc=${rc_zasm})\n${out_zasm}\n${err_zasm}")
endif()

execute_process(
  COMMAND "${IRCHECK}" --tool --ir v1.1 "${zasm_jsonl}"
  RESULT_VARIABLE rc_ircheck
  OUTPUT_VARIABLE out_ircheck
  ERROR_VARIABLE err_ircheck
)
if(NOT rc_ircheck EQUAL 0)
  message(FATAL_ERROR "ircheck failed (rc=${rc_ircheck})\n${out_ircheck}\n${err_ircheck}")
endif()

execute_process(
  COMMAND "${LOWER}" --input "${zasm_jsonl}" --o "${lower_obj}"
  RESULT_VARIABLE rc_lower
  OUTPUT_VARIABLE out_lower
  ERROR_VARIABLE err_lower
)
if(NOT rc_lower EQUAL 0)
  message(FATAL_ERROR "lower failed (rc=${rc_lower})\n${out_lower}\n${err_lower}")
endif()

execute_process(
  COMMAND "${CLANG}" -std=c11 -c "${runner_c}" "-I${include_dir}" -o "${runner_obj}"
  RESULT_VARIABLE rc_runner
  OUTPUT_VARIABLE out_runner
  ERROR_VARIABLE err_runner
)
if(NOT rc_runner EQUAL 0)
  message(FATAL_ERROR "clang failed to compile zabi runner (rc=${rc_runner})\n${out_runner}\n${err_runner}")
endif()

execute_process(
  COMMAND "${CLANG}" -o "${lower_exe}" "${runner_obj}" "${lower_obj}" "${libzing}"
  RESULT_VARIABLE rc_link
  OUTPUT_VARIABLE out_link
  ERROR_VARIABLE err_link
)
if(NOT rc_link EQUAL 0)
  message(FATAL_ERROR "clang failed to link lower exe (rc=${rc_link})\n${out_link}\n${err_link}")
endif()

execute_process(
  COMMAND "${llvm_exe}"
  RESULT_VARIABLE rc_run_llvm
  OUTPUT_VARIABLE out_run_llvm
  ERROR_VARIABLE err_run_llvm
)
execute_process(
  COMMAND "${lower_exe}"
  RESULT_VARIABLE rc_run_lower
  OUTPUT_VARIABLE out_run_lower
  ERROR_VARIABLE err_run_lower
)

if(NOT rc_run_llvm EQUAL rc_run_lower)
  message(FATAL_ERROR "exit code mismatch: llvm=${rc_run_llvm} lower=${rc_run_lower}\nllvm stdout:\n${out_run_llvm}\nllvm stderr:\n${err_run_llvm}\nlower stdout:\n${out_run_lower}\nlower stderr:\n${err_run_lower}")
endif()

if(NOT out_run_llvm STREQUAL out_run_lower)
  message(FATAL_ERROR "stdout mismatch\nllvm stdout:\n${out_run_llvm}\nlower stdout:\n${out_run_lower}\nllvm stderr:\n${err_run_llvm}\nlower stderr:\n${err_run_lower}")
endif()

if(DEFINED EXPECT_STDOUT)
  string(FIND "${out_run_llvm}" "${EXPECT_STDOUT}" idx)
  if(idx EQUAL -1)
    message(FATAL_ERROR "stdout did not contain expected substring '${EXPECT_STDOUT}'\nstdout:\n${out_run_llvm}\nstderr:\n${err_run_llvm}")
  endif()
endif()

file(SIZE "${llvm_exe}" llvm_size)
file(SIZE "${lower_exe}" lower_size)
message(STATUS "llvm exe size=${llvm_size} lower exe size=${lower_size}")

#
# Optional PGO stage: zem profile (bulk-mem lengths) → lower guided build
#
if(DEFINED ZEM)
  execute_process(
    COMMAND "${ZEM}" --pgo-len-out "${pgo_jsonl}" "${zasm_jsonl}"
    RESULT_VARIABLE rc_zem_pgo
    OUTPUT_VARIABLE out_zem_pgo
    ERROR_VARIABLE err_zem_pgo
  )
  if(NOT rc_zem_pgo EQUAL 0)
    message(FATAL_ERROR "zem --pgo-len-out failed (rc=${rc_zem_pgo})\nstdout:\n${out_zem_pgo}\nstderr:\n${err_zem_pgo}")
  endif()

  execute_process(
    COMMAND "${LOWER}" --input "${zasm_jsonl}" --o "${lower_pgo_obj}" --pgo-len-profile "${pgo_jsonl}"
    RESULT_VARIABLE rc_lower_pgo
    OUTPUT_VARIABLE out_lower_pgo
    ERROR_VARIABLE err_lower_pgo
  )
  if(NOT rc_lower_pgo EQUAL 0)
    message(WARNING "lower (pgo-len-profile) failed (rc=${rc_lower_pgo}); leaving profile at ${pgo_jsonl}\n${out_lower_pgo}\n${err_lower_pgo}")
    return()
  endif()

  execute_process(
    COMMAND "${CLANG}" -o "${lower_pgo_exe}" "${runner_obj}" "${lower_pgo_obj}" "${libzing}"
    RESULT_VARIABLE rc_link_pgo
    OUTPUT_VARIABLE out_link_pgo
    ERROR_VARIABLE err_link_pgo
  )
  if(NOT rc_link_pgo EQUAL 0)
    message(FATAL_ERROR "clang failed to link lower pgo exe (rc=${rc_link_pgo})\n${out_link_pgo}\n${err_link_pgo}")
  endif()

  execute_process(
    COMMAND "${lower_pgo_exe}"
    RESULT_VARIABLE rc_run_lower_pgo
    OUTPUT_VARIABLE out_run_lower_pgo
    ERROR_VARIABLE err_run_lower_pgo
  )

  if(NOT rc_run_llvm EQUAL rc_run_lower_pgo)
    message(FATAL_ERROR "exit code mismatch: llvm=${rc_run_llvm} lower(pgo)=${rc_run_lower_pgo}\nllvm stdout:\n${out_run_llvm}\nllvm stderr:\n${err_run_llvm}\nlower(pgo) stdout:\n${out_run_lower_pgo}\nlower(pgo) stderr:\n${err_run_lower_pgo}")
  endif()

  if(NOT out_run_llvm STREQUAL out_run_lower_pgo)
    message(FATAL_ERROR "stdout mismatch (lower pgo)\nllvm stdout:\n${out_run_llvm}\nlower(pgo) stdout:\n${out_run_lower_pgo}\nllvm stderr:\n${err_run_llvm}\nlower(pgo) stderr:\n${err_run_lower_pgo}")
  endif()

  file(SIZE "${lower_pgo_exe}" lower_pgo_size)
  math(EXPR lower_delta "${lower_pgo_size} - ${lower_size}")
  message(STATUS "lower pgo exe size=${lower_pgo_size} (delta=${lower_delta})")
endif()

#
# Optional strip stage: zem coverage → strip uncovered code → lower stripped IR
#
if(DEFINED ZEM AND DEFINED STRIP_MODE)
  execute_process(
    COMMAND "${ZEM}" --coverage-out "${cov_jsonl}" "${zasm_jsonl}"
    RESULT_VARIABLE rc_zem_cov
    OUTPUT_VARIABLE out_zem_cov
    ERROR_VARIABLE err_zem_cov
  )
  if(NOT rc_zem_cov EQUAL 0)
    message(FATAL_ERROR "zem --coverage-out failed (rc=${rc_zem_cov})\nstdout:\n${out_zem_cov}\nstderr:\n${err_zem_cov}")
  endif()

  execute_process(
    COMMAND "${ZEM}" --strip "${STRIP_MODE}" --strip-profile "${cov_jsonl}" --strip-out "${stripped_zasm_jsonl}" --strip-stats-out "${strip_stats_jsonl}" "${zasm_jsonl}"
    RESULT_VARIABLE rc_zem_strip
    OUTPUT_VARIABLE out_zem_strip
    ERROR_VARIABLE err_zem_strip
  )
  if(NOT rc_zem_strip EQUAL 0)
    message(FATAL_ERROR "zem --strip failed (rc=${rc_zem_strip})\nstdout:\n${out_zem_strip}\nstderr:\n${err_zem_strip}")
  endif()

  execute_process(
    COMMAND "${IRCHECK}" --tool --ir v1.1 "${stripped_zasm_jsonl}"
    RESULT_VARIABLE rc_ircheck_strip
    OUTPUT_VARIABLE out_ircheck_strip
    ERROR_VARIABLE err_ircheck_strip
  )
  if(NOT rc_ircheck_strip EQUAL 0)
    message(FATAL_ERROR "ircheck failed (stripped IR) (rc=${rc_ircheck_strip})\n${out_ircheck_strip}\n${err_ircheck_strip}")
  endif()

  execute_process(
    COMMAND "${LOWER}" --input "${stripped_zasm_jsonl}" --o "${lower_stripped_obj}"
    RESULT_VARIABLE rc_lower_strip
    OUTPUT_VARIABLE out_lower_strip
    ERROR_VARIABLE err_lower_strip
  )
  if(NOT rc_lower_strip EQUAL 0)
    message(FATAL_ERROR "lower failed (stripped IR) (rc=${rc_lower_strip})\n${out_lower_strip}\n${err_lower_strip}")
  endif()

  execute_process(
    COMMAND "${CLANG}" -o "${lower_stripped_exe}" "${runner_obj}" "${lower_stripped_obj}" "${libzing}"
    RESULT_VARIABLE rc_link_strip
    OUTPUT_VARIABLE out_link_strip
    ERROR_VARIABLE err_link_strip
  )
  if(NOT rc_link_strip EQUAL 0)
    message(FATAL_ERROR "clang failed to link lower stripped exe (rc=${rc_link_strip})\n${out_link_strip}\n${err_link_strip}")
  endif()

  execute_process(
    COMMAND "${lower_stripped_exe}"
    RESULT_VARIABLE rc_run_lower_strip
    OUTPUT_VARIABLE out_run_lower_strip
    ERROR_VARIABLE err_run_lower_strip
  )

  if(NOT rc_run_llvm EQUAL rc_run_lower_strip)
    message(FATAL_ERROR "exit code mismatch: llvm=${rc_run_llvm} lower(stripped)=${rc_run_lower_strip}\nllvm stdout:\n${out_run_llvm}\nllvm stderr:\n${err_run_llvm}\nlower(stripped) stdout:\n${out_run_lower_strip}\nlower(stripped) stderr:\n${err_run_lower_strip}")
  endif()

  if(NOT out_run_llvm STREQUAL out_run_lower_strip)
    message(FATAL_ERROR "stdout mismatch (lower stripped)\nllvm stdout:\n${out_run_llvm}\nlower(stripped) stdout:\n${out_run_lower_strip}\nllvm stderr:\n${err_run_llvm}\nlower(stripped) stderr:\n${err_run_lower_strip}")
  endif()

  file(SIZE "${lower_stripped_exe}" lower_strip_size)
  math(EXPR lower_strip_delta "${lower_strip_size} - ${lower_size}")
  message(STATUS "lower stripped exe size=${lower_strip_size} (delta=${lower_strip_delta})")
endif()

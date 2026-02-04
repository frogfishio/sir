if(NOT DEFINED SIRC)
  message(FATAL_ERROR "run_sirc_then_emit_zasm_and_run_zem.cmake: missing -DSIRC")
endif()
if(NOT DEFINED SIRCC)
  message(FATAL_ERROR "run_sirc_then_emit_zasm_and_run_zem.cmake: missing -DSIRCC")
endif()
if(NOT DEFINED ZEM)
  message(FATAL_ERROR "run_sirc_then_emit_zasm_and_run_zem.cmake: missing -DZEM")
endif()
if(NOT DEFINED IRCHECK)
  message(FATAL_ERROR "run_sirc_then_emit_zasm_and_run_zem.cmake: missing -DIRCHECK")
endif()
if(NOT DEFINED INPUT_SIR)
  message(FATAL_ERROR "run_sirc_then_emit_zasm_and_run_zem.cmake: missing -DINPUT_SIR")
endif()
if(NOT DEFINED OUT_PREFIX)
  message(FATAL_ERROR "run_sirc_then_emit_zasm_and_run_zem.cmake: missing -DOUT_PREFIX")
endif()
if(NOT DEFINED EXPECT_STDOUT)
  message(FATAL_ERROR "run_sirc_then_emit_zasm_and_run_zem.cmake: missing -DEXPECT_STDOUT")
endif()

set(out_jsonl "${OUT_PREFIX}.sir.jsonl")
set(out_zasm  "${OUT_PREFIX}.zasm.jsonl")

execute_process(
  COMMAND "${SIRC}" "${INPUT_SIR}" -o "${out_jsonl}"
  RESULT_VARIABLE rc_sirc
  OUTPUT_VARIABLE out_sirc
  ERROR_VARIABLE err_sirc
)
if(NOT rc_sirc EQUAL 0)
  message(FATAL_ERROR "sirc failed (rc=${rc_sirc})\n${out_sirc}\n${err_sirc}")
endif()

set(INPUT "${out_jsonl}")
set(OUTPUT "${out_zasm}")
set(EXPECT_STDOUT "${EXPECT_STDOUT}")

set(zem_script "${CMAKE_CURRENT_LIST_DIR}/../../sircc/tests/emit_zasm_and_run_zem.cmake")
include("${zem_script}")


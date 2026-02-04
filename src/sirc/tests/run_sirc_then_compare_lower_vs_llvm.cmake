if(NOT DEFINED SIRC)
  message(FATAL_ERROR "run_sirc_then_compare_lower_vs_llvm.cmake: missing -DSIRC")
endif()
if(NOT DEFINED SIRCC)
  message(FATAL_ERROR "run_sirc_then_compare_lower_vs_llvm.cmake: missing -DSIRCC")
endif()
if(NOT DEFINED LOWER)
  message(FATAL_ERROR "run_sirc_then_compare_lower_vs_llvm.cmake: missing -DLOWER")
endif()
if(NOT DEFINED IRCHECK)
  message(FATAL_ERROR "run_sirc_then_compare_lower_vs_llvm.cmake: missing -DIRCHECK")
endif()
if(NOT DEFINED ZABI25_ROOT)
  message(FATAL_ERROR "run_sirc_then_compare_lower_vs_llvm.cmake: missing -DZABI25_ROOT")
endif()
if(NOT DEFINED INPUT_SIR)
  message(FATAL_ERROR "run_sirc_then_compare_lower_vs_llvm.cmake: missing -DINPUT_SIR")
endif()
if(NOT DEFINED OUT_PREFIX)
  message(FATAL_ERROR "run_sirc_then_compare_lower_vs_llvm.cmake: missing -DOUT_PREFIX")
endif()

set(out_jsonl "${OUT_PREFIX}.sir.jsonl")

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
set(OUT_PREFIX "${OUT_PREFIX}")

set(compare_script "${CMAKE_CURRENT_LIST_DIR}/../../sircc/tests/compare_llvm_vs_lower.cmake")
include("${compare_script}")

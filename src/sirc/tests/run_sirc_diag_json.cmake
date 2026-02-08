if(NOT DEFINED SIRC OR SIRC STREQUAL "")
  message(FATAL_ERROR "run_sirc_diag_json: missing -DSIRC")
endif()
if(NOT DEFINED INPUT OR INPUT STREQUAL "")
  message(FATAL_ERROR "run_sirc_diag_json: missing -DINPUT")
endif()

execute_process(
  COMMAND ${SIRC} --lint --diagnostics json ${INPUT}
  RESULT_VARIABLE rc
  OUTPUT_VARIABLE out
  ERROR_VARIABLE err
)

# Expect: parse fails, rc=1, and stderr contains JSONL diags.
if(NOT rc EQUAL 1)
  message(STATUS "stdout:\n${out}")
  message(STATUS "stderr:\n${err}")
  message(FATAL_ERROR "expected rc=1 for bad input, got rc=${rc}")
endif()

string(FIND "${err}" "{\"k\":\"diag\"" has_diag)
if(has_diag EQUAL -1)
  message(STATUS "stderr:\n${err}")
  message(FATAL_ERROR "expected JSON diagnostics on stderr")
endif()

string(FIND "${err}" "\"code\":\"sirc.parse.syntax\"" has_code)
if(has_code EQUAL -1)
  message(STATUS "stderr:\n${err}")
  message(FATAL_ERROR "expected sirc.parse.syntax diagnostic code")
endif()


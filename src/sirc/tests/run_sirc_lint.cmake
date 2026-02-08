if(NOT DEFINED SIRC OR SIRC STREQUAL "")
  message(FATAL_ERROR "run_sirc_lint: missing -DSIRC")
endif()
if(NOT DEFINED INPUT OR INPUT STREQUAL "")
  message(FATAL_ERROR "run_sirc_lint: missing -DINPUT")
endif()

execute_process(
  COMMAND ${SIRC} --lint ${INPUT}
  RESULT_VARIABLE rc
  OUTPUT_VARIABLE out
  ERROR_VARIABLE err
)

if(NOT rc EQUAL 0)
  message(STATUS "stdout:\n${out}")
  message(STATUS "stderr:\n${err}")
  message(FATAL_ERROR "sirc --lint failed with rc=${rc}")
endif()


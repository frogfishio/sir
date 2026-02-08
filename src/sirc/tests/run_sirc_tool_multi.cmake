if(NOT DEFINED SIRC OR SIRC STREQUAL "")
  message(FATAL_ERROR "run_sirc_tool_multi: missing -DSIRC")
endif()
if(NOT DEFINED INPUT1 OR INPUT1 STREQUAL "")
  message(FATAL_ERROR "run_sirc_tool_multi: missing -DINPUT1")
endif()
if(NOT DEFINED INPUT2 OR INPUT2 STREQUAL "")
  message(FATAL_ERROR "run_sirc_tool_multi: missing -DINPUT2")
endif()
if(NOT DEFINED OUT OR OUT STREQUAL "")
  message(FATAL_ERROR "run_sirc_tool_multi: missing -DOUT")
endif()

file(REMOVE "${OUT}")

execute_process(
  COMMAND ${SIRC} --tool -o ${OUT} ${INPUT1} ${INPUT2}
  RESULT_VARIABLE rc
  OUTPUT_VARIABLE out
  ERROR_VARIABLE err
)

if(NOT rc EQUAL 0)
  message(STATUS "stdout:\n${out}")
  message(STATUS "stderr:\n${err}")
  message(FATAL_ERROR "sirc --tool failed with rc=${rc}")
endif()

if(NOT EXISTS "${OUT}")
  message(FATAL_ERROR "expected output file to exist: ${OUT}")
endif()

file(READ "${OUT}" contents)
string(REGEX MATCHALL "\"k\":\"meta\"" metas "${contents}")
list(LENGTH metas meta_count)
if(meta_count LESS 2)
  message(STATUS "output:\n${contents}")
  message(FATAL_ERROR "expected >=2 meta records in concatenated output, got ${meta_count}")
endif()


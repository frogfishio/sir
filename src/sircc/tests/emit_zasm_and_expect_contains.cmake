if(NOT DEFINED SIRCC)
  set(SIRCC "sircc")
endif()

if(NOT DEFINED INPUT)
  message(FATAL_ERROR "emit_zasm_and_expect_contains.cmake: missing -DINPUT=... (sir.jsonl)")
endif()
if(NOT DEFINED OUTPUT)
  message(FATAL_ERROR "emit_zasm_and_expect_contains.cmake: missing -DOUTPUT=... (.jsonl)")
endif()
if(NOT DEFINED EXPECT)
  message(FATAL_ERROR "emit_zasm_and_expect_contains.cmake: missing -DEXPECT=... (substring)")
endif()
if(NOT DEFINED EXPECT2)
  set(EXPECT2)
endif()

execute_process(
  COMMAND "${SIRCC}" "${INPUT}" -o "${OUTPUT}" --emit-zasm
  RESULT_VARIABLE rc
  OUTPUT_VARIABLE out
  ERROR_VARIABLE err
)

if(NOT rc EQUAL 0)
  message(FATAL_ERROR "sircc --emit-zasm failed (rc=${rc})\n${out}\n${err}")
endif()

file(READ "${OUTPUT}" content)
string(FIND "${content}" "${EXPECT}" idx)
if(idx EQUAL -1)
  message(FATAL_ERROR "zasm output missing expected substring: ${EXPECT}\noutput:\n${content}")
endif()

if(NOT EXPECT2 STREQUAL "")
  string(FIND "${content}" "${EXPECT2}" idx2)
  if(idx2 EQUAL -1)
    message(FATAL_ERROR "zasm output missing expected substring: ${EXPECT2}\noutput:\n${content}")
  endif()
endif()

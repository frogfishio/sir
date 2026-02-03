if(NOT DEFINED SIRCC)
  set(SIRCC "sircc")
endif()

if(NOT DEFINED ZEM)
  message(FATAL_ERROR "emit_zasm_and_run_zem_expect_exit.cmake: missing -DZEM=... (path to zem)")
endif()

if(NOT DEFINED IRCHECK)
  message(FATAL_ERROR "emit_zasm_and_run_zem_expect_exit.cmake: missing -DIRCHECK=... (path to ircheck)")
endif()

if(NOT DEFINED INPUT)
  message(FATAL_ERROR "emit_zasm_and_run_zem_expect_exit.cmake: missing -DINPUT=... (sir.jsonl)")
endif()
if(NOT DEFINED OUTPUT)
  message(FATAL_ERROR "emit_zasm_and_run_zem_expect_exit.cmake: missing -DOUTPUT=... (.jsonl)")
endif()
if(NOT DEFINED EXPECT_EXIT)
  message(FATAL_ERROR "emit_zasm_and_run_zem_expect_exit.cmake: missing -DEXPECT_EXIT=... (exit code)")
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

execute_process(
  COMMAND "${IRCHECK}" --tool --ir v1.1 "${OUTPUT}"
  RESULT_VARIABLE rc2
  OUTPUT_VARIABLE out2
  ERROR_VARIABLE err2
)

if(NOT rc2 EQUAL 0)
  message(FATAL_ERROR "ircheck failed (rc=${rc2})\n${out2}\n${err2}")
endif()

execute_process(
  COMMAND "${ZEM}" "${OUTPUT}"
  RESULT_VARIABLE rc3
  OUTPUT_VARIABLE out3
  ERROR_VARIABLE err3
)

if(NOT rc3 EQUAL EXPECT_EXIT)
  message(FATAL_ERROR "unexpected zem exit code: got ${rc3}, want ${EXPECT_EXIT}\nstdout:\n${out3}\nstderr:\n${err3}")
endif()


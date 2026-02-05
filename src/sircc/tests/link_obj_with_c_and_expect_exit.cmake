# Expects:
#   -DSIRCC=<path to sircc>
#   -DINPUT=<path to .sir.jsonl>
#   -DHARNESS_C=<path to harness .c>
#   -DOBJ=<path to output .o>
#   -DEXE=<path to output exe>
#   -DEXPECT=<expected exit code>
# Optional:
#   -DCLANG=<path to clang> (default: clang)

if(NOT DEFINED SIRCC)
  message(FATAL_ERROR "link_obj_with_c_and_expect_exit.cmake: missing -DSIRCC")
endif()
if(NOT DEFINED INPUT)
  message(FATAL_ERROR "link_obj_with_c_and_expect_exit.cmake: missing -DINPUT")
endif()
if(NOT DEFINED HARNESS_C)
  message(FATAL_ERROR "link_obj_with_c_and_expect_exit.cmake: missing -DHARNESS_C")
endif()
if(NOT DEFINED OBJ)
  message(FATAL_ERROR "link_obj_with_c_and_expect_exit.cmake: missing -DOBJ")
endif()
if(NOT DEFINED EXE)
  message(FATAL_ERROR "link_obj_with_c_and_expect_exit.cmake: missing -DEXE")
endif()
if(NOT DEFINED EXPECT)
  message(FATAL_ERROR "link_obj_with_c_and_expect_exit.cmake: missing -DEXPECT")
endif()
if(NOT DEFINED CLANG)
  set(CLANG clang)
endif()

execute_process(
  COMMAND "${SIRCC}" --emit-obj "${INPUT}" -o "${OBJ}"
  RESULT_VARIABLE rc_obj
  OUTPUT_VARIABLE out_obj
  ERROR_VARIABLE err_obj
)
if(NOT rc_obj EQUAL 0)
  message(FATAL_ERROR "sircc --emit-obj failed (rc=${rc_obj})\n${out_obj}\n${err_obj}")
endif()

execute_process(
  COMMAND "${CLANG}" -std=c11 -Wall -Wextra -Werror "${HARNESS_C}" "${OBJ}" -o "${EXE}"
  RESULT_VARIABLE rc_link
  OUTPUT_VARIABLE out_link
  ERROR_VARIABLE err_link
)
if(NOT rc_link EQUAL 0)
  message(FATAL_ERROR "clang link failed (rc=${rc_link})\n${out_link}\n${err_link}")
endif()

execute_process(
  COMMAND "${EXE}"
  RESULT_VARIABLE run_rc
)

if(NOT run_rc EQUAL EXPECT)
  message(FATAL_ERROR "unexpected exit code for ${EXE}: got ${run_rc}, want ${EXPECT}")
endif()


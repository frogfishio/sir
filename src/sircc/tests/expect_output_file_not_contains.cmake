# Expects:
#   -DSIRCC=<path to sircc>
#   -DARGS=<cmake list of args>
#   -DOUT=<path to output file>
#   -DNOT_EXPECT=<substring that must NOT appear in output file>
# Optional:
#   -DNOT_EXPECT2=<substring>
#   -DNOT_EXPECT3=<substring>
#   -DNOT_EXPECT4=<substring>
#   -DNOT_EXPECT5=<substring>

if(NOT DEFINED SIRCC)
  message(FATAL_ERROR "expect_output_file_not_contains.cmake: missing -DSIRCC")
endif()
if(NOT DEFINED ARGS)
  set(ARGS)
endif()
if(NOT DEFINED OUT)
  message(FATAL_ERROR "expect_output_file_not_contains.cmake: missing -DOUT")
endif()
if(NOT DEFINED NOT_EXPECT)
  message(FATAL_ERROR "expect_output_file_not_contains.cmake: missing -DNOT_EXPECT")
endif()

execute_process(
  COMMAND ${SIRCC} ${ARGS}
  RESULT_VARIABLE rc
  OUTPUT_VARIABLE out
  ERROR_VARIABLE err
)

if(NOT rc EQUAL 0)
  message(FATAL_ERROR "expected exit code 0, got ${rc}\nstdout:\n${out}\nstderr:\n${err}")
endif()

if(NOT EXISTS "${OUT}")
  message(FATAL_ERROR "expected output file to exist: ${OUT}\nstdout:\n${out}\nstderr:\n${err}")
endif()

file(READ "${OUT}" blob)

foreach(k NOT_EXPECT NOT_EXPECT2 NOT_EXPECT3 NOT_EXPECT4 NOT_EXPECT5)
  if(DEFINED ${k})
    set(exp "${${k}}")
    string(REPLACE "\\\"" "\"" exp "${exp}")
    string(FIND "${blob}" "${exp}" idx)
    if(NOT idx EQUAL -1)
      message(FATAL_ERROR "output file unexpectedly contained '${exp}'\nfile: ${OUT}")
    endif()
  endif()
endforeach()


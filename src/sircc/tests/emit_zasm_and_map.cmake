if(NOT DEFINED SIRCC)
  set(SIRCC "sircc")
endif()

if(NOT DEFINED INPUT)
  message(FATAL_ERROR "emit_zasm_and_map.cmake: missing -DINPUT=... (sir.jsonl)")
endif()
if(NOT DEFINED OUTPUT)
  message(FATAL_ERROR "emit_zasm_and_map.cmake: missing -DOUTPUT=... (.jsonl)")
endif()
if(NOT DEFINED MAP)
  message(FATAL_ERROR "emit_zasm_and_map.cmake: missing -DMAP=... (.map.jsonl)")
endif()

execute_process(
  COMMAND "${SIRCC}" "${INPUT}" -o "${OUTPUT}" --emit-zasm --emit-zasm-map "${MAP}"
  RESULT_VARIABLE rc
  OUTPUT_VARIABLE out
  ERROR_VARIABLE err
)

if(NOT rc EQUAL 0)
  message(FATAL_ERROR "sircc --emit-zasm failed (rc=${rc})\n${out}\n${err}")
endif()

if(NOT EXISTS "${MAP}")
  message(FATAL_ERROR "expected map output does not exist: ${MAP}")
endif()

file(SIZE "${MAP}" map_size)
if(map_size EQUAL 0)
  message(FATAL_ERROR "map output is empty: ${MAP}")
endif()


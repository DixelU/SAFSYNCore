if(NOT DEFINED RENDERER OR NOT DEFINED OUTPUT OR NOT DEFINED EXPECTED)
  message(FATAL_ERROR "RENDERER, OUTPUT, and EXPECTED are required")
endif()

execute_process(
  COMMAND "${RENDERER}" --demo "${OUTPUT}"
  RESULT_VARIABLE render_result
  OUTPUT_VARIABLE render_output
  ERROR_VARIABLE render_error)
if(NOT render_result EQUAL 0)
  message(FATAL_ERROR "coherent demo failed: ${render_error}")
endif()

file(SHA256 "${OUTPUT}" actual)
string(TOUPPER "${actual}" actual_upper)
string(TOUPPER "${EXPECTED}" expected_upper)
if(NOT actual_upper STREQUAL expected_upper)
  message(FATAL_ERROR "coherent demo SHA changed: ${actual}, expected ${EXPECTED}")
endif()

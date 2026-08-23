if(NOT DEFINED BENCH_EXE OR NOT DEFINED OUTPUT_PATH)
  message(FATAL_ERROR "BENCH_EXE and OUTPUT_PATH are required")
endif()

file(REMOVE "${OUTPUT_PATH}")
execute_process(
  COMMAND "${BENCH_EXE}" --json --output "${OUTPUT_PATH}"
  RESULT_VARIABLE bench_result
  OUTPUT_VARIABLE bench_stdout
  ERROR_VARIABLE bench_stderr
)

if(bench_result EQUAL 0)
  message(FATAL_ERROR "missing-model JSON/output request unexpectedly succeeded")
endif()
if(NOT bench_stderr MATCHES "benchmark result unavailable: model not provided")
  message(FATAL_ERROR "missing-model error was not reported: ${bench_stderr}")
endif()
if(NOT bench_stdout STREQUAL "")
  message(FATAL_ERROR "missing-model request contaminated JSON stdout: ${bench_stdout}")
endif()
if(EXISTS "${OUTPUT_PATH}")
  message(FATAL_ERROR "missing-model request unexpectedly created ${OUTPUT_PATH}")
endif()

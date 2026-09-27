execute_process(COMMAND "${BENCH_EXE}" --trace --trace-output "${TRACE_PATH}"
  RESULT_VARIABLE bench_result OUTPUT_VARIABLE bench_output ERROR_VARIABLE bench_error)
if(NOT bench_result EQUAL 0)
  message(FATAL_ERROR "traced benchmark failed: ${bench_error}")
endif()
if(NOT bench_output MATCHES "p50_ms=.*p95_ms=.*p99_ms=")
  message(FATAL_ERROR "benchmark latency output missing: ${bench_output}")
endif()

execute_process(COMMAND "${VIEWER_EXE}" "${TRACE_PATH}" --json
  RESULT_VARIABLE viewer_result OUTPUT_VARIABLE viewer_output ERROR_VARIABLE viewer_error)
if(NOT viewer_result EQUAL 0)
  message(FATAL_ERROR "trace viewer failed: ${viewer_error}")
endif()
string(JSON total_count GET "${viewer_output}" phases total samples)
string(JSON model_count GET "${viewer_output}" phases model_inference samples)
if(NOT total_count EQUAL 200 OR NOT model_count EQUAL 200)
  message(FATAL_ERROR "bench trace missing real host phase: ${viewer_output}")
endif()

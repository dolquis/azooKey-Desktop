file(WRITE "${TRACE_PATH}"
  "{\"trace_id\":\"a\",\"phase\":\"total\",\"latency_ms\":5}\n"
  "{\"trace_id\":\"b\",\"phase\":\"total\",\"latency_ms\":1}\n"
  "{\"trace_id\":\"c\",\"phase\":\"total\",\"latency_ms\":4}\n"
  "{\"trace_id\":\"d\",\"phase\":\"total\",\"latency_ms\":2}\n"
  "{\"trace_id\":\"e\",\"phase\":\"total\",\"latency_ms\":3}\n"
  "{\"trace_id\":\"a\",\"phase\":\"model_inference\",\"latency_ms\":30}\n"
  "{\"trace_id\":\"b\",\"phase\":\"model_inference\",\"latency_ms\":10}\n"
  "{\"trace_id\":\"c\",\"phase\":\"model_inference\",\"latency_ms\":20}\n"
  "not-json\n"
  "{\"trace_id\":\"x\",\"latency_ms\":11}\n"
  "{\"trace_id\":\"x\",\"phase\":\"new_phase\",\"latency_ms\":11}\n"
  "{\"trace_id\":\"x\",\"phase\":\"total\",\"latency_ms\":-1}\n"
  "{\"trace_id\":\"x\",\"phase\":\"total\",\"latency_ms\":\"11\"}\n"
  "{\"trace_id\":\"x\",\"phase\":\"key_down\",\"t_ms\":0}\n"
  "{\"trace_id\":\"x\",\"phase\":\"key_down\",\"latency_ms\":0}\n"
  "\n")

execute_process(COMMAND "${VIEWER_EXE}" "${TRACE_PATH}" --summary --json
  RESULT_VARIABLE json_result OUTPUT_VARIABLE json_output ERROR_VARIABLE json_error)
if(NOT json_result EQUAL 0)
  message(FATAL_ERROR "trace viewer JSON failed: ${json_error}")
endif()
string(JSON version GET "${json_output}" schema_version)
string(JSON samples GET "${json_output}" samples)
string(JSON skipped GET "${json_output}" skipped_lines)
string(JSON total_n GET "${json_output}" phases total samples)
string(JSON total_p50 GET "${json_output}" phases total p50_ms)
string(JSON total_p95 GET "${json_output}" phases total p95_ms)
string(JSON total_p99 GET "${json_output}" phases total p99_ms)
string(JSON model_n GET "${json_output}" phases model_inference samples)
string(JSON model_p50 GET "${json_output}" phases model_inference p50_ms)
if(NOT version EQUAL 1 OR NOT samples EQUAL 5 OR NOT skipped EQUAL 7 OR
   NOT total_n EQUAL 5 OR NOT total_p50 EQUAL 3 OR NOT total_p95 EQUAL 4 OR
   NOT total_p99 EQUAL 4 OR NOT model_n EQUAL 3 OR NOT model_p50 EQUAL 20 OR
   json_output MATCHES "\"key_down\"")
  message(FATAL_ERROR "unexpected trace viewer JSON: ${json_output}")
endif()
if(NOT json_error MATCHES "trace line 9 skipped: invalid JSON object" OR
   NOT json_error MATCHES "trace line 10 skipped: missing or invalid phase" OR
   NOT json_error MATCHES "further skipped-line warnings suppressed")
  message(FATAL_ERROR "missing or unbounded trace warnings: ${json_error}")
endif()

execute_process(COMMAND "${VIEWER_EXE}" "${TRACE_PATH}" --summary
  RESULT_VARIABLE text_result OUTPUT_VARIABLE text_output ERROR_VARIABLE text_error)
if(NOT text_result EQUAL 0 OR
   NOT text_output MATCHES "QueryCandidates latency summary \\(N=5, skipped=7\\)" OR
   NOT text_output MATCHES "total \\(N=5\\): p50=3 ms p95=4 ms p99=4 ms")
  message(FATAL_ERROR "unexpected trace viewer summary: ${text_output} ${text_error}")
endif()

file(WRITE "${TRACE_PATH}.invalid" "not-json\n")
execute_process(COMMAND "${VIEWER_EXE}" "${TRACE_PATH}.invalid" --json
  RESULT_VARIABLE invalid_result OUTPUT_VARIABLE invalid_output ERROR_VARIABLE invalid_error)
if(invalid_result EQUAL 0 OR NOT invalid_error MATCHES "no valid phase samples" OR
   NOT invalid_output STREQUAL "")
  message(FATAL_ERROR "invalid-only trace was accepted: ${invalid_output} ${invalid_error}")
endif()

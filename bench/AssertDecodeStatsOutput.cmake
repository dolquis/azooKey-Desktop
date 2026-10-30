foreach(required_variable BENCH_EXE EVAL_DATA OUTPUT_PATH DECODE_STATS_PATH)
  if(NOT DEFINED ${required_variable})
    message(FATAL_ERROR "${required_variable} is required")
  endif()
endforeach()

file(REMOVE "${OUTPUT_PATH}" "${DECODE_STATS_PATH}")

# Without a model the engine has no decode stats, so each line records null
# instead of a false "not cut off".
execute_process(
  COMMAND "${BENCH_EXE}" --eval "${EVAL_DATA}" --output "${OUTPUT_PATH}" --iterations 2
          --decode-stats "${DECODE_STATS_PATH}"
  RESULT_VARIABLE run_result
  OUTPUT_VARIABLE run_stdout
  ERROR_VARIABLE run_stderr
)
if(NOT run_result EQUAL 0)
  message(FATAL_ERROR "decode stats run failed: ${run_stderr}${run_stdout}")
endif()

file(STRINGS "${DECODE_STATS_PATH}" lines ENCODING UTF-8)
list(LENGTH lines line_count)
# conversion_quality_smoke.jsonl has seven cases; two iterations each.
if(NOT line_count EQUAL 14)
  message(FATAL_ERROR "expected 14 decode stats lines, got ${line_count}")
endif()
set(expected_call 1)
foreach(line IN LISTS lines)
  string(JSON call GET "${line}" call)
  string(JSON input GET "${line}" input)
  string(JSON latency TYPE "${line}" latency_ms)
  string(JSON deadline TYPE "${line}" deadline_exceeded)
  string(JSON last_error TYPE "${line}" last_error)
  string(JSON candidate_count LENGTH "${line}" candidates)
  if(NOT call EQUAL expected_call OR input STREQUAL "" OR NOT latency STREQUAL "NUMBER" OR
     NOT deadline STREQUAL "NULL" OR NOT last_error STREQUAL "NULL")
    message(FATAL_ERROR "unexpected decode stats line: ${line}")
  endif()
  if(candidate_count GREATER 0)
    string(JSON source GET "${line}" candidates 0 source)
    string(JSON unfinished GET "${line}" candidates 0 unfinished)
    if(source STREQUAL "" OR NOT unfinished STREQUAL "OFF")
      message(FATAL_ERROR "unexpected decode stats candidate: ${line}")
    endif()
  endif()
  math(EXPR expected_call "${expected_call} + 1")
endforeach()

execute_process(
  COMMAND "${BENCH_EXE}" --eval "${EVAL_DATA}" --output "${OUTPUT_PATH}" --nll on
  RESULT_VARIABLE nll_without_model_result
  OUTPUT_QUIET ERROR_QUIET
)
if(NOT nll_without_model_result EQUAL 2)
  message(FATAL_ERROR "--nll without --model must exit 2, got ${nll_without_model_result}")
endif()

execute_process(
  COMMAND "${BENCH_EXE}" --decode-stats "${DECODE_STATS_PATH}"
  RESULT_VARIABLE stats_without_eval_result
  OUTPUT_QUIET ERROR_QUIET
)
if(NOT stats_without_eval_result EQUAL 2)
  message(FATAL_ERROR "--decode-stats without --eval must exit 2, got ${stats_without_eval_result}")
endif()

foreach(required_variable
        BENCH_EXE EVAL_DATA OUTPUT_PATH PER_CASE_PATH BASELINE_PATH INCOMPATIBLE_BASELINE_PATH)
  if(NOT DEFINED ${required_variable})
    message(FATAL_ERROR "${required_variable} is required")
  endif()
endforeach()

file(REMOVE "${OUTPUT_PATH}" "${PER_CASE_PATH}" "${BASELINE_PATH}"
            "${INCOMPATIBLE_BASELINE_PATH}")

execute_process(
  COMMAND "${BENCH_EXE}" --eval "${EVAL_DATA}" --output "${OUTPUT_PATH}"
          --per-case "${PER_CASE_PATH}" --iterations 1
  RESULT_VARIABLE first_result
  OUTPUT_VARIABLE first_stdout
  ERROR_VARIABLE first_stderr
)
if(NOT first_result EQUAL 0)
  message(FATAL_ERROR "first conversion quality run failed: ${first_stderr}${first_stdout}")
endif()
file(COPY_FILE "${OUTPUT_PATH}" "${BASELINE_PATH}" ONLY_IF_DIFFERENT)

execute_process(
  COMMAND "${BENCH_EXE}" --eval "${EVAL_DATA}" --output "${OUTPUT_PATH}"
          --per-case "${PER_CASE_PATH}" --baseline "${BASELINE_PATH}" --iterations 1
  RESULT_VARIABLE second_result
  OUTPUT_VARIABLE second_stdout
  ERROR_VARIABLE second_stderr
)
if(NOT second_result EQUAL 0)
  message(FATAL_ERROR "baseline comparison run failed: ${second_stderr}${second_stdout}")
endif()

file(READ "${OUTPUT_PATH}" report)
string(JSON version GET "${report}" version)
string(JSON exact_rate GET "${report}" summary exact_match_rate)
string(JSON nfkc_rate GET "${report}" summary nfkc_exact_match_rate)
string(JSON cer GET "${report}" summary cer)
string(JSON nfkc_cer GET "${report}" summary nfkc_cer)
string(JSON baseline_diff GET "${report}" diff_vs_baseline top1_accuracy)
string(JSON typo_false_positive_rate GET "${report}" summary typo_false_positive_rate)
string(JSON typo_overcorrection_rate GET "${report}" summary typo_overcorrection_rate)
string(FIND "${report}" "\"typo_clean\":" typo_clean_category_position)
if(NOT version EQUAL 1 OR exact_rate LESS 0.833 OR exact_rate GREATER 0.834 OR
   nfkc_rate LESS 0.833 OR nfkc_rate GREATER 0.834 OR
   cer LESS 0.1428 OR cer GREATER 0.1429 OR
   nfkc_cer LESS 0.1428 OR nfkc_cer GREATER 0.1429 OR
   NOT baseline_diff EQUAL 0 OR
   NOT typo_false_positive_rate EQUAL 0 OR NOT typo_overcorrection_rate EQUAL 0 OR
   NOT typo_clean_category_position EQUAL -1)
  message(FATAL_ERROR "unexpected conversion quality summary: ${report}")
endif()

file(STRINGS "${PER_CASE_PATH}" per_case_lines ENCODING UTF-8)
list(LENGTH per_case_lines per_case_count)
if(NOT per_case_count EQUAL 7)
  message(FATAL_ERROR "expected seven per-case records, got ${per_case_count}")
endif()

string(REPLACE "\"category_filter\":\"all\"" "\"category_filter\":\"incompatible\""
       incompatible_baseline "${report}")
if("${incompatible_baseline}" STREQUAL "${report}")
  message(FATAL_ERROR "failed to construct incompatible baseline fixture")
endif()
file(WRITE "${INCOMPATIBLE_BASELINE_PATH}" "${incompatible_baseline}")
execute_process(
  COMMAND "${BENCH_EXE}" --eval "${EVAL_DATA}" --output "${OUTPUT_PATH}"
          --baseline "${INCOMPATIBLE_BASELINE_PATH}" --iterations 1
  RESULT_VARIABLE incompatible_result
  OUTPUT_VARIABLE incompatible_stdout
  ERROR_VARIABLE incompatible_stderr
)
if(incompatible_result EQUAL 0)
  message(FATAL_ERROR "incompatible baseline was accepted")
endif()
string(FIND "${incompatible_stderr}${incompatible_stdout}"
       "baseline is incompatible: category_filter" incompatible_message_position)
if(incompatible_message_position EQUAL -1)
  message(FATAL_ERROR
          "unexpected incompatible baseline error: ${incompatible_stderr}${incompatible_stdout}")
endif()

file(REMOVE "${OUTPUT_PATH}" "${PER_CASE_PATH}" "${BASELINE_PATH}"
            "${INCOMPATIBLE_BASELINE_PATH}")

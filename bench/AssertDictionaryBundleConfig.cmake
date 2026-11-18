# --dict-dir and --lattice (conversion-quality-benchmark-spec sections 7 and 14.1): both runs
# record the compatibility keys, and a run with the bundled dictionaries does not compare
# against one without them.
foreach(required_variable BENCH_EXE EVAL_DATA DICT_DIR OUTPUT_PATH BASELINE_PATH)
  if(NOT DEFINED ${required_variable})
    message(FATAL_ERROR "${required_variable} is required")
  endif()
endforeach()

file(REMOVE "${OUTPUT_PATH}" "${BASELINE_PATH}")

function(run_bench name)
  execute_process(
    COMMAND "${BENCH_EXE}" --eval "${EVAL_DATA}" --output "${OUTPUT_PATH}" --iterations 1 ${ARGN}
    RESULT_VARIABLE result
    OUTPUT_VARIABLE stdout
    ERROR_VARIABLE stderr
  )
  set(${name}_result "${result}" PARENT_SCOPE)
  set(${name}_output "${stderr}${stdout}" PARENT_SCOPE)
endfunction()

function(expect_config expected_sha expected_lattice)
  file(READ "${OUTPUT_PATH}" report)
  string(JSON sha GET "${report}" config dictionary_bundle_sha256)
  string(JSON lattice GET "${report}" config lattice)
  string(JSON p99 GET "${report}" summary latency_p99_ms)
  string(JSON katakana_n GET "${report}" by_subset katakana n)
  if(expected_sha STREQUAL "hash")
    if(NOT sha MATCHES "^[0-9a-f]+$")
      message(FATAL_ERROR "expected a dictionary bundle SHA-256, got ${sha}")
    endif()
    string(LENGTH "${sha}" sha_length)
    if(NOT sha_length EQUAL 64)
      message(FATAL_ERROR "expected a dictionary bundle SHA-256, got ${sha}")
    endif()
  elseif(NOT sha STREQUAL expected_sha)
    message(FATAL_ERROR "expected dictionary_bundle_sha256=${expected_sha}, got ${sha}")
  endif()
  if(NOT lattice STREQUAL expected_lattice OR p99 LESS 0 OR NOT katakana_n EQUAL 0)
    message(FATAL_ERROR "unexpected config or summary: ${report}")
  endif()
  set(last_sha "${sha}" PARENT_SCOPE)
endfunction()

function(expect_failure name message_text)
  if(${name}_result EQUAL 0)
    message(FATAL_ERROR "${name} run was accepted")
  endif()
  string(FIND "${${name}_output}" "${message_text}" position)
  if(position EQUAL -1)
    message(FATAL_ERROR "unexpected ${name} error: ${${name}_output}")
  endif()
endfunction()

run_bench(plain)
if(NOT plain_result EQUAL 0)
  message(FATAL_ERROR "run without --dict-dir failed: ${plain_output}")
endif()
expect_config("none" "off")
file(COPY_FILE "${OUTPUT_PATH}" "${BASELINE_PATH}" ONLY_IF_DIFFERENT)

run_bench(bundled --dict-dir "${DICT_DIR}" --lattice off)
if(NOT bundled_result EQUAL 0)
  message(FATAL_ERROR "run with --dict-dir failed: ${bundled_output}")
endif()
expect_config("hash" "off")
set(first_sha "${last_sha}")
run_bench(repeated --dict-dir "${DICT_DIR}" --lattice off)
expect_config("${first_sha}" "off")

run_bench(compared --dict-dir "${DICT_DIR}" --lattice off --baseline "${BASELINE_PATH}")
expect_failure(compared "baseline is incompatible: dictionary_bundle_sha256")

# --dict-dir defaults to --lattice on, which needs lattice conversion.
run_bench(default_on --dict-dir "${DICT_DIR}")
expect_failure(default_on "--lattice on needs lattice conversion")
run_bench(on_without_dictionary --lattice on)
expect_failure(on_without_dictionary "--lattice on requires --dict-dir")
run_bench(empty_directory --dict-dir "${CMAKE_CURRENT_LIST_DIR}" --lattice off)
expect_failure(empty_directory "--dict-dir holds no .azdic file")

file(REMOVE "${OUTPUT_PATH}" "${BASELINE_PATH}")

# --dict-dir and --lattice (conversion-quality-benchmark-spec sections 7 and 14.1): the bundled
# dictionaries change the candidates, both runs record the compatibility keys, a run with the
# bundled dictionaries does not compare against one without them, and bad input exits 2.
foreach(required_variable BENCH_EXE FIXTURE_DIR WORK_DIR)
  if(NOT DEFINED ${required_variable})
    message(FATAL_ERROR "${required_variable} is required")
  endif()
endforeach()

set(DICT_DIR "${FIXTURE_DIR}/ambiguous")
set(OUTPUT_PATH "${WORK_DIR}/result.json")
set(BASELINE_PATH "${WORK_DIR}/baseline.json")
# Only the fixture dictionary knows this reading; the built-in converter returns it unconverted.
set(EVAL_DATA "${WORK_DIR}/bench/data/dict_dir_smoke.jsonl")
file(REMOVE_RECURSE "${WORK_DIR}")
file(WRITE "${EVAL_DATA}"
     "{\"id\":\"dict_dir_smoke_001\",\"input\":\"げんいん\",\"expected\":\"原因\","
     "\"category\":[\"general\"],\"provenance\":\"authored\"}\n")
file(MAKE_DIRECTORY "${WORK_DIR}/empty" "${WORK_DIR}/broken" "${WORK_DIR}/no_layer")
file(COPY_FILE "${FIXTURE_DIR}/magic.azdic" "${WORK_DIR}/broken/sudachi_lexicon.azdic")
file(COPY_FILE "${FIXTURE_DIR}/valid.azdic" "${WORK_DIR}/no_layer/other.azdic")

function(run_bench name)
  file(REMOVE "${OUTPUT_PATH}")
  execute_process(
    COMMAND "${BENCH_EXE}" --eval "${EVAL_DATA}" --output "${OUTPUT_PATH}" --iterations 1 ${ARGN}
    RESULT_VARIABLE result
    OUTPUT_VARIABLE stdout
    ERROR_VARIABLE stderr
  )
  set(${name}_result "${result}" PARENT_SCOPE)
  set(${name}_output "${stderr}${stdout}" PARENT_SCOPE)
endfunction()

function(expect_success name)
  if(NOT ${name}_result EQUAL 0)
    message(FATAL_ERROR "${name} run failed: ${${name}_output}")
  endif()
endfunction()

function(expect_config expected_sha expected_lattice expected_top1)
  file(READ "${OUTPUT_PATH}" report)
  string(JSON sha GET "${report}" config dictionary_bundle_sha256)
  string(JSON lattice GET "${report}" config lattice)
  string(JSON top1 GET "${report}" summary top1_accuracy)
  string(JSON p99 GET "${report}" summary latency_p99_ms)
  string(JSON katakana_n GET "${report}" by_subset katakana n)
  if(expected_sha STREQUAL "hash")
    string(LENGTH "${sha}" sha_length)
    if(NOT sha MATCHES "^[0-9a-f]+$" OR NOT sha_length EQUAL 64)
      message(FATAL_ERROR "expected a dictionary bundle SHA-256, got ${sha}")
    endif()
  elseif(NOT sha STREQUAL expected_sha)
    message(FATAL_ERROR "expected dictionary_bundle_sha256=${expected_sha}, got ${sha}")
  endif()
  if(NOT lattice STREQUAL expected_lattice OR NOT top1 EQUAL expected_top1 OR p99 LESS 0 OR
     NOT katakana_n EQUAL 0)
    message(FATAL_ERROR "unexpected config or summary: ${report}")
  endif()
  set(last_sha "${sha}" PARENT_SCOPE)
endfunction()

function(expect_failure name message_text)
  if(NOT ${name}_result EQUAL 2)
    message(FATAL_ERROR "${name} run should exit 2, got ${${name}_result}: ${${name}_output}")
  endif()
  string(FIND "${${name}_output}" "${message_text}" position)
  if(position EQUAL -1)
    message(FATAL_ERROR "unexpected ${name} error: ${${name}_output}")
  endif()
endfunction()

run_bench(plain)
expect_success(plain)
expect_config("none" "off" 0)
file(COPY_FILE "${OUTPUT_PATH}" "${BASELINE_PATH}")

run_bench(bundled --dict-dir "${DICT_DIR}" --lattice off)
expect_success(bundled)
expect_config("hash" "off" 1)
set(first_sha "${last_sha}")
run_bench(repeated --dict-dir "${DICT_DIR}" --lattice off)
expect_success(repeated)
expect_config("${first_sha}" "off" 1)

run_bench(compared --dict-dir "${DICT_DIR}" --lattice off --baseline "${BASELINE_PATH}")
expect_failure(compared "baseline is incompatible: dictionary_bundle_sha256")

# --dict-dir defaults to --lattice on, which needs lattice conversion.
run_bench(default_on --dict-dir "${DICT_DIR}")
expect_failure(default_on "--lattice on needs lattice conversion")
run_bench(on_without_dictionary --lattice on)
expect_failure(on_without_dictionary "--lattice on requires --dict-dir")
run_bench(empty_directory --dict-dir "${WORK_DIR}/empty" --lattice off)
expect_failure(empty_directory "--dict-dir holds no .azdic file")
run_bench(broken_layer --dict-dir "${WORK_DIR}/broken" --lattice off)
expect_failure(broken_layer "dictionary layer sudachi_lexicon.azdic failed to load")
run_bench(no_layer --dict-dir "${WORK_DIR}/no_layer" --lattice off)
expect_failure(no_layer "--dict-dir holds no bundled dictionary layer")

file(REMOVE_RECURSE "${WORK_DIR}")

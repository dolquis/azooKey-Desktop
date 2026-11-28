# --dict-dir and --lattice (conversion-quality-benchmark-spec sections 7 and 14.1): the bundled
# dictionaries change the candidates, both runs record the compatibility keys, a run with the
# bundled dictionaries does not compare against one without them, --lattice on converts a
# sentence --lattice off does not, and bad input exits 2.
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
file(MAKE_DIRECTORY "${WORK_DIR}/empty" "${WORK_DIR}/broken" "${WORK_DIR}/no_layer"
     "${WORK_DIR}/lattice" "${WORK_DIR}/broken_lattice")
file(COPY_FILE "${FIXTURE_DIR}/magic.azdic" "${WORK_DIR}/broken/sudachi_lexicon.azdic")
file(COPY_FILE "${FIXTURE_DIR}/valid.azdic" "${WORK_DIR}/no_layer/other.azdic")
# DEV-2101: the sentence fixture of the lattice, whole and with a lattice_core lacking CMAT.
foreach(directory lattice broken_lattice)
  file(COPY_FILE "${FIXTURE_DIR}/lattice/sudachi_lexicon.azdic"
       "${WORK_DIR}/${directory}/sudachi_lexicon.azdic")
endforeach()
file(COPY_FILE "${FIXTURE_DIR}/lattice/lattice_core.azdic" "${WORK_DIR}/lattice/lattice_core.azdic")
file(COPY_FILE "${FIXTURE_DIR}/lattice/core_no_cmat.azdic"
     "${WORK_DIR}/broken_lattice/lattice_core.azdic")
# Only the lattice splits this sentence; the dictionary has no word for the whole reading.
set(LATTICE_EVAL_DATA "${WORK_DIR}/bench/data/lattice_smoke.jsonl")
file(WRITE "${LATTICE_EVAL_DATA}"
     "{\"id\":\"lattice_smoke_001\",\"input\":\"きょうはいいてんきです\","
     "\"expected\":\"今日はいい天気です\",\"category\":[\"general\"],\"provenance\":\"authored\"}\n")

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

# --dict-dir defaults to --lattice on, which needs lattice_core.azdic (lattice spec section 10.1).
run_bench(default_on --dict-dir "${DICT_DIR}")
expect_failure(default_on "--lattice on could not load lattice_core.azdic")
run_bench(broken_lattice --dict-dir "${WORK_DIR}/broken_lattice" --lattice on)
expect_failure(broken_lattice "--lattice on could not load lattice_core.azdic")
# A and B on the same dictionaries: only B converts the sentence.
set(EVAL_DATA "${LATTICE_EVAL_DATA}")
run_bench(lattice_off --dict-dir "${WORK_DIR}/lattice" --lattice off)
expect_success(lattice_off)
expect_config("hash" "off" 0)
set(lattice_sha "${last_sha}")
run_bench(lattice_on --dict-dir "${WORK_DIR}/lattice")
expect_success(lattice_on)
expect_config("${lattice_sha}" "on" 1)
run_bench(on_without_dictionary --lattice on)
expect_failure(on_without_dictionary "--lattice on requires --dict-dir")
# Stage 2 (lattice spec section 3.3, DEV-2102) is measured with the model and NLL only.
run_bench(stage_two_without_model --dict-dir "${WORK_DIR}/lattice" --lattice-with-model 4)
expect_failure(stage_two_without_model
               "--lattice-with-model requires --eval, --model, --nll on and --lattice on")
run_bench(stage_two_band_alone --dict-dir "${WORK_DIR}/lattice" --lattice-band lowest)
expect_failure(stage_two_band_alone "--lattice-band requires --lattice-with-model")
run_bench(stage_two_too_many --dict-dir "${WORK_DIR}/lattice" --lattice-with-model 9)
expect_failure(stage_two_too_many "--lattice-with-model must be 1 to 8")
run_bench(empty_directory --dict-dir "${WORK_DIR}/empty" --lattice off)
expect_failure(empty_directory "--dict-dir holds no .azdic file")
run_bench(broken_layer --dict-dir "${WORK_DIR}/broken" --lattice off)
expect_failure(broken_layer "dictionary layer sudachi_lexicon.azdic failed to load")
run_bench(no_layer --dict-dir "${WORK_DIR}/no_layer" --lattice off)
expect_failure(no_layer "--dict-dir holds no bundled dictionary layer")

file(REMOVE_RECURSE "${WORK_DIR}")

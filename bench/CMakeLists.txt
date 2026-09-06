set(AZOOKEY_BENCH_GIT_COMMIT "" CACHE STRING
    "Git commit recorded in benchmark JSON; defaults to HEAD at build time")
set(AZOOKEY_BENCH_GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
set(AZOOKEY_BENCH_COMMIT_HEADER "${AZOOKEY_BENCH_GENERATED_DIR}/BenchmarkCommit.h")
add_custom_target(azookey_benchmark_commit_header
  COMMAND "${CMAKE_COMMAND}"
          "-DSOURCE_DIR=${CMAKE_SOURCE_DIR}"
          "-DOUTPUT_FILE=${AZOOKEY_BENCH_COMMIT_HEADER}"
          "-DAZOOKEY_BENCH_GIT_COMMIT_OVERRIDE=${AZOOKEY_BENCH_GIT_COMMIT}"
          -P "${CMAKE_CURRENT_SOURCE_DIR}/GenerateBenchmarkCommit.cmake"
  BYPRODUCTS "${AZOOKEY_BENCH_COMMIT_HEADER}"
  VERBATIM
)

add_library(azookey_benchmark_result BenchmarkCommandLine.cpp BenchmarkResult.cpp)
target_include_directories(azookey_benchmark_result PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(azookey_benchmark_result
  PUBLIC azookey_core azookey_ipc
  PRIVATE azookey_project_options azookey_project_warnings
)
if(WIN32)
  target_link_libraries(azookey_benchmark_result PRIVATE shell32)
endif()

add_library(azookey_conversion_quality STATIC ConversionQuality.cpp)
target_include_directories(azookey_conversion_quality PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(azookey_conversion_quality
  PUBLIC azookey_core
  PRIVATE azookey_ipc azookey_project_options azookey_project_warnings
)
if(WIN32)
  target_link_libraries(azookey_conversion_quality PRIVATE bcrypt psapi)
endif()

add_executable(azookey_bench live_bench.cpp)
add_dependencies(azookey_bench azookey_benchmark_commit_header)
target_include_directories(azookey_bench PRIVATE "${AZOOKEY_BENCH_GENERATED_DIR}")
target_link_libraries(azookey_bench
  PRIVATE azookey_benchmark_result azookey_conversion_quality azookey_host
          azookey_project_options azookey_project_warnings
)
target_compile_definitions(azookey_bench PRIVATE
  AZOOKEY_BENCH_CONFIG="$<IF:$<BOOL:$<CONFIG>>,$<CONFIG>,unknown>"
)

add_executable(azookey_zenzai_bench zenzai_bench.cpp)
add_dependencies(azookey_zenzai_bench azookey_benchmark_commit_header)
target_include_directories(azookey_zenzai_bench PRIVATE "${AZOOKEY_BENCH_GENERATED_DIR}")
target_link_libraries(azookey_zenzai_bench
  PRIVATE azookey_benchmark_result azookey_host azookey_project_options azookey_project_warnings
)
target_compile_definitions(azookey_zenzai_bench PRIVATE
  AZOOKEY_BENCH_CONFIG="$<IF:$<BOOL:$<CONFIG>>,$<CONFIG>,unknown>"
)

set(AZOOKEY_ZENZAI_TEST_MODEL "" CACHE FILEPATH
    "Path to a real Zenzai GGUF used by the llama.cpp CTest smoke")

if(AZOOKEY_BUILD_TESTS)
  add_executable(benchmark_result_tests benchmark_result_test.cpp)
  target_link_libraries(benchmark_result_tests
    PRIVATE azookey_benchmark_result GTest::gtest_main
            azookey_project_options azookey_project_warnings
  )
  azookey_discover_tests(benchmark_result_tests)

  add_executable(conversion_quality_tests conversion_quality_test.cpp)
  target_link_libraries(conversion_quality_tests
    PRIVATE azookey_conversion_quality GTest::gtest_main
            azookey_project_options azookey_project_warnings
  )
  azookey_discover_tests(conversion_quality_tests)

  set_property(GLOBAL APPEND PROPERTY AZOOKEY_TEST_TARGETS
    azookey_bench
    azookey_zenzai_bench
  )
  add_test(NAME azookey_bench_smoke COMMAND azookey_bench --max-p95-ms 50)
  set_tests_properties(azookey_bench_smoke PROPERTIES TIMEOUT 10)
  add_test(NAME azookey_bench_json_smoke
           COMMAND azookey_bench --json --max-p95-ms 50)
  set_tests_properties(azookey_bench_json_smoke PROPERTIES
    PASS_REGULAR_EXPRESSION
      "\"bench\":\"azookey_bench\".*\"schemaVersion\":1.*\"passed\":true"
    FAIL_REGULAR_EXPRESSION "p50_ms="
    TIMEOUT 10
  )
  if(WIN32)
    add_test(NAME azookey_conversion_quality_smoke
             COMMAND "${CMAKE_COMMAND}"
                     "-DBENCH_EXE=$<TARGET_FILE:azookey_bench>"
                     "-DEVAL_DATA=${CMAKE_CURRENT_SOURCE_DIR}/data/conversion_quality_smoke.jsonl"
                     "-DOUTPUT_PATH=${CMAKE_CURRENT_BINARY_DIR}/conversion-quality-smoke.json"
                     "-DPER_CASE_PATH=${CMAKE_CURRENT_BINARY_DIR}/conversion-quality-smoke.jsonl"
                     "-DBASELINE_PATH=${CMAKE_CURRENT_BINARY_DIR}/conversion-quality-baseline.json"
                     "-DINCOMPATIBLE_BASELINE_PATH=${CMAKE_CURRENT_BINARY_DIR}/conversion-quality-incompatible-baseline.json"
                     -P "${CMAKE_CURRENT_SOURCE_DIR}/AssertConversionQualityOutput.cmake")
    set_tests_properties(azookey_conversion_quality_smoke PROPERTIES TIMEOUT 10)
  endif()
  add_test(NAME azookey_zenzai_bench_missing_model_output
           COMMAND "${CMAKE_COMMAND}"
                   "-DBENCH_EXE=$<TARGET_FILE:azookey_zenzai_bench>"
                   "-DOUTPUT_PATH=${CMAKE_CURRENT_BINARY_DIR}/missing-model-result.json"
                   -P "${CMAKE_CURRENT_SOURCE_DIR}/AssertMissingModelOutput.cmake")
  set_tests_properties(azookey_zenzai_bench_missing_model_output PROPERTIES
    TIMEOUT 10
    ENVIRONMENT "AZOOKEY_ZENZAI_MODEL="
  )
  if(AZOOKEY_WITH_LLAMA_CPP AND AZOOKEY_ZENZAI_TEST_MODEL)
    add_test(NAME azookey_zenzai_real_model_nihongo_smoke
             COMMAND azookey_zenzai_bench
                     --model "${AZOOKEY_ZENZAI_TEST_MODEL}"
                     --input "にほんご"
                     --expected-prompt-token-ids "172,120,202,444,677,259,796,172,120,203"
                     --iterations 1 --warmup 0 --require-model --require-zenzai)
    set_tests_properties(azookey_zenzai_real_model_nihongo_smoke PROPERTIES
      LABELS "zenzai-real-model"
      PASS_REGULAR_EXPRESSION "zenzai_candidates=[1-9][0-9]* top_surface=日本語 "
      FAIL_REGULAR_EXPRESSION "utf8-prefix-trimmed"
      RUN_SERIAL TRUE
      TIMEOUT 120
    )
    add_test(NAME azookey_zenzai_real_model_sentence_smoke
             COMMAND azookey_zenzai_bench
                     --model "${AZOOKEY_ZENZAI_TEST_MODEL}"
                     --input "わたしはがくせいです"
                     --expected-prompt-token-ids "172,120,202,628,327,330,623,643,299,492,280,406,271,172,120,203"
                     --iterations 1 --warmup 0 --require-model --require-zenzai)
    set_tests_properties(azookey_zenzai_real_model_sentence_smoke PROPERTIES
      LABELS "zenzai-real-model"
      PASS_REGULAR_EXPRESSION "zenzai_candidates=[1-9][0-9]* top_surface=私は学生です "
      FAIL_REGULAR_EXPRESSION "utf8-prefix-trimmed"
      RUN_SERIAL TRUE
      TIMEOUT 120
    )
    add_test(NAME azookey_zenzai_real_model_json_smoke
             COMMAND azookey_zenzai_bench
                     --model "${AZOOKEY_ZENZAI_TEST_MODEL}"
                     --input "にほんご"
                     --iterations 1 --warmup 0 --require-model --require-zenzai --json)
    set_tests_properties(azookey_zenzai_real_model_json_smoke PROPERTIES
      LABELS "zenzai-real-model"
      PASS_REGULAR_EXPRESSION
        "\"bench\":\"azookey_zenzai_bench\".*\"schemaVersion\":1.*\"passed\":true"
      FAIL_REGULAR_EXPRESSION "status=ok"
      RUN_SERIAL TRUE
      TIMEOUT 120
    )
  elseif(NOT AZOOKEY_WITH_LLAMA_CPP)
    add_test(NAME azookey_zenzai_bench_smoke
             COMMAND azookey_zenzai_bench --mock-zenzai --iterations 20 --warmup 2)
    set_tests_properties(azookey_zenzai_bench_smoke PROPERTIES
      TIMEOUT 10
      ENVIRONMENT "AZOOKEY_ZENZAI_MODEL=azookey_zenzai_bench_missing_env_model.gguf"
    )
    add_test(NAME azookey_zenzai_bench_json_smoke
             COMMAND azookey_zenzai_bench --mock-zenzai --iterations 20 --warmup 2 --json)
    set_tests_properties(azookey_zenzai_bench_json_smoke PROPERTIES
      PASS_REGULAR_EXPRESSION
        "\"bench\":\"azookey_zenzai_bench\".*\"schemaVersion\":1.*\"passed\":true"
      FAIL_REGULAR_EXPRESSION "status=ok"
      TIMEOUT 10
      ENVIRONMENT "AZOOKEY_ZENZAI_MODEL=azookey_zenzai_bench_missing_env_model.gguf"
    )
    add_test(NAME azookey_zenzai_bench_rejects_degraded_mock
             COMMAND azookey_zenzai_bench --mock-zenzai --input ascii
                     --iterations 1 --warmup 0 --require-zenzai)
    set_tests_properties(azookey_zenzai_bench_rejects_degraded_mock PROPERTIES
      TIMEOUT 10
      WILL_FAIL TRUE
      ENVIRONMENT "AZOOKEY_ZENZAI_MODEL=azookey_zenzai_bench_missing_env_model.gguf"
    )
  endif()
endif()

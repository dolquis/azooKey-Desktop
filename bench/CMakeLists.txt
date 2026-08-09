add_executable(azookey_bench live_bench.cpp)
target_link_libraries(azookey_bench
  PRIVATE azookey_host azookey_project_options azookey_project_warnings
)

add_executable(azookey_zenzai_bench zenzai_bench.cpp)
target_link_libraries(azookey_zenzai_bench
  PRIVATE azookey_host azookey_project_options azookey_project_warnings
)

set(AZOOKEY_ZENZAI_TEST_MODEL "" CACHE FILEPATH
    "Path to a real Zenzai GGUF used by the llama.cpp CTest smoke")

if(AZOOKEY_BUILD_TESTS)
  set_property(GLOBAL APPEND PROPERTY AZOOKEY_TEST_TARGETS
    azookey_bench
    azookey_zenzai_bench
  )
  add_test(NAME azookey_bench_smoke COMMAND azookey_bench --max-p95-ms 50)
  set_tests_properties(azookey_bench_smoke PROPERTIES TIMEOUT 10)
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
      TIMEOUT 120
    )
  elseif(NOT AZOOKEY_WITH_LLAMA_CPP)
    add_test(NAME azookey_zenzai_bench_smoke
             COMMAND azookey_zenzai_bench --mock-zenzai --iterations 20 --warmup 2)
    set_tests_properties(azookey_zenzai_bench_smoke PROPERTIES
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

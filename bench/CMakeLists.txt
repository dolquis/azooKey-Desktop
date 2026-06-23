add_executable(azookey_bench live_bench.cpp)
target_link_libraries(azookey_bench
  PRIVATE azookey_host azookey_project_options azookey_project_warnings
)

add_executable(azookey_zenzai_bench zenzai_bench.cpp)
target_link_libraries(azookey_zenzai_bench
  PRIVATE azookey_host azookey_project_options azookey_project_warnings
)

if(AZOOKEY_BUILD_TESTS)
  add_test(NAME azookey_bench_smoke COMMAND azookey_bench --max-p95-ms 50)
  set_tests_properties(azookey_bench_smoke PROPERTIES TIMEOUT 10)
  if(NOT AZOOKEY_WITH_LLAMA_CPP)
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

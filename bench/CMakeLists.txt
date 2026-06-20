add_executable(azookey_bench live_bench.cpp)
target_link_libraries(azookey_bench
  PRIVATE azookey_host azookey_project_options azookey_project_warnings
)

add_executable(azookey_zenzai_bench zenzai_bench.cpp)
target_link_libraries(azookey_zenzai_bench
  PRIVATE azookey_host azookey_project_options azookey_project_warnings
)

if(AZOOKEY_BUILD_TESTS)
  add_test(NAME azookey_bench_smoke COMMAND azookey_bench)
  set_tests_properties(azookey_bench_smoke PROPERTIES TIMEOUT 10)
  add_test(NAME azookey_zenzai_bench_smoke
           COMMAND azookey_zenzai_bench --mock-zenzai --iterations 20 --warmup 2)
  set_tests_properties(azookey_zenzai_bench_smoke PROPERTIES TIMEOUT 10)
endif()

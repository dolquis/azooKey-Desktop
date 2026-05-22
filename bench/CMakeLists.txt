add_executable(azookey_bench live_bench.cpp)
target_link_libraries(azookey_bench PRIVATE azookey_host)
add_test(NAME bench_smoke COMMAND azookey_bench)

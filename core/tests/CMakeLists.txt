add_executable(core_tests
  live_conversion_chunker_test.cpp
  number_rewriter_test.cpp
  redaction_test.cpp
  romaji_kana_converter_test.cpp
  simple_converter_test.cpp
)

target_link_libraries(core_tests
  PRIVATE azookey_core GTest::gtest_main azookey_project_options azookey_project_warnings
)

azookey_discover_tests(core_tests)

add_executable(runtime_logger_tests
  runtime_logger_test.cpp
)

target_link_libraries(runtime_logger_tests
  PRIVATE azookey_core GTest::gtest_main azookey_project_options azookey_project_warnings
)

# RuntimeLogger uses a component-scoped OS mutex, so logger cases must not compete
# with one another and hit the best-effort write timeout under parallel CTest.
azookey_discover_tests(runtime_logger_tests RESOURCE_LOCK azookey-runtime-log)

add_executable(core_tests
  romaji_kana_converter_test.cpp
  simple_converter_test.cpp
)

target_link_libraries(core_tests
  PRIVATE azookey_core GTest::gtest_main azookey_project_options azookey_project_warnings
)

azookey_discover_tests(core_tests)

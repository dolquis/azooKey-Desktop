add_executable(diagnostics_tests diagnostics_test.cpp)
target_link_libraries(diagnostics_tests
  PRIVATE azookey_diagnostics azookey_learning GTest::gtest_main
          azookey_project_options azookey_project_warnings
)
target_compile_definitions(diagnostics_tests
  PRIVATE AZOOKEY_TEST_SOURCE_DIR="${PROJECT_SOURCE_DIR}"
)
add_dependencies(diagnostics_tests azookey_diag)
azookey_discover_tests(diagnostics_tests)

add_test(
  NAME diagnostics_cli_rejects_help_with_json
  COMMAND $<TARGET_FILE:azookey_diag> --help --json
)
set_tests_properties(diagnostics_cli_rejects_help_with_json PROPERTIES WILL_FAIL TRUE)

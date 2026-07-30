add_executable(diagnostics_tests diagnostics_test.cpp)
target_link_libraries(diagnostics_tests
  PRIVATE azookey_diagnostics GTest::gtest_main
          azookey_project_options azookey_project_warnings
)
azookey_discover_tests(diagnostics_tests)

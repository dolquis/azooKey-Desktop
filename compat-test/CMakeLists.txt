add_library(azookey_compat_test_support STATIC
  runner/ReportWriter.cpp
  runner/ScreenshotCapture.cpp
)
target_include_directories(azookey_compat_test_support
  PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}"
)
target_link_libraries(azookey_compat_test_support
  PUBLIC azookey_ipc azookey_project_options
  PRIVATE azookey_project_warnings ole32 windowscodecs
)

add_executable(compat_test
  runner/CompatRunner.cpp
  cases/C001_basic_input.cpp
  cases/C002_backspace.cpp
  cases/C003_escape.cpp
  cases/C004_candidate_position.cpp
)
target_link_libraries(compat_test
  PRIVATE
    azookey_compat_test_support
    azookey_project_options
    azookey_project_warnings
    ole32
    user32
    uiautomationcore
)
target_compile_definitions(compat_test
  PRIVATE
    AZOOKEY_COMPAT_DEFAULT_TARGET="${CMAKE_CURRENT_SOURCE_DIR}/targets/notepad.json"
)

if(AZOOKEY_BUILD_TESTS)
  add_executable(compat_test_unit_tests
    tests/compat_test_unit_tests.cpp
  )
  target_link_libraries(compat_test_unit_tests
    PRIVATE
      azookey_compat_test_support
      GTest::gtest_main
      azookey_project_options
      azookey_project_warnings
  )
  azookey_discover_tests(compat_test_unit_tests)
endif()

find_package(Python3 3.10 REQUIRED COMPONENTS Interpreter)

if(AZOOKEY_BUILD_DICT_TOOLS)
  add_custom_target(dictbuild
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/dictbuild.py" --help
    COMMENT "Offline dictionary builder usage")
endif()

if(AZOOKEY_BUILD_TESTS)
  set(DICT_FIXTURE "${CMAKE_CURRENT_BINARY_DIR}/fixtures")
  add_custom_command(OUTPUT "${DICT_FIXTURE}/ready"
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/make_fixtures.py" "${DICT_FIXTURE}"
    DEPENDS dictbuild.py tests/make_fixtures.py
    VERBATIM)
  add_custom_target(dictionary_fixtures DEPENDS "${DICT_FIXTURE}/ready")
  add_executable(dictionary_tests tests/dictionary_test.cpp)
  add_dependencies(dictionary_tests dictionary_fixtures)
  target_compile_definitions(dictionary_tests PRIVATE AZOOKEY_DICT_FIXTURE="${DICT_FIXTURE}")
  target_link_libraries(dictionary_tests PRIVATE azookey_host GTest::gtest_main
    azookey_project_options azookey_project_warnings)
  azookey_discover_tests(dictionary_tests)
  add_test(NAME dictbuild_python_tests
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_dictbuild.py")
endif()

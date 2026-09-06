add_executable(host_engine_tests engine_test.cpp)
target_link_libraries(host_engine_tests
  PRIVATE azookey_host GTest::gtest_main azookey_project_options azookey_project_warnings
)
azookey_discover_tests(host_engine_tests)

add_executable(host_dispatcher_tests dispatcher_test.cpp)
target_include_directories(host_dispatcher_tests PRIVATE ${PROJECT_SOURCE_DIR}/ipc/tests)
target_compile_definitions(host_dispatcher_tests PRIVATE
  AZOOKEY_IPC_TESTDATA_DIR="${PROJECT_SOURCE_DIR}/ipc/testdata"
)
target_link_libraries(host_dispatcher_tests
  PRIVATE azookey_host GTest::gtest_main azookey_project_options azookey_project_warnings
)
azookey_discover_tests(host_dispatcher_tests)

add_executable(host_scheduler_tests scheduler_test.cpp)
target_link_libraries(host_scheduler_tests
  PRIVATE azookey_host GTest::gtest_main azookey_project_options azookey_project_warnings
)
azookey_discover_tests(host_scheduler_tests)

add_executable(host_args_tests args_test.cpp)
target_link_libraries(host_args_tests
  PRIVATE azookey_host GTest::gtest_main azookey_project_options azookey_project_warnings
)
azookey_discover_tests(host_args_tests)

add_executable(host_user_data_paths_tests user_data_paths_test.cpp)
target_link_libraries(host_user_data_paths_tests
  PRIVATE azookey_host_paths GTest::gtest_main azookey_project_options azookey_project_warnings
)
azookey_discover_tests(host_user_data_paths_tests)

add_executable(host_model_catalog_tests model_catalog_test.cpp)
target_link_libraries(host_model_catalog_tests
  PRIVATE azookey_host GTest::gtest_main azookey_project_options azookey_project_warnings
)
azookey_discover_tests(host_model_catalog_tests)

if(WIN32)
  add_executable(host_http_downloader_tests http_downloader_test.cpp)
  target_link_libraries(host_http_downloader_tests
    PRIVATE azookey_host GTest::gtest_main azookey_project_options azookey_project_warnings ws2_32
  )
  azookey_discover_tests(host_http_downloader_tests)
endif()

add_executable(host_userdict_cli_tests userdict_cli_test.cpp)
target_link_libraries(host_userdict_cli_tests
  PRIVATE azookey_host GTest::gtest_main azookey_project_options azookey_project_warnings
)
azookey_discover_tests(host_userdict_cli_tests)

add_executable(host_lookup_cli_tests lookup_cli_test.cpp)
target_link_libraries(host_lookup_cli_tests
  PRIVATE azookey_host GTest::gtest_main azookey_project_options azookey_project_warnings
)
azookey_discover_tests(host_lookup_cli_tests)

add_executable(host_settings_store_tests settings_store_test.cpp)
target_link_libraries(host_settings_store_tests
  PRIVATE azookey_host GTest::gtest_main azookey_project_options azookey_project_warnings
)
azookey_discover_tests(host_settings_store_tests)

# The CLI libraries are also exercised through the real command line: only the
# process boundary shows whether non-ASCII arguments survive Windows argv.
add_test(NAME host_cli_unicode_argv
         COMMAND "${CMAKE_COMMAND}"
                 "-DHOST_EXE=$<TARGET_FILE:azookey_inference_host>"
                 "-DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/cli-unicode-argv"
                 -P "${CMAKE_CURRENT_SOURCE_DIR}/AssertUnicodeArgvCli.cmake")
set_tests_properties(host_cli_unicode_argv PROPERTIES TIMEOUT 60)
set_property(GLOBAL APPEND PROPERTY AZOOKEY_TEST_TARGETS azookey_inference_host)

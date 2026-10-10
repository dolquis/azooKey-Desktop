add_executable(host_engine_tests engine_test.cpp nll_scorer_test.cpp rewriter_test.cpp
  candidate_tags_test.cpp english_candidates_test.cpp english_dictionary_files_test.cpp
  punctuation_inserter_test.cpp)
target_compile_definitions(host_engine_tests PRIVATE
  AZOOKEY_REWRITER_DATA_DIR="${PROJECT_SOURCE_DIR}/data")
target_link_libraries(host_engine_tests
  PRIVATE azookey_host GTest::gtest_main azookey_project_options azookey_project_warnings
)
azookey_discover_tests(host_engine_tests)
if(AZOOKEY_WITH_LLAMA_CPP AND AZOOKEY_ZENZAI_TEST_MODEL)
  target_compile_definitions(host_engine_tests PRIVATE
    AZOOKEY_AI_TEST_MODEL="${AZOOKEY_ZENZAI_TEST_MODEL}")
endif()

add_executable(host_dispatcher_tests dispatcher_test.cpp ai_backend_test.cpp host_etw_test.cpp
  anomaly_detector_test.cpp)
# Both dispatcher_test.cpp and host_etw_test.cpp define ThrowingConverter.
set_source_files_properties(host_etw_test.cpp PROPERTIES SKIP_UNITY_BUILD_INCLUSION ON)
target_include_directories(host_dispatcher_tests PRIVATE
  ${PROJECT_SOURCE_DIR}/ipc/tests ${PROJECT_SOURCE_DIR}/core/tests)
target_compile_definitions(host_dispatcher_tests PRIVATE
  AZOOKEY_IPC_TESTDATA_DIR="${PROJECT_SOURCE_DIR}/ipc/testdata"
  AZOOKEY_DICT_FIXTURE="${PROJECT_BINARY_DIR}/dictbuild/fixtures"
)
add_dependencies(host_dispatcher_tests dictionary_fixtures)
target_link_libraries(host_dispatcher_tests
  PRIVATE azookey_host GTest::gtest_main azookey_project_options azookey_project_warnings
)
azookey_discover_tests(host_dispatcher_tests)

add_executable(host_health_state_tests health_state_machine_test.cpp)
target_link_libraries(host_health_state_tests
  PRIVATE azookey_host GTest::gtest_main azookey_project_options azookey_project_warnings
)
azookey_discover_tests(host_health_state_tests)

add_executable(host_scheduler_tests scheduler_test.cpp)
target_link_libraries(host_scheduler_tests
  PRIVATE azookey_host GTest::gtest_main azookey_project_options azookey_project_warnings
)
azookey_discover_tests(host_scheduler_tests)

add_executable(host_args_tests args_test.cpp startup_test.cpp)
target_link_libraries(host_args_tests
  PRIVATE azookey_host GTest::gtest_main azookey_project_options azookey_project_warnings
)
azookey_discover_tests(host_args_tests)

add_executable(host_stderr_line_queue_tests stderr_line_queue_test.cpp)
target_link_libraries(host_stderr_line_queue_tests
  PRIVATE azookey_host GTest::gtest_main azookey_project_options azookey_project_warnings
)
azookey_discover_tests(host_stderr_line_queue_tests)

add_executable(host_user_data_paths_tests user_data_paths_test.cpp)
target_link_libraries(host_user_data_paths_tests
  PRIVATE azookey_host_paths GTest::gtest_main azookey_project_options azookey_project_warnings
)
azookey_discover_tests(host_user_data_paths_tests)

add_executable(host_model_catalog_tests model_catalog_test.cpp model_scanner_test.cpp)
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

  add_executable(host_trending_word_fetcher_tests trending_word_fetcher_test.cpp)
  target_link_libraries(host_trending_word_fetcher_tests
    PRIVATE azookey_host GTest::gtest_main azookey_project_options azookey_project_warnings
  )
  azookey_discover_tests(host_trending_word_fetcher_tests)
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

add_executable(host_newwords_cli_tests newwords_cli_test.cpp)
target_link_libraries(host_newwords_cli_tests
  PRIVATE azookey_host GTest::gtest_main azookey_project_options azookey_project_warnings
)
azookey_discover_tests(host_newwords_cli_tests)

add_executable(host_settings_store_tests settings_store_test.cpp)
target_link_libraries(host_settings_store_tests
  PRIVATE azookey_host GTest::gtest_main azookey_project_options azookey_project_warnings
)
azookey_discover_tests(host_settings_store_tests)

# The CLI libraries are also exercised through the real command line: only the
# process boundary shows whether non-ASCII arguments survive Windows argv.
if(WIN32)
  # Run the real entry point with a flush deadline beyond the bounded shutdown
  # tests. The shipped Host keeps EngineConfig's normal five-second interval.
  set(host_process_test_flush_interval_sec 3600)
  add_executable(host_shutdown_test_fixture ../src/main.cpp)
  target_compile_definitions(host_shutdown_test_fixture PRIVATE
    AZOOKEY_HOST_PROCESS_TEST_FLUSH_INTERVAL_SEC=${host_process_test_flush_interval_sec})
  target_link_libraries(host_shutdown_test_fixture PRIVATE
    azookey_host azookey_binary_hardening azookey_project_options azookey_project_warnings)
  add_executable(host_model_diagnostic_test_fixture ../src/main.cpp)
  target_compile_definitions(host_model_diagnostic_test_fixture PRIVATE
    AZOOKEY_HOST_PROCESS_TEST_FLUSH_INTERVAL_SEC=${host_process_test_flush_interval_sec}
    AZOOKEY_HOST_PROCESS_TEST_MODEL_LOG_FLOOD=1)
  target_link_libraries(host_model_diagnostic_test_fixture PRIVATE
    azookey_host azookey_binary_hardening azookey_project_options azookey_project_warnings)
  add_executable(host_process_integration_tests host_process_integration_test.cpp)
  target_compile_definitions(host_process_integration_tests PRIVATE
    HOST_PROCESS_TEST_EXE="$<TARGET_FILE:azookey_inference_host>"
    HOST_SHUTDOWN_TEST_EXE="$<TARGET_FILE:host_shutdown_test_fixture>"
    HOST_MODEL_DIAGNOSTIC_TEST_EXE="$<TARGET_FILE:host_model_diagnostic_test_fixture>"
    AZOOKEY_HOST_PROCESS_TEST_FLUSH_INTERVAL_SEC=${host_process_test_flush_interval_sec})
  target_link_libraries(host_process_integration_tests
    PRIVATE azookey_host GTest::gtest_main azookey_project_options azookey_project_warnings)
  add_dependencies(host_process_integration_tests azookey_inference_host host_shutdown_test_fixture
    host_model_diagnostic_test_fixture)
  azookey_discover_tests(host_process_integration_tests
    RESOURCE_LOCK host_process_integration)

  add_test(NAME host_cli_unicode_argv
           COMMAND "${CMAKE_COMMAND}"
                   "-DHOST_EXE=$<TARGET_FILE:azookey_inference_host>"
                   "-DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/cli-unicode-argv"
                   -P "${CMAKE_CURRENT_SOURCE_DIR}/AssertUnicodeArgvCli.cmake")
  set_tests_properties(host_cli_unicode_argv PROPERTIES TIMEOUT 60)
endif()
set_property(GLOBAL APPEND PROPERTY AZOOKEY_TEST_TARGETS azookey_inference_host)

add_executable(host_store_only_zip_tests store_only_zip_test.cpp)
target_link_libraries(host_store_only_zip_tests
  PRIVATE azookey_host GTest::gtest_main azookey_project_options azookey_project_warnings
)
azookey_discover_tests(host_store_only_zip_tests)

add_executable(host_backup_archive_tests backup_archive_test.cpp)
target_link_libraries(host_backup_archive_tests
  PRIVATE azookey_host GTest::gtest_main azookey_project_options azookey_project_warnings
)
azookey_discover_tests(host_backup_archive_tests)

add_executable(host_learning_data_manager_tests learning_data_manager_test.cpp)
target_link_libraries(host_learning_data_manager_tests
  PRIVATE azookey_host GTest::gtest_main azookey_project_options azookey_project_warnings
)
azookey_discover_tests(host_learning_data_manager_tests)

add_executable(host_user_learning_scorer_tests user_learning_scorer_test.cpp)
target_link_libraries(host_user_learning_scorer_tests
  PRIVATE azookey_host GTest::gtest_main azookey_project_options azookey_project_warnings
)
azookey_discover_tests(host_user_learning_scorer_tests)

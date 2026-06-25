add_executable(learning_tests learning_test.cpp)
target_link_libraries(learning_tests
  PRIVATE azookey_learning GTest::gtest_main azookey_project_options azookey_project_warnings
)
azookey_discover_tests(learning_tests)

add_executable(user_dictionary_tests user_dictionary_test.cpp)
target_link_libraries(user_dictionary_tests
  PRIVATE azookey_learning GTest::gtest_main azookey_project_options azookey_project_warnings
)
azookey_discover_tests(user_dictionary_tests)

add_executable(reranker_tests reranker_test.cpp)
target_link_libraries(reranker_tests
  PRIVATE azookey_learning GTest::gtest_main azookey_project_options azookey_project_warnings
)
azookey_discover_tests(reranker_tests)

add_executable(atomic_file_tests atomic_file_test.cpp)
target_link_libraries(atomic_file_tests
  PRIVATE azookey_learning GTest::gtest_main azookey_project_options azookey_project_warnings
)
# AtomicFile.h is a private header under learning/src, exercised directly here.
target_include_directories(atomic_file_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
azookey_discover_tests(atomic_file_tests)

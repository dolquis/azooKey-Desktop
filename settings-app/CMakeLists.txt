find_program(AZOOKEY_MSBUILD_EXECUTABLE NAMES MSBuild.exe msbuild REQUIRED)

if(AZOOKEY_BUILD_TESTS AND TARGET GTest::gtest_main)
  add_executable(azookey_settings_launch_arguments_tests
    LaunchArguments.cpp
    tests/launch_arguments_test.cpp
  )
  target_include_directories(azookey_settings_launch_arguments_tests
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  target_compile_definitions(azookey_settings_launch_arguments_tests
    PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
  target_link_libraries(azookey_settings_launch_arguments_tests
    PRIVATE
      GTest::gtest_main
      azookey_project_options
      azookey_project_warnings
  )
  azookey_discover_tests(azookey_settings_launch_arguments_tests LABEL settings-app)

  add_executable(azookey_settings_persistence_tests
    SettingsDocument.cpp
    SettingsIpcClient.cpp
    tests/settings_document_test.cpp
    tests/settings_ipc_client_test.cpp
  )
  target_include_directories(azookey_settings_persistence_tests
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  target_compile_definitions(azookey_settings_persistence_tests
    PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
  target_link_libraries(azookey_settings_persistence_tests
    PRIVATE
      azookey_learning
      azookey_ipc
      GTest::gtest_main
      azookey_project_options
      azookey_project_warnings
  )
  azookey_discover_tests(azookey_settings_persistence_tests LABEL settings-app)
endif()

set(AZOOKEY_SETTINGS_CONFIGURATION "$<IF:$<CONFIG:Debug>,Debug,Release>")
set(AZOOKEY_SETTINGS_OUTPUT_DIR
    "${CMAKE_BINARY_DIR}/settings-app/${AZOOKEY_SETTINGS_CONFIGURATION}/")

add_custom_target(azookey_settings
  COMMAND "${AZOOKEY_MSBUILD_EXECUTABLE}"
          "${CMAKE_CURRENT_SOURCE_DIR}/azookey_settings.vcxproj"
          /restore
          /target:Build
          /property:Configuration=${AZOOKEY_SETTINGS_CONFIGURATION}
          /property:Platform=x64
          /property:OutDir=${AZOOKEY_SETTINGS_OUTPUT_DIR}
          /property:IntDir=${CMAKE_CURRENT_BINARY_DIR}/obj/${AZOOKEY_SETTINGS_CONFIGURATION}/
  BYPRODUCTS "${AZOOKEY_SETTINGS_OUTPUT_DIR}/azookey_settings.exe"
  USES_TERMINAL
  VERBATIM
)

find_program(AZOOKEY_MSBUILD_EXECUTABLE NAMES MSBuild.exe msbuild REQUIRED)

# The settings app shows the neologd_lexicon attribution from the same pinned manifest the
# Host embeds (auto-word-registration-spec section 15.14).
set(AZOOKEY_SETTINGS_NEOLOGD_PIN "${PROJECT_SOURCE_DIR}/dictbuild/packs/neologd_lexicon.manifest.json")
set(AZOOKEY_SETTINGS_NEOLOGD_PIN_CPP "${CMAKE_CURRENT_BINARY_DIR}/generated/NeologdPackPin.cpp")
add_custom_command(OUTPUT "${AZOOKEY_SETTINGS_NEOLOGD_PIN_CPP}"
  COMMAND ${CMAKE_COMMAND} -DINPUT=${AZOOKEY_SETTINGS_NEOLOGD_PIN}
    -DOUTPUT=${AZOOKEY_SETTINGS_NEOLOGD_PIN_CPP}
    -P "${CMAKE_CURRENT_SOURCE_DIR}/EmbedNeologdPin.cmake"
  DEPENDS "${AZOOKEY_SETTINGS_NEOLOGD_PIN}" "${CMAKE_CURRENT_SOURCE_DIR}/EmbedNeologdPin.cmake"
  VERBATIM)

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
    AppProfiles.cpp
    LearningPaneModel.cpp
    ModelPaneModel.cpp
    NeologdAttribution.cpp
    ProofreadModel.cpp
    StatusPaneModel.cpp
    SettingsDocument.cpp
    SettingsFields.cpp
    SettingsIpcClient.cpp
    "${AZOOKEY_SETTINGS_NEOLOGD_PIN_CPP}"
    tests/app_profiles_test.cpp
    tests/neologd_attribution_test.cpp
    tests/pane_models_test.cpp
    tests/proofread_model_test.cpp
    tests/status_pane_model_test.cpp
    tests/settings_document_test.cpp
    tests/settings_fields_test.cpp
    tests/settings_ipc_client_test.cpp
  )
  target_include_directories(azookey_settings_persistence_tests
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  target_compile_definitions(azookey_settings_persistence_tests
    PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN
      AZOOKEY_NEOLOGD_PIN_PATH="${AZOOKEY_SETTINGS_NEOLOGD_PIN}"
      AZOOKEY_SETTINGS_SCHEMA_PATH="${PROJECT_SOURCE_DIR}/settings/mvp-settings.schema.json"
      AZOOKEY_SETTINGS_RESW_PATH="${CMAKE_CURRENT_SOURCE_DIR}/Strings/ja-JP/Resources.resw"
      AZOOKEY_SETTINGS_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
  target_link_libraries(azookey_settings_persistence_tests
    PRIVATE
      azookey_learning
      azookey_core
      azookey_ipc
      GTest::gtest_main
      azookey_project_options
      azookey_project_warnings
  )
  azookey_discover_tests(azookey_settings_persistence_tests LABEL settings-app)
endif()

set(AZOOKEY_PRODUCT_VERSION "0.0.0" CACHE STRING "Product version (MAJOR.MINOR.PATCH)")
set(AZOOKEY_SETTINGS_CONFIGURATION "$<IF:$<CONFIG:Debug>,Debug,Release>")
set(AZOOKEY_SETTINGS_OUTPUT_DIR
    "${CMAKE_BINARY_DIR}/settings-app/${AZOOKEY_SETTINGS_CONFIGURATION}/")
if(EXISTS "${AZOOKEY_WIL_SOURCE_DIR}/include/wil/resource.h")
  set(AZOOKEY_SETTINGS_WIL_INCLUDE_DIR "${AZOOKEY_WIL_SOURCE_DIR}/include")
elseif(DEFINED wil_SOURCE_DIR AND EXISTS "${wil_SOURCE_DIR}/include/wil/resource.h")
  set(AZOOKEY_SETTINGS_WIL_INCLUDE_DIR "${wil_SOURCE_DIR}/include")
else()
  message(FATAL_ERROR "Unable to resolve the WIL include directory for azookey_settings")
endif()

add_custom_target(azookey_settings
  COMMAND "${AZOOKEY_MSBUILD_EXECUTABLE}"
          "${CMAKE_CURRENT_SOURCE_DIR}/azookey_settings.vcxproj"
          /restore
          /target:Build
          /property:Configuration=${AZOOKEY_SETTINGS_CONFIGURATION}
          /property:Platform=x64
          /property:AzooKeyProductVersion=${AZOOKEY_PRODUCT_VERSION}
          /property:OutDir=${AZOOKEY_SETTINGS_OUTPUT_DIR}
          /property:IntDir=${CMAKE_CURRENT_BINARY_DIR}/obj/${AZOOKEY_SETTINGS_CONFIGURATION}/
          /property:AzooKeyWilIncludeDir=${AZOOKEY_SETTINGS_WIL_INCLUDE_DIR}
          /property:AzooKeyNeologdPinSource=${AZOOKEY_SETTINGS_NEOLOGD_PIN_CPP}
  DEPENDS "${AZOOKEY_SETTINGS_NEOLOGD_PIN_CPP}"
  BYPRODUCTS "${AZOOKEY_SETTINGS_OUTPUT_DIR}/azookey_settings.exe"
  USES_TERMINAL
  VERBATIM
)

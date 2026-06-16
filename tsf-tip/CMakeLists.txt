add_library(azookey_tsf_tip SHARED
  src/CandidateWindow.cpp
  src/DllMain.cpp
  src/DisplayAttribute.cpp
  src/exports.def
  src/TextService.cpp
  src/TextServiceFactory.cpp
)

target_include_directories(azookey_tsf_tip PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(azookey_tsf_tip
  PRIVATE
    azookey_core
    azookey_ipc
    azookey_project_options
    azookey_project_warnings
    ole32
    uuid
    shlwapi
    shcore
)
target_compile_definitions(azookey_tsf_tip PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)

if(AZOOKEY_BUILD_TESTS)
  function(add_tsf_tip_unit_test target_name test_source)
    add_executable(${target_name}
      src/CandidateWindow.cpp
      src/DisplayAttribute.cpp
      src/TextService.cpp
      ${test_source}
    )
    target_include_directories(${target_name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
    target_compile_definitions(${target_name} PRIVATE
      UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN AZOOKEY_TSF_TESTING)
    target_link_libraries(${target_name}
      PRIVATE
        azookey_core
        azookey_ipc
        GTest::gtest_main
        azookey_project_options
        azookey_project_warnings
        ole32
        uuid
        shlwapi
        shcore
        user32
        gdi32
    )
    gtest_discover_tests(${target_name})
  endfunction()

  add_executable(tsf_tip_com_smoke_tests
    tests/com_smoke_test.cpp
  )
  target_include_directories(tsf_tip_com_smoke_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
  target_compile_definitions(tsf_tip_com_smoke_tests PRIVATE
    AZOOKEY_TSF_TIP_DLL_PATH="$<TARGET_FILE:azookey_tsf_tip>"
  )
  target_compile_definitions(tsf_tip_com_smoke_tests PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
  # ole32/uuid: CoCreateInstance + TSF GUIDs for the registration round-trip test.
  # advapi32: process-elevation check (the round-trip test is skipped unelevated).
  target_link_libraries(tsf_tip_com_smoke_tests
    PRIVATE GTest::gtest_main azookey_project_options azookey_project_warnings
            ole32 uuid advapi32
  )
  add_dependencies(tsf_tip_com_smoke_tests azookey_tsf_tip)
  gtest_discover_tests(tsf_tip_com_smoke_tests)

  add_tsf_tip_unit_test(tsf_tip_onkeydown_preedit_tests tests/onkeydown_preedit_test.cpp)
  add_tsf_tip_unit_test(tsf_tip_display_attribute_tests tests/display_attribute_test.cpp)
  add_tsf_tip_unit_test(tsf_tip_activate_uiless_tests tests/activate_uiless_test.cpp)
  add_tsf_tip_unit_test(tsf_tip_candidate_window_dpi_tests tests/candidate_window_dpi_test.cpp)
endif()

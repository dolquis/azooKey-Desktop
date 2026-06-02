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
)
target_compile_definitions(azookey_tsf_tip PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)

if(AZOOKEY_BUILD_TESTS)
  add_executable(tsf_tip_com_smoke_tests
    tests/com_smoke_test.cpp
  )
  target_include_directories(tsf_tip_com_smoke_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
  target_compile_definitions(tsf_tip_com_smoke_tests PRIVATE
    AZOOKEY_TSF_TIP_DLL_PATH="$<TARGET_FILE:azookey_tsf_tip>"
  )
  target_compile_definitions(tsf_tip_com_smoke_tests PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
  target_link_libraries(tsf_tip_com_smoke_tests
    PRIVATE GTest::gtest_main azookey_project_options azookey_project_warnings
  )
  add_dependencies(tsf_tip_com_smoke_tests azookey_tsf_tip)
  gtest_discover_tests(tsf_tip_com_smoke_tests)

  add_executable(tsf_tip_onkeydown_preedit_tests
    src/CandidateWindow.cpp
    src/DisplayAttribute.cpp
    src/TextService.cpp
    tests/onkeydown_preedit_test.cpp
  )
  target_include_directories(tsf_tip_onkeydown_preedit_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
  target_compile_definitions(tsf_tip_onkeydown_preedit_tests PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
  target_link_libraries(tsf_tip_onkeydown_preedit_tests
    PRIVATE
      azookey_core
      azookey_ipc
      GTest::gtest_main
      azookey_project_options
      azookey_project_warnings
      ole32
      uuid
      shlwapi
      user32
      gdi32
  )
  gtest_discover_tests(tsf_tip_onkeydown_preedit_tests)

  add_executable(tsf_tip_display_attribute_tests
    src/CandidateWindow.cpp
    src/DisplayAttribute.cpp
    src/TextService.cpp
    tests/display_attribute_test.cpp
  )
  target_include_directories(tsf_tip_display_attribute_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
  target_compile_definitions(tsf_tip_display_attribute_tests PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
  target_link_libraries(tsf_tip_display_attribute_tests
    PRIVATE
      azookey_core
      azookey_ipc
      GTest::gtest_main
      azookey_project_options
      azookey_project_warnings
      ole32
      uuid
      shlwapi
      user32
      gdi32
  )
  gtest_discover_tests(tsf_tip_display_attribute_tests)
endif()

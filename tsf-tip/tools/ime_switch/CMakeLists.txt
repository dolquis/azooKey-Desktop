# Moves the interactive session off azooKey before unregistration (DEV-1654).
# Built next to the TIP DLL so unregister-dev.ps1 and the MSI find it there.
if(WIN32)
  add_executable(azookey_ime_switch WIN32 ImeSwitchMain.cpp)
  target_include_directories(azookey_ime_switch PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${PROJECT_SOURCE_DIR}/tsf-tip/include)
  target_compile_definitions(azookey_ime_switch PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
  target_link_libraries(azookey_ime_switch PRIVATE
    azookey_core azookey_binary_hardening azookey_project_options azookey_project_warnings
    ole32 uuid)
  set_target_properties(azookey_ime_switch PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "$<TARGET_FILE_DIR:azookey_tsf_tip>")
endif()

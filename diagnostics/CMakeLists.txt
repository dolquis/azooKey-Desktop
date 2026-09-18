set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
  "${PROJECT_SOURCE_DIR}/settings/mvp-settings.schema.json"
)
file(READ "${PROJECT_SOURCE_DIR}/settings/mvp-settings.schema.json"
     azookey_settings_schema_remaining)
# MSVC は 1 つの文字列リテラルを約 16 KB までしか受け付けない（C2026）。
# 改行位置で分割し、隣接する raw string literal として連結させる。
# 改行は ASCII なので、UTF-8 の多バイト文字を途中で割らない。
set(azookey_settings_schema_chunk_bytes 8192)
set(AZOOKEY_SETTINGS_SCHEMA_LITERALS "")
string(LENGTH "${azookey_settings_schema_remaining}" azookey_settings_schema_length)
while(azookey_settings_schema_length GREATER 0)
  if(azookey_settings_schema_length GREATER azookey_settings_schema_chunk_bytes)
    string(SUBSTRING "${azookey_settings_schema_remaining}" 0
           ${azookey_settings_schema_chunk_bytes} azookey_settings_schema_window)
    string(FIND "${azookey_settings_schema_window}" "\n" azookey_settings_schema_cut REVERSE)
    if(azookey_settings_schema_cut EQUAL -1)
      message(FATAL_ERROR
        "mvp-settings.schema.json has a line longer than ${azookey_settings_schema_chunk_bytes} bytes")
    endif()
    math(EXPR azookey_settings_schema_cut "${azookey_settings_schema_cut} + 1")
  else()
    set(azookey_settings_schema_cut ${azookey_settings_schema_length})
  endif()
  string(SUBSTRING "${azookey_settings_schema_remaining}" 0
         ${azookey_settings_schema_cut} azookey_settings_schema_piece)
  string(APPEND AZOOKEY_SETTINGS_SCHEMA_LITERALS
         "\n    R\"AZOOKEY_SCHEMA(${azookey_settings_schema_piece})AZOOKEY_SCHEMA\"")
  string(SUBSTRING "${azookey_settings_schema_remaining}" ${azookey_settings_schema_cut} -1
         azookey_settings_schema_remaining)
  string(LENGTH "${azookey_settings_schema_remaining}" azookey_settings_schema_length)
endwhile()
configure_file(
  "${CMAKE_CURRENT_SOURCE_DIR}/SettingsSchema.h.in"
  "${CMAKE_CURRENT_BINARY_DIR}/generated/SettingsSchema.h"
  @ONLY
)

add_library(azookey_diagnostics STATIC Diagnostics.cpp)
set_property(SOURCE Diagnostics.cpp APPEND PROPERTY OBJECT_DEPENDS
  "${CMAKE_CURRENT_BINARY_DIR}/generated/SettingsSchema.h"
)
target_include_directories(azookey_diagnostics
  PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}"
  PRIVATE
    "${CMAKE_CURRENT_BINARY_DIR}/generated"
    "${PROJECT_SOURCE_DIR}/tsf-tip/include"
)
target_link_libraries(azookey_diagnostics
  PUBLIC azookey_ipc azookey_project_options
  PRIVATE
    azookey_core
    azookey_host_paths
    azookey_learning
    azookey_project_warnings
)

if(WIN32)
  target_link_libraries(azookey_diagnostics PRIVATE advapi32 ole32)
  add_subdirectory(etw)
endif()

add_executable(azookey_diag azookey_diag.cpp)
target_link_libraries(azookey_diag
  PRIVATE azookey_diagnostics azookey_project_options azookey_project_warnings
)

if(AZOOKEY_BUILD_TESTS)
  add_subdirectory(tests)
endif()

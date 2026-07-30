set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
  "${PROJECT_SOURCE_DIR}/settings/mvp-settings.schema.json"
)
file(READ "${PROJECT_SOURCE_DIR}/settings/mvp-settings.schema.json"
     AZOOKEY_SETTINGS_SCHEMA_JSON)
configure_file(
  "${CMAKE_CURRENT_SOURCE_DIR}/SettingsSchema.h.in"
  "${CMAKE_CURRENT_BINARY_DIR}/generated/SettingsSchema.h"
  @ONLY
)

add_library(azookey_diagnostics STATIC Diagnostics.cpp)
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
endif()

add_executable(azookey_diag azookey_diag.cpp)
target_link_libraries(azookey_diag
  PRIVATE azookey_diagnostics azookey_project_options azookey_project_warnings
)

if(AZOOKEY_BUILD_TESTS)
  add_subdirectory(tests)
endif()

find_program(AZOOKEY_MSBUILD_EXECUTABLE NAMES MSBuild.exe msbuild)

if(NOT AZOOKEY_MSBUILD_EXECUTABLE)
  message(WARNING
    "MSBuild was not found; the azookey_settings target is unavailable. "
    "Run CMake from a Visual Studio developer environment to build it.")
  return()
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

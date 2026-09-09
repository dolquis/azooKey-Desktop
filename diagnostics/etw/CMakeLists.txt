if(WIN32)
  find_program(AZOOKEY_MC_EXECUTABLE NAMES mc.exe mc REQUIRED)
  set(etw_generated "${CMAKE_CURRENT_BINARY_DIR}/generated")
  add_custom_command(
    OUTPUT "${etw_generated}/AzooKey.rc" "${etw_generated}/AzooKey.h"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${etw_generated}"
    COMMAND "${AZOOKEY_MC_EXECUTABLE}" -h "${etw_generated}" -r "${etw_generated}"
            "${CMAKE_CURRENT_SOURCE_DIR}/AzooKey.man"
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/AzooKey.man"
    VERBATIM
  )
  add_library(azookey_etw_manifest SHARED "${etw_generated}/AzooKey.rc")
  set_target_properties(azookey_etw_manifest PROPERTIES LINKER_LANGUAGE CXX)
  target_include_directories(azookey_etw_manifest PRIVATE "${etw_generated}")
  target_link_options(azookey_etw_manifest PRIVATE /NOENTRY /INCREMENTAL:NO)
endif()

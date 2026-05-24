add_library(azookey_ipc STATIC
  src/Json.cpp
  src/Messages.cpp
  src/NamedPipeTransport.cpp
  src/Payloads.cpp
)

target_include_directories(azookey_ipc PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(azookey_ipc
  PUBLIC azookey_project_options
  PRIVATE azookey_project_warnings
)

if(WIN32)
  # advapi32 is required by ConvertStringSecurityDescriptorToSecurityDescriptorW
  # used by the M1 Named Pipe DACL setup.
  target_link_libraries(azookey_ipc PUBLIC advapi32)
endif()

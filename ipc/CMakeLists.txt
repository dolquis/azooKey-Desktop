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
  if(TARGET WIL::WIL)
    target_link_libraries(azookey_ipc PRIVATE WIL::WIL)
  endif()

  # advapi32 is required by ConvertStringSecurityDescriptorToSecurityDescriptorW
  # used by the M1 Named Pipe DACL setup.
  target_link_libraries(azookey_ipc PUBLIC advapi32)
endif()

if(AZOOKEY_BUILD_FUZZERS)
  add_executable(ipc_json_fuzzer
    fuzz/json_fuzzer.cpp
    src/Json.cpp
  )
  target_include_directories(ipc_json_fuzzer PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
  target_link_libraries(ipc_json_fuzzer
    PRIVATE azookey_project_options azookey_project_warnings
  )
  target_compile_options(ipc_json_fuzzer PRIVATE -fsanitize=fuzzer)
  target_link_options(ipc_json_fuzzer PRIVATE -fsanitize=fuzzer)
endif()

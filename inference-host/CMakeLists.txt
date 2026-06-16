add_library(azookey_host STATIC
  src/Dispatcher.cpp
  src/InferenceEngine.cpp
  src/RequestScheduler.cpp
  src/SettingsStore.cpp
  src/UserDataPaths.cpp
  src/ZenzaiModelConverter.cpp
)

target_include_directories(azookey_host PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(azookey_host
  PUBLIC azookey_core azookey_learning azookey_ipc azookey_project_options
  PRIVATE azookey_project_warnings
)
target_compile_definitions(azookey_host PUBLIC AZOOKEY_BACKEND_DEFAULT="${AZOOKEY_BACKEND}")

if(WIN32)
  target_link_libraries(azookey_host PUBLIC ole32 shell32)
endif()

add_executable(azookey_inference_host src/main.cpp)
target_link_libraries(azookey_inference_host
  PRIVATE azookey_host azookey_project_options azookey_project_warnings
)

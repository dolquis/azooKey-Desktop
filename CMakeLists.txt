cmake_minimum_required(VERSION 3.21)
project(azooKeyDesktopWindowsMvp LANGUAGES CXX)

option(AZOOKEY_BUILD_TESTS "Build unit tests" ON)
option(AZOOKEY_BUILD_BENCH "Build bench tools" ON)
option(AZOOKEY_FETCH_GOOGLETEST
       "Download GoogleTest via FetchContent when it is not found locally" OFF)
option(AZOOKEY_FETCH_LLAMA_CPP
       "Download llama.cpp via FetchContent when no local source tree is provided" OFF)
set(AZOOKEY_LLAMA_CPP_SOURCE_DIR "" CACHE PATH
    "Path to an existing llama.cpp source tree to add_subdirectory")
set(AZOOKEY_LLAMA_CPP_GIT_TAG
    "05f6ac6283a7859a03cd59405504262c85c4bf06" CACHE STRING
    "Pinned llama.cpp revision used when AZOOKEY_FETCH_LLAMA_CPP is ON")
set(AZOOKEY_BACKEND "cpu" CACHE STRING "Default inference backend: cpu or cuda")
set_property(CACHE AZOOKEY_BACKEND PROPERTY STRINGS cpu cuda)

if(NOT AZOOKEY_BACKEND STREQUAL "cpu" AND NOT AZOOKEY_BACKEND STREQUAL "cuda")
  message(FATAL_ERROR "AZOOKEY_BACKEND must be either 'cpu' or 'cuda'")
endif()

set(AZOOKEY_WITH_LLAMA_CPP OFF)
if(AZOOKEY_LLAMA_CPP_SOURCE_DIR)
  set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
  set(LLAMA_BUILD_COMMON OFF CACHE BOOL "" FORCE)
  set(LLAMA_BUILD_TESTS OFF CACHE BOOL "" FORCE)
  set(LLAMA_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
  set(LLAMA_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
  set(LLAMA_BUILD_SERVER OFF CACHE BOOL "" FORCE)
  set(LLAMA_CURL OFF CACHE BOOL "" FORCE)
  set(GGML_NATIVE OFF CACHE BOOL "" FORCE)
  set(GGML_CUDA OFF CACHE BOOL "" FORCE)
  add_subdirectory("${AZOOKEY_LLAMA_CPP_SOURCE_DIR}"
                   "${CMAKE_BINARY_DIR}/_deps/llama.cpp-build"
                   EXCLUDE_FROM_ALL)
elseif(AZOOKEY_FETCH_LLAMA_CPP)
  set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
  set(LLAMA_BUILD_COMMON OFF CACHE BOOL "" FORCE)
  set(LLAMA_BUILD_TESTS OFF CACHE BOOL "" FORCE)
  set(LLAMA_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
  set(LLAMA_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
  set(LLAMA_BUILD_SERVER OFF CACHE BOOL "" FORCE)
  set(LLAMA_CURL OFF CACHE BOOL "" FORCE)
  set(GGML_NATIVE OFF CACHE BOOL "" FORCE)
  set(GGML_CUDA OFF CACHE BOOL "" FORCE)
  include(FetchContent)
  FetchContent_Declare(
    llama_cpp
    GIT_REPOSITORY https://github.com/ggml-org/llama.cpp.git
    GIT_TAG        ${AZOOKEY_LLAMA_CPP_GIT_TAG}
    GIT_PROGRESS   TRUE
  )
  FetchContent_MakeAvailable(llama_cpp)
endif()

if(TARGET llama)
  set(AZOOKEY_WITH_LLAMA_CPP ON)
else()
  message(STATUS
    "llama.cpp target not available; Zenzai loader uses no-egress mock runtime. "
    "Set AZOOKEY_LLAMA_CPP_SOURCE_DIR or AZOOKEY_FETCH_LLAMA_CPP=ON for real loads.")
endif()

add_library(azookey_project_options INTERFACE)
target_compile_features(azookey_project_options INTERFACE cxx_std_17)
target_compile_options(azookey_project_options INTERFACE
  $<$<CXX_COMPILER_ID:MSVC>:/utf-8 /permissive- /EHsc /Zc:__cplusplus /sdl>
)

add_library(azookey_project_warnings INTERFACE)
target_compile_options(azookey_project_warnings INTERFACE
  $<$<CXX_COMPILER_ID:MSVC>:/W4>
  $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Wall -Wextra -Wpedantic>
)

if(AZOOKEY_BUILD_TESTS)
  enable_testing()

  # Prefer a system-installed GoogleTest. The network download is opt-in
  # (AZOOKEY_FETCH_GOOGLETEST) so that the default `cmake -S . -B build`
  # still succeeds in no-egress environments; tests are simply skipped with
  # a warning when GoogleTest is unavailable.
  find_package(GTest QUIET)
  if(NOT GTest_FOUND AND AZOOKEY_FETCH_GOOGLETEST)
    set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
    set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
    include(FetchContent)
    FetchContent_Declare(
      googletest
      GIT_REPOSITORY https://github.com/google/googletest.git
      GIT_TAG        v1.17.0
    )
    FetchContent_MakeAvailable(googletest)
  endif()

  if(TARGET GTest::gtest_main)
    include(GoogleTest)
  else()
    message(WARNING
      "GoogleTest not found; unit tests are disabled. Install GoogleTest or "
      "re-run CMake with -DAZOOKEY_FETCH_GOOGLETEST=ON to download it.")
    set(AZOOKEY_BUILD_TESTS OFF)
  endif()
endif()

add_subdirectory(core)
add_subdirectory(ipc)
add_subdirectory(learning)
add_subdirectory(inference-host)

if(WIN32)
  add_subdirectory(tsf-tip)
endif()

if(AZOOKEY_BUILD_BENCH)
  add_subdirectory(bench)
endif()

if(AZOOKEY_BUILD_TESTS)
  add_subdirectory(core/tests)
  add_subdirectory(ipc/tests)
  add_subdirectory(learning/tests)
  add_subdirectory(inference-host/tests)
endif()

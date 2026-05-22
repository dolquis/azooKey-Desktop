cmake_minimum_required(VERSION 3.21)
project(azooKeyDesktopWindowsMvp LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(MSVC)
  add_compile_options(/utf-8)
endif()

option(AZOOKEY_BUILD_TESTS "Build unit tests" ON)
option(AZOOKEY_BUILD_BENCH "Build bench tools" ON)
option(AZOOKEY_FETCH_GOOGLETEST
       "Download GoogleTest via FetchContent when it is not found locally" OFF)
set(AZOOKEY_BACKEND "cpu" CACHE STRING "Default inference backend: cpu or cuda")
set_property(CACHE AZOOKEY_BACKEND PROPERTY STRINGS cpu cuda)

if(NOT AZOOKEY_BACKEND STREQUAL "cpu" AND NOT AZOOKEY_BACKEND STREQUAL "cuda")
  message(FATAL_ERROR "AZOOKEY_BACKEND must be either 'cpu' or 'cuda'")
endif()

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

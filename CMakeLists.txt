cmake_minimum_required(VERSION 3.24)
project(azooKeyDesktopWindowsMvp LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(MSVC)
  add_compile_options(/utf-8)
endif()

option(AZOOKEY_BUILD_TESTS "Build unit tests" ON)
option(AZOOKEY_BUILD_BENCH "Build bench tools" ON)

if(AZOOKEY_BUILD_TESTS)
  enable_testing()

  # GoogleTest: FIND_PACKAGE_ARGS makes a system-installed GTest the preferred
  # source, and FetchContent downloads the pinned tag only when none is found.
  set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
  set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
  include(FetchContent)
  FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG        v1.17.0
    FIND_PACKAGE_ARGS NAMES GTest
  )
  FetchContent_MakeAvailable(googletest)
  include(GoogleTest)
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

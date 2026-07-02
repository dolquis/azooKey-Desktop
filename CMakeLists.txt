cmake_minimum_required(VERSION 3.21)

# --- Compiler cache (sccache/ccache) -----------------------------------------
# Detected before project() so that, on MSVC, we can pick a debug-information
# format compatible with the cache. sccache/ccache cannot cache objects that
# share a PDB (the MSVC /Zi default); embedding debug info in the object
# (/Z7 via CMAKE_MSVC_DEBUG_INFORMATION_FORMAT=Embedded) restores cache hits.
# Opt out with -DAZOOKEY_USE_COMPILER_CACHE=OFF. CI does not ship sccache, so
# this stays a no-op there (the Release PDB artifact is unaffected).
option(AZOOKEY_USE_COMPILER_CACHE
       "Use sccache/ccache as the compiler launcher when found on PATH" ON)
if(AZOOKEY_USE_COMPILER_CACHE AND NOT DEFINED CMAKE_CXX_COMPILER_LAUNCHER)
  find_program(AZOOKEY_COMPILER_CACHE NAMES sccache ccache)
  if(AZOOKEY_COMPILER_CACHE AND POLICY CMP0141)
    cmake_policy(SET CMP0141 NEW)
    set(CMAKE_MSVC_DEBUG_INFORMATION_FORMAT
        "$<$<CONFIG:Debug,RelWithDebInfo>:Embedded>")
  endif()
endif()

project(azooKeyDesktopWindowsMvp LANGUAGES CXX)

# Emit compile_commands.json in the build dir for clang-tidy and editor tooling
# (gitignored). Kept separate from the committed clang-cl DB used by clangd.
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Guard on the option too: find_program() caches AZOOKEY_COMPILER_CACHE, so a
# build dir first configured with the cache ON would otherwise keep setting the
# launcher even after re-configuring with -DAZOOKEY_USE_COMPILER_CACHE=OFF.
if(AZOOKEY_USE_COMPILER_CACHE AND AZOOKEY_COMPILER_CACHE)
  set(CMAKE_C_COMPILER_LAUNCHER "${AZOOKEY_COMPILER_CACHE}")
  set(CMAKE_CXX_COMPILER_LAUNCHER "${AZOOKEY_COMPILER_CACHE}")
  message(STATUS "Compiler cache enabled: ${AZOOKEY_COMPILER_CACHE}")
endif()

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
target_compile_features(azookey_project_options INTERFACE cxx_std_20)
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
    set(AZOOKEY_GTEST_DISCOVERY_TIMEOUT 60 CACHE STRING
        "Seconds to wait while CMake enumerates GoogleTest cases")
    set(AZOOKEY_GTEST_TEST_TIMEOUT 60 CACHE STRING
        "Seconds to allow each discovered GoogleTest case to run")
    function(azookey_discover_tests target_name)
      set(one_value_args LABEL TIMEOUT)
      cmake_parse_arguments(AZOOKEY_TEST "" "${one_value_args}" "" ${ARGN})

      set(azookey_test_timeout "${AZOOKEY_GTEST_TEST_TIMEOUT}")
      if(AZOOKEY_TEST_TIMEOUT)
        set(azookey_test_timeout "${AZOOKEY_TEST_TIMEOUT}")
      endif()

      set(azookey_test_properties TIMEOUT ${azookey_test_timeout})
      if(AZOOKEY_TEST_LABEL)
        list(APPEND azookey_test_properties LABELS ${AZOOKEY_TEST_LABEL})
      endif()

      gtest_discover_tests(${target_name}
        DISCOVERY_TIMEOUT ${AZOOKEY_GTEST_DISCOVERY_TIMEOUT}
        PROPERTIES ${azookey_test_properties}
      )
      set_property(GLOBAL APPEND PROPERTY AZOOKEY_TEST_TARGETS ${target_name})
    endfunction()
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

if(AZOOKEY_BUILD_TESTS)
  get_property(AZOOKEY_CHECK_DEPENDS GLOBAL PROPERTY AZOOKEY_TEST_TARGETS)
  if(AZOOKEY_CHECK_DEPENDS)
    add_custom_target(azookey_check
      COMMAND ${CMAKE_CTEST_COMMAND}
              --test-dir "${CMAKE_BINARY_DIR}"
              --output-on-failure
              --no-tests=error
      USES_TERMINAL
      COMMENT "Build test executables and run CTest")
    add_dependencies(azookey_check ${AZOOKEY_CHECK_DEPENDS})
  endif()
endif()

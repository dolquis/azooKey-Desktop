cmake_minimum_required(VERSION 3.21)

# --- Compiler cache (sccache/ccache) -----------------------------------------
# Detected before project() so that, on MSVC, we can pick a debug-information
# format compatible with the cache. sccache/ccache cannot cache objects that
# share a PDB (the MSVC /Zi default); embedding debug info in the object
# (/Z7 via CMAKE_MSVC_DEBUG_INFORMATION_FORMAT=Embedded) restores cache hits.
# Opt out with -DAZOOKEY_USE_COMPILER_CACHE=OFF. CI provisions sccache via
# mozilla-actions/sccache-action; when sccache/ccache is absent (e.g. a plain
# local build) this stays a no-op. The Embedded switch is gated to Debug/
# RelWithDebInfo, so the Release PDB artifact is unaffected.
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
# (gitignored). The committed .clangd points clangd at the separately generated
# build/clangd database.
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Guard on the option too: find_program() caches AZOOKEY_COMPILER_CACHE, so a
# build dir first configured with the cache ON would otherwise keep setting the
# launcher even after re-configuring with -DAZOOKEY_USE_COMPILER_CACHE=OFF.
if(AZOOKEY_USE_COMPILER_CACHE AND AZOOKEY_COMPILER_CACHE)
  set(CMAKE_C_COMPILER_LAUNCHER "${AZOOKEY_COMPILER_CACHE}")
  set(CMAKE_CXX_COMPILER_LAUNCHER "${AZOOKEY_COMPILER_CACHE}")
  message(STATUS "Compiler cache enabled: ${AZOOKEY_COMPILER_CACHE}")
endif()

# CMake detects MSVC's /showIncludes prefix in English, while cl.exe follows
# the host UI language during normal Ninja builds. Keep the compiler output in
# English so Ninja records header dependencies instead of silently storing an
# empty dependency set on non-English Windows hosts. Preserve any existing
# launcher (for example sccache) after the environment wrapper.
if(MSVC)
  foreach(_azookey_lang IN ITEMS C CXX)
    set(_azookey_existing_launcher
        "${CMAKE_${_azookey_lang}_COMPILER_LAUNCHER}")
    set(CMAKE_${_azookey_lang}_COMPILER_LAUNCHER
        "${CMAKE_COMMAND};-E;env;VSLANG=1033")
    if(_azookey_existing_launcher)
      list(APPEND CMAKE_${_azookey_lang}_COMPILER_LAUNCHER
           ${_azookey_existing_launcher})
    endif()
  endforeach()
  unset(_azookey_existing_launcher)
endif()

option(AZOOKEY_BUILD_TESTS "Build unit tests" ON)
option(AZOOKEY_BUILD_BENCH "Build bench tools" ON)
option(AZOOKEY_BUILD_TSF_TIP "Build the Windows TSF TIP and its tests" ON)
option(AZOOKEY_BUILD_FUZZERS "Build Clang/libFuzzer targets" OFF)
# MSIX Option A（external-location / identity package, M28）では、実行体に
# side-by-side manifest の <msix> 要素を埋め込まないと Add-AppxPackage
# -ExternalLocation で登録しても runtime に package identity が付かない
# （Package.Current が null）。埋め込みは inference-host/CMakeLists.txt が行う。
# MSVC 以外のツールチェーン検証や、identity 無しの実行体が必要な場合は OFF。
option(AZOOKEY_EMBED_MSIX_IDENTITY
       "Embed pkg/msix/azookey_inference_host.exe.manifest into the host executable (MSVC)" ON)
option(AZOOKEY_ENABLE_ASAN "Enable AddressSanitizer for supported targets" OFF)
option(AZOOKEY_ENABLE_UBSAN "Enable UndefinedBehaviorSanitizer for supported targets" OFF)
option(AZOOKEY_ENABLE_COVERAGE
       "Enable Clang source-based coverage instrumentation for supported targets" OFF)

if(AZOOKEY_BUILD_FUZZERS AND NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  message(FATAL_ERROR
    "AZOOKEY_BUILD_FUZZERS requires a Clang toolchain with libFuzzer support")
endif()
option(AZOOKEY_FETCH_GOOGLETEST
       "Download GoogleTest via FetchContent when it is not found locally" OFF)
option(AZOOKEY_FETCH_LLAMA_CPP
       "Download llama.cpp via FetchContent when no local source tree is provided" OFF)
option(AZOOKEY_FETCH_WIL
       "Download WIL via FetchContent for Windows RAII helpers" OFF)
set(AZOOKEY_LLAMA_CPP_SOURCE_DIR "" CACHE PATH
    "Path to an existing llama.cpp source tree to add_subdirectory")
set(AZOOKEY_WIL_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/wil" CACHE PATH
    "Path to an existing WIL source tree to add_subdirectory")
set(AZOOKEY_LLAMA_CPP_GIT_TAG
    "427291b5b34cd914a31b3fd3b61a68f6184f4b9f" CACHE STRING
    "Pinned llama.cpp revision used when AZOOKEY_FETCH_LLAMA_CPP is ON")
set(AZOOKEY_WIL_GIT_TAG
    "b6ec76a2d9a609897f25a7fa0a0bdf4238e94e35" CACHE STRING
    "Pinned WIL revision used when AZOOKEY_FETCH_WIL is ON")
set(AZOOKEY_BACKEND "cpu" CACHE STRING "Default inference backend: cpu or cuda")
set_property(CACHE AZOOKEY_BACKEND PROPERTY STRINGS cpu cuda)

if(NOT AZOOKEY_BACKEND STREQUAL "cpu" AND NOT AZOOKEY_BACKEND STREQUAL "cuda")
  message(FATAL_ERROR "AZOOKEY_BACKEND must be either 'cpu' or 'cuda'")
endif()

function(azookey_add_llama_cpp)
  # llama.cpp and ggml do not link azookey_project_options. Scope the MSVC
  # hardening flags to their subdirectory so every third-party object that is
  # linked into the host receives CFG and Spectre instrumentation as well.
  if(MSVC)
    string(APPEND CMAKE_C_FLAGS " /guard:cf /Qspectre")
    string(APPEND CMAKE_CXX_FLAGS " /guard:cf /Qspectre")
  endif()

  if(AZOOKEY_LLAMA_CPP_SOURCE_DIR)
    set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
    set(LLAMA_BUILD_COMMON OFF CACHE BOOL "" FORCE)
    set(LLAMA_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(LLAMA_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
    set(LLAMA_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(LLAMA_BUILD_SERVER OFF CACHE BOOL "" FORCE)
    set(LLAMA_CURL OFF CACHE BOOL "" FORCE)
    set(GGML_RPC OFF CACHE BOOL "" FORCE)
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
    set(GGML_RPC OFF CACHE BOOL "" FORCE)
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
endfunction()

set(AZOOKEY_WITH_LLAMA_CPP OFF)
azookey_add_llama_cpp()

if(TARGET llama)
  set(AZOOKEY_WITH_LLAMA_CPP ON)
else()
  message(STATUS
    "llama.cpp target not available; Zenzai loader uses no-egress mock runtime. "
    "Set AZOOKEY_LLAMA_CPP_SOURCE_DIR or AZOOKEY_FETCH_LLAMA_CPP=ON for real loads.")
endif()

if(WIN32)
  set(WIL_BUILD_TESTS OFF CACHE BOOL "" FORCE)
  set(WIL_BUILD_PACKAGING OFF CACHE BOOL "" FORCE)
  if(EXISTS "${AZOOKEY_WIL_SOURCE_DIR}/CMakeLists.txt")
    add_subdirectory("${AZOOKEY_WIL_SOURCE_DIR}"
                     "${CMAKE_BINARY_DIR}/_deps/wil-build"
                     EXCLUDE_FROM_ALL)
  elseif(AZOOKEY_FETCH_WIL)
    include(FetchContent)
    FetchContent_Declare(
      wil
      GIT_REPOSITORY https://github.com/microsoft/wil.git
      GIT_TAG        ${AZOOKEY_WIL_GIT_TAG}
      GIT_PROGRESS   TRUE
    )
    FetchContent_MakeAvailable(wil)
  else()
    message(FATAL_ERROR
      "WIL source not found at '${AZOOKEY_WIL_SOURCE_DIR}'. "
      "Run 'git submodule update --init third_party/wil' or configure with "
      "-DAZOOKEY_FETCH_WIL=ON.")
  endif()
endif()

add_library(azookey_project_options INTERFACE)
target_compile_features(azookey_project_options INTERFACE cxx_std_20)
target_compile_options(azookey_project_options INTERFACE
  $<$<CXX_COMPILER_ID:MSVC>:/utf-8 /permissive- /EHsc /Zc:__cplusplus /sdl /guard:cf /Qspectre>
)

# Link-time mitigations are limited to the two native shipping binaries.
# /guard:cf lives in azookey_project_options so project static-library objects
# are instrumented before they are linked into these final images.
add_library(azookey_binary_hardening INTERFACE)
target_link_options(azookey_binary_hardening INTERFACE
  $<$<CXX_COMPILER_ID:MSVC>:/GUARD:CF /DYNAMICBASE /NXCOMPAT>
  $<$<AND:$<CXX_COMPILER_ID:MSVC>,$<EQUAL:${CMAKE_SIZEOF_VOID_P},8>>:/HIGHENTROPYVA /CETCOMPAT>
  # Search only the module directory, application directory, and System32.
  # The first two preserve the MSI app-local CRT contract for both the TIP DLL
  # and host executable while excluding the current directory and PATH.
  $<$<CXX_COMPILER_ID:MSVC>:/DEPENDENTLOADFLAG:0xB00>
)

if(AZOOKEY_ENABLE_ASAN)
  if(MSVC)
    # The default MSVC Debug flags include /RTC1, which cannot be combined
    # with AddressSanitizer. Preserve the remaining Debug defaults.
    string(REPLACE "/RTC1" "" CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG}")
    target_compile_options(azookey_project_options INTERFACE
      /fsanitize=address)
    target_link_options(azookey_project_options INTERFACE
      /INCREMENTAL:NO)
  elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(azookey_project_options INTERFACE
      -fsanitize=address -fno-omit-frame-pointer)
    target_link_options(azookey_project_options INTERFACE
      -fsanitize=address)
  else()
    message(FATAL_ERROR
      "AZOOKEY_ENABLE_ASAN is unsupported with ${CMAKE_CXX_COMPILER_ID}")
  endif()
endif()

if(AZOOKEY_ENABLE_UBSAN)
  if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(azookey_project_options INTERFACE
      -fsanitize=undefined -fno-omit-frame-pointer)
    target_link_options(azookey_project_options INTERFACE
      -fsanitize=undefined)
  else()
    message(FATAL_ERROR
      "AZOOKEY_ENABLE_UBSAN is unsupported with ${CMAKE_CXX_COMPILER_ID}")
  endif()
endif()

if(AZOOKEY_ENABLE_COVERAGE)
  if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    target_compile_options(azookey_project_options INTERFACE
      -fprofile-instr-generate -fcoverage-mapping)
    target_link_options(azookey_project_options INTERFACE
      -fprofile-instr-generate)
  else()
    message(FATAL_ERROR
      "AZOOKEY_ENABLE_COVERAGE requires a Clang toolchain")
  endif()
endif()

if(AZOOKEY_ENABLE_ASAN AND MSVC)
  if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "windows-asan currently supports x64 builds only")
  endif()
  get_filename_component(AZOOKEY_MSVC_COMPILER_DIR "${CMAKE_CXX_COMPILER}" DIRECTORY)
  set(AZOOKEY_MSVC_ASAN_RUNTIME
      "${AZOOKEY_MSVC_COMPILER_DIR}/clang_rt.asan_dynamic-x86_64.dll")
  if(NOT EXISTS "${AZOOKEY_MSVC_ASAN_RUNTIME}")
    message(FATAL_ERROR
      "MSVC ASan runtime not found next to the compiler: "
      "${AZOOKEY_MSVC_ASAN_RUNTIME}")
  endif()
endif()

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
  if(AZOOKEY_ENABLE_ASAN AND MSVC AND AZOOKEY_FETCH_GOOGLETEST)
    # A prebuilt GoogleTest may use non-ASan STL annotation settings. Force a
    # source build so GoogleTest and its consumers use the same MSVC flags.
    set(GTest_FOUND FALSE)
  else()
    find_package(GTest QUIET)
  endif()
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

  if(AZOOKEY_ENABLE_ASAN AND MSVC)
    if(TARGET gtest AND TARGET gtest_main)
      # MSVC's ASan-enabled STL changes iterator-annotation metadata. GoogleTest
      # must use the same setting as its consumers or links fail with LNK2038.
      target_compile_options(gtest PRIVATE /fsanitize=address)
      target_compile_options(gtest_main PRIVATE /fsanitize=address)
    elseif(TARGET GTest::gtest_main)
      message(FATAL_ERROR
        "windows-asan requires GoogleTest built from source with "
        "-DAZOOKEY_FETCH_GOOGLETEST=ON; a non-ASan prebuilt package is incompatible.")
    endif()
  endif()

  if(TARGET GTest::gtest_main)
    include(GoogleTest)
    set(AZOOKEY_GTEST_DISCOVERY_TIMEOUT 60 CACHE STRING
        "Seconds to wait while CMake enumerates GoogleTest cases")
    set(AZOOKEY_GTEST_TEST_TIMEOUT 60 CACHE STRING
        "Seconds to allow each discovered GoogleTest case to run")
    set(AZOOKEY_CTEST_PARALLEL_JOBS 4 CACHE STRING
        "Number of CTest cases to run concurrently")
    function(azookey_discover_tests target_name)
      set(one_value_args LABEL RESOURCE_LOCK TIMEOUT)
      cmake_parse_arguments(AZOOKEY_TEST "" "${one_value_args}" "" ${ARGN})

      set(azookey_test_timeout "${AZOOKEY_GTEST_TEST_TIMEOUT}")
      if(AZOOKEY_TEST_TIMEOUT)
        set(azookey_test_timeout "${AZOOKEY_TEST_TIMEOUT}")
      endif()

      set(azookey_test_properties TIMEOUT ${azookey_test_timeout})
      if(AZOOKEY_TEST_LABEL)
        list(APPEND azookey_test_properties LABELS ${AZOOKEY_TEST_LABEL})
      endif()
      if(AZOOKEY_TEST_RESOURCE_LOCK)
        list(APPEND azookey_test_properties RESOURCE_LOCK ${AZOOKEY_TEST_RESOURCE_LOCK})
      endif()

      if(AZOOKEY_ENABLE_ASAN AND MSVC)
        # Ninja's GoogleTest discovery runs the executable immediately after
        # linking. Place the runtime during configure so parallel POST_BUILD
        # discovery steps cannot race while copying the same DLL.
        file(COPY_FILE
          "${AZOOKEY_MSVC_ASAN_RUNTIME}"
          "${CMAKE_CURRENT_BINARY_DIR}/clang_rt.asan_dynamic-x86_64.dll"
          ONLY_IF_DIFFERENT)
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
  add_subdirectory(diagnostics)
  add_subdirectory(compat-test)
  add_subdirectory(settings-app)
endif()

if(WIN32 AND AZOOKEY_BUILD_TSF_TIP)
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
              --parallel "${AZOOKEY_CTEST_PARALLEL_JOBS}"
              --output-on-failure
              --no-tests=error
      USES_TERMINAL
      COMMENT "Build the host and test executables, then run CTest")
    add_dependencies(azookey_check ${AZOOKEY_CHECK_DEPENDS} azookey_inference_host)
  endif()
endif()

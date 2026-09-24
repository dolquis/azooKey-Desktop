add_library(azookey_core STATIC
  src/AppProfileResolver.cpp
  src/BracketPairing.cpp
  src/BracketSettings.cpp
  src/BracketTable.cpp
  src/CommandLine.cpp
  src/CrashRetention.cpp
  src/CrashReporting.cpp
  src/DoubleArrayTrie.cpp
  src/EtwLogger.cpp
  src/InputState.cpp
  src/KatakanaRewriter.cpp
  src/LiveConversionChunker.cpp
  src/NumberRewriter.cpp
  src/PlatformPaths.cpp
  src/PunctuationRules.cpp
  src/Redaction.cpp
  src/RewriterIndex.cpp
  src/RewriterNormalization.cpp
  src/RuntimeLogger.cpp
  src/RomajiKanaConverter.cpp
  src/SimpleConverter.cpp
  src/SymbolChainTable.cpp
  src/SymbolRewriter.cpp
  src/ThreadStackGuarantee.cpp
  src/UserActionMap.cpp
  src/Utf8.cpp
)

# Keep the crash filter out of binaries that only use the thread stack helper
# through azookey_ipc (notably the TIP DLL), including with unity builds.
set_source_files_properties(src/CrashReporting.cpp src/ThreadStackGuarantee.cpp
  PROPERTIES SKIP_UNITY_BUILD_INCLUSION ON)

target_include_directories(azookey_core
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(azookey_core
  PUBLIC azookey_project_options azookey_json
  PRIVATE azookey_project_warnings
)
if(WIN32)
  # Unity batches can include Win32 headers before sources using std::max.
  target_compile_definitions(azookey_core PRIVATE NOMINMAX)
  target_link_libraries(azookey_core PRIVATE ole32 shell32 icu advapi32 psapi)
else()
  find_package(ICU REQUIRED COMPONENTS uc)
  target_link_libraries(azookey_core PRIVATE ICU::uc)
endif()

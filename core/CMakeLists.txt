add_library(azookey_core STATIC
  src/BracketPairing.cpp
  src/BracketSettings.cpp
  src/BracketTable.cpp
  src/CommandLine.cpp
  src/DoubleArrayTrie.cpp
  src/KatakanaRewriter.cpp
  src/LiveConversionChunker.cpp
  src/NumberRewriter.cpp
  src/PlatformPaths.cpp
  src/Redaction.cpp
  src/RuntimeLogger.cpp
  src/RomajiKanaConverter.cpp
  src/SimpleConverter.cpp
  src/Utf8.cpp
)

target_include_directories(azookey_core
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(azookey_core
  PUBLIC azookey_project_options
  PRIVATE azookey_project_warnings azookey_json
)
if(WIN32)
  target_link_libraries(azookey_core PRIVATE ole32 shell32)
endif()

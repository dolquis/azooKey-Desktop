add_library(azookey_core STATIC
  src/LiveConversionChunker.cpp
  src/NumberRewriter.cpp
  src/Redaction.cpp
  src/RomajiKanaConverter.cpp
  src/SimpleConverter.cpp
)

target_include_directories(azookey_core
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(azookey_core
  PUBLIC azookey_project_options
  PRIVATE azookey_project_warnings
)

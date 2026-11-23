add_library(azookey_correction STATIC
  src/KeyboardAdjacencyModel.cpp
  src/ReadingHypothesis.cpp
  src/RomajiVariantNormalizer.cpp
  src/TypoCorrectionEngine.cpp
)

target_include_directories(azookey_correction PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(azookey_correction
  PUBLIC azookey_core azookey_project_options
  PRIVATE azookey_project_warnings
)

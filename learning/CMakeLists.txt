add_library(azookey_learning STATIC
  src/AutoWordStore.cpp
  src/DictionaryStore.cpp
  src/DpapiCrypto.cpp
  src/LearningStore.cpp
  src/Reranker.cpp
  src/TypoCorrectionStore.cpp
  src/UserDictionary.cpp
)

# These TSV stores define the same anonymous-namespace helpers.
set_source_files_properties(src/LearningStore.cpp src/TypoCorrectionStore.cpp
  PROPERTIES SKIP_UNITY_BUILD_INCLUSION ON)

target_include_directories(azookey_learning PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(azookey_learning
  PUBLIC azookey_core azookey_ipc azookey_project_options
  PRIVATE azookey_project_warnings
)

if(WIN32)
  target_link_libraries(azookey_learning PUBLIC crypt32)
endif()

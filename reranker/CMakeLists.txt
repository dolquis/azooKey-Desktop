find_package(Threads REQUIRED)
add_library(azookey_reranker STATIC src/FeatureExtractor.cpp src/TinyRerankerRuntime.cpp)
target_include_directories(azookey_reranker PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(azookey_reranker
  PUBLIC azookey_core PRIVATE Threads::Threads azookey_project_warnings
)

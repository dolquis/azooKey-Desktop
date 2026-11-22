# Vendored SQLite amalgamation for the typo v2 store
# (docs/typo-correction-learning-spec.md section 12.8, "SQLite の取り込み").
# A target of its own keeps azookey_project_options / azookey_project_warnings
# (C++ flags, /sdl, sanitizers and coverage) off the third-party C source; the
# hardening the shipping binaries need is added explicitly, as for llama.cpp.
set(AZOOKEY_SQLITE_DIR "${PROJECT_SOURCE_DIR}/third_party/sqlite")
add_library(azookey_sqlite3 STATIC "${AZOOKEY_SQLITE_DIR}/sqlite3.c")
set_target_properties(azookey_sqlite3 PROPERTIES UNITY_BUILD OFF)
set_source_files_properties("${AZOOKEY_SQLITE_DIR}/sqlite3.c"
  PROPERTIES SKIP_UNITY_BUILD_INCLUSION ON)
target_include_directories(azookey_sqlite3 SYSTEM INTERFACE "${AZOOKEY_SQLITE_DIR}")
target_compile_definitions(azookey_sqlite3 PRIVATE
  # Required by section 12.8.
  SQLITE_OMIT_LOAD_EXTENSION
  SQLITE_DQS=0
  SQLITE_TEMP_STORE=3
  # The store also sets PRAGMA secure_delete; this makes it the default.
  SQLITE_SECURE_DELETE
  SQLITE_OMIT_DEPRECATED
  SQLITE_OMIT_SHARED_CACHE
)
target_compile_options(azookey_sqlite3 PRIVATE
  $<$<C_COMPILER_ID:MSVC>:/guard:cf /Qspectre>
  $<$<BOOL:${AZOOKEY_CLANG_CL}>:/guard:cf>
)
if(NOT WIN32)
  find_package(Threads REQUIRED)
  target_link_libraries(azookey_sqlite3 PRIVATE Threads::Threads)
endif()

add_library(azookey_learning STATIC
  src/AutoWordStore.cpp
  src/ContextHash.cpp
  src/DictionaryStore.cpp
  src/DpapiCrypto.cpp
  src/LearningDecay.cpp
  src/LearningStore.cpp
  src/Persona.cpp
  src/Reranker.cpp
  src/Sha256.cpp
  src/TypoCorrectionStore.cpp
  src/TypoLearningStore.cpp
  src/TypoPattern.cpp
  src/UserDictionary.cpp
  src/UserDictionaryPos.cpp
  src/UserDictionaryTextImport.cpp
)

# These TSV stores define the same anonymous-namespace helpers, and
# TypoLearningStore.cpp includes the SQLite header with its macros.
set_source_files_properties(src/LearningStore.cpp src/TypoCorrectionStore.cpp
  src/TypoLearningStore.cpp
  PROPERTIES SKIP_UNITY_BUILD_INCLUSION ON)

target_include_directories(azookey_learning PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(azookey_learning
  PUBLIC azookey_core azookey_ipc azookey_project_options
  PRIVATE azookey_project_warnings azookey_sqlite3
)

if(WIN32)
  # normaliz: NormalizeString for the context hash NFC step.
  target_link_libraries(azookey_learning PUBLIC crypt32 normaliz)
endif()

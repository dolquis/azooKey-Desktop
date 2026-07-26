add_library(azookey_host STATIC
  src/Dispatcher.cpp
  src/InferenceEngine.cpp
  src/ModelCatalog.cpp
  src/RequestScheduler.cpp
  src/SettingsStore.cpp
  src/UserDictCli.cpp
  src/UserDataPaths.cpp
  src/ZenzaiModelConverter.cpp
)

target_include_directories(azookey_host PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(azookey_host
  PUBLIC azookey_core azookey_learning azookey_ipc azookey_project_options
  PRIVATE azookey_project_warnings
)
target_compile_definitions(azookey_host PUBLIC AZOOKEY_BACKEND_DEFAULT="${AZOOKEY_BACKEND}")
if(AZOOKEY_WITH_LLAMA_CPP)
  target_link_libraries(azookey_host PRIVATE llama)
  target_compile_definitions(azookey_host PUBLIC AZOOKEY_WITH_LLAMA_CPP=1)
else()
  target_compile_definitions(azookey_host PUBLIC AZOOKEY_WITH_LLAMA_CPP=0)
endif()

if(WIN32)
  target_link_libraries(azookey_host PUBLIC ole32 shell32)
endif()

add_executable(azookey_inference_host src/main.cpp)
target_link_libraries(azookey_inference_host
  PRIVATE azookey_host azookey_project_options azookey_project_warnings
)

# --- MSIX package identity（Option A / M28・DEV-101）--------------------------
# pkg/msix/azookey_inference_host.exe.manifest の <msix> 要素を実行体へ埋め込む。
# 埋め込みが無いと Add-AppxPackage -ExternalLocation で identity package を
# 登録しても host に package identity が付かない（Package.Current が null。
# docs/sideload-packaging-spec.md §1.1.2）。identity package を登録していない
# 環境では本要素は無視されるだけなので、通常の開発ビルドにも影響しない。
# /MANIFESTINPUT は /MANIFEST:EMBED を要求する（MSVC linker options）。
# manifest の 3 属性と pkg/msix/AppxManifest.xml の整合は
# scripts/tests/msix-identity-consistency.Tests.ps1 が CI で検証する。
if(WIN32 AND MSVC AND AZOOKEY_EMBED_MSIX_IDENTITY)
  set(AZOOKEY_MSIX_APP_MANIFEST
      "${PROJECT_SOURCE_DIR}/pkg/msix/azookey_inference_host.exe.manifest")
  if(EXISTS "${AZOOKEY_MSIX_APP_MANIFEST}")
    target_link_options(azookey_inference_host PRIVATE
      "/MANIFEST:EMBED"
      "/MANIFESTINPUT:${AZOOKEY_MSIX_APP_MANIFEST}"
    )
    # manifest だけを編集した場合も再リンクさせる（identity 値の差し替えが
    # 実行体へ反映されないまま VM smoke に持ち込まれるのを防ぐ）。
    set_property(TARGET azookey_inference_host APPEND
                 PROPERTY LINK_DEPENDS "${AZOOKEY_MSIX_APP_MANIFEST}")
  else()
    message(WARNING
      "MSIX identity manifest not found: ${AZOOKEY_MSIX_APP_MANIFEST}. "
      "azookey_inference_host.exe will be built without package identity metadata.")
  endif()
endif()

add_library(azookey_host_paths STATIC
  src/UserDataPaths.cpp
)
target_include_directories(azookey_host_paths PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(azookey_host_paths
  PUBLIC azookey_core azookey_project_options
  PRIVATE azookey_project_warnings
)

add_library(azookey_host STATIC
  src/Dispatcher.cpp
  src/InferenceEngine.cpp
  src/ModelCatalog.cpp
  src/RequestScheduler.cpp
  src/SettingsStore.cpp
  src/UserDictCli.cpp
  src/ZenzaiModelConverter.cpp
)

target_include_directories(azookey_host PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(azookey_host
  PUBLIC
    azookey_core
    azookey_host_paths
    azookey_learning
    azookey_ipc
    azookey_project_options
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
  target_link_libraries(azookey_host PUBLIC ole32 psapi)
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
# manifest の 3 属性と pkg/msix/AppxManifest.xml の整合は
# scripts/tests/msix-identity-consistency.Tests.ps1 が CI で検証する。
if(WIN32 AND MSVC AND AZOOKEY_EMBED_MSIX_IDENTITY)
  set(AZOOKEY_MSIX_APP_MANIFEST
      "${PROJECT_SOURCE_DIR}/pkg/msix/azookey_inference_host.exe.manifest")
  if(EXISTS "${AZOOKEY_MSIX_APP_MANIFEST}")
    # .manifest 拡張子のソースは CMake が manifest として分類し、MSVC では
    # `cmake -E vs_link_exe --manifests ...` 経由で mt.exe へ渡してリンカ生成
    # manifest（UAC フラグメント等）とマージ・埋め込みする。リンク辺の依存にも
    # 入るため、manifest だけを編集しても再リンクされる。
    # リンカへ /MANIFEST:EMBED を直接渡してはいけない: リンカ自身が埋め込んで
    # しまい、CMake が続けて呼ぶ mt.exe の入力が 0 になって
    # 「mt : command line error c10100a7」でビルドが落ちる。
    target_sources(azookey_inference_host PRIVATE "${AZOOKEY_MSIX_APP_MANIFEST}")
  else()
    message(WARNING
      "MSIX identity manifest not found: ${AZOOKEY_MSIX_APP_MANIFEST}. "
      "azookey_inference_host.exe will be built without package identity metadata.")
  endif()
endif()

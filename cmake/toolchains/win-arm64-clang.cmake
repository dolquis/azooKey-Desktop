# Windows on Arm (ARM64) cross-compile toolchain.
#
# 正典: docs/copilot-pc-backend-spec.md §8.1。x64 ホスト（`vcvarsall x64_arm64` /
# ilammy/msvc-dev-cmd の arch: amd64_arm64）から Ninja でクロスビルドする前提で使う。
#
# コンパイラを clang-cl とするのは llama.cpp / ggml が ARM ターゲットで MSVC cl.exe を
# FATAL_ERROR で拒否するためである（ggml/src/ggml-cpu/CMakeLists.txt）。
#
# CMAKE_SYSTEM_NAME を設定するとクロスコンパイルモードに入り、CMAKE_SYSTEM_PROCESSOR が
# ARM64 に固定される。設定しない場合 CMake はホスト値（Windows では AMD64）に追従し、
# ルート CMakeLists.txt の ARM64 フラグ分岐（§8.2）が無音でスキップされる。
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR ARM64)

set(CMAKE_C_COMPILER clang-cl)
set(CMAKE_CXX_COMPILER clang-cl)
set(CMAKE_C_COMPILER_TARGET arm64-pc-windows-msvc)
set(CMAKE_CXX_COMPILER_TARGET arm64-pc-windows-msvc)

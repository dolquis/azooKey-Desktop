# Embeds the pinned neologd_lexicon manifest (auto-word-registration-spec section 15.14) as
# bytes, so the installer never ships a file named after the pack (dictbuild/check_bundle.py).
# Usage: cmake -DINPUT=<manifest.json> -DOUTPUT=<generated.cpp> -P EmbedNeologdPin.cmake
file(READ "${INPUT}" content HEX)
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," bytes "${content}")
file(WRITE "${OUTPUT}.tmp"
  "// Generated from dictbuild/packs/neologd_lexicon.manifest.json. Do not edit.\n"
  "#include \"azookey/host/NeologdPack.h\"\n\n"
  "namespace azookey::host {\n"
  "std::string_view PinnedNeologdPackManifestJson() {\n"
  "  static constexpr unsigned char kBytes[] = {${bytes}};\n"
  "  return {reinterpret_cast<const char*>(kBytes), sizeof(kBytes)};\n"
  "}\n"
  "}  // namespace azookey::host\n")
file(COPY_FILE "${OUTPUT}.tmp" "${OUTPUT}" ONLY_IF_DIFFERENT)
file(REMOVE "${OUTPUT}.tmp")

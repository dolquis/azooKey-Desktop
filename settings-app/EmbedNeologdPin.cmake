# Embeds the pinned neologd_lexicon manifest for the settings app, which shows its
# attribution before the pack is enabled (auto-word-registration-spec section 15.14).
# The installer never ships a file named after the pack (dictbuild/check_bundle.py).
# Usage: cmake -DINPUT=<manifest.json> -DOUTPUT=<generated.cpp> -P EmbedNeologdPin.cmake
file(READ "${INPUT}" content HEX)
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," bytes "${content}")
file(WRITE "${OUTPUT}.tmp"
  "// Generated from dictbuild/packs/neologd_lexicon.manifest.json. Do not edit.\n"
  "#include <string_view>\n\n"
  "namespace azookey::settings {\n"
  "std::string_view PinnedNeologdPackManifestJson() {\n"
  "  static constexpr unsigned char kBytes[] = {${bytes}};\n"
  "  return {reinterpret_cast<const char*>(kBytes), sizeof(kBytes)};\n"
  "}\n"
  "}  // namespace azookey::settings\n")
file(COPY_FILE "${OUTPUT}.tmp" "${OUTPUT}" ONLY_IF_DIFFERENT)
file(REMOVE "${OUTPUT}.tmp")

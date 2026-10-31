# Embeds settings/default-model-catalog.json (model-management-spec section 3.1.1) as bytes, so
# the installer and the verification package carry no catalog file a user could edit to move
# the pin.
# Usage: cmake -DINPUT=<catalog.json> -DOUTPUT=<generated.cpp> -P EmbedDefaultModelCatalog.cmake
file(READ "${INPUT}" content HEX)
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," bytes "${content}")
file(WRITE "${OUTPUT}.tmp"
  "// Generated from settings/default-model-catalog.json. Do not edit.\n"
  "#include \"azookey/host/ModelCatalog.h\"\n\n"
  "namespace azookey::host {\n"
  "std::string_view DefaultModelCatalogJson() {\n"
  "  static constexpr unsigned char kBytes[] = {${bytes}};\n"
  "  return {reinterpret_cast<const char*>(kBytes), sizeof(kBytes)};\n"
  "}\n"
  "}  // namespace azookey::host\n")
file(COPY_FILE "${OUTPUT}.tmp" "${OUTPUT}" ONLY_IF_DIFFERENT)
file(REMOVE "${OUTPUT}.tmp")

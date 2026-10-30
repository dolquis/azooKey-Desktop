#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>

#include "azookey/host/ModelCatalog.h"

namespace {

constexpr char kSha256[] = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";

std::filesystem::path TestDir(const char* name) {
  auto path = std::filesystem::temp_directory_path() / name;
  std::filesystem::remove_all(path);
  std::filesystem::create_directories(path);
  return path;
}

void Touch(const std::filesystem::path& path) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream out(path, std::ios::binary);
  out << "not a real model";
}

std::string SingleModelCatalogJson(std::string file_name = "zenzai-small-q4.gguf") {
  return std::string(R"({
    "schemaVersion": 1,
    "models": [
      {
        "id": "zenzai-small-q4",
        "fileName": ")") +
         file_name + R"(",
        "sha256": ")" +
         kSha256 + R"("
      }
    ]
  })";
}

// Pinned values from docs/sideload-packaging-spec.md section 1.6.2: zenz stays the default and
// kanakanji-edge-33m Q8_0 is a selectable entry.
constexpr char kZenzSha256[] = "29c223d4c23327b80fd13ebb5ab2555057a46317997d5da391584ffbef0db673";
constexpr char kEdgeSha256[] = "924b1a0e2b95a17208366048071b9fd259364f24fbc6090f0e4fd680f8bdb146";

std::string ZenzWithEdgeCatalogJson() {
  return std::string(R"({
    "schemaVersion": 1,
    "defaultModelId": "zenz-v3.2-small",
    "models": [
      {
        "id": "zenz-v3.2-small",
        "displayName": "zenz-v3.2-small",
        "fileName": "ggml-model-Q5_K_M.gguf",
        "sha256": ")") +
         kZenzSha256 + R"("
      },
      {
        "id": "kanakanji-edge-33m",
        "displayName": "kanakanji-edge 33M",
        "fileName": "kanakanji-edge-33m-Q8_0.gguf",
        "sha256": ")" +
         kEdgeSha256 + R"(",
        "backendHint": "llama.cpp",
        "promptFormat": "kanakanji-edge-v1"
      }
    ]
  })";
}

}  // namespace

TEST(ModelCatalogTest, ParsesCatalogAndFillsDefaults) {
  const auto parsed = azookey::host::ParseModelCatalogJson(SingleModelCatalogJson());

  ASSERT_TRUE(parsed.ok) << parsed.error.value_or("");
  EXPECT_EQ(parsed.catalog.schema_version, 1u);
  ASSERT_EQ(parsed.catalog.models.size(), 1u);
  EXPECT_EQ(parsed.catalog.default_model_id, "zenzai-small-q4");
  EXPECT_EQ(parsed.catalog.models[0].id, "zenzai-small-q4");
  EXPECT_EQ(parsed.catalog.models[0].display_name, "zenzai-small-q4");
  EXPECT_EQ(parsed.catalog.models[0].file_name, "zenzai-small-q4.gguf");
  EXPECT_EQ(parsed.catalog.models[0].sha256, kSha256);
  EXPECT_EQ(parsed.catalog.models[0].backend_hint, "llama.cpp");
  EXPECT_EQ(parsed.catalog.models[0].prompt_format, azookey::host::ZenzaiPromptFormat::ZenzV3);
}

TEST(ModelCatalogTest, ParsesPromptFormatAndRejectsUnknownValues) {
  const auto catalog_with = [](const std::string& prompt_format_json) {
    return std::string(R"({
    "schemaVersion": 1,
    "models": [
      {
        "id": "edge",
        "fileName": "edge.gguf",
        "sha256": ")") +
           kSha256 + R"(",
        "promptFormat": )" +
           prompt_format_json + R"(
      }
    ]
  })";
  };

  const auto edge = azookey::host::ParseModelCatalogJson(catalog_with(R"("kanakanji-edge-v1")"));
  ASSERT_TRUE(edge.ok) << edge.error.value_or("");
  EXPECT_EQ(edge.catalog.models[0].prompt_format,
            azookey::host::ZenzaiPromptFormat::KanakanjiEdgeV1);

  const auto zenz = azookey::host::ParseModelCatalogJson(catalog_with(R"("zenz-v3")"));
  ASSERT_TRUE(zenz.ok) << zenz.error.value_or("");
  EXPECT_EQ(zenz.catalog.models[0].prompt_format, azookey::host::ZenzaiPromptFormat::ZenzV3);

  EXPECT_FALSE(azookey::host::ParseModelCatalogJson(catalog_with(R"("zenz-v4")")).ok);
  EXPECT_FALSE(azookey::host::ParseModelCatalogJson(catalog_with(R"("")")).ok);
  EXPECT_FALSE(azookey::host::ParseModelCatalogJson(catalog_with(R"("ZENZ-V3")")).ok);
  EXPECT_FALSE(azookey::host::ParseModelCatalogJson(catalog_with("1")).ok);
}

TEST(ModelCatalogTest, UsesExplicitDefaultModelId) {
  const auto parsed = azookey::host::ParseModelCatalogJson(std::string(R"({
    "schemaVersion": 1,
    "defaultModelId": "large",
    "models": [
      {
        "id": "small",
        "displayName": "Small",
        "fileName": "small.gguf",
        "sha256": ")") + kSha256 + R"("
      },
      {
        "id": "large",
        "displayName": "Large",
        "fileName": "large.GGUF",
        "sha256": ")" + kSha256 + R"(",
        "backendHint": "llama.cpp"
      }
    ]
  })");

  ASSERT_TRUE(parsed.ok) << parsed.error.value_or("");
  EXPECT_EQ(parsed.catalog.default_model_id, "large");
  ASSERT_EQ(parsed.catalog.models.size(), 2u);
  EXPECT_EQ(parsed.catalog.models[1].file_name, "large.GGUF");
}

TEST(ModelCatalogTest, RejectsUnsafeOrInvalidEntries) {
  EXPECT_FALSE(azookey::host::ParseModelCatalogJson(SingleModelCatalogJson("..\\evil.gguf")).ok);
  EXPECT_FALSE(azookey::host::ParseModelCatalogJson(SingleModelCatalogJson("../evil.gguf")).ok);
  EXPECT_FALSE(azookey::host::ParseModelCatalogJson(SingleModelCatalogJson(".gguf")).ok);
  EXPECT_FALSE(
      azookey::host::ParseModelCatalogJson(SingleModelCatalogJson("other.txt\\u0000.gguf")).ok);
  EXPECT_FALSE(azookey::host::ParseModelCatalogJson(SingleModelCatalogJson("zenzai.txt")).ok);
  EXPECT_FALSE(azookey::host::ParseModelCatalogJson(R"({
    "schemaVersion": 1,
    "models": [
      { "id": "bad", "fileName": "bad.gguf", "sha256": "not-sha" }
    ]
  })")
                   .ok);
}

TEST(ModelCatalogTest, RejectsEmptyDisplayName) {
  EXPECT_FALSE(azookey::host::ParseModelCatalogJson(std::string(R"({
    "schemaVersion": 1,
    "models": [
      {
        "id": "empty-display",
        "displayName": "",
        "fileName": "empty-display.gguf",
        "sha256": ")") + kSha256 + R"("
      }
    ]
  })")
                   .ok);
}

TEST(ModelCatalogTest, RejectsDuplicateIdsAndUnknownDefault) {
  EXPECT_FALSE(azookey::host::ParseModelCatalogJson(std::string(R"({
    "schemaVersion": 1,
    "models": [
      { "id": "dup", "fileName": "a.gguf", "sha256": ")") +
                                                    kSha256 + R"(" },
      { "id": "dup", "fileName": "b.gguf", "sha256": ")" +
                                                    kSha256 + R"(" }
    ]
  })")
                   .ok);

  EXPECT_FALSE(azookey::host::ParseModelCatalogJson(std::string(R"({
    "schemaVersion": 1,
    "defaultModelId": "missing",
    "models": [
      { "id": "present", "fileName": "present.gguf", "sha256": ")") +
                                                    kSha256 + R"(" }
    ]
  })")
                   .ok);
}

TEST(ModelCatalogTest, ResolvesDefaultModelUnderZenzaiDirectory) {
  const auto parsed = azookey::host::ParseModelCatalogJson(SingleModelCatalogJson());
  ASSERT_TRUE(parsed.ok) << parsed.error.value_or("");

  const auto root = TestDir("azookey_model_catalog_resolve");
  const auto zenzai_dir = azookey::host::DefaultZenzaiModelDirectory(root / "models");
  const auto model_path = zenzai_dir / "zenzai-small-q4.gguf";
  Touch(model_path);

  const auto resolved = azookey::host::ResolveModelCatalogModel(parsed.catalog, zenzai_dir);

  EXPECT_TRUE(resolved.ok()) << resolved.error.value_or("");
  ASSERT_TRUE(resolved.model.has_value());
  EXPECT_TRUE(resolved.model->exists);
  EXPECT_EQ(resolved.model->entry.id, "zenzai-small-q4");
  EXPECT_EQ(resolved.model->path, model_path);

  std::filesystem::remove_all(root);
}

TEST(ModelCatalogTest, ReportsMissingLocalFileButKeepsResolvedPath) {
  const auto parsed = azookey::host::ParseModelCatalogJson(SingleModelCatalogJson());
  ASSERT_TRUE(parsed.ok) << parsed.error.value_or("");

  const auto root = TestDir("azookey_model_catalog_missing");
  const auto zenzai_dir = azookey::host::DefaultZenzaiModelDirectory(root / "models");

  const auto resolved = azookey::host::ResolveModelCatalogModel(parsed.catalog, zenzai_dir);

  EXPECT_EQ(resolved.status, azookey::host::ModelCatalogResolveStatus::MissingLocalFile);
  ASSERT_TRUE(resolved.model.has_value());
  EXPECT_FALSE(resolved.model->exists);
  EXPECT_EQ(resolved.model->path, zenzai_dir / "zenzai-small-q4.gguf");

  std::filesystem::remove_all(root);
}

TEST(ModelCatalogTest, ReportsUnknownRequestedModelId) {
  const auto parsed = azookey::host::ParseModelCatalogJson(SingleModelCatalogJson());
  ASSERT_TRUE(parsed.ok) << parsed.error.value_or("");

  const auto resolved =
      azookey::host::ResolveModelCatalogModel(parsed.catalog, "C:/models/zenzai", "missing");

  EXPECT_EQ(resolved.status, azookey::host::ModelCatalogResolveStatus::UnknownModelId);
  EXPECT_FALSE(resolved.model.has_value());
  ASSERT_TRUE(resolved.error.has_value());
  EXPECT_NE(resolved.error->find("missing"), std::string::npos);
}

TEST(ModelCatalogTest, ParsesEdgeEntryWithoutChangingZenzDefault) {
  const auto parsed = azookey::host::ParseModelCatalogJson(ZenzWithEdgeCatalogJson());

  ASSERT_TRUE(parsed.ok) << parsed.error.value_or("");
  EXPECT_EQ(parsed.catalog.default_model_id, "zenz-v3.2-small");
  ASSERT_EQ(parsed.catalog.models.size(), 2u);

  const auto& zenz = parsed.catalog.models[0];
  EXPECT_EQ(zenz.sha256, kZenzSha256);
  EXPECT_EQ(zenz.backend_hint, "llama.cpp");
  EXPECT_EQ(zenz.prompt_format, azookey::host::ZenzaiPromptFormat::ZenzV3);

  const auto& edge = parsed.catalog.models[1];
  EXPECT_EQ(edge.id, "kanakanji-edge-33m");
  EXPECT_EQ(edge.display_name, "kanakanji-edge 33M");
  EXPECT_EQ(edge.file_name, "kanakanji-edge-33m-Q8_0.gguf");
  EXPECT_EQ(edge.sha256, kEdgeSha256);
  EXPECT_EQ(edge.backend_hint, "llama.cpp");
  EXPECT_EQ(edge.prompt_format, azookey::host::ZenzaiPromptFormat::KanakanjiEdgeV1);
}

TEST(ModelCatalogTest, ResolvesRequestedEdgeEntryAndKeepsZenzAsDefault) {
  const auto parsed = azookey::host::ParseModelCatalogJson(ZenzWithEdgeCatalogJson());
  ASSERT_TRUE(parsed.ok) << parsed.error.value_or("");

  const auto root = TestDir("azookey_model_catalog_edge");
  const auto zenzai_dir = azookey::host::DefaultZenzaiModelDirectory(root / "models");
  const auto edge_path = zenzai_dir / "kanakanji-edge-33m-Q8_0.gguf";
  Touch(edge_path);

  const auto edge =
      azookey::host::ResolveModelCatalogModel(parsed.catalog, zenzai_dir, "kanakanji-edge-33m");
  EXPECT_TRUE(edge.ok()) << edge.error.value_or("");
  ASSERT_TRUE(edge.model.has_value());
  EXPECT_EQ(edge.model->path, edge_path);
  EXPECT_EQ(edge.model->entry.sha256, kEdgeSha256);
  EXPECT_EQ(edge.model->entry.prompt_format, azookey::host::ZenzaiPromptFormat::KanakanjiEdgeV1);

  const auto fallback = azookey::host::ResolveModelCatalogModel(parsed.catalog, zenzai_dir);
  EXPECT_EQ(fallback.status, azookey::host::ModelCatalogResolveStatus::MissingLocalFile);
  ASSERT_TRUE(fallback.model.has_value());
  EXPECT_EQ(fallback.model->entry.id, "zenz-v3.2-small");
  EXPECT_EQ(fallback.model->path, zenzai_dir / "ggml-model-Q5_K_M.gguf");

  std::filesystem::remove_all(root);
}

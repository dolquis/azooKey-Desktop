#include <gtest/gtest.h>

#include <cctype>
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

// Pinned values: edge from docs/sideload-packaging-spec.md section 1.6.2, zenz from the pin in
// .github/workflows/windows.yml. zenz stays the default and edge Q8_0 is a selectable entry.
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

namespace {

constexpr char kZenzRevision[] = "c67e03e07d215c869f591b274c1631170d3e11fe";
constexpr char kEdgeRevision[] = "da588df2db17e5ab69316986823806ec5f911450";

// One v2 entry whose artifacts object is given whole.
std::string V2CatalogJson(const std::string& artifacts, const std::string& extra_entry_keys = "") {
  return R"({"schemaVersion": 2, "models": [{"id": "m", )" + extra_entry_keys + R"("artifacts": )" +
         artifacts + "}]}";
}

// artifacts with a llama.cpp artifact; extra is appended inside it.
std::string LlamaArtifact(const std::string& extra) {
  return std::string(R"({"llama.cpp": {"fileName": "m.gguf", "sha256": ")") + kSha256 + "\"" +
         extra + "}}";
}

std::string SourceKeys(const std::string& repo, const std::string& revision) {
  return R"(, "source": {"huggingFaceRepo": ")" + repo + R"(", "revision": ")" + revision + "\"}";
}

std::string ErrorOf(const std::string& json) {
  const auto parsed = azookey::host::ParseModelCatalogJson(json);
  EXPECT_FALSE(parsed.ok) << json;
  return parsed.error.value_or("");
}

}  // namespace

TEST(ModelCatalogTest, ParsesV2ArtifactWithHuggingFaceSource) {
  const auto parsed = azookey::host::ParseModelCatalogJson(V2CatalogJson(
      LlamaArtifact(R"(, "sizeBytes": 1234)" + SourceKeys("owner/repo-name", kZenzRevision)),
      R"("promptFormat": "kanakanji-edge-v1", )"));
  ASSERT_TRUE(parsed.ok) << parsed.error.value_or("");
  EXPECT_EQ(parsed.catalog.schema_version, 2u);
  ASSERT_EQ(parsed.catalog.models.size(), 1u);
  const auto& entry = parsed.catalog.models[0];
  EXPECT_EQ(entry.file_name, "m.gguf");
  EXPECT_EQ(entry.sha256, kSha256);
  EXPECT_EQ(entry.backend_hint, "llama.cpp");
  EXPECT_EQ(entry.prompt_format, azookey::host::ZenzaiPromptFormat::KanakanjiEdgeV1);
  EXPECT_EQ(entry.size_bytes, 1234u);
  ASSERT_TRUE(entry.source.has_value());
  EXPECT_EQ(entry.source->hugging_face_repo, "owner/repo-name");
  EXPECT_EQ(entry.source->revision, kZenzRevision);
  EXPECT_EQ(
      azookey::host::ModelCatalogDownloadUrl(entry),
      std::string("https://huggingface.co/owner/repo-name/resolve/") + kZenzRevision + "/m.gguf");
}

TEST(ModelCatalogTest, EntriesWithoutSourceHaveNoDownloadUrl) {
  const auto parsed = azookey::host::ParseModelCatalogJson(V2CatalogJson(LlamaArtifact("")));
  ASSERT_TRUE(parsed.ok) << parsed.error.value_or("");
  EXPECT_FALSE(parsed.catalog.models[0].source.has_value());
  EXPECT_EQ(parsed.catalog.models[0].size_bytes, 0u);
  EXPECT_FALSE(azookey::host::ModelCatalogDownloadUrl(parsed.catalog.models[0]).has_value());

  // A v1 entry never has a source.
  const auto v1 = azookey::host::ParseModelCatalogJson(SingleModelCatalogJson());
  ASSERT_TRUE(v1.ok) << v1.error.value_or("");
  EXPECT_EQ(v1.catalog.schema_version, 1u);
  EXPECT_FALSE(azookey::host::ModelCatalogDownloadUrl(v1.catalog.models[0]).has_value());
}

TEST(ModelCatalogTest, DownloadUrlPercentEncodesTheFileName) {
  azookey::host::ModelCatalogEntry entry;
  entry.file_name = "a b+c.gguf";
  entry.source = azookey::host::ModelCatalogSource{"owner/repo", kEdgeRevision};
  EXPECT_EQ(azookey::host::ModelCatalogDownloadUrl(entry),
            std::string("https://huggingface.co/owner/repo/resolve/") + kEdgeRevision +
                "/a%20b%2Bc.gguf");
}

TEST(ModelCatalogTest, RejectionsNameTheVersionOrTheField) {
  const auto version = ErrorOf(R"({"schemaVersion": 3, "models": []})");
  EXPECT_NE(version.find("schemaVersion must be 1 or 2"), std::string::npos) << version;
  // A v2 key in a v1 catalog, and a v1 key in a v2 entry, name the key and the version.
  const auto v1_with_artifacts = ErrorOf(std::string(R"({"schemaVersion": 1, "models": [{)") +
                                         R"("id": "m", "fileName": "m.gguf", "sha256": ")" +
                                         kSha256 + R"(", "artifacts": {}}]})");
  EXPECT_NE(v1_with_artifacts.find("schemaVersion 1: artifacts"), std::string::npos)
      << v1_with_artifacts;
  const auto v2_with_hint =
      ErrorOf(V2CatalogJson(LlamaArtifact(""), R"("backendHint": "llama.cpp", )"));
  EXPECT_NE(v2_with_hint.find("schemaVersion 2: backendHint"), std::string::npos) << v2_with_hint;
  const auto top = ErrorOf(R"({"schemaVersion": 2, "models": [], "extra": 1})");
  EXPECT_NE(top.find("top-level field for schemaVersion 2: extra"), std::string::npos) << top;
  const auto top_v1 = ErrorOf(R"({"schemaVersion": 1, "models": [], "extra": 1})");
  EXPECT_NE(top_v1.find("top-level field for schemaVersion 1: extra"), std::string::npos) << top_v1;
  // An unsupported version is reported before any field it might not define.
  const auto v3 = ErrorOf(R"({"schemaVersion": 3, "models": [], "extra": 1})");
  EXPECT_NE(v3.find("schemaVersion must be 1 or 2"), std::string::npos) << v3;
  // R2 stays undefined until DEV-1768.
  const auto r2 = ErrorOf(V2CatalogJson(R"({"onnxruntime-genai": {}})"));
  EXPECT_NE(r2.find("not supported for schemaVersion 2: onnxruntime-genai"), std::string::npos)
      << r2;
  const auto no_llama = ErrorOf(V2CatalogJson("{}"));
  EXPECT_NE(no_llama.find("requires llama.cpp"), std::string::npos) << no_llama;
  const auto no_artifacts = ErrorOf(R"({"schemaVersion": 2, "models": [{"id": "m"}]})");
  EXPECT_NE(no_artifacts.find("requires artifacts"), std::string::npos) << no_artifacts;
}

TEST(ModelCatalogTest, RejectsUnpinnedOrUnsafeSources) {
  const auto with_source = [](const std::string& repo, const std::string& revision) {
    return V2CatalogJson(LlamaArtifact(R"(, "sizeBytes": 10)" + SourceKeys(repo, revision)));
  };
  EXPECT_TRUE(azookey::host::ParseModelCatalogJson(with_source("o/r", kEdgeRevision)).ok);
  // A branch, a short or an uppercase commit id: the artifact could change under the pin.
  for (const char* revision : {"main", "c67e03e", "C67E03E07D215C869F591B274C1631170D3E11FE"}) {
    const auto error = ErrorOf(with_source("o/r", revision));
    EXPECT_NE(error.find("revision"), std::string::npos) << revision << ": " << error;
  }
  // Only owner/name on huggingface.co; no other host, path or traversal.
  for (const char* repo :
       {"repo", "o/r/x", "../r", "o/..", "o/", "/r", "https://e.com/r", "o/.r", "o r/x", "o/r?x"}) {
    const auto error = ErrorOf(with_source(repo, kEdgeRevision));
    EXPECT_NE(error.find("huggingFaceRepo"), std::string::npos) << repo << ": " << error;
  }
  // The size is the download limit, so a source requires it.
  const auto no_size = ErrorOf(V2CatalogJson(LlamaArtifact(SourceKeys("o/r", kEdgeRevision))));
  EXPECT_NE(no_size.find("requires sizeBytes"), std::string::npos) << no_size;
  for (const char* size : {"0", "-1", "1.5", "\"10\"", "9007199254740992"}) {
    const auto error =
        ErrorOf(V2CatalogJson(LlamaArtifact(std::string(R"(, "sizeBytes": )") + size)));
    EXPECT_NE(error.find("sizeBytes"), std::string::npos) << size << ": " << error;
  }
  // The product's fixed limit (4 GiB) bounds every download.
  EXPECT_TRUE(azookey::host::ParseModelCatalogJson(
                  V2CatalogJson(LlamaArtifact(R"(, "sizeBytes": 4294967296)")))
                  .ok);
  const auto over = ErrorOf(V2CatalogJson(LlamaArtifact(R"(, "sizeBytes": 4294967297)")));
  EXPECT_NE(over.find("exceeds the limit"), std::string::npos) << over;
  // A catalog cannot name a URL or another host.
  const auto url = ErrorOf(V2CatalogJson(LlamaArtifact(R"(, "url": "https://e.com/m.gguf")")));
  EXPECT_NE(url.find("unknown field for schemaVersion 2: url"), std::string::npos) << url;
  const auto host = ErrorOf(V2CatalogJson(LlamaArtifact(
      R"(, "sizeBytes": 10, "source": {"huggingFaceRepo": "o/r", "host": "e.com"})")));
  EXPECT_NE(host.find("unknown field for schemaVersion 2: host"), std::string::npos) << host;
}

TEST(ModelCatalogTest, EmbeddedDefaultCatalogPinsZenzAsDefaultAndEdgeAsSelectable) {
  const auto parsed =
      azookey::host::ParseModelCatalogJson(azookey::host::DefaultModelCatalogJson());
  ASSERT_TRUE(parsed.ok) << parsed.error.value_or("");
  EXPECT_EQ(parsed.catalog.schema_version, 2u);
  EXPECT_EQ(parsed.catalog.default_model_id, "zenz-v3.2-small");
  ASSERT_EQ(parsed.catalog.models.size(), 2u);

  // docs/sideload-packaging-spec.md section 1.6.2.
  const auto& zenz = parsed.catalog.models[0];
  EXPECT_EQ(zenz.id, "zenz-v3.2-small");
  EXPECT_EQ(zenz.file_name, "ggml-model-Q5_K_M.gguf");
  EXPECT_EQ(zenz.sha256, kZenzSha256);
  EXPECT_EQ(zenz.size_bytes, 73871936u);
  EXPECT_EQ(zenz.prompt_format, azookey::host::ZenzaiPromptFormat::ZenzV3);
  EXPECT_EQ(azookey::host::ModelCatalogDownloadUrl(zenz),
            std::string("https://huggingface.co/Miwa-Keita/zenz-v3.2-small-gguf/resolve/") +
                kZenzRevision + "/ggml-model-Q5_K_M.gguf");

  const auto& edge = parsed.catalog.models[1];
  EXPECT_EQ(edge.id, "kanakanji-edge-33m");
  EXPECT_EQ(edge.file_name, "kanakanji-edge-33m-Q8_0.gguf");
  EXPECT_EQ(edge.sha256, kEdgeSha256);
  EXPECT_EQ(edge.size_bytes, 35806592u);
  EXPECT_EQ(edge.prompt_format, azookey::host::ZenzaiPromptFormat::KanakanjiEdgeV1);
  EXPECT_EQ(azookey::host::ModelCatalogDownloadUrl(edge),
            std::string("https://huggingface.co/yuuki14202028/kanakanji-edge-33m/resolve/") +
                kEdgeRevision + "/kanakanji-edge-33m-Q8_0.gguf");
}

TEST(ModelCatalogTest, FileNamesArePrintableAsciiAndNeverWindowsDeviceNames) {
  EXPECT_TRUE(azookey::host::ParseModelCatalogJson(SingleModelCatalogJson("a b-c_1.Q8.gguf")).ok);
  // Non-ASCII (UTF-8 bytes of "あ") would be one name in the URL and another on disk.
  EXPECT_FALSE(
      azookey::host::ParseModelCatalogJson(SingleModelCatalogJson("\xE3\x81\x82.gguf")).ok);
  EXPECT_FALSE(azookey::host::ParseModelCatalogJson(SingleModelCatalogJson("model\x7f.gguf")).ok);
  for (const char* name : {"CON.gguf", "con.gguf", "Nul.gguf", "aux.model.gguf", "PRN .gguf",
                           "COM1.gguf", "lpt9.gguf", "COM0.gguf", "CONIN$.gguf", "conout$.gguf"}) {
    const auto parsed = azookey::host::ParseModelCatalogJson(SingleModelCatalogJson(name));
    EXPECT_FALSE(parsed.ok) << name;
    EXPECT_NE(parsed.error.value_or("").find("safe GGUF file name"), std::string::npos) << name;
  }
  // Only the exact device names are reserved.
  for (const char* name : {"CONSOLE.gguf", "COM10.gguf", "LPT.gguf", "nul-model.gguf"}) {
    EXPECT_TRUE(azookey::host::ParseModelCatalogJson(SingleModelCatalogJson(name)).ok) << name;
  }
}

TEST(ModelCatalogTest, MatchesSelectedPathByDirectoryFileNameAndSha256) {
  const auto parsed =
      azookey::host::ParseModelCatalogJson(azookey::host::DefaultModelCatalogJson());
  ASSERT_TRUE(parsed.ok) << parsed.error.value_or("");
  const auto zenzai_dir =
      azookey::host::DefaultZenzaiModelDirectory(std::filesystem::path("root") / "models");

  const auto* edge = azookey::host::MatchSelectedPathToCatalog(
      parsed.catalog, zenzai_dir, zenzai_dir / "kanakanji-edge-33m-Q8_0.gguf", kEdgeSha256);
  ASSERT_NE(edge, nullptr);
  EXPECT_EQ(edge->id, "kanakanji-edge-33m");
  EXPECT_EQ(edge->prompt_format, azookey::host::ZenzaiPromptFormat::KanakanjiEdgeV1);

  // File names compare without case, as on Windows; the SHA-256 may be in either case.
  std::string upper_sha = kZenzSha256;
  for (auto& ch : upper_sha) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
  const auto* zenz = azookey::host::MatchSelectedPathToCatalog(
      parsed.catalog, zenzai_dir, zenzai_dir / "GGML-MODEL-Q5_K_M.GGUF", upper_sha);
  ASSERT_NE(zenz, nullptr);
  EXPECT_EQ(zenz->id, "zenz-v3.2-small");

  // The same file name with another SHA-256 (a user's GGUF) is not the catalog's.
  EXPECT_EQ(azookey::host::MatchSelectedPathToCatalog(
                parsed.catalog, zenzai_dir, zenzai_dir / "ggml-model-Q5_K_M.gguf", kEdgeSha256),
            nullptr);
  // Outside zenzai\ or in a subdirectory of it.
  EXPECT_EQ(azookey::host::MatchSelectedPathToCatalog(
                parsed.catalog, zenzai_dir,
                std::filesystem::path("root") / "ggml-model-Q5_K_M.gguf", kZenzSha256),
            nullptr);
  EXPECT_EQ(
      azookey::host::MatchSelectedPathToCatalog(
          parsed.catalog, zenzai_dir, zenzai_dir / "sub" / "ggml-model-Q5_K_M.gguf", kZenzSha256),
      nullptr);
  // A ".." that leads back into zenzai\ is normalized first.
  EXPECT_NE(azookey::host::MatchSelectedPathToCatalog(
                parsed.catalog, zenzai_dir, zenzai_dir / "sub" / ".." / "ggml-model-Q5_K_M.gguf",
                kZenzSha256),
            nullptr);
  // An unusable hash matches nothing.
  EXPECT_EQ(azookey::host::MatchSelectedPathToCatalog(parsed.catalog, zenzai_dir,
                                                      zenzai_dir / "ggml-model-Q5_K_M.gguf", ""),
            nullptr);
}

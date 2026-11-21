// M73 (docs/homophone-notes-spec.md sections 4, 6 and 7): the Host merges the
// notes into the QueryCandidates list and each QueryBatchConversion segment.
#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "ScopedTempDirectory.h"
#include "azookey/core/IConverter.h"
#include "azookey/host/Dispatcher.h"
#include "azookey/host/HomophoneNotesData.h"
#include "azookey/host/InferenceEngine.h"
#include "azookey/host/RequestScheduler.h"
#include "azookey/host/SettingsStore.h"
#include "azookey/ipc/Messages.h"
#include "azookey/ipc/Payloads.h"

namespace ipc = azookey::ipc;
using azookey::host::test::ScopedTempDirectory;

namespace {

// Authored rows in the section 3.2 grammar, not taken from the bundled data.
constexpr const char* kHomophoneNotesTsv =
    "# authored test data\n"
    "せいさく\t制作\t芸術作品を作ること\n"
    "せいさく\t製作\t道具を使って物を作ること\n"
    "かてい\t過程\t物事が進む道筋\n"
    "かてい\t課程\t学習の範囲と順序\n";

class HomophoneConverter final : public azookey::core::IConverter {
 public:
  std::vector<azookey::core::Candidate> Convert(const std::string& kana,
                                                const azookey::core::ConversionContext&) override {
    if (kana == "せいさく") {
      azookey::core::Candidate annotated{"製作", kana, 8.0};
      annotated.description = "rewriter note";
      return {{"制作", kana, 9.0}, annotated, {"せいさく", kana, 7.0}};
    }
    // Only one candidate of the reading is in the data.
    if (kana == "かてい") return {{"過程", kana, 9.0}, {"家庭", kana, 8.0}};
    if (kana == "かだい") return {{"課題", kana, 9.0}, {"過大", kana, 8.0}};
    return {{kana, kana, 1.0}};
  }
  std::vector<azookey::core::Candidate> PredictNext(
      const std::string& kana, const azookey::core::ConversionContext& context) override {
    return Convert(kana, context);
  }
  std::vector<azookey::core::Candidate> Correct(
      const std::string& kana, const azookey::core::CorrectionHint&,
      const azookey::core::ConversionContext& context) override {
    return Convert(kana, context);
  }
  void Commit(const azookey::core::Candidate&, const azookey::core::ConversionContext&) override {}
  void Learn(const std::string&, const std::string&) override {}
};

class HomophoneNotesDispatchTest : public ::testing::Test {
 protected:
  HomophoneNotesDispatchTest()
      : settings_path(temp.path() / "settings.json"),
        data_path(temp.path() / "homophone-notes.tsv"),
        engine(std::make_unique<HomophoneConverter>(), nullptr, {}) {}

  void WriteFile(const std::filesystem::path& path, const std::string& text) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    ASSERT_TRUE(out.is_open());
    out << text;
  }

  // Writes settings.json and publishes it through UpdateConfig, as the
  // settings app does. The first call also creates the Dispatcher.
  void ApplySettings(bool homophone_notes) {
    WriteFile(settings_path,
              std::string(R"({"homophoneNotes":)") + (homophone_notes ? "true" : "false") + "}");
    if (!dispatcher) {
      settings_store.emplace(settings_path);
      settings_store->Load();
      azookey::host::DispatcherConfig config;
      config.host_version = "0.1.0";
      config.host_generation_id = "homophone-notes-test";
      config.homophone_notes = std::make_shared<azookey::host::HomophoneNotesData>(data_path);
      dispatcher.emplace(&engine, &scheduler, nullptr, config, &*settings_store);
    }
    const auto response = dispatcher->Dispatch(Envelope(ipc::MessageType::UpdateConfig, "{}"));
    ASSERT_TRUE(response.has_value());
    const auto parsed = ipc::ParseUpdateConfigResponse(response->payload_json);
    ASSERT_TRUE(parsed.has_value());
    ASSERT_TRUE(parsed->ok);
  }

  ipc::Envelope Envelope(ipc::MessageType type, std::string payload) {
    ipc::Envelope env;
    env.version = 1;
    env.request_id = ++next_id;
    env.type = type;
    env.payload_json = std::move(payload);
    return env;
  }

  std::vector<ipc::CandidateField> Query(const std::string& reading) {
    ipc::QueryCandidatesRequest request;
    request.reading = reading;
    const auto response = dispatcher->Dispatch(
        Envelope(ipc::MessageType::QueryCandidates, ipc::BuildQueryCandidatesRequest(request)));
    EXPECT_TRUE(response.has_value());
    if (!response) return {};
    const auto parsed = ipc::ParseQueryCandidatesResponse(response->payload_json);
    EXPECT_TRUE(parsed.has_value());
    return parsed ? parsed->candidates : std::vector<ipc::CandidateField>{};
  }

  static const ipc::CandidateField* Find(const std::vector<ipc::CandidateField>& candidates,
                                         const std::string& surface) {
    for (const auto& candidate : candidates)
      if (candidate.surface == surface) return &candidate;
    return nullptr;
  }

  static std::vector<std::string> Surfaces(const std::vector<ipc::CandidateField>& candidates) {
    std::vector<std::string> surfaces;
    for (const auto& candidate : candidates) surfaces.push_back(candidate.surface);
    return surfaces;
  }

  ScopedTempDirectory temp;
  std::filesystem::path settings_path;
  std::filesystem::path data_path;
  azookey::host::InferenceEngine engine;
  azookey::host::RequestScheduler scheduler;
  std::optional<azookey::host::SettingsStore> settings_store;
  std::optional<azookey::host::Dispatcher> dispatcher;
  uint64_t next_id{0};
};

TEST_F(HomophoneNotesDispatchTest, OffLeavesDescriptionsAndDoesNotReadTheData) {
  WriteFile(data_path, kHomophoneNotesTsv);
  ApplySettings(false);
  const auto off = Query("せいさく");
  ASSERT_NE(Find(off, "制作"), nullptr);
  EXPECT_TRUE(Find(off, "制作")->description.empty());
  ASSERT_NE(Find(off, "製作"), nullptr);
  EXPECT_EQ(Find(off, "製作")->description, "rewriter note");
  for (const auto& candidate : off)
    if (candidate.surface != "製作")
      EXPECT_TRUE(candidate.description.empty()) << candidate.surface;

  // Had the query above read the data, its success would be cached and the
  // notes would survive the removal of the file.
  std::filesystem::remove(data_path);
  ApplySettings(true);
  const auto on_without_data = Query("せいさく");
  EXPECT_EQ(Surfaces(on_without_data), Surfaces(off));
  ASSERT_NE(Find(on_without_data, "制作"), nullptr);
  EXPECT_TRUE(Find(on_without_data, "制作")->description.empty());
}

TEST_F(HomophoneNotesDispatchTest, OnNotesTheListWithoutChangingOrderOrCount) {
  WriteFile(data_path, kHomophoneNotesTsv);
  ApplySettings(false);
  const auto off = Query("せいさく");
  ApplySettings(true);  // Section 6: read on the next query after false -> true.
  const auto on = Query("せいさく");
  EXPECT_EQ(Surfaces(on), Surfaces(off));
  ASSERT_EQ(on.size(), off.size());
  for (size_t i = 0; i < on.size(); ++i) {
    EXPECT_EQ(on[i].reading, off[i].reading);
    EXPECT_EQ(on[i].score, off[i].score);
    EXPECT_EQ(on[i].source, off[i].source);
  }
  ASSERT_NE(Find(on, "制作"), nullptr);
  EXPECT_EQ(Find(on, "制作")->description, "芸術作品を作ること");
  // Section 4 item 3: the rewriter's note stays, and its candidate still counts.
  EXPECT_EQ(Find(on, "製作")->description, "rewriter note");
  ASSERT_NE(Find(on, "せいさく"), nullptr);
  EXPECT_TRUE(Find(on, "せいさく")->description.empty());

  // Section 4 item 2: 過程 is the only candidate of its reading in the data.
  const auto single = Query("かてい");
  ASSERT_NE(Find(single, "過程"), nullptr);
  EXPECT_TRUE(Find(single, "過程")->description.empty());

  ApplySettings(false);
  EXPECT_TRUE(Find(Query("せいさく"), "制作")->description.empty());
}

TEST_F(HomophoneNotesDispatchTest, AMissingFileStopsOnlyTheNotes) {
  ApplySettings(true);
  const auto candidates = Query("せいさく");
  ASSERT_FALSE(candidates.empty());
  EXPECT_EQ(candidates.front().surface, "制作");
  EXPECT_TRUE(candidates.front().description.empty());
  // The failure is cached: a file that appears later is not read.
  WriteFile(data_path, kHomophoneNotesTsv);
  EXPECT_TRUE(Find(Query("せいさく"), "制作")->description.empty());
}

TEST_F(HomophoneNotesDispatchTest, RowsThatBreakTheGrammarAreDroppedAndTheRestLoad) {
  WriteFile(data_path, std::string(kHomophoneNotesTsv) + "かだい\t課題\n" +
                           "かだい\t過大\t過大\t余分\n" + "かだい\t課題\t後の行\n");
  ApplySettings(true);
  const auto candidates = Query("せいさく");
  ASSERT_NE(Find(candidates, "制作"), nullptr);
  EXPECT_EQ(Find(candidates, "制作")->description, "芸術作品を作ること");
  const auto dropped = Query("かだい");
  for (const auto& candidate : dropped) EXPECT_TRUE(candidate.description.empty());
}

TEST_F(HomophoneNotesDispatchTest, BatchConversionNotesEachSegmentList) {
  WriteFile(data_path, kHomophoneNotesTsv);
  ApplySettings(true);
  ipc::QueryBatchConversionRequest request;
  request.reading = "せいさく";
  const auto response = dispatcher->Dispatch(Envelope(
      ipc::MessageType::QueryBatchConversion, ipc::BuildQueryBatchConversionRequest(request)));
  ASSERT_TRUE(response.has_value());
  const auto parsed = ipc::ParseQueryBatchConversionResponse(response->payload_json);
  ASSERT_TRUE(parsed.has_value());
  ASSERT_EQ(parsed->segments.size(), 1u);
  const auto& segment = parsed->segments.front().candidates;
  ASSERT_NE(Find(segment, "制作"), nullptr);
  EXPECT_EQ(Find(segment, "制作")->description, "芸術作品を作ること");
  EXPECT_EQ(Find(segment, "製作")->description, "rewriter note");
  EXPECT_EQ(parsed->full_surface, segment.front().surface);
}
}  // namespace

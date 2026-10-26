#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <ctffunc.h>
#include <ctfutb.h>
#include <gtest/gtest.h>
#include <msctf.h>
#include <olectl.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "azookey/ipc/Json.h"
#include "azookey/learning/AtomicFile.h"
#include "azookey/learning/FileLock.h"
#include "azookey/tsf/LangBarInputModeItem.h"
#include "azookey/tsf/TipSettingsWriter.h"

// The input indicator button of docs/tsf-deep-integration-spec.md §4.5.

namespace {

using azookey::tsf::InputModeIndicator;
using azookey::tsf::LangBarInputModeItem;

constexpr CLSID kTestClsid = {
    0x1d2c3b4a, 0x5e6f, 0x4a7b, {0x8c, 0x9d, 0xae, 0xbf, 0xc0, 0xd1, 0xe2, 0xf3}};

class CountingSink final : public ITfLangBarItemSink {
 public:
  STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
    if (!ppv) return E_POINTER;
    *ppv = nullptr;
    if (riid != IID_IUnknown && riid != IID_ITfLangBarItemSink) return E_NOINTERFACE;
    *ppv = static_cast<ITfLangBarItemSink*>(this);
    AddRef();
    return S_OK;
  }
  STDMETHODIMP_(ULONG) AddRef() override { return static_cast<ULONG>(++refs); }
  STDMETHODIMP_(ULONG) Release() override { return static_cast<ULONG>(--refs); }
  STDMETHODIMP OnUpdate(DWORD flags) override {
    ++updates;
    last_flags = flags;
    return S_OK;
  }
  long refs{1};
  int updates{0};
  DWORD last_flags{0};
};

struct Click {
  TfLBIClick click;
  POINT point;
};

}  // namespace

TEST(InputModeIndicatorTest, ResolvesFromOpenAndAlphanumericState) {
  EXPECT_EQ(azookey::tsf::ResolveInputModeIndicator(true, false), InputModeIndicator::Hiragana);
  EXPECT_EQ(azookey::tsf::ResolveInputModeIndicator(true, true), InputModeIndicator::Alphanumeric);
  EXPECT_EQ(azookey::tsf::ResolveInputModeIndicator(false, false), InputModeIndicator::Direct);
  EXPECT_EQ(azookey::tsf::ResolveInputModeIndicator(false, true), InputModeIndicator::Direct);
}

TEST(InputModeIndicatorTest, TextAndTooltipTellEveryStateApart) {
  EXPECT_EQ(azookey::tsf::InputModeIndicatorText(InputModeIndicator::Hiragana), L"あ");
  EXPECT_EQ(azookey::tsf::InputModeIndicatorText(InputModeIndicator::Alphanumeric), L"_A");
  EXPECT_EQ(azookey::tsf::InputModeIndicatorText(InputModeIndicator::Direct), L"A");
  EXPECT_NE(azookey::tsf::InputModeIndicatorTooltip(InputModeIndicator::Hiragana),
            azookey::tsf::InputModeIndicatorTooltip(InputModeIndicator::Alphanumeric));
  EXPECT_NE(azookey::tsf::InputModeIndicatorTooltip(InputModeIndicator::Alphanumeric),
            azookey::tsf::InputModeIndicatorTooltip(InputModeIndicator::Direct));
}

TEST(InputModeIndicatorTest, GlyphIsWhiteUnlessTheTaskbarIsLight) {
  // A missing or unreadable SystemUsesLightTheme is taken as a dark taskbar.
  EXPECT_EQ(azookey::tsf::InputModeGlyphColor(std::nullopt), RGB(255, 255, 255));
  EXPECT_EQ(azookey::tsf::InputModeGlyphColor(0u), RGB(255, 255, 255));
  EXPECT_EQ(azookey::tsf::InputModeGlyphColor(1u), RGB(0, 0, 0));
}

TEST(InputModeIndicatorTest, IconHasTheRequestedSizeAndAnAlphaChannel) {
  for (const auto indicator : {InputModeIndicator::Hiragana, InputModeIndicator::Alphanumeric,
                               InputModeIndicator::Direct}) {
    for (const int size : {16, 24, 32}) {
      SCOPED_TRACE(size);
      HICON icon = azookey::tsf::CreateInputModeIcon(indicator, size, RGB(255, 255, 255));
      ASSERT_NE(icon, nullptr);
      ICONINFO info{};
      ASSERT_TRUE(GetIconInfo(icon, &info));
      BITMAP bitmap{};
      ASSERT_NE(GetObjectW(info.hbmColor, sizeof(bitmap), &bitmap), 0);
      EXPECT_EQ(bitmap.bmWidth, size);
      EXPECT_EQ(bitmap.bmHeight, size);

      BITMAPINFO request{};
      request.bmiHeader.biSize = sizeof(request.bmiHeader);
      request.bmiHeader.biWidth = size;
      request.bmiHeader.biHeight = -size;
      request.bmiHeader.biPlanes = 1;
      request.bmiHeader.biBitCount = 32;
      request.bmiHeader.biCompression = BI_RGB;
      std::vector<uint32_t> pixels(static_cast<size_t>(size) * size);
      HDC dc = GetDC(nullptr);
      ASSERT_EQ(GetDIBits(dc, info.hbmColor, 0, size, pixels.data(), &request, DIB_RGB_COLORS),
                size);
      ReleaseDC(nullptr, dc);
      size_t opaque = 0;
      for (const uint32_t pixel : pixels) {
        if ((pixel >> 24) != 0) ++opaque;
      }
      // The glyph covers part of the square, never all of it.
      EXPECT_GT(opaque, 0u);
      EXPECT_LT(opaque, pixels.size());
      // The corner stays transparent so the taskbar shows through.
      EXPECT_EQ(pixels.front() >> 24, 0u);

      DeleteObject(info.hbmColor);
      DeleteObject(info.hbmMask);
      DestroyIcon(icon);
    }
  }
  EXPECT_EQ(azookey::tsf::CreateInputModeIcon(InputModeIndicator::Direct, 0, 0), nullptr);
}

TEST(LangBarInputModeItemTest, ReportsTheInputModeItemOfTheTextService) {
  auto* item = new LangBarInputModeItem(kTestClsid, InputModeIndicator::Hiragana, nullptr);
  TF_LANGBARITEMINFO info{};
  ASSERT_EQ(item->GetInfo(&info), S_OK);
  EXPECT_TRUE(IsEqualGUID(info.guidItem, GUID_LBI_INPUTMODE));
  EXPECT_TRUE(IsEqualGUID(info.clsidService, kTestClsid));
  EXPECT_NE(info.dwStyle & TF_LBI_STYLE_BTN_BUTTON, 0u);
  EXPECT_NE(info.szDescription[0], L'\0');
  DWORD status = 1;
  ASSERT_EQ(item->GetStatus(&status), S_OK);
  EXPECT_EQ(status, 0u);
  HICON icon = nullptr;
  ASSERT_EQ(item->GetIcon(&icon), S_OK);
  EXPECT_NE(icon, nullptr);
  DestroyIcon(icon);
  BSTR text = nullptr;
  ASSERT_EQ(item->GetText(&text), S_OK);
  EXPECT_STREQ(text, L"あ");
  SysFreeString(text);
  BSTR tooltip = nullptr;
  ASSERT_EQ(item->GetTooltipString(&tooltip), S_OK);
  EXPECT_NE(SysStringLen(tooltip), 0u);
  SysFreeString(tooltip);
  EXPECT_EQ(item->Release(), 0u);
}

TEST(LangBarInputModeItemTest, NotifiesTheSinkOnlyWhenTheIndicatorChanges) {
  CountingSink sink;
  auto* item = new LangBarInputModeItem(kTestClsid, InputModeIndicator::Hiragana, nullptr);
  ITfSource* source = nullptr;
  ASSERT_EQ(item->QueryInterface(IID_ITfSource, reinterpret_cast<void**>(&source)), S_OK);
  DWORD cookie = 0;
  ASSERT_EQ(source->AdviseSink(IID_ITfLangBarItemSink, &sink, &cookie), S_OK);
  DWORD second = 0;
  EXPECT_EQ(source->AdviseSink(IID_ITfLangBarItemSink, &sink, &second), CONNECT_E_ADVISELIMIT);
  EXPECT_EQ(source->AdviseSink(IID_ITfCompartmentEventSink, &sink, &second),
            CONNECT_E_CANNOTCONNECT);

  item->SetIndicator(InputModeIndicator::Hiragana);
  EXPECT_EQ(sink.updates, 0);
  item->SetIndicator(InputModeIndicator::Direct);
  EXPECT_EQ(sink.updates, 1);
  EXPECT_EQ(sink.last_flags, static_cast<DWORD>(TF_LBI_ICON | TF_LBI_TEXT | TF_LBI_TOOLTIP));
  item->SetIndicator(InputModeIndicator::Alphanumeric);
  EXPECT_EQ(sink.updates, 2);

  EXPECT_EQ(source->UnadviseSink(cookie + 1), CONNECT_E_NOCONNECTION);
  ASSERT_EQ(source->UnadviseSink(cookie), S_OK);
  EXPECT_EQ(sink.refs, 1);
  item->SetIndicator(InputModeIndicator::Direct);
  EXPECT_EQ(sink.updates, 2);
  source->Release();
  EXPECT_EQ(item->Release(), 0u);
}

TEST(LangBarInputModeItemTest, ReleasingTheItemReleasesAnAdvisedSink) {
  CountingSink sink;
  auto* item = new LangBarInputModeItem(kTestClsid, InputModeIndicator::Direct, nullptr);
  DWORD cookie = 0;
  ASSERT_EQ(item->AdviseSink(IID_ITfLangBarItemSink, &sink, &cookie), S_OK);
  EXPECT_EQ(sink.refs, 2);
  EXPECT_EQ(item->Release(), 0u);
  EXPECT_EQ(sink.refs, 1);
}

TEST(LangBarInputModeItemTest, PassesClicksToTheOwnerUntilDetached) {
  std::vector<Click> clicks;
  auto* item = new LangBarInputModeItem(
      kTestClsid, InputModeIndicator::Hiragana,
      [&clicks](TfLBIClick click, POINT point, const RECT*) { clicks.push_back({click, point}); });
  ASSERT_EQ(item->OnClick(TF_LBI_CLK_LEFT, POINT{3, 4}, nullptr), S_OK);
  ASSERT_EQ(item->OnClick(TF_LBI_CLK_RIGHT, POINT{5, 6}, nullptr), S_OK);
  ASSERT_EQ(clicks.size(), 2u);
  EXPECT_EQ(clicks[0].click, TF_LBI_CLK_LEFT);
  EXPECT_EQ(clicks[0].point.x, 3);
  EXPECT_EQ(clicks[1].click, TF_LBI_CLK_RIGHT);
  item->Detach();
  ASSERT_EQ(item->OnClick(TF_LBI_CLK_LEFT, POINT{}, nullptr), S_OK);
  EXPECT_EQ(clicks.size(), 2u);
  EXPECT_EQ(item->Release(), 0u);
}

TEST(LangBarInputModeItemTest, ExposesOnlyTheButtonAndSourceInterfaces) {
  auto* item = new LangBarInputModeItem(kTestClsid, InputModeIndicator::Hiragana, nullptr);
  for (const IID& iid :
       {IID_IUnknown, IID_ITfLangBarItem, IID_ITfLangBarItemButton, IID_ITfSource}) {
    IUnknown* unknown = nullptr;
    ASSERT_EQ(item->QueryInterface(iid, reinterpret_cast<void**>(&unknown)), S_OK);
    unknown->Release();
  }
  IUnknown* other = reinterpret_cast<IUnknown*>(1);
  EXPECT_EQ(item->QueryInterface(IID_ITfLangBarItemBitmapButton, reinterpret_cast<void**>(&other)),
            E_NOINTERFACE);
  EXPECT_EQ(other, nullptr);
  EXPECT_EQ(item->Release(), 0u);
}

// The right-click menu (spec §4.5, DEV-1738).
namespace {

using azookey::tsf::InputModeMenuCommand;
using azookey::tsf::InputModeMenuEntry;
using azookey::tsf::InputModeMenuState;

const InputModeMenuEntry& EntryFor(const std::vector<InputModeMenuEntry>& entries,
                                   InputModeMenuCommand command) {
  for (const auto& entry : entries) {
    if (entry.command == command) return entry;
  }
  ADD_FAILURE() << "no entry " << static_cast<UINT>(command);
  return entries.front();
}

}  // namespace

TEST(InputModeMenuTest, ListsTheModesTheSwitchesAndTheSettingsAppInOrder) {
  const auto entries = azookey::tsf::BuildInputModeMenu(InputModeMenuState{});
  ASSERT_EQ(entries.size(), 6u);
  const InputModeMenuCommand order[] = {
      InputModeMenuCommand::Hiragana,   InputModeMenuCommand::Alphanumeric,
      InputModeMenuCommand::Direct,     InputModeMenuCommand::LiveConversion,
      InputModeMenuCommand::Prediction, InputModeMenuCommand::OpenSettings};
  for (size_t i = 0; i < entries.size(); ++i) EXPECT_EQ(entries[i].command, order[i]) << i;
  EXPECT_EQ(entries[0].label, L"ひらがな");
  EXPECT_EQ(entries[1].label, L"半角英数");
  EXPECT_EQ(entries[2].label, L"直接入力");
  EXPECT_EQ(entries[3].label, L"ライブ変換");
  EXPECT_EQ(entries[4].label, L"予測");
  EXPECT_EQ(entries[5].label, L"設定を開く");
  // Separators set the modes, the switches and the settings app apart.
  const bool separators[] = {false, false, false, true, false, true};
  for (size_t i = 0; i < entries.size(); ++i)
    EXPECT_EQ(entries[i].separator_before, separators[i]) << i;
  EXPECT_FALSE(entries[5].checked);
  EXPECT_FALSE(entries[5].radio);
  EXPECT_TRUE(entries[5].enabled);
}

TEST(InputModeMenuTest, ChecksExactlyTheCurrentModeAsARadioItem) {
  const std::pair<InputModeIndicator, InputModeMenuCommand> cases[] = {
      {InputModeIndicator::Hiragana, InputModeMenuCommand::Hiragana},
      {InputModeIndicator::Alphanumeric, InputModeMenuCommand::Alphanumeric},
      {InputModeIndicator::Direct, InputModeMenuCommand::Direct}};
  for (const auto& [indicator, command] : cases) {
    InputModeMenuState state;
    state.indicator = indicator;
    const auto entries = azookey::tsf::BuildInputModeMenu(state);
    int checked = 0;
    for (size_t i = 0; i < 3; ++i) {
      EXPECT_TRUE(entries[i].radio);
      EXPECT_TRUE(entries[i].enabled);
      checked += entries[i].checked ? 1 : 0;
    }
    EXPECT_EQ(checked, 1);
    EXPECT_TRUE(EntryFor(entries, command).checked);
  }
}

TEST(InputModeMenuTest, SwitchesShowTheirCurrentValue) {
  for (const bool live : {false, true}) {
    for (const bool prediction : {false, true}) {
      InputModeMenuState state;
      state.live_conversion = live;
      state.prediction = prediction;
      const auto entries = azookey::tsf::BuildInputModeMenu(state);
      const auto& live_entry = EntryFor(entries, InputModeMenuCommand::LiveConversion);
      const auto& prediction_entry = EntryFor(entries, InputModeMenuCommand::Prediction);
      EXPECT_EQ(live_entry.checked, live);
      EXPECT_EQ(prediction_entry.checked, prediction);
      EXPECT_FALSE(live_entry.radio);
      EXPECT_FALSE(prediction_entry.radio);
      EXPECT_TRUE(live_entry.enabled);
      EXPECT_TRUE(prediction_entry.enabled);
    }
  }
}

TEST(InputModeMenuTest, GraysOutOnlyTheSwitchesWhereSettingsCannotBeWritten) {
  InputModeMenuState state;
  state.live_conversion = true;
  state.settings_writable = false;
  const auto entries = azookey::tsf::BuildInputModeMenu(state);
  for (const auto& entry : entries) {
    const bool is_switch = entry.command == InputModeMenuCommand::LiveConversion ||
                           entry.command == InputModeMenuCommand::Prediction;
    EXPECT_EQ(entry.enabled, !is_switch) << static_cast<UINT>(entry.command);
  }
  // The value is still shown.
  EXPECT_TRUE(EntryFor(entries, InputModeMenuCommand::LiveConversion).checked);
}

TEST(InputModeMenuTest, TestBuildsCloseTheMenuWithoutAChoiceUnlessAPresenterIsSet) {
  const auto entries = azookey::tsf::BuildInputModeMenu(InputModeMenuState{});
  EXPECT_EQ(azookey::tsf::ShowInputModeMenu(entries, POINT{}, nullptr), InputModeMenuCommand::None);
  size_t shown = 0;
  azookey::tsf::testing::SetInputModeMenuPresenterForTest(
      [&](const std::vector<InputModeMenuEntry>& presented) {
        shown = presented.size();
        return InputModeMenuCommand::OpenSettings;
      });
  EXPECT_EQ(azookey::tsf::ShowInputModeMenu(entries, POINT{}, nullptr),
            InputModeMenuCommand::OpenSettings);
  EXPECT_EQ(shown, entries.size());
  azookey::tsf::testing::SetInputModeMenuPresenterForTest(nullptr);
}

// Writing one switch of settings.json from the menu.
namespace {

using azookey::tsf::TipSettingWriteStatus;
namespace json = azookey::ipc::json;

constexpr std::chrono::milliseconds kWriteTimeout{1000};

class TipSettingsWriterTest : public ::testing::Test {
 protected:
  void SetUp() override {
    static std::atomic<unsigned> sequence{0};
    root = std::filesystem::temp_directory_path() /
           (L"azookey-tip-settings-writer-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
            std::to_wstring(GetTickCount64()) + L"-" + std::to_wstring(sequence++));
    path = root / L"config" / L"settings.json";
  }
  void TearDown() override {
    std::error_code error;
    std::filesystem::remove_all(root, error);
  }
  void Write(const std::string& text) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    stream << text;
    stream.close();
    ASSERT_TRUE(stream.good());
  }
  std::string Read() const {
    std::ifstream stream(path, std::ios::binary);
    std::ostringstream buffer;
    buffer << stream.rdbuf();
    return buffer.str();
  }
  size_t FilesBesideSettings() const {
    size_t files = 0;
    for (const auto& entry : std::filesystem::directory_iterator(path.parent_path())) {
      (void)entry;
      ++files;
    }
    return files;
  }
  std::filesystem::path root;
  std::filesystem::path path;
};

}  // namespace

TEST_F(TipSettingsWriterTest, SetsTheKeyAndKeepsEverythingElse) {
  Write(R"({"liveConversion":false,"maxCandidates":9,"privacy":{"secureApps":["a.exe"]},)"
        R"("profilesByApp":{"code.exe":{"predictionEnabled":false}}})");
  ASSERT_EQ(azookey::tsf::WriteTipBoolSetting(path, "liveConversion", true, kWriteTimeout),
            TipSettingWriteStatus::Written);
  const std::string written = Read();
  ASSERT_FALSE(written.empty());
  EXPECT_EQ(written.back(), '\n');
  const auto parsed = json::Parse(written);
  ASSERT_TRUE(parsed && parsed->IsObject());
  EXPECT_EQ(parsed->GetBool("liveConversion"), true);
  EXPECT_EQ(parsed->GetNumber("maxCandidates"), 9.0);
  const auto* privacy = parsed->Find("privacy");
  ASSERT_TRUE(privacy && privacy->IsObject());
  ASSERT_NE(privacy->GetArray("secureApps"), nullptr);
  // A per-app profile is a different key and stays as it is.
  const auto* profiles = parsed->Find("profilesByApp");
  ASSERT_TRUE(profiles && profiles->IsObject());
  const auto* code = profiles->Find("code.exe");
  ASSERT_TRUE(code && code->IsObject());
  EXPECT_EQ(code->GetBool("predictionEnabled"), false);
  EXPECT_EQ(parsed->Find("predictionEnabled"), nullptr);
  EXPECT_EQ(FilesBesideSettings(), 1u);  // No temporary file is left.

  ASSERT_EQ(azookey::tsf::WriteTipBoolSetting(path, "predictionEnabled", false, kWriteTimeout),
            TipSettingWriteStatus::Written);
  const auto again = json::Parse(Read());
  ASSERT_TRUE(again && again->IsObject());
  EXPECT_EQ(again->GetBool("predictionEnabled"), false);
  EXPECT_EQ(again->GetBool("liveConversion"), true);
}

TEST_F(TipSettingsWriterTest, CreatesAMissingFileWithOnlyTheKey) {
  ASSERT_FALSE(std::filesystem::exists(path.parent_path()));
  ASSERT_EQ(azookey::tsf::WriteTipBoolSetting(path, "predictionEnabled", false, kWriteTimeout),
            TipSettingWriteStatus::Written);
  const auto parsed = json::Parse(Read());
  ASSERT_TRUE(parsed && parsed->IsObject());
  EXPECT_EQ(parsed->AsObject().size(), 1u);
  EXPECT_EQ(parsed->GetBool("predictionEnabled"), false);
}

TEST_F(TipSettingsWriterTest, LeavesAnInvalidFileAsItIs) {
  for (const std::string original :
       {std::string("{\"liveConversion\":tru"), std::string("[true]"), std::string()}) {
    Write(original);
    EXPECT_EQ(azookey::tsf::WriteTipBoolSetting(path, "liveConversion", true, kWriteTimeout),
              TipSettingWriteStatus::InvalidFile)
        << original;
    EXPECT_EQ(Read(), original);
    // Neither a quarantine copy nor a temporary file is left beside it.
    EXPECT_EQ(FilesBesideSettings(), 1u) << original;
  }
}

TEST_F(TipSettingsWriterTest, GivesUpWithoutWritingWhileAnotherWriterHoldsTheLock) {
  const std::string original = R"({"liveConversion":false})";
  Write(original);
  // The settings app and the Host take this lock for the same file.
  auto held = azookey::learning::AcquireExclusiveFileLockForPath(path, kWriteTimeout);
  ASSERT_TRUE(held.has_value());
  std::optional<TipSettingWriteStatus> status;
  // The lock is a mutex this thread owns, so the writer runs on another one.
  std::thread writer([&] {
    status = azookey::tsf::WriteTipBoolSetting(path, "liveConversion", true,
                                               std::chrono::milliseconds(50));
  });
  writer.join();
  ASSERT_TRUE(status.has_value());
  EXPECT_EQ(*status, TipSettingWriteStatus::LockUnavailable);
  EXPECT_EQ(Read(), original);
  held.reset();
  EXPECT_EQ(azookey::tsf::WriteTipBoolSetting(path, "liveConversion", true, kWriteTimeout),
            TipSettingWriteStatus::Written);
}

// A writer that edits another key under the same lock, the way the settings
// app saves only the keys the user changed, keeps the TIP's change, and the
// TIP keeps the other writer's.
TEST_F(TipSettingsWriterTest, ConcurrentWritersUnderTheLockKeepEachOthersKeys) {
  Write(R"({"maxCandidates":1})");
  constexpr int kRounds = 20;
  constexpr std::chrono::milliseconds kWait{10000};
  std::atomic<int> tip_failures{0};
  std::atomic<int> other_failures{0};
  std::thread tip([&] {
    for (int i = 0; i < kRounds; ++i) {
      if (azookey::tsf::WriteTipBoolSetting(path, "liveConversion", i % 2 == 0, kWait) !=
          TipSettingWriteStatus::Written)
        ++tip_failures;
    }
  });
  std::thread other([&] {
    for (int i = 0; i < kRounds; ++i) {
      auto lock = azookey::learning::AcquireExclusiveFileLockForPath(path, kWait);
      auto parsed = lock ? json::Parse(Read()) : std::nullopt;
      if (!parsed || !parsed->IsObject()) {
        ++other_failures;
        continue;
      }
      auto root_object = parsed->AsObject();
      root_object["maxCandidates"] = json::Value(i + 2);
      std::string serialized = json::Stringify(json::Value(std::move(root_object)));
      serialized.push_back('\n');
      if (!azookey::learning::WriteTextFileAtomically(path, serialized)) ++other_failures;
    }
  });
  tip.join();
  other.join();
  EXPECT_EQ(tip_failures.load(), 0);
  EXPECT_EQ(other_failures.load(), 0);
  const auto parsed = json::Parse(Read());
  ASSERT_TRUE(parsed && parsed->IsObject());
  EXPECT_EQ(parsed->GetBool("liveConversion"), (kRounds - 1) % 2 == 0);
  EXPECT_EQ(parsed->GetNumber("maxCandidates"), static_cast<double>(kRounds + 1));
}

TEST(TipSettingsWriterStatusTest, NamesEveryStatusForTheLog) {
  EXPECT_EQ(azookey::tsf::TipSettingWriteStatusName(TipSettingWriteStatus::Written), "written");
  EXPECT_EQ(azookey::tsf::TipSettingWriteStatusName(TipSettingWriteStatus::LockUnavailable),
            "lock_unavailable");
  EXPECT_EQ(azookey::tsf::TipSettingWriteStatusName(TipSettingWriteStatus::ReadFailed),
            "read_failed");
  EXPECT_EQ(azookey::tsf::TipSettingWriteStatusName(TipSettingWriteStatus::InvalidFile),
            "invalid_file");
  EXPECT_EQ(azookey::tsf::TipSettingWriteStatusName(TipSettingWriteStatus::WriteFailed),
            "write_failed");
}

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <functional>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "ScopedTempDirectory.h"
#include "azookey/host/UpdateChecker.h"
#include "azookey/host/UpdateFetchService.h"
#include "azookey/ipc/Payloads.h"

namespace {
using namespace std::chrono_literals;
using azookey::host::HttpDownloadRequest;
using azookey::host::HttpDownloadResult;
using azookey::host::HttpDownloadStatus;
using azookey::host::HttpTextRequest;
using azookey::host::HttpTextResult;
using azookey::host::IsInstallableVersion;
using azookey::host::SelectUpdatePackage;
using azookey::host::UpdateFetchCancelResult;
using azookey::host::UpdateFetchDependencies;
using azookey::host::UpdateFetchOptions;
using azookey::host::UpdateFetchService;
using azookey::host::UpdateFetchSnapshot;
using azookey::host::UpdateFetchStartResult;
using azookey::host::UpdatePackageFileName;
using azookey::host::VulkanAddonPackageFileName;
using azookey::host::test::ScopedTempDirectory;

constexpr std::string_view kDownload =
    "https://github.com/dolquis/azooKey-Desktop/releases/download/";
const std::string kSha(64, 'a');

std::string Asset(std::string_view name, std::string_view url, std::string_view digest,
                  uint64_t size = 100) {
  return R"({"name":")" + std::string(name) + R"(","browser_download_url":")" + std::string(url) +
         R"(","digest":")" + std::string(digest) + R"(","size":)" + std::to_string(size) + "}";
}

std::string BaseAsset(std::string_view version = "1.2.3", std::string_view digest = {},
                      uint64_t size = 100) {
  const auto name = UpdatePackageFileName(version);
  return Asset(name, std::string(kDownload) + "v" + std::string(version) + "/" + name,
               digest.empty() ? "sha256:" + kSha : std::string(digest), size);
}

const std::string kAddonSha(64, 'b');

std::string AddonAsset(std::string_view version = "1.2.3", std::string_view digest = {},
                       uint64_t size = 200) {
  const auto name = VulkanAddonPackageFileName(version);
  return Asset(name, std::string(kDownload) + "v" + std::string(version) + "/" + name,
               digest.empty() ? "sha256:" + kAddonSha : std::string(digest), size);
}

std::string ReleaseWith(std::string_view tag, const std::vector<std::string>& assets,
                        bool draft = false) {
  std::string list;
  for (const auto& asset : assets) list += (list.empty() ? "" : ",") + asset;
  return R"({"tag_name":")" + std::string(tag) + R"(","draft":)" + (draft ? "true" : "false") +
         R"(,"assets":[)" + list + "]}";
}

TEST(UpdatePackageTest, OnlyAPlainVersionIsInstallable) {
  EXPECT_TRUE(IsInstallableVersion("1.2.3"));
  for (const char* invalid : {"v1.2.3", "1.2.3-beta.1", "1.2", "", "1.2.3/../x", "..\\1.2.3"}) {
    EXPECT_FALSE(IsInstallableVersion(invalid)) << invalid;
  }
  EXPECT_EQ(UpdatePackageFileName("1.2.3"), "azooKey-1.2.3-x64.msi");
}

TEST(UpdatePackageTest, SelectsTheBaseMsiWithItsDigestAndSize) {
  const auto json = ReleaseWith(
      "v1.2.3", {Asset("azooKey-vulkan-addon-1.2.3-x64.msi",
                       std::string(kDownload) + "v1.2.3/azooKey-vulkan-addon-1.2.3-x64.msi",
                       "sha256:" + std::string(64, 'b')),
                 BaseAsset()});
  std::string error;
  const auto package = SelectUpdatePackage(json, "1.2.3", kDownload, 1000, &error);
  ASSERT_TRUE(package) << error;
  EXPECT_EQ(package->file_name, "azooKey-1.2.3-x64.msi");
  EXPECT_EQ(package->download_url, std::string(kDownload) + "v1.2.3/azooKey-1.2.3-x64.msi");
  EXPECT_EQ(package->sha256, kSha);
  EXPECT_EQ(package->size, 100u);
}

TEST(UpdatePackageTest, RefusesReleasesAndAssetsItCannotTrust) {
  struct Case {
    std::string json;
    const char* error;
  };
  const std::vector<Case> cases = {
      {ReleaseWith("v1.2.4", {BaseAsset()}), "not_available"},
      {ReleaseWith("v1.2.3", {BaseAsset()}, true), "not_available"},
      {"not json", "not_available"},
      {ReleaseWith("v1.2.3", {}), "asset_missing"},
      // The add-on alone, two assets of the base name, or a base asset served from elsewhere.
      {ReleaseWith("v1.2.3",
                   {Asset("azooKey-vulkan-addon-1.2.3-x64.msi",
                          std::string(kDownload) + "v1.2.3/azooKey-vulkan-addon-1.2.3-x64.msi",
                          "sha256:" + kSha)}),
       "asset_missing"},
      {ReleaseWith("v1.2.3", {BaseAsset(), BaseAsset()}), "asset_missing"},
      {ReleaseWith("v1.2.3",
                   {Asset("azooKey-1.2.3-x64.msi", "https://example.com/azooKey-1.2.3-x64.msi",
                          "sha256:" + kSha)}),
       "asset_missing"},
      {ReleaseWith("v1.2.3", {Asset("azooKey-1.2.3-x64.msi",
                                    std::string(kDownload) + "v1.2.2/azooKey-1.2.3-x64.msi",
                                    "sha256:" + kSha)}),
       "asset_missing"},
      {ReleaseWith("v1.2.3", {BaseAsset("1.2.3", "sha1:" + std::string(40, 'a'))}),
       "digest_missing"},
      {ReleaseWith("v1.2.3", {BaseAsset("1.2.3", "sha256:" + std::string(64, 'A'))}),
       "digest_missing"},
      {ReleaseWith("v1.2.3", {BaseAsset("1.2.3", "sha256:" + std::string(63, 'a'))}),
       "digest_missing"},
      {ReleaseWith("v1.2.3", {BaseAsset("1.2.3", {}, 0)}), "too_large"},
      {ReleaseWith("v1.2.3", {BaseAsset("1.2.3", {}, 1001)}), "too_large"},
  };
  for (const auto& c : cases) {
    std::string error;
    EXPECT_FALSE(SelectUpdatePackage(c.json, "1.2.3", kDownload, 1000, &error)) << c.json;
    EXPECT_EQ(error, c.error) << c.json;
  }
  // A digest without its field, too.
  std::string error;
  EXPECT_FALSE(SelectUpdatePackage(
      R"({"tag_name":"v1.2.3","assets":[{"name":"azooKey-1.2.3-x64.msi","browser_download_url":")" +
          std::string(kDownload) + R"(v1.2.3/azooKey-1.2.3-x64.msi","size":100}]})",
      "1.2.3", kDownload, 1000, &error));
  EXPECT_EQ(error, "digest_missing");
}

// Fakes for the two network operations. The download blocks until released or cancelled.
struct FakeNetwork {
  std::mutex mutex;
  std::condition_variable changed;
  std::vector<HttpTextRequest> text_requests;
  std::vector<HttpDownloadRequest> downloads;
  HttpTextResult release;
  HttpDownloadResult download_result{HttpDownloadStatus::Downloaded};
  // DEV-2081: the add-on's download (named azooKey-vulkan-addon-*) has its own result and hold.
  HttpDownloadResult addon_download_result{HttpDownloadStatus::Downloaded};
  bool hold_download{false};
  bool hold_addon_download{false};
  bool throw_addon_download{false};
  bool released{false};

  static bool IsAddon(const HttpDownloadRequest& request) {
    return request.destination.filename().string().starts_with("azooKey-vulkan-addon-");
  }
  HttpTextResult FetchText(const HttpTextRequest& request) {
    std::lock_guard lock(mutex);
    text_requests.push_back(request);
    return release;
  }
  HttpDownloadResult Download(const HttpDownloadRequest& request) {
    std::unique_lock lock(mutex);
    downloads.push_back(request);
    changed.notify_all();
    const bool addon = IsAddon(request);
    if (addon && throw_addon_download) throw std::runtime_error("add-on download threw");
    if (hold_download || (addon && hold_addon_download)) {
      // Bounded far above the time the test needs; the bound only keeps a failure from hanging.
      const auto deadline = std::chrono::steady_clock::now() + 10s;
      while (!released && !(request.cancelled && request.cancelled()) &&
             std::chrono::steady_clock::now() < deadline) {
        changed.wait_for(lock, 10ms);
      }
      if (!released) return HttpDownloadResult{};  // Failed, as a cancelled download is.
    }
    if (request.progress) request.progress(request.max_bytes, request.max_bytes);
    return addon ? addon_download_result : download_result;
  }
  bool WaitForDownload(size_t count = 1) {
    std::unique_lock lock(mutex);
    return changed.wait_for(lock, 10s, [&] { return downloads.size() >= count; });
  }
  void Release() {
    std::lock_guard lock(mutex);
    released = true;
    changed.notify_all();
  }
};

struct Fixture {
  ScopedTempDirectory temp;
  FakeNetwork network;
  std::atomic<bool> allowed{true};
  std::string channel{"stable"};
  bool addon_installed{false};

  std::filesystem::path StatusPath() const { return temp.path() / "update-status.json"; }
  std::filesystem::path UpdatesDir() const { return temp.path() / "updates"; }

  void WriteStatus(std::string_view result, std::string_view latest, std::string_view current,
                   std::string_view status_channel = "stable") {
    std::ofstream output(StatusPath(), std::ios::binary | std::ios::trunc);
    output << R"({"version":1,"checkedAtEpoch":1,"channel":")" << status_channel
           << R"(","result":")" << result << R"(","currentVersion":")" << current
           << R"(","latestVersion":")" << latest << R"("})";
  }

  std::unique_ptr<UpdateFetchService> Make() {
    UpdateFetchOptions options;
    options.current_version = "1.0.0";
    options.api_base_url = L"https://api.example.test/repos/owner/repo";
    options.status_path = StatusPath();
    options.updates_dir = UpdatesDir();
    options.max_bytes = 1000;
    UpdateFetchDependencies dependencies;
    dependencies.fetch_text = [this](const HttpTextRequest& r) { return network.FetchText(r); };
    dependencies.download = [this](const HttpDownloadRequest& r) { return network.Download(r); };
    dependencies.allowed = [this] { return allowed.load(); };
    dependencies.channel = [this] { return channel; };
    dependencies.vulkan_addon_installed = [this] { return addon_installed; };
    return std::make_unique<UpdateFetchService>(std::move(options), std::move(dependencies));
  }
};

// Polls the observable state, bounded at 10 seconds.
UpdateFetchSnapshot WaitForEnd(const UpdateFetchService& service) {
  const auto deadline = std::chrono::steady_clock::now() + 10s;
  auto snapshot = service.Snapshot();
  while (snapshot.state == UpdateFetchSnapshot::State::Running &&
         std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(5ms);
    snapshot = service.Snapshot();
  }
  return snapshot;
}

TEST(UpdateFetchServiceTest, DownloadsTheRecordedUpdateAgainstItsDigest) {
  Fixture f;
  f.WriteStatus("update_available", "1.2.3", "1.0.0");
  f.network.release.body = ReleaseWith("v1.2.3", {BaseAsset()});
  // An older package and its part are removed once the new one is verified; other files stay.
  std::filesystem::create_directories(f.UpdatesDir());
  // Built rather than listed as literals: a list of "azooKey-..." strings reads as an API key
  // assignment to the secret scanner.
  const auto older = UpdatePackageFileName("1.2.2");
  const auto older_addon = VulkanAddonPackageFileName("1.2.2");
  const auto current_addon_part = VulkanAddonPackageFileName("1.2.3") + ".part";
  const std::vector<std::string> names = {older,       older + ".part",       "notes.txt",
                                          older_addon, older_addon + ".part", current_addon_part};
  for (const auto& name : names) {
    std::ofstream(f.UpdatesDir() / name) << "x";
  }
  std::ofstream(f.UpdatesDir() / "azooKey-latest-x64.msi") << "x";
  auto service = f.Make();
  UpdateFetchSnapshot started;
  ASSERT_EQ(service->Start("1.2.3", &started), UpdateFetchStartResult::Started);
  EXPECT_EQ(started.state, UpdateFetchSnapshot::State::Running);
  const auto end = WaitForEnd(*service);
  ASSERT_EQ(end.state, UpdateFetchSnapshot::State::Ready) << end.error;
  EXPECT_EQ(end.version, "1.2.3");
  EXPECT_EQ(end.sha256, kSha);
  EXPECT_EQ(end.received_bytes, 100u);
  EXPECT_EQ(end.total_bytes, 100u);

  ASSERT_EQ(f.network.text_requests.size(), 1u);
  EXPECT_EQ(f.network.text_requests[0].url,
            L"https://api.example.test/repos/owner/repo/releases/tags/v1.2.3");
  EXPECT_GT(f.network.text_requests[0].max_bytes, 0u);
  ASSERT_TRUE(f.network.text_requests[0].cancelled);
  EXPECT_FALSE(f.network.text_requests[0].cancelled());
  ASSERT_TRUE(f.network.text_requests[0].network_allowed);
  EXPECT_TRUE(f.network.text_requests[0].network_allowed());
  ASSERT_EQ(f.network.downloads.size(), 1u);
  const auto& download = f.network.downloads[0];
  EXPECT_EQ(download.url,
            L"https://github.com/dolquis/azooKey-Desktop/releases/download/v1.2.3/"
            L"azooKey-1.2.3-x64.msi");
  EXPECT_EQ(download.destination, f.UpdatesDir() / "azooKey-1.2.3-x64.msi");
  EXPECT_EQ(download.expected_sha256, kSha);
  EXPECT_EQ(download.max_bytes, 100u);
  ASSERT_TRUE(download.network_allowed);
  EXPECT_TRUE(download.network_allowed());
  EXPECT_FALSE(std::filesystem::exists(f.UpdatesDir() / "azooKey-1.2.2-x64.msi"));
  EXPECT_FALSE(std::filesystem::exists(f.UpdatesDir() / "azooKey-1.2.2-x64.msi.part"));
  // DEV-2081: the add-on of an older version goes too; that of this version stays for a resume.
  EXPECT_FALSE(std::filesystem::exists(f.UpdatesDir() / older_addon));
  EXPECT_FALSE(std::filesystem::exists(f.UpdatesDir() / (older_addon + ".part")));
  EXPECT_TRUE(std::filesystem::exists(f.UpdatesDir() / current_addon_part));
  // Only this service's own files go: other names stay.
  EXPECT_TRUE(std::filesystem::exists(f.UpdatesDir() / "notes.txt"));
  EXPECT_TRUE(std::filesystem::exists(f.UpdatesDir() / "azooKey-latest-x64.msi"));

  azookey::ipc::UpdateFetchResponse response;
  azookey::host::FillUpdateFetchResponse(end, response);
  EXPECT_EQ(response.state, "ready");
  EXPECT_EQ(response.sha256, kSha);
  EXPECT_TRUE(response.result_error.empty());
  // Without the add-on installed, nothing about it is answered.
  EXPECT_TRUE(response.addon_sha256.empty());
  EXPECT_TRUE(response.addon_error.empty());
}

TEST(UpdateFetchServiceTest, StartsOnlyForTheRecordedAvailableVersion) {
  Fixture f;
  f.network.release.body = ReleaseWith("v1.2.3", {BaseAsset()});
  auto service = f.Make();
  EXPECT_EQ(service->Start("1.2.3"), UpdateFetchStartResult::NotAvailable);  // No record.
  struct Case {
    const char* result;
    const char* latest;
    const char* current;
    const char* channel;
  };
  for (const auto& c : {Case{"up_to_date", "1.2.3", "1.0.0", "stable"},
                        Case{"update_available", "1.2.4", "1.0.0", "stable"},
                        Case{"update_available", "1.2.3", "0.9.0", "stable"},
                        Case{"update_available", "1.2.3", "1.0.0", "beta"}}) {
    f.WriteStatus(c.result, c.latest, c.current, c.channel);
    EXPECT_EQ(service->Start("1.2.3"), UpdateFetchStartResult::NotAvailable)
        << c.result << " " << c.latest << " " << c.current << " " << c.channel;
  }
  f.WriteStatus("update_available", "1.2.3", "1.0.0");
  for (const char* invalid : {"v1.2.3", "1.2.3-beta.1", "../1.2.3", ""}) {
    EXPECT_EQ(service->Start(invalid), UpdateFetchStartResult::NotAvailable) << invalid;
  }
  f.allowed = false;
  EXPECT_EQ(service->Start("1.2.3"), UpdateFetchStartResult::Disabled);
  EXPECT_TRUE(f.network.text_requests.empty());
  EXPECT_TRUE(f.network.downloads.empty());
  EXPECT_EQ(service->Snapshot().state, UpdateFetchSnapshot::State::Idle);
}

TEST(UpdateFetchServiceTest, FailuresEndWithAFixedCategory) {
  struct Case {
    std::optional<std::string> fetch_error;
    std::string body;
    bool download_fails;
    const char* error;
  };
  const std::vector<Case> cases = {
      {std::string("receiving HTTP response failed"), "", false, "network"},
      {std::nullopt, ReleaseWith("v1.2.3", {BaseAsset("1.2.3", "md5:x")}), false, "digest_missing"},
      {std::nullopt, ReleaseWith("v1.2.3", {BaseAsset()}), true, "download_failed"},
  };
  for (const auto& c : cases) {
    Fixture f;
    f.WriteStatus("update_available", "1.2.3", "1.0.0");
    f.network.release.error = c.fetch_error;
    f.network.release.body = c.body;
    if (c.download_fails) f.network.download_result = HttpDownloadResult{};
    auto service = f.Make();
    ASSERT_EQ(service->Start("1.2.3"), UpdateFetchStartResult::Started);
    const auto end = WaitForEnd(*service);
    EXPECT_EQ(end.state, UpdateFetchSnapshot::State::Failed) << c.error;
    EXPECT_EQ(end.error, c.error);
    EXPECT_TRUE(end.sha256.empty());
    azookey::ipc::UpdateFetchResponse response;
    azookey::host::FillUpdateFetchResponse(end, response);
    EXPECT_EQ(response.state, "failed");
    EXPECT_EQ(response.result_error, c.error);
    EXPECT_TRUE(response.sha256.empty());
  }
}

TEST(UpdateFetchServiceTest, ARunningDownloadIsBusyAndStopsOnCancelOrWhenDisallowed) {
  for (const bool by_settings : {false, true}) {
    Fixture f;
    f.WriteStatus("update_available", "1.2.3", "1.0.0");
    f.network.release.body = ReleaseWith("v1.2.3", {BaseAsset()});
    f.network.hold_download = true;
    auto service = f.Make();
    ASSERT_EQ(service->Start("1.2.3"), UpdateFetchStartResult::Started);
    ASSERT_TRUE(f.network.WaitForDownload());
    EXPECT_EQ(service->Start("1.2.3"), UpdateFetchStartResult::Busy);
    EXPECT_EQ(service->Cancel("1.2.4"), UpdateFetchCancelResult::NotRunning);
    if (by_settings) {
      f.allowed = false;  // A reload turned autoUpdate off, or the privacy mode forbids it.
    } else {
      EXPECT_EQ(service->Cancel("1.2.3"), UpdateFetchCancelResult::Cancelling);
    }
    const auto end = WaitForEnd(*service);
    EXPECT_EQ(end.state, UpdateFetchSnapshot::State::Failed);
    EXPECT_EQ(end.error, by_settings ? "disabled" : "cancelled");
    EXPECT_EQ(service->Cancel("1.2.3"), UpdateFetchCancelResult::NotRunning);
  }
}

TEST(UpdateFetchServiceTest, ABusyStartReportsTheRunningDownloadAndAFinishedOneCanRestart) {
  Fixture f;
  f.WriteStatus("update_available", "1.2.3", "1.0.0");
  f.network.release.body = ReleaseWith("v1.2.3", {BaseAsset()});
  f.network.hold_download = true;
  auto service = f.Make();
  ASSERT_EQ(service->Start("1.2.3"), UpdateFetchStartResult::Started);
  ASSERT_TRUE(f.network.WaitForDownload());
  // The settings app follows a download another window started by this state.
  UpdateFetchSnapshot busy;
  EXPECT_EQ(service->Start("1.2.3", &busy), UpdateFetchStartResult::Busy);
  EXPECT_EQ(busy.state, UpdateFetchSnapshot::State::Running);
  EXPECT_EQ(busy.version, "1.2.3");
  f.network.Release();
  ASSERT_EQ(WaitForEnd(*service).state, UpdateFetchSnapshot::State::Ready);
  // The finished worker is joined when the next download starts.
  UpdateFetchSnapshot again;
  ASSERT_EQ(service->Start("1.2.3", &again), UpdateFetchStartResult::Started);
  EXPECT_EQ(again.state, UpdateFetchSnapshot::State::Running);
  EXPECT_TRUE(again.sha256.empty());
  const auto end = WaitForEnd(*service);
  EXPECT_EQ(end.state, UpdateFetchSnapshot::State::Ready);
  EXPECT_EQ(f.network.downloads.size(), 2u);
}

TEST(UpdateFetchServiceTest, ShutdownStopsTheDownloadAndRefusesLaterStarts) {
  Fixture f;
  f.WriteStatus("update_available", "1.2.3", "1.0.0");
  f.network.release.body = ReleaseWith("v1.2.3", {BaseAsset()});
  f.network.hold_download = true;
  auto service = f.Make();
  ASSERT_EQ(service->Start("1.2.3"), UpdateFetchStartResult::Started);
  ASSERT_TRUE(f.network.WaitForDownload());
  service->Shutdown();
  EXPECT_EQ(service->Snapshot().error, "cancelled");
  EXPECT_EQ(service->Start("1.2.3"), UpdateFetchStartResult::Unavailable);
}

// DEV-2081: the add-on MSI is chosen by its own exact name, by the same rules as the base.
TEST(UpdatePackageTest, SelectsTheVulkanAddonMsiByItsOwnName) {
  std::string error;
  const auto addon = azookey::host::SelectVulkanAddonPackage(
      ReleaseWith("v1.2.3", {BaseAsset(), AddonAsset()}), "1.2.3", kDownload, 1000, &error);
  ASSERT_TRUE(addon) << error;
  EXPECT_EQ(addon->file_name, VulkanAddonPackageFileName("1.2.3"));
  EXPECT_EQ(addon->download_url,
            std::string(kDownload) + "v1.2.3/" + VulkanAddonPackageFileName("1.2.3"));
  EXPECT_EQ(addon->sha256, kAddonSha);
  EXPECT_EQ(addon->size, 200u);
  // The base alone never stands in for the add-on, and the add-on's digest is checked.
  EXPECT_FALSE(azookey::host::SelectVulkanAddonPackage(ReleaseWith("v1.2.3", {BaseAsset()}),
                                                       "1.2.3", kDownload, 1000, &error));
  EXPECT_EQ(error, "asset_missing");
  EXPECT_FALSE(azookey::host::SelectVulkanAddonPackage(
      ReleaseWith("v1.2.3", {BaseAsset(), AddonAsset("1.2.3", "sha256:x")}), "1.2.3", kDownload,
      1000, &error));
  EXPECT_EQ(error, "digest_missing");
}

TEST(UpdateFetchServiceTest, TheInstalledAddonFollowsTheBaseInTheSameDownload) {
  Fixture f;
  f.addon_installed = true;
  f.WriteStatus("update_available", "1.2.3", "1.0.0");
  f.network.release.body = ReleaseWith("v1.2.3", {BaseAsset(), AddonAsset()});
  auto service = f.Make();
  ASSERT_EQ(service->Start("1.2.3"), UpdateFetchStartResult::Started);
  const auto end = WaitForEnd(*service);
  ASSERT_EQ(end.state, UpdateFetchSnapshot::State::Ready) << end.error;
  EXPECT_EQ(end.sha256, kSha);
  EXPECT_EQ(end.addon_sha256, kAddonSha);
  EXPECT_TRUE(end.addon_error.empty());
  // The progress counts both files.
  EXPECT_EQ(end.total_bytes, 300u);
  EXPECT_EQ(end.received_bytes, 300u);
  // One API request; the base first, then the add-on, each against its own digest and size.
  EXPECT_EQ(f.network.text_requests.size(), 1u);
  ASSERT_EQ(f.network.downloads.size(), 2u);
  EXPECT_EQ(f.network.downloads[0].destination, f.UpdatesDir() / UpdatePackageFileName("1.2.3"));
  const auto& addon = f.network.downloads[1];
  EXPECT_EQ(addon.url,
            L"https://github.com/dolquis/azooKey-Desktop/releases/download/v1.2.3/"
            L"azooKey-vulkan-addon-1.2.3-x64.msi");
  EXPECT_EQ(addon.destination, f.UpdatesDir() / VulkanAddonPackageFileName("1.2.3"));
  EXPECT_EQ(addon.expected_sha256, kAddonSha);
  EXPECT_EQ(addon.max_bytes, 200u);
  ASSERT_TRUE(addon.network_allowed);
  ASSERT_TRUE(addon.cancelled);

  azookey::ipc::UpdateFetchResponse response;
  azookey::host::FillUpdateFetchResponse(end, response);
  EXPECT_EQ(response.sha256, kSha);
  EXPECT_EQ(response.addon_sha256, kAddonSha);
  EXPECT_TRUE(response.addon_error.empty());
}

TEST(UpdateFetchServiceTest, AnAddonNotInstalledIsNotDownloaded) {
  Fixture f;
  f.WriteStatus("update_available", "1.2.3", "1.0.0");
  f.network.release.body = ReleaseWith("v1.2.3", {BaseAsset(), AddonAsset()});
  auto service = f.Make();
  ASSERT_EQ(service->Start("1.2.3"), UpdateFetchStartResult::Started);
  const auto end = WaitForEnd(*service);
  ASSERT_EQ(end.state, UpdateFetchSnapshot::State::Ready) << end.error;
  EXPECT_EQ(f.network.downloads.size(), 1u);
  EXPECT_EQ(end.total_bytes, 100u);
  EXPECT_TRUE(end.addon_sha256.empty());
  EXPECT_TRUE(end.addon_error.empty());
}

TEST(UpdateFetchServiceTest, AnAddonFailureLeavesTheBaseReadyWithItsCategory) {
  struct Case {
    std::vector<std::string> assets;
    bool download_fails;
    const char* error;
  };
  const std::vector<Case> cases = {
      // A release the add-on job did not attach to.
      {{BaseAsset()}, false, "asset_missing"},
      {{BaseAsset(), AddonAsset("1.2.3", "md5:x")}, false, "digest_missing"},
      {{BaseAsset(), AddonAsset("1.2.3", {}, 1001)}, false, "too_large"},
      {{BaseAsset(), AddonAsset()}, true, "download_failed"},
  };
  for (const auto& c : cases) {
    Fixture f;
    f.addon_installed = true;
    f.WriteStatus("update_available", "1.2.3", "1.0.0");
    f.network.release.body = ReleaseWith("v1.2.3", c.assets);
    if (c.download_fails) f.network.addon_download_result = HttpDownloadResult{};
    auto service = f.Make();
    ASSERT_EQ(service->Start("1.2.3"), UpdateFetchStartResult::Started);
    const auto end = WaitForEnd(*service);
    ASSERT_EQ(end.state, UpdateFetchSnapshot::State::Ready) << c.error;
    EXPECT_EQ(end.sha256, kSha) << c.error;
    EXPECT_TRUE(end.addon_sha256.empty()) << c.error;
    EXPECT_EQ(end.addon_error, c.error);
    azookey::ipc::UpdateFetchResponse response;
    azookey::host::FillUpdateFetchResponse(end, response);
    EXPECT_EQ(response.state, "ready");
    EXPECT_EQ(response.addon_error, c.error);
    EXPECT_TRUE(response.result_error.empty());
  }
}

// An exception while the add-on downloads is the add-on's failure: the verified base stays ready.
TEST(UpdateFetchServiceTest, AnExceptionDuringTheAddonLeavesTheBaseReady) {
  Fixture f;
  f.addon_installed = true;
  f.WriteStatus("update_available", "1.2.3", "1.0.0");
  f.network.release.body = ReleaseWith("v1.2.3", {BaseAsset(), AddonAsset()});
  f.network.throw_addon_download = true;
  auto service = f.Make();
  ASSERT_EQ(service->Start("1.2.3"), UpdateFetchStartResult::Started);
  const auto end = WaitForEnd(*service);
  ASSERT_EQ(end.state, UpdateFetchSnapshot::State::Ready) << end.error;
  EXPECT_EQ(end.sha256, kSha);
  EXPECT_TRUE(end.addon_sha256.empty());
  EXPECT_EQ(end.addon_error, "download_failed");
}

TEST(UpdateFetchServiceTest, AStopDuringTheAddonStopsTheWholeDownload) {
  for (const bool by_settings : {false, true}) {
    Fixture f;
    f.addon_installed = true;
    f.WriteStatus("update_available", "1.2.3", "1.0.0");
    f.network.release.body = ReleaseWith("v1.2.3", {BaseAsset(), AddonAsset()});
    f.network.hold_addon_download = true;
    auto service = f.Make();
    ASSERT_EQ(service->Start("1.2.3"), UpdateFetchStartResult::Started);
    ASSERT_TRUE(f.network.WaitForDownload(2));
    if (by_settings) {
      f.allowed = false;
    } else {
      EXPECT_EQ(service->Cancel("1.2.3"), UpdateFetchCancelResult::Cancelling);
    }
    const auto end = WaitForEnd(*service);
    EXPECT_EQ(end.state, UpdateFetchSnapshot::State::Failed);
    EXPECT_EQ(end.error, by_settings ? "disabled" : "cancelled");
    azookey::ipc::UpdateFetchResponse response;
    azookey::host::FillUpdateFetchResponse(end, response);
    EXPECT_TRUE(response.sha256.empty());
    EXPECT_TRUE(response.addon_sha256.empty());
    EXPECT_TRUE(response.addon_error.empty());
  }
}

}  // namespace

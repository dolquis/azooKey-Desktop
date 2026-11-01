#include "ModelPaneModel.h"

#include <algorithm>
#include <cwctype>
#include <filesystem>
#include <iterator>
#include <string>

namespace azookey::settings {
namespace {

std::wstring NormalizedWide(std::string_view utf8) {
  std::u8string text;
  text.reserve(utf8.size());
  for (const char ch : utf8) text.push_back(static_cast<char8_t>(ch));
  std::wstring wide = std::filesystem::path(text).lexically_normal().wstring();
  std::transform(wide.begin(), wide.end(), wide.begin(),
                 [](wchar_t ch) { return static_cast<wchar_t>(std::towlower(ch)); });
  return wide;
}

}  // namespace

ModelEntryState ClassifyModel(const azookey::ipc::ListedModel& model) {
  if (!model.valid) return ModelEntryState::Invalid;
  if (model.last_load_status == "failed") return ModelEntryState::LoadFailed;
  if (model.last_load_status == "success") return ModelEntryState::Loaded;
  return ModelEntryState::NotLoaded;
}

bool IsSelectableModel(const azookey::ipc::ListedModel& model) {
  return model.valid && model.format == "gguf";
}

bool SameModelPath(std::string_view left, std::string_view right) {
  if (left.empty() || right.empty()) return false;
  try {
    return NormalizedWide(left) == NormalizedWide(right);
  } catch (...) {
    return left == right;
  }
}

std::string FormatByteSize(uint64_t bytes) {
  if (bytes < 1024) return std::to_string(bytes) + " B";
  static constexpr const char* kUnits[] = {"KB", "MB", "GB", "TB"};
  double value = static_cast<double>(bytes) / 1024.0;
  size_t unit = 0;
  while (value >= 1024.0 && unit + 1 < std::size(kUnits)) {
    value /= 1024.0;
    ++unit;
  }
  const auto rounded = static_cast<uint64_t>(value * 10.0 + 0.5);
  return std::to_string(rounded / 10) + "." + std::to_string(rounded % 10) + " " + kUnits[unit];
}

std::string ModelErrorResource(std::string_view error) {
  const bool known =
      std::find(kModelErrorCodes.begin(), kModelErrorCodes.end(), error) != kModelErrorCodes.end();
  if (!known) return "ModelsError_other";
  std::string name = "ModelsError_";
  for (const char ch : error) name.push_back(ch == ' ' ? '_' : ch);
  return name;
}

std::string HostCallStatusResource(HostCallStatus status) {
  switch (status) {
    case HostCallStatus::Ok:
      break;
    case HostCallStatus::Unavailable:
      return "HostCall_Unavailable";
    case HostCallStatus::HostNotRunning:
      return "HostCall_HostNotRunning";
    case HostCallStatus::HandshakeRejected:
      return "HostCall_HandshakeRejected";
    case HostCallStatus::Unsupported:
      return "HostCall_Unsupported";
    case HostCallStatus::Timeout:
      return "HostCall_Timeout";
    case HostCallStatus::InvalidResponse:
      return "HostCall_InvalidResponse";
  }
  return "HostCall_Failed";
}

std::string CatalogFetchErrorResource(std::string_view error) {
  if (error.starts_with("format:")) error = "format";
  const bool known = std::find(kCatalogFetchErrorCodes.begin(), kCatalogFetchErrorCodes.end(),
                               error) != kCatalogFetchErrorCodes.end();
  if (!known) return "CatalogFetchError_other";
  std::string name = "CatalogFetchError_";
  for (const char ch : error) name.push_back(ch == ' ' ? '_' : ch);
  return name;
}

std::string CatalogFetchResultResource(const azookey::ipc::CatalogFetchResponse& response) {
  if (response.status == "downloaded") return "Models_Catalog_Result_downloaded";
  if (response.status == "already_valid") return "Models_Catalog_Result_already_valid";
  if (response.status == "offline") return "Models_Catalog_Result_offline";
  if (response.status == "not_fetchable") return "Models_Catalog_Result_not_fetchable";
  return CatalogFetchErrorResource(response.result_error);
}

bool CatalogFetchSucceeded(const azookey::ipc::CatalogFetchResponse& response) {
  return response.state == "done" && !response.path.empty() &&
         (response.status == "downloaded" || response.status == "already_valid");
}

std::string CatalogEntryStateResource(std::string_view state) {
  if (state == "verified") return "Models_Catalog_State_verified";
  if (state == "mismatch") return "Models_Catalog_State_mismatch";
  return "Models_Catalog_State_missing";
}

uint32_t ProgressPercent(uint64_t received, uint64_t total) {
  if (total == 0) return 0;
  if (received >= total) return 100;
  // received < total, so the product of received and 100 is computed without overflow by
  // dividing first when needed.
  const uint64_t percent =
      received > UINT64_MAX / 100 ? received / (total / 100) : received * 100 / total;
  return static_cast<uint32_t>(std::min<uint64_t>(percent, 99));
}

bool CatalogFetchStallWatch::Stalled(uint64_t received, Clock::time_point now) {
  if (received != received_) {
    received_ = received;
    last_change_ = now;
    return false;
  }
  return now - last_change_ > limit_;
}

}  // namespace azookey::settings

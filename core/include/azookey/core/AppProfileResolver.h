#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "azookey/ipc/Json.h"

namespace azookey::core {
struct ForegroundApp {
  std::string process_name;
  std::string window_class;
  bool resolved{false};
};
using AppNameEqual = bool (*)(std::string_view, std::string_view);
bool EqualAsciiAppName(std::string_view left, std::string_view right);
bool EqualAppName(std::string_view left, std::string_view right);

// Shared validation for runtime loading and settings-app writeback. Invalid
// fields are omitted (inherit); diagnostics never contain names or values.
ipc::json::Object SanitizeAppProfiles(const ipc::json::Object& profiles,
                                      std::vector<std::string>* warnings = nullptr);

// Immutable, I/O-free profile selection. Resolved fields are configuration,
// not authorization: consumers must enforce PrivacyGate before acting on them.
class AppProfileResolver {
 public:
  static AppProfileResolver FromSettings(const ipc::json::Value& settings,
                                         std::vector<std::string>* warnings = nullptr);
  ipc::json::Object Resolve(const ForegroundApp& app, AppNameEqual equal = EqualAppName) const;
  std::optional<ipc::json::Value> ResolveField(std::string_view field, const ForegroundApp& app,
                                               AppNameEqual equal = EqualAppName) const;

 private:
  ipc::json::Object ResolveImpl(const ForegroundApp& app, AppNameEqual equal,
                                std::optional<std::string_view> field) const;
  ipc::json::Object profiles_;
  ipc::json::Object legacy_;
  ipc::json::Object globals_;
};
}  // namespace azookey::core

#pragma once
#include <HalStorage.h>

#include <string>

// Plugin folders may live under any of these SD roots; earlier roots win on
// name collisions. /.crosspoint/plugins is the historical home; /plugins and
// /.plugins are friendlier for users copying folders onto the card from a
// computer.
namespace PluginLocations {
inline constexpr const char* kRoots[] = {"/.crosspoint/plugins", "/plugins", "/.plugins"};
inline constexpr size_t kRootCount = sizeof(kRoots) / sizeof(kRoots[0]);

// Directory of the named plugin ("<root>/<name>"), or "" when absent.
inline std::string findPluginDir(const char* name) {
  for (size_t i = 0; i < kRootCount; i++) {
    std::string dir = std::string(kRoots[i]) + "/" + name;
    if (Storage.exists(dir.c_str())) return dir;
  }
  return {};
}
}  // namespace PluginLocations

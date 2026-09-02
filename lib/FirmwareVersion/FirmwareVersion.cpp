#include "FirmwareVersion.h"

#include <cstdint>
#include <limits>

namespace firmware_version {
namespace {

struct ParsedVersion {
  uint32_t major = 0;
  uint32_t minor = 0;
  uint32_t patch = 0;
  bool prerelease = false;
  bool valid = false;
};

bool parseComponent(const char*& cursor, uint32_t& value) {
  if (*cursor < '0' || *cursor > '9') return false;

  value = 0;
  do {
    const uint32_t digit = static_cast<uint32_t>(*cursor - '0');
    if (value > (std::numeric_limits<uint32_t>::max() - digit) / 10) return false;
    value = value * 10 + digit;
    ++cursor;
  } while (*cursor >= '0' && *cursor <= '9');
  return true;
}

ParsedVersion parse(const char* text) {
  ParsedVersion version;
  if (!text) return version;

  const char* cursor = text;
  if (*cursor == 'v' || *cursor == 'V') ++cursor;

  if (!parseComponent(cursor, version.major) || *cursor++ != '.' || !parseComponent(cursor, version.minor) ||
      *cursor++ != '.' || !parseComponent(cursor, version.patch)) {
    return version;
  }

  // Build metadata does not make a release a prerelease. Every other suffix
  // ("-rc.1", "rc", "-dev", etc.) does.
  version.prerelease = *cursor != '\0' && *cursor != '+';
  version.valid = true;
  return version;
}

}  // namespace

bool isNewer(const char* current, const char* candidate) {
  const ParsedVersion currentVersion = parse(current);
  const ParsedVersion candidateVersion = parse(candidate);
  if (!currentVersion.valid || !candidateVersion.valid) return false;

  if (candidateVersion.major != currentVersion.major) return candidateVersion.major > currentVersion.major;
  if (candidateVersion.minor != currentVersion.minor) return candidateVersion.minor > currentVersion.minor;
  if (candidateVersion.patch != currentVersion.patch) return candidateVersion.patch > currentVersion.patch;

  return currentVersion.prerelease && !candidateVersion.prerelease;
}

}  // namespace firmware_version

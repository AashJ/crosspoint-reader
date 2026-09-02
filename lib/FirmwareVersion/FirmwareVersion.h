#pragma once

namespace firmware_version {

// Compares semantic-version cores without allocating. An optional leading
// 'v' is accepted, and a stable release supersedes a prerelease with the same
// major/minor/patch values.
bool isNewer(const char* current, const char* candidate);

}  // namespace firmware_version

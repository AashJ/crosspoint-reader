#include <gtest/gtest.h>

#include "FirmwareVersion.h"

TEST(FirmwareVersion, AcceptsGithubVPrefix) { EXPECT_TRUE(firmware_version::isNewer("1.6.0", "v1.6.1")); }

TEST(FirmwareVersion, ComparesMajorMinorAndPatch) {
  EXPECT_TRUE(firmware_version::isNewer("1.9.9", "2.0.0"));
  EXPECT_TRUE(firmware_version::isNewer("1.6.9", "1.7.0"));
  EXPECT_TRUE(firmware_version::isNewer("1.6.0", "1.6.1"));
}

TEST(FirmwareVersion, RejectsOlderOrEqualVersions) {
  EXPECT_FALSE(firmware_version::isNewer("2.0.0", "1.9.9"));
  EXPECT_FALSE(firmware_version::isNewer("1.6.0", "v1.6.0"));
}

TEST(FirmwareVersion, StableReleaseSupersedesPrerelease) {
  EXPECT_TRUE(firmware_version::isNewer("1.6.0-rc.1", "v1.6.0"));
  EXPECT_TRUE(firmware_version::isNewer("1.6.0rc", "1.6.0"));
}

TEST(FirmwareVersion, PrereleaseDoesNotSupersedeStable) {
  EXPECT_FALSE(firmware_version::isNewer("1.6.0", "v1.6.0-rc.2"));
}

TEST(FirmwareVersion, BuildMetadataDoesNotChangePrecedence) {
  EXPECT_FALSE(firmware_version::isNewer("1.6.0+abc", "v1.6.0+def"));
}

TEST(FirmwareVersion, RejectsInvalidOrOverflowingVersions) {
  EXPECT_FALSE(firmware_version::isNewer("not-a-version", "1.6.1"));
  EXPECT_FALSE(firmware_version::isNewer("1.6.0", "latest"));
  EXPECT_FALSE(firmware_version::isNewer("1.6.0", "42949672960.0.0"));
}

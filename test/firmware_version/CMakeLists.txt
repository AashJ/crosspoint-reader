add_executable(FirmwareVersionTest
  FirmwareVersionTest.cpp
  ${REPO_ROOT}/lib/FirmwareVersion/FirmwareVersion.cpp
)

target_include_directories(FirmwareVersionTest PRIVATE
  ${REPO_ROOT}/lib/FirmwareVersion
)

target_link_libraries(FirmwareVersionTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(FirmwareVersionTest)

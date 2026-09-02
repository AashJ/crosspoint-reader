add_executable(LibraryHomeScreenTest
  LibraryHomeScreenTest.cpp
  ${REPO_ROOT}/freeink-sdk/libs/ui/FreeInkUI/src/FreeInkUI.cpp
)

target_include_directories(LibraryHomeScreenTest PRIVATE
  ${REPO_ROOT}/src
  ${REPO_ROOT}/freeink-sdk/libs/ui/FreeInkUI/include
)

target_link_libraries(LibraryHomeScreenTest PRIVATE
  crosspoint_test_common
)

add_test(NAME LibraryHomeScreenTest COMMAND LibraryHomeScreenTest)

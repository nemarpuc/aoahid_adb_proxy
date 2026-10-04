cmake_minimum_required(VERSION 3.20)
project(aoahid_adb_proxy VERSION 3.1.3 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(AOAHID_ADB_PROXY_BUILD_EXAMPLES "Build examples." ON)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  option(AOAHID_ADB_PROXY_BUILD_TESTS "Build tests." ON)
else()
  option(AOAHID_ADB_PROXY_BUILD_TESTS "Build tests." OFF)
endif()

# libaoahid: an installed package (e.g. an official release archive via
# CMAKE_PREFIX_PATH) is preferred; otherwise the pinned source tag is fetched.
find_package(aoahid 4.0 CONFIG QUIET)
if(NOT aoahid_FOUND)
  include(FetchContent)
  FetchContent_Declare(
    aoahid
    GIT_REPOSITORY https://github.com/nemarpuc/libaoahid.git
    GIT_TAG        ef7b3d1e446123e0ee11a8c6cc167daaed893a36) # v4.0.0, by commit so a moved tag cannot change it
  FetchContent_MakeAvailable(aoahid)
endif()

find_package(Threads REQUIRED)

add_library(aoahid_adb_proxy SHARED src/aoahid_adb_proxy.cpp)
target_include_directories(aoahid_adb_proxy PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
target_link_libraries(aoahid_adb_proxy PUBLIC aoahid::aoahid PRIVATE Threads::Threads)
if(WIN32)
  target_link_libraries(aoahid_adb_proxy PRIVATE ws2_32)
  set_target_properties(aoahid_adb_proxy PROPERTIES WINDOWS_EXPORT_ALL_SYMBOLS ON)
endif()
set_target_properties(aoahid_adb_proxy PROPERTIES
  VERSION ${PROJECT_VERSION} SOVERSION ${PROJECT_VERSION_MAJOR})

if(AOAHID_ADB_PROXY_BUILD_EXAMPLES)
  add_executable(basic_proxy_example examples/basic_proxy.cpp)
  target_link_libraries(basic_proxy_example PRIVATE aoahid_adb_proxy)
endif()

if(AOAHID_ADB_PROXY_BUILD_TESTS)
  enable_testing()
  # The proxy source built against the test's in-memory Channel: only the
  # libaoahid headers are used, so the test needs no USB device.
  add_executable(aoahid_adb_proxy_tests tests/test_proxy.cpp src/aoahid_adb_proxy.cpp)
  target_include_directories(aoahid_adb_proxy_tests PRIVATE
    include $<TARGET_PROPERTY:aoahid::aoahid,INTERFACE_INCLUDE_DIRECTORIES>)
  target_link_libraries(aoahid_adb_proxy_tests PRIVATE Threads::Threads)
  if(WIN32)
    target_link_libraries(aoahid_adb_proxy_tests PRIVATE ws2_32)
  endif()
  add_test(NAME aoahid_adb_proxy.proxy COMMAND aoahid_adb_proxy_tests)
  set_tests_properties(aoahid_adb_proxy.proxy PROPERTIES TIMEOUT 120)
endif()

include(GNUInstallDirs)
install(TARGETS aoahid_adb_proxy
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(FILES include/aoahid_adb_proxy.h DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
if(AOAHID_ADB_PROXY_BUILD_EXAMPLES)
  install(TARGETS basic_proxy_example RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()
install(FILES LICENSE DESTINATION ${CMAKE_INSTALL_DATADIR}/doc/aoahid_adb_proxy)

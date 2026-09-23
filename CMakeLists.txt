cmake_minimum_required(VERSION 3.20)
project(aoahid_adb_proxy VERSION 2.0.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(AOAHID_ADB_PROXY_BUILD_EXAMPLES "Build examples." ON)

# Libaoa_hid: an installed package (e.g. an official release archive via
# CMAKE_PREFIX_PATH) is preferred; otherwise the pinned source tag is fetched.
find_package(aoahid 3.0 CONFIG QUIET)
if(NOT aoahid_FOUND)
  include(FetchContent)
  FetchContent_Declare(
    aoahid
    GIT_REPOSITORY https://github.com/nemarpuc/Libaoa_hid.git
    GIT_TAG        v3.0.0)
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

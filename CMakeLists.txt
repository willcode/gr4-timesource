# Copyright 2026 Jeff Long
# SPDX-License-Identifier: MIT

cmake_minimum_required(VERSION 3.28)

project(
  gr4-timesource
  VERSION 0.1.0
  DESCRIPTION "GPS and PPS timing source blocks for GNU Radio 4, loaded as a block library"
  LANGUAGES CXX)

# A single-config generator given no build type compiles without optimization and without
# NDEBUG. The package builds Release there unless the caller names a type.
get_property(GR4TIMESOURCE_MULTI_CONFIG GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
if(NOT CMAKE_BUILD_TYPE AND NOT GR4TIMESOURCE_MULTI_CONFIG)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
  message(STATUS "gr4-timesource: no CMAKE_BUILD_TYPE given; building Release")
endif()

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

set(GR4TIMESOURCE_WARNINGS -Wall -Wextra)

# GNU Radio 4 core and its block-library macros, and the platform libraries the blocks stand on.
# libserialport opens, configures, reads and lists serial ports, and states a USB port's vendor,
# product and strings; pkg-config finds it. The kernel clock interfaces are the platform's own
# headers, checked below.
find_package(Threads REQUIRED)
find_package(gnuradio4 4.0 CONFIG REQUIRED)
find_package(GnuRadioBlockLib CONFIG REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBSERIALPORT REQUIRED IMPORTED_TARGET libserialport)
message(STATUS "gr4-timesource: libserialport ${LIBSERIALPORT_VERSION} FOUND")

# The RFC 2783 PPS interface (Linux with pps-tools, FreeBSD, NetBSD) gives the HwPps clock mode.
# Without it the mode reports itself unavailable at start. The TAI offset is read from struct
# timex where the platform's has the field.
include(CheckCXXSourceCompiles)
check_cxx_source_compiles(
  [[
#include <sys/timepps.h>
int main() {
    pps_handle_t handle{};
    pps_info_t info{};
    struct timespec wait{};
    return time_pps_fetch(handle, PPS_TSFMT_TSPEC, &info, &wait);
}
]]
  GR4TIMESOURCE_HAVE_TIMEPPS)
if(GR4TIMESOURCE_HAVE_TIMEPPS)
  message(STATUS "gr4-timesource: RFC 2783 sys/timepps.h FOUND; the HwPps clock mode is built")
else()
  message(STATUS "gr4-timesource: RFC 2783 sys/timepps.h NOT FOUND; the HwPps clock mode reports itself unavailable")
endif()
check_cxx_source_compiles(
  [[
#include <sys/timex.h>
int main() {
    struct timex tx{};
    return static_cast<int>(tx.tai);
}
]]
  GR4TIMESOURCE_HAVE_TIMEX_TAI)

# The headers of the package: the NMEA parser, and the serial port and clock layers the blocks
# stand on. Installed under include/gr4-timesource/.
set(GR4TIMESOURCE_INSTALL_INCLUDEDIR ${CMAKE_INSTALL_INCLUDEDIR}/gr4-timesource)
add_library(timesource-headers INTERFACE)
target_include_directories(
  timesource-headers INTERFACE $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
                               $<INSTALL_INTERFACE:${GR4TIMESOURCE_INSTALL_INCLUDEDIR}>)
target_compile_features(timesource-headers INTERFACE cxx_std_23)
target_link_libraries(timesource-headers INTERFACE PkgConfig::LIBSERIALPORT Threads::Threads)
if(GR4TIMESOURCE_HAVE_TIMEPPS)
  target_compile_definitions(timesource-headers INTERFACE TIMESOURCE_HAVE_TIMEPPS)
endif()
if(GR4TIMESOURCE_HAVE_TIMEX_TAI)
  target_compile_definitions(timesource-headers INTERFACE TIMESOURCE_HAVE_TIMEX_TAI)
endif()
set_target_properties(timesource-headers PROPERTIES EXPORT_NAME headers)
add_library(gr4-timesource::headers ALIAS timesource-headers)

# ---- the block library -------------------------------------------------------------------
#
# include/gnuradio-4.0/timesource/TimesourceBlocks.hpp registers each block under its type name.
# gr_add_block_library() generates the registration units from it into an object library, and
# the shared library built from those objects goes to lib/gnuradio-4/plugins, in the build tree
# and under the install prefix. A program that loads plugins from there creates the blocks by
# name.
gr_add_block_library(GrTimesourceBlocks HEADERS include/gnuradio-4.0/timesource/TimesourceBlocks.hpp
                     LINK_LIBRARIES timesource-headers)
if(NOT TARGET GrTimesourceBlocksObject)
  message(FATAL_ERROR "gr4-timesource: the block registration was not generated")
endif()
target_compile_options(GrTimesourceBlocksObject PRIVATE ${GR4TIMESOURCE_WARNINGS})
add_library(gr4-timesource-blocks SHARED $<TARGET_OBJECTS:GrTimesourceBlocksObject>)
add_library(gr4-timesource::blocks ALIAS gr4-timesource-blocks)
set_target_properties(gr4-timesource-blocks PROPERTIES EXPORT_NAME blocks LIBRARY_OUTPUT_DIRECTORY
                                                                          ${PROJECT_BINARY_DIR}/lib/gnuradio-4/plugins)
target_link_libraries(gr4-timesource-blocks PUBLIC gnuradio4::gnuradio-core gnuradio4::gnuradio-blocklib-core
                                                   timesource-headers)

# ---- install -----------------------------------------------------------------------------
install(
  TARGETS gr4-timesource-blocks
  EXPORT gr4-timesourceTargets
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}/gnuradio-4/plugins)
install(TARGETS timesource-headers EXPORT gr4-timesourceTargets)
install(
  DIRECTORY include/timesource
  DESTINATION ${GR4TIMESOURCE_INSTALL_INCLUDEDIR}
  FILES_MATCHING
  PATTERN "*.hpp")
install(
  EXPORT gr4-timesourceTargets
  NAMESPACE gr4-timesource::
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/gr4-timesource)
file(
  CONFIGURE
  OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/gr4-timesourceConfig.cmake
  CONTENT
    [[
include(CMakeFindDependencyMacro)
find_dependency(Threads)
find_dependency(gnuradio4 CONFIG)
find_dependency(PkgConfig)
pkg_check_modules(LIBSERIALPORT QUIET IMPORTED_TARGET libserialport)
if(NOT LIBSERIALPORT_FOUND)
  set(gr4-timesource_FOUND FALSE)
  set(gr4-timesource_NOT_FOUND_MESSAGE "gr4-timesource needs libserialport, and pkg-config finds no libserialport.pc")
  return()
endif()
include("${CMAKE_CURRENT_LIST_DIR}/gr4-timesourceTargets.cmake")
]]
  @ONLY)
write_basic_package_version_file(${CMAKE_CURRENT_BINARY_DIR}/gr4-timesourceConfigVersion.cmake
                                 COMPATIBILITY SameMinorVersion)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/gr4-timesourceConfig.cmake
              ${CMAKE_CURRENT_BINARY_DIR}/gr4-timesourceConfigVersion.cmake
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/gr4-timesource)

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

# ---- the cases ---------------------------------------------------------------------------
#
# One binary per subject under test/ and one ctest entry per case, each named on the
# binary's command line. The offline cases carry the label offline and open no device. The
# hardware cases read a GPS receiver and carry the label hardware; GR4TIMESOURCE_GPS_DEVICE and
# GR4TIMESOURCE_GPS_BAUD in the environment name its serial port and rate. Boost.UT is one
# header, boost/ut.hpp. find_path() looks for it under include/ of every CMAKE_PREFIX_PATH entry
# and the system's include directories; GR4TIMESOURCE_UT_INCLUDE_DIR names its directory instead.
option(GR4TIMESOURCE_ENABLE_TESTING "Build the cases" ${PROJECT_IS_TOP_LEVEL})
option(GR4TIMESOURCE_ENABLE_HARDWARE_TESTS "Build the cases that read a GPS receiver (label hardware)" OFF)

set(GR4TIMESOURCE_TESTS FALSE)
if(GR4TIMESOURCE_ENABLE_TESTING)
  find_path(GR4TIMESOURCE_UT_INCLUDE_DIR boost/ut.hpp DOC "Directory holding boost/ut.hpp")
  if(GR4TIMESOURCE_UT_INCLUDE_DIR)
    message(STATUS "gr4-timesource: Boost.UT FOUND in ${GR4TIMESOURCE_UT_INCLUDE_DIR}")
    set(GR4TIMESOURCE_TESTS TRUE)
    enable_testing()
  else()
    message(STATUS "gr4-timesource: the cases are SKIPPED: no boost/ut.hpp under CMAKE_PREFIX_PATH; "
                   "set GR4TIMESOURCE_UT_INCLUDE_DIR to its directory")
  endif()
endif()

if(GR4TIMESOURCE_TESTS)
  add_library(timesource-ut INTERFACE)
  target_include_directories(timesource-ut SYSTEM INTERFACE ${GR4TIMESOURCE_UT_INCLUDE_DIR})
  target_include_directories(timesource-ut INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/test)

  # timesource_test(<binary> CASES <ctest name>... [LABEL <label>] [TIMEOUT <s>] [LIBS ...])
  #
  # Builds test/<binary>.cpp and registers one ctest entry per case. A case name that matches
  # nothing in the binary fails the entry (test/support/Cases.hpp).
  function(timesource_test name)
    cmake_parse_arguments(T "" "LABEL;TIMEOUT" "CASES;LIBS" ${ARGN})
    if(NOT T_LABEL)
      set(T_LABEL offline)
    endif()
    if(NOT T_TIMEOUT)
      set(T_TIMEOUT 15)
    endif()
    add_executable(${name} test/${name}.cpp)
    target_link_libraries(${name} PRIVATE timesource-headers timesource-ut ${T_LIBS})
    target_compile_options(${name} PRIVATE ${GR4TIMESOURCE_WARNINGS})
    foreach(case ${T_CASES})
      add_test(NAME ${case} COMMAND ${name} ${case})
      set_tests_properties(${case} PROPERTIES TIMEOUT ${T_TIMEOUT} LABELS ${T_LABEL})
    endforeach()
  endfunction()

  # test/support/FakeSerialPort.cpp defines the libserialport functions SerialPort calls, over
  # ports a case registers. A binary that links these objects uses those definitions in place of
  # the shared library's: the GPS block lists the registered ports and reads the sentences its
  # cases write, and no device is listed or opened.
  add_library(timesource-fake-serialport OBJECT test/support/FakeSerialPort.cpp)
  target_link_libraries(timesource-fake-serialport PRIVATE timesource-headers timesource-ut)
  target_compile_options(timesource-fake-serialport PRIVATE ${GR4TIMESOURCE_WARNINGS})

  timesource_test(
    qa_SerialPort
    CASES serial.ranks-a-receiver-above-a-bridge
          serial.rates-by-name
          serial.a-product-string-names-a-receiver
          serial.describes-a-usb-port
          serial.names-the-ports-it-saw
          serial.detection-passes-over-a-silent-port
          serial.refuses-a-path-that-does-not-exist
          serial.refuses-a-file-that-is-no-serial-port)

  timesource_test(
    qa_GpsSource
    CASES gps.emits-a-tag-on-each-new-second
          gps.marks-a-second-without-a-fix-unlocked
          gps.meta-info-off-leaves-the-fix-out
          gps.clock-mode-writes-sample-rate-samples-per-second
          gps.context-goes-on-the-tag
          gps.enumerations-are-set-by-name
          gps.reads-at-4800-baud-8n1-by-default
          gps.refuses-a-start-on-a-path-that-does-not-exist
          gps.detection-passes-over-a-silent-port
    LIBS gnuradio4::gnuradio-core gnuradio4::gnuradio-blocklib-core timesource-fake-serialport)

  timesource_test(
    qa_PpsSource
    CASES pps.ntp-emits-one-tag-per-second
          pps.ptp-without-a-device-is-unavailable
          pps.hardware-pps-without-a-device-is-unavailable
          pps.discipline-units
          pps.ptp-locks-to-a-disciplined-system-clock
          pps.a-ptp-clock-on-tai-takes-the-kernel-offset-or-the-fallback
          pps.ptp-offset-reads-both-clocks
          pps.a-ptp-block-tags-the-utc-second
          pps.a-clock-step-keeps-the-seconds-in-order
          pps.a-fetch-error-is-paced-and-reported-once
          pps.auto-is-the-system-clock
          pps.names-modes-and-leap-states
          pps.a-pulse-marks-its-nearest-second
          pps.time-to-the-next-second
          pps.samples-per-second-follow-the-mode
    LIBS gnuradio4::gnuradio-core gnuradio4::gnuradio-blocklib-core)

  timesource_test(
    qa_NmeaParser
    CASES nmea.checksum-of-a-known-sentence
          nmea.rmc-fields
          nmea.gga-fields
          nmea.zda-fields
          nmea.gsa-fields
          nmea.vtg-fields
          nmea.a-new-second-closes-the-record
          nmea.a-second-carries-the-date-across-a-midnight
          nmea.a-leap-second-is-23-59-59-plus-one-second
          nmea.any-talker-prefix
          nmea.a-bad-checksum-changes-nothing
          nmea.a-truncated-sentence-changes-nothing
          nmea.an-empty-field-keeps-the-default
          nmea.a-sentence-without-time-opens-no-second
          nmea.other-text-is-ignored
          nmea.checked-counts-checked-sentences-of-the-read-kinds)

  # The GPS receiver case reads a real receiver for up to thirty seconds. It is off by default,
  # carries the label hardware, and reads the port GR4TIMESOURCE_GPS_DEVICE names at the rate
  # GR4TIMESOURCE_GPS_BAUD names, 4800 where unset. It opens the port for reading only.
  if(GR4TIMESOURCE_ENABLE_HARDWARE_TESTS)
    timesource_test(
      hw_GpsSource
      CASES hw.gps-a-receiver-tags-a-second
      LABEL hardware
      TIMEOUT 60
      LIBS gnuradio4::gnuradio-core gnuradio4::gnuradio-blocklib-core)
    message(STATUS "gr4-timesource: the GPS receiver case is ENABLED (label hardware)")
  endif()
endif()

# ---- the examples ------------------------------------------------------------------------
#
# gps_status builds a GpsSource graph in code and prints the receiver's status once per second.
# examples/gps_status.grc holds the same source as a graph file for rungraph. Neither is
# installed.
option(GR4TIMESOURCE_ENABLE_EXAMPLES "Build the example programs" ${PROJECT_IS_TOP_LEVEL})
if(GR4TIMESOURCE_ENABLE_EXAMPLES)
  add_executable(gps_status examples/gps_status.cpp)
  target_link_libraries(gps_status PRIVATE timesource-headers gnuradio4::gnuradio-core gnuradio4::gnuradio-blocklib-core)
  target_compile_options(gps_status PRIVATE ${GR4TIMESOURCE_WARNINGS})
endif()

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

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

# DiscontinuityMonitor needs gnuradio-4.0/algorithm/timing/SampleClock.hpp from gnuradio4Library
# and gr::UnfilteredTagPropagation from the core. Upstream main carries the tag policy, and the
# willcode fork of the library adds SampleClock.hpp.
# GR4TIMESOURCE_TIMING_BLOCKS takes AUTO, ON or OFF. AUTO builds each block whose pieces are
# found and prints one line for each block it leaves out. ON fails the configure where a block
# lacks a piece. OFF builds no measurement block. Each check is a try_compile against the found targets
# through check_cxx_source_compiles(): __has_include() for a header, a use of the name for the
# tag policy. A failed result leaves the cache before its check runs again, and a reconfigure
# sees a piece the prefix gained since. A passed result stays cached.
set(GR4TIMESOURCE_TIMING_BLOCKS
    AUTO
    CACHE STRING "Build the measurement blocks where their pieces are found: AUTO, ON or OFF")
set_property(CACHE GR4TIMESOURCE_TIMING_BLOCKS PROPERTY STRINGS AUTO ON OFF)
if(NOT GR4TIMESOURCE_TIMING_BLOCKS MATCHES "^(ON|OFF|AUTO)$")
  message(FATAL_ERROR "gr4-timesource: GR4TIMESOURCE_TIMING_BLOCKS is '${GR4TIMESOURCE_TIMING_BLOCKS}'; "
                      "it takes AUTO, ON or OFF")
endif()

# timesource_probe(<result> <source> <libraries>): the check, rerun while its cached result is a failure.
macro(timesource_probe result source libraries)
  if(NOT ${result})
    unset(${result} CACHE)
  endif()
  set(CMAKE_REQUIRED_LIBRARIES ${libraries})
  set(CMAKE_REQUIRED_QUIET ON)
  check_cxx_source_compiles("${source}" ${result})
  unset(CMAKE_REQUIRED_LIBRARIES)
  unset(CMAKE_REQUIRED_QUIET)
endmacro()

# timesource_timing_block(<block> <out> <piece>...): <out> is TRUE where the found GNU Radio 4
# holds every piece, a header under include/ or gr::UnfilteredTagPropagation. Otherwise AUTO
# prints the block and its missing pieces with their origin, and ON stops the configure with the
# same text.
function(timesource_timing_block block out)
  set(missing "")
  foreach(piece ${ARGN})
    string(MAKE_C_IDENTIFIER "${piece}" id)
    if(piece STREQUAL "gr::UnfilteredTagPropagation")
      timesource_probe(GR4TIMESOURCE_HAS_${id} "#include <gnuradio-4.0/Block.hpp>\nusing Policy = gr::UnfilteredTagPropagation;\nint main() { return 0; }"
                       gnuradio4::gnuradio-core)
    elseif(TARGET gnuradio4::gnuradio-algorithm)
      timesource_probe(GR4TIMESOURCE_HAS_${id} "#if !__has_include(<${piece}>)\n#error absent\n#endif\nint main() { return 0; }"
                       gnuradio4::gnuradio-algorithm)
    else()
      set(GR4TIMESOURCE_HAS_${id} FALSE)
    endif()
    if(NOT GR4TIMESOURCE_HAS_${id})
      list(APPEND missing "${piece}")
    endif()
  endforeach()
  if(NOT missing)
    set(${out} TRUE PARENT_SCOPE)
    message(STATUS "gr4-timesource: ${block} is built")
    return()
  endif()
  set(${out} FALSE PARENT_SCOPE)
  list(JOIN missing ", " lacking)
  set(origins "")
  if(NOT TARGET gnuradio4::gnuradio-algorithm AND NOT missing STREQUAL "gr::UnfilteredTagPropagation")
    string(APPEND origins " No gnuradio4Library package was found.")
  endif()
  if("gnuradio-4.0/algorithm/timing/SampleClock.hpp" IN_LIST missing)
    string(APPEND origins " gnuradio-4.0/algorithm/timing/SampleClock.hpp comes from the willcode fork of the GNU "
                          "Radio 4 library, and upstream main lacks it.")
  endif()
  if("gr::UnfilteredTagPropagation" IN_LIST missing)
    string(APPEND origins " gr::UnfilteredTagPropagation comes from the GNU Radio 4 core, upstream main included.")
  endif()
  if(GR4TIMESOURCE_TIMING_BLOCKS STREQUAL "ON")
    message(FATAL_ERROR "gr4-timesource: GR4TIMESOURCE_TIMING_BLOCKS is ON, and ${block} needs ${lacking}, which "
                        "the found GNU Radio 4 lacks.${origins} Set GR4TIMESOURCE_TIMING_BLOCKS to AUTO to build "
                        "the blocks whose pieces are found, or to OFF.")
  endif()
  message(STATUS "gr4-timesource: ${block} is SKIPPED: the found GNU Radio 4 lacks ${lacking}.${origins} "
                 "The rest of the package builds without ${block}.")
endfunction()

set(GR4TIMESOURCE_DISCONTINUITY FALSE)
if(NOT GR4TIMESOURCE_TIMING_BLOCKS STREQUAL "OFF")
  find_package(gnuradio4Library CONFIG QUIET)
  timesource_timing_block(DiscontinuityMonitor GR4TIMESOURCE_DISCONTINUITY
                          gnuradio-4.0/algorithm/timing/SampleClock.hpp gr::UnfilteredTagPropagation)
endif()

# The headers of the package: the blocks, the NMEA parser, and the serial port and clock layers
# the sources stand on. Installed under include/gr4-timesource/.
set(GR4TIMESOURCE_INSTALL_INCLUDEDIR ${CMAKE_INSTALL_INCLUDEDIR}/gr4-timesource)
add_library(timesource-headers INTERFACE)
target_include_directories(
  timesource-headers INTERFACE $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
                               $<INSTALL_INTERFACE:${GR4TIMESOURCE_INSTALL_INCLUDEDIR}>)
target_compile_features(timesource-headers INTERFACE cxx_std_23)
target_link_libraries(timesource-headers INTERFACE PkgConfig::LIBSERIALPORT Threads::Threads)
if(GR4TIMESOURCE_DISCONTINUITY)
  target_link_libraries(timesource-headers INTERFACE gnuradio4::gnuradio-algorithm)
endif()
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
# include/gnuradio-4.0/timesource/TimesourceBlocks.hpp registers each source under its type name.
# DiscontinuityMonitorBlocks.hpp beside it registers DiscontinuityMonitor, and the build reads it
# where the block is built. gr_add_block_library()
# generates the registration units into an object library, and
# the shared library built from those objects goes to lib/gnuradio-4/plugins, in the build tree
# and under the install prefix. A program that loads plugins from there creates the blocks by
# name. A block-library macro that audits the registering headers under include/
# (GR_BLOCKLIB_REQUIRE_REGISTERED_HEADERS) takes the header of a block left out as unregistered
# on purpose.
set(GR4TIMESOURCE_REGISTERED include/gnuradio-4.0/timesource/TimesourceBlocks.hpp)
set(GR4TIMESOURCE_UNREGISTERED "")
foreach(block DISCONTINUITY:DiscontinuityMonitor)
  string(REPLACE ":" ";" pair "${block}")
  list(GET pair 0 flag)
  list(GET pair 1 name)
  if(GR4TIMESOURCE_${flag})
    list(APPEND GR4TIMESOURCE_REGISTERED include/gnuradio-4.0/timesource/${name}Blocks.hpp)
  else()
    list(APPEND GR4TIMESOURCE_UNREGISTERED include/gnuradio-4.0/timesource/${name}Blocks.hpp)
  endif()
endforeach()
set(GR4TIMESOURCE_REGISTRATION HEADERS ${GR4TIMESOURCE_REGISTERED})
if(GR4TIMESOURCE_UNREGISTERED AND DEFINED GR_BLOCKLIB_REQUIRE_REGISTERED_HEADERS)
  list(APPEND GR4TIMESOURCE_REGISTRATION UNREGISTERED_HEADERS ${GR4TIMESOURCE_UNREGISTERED})
endif()
gr_add_block_library(GrTimesourceBlocks ${GR4TIMESOURCE_REGISTRATION} LINK_LIBRARIES timesource-headers)
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

  # The measurement cases drive their block through the test blocks of the GNU Radio 4 blocks
  # package, found as gnuradio4Blocks.
  if(GR4TIMESOURCE_DISCONTINUITY)
    find_package(gnuradio4Blocks CONFIG QUIET)
  endif()
  if(GR4TIMESOURCE_DISCONTINUITY AND TARGET gnuradio4::gr-testing)
    timesource_test(
      qa_DiscontinuityMonitor
      CASES discontinuity.totals-dropped-samples-and-counts-every-cause
            discontinuity.a-rate-change-re-anchors-and-both-epochs-convert-exactly
            discontinuity.refuses-counts-and-names-a-rate-outside-the-tolerance
            discontinuity.the-account-is-independent-of-the-chunking
            discontinuity.every-type-passes-its-samples-through
            discontinuity.forwards-a-key-the-default-filter-drops
      LIBS gnuradio4::gnuradio-core gnuradio4::gnuradio-blocklib-core gnuradio4::gr-testing)
  elseif(GR4TIMESOURCE_DISCONTINUITY)
    message(STATUS "gr4-timesource: the DiscontinuityMonitor cases are SKIPPED: no gnuradio4Blocks under "
                   "CMAKE_PREFIX_PATH")
  endif()

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
set(GR4TIMESOURCE_HEADER_EXCLUDES "")
if(NOT GR4TIMESOURCE_DISCONTINUITY)
  list(APPEND GR4TIMESOURCE_HEADER_EXCLUDES PATTERN "DiscontinuityMonitor.hpp" EXCLUDE)
endif()
install(
  DIRECTORY include/timesource
  DESTINATION ${GR4TIMESOURCE_INSTALL_INCLUDEDIR}
  FILES_MATCHING
  PATTERN "*.hpp" ${GR4TIMESOURCE_HEADER_EXCLUDES})
install(
  EXPORT gr4-timesourceTargets
  NAMESPACE gr4-timesource::
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/gr4-timesource)
set(GR4TIMESOURCE_CONFIG_LIBRARY "")
if(GR4TIMESOURCE_DISCONTINUITY)
  set(GR4TIMESOURCE_CONFIG_LIBRARY "find_dependency(gnuradio4Library CONFIG)")
endif()
file(
  CONFIGURE
  OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/gr4-timesourceConfig.cmake
  CONTENT
    [[
include(CMakeFindDependencyMacro)
find_dependency(Threads)
find_dependency(gnuradio4 CONFIG)
@GR4TIMESOURCE_CONFIG_LIBRARY@
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

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

# The platform library the package stands on. libserialport opens, configures, reads and lists
# serial ports, and states a USB port's vendor, product and strings; pkg-config finds it.
find_package(Threads REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBSERIALPORT REQUIRED IMPORTED_TARGET libserialport)
message(STATUS "gr4-timesource: libserialport ${LIBSERIALPORT_VERSION} FOUND")

# The headers of the package: the NMEA parser and the serial port layer. Installed under
# include/gr4-timesource/.
set(GR4TIMESOURCE_INSTALL_INCLUDEDIR ${CMAKE_INSTALL_INCLUDEDIR}/gr4-timesource)
add_library(timesource-headers INTERFACE)
target_include_directories(
  timesource-headers INTERFACE $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
                               $<INSTALL_INTERFACE:${GR4TIMESOURCE_INSTALL_INCLUDEDIR}>)
target_compile_features(timesource-headers INTERFACE cxx_std_23)
target_link_libraries(timesource-headers INTERFACE PkgConfig::LIBSERIALPORT Threads::Threads)
set_target_properties(timesource-headers PROPERTIES EXPORT_NAME headers)
add_library(gr4-timesource::headers ALIAS timesource-headers)

# ---- install -----------------------------------------------------------------------------
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

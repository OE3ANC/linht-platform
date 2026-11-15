# SPDX-License-Identifier: GPL-3.0-only
cmake_minimum_required(VERSION 3.25)
project(linht C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
add_compile_definitions(_GNU_SOURCE)
add_compile_options(-Wall -Wextra)

include(GNUInstallDirs)
find_package(PkgConfig REQUIRED)
pkg_check_modules(SYSTEMD REQUIRED IMPORTED_TARGET libsystemd)
pkg_check_modules(YAML REQUIRED IMPORTED_TARGET yaml-0.1)

enable_testing()
add_subdirectory(settingsd)
add_subdirectory(shelld)
add_subdirectory(apps)

install(DIRECTORY schemas/ DESTINATION ${CMAKE_INSTALL_DATADIR}/linht/schemas FILES_MATCHING PATTERN "*.yaml")
install(FILES apps/radio/manifest.yaml DESTINATION lib/linht/apps/org.m17project.linht.radio)

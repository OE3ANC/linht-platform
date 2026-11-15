# SPDX-License-Identifier: GPL-3.0-only
add_library(settings-value STATIC settings/value.c)
target_include_directories(settings-value PUBLIC settings)

add_executable(test-settings-value settings/tests/test_value.c)
target_link_libraries(test-settings-value PRIVATE settings-value)
add_test(NAME settings-app-value COMMAND test-settings-value)

# UI apps: against the radio's LVGL (pkg-config), or the same LVGL revision with SDL for the simulator.
option(LINHT_SIM "Build the UI apps for the SDL desktop simulator" OFF)
if(LINHT_SIM)
  include(FetchContent)
  set(LV_CONF_PATH ${PROJECT_SOURCE_DIR}/sim/lv_conf.h CACHE STRING "" FORCE)
  set(LV_CONF_BUILD_DISABLE_EXAMPLES ON CACHE BOOL "" FORCE)
  set(LV_CONF_BUILD_DISABLE_DEMOS ON CACHE BOOL "" FORCE)
  set(LV_CONF_BUILD_DISABLE_THORVG_INTERNAL ON CACHE BOOL "" FORCE)
  FetchContent_Declare(lvgl GIT_REPOSITORY https://github.com/lvgl/lvgl.git
                       GIT_TAG e1c0b21b2723d391b885de4b2ee5cc997eccca91) # v9.1.0, as in the image
  FetchContent_MakeAvailable(lvgl)
  target_compile_options(lvgl PRIVATE -w) # third-party code: our -Wall -Wextra is for our own sources
  pkg_check_modules(SDL2 REQUIRED IMPORTED_TARGET sdl2)
  target_link_libraries(lvgl PUBLIC PkgConfig::SDL2)
  set(LVGL_TARGET lvgl)
else()
  pkg_check_modules(LVGL IMPORTED_TARGET lvgl)
  if(LVGL_FOUND)
    # meta-oe builds liblvgl with its DRM driver but links neither it nor lvgl.pc against libdrm
    pkg_check_modules(DRM REQUIRED IMPORTED_TARGET libdrm)
    set(LVGL_TARGET PkgConfig::LVGL PkgConfig::DRM)
  endif()
endif()

if(LVGL_TARGET)
  add_library(linht-ui STATIC ${PROJECT_SOURCE_DIR}/ui/linht-ui.c)
  target_include_directories(linht-ui PUBLIC ${PROJECT_SOURCE_DIR}/ui)
  target_link_libraries(linht-ui PUBLIC ${LVGL_TARGET} PkgConfig::SYSTEMD)
  if(LINHT_SIM)
    target_compile_definitions(linht-ui PUBLIC LINHT_SIM)
  endif()

  add_executable(linht-launcher launcher/main.c)
  target_link_libraries(linht-launcher PRIVATE linht-ui)
  add_executable(linht-settings settings/main.c)
  target_link_libraries(linht-settings PRIVATE linht-ui settings-value)

  foreach(app launcher settings)
    install(TARGETS linht-${app} DESTINATION lib/linht/apps/org.m17project.linht.${app}/bin)
    install(FILES ${app}/manifest.yaml DESTINATION lib/linht/apps/org.m17project.linht.${app})
  endforeach()
  install(FILES ${PROJECT_SOURCE_DIR}/ui/70-linht-lcd.rules DESTINATION lib/udev/rules.d)
else()
  message(STATUS "UI apps disabled: no LVGL found (-DLINHT_SIM=ON builds them for the simulator)")
endif()

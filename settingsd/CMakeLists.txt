# SPDX-License-Identifier: GPL-3.0-only
add_library(settings-core STATIC schema.c store.c)
target_include_directories(settings-core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR} ${PROJECT_SOURCE_DIR}/common)
target_link_libraries(settings-core PUBLIC PkgConfig::YAML)

add_executable(test-schema tests/test_schema.c)
target_link_libraries(test-schema PRIVATE settings-core)
target_compile_definitions(test-schema PRIVATE SCHEMA_DIR="${PROJECT_SOURCE_DIR}/schemas")
add_test(NAME settings-schema COMMAND test-schema)

add_executable(test-store tests/test_store.c)
target_link_libraries(test-store PRIVATE settings-core)
add_test(NAME settings-store COMMAND test-store)

add_executable(linht-settingsd main.c bus.c)
target_link_libraries(linht-settingsd PRIVATE settings-core PkgConfig::SYSTEMD)

configure_file(linht-settingsd.service.in linht-settingsd.service @ONLY)
install(TARGETS linht-settingsd DESTINATION ${CMAKE_INSTALL_LIBEXECDIR}/linht)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/linht-settingsd.service DESTINATION lib/systemd/system)
install(FILES org.m17project.LinHT.Settings.conf DESTINATION ${CMAKE_INSTALL_DATADIR}/dbus-1/system.d)
install(FILES linht-settingsd.sysusers DESTINATION lib/sysusers.d RENAME linht-settingsd.conf)

# D-Bus tests need a private session bus and python dbus-next (in the dev shell, not on the device)
find_package(Python3 COMPONENTS Interpreter)
find_program(DBUS_RUN_SESSION dbus-run-session)
if(Python3_FOUND AND DBUS_RUN_SESSION)
  execute_process(COMMAND ${Python3_EXECUTABLE} -c "import dbus_next" RESULT_VARIABLE NO_DBUS_NEXT OUTPUT_QUIET ERROR_QUIET)
endif()
if(DBUS_RUN_SESSION AND Python3_FOUND AND NOT NO_DBUS_NEXT)
  add_test(NAME settings-bus
           COMMAND ${DBUS_RUN_SESSION} --config-file=${CMAKE_CURRENT_SOURCE_DIR}/tests/session.conf --
                   ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_bus.py
                   $<TARGET_FILE:linht-settingsd> ${PROJECT_SOURCE_DIR}/schemas ${PROJECT_SOURCE_DIR}/interfaces)
  set_tests_properties(settings-bus PROPERTIES TIMEOUT 60)
else()
  message(STATUS "settings-bus test disabled: needs dbus-run-session and python3 dbus-next")
endif()

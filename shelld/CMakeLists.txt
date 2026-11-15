# SPDX-License-Identifier: GPL-3.0-only
add_library(shell-core STATIC keys.c apps.c fg.c)
target_include_directories(shell-core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR} ${PROJECT_SOURCE_DIR}/common)
target_link_libraries(shell-core PUBLIC PkgConfig::YAML)

add_executable(test-keys tests/test_keys.c)
target_link_libraries(test-keys PRIVATE shell-core)
add_test(NAME shell-keys COMMAND test-keys)

add_executable(test-apps tests/test_apps.c)
target_link_libraries(test-apps PRIVATE shell-core)
add_test(NAME shell-apps COMMAND test-apps)

add_executable(test-fg tests/test_fg.c)
target_link_libraries(test-fg PRIVATE shell-core)
add_test(NAME shell-fg COMMAND test-fg)

add_executable(linht-shelld main.c)
target_link_libraries(linht-shelld PRIVATE shell-core PkgConfig::SYSTEMD)

configure_file(linht-shelld.service.in linht-shelld.service @ONLY)
install(TARGETS linht-shelld DESTINATION ${CMAKE_INSTALL_LIBEXECDIR}/linht)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/linht-shelld.service DESTINATION lib/systemd/system)
install(FILES linht-gui-test-after-shelld.conf DESTINATION lib/systemd/system/linht-gui-test.service.d
        RENAME 50-linht-shelld.conf)
install(FILES org.m17project.LinHT.Shell.conf DESTINATION ${CMAKE_INSTALL_DATADIR}/dbus-1/system.d)
install(FILES 70-linht-keys.rules DESTINATION lib/udev/rules.d)

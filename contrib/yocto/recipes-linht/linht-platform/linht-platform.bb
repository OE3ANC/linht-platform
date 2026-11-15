# SPDX-License-Identifier: GPL-3.0-only
SUMMARY = "LinHT platform: settings service, shell, launcher and system apps"
HOMEPAGE = "https://github.com/OE3ANC/linht-platform"
LICENSE = "GPL-3.0-only"
LIC_FILES_CHKSUM = "file://LICENSE;md5=1ebbd3e34237af26da5dc08a4e440464"

# Built from the repo this layer lives in (D28, D30), mounted read-only at /linht-launcher in the
# build container. The file fetcher checksums the sources, so changes rebuild. A git SRC_URI with a
# pinned SRCREV could only point to an older commit of this same repo.
REPO = "${@os.path.abspath(d.getVar('THISDIR') + '/../../../..')}"
FILESEXTRAPATHS:prepend := "${REPO}:"
SRC_URI = " \
    file://CMakeLists.txt;subdir=src \
    file://LICENSE;subdir=src \
    file://common;subdir=src \
    file://settingsd;subdir=src \
    file://shelld;subdir=src \
    file://ui;subdir=src \
    file://apps;subdir=src \
    file://schemas;subdir=src \
    file://interfaces;subdir=src \
    file://tests;subdir=src \
"
S = "${WORKDIR}/src"

inherit cmake pkgconfig systemd useradd

DEPENDS = "systemd libyaml lvgl libdrm"
# /dev/input/keypad (linht-input-rules) and linht-gui-test.service, the interim Radio app (linht-utils)
RDEPENDS:${PN} += "linht-input-rules linht-utils"

# Created at build time, so dbus-daemon knows the user when it reads the policy at boot.
USERADD_PACKAGES = "${PN}"
USERADD_PARAM:${PN} = "--system --home-dir /var/lib/linht/settings --no-create-home --shell /sbin/nologin linht-settings"

SYSTEMD_SERVICE:${PN} = "linht-settingsd.service linht-shelld.service"
SYSTEMD_AUTO_ENABLE = "enable"

FILES:${PN} += " \
    ${libexecdir}/linht \
    ${nonarch_libdir}/linht/apps \
    ${datadir}/linht/schemas \
    ${datadir}/dbus-1/system.d \
    ${systemd_system_unitdir}/linht-gui-test.service.d \
    ${nonarch_libdir}/sysusers.d \
    ${nonarch_base_libdir}/udev/rules.d \
    ${nonarch_libdir}/udev/rules.d \
"

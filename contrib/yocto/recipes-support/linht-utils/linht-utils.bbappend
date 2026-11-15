# SPDX-License-Identifier: GPL-3.0-only

# Pin the revision the 2026-11-14 image was built from; the recipe tracks AUTOREV (D28).
SRCREV = "66438c4accad392ac631f362dd5e6e867c3c759e"

# LinHT-utils is GPL-3.0 (D20), but upstream has no LICENSE file yet, and the recipe says CLOSED.
LICENSE = "GPL-3.0-only"
LIC_FILES_CHKSUM = "file://${COMMON_LICENSE_DIR}/GPL-3.0-only;md5=c79ff39f19dfec6d293b95dea7b07891"

# gui_test as the interim Radio app (D16)
FILESEXTRAPATHS:prepend := "${THISDIR}/files:"
SRC_URI += " \
    file://0001-gui_test-read-keys-from-linht-keys.patch \
    file://0002-gui_test-run-RF-cleanup-on-SIGTERM.patch \
    file://0003-gui_test-use-standard-evdev-key-values.patch \
"
# 0003 must ship with the keypad driver fix in contrib/yocto/recipes-kernel/matrix-keypad.
RDEPENDS:${PN} += "matrix-keypad-module"

# The recipe copies ${S}/* to /root/LinHT-utils; drop quilt's patches/, whose links point to the build host.
do_install:append() {
    rm -rf ${D}/root/LinHT-utils/patches
}

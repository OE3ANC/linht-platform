# SPDX-License-Identifier: GPL-3.0-only
# Standard evdev key values on Rev C (the driver inverted every key). Ships together with
# linht-utils' 0003-gui_test-use-standard-evdev-key-values.patch; either one alone makes PTT
# key the transmitter on release.
FILESEXTRAPATHS:prepend := "${THISDIR}/files:"
SRC_URI += "file://0001-matrix-keypad-report-presses-as-1.patch"

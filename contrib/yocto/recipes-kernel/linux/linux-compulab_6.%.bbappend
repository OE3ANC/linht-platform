# SPDX-License-Identifier: GPL-3.0-only
FILESEXTRAPATHS:prepend := "${THISDIR}/files:"

SRC_URI += " \
    file://linht-platform.cfg \
    file://linht-platform.dtsi \
"

# The CompuLab recipe ends do_configure with "make compulab-mx93_defconfig", which regenerates
# .config and drops fragments merged by kernel-yocto. meta-linht-hardware therefore appends its
# options to ${B}/.config and copies its device tree in do_configure:append. Do the same in a
# task of its own after do_configure, so the result never depends on bbappend parse order.
# The kernel build re-syncs the appended options (later assignments override earlier ones).
do_linht_platform() {
    dts=${S}/arch/${ARCH}/boot/dts/compulab
    install -m 0644 ${WORKDIR}/linht-platform.dtsi $dts/
    grep -q '"linht-platform.dtsi"' $dts/sbc-mcm-imx93.dts || \
        echo '#include "linht-platform.dtsi"' >> $dts/sbc-mcm-imx93.dts

    grep -E '^CONFIG_[A-Z0-9_]+=' ${WORKDIR}/linht-platform.cfg | while read -r opt; do
        grep -qxF "$opt" ${B}/.config || echo "$opt" >> ${B}/.config
    done
}
addtask linht_platform after do_configure before do_compile

# SPDX-License-Identifier: GPL-2.0
#
# haydn (Mi 11X Pro / Redmi K40 Pro+, SM8350 lahaina) device trees on 5.10.
# Base SoC dtbs go into vendor_boot (BOARD_INCLUDE_DTB_IN_BOOTIMG),
# the haydn overlay goes into dtbo.img.

dtb-$(CONFIG_ARCH_LAHAINA) += lahaina.dtb lahaina-v2.dtb lahaina-v2.1.dtb

ifeq ($(CONFIG_BUILD_ARM64_DT_OVERLAY),y)
dtbo-$(CONFIG_ARCH_LAHAINA) += haydn-sm8350-overlay.dtbo
haydn-sm8350-overlay.dtbo-base := lahaina.dtb lahaina-v2.dtb lahaina-v2.1.dtb
endif

always-y	+= $(dtb-y)
clean-files	:= *.dtb *.dtbo

# SPDX-License-Identifier: GPL-2.0-only

obj-$(CONFIG_ARCH_QCOM) += drivers/platform/msm/

# clang 18 / 5.10 port: keep msm-5.4 code building
subdir-ccflags-y += -Wno-shift-count-overflow -Wno-tautological-pointer-compare -Wno-unused-label -Wno-unused-variable

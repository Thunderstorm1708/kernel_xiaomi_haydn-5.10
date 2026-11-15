export RMNET_CORE_PATH=datarmnet/core
export RMNET_CORE_INC_DIR=$(srctree)/$(src)/../$(RMNET_CORE_PATH)

obj-y += offload/ shs/

# 5.10: y2038/netlink compat for the msm-5.4 sources
subdir-ccflags-y += -include $(srctree)/techpack/datarmnet-ext/compat510.h
# generated (obfuscated) sources
subdir-ccflags-y += -Wno-misleading-indentation

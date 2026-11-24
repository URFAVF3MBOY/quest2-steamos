# SPDX-License-Identifier: GPL-2.0-only
# Qualcomm's Spectra camera drivers from Meta's Quest 2 kernel (4.19), for its own camera stack (Meta's tracker).
# Built at techpack/camera; the include paths and the compatibility header reach every subdirectory.
subdir-ccflags-y += -DCONFIG_SPECTRA_CAMERA=1 -DCONFIG_SPECTRA_CAMERA_ICP=1 \
	-DCONFIG_SPECTRA_CAMERA_LOG_ERROR=1 -DCONFIG_SPECTRA_CAMERA_LOG_WARN=1 -DCONFIG_SPECTRA_CAMERA_LOG_INFO=1 \
	-I$(srctree)/techpack/camera/include/uapi -I$(srctree)/techpack/camera/include \
	-include $(srctree)/techpack/camera/include/q2cam_compat.h \
	-Wno-error -Wno-missing-prototypes -Wno-missing-declarations
obj-y += drivers/

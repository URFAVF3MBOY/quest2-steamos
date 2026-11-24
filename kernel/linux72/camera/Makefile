# SPDX-License-Identifier: GPL-2.0-only
# Qualcomm's Spectra camera drivers from Meta's Quest 2 kernel (4.19), for its own camera stack (Meta's tracker).
# Built at techpack/camera; the include paths reach every subdirectory.
subdir-ccflags-y += -DCONFIG_SPECTRA_CAMERA=1 -DCONFIG_SPECTRA_CAMERA_ICP=1 \
	-I$(srctree)/techpack/camera/include/uapi -I$(srctree)/techpack/camera/include \
	-Wno-error -Wno-missing-prototypes -Wno-missing-declarations
obj-y += drivers/

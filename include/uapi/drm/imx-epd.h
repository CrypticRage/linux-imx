/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
#ifndef _UAPI_IMX_EPD_H_
#define _UAPI_IMX_EPD_H_
#include <drm/drm.h>
#define DRM_IMX_EPD_SEQUENCE 0x00
#define IMX_EPD_VERSION 1
#define IMX_EPD_SUBMIT 0
#define IMX_EPD_STATUS 1
#define IMX_EPD_ABORT 2
#define IMX_EPD_CAPS 3
#define IMX_EPD_INJECT 4
#define IMX_EPD_INJECT_UNDERRUN 1
#define IMX_EPD_INJECT_STALL 2
#define IMX_EPD_REFS_HELD 1
#define IMX_EPD_CONTROLLER_STOPPED 2
#define IMX_EPD_CAP_FAULT_TESTS 4
#define IMX_EPD_IDLE 0
#define IMX_EPD_RUNNING 1
#define IMX_EPD_DONE 2
#define IMX_EPD_ABORTED 3
#define IMX_EPD_FAULT 4
#define IMX_EPD_MAX_PHASES 256
#define IMX_EPD_MAX_SCANS 256
struct drm_imx_epd_phase { __u32 fb_id; __u32 scans; };
struct drm_imx_epd_sequence {
 __u32 version, command, crtc_id, idle_fb_id;
 __u32 num_phases, flags;
 __u64 phases_ptr, generation;
 __s32 lock_fd;
 __u32 status, completed_scans;
 __s32 phase_index, error;
 __u32 count_valid;
};
#define DRM_IOCTL_IMX_EPD_SEQUENCE DRM_IOWR(DRM_COMMAND_BASE + DRM_IMX_EPD_SEQUENCE, struct drm_imx_epd_sequence)
#endif

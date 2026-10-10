/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _IMX_EPD_H_
#define _IMX_EPD_H_
#include <uapi/drm/imx-epd.h>
struct drm_device;
struct drm_file;
struct drm_crtc;
int lcdifv3_epd_ioctl(struct drm_device *, void *, struct drm_file *);
u64 lcdifv3_epd_deadline_ns(struct drm_crtc *);
int imx93_epd_park_outputs(struct drm_crtc *);
#endif

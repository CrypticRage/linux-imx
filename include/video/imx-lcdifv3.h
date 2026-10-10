/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * Copyright 2019,2022 NXP
 */

#ifndef __IMX_LCDIFV3_H__
#define __IMX_LCDIFV3_H__

struct lcdifv3_soc;
struct videomode;

struct lcdifv3_client_platformdata {
	struct device_node *of_node;
};

int  lcdifv3_vblank_irq_get(struct lcdifv3_soc *lcdifv3);
void lcdifv3_vblank_irq_enable(struct lcdifv3_soc *lcdifv3);
void lcdifv3_vblank_irq_disable(struct lcdifv3_soc *lcdifv3);
void lcdifv3_vblank_irq_clear(struct lcdifv3_soc *lcdifv3);

int  lcdifv3_get_bus_fmt_from_pix_fmt(struct lcdifv3_soc *lcdifv3,
				    uint32_t format);
int  lcdifv3_set_pix_fmt(struct lcdifv3_soc *lcdifv3, u32 format);
void lcdifv3_set_bus_fmt(struct lcdifv3_soc *lcdifv3, u32 bus_format);
void lcdifv3_set_fb_addr(struct lcdifv3_soc *lcdifv3, int id, u32 addr);
void lcdifv3_set_mode(struct lcdifv3_soc *lcdifv3, struct videomode *vmode);
void lcdifv3_set_pitch(struct lcdifv3_soc *lcdifv3, unsigned int pitch);
void lcdifv3_en_shadow_load(struct lcdifv3_soc *lcdifv3);
bool lcdifv3_shadow_pending(struct lcdifv3_soc *lcdifv3);
u32 lcdifv3_irq_status(struct lcdifv3_soc *lcdifv3);
void lcdifv3_irq_ack(struct lcdifv3_soc *lcdifv3, u32 status);
void lcdifv3_epd_error_irqs(struct lcdifv3_soc *lcdifv3, bool enable);
void lcdifv3_enable_controller(struct lcdifv3_soc *lcdifv3);
void lcdifv3_disable_controller(struct lcdifv3_soc *lcdifv3);
void lcdifv3_dump_registers(struct lcdifv3_soc *lcdifv3);

#endif

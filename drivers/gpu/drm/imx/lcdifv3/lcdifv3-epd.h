/* SPDX-License-Identifier: GPL-2.0 */
/* Experimental HV-off finite queue; no loaded-safe/HV shutdown guarantee. */
struct lcdifv3_epd_plan {
	struct drm_framebuffer *idle, *fb[IMX_EPD_MAX_PHASES];
	u32 addr[IMX_EPD_MAX_PHASES], idle_addr, scans[IMX_EPD_MAX_PHASES];
	u32 phases, total, remaining;
	int active, pending;
	bool started, abort, injected;
	struct file *drm_file, *lock_file;
};

static void lcdifv3_epd_free(struct lcdifv3_crtc *ctx, struct lcdifv3_epd_plan *p)
{
	unsigned int i;
	if (!p)
		return;
	for (i = 0; i < p->phases; i++)
		if (p->fb[i])
			drm_framebuffer_put(p->fb[i]);
	if (p->idle)
		drm_framebuffer_put(p->idle);
	if (p->lock_file)
		fput(p->lock_file);
	if (p->drm_file)
		fput(p->drm_file);
	kfree(p);
}

static void lcdifv3_epd_release_work(struct work_struct *work)
{
	struct lcdifv3_crtc *ctx = container_of(work, struct lcdifv3_crtc, epd_release);
	struct lcdifv3_epd_plan *p;
	unsigned long flags;
	spin_lock_irqsave(&ctx->epd_lock, flags);
	p = ctx->epd_plan;
	ctx->epd_plan = NULL;
	spin_unlock_irqrestore(&ctx->epd_lock, flags);
	cancel_delayed_work_sync(&ctx->epd_watchdog);
	lcdifv3_epd_error_irqs(dev_get_drvdata(ctx->dev->parent), false);
	drm_crtc_vblank_put(&ctx->base);
	lcdifv3_epd_free(ctx, p);
}

static void lcdifv3_epd_program(struct lcdifv3_crtc *ctx, int index)
{
	struct lcdifv3_soc *lcdif = dev_get_drvdata(ctx->dev->parent);
	struct lcdifv3_epd_plan *p = ctx->epd_plan;
	p->pending = index;
	lcdifv3_set_fb_addr(lcdif, 0, index < 0 ? p->idle_addr : p->addr[index]);
	lcdifv3_en_shadow_load(lcdif);
}

static void lcdifv3_epd_finish(struct lcdifv3_crtc *ctx, u32 state)
{
	ctx->epd_state = state;
	WRITE_ONCE(ctx->epd_until, 0);
	schedule_work(&ctx->epd_release);
}

static void lcdifv3_epd_fault(struct lcdifv3_crtc *ctx, int error)
{
	ctx->epd_error = error;
	ctx->epd_valid = false;
	ctx->epd_plan->abort = true;
	lcdifv3_epd_program(ctx, -1);
}

/* Requires epd_lock and the same root DRM master/queue generation. A second
 * explicit DT opt-in restricts this to the disconnected, HV-off bench. */
static int lcdifv3_epd_inject(struct lcdifv3_crtc *ctx, u32 kind)
{
	struct lcdifv3_epd_plan *p = ctx->epd_plan;
	if (!ctx->epd_fault_opt_in || !IS_BUILTIN(CONFIG_DRM_IMX93_PARALLEL_DISPLAY_FORMAT))
		return -EOPNOTSUPP;
	if (!p || ctx->epd_state != IMX_EPD_RUNNING || !p->started || p->active < 0 || p->injected)
		return -EBUSY;
	if (kind != IMX_EPD_INJECT_UNDERRUN && kind != IMX_EPD_INJECT_STALL)
		return -EINVAL;
	p->injected = true;
	if (kind == IMX_EPD_INJECT_UNDERRUN) {
		/* Exercise the same policy as a latched underrun, not physical starvation. */
		lcdifv3_epd_fault(ctx, -EIO);
	} else {
		/* Real interrupt-progress loss; keep DMA running to test the watchdog. */
		ctx->epd_irq_stalled = true;
		ctx->epd_valid = false;
		lcdifv3_vblank_irq_disable(dev_get_drvdata(ctx->dev->parent));
	}
	return 0;
}

/* Called at VS_BLANK, after the old frame's active pixels finished. FPV
 * fetch/shadow load is selected in this BSP. A cleared load bit acknowledges
 * the framebuffer selected for the next active interval. */
static void lcdifv3_epd_tick(struct lcdifv3_crtc *ctx, u32 irq_status)
{
	struct lcdifv3_soc *lcdif = dev_get_drvdata(ctx->dev->parent);
	struct lcdifv3_epd_plan *p;
	unsigned long flags;
	u64 now = ktime_get_ns();
	int old;
	spin_lock_irqsave(&ctx->epd_lock, flags);
	p = ctx->epd_plan;
	/* Also mask an IRQ that was already in flight when injection acquired the lock. */
	if (READ_ONCE(ctx->epd_irq_stalled))
		irq_status &= ~BIT(2);
	if (!p || ctx->epd_state != IMX_EPD_RUNNING)
		goto out;
	if (irq_status & (BIT(1) | BIT(8) | BIT(24))) {
		lcdifv3_epd_fault(ctx, -EIO);
		goto out;
	}
	if (!(irq_status & BIT(2)))
		goto out;
	if (p->started && ctx->epd_last_irq && now - ctx->epd_last_irq > ctx->epd_period * 3 / 2) {
		ctx->epd_error = -ETIME;
		ctx->epd_valid = false;
		p->abort = true;
		lcdifv3_epd_program(ctx, -1);
		ctx->epd_last_irq = now;
		goto out;
	}
	ctx->epd_last_irq = now;
	if (lcdifv3_shadow_pending(lcdif) || READ_ONCE(ctx->pending_event)) {
		if (p->started || now > ctx->epd_arm_time + ctx->epd_period * 3) {
			ctx->epd_error = -ETIMEDOUT;
			ctx->epd_valid = false;
			p->abort = true;
			lcdifv3_epd_program(ctx, -1);
		}
		goto out;
	}
	old = p->active;
	if (p->started && old >= 0 && ctx->epd_valid) {
		ctx->epd_completed++;
		if (p->remaining)
			p->remaining--;
	}
	p->active = p->pending;
	if (p->active < 0) {
		lcdifv3_epd_finish(ctx, ctx->epd_error ? IMX_EPD_FAULT
					: p->abort     ? IMX_EPD_ABORTED
						       : IMX_EPD_DONE);
		goto out;
	}
	if (!p->started || old != p->active)
		p->remaining = p->scans[p->active];
	p->started = true;
	if (p->abort)
		lcdifv3_epd_program(ctx, -1);
	else if (p->remaining > 1)
		lcdifv3_epd_program(ctx, p->active);
	else if (p->active + 1 < p->phases)
		lcdifv3_epd_program(ctx, p->active + 1);
	else
		lcdifv3_epd_program(ctx, -1);
out:
	spin_unlock_irqrestore(&ctx->epd_lock, flags);
}

/* Emergency teardown does not guarantee a whole last scan. Park pins before
 * disabling DMA/display so a frozen output word cannot stay asserted. */
static void lcdifv3_epd_watchdog(struct work_struct *work)
{
	struct lcdifv3_crtc *ctx =
	    container_of(to_delayed_work(work), struct lcdifv3_crtc, epd_watchdog);
	struct drm_device *drm = ctx->base.dev;
	struct lcdifv3_soc *lcdif = dev_get_drvdata(ctx->dev->parent);
	unsigned long flags;
	int ret = -EOPNOTSUPP;
	drm_modeset_lock_all(drm);
	if (READ_ONCE(ctx->epd_until)) {
#if IS_BUILTIN(CONFIG_DRM_IMX93_PARALLEL_DISPLAY_FORMAT)
		ret = imx93_epd_park_outputs(&ctx->base);
#endif
		spin_lock_irqsave(&ctx->epd_lock, flags);
		if (!ctx->epd_error)
			ctx->epd_error = ret ? ret : -ETIMEDOUT;
		ctx->epd_valid = false;
		ctx->epd_forced_stop = true;
		spin_unlock_irqrestore(&ctx->epd_lock, flags);
		lcdifv3_disable_controller(lcdif);
		if (ctx->epd_irq_stalled) {
			ctx->epd_irq_stalled = false;
			lcdifv3_vblank_irq_enable(lcdif);
		}
		lcdifv3_epd_force_stop(ctx);
	}
	drm_modeset_unlock_all(drm);
}

static int lcdifv3_epd_fb(struct drm_device *drm, struct drm_file *owner, u32 id,
			  struct drm_framebuffer **fb, u32 *addr, struct drm_framebuffer *idle)
{
	struct drm_gem_dma_object *gem;
	u64 address;
	*fb = drm_framebuffer_lookup(drm, owner, id);
	if (!*fb)
		return -ENOENT;
	if ((*fb)->format->format != DRM_FORMAT_XRGB8888 || (*fb)->width != 432 ||
	    (*fb)->height != 1083 || (*fb)->pitches[0] != idle->pitches[0])
		return -EINVAL;
	gem = drm_fb_dma_get_gem_obj(*fb, 0);
	if (!gem)
		return -EINVAL;
	if ((u64)(*fb)->offsets[0] + (u64)(*fb)->pitches[0] * (*fb)->height > gem->base.size)
		return -EINVAL;
	address = gem->dma_addr + (*fb)->offsets[0];
	if (address > U32_MAX ||
	    address + (u64)(*fb)->pitches[0] * (*fb)->height > (u64)U32_MAX + 1)
		return -ERANGE;
	*addr = address;
	return 0;
}

int lcdifv3_epd_ioctl(struct drm_device *drm, void *data, struct drm_file *owner)
{
	struct drm_imx_epd_sequence *arg = data;
	struct drm_imx_epd_phase *phases = NULL;
	struct lcdifv3_crtc *ctx = NULL;
	struct lcdifv3_epd_plan *p = NULL;
	struct drm_crtc *crtc;
	struct lcdifv3_soc *lcdif;
	struct drm_display_mode *mode;
	unsigned long flags;
	int ret = 0, i;
	if (arg->version != IMX_EPD_VERSION || arg->flags || arg->command > IMX_EPD_INJECT)
		return -EINVAL;
	drm_for_each_crtc(crtc, drm)
	{
		if (crtc->base.id == arg->crtc_id && crtc->funcs == &lcdifv3_crtc_funcs) {
			ctx = to_lcdifv3_crtc(crtc);
			break;
		}
	}
	if (!ctx || !ctx->epd_opt_in)
		return -EOPNOTSUPP;
	if (arg->command == IMX_EPD_CAPS) {
		arg->num_phases = IMX_EPD_MAX_PHASES;
		arg->completed_scans = IMX_EPD_MAX_SCANS;
		arg->flags =
		    ctx->epd_fault_opt_in && IS_BUILTIN(CONFIG_DRM_IMX93_PARALLEL_DISPLAY_FORMAT)
			? IMX_EPD_CAP_FAULT_TESTS
			: 0;
		return 0;
	}
	if (arg->command != IMX_EPD_SUBMIT) {
		spin_lock_irqsave(&ctx->epd_lock, flags);
		if (arg->generation != ctx->epd_generation || owner->client_id != ctx->epd_owner)
			ret = -ESTALE;
		else if (arg->command == IMX_EPD_ABORT && ctx->epd_plan &&
			 ctx->epd_state == IMX_EPD_RUNNING)
			ctx->epd_plan->abort = true;
		else if (arg->command == IMX_EPD_INJECT)
			ret = lcdifv3_epd_inject(ctx, arg->num_phases);
		goto report;
	}
	if (!arg->num_phases || arg->num_phases > IMX_EPD_MAX_PHASES || arg->lock_fd < 0)
		return -EINVAL;
	/* Too large for the stack at the full phase count. */
	phases = memdup_array_user(u64_to_user_ptr(arg->phases_ptr), arg->num_phases,
				   sizeof(*phases));
	if (IS_ERR(phases))
		return PTR_ERR(phases);
	p = kzalloc(sizeof(*p), GFP_KERNEL);
	if (!p) {
		kfree(phases);
		return -ENOMEM;
	}
	p->phases = arg->num_phases;
	p->active = -1;
	p->lock_file = fget(arg->lock_fd);
	if (!p->lock_file || !S_ISREG(file_inode(p->lock_file)->i_mode)) {
		ret = -EINVAL;
		goto free;
	}
	drm_modeset_lock_all(drm);
	lcdif = dev_get_drvdata(ctx->dev->parent);
	mode = &ctx->base.state->adjusted_mode;
	if (!ctx->base.state->active || !ctx->base.primary->state->fb ||
	    ctx->base.primary->state->fb->base.id != arg->idle_fb_id || mode->clock != 30000 ||
	    mode->hdisplay != 432 || mode->vdisplay != 1083 || mode->htotal != 435 ||
	    mode->vtotal != 1086 || lcdifv3_shadow_pending(lcdif) ||
	    READ_ONCE(ctx->pending_event)) {
		ret = -EINVAL;
		goto unlock;
	}
	p->idle = drm_framebuffer_lookup(drm, owner, arg->idle_fb_id);
	if (!p->idle) {
		ret = -ENOENT;
		goto unlock;
	}
	ret = lcdifv3_epd_fb(drm, owner, arg->idle_fb_id, &p->fb[0], &p->idle_addr, p->idle);
	if (ret)
		goto unlock;
	drm_framebuffer_put(p->fb[0]);
	p->fb[0] = NULL;
	for (i = 0; i < p->phases; i++) {
		if (!phases[i].scans || phases[i].scans > IMX_EPD_MAX_SCANS ||
		    phases[i].fb_id == arg->idle_fb_id) {
			ret = -EINVAL;
			goto unlock;
		}
		p->scans[i] = phases[i].scans;
		p->total += phases[i].scans;
		if (p->total > IMX_EPD_MAX_SCANS) {
			ret = -E2BIG;
			goto unlock;
		}
		ret = lcdifv3_epd_fb(drm, owner, phases[i].fb_id, &p->fb[i], &p->addr[i], p->idle);
		if (ret)
			goto unlock;
	}
	ret = drm_crtc_vblank_get(&ctx->base);
	if (ret)
		goto unlock;
	spin_lock_irqsave(&ctx->epd_lock, flags);
	if (ctx->epd_plan || work_busy(&ctx->epd_release)) {
		spin_unlock_irqrestore(&ctx->epd_lock, flags);
		drm_crtc_vblank_put(&ctx->base);
		ret = -EBUSY;
		goto unlock;
	}
	get_file(owner->filp);
	p->drm_file = owner->filp;
	ctx->epd_plan = p;
	p = NULL;
	ctx->epd_generation++;
	ctx->epd_owner = owner->client_id;
	ctx->epd_state = IMX_EPD_RUNNING;
	ctx->epd_completed = 0;
	ctx->epd_error = 0;
	ctx->epd_valid = true;
	ctx->epd_forced_stop = false;
	ctx->epd_irq_stalled = false;
	ctx->epd_period = 15747000;
	ctx->epd_last_irq = 0;
	ctx->epd_arm_time = ktime_get_ns();
	WRITE_ONCE(ctx->epd_until,
		   ctx->epd_arm_time + ctx->epd_period * (ctx->epd_plan->total + 3) + 500000000);
	lcdifv3_epd_error_irqs(lcdif, true);
	lcdifv3_epd_program(ctx, 0);
	mod_delayed_work(
	    system_wq, &ctx->epd_watchdog,
	    nsecs_to_jiffies(ctx->epd_period * (ctx->epd_plan->total + 3) + 500000000));
	ret = 0;
report:
	arg->generation = ctx->epd_generation;
	arg->status = ctx->epd_state;
	arg->flags =
	    (ctx->epd_plan != NULL || work_busy(&ctx->epd_release) ? IMX_EPD_REFS_HELD : 0) |
	    (ctx->epd_forced_stop ? IMX_EPD_CONTROLLER_STOPPED : 0);
	arg->completed_scans = ctx->epd_completed;
	arg->error = ctx->epd_error;
	arg->count_valid = ctx->epd_valid;
	arg->phase_index = ctx->epd_plan ? ctx->epd_plan->active : -1;
	spin_unlock_irqrestore(&ctx->epd_lock, flags);
	if (arg->command != IMX_EPD_SUBMIT)
		return ret;
unlock:
	drm_modeset_unlock_all(drm);
free:
	lcdifv3_epd_free(ctx, p);
	kfree(phases);
	return ret;
}
EXPORT_SYMBOL_GPL(lcdifv3_epd_ioctl);

u64 lcdifv3_epd_deadline_ns(struct drm_crtc *crtc)
{
	if (!crtc || crtc->funcs != &lcdifv3_crtc_funcs)
		return 0;
	return READ_ONCE(to_lcdifv3_crtc(crtc)->epd_until);
}
EXPORT_SYMBOL_GPL(lcdifv3_epd_deadline_ns);

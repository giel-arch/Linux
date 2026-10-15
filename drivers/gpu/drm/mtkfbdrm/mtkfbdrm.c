// SPDX-License-Identifier: GPL-2.0
/*
 * mtkfbdrm - Minimal DRM/KMS device on top of the MTK legacy framebuffer.
 *
 * The MT6768 ships only the legacy "mtkfb" fbdev driver and no DRM/KMS
 * support.  Userspace compositors (phosh/mutter) require DRM.  This driver
 * exposes a small KMS device whose scanout is the mtkfb framebuffer itself:
 * every frame the current DRM framebuffer is memcpy'd into the mtkfb scanout
 * memory, which is known to be displayed by the vendor display pipeline.
 *
 * Layout follows drivers/gpu/drm/vkms (timer-driven vblank) with GEM CMA
 * backing memory so that dumb buffers and software rendering
 * (mesa kms_swrast / llvmpipe) work.
 */

#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/hrtimer.h>
#include <linux/workqueue.h>
#include <linux/io.h>
#include <linux/delay.h>

#include <drm/drmP.h>
#include <drm/drm_atomic_helper.h>
#include <drm/drm_crtc_helper.h>
#include <drm/drm_fb_cma_helper.h>
#include <drm/drm_gem.h>
#include <drm/drm_gem_cma_helper.h>
#include <drm/drm_gem_framebuffer_helper.h>
#include <drm/drm_plane_helper.h>

#define DRIVER_NAME	"mtkfbdrm"
#define DRIVER_DESC	"DRM KMS over MTK legacy framebuffer"
#define DRIVER_DATE	"20261014"
#define DRIVER_MAJOR	1
#define DRIVER_MINOR	0

/* A38 panel; matches the mtkfb fb0 geometry */
#define MTKFBDRM_XRES	720
#define MTKFBDRM_YRES	1612
#define MTKFBDRM_REFRESH 60

extern int mtkfb_get_scanout_info(u64 *pa, u32 *stride, size_t *size);

static const u32 mtkfbdrm_formats[] = {
	DRM_FORMAT_XRGB8888,
};

struct mtkfbdrm_output {
	struct drm_crtc crtc;
	struct drm_encoder encoder;
	struct drm_connector connector;
	struct hrtimer vblank_hrtimer;
	ktime_t period_ns;
	bool enabled;
};

struct mtkfbdrm_device {
	struct drm_device drm;
	struct platform_device *platform;
	struct mtkfbdrm_output output;
	struct drm_fbdev_cma *fbdev;

	/* scanout target = mtkfb fb0 memory */
	struct delayed_work refresh_work;
	struct delayed_work find_work;
	void __iomem *scanout;
	size_t scanout_len;
	u32 scanout_stride;
	bool scanout_ready;
};

#define drm_crtc_to_mtkfbdrm_output(target) \
	container_of(target, struct mtkfbdrm_output, crtc)

#define drm_device_to_mtkfbdrm(target) \
	container_of(target, struct mtkfbdrm_device, drm)

/* ------------------------------------------------------------------ */
/* scanout copy                                                        */

static void mtkfbdrm_copy_fb(struct drm_framebuffer *fb)
{
	struct mtkfbdrm_device *mtk = container_of((void *)fb->dev,
			struct mtkfbdrm_device, drm);
	struct drm_gem_cma_object *cma;
	void *src;
	unsigned int y, lines, width;

	if (!mtk->scanout_ready || !fb)
		return;

	cma = to_drm_gem_cma_obj(fb->obj[0]);
	if (!cma || !cma->vaddr)
		return;

	src = cma->vaddr;
	width = min_t(u32, fb->width, MTKFBDRM_XRES);
	lines = min_t(u32, fb->height, MTKFBDRM_YRES);

	for (y = 0; y < lines; y++) {
		memcpy((void *)mtk->scanout + y * mtk->scanout_stride,
		       src + y * fb->pitches[0],
		       width * 4);
	}
}

static void mtkfbdrm_refresh_work(struct work_struct *work)
{
	struct mtkfbdrm_device *mtk = container_of(work,
			struct mtkfbdrm_device, refresh_work.work);
	struct drm_plane *plane = mtk->output.crtc.primary;
	struct drm_framebuffer *fb;

	if (!mtk->output.enabled) {
		schedule_delayed_work(&mtk->refresh_work,
				      msecs_to_jiffies(200));
		return;
	}

	drm_modeset_lock(&plane->mutex, NULL);
	fb = plane->state ? plane->state->fb : NULL;
	if (fb)
		drm_framebuffer_get(fb);
	drm_modeset_unlock(&plane->mutex);

	if (fb) {
		mtkfbdrm_copy_fb(fb);
		drm_framebuffer_put(fb);
	}

	schedule_delayed_work(&mtk->refresh_work, msecs_to_jiffies(33));
}

/* ------------------------------------------------------------------ */
/* scanout discovery                                                   */

static void mtkfbdrm_find_scanout_work(struct work_struct *work)
{
	struct mtkfbdrm_device *mtk = container_of(work,
			struct mtkfbdrm_device, find_work.work);
	u64 pa = 0;
	u32 stride = 0;
	size_t size = 0;
	int ret;

	ret = mtkfb_get_scanout_info(&pa, &stride, &size);
	if (ret || !pa || !size) {
		schedule_delayed_work(&mtk->find_work,
				      msecs_to_jiffies(500));
		return;
	}

	mtk->scanout = ioremap_wc(pa, size);
	if (!mtk->scanout) {
		DRM_ERROR("mtkfbdrm: cannot ioremap scanout 0x%llx\n", pa);
		return;
	}
	mtk->scanout_stride = stride;
	mtk->scanout_len = size;
	mtk->scanout_ready = true;
	DRM_INFO("mtkfbdrm: scanout 0x%llx stride=%u size=%zu ready\n",
		 pa, stride, size);

	schedule_delayed_work(&mtk->refresh_work, msecs_to_jiffies(33));
}

/* ------------------------------------------------------------------ */
/* crtc + vblank (vkms pattern)                                        */

static enum hrtimer_restart mtkfbdrm_vblank_simulate(struct hrtimer *timer)
{
	struct mtkfbdrm_output *output = container_of(timer,
			struct mtkfbdrm_output, vblank_hrtimer);
	struct drm_crtc *crtc = &output->crtc;

	drm_crtc_handle_vblank(crtc);
	hrtimer_forward_now(&output->vblank_hrtimer, output->period_ns);

	return HRTIMER_RESTART;
}

static int mtkfbdrm_enable_vblank(struct drm_crtc *crtc)
{
	struct drm_device *dev = crtc->dev;
	unsigned int pipe = drm_crtc_index(crtc);
	struct drm_vblank_crtc *vblank = &dev->vblank[pipe];
	struct mtkfbdrm_output *out = drm_crtc_to_mtkfbdrm_output(crtc);

	drm_calc_timestamping_constants(crtc, &crtc->mode);

	hrtimer_init(&out->vblank_hrtimer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
	out->vblank_hrtimer.function = &mtkfbdrm_vblank_simulate;
	out->period_ns = ktime_set(0, vblank->framedur_ns);
	hrtimer_start(&out->vblank_hrtimer, out->period_ns, HRTIMER_MODE_REL);

	return 0;
}

static void mtkfbdrm_disable_vblank(struct drm_crtc *crtc)
{
	struct mtkfbdrm_output *out = drm_crtc_to_mtkfbdrm_output(crtc);

	hrtimer_cancel(&out->vblank_hrtimer);
}

static bool mtkfbdrm_get_vblank_timestamp(struct drm_device *dev,
					  unsigned int pipe, int *max_error,
					  ktime_t *vblank_time,
					  bool in_vblank_irq)
{
	struct mtkfbdrm_device *mtk = drm_device_to_mtkfbdrm(dev);
	struct mtkfbdrm_output *output = &mtk->output;

	*vblank_time = output->vblank_hrtimer.node.expires;
	if (!in_vblank_irq)
		*vblank_time -= output->period_ns;

	return true;
}

static const struct drm_crtc_funcs mtkfbdrm_crtc_funcs = {
	.set_config		= drm_atomic_helper_set_config,
	.destroy		= drm_crtc_cleanup,
	.page_flip		= drm_atomic_helper_page_flip,
	.reset			= drm_atomic_helper_crtc_reset,
	.atomic_duplicate_state	= drm_atomic_helper_crtc_duplicate_state,
	.atomic_destroy_state	= drm_atomic_helper_crtc_destroy_state,
	.enable_vblank		= mtkfbdrm_enable_vblank,
	.disable_vblank		= mtkfbdrm_disable_vblank,
};

static void mtkfbdrm_crtc_atomic_enable(struct drm_crtc *crtc,
					struct drm_crtc_state *old_state)
{
	struct mtkfbdrm_device *mtk = drm_device_to_mtkfbdrm(crtc->dev);

	mtk->output.enabled = true;
	drm_crtc_vblank_on(crtc);
}

static void mtkfbdrm_crtc_atomic_disable(struct drm_crtc *crtc,
					 struct drm_crtc_state *old_state)
{
	struct mtkfbdrm_device *mtk = drm_device_to_mtkfbdrm(crtc->dev);

	mtk->output.enabled = false;
	drm_crtc_vblank_off(crtc);
}

static void mtkfbdrm_crtc_atomic_flush(struct drm_crtc *crtc,
				       struct drm_crtc_state *old_state)
{
	unsigned long flags;

	if (crtc->state->event) {
		spin_lock_irqsave(&crtc->dev->event_lock, flags);
		if (drm_crtc_vblank_get(crtc) != 0)
			drm_crtc_send_vblank_event(crtc, crtc->state->event);
		else
			drm_crtc_arm_vblank_event(crtc, crtc->state->event);
		spin_unlock_irqrestore(&crtc->dev->event_lock, flags);
		crtc->state->event = NULL;
	}
}

static const struct drm_crtc_helper_funcs mtkfbdrm_crtc_helper_funcs = {
	.atomic_flush	= mtkfbdrm_crtc_atomic_flush,
	.atomic_enable	= mtkfbdrm_crtc_atomic_enable,
	.atomic_disable	= mtkfbdrm_crtc_atomic_disable,
};

static int mtkfbdrm_crtc_init(struct drm_device *dev, struct drm_crtc *crtc,
			      struct drm_plane *primary)
{
	int ret;

	ret = drm_crtc_init_with_planes(dev, crtc, primary, NULL,
					&mtkfbdrm_crtc_funcs, NULL);
	if (ret) {
		DRM_ERROR("Failed to init CRTC\n");
		return ret;
	}
	drm_crtc_helper_add(crtc, &mtkfbdrm_crtc_helper_funcs);

	return ret;
}

/* ------------------------------------------------------------------ */
/* plane                                                               */

static const struct drm_plane_funcs mtkfbdrm_plane_funcs = {
	.update_plane		= drm_atomic_helper_update_plane,
	.disable_plane		= drm_atomic_helper_disable_plane,
	.destroy		= drm_plane_cleanup,
	.reset			= drm_atomic_helper_plane_reset,
	.atomic_duplicate_state	= drm_atomic_helper_plane_duplicate_state,
	.atomic_destroy_state	= drm_atomic_helper_plane_destroy_state,
};

static void mtkfbdrm_plane_atomic_update(struct drm_plane *plane,
					 struct drm_plane_state *old_state)
{
	/* the refresh worker picks up the new fb */
}

static const struct drm_plane_helper_funcs mtkfbdrm_plane_helper_funcs = {
	.atomic_update = mtkfbdrm_plane_atomic_update,
};

static struct drm_plane *mtkfbdrm_plane_init(struct mtkfbdrm_device *mtk)
{
	struct drm_device *dev = &mtk->drm;
	struct drm_plane *plane;
	int ret;

	plane = kzalloc(sizeof(*plane), GFP_KERNEL);
	if (!plane)
		return ERR_PTR(-ENOMEM);

	ret = drm_universal_plane_init(dev, plane, 0,
				       &mtkfbdrm_plane_funcs,
				       mtkfbdrm_formats,
				       ARRAY_SIZE(mtkfbdrm_formats),
				       NULL, DRM_PLANE_TYPE_PRIMARY, NULL);
	if (ret) {
		kfree(plane);
		return ERR_PTR(ret);
	}

	drm_plane_helper_add(plane, &mtkfbdrm_plane_helper_funcs);

	return plane;
}

/* ------------------------------------------------------------------ */
/* connector + encoder                                                 */

static void mtkfbdrm_connector_destroy(struct drm_connector *connector)
{
	drm_connector_unregister(connector);
	drm_connector_cleanup(connector);
}

static const struct drm_connector_funcs mtkfbdrm_connector_funcs = {
	.fill_modes = drm_helper_probe_single_connector_modes,
	.destroy = mtkfbdrm_connector_destroy,
	.reset = drm_atomic_helper_connector_reset,
	.atomic_duplicate_state = drm_atomic_helper_connector_duplicate_state,
	.atomic_destroy_state = drm_atomic_helper_connector_destroy_state,
};

static int mtkfbdrm_conn_get_modes(struct drm_connector *connector)
{
	struct drm_display_mode *mode;

	mode = drm_mode_create(connector->dev);
	if (!mode)
		return 0;

	mode->type = DRM_MODE_TYPE_PREFERRED;
	mode->clock = 69638;
	mode->hdisplay = MTKFBDRM_XRES;
	mode->hsync_start = MTKFBDRM_XRES + 8;
	mode->hsync_end = MTKFBDRM_XRES + 48;
	mode->htotal = MTKFBDRM_XRES + 80;
	mode->vdisplay = MTKFBDRM_YRES;
	mode->vsync_start = MTKFBDRM_YRES + 4;
	mode->vsync_end = MTKFBDRM_YRES + 12;
	mode->vtotal = MTKFBDRM_YRES + 22;
	mode->vrefresh = MTKFBDRM_REFRESH;

	drm_mode_probed_add(connector, mode);

	connector->display_info.width_mm = 66;
	connector->display_info.height_mm = 148;

	return 1;
}

static enum drm_mode_status mtkfbdrm_conn_mode_valid(
		struct drm_connector *connector, struct drm_display_mode *mode)
{
	if (mode->hdisplay != MTKFBDRM_XRES || mode->vdisplay != MTKFBDRM_YRES)
		return MODE_BAD;

	return MODE_OK;
}

static const struct drm_encoder_funcs mtkfbdrm_encoder_funcs = {
	.destroy = drm_encoder_cleanup,
};

static const struct drm_connector_helper_funcs mtkfbdrm_conn_helper_funcs = {
	.get_modes = mtkfbdrm_conn_get_modes,
	.mode_valid = mtkfbdrm_conn_mode_valid,
};

/* ------------------------------------------------------------------ */
/* driver                                                              */

static const struct file_operations mtkfbdrm_driver_fops = {
	.owner		= THIS_MODULE,
	.open		= drm_open,
	.mmap		= drm_gem_cma_mmap,
	.unlocked_ioctl	= drm_ioctl,
	.compat_ioctl	= drm_compat_ioctl,
	.poll		= drm_poll,
	.read		= drm_read,
	.llseek		= no_llseek,
	.release	= drm_release,
};

static void mtkfbdrm_release(struct drm_device *dev)
{
	struct mtkfbdrm_device *mtk = drm_device_to_mtkfbdrm(dev);

	cancel_delayed_work_sync(&mtk->refresh_work);
	cancel_delayed_work_sync(&mtk->find_work);
	if (mtk->scanout)
		iounmap(mtk->scanout);
	drm_mode_config_cleanup(&mtk->drm);
	drm_dev_fini(&mtk->drm);
}

static struct drm_driver mtkfbdrm_driver = {
	.driver_features	= DRIVER_MODESET | DRIVER_ATOMIC | DRIVER_GEM |
				  DRIVER_PRIME,
	.release		= mtkfbdrm_release,
	.fops			= &mtkfbdrm_driver_fops,
	.gem_free_object_unlocked = drm_gem_cma_free_object,
	.gem_vm_ops		= &drm_gem_cma_vm_ops,
	.dumb_create		= drm_gem_cma_dumb_create,
	.prime_handle_to_fd	= drm_gem_prime_handle_to_fd,
	.prime_fd_to_handle	= drm_gem_prime_fd_to_handle,
	.gem_prime_import	= drm_gem_prime_import,
	.gem_prime_export	= drm_gem_prime_export,
	.gem_prime_get_sg_table	= drm_gem_cma_prime_get_sg_table,
	.gem_prime_import_sg_table = drm_gem_cma_prime_import_sg_table,
	.gem_prime_mmap		= drm_gem_cma_prime_mmap,
	.get_vblank_timestamp	= mtkfbdrm_get_vblank_timestamp,

	.name			= DRIVER_NAME,
	.desc			= DRIVER_DESC,
	.date			= DRIVER_DATE,
	.major			= DRIVER_MAJOR,
	.minor			= DRIVER_MINOR,
};

static const struct drm_mode_config_funcs mtkfbdrm_mode_funcs = {
	.fb_create = drm_gem_fb_create,
	.atomic_check = drm_atomic_helper_check,
	.atomic_commit = drm_atomic_helper_commit,
};

static int mtkfbdrm_output_init(struct mtkfbdrm_device *mtk)
{
	struct mtkfbdrm_output *output = &mtk->output;
	struct drm_device *dev = &mtk->drm;
	struct drm_connector *connector = &output->connector;
	struct drm_encoder *encoder = &output->encoder;
	struct drm_crtc *crtc = &output->crtc;
	struct drm_plane *primary;
	int ret;

	primary = mtkfbdrm_plane_init(mtk);
	if (IS_ERR(primary))
		return PTR_ERR(primary);

	ret = mtkfbdrm_crtc_init(dev, crtc, primary);
	if (ret)
		goto err_crtc;

	ret = drm_connector_init(dev, connector, &mtkfbdrm_connector_funcs,
				 DRM_MODE_CONNECTOR_VIRTUAL);
	if (ret) {
		DRM_ERROR("Failed to init connector\n");
		goto err_connector;
	}

	drm_connector_helper_add(connector, &mtkfbdrm_conn_helper_funcs);

	ret = drm_connector_register(connector);
	if (ret) {
		DRM_ERROR("Failed to register connector\n");
		goto err_connector_register;
	}

	ret = drm_encoder_init(dev, encoder, &mtkfbdrm_encoder_funcs,
			       DRM_MODE_ENCODER_VIRTUAL, NULL);
	if (ret) {
		DRM_ERROR("Failed to init encoder\n");
		goto err_encoder;
	}
	encoder->possible_crtcs = 1;

	ret = drm_connector_attach_encoder(connector, encoder);
	if (ret) {
		DRM_ERROR("Failed to attach connector to encoder\n");
		goto err_attach;
	}

	drm_mode_config_reset(dev);

	return 0;

err_attach:
	drm_encoder_cleanup(encoder);
err_encoder:
	drm_connector_unregister(connector);
err_connector_register:
	drm_connector_cleanup(connector);
err_connector:
	drm_crtc_cleanup(crtc);
err_crtc:
	drm_plane_cleanup(primary);
	return ret;
}

static int mtkfbdrm_modeset_init(struct mtkfbdrm_device *mtk)
{
	struct drm_device *dev = &mtk->drm;
	int ret;

	drm_mode_config_init(dev);
	dev->mode_config.funcs = &mtkfbdrm_mode_funcs;
	dev->mode_config.min_width = 32;
	dev->mode_config.min_height = 32;
	dev->mode_config.max_width = MTKFBDRM_XRES;
	dev->mode_config.max_height = MTKFBDRM_YRES;

	ret = mtkfbdrm_output_init(mtk);
	if (ret)
		drm_mode_config_cleanup(dev);

	return ret;
}

static int mtkfbdrm_probe(struct platform_device *pdev)
{
	struct mtkfbdrm_device *mtk;
	int ret;

	mtk = kzalloc(sizeof(*mtk), GFP_KERNEL);
	if (!mtk)
		return -ENOMEM;

	mtk->platform = pdev;
	platform_set_drvdata(pdev, mtk);

	ret = drm_dev_init(&mtk->drm, &mtkfbdrm_driver, &pdev->dev);
	if (ret) {
		kfree(mtk);
		return ret;
	}

	INIT_DELAYED_WORK(&mtk->refresh_work, mtkfbdrm_refresh_work);
	INIT_DELAYED_WORK(&mtk->find_work, mtkfbdrm_find_scanout_work);

	ret = mtkfbdrm_modeset_init(mtk);
	if (ret)
		goto err_fini;

	ret = drm_dev_register(&mtk->drm, 0);
	if (ret)
		goto err_cleanup;

	/* fbcon on top of the DRM device */
	mtk->fbdev = drm_fb_cma_fbdev_init(&mtk->drm, 32, 1);

	/* start waiting for the mtkfb scanout to appear */
	schedule_delayed_work(&mtk->find_work, msecs_to_jiffies(200));

	DRM_INFO("mtkfbdrm: initialized\n");

	return 0;

err_cleanup:
	drm_mode_config_cleanup(&mtk->drm);
err_fini:
	drm_dev_fini(&mtk->drm);
	kfree(mtk);
	return ret;
}

static int mtkfbdrm_remove(struct platform_device *pdev)
{
	struct mtkfbdrm_device *mtk = platform_get_drvdata(pdev);

	drm_atomic_helper_shutdown(&mtk->drm);
	drm_fb_cma_fbdev_fini(&mtk->drm);
	drm_dev_unregister(&mtk->drm);
	return 0;
}

static struct platform_driver mtkfbdrm_platform_driver = {
	.probe	= mtkfbdrm_probe,
	.remove	= mtkfbdrm_remove,
	.driver	= {
		.name = DRIVER_NAME,
	},
};

static struct platform_device *mtkfbdrm_pdev;

static int __init mtkfbdrm_init(void)
{
	int ret;

	ret = platform_driver_register(&mtkfbdrm_platform_driver);
	if (ret)
		return ret;

	mtkfbdrm_pdev = platform_device_register_simple(DRIVER_NAME, -1,
							NULL, 0);
	if (IS_ERR(mtkfbdrm_pdev)) {
		platform_driver_unregister(&mtkfbdrm_platform_driver);
		return PTR_ERR(mtkfbdrm_pdev);
	}

	return 0;
}

static void __exit mtkfbdrm_exit(void)
{
	platform_device_unregister(mtkfbdrm_pdev);
	platform_driver_unregister(&mtkfbdrm_platform_driver);
}

module_init(mtkfbdrm_init);
module_exit(mtkfbdrm_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION(DRIVER_DESC);
MODULE_AUTHOR("postmarketOS port");

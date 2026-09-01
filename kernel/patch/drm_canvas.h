/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __DRM_CANVAS_H__
#define __DRM_CANVAS_H__

#include <linux/errno.h>
#include <linux/bits.h>
#include <linux/kconfig.h>
#include <linux/types.h>

struct drm_framebuffer;

enum drm_canvas_command_type {
	DRM_CANVAS_FILL,
	DRM_CANVAS_MONO,
	DRM_CANVAS_COPY,
};

#define DRM_CANVAS_MONO_TRANSPARENT BIT(0)

/*
 * A deliberately small kernel-compositor ABI.  Coordinates are framebuffer
 * coordinates and the list is executed in order.  MONO expands a kernel
 * bitmap, most-significant bit first, directly from the command stream.
 */
struct drm_canvas_command {
	const u8 *bits;
	u32 foreground;
	u32 background;
	s16 x;
	s16 y;
	u16 width;
	u16 height;
	u16 stride;
	u8 scale;
	u8 type;
	u8 flags;
	u8 reserved;
};

#if IS_REACHABLE(CONFIG_DRM_I915)
int drm_i915_canvas_probe(struct drm_framebuffer *fb);
int drm_i915_canvas_render(struct drm_framebuffer *fb,
			   const struct drm_canvas_command *commands,
			   unsigned int count);
#else
static inline int drm_i915_canvas_probe(struct drm_framebuffer *fb)
{
	return -EOPNOTSUPP;
}

static inline int
drm_i915_canvas_render(struct drm_framebuffer *fb,
		       const struct drm_canvas_command *commands,
		       unsigned int count)
{
	return -EOPNOTSUPP;
}
#endif

#endif

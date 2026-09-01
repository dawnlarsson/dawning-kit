// SPDX-License-Identifier: GPL-2.0
/*
 * The hardware floor for Moonwater Canvas.
 *
 * Canvas owns KMS and supplies an ordered display list.  This file lives in
 * i915 so no private driver object crosses the module boundary: it turns the
 * primitives Canvas actually needs into copy-engine ring commands and
 * lets the normal i915 VMA, reservation and frontbuffer machinery own their
 * lifetime.
 */

#include <drm/drm_canvas.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_framebuffer.h>
#include <drm/drm_gem_framebuffer_helper.h>

#include "gem/i915_gem_object.h"
#include "gt/intel_engine.h"
#include "gt/intel_engine_pm.h"
#include "gt/intel_gpu_commands.h"
#include "gt/intel_gt.h"
#include "gt/intel_ring.h"
#include "i915_drv.h"
#include "i915_vma.h"

#define CANVAS_MONO_BYTES 128
/* kernel_context has a 1024-dword ring; leave a quarter for finalisation. */
#define CANVAS_REQUEST_DWORDS 768
#define XY_MONO_SRC_COPY_IMM_CMD (2u << 29 | 0x71u << 22)

static int canvas_target(struct drm_framebuffer *fb,
			 struct drm_i915_private **i915_out,
			 struct drm_i915_gem_object **obj_out,
			 struct intel_engine_cs **engine_out)
{
	struct drm_i915_private *i915;
	struct drm_i915_gem_object *obj;
	struct drm_gem_object *gem;
	struct intel_engine_cs *engine;

	if (!fb || !fb->dev || !fb->dev->driver ||
	    strcmp(fb->dev->driver->name, "i915"))
		return -EOPNOTSUPP;

	if (fb->format->format != DRM_FORMAT_XRGB8888 &&
	    fb->format->format != DRM_FORMAT_ARGB8888)
		return -EOPNOTSUPP;

	if (fb->modifier != DRM_FORMAT_MOD_LINEAR || fb->pitches[0] > S16_MAX)
		return -EOPNOTSUPP;

	i915 = to_i915(fb->dev);
	if (GRAPHICS_VER(i915) < 6 ||
	    GRAPHICS_VER_FULL(i915) >= IP_VER(12, 55))
		return -EOPNOTSUPP;

	gem = drm_gem_fb_get_obj(fb, 0);
	if (!gem)
		return -EINVAL;

	obj = to_intel_bo(gem);
	if (i915_gem_object_is_tiled(obj))
		return -EOPNOTSUPP;

	engine = to_gt(i915)->engine_class[COPY_ENGINE_CLASS][0];
	if (!engine || !engine->kernel_context)
		return -EOPNOTSUPP;

	*i915_out = i915;
	*obj_out = obj;
	*engine_out = engine;
	return 0;
}

int drm_i915_canvas_probe(struct drm_framebuffer *fb)
{
	struct drm_i915_private *i915;
	struct drm_i915_gem_object *obj;
	struct intel_engine_cs *engine;

	return canvas_target(fb, &i915, &obj, &engine);
}
EXPORT_SYMBOL_GPL(drm_i915_canvas_probe);

static int canvas_validate(const struct drm_framebuffer *fb,
			   const struct drm_canvas_command *command)
{
	u32 width = command->width;
	u32 height = command->height;

	if (command->x < 0 || command->y < 0 || !width || !height ||
	    command->reserved ||
	    command->x + width > fb->width || command->y + height > fb->height)
		return -EINVAL;

	if (command->type == DRM_CANVAS_FILL)
		return command->flags ? -EINVAL : 0;
	if (command->type == DRM_CANVAS_COPY)
		return !command->bits || !command->stride || command->scale != 1 ||
		       command->stride < width || command->flags ? -EINVAL : 0;

	if (command->type != DRM_CANVAS_MONO || !command->bits ||
	    !command->stride || !command->scale ||
	    command->flags & ~DRM_CANVAS_MONO_TRANSPARENT)
		return -EINVAL;

	width *= command->scale;
	height *= command->scale;
	if (command->x + width > fb->width || command->y + height > fb->height ||
	    DIV_ROUND_UP(width, 16) * 2 * height > CANVAS_MONO_BYTES)
		return -E2BIG;

	return 0;
}

static size_t canvas_stage_bytes(const struct drm_canvas_command *command)
{
	if (command->type != DRM_CANVAS_COPY)
		return 0;

	return ALIGN((size_t)command->width * sizeof(u32) * command->height,
		     L1_CACHE_BYTES);
}

static struct drm_i915_gem_object *
canvas_stage(struct drm_i915_private *i915,
	     const struct drm_canvas_command *commands, unsigned int count)
{
	struct drm_i915_gem_object *obj;
	size_t bytes = 0, at = 0;
	u32 *map;
	unsigned int i;

	for (i = 0; i < count; i++) {
		size_t add = canvas_stage_bytes(&commands[i]);

		if (add > SIZE_MAX - bytes)
			return ERR_PTR(-EOVERFLOW);
		bytes += add;
	}
	if (!bytes)
		return NULL;

	obj = i915_gem_object_create_shmem(i915, PAGE_ALIGN(bytes));
	if (IS_ERR(obj))
		return obj;

	map = i915_gem_object_pin_map_unlocked(obj, I915_MAP_WC);
	if (IS_ERR(map)) {
		i915_gem_object_put(obj);
		return ERR_CAST(map);
	}

	for (i = 0; i < count; i++) {
		const struct drm_canvas_command *command = &commands[i];
		const u32 *source;
		u32 *to;
		unsigned int x, y;

		if (command->type != DRM_CANVAS_COPY)
			continue;

		source = (const u32 *)command->bits;
		to = (u32 *)((u8 *)map + at);
		for (y = 0; y < command->height; y++) {
			if (!command->background) {
				memcpy(to, source,
				       command->width * sizeof(*source));
			} else {
				for (x = 0; x < command->width; x++)
					to[x] = source[x] | command->background;
			}

			to += command->width;
			source += command->stride;
		}
		at += canvas_stage_bytes(command);
	}

	i915_gem_object_flush_map(obj);
	i915_gem_object_unpin_map(obj);
	return obj;
}

static int canvas_emit_fill(struct i915_request *rq, u64 address, u32 pitch,
			    const struct drm_canvas_command *command)
{
	const bool wide = GRAPHICS_VER(rq->i915) >= 8;
	u32 *cs;

	cs = intel_ring_begin(rq, wide ? 8 : 6);
	if (IS_ERR(cs))
		return PTR_ERR(cs);

	*cs++ = XY_COLOR_BLT_CMD | BLT_WRITE_RGBA | ((wide ? 7 : 6) - 2);
	*cs++ = BLT_DEPTH_32 | BLT_ROP_COLOR_COPY | pitch;
	*cs++ = (u32)command->y << 16 | (u16)command->x;
	*cs++ = (u32)(command->y + command->height) << 16 |
		(u16)(command->x + command->width);
	*cs++ = lower_32_bits(address);
	if (wide)
		*cs++ = upper_32_bits(address);
	*cs++ = command->foreground;
	if (wide)
		*cs++ = MI_NOOP;

	intel_ring_advance(rq, cs);
	return 0;
}

static int canvas_emit_copy(struct i915_request *rq, u64 destination,
			    u32 destination_pitch, u64 source,
			    const struct drm_canvas_command *command)
{
	const bool wide = GRAPHICS_VER(rq->i915) >= 8;
	u32 *cs;

	cs = intel_ring_begin(rq, wide ? 10 : 8);
	if (IS_ERR(cs))
		return PTR_ERR(cs);

	*cs++ = XY_SRC_COPY_BLT_CMD | BLT_WRITE_RGBA |
		((wide ? 10 : 8) - 2);
	*cs++ = BLT_DEPTH_32 | BLT_ROP_SRC_COPY | destination_pitch;
	*cs++ = (u32)command->y << 16 | (u16)command->x;
	*cs++ = (u32)(command->y + command->height) << 16 |
		(u16)(command->x + command->width);
	*cs++ = lower_32_bits(destination);
	if (wide)
		*cs++ = upper_32_bits(destination);
	*cs++ = 0;
	*cs++ = command->width * sizeof(u32);
	*cs++ = lower_32_bits(source);
	if (wide)
		*cs++ = upper_32_bits(source);

	intel_ring_advance(rq, cs);
	return 0;
}

static unsigned int canvas_mono(const struct drm_canvas_command *command,
				u32 *data)
{
	u32 width = command->width * command->scale;
	u32 height = command->height * command->scale;
	u32 row_bytes = DIV_ROUND_UP(width, 16) * 2;
	u32 sy, sx, dy, dx;

	memset(data, 0, CANVAS_MONO_BYTES);
	if (command->scale == 1) {
		u32 source_bytes = DIV_ROUND_UP(command->width, 8);

		for (sy = 0; sy < command->height; sy++)
			memcpy((u8 *)data + sy * row_bytes,
			       command->bits + sy * command->stride,
			       source_bytes);

		return ALIGN(row_bytes * height, sizeof(u64)) / sizeof(u32);
	}

	for (sy = 0; sy < command->height; sy++) {
		const u8 *source = command->bits + sy * command->stride;

		for (sx = 0; sx < command->width; sx++) {
			if (!(source[sx / 8] & (0x80 >> (sx & 7))))
				continue;

			for (dy = 0; dy < command->scale; dy++) {
				u8 *row = (u8 *)data +
					(sy * command->scale + dy) * row_bytes;

				for (dx = 0; dx < command->scale; dx++) {
					u32 x = sx * command->scale + dx;

					row[x / 8] |= 0x80 >> (x & 7);
				}
			}
		}
	}

	return ALIGN(row_bytes * height, sizeof(u64)) / sizeof(u32);
}

static int canvas_emit_mono(struct i915_request *rq, u64 address, u32 pitch,
			    const struct drm_canvas_command *command)
{
	const bool wide = GRAPHICS_VER(rq->i915) >= 8;
	u32 data[CANVAS_MONO_BYTES / sizeof(u32)];
	u32 dwords = canvas_mono(command, data);
	u32 width = command->width * command->scale;
	u32 height = command->height * command->scale;
	u32 base = wide ? 8 : 7;
	u32 *cs;

	cs = intel_ring_begin(rq, base + dwords + (!wide));
	if (IS_ERR(cs))
		return PTR_ERR(cs);

	*cs++ = XY_MONO_SRC_COPY_IMM_CMD | BLT_WRITE_RGBA |
		(base + dwords - 2);
	*cs++ = BLT_DEPTH_32 | BLT_ROP_SRC_COPY | pitch |
		(command->flags & DRM_CANVAS_MONO_TRANSPARENT ? BIT(29) : 0);
	*cs++ = (u32)command->y << 16 | (u16)command->x;
	*cs++ = (u32)(command->y + height) << 16 |
		(u16)(command->x + width);
	*cs++ = lower_32_bits(address);
	if (wide)
		*cs++ = upper_32_bits(address);
	*cs++ = command->background;
	*cs++ = command->foreground;
	memcpy(cs, data, dwords * sizeof(*cs));
	cs += dwords;
	if (!wide)
		*cs++ = MI_NOOP;

	intel_ring_advance(rq, cs);
	return 0;
}

static int canvas_submit(struct intel_engine_cs *engine, struct i915_vma *vma,
			 struct i915_vma *stage_vma, size_t *stage_at,
			 const struct drm_framebuffer *fb,
			 const struct drm_canvas_command *commands,
			 unsigned int first, unsigned int last,
			 struct i915_request **wait)
{
	struct i915_request *rq;
	u64 address = i915_vma_offset(vma);
	u64 stage_address = stage_vma ? i915_vma_offset(stage_vma) : 0;
	unsigned int i;
	bool copies = false;
	int err;

	for (i = first; i < last; i++)
		copies |= commands[i].type == DRM_CANVAS_COPY;

	rq = i915_request_create(engine->kernel_context);
	if (IS_ERR(rq))
		return PTR_ERR(rq);

	err = i915_vma_move_to_active(vma, rq, EXEC_OBJECT_WRITE);
	if (!err && copies)
		err = i915_vma_move_to_active(stage_vma, rq, 0);
	for (i = first; !err && i < last; i++) {
		if (commands[i].type == DRM_CANVAS_FILL) {
			err = canvas_emit_fill(rq, address, fb->pitches[0],
					       &commands[i]);
		} else if (commands[i].type == DRM_CANVAS_MONO) {
			err = canvas_emit_mono(rq, address, fb->pitches[0],
					       &commands[i]);
		} else {
			err = canvas_emit_copy(rq, address, fb->pitches[0],
					       stage_address + *stage_at, &commands[i]);
			*stage_at += canvas_stage_bytes(&commands[i]);
		}
	}

	if (!err)
		err = rq->engine->emit_flush(rq, EMIT_FLUSH);

	if (*wait)
		i915_request_put(*wait);
	*wait = i915_request_get(rq);
	i915_request_add(rq);
	return err;
}

int drm_i915_canvas_render(struct drm_framebuffer *fb,
			   const struct drm_canvas_command *commands,
			   unsigned int count)
{
	struct drm_i915_private *i915;
	struct drm_i915_gem_object *obj, *stage;
	struct intel_engine_cs *engine;
	struct i915_gem_ww_ctx ww;
	struct i915_request *wait = NULL;
	struct i915_vma *vma, *stage_vma = NULL;
	size_t stage_at = 0;
	bool vma_pinned = false, stage_pinned = false;
	unsigned int first, i, dwords;
	int err;

	err = canvas_target(fb, &i915, &obj, &engine);
	if (err || !count)
		return err;

	for (i = 0; i < count; i++) {
		err = canvas_validate(fb, &commands[i]);
		if (err)
			return err;
	}

	stage = canvas_stage(i915, commands, count);
	if (IS_ERR(stage))
		return PTR_ERR(stage);

	/* i915_vma_put() below owns this matching object reference. */
	i915_gem_object_get(obj);
	vma = i915_vma_instance(obj, engine->kernel_context->vm, NULL);
	if (IS_ERR(vma)) {
		i915_gem_object_put(obj);
		if (stage)
			i915_gem_object_put(stage);
		return PTR_ERR(vma);
	}

	if (stage) {
		stage_vma = i915_vma_instance(stage,
					      engine->kernel_context->vm, NULL);
		if (IS_ERR(stage_vma)) {
			err = PTR_ERR(stage_vma);
			stage_vma = NULL;
			i915_gem_object_put(stage);
			goto out_vma;
		}
	}

	i915_gem_ww_ctx_init(&ww, true);
retry:
	vma_pinned = false;
	stage_pinned = false;
	err = i915_gem_object_lock(obj, &ww);
	if (!err && stage)
		err = i915_gem_object_lock(stage, &ww);
	if (!err)
		err = i915_vma_pin_ww(vma, &ww, 0, 0, PIN_USER);
	if (!err) {
		vma_pinned = true;
		if (stage_vma)
			err = i915_vma_pin_ww(stage_vma, &ww, 0, 0, PIN_USER);
		if (!err && stage_vma)
			stage_pinned = true;
	}
	if (err && vma_pinned) {
		i915_vma_unpin(vma);
		vma_pinned = false;
	}
	if (err == -EDEADLK) {
		err = i915_gem_ww_ctx_backoff(&ww);
		if (!err)
			goto retry;
	}
	if (err)
		goto out_ww;

	/* One wake reference also serialises every kernel-context request here. */
	intel_engine_pm_get(engine);

	/* Keep requests below the copy engine kernel context's 4 KiB ring. */
	for (first = i = dwords = 0; i < count; i++) {
		unsigned int size = commands[i].type == DRM_CANVAS_FILL ? 8 :
				    commands[i].type == DRM_CANVAS_COPY ? 10 : 42;

		if (i > first && dwords + size > CANVAS_REQUEST_DWORDS) {
			err = canvas_submit(engine, vma, stage_vma, &stage_at, fb,
					    commands, first, i, &wait);
			if (err)
				goto out_wait;
			first = i;
			dwords = 0;
		}
		dwords += size;
	}

	err = canvas_submit(engine, vma, stage_vma, &stage_at, fb,
			    commands, first, count, &wait);

out_wait:
	intel_engine_pm_put(engine);
	if (stage_pinned)
		i915_vma_unpin(stage_vma);
	if (vma_pinned)
		i915_vma_unpin(vma);
out_ww:
	i915_gem_ww_ctx_fini(&ww);
	if (wait && i915_request_wait(wait, 0, MAX_SCHEDULE_TIMEOUT) < 0 && !err)
		err = -EIO;
	if (wait)
		i915_request_put(wait);
	if (stage_vma)
		i915_vma_put(stage_vma);
out_vma:
	i915_vma_put(vma);
	return err;
}
EXPORT_SYMBOL_GPL(drm_i915_canvas_render);

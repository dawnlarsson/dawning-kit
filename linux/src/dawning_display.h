/* Dawning display: in-kernel compositor entry point. */

#ifndef DAWNING_DISPLAY_H
#define DAWNING_DISPLAY_H

struct drm_device;

#ifdef CONFIG_DAWNING_DISPLAY
void dawning_display_register(struct drm_device *dev);
#else
static inline void dawning_display_register(struct drm_device *dev) {}
#endif

#endif

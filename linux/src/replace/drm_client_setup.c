/*
        Replaces drm_client_setup.

        The in-kernel compositor lives outside drivers/gpu/drm and there is no
        exported way for it to find a DRM device, so something has to hand it
        one. Upstream dispatches to a fixed set of clients and knows nothing
        about us.

        Rather than editing that file, the linker is told to send every call
        to drm_client_setup here instead. The original is still reachable as
        __real_drm_client_setup, so anything we do not want to handle falls
        through to it unchanged -- a kernel built without CONFIG_DAWNING_DISPLAY
        behaves exactly as upstream does.

        This works because the callers are DRM drivers in other object files,
        so the call is resolved by the linker. A call from inside
        drm_client_setup.c itself would be direct and would not be wrapped.
*/

struct drm_format_info;

void __real_drm_client_setup(struct drm_device *dev, const struct drm_format_info *format);

void __wrap_drm_client_setup(struct drm_device *dev, const struct drm_format_info *format)
{
        if (dawning_display_take_over(dev))
                return;

        __real_drm_client_setup(dev, format);
}

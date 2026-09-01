/*
        Canvas -- GPU display lists

        Composition is already expressed as solid rectangles and monochrome
        bitmaps.  Keep those words until the picture is complete, then hand
        the ordered list to the driver's copy engine in one go.  Nothing here
        knows an i915 object or command packet; that private work stays in the
        driver beside the VMA and fence machinery that owns it.

        A list is all-or-nothing.  Pixel windows become staged copy sources;
        an allocation ceiling or a driver failure discards the list and the
        ordinary mapped-buffer compose is run from the start.  CPU and GPU
        writes are therefore never spliced into one picture in an order
        neither side can see.
*/

#define CANVAS_GPU_COMMANDS 8192

struct canvas_gpu_batch
{
        struct output *output;
        unsigned int count;
        _Bool failed;
};

static void canvas_gpu_start(struct output *output)
{
        int ret = drm_i915_canvas_probe(output->buffer->fb);

        if (ret)
                return;

        output->gpu_commands = kvmalloc_array(CANVAS_GPU_COMMANDS,
                                               sizeof(*output->gpu_commands),
                                               GFP_KERNEL);
        if (!output->gpu_commands)
                return;

        output->gpu_capacity = CANVAS_GPU_COMMANDS;
        log_canvas("composition: i915 copy engine, display list starts at %u commands\n",
                   output->gpu_capacity);
}

static void canvas_gpu_stop(struct output *output)
{
        kvfree(output->gpu_commands);
        output->gpu_commands = NULL;
        output->gpu_capacity = 0;
}

static struct drm_canvas_command *canvas_gpu_command(const struct target *t)
{
        struct canvas_gpu_batch *batch = t->gpu;
        struct output *output;

        if (!batch || batch->failed)
                return NULL;

        output = batch->output;
        if (batch->count == output->gpu_capacity)
        {
                unsigned int capacity = output->gpu_capacity;
                struct drm_canvas_command *commands;

                if (capacity > UINT_MAX / 2)
                        goto fail;

                capacity *= 2;
                commands = kvmalloc_array(capacity, sizeof(*commands), GFP_KERNEL);
                if (!commands)
                        goto fail;

                memory_copy_apart(commands, output->gpu_commands,
                                  (unsigned long)batch->count * sizeof(*commands));
                kvfree(output->gpu_commands);
                output->gpu_commands = commands;
                output->gpu_capacity = capacity;
        }

        return &output->gpu_commands[batch->count++];

fail:
        batch->failed = true;
        return NULL;
}

static _Bool canvas_gpu_fill(const struct target *t, int x, int y,
                             unsigned int width, unsigned int height, u32 colour)
{
        struct drm_canvas_command *command;

        if (!t->gpu)
                return false;

        command = canvas_gpu_command(t);
        if (command)
                *command = (struct drm_canvas_command){
                    .foreground = colour,
                    .x = (s16)x,
                    .y = (s16)y,
                    .width = (u16)width,
                    .height = (u16)height,
                    .type = DRM_CANVAS_FILL,
                };

        return true;
}

/*
        A complete bitmap, so the engine can expand it in one command.  False
        means the bitmap is too large for the hardware's 128-byte immediate
        operand; bits_draw then emits its clipped runs as ordinary fills.
*/
static _Bool canvas_gpu_mono(const struct target *t, int x, int y, int scale,
                             const u8 *bits, unsigned int stride,
                             unsigned int width, unsigned int height,
                             u32 foreground, u32 background, _Bool transparent)
{
        struct drm_canvas_command *command;
        unsigned int drawn_width, drawn_height, row_bytes;

        if (!t->gpu)
                return false;

        drawn_width = width * (unsigned int)scale;
        drawn_height = height * (unsigned int)scale;
        row_bytes = DIV_ROUND_UP(drawn_width, 16) * 2;

        if (!scale || drawn_width > U16_MAX || drawn_height > U16_MAX ||
            row_bytes * drawn_height > 128)
                return false;

        command = canvas_gpu_command(t);
        if (command)
                *command = (struct drm_canvas_command){
                    .bits = bits,
                    .foreground = foreground,
                    .background = background,
                    .x = (s16)x,
                    .y = (s16)y,
                    .width = (u16)width,
                    .height = (u16)height,
                    .stride = (u16)stride,
                    .scale = (u8)scale,
                    .type = DRM_CANVAS_MONO,
                    .flags = transparent ? DRM_CANVAS_MONO_TRANSPARENT : 0,
                };

        return true;
}

static _Bool canvas_gpu_copy(const struct target *t, int x, int y,
                             unsigned int width, unsigned int height,
                             const u32 *source, unsigned int source_pitch)
{
        struct drm_canvas_command *command;

        if (!t->gpu)
                return false;

        command = canvas_gpu_command(t);
        if (command)
                *command = (struct drm_canvas_command){
                    .bits = (const u8 *)source,
                    .background = t->opaque,
                    .x = (s16)x,
                    .y = (s16)y,
                    .width = (u16)width,
                    .height = (u16)height,
                    .stride = (u16)source_pitch,
                    .scale = 1,
                    .type = DRM_CANVAS_COPY,
                };

        return true;
}

static _Bool canvas_gpu_submit(struct canvas_gpu_batch *batch)
{
        struct output *output = batch->output;
        int ret;

        if (batch->failed || !batch->count || output->gpu_failed)
                return false;

        ret = drm_i915_canvas_render(output->buffer->fb,
                                     output->gpu_commands, batch->count);
        if (!ret)
                return true;

        output->gpu_failed = true;
        log_canvas_error("i915 composition failed (%d), using the processor\n", ret);
        return false;
}

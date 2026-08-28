/*
        The engine, when there is one.

        Everything Canvas draws goes through canvas_rect_fill and
        canvas_row_blit in fill.asm, which write pixels with the processor.
        Measured on a 1024x768 output: 124.6 microseconds a compose, 5.2
        million pixels painted across three composes, 421 pixels a run. That
        is close to what the memory will do and there is not much left in the
        instructions.

        What is left is not drawing with the processor at all. Every display
        engine of the last fifteen years can fill and copy a rectangle
        itself, and the pixels never cross the cache.

        This is the part that does not know which engine. A backend says what
        it can do and how to ask; everything above here asks in the same
        words whatever is underneath, and gets the assembly when there is
        nothing underneath.


        Why a size below which the processor wins

        Submitting to an engine costs a job, a fence and a wait. That is
        microseconds before a single pixel moves, and fill.asm writes a
        thousand pixels in less. So a backend states the smallest rectangle
        worth handing over and small work never leaves the processor. The
        number is the backend's because it is a property of its submission
        path, not of the picture.

        This is the same shape as the memory routines: a size class decides
        which body runs, and the general one is not always the quick one.


        What a backend has to provide

        probe   look at a drm_device, decide whether this engine is one it
                drives, and set itself up. Returning false is ordinary: a
                machine with no engine we know is the common case and has to
                stay quiet.
        fill    one rectangle, one colour, in the target's own pixels.
        blit    one rectangle from another surface, same format.
        done    wait for everything submitted since the last done. Compose
                calls it once at the end rather than fencing every call,
                because a fence per rectangle would cost more than the
                rectangles saved.
        release give back whatever probe took.

        fill and blit return false when they will not do it -- wrong format,
        rectangle too small, engine busy, anything. False is not an error and
        is not logged: the caller draws it with the processor and carries on.
        That is what keeps a half working backend from being worse than none.


        Why the target is described rather than passed

        A backend needs the engine's address for the pixels, which is not the
        pointer the processor uses. The surface carries both, and whoever
        maps a buffer fills in what it knows. A backend that finds no engine
        address declines, which is the same path as an unknown format.
*/

struct canvas_surface
{
        u32 *pixels;            /* what the processor writes through */
        u64 engine_address;     /* what the engine reads, zero if unknown */
        unsigned long pitch;    /* pixels, not bytes, as everywhere here */
        unsigned int width;
        unsigned int height;
        u32 format;             /* a drm fourcc */
};

struct canvas_gpu;

struct canvas_gpu_ops
{
        const char *name;

        _Bool (*probe)(struct canvas_gpu *gpu, struct drm_device *dev);
        _Bool (*fill)(struct canvas_gpu *gpu, const struct canvas_surface *to,
                      int x, int y, unsigned int width, unsigned int height,
                      u32 colour);
        _Bool (*blit)(struct canvas_gpu *gpu, const struct canvas_surface *to,
                      int to_x, int to_y, const struct canvas_surface *from,
                      int from_x, int from_y,
                      unsigned int width, unsigned int height);
        void (*done)(struct canvas_gpu *gpu);
        void (*release)(struct canvas_gpu *gpu);
};

struct canvas_gpu
{
        const struct canvas_gpu_ops *ops;
        void *device;                   /* the backend's own handle */
        unsigned int least_pixels;      /* below this the processor wins */
        _Bool owed;                     /* work submitted since the last done */
};

static struct canvas_gpu canvas_engine;

/*
        The backends, in the order they are asked.

        Empty until one is written. A machine with no backend runs exactly
        the code it ran before this file existed, which is the point: adding
        the layer costs a null check on a path that fills a rectangle.
*/
static const struct canvas_gpu_ops *const canvas_gpu_backends[] = {
        NULL,
};

/*
        Which engine this display device has, if any.

        Said once, at attach, and quietly. A machine with no backend is the
        common case and must not look like a fault: the line is at the same
        level as the one naming the scanout format, because it is the same
        kind of fact about what we are drawing on.
*/
static void canvas_gpu_find(struct drm_device *dev)
{
        unsigned int i;

        canvas_engine.ops = NULL;
        canvas_engine.device = NULL;
        canvas_engine.owed = false;

        for (i = 0; i < sizeof(canvas_gpu_backends) / sizeof(canvas_gpu_backends[0]); i++)
        {
                const struct canvas_gpu_ops *ops = canvas_gpu_backends[i];

                if (!ops || !ops->probe)
                        continue;

                canvas_engine.ops = ops;
                canvas_engine.least_pixels = 0;

                if (ops->probe(&canvas_engine, dev))
                {
                        log_canvas("engine %s, rectangles from %u pixels\n",
                                   ops->name, canvas_engine.least_pixels);
                        return;
                }

                canvas_engine.ops = NULL;
        }

        log_canvas("no engine backend for this device, drawing with the processor\n");
}

static void canvas_gpu_forget(void)
{
        if (canvas_engine.ops && canvas_engine.ops->release)
                canvas_engine.ops->release(&canvas_engine);

        canvas_engine.ops = NULL;
        canvas_engine.device = NULL;
        canvas_engine.owed = false;
}

/*
        A rectangle of one colour.

        Answers false when it did not do it, and false is the ordinary
        answer: no backend, too small, wrong format, engine declined. The
        caller then draws it the way it always did.
*/
static _Bool canvas_gpu_fill(const struct canvas_surface *to, int x, int y,
                             unsigned int width, unsigned int height, u32 colour)
{
        const struct canvas_gpu_ops *ops = canvas_engine.ops;

        if (!ops || !ops->fill)
                return false;

        if (!to->engine_address)
                return false;

        if ((unsigned long)width * height < canvas_engine.least_pixels)
                return false;

        if (!ops->fill(&canvas_engine, to, x, y, width, height, colour))
                return false;

        canvas_engine.owed = true;
        return true;
}

static _Bool canvas_gpu_blit(const struct canvas_surface *to, int to_x, int to_y,
                             const struct canvas_surface *from, int from_x, int from_y,
                             unsigned int width, unsigned int height)
{
        const struct canvas_gpu_ops *ops = canvas_engine.ops;

        if (!ops || !ops->blit)
                return false;

        if (!to->engine_address || !from->engine_address)
                return false;

        if (to->format != from->format)
                return false;

        if ((unsigned long)width * height < canvas_engine.least_pixels)
                return false;

        if (!ops->blit(&canvas_engine, to, to_x, to_y, from, from_x, from_y,
                       width, height))
                return false;

        canvas_engine.owed = true;
        return true;
}

/*
        Everything submitted, finished.

        Called once where a compose ends, before the buffer is unmapped and
        the driver is told what changed. Nothing may read those pixels with
        the processor until this returns, which is why it is here and not
        inside fill: a fence for every rectangle would cost more than the
        rectangles saved.

        Cheap and correct when nothing was submitted, because most composes
        on most machines will submit nothing.
*/
static void canvas_gpu_settle(void)
{
        if (!canvas_engine.owed)
                return;

        if (canvas_engine.ops && canvas_engine.ops->done)
                canvas_engine.ops->done(&canvas_engine);

        canvas_engine.owed = false;
}

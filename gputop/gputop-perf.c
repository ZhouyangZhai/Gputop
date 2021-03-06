/*
 * GPU Top
 *
 * Copyright (C) 2014 Intel Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include <config.h>

#include <linux/perf_event.h>
#include <i915_oa_drm.h>

#include <asm/unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/ioctl.h>

#include <limits.h>
#include <errno.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <assert.h>
#include <inttypes.h>
#include <poll.h>

#include <uv.h>
#include <dirent.h>

#include "intel_chipset.h"

#include "gputop-util.h"
#include "gputop-list.h"
#include "gputop-mainloop.h"
#include "gputop-log.h"
#include "gputop-perf.h"
#include "gputop-oa-counters.h"
#include "gputop-cpu.h"

#include "oa-hsw.h"
#include "oa-bdw.h"
#include "oa-chv.h"
#include "oa-skl.h"
#include "oa-bxt.h"

/* Samples read() from i915 perf */
struct oa_sample {
   struct i915_perf_record_header header;
   uint8_t oa_report[];
};

#define MAX_I915_PERF_OA_SAMPLE_SIZE (8 +   /* i915_perf_record_header */ \
                                      256)  /* raw OA counter snapshot */


#define TAKEN(HEAD, TAIL, POT_SIZE)    (((HEAD) - (TAIL)) & (POT_SIZE - 1))

/* Note: this will equate to 0 when the buffer is exactly full... */
#define REMAINING(HEAD, TAIL, POT_SIZE) (POT_SIZE - TAKEN (HEAD, TAIL, POT_SIZE))

#if defined(__i386__)
#define rmb()           __asm__ volatile("lock; addl $0,0(%%esp)" ::: "memory")
#define mb()            __asm__ volatile("lock; addl $0,0(%%esp)" ::: "memory")
#endif

#if defined(__x86_64__)
#define rmb()           __asm__ volatile("lfence" ::: "memory")
#define mb()            __asm__ volatile("mfence" ::: "memory")
#endif

/* Allow building for a more recent kernel than the system headers
 * correspond too... */
#ifndef PERF_RECORD_DEVICE
#define PERF_RECORD_DEVICE      14
#endif
#ifndef PERF_FLAG_FD_CLOEXEC
#define PERF_FLAG_FD_CLOEXEC   (1UL << 3) /* O_CLOEXEC */
#endif

/* attr.config */

struct intel_device {
    uint32_t device;
    uint32_t subsystem_device;
    uint32_t subsystem_vendor;
};

bool gputop_fake_mode = false;

static struct intel_device intel_dev;

static unsigned int page_size;

struct gputop_hash_table *metrics;
struct array *gputop_perf_oa_supported_metric_set_guids;
struct perf_oa_user *gputop_perf_current_user;

static int drm_fd = -1;
static int drm_card = -1;

static gputop_list_t ctx_handles_list = {
    .prev = &ctx_handles_list,
    .next= &ctx_handles_list
};

/******************************************************************************/

static uint64_t
sysfs_card_read(const char *file)
{
        char buf[512];

        snprintf(buf, sizeof(buf), "/sys/class/drm/card%d/%s", drm_card, file);

        return gputop_read_file_uint64(buf);
}


bool gputop_add_ctx_handle(int ctx_fd, uint32_t ctx_id)
{
    struct ctx_handle *handle = xmalloc0(sizeof(*handle));
    if (!handle) {
        return false;
    }
    handle->id = ctx_id;
    handle->fd = ctx_fd;

    gputop_list_insert(&ctx_handles_list, &handle->link);

    return true;
}

bool gputop_remove_ctx_handle(uint32_t ctx_id)
{
    struct ctx_handle *ctx;
    gputop_list_for_each(ctx, &ctx_handles_list, link) {
        if (ctx->id == ctx_id) {
            gputop_list_remove(&ctx->link);
            free(ctx);
            return true;
        }
    }
    return false;
}

struct ctx_handle *get_first_available_ctx(char **error)
{
    struct ctx_handle *ctx = NULL;

    ctx = gputop_list_first(&ctx_handles_list, struct ctx_handle, link);
    if (!ctx)
        asprintf(error, "Error unable to find a context\n");

    return ctx;
}

struct ctx_handle *lookup_ctx_handle(uint32_t ctx_id)
{
    struct ctx_handle *ctx = NULL;
    gputop_list_for_each(ctx, &ctx_handles_list, link) {
        if (ctx->id == ctx_id) {
            break;
        }
    }
    return ctx;
}

/* Handle restarting ioctl if interrupted... */
static int
perf_ioctl(int fd, unsigned long request, void *arg)
{
    int ret;

    do {
        ret = ioctl(fd, request, arg);
    } while (ret == -1 && (errno == EINTR || errno == EAGAIN));
    return ret;
}

static long
perf_event_open (struct perf_event_attr *hw_event,
                 pid_t pid,
                 int cpu,
                 int group_fd,
                 unsigned long flags)
{
    return syscall(__NR_perf_event_open, hw_event, pid, cpu, group_fd, flags);
}

static void
perf_ready_cb(uv_poll_t *poll, int status, int events)
{
    struct gputop_perf_stream *stream = poll->data;

    if (stream->ready_cb)
        stream->ready_cb(stream);
}

static void
perf_fake_ready_cb(uv_timer_t *poll)
{
    struct gputop_perf_stream *stream = poll->data;

    if (stream->ready_cb)
        stream->ready_cb(stream);
}

void
gputop_perf_stream_ref(struct gputop_perf_stream *stream)
{
    stream->ref_count++;
}

/* Stream closing is split up to allow for the closure of
 * uv poll or timer handles to happen via the mainloop,
 * via uv_close() before we finish up here... */
static void
finish_stream_close(struct gputop_perf_stream *stream)
{
    switch(stream->type) {
    case GPUTOP_STREAM_PERF:
        if (stream->fd > 0) {

            if (stream->perf.mmap_page) {
                munmap(stream->perf.mmap_page, stream->perf.buffer_size + page_size);
                stream->perf.mmap_page = NULL;
                stream->perf.buffer = NULL;
                stream->perf.buffer_size = 0;
            }

            if (stream->perf.header_buf.offsets) {
                free(stream->perf.header_buf.offsets);
                stream->perf.header_buf.offsets = NULL;
            }

            close(stream->fd);
            stream->fd = -1;

            fprintf(stderr, "closed perf stream\n");
        }

        break;
    case GPUTOP_STREAM_I915_PERF:
        if (stream->fd == -1) {
            if (stream->oa.bufs[0])
                free(stream->oa.bufs[0]);
            if (stream->oa.bufs[1])
                free(stream->oa.bufs[1]);
            fprintf(stderr, "closed i915 fake perf stream\n");
        }
        if (stream->fd > 0) {
            if (stream->oa.bufs[0])
                free(stream->oa.bufs[0]);
            if (stream->oa.bufs[1])
                free(stream->oa.bufs[1]);

            close(stream->fd);
            stream->fd = -1;

            fprintf(stderr, "closed i915 perf stream\n");
        }

        break;
    case GPUTOP_STREAM_CPU:
        free(stream->cpu.stats_buf);
        uv_timer_stop(&stream->cpu.sample_timer);
        stream->cpu.stats_buf = NULL;
        fprintf(stderr, "closed cpu stats stream\n");
        break;
    }

    stream->closed = true;
    stream->on_close_cb(stream);
}

static void
stream_handle_closed_cb(uv_handle_t *handle)
{
    struct gputop_perf_stream *stream = handle->data;

    if (--(stream->n_closing_uv_handles) == 0)
        finish_stream_close(stream);
}

void
gputop_perf_stream_close(struct gputop_perf_stream *stream,
                         void (*on_close_cb)(struct gputop_perf_stream *stream))
{
    stream->on_close_cb = on_close_cb;

    /* First we close any libuv handles before closing anything else in
     * stream_handle_closed_cb()...
     */
    switch(stream->type) {
    case GPUTOP_STREAM_PERF:
        if (stream->fd >= 0) {
            uv_close((uv_handle_t *)&stream->fd_poll, stream_handle_closed_cb);
            stream->n_closing_uv_handles++;
        }
        break;
    case GPUTOP_STREAM_I915_PERF:
        if (stream->fd == -1) {
            uv_close((uv_handle_t *)&stream->fd_timer, stream_handle_closed_cb);
            stream->n_closing_uv_handles++;

        }
        if (stream->fd >= 0) {
            uv_close((uv_handle_t *)&stream->fd_poll, stream_handle_closed_cb);
            stream->n_closing_uv_handles++;
        }
        break;
    case GPUTOP_STREAM_CPU:
        break;
    }

    if (!stream->n_closing_uv_handles)
        finish_stream_close(stream);
}

void
gputop_perf_stream_unref(struct gputop_perf_stream *stream)
{
    if (--(stream->ref_count) == 0) {
        /* gputop_perf_stream_close() must have been called before the
         * last reference is dropped... */
        assert(stream->closed);

        if (stream->user.destroy_cb)
            stream->user.destroy_cb(stream);

        free(stream);
        fprintf(stderr, "freed gputop-perf stream\n");
    }
}

struct gputop_perf_stream *
gputop_open_i915_perf_oa_stream(struct gputop_metric_set *metric_set,
                                int period_exponent,
                                struct ctx_handle *ctx,
                                void (*ready_cb)(struct gputop_perf_stream *),
                                bool overwrite,
                                char **error)
{
    struct gputop_perf_stream *stream;
    struct i915_perf_open_param param;
    int stream_fd = -1;
    int oa_stream_fd = drm_fd;

    if (!gputop_fake_mode) {
        uint64_t properties[DRM_I915_PERF_PROP_MAX * 2];
        int p = 0;

        memset(&param, 0, sizeof(param));

        param.flags = 0;
        param.flags |= I915_PERF_FLAG_FD_CLOEXEC;
        param.flags |= I915_PERF_FLAG_FD_NONBLOCK;

        properties[p++] = DRM_I915_PERF_PROP_SAMPLE_OA;
        properties[p++] = true;

        properties[p++] = DRM_I915_PERF_PROP_OA_METRICS_SET;
        properties[p++] = metric_set->perf_oa_metrics_set;

        properties[p++] = DRM_I915_PERF_PROP_OA_FORMAT;
        properties[p++] = metric_set->perf_oa_format;

        properties[p++] = DRM_I915_PERF_PROP_OA_EXPONENT;
        properties[p++] = period_exponent;

        if (ctx) {
            properties[p++] = DRM_I915_PERF_PROP_CTX_HANDLE;
            properties[p++] = ctx->id;

            // N.B The file descriptor that was used to create the context,
            // _must_ be same as the one we use to open the per-context stream.
            // Since in the kernel we lookup the intel_context based on the ctx
            // id and the fd that was used to open the stream, so if there is a
            // mismatch between the file descriptors for the stream and the
            // context creation then the kernel will simply fail with the
            // lookup.
            oa_stream_fd = ctx->fd;
            dbg("opening per context i915 perf stream: fd = %d, ctx=%u\n", ctx->fd, ctx->id);
        }

        param.properties_ptr = (uint64_t)properties;
        param.num_properties = p / 2;

        stream_fd = perf_ioctl(oa_stream_fd, I915_IOCTL_PERF_OPEN, &param);
        if (stream_fd == -1) {
            asprintf(error, "Error opening i915 perf OA event: %m\n");
            return NULL;
        }
    }

    stream = xmalloc0(sizeof(*stream));
    stream->type = GPUTOP_STREAM_I915_PERF;
    stream->ref_count = 1;
    stream->metric_set = metric_set;
    stream->ready_cb = ready_cb;
    stream->per_ctx_mode = ctx != NULL;

    stream->fd = stream_fd;

    if (gputop_fake_mode) {
        stream->start_time = gputop_get_time();
        stream->prev_clocks = gputop_get_time();
        stream->period = 80 * (2 << period_exponent);
        stream->prev_timestamp = gputop_get_time();
    }

    /* We double buffer the samples we read from the kernel so
     * we can maintain a stream->last pointer for calculating
     * counter deltas */
    stream->oa.buf_sizes = MAX_I915_PERF_OA_SAMPLE_SIZE * 100;
    stream->oa.bufs[0] = xmalloc0(stream->oa.buf_sizes);
    stream->oa.bufs[1] = xmalloc0(stream->oa.buf_sizes);

    stream->overwrite = overwrite;
    if (overwrite) {
#warning "TODO: support flight-recorder mode"
        assert(0);
    }

    stream->fd_poll.data = stream;
    stream->fd_timer.data = stream;

    if (gputop_fake_mode)
    {
        uv_timer_init(gputop_mainloop, &stream->fd_timer);
        uv_timer_start(&stream->fd_timer, perf_fake_ready_cb, 1000, 1000);
    }
    else
    {
        uv_poll_init(gputop_mainloop, &stream->fd_poll, stream->fd);
        uv_poll_start(&stream->fd_poll, UV_READABLE, perf_ready_cb);
    }


    return stream;
}

struct gputop_perf_stream *
gputop_perf_open_trace(int pid,
                       int cpu,
                       const char *system,
                       const char *event,
                       size_t trace_struct_size,
                       size_t perf_buffer_size,
                       void (*ready_cb)(uv_poll_t *poll, int status, int events),
                       bool overwrite,
                       char **error)
{
    struct gputop_perf_stream *stream;
    struct perf_event_attr attr;
    int event_fd;
    uint8_t *mmap_base;
    int expected_max_samples;
    char *filename = NULL;
    int id = 0;
    size_t sample_size = 0;

    asprintf(&filename, "/sys/kernel/debug/tracing/events/%s/%s/id", system, event);
    if (filename) {
        struct stat st;

        if (stat(filename, &st) < 0) {
            int err = errno;

            free(filename);
            filename = NULL;

            if (err == EPERM) {
                asprintf(error, "Permission denied to open tracepoint %s:%s"
                         " (Linux tracepoints require root privileges)",
                         system, event);
                return NULL;
            } else {
                asprintf(error, "Failed to open tracepoint %s:%s: %s",
                         system, event,
                         strerror(err));
                return NULL;
            }
        }
    }

    id = gputop_read_file_uint64(filename);
    free(filename);
    filename = NULL;

    memset(&attr, 0, sizeof(attr));
    attr.size = sizeof(attr);
    attr.type = PERF_TYPE_TRACEPOINT;
    attr.config = id;

    attr.sample_type = PERF_SAMPLE_RAW | PERF_SAMPLE_TIME;
    attr.sample_period = 1;

    attr.watermark = true;
    attr.wakeup_watermark = perf_buffer_size / 4;

    event_fd = perf_event_open(&attr,
                               pid,
                               cpu,
                               -1, /* group fd */
                               PERF_FLAG_FD_CLOEXEC); /* flags */
    if (event_fd == -1) {
        asprintf(error, "Error opening perf tracepoint event: %m\n");
        return NULL;
    }

    /* NB: A read-write mapping ensures the kernel will stop writing data when
     * the buffer is full, and will report samples as lost. */
    mmap_base = mmap(NULL,
                     perf_buffer_size + page_size,
                     PROT_READ | PROT_WRITE, MAP_SHARED, event_fd, 0);
    if (mmap_base == MAP_FAILED) {
        asprintf(error, "Error mapping circular buffer, %m\n");
        close (event_fd);
        return NULL;
    }

    stream = xmalloc0(sizeof(*stream));
    stream->type = GPUTOP_STREAM_PERF;
    stream->ref_count = 1;
    stream->fd = event_fd;
    stream->perf.buffer = mmap_base + page_size;
    stream->perf.buffer_size = perf_buffer_size;
    stream->perf.mmap_page = (void *)mmap_base;

    sample_size =
        sizeof(struct perf_event_header) +
        8 /* _TIME */ +
        trace_struct_size; /* _RAW */

    expected_max_samples = (stream->perf.buffer_size / sample_size) * 1.2;

    memset(&stream->perf.header_buf, 0, sizeof(stream->perf.header_buf));

    stream->overwrite = overwrite;
    if (overwrite) {
        stream->perf.header_buf.len = expected_max_samples;
        stream->perf.header_buf.offsets =
            xmalloc(sizeof(uint32_t) * expected_max_samples);
    }

    stream->fd_poll.data = stream;
    uv_poll_init(gputop_mainloop, &stream->fd_poll, stream->fd);
    uv_poll_start(&stream->fd_poll, UV_READABLE, ready_cb);

    return stream;
}

struct gputop_perf_stream *
gputop_perf_open_generic_counter(int pid,
                                 int cpu,
                                 uint64_t type,
                                 uint64_t config,
                                 size_t perf_buffer_size,
                                 void (*ready_cb)(uv_poll_t *poll, int status, int events),
                                 bool overwrite,
                                 char **error)
{
    struct gputop_perf_stream *stream;
    struct perf_event_attr attr;
    int event_fd;
    uint8_t *mmap_base;
    int expected_max_samples;
    size_t sample_size = 0;

    memset(&attr, 0, sizeof(attr));
    attr.size = sizeof(attr);
    attr.type = type;
    attr.config = config;

    attr.sample_type = PERF_SAMPLE_READ | PERF_SAMPLE_TIME;
    attr.sample_period = 1;

    attr.watermark = true;
    attr.wakeup_watermark = perf_buffer_size / 4;

    event_fd = perf_event_open(&attr,
                               pid,
                               cpu,
                               -1, /* group fd */
                               PERF_FLAG_FD_CLOEXEC); /* flags */
    if (event_fd == -1) {
        asprintf(error, "Error opening perf event: %m\n");
        return NULL;
    }

    /* NB: A read-write mapping ensures the kernel will stop writing data when
     * the buffer is full, and will report samples as lost. */
    mmap_base = mmap(NULL,
                     perf_buffer_size + page_size,
                     PROT_READ | PROT_WRITE, MAP_SHARED, event_fd, 0);
    if (mmap_base == MAP_FAILED) {
        asprintf(error, "Error mapping circular buffer, %m\n");
        close (event_fd);
        return NULL;
    }

    stream = xmalloc0(sizeof(*stream));
    stream->type = GPUTOP_STREAM_PERF;
    stream->ref_count = 1;
    stream->fd = event_fd;
    stream->perf.buffer = mmap_base + page_size;
    stream->perf.buffer_size = perf_buffer_size;
    stream->perf.mmap_page = (void *)mmap_base;

    sample_size =
        sizeof(struct perf_event_header) +
        8; /* _TIME */
    expected_max_samples = (stream->perf.buffer_size / sample_size) * 1.2;

    memset(&stream->perf.header_buf, 0, sizeof(stream->perf.header_buf));

    stream->overwrite = overwrite;
    if (overwrite) {
        stream->perf.header_buf.len = expected_max_samples;
        stream->perf.header_buf.offsets =
            xmalloc(sizeof(uint32_t) * expected_max_samples);
    }

    stream->fd_poll.data = stream;
    uv_poll_init(gputop_mainloop, &stream->fd_poll, stream->fd);
    uv_poll_start(&stream->fd_poll, UV_READABLE, ready_cb);

    return stream;
}

static void
log_cpu_stats_cb(uv_timer_t *timer)
{
    struct gputop_perf_stream *stream = timer->data;

    if (stream->cpu.stats_buf_pos < stream->cpu.stats_buf_len) {
        struct cpu_stat *stats = stream->cpu.stats_buf + stream->cpu.stats_buf_pos;
        int n_cpus = gputop_cpu_count();

        gputop_cpu_read_stats(stats, n_cpus);
        stream->cpu.stats_buf_pos += n_cpus;
    }

    if (stream->cpu.stats_buf_pos >= stream->cpu.stats_buf_len) {
        stream->cpu.stats_buf_full = true;
        if (stream->overwrite)
            stream->cpu.stats_buf_pos = 0;
    }
}

struct gputop_perf_stream *
gputop_perf_open_cpu_stats(bool overwrite, uint64_t sample_period_ms)
{
    struct gputop_perf_stream *stream;
    int n_cpus = gputop_cpu_count();

    stream = xmalloc0(sizeof(*stream));
    stream->type = GPUTOP_STREAM_CPU;
    stream->ref_count = 1;

    stream->cpu.stats_buf_len = MAX(10, 1000 / sample_period_ms);
    stream->cpu.stats_buf = xmalloc(stream->cpu.stats_buf_len *
                                    sizeof(struct cpu_stat) * n_cpus);
    stream->cpu.stats_buf_pos = 0;

    stream->overwrite = overwrite;

    stream->cpu.sample_timer.data = stream;
    uv_timer_init(gputop_mainloop, &stream->cpu.sample_timer);

    uv_timer_start(&stream->cpu.sample_timer,
                   log_cpu_stats_cb,
                   sample_period_ms,
                   sample_period_ms);

    return stream;
}

static void
init_dev_info(int drm_fd, uint32_t devid)
{
    int threads_per_eu = 7;

    gputop_devinfo.devid = devid;

    gputop_devinfo.timestamp_frequency = 12500000;

    if (gputop_fake_mode) {
            gputop_devinfo.n_eus = 10;
            gputop_devinfo.n_eu_slices = 1;
            gputop_devinfo.n_eu_sub_slices = 1;
            gputop_devinfo.slice_mask = 0x1;
            gputop_devinfo.subslice_mask = 0x1;
            gputop_devinfo.gt_min_freq = 500;
            gputop_devinfo.gt_min_freq = 1100;
    } else {
        if (IS_HASWELL(devid)) {
            if (IS_HSW_GT1(devid)) {
                gputop_devinfo.n_eus = 10;
                gputop_devinfo.n_eu_slices = 1;
                gputop_devinfo.n_eu_sub_slices = 1;
                gputop_devinfo.slice_mask = 0x1;
                gputop_devinfo.subslice_mask = 0x1;
            } else if (IS_HSW_GT2(devid)) {
                gputop_devinfo.n_eus = 20;
                gputop_devinfo.n_eu_slices = 1;
                gputop_devinfo.n_eu_sub_slices = 2;
                gputop_devinfo.slice_mask = 0x1;
                gputop_devinfo.subslice_mask = 0x3;
            } else if (IS_HSW_GT3(devid)) {
                gputop_devinfo.n_eus = 40;
                gputop_devinfo.n_eu_slices = 2;
                gputop_devinfo.n_eu_sub_slices = 2;
                gputop_devinfo.slice_mask = 0x3;
                gputop_devinfo.subslice_mask = 0xf;
            }
            gputop_devinfo.gen = 7;
        } else {
            i915_getparam_t gp;
            int ret;
            int n_eus = 0;
            int slice_mask = 0;
            int ss_mask = 0;
            int s_max;
            int ss_max;
            uint64_t subslice_mask = 0;
            int s;

            if (IS_BROADWELL(devid)) {
                s_max = 2;
                ss_max = 3;
                gputop_devinfo.gen = 8;
            } else if (IS_CHERRYVIEW(devid)) {
                s_max = 1;
                ss_max = 2;
                gputop_devinfo.gen = 8;
            } else if (IS_SKYLAKE(devid)) {
                s_max = 3;
                ss_max = 3;
                gputop_devinfo.gen = 9;

                assert(!IS_BROXTON(devid)); /* XXX: the frequency is different
                                               for Broxton */

                gputop_devinfo.timestamp_frequency = 12000000;
            }

            gp.param = I915_PARAM_EU_TOTAL;
            gp.value = &n_eus;
            ret = perf_ioctl(drm_fd, I915_IOCTL_GETPARAM, &gp);
            assert(ret == 0 && n_eus > 0);

            gp.param = I915_PARAM_SLICE_MASK;
            gp.value = &slice_mask;
            ret = perf_ioctl(drm_fd, I915_IOCTL_GETPARAM, &gp);
            assert(ret == 0 && slice_mask);

            gp.param = I915_PARAM_SUBSLICE_MASK;
            gp.value = &ss_mask;
            ret = perf_ioctl(drm_fd, I915_IOCTL_GETPARAM, &gp);
            assert(ret == 0 && ss_mask);

            gputop_devinfo.n_eus = n_eus;
            gputop_devinfo.n_eu_slices = __builtin_popcount(slice_mask);
            gputop_devinfo.slice_mask = slice_mask;

            /* Note: some of the metrics we have (as described in XML)
             * are conditional on a $SubsliceMask variable which is
             * expected to also reflect the slice mask by packing
             * together subslice masks for each slice in one value...
             */
            for (s = 0; s < s_max; s++) {
                if (slice_mask & (1<<s)) {
                    subslice_mask |= ss_mask << (ss_max * s);
                }
            }
            gputop_devinfo.subslice_mask = subslice_mask;
            gputop_devinfo.n_eu_sub_slices = __builtin_popcount(subslice_mask);
        }

        assert(drm_card >= 0);
        gputop_devinfo.gt_min_freq = sysfs_card_read("gt_min_freq_mhz");
        gputop_devinfo.gt_max_freq = sysfs_card_read("gt_max_freq_mhz");
    }

    gputop_devinfo.eu_threads_count =
        gputop_devinfo.n_eus * threads_per_eu;

}

static unsigned int
read_perf_head(struct perf_event_mmap_page *mmap_page)
{
    unsigned int head = (*(volatile uint64_t *)&mmap_page->data_head);
    rmb();

    return head;
}

static void
write_perf_tail(struct perf_event_mmap_page *mmap_page,
                unsigned int tail)
{
    /* Make sure we've finished reading all the sample data we
     * we're consuming before updating the tail... */
    mb();
    mmap_page->data_tail = tail;
}

static bool
perf_stream_data_pending(struct gputop_perf_stream *stream)
{
    uint64_t head = read_perf_head(stream->perf.mmap_page);
    uint64_t tail = stream->perf.mmap_page->data_tail;

    return !!TAKEN(head, tail, stream->perf.buffer_size);
}

static bool
i915_perf_stream_data_pending(struct gputop_perf_stream *stream)
{
    struct pollfd pollfd = { stream->fd, POLLIN, 0 };
    int ret;
    if (gputop_fake_mode) {
        uint64_t elapsed_time = gputop_get_time() - stream->start_time;
        if (elapsed_time / stream->period - stream->gen_so_far > 0)
            return true;
        else
            return false;
    } else {
        while ((ret = poll(&pollfd, 1, 0)) < 0 && errno == EINTR)
            ;

        if (ret == 1 && pollfd.revents & POLLIN)
            return true;
        else
            return false;
    }
}

bool
gputop_stream_data_pending(struct gputop_perf_stream *stream)
{
    switch (stream->type) {
    case GPUTOP_STREAM_PERF:
        return perf_stream_data_pending(stream);
    case GPUTOP_STREAM_I915_PERF:
        return i915_perf_stream_data_pending(stream);
    case GPUTOP_STREAM_CPU:
        if (stream->cpu.stats_buf_pos == 0 && !stream->cpu.stats_buf_full)
            return false;
        else
            return true;
    }

    assert(0);

}

/* Perf supports a flight recorder mode whereby it won't stop writing
 * samples once the buffer is full and will instead overwrite old
 * samples.
 *
 * The difficulty with this mode is that because samples don't have a
 * uniform size, once the head gets trampled we can no longer parse
 * *any* samples since the location of each sample depends of the
 * length of the previous.
 *
 * Since we are paranoid about wasting memory bandwidth - as such a
 * common gpu bottleneck - we would rather not resort to copying
 * samples into another buffer, especially to implement a tracing
 * feature where higher sampler frequencies are interesting.
 *
 * To simplify things to handle the main case we care about where
 * the perf circular buffer is full of samples (as opposed to
 * lots of throttle or status records) we can define a fixed number
 * of pointers to track, given the size of the perf buffer and
 * known size for samples. These can be tracked in a circular
 * buffer with fixed size records where overwriting the head isn't
 * a problem.
 */

/*
 * For each update of this buffer we:
 *
 * 1) Check what new records have been added:
 *
 *    * if buf->last_perf_head uninitialized, set it to the perf tail
 *    * foreach new record from buf->last_perf_head to the current perf head:
 *        - check there's room for a new header offset, but if not:
 *            - report an error
 *            - move the tail forward (loosing a record)
 *        - add a header offset to buf->offsets[buf->head]
 *        - buf->head++;
 *        - recognise when the perf head wraps and mark the buffer 'full'
 *
 * 2) Optionally parse any of the new records (i.e. before we update
 *    tail)
 *
 *    Typically we aren't processing the records while tracing, but
 *    beware that if anything does need passing on the fly then it
 *    needs to be done before we update the tail pointer below.
 *
 * 3) If buf 'full'; check how much of perf's tail has been eaten:
 *
 *    * move buf->tail forward to the next offset that is ahead of
 *      perf's (head + header->size)
 *        XXX: we can assert() that we don't overtake buf->head. That
 *        shouldn't be possible if we aren't enabling perf's
 *        overwriting/flight recorder mode.
 *          XXX: Note: we do this after checking for new records so we
 *          don't have to worry about the corner case of eating more
 *          than we previously knew about.
 *
 * 4) Set perf's tail to perf's head (i.e. consume everything so that
 *    perf won't block when wrapping around and overwriting old
 *    samples.)
 */
void
gputop_perf_update_header_offsets(struct gputop_perf_stream *stream)
{
    struct gputop_perf_header_buf *hdr_buf  = &stream->perf.header_buf;
    uint8_t *data = stream->perf.buffer;
    const uint64_t mask = stream->perf.buffer_size - 1;
    uint64_t perf_head;
    uint64_t perf_tail;
    uint32_t buf_head;
    uint32_t buf_tail;
    uint32_t n_new = 0;

    perf_head = read_perf_head(stream->perf.mmap_page);

    //if (hdr_buf->head == hdr_buf->tail)
        //perf_tail = hdr_buf->last_perf_head;
    //else
        perf_tail = stream->perf.mmap_page->data_tail;

#if 0
    if (perf_tail > perf_head) {
        dbg("Unexpected perf tail > head condition\n");
        return;
    }
#endif

    if (perf_head == perf_tail)
        return;

    //hdr_buf->last_perf_head = perf_head;

    buf_head = hdr_buf->head;
    buf_tail = hdr_buf->tail;

#if 1
    printf("perf records:\n");
    printf("> fd = %d\n", stream->fd);
    printf("> size = %lu\n", stream->perf.buffer_size);
    printf("> tail_ptr = %p\n", &stream->perf.mmap_page->data_tail);
    printf("> head=%"PRIu64"\n", perf_head);
    printf("> tail=%"PRIu64"\n", (uint64_t)stream->perf.mmap_page->data_tail);
    printf("> TAKEN=%"PRIu64"\n", (uint64_t)TAKEN(perf_head, stream->perf.mmap_page->data_tail, stream->perf.buffer_size));
    printf("> records:\n");
#endif

    while (TAKEN(perf_head, perf_tail, stream->perf.buffer_size)) {
        uint64_t perf_offset = perf_tail & mask;
        const struct perf_event_header *header =
            (const struct perf_event_header *)(data + perf_offset);

        n_new++;

        if (header->size == 0) {
            dbg("Spurious header size == 0\n");
            /* XXX: How should we handle this instead of exiting() */
            break;
            //exit(1);
        }

        if (header->size > (perf_head - perf_tail)) {
            dbg("Spurious header size would overshoot head\n");
            /* XXX: How should we handle this instead of exiting() */
            break;
            //exit(1);
        }

        /* Once perf wraps, the buffer is full of data and perf starts
         * to eat its tail, overwriting old data. */
        if ((const uint8_t *)header + header->size > data + stream->perf.buffer_size)
            hdr_buf->full = true;

        if ((buf_head - buf_tail) == hdr_buf->len)
            buf_tail++;

        /* Checking what tail records have been being overwritten by this
         * new record...
         *
         * NB: A record may be split at the end of the buffer
         * NB: A large record may trample multiple smaller records
         * NB: it's possible no records have been trampled
         */
        if (hdr_buf->full) {
            while (1) {
                uint32_t buf_tail_offset = hdr_buf->offsets[buf_tail % hdr_buf->len];

                /* To simplify checking for an overlap, invariably ensure the
                 * buf_tail_offset is ahead of perf, even if it means using a
                 * fake offset beyond the bounds of the buffer... */
                if (buf_tail_offset < perf_offset)
                    buf_tail_offset += stream->perf.buffer_size;

                if ((perf_offset + header->size) < buf_tail_offset) /* nothing eaten */
                    break;

                buf_tail++;
            }
        }

        hdr_buf->offsets[buf_head++ % hdr_buf->len] = perf_offset;

        perf_tail += header->size;
    }

    /* Consume all perf records so perf wont be blocked from
     * overwriting old samples... */
    write_perf_tail(stream->perf.mmap_page, perf_head);

    hdr_buf->head = buf_head;
    hdr_buf->tail = buf_tail;

#if 1
    printf("headers update:\n");
    printf("n new records = %d\n", n_new);
    printf("buf len = %d\n", hdr_buf->len);
    printf("perf head = %"PRIu64"\n", perf_head);
    printf("perf tail = %"PRIu64"\n", perf_tail);
    printf("buf head = %"PRIu32"\n", hdr_buf->head);
    printf("buf tail = %"PRIu32"\n", hdr_buf->tail);

    if (!hdr_buf->full) {
        float percentage = (hdr_buf->offsets[(hdr_buf->head - 1) % hdr_buf->len] / (float)stream->perf.buffer_size) * 100.0f;
        printf("> %d%% full\n", (int)percentage);
    } else
        printf("> 100%% full\n");

    printf("> n records = %u\n", (hdr_buf->head - hdr_buf->tail));
#endif
}

void
gputop_i915_perf_print_records(struct gputop_perf_stream *stream,
                               uint8_t *buf,
                               int len)
{
    const struct i915_perf_record_header *header;
    int offset = 0;

    printf("records:\n");

    for (offset = 0; offset < len; offset += header->size) {
        header = (const struct i915_perf_record_header *)(buf + offset);

        if (header->size == 0) {
            printf("Spurious header size == 0\n");
            return;
        }
        printf("- header size = %d\n", header->size);

        switch (header->type) {

        case DRM_I915_PERF_RECORD_OA_BUFFER_LOST:
            printf("- OA buffer error - all records lost\n");
            break;
        case DRM_I915_PERF_RECORD_OA_REPORT_LOST:
            printf("- OA report lost\n");
            break;
        case DRM_I915_PERF_RECORD_SAMPLE: {
            printf("- Sample\n");
            break;
        }

        default:
            printf("- Spurious header type = %d\n", header->type);
        }
    }
}


static void
read_perf_samples(struct gputop_perf_stream *stream)
{
    dbg("FIXME: read core perf samples");
}


struct report_layout
{
    struct i915_perf_record_header header;
    uint32_t rep_id;
    uint32_t timest;
    uint32_t context_id;
    uint32_t clock_ticks;
    uint32_t counter_40_lsb[32];
    uint32_t agg_counter[4];
    uint8_t counter_40_msb[32];
    uint32_t bool_custom_counters[16];
};

// Function that generates fake Broadwell report metrics
int
gputop_perf_fake_read(struct gputop_perf_stream *stream,
                      uint8_t *buf, int buf_length)
{
    struct report_layout *report = (struct report_layout *)buf;
    struct i915_perf_record_header header;
    uint32_t timestamp, elapsed_clocks;
    int i;
    uint64_t counter;
    uint64_t elapsed_time = gputop_get_time() - stream->start_time;
    uint32_t records_to_gen;

    header.type = DRM_I915_PERF_RECORD_SAMPLE;
    header.pad = 0;
    header.size = sizeof(struct report_layout);

    // Calculate the minimum between records required (in relation to the time elapsed)
    // and the maximum number of records that can bit in the buffer.
    if (elapsed_time / stream->period - stream->gen_so_far < buf_length / header.size)
        records_to_gen = elapsed_time / stream->period - stream->gen_so_far;
    else
        records_to_gen = buf_length / header.size;

    for (i = 0; i < records_to_gen; i++) {
        int j;
        uint32_t counter_lsb;
        uint8_t counter_msb;

        // Header
        report->header = header;

        // Reason / Report ID
        report->rep_id = 1 << 19;

        // Timestamp
        timestamp = stream->period / 80 + stream->prev_timestamp;
        stream->prev_timestamp = timestamp;
        report->timest = timestamp;

        // GPU Clock Ticks
        elapsed_clocks = stream->period / 2 + stream->prev_clocks;
        stream->prev_clocks = elapsed_clocks;
        report->clock_ticks = elapsed_clocks;

        counter = elapsed_clocks * gputop_devinfo.n_eus;
        counter_msb = (counter >> 32) & 0xFF;
        counter_lsb = (uint32_t)counter;

        // Populate the 40 bit counters
        for (j = 0; j < 32; j++) {
            report->counter_40_lsb[j] = counter_lsb;
            report->counter_40_msb[j] = counter_msb;
        }

        // Populate the next 4 Counters
        for (j = 0; j < 4; j++)
            report->agg_counter[j] = counter_lsb;

        // Populate the final 16 boolean & custom counters
        counter = elapsed_clocks * 2;
        counter_lsb = (uint32_t)counter;
        for (j = 0; j < 16; j++)
            report->bool_custom_counters[j] = counter_lsb;

        stream->gen_so_far++;
        report++;
    }
    return header.size * records_to_gen;
}

static void
read_i915_perf_samples(struct gputop_perf_stream *stream)
{
    do {
        int offset = 0;
        int buf_idx;
        uint8_t *buf;
        int count;

        /* We double buffer reads so we can safely keep a pointer to
         * our last sample for calculating deltas */
        buf_idx = !stream->oa.last_buf_idx;
        buf = stream->oa.bufs[buf_idx];

        if (gputop_fake_mode)
            count = gputop_perf_fake_read(stream, buf, stream->oa.buf_sizes);
        else
            count = read(stream->fd, buf, stream->oa.buf_sizes);

        if (count < 0) {
            if (errno == EINTR)
                continue;
            else if (errno == EAGAIN)
                break;
            else {
                dbg("Error reading i915 OA event stream %m");
                break;
            }
        }

        if (count == 0)
            break;

        while (offset < count) {
            const struct i915_perf_record_header *header =
                (const struct i915_perf_record_header *)(buf + offset);

            if (header->size == 0) {
                dbg("Spurious header size == 0\n");
                /* XXX: How should we handle this instead of exiting() */
                exit(1);
            }

            offset += header->size;

            switch (header->type) {

            case DRM_I915_PERF_RECORD_OA_BUFFER_LOST:
                dbg("i915 perf: OA buffer error - all records lost\n");
                break;
            case DRM_I915_PERF_RECORD_OA_REPORT_LOST:
                dbg("i915 perf: OA report lost\n");
                break;

            case DRM_I915_PERF_RECORD_SAMPLE: {
                struct oa_sample *sample = (struct oa_sample *)header;
                uint8_t *report = sample->oa_report;

                if (stream->oa.last)
                    gputop_perf_current_user->sample(stream, stream->oa.last, report);

                stream->oa.last = report;

                /* track which buffer oa.last points into so our next read
                 * won't clobber it... */
                stream->oa.last_buf_idx = buf_idx;
                break;
            }

            default:
                dbg("i915 perf: Spurious header type = %d\n", header->type);
            }
        }
    } while(1);
}

void
gputop_perf_read_samples(struct gputop_perf_stream *stream)
{
    switch (stream->type) {
    case GPUTOP_STREAM_PERF:
        read_perf_samples(stream);
        return;
    case GPUTOP_STREAM_I915_PERF:
        read_i915_perf_samples(stream);
        return;
    case GPUTOP_STREAM_CPU:
        assert(0);
        return;
    }

    assert(0);
}

/******************************************************************************/

uint64_t
read_uint64_oa_counter(const struct gputop_metric_set *metric_set,
                       const struct gputop_metric_set_counter *counter,
                       uint64_t *deltas)
{
    return counter->oa_counter_read_uint64(&gputop_devinfo, metric_set, deltas);
}

uint32_t
read_uint32_oa_counter(const struct gputop_metric_set *metric_set,
                       const struct gputop_metric_set_counter *counter,
                       uint64_t *deltas)
{
    assert(0);
    //return counter->oa_counter_read_uint32(&gputop_devinfo, metric_set, deltas);
}

bool
read_bool_oa_counter(const struct gputop_metric_set *metric_set,
                     const struct gputop_metric_set_counter *counter,
                     uint64_t *deltas)
{
    assert(0);
    //return counter->oa_counter_read_bool(&gputop_devinfo, metric_set, deltas);
}

double
read_double_oa_counter(const struct gputop_metric_set *metric_set,
                       const struct gputop_metric_set_counter *counter,
                       uint64_t *deltas)
{
    assert(0);
    //return counter->oa_counter_read_double(&gputop_devinfo, metric_set, deltas);
}

float
read_float_oa_counter(const struct gputop_metric_set *metric_set,
                      const struct gputop_metric_set_counter *counter,
                      uint64_t *deltas)
{
    return counter->oa_counter_read_float(&gputop_devinfo, metric_set, deltas);
}

uint64_t
read_report_timestamp(const uint32_t *report)
{
   uint64_t timestamp = report[1];

   /* The least significant timestamp bit represents 80ns */
   timestamp *= 80;

   return timestamp;
}

static int
get_card_for_fd(int drm_fd)
{
    struct stat sb;
    int mjr, mnr;
    char buffer[128];
    DIR *drm_dir;
    int entry_size;
    struct dirent *entry1, *entry2;
    int name_max;
    int retval = -1;

    if (fstat(drm_fd, &sb)) {
        gputop_log(GPUTOP_LOG_LEVEL_HIGH, "Failed to stat DRM fd\n", -1);
        return false;
    }

    mjr = major(sb.st_rdev);
    mnr = minor(sb.st_rdev);

    snprintf(buffer, sizeof(buffer), "/sys/dev/char/%d:%d/device/drm", mjr, mnr);

    drm_dir = opendir(buffer);
    assert(drm_dir != NULL);

    name_max = pathconf(buffer, _PC_NAME_MAX);

    if (name_max == -1)
        name_max = 255;

    entry_size = offsetof(struct dirent, d_name) + name_max + 1;
    entry1 = alloca(entry_size);

    while ((readdir_r(drm_dir, entry1, &entry2) == 0) && entry2 != NULL) {
        if (entry2->d_type == DT_DIR && strncmp(entry2->d_name, "card", 4) == 0) {
	    retval=strtoull(entry2->d_name + 4, NULL, 10);
            break;
        }
    }

    closedir(drm_dir);
    return retval;
}

static uint32_t
read_device_param(const char *stem, int id, const char *param)
{
    char *name;
    int ret = asprintf(&name, "/sys/class/drm/%s%u/device/%s", stem, id, param);
    uint32_t value;

    assert(ret != -1);

    value = gputop_read_file_uint64(name);
    free(name);

    return value;
}

static int
find_intel_render_node(void)
{
    for (int i = 128; i < (128 + 16); i++) {
        if (read_device_param("renderD", i, "vendor") == 0x8086)
            return i;
    }

    return -1;
}

static int
open_render_node(struct intel_device *dev)
{
    char *name;
    int ret;
    int fd;

    int render = find_intel_render_node();
    if (render < 0)
        return -1;

    ret = asprintf(&name, "/dev/dri/renderD%u", render);
    assert(ret != -1);

    fd = open(name, O_RDWR);
    free(name);

    if (fd == -1)
        return -1;

    dev->device = read_device_param("renderD", render, "device");
    dev->subsystem_device = read_device_param("renderD",
                                              render, "subsystem_device");
    dev->subsystem_vendor = read_device_param("renderD",
                                              render, "subsystem_vendor");

    return fd;
}

bool
gputop_enumerate_metrics_via_sysfs(void)
{
    DIR *metrics_dir;
    struct dirent *entry1, *entry2;
    char buffer[128];
    int name_max;
    int entry_size;

    assert(drm_card >= 0);
    snprintf(buffer, sizeof(buffer), "/sys/class/drm/card%d/metrics", drm_card);

    metrics_dir = opendir(buffer);
    if (metrics_dir == NULL)
        return false;

    name_max = pathconf(buffer, _PC_NAME_MAX);

    if (name_max == -1)
        name_max = 255;

    entry_size = offsetof(struct dirent, d_name) + name_max + 1;
    entry1 = alloca(entry_size);

    while ((readdir_r(metrics_dir, entry1, &entry2) == 0) && entry2 != NULL)
    {
        struct gputop_metric_set *metric_set;
        struct gputop_hash_entry *metrics_entry;

        if (entry2->d_type != DT_DIR || entry2->d_name[0] == '.')
            continue;

        metrics_entry =
            gputop_hash_table_search(metrics, entry2->d_name);

        if (metrics_entry == NULL)
            continue;

        metric_set = (struct gputop_metric_set*)metrics_entry->data;

        snprintf(buffer, sizeof(buffer),
                 "/sys/class/drm/card%d/metrics/%s/id",
                 drm_card, entry2->d_name);

        metric_set->perf_oa_metrics_set = gputop_read_file_uint64(buffer);
        array_append(gputop_perf_oa_supported_metric_set_guids, &metric_set->guid);
    }
    closedir(metrics_dir);

    return true;
}

// function that hard-codes the guids specific for the broadwell configuration
bool
gputop_enumerate_metrics_fake(void)
{
    static const char *fake_bdw_guids[] = {
        "b541bd57-0e0f-4154-b4c0-5858010a2bf7",
        "35fbc9b2-a891-40a6-a38d-022bb7057552",
        "233d0544-fff7-4281-8291-e02f222aff72",
        "2b255d48-2117-4fef-a8f7-f151e1d25a2c",
        "f7fd3220-b466-4a4d-9f98-b0caf3f2394c",
        "e99ccaca-821c-4df9-97a7-96bdb7204e43",
        "27a364dc-8225-4ecb-b607-d6f1925598d9",
        "857fc630-2f09-4804-85f1-084adfadd5ab",
        "343ebc99-4a55-414c-8c17-d8e259cf5e20",
        "2cf0c064-68df-4fac-9b3f-57f51ca8a069",
        "78a87ff9-543a-49ce-95ea-26d86071ea93",
        "9f2cece5-7bfe-4320-ad66-8c7cc526bec5",
        "d890ef38-d309-47e4-b8b5-aa779bb19ab0",
        "5fdff4a6-9dc8-45e1-bfda-ef54869fbdd4",
        "2c0e45e1-7e2c-4a14-ae00-0b7ec868b8aa",
        "71148d78-baf5-474f-878a-e23158d0265d",
        "b996a2b7-c59c-492d-877a-8cd54fd6df84",
        "eb2fecba-b431-42e7-8261-fe9429a6e67a",
        "60749470-a648-4a4b-9f10-dbfe1e36e44d",
    };

    struct gputop_metric_set *metric_set;
    struct gputop_hash_entry *metrics_entry;

    int i;
    int array_length = sizeof(fake_bdw_guids) / sizeof(fake_bdw_guids[0]);

    for (i = 0; i < array_length; i++){
        metrics_entry = gputop_hash_table_search(metrics, fake_bdw_guids[i]);
        metric_set = (struct gputop_metric_set*)metrics_entry->data;
        metric_set->perf_oa_metrics_set = i;
        array_append(gputop_perf_oa_supported_metric_set_guids, &metric_set->guid);
    }

    return true;
}

/* The C code generated by oa-gen.py calls this function for each
 * metric set.
 */
void
gputop_register_oa_metric_set(struct gputop_metric_set *metric_set)
{
    gputop_hash_table_insert(metrics, metric_set->guid, metric_set);
}

bool
gputop_perf_initialize(void)
{
    if (gputop_devinfo.n_eus)
        return true;

    if (getenv("GPUTOP_FAKE_MODE") && strcmp(getenv("GPUTOP_FAKE_MODE"), "1") == 0) {
        gputop_fake_mode = true;
        intel_dev.device = 5654; // broadwell specific id
    } else {
        drm_fd = open_render_node(&intel_dev);
        if (drm_fd < 0) {
            gputop_log(GPUTOP_LOG_LEVEL_HIGH, "Failed to open render node", -1);
            return false;
        }
        drm_card = get_card_for_fd(drm_fd);
    }

    /* NB: eu_count needs to be initialized before declaring counters */
    init_dev_info(drm_fd, intel_dev.device);
    page_size = sysconf(_SC_PAGE_SIZE);

    metrics = gputop_hash_table_create(NULL, gputop_key_hash_string,
                                       gputop_key_string_equal);
    gputop_perf_oa_supported_metric_set_guids = array_new(sizeof(char*), 1);

    if (IS_HASWELL(intel_dev.device)) {
        gputop_oa_add_metrics_hsw(&gputop_devinfo);
    } else if (IS_BROADWELL(intel_dev.device)) {
        gputop_oa_add_metrics_bdw(&gputop_devinfo);
    } else if (IS_CHERRYVIEW(intel_dev.device)) {
        gputop_oa_add_metrics_chv(&gputop_devinfo);
    } else if (IS_SKYLAKE(intel_dev.device)) {
        gputop_oa_add_metrics_skl(&gputop_devinfo);
    } else if (IS_BROXTON(intel_dev.device)) {
        gputop_oa_add_metrics_bxt(&gputop_devinfo);
    } else
        assert(0);

    if (gputop_fake_mode)
        return gputop_enumerate_metrics_fake();
    else
        return gputop_enumerate_metrics_via_sysfs();
}

static void
free_perf_oa_metrics(struct gputop_hash_entry *entry)
{
    free(entry->data);
}

void
gputop_perf_free(void)
{
    gputop_hash_table_destroy(metrics, free_perf_oa_metrics);
    array_free(gputop_perf_oa_supported_metric_set_guids);
}

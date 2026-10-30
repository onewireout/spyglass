/* test: capture one frame into a GBM dmabuf and print what happened.
 * doesn't talk to spyglass or PipeWire, just checks resonance can hand us a dmabuf frame */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <dirent.h>
#include <wayland-client.h>
#include <gbm.h>
#include <xf86drm.h>

#include "ext-image-copy-capture-v1-client-protocol.h"
#include "ext-image-capture-source-v1-client-protocol.h"
#include "linux-dmabuf-v1-client-protocol.h"

#define MAX_MODIFIERS 64

struct state {
    struct wl_display *display;
    struct wl_output *output;
    struct ext_output_image_capture_source_manager_v1 *source_mgr;
    struct ext_image_copy_capture_manager_v1 *capture_mgr;
    struct zwp_linux_dmabuf_v1 *dmabuf;

    /* session constraints */
    uint32_t buf_w, buf_h;
    bool have_size;
    dev_t dmabuf_dev;
    bool have_dev;
    uint32_t chosen_format;
    uint64_t modifiers[MAX_MODIFIERS];
    int n_modifiers;
    bool have_format;
    bool constraints_done;

    /* frame result */
    bool frame_ready, frame_failed;
    uint32_t fail_reason;
};

/* ---- registry ---- */

static void on_global(void *data, struct wl_registry *reg, uint32_t name,
        const char *iface, uint32_t version) {
    struct state *s = data;
    if (!strcmp(iface, wl_output_interface.name) && !s->output) {
        s->output = wl_registry_bind(reg, name, &wl_output_interface, 4);
    } else if (!strcmp(iface, ext_output_image_capture_source_manager_v1_interface.name)) {
        s->source_mgr = wl_registry_bind(reg, name,
            &ext_output_image_capture_source_manager_v1_interface, 1);
    } else if (!strcmp(iface, ext_image_copy_capture_manager_v1_interface.name)) {
        s->capture_mgr = wl_registry_bind(reg, name,
            &ext_image_copy_capture_manager_v1_interface, 1);
    } else if (!strcmp(iface, zwp_linux_dmabuf_v1_interface.name)) {
        uint32_t v = version < 4 ? version : 4;
        s->dmabuf = wl_registry_bind(reg, name, &zwp_linux_dmabuf_v1_interface, v);
    }
    (void)version;
}
static void on_global_remove(void *d, struct wl_registry *r, uint32_t n) {}
static const struct wl_registry_listener registry_listener = {
    .global = on_global, .global_remove = on_global_remove,
};

/* ---- capture session ---- */

static void session_buffer_size(void *data, struct ext_image_copy_capture_session_v1 *sess,
        uint32_t w, uint32_t h) {
    struct state *s = data;
    s->buf_w = w; s->buf_h = h; s->have_size = true;
    (void)sess;
}
static void session_shm_format(void *data, struct ext_image_copy_capture_session_v1 *sess,
        uint32_t format) { (void)data; (void)sess; (void)format; }
static void session_dmabuf_device(void *data, struct ext_image_copy_capture_session_v1 *sess,
        struct wl_array *device) {
    struct state *s = data;
    if (device->size == sizeof(dev_t)) {
        memcpy(&s->dmabuf_dev, device->data, sizeof(dev_t));
        s->have_dev = true;
    }
    (void)sess;
}
static void session_dmabuf_format(void *data, struct ext_image_copy_capture_session_v1 *sess,
        uint32_t format, struct wl_array *modifiers) {
    struct state *s = data;
    if (s->have_format) return; /* just take the first format offered */
    s->chosen_format = format;
    s->n_modifiers = 0;
    uint64_t *m;
    wl_array_for_each(m, modifiers) {
        if (s->n_modifiers < MAX_MODIFIERS)
            s->modifiers[s->n_modifiers++] = *m;
    }
    s->have_format = true;
    (void)sess;
}
static void session_done(void *data, struct ext_image_copy_capture_session_v1 *sess) {
    struct state *s = data;
    s->constraints_done = true;
    (void)sess;
}
static void session_stopped(void *data, struct ext_image_copy_capture_session_v1 *sess) {
    fprintf(stderr, "probe: session stopped\n");
    (void)data; (void)sess;
}
static const struct ext_image_copy_capture_session_v1_listener session_listener = {
    .buffer_size    = session_buffer_size,
    .shm_format     = session_shm_format,
    .dmabuf_device  = session_dmabuf_device,
    .dmabuf_format  = session_dmabuf_format,
    .done           = session_done,
    .stopped        = session_stopped,
};

/* ---- frame ---- */

static void frame_transform(void *d, struct ext_image_copy_capture_frame_v1 *f, uint32_t t) {}
static void frame_damage(void *d, struct ext_image_copy_capture_frame_v1 *f,
        int32_t x, int32_t y, int32_t w, int32_t h) {}
static void frame_presentation_time(void *d, struct ext_image_copy_capture_frame_v1 *f,
        uint32_t hi, uint32_t lo, uint32_t ns) {}
static void frame_ready(void *data, struct ext_image_copy_capture_frame_v1 *f) {
    struct state *s = data;
    s->frame_ready = true;
    (void)f;
}
static void frame_failed(void *data, struct ext_image_copy_capture_frame_v1 *f, uint32_t reason) {
    struct state *s = data;
    s->frame_failed = true;
    s->fail_reason = reason;
    (void)f;
}
static const struct ext_image_copy_capture_frame_v1_listener frame_listener = {
    .transform         = frame_transform,
    .damage            = frame_damage,
    .presentation_time = frame_presentation_time,
    .ready             = frame_ready,
    .failed            = frame_failed,
};

/* ---- helpers ---- */

/* find the /dev/dri node matching the device the compositor gave us */
static int open_drm_node_for_devt(dev_t target) {
    DIR *dir = opendir("/dev/dri");
    if (!dir) return -1;
    struct dirent *ent;
    int fd = -1;
    while ((ent = readdir(dir))) {
        if (ent->d_name[0] == '.') continue;
        char path[256];
        snprintf(path, sizeof(path), "/dev/dri/%s", ent->d_name);
        struct stat st;
        if (stat(path, &st) < 0) continue;
        if (st.st_rdev == target) {
            fd = open(path, O_RDWR | O_CLOEXEC);
            fprintf(stderr, "probe: matched dmabuf_device to %s\n", path);
            break;
        }
    }
    closedir(dir);
    return fd;
}

int main(void) {
    struct state s = {0};
    s.display = wl_display_connect(NULL);
    if (!s.display) { fprintf(stderr, "probe: cannot connect to Wayland display\n"); return 1; }

    struct wl_registry *reg = wl_display_get_registry(s.display);
    wl_registry_add_listener(reg, &registry_listener, &s);
    wl_display_roundtrip(s.display);

    if (!s.output || !s.source_mgr || !s.capture_mgr || !s.dmabuf) {
        fprintf(stderr, "probe: missing globals (output=%s source_mgr=%s capture_mgr=%s dmabuf=%s)\n",
            s.output ? "ok" : "MISSING", s.source_mgr ? "ok" : "MISSING",
            s.capture_mgr ? "ok" : "MISSING", s.dmabuf ? "ok" : "MISSING");
        fprintf(stderr, "probe: this means resonance hasn't been rebuilt+restarted with the new managers yet.\n");
        return 1;
    }
    fprintf(stderr, "probe: all globals bound OK\n");

    struct ext_image_capture_source_v1 *source =
        ext_output_image_capture_source_manager_v1_create_source(s.source_mgr, s.output);

    struct ext_image_copy_capture_session_v1 *session =
        ext_image_copy_capture_manager_v1_create_session(s.capture_mgr, source, 0 /* no cursor */);
    ext_image_copy_capture_session_v1_add_listener(session, &session_listener, &s);

    while (!s.constraints_done) {
        if (wl_display_dispatch(s.display) < 0) {
            fprintf(stderr, "probe: display error waiting for constraints\n");
            return 1;
        }
    }

    if (!s.have_size || !s.have_format || !s.have_dev) {
        fprintf(stderr, "probe: incomplete constraints (size=%d format=%d dev=%d)\n",
            s.have_size, s.have_format, s.have_dev);
        return 1;
    }
    fprintf(stderr, "probe: session wants %ux%u, format=0x%08x, %d modifiers, dev_t=%lu\n",
        s.buf_w, s.buf_h, s.chosen_format, s.n_modifiers, (unsigned long)s.dmabuf_dev);

    int drm_fd = open_drm_node_for_devt(s.dmabuf_dev);
    if (drm_fd < 0) {
        fprintf(stderr, "probe: could not find/open matching DRM node\n");
        return 1;
    }

    struct gbm_device *gbm = gbm_create_device(drm_fd);
    if (!gbm) { fprintf(stderr, "probe: gbm_create_device failed\n"); return 1; }

    struct gbm_bo *bo = gbm_bo_create_with_modifiers2(gbm, s.buf_w, s.buf_h,
        s.chosen_format, s.modifiers, s.n_modifiers, GBM_BO_USE_RENDERING);
    if (!bo) {
        fprintf(stderr, "probe: gbm_bo_create_with_modifiers2 failed, trying no-modifier fallback\n");
        bo = gbm_bo_create(gbm, s.buf_w, s.buf_h, s.chosen_format,
            GBM_BO_USE_RENDERING | GBM_BO_USE_LINEAR);
    }
    if (!bo) { fprintf(stderr, "probe: gbm buffer allocation failed\n"); return 1; }

    int n_planes = gbm_bo_get_plane_count(bo);
    uint64_t modifier = gbm_bo_get_modifier(bo);

    struct zwp_linux_buffer_params_v1 *params = zwp_linux_dmabuf_v1_create_params(s.dmabuf);
    for (int i = 0; i < n_planes; i++) {
        int plane_fd = gbm_bo_get_fd_for_plane(bo, i);
        if (plane_fd < 0) { fprintf(stderr, "probe: gbm_bo_get_fd_for_plane failed\n"); return 1; }
        uint32_t offset = gbm_bo_get_offset(bo, i);
        uint32_t stride = gbm_bo_get_stride_for_plane(bo, i);
        zwp_linux_buffer_params_v1_add(params, plane_fd, i, offset, stride,
            (uint32_t)(modifier >> 32), (uint32_t)(modifier & 0xffffffff));
        close(plane_fd); /* wayland dups it internally */
    }

    struct wl_buffer *buffer = zwp_linux_buffer_params_v1_create_immed(
        params, (int32_t)s.buf_w, (int32_t)s.buf_h, s.chosen_format, 0);
    zwp_linux_buffer_params_v1_destroy(params);
    wl_display_roundtrip(s.display); /* surface any immediate protocol errors */

    fprintf(stderr, "probe: gbm buffer allocated (%d planes), wl_buffer created\n", n_planes);

    struct ext_image_copy_capture_frame_v1 *frame =
        ext_image_copy_capture_session_v1_create_frame(session);
    ext_image_copy_capture_frame_v1_add_listener(frame, &frame_listener, &s);
    ext_image_copy_capture_frame_v1_attach_buffer(frame, buffer);
    ext_image_copy_capture_frame_v1_damage_buffer(frame, 0, 0, (int32_t)s.buf_w, (int32_t)s.buf_h);
    ext_image_copy_capture_frame_v1_capture(frame);

    while (!s.frame_ready && !s.frame_failed) {
        if (wl_display_dispatch(s.display) < 0) {
            fprintf(stderr, "probe: display error waiting for frame\n");
            return 1;
        }
    }

    if (s.frame_failed) {
        fprintf(stderr, "probe: FRAME FAILED, reason=%u\n", s.fail_reason);
        return 1;
    }

    printf("probe: SUCCESS - captured one %ux%u dmabuf frame (format=0x%08x, %d planes)\n",
        s.buf_w, s.buf_h, s.chosen_format, n_planes);
    return 0;
}

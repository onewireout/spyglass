/* zero-copy screen capture with fast GPU readback
 * resonance blits the screen into our dmabuf, so it never does a slow CPU
 * readback in its render loop (that was stealing frames from games).
 * mmap of the dmabuf took ~580ms/frame, PBO readback takes ~0.6ms.
 * output format is the same as screencopy.c */
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
#include <time.h>
#include <wayland-client.h>
#include <gbm.h>
#include <xf86drm.h>

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <GLES2/gl2ext.h>

#include "ext-image-copy-capture-v1-client-protocol.h"
#include "ext-image-capture-source-v1-client-protocol.h"
#include "linux-dmabuf-v1-client-protocol.h"

#define MAGIC       0x52534350u  /* 'RSCP' -- same magic screencopy.c uses */
#define MAX_MODIFIERS 64
#define MAX_OUTPUTS   8

/* GL_RGBA readback has the same byte order as WL_SHM_FORMAT_ABGR8888,
 * so reuse that code and spyglass.py doesn't need changes */
#define OUT_FORMAT_CODE 0x34324142u /* ABGR8888 */

struct output_entry {
    struct wl_output *output;
    char name[64];
};

struct state {
    struct wl_display *display;
    struct wl_output *output;
    struct output_entry outputs[MAX_OUTPUTS];
    int output_count;
    struct ext_output_image_capture_source_manager_v1 *source_mgr;
    struct ext_image_copy_capture_manager_v1 *capture_mgr;
    struct zwp_linux_dmabuf_v1 *dmabuf;

    uint32_t buf_w, buf_h;
    bool have_size;
    dev_t dmabuf_dev;
    bool have_dev;
    uint32_t chosen_format;
    uint64_t modifiers[MAX_MODIFIERS];
    int n_modifiers;
    bool have_format;
    bool constraints_done;

    bool frame_ready, frame_failed;
    uint32_t fail_reason;
};

static bool write_all(const void *p, size_t n) {
    const uint8_t *b = p;
    while (n > 0) {
        ssize_t r = write(STDOUT_FILENO, b, n);
        if (r <= 0) return false;
        b += r; n -= r;
    }
    return true;
}

static void output_name_ev(void *d, struct wl_output *o, const char *name) {
    struct output_entry *e = d;
    strncpy(e->name, name ? name : "", sizeof(e->name) - 1);
    e->name[sizeof(e->name) - 1] = '\0';
    (void)o;
}
static void output_geometry_ev(void *d, struct wl_output *o, int32_t x, int32_t y,
        int32_t pw, int32_t ph, int32_t sub, const char *make, const char *model, int32_t t) {
    (void)d; (void)o; (void)x; (void)y; (void)pw; (void)ph; (void)sub; (void)make; (void)model; (void)t;
}
static void output_mode_ev(void *d, struct wl_output *o, uint32_t f, int32_t w, int32_t h, int32_t r) {
    (void)d; (void)o; (void)f; (void)w; (void)h; (void)r;
}
static void output_done_ev(void *d, struct wl_output *o) { (void)d; (void)o; }
static void output_scale_ev(void *d, struct wl_output *o, int32_t f) { (void)d; (void)o; (void)f; }
static void output_description_ev(void *d, struct wl_output *o, const char *desc) { (void)d; (void)o; (void)desc; }
static const struct wl_output_listener output_listener = {
    .geometry = output_geometry_ev, .mode = output_mode_ev, .done = output_done_ev,
    .scale = output_scale_ev, .name = output_name_ev, .description = output_description_ev,
};

static void on_global(void *data, struct wl_registry *reg, uint32_t name,
        const char *iface, uint32_t version) {
    struct state *s = data;
    if (!strcmp(iface, wl_output_interface.name) && s->output_count < MAX_OUTPUTS) {
        struct output_entry *e = &s->outputs[s->output_count++];
        e->name[0] = '\0';
        e->output = wl_registry_bind(reg, name, &wl_output_interface, 4);
        wl_output_add_listener(e->output, &output_listener, e);
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
static void on_global_remove(void *d, struct wl_registry *r, uint32_t n) { (void)d; (void)r; (void)n; }
static const struct wl_registry_listener registry_listener = { .global = on_global, .global_remove = on_global_remove };

static void session_buffer_size(void *data, struct ext_image_copy_capture_session_v1 *sess, uint32_t w, uint32_t h) {
    struct state *s = data; s->buf_w = w; s->buf_h = h; s->have_size = true; (void)sess;
}
static void session_shm_format(void *d, struct ext_image_copy_capture_session_v1 *s2, uint32_t f) { (void)d; (void)s2; (void)f; }
static void session_dmabuf_device(void *data, struct ext_image_copy_capture_session_v1 *sess, struct wl_array *device) {
    struct state *s = data;
    if (device->size == sizeof(dev_t)) { memcpy(&s->dmabuf_dev, device->data, sizeof(dev_t)); s->have_dev = true; }
    (void)sess;
}
static void session_dmabuf_format(void *data, struct ext_image_copy_capture_session_v1 *sess, uint32_t format, struct wl_array *modifiers) {
    struct state *s = data;
    if (s->have_format) return;
    s->chosen_format = format; s->n_modifiers = 0;
    uint64_t *m;
    wl_array_for_each(m, modifiers) { if (s->n_modifiers < MAX_MODIFIERS) s->modifiers[s->n_modifiers++] = *m; }
    s->have_format = true; (void)sess;
}
static void session_done(void *data, struct ext_image_copy_capture_session_v1 *sess) {
    struct state *s = data; s->constraints_done = true; (void)sess;
}
static void session_stopped(void *d, struct ext_image_copy_capture_session_v1 *s2) {
    fprintf(stderr, "capture_dmabuf: session stopped\n"); (void)d; (void)s2;
}
static const struct ext_image_copy_capture_session_v1_listener session_listener = {
    .buffer_size = session_buffer_size, .shm_format = session_shm_format,
    .dmabuf_device = session_dmabuf_device, .dmabuf_format = session_dmabuf_format,
    .done = session_done, .stopped = session_stopped,
};

static void frame_transform(void *d, struct ext_image_copy_capture_frame_v1 *f, uint32_t t) { (void)d; (void)f; (void)t; }
static void frame_damage(void *d, struct ext_image_copy_capture_frame_v1 *f, int32_t x, int32_t y, int32_t w, int32_t h) { (void)d; (void)f; (void)x; (void)y; (void)w; (void)h; }
static void frame_presentation_time(void *d, struct ext_image_copy_capture_frame_v1 *f, uint32_t hi, uint32_t lo, uint32_t ns) { (void)d; (void)f; (void)hi; (void)lo; (void)ns; }
static void frame_ready(void *data, struct ext_image_copy_capture_frame_v1 *f) { struct state *s = data; s->frame_ready = true; (void)f; }
static void frame_failed(void *data, struct ext_image_copy_capture_frame_v1 *f, uint32_t reason) { struct state *s = data; s->frame_failed = true; s->fail_reason = reason; (void)f; }
static const struct ext_image_copy_capture_frame_v1_listener frame_listener = {
    .transform = frame_transform, .damage = frame_damage, .presentation_time = frame_presentation_time,
    .ready = frame_ready, .failed = frame_failed,
};

static int open_drm_node_for_devt(dev_t target) {
    DIR *dir = opendir("/dev/dri");
    if (!dir) return -1;
    struct dirent *ent;
    int fd = -1;
    while ((ent = readdir(dir))) {
        if (ent->d_name[0] == '.') continue;
        char path[300];
        snprintf(path, sizeof(path), "/dev/dri/%s", ent->d_name);
        struct stat st;
        if (stat(path, &st) < 0) continue;
        if (st.st_rdev == target) {
            fd = open(path, O_RDWR | O_CLOEXEC);
            fprintf(stderr, "capture_dmabuf: matched dmabuf_device to %s\n", path);
            break;
        }
    }
    closedir(dir);
    return fd;
}

int main(int argc, char *argv[]) {
    const char *output_name_arg = NULL;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--output-name") && i + 1 < argc) {
            output_name_arg = argv[++i];
        }
    }

    struct state s = {0};
    s.display = wl_display_connect(NULL);
    if (!s.display) { fprintf(stderr, "capture_dmabuf: cannot connect to Wayland display\n"); return 1; }

    struct wl_registry *reg = wl_display_get_registry(s.display);
    wl_registry_add_listener(reg, &registry_listener, &s);
    wl_display_roundtrip(s.display);
    wl_display_roundtrip(s.display); /* flush wl_output name events */

    if (s.output_count == 0 || !s.source_mgr || !s.capture_mgr || !s.dmabuf) {
        fprintf(stderr, "capture_dmabuf: missing globals (outputs=%d source_mgr=%s capture_mgr=%s dmabuf=%s)\n",
            s.output_count, s.source_mgr ? "ok" : "MISSING",
            s.capture_mgr ? "ok" : "MISSING", s.dmabuf ? "ok" : "MISSING");
        return 1;
    }

    int chosen_idx = -1;
    if (output_name_arg) {
        for (int i = 0; i < s.output_count; i++) {
            if (!strcmp(s.outputs[i].name, output_name_arg)) { chosen_idx = i; break; }
        }
        if (chosen_idx < 0) {
            fprintf(stderr, "capture_dmabuf: output '%s' not found\n", output_name_arg);
            return 1;
        }
    } else {
        chosen_idx = 0;
    }
    s.output = s.outputs[chosen_idx].output;
    fprintf(stderr, "capture_dmabuf: using output '%s'\n", s.outputs[chosen_idx].name);

    struct ext_image_capture_source_v1 *source =
        ext_output_image_capture_source_manager_v1_create_source(s.source_mgr, s.output);
    struct ext_image_copy_capture_session_v1 *session =
        ext_image_copy_capture_manager_v1_create_session(s.capture_mgr, source, 0);
    ext_image_copy_capture_session_v1_add_listener(session, &session_listener, &s);

    while (!s.constraints_done) {
        if (wl_display_dispatch(s.display) < 0) { fprintf(stderr, "capture_dmabuf: display error\n"); return 1; }
    }
    if (!s.have_size || !s.have_format || !s.have_dev) {
        fprintf(stderr, "capture_dmabuf: incomplete constraints\n");
        return 1;
    }
    fprintf(stderr, "capture_dmabuf: %ux%u format=0x%08x %d modifiers dev_t=%lu\n",
        s.buf_w, s.buf_h, s.chosen_format, s.n_modifiers, (unsigned long)s.dmabuf_dev);

    int drm_fd = open_drm_node_for_devt(s.dmabuf_dev);
    if (drm_fd < 0) { fprintf(stderr, "capture_dmabuf: could not open matching DRM node\n"); return 1; }
    struct gbm_device *gbm = gbm_create_device(drm_fd);
    if (!gbm) { fprintf(stderr, "capture_dmabuf: gbm_create_device failed\n"); return 1; }

    /* one buffer is enough, readback is under 1ms */
    uint64_t linear = 0;
    struct gbm_bo *bo = gbm_bo_create_with_modifiers2(gbm, s.buf_w, s.buf_h,
        s.chosen_format, &linear, 1, GBM_BO_USE_RENDERING);
    if (!bo) {
        fprintf(stderr, "capture_dmabuf: linear allocation failed, trying driver default\n");
        bo = gbm_bo_create(gbm, s.buf_w, s.buf_h, s.chosen_format, GBM_BO_USE_RENDERING | GBM_BO_USE_LINEAR);
    }
    if (!bo) { fprintf(stderr, "capture_dmabuf: gbm buffer allocation failed\n"); return 1; }

    uint32_t offset = gbm_bo_get_offset(bo, 0);
    uint32_t stride = gbm_bo_get_stride_for_plane(bo, 0);
    uint64_t modifier = gbm_bo_get_modifier(bo);

    struct zwp_linux_buffer_params_v1 *params = zwp_linux_dmabuf_v1_create_params(s.dmabuf);
    int wire_fd = gbm_bo_get_fd_for_plane(bo, 0);
    zwp_linux_buffer_params_v1_add(params, wire_fd, 0, offset, stride,
        (uint32_t)(modifier >> 32), (uint32_t)(modifier & 0xffffffff));
    close(wire_fd);
    struct wl_buffer *wl_buf = zwp_linux_buffer_params_v1_create_immed(
        params, (int32_t)s.buf_w, (int32_t)s.buf_h, s.chosen_format, 0);
    zwp_linux_buffer_params_v1_destroy(params);
    wl_display_roundtrip(s.display);

    /* EGL/GL setup for GPU readback (see pbo_readback_test.c) */
    PFNEGLGETPLATFORMDISPLAYEXTPROC eglGetPlatformDisplayEXT =
        (PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");
    if (!eglGetPlatformDisplayEXT) { fprintf(stderr, "capture_dmabuf: no eglGetPlatformDisplayEXT\n"); return 1; }

    EGLDisplay egl_dpy = eglGetPlatformDisplayEXT(EGL_PLATFORM_GBM_KHR, gbm, NULL);
    if (egl_dpy == EGL_NO_DISPLAY) { fprintf(stderr, "capture_dmabuf: eglGetPlatformDisplayEXT failed\n"); return 1; }

    EGLint major, minor;
    if (!eglInitialize(egl_dpy, &major, &minor)) { fprintf(stderr, "capture_dmabuf: eglInitialize failed\n"); return 1; }

    eglBindAPI(EGL_OPENGL_ES_API);
    static const EGLint config_attribs[] = { EGL_SURFACE_TYPE, EGL_DONT_CARE, EGL_NONE };
    EGLConfig config;
    EGLint n_configs;
    if (!eglChooseConfig(egl_dpy, config_attribs, &config, 1, &n_configs) || n_configs < 1) {
        fprintf(stderr, "capture_dmabuf: eglChooseConfig failed\n"); return 1;
    }
    static const EGLint ctx_attribs[] = { EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE };
    EGLContext egl_ctx = eglCreateContext(egl_dpy, config, EGL_NO_CONTEXT, ctx_attribs);
    if (egl_ctx == EGL_NO_CONTEXT) { fprintf(stderr, "capture_dmabuf: eglCreateContext failed\n"); return 1; }
    if (!eglMakeCurrent(egl_dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, egl_ctx)) {
        fprintf(stderr, "capture_dmabuf: eglMakeCurrent failed\n"); return 1;
    }

    PFNEGLCREATEIMAGEKHRPROC eglCreateImageKHR = (PFNEGLCREATEIMAGEKHRPROC)eglGetProcAddress("eglCreateImageKHR");
    PFNGLEGLIMAGETARGETTEXTURE2DOESPROC glEGLImageTargetTexture2DOES =
        (PFNGLEGLIMAGETARGETTEXTURE2DOESPROC)eglGetProcAddress("glEGLImageTargetTexture2DOES");
    if (!eglCreateImageKHR || !glEGLImageTargetTexture2DOES) {
        fprintf(stderr, "capture_dmabuf: missing EGL image extensions\n"); return 1;
    }

    int import_fd = gbm_bo_get_fd_for_plane(bo, 0);
    EGLint img_attribs[] = {
        EGL_WIDTH, (EGLint)s.buf_w,
        EGL_HEIGHT, (EGLint)s.buf_h,
        EGL_LINUX_DRM_FOURCC_EXT, (EGLint)s.chosen_format,
        EGL_DMA_BUF_PLANE0_FD_EXT, import_fd,
        EGL_DMA_BUF_PLANE0_OFFSET_EXT, (EGLint)offset,
        EGL_DMA_BUF_PLANE0_PITCH_EXT, (EGLint)stride,
        EGL_NONE
    };
    EGLImageKHR image = eglCreateImageKHR(egl_dpy, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, NULL, img_attribs);
    if (image == EGL_NO_IMAGE_KHR) { fprintf(stderr, "capture_dmabuf: eglCreateImageKHR failed: 0x%x\n", eglGetError()); return 1; }

    GLuint tex;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, image);
    if (glGetError() != GL_NO_ERROR) { fprintf(stderr, "capture_dmabuf: glEGLImageTargetTexture2DOES failed\n"); return 1; }

    GLuint fbo;
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        fprintf(stderr, "capture_dmabuf: FBO incomplete\n"); return 1;
    }

    size_t frame_size = (size_t)s.buf_w * s.buf_h * 4;
    GLuint pbo;
    glGenBuffers(1, &pbo);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, pbo);
    glBufferData(GL_PIXEL_PACK_BUFFER, (GLsizeiptr)frame_size, NULL, GL_STREAM_READ);

    fprintf(stderr, "capture_dmabuf: EGL/GL readback pipeline ready, starting capture loop\n");
    if (!freopen(NULL, "wb", stdout)) return 1;

    while (1) {
        struct ext_image_copy_capture_frame_v1 *frame =
            ext_image_copy_capture_session_v1_create_frame(session);
        ext_image_copy_capture_frame_v1_add_listener(frame, &frame_listener, &s);
        ext_image_copy_capture_frame_v1_attach_buffer(frame, wl_buf);
        ext_image_copy_capture_frame_v1_damage_buffer(frame, 0, 0, (int32_t)s.buf_w, (int32_t)s.buf_h);
        ext_image_copy_capture_frame_v1_capture(frame);

        s.frame_ready = false;
        s.frame_failed = false;
        while (!s.frame_ready && !s.frame_failed) {
            if (wl_display_dispatch(s.display) < 0) {
                fprintf(stderr, "capture_dmabuf: display error in loop\n");
                return 1;
            }
        }
        ext_image_copy_capture_frame_v1_destroy(frame);

        if (s.frame_failed) {
            fprintf(stderr, "capture_dmabuf: frame failed, reason=%u\n", s.fail_reason);
            if (s.fail_reason == EXT_IMAGE_COPY_CAPTURE_FRAME_V1_FAILURE_REASON_STOPPED) return 1;
            continue;
        }

        glReadPixels(0, 0, (GLsizei)s.buf_w, (GLsizei)s.buf_h, GL_RGBA, GL_UNSIGNED_BYTE, 0);
        /* no glFinish(), glMapBufferRange already waits for the transfer.
         * glFinish stalled the whole GPU 60 times a second */
        void *mapped = glMapBufferRange(GL_PIXEL_PACK_BUFFER, 0, (GLsizeiptr)frame_size, GL_MAP_READ_BIT);
        if (!mapped) {
            fprintf(stderr, "capture_dmabuf: glMapBufferRange failed\n");
            return 1;
        }

        uint32_t hdr[4] = { MAGIC, s.buf_w, s.buf_h, OUT_FORMAT_CODE };
        if (!write_all(hdr, sizeof(hdr))) { glUnmapBuffer(GL_PIXEL_PACK_BUFFER); return 1; }
        if (!write_all(mapped, frame_size)) { glUnmapBuffer(GL_PIXEL_PACK_BUFFER); return 1; }
        glUnmapBuffer(GL_PIXEL_PACK_BUFFER);

        /* cap at the target fps. in a 144fps game frames come almost every
         * repaint and readback is fast enough that nothing else throttles it,
         * so without this the PipeWire queue grows forever */
        nanosleep(&(struct timespec){ 0, 16666666L }, NULL);
    }
}

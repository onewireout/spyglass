/* test: capture one frame into a dmabuf (like dmabuf_probe.c), then time
 * reading it back through an EGLImage + PBO instead of mmap (~580ms/frame) */
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

#define MAX_MODIFIERS 64

struct state {
    struct wl_display *display;
    struct wl_output *output;
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
static void session_stopped(void *d, struct ext_image_copy_capture_session_v1 *s2) { (void)d; (void)s2; }
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
        if (st.st_rdev == target) { fd = open(path, O_RDWR | O_CLOEXEC); break; }
    }
    closedir(dir);
    return fd;
}

static double now_secs(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

int main(void) {
    struct state s = {0};
    s.display = wl_display_connect(NULL);
    if (!s.display) { fprintf(stderr, "cannot connect to wayland\n"); return 1; }

    struct wl_registry *reg = wl_display_get_registry(s.display);
    wl_registry_add_listener(reg, &registry_listener, &s);
    wl_display_roundtrip(s.display);

    if (!s.output || !s.source_mgr || !s.capture_mgr || !s.dmabuf) {
        fprintf(stderr, "missing globals\n"); return 1;
    }

    struct ext_image_capture_source_v1 *source =
        ext_output_image_capture_source_manager_v1_create_source(s.source_mgr, s.output);
    struct ext_image_copy_capture_session_v1 *session =
        ext_image_copy_capture_manager_v1_create_session(s.capture_mgr, source, 0);
    ext_image_copy_capture_session_v1_add_listener(session, &session_listener, &s);

    while (!s.constraints_done) {
        if (wl_display_dispatch(s.display) < 0) { fprintf(stderr, "display error\n"); return 1; }
    }
    if (!s.have_size || !s.have_format || !s.have_dev) { fprintf(stderr, "incomplete constraints\n"); return 1; }
    fprintf(stderr, "capture: %ux%u format=0x%08x dev_t=%lu\n", s.buf_w, s.buf_h, s.chosen_format, (unsigned long)s.dmabuf_dev);

    int drm_fd = open_drm_node_for_devt(s.dmabuf_dev);
    if (drm_fd < 0) { fprintf(stderr, "could not open DRM node\n"); return 1; }
    struct gbm_device *gbm = gbm_create_device(drm_fd);
    if (!gbm) { fprintf(stderr, "gbm_create_device failed\n"); return 1; }

    uint64_t linear = 0;
    struct gbm_bo *bo = gbm_bo_create_with_modifiers2(gbm, s.buf_w, s.buf_h, s.chosen_format, &linear, 1, GBM_BO_USE_RENDERING);
    if (!bo) { fprintf(stderr, "gbm_bo_create_with_modifiers2 failed\n"); return 1; }

    int n_planes = gbm_bo_get_plane_count(bo);
    uint64_t modifier = gbm_bo_get_modifier(bo);
    int plane_fd = gbm_bo_get_fd_for_plane(bo, 0);
    uint32_t offset = gbm_bo_get_offset(bo, 0);
    uint32_t stride = gbm_bo_get_stride_for_plane(bo, 0);
    fprintf(stderr, "gbm buffer: %d planes, stride=%u\n", n_planes, stride);

    struct zwp_linux_buffer_params_v1 *params = zwp_linux_dmabuf_v1_create_params(s.dmabuf);
    int wire_fd = gbm_bo_get_fd_for_plane(bo, 0);
    zwp_linux_buffer_params_v1_add(params, wire_fd, 0, offset, stride,
        (uint32_t)(modifier >> 32), (uint32_t)(modifier & 0xffffffff));
    close(wire_fd);
    struct wl_buffer *wl_buf = zwp_linux_buffer_params_v1_create_immed(params, (int32_t)s.buf_w, (int32_t)s.buf_h, s.chosen_format, 0);
    zwp_linux_buffer_params_v1_destroy(params);
    wl_display_roundtrip(s.display);

    struct ext_image_copy_capture_frame_v1 *frame = ext_image_copy_capture_session_v1_create_frame(session);
    ext_image_copy_capture_frame_v1_add_listener(frame, &frame_listener, &s);
    ext_image_copy_capture_frame_v1_attach_buffer(frame, wl_buf);
    ext_image_copy_capture_frame_v1_damage_buffer(frame, 0, 0, (int32_t)s.buf_w, (int32_t)s.buf_h);
    ext_image_copy_capture_frame_v1_capture(frame);
    while (!s.frame_ready && !s.frame_failed) {
        if (wl_display_dispatch(s.display) < 0) { fprintf(stderr, "display error\n"); return 1; }
    }
    if (s.frame_failed) { fprintf(stderr, "frame failed\n"); return 1; }
    fprintf(stderr, "frame captured, starting EGL/GL readback test\n");

    /* ---- EGL setup on the same DRM device ---- */
    PFNEGLGETPLATFORMDISPLAYEXTPROC eglGetPlatformDisplayEXT =
        (PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");
    if (!eglGetPlatformDisplayEXT) { fprintf(stderr, "no eglGetPlatformDisplayEXT\n"); return 1; }

    EGLDisplay egl_dpy = eglGetPlatformDisplayEXT(EGL_PLATFORM_GBM_KHR, gbm, NULL);
    if (egl_dpy == EGL_NO_DISPLAY) { fprintf(stderr, "eglGetPlatformDisplayEXT failed\n"); return 1; }

    EGLint major, minor;
    if (!eglInitialize(egl_dpy, &major, &minor)) { fprintf(stderr, "eglInitialize failed\n"); return 1; }
    fprintf(stderr, "EGL %d.%d\n", major, minor);

    eglBindAPI(EGL_OPENGL_ES_API);

    static const EGLint config_attribs[] = { EGL_SURFACE_TYPE, EGL_DONT_CARE, EGL_NONE };
    EGLConfig config;
    EGLint n_configs;
    if (!eglChooseConfig(egl_dpy, config_attribs, &config, 1, &n_configs) || n_configs < 1) {
        fprintf(stderr, "eglChooseConfig failed\n"); return 1;
    }

    static const EGLint ctx_attribs[] = { EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE };
    EGLContext egl_ctx = eglCreateContext(egl_dpy, config, EGL_NO_CONTEXT, ctx_attribs);
    if (egl_ctx == EGL_NO_CONTEXT) { fprintf(stderr, "eglCreateContext failed\n"); return 1; }

    if (!eglMakeCurrent(egl_dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, egl_ctx)) {
        fprintf(stderr, "eglMakeCurrent failed (need EGL_KHR_surfaceless_context)\n"); return 1;
    }
    fprintf(stderr, "EGL context current\n");

    PFNEGLCREATEIMAGEKHRPROC eglCreateImageKHR = (PFNEGLCREATEIMAGEKHRPROC)eglGetProcAddress("eglCreateImageKHR");
    PFNEGLDESTROYIMAGEKHRPROC eglDestroyImageKHR = (PFNEGLDESTROYIMAGEKHRPROC)eglGetProcAddress("eglDestroyImageKHR");
    PFNGLEGLIMAGETARGETTEXTURE2DOESPROC glEGLImageTargetTexture2DOES =
        (PFNGLEGLIMAGETARGETTEXTURE2DOESPROC)eglGetProcAddress("glEGLImageTargetTexture2DOES");
    if (!eglCreateImageKHR || !glEGLImageTargetTexture2DOES) { fprintf(stderr, "missing EGL image extensions\n"); return 1; }

    /* fresh fd for EGL, wayland already took the first one */
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
    if (image == EGL_NO_IMAGE_KHR) { fprintf(stderr, "eglCreateImageKHR failed: 0x%x\n", eglGetError()); return 1; }
    fprintf(stderr, "EGLImage created from dmabuf\n");

    GLuint tex;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, image);
    GLenum gl_err = glGetError();
    if (gl_err != GL_NO_ERROR) { fprintf(stderr, "glEGLImageTargetTexture2DOES error: 0x%x\n", gl_err); return 1; }
    fprintf(stderr, "texture bound to EGLImage\n");

    GLuint fbo;
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
    GLenum fbo_status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (fbo_status != GL_FRAMEBUFFER_COMPLETE) { fprintf(stderr, "FBO incomplete: 0x%x\n", fbo_status); return 1; }
    fprintf(stderr, "FBO complete\n");

    size_t frame_size = (size_t)s.buf_w * s.buf_h * 4;
    GLuint pbo;
    glGenBuffers(1, &pbo);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, pbo);
    glBufferData(GL_PIXEL_PACK_BUFFER, (GLsizeiptr)frame_size, NULL, GL_STREAM_READ);

    /* warm-up read, the first one includes driver setup */
    double t0 = now_secs();
    glReadPixels(0, 0, (GLsizei)s.buf_w, (GLsizei)s.buf_h, GL_RGBA, GL_UNSIGNED_BYTE, 0);
    glFinish();
    double t1 = now_secs();
    fprintf(stderr, "warm-up glReadPixels+glFinish: %.4fs\n", t1 - t0);

    void *mapped = glMapBufferRange(GL_PIXEL_PACK_BUFFER, 0, (GLsizeiptr)frame_size, GL_MAP_READ_BIT);
    double t2 = now_secs();
    fprintf(stderr, "glMapBufferRange: %.4fs\n", t2 - t1);
    if (!mapped) { fprintf(stderr, "glMapBufferRange failed\n"); return 1; }

    uint8_t *bytes = mapped;
    size_t nonzero = 0;
    for (size_t i = 0; i < 1000000 && i < frame_size; i++) if (bytes[i] != 0) nonzero++;
    printf("warm-up: nonzero bytes in first 1M: %zu, first pixel: %02x%02x%02x%02x\n",
        nonzero, bytes[0], bytes[1], bytes[2], bytes[3]);
    glUnmapBuffer(GL_PIXEL_PACK_BUFFER);

    /* timed reads for a steady number */
    for (int i = 0; i < 5; i++) {
        double a = now_secs();
        glReadPixels(0, 0, (GLsizei)s.buf_w, (GLsizei)s.buf_h, GL_RGBA, GL_UNSIGNED_BYTE, 0);
        glFinish();
        double b = now_secs();
        void *m2 = glMapBufferRange(GL_PIXEL_PACK_BUFFER, 0, (GLsizeiptr)frame_size, GL_MAP_READ_BIT);
        double c = now_secs();
        glUnmapBuffer(GL_PIXEL_PACK_BUFFER);
        printf("iter %d: glReadPixels+glFinish=%.4fs map=%.4fs total=%.4fs\n", i, b - a, c - b, c - a);
    }

    eglDestroyImageKHR(egl_dpy, image);
    close(import_fd);
    return 0;
}

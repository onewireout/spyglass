#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include <unistd.h>
#include <sys/mman.h>
#include <time.h>
#include <wayland-client.h>
#include "wlr-screencopy-unstable-v1-client-protocol.h"
#include "wlr-foreign-toplevel-management-unstable-v1-client-protocol.h"
#include "xdg-output-unstable-v1-client-protocol.h"

#define MAGIC       0x52534350u  /* 'RSCP' */
#define MAX_OUTPUTS 8
#define MAX_WINDOWS 256

/* ---- screencopy state ---- */

struct output_entry {
    struct wl_output *output;
    struct zxdg_output_v1 *xdg_output;
    char name[64];
    int32_t ox, oy;  /* global position */
    int32_t ow, oh;  /* pixel dimensions (current mode) */
    bool has_logical_pos;
};

struct state {
    struct wl_display  *display;
    struct wl_shm      *shm;
    struct output_entry outputs[MAX_OUTPUTS];
    int                 output_count;
    struct zwlr_screencopy_manager_v1 *manager;
    struct zxdg_output_manager_v1 *xdg_output_manager;
};

struct frame {
    struct state *st;
    struct zwlr_screencopy_frame_v1 *wlr_frame;
    struct wl_buffer *buffer;
    void   *data;
    size_t  buf_size;
    uint32_t width, height, stride, format;
    uint32_t crop_w, crop_h;  /* requested region size; 0 = use full buffer */
    bool y_invert, got_shm, done, failed;
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

static void on_buffer(void *data, struct zwlr_screencopy_frame_v1 *wlr_frame,
        uint32_t format, uint32_t width, uint32_t height, uint32_t stride) {
    struct frame *f = data;
    if (!f->got_shm) {
        f->format = format; f->width = width;
        f->height = height; f->stride = stride;
        f->got_shm = true;
    }
}

static void on_linux_dmabuf(void *data, struct zwlr_screencopy_frame_v1 *wlr_frame,
        uint32_t format, uint32_t width, uint32_t height) {}

static void on_buffer_done(void *data, struct zwlr_screencopy_frame_v1 *wlr_frame) {
    struct frame *f = data;
    if (!f->got_shm) { f->failed = true; return; }

    f->buf_size = (size_t)f->stride * f->height;
    int fd = memfd_create("screencopy", 0);
    if (fd < 0 || ftruncate(fd, (off_t)f->buf_size) < 0) {
        if (fd >= 0) close(fd);
        f->failed = true; return;
    }
    f->data = mmap(NULL, f->buf_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (f->data == MAP_FAILED) { close(fd); f->failed = true; return; }

    struct wl_shm_pool *pool = wl_shm_create_pool(f->st->shm, fd, (int32_t)f->buf_size);
    close(fd);
    f->buffer = wl_shm_pool_create_buffer(pool, 0,
            (int32_t)f->width, (int32_t)f->height, (int32_t)f->stride, f->format);
    wl_shm_pool_destroy(pool);
    zwlr_screencopy_frame_v1_copy(wlr_frame, f->buffer);
}

static void on_flags(void *data, struct zwlr_screencopy_frame_v1 *wlr_frame, uint32_t flags) {
    ((struct frame *)data)->y_invert = (flags & ZWLR_SCREENCOPY_FRAME_V1_FLAGS_Y_INVERT) != 0;
}

static void on_ready(void *data, struct zwlr_screencopy_frame_v1 *wlr_frame,
        uint32_t tv_sec_hi, uint32_t tv_sec_lo, uint32_t tv_nsec) {
    ((struct frame *)data)->done = true;
}

static void on_failed(void *data, struct zwlr_screencopy_frame_v1 *wlr_frame) {
    ((struct frame *)data)->failed = true;
}

static void on_damage(void *data, struct zwlr_screencopy_frame_v1 *wlr_frame,
        uint32_t x, uint32_t y, uint32_t width, uint32_t height) {}

static const struct zwlr_screencopy_frame_v1_listener frame_listener = {
    .buffer       = on_buffer,
    .linux_dmabuf = on_linux_dmabuf,
    .buffer_done  = on_buffer_done,
    .flags        = on_flags,
    .ready        = on_ready,
    .failed       = on_failed,
    .damage       = on_damage,
};

static void output_geometry(void *d, struct wl_output *o,
        int32_t x, int32_t y, int32_t pw, int32_t ph,
        int32_t sub, const char *make, const char *model, int32_t t) {
    struct output_entry *e = d;
    /* wlroots leaves wl_output.geometry at 0,0, the real position comes
     * from xdg-output below. kept as a fallback for other compositors */
    if (!e->has_logical_pos) {
        e->ox = x; e->oy = y;
    }
    (void)pw; (void)ph; (void)sub; (void)make; (void)model; (void)t; (void)o;
}
static void output_mode(void *d, struct wl_output *o,
        uint32_t f, int32_t w, int32_t h, int32_t r) {
    if (f & WL_OUTPUT_MODE_CURRENT) {
        struct output_entry *e = d;
        e->ow = w; e->oh = h;
    }
    (void)o; (void)r;
}
static void output_done(void *d, struct wl_output *o) {}
static void output_scale(void *d, struct wl_output *o, int32_t f) {}
static void output_name(void *d, struct wl_output *o, const char *name) {
    struct output_entry *e = d;
    strncpy(e->name, name ? name : "", sizeof(e->name) - 1);
    e->name[sizeof(e->name) - 1] = '\0';
}
static void output_description(void *d, struct wl_output *o, const char *desc) {}

static const struct wl_output_listener output_listener = {
    .geometry    = output_geometry,
    .mode        = output_mode,
    .done        = output_done,
    .scale       = output_scale,
    .name        = output_name,
    .description = output_description,
};

static void xdg_output_logical_position(void *d, struct zxdg_output_v1 *xo,
        int32_t x, int32_t y) {
    struct output_entry *e = d;
    e->ox = x; e->oy = y;
    e->has_logical_pos = true;
    (void)xo;
}
static void xdg_output_logical_size(void *d, struct zxdg_output_v1 *xo, int32_t w, int32_t h) {
    (void)d; (void)xo; (void)w; (void)h;
}
static void xdg_output_done(void *d, struct zxdg_output_v1 *xo) { (void)d; (void)xo; }
static void xdg_output_name(void *d, struct zxdg_output_v1 *xo, const char *name) {
    (void)d; (void)xo; (void)name;
}
static void xdg_output_description(void *d, struct zxdg_output_v1 *xo, const char *desc) {
    (void)d; (void)xo; (void)desc;
}

static const struct zxdg_output_v1_listener xdg_output_listener = {
    .logical_position = xdg_output_logical_position,
    .logical_size     = xdg_output_logical_size,
    .done             = xdg_output_done,
    .name             = xdg_output_name,
    .description      = xdg_output_description,
};

static void on_global(void *data, struct wl_registry *reg,
        uint32_t name, const char *iface, uint32_t version) {
    struct state *s = data;
    if (!strcmp(iface, wl_shm_interface.name)) {
        s->shm = wl_registry_bind(reg, name, &wl_shm_interface, 1);
    } else if (!strcmp(iface, wl_output_interface.name) && s->output_count < MAX_OUTPUTS) {
        uint32_t v = version < 4 ? version : 4;
        struct output_entry *e = &s->outputs[s->output_count++];
        e->name[0] = '\0';
        e->output = wl_registry_bind(reg, name, &wl_output_interface, v);
        wl_output_add_listener(e->output, &output_listener, e);
    } else if (!strcmp(iface, zwlr_screencopy_manager_v1_interface.name)) {
        uint32_t v = version < 3 ? version : 3;
        s->manager = wl_registry_bind(reg, name, &zwlr_screencopy_manager_v1_interface, v);
    } else if (!strcmp(iface, zxdg_output_manager_v1_interface.name)) {
        uint32_t v = version < 3 ? version : 3;
        s->xdg_output_manager = wl_registry_bind(reg, name, &zxdg_output_manager_v1_interface, v);
    }
}

static void on_global_remove(void *d, struct wl_registry *r, uint32_t n) {}

static const struct wl_registry_listener registry_listener = {
    .global        = on_global,
    .global_remove = on_global_remove,
};

/* ---- window listing ---- */

struct win_entry {
    char *title;
    char *app_id;
    bool activated;
    struct wl_output *first_output;
};

static struct win_entry wins[MAX_WINDOWS];
static int win_count = 0;

static void win_title(void *d, struct zwlr_foreign_toplevel_handle_v1 *h, const char *t) {
    struct win_entry *e = d;
    free(e->title); e->title = strdup(t ? t : "");
}
static void win_app_id(void *d, struct zwlr_foreign_toplevel_handle_v1 *h, const char *id) {
    struct win_entry *e = d;
    free(e->app_id); e->app_id = strdup(id ? id : "");
}
static void win_output_enter(void *d, struct zwlr_foreign_toplevel_handle_v1 *h, struct wl_output *o) {
    struct win_entry *e = d;
    if (!e->first_output)
        e->first_output = o;
}
static void win_output_leave(void *d, struct zwlr_foreign_toplevel_handle_v1 *h, struct wl_output *o) {}
static void win_state(void *d, struct zwlr_foreign_toplevel_handle_v1 *h, struct wl_array *a) {
    struct win_entry *e = d;
    uint32_t *val;
    wl_array_for_each(val, a) {
        if (*val == ZWLR_FOREIGN_TOPLEVEL_HANDLE_V1_STATE_ACTIVATED)
            e->activated = true;
    }
}
static void win_done(void *d, struct zwlr_foreign_toplevel_handle_v1 *h) {}
static void win_closed(void *d, struct zwlr_foreign_toplevel_handle_v1 *h) {}
static void win_parent(void *d, struct zwlr_foreign_toplevel_handle_v1 *h,
        struct zwlr_foreign_toplevel_handle_v1 *p) {}

static const struct zwlr_foreign_toplevel_handle_v1_listener win_handle_listener = {
    .title        = win_title,
    .app_id       = win_app_id,
    .output_enter = win_output_enter,
    .output_leave = win_output_leave,
    .state        = win_state,
    .done         = win_done,
    .closed       = win_closed,
    .parent       = win_parent,
};

static void win_mgr_toplevel(void *d, struct zwlr_foreign_toplevel_manager_v1 *mgr,
        struct zwlr_foreign_toplevel_handle_v1 *handle) {
    if (win_count >= MAX_WINDOWS) return;
    struct win_entry *e = &wins[win_count++];
    e->title = NULL; e->app_id = NULL;
    e->activated = false; e->first_output = NULL;
    zwlr_foreign_toplevel_handle_v1_add_listener(handle, &win_handle_listener, e);
}
static void win_mgr_finished(void *d, struct zwlr_foreign_toplevel_manager_v1 *mgr) {}

static const struct zwlr_foreign_toplevel_manager_v1_listener win_mgr_listener = {
    .toplevel = win_mgr_toplevel,
    .finished = win_mgr_finished,
};

struct list_state {
    struct wl_display *display;
    struct zwlr_foreign_toplevel_manager_v1 *manager;
    struct output_entry outputs[MAX_OUTPUTS];
    int output_count;
};

static void list_on_global(void *data, struct wl_registry *reg,
        uint32_t name, const char *iface, uint32_t version) {
    struct list_state *s = data;
    if (!strcmp(iface, zwlr_foreign_toplevel_manager_v1_interface.name)) {
        uint32_t v = version < 3 ? version : 3;
        s->manager = wl_registry_bind(reg, name,
                &zwlr_foreign_toplevel_manager_v1_interface, v);
    } else if (!strcmp(iface, wl_output_interface.name) && s->output_count < MAX_OUTPUTS) {
        uint32_t v = version < 4 ? version : 4;
        struct output_entry *e = &s->outputs[s->output_count++];
        e->name[0] = '\0';
        e->output = wl_registry_bind(reg, name, &wl_output_interface, v);
        wl_output_add_listener(e->output, &output_listener, e);
    }
}
static void list_on_global_remove(void *d, struct wl_registry *r, uint32_t n) {}

static const struct wl_registry_listener list_registry_listener = {
    .global        = list_on_global,
    .global_remove = list_on_global_remove,
};

static const char *resolve_output_name(struct list_state *s, struct wl_output *o) {
    for (int i = 0; i < s->output_count; i++)
        if (s->outputs[i].output == o && s->outputs[i].name[0])
            return s->outputs[i].name;
    return NULL;
}

static struct list_state *list_connect(void) {
    static struct list_state s;
    memset(&s, 0, sizeof(s));
    s.display = wl_display_connect(NULL);
    if (!s.display) return NULL;

    struct wl_registry *reg = wl_display_get_registry(s.display);
    wl_registry_add_listener(reg, &list_registry_listener, &s);
    wl_display_roundtrip(s.display);  /* bind globals */
    wl_display_roundtrip(s.display);  /* wl_output name events */

    if (!s.manager) {
        wl_display_disconnect(s.display);
        return NULL;
    }

    zwlr_foreign_toplevel_manager_v1_add_listener(s.manager, &win_mgr_listener, NULL);
    wl_display_roundtrip(s.display);  /* get all toplevel handles + initial events */
    wl_display_roundtrip(s.display);  /* flush remaining title/app_id/state/output_enter */
    return &s;
}

static void print_json_str(const char *s) {
    if (!s) s = "";
    putchar('"');
    for (; *s; s++) {
        if (*s == '"' || *s == '\\') putchar('\\');
        else if (*s == '\n') { putchar('\\'); putchar('n'); continue; }
        putchar(*s);
    }
    putchar('"');
}

static int list_windows(void) {
    win_count = 0;
    struct list_state *s = list_connect();
    if (!s) {
        fprintf(stderr, "list-windows: cannot connect or missing globals\n");
        return 1;
    }

    printf("[");
    for (int i = 0; i < win_count; i++) {
        if (i > 0) printf(",");
        printf("{\"title\":");
        print_json_str(wins[i].title);
        printf(",\"app_id\":");
        print_json_str(wins[i].app_id);
        printf("}");
        free(wins[i].title);
        free(wins[i].app_id);
    }
    printf("]\n");

    wl_display_disconnect(s->display);
    return 0;
}

static int get_active_output(void) {
    win_count = 0;
    struct list_state *s = list_connect();
    if (!s) {
        fprintf(stderr, "active-output: cannot connect or missing globals\n");
        return 1;
    }

    for (int i = 0; i < win_count; i++) {
        if (wins[i].activated && wins[i].first_output) {
            const char *name = resolve_output_name(s, wins[i].first_output);
            if (name) {
                printf("%s\n", name);
                wl_display_disconnect(s->display);
                return 0;
            }
        }
        free(wins[i].title);
        free(wins[i].app_id);
    }

    wl_display_disconnect(s->display);
    return 1;  /* no active window found */
}

/* ---- main ---- */

int main(int argc, char *argv[]) {
    if (argc > 1 && strcmp(argv[1], "--list-windows") == 0)
        return list_windows();

    if (argc > 1 && strcmp(argv[1], "--active-output") == 0)
        return get_active_output();

    int output_idx = 0;
    const char *output_name_arg = NULL;
    int region_x = -1, region_y = -1, region_w = -1, region_h = -1;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--output-index") && i + 1 < argc) {
            output_idx = atoi(argv[i + 1]);
            i++;
        } else if (!strcmp(argv[i], "--output-name") && i + 1 < argc) {
            output_name_arg = argv[i + 1];
            i++;
        } else if (!strcmp(argv[i], "--region") && i + 1 < argc) {
            sscanf(argv[i + 1], "%d,%d,%d,%d",
                   &region_x, &region_y, &region_w, &region_h);
            i++;
        }
    }

    struct state s = {0};
    s.display = wl_display_connect(NULL);
    if (!s.display) {
        fprintf(stderr, "screencopy: cannot connect to Wayland display\n");
        return 1;
    }

    struct wl_registry *reg = wl_display_get_registry(s.display);
    wl_registry_add_listener(reg, &registry_listener, &s);
    wl_display_roundtrip(s.display);
    wl_display_roundtrip(s.display);  /* flush wl_output name events */

    if (s.xdg_output_manager) {
        /* wlroots: use xdg-output's position, not wl_output.geometry */
        for (int i = 0; i < s.output_count; i++) {
            struct output_entry *e = &s.outputs[i];
            e->xdg_output = zxdg_output_manager_v1_get_xdg_output(s.xdg_output_manager, e->output);
            zxdg_output_v1_add_listener(e->xdg_output, &xdg_output_listener, e);
        }
        wl_display_roundtrip(s.display);
    }

    if (!s.shm || s.output_count == 0 || !s.manager) {
        fprintf(stderr, "screencopy: missing globals (shm=%s outputs=%d manager=%s)\n",
                s.shm ? "ok" : "MISSING", s.output_count,
                s.manager ? "ok" : "MISSING");
        return 1;
    }

    struct wl_output *capture_output = NULL;
    int32_t rel_x = 0, rel_y = 0, rel_w = 0, rel_h = 0;
    bool use_region = false;

    if (region_x >= 0) {
        /* find the output that contains the window's center */
        int cx = region_x + region_w / 2;
        int cy = region_y + region_h / 2;
        for (int i = 0; i < s.output_count; i++) {
            struct output_entry *e = &s.outputs[i];
            if (cx >= e->ox && cx < e->ox + e->ow &&
                    cy >= e->oy && cy < e->oy + e->oh) {
                capture_output = e->output;
                rel_x = region_x - e->ox;
                rel_y = region_y - e->oy;
                rel_w = region_w;
                rel_h = region_h;
                fprintf(stderr, "screencopy: region on output %s rel=(%d,%d %dx%d)\n",
                        e->name, rel_x, rel_y, rel_w, rel_h);
                break;
            }
        }
        if (!capture_output) {
            /* fall back to the first output */
            capture_output = s.outputs[0].output;
            rel_x = region_x; rel_y = region_y;
            rel_w = region_w; rel_h = region_h;
            fprintf(stderr, "screencopy: region not on any output, using output 0\n");
        }
        use_region = true;
    } else if (output_name_arg) {
        for (int i = 0; i < s.output_count; i++) {
            if (strcmp(s.outputs[i].name, output_name_arg) == 0) {
                capture_output = s.outputs[i].output;
                break;
            }
        }
        if (!capture_output) {
            fprintf(stderr, "screencopy: output '%s' not found (available:", output_name_arg);
            for (int i = 0; i < s.output_count; i++)
                fprintf(stderr, " %s", s.outputs[i].name[0] ? s.outputs[i].name : "?");
            fprintf(stderr, ")\n");
            wl_display_disconnect(s.display);
            return 1;
        }
    } else {
        if (output_idx >= s.output_count) {
            fprintf(stderr, "screencopy: output index %d not available (have %d)\n",
                    output_idx, s.output_count);
            wl_display_disconnect(s.display);
            return 1;
        }
        capture_output = s.outputs[output_idx].output;
    }

    if (!freopen(NULL, "wb", stdout)) return 1;

    while (1) {
        struct frame f = { .st = &s };
        if (use_region) {
            f.crop_w = (uint32_t)rel_w;
            f.crop_h = (uint32_t)rel_h;
        }

        if (use_region) {
            f.wlr_frame = zwlr_screencopy_manager_v1_capture_output_region(
                s.manager, 0, capture_output, rel_x, rel_y, rel_w, rel_h);
        } else {
            f.wlr_frame = zwlr_screencopy_manager_v1_capture_output(
                s.manager, 0, capture_output);
        }
        zwlr_screencopy_frame_v1_add_listener(f.wlr_frame, &frame_listener, &f);

        while (!f.done && !f.failed) {
            if (wl_display_dispatch(s.display) < 0) {
                fprintf(stderr, "screencopy: display error\n");
                goto quit;
            }
        }

        if (!f.failed) {
            if (f.y_invert) {
                uint8_t *tmp = malloc(f.stride);
                if (tmp) {
                    for (uint32_t i = 0; i < f.height / 2; i++) {
                        uint8_t *a = (uint8_t *)f.data + i * f.stride;
                        uint8_t *b = (uint8_t *)f.data + (f.height - 1 - i) * f.stride;
                        memcpy(tmp, a, f.stride);
                        memcpy(a, b, f.stride);
                        memcpy(b, tmp, f.stride);
                    }
                    free(tmp);
                }
            }

            uint32_t out_w = f.crop_w ? f.crop_w : f.width;
            uint32_t out_h = f.crop_h ? f.crop_h : f.height;
            uint32_t hdr[4] = { MAGIC, out_w, out_h, f.format };
            if (!write_all(hdr, sizeof(hdr))) goto quit;

            for (uint32_t row = 0; row < out_h; row++) {
                if (!write_all((uint8_t *)f.data + row * f.stride, out_w * 4))
                    goto quit;
            }
        } else {
            fprintf(stderr, "screencopy: frame failed, retrying\n");
            nanosleep(&(struct timespec){ 0, 100000000L }, NULL);
        }

        zwlr_screencopy_frame_v1_destroy(f.wlr_frame);
        if (f.buffer) wl_buffer_destroy(f.buffer);
        if (f.data && f.data != MAP_FAILED) munmap(f.data, f.buf_size);

        nanosleep(&(struct timespec){ 0, 16666666L }, NULL);
    }

quit:
    wl_display_disconnect(s.display);
    return 1;
}

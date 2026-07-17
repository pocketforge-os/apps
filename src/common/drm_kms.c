#include "drm_kms.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <dma-buf.h>   /* vendored linux-uapi */

/* Plane "type" property enum values — stable KMS ABI (exposed to userspace
 * as the enum entries of the immutable "type" plane property). */
#define PF_PLANE_TYPE_PRIMARY 1

int pf_ioctl(int fd, unsigned long req, void *arg)
{
    int r;
    do {
        r = ioctl(fd, req, arg);
    } while (r == -1 && (errno == EINTR || errno == EAGAIN));
    return r;
}

void pf_drm_driver_name(int fd, char *buf, unsigned len)
{
    struct drm_version v;
    memset(&v, 0, sizeof v);
    buf[0] = '\0';
    if (pf_ioctl(fd, DRM_IOCTL_VERSION, &v) < 0)
        return;
    if (v.name_len == 0)
        return;
    char *name = calloc(1, v.name_len + 1);
    if (!name)
        return;
    v.name = name;
    /* second call fills exactly name_len bytes; date/desc left NULL */
    if (pf_ioctl(fd, DRM_IOCTL_VERSION, &v) == 0) {
        name[v.name_len] = '\0';
        snprintf(buf, len, "%s", name);
    }
    free(name);
}

static int has_cap(int fd, uint64_t cap)
{
    struct drm_get_cap gc = { .capability = cap, .value = 0 };
    if (pf_ioctl(fd, DRM_IOCTL_GET_CAP, &gc) < 0)
        return 0;
    return gc.value != 0;
}

int pf_drm_open(const char *path, const char *want_driver)
{
    if (path) {
        int fd = open(path, O_RDWR | O_CLOEXEC);
        if (fd < 0)
            fprintf(stderr, "drm: open(%s): %s\n", path, strerror(errno));
        return fd;
    }
    for (int i = 0; i < 16; i++) {
        char p[32];
        snprintf(p, sizeof p, "/dev/dri/card%d", i);
        int fd = open(p, O_RDWR | O_CLOEXEC);
        if (fd < 0)
            continue;
        char name[64];
        pf_drm_driver_name(fd, name, sizeof name);
        int dumb = has_cap(fd, DRM_CAP_DUMB_BUFFER);
        if (dumb && (!want_driver || strcmp(name, want_driver) == 0)) {
            fprintf(stderr, "drm: using %s (driver=%s)\n", p, name);
            return fd;
        }
        close(fd);
    }
    fprintf(stderr, "drm: no suitable card found (want_driver=%s)\n",
            want_driver ? want_driver : "any-with-dumb");
    return -1;
}

int pf_drm_set_caps(int fd)
{
    struct drm_set_client_cap cc;
    cc.capability = DRM_CLIENT_CAP_UNIVERSAL_PLANES;
    cc.value = 1;
    if (pf_ioctl(fd, DRM_IOCTL_SET_CLIENT_CAP, &cc) < 0) {
        fprintf(stderr, "drm: UNIVERSAL_PLANES: %s\n", strerror(errno));
        return -1;
    }
    cc.capability = DRM_CLIENT_CAP_ATOMIC;
    cc.value = 1;
    if (pf_ioctl(fd, DRM_IOCTL_SET_CLIENT_CAP, &cc) < 0) {
        fprintf(stderr, "drm: CLIENT_CAP_ATOMIC: %s\n", strerror(errno));
        return -1;
    }
    return 0;
}

int pf_drm_create_dumb(int fd, uint32_t w, uint32_t h, struct pf_dumb_fb *fb)
{
    struct drm_mode_create_dumb c;
    memset(&c, 0, sizeof c);
    c.width = w;
    c.height = h;
    c.bpp = 32;
    if (pf_ioctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &c) < 0) {
        fprintf(stderr, "drm: CREATE_DUMB %ux%u: %s\n", w, h, strerror(errno));
        return -1;
    }
    memset(fb, 0, sizeof *fb);
    fb->handle = c.handle;
    fb->width = w;
    fb->height = h;
    fb->stride = c.pitch;
    fb->size = c.size;
    fb->prime_fd = -1;
    return 0;
}

int pf_drm_map_dumb(int fd, struct pf_dumb_fb *fb)
{
    struct drm_mode_map_dumb m;
    memset(&m, 0, sizeof m);
    m.handle = fb->handle;
    if (pf_ioctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &m) < 0) {
        fprintf(stderr, "drm: MAP_DUMB: %s\n", strerror(errno));
        return -1;
    }
    fb->map = mmap(NULL, fb->size, PROT_READ | PROT_WRITE, MAP_SHARED, fd,
                   m.offset);
    if (fb->map == MAP_FAILED) {
        fb->map = NULL;
        fprintf(stderr, "drm: mmap dumb: %s\n", strerror(errno));
        return -1;
    }
    return 0;
}

int pf_drm_export_dumb(int fd, struct pf_dumb_fb *fb)
{
    struct drm_prime_handle p;
    memset(&p, 0, sizeof p);
    p.handle = fb->handle;
    p.flags = DRM_CLOEXEC | DRM_RDWR;
    if (pf_ioctl(fd, DRM_IOCTL_PRIME_HANDLE_TO_FD, &p) < 0) {
        fprintf(stderr, "drm: PRIME_HANDLE_TO_FD: %s\n", strerror(errno));
        return -1;
    }
    fb->prime_fd = p.fd;
    return 0;
}

int pf_drm_addfb2(int fd, struct pf_dumb_fb *fb, uint32_t fourcc)
{
    struct drm_mode_fb_cmd2 f;
    memset(&f, 0, sizeof f);
    f.width = fb->width;
    f.height = fb->height;
    f.pixel_format = fourcc;
    f.handles[0] = fb->handle;
    f.pitches[0] = fb->stride;
    if (pf_ioctl(fd, DRM_IOCTL_MODE_ADDFB2, &f) < 0) {
        fprintf(stderr, "drm: ADDFB2 %ux%u fourcc=0x%08x: %s\n",
                fb->width, fb->height, fourcc, strerror(errno));
        return -1;
    }
    fb->fb_id = f.fb_id;
    return 0;
}

void pf_drm_destroy_dumb(int fd, struct pf_dumb_fb *fb)
{
    if (fb->map) {
        munmap(fb->map, fb->size);
        fb->map = NULL;
    }
    if (fb->prime_fd >= 0) {
        close(fb->prime_fd);
        fb->prime_fd = -1;
    }
    if (fb->fb_id) {
        pf_ioctl(fd, DRM_IOCTL_MODE_RMFB, &fb->fb_id);
        fb->fb_id = 0;
    }
    if (fb->handle) {
        struct drm_mode_destroy_dumb d = { .handle = fb->handle };
        pf_ioctl(fd, DRM_IOCTL_MODE_DESTROY_DUMB, &d);
        fb->handle = 0;
    }
}

int pf_dmabuf_sync(int prime_fd, int start, int write)
{
    struct dma_buf_sync s;
    memset(&s, 0, sizeof s);
    s.flags = (start ? DMA_BUF_SYNC_START : DMA_BUF_SYNC_END) |
              (write ? DMA_BUF_SYNC_RW : DMA_BUF_SYNC_READ);
    if (pf_ioctl(prime_fd, DMA_BUF_IOCTL_SYNC, &s) < 0) {
        fprintf(stderr, "dma-buf: SYNC(%s): %s\n", start ? "start" : "end",
                strerror(errno));
        return -1;
    }
    return 0;
}

/* -------- property lookup -------- */

/* Find the id (and current value) of a named property on a KMS object. */
static int obj_prop(int fd, uint32_t obj_id, uint32_t obj_type,
                    const char *name, uint32_t *prop_id, uint64_t *value)
{
    struct drm_mode_obj_get_properties gp;
    uint32_t ids_buf[128];
    uint64_t vals_buf[128];

    memset(&gp, 0, sizeof gp);
    gp.obj_id = obj_id;
    gp.obj_type = obj_type;
    if (pf_ioctl(fd, DRM_IOCTL_MODE_OBJ_GETPROPERTIES, &gp) < 0)
        return -1;
    if (gp.count_props > 128) {
        fprintf(stderr, "drm: obj %u has %u props (>128)\n", obj_id,
                gp.count_props);
        return -1;
    }
    gp.props_ptr = (uintptr_t)ids_buf;
    gp.prop_values_ptr = (uintptr_t)vals_buf;
    if (pf_ioctl(fd, DRM_IOCTL_MODE_OBJ_GETPROPERTIES, &gp) < 0)
        return -1;

    for (uint32_t i = 0; i < gp.count_props; i++) {
        struct drm_mode_get_property p;
        memset(&p, 0, sizeof p);
        p.prop_id = ids_buf[i];
        if (pf_ioctl(fd, DRM_IOCTL_MODE_GETPROPERTY, &p) < 0)
            continue;
        if (strcmp(p.name, name) == 0) {
            *prop_id = ids_buf[i];
            if (value)
                *value = vals_buf[i];
            return 0;
        }
    }
    return -1;
}

static int need_prop(int fd, uint32_t obj_id, uint32_t obj_type,
                     const char *name, uint32_t *prop_id)
{
    if (obj_prop(fd, obj_id, obj_type, name, prop_id, NULL) < 0) {
        fprintf(stderr, "drm: object %u: property \"%s\" not found\n",
                obj_id, name);
        return -1;
    }
    return 0;
}

/* -------- pipe discovery -------- */

int pf_kms_discover(int fd, struct pf_kms *k)
{
    memset(k, 0, sizeof *k);

    /* resources (two-call; retry if counts grow between calls) */
    uint32_t conn_ids[32], crtc_ids[32], enc_ids[32];
    struct drm_mode_card_res res;
    for (;;) {
        memset(&res, 0, sizeof res);
        if (pf_ioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &res) < 0) {
            fprintf(stderr, "drm: GETRESOURCES: %s\n", strerror(errno));
            return -1;
        }
        if (res.count_connectors > 32 || res.count_crtcs > 32 ||
            res.count_encoders > 32) {
            fprintf(stderr, "drm: resource counts exceed 32\n");
            return -1;
        }
        uint32_t want_conn = res.count_connectors,
                 want_crtc = res.count_crtcs, want_enc = res.count_encoders;
        res.connector_id_ptr = (uintptr_t)conn_ids;
        res.crtc_id_ptr = (uintptr_t)crtc_ids;
        res.encoder_id_ptr = (uintptr_t)enc_ids;
        res.count_fbs = 0;
        res.fb_id_ptr = 0;
        if (pf_ioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &res) < 0) {
            fprintf(stderr, "drm: GETRESOURCES(2): %s\n", strerror(errno));
            return -1;
        }
        if (res.count_connectors <= want_conn &&
            res.count_crtcs <= want_crtc && res.count_encoders <= want_enc)
            break; /* stable */
    }
    if (res.count_connectors == 0 || res.count_crtcs == 0) {
        fprintf(stderr, "drm: no connectors/crtcs\n");
        return -1;
    }

    /* first connected connector with at least one mode */
    struct drm_mode_modeinfo modes[64];
    uint32_t conn_encs[16];
    uint32_t n_conn_encs = 0;
    for (uint32_t i = 0; i < res.count_connectors && !k->connector_id; i++) {
        struct drm_mode_get_connector gc;
        for (;;) {
            memset(&gc, 0, sizeof gc);
            gc.connector_id = conn_ids[i];
            if (pf_ioctl(fd, DRM_IOCTL_MODE_GETCONNECTOR, &gc) < 0)
                break;
            if (gc.count_modes > 64 || gc.count_encoders > 16)
                break;
            uint32_t want_modes = gc.count_modes,
                     want_encs = gc.count_encoders;
            gc.modes_ptr = (uintptr_t)modes;
            gc.encoders_ptr = (uintptr_t)conn_encs;
            gc.count_props = 0;
            gc.props_ptr = gc.prop_values_ptr = 0;
            if (pf_ioctl(fd, DRM_IOCTL_MODE_GETCONNECTOR, &gc) < 0)
                break;
            if (gc.count_modes <= want_modes && gc.count_encoders <= want_encs) {
                /* connection: 1 = connected */
                if (gc.connection == 1 && gc.count_modes > 0) {
                    k->connector_id = conn_ids[i];
                    k->mode = modes[0];
                    n_conn_encs = gc.count_encoders;
                }
                break;
            }
        }
    }
    if (!k->connector_id) {
        fprintf(stderr, "drm: no connected connector with modes\n");
        return -1;
    }

    /* CRTC: first one an encoder of this connector can drive */
    for (uint32_t e = 0; e < n_conn_encs && !k->crtc_id; e++) {
        struct drm_mode_get_encoder ge;
        memset(&ge, 0, sizeof ge);
        ge.encoder_id = conn_encs[e];
        if (pf_ioctl(fd, DRM_IOCTL_MODE_GETENCODER, &ge) < 0)
            continue;
        for (uint32_t c = 0; c < res.count_crtcs; c++) {
            if (ge.possible_crtcs & (1u << c)) {
                k->crtc_id = crtc_ids[c];
                break;
            }
        }
    }
    if (!k->crtc_id) {
        fprintf(stderr, "drm: no CRTC reachable from connector %u\n",
                k->connector_id);
        return -1;
    }
    uint32_t crtc_index = 0;
    for (uint32_t c = 0; c < res.count_crtcs; c++)
        if (crtc_ids[c] == k->crtc_id)
            crtc_index = c;

    /* primary plane for that CRTC */
    uint32_t plane_ids[64];
    struct drm_mode_get_plane_res pr;
    for (;;) {
        memset(&pr, 0, sizeof pr);
        if (pf_ioctl(fd, DRM_IOCTL_MODE_GETPLANERESOURCES, &pr) < 0) {
            fprintf(stderr, "drm: GETPLANERESOURCES: %s\n", strerror(errno));
            return -1;
        }
        if (pr.count_planes > 64) {
            fprintf(stderr, "drm: >64 planes\n");
            return -1;
        }
        uint32_t want = pr.count_planes;
        pr.plane_id_ptr = (uintptr_t)plane_ids;
        if (pf_ioctl(fd, DRM_IOCTL_MODE_GETPLANERESOURCES, &pr) < 0) {
            fprintf(stderr, "drm: GETPLANERESOURCES(2): %s\n", strerror(errno));
            return -1;
        }
        if (pr.count_planes <= want)
            break;
    }
    for (uint32_t i = 0; i < pr.count_planes && !k->plane_id; i++) {
        struct drm_mode_get_plane gpl;
        memset(&gpl, 0, sizeof gpl);
        gpl.plane_id = plane_ids[i];
        if (pf_ioctl(fd, DRM_IOCTL_MODE_GETPLANE, &gpl) < 0)
            continue;
        if (!(gpl.possible_crtcs & (1u << crtc_index)))
            continue;
        uint32_t type_prop;
        uint64_t type_val = ~0ull;
        if (obj_prop(fd, plane_ids[i], DRM_MODE_OBJECT_PLANE, "type",
                     &type_prop, &type_val) < 0)
            continue;
        if (type_val == PF_PLANE_TYPE_PRIMARY)
            k->plane_id = plane_ids[i];
    }
    if (!k->plane_id) {
        fprintf(stderr, "drm: no primary plane for crtc %u\n", k->crtc_id);
        return -1;
    }

    /* atomic property ids */
    if (need_prop(fd, k->connector_id, DRM_MODE_OBJECT_CONNECTOR, "CRTC_ID",
                  &k->conn_crtc_id) < 0 ||
        need_prop(fd, k->crtc_id, DRM_MODE_OBJECT_CRTC, "MODE_ID",
                  &k->crtc_mode_id) < 0 ||
        need_prop(fd, k->crtc_id, DRM_MODE_OBJECT_CRTC, "ACTIVE",
                  &k->crtc_active) < 0 ||
        need_prop(fd, k->plane_id, DRM_MODE_OBJECT_PLANE, "FB_ID",
                  &k->plane_fb_id) < 0 ||
        need_prop(fd, k->plane_id, DRM_MODE_OBJECT_PLANE, "CRTC_ID",
                  &k->plane_crtc_id) < 0 ||
        need_prop(fd, k->plane_id, DRM_MODE_OBJECT_PLANE, "SRC_X",
                  &k->plane_src_x) < 0 ||
        need_prop(fd, k->plane_id, DRM_MODE_OBJECT_PLANE, "SRC_Y",
                  &k->plane_src_y) < 0 ||
        need_prop(fd, k->plane_id, DRM_MODE_OBJECT_PLANE, "SRC_W",
                  &k->plane_src_w) < 0 ||
        need_prop(fd, k->plane_id, DRM_MODE_OBJECT_PLANE, "SRC_H",
                  &k->plane_src_h) < 0 ||
        need_prop(fd, k->plane_id, DRM_MODE_OBJECT_PLANE, "CRTC_X",
                  &k->plane_crtc_x) < 0 ||
        need_prop(fd, k->plane_id, DRM_MODE_OBJECT_PLANE, "CRTC_Y",
                  &k->plane_crtc_y) < 0 ||
        need_prop(fd, k->plane_id, DRM_MODE_OBJECT_PLANE, "CRTC_W",
                  &k->plane_crtc_w) < 0 ||
        need_prop(fd, k->plane_id, DRM_MODE_OBJECT_PLANE, "CRTC_H",
                  &k->plane_crtc_h) < 0)
        return -1;

    /* mode blob */
    struct drm_mode_create_blob cb;
    memset(&cb, 0, sizeof cb);
    cb.data = (uintptr_t)&k->mode;
    cb.length = sizeof k->mode;
    if (pf_ioctl(fd, DRM_IOCTL_MODE_CREATEPROPBLOB, &cb) < 0) {
        fprintf(stderr, "drm: CREATEPROPBLOB(mode): %s\n", strerror(errno));
        return -1;
    }
    k->mode_blob_id = cb.blob_id;

    fprintf(stderr,
            "drm: pipe connector=%u crtc=%u plane=%u mode=%ux%u@%u \"%s\"\n",
            k->connector_id, k->crtc_id, k->plane_id, k->mode.hdisplay,
            k->mode.vdisplay, k->mode.vrefresh, k->mode.name);
    return 0;
}

/* -------- atomic commits -------- */

struct areq {
    uint32_t objs[4];
    uint32_t counts[4];
    uint32_t nobjs;
    uint32_t props[24];
    uint64_t vals[24];
    uint32_t nprops;
};

/* Properties for one object must be contiguous; call for each object in
 * turn. */
static void areq_obj(struct areq *r, uint32_t obj_id)
{
    r->objs[r->nobjs] = obj_id;
    r->counts[r->nobjs] = 0;
    r->nobjs++;
}

static void areq_prop(struct areq *r, uint32_t prop_id, uint64_t val)
{
    r->props[r->nprops] = prop_id;
    r->vals[r->nprops] = val;
    r->nprops++;
    r->counts[r->nobjs - 1]++;
}

static int areq_commit(int fd, struct areq *r, uint32_t flags)
{
    struct drm_mode_atomic a;
    memset(&a, 0, sizeof a);
    a.flags = flags;
    a.count_objs = r->nobjs;
    a.objs_ptr = (uintptr_t)r->objs;
    a.count_props_ptr = (uintptr_t)r->counts;
    a.props_ptr = (uintptr_t)r->props;
    a.prop_values_ptr = (uintptr_t)r->vals;
    if (pf_ioctl(fd, DRM_IOCTL_MODE_ATOMIC, &a) < 0) {
        fprintf(stderr, "drm: ATOMIC commit (flags=0x%x): %s\n", flags,
                strerror(errno));
        return -1;
    }
    return 0;
}

static void plane_props(struct areq *r, struct pf_kms *k, uint32_t fb_id)
{
    areq_obj(r, k->plane_id);
    areq_prop(r, k->plane_fb_id, fb_id);
    areq_prop(r, k->plane_crtc_id, k->crtc_id);
    areq_prop(r, k->plane_src_x, 0);
    areq_prop(r, k->plane_src_y, 0);
    areq_prop(r, k->plane_src_w, (uint64_t)k->mode.hdisplay << 16);
    areq_prop(r, k->plane_src_h, (uint64_t)k->mode.vdisplay << 16);
    areq_prop(r, k->plane_crtc_x, 0);
    areq_prop(r, k->plane_crtc_y, 0);
    areq_prop(r, k->plane_crtc_w, k->mode.hdisplay);
    areq_prop(r, k->plane_crtc_h, k->mode.vdisplay);
}

int pf_kms_modeset(int fd, struct pf_kms *k, uint32_t fb_id)
{
    struct areq r;
    memset(&r, 0, sizeof r);
    areq_obj(&r, k->connector_id);
    areq_prop(&r, k->conn_crtc_id, k->crtc_id);
    areq_obj(&r, k->crtc_id);
    areq_prop(&r, k->crtc_mode_id, k->mode_blob_id);
    areq_prop(&r, k->crtc_active, 1);
    plane_props(&r, k, fb_id);
    return areq_commit(fd, &r, DRM_MODE_ATOMIC_ALLOW_MODESET);
}

int pf_kms_flip(int fd, struct pf_kms *k, uint32_t fb_id)
{
    struct areq r;
    memset(&r, 0, sizeof r);
    areq_obj(&r, k->plane_id);
    areq_prop(&r, k->plane_fb_id, fb_id);
    return areq_commit(fd, &r, DRM_MODE_PAGE_FLIP_EVENT);
}

int pf_kms_wait_flip(int fd)
{
    char buf[1024];
    for (;;) {
        ssize_t n = read(fd, buf, sizeof buf);
        if (n < 0) {
            if (errno == EINTR || errno == EAGAIN)
                continue;
            fprintf(stderr, "drm: event read: %s\n", strerror(errno));
            return -1;
        }
        ssize_t off = 0;
        while (off < n) {
            struct drm_event *e = (struct drm_event *)(buf + off);
            if (e->type == DRM_EVENT_FLIP_COMPLETE)
                return 0;
            off += e->length;
        }
    }
}

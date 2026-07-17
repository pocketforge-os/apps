/* pf-drm-present — owned DRM presenter for the closed-UM PowerVR stack on
 * mainline (tsp-mc9m.15.2).
 *
 * Proves the REVERSED-import scanout pattern mandated by the tsp-mc9m.15
 * design synthesis (GPU allocs are non-contiguous, sun4i-drm rejects them,
 * no SoC IOMMU, blob has dma-buf IMPORT but no export):
 *
 *   sun4i-drm dumb buffer (CMA-contiguous) -> PRIME export
 *     -> eglCreateImageKHR(EGL_LINUX_DMA_BUF_EXT)
 *     -> glEGLImageTargetTexture2DOES -> FBO -> GPU renders INTO it
 *     -> EGL_KHR_fence_sync -> drmModeAddFB2 + atomic flip
 *
 * Modes (one required):
 *   --smoke         DRM half only, no EGL: dumb alloc + AddFB2 + atomic
 *                   modeset + CPU-pattern flip loop. Runs device-free
 *                   against vkms (the CI/dev smoke).
 *   --import-proof  Headless, no CRTC needed (works on the mainline image
 *                   TODAY: card0/sun4i-drm registers without a display):
 *                   dumb alloc -> PRIME -> EGLImage -> FBO -> GPU render ->
 *                   mmap dumb buffer -> verify pixels + fnv1a64.
 *   --present       Full path: modeset + GPU-rendered animated pattern +
 *                   fence-synced atomic flip loop (gated on DISPLAY first
 *                   light on the device).
 *
 * Options:
 *   --card /dev/dri/cardN   explicit node (default: scan)
 *   --driver NAME           scan for driver NAME (smoke default: vkms)
 *   --frames N              flip-loop length (default 60)
 *   --size WxH              import-proof buffer size (default 256x256)
 *   --rotate {0,90,180,270} content rotation for the 270-degree panel
 *   --readpixels            fallback: GPU renders to pbuffer, CPU copies
 *                           into the dumb buffer (proof-of-life path when
 *                           EGLImage import fails at runtime)
 *
 * Format is XRGB8888 (DC pixfmt 90 / B8G8R8X8 — the 85e6dd6 lesson).
 * EGL_DEFAULT_DISPLAY only; dc_null (or a real DC) must be loaded for
 * eglInitialize. Zero blob change.
 *
 * Output contract: SMOKE:|IMPORT-PROOF:|PRESENT: PASS|FAIL ... on the last
 * line; intermediate DRM/EGL details on stderr.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>

#include "../common/drm_kms.h"
#include "../common/egl_dl.h"
#include "../common/fnv1a64.h"

static const char *mode_tag = "PRESENT";

static void die(const char *stage)
{
    fprintf(stderr, "%s: FAIL stage=%s eglError=0x%x glError=0x%x\n",
            mode_tag, stage, E.eglGetError ? E.eglGetError() : 0,
            E.glGetError ? E.glGetError() : 0);
    exit(1);
}

/* ---------------- CPU test pattern (smoke + reference) ---------------- */

/* XRGB8888 gradient with a frame-parity bar so successive flips differ. */
static void fill_pattern(struct pf_dumb_fb *fb, int frame)
{
    uint8_t *base = fb->map;
    for (uint32_t y = 0; y < fb->height; y++) {
        uint32_t *row = (uint32_t *)(base + (size_t)y * fb->stride);
        for (uint32_t x = 0; x < fb->width; x++) {
            uint8_t r = (uint8_t)(255u * x / (fb->width ? fb->width : 1));
            uint8_t g = (uint8_t)(255u * y / (fb->height ? fb->height : 1));
            uint8_t b = (frame & 1) ? 0xff : 0x00;
            if (y < 16)
                r = g = b = (frame & 1) ? 0xff : 0x00; /* parity bar */
            row[x] = 0xff000000u | ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
        }
    }
}

/* ---------------- GL setup shared by import-proof / present ------------ */

static EGLDisplay dpy;
static EGLContext ctx;
static EGLSurface pbuf;
static GLuint prog;
static GLint u_rot;

/* Per-dumb-buffer GL render target. */
struct gl_target {
    EGLImageKHR image;
    GLuint tex, fbo;
};

static void egl_setup(void)
{
    if (pf_egl_load() != 0)
        die("dlopen");
    dpy = E.eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (dpy == EGL_NO_DISPLAY)
        die("eglGetDisplay");
    EGLint maj, min;
    if (!E.eglInitialize(dpy, &maj, &min))
        die("eglInitialize");
    fprintf(stderr, "egl: %d.%d vendor=\"%s\"\n", maj, min,
            E.eglQueryString(dpy, EGL_VENDOR));

    static const EGLint cfg_attrs[] = {
        EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8,
        EGL_ALPHA_SIZE, 8,
        EGL_NONE
    };
    EGLConfig cfg;
    EGLint ncfg = 0;
    if (!E.eglChooseConfig(dpy, cfg_attrs, &cfg, 1, &ncfg) || ncfg < 1)
        die("eglChooseConfig");
    /* small pbuffer as the bound surface; all real rendering goes to FBOs
     * (proven path on this blob; surfaceless is advertised but unproven) */
    static const EGLint pb_attrs[] = { EGL_WIDTH, 16, EGL_HEIGHT, 16,
                                       EGL_NONE };
    pbuf = E.eglCreatePbufferSurface(dpy, cfg, pb_attrs);
    if (pbuf == EGL_NO_SURFACE)
        die("eglCreatePbufferSurface");
    static const EGLint ctx_attrs[] = { EGL_CONTEXT_CLIENT_VERSION, 2,
                                        EGL_NONE };
    ctx = E.eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, ctx_attrs);
    if (ctx == EGL_NO_CONTEXT)
        die("eglCreateContext");
    if (!E.eglMakeCurrent(dpy, pbuf, pbuf, ctx))
        die("eglMakeCurrent");
    fprintf(stderr, "gl: renderer=\"%s\" version=\"%s\"\n",
            E.glGetString(GL_RENDERER), E.glGetString(GL_VERSION));
}

static GLuint mkshader(GLenum type, const char *src)
{
    GLuint s = E.glCreateShader(type);
    E.glShaderSource(s, 1, &src, NULL);
    E.glCompileShader(s);
    GLint ok = 0;
    E.glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512];
        E.glGetShaderInfoLog(s, sizeof log, NULL, log);
        fprintf(stderr, "shader log: %s\n", log);
        die("shader");
    }
    return s;
}

static void gl_program_setup(void)
{
    /* rot carries a full 2x2 matrix as vec4 (row-major: a b c d) so the
     * CPU can bake content rotation + the GL->scanout Y flip into one
     * transform. */
    static const char *vsrc =
        "attribute vec2 pos;\n"
        "uniform vec4 rot;\n"
        "void main(){\n"
        "  vec2 p = vec2(rot.x*pos.x + rot.y*pos.y,\n"
        "                rot.z*pos.x + rot.w*pos.y);\n"
        "  gl_Position = vec4(p, 0.0, 1.0);\n"
        "}\n";
    static const char *fsrc =
        "precision mediump float;\n"
        "void main(){ gl_FragColor = vec4(1.0, 0.0, 0.0, 1.0); }\n";
    prog = E.glCreateProgram();
    E.glAttachShader(prog, mkshader(GL_VERTEX_SHADER, vsrc));
    E.glAttachShader(prog, mkshader(GL_FRAGMENT_SHADER, fsrc));
    E.glBindAttribLocation(prog, 0, "pos");
    E.glLinkProgram(prog);
    GLint ok = 0;
    E.glGetProgramiv(prog, GL_LINK_STATUS, &ok);
    if (!ok)
        die("link");
    E.glUseProgram(prog);
    u_rot = E.glGetUniformLocation(prog, "rot");
}

static int gl_target_setup(struct pf_dumb_fb *fb, struct gl_target *t)
{
    if (!E.eglCreateImageKHR || !E.glEGLImageTargetTexture2DOES) {
        fprintf(stderr,
                "egl: dma-buf import entry points absent "
                "(eglCreateImageKHR=%p glEGLImageTargetTexture2DOES=%p)\n",
                (void *)E.eglCreateImageKHR,
                (void *)E.glEGLImageTargetTexture2DOES);
        return -1;
    }
    const EGLint attrs[] = {
        EGL_WIDTH, (EGLint)fb->width,
        EGL_HEIGHT, (EGLint)fb->height,
        EGL_LINUX_DRM_FOURCC_EXT, (EGLint)DRM_FORMAT_XRGB8888,
        EGL_DMA_BUF_PLANE0_FD_EXT, fb->prime_fd,
        EGL_DMA_BUF_PLANE0_OFFSET_EXT, 0,
        EGL_DMA_BUF_PLANE0_PITCH_EXT, (EGLint)fb->stride,
        EGL_NONE
    };
    t->image = E.eglCreateImageKHR(dpy, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT,
                                   NULL, attrs);
    if (t->image == EGL_NO_IMAGE_KHR) {
        fprintf(stderr, "egl: eglCreateImageKHR(dma-buf fd=%d) eglError=0x%x\n",
                fb->prime_fd, E.eglGetError());
        return -1;
    }
    E.glGenTextures(1, &t->tex);
    E.glBindTexture(GL_TEXTURE_2D, t->tex);
    E.glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, t->image);
    GLenum err = E.glGetError();
    if (err != GL_NO_ERROR) {
        fprintf(stderr, "gl: glEGLImageTargetTexture2DOES glError=0x%x\n", err);
        return -1;
    }
    E.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    E.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    E.glGenFramebuffers(1, &t->fbo);
    E.glBindFramebuffer(GL_FRAMEBUFFER, t->fbo);
    E.glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                             GL_TEXTURE_2D, t->tex, 0);
    GLenum st = E.glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (st != GL_FRAMEBUFFER_COMPLETE) {
        fprintf(stderr, "gl: FBO on imported image incomplete status=0x%x\n",
                st);
        return -1;
    }
    return 0;
}

/* Render the red triangle on green into whatever FBO is bound.
 * angle_deg rotates the content; the GL->scanout Y flip is baked in. */
static void render_scene(uint32_t w, uint32_t h, float angle_deg)
{
    float a = angle_deg * (float)M_PI / 180.0f;
    float c = cosf(a), s = sinf(a);
    /* rot(a) then flip Y: [c s; -s -c]... row2 = -(row2 of rot) */
    E.glUniform4f(u_rot, c, -s, -s, -c);
    static const GLfloat verts[] = { -0.9f, -0.9f, 0.9f, -0.9f, 0.0f, 0.9f };
    E.glViewport(0, 0, (GLint)w, (GLint)h);
    E.glClearColor(0.0f, 1.0f, 0.0f, 1.0f);
    E.glClear(GL_COLOR_BUFFER_BIT);
    E.glEnableVertexAttribArray(0);
    E.glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, verts);
    E.glDrawArrays(GL_TRIANGLES, 0, 3);
    if (E.glGetError() != GL_NO_ERROR)
        die("draw");
}

/* Wait for the GPU to finish: fence sync when available (the presenter's
 * required ordering before AddFB2/flip), glFinish fallback. */
static void gpu_sync(void)
{
    if (E.eglCreateSyncKHR && E.eglClientWaitSyncKHR && E.eglDestroySyncKHR) {
        EGLSyncKHR sync = E.eglCreateSyncKHR(dpy, EGL_SYNC_FENCE_KHR, NULL);
        if (sync != EGL_NO_SYNC_KHR) {
            EGLint r = E.eglClientWaitSyncKHR(
                dpy, sync, EGL_SYNC_FLUSH_COMMANDS_BIT_KHR,
                1000000000ull /* 1s */);
            E.eglDestroySyncKHR(dpy, sync);
            if (r == EGL_CONDITION_SATISFIED_KHR)
                return;
            fprintf(stderr, "egl: fence wait returned 0x%x — glFinish fallback\n",
                    r);
        }
    }
    E.glFinish();
}

/* --readpixels fallback: render to the (small) currently-bound target is
 * not possible — instead render into a WxH pbuffer-sized viewport on the
 * default surface only if the pbuffer is big enough. Simpler and always
 * correct: render offscreen to an RGBA readback via the bound FBO if
 * available, else the pbuffer. Here: render on the DEFAULT surface (the
 * pbuffer) is limited to 16x16, so the fallback allocates its own WxH
 * pbuffer once. */
static EGLSurface rp_surf = EGL_NO_SURFACE;

static void readpixels_into(struct pf_dumb_fb *fb, float angle_deg)
{
    if (rp_surf == EGL_NO_SURFACE) {
        static const EGLint cfg_attrs[] = {
            EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
            EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
            EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8,
            EGL_ALPHA_SIZE, 8,
            EGL_NONE
        };
        EGLConfig cfg;
        EGLint ncfg = 0;
        if (!E.eglChooseConfig(dpy, cfg_attrs, &cfg, 1, &ncfg) || ncfg < 1)
            die("rp:eglChooseConfig");
        const EGLint pb[] = { EGL_WIDTH, (EGLint)fb->width,
                              EGL_HEIGHT, (EGLint)fb->height, EGL_NONE };
        rp_surf = E.eglCreatePbufferSurface(dpy, cfg, pb);
        if (rp_surf == EGL_NO_SURFACE)
            die("rp:eglCreatePbufferSurface");
    }
    if (!E.eglMakeCurrent(dpy, rp_surf, rp_surf, ctx))
        die("rp:eglMakeCurrent");
    E.glBindFramebuffer(GL_FRAMEBUFFER, 0);
    render_scene(fb->width, fb->height, angle_deg);
    E.glFinish();

    size_t rowbytes = (size_t)fb->width * 4;
    uint8_t *rgba = malloc(rowbytes * fb->height);
    if (!rgba)
        die("rp:malloc");
    E.glPixelStorei(GL_PACK_ALIGNMENT, 1);
    E.glReadPixels(0, 0, (GLint)fb->width, (GLint)fb->height, GL_RGBA,
                   GL_UNSIGNED_BYTE, rgba);
    if (E.glGetError() != GL_NO_ERROR)
        die("rp:readpixels");

    /* RGBA rows (bottom-up) -> XRGB8888 rows (top-down). render_scene
     * already baked a Y flip for the FBO path; glReadPixels flips again,
     * so copy rows straight to land identical to the zero-copy path. */
    pf_dmabuf_sync(fb->prime_fd, 1, 1);
    for (uint32_t y = 0; y < fb->height; y++) {
        const uint8_t *src = rgba + (size_t)y * rowbytes;
        uint32_t *dst = (uint32_t *)((uint8_t *)fb->map +
                                     (size_t)y * fb->stride);
        for (uint32_t x = 0; x < fb->width; x++) {
            const uint8_t *p = src + (size_t)x * 4;
            dst[x] = 0xff000000u | ((uint32_t)p[0] << 16) |
                     ((uint32_t)p[1] << 8) | p[2];
        }
    }
    pf_dmabuf_sync(fb->prime_fd, 0, 1);
    free(rgba);
}

/* Verify the rendered pattern in an XRGB8888 dumb buffer via CPU. */
static int verify_buffer(struct pf_dumb_fb *fb, const char *tag)
{
    pf_dmabuf_sync(fb->prime_fd, 1, 0);
    uint8_t *base = fb->map;
    /* center pixel */
    uint32_t cx = fb->width / 2, cy = fb->height / 2;
    const uint8_t *c = base + (size_t)cy * fb->stride + (size_t)cx * 4;
    /* corner well outside the triangle */
    const uint8_t *k = base + 4 * (size_t)fb->stride + 4 * 4;
    /* hash the visible rows (stride padding excluded) */
    uint64_t h = 0xcbf29ce484222325ULL;
    for (uint32_t y = 0; y < fb->height; y++) {
        const uint8_t *row = base + (size_t)y * fb->stride;
        for (size_t i = 0; i < (size_t)fb->width * 4; i++) {
            h ^= row[i];
            h *= 0x100000001b3ULL;
        }
    }
    pf_dmabuf_sync(fb->prime_fd, 0, 0);
    /* XRGB8888 little-endian bytes: B G R X */
    int center_red = c[2] > 200 && c[1] < 50 && c[0] < 50;
    int corner_green = k[2] < 50 && k[1] > 200 && k[0] < 50;
    printf("%s-PIXELS: center=bgrx:%02x%02x%02x%02x corner=bgrx:%02x%02x%02x%02x fnv1a64=%016llx\n",
           tag, c[0], c[1], c[2], c[3], k[0], k[1], k[2], k[3],
           (unsigned long long)h);
    return center_red && corner_green;
}

/* ---------------- modes ---------------- */

static int run_smoke(const char *card, const char *driver, int frames)
{
    mode_tag = "SMOKE";
    int fd = pf_drm_open(card, driver ? driver : (card ? NULL : "vkms"));
    if (fd < 0)
        goto fail;
    if (pf_drm_set_caps(fd) < 0)
        goto fail;
    struct pf_kms k;
    if (pf_kms_discover(fd, &k) < 0)
        goto fail;

    struct pf_dumb_fb fb[2];
    for (int i = 0; i < 2; i++) {
        if (pf_drm_create_dumb(fd, k.mode.hdisplay, k.mode.vdisplay,
                               &fb[i]) < 0 ||
            pf_drm_map_dumb(fd, &fb[i]) < 0 ||
            pf_drm_addfb2(fd, &fb[i], DRM_FORMAT_XRGB8888) < 0)
            goto fail;
    }

    fill_pattern(&fb[0], 0);
    if (pf_kms_modeset(fd, &k, fb[0].fb_id) < 0)
        goto fail;
    fprintf(stderr, "smoke: modeset done, flipping %d frames\n", frames);

    int back = 1;
    for (int f = 1; f <= frames; f++) {
        fill_pattern(&fb[back], f);
        if (pf_kms_flip(fd, &k, fb[back].fb_id) < 0)
            goto fail;
        if (pf_kms_wait_flip(fd) < 0)
            goto fail;
        back ^= 1;
    }
    char name[64];
    pf_drm_driver_name(fd, name, sizeof name);
    printf("SMOKE: PASS driver=%s flip-loop frames=%d mode=%ux%u fourcc=XR24\n",
           name, frames, k.mode.hdisplay, k.mode.vdisplay);
    return 0;
fail:
    printf("SMOKE: FAIL\n");
    return 1;
}

static int run_import_proof(const char *card, const char *driver,
                            uint32_t w, uint32_t h, int readpixels)
{
    mode_tag = "IMPORT-PROOF";
    int fd = pf_drm_open(card, driver);
    if (fd < 0)
        goto fail;
    char name[64];
    pf_drm_driver_name(fd, name, sizeof name);

    struct pf_dumb_fb fb;
    if (pf_drm_create_dumb(fd, w, h, &fb) < 0 ||
        pf_drm_map_dumb(fd, &fb) < 0 || pf_drm_export_dumb(fd, &fb) < 0)
        goto fail;
    fprintf(stderr,
            "import-proof: dumb %ux%u stride=%u size=%llu prime_fd=%d on %s\n",
            w, h, fb.stride, (unsigned long long)fb.size, fb.prime_fd, name);

    egl_setup();
    gl_program_setup();

    int zero_copy = 0;
    if (!readpixels) {
        struct gl_target t;
        if (gl_target_setup(&fb, &t) == 0) {
            E.glBindFramebuffer(GL_FRAMEBUFFER, t.fbo);
            render_scene(w, h, 0.0f);
            gpu_sync();
            zero_copy = 1;
        } else {
            fprintf(stderr,
                    "import-proof: EGLImage path failed — falling back to "
                    "readpixels proof-of-life\n");
        }
    }
    if (!zero_copy)
        readpixels_into(&fb, 0.0f);

    int ok = verify_buffer(&fb, "IMPORT-PROOF");
    printf("IMPORT-PROOF: %s path=%s driver=%s %ux%u\n",
           ok ? "PASS" : "FAIL", zero_copy ? "eglimage-zero-copy" : "readpixels-copy",
           name, w, h);
    return ok ? 0 : 1;
fail:
    printf("IMPORT-PROOF: FAIL\n");
    return 1;
}

static int run_present(const char *card, const char *driver, int frames,
                       int rotate, int readpixels)
{
    mode_tag = "PRESENT";
    int fd = pf_drm_open(card, driver);
    if (fd < 0)
        goto fail;
    if (pf_drm_set_caps(fd) < 0)
        goto fail;
    struct pf_kms k;
    if (pf_kms_discover(fd, &k) < 0)
        goto fail;

    struct pf_dumb_fb fb[2];
    struct gl_target t[2];
    for (int i = 0; i < 2; i++) {
        if (pf_drm_create_dumb(fd, k.mode.hdisplay, k.mode.vdisplay,
                               &fb[i]) < 0 ||
            pf_drm_map_dumb(fd, &fb[i]) < 0 ||
            pf_drm_export_dumb(fd, &fb[i]) < 0 ||
            pf_drm_addfb2(fd, &fb[i], DRM_FORMAT_XRGB8888) < 0)
            goto fail;
    }

    egl_setup();
    gl_program_setup();

    int zero_copy = 0;
    if (!readpixels) {
        if (gl_target_setup(&fb[0], &t[0]) == 0 &&
            gl_target_setup(&fb[1], &t[1]) == 0)
            zero_copy = 1;
        else
            fprintf(stderr,
                    "present: EGLImage path failed — readpixels fallback\n");
    }

    /* frame 0 into fb[0], modeset onto it */
    if (zero_copy) {
        E.glBindFramebuffer(GL_FRAMEBUFFER, t[0].fbo);
        render_scene(fb[0].width, fb[0].height, (float)rotate);
        gpu_sync();
    } else {
        readpixels_into(&fb[0], (float)rotate);
    }
    if (pf_kms_modeset(fd, &k, fb[0].fb_id) < 0)
        goto fail;
    fprintf(stderr, "present: modeset done (%s), flipping %d frames\n",
            zero_copy ? "eglimage-zero-copy" : "readpixels-copy", frames);

    int back = 1;
    for (int f = 1; f <= frames; f++) {
        float angle = (float)rotate + (float)f * 3.0f; /* visible motion */
        if (zero_copy) {
            E.glBindFramebuffer(GL_FRAMEBUFFER, t[back].fbo);
            render_scene(fb[back].width, fb[back].height, angle);
            gpu_sync(); /* fence BEFORE the flip — required ordering */
        } else {
            readpixels_into(&fb[back], angle);
        }
        if (pf_kms_flip(fd, &k, fb[back].fb_id) < 0)
            goto fail;
        if (pf_kms_wait_flip(fd) < 0)
            goto fail;
        back ^= 1;
    }
    printf("PRESENT: PASS path=%s frames=%d mode=%ux%u rotate=%d fourcc=XR24\n",
           zero_copy ? "eglimage-zero-copy" : "readpixels-copy", frames,
           k.mode.hdisplay, k.mode.vdisplay, rotate);
    return 0;
fail:
    printf("PRESENT: FAIL\n");
    return 1;
}

/* ---------------- main ---------------- */

static void usage(void)
{
    fprintf(stderr,
        "usage: pf-drm-present --smoke|--import-proof|--present\n"
        "         [--card /dev/dri/cardN] [--driver NAME] [--frames N]\n"
        "         [--size WxH] [--rotate 0|90|180|270] [--readpixels]\n");
    exit(2);
}

int main(int argc, char **argv)
{
    enum { NONE, SMOKE, IMPORT, PRESENT } mode = NONE;
    const char *card = NULL, *driver = NULL;
    int frames = 60, rotate = 0, readpixels = 0;
    uint32_t w = 256, h = 256;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--smoke")) mode = SMOKE;
        else if (!strcmp(argv[i], "--import-proof")) mode = IMPORT;
        else if (!strcmp(argv[i], "--present")) mode = PRESENT;
        else if (!strcmp(argv[i], "--card") && i + 1 < argc) card = argv[++i];
        else if (!strcmp(argv[i], "--driver") && i + 1 < argc) driver = argv[++i];
        else if (!strcmp(argv[i], "--frames") && i + 1 < argc) frames = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--rotate") && i + 1 < argc) rotate = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--readpixels")) readpixels = 1;
        else if (!strcmp(argv[i], "--size") && i + 1 < argc) {
            if (sscanf(argv[++i], "%ux%u", &w, &h) != 2 || !w || !h)
                usage();
        } else usage();
    }
    if (rotate != 0 && rotate != 90 && rotate != 180 && rotate != 270)
        usage();

    switch (mode) {
    case SMOKE:   return run_smoke(card, driver, frames);
    case IMPORT:  return run_import_proof(card, driver, w, h, readpixels);
    case PRESENT: return run_present(card, driver, frames, rotate, readpixels);
    default:      usage();
    }
    return 2;
}

/* pf-egl-audit — runtime EGL/GLES capability audit for the closed PowerVR
 * userspace on the mainline kernel (tsp-mc9m.15.2).
 *
 * Successor of a2-egl-pbuffer-test (tsp-mc9m.2, homed here): same headless
 * pbuffer render proof and output contract (A2-RENDER: PASS, canonical
 * fnv1a64=ece220fb80ed4271 for the 256x256 red-triangle-on-green), extended
 * to close the strings-vs-exposure gap:
 *   - dumps eglQueryString(EGL_EXTENSIONS) + glGetString(GL_EXTENSIONS)
 *   - reports which scanout-relevant extension ENTRY POINTS actually
 *     resolve through eglGetProcAddress (a string can be advertised while
 *     the entry point is absent, and vice versa)
 *   - dumps eglQueryDmaBufFormatsEXT / eglQueryDmaBufModifiersEXT when
 *     present (guarded; the blob advertises import_modifiers but the
 *     query entry points may be absent)
 *
 * Runtime prereqs on the device: pvrsrvkm.ko + dc_null.ko loaded (the blob's
 * WSEGL_InitialiseDisplay needs DCDevicesQueryCount>=1 even for the pbuffer
 * path — tsp-mc9m.9 round-7), rgx firmware staged, blob .so set resolvable
 * (/usr/lib/pvr-rogue via ld.so.conf or LD_LIBRARY_PATH).
 *
 * EGL_DEFAULT_DISPLAY only — this blob ships no eglGetPlatformDisplay.
 *
 * Output contract (greppable over serial): AUDIT-*: lines, then
 * A2-RENDER: PASS|FAIL as the overall verdict line.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "../common/egl_dl.h"
#include "../common/fnv1a64.h"

#define W 256
#define H 256

static EGLDisplay dpy;

static void die(const char *stage)
{
    fprintf(stderr, "A2-RENDER: FAIL stage=%s eglError=0x%x glError=0x%x\n",
            stage, E.eglGetError ? E.eglGetError() : 0,
            E.glGetError ? E.glGetError() : 0);
    exit(1);
}

/* Print a space-separated extension list one-per-line under a tag. */
static void dump_list(const char *tag, const char *list)
{
    if (!list) {
        printf("%s: (null)\n", tag);
        return;
    }
    char *dup = strdup(list);
    for (char *tok = strtok(dup, " "); tok; tok = strtok(NULL, " "))
        printf("%s: %s\n", tag, tok);
    free(dup);
}

static void dump_entry_points(void)
{
#define X(t, n) \
    printf("AUDIT-ENTRY: %s=%s\n", #n, E.n ? "present" : "ABSENT");
    PF_EXT_FNS(X)
#undef X
}

static void dump_dmabuf_formats(void)
{
    if (!E.eglQueryDmaBufFormatsEXT) {
        printf("AUDIT-DMABUF: eglQueryDmaBufFormatsEXT absent — skipping format query\n");
        return;
    }
    EGLint n = 0;
    if (!E.eglQueryDmaBufFormatsEXT(dpy, 0, NULL, &n)) {
        printf("AUDIT-DMABUF: format count query failed eglError=0x%x\n",
               E.eglGetError());
        return;
    }
    printf("AUDIT-DMABUF: %d import formats\n", n);
    if (n <= 0 || n > 256)
        return;
    EGLint *fmts = calloc(n, sizeof *fmts);
    if (!E.eglQueryDmaBufFormatsEXT(dpy, n, fmts, &n)) {
        free(fmts);
        return;
    }
    for (EGLint i = 0; i < n; i++) {
        uint32_t f = (uint32_t)fmts[i];
        char cc[5] = { f & 0xff, (f >> 8) & 0xff, (f >> 16) & 0xff,
                       (f >> 24) & 0xff, 0 };
        printf("AUDIT-DMABUF-FMT: 0x%08x '%s'", f, cc);
        if (E.eglQueryDmaBufModifiersEXT) {
            EGLint nm = 0;
            if (E.eglQueryDmaBufModifiersEXT(dpy, fmts[i], 0, NULL, NULL, &nm)) {
                printf(" modifiers=%d", nm);
                if (nm > 0 && nm <= 64) {
                    EGLuint64KHR mods[64];
                    EGLBoolean ext_only[64];
                    if (E.eglQueryDmaBufModifiersEXT(dpy, fmts[i], nm, mods,
                                                     ext_only, &nm))
                        for (EGLint m = 0; m < nm; m++)
                            printf(" 0x%016llx%s",
                                   (unsigned long long)mods[m],
                                   ext_only[m] ? "(ext-only)" : "");
                }
            }
        }
        printf("\n");
    }
    free(fmts);
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
        die(type == GL_VERTEX_SHADER ? "vshader" : "fshader");
    }
    return s;
}

int main(void)
{
    if (pf_egl_load() != 0)
        die("dlopen");

    dpy = E.eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (dpy == EGL_NO_DISPLAY)
        die("eglGetDisplay");
    EGLint maj = 0, min = 0;
    if (!E.eglInitialize(dpy, &maj, &min))
        die("eglInitialize");
    printf("A2-EGL: version=%d.%d vendor=\"%s\" apis=\"%s\"\n", maj, min,
           E.eglQueryString(dpy, EGL_VENDOR),
           E.eglQueryString(dpy, EGL_CLIENT_APIS));

    dump_list("AUDIT-EGL-EXT", E.eglQueryString(dpy, EGL_EXTENSIONS));
    dump_entry_points();
    dump_dmabuf_formats();

    /* ---- pbuffer render proof (verbatim a2-egl-pbuffer-test) ---- */
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

    static const EGLint pb_attrs[] = { EGL_WIDTH, W, EGL_HEIGHT, H, EGL_NONE };
    EGLSurface surf = E.eglCreatePbufferSurface(dpy, cfg, pb_attrs);
    if (surf == EGL_NO_SURFACE)
        die("eglCreatePbufferSurface");

    static const EGLint ctx_attrs[] = { EGL_CONTEXT_CLIENT_VERSION, 2,
                                        EGL_NONE };
    EGLContext ctx = E.eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, ctx_attrs);
    if (ctx == EGL_NO_CONTEXT)
        die("eglCreateContext");
    if (!E.eglMakeCurrent(dpy, surf, surf, ctx))
        die("eglMakeCurrent");

    printf("A2-GL: renderer=\"%s\" vendor=\"%s\" version=\"%s\"\n",
           E.glGetString(GL_RENDERER), E.glGetString(GL_VENDOR),
           E.glGetString(GL_VERSION));
    dump_list("AUDIT-GLES-EXT", (const char *)E.glGetString(GL_EXTENSIONS));

    static const char *vsrc =
        "attribute vec2 pos;\n"
        "void main(){ gl_Position = vec4(pos, 0.0, 1.0); }\n";
    static const char *fsrc =
        "precision mediump float;\n"
        "void main(){ gl_FragColor = vec4(1.0, 0.0, 0.0, 1.0); }\n";

    GLuint prog = E.glCreateProgram();
    E.glAttachShader(prog, mkshader(GL_VERTEX_SHADER, vsrc));
    E.glAttachShader(prog, mkshader(GL_FRAGMENT_SHADER, fsrc));
    E.glBindAttribLocation(prog, 0, "pos");
    E.glLinkProgram(prog);
    GLint ok = 0;
    E.glGetProgramiv(prog, GL_LINK_STATUS, &ok);
    if (!ok)
        die("link");
    E.glUseProgram(prog);

    /* big triangle covering the center of the viewport */
    static const GLfloat verts[] = { -0.9f, -0.9f, 0.9f, -0.9f, 0.0f, 0.9f };
    E.glViewport(0, 0, W, H);
    E.glClearColor(0.0f, 1.0f, 0.0f, 1.0f);
    E.glClear(GL_COLOR_BUFFER_BIT);
    E.glEnableVertexAttribArray(0);
    E.glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, verts);
    E.glDrawArrays(GL_TRIANGLES, 0, 3);
    if (E.glGetError() != GL_NO_ERROR)
        die("draw");
    E.glFinish();

    static uint8_t px[W * H * 4];
    E.glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, px);
    if (E.glGetError() != GL_NO_ERROR)
        die("readpixels");

    uint64_t h = fnv1a64(px, sizeof px);
    /* center should be red (triangle), corner green (clear) */
    size_t c = ((H / 2) * W + W / 2) * 4;
    size_t k = ((H - 4) * W + 4) * 4;
    printf("A2-PIXELS: center=%02x%02x%02x%02x corner=%02x%02x%02x%02x fnv1a64=%016llx\n",
           px[c], px[c + 1], px[c + 2], px[c + 3],
           px[k], px[k + 1], px[k + 2], px[k + 3], (unsigned long long)h);

    int center_red = px[c] > 200 && px[c + 1] < 50 && px[c + 2] < 50;
    int corner_green = px[k] < 50 && px[k + 1] > 200 && px[k + 2] < 50;
    if (center_red && corner_green) {
        printf("A2-RENDER: PASS center=red corner=green %dx%d pbuffer, GPU render verified\n",
               W, H);
        return 0;
    }
    printf("A2-RENDER: FAIL pixel-mismatch (center_red=%d corner_green=%d)\n",
           center_red, corner_green);
    return 1;
}

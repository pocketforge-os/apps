#include "egl_dl.h"

#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>

struct pf_egl E;

static void *dlopen_or_env(const char *envvar, const char *fallback)
{
    const char *name = getenv(envvar);
    if (!name || !*name)
        name = fallback;
    void *h = dlopen(name, RTLD_NOW | RTLD_GLOBAL);
    if (!h)
        fprintf(stderr, "egl_dl: dlopen(%s): %s\n", name, dlerror());
    return h;
}

int pf_egl_load(void)
{
    /* RTLD_GLOBAL: the blob's internal libs (libIMGegl, libsrv_um, ...)
     * cross-resolve symbols between each other at load time. */
    void *egl = dlopen_or_env("PF_EGL_LIB", "libEGL.so.1");
    if (!egl)
        return -1;
    void *gles = dlopen_or_env("PF_GLES_LIB", "libGLESv2.so.2");
    if (!gles)
        return -1;

#define X(n) \
    E.n = (__typeof__(&n))dlsym(egl, #n); \
    if (!E.n) { fprintf(stderr, "egl_dl: dlsym(%s): %s\n", #n, dlerror()); return -1; }
    PF_EGL_FNS(X)
#undef X

#define X(n) \
    E.n = (__typeof__(&n))dlsym(gles, #n); \
    if (!E.n) { fprintf(stderr, "egl_dl: dlsym(%s): %s\n", #n, dlerror()); return -1; }
    PF_GLES_FNS(X)
#undef X

    /* Extensions: resolved via eglGetProcAddress; NULL is a reportable
     * result (strings-vs-exposure gap), not a load failure. */
#define X(t, n) E.n = (t)E.eglGetProcAddress(#n);
    PF_EXT_FNS(X)
#undef X

    return 0;
}

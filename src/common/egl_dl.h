/* egl_dl — dlopen/dlsym loader for the closed PowerVR EGL/GLES2 userspace.
 *
 * Nothing in these tools links against the blobs at build time: the same
 * source builds natively (x86_64, DRM-only vkms smoke — EGL never loaded)
 * and cross (aarch64, loads /usr/lib/pvr-rogue via ld.so at runtime).
 *
 * Library names default to libEGL.so.1 / libGLESv2.so.2 (resolved through
 * ld.so — /etc/ld.so.conf.d/00-pvr.conf puts /usr/lib/pvr-rogue first on
 * the device). Override with PF_EGL_LIB / PF_GLES_LIB.
 *
 * Extension entry points are resolved through eglGetProcAddress and may be
 * NULL — callers must check (that strings-vs-exposure gap is exactly what
 * pf-egl-audit reports on).
 */
#ifndef PF_EGL_DL_H
#define PF_EGL_DL_H

#define EGL_EGLEXT_PROTOTYPES
#define GL_GLEXT_PROTOTYPES
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>

/* Core EGL symbols, dlsym'd from libEGL. */
#define PF_EGL_FNS(X) \
    X(eglGetDisplay) X(eglInitialize) X(eglTerminate) X(eglQueryString) \
    X(eglGetError) X(eglGetProcAddress) X(eglChooseConfig) \
    X(eglGetConfigAttrib) X(eglCreatePbufferSurface) X(eglDestroySurface) \
    X(eglCreateContext) X(eglDestroyContext) X(eglMakeCurrent) \
    X(eglSwapBuffers) X(eglBindAPI)

/* Core GLES2 symbols, dlsym'd from libGLESv2. */
#define PF_GLES_FNS(X) \
    X(glGetString) X(glGetError) X(glGetIntegerv) \
    X(glCreateShader) X(glShaderSource) X(glCompileShader) \
    X(glGetShaderiv) X(glGetShaderInfoLog) X(glDeleteShader) \
    X(glCreateProgram) X(glAttachShader) X(glBindAttribLocation) \
    X(glLinkProgram) X(glGetProgramiv) X(glGetProgramInfoLog) \
    X(glUseProgram) X(glDeleteProgram) X(glGetUniformLocation) \
    X(glUniform1f) X(glUniform2f) X(glUniform4f) \
    X(glViewport) X(glClearColor) X(glClear) \
    X(glEnableVertexAttribArray) X(glVertexAttribPointer) X(glDrawArrays) \
    X(glFinish) X(glFlush) X(glReadPixels) X(glPixelStorei) \
    X(glGenTextures) X(glBindTexture) X(glTexParameteri) X(glTexImage2D) \
    X(glDeleteTextures) \
    X(glGenFramebuffers) X(glBindFramebuffer) X(glFramebufferTexture2D) \
    X(glCheckFramebufferStatus) X(glDeleteFramebuffers)

/* Extension entry points, via eglGetProcAddress; each may be NULL. */
#define PF_EXT_FNS(X) \
    X(PFNEGLCREATEIMAGEKHRPROC,            eglCreateImageKHR) \
    X(PFNEGLDESTROYIMAGEKHRPROC,           eglDestroyImageKHR) \
    X(PFNGLEGLIMAGETARGETTEXTURE2DOESPROC, glEGLImageTargetTexture2DOES) \
    X(PFNEGLCREATESYNCKHRPROC,             eglCreateSyncKHR) \
    X(PFNEGLCLIENTWAITSYNCKHRPROC,         eglClientWaitSyncKHR) \
    X(PFNEGLDESTROYSYNCKHRPROC,            eglDestroySyncKHR) \
    X(PFNEGLQUERYDMABUFFORMATSEXTPROC,     eglQueryDmaBufFormatsEXT) \
    X(PFNEGLQUERYDMABUFMODIFIERSEXTPROC,   eglQueryDmaBufModifiersEXT)

struct pf_egl {
#define X(n) __typeof__(&n) n;
    PF_EGL_FNS(X)
    PF_GLES_FNS(X)
#undef X
#define X(t, n) t n;
    PF_EXT_FNS(X)
#undef X
};

/* The single loaded instance (tools are single-threaded). */
extern struct pf_egl E;

/* Load libEGL + libGLESv2 and resolve everything above. Returns 0 on
 * success; on failure prints the dlopen/dlsym error to stderr and returns
 * -1. Extension pointers may still be NULL after success. */
int pf_egl_load(void);

#endif

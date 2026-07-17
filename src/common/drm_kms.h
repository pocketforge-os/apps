/* drm_kms — minimal raw-UAPI DRM/KMS helpers (no libdrm).
 *
 * Deliberately ioctl-level against the vendored kernel UAPI headers so the
 * tools build identically in the pinned cross container (which ships no
 * libdrm) and stay fully static. Covers exactly what the presenter needs:
 * dumb buffers, PRIME export, AddFB2, atomic modeset + flip.
 */
#ifndef PF_DRM_KMS_H
#define PF_DRM_KMS_H

#include <stdint.h>
#include <drm.h>
#include <drm_mode.h>
#include <drm_fourcc.h>

/* One 32bpp dumb-buffer framebuffer. */
struct pf_dumb_fb {
    uint32_t handle;   /* GEM handle */
    uint32_t fb_id;    /* KMS FB id, 0 until pf_drm_addfb2 */
    uint32_t width, height, stride;
    uint64_t size;
    void *map;         /* CPU view, NULL until pf_drm_map_dumb */
    int prime_fd;      /* dma-buf fd, -1 until pf_drm_export_dumb */
};

/* Discovered modeset pipe + the atomic property ids it needs. */
struct pf_kms {
    uint32_t connector_id, crtc_id, plane_id;
    struct drm_mode_modeinfo mode;
    uint32_t mode_blob_id;
    /* property ids */
    uint32_t conn_crtc_id;
    uint32_t crtc_mode_id, crtc_active;
    uint32_t plane_fb_id, plane_crtc_id;
    uint32_t plane_src_x, plane_src_y, plane_src_w, plane_src_h;
    uint32_t plane_crtc_x, plane_crtc_y, plane_crtc_w, plane_crtc_h;
};

/* ioctl with EINTR/EAGAIN retry (the drmIoctl idiom). */
int pf_ioctl(int fd, unsigned long req, void *arg);

/* Open a DRM node. path: explicit /dev/dri/cardN, or NULL to scan card0..
 * card15 for a node with dumb-buffer + atomic support (preferring a driver
 * whose name matches want_driver when given). Returns fd or -1. */
int pf_drm_open(const char *path, const char *want_driver);

/* Driver name of an open node (drm_version), "" on failure. */
void pf_drm_driver_name(int fd, char *buf, unsigned len);

/* Enable UNIVERSAL_PLANES + ATOMIC client caps. */
int pf_drm_set_caps(int fd);

int pf_drm_create_dumb(int fd, uint32_t w, uint32_t h, struct pf_dumb_fb *fb);
int pf_drm_map_dumb(int fd, struct pf_dumb_fb *fb);
int pf_drm_export_dumb(int fd, struct pf_dumb_fb *fb);   /* PRIME fd */
int pf_drm_addfb2(int fd, struct pf_dumb_fb *fb, uint32_t fourcc);
void pf_drm_destroy_dumb(int fd, struct pf_dumb_fb *fb);

/* dma-buf CPU-access bracketing (DMA_BUF_IOCTL_SYNC) around reads/writes
 * of the mapped buffer while the GPU also touches it. */
int pf_dmabuf_sync(int prime_fd, int start, int write);

/* Pick first connected connector (+mode[0]), a CRTC it can drive, and that
 * CRTC's primary plane; resolve all atomic property ids; create the mode
 * blob. Requires pf_drm_set_caps first. */
int pf_kms_discover(int fd, struct pf_kms *k);

/* Initial atomic commit: full modeset lighting up the pipe with fb_id. */
int pf_kms_modeset(int fd, struct pf_kms *k, uint32_t fb_id);

/* Per-frame atomic flip to fb_id with DRM_MODE_PAGE_FLIP_EVENT;
 * pf_kms_wait_flip blocks until the completion event arrives. */
int pf_kms_flip(int fd, struct pf_kms *k, uint32_t fb_id);
int pf_kms_wait_flip(int fd);

#endif

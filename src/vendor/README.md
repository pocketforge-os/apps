# Vendored headers — provenance

All headers here are unmodified copies; do not edit in place. Re-vendor from the
sources below when updating.

## drm-uapi/ + linux-uapi/

Kernel UAPI headers (GPL-2.0 WITH Linux-syscall-note — the UAPI exception makes
userspace use unrestricted):

- `drm-uapi/drm.h`, `drm-uapi/drm_mode.h`, `drm-uapi/drm_fourcc.h` — from
  `pocketforge-os/kernel-sunxi-6.x` @ `a994e1ad780c5a14fea3040e8fab9f7e62909653`
  (`device/a133`), paths `include/uapi/drm/*.h`.
- `linux-uapi/dma-buf.h` — same repo/commit, `include/uapi/linux/dma-buf.h`.

These are raw in-tree UAPI copies (not `make headers_install` output), so
they keep the `__user` sparse annotation — the build defines `-D__user=`
rather than editing the files.

Vendored (rather than relying on host `/usr/include/drm`) so the tools build
identically in the pinned `pocketforge/build:10.3-2021.07-bookworm` cross
container, which ships no DRM UAPI headers, and so the ABI we compile against
is exactly our own kernel's.

## khronos/

Stock Khronos registry headers (Apache-2.0, per each file's SPDX header):
`EGL/egl.h`, `EGL/eglext.h`, `EGL/eglplatform.h`, `KHR/khrplatform.h`,
`GLES2/gl2.h`, `GLES2/gl2ext.h`, `GLES2/gl2platform.h`.

Copied from the `blobs` checkout `tsp/22.102.54.38/include/` (see that repo's
`PROVENANCE.md`); they are the API headers matching the closed PowerVR DDK
1.19@6345021 userspace. Note the tools **dlopen** the GL/EGL libraries at
runtime — nothing links against the closed blobs at build time.

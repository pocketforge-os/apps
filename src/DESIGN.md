# GPU scanout bring-up tools — design & on-device runbook

Owned tools for bringing GPU pixels to the mainline A133 panel through the
closed PowerVR userspace, with **zero blob change** (bead `tsp-mc9m.15.2`,
epic `tsp-mc9m.15`). This doc records the pattern they prove, why it is the
only viable one, and the exact on-device commands to run them.

## The reversed-import scanout pattern (and why it is mandatory)

From the `tsp-mc9m.15` scanout synthesis, three hardware/blob facts force a
single design:

1. **GPU-native allocations are non-contiguous** (the DDK allocs via
   `alloc_pages`, UMA).
2. **`sun4i-drm` rejects non-contiguous dma-buf imports** (`drm_gem_dma_prime_import`),
   and the A100/A133 has **no upstream SoC IOMMU** — the DE2 display engine
   must scan out **physically contiguous** memory.
3. The blob EGL advertises **`EGL_EXT_image_dma_buf_import`** (+modifiers)
   but **no `EGL_MESA_image_dma_buf_export`** — it can import a dma-buf as a
   render target, but cannot export its own allocations.

Export-from-GPU is therefore impossible twice over (non-contiguous *and* no
export extension). The only path is to allocate on the **display** side and
import into the GPU:

```
sun4i-drm dumb buffer (CMA-contiguous)
  -> DRM_IOCTL_PRIME_HANDLE_TO_FD            (PRIME export, a dma-buf fd)
  -> eglCreateImageKHR(EGL_LINUX_DMA_BUF_EXT) (import into EGL)
  -> glEGLImageTargetTexture2DOES -> FBO      (GPU renders INTO the buffer)
  -> EGL_KHR_fence_sync                       (wait GPU done BEFORE scanout)
  -> drmModeAddFB2 + atomic commit/flip       (scan the same buffer out)
```

Double-buffered 720p XRGB8888 is ~7.4 MB — comfortable in the 4.9-era 64 MiB
CMA precedent. Pixel format is **XRGB8888 / DRM_FORMAT_XRGB8888**, matching
DC pixfmt 90 / `B8G8R8X8` (the 85e6dd6 lesson: the closed UM's render-target
classifier rejects the alpha variant).

## The two tools

Both **dlopen** the closed EGL/GLES userspace at runtime (nothing links the
blobs) and speak **raw DRM UAPI** (no libdrm) so they build identically in
the pinned glibc-2.33 cross container, which ships neither.

### `pf-egl-audit` — runtime capability audit + render proof

Closes the *strings-vs-exposure* gap: an extension can be advertised in the
string while its entry point is absent (or vice-versa). Dumps:

- `eglQueryString(EGL_EXTENSIONS)` and `glGetString(GL_EXTENSIONS)`
- **entry-point presence** of every scanout-relevant call
  (`eglCreateImageKHR`, `glEGLImageTargetTexture2DOES`, the fence-sync
  trio, `eglQueryDmaBufFormatsEXT/ModifiersEXT`) via `eglGetProcAddress`
- `eglQueryDmaBufFormatsEXT` formats + `eglQueryDmaBufModifiersEXT`
  modifiers (guarded — entry points may be absent)

then runs the canonical headless pbuffer render proof homed verbatim from
`a2-egl-pbuffer-test` (tsp-mc9m.2): red triangle on green, reference
`fnv1a64=ece220fb80ed4271`, `A2-RENDER: PASS`.

### `pf-drm-present` — owned DRM presenter

- `--smoke` — DRM half only (no EGL): dumb alloc + AddFB2 + atomic modeset
  + CPU-pattern flip loop. **Device-free** against vkms — the CI/dev smoke.
- `--import-proof` — headless, **no CRTC needed**: dumb alloc → PRIME →
  EGLImage → FBO → GPU render → mmap → verify pixels/`fnv1a64`. Runs on the
  mainline image *today* (card0/sun4i-drm registers without a display).
- `--present` — full path: modeset + GPU-rendered pattern + fence-synced
  atomic flip loop. Gated on DISPLAY first light (`tsp-mc9m.12.6`).
  `--rotate {0,90,180,270}` for the 270° panel (rotation is baked into the
  vertex transform alongside the GL→scanout Y flip).

### Fallback ladder (inside both import paths)

1. **EGLImage zero-copy** — GPU renders straight into the scanout dumb
   buffer. The product path.
2. **`--readpixels` copy** — GPU renders to a pbuffer, CPU copies into the
   dumb buffer (dma-buf-synced). Proof-of-life when EGLImage import fails at
   runtime; correctness only, not zero-copy.

If EGLImage import fails, the tool reports the exact failing call
(`eglCreateImageKHR` eglError / `glEGLImageTargetTexture2DOES` glError) so
the failure is diagnosable, then falls to the copy path.

## Build

```sh
make native                 # host build; `make smoke` runs the vkms smoke
./scripts/cross-build.sh    # aarch64 device build in the pinned container
```

The device userspace is glibc 2.33; stock Ubuntu cross gcc emits glibc-2.34+
symbols the device rejects, so cross builds go through
`pocketforge/build:10.3-2021.07-bookworm` (ARM 10.3-2021.07 toolchain).
Cross binaries are rpath-clean, NEEDED only libc/libdl/libm, max
`GLIBC_2.17`.

## On-device runbook (the handoff recipes)

Prereqs on the DUT (per tsp-mc9m.9): `pvrsrvkm.ko` **then** `dc_null.ko`
loaded (the blob's `WSEGL_InitialiseDisplay` needs `DCDevicesQueryCount>=1`
even headless), rgx firmware staged in `/lib/firmware`, and the blob `.so`
set resolvable — `/usr/lib/pvr-rogue` via `/etc/ld.so.conf.d/00-pvr.conf`
(or `export LD_LIBRARY_PATH=/usr/lib/pvr-rogue`). `EGL_DEFAULT_DISPLAY`
only (this blob ships no `eglGetPlatformDisplay`).

### 1. Extension audit + headless render proof (any GPU window; no display)

```sh
# expect: A2-EGL version=1.4 vendor="Imagination Technologies"
#         AUDIT-EGL-EXT: EGL_EXT_image_dma_buf_import  (+ _modifiers)
#         AUDIT-ENTRY: eglCreateImageKHR=present
#         AUDIT-ENTRY: glEGLImageTargetTexture2DOES=present
#         AUDIT-GLES-EXT: GL_OES_EGL_image
#         AUDIT-DMABUF: <N> import formats  (XR24 'XR24' expected)
#         A2-RENDER: PASS ... fnv1a64=ece220fb80ed4271
./pf-egl-audit
```

### 2. Headless dma-buf import proof (card0/sun4i-drm, no CRTC)

```sh
# proves the FULL reversed-import glue before any display exists:
# dumb-alloc -> PRIME -> EGLImage -> FBO render -> mmap -> verify.
# expect: IMPORT-PROOF: PASS path=eglimage-zero-copy driver=sun4i-drm 256x256
# if EGLImage import fails at runtime, add --readpixels for proof-of-life:
./pf-drm-present --import-proof --driver sun4i-drm
./pf-drm-present --import-proof --driver sun4i-drm --readpixels   # fallback
```

### 3. Full present to the panel (GATED on DISPLAY first light, tsp-mc9m.12.6)

```sh
# GPU-rendered animated pattern, fence-synced atomic flips onto the real
# CRTC/connector. --rotate 270 for the panel orientation.
# verify visually via capture-screen + review-screen (owner OK for the
# hardware acceptance gate).
./pf-drm-present --present --driver sun4i-drm --rotate 270 --frames 120
```

The audit (1) and import proof (2) need only **one** GPU window and no
display, so they can be brokered together; the full present (3) waits for
DISPLAY first light.

# PocketForge Apps

First-party app monorepo. Each app is a self-contained directory with an `app.toml` manifest, icon, and `launch` script conforming to the launcher-agnostic app contract.

Apps:
- `steamlink/` — Steam Link wrapper (first-boot fetch + `SDL3_DYNAMIC_API` binding)
- `poolsuite-fm/` — Native audio radio (poolside.fm)
- `poolsuite-tv/` — Native YouTube-playlist video (`mpv`/`yt-dlp` + Cedar HW decode)
- `poolsuite-web/` — WPE WebKit + Cog kiosk (optional, Phase 3)

Per-app install trees are consumed by the image builder. Adding an app = add a directory here; nothing in `launcher` or `libsdl3-sunxifb` changes.

Populated starting **Phase 1** (`steamlink` only), then **Phase 2** (FM/TV), **Phase 3** (Web). See the [pocketforge-os](https://github.com/pocketforge-os) org for the full repo set.

## Tools (`src/`)

Owned GPU/display bring-up tools for the mainline A133 stack (tsp-mc9m.15.2).
Both dlopen the closed PowerVR EGL/GLES userspace at runtime — nothing links
against the blobs at build time — and speak raw DRM UAPI (no libdrm).

- **`src/pf-egl-audit/`** — runtime EGL/GLES capability audit: extension
  strings, extension *entry-point* presence (the strings-vs-exposure gap),
  dma-buf import formats/modifiers, plus the canonical headless pbuffer
  render proof (successor of `a2-egl-pbuffer-test` from tsp-mc9m.2; same
  output contract, reference `fnv1a64=ece220fb80ed4271`).
- **`src/pf-drm-present/`** — owned DRM presenter proving the
  reversed-import scanout path (dumb buffer → PRIME → `eglCreateImageKHR` →
  FBO → GPU renders in → fence → atomic flip). `--smoke` runs the DRM half
  device-free against vkms; `--import-proof` proves the headless dma-buf
  import path (no CRTC needed); `--present` is the display-gated flip loop.
  `--readpixels` selects the copy-path fallback ladder.

Build: `make native` (host; `make smoke` for the vkms smoke) and
`scripts/cross-build.sh` (aarch64 device build inside the pinned
`pocketforge/build:10.3-2021.07-bookworm` container — the device userspace
is glibc 2.33, stock Ubuntu cross gcc output is rejected on-device).
Vendored header provenance: `src/vendor/README.md`.

# PocketForge Apps

First-party app monorepo. Each app is a self-contained directory with an `app.toml` manifest, icon, and `launch` script conforming to the launcher-agnostic app contract.

Apps:
- `steamlink/` — Steam Link wrapper (first-boot fetch + `SDL3_DYNAMIC_API` binding)
- `poolsuite-fm/` — Native audio radio (poolside.fm)
- `poolsuite-tv/` — Native YouTube-playlist video (`mpv`/`yt-dlp` + Cedar HW decode)
- `poolsuite-web/` — WPE WebKit + Cog kiosk (optional, Phase 3)

Per-app install trees are consumed by the image builder. Adding an app = add a directory here; nothing in `launcher` or `libsdl3-sunxifb` changes.

Populated starting **Phase 1** (`steamlink` only), then **Phase 2** (FM/TV), **Phase 3** (Web). See the [pocketforge-os](https://github.com/pocketforge-os) org for the full repo set.

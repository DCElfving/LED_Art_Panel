# AGENTS.md — LED Art Panel

Operational context for AI coding agents. Humans: see **[README.md](./README.md)**
(full setup, architecture, and a "make it your own" guide).

## What this is
ESP32 + 32×32 HUB75 RGB LED matrix. A self-hosted web UI (served from LittleFS)
lets users upload images/GIFs and run generative effects. Single-file Arduino
sketch + a self-contained web page.

## Project layout
- `LED_Art_Panel.ino` — firmware: WiFi, web server, playback, transitions, mDNS naming.
- `Effect2D.h` — generative effects (add new ones here).
- `MinimalFX.h` — tiny effects engine that pushes pixels to the panel.
- `PlaybackState.h` — reads one image file, holds the current 32×32 frame.
- `data/index.html` — the entire web UI (HTML+CSS+JS in one file, no external assets).
- `setup_panel.py` — stdlib Python environment setup for student computers (IDE,
  Rosetta, USB drivers, pinned core + libraries, LittleFS plugin, test compile;
  never flashes; runs from anywhere, test-compiling this sketch when run from
  its folder). Identical to the networked version's copy; this sketch's FQBN
  comes from `sketch.yaml`. Packaged with PyInstaller (`Setup-Mac`,
  `Setup-Windows.exe`, `Setup-Linux`; `.github/workflows/build-setup.yml`).
- `sketch.yaml` — `default_fqbn` with the No OTA partition scheme.

## Build & flash constraints (do not violate)
- **ESP32 Arduino core: 3.3.x stable.** NOT 4.x-alpha (removes `driver/i2s.h` →
  build fails), NOT 2.x.
- **Library:** `ESP32-HUB75-MatrixPanel-DMA` (MrCodetastic), v3.0.14+.
- **Partition scheme:** "No OTA (1MB APP / 3MB SPIFFS)".
- **macOS needs Apple's Command Line Developer Tools:** the ESP32 core runs `python3`
  on every build and upload, and `/usr/bin/python3` is only a stub without them.
- **Flashing is two parts:** the sketch AND the `data/` LittleFS image. Editing
  `data/index.html` requires re-uploading LittleFS, not just recompiling the sketch.
- Flash is ~98% full (≈16 KB free). Check `Sketch uses` after any firmware change.
- Keep the single-file `.ino` format. Keep `data/index.html` self-contained and
  offline-capable (no external fonts/scripts/images).
- Device identity comes from the factory MAC via `esp_read_mac(mac,
  ESP_MAC_WIFI_STA)` — never `WiFi.macAddress()` (returns zeros in AP-only mode).

## Test / preview
- No automated firmware test harness, and no mock server: verify by compiling in
  the Arduino IDE (or `arduino-cli compile` with the FQBN in `sketch.yaml`), then
  test UI changes on a panel after Upload LittleFS.

## Common tasks
- **Add an effect:** write `effect_x()` in `Effect2D.h`; add a `case` in
  `effect_2d_wrapper()`; add a default in `defaultSpeedFor2DEffect()`; raise the
  bound in `set2DEffect()`; add it to `WLED_MODES` + `drawModePreview()` in `data/index.html`.
- **Add an endpoint/setting:** `server.on(...)` route + handler in `.ino`; persist via
  `saveConfig()`/`loadConfig()`; expose in `handleList()`; wire a control + `fetch()` in the UI.

## HTTP endpoints
`GET /` (UI), `GET /list` (status+settings+images JSON), `GET /files` (gallery),
`POST /settings`, `POST /wled`, `POST /jump`, `POST /upload`, `DELETE /delete?name=`.

## Image format
`.bin` = `[uint16 frameCount]` + N frames of 1024 RGB565 pixels (2048 bytes each).
Conversion happens client-side in the browser before upload.

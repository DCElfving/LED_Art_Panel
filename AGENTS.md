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
- `mock_server.py` — local mock of the device's HTTP endpoints for UI testing.

## Build & flash constraints (do not violate)
- **ESP32 Arduino core: 3.3.x stable.** NOT 4.x-alpha (removes `driver/i2s.h` →
  build fails), NOT 2.x.
- **Library:** `ESP32-HUB75-MatrixPanel-DMA` (MrCodetastic), v3.0.14+.
- **Partition scheme:** "No OTA (1MB APP / 3MB SPIFFS)".
- **Flashing is two parts:** the sketch AND the `data/` LittleFS image. Editing
  `data/index.html` requires re-uploading LittleFS, not just recompiling the sketch.
- Keep the single-file `.ino` format. Keep `data/index.html` self-contained and
  offline-capable (no external fonts/scripts/images).
- Device identity comes from the factory MAC via `esp_read_mac(mac,
  ESP_MAC_WIFI_STA)` — never `WiFi.macAddress()` (returns zeros in AP-only mode).

## Test / preview
- UI without hardware: `cd LED_Art_Panel && python3 mock_server.py` → http://localhost:8000
  (pure stdlib; run from inside the folder — relative paths).
- No automated firmware test harness; verify by compiling in the Arduino IDE.
- When adding a setting, keep `mock_server.py`'s `/list` JSON in sync with the firmware.

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

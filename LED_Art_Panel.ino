/*
 * LED Art Panel
 * ---------------------------------------------------------------------------
 * A standalone, WiFi-enabled digital art frame: an ESP32 drives a 32x32 HUB75
 * RGB matrix that plays uploaded images/GIFs, or runs generative 2D effects.
 *
 * Two display modes:
 *   - Images:  shuffled playback of uploaded .bin frames with crossfade or
 *              pixel-dissolve transitions.
 *   - Effects: generative animations (Fire, Game of Life, Rain, Aurora).
 *
 * Networking:
 *   - Always hosts its own open Access Point (unique name per device).
 *   - Optionally joins a home WiFi network as well, with auto-reconnect.
 *   - Reachable by name via a tiny built-in mDNS responder (<host>.local).
 *
 * Build note: select "No OTA (1MB APP / 3MB SPIFFS)" under
 * Tools > Partition Scheme in the Arduino IDE to maximise image storage.
 * ---------------------------------------------------------------------------
 */

#include <ESP32-HUB75-MatrixPanel-I2S-DMA.h>
#include <WiFi.h>
#include <WebServer.h>
#include <LittleFS.h>
#include <WiFiUdp.h>
#include <esp_mac.h>     // esp_read_mac() — factory MAC, independent of WiFi state
#include "MinimalFX.h"   // Lightweight WS2812FX stand-in — saves flash

// ===========================================================================
// Panel configuration
// ===========================================================================
#define PANEL_RES_X 32
#define PANEL_RES_Y 32
#define PANEL_CHAIN 1
#define LED_COUNT   (PANEL_RES_X * PANEL_RES_Y)

// HUB75 pin map (GPIO). B0/B1 are non-standard on this PCB.
#define R1_PIN  18   // pin 1  (R0)
#define G1_PIN  21   // pin 2  (G0)
#define B1_PIN  4    // pin 4  (B0*)
#define R2_PIN  32   // pin 5  (R1)
#define G2_PIN  19   // pin 6  (G1)
#define B2_PIN  33   // pin 8  (B1*)
#define A_PIN   25   // pin 9  (A)
#define B_PIN   5    // pin 10 (B)
#define C_PIN   26   // pin 11 (C)
#define D_PIN   17   // pin 12 (D)
#define CLK_PIN 27   // pin 13 (CLK)
#define LAT_PIN 16   // pin 14 (LAT)
#define OE_PIN  14   // pin 15 (OE)
#define E_PIN   -1   // Not used (pin 8 repurposed as B1 on this panel)

// ===========================================================================
// Global state
// ===========================================================================
MatrixPanel_I2S_DMA *dma_display = nullptr;

// WiFi identity. Credentials load from flash; AP + host names are generated
// from the MAC in setupWiFi() so each panel is unique on a shared network.
String station_ssid = "";
String station_password = "";
char   ap_ssid[16]  = "";        // e.g. "Panel_C3D4"
char   panelId[5]   = "";        // e.g. "C3D4" — shown on the panel so users know which network to join
char   hostName[20] = "";        // e.g. "led-panelc3d4" -> led-panelc3d4.local
const char *ap_password = "";    // Open network

WebServer server(80);
WiFiUDP   mdnsUDP;                // Minimal mDNS responder socket

// Display mode.
enum : uint8_t { MODE_IMAGES = 0, MODE_EFFECTS = 1 };
uint8_t deviceMode = MODE_IMAGES;

// Image transition style.
enum : uint8_t { TRANS_CROSSFADE = 0, TRANS_PIXEL_DISSOLVE = 1 };
uint8_t transitionMode = TRANS_CROSSFADE;

// Effects engine (drives the matrix through myCustomShow()).
WS2812FX ws2812fx = WS2812FX(LED_COUNT, -1, NEO_RGB + NEO_KHZ800);
bool     wledMode = false;       // true only while in Effects mode

// Playback tuning.
uint8_t              matrixBrightness = 37;      // 0..100 (first-start default)
unsigned long        displayDuration  = 300000;  // ms each image is held
unsigned long        lastSwitchTime   = 0;
const unsigned long  crossfadeDuration = 3000;   // quick blend
const unsigned long  dissolveDuration  = 50000;  // slow, dramatic dissolve
const unsigned long  frameDuration     = 100;    // GIF frame advance interval

// Playlist of .bin files on flash.
#define MAX_PLAYLIST 64
String playlist[MAX_PLAYLIST];
int    playlist_count = 0;

// ===========================================================================
// Effects bridge — push the WS2812FX buffer onto the HUB75 matrix
// ===========================================================================
void myCustomShow() {
  if (!dma_display) return;
  for (int y = 0; y < PANEL_RES_Y; y++) {
    for (int x = 0; x < PANEL_RES_X; x++) {
      uint32_t color = ws2812fx.getPixelColor(y * PANEL_RES_X + x);
      dma_display->drawPixelRGB888(x, y,
        (uint8_t)(color >> 16), (uint8_t)(color >> 8), (uint8_t)color);
    }
  }
}

#include "Effect2D.h"       // 2D effect implementations (uses ws2812fx)
#include "PlaybackState.h"  // Per-image frame buffer + file reader

PlaybackState state32_curr, state32_next;
void updateAnimationState(PlaybackState &state, unsigned long now);  // fwd decl
void advanceToNextImage(unsigned long now);                          // fwd decl

// ===========================================================================
// Pixel-dissolve transition tables
// Pixels fade out/in one-by-one in a shuffled order for a "dissolve" effect.
// ===========================================================================
uint16_t  pixelOrder[1024];   // Fisher-Yates shuffled pixel indices
uint16_t  pixelRank[1024];    // Reverse lookup: shuffle position of each pixel
bool      phase2Shuffled = false;
const int FADE_WINDOW = 150;  // pixels fading at once (~15% of the panel)

void shufflePixelOrder() {
  for (int i = 0; i < 1024; i++) pixelOrder[i] = i;
  for (int i = 1023; i > 0; i--) {
    int j = random(i + 1);
    uint16_t tmp = pixelOrder[i];
    pixelOrder[i] = pixelOrder[j];
    pixelOrder[j] = tmp;
  }
  for (int i = 0; i < 1024; i++) pixelRank[pixelOrder[i]] = i;  // O(1) render lookup
}

// ===========================================================================
// Centered text, drawn with the Adafruit_GFX font built into the panel library.
// ===========================================================================
void drawTextCentered(int16_t y, const char *s, uint16_t color) {
  int16_t x1, y1; uint16_t w, h;
  dma_display->setTextWrap(false);
  dma_display->setTextSize(1);
  dma_display->setTextColor(color);
  dma_display->getTextBounds(s, 0, y, &x1, &y1, &w, &h);
  dma_display->setCursor((32 - (int)w) / 2, y);
  dma_display->print(s);
}

// ===========================================================================
// Config persistence (LittleFS)
// ===========================================================================
void saveConfig() {
  File f = LittleFS.open(F("/config.txt"), "w");
  if (!f) return;
  f.println(matrixBrightness);        // 0..100
  f.println(displayDuration / 1000);  // seconds
  f.println(deviceMode);              // 0=images, 1=effects
  f.println(transitionMode);          // 0=crossfade, 1=dissolve
  f.close();
}

void loadConfig() {
  if (!LittleFS.exists(F("/config.txt"))) return;  // keep defaults on first boot

  File f = LittleFS.open(F("/config.txt"), "r");
  if (!f) return;

  int b  = f.parseInt();
  int d  = f.parseInt();
  int m  = f.available() ? f.parseInt() : -1;
  int tm = f.available() ? f.parseInt() : -1;
  f.close();

  if (b >= 0 && b <= 100)                            matrixBrightness = (uint8_t)b;
  if (d > 0)                                         displayDuration  = (unsigned long)d * 1000;
  if (m == MODE_IMAGES || m == MODE_EFFECTS)         deviceMode       = (uint8_t)m;
  if (tm == TRANS_CROSSFADE || tm == TRANS_PIXEL_DISSOLVE) transitionMode = (uint8_t)tm;
}

void loadWifiConfig() {
  if (!LittleFS.exists(F("/wifi.txt"))) return;
  File f = LittleFS.open(F("/wifi.txt"), "r");
  if (!f) return;
  station_ssid     = f.readStringUntil('\n');
  station_password = f.readStringUntil('\n');
  station_ssid.trim();
  station_password.trim();
  f.close();
}

// ===========================================================================
// Playlist helpers
// ===========================================================================
void refreshPlaylist() {
  playlist_count = 0;
  File root = LittleFS.open(F("/"));
  File file = root.openNextFile();
  while (file && playlist_count < MAX_PLAYLIST) {
    String name = file.name();
    if (name.endsWith(F(".bin"))) {
      playlist[playlist_count++] = name.startsWith("/") ? name : String('/') + name;
    }
    file = root.openNextFile();
  }
}

// ===========================================================================
// Web handlers
// ===========================================================================
static String jsonEscape(const String &in) {
  String out;
  out.reserve(in.length() + 4);
  for (size_t i = 0; i < in.length(); i++) {
    char c = in[i];
    if (c == '"' || c == '\\') out += '\\';
    if ((uint8_t)c >= 0x20) out += c;
  }
  return out;
}

void handleRoot() {
  File file = LittleFS.open(F("/index.html"), "r");
  if (file) {
    server.streamFile(file, F("text/html"));
    file.close();
  } else {
    server.send(404, F("text/plain"), F("index.html not found — upload the filesystem image."));
  }
}

void handleOK() { server.send(200); }

// GET /list — device status + settings + image list (streamed to save RAM).
void handleList() {
  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send(200, F("application/json"), "");

  server.sendContent(F("{\"total\":"));      server.sendContent(String(LittleFS.totalBytes()));
  server.sendContent(F(",\"used\":"));       server.sendContent(String(LittleFS.usedBytes()));
  server.sendContent(F(",\"localIP\":\""));  server.sendContent(jsonEscape(WiFi.localIP().toString()));  server.sendContent(F("\""));
  server.sendContent(F(",\"apIP\":\""));     server.sendContent(jsonEscape(WiFi.softAPIP().toString())); server.sendContent(F("\""));
  server.sendContent(F(",\"mdns\":\""));     server.sendContent(jsonEscape(String(hostName) + F(".local"))); server.sendContent(F("\""));
  server.sendContent(F(",\"ssid\":\""));     server.sendContent(jsonEscape(station_ssid)); server.sendContent(F("\""));
  server.sendContent(F(",\"pass_set\":"));   server.sendContent(station_password.length() > 0 ? F("true") : F("false"));
  server.sendContent(F(",\"brightness\":")); server.sendContent(String(matrixBrightness));
  server.sendContent(F(",\"interval\":"));   server.sendContent(String(displayDuration / 1000));
  server.sendContent(F(",\"deviceMode\":")); server.sendContent(String(deviceMode));
  server.sendContent(F(",\"transitionMode\":")); server.sendContent(String(transitionMode));
  server.sendContent(F(",\"wledActive\":")); server.sendContent(wledMode ? F("true") : F("false"));

  uint8_t m = current_2d_effect_id ? current_2d_effect_id : 1;  // avoid reporting 0
  server.sendContent(F(",\"wledMode\":"));       server.sendContent(String(m));
  server.sendContent(F(",\"wledSpeed\":"));      server.sendContent(String(ws2812fx.getSpeed()));
  server.sendContent(F(",\"wledBrightness\":")); server.sendContent(String(ws2812fx.getBrightness()));
  server.sendContent(F(",\"wledColor\":"));      server.sendContent(String(ws2812fx.getColor()));
  server.sendContent(F(",\"files\":["));

  File root = LittleFS.open(F("/"));
  File file = root.openNextFile();
  bool first = true;
  while (file) {
    String name = file.name();
    if (name.endsWith(F(".bin"))) {
      if (!first) server.sendContent(F(","));
      server.sendContent(F("{\"name\":\"")); server.sendContent(jsonEscape(name));
      server.sendContent(F("\",\"size\":"));  server.sendContent(String(file.size()));
      server.sendContent(F("}"));
      first = false;
    }
    file = root.openNextFile();
  }
  server.sendContent(F("]}"));
}

// GET /files — just the image list (used by the gallery).
void handleFiles() {
  String json = "[";
  File root = LittleFS.open(F("/"));
  File file = root.openNextFile();
  bool first = true;
  while (file) {
    String name = file.name();
    if (name.endsWith(F(".bin"))) {
      if (!first) json += ",";
      json += "{\"name\":\"" + jsonEscape(name) + "\",\"size\":" + String(file.size()) + "}";
      first = false;
    }
    file = root.openNextFile();
  }
  json += "]";
  server.send(200, F("application/json"), json);
}

// POST /settings — matrix params and/or WiFi credentials.
void handleSettings() {
  bool changedConfig = false;
  bool shouldRestart = false;

  if (server.hasArg(F("brightness"))) {
    int b = constrain(server.arg(F("brightness")).toInt(), 0, 100);
    matrixBrightness = (uint8_t)b;
    if (dma_display) dma_display->setBrightness8((matrixBrightness * 255) / 100);
    changedConfig = true;
  }

  if (server.hasArg(F("interval"))) {
    int i = server.arg(F("interval")).toInt();
    if (i < 1) i = 1;
    displayDuration = (unsigned long)i * 1000;
    changedConfig = true;
  }

  if (server.hasArg(F("mode"))) {
    int m = server.arg(F("mode")).toInt();
    if (m == MODE_IMAGES || m == MODE_EFFECTS) {
      deviceMode = (uint8_t)m;
      wledMode = (deviceMode == MODE_EFFECTS);
      if (wledMode) ws2812fx.start(); else ws2812fx.stop();
      changedConfig = true;
    }
  }

  if (server.hasArg(F("transitionMode"))) {
    int tm = server.arg(F("transitionMode")).toInt();
    if (tm == TRANS_CROSSFADE || tm == TRANS_PIXEL_DISSOLVE) {
      transitionMode = (uint8_t)tm;
      changedConfig = true;
    }
  }

  // WiFi credentials — persist immediately and reboot to apply.
  if (server.hasArg(F("ssid")) && server.hasArg(F("pass"))) {
    station_ssid     = server.arg(F("ssid"));
    station_password = server.arg(F("pass"));
    File f = LittleFS.open(F("/wifi.txt"), "w");
    if (f) {
      f.println(station_ssid);
      f.println(station_password);
      f.close();
      shouldRestart = true;
    }
  }

  if (changedConfig) saveConfig();

  if (shouldRestart) {
    server.send(200, F("text/plain"), F("Saved. Restarting..."));
    delay(100);
    ESP.restart();
  } else {
    server.send(200, F("text/plain"), F("Saved"));
  }
}

// DELETE /delete?name=... — remove an image.
void handleDelete() {
  if (!server.hasArg(F("name"))) { server.send(400, F("text/plain"), F("Missing name")); return; }
  String path = F("/");
  path += server.arg(F("name"));
  path.replace(F("//"), F("/"));

  if (state32_curr.filename == path) state32_curr.close();
  if (state32_next.filename == path) state32_next.close();

  if (!LittleFS.exists(path))      { server.send(404, F("text/plain"), F("Not found")); return; }
  if (LittleFS.remove(path)) {
    refreshPlaylist();
    server.send(200, F("text/plain"), F("Deleted"));
  } else {
    server.send(500, F("text/plain"), F("Delete failed"));
  }
}

// POST /upload — receive an image .bin into flash.
File uploadFile;
void handleFileUpload() {
  HTTPUpload &upload = server.upload();
  if (upload.status == UPLOAD_FILE_START) {
    String filename = upload.filename;
    if (!filename.startsWith("/")) filename = "/" + filename;
    uploadFile = LittleFS.open(filename, "w");
  } else if (upload.status == UPLOAD_FILE_WRITE) {
    if (uploadFile) uploadFile.write(upload.buf, upload.currentSize);
  } else if (upload.status == UPLOAD_FILE_END) {
    if (uploadFile) { uploadFile.close(); refreshPlaylist(); }
  }
}

// POST /wled — toggle Effects mode and set effect/brightness/color.
void handleWLED() {
  if (server.hasArg(F("active"))) {
    bool active = (server.arg(F("active")) == F("true"));
    if (active) {
      deviceMode = MODE_EFFECTS;
      wledMode = true;
      ws2812fx.start();
    } else {
      if (deviceMode == MODE_EFFECTS) deviceMode = MODE_IMAGES;
      wledMode = false;
      ws2812fx.stop();
      if (dma_display) dma_display->clearScreen();
      if (playlist_count > 0) {           // resume image playback immediately
        state32_curr.currentFrameIndex = -1;
        updateAnimationState(state32_curr, millis());
      }
    }
    saveConfig();
  }

  if (server.hasArg(F("mode")))       set2DEffect(server.arg(F("mode")).toInt());
  if (server.hasArg(F("brightness"))) ws2812fx.setBrightness(server.arg(F("brightness")).toInt());
  if (server.hasArg(F("color"))) {
    const char *p = server.arg(F("color")).c_str();
    if (*p == '#') p++;
    ws2812fx.setColor(strtoul(p, NULL, 16));
  }

  server.send(200, F("application/json"),
              wledMode ? F("{\"status\":\"ok\",\"wled\":true}")
                       : F("{\"status\":\"ok\",\"wled\":false}"));
}

// POST /jump?name=... — jump straight to a specific image.
void handleJump() {
  if (!server.hasArg(F("name"))) { server.send(400, F("text/plain"), F("Missing name")); return; }
  String name = server.arg(F("name"));
  if (!name.startsWith("/")) name = "/" + name;

  if (deviceMode != MODE_IMAGES) {       // leave Effects mode if active
    deviceMode = MODE_IMAGES;
    wledMode = false;
    ws2812fx.stop();
    saveConfig();
  }

  refreshPlaylist();

  state32_curr.close();
  state32_curr.open(name);
  if (state32_curr.frameCount > 0) {
    state32_curr.loadFrame(0);
    state32_curr.lastFrameAdvance = millis();
  }

  if (playlist_count > 1) {              // queue a random different next image
    int nextIdx = random(playlist_count);
    while (playlist[nextIdx] == name) nextIdx = random(playlist_count);
    state32_next.close();
    state32_next.open(playlist[nextIdx]);
    state32_next.lastFrameAdvance = millis();
  }

  lastSwitchTime = millis();
  server.send(200, F("text/plain"), F("ok"));
}

// ===========================================================================
// Rendering helpers
// ===========================================================================
void updateAnimationState(PlaybackState &state, unsigned long now) {
  if (state.frameCount == 0) return;
  if (state.currentFrameIndex == -1) {          // first load
    state.loadFrame(0);
    state.lastFrameAdvance = now;
    return;
  }
  if (state.frameCount > 1 && now - state.lastFrameAdvance > frameDuration) {
    state.loadFrame((state.currentFrameIndex + 1) % state.frameCount);
    state.lastFrameAdvance = now;
  }
}

// Scale an RGB565 color by brightness (0=black, 255=full).
uint16_t dimRGB565(uint16_t color, uint8_t brightness) {
  if (brightness == 255) return color;
  if (brightness == 0)   return 0;
  uint16_t r = (((color >> 11) & 0x1F) * brightness) >> 8;
  uint16_t g = (((color >> 5)  & 0x3F) * brightness) >> 8;
  uint16_t b = ((color & 0x1F) * brightness) >> 8;
  return (r << 11) | (g << 5) | b;
}

// Blend two RGB565 colors (ratio 0=c1, 255=c2), using >>8 instead of /255.
uint16_t blendRGB565(uint16_t c1, uint16_t c2, uint8_t ratio) {
  if (ratio == 0)   return c1;
  if (ratio == 255) return c2;
  uint32_t r = (((c1 >> 11) & 0x1F) * (256 - ratio) + ((c2 >> 11) & 0x1F) * ratio) >> 8;
  uint32_t g = (((c1 >> 5)  & 0x3F) * (256 - ratio) + ((c2 >> 5)  & 0x3F) * ratio) >> 8;
  uint32_t b = (( c1        & 0x1F) * (256 - ratio) + ( c2        & 0x1F) * ratio) >> 8;
  return (r << 11) | (g << 5) | b;
}

// ===========================================================================
// Minimal mDNS responder
// Answers A-record queries for "<hostName>.local" without the full ESPmDNS
// library (~30KB flash saving). Re-announces periodically.
// ===========================================================================
int mdnsBuildAnswer(uint8_t *resp, uint16_t txid) {
  IPAddress ip = WiFi.localIP();
  if (ip == IPAddress(0, 0, 0, 0)) ip = WiFi.softAPIP();

  resp[0] = txid >> 8; resp[1] = txid & 0xFF;
  resp[2] = 0x84; resp[3] = 0x00;   // QR=1, AA=1
  resp[4] = 0; resp[5] = 0;         // QDCOUNT=0
  resp[6] = 0; resp[7] = 1;         // ANCOUNT=1
  resp[8] = 0; resp[9] = 0; resp[10] = 0; resp[11] = 0;
  int rpos = 12;

  const char *parts[] = { hostName, "local" };
  for (int p = 0; p < 2; p++) {
    uint8_t l = strlen(parts[p]);
    resp[rpos++] = l;
    for (uint8_t i = 0; i < l; i++) resp[rpos++] = tolower(parts[p][i]);
  }
  resp[rpos++] = 0;
  resp[rpos++] = 0; resp[rpos++] = 1;      // TYPE A
  resp[rpos++] = 0x80; resp[rpos++] = 1;   // CLASS IN, cache-flush
  resp[rpos++] = 0; resp[rpos++] = 0; resp[rpos++] = 0; resp[rpos++] = 120;  // TTL 120s
  resp[rpos++] = 0; resp[rpos++] = 4;
  resp[rpos++] = ip[0]; resp[rpos++] = ip[1]; resp[rpos++] = ip[2]; resp[rpos++] = ip[3];
  return rpos;
}

void mdnsSend(uint8_t *resp, int len) {
  mdnsUDP.beginMulticastPacket();
  mdnsUDP.write(resp, len);
  mdnsUDP.endPacket();
}

void mdnsBegin() {
  mdnsUDP.beginMulticast(IPAddress(224, 0, 0, 251), 5353);
  delay(100);
  uint8_t resp[128];
  int len = mdnsBuildAnswer(resp, 0);
  mdnsSend(resp, len);
  delay(1000);
  mdnsSend(resp, len);   // announce twice per RFC 6762
}

void mdnsHandle() {
  // Periodic re-announcement keeps us visible (and updates our IP if STA
  // connected after boot).
  static unsigned long lastAnnounce = 0;
  if (millis() - lastAnnounce > 120000) {
    lastAnnounce = millis();
    uint8_t resp[128];
    mdnsSend(resp, mdnsBuildAnswer(resp, 0));
  }

  int len = mdnsUDP.parsePacket();
  if (len < 12) return;
  uint8_t buf[256];
  if (len > (int)sizeof(buf)) { mdnsUDP.flush(); return; }
  mdnsUDP.read(buf, len);

  if (buf[2] & 0x80) return;                     // ignore responses
  uint16_t qdcount = (buf[4] << 8) | buf[5];
  if (qdcount == 0) return;

  // Parse the first question name.
  int pos = 12, qi = 0;
  char qname[64];
  while (pos < len && buf[pos] != 0 && qi < 62) {
    uint8_t lbl = buf[pos++];
    if (lbl > 63) return;                        // no compressed labels
    if (qi > 0) qname[qi++] = '.';
    for (uint8_t i = 0; i < lbl && pos < len && qi < 62; i++)
      qname[qi++] = (char)tolower(buf[pos++]);
  }
  qname[qi] = 0;
  if (pos >= len) return;
  pos++;                                          // skip null terminator

  char expected[48];
  snprintf(expected, sizeof(expected), "%s.local", hostName);
  for (char *p = expected; *p; p++) *p = tolower(*p);

  if (strcmp(qname, expected) != 0) return;
  if (pos + 4 > len) return;
  uint16_t qtype = (buf[pos] << 8) | buf[pos + 1];
  if (qtype != 1 && qtype != 255) return;         // A or ANY only

  uint8_t resp[128];
  mdnsSend(resp, mdnsBuildAnswer(resp, (buf[0] << 8) | buf[1]));
}

// ===========================================================================
// WiFi
// Generates a unique identity from the MAC, hosts an open AP, and optionally
// joins a home network with auto-reconnect. Called once from setup().
// ===========================================================================
void setupWiFi() {
  WiFi.persistent(false);
  WiFi.mode(station_ssid.length() ? WIFI_AP_STA : WIFI_AP);

  // Build unique names from the factory MAC (last 2 bytes -> 4 hex digits).
  // esp_read_mac() reads efuse directly, so it works even in AP-only mode
  // where WiFi.macAddress() (station interface) would return all zeros.
  uint8_t mac[6];
  esp_read_mac(mac, ESP_MAC_WIFI_STA);
  const char h[] = "0123456789ABCDEF";
  snprintf(panelId, sizeof(panelId), "%c%c%c%c",
           h[(mac[4] >> 4) & 0xF], h[mac[4] & 0xF],
           h[(mac[5] >> 4) & 0xF], h[mac[5] & 0xF]);
  snprintf(ap_ssid, sizeof(ap_ssid), "Panel_%s", panelId);
  snprintf(hostName, sizeof(hostName), "led-panel%s", panelId);

  WiFi.softAP(ap_ssid, ap_password);   // always-on direct access point

  // Join home network if configured.
  if (station_ssid.length()) {
    WiFi.setAutoReconnect(true);
    WiFi.begin(station_ssid.c_str(), station_password.c_str());
    for (int i = 0; i < 20 && WiFi.status() != WL_CONNECTED; i++) delay(250);  // ~5s, non-fatal
  }

  WiFi.setSleep(false);                 // keep radio awake for snappy HTTP
  delay(200);                           // let interfaces settle before mDNS binds
  mdnsBegin();
}

// Nudge a dropped station link back up (auto-reconnect usually handles it).
void wifiHandle() {
  static unsigned long lastCheck = 0;
  if (station_ssid.length() == 0) return;
  if (millis() - lastCheck < 30000) return;
  lastCheck = millis();
  if (WiFi.status() != WL_CONNECTED) WiFi.reconnect();
}

// ===========================================================================
// Setup
// ===========================================================================
void setup() {
  Serial.begin(115200);
  delay(100);

  if (!LittleFS.begin(true)) return;
  loadConfig();
  loadWifiConfig();
  refreshPlaylist();

  setupWiFi();

  // Routes.
  server.on("/",         HTTP_GET,    handleRoot);
  server.on("/list",     HTTP_GET,    handleList);
  server.on("/files",    HTTP_GET,    handleFiles);
  server.on("/settings", HTTP_POST,   handleSettings);
  server.on("/delete",   HTTP_DELETE, handleDelete);
  server.on("/wled",     HTTP_POST,   handleWLED);
  server.on("/jump",     HTTP_POST,   handleJump);
  server.on("/upload",   HTTP_POST,   handleOK, handleFileUpload);
  server.serveStatic("/", LittleFS, "/");   // serve .bin thumbnails etc.
  server.begin();

  // Matrix init.
  HUB75_I2S_CFG mxconfig(PANEL_RES_X, PANEL_RES_Y, PANEL_CHAIN);
  mxconfig.gpio.r1 = R1_PIN; mxconfig.gpio.g1 = G1_PIN; mxconfig.gpio.b1 = B1_PIN;
  mxconfig.gpio.r2 = R2_PIN; mxconfig.gpio.g2 = G2_PIN; mxconfig.gpio.b2 = B2_PIN;
  mxconfig.gpio.a = A_PIN; mxconfig.gpio.b = B_PIN; mxconfig.gpio.c = C_PIN;
  mxconfig.gpio.d = D_PIN; mxconfig.gpio.e = E_PIN;
  mxconfig.gpio.lat = LAT_PIN; mxconfig.gpio.oe = OE_PIN; mxconfig.gpio.clk = CLK_PIN;

  dma_display = new MatrixPanel_I2S_DMA(mxconfig);
  dma_display->begin();
  dma_display->setBrightness8((matrixBrightness * 255) / 100);
  dma_display->clearScreen();

  // Effects engine init.
  ws2812fx.init();
  ws2812fx.setBrightness(128);
  ws2812fx.setColor(0xFFAA00);
  setupCustomEffects();
  set2DEffect(1);                          // default effect
  ws2812fx.setCustomShow(myCustomShow);
  wledMode = (deviceMode == MODE_EFFECTS);
  if (wledMode) ws2812fx.start(); else ws2812fx.stop();

  shufflePixelOrder();

  // Prime image playback with a random pick.
  if (playlist_count > 0) {
    int idx = random(playlist_count);
    state32_curr.open(playlist[idx]);
    state32_curr.lastFrameAdvance = millis();
    state32_next.open(playlist[idx]);
    state32_next.lastFrameAdvance = millis();
  }
}

// ===========================================================================
// Loop
// ===========================================================================
void loop() {
  static bool placeholderDrawn = false;

  server.handleClient();
  mdnsHandle();
  wifiHandle();

  // --- Effects mode ---
  if (deviceMode == MODE_EFFECTS) {
    placeholderDrawn = false;
    ws2812fx.service();
    return;
  }

  // --- Images mode: empty state ---
  // Nothing uploaded yet: show which WiFi network to join (Panel_XXXX).
  if (playlist_count == 0) {
    if (!placeholderDrawn) {
      dma_display->fillScreen(dma_display->color565(0, 0, 0));
      drawTextCentered(7,  "WIFI",  dma_display->color565(255, 255, 255));
      drawTextCentered(18, panelId, dma_display->color565(58, 122, 191));  // steel blue
      placeholderDrawn = true;
    }
    return;
  }
  placeholderDrawn = false;

  unsigned long now = millis();
  unsigned long timeSinceSwitch = now - lastSwitchTime;

  updateAnimationState(state32_curr, now);
  updateAnimationState(state32_next, now);

  // --- Transition state machine ---
  uint8_t blendRatio   = 0;   // crossfade: 0=curr, 255=next
  uint8_t transPhase   = 3;   // dissolve: 0=out, 1=black, 2=in, 3=hold
  int     transProgress = 0;

  if (transitionMode == TRANS_CROSSFADE) {
    if (timeSinceSwitch < crossfadeDuration) {
      blendRatio = (timeSinceSwitch * 255) / crossfadeDuration;
    } else if (timeSinceSwitch < displayDuration + crossfadeDuration) {
      blendRatio = 255;
    } else {
      advanceToNextImage(now);
      blendRatio = 0;
    }
  } else {
    // Pixel dissolve: fade out -> brief black -> fade in -> hold.
    const unsigned long blackPause  = 3000;
    unsigned long        dissolveTime = (dissolveDuration - blackPause) / 2;

    if (timeSinceSwitch < dissolveTime) {
      transPhase = 0;
      transProgress = min((int)((timeSinceSwitch * 1024UL) / dissolveTime), 1024);
    } else if (timeSinceSwitch < dissolveTime + blackPause) {
      transPhase = 1;
      if (!phase2Shuffled) { shufflePixelOrder(); phase2Shuffled = true; }
    } else if (timeSinceSwitch < dissolveDuration) {
      transPhase = 2;
      unsigned long resolveElapsed = timeSinceSwitch - dissolveTime - blackPause;
      transProgress = min((int)((resolveElapsed * 1024UL) / dissolveTime), 1024);
    } else if (timeSinceSwitch < displayDuration + dissolveDuration) {
      transPhase = 3;
    } else {
      advanceToNextImage(now);
      shufflePixelOrder();
      phase2Shuffled = false;
      transPhase = 0;
      transProgress = 0;
    }
  }

  // --- Render 32x32 ---
  for (int y = 0; y < 32; y++) {
    for (int x = 0; x < 32; x++) {
      int idx = y * 32 + x;
      uint16_t color;

      if (transitionMode == TRANS_CROSSFADE) {
        uint16_t c1 = state32_curr.frameData[idx];
        uint16_t c2 = state32_next.frameData[idx];
        color = (blendRatio == 0) ? c1 : (blendRatio == 255 ? c2 : blendRGB565(c1, c2, blendRatio));
      } else if (transPhase == 0 || transPhase == 2) {
        int startTime = (pixelRank[idx] * (1024 - FADE_WINDOW)) / 1023;
        int fade = constrain(((transProgress - startTime) * 255) / FADE_WINDOW, 0, 255);
        color = (transPhase == 0) ? dimRGB565(state32_curr.frameData[idx], 255 - fade)
                                  : dimRGB565(state32_next.frameData[idx], fade);
      } else if (transPhase == 1) {
        color = 0;
      } else {
        color = state32_next.frameData[idx];
      }

      dma_display->drawPixel(x, y, color);
    }
    if ((y & 7) == 7) yield();
  }
}

// Promote "next" image to "current" and pick a new random "next".
void advanceToNextImage(unsigned long now) {
  memcpy(state32_curr.frameData, state32_next.frameData, sizeof(state32_curr.frameData));
  int nextFrameIdx = state32_next.currentFrameIndex;
  state32_curr.close();
  state32_curr.open(state32_next.filename);
  state32_curr.currentFrameIndex = nextFrameIdx;
  state32_curr.lastFrameAdvance = now;

  int nextIdx = random(playlist_count);
  if (playlist_count > 1) {
    while (playlist[nextIdx] == state32_curr.filename) nextIdx = random(playlist_count);
  }
  state32_next.close();
  state32_next.open(playlist[nextIdx]);
  state32_next.lastFrameAdvance = now;
  lastSwitchTime = now;
}

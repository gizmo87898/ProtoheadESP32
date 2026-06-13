#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>

#define FASTLED_INTERNAL
#include <FastLED.h>

// ================= USER CONFIG =================

#define DATA_PIN 13                 // WS2812 data pin
#define COLOR_ORDER GRB
#define LED_TYPE WS2812B

#define MATRIX_W 8
#define MATRIX_H 8
#define LEDS_PER_MATRIX 64
#define NUM_MATRICES 11
#define TOTAL_LEDS (NUM_MATRICES * LEDS_PER_MATRIX)

// Most 8x8 WS2812 panels are wired serpentine.
// If your image looks wrong, change this to false.
#define SERPENTINE_MATRIX true

// Set this to match your power supply.
// 704 LEDs at full white can draw a lot; do not power LEDs from the ESP32.
#define MAX_POWER_MILLIAMPS 10000

const char *AP_SSID = "esp32";
const char *AP_PASS = "protohead123"; // must be 8+ chars, or use "" for open AP

const uint16_t WEB_PORT = 8080;

// =================================================

CRGB leds[TOTAL_LEDS];

enum LedMode : uint8_t {
  MODE_OFF = 0,
  MODE_SOLID,
  MODE_RAINBOW,
  MODE_CHECKER,
  MODE_WIPE,
  MODE_CUSTOM
};

struct MatrixState {
  LedMode mode;
  CRGB color;
  uint8_t brightness;  // 0-255
  uint16_t speedMs;    // animation delay
  bool dirty;
};

MatrixState matrixState[NUM_MATRICES];
CRGB customPixels[NUM_MATRICES][LEDS_PER_MATRIX];

uint32_t lastDrawMs[NUM_MATRICES];
uint16_t frameCounter[NUM_MATRICES];

WebServer server(WEB_PORT);

// ================= WEB UI =================

const char INDEX_HTML[] PROGMEM = R"HTML(
<!doctype html>
<html>
<head>
  <meta charset="utf-8">
  <title>ESP32 LED Matrix Controller</title>
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <style>
    body {
      font-family: Arial, sans-serif;
      background: #111;
      color: #eee;
      margin: 0;
      padding: 18px;
    }
    h1 { margin-top: 0; }
    .top, .card {
      background: #1b1b1b;
      border: 1px solid #333;
      border-radius: 12px;
      padding: 14px;
      margin-bottom: 14px;
    }
    .grid {
      display: grid;
      grid-template-columns: repeat(auto-fit, minmax(245px, 1fr));
      gap: 14px;
    }
    label {
      display: block;
      margin-top: 8px;
      font-size: 14px;
      color: #ccc;
    }
    input, select, button {
      width: 100%;
      box-sizing: border-box;
      margin-top: 4px;
      padding: 8px;
      border-radius: 8px;
      border: 1px solid #444;
      background: #222;
      color: #eee;
    }
    input[type=color] {
      height: 42px;
      padding: 3px;
    }
    button {
      cursor: pointer;
      background: #2c5aff;
      border: none;
      margin-top: 10px;
      font-weight: bold;
    }
    button.secondary {
      background: #444;
    }
    .pixels {
      display: grid;
      grid-template-columns: repeat(8, 20px);
      grid-template-rows: repeat(8, 20px);
      gap: 3px;
      margin-top: 10px;
      user-select: none;
    }
    .px {
      width: 20px;
      height: 20px;
      background: #050505;
      border: 1px solid #333;
      border-radius: 4px;
      cursor: pointer;
    }
    .hint {
      color: #aaa;
      font-size: 13px;
      line-height: 1.4;
    }
  </style>
</head>
<body>
  <h1>ESP32 11× 8x8 WS2812 Matrix Controller</h1>

  <div class="top">
    <div class="hint">
      Connect to WiFi <b>ESP32-Matrix-Control</b>, then open
      <b>http://192.168.4.1:8080</b>
    </div>

    <h3>Apply to all matrices</h3>

    <label>Mode</label>
    <select id="allMode">
      <option value="off">Off</option>
      <option value="solid">Solid</option>
      <option value="rainbow">Rainbow</option>
      <option value="checker">Checker</option>
      <option value="wipe">Wipe</option>
      <option value="custom">Custom / Paint</option>
    </select>

    <label>Color</label>
    <input id="allColor" type="color" value="#ff0000">

    <label>Brightness</label>
    <input id="allBrightness" type="range" min="0" max="255" value="96">

    <label>Animation speed, ms</label>
    <input id="allSpeed" type="range" min="10" max="800" value="80">

    <button onclick="applyAll()">Apply to all</button>
    <button class="secondary" onclick="clearAll()">Clear custom pixels</button>
  </div>

  <div id="cards" class="grid"></div>

<script>
let state = null;

function post(path, data) {
  return fetch(path, {
    method: "POST",
    headers: {"Content-Type": "application/x-www-form-urlencoded"},
    body: new URLSearchParams(data)
  });
}

function makePixelGrid(matrixIndex) {
  let html = '<div class="pixels">';
  for (let y = 0; y < 8; y++) {
    for (let x = 0; x < 8; x++) {
      html += `<div class="px" onclick="paintPixel(${matrixIndex},${x},${y})"></div>`;
    }
  }
  html += '</div>';
  return html;
}

function makeCard(i, s) {
  return `
    <div class="card">
      <h3>Matrix ${i + 1}</h3>

      <label>Mode</label>
      <select id="mode${i}">
        <option value="off">Off</option>
        <option value="solid">Solid</option>
        <option value="rainbow">Rainbow</option>
        <option value="checker">Checker</option>
        <option value="wipe">Wipe</option>
        <option value="custom">Custom / Paint</option>
      </select>

      <label>Color</label>
      <input id="color${i}" type="color" value="${s.color}">

      <label>Brightness</label>
      <input id="brightness${i}" type="range" min="0" max="255" value="${s.brightness}">

      <label>Animation speed, ms</label>
      <input id="speed${i}" type="range" min="10" max="800" value="${s.speed}">

      <button onclick="applyMatrix(${i})">Apply</button>

      <div class="hint">
        Pixel painter uses this matrix's color. Clicking a pixel switches that matrix to custom mode.
      </div>
      ${makePixelGrid(i)}
    </div>
  `;
}

async function loadState() {
  const res = await fetch("/state");
  state = await res.json();

  const cards = document.getElementById("cards");
  cards.innerHTML = "";

  for (let i = 0; i < state.matrices.length; i++) {
    cards.innerHTML += makeCard(i, state.matrices[i]);
  }

  for (let i = 0; i < state.matrices.length; i++) {
    document.getElementById("mode" + i).value = state.matrices[i].mode;
  }
}

async function applyMatrix(i) {
  await post("/set", {
    m: i,
    mode: document.getElementById("mode" + i).value,
    color: document.getElementById("color" + i).value,
    brightness: document.getElementById("brightness" + i).value,
    speed: document.getElementById("speed" + i).value
  });
  await loadState();
}

async function applyAll() {
  await post("/set", {
    m: "all",
    mode: document.getElementById("allMode").value,
    color: document.getElementById("allColor").value,
    brightness: document.getElementById("allBrightness").value,
    speed: document.getElementById("allSpeed").value
  });
  await loadState();
}

async function clearAll() {
  await post("/clear", {});
  await loadState();
}

async function paintPixel(m, x, y) {
  const color = document.getElementById("color" + m).value;
  await post("/pixel", {m, x, y, color});
  document.getElementById("mode" + m).value = "custom";
}

loadState();
</script>
</body>
</html>
)HTML";

// ================= HELPERS =================

uint16_t xyToIndex(uint8_t x, uint8_t y) {
  if (x >= MATRIX_W || y >= MATRIX_H) return 0;

#if SERPENTINE_MATRIX
  if (y & 1) {
    return y * MATRIX_W + (MATRIX_W - 1 - x);
  } else {
    return y * MATRIX_W + x;
  }
#else
  return y * MATRIX_W + x;
#endif
}

uint16_t matrixLedOffset(uint8_t matrix, uint8_t localIndex) {
  return matrix * LEDS_PER_MATRIX + localIndex;
}

CRGB applyBrightness(CRGB c, uint8_t brightness) {
  c.nscale8_video(brightness);
  return c;
}

CRGB parseHexColor(String s) {
  s.trim();
  if (s.startsWith("#")) s.remove(0, 1);

  if (s.length() != 6) {
    return CRGB::White;
  }

  uint32_t value = strtoul(s.c_str(), nullptr, 16);
  return CRGB(
    (value >> 16) & 0xFF,
    (value >> 8) & 0xFF,
    value & 0xFF
  );
}

String colorToHex(CRGB c) {
  char buf[8];
  snprintf(buf, sizeof(buf), "#%02X%02X%02X", c.r, c.g, c.b);
  return String(buf);
}

const char *modeToString(LedMode mode) {
  switch (mode) {
    case MODE_OFF: return "off";
    case MODE_SOLID: return "solid";
    case MODE_RAINBOW: return "rainbow";
    case MODE_CHECKER: return "checker";
    case MODE_WIPE: return "wipe";
    case MODE_CUSTOM: return "custom";
    default: return "off";
  }
}

LedMode stringToMode(String s) {
  s.toLowerCase();

  if (s == "solid") return MODE_SOLID;
  if (s == "rainbow") return MODE_RAINBOW;
  if (s == "checker") return MODE_CHECKER;
  if (s == "wipe") return MODE_WIPE;
  if (s == "custom") return MODE_CUSTOM;

  return MODE_OFF;
}

void setMatrixPixel(uint8_t matrix, uint8_t x, uint8_t y, CRGB color) {
  if (matrix >= NUM_MATRICES || x >= MATRIX_W || y >= MATRIX_H) return;

  uint16_t localIndex = xyToIndex(x, y);
  uint16_t globalIndex = matrixLedOffset(matrix, localIndex);

  leds[globalIndex] = applyBrightness(color, matrixState[matrix].brightness);
}

void fillMatrix(uint8_t matrix, CRGB color) {
  if (matrix >= NUM_MATRICES) return;

  CRGB out = applyBrightness(color, matrixState[matrix].brightness);

  for (uint8_t i = 0; i < LEDS_PER_MATRIX; i++) {
    leds[matrixLedOffset(matrix, i)] = out;
  }
}

void renderMatrix(uint8_t matrix) {
  if (matrix >= NUM_MATRICES) return;

  MatrixState &s = matrixState[matrix];

  switch (s.mode) {
    case MODE_OFF:
      fillMatrix(matrix, CRGB::Black);
      break;

    case MODE_SOLID:
      fillMatrix(matrix, s.color);
      break;

    case MODE_RAINBOW: {
      for (uint8_t y = 0; y < MATRIX_H; y++) {
        for (uint8_t x = 0; x < MATRIX_W; x++) {
          uint8_t hue = frameCounter[matrix] * 3 + x * 12 + y * 8 + matrix * 18;
          CRGB c;
          hsv2rgb_rainbow(CHSV(hue, 255, 255), c);
          setMatrixPixel(matrix, x, y, c);
        }
      }
      break;
    }

    case MODE_CHECKER: {
      for (uint8_t y = 0; y < MATRIX_H; y++) {
        for (uint8_t x = 0; x < MATRIX_W; x++) {
          bool on = ((x + y + frameCounter[matrix]) & 1) == 0;
          setMatrixPixel(matrix, x, y, on ? s.color : CRGB::Black);
        }
      }
      break;
    }

    case MODE_WIPE: {
      uint8_t count = frameCounter[matrix] % (LEDS_PER_MATRIX + 1);

      for (uint8_t i = 0; i < LEDS_PER_MATRIX; i++) {
        leds[matrixLedOffset(matrix, i)] =
          i < count ? applyBrightness(s.color, s.brightness) : CRGB::Black;
      }
      break;
    }

    case MODE_CUSTOM: {
      for (uint8_t i = 0; i < LEDS_PER_MATRIX; i++) {
        leds[matrixLedOffset(matrix, i)] =
          applyBrightness(customPixels[matrix][i], s.brightness);
      }
      break;
    }
  }
}

void updateAnimations() {
  uint32_t now = millis();
  bool needShow = false;

  for (uint8_t m = 0; m < NUM_MATRICES; m++) {
    bool animated =
      matrixState[m].mode == MODE_RAINBOW ||
      matrixState[m].mode == MODE_CHECKER ||
      matrixState[m].mode == MODE_WIPE;

    if (matrixState[m].dirty || (animated && now - lastDrawMs[m] >= matrixState[m].speedMs)) {
      if (animated) {
        frameCounter[m]++;
      }

      renderMatrix(m);
      matrixState[m].dirty = false;
      lastDrawMs[m] = now;
      needShow = true;
    }
  }

  if (needShow) {
    FastLED.show();
  }
}

// ================= HTTP HANDLERS =================

void handleRoot() {
  server.send_P(200, "text/html", INDEX_HTML);
}

void handleState() {
  String json;
  json.reserve(1600);

  json += "{\"count\":";
  json += NUM_MATRICES;
  json += ",\"matrices\":[";

  for (uint8_t i = 0; i < NUM_MATRICES; i++) {
    if (i > 0) json += ",";

    json += "{";
    json += "\"mode\":\"";
    json += modeToString(matrixState[i].mode);
    json += "\",\"color\":\"";
    json += colorToHex(matrixState[i].color);
    json += "\",\"brightness\":";
    json += matrixState[i].brightness;
    json += ",\"speed\":";
    json += matrixState[i].speedMs;
    json += "}";
  }

  json += "]}";

  server.send(200, "application/json", json);
}

void applyArgsToMatrix(uint8_t m) {
  if (m >= NUM_MATRICES) return;

  if (server.hasArg("mode")) {
    matrixState[m].mode = stringToMode(server.arg("mode"));
  }

  if (server.hasArg("color")) {
    matrixState[m].color = parseHexColor(server.arg("color"));
  }

  if (server.hasArg("brightness")) {
    int b = server.arg("brightness").toInt();
    matrixState[m].brightness = constrain(b, 0, 255);
  }

  if (server.hasArg("speed")) {
    int spd = server.arg("speed").toInt();
    matrixState[m].speedMs = constrain(spd, 10, 2000);
  }

  matrixState[m].dirty = true;
}

void handleSet() {
  if (!server.hasArg("m")) {
    server.send(400, "text/plain", "Missing m");
    return;
  }

  String target = server.arg("m");

  if (target == "all") {
    for (uint8_t m = 0; m < NUM_MATRICES; m++) {
      applyArgsToMatrix(m);
    }
  } else {
    int m = target.toInt();
    if (m < 0 || m >= NUM_MATRICES) {
      server.send(400, "text/plain", "Bad matrix number");
      return;
    }

    applyArgsToMatrix((uint8_t)m);
  }

  updateAnimations();
  server.send(200, "text/plain", "OK");
}

void handlePixel() {
  if (!server.hasArg("m") || !server.hasArg("x") || !server.hasArg("y") || !server.hasArg("color")) {
    server.send(400, "text/plain", "Missing m, x, y, or color");
    return;
  }

  int m = server.arg("m").toInt();
  int x = server.arg("x").toInt();
  int y = server.arg("y").toInt();

  if (m < 0 || m >= NUM_MATRICES || x < 0 || x >= MATRIX_W || y < 0 || y >= MATRIX_H) {
    server.send(400, "text/plain", "Bad pixel coordinate");
    return;
  }

  CRGB c = parseHexColor(server.arg("color"));
  uint16_t localIndex = xyToIndex((uint8_t)x, (uint8_t)y);

  customPixels[m][localIndex] = c;
  matrixState[m].mode = MODE_CUSTOM;
  matrixState[m].dirty = true;

  updateAnimations();
  server.send(200, "text/plain", "OK");
}

void handleClear() {
  for (uint8_t m = 0; m < NUM_MATRICES; m++) {
    for (uint8_t i = 0; i < LEDS_PER_MATRIX; i++) {
      customPixels[m][i] = CRGB::Black;
    }

    matrixState[m].dirty = true;
  }

  updateAnimations();
  server.send(200, "text/plain", "OK");
}

void handleNotFound() {
  server.send(404, "text/plain", "Not found");
}

// ================= SETUP / LOOP =================

void setup() {
  Serial.begin(115200);
  delay(300);

  Serial.println();
  Serial.println("Booting ESP32 LED matrix controller...");

  for (uint8_t m = 0; m < NUM_MATRICES; m++) {
    matrixState[m].mode = MODE_OFF;
    matrixState[m].color = CRGB::Red;
    matrixState[m].brightness = 96;
    matrixState[m].speedMs = 80;
    matrixState[m].dirty = true;

    lastDrawMs[m] = 0;
    frameCounter[m] = 0;

    for (uint8_t i = 0; i < LEDS_PER_MATRIX; i++) {
      customPixels[m][i] = CRGB::Black;
    }
  }

  FastLED.addLeds<LED_TYPE, DATA_PIN, COLOR_ORDER>(leds, TOTAL_LEDS);
  FastLED.setCorrection(TypicalLEDStrip);
  FastLED.setMaxPowerInVoltsAndMilliamps(5, MAX_POWER_MILLIAMPS);
  FastLED.clear(true);

  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_SSID, AP_PASS);

  IPAddress ip = WiFi.softAPIP();

  Serial.print("WiFi AP SSID: ");
  Serial.println(AP_SSID);
  Serial.print("Web UI: http://");
  Serial.print(ip);
  Serial.print(":");
  Serial.println(WEB_PORT);

  server.on("/", HTTP_GET, handleRoot);
  server.on("/state", HTTP_GET, handleState);
  server.on("/set", HTTP_POST, handleSet);
  server.on("/pixel", HTTP_POST, handlePixel);
  server.on("/clear", HTTP_POST, handleClear);
  server.onNotFound(handleNotFound);

  server.begin();
  Serial.println("HTTP server started on port 8080.");

  updateAnimations();
}

void loop() {
  server.handleClient();
  updateAnimations();
}
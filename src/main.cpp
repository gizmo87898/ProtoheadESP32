#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <LittleFS.h>
#include <ArduinoJson.h>

// =====================================================
// HARDWARE CONFIG
// =====================================================

#define MAX_DIN_PIN 13
#define MAX_CLK_PIN 14
#define MAX_CS_PIN  15

#define NUM_MODULES 14
#define ROWS_PER_MODULE 8
#define BYTES_PER_FRAME (NUM_MODULES * ROWS_PER_MODULE)

#define WEB_PORT 8080

// If display is mirrored/flipped, change these first.
#define MODULE_MIRROR_X false
#define MODULE_MIRROR_Y false

// MAX7219 intensity: 0-15
uint8_t globalIntensity = 3;

// WiFi AP
const char *AP_SSID = "esp32";
const char *AP_PASS = "protogen123"; // 8+ chars

// Animation limits
#define MAX_FRAMES 80
#define MAX_NAME_LEN 31

// =====================================================
// MODULE ORDER
// =====================================================
//
// Daisy-chain order:
//
// 0  = Left mouth 1
// 1  = Left mouth 2
// 2  = Left mouth 3
//
// 3  = Right mouth 1
// 4  = Right mouth 2
// 5  = Right mouth 3
//
// 6  = Left eye 1
// 7  = Left eye 2
//
// 8  = Right eye 1
// 9  = Right eye 2
//
// 10 = Left ear
// 11 = Right ear
//
// 12 = Left nose
// 13 = Right nose
//
// Wiring:
// ESP32 GPIO13 -> DIN module 0
// ESP32 GPIO14 -> CLK all modules
// ESP32 GPIO15 -> CS/LOAD all modules
// Module 0 DOUT -> Module 1 DIN -> ... -> Module 13 DIN
//
// =====================================================

// =====================================================
// MAX7219 REGISTERS
// =====================================================

#define MAX_REG_NOOP        0x00
#define MAX_REG_DIGIT0      0x01
#define MAX_REG_DECODEMODE  0x09
#define MAX_REG_INTENSITY   0x0A
#define MAX_REG_SCANLIMIT   0x0B
#define MAX_REG_SHUTDOWN    0x0C
#define MAX_REG_DISPLAYTEST 0x0F

// =====================================================
// GLOBAL STATE
// =====================================================

WebServer server(WEB_PORT);

uint8_t displayFrame[BYTES_PER_FRAME];

uint8_t animFrames[MAX_FRAMES][BYTES_PER_FRAME];
uint16_t animFrameCount = 0;
uint16_t animFrameMs = 125;
bool animLoop = true;
bool animPlaying = false;
uint16_t animIndex = 0;
uint32_t lastAnimMs = 0;
String currentAnimName = "";

String defaultAnimName = "";

// =====================================================
// BASIC HELPERS
// =====================================================

uint8_t reverseByte(uint8_t b) {
  b = (b & 0xF0) >> 4 | (b & 0x0F) << 4;
  b = (b & 0xCC) >> 2 | (b & 0x33) << 2;
  b = (b & 0xAA) >> 1 | (b & 0x55) << 1;
  return b;
}

bool isSafeName(const String &s) {
  if (s.length() == 0 || s.length() > MAX_NAME_LEN) return false;

  for (uint16_t i = 0; i < s.length(); i++) {
    char c = s[i];
    bool ok =
      (c >= 'a' && c <= 'z') ||
      (c >= 'A' && c <= 'Z') ||
      (c >= '0' && c <= '9') ||
      c == '_' ||
      c == '-';

    if (!ok) return false;
  }

  return true;
}

String animPath(const String &name) {
  return "/anim_" + name + ".json";
}

int hexVal(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return 10 + c - 'a';
  if (c >= 'A' && c <= 'F') return 10 + c - 'A';
  return -1;
}

bool hexToFrame(const String &hex, uint8_t *out) {
  if (hex.length() != BYTES_PER_FRAME * 2) {
    return false;
  }

  for (uint16_t i = 0; i < BYTES_PER_FRAME; i++) {
    int hi = hexVal(hex[i * 2]);
    int lo = hexVal(hex[i * 2 + 1]);

    if (hi < 0 || lo < 0) return false;

    out[i] = (hi << 4) | lo;
  }

  return true;
}

String frameToHex(const uint8_t *frame) {
  const char *digits = "0123456789ABCDEF";
  String out;
  out.reserve(BYTES_PER_FRAME * 2);

  for (uint16_t i = 0; i < BYTES_PER_FRAME; i++) {
    out += digits[(frame[i] >> 4) & 0x0F];
    out += digits[frame[i] & 0x0F];
  }

  return out;
}

// =====================================================
// MAX7219 LOW LEVEL
// =====================================================

void maxClockBit(bool bitVal) {
  digitalWrite(MAX_CLK_PIN, LOW);
  digitalWrite(MAX_DIN_PIN, bitVal ? HIGH : LOW);
  digitalWrite(MAX_CLK_PIN, HIGH);
}

void maxSend16(uint8_t reg, uint8_t data) {
  uint16_t packet = ((uint16_t)reg << 8) | data;

  for (int8_t i = 15; i >= 0; i--) {
    maxClockBit(packet & (1 << i));
  }
}

void maxSendAll(uint8_t reg, uint8_t data) {
  digitalWrite(MAX_CS_PIN, LOW);

  for (int8_t m = NUM_MODULES - 1; m >= 0; m--) {
    maxSend16(reg, data);
  }

  digitalWrite(MAX_CS_PIN, HIGH);
}

void maxClear() {
  memset(displayFrame, 0, sizeof(displayFrame));

  for (uint8_t row = 0; row < 8; row++) {
    digitalWrite(MAX_CS_PIN, LOW);

    for (int8_t m = NUM_MODULES - 1; m >= 0; m--) {
      maxSend16(MAX_REG_DIGIT0 + row, 0x00);
    }

    digitalWrite(MAX_CS_PIN, HIGH);
  }
}

void maxShowFrame(const uint8_t *frame) {
  for (uint8_t hwRow = 0; hwRow < 8; hwRow++) {
    digitalWrite(MAX_CS_PIN, LOW);

    for (int8_t m = NUM_MODULES - 1; m >= 0; m--) {
      uint8_t logicalRow = MODULE_MIRROR_Y ? (7 - hwRow) : hwRow;
      uint8_t data = frame[m * 8 + logicalRow];

      if (MODULE_MIRROR_X) {
        data = reverseByte(data);
      }

      maxSend16(MAX_REG_DIGIT0 + hwRow, data);
    }

    digitalWrite(MAX_CS_PIN, HIGH);
  }
}

void maxInit() {
  pinMode(MAX_DIN_PIN, OUTPUT);
  pinMode(MAX_CLK_PIN, OUTPUT);
  pinMode(MAX_CS_PIN, OUTPUT);

  digitalWrite(MAX_CS_PIN, HIGH);
  digitalWrite(MAX_CLK_PIN, LOW);

  delay(100);

  maxSendAll(MAX_REG_SHUTDOWN, 0x00);
  maxSendAll(MAX_REG_DISPLAYTEST, 0x00);
  maxSendAll(MAX_REG_DECODEMODE, 0x00);
  maxSendAll(MAX_REG_SCANLIMIT, 0x07);
  maxSendAll(MAX_REG_INTENSITY, globalIntensity);
  maxSendAll(MAX_REG_SHUTDOWN, 0x01);

  maxClear();
}

// =====================================================
// FILE STORAGE
// =====================================================

void loadDefaultName() {
  defaultAnimName = "";

  if (!LittleFS.exists("/default.txt")) {
    return;
  }

  File f = LittleFS.open("/default.txt", "r");
  if (!f) return;

  defaultAnimName = f.readString();
  defaultAnimName.trim();

  f.close();

  if (!isSafeName(defaultAnimName)) {
    defaultAnimName = "";
  }
}

void saveDefaultName(const String &name) {
  File f = LittleFS.open("/default.txt", "w");
  if (!f) return;

  f.print(name);
  f.close();

  defaultAnimName = name;
}

bool loadAnimation(const String &name) {
  if (!isSafeName(name)) return false;

  String path = animPath(name);

  if (!LittleFS.exists(path)) {
    return false;
  }

  File f = LittleFS.open(path, "r");
  if (!f) return false;

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, f);
  f.close();

  if (err) {
    Serial.print("JSON load error: ");
    Serial.println(err.c_str());
    return false;
  }

  int fps = doc["fps"] | 8;
  fps = constrain(fps, 1, 60);

  bool loopVal = doc["loop"] | true;

  JsonArray frames = doc["frames"].as<JsonArray>();
  if (frames.isNull() || frames.size() == 0 || frames.size() > MAX_FRAMES) {
    return false;
  }

  uint16_t count = 0;

  for (JsonVariant v : frames) {
    const char *hex = v.as<const char *>();
    if (!hex) return false;

    if (!hexToFrame(String(hex), animFrames[count])) {
      return false;
    }

    count++;
  }

  animFrameCount = count;
  animFrameMs = 1000 / fps;
  animLoop = loopVal;
  animIndex = 0;
  currentAnimName = name;

  memcpy(displayFrame, animFrames[0], BYTES_PER_FRAME);
  maxShowFrame(displayFrame);

  return true;
}

// =====================================================
// HTTP HELPERS
// =====================================================

void sendText(int code, const String &msg) {
  server.send(code, "text/plain", msg);
}

void handleApiList() {
  String json = "{";
  json += "\"animations\":[";

  bool first = true;

  File root = LittleFS.open("/");
  File file = root.openNextFile();

  while (file) {
    String fname = String(file.name());

    if (fname.startsWith("/")) {
      fname.remove(0, 1);
    }

    if (fname.startsWith("anim_") && fname.endsWith(".json")) {
      String name = fname.substring(5, fname.length() - 5);

      if (!first) json += ",";
      first = false;

      json += "\"";
      json += name;
      json += "\"";
    }

    file = root.openNextFile();
  }

  json += "],";
  json += "\"default\":\"" + defaultAnimName + "\",";
  json += "\"current\":\"" + currentAnimName + "\",";
  json += "\"playing\":";
  json += animPlaying ? "true" : "false";
  json += ",";
  json += "\"intensity\":";
  json += globalIntensity;
  json += "}";

  server.send(200, "application/json", json);
}

void handleApiLoad() {
  if (!server.hasArg("name")) {
    sendText(400, "Missing name");
    return;
  }

  String name = server.arg("name");

  if (!isSafeName(name)) {
    sendText(400, "Bad name. Use only letters, numbers, underscore, or dash.");
    return;
  }

  String path = animPath(name);

  if (!LittleFS.exists(path)) {
    sendText(404, "Animation not found");
    return;
  }

  File f = LittleFS.open(path, "r");
  if (!f) {
    sendText(500, "Could not open file");
    return;
  }

  server.streamFile(f, "application/json");
  f.close();
}

void handleApiSave() {
  String body = server.arg("plain");

  if (body.length() == 0) {
    sendText(400, "Empty body");
    return;
  }

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, body);

  if (err) {
    sendText(400, String("Bad JSON: ") + err.c_str());
    return;
  }

  String name = doc["name"] | "";

  if (!isSafeName(name)) {
    sendText(400, "Bad name. Use only letters, numbers, underscore, or dash.");
    return;
  }

  int fps = doc["fps"] | 8;
  fps = constrain(fps, 1, 60);

  JsonArray frames = doc["frames"].as<JsonArray>();

  if (frames.isNull() || frames.size() == 0) {
    sendText(400, "No frames");
    return;
  }

  if (frames.size() > MAX_FRAMES) {
    sendText(400, "Too many frames");
    return;
  }

  uint8_t testFrame[BYTES_PER_FRAME];

  for (JsonVariant v : frames) {
    const char *hex = v.as<const char *>();
    if (!hex || !hexToFrame(String(hex), testFrame)) {
      sendText(400, "Bad frame hex data");
      return;
    }
  }

  String path = animPath(name);
  File f = LittleFS.open(path, "w");

  if (!f) {
    sendText(500, "Could not write file");
    return;
  }

  serializeJson(doc, f);
  f.close();

  loadAnimation(name);
  animPlaying = false;

  sendText(200, "Saved");
}

void handleApiPlay() {
  if (!server.hasArg("name")) {
    sendText(400, "Missing name");
    return;
  }

  String name = server.arg("name");

  if (!loadAnimation(name)) {
    sendText(400, "Could not load animation");
    return;
  }

  animPlaying = true;
  animIndex = 0;
  lastAnimMs = millis();

  sendText(200, "Playing");
}

void handleApiStop() {
  animPlaying = false;
  sendText(200, "Stopped");
}

void handleApiDefault() {
  if (!server.hasArg("name")) {
    sendText(400, "Missing name");
    return;
  }

  String name = server.arg("name");

  if (!isSafeName(name)) {
    sendText(400, "Bad name");
    return;
  }

  if (!LittleFS.exists(animPath(name))) {
    sendText(404, "Animation not found");
    return;
  }

  saveDefaultName(name);
  sendText(200, "Default set");
}

void handleApiDelete() {
  if (!server.hasArg("name")) {
    sendText(400, "Missing name");
    return;
  }

  String name = server.arg("name");

  if (!isSafeName(name)) {
    sendText(400, "Bad name");
    return;
  }

  String path = animPath(name);

  if (!LittleFS.exists(path)) {
    sendText(404, "Animation not found");
    return;
  }

  LittleFS.remove(path);

  if (defaultAnimName == name) {
    saveDefaultName("");
  }

  if (currentAnimName == name) {
    currentAnimName = "";
    animPlaying = false;
    animFrameCount = 0;
    memset(displayFrame, 0, BYTES_PER_FRAME);
    maxShowFrame(displayFrame);
  }

  sendText(200, "Deleted");
}

void handleApiPreview() {
  if (!server.hasArg("frame")) {
    sendText(400, "Missing frame");
    return;
  }

  String hex = server.arg("frame");

  if (!hexToFrame(hex, displayFrame)) {
    sendText(400, "Bad frame data");
    return;
  }

  animPlaying = false;
  maxShowFrame(displayFrame);

  sendText(200, "Previewed");
}

void handleApiIntensity() {
  if (!server.hasArg("value")) {
    sendText(400, "Missing value");
    return;
  }

  int v = server.arg("value").toInt();
  globalIntensity = constrain(v, 0, 15);
  maxSendAll(MAX_REG_INTENSITY, globalIntensity);

  sendText(200, "Intensity set");
}

// =====================================================
// WEB UI
// =====================================================

const char INDEX_HTML[] PROGMEM = R"HTML(
<!doctype html>
<html>
<head>
  <meta charset="utf-8">
  <title>Protogen MAX7219 Head</title>
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <style>
    body {
      background: #101010;
      color: #eee;
      font-family: Arial, sans-serif;
      margin: 0;
      padding: 16px;
    }

    h1, h2, h3 {
      margin-top: 0;
    }

    .card {
      background: #1b1b1b;
      border: 1px solid #333;
      border-radius: 12px;
      padding: 14px;
      margin-bottom: 14px;
    }

    button, input, select {
      background: #242424;
      color: #eee;
      border: 1px solid #444;
      border-radius: 8px;
      padding: 8px;
      margin: 4px;
    }

    button {
      cursor: pointer;
      background: #2d5cff;
      border: none;
      font-weight: bold;
    }

    button.secondary {
      background: #444;
    }

    button.danger {
      background: #a82121;
    }

    button.active {
      background: #00a86b;
    }

    input[type=range] {
      width: 180px;
    }

    .row {
      display: flex;
      flex-wrap: wrap;
      align-items: center;
      gap: 6px;
      margin-bottom: 8px;
    }

    .groups button {
      min-width: 120px;
    }

    .grid {
      display: grid;
      gap: 3px;
      width: max-content;
      margin-top: 10px;
      touch-action: none;
    }

    .px {
      width: 22px;
      height: 22px;
      background: #050505;
      border: 1px solid #333;
      border-radius: 4px;
      user-select: none;
    }

    .px.on {
      background: #00e676;
      box-shadow: 0 0 8px #00e676;
    }

    .frameList {
      display: flex;
      flex-wrap: wrap;
      gap: 4px;
    }

    .frameBtn {
      background: #444;
      min-width: 40px;
    }

    .frameBtn.active {
      background: #00a86b;
    }

    .small {
      color: #aaa;
      font-size: 13px;
    }

    .status {
      color: #8fd18f;
      font-size: 14px;
      min-height: 18px;
    }

    .saved button {
      margin: 3px;
    }
  </style>
</head>
<body>
  <h1>Protogen MAX7219 Head</h1>

  <div class="card">
    <div class="small">
      WiFi: <b>Protogen-Head</b> |
      Web UI: <b>http://192.168.4.1:8080</b>
    </div>
    <div id="status" class="status"></div>
  </div>

  <div class="card">
    <h2>Animation</h2>

    <div class="row">
      <label>Name:</label>
      <input id="animName" value="default_face" maxlength="31">

      <label>FPS:</label>
      <input id="fps" type="number" min="1" max="60" value="8">

      <label>Brightness:</label>
      <input id="intensity" type="range" min="0" max="15" value="3" oninput="setIntensity()">
      <span id="intensityText">3</span>
    </div>

    <div class="row">
      <button onclick="saveAnimation()">Save animation</button>
      <button onclick="playCurrent()">Play saved</button>
      <button class="secondary" onclick="stopPlayback()">Stop</button>
      <button onclick="setDefault()">Set as power-on default</button>
    </div>

    <div class="small">
      Name can only use letters, numbers, underscore, or dash.
      Max frames in this firmware: 80.
    </div>
  </div>

  <div class="card">
    <h2>Saved Animations</h2>
    <div id="savedList" class="saved"></div>
  </div>

  <div class="card">
    <h2>Frame Editor</h2>

    <div class="groups" id="groups"></div>

    <div class="row">
      <button id="drawBtn" class="active" onclick="setTool(1)">Draw</button>
      <button id="eraseBtn" onclick="setTool(0)">Erase</button>
      <button class="secondary" onclick="invertGroup()">Invert group</button>
      <button class="secondary" onclick="clearGroup()">Clear group</button>
      <button class="secondary" onclick="clearWholeFrame()">Clear whole frame</button>
    </div>

    <div id="grid" class="grid"></div>
  </div>

  <div class="card">
    <h2>Frames</h2>

    <div class="row">
      <button onclick="addBlankFrame()">Add blank</button>
      <button onclick="duplicateFrame()">Duplicate</button>
      <button class="danger" onclick="deleteFrame()">Delete</button>
      <button class="secondary" onclick="prevFrame()">Prev</button>
      <button class="secondary" onclick="nextFrame()">Next</button>
    </div>

    <div id="frameList" class="frameList"></div>
  </div>

<script>
const NUM_MODULES = 14;
const BYTES_PER_FRAME = 112;

const GROUPS = [
  { name: "Left Mouth",  modules: [0, 1, 2] },
  { name: "Right Mouth", modules: [3, 4, 5] },

  { name: "Left Eye",    modules: [6, 7] },
  { name: "Right Eye",   modules: [8, 9] },

  { name: "Left Ear",    modules: [10] },
  { name: "Right Ear",   modules: [11] },

  { name: "Left Nose",   modules: [12] },
  { name: "Right Nose",  modules: [13] }
];

let frames = [new Uint8Array(BYTES_PER_FRAME)];
let selectedFrame = 0;
let selectedGroup = 0;
let tool = 1;
let mouseDown = false;
let previewTimer = null;

function status(msg) {
  document.getElementById("status").textContent = msg;
  setTimeout(() => {
    if (document.getElementById("status").textContent === msg) {
      document.getElementById("status").textContent = "";
    }
  }, 2500);
}

function isSafeName(name) {
  return /^[A-Za-z0-9_-]{1,31}$/.test(name);
}

function byteToHex(b) {
  return b.toString(16).padStart(2, "0").toUpperCase();
}

function frameToHex(frame) {
  let out = "";
  for (let i = 0; i < frame.length; i++) {
    out += byteToHex(frame[i]);
  }
  return out;
}

function hexToFrame(hex) {
  let frame = new Uint8Array(BYTES_PER_FRAME);

  for (let i = 0; i < BYTES_PER_FRAME; i++) {
    frame[i] = parseInt(hex.substr(i * 2, 2), 16);
  }

  return frame;
}

function getPixel(frame, module, x, y) {
  let idx = module * 8 + y;
  let mask = 1 << (7 - x);
  return (frame[idx] & mask) !== 0;
}

function setPixel(frame, module, x, y, value) {
  let idx = module * 8 + y;
  let mask = 1 << (7 - x);

  if (value) {
    frame[idx] |= mask;
  } else {
    frame[idx] &= ~mask;
  }
}

function currentFrame() {
  return frames[selectedFrame];
}

function sendPreviewDebounced() {
  clearTimeout(previewTimer);

  previewTimer = setTimeout(async () => {
    let body = new URLSearchParams();
    body.set("frame", frameToHex(currentFrame()));

    await fetch("/api/preview", {
      method: "POST",
      body
    });
  }, 60);
}

function makeGroups() {
  let el = document.getElementById("groups");
  el.innerHTML = "";

  GROUPS.forEach((g, i) => {
    let b = document.createElement("button");
    b.textContent = g.name;
    b.className = i === selectedGroup ? "active" : "";
    b.onclick = () => {
      selectedGroup = i;
      makeGroups();
      drawGrid();
    };
    el.appendChild(b);
  });
}

function drawGrid() {
  let group = GROUPS[selectedGroup];
  let width = group.modules.length * 8;
  let grid = document.getElementById("grid");

  grid.style.gridTemplateColumns = `repeat(${width}, 22px)`;
  grid.innerHTML = "";

  for (let y = 0; y < 8; y++) {
    for (let gx = 0; gx < width; gx++) {
      let moduleIndex = Math.floor(gx / 8);
      let localX = gx % 8;
      let module = group.modules[moduleIndex];

      let px = document.createElement("div");
      px.className = "px";

      if (getPixel(currentFrame(), module, localX, y)) {
        px.classList.add("on");
      }

      function paint() {
        setPixel(currentFrame(), module, localX, y, tool === 1);
        drawGrid();
        sendPreviewDebounced();
      }

      px.onmousedown = (e) => {
        e.preventDefault();
        mouseDown = true;
        paint();
      };

      px.onmouseenter = () => {
        if (mouseDown) paint();
      };

      px.ontouchstart = (e) => {
        e.preventDefault();
        mouseDown = true;
        paint();
      };

      px.ontouchmove = (e) => {
        e.preventDefault();

        let touch = e.touches[0];
        let target = document.elementFromPoint(touch.clientX, touch.clientY);

        if (target && target.classList.contains("px")) {
          target.dispatchEvent(new MouseEvent("mousedown"));
        }
      };

      grid.appendChild(px);
    }
  }
}

document.body.onmouseup = () => mouseDown = false;
document.body.ontouchend = () => mouseDown = false;

function setTool(t) {
  tool = t;
  document.getElementById("drawBtn").className = t === 1 ? "active" : "";
  document.getElementById("eraseBtn").className = t === 0 ? "active" : "";
}

function clearGroup() {
  let group = GROUPS[selectedGroup];

  for (let module of group.modules) {
    for (let y = 0; y < 8; y++) {
      currentFrame()[module * 8 + y] = 0;
    }
  }

  drawGrid();
  sendPreviewDebounced();
}

function invertGroup() {
  let group = GROUPS[selectedGroup];

  for (let module of group.modules) {
    for (let y = 0; y < 8; y++) {
      currentFrame()[module * 8 + y] ^= 0xFF;
    }
  }

  drawGrid();
  sendPreviewDebounced();
}

function clearWholeFrame() {
  currentFrame().fill(0);
  drawGrid();
  sendPreviewDebounced();
}

function addBlankFrame() {
  if (frames.length >= 80) {
    status("Frame limit reached");
    return;
  }

  frames.push(new Uint8Array(BYTES_PER_FRAME));
  selectedFrame = frames.length - 1;
  refreshFrames();
  drawGrid();
  sendPreviewDebounced();
}

function duplicateFrame() {
  if (frames.length >= 80) {
    status("Frame limit reached");
    return;
  }

  let copy = new Uint8Array(currentFrame());
  frames.splice(selectedFrame + 1, 0, copy);
  selectedFrame++;
  refreshFrames();
  drawGrid();
  sendPreviewDebounced();
}

function deleteFrame() {
  if (frames.length <= 1) {
    currentFrame().fill(0);
  } else {
    frames.splice(selectedFrame, 1);
    selectedFrame = Math.max(0, selectedFrame - 1);
  }

  refreshFrames();
  drawGrid();
  sendPreviewDebounced();
}

function prevFrame() {
  selectedFrame = Math.max(0, selectedFrame - 1);
  refreshFrames();
  drawGrid();
  sendPreviewDebounced();
}

function nextFrame() {
  selectedFrame = Math.min(frames.length - 1, selectedFrame + 1);
  refreshFrames();
  drawGrid();
  sendPreviewDebounced();
}

function refreshFrames() {
  let el = document.getElementById("frameList");
  el.innerHTML = "";

  frames.forEach((f, i) => {
    let b = document.createElement("button");
    b.textContent = i + 1;
    b.className = "frameBtn" + (i === selectedFrame ? " active" : "");
    b.onclick = () => {
      selectedFrame = i;
      refreshFrames();
      drawGrid();
      sendPreviewDebounced();
    };
    el.appendChild(b);
  });
}

async function saveAnimation() {
  let name = document.getElementById("animName").value.trim();
  let fps = parseInt(document.getElementById("fps").value || "8");

  if (!isSafeName(name)) {
    status("Bad name. Use letters, numbers, underscore, or dash.");
    return;
  }

  let data = {
    name,
    fps,
    loop: true,
    frames: frames.map(f => frameToHex(f))
  };

  let res = await fetch("/api/save", {
    method: "POST",
    headers: {"Content-Type": "application/json"},
    body: JSON.stringify(data)
  });

  status(await res.text());
  await loadSavedList();
}

async function playCurrent() {
  let name = document.getElementById("animName").value.trim();

  if (!isSafeName(name)) {
    status("Bad name");
    return;
  }

  let body = new URLSearchParams();
  body.set("name", name);

  let res = await fetch("/api/play", {
    method: "POST",
    body
  });

  status(await res.text());
  await loadSavedList();
}

async function stopPlayback() {
  let res = await fetch("/api/stop", {method: "POST"});
  status(await res.text());
}

async function setDefault() {
  let name = document.getElementById("animName").value.trim();

  if (!isSafeName(name)) {
    status("Bad name");
    return;
  }

  let body = new URLSearchParams();
  body.set("name", name);

  let res = await fetch("/api/default", {
    method: "POST",
    body
  });

  status(await res.text());
  await loadSavedList();
}

async function loadAnimationByName(name) {
  let res = await fetch("/api/load?name=" + encodeURIComponent(name));

  if (!res.ok) {
    status(await res.text());
    return;
  }

  let data = await res.json();

  document.getElementById("animName").value = data.name;
  document.getElementById("fps").value = data.fps || 8;

  frames = data.frames.map(hexToFrame);

  if (frames.length === 0) {
    frames = [new Uint8Array(BYTES_PER_FRAME)];
  }

  selectedFrame = 0;
  refreshFrames();
  drawGrid();
  sendPreviewDebounced();

  status("Loaded " + name);
}

async function deleteAnimationByName(name) {
  if (!confirm("Delete animation " + name + "?")) return;

  let body = new URLSearchParams();
  body.set("name", name);

  let res = await fetch("/api/delete", {
    method: "POST",
    body
  });

  status(await res.text());
  await loadSavedList();
}

async function playAnimationByName(name) {
  let body = new URLSearchParams();
  body.set("name", name);

  let res = await fetch("/api/play", {
    method: "POST",
    body
  });

  status(await res.text());
  await loadSavedList();
}

async function defaultAnimationByName(name) {
  let body = new URLSearchParams();
  body.set("name", name);

  let res = await fetch("/api/default", {
    method: "POST",
    body
  });

  status(await res.text());
  await loadSavedList();
}

async function loadSavedList() {
  let res = await fetch("/api/list");
  let data = await res.json();

  document.getElementById("intensity").value = data.intensity;
  document.getElementById("intensityText").textContent = data.intensity;

  let el = document.getElementById("savedList");
  el.innerHTML = "";

  if (data.animations.length === 0) {
    el.innerHTML = "<div class='small'>No saved animations yet.</div>";
    return;
  }

  for (let name of data.animations) {
    let wrapper = document.createElement("div");
    wrapper.className = "row";

    let label = document.createElement("span");
    label.textContent = name;

    if (name === data.default) {
      label.textContent += "  [power-on default]";
    }

    wrapper.appendChild(label);

    let load = document.createElement("button");
    load.textContent = "Load";
    load.className = "secondary";
    load.onclick = () => loadAnimationByName(name);
    wrapper.appendChild(load);

    let play = document.createElement("button");
    play.textContent = "Play";
    play.onclick = () => playAnimationByName(name);
    wrapper.appendChild(play);

    let def = document.createElement("button");
    def.textContent = "Default";
    def.onclick = () => defaultAnimationByName(name);
    wrapper.appendChild(def);

    let del = document.createElement("button");
    del.textContent = "Delete";
    del.className = "danger";
    del.onclick = () => deleteAnimationByName(name);
    wrapper.appendChild(del);

    el.appendChild(wrapper);
  }
}

async function setIntensity() {
  let value = document.getElementById("intensity").value;
  document.getElementById("intensityText").textContent = value;

  let body = new URLSearchParams();
  body.set("value", value);

  await fetch("/api/intensity", {
    method: "POST",
    body
  });
}

makeGroups();
refreshFrames();
drawGrid();
loadSavedList();
sendPreviewDebounced();
</script>
</body>
</html>
)HTML";

void handleRoot() {
  server.send_P(200, "text/html", INDEX_HTML);
}

// =====================================================
// ANIMATION LOOP
// =====================================================

void updateAnimation() {
  if (!animPlaying || animFrameCount == 0) {
    return;
  }

  uint32_t now = millis();

  if (now - lastAnimMs < animFrameMs) {
    return;
  }

  lastAnimMs = now;

  memcpy(displayFrame, animFrames[animIndex], BYTES_PER_FRAME);
  maxShowFrame(displayFrame);

  animIndex++;

  if (animIndex >= animFrameCount) {
    if (animLoop) {
      animIndex = 0;
    } else {
      animIndex = animFrameCount - 1;
      animPlaying = false;
    }
  }
}

// =====================================================
// SETUP / LOOP
// =====================================================

void setup() {
  Serial.begin(115200);
  delay(500);

  Serial.println();
  Serial.println("Booting Protogen MAX7219 controller...");

  maxInit();

  if (!LittleFS.begin(true)) {
    Serial.println("LittleFS mount failed.");
  } else {
    Serial.println("LittleFS mounted.");
  }

  loadDefaultName();

  WiFi.disconnect(true);
  delay(300);

  WiFi.mode(WIFI_AP);

  IPAddress localIP(192, 168, 4, 1);
  IPAddress gateway(192, 168, 4, 1);
  IPAddress subnet(255, 255, 255, 0);

  WiFi.softAPConfig(localIP, gateway, subnet);

  bool apStarted = WiFi.softAP(AP_SSID, AP_PASS, 1, 0, 4);

  Serial.print("AP started: ");
  Serial.println(apStarted ? "YES" : "NO");
  Serial.print("SSID: ");
  Serial.println(AP_SSID);
  Serial.print("Password: ");
  Serial.println(AP_PASS);
  Serial.print("IP: ");
  Serial.println(WiFi.softAPIP());
  Serial.print("Web UI: http://");
  Serial.print(WiFi.softAPIP());
  Serial.print(":");
  Serial.println(WEB_PORT);

  server.on("/", HTTP_GET, handleRoot);

  server.on("/test", HTTP_GET, []() {
    server.send(200, "text/plain", "ESP32 MAX7219 HTTP server works");
  });

  server.on("/api/list", HTTP_GET, handleApiList);
  server.on("/api/load", HTTP_GET, handleApiLoad);

  server.on("/api/save", HTTP_POST, handleApiSave);
  server.on("/api/play", HTTP_POST, handleApiPlay);
  server.on("/api/stop", HTTP_POST, handleApiStop);
  server.on("/api/default", HTTP_POST, handleApiDefault);
  server.on("/api/delete", HTTP_POST, handleApiDelete);
  server.on("/api/preview", HTTP_POST, handleApiPreview);
  server.on("/api/intensity", HTTP_POST, handleApiIntensity);

  server.onNotFound([]() {
    server.send(404, "text/plain", "Not found");
  });

  server.begin();

  Serial.println("HTTP server started.");

  if (defaultAnimName.length() > 0 && loadAnimation(defaultAnimName)) {
    animPlaying = true;
    Serial.print("Loaded default animation: ");
    Serial.println(defaultAnimName);
  } else {
    memset(displayFrame, 0, BYTES_PER_FRAME);
    maxShowFrame(displayFrame);
    Serial.println("No default animation loaded.");
  }
}

void loop() {
  server.handleClient();
  updateAnimation();
}

#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <LittleFS.h>
#include <ArduinoJson.h>

#define MAX_DIN_PIN 13
#define MAX_CLK_PIN 14
#define MAX_CS_PIN  15

#define NUM_MODULES 16
#define ROWS_PER_MODULE 8
#define BYTES_PER_FRAME (NUM_MODULES * ROWS_PER_MODULE)

#define OLD_NUM_MODULES_14 14
#define OLD_BYTES_PER_FRAME_14 (OLD_NUM_MODULES_14 * ROWS_PER_MODULE)

#define WEB_PORT 8080

// Analog microphone module. Use MAX4466/MAX9814/electret amp with analog OUT.
// Wiring: MIC OUT -> GPIO4, MIC VCC -> 3.3V, MIC GND -> GND.
#define MIC_ADC_PIN 4
#define MIC_SAMPLE_INTERVAL_MS 10
#define MIC_SAMPLES_PER_UPDATE 32
#define MIC_DEFAULT_TRIGGER_LEVEL 180
#define MIC_SILENCE_TIMEOUT_MS 450

#define MAX_FRAMES 80
#define MAX_NAME_LEN 31

const char *AP_SSID = "esp32";
const char *AP_PASS = "protogen123";

uint8_t globalIntensity = 3;

const char *MATRIX_LABELS[NUM_MODULES] = {
  "Left Mouth 1","Left Mouth 2","Left Mouth 3","Left Mouth 4",
  "Right Mouth 1","Right Mouth 2","Right Mouth 3","Right Mouth 4",
  "Left Eye 1","Left Eye 2","Right Eye 1","Right Eye 2",
  "Left Ear","Right Ear","Left Nose","Right Nose"
};

const char *MATRIX_KEYS[NUM_MODULES] = {
  "leftMouth1","leftMouth2","leftMouth3","leftMouth4",
  "rightMouth1","rightMouth2","rightMouth3","rightMouth4",
  "leftEye1","leftEye2","rightEye1","rightEye2",
  "leftEar","rightEar","leftNose","rightNose"
};

#define MAX_REG_DIGIT0      0x01
#define MAX_REG_DECODEMODE  0x09
#define MAX_REG_INTENSITY   0x0A
#define MAX_REG_SCANLIMIT   0x0B
#define MAX_REG_SHUTDOWN    0x0C
#define MAX_REG_DISPLAYTEST 0x0F

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

uint16_t micTriggerLevel = MIC_DEFAULT_TRIGGER_LEVEL;
uint16_t micCurrentLevel = 0;
float micBaseline = 2048.0f;
float micEnvelope = 0.0f;
bool micActive = false;
bool micControlledPlayback = false;
String lastMicAnimName = "";
uint32_t lastMicLoudMs = 0;
uint32_t lastMicUpdateMs = 0;

struct MatrixMap {
  uint8_t physical;
  bool mirror;
  uint16_t rotation;
};

MatrixMap moduleMap[NUM_MODULES];

bool isValidRotation(uint16_t r) {
  return r == 0 || r == 90 || r == 180 || r == 270;
}

void setDefaultModuleMap() {
  for (uint8_t i = 0; i < NUM_MODULES; i++) {
    moduleMap[i].physical = i;
    moduleMap[i].mirror = false;
    moduleMap[i].rotation = 0;
  }
}

bool validateModuleMap(MatrixMap *candidate) {
  bool used[NUM_MODULES];
  memset(used, 0, sizeof(used));

  for (uint8_t i = 0; i < NUM_MODULES; i++) {
    if (candidate[i].physical >= NUM_MODULES) return false;
    if (!isValidRotation(candidate[i].rotation)) return false;
    if (used[candidate[i].physical]) return false;
    used[candidate[i].physical] = true;
  }

  return true;
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

bool rawHexToBytes(const String &hex, uint8_t *out, uint16_t expectedBytes) {
  if (hex.length() != expectedBytes * 2) return false;

  for (uint16_t i = 0; i < expectedBytes; i++) {
    int hi = hexVal(hex[i * 2]);
    int lo = hexVal(hex[i * 2 + 1]);

    if (hi < 0 || lo < 0) return false;

    out[i] = (hi << 4) | lo;
  }

  return true;
}

// Converts old 14-module frames to the new logical 16-module layout.
// Old mouth 0,1,2 -> new 0,1,2,3 blank.
// Old mouth 3,4,5 -> new 4,5,6,7 blank.
void convertOld14ToNew16(const uint8_t *oldFrame, uint8_t *newFrame) {
  memset(newFrame, 0, BYTES_PER_FRAME);

  memcpy(&newFrame[0 * 8],  &oldFrame[0 * 8],  8);
  memcpy(&newFrame[1 * 8],  &oldFrame[1 * 8],  8);
  memcpy(&newFrame[2 * 8],  &oldFrame[2 * 8],  8);

  memcpy(&newFrame[4 * 8],  &oldFrame[3 * 8],  8);
  memcpy(&newFrame[5 * 8],  &oldFrame[4 * 8],  8);
  memcpy(&newFrame[6 * 8],  &oldFrame[5 * 8],  8);

  memcpy(&newFrame[8 * 8],  &oldFrame[6 * 8],  8);
  memcpy(&newFrame[9 * 8],  &oldFrame[7 * 8],  8);

  memcpy(&newFrame[10 * 8], &oldFrame[8 * 8],  8);
  memcpy(&newFrame[11 * 8], &oldFrame[9 * 8],  8);

  memcpy(&newFrame[12 * 8], &oldFrame[10 * 8], 8);
  memcpy(&newFrame[13 * 8], &oldFrame[11 * 8], 8);

  memcpy(&newFrame[14 * 8], &oldFrame[12 * 8], 8);
  memcpy(&newFrame[15 * 8], &oldFrame[13 * 8], 8);
}

bool hexToFrameCompat(const String &hex, uint8_t *out) {
  if (hex.length() == BYTES_PER_FRAME * 2) {
    return rawHexToBytes(hex, out, BYTES_PER_FRAME);
  }

  if (hex.length() == OLD_BYTES_PER_FRAME_14 * 2) {
    uint8_t oldFrame[OLD_BYTES_PER_FRAME_14];

    if (!rawHexToBytes(hex, oldFrame, OLD_BYTES_PER_FRAME_14)) {
      return false;
    }

    convertOld14ToNew16(oldFrame, out);
    return true;
  }

  return false;
}

bool hexToFrame16Only(const String &hex, uint8_t *out) {
  return rawHexToBytes(hex, out, BYTES_PER_FRAME);
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

bool getPixel(const uint8_t *frame, uint8_t module, uint8_t x, uint8_t y) {
  uint8_t row = frame[module * 8 + y];
  return (row & (1 << (7 - x))) != 0;
}

void setPixel(uint8_t *frame, uint8_t module, uint8_t x, uint8_t y, bool value) {
  uint8_t idx = module * 8 + y;
  uint8_t mask = 1 << (7 - x);

  if (value) frame[idx] |= mask;
  else frame[idx] &= ~mask;
}

void transformCoord(uint8_t x, uint8_t y, bool mirror, uint16_t rotation, uint8_t &outX, uint8_t &outY) {
  // Horizontal mirror first, then rotation.
  if (mirror) {
    x = 7 - x;
  }

  switch (rotation) {
    case 90:
      outX = 7 - y;
      outY = x;
      break;

    case 180:
      outX = 7 - x;
      outY = 7 - y;
      break;

    case 270:
      outX = y;
      outY = 7 - x;
      break;

    default:
      outX = x;
      outY = y;
      break;
  }
}

void logicalToPhysicalFrame(const uint8_t *logicalFrame, uint8_t *physicalFrame) {
  memset(physicalFrame, 0, BYTES_PER_FRAME);

  for (uint8_t logical = 0; logical < NUM_MODULES; logical++) {
    uint8_t physical = moduleMap[logical].physical;
    bool mirror = moduleMap[logical].mirror;
    uint16_t rotation = moduleMap[logical].rotation;

    if (physical >= NUM_MODULES) continue;

    for (uint8_t y = 0; y < 8; y++) {
      for (uint8_t x = 0; x < 8; x++) {
        if (getPixel(logicalFrame, logical, x, y)) {
          uint8_t tx, ty;
          transformCoord(x, y, mirror, rotation, tx, ty);
          setPixel(physicalFrame, physical, tx, ty, true);
        }
      }
    }
  }
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

void maxShowPhysicalFrame(const uint8_t *physicalFrame) {
  for (uint8_t row = 0; row < 8; row++) {
    digitalWrite(MAX_CS_PIN, LOW);

    for (int8_t m = NUM_MODULES - 1; m >= 0; m--) {
      maxSend16(MAX_REG_DIGIT0 + row, physicalFrame[m * 8 + row]);
    }

    digitalWrite(MAX_CS_PIN, HIGH);
  }
}

void maxShowFrame(const uint8_t *logicalFrame) {
  uint8_t physicalFrame[BYTES_PER_FRAME];
  logicalToPhysicalFrame(logicalFrame, physicalFrame);
  maxShowPhysicalFrame(physicalFrame);
}

void maxClear() {
  memset(displayFrame, 0, sizeof(displayFrame));

  uint8_t empty[BYTES_PER_FRAME];
  memset(empty, 0, sizeof(empty));
  maxShowPhysicalFrame(empty);
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

const uint8_t GLYPHS_0_F[16][8] = {
  {0x3C,0x66,0x6E,0x76,0x66,0x66,0x3C,0x00},
  {0x18,0x38,0x18,0x18,0x18,0x18,0x7E,0x00},
  {0x3C,0x66,0x06,0x0C,0x18,0x30,0x7E,0x00},
  {0x3C,0x66,0x06,0x1C,0x06,0x66,0x3C,0x00},
  {0x0C,0x1C,0x3C,0x6C,0x7E,0x0C,0x0C,0x00},
  {0x7E,0x60,0x7C,0x06,0x06,0x66,0x3C,0x00},
  {0x1C,0x30,0x60,0x7C,0x66,0x66,0x3C,0x00},
  {0x7E,0x66,0x06,0x0C,0x18,0x18,0x18,0x00},
  {0x3C,0x66,0x66,0x3C,0x66,0x66,0x3C,0x00},
  {0x3C,0x66,0x66,0x3E,0x06,0x0C,0x38,0x00},
  {0x18,0x3C,0x66,0x66,0x7E,0x66,0x66,0x00},
  {0x7C,0x66,0x66,0x7C,0x66,0x66,0x7C,0x00},
  {0x3C,0x66,0x60,0x60,0x60,0x66,0x3C,0x00},
  {0x78,0x6C,0x66,0x66,0x66,0x6C,0x78,0x00},
  {0x7E,0x60,0x60,0x7C,0x60,0x60,0x7E,0x00},
  {0x7E,0x60,0x60,0x7C,0x60,0x60,0x60,0x00}
};

void showPhysicalModuleNumbers() {
  uint8_t physicalFrame[BYTES_PER_FRAME];
  memset(physicalFrame, 0, sizeof(physicalFrame));

  for (uint8_t m = 0; m < NUM_MODULES; m++) {
    memcpy(&physicalFrame[m * 8], GLYPHS_0_F[m], 8);
  }

  maxShowPhysicalFrame(physicalFrame);
}

// =====================================================
// FILE STORAGE
// =====================================================

void loadDefaultName() {
  defaultAnimName = "";

  if (!LittleFS.exists("/default.txt")) return;

  File f = LittleFS.open("/default.txt", "r");
  if (!f) return;

  defaultAnimName = f.readString();
  defaultAnimName.trim();
  f.close();

  if (!isSafeName(defaultAnimName)) defaultAnimName = "";
}

void saveDefaultName(const String &name) {
  File f = LittleFS.open("/default.txt", "w");
  if (!f) return;

  f.print(name);
  f.close();

  defaultAnimName = name;
}

void loadMicConfig() {
  micTriggerLevel = MIC_DEFAULT_TRIGGER_LEVEL;

  if (!LittleFS.exists("/mic_config.json")) return;

  File f = LittleFS.open("/mic_config.json", "r");
  if (!f) return;

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, f);
  f.close();

  if (!err) {
    int threshold = doc["threshold"] | MIC_DEFAULT_TRIGGER_LEVEL;
    micTriggerLevel = constrain(threshold, 10, 2000);
  }
}

bool saveMicConfig() {
  JsonDocument doc;
  doc["threshold"] = micTriggerLevel;

  File f = LittleFS.open("/mic_config.json", "w");
  if (!f) return false;

  size_t written = serializeJson(doc, f);
  f.flush();
  f.close();

  return written > 0;
}
void loadBrightnessConfig() {
  globalIntensity = 3;

  if (!LittleFS.exists("/brightness_config.json")) {
    return;
  }

  File f = LittleFS.open("/brightness_config.json", "r");
  if (!f) return;

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, f);
  f.close();

  if (!err) {
    int brightness = doc["brightness"] | 3;
    globalIntensity = constrain(brightness, 0, 15);
  }
}

bool saveBrightnessConfig() {
  JsonDocument doc;
  doc["brightness"] = globalIntensity;

  File f = LittleFS.open("/brightness_config.json", "w");
  if (!f) return false;

  size_t written = serializeJson(doc, f);
  f.flush();
  f.close();

  return written > 0;
}

bool getAnimationMicEnabled(const String &name) {
  if (!isSafeName(name)) return false;

  String path = animPath(name);
  if (!LittleFS.exists(path)) return false;

  File f = LittleFS.open(path, "r");
  if (!f) return false;

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, f);
  f.close();

  if (err) return false;

  return doc["mic"] | false;
}

bool setAnimationMicEnabled(const String &name, bool enabled) {
  if (!isSafeName(name)) return false;

  String path = animPath(name);
  if (!LittleFS.exists(path)) return false;

  File f = LittleFS.open(path, "r");
  if (!f) return false;

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, f);
  f.close();

  if (err) return false;

  doc["mic"] = enabled;

  f = LittleFS.open(path, "w");
  if (!f) return false;

  serializeJson(doc, f);
  f.close();

  return true;
}

void saveModuleMap() {
  JsonDocument doc;
  JsonArray arr = doc["map"].to<JsonArray>();

  for (uint8_t i = 0; i < NUM_MODULES; i++) {
    JsonObject obj = arr.add<JsonObject>();
    obj["key"] = MATRIX_KEYS[i];
    obj["label"] = MATRIX_LABELS[i];
    obj["physical"] = moduleMap[i].physical;
    obj["mirror"] = moduleMap[i].mirror;
    obj["rotation"] = moduleMap[i].rotation;
  }

  File f = LittleFS.open("/module_map.json", "w");
  if (!f) return;

  serializeJson(doc, f);
  f.close();
}

void loadModuleMap() {
  setDefaultModuleMap();

  if (!LittleFS.exists("/module_map.json")) {
    saveModuleMap();
    return;
  }

  File f = LittleFS.open("/module_map.json", "r");
  if (!f) return;

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, f);
  f.close();

  if (err) {
    saveModuleMap();
    return;
  }

  JsonArray arr = doc["map"].as<JsonArray>();

  if (arr.isNull() || arr.size() != NUM_MODULES) {
    saveModuleMap();
    return;
  }

  MatrixMap candidate[NUM_MODULES];

  for (uint8_t i = 0; i < NUM_MODULES; i++) {
    JsonObject obj = arr[i].as<JsonObject>();

    candidate[i].physical = constrain((int)(obj["physical"] | i), 0, NUM_MODULES - 1);
    candidate[i].mirror = obj["mirror"] | false;
    candidate[i].rotation = obj["rotation"] | 0;

    if (!isValidRotation(candidate[i].rotation)) {
      candidate[i].rotation = 0;
    }
  }

  if (!validateModuleMap(candidate)) {
    saveModuleMap();
    return;
  }

  for (uint8_t i = 0; i < NUM_MODULES; i++) {
    moduleMap[i] = candidate[i];
  }
}

bool loadAnimation(const String &name) {
  if (!isSafeName(name)) return false;

  String path = animPath(name);
  if (!LittleFS.exists(path)) return false;

  File f = LittleFS.open(path, "r");
  if (!f) return false;

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, f);
  f.close();

  if (err) return false;

  int fps = doc["fps"] | 8;
  fps = constrain(fps, 1, 60);

  bool loopVal = doc["loop"] | true;
  JsonArray frames = doc["frames"].as<JsonArray>();

  if (frames.isNull() || frames.size() == 0 || frames.size() > MAX_FRAMES) return false;

  uint16_t count = 0;

  for (JsonVariant v : frames) {
    const char *hex = v.as<const char *>();
    if (!hex) return false;

    if (!hexToFrameCompat(String(hex), animFrames[count])) return false;

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
// MICROPHONE PLAYBACK
// =====================================================

bool findNextMicAnimation(String &outName) {
  outName = "";

  File root = LittleFS.open("/");
  File file = root.openNextFile();

  String firstEnabled = "";
  bool returnNextEnabled = lastMicAnimName.length() == 0;

  while (file) {
    String fname = String(file.name());
    if (fname.startsWith("/")) fname.remove(0, 1);

    if (fname.startsWith("anim_") && fname.endsWith(".json")) {
      String name = fname.substring(5, fname.length() - 5);

      if (getAnimationMicEnabled(name)) {
        if (firstEnabled.length() == 0) {
          firstEnabled = name;
        }

        if (returnNextEnabled) {
          outName = name;
          return true;
        }

        if (name == lastMicAnimName) {
          returnNextEnabled = true;
        }
      }
    }

    file = root.openNextFile();
  }

  if (firstEnabled.length() > 0) {
    outName = firstEnabled;
    return true;
  }

  return false;
}

void startMicAnimation() {
  if (micControlledPlayback && animPlaying) return;

  String name;

  if (!findNextMicAnimation(name)) return;

  if (loadAnimation(name)) {
    animPlaying = true;
    animIndex = 0;
    lastAnimMs = millis();
    micControlledPlayback = true;
    lastMicAnimName = name;

    Serial.print("Mic started animation: ");
    Serial.println(name);
  }
}

void stopMicAnimation() {
  if (!micControlledPlayback) return;

  micControlledPlayback = false;

  if (defaultAnimName.length() > 0 && loadAnimation(defaultAnimName)) {
    animPlaying = true;
    animIndex = 0;
    lastAnimMs = millis();

    Serial.print("Mic stopped, returned to default: ");
    Serial.println(defaultAnimName);
  } else {
    animPlaying = false;
    Serial.println("Mic stopped, no default animation set.");
  }
}

void updateMicrophone() {
  uint32_t now = millis();

  if (now - lastMicUpdateMs < MIC_SAMPLE_INTERVAL_MS) return;

  lastMicUpdateMs = now;

  int peak = 0;

  for (uint8_t i = 0; i < MIC_SAMPLES_PER_UPDATE; i++) {
    int sample = analogRead(MIC_ADC_PIN);

    micBaseline += ((float)sample - micBaseline) * 0.002f;

    int amp = abs(sample - (int)micBaseline);
    if (amp > peak) peak = amp;
  }

  if (peak > micEnvelope) {
    micEnvelope = peak;
  } else {
    micEnvelope *= 0.82f;
  }

  micCurrentLevel = constrain((int)micEnvelope, 0, 4095);

  if (micCurrentLevel >= micTriggerLevel) {
    lastMicLoudMs = now;
  }

  bool voiceNow = (now - lastMicLoudMs) < MIC_SILENCE_TIMEOUT_MS;

  if (voiceNow && !micActive) {
    micActive = true;
    startMicAnimation();
  }

  if (!voiceNow && micActive) {
    micActive = false;
    stopMicAnimation();
  }
}

// =====================================================
// HTTP HELPERS
// =====================================================

void sendNoCacheHeaders() {
  server.sendHeader("Cache-Control", "no-store, no-cache, must-revalidate, max-age=0");
  server.sendHeader("Pragma", "no-cache");
  server.sendHeader("Expires", "0");
}

void sendText(int code, const String &msg) {
  sendNoCacheHeaders();
  server.send(code, "text/plain", msg);
}

void sendJson(int code, const String &json) {
  sendNoCacheHeaders();
  server.send(code, "application/json", json);
}

bool saveAnimationObject(JsonObject obj, String &savedName, String &errMsg) {
  String name = obj["name"] | "";

  if (!isSafeName(name)) {
    errMsg = "Bad name. Use only letters, numbers, underscore, or dash.";
    return false;
  }

  int fps = obj["fps"] | 8;
  fps = constrain(fps, 1, 60);

  bool loopVal = obj["loop"] | true;

  bool existingMic = getAnimationMicEnabled(name);
  bool micVal = existingMic;

  if (obj["mic"].is<bool>()) {
    micVal = obj["mic"].as<bool>();
  } else if (obj["micEnabled"].is<bool>()) {
    micVal = obj["micEnabled"].as<bool>();
  }

  JsonArray frames = obj["frames"].as<JsonArray>();

  if (frames.isNull() || frames.size() == 0) {
    errMsg = "No frames";
    return false;
  }

  if (frames.size() > MAX_FRAMES) {
    errMsg = "Too many frames";
    return false;
  }

  JsonDocument outDoc;
  outDoc["name"] = name;
  outDoc["fps"] = fps;
  outDoc["loop"] = loopVal;
  outDoc["mic"] = micVal;
  outDoc["modules"] = NUM_MODULES;
  outDoc["bytesPerFrame"] = BYTES_PER_FRAME;

  JsonArray outFrames = outDoc["frames"].to<JsonArray>();
  uint8_t convertedFrame[BYTES_PER_FRAME];

  for (JsonVariant v : frames) {
    const char *hex = v.as<const char *>();

    if (!hex || !hexToFrameCompat(String(hex), convertedFrame)) {
      errMsg = "Bad frame hex data. Must be 14-module or 16-module format.";
      return false;
    }

    outFrames.add(frameToHex(convertedFrame));
  }

  File f = LittleFS.open(animPath(name), "w");
  if (!f) {
    errMsg = "Could not write file";
    return false;
  }

  serializeJson(outDoc, f);
  f.close();

  savedName = name;
  return true;
}

// =====================================================
// API HANDLERS
// =====================================================

void handleApiList() {
  String json = "{";
  json += "\"animations\":[";

  bool first = true;

  File root = LittleFS.open("/");
  File file = root.openNextFile();

  while (file) {
    String fname = String(file.name());
    if (fname.startsWith("/")) fname.remove(0, 1);

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
  json += "\"micAnimations\":[";

  bool firstMic = true;
  root = LittleFS.open("/");
  file = root.openNextFile();

  while (file) {
    String fname = String(file.name());
    if (fname.startsWith("/")) fname.remove(0, 1);

    if (fname.startsWith("anim_") && fname.endsWith(".json")) {
      String name = fname.substring(5, fname.length() - 5);

      if (getAnimationMicEnabled(name)) {
        if (!firstMic) json += ",";
        firstMic = false;

        json += "\"";
        json += name;
        json += "\"";
      }
    }

    file = root.openNextFile();
  }

  json += "],";
  json += "\"default\":\"" + defaultAnimName + "\",";
  json += "\"current\":\"" + currentAnimName + "\",";
  json += "\"playing\":";
  json += animPlaying ? "true" : "false";
  json += ",";
  json += "\"micActive\":";
  json += micActive ? "true" : "false";
  json += ",";
  json += "\"micLevel\":";
  json += micCurrentLevel;
  json += ",";
  json += "\"micThreshold\":";
  json += micTriggerLevel;
  json += ",";
  json += "\"intensity\":";
  json += globalIntensity;
  json += ",";
  json += "\"modules\":";
  json += NUM_MODULES;
  json += ",";
  json += "\"bytesPerFrame\":";
  json += BYTES_PER_FRAME;
  json += "}";

  sendJson(200, json);
}

void handleApiModuleMapGet() {
  JsonDocument doc;
  JsonArray arr = doc["map"].to<JsonArray>();

  for (uint8_t i = 0; i < NUM_MODULES; i++) {
    JsonObject obj = arr.add<JsonObject>();
    obj["index"] = i;
    obj["key"] = MATRIX_KEYS[i];
    obj["label"] = MATRIX_LABELS[i];
    obj["physical"] = moduleMap[i].physical;
    obj["mirror"] = moduleMap[i].mirror;
    obj["rotation"] = moduleMap[i].rotation;
  }

  String json;
  serializeJson(doc, json);
  sendJson(200, json);
}

void handleApiModuleMapSave() {
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

  JsonArray arr = doc["map"].as<JsonArray>();

  if (arr.isNull() || arr.size() != NUM_MODULES) {
    sendText(400, "Expected exactly 16 module assignments");
    return;
  }

  MatrixMap candidate[NUM_MODULES];

  for (uint8_t i = 0; i < NUM_MODULES; i++) {
    JsonObject obj = arr[i].as<JsonObject>();

    candidate[i].physical = constrain((int)(obj["physical"] | i), 0, NUM_MODULES - 1);
    candidate[i].mirror = obj["mirror"] | false;
    candidate[i].rotation = obj["rotation"] | 0;

    if (!isValidRotation(candidate[i].rotation)) {
      sendText(400, "Bad rotation. Use 0, 90, 180, or 270.");
      return;
    }
  }

  if (!validateModuleMap(candidate)) {
    sendText(400, "Each physical module number 0-15 must be used exactly once.");
    return;
  }

  for (uint8_t i = 0; i < NUM_MODULES; i++) {
    moduleMap[i] = candidate[i];
  }

  saveModuleMap();

  if (animFrameCount > 0) {
    maxShowFrame(displayFrame);
  }

  sendText(200, "Matrix assignment saved");
}

void handleApiModuleMapReset() {
  setDefaultModuleMap();
  saveModuleMap();

  if (animFrameCount > 0) {
    maxShowFrame(displayFrame);
  }

  sendText(200, "Matrix assignment reset");
}

void handleApiTestModules() {
  animPlaying = false;
  showPhysicalModuleNumbers();
  sendText(200, "Showing physical module numbers 0-F");
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

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, f);
  f.close();

  if (err) {
    sendText(500, String("Bad stored JSON: ") + err.c_str());
    return;
  }

  String animName = doc["name"] | name;
  int fps = doc["fps"] | 8;
  fps = constrain(fps, 1, 60);
  bool loopVal = doc["loop"] | true;

  JsonArray inFrames = doc["frames"].as<JsonArray>();

  if (inFrames.isNull() || inFrames.size() == 0 || inFrames.size() > MAX_FRAMES) {
    sendText(500, "Stored animation has bad frame list");
    return;
  }

  JsonDocument outDoc;
  outDoc["name"] = animName;
  outDoc["fps"] = fps;
  outDoc["loop"] = loopVal;
  outDoc["mic"] = doc["mic"] | false;
  outDoc["modules"] = NUM_MODULES;
  outDoc["bytesPerFrame"] = BYTES_PER_FRAME;

  JsonArray outFrames = outDoc["frames"].to<JsonArray>();
  uint8_t convertedFrame[BYTES_PER_FRAME];

  for (JsonVariant v : inFrames) {
    const char *hex = v.as<const char *>();

    if (!hex || !hexToFrameCompat(String(hex), convertedFrame)) {
      sendText(500, "Stored animation has incompatible frame data");
      return;
    }

    outFrames.add(frameToHex(convertedFrame));
  }

  String json;
  serializeJson(outDoc, json);
  server.send(200, "application/json", json);
}

void handleApiDownload() {
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

  File f = LittleFS.open(path, "r");
  if (!f) {
    sendText(500, "Could not open animation");
    return;
  }

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, f);
  f.close();

  if (err) {
    sendText(500, String("Bad stored JSON: ") + err.c_str());
    return;
  }

  JsonArray inFrames = doc["frames"].as<JsonArray>();

  if (inFrames.isNull()) {
    sendText(500, "Animation has no frames");
    return;
  }

  JsonDocument outDoc;
  outDoc["name"] = doc["name"] | name;
  outDoc["fps"] = doc["fps"] | 8;
  outDoc["loop"] = doc["loop"] | true;
  outDoc["mic"] = doc["mic"] | false;
  outDoc["modules"] = NUM_MODULES;
  outDoc["bytesPerFrame"] = BYTES_PER_FRAME;

  JsonArray outFrames = outDoc["frames"].to<JsonArray>();
  uint8_t convertedFrame[BYTES_PER_FRAME];

  for (JsonVariant v : inFrames) {
    const char *hex = v.as<const char *>();

    if (!hex || !hexToFrameCompat(String(hex), convertedFrame)) {
      sendText(500, "Bad frame data");
      return;
    }

    outFrames.add(frameToHex(convertedFrame));
  }

  String json;
  serializeJson(outDoc, json);

  String filename = name + ".json";
  server.sendHeader("Content-Disposition", "attachment; filename=\"" + filename + "\"");
  server.send(200, "application/json", json);
}

void handleApiExportAll() {
  server.sendHeader("Content-Disposition", "attachment; filename=\"protogen_animations_backup_16module.json\"");
  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send(200, "application/json", "");

  server.sendContent("{\"type\":\"protogen-max7219-backup\",");
  server.sendContent("\"modules\":");
  server.sendContent(String(NUM_MODULES));
  server.sendContent(",\"bytesPerFrame\":");
  server.sendContent(String(BYTES_PER_FRAME));
  server.sendContent(",\"default\":\"");
  server.sendContent(defaultAnimName);
  server.sendContent("\",");

  server.sendContent("\"moduleMap\":[");
  for (uint8_t i = 0; i < NUM_MODULES; i++) {
    if (i > 0) server.sendContent(",");

    server.sendContent("{\"key\":\"");
    server.sendContent(MATRIX_KEYS[i]);
    server.sendContent("\",\"label\":\"");
    server.sendContent(MATRIX_LABELS[i]);
    server.sendContent("\",\"physical\":");
    server.sendContent(String(moduleMap[i].physical));
    server.sendContent(",\"mirror\":");
    server.sendContent(moduleMap[i].mirror ? "true" : "false");
    server.sendContent(",\"rotation\":");
    server.sendContent(String(moduleMap[i].rotation));
    server.sendContent("}");
  }
  server.sendContent("],");

  server.sendContent("\"animations\":[");

  bool first = true;

  File root = LittleFS.open("/");
  File file = root.openNextFile();

  while (file) {
    String fname = String(file.name());
    if (fname.startsWith("/")) fname.remove(0, 1);

    if (fname.startsWith("anim_") && fname.endsWith(".json")) {
      File animFile = LittleFS.open("/" + fname, "r");

      if (animFile) {
        JsonDocument doc;
        DeserializationError err = deserializeJson(doc, animFile);
        animFile.close();

        if (!err) {
          String name = doc["name"] | fname.substring(5, fname.length() - 5);
          int fps = doc["fps"] | 8;
          bool loopVal = doc["loop"] | true;
          bool micVal = doc["mic"] | false;
          JsonArray inFrames = doc["frames"].as<JsonArray>();

          if (!inFrames.isNull()) {
            JsonDocument outDoc;
            outDoc["name"] = name;
            outDoc["fps"] = fps;
            outDoc["loop"] = loopVal;
            outDoc["mic"] = micVal;
            outDoc["modules"] = NUM_MODULES;
            outDoc["bytesPerFrame"] = BYTES_PER_FRAME;

            JsonArray outFrames = outDoc["frames"].to<JsonArray>();

            uint8_t convertedFrame[BYTES_PER_FRAME];
            bool ok = true;

            for (JsonVariant v : inFrames) {
              const char *hex = v.as<const char *>();

              if (!hex || !hexToFrameCompat(String(hex), convertedFrame)) {
                ok = false;
                break;
              }

              outFrames.add(frameToHex(convertedFrame));
            }

            if (ok) {
              String raw;
              serializeJson(outDoc, raw);

              if (!first) server.sendContent(",");
              server.sendContent(raw);
              first = false;
            }
          }
        }
      }
    }

    file = root.openNextFile();
  }

  server.sendContent("]}");
  server.sendContent("");
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

  JsonObject obj = doc.as<JsonObject>();

  String savedName;
  String errMsg;

  if (!saveAnimationObject(obj, savedName, errMsg)) {
    sendText(400, errMsg);
    return;
  }

  loadAnimation(savedName);
  animPlaying = false;
  micControlledPlayback = false;

  sendText(200, "Saved");
}

void handleApiImport() {
  String body = server.arg("plain");

  if (body.length() == 0) {
    sendText(400, "Empty upload");
    return;
  }

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, body);

  if (err) {
    sendText(400, String("Bad JSON upload: ") + err.c_str());
    return;
  }

  if (doc["moduleMap"].is<JsonArray>()) {
    JsonArray arr = doc["moduleMap"].as<JsonArray>();

    if (arr.size() == NUM_MODULES) {
      MatrixMap candidate[NUM_MODULES];

      for (uint8_t i = 0; i < NUM_MODULES; i++) {
        JsonObject obj = arr[i].as<JsonObject>();
        candidate[i].physical = constrain((int)(obj["physical"] | i), 0, NUM_MODULES - 1);
        candidate[i].mirror = obj["mirror"] | false;
        candidate[i].rotation = obj["rotation"] | 0;

        if (!isValidRotation(candidate[i].rotation)) {
          candidate[i].rotation = 0;
        }
      }

      if (validateModuleMap(candidate)) {
        for (uint8_t i = 0; i < NUM_MODULES; i++) {
          moduleMap[i] = candidate[i];
        }

        saveModuleMap();
      }
    }
  }

  if (doc["animations"].is<JsonArray>()) {
    JsonArray animations = doc["animations"].as<JsonArray>();

    if (animations.size() == 0) {
      sendText(400, "Backup has no animations");
      return;
    }

    uint16_t imported = 0;

    for (JsonVariant v : animations) {
      JsonObject obj = v.as<JsonObject>();

      if (obj.isNull()) {
        sendText(400, "Backup contains a bad animation object");
        return;
      }

      String savedName;
      String errMsg;

      if (!saveAnimationObject(obj, savedName, errMsg)) {
        sendText(400, "Import failed: " + errMsg);
        return;
      }

      imported++;
    }

    String backupDefault = doc["default"] | "";

    if (isSafeName(backupDefault) && LittleFS.exists(animPath(backupDefault))) {
      saveDefaultName(backupDefault);
    }

    sendText(200, "Imported " + String(imported) + " animations.");
    return;
  }

  JsonObject obj = doc.as<JsonObject>();

  String savedName;
  String errMsg;

  if (!saveAnimationObject(obj, savedName, errMsg)) {
    sendText(400, errMsg);
    return;
  }

  loadAnimation(savedName);
  animPlaying = false;

  sendText(200, "Imported " + savedName + ".");
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
  micControlledPlayback = false;

  sendText(200, "Playing");
}

void handleApiStop() {
  animPlaying = false;
  micControlledPlayback = false;
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

  if (defaultAnimName == name) saveDefaultName("");

  if (currentAnimName == name) {
    currentAnimName = "";
    animPlaying = false;
    micControlledPlayback = false;
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

  if (!hexToFrame16Only(hex, displayFrame)) {
    sendText(400, "Bad frame data");
    return;
  }

  animPlaying = false;
  micControlledPlayback = false;
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

  if (!saveBrightnessConfig()) {
    String json = "{\"ok\":false,\"error\":\"Brightness changed but LittleFS save failed\",\"intensity\":" + String(globalIntensity) + "}";
    sendJson(500, json);
    return;
  }

  String json = "{\"ok\":true,\"intensity\":" + String(globalIntensity) + "}";
  sendJson(200, json);
}


void handleApiMicSet() {
  if (!server.hasArg("name") || !server.hasArg("enabled")) {
    sendText(400, "Missing name or enabled");
    return;
  }

  String name = server.arg("name");
  String enabledStr = server.arg("enabled");
  enabledStr.toLowerCase();

  bool enabled =
    enabledStr == "1" ||
    enabledStr == "true" ||
    enabledStr == "yes" ||
    enabledStr == "on";

  if (!isSafeName(name)) {
    sendText(400, "Bad name");
    return;
  }

  if (!setAnimationMicEnabled(name, enabled)) {
    sendText(500, "Could not update mic setting");
    return;
  }

  sendText(200, enabled ? "Mic trigger enabled" : "Mic trigger disabled");
}

void handleApiMicConfig() {
  if (!server.hasArg("threshold")) {
    sendText(400, "Missing threshold");
    return;
  }

  int threshold = server.arg("threshold").toInt();
  micTriggerLevel = constrain(threshold, 10, 2000);

  if (!saveMicConfig()) {
    String json = "{\"ok\":false,\"error\":\"Mic threshold changed but LittleFS save failed\",\"threshold\":" + String(micTriggerLevel) + "}";
    sendJson(500, json);
    return;
  }

  String json = "{\"ok\":true,\"threshold\":" + String(micTriggerLevel) + "}";
  sendJson(200, json);
}

void handleApiMicState() {
  String json = "{";
  json += "\"level\":";
  json += micCurrentLevel;
  json += ",";
  json += "\"threshold\":";
  json += micTriggerLevel;
  json += ",";
  json += "\"active\":";
  json += micActive ? "true" : "false";
  json += ",";
  json += "\"controlledPlayback\":";
  json += micControlledPlayback ? "true" : "false";
  json += ",";
  json += "\"current\":\"";
  json += currentAnimName;
  json += "\"";
  json += "}";

  sendJson(200, json);
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

    input[type=file] {
      max-width: 320px;
    }

    input[type=checkbox] {
      width: auto;
      margin-right: 6px;
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
      max-width: 100%;
      overflow-x: auto;
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
      line-height: 1.4;
    }

    .status {
      color: #8fd18f;
      font-size: 14px;
      min-height: 18px;
    }

    .saved button {
      margin: 3px;
    }

    .micCheck {
      display: inline-flex;
      align-items: center;
      gap: 5px;
      background: #242424;
      border: 1px solid #444;
      border-radius: 8px;
      padding: 7px 9px;
      margin: 4px;
      font-size: 13px;
    }

    .meterOuter {
      width: min(420px, 100%);
      height: 18px;
      background: #050505;
      border: 1px solid #444;
      border-radius: 999px;
      overflow: hidden;
    }

    .meterInner {
      height: 100%;
      width: 0%;
      background: #00e676;
    }

    .meterInner.active {
      background: #ffcc00;
    }

    .assignTable {
      width: 100%;
      border-collapse: collapse;
      font-size: 14px;
    }

    .assignTable th, .assignTable td {
      border-bottom: 1px solid #333;
      padding: 6px 4px;
      text-align: left;
    }

    .assignTable select {
      min-width: 82px;
    }

    .matrixName {
      min-width: 125px;
    }
  </style>
</head>
<body>
  <h1>Protogen MAX7219 Head</h1>

  <div class="card">
    <div class="small">
      WiFi: <b>Protogen-Head</b> |
      Web UI: <b>http://192.168.1.1:8080</b><br>
      Layout: <b>16 modules</b>, 4 modules per mouth, 128 bytes per frame.
      Old 14-module JSONs are auto-converted on import/load.
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
      <input id="intensity" type="range" min="0" max="15" value="3"
        oninput="brightnessChanged(this.value)">
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
    <h2>Microphone Trigger</h2>

    <div class="row">
      <label>Threshold:</label>
      <input id="micThreshold" type="range" min="10" max="2000" value="180"
        oninput="micThresholdChanged(this.value)">
      <span id="micThresholdText">180</span>
    </div>

    <div class="row">
      <div class="meterOuter">
        <div id="micMeter" class="meterInner"></div>
      </div>
      <span id="micLevelText">level 0</span>
      <span id="micActiveText"></span>
    </div>

    <div class="small">
      Use an analog mic module: <b>OUT → GPIO4</b>, <b>VCC → 3.3V</b>, <b>GND → GND</b>.
      Check “Mic Play” next to saved animations below. When you talk, the ESP32 plays one checked animation.
      When you stop talking, it returns to the power-on default animation.
    </div>
  </div>

  <div class="card">
    <h2>Matrix Assignment</h2>

    <div class="small">
      Press <b>Show physical module numbers</b> after wiring. It ignores the assignment map and shows the real daisy-chain numbers 0-F.
      Set each logical face part to the physical module number it appears on.
      Mirror is horizontal mirror. Rotation is applied after mirror.
    </div>

    <div style="overflow-x:auto; margin-top:10px;">
      <table class="assignTable">
        <thead>
          <tr>
            <th>Logical matrix</th>
            <th>Physical #</th>
            <th>Mirror</th>
            <th>Rotation</th>
          </tr>
        </thead>
        <tbody id="assignmentRows"></tbody>
      </table>
    </div>

    <div class="row" style="margin-top:10px;">
      <button onclick="saveModuleMap()">Save assignment</button>
      <button class="secondary" onclick="resetModuleMap()">Reset 0-15</button>
      <button class="secondary" onclick="testPhysicalModules()">Show physical module numbers</button>
      <button class="secondary" onclick="reloadCurrentFrame()">Return to current frame</button>
    </div>

    <div class="small" id="assignStatus"></div>
  </div>

  <div class="card">
    <h2>Import / Export</h2>

    <div class="row">
      <button onclick="downloadAll()">Download full backup</button>
    </div>

    <div class="row">
      <input id="importFile" type="file" accept=".json,application/json">
      <button onclick="importAnimationFile()">Upload / import JSON</button>
    </div>

    <div class="small">
      Full backup export includes animations, the power-on default, and the matrix assignment map.
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
const NUM_MODULES = 16;
const BYTES_PER_FRAME = 128;

const GROUPS = [
  { name: "Left Mouth",  modules: [0, 1, 2, 3] },
  { name: "Right Mouth", modules: [4, 5, 6, 7] },
  { name: "Left Eye",    modules: [8, 9] },
  { name: "Right Eye",   modules: [10, 11] },
  { name: "Left Ear",    modules: [12] },
  { name: "Right Ear",   modules: [13] },
  { name: "Left Nose",   modules: [14] },
  { name: "Right Nose",  modules: [15] }
];

const MATRIX_LABELS = [
  "Left Mouth 1", "Left Mouth 2", "Left Mouth 3", "Left Mouth 4",
  "Right Mouth 1", "Right Mouth 2", "Right Mouth 3", "Right Mouth 4",
  "Left Eye 1", "Left Eye 2", "Right Eye 1", "Right Eye 2",
  "Left Ear", "Right Ear", "Left Nose", "Right Nose"
];

let moduleMap = [];
let frames = [new Uint8Array(BYTES_PER_FRAME)];
let selectedFrame = 0;
let selectedGroup = 0;
let tool = 1;
let mouseDown = false;
let previewTimer = null;
let brightnessSaveTimer = null;
let micSaveTimer = null;
let currentBrightnessValue = 3;
let currentMicThresholdValue = 180;
let settingsLoaded = false;

function status(msg) {
  document.getElementById("status").textContent = msg;

  setTimeout(() => {
    if (document.getElementById("status").textContent === msg) {
      document.getElementById("status").textContent = "";
    }
  }, 4000);
}

function assignStatus(msg) {
  document.getElementById("assignStatus").textContent = msg;
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

function copyModule(src, srcModule, dst, dstModule) {
  for (let y = 0; y < 8; y++) {
    dst[dstModule * 8 + y] = src[srcModule * 8 + y];
  }
}

function hexToFrame(hex) {
  if (hex.length === 256) {
    let frame = new Uint8Array(BYTES_PER_FRAME);

    for (let i = 0; i < BYTES_PER_FRAME; i++) {
      frame[i] = parseInt(hex.substr(i * 2, 2), 16);
    }

    return frame;
  }

  if (hex.length === 224) {
    let oldBytes = new Uint8Array(112);

    for (let i = 0; i < 112; i++) {
      oldBytes[i] = parseInt(hex.substr(i * 2, 2), 16);
    }

    let frame = new Uint8Array(BYTES_PER_FRAME);

    copyModule(oldBytes, 0, frame, 0);
    copyModule(oldBytes, 1, frame, 1);
    copyModule(oldBytes, 2, frame, 2);

    copyModule(oldBytes, 3, frame, 4);
    copyModule(oldBytes, 4, frame, 5);
    copyModule(oldBytes, 5, frame, 6);

    copyModule(oldBytes, 6, frame, 8);
    copyModule(oldBytes, 7, frame, 9);

    copyModule(oldBytes, 8, frame, 10);
    copyModule(oldBytes, 9, frame, 11);

    copyModule(oldBytes, 10, frame, 12);
    copyModule(oldBytes, 11, frame, 13);

    copyModule(oldBytes, 12, frame, 14);
    copyModule(oldBytes, 13, frame, 15);

    return frame;
  }

  throw new Error("Bad frame length. Expected 256 hex chars for 16 modules or 224 hex chars for old 14 modules.");
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

async function loadModuleMap() {
  let res = await fetch("/api/modulemap?ts=" + Date.now(), {cache: "no-store"});
  let data = await res.json();

  moduleMap = data.map || [];
  renderModuleMapTable();
}

function renderModuleMapTable() {
  let tbody = document.getElementById("assignmentRows");
  tbody.innerHTML = "";

  for (let i = 0; i < NUM_MODULES; i++) {
    let item = moduleMap[i] || {
      index: i,
      label: MATRIX_LABELS[i],
      physical: i,
      mirror: false,
      rotation: 0
    };

    let tr = document.createElement("tr");

    let nameTd = document.createElement("td");
    nameTd.className = "matrixName";
    nameTd.textContent = item.label || MATRIX_LABELS[i];
    tr.appendChild(nameTd);

    let physicalTd = document.createElement("td");
    let physicalSelect = document.createElement("select");
    physicalSelect.id = "phys_" + i;

    for (let p = 0; p < NUM_MODULES; p++) {
      let opt = document.createElement("option");
      opt.value = p;
      opt.textContent = p.toString(16).toUpperCase();
      if (p === item.physical) opt.selected = true;
      physicalSelect.appendChild(opt);
    }

    physicalTd.appendChild(physicalSelect);
    tr.appendChild(physicalTd);

    let mirrorTd = document.createElement("td");
    let mirrorBox = document.createElement("input");
    mirrorBox.type = "checkbox";
    mirrorBox.id = "mirror_" + i;
    mirrorBox.checked = !!item.mirror;
    mirrorTd.appendChild(mirrorBox);
    tr.appendChild(mirrorTd);

    let rotationTd = document.createElement("td");
    let rotationSelect = document.createElement("select");
    rotationSelect.id = "rot_" + i;

    [0, 90, 180, 270].forEach(r => {
      let opt = document.createElement("option");
      opt.value = r;
      opt.textContent = r + "°";
      if (r === item.rotation) opt.selected = true;
      rotationSelect.appendChild(opt);
    });

    rotationTd.appendChild(rotationSelect);
    tr.appendChild(rotationTd);

    tbody.appendChild(tr);
  }
}

function getModuleMapFromUI() {
  let out = [];

  for (let i = 0; i < NUM_MODULES; i++) {
    out.push({
      index: i,
      key: moduleMap[i]?.key || "",
      label: moduleMap[i]?.label || MATRIX_LABELS[i],
      physical: parseInt(document.getElementById("phys_" + i).value),
      mirror: document.getElementById("mirror_" + i).checked,
      rotation: parseInt(document.getElementById("rot_" + i).value)
    });
  }

  return out;
}

function validateModuleMapClient(map) {
  let used = new Set();

  for (let item of map) {
    if (item.physical < 0 || item.physical >= NUM_MODULES) {
      return "Physical module must be 0-15.";
    }

    if (used.has(item.physical)) {
      return "Duplicate physical module " + item.physical.toString(16).toUpperCase() + ". Each physical number must be used once.";
    }

    used.add(item.physical);
  }

  return "";
}

async function saveModuleMap() {
  let map = getModuleMapFromUI();
  let err = validateModuleMapClient(map);

  if (err) {
    assignStatus(err);
    status(err);
    return;
  }

  let res = await fetch("/api/modulemap", {
    method: "POST",
    headers: {"Content-Type": "application/json"},
    body: JSON.stringify({map})
  });

  let text = await res.text();
  assignStatus(text);
  status(text);

  await loadModuleMap();
  sendPreviewDebounced();
}

async function resetModuleMap() {
  let res = await fetch("/api/modulemap/reset", {method: "POST"});
  let text = await res.text();

  assignStatus(text);
  status(text);

  await loadModuleMap();
  sendPreviewDebounced();
}

async function testPhysicalModules() {
  let res = await fetch("/api/testmodules", {method: "POST"});
  status(await res.text());
}

function reloadCurrentFrame() {
  sendPreviewDebounced();
}

async function saveAnimation() {
  let name = document.getElementById("animName").value.trim();
  let fps = parseInt(document.getElementById("fps").value || "8");

  if (!isSafeName(name)) {
    status("Bad name. Use letters, numbers, underscore, or dash.");
    return;
  }

  let mic = false;

  try {
    let list = await fetch("/api/list").then(r => r.json());
    mic = (list.micAnimations || []).includes(name);
  } catch (e) {
    mic = false;
  }

  let data = {
    name,
    fps,
    loop: true,
    mic,
    modules: NUM_MODULES,
    bytesPerFrame: BYTES_PER_FRAME,
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

  try {
    frames = data.frames.map(hexToFrame);
  } catch (e) {
    status(e.message);
    return;
  }

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

async function setMicForAnimation(name, enabled) {
  let body = new URLSearchParams();
  body.set("name", name);
  body.set("enabled", enabled ? "1" : "0");

  let res = await fetch("/api/micset", {
    method: "POST",
    body
  });

  status(await res.text());
  await loadSavedList();
}

function downloadAnimationByName(name) {
  window.location.href = "/api/download?name=" + encodeURIComponent(name);
}

function downloadAll() {
  window.location.href = "/api/export";
}

async function importAnimationFile() {
  let input = document.getElementById("importFile");

  if (!input.files || input.files.length === 0) {
    status("Choose a JSON file first");
    return;
  }

  let file = input.files[0];
  let text = await file.text();

  let res = await fetch("/api/import", {
    method: "POST",
    headers: {"Content-Type": "application/json"},
    body: text
  });

  status(await res.text());
  await loadSavedList();
  await loadModuleMap();

  input.value = "";
}

async function loadSavedList() {
  let res = await fetch("/api/list?ts=" + Date.now(), {cache: "no-store"});
  let data = await res.json();

  // Only initialize sliders from the ESP32 when the page first loads.
  // After that, slider state is controlled only by direct input events.
  if (!settingsLoaded) {
    currentBrightnessValue = parseInt(data.intensity ?? 3);
    document.getElementById("intensity").value = currentBrightnessValue;
    document.getElementById("intensityText").textContent = currentBrightnessValue;

    currentMicThresholdValue = parseInt(data.micThreshold ?? 180);
    if (document.getElementById("micThreshold")) {
      document.getElementById("micThreshold").value = currentMicThresholdValue;
      document.getElementById("micThresholdText").textContent = currentMicThresholdValue;
    }

    settingsLoaded = true;
  }

  let micAnimations = data.micAnimations || [];

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

    let micLabel = document.createElement("label");
    micLabel.className = "micCheck";

    let micBox = document.createElement("input");
    micBox.type = "checkbox";
    micBox.checked = micAnimations.includes(name);
    micBox.onchange = () => setMicForAnimation(name, micBox.checked);

    micLabel.appendChild(micBox);
    micLabel.appendChild(document.createTextNode("Mic Play"));
    wrapper.appendChild(micLabel);

    let load = document.createElement("button");
    load.textContent = "Load";
    load.className = "secondary";
    load.onclick = () => loadAnimationByName(name);
    wrapper.appendChild(load);

    let play = document.createElement("button");
    play.textContent = "Play";
    play.onclick = () => playAnimationByName(name);
    wrapper.appendChild(play);

    let download = document.createElement("button");
    download.textContent = "Download";
    download.className = "secondary";
    download.onclick = () => downloadAnimationByName(name);
    wrapper.appendChild(download);

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

function setBrightnessUI(value) {
  currentBrightnessValue = parseInt(value);
  document.getElementById("intensity").value = currentBrightnessValue;
  document.getElementById("intensityText").textContent = currentBrightnessValue;
}

function brightnessChanged(value) {
  value = parseInt(value);
  setBrightnessUI(value);

  clearTimeout(brightnessSaveTimer);

  // Save the captured value, not whatever the slider may contain after later mouse events.
  brightnessSaveTimer = setTimeout(() => {
    saveIntensityValue(value);
  }, 150);
}

async function saveIntensityValue(value) {
  value = parseInt(value);
  setBrightnessUI(value);

  let body = new URLSearchParams();
  body.set("value", value);

  try {
    let res = await fetch("/api/intensity?ts=" + Date.now(), {
      method: "POST",
      cache: "no-store",
      headers: {"Cache-Control": "no-cache"},
      body
    });

    let text = await res.text();
    let data = null;

    try {
      data = JSON.parse(text);
    } catch (e) {}

    if (!res.ok || (data && data.ok === false)) {
      status(data?.error || text || "Brightness save failed");
      return;
    }

    if (data && data.intensity !== undefined) {
      setBrightnessUI(parseInt(data.intensity));
      status("Brightness saved: " + currentBrightnessValue);
    } else {
      status("Brightness saved: " + value);
    }
  } catch (e) {
    status("Brightness save request failed");
  }
}

function setMicThresholdUI(value) {
  currentMicThresholdValue = parseInt(value);
  document.getElementById("micThreshold").value = currentMicThresholdValue;
  document.getElementById("micThresholdText").textContent = currentMicThresholdValue;
}

function micThresholdChanged(value) {
  value = parseInt(value);
  setMicThresholdUI(value);

  clearTimeout(micSaveTimer);

  // Save the captured value, not whatever the slider may contain after later mouse events.
  micSaveTimer = setTimeout(() => {
    saveMicThresholdValue(value);
  }, 150);
}

async function saveMicThresholdValue(value) {
  value = parseInt(value);
  setMicThresholdUI(value);

  let body = new URLSearchParams();
  body.set("threshold", value);

  try {
    let res = await fetch("/api/micconfig?ts=" + Date.now(), {
      method: "POST",
      cache: "no-store",
      headers: {"Cache-Control": "no-cache"},
      body
    });

    let text = await res.text();
    let data = null;

    try {
      data = JSON.parse(text);
    } catch (e) {}

    if (!res.ok || (data && data.ok === false)) {
      status(data?.error || text || "Mic threshold save failed");
      return;
    }

    if (data && data.threshold !== undefined) {
      setMicThresholdUI(parseInt(data.threshold));
      status("Mic threshold saved: " + currentMicThresholdValue);
    } else {
      status("Mic threshold saved: " + value);
    }
  } catch (e) {
    status("Mic threshold save request failed");
  }
}

async function updateMicMeter() {
  try {
    let data = await fetch("/api/micstate?ts=" + Date.now(), {cache: "no-store"}).then(r => r.json());

    let level = data.level || 0;
    let active = data.active || false;

    // Use the UI threshold for the meter scale so mic polling never changes the slider.
    let percent = Math.min(100, Math.round((level / Math.max(currentMicThresholdValue * 2, 1)) * 100));

    let meter = document.getElementById("micMeter");
    if (!meter) return;

    meter.style.width = percent + "%";
    meter.className = active ? "meterInner active" : "meterInner";

    document.getElementById("micLevelText").textContent = "level " + level;
    document.getElementById("micActiveText").textContent = active ? "VOICE ACTIVE" : "";

    // Do not write to micThreshold here. This polling loop used to snap the slider
    // back to stale backend values while the user was dragging/saving.
  } catch (e) {
    // Ignore temporary WiFi/request errors.
  }
}

makeGroups();
refreshFrames();
drawGrid();
loadModuleMap();
loadSavedList();
sendPreviewDebounced();
setInterval(updateMicMeter, 300);
</script>
</body>
</html>
)HTML";

void handleRoot() {
  sendNoCacheHeaders();
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
  Serial.println("Booting Protogen MAX7219 controller with matrix assignment...");

  setDefaultModuleMap();
  if (!LittleFS.begin(true)) {
    Serial.println("LittleFS mount failed.");
  } else {
    Serial.println("LittleFS mounted.");
  }

  loadBrightnessConfig();

  maxInit();
  loadDefaultName();
  loadModuleMap();
  loadMicConfig();

  analogReadResolution(12);
  analogSetPinAttenuation(MIC_ADC_PIN, ADC_11db);

  long warmupSum = 0;
  for (uint16_t i = 0; i < 128; i++) {
    warmupSum += analogRead(MIC_ADC_PIN);
    delay(2);
  }
  micBaseline = warmupSum / 128.0f;

  WiFi.disconnect(true);
  delay(300);

  WiFi.mode(WIFI_AP);

  IPAddress localIP(192, 168, 1, 1);
  IPAddress gateway(192, 168, 1, 1);
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
  Serial.print("Modules: ");
  Serial.println(NUM_MODULES);
  Serial.print("Bytes per frame: ");
  Serial.println(BYTES_PER_FRAME);
  Serial.print("Mic ADC pin: GPIO");
  Serial.println(MIC_ADC_PIN);
  Serial.print("Mic threshold: ");
  Serial.println(micTriggerLevel);

  server.on("/", HTTP_GET, handleRoot);

  server.on("/test", HTTP_GET, []() {
    server.send(200, "text/plain", "ESP32 MAX7219 HTTP server works - matrix assignment version");
  });

  server.on("/api/list", HTTP_GET, handleApiList);
  server.on("/api/load", HTTP_GET, handleApiLoad);
  server.on("/api/download", HTTP_GET, handleApiDownload);
  server.on("/api/export", HTTP_GET, handleApiExportAll);
  server.on("/api/modulemap", HTTP_GET, handleApiModuleMapGet);
  server.on("/api/micstate", HTTP_GET, handleApiMicState);

  server.on("/api/save", HTTP_POST, handleApiSave);
  server.on("/api/import", HTTP_POST, handleApiImport);
  server.on("/api/play", HTTP_POST, handleApiPlay);
  server.on("/api/stop", HTTP_POST, handleApiStop);
  server.on("/api/default", HTTP_POST, handleApiDefault);
  server.on("/api/delete", HTTP_POST, handleApiDelete);
  server.on("/api/preview", HTTP_POST, handleApiPreview);
  server.on("/api/intensity", HTTP_POST, handleApiIntensity);
  server.on("/api/modulemap", HTTP_POST, handleApiModuleMapSave);
  server.on("/api/modulemap/reset", HTTP_POST, handleApiModuleMapReset);
  server.on("/api/testmodules", HTTP_POST, handleApiTestModules);
  server.on("/api/micset", HTTP_POST, handleApiMicSet);
  server.on("/api/micconfig", HTTP_POST, handleApiMicConfig);

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
  updateMicrophone();
}

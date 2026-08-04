#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_GC9A01A.h>
#include <ESP32-HUB75-MatrixPanel-I2S-DMA.h>
#include <mbedtls/base64.h>
#include <esp_system.h>

// ============================================================================
// PROTOGEN HUB75 + GC9A01 REBASE
//
// HUB75:
//   Two 64x32 RGB HUB75 panels chained horizontally:
//   ESP32 -> Left panel IN -> Left panel OUT -> Right panel IN
//
//   The web editor has two tabs: LEFT and RIGHT.
//   Each animation frame contains one 64x32 RGB image for each panel.
//
// GC9A01 ears:
//   Two 240x240 SPI displays, one left and one right.
//   Upload a normal image from the web UI. The browser center-crops/resizes it
//   to 240x240, converts it to RGB565, then stores it in LittleFS.
//
// IMPORTANT:
//   * Do not power HUB75 panels from the ESP32 5V pin.
//   * Power HUB75 panels from a substantial regulated 5V supply.
//   * Connect HUB75 GND, GC9A01 GND, ESP32 GND, and the 5V supply GND together.
//   * The default HUB75 pin mapping below is an example for ESP32-S3-DevKitC-1.
//     Change the pin constants to match your physical wiring before flashing.
// ============================================================================

// ----------------------------------------------------------------------------
// Wi-Fi AP
// ----------------------------------------------------------------------------

#define WEB_PORT 8080
const char *AP_SSID = "esp32";
const char *AP_PASS = "protogen123";

// ----------------------------------------------------------------------------
// HUB75 panel configuration
// ----------------------------------------------------------------------------

#define HUB_PANEL_W 64
#define HUB_PANEL_H 32
#define HUB_PANEL_COUNT 2
#define HUB_TOTAL_W (HUB_PANEL_W * HUB_PANEL_COUNT)
#define HUB_PANEL_PIXELS (HUB_PANEL_W * HUB_PANEL_H)
#define HUB_FRAME_BYTES (HUB_PANEL_PIXELS * HUB_PANEL_COUNT)  // RGB332, 1 byte/pixel
#define MAX_FRAMES 12

// Set true when the first physical panel in the chain is mounted on the left.
// If your ribbon cable chain is ESP32 -> right panel -> left panel, set false.
#define HUB_LEFT_ON_FIRST_PANEL true

// Standard 64x32 P3 HUB75 panels are normally 1/16 scan and do not use E.
// If your panel has an E signal and needs it, set HUB_E_PIN to an unused GPIO.
#define HUB_R1_PIN   9
#define HUB_G1_PIN   3
#define HUB_B1_PIN   8
#define HUB_R2_PIN   18
#define HUB_G2_PIN   17
#define HUB_B2_PIN   38
#define HUB_A_PIN    36
#define HUB_B_PIN    10
#define HUB_C_PIN    40
#define HUB_D_PIN    37
#define HUB_E_PIN    -1
#define HUB_LAT_PIN  15
#define HUB_OE_PIN   11
#define HUB_CLK_PIN  12

// 0..255. Lower values are usually far more wearable and power-friendly.
uint8_t hubBrightness = 64;

// ----------------------------------------------------------------------------
// GC9A01 ear display configuration
// ----------------------------------------------------------------------------

// Ear LCD pin driving is disabled for now because the HUB75 wiring must keep
// GPIO18, GPIO36, and GPIO37. Leave this at 0 until the ears are moved to
// non-conflicting pins. Browser upload/storage can stay, but no GC9A01 pins
// are initialized or driven.
#define ENABLE_EAR_LCDS 0

// Both ear displays share SPI clock, MOSI, DC, and reset.
// They need separate chip-select pins. These are ignored while ENABLE_EAR_LCDS=0.
#define EAR_MOSI_PIN     18
#define EAR_SCLK_PIN     21
#define EAR_DC_PIN       35
#define EAR_RST_PIN      36
#define EAR_LEFT_CS_PIN  37
#define EAR_RIGHT_CS_PIN 39

#define EAR_W 240
#define EAR_H 240
#define EAR_IMAGE_BYTES (EAR_W * EAR_H * 2)  // RGB565 little-endian
const char *EAR_LEFT_PATH = "/ear_left.rgb565";
const char *EAR_RIGHT_PATH = "/ear_right.rgb565";

// ----------------------------------------------------------------------------
// Microphone configuration
// ----------------------------------------------------------------------------

#define MIC_ADC_PIN 4
#define MIC_SAMPLE_INTERVAL_MS 10
#define MIC_SAMPLES_PER_UPDATE 32
#define MIC_DEFAULT_TRIGGER_LEVEL 180
#define MIC_SILENCE_TIMEOUT_MS 450

// ----------------------------------------------------------------------------
// Animation / persistent configuration
// ----------------------------------------------------------------------------

#define MAX_NAME_LEN 31

const char *ANIM_PREFIX = "/anim_";
const char *ANIM_SUFFIX = ".json";
const char *DEFAULT_PATH = "/default.txt";
const char *BRIGHTNESS_PATH = "/brightness.json";
const char *MIC_CONFIG_PATH = "/mic_config.json";
const char *RANDOM_CONFIG_PATH = "/random_config.json";

WebServer server(WEB_PORT);

MatrixPanel_I2S_DMA *hub = nullptr;
#if ENABLE_EAR_LCDS
Adafruit_GC9A01A earLeft(EAR_LEFT_CS_PIN, EAR_DC_PIN, EAR_RST_PIN);
Adafruit_GC9A01A earRight(EAR_RIGHT_CS_PIN, EAR_DC_PIN, EAR_RST_PIN);
#endif

uint8_t animFrames[MAX_FRAMES][HUB_FRAME_BYTES];
uint8_t displayFrame[HUB_FRAME_BYTES];

uint16_t animFrameCount = 0;
uint16_t animFrameMs = 125;
bool animLoop = true;
bool animPlaying = false;
uint16_t animIndex = 0;
uint32_t lastAnimMs = 0;
String currentAnimName = "";
String defaultAnimName = "";

bool randomEnabled = false;
uint16_t randomIntervalSeconds = 60;
uint32_t lastRandomSwitchMs = 0;
String lastRandomAnimName = "";

uint16_t micTriggerLevel = MIC_DEFAULT_TRIGGER_LEVEL;
uint16_t micCurrentLevel = 0;
float micBaseline = 2048.0f;
float micEnvelope = 0.0f;
bool micActive = false;
bool micControlledPlayback = false;
String lastMicAnimName = "";
uint32_t lastMicLoudMs = 0;
uint32_t lastMicUpdateMs = 0;

File earUploadFile;
String earUploadTempPath = "";
String earUploadFinalPath = "";
size_t earUploadBytes = 0;
bool earUploadOk = false;

// ============================================================================
// BASIC HELPERS
// ============================================================================

void sendNoCacheHeaders() {
  server.sendHeader("Cache-Control", "no-store, no-cache, must-revalidate, max-age=0");
  server.sendHeader("Pragma", "no-cache");
  server.sendHeader("Expires", "0");
}

void sendText(int statusCode, const String &text) {
  sendNoCacheHeaders();
  server.send(statusCode, "text/plain", text);
}

void sendJson(int statusCode, const String &json) {
  sendNoCacheHeaders();
  server.send(statusCode, "application/json", json);
}

bool isSafeName(const String &name) {
  if (name.length() == 0 || name.length() > MAX_NAME_LEN) return false;

  for (uint16_t i = 0; i < name.length(); i++) {
    char c = name[i];
    bool okay =
      (c >= 'a' && c <= 'z') ||
      (c >= 'A' && c <= 'Z') ||
      (c >= '0' && c <= '9') ||
      c == '_' ||
      c == '-';

    if (!okay) return false;
  }

  return true;
}

String animPath(const String &name) {
  return String(ANIM_PREFIX) + name + ANIM_SUFFIX;
}

bool writeStringAtomic(const String &path, const String &contents) {
  String temp = path + ".tmp";
  LittleFS.remove(temp);

  File f = LittleFS.open(temp, "w");
  if (!f) return false;

  size_t written = f.print(contents);
  f.flush();
  f.close();

  if (written != contents.length()) {
    LittleFS.remove(temp);
    return false;
  }

  LittleFS.remove(path);
  return LittleFS.rename(temp, path);
}

bool writeJsonAtomic(const String &path, JsonDocument &doc) {
  String temp = path + ".tmp";
  LittleFS.remove(temp);

  File f = LittleFS.open(temp, "w");
  if (!f) return false;

  size_t written = serializeJson(doc, f);
  f.flush();
  f.close();

  if (written == 0) {
    LittleFS.remove(temp);
    return false;
  }

  LittleFS.remove(path);
  return LittleFS.rename(temp, path);
}

bool readJsonFile(const String &path, JsonDocument &doc) {
  if (!LittleFS.exists(path)) return false;

  File f = LittleFS.open(path, "r");
  if (!f) return false;

  DeserializationError err = deserializeJson(doc, f);
  f.close();

  return !err;
}

bool bytesToBase64(const uint8_t *src, size_t srcLen, String &out) {
  size_t encodedLen = 0;
  size_t capacity = 4 * ((srcLen + 2) / 3) + 1;

  char *buffer = (char *)malloc(capacity);
  if (!buffer) return false;

  int result = mbedtls_base64_encode(
    reinterpret_cast<unsigned char *>(buffer),
    capacity,
    &encodedLen,
    src,
    srcLen
  );

  if (result != 0) {
    free(buffer);
    return false;
  }

  buffer[encodedLen] = '\0';
  out = String(buffer);
  free(buffer);
  return true;
}

bool base64ToBytes(const String &input, uint8_t *dst, size_t expectedLen) {
  size_t decodedLen = 0;

  int result = mbedtls_base64_decode(
    dst,
    expectedLen,
    &decodedLen,
    reinterpret_cast<const unsigned char *>(input.c_str()),
    input.length()
  );

  return result == 0 && decodedLen == expectedLen;
}

uint16_t rgb332To565(uint8_t value) {
  uint8_t r3 = (value >> 5) & 0x07;
  uint8_t g3 = (value >> 2) & 0x07;
  uint8_t b2 = value & 0x03;

  uint8_t r8 = (r3 * 255) / 7;
  uint8_t g8 = (g3 * 255) / 7;
  uint8_t b8 = (b2 * 255) / 3;

  return hub->color565(r8, g8, b8);
}

bool getAnimFlag(const String &name, const char *flagName) {
  if (!isSafeName(name)) return false;

  JsonDocument doc;
  if (!readJsonFile(animPath(name), doc)) return false;

  return doc[flagName] | false;
}

bool setAnimFlag(const String &name, const char *flagName, bool value) {
  if (!isSafeName(name)) return false;

  String path = animPath(name);
  JsonDocument doc;
  if (!readJsonFile(path, doc)) return false;

  doc[flagName] = value;
  return writeJsonAtomic(path, doc);
}

// ============================================================================
// PERSISTENT SETTINGS
// ============================================================================

void loadDefaultName() {
  defaultAnimName = "";

  if (!LittleFS.exists(DEFAULT_PATH)) return;

  File f = LittleFS.open(DEFAULT_PATH, "r");
  if (!f) return;

  defaultAnimName = f.readString();
  defaultAnimName.trim();
  f.close();

  if (!isSafeName(defaultAnimName)) {
    defaultAnimName = "";
  }
}

bool saveDefaultName(const String &name) {
  if (name.length() > 0 && !isSafeName(name)) return false;

  if (!writeStringAtomic(DEFAULT_PATH, name)) return false;

  defaultAnimName = name;
  return true;
}

void loadBrightnessConfig() {
  hubBrightness = 64;

  JsonDocument doc;
  if (!readJsonFile(BRIGHTNESS_PATH, doc)) return;

  int saved = doc["brightness"] | 64;
  hubBrightness = constrain(saved, 0, 255);
}

bool saveBrightnessConfig() {
  JsonDocument doc;
  doc["brightness"] = hubBrightness;
  return writeJsonAtomic(BRIGHTNESS_PATH, doc);
}

void loadMicConfig() {
  micTriggerLevel = MIC_DEFAULT_TRIGGER_LEVEL;

  JsonDocument doc;
  if (!readJsonFile(MIC_CONFIG_PATH, doc)) return;

  int saved = doc["threshold"] | MIC_DEFAULT_TRIGGER_LEVEL;
  micTriggerLevel = constrain(saved, 10, 2000);
}

bool saveMicConfig() {
  JsonDocument doc;
  doc["threshold"] = micTriggerLevel;
  return writeJsonAtomic(MIC_CONFIG_PATH, doc);
}

void loadRandomConfig() {
  randomEnabled = false;
  randomIntervalSeconds = 60;

  JsonDocument doc;
  if (!readJsonFile(RANDOM_CONFIG_PATH, doc)) return;

  randomEnabled = doc["enabled"] | false;
  int interval = doc["intervalSeconds"] | 60;
  randomIntervalSeconds = constrain(interval, 1, 3600);
}

bool saveRandomConfig() {
  JsonDocument doc;
  doc["enabled"] = randomEnabled;
  doc["intervalSeconds"] = randomIntervalSeconds;
  return writeJsonAtomic(RANDOM_CONFIG_PATH, doc);
}

// ============================================================================
// HUB75 DISPLAY
// ============================================================================

void initHub75() {
  HUB75_I2S_CFG::i2s_pins pins = {
    HUB_R1_PIN, HUB_G1_PIN, HUB_B1_PIN,
    HUB_R2_PIN, HUB_G2_PIN, HUB_B2_PIN,
    HUB_A_PIN, HUB_B_PIN, HUB_C_PIN, HUB_D_PIN, HUB_E_PIN,
    HUB_LAT_PIN, HUB_OE_PIN, HUB_CLK_PIN
  };

  HUB75_I2S_CFG config(
    HUB_PANEL_W,
    HUB_PANEL_H,
    HUB_PANEL_COUNT,
    pins
  );

  config.double_buff = false;

  hub = new MatrixPanel_I2S_DMA(config);
  hub->begin();
  hub->setBrightness8(hubBrightness);
  hub->clearScreen();
}

void drawHubFrame(const uint8_t *frame) {
  if (!hub) return;

  hub->clearScreen();

  for (uint8_t y = 0; y < HUB_PANEL_H; y++) {
    for (uint8_t x = 0; x < HUB_PANEL_W; x++) {
      uint8_t leftColor = frame[y * HUB_PANEL_W + x];
      uint8_t rightColor = frame[HUB_PANEL_PIXELS + y * HUB_PANEL_W + x];

      uint16_t leftX = HUB_LEFT_ON_FIRST_PANEL ? x : (x + HUB_PANEL_W);
      uint16_t rightX = HUB_LEFT_ON_FIRST_PANEL ? (x + HUB_PANEL_W) : x;

      if (leftColor != 0) {
        hub->drawPixel(leftX, y, rgb332To565(leftColor));
      }

      if (rightColor != 0) {
        hub->drawPixel(rightX, y, rgb332To565(rightColor));
      }
    }
  }
}

void clearHub() {
  memset(displayFrame, 0, sizeof(displayFrame));
  if (hub) hub->clearScreen();
}

// ============================================================================
// GC9A01 EAR DISPLAY
// ============================================================================

void initEars() {
#if ENABLE_EAR_LCDS
  pinMode(EAR_LEFT_CS_PIN, OUTPUT);
  pinMode(EAR_RIGHT_CS_PIN, OUTPUT);
  digitalWrite(EAR_LEFT_CS_PIN, HIGH);
  digitalWrite(EAR_RIGHT_CS_PIN, HIGH);

  SPI.begin(EAR_SCLK_PIN, -1, EAR_MOSI_PIN, EAR_LEFT_CS_PIN);

  earLeft.begin();
  earRight.begin();

  earLeft.setRotation(0);
  earRight.setRotation(0);

  earLeft.fillScreen(GC9A01A_BLACK);
  earRight.fillScreen(GC9A01A_BLACK);
#else
  // Disabled so the GC9A01 code does not reconfigure or toggle any GPIOs
  // currently used by the HUB75 matrix.
  Serial.println("GC9A01 ear LCD pin control disabled.");
#endif
}

void showEarImage(Adafruit_GC9A01A &display, const char *path) {
#if ENABLE_EAR_LCDS
  display.fillScreen(GC9A01A_BLACK);

  if (!LittleFS.exists(path)) return;

  File f = LittleFS.open(path, "r");
  if (!f) return;

  if (f.size() != EAR_IMAGE_BYTES) {
    f.close();
    return;
  }

  uint16_t row[EAR_W];

  for (uint16_t y = 0; y < EAR_H; y++) {
    size_t got = f.read(reinterpret_cast<uint8_t *>(row), sizeof(row));

    if (got != sizeof(row)) {
      break;
    }

    // Files are stored little-endian by the browser, matching ESP32 uint16_t.
    display.drawRGBBitmap(0, y, row, EAR_W, 1);
  }

  f.close();
#else
  (void)display;
  (void)path;
#endif
}

void showAllEarImages() {
#if ENABLE_EAR_LCDS
  showEarImage(earLeft, EAR_LEFT_PATH);
  showEarImage(earRight, EAR_RIGHT_PATH);
#else
  // Ear LCD output disabled. Stored images remain in LittleFS for later use.
#endif
}

bool clearEarImage(const char *path) {
  LittleFS.remove(path);

#if ENABLE_EAR_LCDS
  if (String(path) == EAR_LEFT_PATH) {
    earLeft.fillScreen(GC9A01A_BLACK);
  } else if (String(path) == EAR_RIGHT_PATH) {
    earRight.fillScreen(GC9A01A_BLACK);
  }
#else
  // Do not touch GC9A01 pins while ear LCD support is disabled.
#endif

  return true;
}

// ============================================================================
// ANIMATION STORAGE / LOAD
// ============================================================================

bool loadAnimation(const String &name) {
  if (!isSafeName(name)) return false;

  JsonDocument doc;
  if (!readJsonFile(animPath(name), doc)) return false;

  int fps = doc["fps"] | 8;
  fps = constrain(fps, 1, 30);

  bool loop = doc["loop"] | true;
  JsonArray storedFrames = doc["frames"].as<JsonArray>();

  if (storedFrames.isNull() || storedFrames.size() == 0 || storedFrames.size() > MAX_FRAMES) {
    return false;
  }

  uint16_t count = 0;

  for (JsonVariant value : storedFrames) {
    const char *encoded = value.as<const char *>();
    if (!encoded) return false;

    if (!base64ToBytes(String(encoded), animFrames[count], HUB_FRAME_BYTES)) {
      return false;
    }

    count++;
  }

  animFrameCount = count;
  animFrameMs = 1000 / fps;
  animLoop = loop;
  animIndex = 0;
  currentAnimName = name;

  memcpy(displayFrame, animFrames[0], HUB_FRAME_BYTES);
  drawHubFrame(displayFrame);

  return true;
}

bool saveAnimationObject(JsonObject input, String &savedName, String &error) {
  String name = input["name"] | "";

  if (!isSafeName(name)) {
    error = "Bad name. Use letters, numbers, underscore, or dash.";
    return false;
  }

  int fps = input["fps"] | 8;
  fps = constrain(fps, 1, 30);

  bool loop = input["loop"] | true;
  bool mic = input["mic"] | getAnimFlag(name, "mic");
  bool random = input["random"] | getAnimFlag(name, "random");

  JsonArray inputFrames = input["frames"].as<JsonArray>();

  if (inputFrames.isNull() || inputFrames.size() == 0) {
    error = "No frames";
    return false;
  }

  if (inputFrames.size() > MAX_FRAMES) {
    error = "Too many frames";
    return false;
  }

  JsonDocument output;
  output["format"] = "hub75-rgb332-dual-64x32-v1";
  output["name"] = name;
  output["fps"] = fps;
  output["loop"] = loop;
  output["mic"] = mic;
  output["random"] = random;
  output["panelWidth"] = HUB_PANEL_W;
  output["panelHeight"] = HUB_PANEL_H;
  output["framesPerAnimationLimit"] = MAX_FRAMES;

  JsonArray outFrames = output["frames"].to<JsonArray>();
  uint8_t testFrame[HUB_FRAME_BYTES];

  for (JsonVariant value : inputFrames) {
    const char *encoded = value.as<const char *>();
    if (!encoded) {
      error = "Frame is missing.";
      return false;
    }

    String frameText(encoded);

    if (!base64ToBytes(frameText, testFrame, HUB_FRAME_BYTES)) {
      error = "Bad HUB75 frame data.";
      return false;
    }

    outFrames.add(frameText);
  }

  if (!writeJsonAtomic(animPath(name), output)) {
    error = "Could not write animation to LittleFS.";
    return false;
  }

  savedName = name;
  return true;
}

bool deleteAnimation(const String &name) {
  if (!isSafeName(name)) return false;

  String path = animPath(name);
  if (!LittleFS.exists(path)) return false;

  LittleFS.remove(path);

  if (defaultAnimName == name) {
    saveDefaultName("");
  }

  if (currentAnimName == name) {
    currentAnimName = "";
    animPlaying = false;
    animFrameCount = 0;
    clearHub();
  }

  if (lastMicAnimName == name) lastMicAnimName = "";
  if (lastRandomAnimName == name) lastRandomAnimName = "";

  return true;
}

// ============================================================================
// RANDOM AND MICROPHONE PLAYBACK
// ============================================================================

bool chooseRandomAnimation(String &outName) {
  outName = "";

  uint16_t eligibleCount = 0;
  File root = LittleFS.open("/");
  File file = root.openNextFile();

  while (file) {
    String filename = String(file.name());
    if (filename.startsWith("/")) filename.remove(0, 1);

    if (filename.startsWith("anim_") && filename.endsWith(".json")) {
      String name = filename.substring(5, filename.length() - 5);
      if (getAnimFlag(name, "random")) {
        eligibleCount++;
      }
    }

    file = root.openNextFile();
  }

  if (eligibleCount == 0) return false;

  uint16_t pick = random(eligibleCount);

  root = LittleFS.open("/");
  file = root.openNextFile();

  while (file) {
    String filename = String(file.name());
    if (filename.startsWith("/")) filename.remove(0, 1);

    if (filename.startsWith("anim_") && filename.endsWith(".json")) {
      String name = filename.substring(5, filename.length() - 5);

      if (getAnimFlag(name, "random")) {
        if (pick == 0) {
          outName = name;
          return true;
        }

        pick--;
      }
    }

    file = root.openNextFile();
  }

  return false;
}

void updateRandomization() {
  if (!randomEnabled || micControlledPlayback || micActive) return;
  if (randomIntervalSeconds == 0) return;

  uint32_t now = millis();
  uint32_t intervalMs = (uint32_t)randomIntervalSeconds * 1000UL;

  if (now - lastRandomSwitchMs < intervalMs) return;

  String nextName;
  if (!chooseRandomAnimation(nextName)) {
    lastRandomSwitchMs = now;
    return;
  }

  // Try once more if there is more than one choice and the same animation was
  // selected again. A repeat is still allowed when only one item is eligible.
  if (nextName == currentAnimName) {
    String retryName;
    if (chooseRandomAnimation(retryName)) {
      nextName = retryName;
    }
  }

  if (loadAnimation(nextName)) {
    animPlaying = true;
    animIndex = 0;
    lastAnimMs = now;
    lastRandomAnimName = nextName;
  }

  lastRandomSwitchMs = now;
}

bool findNextMicAnimation(String &outName) {
  outName = "";

  File root = LittleFS.open("/");
  File file = root.openNextFile();

  String firstEnabled = "";
  bool chooseNext = lastMicAnimName.length() == 0;

  while (file) {
    String filename = String(file.name());
    if (filename.startsWith("/")) filename.remove(0, 1);

    if (filename.startsWith("anim_") && filename.endsWith(".json")) {
      String name = filename.substring(5, filename.length() - 5);

      if (getAnimFlag(name, "mic")) {
        if (firstEnabled.length() == 0) firstEnabled = name;

        if (chooseNext) {
          outName = name;
          return true;
        }

        if (name == lastMicAnimName) {
          chooseNext = true;
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
  }
}

void stopMicAnimation() {
  if (!micControlledPlayback) return;

  micControlledPlayback = false;

  if (defaultAnimName.length() > 0 && loadAnimation(defaultAnimName)) {
    animPlaying = true;
    animIndex = 0;
    lastAnimMs = millis();
  } else {
    animPlaying = false;
  }

  lastRandomSwitchMs = millis();
}

void updateMicrophone() {
  uint32_t now = millis();

  if (now - lastMicUpdateMs < MIC_SAMPLE_INTERVAL_MS) return;
  lastMicUpdateMs = now;

  int peak = 0;

  for (uint8_t i = 0; i < MIC_SAMPLES_PER_UPDATE; i++) {
    int sample = analogRead(MIC_ADC_PIN);

    micBaseline += ((float)sample - micBaseline) * 0.002f;

    int amplitude = abs(sample - (int)micBaseline);
    if (amplitude > peak) peak = amplitude;
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

void updateAnimation() {
  if (!animPlaying || animFrameCount == 0) return;

  uint32_t now = millis();

  if (now - lastAnimMs < animFrameMs) return;
  lastAnimMs = now;

  memcpy(displayFrame, animFrames[animIndex], HUB_FRAME_BYTES);
  drawHubFrame(displayFrame);

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

// ============================================================================
// EAR IMAGE UPLOAD
// ============================================================================

void handleEarUploadChunk(const char *finalPath) {
  HTTPUpload &upload = server.upload();

  if (upload.status == UPLOAD_FILE_START) {
    earUploadFinalPath = finalPath;
    earUploadTempPath = String(finalPath) + ".tmp";
    earUploadBytes = 0;
    earUploadOk = false;

    LittleFS.remove(earUploadTempPath);

    earUploadFile = LittleFS.open(earUploadTempPath, "w");
    if (!earUploadFile) {
      Serial.println("Could not open ear upload temp file.");
    }
  } else if (upload.status == UPLOAD_FILE_WRITE) {
    if (earUploadFile) {
      size_t written = earUploadFile.write(upload.buf, upload.currentSize);
      earUploadBytes += written;
    }
  } else if (upload.status == UPLOAD_FILE_END) {
    if (earUploadFile) {
      earUploadFile.flush();
      earUploadFile.close();
    }

    if (earUploadBytes == EAR_IMAGE_BYTES) {
      LittleFS.remove(earUploadFinalPath);

      if (LittleFS.rename(earUploadTempPath, earUploadFinalPath)) {
        earUploadOk = true;
      }
    }

    if (!earUploadOk) {
      LittleFS.remove(earUploadTempPath);
    }
  } else if (upload.status == UPLOAD_FILE_ABORTED) {
    if (earUploadFile) earUploadFile.close();
    LittleFS.remove(earUploadTempPath);
    earUploadOk = false;
  }
}

void finishEarUpload(const char *finalPath) {
  if (!earUploadOk || earUploadFinalPath != finalPath) {
    sendText(400, "Ear upload failed. Expected a 240x240 RGB565 image.");
    return;
  }

#if ENABLE_EAR_LCDS
  if (String(finalPath) == EAR_LEFT_PATH) {
    showEarImage(earLeft, EAR_LEFT_PATH);
  } else {
    showEarImage(earRight, EAR_RIGHT_PATH);
  }

  sendJson(200, "{\"ok\":true,\"earsEnabled\":true}");
#else
  // Upload is still saved to LittleFS, but the display pins are not touched.
  sendJson(200, "{\"ok\":true,\"earsEnabled\":false}");
#endif
}

// ============================================================================
// API HANDLERS
// ============================================================================

void handleApiList() {
  JsonDocument doc;

  doc["default"] = defaultAnimName;
  doc["current"] = currentAnimName;
  doc["playing"] = animPlaying;
  doc["brightness"] = hubBrightness;
  doc["micThreshold"] = micTriggerLevel;
  doc["randomEnabled"] = randomEnabled;
  doc["randomIntervalSeconds"] = randomIntervalSeconds;
  doc["leftEarPresent"] = LittleFS.exists(EAR_LEFT_PATH);
  doc["rightEarPresent"] = LittleFS.exists(EAR_RIGHT_PATH);
  doc["maxFrames"] = MAX_FRAMES;

  JsonArray animations = doc["animations"].to<JsonArray>();

  File root = LittleFS.open("/");
  File file = root.openNextFile();

  while (file) {
    String filename = String(file.name());
    if (filename.startsWith("/")) filename.remove(0, 1);

    if (filename.startsWith("anim_") && filename.endsWith(".json")) {
      String name = filename.substring(5, filename.length() - 5);

      JsonDocument animationDoc;
      if (readJsonFile(animPath(name), animationDoc)) {
        JsonObject item = animations.add<JsonObject>();
        item["name"] = name;
        item["mic"] = animationDoc["mic"] | false;
        item["random"] = animationDoc["random"] | false;
        item["fps"] = animationDoc["fps"] | 8;
        item["frameCount"] = animationDoc["frames"].as<JsonArray>().size();
      }
    }

    file = root.openNextFile();
  }

  String json;
  serializeJson(doc, json);
  sendJson(200, json);
}

void handleApiLoad() {
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

  sendNoCacheHeaders();
  server.streamFile(f, "application/json");
  f.close();
}

void handleApiSave() {
  String body = server.arg("plain");

  if (body.length() == 0) {
    sendText(400, "Empty animation body");
    return;
  }

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, body);

  if (err) {
    sendText(400, String("Bad animation JSON: ") + err.c_str());
    return;
  }

  String savedName;
  String error;

  if (!saveAnimationObject(doc.as<JsonObject>(), savedName, error)) {
    sendText(400, error);
    return;
  }

  loadAnimation(savedName);
  animPlaying = false;
  micControlledPlayback = false;
  lastRandomSwitchMs = millis();

  sendText(200, "Animation saved");
}

void handleApiImport() {
  String body = server.arg("plain");

  if (body.length() == 0) {
    sendText(400, "Empty import");
    return;
  }

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, body);

  if (err) {
    sendText(400, String("Bad import JSON: ") + err.c_str());
    return;
  }

  if (doc["animations"].is<JsonArray>()) {
    JsonArray animations = doc["animations"].as<JsonArray>();
    uint16_t count = 0;

    for (JsonVariant item : animations) {
      String name;
      String error;

      if (!saveAnimationObject(item.as<JsonObject>(), name, error)) {
        sendText(400, String("Import failed: ") + error);
        return;
      }

      count++;
    }

    String importedDefault = doc["default"] | "";
    if (isSafeName(importedDefault) && LittleFS.exists(animPath(importedDefault))) {
      saveDefaultName(importedDefault);
    }

    sendText(200, "Imported " + String(count) + " animations");
    return;
  }

  String savedName;
  String error;

  if (!saveAnimationObject(doc.as<JsonObject>(), savedName, error)) {
    sendText(400, error);
    return;
  }

  sendText(200, "Imported " + savedName);
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

  server.sendHeader("Content-Disposition", "attachment; filename=\"" + name + ".json\"");
  sendNoCacheHeaders();
  server.streamFile(f, "application/json");
  f.close();
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
  lastRandomSwitchMs = millis();
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

  if (!isSafeName(name) || !LittleFS.exists(animPath(name))) {
    sendText(400, "Animation not found");
    return;
  }

  if (!saveDefaultName(name)) {
    sendText(500, "Could not save default animation");
    return;
  }

  sendText(200, "Power-on default set");
}

void handleApiDelete() {
  if (!server.hasArg("name")) {
    sendText(400, "Missing name");
    return;
  }

  String name = server.arg("name");

  if (!deleteAnimation(name)) {
    sendText(400, "Could not delete animation");
    return;
  }

  sendText(200, "Deleted");
}

void handleApiPreview() {
  if (!server.hasArg("frame")) {
    sendText(400, "Missing frame");
    return;
  }

  if (!base64ToBytes(server.arg("frame"), displayFrame, HUB_FRAME_BYTES)) {
    sendText(400, "Bad frame data");
    return;
  }

  animPlaying = false;
  micControlledPlayback = false;
  drawHubFrame(displayFrame);

  sendText(200, "Previewed");
}

void handleApiBrightness() {
  if (!server.hasArg("value")) {
    sendText(400, "Missing brightness");
    return;
  }

  hubBrightness = constrain(server.arg("value").toInt(), 0, 255);

  if (hub) hub->setBrightness8(hubBrightness);

  if (!saveBrightnessConfig()) {
    sendJson(500, "{\"ok\":false,\"error\":\"Brightness changed but LittleFS save failed\"}");
    return;
  }

  sendJson(200, "{\"ok\":true,\"brightness\":" + String(hubBrightness) + "}");
}

void handleApiMicConfig() {
  if (!server.hasArg("threshold")) {
    sendText(400, "Missing mic threshold");
    return;
  }

  micTriggerLevel = constrain(server.arg("threshold").toInt(), 10, 2000);

  if (!saveMicConfig()) {
    sendJson(500, "{\"ok\":false,\"error\":\"Mic threshold changed but LittleFS save failed\"}");
    return;
  }

  sendJson(200, "{\"ok\":true,\"threshold\":" + String(micTriggerLevel) + "}");
}

void handleApiMicState() {
  String json = "{";
  json += "\"level\":" + String(micCurrentLevel) + ",";
  json += "\"active\":";
  json += micActive ? "true" : "false";
  json += ",";
  json += "\"threshold\":" + String(micTriggerLevel);
  json += "}";

  sendJson(200, json);
}

void handleApiRandomConfig() {
  if (!server.hasArg("enabled") || !server.hasArg("interval")) {
    sendText(400, "Missing randomization settings");
    return;
  }

  String enabled = server.arg("enabled");
  enabled.toLowerCase();

  randomEnabled =
    enabled == "1" ||
    enabled == "true" ||
    enabled == "yes" ||
    enabled == "on";

  randomIntervalSeconds = constrain(server.arg("interval").toInt(), 1, 3600);
  lastRandomSwitchMs = millis();

  if (!saveRandomConfig()) {
    sendText(500, "Could not save randomization config");
    return;
  }

  sendText(200, "Randomization settings saved");
}

void handleApiMicFlag() {
  if (!server.hasArg("name") || !server.hasArg("enabled")) {
    sendText(400, "Missing name or enabled");
    return;
  }

  String enabled = server.arg("enabled");
  enabled.toLowerCase();

  bool value =
    enabled == "1" ||
    enabled == "true" ||
    enabled == "yes" ||
    enabled == "on";

  if (!setAnimFlag(server.arg("name"), "mic", value)) {
    sendText(500, "Could not change Mic Play setting");
    return;
  }

  sendText(200, "Mic Play setting saved");
}

void handleApiRandomFlag() {
  if (!server.hasArg("name") || !server.hasArg("enabled")) {
    sendText(400, "Missing name or enabled");
    return;
  }

  String enabled = server.arg("enabled");
  enabled.toLowerCase();

  bool value =
    enabled == "1" ||
    enabled == "true" ||
    enabled == "yes" ||
    enabled == "on";

  if (!setAnimFlag(server.arg("name"), "random", value)) {
    sendText(500, "Could not change randomization setting");
    return;
  }

  sendText(200, "Randomization setting saved");
}

void handleApiClearEar() {
  if (!server.hasArg("target")) {
    sendText(400, "Missing ear target");
    return;
  }

  String target = server.arg("target");

  if (target == "left") {
    clearEarImage(EAR_LEFT_PATH);
  } else if (target == "right") {
    clearEarImage(EAR_RIGHT_PATH);
  } else if (target == "both") {
    clearEarImage(EAR_LEFT_PATH);
    clearEarImage(EAR_RIGHT_PATH);
  } else {
    sendText(400, "Bad ear target");
    return;
  }

  sendText(200, "Ear image cleared");
}

// ============================================================================
// WEB UI
// ============================================================================

const char INDEX_HTML[] PROGMEM = R"HTML(
<!doctype html>
<html>
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Protogen HUB75 + Ear LCD</title>
<style>
:root { color-scheme: dark; }
* { box-sizing: border-box; }
body { margin:0; padding:14px; background:#101217; color:#eef3f4; font-family:Arial,sans-serif; }
h1,h2,h3 { margin:0 0 10px; }
h1 { font-size:1.45rem; }
h2 { font-size:1.1rem; }
.card { background:#1b2028; border:1px solid #303947; border-radius:13px; padding:13px; margin:0 0 12px; }
.row { display:flex; flex-wrap:wrap; align-items:center; gap:7px; margin:6px 0; }
button,input,select { font:inherit; background:#252c36; color:#eef3f4; border:1px solid #465264; border-radius:8px; padding:8px; }
button { background:#2d69e8; border:0; font-weight:700; cursor:pointer; }
button.secondary { background:#495464; }
button.danger { background:#a63838; }
button.active { background:#118b65; }
button:disabled { opacity:.45; cursor:default; }
input[type=range] { width:min(230px,72vw); padding:0; }
input[type=checkbox] { width:auto; accent-color:#20bc84; }
.small { color:#aeb9c5; font-size:.82rem; line-height:1.38; }
.status { min-height:19px; color:#83e2b6; font-size:.88rem; }
.gridWrap { overflow:auto; padding:8px; background:#0d1014; border:1px solid #354150; border-radius:10px; max-width:100%; }
#pixelCanvas { display:block; width:512px; height:256px; max-width:none; image-rendering:pixelated; touch-action:none; cursor:crosshair; background:#000; }
.tabs button { min-width:100px; }
.frameList { display:flex; flex-wrap:wrap; gap:5px; }
.frameList button { min-width:39px; background:#485362; }
.frameList button.active { background:#118b65; }
.savedRow { display:flex; flex-wrap:wrap; align-items:center; gap:5px; border-top:1px solid #303947; padding:8px 0; }
.savedName { min-width:120px; font-weight:700; }
.flag { display:inline-flex; gap:4px; align-items:center; padding:6px 8px; border:1px solid #465264; border-radius:8px; background:#252c36; font-size:.82rem; }
.earPreviews { display:flex; flex-wrap:wrap; gap:14px; align-items:start; }
.earPreviewWrap { text-align:center; }
.earPreview { width:120px; height:120px; border-radius:50%; background:#000; border:2px solid #465264; object-fit:cover; }
hr { border:0; border-top:1px solid #303947; margin:12px 0; }
</style>
</head>
<body>
<h1>Protogen HUB75 + GC9A01 Ears</h1>

<div class="card">
  <div class="small">
    AP: <b>esp32</b> &nbsp; | &nbsp; Web UI: <b>http://192.168.1.1:8080</b><br>
    HUB75 animation format: two 64×32 panels, Left + Right, 256 colors per pixel.
  </div>
  <div id="status" class="status"></div>
</div>

<div class="card">
  <h2>HUB75 Animation</h2>
  <div class="row">
    <label>Name <input id="animName" maxlength="31" value="default_face"></label>
    <label>FPS <input id="fps" type="number" min="1" max="30" value="8" style="width:75px"></label>
    <label>Brightness <input id="brightness" type="range" min="0" max="255" value="64" oninput="brightnessChanged(this.value)"></label>
    <span id="brightnessText">64</span>
  </div>
  <div class="row">
    <button onclick="saveAnimation()">Save</button>
    <button onclick="playCurrent()">Play saved</button>
    <button class="secondary" onclick="stopPlayback()">Stop</button>
    <button onclick="setDefault()">Set power-on default</button>
  </div>
  <div class="small">A frame contains both 64×32 HUB75 halves. Save/load/import only uses this new HUB75 format.</div>
</div>

<div class="card">
  <h2>Microphone Trigger</h2>
  <div class="row">
    <label>Threshold <input id="micThreshold" type="range" min="10" max="2000" value="180" oninput="micThresholdChanged(this.value)"></label>
    <span id="micThresholdText">180</span>
  </div>
  <div class="row">
    <div style="width:min(420px,100%);height:18px;border:1px solid #465264;border-radius:999px;overflow:hidden;background:#060709">
      <div id="micMeter" style="height:100%;width:0%;background:#20cf8b"></div>
    </div>
    <span id="micLevelText" class="small">level 0</span>
    <b id="micActiveText" style="color:#ffd25d;font-size:.8rem"></b>
  </div>
  <div class="small">Mic OUT → GPIO4. Mark animations with <b>Mic Play</b> below. Speaking plays the selected animation; silence returns to the power-on default.</div>
</div>

<div class="card">
  <h2>Randomization</h2>
  <div class="row">
    <label class="flag"><input id="randomEnabled" type="checkbox" onchange="saveRandomConfig()"> Enable animation randomization</label>
    <label>Every <input id="randomInterval" type="number" min="1" max="3600" value="60" onchange="saveRandomConfig()" style="width:90px"> seconds</label>
  </div>
  <div class="small">Boot always starts with the power-on default. After the selected interval, the ESP32 randomly chooses from animations checked as <b>Random</b>.</div>
</div>

<div class="card">
  <h2>GC9A01 Ear Images (LCD pins disabled)</h2>
  <div class="row">
    <select id="earTarget">
      <option value="left">Left ear</option>
      <option value="right">Right ear</option>
      <option value="both">Both ears</option>
    </select>
    <input id="earImageFile" type="file" accept="image/*">
    <button onclick="uploadEarImage()">Convert and upload image</button>
    <button class="secondary" onclick="clearEars()">Clear selected</button>
  </div>
  <div class="small">PNG, JPG, WebP, and other browser-readable images are center-cropped and resized to 240×240 before upload. Uploading stores the selected ear image on the ESP32, but the GC9A01 LCD pins are disabled in this build so the HUB75 row pins are not disturbed.</div>
  <div class="earPreviews" style="margin-top:10px">
    <div class="earPreviewWrap"><img id="leftEarPreview" class="earPreview" alt="Left ear"><div class="small">Left ear</div></div>
    <div class="earPreviewWrap"><img id="rightEarPreview" class="earPreview" alt="Right ear"><div class="small">Right ear</div></div>
  </div>
</div>

<div class="card">
  <h2>Saved HUB75 Animations</h2>
  <div id="savedList"></div>
</div>

<div class="card">
  <h2>Left / Right HUB75 Frame Editor</h2>
  <div class="row tabs">
    <button id="leftTab" class="active" onclick="setPanel('left')">Left</button>
    <button id="rightTab" onclick="setPanel('right')">Right</button>
    <label>Color <input id="colorPicker" type="color" value="#00e6ff"></label>
    <button id="drawBtn" class="active" onclick="setTool('draw')">Draw</button>
    <button id="eraseBtn" class="secondary" onclick="setTool('erase')">Erase</button>
  </div>
  <div class="row">
    <button class="secondary" onclick="clearPanel()">Clear this side</button>
    <button class="secondary" onclick="fillPanel()">Fill this side</button>
    <button class="secondary" onclick="previewFrame()">Preview current frame</button>
  </div>
  <div class="gridWrap"><canvas id="pixelCanvas" width="512" height="256"></canvas></div>
  <div class="small" style="margin-top:8px">Canvas is 64×32 pixels. Click/drag or use touch to draw.</div>
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

<div class="card">
  <h2>Import / Download</h2>
  <div class="row">
    <input id="importFile" type="file" accept=".json,application/json">
    <button onclick="importAnimation()">Import HUB75 animation JSON</button>
  </div>
  <div class="small">Download is available per saved animation. Old MAX7219 animation files are intentionally not supported by this rebase.</div>
</div>

<script>
const W = 64;
const H = 32;
const PANEL_BYTES = W * H;
const FRAME_BYTES = PANEL_BYTES * 2;
const SCALE = 8;

let frames = [new Uint8Array(FRAME_BYTES)];
let selectedFrame = 0;
let activePanel = 'left';
let tool = 'draw';
let painting = false;
let previewTimer = null;
let brightnessSaveTimer = null;
let micSaveTimer = null;
let brightnessValue = 64;
let micThresholdValue = 180;
let settingsLoaded = false;
let animationMic = false;
let animationRandom = false;

const canvas = document.getElementById('pixelCanvas');
const ctx = canvas.getContext('2d');
ctx.imageSmoothingEnabled = false;

function status(text) {
  const el = document.getElementById('status');
  el.textContent = text;
  setTimeout(() => { if (el.textContent === text) el.textContent = ''; }, 4500);
}

function currentFrame() {
  return frames[selectedFrame];
}

function isSafeName(name) {
  return /^[A-Za-z0-9_-]{1,31}$/.test(name);
}

function rgb332ToCss(value) {
  const r = ((value >> 5) & 7) * 255 / 7;
  const g = ((value >> 2) & 7) * 255 / 7;
  const b = (value & 3) * 255 / 3;
  return `rgb(${Math.round(r)},${Math.round(g)},${Math.round(b)})`;
}

function hexToRgb332(hex) {
  const r = parseInt(hex.slice(1, 3), 16);
  const g = parseInt(hex.slice(3, 5), 16);
  const b = parseInt(hex.slice(5, 7), 16);
  return ((r >> 5) << 5) | ((g >> 5) << 2) | (b >> 6);
}

function panelOffset() {
  return activePanel === 'left' ? 0 : PANEL_BYTES;
}

function setPanel(side) {
  activePanel = side;
  document.getElementById('leftTab').className = side === 'left' ? 'active' : 'secondary';
  document.getElementById('rightTab').className = side === 'right' ? 'active' : 'secondary';
  drawEditor();
}

function setTool(nextTool) {
  tool = nextTool;
  document.getElementById('drawBtn').className = tool === 'draw' ? 'active' : 'secondary';
  document.getElementById('eraseBtn').className = tool === 'erase' ? 'active' : 'secondary';
}

function drawEditor() {
  const data = currentFrame();
  const offset = panelOffset();

  ctx.fillStyle = '#000';
  ctx.fillRect(0, 0, canvas.width, canvas.height);

  for (let y = 0; y < H; y++) {
    for (let x = 0; x < W; x++) {
      const c = data[offset + y * W + x];
      if (c) {
        ctx.fillStyle = rgb332ToCss(c);
        ctx.fillRect(x * SCALE, y * SCALE, SCALE, SCALE);
      }
    }
  }

  ctx.strokeStyle = 'rgba(255,255,255,0.08)';
  ctx.lineWidth = 1;
  for (let x = 0; x <= W; x++) {
    ctx.beginPath();
    ctx.moveTo(x * SCALE + 0.5, 0);
    ctx.lineTo(x * SCALE + 0.5, H * SCALE);
    ctx.stroke();
  }
  for (let y = 0; y <= H; y++) {
    ctx.beginPath();
    ctx.moveTo(0, y * SCALE + 0.5);
    ctx.lineTo(W * SCALE, y * SCALE + 0.5);
    ctx.stroke();
  }
}

function paintFromEvent(event) {
  const rect = canvas.getBoundingClientRect();
  const x = Math.floor((event.clientX - rect.left) * W / rect.width);
  const y = Math.floor((event.clientY - rect.top) * H / rect.height);

  if (x < 0 || x >= W || y < 0 || y >= H) return;

  const color = tool === 'erase' ? 0 : hexToRgb332(document.getElementById('colorPicker').value);
  currentFrame()[panelOffset() + y * W + x] = color;

  drawEditor();
  sendPreviewDebounced();
}

canvas.addEventListener('pointerdown', event => {
  event.preventDefault();
  painting = true;
  canvas.setPointerCapture(event.pointerId);
  paintFromEvent(event);
});
canvas.addEventListener('pointermove', event => {
  if (!painting) return;
  event.preventDefault();
  paintFromEvent(event);
});
canvas.addEventListener('pointerup', event => {
  painting = false;
  try { canvas.releasePointerCapture(event.pointerId); } catch (e) {}
});
canvas.addEventListener('pointercancel', () => painting = false);

function clearPanel() {
  currentFrame().fill(0, panelOffset(), panelOffset() + PANEL_BYTES);
  drawEditor();
  sendPreviewDebounced();
}

function fillPanel() {
  const color = hexToRgb332(document.getElementById('colorPicker').value);
  currentFrame().fill(color, panelOffset(), panelOffset() + PANEL_BYTES);
  drawEditor();
  sendPreviewDebounced();
}

function bytesToBase64(bytes) {
  let text = '';
  const chunk = 0x4000;

  for (let i = 0; i < bytes.length; i += chunk) {
    text += String.fromCharCode(...bytes.subarray(i, Math.min(i + chunk, bytes.length)));
  }

  return btoa(text);
}

function base64ToBytes(encoded) {
  const text = atob(encoded);

  if (text.length !== FRAME_BYTES) {
    throw new Error('Bad HUB75 frame size');
  }

  const bytes = new Uint8Array(FRAME_BYTES);
  for (let i = 0; i < text.length; i++) bytes[i] = text.charCodeAt(i);
  return bytes;
}

function refreshFrames() {
  const el = document.getElementById('frameList');
  el.innerHTML = '';

  frames.forEach((frame, i) => {
    const button = document.createElement('button');
    button.textContent = i + 1;
    button.className = i === selectedFrame ? 'active' : '';
    button.onclick = () => {
      selectedFrame = i;
      refreshFrames();
      drawEditor();
      sendPreviewDebounced();
    };
    el.appendChild(button);
  });
}

function addBlankFrame() {
  if (frames.length >= 12) {
    status('Frame limit reached');
    return;
  }

  frames.push(new Uint8Array(FRAME_BYTES));
  selectedFrame = frames.length - 1;
  refreshFrames();
  drawEditor();
  sendPreviewDebounced();
}

function duplicateFrame() {
  if (frames.length >= 12) {
    status('Frame limit reached');
    return;
  }

  frames.splice(selectedFrame + 1, 0, new Uint8Array(currentFrame()));
  selectedFrame++;
  refreshFrames();
  drawEditor();
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
  drawEditor();
  sendPreviewDebounced();
}

function prevFrame() {
  selectedFrame = Math.max(0, selectedFrame - 1);
  refreshFrames();
  drawEditor();
  sendPreviewDebounced();
}

function nextFrame() {
  selectedFrame = Math.min(frames.length - 1, selectedFrame + 1);
  refreshFrames();
  drawEditor();
  sendPreviewDebounced();
}

function sendPreviewDebounced() {
  clearTimeout(previewTimer);
  previewTimer = setTimeout(previewFrame, 100);
}

async function previewFrame() {
  const body = new URLSearchParams();
  body.set('frame', bytesToBase64(currentFrame()));

  try {
    await fetch('/api/preview?ts=' + Date.now(), {
      method: 'POST',
      cache: 'no-store',
      body
    });
  } catch (e) {}
}

async function saveAnimation() {
  const name = document.getElementById('animName').value.trim();
  const fps = parseInt(document.getElementById('fps').value || '8');

  if (!isSafeName(name)) {
    status('Bad name. Use letters, numbers, dash, or underscore.');
    return;
  }

  const payload = {
    format: 'hub75-rgb332-dual-64x32-v1',
    name,
    fps,
    loop: true,
    mic: animationMic,
    random: animationRandom,
    frames: frames.map(bytesToBase64)
  };

  try {
    const res = await fetch('/api/save?ts=' + Date.now(), {
      method: 'POST',
      cache: 'no-store',
      headers: {'Content-Type': 'application/json'},
      body: JSON.stringify(payload)
    });

    status(await res.text());
    await loadSavedList();
  } catch (e) {
    status('Animation save request failed');
  }
}

async function loadAnimationByName(name) {
  try {
    const res = await fetch('/api/load?name=' + encodeURIComponent(name) + '&ts=' + Date.now(), {cache:'no-store'});

    if (!res.ok) {
      status(await res.text());
      return;
    }

    const data = await res.json();

    if (!Array.isArray(data.frames) || data.frames.length === 0) {
      status('Animation has no frames');
      return;
    }

    frames = data.frames.map(base64ToBytes);
    selectedFrame = 0;
    animationMic = !!data.mic;
    animationRandom = !!data.random;

    document.getElementById('animName').value = data.name || name;
    document.getElementById('fps').value = data.fps || 8;

    refreshFrames();
    drawEditor();
    previewFrame();
    status('Loaded ' + name);
  } catch (e) {
    status('Could not load animation');
  }
}

async function playCurrent() {
  const name = document.getElementById('animName').value.trim();
  if (!isSafeName(name)) return status('Bad name');

  const body = new URLSearchParams();
  body.set('name', name);

  const res = await fetch('/api/play?ts=' + Date.now(), {method:'POST', cache:'no-store', body});
  status(await res.text());
  await loadSavedList();
}

async function stopPlayback() {
  const res = await fetch('/api/stop?ts=' + Date.now(), {method:'POST', cache:'no-store'});
  status(await res.text());
}

async function setDefault() {
  const name = document.getElementById('animName').value.trim();
  if (!isSafeName(name)) return status('Bad name');

  const body = new URLSearchParams();
  body.set('name', name);

  const res = await fetch('/api/default?ts=' + Date.now(), {method:'POST', cache:'no-store', body});
  status(await res.text());
  await loadSavedList();
}

async function deleteAnimation(name) {
  if (!confirm('Delete ' + name + '?')) return;

  const body = new URLSearchParams();
  body.set('name', name);

  const res = await fetch('/api/delete?ts=' + Date.now(), {method:'POST', cache:'no-store', body});
  status(await res.text());
  await loadSavedList();
}

function downloadAnimation(name) {
  window.location.href = '/api/download?name=' + encodeURIComponent(name);
}

async function setAnimationFlag(name, flag, enabled) {
  const body = new URLSearchParams();
  body.set('name', name);
  body.set('enabled', enabled ? '1' : '0');

  const url = flag === 'mic' ? '/api/micset' : '/api/randomset';
  const res = await fetch(url + '?ts=' + Date.now(), {method:'POST', cache:'no-store', body});
  status(await res.text());
  await loadSavedList();
}

async function importAnimation() {
  const input = document.getElementById('importFile');

  if (!input.files || input.files.length === 0) {
    status('Choose an animation JSON first');
    return;
  }

  const text = await input.files[0].text();

  const res = await fetch('/api/import?ts=' + Date.now(), {
    method:'POST',
    cache:'no-store',
    headers:{'Content-Type':'application/json'},
    body:text
  });

  status(await res.text());
  input.value = '';
  await loadSavedList();
}

function setBrightnessUI(value) {
  brightnessValue = parseInt(value);
  document.getElementById('brightness').value = brightnessValue;
  document.getElementById('brightnessText').textContent = brightnessValue;
}

function brightnessChanged(value) {
  value = parseInt(value);
  setBrightnessUI(value);

  clearTimeout(brightnessSaveTimer);
  brightnessSaveTimer = setTimeout(() => saveBrightness(value), 150);
}

async function saveBrightness(value) {
  const body = new URLSearchParams();
  body.set('value', value);

  try {
    const res = await fetch('/api/brightness?ts=' + Date.now(), {method:'POST',cache:'no-store',body});
    const text = await res.text();
    const data = JSON.parse(text);

    if (!res.ok || data.ok === false) {
      status(data.error || 'Brightness save failed');
      return;
    }

    setBrightnessUI(data.brightness);
    status('Brightness saved: ' + data.brightness);
  } catch (e) {
    status('Brightness save request failed');
  }
}

function setMicThresholdUI(value) {
  micThresholdValue = parseInt(value);
  document.getElementById('micThreshold').value = micThresholdValue;
  document.getElementById('micThresholdText').textContent = micThresholdValue;
}

function micThresholdChanged(value) {
  value = parseInt(value);
  setMicThresholdUI(value);

  clearTimeout(micSaveTimer);
  micSaveTimer = setTimeout(() => saveMicThreshold(value), 150);
}

async function saveMicThreshold(value) {
  const body = new URLSearchParams();
  body.set('threshold', value);

  try {
    const res = await fetch('/api/micconfig?ts=' + Date.now(), {method:'POST',cache:'no-store',body});
    const text = await res.text();
    const data = JSON.parse(text);

    if (!res.ok || data.ok === false) {
      status(data.error || 'Mic threshold save failed');
      return;
    }

    setMicThresholdUI(data.threshold);
    status('Mic threshold saved: ' + data.threshold);
  } catch (e) {
    status('Mic threshold save request failed');
  }
}

async function updateMicMeter() {
  try {
    const data = await fetch('/api/micstate?ts=' + Date.now(), {cache:'no-store'}).then(r => r.json());
    const level = data.level || 0;
    const active = !!data.active;
    const percent = Math.min(100, Math.round(level / Math.max(micThresholdValue * 2, 1) * 100));

    const meter = document.getElementById('micMeter');
    meter.style.width = percent + '%';
    meter.style.background = active ? '#ffcf58' : '#20cf8b';
    document.getElementById('micLevelText').textContent = 'level ' + level;
    document.getElementById('micActiveText').textContent = active ? 'VOICE ACTIVE' : '';
  } catch (e) {}
}

async function saveRandomConfig() {
  const enabled = document.getElementById('randomEnabled').checked;
  const interval = Math.max(1, Math.min(3600, parseInt(document.getElementById('randomInterval').value || '60')));
  document.getElementById('randomInterval').value = interval;

  const body = new URLSearchParams();
  body.set('enabled', enabled ? '1' : '0');
  body.set('interval', interval);

  const res = await fetch('/api/randomconfig?ts=' + Date.now(), {method:'POST',cache:'no-store',body});
  status(await res.text());
}

async function loadSavedList() {
  try {
    const data = await fetch('/api/list?ts=' + Date.now(), {cache:'no-store'}).then(r => r.json());

    if (!settingsLoaded) {
      setBrightnessUI(data.brightness ?? 64);
      setMicThresholdUI(data.micThreshold ?? 180);
      document.getElementById('randomEnabled').checked = !!data.randomEnabled;
      document.getElementById('randomInterval').value = data.randomIntervalSeconds ?? 60;
      settingsLoaded = true;
    }

    const leftImage = document.getElementById('leftEarPreview');
    const rightImage = document.getElementById('rightEarPreview');
    if (!data.leftEarPresent) leftImage.removeAttribute('src');
    if (!data.rightEarPresent) rightImage.removeAttribute('src');

    const list = document.getElementById('savedList');
    list.innerHTML = '';

    if (!data.animations || data.animations.length === 0) {
      list.innerHTML = '<div class="small">No saved HUB75 animations yet.</div>';
      return;
    }

    for (const item of data.animations) {
      const row = document.createElement('div');
      row.className = 'savedRow';

      const label = document.createElement('span');
      label.className = 'savedName';
      label.textContent = item.name + (item.name === data.default ? ' [default]' : '');
      row.appendChild(label);

      const mic = document.createElement('label');
      mic.className = 'flag';
      const micBox = document.createElement('input');
      micBox.type = 'checkbox';
      micBox.checked = !!item.mic;
      micBox.onchange = () => setAnimationFlag(item.name, 'mic', micBox.checked);
      mic.appendChild(micBox);
      mic.append('Mic Play');
      row.appendChild(mic);

      const random = document.createElement('label');
      random.className = 'flag';
      const randomBox = document.createElement('input');
      randomBox.type = 'checkbox';
      randomBox.checked = !!item.random;
      randomBox.onchange = () => setAnimationFlag(item.name, 'random', randomBox.checked);
      random.appendChild(randomBox);
      random.append('Random');
      row.appendChild(random);

      const load = document.createElement('button');
      load.textContent = 'Load';
      load.className = 'secondary';
      load.onclick = () => loadAnimationByName(item.name);
      row.appendChild(load);

      const play = document.createElement('button');
      play.textContent = 'Play';
      play.onclick = () => playAnimationByName(item.name);
      row.appendChild(play);

      const download = document.createElement('button');
      download.textContent = 'Download';
      download.className = 'secondary';
      download.onclick = () => downloadAnimation(item.name);
      row.appendChild(download);

      const def = document.createElement('button');
      def.textContent = 'Default';
      def.onclick = () => defaultAnimationByName(item.name);
      row.appendChild(def);

      const del = document.createElement('button');
      del.textContent = 'Delete';
      del.className = 'danger';
      del.onclick = () => deleteAnimation(item.name);
      row.appendChild(del);

      list.appendChild(row);
    }
  } catch (e) {
    status('Could not refresh saved animations');
  }
}

async function playAnimationByName(name) {
  const body = new URLSearchParams();
  body.set('name', name);
  const res = await fetch('/api/play?ts=' + Date.now(), {method:'POST',cache:'no-store',body});
  status(await res.text());
}

async function defaultAnimationByName(name) {
  const body = new URLSearchParams();
  body.set('name', name);
  const res = await fetch('/api/default?ts=' + Date.now(), {method:'POST',cache:'no-store',body});
  status(await res.text());
  await loadSavedList();
}

async function imageFileToRGB565(file) {
  const bitmap = await createImageBitmap(file);
  const work = document.createElement('canvas');
  work.width = 240;
  work.height = 240;
  const workCtx = work.getContext('2d');

  workCtx.fillStyle = '#000';
  workCtx.fillRect(0, 0, 240, 240);

  const scale = Math.max(240 / bitmap.width, 240 / bitmap.height);
  const width = bitmap.width * scale;
  const height = bitmap.height * scale;
  workCtx.drawImage(bitmap, (240 - width) / 2, (240 - height) / 2, width, height);

  const pixels = workCtx.getImageData(0, 0, 240, 240).data;
  const output = new Uint8Array(240 * 240 * 2);

  for (let p = 0, o = 0; p < pixels.length; p += 4, o += 2) {
    const r = pixels[p] >> 3;
    const g = pixels[p + 1] >> 2;
    const b = pixels[p + 2] >> 3;
    const rgb565 = (r << 11) | (g << 5) | b;

    // Little-endian, matching ESP32 uint16_t storage.
    output[o] = rgb565 & 0xFF;
    output[o + 1] = rgb565 >> 8;
  }

  return {bytes: output, preview: work.toDataURL('image/png')};
}

async function uploadRawEar(target, bytes) {
  const form = new FormData();
  form.append('image', new Blob([bytes], {type:'application/octet-stream'}), target + '.rgb565');

  const res = await fetch('/api/ear/' + target + '?ts=' + Date.now(), {
    method:'POST',
    cache:'no-store',
    body:form
  });

  const text = await res.text();
  if (!res.ok) throw new Error(text || 'Ear upload failed');
}

async function uploadEarImage() {
  const input = document.getElementById('earImageFile');
  const target = document.getElementById('earTarget').value;

  if (!input.files || input.files.length === 0) {
    status('Choose an image first');
    return;
  }

  try {
    status('Converting image...');
    const prepared = await imageFileToRGB565(input.files[0]);

    if (target === 'left' || target === 'both') {
      await uploadRawEar('left', prepared.bytes);
      document.getElementById('leftEarPreview').src = prepared.preview;
    }

    if (target === 'right' || target === 'both') {
      await uploadRawEar('right', prepared.bytes);
      document.getElementById('rightEarPreview').src = prepared.preview;
    }

    input.value = '';
    status('Ear image uploaded');
  } catch (e) {
    status(e.message || 'Could not upload ear image');
  }
}

async function clearEars() {
  const target = document.getElementById('earTarget').value;
  const body = new URLSearchParams();
  body.set('target', target);

  const res = await fetch('/api/ear/clear?ts=' + Date.now(), {method:'POST',cache:'no-store',body});
  status(await res.text());

  if (target === 'left' || target === 'both') document.getElementById('leftEarPreview').removeAttribute('src');
  if (target === 'right' || target === 'both') document.getElementById('rightEarPreview').removeAttribute('src');
}

setPanel('left');
refreshFrames();
drawEditor();
loadSavedList();
setInterval(updateMicMeter, 300);
</script>
</body>
</html>
)HTML";

void handleRoot() {
  sendNoCacheHeaders();
  server.send_P(200, "text/html", INDEX_HTML);
}

// ============================================================================
// SETUP / LOOP
// ============================================================================

void setup() {
  Serial.begin(115200);
  delay(400);

  Serial.println();
  Serial.println("Booting Protogen HUB75 + GC9A01 controller...");

  if (!LittleFS.begin(true)) {
    Serial.println("LittleFS mount failed.");
  } else {
    Serial.println("LittleFS mounted.");
  }

  loadBrightnessConfig();
  loadDefaultName();
  loadMicConfig();
  loadRandomConfig();

  initHub75();
  initEars();
  showAllEarImages();

  analogReadResolution(12);
  analogSetPinAttenuation(MIC_ADC_PIN, ADC_11db);

  long warmupSum = 0;
  for (uint16_t i = 0; i < 128; i++) {
    warmupSum += analogRead(MIC_ADC_PIN);
    delay(2);
  }
  micBaseline = warmupSum / 128.0f;

  randomSeed((uint32_t)esp_random());

  WiFi.disconnect(true);
  delay(250);

  WiFi.mode(WIFI_AP);

  IPAddress localIP(192, 168, 1, 1);
  IPAddress gateway(192, 168, 1, 1);
  IPAddress subnet(255, 255, 255, 0);

  WiFi.softAPConfig(localIP, gateway, subnet);
  bool apStarted = WiFi.softAP(AP_SSID, AP_PASS, 1, 0, 4);

  Serial.print("AP started: ");
  Serial.println(apStarted ? "YES" : "NO");
  Serial.print("Open: http://");
  Serial.print(WiFi.softAPIP());
  Serial.print(":");
  Serial.println(WEB_PORT);

  server.on("/", HTTP_GET, handleRoot);
  server.on("/test", HTTP_GET, []() {
    sendText(200, "Protogen HUB75 + GC9A01 controller is running");
  });

  server.on("/api/list", HTTP_GET, handleApiList);
  server.on("/api/load", HTTP_GET, handleApiLoad);
  server.on("/api/download", HTTP_GET, handleApiDownload);
  server.on("/api/micstate", HTTP_GET, handleApiMicState);

  server.on("/api/save", HTTP_POST, handleApiSave);
  server.on("/api/import", HTTP_POST, handleApiImport);
  server.on("/api/play", HTTP_POST, handleApiPlay);
  server.on("/api/stop", HTTP_POST, handleApiStop);
  server.on("/api/default", HTTP_POST, handleApiDefault);
  server.on("/api/delete", HTTP_POST, handleApiDelete);
  server.on("/api/preview", HTTP_POST, handleApiPreview);
  server.on("/api/brightness", HTTP_POST, handleApiBrightness);
  server.on("/api/micconfig", HTTP_POST, handleApiMicConfig);
  server.on("/api/randomconfig", HTTP_POST, handleApiRandomConfig);
  server.on("/api/micset", HTTP_POST, handleApiMicFlag);
  server.on("/api/randomset", HTTP_POST, handleApiRandomFlag);
  server.on("/api/ear/clear", HTTP_POST, handleApiClearEar);

  server.on(
    "/api/ear/left",
    HTTP_POST,
    []() { finishEarUpload(EAR_LEFT_PATH); },
    []() { handleEarUploadChunk(EAR_LEFT_PATH); }
  );

  server.on(
    "/api/ear/right",
    HTTP_POST,
    []() { finishEarUpload(EAR_RIGHT_PATH); },
    []() { handleEarUploadChunk(EAR_RIGHT_PATH); }
  );

  server.onNotFound([]() {
    sendText(404, "Not found");
  });

  server.begin();
  Serial.println("HTTP server started.");

  // Power-on behavior: default animation first. Randomization waits its full
  // configured interval before making its first selection.
  if (defaultAnimName.length() > 0 && loadAnimation(defaultAnimName)) {
    animPlaying = true;
    lastAnimMs = millis();
    Serial.print("Loaded power-on default: ");
    Serial.println(defaultAnimName);
  } else {
    clearHub();
    Serial.println("No power-on default animation is set.");
  }

  lastRandomSwitchMs = millis();
}

void loop() {
  server.handleClient();
  updateMicrophone();
  updateRandomization();
  updateAnimation();
}

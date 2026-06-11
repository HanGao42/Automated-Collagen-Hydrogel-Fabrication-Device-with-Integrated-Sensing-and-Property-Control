/*
  ESP32 Camera ROI Color Reader

  What it does:
  - Captures RGB565 frames from an ESP32 camera.
  - Measures the average color in the center region of the image.
  - Prints RGB, normalized RGB, and HSV values to the Serial Monitor.

  Arduino IDE:
  - Board: ESP32S3 Dev Module for the Freenove ESP32-S3 WROOM camera board.
  - Serial Monitor baud rate: 115200.
*/

#include <Arduino.h>
#include "esp_camera.h"

// ---------------------------------------------------------------------------
// Camera model
// ---------------------------------------------------------------------------
// This project currently uses the Freenove ESP32-S3 WROOM camera pinout.
// If you later use AI Thinker ESP32-CAM, comment this line and uncomment the
// CAMERA_MODEL_AI_THINKER line below.
#define CAMERA_MODEL_FREENOVE_ESP32S3_WROOM
// #define CAMERA_MODEL_AI_THINKER

#if defined(CAMERA_MODEL_FREENOVE_ESP32S3_WROOM)
#define PWDN_GPIO_NUM     -1
#define RESET_GPIO_NUM    -1
#define XCLK_GPIO_NUM     15
#define SIOD_GPIO_NUM      4
#define SIOC_GPIO_NUM      5
#define Y9_GPIO_NUM       16
#define Y8_GPIO_NUM       17
#define Y7_GPIO_NUM       18
#define Y6_GPIO_NUM       12
#define Y5_GPIO_NUM       10
#define Y4_GPIO_NUM        8
#define Y3_GPIO_NUM        9
#define Y2_GPIO_NUM       11
#define VSYNC_GPIO_NUM     6
#define HREF_GPIO_NUM      7
#define PCLK_GPIO_NUM     13
#define STATUS_LED_GPIO   48

#elif defined(CAMERA_MODEL_AI_THINKER)
#define PWDN_GPIO_NUM     32
#define RESET_GPIO_NUM    -1
#define XCLK_GPIO_NUM      0
#define SIOD_GPIO_NUM     26
#define SIOC_GPIO_NUM     27
#define Y9_GPIO_NUM       35
#define Y8_GPIO_NUM       34
#define Y7_GPIO_NUM       39
#define Y6_GPIO_NUM       36
#define Y5_GPIO_NUM       21
#define Y4_GPIO_NUM       19
#define Y3_GPIO_NUM       18
#define Y2_GPIO_NUM        5
#define VSYNC_GPIO_NUM    25
#define HREF_GPIO_NUM     23
#define PCLK_GPIO_NUM     22
#define STATUS_LED_GPIO    4
#else
#error "Please define a camera model."
#endif

// ---------------------------------------------------------------------------
// Measurement settings
// ---------------------------------------------------------------------------
static const framesize_t CAMERA_FRAME_SIZE = FRAMESIZE_QQVGA; // 160 x 120
static const pixformat_t CAMERA_PIXEL_FORMAT = PIXFORMAT_RGB565;
static const int CAMERA_XCLK_FREQ_HZ = 10000000;

static const uint32_t CAPTURE_INTERVAL_MS = 1500;
static const uint8_t FRAMES_TO_AVERAGE = 5;
static const uint8_t WARMUP_FRAME_COUNT = 8;

// The measured area is centered in the image.
static const uint8_t ROI_WIDTH_PERCENT = 45;
static const uint8_t ROI_HEIGHT_PERCENT = 45;
static const int ROI_OFFSET_X_PIXELS = 0;
static const int ROI_OFFSET_Y_PIXELS = 0;

// 1 = use every pixel. 2 = use every other pixel, faster and still stable.
static const uint8_t SAMPLE_STRIDE = 2;

// For color calibration, stable camera settings are more important than pretty
// images. The camera warms up with auto settings, then locks them.
static const bool LOCK_AUTO_SETTINGS_AFTER_WARMUP = true;

// Use this only if glare or very dark pixels are a problem.
static const bool USE_BRIGHTNESS_REJECTION = false;
static const uint8_t MIN_VALID_BRIGHTNESS = 15;
static const uint8_t MAX_VALID_BRIGHTNESS = 245;

static const bool RGB565_BIG_ENDIAN = true;
static const bool SWAP_RED_BLUE = false;
static const bool CAMERA_VERTICAL_FLIP = true;

static const char *CAMERA_ON_COMMAND = "CameraOn";
static const char *CAMERA_OFF_COMMAND = "CameraOff";
static const char *START_COMMAND = "Start"; // Backward-compatible alias for CameraOn.
static const char *STOP_COMMAND = "Stop";   // Backward-compatible alias for CameraOff.

// ---------------------------------------------------------------------------
// Data structures
// ---------------------------------------------------------------------------
struct RoiRect {
  int x;
  int y;
  int w;
  int h;
};

struct ColorAccumulator {
  uint64_t rSum;
  uint64_t gSum;
  uint64_t bSum;
  uint32_t pixelCount;
};

struct ColorResult {
  float r;
  float g;
  float b;
  float hDeg;
  float sPercent;
  float vPercent;
  uint32_t pixelCount;
  bool valid;
};

static uint32_t measurementIndex = 0;
static bool measurementRunning = false;
static bool cameraInitialized = false;
static String serialCommandBuffer = "";

void setStatusLed(bool on) {
#if defined(CAMERA_MODEL_FREENOVE_ESP32S3_WROOM)
  // Freenove GPIO48 is an onboard WS2812 status LED.
  if (on) {
    neopixelWrite(STATUS_LED_GPIO, 80, 80, 80);
  } else {
    neopixelWrite(STATUS_LED_GPIO, 0, 0, 0);
  }
#else
  pinMode(STATUS_LED_GPIO, OUTPUT);
  digitalWrite(STATUS_LED_GPIO, on ? HIGH : LOW);
#endif
}

// ---------------------------------------------------------------------------
// Utility functions
// ---------------------------------------------------------------------------
int clampInt(int value, int low, int high) {
  if (value < low) return low;
  if (value > high) return high;
  return value;
}

void decodeRgb565(const uint8_t *buffer, size_t index, uint8_t &r, uint8_t &g, uint8_t &b) {
  uint16_t pixel;
  if (RGB565_BIG_ENDIAN) {
    pixel = ((uint16_t)buffer[index] << 8) | buffer[index + 1];
  } else {
    pixel = ((uint16_t)buffer[index + 1] << 8) | buffer[index];
  }

  uint8_t r5 = (pixel >> 11) & 0x1F;
  uint8_t g6 = (pixel >> 5) & 0x3F;
  uint8_t b5 = pixel & 0x1F;

  r = (r5 * 255 + 15) / 31;
  g = (g6 * 255 + 31) / 63;
  b = (b5 * 255 + 15) / 31;

  if (SWAP_RED_BLUE) {
    uint8_t temp = r;
    r = b;
    b = temp;
  }
}

void rgbToHsv(float r, float g, float b, float &hDeg, float &sPercent, float &vPercent) {
  float rf = r / 255.0f;
  float gf = g / 255.0f;
  float bf = b / 255.0f;

  float maxValue = max(rf, max(gf, bf));
  float minValue = min(rf, min(gf, bf));
  float delta = maxValue - minValue;

  float h = 0.0f;
  if (delta > 0.0001f) {
    if (maxValue == rf) {
      h = 60.0f * fmodf((gf - bf) / delta, 6.0f);
    } else if (maxValue == gf) {
      h = 60.0f * (((bf - rf) / delta) + 2.0f);
    } else {
      h = 60.0f * (((rf - gf) / delta) + 4.0f);
    }
  }

  if (h < 0.0f) {
    h += 360.0f;
  }

  float s = (maxValue <= 0.0001f) ? 0.0f : (delta / maxValue);

  hDeg = h;
  sPercent = s * 100.0f;
  vPercent = maxValue * 100.0f;
}

RoiRect makeCenteredRoi(int frameWidth, int frameHeight) {
  int roiW = max(1, (frameWidth * ROI_WIDTH_PERCENT) / 100);
  int roiH = max(1, (frameHeight * ROI_HEIGHT_PERCENT) / 100);

  int roiX = (frameWidth - roiW) / 2 + ROI_OFFSET_X_PIXELS;
  int roiY = (frameHeight - roiH) / 2 + ROI_OFFSET_Y_PIXELS;

  roiX = clampInt(roiX, 0, frameWidth - roiW);
  roiY = clampInt(roiY, 0, frameHeight - roiH);

  RoiRect roi = { roiX, roiY, roiW, roiH };
  return roi;
}

bool addFrameToAccumulator(camera_fb_t *fb, const RoiRect &roi, ColorAccumulator &acc) {
  if (!fb || !fb->buf) return false;
  if (fb->format != PIXFORMAT_RGB565) return false;

  int width = (int)fb->width;
  int height = (int)fb->height;
  int stride = max((int)SAMPLE_STRIDE, 1);

  int xEnd = clampInt(roi.x + roi.w, 0, width);
  int yEnd = clampInt(roi.y + roi.h, 0, height);

  for (int y = roi.y; y < yEnd; y += stride) {
    for (int x = roi.x; x < xEnd; x += stride) {
      size_t index = (((size_t)y * (size_t)width) + (size_t)x) * 2;
      if (index + 1 >= fb->len) continue;

      uint8_t r, g, b;
      decodeRgb565(fb->buf, index, r, g, b);

      uint8_t brightness = max(r, max(g, b));
      if (USE_BRIGHTNESS_REJECTION &&
          (brightness < MIN_VALID_BRIGHTNESS || brightness > MAX_VALID_BRIGHTNESS)) {
        continue;
      }

      acc.rSum += r;
      acc.gSum += g;
      acc.bSum += b;
      acc.pixelCount++;
    }
  }

  return true;
}

ColorResult finishResult(const ColorAccumulator &acc) {
  ColorResult result;
  result.valid = acc.pixelCount > 0;
  result.pixelCount = acc.pixelCount;

  if (!result.valid) {
    result.r = 0.0f;
    result.g = 0.0f;
    result.b = 0.0f;
    result.hDeg = 0.0f;
    result.sPercent = 0.0f;
    result.vPercent = 0.0f;
    return result;
  }

  result.r = (float)acc.rSum / (float)acc.pixelCount;
  result.g = (float)acc.gSum / (float)acc.pixelCount;
  result.b = (float)acc.bSum / (float)acc.pixelCount;
  rgbToHsv(result.r, result.g, result.b, result.hDeg, result.sPercent, result.vPercent);
  return result;
}

void printSerialHeader() {
  Serial.println();
  Serial.println("ESP32 Camera ROI Color Reader");
  Serial.println("Move the sample into the center of the camera image.");
  Serial.println("Output: average color only. pH still needs calibration data.");
  Serial.println("Camera is OFF by default.");
  Serial.println("Send CameraOn to initialise the camera and begin measuring.");
  Serial.println("Send CameraOff to stop measuring and release the camera.");
  Serial.println();
  Serial.println("CSV columns:");
  Serial.println("CSV,sample,ms,frames,width,height,roi_x,roi_y,roi_w,roi_h,pixels,r_avg,g_avg,b_avg,h_deg,s_pct,v_pct,r_norm,g_norm,b_norm");
  Serial.println();
}

void handleCommand(String command) {
  command.trim();
  if (command.length() == 0) return;

  if (command.equalsIgnoreCase(CAMERA_ON_COMMAND) || command.equalsIgnoreCase(START_COMMAND)) {
    startCameraMeasurement();
    return;
  }

  if (command.equalsIgnoreCase(CAMERA_OFF_COMMAND) || command.equalsIgnoreCase(STOP_COMMAND)) {
    stopCameraMeasurement();
    return;
  }

  Serial.print("ERROR: Unknown command: ");
  Serial.println(command);
  Serial.println("Use CameraOn or CameraOff.");
}

void pollSerialCommands() {
  while (Serial.available() > 0) {
    char ch = (char)Serial.read();

    if (ch == '\n' || ch == '\r') {
      handleCommand(serialCommandBuffer);
      serialCommandBuffer = "";
      continue;
    }

    if (serialCommandBuffer.length() < 32) {
      serialCommandBuffer += ch;
    } else {
      serialCommandBuffer = "";
      Serial.println("ERROR: Command too long. Use CameraOn or CameraOff.");
    }
  }
}

void printMeasurement(const ColorResult &result, const RoiRect &roi, int width, int height, uint32_t elapsedMs) {
  if (!result.valid) {
    Serial.println("ERROR: No valid ROI pixels measured.");
    return;
  }

  float rNorm = result.r / 255.0f;
  float gNorm = result.g / 255.0f;
  float bNorm = result.b / 255.0f;

  Serial.printf(
    "Sample %lu | RGB avg=(%.1f, %.1f, %.1f) | HSV=(%.1f deg, %.1f%%, %.1f%%) | ROI=(%d,%d,%d,%d) | pixels=%lu | pH=not calibrated\n",
    (unsigned long)measurementIndex,
    result.r, result.g, result.b,
    result.hDeg, result.sPercent, result.vPercent,
    roi.x, roi.y, roi.w, roi.h,
    (unsigned long)result.pixelCount
  );

  Serial.printf(
    "CSV,%lu,%lu,%u,%d,%d,%d,%d,%d,%d,%lu,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.4f,%.4f,%.4f\n",
    (unsigned long)measurementIndex,
    (unsigned long)millis(),
    (unsigned int)FRAMES_TO_AVERAGE,
    width, height,
    roi.x, roi.y, roi.w, roi.h,
    (unsigned long)result.pixelCount,
    result.r, result.g, result.b,
    result.hDeg, result.sPercent, result.vPercent,
    rNorm, gNorm, bNorm
  );

  Serial.printf("Measurement time: %lu ms\n", (unsigned long)elapsedMs);
  Serial.println();
}

// ---------------------------------------------------------------------------
// Camera setup
// ---------------------------------------------------------------------------
bool initCamera() {
  camera_config_t config;
  memset(&config, 0, sizeof(config));

  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer = LEDC_TIMER_0;
  config.pin_d0 = Y2_GPIO_NUM;
  config.pin_d1 = Y3_GPIO_NUM;
  config.pin_d2 = Y4_GPIO_NUM;
  config.pin_d3 = Y5_GPIO_NUM;
  config.pin_d4 = Y6_GPIO_NUM;
  config.pin_d5 = Y7_GPIO_NUM;
  config.pin_d6 = Y8_GPIO_NUM;
  config.pin_d7 = Y9_GPIO_NUM;
  config.pin_xclk = XCLK_GPIO_NUM;
  config.pin_pclk = PCLK_GPIO_NUM;
  config.pin_vsync = VSYNC_GPIO_NUM;
  config.pin_href = HREF_GPIO_NUM;
  config.pin_sccb_sda = SIOD_GPIO_NUM;
  config.pin_sccb_scl = SIOC_GPIO_NUM;
  config.pin_pwdn = PWDN_GPIO_NUM;
  config.pin_reset = RESET_GPIO_NUM;

  config.xclk_freq_hz = CAMERA_XCLK_FREQ_HZ;
  config.pixel_format = CAMERA_PIXEL_FORMAT;
  config.frame_size = CAMERA_FRAME_SIZE;
  config.jpeg_quality = 12;
  config.fb_count = 1;
  config.fb_location = psramFound() ? CAMERA_FB_IN_PSRAM : CAMERA_FB_IN_DRAM;
  config.grab_mode = CAMERA_GRAB_WHEN_EMPTY;

  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) {
    Serial.printf("Camera init failed with error 0x%x\n", err);
    return false;
  }

  sensor_t *sensor = esp_camera_sensor_get();
  if (sensor) {
    sensor->set_brightness(sensor, 0);
    sensor->set_contrast(sensor, 0);
    sensor->set_saturation(sensor, 0);
    sensor->set_special_effect(sensor, 0);
    sensor->set_vflip(sensor, CAMERA_VERTICAL_FLIP ? 1 : 0);

    sensor->set_whitebal(sensor, 1);
    sensor->set_awb_gain(sensor, 1);
    sensor->set_exposure_ctrl(sensor, 1);
    sensor->set_gain_ctrl(sensor, 1);
  }

  return true;
}

void discardWarmupFrames(uint8_t count) {
  for (uint8_t i = 0; i < count; i++) {
    camera_fb_t *fb = esp_camera_fb_get();
    if (fb) {
      esp_camera_fb_return(fb);
    }
    delay(120);
  }
}

void lockAutoSettingsIfRequested() {
  if (!LOCK_AUTO_SETTINGS_AFTER_WARMUP) return;

  sensor_t *sensor = esp_camera_sensor_get();
  if (!sensor) return;

  sensor->set_whitebal(sensor, 0);
  sensor->set_awb_gain(sensor, 0);
  sensor->set_exposure_ctrl(sensor, 0);
  sensor->set_gain_ctrl(sensor, 0);

  Serial.println("Auto exposure, gain, and white balance locked after warmup.");
}

bool startCameraMeasurement() {
  setStatusLed(true);

  if (measurementRunning && cameraInitialized) {
    Serial.println("OK: Camera already ON and measuring.");
    return true;
  }

  if (!cameraInitialized) {
    Serial.println("Initialising camera...");
    if (!initCamera()) {
      Serial.println("ERROR: Camera init failed. Check wiring, pins, board choice, and power.");
      measurementRunning = false;
      cameraInitialized = false;
      setStatusLed(false);
      return false;
    }

    cameraInitialized = true;
    Serial.println("Camera ready. Warming up...");
    discardWarmupFrames(WARMUP_FRAME_COUNT);
    lockAutoSettingsIfRequested();
    discardWarmupFrames(2);
  }

  measurementRunning = true;
  Serial.println("OK: CameraOn received. Color measurement is running.");
  return true;
}

void stopCameraMeasurement() {
  setStatusLed(false);

  if (measurementRunning) {
    measurementRunning = false;
  }

  if (cameraInitialized) {
    esp_camera_deinit();
    cameraInitialized = false;
    Serial.println("OK: CameraOff received. Camera is OFF.");
    return;
  }

  Serial.println("OK: Camera already OFF.");
}

// ---------------------------------------------------------------------------
// Main measurement loop
// ---------------------------------------------------------------------------
void performMeasurementBatch() {
  if (!cameraInitialized) {
    Serial.println("ERROR: Camera is OFF. Send CameraOn first.");
    measurementRunning = false;
    return;
  }

  measurementIndex++;
  uint32_t startMs = millis();

  ColorAccumulator acc = { 0, 0, 0, 0 };
  RoiRect roi = { 0, 0, 0, 0 };
  int frameWidth = 0;
  int frameHeight = 0;
  uint8_t goodFrames = 0;

  for (uint8_t frameNumber = 0; frameNumber < FRAMES_TO_AVERAGE; frameNumber++) {
    camera_fb_t *fb = esp_camera_fb_get();
    if (!fb) {
      Serial.println("Camera capture failed.");
      delay(100);
      continue;
    }

    frameWidth = (int)fb->width;
    frameHeight = (int)fb->height;
    roi = makeCenteredRoi(frameWidth, frameHeight);

    if (addFrameToAccumulator(fb, roi, acc)) {
      goodFrames++;
    } else {
      Serial.println("Unsupported frame format or invalid frame.");
    }

    esp_camera_fb_return(fb);
    delay(60);
  }

  if (goodFrames == 0) {
    Serial.println("No good frames in this batch.");
    return;
  }

  ColorResult result = finishResult(acc);
  uint32_t elapsedMs = millis() - startMs;
  printMeasurement(result, roi, frameWidth, frameHeight, elapsedMs);
}

void setup() {
  Serial.begin(115200);
  delay(1200);
  setStatusLed(false);

  printSerialHeader();

  Serial.printf("PSRAM: %s\n", psramFound() ? "found" : "not found");
  Serial.println("Camera is OFF.");
  Serial.println("Waiting for CameraOn command...");
  Serial.println();
}

void loop() {
  pollSerialCommands();

  if (measurementRunning) {
    performMeasurementBatch();
    uint32_t waitStartMs = millis();
    while (millis() - waitStartMs < CAPTURE_INTERVAL_MS) {
      pollSerialCommands();
      delay(20);
    }
  } else {
    delay(20);
  }
}

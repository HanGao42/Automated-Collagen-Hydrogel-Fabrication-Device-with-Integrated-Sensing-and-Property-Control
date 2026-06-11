/*
  ESP32 ROI Colour Detector / ESP32 感兴趣区域颜色检测

  Purpose / 用途:
  - Capture RGB565 frames from an ESP32 camera without Wi-Fi or a web server.
    不使用 Wi-Fi 或网页服务器，直接从 ESP32 摄像头采集 RGB565 图像。
  - Measure the average colour inside a fixed centre ROI.
    在固定中心 ROI（感兴趣区域）内计算平均颜色。
  - Print stable numerical RGB and HSV values to Serial Monitor.
    通过串口监视器输出稳定的 RGB 和 HSV 数值。

  Default hardware / 默认硬件:
  - Freenove ESP32-S3 WROOM Board camera example pinout.
    Freenove ESP32-S3 WROOM 开发板摄像头示例引脚。

  Notes / 说明:
  - Keep lighting, camera distance, sample container, and ROI position fixed.
    保持光照、摄像头距离、样品容器和 ROI 位置固定。
  - This sketch measures colour only. It does not estimate pH yet.
    本程序只测量颜色，暂时不估计 pH。
  - In Arduino IDE, select "ESP32S3 Dev Module", not "AI Thinker ESP32-CAM".
    在 Arduino IDE 中请选择 "ESP32S3 Dev Module"，不要选择 "AI Thinker ESP32-CAM"。
*/

#include <Arduino.h>
#include "esp_camera.h"

// Optional SD saving is disabled by default.
// 默认关闭 SD 保存功能，保持程序简单稳定。
#ifndef SAVE_ROI_PPM_TO_SD
#define SAVE_ROI_PPM_TO_SD 0
#endif

#if SAVE_ROI_PPM_TO_SD
#include "FS.h"
#include "SD_MMC.h"
#endif

// ---------------------------------------------------------------------------
// 1. Select camera model / 选择摄像头型号
// ---------------------------------------------------------------------------
// Default is Freenove ESP32-S3 WROOM Board.
// The Freenove tutorial camera sketches use CAMERA_MODEL_ESP32S3_EYE.
// 默认使用 Freenove ESP32-S3 WROOM 开发板。
// Freenove 教程中的摄像头示例使用 CAMERA_MODEL_ESP32S3_EYE。
#define CAMERA_MODEL_FREENOVE_ESP32S3_WROOM

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

// Freenove's onboard WS2812 status LED is GPIO48.
// Freenove 板载 WS2812 状态灯为 GPIO48。
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
#error "Please define your ESP32 camera model pins. 请先定义你的 ESP32 摄像头引脚。"
#endif

// ---------------------------------------------------------------------------
// 2. Adjustable parameters / 可调参数
// ---------------------------------------------------------------------------

// Resolution / 分辨率:
// RGB565 uses 2 bytes per pixel. QQVGA is stable on most ESP32-CAM boards.
// RGB565 每像素 2 字节。QQVGA 对大多数 ESP32-CAM 更稳定。
static const framesize_t CAMERA_FRAME_SIZE = FRAMESIZE_QQVGA; // 160x120
// If your board has PSRAM and is stable, you can try FRAMESIZE_QVGA (320x240).
// 如果你的板子有 PSRAM 且运行稳定，可以尝试 FRAMESIZE_QVGA (320x240)。

// Pixel format / 像素格式:
// RGB565 avoids JPEG decoding and is easier for direct colour measurement.
// RGB565 不需要 JPEG 解码，更适合直接测量颜色。
static const pixformat_t CAMERA_PIXEL_FORMAT = PIXFORMAT_RGB565;

// Freenove camera examples use 10 MHz XCLK on this ESP32-S3 camera board.
// Freenove 摄像头示例在该 ESP32-S3 摄像头板上使用 10 MHz XCLK。
static const int CAMERA_XCLK_FREQ_HZ = 10000000;

// Capture interval after each measurement batch.
// 每次完成一组测量后的等待时间。
static const uint32_t CAPTURE_INTERVAL_MS = 1500;

// Number of frames averaged into one printed result.
// 每次输出前平均多少帧，数值越大越稳定但越慢。
static const uint8_t FRAMES_TO_AVERAGE = 5;

// Frames discarded during startup so exposure and white balance can settle.
// 启动时丢弃若干帧，让自动曝光和白平衡先稳定。
static const uint8_t WARMUP_FRAME_COUNT = 8;

// ROI size as a percentage of the frame, centred by default.
// ROI 尺寸，用整张图像百分比表示，默认位于中心。
static const uint8_t ROI_WIDTH_PERCENT = 45;
static const uint8_t ROI_HEIGHT_PERCENT = 45;

// ROI centre offset in pixels. Use these if the liquid is not exactly centred.
// ROI 中心偏移像素。如果液体区域不在正中心，可以调整这里。
static const int ROI_OFFSET_X_PIXELS = 0;
static const int ROI_OFFSET_Y_PIXELS = 0;

// Process every Nth pixel inside ROI. 1 = use all pixels, 2 = every other pixel.
// ROI 内每隔 N 个像素采样。1 表示全部像素，2 表示隔一个像素采样。
static const uint8_t SAMPLE_STRIDE = 2;

// Lock auto exposure/gain/white balance after warmup.
// 预热后锁定自动曝光、增益和白平衡。
// For calibration, true is usually better under fixed lighting.
// 对于后续标定，在固定光照下通常建议 true。
static const bool LOCK_AUTO_SETTINGS_AFTER_WARMUP = true;

// Optional simple outlier rejection for very dark pixels or glare.
// 可选：忽略过暗像素或强反光像素。
static const bool USE_BRIGHTNESS_REJECTION = false;
static const uint8_t MIN_VALID_BRIGHTNESS = 15;  // 0-255
static const uint8_t MAX_VALID_BRIGHTNESS = 245; // 0-255

// RGB565 byte order. Most ESP32 camera buffers work with true here.
// RGB565 字节顺序。大多数 ESP32 摄像头缓冲区使用 true。
static const bool RGB565_BIG_ENDIAN = true;

// If red and blue appear swapped in your output, change this to true.
// 如果输出中红色和蓝色明显反了，把这里改成 true。
static const bool SWAP_RED_BLUE = false;

// Flip camera image vertically. Freenove camera examples commonly enable this.
// 是否上下翻转摄像头图像。Freenove 摄像头示例通常会开启。
static const bool CAMERA_VERTICAL_FLIP = true;

// Onboard status LED. Do not use it as a colour-measurement light source.
// 板载状态灯。不要把它作为颜色测量光源。
static const bool USE_ONBOARD_STATUS_LED = false;

// SD saving options. Only used when SAVE_ROI_PPM_TO_SD is set to 1.
// SD 保存选项。只有 SAVE_ROI_PPM_TO_SD 设置为 1 时才使用。
static const uint16_t SAVE_EVERY_N_MEASUREMENTS = 10;

// ---------------------------------------------------------------------------
// 3. Data structures / 数据结构
// ---------------------------------------------------------------------------

struct RoiRect {
  int x;
  int y;
  int w;
  int h;
};

struct ColourAccumulator {
  uint64_t rSum;
  uint64_t gSum;
  uint64_t bSum;
  uint32_t pixelCount;
};

struct ColourResult {
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
static bool sdReady = false;

// ---------------------------------------------------------------------------
// 4. Utility functions / 工具函数
// ---------------------------------------------------------------------------

int clampInt(int value, int low, int high) {
  if (value < low) return low;
  if (value > high) return high;
  return value;
}

void decodeRgb565(const uint8_t *buffer, size_t index, uint8_t &r, uint8_t &g, uint8_t &b) {
  // Convert two RGB565 bytes into 8-bit R/G/B.
  // 将两个 RGB565 字节转换为 8 位 R/G/B。
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
  // Convert average RGB to HSV.
  // 将平均 RGB 转换为 HSV。
  float rf = r / 255.0f;
  float gf = g / 255.0f;
  float bf = b / 255.0f;

  float maxValue = max(rf, max(gf, bf));
  float minValue = min(rf, min(gf, bf));
  float delta = maxValue - minValue;

  float h = 0.0f;
  if (delta > 0.0001f) {
    if (maxValue == rf) {
      h = 60.0f * fmodf(((gf - bf) / delta), 6.0f);
    } else if (maxValue == gf) {
      h = 60.0f * (((bf - rf) / delta) + 2.0f);
    } else {
      h = 60.0f * (((rf - gf) / delta) + 4.0f);
    }
  }
  if (h < 0.0f) h += 360.0f;

  float s = (maxValue <= 0.0001f) ? 0.0f : (delta / maxValue);

  hDeg = h;
  sPercent = s * 100.0f;
  vPercent = maxValue * 100.0f;
}

RoiRect makeCenteredRoi(int frameWidth, int frameHeight) {
  // Build a centred ROI and keep it inside the frame.
  // 创建中心 ROI，并确保不超出图像边界。
  int roiW = max(1, (frameWidth * ROI_WIDTH_PERCENT) / 100);
  int roiH = max(1, (frameHeight * ROI_HEIGHT_PERCENT) / 100);

  int roiX = (frameWidth - roiW) / 2 + ROI_OFFSET_X_PIXELS;
  int roiY = (frameHeight - roiH) / 2 + ROI_OFFSET_Y_PIXELS;

  roiX = clampInt(roiX, 0, frameWidth - roiW);
  roiY = clampInt(roiY, 0, frameHeight - roiH);

  RoiRect roi = { roiX, roiY, roiW, roiH };
  return roi;
}

bool addFrameToAccumulator(camera_fb_t *fb, const RoiRect &roi, ColourAccumulator &acc) {
  // Analyse only the ROI, not the full frame.
  // 只分析 ROI，不处理整张图像。
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
      if (USE_BRIGHTNESS_REJECTION) {
        if (brightness < MIN_VALID_BRIGHTNESS || brightness > MAX_VALID_BRIGHTNESS) {
          continue;
        }
      }

      acc.rSum += r;
      acc.gSum += g;
      acc.bSum += b;
      acc.pixelCount++;
    }
  }

  return true;
}

ColourResult finishResult(const ColourAccumulator &acc) {
  ColourResult result;
  result.valid = acc.pixelCount > 0;
  result.pixelCount = acc.pixelCount;

  if (!result.valid) {
    result.r = result.g = result.b = 0.0f;
    result.hDeg = result.sPercent = result.vPercent = 0.0f;
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
  Serial.println("ESP32 ROI Colour Detector / ESP32 ROI 颜色检测");
  Serial.println("Output is numeric colour data only. pH is not calibrated yet.");
  Serial.println("输出为数值颜色数据。当前尚未进行 pH 标定。");
  Serial.println();
  Serial.println("CSV columns / CSV 列:");
  Serial.println("CSV,sample,ms,frames,width,height,roi_x,roi_y,roi_w,roi_h,pixels,r_avg,g_avg,b_avg,h_deg,s_pct,v_pct,r_norm,g_norm,b_norm");
  Serial.println();
}

void printMeasurement(const ColourResult &result, const RoiRect &roi, int width, int height, uint32_t elapsedMs) {
  if (!result.valid) {
    Serial.println("ERROR: No valid ROI pixels measured. / 错误：ROI 内没有有效像素。");
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

  Serial.printf("Measurement time / 测量耗时: %lu ms\n", (unsigned long)elapsedMs);
  Serial.println();
}

// ---------------------------------------------------------------------------
// 5. Optional SD helper / 可选 SD 保存函数
// ---------------------------------------------------------------------------

#if SAVE_ROI_PPM_TO_SD
bool initSdCard() {
  // Use 1-bit mode to reduce pin conflicts on ESP32-CAM.
  // 使用 1-bit 模式，减少 ESP32-CAM 上的引脚冲突。
  if (!SD_MMC.begin("/sdcard", true)) {
    Serial.println("SD_MMC init failed. ROI image saving disabled. / SD 初始化失败，关闭图像保存。");
    return false;
  }
  Serial.println("SD card ready. / SD 卡已准备好。");
  return true;
}

void saveRoiAsPpm(camera_fb_t *fb, const RoiRect &roi) {
  // Save ROI as a simple binary PPM image.
  // 将 ROI 保存为简单的二进制 PPM 图片。
  if (!sdReady || !fb || fb->format != PIXFORMAT_RGB565) return;
  if ((measurementIndex % SAVE_EVERY_N_MEASUREMENTS) != 0) return;

  char path[40];
  snprintf(path, sizeof(path), "/roi_%06lu.ppm", (unsigned long)measurementIndex);

  File file = SD_MMC.open(path, FILE_WRITE);
  if (!file) {
    Serial.println("Failed to open PPM file. / 无法创建 PPM 文件。");
    return;
  }

  file.printf("P6\n%d %d\n255\n", roi.w, roi.h);

  int width = (int)fb->width;
  for (int y = roi.y; y < roi.y + roi.h; y++) {
    for (int x = roi.x; x < roi.x + roi.w; x++) {
      size_t index = (((size_t)y * (size_t)width) + (size_t)x) * 2;
      if (index + 1 >= fb->len) continue;

      uint8_t rgb[3];
      decodeRgb565(fb->buf, index, rgb[0], rgb[1], rgb[2]);
      file.write(rgb, 3);
    }
  }

  file.close();
  Serial.printf("Saved ROI image / 已保存 ROI 图片: %s\n", path);
}
#endif

// ---------------------------------------------------------------------------
// 6. Camera setup / 摄像头设置
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

  // One frame buffer is slower but more deterministic for measurement.
  // 单缓冲速度较慢，但更适合稳定测量。
  config.fb_count = 1;
  config.fb_location = psramFound() ? CAMERA_FB_IN_PSRAM : CAMERA_FB_IN_DRAM;
  config.grab_mode = CAMERA_GRAB_WHEN_EMPTY;

  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) {
    Serial.printf("Camera init failed with error 0x%x / 摄像头初始化失败: 0x%x\n", err, err);
    return false;
  }

  sensor_t *sensor = esp_camera_sensor_get();
  if (sensor) {
    // Neutral image controls. Keep these fixed for repeatable measurements.
    // 中性图像参数。为了测量可重复性，建议保持固定。
    sensor->set_brightness(sensor, 0);
    sensor->set_contrast(sensor, 0);
    sensor->set_saturation(sensor, 0);
    sensor->set_special_effect(sensor, 0);
    sensor->set_vflip(sensor, CAMERA_VERTICAL_FLIP ? 1 : 0);

    // Let automatic controls work during warmup, then optionally lock them.
    // 预热阶段允许自动控制工作，之后可选择锁定。
    sensor->set_whitebal(sensor, 1);
    sensor->set_awb_gain(sensor, 1);
    sensor->set_exposure_ctrl(sensor, 1);
    sensor->set_gain_ctrl(sensor, 1);
  }

  return true;
}

void discardWarmupFrames(uint8_t count) {
  // Capture and discard frames so the camera settles before measuring.
  // 采集并丢弃若干帧，让摄像头在测量前稳定。
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

  // Disable automatic changes after warmup.
  // 预热后关闭自动变化，减少数值漂移。
  sensor->set_whitebal(sensor, 0);
  sensor->set_awb_gain(sensor, 0);
  sensor->set_exposure_ctrl(sensor, 0);
  sensor->set_gain_ctrl(sensor, 0);

  Serial.println("Auto exposure/gain/white balance locked after warmup.");
  Serial.println("预热后已锁定自动曝光、增益和白平衡。");
}

// ---------------------------------------------------------------------------
// 7. Measurement loop / 测量循环
// ---------------------------------------------------------------------------

void performMeasurementBatch() {
  measurementIndex++;
  uint32_t startMs = millis();

  ColourAccumulator acc = { 0, 0, 0, 0 };
  RoiRect roi = { 0, 0, 0, 0 };
  int frameWidth = 0;
  int frameHeight = 0;
  uint8_t goodFrames = 0;

  for (uint8_t frameNumber = 0; frameNumber < FRAMES_TO_AVERAGE; frameNumber++) {
    camera_fb_t *fb = esp_camera_fb_get();
    if (!fb) {
      Serial.println("Camera capture failed. / 摄像头采集失败。");
      delay(100);
      continue;
    }

    frameWidth = (int)fb->width;
    frameHeight = (int)fb->height;
    roi = makeCenteredRoi(frameWidth, frameHeight);

    bool ok = addFrameToAccumulator(fb, roi, acc);
    if (ok) {
      goodFrames++;
    } else {
      Serial.println("Unsupported frame format or invalid frame. / 不支持的帧格式或无效图像帧。");
    }

#if SAVE_ROI_PPM_TO_SD
    if (frameNumber == 0) {
      saveRoiAsPpm(fb, roi);
    }
#endif

    // Important: return the frame buffer immediately after processing.
    // 重要：处理完成后立刻归还帧缓冲，避免内存耗尽。
    esp_camera_fb_return(fb);

    delay(60);
  }

  if (goodFrames == 0) {
    Serial.println("No good frames in this batch. / 本次测量没有有效图像帧。");
    return;
  }

  ColourResult result = finishResult(acc);
  uint32_t elapsedMs = millis() - startMs;
  printMeasurement(result, roi, frameWidth, frameHeight, elapsedMs);
}

void setup() {
  Serial.begin(115200);
  delay(1200);

  printSerialHeader();

  Serial.printf("PSRAM: %s / PSRAM 状态: %s\n", psramFound() ? "found" : "not found", psramFound() ? "已检测到" : "未检测到");
  Serial.println("Initialising camera... / 正在初始化摄像头...");

  if (USE_ONBOARD_STATUS_LED) {
    pinMode(STATUS_LED_GPIO, OUTPUT);
    digitalWrite(STATUS_LED_GPIO, HIGH);
    Serial.println("Onboard status LED is ON. Do not use it as measurement lighting.");
    Serial.println("板载状态灯已开启。不要把它作为测量光源。");
  }

  if (!initCamera()) {
    Serial.println("Fix camera wiring/pins/power, then reset the board.");
    Serial.println("请检查摄像头排线、引脚和供电，然后重启开发板。");
    while (true) {
      delay(1000);
    }
  }

  Serial.println("Camera ready. Warming up... / 摄像头已就绪，正在预热...");
  discardWarmupFrames(WARMUP_FRAME_COUNT);
  lockAutoSettingsIfRequested();
  discardWarmupFrames(2);

#if SAVE_ROI_PPM_TO_SD
  sdReady = initSdCard();
#else
  sdReady = false;
#endif

  Serial.println("Colour measurement started. / 颜色测量已开始。");
  Serial.println();
}

void loop() {
  performMeasurementBatch();
  delay(CAPTURE_INTERVAL_MS);
}

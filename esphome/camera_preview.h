#pragma once

#include "esphome.h"
#include "esp_heap_caps.h"
#include "jpeg_decoder.h"

// One XGA frame in PSRAM. Called synchronously by on_image and display lambda.
namespace camera_preview {
static uint8_t *pixels = nullptr;
static bool ready = false;
static constexpr int FRAME_WIDTH = 1024;
static constexpr int FRAME_HEIGHT = 768;
static constexpr size_t BUFFER_SIZE = FRAME_WIDTH * FRAME_HEIGHT * 3;

inline void accept(const uint8_t *data, size_t length) {
  if (pixels == nullptr) {
    pixels = static_cast<uint8_t *>(heap_caps_malloc(BUFFER_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (pixels == nullptr) {
      ESP_LOGE("camera_preview", "Cannot allocate preview in PSRAM");
      return;
    }
  }
  esp_jpeg_image_cfg_t cfg{};
  cfg.indata = const_cast<uint8_t *>(data);
  cfg.indata_size = length;
  cfg.outbuf = pixels;
  cfg.outbuf_size = BUFFER_SIZE;
  cfg.out_format = JPEG_IMAGE_FORMAT_RGB888;
  cfg.out_scale = JPEG_IMAGE_SCALE_0;
  esp_jpeg_image_output_t result{};
  ready = false;
  if (esp_jpeg_get_image_info(&cfg, &result) != ESP_OK || result.width != FRAME_WIDTH || result.height != FRAME_HEIGHT) {
    ESP_LOGW("camera_preview", "Preview requires 1024x768 JPEG");
    return;
  }
  ready = esp_jpeg_decode(&cfg, &result) == ESP_OK;
}

inline void draw(esphome::display::Display &display) {
  if (!ready) return;
  // Keep the confirmed orientation. Scale the rotated frame to cover the LCD,
  // cropping the source centrally instead of drawing black letterbox bars.
  constexpr int CROP_WIDTH = FRAME_HEIGHT * 240 / 320;
  constexpr int CROP_LEFT = (FRAME_WIDTH - CROP_WIDTH) / 2;
  for (int y = 0; y < 240; y++) {
    for (int x = 0; x < 320; x++) {
      const int source_x = CROP_LEFT + y * CROP_WIDTH / 240;
      const int source_y = FRAME_HEIGHT - 1 - x * FRAME_HEIGHT / 320;
      const size_t offset = (source_y * FRAME_WIDTH + source_x) * 3;
      display.draw_pixel_at(x, y, esphome::Color(pixels[offset], pixels[offset + 1], pixels[offset + 2]));
    }
  }
}
}  // namespace camera_preview

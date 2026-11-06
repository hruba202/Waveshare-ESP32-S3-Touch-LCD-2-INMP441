#pragma once
#include <atomic>
#include <string>
#include <vector>
#include "esphome/core/component.h"
#include "esphome/components/microphone/microphone.h"
#include "esphome/components/sensor/sensor.h"
#include "esphome/components/display/display.h"
#include "esp_http_server.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"

namespace esphome::lan_microphone {
class LanMicrophone : public Component {
 public:
  void setup() override;
  void loop() override;
  void on_shutdown() override;
  float get_setup_priority() const override { return setup_priority::AFTER_WIFI; }
  void set_microphone(microphone::Microphone *v) { mic_ = v; }
  void set_token(const std::string &v) { token_ = v; }
  void set_subnet(uint32_t network, uint32_t mask) { network_ = network; mask_ = mask; }
  void set_port(uint16_t v) { port_ = v; }
  void set_enabled(bool v);
  bool is_enabled() const { return enabled_.load(); }
  bool is_sending() const;
  void set_gain(float v) { gain_.store(static_cast<uint16_t>(v < 1 ? 1 : (v > 32 ? 32 : v))); }
  void on_data(const std::vector<uint8_t> &data);
  void draw_status(display::Display &it);
  void draw_overlay(display::Display &it);
  void set_input_rms_sensor(sensor::Sensor *s) { input_rms_sensor_ = s; }
  void set_output_rms_sensor(sensor::Sensor *s) { output_rms_sensor_ = s; }
  void set_output_peak_sensor(sensor::Sensor *s) { output_peak_sensor_ = s; }
  void set_clipping_sensor(sensor::Sensor *s) { clipping_sensor_ = s; }
  void set_dropped_blocks_sensor(sensor::Sensor *s) { dropped_sensor_ = s; }
  void set_samples_per_second_sensor(sensor::Sensor *s) { samples_sensor_ = s; }
  void set_zero_samples_sensor(sensor::Sensor *s) { zero_sensor_ = s; }
  void set_input_near_rail_sensor(sensor::Sensor *s) { near_rail_sensor_ = s; }
  void set_connections_sensor(sensor::Sensor *s) { connections_sensor_ = s; }

 protected:
  struct Block { uint32_t generation; uint8_t pcm[640]; };  // 20 ms at 16 kHz / PCM16
  struct Worker { LanMicrophone *self; httpd_req_t *request; uint32_t generation; };
  struct Levels {
    uint64_t input_sum{0}, output_sum{0};
    uint32_t count{0}, clipped{0}, peak{0}, zeros{0}, near_rail{0};
  };
  static esp_err_t handle_request_(httpd_req_t *req);
  static void stream_task_(void *arg);
  void serve_(Worker &work);
  void abort_client_();
  void start_server_();
  void publish_levels_(uint32_t elapsed_ms);
  bool allowed_(httpd_req_t *req) const;
  bool authorized_(httpd_req_t *req) const;
  static void text_(display::Display &it, int x, int y, const char *text, Color color, int scale = 2);
  microphone::Microphone *mic_{nullptr};
  std::string token_;
  uint32_t network_{0}, mask_{0};
  uint16_t port_{8081};
  httpd_handle_t server_{nullptr};
  QueueHandle_t queue_{nullptr};
  SemaphoreHandle_t fd_mutex_{nullptr};
  int client_fd_{-1};  // guarded by fd_mutex_, never closed by the main loop
  std::atomic<bool> enabled_{false}, network_ready_{false}, client_active_{false}, demand_{false};
  std::atomic<uint32_t> generation_{0}, last_tx_{0}, dropped_{0};
  std::atomic<uint32_t> connections_{0};
  std::atomic<uint16_t> gain_{1};
  bool mic_started_{false};
  uint32_t last_server_attempt_{0}, last_metrics_{0};
  Block assembling_{};  // callback thread only
  size_t assembling_size_{0};
  uint32_t assembling_generation_{0};
  portMUX_TYPE levels_mux_ = portMUX_INITIALIZER_UNLOCKED;
  Levels levels_;
  float output_db_{-120.0f}, peak_db_{-120.0f}, clip_percent_{0.0f};
  sensor::Sensor *input_rms_sensor_{nullptr}, *output_rms_sensor_{nullptr},
                 *output_peak_sensor_{nullptr}, *clipping_sensor_{nullptr}, *dropped_sensor_{nullptr};
  sensor::Sensor *samples_sensor_{nullptr}, *zero_sensor_{nullptr},
                 *near_rail_sensor_{nullptr}, *connections_sensor_{nullptr};
};
}  // namespace esphome::lan_microphone

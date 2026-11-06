#include "lan_microphone.h"
#include "audio_format.h"
#include "esphome/components/wifi/wifi_component.h"
#include "esphome/core/log.h"
#include "esp_timer.h"
#include "lwip/sockets.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <new>

namespace esphome::lan_microphone {
static const char *const TAG = "lan_microphone";
static uint32_t now_ms() { return static_cast<uint32_t>(esp_timer_get_time() / 1000); }
static float dbfs(double amplitude) { return amplitude > 0 ? std::max(-120.0f, static_cast<float>(20 * std::log10(amplitude / 32768.0))) : -120.0f; }

void LanMicrophone::setup() {
  queue_ = xQueueCreate(16, sizeof(Block));
  fd_mutex_ = xSemaphoreCreateMutex();
  if (!queue_ || !fd_mutex_) { mark_failed(); return; }
  const auto info = mic_->get_audio_stream_info();
  if (info.get_bits_per_sample() != 32 || info.get_channels() != 1 || info.get_sample_rate() != 16000) {
    ESP_LOGE(TAG, "Requires 32-bit left-slot mono input at 16000 Hz");
    mark_failed();
  }
}

void LanMicrophone::start_server_() {
  httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
  cfg.server_port = port_;
  cfg.ctrl_port = port_ + 1;
  cfg.stack_size = 4096;
  cfg.max_open_sockets = 3;  // one stream, temporary rejection/probe connections
  cfg.max_uri_handlers = 1;
  cfg.recv_wait_timeout = 1;
  cfg.send_wait_timeout = 1;
  cfg.lru_purge_enable = false;
  if (httpd_start(&server_, &cfg) != ESP_OK) { server_ = nullptr; return; }
  httpd_uri_t uri{};
  uri.uri = "/audio.wav";
  uri.method = HTTP_GET;
  uri.handler = handle_request_;
  uri.user_ctx = this;
  if (httpd_register_uri_handler(server_, &uri) != ESP_OK) {
    httpd_stop(server_);
    server_ = nullptr;
    return;
  }
  ESP_LOGI(TAG, "LAN WAV endpoint ready on port %u (token not logged)", port_);
}

void LanMicrophone::set_enabled(bool v) {
  enabled_.store(v);
  if (!v) { demand_.store(false); abort_client_(); }
}
void LanMicrophone::abort_client_() {
  if (!fd_mutex_) return;
  xSemaphoreTake(fd_mutex_, portMAX_DELAY);
  if (client_fd_ >= 0) ::shutdown(client_fd_, SHUT_RDWR);
  xSemaphoreGive(fd_mutex_);
}
bool LanMicrophone::is_sending() const {
  return enabled_.load() && network_ready_.load() && client_active_.load() &&
         last_tx_.load() != 0 && static_cast<uint32_t>(now_ms() - last_tx_.load()) < 1500;
}
void LanMicrophone::loop() {
  if (is_failed()) return;
  const bool connected = wifi::global_wifi_component && wifi::global_wifi_component->is_connected();
  if (!connected && network_ready_.exchange(false)) { demand_.store(false); abort_client_(); }
  if (connected) network_ready_.store(true);
  const uint32_t now = now_ms();
  if (connected && !server_ && (last_server_attempt_ == 0 || now - last_server_attempt_ > 5000)) {
    last_server_attempt_ = now;
    start_server_();
  }
  const bool want_mic = enabled_.load() && connected && demand_.load();
  if (want_mic && !mic_started_) {
    mic_->start();
    mic_started_ = true;
  } else if (!want_mic && mic_started_ && !mic_->is_stopped()) {
    // start() takes a listener immediately, while I2S starts in a later loop.
    // Do not call stop() while STATE_STOPPED: that method would return without
    // returning the pending listener and the mic could start after disconnect.
    mic_->stop();
    mic_started_ = false;
  }
  if (now - last_metrics_ >= 1000) {
    const uint32_t elapsed = now - last_metrics_;
    last_metrics_ = now;
    publish_levels_(elapsed);
  }
}
void LanMicrophone::on_shutdown() {
  set_enabled(false);
  if (mic_started_) { mic_->stop(); mic_started_ = false; }
}

bool LanMicrophone::allowed_(httpd_req_t *req) const {
  sockaddr_in peer{};
  socklen_t len = sizeof(peer);
  if (getpeername(httpd_req_to_sockfd(req), reinterpret_cast<sockaddr *>(&peer), &len) != 0 || peer.sin_family != AF_INET) return false;
  return (ntohl(peer.sin_addr.s_addr) & mask_) == network_;
}
bool LanMicrophone::authorized_(httpd_req_t *req) const {
  char query[192]{}, supplied[129]{};
  const size_t len = httpd_req_get_url_query_len(req);
  if (len == 0 || len >= sizeof(query) || httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK ||
      httpd_query_key_value(query, "token", supplied, sizeof(supplied)) != ESP_OK) return false;
  if (std::strlen(supplied) != token_.size()) return false;
  uint8_t difference = 0;
  for (size_t i = 0; i < token_.size(); ++i) difference |= static_cast<uint8_t>(supplied[i] ^ token_[i]);
  return difference == 0;
}
esp_err_t LanMicrophone::handle_request_(httpd_req_t *req) {
  auto *self = static_cast<LanMicrophone *>(req->user_ctx);
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  httpd_resp_set_hdr(req, "Connection", "close");
  if (!self->allowed_(req)) { httpd_resp_set_status(req, "403 Forbidden"); return httpd_resp_sendstr(req, "LAN only"); }
  if (!self->authorized_(req)) { httpd_resp_set_status(req, "401 Unauthorized"); return httpd_resp_sendstr(req, "Invalid token"); }
  if (!self->enabled_.load() || !self->network_ready_.load()) {
    httpd_resp_set_status(req, "503 Service Unavailable"); return httpd_resp_sendstr(req, "Stream is off");
  }
  bool expected = false;
  if (!self->client_active_.compare_exchange_strong(expected, true)) {
    httpd_resp_set_status(req, "409 Conflict"); return httpd_resp_sendstr(req, "One audio client only");
  }
  httpd_req_t *async_req = nullptr;
  if (httpd_req_async_handler_begin(req, &async_req) != ESP_OK) {
    self->client_active_.store(false);
    return ESP_FAIL;
  }
  auto *work = new (std::nothrow) Worker{self, async_req, self->generation_.fetch_add(1) + 1};
  if (!work || xTaskCreate(stream_task_, "mic_http", 6144, work, 3, nullptr) != pdPASS) {
    delete work;
    httpd_resp_set_status(async_req, "503 Service Unavailable");
    httpd_resp_sendstr(async_req, "No streaming task available");
    httpd_req_async_handler_complete(async_req);
    self->client_active_.store(false);
  }
  return ESP_OK;
}
void LanMicrophone::stream_task_(void *arg) {
  auto *work = static_cast<Worker *>(arg);
  auto *self = work->self;
  self->serve_(*work);
  self->demand_.store(false);
  self->last_tx_.store(0);
  xSemaphoreTake(self->fd_mutex_, portMAX_DELAY);
  self->client_fd_ = -1;
  xSemaphoreGive(self->fd_mutex_);
  // Close the streaming session after completing its async handler.
  const int fd = httpd_req_to_sockfd(work->request);
  httpd_req_async_handler_complete(work->request);
  httpd_sess_trigger_close(self->server_, fd);
  delete work;
  self->client_active_.store(false);
  vTaskDelete(nullptr);
}
void LanMicrophone::serve_(Worker &work) {
  const int fd = httpd_req_to_sockfd(work.request);
  xSemaphoreTake(fd_mutex_, portMAX_DELAY);
  client_fd_ = fd;
  xSemaphoreGive(fd_mutex_);
  timeval timeout{1, 0};
  setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
  int send_buffer = 2048;
  setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &send_buffer, sizeof(send_buffer));
  Block block{};
  while (xQueueReceive(queue_, &block, 0) == pdTRUE) {}  // no historical audio
  if (!enabled_.load() || !network_ready_.load()) return;
  connections_.fetch_add(1);
  demand_.store(true);
  httpd_resp_set_type(work.request, "audio/wav");
  httpd_resp_set_hdr(work.request, "Cache-Control", "no-store");
  httpd_resp_set_hdr(work.request, "Connection", "close");
  const auto header = wav_header();
  if (httpd_resp_send_chunk(work.request, reinterpret_cast<const char *>(header.data()), header.size()) != ESP_OK) return;
  const uint32_t began = now_ms();
  uint32_t last_data = began;
  // WAV's large finite length is never reached: reconnect before 12 hours.
  while (enabled_.load() && network_ready_.load() && demand_.load() && now_ms() - began < 43200000UL) {
    if (xQueueReceive(queue_, &block, pdMS_TO_TICKS(100)) == pdTRUE) {
      if (block.generation != work.generation) continue;
      if (httpd_resp_send_chunk(work.request, reinterpret_cast<const char *>(block.pcm), sizeof(block.pcm)) != ESP_OK) return;
      last_data = now_ms();
      last_tx_.store(last_data);
    } else if (now_ms() - last_data > 3000) {
      break;  // no microphone data: do not hang a client forever
    }
  }
  if (enabled_.load() && network_ready_.load()) httpd_resp_send_chunk(work.request, nullptr, 0);
}

void LanMicrophone::on_data(const std::vector<uint8_t> &data) {
  if (!enabled_.load() || !demand_.load() || !network_ready_.load() || !queue_) return;
  const uint32_t generation = generation_.load();
  if (assembling_generation_ != generation) { assembling_size_ = 0; assembling_generation_ = generation; }
  const uint16_t gain = gain_.load();
  Levels chunk{};
  for (size_t i = 0; i + 4 <= data.size(); i += 4) {
    int32_t raw;
    std::memcpy(&raw, data.data() + i, sizeof(raw));
    const int16_t input = pcm16(raw, 1);
    const int16_t output = pcm16(raw, gain);
    const int64_t amplified = scaled_sample(raw, gain);
    chunk.input_sum += static_cast<int64_t>(input) * input;
    chunk.output_sum += static_cast<int64_t>(output) * output;
    chunk.peak = std::max(chunk.peak, static_cast<uint32_t>(std::abs(static_cast<int>(output))));
    chunk.clipped += amplified > 32767 || amplified < -32768;
    chunk.zeros += input == 0;
    // Diagnostic only: within about 0.07 dB of a signed full-scale input.
    // This is not proof of physical microphone clipping.
    chunk.near_rail += raw >= 0x7f000000 || raw <= -0x7f000000;
    ++chunk.count;
    assembling_.pcm[assembling_size_++] = static_cast<uint8_t>(output);
    assembling_.pcm[assembling_size_++] = static_cast<uint8_t>(static_cast<uint16_t>(output) >> 8);
    if (assembling_size_ == sizeof(assembling_.pcm)) {
      assembling_.generation = generation;
      if (xQueueSend(queue_, &assembling_, 0) != pdTRUE) dropped_.fetch_add(1);
      assembling_size_ = 0;
    }
  }
  portENTER_CRITICAL(&levels_mux_);
  levels_.input_sum += chunk.input_sum;
  levels_.output_sum += chunk.output_sum;
  levels_.count += chunk.count;
  levels_.clipped += chunk.clipped;
  levels_.zeros += chunk.zeros;
  levels_.near_rail += chunk.near_rail;
  levels_.peak = std::max(levels_.peak, chunk.peak);
  portEXIT_CRITICAL(&levels_mux_);
}
void LanMicrophone::publish_levels_(uint32_t elapsed_ms) {
  Levels values;
  portENTER_CRITICAL(&levels_mux_);
  values = levels_;
  levels_ = Levels{};
  portEXIT_CRITICAL(&levels_mux_);
  float input_db = -120.0f;
  output_db_ = peak_db_ = -120.0f;
  clip_percent_ = 0;
  if (values.count) {
    input_db = dbfs(std::sqrt(static_cast<double>(values.input_sum) / values.count));
    output_db_ = dbfs(std::sqrt(static_cast<double>(values.output_sum) / values.count));
    peak_db_ = dbfs(values.peak);
    clip_percent_ = 100.0f * values.clipped / values.count;
  }
  if (input_rms_sensor_) input_rms_sensor_->publish_state(input_db);
  if (output_rms_sensor_) output_rms_sensor_->publish_state(output_db_);
  if (output_peak_sensor_) output_peak_sensor_->publish_state(peak_db_);
  if (clipping_sensor_) clipping_sensor_->publish_state(clip_percent_);
  if (dropped_sensor_) dropped_sensor_->publish_state(dropped_.load());
  const float rate = elapsed_ms ? 1000.0f * values.count / elapsed_ms : 0;
  const float zeros = values.count ? 100.0f * values.zeros / values.count : 0;
  const float near_rail = values.count ? 100.0f * values.near_rail / values.count : 0;
  if (samples_sensor_) samples_sensor_->publish_state(rate);
  if (zero_sensor_) zero_sensor_->publish_state(zeros);
  if (near_rail_sensor_) near_rail_sensor_->publish_state(near_rail);
  if (connections_sensor_) connections_sensor_->publish_state(connections_.load());
  ESP_LOGD(TAG, "Input: samples=%u/s zeros=%.2f%% near_rail=%.2f%% mic=%s client=%s sessions=%u",
           static_cast<unsigned>(rate), zeros, near_rail, mic_->is_running() ? "on" : "off",
           client_active_.load() ? "on" : "off", static_cast<unsigned>(connections_.load()));
}

// Compact local 5x7 uppercase glyphs; no downloadable font or font service.
static const uint8_t GLYPHS[][5] = {
  {0x7e,0x11,0x11,0x11,0x7e},{0x7f,0x49,0x49,0x49,0x36},{0x3e,0x41,0x41,0x41,0x22},
  {0x7f,0x41,0x41,0x22,0x1c},{0x7f,0x49,0x49,0x49,0x41},{0x7f,0x09,0x09,0x09,0x01},
  {0x3e,0x41,0x49,0x49,0x7a},{0x7f,0x08,0x08,0x08,0x7f},{0,0x41,0x7f,0x41,0},
  {0x20,0x40,0x41,0x3f,0x01},{0x7f,0x08,0x14,0x22,0x41},{0x7f,0x40,0x40,0x40,0x40},
  {0x7f,0x02,0x0c,0x02,0x7f},{0x7f,0x04,0x08,0x10,0x7f},{0x3e,0x41,0x41,0x41,0x3e},
  {0x7f,0x09,0x09,0x09,0x06},{0x3e,0x41,0x51,0x21,0x5e},{0x7f,0x09,0x19,0x29,0x46},
  {0x46,0x49,0x49,0x49,0x31},{0x01,0x01,0x7f,0x01,0x01},{0x3f,0x40,0x40,0x40,0x3f},
  {0x1f,0x20,0x40,0x20,0x1f},{0x3f,0x40,0x38,0x40,0x3f},{0x63,0x14,0x08,0x14,0x63},
  {0x07,0x08,0x70,0x08,0x07},{0x61,0x51,0x49,0x45,0x43},
};
void LanMicrophone::text_(display::Display &it, int x, int y, const char *text, Color color, int scale) {
  while (*text) {
    const char c = *text++;
    if (c >= 'A' && c <= 'Z') {
      for (int col = 0; col < 5; ++col) for (int row = 0; row < 7; ++row)
        if (GLYPHS[c - 'A'][col] & (1 << row)) it.filled_rectangle(x + col * scale, y + row * scale, scale, scale, color);
    }
    x += 6 * scale;
  }
}
void LanMicrophone::draw_status(display::Display &it) {
  const Color white(230,230,230), red(255,30,30), amber(255,180,0), green(30,220,70);
  text_(it, 12, 16, "MIKROFON", white, 3);
  const bool sending = is_sending();
  const char *state = is_failed() ? "CHYBA" : (!network_ready_.load() ? "WIFI VYPADEK" :
                      (!enabled_.load() ? "VYPNUTO" : (sending ? "MIC POSILA" : "CEKA NA KLIENTA")));
  const Color color = sending ? red : (enabled_.load() ? amber : green);
  it.filled_rectangle(8, 56, 304, 40, Color(20,20,20));
  text_(it, 16, 68, state, color, 2);
  if (sending) it.filled_circle(292, 20, 8, red);
  text_(it, 12, 112, "UROVEN ZVUKU", white);
  const int width = static_cast<int>(std::max(0.0f, std::min(1.0f, (output_db_ + 80) / 80)) * 288);
  it.rectangle(12, 138, 292, 22, white);
  if (width) it.filled_rectangle(14, 140, width, 18, clip_percent_ > 0 ? red : green);
  if (sending && output_db_ <= -110) text_(it, 12, 178, "BEZ SIGNALU", amber);
  else if (clip_percent_ > 0) text_(it, 12, 178, "PREBUZENI", red);
  else text_(it, 12, 178, "LAN WAV MONO", white);
}
void LanMicrophone::draw_overlay(display::Display &it) {
  // Draw only from the display lambda, after the original camera preview.
  // Always repaint this small area so OFF/waiting cannot leave a stale red marker.
  const bool sending = is_sending();
  const Color color = sending ? Color(255,30,30) : Color(255,180,0);
  const char *label = sending ? "MIC POSILA" : (is_enabled() ? "MIC CEKA" : "MIC VYPNUT");
  it.filled_rectangle(0, 0, 152, 24, Color(0,0,0));
  text_(it, 4, 5, label, color, 2);
  if (sending) it.filled_circle(140, 12, 5, color);
}
}  // namespace esphome::lan_microphone

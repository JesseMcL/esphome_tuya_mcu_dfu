#include "tuya_mcu_dfu.h"

#include <cstring>

#include "esphome/core/hal.h"
#include "esphome/core/log.h"

#ifdef USE_TUYA_MCU_DFU_RUNTIME
#include "esphome/core/application.h"
#include "esphome/components/json/json_util.h"
#include "esphome/components/md5/md5.h"
#include "esphome/components/network/util.h"
#endif

namespace esphome::tuya_mcu_dfu {

static const char *const TAG = "tuya_mcu_dfu";

static constexpr uint8_t CMD_HEARTBEAT = 0x00;
static constexpr uint8_t CMD_PRODUCT = 0x01;
static constexpr uint8_t CMD_OTA_START = 0x0A;
static constexpr uint8_t CMD_OTA_DATA = 0x0B;
static constexpr size_t MAX_RX = 512;                 // product info JSON can be a few hundred bytes
static constexpr uint32_t END_ACK_TIMEOUT = 1500;     // the end frame needn't be acknowledged
static constexpr uint32_t PRODUCT_QUERY_INTERVAL = 1000;
static constexpr uint32_t PRODUCT_WINDOW = 10000;     // then fall back to the version DP

// ---------------------------------------------------------------------------
// Setup / state
// ---------------------------------------------------------------------------

void TuyaMcuDfu::set_firmware(const uint8_t *data, size_t len, const char *version, const char *title,
                              const char *md5, const char *release_url, const char *summary) {
  this->fw_ = data;
  this->fw_len_ = len;
  this->fw_in_ram_ = false;
  this->fw_version_ = version;
  this->update_info_.latest_version = version;
  this->update_info_.title = title;
  this->update_info_.md5 = md5;
  this->update_info_.release_url = release_url;
  this->update_info_.summary = summary;
}

void TuyaMcuDfu::setup() {
  if (this->has_version_dp_) {
    this->parent_->register_listener(this->version_dp_, [this](const tuya::TuyaDatapoint &dp) {
      if (dp.type != tuya::TuyaDatapointType::STRING)
        return;
      if (this->update_info_.current_version != dp.value_string)
        ESP_LOGI(TAG, "MCU firmware version: %s", dp.value_string.c_str());
      this->update_info_.current_version = dp.value_string;
      if (this->phase_ == Phase::VERIFY && dp.value_string == this->fw_version_) {
        this->verified_("version datapoint");
      } else if (this->phase_ == Phase::IDLE) {
        this->refresh_state_();
      }
    });
  }
#ifdef USE_TUYA_MCU_DFU_RUNTIME
  // First check once the network had a chance to come up, then periodically
  this->set_timeout("first_check", 15000, [this]() { this->check(); });
  if (this->check_interval_ != 0 && this->check_interval_ != SCHEDULER_DONT_RUN)
    this->set_interval("check", this->check_interval_, [this]() { this->check(); });
#endif
  this->refresh_state_();
  this->disable_loop();
}

void TuyaMcuDfu::dump_config() {
  ESP_LOGCONFIG(TAG, "Tuya MCU firmware update:");
  if (this->has_version_dp_) {
    ESP_LOGCONFIG(TAG, "  Version datapoint: %u", this->version_dp_);
  } else {
    ESP_LOGCONFIG(TAG, "  Version datapoint: none (product information is used to verify)");
  }
#ifdef USE_TUYA_MCU_DFU_RUNTIME
  ESP_LOGCONFIG(TAG, "  Source (runtime): %s", this->source_url_);
  ESP_LOGCONFIG(TAG, "  Max image size: %" PRIu32 " bytes", this->max_size_);
#else
  if (this->fw_ != nullptr) {
    ESP_LOGCONFIG(TAG, "  Bundled firmware: %s (%u bytes, md5 %s)", this->fw_version_.c_str(),
                  (unsigned) this->fw_len_, this->update_info_.md5.c_str());
  } else {
    ESP_LOGCONFIG(TAG, "  Bundled firmware: none");
  }
#endif
  ESP_LOGCONFIG(TAG, "  MCU version: %s",
                this->update_info_.current_version.empty() ? "unknown" : this->update_info_.current_version.c_str());
}

void TuyaMcuDfu::check() {
#ifdef USE_TUYA_MCU_DFU_RUNTIME
  if (this->phase_ == Phase::IDLE)
    this->fetch_manifest_();
#endif
  this->refresh_state_();
}

void TuyaMcuDfu::refresh_state_() {
  if (this->phase_ != Phase::IDLE)
    return;
  // No release known: never publish, so the entity has no state and Home
  // Assistant shows it as unknown. (HA derives "update available" from the
  // installed vs latest strings, and there is no honest "latest" to report.)
  if (!this->has_release_())
    return;
  update::UpdateState st;
  if (this->update_info_.current_version.empty() || this->update_info_.current_version != this->fw_version_) {
    // unknown version (no version DP, or e.g. stock firmware) counts as installable
    st = update::UPDATE_STATE_AVAILABLE;
  } else {
    st = update::UPDATE_STATE_NO_UPDATE;
  }
  bool trigger = st == update::UPDATE_STATE_AVAILABLE && this->state_ != update::UPDATE_STATE_AVAILABLE;
  this->state_ = st;
  this->publish_state();
  if (trigger)
    this->get_update_available_trigger()->trigger(this->update_info_);
}

// ---------------------------------------------------------------------------
// Install
// ---------------------------------------------------------------------------

void TuyaMcuDfu::perform(bool force) {
  if (!this->has_release_()) {
    ESP_LOGW(TAG, "No MCU firmware release known");
    return;
  }
  if (this->is_updating()) {
    ESP_LOGW(TAG, "Update already in progress");
    return;
  }
  if (this->state_ != update::UPDATE_STATE_AVAILABLE && !force)
    return;
#ifdef USE_TUYA_MCU_DFU_RUNTIME
  if (!this->download_image_()) {
    this->status_set_error(LOG_STR("MCU firmware download failed"));
    this->publish_state();
    return;
  }
#endif
  if (this->fw_ == nullptr) {
    ESP_LOGW(TAG, "No MCU firmware image available");
    return;
  }
  this->begin_transfer_();
}

void TuyaMcuDfu::begin_transfer_() {
  ESP_LOGI(TAG, "Updating MCU firmware %s -> %s (%u bytes)",
           this->update_info_.current_version.empty() ? "?" : this->update_info_.current_version.c_str(),
           this->fw_version_.c_str(), (unsigned) this->fw_len_);
  // Take the UART: tuya: would otherwise read (and drop) the MCU's replies.
  // Its scheduler-driven heartbeat still goes out; the MCU's answer is ignored.
  this->parent_->disable_loop();
  this->state_ = update::UPDATE_STATE_INSTALLING;
  this->update_info_.has_progress = true;
  this->set_progress_(0);
  this->rx_.clear();
  this->offset_ = 0;
  this->tries_ = 0;
  this->phase_ = Phase::START;
  this->high_freq_.start();
  this->enable_loop();
  this->send_start_();
}

uint8_t TuyaMcuDfu::fw_byte_(size_t i) const {
  return this->fw_in_ram_ ? this->fw_[i] : progmem_read_byte(this->fw_ + i);
}

void TuyaMcuDfu::set_progress_(float p) {
  this->update_info_.progress = p;
  if (p - this->last_published_progress_ >= 2.0f || p <= 0.0f || p >= 100.0f) {
    this->last_published_progress_ = p;
    this->publish_state();
  }
}

void TuyaMcuDfu::send_frame_(uint8_t cmd, const uint8_t *payload, size_t len) {
  uint8_t hdr[6] = {0x55, 0xAA, 0x00, cmd, (uint8_t) (len >> 8), (uint8_t) len};
  uint8_t sum = 0;
  for (uint8_t b : hdr)
    sum += b;
  this->write_array(hdr, sizeof(hdr));
  for (size_t i = 0; i < len; i++) {
    uint8_t b = payload[i];
    sum += b;
    this->write_byte(b);
  }
  this->write_byte(sum);
  this->sent_at_ = millis();
}

void TuyaMcuDfu::send_start_() {
  uint8_t p[4] = {(uint8_t) (this->fw_len_ >> 24), (uint8_t) (this->fw_len_ >> 16), (uint8_t) (this->fw_len_ >> 8),
                  (uint8_t) this->fw_len_};
  ESP_LOGD(TAG, "-> start (%u bytes), try %u", (unsigned) this->fw_len_, this->tries_ + 1);
  this->send_frame_(CMD_OTA_START, p, sizeof(p));
}

void TuyaMcuDfu::send_packet_() {
  size_t n = this->fw_len_ - this->offset_;
  if (n > this->packet_size_)
    n = this->packet_size_;
  this->last_len_ = n;
  std::vector<uint8_t> p(4 + n);
  p[0] = this->offset_ >> 24;
  p[1] = this->offset_ >> 16;
  p[2] = this->offset_ >> 8;
  p[3] = this->offset_;
  for (size_t i = 0; i < n; i++)
    p[4 + i] = this->fw_byte_(this->offset_ + i);
  this->send_frame_(CMD_OTA_DATA, p.data(), p.size());
}

void TuyaMcuDfu::send_end_() {
  uint8_t p[4] = {(uint8_t) (this->fw_len_ >> 24), (uint8_t) (this->fw_len_ >> 16), (uint8_t) (this->fw_len_ >> 8),
                  (uint8_t) this->fw_len_};
  ESP_LOGD(TAG, "-> end");
  this->send_frame_(CMD_OTA_DATA, p, sizeof(p));
}

void TuyaMcuDfu::send_product_query_() {
  ESP_LOGV(TAG, "-> product query");
  this->send_frame_(CMD_PRODUCT, nullptr, 0);
}

void TuyaMcuDfu::finish_transfer_() {
  ESP_LOGI(TAG, "Transfer complete, waiting for the MCU to restart with %s", this->fw_version_.c_str());
  this->set_progress_(100);
  // As the Tuya spec: ask for product information until the MCU answers with
  // its new version. tuya: stays suspended so we see the reply.
  this->phase_ = Phase::VERIFY;
  this->verify_start_ = millis();
  this->product_seen_ = false;
  this->rx_.clear();
  this->sent_at_ = millis() - PRODUCT_QUERY_INTERVAL + 500;  // first query in 0.5 s
}

void TuyaMcuDfu::release_uart_(bool poke) {
  this->high_freq_.stop();
  if (!this->parent_->is_idle())
    return;
  this->parent_->enable_loop();
  // Don't wait up to 15 s for tuya:'s next heartbeat: a restarted MCU answers
  // this one with "first heartbeat", which makes tuya: re-initialise now.
  if (poke)
    this->set_timeout("hb", 300, [this]() { this->send_frame_(CMD_HEARTBEAT, nullptr, 0); });
}

void TuyaMcuDfu::verified_(const char *how) {
  ESP_LOGI(TAG, "MCU update to %s verified (%s)", this->fw_version_.c_str(), how);
  this->update_info_.current_version = this->fw_version_;
  this->phase_ = Phase::IDLE;
  this->release_uart_(true);
  this->update_info_.has_progress = false;
  this->status_clear_error();
#ifdef USE_TUYA_MCU_DFU_RUNTIME
  this->free_image_();
#endif
  this->refresh_state_();
}

void TuyaMcuDfu::fail_(const char *why) {
  ESP_LOGE(TAG, "MCU update failed: %s", why);
  this->update_info_.progress = 0;
  this->phase_ = Phase::IDLE;
  this->release_uart_(true);
  this->update_info_.has_progress = false;
  this->status_set_error(LOG_STR("MCU firmware update failed"));
  this->state_ = update::UPDATE_STATE_AVAILABLE;
  this->publish_state();
}

// "v" from the product information JSON, e.g. {"p":"abc","v":"1.0.0","m":0}
static bool product_version(const uint8_t *data, size_t len, std::string &out) {
  std::string s(reinterpret_cast<const char *>(data), len);
  size_t i = s.find("\"v\"");
  if (i == std::string::npos)
    return false;
  i += 3;
  while (i < s.size() && (s[i] == ' ' || s[i] == ':'))
    i++;
  if (i >= s.size() || s[i] != '"')
    return false;
  size_t end = s.find('"', i + 1);
  if (end == std::string::npos)
    return false;
  out = s.substr(i + 1, end - i - 1);
  return true;
}

void TuyaMcuDfu::on_product_(const uint8_t *data, size_t len) {
  std::string v;
  this->product_seen_ = true;
  if (!product_version(data, len, v)) {
    ESP_LOGD(TAG, "MCU answered, product information has no version");
    if (!this->has_version_dp_) {
      // Nothing more to go on: the MCU runs and talks, assume it's the new firmware
      ESP_LOGW(TAG, "The MCU doesn't report a version; can't confirm it runs %s", this->fw_version_.c_str());
      this->verified_("MCU restarted, version not reported");
    }
    return;  // else: keep waiting for the version DP (see loop)
  }
  ESP_LOGD(TAG, "MCU reports version %s", v.c_str());
  if (v == this->fw_version_)
    this->verified_("product information");
  // else e.g. a bootloader answering with its own version: keep asking
}

void TuyaMcuDfu::on_frame_(uint8_t cmd, const uint8_t *data, size_t len) {
#if ESPHOME_LOG_LEVEL >= ESPHOME_LOG_LEVEL_VERBOSE
  char hex[format_hex_pretty_size(16)];
  ESP_LOGV(TAG, "<- cmd 0x%02X len %u [%s]", cmd, (unsigned) len,
           format_hex_pretty_to(hex, data, len > 16 ? 16 : len));
#endif
  if (this->phase_ == Phase::START && cmd == CMD_OTA_START) {
    uint8_t sel = len >= 1 ? data[0] : 0;
    this->packet_size_ = sel == 1 ? 512 : sel == 2 ? 1024 : 256;
    ESP_LOGI(TAG, "MCU accepted update, %u-byte packets", this->packet_size_);
    this->phase_ = Phase::DATA;
    this->tries_ = 0;
    this->send_packet_();
  } else if (this->phase_ == Phase::DATA && cmd == CMD_OTA_DATA) {
    this->offset_ += this->last_len_;
    this->tries_ = 0;
    this->set_progress_(100.0f * this->offset_ / this->fw_len_);
    if (this->offset_ >= this->fw_len_) {
      this->phase_ = Phase::FINISH;
      this->send_end_();
    } else {
      this->send_packet_();
    }
  } else if (this->phase_ == Phase::FINISH && cmd == CMD_OTA_DATA) {
    this->finish_transfer_();
  } else if (this->phase_ == Phase::VERIFY && cmd == CMD_PRODUCT) {
    this->on_product_(data, len);
  }
  // anything else (heartbeat replies etc.) is ignored while we own the UART
}

void TuyaMcuDfu::poll_uart_() {
  uint8_t c;
  while (this->available() && this->read_byte(&c)) {
    this->rx_.push_back(c);
    size_t n = this->rx_.size();
    if (n == 1 && c != 0x55) {
      this->rx_.clear();
      continue;
    }
    if (n == 2 && c != 0xAA) {
      this->rx_.clear();
      if (c == 0x55)
        this->rx_.push_back(c);
      continue;
    }
    if (n < 6)
      continue;
    size_t len = ((size_t) this->rx_[4] << 8) | this->rx_[5];
    if (len > MAX_RX) {
      this->rx_.clear();
      continue;
    }
    if (n < 7 + len)
      continue;
    uint8_t sum = 0;
    for (size_t i = 0; i < 6 + len; i++)
      sum += this->rx_[i];
    if (sum == this->rx_[6 + len])
      this->on_frame_(this->rx_[3], this->rx_.data() + 6, len);
    this->rx_.clear();
    if (this->phase_ == Phase::IDLE || !this->parent_->is_idle())
      return;  // finished: the rest belongs to tuya:
  }
}

void TuyaMcuDfu::loop() {
  const bool uart_ours = this->parent_->is_idle();
  if (this->phase_ == Phase::IDLE) {
    this->disable_loop();
    return;
  }
  if (uart_ours)
    this->poll_uart_();
  if (this->phase_ == Phase::IDLE)
    return;
  const uint32_t now = millis();  // after poll: a reply may just have re-armed sent_at_

  switch (this->phase_) {
    case Phase::START:
    case Phase::DATA:
    case Phase::FINISH: {
      uint32_t timeout = this->phase_ == Phase::START    ? this->start_timeout_
                         : this->phase_ == Phase::FINISH ? END_ACK_TIMEOUT
                                                         : this->packet_timeout_;
      if (now - this->sent_at_ < timeout)
        return;
      if (this->phase_ == Phase::FINISH) {
        this->finish_transfer_();
        return;
      }
      if (++this->tries_ > this->retries_) {
        this->fail_(this->phase_ == Phase::START ? "MCU did not accept the update (no 0x0A reply)"
                                                 : "MCU stopped acknowledging data packets");
        return;
      }
      ESP_LOGW(TAG, "No reply from MCU, retrying (%u/%u)", this->tries_, this->retries_);
      this->rx_.clear();
      while (this->available()) {  // drop anything stale so a late ack can't be miscounted
        uint8_t c;
        this->read_byte(&c);
      }
      if (this->phase_ == Phase::START)
        this->send_start_();
      else
        this->send_packet_();
      return;
    }

    case Phase::VERIFY: {
      const uint32_t elapsed = now - this->verify_start_;
      if (elapsed > this->verify_timeout_) {
        this->fail_("MCU did not come back with the new version");
        return;
      }
      if (!uart_ours)
        return;  // waiting for the version DP through tuya:
      if (this->has_version_dp_ && (elapsed > PRODUCT_WINDOW || this->product_seen_)) {
        // No (matching) product version yet: let tuya: re-initialise and wait
        // for the version DP instead
        ESP_LOGD(TAG, "Waiting for the MCU to report its version on DP %u", this->version_dp_);
        this->release_uart_(true);
        this->enable_loop();  // keep watching the timeout
        return;
      }
      if (now - this->sent_at_ >= PRODUCT_QUERY_INTERVAL)
        this->send_product_query_();
      return;
    }

    case Phase::IDLE:
      return;
  }
}

// ---------------------------------------------------------------------------
// Runtime download (http_request)
// ---------------------------------------------------------------------------
#ifdef USE_TUYA_MCU_DFU_RUNTIME

// Read a response body into buf (up to cap bytes). Returns bytes read, or -1.
static int read_body(http_request::HttpContainer *c, uint8_t *buf, size_t cap, uint32_t timeout_ms) {
  size_t n = 0;
  uint32_t last = millis();
  while (n < cap && !c->is_read_complete()) {
    App.feed_wdt();
    int r = c->read(buf + n, std::min<size_t>(cap - n, 512));
    if (r < 0)
      return c->is_read_complete() ? (int) n : -1;
    if (r == 0) {
      if (millis() - last > timeout_ms)
        return -1;
      delay(1);
      continue;
    }
    n += r;
    last = millis();
  }
  return (int) n;
}

static std::string resolve_url(const std::string &base, const std::string &path) {
  if (path.rfind("http://", 0) == 0 || path.rfind("https://", 0) == 0)
    return path;
  if (!path.empty() && path[0] == '/') {
    size_t host_end = base.find('/', base.find("://") + 3);
    return base.substr(0, host_end) + path;
  }
  return base.substr(0, base.rfind('/') + 1) + path;
}

bool TuyaMcuDfu::fetch_manifest_() {
  if (!network::is_connected()) {
    ESP_LOGD(TAG, "Network not connected, skipping MCU firmware check");
    return false;
  }
  auto c = this->http_->get(this->source_url_);
  if (c == nullptr || c->status_code != http_request::HTTP_STATUS_OK) {
    ESP_LOGW(TAG, "Failed to fetch MCU firmware manifest %s (HTTP %d)", this->source_url_,
             c == nullptr ? -1 : c->status_code);
    if (c != nullptr)
      c->end();
    this->status_set_warning(LOG_STR("MCU firmware manifest unavailable"));
    return false;
  }
  static constexpr size_t MANIFEST_MAX = 4096;
  std::vector<uint8_t> body(MANIFEST_MAX);
  int n = read_body(c.get(), body.data(), body.size(), this->http_->get_timeout());
  c->end();
  if (n <= 0) {
    ESP_LOGW(TAG, "Failed to read the MCU firmware manifest");
    this->status_set_warning(LOG_STR("MCU firmware manifest unavailable"));
    return false;
  }

  update::UpdateInfo info;
  std::string path;
  size_t size = 0;
  const std::string family = this->chip_family_;
  bool ok = json::parse_json(body.data(), n, [&](JsonObject root) -> bool {
    if (!root["version"].is<const char *>() || !root["builds"].is<JsonArray>())
      return false;
    info.title = root["name"] | "";
    info.latest_version = root["version"].as<std::string>();
    JsonArray builds = root["builds"].as<JsonArray>();
    if (family.empty() && builds.size() != 1) {
      ESP_LOGE(TAG, "Manifest has %u builds; set chip_family", (unsigned) builds.size());
      return false;
    }
    for (JsonObject b : builds) {
      if (!family.empty() && family != (b["chipFamily"] | ""))
        continue;
      JsonObject ota = b["ota"];
      if (!ota["path"].is<const char *>() || !ota["md5"].is<const char *>())
        return false;
      path = ota["path"].as<std::string>();
      info.md5 = ota["md5"].as<std::string>();
      info.release_url = ota["release_url"] | "";
      info.summary = ota["summary"] | "";
      size = ota["size"] | 0;
      return true;
    }
    ESP_LOGE(TAG, "No build for chipFamily '%s' in the manifest", family.c_str());
    return false;
  });
  if (!ok) {
    ESP_LOGW(TAG, "Invalid MCU firmware manifest");
    this->status_set_warning(LOG_STR("Invalid MCU firmware manifest"));
    return false;
  }
  this->status_clear_warning();

  if (info.md5 != this->update_info_.md5)
    this->free_image_();  // a different release: drop an old download
  this->firmware_url_ = resolve_url(this->source_url_, path);
  this->expected_size_ = size;
  if (info.latest_version != this->fw_version_)
    ESP_LOGI(TAG, "MCU firmware release: %s", info.latest_version.c_str());
  this->fw_version_ = info.latest_version;
  this->update_info_.latest_version = info.latest_version;
  this->update_info_.title = info.title;
  this->update_info_.md5 = info.md5;
  this->update_info_.release_url = info.release_url;
  this->update_info_.summary = info.summary;
  this->update_info_.firmware_url = this->firmware_url_;
  return true;
}

bool TuyaMcuDfu::download_image_() {
  if (this->fw_ != nullptr && this->fw_in_ram_)
    return true;  // already downloaded and checked
  if (this->firmware_url_.empty() && !this->fetch_manifest_())
    return false;
  ESP_LOGI(TAG, "Downloading MCU firmware %s", this->firmware_url_.c_str());
  auto c = this->http_->get(this->firmware_url_);
  if (c == nullptr || c->status_code != http_request::HTTP_STATUS_OK) {
    ESP_LOGE(TAG, "Download failed (HTTP %d)", c == nullptr ? -1 : c->status_code);
    if (c != nullptr)
      c->end();
    return false;
  }
  size_t cap = c->content_length != 0 ? c->content_length : this->expected_size_;
  if (cap == 0)
    cap = this->max_size_;  // unknown length (chunked): read up to the limit
  if (cap > this->max_size_) {
    ESP_LOGE(TAG, "Image is %u bytes, more than max_size (%u)", (unsigned) cap, (unsigned) this->max_size_);
    c->end();
    return false;
  }
  RAMAllocator<uint8_t> allocator;
  uint8_t *buf = allocator.allocate(cap);
  if (buf == nullptr) {
    ESP_LOGE(TAG, "Can't allocate %u bytes for the image", (unsigned) cap);
    c->end();
    return false;
  }
  int n = read_body(c.get(), buf, cap, this->http_->get_timeout());
  bool complete = c->is_read_complete();
  c->end();
  if (n <= 0 || !complete || (this->expected_size_ != 0 && (size_t) n != this->expected_size_)) {
    ESP_LOGE(TAG, "Download incomplete (%d bytes)", n);
    allocator.deallocate(buf, cap);
    return false;
  }
  md5::MD5Digest md5;
  md5.init();
  md5.add(buf, n);
  md5.calculate();
  if (!md5.equals_hex(this->update_info_.md5.c_str())) {
    ESP_LOGE(TAG, "MD5 mismatch, image rejected");
    allocator.deallocate(buf, cap);
    return false;
  }
  ESP_LOGI(TAG, "Downloaded %d bytes, MD5 ok", n);
  this->ram_ = buf;
  this->ram_size_ = cap;
  this->fw_ = buf;
  this->fw_len_ = n;
  this->fw_in_ram_ = true;
  return true;
}

void TuyaMcuDfu::free_image_() {
  if (this->ram_ != nullptr) {
    RAMAllocator<uint8_t> allocator;
    allocator.deallocate(this->ram_, this->ram_size_);
  }
  this->ram_ = nullptr;
  this->ram_size_ = 0;
  if (this->fw_in_ram_) {
    this->fw_ = nullptr;
    this->fw_len_ = 0;
    this->fw_in_ram_ = false;
  }
}

#endif  // USE_TUYA_MCU_DFU_RUNTIME

}  // namespace esphome::tuya_mcu_dfu

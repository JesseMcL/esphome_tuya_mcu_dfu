#include "tuya_mcu_dfu.h"

#include "esphome/core/hal.h"
#include "esphome/core/log.h"

namespace esphome::tuya_mcu_dfu {

static const char *const TAG = "tuya_mcu_dfu";

static constexpr uint8_t CMD_OTA_START = 0x0A;
static constexpr uint8_t CMD_OTA_DATA = 0x0B;
static constexpr size_t MAX_RX = 64;

void TuyaMcuDfu::set_firmware(const uint8_t *data, size_t len, const char *version, const char *title,
                              const char *md5, const char *release_url, const char *summary) {
  this->fw_ = data;
  this->fw_len_ = len;
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
      if (this->phase_ == Phase::VERIFY) {
        if (dp.value_string != this->fw_version_)
          return;  // e.g. a bootloader's own version: keep waiting until the timeout
        this->verified_();
      } else if (this->is_updating()) {
        return;
      }
      this->refresh_state_();
    });
  } else {
    // No version DP: the MCU coming back up (tuya: initialised) is the check
    this->parent_->add_on_initialized_callback([this]() {
      if (this->phase_ == Phase::VERIFY) {
        this->update_info_.current_version = this->fw_version_;
        this->verified_();
      }
    });
  }
  this->refresh_state_();
  this->disable_loop();
}

void TuyaMcuDfu::verified_() {
  ESP_LOGI(TAG, "MCU update to %s verified", this->fw_version_.c_str());
  this->phase_ = Phase::IDLE;
  this->update_info_.has_progress = false;
  this->status_clear_error();
  this->refresh_state_();
}

void TuyaMcuDfu::dump_config() {
  ESP_LOGCONFIG(TAG, "Tuya MCU firmware update:");
  if (this->has_version_dp_) {
    ESP_LOGCONFIG(TAG, "  Version datapoint: %u", this->version_dp_);
  } else {
    ESP_LOGCONFIG(TAG, "  Version datapoint: none");
  }
  if (this->fw_ != nullptr) {
    ESP_LOGCONFIG(TAG, "  Bundled firmware: %s (%u bytes, md5 %s)", this->fw_version_.c_str(),
                  (unsigned) this->fw_len_, this->update_info_.md5.c_str());
  } else {
    ESP_LOGCONFIG(TAG, "  Bundled firmware: none");
  }
  ESP_LOGCONFIG(TAG, "  MCU version: %s",
                this->update_info_.current_version.empty() ? "unknown" : this->update_info_.current_version.c_str());
}

void TuyaMcuDfu::refresh_state_() {
  if (this->phase_ != Phase::IDLE)
    return;
  // No firmware bundled: never publish, so the entity has no state and Home
  // Assistant shows it as unknown. (HA derives "update available" from the
  // installed vs latest strings, and there is no honest "latest" to report.)
  if (this->fw_ == nullptr)
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

void TuyaMcuDfu::perform(bool force) {
  if (this->fw_ == nullptr) {
    ESP_LOGW(TAG, "No MCU firmware bundled in this build");
    return;
  }
  if (this->is_updating()) {
    ESP_LOGW(TAG, "Update already in progress");
    return;
  }
  if (this->state_ != update::UPDATE_STATE_AVAILABLE && !force)
    return;

  ESP_LOGI(TAG, "Updating MCU firmware %s -> %s (%u bytes)",
           this->update_info_.current_version.empty() ? "?" : this->update_info_.current_version.c_str(),
           this->fw_version_.c_str(), (unsigned) this->fw_len_);
  // Take the UART: tuya: would otherwise read (and drop) the MCU's 0x0A/0x0B
  // replies. Its scheduler-driven heartbeat still goes out; the bootloader
  // answers it and we ignore that answer.
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
    p[4 + i] = progmem_read_byte(this->fw_ + this->offset_ + i);
  this->send_frame_(CMD_OTA_DATA, p.data(), p.size());
}

void TuyaMcuDfu::send_end_() {
  uint8_t p[4] = {(uint8_t) (this->fw_len_ >> 24), (uint8_t) (this->fw_len_ >> 16), (uint8_t) (this->fw_len_ >> 8),
                  (uint8_t) this->fw_len_};
  ESP_LOGD(TAG, "-> end");
  this->send_frame_(CMD_OTA_DATA, p, sizeof(p));
}

void TuyaMcuDfu::fail_(const char *why) {
  ESP_LOGE(TAG, "MCU update failed: %s", why);
  this->update_info_.progress = 0;
  this->phase_ = Phase::IDLE;
  this->high_freq_.stop();
  this->parent_->enable_loop();
  this->update_info_.has_progress = false;
  this->status_set_error(LOG_STR("MCU firmware update failed"));
  this->state_ = update::UPDATE_STATE_AVAILABLE;
  this->publish_state();
}

void TuyaMcuDfu::finish_transfer_() {
  ESP_LOGI(TAG, "Transfer complete, waiting for the MCU to restart and report %s", this->fw_version_.c_str());
  this->phase_ = Phase::VERIFY;
  this->verify_start_ = millis();
  this->high_freq_.stop();
  this->parent_->enable_loop();  // tuya: re-initialises when the new app boots
  this->set_progress_(100);
  // Don't wait up to 15 s for tuya:'s next heartbeat: poke the new app now.
  // Its "first heartbeat" reply makes tuya: re-initialise straight away.
  this->set_timeout("hb", 500, [this]() { this->send_frame_(0x00, nullptr, 0); });
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
  }
  // anything else (heartbeat replies etc.) is ignored during the transfer
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
  }
}

void TuyaMcuDfu::loop() {
  switch (this->phase_) {
    case Phase::IDLE:
      this->disable_loop();
      return;

    case Phase::START:
    case Phase::DATA:
    case Phase::FINISH: {
      this->poll_uart_();
      if (this->phase_ == Phase::VERIFY || this->phase_ == Phase::IDLE)
        return;
      const uint32_t now = millis();  // after poll: a reply may just have re-armed sent_at_
      uint32_t timeout = this->phase_ == Phase::START ? this->start_timeout_
                         : this->phase_ == Phase::FINISH ? 1500
                                                         : this->packet_timeout_;
      if (now - this->sent_at_ < timeout)
        return;
      if (this->phase_ == Phase::FINISH) {
        // Tuya: the MCU does not have to acknowledge the last packet
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

    case Phase::VERIFY:
      if (millis() - this->verify_start_ > this->verify_timeout_) {
        this->fail_("MCU did not report the new version after the update");
      }
      return;
  }
}

}  // namespace esphome::tuya_mcu_dfu

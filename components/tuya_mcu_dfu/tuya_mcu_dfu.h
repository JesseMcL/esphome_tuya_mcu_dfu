#pragma once

#include <string>
#include <vector>

#include "esphome/core/component.h"
#include "esphome/core/defines.h"
#include "esphome/core/helpers.h"
#include "esphome/components/tuya/tuya.h"
#include "esphome/components/uart/uart.h"
#include "esphome/components/update/update_entity.h"

#ifdef USE_TUYA_MCU_DFU_RUNTIME
#include "esphome/components/http_request/http_request.h"
#endif

namespace esphome::tuya_mcu_dfu {

/// Updates the MCU behind a `tuya:` component with the standard Tuya MCU
/// upgrade protocol:
///   module -> MCU  0x0A [size u32 BE]              MCU -> 0x0A [pkt size sel]
///   module -> MCU  0x0B [offset u32 BE][data]      MCU -> 0x0B []
///   module -> MCU  0x0B [size u32 BE]              (end, ack optional)
///   module -> MCU  0x01                            MCU -> 0x01 {"v":"<new>",..}
/// While transferring and verifying, the `tuya:` component's loop is suspended
/// so this component sees the MCU's replies (`tuya:` then reports is_idle();
/// code that writes datapoints should hold off meanwhile). Afterwards `tuya:`
/// resumes and re-initialises with the restarted MCU.
///
/// The image is either bundled at build time (set_firmware) or, with
/// USE_TUYA_MCU_DFU_RUNTIME, downloaded with http_request when installing.
class TuyaMcuDfu : public update::UpdateEntity, public Component, public uart::UARTDevice {
 public:
  void set_tuya_parent(tuya::Tuya *parent) { this->parent_ = parent; }
  void set_version_datapoint(uint8_t dp) {
    this->version_dp_ = dp;
    this->has_version_dp_ = true;
  }
  void set_timeouts(uint32_t start_ms, uint32_t packet_ms, uint32_t verify_ms, uint8_t retries) {
    this->start_timeout_ = start_ms;
    this->packet_timeout_ = packet_ms;
    this->verify_timeout_ = verify_ms;
    this->retries_ = retries;
  }
  void set_firmware(const uint8_t *data, size_t len, const char *version, const char *title, const char *md5,
                    const char *release_url, const char *summary);
#ifdef USE_TUYA_MCU_DFU_RUNTIME
  void set_http_request(http_request::HttpRequestComponent *http) { this->http_ = http; }
  void set_source_url(const char *url) { this->source_url_ = url; }
  void set_chip_family(const char *chip_family) { this->chip_family_ = chip_family; }
  void set_max_size(uint32_t max_size) { this->max_size_ = max_size; }
  void set_check_interval(uint32_t ms) { this->check_interval_ = ms; }
#endif

  void setup() override;
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::DATA; }

  void perform(bool force) override;
  void check() override;

  bool is_updating() const { return this->phase_ != Phase::IDLE; }

 protected:
  enum class Phase : uint8_t { IDLE, START, DATA, FINISH, VERIFY };

  bool has_release_() const { return !this->fw_version_.empty(); }
  void refresh_state_();
  void begin_transfer_();
  void verified_(const char *how);
  void release_uart_(bool poke);
  void send_frame_(uint8_t cmd, const uint8_t *payload, size_t len);
  void send_start_();
  void send_packet_();
  void send_end_();
  void send_product_query_();
  void on_frame_(uint8_t cmd, const uint8_t *data, size_t len);
  void on_product_(const uint8_t *data, size_t len);
  void poll_uart_();
  void fail_(const char *why);
  void finish_transfer_();
  void set_progress_(float p);
  uint8_t fw_byte_(size_t i) const;

  tuya::Tuya *parent_{nullptr};
  uint8_t version_dp_{0};
  bool has_version_dp_{false};
  const uint8_t *fw_{nullptr};  ///< image: PROGMEM (bundled) or RAM (downloaded)
  size_t fw_len_{0};
  bool fw_in_ram_{false};
  std::string fw_version_;  ///< version of the available release, "" = none known

  uint32_t start_timeout_{8000};
  uint32_t packet_timeout_{5000};
  uint32_t verify_timeout_{60000};
  uint8_t retries_{3};

  Phase phase_{Phase::IDLE};
  uint32_t offset_{0};
  uint16_t packet_size_{256};
  uint16_t last_len_{0};
  uint8_t tries_{0};
  uint32_t sent_at_{0};
  uint32_t verify_start_{0};
  bool product_seen_{false};
  float last_published_progress_{-1};

  std::vector<uint8_t> rx_;
  HighFrequencyLoopRequester high_freq_;

#ifdef USE_TUYA_MCU_DFU_RUNTIME
  bool fetch_manifest_();
  bool download_image_();
  void free_image_();

  http_request::HttpRequestComponent *http_{nullptr};
  const char *source_url_{""};
  const char *chip_family_{""};
  uint32_t max_size_{65536};
  uint32_t check_interval_{0};
  std::string firmware_url_;
  size_t expected_size_{0};
  uint8_t *ram_{nullptr};
  size_t ram_size_{0};
#endif
};

}  // namespace esphome::tuya_mcu_dfu

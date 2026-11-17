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
/// upgrade protocol, as a Tuya module (TuyaOS) sends it:
///   module -> MCU  0x0A [size u32 BE]              MCU -> 0x0A [pkt size code]
///                  (channel > 9: [size u32 BE][channel][0 0 0])
///   module -> MCU  0x0B [offset][data]             MCU -> 0x0B []
///   module -> MCU  0x0B [offset = size]            (end, ack optional)
///   module -> MCU  0x01                            MCU -> 0x01 {"v":"<new>",..}
/// Packet size code 0..5 = 256, 512, 1024, 2048, 3072, 4096 bytes. The offset
/// is u32 BE, or u16 BE if the MCU's 0x0A reply frame has version byte 0.
/// While transferring and verifying, the `tuya:` component's loop is suspended
/// so this component sees the MCU's replies (`tuya:` then reports is_idle();
/// code that writes datapoints should hold off meanwhile). Afterwards `tuya:`
/// resumes and re-initialises with the restarted MCU.
///
/// The installed version is read from the MCU's product information (0x01):
/// each time `tuya:` finishes initialising, this component borrows the UART
/// for one query (a fraction of a second). Datapoint reports the MCU sends in
/// that window are recovered with a status query (0x08) afterwards.
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
  void set_channel(uint8_t channel) { this->channel_ = channel; }
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

  bool is_updating() const { return this->phase_ != Phase::IDLE && this->phase_ != Phase::QUERY; }

 protected:
  enum class Phase : uint8_t { IDLE, START, DATA, FINISH, VERIFY, QUERY };

  bool has_release_() const { return !this->fw_version_.empty(); }
  void refresh_state_();
  void begin_transfer_();
  void verified_(const char *how);
  void release_uart_(bool poke);
  void send_frame_(uint8_t cmd, const uint8_t *payload, size_t len);
  bool flush_tx_();
  void schedule_version_query_();
  void begin_version_query_();
  void end_version_query_();
  void watch_reinit_();
  uint8_t wifi_status_code_() const;
  void resync_tuya_();
  void on_heartbeat_(const uint8_t *data, size_t len);
  void send_start_();
  void send_packet_();
  void send_end_();
  void send_product_query_();
  void on_frame_(uint8_t ver, uint8_t cmd, const uint8_t *data, size_t len);
  void send_offset_frame_(uint32_t offset, size_t n);
  bool check_image_();
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

  uint8_t channel_{0};

  uint32_t start_timeout_{5000};
  uint32_t packet_timeout_{5000};
  uint32_t verify_timeout_{60000};
  uint8_t retries_{3};

  Phase phase_{Phase::IDLE};
  uint32_t offset_{0};
  uint16_t packet_size_{256};
  uint8_t offset_width_{4};  ///< 4, or 2 if the MCU's 0x0A reply has frame version 0
  uint16_t last_len_{0};
  uint8_t tries_{0};
  uint32_t sent_at_{0};          ///< when the last frame finished going out
  std::vector<uint8_t> tx_;       ///< frame being written (large frames go out over several loops)
  size_t tx_pos_{0};
  bool probe_sent_{false};        ///< START: heartbeat sent to see if a bootloader runs
  bool restarted_seen_{false};    ///< START: "first heartbeat" (0x00 [00]) already acted on
  bool collided_{false};          ///< DATA: a heartbeat reply came instead of the ack
  uint8_t last_rx_ver_{0};        ///< protocol version of the MCU's last frame
  uint8_t query_tries_{0};
  bool swallowed_dp_{false};      ///< QUERY: a datapoint report arrived while tuya: was paused
  bool restart_unseen_{false};    ///< the MCU restarted, but tuya: was paused and didn't see it
  uint8_t reinit_ticks_{0};
  uint8_t reinit_wifi_seen_{0};
  bool reinit_begun_{false};      ///< tuya: has started re-initialising since the watch began
  uint32_t verify_start_{0};
  bool product_seen_{false};
  bool verify_hb_seen_{false};     ///< VERIFY: the MCU has answered a heartbeat since the transfer
  uint32_t verify_hb_at_{0};
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

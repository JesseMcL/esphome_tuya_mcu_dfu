# esphome_tuya_mcu_dfu

An ESPHome external component that updates the firmware of the **MCU behind a Tuya Wi-Fi module**: the microcontroller ESPHome talks to with the [`tuya:`](https://esphome.io/components/tuya.html) component. It appears in Home Assistant as a normal **update** entity, with an installed version, a latest version, progress and an Install button.

The image is sent with the standard **Tuya MCU upgrade protocol** (`0x0A` / `0x0B`). It is **bundled into the ESPHome build** from a release manifest, so it works on every ESPHome platform, including BK72xx/LibreTiny, which has no `http_request`.

```
 Home Assistant ── update entity ── ESPHome (tuya: + tuya_mcu_dfu)
                                       │ UART, TuyaMCU frames
                                       ▼ 0x0A start, 0x0B data …
                                      MCU
```

## Usage

```yaml
external_components:
  - source: github://JesseMcL/esphome_tuya_mcu_dfu@main
    components: [tuya_mcu_dfu]

uart:
  id: mcu_uart
  tx_pin: P11
  rx_pin: P10
  baud_rate: 115200

tuya:
  id: mcu
  uart_id: mcu_uart

update:
  - platform: tuya_mcu_dfu
    name: MCU firmware
    uart_id: mcu_uart                 # the same UART as tuya:
    source: https://github.com/you/your-mcu-fw/releases/latest/download/manifest.json
    version_datapoint: 120            # optional, see below
```

| Option | Default | |
|---|---|---|
| `source` | required | Release manifest: an `http(s)://` URL, or a path relative to the YAML. Fetched when ESPHome compiles. |
| `uart_id` | | The UART used by `tuya:`. |
| `tuya_id` | the only `tuya:` | |
| `version_datapoint` | none | String datapoint in which the MCU reports its firmware version. |
| `chip_family` | the only build | Selects `builds[].chipFamily` in the manifest. Required if it has several builds. |
| `required` | `false` | Fail the build if the manifest or image can't be fetched. If `false`, the build continues without firmware: a warning is logged and the entity has no state. |
| `start_timeout` | `8s` | Wait for the MCU's reply to `0x0A`. |
| `packet_timeout` | `5s` | Wait for each `0x0B` acknowledgement. |
| `verify_timeout` | `60s` | Wait for the MCU to come back with the new firmware. |
| `retries` | `3` | Retries per frame before giving up. |

All the usual [update entity](https://esphome.io/components/update/) options, triggers and actions (`update.perform`) work as well.

**Rolling out a new MCU release:** publish it (a new manifest at `source`), recompile and OTA ESPHome, then press **Install**.

### Version datapoint

With `version_datapoint`, the entity shows the MCU's version as installed, and offers the update only when it differs from the bundled one. After the transfer, the update counts as verified once the MCU reports the new version.

Tuya has no standard way for an MCU to report its version to the module. The product-info reply (`0x01`) carries a `v` field, but ESPHome's `tuya:` doesn't expose it. Firmware you write can report the version in a string DP. Stock firmware can't, so leave the option out for it: the installed version then shows as unknown and the update is always offered. In that case the update counts as verified when `tuya:` finishes initialising again after the MCU restarts.

## Manifest

The manifest uses the same shape as ESPHome's own firmware manifests. The image path is relative to the manifest (or a full URL). The `md5` is checked at build time.

```json
{
  "name": "My MCU firmware",
  "version": "1.2.0",
  "builds": [
    {
      "chipFamily": "MS51FC0AE",
      "ota": {
        "path": "mcu-1.2.0.bin",
        "md5": "0123456789abcdef0123456789abcdef",
        "release_url": "https://github.com/you/your-mcu-fw/releases/tag/v1.2.0",
        "summary": "Shown in Home Assistant"
      }
    }
  ]
}
```

`tools/make_manifest.py` writes one:

```sh
python tools/make_manifest.py mcu-1.2.0.bin --version 1.2.0 --chip-family MS51FC0AE \
  --name "My MCU firmware" -o manifest.json
```

Upload both files as GitHub release assets, and `…/releases/latest/download/manifest.json` always points to the newest release. Downloads are cached by MD5 in `.esphome/tuya_mcu_dfu/`.

## Protocol

| Direction | Frame | |
|---|---|---|
| module → MCU | `0x0A` size (u32 BE) | start |
| MCU → module | `0x0A` packet size: `0` = 256, `1` = 512, `2` = 1024 bytes | accepted |
| module → MCU | `0x0B` offset (u32 BE) + data | one packet |
| MCU → module | `0x0B` (empty) | acknowledged; next packet |
| module → MCU | `0x0B` size (u32 BE), no data | end. An acknowledgement is optional |

While transferring, the component suspends `tuya:`'s loop so it can read the MCU's replies itself. Other code that writes datapoints should hold off until the update finishes; `tuya:` reports `is_idle()` during the transfer. After the end frame, `tuya:` resumes and gets a heartbeat straight away. An MCU that restarted answers that heartbeat with `0x00`, which makes `tuya:` initialise again.

**What the image is, and where it goes, is up to the MCU.** Tuya doesn't define an image format, signature or checksum for MCU firmware. Each vendor's MCU (or its bootloader) decides what to do with the bytes it receives. This component only transports them.

> **ESPHome `tuya:` re-init quirk.** After an MCU restart, `tuya:` re-runs its init but waits forever at the Wi-Fi-status step: it only sends the Wi-Fi state when that state changes. Firmware you write can work around this by replying to the `0x02` (configuration) query with an extra, unsolicited `0x03` acknowledgement, followed by a `0x2B` network-status query.

## Caveats

- **A failed update can leave the MCU without working firmware.** Unless its bootloader keeps the old image or offers a rescue path, recovery means flashing the MCU directly (ICP/SWD and so on).
- **Stock MCU firmware is closed.** Whether a given stock MCU accepts an update, and what image it expects, is unknown until you try it.
- **The image lives in the ESPHome binary.** It is stored in flash as PROGMEM, so its size counts against the module's flash.

## Tests

`tests/run.sh` compiles ESPHome for the `host` platform and updates `tests/fake_mcu.py`, a minimal TuyaMCU on a pseudo-terminal. It runs once with a version datapoint and once without. It needs `esphome`, `socat` and `python3`. CI runs it against the current ESPHome release and the current pre-release.

## Used by

- [GL-C-301W_ESPHome](https://github.com/JesseMcL/GL-C-301W_ESPHome): open firmware for the Gledopto GL-C-301W, including an MS51 bootloader that implements this protocol.

## License

[MIT](LICENSE). ESPHome itself is GPL-3.0 (C++) / MIT (Python), so firmware built with it is distributed under GPL-3.0.

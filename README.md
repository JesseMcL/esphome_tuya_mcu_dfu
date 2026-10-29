# esphome_tuya_mcu_dfu

An ESPHome external component that updates the firmware of the **MCU behind a Tuya Wi-Fi module**: the microcontroller ESPHome talks to with the [`tuya:`](https://esphome.io/components/tuya.html) component. It appears in Home Assistant as a normal **update** entity, with an installed version, a latest version, progress and an Install button.

The image is sent with the standard [Tuya MCU upgrade protocol](https://developer.tuya.com/en/docs/mcu-standard-protocol/MCUSDK-wifi-mcuota?id=Kd2fv0vlx6wp4) (`0x0A` / `0x0B`). It comes from a release manifest, which is fetched in one of two ways:

- **When ESPHome compiles** (`fetch: build`, the default). The image is bundled into the ESPHome binary. This works on every platform, including BK72xx/LibreTiny, which has no `http_request`.
- **By the device itself** (`fetch: runtime`), using [`http_request`](https://esphome.io/components/http_request/) (ESP32, ESP8266, RP2040, host). New MCU releases show up without rebuilding ESPHome.

```
 Home Assistant ── update entity ── ESPHome (tuya: + tuya_mcu_dfu)
                                       │ UART, TuyaMCU frames
                                       ▼ 0x0A start, 0x0B data …, 0x01 version check
                                      MCU
```

> ## ⚠️ Read this first
>
> **The Tuya MCU upgrade command is an opaque transport.** Tuya defines how the bytes are carried (start, offset-addressed packets, end) and nothing about what they are. There is no image format, header, signature or checksum beyond the per-frame byte sum. What the MCU does with the bytes is entirely up to its vendor's firmware or bootloader: whether it accepts them, where it writes them, whether it checks them, and what happens if they are wrong.
>
> In practice this means:
>
> - **Only send an MCU image built for that exact MCU and bootloader.** There is no way to discover from the outside what a stock MCU expects. A wrong image, or a correct one the MCU mishandles, can leave it **without working firmware**. Recovering it then needs the programming pins (ICP, SWD and so on) and a way to restore or replace its firmware. Stock MCU firmware is usually read-protected, so a backup is often impossible.
> - **The MD5 in the manifest only checks the download.** It proves you got the file you published, not that the MCU will accept it.
> - **Tuya's own platform** hosts and pushes MCU images only for products where the vendor uploaded one, so for most devices no official image exists to compare with.
> - **A Tuya-certified module** waits for the MCU to report the new version in its product information (`0x01`) and otherwise reports failure. This component does the same (see [Verification](#verification)), but it cannot undo anything the MCU did.
>
> Use this with MCU firmware you control, or after you have a tested way to recover the MCU. You are responsible for what you send to it.

### Intended use

In theory this component could replace a device's **stock** Tuya MCU firmware, since it speaks the same protocol a Tuya module uses. It is **designed for MCUs where you already control the code**: you wrote or built the firmware running on the MCU, and you know how it handles the upgrade command (typically your own bootloader). That is the setup it has been tested in.

Replacing stock firmware is a different problem, and a device-specific one:

- **The stock updater decides what it accepts.** Its expected image format, target address and checks are unknown.
- **An update over this protocol is usually limited to what that updater writes**, typically the application area. Whether the rest of the chip can change depends on the MCU and on code you get to run on it:
  - the bootloader region;
  - the size of the boot/application partitions;
  - configuration and option bytes (boot source, reset pin, read protection, watchdog).

  Some MCUs let running code reprogram all of that. For example, the Nuvoton MS51 can rewrite its LDROM and CONFIG bytes, including the LDROM size, by IAP from the application. Others can only be changed with a programmer.
- **Partition changes are risky without a recovery path.** A first image that changes the partitioning, the bootloader or option bytes from inside the MCU has to survive power loss at every step. Without a programmer as a fallback, a mistake there is usually permanent.

If you plan to try it, start with a device you can recover by ICP, SWD or similar.

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

Runtime fetch on targets with `http_request`:

```yaml
http_request:

update:
  - platform: tuya_mcu_dfu
    name: MCU firmware
    uart_id: mcu_uart
    fetch: runtime
    source: https://github.com/you/your-mcu-fw/releases/latest/download/manifest.json
    update_interval: 6h               # how often to check the manifest
    max_size: 131072                  # largest image to download into RAM
```

| Option | Default | |
|---|---|---|
| `source` | required | Release manifest: an `http(s)://` URL, or, with `fetch: build` only, a path relative to the YAML. |
| `fetch` | `build` | `build`: fetched when ESPHome compiles and bundled. `runtime`: fetched by the device with `http_request`. |
| `uart_id` | | The UART used by `tuya:`. |
| `tuya_id` | the only `tuya:` | |
| `version_datapoint` | none | String datapoint in which the MCU reports its firmware version (see below). |
| `chip_family` | the only build | Selects `builds[].chipFamily` in the manifest. Required if it has several builds. |
| `start_timeout` | `8s` | Wait for the MCU's reply to `0x0A`. |
| `packet_timeout` | `5s` | Wait for each `0x0B` acknowledgement. |
| `verify_timeout` | `60s` | Wait for the MCU to come back with the new version. |
| `retries` | `3` | Retries per frame before giving up. |
| **`fetch: build` only** | | |
| `required` | `false` | Fail the build if the manifest or image can't be fetched. If `false`, the build continues without firmware: a warning is logged and the entity has no state. |
| **`fetch: runtime` only** | | |
| `http_request_id` | the only `http_request:` | |
| `update_interval` | `6h` | How often to check the manifest. The first check is 15 s after boot. `update.check` triggers one manually. |
| `max_size` | `65536` | Largest image accepted, in bytes. The image is downloaded **into RAM**, checked against the manifest MD5, and freed after the update. |

All the usual [update entity](https://esphome.io/components/update/) options, triggers, conditions and actions work as well (`update.check`, `update.perform`, `update.is_available`).

**Rolling out a new MCU release:**

- With `fetch: build`: publish it, recompile and OTA ESPHome, then press **Install**.
- With `fetch: runtime`: publish it, then wait for the next check (or run `update.check`) and press **Install**.

### Runtime fetch: things to know

- **Downloading blocks the main loop**, like ESPHome's own `http_request` OTA on most platforms. The manifest is usually under 1 KB. A 64 KB image over Wi-Fi typically takes a second or two.
- **The download happens when you press Install**, not at check time, so the RAM is only used during an update. The MD5 is checked before anything is sent to the MCU. A mismatch, a short download or an oversized image aborts the update, and the MCU is not touched.
- **HTTPS** follows `http_request`'s settings: the certificate bundle on ESP32, or `verify_ssl: false`. GitHub `…/releases/latest/download/…` redirects; `http_request` follows them by default.

### Version and verification

The entity's installed version comes from:

- **`version_datapoint`**, if set: a string DP your MCU firmware reports.
- Otherwise it is **unknown**, and the update is always offered. `tuya:` reads the MCU's product information but doesn't expose it.

<a id="verification"></a>After the transfer, the component follows the Tuya spec. It asks the MCU for its product information (`0x01`) every second and checks the `"v"` field against the new version:

| MCU replies | Result |
|---|---|
| `"v"` equals the new version | verified |
| another `"v"`, or no reply, with `version_datapoint` set | `tuya:` re-initialises and the DP has to show the new version before `verify_timeout` |
| another `"v"`, or no reply, without `version_datapoint` | keep asking until `verify_timeout`, then fail |
| no `"v"` at all, without `version_datapoint` | accepted with a warning: the MCU runs, but its version can't be checked |

## Manifest

The manifest uses the same shape as ESPHome's own firmware manifests. The image path is relative to the manifest (or a full URL). `size` is optional and used to check runtime downloads.

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
        "size": 8811,
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

Upload both files as GitHub release assets, and `…/releases/latest/download/manifest.json` always points to the newest release. With `fetch: build`, downloads are cached by MD5 in `.esphome/tuya_mcu_dfu/`.

## Protocol

| Direction | Frame | |
|---|---|---|
| module → MCU | `0x0A` size (u32 BE) | start |
| MCU → module | `0x0A` packet size: `0` = 256, `1` = 512, `2` = 1024 bytes | accepted |
| module → MCU | `0x0B` offset (u32 BE) + data | one packet |
| MCU → module | `0x0B` (empty) | acknowledged; next packet |
| module → MCU | `0x0B` size (u32 BE), no data | end. An acknowledgement is optional |
| module → MCU | `0x01` | product information request |
| MCU → module | `0x01` `{"p":…,"v":"<version>",…}` | must carry the new version within a minute (Tuya spec) |

While transferring and verifying, the component suspends `tuya:`'s loop so it can read the MCU's replies itself. Code that writes datapoints should hold off until the update finishes; `tuya:` reports `is_idle()` during that time. Afterwards `tuya:` resumes and gets a heartbeat straight away. An MCU that restarted answers that heartbeat with `0x00`, which makes `tuya:` initialise again.

> **ESPHome `tuya:` re-init quirk.** After an MCU restart, `tuya:` re-runs its init but waits forever at the Wi-Fi-status step: it only sends the Wi-Fi state when that state changes. Firmware you write can work around this by replying to the `0x02` (configuration) query with an extra, unsolicited `0x03` acknowledgement, followed by a `0x2B` network-status query.

## Known uses outside an official Tuya module

As of late 2026, I found **no public report of a *stock* Tuya MCU firmware being successfully updated through this protocol by anything other than a Tuya-certified module.** What exists:

| Project | MCU | Status |
|---|---|---|
| [GL-C-301W_ESPHome](https://github.com/JesseMcL/GL-C-301W_ESPHome) (this component's origin) | Nuvoton MS51FC0AE in a Gledopto GL-C-301W | Replacement MCU firmware with its own LDROM bootloader that implements this protocol. The first install is by ICP. Updates from ESPHome through that bootloader are **confirmed on hardware**. Replacing the **stock** MCU firmware through its own updater is untested. |
| [OpenSSS10](https://github.com/lwalkera/OpenSSS10) | FT32F030C8AT7 in a Tuya Wi-Fi smart switch | Replacement MCU firmware that runs behind the stock bootloader. `make ota` sends it through OpenBeken with `0x0A`/`0x0B`, which needs OpenBeken MCU-update support that was still a proposed pull request. The README recommends a first flash by SWD and calls the stock OTA path untested. |
| [Open-TuyaMCU for Nuvoton](https://www.elektroda.com/rtvforum/topic3963639.html) (elektroda, 2023) | Nuvoton in a Treatlife DS02S | Replacement MCU firmware flashed by SWD. The author notes the Tuya update protocol is documented but not implemented. |
| [Tuya Module Debugging Assistant](https://www.elektroda.com/rtvforum/topic4085796.html) (elektroda, 2024) | Simulated | The reverse direction: Tuya's PC tool impersonates an MCU, so the cloud pushes the vendor's MCU image to it and it saves the file. Useful for obtaining an official image, but nothing was flashed. |

Successful updates (or failures) with this component are welcome as issues, with device, MCU, stock or custom firmware, and what happened.

## Tests

`tests/run.sh` compiles ESPHome for the `host` platform and drives `tests/fake_mcu.py`, a minimal TuyaMCU on a pseudo-terminal:

| Config | Checks |
|---|---|
| `host.yaml` | Bundled image, version DP. |
| `host_no_version_dp.yaml` | Bundled image, verified through product information only. |
| `host_runtime.yaml` | `http_request` download from a local HTTP server. |
| `host_runtime_bad_md5.yaml` | Manifest MD5 doesn't match: the download is rejected, nothing is sent. |

It needs `esphome`, `socat`, `python3` and `libssl-dev`. CI runs it against the current ESPHome release and the current pre-release, and compiles `tests/esp32c3_runtime.yaml` for an ESP32-C3.

## Used by

- [GL-C-301W_ESPHome](https://github.com/JesseMcL/GL-C-301W_ESPHome): open firmware for the Gledopto GL-C-301W, including an MS51 bootloader that implements this protocol.

## License

[MIT](LICENSE). ESPHome itself is GPL-3.0 (C++) / MIT (Python), so firmware built with it is distributed under GPL-3.0.

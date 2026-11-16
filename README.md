# esphome_tuya_mcu_dfu

An ESPHome external component that updates the firmware of the **MCU behind a Tuya Wi-Fi module**: the microcontroller ESPHome talks to with the [`tuya:`](https://esphome.io/components/tuya.html) component. It appears in Home Assistant as a normal **update** entity, with an installed version, a latest version, progress and an Install button.

The image is sent with the standard [Tuya MCU upgrade protocol](https://developer.tuya.com/en/docs/mcu-standard-protocol/MCUSDK-wifi-mcuota?id=Kd2fv0vlx6wp4) (`0x0A` / `0x0B`), the way a Tuya module (TuyaOS) sends it: all six packet sizes, 16- or 32-bit offsets and the firmware channel. Its MD5 is checked on the device before anything is sent. The image comes from a release manifest, or from a URL or file pinned by its hash, fetched in one of two ways:

- **When ESPHome compiles** (`fetch: build`, the default). The image is bundled into the ESPHome binary. This works on every platform, including BK72xx/LibreTiny, which has no `http_request`. If the image can't be fetched or doesn't match its hash or size, **the build fails**.
- **By the device itself** (`fetch: runtime`), using [`http_request`](https://esphome.io/components/http_request/) (ESP32, ESP8266, RP2040, host). New MCU releases show up without rebuilding ESPHome. The build checks the release if it can reach it, but only warns, since it may be published later.

The installed version is read from the MCU's product information, so Home Assistant shows what the MCU actually runs.

Devices it has been used with, stock and custom MCU firmware: **[known_devices.md](known_devices.md)**.

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

This component can replace a device's **stock** Tuya MCU firmware, since it speaks the same protocol a Tuya module uses, and it has done so on at least one device (see [known_devices.md](known_devices.md)). It is **designed for MCUs where you control the code, or whose updater is known**: you wrote or built the firmware running on the MCU and know how it handles the upgrade command (typically your own bootloader), or someone has tried the stock updater on that exact device.

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

A single image pinned by its hash, without a manifest (`fetch: build` only):

```yaml
update:
  - platform: tuya_mcu_dfu
    name: MCU firmware
    uart_id: mcu_uart
    firmware:
      url: https://example.com/mcu-1.0.5.bin   # or path: relative to the YAML
      version: 1.0.5                           # what the MCU reports afterwards
      sha256: b22da684…                        # or md5; required for url
      size: 20480                              # optional
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
| `source` | | Release manifest: an `http(s)://` URL, or, with `fetch: build` only, a path relative to the YAML. Give `source` or `firmware`. |
| `firmware` | | `fetch: build` only, instead of `source`: one image. `url` or `path` (relative to the YAML); `version` (required); `sha256` and/or `md5` (required with `url`, optional with `path`); optional `size`, `title`, `summary`, `release_url`. |
| `fetch` | `build` | `build`: fetched when ESPHome compiles and bundled. `runtime`: fetched by the device with `http_request`. |
| `uart_id` | | The UART used by `tuya:`. |
| `tuya_id` | the only `tuya:` | |
| `version_datapoint` | none | String datapoint in which the MCU reports its firmware version. Only needed if its product information has no `"v"` (see below). |
| `chip_family` | the only build | Selects `builds[].chipFamily` in the manifest. Required if it has several builds. |
| `channel` | none | Firmware channel (type). Above 9, the `0x0A` start frame carries it (8-byte form), as a Tuya module does; 0–9 send the size alone. |
| `start_timeout` | `5s` | Wait for the MCU's reply to `0x0A`. |
| `packet_timeout` | `5s` | Wait for each `0x0B` acknowledgement. |
| `verify_timeout` | `60s` | Wait for the MCU to come back with the new version. |
| `retries` | `3` | Retries per frame before giving up. |
| **`fetch: runtime` only** | | |
| `http_request_id` | the only `http_request:` | |
| `update_interval` | `6h` | How often to check the manifest. The first check is 15 s after boot. `update.check` triggers one manually. |
| `max_size` | `65536` | Largest image accepted, in bytes. The image is downloaded **into RAM**, checked against the manifest MD5, and freed after the update. |

All the usual [update entity](https://esphome.io/components/update/) options, triggers, conditions and actions work as well (`update.check`, `update.perform`, `update.is_available`).

### Build-time checks

With `fetch: build`, the image is part of the ESPHome binary, so a problem with the source **fails the build**: a manifest or file that can't be read, a URL that can't be fetched, a manifest without `version`, `ota.path` or `ota.md5`, no build for `chip_family`, or an image that doesn't match its MD5, SHA-256 or `size`. A firmware that would offer an update it can't install is never built. Downloads are cached by hash, so a pinned image still builds offline once fetched.

With `fetch: runtime`, the build fetches the manifest and image if it can and checks the same things, but only logs a warning: the device fetches the release itself, and it may not be published yet.

> **Remote builds.** Relative paths are resolved from the YAML's directory. If ESPHome builds on another machine (a remote build server, for example), files next to your YAML may not be copied there and the build fails with "can't read". Use an `http(s)://` URL for the manifest or `firmware:` instead.

**Rolling out a new MCU release:**

- With `fetch: build`: publish it, recompile and OTA ESPHome, then press **Install**.
- With `fetch: runtime`: publish it, then wait for the next check (or run `update.check`) and press **Install**.

### Runtime fetch: things to know

- **Downloading blocks the main loop**, like ESPHome's own `http_request` OTA on most platforms. The manifest is usually under 1 KB. A 64 KB image over Wi-Fi typically takes a second or two.
- **The download happens when you press Install**, not at check time, so the RAM is only used during an update. A short download or an oversized image aborts the update, and the MCU is not touched.
- **HTTPS** follows `http_request`'s settings: the certificate bundle on ESP32, or `verify_ssl: false`. GitHub `…/releases/latest/download/…` redirects; `http_request` follows them by default.

### Image check before sending

Before the `0x0A` start frame, `perform()` checks the image's MD5 against the release's, for bundled and downloaded images alike. A mismatch (a bad download, or a corrupted bundled copy) logs `MD5 mismatch, image rejected`, sets an error and sends nothing. Bundled images are also checked when ESPHome compiles: against the manifest's `md5` (and `size`), or the `firmware:` hash.

### Version and verification

The installed version is the `"v"` of the MCU's product information (`0x01`, e.g. `{"p":"…","v":"1.0.5","m":1}`). `tuya:` reads it while it initialises but doesn't expose it, so each time `tuya:` finishes initialising (at boot, and after the MCU restarts), this component asks once itself. It suspends `tuya:` for that query, typically well under a second; if the MCU reports a datapoint meanwhile, it asks the MCU for all datapoints (`0x08`) afterwards so `tuya:` doesn't miss it.

If the product information has no `"v"`, set **`version_datapoint`** to a string DP your MCU firmware reports. Without either, the installed version is unknown and the update is always offered.

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

Upload both files as GitHub release assets, and `…/releases/latest/download/manifest.json` always points to the newest release. With `fetch: build`, downloads are cached by their hash in `.esphome/tuya_mcu_dfu/`.

## Protocol

As sent by TuyaOS 3.x (reverse engineered from a BK7231N module's firmware; see [known_devices.md](known_devices.md)):

| Direction | Frame | |
|---|---|---|
| module → MCU | `0x0A` size (u32 BE); with `channel` > 9: size, channel, 3 zero bytes | start |
| MCU → module | `0x0A` packet size code: `0` (or empty) = 256, `1` = 512, `2` = 1024, `3` = 2048, `4` = 3072, `5` = 4096 bytes | accepted. Frame version byte `0` = 16-bit offsets from here on, otherwise 32-bit |
| module → MCU | `0x0B` offset (u32 or u16 BE) + data | one packet |
| MCU → module | `0x0B` (empty) | acknowledged; next packet |
| module → MCU | `0x0B` offset = size, no data | end. An acknowledgement is optional |
| module → MCU | `0x01` | product information request |
| MCU → module | `0x01` `{"p":…,"v":"<version>",…}` | must carry the new version within a minute (Tuya spec) |

Nothing else is added: no image header, no checksum besides each frame's byte sum. A module tries each frame 3 times with a 5 s timeout, the defaults here too.

While transferring and verifying, the component suspends `tuya:`'s loop so it can read the MCU's replies itself. Code that writes datapoints should hold off until the update finishes; `tuya:` reports `is_idle()` during that time.

- **Heartbeats keep going.** `tuya:` sends one every 15 s regardless, and the MCU answers it. During `START`, a "first heartbeat" reply (`0x00` `[00]`) means the MCU has just restarted (an application handing over to its bootloader), and `0x0A` is sent again at once; a heartbeat probe 1 s after an unanswered `0x0A` finds this out early. During `DATA`, a heartbeat reply where the acknowledgement should be means the heartbeat crossed the packet, which the MCU may have dropped: the packet is sent again after 1 s instead of the full `packet_timeout`.
- **Large packets don't block.** A data packet is written a chunk per loop (a 4096-byte packet takes over 4 s at 9600 baud), and `packet_timeout` counts from its last byte.

Afterwards `tuya:` resumes and gets a heartbeat straight away. An MCU that restarted answers that heartbeat with `0x00`, which makes `tuya:` initialise again.

> **ESPHome `tuya:` re-init quirk.** After an MCU restart, `tuya:` re-runs its init but waits forever at the Wi-Fi-status step: it only sends the Wi-Fi state when that state changes. After an update the component watches for this and sends the Wi-Fi status itself, which the MCU acknowledges and `tuya:` moves on. If the MCU's "first heartbeat" reply reached the component instead of `tuya:` (so `tuya:` doesn't know the MCU restarted), it sends the Wi-Fi status and asks for all datapoints itself. Firmware you write can also avoid the stall by replying to the `0x02` (configuration) query with an extra, unsolicited `0x03` acknowledgement, followed by a `0x2B` network-status query.

### After an update

Some devices may expect or require a power cycle to function properly after an update: the MCU restarts, but parts it controls (sensors, radios, other chips on the board) may not, and can be left in a state the new firmware doesn't expect. If the device misbehaves after an update that verified, power-cycle it before anything else.

## Known devices

[known_devices.md](known_devices.md) lists devices this component has been used with, stock and custom MCU firmware, and related projects. Reports of successful updates (or failures) are welcome as issues: device, MCU, stock or custom firmware, and what happened.

## Tests

`tests/run.sh` compiles ESPHome for the `host` platform and drives `tests/fake_mcu.py`, a minimal TuyaMCU on a pseudo-terminal:

| Config | Checks |
|---|---|
| `host.yaml` | Bundled image from a manifest, version DP. Repeated against every packet size code (0–5), 16-bit offsets (reply frame version 0), an MCU that ignores the first `0x0A` and restarts into its bootloader, and a packet lost to a crossing heartbeat. |
| `host_reinit.yaml` | The restarted MCU waits for the Wi-Fi status (the `tuya:` stall), also with its "first heartbeat" reply going to the component instead of `tuya:`. |
| `host_no_version_dp.yaml` | Verified through product information only. |
| `host_boot_version.yaml` | The MCU already runs the release: version read at boot, no update offered; also with a datapoint report arriving during that query. |
| `host_channel.yaml` | `channel: 10`: the 8-byte start frame. |
| `host_inline_url.yaml` | `firmware:` from a URL, pinned by SHA-256 and size. |
| `host_inline_path.yaml` | `firmware:` from a local file, no hash. |
| `host_inline_bad_hash.yaml`, `host_missing_manifest.yaml`, `host_unreachable_url.yaml`, `host_manifest_bad_md5.yaml`, `host_no_chip_family.yaml` | `fetch: build` source problems: the build fails (`esphome config`). |
| `host_runtime_unpublished.yaml` | `fetch: runtime`, release not published: the build only warns. |
| `host_runtime.yaml` | `http_request` download from a local HTTP server. |
| `host_runtime_bad_md5.yaml` | Manifest MD5 doesn't match: the image is rejected before `0x0A`, nothing is sent. |

The fake MCU checks the start frame length, every offset, packet sizes and the end frame. `ONLY=<regex> tests/run.sh` runs a subset.

It needs `esphome`, `socat`, `python3` and `libssl-dev`. CI runs it against the current ESPHome release and the current pre-release, and compiles `tests/esp32c3_runtime.yaml` for an ESP32-C3.

## Used by

- [GL-C-301W_ESPHome](https://github.com/JesseMcL/GL-C-301W_ESPHome): open firmware for the Gledopto GL-C-301W, including an MS51 bootloader that implements this protocol.

## License

[MIT](LICENSE). ESPHome itself is GPL-3.0 (C++) / MIT (Python), so firmware built with it is distributed under GPL-3.0.

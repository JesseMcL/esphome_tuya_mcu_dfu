# Known devices

Devices `tuya_mcu_dfu` has been used with, and related projects. Reports of successful updates (or failures) are welcome as issues: device, MCU, stock or custom firmware, and what happened.

## Used with this component

| Device | MCU | Firmware | Status |
|---|---|---|---|
| Tuya PS10 presence sensor (board NAS-PS10W6) | FMD FT61E0A5 | Stock | **Confirmed on hardware**: stock 1.0.4 updated to the official 1.0.5 image through the stock updater, then to a modified image, and rolled back from it to 1.0.5 (see [Rollback and downgrades](README.md#rollback-and-downgrades)). The radar kept working after the rollback. |
| Gledopto GL-C-301W ([GL-C-301W_ESPHome](https://github.com/JesseMcL/GL-C-301W_ESPHome), this component's origin) | Nuvoton MS51FC0AE | Custom | Replacement MCU firmware with its own bootloader implementing this protocol (first install by ICP). Updates through that bootloader are **confirmed on hardware**. Updating the **stock** MCU firmware through its own updater is untested. |

Thanks to [openshwprojects/FlashDumps](https://github.com/openshwprojects/FlashDumps), whose collection includes the Tuya CDN link for the official PS10 1.0.5 MCU image (under `IoT/zOTAz/Tuya`, channel 9). The stock 1.0.4 image isn't available, so an update can't be rolled back to it.

## Related projects

| Project | MCU | Notes |
|---|---|---|
| [OpenSSS10](https://github.com/lwalkera/OpenSSS10) | FT32F030C8AT7 in a Tuya Wi-Fi smart switch | Replacement MCU firmware that runs behind the stock bootloader. `make ota` sends it through OpenBeken with `0x0A`/`0x0B`, which needs OpenBeken MCU-update support that was still a proposed pull request. The README recommends a first flash by SWD and calls the stock OTA path untested. |
| [Open-TuyaMCU for Nuvoton](https://www.elektroda.com/rtvforum/topic3963639.html) (elektroda, 2023) | Nuvoton in a Treatlife DS02S | Replacement MCU firmware flashed by SWD. The author notes the Tuya update protocol is documented but not implemented. |
| [Tuya Module Debugging Assistant](https://www.elektroda.com/rtvforum/topic4085796.html) (elektroda, 2024) | Simulated | The reverse direction: Tuya's PC tool impersonates an MCU, so the cloud pushes the vendor's MCU image to it and it saves the file. Useful for obtaining an official image, but nothing was flashed. |

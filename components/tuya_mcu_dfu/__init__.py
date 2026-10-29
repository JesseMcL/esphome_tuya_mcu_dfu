"""tuya_mcu_dfu - firmware update for the MCU behind ESPHome's `tuya:`.

  update:
    - platform: tuya_mcu_dfu

Streams a firmware image to the MCU with the standard Tuya MCU upgrade
protocol (0x0A start / 0x0B data). The image is bundled into the ESPHome
build at compile time from an ESPHome-style release manifest.
"""
import esphome.codegen as cg

CODEOWNERS = ["@JesseMcL"]
DEPENDENCIES = ["tuya", "uart"]

tuya_mcu_dfu_ns = cg.esphome_ns.namespace("tuya_mcu_dfu")

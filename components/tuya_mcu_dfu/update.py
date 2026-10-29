"""Tuya MCU firmware update entity.

The release manifest (ESPHome style) and image come from `source`:

  fetch: build    (default) fetched when ESPHome compiles and embedded in the
                  binary. Works on every platform, including BK72xx
                  (LibreTiny), which has no `http_request`.
  fetch: runtime  fetched by the device with `http_request`: the manifest is
                  checked every `update_interval`, the image is downloaded
                  into RAM (up to `max_size`) and MD5-checked when you install.

The update itself uses the standard Tuya MCU upgrade protocol (0x0A / 0x0B).
"""
import hashlib
import json
import logging
from pathlib import Path
import urllib.parse
import urllib.request

import esphome.codegen as cg
from esphome.components import uart, update
from esphome.components.tuya import CONF_TUYA_ID, Tuya
import esphome.config_validation as cv
from esphome.const import (
    CONF_RAW_DATA_ID,
    CONF_SOURCE,
    CONF_UPDATE_INTERVAL,
)
from esphome.core import CORE

from . import tuya_mcu_dfu_ns

_LOGGER = logging.getLogger(__name__)

DEPENDENCIES = ["tuya", "uart"]

CONF_VERSION_DATAPOINT = "version_datapoint"
CONF_CHIP_FAMILY = "chip_family"
CONF_REQUIRED = "required"
CONF_PACKET_TIMEOUT = "packet_timeout"
CONF_START_TIMEOUT = "start_timeout"
CONF_VERIFY_TIMEOUT = "verify_timeout"
CONF_RETRIES = "retries"
CONF_FETCH = "fetch"
CONF_HTTP_REQUEST_ID = "http_request_id"
CONF_MAX_SIZE = "max_size"

FETCH_BUILD = "build"
FETCH_RUNTIME = "runtime"

TuyaMcuDfu = tuya_mcu_dfu_ns.class_(
    "TuyaMcuDfu", update.UpdateEntity, cg.Component, uart.UARTDevice
)


def AUTO_LOAD(config=None):
    # MD5 of the downloaded image is checked on the device in runtime mode
    if config is not None and config.get(CONF_FETCH) == FETCH_RUNTIME:
        return ["md5"]
    return []


def _source(value):
    value = cv.string_strict(value)
    if value.startswith(("http://", "https://")):
        return cv.url(value)
    return value


def _http_request_component():
    from esphome.components.http_request import HttpRequestComponent

    return HttpRequestComponent


_BASE = (
    update.update_schema(TuyaMcuDfu)
    .extend(
        {
            cv.GenerateID(CONF_TUYA_ID): cv.use_id(Tuya),
            # Release manifest: URL, or (build only) path relative to the YAML
            cv.Required(CONF_SOURCE): _source,
            # builds[].chipFamily to use; may be left out if there is one build
            cv.Optional(CONF_CHIP_FAMILY): cv.string,
            # String DP in which the MCU reports its firmware version. Without
            # it, the "v" of the MCU's product information (0x01) is used.
            cv.Optional(CONF_VERSION_DATAPOINT): cv.uint8_t,
            cv.Optional(CONF_START_TIMEOUT, default="8s"): cv.positive_time_period_milliseconds,
            cv.Optional(CONF_PACKET_TIMEOUT, default="5s"): cv.positive_time_period_milliseconds,
            cv.Optional(CONF_VERIFY_TIMEOUT, default="60s"): cv.positive_time_period_milliseconds,
            cv.Optional(CONF_RETRIES, default=3): cv.int_range(min=0, max=10),
        }
    )
    .extend(uart.UART_DEVICE_SCHEMA)
    .extend(cv.COMPONENT_SCHEMA)
)


def _runtime_schema(value):
    # Built lazily so http_request is only imported when it's used
    schema = _BASE.extend(
        {
            cv.GenerateID(CONF_HTTP_REQUEST_ID): cv.use_id(_http_request_component()),
            cv.Optional(CONF_UPDATE_INTERVAL, default="6h"): cv.update_interval,
            cv.Optional(CONF_MAX_SIZE, default=65536): cv.All(
                cv.validate_bytes, cv.int_range(min=1024, max=4 * 1024 * 1024)
            ),
        }
    )
    value = schema(value)
    if not value[CONF_SOURCE].startswith(("http://", "https://")):
        raise cv.Invalid("fetch: runtime needs an http(s):// source", path=[CONF_SOURCE])
    return value


CONFIG_SCHEMA = cv.typed_schema(
    {
        FETCH_BUILD: _BASE.extend(
            {
                cv.GenerateID(CONF_RAW_DATA_ID): cv.declare_id(cg.uint8),
                # Fail the build if the firmware can't be fetched (default:
                # warn and build without firmware)
                cv.Optional(CONF_REQUIRED, default=False): cv.boolean,
            }
        ),
        FETCH_RUNTIME: _runtime_schema,
    },
    key=CONF_FETCH,
    default_type=FETCH_BUILD,
    lower=True,
)


def _read(src: str) -> tuple[bytes, str]:
    """Return (content, resolved location) for a URL or a file path."""
    if src.startswith(("http://", "https://")):
        req = urllib.request.Request(src, headers={"User-Agent": "esphome-tuya_mcu_dfu"})
        with urllib.request.urlopen(req, timeout=30) as r:  # follows redirects
            return r.read(), src
    p = Path(src)
    if not p.is_absolute():
        p = Path(CORE.relative_config_path(".")) / p
    return p.read_bytes(), str(p.resolve())


def _join(location: str, path: str) -> str:
    if path.startswith(("http://", "https://")) or Path(path).is_absolute():
        return path
    if location.startswith(("http://", "https://")):
        return urllib.parse.urljoin(location, path)
    return str(Path(location).parent / path)


def _pick_build(manifest: dict, chip_family: str | None, source: str) -> dict:
    builds = manifest.get("builds", [])
    if chip_family is None:
        if len(builds) != 1:
            raise cv.Invalid(f"{source} has {len(builds)} builds; set chip_family")
        return builds[0]
    for build in builds:
        if build.get("chipFamily") == chip_family:
            return build
    raise cv.Invalid(f"No build for chipFamily '{chip_family}' in {source}")


def fetch_firmware(source: str, chip_family: str | None) -> dict:
    raw, loc = _read(source)
    manifest = json.loads(raw)
    ota = _pick_build(manifest, chip_family, source)["ota"]
    url = _join(loc, ota["path"])
    md5 = ota["md5"].lower()
    cache = Path(CORE.relative_internal_path("tuya_mcu_dfu")) / (md5 + ".bin")
    data = cache.read_bytes() if cache.is_file() else _read(url)[0]
    if hashlib.md5(data).hexdigest() != md5:
        raise cv.Invalid(f"MD5 mismatch for {url}")
    cache.parent.mkdir(parents=True, exist_ok=True)
    cache.write_bytes(data)
    return {
        "data": data,
        "version": str(manifest["version"]),
        "title": manifest.get("name", ""),
        "md5": md5,
        "release_url": ota.get("release_url", ""),
        "summary": ota.get("summary", ""),
        "url": url,
    }


async def to_code(config):
    var = await update.new_update(config)
    # update_interval is ours (a plain interval), not a PollingComponent's
    await cg.register_component(var, {k: v for k, v in config.items() if k != CONF_UPDATE_INTERVAL})
    await uart.register_uart_device(var, config)
    parent = await cg.get_variable(config[CONF_TUYA_ID])
    cg.add(var.set_tuya_parent(parent))
    if CONF_VERSION_DATAPOINT in config:
        cg.add(var.set_version_datapoint(config[CONF_VERSION_DATAPOINT]))
    cg.add(var.set_timeouts(config[CONF_START_TIMEOUT], config[CONF_PACKET_TIMEOUT],
                            config[CONF_VERIFY_TIMEOUT], config[CONF_RETRIES]))

    if config[CONF_FETCH] == FETCH_RUNTIME:
        cg.add_define("USE_TUYA_MCU_DFU_RUNTIME")
        http = await cg.get_variable(config[CONF_HTTP_REQUEST_ID])
        cg.add(var.set_http_request(http))
        cg.add(var.set_source_url(config[CONF_SOURCE]))
        cg.add(var.set_chip_family(config.get(CONF_CHIP_FAMILY, "")))
        cg.add(var.set_max_size(config[CONF_MAX_SIZE]))
        cg.add(var.set_check_interval(config[CONF_UPDATE_INTERVAL]))
        return

    try:
        fw = fetch_firmware(config[CONF_SOURCE], config.get(CONF_CHIP_FAMILY))
    except Exception as err:  # noqa: BLE001
        if config[CONF_REQUIRED]:
            raise cv.Invalid(f"tuya_mcu_dfu: could not fetch MCU firmware: {err}") from err
        _LOGGER.warning("tuya_mcu_dfu: no MCU firmware bundled (%s): %s", config[CONF_SOURCE], err)
        return

    _LOGGER.info("tuya_mcu_dfu: bundling MCU firmware %s (%d bytes) from %s",
                 fw["version"], len(fw["data"]), fw["url"])
    arr = cg.progmem_array(config[CONF_RAW_DATA_ID], list(fw["data"]))
    cg.add(var.set_firmware(arr, len(fw["data"]), fw["version"], fw["title"], fw["md5"],
                            fw["release_url"], fw["summary"]))

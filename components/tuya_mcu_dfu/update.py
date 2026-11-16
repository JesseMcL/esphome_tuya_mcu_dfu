"""Tuya MCU firmware update entity.

The image comes from a release manifest (ESPHome style) in `source`, or, with
fetch: build, from an inline `firmware:` block (url or path, version, hash).
The manifest is fetched:

  fetch: build    (default) fetched when ESPHome compiles and embedded in the
                  binary. Works on every platform, including BK72xx
                  (LibreTiny), which has no `http_request`. Anything wrong with
                  the source (missing, unreachable, bad manifest, hash or size
                  mismatch) fails the build: a device without its image is
                  never built.
  fetch: runtime  fetched by the device with `http_request`: the manifest is
                  checked every `update_interval`, the image is downloaded
                  into RAM (up to `max_size`) and MD5-checked when you install.
                  The build checks the manifest and image if it can reach
                  them, and only warns: the release may be published later.

The update itself uses the standard Tuya MCU upgrade protocol (0x0A / 0x0B).
The image's MD5 is checked on the device before anything is sent to the MCU.
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
    CONF_FILE,
    CONF_ID,
    CONF_PATH,
    CONF_RAW_DATA_ID,
    CONF_SIZE,
    CONF_SOURCE,
    CONF_UPDATE_INTERVAL,
    CONF_URL,
    CONF_VERSION,
)
from esphome.core import CORE

from . import tuya_mcu_dfu_ns

_LOGGER = logging.getLogger(__name__)

DEPENDENCIES = ["tuya", "uart"]

CONF_VERSION_DATAPOINT = "version_datapoint"
CONF_CHIP_FAMILY = "chip_family"
CONF_PACKET_TIMEOUT = "packet_timeout"
CONF_START_TIMEOUT = "start_timeout"
CONF_VERIFY_TIMEOUT = "verify_timeout"
CONF_RETRIES = "retries"
CONF_FETCH = "fetch"
CONF_HTTP_REQUEST_ID = "http_request_id"
CONF_MAX_SIZE = "max_size"
CONF_FIRMWARE = "firmware"
CONF_MD5 = "md5"
CONF_SHA256 = "sha256"
CONF_TITLE = "title"
CONF_SUMMARY = "summary"
CONF_RELEASE_URL = "release_url"
CONF_CHANNEL = "channel"

FETCH_BUILD = "build"
FETCH_RUNTIME = "runtime"

TuyaMcuDfu = tuya_mcu_dfu_ns.class_(
    "TuyaMcuDfu", update.UpdateEntity, cg.Component, uart.UARTDevice
)


# The image's MD5 is checked on the device before every update
AUTO_LOAD = ["md5"]


def _source(value):
    value = cv.string_strict(value)
    if value.startswith(("http://", "https://")):
        return cv.url(value)
    return value


def _http_request_component():
    from esphome.components.http_request import HttpRequestComponent

    return HttpRequestComponent


def _hex(n):
    def validator(value):
        value = cv.string_strict(value).strip().lower()
        if len(value) != n or any(c not in "0123456789abcdef" for c in value):
            raise cv.Invalid(f"expected {n} hex digits")
        return value

    return validator


def _firmware(value):
    value = cv.Schema(
        {
            cv.Exclusive(CONF_URL, CONF_FILE): cv.url,
            # relative to the YAML
            cv.Exclusive(CONF_PATH, CONF_FILE): cv.string_strict,
            # what the MCU reports after the update
            cv.Required(CONF_VERSION): cv.string_strict,
            cv.Optional(CONF_MD5): _hex(32),
            cv.Optional(CONF_SHA256): _hex(64),
            cv.Optional(CONF_SIZE): cv.positive_not_null_int,
            cv.Optional(CONF_TITLE, default=""): cv.string,
            cv.Optional(CONF_SUMMARY, default=""): cv.string,
            cv.Optional(CONF_RELEASE_URL, default=""): cv.string,
        }
    )(value)
    if CONF_URL not in value and CONF_PATH not in value:
        raise cv.Invalid("firmware needs url or path")
    if CONF_URL in value and CONF_MD5 not in value and CONF_SHA256 not in value:
        raise cv.Invalid("firmware from a url needs md5 or sha256, to pin the image")
    return value


_BASE = (
    update.update_schema(TuyaMcuDfu)
    .extend(
        {
            cv.GenerateID(CONF_TUYA_ID): cv.use_id(Tuya),
            # Release manifest: URL, or (build only) path relative to the YAML
            cv.Optional(CONF_SOURCE): _source,
            # builds[].chipFamily to use; may be left out if there is one build
            cv.Optional(CONF_CHIP_FAMILY): cv.string,
            # Firmware channel (type) in the 0x0A start frame. Tuya modules send
            # the 8-byte form (size + channel) only for channels above 9; 0..9
            # (or not set) send the size alone.
            cv.Optional(CONF_CHANNEL): cv.uint8_t,
            # String DP in which the MCU reports its firmware version. Without
            # it, the "v" of the MCU's product information (0x01) is used.
            cv.Optional(CONF_VERSION_DATAPOINT): cv.uint8_t,
            cv.Optional(CONF_START_TIMEOUT, default="5s"): cv.positive_time_period_milliseconds,
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
    if CONF_SOURCE not in value:
        raise cv.Invalid("fetch: runtime needs a source (manifest URL)")
    if not value[CONF_SOURCE].startswith(("http://", "https://")):
        raise cv.Invalid("fetch: runtime needs an http(s):// source", path=[CONF_SOURCE])
    return value




class SourceError(Exception):
    """The firmware source can't be used (missing, unreachable, malformed or
    not matching its hash/size)."""


def _read(src: str) -> tuple[bytes, str]:
    """Return (content, resolved location) for a URL or a file path."""
    if src.startswith(("http://", "https://")):
        req = urllib.request.Request(src, headers={"User-Agent": "esphome-tuya_mcu_dfu"})
        try:
            with urllib.request.urlopen(req, timeout=30) as r:  # follows redirects
                return r.read(), src
        except Exception as err:  # noqa: BLE001  (HTTPError, URLError, timeout, ...)
            raise SourceError(f"can't fetch {src}: {err}") from err
    p = Path(src)
    if not p.is_absolute():
        p = Path(CORE.relative_config_path(".")) / p
    try:
        return p.read_bytes(), str(p.resolve())
    except OSError as err:
        raise SourceError(
            f"can't read {src} (looked for {p}): {err.strerror or err}. Relative paths are "
            f"resolved from the YAML's directory ({CORE.relative_config_path('.')}). If ESPHome "
            f"builds on another machine (for example a remote build server), local files next to "
            f"your YAML may not be copied there; use an http(s):// URL instead."
        ) from err


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
            raise SourceError(f"{source} has {len(builds)} builds; set chip_family")
        return builds[0]
    for build in builds:
        if build.get("chipFamily") == chip_family:
            return build
    raise SourceError(f"no build for chipFamily '{chip_family}' in {source}")


def _fetch_image(url: str, algo: str | None, digest: str | None) -> bytes:
    """Image from url / path. With a digest, the download is cached by it and
    must match it."""
    cache = None
    if digest is not None:
        cache = Path(CORE.relative_internal_path("tuya_mcu_dfu")) / f"{algo}-{digest}.bin"
        if cache.is_file() and hashlib.new(algo, cache.read_bytes()).hexdigest() == digest:
            return cache.read_bytes()
    data = _read(url)[0]
    if digest is not None:
        got = hashlib.new(algo, data).hexdigest()
        if got != digest:
            raise SourceError(f"{algo.upper()} mismatch for {url}: expected {digest}, got {got}")
        cache.parent.mkdir(parents=True, exist_ok=True)
        cache.write_bytes(data)
    return data


def _manifest(source: str, chip_family: str | None) -> tuple[dict, dict, str]:
    """(manifest, the build's ota, image url). Raises SourceError."""
    raw, loc = _read(source)
    try:
        manifest = json.loads(raw)
    except ValueError as err:
        raise SourceError(f"{source} is not valid JSON: {err}") from err
    if not isinstance(manifest, dict) or "version" not in manifest:
        raise SourceError(f"{source} has no version")
    ota = _pick_build(manifest, chip_family, source).get("ota")
    if not isinstance(ota, dict) or not ota.get("path") or not ota.get("md5"):
        raise SourceError(f"{source}: the build needs ota.path and ota.md5")
    md5 = str(ota["md5"]).strip().lower()
    if len(md5) != 32 or any(c not in "0123456789abcdef" for c in md5):
        raise SourceError(f"{source}: ota.md5 is not 32 hex digits")
    ota = {**ota, "md5": md5}
    return manifest, ota, _join(loc, ota["path"])


def fetch_firmware(source: str, chip_family: str | None) -> dict:
    manifest, ota, url = _manifest(source, chip_family)
    md5 = ota["md5"]
    data = _fetch_image(url, "md5", md5)
    if "size" in ota and len(data) != int(ota["size"]):
        raise SourceError(f"{url} is {len(data)} bytes, the manifest says {ota['size']}")
    return {
        "data": data,
        "version": str(manifest["version"]),
        "title": manifest.get("name", ""),
        "md5": md5,
        "release_url": ota.get("release_url", ""),
        "summary": ota.get("summary", ""),
        "url": url,
    }


def inline_firmware(fw: dict) -> dict:
    src = fw.get(CONF_URL) or fw[CONF_PATH]
    algo = next((a for a in ("sha256", "md5") if a in fw), None)
    data = _fetch_image(src, algo, fw.get(algo))  # local path: hash optional
    for other in ("sha256", "md5"):
        if other in fw and hashlib.new(other, data).hexdigest() != fw[other]:
            raise SourceError(f"{other.upper()} mismatch for {src}")
    if CONF_SIZE in fw and len(data) != fw[CONF_SIZE]:
        raise SourceError(f"{src} is {len(data)} bytes, expected {fw[CONF_SIZE]}")
    return {
        "data": data,
        "version": fw[CONF_VERSION],
        "title": fw[CONF_TITLE],
        "md5": hashlib.md5(data).hexdigest(),
        "release_url": fw[CONF_RELEASE_URL],
        "summary": fw[CONF_SUMMARY],
        "url": src,
    }


_KEY = "tuya_mcu_dfu_firmware"


def _fetch_for_build(config):
    """fetch: build - the image is bundled, so anything wrong with the source
    is a configuration error."""
    try:
        if CONF_FIRMWARE in config:
            fw = inline_firmware(config[CONF_FIRMWARE])
        else:
            fw = fetch_firmware(config[CONF_SOURCE], config.get(CONF_CHIP_FAMILY))
    except SourceError as err:
        path = [CONF_FIRMWARE] if CONF_FIRMWARE in config else [CONF_SOURCE]
        raise cv.Invalid(f"MCU firmware: {err}", path=path) from err
    CORE.data.setdefault(_KEY, {})[str(config[CONF_ID])] = fw
    return config


def _check_runtime_source(config):
    """fetch: runtime - the device fetches the release, which may not be
    published yet. Check it if it can be reached, but only warn."""
    source = config[CONF_SOURCE]
    try:
        manifest, ota, url = _manifest(source, config.get(CONF_CHIP_FAMILY))
        data = _read(url)[0]
        if hashlib.md5(data).hexdigest() != ota["md5"]:
            raise SourceError(f"MD5 mismatch for {url}: the device will reject this release")
        if "size" in ota and len(data) != int(ota["size"]):
            raise SourceError(f"{url} is {len(data)} bytes, the manifest says {ota['size']}")
        if len(data) > config[CONF_MAX_SIZE]:
            raise SourceError(f"{url} is {len(data)} bytes, more than max_size ({config[CONF_MAX_SIZE]})")
        _LOGGER.info("tuya_mcu_dfu: release %s at %s checks out (%d bytes)", manifest["version"], url,
                     len(data))
    except SourceError as err:
        _LOGGER.warning("tuya_mcu_dfu: %s (the device fetches %s at runtime; building anyway)", err, source)
    return config


CONFIG_SCHEMA = cv.typed_schema(
    {
        FETCH_BUILD: cv.All(
            _BASE.extend(
                {
                    cv.GenerateID(CONF_RAW_DATA_ID): cv.declare_id(cg.uint8),
                    # Image given directly instead of a manifest
                    cv.Optional(CONF_FIRMWARE): _firmware,
                }
            ),
            cv.has_exactly_one_key(CONF_SOURCE, CONF_FIRMWARE),
            _fetch_for_build,
        ),
        FETCH_RUNTIME: cv.All(_runtime_schema, _check_runtime_source),
    },
    key=CONF_FETCH,
    default_type=FETCH_BUILD,
    lower=True,
)


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
    if CONF_CHANNEL in config:
        cg.add(var.set_channel(config[CONF_CHANNEL]))

    if config[CONF_FETCH] == FETCH_RUNTIME:
        cg.add_define("USE_TUYA_MCU_DFU_RUNTIME")
        http = await cg.get_variable(config[CONF_HTTP_REQUEST_ID])
        cg.add(var.set_http_request(http))
        cg.add(var.set_source_url(config[CONF_SOURCE]))
        cg.add(var.set_chip_family(config.get(CONF_CHIP_FAMILY, "")))
        cg.add(var.set_max_size(config[CONF_MAX_SIZE]))
        cg.add(var.set_check_interval(config[CONF_UPDATE_INTERVAL]))
        return

    # fetched (and checked) during validation
    fw = CORE.data[_KEY][str(config[CONF_ID])]

    _LOGGER.info("tuya_mcu_dfu: bundling MCU firmware %s (%d bytes) from %s",
                 fw["version"], len(fw["data"]), fw["url"])
    arr = cg.progmem_array(config[CONF_RAW_DATA_ID], list(fw["data"]))
    cg.add(var.set_firmware(arr, len(fw["data"]), fw["version"], fw["title"], fw["md5"],
                            fw["release_url"], fw["summary"]))

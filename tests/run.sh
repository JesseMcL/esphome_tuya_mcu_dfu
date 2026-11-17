#!/usr/bin/env bash
# Host test: ESPHome (host platform) <-> pty <-> fake_mcu.py.
# The fake MCU starts at 1.0.0; the release is 1.1.0 (a 5000-byte image).
#   host.yaml                  bundled from a manifest, version DP; repeated
#                              for every packet size code (0..5), 16-bit
#                              offsets (0x0A reply frame version 0), an MCU
#                              that ignores the first 0x0A (resets into its
#                              bootloader, which answers the retry) and a
#                              packet lost to a crossing heartbeat
#   host_reinit.yaml           update after tuya: initialised; the restarted
#                              MCU waits for the Wi-Fi status (tuya: stall),
#                              also with its "first heartbeat" reply going to
#                              the component instead of tuya:
#   host_no_version_dp.yaml    verified through product information only
#   host_boot_version.yaml     MCU already at 1.1.0: version read at boot
#                              (0x01), no update offered; also with a
#                              datapoint report arriving during that query
#   host_channel.yaml          channel 10: 8-byte 0x0A start frame
#   host_inline_url.yaml       inline firmware: url + sha256 + size
#   host_inline_path.yaml      inline firmware: local path, no hash
#   host_no_id.yaml            no id: (auto-generated ID); code generation only
#   host_runtime.yaml          http_request download         -> updated
#   host_runtime_bad_md5.yaml  download with a wrong MD5     -> rejected
# Build-time source problems (esphome config), fetch: build -> must fail:
#   host_inline_bad_hash.yaml  wrong sha256
#   host_missing_manifest.yaml local manifest doesn't exist
#   host_unreachable_url.yaml  manifest URL 404
#   host_manifest_bad_md5.yaml manifest MD5 doesn't match the image
#   host_no_chip_family.yaml   no build for chip_family
# fetch: runtime -> only a warning:
#   host_runtime_unpublished.yaml  release not published yet
# Needs: esphome, socat, python3 (and libssl for the host build's MD5).
set -euo pipefail
cd "$(dirname "$0")"
LOGS=$(mktemp -d)
trap 'kill $(jobs -p) 2>/dev/null || true; [ -n "${KEEP:-}" ] || rm -rf "$LOGS"' EXIT

mkdir -p build/bad
head -c 5000 /dev/urandom > build/firmware.bin
printf '"%s"\n' "$(sha256sum build/firmware.bin | cut -d' ' -f1)" > build/firmware.sha256
python3 ../tools/make_manifest.py build/firmware.bin --version 1.1.0 --chip-family TEST \
  -o build/manifest.json
cp build/firmware.bin build/bad/firmware.bin
python3 - <<'PY'
import json
m = json.load(open("build/manifest.json"))
m["builds"][0]["ota"]["md5"] = "0" * 32
json.dump(m, open("build/bad/manifest.json", "w"))
PY
python3 -m http.server 8765 --bind 127.0.0.1 --directory build > "$LOGS/http.log" 2>&1 &
sleep 0.5

# run NAME CONFIG EXPECT [fake_mcu args...]
#   EXPECT: update | noupdate | reject (MD5, nothing sent)
#           | build-fail=REGEX (esphome config must fail with REGEX)
#           | config-warn=REGEX (esphome config must pass, warning REGEX)
#           | generate=REGEX (code generation must pass, logging REGEX)
#   update/noupdate may add +REGEX: the device log must also match REGEX
#   (dots stand in for spaces).
run() {
  local name=$1 cfg=$2 expect=$3 prog extra=
  shift 3
  echo "=== $name ($cfg $*)"
  case $expect in
    build-fail=*|config-warn=*)
      local re=${expect#*=}
      if esphome config "$cfg" > "$LOGS/$name.build.log" 2>&1; then
        [ "${expect%%=*}" = config-warn ] || { echo "config should have failed"; tail -5 "$LOGS/$name.build.log"; return 1; }
      else
        [ "${expect%%=*}" = build-fail ] || { tail -30 "$LOGS/$name.build.log"; return 1; }
      fi
      grep -oE "$re[^']{0,120}" "$LOGS/$name.build.log" | head -1
      grep -qE "$re" "$LOGS/$name.build.log"
      return
      ;;
    generate=*)
      esphome compile --only-generate "$cfg" > "$LOGS/$name.build.log" 2>&1 \
        || { tail -30 "$LOGS/$name.build.log"; return 1; }
      grep -qE "${expect#*=}" "$LOGS/$name.build.log"
      return
      ;;
  esac
  if [[ $expect == *+* ]]; then extra=${expect#*+}; expect=${expect%%+*}; fi
  if ! esphome compile "$cfg" > "$LOGS/$name.build.log" 2>&1; then
    tail -30 "$LOGS/$name.build.log"
    return 1
  fi
  socat pty,raw,echo=0,link=/tmp/tuya-mcu-dfu-esp pty,raw,echo=0,link=/tmp/tuya-mcu-dfu-mcu &
  local socat_pid=$!
  sleep 1
  local mcu_version=1.0.0
  [ "$expect" = noupdate ] && mcu_version=1.1.0
  python3 fake_mcu.py /tmp/tuya-mcu-dfu-mcu --version $mcu_version --new-version 1.1.0 \
    --expect build/firmware.bin "$@" > "$LOGS/$name.mcu.log" 2>&1 &
  local mcu_pid=$!
  sleep 0.5  # tuya:'s first heartbeat must find the fake MCU listening
  local node
  node=$(sed -n 's/^  name: *//p' "$cfg" | head -1)
  prog=.esphome/build/$node/.pioenvs/$node/program
  "$prog" > "$LOGS/$name.esp.log" 2>&1 &
  local esp_pid=$! i
  # up to 40 s; stop once the outcome (and anything extra expected) is logged
  for i in $(seq 160); do
    sleep 0.25
    if grep -qE "verified|MCU update failed|image rejected|TEST: no update available" "$LOGS/$name.esp.log"; then
      [ -z "$extra" ] || grep -qE "$extra" "$LOGS/$name.esp.log" || continue
      sleep 0.5
      break
    fi
  done
  kill $esp_pid 2>/dev/null || true
  wait $esp_pid 2>/dev/null || true
  kill $mcu_pid $socat_pid 2>/dev/null || true
  wait $mcu_pid $socat_pid 2>/dev/null || true
  sed 's/\x1b\[[0-9;]*m//g' "$LOGS/$name.esp.log" | grep -E "tuya_mcu_dfu|TEST|Initialization" | grep -v "\[C\]" || true
  cat "$LOGS/$name.mcu.log"
  if [ -n "$extra" ] && ! grep -qE "$extra" "$LOGS/$name.esp.log"; then
    echo "expected in the device log: $extra"
    return 1
  fi
  if [[ " $* " == *" --real-reinit "* ]] && ! sed -n '/restarted with version/,$p' "$LOGS/$name.mcu.log" | grep -q "status query"; then
    echo "tuya: didn't get the datapoints of the restarted MCU"
    return 1
  fi
  case $expect in
    update)
      grep -q "MCU update to 1.1.0 verified" "$LOGS/$name.esp.log" && grep -q "image OK" "$LOGS/$name.mcu.log" \
        && ! grep -q "protocol error" "$LOGS/$name.mcu.log" ;;
    noupdate)
      grep -q "TEST: no update available" "$LOGS/$name.esp.log" && ! grep -q "update start" "$LOGS/$name.mcu.log" ;;
    *)
      grep -q "MD5 mismatch" "$LOGS/$name.esp.log" && ! grep -q "update start" "$LOGS/$name.mcu.log" ;;
  esac
}

TESTS=(
  "manifest          host.yaml                  update"
  "code1-512         host.yaml                  update --pkt-code 1"
  "code2-1024        host.yaml                  update --pkt-code 2"
  "code3-2048        host.yaml                  update --pkt-code 3"
  "code4-3072        host.yaml                  update --pkt-code 4"
  "code5-4096        host.yaml                  update --pkt-code 5"
  "offset16          host.yaml                  update --ota-version 0"
  "start-retry       host.yaml                  update+MCU.restarted,.sending.start.again --ignore-first-start"
  "heartbeat-cross   host.yaml                  update+No.ack.after.a.heartbeat --drop-packet 3"
  "reinit-stall      host_reinit.yaml           update+waiting.at.the.Wi-Fi.status.step --real-reinit"
  "restart-unseen    host_reinit.yaml           update+restarted.while.tuya --real-reinit --slow-restart"
  "product-info      host_no_version_dp.yaml    update"
  "boot-version      host_boot_version.yaml     noupdate+MCU.firmware.version:.1.1.0"
  "query-dp-report   host_boot_version.yaml     noupdate+requesting.a.status.update --report-during-query"
  "channel10         host_channel.yaml          update --expect-channel 10"
  "inline-url        host_inline_url.yaml       update"
  "inline-path       host_inline_path.yaml      update"
  "inline-bad-hash   host_inline_bad_hash.yaml  build-fail=SHA256.mismatch"
  "missing-manifest  host_missing_manifest.yaml build-fail=can't.read"
  "unreachable-url   host_unreachable_url.yaml  build-fail=can't.fetch"
  "manifest-bad-md5  host_manifest_bad_md5.yaml build-fail=MD5.mismatch"
  "no-chip-family    host_no_chip_family.yaml   build-fail=no.build.for.chipFamily"
  "runtime-unpub     host_runtime_unpublished.yaml config-warn=building.anyway"
  "no-id             host_no_id.yaml            generate=bundling.MCU.firmware"
  "runtime           host_runtime.yaml          update"
  "runtime-bad-md5   host_runtime_bad_md5.yaml  reject"
)
fail=0
results=()
for t in "${TESTS[@]}"; do
  set -- $t
  name=$1
  if [ -n "${ONLY:-}" ] && [[ ! $name =~ $ONLY ]]; then continue; fi
  if run "$@"; then results+=("PASS $name"); else results+=("FAIL $name"); fail=1; fi
done
printf '%s\n' "${results[@]}"
exit $fail

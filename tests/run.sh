#!/usr/bin/env bash
# Host test: ESPHome (host platform) <-> pty <-> fake_mcu.py.
# The fake MCU starts at 1.0.0; the release is 1.1.0 (a 5000-byte image).
#   host.yaml                  bundled from a manifest, version DP; repeated
#                              for every packet size code (0..5), 16-bit
#                              offsets (0x0A reply frame version 0) and an MCU
#                              that ignores the first 0x0A (resets into its
#                              bootloader, which answers the retry)
#   host_no_version_dp.yaml    verified through product information only
#   host_channel.yaml          channel 10: 8-byte 0x0A start frame
#   host_inline_url.yaml       inline firmware: url + sha256 + size
#   host_inline_path.yaml      inline firmware: local path, no hash
#   host_inline_bad_hash.yaml  wrong sha256                  -> build fails
#   host_runtime.yaml          http_request download         -> updated
#   host_runtime_bad_md5.yaml  download with a wrong MD5     -> rejected
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
#   EXPECT: update | reject (MD5, nothing sent) | build-fail
run() {
  local name=$1 cfg=$2 expect=$3 prog
  shift 3
  echo "=== $name ($cfg $*)"
  if ! esphome compile "$cfg" > "$LOGS/$name.build.log" 2>&1; then
    if [ "$expect" = build-fail ]; then
      grep -o "SHA256 mismatch[^']*" "$LOGS/$name.build.log" | head -1
      grep -q "SHA256 mismatch" "$LOGS/$name.build.log"
      return
    fi
    tail -30 "$LOGS/$name.build.log"
    return 1
  fi
  [ "$expect" != build-fail ] || { echo "build should have failed"; return 1; }
  socat pty,raw,echo=0,link=/tmp/tuya-mcu-dfu-esp pty,raw,echo=0,link=/tmp/tuya-mcu-dfu-mcu &
  local socat_pid=$!
  sleep 1
  python3 fake_mcu.py /tmp/tuya-mcu-dfu-mcu --version 1.0.0 --new-version 1.1.0 \
    --expect build/firmware.bin "$@" > "$LOGS/$name.mcu.log" 2>&1 &
  local mcu_pid=$!
  local node
  node=$(sed -n 's/^  name: *//p' "$cfg" | head -1)
  prog=.esphome/build/$node/.pioenvs/$node/program
  "$prog" > "$LOGS/$name.esp.log" 2>&1 &
  local esp_pid=$! i
  for i in $(seq 100); do  # up to 25 s; stop once the outcome is logged
    sleep 0.25
    grep -qE "verified|MCU update failed|image rejected" "$LOGS/$name.esp.log" && { sleep 0.5; break; }
  done
  kill $esp_pid 2>/dev/null || true
  wait $esp_pid 2>/dev/null || true
  kill $mcu_pid $socat_pid 2>/dev/null || true
  wait $mcu_pid $socat_pid 2>/dev/null || true
  sed 's/\x1b\[[0-9;]*m//g' "$LOGS/$name.esp.log" | grep -E "tuya_mcu_dfu|TEST" | grep -v "\[C\]" || true
  cat "$LOGS/$name.mcu.log"
  if [ "$expect" = update ]; then
    grep -q "MCU update to 1.1.0 verified" "$LOGS/$name.esp.log" && grep -q "image OK" "$LOGS/$name.mcu.log"
  else
    grep -q "MD5 mismatch" "$LOGS/$name.esp.log" && ! grep -q "update start" "$LOGS/$name.mcu.log"
  fi
}

TESTS=(
  "manifest          host.yaml                  update"
  "code1-512         host.yaml                  update --pkt-code 1"
  "code2-1024        host.yaml                  update --pkt-code 2"
  "code3-2048        host.yaml                  update --pkt-code 3"
  "code4-3072        host.yaml                  update --pkt-code 4"
  "code5-4096        host.yaml                  update --pkt-code 5"
  "offset16          host.yaml                  update --ota-version 0"
  "start-retry       host.yaml                  update --ignore-first-start"
  "product-info      host_no_version_dp.yaml    update"
  "channel10         host_channel.yaml          update --expect-channel 10"
  "inline-url        host_inline_url.yaml       update"
  "inline-path       host_inline_path.yaml      update"
  "inline-bad-hash   host_inline_bad_hash.yaml  build-fail"
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

#!/usr/bin/env bash
# Host test: ESPHome (host platform) <-> pty <-> fake_mcu.py.
# The fake MCU starts at 1.0.0; the release is 1.1.0.
#   host.yaml                  bundled image, version DP       -> updated
#   host_no_version_dp.yaml    bundled image, product info     -> updated
#   host_runtime.yaml          http_request download           -> updated
#   host_runtime_bad_md5.yaml  download with a wrong MD5       -> rejected
# Needs: esphome, socat, python3 (and libssl for the host build's MD5).
set -euo pipefail
cd "$(dirname "$0")"
LOGS=$(mktemp -d)
trap 'kill $(jobs -p) 2>/dev/null || true; [ -n "${KEEP:-}" ] || rm -rf "$LOGS"' EXIT

mkdir -p build/bad
head -c 5000 /dev/urandom > build/firmware.bin
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

run() {  # config, expect_update(1/0)
  local cfg=$1 expect=$2 name prog
  name=$(basename "$cfg" .yaml)
  echo "=== $cfg"
  esphome compile "$cfg" > "$LOGS/$name.build.log" 2>&1 || { tail -30 "$LOGS/$name.build.log"; return 1; }
  socat pty,raw,echo=0,link=/tmp/tuya-mcu-dfu-esp pty,raw,echo=0,link=/tmp/tuya-mcu-dfu-mcu &
  local socat_pid=$!
  sleep 1
  python3 fake_mcu.py /tmp/tuya-mcu-dfu-mcu --version 1.0.0 --new-version 1.1.0 \
    --expect build/firmware.bin > "$LOGS/$name.mcu.log" 2>&1 &
  local mcu_pid=$!
  prog=$(ls -t .esphome/build/*/.pioenvs/*/program | head -1)
  timeout 20 "$prog" > "$LOGS/$name.esp.log" 2>&1 || true
  kill $mcu_pid $socat_pid 2>/dev/null || true
  wait $mcu_pid $socat_pid 2>/dev/null || true
  sed 's/\x1b\[[0-9;]*m//g' "$LOGS/$name.esp.log" | grep -E "tuya_mcu_dfu|TEST" | grep -v "\[C\]" || true
  cat "$LOGS/$name.mcu.log"
  if [ "$expect" = 1 ]; then
    grep -q "MCU update to 1.1.0 verified" "$LOGS/$name.esp.log" && grep -q "image OK" "$LOGS/$name.mcu.log"
  else
    grep -q "MD5 mismatch" "$LOGS/$name.esp.log" && ! grep -q "update start" "$LOGS/$name.mcu.log"
  fi
}

fail=0
for t in "host.yaml 1" "host_no_version_dp.yaml 1" "host_runtime.yaml 1" "host_runtime_bad_md5.yaml 0"; do
  set -- $t
  if run "$1" "$2"; then echo "PASS $1"; else echo "FAIL $1"; fail=1; fi
done
exit $fail

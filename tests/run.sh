#!/usr/bin/env bash
# Host test: ESPHome (host platform) <-> pty <-> fake_mcu.py.
# For each config, the fake MCU starts at 1.0.0 and must end up running the
# bundled 1.1.0 image, and ESPHome must log "MCU update to 1.1.0 verified".
# Needs: esphome, socat, python3.
set -euo pipefail
cd "$(dirname "$0")"
LOGS=$(mktemp -d)
trap 'kill $(jobs -p) 2>/dev/null || true; [ -n "${KEEP:-}" ] || rm -rf "$LOGS"' EXIT

mkdir -p build
head -c 5000 /dev/urandom > build/firmware.bin
python3 ../tools/make_manifest.py build/firmware.bin --version 1.1.0 --chip-family TEST \
  -o build/manifest.json

fail=0
for cfg in host.yaml host_no_version_dp.yaml; do
  name=$(basename "$cfg" .yaml)
  echo "=== $cfg"
  esphome compile "$cfg" > "$LOGS/$name.build.log" 2>&1 || { tail -30 "$LOGS/$name.build.log"; exit 1; }
  socat pty,raw,echo=0,link=/tmp/tuya-mcu-dfu-esp pty,raw,echo=0,link=/tmp/tuya-mcu-dfu-mcu &
  SOCAT=$!
  sleep 1
  python3 fake_mcu.py /tmp/tuya-mcu-dfu-mcu --version 1.0.0 --new-version 1.1.0 \
    --expect build/firmware.bin > "$LOGS/$name.mcu.log" 2>&1 &
  MCU=$!
  prog=$(find .esphome/build -path "*/.pioenvs/*/program" -newer "$cfg" | xargs ls -t | head -1)
  timeout 20 "$prog" > "$LOGS/$name.esp.log" 2>&1 || true
  kill $MCU $SOCAT 2>/dev/null || true
  wait $MCU $SOCAT 2>/dev/null || true
  sed 's/\x1b\[[0-9;]*m//g' "$LOGS/$name.esp.log" | grep -E "tuya_mcu_dfu|TEST" | grep -v "\[C\]" || true
  cat "$LOGS/$name.mcu.log"
  if grep -q "MCU update to 1.1.0 verified" "$LOGS/$name.esp.log" && grep -q "image OK" "$LOGS/$name.mcu.log"; then
    echo "PASS $cfg"
  else
    echo "FAIL $cfg"; fail=1
  fi
done
exit $fail

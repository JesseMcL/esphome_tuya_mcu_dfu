#!/usr/bin/env python3
"""A minimal TuyaMCU-protocol MCU on a pty, for the host test (run.sh).

Answers ESPHome's `tuya:` init, reports its firmware version in a string
datapoint, and accepts a firmware update (0x0A / 0x0B). After the update it
"restarts" with the new version if the received image matches --expect.

  fake_mcu.py TTY --version 1.0.0 --new-version 1.1.0 --expect firmware.bin
      [--pkt-code 0..5] [--ota-version V] [--ignore-first-start]
      [--expect-channel C]

--pkt-code      packet size code in the 0x0A reply (256 .. 4096 bytes)
--ota-version   frame version byte of the 0x0A reply; 0 = 16-bit offsets
--ignore-first-start
                don't answer the first 0x0A (like an app that just resets
                into its bootloader, which answers the retry)
--expect-channel
                the 0x0A must be the 8-byte form with this channel
"""
import argparse
import os
import select
import sys
import termios
import tty

HEARTBEAT, PRODUCT, CONF, WIFI_STATE, DP_DELIVER, DP_REPORT, DP_QUERY = 0x00, 0x01, 0x02, 0x03, 0x06, 0x07, 0x08
OTA_START, OTA_DATA, NET_STATUS = 0x0A, 0x0B, 0x2B


def log(*a):
    print("FAKE", *a, flush=True)


PACKET_SIZES = [256, 512, 1024, 2048, 3072, 4096]


class Mcu:
    def __init__(self, fd, version, dp, expect, a):
        self.fd, self.version, self.dp, self.expect = fd, version, dp, expect
        self.a = a
        self.first_hb = True
        self.image = None
        self.size = 0
        self.width = 4
        self.starts = 0
        self.errors = []

    def send(self, cmd, data=b"", ver=0x03):
        f = bytes([0x55, 0xAA, ver, cmd, len(data) >> 8, len(data) & 0xFF]) + data
        os.write(self.fd, f + bytes([sum(f) & 0xFF]))

    def report_version(self):
        v = self.version.encode()
        self.send(DP_REPORT, bytes([self.dp, 0x03, len(v) >> 8, len(v) & 0xFF]) + v)

    def frame(self, cmd, data):
        if cmd == HEARTBEAT:
            self.send(HEARTBEAT, b"\x00" if self.first_hb else b"\x01")
            self.first_hb = False
        elif cmd == PRODUCT:
            self.send(PRODUCT, b'{"p":"fake","v":"%s","m":0}' % self.version.encode())
        elif cmd == CONF:
            self.send(CONF)
            self.send(WIFI_STATE)       # see the ESPHome re-init note in README
            self.send(NET_STATUS)
        elif cmd == WIFI_STATE:
            self.send(WIFI_STATE)
        elif cmd in (DP_QUERY, DP_DELIVER):
            self.report_version()
        elif cmd == OTA_START:
            self.starts += 1
            if self.a.ignore_first_start and self.starts == 1:
                log("start ignored (resetting into the bootloader)")
                return
            want = 4 if self.a.expect_channel is None else 8
            if len(data) != want or (want == 8 and (data[4] != self.a.expect_channel or data[5:8] != b"\0\0\0")):
                self.errors.append("start frame %s, expected %d bytes" % (data.hex(), want))
            self.size = int.from_bytes(data[:4], "big")
            self.image = bytearray()
            self.width = 2 if self.a.ota_version == 0 else 4
            log("update start, %d bytes, start frame %d bytes" % (self.size, len(data)))
            self.send(OTA_START, bytes([self.a.pkt_code]), ver=self.a.ota_version)
        elif cmd == OTA_DATA and self.image is not None:
            w = self.width
            off = int.from_bytes(data[:w], "big")
            if len(data) == w:
                if off != self.size:
                    self.errors.append("end offset %d, size %d" % (off, self.size))
                self.finish()
                return
            if len(data) - w > PACKET_SIZES[self.a.pkt_code]:
                self.errors.append("packet of %d bytes" % (len(data) - w))
            if off == len(self.image):
                self.image += data[w:]
            else:
                self.errors.append("offset %d, expected %d" % (off, len(self.image)))
            self.send(OTA_DATA, ver=self.a.ota_version)

    def finish(self):
        ok = bytes(self.image) == self.expect and not self.errors
        for e in self.errors:
            log("protocol error:", e)
        log("update end, %d bytes, image %s" % (len(self.image), "OK" if ok else "MISMATCH"))
        self.image = None
        if ok:
            self.version = self.new_version
            self.first_hb = True                  # "restarted"
            log("restarted with version", self.version)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("tty")
    ap.add_argument("--version", default="1.0.0")
    ap.add_argument("--new-version", required=True)
    ap.add_argument("--dp", type=int, default=120)
    ap.add_argument("--expect", required=True)
    ap.add_argument("--pkt-code", type=int, default=0, choices=range(6))
    ap.add_argument("--ota-version", type=int, default=3)
    ap.add_argument("--ignore-first-start", action="store_true")
    ap.add_argument("--expect-channel", type=int)
    a = ap.parse_args()

    fd = os.open(a.tty, os.O_RDWR | os.O_NOCTTY)
    tty.setraw(fd)
    attrs = termios.tcgetattr(fd)
    attrs[3] &= ~termios.ECHO
    termios.tcsetattr(fd, termios.TCSANOW, attrs)
    mcu = Mcu(fd, a.version, a.dp, open(a.expect, "rb").read(), a)
    mcu.new_version = a.new_version
    buf = bytearray()
    while True:
        select.select([fd], [], [])
        buf += os.read(fd, 4096)
        while True:
            i = buf.find(b"\x55\xaa")
            if i < 0:
                buf.clear()
                break
            del buf[:i]
            if len(buf) < 7:
                break
            n = (buf[4] << 8) | buf[5]
            if len(buf) < 7 + n:
                break
            f = bytes(buf[:7 + n])
            del buf[:7 + n]
            if sum(f[:-1]) & 0xFF == f[-1]:
                mcu.frame(f[3], f[6:6 + n])


if __name__ == "__main__":
    sys.exit(main())

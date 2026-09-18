#!/bin/sh
# SPDX-License-Identifier: MIT
# mgmtd smoke test: run against a mock-mode instance on localhost.
set -e
PORT=${1:-6800}
B="http://localhost:$PORT"
fail() { echo "FAIL: $1"; exit 1; }

curl -sf "$B/" | grep -q vmpu68 || fail "web ui"
curl -sf "$B/api/status" | grep -q '"sig_ok":true' || fail "status/signature"
curl -sf "$B/api/status" | grep -q '"mock":true' || fail "expected mock mode"

curl -sf -X POST "$B/api/bus/write?addr=0x001000&word=1&data=0xBEEF" | grep -q '"fault":false' || fail "bus write"
curl -sf "$B/api/bus/read?addr=0x001000&word=1" | grep -q '"value":48879' || fail "bus read back"
curl -sf "$B/api/bus/read?addr=0x400000&word=1" | grep -q '"fault":true' || fail "bus fault detect"

curl -sf -X POST "$B/api/mock/dma?addr=0x2000&data=0xD000&n=3" > /dev/null || fail "mock dma"
curl -sf "$B/api/snoop?max=10" | grep -q '"count":3' || fail "snoop drain"
curl -sf "$B/api/bus/read?addr=0x2002&word=1" | grep -q '"value":53249' || fail "dma landed in ram"

curl -sf "$B/api/fpga/flashid" | grep -q '"id":\[239,64,22\]' || fail "flash id"
head -c 135100 /dev/urandom > /tmp/smoke-bit.bin
curl -sf -X POST --data-binary @/tmp/smoke-bit.bin "$B/api/fpga/flash" | grep -q '"ok":true' || fail "fpga flash"
head -c 100000 /dev/urandom > /tmp/smoke-fw.bin
curl -sf -X POST --data-binary @/tmp/smoke-fw.bin "$B/api/ota" | grep -q '"ok":true' || fail "ota stage"

curl -sf -X POST "$B/api/led?v=5" | grep -q '"ok":true' || fail "led"
curl -sf -X POST "$B/api/drv?reset=1&halt=0" | grep -q '"ok":true' || fail "drv"
curl -sf "$B/api/log" | grep -q "mgmtd start" || fail "log"

# emulator: load vectors + program via API, run, verify from the bus side
python3 -c "
import sys
v = bytes([0,0,0x20,0,0,0,0x04,0])
sys.stdout.buffer.write(v)" > /tmp/smoke-vec.bin
python3 -c "
import sys
p = bytes([0x70,0,0x41,0xF8,0x10,0,0x30,0xC0,0x52,0x40,0x0C,0x40,0,0x10,0x66,0xF6,0x60,0xFE])
sys.stdout.buffer.write(p)" > /tmp/smoke-prog.bin
curl -sf -X POST --data-binary @/tmp/smoke-vec.bin  "$B/api/emu/load?addr=0" > /dev/null || fail "emu load vec"
curl -sf -X POST --data-binary @/tmp/smoke-prog.bin "$B/api/emu/load?addr=0x400" > /dev/null || fail "emu load prog"
curl -sf -X POST "$B/api/emu/reset" > /dev/null || fail "emu reset"
curl -sf -X POST "$B/api/emu/start" > /dev/null || fail "emu start"
sleep 1
curl -sf "$B/api/emu/state" | grep -q '"running":true' || fail "emu running"
curl -sf "$B/api/emu/state" | grep -q '"pc":1040' || fail "emu pc at spin (0x410)"
curl -sf -X POST "$B/api/emu/stop" > /dev/null || fail "emu stop"
curl -sf "$B/api/bus/read?addr=0x101E&word=1" | grep -q '"value":15' || fail "emu output in ram"

echo "SMOKE ALL PASS"

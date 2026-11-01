#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""
Runs the thermostat firmware in the mcuemu emulator against a scripted Tuya MCU.

The script plays the thermostat's MCU on the module's UART (9600 baud, the
control API of `mcuemu serve`): it answers the product query with the product
id of a model, reports datapoints when asked, and checks that the firmware
picks the model, acknowledges the reports and applies the datapoints (the
firmware prints what it does to the RTT channel of a debug build).

    run_thermostat.py --mcuemu PATH --image THERMOSTAT-full.bin --elf zephyr.elf \
        --coordinator samplegw_tb03f.bin --models tt_models.c [--model N ...]

What it cannot see: the Zigbee side (the network is encrypted) and writes from
the coordinator; both are covered only by the host tests of lib/tuya_thermostat.
"""
import argparse
import base64
import json
import os
import re
import shutil
import socket
import struct
import subprocess
import sys
import tempfile

YAML = """\
version: 1
world:
  seed: 1
  radio:
    - {{ id: air0, model: log_distance, reference_loss_db: 40, path_loss_exponent: 2.5 }}
devices:
  - id: coordinator
    mcus:
      - id: zb
        model: tlsr8258
        variant: F1K
        firmware: coordinator.bin
        radio: {{ air: air0, position: [0, 0, 1], tx_power_dbm: 10, protocols: [zigbee] }}
  - id: thermostat
    mcus:
      - id: zb
        model: tlsr8258
        variant: F512
        firmware: thermostat.bin
        start_at: 5s
        rtt: {{ address: {rtt} }}
        radio: {{ air: air0, position: [3, 0, 1], tx_power_dbm: 10, protocols: [zigbee] }}
"""


class WebSocket:
    def __init__(self, port):
        self.s = socket.create_connection(("127.0.0.1", port))
        key = base64.b64encode(os.urandom(16)).decode()
        self.s.sendall(
            ("GET / HTTP/1.1\r\nHost: 127.0.0.1\r\nUpgrade: websocket\r\n"
             "Connection: Upgrade\r\nSec-WebSocket-Key: %s\r\n"
             "Sec-WebSocket-Version: 13\r\n\r\n" % key).encode())
        buf = b""
        while b"\r\n\r\n" not in buf:
            buf += self.s.recv(1)
        if b" 101 " not in buf.split(b"\r\n")[0]:
            raise RuntimeError("websocket handshake failed: %r" % buf)
        self.rest = b""

    def _read(self, n):
        while len(self.rest) < n:
            chunk = self.s.recv(65536)
            if not chunk:
                raise EOFError
            self.rest += chunk
        out, self.rest = self.rest[:n], self.rest[n:]
        return out

    def send(self, text):
        data = text.encode()
        mask = os.urandom(4)
        n = len(data)
        head = bytes([0x81])
        if n < 126:
            head += bytes([0x80 | n])
        elif n < 65536:
            head += bytes([0x80 | 126]) + struct.pack(">H", n)
        else:
            head += bytes([0x80 | 127]) + struct.pack(">Q", n)
        self.s.sendall(head + mask + bytes(b ^ mask[i % 4] for i, b in enumerate(data)))

    def recv(self):
        payload = b""
        while True:
            b0, b1 = self._read(2)
            n = b1 & 0x7F
            if n == 126:
                n = struct.unpack(">H", self._read(2))[0]
            elif n == 127:
                n = struct.unpack(">Q", self._read(8))[0]
            payload += self._read(n)
            if b0 & 0x80:
                return json.loads(payload)


class Emu:
    def __init__(self, ws):
        self.ws = ws
        self.next_id = 1
        self.events = []

    def call(self, method, **params):
        rid = self.next_id
        self.next_id += 1
        self.ws.send(json.dumps({"jsonrpc": "2.0", "id": rid, "method": method, "params": params}))
        while True:
            m = self.ws.recv()
            if m.get("method") == "event":
                self.events.append(m["params"])
            elif m.get("id") == rid:
                if "error" in m:
                    raise RuntimeError("%s: %s" % (method, m["error"]))
                return m["result"]

    def take_events(self):
        ev, self.events = self.events, []
        return ev


def frame(cmd, data=b"", seq=1, version=0):
    f = bytes([0x55, 0xAA, version]) + struct.pack(">H", seq) + bytes([cmd]) + \
        struct.pack(">H", len(data)) + data
    return f + bytes([sum(f) & 0xFF])


class FrameParser:
    """Frames of the link: 55 AA ver seq(2) cmd len(2) data sum; yields (cmd, frame)."""

    def __init__(self):
        self.buf = bytearray()

    def feed(self, data):
        self.buf += data
        out = []
        while True:
            i = self.buf.find(b"\x55\xaa")
            if i < 0:
                # keep a lone 0x55: the 0xAA may come with the next byte
                del self.buf[:max(0, len(self.buf) - (1 if self.buf[-1:] == b"\x55" else 0))]
                return out
            del self.buf[:i]
            if len(self.buf) < 8:
                return out
            total = 8 + struct.unpack(">H", bytes(self.buf[6:8]))[0] + 1
            if len(self.buf) < total:
                return out
            f = bytes(self.buf[:total])
            del self.buf[:total]
            if sum(f[:-1]) & 0xFF == f[-1]:
                out.append((f[5], f))


def parse_models(path):
    text = open(path).read()
    models = []
    for m in re.finditer(r'\[(\d+)\] = \{\.name = "([^"]+)", \.products = \(const char \*const\[\]\)\{([^}]*)\}'
                         r'.*?\.dp = (dp_model_\w+)\}', text):
        prods = re.findall(r'"([^"]+)"', m.group(3))
        models.append({"index": int(m.group(1)), "name": m.group(2), "products": prods,
                       "dp": m.group(4)})
    for md in models:
        body = re.search(r"%s\[IOTDRV_TT_FIELD_COUNT\] = \{(.*?)\n\};" % md["dp"], text, re.S).group(1)
        md["dps"] = [(f, int(i, 16), int(t), int(l)) for f, i, t, l in
                     re.findall(r"\[IOTDRV_TT_F_(\w+)\] = DPT\((0x[0-9a-f]+), (\d+), (\d+),", body)]
    return models


def dp_bytes(dp_id, dp_type, length):
    if dp_type == 1:    # bool
        data = b"\x01"
    elif dp_type == 4:  # enum
        data = b"\x00"
    elif dp_type == 2:  # value
        data = struct.pack(">I", 2300)
    else:
        return None
    return bytes([dp_id, dp_type]) + struct.pack(">H", len(data)) + data


def run_model(args, md, root, rtt):
    proc = subprocess.Popen([args.mcuemu, "serve", "--root", root, "--port", "0"],
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    try:
        port = None
        for line in proc.stdout:
            m = re.search(r":(\d+)", line)
            if m:
                port = int(m.group(1))
                break
        emu = Emu(WebSocket(port))
        emu.call("load", yaml=YAML.format(rtt=rtt))
        emu.call("subscribe", endpoint="thermostat.zb.uart0")
        emu.call("subscribe", endpoint="thermostat.zb.rtt")

        parser = FrameParser()
        rtt_text = ""
        seen = []
        frames = []
        reported = []
        state = "wait_query"
        product = md["products"][0]
        for _ in range(args.max_steps):
            emu.call("run", **{"for": "100ms", "wait": True})
            for ev in emu.take_events():
                data = bytes.fromhex(ev["value"]["hex"])
                if ev["endpoint"].endswith("rtt"):
                    rtt_text += data.decode("latin1")
                    continue
                for cmd, raw in parser.feed(data):
                    seen.append(cmd)
                    frames.append(raw)
            if state == "wait_query" and 0x01 in seen:
                info = json.dumps({"p": product, "v": "1.0.0"}).encode()
                emu.call("set", endpoint="thermostat.zb.uart0", value={"hex": frame(0x01, info).hex()})
                state = "wait_dps"
            elif state == "wait_dps" and 0x28 in seen:
                payload = b""
                for name, i, t, l in md["dps"]:
                    d = dp_bytes(i, t, l)
                    if d:
                        payload += d
                        reported.append((name, i))
                emu.call("set", endpoint="thermostat.zb.uart0",
                         value={"hex": frame(0x05, payload, seq=2).hex()})
                state = "wait_ack"
                steps_after = 0
            elif state == "wait_ack":
                steps_after += 1
                if steps_after >= 20:
                    break
        return seen, rtt_text, reported, state, frames
    finally:
        proc.terminate()
        proc.wait()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--mcuemu", required=True)
    ap.add_argument("--image", required=True, help="thermostat -full.bin of a build with debug.conf")
    ap.add_argument("--elf", required=True, help="its zephyr.elf, for the RTT control block")
    ap.add_argument("--coordinator", required=True)
    ap.add_argument("--models", required=True, help="lib/tuya_thermostat/tt_models.c")
    ap.add_argument("--nm", default=os.environ.get("LLVM_NM", "llvm-nm"))
    ap.add_argument("--model", type=int, action="append", help="model index (0 = r01), default all")
    ap.add_argument("--verbose", action="store_true", help="print the RTT text and the frames")
    ap.add_argument("--max-steps", type=int, default=150)
    args = ap.parse_args()

    nm = subprocess.run([args.nm, args.elf], capture_output=True, text=True, check=True).stdout
    rtt = int(re.search(r"^([0-9a-f]+) \w _SEGGER_RTT$", nm, re.M).group(1), 16)

    models = parse_models(args.models)
    selected = args.model if args.model else range(len(models))
    failed = 0
    with tempfile.TemporaryDirectory() as root:
        shutil.copy(args.image, os.path.join(root, "thermostat.bin"))
        shutil.copy(args.coordinator, os.path.join(root, "coordinator.bin"))
        for idx in selected:
            md = models[idx]
            seen, text, reported, state, frames = run_model(args, md, root, "0x%x" % rtt)
            applied = set(int(x) for x in re.findall(r"tt: dp (\d+) field", text))
            chosen = re.findall(r"tt: model (\S+)", text)
            ok = chosen == [md["name"]] and 0x06 in seen and len(applied) > 0
            print("%s product %s: %s" % (md["name"], md["products"][0], "ok" if ok else "FAIL"))
            print("  model chosen: %s; module sent commands %s; state %s" %
                  (chosen, sorted(set("0x%02x" % c for c in seen)), state))
            print("  datapoints reported %d, applied %d" % (len(reported), len(applied)))
            missing = [n for n, i in reported if i not in applied]
            if missing:
                print("  not applied (value or type the codec refuses): %s" % ", ".join(missing))
            if args.verbose:
                print("  RTT: " + text.replace("\n", "\n       "))
                print("  frames from the module: %s" % [f.hex() for f in frames])
            if not ok:
                failed += 1
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()

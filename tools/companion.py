#!/usr/bin/env python3
"""Drives a Tern node over the companion protocol (draft/companion.md in ternmesh/spec).

    python3 tools/companion.py --port /dev/ttyUSB0 state
    python3 tools/companion.py --port /dev/ttyUSB0 send <address> <text>
    python3 tools/companion.py --port /dev/ttyUSB0 contact <address> <name>
    python3 tools/companion.py --port /dev/ttyUSB0 end <address>
    python3 tools/companion.py --port /dev/ttyUSB0 set power 10
    python3 tools/companion.py --port /dev/ttyUSB0 watch
    python3 tools/companion.py selftest tests/vectors/companion.json

`contact` saves an address under a name, which also lets that node make first contact; `end` ends
the session with one. `state` says hello, sets the node's clock from this computer's, and prints everything the node
holds. `send` sends a message and prints what becomes of it. `watch` prints news as it comes.
While it waits for news it sends PING every IDLE seconds, as the specification asks, and if the
node took it for gone anyway, it says HELLO again and syncs what it missed.
The node's console text, which shares the port, is printed on stderr with --console.

On Windows the port is COM3 or similar. Real ports need pyserial (pip install pyserial); on Linux
and macOS a terminal device works without it. `selftest` checks this script's encoding against
the specification's vectors, and needs nothing.

This is an example client, not a library: every frame is spelled out here, as a client in any
language would spell it out.
"""

import argparse
import json
import os
import random
import struct
import sys
import time

MAX_FRAME, MAGIC = 180, b"\xf5\x54"
VERSION = 1
ANSWER_WAIT = 5.0
IDLE = 20.0  # the most a client lets pass after an answer before it asks again

# type: (name, fields). A field is (name, kind) with kind one of B b H I addr str.
FRAMES = {
    0x01: ("HELLO", [("version", "B")]),
    0x02: ("SYNC", [("after", "I")]),
    0x03: ("PING", []),
    0x04: ("SET_TIME", [("time", "I")]),
    0x05: ("SET", [("setting", "B")]),
    0x10: ("SEND", [("ref", "I"), ("to", "addr"), ("text", "str")]),
    0x11: ("READ", [("through", "I")]),
    0x18: ("SAVE_CONTACT", [("address", "addr"), ("name", "str")]),
    0x19: ("REMOVE_CONTACT", [("address", "addr")]),
    0x1A: ("END_SESSION", [("address", "addr")]),
    0x40: ("OK", []),
    0x41: ("ERROR", [("code", "B")]),
    0x42: ("INFO", [("version", "B"), ("firmware", "str")]),
    0x43: ("SYNCED", []),
    0x44: ("QUEUED", [("id", "I")]),
    0x80: ("SELF", [("address", "addr"), ("role", "B"), ("region", "str"), ("power", "b"),
                    ("time", "I")]),
    0x81: ("CONTACT", [("address", "addr"), ("session", "B"), ("name", "str")]),
    0x82: ("CONTACT_GONE", [("address", "addr")]),
    0x83: ("MESSAGE", [("id", "I"), ("contact", "addr"), ("time", "I"), ("flags", "B"),
                       ("state", "B"), ("reason", "B"), ("wait", "H"), ("text", "str")]),
    0x84: ("STATE", [("id", "I"), ("state", "B"), ("reason", "B"), ("wait", "H")]),
    0x85: ("NEIGHBOUR", [("routing_id", "I"), ("role", "B"), ("snr_quarter_db", "b"),
                         ("heard", "H")]),
    0x86: ("NEIGHBOUR_GONE", [("routing_id", "I")]),
    0x87: ("AIRTIME", [("period", "I"), ("allowed", "I"), ("used", "I"), ("wait", "I")]),
    0x88: ("POWER", [("millivolts", "H"), ("percent", "B"), ("flags", "B")]),
    0x89: ("ASKED", [("address", "addr"), ("why", "B")]),
}
TYPE = {name: t for t, (name, _) in FRAMES.items()}
SETTINGS = {1: ("region", "str"), 2: ("role", "B"), 3: ("power", "b"), 4: ("passkey", "I")}
SETTING = {name: (n, kind) for n, (name, kind) in SETTINGS.items()}
STATES = ["waiting", "sent", "delivered", "not delivered", "received"]
REASONS = ["", "for a route", "for a session", "for the region's limit", "for its budget",
           "for the radio"]
ERRORS = {1: "not something this node knows", 2: "malformed", 3: "refused",
          4: "not a valid address, or the node's own", 5: "no room", 6: "HELLO first",
          7: "the Bluetooth MTU is too small", 8: "not now"}


def crc16(data):
    crc = 0xFFFF
    for byte in data:
        crc ^= byte << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021 if crc & 0x8000 else crc << 1) & 0xFFFF
    return crc


def encode(kind_name, seq, /, **values):
    t = TYPE[kind_name]
    fields = list(FRAMES[t][1])
    if kind_name == "SET":
        fields.append(("value", SETTINGS[values["setting"]][1]))
    out = bytes([t, seq])
    for field, kind in fields:
        v = values[field]
        if kind == "addr":
            out += v
        elif kind == "str":
            raw = v.encode("utf-8")
            out += bytes([len(raw)]) + raw
        else:
            out += struct.pack(">" + kind, v)
    return out


def decode(frame):
    """A frame's fields as a dict, or None for one this script cannot read."""
    if len(frame) < 2 or frame[0] not in FRAMES:
        return None
    name, fields = FRAMES[frame[0]]
    values, at = {"type": name, "seq": frame[1]}, 2
    for field, kind in fields:
        if kind == "addr":
            if at + 32 > len(frame):
                return None
            values[field], at = frame[at:at + 32], at + 32
        elif kind == "str":
            if at >= len(frame) or at + 1 + frame[at] > len(frame):
                return None
            values[field] = frame[at + 1:at + 1 + frame[at]].decode("utf-8", "replace")
            at += 1 + frame[at]
        else:
            size = struct.calcsize(">" + kind)
            if at + size > len(frame):
                return None
            values[field] = struct.unpack(">" + kind, frame[at:at + size])[0]
            at += size
    return values


def wrap(frame):
    body = struct.pack(">H", len(frame)) + frame
    return MAGIC + body + struct.pack(">H", crc16(body))


class Parser:
    """Frames out of a byte stream, and the text between them."""

    def __init__(self):
        self.buf = bytearray()

    def push(self, data):
        """Returns (frames, text) found so far."""
        self.buf += data
        frames, text = [], bytearray()
        while self.buf:
            if self.buf[0] != MAGIC[0] or (len(self.buf) >= 2 and self.buf[1] != MAGIC[1]):
                text.append(self.buf.pop(0))
                continue
            if len(self.buf) < 4:
                break
            n = struct.unpack(">H", self.buf[2:4])[0]
            if not 2 <= n <= MAX_FRAME:
                text.append(self.buf.pop(0))
                continue
            if len(self.buf) < n + 6:
                break
            body = bytes(self.buf[2:n + 4])
            if struct.unpack(">H", self.buf[n + 4:n + 6])[0] != crc16(body):
                text.append(self.buf.pop(0))
                continue
            frames.append(body[2:])
            del self.buf[:n + 6]
        return frames, bytes(text)


class Port:
    """A serial port: pyserial where it is installed, else a POSIX terminal device."""

    def __init__(self, path):
        try:
            import serial  # noqa: PLC0415

            self.s = serial.Serial(path, 115200, timeout=0.05)
            self.fd = None
        except ImportError:
            import termios  # noqa: PLC0415

            self.s = None
            self.fd = os.open(path, os.O_RDWR | os.O_NOCTTY)
            attrs = termios.tcgetattr(self.fd)
            attrs[0] = attrs[1] = attrs[3] = 0  # no input, output or line processing
            attrs[2] = termios.CS8 | termios.CREAD | termios.CLOCAL
            attrs[4] = attrs[5] = termios.B115200
            attrs[6][termios.VMIN], attrs[6][termios.VTIME] = 0, 0
            termios.tcsetattr(self.fd, termios.TCSANOW, attrs)

    def write(self, data):
        if self.s:
            self.s.write(data)
        else:
            os.write(self.fd, data)

    def read(self):
        if self.s:
            return self.s.read(512)
        import select  # noqa: PLC0415

        ready, _, _ = select.select([self.fd], [], [], 0.05)
        return os.read(self.fd, 512) if ready else b""


class Lapsed(Exception):
    """ERROR 6 after HELLO: the node took this client for gone."""


class Node:
    """One connection: requests one at a time, news as it comes."""

    def __init__(self, port, console=False, idle=IDLE):
        self.port, self.parser, self.console, self.idle = port, Parser(), console, idle
        self.seq, self.pending, self.inbox = 0, [], []
        self.greeted, self.answered = False, time.monotonic()
        self.messages = {}  # message id: the MESSAGE last heard, with any STATE since
        self.lost = set()  # ids that, since a restart, name another message or none

    def _read(self):
        frames, text = self.parser.push(self.port.read())
        if text and self.console:
            sys.stderr.write(text.decode("utf-8", "replace"))
        for m in filter(None, map(decode, frames)):
            if m["type"] == "MESSAGE":
                self.messages[m["id"]] = dict(m, seq=0)
            elif m["type"] == "STATE" and m["id"] in self.messages:
                self.messages[m["id"]].update(state=m["state"], reason=m["reason"],
                                              wait=m["wait"])
            self.inbox.append(m)

    def _frames(self, wait):
        """Frames that arrive within `wait` seconds of the last. Several can come in one read;
        those not yet taken wait in the inbox for the next caller."""
        end = time.monotonic() + wait
        while True:
            while self.inbox:
                end = time.monotonic() + wait
                yield self.inbox.pop(0)
            if time.monotonic() >= end:
                return
            self._read()

    def request(self, kind, /, **values):
        """Sends a request and returns its answer, keeping any news that came first."""
        self.seq = self.seq % 255 + 1
        self.port.write(wrap(encode(kind, self.seq, **values)))
        for m in self._frames(ANSWER_WAIT):
            if TYPE[m["type"]] >= 0x80:
                self.pending.append(m)
            elif m["seq"] == self.seq:
                self.answered = time.monotonic()
                if m["type"] == "ERROR" and m["code"] == 6 and self.greeted:
                    raise Lapsed()
                if m["type"] == "ERROR":
                    raise SystemExit(f"{kind}: {ERRORS.get(m['code'], m['code'])}")
                return m
        raise SystemExit(f"{kind}: no answer. Is the node on this port, and running Tern?")

    def news(self, wait):
        """News kept, then news for `wait` seconds. A PING goes whenever `idle` has passed since
        the last answer, and a node that took this client for gone is greeted again."""
        end = time.monotonic() + wait
        while True:
            self.pending += [m for m in self.inbox if TYPE[m["type"]] >= 0x80]
            self.inbox.clear()
            while self.pending:
                yield self.pending.pop(0)
            now = time.monotonic()
            if now >= end:
                return
            if now - self.answered < self.idle:
                self._read()
                continue
            try:
                self.request("PING")
            except Lapsed:
                print("the node took this client for gone: saying HELLO again", flush=True)
                self.catch_up()

    def catch_up(self):
        """Greets the node again and syncs from the first message, keeping as news only the
        messages that differ from those already heard. Not from the last id heard: the same
        ERROR 6 comes from a node that restarted, and its ids start again at 1."""
        heard = self.messages
        self.messages = {}
        self.hello()
        self.request("SYNC", after=0)
        self.pending = [m for m in self.pending
                        if m["type"] != "MESSAGE" or heard.get(m["id"]) != dict(m, seq=0)]
        same = ("contact", "time", "text")
        self.lost |= {i for i, m in heard.items()
                      if [m[k] for k in same] != [self.messages.get(i, {}).get(k) for k in same]}

    def hello(self):
        self.greeted = False
        info = self.request("HELLO", version=VERSION)
        self.greeted = True
        self.pending.clear()
        return info


def describe(m, names):
    t = m["type"]
    if t == "SELF":
        return (f"this node: {m['address'].hex()}\n  a {'relay' if m['role'] else 'leaf'} in "
                f"{m['region'] or 'no region'}, at {m['power']} dBm")
    if t == "CONTACT":
        names[m["address"]] = m["name"]
        return (f"contact {m['name']!r}: {m['address'].hex()}"
                f"{', with a session' if m['session'] else ''}")
    if t == "CONTACT_GONE":
        return f"contact removed: {m['address'].hex()}"
    if t == "MESSAGE":
        who = names.get(m["contact"], m["contact"].hex()[:16])
        way = "from" if m["state"] == 4 else "to"
        read = ", read" if m["flags"] & 1 else ""
        return f"message #{m['id']} {way} {who}: {m['text']!r} ({state(m)}{read})"
    if t == "STATE":
        return f"message #{m['id']}: {state(m)}"
    if t == "NEIGHBOUR":
        return (f"neighbour {m['routing_id']:08x}, a {'relay' if m['role'] else 'leaf'}, "
                f"SNR {m['snr_quarter_db'] / 4} dB, heard {m['heard']} s ago")
    if t == "NEIGHBOUR_GONE":
        return f"neighbour {m['routing_id']:08x} gone"
    if t == "AIRTIME":
        if not m["period"]:
            return "air: no limit on transmitting"
        return (f"air: {m['used']} of {m['allowed']} ms used in {m['period']} s"
                + (f", {m['wait']} ms until the longest frame may go" if m["wait"] else ""))
    if t == "POWER":
        if not m["millivolts"] and m["percent"] == 255:
            return "power: not measured"
        return f"power: {m['millivolts']} mV, {m['percent']}%"
    if t == "ASKED":
        why = {1: "it is not a contact: save it as one to let it in",
               2: "there is no room for another session: end one to make room"}
        return (f"refused first contact from {m['address'].hex()}: "
                f"{why.get(m['why'], 'no reason this script knows')}")
    return str(m)


def state(m):
    s = STATES[m["state"]] if m["state"] < len(STATES) else str(m["state"])
    if m["state"] == 0 and m["reason"] < len(REASONS) and m["reason"]:
        s += " " + REASONS[m["reason"]]
    if m["state"] == 0 and m["wait"]:
        s += f", about {m['wait']} s"
    return s


def address(text):
    raw = bytes.fromhex(text)
    if len(raw) != 32:
        raise SystemExit("an address is sixty-four hex digits")
    return raw


def run(args):
    node = Node(Port(args.port), args.console, args.idle)
    info = node.hello()
    print(f"{info['firmware']}, companion protocol version {info['version']}")
    names = {}
    if args.command == "state":
        node.request("SET_TIME", time=int(time.time()))
        node.request("SYNC", after=0)
        for m in node.pending:
            print(describe(m, names))
    elif args.command == "send":
        ref = random.getrandbits(32)
        to = address(args.address)
        q = node.request("SEND", ref=ref, to=to, text=args.text)
        print(f"queued as message #{q['id']}")
        for m in node.news(args.wait):
            if m.get("id") != q["id"]:
                continue
            # A node that restarted numbers its messages from 1 again, so this id may now be
            # another message's: one that is not the text sent, or one a catch-up found changed.
            if m["type"] == "MESSAGE" and (m["contact"], m["text"]) != (to, args.text):
                node.lost.add(q["id"])
            if q["id"] in node.lost:
                break
            print(describe(m, names))
            if m["state"] in (2, 3):
                break
        if q["id"] in node.lost:
            print(f"message #{q['id']}: the node restarted, and no longer holds it")
    elif args.command == "contact":
        node.request("SAVE_CONTACT", address=address(args.address), name=args.name)
        print("saved")
    elif args.command == "end":
        if info["version"] < 1:
            raise SystemExit("this node's firmware is too old to end a session from here")
        node.request("END_SESSION", address=address(args.address))
        print("ended")
    elif args.command == "set":
        n, kind = SETTING[args.setting]
        value = args.value if kind == "str" else int(args.value, 0)
        node.request("SET", setting=n, value=value)
        print("set")
    elif args.command == "watch":
        # From the first message, so a sync after a lapse can tell what changed; the old ones
        # are not news, and are not printed.
        node.request("SYNC", after=0)
        node.pending = [m for m in node.pending if m["type"] != "MESSAGE"]
        end = time.monotonic() + args.seconds if args.seconds else None
        while end is None or time.monotonic() < end:
            for m in node.news(1.0):
                print(describe(m, names), flush=True)


def selftest(path):
    """Checks this script's frames and framing against the specification's vectors."""
    v = json.load(open(path, encoding="utf-8"))
    failed = 0
    for f in v["frames"]:
        fields = dict(f["fields"])
        for k in ("to", "address", "contact"):
            if k in fields:
                fields[k] = bytes.fromhex(fields[k])
        frame = bytes.fromhex(f["frame"])
        ok = encode(f["type"], f["seq"], **fields) == frame and wrap(frame).hex() == f["stream"]
        got = decode(frame)
        ok = ok and got is not None and got["type"] == f["type"]
        failed += not ok
    for s in v["streams"]:
        frames, text = Parser().push(bytes.fromhex(s["stream"]))
        want = [bytes.fromhex(i["frame"]) for i in s["items"] if "frame" in i]
        failed += frames != want
    print("selftest:", "failed" if failed else "passed")
    return 1 if failed else 0


def main():
    p = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    p.add_argument("--port", help="the node's serial port")
    p.add_argument("--console", action="store_true", help="print the node's console on stderr")
    p.add_argument("--idle", type=float, default=IDLE,
                   help=f"seconds between requests while waiting for news (default {IDLE:g})")
    sub = p.add_subparsers(dest="command", required=True)
    sub.add_parser("state")
    s = sub.add_parser("send")
    s.add_argument("address")
    s.add_argument("text")
    s.add_argument("--wait", type=float, default=30.0, help="seconds to watch it for")
    c = sub.add_parser("contact")
    c.add_argument("address")
    c.add_argument("name")
    e = sub.add_parser("end")
    e.add_argument("address")
    t = sub.add_parser("set")
    t.add_argument("setting", choices=list(SETTING))
    t.add_argument("value")
    w = sub.add_parser("watch")
    w.add_argument("--seconds", type=float, help="stop after this long (default: never)")
    x = sub.add_parser("selftest")
    x.add_argument("vectors")
    args = p.parse_args()
    if args.command == "selftest":
        return selftest(args.vectors)
    if not args.port:
        p.error("--port is needed")
    run(args)
    return 0


if __name__ == "__main__":
    sys.exit(main())

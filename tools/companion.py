#!/usr/bin/env python3
"""Drives a Tern node over the companion protocol (draft/companion.md in ternmesh/spec).

    python3 tools/companion.py --port /dev/ttyUSB0 state
    python3 tools/companion.py --port /dev/ttyUSB0 send <address> <text>
    python3 tools/companion.py --port /dev/ttyUSB0 contact <address> <name>
    python3 tools/companion.py --port /dev/ttyUSB0 end <address>
    python3 tools/companion.py --port /dev/ttyUSB0 set power 10
    python3 tools/companion.py --port /dev/ttyUSB0 watch
    python3 tools/companion.py --port /dev/ttyUSB0 update tern-heltec-v3-eu868-0.2.0-app.bin
    python3 tools/companion.py selftest tests/vectors/companion.json

`contact` saves an address under a name, which also lets that node make first contact; `end` ends
the session with one. `state` says hello, sets the node's clock from this computer's, and prints everything the node
holds. `send` sends a message and prints what becomes of it. `watch` prints news as it comes.
`update` gives the node a new image of its firmware, going on from where the node's bytes end if
it was given part of the same image before, and the node restarts into it.
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
# The version this script speaks. It has no commands for groups, which came with version 2, so it
# says 1 and is told of none: the frames of versions 2 and 3 are here only to be checked. `update`
# says the latest, which it needs.
VERSION = 1
# The latest version the frames below are, which selftest reads the vectors by.
LATEST = 4
UPDATE_CHUNK = 172
ANSWER_WAIT = 5.0
IDLE = 20.0  # the most a client lets pass after an answer before it asks again

# type: (name, fields). A field is (name, kind) with kind one of B b H I addr gid digest str raw
# (bytes of anything, after a length).
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
    0x20: ("MAKE_GROUP", [("name", "str")]),
    0x21: ("LEAVE_GROUP", [("group", "gid")]),
    0x22: ("NAME_GROUP", [("group", "gid"), ("name", "str")]),
    0x23: ("SEND_GROUP", [("ref", "I"), ("group", "gid"), ("text", "str")]),
    0x24: ("SEND_INVITE", [("group", "gid"), ("to", "addr")]),
    0x25: ("JOIN", [("id", "I")]),
    0x30: ("UPDATE_BEGIN", [("size", "I"), ("digest", "digest")]),
    0x31: ("UPDATE_DATA", [("offset", "I"), ("data", "raw")]),
    0x32: ("UPDATE_END", []),
    0x40: ("OK", []),
    0x41: ("ERROR", [("code", "B")]),
    0x42: ("INFO", [("version", "B"), ("firmware", "str"), ("board", "str"), ("release", "str")]),
    0x43: ("SYNCED", [("news", "B")]),
    0x44: ("QUEUED", [("id", "I")]),
    0x45: ("MADE", [("group", "gid")]),
    0x46: ("UPDATING", [("offset", "I")]),
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
    0x8A: ("GROUP", [("group", "gid"), ("name", "str")]),
    0x8B: ("GROUP_GONE", [("group", "gid")]),
    0x8C: ("GROUP_MESSAGE", [("id", "I"), ("group", "gid"), ("from", "I"), ("time", "I"),
                             ("flags", "B"), ("state", "B"), ("reason", "B"), ("wait", "H"),
                             ("text", "str")]),
    0x8D: ("INVITE", [("id", "I"), ("contact", "addr"), ("group", "gid"), ("time", "I"),
                      ("flags", "B"), ("state", "B"), ("reason", "B"), ("wait", "H"),
                      ("name", "str")]),
}
# Fields a later version added at the end of a frame: an earlier version's frame stops before them.
LATER = {(0x43, "news"): 3, (0x42, "board"): 4, (0x42, "release"): 4}
BYTES = {"addr": 32, "gid": 8, "digest": 32}
TYPE = {name: t for t, (name, _) in FRAMES.items()}
SETTINGS = {1: ("region", "str"), 2: ("role", "B"), 3: ("power", "b"), 4: ("passkey", "I")}
SETTING = {name: (n, kind) for n, (name, kind) in SETTINGS.items()}
STATES = ["waiting", "sent", "delivered", "not delivered", "received"]
REASONS = ["", "for a route", "for a session", "for the region's limit", "for its budget",
           "for the radio"]
ERRORS = {1: "not something this node knows", 2: "malformed", 3: "refused",
          4: "not a valid address, or the node's own", 5: "no room", 6: "HELLO first",
          7: "the Bluetooth MTU is too small", 8: "not now", 9: "not held",
          10: "not where the update is", 11: "not an image this node runs"}


def crc16(data):
    crc = 0xFFFF
    for byte in data:
        crc ^= byte << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021 if crc & 0x8000 else crc << 1) & 0xFFFF
    return crc


def fields_of(t, speak):
    """A type's fields as version `speak` has them."""
    return [(f, k) for f, k in FRAMES[t][1] if LATER.get((t, f), 0) <= speak]


def encode(kind_name, seq, /, *, speak=VERSION, **values):
    t = TYPE[kind_name]
    fields = fields_of(t, speak)
    if kind_name == "SET":
        fields.append(("value", SETTINGS[values["setting"]][1]))
    out = bytes([t, seq])
    for field, kind in fields:
        v = values[field]
        if kind in BYTES:
            out += v
        elif kind == "str":
            raw = v.encode("utf-8")
            out += bytes([len(raw)]) + raw
        elif kind == "raw":
            out += bytes([len(v)]) + v
        else:
            out += struct.pack(">" + kind, v)
    return out


def decode(frame, speak=VERSION):
    """A frame's fields as version `speak` has them, or None for one this script cannot read."""
    if len(frame) < 2 or frame[0] not in FRAMES:
        return None
    if frame[0] == 0x42 and len(frame) > 2:
        speak = min(speak, frame[2])  # INFO, from a node older than this script: as it has it
    name, fields = FRAMES[frame[0]][0], fields_of(frame[0], speak)
    values, at = {"type": name, "seq": frame[1]}, 2
    for field, kind in fields:
        if kind in BYTES:
            if at + BYTES[kind] > len(frame):
                return None
            values[field], at = frame[at:at + BYTES[kind]], at + BYTES[kind]
        elif kind in ("str", "raw"):
            if at >= len(frame) or at + 1 + frame[at] > len(frame):
                return None
            raw = frame[at + 1:at + 1 + frame[at]]
            values[field] = raw.decode("utf-8", "replace") if kind == "str" else raw
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

    def __init__(self, port, console=False, idle=IDLE, speak=VERSION):
        self.port, self.parser, self.console, self.idle = port, Parser(), console, idle
        self.speak = speak
        self.seq, self.pending, self.inbox = 0, [], []
        self.greeted, self.answered = False, time.monotonic()
        self.messages = {}  # message id: the MESSAGE last heard, with any STATE since
        self.lost = set()  # ids that, since a restart, name another message or none

    def _read(self):
        frames, text = self.parser.push(self.port.read())
        if text and self.console:
            sys.stderr.write(text.decode("utf-8", "replace"))
        for m in filter(None, (decode(f, self.speak) for f in frames)):
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

    def request(self, kind, /, refusable=False, **values):
        """Sends a request and returns its answer, keeping any news that came first. An ERROR
        ends the script, unless the caller takes refusals."""
        self.seq = self.seq % 255 + 1
        self.port.write(wrap(encode(kind, self.seq, speak=self.speak, **values)))
        for m in self._frames(ANSWER_WAIT):
            if TYPE[m["type"]] >= 0x80:
                self.pending.append(m)
            elif m["seq"] == self.seq:
                self.answered = time.monotonic()
                if m["type"] == "ERROR" and m["code"] == 6 and self.greeted:
                    raise Lapsed()
                if m["type"] == "ERROR" and not refusable:
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
        info = self.request("HELLO", version=self.speak)
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


def update(node, info, path):
    """Sends the image a chunk at a time, from where the node's bytes end, and asks it to run it.
    A chunk not answered is sent again; one the node had already is answered without being taken
    twice. UPDATE_END is not sent again: the node may be restarting into the image."""
    if info["version"] < 4:
        raise SystemExit("this node's firmware is too old to be updated over this link")
    if not info["board"]:
        raise SystemExit("this node cannot be updated over this link: flash it over USB")
    image = open(path, "rb").read()
    import hashlib  # noqa: PLC0415

    digest = hashlib.sha256(image).digest()
    print(f"updating {info['board']} from {info['release'] or 'an unknown release'}: "
          f"{len(image)} bytes, SHA-256 {digest.hex()}")
    at = node.request("UPDATE_BEGIN", size=len(image), digest=digest)["offset"]
    if at:
        print(f"going on from byte {at}")
    shown = -1
    while at < len(image):
        chunk = image[at:at + UPDATE_CHUNK]
        a = node.request("UPDATE_DATA", refusable=True, offset=at, data=chunk)
        if a["type"] == "ERROR" and a["code"] == 10:
            at = node.request("UPDATE_BEGIN", size=len(image), digest=digest)["offset"]
            continue
        if a["type"] == "ERROR":
            raise SystemExit(f"UPDATE_DATA: {ERRORS.get(a['code'], a['code'])}")
        at += len(chunk)
        percent = at * 100 // len(image)
        if percent != shown:
            print(f"\r{percent}%", end="", flush=True)
            shown = percent
    print()
    node.request("UPDATE_END")
    print("the node has the image, and restarts into it")


def run(args):
    speak = LATEST if args.command == "update" else VERSION
    node = Node(Port(args.port), args.console, args.idle, speak)
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
    elif args.command == "update":
        update(node, info, args.image)
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
        for k in ("to", "address", "contact", "group", "digest", "data"):
            if k in fields:
                fields[k] = bytes.fromhex(fields[k])
        frame = bytes.fromhex(f["frame"])
        ok = encode(f["type"], f["seq"], speak=LATEST, **fields) == frame
        ok = ok and wrap(frame).hex() == f["stream"]
        got = decode(frame, LATEST)
        ok = ok and got is not None and got["type"] == f["type"]
        failed += not ok
    # Each connection read by the version its client speaks: an older one's SYNCED is two bytes.
    runs = [v["exchange"], *v["update"], v["refusals"]]
    for c in [{"version": LATEST, "frames": f} for f in runs] + v["older"]:
        for f in c["frames"]:
            got = decode(bytes.fromhex(f["frame"]), c["version"])
            failed += got is None or got["type"] != f["type"]
            failed += got is not None and f["type"] == "SYNCED" and ("news" in got) != (c["version"] >= 3)
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
    u = sub.add_parser("update")
    u.add_argument("image", help="the firmware's -app.bin image for this node's board and region")
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

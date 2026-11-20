#!/usr/bin/env python3
"""Reads the logs tools/fieldlog.py wrote and prints a field test's numbers (docs/field-test.md).

    python3 tools/fieldsummary.py logs/
    python3 tools/fieldsummary.py logs-base/ logs-far/      # loggers at two sites

Every <name>.log in the folders given is a board, by that name. A test runs from a line
`## test <name>` in any log to the next, or to `## end`; a mark `## off <board>` or
`## on <board>` says a board was switched off or on. Everything is read from what the firmware
prints today: `status`, `routes` and `sleep` typed on each board during a test give the counters
and the routing ids that tie the boards together, so type them at the start and end of each.

For each test it prints:

* unicast, for each source and destination: messages sent, acknowledged and heard; the tries
  each took; the time from first sent to heard (one way, which needs both boards' logs on clocks
  that agree) and to acknowledged (a round trip, on the source's clock alone); the path each took
  and its hops, from the relays' `passed on` lines; and hops sent again along the way;
* first contact: each handshake's time from `contact` to complete, and how often each of its
  frames went;
* sleeping leaves: polls answered and not, the keepers that answered and when the keeper
  changed, frames held and received, and the share of time the radio was on;
* airtime: each board's budget and counters, from its last `status` in the test, and the time
  on air of the frames its log shows;
* recovery: for each board switched off, how long each source took to have a message
  acknowledged again, and what was lost meanwhile.

Messages are matched by their text, so give each a text of its own: `send line5-17`.
Needs nothing but Python 3. `--self-test` checks it against logs it writes itself.
"""

import argparse
import collections
import datetime
import glob
import math
import os
import re
import statistics
import sys

LINE = re.compile(r"^(\d{4}-\d\d-\d\dT\d\d:\d\d:\d\d\.\d{3})Z (.*)$")
HEX8 = r"([0-9a-f]{8})"

# What the firmware prints (ports/node/node.c), and what each is called here.
PATTERNS = [
    ("sent", re.compile(r'^sent #(\d+)( again)? "(.*)": (\d+) bytes at (-?\d+) dBm by ' + HEX8 +
                        r", (\d+)\.(\d{3}) ms on the air$")),
    ("delivered", re.compile(r"^delivered #(\d+): acknowledged (\d+) ms after it was handed over, "
                             r"having gone (\d+) times?$")),
    ("lost", re.compile(r"^not delivered #(\d+): no acknowledgement, having gone (\d+) times?")),
    ("heard", re.compile(r'^heard #(\d+) "(.*)" at (-?\d+) dBm, SNR (-?\d+\.\d+) dB$')),
    ("copy", re.compile(r"^heard #(\d+) again: ")),
    ("relayed", re.compile(r"^passed on a (\d+)-byte frame for " + HEX8 + " by " + HEX8 +
                           r", at (-?\d+) dBm$")),
    ("flooded", re.compile(r"^passed on a (\d+)-byte frame for every node, at (-?\d+) dBm$")),
    ("group", re.compile(r"^sent to a group: (\d+) bytes at (-?\d+) dBm, (\d+)\.(\d{3}) ms on "
                         r"the air$")),
    ("fc_sent", re.compile(r"^first contact: sent message_(\d), (\d+) bytes at (-?\d+) dBm by " +
                           HEX8 + "$")),
    ("fc_heard", re.compile(r"^first contact: heard message_(\d) at ")),
    ("fc_done", re.compile(r"^first contact: complete\. Session started, as the "
                           r"(initiator|responder), with ([0-9A-F]{64})\.")),
    ("fc_gave_up", re.compile(r"^first contact: no answer to message_(\d) after (\d+) tries")),
    ("fc_refused", re.compile(r"^first contact: refused ([0-9A-F]{64})")),
    ("fc_failed", re.compile(r"^first contact: a frame of the handshake failed its checks")),
    ("fc_unsent", re.compile(r"^first contact: message_(\d) not sent: ")),
    ("poll_ok", re.compile(r"^poll (\d+): answered by " + HEX8 + r", (\d+) frames? held$")),
    ("poll_miss", re.compile(r"^poll (\d+): no answer from " + HEX8 + r" \((\d+) in a row\)$")),
    ("poll_got", re.compile(r"^poll (\d+): listened, (\d+) of (\d+) held frames? came")),
    ("leaf_radio", re.compile(r"^radio: (asleep now|on now); on (\d+)\.(\d)% of the (\d+) s "
                              r"since it started, asleep (\d+) s in (\d+) sleeps$")),
    ("keeper", re.compile(r"^sleep: a relay, which never sleeps\..*held (\d+) frames for them, "
                          r"handed (\d+) over, dropped (\d+) for want of room and (\d+) held "
                          r"too long$")),
    ("told_sleeps", re.compile(r"^([0-9A-F]{64}) sleeps: up to (\d+) s between wakes")),
    ("address", re.compile(r"^this board's address: ([0-9A-F]{64})$")),
    ("radio", re.compile(r"^radio: (\S+?)(?: \(not the region's settings\))?, (\d+) Hz, SF(\d+), "
                         r"(\d+) Hz, CR 4/(\d), (-?\d+) dBm")),
    ("window", re.compile(r"^transmitted: (\d+) ms of the (\d+) ms allowed in any (\d+) s$")),
    ("budget", re.compile(r"^airtime budget: (\d+) ms of (\d+) ms left for its own frames"
                          r"(?:, (\d+) ms of (\d+) ms for frames it passes on)?$")),
    ("routing", re.compile(r"^routing: id " + HEX8 + r", a (relay|leaf), hears (\d+), (\d+) of "
                           r"them both ways, routes to (\d+) ")),
    ("routing_id", re.compile(r"^routing id " + HEX8 + r", seq (\d+)")),
    ("forwarding", re.compile(r"^forwarding: passed on (\d+), sent (\d+) hops again and gave (\d+) "
                              r"up; dropped (\d+) with no route, (\d+) with no room; waited (\d+) "
                              r"times")),
    ("route", re.compile(r"^  " + HEX8 + "  by " + HEX8 + r"  (\d+) ms on the air$")),
    ("radio_error", re.compile(r"^radio error (-?\d+)")),
    ("duty_refused", re.compile(r"^not sent: this board has transmitted as much as")),
    ("watchdog", re.compile(r"^the watchdog restarted this board")),
    # Not Tern's: what an ESP32's ROM and Zephyr print as they start.
    ("boot", re.compile(r"^(ESP-ROM:|rst:0x|\*\*\* Booting Zephyr)")),
    ("cmd", re.compile(r"^>> (.*)$")),
    ("mark", re.compile(r"^## (.*)$")),
]

FORWARDING = ("passed on", "hops sent again", "hops given up", "no route", "no room", "waited")
PREAMBLE = 16  # symbols: every Tern frame's (the specification's draft/phy.md)


def airtime_ms(length, sf, bw, cr, preamble=PREAMBLE):
    """A frame's time on the air, as src/lora.c reckons it: explicit header, CRC on, low data rate
    optimisation where a symbol is 16 ms or longer."""
    ldro = (1 << sf) * 1000 >= bw * 16
    bits = 8 * length - 4 * sf + 28 + 16
    per_block = 4 * (sf - (2 if ldro else 0))
    blocks = -(-bits // per_block) if bits > 0 else 0
    symbols = 8 + blocks * (cr + 4)
    return (4 * preamble + 17 + 4 * symbols) * (1 << sf) * 1000 / (bw * 4)


def when(text):
    return datetime.datetime.strptime(text, "%Y-%m-%dT%H:%M:%S.%f").replace(
        tzinfo=datetime.timezone.utc).timestamp()


def clock(t):
    return datetime.datetime.fromtimestamp(t, datetime.timezone.utc).strftime("%H:%M:%S")


class Event:
    __slots__ = ("t", "board", "kind", "m", "text")

    def __init__(self, t, board, kind, m, text):
        self.t, self.board, self.kind, self.m, self.text = t, board, kind, m, text


def read_logs(folders):
    events, boards = [], []
    for folder in folders:
        for path in sorted(glob.glob(os.path.join(folder, "*.log"))):
            board = os.path.basename(path)[:-4]
            if board in boards:
                sys.exit(f"two logs for the board {board!r}: give each board a name of its own")
            boards.append(board)
            with open(path, encoding="utf-8", errors="replace") as f:
                for raw in f:
                    m = LINE.match(raw.rstrip("\n"))
                    if not m:
                        continue
                    t, text = when(m.group(1)), m.group(2)
                    for kind, pattern in PATTERNS:
                        mm = pattern.match(text)
                        if mm:
                            events.append(Event(t, board, kind, mm, text))
                            break
    events.sort(key=lambda e: e.t)
    return events, boards


def tests_of(events):
    """(name, start, end) for each test, from the marks: one mark in every log is one test."""
    marks = []
    for e in events:
        if e.kind != "mark":
            continue
        words = e.m.group(1).split()
        if words and words[0] in ("test", "end"):
            name = " ".join(words[1:]) if words[0] == "test" else None
            if marks and marks[-1][0] == name and e.t - marks[-1][1] < 10:
                continue  # the same mark, from all.in into every log
            marks.append((name, e.t))
    last = events[-1].t + 1 if events else 0
    tests = []
    for i, (name, start) in enumerate(marks):
        if name is not None:
            end = marks[i + 1][1] if i + 1 < len(marks) else last
            tests.append((name, start, end))
    if not tests and events:
        tests.append(("(the whole log)", events[0].t, last))
    return tests


def pct(n, d):
    return f"{100 * n / d:.1f}%" if d else "-"


def spread(values, unit="ms", scale=1.0):
    if not values:
        return "-"
    v = sorted(x * scale for x in values)
    p95 = v[min(len(v) - 1, math.ceil(0.95 * len(v)) - 1)]
    return (f"p50 {statistics.median(v):.0f} {unit}, p95 {p95:.0f} {unit}, max {v[-1]:.0f} {unit}"
            f" (n={len(v)})")


class Summary:
    def __init__(self, events, boards):
        self.events, self.boards = events, boards
        # What ties the boards together, from every log, whichever test it was typed in.
        self.id_of, self.board_of_id, self.board_of_address, self.role = {}, {}, {}, {}
        self.radio = {}
        for e in events:
            if e.kind in ("routing", "routing_id"):
                rid = e.m.group(1)
                self.id_of[e.board] = rid
                self.board_of_id[rid] = e.board
                if e.kind == "routing":
                    self.role[e.board] = e.m.group(2)
            elif e.kind == "address":
                self.board_of_address[e.m.group(1)] = e.board
            elif e.kind == "radio" and e.board not in self.radio:
                g = e.m.groups()
                self.radio[e.board] = (int(g[2]), int(g[3]), int(g[4]) - 4)

    def name(self, rid):
        return self.board_of_id.get(rid, rid)

    def air(self, board, length):
        r = self.radio.get(board) or next(iter(self.radio.values()), None)
        return airtime_ms(length, *r) if r else None

    # --- Unicast -------------------------------------------------------------------------------

    def messages(self, ev):
        """Each message a board sent: its first and later goes, and what became of it."""
        msgs, open_by_counter = [], {}
        for e in ev:
            if e.kind == "sent":
                counter, again, text = e.m.group(1), e.m.group(2), e.m.group(3)
                key = (e.board, counter)
                msg = open_by_counter.get(key)
                if msg is None or (not again and msg["text"] != text):
                    msg = {"src": e.board, "counter": counter, "text": text, "t": e.t,
                           "len": int(e.m.group(4)), "goes": [], "outcome": None}
                    msgs.append(msg)
                    open_by_counter[key] = msg
                msg["goes"].append((e.t, e.m.group(6), float(e.m.group(7) + "." + e.m.group(8))))
            elif e.kind in ("delivered", "lost"):
                msg = open_by_counter.pop((e.board, e.m.group(1)), None)
                if msg is not None:
                    msg["outcome"] = (e.kind, e.t, int(e.m.group(2)) if e.kind == "delivered"
                                      else None, int(e.m.groups()[-1]))
        return msgs

    def path(self, msg, ev, until):
        """The boards a message crossed, from its source's first go and each relay's `passed on`:
        the relay its source sent it by passed on a frame as long, then the next, and so on."""
        t0, nxt = msg["goes"][0][0], msg["goes"][0][1]
        hops, at, retries, dest_id = [msg["src"]], t0, 0, None
        dest = msg.get("dest")
        if dest is not None:
            dest_id = self.id_of.get(dest)
        for _ in range(32):
            board = self.board_of_id.get(nxt)
            if board is None:
                hops.append(nxt + "?")  # a board with no log, or whose id no `status` gave
                break
            hops.append(board)
            if nxt == dest_id or board == dest:
                break
            sends = [e for e in ev if e.board == board and e.kind == "relayed" and
                     int(e.m.group(1)) == msg["len"] and at <= e.t <= until and
                     (dest_id is None or e.m.group(2) == dest_id)]
            if not sends:
                break
            retries += len({e.t for e in sends if e.m.group(3) == sends[0].m.group(3)}) - 1
            dest_id = dest_id or sends[0].m.group(2)
            at, nxt = sends[0].t, sends[0].m.group(3)
        return hops, retries

    def unicast(self, ev, out):
        msgs = self.messages(ev)
        heard = [e for e in ev if e.kind == "heard"]
        used = set()
        for msg in msgs:
            for e in heard:
                if id(e) not in used and e.m.group(2) == msg["text"] and e.t >= msg["t"] - 1:
                    used.add(id(e))
                    msg["dest"], msg["heard_at"] = e.board, e.t
                    break
        # A message never heard went where that source's other messages went, as far as can tell.
        usual = collections.defaultdict(collections.Counter)
        for msg in msgs:
            if "dest" in msg:
                usual[msg["src"]][msg["dest"]] += 1
        for msg in msgs:
            if "dest" not in msg and usual[msg["src"]]:
                msg["dest"] = usual[msg["src"]].most_common(1)[0][0]
        pairs = collections.defaultdict(list)
        for msg in msgs:
            pairs[(msg["src"], msg.get("dest", "?"))].append(msg)
        if not pairs:
            return
        out.append("unicast")
        for (src, dest), ms in sorted(pairs.items()):
            acked = [m for m in ms if m["outcome"] and m["outcome"][0] == "delivered"]
            lost = [m for m in ms if m["outcome"] and m["outcome"][0] == "lost"]
            got = [m for m in ms if "heard_at" in m]
            out.append(f"  {src} -> {dest}: {len(ms)} sent, {len(acked)} acknowledged "
                       f"({pct(len(acked), len(ms))}), {len(got)} heard ({pct(len(got), len(ms))}), "
                       f"{len(lost)} not delivered, {len(ms) - len(acked) - len(lost)} still open")
            goes = [len(m["goes"]) for m in ms]
            out.append(f"    goes from the source: mean {statistics.mean(goes):.2f}, max {max(goes)}"
                       f"; acknowledged after {spread([m['outcome'][3] for m in acked], 'goes')}"
                       if acked else
                       f"    goes from the source: mean {statistics.mean(goes):.2f}, max {max(goes)}")
            out.append("    one way, first sent to heard: " +
                       spread([m["heard_at"] - m["t"] for m in got], scale=1000))
            out.append("    round trip, handed over to acknowledged: " +
                       spread([m["outcome"][2] for m in acked]))
            paths, relay_retries = collections.Counter(), 0
            for m in ms:
                hops, retries = self.path(m, ev, m.get("heard_at", m["t"] + 60))
                if hops[-1] == dest:
                    paths[f"{' > '.join(hops)} ({len(hops) - 1} hops)"] += 1
                else:
                    paths[f"{' > '.join(hops)}, and no further in the logs"] += 1
                relay_retries += retries
            for p, n in paths.most_common():
                out.append(f"    path {p}: {n}")
            out.append(f"    hops sent again by relays with logs: {relay_retries}")
            copies = sum(1 for e in ev if e.kind == "copy" and e.board == dest)
            if copies:
                out.append(f"    copies heard again at {dest}, their acknowledgement lost: {copies}")
        self.msgs = msgs

    # --- First contact -------------------------------------------------------------------------

    def contact(self, ev, out):
        rows = []
        for e in ev:
            if e.kind == "cmd" and e.m.group(1).startswith("contact "):
                rows.append({"board": e.board, "t": e.t, "sent": collections.Counter(),
                             "done": None, "peer": e.m.group(1)[8:].strip(), "end": None})
            elif e.kind == "fc_sent":
                row = next((r for r in reversed(rows) if r["board"] == e.board and not r["end"]),
                           None)
                if row is None and e.m.group(1) == "1":
                    row = {"board": e.board, "t": e.t, "sent": collections.Counter(), "done": None,
                           "peer": "?", "end": None}
                    rows.append(row)
                if row is not None:
                    row["sent"][e.m.group(1)] += 1
            elif e.kind in ("fc_done", "fc_gave_up", "fc_failed", "fc_unsent"):
                row = next((r for r in reversed(rows) if r["board"] == e.board and not r["end"]),
                           None)
                if row is None:
                    continue
                if e.kind == "fc_done" and e.m.group(1) == "initiator":
                    row["done"], row["end"] = e.t, "complete"
                    row["peer"] = self.board_of_address.get(e.m.group(2), e.m.group(2)[:8])
                elif e.kind == "fc_gave_up":
                    row["end"] = f"given up at message_{e.m.group(1)}"
                elif e.kind == "fc_failed":
                    row["end"] = "failed its checks"
                elif e.kind == "fc_unsent":
                    row["sent"]["unsent " + e.m.group(1)] += 1
        responders = [e for e in ev if e.kind == "fc_done" and e.m.group(1) == "responder"]
        refused = [e for e in ev if e.kind == "fc_refused"]
        if not rows and not responders and not refused:
            return
        out.append("first contact")
        times = []
        for r in rows:
            peer = self.board_of_address.get(r["peer"].upper(), r["peer"][:8])
            goes = ", ".join(f"message_{k} {v}" for k, v in sorted(r["sent"].items()))
            if r["done"]:
                times.append(r["done"] - r["t"])
                out.append(f"  {r['board']} -> {peer}: complete in {r['done'] - r['t']:.1f} s;"
                           f" sent {goes or 'nothing logged'}")
            else:
                out.append(f"  {r['board']} -> {peer}: {r['end'] or 'not complete'}; sent "
                           f"{goes or 'nothing logged'}")
        for e in responders:
            out.append(f"  {e.board} completed as the responder at {clock(e.t)}")
        for e in refused:
            peer = self.board_of_address.get(e.m.group(1), e.m.group(1)[:8])
            out.append(f"  {e.board} refused {peer} at {clock(e.t)}: not a contact, and not "
                       "accepting")
        if rows:
            out.append(f"  completed {sum(1 for r in rows if r['done'])} of {len(rows)}; time "
                       f"{spread(times, 's')}")

    # --- Sleeping leaves -------------------------------------------------------------------------

    def sleeping(self, ev, out, offs):
        leaves = sorted({e.board for e in ev if e.kind in ("poll_ok", "poll_miss")})
        keepers = [e for e in ev if e.kind == "keeper"]
        if not leaves and not keepers:
            return
        out.append("sleeping leaves")
        for leaf in leaves:
            polls = [e for e in ev if e.board == leaf and e.kind in ("poll_ok", "poll_miss")]
            ok = [e for e in polls if e.kind == "poll_ok"]
            held = sum(int(e.m.group(3)) for e in ok)
            came = sum(int(e.m.group(2)) for e in ev if e.board == leaf and e.kind == "poll_got")
            spans, changes = [], []
            for e in ok:
                k = self.name(e.m.group(2))
                if not spans or spans[-1][0] != k:
                    if spans:
                        changes.append((e.t, spans[-1][0], k))
                    spans.append([k, e.t, e.t])
                spans[-1][2] = e.t
            out.append(f"  {leaf}: {len(polls)} polls, {len(ok)} answered, "
                       f"{len(polls) - len(ok)} not; {held} frames held for it, {came} came")
            out.append("    keepers: " + ", ".join(f"{k} {clock(a)} to {clock(b)}"
                                                   for k, a, b in spans))
            for t, old, new in changes:
                out.append(f"    keeper changed at {clock(t)}: {old} to {new}")
            for off_t, board in offs:
                was = [s for s in spans if s[0] == board and s[1] <= off_t]
                after = [e for e in ok if e.t > off_t and self.name(e.m.group(2)) != board]
                if was and after:
                    missed = sum(1 for e in polls if off_t < e.t < after[0].t and
                                 e.kind == "poll_miss")
                    out.append(f"    after {board} went off at {clock(off_t)}: kept by "
                               f"{self.name(after[0].m.group(2))} {after[0].t - off_t:.0f} s "
                               f"later, {missed} polls unanswered between")
                elif was:
                    out.append(f"    after {board} went off at {clock(off_t)}: no other keeper "
                               "answered in this test")
            radio = [e for e in ev if e.board == leaf and e.kind == "leaf_radio"]
            if radio:
                g = radio[-1].m.groups()
                out.append(f"    radio on {g[1]}.{g[2]}% of {g[3]} s, asleep {g[4]} s in {g[5]} "
                           "sleeps (its last `sleep`)")
        for board in sorted({e.board for e in keepers}):
            g = [e for e in keepers if e.board == board][-1].m.groups()
            out.append(f"  {board}, a keeper: held {g[0]} frames, handed {g[1]} over, dropped "
                       f"{g[2]} for want of room and {g[3]} held too long (its last `sleep`)")

    # --- Airtime -------------------------------------------------------------------------------

    def airtime(self, ev, out, span_s):
        out.append("airtime")
        for board in self.boards:
            mine = [e for e in ev if e.board == board]
            if not mine:
                continue
            parts = []
            fw = [e for e in mine if e.kind == "forwarding"]
            budget = [e for e in mine if e.kind == "budget"]
            window = [e for e in mine if e.kind == "window"]
            if budget:
                g = budget[-1].m.groups()
                parts.append(f"budget left: own {g[0]} of {g[1]} ms" +
                             (f", relayed {g[2]} of {g[3]} ms" if g[2] else ""))
            if window:
                g = window[-1].m.groups()
                parts.append(f"region's window: {g[0]} of {g[1]} ms in {g[2]} s")
            own = sum(float(e.m.group(7) + "." + e.m.group(8)) for e in mine if e.kind == "sent")
            own += sum(float(e.m.group(3) + "." + e.m.group(4)) for e in mine if e.kind == "group")
            fc = [self.air(board, int(e.m.group(2))) for e in mine if e.kind == "fc_sent"]
            own += sum(x for x in fc if x)
            relayed = [self.air(board, int(e.m.group(1))) for e in mine
                       if e.kind in ("relayed", "flooded")]
            n_rel = len(relayed)
            relayed_ms = sum(x for x in relayed if x)
            line = (f"  {board} ({self.role.get(board, '?')}): frames in its log took "
                    f"{own:.0f} ms of its own, {relayed_ms:.0f} ms passing {n_rel} on")
            if span_s > 0:
                line += f", {100 * (own + relayed_ms) / 1000 / span_s:.2f}% of the test"
            out.append(line)
            if parts:
                out.append("    " + "; ".join(parts))
            if len(fw) >= 2:
                a = [int(x) for x in fw[0].m.groups()]
                b = [int(x) for x in fw[-1].m.groups()]
                out.append("    forwarding, first to last `status`: " +
                           ", ".join(f"{n} {y - x:+d}" for n, x, y in zip(FORWARDING, a, b)))
            elif fw:
                out.append("    forwarding: " + ", ".join(
                    f"{n} {x}" for n, x in zip(FORWARDING, fw[-1].m.groups())))
            refused = sum(1 for e in mine if e.kind == "duty_refused")
            if refused:
                out.append(f"    refused by the region's limit: {refused}")

    # --- Recovery ------------------------------------------------------------------------------

    def recovery(self, ev, out, offs):
        if not offs:
            return
        out.append("recovery")
        for off_t, board in offs:
            ons = [e.t for e in ev if e.kind == "mark" and e.m.group(1).split()[:2] == ["on", board]
                   and e.t > off_t]
            out.append(f"  {board} off at {clock(off_t)}" +
                       (f", on again at {clock(ons[0])}" if ons else ""))
            by_src = collections.defaultdict(list)
            for m in getattr(self, "msgs", []):
                by_src[(m["src"], m.get("dest", "?"))].append(m)
            for (src, dest), ms in sorted(by_src.items()):
                if src == board or dest == board:
                    continue
                before = [m for m in ms if m["outcome"] and m["outcome"][0] == "delivered" and
                          m["outcome"][1] <= off_t]
                after = [m for m in ms if m["outcome"] and m["outcome"][0] == "delivered" and
                         m["outcome"][1] > off_t]
                lost = [m for m in ms if m["outcome"] and m["outcome"][0] == "lost" and
                        m["t"] >= off_t - 120 and (not after or m["t"] < after[0]["t"])]
                nexts_before = {g[1] for m in ms if m["t"] < off_t for g in m["goes"]}
                nexts_after = {g[1] for m in ms if m["t"] >= off_t for g in m["goes"]}
                line = f"    {src} -> {dest}: "
                if after:
                    line += (f"first acknowledged again {after[0]['outcome'][1] - off_t:.0f} s "
                             f"after; ")
                else:
                    line += "none acknowledged after; "
                line += f"{len(lost)} not delivered meanwhile"
                if before:
                    line += f"; last acknowledged {off_t - before[-1]['outcome'][1]:.0f} s before"
                line += (f"; sent by {', '.join(sorted(self.name(x) for x in nexts_before)) or '-'}"
                         f" before, {', '.join(sorted(self.name(x) for x in nexts_after)) or '-'}"
                         " after")
                out.append(line)

    # --- The rest ------------------------------------------------------------------------------

    def health(self, ev, out):
        rows = []
        for board in self.boards:
            mine = [e for e in ev if e.board == board]
            lost = sum(1 for e in mine if e.kind == "mark" and e.m.group(1).startswith("lost"))
            boots = sum(1 for e in mine if e.kind == "boot")
            errors = sum(1 for e in mine if e.kind == "radio_error")
            dog = sum(1 for e in mine if e.kind == "watchdog")
            if lost or boots or errors or dog:
                rows.append(f"  {board}: port lost {lost}, starts seen {boots}, radio errors "
                            f"{errors}, watchdog reports {dog}")
        if rows:
            out.append("restarts and errors")
            out.extend(rows)

    def test(self, name, start, end):
        ev = [e for e in self.events if start <= e.t < end]
        out = [f"== {name}: {clock(start)} to {clock(end)} UTC, {(end - start) / 60:.1f} min"]
        offs = [(e.t, e.m.group(1).split()[1]) for e in ev
                if e.kind == "mark" and e.m.group(1).split()[:1] == ["off"] and
                len(e.m.group(1).split()) > 1]
        offs = [o for i, o in enumerate(offs) if i == 0 or o[1] != offs[i - 1][1] or
                o[0] - offs[i - 1][0] > 10]
        self.msgs = []
        self.unicast(ev, out)
        self.contact(ev, out)
        self.sleeping(ev, out, offs)
        self.airtime(ev, out, end - start)
        self.recovery(ev, out, offs)
        self.health(ev, out)
        return out


def summarise(folders):
    events, boards = read_logs(folders)
    if not events:
        return ["no log lines found"]
    s = Summary(events, boards)
    out = ["boards: " + ", ".join(f"{b} ({s.id_of.get(b, 'no id yet: type status')}, "
                                  f"{s.role.get(b, '?')})" for b in boards)]
    for name, start, end in tests_of(events):
        out.append("")
        out.extend(s.test(name, start, end))
    return out


# --- Self-test ---------------------------------------------------------------------------------

def self_test():
    """Five boards in a line, a > b > c > d > e, with a sleeping leaf s kept by c then d: logs as
    the firmware would print them, and the numbers they must give."""
    import tempfile  # noqa: PLC0415

    assert abs(airtime_ms(255, 7, 125000, 1) - 407.808) < 0.001  # draft/airtime.md
    assert abs(airtime_ms(255, 9, 500000, 1) - 320.768) < 0.001
    base = datetime.datetime(2026, 11, 19, 12, 0, 0, tzinfo=datetime.timezone.utc).timestamp()
    ids = {"a": "0000000a", "b": "0000000b", "c": "0000000c", "d": "0000000d", "e": "0000000e",
           "s": "00000005"}
    addr = {k: (v * 8).upper() for k, v in ids.items()}
    logs = collections.defaultdict(list)

    def put(board, t, text):
        logs[board].append((t, text))

    for b in ids:
        put(b, 0, "## test line")
        put(b, 1, f"this board's address: {addr[b]}")
        put(b, 1.1, "radio: EU868, 869525000 Hz, SF7, 125000 Hz, CR 4/5, 14 dBm, sync word 0x5E")
        put(b, 1.2, f"routing: id {ids[b]}, a {'leaf' if b in 'as' else 'relay'}, hears 2, 2 of "
                    "them both ways, routes to 5 of the 1024 it has room for. 'routes' lists them.")
        put(b, 1.3, "forwarding: passed on 0, sent 0 hops again and gave 0 up; dropped 0 with no "
                    "route, 0 with no room; waited 0 times")
    # First contact a -> e across three relays: message_1 twice.
    put("a", 5, f">> contact {addr['e']}")
    put("a", 5.1, "first contact: sent message_1, 56 bytes at 14 dBm by 0000000b")
    put("a", 9.1, "first contact: sent message_1, 56 bytes at 14 dBm by 0000000b")
    put("e", 10, "first contact: heard message_1 at -90 dBm, SNR 5.00 dB")
    put("e", 10.2, "first contact: sent message_2, 60 bytes at 14 dBm by 0000000d")
    put("a", 11, "first contact: heard message_2 at -91 dBm, SNR 4.00 dB")
    put("a", 11.1, "first contact: sent message_3, 80 bytes at 14 dBm by 0000000b")
    put("e", 12, f"first contact: complete. Session started, as the responder, with {addr['a']}. "
                 "It is peer 1 of 1, and where 'send' now goes.")
    put("e", 12.1, "first contact: sent message_4, 24 bytes at 14 dBm by 0000000d")
    put("a", 13.5, f"first contact: complete. Session started, as the initiator, with {addr['e']}"
                   ". It is peer 1 of 1, and where 'send' now goes.")
    # Ten messages a -> e, each 40 s apart; 9 arrive, the ninth after a relay's hop sent again,
    # the tenth not at all; c goes off before the tenth.
    for i in range(10):
        t = 20 + 40 * i
        text = f"line-{i}"
        put("a", t, f">> send {text}")
        put("a", t + 0.2, f'sent #{i} "{text}": 30 bytes at 14 dBm by 0000000b, 80.128 ms on the '
                          "air")
        if i == 9:
            for k in range(4):
                put("a", t + 5 + 5 * k, f'sent #{i} again "{text}": 30 bytes at 14 dBm by '
                                        "0000000b, 80.128 ms on the air")
            put("a", t + 39, f"not delivered #{i}: no acknowledgement, having gone 5 times. Is "
                             "the other board on and in range? 'routes' shows whether there is a "
                             "route to it.")
            continue
        put("b", t + 0.4, "passed on a 30-byte frame for 0000000e by 0000000c, at 14 dBm")
        if i == 8:
            put("c", t + 0.6, "passed on a 30-byte frame for 0000000e by 0000000d, at 14 dBm")
            put("c", t + 4.6, "passed on a 30-byte frame for 0000000e by 0000000d, at 14 dBm")
            dt = 4.0
        else:
            put("c", t + 0.6, "passed on a 30-byte frame for 0000000e by 0000000d, at 14 dBm")
            dt = 0.0
        put("d", t + 0.8 + dt, "passed on a 30-byte frame for 0000000e by 0000000e, at 14 dBm")
        put("e", t + 1.0 + dt, f'heard #{i} "{text}" at -95 dBm, SNR 3.50 dB')
        put("a", t + 2.0 + dt, f"delivered #{i}: acknowledged {int((1.8 + dt) * 1000)} ms after it "
                               "was handed over, having gone 1 time")
    # The sleeping leaf: kept by c, which goes off at 370 s; d answers at 420 s.
    put("s", 30, "telling peer 1 that this board sleeps")
    put("a", 31, f"{addr['s']} sleeps: up to 300 s between wakes. Messages to it wait for it.")
    put("s", 60, "poll 100: answered by 0000000c, 1 frame held")
    put("s", 61, "poll 100: listened, 1 of 1 held frame came")
    put("s", 360, "poll 101: answered by 0000000c, 0 frames held")
    for b in ids:
        put(b, 370, "## off c")
    for k, t in enumerate((380, 390, 400)):
        put("s", t, f"poll {102 + k}: no answer from 0000000c ({k + 1} in a row)")
    put("s", 420, "poll 105: answered by 0000000d, 0 frames held")
    put("s", 430, "radio: asleep now; on 4.1% of the 430 s since it started, asleep 412 s in 9 "
                  "sleeps")
    put("d", 440, "sleep: a relay, which never sleeps. It keeps the sleeping leaves that name it: "
                  "it has room for their frames; held 0 frames for them, handed 0 over, dropped 0 "
                  "for want of room and 0 held too long")
    put("c", 300, "sleep: a relay, which never sleeps. It keeps the sleeping leaves that name it: "
                  "it has room for their frames; held 1 frames for them, handed 1 over, dropped 0 "
                  "for want of room and 0 held too long")
    put("b", 450, "forwarding: passed on 10, sent 0 hops again and gave 0 up; dropped 0 with no "
                  "route, 0 with no room; waited 1 times")
    put("b", 450.1, "airtime budget: 1200 ms of 1200 ms left for its own frames, 8400 ms of "
                    "8400 ms for frames it passes on")
    put("b", 455, "## lost /dev/cu.usbserial-0001: device reports readiness to read but returned "
                  "no data")
    put("b", 456, "## open /dev/cu.usbserial-0001, DTR released, RTS released")
    put("b", 456.5, "ESP-ROM:esp32s3-20210327")
    with tempfile.TemporaryDirectory() as folder:
        for board, lines in logs.items():
            with open(os.path.join(folder, board + ".log"), "w", encoding="utf-8") as f:
                for t, text in sorted(lines, key=lambda x: x[0]):
                    d = datetime.datetime.fromtimestamp(base + t, datetime.timezone.utc)
                    f.write(d.strftime("%Y-%m-%dT%H:%M:%S.") + f"{d.microsecond // 1000:03d}Z "
                            f"{text}\n")
            # A line from another program, and a half-written one: both skipped.
        with open(os.path.join(folder, "a.log"), "a", encoding="utf-8") as f:
            f.write("garbage\n2026-11-19T12:59")
        out = summarise([folder])
    text = "\n".join(out)
    want = [
        "boards: a (0000000a, leaf), b (0000000b, relay)",
        "== line: 12:00:00 to ",
        "  a -> e: 10 sent, 9 acknowledged (90.0%), 9 heard (90.0%), 1 not delivered, 0 still open",
        "    goes from the source: mean 1.40, max 5; acknowledged after p50 1 goes",
        "    one way, first sent to heard: p50 800 ms, p95 4800 ms, max 4800 ms (n=9)",
        "    round trip, handed over to acknowledged: p50 1800 ms, p95 5800 ms, max 5800 ms (n=9)",
        "    path a > b > c > d > e (4 hops): 9",
        "    path a > b, and no further in the logs: 1",
        "    hops sent again by relays with logs: 1",
        "  a -> e: complete in 8.5 s; sent message_1 2, message_3 1",
        "  e completed as the responder at 12:00:12",
        "  completed 1 of 1; time p50 8 s",
        "  s: 6 polls, 3 answered, 3 not; 1 frames held for it, 1 came",
        "    keepers: c 12:01:00 to 12:06:00, d 12:07:00 to 12:07:00",
        "    keeper changed at 12:07:00: c to d",
        "    after c went off at 12:06:10: kept by d 50 s later, 3 polls unanswered between",
        "    radio on 4.1% of 430 s, asleep 412 s in 9 sleeps",
        "  c, a keeper: held 1 frames, handed 1 over",
        "  a (leaf): frames in its log took 1506 ms of its own, 0 ms passing 0 on",
        "  b (relay): frames in its log took 0 ms of its own, 721 ms passing 9 on",
        "    budget left: own 1200 of 1200 ms, relayed 8400 of 8400 ms",
        "    forwarding, first to last `status`: passed on +10, hops sent again +0",
        "  c off at 12:06:10",
        "    a -> e: none acknowledged after; 1 not delivered meanwhile; last acknowledged 24 s "
        "before; sent by b before, b after",
        "  b: port lost 1, starts seen 1, radio errors 0, watchdog reports 0",
    ]
    missing = [w for w in want if w not in text]
    if missing:
        print(text)
        print("\nmissing:\n" + "\n".join(missing))
        sys.exit(1)
    print("fieldsummary: self-test passed")


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("folders", nargs="*", help="folders of <board>.log, as fieldlog.py writes them")
    ap.add_argument("--self-test", action="store_true",
                    help="check this script against logs it writes")
    args = ap.parse_args()
    if args.self_test:
        self_test()
        return
    if not args.folders:
        ap.error("give the folder fieldlog.py wrote to")
    print("\n".join(summarise(args.folders)))


if __name__ == "__main__":
    main()

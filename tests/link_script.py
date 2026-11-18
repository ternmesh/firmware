#!/usr/bin/env python3
"""Runs tools/companion.py against tests/link_pty.c's node, as it would run against a board.

    python3 tests/link_script.py BUILD/test_link_pty tools/companion.py

The node is the port's own link.c on a pseudo-terminal, with console text in between its frames,
so this checks the script, the framing and the link together, everything but the radio and USB.
"""

import json
import os
import subprocess
import tempfile
import sys
import time

VECTORS = os.path.join(os.path.dirname(os.path.abspath(__file__)), "vectors", "companion.json")
BOB = "3d4017c3e843895a92b70aa74d1b7ebc9c982ccf2ec4968cc0cd55f12af4660c"
GONE = "the node took this client for gone"
# The specification's join code for a group called Ridge walkers (vectors/groups.json).
RIDGE = "HTTPS://TERNMESH.ORG/G#YTCMJRGEYTCMJRGEYTCMJRGEYRS2WUTJMRTWKIDXMFWGWZLSOM"


def main():
    node_bin, script = sys.argv[1], sys.argv[2]
    failed = 0

    def run(port, *args, expect, absent=(), meanwhile=None, code=0):
        """Runs the script; what it prints must hold `expect` in order, and nothing in `absent`,
        and it must exit with `code`; one that fails is read with what it says on stderr.
        `meanwhile`, if given, is called while it runs."""
        nonlocal failed
        p = subprocess.Popen([sys.executable, script, "--port", port, *args],
                             stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        if meanwhile:
            meanwhile()
        out, err = p.communicate(timeout=60)
        r = subprocess.CompletedProcess(p.args, p.returncode, out, err)
        said = r.stdout + (r.stderr if code else "")
        at, missing = 0, []
        for e in expect:
            found = said.find(e, at)
            if found < 0:
                missing.append(e)
            else:
                at = found + len(e)
        there = [a for a in absent if a in said]
        if r.returncode != code or missing or there:
            failed += 1
            print(f"FAIL {' '.join(args)}: missing {missing}, not wanted {there}\n"
                  f"{r.stdout}{r.stderr}")
        else:
            print(f"ok   {' '.join(args)}")

    def node(*args):
        n = subprocess.Popen([node_bin, *args], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                             text=True)
        return n, n.stdout.readline().strip()

    board, port = node()
    try:
        run(port, "state", expect=["tern host test", "a relay in EU868, at 14 dBm",
                                   "neighbour 1d2e3f40", "air: 1234 of 360000 ms used in 3600 s"])
        run(port, "contact", BOB, "Bob", expect=["saved"])
        # An image with no signature is not sent, unless asked; asked, the node refuses it.
        image = tempfile.NamedTemporaryFile(suffix=".bin", delete=False)
        image.write(bytes(i * 7 & 0xFF for i in range(5000)))
        image.close()
        run(port, "update", image.name, expect=["has no signature beside it"], code=1)
        run(port, "update", image.name, "--unsigned",
            expect=["sending an image with no signature", "UPDATE_BEGIN: not an image this node "
                    "runs, or not signed by a release key it holds"], code=1)
        os.unlink(image.name)
        # The specification's image, signed with its test key, which this node holds.
        v = json.load(open(VECTORS, encoding="utf-8"))
        with tempfile.TemporaryDirectory() as d:
            signed = os.path.join(d, "image.bin")
            open(signed, "wb").write(bytes.fromhex(v["image"]))
            open(signed + ".sig", "wb").write(bytes.fromhex(v["image_signature"]))
            run(port, "update", signed, "--test-key",
                expect=["updating host from 0.2.0: 400 bytes", "100%",
                        "the node has the image, and restarts into it"])
        run(port, "send", BOB, "On the ridge by six", "--wait", "2",
            expect=["queued as message #1",
                    "to 3d4017c3e843895a: 'On the ridge by six' (waiting for the radio)",
                    "message #1: waiting"])
        run(port, "state",
            expect=["contact 'Bob'", "message #1 to Bob: 'On the ridge by six' (waiting)"])
        run(port, "set", "power", "10", expect=["set"])
        # A group joined from its code, given lower-case, and the code asked for again.
        run(port, "group", "join", RIDGE.lower(), expect=["joined group c8eafadc0857a696"])
        run(port, "group", "code", "c8eafadc0857a696", expect=[RIDGE])
        run(port, "state", expect=["group 'Ridge walkers'"])
        run(port, "wipe", expect=["wipe erases the node's address"], code=1)
        run(port, "wipe", "--yes", expect=["wiped: the node starts again as new"])
        run(port, "off", expect=["turned off: press its button to turn it on again"])
    finally:
        board.stdin.close()
        board.wait(timeout=10)

    # A node that takes a client for gone after a second and a half: one that pings more often
    # than that keeps its connection, and one that pings less often is cut off and starts again.
    board, port = node("1500")
    try:
        run(port, "--idle", "0.5", "watch", "--seconds", "4",
            expect=["this node:", "neighbour 1d2e3f40"], absent=[GONE])
        run(port, "--idle", "2", "watch", "--seconds", "6",
            expect=["this node:", GONE, "this node:", "neighbour 1d2e3f40"])
    finally:
        board.stdin.close()
        board.wait(timeout=10)

    # A node that restarts answers ERROR 6 too, with its ids begun again: the message it now
    # calls #1 is not the one the client heard of as #1, received before the restart, and is
    # news a sync after #1 would miss.
    board, port = node()

    def tell(what, after=0.0):
        time.sleep(after)
        board.stdin.write(what)
        board.stdin.flush()

    try:
        tell("m")
        run(port, "--idle", "1", "watch", "--seconds", "4", meanwhile=lambda: tell("r", 1.5),
            expect=[GONE, "message #1 from 3d4017c3e843895a: 'Back after a restart' (received)"],
            absent=["Morning"])
    finally:
        board.stdin.close()
        board.wait(timeout=10)

    # A restart while a send is watched: the node's new #1 is not the message sent as #1.
    board, port = node()
    try:
        run(port, "--idle", "1", "send", BOB, "On the ridge by six", "--wait", "4",
            meanwhile=lambda: tell("r", 1.5),
            expect=["queued as message #1", GONE,
                    "message #1: the node restarted, and no longer holds it"],
            absent=["Back after a restart"])
    finally:
        board.stdin.close()
        board.wait(timeout=10)
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())

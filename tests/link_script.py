#!/usr/bin/env python3
"""Runs tools/companion.py against tests/link_pty.c's node, as it would run against a board.

    python3 tests/link_script.py BUILD/test_link_pty tools/companion.py

The node is the port's own link.c on a pseudo-terminal, with console text in between its frames,
so this checks the script, the framing and the link together, everything but the radio and USB.
"""

import subprocess
import sys

BOB = "3d4017c3e843895a92b70aa74d1b7ebc9c982ccf2ec4968cc0cd55f12af4660c"


def main():
    node_bin, script = sys.argv[1], sys.argv[2]
    node = subprocess.Popen([node_bin], stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
    port = node.stdout.readline().strip()
    failed = 0

    def run(*args, expect):
        nonlocal failed
        r = subprocess.run([sys.executable, script, "--port", port, *args], capture_output=True,
                           text=True, timeout=60)
        missing = [e for e in expect if e not in r.stdout]
        if r.returncode != 0 or missing:
            failed += 1
            print(f"FAIL {' '.join(args)}: missing {missing}\n{r.stdout}{r.stderr}")
        else:
            print(f"ok   {' '.join(args)}")

    try:
        run("state", expect=["tern host test", "a relay in EU868, at 14 dBm",
                             "neighbour 1d2e3f40", "air: 1234 of 360000 ms used in 3600 s"])
        run("contact", BOB, "Bob", expect=["saved"])
        run("send", BOB, "On the ridge by six", "--wait", "2",
            expect=["queued as message #1",
                    "to 3d4017c3e843895a: 'On the ridge by six' (waiting for the radio)",
                    "message #1: waiting"])
        run("state", expect=["contact 'Bob'", "message #1 to Bob: 'On the ridge by six' (waiting)"])
        run("set", "power", "10", expect=["set"])
    finally:
        node.stdin.close()
        node.wait(timeout=10)
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())

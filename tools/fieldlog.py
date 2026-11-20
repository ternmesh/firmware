#!/usr/bin/env python3
"""Logs the consoles of several Tern boards at once, for a field test (docs/field-test.md).

    python3 tools/fieldlog.py --board a=/dev/cu.usbserial-0001 --board e=/dev/cu.usbmodem114101 \\
        --dir logs/

Each board gets a log, <dir>/<name>.log, one line for each line its console prints, after the
time it began to arrive: the computer's clock in UTC, to the millisecond. Each board also gets a
FIFO, <dir>/<name>.in: a line written to it is typed on that board's console, and logged as
`>> <line>`. <dir>/all.in types a line on every board. A line that begins `## ` is not typed: it
is written to the logs as it is, as a mark for tools/fieldsummary.py, such as `## test line-5` or
`## off c`. So, from another terminal:

    echo status > logs/a.in
    echo '## test line-5' > logs/all.in
    for i in $(seq 1 40); do echo "send line5-$i" > logs/a.in; sleep 30; done

A port that goes away, because the board was reset or unplugged, is logged as `## lost`, and
opened again once it is back, logged as `## open`. Lines typed while it is away are logged as
`## not typed` and dropped.

Opening a port can reset an ESP32: on boards with a CP2102 or CH9102, RTS drives the chip's reset
and DTR its boot pin. Such a port is opened with both released, which does not reset it on Linux;
on macOS the driver may still pulse them as the port opens, so the first line of a log may follow
a restart. An nRF52840 board's console is USB CDC, and the board writes to it only while DTR is
raised, so that is raised. Which a port is, is told from its USB vendor; `,dtr=0` or `,dtr=1`
after the port says it instead. RTS is always released. On macOS use the /dev/cu.* name, not
/dev/tty.*.

Needs pyserial (pip install pyserial). `--self-test` checks the logger against a pseudo-terminal,
touching no board.
"""

import argparse
import datetime
import errno
import os
import select
import sys
import threading
import time

# USB vendors whose serial chips put RTS on an ESP32's reset and DTR on its boot pin, and
# Espressif's own USB Serial/JTAG, which resets on RTS: their ports are opened with both released.
UART_VENDORS = {0x10C4, 0x1A86, 0x0403, 0x303A}


def stamp(t=None):
    """A time as the logs write it: UTC, to the millisecond."""
    t = time.time() if t is None else t
    d = datetime.datetime.fromtimestamp(t, datetime.timezone.utc)
    return d.strftime("%Y-%m-%dT%H:%M:%S.") + f"{d.microsecond // 1000:03d}Z"


def printable(raw):
    """A console line as text: \\r dropped, and other control bytes, such as a companion frame's,
    shown as U+FFFD."""
    text = raw.replace(b"\r", b"").decode("utf-8", errors="replace")
    return "".join(c if c >= " " or c == "\t" else "�" for c in text)


def wants_dtr(port, given):
    """Whether to raise DTR on this port: as given, or from its USB vendor."""
    if given is not None:
        return given
    try:
        from serial.tools import list_ports  # noqa: PLC0415

        for p in list_ports.comports():
            if p.device == port and p.vid is not None:
                return p.vid not in UART_VENDORS
    except Exception:  # noqa: BLE001 - a port listing that fails says nothing either way
        pass
    # Not found: a usbserial or ttyUSB name is a UART bridge, anything else most likely CDC.
    return not ("usbserial" in port or "ttyUSB" in port or "SLAB" in port or "wchusb" in port)


class Board:
    """One board's port, log and FIFO. A thread reads the port and writes the log; type() is
    called from the main thread."""

    def __init__(self, name, port, dtr, folder, baud, echo):
        self.name, self.port, self.dtr_given, self.baud, self.echo = name, port, dtr, baud, echo
        self.log = open(os.path.join(folder, name + ".log"), "a", encoding="utf-8", buffering=1)
        self.fifo = os.path.join(folder, name + ".in")
        self.s = None
        self.lock = threading.Lock()
        self.stop = threading.Event()
        self.thread = threading.Thread(target=self.run, name=name, daemon=True)

    def write(self, text, t=None):
        with self.lock:
            self.log.write(f"{stamp(t)} {text}\n")
        if self.echo:
            print(f"{self.name}: {text}", flush=True)

    def open(self):
        import serial  # noqa: PLC0415

        s = serial.Serial()
        s.port, s.baudrate, s.timeout = self.port, self.baud, 0.2
        dtr = wants_dtr(self.port, self.dtr_given)
        # Set before the port opens, so pyserial applies them as it does.
        s.dtr, s.rts = dtr, False
        if os.name == "posix":
            s.exclusive = True
        s.open()
        self.s = s
        self.write(f"## open {self.port}, DTR {'raised' if dtr else 'released'}, RTS released")

    def close(self, why):
        if self.s is not None:
            try:
                self.s.close()
            except Exception:  # noqa: BLE001 - it is gone already
                pass
            self.s = None
            self.write(f"## lost {self.port}: {why}")

    def run(self):
        buf, began, last = b"", None, 0.0
        while not self.stop.is_set():
            if self.s is None:
                if not os.path.exists(self.port) and os.name == "posix":
                    time.sleep(0.5)
                    continue
                try:
                    self.open()
                except Exception as e:  # noqa: BLE001 - tried again in a second
                    if time.time() - last > 60:
                        self.write(f"## cannot open {self.port}: {e}")
                        last = time.time()
                    time.sleep(1)
                    continue
            try:
                data = self.s.read(self.s.in_waiting or 1)
            except Exception as e:  # noqa: BLE001 - unplugged, reset, or the driver gave up
                self.close(e)
                time.sleep(0.5)
                continue
            now = time.time()
            if data and began is None:
                began = now
            buf += data
            while b"\n" in buf:
                line, buf = buf.split(b"\n", 1)
                self.write(printable(line), began)
                began = now if buf else None
            # A line with no end yet, such as a command being typed, is written after a second.
            if buf and began is not None and now - began > 1.0:
                self.write(printable(buf), began)
                buf, began = b"", None
        self.close("logger stopped")

    def type(self, line):
        if line.startswith("## "):
            self.write(line)
            return
        s = self.s
        if s is None:
            self.write(f"## not typed, the port is closed: {line}")
            return
        try:
            s.write(line.encode("utf-8") + b"\n")
            s.flush()
            self.write(f">> {line}")
        except Exception as e:  # noqa: BLE001 - the reader notices and reopens
            self.write(f"## not typed ({e}): {line}")


def make_fifo(path):
    try:
        os.mkfifo(path)
    except FileExistsError:
        pass
    # Held open for writing too, so that the FIFO never reads as ended between writers.
    rd = os.open(path, os.O_RDONLY | os.O_NONBLOCK)
    wr = os.open(path, os.O_WRONLY)
    return rd, wr


def serve(boards, folder, stop):
    """Reads the FIFOs and types their lines, until stop is set."""
    fifos = {}
    for b in boards:
        fifos[make_fifo(b.fifo)[0]] = [b]
    fifos[make_fifo(os.path.join(folder, "all.in"))[0]] = boards
    pending = {fd: b"" for fd in fifos}
    while not stop.is_set():
        ready, _, _ = select.select(list(fifos), [], [], 0.2)
        for fd in ready:
            try:
                data = os.read(fd, 4096)
            except OSError as e:
                if e.errno == errno.EAGAIN:
                    continue
                raise
            pending[fd] += data
            while b"\n" in pending[fd]:
                line, pending[fd] = pending[fd].split(b"\n", 1)
                text = line.decode("utf-8", errors="replace").rstrip("\r")
                if text.strip():
                    for b in fifos[fd]:
                        b.type(text)


def parse_board(text):
    """name=port, or name=port,dtr=0|1."""
    name, sep, rest = text.partition("=")
    if not sep or not name or not rest or "/" in name:
        raise argparse.ArgumentTypeError(f"{text!r}: expected name=port")
    port, dtr = rest, None
    for flag in ("dtr=0", "dtr=1"):
        if rest.endswith("," + flag):
            port, dtr = rest[: -len(flag) - 1], flag.endswith("1")
    if name == "all":
        raise argparse.ArgumentTypeError("'all' is the name of the FIFO for every board")
    return name, port, dtr


def self_test():
    """A pseudo-terminal stands for a board: what it prints is logged with a time, a line written
    to the FIFO reaches it, a mark is logged and not typed, and its going is logged."""
    import tempfile  # noqa: PLC0415

    import serial  # noqa: F401, PLC0415 - fails here, clearly, without pyserial

    master, slave = os.openpty()
    with tempfile.TemporaryDirectory() as folder:
        b = Board("p", os.ttyname(slave), False, folder, 115200, False)
        stop = threading.Event()
        b.thread.start()
        server = threading.Thread(target=serve, args=([b], folder, stop), daemon=True)
        server.start()
        deadline = time.time() + 5
        while b.s is None and time.time() < deadline:
            time.sleep(0.05)
        assert b.s is not None, "the pseudo-terminal did not open"
        os.write(master, b'heard #1 "x" at -80 dBm, SNR 5.25 dB\r\n')
        time.sleep(0.3)
        with open(os.path.join(folder, "p.in"), "w") as f:
            f.write("status\n## test one\n")
        with open(os.path.join(folder, "all.in"), "w") as f:
            f.write("routes\n")
        got, deadline = b"", time.time() + 5
        while got.count(b"\n") < 2 and time.time() < deadline:
            r, _, _ = select.select([master], [], [], 0.2)
            if r:
                got += os.read(master, 1024)
        got = got.replace(b"\r", b"")
        assert got == b"status\nroutes\n", got
        b.stop.set()
        stop.set()
        b.thread.join(2)
        server.join(2)
        os.close(master)
        os.close(slave)
        with open(os.path.join(folder, "p.log"), encoding="utf-8") as f:
            log = f.read().splitlines()
    body = [line.split(" ", 1)[1] for line in log]
    assert body[0].startswith("## open "), body
    assert body[1] == 'heard #1 "x" at -80 dBm, SNR 5.25 dB', body
    assert ">> status" in body and "## test one" in body and ">> routes" in body, body
    assert body[-1].startswith("## lost "), body
    assert all(len(line.split(" ", 1)[0]) == 24 for line in log), log
    print("fieldlog: self-test passed")


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--board", action="append", type=parse_board, default=[],
                    help="name=port, or name=port,dtr=0|1; once for each board")
    ap.add_argument("--dir", default="logs", help="where the logs and FIFOs go (default logs)")
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--quiet", action="store_true", help="do not print the lines here as well")
    ap.add_argument("--self-test", action="store_true", help="check the logger on a pty")
    args = ap.parse_args()
    if args.self_test:
        self_test()
        return
    if not args.board:
        ap.error("no boards: --board name=port, once for each")
    names = [n for n, _, _ in args.board]
    if len(set(names)) != len(names):
        ap.error("two boards have one name")
    try:
        import serial  # noqa: F401, PLC0415
    except ImportError:
        sys.exit("fieldlog needs pyserial: pip install pyserial")
    os.makedirs(args.dir, exist_ok=True)
    boards = [Board(n, p, d, args.dir, args.baud, not args.quiet) for n, p, d in args.board]
    stop = threading.Event()
    for b in boards:
        b.write("## logger started")
        b.thread.start()
    print(f"logging {', '.join(names)} to {args.dir}/; type into {args.dir}/<name>.in or "
          f"{args.dir}/all.in. Ctrl-C stops.", file=sys.stderr)
    try:
        serve(boards, args.dir, stop)
    except KeyboardInterrupt:
        pass
    stop.set()
    for b in boards:
        b.stop.set()
    for b in boards:
        b.thread.join(2)


if __name__ == "__main__":
    main()

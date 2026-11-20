# The field test

Tern's claim against Meshtastic and MeshCore is that routed unicast and an airtime budget keep a
mesh from the collapse that flooding brings. On radios, nothing has yet crossed more than one
relay, and no first-contact handshake has crossed one at all (the specification's
`draft/first-contact.md`, "Not yet specified"; `draft/forwarding.md`, "Not yet measured"). This is
the test that changes that, written so that it can run as soon as there are boards for it.

Every command below is one the console takes today (`command()` in
[`ports/node/node.c`](../ports/node/node.c)), and every line it says to look for is one the
firmware prints today. What the test needs and the firmware does not yet print is under
[Firmware gaps](#firmware-gaps).

## What it proves

| Test | What it shows | The numbers |
|---|---|---|
| [T0](#t0-on-the-bench) | Every board starts, and every link the test relies on is there, and those it must not are not | `selftest` times; test frames heard |
| [T1](#t1-a-line) | A message crosses 3, 4 and 5 hops by its route, and is acknowledged | delivery rate, one-way latency, round trip, tries, hops sent again, the path |
| [T2](#t2-first-contact-across-relays) | A handshake crosses relays, its last hops sent once | time to complete, tries of each frame |
| [T3](#t3-a-relay-switched-off) | A route around a relay that has gone, where there is one | time to the next delivery, messages lost |
| [T4](#t4-a-cluster) | Several pairs at once, through shared relays | delivery and latency for each pair, airtime of each node |
| [T5](#t5-a-sleeping-leaf) | A sleeping leaf is reached through its keeper, and finds another when its keeper goes | delivery, latency, polls, keeper handover time |
| [T6](#t6-a-node-that-sends-too-much) | The budget holds back a node that sends too much, and the others' messages still go | the node's airtime, the others' delivery |

## Boards and places

The line needs one board more than its hops: six for five hops. The cluster needs five to eight,
and reuses the line's boards.

```
 The line                  A ─── R1 ─── R2 ─── R3 ─── R4 ─── E        (3 hops: A R1 R2 E)

 The cluster                    L1     L2
                                 \     /
                          L3 ─── C1 ─── C2 ─── L4        C1 C2 C3 relays, L1..L4 leaves,
                                   \   /                 S a sleeping leaf
                                    C3 ─── S
```

* **Roles.** A build is a relay unless built otherwise (`CONFIG_TERN_RELAY`,
  `ports/node/Kconfig.tern`). A leaf is made with the companion protocol, before the logger opens
  the port: `python3 tools/companion.py --port <port> set role 0`. The board restarts into it.
  `status` says which it is: `routing: id 1a2b3c4d, a leaf, ...`. A and E are leaves in the line,
  so that nothing passes through them.
* **Distance.** Each board must hear its neighbours in the drawing and no other. Nothing on the
  console narrows what a board hears ([gaps](#firmware-gaps), 11), so this is distance or
  obstacles. On flat ground with no obstacles the simulator has neighbours only at 2 km apart at
  14 dBm on EU868 and 3.5 km at 20 dBm on US915 (below); in town, far less. Check it in
  [T0](#t0-on-the-bench), then with `routes` before each test.
* **Power.** The same on every board, set with `python3 tools/companion.py --port <port> set
  power <dBm>` before logging. `power` on the console is for the bench only.
* **Firmware.** One build on every board. Note its commit.
* **Logging.** `tools/fieldlog.py` on whatever is beside a board: a laptop, or a Raspberry Pi on a
  power bank. The two ends of each pair must be logged; relays need not be, but those that are
  give the path and each hop sent again. A board without a logger is read afterwards: plug it in,
  log it, and type `status`, `routes` and `sleep`. Its counters run from its last start.
* **Clocks.** Each logger writes its computer's clock, in UTC, to the millisecond. The one-way
  latency compares two logs, so their clocks must agree: NTP on each, checked before and after.
  The round trip needs the source's clock alone, and is the number to trust.

## Running it

On each logging computer, with pyserial installed:

```bash
python3 tools/fieldlog.py --board a=/dev/cu.usbserial-0001 --board r1=/dev/cu.usbmodem114101 \
    --dir logs/
```

Then, from another terminal there, type on a board by writing to its FIFO, and on all of them
through `all.in`. A line that starts `## ` is a mark in the logs, not typed:

```bash
echo status > logs/a.in
echo '## test t1-4hops' > logs/all.in      # starts a test in every log here
echo '## off r2' > logs/all.in             # R2 was switched off now
echo '## end' > logs/all.in
```

A test starts at its `## test <name>` and runs to the next, or to `## end`. Mark a test at the
same moment on every logging computer: the summary takes one mark within ten seconds of another,
of the same name, as the same.

Each message is matched by its text, so each needs its own. 32 bytes, as the simulator's
messages are below:

```bash
for i in $(seq -w 1 40); do
    echo "send $(printf '%-32s' "t1-4h-$i" | tr ' ' .)" > logs/a.in; sleep 30
done
```

Do not start a text with `ping`: a board answers those (`heard_into()`), which adds traffic.

Afterwards, with every logging computer's folder copied to one place:

```bash
python3 tools/fieldsummary.py logs-base/ logs-far/
```

It prints, for each test, the numbers below. What each is taken from is in its docstring.

## T0: on the bench

**Shows:** every board starts, and does the arithmetic of a handshake, and the links are as drawn.

1. All boards on one table. On each: `status`. Look for `radio: <region>, ... sync word 0x5E`
   with no `(not the region's settings)`, the role meant, and the same region on every board.
   Note each board's address and routing id; the summary takes them from these lines.
2. On each: `selftest`. It runs a handshake between two nodes in the board's memory, nothing on
   the air. Pass: `selftest: passed, in <n> ms; ...`. Record each step's time from
   `selftest: respond to message_1 <n> ms, ...` and the three after it: this is what each board
   adds to a handshake ([T2](#t2-first-contact-across-relays)).
3. At the sites, for each pair that must hear each other and each that must not: on one,
   `bench on`, `counts reset`; on the other, `bench on`, `beacon 20 500`. When it prints
   `beacon: done, 20 sent`, type `counts` on the first: `counts: ..., test frames <n>, ...,
   mean <rssi> dBm, SNR <snr> cB`. Then `bench off` on both. Pass: 18 or more of 20 between
   neighbours, none between nodes two apart. Record RSSI and SNR for each link.
4. Leave every board running ten minutes, then type `routes` on each. Pass: under `neighbours:`,
   only its neighbours in the drawing, each `up`; under `routes:`, a line for each other board,
   `<its id>  by <the neighbour toward it>  <n> ms on the air`. A route by any other neighbour
   means the topology is not the one drawn: move a board, and look again.

## T1: a line

**Shows:** routed unicast across 3, 4 and 5 hops, each hop and the whole way acknowledged.

Run it three times, at 3, 4 and 5 hops: E moves to the end of R2, R3, then R4.

1. On E: `accept`. It prints `for 120 s, a board that is not yet one of this one's peers may make
   contact`. On A: `contact <E's address>`. Wait for A's `first contact: complete. Session
   started, as the initiator, with <E's address>.` (this is [T2](#t2-first-contact-across-relays)
   too). A's `send` now goes to E; `peers` says so.
2. `## test t1-<n>hops` into `all.in`, then `status` on every logged board.
3. Forty messages from A, one every 30 s, as above. Twenty minutes.
4. `status`, then `routes`, on every logged board, then `## end`.

The lines that matter, in order:

| Board | Line | Means |
|---|---|---|
| A | `sent #17 "t1-4h-17....": 55 bytes at 14 dBm by 0000000b, 115.968 ms on the air` | message 17 went, to R1 (`by` is the next hop's routing id). Again with `again` for each try from the source |
| R1..R4 | `passed on a 55-byte frame for 0000000e by 0000000c, at 14 dBm` | a relay passed it on: for E, by R2. Each time it goes, so two such lines for one message is a hop sent again |
| E | `heard #17 "t1-4h-17...." at -101 dBm, SNR 4.25 dB` | it arrived |
| E | `heard #17 again: its acknowledgement did not get back, and is sent again` | a copy: the acknowledgement was lost on the way back |
| A | `delivered #17: acknowledged 1734 ms after it was handed over, having gone 1 time` | the round trip, and how many times it went from A |
| A | `not delivered #17: no acknowledgement, having gone 4 times. ...` | lost |
| every relay | `forwarding: passed on 120, sent 3 hops again and gave 0 up; dropped 0 with no route, 0 with no room; waited 2 times for a frame being received` | from `status`: the hops sent again and given up, over the test |

**Record:** for A to E, messages sent, acknowledged and heard; the one-way latency (sent to heard)
and round trip (`delivered` after); goes from the source; each relay's hops sent again and given
up; the path each message took; and each node's airtime (below). `fieldsummary.py` prints them
all.

**Pass:** at least 95% acknowledged at each length, and every message's path the line's. The
simulator's figures, on a channel with no fading, are the comparison: a field result far from
them is worth a look at what the logs say about the hops.

**What the simulator predicts.** `scenarios/line.tsim` in ternmesh/sim has these nodes in a row
with no shadowing, but floods. Under the firmware's own core (`routing = core`, `tsim/core.h`,
built against this repository at 23bf446 and ternmesh/sim at 44984af), with A sending a 32-byte
message to the far end once a minute for an hour:

| Hops | EU868 profile, SF7 at 125 kHz, 14 dBm, 2 km apart | US915 profile, SF9 at 500 kHz, 20 dBm, 3.5 km apart |
|---|---|---|
| 3 | 100%, one way p50 0.55 s, p95 0.70 s | 100%, p50 0.48 s, p95 0.60 s |
| 4 | 100%, p50 0.76 s, p95 0.98 s | 100%, p50 0.67 s, p95 0.82 s |
| 5 | 100%, p50 1.02 s, p95 1.24 s | 100%, p50 0.85 s, p95 1.02 s |

The busiest node spent 0.33% of its time on the air on EU868 and 0.28% on US915, announces
included; no hop was sent again. The simulator reports one-way latency; the acknowledgement comes
back over as many hops in a shorter frame, so A's round trip should be a little under twice these.
With shadowing (`channel.sigma = 8`), three seeds of five nodes gave lines of 1, 3 and 4 hops:
links the drawing does not have appear, which is why T0 checks them. The scenario, a copy of
`line.tsim` changed for it:

```
nodes = 6
placement = line
spacing = 2000
channel.sigma = 0
radio.sf = 7
radio.bw = 125000
radio.preamble = 16
radio.tx_dbm = 14
routing = core
mac = listen
traffic.interval = none
warmup = 30 min
duration = 1 h
deadline = 60 s
```

run as `tsim -s nodes=5 -s "traffic.send=1860 s, 0, 4, 32" ... line-core.tsim`, one
`traffic.send` a minute; `-s radio.sf=9 -s radio.bw=500000 -s radio.tx_dbm=20 -s spacing=3500`
for US915.

## T2: first contact across relays

**Shows:** a handshake of four frames crosses relays, message_2 and message_4 coming back, the
last hop of each sent once.

At each length of [T1](#t1-a-line), before its messages, and then five times more at five hops:

1. On A: `drop 1` if it has a session with E. On E: `drop <A's number in 'peers'>`, then
   `accept`.
2. On A: `contact <E's address>`.
3. Wait for A's `first contact: complete`, or for `first contact: no answer to message_1 after
   <n> tries; given up.`

| Board | Line |
|---|---|
| A | `first contact: sent message_1, 56 bytes at 14 dBm by 0000000b` (and message_3, 80 bytes); once for each go |
| relays | `passed on a 56-byte frame for 0000000e by ...` (60 bytes for message_2, 80 for message_3, 24 for message_4) |
| E | `first contact: heard message_1 at <rssi> dBm, SNR <snr> dB`, then `first contact: sent message_2, 60 bytes ...` |
| E | `first contact: complete. Session started, as the responder, with <A's address>. ...` |
| A | `first contact: complete. Session started, as the initiator, with <E's address>. ...` |
| E, failing | `first contact: refused <address>, which is not one of this board's peers or contacts. ...`: `accept` was not typed, or ran out |

**Record:** the time from `contact` to A's `complete`; how often each message went; failures and
at which message.

**Pass:** all complete, at every length.

**What to expect.** Nothing has simulated a handshake across a relay (`draft/forwarding.md`, "Not
yet measured"). A rough figure: four crossings of the line, each about as long as T1's one-way
latency for frames of 56 to 80 bytes, plus each step's arithmetic from T0's `selftest`. At four
hops on EU868, about 4 × 0.8 s plus the arithmetic. A frame lost on its last hop is not sent
again by the last relay, so it costs a try from its source: count those as `sent message_1`
(or message_3) more than once.

## T3: a relay switched off

**Shows:** what a route does when a relay goes. On the line there is no other way, and messages
must fail, cleanly, and go again once it is back. In the cluster there is, and the next message
should take it.

1. The line at four hops, A sending once every 30 s as in T1. `## test t3-line`.
2. After ten messages, switch R2 off and mark it: `## off r2`.
3. Ten minutes later, switch it on and mark it: `## on r2`. Ten more messages. `status` and
   `routes` on every logged board. `## end`.
4. The same in [the cluster](#t4-a-cluster), L1 sending to L4 with C1, C2 and C3 all relays:
   switch off whichever relay `routes` on L1 shows its route to L4 going `by`, and mark it.

| Board | Line | Means |
|---|---|---|
| A | `sent #23 again "..." ... by 0000000b` | tried again from the source |
| R1 | `passed on a 55-byte frame for 0000000e by 0000000c` twice or three times, then nothing | the hop to R2 sent again, then given up (`HOP_RETRIES` 2) |
| R1 | a later `passed on ... by <another id>` | the frame went another way (`SALVAGE`); in the cluster only |
| A | `not delivered #23: no acknowledgement, having gone 4 times` | lost |
| R1 | `forwarding: ... sent <n> hops again and gave <m> up ...` | from `status` |

**Record:** for each source, the time from `## off` to its next acknowledged message; how many
were not delivered in between; the next hops before and after. After `## on`, the time to the next
acknowledged message. Run `routes` on R1 every few minutes while R2 is off: the route by R2 stays
until R1 has given 24 hops in a row up to it (`DEAD_HOPS`), or hears another route.

**Pass:** in the cluster, messages after the first lost one go another way and are acknowledged.
On the line, nothing is delivered while R2 is off, and delivery comes back within two messages of
it starting again.

**What to expect.** No simulator scenario switches one chosen relay off: `churn` takes random
nodes down for random times (ternmesh/sim `docs/scenarios.md`). The specification's parameters
bound it: a hop is given up after three goes, 4 s plus two airtimes apart (`HOP_WAIT`), and
tried another way once (`SALVAGE`); A tries a message four times (`RETRIES` 3).

## T4: a cluster

**Shows:** routed unicast through shared relays, several pairs at once, and what each node spends
on the air.

1. Lay out the cluster. T0's steps 3 and 4 for its links.
2. Sessions: L1 with L4, L2 with L3, L4 with S (S awake for now: `sleep off` on it). `accept` on
   one, `contact` on the other, as in T1. A leaf with two sessions sends to the one `to <n>`
   chose; `peers` lists them.
3. `## test t4-cluster`, `status` on every board.
4. For half an hour: L1 to L4 every 30 s, L2 to L3 every 45 s, L4 to L1 every 60 s, all at once.
5. `status` and `routes` on every board; `## end`.

**Record:** as T1, for each pair; and for each node, its airtime.

**Airtime.** `status` prints, for each board:

```
airtime budget: 11920 ms of 12000 ms left for its own frames, 4200 ms of 4200 ms for frames it passes on
transmitted: 1834 ms of the 360000 ms allowed in any 3600 s
```

The budget's two parts hold 2% of 600 s and 7% of 60 s (`tern_airtime_defaults()`); a leaf has no
relayed part. `transmitted` is the region's own limit, printed only where there is one, as on
EU868: it is every frame the board sent in the last hour, announces and acknowledgements too, and
the best figure there is of a node's airtime. Elsewhere `fieldsummary.py` adds up the frames the
log shows, by the LoRa time-on-air formula, which leaves out announces, acknowledgements and
polls ([gaps](#firmware-gaps), 1 and 4). The screen's bench pages (`screen bench on`) show every
frame's total, for a board that has a screen.

**Pass:** at least 95% delivered for each pair, and no board's `transmitted` above 1% of the hour.

**What the simulator predicts.** The nearest is `scenarios/core/town.tsim`: 200 relays under the
firmware's core, where the busiest node, once settled, spends 0.42 to 0.64% of its time on routing
(`draft/routing.md`, "Rationale"). The cluster is far smaller; its routing should cost less.

## T5: a sleeping leaf

**Shows:** a leaf that sleeps is reached through its keeper, and finds another keeper when the
first goes.

1. The cluster, with S a leaf near C3 and also in reach of C2 (its second keeper). On S:
   `sleep on 300 2`. It prints `sleep: on, polling its keeper at least every 300 s and listening
   2 s after each poll`. 300 s is the shortest wake (`WAKE_MIN`).
2. Wait for S's `telling peer 1 that this board sleeps`, and on L4 `<S's address> sleeps: up to
   300 s between wakes. Messages to it wait for it.`
3. `## test t5-sleep`. Type `sleep` on S: `asleep, kept by <C3's id>: next poll in <n> s, ...`.
4. For an hour: L4 to S every 5 minutes, and S to L4 every 10.
5. Switch C3 off; mark `## off c3`. Another hour.
6. `sleep` on S, C2 and C3 (once it is back); `status` everywhere; `## end`.

| Board | Line | Means |
|---|---|---|
| S | `poll 4012: answered by 0000000c, 1 frame held` | a poll, its keeper, and how many frames it holds for S |
| S | `poll 4012: listened, 1 of 1 held frame came` | they came while it listened |
| S | `poll 4013: no answer from 0000000c (2 in a row)` | the keeper did not answer; six in a row (`POLL_TRIES`) and S takes another |
| S | `poll 4019: answered by 0000000b, 0 frames held` | another keeper: the handover is done |
| S | `radio: asleep now; on 1.6% of the 3600 s since it started, asleep 3542 s in 12 sleeps` | from `sleep`: the share of time the radio was on |
| C3 | `sleep: a relay, which never sleeps. It keeps the sleeping leaves that name it: it has room for their frames; held 12 frames for them, handed 12 over, dropped 0 for want of room and 0 held too long` | from `sleep` |
| L4 | `delivered #5: acknowledged 212340 ms after it was handed over, ...` | a message to S, held for up to a wake |

**Record:** for messages to S, delivered and latency; for S, polls answered and not, frames held
and come, the radio's share of time on; the time from `## off c3` to S's first poll answered by
another keeper, and the polls unanswered between; each keeper's held, handed and dropped.

**Pass:** every message to S delivered within a wake and a minute (360 s) while C3 is on; S kept
by C2 within 360 s plus six polls 10 s apart (`POLL_RETRY`) of C3 going; messages to S delivered
again after.

**What the simulator predicts.** The simulator's run of the firmware's core does not yet sleep
(`draft/sleeping.md`, "Not yet measured"). Its own model of the draft, on
`scenarios/sleep/region.tsim` with its recommended changes (`scenarios/sleep/region-tuned.tsim`,
ternmesh/sim `docs/sleeping.md`), at a wake of 300 s: 98.4% delivered to sleeping leaves, median
latency 197 s, a sleeping leaf's radio on 1.5% of the time. That is a thousand nodes; one leaf
and three relays should do no worse.

## T6: a node that sends too much

**Shows:** the budget at work. A node that sends far more than its share has its own frames held
to it, and the others' messages still go.

1. The cluster, as T4, L2 to L3 every 45 s as before.
2. `## test t6-load`. On L1, sending to L4, queue 32 messages of 128 bytes, the most a message
   holds (`TERN_COMPANION_TEXT_MAX`), at once, every minute, for twenty minutes: 32 lines into
   `l1.in`, each `send` with its own text. Each frame is 151 bytes, 254 ms on EU868, so this
   asks for 13% of L1's time. A board holds 32 messages waiting (`LINK_MESSAGES`); `not sent: no
   room for another message, or nothing to send` means the queue is full, and is expected.
3. `status` on L1 every two minutes: watch `airtime budget: <n> ms of 12000 ms left for its own
   frames` fall to near 0 and stay there.
4. `## end`; `status` everywhere.

**Record:** L1's airtime from its `sent` lines over the test, against 2% of it; L1's messages
delivered and their round trip; L2 to L3's delivery and latency, against T4's.

**Pass:** L1's own frames take no more than 2% of the test once its budget is spent, plus its
first 12 s; L2 to L3 delivered as well as in T4.

**What the simulator predicts.** `draft/airtime.md`, "What the budget did": with a twentieth of
the nodes writing a message every 10 seconds, the own part kept the others' messages arriving on
time 36 to 69% of the time on the fast preset against 14 to 19% with no budget, while the writers'
own messages went out behind a backlog minutes long. Expect L1's round trips to grow through the
test, and L2's not to.

## Firmware gaps

What the test needs and the console does not give today. None of it blocks the test; each makes a
number above weaker, as said.

1. **No total airtime on the console.** The node counts every frame it sends and receives
   (`air_total`, `frames_out`, `frames_in` in `node.c`) and shows them on the screen's bench page
   and to a companion client, but no console command prints them. Off EU868, and on a board
   with no screen such as the T1000-E, a node's airtime is known only from the frames its log
   names. Wanted: `status` to print `on the air: <ms> in <frames> frames sent, <ms> in <frames>
   received`.
2. **Forwarder counters not printed.** `struct tern_forward_counts` has `salvaged`, `unpaid`
   and `hop_limit`, which `status` leaves out. T3 needs `salvaged`; T6 needs `unpaid`, the frames
   a relay dropped because the budget could not pay for them.
3. **No line for a hop.** Nothing says when a hop succeeded (the frame heard passed on, an
   acknowledgement, a held notice) or was given up, or to which neighbour. The rate at which hops
   are acknowledged, link by link, is known only from `passed on` lines repeated and the totals
   in `status`. Wanted: `hop to <id> given up after <n> goes`, and on success a count per
   neighbour in `routes`.
4. **Frames sent without a line.** A board's own acknowledgements, announces, requests, polls,
   poll answers and held notices go with nothing printed, so airtime from logs is a lower bound.
5. **Lines that cannot be matched surely.** `sent #<n>` names the next hop but not the
   destination, and `passed on` names neither the message's counter nor its tag. The path of a
   message is found by its length and timing, which two messages of one length at once would
   confuse. Wanted: the tag on both.
6. **Routes change silently.** `routes` prints the metric, not the hops, and nothing is printed
   when a route's next hop changes, a neighbour's link goes up or down, or a neighbour is
   forgotten. T3's recovery is seen only in which neighbour the next frame goes `by`.
7. **Keeping is silent.** Nothing on a leaf says it changed keeper but the id in its next
   `poll ... answered by` line. Nothing on a relay says it held a frame, handed one over, or sent
   a held notice, beyond the totals in `sleep`; nothing on a source says it heard a held notice.
8. **The budget waits silently.** When a node's own frame waits for its budget, or a relay drops
   one for want of it, only a companion client is told (the `WAITING` reasons of
   `draft/airtime.md`, "What a client is told"). The console says nothing; T6 sees it only in the
   gaps between `sent` lines and in `airtime budget`.
9. **No start banner.** A board prints nothing on its console as it starts: no firmware version,
   role, routing id or region. A restart is seen only in the ESP32 ROM's lines or the logger
   losing the port, and an nRF52840 board says nothing at all. Wanted: one line with all of them.
10. **Role and power only over the companion link.** `set role` and `set power` are companion
    settings (`tools/companion.py`), which cannot share the port with the logger; `power` on the
    console is for the bench, where routing is off.
11. **No way to make a line on a table.** A board takes every neighbour it hears. A command to
    ignore a neighbour, for testing only, would let the whole test run in one room.
12. **No board time in log lines.** Latency is taken from when a line reaches the computer. USB
    and the console add milliseconds, and one-way latency needs the computers' clocks to agree.
    A board's uptime in milliseconds on each line, or on `sent`, `heard` and `delivered`, would
    give one-way latency from the board's own clock where both ends share a computer.
13. **`heard` does not say from whom.** It gives the counter and the text, not the peer. With
    several peers sending to one board, messages are told apart only by their text.

## The tools

* [`tools/fieldlog.py`](../tools/fieldlog.py): the logger. Needs pyserial. It opens an ESP32's
  UART bridge with DTR and RTS released, so as not to reset it, and an nRF52840's USB console with
  DTR raised, which that board needs before it writes anything (`console_heard()` in
  `ports/nrf52/src/board.c`). `--self-test` checks it on a pseudo-terminal.
* [`tools/fieldsummary.py`](../tools/fieldsummary.py): the numbers. Needs nothing but Python.
  `--self-test`, which CTest runs, checks it against logs of a line it writes itself.

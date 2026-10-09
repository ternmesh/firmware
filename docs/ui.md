# The user interface

**Status:** a proposal, written to be argued with. Nothing here is decided, and nothing in it is
protocol: how a node looks to the person holding it is this repository's business, not the
specification's. But most of what a user needs to see is something the specification has not
drafted yet, so each part below says which draft it waits on.

This document is about the interface someone uses when they carry a Tern node about, as they
would a Meshtastic or MeshCore one. The ESP32 port draws [a first version](../ports/esp32/README.md#the-screen)
of [the node's own screen](#the-nodes-own-screen): Home, Messages, Air, Share and This node. The bench
screen it had before, the serial console drawn on the display for developers, is now a developer
setting.

## What a user does

Before any screens, the things someone with a node needs to do, in roughly the order they need to
do them:

1. **Set it up.** Choose the region, whether the node relays for others or is a leaf, and the
   power its antenna allows. Once, and rarely again.
2. **Know it is working.** That it is on, on the air, and hears other nodes, without a laptop.
3. **Tell someone how to reach them,** and learn how to reach someone else.
4. **Send a message,** and know what became of it: waiting, on its way, arrived, or given up.
5. **Read what has come in,** and know who it is from.
6. **Know what the air allows.** When a message waits because the node has used its share of the
   channel, say so, and say for how long.
7. **Know how long the battery lasts.**

Debugging the mesh (who hears whom, how strongly, by which route) is not on this list. Developers
need it, and it stays available, but it is not what the interface is built around.

## Principles

1. **The user's words, not the protocol's.** A user has contacts, messages, and a share of the
   air. They do not have routing ids, sessions, counters or handshakes. First contact happens
   because someone sent a message to a new contact, not because they typed `contact`.
2. **Honest about the air.** Tern meters airtime, and that is the one thing about it no other mesh
   shows. When something waits, the interface says why: no route yet, the node's budget, or the
   region's limit. A message is never shown as sent when it is only queued.
3. **Nothing on the air that the protocol keeps off it.** Names and nicknames are kept on the
   node and the phone, never sent. The protocol keeps both ends of a message out of the clear; the
   interface must not undo that by, say, announcing a name.
4. **One model, many views.** The node keeps one account of its state. The screen, the serial
   console and a phone app are each a view of it, and none of them reaches into the protocol's
   structures for itself (see [The node model](#the-node-model)).
5. **Useful without a phone, best with one.** Typing on one button is miserable. The node's own
   screen covers knowing it works and reading what came in; writing and contacts belong on a phone.
6. **Off by default on a battery.** A screen that is lit all day is most of a small node's power.

## The node model

Today the Heltec port keeps its state in `static` variables in `main.c`, and the serial console
prints from them directly. A screen that did the same would have to be rewritten with every change
to the demo, and a phone app could not be written at all.

Instead, one structure describes the node as the user sees it, filled in by the code that runs the
protocol and read by every view:

```
 protocol (core + port)  ──fills──▶  node model  ──read by──▶  screen
                                                           ├──▶  serial console
                                                           └──▶  phone, over the companion link
```

What it holds, as far as the drafts allow today:

| Part | Holds | Waits on |
|---|---|---|
| Identity | the address, the region and its settings, relay or leaf | — |
| Neighbours | who is heard, how well, how recently | — |
| Contacts | an address, a local name, whether a session exists | a contact list; first contact settles the rest |
| Messages | text, the contact, the time, and a delivery state | forwarding under the secured frame |
| Airtime | what is used and what is left: the region's limit, and the node's budget | the budget is not drafted |
| Power | battery and whether it is charging | the board |

Delivery states follow the [forwarding draft](https://github.com/ternmesh/spec/blob/main/draft/forwarding.md):
**waiting** (for a route, the budget or the radio), **sent** (the first neighbour was heard
passing it on), **delivered** (the destination's acknowledgement came back), **not delivered**
(the source gave up). Nothing in between is shown as more certain than it is.

The model is a plain struct the caller owns, as the core's rules require of everything, so it can
be tested on a host and two can live side by side in the simulator. Where it should live is open:
in the port while there is one board, and probably in the core once a second board needs the same
thing.

The screen reads from a small first version of this (`ui_node` in `ports/esp32/main/ui.h`),
filled from the radio, the router and the companion link. The link keeps the contacts and the
messages with their delivery states (`ports/esp32/main/link.c`), and whether each received
message has been read, so a message read on the screen is read on the phone too, and the other
way round. The bench pages keep their own snapshot (`node_status` in `status.h`).

## The node's own screen

On the boards Tern starts with, a 128×64 monochrome display and one button. Its job is a glance:

| Page | Shows | On the Heltec V3 |
|---|---|---|
| Home | whether the node is on the air, how many nodes it hears, unread messages, battery, airtime left | all of it; the battery as a charge estimated from its voltage, said to be low at 10% or less, and charging as far as the voltage shows; a Bluetooth rune while a phone is connected |
| Messages | the latest few, newest first, with who they are from and their delivery state | one at a time; a long press shows the one before. A group's names the group, and who wrote it as far as the claimed routing id matches a contact |
| Contact card | this node's address as a QR code and a short code to read aloud, for someone adding it | **Share**, the QR code and the short code, and **This node**, the short code and the address in hex |
| Nearby | the nodes heard directly, with how well and how long ago | most recently heard first, seven to a screen; a long press shows the next. A node by a contact's name, else its routing id |
| Air | the airtime account: used, allowed, and when more is free | the region's limit; the budget is not drafted |
| Mesh | routes and frame counts, for whoever is curious; the bench screen's pages, more or less | the bench pages, behind `screen bench on` |
| Phones | the phones paired, and forgetting them | how many are bonded and whether one is connected; forgetting asks first |
| Reset | erasing the node for a new owner | the whole of its storage, and a new address; asks first, and keeps the time on the air |

Controls on one button: a short press moves to the next page, a long press acts on the page shown
(show the message before, the next nodes nearby, or on a bench page send a ping), and any press wakes the screen.
On Phones and Reset a long press only asks to be sure, for ten seconds, and a second long press in
that time forgets the phones or erases the node; a short press leaves the page and keeps it all. A
message shown is read once a press says someone saw it. The screen goes dark after a while on a
battery, and lights when a message arrives, showing it; while one is unread the LED blinks a
moment every few seconds, for a node in sight with its screen dark. Holding the button for five seconds turns
the node off, with a countdown from the second second so that letting go keeps it on, as Meshtastic
does; a press turns it on again. A node turns itself off when its battery is empty, and says so.
Boards with more buttons or a touch screen get more, but nothing should need them.

Two screens come before the pages. At start, the node's name and firmware version, then its
region and short code, for a few seconds or until a press: what Meshtastic and MeshCore show, and
the version someone asking for help is first asked for. And if it cannot start, why, and what to
do about it, on the screen: a board that fails only on its serial console looks dead to anyone
without a laptop.

A QR code fits: a 32-byte address is 64 characters of upper-case hex, which a version 3 code holds
in its alphanumeric mode at its lowest error correction (77 characters). That is 29 modules square,
so 58 pixels at two pixels a module. The Heltec V3 draws it so, dark on light. The code holds the
address's link, `HTTPS://TERNMESH.ORG/A/` and the address in base32, 75 characters, as
[draft/sharing.md](https://github.com/ternmesh/spec/blob/main/draft/sharing.md) defines it: a
web address, because a phone's camera does nothing with a scheme no app has claimed.

## Addresses and contacts

This is the hardest part of the interface, and the part most unlike other meshes. A Tern address
is a public key: sixty-four hex digits that nobody can read out, type or remember, and that the
protocol never sends in clear. There are no node names on the air to fall back on.

So:

* **Names are local.** A contact is an address with a name the user gave it, kept on the node or
  the phone. Two people may call the same node different things.
* **Addresses travel off the air,** or at least outside the mesh: a QR code shown on one screen
  and scanned by a phone, a link sent by other means, or the full hex pasted from a console.
* **A short code to compare,** so two people standing together can check they have the right
  node: twelve digits from a hash of the address, the same in every implementation
  ([draft/sharing.md](https://github.com/ternmesh/spec/blob/main/draft/sharing.md)).

Whether a node may learn a new contact over the air at all (someone it hears asking to make
contact) is a question for the first-contact draft, not the interface. The demo's `accept`
window is a stand-in, not a design.

## The companion link

On Meshtastic and MeshCore, most people use a phone app most of the time, over Bluetooth or USB,
and the node's screen is secondary. Tern will be the same, so the interface that matters most is
the link between the node and the phone: what the node offers, and in what form.

* **What it carries** is the node model: read it, be told when it changes, send a message, add a
  contact, change a setting.
* **Over what:** Bluetooth Low Energy for a phone, the USB serial port for a computer, the same
  framed protocol on both.
* **Who defines it.** If anyone is to write their own app, as people have for other meshes, this
  link has to be written down and kept stable. That makes it a specification section, in
  `ternmesh/spec`, even though it never goes over LoRa.
* **The ESP32's random numbers.** The Heltec port keeps the chip's noise source on, which it may
  not do while Bluetooth is running. A port with Bluetooth has to gather the randomness it needs
  for keys before the radio starts, or between uses of it. Solvable, but it shapes the port.

## Open questions

| Question | Depends on |
|---|---|
| How a node's airtime budget is shown, and what a user can do about it | the airtime budget, not drafted |
| Group or broadcast messages, and whether the home screen is a channel or a list of conversations | broadcast, not drafted (forwarding.md lists it as not yet) |
| The short code two people compare, the same in every implementation | settled: [draft/sharing.md](https://github.com/ternmesh/spec/blob/main/draft/sharing.md), shown by the Heltec V3 port |
| Whether a node accepts first contact from a node it has no contact for | first contact |
| The companion protocol, and where it is specified | settled: [draft/companion.md](https://github.com/ternmesh/spec/blob/main/draft/companion.md), spoken over USB by the Heltec V3 port |
| Where the node model lives once there are two boards | a second board |
| What a leaf that sleeps on a schedule shows, and when | sleeping leaves, not drafted |

## What happened to the bench screen

It stays while the demo stays, as a debug view: its pages are the **Mesh** page above, and come
after the others when `screen bench on` is typed on the console. The rest of it was replaced by
the pages above.

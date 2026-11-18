# Signed releases

Every file a release carries is signed with the project's **release key**, and a node updated
over the companion link takes only images signed by a key it is built with. This is the
specification's [Signed images](https://github.com/ternmesh/spec/blob/main/draft/companion.md#signed-images)
(the companion protocol's version 13), and the "now" items of the decision memo
[`decisions/signed-firmware-updates.md`](https://github.com/ternmesh/spec/blob/main/decisions/signed-firmware-updates.md).

What it does and does not stop:

* **ESP32 boards** refuse an image over the link, from any client, unless it is signed by a
  release key. Over USB they take anything, as before: esptool, the web flasher and a
  developer's own builds are unchanged.
* **nRF52840 boards** are updated by their bootloader, which checks no signature. While it waits
  for an image, anyone in Bluetooth range can install one, signed or not
  ([ports/nrf52](../ports/nrf52/README.md#updating-over-bluetooth)). The signature lets an honest
  client refuse a tampered package before it starts; it does not let the board refuse one.
* **Nothing protects what a node keeps from whoever holds it.** Its identity, sessions, groups'
  secrets and messages are in its flash in the clear, and a person with the board and a cable
  can read them, or install firmware that does. A node that is lost or taken should be treated
  as having given up its address and its groups. `WIPE` erases one that is still in hand.

## What is published

For each file `F` a release carries (each `.bin`, `.uf2` and `.zip`, and `SHA256SUMS`):

| File | Holds |
|---|---|
| `F.sig` | 64 bytes, nothing else: the Ed25519 signature (RFC 8032) by the release key over the 48 bytes `"TERN-FIRMWARE-V1"` (16 ASCII bytes) followed by `SHA-256(F)`, the file exactly as published |

`SHA256SUMS` lists the images only, not the signatures. To check a file by hand:

```sh
pip install "cryptography>=42"
python3 tools/release_key.py verify tern-heltec-v3-eu868-0.7.0-app.bin   # reads the .sig beside it
```

### In the site's `latest.json`

The site (ternmesh/site, `tools/firmware.ts`) fetches each image it offers with its `.sig`, and
refuses a release whose signature for any of them is missing or does not verify with a release
key. Each entry of `latest.json`'s `images` gains one field:

| Field | |
|---|---|
| `signature` | the 64 bytes of the file's `.sig`, as 128 lowercase hex digits |

So an entry reads:

```json
{
    "board": "heltec-v3",
    "region": "EU868",
    "file": "tern-heltec-v3-eu868-0.7.0-app.bin",
    "size": 712304,
    "sha256": "…64 hex digits…",
    "signature": "…128 hex digits…"
}
```

and for an nRF52840 board the same, with `"update": "bootloader"` and the `.zip`'s signature. The
signature is over the file `file` names, whose SHA-256 is `sha256`, so a client can check it
before downloading anything: the message is `"TERN-FIRMWARE-V1"` followed by the 32 bytes of
`sha256`.

## What each client must do

The companion protocol's version 13 ([ternmesh/spec#61](https://github.com/ternmesh/spec/pull/61)) adds two things a client sees:
`UPDATE_BEGIN`'s `signature`, and the request `WIPE` (`0x37`). And a node pairs with a passkey it
does not show only for three minutes after someone has been at it.

**Every client** (the Android and Apple apps, `/app` on the site, `tools/companion.py`):

1. Speak version 13 in `HELLO`.
2. Hold the release keys, the same list as [`ports/node/release_keys.h`](../ports/node/release_keys.h),
   and verify an image's signature before sending it either way: Ed25519 over
   `"TERN-FIRMWARE-V1" || SHA-256(image)`. Refuse an image with no signature, or one that does
   not verify, unless the user asks after being told it may not be the release's.
3. Send the signature in `UPDATE_BEGIN`: after `size` and `digest`, a byte of length (64, or 0
   for none) and the signature. Only to a node of version 13 or later; to an earlier node,
   `UPDATE_BEGIN` is as before. `UPDATE_BEGIN` is then 103 bytes.
4. Read `ERROR` 11 to `UPDATE_BEGIN` as "this node takes only signed images, and this one is not
   signed by a key it holds". It comes at once, before any image is sent. A client of version 12
   or earlier gets it for every image a node of version 13 is given, so an older app cannot
   update a newer ESP32 node: it must be updated.
5. Before `UPDATE_BOOTLOADER` (an nRF52840 node, `INFO`'s `update` 2), check the `.zip`'s
   signature, send the package as soon as the bootloader advertises, and tell the user that the
   node takes an image from anyone in range until the update finishes.
6. Offer **Wipe**: send `WIPE` only when the user asks, after saying that the node's address,
   contacts, groups and messages are gone for good and that it must be paired again. On `OK`,
   forget the operating system's Bluetooth bond with the node and everything the app holds for
   it: the node restarts with a new address. A node of version 12 or earlier does not know it.
7. When pairing fails, or before pairing a node that has no screen, tell the user to press the
   node's button, or turn it off and on (or press RESET on a board with no button), and pair
   within three minutes. A node that already has a bond refuses a new phone outside that window
   even with the right passkey, unless it shows a passkey on its screen.

**The site's web flasher** writes images over USB, which a node never checks. It should check
each image's `.sig` against the release keys before writing it, and say so if one does not
verify, since it is the one place a tampered image would be caught.

## What a node keeps

Whoever holds a node can read its keys: nothing a node keeps is encrypted, and over USB anyone
can read its flash or install firmware that does (the decision memo
[`decisions/keys-at-rest.md`](https://github.com/ternmesh/spec/blob/main/decisions/keys-at-rest.md)
says why the project does not turn on flash encryption or readout protection, which would take
the board away from its owner). What the firmware does to keep less:

* **Erasure in RAM** is as the specification requires: a session's secret `S` once its keys are
  made, each epoch key once its epoch is over, each message key after its one use, and every
  key of a handshake once it ends or is given up, through `tern_wipe()`, which the compiler
  cannot leave out. A received message's plaintext, or an invite's group secret, is wiped from
  the stack once handled.
* **In flash, not yet.** Both ports keep their records in a store that writes a new copy and
  marks the old one superseded (NVS on ESP32 and on Zephyr), so a session's earlier record, with
  an epoch key since erased in RAM, or a left group's secret, stays readable in flash until the
  store reuses that page. Closing that needs secrets kept apart, in flash the firmware erases
  itself, and is not done yet. `WIPE` and the Reset page erase the whole store, not record by
  record, so nothing survives them.
* **Messages** are capped: 32, the oldest finished one making room for the next.
* **Bonds, the identity and everything else** go with `WIPE` (the companion protocol's version
  13), from a client, or the Reset page on a board with a screen.

## The release key

The release workflow (`.github/workflows/release.yml`) signs with the private key in the GitHub
Actions secret `TERN_RELEASE_SIGNING_KEY`. It refuses to build or publish a release when the secret
is not set, when its key is not one of `ports/node/release_keys.h`, or when a release build would
take unsigned images; a release is never published unsigned. CI signs and verifies with the
specification's published **test key** (`tools/release_key.py --test`), whose private half is in
`tests/vectors/companion.json` for anyone to use. It is never a release key.

`release_keys.h` lists one release key, added on 2026-11-18:
`09800d79c2c3d0cab7f1710a18cd50d6f0e39c57d9cfe395b371195d4942f88d`. A node built with an empty
list takes no update over the link at all, and a tag with no key listed fails at once.

### Setting it up (the owner, once)

On a computer you trust, ideally one kept off the network for this:

1. Make the key, and keep its private half offline:

   ```sh
   pip install "cryptography>=42"
   python3 tools/release_key.py generate tern-release.key
   ```

   This writes the 64-hex-digit private key to `tern-release.key` (mode 0600) and prints the
   public key, and the lines for `release_keys.h`. Keep two copies of `tern-release.key` offline,
   apart. Losing it means every ESP32 node in the field must be updated over USB, to firmware
   with a new key, before it takes another update over the air.

2. Give the firmware the public key: add the printed lines to
   [`ports/node/release_keys.h`](../ports/node/release_keys.h), above `{0}`, with a comment saying
   when it was added, and open a pull request. `python3 tools/release_key.py public
   tern-release.key` prints them again.

3. Give the release workflow the private key, as a repository secret (or, better, an environment
   secret behind required reviewers, with `environment:` added to the workflow's jobs):

   ```sh
   gh secret set TERN_RELEASE_SIGNING_KEY --repo ternmesh/firmware < tern-release.key
   ```

4. Check that the two agree:

   ```sh
   TERN_RELEASE_SIGNING_KEY=$(cat tern-release.key) python3 tools/release_key.py listed
   ```

5. Publish the public key where users and the apps can find it: in the apps' own list, in the
   site's, and in the specification's notes for the release.

The first release after this is the first that ESP32 nodes take over the air. Nodes running a
release from before it take any image, so the first signed release is still installed the old
way; from then on only signed images are.

### Changing the key

1. Make the new key as above. Add its lines to `release_keys.h` beside the old one's, and release
   that, signed with the **old** key, so that nodes running the old firmware take it.
2. Once that release has had time to reach the nodes, set the secret to the new key, and sign
   releases with it from then on.
3. A later release drops the old key from the list.

A key that is exposed is changed the same way, at once; until a release without it reaches every
node, whoever holds it can sign images those nodes take. Withdrawing a key sooner is not
specified yet.

## For developers

A build of your own runs anywhere over USB. To update one over Bluetooth, build it with
`CONFIG_TERN_UPDATE_UNSIGNED=y` (in `menuconfig`, or a `.defaults` file of your own), flash that
over USB once, and send your images with `tools/companion.py update --unsigned`. A node built so
takes an image from anyone who can pair with it: keep it on the bench. A release never sets it.

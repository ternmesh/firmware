# Test vectors from the specification

`unicast-security.json` is a byte-for-byte copy of
[`vectors/unicast-security.json`](https://github.com/ternmesh/spec/blob/main/vectors/unicast-security.json)
in ternmesh/spec, as of [ternmesh/spec#12](https://github.com/ternmesh/spec/pull/12) (secured
unicast with the routed head, and acknowledgements). Like everything under the
specification's `vectors/`, it is dedicated to the public domain (CC0-1.0).

The build turns it into a C header with `unicast_to_c.py`, which only reformats: no value is
computed there. `tests/unicast.c` checks the core against it.

`first-contact.json` is likewise a copy of
[`vectors/first-contact.json`](https://github.com/ternmesh/spec/blob/main/vectors/first-contact.json),
as of commit `522065e` (first contact, draft 0, with the impersonation case added in
[ternmesh/spec#4](https://github.com/ternmesh/spec/pull/4)). `contact_to_c.py` turns it into a
header for `tests/contact.c` the same way.

`phy.json` is a copy of
[`vectors/phy.json`](https://github.com/ternmesh/spec/blob/main/vectors/phy.json), as of commit
its latest in [ternmesh/spec#5](https://github.com/ternmesh/spec/pull/5) (radio settings, draft 0, with
every receiver setting and cases for the limit on transmitting), with the AU915 and NZ915
profiles of [ternmesh/spec#36](https://github.com/ternmesh/spec/pull/36), and the CL915, MY919,
TH920, RU868 and KZ863 profiles and each profile's `min_eirp_dbm` of
[ternmesh/spec#41](https://github.com/ternmesh/spec/pull/41).
`phy_to_c.py` turns it into a header for `tests/region.c`.

`routing.json` is a copy of
[`vectors/routing.json`](https://github.com/ternmesh/spec/blob/main/vectors/routing.json), as of
the routing draft in [ternmesh/spec#8](https://github.com/ternmesh/spec/pull/8), which adds a full
table's cases. Its default-route cases are as of
[ternmesh/spec#40](https://github.com/ternmesh/spec/pull/40): a leaf with no link to a relay up
takes the nearest relay it hears. Its `quiet` cases are as of
[ternmesh/spec#81](https://github.com/ternmesh/spec/pull/81): a leaf gone quiet goes out of use.
`routing_to_c.py`
turns it into a header for `tests/route.c`.

`forwarding.json` is a copy of
[`vectors/forwarding.json`](https://github.com/ternmesh/spec/blob/main/vectors/forwarding.json), as of
[ternmesh/spec#15](https://github.com/ternmesh/spec/pull/15), which adds when a radio is
receiving, with its `acknowledged` cases as of
[ternmesh/spec#81](https://github.com/ternmesh/spec/pull/81): which way an acknowledgement goes.
`forwarding_to_c.py` turns it
into a header for `tests/forward.c`, and its listens alone into one for `tests/listen.c`.

`companion.json` is a copy of
[`vectors/companion.json`](https://github.com/ternmesh/spec/blob/main/vectors/companion.json), as
of the companion protocol's version 13, which adds UPDATE_BEGIN's signature, signed images'
signatures and WIPE ([ternmesh/spec#61](https://github.com/ternmesh/spec/pull/61)), after version 12's UPDATE_BOOTLOADER and
INFO's update, for a node whose bootloader takes its updates
([ternmesh/spec#55](https://github.com/ternmesh/spec/pull/55)), and version 11's SELF's pairing, for a node without a screen
that pairs with a default passkey ([ternmesh/spec#54](https://github.com/ternmesh/spec/pull/54)),
and version 10's the most a node may send at
([ternmesh/spec#51](https://github.com/ternmesh/spec/pull/51), with its SELF corrected in
[ternmesh/spec#52](https://github.com/ternmesh/spec/pull/52)), and version 9's turning the node off
([ternmesh/spec#50](https://github.com/ternmesh/spec/pull/50)), after version 8's fix and
satellite receiver ([ternmesh/spec#46](https://github.com/ternmesh/spec/pull/46)) and version 7's
join codes
([ternmesh/spec#32](https://github.com/ternmesh/spec/pull/32)). `companion_to_c.py` turns it into a
header for `tests/companion.c`, and its connections, `bootloader`'s among them, and its
`unknown_to_older` alone into one for `tests/link.c`.
`tools/companion.py selftest` reads it directly.

`flooding.json` and `groups.json` are copies of
[`vectors/flooding.json`](https://github.com/ternmesh/spec/blob/main/vectors/flooding.json) and
[`vectors/groups.json`](https://github.com/ternmesh/spec/blob/main/vectors/groups.json), as of
[ternmesh/spec#18](https://github.com/ternmesh/spec/pull/18), which adds both sections.
`flooding_to_c.py` and `groups_to_c.py` turn them into headers for `tests/flood.c` and
`tests/group.c`. The same pull request gives a unicast message the node flag, so
`unicast-security.json` and `forwarding.json` are as of it too.
`flooding.json` is since as of
[ternmesh/spec#25](https://github.com/ternmesh/spec/pull/25), which adds a busy relay. Both are
since as of [ternmesh/spec#28](https://github.com/ternmesh/spec/pull/28), which gives a group frame
the node flag, `0x61`, flooded as `0x60` is, and puts `hdr` in a flooded frame's id.

`sharing.json` is a copy of
[`vectors/sharing.json`](https://github.com/ternmesh/spec/blob/main/vectors/sharing.json), as of
[ternmesh/spec#21](https://github.com/ternmesh/spec/pull/21) (sharing an address: the text form,
the `ternmesh.org` link with the address in base32, and the short code). `sharing_to_c.py` turns it into a header for
`tests/share.c`.

`groups.json` is since as of [ternmesh/spec#32](https://github.com/ternmesh/spec/pull/32) too,
which adds join codes; `groups_to_c.py` turns its `join_codes` and `bad_join_codes` into
`tests/group.c`'s.

`positions.json` is a copy of
[`vectors/positions.json`](https://github.com/ternmesh/spec/blob/main/vectors/positions.json), as
of [ternmesh/spec#27](https://github.com/ternmesh/spec/pull/27) (positions, draft 0).
`positions_to_c.py` turns it into a header for `tests/position.c`, without its frames, which are
the unicast and group frames' own, and its sizes.

`cards.json` is a copy of
[`vectors/cards.json`](https://github.com/ternmesh/spec/blob/main/vectors/cards.json), as of
[ternmesh/spec#30](https://github.com/ternmesh/spec/pull/30) (presence cards, draft 0), which also
adds cards to `flooding.json`. `cards_to_c.py` turns it into a header for `tests/card.c`.
`positions.json` is since as of [ternmesh/spec#29](https://github.com/ternmesh/spec/pull/29), whose
group frames carry a count.

`routing.json`, `flooding.json` and `positions.json` are since as of
[ternmesh/spec#36](https://github.com/ternmesh/spec/pull/36) too, which adds the AU915 and NZ915
profiles to each. `routing.json` and `positions.json` are since as of
[ternmesh/spec#41](https://github.com/ternmesh/spec/pull/41), which adds the five profiles after
them.

`routing.json` is since as of [ternmesh/spec#37](https://github.com/ternmesh/spec/pull/37), which
signs every announce: `routing_to_c.py` carries each announce's seed and the `verified` cases,
and `tests/route.c` signs and checks them with the core's own Ed25519.

`positions.json` is as of [ternmesh/spec#48](https://github.com/ternmesh/spec/pull/48), whose
group frame starts with `POSITION_HOPS`.

`routing.json` is as of [ternmesh/spec#44](https://github.com/ternmesh/spec/pull/44), leaves
that move: solicits and answers, a listed route's leaf bit, a metric of at most `0x7FFE`, the
newer of two routes as good, and the `anchors`, `answering` and `answered` cases, which
`routing_to_c.py` carries to `tests/route.c`.

`airtime.json` is a copy of
[`vectors/airtime.json`](https://github.com/ternmesh/spec/blob/main/vectors/airtime.json), as of
commit `792b718` (the airtime budget, draft 0, with the wait estimate that keeps the queue's
order, solicits and answers in the routing part, and the own part at 2% and the relayed part at
7%).
`airtime_to_c.py` turns it into a header for `tests/airtime.c`.

`unicast-security.json`, `first-contact.json`, `forwarding.json`, `flooding.json` and
`positions.json` are as of [ternmesh/spec#59](https://github.com/ternmesh/spec/pull/59), the
unicast hardening: each epoch's own tag key, which changes every destination tag and proof
(and so positions' frames); copies answered only when they are copies, four times and for ten
minutes; a receiver's window and a sender's counter across restarts; starting again; a session's
life; how often a responder answers `message_1`; whether hearing a frame teaches anything; and
where a writer's flood starts. `unicast_to_c.py` carries `restart`, `at_s`, `then` and
`lifetimes` to `tests/unicast.c`, and `flooding_to_c.py` the `starts` to `tests/flood.c`.
first-contact.json's `answers` are checked by `tests/demo.c`'s
a_board_answers_so_many_handshakes, which plays them between boards, not from the file.
`cards.json` is not yet: its `verifications` need `tern_address_verify()` to refuse an address
outside the prime-order subgroup, which comes with the firmware's hardening of it.

When the specification's vectors change, copy the new file here in the same pull request that
changes the code to match, and update the commit above.

`sleeping.json` is a copy of
[`vectors/sleeping.json`](https://github.com/ternmesh/spec/blob/main/vectors/sleeping.json), as of
[ternmesh/spec#62](https://github.com/ternmesh/spec/pull/62) (leaves that sleep, with the changes the
simulator asked for). `sleeping_to_c.py` turns it into a header for `tests/sleep.c`.

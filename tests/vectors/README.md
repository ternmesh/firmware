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
every receiver setting and cases for the limit on transmitting).
`phy_to_c.py` turns it into a header for `tests/region.c`.

`routing.json` is a copy of
[`vectors/routing.json`](https://github.com/ternmesh/spec/blob/main/vectors/routing.json), as of
the routing draft in [ternmesh/spec#8](https://github.com/ternmesh/spec/pull/8), which adds a full
table's cases. `routing_to_c.py`
turns it into a header for `tests/route.c`.

`forwarding.json` is a copy of
[`vectors/forwarding.json`](https://github.com/ternmesh/spec/blob/main/vectors/forwarding.json), as of
[ternmesh/spec#15](https://github.com/ternmesh/spec/pull/15), which adds when a radio is
receiving. `forwarding_to_c.py` turns it
into a header for `tests/forward.c`, and its listens alone into one for `tests/listen.c`.

`companion.json` is a copy of
[`vectors/companion.json`](https://github.com/ternmesh/spec/blob/main/vectors/companion.json), as
of the companion protocol's version 1, in
[ternmesh/spec#16](https://github.com/ternmesh/spec/pull/16). `companion_to_c.py` turns it into a
header for `tests/companion.c`, and its two exchanges alone into one for `tests/link.c`.
`tools/companion.py selftest` reads it directly.

`flooding.json` and `groups.json` are copies of
[`vectors/flooding.json`](https://github.com/ternmesh/spec/blob/main/vectors/flooding.json) and
[`vectors/groups.json`](https://github.com/ternmesh/spec/blob/main/vectors/groups.json), as of
[ternmesh/spec#18](https://github.com/ternmesh/spec/pull/18), which adds both sections.
`flooding_to_c.py` and `groups_to_c.py` turn them into headers for `tests/flood.c` and
`tests/group.c`. The same pull request gives a unicast message the node flag, so
`unicast-security.json` and `forwarding.json` are as of it too.

`sharing.json` is a copy of
[`vectors/sharing.json`](https://github.com/ternmesh/spec/blob/main/vectors/sharing.json), as of
[ternmesh/spec#19](https://github.com/ternmesh/spec/pull/19) (sharing an address: the text form,
the `TERN:` link and the short code). `sharing_to_c.py` turns it into a header for
`tests/share.c`.

When the specification's vectors change, copy the new file here in the same pull request that
changes the code to match, and update the commit above.

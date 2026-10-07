# Test vectors from the specification

`unicast-security.json` is a byte-for-byte copy of
[`vectors/unicast-security.json`](https://github.com/ternmesh/spec/blob/main/vectors/unicast-security.json)
in ternmesh/spec, as of commit `4668b27` (secured unicast, draft 0). Like everything under the
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
the routing draft in [ternmesh/spec#6](https://github.com/ternmesh/spec/pull/6). `routing_to_c.py`
turns it into a header for `tests/route.c`.

When the specification's vectors change, copy the new file here in the same pull request that
changes the code to match, and update the commit above.

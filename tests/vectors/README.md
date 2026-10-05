# Test vectors from the specification

`unicast-security.json` is a byte-for-byte copy of
[`vectors/unicast-security.json`](https://github.com/ternmesh/spec/blob/main/vectors/unicast-security.json)
in ternmesh/spec, as of commit `4668b27` (secured unicast, draft 0). Like everything under the
specification's `vectors/`, it is dedicated to the public domain (CC0-1.0).

The build turns it into a C header with `unicast_to_c.py`, which only reformats: no value is
computed there. `tests/unicast.c` checks the core against it.

When the specification's vectors change, copy the new file here in the same pull request that
changes the code to match, and update the commit above.
